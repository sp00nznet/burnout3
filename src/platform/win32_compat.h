/*
 * win32_compat.h - POSIX implementation of generic Win32 host primitives
 *
 * The Xbox kernel HLE (src/kernel) is built on top of a handful of *generic*
 * OS primitives -- threads, events, mutexes, semaphores, interlocked atomics,
 * a heap, timers, time queries. These are NOT Xbox semantics; they are host
 * services that any emulator needs. This layer implements that exact subset
 * honestly on POSIX so the Xbox-specific HLE code above it is unchanged and
 * stays verifiable.
 *
 * Linux-only: on Windows these come from the real <windows.h>.
 */

#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

#if !defined(_WIN32)

#include "xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants --------------------------------------------------------- */
#define WAIT_OBJECT_0        0x00000000u
#define WAIT_ABANDONED_0     0x00000080u
#define WAIT_ABANDONED       0x00000080u
#define WAIT_IO_COMPLETION   0x000000C0u
#define WAIT_TIMEOUT         0x00000102u
#define WAIT_FAILED          0xFFFFFFFFu
#ifndef INFINITE
#define INFINITE             0xFFFFFFFFu
#endif

#define CREATE_SUSPENDED         0x00000004u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u

#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_LOWEST        (-2)
#define THREAD_PRIORITY_BELOW_NORMAL  (-1)
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_HIGHEST       2
#define THREAD_PRIORITY_TIME_CRITICAL 15

#define DUPLICATE_CLOSE_SOURCE   0x00000001u
#define DUPLICATE_SAME_ACCESS    0x00000002u

#define WT_EXECUTEONLYONCE       0x00000008u
#define WT_EXECUTEDEFAULT        0x00000000u

/* VirtualAlloc / VirtualProtect */
#define MEM_COMMIT     0x00001000u
#define MEM_RESERVE    0x00002000u
#define MEM_DECOMMIT   0x00004000u
#define MEM_RELEASE    0x00008000u
#define MEM_TOP_DOWN   0x00100000u
#define PAGE_NOACCESS          0x01u
#define PAGE_READONLY          0x02u
#define PAGE_READWRITE         0x04u
#define PAGE_WRITECOPY         0x08u
#define PAGE_EXECUTE           0x10u
#define PAGE_EXECUTE_READ      0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

/* Code-page ids accepted by MultiByteToWideChar (only UTF8/ACP are honoured) */
#define CP_ACP    0u
#define CP_UTF8   65001u

/* ---- Callback typedefs ------------------------------------------------- */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID lpParameter);
typedef VOID  (WINAPI *PAPCFUNC)(ULONG_PTR Parameter);
typedef VOID  (WINAPI *WAITORTIMERCALLBACK)(PVOID lpParameter, BOOLEAN TimerOrWaitFired);
typedef void  *LPSECURITY_ATTRIBUTES;
typedef void  *PTP_CALLBACK_INSTANCE;
typedef VOID  (WINAPI *PTP_SIMPLE_CALLBACK)(PTP_CALLBACK_INSTANCE Instance, PVOID Context);

/* ---- Last-error -------------------------------------------------------- */
DWORD GetLastError(void);
VOID  SetLastError(DWORD dwErrCode);

/* ---- Interlocked atomics ---------------------------------------------- */
LONG InterlockedIncrement(volatile LONG *Addend);
LONG InterlockedDecrement(volatile LONG *Addend);
LONG InterlockedExchange(volatile LONG *Target, LONG Value);
LONG InterlockedExchangeAdd(volatile LONG *Addend, LONG Value);
LONG InterlockedCompareExchange(volatile LONG *Dest, LONG Exchange, LONG Comparand);
PVOID InterlockedCompareExchangePointer(PVOID volatile *Dest, PVOID Exchange, PVOID Comparand);

/* ---- Critical sections ------------------------------------------------- */
VOID InitializeCriticalSection(LPCRITICAL_SECTION cs);
VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
VOID EnterCriticalSection(LPCRITICAL_SECTION cs);
VOID LeaveCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
VOID DeleteCriticalSection(LPCRITICAL_SECTION cs);

/* ---- Condition variables (paired with a CRITICAL_SECTION) ----------- */
VOID InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
VOID WakeConditionVariable(PCONDITION_VARIABLE cv);
VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv);

/* ---- Generic handle lifetime ------------------------------------------ */
BOOL  CloseHandle(HANDLE h);
BOOL  DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc,
                      PHANDLE dst, DWORD access, BOOL inherit, DWORD options);

