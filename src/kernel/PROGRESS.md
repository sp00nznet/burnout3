# Xbox Kernel Replacement Layer - Implementation Progress

**Last updated: 2026-02-17**

## COMPLETED (6/15 files):
1. **kernel.h** - Master header with all Xbox NT types, status codes, structs, and all function prototypes
2. **kernel_path.c** - Xbox→Windows path translation (D:\, T:\, U:\, Z:\, \Device\CdRom0\)
3. **kernel_pool.c** - ExAllocatePool, ExAllocatePoolWithTag, ExFreePool, ExQueryPoolBlockSize
4. **kernel_rtl.c** - Critical sections, string init/conversion, NTSTATUS mapping, time, sprintf, exceptions
5. **kernel_memory.c** - Mm* contiguous/system memory, NtAllocateVirtualMemory, NtFreeVirtualMemory, NtQueryVirtualMemory
6. **kernel_file.c** - NtCreateFile, NtOpenFile, NtReadFile, NtWriteFile, NtClose, NtDeleteFile, Query/Set info, directory enumeration, volume info, flush, IoCreateFile, symbolic links

## REMAINING (9/15 files):
7. **kernel_thread.c** - PsCreateSystemThreadEx, PsTerminateSystemThread, KeDelayExecutionThread, priority, NtYieldExecution, NtDuplicateObject
8. **kernel_sync.c** - Events, semaphores, wait functions, timers, DPCs, KeSynchronizeExecution
9. **kernel_hal.c** - IRQL simulation, perf counters, KeQuerySystemTime, KeBugCheck, floating point, HAL stubs, KeTickCount
10. **kernel_xbox.c** - HardwareInfo, KrnlVersion, dummy keys, XeLoadSection, PhyGetLinkState, LaunchDataPage
11. **kernel_ob.c** - Object manager ref counting stubs, type object pointers
12. **kernel_io.c** - I/O manager device/packet stubs, type object pointers
13. **kernel_crypto.c** - SHA-1, RC4, HMAC, RSA via BCrypt API
14. **kernel_thunks.c** - 147-entry thunk table wiring + xbox_kernel_init/shutdown + xbox_log implementation
15. **CMakeLists.txt** - Static library build config

## KEY REFERENCE:
- Thunk table: 147 entries at VA 0x0036B7C0 (each 4 bytes, ending at 0x0036BA08)
- 315 kernel call sites across 87 unique functions
- All ordinals/thunk addresses in: tools/xbe_parser/burnout3_analysis.json
- All prototypes already declared in kernel.h
