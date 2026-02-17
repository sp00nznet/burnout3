/*
 * kernel.h - Xbox Kernel Replacement Layer
 *
 * Master header for the Xbox kernel function replacements.
 * Defines Xbox NT types, status codes, and all xbox_* function prototypes.
 *
 * The original Xbox kernel uses a subset of the Windows NT kernel API
 * with some Xbox-specific extensions. Key differences from Windows NT:
 *   - OBJECT_ATTRIBUTES.ObjectName is PANSI_STRING (not PUNICODE_STRING)
 *   - File paths use Xbox device notation (\Device\CdRom0\, D:\, T:\, etc.)
 *   - 32-bit x86 only, __stdcall calling convention for kernel functions
 *   - IRQL levels used for synchronization (simulated on Windows)
 */

#ifndef XBOX_KERNEL_H
#define XBOX_KERNEL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Xbox NT Type Definitions
 * ============================================================================ */

typedef LONG NTSTATUS;
typedef UCHAR KIRQL, *PKIRQL;
typedef CCHAR KPROCESSOR_MODE;
typedef LONG KPRIORITY;

/* Processor modes */
#define KernelMode  0
#define UserMode    1

/* IRQL levels (Xbox uses same NT IRQL model) */
#define PASSIVE_LEVEL   0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2

/* NTSTATUS codes */
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103L)
#define STATUS_BUFFER_OVERFLOW          ((NTSTATUS)0x80000005L)
#define STATUS_NO_MORE_FILES            ((NTSTATUS)0x80000006L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_HANDLE           ((NTSTATUS)0xC0000008L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_NO_SUCH_FILE             ((NTSTATUS)0xC000000FL)
#define STATUS_END_OF_FILE              ((NTSTATUS)0xC0000011L)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017L)
#define STATUS_ALREADY_COMMITTED        ((NTSTATUS)0xC0000021L)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022L)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034L)
#define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)0xC0000035L)
#define STATUS_OBJECT_PATH_NOT_FOUND    ((NTSTATUS)0xC000003AL)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_INTERNAL_ERROR           ((NTSTATUS)0xC00000E5L)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120L)

#define NT_SUCCESS(Status)  (((NTSTATUS)(Status)) >= 0)

/* Xbox ANSI_STRING (Xbox kernel uses ANSI, not Unicode, for paths) */
typedef struct _XBOX_ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} XBOX_ANSI_STRING, *PXBOX_ANSI_STRING;

/* Xbox UNICODE_STRING */
typedef struct _XBOX_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWCHAR Buffer;
} XBOX_UNICODE_STRING, *PXBOX_UNICODE_STRING;

/* Xbox OBJECT_ATTRIBUTES - uses ANSI_STRING for ObjectName */
typedef struct _XBOX_OBJECT_ATTRIBUTES {
    HANDLE          RootDirectory;
    PXBOX_ANSI_STRING ObjectName;
    ULONG           Attributes;
} XBOX_OBJECT_ATTRIBUTES, *PXBOX_OBJECT_ATTRIBUTES;

/* I/O Status Block */
typedef struct _XBOX_IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} XBOX_IO_STATUS_BLOCK, *PXBOX_IO_STATUS_BLOCK;

/* File information classes used by NtQueryInformationFile / NtSetInformationFile */
typedef enum _XBOX_FILE_INFORMATION_CLASS {
    XboxFileDirectoryInformation        = 1,
    XboxFileBasicInformation            = 4,
    XboxFileStandardInformation         = 5,
    XboxFileInternalInformation         = 6,
    XboxFilePositionInformation         = 14,
    XboxFileNetworkOpenInformation      = 34,
    XboxFileStreamInformation           = 36,
    XboxFileDispositionInformation      = 13,
    XboxFileRenameInformation           = 10,
    XboxFileEndOfFileInformation        = 20,
    XboxFileAllocationInformation       = 19,
} XBOX_FILE_INFORMATION_CLASS;

