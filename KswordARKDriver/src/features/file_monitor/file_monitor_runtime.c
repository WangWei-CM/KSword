/*++

Module Name:

    file_monitor_runtime.c

Abstract:

    Phase-12 file-system minifilter runtime and event ring buffer.

Environment:

    Kernel-mode minifilter + KMDF control device

--*/

#include <fltKernel.h>
#include "ark/ark_driver.h"

#include <ntstrsafe.h>

#define KSWORD_ARK_FILE_MONITOR_TAG 'mFsK'

typedef struct _KSWORD_ARK_FILE_MONITOR_RUNTIME
{
    PFLT_FILTER Filter;
    WDFDEVICE Device;
    KSPIN_LOCK RingLock;
    ULONG HeadIndex;
    ULONG TailIndex;
    ULONG QueuedCount;
    ULONG DroppedCount;
    ULONG OperationMask;
    ULONG ProcessIdFilter;
    ULONG RuntimeFlags;
    LONG64 Sequence;
    NTSTATUS RegisterStatus;
    NTSTATUS StartStatus;
    NTSTATUS LastErrorStatus;
    KSWORD_ARK_FILE_MONITOR_EVENT Ring[KSWORD_ARK_FILE_MONITOR_RING_CAPACITY];
} KSWORD_ARK_FILE_MONITOR_RUNTIME;

static KSWORD_ARK_FILE_MONITOR_RUNTIME g_KswordArkFileMonitorRuntime;

static NTSTATUS FLTAPI
KswordARKFileMonitorUnloadCallback(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

static FLT_PREOP_CALLBACK_STATUS FLTAPI
KswordARKFileMonitorPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
    );

static FLT_POSTOP_CALLBACK_STATUS FLTAPI
KswordARKFileMonitorPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

