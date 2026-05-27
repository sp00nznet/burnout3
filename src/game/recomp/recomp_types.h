/**
 * Burnout 3: Takedown - Recompiled Code Runtime Support
 *
 * Type definitions and helper macros used by mechanically
 * translated x86→C code. Each original x86 function is
 * translated to a C function that uses these types.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   VirtualAlloc (see xbox_memory_layout.c). Recompiled code
 *   accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi, ebp) are local variables
 *   in each translated function, automatically preserving the caller's
 *   values through C's stack frame mechanism.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes a dummy return address; ret pops it.
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions. */
#if !defined(_MSC_VER) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ── Memory offset ──────────────────────────────────────── */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during xbox_MemoryLayoutInit, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* ── Global registers ──────────────────────────────────── */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to XBOX_STACK_TOP)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - these are global because callers pass implicit
 *   parameters through them (e.g. 'this' via esi in thiscall).
 *   The callee-save contract is enforced by PUSH32/POP32 instructions
 *   in the generated code, not by C local variable scoping.
 *
 * NOT global: ebp - stays local in each function because 20K+ FPO
 * (Frame Pointer Omission) functions use it as scratch without
 * save/restore. For SEH functions, g_seh_ebp bridges the gap.
 */
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog (sub_00244784) sets up ebp for the caller, but since ebp is
 * a local variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
extern uint32_t g_seh_ebp;

/* ── ICALL trace ring buffer (for debugging) ────────── */
#define ICALL_TRACE_SIZE 16
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

/* ── ICALL failure diagnostic ────────────────────────────── */
void recomp_icall_fail_log(uint32_t va);

/* ── Memory access helpers ──────────────────────────────── */

/** Translate an Xbox VA to an actual pointer.
 *  Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 *  in the recompiled code can overflow. Without the mask, a 64-bit
 *  uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 *  our mapping and causing access violations.
 *
 *  Native pointer detection: RenderWare's internal allocator (and some
 *  kernel functions) may store native heap pointers in Xbox memory.
 *  When recompiled code reads these back and uses them as addresses,
 *  adding g_xbox_mem_offset would cause a double-offset fault. We detect
 *  this by checking if the address already falls within our mapped
 *  native region and skip the offset addition in that case.
 *
 *  This is safe because Xbox mirror views all alias the same physical
 *  memory, so a native pointer to any mirror correctly accesses the
 *  right underlying data. */
static __forceinline uintptr_t xbox_ptr_resolve(uint32_t addr)
{
    /* Fast path: most addresses are Xbox VAs below the mapping base.
     * The mapped native region spans [offset, offset + 29*64MB).
     * Any address in this range is already a native pointer and
     * should NOT have the offset added again. */
    uint32_t offset32 = (uint32_t)g_xbox_mem_offset;
    if (offset32 != 0) {
        uint32_t rel = addr - offset32;  /* wraps if addr < offset32 */
        if (rel < 0x74000000u) {  /* 29 * 64MB = total mapped region */
            return (uintptr_t)addr;
        }
    }
    return (uintptr_t)addr + g_xbox_mem_offset;
}
#define XBOX_PTR(addr) xbox_ptr_resolve((uint32_t)(addr))

/**
 * Check if a uint32_t value is a valid game pointer.
 * Accepts both Xbox VAs (0x10000..0x4000000) and native pointers
 * that fall within the mapped Xbox memory region.
 * Use this for vtable validation instead of rigid Xbox VA range checks.
 */
static __forceinline int is_valid_game_ptr(uint32_t val)
{
    /* Xbox VA range (covers .text, .data, .rdata, heap, stack) */
    if (val >= 0x10000 && val < 0x4000000)
        return 1;
    /* Native pointer in our mapped region */
    uint32_t offset32 = (uint32_t)g_xbox_mem_offset;
    if (offset32 != 0) {
        uint32_t rel = val - offset32;
        if (rel < 0x74000000u)
            return 1;
    }
    return 0;
}

/**
 * Convert a native pointer value to its Xbox VA equivalent.
 * Returns the Xbox VA (modulo 64MB mirror) if the value is a native
 * pointer in the mapped region. Returns the value unchanged if it's
 * already an Xbox VA.
 */
static __forceinline uint32_t native_to_xbox_va(uint32_t val)
{
    uint32_t offset32 = (uint32_t)g_xbox_mem_offset;
    if (offset32 != 0 && val >= offset32) {
        uint32_t rel = val - offset32;
        if (rel < 0x74000000u)
            return rel % 0x04000000u;
    }
    return val;
}

