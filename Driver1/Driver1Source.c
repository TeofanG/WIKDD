#include <ntddk.h>
#include "../sioctl.h"
#include "TpTests.h"

#define DEVICE_NAME     L"\\Device\\IoctlSample"
#define SYMLINK_NAME    L"\\??\\IoctlSample"

#define DRIVER2_DEVICE_NAME L"\\Device\\IoctlSampleD2"

#define TP_MAX_THREADS 10

static PDEVICE_OBJECT g_D2DeviceObject = NULL;
static PFILE_OBJECT   g_D2FileObject = NULL;

typedef struct _TP_WORK_ITEM
{
    LIST_ENTRY ListEntry;
    ULONG WorkId;
} TP_WORK_ITEM, * PTP_WORK_ITEM;

typedef struct _KERNEL_THREAD_POOL
{
    BOOLEAN Initialized;

    ULONG ThreadCount;
    HANDLE ThreadHandles[TP_MAX_THREADS];

    KEVENT StopEvent;          
    KEVENT WorkAvailableEvent; 

    KSPIN_LOCK QueueLock;
    LIST_ENTRY WorkQueue;
} KERNEL_THREAD_POOL, * PKERNEL_THREAD_POOL;

static KERNEL_THREAD_POOL g_Tp;

static NTSTATUS TpInit(_Inout_ PKERNEL_THREAD_POOL Tp, _In_ ULONG ThreadCount);

static NTSTATUS TpQueueWork(_Inout_ PKERNEL_THREAD_POOL Tp, _In_ ULONG WorkId);

static NTSTATUS TpStop(_Inout_ PKERNEL_THREAD_POOL Tp);

static NTSTATUS TpInit_Void(_Inout_ VOID* Tp, _In_ ULONG ThreadCount)
{
    return TpInit((PKERNEL_THREAD_POOL)Tp, ThreadCount);
}

static NTSTATUS TpQueueWork_Void(_Inout_ VOID* Tp, _In_ ULONG WorkId)
{
    return TpQueueWork((PKERNEL_THREAD_POOL)Tp, WorkId);
}

static NTSTATUS TpStop_Void(_Inout_ VOID* Tp)
{
    return TpStop((PKERNEL_THREAD_POOL)Tp);
}

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

static VOID TpFreeQueuedWorkItems(_Inout_ PKERNEL_THREAD_POOL Tp)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Tp->QueueLock, &oldIrql);

    while (!IsListEmpty(&Tp->WorkQueue))
    {
        PLIST_ENTRY e = RemoveHeadList(&Tp->WorkQueue);
        PTP_WORK_ITEM item = CONTAINING_RECORD(e, TP_WORK_ITEM, ListEntry);
        ExFreePoolWithTag(item, 'krow');
    }

    KeReleaseSpinLock(&Tp->QueueLock, oldIrql);
}

static PTP_WORK_ITEM TpDequeueOne(_Inout_ PKERNEL_THREAD_POOL Tp)
{
    PTP_WORK_ITEM item = NULL;
    KIRQL oldIrql;

    KeAcquireSpinLock(&Tp->QueueLock, &oldIrql);

    if (!IsListEmpty(&Tp->WorkQueue))
    {
        PLIST_ENTRY e = RemoveTailList(&Tp->WorkQueue);
        item = CONTAINING_RECORD(e, TP_WORK_ITEM, ListEntry);
    }

    KeReleaseSpinLock(&Tp->QueueLock, oldIrql);
    return item;
}