static const FLT_OPERATION_REGISTRATION g_KswordArkFileMonitorOperations[] =
{
    { IRP_MJ_CREATE, 0, KswordARKFileMonitorPreOperation, KswordARKFileMonitorPostOperation },
    { IRP_MJ_READ, 0, KswordARKFileMonitorPreOperation, NULL },
    { IRP_MJ_WRITE, 0, KswordARKFileMonitorPreOperation, NULL },
    { IRP_MJ_SET_INFORMATION, 0, KswordARKFileMonitorPreOperation, KswordARKFileMonitorPostOperation },
    { IRP_MJ_CLEANUP, 0, KswordARKFileMonitorPreOperation, NULL },
    { IRP_MJ_CLOSE, 0, KswordARKFileMonitorPreOperation, NULL },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION g_KswordArkFileMonitorRegistration =
{
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    NULL,
    g_KswordArkFileMonitorOperations,
    KswordARKFileMonitorUnloadCallback,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

static ULONG
KswordARKFileMonitorAdvanceIndex(
    _In_ ULONG CurrentIndex
    )
/*++

Routine Description:

    推进 ring buffer 下标。中文说明：容量固定为 2^n 不做假设，统一用取模保持
    可读性和后续容量调整安全。

Arguments:

    CurrentIndex - 当前下标。

Return Value:

    下一个下标。

--*/
{
    return (CurrentIndex + 1UL) % KSWORD_ARK_FILE_MONITOR_RING_CAPACITY;
}

static VOID
KswordARKFileMonitorLog(
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR MessageText
    )
/*++

Routine Description:

    写入文件监控运行时日志。中文说明：minifilter 早期初始化时 Device 可能为空，
    此时只跳过 WDF 日志，不影响 FltMgr 注册。

Arguments:

    LevelText - 日志等级。
    MessageText - 日志正文。

Return Value:

    None.

--*/
{
    WDFDEVICE device = g_KswordArkFileMonitorRuntime.Device;

    if (device == WDF_NO_HANDLE || device == NULL) {
        return;
    }

    (VOID)KswordARKDriverEnqueueLogFrame(
        device,
        LevelText != NULL ? LevelText : "Info",
        MessageText != NULL ? MessageText : "");
}

static ULONG
KswordARKFileMonitorMapMajorToOperation(
    _In_ UCHAR MajorFunction,
    _In_ UCHAR MinorFunction,
    _In_opt_ PFLT_PARAMETERS Parameters
    )
/*++

Routine Description:

    将 IRP major/minor 映射为协议 operation mask。中文说明：Rename/Delete 都从
    IRP_MJ_SET_INFORMATION 细分，无法识别时保留 SETINFO 语义。

Arguments:

    MajorFunction - IRP major function。
    MinorFunction - IRP minor function。
    Parameters - 当前 I/O 参数，可为空。

Return Value:

    KSWORD_ARK_FILE_MONITOR_OPERATION_* 位。

--*/
{
    UNREFERENCED_PARAMETER(MinorFunction);

    switch (MajorFunction) {
    case IRP_MJ_CREATE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_CREATE;
    case IRP_MJ_READ:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_READ;
    case IRP_MJ_WRITE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_WRITE;
    case IRP_MJ_CLEANUP:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_CLEANUP;
    case IRP_MJ_CLOSE:
        return KSWORD_ARK_FILE_MONITOR_OPERATION_CLOSE;
    case IRP_MJ_SET_INFORMATION:
        if (Parameters != NULL) {
            FILE_INFORMATION_CLASS informationClass = Parameters->SetFileInformation.FileInformationClass;
            if (informationClass == FileRenameInformation ||
                informationClass == FileRenameInformationEx ||
                informationClass == FileLinkInformation ||
                informationClass == FileLinkInformationEx) {
                return KSWORD_ARK_FILE_MONITOR_OPERATION_RENAME;
            }
            if (informationClass == FileDispositionInformation ||
                informationClass == FileDispositionInformationEx) {
                return KSWORD_ARK_FILE_MONITOR_OPERATION_DELETE;
            }
        }
        return KSWORD_ARK_FILE_MONITOR_OPERATION_SETINFO;
    default:
        return 0UL;
    }
}

static VOID
KswordARKFileMonitorCopyNameToEvent(
    _Inout_ KSWORD_ARK_FILE_MONITOR_EVENT* Event,
    _In_opt_ PCUNICODE_STRING FileName
    )
/*++

Routine Description:

    将 FltMgr 返回的 normalized/opened name 复制到事件。中文说明：固定数组永远
    尾零，超长路径显式标记 TRUNCATED，避免 R3 误以为完整。

Arguments:

    Event - 待填充事件。
    FileName - 文件名 UNICODE_STRING，可为空。

Return Value:

    None.

--*/
{
    ULONG sourceChars = 0UL;
    ULONG charsToCopy = 0UL;

    if (Event == NULL || FileName == NULL || FileName->Buffer == NULL || FileName->Length == 0U) {
        return;
    }

    sourceChars = (ULONG)(FileName->Length / sizeof(WCHAR));
    charsToCopy = sourceChars;
    if (charsToCopy >= KSWORD_ARK_FILE_MONITOR_PATH_CHARS) {
        charsToCopy = KSWORD_ARK_FILE_MONITOR_PATH_CHARS - 1UL;
        Event->fieldFlags |= KSWORD_ARK_FILE_MONITOR_FIELD_PATH_TRUNCATED;
    }

    if (charsToCopy > 0UL) {
        RtlCopyMemory(
            Event->path,
            FileName->Buffer,
            (SIZE_T)charsToCopy * sizeof(WCHAR));
        Event->path[charsToCopy] = L'\0';
        Event->pathLengthChars = charsToCopy;
        Event->fieldFlags |= KSWORD_ARK_FILE_MONITOR_FIELD_PATH_PRESENT;
    }
}

static VOID
KswordARKFileMonitorFillCommonEvent(
    _Inout_ KSWORD_ARK_FILE_MONITOR_EVENT* Event,
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ ULONG OperationType,
    _In_ BOOLEAN IsPostOperation
    )
/*++

Routine Description:

    填充文件事件公共字段。中文说明：这里不做阻断，只采集展示所需字段；路径
    解析失败时仍保留 PID、MajorFunction、FileObject 等上下文。

Arguments:

    Event - 输出事件。
    Data - FltMgr callback data。
    FltObjects - FltMgr related objects。
    OperationType - 协议 operation type。
    IsPostOperation - TRUE 表示 post 回调记录结果。

Return Value:

    None.

--*/
{
    PFLT_FILE_NAME_INFORMATION nameInformation = NULL;
    NTSTATUS nameStatus = STATUS_SUCCESS;

    RtlZeroMemory(Event, sizeof(*Event));
    Event->version = KSWORD_ARK_FILE_MONITOR_PROTOCOL_VERSION;
    Event->size = sizeof(*Event);
    Event->operationType = OperationType;
    Event->majorFunction = Data->Iopb->MajorFunction;
    Event->minorFunction = Data->Iopb->MinorFunction;
    Event->processId = (ULONG)(ULONG_PTR)FltGetRequestorProcessId(Data);
    Event->threadId = HandleToULong(PsGetCurrentThreadId());
    Event->sequence = (ULONG64)InterlockedIncrement64(&g_KswordArkFileMonitorRuntime.Sequence);
    KeQuerySystemTimePrecise((PLARGE_INTEGER)&Event->timeUtc100ns);

    if (FltObjects != NULL && FltObjects->FileObject != NULL) {
        Event->fileObjectAddress = (ULONG64)(ULONG_PTR)FltObjects->FileObject;
    }

    if (IsPostOperation) {
        Event->resultStatus = Data->IoStatus.Status;
        Event->fieldFlags |=
            KSWORD_ARK_FILE_MONITOR_FIELD_RESULT_PRESENT |
            KSWORD_ARK_FILE_MONITOR_FIELD_POST_OPERATION;
    }

    if (Event->processId <= 4UL) {
        Event->fieldFlags |= KSWORD_ARK_FILE_MONITOR_FIELD_SYSTEM_PROCESS;
    }

    if (Data->Iopb->MajorFunction == IRP_MJ_CREATE) {
        Event->desiredAccess = Data->Iopb->Parameters.Create.SecurityContext != NULL ?
            Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess :
            0UL;
        Event->shareAccess = Data->Iopb->Parameters.Create.ShareAccess;
        Event->createOptions = Data->Iopb->Parameters.Create.Options;
        Event->fieldFlags |= KSWORD_ARK_FILE_MONITOR_FIELD_ACCESS_PRESENT;
    }
    else if (Data->Iopb->MajorFunction == IRP_MJ_SET_INFORMATION) {
        Event->fileInformationClass = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    }

    nameStatus = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInformation);
    if (NT_SUCCESS(nameStatus) && nameInformation != NULL) {
        (VOID)FltParseFileNameInformation(nameInformation);
        KswordARKFileMonitorCopyNameToEvent(Event, &nameInformation->Name);
    }
    else if (FltObjects != NULL &&
        FltObjects->FileObject != NULL &&
        FltObjects->FileObject->FileName.Buffer != NULL) {
        KswordARKFileMonitorCopyNameToEvent(Event, &FltObjects->FileObject->FileName);
    }

    if (nameInformation != NULL) {
        FltReleaseFileNameInformation(nameInformation);
    }
}

static VOID
KswordARKFileMonitorPushEvent(
    _In_ const KSWORD_ARK_FILE_MONITOR_EVENT* Event
    )
/*++

Routine Description:

    把事件放入固定 ring buffer。中文说明：满时丢弃最旧事件并增加 droppedCount，
    R3 后续在状态栏显示 dropped count，提醒用户缩小过滤范围。

Arguments:

    Event - 已填充事件。

Return Value:

    None.

--*/
{
    KIRQL oldIrql;
    ULONG slotIndex = 0UL;

    if (Event == NULL) {
        return;
    }

    KeAcquireSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, &oldIrql);

    slotIndex = g_KswordArkFileMonitorRuntime.TailIndex;
    RtlCopyMemory(
        &g_KswordArkFileMonitorRuntime.Ring[slotIndex],
        Event,
        sizeof(*Event));
    g_KswordArkFileMonitorRuntime.TailIndex =
        KswordARKFileMonitorAdvanceIndex(g_KswordArkFileMonitorRuntime.TailIndex);

    if (g_KswordArkFileMonitorRuntime.QueuedCount == KSWORD_ARK_FILE_MONITOR_RING_CAPACITY) {
        g_KswordArkFileMonitorRuntime.HeadIndex =
            KswordARKFileMonitorAdvanceIndex(g_KswordArkFileMonitorRuntime.HeadIndex);
        g_KswordArkFileMonitorRuntime.DroppedCount += 1UL;
        g_KswordArkFileMonitorRuntime.RuntimeFlags |= KSWORD_ARK_FILE_MONITOR_RUNTIME_DROPPED;
    }
    else {
        g_KswordArkFileMonitorRuntime.QueuedCount += 1UL;
    }

    KeReleaseSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, oldIrql);
}

