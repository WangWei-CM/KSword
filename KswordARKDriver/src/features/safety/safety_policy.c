/*++

Module Name:

    safety_policy.c

Abstract:

    Phase-15 centralized dangerous-operation safety policy.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntstrsafe.h>

#ifndef STATUS_REQUEST_NOT_ACCEPTED
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

#define KSWORD_ARK_SAFETY_MAX_USER_PID 0xFFFFFFF0UL

typedef struct _KSWORD_ARK_SAFETY_STATE
{
    EX_PUSH_LOCK Lock;
    ULONG PolicyFlags;
    ULONG Generation;
    ULONG LastOperation;
    ULONG LastDecision;
    ULONG LastReason;
    ULONG LastRiskLevel;
    ULONG LastTargetProcessId;
    NTSTATUS LastStatus;
    ULONGLONG AllowedCount;
    ULONGLONG DeniedCount;
    ULONGLONG AuditOnlyCount;
    WCHAR LastTargetText[KSWORD_ARK_SAFETY_TEXT_MAX_CHARS];
} KSWORD_ARK_SAFETY_STATE;

static KSWORD_ARK_SAFETY_STATE g_KswordArkSafetyState;

static VOID
KswordARKSafetyCopyWideText(
    _Out_writes_(DestinationChars) PWSTR Destination,
    _In_ USHORT DestinationChars,
    _In_reads_opt_(SourceChars) PCWSTR Source,
    _In_ USHORT SourceChars
    )
/*++

Routine Description:

    复制 safety 目标文本。中文说明：所有审计文本固定长度且强制 NUL 结尾，
    防止日志/响应解析越界。

Arguments:

    Destination - 目标缓冲。
    DestinationChars - 目标字符容量。
    Source - 来源文本，可为空。
    SourceChars - 来源字符数。

Return Value:

    None. 本函数没有返回值。

--*/
{
    USHORT copyChars = 0U;

    if (Destination == NULL || DestinationChars == 0U) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL || SourceChars == 0U) {
        return;
    }

    copyChars = SourceChars;
    if (copyChars >= DestinationChars) {
        copyChars = DestinationChars - 1U;
    }
    RtlCopyMemory(Destination, Source, (SIZE_T)copyChars * sizeof(WCHAR));
    Destination[copyChars] = L'\0';
}

static ULONG
KswordARKSafetyRequiredPolicyFlagForOperation(
    _In_ ULONG Operation
    )
/*++

Routine Description:

    将危险操作映射到策略允许位。中文说明：所有 feature 不再自己判断策略，
    必须通过这里获得统一开关语义。

Arguments:

    Operation - KSWORD_ARK_SAFETY_OPERATION_*。

Return Value:

    对应 KSWORD_ARK_SAFETY_POLICY_FLAG_*；未知操作返回 0。

--*/
{
    switch (Operation) {
    case KSWORD_ARK_SAFETY_OPERATION_PROCESS_TERMINATE:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_PROCESS_TERMINATE;
    case KSWORD_ARK_SAFETY_OPERATION_PROCESS_SUSPEND:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_PROCESS_SUSPEND;
    case KSWORD_ARK_SAFETY_OPERATION_PROCESS_SET_PROTECTION:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_PROCESS_PROTECTION;
    case KSWORD_ARK_SAFETY_OPERATION_FILE_DELETE:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_FILE_DELETE;
    case KSWORD_ARK_SAFETY_OPERATION_CALLBACK_SET_RULES:
    case KSWORD_ARK_SAFETY_OPERATION_CALLBACK_REMOVE_EXTERNAL:
    case KSWORD_ARK_SAFETY_OPERATION_CALLBACK_CANCEL_PENDING:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_CALLBACK_CONTROL;
    case KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_MEMORY_WRITE;
    case KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_KERNEL_PATCH;
    case KSWORD_ARK_SAFETY_OPERATION_DRIVER_UNLOAD:
        return KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_DRIVER_UNLOAD;
    default:
        return 0UL;
    }
}

