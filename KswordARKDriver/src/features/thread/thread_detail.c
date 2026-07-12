/*++

Module Name:

    thread_detail.c

Abstract:

    Read-only PDB/DynData-backed thread runtime detail query.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "..\process\process_crossview.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>

typedef struct _KSWORD_ARK_DETAIL_CLIENT_ID_LOCAL
{
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} KSWORD_ARK_DETAIL_CLIENT_ID_LOCAL, *PKSWORD_ARK_DETAIL_CLIENT_ID_LOCAL;

NTSYSAPI
NTSTATUS
NTAPI
PsLookupThreadByThreadId(
    _In_ HANDLE ThreadId,
    _Outptr_ PETHREAD* Thread
    );

NTSYSAPI
PEPROCESS
NTAPI
PsGetThreadProcess(
    _In_ PETHREAD Thread
    );

static BOOLEAN
KswordARKThreadDetailOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    判断一个 ETHREAD/KTHREAD 偏移是否可用于详情采样。

Arguments:

    Offset - 来自 DynData/PDB 的字段偏移。

Return Value:

    TRUE 表示偏移可用；FALSE 表示缺失或 sentinel。

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static ULONG
KswordARKThreadDetailProtocolOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    将驱动内部 offset sentinel 转成 shared 协议 sentinel。

Arguments:

    Offset - 原始 DynData/PDB 偏移。

Return Value:

    可用 offset 或 KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE。

--*/
{
    if (!KswordARKThreadDetailOffsetPresent(Offset)) {
        return KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
    }

    return Offset;
}

static VOID
KswordARKThreadDetailFillOffsets(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_THREAD_DETAIL_OFFSETS* Offsets
    )
/*++

Routine Description:

    把当前线程相关 DynData 偏移复制到响应包。

Arguments:

    DynState - 当前 DynData 快照。
    Offsets - shared 响应内的 offset 子结构。

Return Value:

    None.

--*/
{
    RtlZeroMemory(Offsets, sizeof(*Offsets));
    Offsets->etCid = KswordARKThreadDetailProtocolOffset(DynState->Kernel.EtCid);
    Offsets->etThreadListEntry = KswordARKThreadDetailProtocolOffset(DynState->Kernel.EtThreadListEntry);
    Offsets->etStartAddress = KswordARKThreadDetailProtocolOffset(DynState->Kernel.EtStartAddress);
    Offsets->etWin32StartAddress = KswordARKThreadDetailProtocolOffset(DynState->Kernel.EtWin32StartAddress);
    Offsets->ktProcess = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtProcess);
    Offsets->ktInitialStack = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtInitialStack);
    Offsets->ktStackLimit = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtStackLimit);
    Offsets->ktStackBase = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtStackBase);
    Offsets->ktKernelStack = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtKernelStack);
    Offsets->ktReadOperationCount = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtReadOperationCount);
    Offsets->ktWriteOperationCount = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtWriteOperationCount);
    Offsets->ktOtherOperationCount = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtOtherOperationCount);
    Offsets->ktReadTransferCount = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtReadTransferCount);
    Offsets->ktWriteTransferCount = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtWriteTransferCount);
    Offsets->ktOtherTransferCount = KswordARKThreadDetailProtocolOffset(DynState->Kernel.KtOtherTransferCount);
}

static BOOLEAN
KswordARKThreadDetailSourcePresent(
    _In_ ULONG Source
    )
