#pragma once

#include "ark/ark_handle.h"
#include "ark/ark_dyndata.h"

EXTERN_C_START

BOOLEAN
KswordARKHandleIsOffsetPresent(
    _In_ ULONG Offset
    );

ULONG
KswordARKHandleNormalizeOffset(
    _In_ ULONG Offset
    );

VOID
KswordARKHandlePrepareObjectDynData(
    _Inout_ KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* Response,
    _In_ const KSW_DYN_STATE* DynState
    );

PVOID
KswordARKHandleGetObjectBodyFromHeader(
    _In_opt_ PVOID ObjectHeader
    );

PVOID
KswordARKHandleGetObjectHeaderFromBody(
    _In_opt_ PVOID ObjectBody
    );

NTSTATUS
KswordARKHandleReadObjectTypeIndex(
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ ULONG* ObjectTypeIndexOut
    );

ULONG
KswordARKHandleMergeTypeIndexSource(
    _In_ BOOLEAN ObjectTypeIndexPresent,
    _In_ ULONG ObjectTypeIndex,
    _In_ BOOLEAN HeaderTypeIndexPresent,
    _In_ ULONG HeaderTypeIndex
    );

VOID
KswordARKHandleFillEntryObjectHeaderAudit(
    _Inout_ KSWORD_ARK_HANDLE_ENTRY* Entry,
    _In_opt_ PVOID ObjectHeader,
    _In_opt_ PVOID ObjectBody,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState
    );

VOID
KswordARKHandleFillQueryObjectHeaderAudit(
    _Inout_ KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* Response,
    _In_opt_ PVOID ObjectHeader,
    _In_opt_ PVOID ObjectBody,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState
    );

EXTERN_C_END
