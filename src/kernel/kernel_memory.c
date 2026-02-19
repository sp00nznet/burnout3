/*
 * kernel_memory.c - Xbox Memory Management
 *
 * Implements Mm* and NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory
 * using Win32 VirtualAlloc/VirtualFree/VirtualQuery.
 *
 * Xbox contiguous memory (MmAllocateContiguousMemory) is used for GPU-accessible
 * buffers. On Windows, actual GPU resources are handled by our D3D11 layer;
 * these allocations just need to return valid CPU-accessible memory.
 */

#include "kernel.h"
#include <malloc.h>

/* ============================================================================
 * Helper: Xbox protect flags → Win32 protect flags
 * ============================================================================ */

static DWORD xbox_protect_to_win32(ULONG xbox_protect)
{
    /* Xbox uses the same PAGE_* constants as Windows NT */
    switch (xbox_protect & 0xFF) {
        case 0x01: return PAGE_NOACCESS;
        case 0x02: return PAGE_READONLY;
        case 0x04: return PAGE_READWRITE;
        case 0x08: return PAGE_WRITECOPY;
        case 0x10: return PAGE_EXECUTE;
        case 0x20: return PAGE_EXECUTE_READ;
        case 0x40: return PAGE_EXECUTE_READWRITE;
        default:   return PAGE_READWRITE;
    }
}

/* ============================================================================
 * Contiguous Memory (GPU-accessible on Xbox)
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

PVOID __stdcall xbox_MmAllocateContiguousMemoryEx(
    ULONG NumberOfBytes,
    ULONG_PTR LowestAcceptableAddress,
    ULONG_PTR HighestAcceptableAddress,
    ULONG Alignment,
    ULONG Protect)
{
    /*
     * Xbox requests physically contiguous, aligned memory for GPU use.
     * We can't guarantee physical contiguity on Windows, but the game's
     * CPU-side code just needs a valid pointer. GPU resources are handled
     * separately by our D3D11 layer.
     *
     * Use _aligned_malloc for alignment, then VirtualAlloc for a fallback.
     */
    PVOID p = NULL;

    if (Alignment > 0 && (Alignment & (Alignment - 1)) == 0) {
        /* Power-of-2 alignment: use _aligned_malloc */
        p = _aligned_malloc(NumberOfBytes, Alignment);
        if (p)
            memset(p, 0, NumberOfBytes);
    }

    if (!p) {
        /* Fallback: page-aligned VirtualAlloc */
        p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                         xbox_protect_to_win32(Protect));
    }

    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemoryEx(%u, align=%u) = %p",
        NumberOfBytes, Alignment, p);
    return p;
}

VOID __stdcall xbox_MmFreeContiguousMemory(PVOID BaseAddress)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeContiguousMemory(%p)", BaseAddress);
    if (!BaseAddress)
        return;

    /*
     * Determine if this was allocated with _aligned_malloc or VirtualAlloc.
     * We use VirtualQuery to check: if it's a VirtualAlloc'd region,
     * AllocationBase will equal the pointer (page-aligned).
     */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) &&
        mbi.AllocationBase == BaseAddress &&
        mbi.State == MEM_COMMIT) {
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
    } else {
        _aligned_free(BaseAddress);
    }
}

/* ============================================================================
 * System Memory
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                           xbox_protect_to_win32(Protect));
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateSystemMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeSystemMemory(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* ============================================================================
 * Memory Query & Protection
 * ============================================================================ */

NTSTATUS __stdcall xbox_MmQueryStatistics(PXBOX_MM_STATISTICS MemoryStatistics)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!MemoryStatistics)
        return STATUS_INVALID_PARAMETER;

    if (!GlobalMemoryStatusEx(&ms))
        return STATUS_UNSUCCESSFUL;

    memset(MemoryStatistics, 0, sizeof(XBOX_MM_STATISTICS));
    MemoryStatistics->Length = sizeof(XBOX_MM_STATISTICS);

    /* Xbox has 64MB RAM. Report plausible values. */
    ULONG page_size = 4096;
    MemoryStatistics->TotalPhysicalPages = 64 * 1024 * 1024 / page_size; /* 16384 pages */
    MemoryStatistics->AvailablePages = (ULONG)(ms.ullAvailPhys / page_size);
    if (MemoryStatistics->AvailablePages > MemoryStatistics->TotalPhysicalPages)
        MemoryStatistics->AvailablePages = MemoryStatistics->TotalPhysicalPages / 2;

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
{
    /* GPU register access - handled by our D3D11 layer. Return a dummy buffer. */
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmMapIoSpace(0x%08X, %u) = %p (stub)", (ULONG)PhysicalAddress, NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmUnmapIoSpace(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

ULONG_PTR __stdcall xbox_MmGetPhysicalAddress(PVOID BaseAddress)
{
    /* No physical address translation on Windows - return the VA as a placeholder */
    return (ULONG_PTR)BaseAddress;
}

VOID __stdcall xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    /* Xbox: mark memory to survive soft-reboot. No-op on Windows. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmPersistContiguousMemory(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, Persist);
}

ULONG __stdcall xbox_MmQueryAddressProtect(PVOID VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi))) {
        /* Return the Xbox-equivalent protection */
        return mbi.Protect;
    }
    return PAGE_NOACCESS;
}

