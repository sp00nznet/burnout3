/*
 * xbox_winnt.h - Portable Win32 / NT type vocabulary
 *
 * The Xbox kernel API is a derivative of the Windows NT kernel API, so its
 * public signatures legitimately speak in NT types (NTSTATUS, HANDLE, ULONG,
 * LARGE_INTEGER, ...). On Windows this header is just <windows.h>. On
 * Linux/Debian there is no Win32 SDK, so we define the exact same type
 * vocabulary on top of <stdint.h>.
 *
 * IMPORTANT: this header declares TYPES ONLY. It contains no Win32 functions.
 * The Xbox kernel HLE (.c files under src/kernel) implements behaviour
 * directly against POSIX -- that is the whole point of building Linux-first:
 * there is no Win32 to silently fall through to.
 *
 * Notes on the Linux definitions:
 *   - Calling-convention macros (__stdcall/__cdecl/__fastcall/WINAPI) expand
 *     to nothing: x86-64 has a single calling convention.
 *   - WCHAR is uint16_t (UTF-16), matching the Xbox -- NOT the 32-bit Linux
 *     wchar_t. Do not feed L"..." literals to WCHAR* APIs in ported code.
 */

#ifndef XBOX_WINNT_H
#define XBOX_WINNT_H

/* Thread-local storage qualifier (portable). */
#if defined(_WIN32)
#define XBOX_THREAD_LOCAL __declspec(thread)
#else
#define XBOX_THREAD_LOCAL __thread
#endif

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER)
#define __debugbreak() __builtin_trap()
#endif

#if defined(_WIN32)

/* ---- Windows host: use the real SDK ------------------------------------- */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/* Wide-string helpers: map to the real CRT functions on Windows. */
#define xbox_wcslen  wcslen
#define xbox_wcsncmp wcsncmp
#define xbox_wcscat  wcscat
#define xbox_wcscpy  wcscpy

#else /* !_WIN32 */

/* ---- Linux / POSIX host: define the NT type vocabulary ------------------ */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>   /* windows.h users relied on string.h being visible */