static ULONG
KswordARKSafetyRiskForOperation(
    _In_ ULONG Operation
    )
/*++

Routine Description:

    给危险操作分配默认风险等级。中文说明：高风险操作需要 UI 确认或 legacy
    兼容确认位，后续 R3 接入显式确认后可以关闭 legacy 兼容。

Arguments:

    Operation - KSWORD_ARK_SAFETY_OPERATION_*。

Return Value:

    KSWORD_ARK_SAFETY_RISK_*。

--*/
{
    switch (Operation) {
    case KSWORD_ARK_SAFETY_OPERATION_PROCESS_TERMINATE:
    case KSWORD_ARK_SAFETY_OPERATION_PROCESS_SET_PROTECTION:
    case KSWORD_ARK_SAFETY_OPERATION_CALLBACK_REMOVE_EXTERNAL:
    case KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE:
    case KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH:
    case KSWORD_ARK_SAFETY_OPERATION_DRIVER_UNLOAD:
        return KSWORD_ARK_SAFETY_RISK_HIGH;
    case KSWORD_ARK_SAFETY_OPERATION_FILE_DELETE:
    case KSWORD_ARK_SAFETY_OPERATION_PROCESS_SUSPEND:
    case KSWORD_ARK_SAFETY_OPERATION_CALLBACK_SET_RULES:
        return KSWORD_ARK_SAFETY_RISK_MEDIUM;
    case KSWORD_ARK_SAFETY_OPERATION_CALLBACK_CANCEL_PENDING:
        return KSWORD_ARK_SAFETY_RISK_LOW;
    default:
        return KSWORD_ARK_SAFETY_RISK_CRITICAL;
    }
}

