/**
 * Burnout 3: Takedown - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize graphics (D3D8→D3D11)
 * 5. Initialize audio (DirectSound→XAudio2)
 * 6. Initialize input (XPP→XInput)
 * 7. Call the game's original entry point (recompiled)
 *
 * The recompiled game code lives in separate translation units generated
 * from the original x86 machine code. Each function is translated to C
 * with the same calling convention and register usage.
 */

#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <dbghelp.h>
#include <xinput.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "xinput.lib")
#endif

/* Compatibility layers */
#include "../kernel/kernel.h"
#include "../kernel/xbox_memory_layout.h"
#include "../d3d/d3d8_xbox.h"
#include "../audio/dsound_xbox.h"
#include "../input/xinput_xbox.h"

/* Asset loading */
#include "txd_loader.h"
#include "bgv_loader.h"

/* 3D renderer */
#include "rw_renderer.h"

/* Video player (boot sequence) */
#include "video_player.h"
#include "menu_gui.h"

/* Audio wave dictionary loader */
#include "awd_loader.h"
#include "../apu/apu.h"

/* RW→D3D11 rendering bridge */
#include "rw_bridge.h"
/* Frontend menu renderer */
#include "fe_menu.h"

/* Global AWD files */
AWDFile *g_awd_fe = NULL;
AWDFile *g_awd_generic = NULL;

/* Recompiled code */
#include "recomp/gen/recomp_funcs.h"

/* ── Crash diagnostics + SEH simulation ────────────────────── */

/*
 * Mini x86-64 instruction decoder for VEH fault skipping.
 *
 * When the RenderWare engine probes memory past 64MB, the real Xbox
 * dispatches the fault through SEH. The game's __try/__except catches it
 * to determine available RAM. We simulate this by decoding the faulting
 * instruction, setting the destination register to 0 (as if reading from
 * unmapped memory), and advancing RIP past the instruction.
 */
static int g_seh_skip_count = 0;

#if defined(_WIN32)
/* The VEH instruction decoder + crash handler is x86-64 + Win32 CONTEXT
 * specific. On Linux the equivalent goes through sigaction(SIGSEGV/SIGFPE)
 * with ucontext_t/mcontext_t (deferred); for now the registration is a
 * no-op so the game links and runs without the recovery path. */
static BOOL veh_skip_faulting_read(PCONTEXT ctx)
{
    uint8_t *rip = (uint8_t *)ctx->Rip;
    int prefix_len = 0;
    int rex_w = 0, rex_r = 0, rex_x = 0, rex_b = 0;

    /* Map register index to CONTEXT field */
    DWORD64 *gpr[] = {
        &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
        &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
        &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
        &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15
    };

    /* Parse legacy prefixes (segment, operand size, etc.) */
    while (prefix_len < 4) {
        uint8_t b = rip[prefix_len];
        if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
            b == 0x64 || b == 0x65) {
            prefix_len++;
        } else {
            break;
        }
    }

    /* Parse REX prefix (0x40-0x4F) */
    if ((rip[prefix_len] & 0xF0) == 0x40) {
        uint8_t rex = rip[prefix_len];
        rex_w = (rex >> 3) & 1;
        rex_r = (rex >> 2) & 1;
        rex_x = (rex >> 1) & 1;
        rex_b = rex & 1;
        prefix_len++;
    }

    uint8_t *op = rip + prefix_len;

    /* Calculate ModRM displacement length */
    /* Returns total bytes for modrm + optional SIB + displacement */
    #define MODRM_LEN(modrm_byte) do { \
        int _mod = ((modrm_byte) >> 6) & 3; \
        int _rm  = ((modrm_byte) & 7) | (rex_b << 3); \
        modrm_total = 1; /* modrm byte itself */ \
        if (_mod == 0 && (_rm & 7) == 4) modrm_total++; /* SIB */ \
        if (_mod == 0 && (_rm & 7) == 5) modrm_total += 4; /* RIP-rel disp32 */ \
        if (_mod == 1) { modrm_total++; if ((_rm & 7) == 4) modrm_total++; } \
        if (_mod == 2) { modrm_total += 4; if ((_rm & 7) == 4) modrm_total++; } \
        if (_mod == 3) modrm_total = 1; /* reg-reg, shouldn't fault */ \
    } while(0)

    int modrm_total = 0;
    int reg_idx;

    /* 8B /r : mov r32/r64, r/m32/r/m64 */
    if (op[0] == 0x8B) {
        reg_idx = ((op[1] >> 3) & 7) | (rex_r << 3);
        MODRM_LEN(op[1]);
        *gpr[reg_idx] = 0;
        ctx->Rip += prefix_len + 1 + modrm_total;
        return TRUE;
    }

    /* 8A /r : mov r8, r/m8 */
    if (op[0] == 0x8A) {
        reg_idx = ((op[1] >> 3) & 7) | (rex_r << 3);
        MODRM_LEN(op[1]);
        /* Zero just the low byte of the register */
        *gpr[reg_idx] &= ~(DWORD64)0xFF;
        ctx->Rip += prefix_len + 1 + modrm_total;
        return TRUE;
    }

    /* 0F B6 /r : movzx r32, r/m8 */
    /* 0F B7 /r : movzx r32, r/m16 */
    /* 0F BE /r : movsx r32, r/m8 */
    /* 0F BF /r : movsx r32, r/m16 */
    if (op[0] == 0x0F && (op[1] == 0xB6 || op[1] == 0xB7 ||
                           op[1] == 0xBE || op[1] == 0xBF)) {
        reg_idx = ((op[2] >> 3) & 7) | (rex_r << 3);
        MODRM_LEN(op[2]);
        *gpr[reg_idx] = 0;
        ctx->Rip += prefix_len + 2 + modrm_total;
        return TRUE;
    }

    /* 3B /r : cmp r32, r/m32 - set flags as if comparing with 0 */
    if (op[0] == 0x3B) {
        reg_idx = ((op[1] >> 3) & 7) | (rex_r << 3);
        MODRM_LEN(op[1]);
        /* Set ZF=0, CF based on comparison with 0 */
        DWORD64 val = *gpr[reg_idx];
        ctx->EFlags &= ~(0x8D5);  /* clear OF, SF, ZF, AF, PF, CF */
        if (val == 0) ctx->EFlags |= 0x40;  /* ZF */
        if (val & (rex_w ? 0x8000000000000000ULL : 0x80000000ULL))
            ctx->EFlags |= 0x80;  /* SF */
        ctx->Rip += prefix_len + 1 + modrm_total;
        return TRUE;
    }

    /* 39 /r : cmp r/m32, r32 - set flags as if mem=0 */
    if (op[0] == 0x39) {
        reg_idx = ((op[1] >> 3) & 7) | (rex_r << 3);
        MODRM_LEN(op[1]);
        DWORD64 val = *gpr[reg_idx];
        ctx->EFlags &= ~(0x8D5);
        if (val == 0) ctx->EFlags |= 0x40;  /* ZF: 0 == val */
        /* 0 - val: CF set if val != 0 */
        if (val != 0) ctx->EFlags |= 0x01;  /* CF */
        /* SF: sign of (0 - val) */
        DWORD64 result = (rex_w ? (DWORD64)(-(int64_t)val) : (DWORD64)(uint32_t)(-(int32_t)(uint32_t)val));
        if (result & (rex_w ? 0x8000000000000000ULL : 0x80000000ULL))
            ctx->EFlags |= 0x80;  /* SF */
        ctx->Rip += prefix_len + 1 + modrm_total;
        return TRUE;
    }

    /* SSE instructions with memory operands.
     * These use legacy prefixes (F3/F2/66/none) + 0F opcode + modrm.
     * For faulting memory reads, zero the destination XMM register
     * and advance RIP to let execution continue.
     */
    {
        int has_f3 = 0, has_f2 = 0, has_66 = 0;
        for (int i = 0; i < prefix_len; i++) {
            if (rip[i] == 0xF3) has_f3 = 1;
            if (rip[i] == 0xF2) has_f2 = 1;
            if (rip[i] == 0x66) has_66 = 1;
        }

        if (op[0] == 0x0F) {
            int is_sse_mem_read = 0;

            /* F3 0F xx: scalar single-precision */
            if (has_f3) {
                switch (op[1]) {
                case 0x10: /* movss xmm, m32 */
                case 0x58: /* addss xmm, m32 */
                case 0x59: /* mulss xmm, m32 */
                case 0x5C: /* subss xmm, m32 */
                case 0x5E: /* divss xmm, m32 */
                case 0x51: /* sqrtss xmm, m32 */
                case 0x5D: /* minss xmm, m32 */
                case 0x5F: /* maxss xmm, m32 */
                case 0x2A: /* cvtsi2ss xmm, r/m32 */
                case 0x2C: /* cvttss2si r32, xmm/m32 */
                case 0x2D: /* cvtss2si r32, xmm/m32 */
                    is_sse_mem_read = 1;
                    break;
                }
            }
            /* F2 0F xx: scalar double-precision */
            else if (has_f2) {
                switch (op[1]) {
                case 0x10: /* movsd xmm, m64 */
                case 0x58: /* addsd */
                case 0x59: /* mulsd */
                case 0x5C: /* subsd */
                case 0x5E: /* divsd */
                    is_sse_mem_read = 1;
                    break;
                }
            }
            /* 66 0F xx: packed double / integer */
            else if (has_66) {
                switch (op[1]) {
                case 0x28: /* movapd xmm, m128 */
                case 0x10: /* movupd xmm, m128 */
                case 0x6F: /* movdqa xmm, m128 */
                    is_sse_mem_read = 1;
                    break;
                }
            }
            /* No prefix: packed single-precision */
            else {
                switch (op[1]) {
                case 0x28: /* movaps xmm, m128 */
                case 0x10: /* movups xmm, m128 */
                case 0x58: /* addps xmm, m128 */
                case 0x59: /* mulps xmm, m128 */
                case 0x5C: /* subps xmm, m128 */
                case 0x5E: /* divps xmm, m128 */
                    is_sse_mem_read = 1;
                    break;
                }
            }

            if (is_sse_mem_read) {
                int xmm_idx = ((op[2] >> 3) & 7) | (rex_r << 3);
                MODRM_LEN(op[2]);

                /* For cvttss2si/cvtss2si, dest is GPR, not XMM */
                if ((has_f3 && (op[1] == 0x2C || op[1] == 0x2D))) {
                    *gpr[xmm_idx] = 0;
                } else if (xmm_idx < 16) {
                    M128A *xmm = &ctx->Xmm0 + xmm_idx;
                    xmm->Low = 0;
                    xmm->High = 0;
                }
                ctx->Rip += prefix_len + 2 + modrm_total;
                return TRUE;
            }
        }
    }

    #undef MODRM_LEN
    return FALSE;
}

/**
 * Skip a faulting write instruction by advancing RIP past it.
 * Unlike read skips, write skips don't need to set a register.
 * Handles common store instructions: mov r/m, r and mov r/m, imm.
 */
static BOOL veh_skip_faulting_write(PCONTEXT ctx)
{
    uint8_t *rip = (uint8_t *)ctx->Rip;
    int prefix_len = 0;
    int rex_w = 0, rex_r = 0, rex_b = 0;

    /* Parse legacy prefixes */
    while (prefix_len < 4) {
        uint8_t b = rip[prefix_len];
        if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
            b == 0x64 || b == 0x65) {
            prefix_len++;
        } else {
            break;
        }
    }

    /* Parse REX prefix */
    if ((rip[prefix_len] & 0xF0) == 0x40) {
        uint8_t rex = rip[prefix_len];
        rex_w = (rex >> 3) & 1;
        rex_r = (rex >> 2) & 1;
        rex_b = rex & 1;
        prefix_len++;
    }

    uint8_t *op = rip + prefix_len;
    int modrm_total = 0;

    #define MODRM_LEN(modrm_byte) do { \
        int _mod = ((modrm_byte) >> 6) & 3; \
        int _rm  = ((modrm_byte) & 7) | (rex_b << 3); \
        modrm_total = 1; \
        if (_mod == 0 && (_rm & 7) == 4) modrm_total++; \
        if (_mod == 0 && (_rm & 7) == 5) modrm_total += 4; \
        if (_mod == 1) { modrm_total++; if ((_rm & 7) == 4) modrm_total++; } \
        if (_mod == 2) { modrm_total += 4; if ((_rm & 7) == 4) modrm_total++; } \
        if (_mod == 3) modrm_total = 1; \
    } while(0)

    (void)rex_r; (void)rex_w;

    /* 89 /r : mov r/m32, r32 */
    if (op[0] == 0x89) {
        MODRM_LEN(op[1]);
        ctx->Rip += prefix_len + 1 + modrm_total;
        return TRUE;
    }

    /* 88 /r : mov r/m8, r8 */
    if (op[0] == 0x88) {
        MODRM_LEN(op[1]);
        ctx->Rip += prefix_len + 1 + modrm_total;
        return TRUE;
    }

    /* C7 /0 id : mov r/m32, imm32 */
    if (op[0] == 0xC7) {
        MODRM_LEN(op[1]);
        ctx->Rip += prefix_len + 1 + modrm_total + 4;
        return TRUE;
    }

    /* C6 /0 ib : mov r/m8, imm8 */
    if (op[0] == 0xC6) {
        MODRM_LEN(op[1]);
        ctx->Rip += prefix_len + 1 + modrm_total + 1;
        return TRUE;
    }

    /* 66 89 /r : mov r/m16, r16 (handled via 0x66 prefix + 89) */
    /* Already handled above since 0x66 is parsed as prefix */

    /* 0F 11 /r : movups xmm, m128 (SSE store) */
    if (op[0] == 0x0F && op[1] == 0x11) {
        MODRM_LEN(op[2]);
        ctx->Rip += prefix_len + 2 + modrm_total;
        return TRUE;
    }

    /* F3 0F 11 /r : movss m32, xmm (SSE scalar store) */
    {
        int has_f3 = 0;
        for (int i = 0; i < prefix_len; i++) {
            if (rip[i] == 0xF3) has_f3 = 1;
        }
        if (has_f3 && op[0] == 0x0F && op[1] == 0x11) {
            MODRM_LEN(op[2]);
            ctx->Rip += prefix_len + 2 + modrm_total;
            return TRUE;
        }

        /* F3 A4 : rep movsb (inline memcpy)
         * F3 A5 : rep movsd (inline memcpy, 4-byte)
         * F3 AA : rep stosb (inline memset)
         * F3 AB : rep stosd (inline memset, 4-byte)
         *
         * Cancel remaining iterations: set RCX=0, advance RSI/RDI past
         * the unmapped region. The rep prefix with RCX=0 is a no-op,
         * so the CPU will naturally advance RIP past the instruction.
         * Assumes DF=0 (CLD), which is standard for MSVC code.
         */
        if (has_f3 && (op[0] == 0xA4 || op[0] == 0xA5)) {
            /* rep movsb / rep movsd */
            uint64_t stride = (op[0] == 0xA5) ? 4 : 1;
            uint64_t remaining = ctx->Rcx * stride;
            ctx->Rcx = 0;
            ctx->Rsi += remaining;
            ctx->Rdi += remaining;
            return TRUE;
        }
        if (has_f3 && (op[0] == 0xAA || op[0] == 0xAB)) {
            /* rep stosb / rep stosd */
            uint64_t stride = (op[0] == 0xAB) ? 4 : 1;
            uint64_t remaining = ctx->Rcx * stride;
            ctx->Rcx = 0;
            ctx->Rdi += remaining;
            return TRUE;
        }
    }

    #undef MODRM_LEN
    return FALSE;
}