#ifdef __cplusplus
extern "C" {
#endif

/* Calling conventions: x86-64 SysV has a single convention. GCC accepts and
 * ignores these attributes on 64-bit, but expanding to nothing is cleaner and
 * avoids -Wattributes noise. */
#define __stdcall
#define __cdecl
#define __fastcall
#define __thiscall
#ifndef WINAPI
#define WINAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef CONST
#define CONST const
#endif

/* Parameter annotation macros (no-ops, used in NT-style prototypes). */
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif

/* ---- Fundamental integer types ----------------------------------------- */
typedef int32_t            LONG,  *PLONG,  *LPLONG;
typedef uint32_t           ULONG, *PULONG;
typedef uint32_t           DWORD, *PDWORD, *LPDWORD;
typedef int16_t            SHORT, *PSHORT;
typedef uint16_t           USHORT, *PUSHORT;
typedef uint16_t           WORD,  *PWORD;
typedef uint8_t            UCHAR, *PUCHAR;
typedef uint8_t            BYTE,  *PBYTE, *LPBYTE;
typedef uint8_t            BOOLEAN, *PBOOLEAN;
typedef char               CHAR,  *PCHAR;
typedef char               CCHAR;
typedef int                INT,   *PINT;
typedef unsigned int       UINT,  *PUINT;
typedef int                BOOL,  *PBOOL;
typedef int64_t            LONGLONG,  LONG64,  *PLONGLONG;
typedef uint64_t           ULONGLONG, ULONG64, *PULONGLONG, DWORDLONG;
typedef float              FLOAT;
typedef double             DOUBLE;

/* void aliases */
typedef void               VOID;
typedef void              *PVOID, *LPVOID;
typedef const void        *PCVOID, *LPCVOID;

/* Handles -- opaque pointers */
typedef void              *HANDLE;
typedef void             **PHANDLE, **LPHANDLE;
typedef void              *HMODULE, *HINSTANCE, *HKEY, *HLOCAL, *HGLOBAL;
typedef void              *HWND, *HDC, *HBITMAP, *HICON, *HMENU, *HCURSOR;

/* PCONTEXT: Windows VEH register state. On Linux the equivalent is
 * ucontext_t*; we leave it opaque here since Linux callers don't use it. */
typedef void              *PCONTEXT;

/* Xbox/UTF-16 wide char is 16-bit, unlike the 32-bit Linux wchar_t. */
typedef uint16_t           WCHAR, *PWCHAR, *LPWSTR, *PWSTR;
typedef const uint16_t    *LPCWSTR, *PCWSTR;

/* Narrow string pointers */
typedef char              *LPSTR,  *PSTR;
typedef const char        *LPCSTR, *PCSTR;

/* Pointer-sized integers */
typedef intptr_t           INT_PTR,  LONG_PTR,  *PLONG_PTR;
typedef uintptr_t          UINT_PTR, ULONG_PTR, *PULONG_PTR;
typedef intptr_t           SSIZE_T;
typedef uintptr_t          SIZE_T,  *PSIZE_T;
typedef intptr_t           DWORD_PTR;
typedef uintptr_t          HALF_PTR;

/* NT-flavoured aliases */
typedef ULONG              ACCESS_MASK, *PACCESS_MASK;
typedef DWORD              COLORREF;

/* Win32 message types (LRESULT/WPARAM/LPARAM are pointer-sized) */
typedef LONG_PTR           LRESULT;
typedef UINT_PTR           WPARAM;
typedef LONG_PTR           LPARAM;
typedef LONG               HRESULT;
typedef LONG               NTSTATUS;

/* ---- LARGE_INTEGER / ULARGE_INTEGER ------------------------------------ */
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG  HighPart; };
    struct { DWORD LowPart; LONG  HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

/* ---- GUID -------------------------------------------------------------- */
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID, *LPGUID, *PGUID;
typedef GUID IID, CLSID;
typedef const GUID *REFGUID, *REFIID, *REFCLSID;

/* ---- Critical section --------------------------------------------------- */
/* Opaque, deliberately sized generously: ported code embeds this in its own
 * structs. Backed by a pthread_mutex_t inside the kernel HLE. */
typedef struct _RTL_CRITICAL_SECTION {
    void     *DebugInfo;
    LONG      LockCount;
    LONG      RecursionCount;
    void     *OwningThread;
    void     *LockSemaphore;
    ULONG_PTR SpinCount;
    uint8_t   _backing[40];   /* room for a pthread_mutex_t */
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION,
  CRITICAL_SECTION,     *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

/* CONDITION_VARIABLE: same single-pointer layout as Win32 (Ptr is a
 * pthread_cond_t* on POSIX). */
typedef struct { PVOID Ptr; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT { NULL }

/* ---- Common small structs ---------------------------------------------- */
typedef struct _FILETIME   { DWORD dwLowDateTime; DWORD dwHighDateTime; }
        FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;
typedef struct _RECT  { LONG left, top, right, bottom; } RECT,  *PRECT,  *LPRECT;
typedef struct _POINT { LONG x, y; }                     POINT, *PPOINT, *LPPOINT;
typedef struct _SIZE  { LONG cx, cy; }                   SIZE,  *PSIZE,  *LPSIZE;

/* ---- Boolean / null / misc constants ----------------------------------- */
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

/* MAKE/LOWORD/HIWORD helpers used pervasively in Win32-style code. */
#ifndef LOWORD
#define LOWORD(l)  ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#define HIWORD(l)  ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))
#define LOBYTE(w)  ((BYTE)((DWORD_PTR)(w) & 0xFF))
#define HIBYTE(w)  ((BYTE)(((DWORD_PTR)(w) >> 8) & 0xFF))
#define MAKEWORD(a,b)  ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELONG(a,b)  ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#endif

#ifdef __cplusplus
}
#endif

/* Generic Win32 host primitives (threads, events, heap, timers, ...)
 * implemented on POSIX. Pulled in here so any file that previously expected
 * <windows.h> gets both the NT type vocabulary above and these functions. */
#include "win32_compat.h"

#endif /* _WIN32 */

#endif /* XBOX_WINNT_H */
