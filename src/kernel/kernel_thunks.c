/*
 * kernel_thunks.c - Xbox Kernel Thunk Table & Initialization
 *
 * Wires the 147-entry kernel thunk table at VA 0x0036B7C0 to our
 * xbox_* function/data implementations.
 *
 * The Xbox kernel thunk table is an array of function/data pointers that
 * game code calls through via indirect calls: call [thunk_addr].
 * Each entry corresponds to a kernel export ordinal.
 *
 * This file provides:
 *   - xbox_kernel_thunk_table[] - the 147 pointer slots
 *   - xbox_resolve_ordinal() - maps ordinal → function/data pointer
 *   - xbox_kernel_init() - fills the thunk table and initializes subsystems
 *   - xbox_kernel_shutdown() - cleanup
 *   - xbox_log() - logging implementation
 */

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

/* ============================================================================
 * Thunk Table Storage
 * ============================================================================ */

ULONG_PTR xbox_kernel_thunk_table[XBOX_KERNEL_THUNK_TABLE_SIZE] = {0};

/* ============================================================================
 * Logging Implementation
 * ============================================================================ */

static FILE* g_log_file = NULL;
static int   g_log_level = XBOX_LOG_INFO;
static CRITICAL_SECTION g_log_cs;
static BOOL  g_log_cs_init = FALSE;

static const char* xbox_log_level_str(int level)
{
    switch (level) {
        case XBOX_LOG_ERROR: return "ERROR";
        case XBOX_LOG_WARN:  return "WARN ";
        case XBOX_LOG_INFO:  return "INFO ";
        case XBOX_LOG_DEBUG: return "DEBUG";
        case XBOX_LOG_TRACE: return "TRACE";
        default:             return "?????";
    }
}

void xbox_log(int level, const char* subsystem, const char* fmt, ...)
{
    va_list args;
    char timestamp[32];
    SYSTEMTIME st;

    if (level > g_log_level)
        return;

    GetLocalTime(&st);
    snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    if (g_log_cs_init)
        EnterCriticalSection(&g_log_cs);

    FILE* out = g_log_file ? g_log_file : stderr;
    fprintf(out, "[%s] %s [%-6s] ", timestamp, xbox_log_level_str(level), subsystem);

    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);

    if (g_log_cs_init)
        LeaveCriticalSection(&g_log_cs);
}

/* ============================================================================
 * Ordinal Resolution
 *
 * Maps each Xbox kernel ordinal to our implementation address.
 * Returns 0 for unimplemented ordinals (logged as warnings).
 *
 * Ordinals are from the Xbox kernel export table. Each one maps to
 * either a function pointer or a data pointer.
 * ============================================================================ */