static VOID TpWorkerThread(_In_ PVOID StartContext)
{
    PKERNEL_THREAD_POOL tp = (PKERNEL_THREAD_POOL)StartContext;

    PVOID waitObjs[2] = { &tp->StopEvent, &tp->WorkAvailableEvent };

    for (;;)
    {
        NTSTATUS st = KeWaitForMultipleObjects(
            2,
            waitObjs,
            WaitAny,
            Executive,
            KernelMode,
            FALSE,
            NULL,
            NULL);

        if (st == STATUS_WAIT_0)
            break;

        if (st == STATUS_WAIT_1)
        {
            for (;;)
            {
                PTP_WORK_ITEM item = TpDequeueOne(tp);
                if (item == NULL)
                    break;

                if (TpIsTestWorkId(item->WorkId))
                {
                    TpTestWorkRoutine(item->WorkId);
                }
                else
                {
                    DbgPrintEx(
                        DPFLTR_IHVDRIVER_ID,
                        DPFLTR_INFO_LEVEL,
                        "Driver1 TP: Work item processed. WorkId=%lu, TID=%p\n",
                        item->WorkId,
                        PsGetCurrentThreadId());
                }

                ExFreePoolWithTag(item, 'krow');
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS TpInit(_Inout_ PKERNEL_THREAD_POOL Tp, _In_ ULONG ThreadCount)
{
    if (Tp->Initialized)
        return STATUS_DEVICE_BUSY;

    if (ThreadCount == 0 || ThreadCount > TP_MAX_THREADS)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Tp, sizeof(*Tp));

    InitializeListHead(&Tp->WorkQueue);
    KeInitializeSpinLock(&Tp->QueueLock);

    KeInitializeEvent(&Tp->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Tp->WorkAvailableEvent, NotificationEvent, FALSE);

    Tp->ThreadCount = 0;

    for (ULONG i = 0; i < ThreadCount; i++)
    {
        HANDLE hThread = NULL;
        NTSTATUS st = PsCreateSystemThread(
            &hThread,
            THREAD_ALL_ACCESS,
            NULL,
            NULL,
            NULL,
            TpWorkerThread,
            Tp);

        if (!NT_SUCCESS(st))
        {
            KeSetEvent(&Tp->StopEvent, IO_NO_INCREMENT, FALSE);

            for (ULONG j = 0; j < Tp->ThreadCount; j++)
            {
                if (Tp->ThreadHandles[j] != NULL)
                {
                    ZwClose(Tp->ThreadHandles[j]);
                    Tp->ThreadHandles[j] = NULL;
                }
            }

            TpFreeQueuedWorkItems(Tp);
            Tp->ThreadCount = 0;
            Tp->Initialized = FALSE;
            return st;
        }

        Tp->ThreadHandles[i] = hThread;
        Tp->ThreadCount++;
    }

    Tp->Initialized = TRUE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver1 TP: initialized with %lu threads\n", ThreadCount);
    return STATUS_SUCCESS;
}

static NTSTATUS TpQueueWork(_Inout_ PKERNEL_THREAD_POOL Tp, _In_ ULONG WorkId)
{
    if (!Tp->Initialized)
        return STATUS_INVALID_DEVICE_STATE;

    PTP_WORK_ITEM item = (PTP_WORK_ITEM)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(TP_WORK_ITEM), 'krow');
    if (item == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(item, sizeof(*item));
    item->WorkId = WorkId;

    KIRQL oldIrql;
    KeAcquireSpinLock(&Tp->QueueLock, &oldIrql);
    InsertHeadList(&Tp->WorkQueue, &item->ListEntry);
    KeReleaseSpinLock(&Tp->QueueLock, oldIrql);

    KeSetEvent(&Tp->WorkAvailableEvent, IO_NO_INCREMENT, FALSE);

    return STATUS_SUCCESS;
}

static NTSTATUS TpStop(_Inout_ PKERNEL_THREAD_POOL Tp)
{
    if (!Tp->Initialized)
        return STATUS_INVALID_DEVICE_STATE;

    KeSetEvent(&Tp->StopEvent, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&Tp->WorkAvailableEvent, IO_NO_INCREMENT, FALSE);

    for (ULONG i = 0; i < Tp->ThreadCount; i++)
    {
        if (Tp->ThreadHandles[i] != NULL)
        {
            PVOID threadObj = NULL;
            NTSTATUS st = ObReferenceObjectByHandle(
                Tp->ThreadHandles[i],
                SYNCHRONIZE,
                *PsThreadType,
                KernelMode,
                &threadObj,
                NULL);

            if (NT_SUCCESS(st))
            {
                KeWaitForSingleObject(threadObj, Executive, KernelMode, FALSE, NULL);
                ObDereferenceObject(threadObj);
            }

            ZwClose(Tp->ThreadHandles[i]);
            Tp->ThreadHandles[i] = NULL;
        }
    }

    TpFreeQueuedWorkItems(Tp);

    Tp->ThreadCount = 0;
    Tp->Initialized = FALSE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Driver1 TP: stopped/uninitialized\n");
    return STATUS_SUCCESS;
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

    case IOCTL_TP_INIT:
    {
        if (irpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(TP_INIT_INPUT))
        {
            CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            return STATUS_BUFFER_TOO_SMALL;
        }

        PTP_INIT_INPUT in = (PTP_INIT_INPUT)Irp->AssociatedIrp.SystemBuffer;
        NTSTATUS st = TpInit(&g_Tp, in->ThreadCount);

        CompleteIrp(Irp, st, 0);
        return st;
    }

    case IOCTL_TP_QUEUE_WORK:
    {
        if (irpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(TP_QUEUE_INPUT))
        {
            CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            return STATUS_BUFFER_TOO_SMALL;
        }

        PTP_QUEUE_INPUT in = (PTP_QUEUE_INPUT)Irp->AssociatedIrp.SystemBuffer;
        NTSTATUS st = TpQueueWork(&g_Tp, in->WorkId);

        CompleteIrp(Irp, st, 0);
        return st;
    }

    case IOCTL_TP_STOP:
    {
        NTSTATUS st = TpStop(&g_Tp);
        CompleteIrp(Irp, st, 0);
        return st;
    }

    case IOCTL_TP_RUN_TESTS:
    {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(TP_TEST_OUTPUT))
        {
            CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            return STATUS_BUFFER_TOO_SMALL;
        }

        TP_TEST_INPUT inLocal = { 0 };
        if (irpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(TP_TEST_INPUT))
        {
            RtlCopyMemory(&inLocal, Irp->AssociatedIrp.SystemBuffer, sizeof(TP_TEST_INPUT));
        }

        PTP_TEST_OUTPUT outBuf = (PTP_TEST_OUTPUT)Irp->AssociatedIrp.SystemBuffer;

        NTSTATUS st = TpRunAllTests(
            TP_MAX_THREADS,
            &g_Tp,
            TpInit_Void,
            TpQueueWork_Void,
            TpStop_Void,
            &inLocal,
            outBuf);

        CompleteIrp(Irp, st, sizeof(TP_TEST_OUTPUT));
        return st;
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

    (VOID)TpStop(&g_Tp);

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