/* fd-backed handle, used by the file-I/O HLE (kernel_file.c). Lets file
 * handles flow through the same CloseHandle path as every other object. */
HANDLE      w32_open_handle(int fd, const char *host_path);
int         w32_handle_fd(HANDLE h);
const char *w32_handle_path(HANDLE h);

/* ---- Events ----------------------------------------------------------- */
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
BOOL   PulseEvent(HANDLE h);

/* ---- Semaphores ------------------------------------------------------- */
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name);
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name);
BOOL   ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount);

/* ---- Mutexes ---------------------------------------------------------- */
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name);
BOOL   ReleaseMutex(HANDLE h);

/* ---- Waiting ---------------------------------------------------------- */
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms);
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable);

/* ---- Threads ---------------------------------------------------------- */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId);
VOID   ExitThread(DWORD exitCode);
BOOL   GetExitCodeThread(HANDLE h, LPDWORD exitCode);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);
BOOL   TerminateThread(HANDLE h, DWORD exitCode);
DWORD  GetCurrentThreadId(void);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD  GetCurrentProcessId(void);
BOOL   SetThreadPriority(HANDLE h, int priority);
int    GetThreadPriority(HANDLE h);
VOID   SwitchToThread(void);
DWORD  QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data);

/* ---- Sleep ------------------------------------------------------------ */
VOID  Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);

/* ---- Timer queues ----------------------------------------------------- */
HANDLE CreateTimerQueue(void);
BOOL   DeleteTimerQueue(HANDLE timerQueue);
BOOL   CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                             WAITORTIMERCALLBACK callback, PVOID param,
                             DWORD dueTime, DWORD period, ULONG flags);
BOOL   DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent);
BOOL   ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period);
BOOL   TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                   PVOID context, PVOID env);

/* ---- Heap ------------------------------------------------------------- */
HANDLE GetProcessHeap(void);
HANDLE HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum);
BOOL   HeapDestroy(HANDLE heap);
LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes);
BOOL   HeapFree(HANDLE heap, DWORD flags, LPVOID mem);
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem);
#define HEAP_ZERO_MEMORY 0x00000008u

/* ---- Virtual memory --------------------------------------------------- */
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect);
BOOL   VirtualFree(LPVOID address, SIZE_T size, DWORD freeType);
BOOL   VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect);

/* ---- Time ------------------------------------------------------------- */
VOID  GetSystemTimeAsFileTime(LPFILETIME ft);
VOID  GetSystemTime(LPSYSTEMTIME st);
VOID  GetLocalTime(LPSYSTEMTIME st);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
BOOL  QueryPerformanceCounter(PLARGE_INTEGER count);
BOOL  QueryPerformanceFrequency(PLARGE_INTEGER freq);

/* ---- Misc ------------------------------------------------------------- */
VOID  OutputDebugStringA(LPCSTR str);
VOID  OutputDebugStringW(LPCWSTR str);
VOID  ExitProcess(UINT exitCode);
BOOL  TerminateProcess(HANDLE process, UINT exitCode);
VOID  SecureZeroMemory(PVOID ptr, SIZE_T cnt);
unsigned int _clearfp(void);   /* clear pending FPU exception flags */
int   MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                          LPWSTR wide, int wideCount);
int   WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                          LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef);

/* ---- Win32 error codes (subset used by RtlNtStatusToDosError) --------- */
#define ERROR_SUCCESS                 0u
#define ERROR_INVALID_FUNCTION        1u
#define ERROR_FILE_NOT_FOUND          2u
#define ERROR_PATH_NOT_FOUND          3u
#define ERROR_ACCESS_DENIED           5u
#define ERROR_INVALID_HANDLE          6u
#define ERROR_NOT_ENOUGH_MEMORY       8u
#define ERROR_NO_MORE_FILES           18u
#define ERROR_GEN_FAILURE             31u
#define ERROR_HANDLE_EOF              38u
#define ERROR_NOT_SUPPORTED           50u
#define ERROR_FILE_EXISTS             80u
#define ERROR_INVALID_PARAMETER       87u
#define ERROR_CALL_NOT_IMPLEMENTED    120u
#define ERROR_INSUFFICIENT_BUFFER     122u
#define ERROR_ALREADY_EXISTS          183u
#define ERROR_MORE_DATA               234u
#define ERROR_MR_MID_NOT_FOUND        317u
#define ERROR_IO_PENDING              997u
#define ERROR_CANCELLED               1223u
#define ERROR_NO_SYSTEM_RESOURCES     1450u
#define ERROR_COMMITMENT_LIMIT        1455u
#define ERROR_DEVICE_NOT_CONNECTED    1167u

