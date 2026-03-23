#define _CRTDBG_MAP_ALLOC
#define WIN32_NO_STATUS
#include <Windows.h>
#include <winternl.h>
#include <intsafe.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include <stdio.h>
#include <assert.h>
#include <crtdbg.h>


//
// **********************************************************
// *                        LIST API                        *
// **********************************************************
//
void
ListInitializeHead(
    _Inout_ PLIST_ENTRY ListHead
)
{
    /*
     *  ListHead->Flink represent the "front" link or next element.
     *  ListHead->Blink represent the "back" link or previous element.
     *
     *  At first, the list is initialized as being recursive, that's it:
     *      both front link and back link points to the head.
     *
     *      /--BLINK-- [ List Head ] --FLINK--\
     *      |            ^      ^             |
     *      \------------|      |-------------/
     *
     */
    ListHead->Flink = ListHead;
    ListHead->Blink = ListHead;
}

bool
ListIsEmpty(
    _In_ _Const_ const PLIST_ENTRY ListHead
)
{
    /*
     * An empty list is the one where both flink and blink points to the head.
     * See the diagarm in InitializeListHead to understand better.
     */
    return (ListHead->Flink == ListHead) &&
        (ListHead->Blink == ListHead);
}

void
ListInsertHead(
    _Inout_ PLIST_ENTRY ListHead,
    _Inout_ PLIST_ENTRY Element
)
{
    /* List must remain recursive. */
    assert(ListHead->Flink->Blink == ListHead);
    assert(ListHead->Blink->Flink == ListHead);

    /*
     * This:
     *  [blink]--[ListHead]--[flink]
     *
     * Becomes:
     *  [blink]--[ListHead]--[element]--[flink]
     *
     * ListHead is actually a sentinel node used to access the actual head and tail.
     */

     /* flink is actually the head of the list */
    PLIST_ENTRY actualHead = ListHead->Flink;

    /*
     * Actual insertion:
     *  1. [ListHead] <--BLINK-- [Element] --FLINK--> [flink]
     *  2. [ListHead] --FLINK--> [Element] <--BLINK-- [flink]
     */
    Element->Flink = ListHead->Flink;
    Element->Blink = ListHead;

    actualHead->Blink = Element;
    ListHead->Flink = Element;
}

PLIST_ENTRY
ListRemoveTail(
    _Inout_ PLIST_ENTRY ListHead
)
{
    /* List must remain recursive. */
    assert(ListHead->Flink->Blink == ListHead);
    assert(ListHead->Blink->Flink == ListHead);

    /* No element in list. Bail. */
    if (ListIsEmpty(ListHead))
    {
        return nullptr;
    }

    /* Blink is actually the tail of the list. */
    PLIST_ENTRY elementToRemove = ListHead->Blink;

    /* [prevElement] -- [elementToRemove] -- [nextElement] */
    PLIST_ENTRY prevElement = elementToRemove->Blink;
    PLIST_ENTRY nextElement = elementToRemove->Flink;

    /*
     * Detach the element:
     *  [prevElement] --FLINK--> [nextElement]
     *                <--BLINK--
     */
    prevElement->Flink = nextElement;
    nextElement->Blink = prevElement;

    /* Make it a recursive element before returning to caller. Don't expose valid list pointers.*/
    elementToRemove->Blink = elementToRemove;
    elementToRemove->Flink = elementToRemove;

    return elementToRemove;
}

//
// **********************************************************
// *                        TP API                          *
// **********************************************************
//

//
// MY_THREAD_POOL - simple thread pool implementation
//
typedef struct _MY_THREAD_POOL
{
    /* When this event is signaled The threads should stop. */
    HANDLE StopThreadPoolEvent;
    /* This event is used to signal that the threads have work to perform. */
    HANDLE WorkScheduledEvent;
    /* Number of threads in the ThreadHandles array. */
    UINT32 NumberOfThreads;
    /* List of threads started in thread pool. */
    HANDLE* ThreadHandles;
    /* The list of work items and the mutex  protecting. */
    SRWLOCK QueueLock;
    /* Enqueued work items - represented as a double linked list. */
    LIST_ENTRY Queue;
} MY_THREAD_POOL;