static LONG WINAPI crash_veh(PEXCEPTION_POINTERS info)
{
    /* Catch stack overflow early - can't do much except report and die */
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr, "\n=== NATIVE STACK OVERFLOW at RIP=0x%p ===\n",
                info->ExceptionRecord->ExceptionAddress);
        fprintf(stderr, "  RSP=0x%p  Xbox ESP=0x%08X\n",
                (void*)info->ContextRecord->Rsp, g_esp);
        fflush(stderr);
        ExitProcess(42);
    }

    /*
     * Handle integer divide by zero in recompiled code.
     *
     * Some recompiled functions contain div/idiv instructions that can
     * fault when the divisor is zero (e.g., uninitialized state during
     * state machine transitions). Rather than crashing, we set the
     * quotient/remainder to 0 and skip the instruction.
     *
     * x86-64 div/idiv format: [REX] F7 ModRM (opcode ext /6 or /7)
     * Result: EAX=quotient, EDX=remainder (both cleared on fault)
     */
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        static int div0_count = 0;
        uint8_t *rip = (uint8_t *)info->ContextRecord->Rip;
        int prefix_len = 0;
        int rex_b = 0;

        /* Parse legacy prefixes */
        while (prefix_len < 4) {
            uint8_t b = rip[prefix_len];
            if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
                b == 0x64 || b == 0x65) {
                prefix_len++;
            } else {
                break;
            }
        }
        /* Parse REX prefix */
        if ((rip[prefix_len] & 0xF0) == 0x40) {
            rex_b = rip[prefix_len] & 1;
            prefix_len++;
        }

        uint8_t *op = rip + prefix_len;
        /* F7 /6 = div r/m32, F7 /7 = idiv r/m32 */
        if (op[0] == 0xF7) {
            int reg_ext = (op[1] >> 3) & 7;
            if (reg_ext == 6 || reg_ext == 7) {
                /* Calculate instruction length (F7 + ModRM + disp) */
                int mod = (op[1] >> 6) & 3;
                int rm = (op[1] & 7) | (rex_b << 3);
                int modrm_total = 1; /* modrm byte */
                if (mod == 0 && (rm & 7) == 4) modrm_total++; /* SIB */
                if (mod == 0 && (rm & 7) == 5) modrm_total += 4; /* RIP-rel */
                if (mod == 1) { modrm_total++; if ((rm & 7) == 4) modrm_total++; }
                if (mod == 2) { modrm_total += 4; if ((rm & 7) == 4) modrm_total++; }

                /* Clear result registers and skip */
                info->ContextRecord->Rax = 0;
                info->ContextRecord->Rdx = 0;
                info->ContextRecord->Rip += prefix_len + 1 + modrm_total;

                div0_count++;
                if (div0_count <= 5 || (div0_count % 10000) == 0) {
                    fprintf(stderr, "  [DIV0 #%d] Skipped %s at RIP=0x%p (Xbox ESP=0x%08X)\n",
                            div0_count, reg_ext == 6 ? "div" : "idiv",
                            info->ExceptionRecord->ExceptionAddress, g_esp);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        /* Unknown divide instruction - fall through to crash reporting */
    }

    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = info->ExceptionRecord->ExceptionInformation[1];
        int is_write = (int)info->ExceptionRecord->ExceptionInformation[0];

        /*
         * Guard: reject native addresses below the Xbox memory base.
         * These are null pointer dereferences or wild pointers that, when
         * offset-subtracted, wrap to large Xbox VAs (e.g., native 0x0
         * wraps to Xbox VA 0xF0000000 which looks like NV2A GPU space).
         */
        if (fault_addr < (uintptr_t)g_xbox_mem_offset) {
            /* Fall through to crash reporting at bottom of handler */
            goto veh_crash_report;
        }

        /*
         * Guard: 32-bit overflow addresses.
         *
         * When recompiled code computes an Xbox VA >= 0xFFFFFFFF and does
         * a multi-byte access (e.g., MEM32(0xFFFFFFFF)), the read spans
         * bytes 0xFFFFFFFF..0x100000002. The CPU faults at the page
         * boundary (native offset 0x100000000+), which exceeds 32 bits.
         * Converting back to Xbox VA wraps to 0, confusing downstream
         * handlers (NV2A, mirror, etc.).
         *
         * Skip these directly - they're sentinel/NULL pointer accesses.
         */
        if ((fault_addr - (uintptr_t)g_xbox_mem_offset) >= 0x100000000ULL) {
            if (!is_write) {
                if (veh_skip_faulting_read(info->ContextRecord)) {
                    static int overflow_skip_count = 0;
                    overflow_skip_count++;
                    if (overflow_skip_count <= 5 || (overflow_skip_count % 50000) == 0) {
                        fprintf(stderr, "  [SKIP-OVERFLOW #%d] native=%p (32-bit VA overflow)\n",
                                overflow_skip_count, (void*)fault_addr);
                        fflush(stderr);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            } else {
                if (veh_skip_faulting_write(info->ContextRecord)) {
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            goto veh_crash_report;
        }

        /*
         * NV2A GPU / hardware I/O address space (0xF0000000+):
         *   0xF0000000-0xF3FFFFFF  GPU framebuffer / texture memory
         *   0xF4000000-0xFCFFFFFF  AGP aperture, push buffer DMA, misc HW
         *   0xFD000000-0xFDFFFFFF  GPU MMIO registers (→ NV2A state machine)
         *   0xFE000000+            Flash ROM, misc
         *
         * MMIO registers (0xFD000000+) are routed through xemu's NV2A
         * register handlers via instruction decoding in the VEH.
         * Other GPU ranges get zero-filled pages as before.
         */
        {
            uint32_t fault_xbox_va = (uint32_t)(fault_addr - g_xbox_mem_offset);
            if (fault_xbox_va >= 0xF0000000u) {
                /* GPU MMIO registers: decode instruction, route to NV2A */
                if (fault_xbox_va >= 0xFD000000u && fault_xbox_va < 0xFE000000u) {
                    extern bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                                      uint32_t fault_xbox_va, int is_write);
                    if (nv2a_hook_handle_mmio(info->ContextRecord, fault_addr,
                                              fault_xbox_va, is_write)) {
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    /* Decode failed - fall through to zero page allocation */
                }

                /* APU MMIO registers: 0xFE800000-0xFE87FFFF (512KB) */
                if (fault_xbox_va >= 0xFE800000u && fault_xbox_va < 0xFE880000u) {
                    extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                                     uint32_t fault_xbox_va, int is_write);
                    if (apu_hook_handle_mmio(info->ContextRecord, fault_addr,
                                             fault_xbox_va, is_write)) {
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                /* Framebuffer / push buffer / other GPU memory */
                static int nv2a_page_count = 0;
                uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF; /* 64KB align */
                LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!result) {
                    result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                         MEM_COMMIT, PAGE_READWRITE);
                }
                if (result) {
                    memset(result, 0, 0x10000);
                    nv2a_page_count++;
                    if (nv2a_page_count <= 20 || (nv2a_page_count % 100) == 0) {
                        const char *region = (fault_xbox_va >= 0xFD000000u) ? "reg" :
                                              (fault_xbox_va >= 0xF4000000u) ? "io" : "fb";
                        fprintf(stderr, "  [NV2A] GPU %s page 0x%08X (%s) [page #%d]\n",
                                region, fault_xbox_va & 0xFFFF0000u,
                                is_write ? "W" : "R", nv2a_page_count);
                        fflush(stderr);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        /*
         * Xbox mirror memory mapping (0x80000000 and 0xC0000000 ranges).
         *
         * On Xbox, physical RAM is accessible through three virtual address ranges:
         *   0x00000000-0x03FFFFFF  Cached (normal CPU access)
         *   0x80000000-0x83FFFFFF  Cached mirror
         *   0xC0000000-0xC3FFFFFF  Uncached / write-combined (GPU-coherent)
         *
         * All three map to the same 64MB of physical DRAM. The D3D library uses
         * the uncached mapping for push buffers, vertex data, and other GPU
         * resources that need write-combined access for DMA coherency.
         *
         * We handle faults in this range on-demand: allocate pages and copy the
         * initial data from the cached mapping. Since we replace D3D at a higher
         * level, the push buffer data written here is not consumed by real GPU
         * hardware, but the memory must be writable for the D3D code to function.
         */
        {
            uint32_t fault_xbox_va = (uint32_t)(fault_addr - g_xbox_mem_offset);
            if ((fault_xbox_va >= 0x80000000u && fault_xbox_va < 0x84000000u) ||
                (fault_xbox_va >= 0xC0000000u && fault_xbox_va < 0xC4000000u)) {
                static int uncached_page_count = 0;
                uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF; /* 64KB align */
                LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!result) {
                    result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                         MEM_COMMIT, PAGE_READWRITE);
                }
                if (result) {
                    /* Copy initial data from cached mapping as baseline */
                    uint32_t page_xbox_va = (uint32_t)(alloc_base - g_xbox_mem_offset);
                    uint32_t cached_va = (page_xbox_va >= 0xC0000000u)
                        ? page_xbox_va - 0xC0000000u
                        : page_xbox_va - 0x80000000u;
                    if (cached_va < XBOX_TOTAL_RAM) {
                        void *src = (void *)((uintptr_t)cached_va + g_xbox_mem_offset);
                        memcpy(result, src, 0x10000);
                    }
                    uncached_page_count++;
                    if (uncached_page_count <= 20 || (uncached_page_count % 100) == 0) {
                        fprintf(stderr, "  [UNCACHED] Xbox VA 0x%08X (%s) [page #%d]\n",
                                page_xbox_va, is_write ? "W" : "R", uncached_page_count);
                        fflush(stderr);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        /*
         * Fallback mirror mapping for addresses past the pre-mapped views.
         *
         * The base 64 MB region and XBOX_NUM_MIRRORS mirror views are mapped
         * at init time via CreateFileMapping + MapViewOfFileEx (true aliases).
         * If a fault occurs past the pre-mapped range, map an additional 64 MB
         * view on demand. This handles edge cases where the memory walker or
         * init code accesses beyond the pre-mapped 1+ GB.
         */
        {
            uintptr_t xbox_region_end = (uintptr_t)(XBOX_HEAP_BASE + XBOX_HEAP_SIZE +
                                                     XBOX_GUARD_SIZE) + g_xbox_mem_offset;
            if (fault_addr >= xbox_region_end) {
                uint32_t fault_xbox_va = (uint32_t)(fault_addr - g_xbox_mem_offset);

                /* Map mirrors for all RAM aliases below the NV2A MMIO range.
                 * Xbox memory map: 0x00-0x03 = cached RAM, 0x80-0x83 = uncached,
                 * 0xC0-0xC3 = write-combined, 0xD0+ = contiguous GPU aperture.
                 * All map to the same 64 MB physical RAM (modulo 0x04000000). */
                if (fault_xbox_va < 0xF0000000u) {
                    HANDLE hMap = xbox_GetMappingHandle();
                    if (hMap) {
                        /* Map a full 64 MB view aligned to the mirror boundary */
                        uint32_t mirror_idx = fault_xbox_va / XBOX_TOTAL_RAM;
                        uintptr_t view_base = (uintptr_t)g_xbox_mem_offset +
                                              (uintptr_t)mirror_idx * XBOX_TOTAL_RAM;
                        LPVOID result = MapViewOfFileEx(
                            hMap, FILE_MAP_ALL_ACCESS, 0, 0,
                            XBOX_TOTAL_RAM, (LPVOID)view_base);
                        if (result) {
                            static int fallback_mirror_count = 0;
                            fallback_mirror_count++;
                            fprintf(stderr, "  [MIRROR-FALLBACK] view %d at %p "
                                    "(Xbox VA 0x%08X-0x%08X)\n",
                                    mirror_idx, result,
                                    mirror_idx * XBOX_TOTAL_RAM,
                                    (mirror_idx + 1) * XBOX_TOTAL_RAM);
                            fflush(stderr);
                            return EXCEPTION_CONTINUE_EXECUTION;
                        } else {
                            static int mirror_fail_count = 0;
                            mirror_fail_count++;
                            if (mirror_fail_count <= 3 || (mirror_fail_count % 50000) == 0) {
                                fprintf(stderr, "  [MIRROR-FAIL] view %d at %p "
                                        "(Xbox VA 0x%08X, error %lu) [#%d]\n",
                                        mirror_idx, (void*)view_base,
                                        fault_xbox_va, GetLastError(),
                                        mirror_fail_count);
                                fflush(stderr);
                            }
                        }
                    }
                }
            }
        }

        /*
         * Bounded fault-skip for isolated bad reads.
         *
         * The Xbox D3D8 code (statically linked) and RW Xbox display driver
         * read from internal D3D structures that don't exist in our D3D11 shim.
         * Rather than crashing on each bad read, decode the faulting instruction,
         * return 0 to the destination register, and advance RIP. This lets the
         * init code proceed and shows us the full call flow.
         *
         * Limited to MAX_FAULT_SKIPS to avoid infinite loops.
         * Writes still crash immediately (they indicate a real problem).
         */
        #define MAX_FAULT_SKIPS 10000000  /* 10M: rendering loop produces many skips */
        if (!is_write) {
            static int fault_skip_count = 0;
            if (fault_skip_count < MAX_FAULT_SKIPS) {
                if (veh_skip_faulting_read(info->ContextRecord)) {
                    fault_skip_count++;
                    if (fault_skip_count <= 10 || (fault_skip_count % 100000) == 0) {
                        HMODULE fmod = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                          (LPCSTR)info->ExceptionRecord->ExceptionAddress, &fmod);
                        fprintf(stderr, "  [SKIP-READ #%d] Xbox VA 0x%08X native=%p at RVA 0x%llX "
                                        "(eax=0x%08X ecx=0x%08X edx=0x%08X)\n",
                                fault_skip_count,
                                (uint32_t)(fault_addr - g_xbox_mem_offset),
                                (void *)fault_addr,
                                (unsigned long long)((uintptr_t)info->ExceptionRecord->ExceptionAddress - (uintptr_t)fmod),
                                g_eax, g_ecx, g_edx);
                        fflush(stderr);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        /*
         * Skip writes to unmapped Xbox memory.
         *
         * The Xbox D3D8 library produces garbage internal structure pointers
         * because the NV2A GPU doesn't exist. RW engine code then tries to
         * read/write through these garbage pointers. Skip writes that are
         * outside all known mapped memory regions to let init continue.
         *
         * We also extend the read-skip to work universally below.
         */
        if (is_write) {
            static int write_skip_count = 0;
            if (write_skip_count < MAX_FAULT_SKIPS) {
                if (veh_skip_faulting_write(info->ContextRecord)) {
                    write_skip_count++;
                    uint32_t fault_xbox_va = (uint32_t)(fault_addr - g_xbox_mem_offset);
                    if (write_skip_count <= 50 || (write_skip_count % 10000) == 0) {
                        HMODULE fmod = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                          (LPCSTR)info->ExceptionRecord->ExceptionAddress, &fmod);
                        fprintf(stderr, "  [SKIP-WRITE #%d] Xbox VA 0x%08X at RVA 0x%llX "
                                        "(eax=0x%08X ecx=0x%08X edx=0x%08X)\n",
                                write_skip_count,
                                fault_xbox_va,
                                (unsigned long long)((uintptr_t)info->ExceptionRecord->ExceptionAddress - (uintptr_t)fmod),
                                g_eax, g_ecx, g_edx);
                        fflush(stderr);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        /* Normal crash reporting */
        veh_crash_report:
        void *frames[32];
        USHORT count;
        HMODULE mod;
        char modname[MAX_PATH];
        uintptr_t base;

        fprintf(stderr, "\n=== VEH: Access violation at RIP=0x%p ===\n",
                info->ExceptionRecord->ExceptionAddress);
        fprintf(stderr, "  %s address 0x%p\n",
                is_write ? "Writing" : "Reading",
                (void*)fault_addr);

        /* Get module base to compute RVA */
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          (LPCSTR)info->ExceptionRecord->ExceptionAddress, &mod);
        base = (uintptr_t)mod;
        GetModuleFileNameA(mod, modname, sizeof(modname));
        fprintf(stderr, "  Module: %s (base=0x%p)\n", modname, (void*)base);
        fprintf(stderr, "  Crash RVA: 0x%llX\n",
                (unsigned long long)((uintptr_t)info->ExceptionRecord->ExceptionAddress - base));

        /* Native stack trace */
        count = CaptureStackBackTrace(0, 32, frames, NULL);
        fprintf(stderr, "  Native stack (%d frames):\n", count);
        for (USHORT i = 0; i < count; i++) {
            HMODULE fmod = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              (LPCSTR)frames[i], &fmod);
            fprintf(stderr, "    [%2d] 0x%p (RVA 0x%llX)\n",
                    i, frames[i],
                    (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)fmod));
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else /* !_WIN32 -- stub VEH handler (real port: sigaction + ucontext) */

static LONG crash_veh(PEXCEPTION_POINTERS info)
{
    (void)info;
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif /* _WIN32 */

/* ── Configuration ──────────────────────────────────────────── */

/* Default path to the original XBE file */
/* Forward slash works on both POSIX and Windows. */
#define DEFAULT_XBE_PATH "Burnout 3 Takedown/default.xbe"

/* Window properties */
#define WINDOW_TITLE "Burnout 3: Takedown Recompiled"
#define WINDOW_CLASS "Burnout3RecompClass"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

/* Win32 menu bar item IDs */
#define IDM_FILE_QUIT       1001
#define IDM_CONFIG_SETTINGS 1002
#define IDM_CONFIG_DEBUG    1003
#define IDM_HELP_ABOUT      1004

/* ── Global state ───────────────────────────────────────────── */

static HWND g_hwnd = NULL;
static BOOL g_running = TRUE;
void *g_xbe_data = NULL;
size_t g_xbe_size = 0;
static IDirect3D8 *g_d3d8 = NULL;
static IDirect3DDevice8 *g_d3d_device = NULL;
static IDirectSound8 *g_dsound = NULL;

/* Loaded game textures (non-static: accessed by fe_menu.c) */
TXD_Dict g_global_txd;
int g_textures_loaded = 0;

/* Procedural paint texture for car models */
static IDirect3DTexture8 *g_paint_tex = NULL;
static IDirect3DTexture8 *g_road_tex = NULL;

/* Create a procedural metallic paint texture.
 * Generates a 64x64 A8R8G8B8 texture with metallic flake effect. */
static IDirect3DTexture8 *create_paint_texture(IDirect3DDevice8 *dev,
                                                uint8_t base_r, uint8_t base_g, uint8_t base_b)
{
    IDirect3DTexture8 *tex = NULL;
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, 64, 64, 1, 0,
                                             D3DFMT_A8R8G8B8, 0, &tex);
    if (FAILED(hr) || !tex) return NULL;

    D3DLOCKED_RECT lr;
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->lpVtbl->Release(tex); return NULL; }

    uint32_t *pixels = (uint32_t *)lr.pBits;
    uint32_t seed = 0xDEADBEEF;
    int y, x;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            /* LCG random for metallic flake */
            seed = seed * 1103515245 + 12345;
            float noise = ((float)((seed >> 16) & 0xFF) / 255.0f) * 2.0f - 1.0f;

            /* Metallic flake: random brightness variation +-8% */
            float flake = 1.0f + noise * 0.08f;

            /* Fresnel-like edge brightening using UV-space distance from center */
            float fx = ((float)x / 63.0f) * 2.0f - 1.0f;
            float fy = ((float)y / 63.0f) * 2.0f - 1.0f;
            float edge = fx * fx + fy * fy;
            float fresnel = 1.0f + edge * 0.15f;

            /* Combine */
            float r = (float)base_r * flake * fresnel;
            float g = (float)base_g * flake * fresnel;
            float b = (float)base_b * flake * fresnel;

            /* Clamp */
            if (r > 255.0f) r = 255.0f; if (r < 0.0f) r = 0.0f;
            if (g > 255.0f) g = 255.0f; if (g < 0.0f) g = 0.0f;
            if (b > 255.0f) b = 255.0f; if (b < 0.0f) b = 0.0f;

            pixels[y * (lr.Pitch / 4) + x] = 0xFF000000 |
                ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    tex->lpVtbl->UnlockRect(tex, 0);
    return tex;
}

/* Create a procedural asphalt road texture.
 * 128x128 with grain/noise pattern for realistic road surface. */
static IDirect3DTexture8 *create_road_texture(IDirect3DDevice8 *dev)
{
    IDirect3DTexture8 *tex = NULL;
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, 128, 128, 1, 0,
                                             D3DFMT_A8R8G8B8, 0, &tex);
    if (FAILED(hr) || !tex) return NULL;

    D3DLOCKED_RECT lr;
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->lpVtbl->Release(tex); return NULL; }

    uint32_t *pixels = (uint32_t *)lr.pBits;
    uint32_t seed = 0x12345678;
    int y, x;
    for (y = 0; y < 128; y++) {
        for (x = 0; x < 128; x++) {
            /* Asphalt base: dark grey with random grain */
            seed = seed * 1103515245 + 12345;
            float noise1 = ((float)((seed >> 16) & 0xFF) / 255.0f);
            seed = seed * 1103515245 + 12345;
            float noise2 = ((float)((seed >> 16) & 0xFF) / 255.0f);

            /* Mix fine grain + coarse patches */
            float fine = noise1 * 0.15f;
            float coarse = noise2 * 0.08f;
            float base_v = 0.55f + fine + coarse;

            /* Occasional light speckle (aggregate in asphalt) */
            seed = seed * 1103515245 + 12345;
            if (((seed >> 20) & 0x1F) == 0) {
                base_v += 0.12f;
            }

            if (base_v > 1.0f) base_v = 1.0f;
            uint8_t v = (uint8_t)(base_v * 255.0f);
            pixels[y * (lr.Pitch / 4) + x] = 0xFF000000 | (v << 16) | (v << 8) | v;
        }
    }
    tex->lpVtbl->UnlockRect(tex, 0);
    return tex;
}

/* Paint color palette per vehicle class */
static void get_class_paint_color(const char *class_name, int car_num,
                                   uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Default: silver metallic */
    *r = 180; *g = 185; *b = 195;

    if (!class_name) return;

    /* Each class gets a signature paint color, with variation per car number */
    if (strcmp(class_name, "COMP") == 0) {
        /* Compact: candy red */
        *r = 210; *g = 30; *b = 30;
    } else if (strcmp(class_name, "CUPE") == 0) {
        /* Coupe: electric blue */
        *r = 40; *g = 80; *b = 220;
    } else if (strcmp(class_name, "HEVY") == 0) {
        /* Heavy: dark green */
        *r = 30; *g = 130; *b = 50;
    } else if (strcmp(class_name, "HSPC") == 0) {
        /* Hypercar: gold */
        *r = 220; *g = 180; *b = 40;
    } else if (strcmp(class_name, "MSCL") == 0) {
        /* Muscle: deep purple */
        *r = 120; *g = 30; *b = 180;
    } else if (strcmp(class_name, "SPRT") == 0) {
        /* Sport: bright orange */
        *r = 230; *g = 120; *b = 20;
    } else if (strcmp(class_name, "SUPR") == 0) {
        /* Super: pearl white */
        *r = 230; *g = 235; *b = 240;
    }

    /* Per-car variation: shift hue slightly based on car number */
    int shift = (car_num * 17) % 40 - 20; /* -20 to +19 */
    int ri = (int)*r + shift;
    int gi = (int)*g + (shift / 2);
    int bi = (int)*b - shift;
    if (ri < 10) ri = 10; if (ri > 255) ri = 255;
    if (gi < 10) gi = 10; if (gi > 255) gi = 255;
    if (bi < 10) bi = 10; if (bi > 255) bi = 255;
    *r = (uint8_t)ri; *g = (uint8_t)gi; *b = (uint8_t)bi;
}

/* 3D model viewer */
static BGV_Model g_car_model;
static int g_car_model_loaded = 0;
static IDirect3DVertexBuffer8 *g_car_vb = NULL;
static IDirect3DIndexBuffer8 *g_car_ib = NULL;
static int g_show_3d_model = 0;  /* toggled by M key */
static float g_model_rot_y = 0.0f;  /* auto-rotation / orbit yaw angle */
static float g_model_cam_pitch = 0.3f;  /* camera pitch (radians, 0=level, >0=above) */
static float g_model_cam_dist = 2.5f;   /* camera distance multiplier (of bounding radius) */
static int g_model_auto_rotate = 1;     /* auto-rotate when no input */

/* Vehicle catalog for model cycling (N/P keys) */
typedef struct {
    char class_name[8];   /* e.g. "COMP" */
    int car_number;       /* e.g. 1 for Car1.bgv */
} VehicleEntry;

#define MAX_VEHICLES 128
static VehicleEntry g_vehicle_list[MAX_VEHICLES];
static int g_vehicle_count = 0;
static int g_vehicle_index = 0;  /* current index into g_vehicle_list */

static void build_vehicle_catalog(void)
{
    static const char *classes[] = {
        "COMP", "CUPE", "HEVY", "HSPC", "MSCL", "SPRT", "SUPR", NULL
    };
    g_vehicle_count = 0;
    int ci;
    for (ci = 0; classes[ci] && g_vehicle_count < MAX_VEHICLES; ci++) {
        int car;
        for (car = 1; car <= 36 && g_vehicle_count < MAX_VEHICLES; car++) {
            char path[256];
            snprintf(path, sizeof(path),
                     "Burnout 3 Takedown\\pveh\\%s\\Car%d.bgv",
                     classes[ci], car);
            FILE *test = fopen(path, "rb");
            if (test) {
                fclose(test);
                strncpy(g_vehicle_list[g_vehicle_count].class_name,
                        classes[ci], 7);
                g_vehicle_list[g_vehicle_count].class_name[7] = '\0';
                g_vehicle_list[g_vehicle_count].car_number = car;
                g_vehicle_count++;
            }
        }
    }
    fprintf(stderr, "  Vehicle catalog: %d models found\n", g_vehicle_count);
}

/* Track catalog for track cycling (T key in 3D mode) */
#define MAX_TRACKS 64
static char g_track_paths[MAX_TRACKS][128];
static int g_track_count = 0;
static int g_track_index = 0;

static void build_track_catalog(void)
{
    static const char *regions[] = { "AS", "EU", "US", NULL };
    static const char *codes[] = {
        "C1_V1", "C1_V2", "C2_V1", "C2_V2", "C3_V1", "C3_V2",
        "C4_V1", "C4_V2", "C5_V1", "C5_V2",
        "M1_V1", "M1_V2", "M2_V1", "M2_V2",
        "P1_V1", "P1_V2", "P2_V1", "P2_V2", NULL
    };
    g_track_count = 0;
    for (int r = 0; regions[r] && g_track_count < MAX_TRACKS; r++) {
        for (int c = 0; codes[c] && g_track_count < MAX_TRACKS; c++) {
            char path[128];
            snprintf(path, sizeof(path),
                     "Burnout 3 Takedown\\Tracks\\%s\\%s\\streamed.dat",
                     regions[r], codes[c]);
            FILE *test = fopen(path, "rb");
            if (test) {
                fclose(test);
                strncpy(g_track_paths[g_track_count], path, 127);
                g_track_count++;
            }
        }
    }
    fprintf(stderr, "  Track catalog: %d tracks found\n", g_track_count);
}

static void load_track_by_index(int index)
{
    if (index < 0 || index >= g_track_count) return;
    fprintf(stderr, "  [TRACK] Loading track %d/%d: %s\n",
            index + 1, g_track_count, g_track_paths[index]);
    if (rw_load_track(g_track_paths[index]) == 0) {
        fprintf(stderr, "  [TRACK] Track loaded OK\n");
        /* Update window title with track info */
        if (g_hwnd) {
            /* Extract region/code from path like "...\EU\C1_V1\..." */
            const char *p = g_track_paths[index];
            const char *region = NULL, *code = NULL;
            for (const char *s = p; *s; s++) {
                if ((*s == '\\' || *s == '/') && s[1] && s[2] && (s[3] == '\\' || s[3] == '/')) {
                    region = s + 1;
                }
            }
            if (region) {
                code = region + 3; /* skip "XX\" */
            }
            /* Track info logged to stderr, title bar stays clean */
            if (region && code) {
                char reg[4] = {0}, cod[8] = {0};
                memcpy(reg, region, 2);
                for (int i = 0; i < 7 && code[i] && code[i] != '\\' && code[i] != '/'; i++)
                    cod[i] = code[i];
                fprintf(stderr, "  [TRACK] Loaded track %d/%d: %s %s\n",
                        index + 1, g_track_count, reg, cod);
            }
        }
    } else {
        fprintf(stderr, "  [TRACK] Failed to load track\n");
    }
}

static void load_vehicle_model(int index)
{
    if (index < 0 || index >= g_vehicle_count) return;

    /* Release old model resources */
    if (g_car_ib) { g_car_ib->lpVtbl->Release(g_car_ib); g_car_ib = NULL; }
    if (g_car_vb) { g_car_vb->lpVtbl->Release(g_car_vb); g_car_vb = NULL; }
    if (g_paint_tex) { g_paint_tex->lpVtbl->Release(g_paint_tex); g_paint_tex = NULL; }
    bgv_free(&g_car_model);
    g_car_model_loaded = 0;

    /* Build path and load */
    char path[256];
    snprintf(path, sizeof(path),
             "Burnout 3 Takedown\\pveh\\%s\\Car%d.bgv",
             g_vehicle_list[index].class_name,
             g_vehicle_list[index].car_number);

    fprintf(stderr, "  [MODEL] Loading %s/%s Car%d...\n",
            g_vehicle_list[index].class_name,
            g_vehicle_list[index].class_name,
            g_vehicle_list[index].car_number);

    if (bgv_load(path, &g_car_model) != 0) {
        fprintf(stderr, "  [MODEL] Failed to load %s\n", path);
        return;
    }
    g_car_model_loaded = 1;
    g_vehicle_index = index;

    /* Create D3D8 vertex buffer */
    DWORD fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    UINT vb_size = g_car_model.vertex_count * sizeof(BGV_Vertex);
    HRESULT hr = g_d3d_device->lpVtbl->CreateVertexBuffer(
        g_d3d_device, vb_size, 0, fvf, D3DPOOL_MANAGED, &g_car_vb);
    if (SUCCEEDED(hr)) {
        BYTE *vb_data = NULL;
        hr = g_car_vb->lpVtbl->Lock(g_car_vb, 0, vb_size, &vb_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(vb_data, g_car_model.vertices, vb_size);
            g_car_vb->lpVtbl->Unlock(g_car_vb);
        }
    }

    /* Create D3D8 index buffer */
    UINT ib_size = g_car_model.index_count * sizeof(uint16_t);
    hr = g_d3d_device->lpVtbl->CreateIndexBuffer(
        g_d3d_device, ib_size, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_car_ib);
    if (SUCCEEDED(hr)) {
        BYTE *ib_data = NULL;
        hr = g_car_ib->lpVtbl->Lock(g_car_ib, 0, ib_size, &ib_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(ib_data, g_car_model.indices, ib_size);
            g_car_ib->lpVtbl->Unlock(g_car_ib);
        }
    }

    /* Create paint texture for this vehicle */
    {
        uint8_t pr, pg, pb;
        get_class_paint_color(g_vehicle_list[index].class_name,
                              g_vehicle_list[index].car_number, &pr, &pg, &pb);
        g_paint_tex = create_paint_texture(g_d3d_device, pr, pg, pb);
        fprintf(stderr, "  [MODEL] Loaded %s Car%d: %u verts, %u tris, paint RGB(%u,%u,%u)\n",
                g_vehicle_list[index].class_name,
                g_vehicle_list[index].car_number,
                g_car_model.vertex_count,
                g_car_model.index_count / 3,
                (unsigned)pr, (unsigned)pg, (unsigned)pb);
    }
}

/* Traffic car model pool: different models for visual variety */
#define TRAFFIC_MODEL_COUNT 6
static BGV_Model g_traffic_models[TRAFFIC_MODEL_COUNT];
static int g_traffic_models_loaded = 0;

static void load_traffic_models(void)
{
    /* Pick models spread across different classes for variety */
    static const struct { const char *cls; int car; } traffic_picks[TRAFFIC_MODEL_COUNT] = {
        {"COMP", 2}, {"SPRT", 3}, {"MSCL", 1},
        {"CUPE", 5}, {"SUPR", 4}, {"HEVY", 1},
    };
    int i;
    g_traffic_models_loaded = 0;
    for (i = 0; i < TRAFFIC_MODEL_COUNT; i++) {
        char path[256];
        snprintf(path, sizeof(path), "Burnout 3 Takedown\\pveh\\%s\\Car%d.bgv",
                 traffic_picks[i].cls, traffic_picks[i].car);
        if (bgv_load_lod(path, &g_traffic_models[i], 1) == 0) {
            g_traffic_models_loaded++;
        } else {
            memset(&g_traffic_models[i], 0, sizeof(BGV_Model));
        }
    }
    /* Apply distinct paint colors to each traffic model */
    {
        static const struct { uint8_t r, g, b; } tints[TRAFFIC_MODEL_COUNT] = {
            {255, 80, 60},   /* red */
            {60, 130, 255},  /* blue */
            {255, 220, 50},  /* yellow */
            {40, 200, 80},   /* green */
            {200, 200, 210}, /* silver */
            {60, 60, 70},    /* dark grey */
        };
        for (i = 0; i < TRAFFIC_MODEL_COUNT; i++) {
            if (g_traffic_models[i].vertices)
                bgv_tint(&g_traffic_models[i], tints[i].r, tints[i].g, tints[i].b);
        }
    }
    fprintf(stderr, "  Traffic models: %d/%d loaded\n",
            g_traffic_models_loaded, TRAFFIC_MODEL_COUNT);
}

static void free_traffic_models(void)
{
    int i;
    for (i = 0; i < TRAFFIC_MODEL_COUNT; i++)
        bgv_free(&g_traffic_models[i]);
    g_traffic_models_loaded = 0;
}

/* ── XBE loading ────────────────────────────────────────────── */

/**
 * Load the original XBE file into memory.
 * The XBE data is needed to initialize the memory layout
 * (copy .rdata and .data sections to their expected addresses).
 */
static BOOL load_xbe(const char *path)
{
    HANDLE hFile;
    DWORD fileSize, bytesRead;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open XBE: %s (error %lu)\n",
                path, GetLastError());
        return FALSE;
    }

    fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        fprintf(stderr, "ERROR: Invalid XBE file size\n");
        CloseHandle(hFile);
        return FALSE;
    }

    g_xbe_data = VirtualAlloc(NULL, fileSize, MEM_COMMIT, PAGE_READWRITE);
    if (!g_xbe_data) {
        fprintf(stderr, "ERROR: Cannot allocate %lu bytes for XBE\n", fileSize);
        CloseHandle(hFile);
        return FALSE;
    }

    if (!ReadFile(hFile, g_xbe_data, fileSize, &bytesRead, NULL) ||
        bytesRead != fileSize) {
        fprintf(stderr, "ERROR: Failed to read XBE (%lu of %lu bytes)\n",
                bytesRead, fileSize);
        VirtualFree(g_xbe_data, 0, MEM_RELEASE);
        g_xbe_data = NULL;
        CloseHandle(hFile);
        return FALSE;
    }

    g_xbe_size = fileSize;
    CloseHandle(hFile);
    fprintf(stderr, "Loaded XBE: %s (%lu bytes)\n", path, fileSize);
    return TRUE;
}

/* ── Window management ──────────────────────────────────────── */

#if defined(_WIN32)
/* Win32 window / menu / message handling. The Linux build uses the SDL2
 * window the d3d8_gl backend creates in CreateDevice; create_window
 * below is a stub there. */
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    /* Let ImGui handle input when menus are open */
    if (menu_gui_wndproc(hwnd, msg, (unsigned long long)wParam, (long long)lParam))
        return 0;

    switch (msg) {
    case WM_CLOSE:
        g_running = FALSE;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_F1) { menu_gui_toggle_settings(); return 0; }
        if (wParam == VK_F2) { menu_gui_toggle_debug(); return 0; }
        if (wParam == VK_F12) { menu_gui_take_screenshot(); return 0; }
        if (wParam == VK_ESCAPE) {
            if (!menu_gui_is_active()) {
                g_running = FALSE;
                PostQuitMessage(0);
            }
            /* When menu is active, ESC is handled by ImGui (closes window) */
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_QUIT:
            g_running = FALSE;
            PostQuitMessage(0);
            return 0;
        case IDM_CONFIG_SETTINGS:
            menu_gui_toggle_settings();
            return 0;
        case IDM_CONFIG_DEBUG:
            menu_gui_toggle_debug();
            return 0;
        case IDM_HELP_ABOUT:
            menu_gui_toggle_settings();
            menu_gui_show_about();
            return 0;
        }
        break;

    case WM_SIZE:
        /* TODO: Notify D3D layer of resize */
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static HMENU create_menu_bar(void)
{
    HMENU menubar = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU config_menu = CreatePopupMenu();
    HMENU help_menu = CreatePopupMenu();

    AppendMenuA(file_menu, MF_STRING, IDM_FILE_QUIT, "Quit\tESC");

    AppendMenuA(config_menu, MF_STRING, IDM_CONFIG_SETTINGS, "Settings...\tF1");
    AppendMenuA(config_menu, MF_STRING, IDM_CONFIG_DEBUG, "Debug...\tF2");

    AppendMenuA(help_menu, MF_STRING, IDM_HELP_ABOUT, "About...");

    AppendMenuA(menubar, MF_POPUP, (UINT_PTR)file_menu, "File");
    AppendMenuA(menubar, MF_POPUP, (UINT_PTR)config_menu, "Config");
    AppendMenuA(menubar, MF_POPUP, (UINT_PTR)help_menu, "About");

    return menubar;
}

static HWND create_window(HINSTANCE hInstance, int width, int height)
{
    WNDCLASSEXA wc = {0};
    RECT rect;
    HWND hwnd;
    HMENU menubar;

    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExA(&wc);

    menubar = create_menu_bar();

    /* Adjust window size for client area (TRUE = has menu) */
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, TRUE);

    hwnd = CreateWindowExA(
        0, WINDOW_CLASS, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, menubar, hInstance, NULL
    );

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    return hwnd;
}

/* ── Subsystem initialization ───────────────────────────────── */

#else /* !_WIN32 -- the d3d8_gl backend owns the SDL2 window. */

static HWND create_window(HINSTANCE hInstance, int width, int height)
{
    (void)hInstance; (void)width; (void)height;
    /* Return a non-NULL sentinel so the caller's NULL check passes.
     * The real SDL window appears when CreateDevice runs later. */
    return (HWND)(uintptr_t)1;
}

#endif /* _WIN32 */

static BOOL init_subsystems(void)
{
    fprintf(stderr, "\n=== Initializing subsystems ===\n");

    /* 1. Xbox memory layout (maps .rdata/.data to original VAs) */
    fprintf(stderr, "[1/4] Memory layout...\n");
    if (!xbox_MemoryLayoutInit(g_xbe_data, g_xbe_size)) {
        fprintf(stderr, "FATAL: Memory layout initialization failed\n");
        fprintf(stderr, "  The address range 0x00010000-0x00770000 must be available.\n");
        fprintf(stderr, "  Try disabling ASLR or running with a fixed base address.\n");
        return FALSE;
    }

    /* Initialize RW state with valid defaults (must be after memory layout) */
    {
        extern void rw_state_init(void);
        rw_state_init();
    }

    /* Verify .text section data integrity */
    fprintf(stderr, "  .text verify: JT[0x16CC8]=0x%08X (expect 0x000166D1)\n", MEM32(0x16CC8));

    /* Dump frontend vtable at 0x3A9E7C (before game code can corrupt it) */
    fprintf(stderr, "  [VTABLE-EARLY] 0x3A9E7C (frontend object vtable):\n");
    fprintf(stderr, "    [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X\n",
            MEM32(0x3A9E7C), MEM32(0x3A9E80), MEM32(0x3A9E84), MEM32(0x3A9E88));
    fprintf(stderr, "    [4]=0x%08X [5]=0x%08X [6]=0x%08X [7]=0x%08X\n",
            MEM32(0x3A9E8C), MEM32(0x3A9E90), MEM32(0x3A9E94), MEM32(0x3A9E98));

    /* 2. Xbox kernel replacement layer */
    fprintf(stderr, "[2/4] Kernel layer...\n");
    fflush(stderr);
    xbox_kernel_init();          /* Fill thunk table with our Win32 implementations */
    fprintf(stderr, "  xbox_kernel_init() done\n");
    fflush(stderr);
    xbox_kernel_bridge_init();   /* Patch Xbox memory thunk entries with synthetic VAs */
    fprintf(stderr, "  xbox_kernel_bridge_init() done\n");
    fflush(stderr);

    /* 2b. Pre-initialize CRT bootstrap locks.
     *
     * The Xbox CRT uses a lock table at 0x3C6500 with 36 entries (8 bytes
     * each: [pointer(4), flag(4)]). Bootstrap locks have flag=1 and must be
     * initialized before any code calls __lock(). Normally _mtinitlocks()
     * (sub_0024858A) does this during CRT startup, but we bypass the CRT
     * entry point.
     *
     * Without this, __lock(8) → _mtinitlocknum(8) → __lock(10) →
     * _mtinitlocknum(10) → __lock(10) → infinite recursion → stack overflow.
     *
     * Since all CS operations are no-ops (single-threaded execution), the
     * pointers just need to be non-zero. We use the pre-allocated CS buffer
     * array at 0x41D310 (in BSS), same as the original _mtinitlocks. */
    {
        uint32_t cs_addr = 0x41D310;  /* Pre-allocated CS buffer array */
        int locks_initialized = 0;
        int i;
        for (i = 0; i < 36; i++) {
            uint32_t flag_va = 0x3C6504 + i * 8;
            uint32_t ptr_va  = 0x3C6500 + i * 8;
            if (MEM32(flag_va) == 1) {
                MEM32(ptr_va) = cs_addr;
                cs_addr += 0x1C;  /* Each CS struct is 0x1C bytes */
                locks_initialized++;
            }
        }
        fprintf(stderr, "  CRT locks: %d bootstrap locks pre-initialized\n",
                locks_initialized);
    }

    /* 2c. Pre-initialize CRT atexit callback table.
     *
     * The CRT atexit/onexit registration function (sub_0024326D) stores
     * callback function pointers in a dynamically-allocated table whose
     * base and current pointers live at Xbox VA 0x76B92C and 0x76B928.
     * These are BSS (zero-initialized), but the code doesn't handle null:
     * sub_00246E8B → sub_001D4D65 reads RW heap block metadata at negative
     * offsets from the table pointer, crashing on MEM8(0 - 11).
     *
     * Fix: allocate a zeroed buffer with 32 bytes of padding (for the
     * negative-offset metadata reads). The zeroed metadata makes
     * sub_001D4D65 return -1 (huge capacity), so sub_0024326D always
     * finds room to store entries without needing to query block size. */
    {
        uint32_t atexit_buf = xbox_HeapAlloc(1024 + 32, 4);
        if (atexit_buf) {
            uint32_t table_base = atexit_buf + 32;
            MEM32(0x76B92C) = table_base;  /* base pointer */
            MEM32(0x76B928) = table_base;  /* current = base (empty table) */
            fprintf(stderr, "  CRT atexit: table at 0x%08X (256 entries)\n", table_base);
        } else {
            fprintf(stderr, "  WARNING: could not allocate atexit table\n");
        }
    }

    /* 2d. Pre-initialize D3D8 push buffer.
     *
     * The Xbox D3D8 library (statically linked into the game) uses a command
     * buffer ("push buffer") to batch GPU commands. Render state setters like
     * sub_00355360 write pairs of uint32 values into this buffer. When full,
     * sub_00351A20 (flush) submits it to the NV2A GPU and resets the pointer.
     *
     * On our recompilation, there's no real GPU. But the code still runs and
     * tries to write to the buffer. Without initialization, the buffer
     * pointers at 0x35D6A0/0x35D6A4 are zero, causing sub_00355360 to enter
     * an infinite flush-retry loop that corrupts the simulated stack.
     *
     * Fix: allocate a buffer so writes succeed (silently discarded).
     * sub_00351A20 (manual override in recomp_manual.c) resets the write
     * pointer when the buffer fills.
     */
    {
        uint32_t cmd_buf_size = 4 * 1024 * 1024;  /* 4 MB */
        uint32_t cmd_buf = xbox_HeapAlloc(cmd_buf_size, 4096);
        if (cmd_buf) {
            MEM32(0x35D69C) = cmd_buf;                  /* base (for flush reset) */
            MEM32(0x35D6A0) = cmd_buf;                  /* write pointer */
            MEM32(0x35D6A4) = cmd_buf + cmd_buf_size;   /* end pointer */
            MEM32(0x3609FC) = cmd_buf_size / 2;          /* size in 16-bit words */
            fprintf(stderr, "  D3D8 push buffer: %u KB at Xbox VA 0x%08X-0x%08X\n",
                    cmd_buf_size / 1024, cmd_buf, cmd_buf + cmd_buf_size);
        }
    }

    /* 2e. D3D8 device context initialization from xemu snapshot.
     *
     * The Xbox D3D8 device context is a STATIC structure at 0x0035D6A0
     * within the D3D8LTCG library section (discovered via xemu capture).
     * It's NOT heap-allocated. The XBE loads uninitialized data there;
     * we overlay it with a runtime snapshot captured from xemu during
     * menu rendering to give the D3D8LTCG gen code proper state.
     *
     * Pointer fixups needed:
     *   - +0x00/+0x04/+0x08: PB GPU addresses → our PB virtual addresses
     *   - +0x784/+0x794/+0x7A8: Surface ptrs → our fake surfaces
     *   - +0x1A04/+0x1A08: These are at 0x35F0C4/0x35F10C (D3D section) = OK
     */
    {
        #include "../../src/nv2a/d3d_device_snapshot.h"

        uint32_t dev = 0x0035D6A0;  /* Original Xbox address (static, in D3D section) */
        uint32_t pb_start = MEM32(0x35D69C);  /* Our push buffer base */
        uint32_t pb_end   = MEM32(0x35D6A4);  /* Our push buffer end */

        /* Copy xemu snapshot over the static data */
        memcpy((void*)XBOX_PTR(dev), d3d_device_snapshot, D3D_DEVICE_SNAPSHOT_SIZE);
        fprintf(stderr, "  D3D8 device: loaded %u-byte xemu snapshot at 0x%08X\n",
                D3D_DEVICE_SNAPSHOT_SIZE, dev);

        /* Point the global to the original address */
        MEM32(0x35FB48) = dev;

        /* Fix up push buffer pointers (xemu had GPU physical addresses) */
        MEM32(dev + 0x00) = pb_start;   /* PB write cursor */
        MEM32(dev + 0x04) = pb_end;     /* PB end */
        MEM32(dev + 0x08) = pb_start;   /* PB base */
        MEM32(dev + 0x0C) = pb_end - pb_start;  /* PB size */

        /* Fix up surface pointers — allocate fake surfaces like before */
        {
            uint32_t fake_surf = xbox_HeapAlloc(0x1000, 16);
            if (fake_surf) {
                /* Render target surface */
                MEM32(fake_surf + 0x10) = 640;
                MEM32(fake_surf + 0x14) = 480;
                MEM32(fake_surf + 0x0C) = 0x00060006;
                MEM32(dev + 0x784) = fake_surf;

                /* Depth/stencil surface */
                MEM32(fake_surf + 0x100 + 0x10) = 640;
                MEM32(fake_surf + 0x100 + 0x14) = 480;
                MEM32(fake_surf + 0x100 + 0x0C) = 0x00020002;
                MEM32(dev + 0x794) = fake_surf + 0x100;

                /* Back buffer surface */
                MEM32(fake_surf + 0x200 + 0x10) = 640;
                MEM32(fake_surf + 0x200 + 0x14) = 480;
                MEM32(fake_surf + 0x200 + 0x0C) = 0x00060006;
                MEM32(dev + 0x7A8) = fake_surf + 0x200;

                /* Surface state fields — USE snapshot's D3D-section pointers
                 * 0x35F0C4 and 0x35F10C are in the D3D section (valid Xbox VAs).
                 * The snapshot already loaded proper surface data there.
                 * Initialize surface dimensions at those addresses too. */
                {
                    uint32_t snap_rt = 0x0035F0C4;  /* from xemu snapshot */
                    uint32_t snap_ds = 0x0035F10C;  /* from xemu snapshot */
                    /* Ensure surface dimensions are set */
                    MEM32(snap_rt + 0x10) = 640;
                    MEM32(snap_rt + 0x14) = 480;
                    MEM32(snap_ds + 0x10) = 640;
                    MEM32(snap_ds + 0x14) = 480;
                    /* Point device to these D3D-section surfaces */
                    MEM32(dev + 0x1A04) = snap_rt;
                    MEM32(dev + 0x1A08) = snap_ds;
                    MEM32(dev + 0x1A14) = snap_rt;
                    MEM32(dev + 0x1A18) = fake_surf;  /* alternate = fake (safety) */
                }
            }
        }

        /* Override viewport dimensions (snapshot may have wrong values) */
        MEM32(dev + 0x954) = 640;
        MEM32(dev + 0x958) = 480;
        MEM32(dev + 0xEE0) = 640;
        MEM32(dev + 0xEE4) = 480;
        MEM32(dev + 0xEE8) = 640;
        MEM32(dev + 0xEEC) = 480;
        MEM32(dev + 0xEF0) = 640;
        MEM32(dev + 0xEF4) = 480;

        /* Render state flags */
        MEM32(dev + 0x7CC) = 0;
        MEM32(dev + 0x1AD4) = 0;

        /* ── PB ring management fixups (post-snapshot) ──
         * The xemu snapshot has xemu heap pointers (0x82xxxxxx) in the PB
         * ring management fields. These must point to our PB allocation.
         *
         * Critical: device+0x30 is a POINTER TO a GPU read position.
         * D3D8LTCG code checks: if (write - requested >= write - *dev+0x30)
         * skip rendering (not enough PB space). In unsigned ring semantics,
         * we need gpu_read > write so the subtraction wraps to a large
         * "available space" value.
         *
         * Previous approach (dev+0x30 → dev+0x2C): made gpu_read==write,
         * eliminating spin-loops but also setting available_space=0.
         * This caused sub_00351770_gen to ALWAYS skip scene rendering.
         *
         * Fix: store pb_end in a dedicated field and point +0x30 there.
         * With gpu_read=pb_end and write=pb_start, the unsigned check
         * (write-1) < (write-pb_end) passes, allowing PB allocation. */
        /* ── PB ring management ──
         * D3D8LTCG gen code uses multiple fields for PB ring management:
         * +0x00: Current PB write cursor (gen code writes commands here)
         * +0x04: PB segment limit (write must stay below this)
         * +0x08: Secondary write cursor (alternate context)
         * +0x24: PB ring base address
         * +0x28: PB ring end address
         * +0x2C: PB write sequence counter
         * +0x30: POINTER to GPU read position
         * +0x44: PB ring total size
         *
         * The gen code checks (device+0 < device+4) before each write.
         * If false, calls sub_003518E0 to wrap/reallocate PB segment.
         * Sub_00351770_gen checks (write-requested >= write-gpu_read)
         * to verify enough ring space; with gpu_read=pb_end and write=pb_start,
         * unsigned subtraction gives huge available space. */
        MEM32(dev + 0x00) = pb_start;       /* PB write cursor = start */
        MEM32(dev + 0x04) = pb_end;         /* PB segment limit = end */
        MEM32(dev + 0x08) = pb_start;       /* Secondary write cursor */
        MEM32(dev + 0x24) = pb_start;       /* PB ring base */
        MEM32(dev + 0x28) = pb_end;         /* PB ring end */
        MEM32(dev + 0x2C) = pb_start;       /* PB write sequence position */
        /* dev+0x3004: fake GPU read position (safe area in device context).
         * Set to pb_end so the ring appears fully free to the gen code.
         * Point dev+0x30 to this address. */
        MEM32(dev + 0x3004) = pb_end;
        MEM32(dev + 0x30) = dev + 0x3004;   /* GPU read ptr → always at PB end */
        MEM32(dev + 0x34) = 0;              /* fence write index */
        MEM32(dev + 0x38) = 3;              /* fence mask (4 entries) */
        MEM32(dev + 0x3C) = 0;              /* fence state */
        MEM32(dev + 0x44) = pb_end - pb_start; /* PB ring size */
        MEM32(dev + 0x48) = dev + 0x3000;   /* fence array → safe area in device */
        MEM32(dev + 0x19FC) = 0;            /* pre-render callback = NULL */
        MEM32(dev + 0x1DD0) = 0;            /* clear flag */

        /* Double-buffered render targets (must be non-NULL for scene render) */
        MEM32(dev + 0x1974) = 0x3A1F;       /* RT surface 0 (from xemu) */
        MEM32(dev + 0x1978) = 0x3A25;       /* RT surface 1 (from xemu) */

        fprintf(stderr, "  D3D8 PB ring: pb_start=0x%08X pb_end=0x%08X "
                "dev+0x24=0x%08X dev+0x28=0x%08X dev+0x2C=0x%08X dev+0x30=0x%08X dev+0x44=0x%08X\n",
                pb_start, pb_end, MEM32(dev + 0x24), MEM32(dev + 0x28),
                MEM32(dev + 0x2C), MEM32(dev + 0x30), MEM32(dev + 0x44));
        fprintf(stderr, "  D3D8 PB ring: GPU_read→dev+0x2C, fence→dev+0x3000, "
                "RT=0x3A1F/0x3A25\n");

        fprintf(stderr, "  D3D8 device context: at Xbox VA 0x%08X (PB 0x%08X-0x%08X)\n",
                dev, pb_start, pb_end);

        /* Load render context snapshot from xemu.
         * 0x4D6770 is the render input struct passed to sub_0034D530 from sub_001AE6F0.
         * Contains viewport dimensions, view/projection matrices, and surface references
         * that the D3D8LTCG gen code needs to produce push buffer draw commands. */
        {
            #include "../../src/nv2a/render_input_snapshot.h"
            memcpy((void*)XBOX_PTR(RENDER_INPUT_ADDR), render_input_snapshot, RENDER_INPUT_SIZE);
            fprintf(stderr, "  Render context: loaded %u bytes at 0x%08X\n",
                    RENDER_INPUT_SIZE, RENDER_INPUT_ADDR);
        }
    }

    /* 3. Graphics (D3D8→D3D11) */
    fprintf(stderr, "[3/4] Graphics (D3D8→D3D11)...\n");
    {
        D3DPRESENT_PARAMETERS pp;
        HRESULT hr;

        g_d3d8 = xbox_Direct3DCreate8(0);
        if (!g_d3d8) {
            fprintf(stderr, "FATAL: Direct3DCreate8 failed\n");
            return FALSE;
        }

        memset(&pp, 0, sizeof(pp));
        pp.BackBufferWidth = DEFAULT_WIDTH;
        pp.BackBufferHeight = DEFAULT_HEIGHT;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = g_hwnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;

        hr = g_d3d8->lpVtbl->CreateDevice(g_d3d8, 0, 0, g_hwnd, 0, &pp, &g_d3d_device);
        if (FAILED(hr)) {
            fprintf(stderr, "FATAL: CreateDevice failed: 0x%08lX\n", hr);
            return FALSE;
        }
    }

    /* 4. Load game textures */
    fprintf(stderr, "[4/5] Loading game textures...\n");
    {
        int n = txd_load("Burnout 3 Takedown\\Data\\Global.txd", g_d3d_device, &g_global_txd);
        if (n > 0) {
            g_textures_loaded = 1;
            fprintf(stderr, "  Loaded %d textures from Global.txd\n", n);
            /* Dump FE_ (frontend) texture names for menu rendering */
            for (int ti = 0; ti < g_global_txd.count; ti++) {
                if (strncmp(g_global_txd.entries[ti].name, "FE_", 3) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "fe_", 3) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "Font", 4) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "font", 4) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "Title", 5) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "Logo", 4) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "logo", 4) == 0 ||
                    strncmp(g_global_txd.entries[ti].name, "Burnout", 7) == 0) {
                    fprintf(stderr, "  [FE-TEX] [%d] '%s' %ux%u fmt=0x%02X tex=%p\n",
                            ti, g_global_txd.entries[ti].name,
                            g_global_txd.entries[ti].width, g_global_txd.entries[ti].height,
                            g_global_txd.entries[ti].format, g_global_txd.entries[ti].texture);
                }
            }
            /* Full texture list dump (first 20 + last 10) */
            fprintf(stderr, "  [TXD-DUMP] All %d textures:\n", g_global_txd.count);
            for (int ti = 0; ti < g_global_txd.count; ti++) {
                fprintf(stderr, "    [%3d] %-24s %4ux%-4u fmt=0x%02X\n",
                        ti, g_global_txd.entries[ti].name,
                        g_global_txd.entries[ti].width, g_global_txd.entries[ti].height,
                        g_global_txd.entries[ti].format);
            }
        } else {
            fprintf(stderr, "  WARNING: No textures loaded from Global.txd\n");
        }
    }

    /* 4b. Build vehicle catalog and load first car model */
    fprintf(stderr, "[4b] Loading car models...\n");
    build_vehicle_catalog();
    if (g_vehicle_count > 0) {
        load_vehicle_model(0);
    } else {
        fprintf(stderr, "  WARNING: No vehicle models found\n");
    }

    /* 4c. Load traffic car models for variety */
    load_traffic_models();

    /* 4d. Initialize 3D renderer and register models */
    rw_renderer_init();
    if (g_car_model_loaded) {
        rw_gameplay_register_models(&g_car_model,
                                     g_traffic_models, TRAFFIC_MODEL_COUNT);
    }

    /* 4e. Build track catalog and load first track */
    build_track_catalog();
    {
        if (g_track_count > 0) {
            load_track_by_index(0);
        } else {
            fprintf(stderr, "  Track geometry not available (proceeding with procedural road)\n");
        }
    }

    /* 4f. Create procedural road texture */
    g_road_tex = create_road_texture(g_d3d_device);
    if (g_road_tex) {
        fprintf(stderr, "  Created procedural road texture (128x128)\n");
    }

    /* 5. Audio + Input */
    fprintf(stderr, "[5/5] Audio + Input...\n");
    xbox_DirectSoundCreate(NULL, &g_dsound, NULL);
    xbox_InputInit();

    /* 6. Video player (for boot sequence) */
    fprintf(stderr, "[6/6] Video player...\n");
    if (video_init() == 0) {
        fprintf(stderr, "  Video player initialized (Media Foundation)\n");
    } else {
        fprintf(stderr, "  WARNING: Video player failed to init (boot videos will be skipped)\n");
    }

    /* 7. ImGui menu system */
    fprintf(stderr, "[7/7] Menu system...\n");
    if (menu_gui_init() == 0) {
        fprintf(stderr, "  Menu system initialized (ImGui)\n");
    } else {
        fprintf(stderr, "  WARNING: Menu system failed to init\n");
    }

    /* 8. RW display driver function pointer table */
    {
        extern void rw_init_display_driver_table(void);
        rw_init_display_driver_table();
    }

    /* 9. NV2A GPU emulation (xemu-based register handlers) */
    {
        extern void nv2a_hook_init(ptrdiff_t xbox_mem_offset);
        nv2a_hook_init(g_xbox_mem_offset);
    }

    /* 10. MCPX APU audio emulation (xemu-based register handlers) */
    {
        typedef struct MCPXAPUState MCPXAPUState;
        extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
        extern MCPXAPUState *g_apu_state;
        /* The APU reads voice data from physical RAM (0x00000000-0x03FFFFFF).
         * Our Xbox RAM is mapped at g_xbox_mem_offset, so the base of physical
         * RAM is at native address (g_xbox_mem_offset + 0). */
        uint8_t *phys_ram = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
        g_apu_state = mcpx_apu_init_standalone(phys_ram);
        if (g_apu_state) {
            fprintf(stderr, "[APU] Press U to toggle 440Hz test tone\n");
        }
    }

    /* [6/6] Load game audio */
    fprintf(stderr, "[6/6] Loading game audio...\n");
    g_awd_fe = awd_load("Burnout 3 Takedown\\sound\\Fe.awd");
    g_awd_generic = awd_load("Burnout 3 Takedown\\sound\\Generic.awd");

    /* Startup chime disabled — sounds now driven by fe_menu.c */

    fprintf(stderr, "=== All subsystems initialized ===\n\n");
    return TRUE;
}

static void shutdown_subsystems(void)
{
    fprintf(stderr, "\n=== Shutting down ===\n");

    /* Reverse order of initialization */
    menu_gui_shutdown();
    video_shutdown();
    rw_renderer_shutdown();
    if (g_paint_tex) { g_paint_tex->lpVtbl->Release(g_paint_tex); g_paint_tex = NULL; }
    if (g_road_tex) { g_road_tex->lpVtbl->Release(g_road_tex); g_road_tex = NULL; }
    if (g_car_ib) { g_car_ib->lpVtbl->Release(g_car_ib); g_car_ib = NULL; }
    if (g_car_vb) { g_car_vb->lpVtbl->Release(g_car_vb); g_car_vb = NULL; }
    bgv_free(&g_car_model);
    g_car_model_loaded = 0;
    free_traffic_models();

    txd_release(&g_global_txd);
    g_textures_loaded = 0;

    if (g_dsound) {
        g_dsound->lpVtbl->Release(g_dsound);
        g_dsound = NULL;
    }
    if (g_d3d_device) {
        g_d3d_device->lpVtbl->Release(g_d3d_device);
        g_d3d_device = NULL;
    }
    if (g_d3d8) {
        g_d3d8->lpVtbl->Release(g_d3d8);
        g_d3d8 = NULL;
    }
    xbox_MemoryLayoutShutdown();

    if (g_xbe_data) {
        VirtualFree(g_xbe_data, 0, MEM_RELEASE);
        g_xbe_data = NULL;
    }

    fprintf(stderr, "Shutdown complete.\n");
}

/* ── KeTickCount updater thread ──────────────────────────── */
/* Xbox KeTickCount is a data export at VA 0x00740020 that increments
 * every ~1ms. The game reads it directly from memory for timing.
 * We update it from a background thread using GetTickCount(). */

static DWORD WINAPI tick_count_thread_func(LPVOID param)
{
    (void)param;
    uint32_t tick_va = XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    for (;;) {
        MEM32(tick_va) = GetTickCount();
        Sleep(1);  /* ~1ms Xbox tick interval */
    }
    return 0;
}

/* ── Watchdog thread: periodically dumps register state ──── */

static DWORD WINAPI watchdog_thread_func(LPVOID param)
{
    (void)param;
    uint64_t prev_count = 0;
    for (;;) {
        Sleep(2000);
        uint64_t count = g_icall_count;
        uint32_t idx = g_icall_trace_idx;
        fprintf(stderr, "  [WATCHDOG] ICALLs: %llu total (+%llu/2s) esp=0x%08X\n",
                count, count - prev_count, g_esp);
        /* Dump game state variables */
        {
            extern volatile uint32_t g_d3d_render_count;
            extern volatile uint32_t g_present_count;
            extern volatile uint32_t g_tick_110e0_count;
            fprintf(stderr, "  [WATCHDOG] game_state=0x%08X pending=0x%08X load_state=0x%08X flag=0x%02X d3d=%u present=%u tick=%u\n",
                    MEM32(0x4D53B8), MEM32(0x4D53B4), MEM32(0x4D5388), MEM8(0x4D5378),
                    g_d3d_render_count, g_present_count, g_tick_110e0_count);
        }
        /* Print last ICALL targets */
        /* Print every slot, zeros included: "nothing printed" is ambiguous
         * between an empty ring and a ring full of VA 0, and those mean very
         * different things. */
        fprintf(stderr, "  [WATCHDOG] ICALLs (idx=%u):", idx);
        for (int j = ICALL_TRACE_SIZE - 1; j >= 0; j--) {
            uint32_t va = g_icall_trace[(idx - 1 - j) & (ICALL_TRACE_SIZE - 1)];
            fprintf(stderr, " %08X", va);
        }
        fprintf(stderr, "\n");
        /* If the game thread has stopped executing recompiled code entirely,
         * it is parked in native code and nothing above will say where.
         * Suspend it and read RIP: bin/burnout3.map turns that straight into a
         * function name. (The old Xbox-stack dump here read g_esp, which is
         * thread-local now, so on this thread it was always 0.) */
        /* Always sample the game thread's RIP, whether or not ICALLs moved:
         * plenty of real work (direct calls, tight loops) never touches the
         * counter, so "count unchanged" is not the same as "stopped". */
        {
            HANDLE gt = xbox_thread_debug_handle();
            if (gt) {
                CONTEXT ctx;
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(gt) != (DWORD)-1) {
                    if (GetThreadContext(gt, &ctx))
                        fprintf(stderr, "  [WATCHDOG] game thread RIP=0x%016llX "
                                "RSP=0x%016llX (icalls %s)\n",
                                (unsigned long long)ctx.Rip,
                                (unsigned long long)ctx.Rsp,
                                count == prev_count ? "STALLED" : "moving");
                    ResumeThread(gt);
                }
            }
        }
        fflush(stderr);
        prev_count = count;
    }
    return 0;
}

/* ── Frame pump (called from recompiled game loop) ─────────── */

/* ── Cached texture pointers (resolved once after TXD load) ──── */
#define BOOST_FIRE_FRAMES 30
static IDirect3DTexture8 *g_tex_boostfire[BOOST_FIRE_FRAMES];
static IDirect3DTexture8 *g_tex_spark = NULL;
static IDirect3DTexture8 *g_tex_smoke = NULL;
static IDirect3DTexture8 *g_tex_shadow = NULL;
static IDirect3DTexture8 *g_tex_star = NULL;
static IDirect3DTexture8 *g_tex_healthbar = NULL;
static IDirect3DTexture8 *g_tex_hud = NULL;
static IDirect3DTexture8 *g_tex_corona = NULL;
static IDirect3DTexture8 *g_tex_boostflare = NULL;
static IDirect3DTexture8 *g_tex_explosion = NULL;
static int g_tex_cache_init = 0;

static void txd_cache_init(void)
{
    if (g_tex_cache_init || !g_textures_loaded) return;
    g_tex_cache_init = 1;
    /* Animated boost fire frames (BoostFireCore01..30) */
    {
        int fi;
        for (fi = 0; fi < BOOST_FIRE_FRAMES; fi++) {
            char name[32];
            sprintf(name, "BoostFireCore%02d", fi + 1);
            g_tex_boostfire[fi] = txd_find(&g_global_txd, name);
        }
    }
    g_tex_spark      = txd_find(&g_global_txd, "fxspark");
    g_tex_smoke      = txd_find(&g_global_txd, "fxsmoke");
    g_tex_shadow     = txd_find(&g_global_txd, "blobbyshadow");
    g_tex_star       = txd_find(&g_global_txd, "star");
    g_tex_healthbar  = txd_find(&g_global_txd, "health_bar");
    g_tex_hud        = txd_find(&g_global_txd, "HUD");
    g_tex_corona     = txd_find(&g_global_txd, "coronastar");
    g_tex_boostflare = txd_find(&g_global_txd, "boostflare");
    g_tex_explosion  = txd_find(&g_global_txd, "fxexplosionfire");
    fprintf(stderr, "TXD cache: fire[0]=%p spark=%p smoke=%p shadow=%p star=%p hbar=%p hud=%p corona=%p flare=%p expl=%p\n",
            g_tex_boostfire[0], g_tex_spark, g_tex_smoke, g_tex_shadow,
            g_tex_star, g_tex_healthbar, g_tex_hud, g_tex_corona,
            g_tex_boostflare, g_tex_explosion);
}

/** Draw a textured quad (2 triangles) with alpha blending.
 *  Switches to XYZRHW+DIFFUSE+TEX1 FVF, binds the texture, draws, restores. */
static void draw_textured_quad(IDirect3DDevice8 *dev, IDirect3DTexture8 *tex,
                                float x0, float y0, float x1, float y1,
                                float z, DWORD color)
{
    typedef struct { float x, y, z, rhw; DWORD color; float u, v; } TV;
    TV verts[6] = {
        {x0, y0, z, 1.0f, color, 0.0f, 0.0f},
        {x1, y0, z, 1.0f, color, 1.0f, 0.0f},
        {x0, y1, z, 1.0f, color, 0.0f, 1.0f},
        {x1, y0, z, 1.0f, color, 1.0f, 0.0f},
        {x1, y1, z, 1.0f, color, 1.0f, 1.0f},
        {x0, y1, z, 1.0f, color, 0.0f, 1.0f},
    };
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
    dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*D3DTSS_COLOROP*/, 4 /*D3DTOP_MODULATE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*D3DTSS_COLORARG1*/, 2 /*D3DTA_TEXTURE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*D3DTSS_COLORARG2*/, 0 /*D3DTA_DIFFUSE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*D3DTSS_ALPHAOP*/, 4 /*D3DTOP_MODULATE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*D3DTSS_ALPHAARG1*/, 2 /*D3DTA_TEXTURE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*D3DTSS_ALPHAARG2*/, 0 /*D3DTA_DIFFUSE*/);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, verts, sizeof(TV));
}