/*++

Routine Description:

    判断线程 detail 字段来源是否可展示。中文说明：unavailable 来源只表示
    当前 profile 未提供该字段，不能作为证据行输出。

Arguments:

    Source - KSW_DYN_FIELD_SOURCE_* 来源值。

Return Value:

    TRUE 表示来源有效，FALSE 表示来源缺失。

--*/
{
    return (Source != KSW_DYN_FIELD_SOURCE_UNAVAILABLE) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKThreadDetailFillSources(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_THREAD_DETAIL_SOURCES* Sources
    )
/*++

Routine Description:

    把 ETHREAD/KTHREAD detail 偏移来源复制到 shared 响应。

Arguments:

    DynState - 当前 DynData 快照。
    Sources - shared 响应内的线程 detail 来源子结构。

Return Value:

    TRUE 表示至少一个来源可展示，FALSE 表示全部缺失。

--*/
{
    BOOLEAN anySourcePresent = FALSE;

    RtlZeroMemory(Sources, sizeof(*Sources));
    Sources->etCid = DynState->KernelSources.EtCid;
    Sources->etThreadListEntry = DynState->KernelSources.EtThreadListEntry;
    Sources->etStartAddress = DynState->KernelSources.EtStartAddress;
    Sources->etWin32StartAddress = DynState->KernelSources.EtWin32StartAddress;
    Sources->ktProcess = DynState->KernelSources.KtProcess;
    Sources->ktInitialStack = DynState->KernelSources.KtInitialStack;
    Sources->ktStackLimit = DynState->KernelSources.KtStackLimit;
    Sources->ktStackBase = DynState->KernelSources.KtStackBase;
    Sources->ktKernelStack = DynState->KernelSources.KtKernelStack;
    Sources->ktReadOperationCount = DynState->KernelSources.KtReadOperationCount;
    Sources->ktWriteOperationCount = DynState->KernelSources.KtWriteOperationCount;
    Sources->ktOtherOperationCount = DynState->KernelSources.KtOtherOperationCount;
    Sources->ktReadTransferCount = DynState->KernelSources.KtReadTransferCount;
    Sources->ktWriteTransferCount = DynState->KernelSources.KtWriteTransferCount;
    Sources->ktOtherTransferCount = DynState->KernelSources.KtOtherTransferCount;

    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->etCid) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->etThreadListEntry) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->etStartAddress) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->etWin32StartAddress) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktProcess) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktInitialStack) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktStackLimit) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktStackBase) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktKernelStack) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktReadOperationCount) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktWriteOperationCount) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktOtherOperationCount) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktReadTransferCount) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktWriteTransferCount) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKThreadDetailSourcePresent(Sources->ktOtherTransferCount) ? TRUE : anySourcePresent;

    return anySourcePresent;
}

static ULONG64
KswordARKThreadDetailKernelGlobalAddress(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONG Rva
    )
/*++

Routine Description:

    按当前 ntoskrnl imageBase 把 PDB profile 全局 RVA 转成 VA。

Arguments:

    DynState - 当前 DynData 快照。
    Rva - 全局符号 RVA。

Return Value:

    当前内核地址；缺失时返回 0。

--*/
{
    if (!DynState->NtosActive || DynState->Ntoskrnl.imageBase == 0ULL) {
        return 0ULL;
    }
    if (!KswordARKThreadDetailOffsetPresent(Rva)) {
        return 0ULL;
    }

    return DynState->Ntoskrnl.imageBase + (ULONG64)Rva;
}

static BOOLEAN
KswordARKThreadDetailFillKernelGlobals(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_RUNTIME_KERNEL_GLOBALS* Globals
    )
