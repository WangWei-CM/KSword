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

EXTERN_C_END
