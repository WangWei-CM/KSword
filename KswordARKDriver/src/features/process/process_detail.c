/*++

Module Name:

    process_detail.c

Abstract:

    Read-only PDB/DynData-backed process runtime detail query.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "process_crossview.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTSYSAPI
PCHAR
NTAPI
PsGetProcessImageFileName(
    _In_ PEPROCESS Process
    );

static BOOLEAN
KswordARKProcessDetailOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    判断一个 DynData/PDB 偏移是否可用于只读采样。

Arguments:

    Offset - 来自 KSW_DYN_STATE 的字段偏移。

Return Value:

    TRUE 表示可读，FALSE 表示缺失或占位 sentinel。

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static ULONG
KswordARKProcessDetailProtocolOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    将驱动内部 offset sentinel 转成 process shared 协议 sentinel。

Arguments:

    Offset - 原始 DynData/PDB 偏移。

Return Value:

    可用 offset 或 KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE。

--*/
{
    if (!KswordARKProcessDetailOffsetPresent(Offset)) {
        return KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
    }

    return Offset;
}

static VOID
KswordARKProcessDetailFillOffsets(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_PROCESS_DETAIL_OFFSETS* Offsets
    )
/*++

Routine Description:

    把当前内核 DynData 偏移快照复制到 shared 响应结构。

Arguments:

    DynState - KswordARKDynDataSnapshot 输出。
    Offsets - R3/R0 共享的进程详情 offset 响应字段。

Return Value:

    None.

--*/
{
    RtlZeroMemory(Offsets, sizeof(*Offsets));
    Offsets->epUniqueProcessId = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpUniqueProcessId);
    Offsets->epActiveProcessLinks = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpActiveProcessLinks);
    Offsets->epThreadListHead = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpThreadListHead);
    Offsets->epImageFileName = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpImageFileName);
    Offsets->epToken = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpToken);
    Offsets->epObjectTable = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpObjectTable);
    Offsets->epSectionObject = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpSectionObject);
    Offsets->epProtection = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpProtection);
    Offsets->epSignatureLevel = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpSignatureLevel);
    Offsets->epSectionSignatureLevel = KswordARKProcessDetailProtocolOffset(DynState->Kernel.EpSectionSignatureLevel);
}

static BOOLEAN
KswordARKProcessDetailSourcePresent(
    _In_ ULONG Source
    )
/*++

Routine Description:

    判断 DynData 字段来源是否可展示。中文说明：来源为 unavailable 时，
    UI 不应把它当成有效 PDB/System Informer 证据。

Arguments:

    Source - KSW_DYN_FIELD_SOURCE_* 来源值。

Return Value:

    TRUE 表示来源有效，FALSE 表示缺失。

--*/
{
    return (Source != KSW_DYN_FIELD_SOURCE_UNAVAILABLE) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKProcessDetailFillSources(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_PROCESS_DETAIL_SOURCES* Sources
    )
/*++

Routine Description:

    把进程 detail 使用到的每个 EPROCESS 偏移来源复制到 shared 响应。

Arguments:

    DynState - KswordARKDynDataSnapshot 输出。
    Sources - R3/R0 共享的进程详情来源响应字段。

Return Value:

    TRUE 表示至少一个来源可展示，FALSE 表示来源全部缺失。

--*/
{
    BOOLEAN anySourcePresent = FALSE;

    RtlZeroMemory(Sources, sizeof(*Sources));
    Sources->epUniqueProcessId = DynState->KernelSources.EpUniqueProcessId;
    Sources->epActiveProcessLinks = DynState->KernelSources.EpActiveProcessLinks;
    Sources->epThreadListHead = DynState->KernelSources.EpThreadListHead;
    Sources->epImageFileName = DynState->KernelSources.EpImageFileName;
    Sources->epToken = DynState->KernelSources.EpToken;
    Sources->epObjectTable = DynState->KernelSources.EpObjectTable;
    Sources->epSectionObject = DynState->KernelSources.EpSectionObject;
    Sources->epProtection = DynState->KernelSources.EpProtection;
    Sources->epSignatureLevel = DynState->KernelSources.EpSignatureLevel;
    Sources->epSectionSignatureLevel = DynState->KernelSources.EpSectionSignatureLevel;

    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epUniqueProcessId) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epActiveProcessLinks) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epThreadListHead) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epImageFileName) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epToken) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epObjectTable) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epSectionObject) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epProtection) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epSignatureLevel) ? TRUE : anySourcePresent;
    anySourcePresent = KswordARKProcessDetailSourcePresent(Sources->epSectionSignatureLevel) ? TRUE : anySourcePresent;

    return anySourcePresent;
}

