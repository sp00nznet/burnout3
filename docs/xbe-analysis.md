# XBE Analysis - Burnout 3: Takedown (default.xbe)

## File Identity

| Field | Value |
|-------|-------|
| File | `default.xbe` |
| Size | 4,308,992 bytes (4.11 MB) |
| Magic | `XBEH` |
| Title | Burnout 3 |
| Title ID | `0x4541005B` (EA = 0x4541, Game = 0x005B) |
| Build Path | `C:\work\B3\CG1Code\GameProjects\VisualStudio.NET\Burnout3\XboxExternal\Burnout3_External.exe` |
| Build Date | 2004-07-29 19:24:09 UTC |
| Certificate Date | 2004-08-06 16:28:48 UTC |
| XDK Version | 5849 (all libraries) |
| Region | North America |
| Media | DVD_X2 |
| Type | Retail |

## Memory Layout

```
Virtual Address Space (0x00010000 - 0x00778800):

0x00010000 ┌──────────────────────┐
           │   XBE Header         │
0x00011000 ├──────────────────────┤
           │   .text (CODE)       │  2.73 MB - Game code + CRT
           │   Entry: 0x001D2807  │
0x002CC200 ├──────────────────────┤
           │   XMV                │  159 KB - Video decoder
0x002F3F40 ├──────────────────────┤
           │   DSOUND             │  51 KB - DirectSound
0x00300D00 ├──────────────────────┤
           │   WMADEC             │  103 KB - WMA decoder
0x0031AA80 ├──────────────────────┤
           │   XONLINE            │  121 KB - Xbox Live
0x003391E0 ├──────────────────────┤
           │   XNET               │  76 KB - Networking
0x0034C2E0 ├──────────────────────┤
           │   D3D                │  81 KB - Direct3D 8 (LTCG)
0x00360A60 ├──────────────────────┤
           │   XGRPH              │  8 KB - Graphics helpers
0x00362AE0 ├──────────────────────┤
           │   XPP                │  35 KB - Peripherals
0x0036B7C0 ├──────────────────────┤
           │   .rdata             │  282 KB - Read-only data
           │   (Kernel thunks     │   + import tables)
0x003B2360 ├──────────────────────┤
           │   .data              │  3.72 MB virtual (417 KB on disk)
           │   (3.32 MB BSS)      │   Large global arrays
0x0076B940 ├──────────────────────┤
           │   DOLBY              │  28 KB - Dolby processing
0x00772AC0 ├──────────────────────┤
           │   XON_RD             │  5 KB - Xbox Live read-only
0x00774000 ├──────────────────────┤
           │   .data1             │  224 bytes
0x007740E0 ├──────────────────────┤
           │   $$XTIMAGE          │  10 KB - Title image
0x007768E0 ├──────────────────────┤
           │   $$XSIMAGE          │  4 KB - Save image
0x007778E0 ├──────────────────────┤
           │   .XTLID             │  2 KB - Title ID data
0x00778800 └──────────────────────┘
```

## Section Details

### `.text` - Main Code Section
- **Virtual Address**: `0x00011000`
- **Size**: `0x002BB200` (2,863,616 bytes, 2.73 MB)
- **Raw Offset**: `0x00001000`
- **Flags**: Preload, Executable, Head Read-Only
- **Contents**: All game logic, RenderWare engine code, statically linked C runtime
- **Entry Point**: `0x001D2807` (1.77 MB into section)
- **Note**: First instruction at entry is `mov ecx, [0x00010118]` - reads XBE certificate address

### XDK Library Sections
These are statically linked Xbox SDK libraries. Each has its own code section:

| Section | Purpose | Recompilation Strategy |
|---------|---------|----------------------|
| `XMV` | Xbox Media Video playback | Replace with FFmpeg/libvpx |
| `DSOUND` | DirectSound 3D audio | Replace with XAudio2 |
| `WMADEC` | WMA audio decoder | Replace with Windows Media Foundation or FFmpeg |
| `XONLINE` | Xbox Live services | Stub out or replace with custom networking |
| `XNET` | Xbox network stack | Replace with Winsock2 |
| `D3D` | Direct3D 8 (LTCG compiled) | Replace with D3D11/Vulkan renderer |
| `XGRPH` | Graphics helper functions | Merge into D3D replacement |
| `XPP` | Xbox Peripheral Protocol | Replace with XInput/DirectInput |
| `DOLBY` | Dolby surround processing | Replace with Windows spatial audio |