/* ---- COM HRESULT codes ------------------------------------------------ */
#ifndef S_OK
#define S_OK            ((HRESULT)0x00000000L)
#define S_FALSE         ((HRESULT)0x00000001L)
#define E_NOTIMPL       ((HRESULT)0x80004001L)
#define E_NOINTERFACE   ((HRESULT)0x80004002L)
#define E_POINTER       ((HRESULT)0x80004003L)
#define E_ABORT         ((HRESULT)0x80004004L)
#define E_FAIL          ((HRESULT)0x80004005L)
#define E_ACCESSDENIED  ((HRESULT)0x80070005L)
#define E_HANDLE        ((HRESULT)0x80070006L)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000EL)
#define E_INVALIDARG    ((HRESULT)0x80070057L)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
#endif

/* ---- Exception records (SEH compile-shim; not yet emulated) ----------- */
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD     ExceptionCode;
    DWORD     ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID     ExceptionAddress;
    DWORD     NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);

/* ---- Memory query ----------------------------------------------------- */
typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    DWORD  AllocationProtect;
    SIZE_T RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length);

typedef struct _MEMORYSTATUSEX {
    DWORD     dwLength;
    DWORD     dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX buffer);

/* ---- File mapping (memfd-backed; gives true aliased mirror views) ----- */
#define FILE_MAP_COPY        0x00000001u
#define FILE_MAP_WRITE       0x00000002u
#define FILE_MAP_READ        0x00000004u
#define FILE_MAP_ALL_ACCESS  0x000F001Fu
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name);
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name);
LPVOID MapViewOfFile(HANDLE mapping, DWORD access,
                     DWORD offHigh, DWORD offLow, SIZE_T count);
LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access,
                       DWORD offHigh, DWORD offLow, SIZE_T count, LPVOID baseAddr);
BOOL   UnmapViewOfFile(LPCVOID baseAddr);
#define CreateFileMapping CreateFileMappingA

/* ---- Aligned allocation ---------------------------------------------- */
void *_aligned_malloc(SIZE_T size, SIZE_T alignment);
void  _aligned_free(void *ptr);

/* ---- Case-insensitive string compare --------------------------------- */
int _stricmp(const char *a, const char *b);
int _strnicmp(const char *a, const char *b, SIZE_T n);

/* ---- Wide-string helpers (operate on the 16-bit Xbox WCHAR) ----------
 * Named xbox_wcs* (not macros over wcs*) so they never collide with the
 * 32-bit-wchar_t CRT functions in <wchar.h>. On Windows xbox_winnt.h maps
 * these names to the real CRT functions. */
SIZE_T xbox_wcslen(const WCHAR *s);
int    xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n);
WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src);
WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src);

/* ---- Time conversion ------------------------------------------------- */
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft);
BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st);

/* ---- Multimedia (winmm) shim: waveOut stubs that always report failure.
 * The APU has a waveOut fallback path; on Linux it just stays inactive
 * and the (real) SDL2 audio path will replace it later. */
typedef DWORD  MMRESULT;
typedef void  *HWAVEOUT;
#define MMSYSERR_NOERROR     0
#define MMSYSERR_INVALPARAM 11
#define WAVE_FORMAT_PCM      1
#define WAVE_MAPPER          ((UINT)-1)
#define CALLBACK_NULL        0x00000000
#define WHDR_DONE            0x00000001
#define WHDR_INQUEUE         0x00000010

typedef struct tWAVEFORMATEX {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;

typedef struct tWAVEHDR {
    LPSTR     lpData;
    DWORD     dwBufferLength;
    DWORD     dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD     dwFlags;
    DWORD     dwLoops;
    struct tWAVEHDR *lpNext;
    DWORD_PTR reserved;
} WAVEHDR;

static inline MMRESULT waveOutOpen(HWAVEOUT *h, UINT id, const WAVEFORMATEX *f,
                                   DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)h;(void)id;(void)f;(void)cb;(void)inst;(void)flags; return MMSYSERR_INVALPARAM; }
static inline MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{ (void)h;(void)hdr;(void)sz; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{ (void)h;(void)hdr;(void)sz; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{ (void)h;(void)hdr;(void)sz; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutReset(HWAVEOUT h) { (void)h; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutClose(HWAVEOUT h) { (void)h; return MMSYSERR_NOERROR; }

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* WIN32_COMPAT_H */