ULONG_PTR xbox_resolve_ordinal(ULONG ordinal)
{
    switch (ordinal) {

    /* ---- Display / AV ---- */
    case   1: return (ULONG_PTR)xbox_AvGetSavedDataAddress;
    case   2: return (ULONG_PTR)xbox_AvSendTVEncoderOption;
    case   3: return (ULONG_PTR)xbox_AvSetDisplayMode;
    case   4: return (ULONG_PTR)xbox_AvSetSavedDataAddress;

    /* ---- Unknown stubs ---- */
    case   8: return (ULONG_PTR)xbox_Unknown_8;
    case  23: return (ULONG_PTR)xbox_Unknown_23;
    case  42: return (ULONG_PTR)xbox_Unknown_42;

    /* ---- Pool Allocator ---- */
    case  15: return (ULONG_PTR)xbox_ExAllocatePool;
    case  16: return (ULONG_PTR)xbox_ExAllocatePoolWithTag;
    case  17: return (ULONG_PTR)&xbox_ExEventObjectType;         /* data */
    case  24: return (ULONG_PTR)xbox_ExQueryPoolBlockSize;

    /* ---- HAL ---- */
    case  40: return (ULONG_PTR)xbox_HalClearSoftwareInterrupt;
    case  41: return (ULONG_PTR)xbox_HalDisableSystemInterrupt;
    case  44: return (ULONG_PTR)xbox_HalGetInterruptVector;
    case  46: return (ULONG_PTR)xbox_HalReadSMCTrayState;
    case  47: return (ULONG_PTR)xbox_HalReadWritePCISpace;
    case  49: return (ULONG_PTR)xbox_HalRequestSoftwareInterrupt;
    case 358: return (ULONG_PTR)xbox_HalIsResetOrShutdownPending;
    case 360: return (ULONG_PTR)xbox_HalInitiateShutdown;

    /* ---- I/O Manager ---- */
    case  62: return (ULONG_PTR)xbox_IoBuildDeviceIoControlRequest;
    case  65: return (ULONG_PTR)&xbox_IoCompletionObjectType;    /* data */
    case  67: return (ULONG_PTR)xbox_IoCreateFile;
    case  69: return (ULONG_PTR)xbox_IoDeleteDevice;
    case  71: return (ULONG_PTR)&xbox_IoDeviceObjectType;        /* data */
    case  74: return (ULONG_PTR)xbox_IoInitializeIrp;
    case  81: return (ULONG_PTR)xbox_IoSetIoCompletion;
    case  83: return (ULONG_PTR)xbox_IoStartNextPacket;
    case  84: return (ULONG_PTR)xbox_IoStartNextPacketByKey;
    case  85: return (ULONG_PTR)xbox_IoStartPacket;
    case  86: return (ULONG_PTR)xbox_IoSynchronousDeviceIoControlRequest;
    case  87: return (ULONG_PTR)xbox_IoSynchronousFsdRequest;
    case 359: return (ULONG_PTR)xbox_IoMarkIrpMustComplete;

    /* ---- Kernel Synchronization ---- */
    case  95: return (ULONG_PTR)xbox_KeAlertThread;
    case  97: return (ULONG_PTR)xbox_KeBugCheck;
    case  98: return (ULONG_PTR)xbox_KeBugCheckEx;
    case  99: return (ULONG_PTR)xbox_KeCancelTimer;
    case 100: return (ULONG_PTR)xbox_KeConnectInterrupt;
    case 107: return (ULONG_PTR)xbox_KeInitializeDpc;
    case 109: return (ULONG_PTR)xbox_KeInitializeInterrupt;
    case 113: return (ULONG_PTR)xbox_KeInitializeTimerEx;
    case 119: return (ULONG_PTR)xbox_KeInsertQueueDpc;
    case 124: return (ULONG_PTR)xbox_KeQueryBasePriorityThread;
    case 126: return (ULONG_PTR)xbox_KeQueryPerformanceCounter;
    case 127: return (ULONG_PTR)xbox_KeQueryPerformanceFrequency;
    case 128: return (ULONG_PTR)xbox_KeQuerySystemTime;
    case 129: return (ULONG_PTR)xbox_KeRaiseIrqlToDpcLevel;
    case 137: return (ULONG_PTR)xbox_KeRemoveQueueDpc;
    case 139: return (ULONG_PTR)xbox_KeRestoreFloatingPointState;
    case 142: return (ULONG_PTR)xbox_KeSaveFloatingPointState;
    case 143: return (ULONG_PTR)xbox_KeSetBasePriorityThread;
    case 145: return (ULONG_PTR)xbox_KeSetEvent;
    case 149: return (ULONG_PTR)xbox_KeSetTimer;
    case 150: return (ULONG_PTR)xbox_KeSetTimerEx;
    case 151: return (ULONG_PTR)xbox_KeStallExecutionProcessor;
    case 153: return (ULONG_PTR)xbox_KeSynchronizeExecution;
    case 156: return (ULONG_PTR)&xbox_KeTickCount;               /* data */
    case 158: return (ULONG_PTR)xbox_KeWaitForMultipleObjects;
    case 159: return (ULONG_PTR)xbox_KeWaitForSingleObject;
    case 160: return (ULONG_PTR)xbox_KfRaiseIrql;
    case 161: return (ULONG_PTR)xbox_KfLowerIrql;

    /* ---- Launch Data ---- */
    case 164: return (ULONG_PTR)&xbox_LaunchDataPage;            /* data: pointer to page */

    /* ---- Memory Management ---- */
    case 165: return (ULONG_PTR)xbox_MmAllocateContiguousMemory;
    case 166: return (ULONG_PTR)xbox_MmAllocateContiguousMemoryEx;
    case 168: return (ULONG_PTR)xbox_MmClaimGpuInstanceMemory;
    case 169: return (ULONG_PTR)xbox_MmCreateKernelStack;
    case 170: return (ULONG_PTR)xbox_MmDeleteKernelStack;
    case 171: return (ULONG_PTR)xbox_MmFreeContiguousMemory;
    case 173: return (ULONG_PTR)xbox_MmGetPhysicalAddress;
    case 175: return (ULONG_PTR)xbox_MmLockUnlockBufferPages;
    case 176: return (ULONG_PTR)xbox_MmLockUnlockPhysicalPage;
    case 178: return (ULONG_PTR)xbox_MmPersistContiguousMemory;
    case 179: return (ULONG_PTR)xbox_MmQueryAddressProtect;
    case 180: return (ULONG_PTR)xbox_MmQueryAllocationSize;
    case 181: return (ULONG_PTR)xbox_MmQueryStatistics;
    case 182: return (ULONG_PTR)xbox_MmSetAddressProtect;

    /* ---- NT Virtual Memory ---- */
    case 184: return (ULONG_PTR)xbox_NtAllocateVirtualMemory;

    /* ---- NT File I/O ---- */
    case 187: return (ULONG_PTR)xbox_NtClose;
    case 189: return (ULONG_PTR)xbox_NtCreateEvent;
    case 190: return (ULONG_PTR)xbox_NtCreateFile;
    case 193: return (ULONG_PTR)xbox_NtCreateSemaphore;
    case 195: return (ULONG_PTR)xbox_NtDeleteFile;
    case 196: return (ULONG_PTR)xbox_NtDeviceIoControlFile;
    case 197: return (ULONG_PTR)xbox_NtDuplicateObject;
    case 198: return (ULONG_PTR)xbox_NtFlushBuffersFile;
    case 199: return (ULONG_PTR)xbox_NtFreeVirtualMemory;
    case 200: return (ULONG_PTR)xbox_NtFsControlFile;
    case 202: return (ULONG_PTR)xbox_NtOpenFile;
    case 203: return (ULONG_PTR)xbox_NtOpenSymbolicLinkObject;
    case 207: return (ULONG_PTR)xbox_NtQueryDirectoryFile;
    case 210: return (ULONG_PTR)xbox_NtQueryFullAttributesFile;
    case 211: return (ULONG_PTR)xbox_NtQueryInformationFile;
    case 215: return (ULONG_PTR)xbox_NtQuerySymbolicLinkObject;
    case 217: return (ULONG_PTR)xbox_NtQueryVirtualMemory;
    case 218: return (ULONG_PTR)xbox_NtQueryVolumeInformationFile;
    case 219: return (ULONG_PTR)xbox_NtReadFile;
    case 222: return (ULONG_PTR)xbox_NtReleaseSemaphore;
    case 225: return (ULONG_PTR)xbox_NtSetEvent;
    case 226: return (ULONG_PTR)xbox_NtSetInformationFile;
    case 228: return (ULONG_PTR)xbox_NtSetSystemTime;
    case 233: return (ULONG_PTR)xbox_NtWaitForMultipleObjectsEx;
    case 234: return (ULONG_PTR)xbox_NtWaitForSingleObject;
    case 236: return (ULONG_PTR)xbox_NtWriteFile;
    case 238: return (ULONG_PTR)xbox_NtYieldExecution;

    /* ---- Object Manager ---- */
    case 246: return (ULONG_PTR)xbox_ObReferenceObjectByHandle;
    case 247: return (ULONG_PTR)xbox_ObReferenceObjectByName;
    case 250: return (ULONG_PTR)xbox_ObfDereferenceObject;

    /* ---- Network / PHY ---- */
    case 252: return (ULONG_PTR)xbox_PhyGetLinkState;
    case 253: return (ULONG_PTR)xbox_PhyInitialize;

    /* ---- Threading ---- */
    case 255: return (ULONG_PTR)xbox_PsCreateSystemThreadEx;
    case 258: return (ULONG_PTR)xbox_PsTerminateSystemThread;
    case 259: return (ULONG_PTR)&xbox_PsThreadObjectType;       /* data */

    /* ---- Runtime Library ---- */
    case 260: return (ULONG_PTR)xbox_RtlAnsiStringToUnicodeString;
    case 269: return (ULONG_PTR)xbox_RtlCompareMemoryUlong;
    case 277: return (ULONG_PTR)xbox_RtlEnterCriticalSection;
    case 279: return (ULONG_PTR)xbox_RtlEqualString;
    case 289: return (ULONG_PTR)xbox_RtlInitAnsiString;
    case 291: return (ULONG_PTR)xbox_RtlInitializeCriticalSection;
    case 294: return (ULONG_PTR)xbox_RtlLeaveCriticalSection;
    case 301: return (ULONG_PTR)xbox_RtlNtStatusToDosError;
    case 302: return (ULONG_PTR)xbox_RtlRaiseException;
    case 304: return (ULONG_PTR)xbox_RtlTimeFieldsToTime;
    case 305: return (ULONG_PTR)xbox_RtlTimeToTimeFields;
    case 308: return (ULONG_PTR)xbox_RtlUnicodeStringToAnsiString;
    case 312: return (ULONG_PTR)xbox_RtlUnwind;
    case 354: return (ULONG_PTR)xbox_RtlRip;

    /* ---- Xbox Identity (data exports) ---- */
    case 322: return (ULONG_PTR)&xbox_HardwareInfo;              /* data */
    case 323: return (ULONG_PTR)xbox_HDKey;                      /* data (array) */
    case 324: return (ULONG_PTR)&xbox_KrnlVersion;               /* data */
    case 325: return (ULONG_PTR)xbox_SignatureKey;                /* data (array) */
    case 326: return (ULONG_PTR)xbox_LANKey;                     /* data (array) */
    case 327: return (ULONG_PTR)xbox_AlternateSignatureKeys;     /* data (array) */
    case 328: return (ULONG_PTR)&xbox_XeImageFileName;           /* data */
    case 355: return (ULONG_PTR)xbox_LANKey;                     /* data (alias) */
    case 356: return (ULONG_PTR)xbox_AlternateSignatureKeys;     /* data (alias) */
    case 357: return (ULONG_PTR)xbox_XePublicKeyData;            /* data (array) */

    /* ---- Port I/O ---- */
    case 335: return (ULONG_PTR)xbox_WRITE_PORT_BUFFER_USHORT;
    case 336: return (ULONG_PTR)xbox_WRITE_PORT_BUFFER_ULONG;

    /* ---- Crypto ---- */
    case 337: return (ULONG_PTR)xbox_XcSHAInit;
    case 338: return (ULONG_PTR)xbox_XcSHAUpdate;
    case 339: return (ULONG_PTR)xbox_XcSHAFinal;
    case 340: return (ULONG_PTR)xbox_XcRC4Key;
    case 344: return (ULONG_PTR)xbox_XcPKDecPrivate;
    case 345: return (ULONG_PTR)xbox_XcPKGetKeyLen;
    case 346: return (ULONG_PTR)xbox_XcVerifyPKCS1Signature;
    case 347: return (ULONG_PTR)xbox_XcModExp;
    case 349: return (ULONG_PTR)xbox_XcKeyTable;
    case 353: return (ULONG_PTR)xbox_XcUpdateCrypto;

    /* ---- Threading (continued) ---- */
    case 256: return (ULONG_PTR)xbox_KeDelayExecutionThread;

    default:
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THUNK,
            "Unresolved kernel ordinal %u", ordinal);
        return 0;
    }
}