### `.rdata` - Read-Only Data
- **Virtual Address**: `0x0036B7C0`
- **Size**: `0x00046B94` (282 KB)
- **Contains**: String literals, vtables, kernel import thunk table
- **Kernel Thunks**: Start at `0x0036B7C0` (147 ordinal entries)

### `.data` - Global Data
- **Virtual Address**: `0x003B2360`
- **Virtual Size**: `0x003B95DC` (3.72 MB)
- **On-Disk Size**: `0x0006844C` (417 KB)
- **BSS (zero-init)**: ~3.32 MB
- **Contains**: Global variables, large buffers (likely geometry/texture staging)

## Entry Point Analysis

The entry point at `0x001D2807` is located 1.77 MB into the `.text` section. The first instructions are:

```asm
001D2807: mov ecx, [0x00010118]  ; Load certificate address from XBE header
```

This is the standard XBE entry point pattern - it begins by reading the XBE header to validate the certificate before jumping to the actual `main()` or `WinMain()` equivalent.

The ~1.77 MB of code before the entry point consists of:
- C/C++ runtime library initialization (LIBCMT, LIBCPMT)
- RenderWare engine code (likely the bulk)
- EA framework code
- Game subsystem implementations

## Kernel Import Table

The kernel thunk table at `0x0036B7C0` contains 147 imports from `XBOXKRNL.EXE`. These are resolved at load time by the Xbox kernel.

### Import Categories and Win32 Replacements

#### Display/AV (4 imports)
| Xbox Kernel | Ordinal | Win32 Replacement |
|-------------|---------|-------------------|
| AvGetSavedDataAddress | 4 | N/A (stub) |
| AvSendTVEncoderOption | 12 | N/A (stub) |
| AvSetDisplayMode | 13 | `ChangeDisplaySettingsEx` |
| AvSetSavedDataAddress | 14 | N/A (stub) |

#### Memory Management (11 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| MmAllocateContiguousMemory | `VirtualAlloc` (aligned) |
| MmAllocateContiguousMemoryEx | `VirtualAlloc` + alignment |
| MmAllocateSystemMemory | `VirtualAlloc` |
| MmClaimGpuInstanceMemory | GPU memory mapping (D3D) |
| MmFreeContiguousMemory | `VirtualFree` |
| MmFreeSystemMemory | `VirtualFree` |
| MmMapIoSpace | `MapViewOfFile` or D3D resource |
| MmPersistContiguousMemory | `FlushViewOfFile` |
| MmQueryAddressProtect | `VirtualQuery` |
| MmQueryStatistics | `GlobalMemoryStatusEx` |
| MmSetAddressProtect | `VirtualProtect` |

#### File I/O (NT API - 33 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| NtCreateFile | `CreateFileW` |
| NtReadFile | `ReadFile` |
| NtWriteFile | `WriteFile` |
| NtClose | `CloseHandle` |
| NtQueryInformationFile | `GetFileInformationByHandle` |
| NtQueryVolumeInformationFile | `GetDiskFreeSpaceEx` |
| NtQueryDirectoryFile | `FindFirstFile/FindNextFile` |
| NtCreateDirectoryObject | `CreateDirectoryW` |
| NtWaitForSingleObject | `WaitForSingleObject` |
| NtWaitForMultipleObjects | `WaitForMultipleObjects` |
| NtCreateEvent | `CreateEventW` |
| NtSetEvent | `SetEvent` |
| NtClearEvent | `ResetEvent` |
| NtCreateMutant | `CreateMutexW` |
| NtReleaseMutant | `ReleaseMutex` |
| NtCreateSemaphore | `CreateSemaphoreW` |
| NtReleaseSemaphore | `ReleaseSemaphore` |

#### Threading / Synchronization (Ke* - 22 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| KeDelayExecutionThread | `Sleep` / `SleepEx` |
| KeQueryPerformanceCounter | `QueryPerformanceCounter` |
| KeQueryPerformanceFrequency | `QueryPerformanceFrequency` |
| KeQuerySystemTime | `GetSystemTimeAsFileTime` |
| KeSetTimer/KeSetTimerEx | `CreateTimerQueueTimer` |
| KeCancelTimer | `DeleteTimerQueueTimer` |
| KeWaitForSingleObject | `WaitForSingleObject` |
| KeInitializeDpc | Custom thread pool |
| KeSetEvent/KeResetEvent | `SetEvent/ResetEvent` |
| KeInitializeInterrupt | N/A (stub) |
| KeConnectInterrupt | N/A (stub) |

