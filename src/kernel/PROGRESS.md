# Xbox Kernel Replacement Layer - Implementation Progress

**Last updated: 2026-02-17**

## STATUS: COMPLETE (15/15 files)

All 147 Xbox kernel imports are now implemented with Win32 replacements.

## Implementation Summary:

1. **kernel.h** - Master header with all Xbox NT types, status codes, structs, and all function prototypes
2. **kernel_path.c** - Xbox→Windows path translation (D:\, T:\, U:\, Z:\, \Device\CdRom0\)
3. **kernel_pool.c** - ExAllocatePool, ExAllocatePoolWithTag, ExFreePool, ExQueryPoolBlockSize
4. **kernel_rtl.c** - Critical sections, string init/conversion, NTSTATUS mapping, time, sprintf, exceptions
5. **kernel_memory.c** - Mm* contiguous/system memory, NtAllocateVirtualMemory, NtFreeVirtualMemory, NtQueryVirtualMemory
6. **kernel_file.c** - NtCreateFile, NtOpenFile, NtReadFile, NtWriteFile, NtClose, NtDeleteFile, Query/Set info, directory enumeration, volume info, flush, IoCreateFile, symbolic links
7. **kernel_thread.c** - PsCreateSystemThreadEx, PsTerminateSystemThread, KeDelayExecutionThread, priority, NtYieldExecution, NtDuplicateObject
8. **kernel_sync.c** - Events, semaphores, wait functions (single/multiple), timers (with DPC support), DPCs (via thread pool), KeSynchronizeExecution
9. **kernel_hal.c** - IRQL simulation (TLS-based), perf counters, KeQuerySystemTime, KeBugCheck, floating point stubs, HAL hardware stubs, KeTickCount, interrupts, AV/display stubs
10. **kernel_xbox.c** - HardwareInfo, KrnlVersion (5849), dummy keys, XeLoadSection/XeUnloadSection, PhyGetLinkState, LaunchDataPage, XeImageFileName
11. **kernel_ob.c** - Object manager ref counting (ObfReferenceObject/ObfDereferenceObject), ObReferenceObjectByHandle/Name, type object pointers
12. **kernel_io.c** - IoCreateDevice/DeleteDevice, IRP stubs, I/O completion, synchronous device I/O stubs, type object pointers
13. **kernel_crypto.c** - SHA-1 (software, byte-exact), RC4 (software), HMAC-SHA1 (RFC 2104), PKI stubs (XcVerifyPKCS1Signature returns TRUE), DES/block cipher stubs
14. **kernel_thunks.c** - 147-entry thunk table wiring, ordinal→function resolver, xbox_kernel_init/shutdown, xbox_log implementation with file/stderr output
15. **CMakeLists.txt** - Static library build config (MSVC + MinGW support)

## Architecture Notes:

- **Thunk table**: 147 entries at VA 0x0036B7C0, all resolved to xbox_* implementations
- **Data exports**: 17 ordinals export data pointers (HardwareInfo, keys, type objects, KeTickCount, etc.)
- **Function exports**: 130 ordinals export function pointers
- **Calling conventions**: __stdcall (most), __fastcall (KfRaiseIrql, KfLowerIrql, ObfRef/Deref), __cdecl (Rtl*printf)
- **IRQL**: Simulated via thread-local storage (tracks level but doesn't enforce preemption)
- **Timers**: Win32 timer queue with DPC callback support
- **DPCs**: Executed via Windows thread pool (TrySubmitThreadpoolCallback)
- **Crypto**: Software SHA-1/RC4/HMAC for byte-exact Xbox compatibility
- **Logging**: Configurable via XBOX_LOG_LEVEL env var (0=ERROR..4=TRACE), outputs to xbox_kernel.log + stderr

## KEY REFERENCE:
- 315 kernel call sites across 87 unique functions in the game code
- All ordinals/thunk addresses in: tools/xbe_parser/burnout3_analysis.json
- All prototypes declared in kernel.h