/* ============================================================================
 * Ordinal List
 *
 * The 147 ordinals imported by Burnout 3, in thunk table order.
 * Extracted from the XBE kernel thunk table at VA 0x0036B7C0.
 * ============================================================================ */

static const ULONG g_thunk_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE] = {
      1,   2,   3,   4,   8,  15,  16,  17,  23,  24,   /* 0-9   */
     40,  41,  42,  44,  46,  47,  49,  62,  65,  67,   /* 10-19 */
     69,  71,  74,  81,  83,  84,  85,  86,  87,  95,   /* 20-29 */
     97,  98,  99, 100, 107, 109, 113, 119, 124, 126,   /* 30-39 */
    127, 128, 129, 137, 139, 142, 143, 145, 149, 150,   /* 40-49 */
    151, 153, 156, 158, 159, 160, 161, 164, 165, 166,   /* 50-59 */
    168, 169, 170, 171, 173, 175, 176, 178, 179, 180,   /* 60-69 */
    181, 182, 184, 187, 189, 190, 193, 195, 196, 197,   /* 70-79 */
    198, 199, 200, 202, 203, 207, 210, 211, 215, 217,   /* 80-89 */
    218, 219, 222, 225, 226, 228, 233, 234, 236, 238,   /* 90-99 */
    246, 247, 250, 252, 253, 255, 256, 258, 259, 260,   /* 100-109 */
    269, 277, 279, 289, 291, 294, 301, 302, 304, 305,   /* 110-119 */
    308, 312, 322, 323, 324, 325, 326, 327, 328, 335,   /* 120-129 */
    336, 337, 338, 339, 340, 344, 345, 346, 347, 349,   /* 130-139 */
    353, 354, 355, 356, 357, 358, 359,                   /* 140-146 */
};