#### Process/Thread Management (Ps* - 4 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| PsCreateSystemThreadEx | `CreateThread` |
| PsTerminateSystemThread | `ExitThread` |
| PsQueryStatistics | `GetProcessMemoryInfo` |
| PsSetCreateThreadNotifyRoutine | N/A (stub) |

#### Runtime Library (Rtl* - 18 imports)
Most Rtl* functions have direct CRT equivalents:
- `RtlAnsiStringToUnicodeString` → `MultiByteToWideChar`
- `RtlInitAnsiString/RtlInitUnicodeString` → string initialization
- `RtlEnterCriticalSection/RtlLeaveCriticalSection` → `EnterCriticalSection/LeaveCriticalSection`
- `RtlInitializeCriticalSection` → `InitializeCriticalSection`
- Memory operations map to CRT `memcpy/memset/memmove`

#### Pool Allocator (Ex* - 5 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| ExAllocatePool | `HeapAlloc` |
| ExAllocatePoolWithTag | `HeapAlloc` (tag for debugging) |
| ExFreePool | `HeapFree` |
| ExRegisterThreadNotification | N/A (stub) |
| ExSaveNonVolatileSetting | Registry or file-based config |

#### Crypto (Xc* - 4 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| XcSHAInit/XcSHAUpdate | `BCryptHash` (SHA) |
| XcHMAC | `BCryptHash` (HMAC) |
| XcRC4Key/XcRC4Crypt | `BCryptEncrypt` (RC4) |

#### Xbox-Specific (11 imports)
| Xbox Kernel | Win32 Replacement |
|-------------|-------------------|
| XboxHardwareInfo | Hardcoded struct |
| XboxEEPROMKey | N/A (stub with dummy key) |
| XboxHDKey | N/A (stub with dummy key) |
| XboxKrnlVersion | Hardcoded version struct |
| XboxSignatureKey | N/A (stub with dummy key) |
| XeLoadSection | `LoadLibrary` or memory-mapped section |
| XeUnloadSection | `FreeLibrary` or unmap |

## TLS (Thread Local Storage)

- TLS Index at `0x0041A7D4`
- Zero-fill size: 144 bytes (0x90)
- No TLS callbacks
- Need to replicate with `__declspec(thread)` or `TlsAlloc/TlsGetValue`

## Library Versions

All 11 libraries are XDK version **5849**:

| Library | Description |
|---------|-------------|
| XAPILIB | Xbox Application API |
| XMV | Xbox Media Video |
| DSOUND | DirectSound |
| XBOXKRNL | Xbox Kernel |
| XONLINES | Xbox Live (Secure) |
| XVOICE | Xbox Voice Chat |
| LIBCMT | C Runtime (Multi-threaded) |
| LIBCPMT | C++ Runtime (Multi-threaded) |
| LIBC | C Standard Library |
| D3D8LTCG | Direct3D 8 (Link-Time Code Gen) |
| XGRAPHCL | Xbox Graphics (Client) |

## Other XBE Files

### `update.xbe` (2.2 MB)
Game update/patch executable. May contain bug fixes or content updates. Should be analyzed to determine if it patches `default.xbe` code.

### `dashupdate.xbe` (55.7 MB)
Xbox dashboard update. Not relevant to game recompilation - this is the standard Xbox system update bundled on game discs.

## Key Challenges

1. **LTCG in D3D section**: Link-Time Code Generation means D3D library code was heavily optimized (inlined, reordered). Standard XDK library signature matching will not work for this section.

2. **Large BSS section**: 3.32 MB of uninitialized global data suggests complex global state that needs careful mapping.

3. **Entry point deep in .text**: The entry point is 1.77 MB into the code section, meaning substantial library code precedes it.

4. **Xbox Live integration**: The game uses XONLINE, XNET, and XVOICE - these need to be stubbed out or replaced.

5. **RenderWare engine**: Criterion's custom RenderWare fork is statically linked. No separate DLLs to swap - all engine code is in `.text`.
