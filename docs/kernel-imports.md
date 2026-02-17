# Xbox Kernel Import → Win32 API Mapping

This document maps all 147 Xbox kernel imports used by Burnout 3 to their Windows 11 equivalents.

## Implementation Priority

**P0 - Critical** (game won't boot without these):
- File I/O (Nt*File), Memory (Mm*), Threading (Ke*/Ps*), Synchronization

**P1 - Required** (game won't render/play without these):
- Display (Av*), will be handled by the D3D replacement layer

**P2 - Important** (gameplay features):
- Crypto (Xc*), Runtime (Rtl*), Pool allocator (Ex*)

**P3 - Can stub** (Xbox-specific, not needed on PC):
- Xbox Live (handled by XONLINE/XNET section replacement)
- Hardware info, EEPROM keys, signature keys

---

## File I/O Subsystem

The Xbox uses NT-style file I/O internally. These map cleanly to Win32.

### Path Translation
Xbox paths use device-style notation:
- `\Device\Harddisk0\Partition1\` → game install directory
- `\Device\CdRom0\` → disc root (our `Burnout 3 Takedown/` directory)
- `T:\` → title persistent storage (save games)
- `U:\` → user storage
- `Z:\` → title utility data (cache)

**Strategy**: Implement a path translation layer:
```c
// Xbox path → Windows path mapping
"\\Device\\CdRom0\\"     → "<game_dir>\\Burnout 3 Takedown\\"
"D:\\"                    → "<game_dir>\\Burnout 3 Takedown\\"
"T:\\"                    → "<save_dir>\\TitleData\\"
"U:\\"                    → "<save_dir>\\UserData\\"
"Z:\\"                    → "<save_dir>\\Cache\\"
```

### NtCreateFile
```c
// Xbox
NTSTATUS NtCreateFile(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions
);

// Win32 replacement
HANDLE CreateFileW(path, access, share, NULL, disposition, flags, NULL);
```

### NtReadFile / NtWriteFile
```c
// Direct mapping to ReadFile/WriteFile
// Xbox NtReadFile → Win32 ReadFile
// Xbox NtWriteFile → Win32 WriteFile
// Handle async I/O via OVERLAPPED if the Xbox version uses async
```

---

## Memory Management

### Contiguous Memory (GPU-accessible)
Xbox uses contiguous physical memory for GPU resources. On Windows, this maps to:
- GPU resources → D3D11 buffers/textures (managed by our D3D layer)
- CPU-side contiguous → `VirtualAlloc` with alignment

```c
// MmAllocateContiguousMemory(size)
void* xbox_MmAllocateContiguousMemory(ULONG size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

// MmAllocateContiguousMemoryEx(size, low, high, align, protect)
void* xbox_MmAllocateContiguousMemoryEx(
    ULONG size, ULONG_PTR low, ULONG_PTR high, ULONG align, ULONG protect) {
    // Use _aligned_malloc for alignment, VirtualAlloc for protection
    void* p = _aligned_malloc(size, align);
    return p;
}
```

### System Memory
```c
// MmAllocateSystemMemory → VirtualAlloc
// MmFreeSystemMemory → VirtualFree
// MmQueryStatistics → GlobalMemoryStatusEx
```

### I/O Space Mapping
```c
// MmMapIoSpace - maps physical GPU registers
// On Windows, GPU access goes through D3D11 - stub this
void* xbox_MmMapIoSpace(ULONG_PTR phys, ULONG size, ULONG protect) {
    // GPU register access - handled by D3D11 layer
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
```

---

## Threading & Synchronization

### Thread Creation
```c
// PsCreateSystemThreadEx → CreateThread
HANDLE xbox_PsCreateSystemThreadEx(
    ..., PKSTART_ROUTINE StartRoutine, PVOID StartContext, ...) {
    return CreateThread(NULL, stackSize, StartRoutine, StartContext, 0, NULL);
}
```

### Timers & Delays
```c
// KeDelayExecutionThread → Sleep/SleepEx
// Note: Xbox uses 100ns intervals, need conversion
void xbox_KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval) {
    LONGLONG ms = -interval->QuadPart / 10000; // 100ns → ms
    SleepEx((DWORD)ms, alertable);
}

// KeQueryPerformanceCounter → QueryPerformanceCounter
// Direct 1:1 mapping, both return LARGE_INTEGER
```

### Critical Sections
```c
// Rtl*CriticalSection → Win32 CRITICAL_SECTION
// Direct 1:1 mapping - same API signatures
```

### Events / Mutexes / Semaphores
```c
// NtCreateEvent → CreateEventW
// NtSetEvent → SetEvent
// NtClearEvent → ResetEvent
// NtCreateMutant → CreateMutexW
// NtReleaseMutant → ReleaseMutex
// NtCreateSemaphore → CreateSemaphoreW
// NtReleaseSemaphore → ReleaseSemaphore
```

---

## Display / AV

These are handled entirely by the D3D replacement layer:
```c
// AvSetDisplayMode - sets TV output mode
// Replace with window creation + D3D11 swap chain setup
void xbox_AvSetDisplayMode(ULONG mode, ...) {
    // Handled by our D3D11 initialization
    // mode typically 640x480 or 720x480 NTSC
}
```

---

## Pool Allocator

```c
// ExAllocatePool/ExAllocatePoolWithTag → HeapAlloc
void* xbox_ExAllocatePool(ULONG size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void* xbox_ExAllocatePoolWithTag(ULONG size, ULONG tag) {
    // Tag used for debugging - can ignore or log
    return HeapAlloc(GetProcessHeap(), 0, size);
}

// ExFreePool → HeapFree
void xbox_ExFreePool(void* p) {
    HeapFree(GetProcessHeap(), 0, p);
}
```

---

## Runtime Library (Rtl*)

Most map directly to CRT functions:

| Xbox | CRT/Win32 |
|------|-----------|
| RtlAnsiStringToUnicodeString | MultiByteToWideChar |
| RtlUnicodeStringToAnsiString | WideCharToMultiByte |
| RtlInitAnsiString | Manual init |
| RtlInitUnicodeString | Manual init |
| RtlFreeAnsiString | HeapFree |
| RtlFreeUnicodeString | HeapFree |
| RtlEnterCriticalSection | EnterCriticalSection |
| RtlLeaveCriticalSection | LeaveCriticalSection |
| RtlInitializeCriticalSection | InitializeCriticalSection |
| RtlEqualString | strcmp |
| RtlAppendStringToString | strcat |
| RtlCopyString | strcpy |
| RtlCharToInteger | atoi / strtol |
| RtlIntegerToChar | itoa / snprintf |
| RtlNtStatusToDosError | Direct mapping table |
| RtlTimeFieldsToTime | SystemTimeToFileTime |
| RtlTimeToTimeFields | FileTimeToSystemTime |

---

## Crypto (Xc*)

Used for save game signing, Xbox Live authentication, etc.

```c
// XcSHAInit/XcSHAUpdate → BCrypt or OpenSSL SHA-1
// XcHMAC → BCrypt HMAC
// XcRC4Key/XcRC4Crypt → BCrypt RC4

// For save compatibility, must produce identical outputs
// Consider using a lightweight crypto lib (mbedtls or OpenSSL)
```

---

## Xbox Hardware / Identity

All can be stubbed with static data:

```c
// XboxHardwareInfo - return a plausible hardware descriptor
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags = 0x00000020,  // Standard retail Xbox
    .GpuRevision = 0xD2,
    .McpRevision = 0xD4,
};

// XboxKrnlVersion - return XDK 5849 version
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1, .Minor = 0, .Build = 5849, .Qfe = 1
};

// XboxEEPROMKey, XboxHDKey, XboxSignatureKey
// Return dummy keys - not needed for PC operation
```

---

## Section Loading

```c
// XeLoadSection - loads a deferred XBE section into memory
// On Xbox, some sections are loaded on demand
// For our recompilation, all sections are always loaded
NTSTATUS xbox_XeLoadSection(PXBE_SECTION_HEADER section) {
    // No-op if all sections are pre-loaded
    return STATUS_SUCCESS;
}
```

---

## I/O Device Functions

```c
// IoCreateDevice, IoCreateSymbolicLink, IoDeleteDevice
// These are used by XDK libraries internally (DSOUND, D3D)
// Since we replace those libraries entirely, these can be stubbed
```

## HAL Functions

```c
// HalReadSMCTrayState → stub (no disc tray on PC)
// HalReturnToFirmware → ExitProcess (return to dashboard = quit game)
// HalRegisterShutdownNotification → atexit() callback
// HalReadWritePCISpace → stub (no raw PCI access needed)
```