/* Volume information classes */
typedef enum _XBOX_FS_INFORMATION_CLASS {
    XboxFileFsVolumeInformation     = 1,
    XboxFileFsSizeInformation       = 3,
    XboxFileFsDeviceInformation     = 4,
    XboxFileFsAttributeInformation  = 5,
    XboxFileFsFullSizeInformation   = 7,
} XBOX_FS_INFORMATION_CLASS;

/* FILE_BASIC_INFORMATION */
typedef struct _XBOX_FILE_BASIC_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG         FileAttributes;
} XBOX_FILE_BASIC_INFORMATION, *PXBOX_FILE_BASIC_INFORMATION;

/* FILE_STANDARD_INFORMATION */
typedef struct _XBOX_FILE_STANDARD_INFORMATION {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         NumberOfLinks;
    BOOLEAN       DeletePending;
    BOOLEAN       Directory;
} XBOX_FILE_STANDARD_INFORMATION, *PXBOX_FILE_STANDARD_INFORMATION;

/* FILE_POSITION_INFORMATION */
typedef struct _XBOX_FILE_POSITION_INFORMATION {
    LARGE_INTEGER CurrentByteOffset;
} XBOX_FILE_POSITION_INFORMATION, *PXBOX_FILE_POSITION_INFORMATION;

/* FILE_END_OF_FILE_INFORMATION */
typedef struct _XBOX_FILE_END_OF_FILE_INFORMATION {
    LARGE_INTEGER EndOfFile;
} XBOX_FILE_END_OF_FILE_INFORMATION, *PXBOX_FILE_END_OF_FILE_INFORMATION;

/* FILE_DISPOSITION_INFORMATION */
typedef struct _XBOX_FILE_DISPOSITION_INFORMATION {
    BOOLEAN DeleteFile;
} XBOX_FILE_DISPOSITION_INFORMATION, *PXBOX_FILE_DISPOSITION_INFORMATION;

/* FILE_NETWORK_OPEN_INFORMATION (used by NtQueryFullAttributesFile) */
typedef struct _XBOX_FILE_NETWORK_OPEN_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         FileAttributes;
} XBOX_FILE_NETWORK_OPEN_INFORMATION, *PXBOX_FILE_NETWORK_OPEN_INFORMATION;

/* FILE_DIRECTORY_INFORMATION (NtQueryDirectoryFile) */
typedef struct _XBOX_FILE_DIRECTORY_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    CHAR          FileName[1]; /* Variable length */
} XBOX_FILE_DIRECTORY_INFORMATION, *PXBOX_FILE_DIRECTORY_INFORMATION;

/* FS_SIZE_INFORMATION */
typedef struct _XBOX_FILE_FS_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG         SectorsPerAllocationUnit;
    ULONG         BytesPerSector;
} XBOX_FILE_FS_SIZE_INFORMATION, *PXBOX_FILE_FS_SIZE_INFORMATION;

/* Memory statistics */
typedef struct _XBOX_MM_STATISTICS {
    ULONG Length;
    ULONG TotalPhysicalPages;
    ULONG AvailablePages;
    ULONG VirtualMemoryBytesCommitted;
    ULONG VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted;
    ULONG PoolPagesCommitted;
    ULONG StackPagesCommitted;
    ULONG ImagePagesCommitted;
} XBOX_MM_STATISTICS, *PXBOX_MM_STATISTICS;

/* Xbox TIME_FIELDS (same as Windows) */
typedef struct _XBOX_TIME_FIELDS {
    SHORT Year;
    SHORT Month;
    SHORT Day;
    SHORT Hour;
    SHORT Minute;
    SHORT Second;
    SHORT Milliseconds;
    SHORT Weekday;
} XBOX_TIME_FIELDS, *PXBOX_TIME_FIELDS;

/* Xbox hardware info */
typedef struct _XBOX_HARDWARE_INFO {
    ULONG Flags;
    UCHAR GpuRevision;
    UCHAR McpRevision;
    UCHAR Reserved[2];
} XBOX_HARDWARE_INFO;