static BOOLEAN
KswordARKSafetyIsCriticalProcessId(
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    判断目标 PID 是否属于默认禁止的关键范围。中文说明：第一版采用稳定的
    系统 PID 保护，避免误杀 Idle/System/会话管理等核心进程。

Arguments:

    ProcessId - 目标 PID。

Return Value:

    TRUE 表示默认拒绝。

--*/
{
    if (ProcessId == 0UL || ProcessId == 4UL) {
        return TRUE;
    }
    if (ProcessId > KSWORD_ARK_SAFETY_MAX_USER_PID) {
        return TRUE;
    }
    return FALSE;
}

static VOID
KswordARKSafetyRecordDecisionLocked(
    _In_ const KSWORD_ARK_SAFETY_CONTEXT* Context,
    _In_ ULONG Decision,
    _In_ ULONG Reason,
    _In_ ULONG RiskLevel,
    _In_ NTSTATUS Status
    )
/*++

Routine Description:

    记录最近一次 safety 决策。中文说明：调用方已经持有写锁，因此本函数只做
    结构更新，不再二次加锁。

Arguments:

    Context - 操作上下文。
    Decision - allow/deny/audit-only。
    Reason - 统一原因码。
    RiskLevel - 风险等级。
    Status - 返回给调用方的状态码。

Return Value:

    None. 本函数没有返回值。

--*/
{
    g_KswordArkSafetyState.LastOperation = Context->Operation;
    g_KswordArkSafetyState.LastDecision = Decision;
    g_KswordArkSafetyState.LastReason = Reason;
    g_KswordArkSafetyState.LastRiskLevel = RiskLevel;
    g_KswordArkSafetyState.LastTargetProcessId = Context->TargetProcessId;
    g_KswordArkSafetyState.LastStatus = Status;
    KswordARKSafetyCopyWideText(
        g_KswordArkSafetyState.LastTargetText,
        KSWORD_ARK_SAFETY_TEXT_MAX_CHARS,
        Context->TargetText,
        Context->TargetTextChars);

    if (Decision == KSWORD_ARK_SAFETY_DECISION_DENY) {
        g_KswordArkSafetyState.DeniedCount += 1ULL;
    }
    else if (Decision == KSWORD_ARK_SAFETY_DECISION_AUDIT_ONLY_ALLOW) {
        g_KswordArkSafetyState.AuditOnlyCount += 1ULL;
    }
    else {
        g_KswordArkSafetyState.AllowedCount += 1ULL;
    }
}

static VOID
KswordARKSafetyLogDecision(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_SAFETY_CONTEXT* Context,
    _In_ ULONG Decision,
    _In_ ULONG Reason,
    _In_ ULONG RiskLevel,
    _In_ NTSTATUS Status
    )
/*++

Routine Description:

    输出 safety 决策审计日志。中文说明：日志集中在策略模块生成，feature 层
    不再各自拼接策略原因。

Arguments:

    Device - WDF 设备对象。
    Context - 操作上下文。
    Decision - 决策结果。
    Reason - 原因码。
    RiskLevel - 风险等级。
    Status - 返回状态。

Return Value:

    None. 本函数没有返回值。

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    PCSTR levelText = (Decision == KSWORD_ARK_SAFETY_DECISION_DENY) ? "Warn" : "Info";

    (VOID)RtlStringCbPrintfA(
        logMessage,
        sizeof(logMessage),
        "Safety decision: op=%lu, pid=%lu, risk=%lu, decision=%lu, reason=%lu, status=0x%08X.",
        (unsigned long)Context->Operation,
        (unsigned long)Context->TargetProcessId,
        (unsigned long)RiskLevel,
        (unsigned long)Decision,
        (unsigned long)Reason,
        (unsigned int)Status);
    (VOID)KswordARKDriverEnqueueLogFrame(Device, levelText, logMessage);
}

VOID
KswordARKSafetyInitialize(
    VOID
    )
/*++

Routine Description:

    初始化中央 safety policy。中文说明：默认开启高级模式和 legacy 未显式确认
    兼容，以保证现有 R3 未施工完成时不破坏工作流，同时所有操作仍进入审计。

Arguments:

    None.

Return Value:

    None. 本函数没有返回值。

--*/
{
    RtlZeroMemory(&g_KswordArkSafetyState, sizeof(g_KswordArkSafetyState));
    ExInitializePushLock(&g_KswordArkSafetyState.Lock);
    g_KswordArkSafetyState.PolicyFlags = KSWORD_ARK_SAFETY_POLICY_FLAG_DEFAULT;
    g_KswordArkSafetyState.Generation = 1UL;
    g_KswordArkSafetyState.LastStatus = STATUS_SUCCESS;
}

NTSTATUS
KswordARKSafetyEvaluate(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_SAFETY_CONTEXT* Context
    )
/*++

Routine Description:

    统一评估危险操作是否允许。中文说明：所有 mutating IOCTL handler 在执行
    实际动作前必须调用本函数；拒绝时返回失败状态并写入审计日志。

Arguments:

    Device - WDF 设备对象，用于日志。
    Context - 危险操作上下文。

Return Value:

    STATUS_SUCCESS 表示允许；失败表示策略拒绝。

--*/
{
    ULONG policyFlags = 0UL;
    ULONG requiredFlag = 0UL;
    ULONG decision = KSWORD_ARK_SAFETY_DECISION_ALLOW;
    ULONG reason = KSWORD_ARK_SAFETY_REASON_NONE;
    ULONG riskLevel = KSWORD_ARK_SAFETY_RISK_CRITICAL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Context == NULL || Context->Operation == KSWORD_ARK_SAFETY_OPERATION_NONE) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquirePushLockExclusive(&g_KswordArkSafetyState.Lock);
    policyFlags = g_KswordArkSafetyState.PolicyFlags;
    requiredFlag = KswordARKSafetyRequiredPolicyFlagForOperation(Context->Operation);
    riskLevel = KswordARKSafetyRiskForOperation(Context->Operation);

    if ((policyFlags & KSWORD_ARK_SAFETY_POLICY_FLAG_ACTIVE) == 0UL) {
        decision = KSWORD_ARK_SAFETY_DECISION_DENY;
        reason = KSWORD_ARK_SAFETY_REASON_POLICY_INACTIVE;
        status = STATUS_ACCESS_DENIED;
    }
    else if (requiredFlag == 0UL || (policyFlags & requiredFlag) == 0UL) {
        decision = KSWORD_ARK_SAFETY_DECISION_DENY;
        reason = KSWORD_ARK_SAFETY_REASON_OPERATION_DISABLED;
        status = STATUS_ACCESS_DENIED;
    }
    else if ((policyFlags & KSWORD_ARK_SAFETY_POLICY_FLAG_ADVANCED_MODE) == 0UL) {
        decision = KSWORD_ARK_SAFETY_DECISION_DENY;
        reason = KSWORD_ARK_SAFETY_REASON_ADVANCED_MODE_REQUIRED;
        status = STATUS_ACCESS_DENIED;
    }
    else if ((policyFlags & KSWORD_ARK_SAFETY_POLICY_FLAG_DENY_CRITICAL_PROCESS) != 0UL &&
        Context->TargetProcessId != 0UL &&
        KswordARKSafetyIsCriticalProcessId(Context->TargetProcessId)) {
        decision = KSWORD_ARK_SAFETY_DECISION_DENY;
        reason = KSWORD_ARK_SAFETY_REASON_CRITICAL_PROCESS_DENIED;
        status = STATUS_ACCESS_DENIED;
    }
    else if ((policyFlags & KSWORD_ARK_SAFETY_POLICY_FLAG_REQUIRE_CONFIRMATION_HIGH_RISK) != 0UL &&
        riskLevel >= KSWORD_ARK_SAFETY_RISK_HIGH &&
        (Context->ContextFlags & KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED) == 0UL &&
        (policyFlags & KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_LEGACY_UNCONFIRMED_R3) == 0UL) {
        decision = KSWORD_ARK_SAFETY_DECISION_DENY;
        reason = KSWORD_ARK_SAFETY_REASON_CONFIRMATION_REQUIRED;
        status = STATUS_REQUEST_NOT_ACCEPTED;
    }
    else if ((policyFlags & KSWORD_ARK_SAFETY_POLICY_FLAG_AUDIT_ONLY) != 0UL) {
        decision = KSWORD_ARK_SAFETY_DECISION_AUDIT_ONLY_ALLOW;
        reason = KSWORD_ARK_SAFETY_REASON_NONE;
        status = STATUS_SUCCESS;
    }

    KswordARKSafetyRecordDecisionLocked(Context, decision, reason, riskLevel, status);
    ExReleasePushLockExclusive(&g_KswordArkSafetyState.Lock);

    KswordARKSafetyLogDecision(Device, Context, decision, reason, riskLevel, status);
    return status;
}

