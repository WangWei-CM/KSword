/*++

Module Name:

    network_runtime.c

Abstract:

    KswordARK WFP network rule runtime and status IOCTL backend.

Environment:

    Kernel-mode WFP callout driver

--*/

#include "network_internal.h"

#include <stdarg.h>

static KSWORD_ARK_NETWORK_RUNTIME g_KswordArkNetworkRuntime;

KSWORD_ARK_NETWORK_RUNTIME*
KswordARKNetworkGetRuntime(
    VOID
    )
/*++

Routine Description:

    返回网络运行时全局对象。中文说明：规则表访问必须由调用方持有 Lock，计数器
    可通过 interlocked 操作更新。

Arguments:

    None.

Return Value:

    指向 KSWORD_ARK_NETWORK_RUNTIME 的指针。

--*/
{
    return &g_KswordArkNetworkRuntime;
}

VOID
KswordARKNetworkLogFormat(
    _In_z_ PCSTR LevelText,
    _In_z_ _Printf_format_string_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    写入网络模块日志。中文说明：WFP classify 热路径只记录规则切换和异常，避免
    高频网络包造成日志通道拥塞。

Arguments:

    LevelText - 日志级别。
    FormatText - printf 风格格式串。
    ... - 格式化参数。

Return Value:

    None. 本函数没有返回值。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    CHAR logBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    if (runtime == NULL || runtime->Device == WDF_NO_HANDLE || FormatText == NULL) {
        return;
    }

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logBuffer, sizeof(logBuffer), FormatText, arguments))) {
        (VOID)KswordARKDriverEnqueueLogFrame(
            runtime->Device,
            LevelText != NULL ? LevelText : "Info",
            logBuffer);
    }
    va_end(arguments);
}

