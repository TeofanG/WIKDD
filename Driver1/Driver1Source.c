#include <ntddk.h>
#include "../sioctl.h"

#define DEVICE_NAME     L"\\Device\\IoctlSample"
#define SYMLINK_NAME    L"\\??\\IoctlSample"

#define DRIVER2_DEVICE_NAME L"\\Device\\IoctlSampleD2"

static PDEVICE_OBJECT g_D2DeviceObject = NULL;
static PFILE_OBJECT   g_D2FileObject = NULL;

static NTSTATUS ResolveDriver2IfNeeded()
{
    if (g_D2DeviceObject != NULL && g_D2FileObject != NULL)
        return STATUS_SUCCESS;

    UNICODE_STRING d2Name = RTL_CONSTANT_STRING(DRIVER2_DEVICE_NAME);

    NTSTATUS status = IoGetDeviceObjectPointer(
        &d2Name,
        FILE_READ_DATA | FILE_WRITE_DATA,
        &g_D2FileObject,
        &g_D2DeviceObject);

    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(
            DPFLTR_IHVDRIVER_ID,
            DPFLTR_ERROR_LEVEL,
            "Driver1: IoGetDeviceObjectPointer(%wZ) failed: 0x%08X\n",
            &d2Name,
            status);
        return status;
    }

    return STATUS_SUCCESS;
}

static VOID CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static NTSTATUS DriverCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    CompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS ForwardCompletionRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_ERROR_LEVEL,
        "Driver1: Forwarded IRP completed. Status=0x%08X\n",
        Irp->IoStatus.Status);

    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS DriverDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;

    switch (code)
    {
    case IOCTL_FIRST_CALL:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Driver1: First IOCTL was called\n");
        CompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    case IOCTL_SECOND_CALL:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Driver1: The second IOCTL was called\n");
        CompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    case IOCTL_FORWARD_TO_D2:
    {
        NTSTATUS st = ResolveDriver2IfNeeded();
        if (!NT_SUCCESS(st) || g_D2DeviceObject == NULL)
        {
            CompleteIrp(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
            return STATUS_DEVICE_DOES_NOT_EXIST;
        }

        IoCopyCurrentIrpStackLocationToNext(Irp);

        IoSetCompletionRoutine(
            Irp,
            ForwardCompletionRoutine,
            NULL,
            TRUE,
            TRUE,
            TRUE);

        return IoCallDriver(g_D2DeviceObject, Irp);
    }

    default:
        CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(SYMLINK_NAME);
    IoDeleteSymbolicLink(&symLink);

    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    if (g_D2FileObject)
    {
        ObDereferenceObject(g_D2FileObject);
        g_D2FileObject = NULL;
        g_D2DeviceObject = NULL;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Driver1: Unloaded\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING devName = RTL_CONSTANT_STRING(DEVICE_NAME);
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(SYMLINK_NAME);

    status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &deviceObject);
    if (!NT_SUCCESS(status))
        return status;

    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(deviceObject);
        return status;
    }

    // set StackSize 
    status = ResolveDriver2IfNeeded();
    if (NT_SUCCESS(status) && g_D2DeviceObject != NULL)
    {
        if (deviceObject->StackSize < (g_D2DeviceObject->StackSize + 1))
            deviceObject->StackSize = (g_D2DeviceObject->StackSize + 1);

        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "Driver1: StackSize set to %u for forwarding\n", deviceObject->StackSize);
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Driver1: Loaded\n");
    return STATUS_SUCCESS;
}