/* Xbox kernel version */
typedef struct _XBOX_KRNL_VERSION {
    USHORT Major;
    USHORT Minor;
    USHORT Build;
    USHORT Qfe;
} XBOX_KRNL_VERSION;

/* XBE Section Header (for XeLoadSection/XeUnloadSection) */
typedef struct _XBE_SECTION_HEADER {
    ULONG Flags;
    PVOID VirtualAddress;
    ULONG VirtualSize;
    ULONG RawAddress;
    ULONG RawSize;
    PCHAR SectionName;
    LONG  SectionReferenceCount;
    PUSHORT HeadSharedPageReferenceCount;
    PUSHORT TailSharedPageReferenceCount;
    BYTE  SectionDigest[20];
} XBE_SECTION_HEADER, *PXBE_SECTION_HEADER;

/* Launch data page */
typedef struct _XBOX_LAUNCH_DATA_PAGE {
    ULONG LaunchDataType;
    ULONG TitleId;
    CHAR  LaunchPath[520];
    ULONG Flags;
    UCHAR Pad[492];
    UCHAR LaunchData[3072];
} XBOX_LAUNCH_DATA_PAGE, *PXBOX_LAUNCH_DATA_PAGE;

/* Timer types */
typedef enum _XBOX_TIMER_TYPE {
    XboxNotificationTimer = 0,
    XboxSynchronizationTimer = 1
} XBOX_TIMER_TYPE;

/* Event types */
typedef enum _XBOX_EVENT_TYPE {
    XboxNotificationEvent = 0,
    XboxSynchronizationEvent = 1
} XBOX_EVENT_TYPE;

/* Forward declarations for kernel objects */
typedef struct _XBOX_KTIMER       XBOX_KTIMER, *PXBOX_KTIMER;
typedef struct _XBOX_KDPC         XBOX_KDPC, *PXBOX_KDPC;
typedef struct _XBOX_KINTERRUPT   XBOX_KINTERRUPT, *PXBOX_KINTERRUPT;

/* DPC routine prototype */
typedef VOID (*PKDEFERRED_ROUTINE)(
    PXBOX_KDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
);

/* Kernel Timer */
struct _XBOX_KTIMER {
    HANDLE      win32_timer;        /* Win32 timer-queue timer handle */
    HANDLE      win32_event;        /* Associated event for signaling */
    PXBOX_KDPC  Dpc;               /* Optional DPC to queue on expiry */
    BOOLEAN     Inserted;
    LONG        Period;
};

/* Deferred Procedure Call */
struct _XBOX_KDPC {
    PKDEFERRED_ROUTINE DeferredRoutine;
    PVOID              DeferredContext;
    PVOID              SystemArgument1;
    PVOID              SystemArgument2;
};

/* Interrupt object */
struct _XBOX_KINTERRUPT {
    PVOID   ServiceRoutine;
    PVOID   ServiceContext;
    ULONG   BusInterruptLevel;
    ULONG   Irql;
    BOOLEAN Connected;
};

/* Thread start routine (Xbox uses __stdcall) */
typedef VOID (__stdcall *PXBOX_SYSTEM_ROUTINE)(PVOID StartContext);

/* Pool types */
#define NonPagedPool    0
#define PagedPool       1

/* File access masks */
#define XBOX_FILE_READ_DATA         0x0001
#define XBOX_FILE_WRITE_DATA        0x0002
#define XBOX_FILE_APPEND_DATA       0x0004
#define XBOX_FILE_READ_ATTRIBUTES   0x0080
#define XBOX_FILE_WRITE_ATTRIBUTES  0x0100
#define XBOX_SYNCHRONIZE            0x00100000
#define XBOX_GENERIC_READ           0x80000000
#define XBOX_GENERIC_WRITE          0x40000000
#define XBOX_GENERIC_ALL            0x10000000
#define XBOX_DELETE                  0x00010000