static ULONG64
KswordARKProcessDetailKernelGlobalAddress(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONG Rva
    )
/*++

Routine Description:

    将 ntoskrnl PDB profile 中的全局 RVA 转成当前启动会话内核地址。

Arguments:

    DynState - 当前 DynData 快照，提供 ntoskrnl identity 和 imageBase。
    Rva - 已校验的全局符号 RVA。

Return Value:

    可展示的当前内核 VA；缺失时返回 0。

--*/
{
    if (!DynState->NtosActive || DynState->Ntoskrnl.imageBase == 0ULL) {
        return 0ULL;
    }
    if (!KswordARKProcessDetailOffsetPresent(Rva)) {
        return 0ULL;
    }

    return DynState->Ntoskrnl.imageBase + (ULONG64)Rva;
}

static BOOLEAN
KswordARKProcessDetailFillKernelGlobals(
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_RUNTIME_KERNEL_GLOBALS* Globals
    )
/*++

Routine Description:

    复制 PDB profile EX 提供的关键 ntoskrnl 全局 RVA 与来源。

Arguments:

    DynState - 当前 DynData 快照。
    Globals - 共享响应中的全局 RVA/来源/运行时地址包。

Return Value:

    TRUE 表示至少一个全局 RVA 可展示，FALSE 表示全部缺失。

--*/
{
    BOOLEAN anyGlobalPresent = FALSE;

    RtlZeroMemory(Globals, sizeof(*Globals));
    Globals->pspCidTableRva = KswordARKProcessDetailProtocolOffset(DynState->KernelGlobals.PspCidTable);
    Globals->psLoadedModuleListRva = KswordARKProcessDetailProtocolOffset(DynState->KernelGlobals.PsLoadedModuleList);
    Globals->mmUnloadedDriversRva = KswordARKProcessDetailProtocolOffset(DynState->KernelGlobals.MmUnloadedDrivers);
    Globals->piDdbCacheTableRva = KswordARKProcessDetailProtocolOffset(DynState->KernelGlobals.PiDDBCacheTable);
    Globals->keServiceDescriptorTableShadowRva = KswordARKProcessDetailProtocolOffset(DynState->KernelGlobals.KeServiceDescriptorTableShadow);
    Globals->mmLastUnloadedDriverRva = KswordARKProcessDetailProtocolOffset(DynState->KernelGlobals.MmLastUnloadedDriver);
    Globals->pspCidTableSource = DynState->KernelGlobalSources.PspCidTable;
    Globals->psLoadedModuleListSource = DynState->KernelGlobalSources.PsLoadedModuleList;
    Globals->mmUnloadedDriversSource = DynState->KernelGlobalSources.MmUnloadedDrivers;
    Globals->piDdbCacheTableSource = DynState->KernelGlobalSources.PiDDBCacheTable;
    Globals->keServiceDescriptorTableShadowSource = DynState->KernelGlobalSources.KeServiceDescriptorTableShadow;
    Globals->mmLastUnloadedDriverSource = DynState->KernelGlobalSources.MmLastUnloadedDriver;
    Globals->pspCidTableAddress = KswordARKProcessDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.PspCidTable);
    Globals->psLoadedModuleListAddress = KswordARKProcessDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.PsLoadedModuleList);
    Globals->mmUnloadedDriversAddress = KswordARKProcessDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.MmUnloadedDrivers);
    Globals->piDdbCacheTableAddress = KswordARKProcessDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.PiDDBCacheTable);
    Globals->keServiceDescriptorTableShadowAddress = KswordARKProcessDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.KeServiceDescriptorTableShadow);
    Globals->mmLastUnloadedDriverAddress = KswordARKProcessDetailKernelGlobalAddress(DynState, DynState->KernelGlobals.MmLastUnloadedDriver);

    anyGlobalPresent = KswordARKProcessDetailOffsetPresent(DynState->KernelGlobals.PspCidTable) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKProcessDetailOffsetPresent(DynState->KernelGlobals.PsLoadedModuleList) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKProcessDetailOffsetPresent(DynState->KernelGlobals.MmUnloadedDrivers) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKProcessDetailOffsetPresent(DynState->KernelGlobals.PiDDBCacheTable) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKProcessDetailOffsetPresent(DynState->KernelGlobals.KeServiceDescriptorTableShadow) ? TRUE : anyGlobalPresent;
    anyGlobalPresent = KswordARKProcessDetailOffsetPresent(DynState->KernelGlobals.MmLastUnloadedDriver) ? TRUE : anyGlobalPresent;

    return anyGlobalPresent;
}

