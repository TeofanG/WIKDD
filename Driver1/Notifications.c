#include <ntifs.h>

#define TAG_NOTIF 'NOTF'
#define PRINT(_Format, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, _Format, __VA_ARGS__)

typedef NTSTATUS (NTAPI* PFUNC_ZwQueryInformationProcess)(
    _In_ HANDLE ProcessHandle,
    _In_ PROCESSINFOCLASS ProcessInformationClass,
    _Out_ PVOID ProcessInformation,
    _In_ ULONG ProcessInformationLength,
    _Out_opt_ PULONG ReturnLength
);

static PFUNC_ZwQueryInformationProcess g_pfnZwQueryInformationProcess = NULL;
static LARGE_INTEGER g_RegistryCookie = { 0 };
static PVOID g_ObRegistrationHandle = NULL;

static NTSTATUS GetImagePathFromHandle(
    _In_ HANDLE hProcess,
    _Out_ PUNICODE_STRING* ProcessPath
)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG nameSize = 0;
    PUNICODE_STRING pPath = NULL;

    *ProcessPath = NULL;

    __try
    {
        status = g_pfnZwQueryInformationProcess(
            hProcess,
            ProcessImageFileName,
            NULL,
            0,
            &nameSize);

        if (status != STATUS_INFO_LENGTH_MISMATCH)
            __leave;

        pPath = (PUNICODE_STRING)ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            nameSize,
            TAG_NOTIF);

        if (!pPath)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        status = g_pfnZwQueryInformationProcess(
            hProcess,
            ProcessImageFileName,
            pPath,
            nameSize,
            &nameSize);

        if (!NT_SUCCESS(status))
            __leave;

        *ProcessPath = pPath;
        status = STATUS_SUCCESS;
    }
    __finally
    {
        if (!NT_SUCCESS(status) && pPath)
            ExFreePoolWithTag(pPath, TAG_NOTIF);
    }

    return status;
}

// Callback: Process Creation Notification
static VOID PsCreateProcessNotifyRoutineEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
)
{
    UNREFERENCED_PARAMETER(Process);

    if (!CreateInfo)
        return;

    PRINT("---- PROCESS CREATED ----\n");
    PRINT("PID: %p\n", ProcessId);

    if (CreateInfo->ImageFileName)
        PRINT("ImagePath: %wZ\n", CreateInfo->ImageFileName);

    if (CreateInfo->CommandLine)
        PRINT("CommandLine: %wZ\n", CreateInfo->CommandLine);

    PRINT("-----------------------\n");
}

// Callback: Driver/Image Load Notification
static VOID PsLoadImageNotifyRoutine(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo
)
{
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(ImageInfo);

    if (!FullImageName)
        return;

    PRINT("==== IMAGE LOADED ====\n");
    PRINT("ImagePath: %wZ\n", FullImageName);
    PRINT("======================\n");
}

static NTSTATUS CmRegistryCallback(
    _In_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
)
{
    UNREFERENCED_PARAMETER(CallbackContext);

    if (!Argument2)
        return STATUS_SUCCESS;

    REG_NOTIFY_CLASS regNotifyClass = (REG_NOTIFY_CLASS)(SIZE_T)Argument1;
    PVOID pParameters = Argument2;

    if (regNotifyClass == RegNtPreRenameKey)
    {
        PREG_RENAME_KEY_INFORMATION pRenameInfo = 
            (PREG_RENAME_KEY_INFORMATION)pParameters;

        if (!pRenameInfo)
            return STATUS_SUCCESS;

        PRINT("----- REGISTRY KEY RENAME-------\n");
        
        if (pRenameInfo->NewName)
            PRINT("NewName: %wZ\n", pRenameInfo->NewName);

        if (pRenameInfo->Object)
        {
            ULONG_PTR objectId;
            PUNICODE_STRING objectName = NULL;
            NTSTATUS status = CmCallbackGetKeyObjectIDEx(
                &g_RegistryCookie,
                pRenameInfo->Object,
                &objectId,
                &objectName,
                0);

            if (NT_SUCCESS(status) && objectName)
            {
                PRINT("OldName: %wZ\n", objectName);
                CmCallbackReleaseKeyObjectIDEx(objectName);
            }
        }

        PRINT("----------------------------\n");
    }
    else if (regNotifyClass == RegNtPostRenameKey)
    {
        PREG_POST_OPERATION_INFORMATION pPostInfo = 
            (PREG_POST_OPERATION_INFORMATION)pParameters;

        if (!pPostInfo)
            return STATUS_SUCCESS;

        PRINT("----REGISTRY KEY RENAME(POST)---\n");
        PRINT("Status: 0x%08X\n", pPostInfo->Status);
        PRINT("--------------------------------\n");
    }

    return STATUS_SUCCESS;
}

NTSTATUS NotificationsInit(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    NTSTATUS status = STATUS_SUCCESS;

    UNICODE_STRING funcName = RTL_CONSTANT_STRING(L"ZwQueryInformationProcess");
    g_pfnZwQueryInformationProcess = 
        (PFUNC_ZwQueryInformationProcess)(SIZE_T)MmGetSystemRoutineAddress(&funcName);

    if (!g_pfnZwQueryInformationProcess)
    {
        PRINT("MmGetSystemRoutineAddress failed for ZwQueryInformationProcess\n");
        return STATUS_NOT_FOUND;
    }

    // Register process creation callback
    status = PsSetCreateProcessNotifyRoutineEx(
        PsCreateProcessNotifyRoutineEx,
        FALSE);

    if (!NT_SUCCESS(status))
    {
        PRINT("PsSetCreateProcessNotifyRoutineEx failed: 0x%X\n", status);
        return status;
    }

    // Register image load callback
    status = PsSetLoadImageNotifyRoutine(PsLoadImageNotifyRoutine);
    if (!NT_SUCCESS(status))
    {
        PRINT("PsSetLoadImageNotifyRoutine failed: 0x%X\n", status);
        PsSetCreateProcessNotifyRoutineEx(PsCreateProcessNotifyRoutineEx, TRUE);
        return status;
    }

    // Register registry callback
    UNICODE_STRING altitude = RTL_CONSTANT_STRING(L"1000");
    status = CmRegisterCallbackEx(
        CmRegistryCallback,
        &altitude,
        DriverObject,
        NULL,
        &g_RegistryCookie,
        NULL);

    if (!NT_SUCCESS(status))
    {
        PRINT("CmRegisterCallbackEx failed: 0x%X\n", status);
        PsSetCreateProcessNotifyRoutineEx(PsCreateProcessNotifyRoutineEx, TRUE);
        PsRemoveLoadImageNotifyRoutine(PsLoadImageNotifyRoutine);
        return status;
    }

    PRINT("All notifications registered successfully\n");
    return STATUS_SUCCESS;
}

VOID NotificationsCleanup(void)
{
    PsSetCreateProcessNotifyRoutineEx(PsCreateProcessNotifyRoutineEx, TRUE);
    PsRemoveLoadImageNotifyRoutine(PsLoadImageNotifyRoutine);

    if (g_RegistryCookie.QuadPart != 0)
        CmUnRegisterCallback(g_RegistryCookie);

    PRINT("All notifications unregistered\n");
}