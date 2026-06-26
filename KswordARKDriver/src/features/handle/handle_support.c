/*++

Module Name:

    handle_support.c

Abstract:

    Shared helpers for handle table and object detail queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "handle_support.h"

/*
 * KSWORD_ARK_OBJECT_HEADER_COMPAT
 * Inputs:
 * - Used only with an already decoded object header or a referenced object body.
 * Processing:
 * - Mirrors the stable front portion needed for read-only audit output, while
 *   KswordARKHandleHasObjectHeaderAuditCapability gates all field reads through
 *   the active DynData/Profile capability mask.
 * Return behavior:
 * - Plain layout helper; it has no function-like return value and must not be
 *   used for mutation.
 */
typedef struct _KSWORD_ARK_OBJECT_HEADER_COMPAT
{
    SSIZE_T PointerCount;
    PVOID HandleCountOrNextToFree;
    EX_PUSH_LOCK Lock;
    UCHAR TypeIndex;
    UCHAR TraceFlags;
    UCHAR InfoMask;
    UCHAR Flags;
#ifdef _WIN64
    ULONG Reserved;
#endif
    PVOID ObjectCreateInfoOrQuotaBlockCharged;
    PVOID SecurityDescriptor;
    QUAD Body;
} KSWORD_ARK_OBJECT_HEADER_COMPAT, *PKSWORD_ARK_OBJECT_HEADER_COMPAT;

C_ASSERT(FIELD_OFFSET(KSWORD_ARK_OBJECT_HEADER_COMPAT, Body) == sizeof(KSWORD_ARK_OBJECT_HEADER_COMPAT) - sizeof(QUAD));

