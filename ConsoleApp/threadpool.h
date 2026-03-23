#ifndef THREADPOOL_H
#define THREADPOOL_H

#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

void ListInitializeHead(_Inout_ PLIST_ENTRY ListHead);
bool ListIsEmpty(_In_ _Const_ const PLIST_ENTRY ListHead);
void ListInsertHead(_Inout_ PLIST_ENTRY ListHead, _Inout_ PLIST_ENTRY Element);
PLIST_ENTRY ListRemoveTail(_Inout_ PLIST_ENTRY ListHead);

typedef struct _MY_THREAD_POOL
{
    HANDLE StopThreadPoolEvent;
    HANDLE WorkScheduledEvent;
    UINT32 NumberOfThreads;
    HANDLE* ThreadHandles;
    SRWLOCK QueueLock;
    LIST_ENTRY Queue;
} MY_THREAD_POOL;

typedef struct _MY_WORK_ITEM
{
    LIST_ENTRY ListEntry;
    LPTHREAD_START_ROUTINE WorkRoutine;
    PVOID Context;
} MY_WORK_ITEM;


NTSTATUS TpInit(_Inout_ MY_THREAD_POOL* ThreadPool, _In_ UINT32 NumberOfThreads);
void TpUninit(_Inout_ MY_THREAD_POOL* ThreadPool);
NTSTATUS TpEnqueueWorkItem(_Inout_ MY_THREAD_POOL* ThreadPool, _In_ LPTHREAD_START_ROUTINE WorkRoutine, _In_opt_ PVOID Context);

#endif 