/** Restore untextured rendering state after textured draws. */
static void restore_untextured(IDirect3DDevice8 *dev)
{
    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->lpVtbl->SetTextureStageState(dev, 0, 1, 1 /*D3DTOP_DISABLE*/);
}

/* ── 3D Model Viewer Rendering ──────────────────────────────── */

/* Math utilities shared with rw_renderer.c */
#include "rw_math.h"

/** Render the 3D car model with ground plane. */
static void render_3d_model_view(IDirect3DDevice8 *dev, float dt)
{
    if (!g_car_model_loaded || !g_car_vb || !g_car_ib)
        return;

    DWORD fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    /* Draw gradient background for model viewer */
    {
        typedef struct { float x, y, z, rhw; DWORD color; } SV;
        DWORD bg_top = 0xFF1A2636;  /* dark blue-grey */
        DWORD bg_bot = 0xFF0A1018;  /* near black */
        SV bg[6] = {
            {0, 0, 0.999f, 1, bg_top}, {640, 0, 0.999f, 1, bg_top},
            {0, 480, 0.999f, 1, bg_bot}, {640, 0, 0.999f, 1, bg_top},
            {640, 480, 0.999f, 1, bg_bot}, {0, 480, 0.999f, 1, bg_bot},
        };
        dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, bg, sizeof(SV));
    }

    /* Camera orbit controls (arrow keys + zoom) */
    {
        int has_input = 0;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) { g_model_rot_y -= dt * 2.0f; has_input = 1; }
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { g_model_rot_y += dt * 2.0f; has_input = 1; }
        if (GetAsyncKeyState(VK_UP) & 0x8000) {
            g_model_cam_pitch += dt * 1.5f;
            if (g_model_cam_pitch > 1.4f) g_model_cam_pitch = 1.4f;
            has_input = 1;
        }
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
            g_model_cam_pitch -= dt * 1.5f;
            if (g_model_cam_pitch < -0.3f) g_model_cam_pitch = -0.3f;
            has_input = 1;
        }
        if (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000 || GetAsyncKeyState(VK_ADD) & 0x8000) {
            g_model_cam_dist -= dt * 2.0f;
            if (g_model_cam_dist < 1.2f) g_model_cam_dist = 1.2f;
            has_input = 1;
        }
        if (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000 || GetAsyncKeyState(VK_SUBTRACT) & 0x8000) {
            g_model_cam_dist += dt * 2.0f;
            if (g_model_cam_dist > 6.0f) g_model_cam_dist = 6.0f;
            has_input = 1;
        }
        /* Space toggles auto-rotation */
        {
            static int sp_prev = 0;
            int sp_now = (GetAsyncKeyState(VK_SPACE) & 0x8000) ? 1 : 0;
            if (sp_now && !sp_prev) g_model_auto_rotate = !g_model_auto_rotate;
            sp_prev = sp_now;
        }
        if (g_model_auto_rotate && !has_input)
            g_model_rot_y += dt * 0.5f;
    }

    /* Set up world matrix (identity - camera orbits around model) */
    D3DMATRIX world_mat;
    mat4_identity((float *)&world_mat);

    /* Set up view matrix: orbit camera using spherical coordinates */
    D3DMATRIX view_mat;
    float cam_r = g_car_model.bounding_radius * g_model_cam_dist;
    float cam_x = cam_r * cosf(g_model_cam_pitch) * sinf(g_model_rot_y);
    float cam_y = cam_r * sinf(g_model_cam_pitch);
    float cam_z = cam_r * cosf(g_model_cam_pitch) * cosf(g_model_rot_y);
    mat4_lookat((float *)&view_mat,
                cam_x, cam_y, cam_z,      /* eye */
                0.0f, 0.3f, 0.0f,         /* target (slightly above center) */
                0.0f, 1.0f, 0.0f);        /* up */

    /* Set up projection matrix */
    D3DMATRIX proj_mat;
    float aspect = 640.0f / 480.0f;
    mat4_perspective((float *)&proj_mat, 60.0f * 3.14159f / 180.0f, aspect, 0.1f, 100.0f);

    dev->lpVtbl->SetTransform(dev, D3DTS_WORLD, &world_mat);
    dev->lpVtbl->SetTransform(dev, D3DTS_VIEW, &view_mat);
    dev->lpVtbl->SetTransform(dev, D3DTS_PROJECTION, &proj_mat);

    /* Render state for 3D */
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);

    /* Bind paint texture (if available) for metallic car paint effect */
    if (g_paint_tex) {
        dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)g_paint_tex);
    } else {
        dev->lpVtbl->SetTexture(dev, 0, NULL);
    }

    /* Draw the car model using DrawIndexedPrimitiveUP */
    dev->lpVtbl->SetVertexShader(dev, fvf);
    dev->lpVtbl->SetStreamSource(dev, 0, NULL, 0);
    dev->lpVtbl->SetIndices(dev, NULL, 0);
    {
        dev->lpVtbl->DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLELIST,
                                           0, g_car_model.vertex_count,
                                           g_car_model.index_count / 3,
                                           g_car_model.indices, D3DFMT_INDEX16,
                                           g_car_model.vertices, sizeof(BGV_Vertex));
    }
    /* Unbind texture */
    dev->lpVtbl->SetTexture(dev, 0, NULL);

    /* Draw ground plane as a simple colored quad */
    {
        typedef struct { float x, y, z; float nx, ny, nz; DWORD color; float u, v; } GV;
        float gs = 10.0f;  /* ground size */
        float gy = -0.15f; /* just below car */
        DWORD gc = 0xFF333333; /* dark grey */
        GV ground[6] = {
            {-gs, gy, -gs,  0,1,0, gc, 0,0},
            { gs, gy, -gs,  0,1,0, gc, 1,0},
            {-gs, gy,  gs,  0,1,0, gc, 0,1},
            { gs, gy, -gs,  0,1,0, gc, 1,0},
            { gs, gy,  gs,  0,1,0, gc, 1,1},
            {-gs, gy,  gs,  0,1,0, gc, 0,1},
        };
        dev->lpVtbl->SetStreamSource(dev, 0, NULL, 0);
        dev->lpVtbl->SetIndices(dev, NULL, 0);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, ground, sizeof(GV));
    }

    /* Draw grid lines on the ground for spatial reference */
    {
        typedef struct { float x, y, z; float nx, ny, nz; DWORD color; float u, v; } GV;
        float gy = -0.14f;
        DWORD lc = 0xFF555555;
        float lw = 0.02f;
        int gi;
        for (gi = -5; gi <= 5; gi++) {
            float p = (float)gi * 2.0f;
            /* Z-axis line */
            GV lz[6] = {
                {p-lw, gy, -10, 0,1,0, lc, 0,0}, {p+lw, gy, -10, 0,1,0, lc, 1,0},
                {p-lw, gy,  10, 0,1,0, lc, 0,1}, {p+lw, gy, -10, 0,1,0, lc, 1,0},
                {p+lw, gy,  10, 0,1,0, lc, 1,1}, {p-lw, gy,  10, 0,1,0, lc, 0,1},
            };
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, lz, sizeof(GV));
            /* X-axis line */
            GV lx[6] = {
                {-10, gy, p-lw, 0,1,0, lc, 0,0}, { 10, gy, p-lw, 0,1,0, lc, 1,0},
                {-10, gy, p+lw, 0,1,0, lc, 0,1}, { 10, gy, p-lw, 0,1,0, lc, 1,0},
                { 10, gy, p+lw, 0,1,0, lc, 1,1}, {-10, gy, p+lw, 0,1,0, lc, 0,1},
            };
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, lx, sizeof(GV));
        }
    }

    /* HUD overlay: vehicle info bar + navigation hint */
    {
        typedef struct { float x, y, z, rhw; DWORD color; } SV;

        dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->lpVtbl->SetStreamSource(dev, 0, NULL, 0);
        dev->lpVtbl->SetIndices(dev, NULL, 0);
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);

        /* Dark bar at top */
        SV bar[6] = {
            {0, 0, 0, 1, 0xCC000000}, {640, 0, 0, 1, 0xCC000000},
            {0, 24, 0, 1, 0xCC000000}, {640, 0, 0, 1, 0xCC000000},
            {640, 24, 0, 1, 0xCC000000}, {0, 24, 0, 1, 0xCC000000},
        };
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, bar, sizeof(SV));

        /* Vehicle counter indicator: colored pips showing position in list */
        if (g_vehicle_count > 0) {
            float pip_x = 10.0f;
            float pip_y = 8.0f;
            float pip_w = 4.0f;
            float pip_h = 8.0f;
            float pip_gap = 2.0f;
            /* Show up to 20 pips around current position */
            int vis_start = g_vehicle_index - 10;
            int vis_end = g_vehicle_index + 10;
            if (vis_start < 0) { vis_end -= vis_start; vis_start = 0; }
            if (vis_end > g_vehicle_count) vis_end = g_vehicle_count;
            int vi;
            for (vi = vis_start; vi < vis_end; vi++) {
                DWORD pc = (vi == g_vehicle_index) ? 0xFFFF8800 : 0xFF666666;
                float pw = (vi == g_vehicle_index) ? pip_w + 2.0f : pip_w;
                float ph = (vi == g_vehicle_index) ? pip_h + 4.0f : pip_h;
                float py = (vi == g_vehicle_index) ? pip_y - 2.0f : pip_y;
                float px = pip_x + (float)(vi - vis_start) * (pip_w + pip_gap);
                SV pip[6] = {
                    {px, py, 0, 1, pc}, {px+pw, py, 0, 1, pc},
                    {px, py+ph, 0, 1, pc}, {px+pw, py, 0, 1, pc},
                    {px+pw, py+ph, 0, 1, pc}, {px, py+ph, 0, 1, pc},
                };
                dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, pip, sizeof(SV));
            }

            /* Navigation arrows: "<" on left, ">" on right */
            /* Left arrow (triangle pointing left) */
            SV arrow_l[3] = {
                {pip_x - 12.0f, pip_y + pip_h/2, 0, 1, 0xFFCCCCCC},
                {pip_x - 4.0f, pip_y, 0, 1, 0xFFCCCCCC},
                {pip_x - 4.0f, pip_y + pip_h, 0, 1, 0xFFCCCCCC},
            };
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, arrow_l, sizeof(SV));
            /* Right arrow */
            float rx = pip_x + (float)(vis_end - vis_start) * (pip_w + pip_gap) + 4.0f;
            SV arrow_r[3] = {
                {rx + 8.0f, pip_y + pip_h/2, 0, 1, 0xFFCCCCCC},
                {rx, pip_y, 0, 1, 0xFFCCCCCC},
                {rx, pip_y + pip_h, 0, 1, 0xFFCCCCCC},
            };
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, arrow_r, sizeof(SV));
        }

        /* Stats bar at bottom */
        SV bot_bar[6] = {
            {0, 456, 0, 1, 0xCC000000}, {640, 456, 0, 1, 0xCC000000},
            {0, 480, 0, 1, 0xCC000000}, {640, 456, 0, 1, 0xCC000000},
            {640, 480, 0, 1, 0xCC000000}, {0, 480, 0, 1, 0xCC000000},
        };
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, bot_bar, sizeof(SV));

        /* Vertex/tri count as horizontal bars (proportional to size) */
        if (g_car_model_loaded) {
            float vbar_w = (float)g_car_model.vertex_count / 10000.0f * 300.0f;
            if (vbar_w > 300.0f) vbar_w = 300.0f;
            float tbar_w = (float)(g_car_model.index_count / 3) / 10000.0f * 300.0f;
            if (tbar_w > 300.0f) tbar_w = 300.0f;
            SV vbar[6] = {
                {10, 460, 0, 1, 0xFF44AA44}, {10+vbar_w, 460, 0, 1, 0xFF44AA44},
                {10, 466, 0, 1, 0xFF44AA44}, {10+vbar_w, 460, 0, 1, 0xFF44AA44},
                {10+vbar_w, 466, 0, 1, 0xFF44AA44}, {10, 466, 0, 1, 0xFF44AA44},
            };
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, vbar, sizeof(SV));
            SV tbar[6] = {
                {10, 470, 0, 1, 0xFF4488CC}, {10+tbar_w, 470, 0, 1, 0xFF4488CC},
                {10, 476, 0, 1, 0xFF4488CC}, {10+tbar_w, 470, 0, 1, 0xFF4488CC},
                {10+tbar_w, 476, 0, 1, 0xFF4488CC}, {10, 476, 0, 1, 0xFF4488CC},
            };
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, tbar, sizeof(SV));
        }

        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    }

    /* Reset transforms to identity for subsequent 2D rendering */
    {
        D3DMATRIX ident;
        mat4_identity((float *)&ident);
        dev->lpVtbl->SetTransform(dev, D3DTS_WORLD, &ident);
        dev->lpVtbl->SetTransform(dev, D3DTS_VIEW, &ident);
        dev->lpVtbl->SetTransform(dev, D3DTS_PROJECTION, &ident);
    }
}

