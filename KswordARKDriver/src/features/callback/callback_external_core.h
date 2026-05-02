#pragma once

#include "callback_internal.h"

#if defined(KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL)
#include "callback_external_minifilter.h"
#include "callback_external_safe.h"
#include "callback_external_wfp.h"
#endif

static __inline VOID
KswordArkCallbackExternalAddCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    聚合外部回调枚举扩展。中文说明：该 inline 入口允许现有 callback_enum.c
    在工程文件尚未追加 core 源文件时仍能编译；具体重型枚举仍位于可选
    callback_external_*.c 文件中。

Arguments:

    Builder - 输入输出枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    if (Builder == NULL) {
        return;
    }

#if defined(KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL)
    KswordArkCallbackExternalWfpAddCallbacks(Builder);
    KswordArkCallbackExternalSafeAddCallbacks(Builder);
    KswordArkCallbackExternalMinifilterAddCallbacks(Builder);
#else
    KswordArkCallbackEnumAddUnsupportedRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT,
        L"WFP callout external package",
        L"外部回调扩展源文件尚未加入工程；主会话追加 callback_external_*.c 并定义启用宏后可使用公开 WFP 枚举/移除。");
    KswordArkCallbackEnumAddUnsupportedRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER,
        L"ETW callback external package",
        L"ETW 外部回调需要安全全局表定位；当前默认返回 STATUS_NOT_SUPPORTED。");
#endif
}

static __inline NTSTATUS
KswordArkCallbackExternalRemoveByRequest(
    _In_ const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST* RequestPacket,
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE* ResponsePacket
    )
/*++

Routine Description:

    聚合外部回调移除扩展。中文说明：每个子模块负责公开 API 校验或返回
    STATUS_NOT_SUPPORTED；该 inline 入口避免新增 core 编译单元成为硬依赖。

Arguments:

    RequestPacket - 输入移除请求。
    ResponsePacket - 输入输出移除响应。

Return Value:

    返回具体移除实现的 NTSTATUS；未知类别返回 STATUS_INVALID_PARAMETER。

--*/
{
    if (RequestPacket == NULL || ResponsePacket == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

#if !defined(KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL)
    UNREFERENCED_PARAMETER(ResponsePacket);
#endif

    switch (RequestPacket->callbackClass) {
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT:
#if defined(KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL)
        return KswordArkCallbackExternalWfpRemove(RequestPacket, ResponsePacket);
#else
        return STATUS_NOT_SUPPORTED;
#endif

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER:
#if defined(KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL)
        return KswordArkCallbackExternalMinifilterRemove(RequestPacket, ResponsePacket);
#else
        return STATUS_NOT_SUPPORTED;
#endif

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER:
#if defined(KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL)
        return KswordArkCallbackExternalSafeRemove(RequestPacket, ResponsePacket);
#else
        return STATUS_NOT_SUPPORTED;
#endif

    default:
        return STATUS_INVALID_PARAMETER;
    }
}