/*++

Routine Description:

    复制线程详情页也会展示的 ntoskrnl 全局 RVA 证据包。

Arguments:

    DynState - 当前 DynData 快照。
    Globals - shared 响应内的全局 RVA/来源/VA 包。

Return Value:

    TRUE 表示至少一个全局 RVA 可展示，FALSE 表示全部缺失。

--*/
{
    BOOLEAN anyGlobalPresent = FALSE;

    RtlZeroMemory(Globals, sizeof(*Globals));
    Globals->pspCidTableRva = KswordARKThreadDetailProtocolOffset(DynState->KernelGlobals.PspCidTable);
    Globals->psLoadedModuleListRva = KswordARKThreadDetailProtocolOffset(DynState->KernelGlobals.PsLoadedModuleList);
    Globals->mmUnloadedDriversRva = KswordARKThreadDetailProtocolOffset(DynState->KernelGlobals.MmUnloadedDrivers);
    Globals->piDdbCacheTableRva = KswordARKThreadDetailProtocolOffset(DynState->KernelGlobals.PiDDBCacheTable);
    Globals->keServiceDescriptorTableShadowRva = KswordARKThreadDetailProtocolOffset(DynState->KernelGlobals.KeServiceDescriptorTableShadow);
    Globals->mmLastUnloadedDriverRva = KswordARKThreadDetailProtocolOffset(DynState->KernelGlobals.MmLastUnloadedDriver);
    Globals->pspCidTableSource = DynState->KernelGlobalSources.PspCidTable;
    Globals->psLoadedModuleListSource = DynState->KernelGlobalSources.PsLoadedModuleList;
    Globals->mmUnloadedDriversSource = DynState->KernelGlobalSources.MmUnloadedDrivers;
    Globals->piDdbCacheTableSource = DynState->KernelGlobalSources.PiDDBCacheTable;
    Globals->keServiceDescriptorTableShadowSource = DynState->KernelGlobalSources.KeServiceDescriptorTableShadow;
    Globals->mmLastUnloadedDriverSource = DynState->KernelGlobalSources.MmLastUnloadedDriver;
    Globals->pspCidTableAddress = KswordARKThreadDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.PspCidTable);
    Globals->psLoadedModuleListAddress = KswordARKThreadDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.PsLoadedModuleList);
    Globals->mmUnloadedDriversAddress = KswordARKThreadDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.MmUnloadedDrivers);
    Globals->piDdbCacheTableAddress = KswordARKThreadDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.PiDDBCacheTable);
    Globals->keServiceDescriptorTableShadowAddress = KswordARKThreadDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.KeServiceDescriptorTableShadow);
    Globals->mmLastUnloadedDriverAddress = KswordARKThreadDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.MmLastUnloadedDriver);

    anyGlobalPresent = KswordARKThreadDetailOffsetPresent(DynState->KernelGlobals.PspCidTable) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKThreadDetailOffsetPresent(DynState->KernelGlobals.PsLoadedModuleList) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKThreadDetailOffsetPresent(DynState->KernelGlobals.MmUnloadedDrivers) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKThreadDetailOffsetPresent(DynState->KernelGlobals.PiDDBCacheTable) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKThreadDetailOffsetPresent(DynState->KernelGlobals.KeServiceDescriptorTableShadow) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKThreadDetailOffsetPresent(DynState->KernelGlobals.MmLastUnloadedDriver) ? TRUE : anyGlobalPresent;

    return anyGlobalPresent;
}

static NTSTATUS
KswordARKThreadDetailReadClientId(
    _In_ const VOID* ThreadObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* ProcessIdOut,
    _Out_ ULONG64* ThreadIdOut
    )