void game_frame_pump(void)
{
    static LARGE_INTEGER s_freq = {0};
    static LARGE_INTEGER s_last = {0};

    /* Initialize timer on first call */
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_last);
    }

    /* Throttle to ~60fps: only render if >= 16ms since last frame */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed_ms = (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)s_freq.QuadPart;
    if (elapsed_ms < 16.0)
        return;
    s_last = now;

    /* Pump Windows messages */
    {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = FALSE;
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    /* ── R key: instant race launch (works from any state) ── */
    {
        extern void fe_menu_force_race(void);
        extern int fe_menu_is_racing(void);
        static int r_key_prev = 0;
        int r_key_now = (GetAsyncKeyState('R') & 0x8000) ? 1 : 0;
        if (r_key_now && !r_key_prev && !fe_menu_is_racing()) {
            fprintf(stderr, "  [KEY] R pressed — launching race!\n");
            fe_menu_force_race();
        }
        r_key_prev = r_key_now;
    }

    /* ── Frontend menu update (when in menu state) ── */
    {
        float frame_dt = (float)(elapsed_ms / 1000.0);
        extern int fe_menu_is_racing(void);
        if (fe_menu_is_active() || fe_menu_is_racing()) {
            fe_menu_update(frame_dt);
        }
    }

    /* ── Input polling & injection into game memory ── */
    {
        extern ptrdiff_t g_xbox_mem_offset;
        #define XINP_MEM32(a) (*(volatile uint32_t*)((uintptr_t)(a) + g_xbox_mem_offset))
        #define XINP_MEMF(a)  (*(volatile float*)((uintptr_t)(a) + g_xbox_mem_offset))
        #define XINP_MEM8(a)  (*(volatile uint8_t*)((uintptr_t)(a) + g_xbox_mem_offset))

        /* Input injection during gameplay. Multiple detection methods:
         * - game_st == 4: crash mode (from xemu session 31)
         * - cam_ptr == 0x4D45D0: gameplay camera active
         * - fe_menu_is_racing(): race launched from menu (R key or Enter) */
        uint32_t game_st = XINP_MEM32(0x4D53B8);
        uint32_t cam_ptr = XINP_MEM32(0x4D5370);
        extern int fe_menu_is_racing(void);
        int in_gameplay = (game_st == 4 || cam_ptr == 0x4D45D0 || fe_menu_is_racing());
        if (in_gameplay) {
            int32_t throttle = 0;  /* positive = gas */
            int32_t steering = 0;  /* positive = right */

            /* Keyboard: WASD for driving, Shift for boost */
            if (GetAsyncKeyState('W') & 0x8000) throttle += 1000;
            if (GetAsyncKeyState('S') & 0x8000) throttle -= 1000;
            if (GetAsyncKeyState('A') & 0x8000) steering += 1000;
            if (GetAsyncKeyState('D') & 0x8000) steering -= 1000;
            XINP_MEM32(0x5FFD0C) = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;

            /* XInput gamepad (port 0) */
            {
                XINPUT_STATE xi;
                memset(&xi, 0, sizeof(xi));
                if (XInputGetState(0, &xi) == ERROR_SUCCESS) {
                    throttle += (int32_t)xi.Gamepad.bRightTrigger * 8;
                    throttle -= (int32_t)xi.Gamepad.bLeftTrigger * 8;
                    steering += (int32_t)xi.Gamepad.sThumbLX / 32;
                    /* A button or RB = boost */
                    if (xi.Gamepad.wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_RIGHT_SHOULDER))
                        XINP_MEM32(0x5FFD0C) = 1;
                }
            }

            /* Write to game input accumulator addresses.
             * sub_000636D0 computes force = (0x4D652C - 0x4D6B24 - 0x4D6B20) * scale.
             * We set the accumulators directly and zero the base/prev values. */
            XINP_MEM32(0x4D652C) = (uint32_t)throttle;
            XINP_MEM32(0x4D6530) = (uint32_t)steering;
            XINP_MEM32(0x4D6B20) = 0;
            XINP_MEM32(0x4D6B24) = 0;
            XINP_MEM32(0x4D6B28) = 0;

            /* Force physics scale factors to known-good values.
             * sub_000636D0 multiplies input by these; if any are 0, force is 0.
             * On Xbox, the game sets these during car/track setup, but some
             * may not be initialized in our recompilation. Force non-zero. */
            {
                float s1 = XINP_MEMF(0x557870);
                float s2 = XINP_MEMF(0x3B1C40);
                float s3 = XINP_MEMF(0x5592C8);
                float s4 = XINP_MEMF(0x3B1C38);
                if (s1 > -1e-10f && s1 < 1e-10f) XINP_MEMF(0x557870) = 0.001f;
                if (s2 > -1e-10f && s2 < 1e-10f) XINP_MEMF(0x3B1C40) = 1.0f;
                if (s3 > -1e-10f && s3 < 1e-10f) XINP_MEMF(0x5592C8) = 0.001f;
                if (s4 > -1e-10f && s4 < 1e-10f) XINP_MEMF(0x3B1C38) = 1.0f;
            }

            /* Direct physics velocity injection as fallback.
             * If the scale chain produces 0 (broken .rdata scales),
             * write force directly to the car's physics velocity vector.
             * Physics ptr = MEM32(0x557880 + 0x1B4), vel at +8/+0xC. */
            if (throttle != 0 || steering != 0) {
                uint32_t phys_ptr = XINP_MEM32(0x557880 + 0x1B4);
                if (phys_ptr > 0x10000 && phys_ptr < 0x4000000) {
                    float cur_vx = XINP_MEMF(phys_ptr + 8);
                    float cur_vy = XINP_MEMF(phys_ptr + 0xC);
                    /* Only inject if the scale path produced 0 */
                    if (cur_vx == 0.0f && cur_vy == 0.0f) {
                        XINP_MEMF(phys_ptr + 8) = (float)throttle * 0.01f;
                        XINP_MEMF(phys_ptr + 0xC) = (float)steering * 0.01f;
                    }
                }
            }

            /* Button events for menu navigation */
            if (GetAsyncKeyState(VK_UP) & 0x8000)     XINP_MEM8(0x4A1C74) = 1;
            if (GetAsyncKeyState(VK_DOWN) & 0x8000)    XINP_MEM8(0x4A1C78) = 1;
            if (GetAsyncKeyState(VK_LEFT) & 0x8000)    XINP_MEM8(0x4A1C76) = 1;
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000)   XINP_MEM8(0x4A1C77) = 1;
            if (GetAsyncKeyState(VK_RETURN) & 0x8000)  XINP_MEM8(0x4A1C75) = 1;
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)  XINP_MEM8(0x4A1C79) = 1;

            /* V key: (reserved, 3D mode is always on now) */
            {
            }

            /* G key: toggle gen render chain (sub_00351490/sub_00351770_gen) */
            {
                extern int g_gen_render_chain_enabled;
                static int g_key_prev = 0;
                int g_key_now = (GetAsyncKeyState('G') & 0x8000) ? 1 : 0;
                if (g_key_now && !g_key_prev) {
                    g_gen_render_chain_enabled = !g_gen_render_chain_enabled;
                    fprintf(stderr, "  [RENDER] Gen render chain %s\n",
                            g_gen_render_chain_enabled ? "ENABLED" : "DISABLED");
                }
                g_key_prev = g_key_now;
            }

            /* R key: instant race launch (bypasses menu navigation) */
            {
                extern void fe_menu_force_race(void);
                extern int fe_menu_is_racing(void);
                static int r_key_prev = 0;
                int r_key_now = (GetAsyncKeyState('R') & 0x8000) ? 1 : 0;
                if (r_key_now && !r_key_prev && !fe_menu_is_racing()) {
                    fprintf(stderr, "  [KEY] R pressed — launching race!\n");
                    fe_menu_force_race();
                }
                r_key_prev = r_key_now;
            }

            /* T key: cycle to next track (in 3D mode) */
            {
                static int t_key_prev = 0;
                int t_key_now = (GetAsyncKeyState('T') & 0x8000) ? 1 : 0;
                if (t_key_now && !t_key_prev && g_track_count > 0) {
                    g_track_index = (g_track_index + 1) % g_track_count;
                    load_track_by_index(g_track_index);
                }
                t_key_prev = t_key_now;
            }

            /* M key: toggle 3D model view */
            {
                static int m_key_prev = 0;
                int m_key_now = (GetAsyncKeyState('M') & 0x8000) ? 1 : 0;
                if (m_key_now && !m_key_prev) {
                    g_show_3d_model = !g_show_3d_model;
                    fprintf(stderr, "  [MODEL] 3D model view %s\n",
                            g_show_3d_model ? "ON" : "OFF");
                }
                m_key_prev = m_key_now;
            }

            /* N/P keys: cycle through vehicle models in 3D view */
            if (g_show_3d_model && g_vehicle_count > 1) {
                static int n_key_prev = 0, p_key_prev = 0;
                int n_key_now = (GetAsyncKeyState('N') & 0x8000) ? 1 : 0;
                int p_key_now = (GetAsyncKeyState('P') & 0x8000) ? 1 : 0;
                if (n_key_now && !n_key_prev) {
                    int next = (g_vehicle_index + 1) % g_vehicle_count;
                    load_vehicle_model(next);
                }
                if (p_key_now && !p_key_prev) {
                    int prev = (g_vehicle_index - 1 + g_vehicle_count) % g_vehicle_count;
                    load_vehicle_model(prev);
                }
                n_key_prev = n_key_now;
                p_key_prev = p_key_now;
            }

            /* G key: toggle NV2A push buffer test */
            {
                static int g_key_prev = 0;
                int g_key_now = (GetAsyncKeyState('G') & 0x8000) ? 1 : 0;
                if (g_key_now && !g_key_prev) {
                    extern int nv2a_pb_test_is_active(void);
                    extern void nv2a_pb_test_set_active(int active);
                    nv2a_pb_test_set_active(!nv2a_pb_test_is_active());
                }
                g_key_prev = g_key_now;
            }

            /* R key: toggle NV2A push buffer replay (xemu menu data) */
            {
                static int r_key_prev = 0;
                int r_key_now = (GetAsyncKeyState('R') & 0x8000) ? 1 : 0;
                if (r_key_now && !r_key_prev) {
                    extern int nv2a_pb_replay_is_active(void);
                    extern void nv2a_pb_replay_set_active(int active);
                    nv2a_pb_replay_set_active(!nv2a_pb_replay_is_active());
                }
                r_key_prev = r_key_now;
            }

            /* U key: play APU test tone (440Hz sine) */
            {
                static int u_key_prev = 0;
                int u_key_now = (GetAsyncKeyState('U') & 0x8000) ? 1 : 0;
                if (u_key_now && !u_key_prev) {
                    extern void mcpx_apu_play_test_tone(MCPXAPUState *d);
                    extern MCPXAPUState *g_apu_state;
                    if (g_apu_state) {
                        mcpx_apu_play_test_tone(g_apu_state);
                    } else {
                        fprintf(stderr, "[APU-TEST] No APU state - can't play test tone\n");
                    }
                }
                u_key_prev = u_key_now;
            }

            /* I key: play AWD game sounds (cycle through Fe.awd entries) */
            {
                static int i_key_prev = 0;
                static int i_sound_idx = 0;
                int i_key_now = (GetAsyncKeyState('I') & 0x8000) ? 1 : 0;
                if (i_key_now && !i_key_prev && g_awd_fe) {
                    awd_stop_all(g_awd_fe);
                    if (i_sound_idx >= g_awd_fe->num_entries) i_sound_idx = 0;
                    fprintf(stderr, "[AWD] Playing Fe.awd[%d]: '%s'\n",
                            i_sound_idx, g_awd_fe->entries[i_sound_idx].name);
                    awd_play_index(g_awd_fe, i_sound_idx, false);
                    i_sound_idx++;
                }
                i_key_prev = i_key_now;
            }

            /* Debug: log input state */
            {
                static int _inp_dbg = 0;
                _inp_dbg++;
                if (_inp_dbg == 1 || (_inp_dbg % 300 == 0) ||
                    (throttle != 0 && _inp_dbg % 30 == 0) ||
                    (steering != 0 && _inp_dbg % 30 == 0)) {
                    float s1 = XINP_MEMF(0x557870);
                    float s2 = XINP_MEMF(0x3B1C40);
                    float s3 = XINP_MEMF(0x5592C8);
                    float s4 = XINP_MEMF(0x3B1C38);
                    uint32_t phys_ptr = XINP_MEM32(0x557880 + 0x1B4);
                    float vx = 0.0f, vy = 0.0f;
                    if (phys_ptr > 0x10000 && phys_ptr < 0x4000000) {
                        vx = XINP_MEMF(phys_ptr + 8);
                        vy = XINP_MEMF(phys_ptr + 0xC);
                    }
                    fprintf(stderr, "  [INPUT] #%d thr=%d steer=%d scales=(%.6f,%.4f,%.6f,%.4f) vel=(%.2f,%.2f)\n",
                            _inp_dbg, throttle, steering, s1, s2, s3, s4, vx, vy);
                }
            }
        }

        #undef XINP_MEM32
        #undef XINP_MEMF
        #undef XINP_MEM8
    }

    /* ── Game state transition logging ── */
    {
        static uint32_t prev_game_state = 0;
        uint32_t cur_state = MEM32(0x4D53B8);
        if (cur_state != prev_game_state && prev_game_state != 0) {
            fprintf(stderr, "[STATE] %u→%u cam=0x%08X\n",
                    prev_game_state, cur_state, MEM32(0x4D5370));
        }
        prev_game_state = cur_state;
    }

    /* ── Boot sequence / gameplay rendering ── */
    if (g_d3d_device) {
        int boot_phase = boot_get_phase();

        /* During boot video phases, render video fullscreen instead of gameplay */
        if (boot_phase < BOOT_PHASE_GAMEPLAY) {
            /* Real wall-clock dt for accurate video playback speed */
            static LARGE_INTEGER boot_last_time = {0};
            static LARGE_INTEGER boot_freq = {0};
            float boot_dt;
            if (boot_freq.QuadPart == 0) {
                QueryPerformanceFrequency(&boot_freq);
                QueryPerformanceCounter(&boot_last_time);
            }
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                boot_dt = (float)(now.QuadPart - boot_last_time.QuadPart) / (float)boot_freq.QuadPart;
                boot_last_time = now;
                if (boot_dt > 0.1f) boot_dt = 0.1f; /* clamp to avoid jumps */
            }

            /* Check for skip input (any key or gamepad button) */
            int skip = 0;
            if (GetAsyncKeyState(VK_RETURN) & 0x8000) skip = 1;
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) skip = 1;
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) skip = 1;
            /* Gamepad: any button */
            {
                XINPUT_STATE xstate;
                if (XInputGetState(0, &xstate) == ERROR_SUCCESS) {
                    if (xstate.Gamepad.wButtons) skip = 1;
                    if (xstate.Gamepad.bLeftTrigger > 30) skip = 1;
                    if (xstate.Gamepad.bRightTrigger > 30) skip = 1;
                }
            }

            boot_update(boot_dt, skip);

            g_d3d_device->lpVtbl->BeginScene(g_d3d_device);
            g_d3d_device->lpVtbl->Clear(g_d3d_device, 0, NULL,
                D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                0xFF000000, /* Black background for videos */
                1.0f, 0);
            boot_render();
            g_d3d_device->lpVtbl->EndScene(g_d3d_device);
            menu_gui_begin_frame();
            menu_gui_render();
            g_d3d_device->lpVtbl->Present(g_d3d_device, NULL, NULL, NULL, NULL);
            return; /* Skip gameplay rendering during boot */
        }

        /* === Gameplay rendering === */

        /* Initialize texture cache on first frame */
        txd_cache_init();

        /* Check if the RW bridge already rendered this frame.
         * If so, the RW pipeline (sub_001DDAF0 → sub_00351090 → bridge)
         * already called BeginScene/Clear/rw_gameplay_render/EndScene.
         * We only need to do the model viewer or NV2A test, plus Present. */
        if (!rw_bridge_frame_rendered()) {
            g_d3d_device->lpVtbl->BeginScene(g_d3d_device);
            g_d3d_device->lpVtbl->Clear(g_d3d_device, 0, NULL,
                D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                0xFF102040, /* Dark blue-grey */
                1.0f, 0);

            /* 3D model view mode (toggle with M key) */
            if (g_show_3d_model && g_car_model_loaded) {
                render_3d_model_view(g_d3d_device, 1.0f / 60.0f);
            }
            else {
                /* 3D rendering mode (fallback when bridge isn't active) */
                rw_gameplay_render();
            }
        } else {
            /* Bridge rendered — BeginScene/EndScene already done.
             * NV2A test and ImGui will run after this block. */
        }
