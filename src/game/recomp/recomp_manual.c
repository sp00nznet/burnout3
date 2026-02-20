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
void sub_001D2793(void);
void sub_00249B7C(void);
void sub_00249B9C(void);

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
    { 0x001D2793u, (recomp_func_t)sub_001D2793 },
    { 0x00249B7Cu, (recomp_func_t)sub_00249B7C },
    { 0x00249B9Cu, (recomp_func_t)sub_00249B9C },
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
    uint32_t ebp;

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

/**
 * sub_001D2793 - Game initialization callback
 *
 * This is an undetected function in the gap between sub_001D276B (ends ~0x001D278E)
 * and xbe_entry_point (starts 0x001D2807). The recompiler didn't detect it because
 * it's only reached via function pointer - pushed as StartContext1 parameter to
 * PsCreateSystemThreadEx at address 0x001D2852 in xbe_entry_point.
 *
 * Called by sub_001D1818 (thread start routine) via:
 *   RECOMP_ICALL(MEM32(ebp + 8))  where ebp+8 = 0x001D2793
 *
 * What it does:
 *   1. Calls sub_001D3F2F (RenderWare global init)
 *   2. Calls sub_001D2EE5 (engine setup)
 *   3. Reads Xbox KPCR via fs:[0x20] → checks process block at offset 0x250
 *   4. If process block pointer valid, sets up TLS-relative data structure
 *   5. Calls sub_001D3EA2 and sub_001D3E4A (validation/finalization)
 *   6. Calls sub_00156400(0, 0, 0) (cdecl - game subsystem init)
 *   7. Calls sub_001D2E6F(1, 1, 0) (stdcall - enable game systems)
 *   8. Returns 0
 *
 * Uses stdcall: ret 4 (takes 1 parameter from caller - StartContext2)
 *
 * Xbox x86 (0x001D2793-0x001D2806):
 *   call sub_001D3F2F
 *   call sub_001D2EE5
 *   mov eax, fs:[0x20]         ; KPCR from TIB
 *   mov eax, [eax+0x250]       ; process block field
 *   test eax, eax / je skip
 *   mov ecx, [eax+0x24]        ; pointer from process block
 *   ...TLS setup using fs:[0x28], fs:[0x04], [0x41A7D4]...
 *   call sub_001D3EA2
 *   call sub_001D3E4A
 *   push 0/0/0; call sub_00156400; add esp, 0xC
 *   push 0/1/1; call sub_001D2E6F
 *   xor eax, eax; ret 4
 */
void sub_001D2793(void)
{
    uint32_t ebp;

    /* call sub_001D3F2F - RenderWare global init (version/cache check) */
    fprintf(stderr, "  [init] sub_001D3F2F (RW global init)...\n");
    PUSH32(esp, 0); sub_001D3F2F();

    /* call sub_001D2EE5 - engine setup (D3D device, timers, DPCs) */
    fprintf(stderr, "  [init] sub_001D2EE5 (engine setup)...\n");
    PUSH32(esp, 0); sub_001D2EE5();
    fprintf(stderr, "  [init] sub_001D2EE5 done\n");

    /* mov eax, fs:[0x20] - KPCR pointer from fake TIB
     * On Xbox, fs:[0x20] is the KPCR (Kernel Processor Control Region).
     * Our fake TIB at VA 0x20 is initialized to 0 (no KPCR), which
     * causes the code to skip the TLS setup block below. */
    eax = MEM32(0x20);

    /* mov eax, [eax + 0x250] - read from KPCR + 0x250 */
    eax = MEM32(eax + 0x250);

    /* test eax, eax; je loc_001D27B2 */
    if (TEST_Z(eax, eax)) goto loc_001D27B2;

    /* mov ecx, [eax + 0x24] */
    ecx = MEM32(eax + 0x24);

    /* jmp loc_001D27B4 */
    goto loc_001D27B4;

loc_001D27B2:
    /* xor ecx, ecx */
    ecx = 0;

loc_001D27B4:
    /* test ecx, ecx; je loc_001D27DF - skip TLS setup if no pointer */
    if (TEST_Z(ecx, ecx)) goto loc_001D27DF;

    /* push edi (callee-save) */
    PUSH32(esp, edi);

    /* mov eax, fs:[0x28] - TLS array pointer from fake TIB */
    eax = MEM32(0x28);

    /* mov edi, fs:[0x04] - stack base from fake TIB */
    edi = MEM32(0x04);

    /* mov edx, [0x41A7D4] - TLS index for this module */
    edx = MEM32(0x41A7D4);

    /* mov edx, [edi + edx*4] - TLS slot[index] */
    edx = MEM32(edi + edx * 4);

    /* sub edx, [eax + 0x28] - subtract base from RW context */
    edx = edx - MEM32(eax + 0x28);

    /* mov byte [ecx], 1 - set enable flag */
    MEM8(ecx) = 1;

    /* add edx, 8 */
    edx = edx + 8;

    /* mov [ecx + 4], edx - store TLS-relative offset */
    MEM32(ecx + 4) = edx;

    /* pop edi */
    POP32(esp, edi);

loc_001D27DF:
    /* call sub_001D3EA2 - validation/finalization */
    fprintf(stderr, "  [init] sub_001D3EA2 (RW validate)... MEM32(0x754D94)=0x%08X\n", MEM32(0x754D94));
    PUSH32(esp, 0); sub_001D3EA2();
    fprintf(stderr, "  [init] after sub_001D3EA2: MEM32(0x754D94)=0x%08X\n", MEM32(0x754D94));

    /* call sub_001D3E4A - C++ static constructors */
    fprintf(stderr, "  [init] sub_001D3E4A (static init)... MEM32(0x754D94)=0x%08X\n", MEM32(0x754D94));
    PUSH32(esp, 0); sub_001D3E4A();
    fprintf(stderr, "  [init] sub_001D3E4A done\n");

    /* push 0; push 0; push 0; call sub_00156400; add esp, 0xC (cdecl) */
    fprintf(stderr, "  [init] sub_00156400 (game subsystem init)...\n");
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0); sub_00156400();
    esp += 0xC;  /* cdecl: caller cleans 3 args */

    /* push 0; push 1; push 1; call sub_001D2E6F (stdcall: callee cleans) */
    fprintf(stderr, "  [init] sub_001D2E6F (enable game systems)...\n");
    PUSH32(esp, 0);
    PUSH32(esp, 1);
    PUSH32(esp, 1);
    PUSH32(esp, 0); sub_001D2E6F();

    /* xor eax, eax - return 0 */
    eax = 0;

    /* ret 4 - stdcall: pop return addr + 1 parameter */
    esp += 4;  /* pop dummy return address */
    esp += 4;  /* pop 1 parameter (StartContext2 from caller) */
    return;
}