//
// MY_WORK_ITEM - A very basic work item
//
typedef struct _MY_WORK_ITEM
{
    /* Required by the MY_THREAD_POOL, so it can be enqueued and dequeued. */
    LIST_ENTRY ListEntry;
    /* Callback to be called. */
    LPTHREAD_START_ROUTINE WorkRoutine;
    /* Caller defined context. To be passed to work routine. */
    PVOID Context;
} MY_WORK_ITEM;

DWORD WINAPI
TpRoutine(
    _In_opt_ PVOID Context
)
{
    MY_THREAD_POOL* threadPool = (MY_THREAD_POOL*)(Context);
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (NULL == threadPool)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Wait for work or for stop event. */
    HANDLE waitEvents[2] = { threadPool->StopThreadPoolEvent,
                             threadPool->WorkScheduledEvent };
    while (true)
    {
        /* This will waits for any of the two events. */
        DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitEvents),
            waitEvents,
            FALSE,
            INFINITE);
        /* StopThreadPoolEvent signaled. Graceful exit. */
        if (WAIT_OBJECT_0 == waitResult)
        {
            status = STATUS_SUCCESS;
            break;
        }
        /* WorkScheduledEvent signaled. We start the processing loop. */
        else if (WAIT_OBJECT_0 + 1 == waitResult)
        {
            /* Processing loop. Keep going until we empty the work queue. */
            while (true)
            {
                LIST_ENTRY* listTail = NULL;
                MY_WORK_ITEM* workItem = NULL;

                /*todo: Pop one element from list. This must be done with lock taken. */
                AcquireSRWLockExclusive(&threadPool->QueueLock);
                listTail = ListRemoveTail(&threadPool->Queue);
                if (NULL != listTail)
                {
                    workItem = CONTAINING_RECORD(listTail, MY_WORK_ITEM, ListEntry);
                }
                ReleaseSRWLockExclusive(&threadPool->QueueLock);

                /* If we have an item, we invoke the work routine it and then we free it. */
                if (NULL != workItem)
                {
                    /*todo: Call work routine. */
                    workItem->WorkRoutine(workItem->Context);
                    /*todo: Ensure memory management - free the work item. */
                    free(workItem);
                }

                /* If we didn't managed to get a work item, we stop the processing loop. */
                if (NULL == workItem)
                {
                    break;
                }
            }
        }
        /* An error occured. We stop the thread. */
        else
        {
            status = STATUS_THREADPOOL_HANDLE_EXCEPTION;
            break;
        }
    }

    return status;
}

void
TpUninit(
    _Inout_ MY_THREAD_POOL* ThreadPool
)
{
    /* Nothing to do. */
    if (NULL == ThreadPool)
    {
        return;
    }

    /* First set the stop event, if any. */
    if (NULL != ThreadPool->StopThreadPoolEvent)
    {
        /*todo: Signal stop event. */
        SetEvent(ThreadPool->StopThreadPoolEvent);
    }

    /* Now wait for threads. */
    if (NULL != ThreadPool->ThreadHandles && ThreadPool->NumberOfThreads > 0)
    {
        for (UINT32 i = 0; i < ThreadPool->NumberOfThreads; ++i)
        {

            if (NULL != ThreadPool->ThreadHandles[i])
            {
                /* Wait for thread. */
                WaitForSingleObject(ThreadPool->ThreadHandles[i], INFINITE);
                /* Close thread handle. */
                CloseHandle(ThreadPool->ThreadHandles[i]);
            }
        }
        free(ThreadPool->ThreadHandles);
    }
    ThreadPool->ThreadHandles = NULL;
    ThreadPool->NumberOfThreads = 0;

    /*todo: Empty the work queue and process the rest of the work items - take care of synchronization! */
    AcquireSRWLockExclusive(&ThreadPool->QueueLock);
    while (!ListIsEmpty(&ThreadPool->Queue))
    {
        LIST_ENTRY* listTail = ListRemoveTail(&ThreadPool->Queue);
        if (NULL != listTail)
        {
            MY_WORK_ITEM* workItem = CONTAINING_RECORD(listTail, MY_WORK_ITEM, ListEntry);
            /* Free remaining work items without executing them. */
            free(workItem);
        }
    }
    ReleaseSRWLockExclusive(&ThreadPool->QueueLock);

    /* And close the event handles. */
    if (NULL != ThreadPool->StopThreadPoolEvent)
    {
        CloseHandle(ThreadPool->StopThreadPoolEvent);
        ThreadPool->StopThreadPoolEvent = NULL;
    }
    if (NULL != ThreadPool->WorkScheduledEvent)
    {
        CloseHandle(ThreadPool->WorkScheduledEvent);
        ThreadPool->WorkScheduledEvent = NULL;
    }
}