/* File create disposition */
#define XBOX_FILE_SUPERSEDE         0x00000000
#define XBOX_FILE_OPEN              0x00000001
#define XBOX_FILE_CREATE            0x00000002
#define XBOX_FILE_OPEN_IF           0x00000003
#define XBOX_FILE_OVERWRITE         0x00000004
#define XBOX_FILE_OVERWRITE_IF      0x00000005

/* File create options */
#define XBOX_FILE_DIRECTORY_FILE        0x00000001
#define XBOX_FILE_NON_DIRECTORY_FILE    0x00000040
#define XBOX_FILE_SYNCHRONOUS_IO_ALERT  0x00000010
#define XBOX_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define XBOX_FILE_NO_INTERMEDIATE_BUFFERING 0x00000008

/* File attributes */
#define XBOX_FILE_ATTRIBUTE_READONLY    0x00000001
#define XBOX_FILE_ATTRIBUTE_HIDDEN      0x00000002
#define XBOX_FILE_ATTRIBUTE_SYSTEM      0x00000004
#define XBOX_FILE_ATTRIBUTE_DIRECTORY   0x00000010
#define XBOX_FILE_ATTRIBUTE_ARCHIVE     0x00000020
#define XBOX_FILE_ATTRIBUTE_NORMAL      0x00000080

/* Wait constants */
#define XBOX_WAIT_OBJECT_0  0

/* SHA context for crypto */
typedef struct _XBOX_SHA_CONTEXT {
    ULONG State[5];
    ULONG Count[2];
    UCHAR Buffer[64];
} XBOX_SHA_CONTEXT, *PXBOX_SHA_CONTEXT;

/* RC4 key for crypto */
typedef struct _XBOX_RC4_CONTEXT {
    UCHAR S[256];
    UCHAR i;
    UCHAR j;
} XBOX_RC4_CONTEXT, *PXBOX_RC4_CONTEXT;

/* I/O completion callback */
typedef VOID (*PIO_APC_ROUTINE)(
    PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG Reserved
);

/* ============================================================================
 * Thunk Table
 * ============================================================================ */

/*
 * The thunk table is a 147-entry array of function pointers at VA 0x0036B7C0.
 * Game code calls kernel functions via: call [thunk_addr]
 * We fill this table at startup with our xbox_* implementations.
 */
#define XBOX_KERNEL_THUNK_TABLE_BASE  0x0036B7C0
#define XBOX_KERNEL_THUNK_TABLE_SIZE  147

extern ULONG_PTR xbox_kernel_thunk_table[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Initialize the thunk table - must be called before game code runs */
void xbox_kernel_init(void);
void xbox_kernel_shutdown(void);

/* ============================================================================
 * Path Translation (kernel_path.c)
 * ============================================================================ */

/* Initialize path translation with base directories */
void xbox_path_init(const char* game_dir, const char* save_dir);

/*
 * Translate an Xbox path to a Windows path.
 * Returns TRUE on success, FALSE if the path couldn't be translated.
 * win_path_buf must be at least MAX_PATH characters.
 */
BOOL xbox_translate_path(const char* xbox_path, WCHAR* win_path_buf, DWORD buf_size);

/* ============================================================================
 * Pool Allocator (kernel_pool.c)
 * ============================================================================ */

PVOID   __stdcall xbox_ExAllocatePool(ULONG NumberOfBytes);
PVOID   __stdcall xbox_ExAllocatePoolWithTag(ULONG NumberOfBytes, ULONG Tag);
VOID    __stdcall xbox_ExFreePool(PVOID P);
ULONG   __stdcall xbox_ExQueryPoolBlockSize(PVOID PoolBlock);

/* ============================================================================
 * Runtime Library (kernel_rtl.c)
 * ============================================================================ */

VOID    __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString);
VOID    __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString);

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString);

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString);

BOOLEAN __stdcall xbox_RtlEqualString(PXBOX_ANSI_STRING String1, PXBOX_ANSI_STRING String2, BOOLEAN CaseInSensitive);
ULONG   __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern);