static BOOLEAN
KswordARKFileMonitorShouldCapture(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ ULONG OperationType
    )
/*++

Routine Description:

    判断当前事件是否符合运行时过滤条件。中文说明：第一版只按操作类型和 PID
    粗过滤，路径/扩展名过滤留给 R3 虚拟化列表和后续规则层。

Arguments:

    Data - FltMgr callback data。
    OperationType - 协议 operation type。

Return Value:

    TRUE 表示应采集。

--*/
{
    ULONG processId = 0UL;

    if ((g_KswordArkFileMonitorRuntime.RuntimeFlags & KSWORD_ARK_FILE_MONITOR_RUNTIME_STARTED) == 0UL) {
        return FALSE;
    }
    if (OperationType == 0UL ||
        (OperationType & g_KswordArkFileMonitorRuntime.OperationMask) == 0UL) {
        return FALSE;
    }

    processId = (ULONG)(ULONG_PTR)FltGetRequestorProcessId(Data);
    if (g_KswordArkFileMonitorRuntime.ProcessIdFilter != 0UL &&
        g_KswordArkFileMonitorRuntime.ProcessIdFilter != processId) {
        return FALSE;
    }

    return TRUE;
}

static FLT_PREOP_CALLBACK_STATUS FLTAPI
KswordARKFileMonitorPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
    )