/* ============================================================================
 * Unresolved Thunk Handler
 *
 * Called if game code tries to use a thunk slot that wasn't resolved.
 * Logs the error and triggers a debug break.
 * ============================================================================ */

static void __stdcall xbox_unresolved_thunk(void)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THUNK,
        "Call to unresolved kernel thunk! Return address is on the stack.");
#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * xbox_kernel_init - Initialize the kernel replacement layer
 *
 * Must be called before any game code runs. Sets up:
 *   1. Logging system
 *   2. Path translation
 *   3. Thunk table (all 147 entries)
 * ============================================================================ */

void xbox_kernel_init(void)
{
    ULONG resolved = 0;
    ULONG unresolved = 0;

    /* Initialize logging */
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_init = TRUE;

    /* Try to open log file, fall back to stderr */
    g_log_file = fopen("xbox_kernel.log", "w");

    /* Set log level from environment variable if present */
    const char* log_env = getenv("XBOX_LOG_LEVEL");
    if (log_env) {
        g_log_level = atoi(log_env);
        if (g_log_level < XBOX_LOG_ERROR) g_log_level = XBOX_LOG_ERROR;
        if (g_log_level > XBOX_LOG_TRACE) g_log_level = XBOX_LOG_TRACE;
    }

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "=== Xbox Kernel Replacement Layer initializing ===");
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "Kernel version: %u.%u.%u.%u (emulated)",
        xbox_KrnlVersion.Major, xbox_KrnlVersion.Minor,
        xbox_KrnlVersion.Build, xbox_KrnlVersion.Qfe);

    /* Fill thunk table */
    for (ULONG i = 0; i < XBOX_KERNEL_THUNK_TABLE_SIZE; i++) {
        ULONG ordinal = g_thunk_ordinals[i];
        ULONG_PTR ptr = xbox_resolve_ordinal(ordinal);

        if (ptr) {
            xbox_kernel_thunk_table[i] = ptr;
            resolved++;
        } else {
            /* Point unresolved thunks to our error handler */
            xbox_kernel_thunk_table[i] = (ULONG_PTR)xbox_unresolved_thunk;
            unresolved++;
        }
    }

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "Thunk table: %u/%u resolved, %u unresolved",
        resolved, XBOX_KERNEL_THUNK_TABLE_SIZE, unresolved);

    if (unresolved > 0) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_THUNK,
            "WARNING: %u kernel imports are unresolved - game may crash!", unresolved);
    }

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "=== Xbox Kernel Replacement Layer ready ===");
}

/* ============================================================================
 * xbox_kernel_shutdown - Clean up the kernel replacement layer
 * ============================================================================ */

void xbox_kernel_shutdown(void)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "=== Xbox Kernel Replacement Layer shutting down ===");

    /* Close log file */
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    if (g_log_cs_init) {
        DeleteCriticalSection(&g_log_cs);
        g_log_cs_init = FALSE;
    }

    /* Zero thunk table */
    memset(xbox_kernel_thunk_table, 0, sizeof(xbox_kernel_thunk_table));
}