VOID    __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
VOID    __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
VOID    __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);

ULONG   __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status);

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time);
VOID    __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields);

VOID    __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue);
VOID    __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord);
VOID    __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message);

int     __cdecl   xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...);
int     __cdecl   xbox_RtlSprintf(char* buffer, const char* format, ...);
int     __cdecl   xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr);
int     __cdecl   xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr);

/* ============================================================================
 * Memory Management (kernel_memory.c)
 * ============================================================================ */

PVOID   __stdcall xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes);
PVOID   __stdcall xbox_MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG_PTR LowestAcceptableAddress, ULONG_PTR HighestAcceptableAddress, ULONG Alignment, ULONG Protect);
VOID    __stdcall xbox_MmFreeContiguousMemory(PVOID BaseAddress);

PVOID   __stdcall xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect);
VOID    __stdcall xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes);

NTSTATUS __stdcall xbox_MmQueryStatistics(PXBOX_MM_STATISTICS MemoryStatistics);
PVOID   __stdcall xbox_MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect);
VOID    __stdcall xbox_MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes);
ULONG_PTR __stdcall xbox_MmGetPhysicalAddress(PVOID BaseAddress);

VOID    __stdcall xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist);
ULONG   __stdcall xbox_MmQueryAddressProtect(PVOID VirtualAddress);
VOID    __stdcall xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect);
ULONG   __stdcall xbox_MmQueryAllocationSize(PVOID BaseAddress);
PVOID   __stdcall xbox_MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes);
VOID    __stdcall xbox_MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages);
VOID    __stdcall xbox_MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage);
PVOID   __stdcall xbox_MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread);
VOID    __stdcall xbox_MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit);

NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS __stdcall xbox_NtFreeVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS __stdcall xbox_NtQueryVirtualMemory(PVOID BaseAddress, PVOID MemoryInformation, ULONG MemoryInformationLength, PULONG ReturnLength);

/* ============================================================================
 * File I/O (kernel_file.c)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions);

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions);

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset);

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset);

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle);

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes);

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass);

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass);

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass);

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock);

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation);

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan);

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength);

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength);

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options);

NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength);

/* ============================================================================
 * Threading (kernel_thread.c)
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle, ULONG ThreadExtraSize, ULONG KernelStackSize,
    ULONG TlsDataSize, PULONG ThreadId, PVOID StartContext1, PVOID StartContext2,
    BOOLEAN CreateSuspended, BOOLEAN DebugStack,
    PXBOX_SYSTEM_ROUTINE StartRoutine);

NTSTATUS __stdcall xbox_PsTerminateSystemThread(NTSTATUS ExitStatus);

NTSTATUS __stdcall xbox_KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
LONG     __stdcall xbox_KeSetBasePriorityThread(PVOID Thread, LONG Increment);
LONG     __stdcall xbox_KeQueryBasePriorityThread(PVOID Thread);
NTSTATUS __stdcall xbox_KeAlertThread(PVOID Thread, KPROCESSOR_MODE AlertMode);
NTSTATUS __stdcall xbox_NtYieldExecution(void);
NTSTATUS __stdcall xbox_NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options);

/* ============================================================================
 * Synchronization (kernel_sync.c)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateEvent(PHANDLE EventHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, ULONG EventType, BOOLEAN InitialState);
NTSTATUS __stdcall xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS __stdcall xbox_NtCreateSemaphore(PHANDLE SemaphoreHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, LONG InitialCount, LONG MaximumCount);
NTSTATUS __stdcall xbox_NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount);
NTSTATUS __stdcall xbox_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS __stdcall xbox_NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], ULONG WaitType, BOOLEAN Alertable, PLARGE_INTEGER Timeout);

LONG     __stdcall xbox_KeSetEvent(PVOID Event, LONG Increment, BOOLEAN Wait);
NTSTATUS __stdcall xbox_KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS __stdcall xbox_KeWaitForMultipleObjects(ULONG Count, PVOID Objects[], ULONG WaitType, ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlockArray);

BOOLEAN  __stdcall xbox_KeCancelTimer(PXBOX_KTIMER Timer);
BOOLEAN  __stdcall xbox_KeSetTimer(PXBOX_KTIMER Timer, LARGE_INTEGER DueTime, PXBOX_KDPC Dpc);
BOOLEAN  __stdcall xbox_KeSetTimerEx(PXBOX_KTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PXBOX_KDPC Dpc);
VOID     __stdcall xbox_KeInitializeTimerEx(PXBOX_KTIMER Timer, XBOX_TIMER_TYPE Type);

VOID     __stdcall xbox_KeInitializeDpc(PXBOX_KDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext);
BOOLEAN  __stdcall xbox_KeInsertQueueDpc(PXBOX_KDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
BOOLEAN  __stdcall xbox_KeRemoveQueueDpc(PXBOX_KDPC Dpc);

BOOLEAN  __stdcall xbox_KeSynchronizeExecution(PXBOX_KINTERRUPT Interrupt, PVOID SynchronizeRoutine, PVOID SynchronizeContext);

/* ============================================================================
 * HAL & Hardware (kernel_hal.c)
 * ============================================================================ */