NTSTATUS
TpInit(
    _Inout_ MY_THREAD_POOL* ThreadPool,
    _In_ UINT32 NumberOfThreads
)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    HRESULT hRes = 0;
    UINT32 requiredSizeForThreads = 0;

    /* Sanity checks for parameters. */
    if (NULL == ThreadPool || 0 == NumberOfThreads)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Preinit Threadpool with zeroes. */
    RtlZeroMemory(ThreadPool, sizeof(MY_THREAD_POOL));

    /* Initialize the work queue. */
    ListInitializeHead(&ThreadPool->Queue);
    InitializeSRWLock(&ThreadPool->QueueLock);

    /* Initialize stop event - once set, this will remain signaled as it needs to notify all threads. */
    ThreadPool->StopThreadPoolEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (NULL == ThreadPool->StopThreadPoolEvent)
    {
        status = STATUS_INVALID_HANDLE;
        goto CleanUp;
    }

    /* Initialize work notified event - this is an auto reset event as it needs to wake one thread at a time. */
    ThreadPool->WorkScheduledEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (NULL == ThreadPool->WorkScheduledEvent)
    {
        status = STATUS_INVALID_HANDLE;
        goto CleanUp;
    }

    /* Now allocate space for the threads. */
    hRes = UInt32Mult(sizeof(HANDLE), NumberOfThreads, &requiredSizeForThreads);
    if (!SUCCEEDED(hRes))
    {
        status = STATUS_INTEGER_OVERFLOW;
        goto CleanUp;
    }
    ThreadPool->ThreadHandles = (HANDLE*)malloc(requiredSizeForThreads);
    if (NULL == ThreadPool->ThreadHandles)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto CleanUp;
    }
    RtlZeroMemory(ThreadPool->ThreadHandles, requiredSizeForThreads);

    /* And finally create the actual threads - they will start executing TpRoutine. */
    for (UINT32 i = 0; i < NumberOfThreads; ++i)
    {
        /*todo: Create a single thread with TpRoutine as work and pass ThreadPool as context. */
        ThreadPool->ThreadHandles[i] = CreateThread(NULL, 0, TpRoutine, ThreadPool, 0, NULL);

        if (NULL == ThreadPool->ThreadHandles[i])
        {
            status = STATUS_INVALID_HANDLE;
            goto CleanUp;
        }
        /* We created one more thread successfully. */
        ThreadPool->NumberOfThreads++;
    }

    /* All good. */
    status = STATUS_SUCCESS;

CleanUp:
    if (!NT_SUCCESS(status))
    {
        /* Destroy TP on failure. */
        TpUninit(ThreadPool);
    }
    return status;
}

NTSTATUS
TpEnqueueWorkItem(
    _Inout_ MY_THREAD_POOL* ThreadPool,
    _In_ LPTHREAD_START_ROUTINE WorkRoutine,
    _In_opt_ PVOID Context
)
{
    /* Allocate space for the work item. Will be freed when item is processed. */
    MY_WORK_ITEM* item = (MY_WORK_ITEM*)malloc(sizeof(MY_WORK_ITEM));
    if (NULL == item)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(item, sizeof(MY_WORK_ITEM));

    /* Assign fields. */
    item->Context = Context;
    item->WorkRoutine = WorkRoutine;

    /* todo: Insert into threadpool. This must be done with lock taken. */
    AcquireSRWLockExclusive(&ThreadPool->QueueLock);
    ListInsertHead(&ThreadPool->Queue, &item->ListEntry);
    ReleaseSRWLockExclusive(&ThreadPool->QueueLock);

    /*todo: Notify thread pool WorkScheduledEvent that a new item is available. */
    SetEvent(ThreadPool->WorkScheduledEvent);

    /* All good. */
    return STATUS_SUCCESS;
}