static VOID
KswordARKProcessDetailCopyImageName(
    _Out_writes_bytes_(KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS) CHAR* Destination,
    _In_opt_z_ const CHAR* Source
    )
/*++

Routine Description:

    将 PsGetProcessImageFileName 返回的短名复制到固定协议缓冲区。

Arguments:

    Destination - 响应包内 16 字节 ANSI 短名缓冲区。
    Source - 内核返回的进程短名，可以为 NULL。

Return Value:

    None.

--*/
{
    SIZE_T index = 0U;

    RtlZeroMemory(Destination, KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS);
    if (Source == NULL) {
        return;
    }

    while (index + 1U < KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS && Source[index] != '\0') {
        Destination[index] = Source[index];
        ++index;
    }
}

static NTSTATUS
KswordARKProcessDetailReadUcharField(
    _In_ const VOID* Object,
    _In_ ULONG Offset,
    _Out_ UCHAR* ValueOut
    )
/*++

Routine Description:

    读取 EPROCESS 中一个 UCHAR 字段，统一经过 cross-view 安全读 helper。

Arguments:

    Object - EPROCESS 地址。
    Offset - 字段偏移。
    ValueOut - 输出字节。

Return Value:

    STATUS_SUCCESS 或安全读失败状态。

--*/
{
    if (ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKProcessDetailOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return KswordARKCrossViewReadMemory((const UCHAR*)Object + Offset, ValueOut, sizeof(*ValueOut));
}

static NTSTATUS
KswordARKProcessDetailReadListEntryField(
    _In_ const VOID* Object,
    _In_ ULONG Offset,
    _Out_ ULONG64* FlinkOut,
    _Out_ ULONG64* BlinkOut
    )
/*++

Routine Description:

    读取 EPROCESS 内嵌 LIST_ENTRY，供 ActiveProcessLinks/ThreadListHead 展示。

Arguments:

    Object - EPROCESS 地址。
    Offset - LIST_ENTRY 字段偏移。
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
    if (!KswordARKProcessDetailOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    RtlZeroMemory(&listEntry, sizeof(listEntry));
    status = KswordARKCrossViewReadMemory((const UCHAR*)Object + Offset, &listEntry, sizeof(listEntry));
    if (NT_SUCCESS(status)) {
        *FlinkOut = (ULONG64)(ULONG_PTR)listEntry.Flink;
        *BlinkOut = (ULONG64)(ULONG_PTR)listEntry.Blink;
    }

    return status;
}

static VOID
KswordARKProcessDetailNoteFailure(
    _In_ NTSTATUS ReadStatus,
    _Inout_ ULONG* FailureCount,
    _Inout_ NTSTATUS* LastStatus
    )
/*++

Routine Description:

    记录一次字段读取失败，用于最终 status/detail 汇总。

Arguments:

    ReadStatus - 单字段读取状态。
    FailureCount - 累计失败数字段。
    LastStatus - 最近失败 NTSTATUS。

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
KswordARKDriverQueryProcessDetail(
    _Out_writes_bytes_(OutputBufferLength) KSWORD_ARK_PROCESS_DETAIL_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_PROCESS_DETAIL_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询单个进程的只读运行时详情。中文说明：该函数只按 PID 获取 EPROCESS，
    然后使用已应用 PDB/DynData 偏移读取字段，不修改目标进程或任何内核链表。

Arguments:

    Response - METHOD_BUFFERED 输出响应。
    OutputBufferLength - 输出缓冲区长度。
    Request - 可选固定请求；NULL 时返回 STATUS_INVALID_PARAMETER。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；字段级缺失通过 response.status/detail 表达。

--*/
{
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    ULONG requestFlags = KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_ALL;
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
    Response->processId = Request->processId;
    Response->requestedFlags = Request->flags;

    if (Request->version != KSWORD_ARK_RUNTIME_DETAIL_PROTOCOL_VERSION) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_UNSUPPORTED;
        Response->lastStatus = STATUS_REVISION_MISMATCH;
        (VOID)RtlStringCchCopyW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"Process detail request version mismatch.");
        return STATUS_SUCCESS;
    }

    if (Request->processId == 0UL) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_LOOKUP_FAILED;
        Response->lastStatus = STATUS_INVALID_PARAMETER;
        (VOID)RtlStringCchCopyW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"Process detail requires a non-zero PID.");
        return STATUS_SUCCESS;
    }

    if (Request->flags != 0UL) {
        requestFlags = Request->flags;
    }

    KswordARKDynDataSnapshot(&dynState);
    Response->dynDataCapabilityMask = dynState.CapabilityMask;
    KswordARKProcessDetailFillOffsets(&dynState, &Response->offsets);
    if (KswordARKProcessDetailFillSources(&dynState, &Response->sources)) {
        Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_OFFSET_SOURCES;
    }
    if (KswordARKProcessDetailFillKernelGlobals(&dynState, &Response->kernelGlobals)) {
        Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_KERNEL_GLOBALS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        Response->status = KSWORD_ARK_DETAIL_STATUS_LOOKUP_FAILED;
        Response->lastStatus = status;
        (VOID)RtlStringCchPrintfW(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"PsLookupProcessByProcessId failed for PID %lu: 0x%08X.", Request->processId, (unsigned int)status);
        return STATUS_SUCCESS;
    }

    Response->processObjectAddress = (ULONG64)(ULONG_PTR)processObject;
    Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_OBJECT_ADDRESS;
    Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_PUBLIC_IDENTITY;
    KswordARKProcessDetailCopyImageName(Response->imageName, PsGetProcessImageFileName(processObject));

    if ((requestFlags & KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_PUBLIC_IDENTITY) != 0UL) {
        Response->uniqueProcessIdValue = (ULONG64)(ULONG_PTR)PsGetProcessId(processObject);
        Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_UNIQUE_PROCESS_ID;
        /* 先确认 PDB/DynData offset 可用，避免把 unavailable sentinel 当成 EPROCESS 偏移读取。 */
        if (KswordARKProcessDetailOffsetPresent(dynState.Kernel.EpImageFileName)) {
            status = KswordARKCrossViewReadMemory(
                (const UCHAR*)processObject + dynState.Kernel.EpImageFileName,
                Response->imageName,
                KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS - 1U);
        }
        else {
            status = STATUS_PROCEDURE_NOT_FOUND;
        }
        if (NT_SUCCESS(status)) {
            Response->imageName[KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS - 1U] = '\0';
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_IMAGE_FILE_NAME;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_LIST_FIELDS;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_LIST_LINKS) != 0UL) {
        status = KswordARKProcessDetailReadListEntryField(processObject, dynState.Kernel.EpActiveProcessLinks, &Response->activeProcessLinksFlink, &Response->activeProcessLinksBlink);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_ACTIVE_PROCESS_LINKS;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_LIST_FIELDS;
            ++missingRequired;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);

        status = KswordARKProcessDetailReadListEntryField(processObject, dynState.Kernel.EpThreadListHead, &Response->threadListHeadFlink, &Response->threadListHeadBlink);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_THREAD_LIST_HEAD;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_LIST_FIELDS;
            ++missingRequired;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_TOKEN_FASTREF) != 0UL) {
        PVOID tokenFastRef = NULL;
        status = KswordARKCrossViewReadPointerField(processObject, dynState.Kernel.EpToken, &tokenFastRef);
        if (NT_SUCCESS(status)) {
            Response->tokenFastRef = (ULONG64)(ULONG_PTR)tokenFastRef;
            Response->tokenObjectAddress = Response->tokenFastRef & ~0xFULL;
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_TOKEN_FASTREF;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_LIST_FIELDS;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_OBJECT_POINTERS) != 0UL) {
        PVOID objectTable = NULL;
        PVOID sectionObject = NULL;
        status = KswordARKCrossViewReadPointerField(processObject, dynState.Kernel.EpObjectTable, &objectTable);
        if (NT_SUCCESS(status)) {
            Response->objectTableAddress = (ULONG64)(ULONG_PTR)objectTable;
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_OBJECT_TABLE;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_OBJECT_TABLE;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);

        status = KswordARKCrossViewReadPointerField(processObject, dynState.Kernel.EpSectionObject, &sectionObject);
        if (NT_SUCCESS(status)) {
            Response->sectionObjectAddress = (ULONG64)(ULONG_PTR)sectionObject;
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_SECTION_OBJECT;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_SECTION_CONTROL_AREA;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if ((requestFlags & KSWORD_ARK_PROCESS_DETAIL_FLAG_INCLUDE_PROTECTION) != 0UL) {
        status = KswordARKProcessDetailReadUcharField(processObject, dynState.Kernel.EpProtection, &Response->protection);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_PROTECTION;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_PROTECTION_PATCH;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);

        status = KswordARKProcessDetailReadUcharField(processObject, dynState.Kernel.EpSignatureLevel, &Response->signatureLevel);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_SIGNATURE_LEVEL;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_PROTECTION_PATCH;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);

        status = KswordARKProcessDetailReadUcharField(processObject, dynState.Kernel.EpSectionSignatureLevel, &Response->sectionSignatureLevel);
        if (NT_SUCCESS(status)) {
            Response->fieldFlags |= KSWORD_ARK_PROCESS_DETAIL_FIELD_SECTION_SIGNATURE;
        }
        else {
            Response->missingCapabilityMask |= KSW_CAP_PROCESS_PROTECTION_PATCH;
        }
        KswordARKProcessDetailNoteFailure(status, &failureCount, &lastStatus);
    }

    if (processObject != NULL) {
        ObDereferenceObject(processObject);
        processObject = NULL;
    }

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
        L"Process detail sampled by PID. fields=0x%08lX missingCaps=0x%I64X failures=%lu.",
        (unsigned long)Response->fieldFlags,
        Response->missingCapabilityMask,
        (unsigned long)failureCount);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKProcessIoctlQueryDetail(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL。中文说明：handler 只做固定
    buffer 校验和栈上请求拷贝，真实只读字段采样在
    KswordARKDriverQueryProcessDetail 中完成。

Arguments:

    Device - WDF 设备对象；当前只用于签名一致，不直接访问设备扩展。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；必须包含 KSWORD_ARK_PROCESS_DETAIL_REQUEST。
    OutputBufferLength - 输出长度；必须容纳 KSWORD_ARK_PROCESS_DETAIL_RESPONSE。
    BytesReturned - 返回响应字节数。

Return Value:

    NTSTATUS from WDF buffer retrieval or process detail backend.

--*/
{
    KSWORD_ARK_PROCESS_DETAIL_REQUEST requestCopy;
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
        sizeof(KSWORD_ARK_PROCESS_DETAIL_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&requestCopy, sizeof(requestCopy));
    RtlCopyMemory(&requestCopy, inputBuffer, sizeof(requestCopy));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_PROCESS_DETAIL_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return KswordARKDriverQueryProcessDetail(
        (KSWORD_ARK_PROCESS_DETAIL_RESPONSE*)outputBuffer,
        actualOutputLength,
        &requestCopy,
        BytesReturned);
}


static size_t
KswordARKProcessRuntimeFieldRequestHeaderSize(VOID)
/*++

Routine Description:

    计算进程 runtime field sample 变长请求头长度。中文说明：entries[1]
    是 shared 协议的变长占位，真实输入长度必须扣掉这一项。

Arguments:

    None.

Return Value:

    不包含第一条 item 占位的请求头字节数。

--*/
{
    return sizeof(KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST) -
        sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST);
}