/*++

Routine Description:

    读取 ETHREAD.Cid，供 UI 对照 PsGetThreadId/PsGetThreadProcessId。

Arguments:

    ThreadObject - ETHREAD 对象。
    Offset - ETHREAD.Cid 偏移。
    ProcessIdOut - 输出 UniqueProcess。
    ThreadIdOut - 输出 UniqueThread。

Return Value:

    STATUS_SUCCESS 或安全读失败状态。

--*/
{
    KSWORD_ARK_DETAIL_CLIENT_ID_LOCAL cid;
    NTSTATUS status;

    if (ProcessIdOut == NULL || ThreadIdOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKThreadDetailOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    RtlZeroMemory(&cid, sizeof(cid));
    status = KswordARKCrossViewReadMemory((const UCHAR*)ThreadObject + Offset, &cid, sizeof(cid));
    if (NT_SUCCESS(status)) {
        *ProcessIdOut = (ULONG64)(ULONG_PTR)cid.UniqueProcess;
        *ThreadIdOut = (ULONG64)(ULONG_PTR)cid.UniqueThread;
    }

    return status;
}

static NTSTATUS
KswordARKThreadDetailReadListEntry(
    _In_ const VOID* ThreadObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* FlinkOut,
    _Out_ ULONG64* BlinkOut
    )
/*++

Routine Description:

    读取 ETHREAD.ThreadListEntry 链表指针。

Arguments:

    ThreadObject - ETHREAD 对象。
    Offset - ThreadListEntry 偏移。
    FlinkOut - 输出 Flink 地址。
    BlinkOut - 输出 Blink 地址。

Return Value:

    STATUS_SUCCESS 或安全读失败状态。

--*/
{
    LIST_ENTRY listEntry;
    NTSTATUS status;

    if (FlinkOut == NULL || BlinkOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKThreadDetailOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    RtlZeroMemory(&listEntry, sizeof(listEntry));
    status = KswordARKCrossViewReadMemory((const UCHAR*)ThreadObject + Offset, &listEntry, sizeof(listEntry));
    if (NT_SUCCESS(status)) {
        *FlinkOut = (ULONG64)(ULONG_PTR)listEntry.Flink;
        *BlinkOut = (ULONG64)(ULONG_PTR)listEntry.Blink;
    }

    return status;
}

static NTSTATUS
KswordARKThreadDetailReadUlong64Field(
    _In_ const VOID* ThreadObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    读取 ETHREAD/KTHREAD 中一个 64 位计数或地址字段。

Arguments:

    ThreadObject - ETHREAD 对象。
    Offset - 字段偏移。
    ValueOut - 输出字段值。

Return Value:

    STATUS_SUCCESS 或安全读失败状态。

--*/
{
    if (ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKThreadDetailOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return KswordARKCrossViewReadUlong64Address((const UCHAR*)ThreadObject + Offset, ValueOut);
}

static VOID
KswordARKThreadDetailNoteFailure(
    _In_ NTSTATUS ReadStatus,
    _Inout_ ULONG* FailureCount,
    _Inout_ NTSTATUS* LastStatus
    )
/*++

Routine Description:

    记录一次字段读取失败，最终折叠为 PARTIAL/CAPABILITY_MISSING 状态。

Arguments:

    ReadStatus - 单字段读取状态。
    FailureCount - 失败计数。
    LastStatus - 最近失败状态。

Return Value:

    None.

--*/
{
    if (!NT_SUCCESS(ReadStatus)) {
        ++(*FailureCount);
        *LastStatus = ReadStatus;
    }
}

NTSTATUS
KswordARKDriverQueryThreadDetail(
    _Out_writes_bytes_(OutputBufferLength) KSWORD_ARK_THREAD_DETAIL_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_THREAD_DETAIL_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询单个线程的只读运行时详情。中文说明：R0 只按 TID 引用 ETHREAD，
    然后按已应用 PDB/DynData 偏移读取字段，不执行挂起、终止或链表修复。

Arguments:

    Response - METHOD_BUFFERED 输出响应。
    OutputBufferLength - 输出缓冲区长度。
    Request - 固定请求。
    BytesWrittenOut - 返回响应大小。

Return Value:

    STATUS_SUCCESS 表示响应包有效；字段级缺失写入 status/detail。

--*/
{
    KSW_DYN_STATE dynState;
    PETHREAD threadObject = NULL;
    PVOID processObject = NULL;
    ULONG requestFlags = KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_ALL;
    ULONG failureCount = 0UL;
    ULONG missingRequired = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS lastStatus = STATUS_SUCCESS;

    if (Response == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < sizeof(*Response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    *BytesWrittenOut = sizeof(*Response);
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RUNTIME_DETAIL_PROTOCOL_VERSION;
    Response->status = KSWORD_ARK_DETAIL_STATUS_UNKNOWN;
    Response->threadId = Request->threadId;
    Response->processId = Request->processId;
    Response->requestedFlags = Request->flags;

    if (Request->version != KSWORD_ARK_RUNTIME_DETAIL_PROTOCOL_VERSION) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_UNSUPPORTED;
        Response->lastStatus = STATUS_REVISION_MISMATCH;
        (VOID)RtlStringCchCopyW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"Thread detail request version mismatch.");
        return STATUS_SUCCESS;
    }

    if (Request->threadId == 0UL) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_LOOKUP_FAILED;
        Response->lastStatus = STATUS_INVALID_PARAMETER;
        (VOID)RtlStringCchCopyW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"Thread detail requires a non-zero TID.");
        return STATUS_SUCCESS;
    }

    if (Request->flags != 0UL) {
        requestFlags = Request->flags;
    }

    KswordARKDynDataSnapshot(&dynState);
    Response->dynDataCapabilityMask = dynState.CapabilityMask;
    KswordARKThreadDetailFillOffsets(&dynState, &Response->offsets);
    if (KswordARKThreadDetailFillSources(&dynState, &Response->sources)) {
        Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_OFFSET_SOURCES;
    }
    if (KswordARKThreadDetailFillKernelGlobals(&dynState, &Response->kernelGlobals)) {
        Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_KERNEL_GLOBALS;
    }

    status = PsLookupThreadByThreadId(ULongToHandle(Request->threadId), &threadObject);
    if (!NT_SUCCESS(status)) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_LOOKUP_FAILED;
        Response->lastStatus = status;
        (VOID)RtlStringCchPrintfW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"PsLookupThreadByThreadId failed for TID %lu: 0x%08X.", Request->threadId, (unsigned int)status);
        return STATUS_SUCCESS;
    }

    Response->threadObjectAddress = (ULONG64)(ULONG_PTR)threadObject;
    Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_OBJECT_ADDRESS;
    Response->threadId = HandleToULong(PsGetThreadId(threadObject));
    Response->processId = HandleToULong(PsGetThreadProcessId(threadObject));
    Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_PUBLIC_IDENTITY;

    if (Request->processId != 0UL && Request->processId != Response->processId) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_LOOKUP_FAILED;
        Response->lastStatus = STATUS_OBJECT_NAME_NOT_FOUND;
        (VOID)RtlStringCchPrintfW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"TID %lu belongs to PID %lu, not requested PID %lu.", Response->threadId, Response->processId, Request->processId);
        ObDereferenceObject(threadObject);
        return STATUS_SUCCESS;
    }

    if ((requestFlags & KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_IDENTITY) != 0UL) {
        status = KswordARKThreadDetailReadClientId(threadObject, dynState.Kernel.EtCid, &Response->cidUniqueProcess, &Response->cidUniqueThread);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_ETHREAD_CID;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_THREAD_LIST_FIELDS;
            ++missingRequired;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);

        processObject = PsGetThreadProcess(threadObject);
        Response->processObjectAddress = (ULONG64)(ULONG_PTR)processObject;
    }

    if ((requestFlags & KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_LISTS) != 0UL) {
        status = KswordARKThreadDetailReadListEntry(threadObject, dynState.Kernel.EtThreadListEntry, &Response->threadListEntryFlink, &Response->threadListEntryBlink);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_THREAD_LIST_ENTRY;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_THREAD_LIST_FIELDS;
            ++missingRequired;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_START) != 0UL) {
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.EtStartAddress, &Response->startAddress);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_START_ADDRESS;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_THREAD_LIST_FIELDS;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);

        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.EtWin32StartAddress, &Response->win32StartAddress);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_WIN32_START_ADDRESS;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_STACK) != 0UL) {
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtProcess, &Response->kthreadProcessObject);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_KTHREAD_PROCESS;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_THREAD_LIST_FIELDS;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);

        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtInitialStack, &Response->initialStack);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtStackLimit, &Response->stackLimit);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtStackBase, &Response->stackBase);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtKernelStack, &Response->kernelStack);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_STACK_LIMITS;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_THREAD_STACK_FIELDS;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_IO) != 0UL) {
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtReadOperationCount, &Response->readOperationCount);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtWriteOperationCount, &Response->writeOperationCount);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtOtherOperationCount, &Response->otherOperationCount);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtReadTransferCount, &Response->readTransferCount);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtWriteTransferCount, &Response->writeTransferCount);
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
        status = KswordARKThreadDetailReadUlong64Field(threadObject, dynState.Kernel.KtOtherTransferCount, &Response->otherTransferCount);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_THREAD_DETAIL_FIELD_IO_COUNTERS;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_THREAD_IO_COUNTERS;
        }
        KswordARKThreadDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    ObDereferenceObject(threadObject);

    if (missingRequired != 0UL) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_CAPABILITY_MISSING;
    }
    else if (failureCount != 0UL) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_PARTIAL;
    }
    else {
        Response->status = KSWORD_ARK_DETAIL_STATUS_OK;
    }

    Response->lastStatus = lastStatus;
    (VOID)RtlStringCchPrintfW(
        Response->detail,
        KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS,
        L"Thread detail sampled by TID. fields=0x%08lX missingCaps=0x%I64X failures=%lu.",
        (unsigned long)Response->fieldFlags,
        Response->missingCapabilityMask,
        (unsigned long)failureCount);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKThreadIoctlQueryDetail(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL。中文说明：handler 只做固定
    buffer 校验和栈上请求拷贝，真实只读 ETHREAD/KTHREAD 采样在后端完成。

Arguments:

    Device - WDF 设备对象；当前仅保持 handler 签名一致。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；必须包含 KSWORD_ARK_THREAD_DETAIL_REQUEST。
    OutputBufferLength - 输出长度；必须容纳 KSWORD_ARK_THREAD_DETAIL_RESPONSE。
    BytesReturned - 返回响应字节数。

Return Value:

    NTSTATUS from WDF buffer retrieval or thread detail backend.

--*/
{
    KSWORD_ARK_THREAD_DETAIL_REQUEST requestCopy;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_THREAD_DETAIL_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    RtlCopyMemory(&requestCopy, inputBuffer, sizeof(requestCopy));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_THREAD_DETAIL_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return KswordARKDriverQueryThreadDetail(
        (KSWORD_ARK_THREAD_DETAIL_RESPONSE*)outputBuffer,
        actualOutputLength,
        &requestCopy,
        BytesReturned);
}


static size_t
KswordARKThreadRuntimeFieldRequestHeaderSize(VOID)
/*++

Routine Description:

    计算线程 runtime field sample 变长请求头长度。中文说明：items[1]
    是协议占位，真实输入长度必须扣除这一项。

Arguments:

    None.

Return Value:

    不包含第一条 item 占位的请求头字节数。

--*/
{
    return sizeof(KSWORD_ARK_THREAD_RUNTIME_FIELD_SAMPLE_REQUEST) -
        sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST);
}

