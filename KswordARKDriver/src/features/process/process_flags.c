/*++

Module Name:

    process_flags.c

Abstract:

    Process BreakOnTermination and ETHREAD APC insertion controls.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../platform/process_resolver.h"

/* 中文说明：PsLookupProcessByProcessId 用于引用目标 EPROCESS。 */
NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

/* 中文说明：ProcessBreakOnTermination 是 ZwSetInformationProcess 的信息类 29。 */
#define KSWORD_ARK_PROCESS_INFORMATION_BREAK_ON_TERMINATION 29UL
/* 中文说明：EPROCESS.Flags 中 BreakOnTermination 对应 bit 13，SKT64 同样使用该位。 */
#define KSWORD_ARK_EPROCESS_FLAGS_BREAK_ON_TERMINATION_MASK 0x00002000UL
/* 中文说明：现代 Windows 10/11 x64 的 EPROCESS.Flags 常用偏移。 */
#define KSWORD_ARK_EPROCESS_FLAGS_OFFSET_WIN10_X64 0x464UL
/* 中文说明：旧 Windows 7 x64 兼容偏移，仅在版本探测明确命中时使用。 */
#define KSWORD_ARK_EPROCESS_FLAGS_OFFSET_WIN7_X64 0x1F4UL
/* 中文说明：Windows x64 ETHREAD.CrossThreadFlags/ApcQueueable 的保守偏移候选。 */
#define KSWORD_ARK_ETHREAD_APC_QUEUEABLE_OFFSET_X64 0x74UL
/* 中文说明：ApcQueueable 位在 CrossThreadFlags 中通常对应 bit 18。 */
#define KSWORD_ARK_ETHREAD_APC_QUEUEABLE_MASK 0x00040000UL

#ifndef PROCESS_SET_INFORMATION
/* 中文说明：旧 WDK 头可能没有用户态同名常量，按 ntifs/winnt 定义补齐。 */
#define PROCESS_SET_INFORMATION 0x0200
#endif

