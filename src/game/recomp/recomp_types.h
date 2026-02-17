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
 *   Each function uses local uint32_t variables for registers.
 *   Callee-saved registers (ebx, esi, edi, ebp) are preserved
 *   by the C compiler's register allocator.
 *
 * Calling convention:
 *   Translated functions use cdecl (stack-based arguments).
 *   Arguments are accessed as function parameters, not via
 *   stack pointer offsets.
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <string.h>

/* ── Memory access helpers ──────────────────────────────── */

/** Read N bytes from a flat memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)(uintptr_t)(addr))
#define MEM16(addr)  (*(volatile uint16_t *)(uintptr_t)(addr))
#define MEM32(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)(uintptr_t)(addr))
#define SMEM16(addr) (*(volatile int16_t  *)(uintptr_t)(addr))
#define SMEM32(addr) (*(volatile int32_t  *)(uintptr_t)(addr))

/** Float memory access. */
#define MEMF(addr)   (*(volatile float    *)(uintptr_t)(addr))
#define MEMD(addr)   (*(volatile double   *)(uintptr_t)(addr))

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

/** Push a 32-bit value onto a simulated stack. */
#define PUSH32(sp, val) do { (sp) -= 4; MEM32(sp) = (uint32_t)(val); } while(0)

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

/* ── Forward declarations for translated functions ──────── */
/* These are generated by the recompiler and included per-file. */

#endif /* RECOMP_TYPES_H */
