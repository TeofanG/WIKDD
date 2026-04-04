#define _CRTDBG_MAP_ALLOC
#define WIN32_NO_STATUS
#include <Windows.h>
#include <winternl.h>
#include <intsafe.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <string.h>
#include <crtdbg.h>
#include <winioctl.h>
#include <stdlib.h>
#include "../sioctl.h"

#include "threadpool.h"

#define IOCTL_DEVICE_PATH "\\\\.\\IoctlSample"

typedef struct _APP_STATE
{
    MY_THREAD_POOL* ThreadPool;
    BOOL ThreadPoolStarted;
} APP_STATE;

typedef struct _MY_CONTEXT
{
    SRWLOCK ContextLock;
    UINT32 Number;
} MY_CONTEXT;

static NTSTATUS SendIoctl(
    _In_ DWORD IoctlCode,
    _In_opt_ void* InBuffer,
    _In_ DWORD InBufferSize
)
{
    HANDLE hDevice = CreateFileA(
        IOCTL_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        printf("CreateFile(%s) failed. Error: %lu\n", IOCTL_DEVICE_PATH, err);
        return HRESULT_FROM_WIN32(err);
    }

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDevice,
        IoctlCode,
        InBuffer,
        InBufferSize,
        nullptr,
        0,
        &bytesReturned,
        nullptr);

    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(hDevice);

    if (!ok)
    {
        printf("DeviceIoControl failed. Error: %lu\n", err);
        return HRESULT_FROM_WIN32(err);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS SendIoctlOut(
    _In_ DWORD IoctlCode,
    _In_opt_ void* InBuffer,
    _In_ DWORD InBufferSize,
    _Out_writes_bytes_(OutBufferSize) void* OutBuffer,
    _In_ DWORD OutBufferSize,
    _Out_opt_ DWORD* BytesReturned
)
{
    HANDLE hDevice = CreateFileA(
        IOCTL_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hDevice == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    DWORD br = 0;
    BOOL ok = DeviceIoControl(
        hDevice,
        IoctlCode,
        InBuffer,
        InBufferSize,
        OutBuffer,
        OutBufferSize,
        &br,
        nullptr);

    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(hDevice);

    if (BytesReturned)
        *BytesReturned = br;

    return ok ? STATUS_SUCCESS : HRESULT_FROM_WIN32(err);
}

DWORD WINAPI
SampleWorkRoutine(
    _In_opt_ PVOID Context
)
{
    UINT32 taskId = (UINT32)(UINT_PTR)Context;
    printf("[Thread %lu] Processing task %u\n", GetCurrentThreadId(), taskId);
    Sleep(100);
    return 0;
}

DWORD WINAPI
TestThreadPoolRoutine(
    _In_opt_ PVOID Context
)
{
    MY_CONTEXT* ctx = (MY_CONTEXT*)(Context);
    if (NULL == ctx)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT32 i = 0; i < 1000; ++i)
    {
        AcquireSRWLockExclusive(&ctx->ContextLock);
        ctx->Number++;
        ReleaseSRWLockExclusive(&ctx->ContextLock);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ListProcesses()
{
    HANDLE hSnapshot = INVALID_HANDLE_VALUE;
    PROCESSENTRY32W pe32 = { 0 };
    DWORD processCount = 0;

    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        printf("Failed to create process snapshot. Error: %lu\n", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32))
    {
        printf("Failed to retrieve first process. Error: %lu\n", GetLastError());
        CloseHandle(hSnapshot);
        return STATUS_UNSUCCESSFUL;
    }

    printf("\n%-8s %-8s %-8s %-50s\n", "PID", "Threads", "Parent", "Process Name");
    printf("-----------------------------------------------------------------------\n");

    do
    {
        printf("%-8lu %-8lu %-8lu %-50ws\n",
            pe32.th32ProcessID,
            pe32.cntThreads,
            pe32.th32ParentProcessID,
            pe32.szExeFile);
        processCount++;

    } while (Process32NextW(hSnapshot, &pe32));

    printf("\n");
    printf("Total processes: %d\n\n", processCount);

    CloseHandle(hSnapshot);
    return STATUS_SUCCESS;
}

void
PrintHelp()
{
    printf("\n=== Available Commands ===\n");
    printf("  help        - Display this help message\n");
    printf("  test        - Test the thread pool\n");
    printf("  start       - Start the thread pool with 5 threads\n");
    printf("  stop        - Stop the thread pool\n");
    printf("  work        - Add a work item to the thread pool\n");
    printf("  lp          - List all running processes\n");
    printf("  ioctl1      - Call first driver IOCTL\n");
    printf("  ioctl2      - Call second driver IOCTL\n");
    printf("  ioctl3      - Ask Driver1 to forward an IOCTL to Driver2\n");
    printf("  starttp     - Start kernel thread pool (5 threads)\n");
    printf("  stoptp      - Stop kernel thread pool\n");
    printf("  worktp      - Queue one kernel work item\n");
    printf("  tptests [t] [w] [i] - Run kernel thread-pool tests (defaults: 5 256 1000)\n");
    printf("  protect <PID> - Protect a process from termination\n");
    printf("  unprotect <PID> - Remove protection from a process\n");
    printf("  exit        - Exit the application\n");
    printf("==========================\n\n");
}

NTSTATUS
ExecuteCommand(
    _In_ const char* Command,
    _Inout_ APP_STATE* AppState
)
{
    NTSTATUS status = STATUS_SUCCESS;

    printf("Executing command: %s\n", Command);

    if (strcmp(Command, "help") == 0)
    {
        PrintHelp();
    }
    else if (strcmp(Command, "lp") == 0)
    {
        status = ListProcesses();

        if (!NT_SUCCESS(status))

        {
            printf("Failed to list processes. Status: 0x%08X\n", status);
        }
    }
    else if (strcmp(Command, "ioctl1") == 0)
    {
        status = SendIoctl(IOCTL_FIRST_CALL, nullptr, 0);
        if (!NT_SUCCESS(status))
        {
            printf("ioctl1 failed. Status: 0x%08X\n", status);
        }
    }
    else if (strcmp(Command, "ioctl2") == 0)
    {
        status = SendIoctl(IOCTL_SECOND_CALL, nullptr, 0);
        if (!NT_SUCCESS(status))
        {
            printf("ioctl2 failed. Status: 0x%08X\n", status);
        }
    }
    else if (strcmp(Command, "ioctl3") == 0)
    {
        status = SendIoctl(IOCTL_FORWARD_TO_D2, nullptr, 0);
        if (!NT_SUCCESS(status))
        {
            printf("ioctl3 failed. Status: 0x%08X\n", status);
        }
    }
    else if (strncmp(Command, "starttp", 7) == 0)
    {
        ULONG threads = 5;

        const char* p = Command + 7;
        while (*p == ' ' || *p == '\t')
            ++p;

        if (*p != '\0')
        {
            char* end = nullptr;
            unsigned long val = strtoul(p, &end, 10);
            if (end == p || (*end != '\0' && *end != ' ' && *end != '\t'))
            {
                printf("Invalid syntax. Use: starttp [1..10]\n");
                return STATUS_INVALID_PARAMETER;
            }
            threads = (ULONG)val;
        }

        if (threads < 1 || threads > 10)
        {
            printf("Thread count out of range. Use 1..10.\n");
            return STATUS_INVALID_PARAMETER;
        }

        TP_INIT_INPUT in = { 0 };
        in.ThreadCount = threads;

        status = SendIoctl(IOCTL_TP_INIT, &in, (DWORD)sizeof(in));
        if (!NT_SUCCESS(status))
            printf("starttp failed. Status: 0x%08X\n", status);
        else
            printf("Kernel thread pool started (%lu threads).\n", threads);
    }
    else if (strcmp(Command, "stoptp") == 0)
    {
        status = SendIoctl(IOCTL_TP_STOP, nullptr, 0);
        if (!NT_SUCCESS(status))
            printf("stoptp failed. Status: 0x%08X\n", status);
        else
            printf("Kernel thread pool stopped.\n");
    }
    else if (strcmp(Command, "worktp") == 0)
    {
        static ULONG workId = 0;
        TP_QUEUE_INPUT in = { 0 };
        in.WorkId = workId++;

        status = SendIoctl(IOCTL_TP_QUEUE_WORK, &in, (DWORD)sizeof(in));
        if (!NT_SUCCESS(status))
            printf("worktp failed. Status: 0x%08X\n", status);
        else
            printf("Queued kernel work item %lu.\n", in.WorkId);
    }
    else if (strncmp(Command, "tptests", 7) == 0)
    {
        TP_TEST_INPUT in = { 0 };
        in.ThreadCount = 5;
        in.WorkItemCount = 256;
        in.IterationsPerItem = 1000;

        unsigned long t = 0, w = 0, it = 0;
        int n = sscanf_s(Command + 7, "%lu %lu %lu", &t, &w, &it);
        if (n >= 1) in.ThreadCount = (ULONG)t;
        if (n >= 2) in.WorkItemCount = (ULONG)w;
        if (n >= 3) in.IterationsPerItem = (ULONG)it;

        TP_TEST_OUTPUT out = { 0 };
        DWORD bytes = 0;

        status = SendIoctlOut(
            IOCTL_TP_RUN_TESTS,
            &in,
            (DWORD)sizeof(in),
            &out,
            (DWORD)sizeof(out),
            &bytes);

        if (!NT_SUCCESS(status))
        {
            printf("tptests failed. Status: 0x%08X\n", status);
        }
        else
        {
            printf("TP tests OverallStatus: 0x%08X\n", out.OverallStatus);
            printf("PassedMask=0x%08X FailedMask=0x%08X\n", out.PassedMask, out.FailedMask);
            printf("Threads=%lu WorkItems=%lu Iters=%lu\n",
                out.ThreadCountUsed, out.WorkItemCountUsed, out.IterationsPerItemUsed);
            printf("Processed=%lu Expected=%llu Actual=%llu StopFreed=%lu\n",
                out.ProcessedWorkItems,
                out.ExpectedCounter,
                out.ActualCounter,
                out.StopFreedItems);
        }
    }
    else if (strcmp(Command, "test") == 0)

    {
        MY_THREAD_POOL tp = { 0 };
        MY_CONTEXT ctx = { 0 };

        status = TpInit(&tp, 5);

        if (!NT_SUCCESS(status))
        {
            printf("Failed to initialize thread pool. Status: 0x%08X\n", status);
            return status;
        }

        InitializeSRWLock(&ctx.ContextLock);

        ctx.Number = 0;

        for (int i = 0; i < 100000; ++i)
        {
            status = TpEnqueueWorkItem(&tp, TestThreadPoolRoutine, &ctx);

            if (!NT_SUCCESS(status))
            {
                TpUninit(&tp);
                printf("Failed to enqueue work item. Status: 0x%08X\n", status);
                return status;
            }
        }

        TpUninit(&tp);

        /* If everything went well, this should output 100000000. */
        printf("Final number value = %d \r\n", ctx.Number);
    }
    else if (strcmp(Command, "start") == 0)
    {
        if (AppState->ThreadPoolStarted)
        {
            printf("Thread pool is already running.\n");
        }
        else
        {
            AppState->ThreadPool = (MY_THREAD_POOL*)malloc(sizeof(MY_THREAD_POOL));
            if (NULL == AppState->ThreadPool)
            {
                printf("Failed to allocate memory for thread pool.\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            RtlZeroMemory(AppState->ThreadPool, sizeof(MY_THREAD_POOL));

            status = TpInit(AppState->ThreadPool, 5);
            if (NT_SUCCESS(status))
            {
                AppState->ThreadPoolStarted = TRUE;
                printf("Thread pool started successfully with 5 threads.\n");
            }
            else
            {
                printf("Failed to start thread pool. Status: 0x%08X\n", status);
                free(AppState->ThreadPool);
                AppState->ThreadPool = NULL;
            }
        }
    }
    else if (strcmp(Command, "stop") == 0)
    {
        if (!AppState->ThreadPoolStarted)
        {
            printf("Thread pool is not running.\n");
        }
        else
        {
            printf("Stopping thread pool...\n");
            TpUninit(AppState->ThreadPool);
            free(AppState->ThreadPool);
            AppState->ThreadPool = NULL;
            AppState->ThreadPoolStarted = FALSE;
            printf("Thread pool stopped successfully.\n");
        }
    }
    else if (strcmp(Command, "work") == 0)
    {
        if (!AppState->ThreadPoolStarted)
        {
            printf("Thread pool is not running. Use 'start' command first.\n");
        }
        else
        {
            static UINT32 taskCounter = 0;
            status = TpEnqueueWorkItem(AppState->ThreadPool, SampleWorkRoutine, (PVOID)(UINT_PTR)taskCounter++);
            if (NT_SUCCESS(status))
            {
                printf("Work item enqueued successfully.\n");
            }
            else
            {
                printf("Failed to enqueue work item. Status: 0x%08X\n", status);
            }
        }
    }
    else if (strncmp(Command, "protect ", 8) == 0)
    {
        ULONG pid = (ULONG)atoi(Command + 8);
        if (pid > 0)
        {
            status = SendIoctl(IOCTL_PROTECT_PROCESS, &pid, sizeof(pid));
            if (NT_SUCCESS(status))
            {
                printf("Process %lu protected.\n", pid);
            }
            else
            {
                printf("Failed to protect process %lu. Status: 0x%08X\n", pid, status);
            }
        }
        else
        {
            printf("Invalid PID. Usage: protect <PID>\n");
        }
    }
    else if (strncmp(Command, "unprotect ", 10) == 0)
    {
        ULONG pid = (ULONG)atoi(Command + 10);
        if (pid > 0)
        {
            status = SendIoctl(IOCTL_UNPROTECT_PROCESS, &pid, sizeof(pid));
            if (NT_SUCCESS(status))
            {
                printf("Process %lu unprotected.\n", pid);
            }
            else
            {
                printf("Failed to unprotect process %lu. Status: 0x%08X\n", pid, status);
            }
        }
        else
        {
            printf("Invalid PID. Usage: unprotect <PID>\n");
        }
    }
    else if (strcmp(Command, "exit") == 0)
    {
        printf("Exiting application...\n");
        return STATUS_REQUEST_ABORTED;
    }
    else
    {
        printf("Unknown command: %s\n", Command);
        printf("Type 'help' for available commands.\n");
    }

    return status;
}

BOOL ProtectProcessCommand(HANDLE hDevice, ULONG pid)
{
    PROTECT_PROCESS_INPUT input = { 0 };
    input.ProcessId = pid;

    BOOL result = DeviceIoControl(
        hDevice,
        IOCTL_PROTECT_PROCESS,
        &input,
        sizeof(PROTECT_PROCESS_INPUT),
        NULL,
        0,
        NULL,
        NULL);

    if (result)
    {
        printf("Process %lu is now protected from termination.\n", pid);
    }
    else
    {
        printf("Failed to protect process %lu. Error: %lu\n", pid, GetLastError());
    }

    return result;
}

BOOL UnprotectProcessCommand(HANDLE hDevice, ULONG pid)
{
    PROTECT_PROCESS_INPUT input = { 0 };
    input.ProcessId = pid;

    BOOL result = DeviceIoControl(
        hDevice,
        IOCTL_UNPROTECT_PROCESS,
        &input,
        sizeof(PROTECT_PROCESS_INPUT),
        NULL,
        0,
        NULL,
        NULL);

    if (result)
    {
        printf("Process %lu is no longer protected.\n", pid);
    }
    else
    {
        printf("Failed to unprotect process %lu. Error: %lu\n", pid, GetLastError());
    }

    return result;
}

int main()
{
    char commandBuffer[256];
    APP_STATE appState = { 0 };
    NTSTATUS status = STATUS_SUCCESS;

    printf("=== User Mode Console Application ===\n");
    printf("Type 'help' for available commands.\n\n");

    while (TRUE)
    {
        printf("> ");
        fflush(stdout);

        if (fgets(commandBuffer, sizeof(commandBuffer), stdin) == NULL)
        {
            break;
        }

        size_t len = strlen(commandBuffer);
        if (len > 0 && commandBuffer[len - 1] == '\n')
        {
            commandBuffer[len - 1] = '\0';
        }

        if (strlen(commandBuffer) == 0)
        {
            continue;
        }

        status = ExecuteCommand(commandBuffer, &appState);

        if (status == STATUS_REQUEST_ABORTED)
        {
            break;
        }

        if (strcmp(commandBuffer, "work") == 0 && appState.ThreadPoolStarted)
        {
            Sleep(150);
            printf("\n");
        }
    }

    if (appState.ThreadPoolStarted)
    {
        printf("Cleaning up thread pool...\n");
        TpUninit(appState.ThreadPool);
        free(appState.ThreadPool);
    }

    printf("Application terminated.\n");
    _CrtDumpMemoryLeaks();
    return 0;
}