static size_t
KswordARKThreadRuntimeFieldSampleResponseHeaderSize(VOID)
/*++

Routine Description:

    计算 runtime field sample 变长响应头长度。

Arguments:

    None.

Return Value:

    不包含第一条 row 占位的响应头字节数。

--*/
{
    return sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE) -
        sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW);
}

static BOOLEAN
KswordARKThreadRuntimeFieldSampleInputValid(
    _In_ size_t HeaderSize,
    _In_ ULONG ItemCount,
    _In_ size_t InputBufferLength
    )
/*++

Routine Description:

    校验线程 runtime field sample 变长请求长度是否覆盖声明的全部 item。

Arguments:

    HeaderSize - 不包含 items[1] 的请求头长度。
    ItemCount - R3 声明的采样项数量。
    InputBufferLength - WDF 实际输入长度。

Return Value:

    TRUE 表示输入长度可用，FALSE 表示长度不足或计数溢出。

--*/
{
    const size_t itemSize = sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST);
    size_t requiredLength = HeaderSize;

    if (InputBufferLength < HeaderSize) {
        return FALSE;
    }
    if (ItemCount > ((((size_t)-1) - HeaderSize) / itemSize)) {
        return FALSE;
    }

    requiredLength = HeaderSize + ((size_t)ItemCount * itemSize);
    return (InputBufferLength >= requiredLength) ? TRUE : FALSE;
}

