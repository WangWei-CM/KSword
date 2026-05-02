#pragma once

#include "callback_internal.h"

EXTERN_C_START

VOID
KswordArkCallbackExternalSafeAddCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    );

NTSTATUS
KswordArkCallbackExternalSafeRemove(
    _In_ const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST* RequestPacket,
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE* ResponsePacket
    );

EXTERN_C_END
