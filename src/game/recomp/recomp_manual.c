/**
 * Burnout 3 - Manually implemented recompiled functions
 *
 * These are functions that the automatic recompiler couldn't handle
 * correctly (mid-function entry points, SEH continuations, etc.).
 * They use the same register model and calling conventions as the
 * generated code.
 */

#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"
#include <math.h>
#include <stdio.h>

/* Forward declarations for manually implemented functions */
void sub_001D1818(void);

/* ── Manual dispatch table ────────────────────────────────────────────
 *
 * Functions defined in this file that aren't in the auto-generated
 * dispatch table (because gen/ is gitignored and regenerated).
 * recomp_lookup_manual() is called by RECOMP_ICALL as a fallback.
 */
static const struct {
    uint32_t xbox_va;
    recomp_func_t func;
} g_manual_funcs[] = {
    { 0x001D1818u, (recomp_func_t)sub_001D1818 },
};
#define NUM_MANUAL_FUNCS (sizeof(g_manual_funcs) / sizeof(g_manual_funcs[0]))

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    for (size_t i = 0; i < NUM_MANUAL_FUNCS; i++) {
        if (g_manual_funcs[i].xbox_va == xbox_va)
            return g_manual_funcs[i].func;
    }
    return NULL;
}

/**
 * sub_001D1818 - Thread start routine (RenderWare initialization)
 *
 * This is a mid-function entry point inside sub_001D17DC (0x001D17DC-0x001D18B0).
 * The automatic recompiler treated it as dead code after an early return at
 * 0x001D1815. In reality, this address is passed as the StartRoutine parameter
 * to PsCreateSystemThreadEx, making it the actual game initialization thread.
 *
 * The original x86 code starts with an SEH prologue:
 *   push 0x18; push 0x36BD40; call __SEH_prolog
 *   and [ebp-4], 0
 *   mov eax, fs:[0x28]    ← TLS pointer (translator drops fs: prefix)
 *
 * What it does:
 *   1. Reads the RW engine context from TLS (fs:[0x28])
 *   2. Copies .data sections into RW engine memory
 *   3. Zeroes BSS
 *   4. Calls the real game init callback (via StartContext1 function pointer)
 *   5. Terminates the thread via PsTerminateSystemThread
 *
 * Stack layout at entry (set up by bridge_PsCreateSystemThreadEx):
 *   [esp+0] = dummy return address (0)
 *   [esp+4] = StartContext1 (function pointer to game init callback)
 *   [esp+8] = StartContext2 (parameter to pass to callback)
 *
 * After our synthetic prologue:
 *   [ebp+8]  = StartContext1 (callback function pointer)
 *   [ebp+12] = StartContext2 (callback parameter)
 *   [ebp-4]  = SEH state (0 = in __try, -1 = outside)
 *   [ebp-28] = saved return value from callback
 *   [ebp-32] = RW engine context pointer
 *   [ebp-36] = destination base pointer
 *   [ebp-40] = data section size
 */
void sub_001D1818(void)
{
    uint32_t ebx, esi, edi, ebp;

    /* Synthetic prologue - simulate push ebp; mov ebp, esp; sub esp, 48
     * The original x86 has an SEH prologue (push 0x18; push handler;
     * call __SEH_prolog) that sets up the frame. We replicate the
     * resulting layout so ebp-relative accesses work correctly. */
    PUSH32(esp, 0);     /* push ebp (placeholder for saved ebp) */
    ebp = esp;          /* mov ebp, esp */
    esp -= 48;          /* reserve frame space for locals */

    /* --- Original code from address 0x001D1824 (after SEH prolog) --- */

    /* and [ebp-4], 0  →  SEH state = 0 (entering __try block) */
    MEM32(ebp - 4) = 0;

    /* mov eax, fs:[0x28] - Read TLS / RW engine context.
     * The translator drops the fs: prefix, so MEM32(0x28) reads from
     * our fake TIB at Xbox VA 0x28 (populated in xbox_MemoryLayoutInit). */
    eax = MEM32(0x28);
    MEM32(ebp - 32) = eax;

    /* Get destination pointer from RW context */
    edx = MEM32(eax + 0x28);
    edx = edx + 4;
    MEM32(ebp - 36) = edx;
    MEM32(edx - 4) = edx;      /* self-pointer at block start (RW pattern) */

    /* Calculate data section size */
    ebx = MEM32(0x36BF80);     /* end of .data in XBE */
    esi = MEM32(0x36BF7C);     /* start of .data in XBE */
    ebx = ebx - esi;           /* size = end - start */
    MEM32(ebp - 40) = ebx;

    /* Copy data sections: rep movsd + rep movsb
     * Note: XBOX_PTR() translates Xbox VAs to native pointers since
     * the generated code originally used (void*)(uintptr_t)edi which
     * would be wrong on 64-bit (edi holds Xbox VAs, not native addrs). */
    ecx = ebx;
    edi = edx;
    eax = ecx;
    ecx = ecx >> 2;            /* dword count */
    memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4);
    esi += ecx * 4; edi += ecx * 4; ecx = 0;

    ecx = eax;
    ecx = ecx & 3;             /* remaining bytes */
    memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx);
    esi += ecx; edi += ecx; ecx = 0;

    /* Zero BSS section */
    ecx = MEM32(0x36BF8C);     /* BSS size */
    if (TEST_Z(ecx, ecx)) goto loc_001D187D;   /* skip if no BSS */

    /* rep stosd + rep stosb to zero BSS */
    eax = 0;
    edi = ebx + edx;           /* BSS start = data_size + dest_base */
    edx = ecx;
    ecx = ecx >> 2;            /* dword count */
    { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }
    edi += ecx * 4; ecx = 0;

    ecx = edx;
    ecx = ecx & 3;             /* remaining bytes */
    memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx);
    edi += ecx; ecx = 0;

loc_001D187D:
    /* Call sub_001D1628(1) - enable debug output */
    PUSH32(esp, 1);
    PUSH32(esp, 0); sub_001D1628();

    /* Call the REAL game init callback: StartContext1(StartContext2)
     * ebp+8 = function pointer, ebp+0xC = parameter */
    PUSH32(esp, MEM32(ebp + 0xC));
    PUSH32(esp, 0); RECOMP_ICALL(MEM32(ebp + 8));

    /* Save callback return value */
    MEM32(ebp - 28) = eax;

    /* Call sub_001D1628(0) - disable debug output */
    PUSH32(esp, 0);
    PUSH32(esp, 0); sub_001D1628();

    goto loc_001D18A2;

    /* SEH exception handler (unreachable in normal flow) */
    PUSH32(esp, MEM32(ebp - 20));
    PUSH32(esp, 0); sub_001D17DC();
    esp += 4; return;

loc_001D18A2:
    /* SEH state = -1 (leaving __try block) */
    MEM32(ebp - 4) = MEM32(ebp - 4) | 0xFFFFFFFFu;

    /* PsTerminateSystemThread(return_value)
     * On real Xbox this doesn't return. In our recompiled version,
     * the bridge stub returns and we clean up the frame. */
    PUSH32(esp, MEM32(ebp - 28));
    PUSH32(esp, 0); RECOMP_ICALL(MEM32(0x36B898));

    /* Clean up frame and return (replaces __debugbreak in generated code) */
    esp = ebp + 4;  /* pop ebp + skip saved ebp */
    return;
}