static ULONG
KswordARKThreadRuntimeFieldSampleReturnedCount(
    _In_ ULONG ItemCount,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    根据输出缓冲区容量和协议最大项数计算线程 sample returnedCount。

Arguments:

    ItemCount - 请求项数量。
    OutputBufferLength - 输出缓冲区长度。

Return Value:

    最多可写入的行数。

--*/
{
    const size_t headerSize = KswordARKThreadRuntimeFieldSampleResponseHeaderSize();
    const ULONG cappedItemCount = ItemCount > KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS ?
        KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS : ItemCount;
    size_t outputRowCapacity = 0U;

    if (OutputBufferLength <= headerSize) {
        return 0UL;
    }

    outputRowCapacity = (OutputBufferLength - headerSize) /
        sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW);
    if (outputRowCapacity > (size_t)cappedItemCount) {
        return cappedItemCount;
    }

    return (ULONG)outputRowCapacity;
}

static VOID
KswordARKThreadRuntimeFieldSampleOne(
    _In_ const VOID* Object,
    _In_ const KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST* Item,
    _Out_ KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW* Row
    )
/*++

Routine Description:

    从 R0 引用到的 ETHREAD/KTHREAD 对象中按 PDB offset 只读采样一个小字段。

Arguments:

    Object - ETHREAD 地址，必须来自 PsLookupThreadByThreadId。
    Item - deep offset JSON 转来的 runtimeItemId/offset/size 元数据。
    Row - 输出行，记录字段状态与小字节样本。

Return Value:

    None. 字段级错误通过 Row->status/lastStatus 返回。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(Row, sizeof(*Row));
    Row->runtimeItemId = Item->runtimeItemId;
    Row->offset = Item->offset;
    Row->size = Item->size;
    Row->flags = Item->flags;
    Row->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_UNKNOWN;
    Row->lastStatus = STATUS_SUCCESS;

    if (Item->size == 0UL || Item->size > KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_VALUE_BYTES) {
        Row->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_SIZE_REJECTED;
        Row->lastStatus = STATUS_INVALID_PARAMETER;
        return;
    }
    if (Item->offset == KSW_DYN_OFFSET_UNAVAILABLE || Item->offset == 0x0000FFFFUL) {
        Row->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OFFSET_REJECTED;
        Row->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        return;
    }
    if (Item->offset > (KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_OFFSET - Item->size)) {
        Row->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OFFSET_REJECTED;
        Row->lastStatus = STATUS_INVALID_PARAMETER;
        return;
    }

    status = KswordARKCrossViewReadMemory(
        (const UCHAR*)Object + Item->offset,
        Row->sampleBytes,
        Item->size);
    Row->lastStatus = status;
    if (!NT_SUCCESS(status)) {
        Row->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_READ_FAILED;
        return;
    }

    Row->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW_STATUS_OK;
    Row->bytesRead = Item->size;
    if (Item->size <= sizeof(Row->valueU64)) {
        RtlCopyMemory(&Row->valueU64, Row->sampleBytes, Item->size);
    }
}

NTSTATUS
KswordARKDriverQueryThreadRuntimeFields(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_reads_bytes_(InputBufferLength) const KSWORD_ARK_THREAD_RUNTIME_FIELD_SAMPLE_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    按 TID 对 ETHREAD/KTHREAD 执行通用 deep PDB runtime field 小字段采样。

Arguments:

    Response - 变长 METHOD_BUFFERED 响应缓冲区。
    OutputBufferLength - 输出缓冲区长度。
    Request - 变长请求，包含 TID/PID 和字段采样项。
    InputBufferLength - 输入缓冲区实际长度。
    BytesWrittenOut - 实际写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应头有效；字段级结果放在 entries[].status。

--*/
{
    KSW_DYN_STATE dynState;
    PETHREAD threadObject = NULL;
    ULONG returnedCount = 0UL;
    ULONG rowIndex = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    const size_t requestHeaderSize = KswordARKThreadRuntimeFieldRequestHeaderSize();
    const size_t responseHeaderSize = KswordARKThreadRuntimeFieldSampleResponseHeaderSize();

    if (Response == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < responseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    *BytesWrittenOut = responseHeaderSize;
    RtlZeroMemory(Response, OutputBufferLength);
    Response->version = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_PROTOCOL_VERSION;
    Response->entrySize = sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW);
    Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_UNKNOWN;

    if (Request->version != KSWORD_ARK_RUNTIME_FIELD_SAMPLE_PROTOCOL_VERSION ||
        !KswordARKThreadRuntimeFieldSampleInputValid(requestHeaderSize, Request->itemCount, InputBufferLength)) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_INVALID_REQUEST;
        Response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    Response->totalCount = Request->itemCount;
    Response->flags = Request->flags;
    returnedCount = KswordARKThreadRuntimeFieldSampleReturnedCount(Request->itemCount, OutputBufferLength);
    Response->returnedCount = returnedCount;
    *BytesWrittenOut = responseHeaderSize + ((size_t)returnedCount * sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW));
    if (returnedCount < Request->itemCount || Request->itemCount > KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_TRUNCATED;
    }

    KswordARKDynDataSnapshot(&dynState);
    Response->dynDataCapabilityMask = dynState.CapabilityMask;

    if (Request->threadId == 0UL) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_LOOKUP_FAILED;
        Response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    status = PsLookupThreadByThreadId(ULongToHandle(Request->threadId), &threadObject);
    if (!NT_SUCCESS(status)) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_LOOKUP_FAILED;
        Response->lastStatus = status;
        return STATUS_SUCCESS;
    }

    Response->objectAddress = (ULONG64)(ULONG_PTR)threadObject;
    for (rowIndex = 0UL; rowIndex < returnedCount; ++rowIndex) {
        KswordARKThreadRuntimeFieldSampleOne(threadObject, &Request->items[rowIndex], &Response->entries[rowIndex]);
        if (!NT_SUCCESS(Response->entries[rowIndex].lastStatus) && Response->lastStatus == STATUS_SUCCESS) {
            Response->lastStatus = Response->entries[rowIndex].lastStatus;
        }
    }

    if (Response->status == KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_UNKNOWN) {
        Response->status = (Response->lastStatus == STATUS_SUCCESS) ?
            KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_OK :
            KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_PARTIAL;
    }

    ObDereferenceObject(threadObject);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKThreadIoctlQueryRuntimeFields(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_THREAD_RUNTIME_FIELDS。中文说明：handler 只
    获取 WDF 缓冲区，真实只读采样由后端完成。

Arguments:

    Device - WDF 设备对象；当前仅保持 handler 签名一致。
    Request - 当前 IOCTL 请求。
    InputBufferLength - WDF 输入长度提示。
    OutputBufferLength - WDF 输出长度提示。
    BytesReturned - 返回实际响应字节数。

Return Value:

    NTSTATUS from WDF buffer retrieval or thread runtime sampler backend.

--*/
{
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        KswordARKThreadRuntimeFieldRequestHeaderSize(),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KswordARKThreadRuntimeFieldSampleResponseHeaderSize(),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return KswordARKDriverQueryThreadRuntimeFields(
        (KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE*)outputBuffer,
        actualOutputLength,
        (const KSWORD_ARK_THREAD_RUNTIME_FIELD_SAMPLE_REQUEST*)inputBuffer,
        actualInputLength,
        BytesReturned);
}
