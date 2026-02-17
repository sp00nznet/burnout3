/*
 * kernel_pool.c - Xbox Pool Allocator
 *
 * Implements ExAllocatePool, ExAllocatePoolWithTag, ExFreePool,
 * ExQueryPoolBlockSize using the Win32 process heap.
 *
 * On Xbox, pool memory is kernel-mode nonpaged/paged pool.
 * Since we run entirely in user mode on Windows, we just use HeapAlloc.
 */

#include "kernel.h"

PVOID __stdcall xbox_ExAllocatePool(ULONG NumberOfBytes)
{
    PVOID p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NumberOfBytes);
    XBOX_TRACE(XBOX_LOG_POOL, "ExAllocatePool(%u) = %p", NumberOfBytes, p);
    return p;
}

PVOID __stdcall xbox_ExAllocatePoolWithTag(ULONG NumberOfBytes, ULONG Tag)
{
    PVOID p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NumberOfBytes);
    XBOX_TRACE(XBOX_LOG_POOL, "ExAllocatePoolWithTag(%u, '%c%c%c%c') = %p",
        NumberOfBytes,
        (char)(Tag & 0xFF), (char)((Tag >> 8) & 0xFF),
        (char)((Tag >> 16) & 0xFF), (char)((Tag >> 24) & 0xFF),
        p);
    return p;
}

VOID __stdcall xbox_ExFreePool(PVOID P)
{
    XBOX_TRACE(XBOX_LOG_POOL, "ExFreePool(%p)", P);
    if (P) {
        HeapFree(GetProcessHeap(), 0, P);
    }
}

ULONG __stdcall xbox_ExQueryPoolBlockSize(PVOID PoolBlock)
{
    SIZE_T size = 0;
    if (PoolBlock) {
        size = HeapSize(GetProcessHeap(), 0, PoolBlock);
        if (size == (SIZE_T)-1)
            size = 0;
    }
    XBOX_TRACE(XBOX_LOG_POOL, "ExQueryPoolBlockSize(%p) = %u", PoolBlock, (ULONG)size);
    return (ULONG)size;
}