/* 中文说明：PsGetNextProcessThread 用于稳定遍历目标进程线程对象。 */
typedef PETHREAD(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

/* 中文说明：运行时解析 PsGetNextProcessThread，避免链接期依赖差异。 */
static KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKProcessFlagsResolvePsGetNextProcessThread(
    VOID
    )
{
    UNICODE_STRING routineName;

    /* 中文说明：名称来自 ntoskrnl 导出表；缺失时禁 APC 功能返回不支持。 */
    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    /* 中文说明：调用方检查 NULL，不在 resolver 内记录状态。 */
    return (KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

/* 中文说明：打开目标进程句柄，只用于 ZwSetInformationProcess 官方入口。 */
static NTSTATUS
KswordARKProcessFlagsOpenProcessHandle(
    _In_ ULONG ProcessId,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ HANDLE* ProcessHandleOut
    )
{
    CLIENT_ID clientId;
    OBJECT_ATTRIBUTES objectAttributes;
    NTSTATUS status = STATUS_SUCCESS;

    /* 中文说明：输出参数先清空，失败分支不会留下无效句柄。 */
    if (ProcessHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ProcessHandleOut = NULL;

    /* 中文说明：PID 0/4 等系统目标由 handler 统一挡住，这里再做一层保护。 */
    if (ProcessId == 0UL || ProcessId <= 4UL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* 中文说明：OBJ_KERNEL_HANDLE 确保句柄只在内核上下文可见。 */
    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(ProcessId);
    clientId.UniqueThread = NULL;

    /* 中文说明：PROCESS_SET_INFORMATION 足够设置 BreakOnTermination。 */
    status = ZwOpenProcess(
        ProcessHandleOut,
        DesiredAccess,
        &objectAttributes,
        &clientId);
    return status;
}

/* 中文说明：通过 ZwSetInformationProcess 设置或清除 BreakOnTermination。 */
static NTSTATUS
KswordARKProcessFlagsSetBreakOnTerminationByZw(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN EnableBreakOnTermination
    )
{
    HANDLE processHandle = NULL;
    ULONG breakValue = EnableBreakOnTermination ? 1UL : 0UL;
    KSWORD_ZW_SET_INFORMATION_PROCESS_FN zwSetInformationProcess = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* 中文说明：动态解析失败时不尝试 EPROCESS.Flags 硬编码写入。 */
    zwSetInformationProcess = KswordARKDriverResolveZwSetInformationProcess();
    if (zwSetInformationProcess == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    /* 中文说明：官方入口需要进程句柄，避免依赖未公开 EPROCESS 位布局。 */
    status = KswordARKProcessFlagsOpenProcessHandle(
        ProcessId,
        PROCESS_SET_INFORMATION,
        &processHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 中文说明：ZwSetInformationProcess 成功后目标进程 critical 标记立即生效。 */
    status = zwSetInformationProcess(
        processHandle,
        KSWORD_ARK_PROCESS_INFORMATION_BREAK_ON_TERMINATION,
        &breakValue,
        sizeof(breakValue));

    /* 中文说明：无论设置成功与否，内核句柄都必须关闭。 */
    ZwClose(processHandle);
    return status;
}

/* 中文说明：解析 EPROCESS.Flags 偏移；只在明确支持的 x64 构建上返回成功。 */
static NTSTATUS
KswordARKProcessFlagsResolveEprocessFlagsOffset(
    _Out_ ULONG* FlagsOffsetOut
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    RTL_OSVERSIONINFOW versionInfo;

    if (FlagsOffsetOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FlagsOffsetOut = 0UL;

#if defined(_M_X64)
    RtlZeroMemory(&versionInfo, sizeof(versionInfo));
    versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
    status = RtlGetVersion(&versionInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 中文说明：Windows 10/11 x64 的 Flags 偏移与 SKT64 当前路径一致。 */
    if (versionInfo.dwMajorVersion >= 10UL) {
        *FlagsOffsetOut = KSWORD_ARK_EPROCESS_FLAGS_OFFSET_WIN10_X64;
        return STATUS_SUCCESS;
    }

    /* 中文说明：仅对 Windows 7 x64 做保守兼容，其它 6.x 版本拒绝硬写。 */
    if (versionInfo.dwMajorVersion == 6UL && versionInfo.dwMinorVersion == 1UL) {
        *FlagsOffsetOut = KSWORD_ARK_EPROCESS_FLAGS_OFFSET_WIN7_X64;
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
#else
    /* 中文说明：非 x64 没有维护 EPROCESS.Flags 偏移表，不进入硬写路径。 */
    UNREFERENCED_PARAMETER(status);
    return STATUS_NOT_SUPPORTED;
#endif
}

/* 中文说明：通过直接写 EPROCESS.Flags 兜底设置 BreakOnTermination。 */
static NTSTATUS
KswordARKProcessFlagsSetBreakOnTerminationByEprocess(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN EnableBreakOnTermination
    )
{
    PEPROCESS processObject = NULL;
    ULONG flagsOffset = 0UL;
    volatile LONG* flagsAddress = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* 中文说明：偏移解析失败时不猜测，避免误写 EPROCESS 其它字段。 */
    status = KswordARKProcessFlagsResolveEprocessFlagsOffset(&flagsOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (flagsOffset == 0UL || flagsOffset > 0x1000UL) {
        return STATUS_NOT_SUPPORTED;
    }

    /* 中文说明：引用目标 EPROCESS，确保硬写期间对象不会被释放。 */
    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        flagsAddress = (volatile LONG*)((PUCHAR)processObject + flagsOffset);
        if (EnableBreakOnTermination) {
            (VOID)InterlockedOr(
                flagsAddress,
                (LONG)KSWORD_ARK_EPROCESS_FLAGS_BREAK_ON_TERMINATION_MASK);
        }
        else {
            (VOID)InterlockedAnd(
                flagsAddress,
                (LONG)(~KSWORD_ARK_EPROCESS_FLAGS_BREAK_ON_TERMINATION_MASK));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    /* 中文说明：硬写完成后释放目标 EPROCESS 引用。 */
    ObDereferenceObject(processObject);
    return status;
}

/* 中文说明：先走官方 ZwSetInformationProcess，失败再用 EPROCESS.Flags 兜底。 */
static NTSTATUS
KswordARKProcessFlagsSetBreakOnTermination(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN EnableBreakOnTermination
    )
{
    NTSTATUS zwStatus = STATUS_SUCCESS;
    NTSTATUS directStatus = STATUS_SUCCESS;

    zwStatus = KswordARKProcessFlagsSetBreakOnTerminationByZw(
        ProcessId,
        EnableBreakOnTermination);
    if (NT_SUCCESS(zwStatus)) {
        return zwStatus;
    }

    /* 中文说明：PPL/受限句柄路径可能拒绝 ZwOpenProcess，因此使用 R0 直写作为补强。 */
    directStatus = KswordARKProcessFlagsSetBreakOnTerminationByEprocess(
        ProcessId,
        EnableBreakOnTermination);
    if (NT_SUCCESS(directStatus)) {
        return directStatus;
    }

    /* 中文说明：优先保留官方路径失败原因；若官方入口缺失则返回兜底原因。 */
    if (zwStatus == STATUS_PROCEDURE_NOT_FOUND || zwStatus == STATUS_NOT_SUPPORTED) {
        return directStatus;
    }
    return zwStatus;
}

/* 中文说明：当前仅对 x64 使用经常见构建验证的 ETHREAD 偏移。 */
static NTSTATUS
KswordARKProcessFlagsResolveApcQueueableOffset(
    _Out_ ULONG* OffsetOut
    )
{
    /* 中文说明：输出为 ULONG 偏移，调用方会再次限制写入大小。 */
    if (OffsetOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *OffsetOut = 0UL;

#if defined(_M_X64)
    /* 中文说明：该偏移来自 ETHREAD.CrossThreadFlags；未知架构不使用。 */
    *OffsetOut = KSWORD_ARK_ETHREAD_APC_QUEUEABLE_OFFSET_X64;
    return STATUS_SUCCESS;
#else
    /* 中文说明：ARM64/x86 未维护偏移表，拒绝执行避免误写线程对象。 */
    return STATUS_NOT_SUPPORTED;
#endif
}

/* 中文说明：清除单个 ETHREAD 的 ApcQueueable 位，异常时返回异常码。 */
static NTSTATUS
KswordARKProcessFlagsClearThreadApcQueueable(
    _In_ PETHREAD ThreadObject,
    _In_ ULONG ApcQueueableOffset,
    _Out_ BOOLEAN* ChangedOut
    )
{
    volatile LONG* fieldAddress = NULL;
    LONG oldValue = 0;
    LONG newValue = 0;

    /* 中文说明：ChangedOut 告诉调用方本次是否实际从 1 变成 0。 */
    if (ThreadObject == NULL || ChangedOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ChangedOut = FALSE;

    /* 中文说明：偏移过大说明布局不可信，直接拒绝。 */
    if (ApcQueueableOffset > 0x1000UL) {
        return STATUS_NOT_SUPPORTED;
    }

    __try {
        /* 中文说明：字段按 LONG 原子位清除，避免覆盖并发设置的其它位。 */
        fieldAddress = (volatile LONG*)((PUCHAR)ThreadObject + ApcQueueableOffset);
        oldValue = InterlockedAnd(fieldAddress, (LONG)(~KSWORD_ARK_ETHREAD_APC_QUEUEABLE_MASK));
        newValue = oldValue & (LONG)(~KSWORD_ARK_ETHREAD_APC_QUEUEABLE_MASK);
        *ChangedOut = ((oldValue ^ newValue) & (LONG)KSWORD_ARK_ETHREAD_APC_QUEUEABLE_MASK) != 0 ? TRUE : FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

/* 中文说明：遍历目标进程线程并清除每个线程的 ApcQueueable 位。 */
static NTSTATUS
KswordARKProcessFlagsDisableApcInsertion(
    _In_ ULONG ProcessId,
    _Out_ ULONG* TouchedThreadCountOut
    )
{
    PEPROCESS processObject = NULL;
    PETHREAD threadCursor = NULL;
    ULONG apcQueueableOffset = 0UL;
    ULONG touchedThreadCount = 0UL;
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS lastFailureStatus = STATUS_SUCCESS;

    /* 中文说明：输出线程计数用于 UI 展示本次影响范围。 */
    if (TouchedThreadCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *TouchedThreadCountOut = 0UL;

    /* 中文说明：先解析偏移，未知平台不进入对象写路径。 */
    status = KswordARKProcessFlagsResolveApcQueueableOffset(&apcQueueableOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 中文说明：线程枚举依赖 PsGetNextProcessThread；不做 PID 猜扫。 */
    psGetNextProcessThread = KswordARKProcessFlagsResolvePsGetNextProcessThread();
    if (psGetNextProcessThread == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    /* 中文说明：引用目标 EPROCESS，确保线程枚举期间进程对象有效。 */
    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 中文说明：PsGetNextProcessThread 返回带引用的 ETHREAD，循环内必须释放。 */
    threadCursor = psGetNextProcessThread(processObject, NULL);
    while (threadCursor != NULL) {
        PETHREAD nextThread = psGetNextProcessThread(processObject, threadCursor);
        BOOLEAN changed = FALSE;
        NTSTATUS threadStatus = STATUS_SUCCESS;

        /* 中文说明：对每个线程独立 SEH，单线程异常不阻断剩余线程处理。 */
        threadStatus = KswordARKProcessFlagsClearThreadApcQueueable(
            threadCursor,
            apcQueueableOffset,
            &changed);
        if (NT_SUCCESS(threadStatus)) {
            if (changed && touchedThreadCount != MAXULONG) {
                touchedThreadCount += 1UL;
            }
        }
        else {
            lastFailureStatus = threadStatus;
        }

        /* 中文说明：释放当前线程引用后推进到下一项。 */
        ObDereferenceObject(threadCursor);
        threadCursor = nextThread;
    }

    /* 中文说明：释放进程引用，线程枚举已经结束。 */
    ObDereferenceObject(processObject);
    *TouchedThreadCountOut = touchedThreadCount;

    /* 中文说明：全部线程都失败时返回最后失败；部分成功由响应 status 表达。 */
    if (touchedThreadCount == 0UL && !NT_SUCCESS(lastFailureStatus)) {
        return lastFailureStatus;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverSetProcessSpecialFlags(
    _In_ ULONG ProcessId,
    _In_ ULONG Action,
    _In_ ULONG Flags,
    _Out_ ULONG* OperationStatusOut,
    _Out_ ULONG* AppliedFlagsOut,
    _Out_ ULONG* TouchedThreadCountOut
    )
/*++

Routine Description:

    Apply dangerous process special flags from R3. 中文说明：当前支持
    BreakOnTermination 开/关，以及清除目标进程现有线程的 APC 插入许可位。

Arguments:

    ProcessId - 目标 PID。
    Action - KSWORD_ARK_PROCESS_SPECIAL_ACTION_*。
    Flags - 预留策略位，当前只记录不改变语义。
    OperationStatusOut - 返回协议状态。
    AppliedFlagsOut - 返回已经应用的语义 flag。
    TouchedThreadCountOut - 返回禁 APC 时实际改变的线程数。

Return Value:

    STATUS_SUCCESS 表示动作完成；失败返回底层 NTSTATUS。

--*/
{
    ULONG touchedThreadCount = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    /* 中文说明：Flags 当前预留，显式标记避免 W4 未引用告警。 */
    UNREFERENCED_PARAMETER(Flags);

    if (OperationStatusOut == NULL ||
        AppliedFlagsOut == NULL ||
        TouchedThreadCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNKNOWN;
    *AppliedFlagsOut = 0UL;
    *TouchedThreadCountOut = 0UL;

    if (ProcessId == 0UL || ProcessId <= 4UL) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_LOOKUP_FAILED;
        return STATUS_INVALID_PARAMETER;
    }

    if (Action == KSWORD_ARK_PROCESS_SPECIAL_ACTION_ENABLE_BREAK_ON_TERMINATION ||
        Action == KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_BREAK_ON_TERMINATION) {
        const BOOLEAN enableBreak =
            (Action == KSWORD_ARK_PROCESS_SPECIAL_ACTION_ENABLE_BREAK_ON_TERMINATION) ? TRUE : FALSE;

        /* 中文说明：优先使用 ZwSetInformationProcess；失败时用 EPROCESS.Flags 兜底。 */
        status = KswordARKProcessFlagsSetBreakOnTermination(ProcessId, enableBreak);
        if (NT_SUCCESS(status)) {
            *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED;
            if (enableBreak) {
                *AppliedFlagsOut |= KSWORD_ARK_PROCESS_SPECIAL_FLAG_BREAK_ON_TERMINATION;
            }
        }
        else if (status == STATUS_PROCEDURE_NOT_FOUND || status == STATUS_NOT_SUPPORTED) {
            *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNSUPPORTED;
        }
        else {
            *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_OPERATION_FAILED;
        }
        return status;
    }

    if (Action == KSWORD_ARK_PROCESS_SPECIAL_ACTION_DISABLE_APC_INSERTION) {
        /* 中文说明：禁 APC 插入是线程级批量写，返回改变线程数量供 R3 审计。 */
        status = KswordARKProcessFlagsDisableApcInsertion(ProcessId, &touchedThreadCount);
        *TouchedThreadCountOut = touchedThreadCount;
        if (NT_SUCCESS(status)) {
            *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_APPLIED;
            *AppliedFlagsOut |= KSWORD_ARK_PROCESS_SPECIAL_FLAG_APC_INSERT_DISABLED;
        }
        else if (status == STATUS_PROCEDURE_NOT_FOUND || status == STATUS_NOT_SUPPORTED) {
            *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_UNSUPPORTED;
        }
        else {
            *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_OPERATION_FAILED;
        }
        return status;
    }

    *OperationStatusOut = KSWORD_ARK_PROCESS_SPECIAL_STATUS_OPERATION_FAILED;
    return STATUS_INVALID_PARAMETER;
}