static BOOLEAN
KswordARKHandleHasObjectHeaderAuditCapability(
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Gate every OBJECT_HEADER field read behind the active DynData/Profile
    capability set. 中文说明：当前 profile 尚未暴露逐字段 OBJECT_HEADER offset；
    因此本函数要求对象类型字段 capability 可用，并在缺失时只返回行级降级状态。

Arguments:

    DynState - Active DynData snapshot captured for the request.

Return Value:

    TRUE when object-header audit reads are allowed; otherwise FALSE.

--*/
{
    if (DynState == NULL) {
        return FALSE;
    }

    if ((DynState->CapabilityMask & KSW_CAP_OBJECT_TYPE_FIELDS) == 0ULL) {
        return FALSE;
    }

    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.OtIndex)) {
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
KswordARKHandleIsOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Normalize DynData offset availability before touching a private kernel field.
    中文说明：System Informer 缺失值和 Ksword 缺失值都在这里统一判定，避免
    后续读取路径散落重复判断。

Arguments:

    Offset - Candidate offset or shift value from the active DynData state.

Return Value:

    TRUE when the value can be used; otherwise FALSE.

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

ULONG
KswordARKHandleNormalizeOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Convert internal unavailable sentinels into the handle protocol sentinel.
    中文说明：UI 依赖这个原始偏移做诊断展示，不参与任何后续操作。

Arguments:

    Offset - Raw DynData value.

Return Value:

    Usable offset/shift or KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE.

--*/
{
    if (!KswordARKHandleIsOffsetPresent(Offset)) {
        return KSWORD_ARK_HANDLE_OFFSET_UNAVAILABLE;
    }

    return Offset;
}

VOID
KswordARKHandlePrepareObjectDynData(
    _Inout_ KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* Response,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Copy object-query DynData diagnostics into the response. 中文说明：只输出
    OBJECT_TYPE 相关 offset 和 capability，不泄漏可复用的内核操作凭据。

Arguments:

    Response - Mutable object query response.
    DynState - Active DynData snapshot.

Return Value:

    None.

--*/
{
    if (Response == NULL || DynState == NULL) {
        return;
    }

    Response->dynDataCapabilityMask = DynState->CapabilityMask;
    Response->otNameOffset = KswordARKHandleNormalizeOffset(DynState->Kernel.OtName);
    Response->otIndexOffset = KswordARKHandleNormalizeOffset(DynState->Kernel.OtIndex);
}

PVOID
KswordARKHandleGetObjectBodyFromHeader(
    _In_opt_ PVOID ObjectHeader
    )
/*++

Routine Description:

    Convert an OBJECT_HEADER pointer into its body pointer. 中文说明：调用方只把
    结果作为只读展示/查询输入，不把该地址作为任意对象操作凭据。

Arguments:

    ObjectHeader - Optional decoded object header pointer.

Return Value:

    Object body pointer when ObjectHeader is non-NULL; otherwise NULL.

--*/
{
    if (ObjectHeader == NULL) {
        return NULL;
    }

    return &((PKSWORD_ARK_OBJECT_HEADER_COMPAT)ObjectHeader)->Body;
}

PVOID
KswordARKHandleGetObjectHeaderFromBody(
    _In_opt_ PVOID ObjectBody
    )
/*++

Routine Description:

    Convert an object body pointer back to its OBJECT_HEADER. 中文说明：该转换仅在
    object 已经通过 ObReferenceObjectByHandle 引用成功后使用，读取失败仍会被
    per-entry/per-query 状态吸收。

Arguments:

    ObjectBody - Optional object body pointer.

Return Value:

    Object header pointer when ObjectBody is non-NULL; otherwise NULL.

--*/
{
    if (ObjectBody == NULL) {
        return NULL;
    }

    return CONTAINING_RECORD(ObjectBody, KSWORD_ARK_OBJECT_HEADER_COMPAT, Body);
}

NTSTATUS
KswordARKHandleReadObjectTypeIndex(
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ ULONG* ObjectTypeIndexOut
    )
/*++

Routine Description:

    Read OBJECT_TYPE.Index through the DynData OtIndex offset. 中文说明：这里不
    依赖编译期 OBJECT_TYPE 布局，缺失 offset 时返回 STATUS_NOT_SUPPORTED。

Arguments:

    ObjectType - Object type pointer from ObGetObjectType.
    DynState - Active DynData snapshot containing OtIndex.
    ObjectTypeIndexOut - Receives the decoded type index.

Return Value:

    STATUS_SUCCESS when the index is decoded; otherwise an NTSTATUS failure.

--*/
{
    if (ObjectTypeIndexOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ObjectTypeIndexOut = 0UL;

    if (ObjectType == NULL || DynState == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.OtIndex)) {
        return STATUS_NOT_SUPPORTED;
    }

    __try {
        *ObjectTypeIndexOut = (ULONG)(*(PUCHAR)((PUCHAR)ObjectType + DynState->Kernel.OtIndex));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

ULONG
KswordARKHandleMergeTypeIndexSource(
    _In_ BOOLEAN ObjectTypeIndexPresent,
    _In_ ULONG ObjectTypeIndex,
    _In_ BOOLEAN HeaderTypeIndexPresent,
    _In_ ULONG HeaderTypeIndex
    )
/*++

Routine Description:

    Describe where the final type index came from. 中文说明：当 OBJECT_TYPE.Index
    与 OBJECT_HEADER.TypeIndex 同时可读但不一致时，R3 可以直接显示 mismatch。

Arguments:

    ObjectTypeIndexPresent - TRUE when OBJECT_TYPE.Index was decoded.
    ObjectTypeIndex - Index from OBJECT_TYPE.
    HeaderTypeIndexPresent - TRUE when OBJECT_HEADER.TypeIndex was decoded.
    HeaderTypeIndex - Index from OBJECT_HEADER.

Return Value:

    KSWORD_ARK_OBJECT_TYPE_SOURCE_* value.

--*/
{
    if (ObjectTypeIndexPresent && HeaderTypeIndexPresent) {
        return (ObjectTypeIndex == HeaderTypeIndex) ?
            KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MATCH :
            KSWORD_ARK_OBJECT_TYPE_SOURCE_BOTH_MISMATCH;
    }

    if (ObjectTypeIndexPresent) {
        return KSWORD_ARK_OBJECT_TYPE_SOURCE_OBJECT_TYPE_INDEX;
    }

    if (HeaderTypeIndexPresent) {
        return KSWORD_ARK_OBJECT_TYPE_SOURCE_OBJECT_HEADER;
    }

    return KSWORD_ARK_OBJECT_TYPE_SOURCE_NONE;
}

static VOID
KswordARKHandleReadHeaderValues(
    _In_opt_ PVOID ObjectHeader,
    _In_opt_ PVOID ObjectBody,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ ULONG* FieldFlagsOut,
    _Out_ ULONG* DecodeStatusOut,
    _Out_ NTSTATUS* ReadStatusOut,
    _Out_ ULONG* HeaderTypeIndexOut,
    _Out_ ULONG* InfoMaskOut,
    _Out_ ULONG* HeaderFlagsOut,
    _Out_ ULONG* TraceFlagsOut,
    _Out_ LONG64* PointerCountOut,
    _Out_ ULONG64* HandleCountOut,
    _Out_ ULONG64* HeaderAddressOut,
    _Out_ ULONG64* TypeAddressOut,
    _Out_ ULONG* TypeIndexSourceOut,
    _Out_ ULONG* NameInfoStatusOut
    )
/*++

Routine Description:

    Safely read common OBJECT_HEADER fields into scalar outputs. 中文说明：所有
    指针解引用都在 __try 中完成，异常只影响当前行/当前查询。

Arguments:

    ObjectHeader - Optional header pointer; derived from ObjectBody when absent.
    ObjectBody - Optional body pointer used to derive the header.
    ObjectType - Optional object type pointer used for source comparison.
    DynState - Active DynData snapshot used for capability and OtIndex reads.
    FieldFlagsOut - Receives protocol field flags.
    DecodeStatusOut - Receives protocol decode status.
    ReadStatusOut - Receives NTSTATUS read status.
    HeaderTypeIndexOut - Receives OBJECT_HEADER.TypeIndex.
    InfoMaskOut - Receives OBJECT_HEADER.InfoMask.
    HeaderFlagsOut - Receives OBJECT_HEADER.Flags.
    TraceFlagsOut - Receives OBJECT_HEADER.TraceFlags.
    PointerCountOut - Receives OBJECT_HEADER.PointerCount.
    HandleCountOut - Receives OBJECT_HEADER.HandleCount.
    HeaderAddressOut - Receives OBJECT_HEADER address.
    TypeAddressOut - Receives OBJECT_TYPE address.
    TypeIndexSourceOut - Receives source/mismatch classification.
    NameInfoStatusOut - Receives conservative name-info availability status.

Return Value:

    None.

--*/
{
    PKSWORD_ARK_OBJECT_HEADER_COMPAT header = NULL;
    ULONG objectTypeIndex = 0UL;
    NTSTATUS typeIndexStatus = STATUS_UNSUCCESSFUL;
    BOOLEAN objectTypeIndexPresent = FALSE;
    BOOLEAN headerTypeIndexPresent = FALSE;

    *FieldFlagsOut = 0UL;
    *DecodeStatusOut = KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE;
    *ReadStatusOut = STATUS_SUCCESS;
    *HeaderTypeIndexOut = 0UL;
    *InfoMaskOut = 0UL;
    *HeaderFlagsOut = 0UL;
    *TraceFlagsOut = 0UL;
    *PointerCountOut = 0LL;
    *HandleCountOut = 0ULL;
    *HeaderAddressOut = 0ULL;
    *TypeAddressOut = 0ULL;
    *TypeIndexSourceOut = KSWORD_ARK_OBJECT_TYPE_SOURCE_NONE;
    *NameInfoStatusOut = KSWORD_ARK_OBJECT_NAME_INFO_STATUS_UNKNOWN;

    if (!KswordARKHandleHasObjectHeaderAuditCapability(DynState)) {
        *DecodeStatusOut = KSWORD_ARK_HANDLE_DECODE_STATUS_HEADER_DYNDATA_MISSING;
        *ReadStatusOut = STATUS_NOT_SUPPORTED;
        return;
    }

    header = (PKSWORD_ARK_OBJECT_HEADER_COMPAT)ObjectHeader;
    if (header == NULL) {
        header = (PKSWORD_ARK_OBJECT_HEADER_COMPAT)KswordARKHandleGetObjectHeaderFromBody(ObjectBody);
    }
    if (header == NULL) {
        *DecodeStatusOut = KSWORD_ARK_HANDLE_DECODE_STATUS_OBJECT_DECODE_FAILED;
        *ReadStatusOut = STATUS_INVALID_PARAMETER;
        return;
    }

    __try {
        *HeaderAddressOut = (ULONG64)(ULONG_PTR)header;
        *PointerCountOut = (LONG64)header->PointerCount;
        *HandleCountOut = (ULONG64)(ULONG_PTR)header->HandleCountOrNextToFree;
        *HeaderTypeIndexOut = (ULONG)header->TypeIndex;
        *InfoMaskOut = (ULONG)header->InfoMask;
        *HeaderFlagsOut = (ULONG)header->Flags;
        *TraceFlagsOut = (ULONG)header->TraceFlags;
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_PRESENT;
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_POINTER_COUNT_PRESENT;
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_HANDLE_COUNT_PRESENT;
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_HEADER_TYPE_INDEX_PRESENT;
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_INFO_MASK_PRESENT;
        headerTypeIndexPresent = TRUE;
        *NameInfoStatusOut = (header->InfoMask == 0U) ?
            KSWORD_ARK_OBJECT_NAME_INFO_STATUS_HEADER_MASK_EMPTY :
            KSWORD_ARK_OBJECT_NAME_INFO_STATUS_HEADER_MASK_NONZERO;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *DecodeStatusOut = KSWORD_ARK_HANDLE_DECODE_STATUS_HEADER_READ_FAILED;
        *ReadStatusOut = GetExceptionCode();
        return;
    }

    if (ObjectBody != NULL) {
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_BODY_PRESENT;
    }
    if (ObjectType != NULL) {
        *FieldFlagsOut |= KSWORD_ARK_HANDLE_FIELD_OBJECT_TYPE_PRESENT;
        *TypeAddressOut = (ULONG64)(ULONG_PTR)ObjectType;
        typeIndexStatus = KswordARKHandleReadObjectTypeIndex(ObjectType, DynState, &objectTypeIndex);
        objectTypeIndexPresent = NT_SUCCESS(typeIndexStatus) ? TRUE : FALSE;
    }

    *TypeIndexSourceOut = KswordARKHandleMergeTypeIndexSource(
        objectTypeIndexPresent,
        objectTypeIndex,
        headerTypeIndexPresent,
        *HeaderTypeIndexOut);
    *DecodeStatusOut = KSWORD_ARK_HANDLE_DECODE_STATUS_OK;
}

VOID
KswordARKHandleFillEntryObjectHeaderAudit(
    _Inout_ KSWORD_ARK_HANDLE_ENTRY* Entry,
    _In_opt_ PVOID ObjectHeader,
    _In_opt_ PVOID ObjectBody,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Copy OBJECT_HEADER audit fields into one enumeration entry. 中文说明：失败只
    写入 Entry->objectHeaderDecodeStatus，不覆盖已经成功解码的基础 handle 字段。

Arguments:

    Entry - Mutable handle enumeration row.
    ObjectHeader - Optional decoded OBJECT_HEADER pointer.
    ObjectBody - Optional object body pointer.
    ObjectType - Optional object type pointer.
    DynState - Active DynData snapshot.

Return Value:

    None.

--*/
{
    ULONG headerFieldFlags = 0UL;

    if (Entry == NULL) {
        return;
    }

    KswordARKHandleReadHeaderValues(
        ObjectHeader,
        ObjectBody,
        ObjectType,
        DynState,
        &headerFieldFlags,
        &Entry->objectHeaderDecodeStatus,
        &Entry->objectHeaderReadStatus,
        &Entry->objectHeaderTypeIndex,
        &Entry->objectHeaderInfoMask,
        &Entry->objectHeaderFlags,
        &Entry->objectHeaderTraceFlags,
        &Entry->pointerCount,
        &Entry->handleCount,
        &Entry->objectHeaderAddress,
        &Entry->objectTypeAddress,
        &Entry->objectTypeIndexSource,
        &Entry->nameInfoStatus);
    Entry->fieldFlags |= headerFieldFlags;
}

VOID
KswordARKHandleFillQueryObjectHeaderAudit(
    _Inout_ KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* Response,
    _In_opt_ PVOID ObjectHeader,
    _In_opt_ PVOID ObjectBody,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Copy OBJECT_HEADER audit fields into the QueryHandleObject response.
    中文说明：该 helper 与枚举路径共用读取逻辑，保证失败状态语义一致。

Arguments:

    Response - Mutable object query response.
    ObjectHeader - Optional object header pointer.
    ObjectBody - Optional referenced object body pointer.
    ObjectType - Optional object type pointer.
    DynState - Active DynData snapshot.

Return Value:

    None.

--*/
{
    ULONG headerFieldFlags = 0UL;

    if (Response == NULL) {
        return;
    }

    KswordARKHandleReadHeaderValues(
        ObjectHeader,
        ObjectBody,
        ObjectType,
        DynState,
        &headerFieldFlags,
        &Response->objectHeaderDecodeStatus,
        &Response->objectHeaderReadStatus,
        &Response->objectHeaderTypeIndex,
        &Response->objectHeaderInfoMask,
        &Response->objectHeaderFlags,
        &Response->objectHeaderTraceFlags,
        &Response->pointerCount,
        &Response->handleCount,
        &Response->objectHeaderAddress,
        &Response->objectTypeAddress,
        &Response->objectTypeIndexSource,
        &Response->nameInfoStatus);
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_HEADER_PRESENT;
    }
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_OBJECT_HEADER_BODY_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_HEADER_BODY_PRESENT;
    }
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_POINTER_COUNT_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_POINTER_COUNT_PRESENT;
    }
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_HANDLE_COUNT_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_HANDLE_COUNT_PRESENT;
    }
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_HEADER_TYPE_INDEX_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_HEADER_TYPE_INDEX_PRESENT;
    }
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_INFO_MASK_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_INFO_MASK_PRESENT;
    }
    if ((headerFieldFlags & KSWORD_ARK_HANDLE_FIELD_OBJECT_TYPE_PRESENT) != 0UL) {
        Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_TYPE_PRESENT;
    }
}
