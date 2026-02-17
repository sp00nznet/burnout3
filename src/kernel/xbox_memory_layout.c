/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include <stdio.h>
#include <string.h>

/* Section info from XBE analysis */

/* .rdata raw file offset */
#define RDATA_RAW_OFFSET        0x0035C000

/* .data raw file offset */
#define DATA_RAW_OFFSET         0x003A3000

/* Additional XDK data sections to map */
static const struct {
    const char *name;
    DWORD va;
    DWORD size;
    DWORD raw_offset;
} g_extra_sections[] = {
    /* DOLBY section */
    { "DOLBY",  0x0076B940, 29056,  0x0040C000 },
    /* XON_RD (Xbox Online read-only data) */
    { "XON_RD", 0x00772AC0, 5416,  0x00414000 },
    /* .data1 */
    { ".data1", 0x00774000, 224,    0x00416000 },
};
#define NUM_EXTRA_SECTIONS (sizeof(g_extra_sections) / sizeof(g_extra_sections[0]))

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_BASE_ADDRESS to the end of the furthest section.
     * The furthest is .data1 at 0x00774000 + 224 = 0x007740E0.
     */
    DWORD map_end = 0x007740E0;  /* Past end of .data1 */
    g_memory_size = map_end - XBOX_BASE_ADDRESS;

    /*
     * Reserve the entire virtual address range.
     * MEM_RESERVE + MEM_COMMIT with PAGE_READWRITE.
     * We request a specific base address - if this fails (e.g., ASLR
     * placed something there), we fall back to relocation.
     */
    g_memory_base = VirtualAlloc(
        (LPVOID)(uintptr_t)XBOX_BASE_ADDRESS,
        g_memory_size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: VirtualAlloc at 0x%08X failed (error %lu)\n",
                XBOX_BASE_ADDRESS, GetLastError());
        fprintf(stderr, "  The required address range may be in use.\n");
        fprintf(stderr, "  Try running without ASLR or with a manifest disabling it.\n");
        return FALSE;
    }

    if ((uintptr_t)g_memory_base != XBOX_BASE_ADDRESS) {
        fprintf(stderr, "xbox_MemoryLayoutInit: VirtualAlloc returned 0x%p instead of 0x%08X\n",
                g_memory_base, XBOX_BASE_ADDRESS);
        VirtualFree(g_memory_base, 0, MEM_RELEASE);
        g_memory_base = NULL;
        return FALSE;
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X\n",
            g_memory_size / 1024, XBOX_BASE_ADDRESS);

    /*
     * Copy .rdata section from XBE.
     */
    if (RDATA_RAW_OFFSET + XBOX_RDATA_SIZE <= xbe_size) {
        void *rdata_dest = (void *)(uintptr_t)XBOX_RDATA_VA;
        memcpy(rdata_dest, xbe + RDATA_RAW_OFFSET, XBOX_RDATA_SIZE);
        fprintf(stderr, "  .rdata: %u bytes at 0x%08X\n", XBOX_RDATA_SIZE, XBOX_RDATA_VA);
    } else {
        fprintf(stderr, "  WARNING: .rdata raw data out of bounds\n");
    }

    /*
     * Copy initialized .data section from XBE.
     * BSS (the rest of .data) is already zeroed by VirtualAlloc.
     */
    if (DATA_RAW_OFFSET + XBOX_DATA_INIT_SIZE <= xbe_size) {
        void *data_dest = (void *)(uintptr_t)XBOX_DATA_VA;
        memcpy(data_dest, xbe + DATA_RAW_OFFSET, XBOX_DATA_INIT_SIZE);
        fprintf(stderr, "  .data: %u bytes initialized, %u bytes BSS at 0x%08X\n",
                XBOX_DATA_INIT_SIZE, XBOX_DATA_SIZE - XBOX_DATA_INIT_SIZE, XBOX_DATA_VA);
    } else {
        fprintf(stderr, "  WARNING: .data raw data out of bounds\n");
    }

    /*
     * Copy extra sections (DOLBY, XON_RD, .data1).
     */
    for (size_t i = 0; i < NUM_EXTRA_SECTIONS; i++) {
        if (g_extra_sections[i].raw_offset + g_extra_sections[i].size <= xbe_size) {
            void *dest = (void *)(uintptr_t)g_extra_sections[i].va;
            memcpy(dest, xbe + g_extra_sections[i].raw_offset, g_extra_sections[i].size);
            fprintf(stderr, "  %s: %u bytes at 0x%08X\n",
                    g_extra_sections[i].name, g_extra_sections[i].size, g_extra_sections[i].va);
        }
    }

    /*
     * Set .rdata as read-only.
     * This helps catch accidental writes to constants early.
     */
    VirtualProtect(
        (LPVOID)(uintptr_t)XBOX_RDATA_VA,
        XBOX_RDATA_SIZE,
        PAGE_READONLY,
        &old_protect
    );

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_memory_base) {
        VirtualFree(g_memory_base, 0, MEM_RELEASE);
        g_memory_base = NULL;
        g_memory_size = 0;
        fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
    }
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}