#if 0  /* Pseudo-3D mode removed - 3D is always on */
        if (0)
        {
            extern ptrdiff_t g_xbox_mem_offset;
            #define _R_MEMF(a) (*(volatile float*)((uintptr_t)(a) + g_xbox_mem_offset))
            #define _R_MEM32(a) (*(volatile uint32_t*)((uintptr_t)(a) + g_xbox_mem_offset))
            uint32_t _phys = _R_MEM32(0x557880 + 0x1B4);
            if (_phys > 0x100 && _phys < 0x4000000) {
                float px      = _R_MEMF(_phys + 0x10);
                float py      = _R_MEMF(_phys + 0x14);
                float heading = _R_MEMF(_phys + 0x18);
                float speed   = _R_MEMF(_phys + 0x1C);

                /* Screen and perspective constants */
                const float SW = 640.0f, SH = 480.0f;
                const float CX = SW * 0.5f;
                const float HORIZON = SH * 0.38f;  /* horizon line y */
                const float CAM_H = 4.5f;          /* camera height */
                const float FOCAL = 120.0f;         /* focal length */
                const float ROAD_HW = 15.0f;       /* road half-width */
                const float VIEW_DIST = 200.0f;     /* max draw distance */

                /* Time-of-day: cycles through dawn→day→sunset→night based on distance.
                 * Full cycle every 3000 world units (~60 seconds at speed 50). */
                DWORD tod_sky_top, tod_sky_bot, tod_road_a, tod_road_b, tod_grass, tod_mtn;
                {
                    float cycle = fmodf(py / 3000.0f, 1.0f);
                    if (cycle < 0.0f) cycle += 1.0f;
                    /* Lerp helper: interpolate ARGB color channels */
                    #define LERP_COL(a, b, t) ( \
                        (((DWORD)(((float)(((a)>>24)&0xFF))*(1.0f-(t)) + ((float)(((b)>>24)&0xFF))*(t))) << 24) | \
                        (((DWORD)(((float)(((a)>>16)&0xFF))*(1.0f-(t)) + ((float)(((b)>>16)&0xFF))*(t))) << 16) | \
                        (((DWORD)(((float)(((a)>>8)&0xFF))*(1.0f-(t)) + ((float)(((b)>>8)&0xFF))*(t))) << 8) | \
                        (((DWORD)(((float)((a)&0xFF))*(1.0f-(t)) + ((float)((b)&0xFF))*(t)))) )
                    if (cycle < 0.25f) {
                        /* Dawn → Day (0.0 - 0.25) */
                        float t = cycle / 0.25f;
                        tod_sky_top = LERP_COL(0xFF2A1040, 0xFF1020A0, t);
                        tod_sky_bot = LERP_COL(0xFFDD6633, 0xFF6090D0, t);
                        tod_grass   = LERP_COL(0xFF1A2810, 0xFF1A3318, t);
                        tod_mtn     = LERP_COL(0xFF3A2040, 0xFF304060, t);
                    } else if (cycle < 0.5f) {
                        /* Day → Sunset (0.25 - 0.5) */
                        float t = (cycle - 0.25f) / 0.25f;
                        tod_sky_top = LERP_COL(0xFF1020A0, 0xFF602080, t);
                        tod_sky_bot = LERP_COL(0xFF6090D0, 0xFFFF6622, t);
                        tod_grass   = LERP_COL(0xFF1A3318, 0xFF2A2810, t);
                        tod_mtn     = LERP_COL(0xFF304060, 0xFF503040, t);
                    } else if (cycle < 0.75f) {
                        /* Sunset → Night (0.5 - 0.75) */
                        float t = (cycle - 0.5f) / 0.25f;
                        tod_sky_top = LERP_COL(0xFF602080, 0xFF080818, t);
                        tod_sky_bot = LERP_COL(0xFFFF6622, 0xFF101830, t);
                        tod_grass   = LERP_COL(0xFF2A2810, 0xFF0A1508, t);
                        tod_mtn     = LERP_COL(0xFF503040, 0xFF101828, t);
                    } else {
                        /* Night → Dawn (0.75 - 1.0) */
                        float t = (cycle - 0.75f) / 0.25f;
                        tod_sky_top = LERP_COL(0xFF080818, 0xFF2A1040, t);
                        tod_sky_bot = LERP_COL(0xFF101830, 0xFFDD6633, t);
                        tod_grass   = LERP_COL(0xFF0A1508, 0xFF1A2810, t);
                        tod_mtn     = LERP_COL(0xFF101828, 0xFF3A2040, t);
                    }
                    /* Road colors darken at night */
                    float night = (cycle > 0.5f) ? (cycle < 0.75f ? (cycle - 0.5f) / 0.25f : 1.0f - (cycle - 0.75f) / 0.25f) : 0.0f;
                    tod_road_a = LERP_COL(0xFF222233, 0xFF111118, night);
                    tod_road_b = LERP_COL(0xFF1A1A2A, 0xFF0A0A14, night);
                    #undef LERP_COL
                }

                /* XYZRHW + DIFFUSE vertex */
                typedef struct { float x, y, z, rhw; DWORD color; } RHW_VERT;
                typedef struct { float x, y, z, rhw; DWORD color; float u, v; } RHW_TEX_VERT;

                /* Screen shake: random viewport offset during crash */
                float shake_x = 0.0f, shake_y = 0.0f;
                {
                    float shake_t = _R_MEMF(0x5FFD18);
                    if (shake_t > 0.0f) {
                        static uint32_t _shake_seed = 31337;
                        _shake_seed = _shake_seed * 1103515245 + 12345;
                        float sx_r = ((float)((_shake_seed >> 16) & 0xFF) / 127.5f) - 1.0f;
                        _shake_seed = _shake_seed * 1103515245 + 12345;
                        float sy_r = ((float)((_shake_seed >> 16) & 0xFF) / 127.5f) - 1.0f;
                        float intensity = shake_t * 12.0f; /* max 12px offset */
                        shake_x = sx_r * intensity;
                        shake_y = sy_r * intensity;
                    }
                }

                /* Helper: project world point to screen.
                 * World: x=lateral (0=center), d=distance ahead of camera.
                 * Returns screen x and y, and scale factor.
                 * shake_x/shake_y add viewport offset during crashes. */
                #define PROJ_X(wx, d) (CX + shake_x + ((wx) - px) * FOCAL / (d))
                #define PROJ_Y(d) (HORIZON + shake_y + CAM_H * FOCAL / (d))
                #define PROJ_SCALE(d) (FOCAL / (d))

                /* Road curve function: overlapping sine waves for S-curves.
                 * Returns a curvature value (positive = road bends right). */
                #define ROAD_CURVE(world_y) \
                    (sinf((world_y) * 0.008f) * 0.5f + sinf((world_y) * 0.022f) * 0.2f)

                /* Road hill function: sine waves for gentle ups and downs.
                 * Returns a vertical offset (positive = uphill). */
                #define ROAD_HILL(world_y) \
                    (sinf((world_y) * 0.005f) * 0.3f + sinf((world_y) * 0.013f) * 0.15f)

                /* Pre-compute curve and hill offsets for each segment boundary.
                 * These accumulate in screen-space pixels so far segments
                 * appear shifted, creating curved and hilly road illusion.
                 * Player heading adds visual road curvature (OutRun-style):
                 * steering left makes the road appear to curve right, giving
                 * the feeling of turning into the road rather than sliding. */
                #define ROAD_SEGS 50
                float curve_offsets[ROAD_SEGS + 1];
                float hill_offsets[ROAD_SEGS + 1];
                {
                    int ci;
                    curve_offsets[0] = 0.0f;
                    hill_offsets[0] = 0.0f;
                    /* Steering-induced visual curvature: road bends opposite to heading */
                    float steer_curve = -heading * 0.4f;
                    for (ci = 0; ci < ROAD_SEGS; ci++) {
                        float ct0 = (float)ci / ROAD_SEGS;
                        float ct1 = (float)(ci + 1) / ROAD_SEGS;
                        float cd0 = 2.0f + ct0 * ct0 * VIEW_DIST;
                        float cd1 = 2.0f + ct1 * ct1 * VIEW_DIST;
                        float cwy = py + (cd0 + cd1) * 0.5f;
                        float scale = FOCAL / ((cd0 + cd1) * 0.5f);
                        float track_curve = ROAD_CURVE(cwy) * scale * 20.0f;
                        float steer_vis = steer_curve * scale * 25.0f;
                        curve_offsets[ci + 1] = curve_offsets[ci] + track_curve + steer_vis;
                        hill_offsets[ci + 1] = hill_offsets[ci] + ROAD_HILL(cwy) * scale * 15.0f;
                    }
                }

                /* Store road curve at player position for physics (centripetal force) */
                {
                    float player_curve = ROAD_CURVE(py);
                    *(volatile float*)((uintptr_t)0x5FFD10 + g_xbox_mem_offset) = player_curve;
                }

                /* Set render state */
                g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                    D3DRS_ZENABLE, 0);
                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                    D3DRS_LIGHTING, 0);
                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                    D3DRS_CULLMODE, 1 /* D3DCULL_NONE */);
                g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0, NULL);

                /* ── Sky gradient ────────────────────────────────────── */
                {
                    DWORD sky_top = tod_sky_top;
                    DWORD sky_bot = tod_sky_bot;
                    RHW_VERT sky[6] = {
                        {0.0f, 0.0f, 0.99f, 1.0f, sky_top},
                        {SW,   0.0f, 0.99f, 1.0f, sky_top},
                        {0.0f, HORIZON, 0.99f, 1.0f, sky_bot},
                        {SW,   0.0f, 0.99f, 1.0f, sky_top},
                        {SW,   HORIZON, 0.99f, 1.0f, sky_bot},
                        {0.0f, HORIZON, 0.99f, 1.0f, sky_bot},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 2, sky, sizeof(RHW_VERT));
                }

                /* ── Stars (visible during night phase) ────────────── */
                {
                    float cycle = fmodf(py / 3000.0f, 1.0f);
                    if (cycle < 0.0f) cycle += 1.0f;
                    /* Stars visible when cycle > 0.5 (sunset→night→dawn) */
                    float star_alpha_f = 0.0f;
                    if (cycle > 0.55f && cycle < 0.95f) {
                        /* Ramp in 0.55-0.65, full 0.65-0.85, ramp out 0.85-0.95 */
                        if (cycle < 0.65f) star_alpha_f = (cycle - 0.55f) / 0.1f;
                        else if (cycle > 0.85f) star_alpha_f = 1.0f - (cycle - 0.85f) / 0.1f;
                        else star_alpha_f = 1.0f;
                    }
                    if (star_alpha_f > 0.01f) {
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            19, 5); /* SRCALPHA */
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            20, 6); /* INVSRCALPHA */
                        /* 40 deterministic stars using Knuth hash of index */
                        #define STAR_COUNT 40
                        static uint32_t _twinkle_seed = 99;
                        _twinkle_seed = _twinkle_seed * 1103515245 + 12345;
                        int si_star;
                        for (si_star = 0; si_star < STAR_COUNT; si_star++) {
                            uint32_t h = (uint32_t)si_star * 2654435761u;
                            float sx_s = (float)((h >> 8) % 620) + 10.0f;
                            float sy_s = (float)((h >> 16) % (int)(HORIZON - 20.0f)) + 5.0f;
                            /* Twinkle: vary brightness per star per frame */
                            float twinkle = 0.6f + 0.4f * sinf((float)si_star * 3.7f +
                                (float)(_twinkle_seed & 0xFF) * 0.025f);
                            int alpha = (int)(star_alpha_f * twinkle * 255.0f);
                            if (alpha > 255) alpha = 255;
                            /* Star color: mostly white, some slightly blue/yellow */
                            DWORD star_rgb = (h & 3) == 0 ? 0x00AACCFF :
                                             (h & 3) == 1 ? 0x00FFFFCC : 0x00FFFFFF;
                            DWORD star_c = ((DWORD)alpha << 24) | star_rgb;
                            float sz = 0.8f + (float)((h >> 4) & 3) * 0.3f; /* 0.8-1.7px */
                            RHW_VERT star_v[6] = {
                                {sx_s-sz, sy_s-sz, 0.97f, 1.0f, star_c},
                                {sx_s+sz, sy_s-sz, 0.97f, 1.0f, star_c},
                                {sx_s-sz, sy_s+sz, 0.97f, 1.0f, star_c},
                                {sx_s+sz, sy_s-sz, 0.97f, 1.0f, star_c},
                                {sx_s+sz, sy_s+sz, 0.97f, 1.0f, star_c},
                                {sx_s-sz, sy_s+sz, 0.97f, 1.0f, star_c},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, star_v, sizeof(RHW_VERT));
                        }
                        #undef STAR_COUNT
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }
                }

                /* ── Horizon scenery (mountain silhouettes) ─────────── */
                {
                    /* Simple mountain range: triangles along the horizon.
                     * Heights vary by a sine pattern offset by player position
                     * so they appear to slowly shift as you drive. */
                    #define MTN_COUNT 12
                    DWORD mtn_col_v = tod_mtn; /* from time-of-day */
                    RHW_VERT mtn_verts[MTN_COUNT * 3];
                    int mi;
                    for (mi = 0; mi < MTN_COUNT; mi++) {
                        float base_x = (float)mi / MTN_COUNT * SW - 30.0f;
                        float width = SW / MTN_COUNT * 1.4f;
                        /* Height varies with a pseudo-random pattern */
                        float h = 20.0f + 35.0f * sinf((float)mi * 2.3f + py * 0.0002f);
                        float peak_x = base_x + width * 0.5f + 10.0f * sinf((float)mi * 1.7f);
                        mtn_verts[mi * 3 + 0] = (RHW_VERT){base_x, HORIZON, 0.98f, 1.0f, mtn_col_v};
                        mtn_verts[mi * 3 + 1] = (RHW_VERT){peak_x, HORIZON - h, 0.98f, 1.0f, mtn_col_v};
                        mtn_verts[mi * 3 + 2] = (RHW_VERT){base_x + width, HORIZON, 0.98f, 1.0f, mtn_col_v};
                    }
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, MTN_COUNT, mtn_verts, sizeof(RHW_VERT));
                    #undef MTN_COUNT
                }

                /* ── Ground plane (grass on both sides of road) ─────── */
                {
                    DWORD grass_col_v = tod_grass; /* from time-of-day */
                    RHW_VERT grass[6] = {
                        {0.0f, HORIZON, 0.95f, 1.0f, grass_col_v},
                        {SW,   HORIZON, 0.95f, 1.0f, grass_col_v},
                        {0.0f, SH,      0.95f, 1.0f, grass_col_v},
                        {SW,   HORIZON, 0.95f, 1.0f, grass_col_v},
                        {SW,   SH,      0.95f, 1.0f, grass_col_v},
                        {0.0f, SH,      0.95f, 1.0f, grass_col_v},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 2, grass, sizeof(RHW_VERT));
                }

                /* ── Road segments (perspective trapezoids with asphalt texture) ── */
                {
                    /* Build all road segment vertices in one array for batched draw */
                    RHW_TEX_VERT road_verts[ROAD_SEGS * 6];
                    int vi = 0;
                    int si;

                    /* Bind road texture and switch to textured vertex format */
                    if (g_road_tex) {
                        g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0,
                            (IDirect3DBaseTexture8 *)g_road_tex);
                        g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device,
                            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                    }

                    for (si = 0; si < ROAD_SEGS; si++) {
                        /* Exponential depth distribution: more detail up close */
                        float t0 = (float)si / ROAD_SEGS;
                        float t1 = (float)(si + 1) / ROAD_SEGS;
                        float d0 = 2.0f + t0 * t0 * VIEW_DIST;
                        float d1 = 2.0f + t1 * t1 * VIEW_DIST;

                        /* Screen Y for near and far edges, with hill offset */
                        float ho0 = hill_offsets[si], ho1 = hill_offsets[si + 1];
                        float y0 = PROJ_Y(d0) - ho0;
                        float y1 = PROJ_Y(d1) - ho1;
                        if (y0 < HORIZON - 30.0f || y1 > SH) continue;
                        if (y0 > SH) y0 = SH;

                        /* Road edges with curve offset */
                        float co0 = curve_offsets[si], co1 = curve_offsets[si + 1];
                        float lx0 = PROJ_X(-ROAD_HW, d0) + co0;
                        float rx0 = PROJ_X(ROAD_HW, d0) + co0;
                        float lx1 = PROJ_X(-ROAD_HW, d1) + co1;
                        float rx1 = PROJ_X(ROAD_HW, d1) + co1;

                        /* Alternating road color based on world distance (rumble strips) */
                        float world_d = py + (d0 + d1) * 0.5f;
                        int stripe = ((int)(world_d / 3.0f)) & 1;
                        DWORD road_col = stripe ? tod_road_a : tod_road_b;

                        /* UV coords: U across road width (0-1), V tiles along distance */
                        float v0 = fmodf(world_d * 0.15f, 1.0f);
                        float v1 = fmodf((py + (d1 + d0) * 0.5f + 3.0f) * 0.15f, 1.0f);

                        /* Draw road surface quad */
                        road_verts[vi++] = (RHW_TEX_VERT){lx0, y0, 0.9f, 1.0f, road_col, 0.0f, v0};
                        road_verts[vi++] = (RHW_TEX_VERT){rx0, y0, 0.9f, 1.0f, road_col, 1.0f, v0};
                        road_verts[vi++] = (RHW_TEX_VERT){lx1, y1, 0.9f, 1.0f, road_col, 0.0f, v1};
                        road_verts[vi++] = (RHW_TEX_VERT){rx0, y0, 0.9f, 1.0f, road_col, 1.0f, v0};
                        road_verts[vi++] = (RHW_TEX_VERT){rx1, y1, 0.9f, 1.0f, road_col, 1.0f, v1};
                        road_verts[vi++] = (RHW_TEX_VERT){lx1, y1, 0.9f, 1.0f, road_col, 0.0f, v1};
                    }
                    if (vi > 0) {
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, vi / 3, road_verts, sizeof(RHW_TEX_VERT));
                    }

                    /* Unbind road texture and restore untextured vertex format */
                    if (g_road_tex) {
                        g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0, NULL);
                        g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device,
                            D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                    }

                    /* Rain puddles on road surface */
                    {
                        float wcyc = fmodf(py / 6000.0f, 1.0f);
                        if (wcyc < 0.0f) wcyc += 1.0f;
                        if (wcyc > 0.70f && wcyc < 0.98f) {
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 1);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                19, 5);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                20, 6);
                            /* Scatter puddles on road using deterministic hash */
                            int puddle_si;
                            for (puddle_si = 0; puddle_si < ROAD_SEGS; puddle_si += 3) {
                                float pt0 = (float)puddle_si / ROAD_SEGS;
                                float pd0 = 2.0f + pt0 * pt0 * VIEW_DIST;
                                float pwy = py + pd0;
                                uint32_t ph = (uint32_t)((int)(pwy / 8.0f)) * 2654435761u;
                                if ((ph & 7) > 2) continue; /* ~37% of segments have puddle */
                                float pho = hill_offsets[puddle_si];
                                float pco = curve_offsets[puddle_si];
                                float ps = PROJ_SCALE(pd0);
                                float plat = (float)((int)((ph >> 8) & 0x1F) - 15) * 0.5f;
                                float ppx = PROJ_X(plat, pd0) + pco;
                                float ppy = PROJ_Y(pd0) - pho;
                                float ppw = 3.0f * ps, pph = 1.0f * ps;
                                if (ppw < 2.0f || ppy < HORIZON || ppy > SH) continue;
                                DWORD puddle_c = 0x305577AA; /* semi-transparent blue-grey */
                                RHW_VERT pud[6] = {
                                    {ppx-ppw, ppy-pph, 0.85f, 1.0f, puddle_c},
                                    {ppx+ppw, ppy-pph, 0.85f, 1.0f, puddle_c},
                                    {ppx-ppw, ppy+pph, 0.85f, 1.0f, puddle_c},
                                    {ppx+ppw, ppy-pph, 0.85f, 1.0f, puddle_c},
                                    {ppx+ppw, ppy+pph, 0.85f, 1.0f, puddle_c},
                                    {ppx-ppw, ppy+pph, 0.85f, 1.0f, puddle_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, pud, sizeof(RHW_VERT));
                            }
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 0);
                        }
                    }

                    /* Road shoulder/curb (raised edge strips for depth) */
                    {
                        RHW_VERT curb_verts[ROAD_SEGS * 12]; /* left + right × 6 verts */
                        int cvi = 0;
                        for (si = 0; si < ROAD_SEGS; si++) {
                            float t0 = (float)si / ROAD_SEGS;
                            float t1 = (float)(si + 1) / ROAD_SEGS;
                            float d0 = 2.0f + t0 * t0 * VIEW_DIST;
                            float d1 = 2.0f + t1 * t1 * VIEW_DIST;
                            float ho0 = hill_offsets[si], ho1 = hill_offsets[si + 1];
                            float y0 = PROJ_Y(d0) - ho0;
                            float y1 = PROJ_Y(d1) - ho1;
                            if (y0 < HORIZON - 30.0f || y1 > SH) continue;
                            if (y0 > SH) y0 = SH;
                            float co0 = curve_offsets[si], co1 = curve_offsets[si + 1];
                            float s0 = PROJ_SCALE(d0), s1 = PROJ_SCALE(d1);
                            float curb_h0 = 0.3f * s0, curb_h1 = 0.3f * s1; /* curb height (subtle) */
                            float curb_w0 = 1.5f * s0, curb_w1 = 1.5f * s1; /* curb width */
                            DWORD curb_top = 0xFF555560;
                            /* Left curb */
                            float le0 = PROJ_X(-ROAD_HW, d0) + co0;
                            float le1 = PROJ_X(-ROAD_HW, d1) + co1;
                            /* Top face */
                            curb_verts[cvi++] = (RHW_VERT){le0-curb_w0, y0-curb_h0, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){le0,         y0-curb_h0, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){le1-curb_w1, y1-curb_h1, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){le0,         y0-curb_h0, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){le1,         y1-curb_h1, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){le1-curb_w1, y1-curb_h1, 0.88f, 1.0f, curb_top};
                            /* Right curb */
                            float re0 = PROJ_X(ROAD_HW, d0) + co0;
                            float re1 = PROJ_X(ROAD_HW, d1) + co1;
                            curb_verts[cvi++] = (RHW_VERT){re0,         y0-curb_h0, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){re0+curb_w0, y0-curb_h0, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){re1,         y1-curb_h1, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){re0+curb_w0, y0-curb_h0, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){re1+curb_w1, y1-curb_h1, 0.88f, 1.0f, curb_top};
                            curb_verts[cvi++] = (RHW_VERT){re1,         y1-curb_h1, 0.88f, 1.0f, curb_top};
                        }
                        if (cvi > 0) {
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, cvi / 3, curb_verts, sizeof(RHW_VERT));
                        }
                    }

                    /* Edge lines and center dashes (drawn over road) */
                    {
                        RHW_VERT line_verts[ROAD_SEGS * 30]; /* 5 lines × 6 verts per seg */
                        int lvi = 0;
                        for (si = 0; si < ROAD_SEGS; si++) {
                            float t0 = (float)si / ROAD_SEGS;
                            float t1 = (float)(si + 1) / ROAD_SEGS;
                            float d0 = 2.0f + t0 * t0 * VIEW_DIST;
                            float d1 = 2.0f + t1 * t1 * VIEW_DIST;
                            float ho0 = hill_offsets[si], ho1 = hill_offsets[si + 1];
                            float y0 = PROJ_Y(d0) - ho0;
                            float y1 = PROJ_Y(d1) - ho1;
                            if (y0 < HORIZON - 30.0f || y1 > SH) continue;
                            if (y0 > SH) y0 = SH;

                            float co0 = curve_offsets[si], co1 = curve_offsets[si + 1];
                            float scale0 = PROJ_SCALE(d0);
                            float scale1 = PROJ_SCALE(d1);
                            float ew0 = 0.6f * scale0, ew1 = 0.6f * scale1;

                            float world_d = py + (d0 + d1) * 0.5f;
                            int stripe = ((int)(world_d / 3.0f)) & 1;
                            DWORD edge_col = stripe ? 0xFFCC2222 : 0xFFCCCCCC;

                            /* Left edge line */
                            float le0 = PROJ_X(-ROAD_HW, d0) + co0;
                            float le1 = PROJ_X(-ROAD_HW, d1) + co1;
                            line_verts[lvi++] = (RHW_VERT){le0-ew0, y0, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){le0+ew0, y0, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){le1-ew1, y1, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){le0+ew0, y0, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){le1+ew1, y1, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){le1-ew1, y1, 0.8f, 1.0f, edge_col};

                            /* Right edge line */
                            float re0 = PROJ_X(ROAD_HW, d0) + co0;
                            float re1 = PROJ_X(ROAD_HW, d1) + co1;
                            line_verts[lvi++] = (RHW_VERT){re0-ew0, y0, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){re0+ew0, y0, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){re1-ew1, y1, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){re0+ew0, y0, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){re1+ew1, y1, 0.8f, 1.0f, edge_col};
                            line_verts[lvi++] = (RHW_VERT){re1-ew1, y1, 0.8f, 1.0f, edge_col};

                            /* Center dash (yellow - divides traffic directions) */
                            float seg_world = py + (d0 + d1) * 0.5f;
                            float phase = fmodf(seg_world, 7.0f);
                            if (phase < 0) phase += 7.0f;
                            if (phase < 3.5f) {
                                DWORD dash_col = 0xFFDDDD44;
                                float dw0 = 0.3f * scale0, dw1 = 0.3f * scale1;
                                float cx0 = PROJ_X(0.0f, d0) + co0;
                                float cx1 = PROJ_X(0.0f, d1) + co1;
                                line_verts[lvi++] = (RHW_VERT){cx0-dw0, y0, 0.7f, 1.0f, dash_col};
                                line_verts[lvi++] = (RHW_VERT){cx0+dw0, y0, 0.7f, 1.0f, dash_col};
                                line_verts[lvi++] = (RHW_VERT){cx1-dw1, y1, 0.7f, 1.0f, dash_col};
                                line_verts[lvi++] = (RHW_VERT){cx0+dw0, y0, 0.7f, 1.0f, dash_col};
                                line_verts[lvi++] = (RHW_VERT){cx1+dw1, y1, 0.7f, 1.0f, dash_col};
                                line_verts[lvi++] = (RHW_VERT){cx1-dw1, y1, 0.7f, 1.0f, dash_col};
                            }

                            /* Lane dashes (white - divides lanes within each side) */
                            {
                                float lane_phase = fmodf(seg_world, 5.0f);
                                if (lane_phase < 0) lane_phase += 5.0f;
                                if (lane_phase < 2.5f) {
                                    DWORD lc = 0xFF888899;
                                    float lw0 = 0.2f * scale0, lw1 = 0.2f * scale1;
                                    /* Right-side lane divider at +ROAD_HW/2 */
                                    float rl0 = PROJ_X(ROAD_HW * 0.5f, d0) + co0;
                                    float rl1 = PROJ_X(ROAD_HW * 0.5f, d1) + co1;
                                    line_verts[lvi++] = (RHW_VERT){rl0-lw0, y0, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){rl0+lw0, y0, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){rl1-lw1, y1, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){rl0+lw0, y0, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){rl1+lw1, y1, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){rl1-lw1, y1, 0.75f, 1.0f, lc};
                                    /* Left-side lane divider at -ROAD_HW/2 */
                                    float ll0 = PROJ_X(-ROAD_HW * 0.5f, d0) + co0;
                                    float ll1 = PROJ_X(-ROAD_HW * 0.5f, d1) + co1;
                                    line_verts[lvi++] = (RHW_VERT){ll0-lw0, y0, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){ll0+lw0, y0, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){ll1-lw1, y1, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){ll0+lw0, y0, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){ll1+lw1, y1, 0.75f, 1.0f, lc};
                                    line_verts[lvi++] = (RHW_VERT){ll1-lw1, y1, 0.75f, 1.0f, lc};
                                }
                            }
                        }
                        if (lvi > 0) {
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, lvi / 3, line_verts, sizeof(RHW_VERT));
                        }
                    }
                }

                /* ── Roadside scenery (trees, posts, buildings) ────────── */
                {
                    /* Objects appear every ~18 world units along road edges.
                     * Type is deterministic based on world position hash. */
                    float obj_spacing = 18.0f;
                    float first_obj = obj_spacing - fmodf(py, obj_spacing);
                    if (first_obj < 0.0f) first_obj += obj_spacing;
                    float pd;
                    for (pd = first_obj; pd < VIEW_DIST; pd += obj_spacing) {
                        float post_y = PROJ_Y(pd);
                        float sc = PROJ_SCALE(pd);
                        if (post_y < HORIZON - 30.0f || post_y > SH || sc < 0.01f) continue;

                        /* Interpolate curve/hill offsets */
                        float t_p = 0.0f;
                        if (pd > 2.0f) t_p = sqrtf((pd - 2.0f) / VIEW_DIST);
                        if (t_p > 1.0f) t_p = 1.0f;
                        float seg_f = t_p * ROAD_SEGS;
                        int seg_i = (int)seg_f;
                        if (seg_i >= ROAD_SEGS) seg_i = ROAD_SEGS - 1;
                        float frac = seg_f - (float)seg_i;
                        float pco = curve_offsets[seg_i] * (1.0f - frac) + curve_offsets[seg_i + 1] * frac;
                        float pho = hill_offsets[seg_i] * (1.0f - frac) + hill_offsets[seg_i + 1] * frac;
                        float sy_base = post_y - pho;

                        /* Deterministic object type from world Y position */
                        int world_idx = (int)((py + pd) / obj_spacing);
                        int obj_type = ((world_idx * 2654435761u) >> 16) % 6;
                        /* 0=post, 1=tree, 2=tree(tall), 3=building, 4=sign, 5=billboard */

                        int side;
                        for (side = 0; side < 2; side++) {
                            float wx = side ? (ROAD_HW + 3.0f) : (-ROAD_HW - 3.0f);
                            float sx_obj = PROJ_X(wx, pd) + pco;

                            if (obj_type == 0) {
                                /* Grey post / guardrail */
                                float pw = 0.4f * sc, ph = 3.5f * sc;
                                if (pw < 0.5f) pw = 0.5f;
                                DWORD c = 0xFFAAAAAA;
                                RHW_VERT v[6] = {
                                    {sx_obj-pw, sy_base-ph, 0.5f, 1.0f, c},
                                    {sx_obj+pw, sy_base-ph, 0.5f, 1.0f, c},
                                    {sx_obj-pw, sy_base,    0.5f, 1.0f, c},
                                    {sx_obj+pw, sy_base-ph, 0.5f, 1.0f, c},
                                    {sx_obj+pw, sy_base,    0.5f, 1.0f, c},
                                    {sx_obj-pw, sy_base,    0.5f, 1.0f, c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, v, sizeof(RHW_VERT));
                            } else if (obj_type == 1 || obj_type == 2) {
                                /* Tree: brown trunk + green triangle canopy */
                                float tw = 0.4f * sc, th = (obj_type == 2 ? 8.0f : 5.0f) * sc;
                                float cw = (obj_type == 2 ? 3.5f : 2.5f) * sc;
                                float ch = (obj_type == 2 ? 5.0f : 3.5f) * sc;
                                if (tw < 0.4f) tw = 0.4f;
                                DWORD trunk_c = 0xFF443322;
                                DWORD leaf_c = (world_idx & 1) ? 0xFF227733 : 0xFF2D8844;
                                /* Trunk */
                                RHW_VERT tr[6] = {
                                    {sx_obj-tw, sy_base-th+ch, 0.5f, 1.0f, trunk_c},
                                    {sx_obj+tw, sy_base-th+ch, 0.5f, 1.0f, trunk_c},
                                    {sx_obj-tw, sy_base,       0.5f, 1.0f, trunk_c},
                                    {sx_obj+tw, sy_base-th+ch, 0.5f, 1.0f, trunk_c},
                                    {sx_obj+tw, sy_base,       0.5f, 1.0f, trunk_c},
                                    {sx_obj-tw, sy_base,       0.5f, 1.0f, trunk_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, tr, sizeof(RHW_VERT));
                                /* Canopy (triangle) */
                                RHW_VERT cn[3] = {
                                    {sx_obj-cw, sy_base-th+ch, 0.48f, 1.0f, leaf_c},
                                    {sx_obj+cw, sy_base-th+ch, 0.48f, 1.0f, leaf_c},
                                    {sx_obj,    sy_base-th,    0.48f, 1.0f, leaf_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 1, cn, sizeof(RHW_VERT));
                            } else if (obj_type == 3) {
                                /* Building: 3D box with front face + side wall + roof top */
                                float b3w = 2.5f * sc, b3h = 6.0f * sc;
                                if (b3w < 1.0f) b3w = 1.0f;
                                DWORD bld_colors[4] = {0xFF556677, 0xFF665544, 0xFF554466, 0xFF446655};
                                DWORD bc = bld_colors[world_idx & 3];
                                /* Darken for side wall */
                                DWORD bc_side = (bc & 0xFF000000) |
                                    (((bc >> 16) & 0xFF) / 2 << 16) |
                                    (((bc >> 8) & 0xFF) / 2 << 8) |
                                    ((bc & 0xFF) / 2);
                                /* Lighten for roof top */
                                DWORD bc_roof = (bc & 0xFF000000) |
                                    ((((bc >> 16) & 0xFF) + 30 > 255 ? 255 : ((bc >> 16) & 0xFF) + 30) << 16) |
                                    ((((bc >> 8) & 0xFF) + 30 > 255 ? 255 : ((bc >> 8) & 0xFF) + 30) << 8) |
                                    (((bc & 0xFF) + 30 > 255 ? 255 : (bc & 0xFF) + 30));
                                /* Front face */
                                RHW_VERT bv[6] = {
                                    {sx_obj-b3w, sy_base-b3h, 0.5f, 1.0f, bc},
                                    {sx_obj+b3w, sy_base-b3h, 0.5f, 1.0f, bc},
                                    {sx_obj-b3w, sy_base,     0.5f, 1.0f, bc},
                                    {sx_obj+b3w, sy_base-b3h, 0.5f, 1.0f, bc},
                                    {sx_obj+b3w, sy_base,     0.5f, 1.0f, bc},
                                    {sx_obj-b3w, sy_base,     0.5f, 1.0f, bc},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, bv, sizeof(RHW_VERT));
                                /* Side wall (perspective depth - extends toward vanishing pt) */
                                float depth_w = b3w * 0.6f;
                                float depth_shrink = 0.85f; /* far edge smaller */
                                int bld_side = (side == 0) ? 1 : 0; /* inner side visible */
                                float side_x = bld_side ? (sx_obj + b3w) : (sx_obj - b3w);
                                float side_dir = bld_side ? 1.0f : -1.0f;
                                RHW_VERT sw[6] = {
                                    {side_x, sy_base-b3h, 0.51f, 1.0f, bc_side},
                                    {side_x+depth_w*side_dir, sy_base-b3h*depth_shrink, 0.51f, 1.0f, bc_side},
                                    {side_x, sy_base, 0.51f, 1.0f, bc_side},
                                    {side_x+depth_w*side_dir, sy_base-b3h*depth_shrink, 0.51f, 1.0f, bc_side},
                                    {side_x+depth_w*side_dir, sy_base, 0.51f, 1.0f, bc_side},
                                    {side_x, sy_base, 0.51f, 1.0f, bc_side},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, sw, sizeof(RHW_VERT));
                                /* Roof top (extends backward into distance) */
                                float roof_d = b3h * 0.15f; /* small roof depth */
                                RHW_VERT rt[6] = {
                                    {sx_obj-b3w, sy_base-b3h, 0.49f, 1.0f, bc_roof},
                                    {sx_obj+b3w, sy_base-b3h, 0.49f, 1.0f, bc_roof},
                                    {sx_obj-b3w*0.9f, sy_base-b3h-roof_d, 0.49f, 1.0f, bc_roof},
                                    {sx_obj+b3w, sy_base-b3h, 0.49f, 1.0f, bc_roof},
                                    {sx_obj+b3w*0.9f, sy_base-b3h-roof_d, 0.49f, 1.0f, bc_roof},
                                    {sx_obj-b3w*0.9f, sy_base-b3h-roof_d, 0.49f, 1.0f, bc_roof},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, rt, sizeof(RHW_VERT));
                                /* Window rows (two rows for more detail) */
                                DWORD win_c = 0xFFAABBDD;
                                float ww = b3w * 0.6f, wh_w = b3h * 0.1f;
                                float wy1 = sy_base - b3h * 0.35f;
                                float wy2 = sy_base - b3h * 0.65f;
                                RHW_VERT wv1[6] = {
                                    {sx_obj-ww, wy1-wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj+ww, wy1-wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj-ww, wy1+wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj+ww, wy1-wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj+ww, wy1+wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj-ww, wy1+wh_w, 0.48f, 1.0f, win_c},
                                };
                                RHW_VERT wv2[6] = {
                                    {sx_obj-ww, wy2-wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj+ww, wy2-wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj-ww, wy2+wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj+ww, wy2-wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj+ww, wy2+wh_w, 0.48f, 1.0f, win_c},
                                    {sx_obj-ww, wy2+wh_w, 0.48f, 1.0f, win_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, wv1, sizeof(RHW_VERT));
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, wv2, sizeof(RHW_VERT));
                            } else if (obj_type == 4) {
                                /* Road sign: thin post with small square sign on top */
                                float pw = 0.3f * sc, ph = 5.0f * sc;
                                float sw_s = 1.8f * sc, sh_s = 1.5f * sc;
                                if (pw < 0.3f) pw = 0.3f;
                                DWORD post_c = 0xFF888888;
                                /* Post */
                                RHW_VERT sp[6] = {
                                    {sx_obj-pw, sy_base-ph+sh_s, 0.5f, 1.0f, post_c},
                                    {sx_obj+pw, sy_base-ph+sh_s, 0.5f, 1.0f, post_c},
                                    {sx_obj-pw, sy_base,         0.5f, 1.0f, post_c},
                                    {sx_obj+pw, sy_base-ph+sh_s, 0.5f, 1.0f, post_c},
                                    {sx_obj+pw, sy_base,         0.5f, 1.0f, post_c},
                                    {sx_obj-pw, sy_base,         0.5f, 1.0f, post_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, sp, sizeof(RHW_VERT));
                                /* Sign face (blue with white border) */
                                DWORD sign_c = 0xFF2244AA;
                                RHW_VERT sf[6] = {
                                    {sx_obj-sw_s, sy_base-ph,     0.48f, 1.0f, sign_c},
                                    {sx_obj+sw_s, sy_base-ph,     0.48f, 1.0f, sign_c},
                                    {sx_obj-sw_s, sy_base-ph+sh_s,0.48f, 1.0f, sign_c},
                                    {sx_obj+sw_s, sy_base-ph,     0.48f, 1.0f, sign_c},
                                    {sx_obj+sw_s, sy_base-ph+sh_s,0.48f, 1.0f, sign_c},
                                    {sx_obj-sw_s, sy_base-ph+sh_s,0.48f, 1.0f, sign_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, sf, sizeof(RHW_VERT));
                                /* White arrow on sign */
                                DWORD arrow_c = 0xFFFFFFFF;
                                float aw = sw_s * 0.3f;
                                float ay = sy_base - ph + sh_s * 0.5f;
                                RHW_VERT arrow[3] = {
                                    {sx_obj - aw, ay + aw*0.5f, 0.47f, 1.0f, arrow_c},
                                    {sx_obj - aw, ay - aw*0.5f, 0.47f, 1.0f, arrow_c},
                                    {sx_obj + aw, ay,           0.47f, 1.0f, arrow_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 1, arrow, sizeof(RHW_VERT));
                            } else if (obj_type == 5) {
                                /* Billboard: tall wide panel on two posts */
                                float bw = 3.5f * sc, bh = 2.5f * sc;
                                float bph = 4.0f * sc;
                                float bpw = 0.3f * sc;
                                if (bpw < 0.3f) bpw = 0.3f;
                                DWORD bp_c = 0xFF666666;
                                /* Left post */
                                RHW_VERT bp_l[6] = {
                                    {sx_obj-bw+bpw, sy_base-bph+bh, 0.5f, 1.0f, bp_c},
                                    {sx_obj-bw+bpw*3, sy_base-bph+bh, 0.5f, 1.0f, bp_c},
                                    {sx_obj-bw+bpw, sy_base,         0.5f, 1.0f, bp_c},
                                    {sx_obj-bw+bpw*3, sy_base-bph+bh, 0.5f, 1.0f, bp_c},
                                    {sx_obj-bw+bpw*3, sy_base,         0.5f, 1.0f, bp_c},
                                    {sx_obj-bw+bpw, sy_base,         0.5f, 1.0f, bp_c},
                                };
                                /* Right post */
                                RHW_VERT bp_r[6] = {
                                    {sx_obj+bw-bpw*3, sy_base-bph+bh, 0.5f, 1.0f, bp_c},
                                    {sx_obj+bw-bpw, sy_base-bph+bh, 0.5f, 1.0f, bp_c},
                                    {sx_obj+bw-bpw*3, sy_base,         0.5f, 1.0f, bp_c},
                                    {sx_obj+bw-bpw, sy_base-bph+bh, 0.5f, 1.0f, bp_c},
                                    {sx_obj+bw-bpw, sy_base,         0.5f, 1.0f, bp_c},
                                    {sx_obj+bw-bpw*3, sy_base,         0.5f, 1.0f, bp_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, bp_l, sizeof(RHW_VERT));
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, bp_r, sizeof(RHW_VERT));
                                /* Billboard face: colored panel */
                                DWORD bb_colors[4] = {0xFF884422, 0xFF226644, 0xFF443388, 0xFF886622};
                                DWORD bb_c = bb_colors[world_idx & 3];
                                RHW_VERT bb[6] = {
                                    {sx_obj-bw, sy_base-bph,    0.48f, 1.0f, bb_c},
                                    {sx_obj+bw, sy_base-bph,    0.48f, 1.0f, bb_c},
                                    {sx_obj-bw, sy_base-bph+bh, 0.48f, 1.0f, bb_c},
                                    {sx_obj+bw, sy_base-bph,    0.48f, 1.0f, bb_c},
                                    {sx_obj+bw, sy_base-bph+bh, 0.48f, 1.0f, bb_c},
                                    {sx_obj-bw, sy_base-bph+bh, 0.48f, 1.0f, bb_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, bb, sizeof(RHW_VERT));
                                /* White text stripe across billboard */
                                DWORD txt_c = 0xFFDDDDDD;
                                float tw_h = bh * 0.2f;
                                float ty_b = sy_base - bph + bh * 0.4f;
                                RHW_VERT txt[6] = {
                                    {sx_obj-bw*0.8f, ty_b,      0.47f, 1.0f, txt_c},
                                    {sx_obj+bw*0.8f, ty_b,      0.47f, 1.0f, txt_c},
                                    {sx_obj-bw*0.8f, ty_b+tw_h, 0.47f, 1.0f, txt_c},
                                    {sx_obj+bw*0.8f, ty_b,      0.47f, 1.0f, txt_c},
                                    {sx_obj+bw*0.8f, ty_b+tw_h, 0.47f, 1.0f, txt_c},
                                    {sx_obj-bw*0.8f, ty_b+tw_h, 0.47f, 1.0f, txt_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, txt, sizeof(RHW_VERT));
                            }
                        }
                    }
                }

                /* ── Tunnel sections (periodic dark overhead) ───────── */
                {
                    /* Tunnel every 2000 world units, lasting 200 units.
                     * When player is inside a tunnel segment, render dark
                     * ceiling and walls for segments within the tunnel zone. */
                    float tunnel_period = 2000.0f;
                    float tunnel_len = 200.0f;
                    float tunnel_phase = fmodf(py, tunnel_period);
                    if (tunnel_phase < 0.0f) tunnel_phase += tunnel_period;
                    int in_tunnel = (tunnel_phase > tunnel_period - tunnel_len);

                    if (in_tunnel) {
                        /* Render ceiling and walls for road segments inside the tunnel */
                        int ti;
                        for (ti = 0; ti < ROAD_SEGS; ti++) {
                            float t0 = (float)ti / ROAD_SEGS;
                            float t1 = (float)(ti + 1) / ROAD_SEGS;
                            float d0 = 2.0f + t0 * t0 * VIEW_DIST;
                            float d1 = 2.0f + t1 * t1 * VIEW_DIST;
                            /* Check if this segment's world Y is inside the tunnel */
                            float seg_wy = py + (d0 + d1) * 0.5f;
                            float seg_tp = fmodf(seg_wy, tunnel_period);
                            if (seg_tp < 0.0f) seg_tp += tunnel_period;
                            if (seg_tp <= tunnel_period - tunnel_len) continue;

                            float ho0 = hill_offsets[ti], ho1 = hill_offsets[ti + 1];
                            float co0 = curve_offsets[ti], co1 = curve_offsets[ti + 1];
                            float y0 = PROJ_Y(d0) - ho0;
                            float y1 = PROJ_Y(d1) - ho1;
                            if (y0 < HORIZON - 30.0f || y1 > SH) continue;
                            if (y0 > SH) y0 = SH;

                            float lx0 = PROJ_X(-ROAD_HW - 2.0f, d0) + co0;
                            float rx0 = PROJ_X(ROAD_HW + 2.0f, d0) + co0;
                            float lx1 = PROJ_X(-ROAD_HW - 2.0f, d1) + co1;
                            float rx1 = PROJ_X(ROAD_HW + 2.0f, d1) + co1;

                            /* Ceiling height above road level */
                            float ceil_h0 = 8.0f * PROJ_SCALE(d0);
                            float ceil_h1 = 8.0f * PROJ_SCALE(d1);

                            DWORD tun_dark = 0xFF181822;
                            DWORD tun_wall = 0xFF252535;

                            /* Ceiling */
                            RHW_VERT ceil_v[6] = {
                                {lx0, y0 - ceil_h0, 0.42f, 1.0f, tun_dark},
                                {rx0, y0 - ceil_h0, 0.42f, 1.0f, tun_dark},
                                {lx1, y1 - ceil_h1, 0.42f, 1.0f, tun_dark},
                                {rx0, y0 - ceil_h0, 0.42f, 1.0f, tun_dark},
                                {rx1, y1 - ceil_h1, 0.42f, 1.0f, tun_dark},
                                {lx1, y1 - ceil_h1, 0.42f, 1.0f, tun_dark},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, ceil_v, sizeof(RHW_VERT));

                            /* Left wall */
                            RHW_VERT lwall[6] = {
                                {lx0, y0 - ceil_h0, 0.43f, 1.0f, tun_wall},
                                {lx0, y0,           0.43f, 1.0f, tun_wall},
                                {lx1, y1 - ceil_h1, 0.43f, 1.0f, tun_wall},
                                {lx0, y0,           0.43f, 1.0f, tun_wall},
                                {lx1, y1,           0.43f, 1.0f, tun_wall},
                                {lx1, y1 - ceil_h1, 0.43f, 1.0f, tun_wall},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, lwall, sizeof(RHW_VERT));

                            /* Right wall */
                            RHW_VERT rwall[6] = {
                                {rx0, y0 - ceil_h0, 0.43f, 1.0f, tun_wall},
                                {rx0, y0,           0.43f, 1.0f, tun_wall},
                                {rx1, y1 - ceil_h1, 0.43f, 1.0f, tun_wall},
                                {rx0, y0,           0.43f, 1.0f, tun_wall},
                                {rx1, y1,           0.43f, 1.0f, tun_wall},
                                {rx1, y1 - ceil_h1, 0.43f, 1.0f, tun_wall},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, rwall, sizeof(RHW_VERT));

                            /* Tunnel lights: orange strip on ceiling every few segments */
                            if ((ti & 3) == 0) {
                                float lw = 0.5f * PROJ_SCALE(d0);
                                float lamp_x = PROJ_X(0.0f, d0) + co0;
                                DWORD lamp_c = 0xFFFFAA44;
                                RHW_VERT lamp[6] = {
                                    {lamp_x - lw * 3.0f, y0 - ceil_h0,        0.41f, 1.0f, lamp_c},
                                    {lamp_x + lw * 3.0f, y0 - ceil_h0,        0.41f, 1.0f, lamp_c},
                                    {lamp_x - lw * 3.0f, y0 - ceil_h0 + lw,   0.41f, 1.0f, lamp_c},
                                    {lamp_x + lw * 3.0f, y0 - ceil_h0,        0.41f, 1.0f, lamp_c},
                                    {lamp_x + lw * 3.0f, y0 - ceil_h0 + lw,   0.41f, 1.0f, lamp_c},
                                    {lamp_x - lw * 3.0f, y0 - ceil_h0 + lw,   0.41f, 1.0f, lamp_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, lamp, sizeof(RHW_VERT));
                            }
                        }
                    }
                }

                /* ── Traffic obstacles (perspective projected) ───────── */
                {
                    #define OBS_BASE   0x5FFE00
                    #define OBS_COUNT  12
                    #define OBS_SIZE   16
                    #define OBS_ADDR(i, off) (OBS_BASE + (i) * OBS_SIZE + (off))
                    /* Same-dir: warm colors, Oncoming: cool/bright colors */
                    DWORD obs_colors[4] = { 0xFFFF6633, 0xFF33CCFF, 0xFFFFCC33, 0xFF66FF66 };
                    DWORD onc_colors[4] = { 0xFFFF2244, 0xFFDD44FF, 0xFFFF8800, 0xFFFF4466 };
                    int oi;
                    for (oi = 0; oi < OBS_COUNT; oi++) {
                        uint32_t flags = _R_MEM32(OBS_ADDR(oi, 0xC));
                        if ((flags & 1) == 0) continue;
                        float ox = _R_MEMF(OBS_ADDR(oi, 0));
                        float oy = _R_MEMF(OBS_ADDR(oi, 4));

                        /* Distance ahead of camera */
                        float dist = oy - py;
                        if (dist < 1.0f || dist > VIEW_DIST) continue;

                        /* Interpolate curve and hill offsets for this distance. */
                        float obs_co = 0.0f, obs_ho = 0.0f;
                        {
                            /* Inverse of d = 2 + t*t*VIEW_DIST → t = sqrt((d-2)/VIEW_DIST) */
                            float t_obs = 0.0f;
                            if (dist > 2.0f)
                                t_obs = sqrtf((dist - 2.0f) / VIEW_DIST);
                            if (t_obs > 1.0f) t_obs = 1.0f;
                            float seg_f = t_obs * ROAD_SEGS;
                            int seg_i = (int)seg_f;
                            if (seg_i >= ROAD_SEGS) seg_i = ROAD_SEGS - 1;
                            float frac = seg_f - (float)seg_i;
                            obs_co = curve_offsets[seg_i] * (1.0f - frac) + curve_offsets[seg_i + 1] * frac;
                            obs_ho = hill_offsets[seg_i] * (1.0f - frac) + hill_offsets[seg_i + 1] * frac;
                        }

                        /* Project to screen */
                        float sx = PROJ_X(ox, dist) + obs_co;
                        float sy = PROJ_Y(dist) - obs_ho;
                        float scale = PROJ_SCALE(dist);

                        /* Cull off-screen */
                        if (sx < -40.0f || sx > SW+40.0f || sy < HORIZON || sy > SH) continue;

                        /* 3D car model: body + cabin + roof + side panels */
                        int is_oncoming = (flags & 2) != 0;
                        float ohw = 1.6f * scale;  /* body half-width */
                        float obh = 1.2f * scale;  /* body height (lower section) */
                        float och = 0.8f * scale;  /* cabin height (upper section) */
                        float orf = 1.0f * scale;  /* roof depth (forward extension) */
                        DWORD oc = is_oncoming ? onc_colors[oi & 3] : obs_colors[oi & 3];
                        /* Darken color for side panels */
                        DWORD oc_dark = (oc & 0xFF000000) |
                            (((oc >> 16) & 0xFF) * 2/3 << 16) |
                            (((oc >> 8) & 0xFF) * 2/3 << 8) |
                            ((oc & 0xFF) * 2/3);
                        /* Lighten for roof */
                        DWORD oc_light = (oc & 0xFF000000) |
                            ((((oc >> 16) & 0xFF) + 40 > 255 ? 255 : ((oc >> 16) & 0xFF) + 40) << 16) |
                            ((((oc >> 8) & 0xFF) + 40 > 255 ? 255 : ((oc >> 8) & 0xFF) + 40) << 8) |
                            (((oc & 0xFF) + 40 > 255 ? 255 : (oc & 0xFF) + 40));

                        float o_belt = sy - obh;         /* beltline */
                        float o_roof = o_belt - och;     /* top of glass */
                        float o_hood = o_roof - orf;     /* front of roof */
                        float o_cw = ohw * 0.82f;        /* cabin narrower */

                        /* Shadow under car (textured if available) */
                        if (g_tex_shadow) {
                            float shw = ohw * 1.4f, shh = obh * 0.25f;
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 1);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 19, 5);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 20, 6);
                            draw_textured_quad(g_d3d_device, g_tex_shadow,
                                sx - shw, sy - shh, sx + shw, sy + shh,
                                0.42f, 0x80000000);
                            restore_untextured(g_d3d_device);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 0);
                        }

                        /* Traffic car body: 3D model or fallback procedural */
                        {
                            int tmi = oi % TRAFFIC_MODEL_COUNT;
                            BGV_Model *tm = &g_traffic_models[tmi];
                            if (g_traffic_models_loaded > 0 && tm->vertices && tm->index_count > 0) {
                                /* Render 3D model at this traffic car's screen position */
                                DWORD fvf_3d = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
                                float tr = tm->bounding_radius;

                                /* World: rotate to face camera (same-dir shows rear, oncoming shows front) */
                                D3DMATRIX tw;
                                float trot = is_oncoming ? 3.14159f : 0.0f;
                                /* Add slight angle based on lane position */
                                trot += (sx - CX) * 0.001f;
                                mat4_rotation_y((float *)&tw, trot);

                                /* View: camera behind and above */
                                D3DMATRIX tv;
                                mat4_lookat((float *)&tv,
                                    0.0f, tr * 0.8f, -tr * 2.2f,
                                    0.0f, tr * 0.15f, 0.0f,
                                    0.0f, 1.0f, 0.0f);

                                /* Projection with off-center shift to match screen position */
                                D3DMATRIX tp;
                                mat4_perspective((float *)&tp,
                                    32.0f * 3.14159f / 180.0f,
                                    SW / SH, 0.1f, 50.0f);
                                /* NDC position: sx,sy → NDC */
                                float ndc_x = (sx / (SW * 0.5f)) - 1.0f;
                                float ndc_y = 1.0f - (sy / (SH * 0.5f));
                                ((float *)&tp)[8] = ndc_x;
                                ((float *)&tp)[9] = ndc_y;
                                /* Scale model to match pseudo-3D apparent size.
                                 * Player car at scale~1 occupies ~48px (ohw=24).
                                 * This traffic car has ohw = 1.6*scale pixels.
                                 * Scale projection f factor proportionally. */
                                float size_ratio = (ohw * 2.0f) / 48.0f;
                                ((float *)&tp)[0] *= size_ratio;
                                ((float *)&tp)[5] *= size_ratio;

                                g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_WORLD, &tw);
                                g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_VIEW, &tv);
                                g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_PROJECTION, &tp);

                                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_ZENABLE, TRUE);
                                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_ZWRITEENABLE, TRUE);
                                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_LIGHTING, FALSE);
                                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_CULLMODE, D3DCULL_NONE);
                                g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0, NULL);
                                g_d3d_device->lpVtbl->SetTextureStageState(g_d3d_device, 0, 1, 1);

                                g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device, fvf_3d);
                                g_d3d_device->lpVtbl->SetStreamSource(g_d3d_device, 0, NULL, 0);
                                g_d3d_device->lpVtbl->SetIndices(g_d3d_device, NULL, 0);

                                g_d3d_device->lpVtbl->DrawIndexedPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST,
                                    0, tm->vertex_count, tm->index_count / 3,
                                    tm->indices, D3DFMT_INDEX16,
                                    tm->vertices, sizeof(BGV_Vertex));

                                /* Restore screen-space state */
                                D3DMATRIX ident;
                                mat4_identity((float *)&ident);
                                g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_WORLD, &ident);
                                g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_VIEW, &ident);
                                g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_PROJECTION, &ident);
                                g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_ZENABLE, FALSE);
                            } else {
                                /* Fallback: procedural colored rectangles */
                                RHW_VERT obody[6] = {
                                    {sx-ohw, o_belt, 0.40f, 1.0f, oc},
                                    {sx+ohw, o_belt, 0.40f, 1.0f, oc},
                                    {sx-ohw, sy,     0.40f, 1.0f, oc_dark},
                                    {sx+ohw, o_belt, 0.40f, 1.0f, oc},
                                    {sx+ohw, sy,     0.40f, 1.0f, oc_dark},
                                    {sx-ohw, sy,     0.40f, 1.0f, oc_dark},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, obody, sizeof(RHW_VERT));
                                RHW_VERT ocab[6] = {
                                    {sx-o_cw, o_roof, 0.38f, 1.0f, oc},
                                    {sx+o_cw, o_roof, 0.38f, 1.0f, oc},
                                    {sx-o_cw, o_belt, 0.38f, 1.0f, oc},
                                    {sx+o_cw, o_roof, 0.38f, 1.0f, oc},
                                    {sx+o_cw, o_belt, 0.38f, 1.0f, oc},
                                    {sx-o_cw, o_belt, 0.38f, 1.0f, oc},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, ocab, sizeof(RHW_VERT));
                                {
                                    float rfw = o_cw * 0.85f;
                                    RHW_VERT oroof[6] = {
                                        {sx-rfw, o_hood,  0.36f, 1.0f, oc_light},
                                        {sx+rfw, o_hood,  0.36f, 1.0f, oc_light},
                                        {sx-o_cw, o_roof, 0.36f, 1.0f, oc_light},
                                        {sx+rfw, o_hood,  0.36f, 1.0f, oc_light},
                                        {sx+o_cw, o_roof, 0.36f, 1.0f, oc_light},
                                        {sx-o_cw, o_roof, 0.36f, 1.0f, oc_light},
                                    };
                                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                        D3DPT_TRIANGLELIST, 2, oroof, sizeof(RHW_VERT));
                                }
                                {
                                    DWORD wc = 0xFF202040;
                                    float gw = o_cw * 0.75f;
                                    RHW_VERT oglass[6] = {
                                        {sx-gw, o_roof+1.0f*scale, 0.37f, 1.0f, wc},
                                        {sx+gw, o_roof+1.0f*scale, 0.37f, 1.0f, wc},
                                        {sx-gw, o_belt-0.5f*scale, 0.37f, 1.0f, wc},
                                        {sx+gw, o_roof+1.0f*scale, 0.37f, 1.0f, wc},
                                        {sx+gw, o_belt-0.5f*scale, 0.37f, 1.0f, wc},
                                        {sx-gw, o_belt-0.5f*scale, 0.37f, 1.0f, wc},
                                    };
                                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                        D3DPT_TRIANGLELIST, 2, oglass, sizeof(RHW_VERT));
                                }
                                {
                                    float side_bias = (sx - CX) / (SW * 0.5f);
                                    float side_w = 1.5f * scale * (1.0f + 0.5f * (side_bias > 0 ? side_bias : -side_bias));
                                    if (side_bias < -0.05f) {
                                        RHW_VERT oside[6] = {
                                            {sx+ohw, o_roof,  0.41f, 1.0f, oc_dark},
                                            {sx+ohw+side_w, o_roof, 0.41f, 1.0f, oc_dark},
                                            {sx+ohw, sy,      0.41f, 1.0f, oc_dark},
                                            {sx+ohw+side_w, o_roof, 0.41f, 1.0f, oc_dark},
                                            {sx+ohw+side_w, sy,     0.41f, 1.0f, oc_dark},
                                            {sx+ohw, sy,      0.41f, 1.0f, oc_dark},
                                        };
                                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                            D3DPT_TRIANGLELIST, 2, oside, sizeof(RHW_VERT));
                                    } else if (side_bias > 0.05f) {
                                        RHW_VERT oside[6] = {
                                            {sx-ohw-side_w, o_roof, 0.41f, 1.0f, oc_dark},
                                            {sx-ohw, o_roof,        0.41f, 1.0f, oc_dark},
                                            {sx-ohw-side_w, sy,     0.41f, 1.0f, oc_dark},
                                            {sx-ohw, o_roof,        0.41f, 1.0f, oc_dark},
                                            {sx-ohw, sy,            0.41f, 1.0f, oc_dark},
                                            {sx-ohw-side_w, sy,     0.41f, 1.0f, oc_dark},
                                        };
                                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                            D3DPT_TRIANGLELIST, 2, oside, sizeof(RHW_VERT));
                                    }
                                }
                            }
                        }
                        /* Taillights on same-direction cars */
                        if (!is_oncoming) {
                            DWORD tl = 0xFFFF2222;
                            float tlw = ohw * 0.2f, tlh = obh * 0.15f;
                            if (tlw < 0.5f) tlw = 0.5f;
                            RHW_VERT tl_l[6] = {
                                {sx-ohw*0.7f-tlw, sy-tlh, 0.32f, 1.0f, tl},
                                {sx-ohw*0.7f+tlw, sy-tlh, 0.32f, 1.0f, tl},
                                {sx-ohw*0.7f-tlw, sy+tlh, 0.32f, 1.0f, tl},
                                {sx-ohw*0.7f+tlw, sy-tlh, 0.32f, 1.0f, tl},
                                {sx-ohw*0.7f+tlw, sy+tlh, 0.32f, 1.0f, tl},
                                {sx-ohw*0.7f-tlw, sy+tlh, 0.32f, 1.0f, tl},
                            };
                            RHW_VERT tl_r[6] = {
                                {sx+ohw*0.7f-tlw, sy-tlh, 0.32f, 1.0f, tl},
                                {sx+ohw*0.7f+tlw, sy-tlh, 0.32f, 1.0f, tl},
                                {sx+ohw*0.7f-tlw, sy+tlh, 0.32f, 1.0f, tl},
                                {sx+ohw*0.7f+tlw, sy-tlh, 0.32f, 1.0f, tl},
                                {sx+ohw*0.7f+tlw, sy+tlh, 0.32f, 1.0f, tl},
                                {sx+ohw*0.7f-tlw, sy+tlh, 0.32f, 1.0f, tl},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, tl_l, sizeof(RHW_VERT));
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, tl_r, sizeof(RHW_VERT));
                        }
                        /* Headlights on oncoming cars */
                        if (is_oncoming) {
                            DWORD hl = 0xFFFFFF88;
                            float hlw = ohw * 0.25f, hlh = obh * 0.15f;
                            RHW_VERT hl_l[6] = {
                                {sx-ohw*0.6f-hlw, sy-hlh, 0.33f, 1.0f, hl},
                                {sx-ohw*0.6f+hlw, sy-hlh, 0.33f, 1.0f, hl},
                                {sx-ohw*0.6f-hlw, sy+hlh, 0.33f, 1.0f, hl},
                                {sx-ohw*0.6f+hlw, sy-hlh, 0.33f, 1.0f, hl},
                                {sx-ohw*0.6f+hlw, sy+hlh, 0.33f, 1.0f, hl},
                                {sx-ohw*0.6f-hlw, sy+hlh, 0.33f, 1.0f, hl},
                            };
                            RHW_VERT hl_r[6] = {
                                {sx+ohw*0.6f-hlw, sy-hlh, 0.33f, 1.0f, hl},
                                {sx+ohw*0.6f+hlw, sy-hlh, 0.33f, 1.0f, hl},
                                {sx+ohw*0.6f-hlw, sy+hlh, 0.33f, 1.0f, hl},
                                {sx+ohw*0.6f+hlw, sy-hlh, 0.33f, 1.0f, hl},
                                {sx+ohw*0.6f+hlw, sy+hlh, 0.33f, 1.0f, hl},
                                {sx+ohw*0.6f-hlw, sy+hlh, 0.33f, 1.0f, hl},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, hl_l, sizeof(RHW_VERT));
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, hl_r, sizeof(RHW_VERT));
                        }
                    }
                    #undef OBS_BASE
                    #undef OBS_COUNT
                    #undef OBS_SIZE
                    #undef OBS_ADDR
                }

                /* ── Player car (3D box model, viewed from behind-above) ── */
                {
                    float car_cx = CX;
                    float car_cy = SH - 55.0f; /* near bottom */
                    /* Steering offset: small horizontal shift */
                    car_cx += heading * 18.0f;
                    /* Steering tilt: skew top relative to bottom */
                    float car_tilt = heading * 12.0f;

                    /* Car dimensions (screen-space pixels) */
                    float bw = 24.0f;   /* body half-width */
                    float bh = 14.0f;   /* body height (rear face) */
                    float ch = 8.0f;    /* cabin height (glass area) */
                    float rw = 20.0f;   /* roof half-width (narrower) */
                    float rd = 10.0f;   /* roof depth forward (toward horizon) */
                    float hw_narrow = 18.0f; /* cabin narrower than fenders */

                    /* Y coordinates */
                    float y_bumper = car_cy + bh;       /* bottom of car */
                    float y_belt   = car_cy;            /* beltline (body/cabin join) */
                    float y_roof   = car_cy - ch;       /* top of rear glass */
                    float y_hood   = y_roof - rd;       /* front of roof (toward horizon) */

                    /* Shadow (ground plane) */
                    {
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 19, 5);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 20, 6);
                        DWORD sh_col = 0x60000000;
                        float shw = bw + 6.0f;
                        if (g_tex_shadow) {
                            draw_textured_quad(g_d3d_device, g_tex_shadow,
                                car_cx - shw, y_bumper - 2.0f,
                                car_cx + shw, y_bumper + 10.0f,
                                0.25f, sh_col);
                            restore_untextured(g_d3d_device);
                        } else {
                            RHW_VERT shadow[6] = {
                                {car_cx-shw+car_tilt*0.3f, y_bumper-2.0f,  0.25f, 1.0f, sh_col},
                                {car_cx+shw+car_tilt*0.3f, y_bumper-2.0f,  0.25f, 1.0f, sh_col},
                                {car_cx-shw,               y_bumper+10.0f, 0.25f, 1.0f, sh_col},
                                {car_cx+shw+car_tilt*0.3f, y_bumper-2.0f,  0.25f, 1.0f, sh_col},
                                {car_cx+shw,               y_bumper+10.0f, 0.25f, 1.0f, sh_col},
                                {car_cx-shw,               y_bumper+10.0f, 0.25f, 1.0f, sh_col},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, shadow, sizeof(RHW_VERT));
                        }
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }

                    /* ---- Car body: 3D model or fallback procedural ---- */
                    if (g_car_model_loaded) {
                        /* Clear Z-buffer so 3D model renders cleanly */
                        g_d3d_device->lpVtbl->Clear(g_d3d_device, 0, NULL,
                            D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

                        /* Render actual 3D car model in the pseudo-3D scene */
                        DWORD fvf_3d = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;

                        /* World matrix: rotate car by steering heading */
                        D3DMATRIX car_world;
                        mat4_rotation_y((float *)&car_world, heading * 0.3f);

                        /* View matrix: camera behind and above, looking at car rear */
                        D3DMATRIX car_view;
                        float r = g_car_model.bounding_radius;
                        mat4_lookat((float *)&car_view,
                                    0.0f, r * 0.8f, -r * 2.2f,  /* eye: behind+above */
                                    0.0f, r * 0.15f, 0.0f,       /* target: car center-low */
                                    0.0f, 1.0f, 0.0f);           /* up */

                        /* Projection with off-center shift to place car at bottom of screen.
                         * Target screen Y ~= 425/480 = NDC Y -0.77.
                         * m[9] shifts Y in NDC, m[8] shifts X in NDC. */
                        D3DMATRIX car_proj;
                        mat4_perspective((float *)&car_proj,
                                         32.0f * 3.14159f / 180.0f,
                                         SW / SH, 0.1f, 50.0f);
                        ((float *)&car_proj)[9] = -0.77f;
                        /* Horizontal shift for steering: heading*18px / 320 half-width */
                        ((float *)&car_proj)[8] = heading * (18.0f / 320.0f);

                        g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_WORLD, &car_world);
                        g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_VIEW, &car_view);
                        g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_PROJECTION, &car_proj);

                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_ZENABLE, TRUE);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_ZWRITEENABLE, TRUE);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_LIGHTING, FALSE);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_CULLMODE, D3DCULL_NONE);

                        /* Bind paint texture for player car */
                        if (g_paint_tex) {
                            g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0,
                                (IDirect3DBaseTexture8 *)g_paint_tex);
                        } else {
                            g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0, NULL);
                        }

                        g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device, fvf_3d);
                        g_d3d_device->lpVtbl->SetStreamSource(g_d3d_device, 0, NULL, 0);
                        g_d3d_device->lpVtbl->SetIndices(g_d3d_device, NULL, 0);

                        g_d3d_device->lpVtbl->DrawIndexedPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST,
                            0, g_car_model.vertex_count,
                            g_car_model.index_count / 3,
                            g_car_model.indices, D3DFMT_INDEX16,
                            g_car_model.vertices, sizeof(BGV_Vertex));

                        /* Unbind paint texture */
                        g_d3d_device->lpVtbl->SetTexture(g_d3d_device, 0, NULL);

                        /* Reset transforms back to identity for subsequent screen-space drawing */
                        D3DMATRIX ident;
                        mat4_identity((float *)&ident);
                        g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_WORLD, &ident);
                        g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_VIEW, &ident);
                        g_d3d_device->lpVtbl->SetTransform(g_d3d_device, D3DTS_PROJECTION, &ident);

                        /* Restore screen-space vertex format */
                        g_d3d_device->lpVtbl->SetVertexShader(g_d3d_device,
                            D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, D3DRS_ZENABLE, FALSE);
                    } else {
                    /* ---- Fallback: procedural car body ---- */
                    /* Lower body (rear face - fenders, bumper, trunk) */
                    {
                        DWORD body_main = 0xFFD0D0EE;
                        DWORD body_dark = 0xFFA0A0CC;
                        RHW_VERT body[6] = {
                            {car_cx-bw+car_tilt*0.5f, y_belt,   0.20f, 1.0f, body_main},
                            {car_cx+bw+car_tilt*0.5f, y_belt,   0.20f, 1.0f, body_main},
                            {car_cx-bw,               y_bumper, 0.20f, 1.0f, body_dark},
                            {car_cx+bw+car_tilt*0.5f, y_belt,   0.20f, 1.0f, body_main},
                            {car_cx+bw,               y_bumper, 0.20f, 1.0f, body_dark},
                            {car_cx-bw,               y_bumper, 0.20f, 1.0f, body_dark},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, body, sizeof(RHW_VERT));
                    }
                    {
                        DWORD bumper_c = 0xFF333344;
                        float bmp_h = 3.0f;
                        RHW_VERT bumper[6] = {
                            {car_cx-bw, y_bumper-bmp_h, 0.19f, 1.0f, bumper_c},
                            {car_cx+bw, y_bumper-bmp_h, 0.19f, 1.0f, bumper_c},
                            {car_cx-bw, y_bumper,       0.19f, 1.0f, bumper_c},
                            {car_cx+bw, y_bumper-bmp_h, 0.19f, 1.0f, bumper_c},
                            {car_cx+bw, y_bumper,       0.19f, 1.0f, bumper_c},
                            {car_cx-bw, y_bumper,       0.19f, 1.0f, bumper_c},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, bumper, sizeof(RHW_VERT));
                    }
                    {
                        DWORD cabin_c = 0xFFC0C0DD;
                        float t2 = car_tilt * 0.8f;
                        RHW_VERT cabin[6] = {
                            {car_cx-hw_narrow+car_tilt, y_roof, 0.18f, 1.0f, cabin_c},
                            {car_cx+hw_narrow+car_tilt, y_roof, 0.18f, 1.0f, cabin_c},
                            {car_cx-hw_narrow+t2,       y_belt, 0.18f, 1.0f, cabin_c},
                            {car_cx+hw_narrow+car_tilt, y_roof, 0.18f, 1.0f, cabin_c},
                            {car_cx+hw_narrow+t2,       y_belt, 0.18f, 1.0f, cabin_c},
                            {car_cx-hw_narrow+t2,       y_belt, 0.18f, 1.0f, cabin_c},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, cabin, sizeof(RHW_VERT));
                    }
                    {
                        DWORD glass_c = 0xFF303050;
                        float gw = hw_narrow * 0.75f;
                        float gt = car_tilt * 0.9f;
                        RHW_VERT glass[6] = {
                            {car_cx-gw+car_tilt, y_roof+2.0f,  0.17f, 1.0f, glass_c},
                            {car_cx+gw+car_tilt, y_roof+2.0f,  0.17f, 1.0f, glass_c},
                            {car_cx-gw+gt,       y_belt-2.0f,  0.17f, 1.0f, glass_c},
                            {car_cx+gw+car_tilt, y_roof+2.0f,  0.17f, 1.0f, glass_c},
                            {car_cx+gw+gt,       y_belt-2.0f,  0.17f, 1.0f, glass_c},
                            {car_cx-gw+gt,       y_belt-2.0f,  0.17f, 1.0f, glass_c},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, glass, sizeof(RHW_VERT));
                    }
                    {
                        DWORD roof_c = 0xFFE8E8FF;
                        DWORD roof_front = 0xFFD0D0EE;
                        float rf_narrow = rw * 0.85f;
                        RHW_VERT roof[6] = {
                            {car_cx-rf_narrow+car_tilt*1.3f, y_hood,  0.16f, 1.0f, roof_front},
                            {car_cx+rf_narrow+car_tilt*1.3f, y_hood,  0.16f, 1.0f, roof_front},
                            {car_cx-rw+car_tilt,             y_roof,  0.16f, 1.0f, roof_c},
                            {car_cx+rf_narrow+car_tilt*1.3f, y_hood,  0.16f, 1.0f, roof_front},
                            {car_cx+rw+car_tilt,             y_roof,  0.16f, 1.0f, roof_c},
                            {car_cx-rw+car_tilt,             y_roof,  0.16f, 1.0f, roof_c},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, roof, sizeof(RHW_VERT));
                    }
                    {
                        DWORD hood_c = 0xFFD8D8F0;
                        DWORD hood_far = 0xFFB0B0D0;
                        float hood_w = bw * 0.9f;
                        float hood_fw = bw * 0.6f;
                        float hood_end = y_hood - 5.0f;
                        RHW_VERT hood[6] = {
                            {car_cx-hood_fw+car_tilt*1.5f, hood_end, 0.22f, 1.0f, hood_far},
                            {car_cx+hood_fw+car_tilt*1.5f, hood_end, 0.22f, 1.0f, hood_far},
                            {car_cx-hood_w+car_tilt*1.2f,  y_hood,   0.22f, 1.0f, hood_c},
                            {car_cx+hood_fw+car_tilt*1.5f, hood_end, 0.22f, 1.0f, hood_far},
                            {car_cx+hood_w+car_tilt*1.2f,  y_hood,   0.22f, 1.0f, hood_c},
                            {car_cx-hood_w+car_tilt*1.2f,  y_hood,   0.22f, 1.0f, hood_c},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, hood, sizeof(RHW_VERT));
                    }
                    {
                        DWORD ws_c = 0xFF252545;
                        float ws_w = rw * 0.8f;
                        float ws_fw = rw * 0.65f;
                        RHW_VERT ws[6] = {
                            {car_cx-ws_fw+car_tilt*1.3f, y_hood,       0.155f, 1.0f, ws_c},
                            {car_cx+ws_fw+car_tilt*1.3f, y_hood,       0.155f, 1.0f, ws_c},
                            {car_cx-ws_w+car_tilt,       y_roof+1.0f,  0.155f, 1.0f, ws_c},
                            {car_cx+ws_fw+car_tilt*1.3f, y_hood,       0.155f, 1.0f, ws_c},
                            {car_cx+ws_w+car_tilt,       y_roof+1.0f,  0.155f, 1.0f, ws_c},
                            {car_cx-ws_w+car_tilt,       y_roof+1.0f,  0.155f, 1.0f, ws_c},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, ws, sizeof(RHW_VERT));
                    }
                    {
                        float side_depth = 4.0f;
                        float left_w  = side_depth + heading * 20.0f;
                        float right_w = side_depth - heading * 20.0f;
                        if (left_w < 1.0f) left_w = 1.0f;
                        if (right_w < 1.0f) right_w = 1.0f;
                        if (left_w > 14.0f) left_w = 14.0f;
                        if (right_w > 14.0f) right_w = 14.0f;
                        DWORD side_l = 0xFF8888AA;
                        DWORD side_r = 0xFF9090BB;
                        float lx = car_cx - bw;
                        RHW_VERT ls[12] = {
                            {lx - left_w+car_tilt*0.3f, y_belt,   0.21f, 1.0f, side_l},
                            {lx+car_tilt*0.5f,          y_belt,   0.21f, 1.0f, side_l},
                            {lx - left_w,               y_bumper, 0.21f, 1.0f, side_l},
                            {lx+car_tilt*0.5f,          y_belt,   0.21f, 1.0f, side_l},
                            {lx,                        y_bumper, 0.21f, 1.0f, side_l},
                            {lx - left_w,               y_bumper, 0.21f, 1.0f, side_l},
                            {lx - left_w*0.6f+car_tilt, y_roof,   0.175f, 1.0f, side_l},
                            {lx+car_tilt,               y_roof,   0.175f, 1.0f, side_l},
                            {lx - left_w*0.6f+car_tilt*0.8f, y_belt, 0.175f, 1.0f, side_l},
                            {lx+car_tilt,               y_roof,   0.175f, 1.0f, side_l},
                            {lx+car_tilt*0.8f,          y_belt,   0.175f, 1.0f, side_l},
                            {lx - left_w*0.6f+car_tilt*0.8f, y_belt, 0.175f, 1.0f, side_l},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 4, ls, sizeof(RHW_VERT));
                        float rx = car_cx + bw;
                        RHW_VERT rs[12] = {
                            {rx+car_tilt*0.5f,           y_belt,   0.21f, 1.0f, side_r},
                            {rx + right_w+car_tilt*0.3f, y_belt,   0.21f, 1.0f, side_r},
                            {rx,                         y_bumper, 0.21f, 1.0f, side_r},
                            {rx + right_w+car_tilt*0.3f, y_belt,   0.21f, 1.0f, side_r},
                            {rx + right_w,               y_bumper, 0.21f, 1.0f, side_r},
                            {rx,                         y_bumper, 0.21f, 1.0f, side_r},
                            {rx+car_tilt,               y_roof,   0.175f, 1.0f, side_r},
                            {rx + right_w*0.6f+car_tilt, y_roof,  0.175f, 1.0f, side_r},
                            {rx+car_tilt*0.8f,          y_belt,   0.175f, 1.0f, side_r},
                            {rx + right_w*0.6f+car_tilt, y_roof,  0.175f, 1.0f, side_r},
                            {rx + right_w*0.6f+car_tilt*0.8f, y_belt, 0.175f, 1.0f, side_r},
                            {rx+car_tilt*0.8f,          y_belt,   0.175f, 1.0f, side_r},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 4, rs, sizeof(RHW_VERT));
                    }
                    } /* end else (fallback procedural car) */

                    /* ---- Taillights (on bumper) ---- */
                    {
                        DWORD tail_col = 0xFFFF2222;
                        float tw = 5.0f, th = 3.0f;
                        float tl_y = y_bumper - th;
                        RHW_VERT tl_l[6] = {
                            {car_cx-bw+2, tl_y,     0.10f, 1.0f, tail_col},
                            {car_cx-bw+2+tw, tl_y,  0.10f, 1.0f, tail_col},
                            {car_cx-bw+2, y_bumper, 0.10f, 1.0f, tail_col},
                            {car_cx-bw+2+tw, tl_y,  0.10f, 1.0f, tail_col},
                            {car_cx-bw+2+tw, y_bumper, 0.10f, 1.0f, tail_col},
                            {car_cx-bw+2, y_bumper, 0.10f, 1.0f, tail_col},
                        };
                        RHW_VERT tl_r[6] = {
                            {car_cx+bw-2-tw, tl_y,     0.10f, 1.0f, tail_col},
                            {car_cx+bw-2, tl_y,        0.10f, 1.0f, tail_col},
                            {car_cx+bw-2-tw, y_bumper, 0.10f, 1.0f, tail_col},
                            {car_cx+bw-2, tl_y,        0.10f, 1.0f, tail_col},
                            {car_cx+bw-2, y_bumper,    0.10f, 1.0f, tail_col},
                            {car_cx+bw-2-tw, y_bumper, 0.10f, 1.0f, tail_col},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, tl_l, sizeof(RHW_VERT));
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, tl_r, sizeof(RHW_VERT));
                        /* Taillight glow (alpha blended, slightly larger) */
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 19, 5);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 20, 6);
                        DWORD glow = 0x40FF0000;
                        float glw = tw + 3.0f;
                        RHW_VERT gl_l[6] = {
                            {car_cx-bw+2-1, tl_y-1,       0.09f, 1.0f, glow},
                            {car_cx-bw+2+glw, tl_y-1,     0.09f, 1.0f, glow},
                            {car_cx-bw+2-1, y_bumper+1,   0.09f, 1.0f, glow},
                            {car_cx-bw+2+glw, tl_y-1,     0.09f, 1.0f, glow},
                            {car_cx-bw+2+glw, y_bumper+1,  0.09f, 1.0f, glow},
                            {car_cx-bw+2-1, y_bumper+1,   0.09f, 1.0f, glow},
                        };
                        RHW_VERT gl_r[6] = {
                            {car_cx+bw-2-glw, tl_y-1,     0.09f, 1.0f, glow},
                            {car_cx+bw-2+1, tl_y-1,       0.09f, 1.0f, glow},
                            {car_cx+bw-2-glw, y_bumper+1,  0.09f, 1.0f, glow},
                            {car_cx+bw-2+1, tl_y-1,       0.09f, 1.0f, glow},
                            {car_cx+bw-2+1, y_bumper+1,   0.09f, 1.0f, glow},
                            {car_cx+bw-2-glw, y_bumper+1,  0.09f, 1.0f, glow},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, gl_l, sizeof(RHW_VERT));
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, gl_r, sizeof(RHW_VERT));
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }

                    /* ---- Boost exhaust flames ---- */
                    {
                        float boost_val = _R_MEMF(0x5FFD08);
                        uint32_t boost_btn = _R_MEM32(0x5FFD0C);
                        if (boost_btn && boost_val > 0.0f && speed > 5.0f) {
                            static uint32_t _flame_seed = 7777;
                            _flame_seed = _flame_seed * 1103515245 + 12345;
                            float flicker = 0.7f + 0.3f * ((float)((_flame_seed >> 16) & 0xFF) / 255.0f);
                            float flame_len = 20.0f * flicker;
                            float fx_l = car_cx - bw * 0.35f;
                            float fx_r = car_cx + bw * 0.35f;

                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 1);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 19, 5);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device, 20, 2 /*D3DBLEND_ONE - additive*/);

                            /* Pick animated fire frame */
                            static uint32_t _fire_frame = 0;
                            _fire_frame++;
                            int fi_idx = (int)(_fire_frame / 2) % BOOST_FIRE_FRAMES;
                            IDirect3DTexture8 *fire_tex = g_tex_boostfire[fi_idx];
                            if (fire_tex) {
                                /* Textured animated boost flame sprites */
                                float fw = 8.0f * flicker;
                                draw_textured_quad(g_d3d_device, fire_tex,
                                    fx_l - fw, y_bumper, fx_l + fw, y_bumper + flame_len,
                                    0.08f, 0xFFFFFF88);
                                draw_textured_quad(g_d3d_device, fire_tex,
                                    fx_r - fw, y_bumper, fx_r + fw, y_bumper + flame_len,
                                    0.08f, 0xFFFFFF88);
                                /* Boost flare glow behind flames */
                                if (g_tex_boostflare) {
                                    float gr = 14.0f * flicker;
                                    draw_textured_quad(g_d3d_device, g_tex_boostflare,
                                        fx_l - gr, y_bumper - gr*0.3f,
                                        fx_l + gr, y_bumper + flame_len + gr*0.5f,
                                        0.09f, 0x60FFAA44);
                                    draw_textured_quad(g_d3d_device, g_tex_boostflare,
                                        fx_r - gr, y_bumper - gr*0.3f,
                                        fx_r + gr, y_bumper + flame_len + gr*0.5f,
                                        0.09f, 0x60FFAA44);
                                }
                                restore_untextured(g_d3d_device);
                            } else {
                                /* Fallback: untextured triangle flames */
                                DWORD f_inner = 0xFFFFFF44;
                                DWORD f_outer = 0xFFFF4400;
                                RHW_VERT flame_l[3] = {
                                    {fx_l - 3.0f, y_bumper, 0.08f, 1.0f, f_inner},
                                    {fx_l + 3.0f, y_bumper, 0.08f, 1.0f, f_inner},
                                    {fx_l, y_bumper + flame_len, 0.08f, 1.0f, f_outer},
                                };
                                RHW_VERT flame_r[3] = {
                                    {fx_r - 3.0f, y_bumper, 0.08f, 1.0f, f_inner},
                                    {fx_r + 3.0f, y_bumper, 0.08f, 1.0f, f_inner},
                                    {fx_r, y_bumper + flame_len, 0.08f, 1.0f, f_outer},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 1, flame_l, sizeof(RHW_VERT));
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 1, flame_r, sizeof(RHW_VERT));
                            }

                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 0);
                        }
                    }

                    /* ── Headlight beams (visible at night) ──────────────── */
                    {
                        float cycle_hl = fmodf(py / 3000.0f, 1.0f);
                        if (cycle_hl < 0.0f) cycle_hl += 1.0f;
                        float beam_alpha_f = 0.0f;
                        if (cycle_hl > 0.55f && cycle_hl < 0.95f) {
                            if (cycle_hl < 0.65f) beam_alpha_f = (cycle_hl - 0.55f) / 0.1f;
                            else if (cycle_hl > 0.85f) beam_alpha_f = 1.0f - (cycle_hl - 0.85f) / 0.1f;
                            else beam_alpha_f = 1.0f;
                        }
                        if (beam_alpha_f > 0.01f) {
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 1);
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                19, 5); /* SRCALPHA */
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                20, 6); /* INVSRCALPHA */
                            int ba = (int)(beam_alpha_f * 50.0f);
                            DWORD beam_near = ((DWORD)ba << 24) | 0x00FFFFCC;
                            DWORD beam_far  = 0x00FFFFCC;
                            float beam_top = HORIZON + 40.0f;
                            float beam_src_y = y_hood - 10.0f; /* from front of hood */
                            RHW_VERT bl[6] = {
                                {car_cx-16.0f, beam_src_y, 0.07f, 1.0f, beam_near},
                                {car_cx-6.0f,  beam_src_y, 0.07f, 1.0f, beam_near},
                                {car_cx-40.0f, beam_top,   0.07f, 1.0f, beam_far},
                                {car_cx-6.0f,  beam_src_y, 0.07f, 1.0f, beam_near},
                                {car_cx+10.0f, beam_top,   0.07f, 1.0f, beam_far},
                                {car_cx-40.0f, beam_top,   0.07f, 1.0f, beam_far},
                            };
                            RHW_VERT br[6] = {
                                {car_cx+6.0f,  beam_src_y, 0.07f, 1.0f, beam_near},
                                {car_cx+16.0f, beam_src_y, 0.07f, 1.0f, beam_near},
                                {car_cx-10.0f, beam_top,   0.07f, 1.0f, beam_far},
                                {car_cx+16.0f, beam_src_y, 0.07f, 1.0f, beam_near},
                                {car_cx+40.0f, beam_top,   0.07f, 1.0f, beam_far},
                                {car_cx-10.0f, beam_top,   0.07f, 1.0f, beam_far},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, bl, sizeof(RHW_VERT));
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, br, sizeof(RHW_VERT));
                            /* Corona glow at headlight source */
                            if (g_tex_corona) {
                                g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                    20, 2 /*D3DBLEND_ONE - additive*/);
                                int ca = (int)(beam_alpha_f * 120.0f);
                                DWORD cc = ((DWORD)ca << 24) | 0x00FFFFAA;
                                float cr = 10.0f;
                                draw_textured_quad(g_d3d_device, g_tex_corona,
                                    car_cx - 11.0f - cr, beam_src_y - cr,
                                    car_cx - 11.0f + cr, beam_src_y + cr,
                                    0.06f, cc);
                                draw_textured_quad(g_d3d_device, g_tex_corona,
                                    car_cx + 11.0f - cr, beam_src_y - cr,
                                    car_cx + 11.0f + cr, beam_src_y + cr,
                                    0.06f, cc);
                                restore_untextured(g_d3d_device);
                            }
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                D3DRS_ALPHABLENDENABLE, 0);
                        }
                    }
                }

                /* ── Speed lines (at high speed or when boosting) ────── */
                {
                    float abs_spd = speed < 0 ? -speed : speed;
                    if (abs_spd > 25.0f) {
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            19, 5); /* D3DRS_SRCBLEND = D3DBLEND_SRCALPHA */
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            20, 6); /* D3DRS_DESTBLEND = D3DBLEND_INVSRCALPHA */
                        /* Draw several speed lines streaking from center outward */
                        static uint32_t _line_seed = 42;
                        int li;
                        for (li = 0; li < 8; li++) {
                            _line_seed = _line_seed * 1103515245 + 12345;
                            float lx = (float)((_line_seed >> 16) & 0x1FF) + 70.0f;
                            _line_seed = _line_seed * 1103515245 + 12345;
                            float ly = HORIZON + (float)((_line_seed >> 16) & 0xFF);
                            float line_len = (abs_spd - 25.0f) * 1.5f;
                            if (line_len > 60.0f) line_len = 60.0f;
                            /* Alpha proportional to speed */
                            int alpha = (int)((abs_spd - 25.0f) * 4.0f);
                            if (alpha > 160) alpha = 160;
                            DWORD lc = ((DWORD)alpha << 24) | 0x00CCDDFF;
                            RHW_VERT sline[6] = {
                                {lx, ly,            0.06f, 1.0f, lc},
                                {lx + 1.5f, ly,     0.06f, 1.0f, lc},
                                {lx, ly + line_len, 0.06f, 1.0f, 0x00CCDDFF},
                                {lx + 1.5f, ly,     0.06f, 1.0f, lc},
                                {lx + 1.5f, ly + line_len, 0.06f, 1.0f, 0x00CCDDFF},
                                {lx, ly + line_len, 0.06f, 1.0f, 0x00CCDDFF},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, sline, sizeof(RHW_VERT));
                        }
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }
                }

                /* ── Wall sparks (on wall collision) ────────────────── */
                {
                    float spark_t = _R_MEMF(0x5FFD30);
                    if (spark_t > 0.0f) {
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            19, 5);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            20, 6);
                        uint32_t spark_side = _R_MEM32(0x5FFD34);
                        float spark_base_x = spark_side ? (CX + 24.0f) : (CX - 24.0f);
                        float spark_base_y = SH - 70.0f;
                        /* Emit 12 spark particles from wall contact point */
                        static uint32_t _spark_seed = 33333;
                        if (g_tex_spark) {
                            g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                                20, 2 /*D3DBLEND_ONE - additive for sparks*/);
                        }
                        int si_sp;
                        for (si_sp = 0; si_sp < 12; si_sp++) {
                            _spark_seed = _spark_seed * 1103515245 + 12345;
                            float dx_sp = ((float)((_spark_seed >> 16) & 0xFF) / 128.0f - 1.0f) *
                                          (spark_side ? 1.0f : -1.0f) * 30.0f;
                            _spark_seed = _spark_seed * 1103515245 + 12345;
                            float dy_sp = ((float)((_spark_seed >> 16) & 0xFF) / 255.0f) * -40.0f;
                            /* Animate outward based on remaining timer */
                            float progress = 1.0f - spark_t / 0.4f;
                            float spx = spark_base_x + dx_sp * progress;
                            float spy = spark_base_y + dy_sp * progress;
                            int spa = (int)(spark_t / 0.4f * 200.0f);
                            if (spa > 200) spa = 200;
                            /* Color: yellow to orange */
                            _spark_seed = _spark_seed * 1103515245 + 12345;
                            DWORD sp_rgb = ((_spark_seed >> 20) & 1) ? 0x00FFAA22 : 0x00FFDD44;
                            DWORD sp_c = ((DWORD)spa << 24) | sp_rgb;
                            float ssz = 1.0f + spark_t * 2.0f;
                            if (g_tex_spark) {
                                /* Textured spark particle */
                                draw_textured_quad(g_d3d_device, g_tex_spark,
                                    spx - ssz*1.5f, spy - ssz*1.5f,
                                    spx + ssz*1.5f, spy + ssz*1.5f,
                                    0.03f, sp_c);
                            } else {
                                RHW_VERT spv[6] = {
                                    {spx-ssz, spy-ssz, 0.03f, 1.0f, sp_c},
                                    {spx+ssz, spy-ssz, 0.03f, 1.0f, sp_c},
                                    {spx-ssz, spy+ssz, 0.03f, 1.0f, sp_c},
                                    {spx+ssz, spy-ssz, 0.03f, 1.0f, sp_c},
                                    {spx+ssz, spy+ssz, 0.03f, 1.0f, sp_c},
                                    {spx-ssz, spy+ssz, 0.03f, 1.0f, sp_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, spv, sizeof(RHW_VERT));
                            }
                        }
                        if (g_tex_spark) restore_untextured(g_d3d_device);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }
                }

                /* ── Rain effect (periodic weather) ──────────────────── */
                {
                    /* Rain cycles every 6000 world units: clear 0-4000, rain 4000-6000 */
                    float weather_cycle = fmodf(py / 6000.0f, 1.0f);
                    if (weather_cycle < 0.0f) weather_cycle += 1.0f;
                    float rain_intensity = 0.0f;
                    if (weather_cycle > 0.67f) {
                        /* Ramp in 0.67-0.75, full 0.75-0.92, ramp out 0.92-1.0 */
                        if (weather_cycle < 0.75f)
                            rain_intensity = (weather_cycle - 0.67f) / 0.08f;
                        else if (weather_cycle > 0.92f)
                            rain_intensity = 1.0f - (weather_cycle - 0.92f) / 0.08f;
                        else
                            rain_intensity = 1.0f;
                    }
                    if (rain_intensity > 0.01f) {
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            19, 5);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            20, 6);
                        /* Rain drops: diagonal streaks falling */
                        static uint32_t _rain_seed = 55555;
                        int rain_count = (int)(rain_intensity * 30.0f);
                        int ri;
                        for (ri = 0; ri < rain_count; ri++) {
                            _rain_seed = _rain_seed * 1103515245 + 12345;
                            float rx = (float)((_rain_seed >> 16) & 0x3FF) - 40.0f;
                            _rain_seed = _rain_seed * 1103515245 + 12345;
                            float ry = (float)((_rain_seed >> 16) & 0x1FF) - 20.0f;
                            float rlen = 12.0f + (float)((_rain_seed >> 8) & 0xF);
                            int ra = (int)(rain_intensity * 120.0f);
                            if (ra > 120) ra = 120;
                            DWORD rc = ((DWORD)ra << 24) | 0x00AABBDD;
                            DWORD rc_t = 0x00AABBDD; /* transparent end */
                            /* Diagonal streak: top-left to bottom-right */
                            RHW_VERT drop[6] = {
                                {rx,        ry,        0.03f, 1.0f, rc},
                                {rx + 1.0f, ry,        0.03f, 1.0f, rc},
                                {rx + 4.0f, ry + rlen, 0.03f, 1.0f, rc_t},
                                {rx + 1.0f, ry,        0.03f, 1.0f, rc},
                                {rx + 5.0f, ry + rlen, 0.03f, 1.0f, rc_t},
                                {rx + 4.0f, ry + rlen, 0.03f, 1.0f, rc_t},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, drop, sizeof(RHW_VERT));
                        }
                        /* Grey rain overlay on screen (fog effect) */
                        {
                            int fog_a = (int)(rain_intensity * 40.0f);
                            DWORD fog_c = ((DWORD)fog_a << 24) | 0x00667788;
                            RHW_VERT fog[6] = {
                                {0.0f, 0.0f, 0.025f, 1.0f, fog_c},
                                {SW,   0.0f, 0.025f, 1.0f, fog_c},
                                {0.0f, SH,   0.025f, 1.0f, fog_c},
                                {SW,   0.0f, 0.025f, 1.0f, fog_c},
                                {SW,   SH,   0.025f, 1.0f, fog_c},
                                {0.0f, SH,   0.025f, 1.0f, fog_c},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, fog, sizeof(RHW_VERT));
                        }
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }
                }

                /* ── Speed bar (bottom-left HUD) ─────────────────────── */
                {
                    float abs_spd = speed < 0 ? -speed : speed;
                    float bar_pct = abs_spd / 50.0f;
                    if (bar_pct > 1.0f) bar_pct = 1.0f;
                    float bar_w = bar_pct * 150.0f;
                    DWORD bg_col = 0xFF202030;
                    RHW_VERT bg_bar[6] = {
                        {10.0f, SH-30.0f, 0.05f, 1.0f, bg_col},
                        {160.0f, SH-30.0f, 0.05f, 1.0f, bg_col},
                        {10.0f, SH-18.0f, 0.05f, 1.0f, bg_col},
                        {160.0f, SH-30.0f, 0.05f, 1.0f, bg_col},
                        {160.0f, SH-18.0f, 0.05f, 1.0f, bg_col},
                        {10.0f, SH-18.0f, 0.05f, 1.0f, bg_col},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 2, bg_bar, sizeof(RHW_VERT));
                    DWORD spd_col = (speed >= 0) ? 0xFF44FF44 : 0xFFFF4444;
                    if (g_tex_healthbar && bar_w > 1.0f) {
                        /* Textured speed bar using game health_bar texture */
                        draw_textured_quad(g_d3d_device, g_tex_healthbar,
                            10.0f, SH - 30.0f, 10.0f + bar_w, SH - 18.0f,
                            0.04f, spd_col);
                        restore_untextured(g_d3d_device);
                    } else {
                        RHW_VERT spd_bar[6] = {
                            {10.0f, SH-30.0f, 0.04f, 1.0f, spd_col},
                            {10.0f+bar_w, SH-30.0f, 0.04f, 1.0f, spd_col},
                            {10.0f, SH-18.0f, 0.04f, 1.0f, spd_col},
                            {10.0f+bar_w, SH-30.0f, 0.04f, 1.0f, spd_col},
                            {10.0f+bar_w, SH-18.0f, 0.04f, 1.0f, spd_col},
                            {10.0f, SH-18.0f, 0.04f, 1.0f, spd_col},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, spd_bar, sizeof(RHW_VERT));
                    }
                }

                /* ── Boost bar (bottom-left HUD, below speed bar) ────── */
                {
                    float boost = _R_MEMF(0x5FFD08);
                    float boost_pct = boost / 100.0f;
                    if (boost_pct > 1.0f) boost_pct = 1.0f;
                    if (boost_pct < 0.0f) boost_pct = 0.0f;
                    float boost_w = boost_pct * 150.0f;
                    /* Background */
                    DWORD bbg_col = 0xFF202030;
                    RHW_VERT bbg[6] = {
                        {10.0f, SH-46.0f, 0.05f, 1.0f, bbg_col},
                        {160.0f, SH-46.0f, 0.05f, 1.0f, bbg_col},
                        {10.0f, SH-34.0f, 0.05f, 1.0f, bbg_col},
                        {160.0f, SH-46.0f, 0.05f, 1.0f, bbg_col},
                        {160.0f, SH-34.0f, 0.05f, 1.0f, bbg_col},
                        {10.0f, SH-34.0f, 0.05f, 1.0f, bbg_col},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 2, bbg, sizeof(RHW_VERT));
                    /* Boost fill: blue normally, orange when actively boosting */
                    uint32_t boost_active = _R_MEM32(0x5FFD0C);
                    DWORD boost_col = (boost_active && boost > 0.0f)
                        ? 0xFFFF8833 : 0xFF3388FF;
                    RHW_VERT bfill[6] = {
                        {10.0f, SH-46.0f, 0.04f, 1.0f, boost_col},
                        {10.0f+boost_w, SH-46.0f, 0.04f, 1.0f, boost_col},
                        {10.0f, SH-34.0f, 0.04f, 1.0f, boost_col},
                        {10.0f+boost_w, SH-46.0f, 0.04f, 1.0f, boost_col},
                        {10.0f+boost_w, SH-34.0f, 0.04f, 1.0f, boost_col},
                        {10.0f, SH-34.0f, 0.04f, 1.0f, boost_col},
                    };
                    if (boost_w > 0.5f) {
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, bfill, sizeof(RHW_VERT));
                    }
                }

                /* ── Takedown counter (top-right HUD) ────────────────── */
                {
                    uint32_t takedowns = _R_MEM32(0x5FFD00);
                    if (takedowns > 0) {
                        DWORD td_col = 0xFFFF3333;
                        uint32_t ti;
                        for (ti = 0; ti < takedowns && ti < 20; ti++) {
                            float tx = SW - 20.0f - (float)(ti % 10) * 14.0f;
                            float ty = 10.0f + (float)(ti / 10) * 14.0f;
                            if (g_tex_star) {
                                /* Textured star pip for each takedown */
                                draw_textured_quad(g_d3d_device, g_tex_star,
                                    tx, ty, tx + 12.0f, ty + 12.0f,
                                    0.02f, 0xFFFF4444);
                                restore_untextured(g_d3d_device);
                            } else {
                                RHW_VERT pip[6] = {
                                    {tx, ty, 0.02f, 1.0f, td_col},
                                    {tx+10.0f, ty, 0.02f, 1.0f, td_col},
                                    {tx, ty+10.0f, 0.02f, 1.0f, td_col},
                                    {tx+10.0f, ty, 0.02f, 1.0f, td_col},
                                    {tx+10.0f, ty+10.0f, 0.02f, 1.0f, td_col},
                                    {tx, ty+10.0f, 0.02f, 1.0f, td_col},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, pip, sizeof(RHW_VERT));
                            }
                        }
                    }
                }

                /* ── Score display (top-left HUD) ───────────────────── */
                {
                    uint32_t score = _R_MEM32(0x5FFD24);
                    float mult = _R_MEMF(0x5FFD28);
                    if (mult < 1.0f) mult = 1.0f;
                    /* Score background with bright border */
                    g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                        D3DRS_ALPHABLENDENABLE, 1);
                    g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                        19, 5);
                    g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                        20, 6);
                    /* Bright border frame */
                    DWORD sborder = 0xC0445566;
                    RHW_VERT s_bdr[6] = {
                        {8.0f, 8.0f, 0.022f, 1.0f, sborder},
                        {172.0f, 8.0f, 0.022f, 1.0f, sborder},
                        {8.0f, 44.0f, 0.022f, 1.0f, sborder},
                        {172.0f, 8.0f, 0.022f, 1.0f, sborder},
                        {172.0f, 44.0f, 0.022f, 1.0f, sborder},
                        {8.0f, 44.0f, 0.022f, 1.0f, sborder},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 2, s_bdr, sizeof(RHW_VERT));
                    /* Dark background fill */
                    DWORD sbg = 0xD0182030;
                    if (g_tex_hud) {
                        draw_textured_quad(g_d3d_device, g_tex_hud,
                            10.0f, 10.0f, 170.0f, 42.0f,
                            0.02f, 0xD0FFFFFF);
                        restore_untextured(g_d3d_device);
                    } else {
                        RHW_VERT s_bg[6] = {
                            {10.0f, 10.0f, 0.02f, 1.0f, sbg},
                            {170.0f, 10.0f, 0.02f, 1.0f, sbg},
                            {10.0f, 42.0f, 0.02f, 1.0f, sbg},
                            {170.0f, 10.0f, 0.02f, 1.0f, sbg},
                            {170.0f, 42.0f, 0.02f, 1.0f, sbg},
                            {10.0f, 42.0f, 0.02f, 1.0f, sbg},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, s_bg, sizeof(RHW_VERT));
                    }
                    /* Score digit visualization: white blocks per digit */
                    {
                        uint32_t s = score;
                        int digit_i;
                        float dx_d = 155.0f;
                        DWORD digit_c = 0xE0EEEEFF;
                        for (digit_i = 0; digit_i < 8 && (s > 0 || digit_i == 0); digit_i++) {
                            int d = s % 10;
                            s /= 10;
                            /* Draw digit as stacked bars (crude 7-segment style) */
                            float dh = 2.5f;
                            float dy_d = 14.0f;
                            int row;
                            for (row = 0; row < d && row < 9; row++) {
                                RHW_VERT dv[6] = {
                                    {dx_d, dy_d, 0.018f, 1.0f, digit_c},
                                    {dx_d+8.0f, dy_d, 0.018f, 1.0f, digit_c},
                                    {dx_d, dy_d+dh, 0.018f, 1.0f, digit_c},
                                    {dx_d+8.0f, dy_d, 0.018f, 1.0f, digit_c},
                                    {dx_d+8.0f, dy_d+dh, 0.018f, 1.0f, digit_c},
                                    {dx_d, dy_d+dh, 0.018f, 1.0f, digit_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, dv, sizeof(RHW_VERT));
                                dy_d += dh + 0.5f;
                            }
                            dx_d -= 12.0f;
                        }
                    }
                    /* Multiplier indicator: colored bar at bottom.
                     * Width scales with multiplier (1x-8x), color shifts. */
                    if (mult > 1.0f) {
                        float m_pct = (mult - 1.0f) / 7.0f;
                        if (m_pct > 1.0f) m_pct = 1.0f;
                        float m_w = m_pct * 150.0f;
                        /* Color: green at 1x→ yellow at 4x → red at 8x */
                        DWORD m_col;
                        if (mult < 4.0f) {
                            int r = (int)((mult - 1.0f) / 3.0f * 255.0f);
                            m_col = 0xC000FF00 | ((DWORD)r << 16);
                        } else {
                            int g = (int)((1.0f - (mult - 4.0f) / 4.0f) * 255.0f);
                            if (g < 0) g = 0;
                            m_col = 0xC0FF0000 | ((DWORD)g << 8);
                        }
                        RHW_VERT m_bar[6] = {
                            {14.0f,      38.0f, 0.015f, 1.0f, m_col},
                            {14.0f+m_w,  38.0f, 0.015f, 1.0f, m_col},
                            {14.0f,      42.0f, 0.015f, 1.0f, m_col},
                            {14.0f+m_w,  38.0f, 0.015f, 1.0f, m_col},
                            {14.0f+m_w,  42.0f, 0.015f, 1.0f, m_col},
                            {14.0f,      42.0f, 0.015f, 1.0f, m_col},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, m_bar, sizeof(RHW_VERT));
                    }
                    g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                        D3DRS_ALPHABLENDENABLE, 0);
                }

                /* ── Checkpoint banner (green flash on milestone) ────── */
                {
                    float cp_flash = _R_MEMF(0x5FFD20);
                    if (cp_flash > 0.0f) {
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            19, 5); /* SRCALPHA */
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            20, 6); /* INVSRCALPHA */
                        /* Green banner across screen center, fading out */
                        int cp_alpha = (int)(cp_flash / 1.5f * 180.0f);
                        if (cp_alpha > 180) cp_alpha = 180;
                        if (cp_alpha < 0) cp_alpha = 0;
                        DWORD cp_col = ((DWORD)cp_alpha << 24) | 0x0022FF44;
                        float banner_y = SH * 0.35f;
                        float banner_h = 28.0f;
                        RHW_VERT cp_bg[6] = {
                            {0.0f, banner_y,            0.02f, 1.0f, cp_col},
                            {SW,   banner_y,            0.02f, 1.0f, cp_col},
                            {0.0f, banner_y + banner_h, 0.02f, 1.0f, cp_col},
                            {SW,   banner_y,            0.02f, 1.0f, cp_col},
                            {SW,   banner_y + banner_h, 0.02f, 1.0f, cp_col},
                            {0.0f, banner_y + banner_h, 0.02f, 1.0f, cp_col},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, cp_bg, sizeof(RHW_VERT));
                        /* White diamond marker in center of banner */
                        DWORD dia_col = ((DWORD)cp_alpha << 24) | 0x00FFFFFF;
                        float dc = CX, dy = banner_y + banner_h * 0.5f;
                        RHW_VERT dia[12] = {
                            {dc,       dy - 10.0f, 0.015f, 1.0f, dia_col},
                            {dc + 8.0f, dy,        0.015f, 1.0f, dia_col},
                            {dc,       dy + 10.0f, 0.015f, 1.0f, dia_col},
                            {dc,       dy - 10.0f, 0.015f, 1.0f, dia_col},
                            {dc - 8.0f, dy,        0.015f, 1.0f, dia_col},
                            {dc,       dy + 10.0f, 0.015f, 1.0f, dia_col},
                        };
                        g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                            D3DPT_TRIANGLELIST, 2, dia, sizeof(RHW_VERT));
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }
                }

                /* ── Flash effect (white=takedown, red=crash) ────────── */
                {
                    float flash = _R_MEMF(0x5FFD04);
                    if (flash > 0.0f) {
                        int alpha = (int)(flash * 2.0f * 180.0f);
                        if (alpha > 180) alpha = 180;
                        if (alpha < 0) alpha = 0;
                        /* Red flash during crash (shake timer active), white for takedown */
                        float shake = _R_MEMF(0x5FFD18);
                        DWORD flash_rgb = (shake > 0.0f) ? 0x00FF4400 : 0x00FFFFFF;
                        DWORD flash_col = ((DWORD)alpha << 24) | flash_rgb;
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 1);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            19, 5);
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            20, 6);
                        /* Use explosion texture for crash flash */
                        if (shake > 0.0f && g_tex_explosion) {
                            draw_textured_quad(g_d3d_device, g_tex_explosion,
                                0.0f, 0.0f, SW, SH, 0.01f, flash_col);
                            restore_untextured(g_d3d_device);
                        } else {
                            RHW_VERT flash_verts[6] = {
                                {0.0f, 0.0f, 0.01f, 1.0f, flash_col},
                                {SW, 0.0f, 0.01f, 1.0f, flash_col},
                                {0.0f, SH, 0.01f, 1.0f, flash_col},
                                {SW, 0.0f, 0.01f, 1.0f, flash_col},
                                {SW, SH, 0.01f, 1.0f, flash_col},
                                {0.0f, SH, 0.01f, 1.0f, flash_col},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, flash_verts, sizeof(RHW_VERT));
                        }
                        g_d3d_device->lpVtbl->SetRenderState(g_d3d_device,
                            D3DRS_ALPHABLENDENABLE, 0);
                    }
                }

                /* ── Rear-view mirror (top center) ──────────────────── */
                {
                    /* Mirror viewport: small rectangle at top center */
                    float mv_x = CX - 60.0f, mv_y = 6.0f;
                    float mv_w = 120.0f, mv_h = 40.0f;

                    /* Mirror border */
                    DWORD bdr = 0xFF888888;
                    float bd = 2.0f;
                    RHW_VERT bdr_v[24] = {
                        /* Top */
                        {mv_x-bd, mv_y-bd, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y-bd, 0.005f, 1.0f, bdr},
                        {mv_x-bd, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y-bd, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x-bd, mv_y, 0.005f, 1.0f, bdr},
                        /* Bottom */
                        {mv_x-bd, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x-bd, mv_y+mv_h+bd, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y+mv_h+bd, 0.005f, 1.0f, bdr},
                        {mv_x-bd, mv_y+mv_h+bd, 0.005f, 1.0f, bdr},
                        /* Left */
                        {mv_x-bd, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x-bd, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x-bd, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        /* Right */
                        {mv_x+mv_w, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w+bd, mv_y+mv_h, 0.005f, 1.0f, bdr},
                        {mv_x+mv_w, mv_y+mv_h, 0.005f, 1.0f, bdr},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 8, bdr_v, sizeof(RHW_VERT));

                    /* Mirror background (road behind) */
                    DWORD mv_road = 0xFF222233;
                    DWORD mv_sky = tod_sky_bot;
                    float mv_hz = mv_y + mv_h * 0.4f; /* horizon in mirror */
                    RHW_VERT mv_bg[12] = {
                        /* Sky */
                        {mv_x, mv_y, 0.004f, 1.0f, mv_sky},
                        {mv_x+mv_w, mv_y, 0.004f, 1.0f, mv_sky},
                        {mv_x, mv_hz, 0.004f, 1.0f, mv_sky},
                        {mv_x+mv_w, mv_y, 0.004f, 1.0f, mv_sky},
                        {mv_x+mv_w, mv_hz, 0.004f, 1.0f, mv_sky},
                        {mv_x, mv_hz, 0.004f, 1.0f, mv_sky},
                        /* Road */
                        {mv_x, mv_hz, 0.004f, 1.0f, mv_road},
                        {mv_x+mv_w, mv_hz, 0.004f, 1.0f, mv_road},
                        {mv_x, mv_y+mv_h, 0.004f, 1.0f, mv_road},
                        {mv_x+mv_w, mv_hz, 0.004f, 1.0f, mv_road},
                        {mv_x+mv_w, mv_y+mv_h, 0.004f, 1.0f, mv_road},
                        {mv_x, mv_y+mv_h, 0.004f, 1.0f, mv_road},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 4, mv_bg, sizeof(RHW_VERT));

                    /* Road edges in mirror (perspective lines converging to center) */
                    float mv_cx = mv_x + mv_w * 0.5f;
                    float mv_rw_near = mv_w * 0.45f; /* road half-width at bottom */
                    float mv_rw_far = mv_w * 0.08f;  /* road half-width at horizon */
                    DWORD el_c = 0xFFCCCCCC;
                    RHW_VERT mv_edges[12] = {
                        /* Left edge */
                        {mv_cx - mv_rw_near - 1.0f, mv_y+mv_h, 0.003f, 1.0f, el_c},
                        {mv_cx - mv_rw_near + 1.0f, mv_y+mv_h, 0.003f, 1.0f, el_c},
                        {mv_cx - mv_rw_far,  mv_hz, 0.003f, 1.0f, el_c},
                        {mv_cx - mv_rw_near + 1.0f, mv_y+mv_h, 0.003f, 1.0f, el_c},
                        {mv_cx - mv_rw_far + 1.0f,  mv_hz, 0.003f, 1.0f, el_c},
                        {mv_cx - mv_rw_far,  mv_hz, 0.003f, 1.0f, el_c},
                        /* Right edge */
                        {mv_cx + mv_rw_near - 1.0f, mv_y+mv_h, 0.003f, 1.0f, el_c},
                        {mv_cx + mv_rw_near + 1.0f, mv_y+mv_h, 0.003f, 1.0f, el_c},
                        {mv_cx + mv_rw_far - 1.0f,  mv_hz, 0.003f, 1.0f, el_c},
                        {mv_cx + mv_rw_near + 1.0f, mv_y+mv_h, 0.003f, 1.0f, el_c},
                        {mv_cx + mv_rw_far,  mv_hz, 0.003f, 1.0f, el_c},
                        {mv_cx + mv_rw_far - 1.0f,  mv_hz, 0.003f, 1.0f, el_c},
                    };
                    g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                        D3DPT_TRIANGLELIST, 4, mv_edges, sizeof(RHW_VERT));

                    /* Render traffic cars behind player in mirror */
                    {
                        #define MV_OBS_BASE 0x5FFE00
                        #define MV_OBS_COUNT 12
                        #define MV_OBS_SIZE 16
                        int moi;
                        for (moi = 0; moi < MV_OBS_COUNT; moi++) {
                            uint32_t mflags = _R_MEM32(MV_OBS_BASE + moi * MV_OBS_SIZE + 0xC);
                            if ((mflags & 1) == 0) continue;
                            float mox = _R_MEMF(MV_OBS_BASE + moi * MV_OBS_SIZE);
                            float moy = _R_MEMF(MV_OBS_BASE + moi * MV_OBS_SIZE + 4);
                            /* Distance behind player (negative = behind) */
                            float behind = py - moy;
                            if (behind < 1.0f || behind > 60.0f) continue;
                            /* Project into mirror viewport */
                            float md = behind;
                            float mt = 1.0f - (md / 60.0f); /* 0=far, 1=near */
                            float mv_sx = mv_cx + (mox - px) * mt * 2.0f;
                            float mv_sy = mv_hz + (mv_y + mv_h - mv_hz) * mt;
                            float msz = 2.0f + mt * 4.0f; /* car size */
                            /* Clamp to mirror bounds */
                            if (mv_sx < mv_x || mv_sx > mv_x + mv_w) continue;
                            if (mv_sy < mv_hz || mv_sy > mv_y + mv_h) continue;
                            int is_onc = (mflags & 2) != 0;
                            DWORD mc = is_onc ? 0xFFFF4444 : 0xFFFFCC44;
                            RHW_VERT mcv[6] = {
                                {mv_sx - msz, mv_sy - msz*0.7f, 0.002f, 1.0f, mc},
                                {mv_sx + msz, mv_sy - msz*0.7f, 0.002f, 1.0f, mc},
                                {mv_sx - msz, mv_sy + msz*0.7f, 0.002f, 1.0f, mc},
                                {mv_sx + msz, mv_sy - msz*0.7f, 0.002f, 1.0f, mc},
                                {mv_sx + msz, mv_sy + msz*0.7f, 0.002f, 1.0f, mc},
                                {mv_sx - msz, mv_sy + msz*0.7f, 0.002f, 1.0f, mc},
                            };
                            g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                D3DPT_TRIANGLELIST, 2, mcv, sizeof(RHW_VERT));
                            /* Headlights on cars behind (they face toward us) */
                            if (!is_onc) {
                                DWORD hl_c = 0xFFFFFF88;
                                float hlsz = msz * 0.3f;
                                RHW_VERT hl_l[6] = {
                                    {mv_sx-msz*0.5f-hlsz, mv_sy-hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx-msz*0.5f+hlsz, mv_sy-hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx-msz*0.5f-hlsz, mv_sy+hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx-msz*0.5f+hlsz, mv_sy-hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx-msz*0.5f+hlsz, mv_sy+hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx-msz*0.5f-hlsz, mv_sy+hlsz, 0.001f, 1.0f, hl_c},
                                };
                                RHW_VERT hl_r[6] = {
                                    {mv_sx+msz*0.5f-hlsz, mv_sy-hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx+msz*0.5f+hlsz, mv_sy-hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx+msz*0.5f-hlsz, mv_sy+hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx+msz*0.5f+hlsz, mv_sy-hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx+msz*0.5f+hlsz, mv_sy+hlsz, 0.001f, 1.0f, hl_c},
                                    {mv_sx+msz*0.5f-hlsz, mv_sy+hlsz, 0.001f, 1.0f, hl_c},
                                };
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, hl_l, sizeof(RHW_VERT));
                                g_d3d_device->lpVtbl->DrawPrimitiveUP(g_d3d_device,
                                    D3DPT_TRIANGLELIST, 2, hl_r, sizeof(RHW_VERT));
                            }
                        }
                        #undef MV_OBS_BASE
                        #undef MV_OBS_COUNT
                        #undef MV_OBS_SIZE
                    }
                }

                #undef ROAD_SEGS
                #undef ROAD_CURVE
                #undef ROAD_HILL
                #undef PROJ_X
                #undef PROJ_Y
                #undef PROJ_SCALE
            }
            #undef _R_MEMF
            #undef _R_MEM32
        }