VOID    __stdcall xbox_HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber, PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);
VOID    __stdcall xbox_HalReturnToFirmware(ULONG Routine);
ULONG   __stdcall xbox_HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount);
VOID    __stdcall xbox_HalClearSoftwareInterrupt(KIRQL RequestIrql);
VOID    __stdcall xbox_HalRequestSoftwareInterrupt(KIRQL RequestIrql);
VOID    __stdcall xbox_HalDisableSystemInterrupt(ULONG BusInterruptLevel, KIRQL Irql);
ULONG   __stdcall xbox_HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql);
VOID    __stdcall xbox_HalInitiateShutdown(void);
BOOLEAN __stdcall xbox_HalIsResetOrShutdownPending(void);

KIRQL   __fastcall xbox_KfRaiseIrql(KIRQL NewIrql);
VOID    __fastcall xbox_KfLowerIrql(KIRQL NewIrql);
KIRQL   __stdcall xbox_KeRaiseIrqlToDpcLevel(void);

VOID    __stdcall xbox_KeStallExecutionProcessor(ULONG MicroSeconds);
LARGE_INTEGER __stdcall xbox_KeQueryPerformanceCounter(void);
LARGE_INTEGER __stdcall xbox_KeQueryPerformanceFrequency(void);
VOID    __stdcall xbox_KeQuerySystemTime(PLARGE_INTEGER CurrentTime);

NTSTATUS __stdcall xbox_KeSaveFloatingPointState(PVOID FloatingPointState);
NTSTATUS __stdcall xbox_KeRestoreFloatingPointState(PVOID FloatingPointState);

VOID    __stdcall xbox_KeBugCheck(ULONG BugCheckCode);
VOID    __stdcall xbox_KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR Param1, ULONG_PTR Param2, ULONG_PTR Param3, ULONG_PTR Param4);

VOID    __stdcall xbox_KeInitializeInterrupt(PXBOX_KINTERRUPT Interrupt, PVOID ServiceRoutine, PVOID ServiceContext, ULONG Vector, KIRQL Irql, ULONG InterruptMode, BOOLEAN ShareVector);
BOOLEAN __stdcall xbox_KeConnectInterrupt(PXBOX_KINTERRUPT Interrupt);

/* KeTickCount - exported as a data pointer, not a function */
extern volatile ULONG xbox_KeTickCount;

/* ============================================================================
 * Xbox Identity & Stubs (kernel_xbox.c)
 * ============================================================================ */

