/*
 * kernel_rtl.c - Xbox Runtime Library Functions
 *
 * Implements Rtl* functions: critical sections, string init/conversion,
 * NTSTATUS→Win32 error mapping, time conversion, sprintf variants.
 *
 * Most of these map 1:1 to Win32 CRT functions.
 */

#include "kernel.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * String Initialization
 * ============================================================================ */

VOID __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)strlen(SourceString);
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 1;
        DestinationString->Buffer = (PCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

VOID __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)(wcslen(SourceString) * sizeof(WCHAR));
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + sizeof(WCHAR);
        DestinationString->Buffer = (PWCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

/* ============================================================================
 * String Conversion (ANSI ↔ Unicode)
 * ============================================================================ */

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG unicode_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    unicode_len = (SourceString->Length + 1) * sizeof(WCHAR);

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, unicode_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)unicode_len;
    } else if (DestinationString->MaximumLength < unicode_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = MultiByteToWideChar(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length,
        DestinationString->Buffer, DestinationString->MaximumLength / sizeof(WCHAR));

    if (result > 0) {
        DestinationString->Length = (USHORT)(result * sizeof(WCHAR));
        DestinationString->Buffer[result] = L'\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG ansi_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    ansi_len = SourceString->Length / sizeof(WCHAR) + 1;

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ansi_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)ansi_len;
    } else if (DestinationString->MaximumLength < ansi_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = WideCharToMultiByte(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer, DestinationString->MaximumLength,
        NULL, NULL);

    if (result >= 0) {
        DestinationString->Length = (USHORT)result;
        if ((USHORT)result < DestinationString->MaximumLength)
            DestinationString->Buffer[result] = '\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * String Comparison
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlEqualString(
    PXBOX_ANSI_STRING String1,
    PXBOX_ANSI_STRING String2,
    BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;

    if (CaseInSensitive)
        return _strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
    else
        return strncmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
}

/*
 * RtlCompareMemoryUlong - Scans memory for a ULONG pattern.
 * Returns the number of bytes that matched.
 */
ULONG __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)
{
    PULONG src = (PULONG)Source;
    ULONG count = Length / sizeof(ULONG);

    for (ULONG i = 0; i < count; i++) {
        if (src[i] != Pattern)
            return i * sizeof(ULONG);
    }
    return count * sizeof(ULONG);
}

/* ============================================================================
 * Critical Sections (direct 1:1 mapping)
 * ============================================================================ */

VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    EnterCriticalSection(CriticalSection);
}

VOID __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    LeaveCriticalSection(CriticalSection);
}

VOID __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    InitializeCriticalSection(CriticalSection);
}

/* ============================================================================
 * NTSTATUS → Win32 Error Code Mapping
 * ============================================================================ */

ULONG __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    switch (Status) {
        case STATUS_SUCCESS:                    return ERROR_SUCCESS;
        case STATUS_INVALID_PARAMETER:          return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MEMORY:                  return ERROR_NOT_ENOUGH_MEMORY;
        case STATUS_INSUFFICIENT_RESOURCES:     return ERROR_NO_SYSTEM_RESOURCES;
        case STATUS_ACCESS_DENIED:              return ERROR_ACCESS_DENIED;
        case STATUS_OBJECT_NAME_NOT_FOUND:      return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND:      return ERROR_PATH_NOT_FOUND;
        case STATUS_OBJECT_NAME_COLLISION:      return ERROR_ALREADY_EXISTS;
        case STATUS_NO_SUCH_FILE:               return ERROR_FILE_NOT_FOUND;
        case STATUS_END_OF_FILE:                return ERROR_HANDLE_EOF;
        case STATUS_INVALID_HANDLE:             return ERROR_INVALID_HANDLE;
        case STATUS_NOT_IMPLEMENTED:            return ERROR_CALL_NOT_IMPLEMENTED;
        case STATUS_UNSUCCESSFUL:               return ERROR_GEN_FAILURE;
        case STATUS_PENDING:                    return ERROR_IO_PENDING;
        case STATUS_BUFFER_OVERFLOW:            return ERROR_MORE_DATA;
        case STATUS_NO_MORE_FILES:              return ERROR_NO_MORE_FILES;
        case STATUS_NOT_SUPPORTED:              return ERROR_NOT_SUPPORTED;
        case STATUS_CANCELLED:                  return ERROR_CANCELLED;
        case STATUS_ALREADY_COMMITTED:          return ERROR_COMMITMENT_LIMIT;
        default:
            /* Fall back to RtlNtStatusToDosError from ntdll if available */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_RTL,
                "RtlNtStatusToDosError: unmapped status 0x%08X", Status);
            return ERROR_MR_MID_NOT_FOUND;
    }
}

/* ============================================================================
 * Time Conversion
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;

    st.wYear         = (WORD)TimeFields->Year;
    st.wMonth        = (WORD)TimeFields->Month;
    st.wDayOfWeek    = (WORD)TimeFields->Weekday;
    st.wDay          = (WORD)TimeFields->Day;
    st.wHour         = (WORD)TimeFields->Hour;
    st.wMinute       = (WORD)TimeFields->Minute;
    st.wSecond       = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;

    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;

    Time->LowPart  = ft.dwLowDateTime;
    Time->HighPart = ft.dwHighDateTime;
    return TRUE;
}

VOID __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime  = Time->LowPart;
    ft.dwHighDateTime = Time->HighPart;

    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = (SHORT)st.wYear;
        TimeFields->Month        = (SHORT)st.wMonth;
        TimeFields->Day          = (SHORT)st.wDay;
        TimeFields->Hour         = (SHORT)st.wHour;
        TimeFields->Minute       = (SHORT)st.wMinute;
        TimeFields->Second       = (SHORT)st.wSecond;
        TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
        TimeFields->Weekday      = (SHORT)st.wDayOfWeek;
    } else {
        memset(TimeFields, 0, sizeof(XBOX_TIME_FIELDS));
    }
}

/* ============================================================================
 * Exception Handling
 * ============================================================================ */

VOID __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)
{
    /* Delegate to Win32 RtlUnwind */
    RtlUnwind(TargetFrame, TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, ReturnValue);
}

VOID __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord)
{
    RaiseException(
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionCode,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionFlags,
        ((PEXCEPTION_RECORD)ExceptionRecord)->NumberParameters,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionInformation);
}

VOID __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlRip: %s - %s: %s",
        ApiName ? ApiName : "?",
        Expression ? Expression : "?",
        Message ? Message : "?");

#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * String Formatting (Rtl sprintf variants → CRT)
 * ============================================================================ */

int __cdecl xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlSprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr)
{
    return vsnprintf(buffer, count, format, argptr);
}

int __cdecl xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr)
{
    return vsprintf(buffer, format, argptr);
}
