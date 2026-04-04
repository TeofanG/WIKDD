#include <ntifs.h>

#define TAG_PROTECT 'PROT'
#define PRINT(_Format, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, _Format, __VA_ARGS__)

#define MAX_PROTECTED_PROCESSES 10

typedef struct _PROTECTED_PROCESS
{
    ULONG ProcessId;
    PVOID ProcessObject;
} PROTECTED_PROCESS, *PPROTECTED_PROCESS;

static PROTECTED_PROCESS g_ProtectedProcesses[MAX_PROTECTED_PROCESSES];
static KSPIN_LOCK g_ProtectionLock;
static PVOID g_ObRegistrationHandle = NULL;

static OB_PREOP_CALLBACK_STATUS
ObPreOperationCallback(
    _In_ PVOID RegistrationContext,
    _Inout_ POB_PRE_OPERATION_INFORMATION PreInfo
)
{
    UNREFERENCED_PARAMETER(RegistrationContext);

    if (!PreInfo || !PreInfo->Object || PreInfo->ObjectType != *PsProcessType)
        return OB_PREOP_SUCCESS;

    if ((PreInfo->Operation & OB_OPERATION_HANDLE_CREATE) == 0)
        return OB_PREOP_SUCCESS;

    if ((PreInfo->Parameters->CreateHandleInformation.DesiredAccess & 1) == 0)
        return OB_PREOP_SUCCESS;

    PEPROCESS targetProcess = (PEPROCESS)PreInfo->Object;
    HANDLE targetPid = PsGetProcessId(targetProcess);
    KIRQL oldIrql;
    ULONG i;

    KeAcquireSpinLock(&g_ProtectionLock, &oldIrql);

    for (i = 0; i < MAX_PROTECTED_PROCESSES; i++)
    {
        if (g_ProtectedProcesses[i].ProcessId == (ULONG)(SIZE_T)targetPid)
        {
            PreInfo->Parameters->CreateHandleInformation.DesiredAccess &= ~1;
            PRINT("Blocked termination attempt on protected process PID: %lu\n", g_ProtectedProcesses[i].ProcessId);
            break;
        }
    }

    KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
    return OB_PREOP_SUCCESS;
}

static VOID
ObPostOperationCallback(
    _In_ PVOID RegistrationContext,
    _In_ POB_POST_OPERATION_INFORMATION OperationInformation
)
{
    UNREFERENCED_PARAMETER(RegistrationContext);
    UNREFERENCED_PARAMETER(OperationInformation);
}

NTSTATUS ProtectProcess(
    _In_ ULONG ProcessId
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PEPROCESS processObject = NULL;
    KIRQL oldIrql;
    ULONG i;

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status))
    {
        PRINT("PsLookupProcessByProcessId failed for PID %lu: 0x%X\n", ProcessId, status);
        return status;
    }

    KeAcquireSpinLock(&g_ProtectionLock, &oldIrql);

    // Check if already protected
    for (i = 0; i < MAX_PROTECTED_PROCESSES; i++)
    {
        if (g_ProtectedProcesses[i].ProcessId == ProcessId)
        {
            KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
            ObDereferenceObject(processObject);
            PRINT("Process PID %lu is already protected\n", ProcessId);
            return STATUS_ALREADY_COMMITTED;
        }
    }

    for (i = 0; i < MAX_PROTECTED_PROCESSES; i++)
    {
        if (g_ProtectedProcesses[i].ProcessId == 0)
        {
            g_ProtectedProcesses[i].ProcessId = ProcessId;
            g_ProtectedProcesses[i].ProcessObject = processObject;
            PRINT("Process PID %lu is now protected\n", ProcessId);
            KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
            return STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
    ObDereferenceObject(processObject);
    PRINT("Protection list is full\n");
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS UnprotectProcess(
    _In_ ULONG ProcessId
)
{
    KIRQL oldIrql;
    ULONG i;

    KeAcquireSpinLock(&g_ProtectionLock, &oldIrql);

    for (i = 0; i < MAX_PROTECTED_PROCESSES; i++)
    {
        if (g_ProtectedProcesses[i].ProcessId == ProcessId)
        {
            if (g_ProtectedProcesses[i].ProcessObject)
                ObDereferenceObject(g_ProtectedProcesses[i].ProcessObject);

            g_ProtectedProcesses[i].ProcessId = 0;
            g_ProtectedProcesses[i].ProcessObject = NULL;
            PRINT("Process PID %lu is no longer protected\n", ProcessId);
            KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
            return STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
    PRINT("Process PID %lu was not in protection list\n", ProcessId);
    return STATUS_NOT_FOUND;
}

NTSTATUS ProcessProtectionInit(void)
{
    NTSTATUS status;
    OB_OPERATION_REGISTRATION operationRegistrations[1];
    OB_CALLBACK_REGISTRATION callbackRegistration;
    UNICODE_STRING altitude;

    status = STATUS_SUCCESS;

    KeInitializeSpinLock(&g_ProtectionLock);
    RtlZeroMemory(g_ProtectedProcesses, sizeof(g_ProtectedProcesses));

    RtlZeroMemory(operationRegistrations, sizeof(operationRegistrations));

    operationRegistrations[0].ObjectType = PsProcessType;
    operationRegistrations[0].Operations = OB_OPERATION_HANDLE_CREATE;
    operationRegistrations[0].PreOperation = ObPreOperationCallback;
    operationRegistrations[0].PostOperation = ObPostOperationCallback;

    RtlZeroMemory(&callbackRegistration, sizeof(callbackRegistration));

    RtlInitUnicodeString(&altitude, L"1000");
    
    callbackRegistration.Version = OB_FLT_REGISTRATION_VERSION;
    callbackRegistration.OperationRegistrationCount = 1;
    callbackRegistration.OperationRegistration = operationRegistrations;
    callbackRegistration.Altitude = altitude;
    callbackRegistration.RegistrationContext = NULL;

    status = ObRegisterCallbacks(&callbackRegistration, &g_ObRegistrationHandle);
    if (!NT_SUCCESS(status))
    {
        PRINT("ObRegisterCallbacks failed: 0x%X\n", status);
        return status;
    }

    PRINT("Process protection initialized\n");
    return STATUS_SUCCESS;
}

VOID ProcessProtectionCleanup(void)
{
    KIRQL oldIrql;
    ULONG i;

    if (g_ObRegistrationHandle)
    {
        ObUnRegisterCallbacks(g_ObRegistrationHandle);
        g_ObRegistrationHandle = NULL;
    }

    KeAcquireSpinLock(&g_ProtectionLock, &oldIrql);

    for (i = 0; i < MAX_PROTECTED_PROCESSES; i++)
    {
        if (g_ProtectedProcesses[i].ProcessObject)
        {
            ObDereferenceObject(g_ProtectedProcesses[i].ProcessObject);
            g_ProtectedProcesses[i].ProcessObject = NULL;
            g_ProtectedProcesses[i].ProcessId = 0;
        }
    }

    KeReleaseSpinLock(&g_ProtectionLock, oldIrql);
    PRINT("Process protection cleaned up\n");
}