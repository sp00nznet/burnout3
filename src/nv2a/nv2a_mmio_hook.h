/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * When recompiled code accesses NV2A MMIO registers (0xFD000000+),
 * the access faults because no physical page is mapped. This module
 * decodes the faulting x86-64 instruction, extracts the read/write
 * operation, routes it through the NV2A register handlers, and
 * advances RIP past the instruction.
 *
 * This is the key bridge between recompiled Xbox D3D8 code and
 * the xemu NV2A GPU emulation.
 */

#ifndef BURNOUT3_NV2A_MMIO_HOOK_H
#define BURNOUT3_NV2A_MMIO_HOOK_H

#include "platform/xbox_winnt.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Initialize the NV2A GPU subsystem.
 * Call this during startup before the game code runs.
 * Allocates VRAM, RAMIN, and initializes register state.
 */
void nv2a_hook_init(ptrdiff_t xbox_mem_offset);

/*
 * Handle an NV2A MMIO access fault.
 *
 * Called from the VEH handler when fault_xbox_va is in GPU register
 * space (0xFD000000-0xFDFFFFFF).
 *
 * Decodes the faulting instruction, performs the MMIO read/write
 * through the NV2A register handlers, updates CPU context, and
 * returns true if handled successfully.
 *
 * Returns false if the instruction couldn't be decoded (caller
 * should fall back to allocating a zero page).
 */
bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write);

/*
 * Handle a GPU framebuffer/push buffer access.
 *
 * For 0xF0000000-0xFCFFFFFF range. Currently just allocates
 * pages backed by NV2A VRAM where appropriate.
 *
 * Returns true if handled.
 */
bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va);

#endif /* BURNOUT3_NV2A_MMIO_HOOK_H */
