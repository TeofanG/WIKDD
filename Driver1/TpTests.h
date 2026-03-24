#pragma once
#include <ntddk.h>
#include "../sioctl.h"

#define TP_TEST_WID_BASE 0x80000000UL

BOOLEAN TpIsTestWorkId(_In_ ULONG WorkId);

VOID TpTestWorkRoutine(_In_ ULONG WorkId);

NTSTATUS TpRunAllTests(
    _In_ ULONG MaxThreads,
    _Inout_ VOID* ThreadPool,
    _In_ NTSTATUS(*TpInitFn)(_Inout_ VOID* Tp, _In_ ULONG ThreadCount),
    _In_ NTSTATUS(*TpQueueFn)(_Inout_ VOID* Tp, _In_ ULONG WorkId),
    _In_ NTSTATUS(*TpStopFn)(_Inout_ VOID* Tp),
    _In_opt_ const TP_TEST_INPUT* In,
    _Out_ TP_TEST_OUTPUT* Out
);
