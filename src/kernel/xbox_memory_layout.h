/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * The game's code and data were linked expecting specific address ranges:
 *
 *   0x00010000 - 0x002BD000  .text (code)       ~2.73 MB
 *   0x002CC200 - 0x00362AE0  XDK library code   ~600 KB
 *   0x0036B7C0 - 0x003B2354  .rdata (constants) ~280 KB
 *   0x003B2360 - 0x0076F000  .data + BSS        ~3.9 MB
 *
 * On Windows, we need to:
 * 1. Reserve the same virtual address range (0x00010000+)
 * 2. Map sections to their expected addresses
 * 3. Handle the fact that Xbox has no address space layout randomization
 * 4. Provide contiguous memory for GPU resources (textures, VBs)
 *
 * Strategy:
 * - Use VirtualAlloc with specific base addresses to place sections
 * - The recompiled code uses the same addresses for globals and data
 * - GPU memory (D3D textures, etc.) is managed separately by D3D11
 * - Stack and heap use normal Windows allocation
 */

#ifndef BURNOUT3_XBOX_MEMORY_LAYOUT_H
#define BURNOUT3_XBOX_MEMORY_LAYOUT_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of the XBE in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* .text section */
#define XBOX_TEXT_VA            0x00011000
#define XBOX_TEXT_SIZE          2863616     /* 0x002BC000 */

/* .rdata section */
#define XBOX_RDATA_VA           0x0036B7C0
#define XBOX_RDATA_SIZE         289684

/* .data section (includes BSS) */
#define XBOX_DATA_VA            0x003B2360
#define XBOX_DATA_SIZE          3904988
#define XBOX_DATA_INIT_SIZE     424960     /* Initialized data in XBE file */
/* BSS starts at DATA_VA + DATA_INIT_SIZE, zero-initialized */

/* Xbox physical memory */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* End of mapped sections */
#define XBOX_MAP_END            (XBOX_DATA_VA + XBOX_DATA_SIZE)

/* Total virtual space needed (from XBOX_MAP_START, not XBOX_BASE_ADDRESS) */
#define XBOX_MAP_TOTAL_SIZE     (XBOX_MAP_END - XBOX_MAP_START)

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/**
 * Release the reserved Xbox memory layout.
 */
void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/** Size of the simulated Xbox stack (1 MB). */
#define XBOX_STACK_SIZE     (1 * 1024 * 1024)

/** Base VA of the stack area (above last XBE section). */
#define XBOX_STACK_BASE     0x00780000

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XBOX_MEMORY_LAYOUT_H */