NTSTATUS
KswordARKSafetyQueryPolicy(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    生成当前 safety policy 响应。中文说明：该查询供 R3 状态页显示高级模式、
    最近一次危险操作和允许/拒绝计数。

Arguments:

    OutputBuffer - 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    BytesWrittenOut - 写入字节数。

Return Value:

    STATUS_SUCCESS 或缓冲校验状态。

--*/
{
    KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE* response = NULL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_SAFETY_PROTOCOL_VERSION;
    response->defaultPolicyFlags = KSWORD_ARK_SAFETY_POLICY_FLAG_DEFAULT;

    ExAcquirePushLockShared(&g_KswordArkSafetyState.Lock);
    response->policyFlags = g_KswordArkSafetyState.PolicyFlags;
    response->policyGeneration = g_KswordArkSafetyState.Generation;
    response->lastOperation = g_KswordArkSafetyState.LastOperation;
    response->lastDecision = g_KswordArkSafetyState.LastDecision;
    response->lastReason = g_KswordArkSafetyState.LastReason;
    response->lastRiskLevel = g_KswordArkSafetyState.LastRiskLevel;
    response->lastTargetProcessId = g_KswordArkSafetyState.LastTargetProcessId;
    response->lastStatus = g_KswordArkSafetyState.LastStatus;
    response->allowedCount = g_KswordArkSafetyState.AllowedCount;
    response->deniedCount = g_KswordArkSafetyState.DeniedCount;
    response->auditOnlyCount = g_KswordArkSafetyState.AuditOnlyCount;
    RtlCopyMemory(
        response->lastTargetText,
        g_KswordArkSafetyState.LastTargetText,
        sizeof(response->lastTargetText));
    ExReleasePushLockShared(&g_KswordArkSafetyState.Lock);

    response->lastTargetText[KSWORD_ARK_SAFETY_TEXT_MAX_CHARS - 1U] = L'\0';
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKSafetySetPolicy(
    _In_ const KSWORD_ARK_SET_SAFETY_POLICY_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    原子更新 safety policy 位。中文说明：R3 可只设置/清除特定位；expectedGeneration
    非零时用于避免覆盖并发更新。

Arguments:

    Request - 设置请求。
    OutputBuffer - 响应缓冲。
    OutputBufferLength - 响应缓冲长度。
    BytesWrittenOut - 写入字节数。

Return Value:

    STATUS_SUCCESS 或校验/并发状态。

--*/
{
    KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE* response = NULL;
    ULONG oldFlags = 0UL;
    ULONG oldGeneration = 0UL;
    ULONG allowedMask =
        KSWORD_ARK_SAFETY_POLICY_FLAG_ACTIVE |
        KSWORD_ARK_SAFETY_POLICY_FLAG_ADVANCED_MODE |
        KSWORD_ARK_SAFETY_POLICY_FLAG_REQUIRE_CONFIRMATION_HIGH_RISK |
        KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_LEGACY_UNCONFIRMED_R3 |
        KSWORD_ARK_SAFETY_POLICY_FLAG_DENY_CRITICAL_PROCESS |
        KSWORD_ARK_SAFETY_POLICY_FLAG_MUTATING_DEFAULTS |
        KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_MEMORY_WRITE |
        KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_KERNEL_PATCH |
        KSWORD_ARK_SAFETY_POLICY_FLAG_ALLOW_DRIVER_UNLOAD |
        KSWORD_ARK_SAFETY_POLICY_FLAG_AUDIT_ONLY;
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL || OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->size < sizeof(KSWORD_ARK_SET_SAFETY_POLICY_REQUEST) ||
        Request->version != KSWORD_ARK_SAFETY_PROTOCOL_VERSION ||
        ((Request->setFlags | Request->clearFlags) & ~allowedMask) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_SAFETY_PROTOCOL_VERSION;

    ExAcquirePushLockExclusive(&g_KswordArkSafetyState.Lock);
    oldFlags = g_KswordArkSafetyState.PolicyFlags;
    oldGeneration = g_KswordArkSafetyState.Generation;
    if (Request->expectedGeneration != 0UL &&
        Request->expectedGeneration != oldGeneration) {
        status = STATUS_REVISION_MISMATCH;
    }
    else {
        g_KswordArkSafetyState.PolicyFlags |= (Request->setFlags & allowedMask);
        g_KswordArkSafetyState.PolicyFlags &= ~(Request->clearFlags & allowedMask);
        g_KswordArkSafetyState.Generation += 1UL;
    }
    response->oldPolicyFlags = oldFlags;
    response->newPolicyFlags = g_KswordArkSafetyState.PolicyFlags;
    response->oldGeneration = oldGeneration;
    response->newGeneration = g_KswordArkSafetyState.Generation;
    response->status = status;
    ExReleasePushLockExclusive(&g_KswordArkSafetyState.Lock);

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