#endif  /* end of #if 0 pseudo-3D removed */

        /* NV2A push buffer test (toggle with G key) */
        {
            extern void nv2a_pb_test_frame(void);
            nv2a_pb_test_frame();
        }

        /* NV2A push buffer replay (toggle with R key, auto-start after 60 frames) */
        {
            extern void nv2a_pb_replay_frame(void);
            extern int nv2a_pb_replay_is_active(void);
            extern void nv2a_pb_replay_set_active(int active);
            static int replay_auto_started = 0;
            extern volatile uint32_t g_present_count;
            if (!replay_auto_started && g_present_count > 60) {
                nv2a_pb_replay_set_active(1);
                replay_auto_started = 1;
            }
            nv2a_pb_replay_frame();
        }

        if (!rw_bridge_frame_rendered()) {
            g_d3d_device->lpVtbl->EndScene(g_d3d_device);
            menu_gui_begin_frame();
            menu_gui_render();
            g_d3d_device->lpVtbl->Present(g_d3d_device, NULL, NULL, NULL, NULL);
        } else {
            /* Bridge rendered but deferred Present so sub_0003FEE0's render
             * dispatch can draw static.dat geometry into the same frame. */
            menu_gui_begin_frame();
            menu_gui_render();
            g_d3d_device->lpVtbl->Present(g_d3d_device, NULL, NULL, NULL, NULL);
        }
        /* Auto-screenshot on frame 150 to capture geometry state */
        {
            extern volatile uint32_t g_present_count;
            if (g_present_count == 150)
                menu_gui_take_screenshot();
            g_present_count++;
        }
    }

    /* Update window title with game state (every 30 frames) */
    {
        static uint32_t s_title_counter = 0;
        s_title_counter++;
        if (g_hwnd && (s_title_counter % 30) == 0) {
            extern volatile uint64_t g_icall_count;
            extern volatile uint32_t g_tick_110e0_count;
            extern volatile uint32_t g_present_count;
            /* Read game state from Xbox memory */
            extern ptrdiff_t g_xbox_mem_offset;
            #define XMEM32(a) (*(volatile uint32_t*)((uintptr_t)(a) + g_xbox_mem_offset))
            #define XMEMF(a)  (*(volatile float*)((uintptr_t)(a) + g_xbox_mem_offset))
            /* Game state read for debug menu (title bar no longer updated) */
            /* Read physics state for display */
            uint32_t phys_ptr = XMEM32(0x557880 + 0x1B4);
            float spd = 0.0f, hdg = 0.0f;
            float px = 0.0f, py = 0.0f;
            if (phys_ptr > 0x100 && phys_ptr < 0x4000000) {
                px  = XMEMF(phys_ptr + 0x10);
                py  = XMEMF(phys_ptr + 0x14);
                hdg = XMEMF(phys_ptr + 0x18);
                spd = XMEMF(phys_ptr + 0x1C);
            }
            uint32_t takedowns = XMEM32(0x5FFD00);
            float boost = *(volatile float*)((uintptr_t)0x5FFD08 + g_xbox_mem_offset);
            uint32_t dist_m = XMEM32(0x5FFD14);
            uint32_t score = XMEM32(0x5FFD24);
            float mult = XMEMF(0x5FFD28);
            if (mult < 1.0f) mult = 1.0f;
            (void)spd; (void)hdg; (void)px; (void)py;
            (void)takedowns; (void)boost; (void)dist_m; (void)score; (void)mult;
            #undef XMEM32
            #undef XMEMF
        }
    }
}