/** Read N bytes from a flat memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))

/** Float memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* ── Flag computation helpers ───────────────────────────── */

/**
 * These macros compute x86 flags for conditional branches.
 * They are used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target → if (COND(a, b)) goto target;
 */

/* Unsigned comparison conditions (from CMP a, b → a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
#define CMP_L(a, b)   ((int32_t)(a) <  (int32_t)(b))     /* less (SF!=OF) */
#define CMP_GE(a, b)  ((int32_t)(a) >= (int32_t)(b))     /* greater or equal */
#define CMP_LE(a, b)  ((int32_t)(a) <= (int32_t)(b))     /* less or equal */
#define CMP_G(a, b)   ((int32_t)(a) >  (int32_t)(b))     /* greater */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  ((int32_t)((uint32_t)(a) & (uint32_t)(b)) < 0) /* SF=1 */

/* ── Arithmetic with carry/overflow detection ───────────── */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ── Rotation / shift helpers ───────────────────────────── */

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return (val >> n) | (val << (32 - n));
}

/* ── Sign/zero extension ───────────────────────────────── */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ── Byte/word register access ──────────────────────────── */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ── Stack simulation (for push/pop heavy prologues) ────── */

/** Push a 32-bit value onto a simulated stack.
 *  Evaluates val BEFORE decrementing sp, matching x86 semantics
 *  where push [esp+N] reads the operand before adjusting ESP. */
#define PUSH32(sp, val) do { uint32_t _pv = (uint32_t)(val); (sp) -= 4; MEM32(sp) = _pv; } while(0)

/** Pop a 32-bit value from a simulated stack. */
#define POP32(sp, dst)  do { (dst) = MEM32(sp); (sp) += 4; } while(0)

/* ── Byte swap (for endian conversion if needed) ────────── */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

/* ── Indirect call dispatch ──────────────────────────────── */

/**
 * Generic function pointer type for dispatch table lookups.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * Indirect call through the dispatch table.
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 a dummy return address before this macro.
 * If not found, pop the dummy return address to keep the stack balanced.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Try native pointer → Xbox VA conversion for addresses in mapped region. \
     * RW engine stores native heap pointers in Xbox memory; when code reads \
     * them back and uses as function addresses, they're native addresses \
     * pointing into our memory mirrors. Convert back to Xbox VA. */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        uint32_t _off32 = (uint32_t)g_xbox_mem_offset; \
        int _resolved = 0; \
        if (_off32 != 0 && _va >= _off32) { \
            uint32_t _xva = (_va - _off32) % 0x04000000u; \
            if (_xva >= 0x00011000 && _xva < 0x002CC800) { \
                _va = _xva; _resolved = 1; \
            } \
        } \
        if (!_resolved) { g_esp += 4; eax = 0; break; } \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) _fn(); \
    else { g_esp += 4; eax = 0; } \
} while(0)

/**
 * Stack-safe indirect call.
 * Restores g_esp to saved_esp (pre-arg value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Reject IEEE 754 floats misread as function pointers. \
     * Common garbage: 0x3F800000 (1.0), 0x3C23D70A (0.01), 0x40000000 (2.0). \
     * Native Xbox VAs are 0x10011000-0x14000000 (exp ~32-40 after offset). \
     * Use exponent >= 50 to avoid false positives on native mirror addresses. \
     * Covers float range ~1e-22 to ~1e38 = all typical game data floats. */ \
    { uint32_t _exp = (_va >> 23) & 0xFF; \
      if (_exp >= 50 && _exp <= 200) { \
        g_esp = (saved_esp); eax = 0; break; \
      } \
    } \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        uint32_t _off32 = (uint32_t)g_xbox_mem_offset; \
        int _resolved = 0; \
        if (_off32 != 0 && _va >= _off32) { \
            uint32_t _xva = (_va - _off32) % 0x04000000u; \
            if (_xva >= 0x00011000 && _xva < 0x002CC800) { \
                _va = _xva; _resolved = 1; \
            } \
        } \
        if (!_resolved) { g_esp = (saved_esp); eax = 0; break; } \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) _fn(); \
    else { g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * Indirect tail call (jmp through function pointer).
 * No return address is pushed - reuses the current frame's return addr.
 */
#define RECOMP_ITAIL(xbox_va) do { \
    recomp_func_t _fn = recomp_lookup_manual((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup_kernel((uint32_t)(xbox_va)); \
    if (_fn) _fn(); \
} while(0)

/* ── Register name aliases for generated code ──────────── */

/**
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 */
#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ── Forward declarations for translated functions ──────── */
/* These are generated by the recompiler and included per-file. */

#endif /* RECOMP_TYPES_H */
