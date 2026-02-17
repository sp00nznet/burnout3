/*
 * kernel_io.c - I/O Manager Stubs
 *
 * Implements IoCreateDevice, IoDeleteDevice, IRP management, and I/O
 * completion stubs.
 *
 * The Xbox I/O manager is used internally by XDK libraries (DSOUND, D3D,
 * storage drivers). Since we replace those libraries entirely with Win32
 * equivalents, most I/O manager functions are stubs that return success.
 */

#include "kernel.h"
#include <string.h>

/* ============================================================================
 * Type Object Pointers
 *
 * Exported type objects for device and completion port types.
 * Game code rarely uses these directly - they're mainly for the
 * XDK libraries we're replacing.
 * ============================================================================ */

static ULONG g_device_object_type_data     = 0x44455643; /* 'DEVC' */
static ULONG g_completion_object_type_data = 0x434F4D50; /* 'COMP' */

PVOID xbox_IoDeviceObjectType     = &g_device_object_type_data;
PVOID xbox_IoCompletionObjectType = &g_completion_object_type_data;

/* ============================================================================
 * Device Management
 *
 * IoCreateDevice/IoDeleteDevice create kernel device objects. Since we
 * don't have real kernel drivers, we allocate dummy objects that satisfy
 * the interface without actual device functionality.
 * ============================================================================ */

/* Minimal device object - just enough to not crash if someone reads fields */
typedef struct _XBOX_FAKE_DEVICE {
    ULONG Type;
    ULONG Size;
    PVOID DeviceExtension;
    PVOID DriverObject;
} XBOX_FAKE_DEVICE;

NTSTATUS __stdcall xbox_IoCreateDevice(
    PVOID DriverObject,
    ULONG DeviceExtensionSize,
    PXBOX_ANSI_STRING DeviceName,
    ULONG DeviceType,
    BOOLEAN Exclusive,
    PVOID* DeviceObject)
{
    XBOX_FAKE_DEVICE* device;
    ULONG total_size;

    (void)DriverObject;
    (void)DeviceType;
    (void)Exclusive;

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    total_size = sizeof(XBOX_FAKE_DEVICE) + DeviceExtensionSize;
    device = (XBOX_FAKE_DEVICE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_size);
    if (!device) {
        *DeviceObject = NULL;
        return STATUS_NO_MEMORY;
    }

    device->Type = DeviceType;
    device->Size = total_size;
    device->DriverObject = DriverObject;
    device->DeviceExtension = (DeviceExtensionSize > 0)
        ? (PVOID)((UCHAR*)device + sizeof(XBOX_FAKE_DEVICE))
        : NULL;

    *DeviceObject = device;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
        "IoCreateDevice: '%.*s' type=%u ext_size=%u → %p",
        DeviceName ? DeviceName->Length : 0,
        DeviceName ? DeviceName->Buffer : "<null>",
        DeviceType, DeviceExtensionSize, device);

    return STATUS_SUCCESS;
}

VOID __stdcall xbox_IoDeleteDevice(PVOID DeviceObject)
{
    if (DeviceObject) {
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
            "IoDeleteDevice: %p", DeviceObject);
        HeapFree(GetProcessHeap(), 0, DeviceObject);
    }
}

/* ============================================================================
 * IRP Management Stubs
 *
 * IRPs (I/O Request Packets) are the core mechanism for driver I/O on Xbox.
 * Since we don't use real drivers, these are all stubs.
 * ============================================================================ */

VOID __stdcall xbox_IoInitializeIrp(PVOID Irp, USHORT PacketSize, CCHAR StackSize)
{
    (void)StackSize;
    if (Irp)
        memset(Irp, 0, PacketSize);
}

VOID __stdcall xbox_IoStartNextPacket(PVOID DeviceObject, BOOLEAN Cancelable)
{
    (void)DeviceObject;
    (void)Cancelable;
}

VOID __stdcall xbox_IoStartNextPacketByKey(PVOID DeviceObject, BOOLEAN Cancelable, ULONG Key)
{
    (void)DeviceObject;
    (void)Cancelable;
    (void)Key;
}

VOID __stdcall xbox_IoStartPacket(PVOID DeviceObject, PVOID Irp, PULONG Key, PVOID CancelFunction)
{
    (void)DeviceObject;
    (void)Irp;
    (void)Key;
    (void)CancelFunction;
}

VOID __stdcall xbox_IoMarkIrpMustComplete(PVOID Irp)
{
    (void)Irp;
}

/* ============================================================================
 * I/O Completion Ports
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSetIoCompletion(
    PVOID IoCompletion,
    PVOID KeyContext,
    PVOID ApcContext,
    NTSTATUS IoStatus,
    ULONG_PTR IoStatusInformation)
{
    (void)IoCompletion;
    (void)KeyContext;
    (void)ApcContext;
    (void)IoStatus;
    (void)IoStatusInformation;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSetIoCompletion: completion=%p status=0x%08X (stubbed)",
        IoCompletion, IoStatus);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Synchronous Device I/O
 *
 * These functions perform synchronous I/O through the driver stack.
 * Stubbed since we replace all Xbox drivers.
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSynchronousDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnedOutputBufferLength,
    BOOLEAN InternalDeviceIoControl)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;

    if (ReturnedOutputBufferLength)
        *ReturnedOutputBufferLength = 0;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousDeviceIoControlRequest: ioctl=0x%08X device=%p (stubbed)",
        IoControlCode, DeviceObject);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;
    (void)Event;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset)
{
    (void)MajorFunction;
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousFsdRequest: major=%u device=%p (stubbed)",
        MajorFunction, DeviceObject);

    return STATUS_SUCCESS;
}