VOID __stdcall xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old_protect;
    VirtualProtect(BaseAddress, NumberOfBytes, xbox_protect_to_win32(NewProtect), &old_protect);
    XBOX_TRACE(XBOX_LOG_MEM, "MmSetAddressProtect(%p, %u, 0x%X)", BaseAddress, NumberOfBytes, NewProtect);
}

ULONG __stdcall xbox_MmQueryAllocationSize(PVOID BaseAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi))) {
        return (ULONG)mbi.RegionSize;
    }
    return 0;
}

PVOID __stdcall xbox_MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes)
{
    /* GPU instance memory - handled by D3D11 layer */
    if (NumberOfPaddingBytes)
        *NumberOfPaddingBytes = 0;
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmClaimGpuInstanceMemory(%u) = %p (stub)", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages)
{
    /* Page locking is not meaningful in user mode. No-op. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockBufferPages(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, UnlockPages);
}

VOID __stdcall xbox_MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockPhysicalPage(0x%08X, %d) - stub", (ULONG)PhysicalAddress, UnlockPage);
}

/* ============================================================================
 * Kernel Stack
 * ============================================================================ */

PVOID __stdcall xbox_MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
{
    /* Allocate a stack-like region. Return the TOP of the stack (high address). */
    PVOID base = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base)
        return NULL;

    /* Xbox convention: return pointer to top of stack */
    PVOID stack_top = (PUCHAR)base + NumberOfBytes;
    XBOX_TRACE(XBOX_LOG_MEM, "MmCreateKernelStack(%u) = %p (base=%p)", NumberOfBytes, stack_top, base);
    return stack_top;
}

VOID __stdcall xbox_MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
{
    /* StackLimit is the low address (base of VirtualAlloc), StackBase is the high address */
    XBOX_TRACE(XBOX_LOG_MEM, "MmDeleteKernelStack(base=%p, limit=%p)", StackBase, StackLimit);
    if (StackLimit)
        VirtualFree(StackLimit, 0, MEM_RELEASE);
}

/* ============================================================================
 * Virtual Memory (Nt API)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect)
{
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    PVOID result = VirtualAlloc(*BaseAddress, *RegionSize,
                                AllocationType, xbox_protect_to_win32(Protect));
    if (!result) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtAllocateVirtualMemory failed: base=%p size=%u type=0x%X err=%u",
            *BaseAddress, (ULONG)*RegionSize, AllocationType, GetLastError());
        return STATUS_NO_MEMORY;
    }

    *BaseAddress = result;
    XBOX_TRACE(XBOX_LOG_MEM, "NtAllocateVirtualMemory(%p, %u) = %p",
        *BaseAddress, (ULONG)*RegionSize, result);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtFreeVirtualMemory(
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType)
{
    if (!BaseAddress || !*BaseAddress)
        return STATUS_INVALID_PARAMETER;

    SIZE_T size = (FreeType & MEM_RELEASE) ? 0 : (RegionSize ? *RegionSize : 0);

    if (!VirtualFree(*BaseAddress, size, FreeType)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtFreeVirtualMemory failed: base=%p type=0x%X err=%u",
            *BaseAddress, FreeType, GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    XBOX_TRACE(XBOX_LOG_MEM, "NtFreeVirtualMemory(%p, 0x%X)", *BaseAddress, FreeType);

    if (FreeType & MEM_RELEASE)
        *BaseAddress = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryVirtualMemory(
    PVOID BaseAddress,
    PVOID MemoryInformation,
    ULONG MemoryInformationLength,
    PULONG ReturnLength)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery(BaseAddress, &mbi, sizeof(mbi)))
        return STATUS_INVALID_PARAMETER;

    /*
     * Xbox NtQueryVirtualMemory returns a MEMORY_BASIC_INFORMATION-like struct.
     * Copy what fits into the caller's buffer.
     */
    ULONG copy_size = (MemoryInformationLength < sizeof(mbi)) ? MemoryInformationLength : (ULONG)sizeof(mbi);
    memcpy(MemoryInformation, &mbi, copy_size);

    if (ReturnLength)
        *ReturnLength = copy_size;

    return STATUS_SUCCESS;
}