/**
 * sub_00249B7C - CRT FPU exception handler (with inlined tail jump target)
 *
 * Original x86: 0x00249B7C sets up an EBP frame, copies some args to locals,
 * then tail-jumps (JMP) to 0x00249B9C which continues using the same frame.
 * The auto-recompiler emits this as two separate C functions, but sub_00249B9C
 * has its own local `ebp` which starts uninitialized (0). When it does
 * `MEMD(ebp - 8)`, it writes to Xbox VA 0xFFFFFFF8 → crash.
 *
 * Fix: inline sub_00249B9C's code so both halves share the same `ebp` local.
 *
 * This is the CRT _fltused / __control87 / pow helper chain:
 *   sub_00244E9C → sub_00244EC0 → sub_00249CB9 → sub_00249B7C → sub_0024BC71
 */
void sub_00249B7C(void)
{
    uint32_t ebp;
    double _fp_stack[8];
    int _fp_top = 0;
    #define fp_push_m(v) (_fp_stack[--_fp_top & 7] = (v))
    #define fp_pop_m() (_fp_top++)
    #define fp_popp_m() (fp_pop_m())
    #define fp_top_m() _fp_stack[_fp_top & 7]

    /* sub_00249B7C prologue */
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp + 0xFFFFFFE0u; /* sub esp, 0x20 */
    MEM32(ebp - 32) = eax;
    eax = MEM32(ebp + 0x18);
    MEM32(ebp - 16) = eax;
    eax = MEM32(ebp + 0x1C);
    MEM32(ebp - 12) = eax;

    /* --- inlined sub_00249B9C (tail jump target) --- */
    MEMD(ebp - 8) = fp_top_m(); fp_popp_m(); /* fstp qword [ebp-8] */
    MEM32(ebp - 28) = ecx;
    eax = MEM32(ebp + 0x10);
    ecx = MEM32(ebp + 0x14);
    MEM32(ebp - 24) = eax;
    MEM32(ebp - 20) = ecx;
    eax = ebp + 8;
    ecx = ebp - 32;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_0024BC71();

    /* loc_00249BBC */
    esp = esp + 0xC;
    fp_push_m(MEMD(ebp - 8)); /* fld qword [ebp-8] */
    if (!CMP_EQ(MEM16(ebp + 8), 0x27F)) {
        /* fldcw word ptr [ebp + 8] - FPU control word restore (no-op for us) */
    }

    /* leave; ret */
    esp = ebp;
    POP32(esp, ebp);
    esp += 4;
    return;

    #undef fp_push_m
    #undef fp_pop_m
    #undef fp_popp_m
    #undef fp_top_m
}

/**
 * sub_00249B9C - continuation of sub_00249B7C (shared frame)
 *
 * This is the tail jump target of sub_00249B7C. In rare cases it's called
 * directly (not through sub_00249B7C). When called directly, ebp must be
 * inherited from the caller via g_seh_ebp. But typically it's only reached
 * via the tail jump, which we've inlined above. This stub exists so the
 * dispatch table doesn't call the broken generated version.
 */
void sub_00249B9C(void)
{
    uint32_t ebp;
    double _fp_stack[8];
    int _fp_top = 0;
    #define fp_push_m(v) (_fp_stack[--_fp_top & 7] = (v))
    #define fp_pop_m() (_fp_top++)
    #define fp_popp_m() (fp_pop_m())
    #define fp_top_m() _fp_stack[_fp_top & 7]

    /* Inherit ebp from caller - this function expects to share a frame */
    ebp = g_seh_ebp;

    MEMD(ebp - 8) = fp_top_m(); fp_popp_m();
    MEM32(ebp - 28) = ecx;
    eax = MEM32(ebp + 0x10);
    ecx = MEM32(ebp + 0x14);
    MEM32(ebp - 24) = eax;
    MEM32(ebp - 20) = ecx;
    eax = ebp + 8;
    ecx = ebp - 32;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_0024BC71();

    esp = esp + 0xC;
    fp_push_m(MEMD(ebp - 8));
    if (!CMP_EQ(MEM16(ebp + 8), 0x27F)) {
        /* fldcw - no-op */
    }

    esp = ebp;
    POP32(esp, ebp);
    esp += 4;
    return;

    #undef fp_push_m
    #undef fp_pop_m
    #undef fp_popp_m
    #undef fp_top_m
}