/*++

Routine Description:

    minifilter pre-operation 回调。中文说明：Create/SetInfo 需要 post 结果时请求
    post callback；Read/Write/Cleanup/Close 直接记录 pre 事件以降低开销。

Arguments:

    Data - FltMgr callback data。
    FltObjects - FltMgr related objects。
    CompletionContext - 本实现不分配上下文，始终返回 NULL。

Return Value:

    FLT_PREOP_SUCCESS_NO_CALLBACK 或 FLT_PREOP_SUCCESS_WITH_CALLBACK。

--*/
{
    ULONG operationType = 0UL;
    KSWORD_ARK_FILE_MONITOR_EVENT event;

    if (CompletionContext != NULL) {
        *CompletionContext = NULL;
    }

    operationType = KswordARKFileMonitorMapMajorToOperation(
        Data->Iopb->MajorFunction,
        Data->Iopb->MinorFunction,
        &Data->Iopb->Parameters);
    if (!KswordARKFileMonitorShouldCapture(Data, operationType)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (Data->Iopb->MajorFunction == IRP_MJ_CREATE ||
        Data->Iopb->MajorFunction == IRP_MJ_SET_INFORMATION) {
        return FLT_PREOP_SUCCESS_WITH_CALLBACK;
    }

    KswordARKFileMonitorFillCommonEvent(
        &event,
        Data,
        FltObjects,
        operationType,
        FALSE);
    KswordARKFileMonitorPushEvent(&event);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static FLT_POSTOP_CALLBACK_STATUS FLTAPI
KswordARKFileMonitorPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
/*++

Routine Description:

    minifilter post-operation 回调。中文说明：这里记录 Create/SetInfo 的最终
    NTSTATUS；如果 FltMgr 正在 drain，则不访问可能不稳定的上下文。

Arguments:

    Data - FltMgr callback data。
    FltObjects - FltMgr related objects。
    CompletionContext - 未使用。
    Flags - post operation flags。

Return Value:

    FLT_POSTOP_FINISHED_PROCESSING。

--*/
{
    ULONG operationType = 0UL;
    KSWORD_ARK_FILE_MONITOR_EVENT event;

    UNREFERENCED_PARAMETER(CompletionContext);

    if ((Flags & FLTFL_POST_OPERATION_DRAINING) != 0UL) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    operationType = KswordARKFileMonitorMapMajorToOperation(
        Data->Iopb->MajorFunction,
        Data->Iopb->MinorFunction,
        &Data->Iopb->Parameters);
    if (!KswordARKFileMonitorShouldCapture(Data, operationType)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    KswordARKFileMonitorFillCommonEvent(
        &event,
        Data,
        FltObjects,
        operationType,
        TRUE);
    KswordARKFileMonitorPushEvent(&event);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

static NTSTATUS FLTAPI
KswordARKFileMonitorUnloadCallback(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
/*++

Routine Description:

    FltMgr 请求卸载过滤器。中文说明：实际资源释放由 WDF driver unload 调用
    KswordARKFileMonitorUninitialize 统一完成。

Arguments:

    Flags - FltMgr unload flags。

Return Value:

    STATUS_SUCCESS.

--*/
{
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKFileMonitorInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_opt_ WDFDEVICE Device
    )
/*++

Routine Description:

    初始化 Phase-12 minifilter runtime。中文说明：只注册 filter，不默认开始采集；
    StartFiltering 由 control IOCTL 显式触发，避免加载驱动就产生高频文件事件。

Arguments:

    DriverObject - DriverEntry 传入的驱动对象。
    RegistryPath - DriverEntry 传入的注册表路径。
    Device - 控制设备，用于日志；早期可为空。

Return Value:

    STATUS_SUCCESS 或 FltRegisterFilter 失败状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&g_KswordArkFileMonitorRuntime, sizeof(g_KswordArkFileMonitorRuntime));
    g_KswordArkFileMonitorRuntime.Device = Device;
    g_KswordArkFileMonitorRuntime.OperationMask = KSWORD_ARK_FILE_MONITOR_OPERATION_ALL;
    g_KswordArkFileMonitorRuntime.RegisterStatus = STATUS_NOT_SUPPORTED;
    g_KswordArkFileMonitorRuntime.StartStatus = STATUS_NOT_SUPPORTED;
    KeInitializeSpinLock(&g_KswordArkFileMonitorRuntime.RingLock);

    status = FltRegisterFilter(
        DriverObject,
        &g_KswordArkFileMonitorRegistration,
        &g_KswordArkFileMonitorRuntime.Filter);
    g_KswordArkFileMonitorRuntime.RegisterStatus = status;
    if (!NT_SUCCESS(status)) {
        g_KswordArkFileMonitorRuntime.Filter = NULL;
        g_KswordArkFileMonitorRuntime.LastErrorStatus = status;
        return status;
    }

    g_KswordArkFileMonitorRuntime.RuntimeFlags |= KSWORD_ARK_FILE_MONITOR_RUNTIME_REGISTERED;
    KswordARKFileMonitorLog("Info", "KswordARK file monitor minifilter registered.");
    return STATUS_SUCCESS;
}

VOID
KswordARKFileMonitorUninitialize(
    VOID
    )
/*++

Routine Description:

    注销 Phase-12 minifilter runtime。中文说明：卸载前先清 STARTED 标志，随后让
    FltUnregisterFilter 等待正在执行的 callback 完成。

Arguments:

    None.

Return Value:

    None.

--*/
{
    g_KswordArkFileMonitorRuntime.RuntimeFlags &= ~KSWORD_ARK_FILE_MONITOR_RUNTIME_STARTED;

    if (g_KswordArkFileMonitorRuntime.Filter != NULL) {
        FltUnregisterFilter(g_KswordArkFileMonitorRuntime.Filter);
        g_KswordArkFileMonitorRuntime.Filter = NULL;
    }

    g_KswordArkFileMonitorRuntime.RuntimeFlags = 0UL;
}

NTSTATUS
KswordARKFileMonitorControl(
    _In_ const KSWORD_ARK_FILE_MONITOR_CONTROL_REQUEST* Request
    )
/*++

Routine Description:

    处理 Start/Stop/Clear 控制命令。中文说明：Start 首次调用 FltStartFiltering，
    后续 Start 只更新过滤条件和 STARTED 标志。

Arguments:

    Request - 控制请求。

Return Value:

    STATUS_SUCCESS 或 FltStartFiltering/参数校验失败状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL oldIrql;

    if (Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Request->action) {
    case KSWORD_ARK_FILE_MONITOR_ACTION_START:
        if (g_KswordArkFileMonitorRuntime.Filter == NULL) {
            g_KswordArkFileMonitorRuntime.LastErrorStatus = STATUS_INVALID_DEVICE_STATE;
            return STATUS_INVALID_DEVICE_STATE;
        }

        if (Request->operationMask != 0UL) {
            g_KswordArkFileMonitorRuntime.OperationMask =
                Request->operationMask & KSWORD_ARK_FILE_MONITOR_OPERATION_ALL;
        }
        if (g_KswordArkFileMonitorRuntime.OperationMask == 0UL) {
            g_KswordArkFileMonitorRuntime.OperationMask = KSWORD_ARK_FILE_MONITOR_OPERATION_ALL;
        }
        g_KswordArkFileMonitorRuntime.ProcessIdFilter = Request->processId;

        if (g_KswordArkFileMonitorRuntime.StartStatus == STATUS_NOT_SUPPORTED) {
            status = FltStartFiltering(g_KswordArkFileMonitorRuntime.Filter);
            g_KswordArkFileMonitorRuntime.StartStatus = status;
            if (!NT_SUCCESS(status)) {
                g_KswordArkFileMonitorRuntime.LastErrorStatus = status;
                return status;
            }
        }

        g_KswordArkFileMonitorRuntime.RuntimeFlags |= KSWORD_ARK_FILE_MONITOR_RUNTIME_STARTED;
        KswordARKFileMonitorLog("Info", "KswordARK file monitor started.");
        return STATUS_SUCCESS;

    case KSWORD_ARK_FILE_MONITOR_ACTION_STOP:
        g_KswordArkFileMonitorRuntime.RuntimeFlags &= ~KSWORD_ARK_FILE_MONITOR_RUNTIME_STARTED;
        KswordARKFileMonitorLog("Info", "KswordARK file monitor stopped.");
        return STATUS_SUCCESS;

    case KSWORD_ARK_FILE_MONITOR_ACTION_CLEAR:
        KeAcquireSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, &oldIrql);
        g_KswordArkFileMonitorRuntime.HeadIndex = 0UL;
        g_KswordArkFileMonitorRuntime.TailIndex = 0UL;
        g_KswordArkFileMonitorRuntime.QueuedCount = 0UL;
        g_KswordArkFileMonitorRuntime.DroppedCount = 0UL;
        g_KswordArkFileMonitorRuntime.RuntimeFlags &= ~KSWORD_ARK_FILE_MONITOR_RUNTIME_DROPPED;
        RtlZeroMemory(
            g_KswordArkFileMonitorRuntime.Ring,
            sizeof(g_KswordArkFileMonitorRuntime.Ring));
        KeReleaseSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, oldIrql);
        return STATUS_SUCCESS;

    default:
        return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS
KswordARKFileMonitorQueryStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    返回文件监控运行时状态。中文说明：R3 可据此显示 registered/started、
    queuedCount 和 droppedCount。

Arguments:

    OutputBuffer - 响应缓冲区。
    OutputBufferLength - 响应长度。
    BytesWrittenOut - 接收写入字节数。

Return Value:

    STATUS_SUCCESS 或缓冲区错误。

--*/
{
    KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE* response = NULL;
    KIRQL oldIrql;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FILE_MONITOR_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->operationMask = g_KswordArkFileMonitorRuntime.OperationMask;
    response->processIdFilter = g_KswordArkFileMonitorRuntime.ProcessIdFilter;
    response->ringCapacity = KSWORD_ARK_FILE_MONITOR_RING_CAPACITY;
    response->sequence = (ULONG64)g_KswordArkFileMonitorRuntime.Sequence;
    response->registerStatus = g_KswordArkFileMonitorRuntime.RegisterStatus;
    response->startStatus = g_KswordArkFileMonitorRuntime.StartStatus;
    response->lastErrorStatus = g_KswordArkFileMonitorRuntime.LastErrorStatus;

    KeAcquireSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, &oldIrql);
    response->runtimeFlags = g_KswordArkFileMonitorRuntime.RuntimeFlags;
    response->queuedCount = g_KswordArkFileMonitorRuntime.QueuedCount;
    response->droppedCount = g_KswordArkFileMonitorRuntime.DroppedCount;
    KeReleaseSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, oldIrql);

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKFileMonitorDrain(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_FILE_MONITOR_DRAIN_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    从文件监控 ring buffer 取出事件。中文说明：取出即消费，R3 应按 returnedCount
    追加到虚拟化列表；缓冲不足时只返回能容纳的事件。

Arguments:

    OutputBuffer - 响应缓冲区。
    OutputBufferLength - 响应长度。
    Request - 可选请求，限制 maxEvents。
    BytesWrittenOut - 接收实际写入长度。

Return Value:

    STATUS_SUCCESS 或缓冲区错误。

--*/
{
    KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE* response = NULL;
    ULONG maxEvents = 0UL;
    ULONG capacityByBuffer = 0UL;
    ULONG eventsToReturn = 0UL;
    ULONG eventIndex = 0UL;
    KIRQL oldIrql;
    const size_t headerSize =
        sizeof(KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE) -
        sizeof(KSWORD_ARK_FILE_MONITOR_EVENT);

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FILE_MONITOR_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_FILE_MONITOR_EVENT);
    response->ringCapacity = KSWORD_ARK_FILE_MONITOR_RING_CAPACITY;

    capacityByBuffer = (ULONG)((OutputBufferLength - headerSize) / sizeof(KSWORD_ARK_FILE_MONITOR_EVENT));
    maxEvents = capacityByBuffer;
    if (Request != NULL && Request->maxEvents != 0UL && Request->maxEvents < maxEvents) {
        maxEvents = Request->maxEvents;
    }

    KeAcquireSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, &oldIrql);
    response->totalQueuedBeforeDrain = g_KswordArkFileMonitorRuntime.QueuedCount;
    response->droppedCount = g_KswordArkFileMonitorRuntime.DroppedCount;
    response->runtimeFlags = g_KswordArkFileMonitorRuntime.RuntimeFlags;

    eventsToReturn = g_KswordArkFileMonitorRuntime.QueuedCount;
    if (eventsToReturn > maxEvents) {
        eventsToReturn = maxEvents;
    }

    for (eventIndex = 0UL; eventIndex < eventsToReturn; ++eventIndex) {
        ULONG slotIndex = g_KswordArkFileMonitorRuntime.HeadIndex;
        RtlCopyMemory(
            &response->events[eventIndex],
            &g_KswordArkFileMonitorRuntime.Ring[slotIndex],
            sizeof(KSWORD_ARK_FILE_MONITOR_EVENT));
        RtlZeroMemory(
            &g_KswordArkFileMonitorRuntime.Ring[slotIndex],
            sizeof(KSWORD_ARK_FILE_MONITOR_EVENT));
        g_KswordArkFileMonitorRuntime.HeadIndex =
            KswordARKFileMonitorAdvanceIndex(g_KswordArkFileMonitorRuntime.HeadIndex);
    }

    g_KswordArkFileMonitorRuntime.QueuedCount -= eventsToReturn;
    response->returnedCount = eventsToReturn;
    KeReleaseSpinLock(&g_KswordArkFileMonitorRuntime.RingLock, oldIrql);

    *BytesWrittenOut = headerSize + ((size_t)eventsToReturn * sizeof(KSWORD_ARK_FILE_MONITOR_EVENT));
    return STATUS_SUCCESS;
}
