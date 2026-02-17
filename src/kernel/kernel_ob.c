/*
 * kernel_ob.c - Object Manager
 *
 * Implements object reference counting and handle-to-object resolution.
 *
 * The Xbox kernel uses a simplified NT object manager. Objects have reference
 * counts and can be looked up by handle or name. Since we use Win32 HANDLEs
 * directly, most of these are thin wrappers.
 *
 * ObfReferenceObject/ObfDereferenceObject use __fastcall on Xbox (parameter
 * in ECX register).
 */

#include "kernel.h"

/* ============================================================================
 * Type Object Pointers
 *
 * Xbox kernel exports type objects for common types. Game code uses these
 * with ObReferenceObjectByHandle to validate handle types.
 * We provide dummy non-NULL pointers so type checks pass.
 * ============================================================================ */

static ULONG g_thread_object_type_data = 0x54485244; /* 'THRD' */
static ULONG g_event_object_type_data  = 0x45564E54; /* 'EVNT' */

PVOID xbox_PsThreadObjectType = &g_thread_object_type_data;
PVOID xbox_ExEventObjectType  = &g_event_object_type_data;

/* ============================================================================
 * Reference Counting
 *
 * On Xbox, kernel objects have embedded reference counts. On Windows,
 * HANDLEs manage their own lifetime, so we track references loosely.
 *
 * ObfDereferenceObject: Decrements ref count. If it reaches zero on Xbox,
 * the object is freed. We approximate by closing the handle on deref.
 * However, since game code may hold multiple references, we only close
 * when explicitly told via NtClose. These become approximate no-ops.
 * ============================================================================ */

VOID __fastcall xbox_ObfReferenceObject(PVOID Object)
{
    /*
     * Increment reference count. With Win32 handles, the OS manages
     * the underlying reference count via DuplicateHandle/CloseHandle.
     * We don't need to track this separately for correctness.
     */
    (void)Object;
    XBOX_TRACE(XBOX_LOG_OB, "ObfReferenceObject: %p", Object);
}

VOID __fastcall xbox_ObfDereferenceObject(PVOID Object)
{
    /*
     * Decrement reference count. We don't close the handle here because
     * game code may still use it. The handle is closed via NtClose.
     */
    (void)Object;
    XBOX_TRACE(XBOX_LOG_OB, "ObfDereferenceObject: %p", Object);
}

/* ============================================================================
 * ObReferenceObjectByHandle
 *
 * Retrieves the object pointer for a given handle. On Xbox, this returns
 * a pointer to the kernel object structure. On Windows, we simply pass
 * the handle through as the "object pointer" since our Ke* functions
 * treat object pointers as HANDLEs.
 * ============================================================================ */

NTSTATUS __stdcall xbox_ObReferenceObjectByHandle(
    HANDLE Handle,
    PVOID ObjectType,
    PVOID* Object)
{
    (void)ObjectType;

    if (!Object)
        return STATUS_INVALID_PARAMETER;

    if (!Handle || Handle == INVALID_HANDLE_VALUE) {
        *Object = NULL;
        return STATUS_INVALID_HANDLE;
    }

    /* Return the handle as the "object pointer" */
    *Object = (PVOID)Handle;

    XBOX_TRACE(XBOX_LOG_OB, "ObReferenceObjectByHandle: handle=%p → object=%p",
        Handle, *Object);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * ObReferenceObjectByName
 *
 * Looks up a named kernel object. On Xbox, named objects include devices,
 * symbolic links, etc. Since we don't maintain a kernel object namespace,
 * this is stubbed to return not-found for most cases.
 * ============================================================================ */

NTSTATUS __stdcall xbox_ObReferenceObjectByName(
    PXBOX_ANSI_STRING ObjectName,
    ULONG Attributes,
    PVOID ObjectType,
    PVOID ParseContext,
    PVOID* Object)
{
    (void)Attributes;
    (void)ObjectType;
    (void)ParseContext;

    if (!Object)
        return STATUS_INVALID_PARAMETER;

    *Object = NULL;

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_OB,
        "ObReferenceObjectByName: '%.*s' (not found - stubbed)",
        ObjectName ? ObjectName->Length : 0,
        ObjectName ? ObjectName->Buffer : "<null>");

    return STATUS_OBJECT_NAME_NOT_FOUND;
}