extern XBOX_HARDWARE_INFO      xbox_HardwareInfo;
extern XBOX_KRNL_VERSION       xbox_KrnlVersion;
extern UCHAR                   xbox_EEPROMKey[16];
extern UCHAR                   xbox_HDKey[16];
extern UCHAR                   xbox_SignatureKey[16];
extern UCHAR                   xbox_LANKey[16];
extern UCHAR                   xbox_AlternateSignatureKeys[16][16];
extern XBOX_ANSI_STRING        xbox_XeImageFileName;
extern UCHAR                   xbox_XePublicKeyData[284];
extern XBOX_LAUNCH_DATA_PAGE*  xbox_LaunchDataPage;

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section);
NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section);

ULONG   __stdcall xbox_PhyGetLinkState(BOOLEAN Verify);
NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2);

/* ============================================================================
 * Object Manager (kernel_ob.c)
 * ============================================================================ */

VOID    __fastcall xbox_ObfDereferenceObject(PVOID Object);
VOID    __fastcall xbox_ObfReferenceObject(PVOID Object);
NTSTATUS __stdcall xbox_ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object);
NTSTATUS __stdcall xbox_ObReferenceObjectByName(PXBOX_ANSI_STRING ObjectName, ULONG Attributes, PVOID ObjectType, PVOID ParseContext, PVOID* Object);

extern PVOID xbox_PsThreadObjectType;
extern PVOID xbox_ExEventObjectType;

/* ============================================================================
 * I/O Manager (kernel_io.c)
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoCreateDevice(PVOID DriverObject, ULONG DeviceExtensionSize, PXBOX_ANSI_STRING DeviceName, ULONG DeviceType, BOOLEAN Exclusive, PVOID* DeviceObject);
VOID     __stdcall xbox_IoDeleteDevice(PVOID DeviceObject);

extern PVOID xbox_IoDeviceObjectType;
extern PVOID xbox_IoCompletionObjectType;

VOID    __stdcall xbox_IoInitializeIrp(PVOID Irp, USHORT PacketSize, CCHAR StackSize);
VOID    __stdcall xbox_IoStartNextPacket(PVOID DeviceObject, BOOLEAN Cancelable);
VOID    __stdcall xbox_IoStartNextPacketByKey(PVOID DeviceObject, BOOLEAN Cancelable, ULONG Key);
VOID    __stdcall xbox_IoStartPacket(PVOID DeviceObject, PVOID Irp, PULONG Key, PVOID CancelFunction);
NTSTATUS __stdcall xbox_IoSetIoCompletion(PVOID IoCompletion, PVOID KeyContext, PVOID ApcContext, NTSTATUS IoStatus, ULONG_PTR IoStatusInformation);
VOID    __stdcall xbox_IoMarkIrpMustComplete(PVOID Irp);
NTSTATUS __stdcall xbox_IoSynchronousDeviceIoControlRequest(ULONG IoControlCode, PVOID DeviceObject, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength, PULONG ReturnedOutputBufferLength, BOOLEAN InternalDeviceIoControl);
NTSTATUS __stdcall xbox_IoBuildDeviceIoControlRequest(ULONG IoControlCode, PVOID DeviceObject, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength, BOOLEAN InternalDeviceIoControl, HANDLE Event, PXBOX_IO_STATUS_BLOCK IoStatusBlock);
NTSTATUS __stdcall xbox_IoSynchronousFsdRequest(ULONG MajorFunction, PVOID DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset);

/* ============================================================================
 * Crypto (kernel_crypto.c)
 * ============================================================================ */

VOID    __stdcall xbox_XcSHAInit(PXBOX_SHA_CONTEXT ShaContext);
VOID    __stdcall xbox_XcSHAUpdate(PXBOX_SHA_CONTEXT ShaContext, const UCHAR* Input, ULONG InputLength);
VOID    __stdcall xbox_XcSHAFinal(PXBOX_SHA_CONTEXT ShaContext, UCHAR* Digest);

VOID    __stdcall xbox_XcRC4Key(PXBOX_RC4_CONTEXT Rc4Context, ULONG KeyLength, const UCHAR* Key);
VOID    __stdcall xbox_XcRC4Crypt(PXBOX_RC4_CONTEXT Rc4Context, ULONG Length, UCHAR* Data);