BOOLEAN
KswordARKNetworkRuleMatchesLocked(
    _In_ const KSWORD_ARK_NETWORK_RULE* Rule,
    _In_ ULONG Direction,
    _In_ ULONG Protocol,
    _In_ USHORT LocalPort,
    _In_ USHORT RemotePort,
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    判断网络五元组摘要是否命中规则。中文说明：端口为 0 表示通配，protocol 为
    ANY 表示通配，processId 为 0 表示所有进程。

Arguments:

    Rule - 规则快照项。
    Direction - 当前方向掩码。
    Protocol - IPPROTO_* 协议号。
    LocalPort - 本地端口。
    RemotePort - 远端端口。
    ProcessId - 进程 ID，未知时为 0。

Return Value:

    TRUE 表示命中；FALSE 表示未命中。

--*/
{
    if (Rule == NULL || (Rule->flags & KSWORD_ARK_NETWORK_RULE_FLAG_ENABLED) == 0UL) {
        return FALSE;
    }
    if ((Rule->directionMask & Direction) == 0UL) {
        return FALSE;
    }
    if (Rule->protocol != KSWORD_ARK_NETWORK_PROTOCOL_ANY && Rule->protocol != Protocol) {
        return FALSE;
    }
    if (Rule->processId != 0UL && Rule->processId != ProcessId) {
        return FALSE;
    }
    if (Rule->localPort != 0U && Rule->localPort != LocalPort) {
        return FALSE;
    }
    if (Rule->remotePort != 0U && Rule->remotePort != RemotePort) {
        return FALSE;
    }

    return TRUE;
}

static NTSTATUS
KswordARKNetworkValidateRule(
    _In_ const KSWORD_ARK_NETWORK_RULE* Rule
    )
/*++

Routine Description:

    校验单条网络规则。中文说明：仅接受 allow/block/hide-port，方向必须包含入站
    或出站，协议仅允许 ANY/TCP/UDP。

Arguments:

    Rule - 待校验规则。

Return Value:

    STATUS_SUCCESS 表示规则可用；失败状态表示应拒绝规则。

--*/
{
    if (Rule == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((Rule->flags & KSWORD_ARK_NETWORK_RULE_FLAG_ENABLED) == 0UL) {
        return STATUS_SUCCESS;
    }
    if (Rule->action != KSWORD_ARK_NETWORK_RULE_ACTION_ALLOW &&
        Rule->action != KSWORD_ARK_NETWORK_RULE_ACTION_BLOCK &&
        Rule->action != KSWORD_ARK_NETWORK_RULE_ACTION_HIDE_PORT) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((Rule->directionMask & KSWORD_ARK_NETWORK_DIRECTION_BOTH) == 0UL ||
        (Rule->directionMask & ~KSWORD_ARK_NETWORK_DIRECTION_BOTH) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Rule->protocol != KSWORD_ARK_NETWORK_PROTOCOL_ANY &&
        Rule->protocol != KSWORD_ARK_NETWORK_PROTOCOL_TCP &&
        Rule->protocol != KSWORD_ARK_NETWORK_PROTOCOL_UDP) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
KswordARKNetworkRefreshCountersLocked(
    _Inout_ KSWORD_ARK_NETWORK_RUNTIME* Runtime
    )
/*++

Routine Description:

    刷新规则数量与运行时标志。中文说明：WFP 注册状态保留，规则活动状态根据当前
    快照自动计算。

Arguments:

    Runtime - 网络运行时。

Return Value:

    None. 本函数没有返回值。

--*/
{
    ULONG ruleIndex = 0UL;
    ULONG registeredFlags = 0UL;

    if (Runtime == NULL) {
        return;
    }

    registeredFlags = Runtime->RuntimeFlags &
        (KSWORD_ARK_NETWORK_RUNTIME_WFP_REGISTERED | KSWORD_ARK_NETWORK_RUNTIME_WFP_STARTED);
    Runtime->RuntimeFlags = registeredFlags;
    Runtime->RuleCount = 0UL;
    Runtime->BlockedRuleCount = 0UL;
    Runtime->HiddenPortRuleCount = 0UL;

    for (ruleIndex = 0UL; ruleIndex < KSWORD_ARK_NETWORK_MAX_RULES; ++ruleIndex) {
        const KSWORD_ARK_NETWORK_RULE* rule = &Runtime->Rules[ruleIndex];
        if ((rule->flags & KSWORD_ARK_NETWORK_RULE_FLAG_ENABLED) == 0UL) {
            continue;
        }
        Runtime->RuleCount += 1UL;
        if (rule->action == KSWORD_ARK_NETWORK_RULE_ACTION_BLOCK) {
            Runtime->BlockedRuleCount += 1UL;
        }
        if (rule->action == KSWORD_ARK_NETWORK_RULE_ACTION_HIDE_PORT) {
            Runtime->HiddenPortRuleCount += 1UL;
        }
    }

    if (Runtime->RuleCount != 0UL) {
        Runtime->RuntimeFlags |= KSWORD_ARK_NETWORK_RUNTIME_RULES_ACTIVE;
    }
    if (Runtime->HiddenPortRuleCount != 0UL) {
        Runtime->RuntimeFlags |= KSWORD_ARK_NETWORK_RUNTIME_PORT_HIDE;
    }
}

NTSTATUS
KswordARKNetworkInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ WDFDEVICE Device
    )
/*++

Routine Description:

    初始化网络运行时并注册 WFP callout。中文说明：WFP 注册失败不应阻塞驱动主
    功能，因此调用方可以记录 warning 后继续加载。

Arguments:

    DriverObject - 驱动对象。
    Device - WDF 控制设备，用于日志。

Return Value:

    STATUS_SUCCESS 或 WFP 注册失败状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(&g_KswordArkNetworkRuntime, sizeof(g_KswordArkNetworkRuntime));
    ExInitializePushLock(&g_KswordArkNetworkRuntime.Lock);
    g_KswordArkNetworkRuntime.Device = Device;
    g_KswordArkNetworkRuntime.DriverObject = DriverObject;
    if (Device != WDF_NO_HANDLE) {
        g_KswordArkNetworkRuntime.DeviceObject = WdfDeviceWdmGetDeviceObject(Device);
    }
    g_KswordArkNetworkRuntime.RegisterStatus = STATUS_NOT_SUPPORTED;
    g_KswordArkNetworkRuntime.EngineStatus = STATUS_NOT_SUPPORTED;

    status = KswordARKNetworkWfpRegister(&g_KswordArkNetworkRuntime);
    g_KswordArkNetworkRuntime.RegisterStatus = status;
    if (NT_SUCCESS(status)) {
        g_KswordArkNetworkRuntime.RuntimeFlags |= KSWORD_ARK_NETWORK_RUNTIME_WFP_REGISTERED;
        KswordARKNetworkLogFormat("Info", "Network WFP callouts registered.");
        return STATUS_SUCCESS;
    }

    KswordARKNetworkLogFormat(
        "Warn",
        "Network WFP callout registration failed, status=0x%08X.",
        (unsigned int)status);
    return status;
}

VOID
KswordARKNetworkUninitialize(
    VOID
    )
/*++

Routine Description:

    清理网络运行时。中文说明：先清空规则，再注销 WFP callout 和 filter，避免卸载
    后 classify 继续引用规则表。

Arguments:

    None.

Return Value:

    None. 本函数没有返回值。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();

    if (runtime == NULL) {
        return;
    }

    ExAcquirePushLockExclusive(&runtime->Lock);
    RtlZeroMemory(runtime->Rules, sizeof(runtime->Rules));
    runtime->RuleCount = 0UL;
    runtime->BlockedRuleCount = 0UL;
    runtime->HiddenPortRuleCount = 0UL;
    runtime->RuntimeFlags &=
        (KSWORD_ARK_NETWORK_RUNTIME_WFP_REGISTERED | KSWORD_ARK_NETWORK_RUNTIME_WFP_STARTED);
    runtime->Generation += 1UL;
    ExReleasePushLockExclusive(&runtime->Lock);

    KswordARKNetworkWfpUnregister(runtime);
    runtime->RuntimeFlags = 0UL;
    runtime->RegisterStatus = STATUS_NOT_SUPPORTED;
    runtime->EngineStatus = STATUS_NOT_SUPPORTED;
}

NTSTATUS
KswordARKNetworkSetRules(
    _In_ const KSWORD_ARK_NETWORK_SET_RULES_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    替换网络过滤规则快照。中文说明：规则快照仅在完整校验成功后一次性替换，
    避免 classify 路径观察到半更新内容。

Arguments:

    Request - R3 规则请求。
    OutputBuffer - 响应缓冲。
    OutputBufferLength - 响应缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应已写入；缓冲错误直接返回失败。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_SET_RULES_RESPONSE* response = NULL;
    KSWORD_ARK_NETWORK_RULE newRules[KSWORD_ARK_NETWORK_MAX_RULES] = { 0 };
    ULONG ruleIndex = 0UL;
    ULONG appliedCount = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL || OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(*response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_SET_RULES_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_NETWORK_STATUS_UNKNOWN;
    response->rejectedIndex = 0xFFFFFFFFUL;
    response->lastStatus = STATUS_SUCCESS;
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_NETWORK_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_NETWORK_STATUS_OPERATION_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    if (Request->action == KSWORD_ARK_NETWORK_ACTION_REPLACE) {
        if (Request->ruleCount > KSWORD_ARK_NETWORK_MAX_RULES) {
            response->status = KSWORD_ARK_NETWORK_STATUS_INVALID_RULE;
            response->lastStatus = STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;
        }

        for (ruleIndex = 0UL; ruleIndex < Request->ruleCount; ++ruleIndex) {
            status = KswordARKNetworkValidateRule(&Request->rules[ruleIndex]);
            if (!NT_SUCCESS(status)) {
                response->status = KSWORD_ARK_NETWORK_STATUS_INVALID_RULE;
                response->rejectedIndex = ruleIndex;
                response->lastStatus = status;
                return STATUS_SUCCESS;
            }
            RtlCopyMemory(&newRules[ruleIndex], &Request->rules[ruleIndex], sizeof(newRules[ruleIndex]));
            if ((newRules[ruleIndex].flags & KSWORD_ARK_NETWORK_RULE_FLAG_ENABLED) != 0UL) {
                appliedCount += 1UL;
            }
        }
    }
    else if (Request->action != KSWORD_ARK_NETWORK_ACTION_CLEAR &&
        Request->action != KSWORD_ARK_NETWORK_ACTION_DISABLE) {
        response->status = KSWORD_ARK_NETWORK_STATUS_INVALID_RULE;
        response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    ExAcquirePushLockExclusive(&runtime->Lock);
    RtlZeroMemory(runtime->Rules, sizeof(runtime->Rules));
    if (Request->action == KSWORD_ARK_NETWORK_ACTION_REPLACE && appliedCount != 0UL) {
        RtlCopyMemory(runtime->Rules, newRules, sizeof(newRules));
    }
    runtime->Generation += 1UL;
    KswordARKNetworkRefreshCountersLocked(runtime);
    response->runtimeFlags = runtime->RuntimeFlags;
    response->appliedCount = runtime->RuleCount;
    response->blockedRuleCount = runtime->BlockedRuleCount;
    response->hiddenPortRuleCount = runtime->HiddenPortRuleCount;
    response->generation = runtime->Generation;
    ExReleasePushLockExclusive(&runtime->Lock);

    if (Request->action == KSWORD_ARK_NETWORK_ACTION_REPLACE) {
        response->status = NT_SUCCESS(runtime->RegisterStatus) ?
            KSWORD_ARK_NETWORK_STATUS_APPLIED :
            KSWORD_ARK_NETWORK_STATUS_WFP_UNAVAILABLE;
        response->lastStatus = runtime->RegisterStatus;
    }
    else if (Request->action == KSWORD_ARK_NETWORK_ACTION_DISABLE) {
        response->status = KSWORD_ARK_NETWORK_STATUS_DISABLED;
        response->lastStatus = STATUS_SUCCESS;
    }
    else {
        response->status = KSWORD_ARK_NETWORK_STATUS_CLEARED;
        response->lastStatus = STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKNetworkQueryStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询网络过滤运行时。中文说明：返回 WFP 注册状态、规则快照、阻断计数与端口
    隐藏规则数量，R3 后续可据此过滤端口表展示。

Arguments:

    OutputBuffer - 响应缓冲。
    OutputBufferLength - 响应缓冲长度。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 或缓冲区错误。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    KSWORD_ARK_NETWORK_STATUS_RESPONSE* response = NULL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(*response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_NETWORK_STATUS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
    response->status = NT_SUCCESS(runtime->RegisterStatus) ?
        KSWORD_ARK_NETWORK_STATUS_APPLIED :
        KSWORD_ARK_NETWORK_STATUS_WFP_UNAVAILABLE;

    ExAcquirePushLockShared(&runtime->Lock);
    response->runtimeFlags = runtime->RuntimeFlags;
    response->ruleCount = runtime->RuleCount;
    response->blockedRuleCount = runtime->BlockedRuleCount;
    response->hiddenPortRuleCount = runtime->HiddenPortRuleCount;
    response->generation = runtime->Generation;
    response->classifyCount = (ULONG64)runtime->ClassifyCount;
    response->blockedCount = (ULONG64)runtime->BlockedCount;
    response->registerStatus = runtime->RegisterStatus;
    response->engineStatus = runtime->EngineStatus;
    RtlCopyMemory(response->rules, runtime->Rules, sizeof(response->rules));
    ExReleasePushLockShared(&runtime->Lock);

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

BOOLEAN
KswordARKNetworkShouldHidePort(
    _In_ ULONG Protocol,
    _In_ USHORT LocalPort,
    _In_ USHORT RemotePort,
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    判断端口是否应在 R3/R0 查询结果中隐藏。中文说明：当前仓库尚未有 R0 TCP 表
    枚举模块，本函数先提供可复用策略入口，后续端口列表查询接入即可生效。

Arguments:

    Protocol - TCP/UDP 协议号。
    LocalPort - 本地端口。
    RemotePort - 远端端口。
    ProcessId - 进程 ID。

Return Value:

    TRUE 表示端口应隐藏；FALSE 表示正常显示。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    ULONG ruleIndex = 0UL;
    BOOLEAN shouldHide = FALSE;

    if ((runtime->RuntimeFlags & KSWORD_ARK_NETWORK_RUNTIME_PORT_HIDE) == 0UL) {
        return FALSE;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    for (ruleIndex = 0UL; ruleIndex < KSWORD_ARK_NETWORK_MAX_RULES; ++ruleIndex) {
        const KSWORD_ARK_NETWORK_RULE* rule = &runtime->Rules[ruleIndex];
        if (rule->action != KSWORD_ARK_NETWORK_RULE_ACTION_HIDE_PORT) {
            continue;
        }
        if (KswordARKNetworkRuleMatchesLocked(
            rule,
            KSWORD_ARK_NETWORK_DIRECTION_BOTH,
            Protocol,
            LocalPort,
            RemotePort,
            ProcessId)) {
            shouldHide = TRUE;
            break;
        }
    }
    ExReleasePushLockShared(&runtime->Lock);

    return shouldHide;
}