static size_t
KswordARKRuntimeFieldSampleResponseHeaderSize(VOID)
/*++

Routine Description:

    计算 runtime field sample 变长响应头长度。中文说明：调用方用该值
    计算输出缓冲区最多能容纳多少行。

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
KswordARKRuntimeFieldSampleRequestLengthValid(
    _In_ size_t HeaderSize,
    _In_ ULONG ItemCount,
    _In_ size_t InputBufferLength
    )
/*++

Routine Description:

    校验 runtime field sample 输入缓冲区是否覆盖声明的全部 item。

Arguments:

    HeaderSize - 不包含 items[1] 的变长请求头长度。
    ItemCount - R3 声明的采样项数量。
    InputBufferLength - WDF 实际输入缓冲区长度。

Return Value:

    TRUE 表示长度一致，FALSE 表示输入被截断或计数溢出。

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
KswordARKRuntimeFieldSampleReturnedCount(
    _In_ ULONG ItemCount,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    根据输出缓冲区容量计算本次最多可返回的 runtime sample 行数。

Arguments:

    ItemCount - 请求项数量。
    OutputBufferLength - WDF 输出缓冲区长度。

Return Value:

    受协议最大值和输出缓冲区共同限制后的 returnedCount。

--*/
{
    const size_t headerSize = KswordARKRuntimeFieldSampleResponseHeaderSize();
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
KswordARKRuntimeFieldSampleOne(
    _In_ const VOID* Object,
    _In_ const KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST* Item,
    _Out_ KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW* Row
    )
/*++

Routine Description:

    从已由 R0 lookup/reference 得到的内核对象中只读采样一个小字段。

Arguments:

    Object - EPROCESS 或 ETHREAD 基址，绝不来自 R3 输入。
    Item - R3 传入的 runtimeItemId/offset/size 元数据。
    Row - 输出行，包含状态、原始小字节和 U64 摘要。

Return Value:

    None. 字段级失败写入 Row->status 与 Row->lastStatus。

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
KswordARKDriverQueryProcessRuntimeFields(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_reads_bytes_(InputBufferLength) const KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    按 PID 对 EPROCESS 执行通用 deep PDB runtime field 小字段采样。

Arguments:

    Response - 变长 METHOD_BUFFERED 响应缓冲区。
    OutputBufferLength - 输出缓冲区长度。
    Request - 变长请求，包含 PID 和字段采样项。
    InputBufferLength - 输入缓冲区实际长度。
    BytesWrittenOut - 实际写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应头有效；行级失败通过 entries[].status 表达。

--*/
{
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    ULONG returnedCount = 0UL;
    ULONG rowIndex = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    const size_t requestHeaderSize = KswordARKProcessRuntimeFieldRequestHeaderSize();
    const size_t responseHeaderSize = KswordARKRuntimeFieldSampleResponseHeaderSize();

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
        !KswordARKRuntimeFieldSampleRequestLengthValid(requestHeaderSize, Request->itemCount, InputBufferLength)) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_INVALID_REQUEST;
        Response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    Response->totalCount = Request->itemCount;
    Response->flags = Request->flags;
    returnedCount = KswordARKRuntimeFieldSampleReturnedCount(Request->itemCount, OutputBufferLength);
    Response->returnedCount = returnedCount;
    *BytesWrittenOut = responseHeaderSize + ((size_t)returnedCount * sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW));
    if (returnedCount < Request->itemCount || Request->itemCount > KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_TRUNCATED;
    }

    KswordARKDynDataSnapshot(&dynState);
    Response->dynDataCapabilityMask = dynState.CapabilityMask;

    if (Request->processId == 0UL) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_LOOKUP_FAILED;
        Response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        Response->status = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_LOOKUP_FAILED;
        Response->lastStatus = status;
        return STATUS_SUCCESS;
    }

    Response->objectAddress = (ULONG64)(ULONG_PTR)processObject;
    for (rowIndex = 0UL; rowIndex < returnedCount; ++rowIndex) {
        KswordARKRuntimeFieldSampleOne(processObject, &Request->items[rowIndex], &Response->entries[rowIndex]);
        if (!NT_SUCCESS(Response->entries[rowIndex].lastStatus) && Response->lastStatus == STATUS_SUCCESS) {
            Response->lastStatus = Response->entries[rowIndex].lastStatus;
        }
    }

    if (Response->status == KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_UNKNOWN) {
        Response->status = (Response->lastStatus == STATUS_SUCCESS) ?
            KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_OK :
            KSWORD_ARK_RUNTIME_FIELD_SAMPLE_STATUS_PARTIAL;
    }

    ObDereferenceObject(processObject);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKProcessIoctlQueryRuntimeFields(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_PROCESS_RUNTIME_FIELDS。中文说明：handler 只
    获取 WDF 输入/输出缓冲区，真实只读采样由后端完成。

Arguments:

    Device - WDF 设备对象；当前仅保持 handler 签名一致。
    Request - 当前 IOCTL 请求。
    InputBufferLength - WDF 传入的输入长度提示。
    OutputBufferLength - WDF 传入的输出长度提示。
    BytesReturned - 返回实际响应字节数。

Return Value:

    NTSTATUS from WDF buffer retrieval or process runtime sampler backend.

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
        KswordARKProcessRuntimeFieldRequestHeaderSize(),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KswordARKRuntimeFieldSampleResponseHeaderSize(),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return KswordARKDriverQueryProcessRuntimeFields(
        (KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE*)outputBuffer,
        actualOutputLength,
        (const KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST*)inputBuffer,
        actualInputLength,
        BytesReturned);
}