VOID    __stdcall xbox_XcHMAC(const UCHAR* Key, ULONG KeyLength, const UCHAR* Data1, ULONG Data1Length, const UCHAR* Data2, ULONG Data2Length, UCHAR* Digest);

ULONG   __stdcall xbox_XcPKGetKeyLen(PVOID PublicKey);
ULONG   __stdcall xbox_XcPKDecPrivate(PVOID PrivateKey, PVOID Input, PVOID Output);
ULONG   __stdcall xbox_XcPKEncPublic(PVOID PublicKey, PVOID Input, PVOID Output);
BOOLEAN __stdcall xbox_XcVerifyPKCS1Signature(PVOID Hash, PVOID PublicKey, PVOID Signature);
ULONG   __stdcall xbox_XcModExp(PULONG Result, PULONG Base, PULONG Exponent, PULONG Modulus, ULONG ModulusLength);

VOID    __stdcall xbox_XcDESKeyParity(PUCHAR Key, ULONG KeyLength);
VOID    __stdcall xbox_XcKeyTable(ULONG CipherSelect, PVOID KeyTable, const UCHAR* Key);
VOID    __stdcall xbox_XcBlockCrypt(ULONG CipherSelect, PVOID Output, PVOID Input, PVOID KeyTable, ULONG Operation);
VOID    __stdcall xbox_XcBlockCryptCBC(ULONG CipherSelect, ULONG OutputLength, PVOID Output, PVOID Input, PVOID KeyTable, ULONG Operation, PVOID FeedbackVector);
VOID    __stdcall xbox_XcCryptService(ULONG Operation, PVOID Param);
VOID    __stdcall xbox_XcUpdateCrypto(PVOID Param1, PVOID Param2);

/* ============================================================================
 * Miscellaneous stubs
 * ============================================================================ */

VOID    __stdcall xbox_WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);
VOID    __stdcall xbox_WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
NTSTATUS __stdcall xbox_NtSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime);

/* Display / AV - handled by D3D layer but declared here for thunk table */
ULONG   __stdcall xbox_AvGetSavedDataAddress(void);
VOID    __stdcall xbox_AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result);
VOID    __stdcall xbox_AvSetSavedDataAddress(ULONG Address);
VOID    __stdcall xbox_AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch, ULONG FrameBuffer);

/* Unknown ordinals - stub */
VOID    __stdcall xbox_Unknown_8(void);
VOID    __stdcall xbox_Unknown_23(void);
VOID    __stdcall xbox_Unknown_42(void);

/* ============================================================================
 * Debug/Logging
 * ============================================================================ */

/* Log levels */
#define XBOX_LOG_ERROR   0
#define XBOX_LOG_WARN    1
#define XBOX_LOG_INFO    2
#define XBOX_LOG_DEBUG   3
#define XBOX_LOG_TRACE   4

void xbox_log(int level, const char* subsystem, const char* fmt, ...);

#ifdef _DEBUG
#define XBOX_TRACE(subsystem, fmt, ...) xbox_log(XBOX_LOG_TRACE, subsystem, fmt, ##__VA_ARGS__)
#else
#define XBOX_TRACE(subsystem, fmt, ...) ((void)0)
#endif

#define XBOX_LOG_FILE    "FILE"
#define XBOX_LOG_MEM     "MEM"
#define XBOX_LOG_THREAD  "THREAD"
#define XBOX_LOG_SYNC    "SYNC"
#define XBOX_LOG_HAL     "HAL"
#define XBOX_LOG_RTL     "RTL"
#define XBOX_LOG_POOL    "POOL"
#define XBOX_LOG_IO      "IO"
#define XBOX_LOG_OB      "OB"
#define XBOX_LOG_CRYPTO  "CRYPTO"
#define XBOX_LOG_XBOX    "XBOX"
#define XBOX_LOG_THUNK   "THUNK"
#define XBOX_LOG_PATH    "PATH"

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_H */
