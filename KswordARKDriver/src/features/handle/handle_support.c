/*++

Module Name:

    handle_support.c

Abstract:

    Shared helpers for handle table and object detail queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "handle_support.h"

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

