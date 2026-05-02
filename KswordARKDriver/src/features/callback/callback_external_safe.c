/*++

Module Name:

    callback_external_safe.c

Abstract:

    Safe handling for callback classes that require version-gated private data.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_external_safe.h"
#include "ark/ark_dyndata.h"

VOID
KswordArkCallbackExternalSafeAddCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    添加 ETW/Object/Registry 外部移除能力说明。中文说明：Object 和 Registry
    的实际枚举行由既有私有只读扫描阶段产生；ETW 当前只有 DynData 偏移能力，
    未能安全定位全局表入口，因此这里只补充“不安全不移除”的明确状态。

Arguments:

    Builder - 输入输出枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;
    KSW_DYN_STATE dynState;

    if (Builder == NULL) {
        return;
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry != NULL) {
        entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER;
        entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_ETW_DYNDATA;
        entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED;
        entry->lastStatus = STATUS_NOT_SUPPORTED;
        entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
            KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS;
        entry->contextAddress = ((ULONG64)dynState.Kernel.EgeGuid << 32) |
            (ULONG64)dynState.Kernel.EreGuidEntry;
        KswordArkCallbackEnumCopyWide(
            entry->name,
            RTL_NUMBER_OF(entry->name),
            L"ETW callback external capability");
        (VOID)RtlStringCbPrintfW(
            entry->detail,
            sizeof(entry->detail),
            L"ETW 仅检测到 DynData 能力位 cap=0x%llX；未安全定位 ETW 全局 Guid 表，枚举/移除均返回 STATUS_NOT_SUPPORTED。",
            dynState.CapabilityMask);
    }

    KswordArkCallbackEnumAddUnsupportedRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT,
        L"Object callback external removal",
        L"Object callback 可由现有私有只读枚举展示候选项；缺少公开按外部 registration handle 卸载 API，移除返回 STATUS_NOT_SUPPORTED。");
    KswordArkCallbackEnumAddUnsupportedRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY,
        L"Registry callback external removal",
        L"Registry callback 可由现有 CmCallbackListHead 只读枚举展示候选项；无法从函数地址安全恢复 cookie，移除返回 STATUS_NOT_SUPPORTED。");
}

NTSTATUS
KswordArkCallbackExternalSafeRemove(
    _In_ const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST* RequestPacket,
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE* ResponsePacket
    )
/*++

Routine Description:

    处理需要内部结构才能移除的外部回调类别。中文说明：本函数只做参数和
    可验证内核模块范围门控，不盲写 Object/Registry/ETW 私有链表，也不关闭
    PatchGuard 或任何系统保护。

Arguments:

    RequestPacket - 输入移除请求。
    ResponsePacket - 输入输出移除响应。

Return Value:

    地址不合法返回 STATUS_INVALID_PARAMETER；类别需要内部结构但当前无安全路径
    时返回 STATUS_NOT_SUPPORTED。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    KSWORD_ARK_CALLBACK_MODULE_CACHE moduleCache;
    WCHAR modulePath[KSWORD_ARK_EXTERNAL_CALLBACK_MODULE_NAME_MAX_CHARS];
    ULONG64 moduleBase = 0ULL;
    ULONG moduleSize = 0UL;

    if (RequestPacket == NULL || ResponsePacket == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (RequestPacket->callbackAddress == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (RequestPacket->callbackClass) {
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY:
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER:
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(modulePath, sizeof(modulePath));
    KswordArkCallbackEnumInitModuleCache(&moduleCache);
    status = KswordArkCallbackEnumResolveModuleByAddressCached(
        &moduleCache,
        RequestPacket->callbackAddress,
        modulePath,
        RTL_NUMBER_OF(modulePath),
        &moduleBase,
        &moduleSize);
    KswordArkCallbackEnumFreeModuleCache(&moduleCache);
    if (!NT_SUCCESS(status)) {
        return STATUS_INVALID_PARAMETER;
    }

    ResponsePacket->mappingFlags |= KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_MODULE;
    ResponsePacket->moduleBase = moduleBase;
    ResponsePacket->moduleSize = moduleSize;
    KswordArkCallbackEnumCopyWide(
        ResponsePacket->modulePath,
        RTL_NUMBER_OF(ResponsePacket->modulePath),
        modulePath);

    return STATUS_NOT_SUPPORTED;
}
