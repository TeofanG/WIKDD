#include "TpTests.h"

typedef struct _TP_TEST_CTX
{
    KSPIN_LOCK CounterLock;
    ULONGLONG Counter;

    ULONG IterationsPerItem;
    volatile LONG ProcessedItems;
} TP_TEST_CTX, * PTP_TEST_CTX;

static TP_TEST_CTX g_TestCtx;

BOOLEAN TpIsTestWorkId(_In_ ULONG WorkId)
{
    return (WorkId & TP_TEST_WID_BASE) != 0;
}

VOID TpTestWorkRoutine(_In_ ULONG WorkId)
{
    UNREFERENCED_PARAMETER(WorkId);

    for (ULONG i = 0; i < g_TestCtx.IterationsPerItem; i++)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_TestCtx.CounterLock, &oldIrql);
        g_TestCtx.Counter++;
        KeReleaseSpinLock(&g_TestCtx.CounterLock, oldIrql);
    }

    InterlockedIncrement(&g_TestCtx.ProcessedItems);
}

static VOID TestCtxInit(_In_ ULONG IterationsPerItem)
{
    RtlZeroMemory(&g_TestCtx, sizeof(g_TestCtx));
    KeInitializeSpinLock(&g_TestCtx.CounterLock);
    g_TestCtx.IterationsPerItem = IterationsPerItem;
}

NTSTATUS TpRunAllTests(
    _In_ ULONG MaxThreads,
    _Inout_ VOID* ThreadPool,
    _In_ NTSTATUS(*TpInitFn)(_Inout_ VOID* Tp, _In_ ULONG ThreadCount),
    _In_ NTSTATUS(*TpQueueFn)(_Inout_ VOID* Tp, _In_ ULONG WorkId),
    _In_ NTSTATUS(*TpStopFn)(_Inout_ VOID* Tp),
    _In_opt_ const TP_TEST_INPUT* In,
    _Out_ TP_TEST_OUTPUT* Out
)
{
    if (!ThreadPool || !TpInitFn || !TpQueueFn || !TpStopFn || !Out)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Size = sizeof(*Out);

    ULONG threadCount = (In && In->ThreadCount) ? In->ThreadCount : 5;
    ULONG workCount = (In && In->WorkItemCount) ? In->WorkItemCount : 256;
    ULONG iters = (In && In->IterationsPerItem) ? In->IterationsPerItem : 1000;

    if (threadCount == 0 || threadCount > MaxThreads)
        return STATUS_INVALID_PARAMETER;
    if (workCount == 0 || iters == 0)
        return STATUS_INVALID_PARAMETER;

    Out->ThreadCountUsed = threadCount;
    Out->WorkItemCountUsed = workCount;
    Out->IterationsPerItemUsed = iters;

    ULONG pass = 0, fail = 0;

    // Test 0: init validation
    {
        (VOID)TpStopFn(ThreadPool);

        NTSTATUS st0 = TpInitFn(ThreadPool, 0);
        if (st0 == STATUS_INVALID_PARAMETER) pass |= (1u << 0); else fail |= (1u << 0);

        NTSTATUS st1 = TpInitFn(ThreadPool, MaxThreads + 1);
        if (st1 == STATUS_INVALID_PARAMETER) pass |= (1u << 0); else fail |= (1u << 0);
    }

    // Test 1: init/stop
    {
        NTSTATUS st = TpInitFn(ThreadPool, threadCount);
        if (!NT_SUCCESS(st))
        {
            fail |= (1u << 1);
            Out->OverallStatus = st;
            Out->PassedMask = pass;
            Out->FailedMask = fail;
            return st;
        }

        st = TpStopFn(ThreadPool);
        if (NT_SUCCESS(st)) pass |= (1u << 1); else fail |= (1u << 1);
    }

    // Test 3: concurrent execution + synchronization
    TestCtxInit(iters);

    {
        NTSTATUS st = TpInitFn(ThreadPool, threadCount);
        if (!NT_SUCCESS(st))
        {
            fail |= (1u << 2) | (1u << 3);
        }
        else
        {
            for (ULONG i = 0; i < workCount; i++)
            {
                ULONG wid = TP_TEST_WID_BASE | i;
                st = TpQueueFn(ThreadPool, wid);
                if (!NT_SUCCESS(st))
                {
                    fail |= (1u << 2) | (1u << 3);
                    break;
                }
            }

            LARGE_INTEGER interval;
            interval.QuadPart = -10 * 1000 * 1000;

            for (ULONG t = 0; t < 10; t++)
            {
                if ((ULONG)g_TestCtx.ProcessedItems >= workCount)
                    break;
                KeDelayExecutionThread(KernelMode, FALSE, &interval);
            }

            Out->ProcessedWorkItems = (ULONG)g_TestCtx.ProcessedItems;
            Out->ExpectedCounter = (ULONGLONG)workCount * (ULONGLONG)iters;
            Out->ActualCounter = g_TestCtx.Counter;

            if (Out->ProcessedWorkItems == workCount) pass |= (1u << 3); else fail |= (1u << 3);
            if (Out->ActualCounter == Out->ExpectedCounter) pass |= (1u << 2); else fail |= (1u << 2);

            (VOID)TpStopFn(ThreadPool);
        }
    }

    // test 4
    TestCtxInit(iters);

    {
        NTSTATUS st = TpInitFn(ThreadPool, threadCount);
        if (!NT_SUCCESS(st))
        {
            fail |= (1u << 4);
        }
        else
        {
            ULONG bigCount = workCount * 8;
            for (ULONG i = 0; i < bigCount; i++)
            {
                ULONG wid = TP_TEST_WID_BASE | i;
                (VOID)TpQueueFn(ThreadPool, wid);
            }

            st = TpStopFn(ThreadPool);
            if (!NT_SUCCESS(st))
            {
                fail |= (1u << 4);
            }
            else
            {
                st = TpInitFn(ThreadPool, threadCount);
                if (!NT_SUCCESS(st)) fail |= (1u << 4);
                else
                {
                    (VOID)TpStopFn(ThreadPool);
                    pass |= (1u << 4);
                }
            }
        }
    }

    Out->PassedMask = pass;
    Out->FailedMask = fail;
    Out->OverallStatus = (fail == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    return Out->OverallStatus;
}