/* ── Main game loop ─────────────────────────────────────────── */

/* Real wall-clock seconds since the previous call. The videos play at their own
 * rate, so they need true elapsed time, not a fixed 1/60. */
static float frame_dt(void)
{
    static LARGE_INTEGER freq = {0}, last = {0};
    LARGE_INTEGER now;
    float dt;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        return 0.0f;
    }
    QueryPerformanceCounter(&now);
    dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    last = now;
    if (dt > 0.1f) dt = 0.1f;   /* clamp: a stall must not skip half a video */
    return dt;
}

/* Any of Enter/Space/Esc, or any gamepad button, skips the current video. */
static int boot_skip_pressed(void)
{
    XINPUT_STATE xs;

    if (GetAsyncKeyState(VK_RETURN) & 0x8000) return 1;
    if (GetAsyncKeyState(VK_SPACE)  & 0x8000) return 1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return 1;
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        if (xs.Gamepad.wButtons) return 1;
        if (xs.Gamepad.bLeftTrigger  > 30) return 1;
        if (xs.Gamepad.bRightTrigger > 30) return 1;
    }
    return 0;
}

/*
 * Drive the game's per-frame tick from the host loop.
 *
 * Burnout 3 is tick-driven, not main-loop-driven. xbe_entry_point() spawns the
 * init thread and returns; that thread runs init (game_state = 2) and calls
 * PsTerminateSystemThread. Nothing in the title then drives its state machine --
 * on hardware the main thread does, after spawning the init thread. Here this
 * loop is that driver, and sub_000110E0 is the tick the game's own frame loops
 * (sub_00015F10 / sub_0001664C) call.
 *
 * The recomp CPU state is __declspec(thread), so this thread has its own g_esp,
 * zero until it is given a stack -- the first PUSH32 in the tick would write
 * through address 0. The slot is allocated once and never freed: this thread
 * ticks for the lifetime of the process.
 */
