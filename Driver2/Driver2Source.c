#include <ntddk.h>
#include "../sioctl.h"

#define DEVICE2_NAME L"\\Device\\IoctlSampleD2"

static VOID CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static NTSTATUS Driver2CreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    CompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS Driver2DeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;

    switch (code)
    {
    case IOCTL_FIRST_CALL:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver2: First IOCTL was called\n");
        CompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    case IOCTL_SECOND_CALL:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver2: The second IOCTL was called\n");
        CompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    case IOCTL_FORWARD_TO_D2:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver2: Forward IOCTL was called\n");
        CompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    default:
        CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static VOID Driver2Unload(_In_ PDRIVER_OBJECT DriverObject)
{
    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver2: Unloaded\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING devName = RTL_CONSTANT_STRING(DEVICE2_NAME);
    PDEVICE_OBJECT deviceObject = NULL;

    NTSTATUS status = IoCreateDevice(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &deviceObject);

    if (!NT_SUCCESS(status))
        return status;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = Driver2CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = Driver2CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Driver2DeviceControl;
    DriverObject->DriverUnload = Driver2Unload;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver2: Loaded\n");
    return STATUS_SUCCESS;
}