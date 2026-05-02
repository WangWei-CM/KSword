/*++

Module Name:

    callback_external_minifilter.c

Abstract:

    Enumerates and safely unloads minifilters through Filter Manager APIs.

Environment:

    Kernel-mode minifilter support library

--*/

#include "callback_external_minifilter.h"

typedef struct _KSWORD_ARK_EXTERNAL_MINIFILTER_ID
{
    ULONG64 FilterObject;
    WCHAR Name[KSWORD_ARK_CALLBACK_ENUM_NAME_CHARS];
} KSWORD_ARK_EXTERNAL_MINIFILTER_ID;

static NTSTATUS
KswordArkCallbackExternalMinifilterQueryName(
    _In_ PFLT_FILTER FilterObject,
    _Out_writes_(NameChars) PWCHAR NameBuffer,
    _In_ ULONG NameChars,
    _Out_opt_ ULONG64* InstanceInfoOut
    )
/*++

Routine Description:

    查询一个 minifilter 的公开名称。中文说明：函数只使用
    FltGetFilterInformation(FilterAggregateStandardInformation)，不读取
    Filter Manager 私有结构。

Arguments:

    FilterObject - 输入 Filter Manager filter 对象。
    NameBuffer - 输出 filter 名称缓冲。
    NameChars - 输出缓冲字符数。
    InstanceInfoOut - 可选输出 FrameID/实例数压缩值。

Return Value:

    成功返回 STATUS_SUCCESS；查询失败返回 FltGetFilterInformation 的状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesReturned = 0UL;
    FILTER_AGGREGATE_STANDARD_INFORMATION* filterInfo = NULL;
    UNICODE_STRING nameString;

    if (NameBuffer == NULL || NameChars == 0UL || FilterObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    NameBuffer[0] = L'\0';
    if (InstanceInfoOut != NULL) {
        *InstanceInfoOut = 0ULL;
    }
    RtlZeroMemory(&nameString, sizeof(nameString));

    status = FltGetFilterInformation(
        FilterObject,
        FilterAggregateStandardInformation,
        NULL,
        0UL,
        &bytesReturned);
    if (status != STATUS_BUFFER_TOO_SMALL || bytesReturned < sizeof(FILTER_AGGREGATE_STANDARD_INFORMATION)) {
        return status;
    }

    filterInfo = (FILTER_AGGREGATE_STANDARD_INFORMATION*)KswordArkAllocateNonPaged(
        bytesReturned,
        KSWORD_ARK_CALLBACK_TAG_EXTERNAL);
    if (filterInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(filterInfo, bytesReturned);

    status = FltGetFilterInformation(
        FilterObject,
        FilterAggregateStandardInformation,
        filterInfo,
        bytesReturned,
        &bytesReturned);
    if (NT_SUCCESS(status) &&
        (filterInfo->Flags & FLTFL_ASI_IS_MINIFILTER) != 0UL &&
        filterInfo->Type.MiniFilter.FilterNameBufferOffset != 0U &&
        filterInfo->Type.MiniFilter.FilterNameLength != 0U) {
        nameString.Buffer = (PWCHAR)((PUCHAR)filterInfo + filterInfo->Type.MiniFilter.FilterNameBufferOffset);
        nameString.Length = filterInfo->Type.MiniFilter.FilterNameLength;
        nameString.MaximumLength = filterInfo->Type.MiniFilter.FilterNameLength;
        KswordArkCallbackEnumCopyUnicode(NameBuffer, NameChars, &nameString);
        if (InstanceInfoOut != NULL) {
            *InstanceInfoOut = ((ULONG64)filterInfo->Type.MiniFilter.FrameID << 32) |
                (ULONG64)filterInfo->Type.MiniFilter.NumberOfInstances;
        }
    }

    ExFreePool(filterInfo);
    return (NameBuffer[0] != L'\0') ? status : STATUS_NOT_FOUND;
}

VOID
KswordArkCallbackExternalMinifilterAddCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    添加外部 minifilter 能力说明。中文说明：主枚举入口已经通过
    FltEnumerateFilters 逐项列出 minifilter；本函数只补一行说明“移除”使用
    FltUnloadFilter 并且必须按枚举到的对象地址重新验证名称。

Arguments:

    Builder - 输入输出枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    if (Builder == NULL) {
        return;
    }

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION;
    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE;
    KswordArkCallbackEnumCopyWide(
        entry->name,
        RTL_NUMBER_OF(entry->name),
        L"Minifilter public unload capability");
    KswordArkCallbackEnumCopyWide(
        entry->detail,
        RTL_NUMBER_OF(entry->detail),
        L"Minifilter 已由 FltEnumerateFilters 枚举；安全移除只走 FltEnumerateFilters/FltUnloadFilter，目标驱动未提供卸载回调时返回不支持或对应错误。");
}

NTSTATUS
KswordArkCallbackExternalMinifilterRemove(
    _In_ const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST* RequestPacket,
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE* ResponsePacket
    )
/*++

Routine Description:

    通过 Filter Manager 公开路径卸载 minifilter。中文说明：请求的
    callbackAddress 必须匹配一次重新枚举得到的 PFLT_FILTER 地址，匹配后使用
    filter 名称调用 FltUnloadFilter；不从 PFLT_FILTER 私有结构写链表。

Arguments:

    RequestPacket - 输入移除请求，callbackAddress 承载枚举到的 PFLT_FILTER。
    ResponsePacket - 输入输出响应，serviceName 返回用于 FltUnloadFilter 的名称。

Return Value:

    成功返回 STATUS_SUCCESS；无法重新枚举匹配目标返回 STATUS_INVALID_PARAMETER；
    当前 IRQL 或目标卸载条件不满足时直接返回 FltUnloadFilter 的 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG filterCount = 0UL;
    ULONG filterIndex = 0UL;
    PFLT_FILTER* filterList = NULL;
    SIZE_T allocationBytes = 0U;
    KSWORD_ARK_EXTERNAL_MINIFILTER_ID targetId;
    UNICODE_STRING filterName;
    BOOLEAN found = FALSE;

    if (RequestPacket == NULL || ResponsePacket == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (RequestPacket->callbackClass != KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER ||
        RequestPacket->callbackAddress == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&targetId, sizeof(targetId));
    RtlZeroMemory(&filterName, sizeof(filterName));

    status = FltEnumerateFilters(NULL, 0UL, &filterCount);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_SUCCESS) {
        return status;
    }
    if (filterCount == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    allocationBytes = (SIZE_T)filterCount * sizeof(PFLT_FILTER);
    filterList = (PFLT_FILTER*)KswordArkAllocateNonPaged(
        allocationBytes,
        KSWORD_ARK_CALLBACK_TAG_EXTERNAL);
    if (filterList == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(filterList, allocationBytes);

    status = FltEnumerateFilters(filterList, filterCount, &filterCount);
    if (!NT_SUCCESS(status)) {
        ExFreePool(filterList);
        return status;
    }

    for (filterIndex = 0UL; filterIndex < filterCount; ++filterIndex) {
        PFLT_FILTER currentFilter = filterList[filterIndex];
        WCHAR currentName[KSWORD_ARK_CALLBACK_ENUM_NAME_CHARS];

        RtlZeroMemory(currentName, sizeof(currentName));
        if (currentFilter == NULL) {
            continue;
        }

        if ((ULONG64)(ULONG_PTR)currentFilter == RequestPacket->callbackAddress) {
            status = KswordArkCallbackExternalMinifilterQueryName(
                currentFilter,
                currentName,
                RTL_NUMBER_OF(currentName),
                NULL);
            if (NT_SUCCESS(status)) {
                targetId.FilterObject = (ULONG64)(ULONG_PTR)currentFilter;
                KswordArkCallbackEnumCopyWide(
                    targetId.Name,
                    RTL_NUMBER_OF(targetId.Name),
                    currentName);
                found = TRUE;
            }
        }
    }

    for (filterIndex = 0UL; filterIndex < filterCount; ++filterIndex) {
        if (filterList[filterIndex] != NULL) {
            FltObjectDereference(filterList[filterIndex]);
        }
    }
    ExFreePool(filterList);

    if (!found || targetId.Name[0] == L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    KswordArkCallbackEnumCopyWide(
        ResponsePacket->serviceName,
        RTL_NUMBER_OF(ResponsePacket->serviceName),
        targetId.Name);
    ResponsePacket->mappingFlags |=
        KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_ENUMERATED |
        KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_PUBLIC_API;

    RtlInitUnicodeString(&filterName, targetId.Name);
    status = FltUnloadFilter(&filterName);
    return status;
}