static void game_tick_drive(void)
{
    static int stack_slot = -1;

    /* Init has not set game_state yet; there is nothing to tick. */
    if (MEM32(0x4D53B8) == 0)
        return;

    if (stack_slot < 0) {
        stack_slot = xbox_worker_stack_alloc();
        if (stack_slot < 0) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "  [TICK] no worker stack free; host tick disabled\n");
            }
            return;
        }
        fprintf(stderr, "  [TICK] host-driven tick armed on slot %d (esp=0x%08X)\n",
                stack_slot, XBOX_WORKER_STACK_TOP(stack_slot));
    }

    /* Re-base every frame rather than trusting the tick to balance its own
     * stack -- it is lifted cdecl and a few bytes of drift per frame would
     * walk off the slice within a minute of gameplay. */
    g_esp = XBOX_WORKER_STACK_TOP(stack_slot);
    g_seh_ebp = g_esp;
    g_esp -= 4; MEM32(g_esp) = 0;   /* dummy return address, as the lifted callers push */

    sub_000110E0();
}

static void game_loop(void)
{
    MSG msg;

    fprintf(stderr, "Entering main loop (press ESC to exit)...\n");

    while (g_running) {
        /* Process Windows messages */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = FALSE;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (!g_running)
            break;

        game_tick_drive();

        /*
         * Frame rendering.
         *
         * Eventually the recompiled game code will drive this.
         * For now, clear to dark blue and present to verify D3D works.
         */
        if (g_d3d_device) {
            /* The boot videos are driven from here, not from the game's frame
             * pump.
             *
             * They used to hang off game_frame_pump (via sub_000110E0), which
             * cannot work: that pump throttles itself to 60 Hz and returns
             * early on its very first call (it sets s_last = now, then finds
             * elapsed == 0), and it only runs at all once the game reaches its
             * tick loop. The game reached it twice and then blocked on a load,
             * so boot_update never ran once.
             *
             * This loop is the right owner anyway now that the game has its own
             * thread: it owns the window and the device, and it runs whether or
             * not the game is busy loading -- which is exactly when the intro
             * is meant to play. */
            int boot_phase = boot_get_phase();
            if (boot_phase < BOOT_PHASE_GAMEPLAY) {
                boot_update(frame_dt(), boot_skip_pressed());
                g_d3d_device->lpVtbl->BeginScene(g_d3d_device);
                g_d3d_device->lpVtbl->Clear(g_d3d_device, 0, NULL,
                    D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                    0xFF000000,  /* black behind the videos */
                    1.0f, 0);
                boot_render();
                g_d3d_device->lpVtbl->EndScene(g_d3d_device);
            } else {
                g_d3d_device->lpVtbl->BeginScene(g_d3d_device);
                g_d3d_device->lpVtbl->Clear(g_d3d_device, 0, NULL,
                    D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                    0xFF001030,  /* Dark blue */
                    1.0f, 0);
                g_d3d_device->lpVtbl->EndScene(g_d3d_device);
            }
            menu_gui_begin_frame();
            menu_gui_render();
            g_d3d_device->lpVtbl->Present(g_d3d_device, NULL, NULL, NULL, NULL);
        }
        /* Reliable diagnostics from the MAIN thread (this loop runs steadily at
         * 60fps, unlike the game thread which starves everything and unlike the
         * profiler thread which deadlocked suspending it). Once a second:
         *   - GetThreadTimes on the game thread: rising CPU = spinning,
         *     flat = blocked. This is the spinning-vs-blocked answer, no
         *     suspend needed.
         *   - the load-loop memory probes.
         *   - only if blocked, suspend once for the parked RIP. */
        {
            static uint64_t last_diag = 0;
            uint64_t now_ms = GetTickCount64();
            if (now_ms - last_diag >= 1000) {
                last_diag = now_ms;
                HANDLE gt = xbox_thread_debug_handle();
                if (gt) {
                    static uint64_t prev_cpu = 0;
                    FILETIME c, e, k, u;
                    if (GetThreadTimes(gt, &c, &e, &k, &u)) {
                        uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
                                     + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
                        uint64_t delta = cpu - prev_cpu;
                        prev_cpu = cpu;
                        uint32_t reg = 0, firstobj = 0, gs = 0;
                        if (g_xbox_mem_offset) {
                            #define DBG(va) (*(volatile uint32_t*)((uintptr_t)(va)+g_xbox_mem_offset))
                            reg = DBG(0x5A3400); firstobj = DBG(0x572988); gs = DBG(0x4D53B8);
                            #undef DBG
                        }
                        fprintf(stderr, "  [DIAG] game thread cpu +%llums/s (%s)  "
                                "state=0x%X registered=%u first_obj=0x%08X\n",
                                (unsigned long long)(delta / 10000),
                                delta > 50000 ? "SPINNING" : "idle/blocked",
                                gs, reg, firstobj);
                        /* Always grab the RIP (safe from the main thread):
                         * whether spinning or blocked, where it is is the
                         * whole question. */
                        {
                            CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
                            ctx.ContextFlags = CONTEXT_CONTROL;
                            if (SuspendThread(gt) != (DWORD)-1) {
                                if (GetThreadContext(gt, &ctx)) {
                                    /* Approximate backtrace: scan the native
                                     * stack for values that land inside the
                                     * image (return addresses). The map turns
                                     * them into the loop nest. */
                                    fprintf(stderr, "  [DIAG] RIP=0x%016llX chain:",
                                            (unsigned long long)ctx.Rip);
                                    uint64_t base = 0x140000000ull, top = base + 0x8000000ull;
                                    uint64_t *sp = (uint64_t*)ctx.Rsp;
                                    int shown = 0;
                                    for (int q = 0; q < 256 && shown < 10; q++) {
                                        uint64_t v = sp[q];
                                        if (v >= base && v < top) {
                                            fprintf(stderr, " 0x%llX", (unsigned long long)v);
                                            shown++;
                                        }
                                    }
                                    fprintf(stderr, "\n");
                                }
                                ResumeThread(gt);
                            }
                        }
                        fflush(stderr);
                    }
                }
            }
        }
        Sleep(16); /* ~60 FPS target */
    }
}

/* ── Entry point ────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    const char *xbe_path = DEFAULT_XBE_PATH;

    (void)hPrevInstance;
    (void)nCmdShow;

    fprintf(stderr, "Burnout 3: Takedown - Static Recompilation\n");
    fprintf(stderr, "==========================================\n\n");

    /* Allow custom XBE path via command line */
    if (lpCmdLine && lpCmdLine[0]) {
        xbe_path = lpCmdLine;
    }

    /* Load the original XBE (needed for data sections) */
    if (!load_xbe(xbe_path)) {
        MessageBoxA(NULL,
            "Failed to load default.xbe.\n\n"
            "Place the game files in a 'Burnout 3 Takedown' folder\n"
            "next to this executable, or pass the XBE path as an argument.",
            WINDOW_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Create the game window */
    g_hwnd = create_window(hInstance, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    if (!g_hwnd) {
        fprintf(stderr, "FATAL: Failed to create window\n");
        shutdown_subsystems();
        return 1;
    }

    /* Initialize all subsystems */
    if (!init_subsystems()) {
        shutdown_subsystems();
        return 1;
    }

    /* Run recompiled code integration tests */
    {
        int recomp_run_tests(void);
        int test_result = recomp_run_tests();
        if (test_result < 0) {
            fprintf(stderr, "WARNING: Integration tests skipped\n");
        } else if (test_result != 0) {
            fprintf(stderr, "WARNING: Some integration tests failed\n");
        }
    }

    /* Register VEH for crash diagnostics */
    AddVectoredExceptionHandler(1, crash_veh);

    /* Start KeTickCount updater thread (Xbox timing) */
    CreateThread(NULL, 0, tick_count_thread_func, NULL, 0, NULL);

    /* Start watchdog thread for periodic register dumps */
    CreateThread(NULL, 0, watchdog_thread_func, NULL, 0, NULL);

    /* Call the recompiled game entry point with crash protection.
     * We push a dummy return address (simulating x86 'call' instruction)
     * because the translated code expects [esp] = return addr on entry. */
    fprintf(stderr, "\n=== Calling xbe_entry_point (0x001D2807) ===\n");
    fprintf(stderr, "  g_esp = 0x%08X before call\n", g_esp);
    fprintf(stderr, "  JT verify pre-entry: [0x16CC8]=0x%08X (expect 0x000166D1)\n", MEM32(0x16CC8));
    fprintf(stderr, "  RW vtable BEFORE init: 0x36B860=0x%08X 0x36B89C=0x%08X\n",
            MEM32(0x36B860), MEM32(0x36B89C));
#if defined(_WIN32)
    __try {
        PUSH32(g_esp, 0); /* simulate 'call' pushing return address */
        xbe_entry_point();
        fprintf(stderr, "xbe_entry_point returned normally (g_eax=0x%08X)\n", g_eax);
    } __except(
        (fprintf(stderr, "CRASH in xbe_entry_point: exception 0x%08lX\n",
                 GetExceptionInformation()->ExceptionRecord->ExceptionCode),
         fprintf(stderr, "  Fault address: 0x%p\n",
                 GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
         GetExceptionInformation()->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            ? fprintf(stderr, "  Access violation %s address 0x%p\n",
                      GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                      (void*)GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1])
            : 0,
         EXCEPTION_EXECUTE_HANDLER)
    ) {
        DWORD code = GetExceptionCode();
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            fprintf(stderr, "  Registers: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                    g_eax, g_ecx, g_edx, g_esp);
            fprintf(stderr, "  ebx=0x%08X esi=0x%08X edi=0x%08X seh_ebp=0x%08X\n",
                    g_ebx, g_esi, g_edi, g_seh_ebp);
            /* Dump simulated Xbox stack to find return addresses */
            {
                int j;
                uint32_t sp = g_esp;
                fprintf(stderr, "  Xbox stack dump (16 dwords from esp=0x%08X):\n", sp);
                for (j = 0; j < 16 && sp + j*4 < XBOX_STACK_TOP; j++) {
                    uint32_t val = MEM32(sp + j*4);
                    fprintf(stderr, "    [esp+%02X] 0x%08X", j*4, val);
                    /* Mark values that look like code addresses */
                    if (val >= 0x00010000 && val < 0x002CE000)
                        fprintf(stderr, " <- .text");
                    fprintf(stderr, "\n");
                }
            }
            break;
        case EXCEPTION_STACK_OVERFLOW:
            fprintf(stderr, "  Stack overflow (infinite recursion?)\n");
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            fprintf(stderr, "  Integer divide by zero\n");
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            fprintf(stderr, "  Illegal instruction (tried to execute Xbox code VA as native?)\n");
            break;
        default:
            fprintf(stderr, "  Exception code: 0x%08lX\n", code);
            break;
        }
    }
#else /* !_WIN32 -- no SEH; call directly (crashes propagate as SIGSEGV) */
    PUSH32(g_esp, 0);
    xbe_entry_point();
    fprintf(stderr, "xbe_entry_point returned normally (g_eax=0x%08X)\n", g_eax);
#endif

    /* Run the game window loop */
    game_loop();

    /* Clean up */
    shutdown_subsystems();

    fprintf(stderr, "\nBurnout 3 exited normally.\n");
    return 0;
}

#if !defined(_WIN32)
/* POSIX entry point. Builds the WinMain-style command-line string from
 * argv (just the first arg if any) and dispatches into WinMain. */
int main(int argc, char **argv)
{
    LPSTR cmd = (argc > 1) ? (LPSTR)argv[1] : (LPSTR)"";
    return WinMain(NULL, NULL, cmd, 0);
}
#endif
