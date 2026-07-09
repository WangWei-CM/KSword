/*++

Module Name:

    file_ioctl.c

Abstract:

    IOCTL handlers for KswordARK file operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKFileIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one file-handler log message.

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style ANSI message template.
    ... - Template arguments.

Return Value:

    None. Formatting or enqueue failures are ignored.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

typedef struct _KSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT
{
    KEVENT CompletionEvent;
    KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST Request;
    NTSTATUS OperationStatus;
} KSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT, *PKSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT;

static VOID
KswordARKFileIntegritySystemWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context
    )
/*++

Routine Description:

    Run the file mandatory-label update from a system worker thread.
    中文说明：文件对象允许 System Mandatory Label，但 Windows 会根据当前
    security subject 校验“是否能把对象 label 提升到目标 RID”。IOCTL handler
    可能处在发起进程 High/Medium token 的上下文中，直接 ZwSetSecurityObject
    设置 System 会返回 STATUS_INVALID_LABEL。本 worker 由系统线程执行，
    让后端仍然使用内核 API，同时满足 System label 的主体要求。

Arguments:

    DeviceObject - WDM device object passed by IoQueueWorkItem; currently used
        only to satisfy the callback signature.
    Context - Points to KSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT containing an
        owned request snapshot and a completion event.

Return Value:

    None. The NTSTATUS is written to Context->OperationStatus and the waiting
    IOCTL thread is released via CompletionEvent.

--*/
{
    PKSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT workContext =
        (PKSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (workContext == NULL) {
        return;
    }

    workContext->OperationStatus =
        KswordARKDriverSetFileIntegrity(&workContext->Request);
    KeSetEvent(&workContext->CompletionEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS
KswordARKFileIoctlSetIntegrityViaSystemWorker(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST* Request
    )
/*++

Routine Description:

    Queue and wait for the file-integrity operation on a system worker thread.
    中文说明：该 helper 只改变执行上下文，不改变文件后端逻辑；实际文件打开、
    Mandatory Label SID/ACL/SD 构造和 ZwSetSecurityObject 仍集中在
    KswordARKDriverSetFileIntegrity 中。

Arguments:

    Device - KMDF device used to allocate an IO_WORKITEM bound to the WDM
        device object.
    Request - Validated IOCTL request snapshot. The helper copies it into the
        stack work context before queueing, so callback does not reference the
        METHOD_BUFFERED system buffer or caller stack.

Return Value:

    STATUS_SUCCESS or a backend NTSTATUS when the worker ran; allocation,
    IRQL, or device-state errors are returned before queueing.

--*/
{
    PDEVICE_OBJECT deviceObject = NULL;
    PIO_WORKITEM workItem = NULL;
    KSWORD_ARK_FILE_INTEGRITY_WORK_CONTEXT workContext;
    NTSTATUS waitStatus = STATUS_SUCCESS;

    if (Device == NULL || Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    deviceObject = WdfDeviceWdmGetDeviceObject(Device);
    if (deviceObject == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    workItem = IoAllocateWorkItem(deviceObject);
    if (workItem == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&workContext, sizeof(workContext));
    KeInitializeEvent(&workContext.CompletionEvent, NotificationEvent, FALSE);
    RtlCopyMemory(&workContext.Request, Request, sizeof(workContext.Request));
    workContext.OperationStatus = STATUS_UNSUCCESSFUL;

    IoQueueWorkItem(
        workItem,
        KswordARKFileIntegritySystemWorker,
        DelayedWorkQueue,
        &workContext);

    waitStatus = KeWaitForSingleObject(
        &workContext.CompletionEvent,
        Executive,
        KernelMode,
        FALSE,
        NULL);

    IoFreeWorkItem(workItem);
    if (!NT_SUCCESS(waitStatus)) {
        return waitStatus;
    }
    return workContext.OperationStatus;
}

NTSTATUS
KswordARKFileIoctlDeletePath(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_DELETE_PATH. The handler validates flags, path
    length, and null termination before invoking the file delete feature.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length; consumed by WDF retrieval.
    OutputBufferLength - Caller output length; unused for this IOCTL.
    BytesReturned - Receives sizeof(request) on success and zero on failure.

Return Value:

    NTSTATUS from validation or KswordARKDriverDeletePath.

--*/
{
    KSWORD_ARK_DELETE_PATH_REQUEST* deleteRequest = NULL;
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0;
    BOOLEAN isDirectory = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(KSWORD_ARK_DELETE_PATH_REQUEST), &inputBuffer, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 delete ioctl: input buffer invalid, status=0x%08X", (unsigned int)status);
        return status;
    }

    deleteRequest = (KSWORD_ARK_DELETE_PATH_REQUEST*)inputBuffer;
    isDirectory = ((deleteRequest->flags & KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY) != 0UL) ? TRUE : FALSE;

    if ((deleteRequest->flags & (~KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY)) != 0UL) {
        KswordARKFileIoctlLog(Device, "Warn", "R0 delete ioctl: flags rejected, flags=0x%08X.", (unsigned int)deleteRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }

    if (deleteRequest->pathLengthChars == 0U || deleteRequest->pathLengthChars >= KSWORD_ARK_DELETE_PATH_MAX_CHARS) {
        KswordARKFileIoctlLog(Device, "Warn", "R0 delete ioctl: path length rejected, chars=%u.", (unsigned int)deleteRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }

    if (deleteRequest->path[deleteRequest->pathLengthChars] != L'\0') {
        KswordARKFileIoctlLog(Device, "Warn", "R0 delete ioctl: path not null-terminated, chars=%u.", (unsigned int)deleteRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }
    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_FILE_DELETE;
        safetyContext.TargetProcessId = 0UL;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        safetyContext.TargetText = deleteRequest->path;
        safetyContext.TargetTextChars = deleteRequest->pathLengthChars;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKFileIoctlLog(Device, "Warn", "R0 delete denied by safety policy: chars=%u, status=0x%08X.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)status);
            return status;
        }
    }

    KswordARKFileIoctlLog(Device, "Info", "R0 delete ioctl: chars=%u, directory=%u.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)isDirectory);
    status = KswordARKDriverDeletePath(deleteRequest->path, deleteRequest->pathLengthChars, isDirectory);
    if (NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Info", "R0 delete success: chars=%u, directory=%u.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)isDirectory);
        *BytesReturned = sizeof(KSWORD_ARK_DELETE_PATH_REQUEST);
    }
    else {
        KswordARKFileIoctlLog(Device, "Error", "R0 delete failed: chars=%u, directory=%u, status=0x%08X.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)isDirectory, (unsigned int)status);
    }

    return status;
}

NTSTATUS
KswordARKFileIoctlQueryFileInfo(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_FILE_INFO。中文说明：handler 只负责 WDF
    缓冲获取和路径/flags 基础校验；真正的只读文件属性查询由 file_actions.c
    完成，保持 IOCTL 层薄而可审计。

Arguments:

    Device - WDF 设备对象，用于记录日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度，实际由 WDF 校验。
    OutputBufferLength - 输出长度，实际由 WDF 校验。
    BytesReturned - 接收驱动写入字节数。

Return Value:

    NTSTATUS 表示缓冲校验或 feature 查询结果。

--*/
{
    KSWORD_ARK_QUERY_FILE_INFO_REQUEST* queryRequest = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    const ULONG allowedFlags =
        KSWORD_ARK_QUERY_FILE_INFO_FLAG_DIRECTORY |
        KSWORD_ARK_QUERY_FILE_INFO_FLAG_OPEN_REPARSE_POINT |
        KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME |
        KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_FILE_INFO_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 query-file-info ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    queryRequest = (KSWORD_ARK_QUERY_FILE_INFO_REQUEST*)inputBuffer;
    if ((queryRequest->flags & ~allowedFlags) != 0UL) {
        KswordARKFileIoctlLog(Device, "Warn", "R0 query-file-info ioctl: flags rejected, flags=0x%08X.", (unsigned int)queryRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    if (queryRequest->pathLengthChars == 0U ||
        queryRequest->pathLengthChars >= KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS ||
        queryRequest->path[queryRequest->pathLengthChars] != L'\0') {
        KswordARKFileIoctlLog(Device, "Warn", "R0 query-file-info ioctl: path rejected, chars=%u.", (unsigned int)queryRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 query-file-info ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryFileInfo(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 query-file-info failed: chars=%u, status=0x%08X.", (unsigned int)queryRequest->pathLengthChars, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE)) {
        KSWORD_ARK_QUERY_FILE_INFO_RESPONSE* response = (KSWORD_ARK_QUERY_FILE_INFO_RESPONSE*)outputBuffer;
        KswordARKFileIoctlLog(
            Device,
            "Info",
            "R0 query-file-info success: chars=%u, status=%lu, fields=0x%08X.",
            (unsigned int)queryRequest->pathLengthChars,
            (unsigned long)response->queryStatus,
            (unsigned int)response->fieldFlags);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKFileIoctlSetIntegrity(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SET_FILE_INTEGRITY。中文说明：handler 校验路径、
    flags 和固定响应缓冲，后端只调用 ZwCreateFile/ZwSetSecurityObject 写入
    LABEL_SECURITY_INFORMATION，不 patch 文件系统私有对象。

Arguments:

    Device - WDF device used for logging and safety policy.
    Request - Current IOCTL request.
    InputBufferLength - Input length; validated by WDF helper.
    OutputBufferLength - Output length; validated by WDF helper.
    BytesReturned - Receives sizeof(response) when output is valid.

Return Value:

    STATUS_SUCCESS once the response packet is valid. The actual file operation
    status is stored in response->status/lastStatus.

--*/
{
    KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST* integrityRequest = NULL;
    KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST integrityRequestSnapshot;
    KSWORD_ARK_SET_FILE_INTEGRITY_RESPONSE* integrityResponse = NULL;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    const ULONG allowedFlags =
        KSWORD_ARK_FILE_INTEGRITY_FLAG_DIRECTORY |
        KSWORD_ARK_FILE_INTEGRITY_FLAG_UI_CONFIRMED;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS operationStatus = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Warn", "R0 file-integrity denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 file-integrity ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    /*
     * METHOD_BUFFERED can map the input and output views to the same system
     * buffer.  Copy the whole fixed request first so response initialization
     * cannot erase flags/integrityRid/pathLengthChars/path before validation.
     */
    integrityRequest = (KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST*)inputBuffer;
    RtlCopyMemory(&integrityRequestSnapshot, integrityRequest, sizeof(integrityRequestSnapshot));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_SET_FILE_INTEGRITY_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 file-integrity ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    integrityResponse = (KSWORD_ARK_SET_FILE_INTEGRITY_RESPONSE*)outputBuffer;
    RtlZeroMemory(integrityResponse, sizeof(*integrityResponse));
    integrityResponse->size = sizeof(*integrityResponse);
    integrityResponse->version = KSWORD_ARK_FILE_INTEGRITY_PROTOCOL_VERSION;
    integrityResponse->flags = integrityRequestSnapshot.flags;
    integrityResponse->integrityRid = integrityRequestSnapshot.integrityRid;
    integrityResponse->status = KSWORD_ARK_FILE_INTEGRITY_STATUS_FAILED;
    integrityResponse->lastStatus = STATUS_INVALID_PARAMETER;
    integrityResponse->pathLengthChars = integrityRequestSnapshot.pathLengthChars;
    *BytesReturned = sizeof(*integrityResponse);

    if (integrityRequestSnapshot.size != sizeof(integrityRequestSnapshot) ||
        integrityRequestSnapshot.version != KSWORD_ARK_FILE_INTEGRITY_PROTOCOL_VERSION ||
        (integrityRequestSnapshot.flags & ~allowedFlags) != 0UL ||
        integrityRequestSnapshot.pathLengthChars == 0U ||
        integrityRequestSnapshot.pathLengthChars >= KSWORD_ARK_FILE_INTEGRITY_PATH_MAX_CHARS ||
        integrityRequestSnapshot.path[integrityRequestSnapshot.pathLengthChars] != L'\0') {
        KswordARKFileIoctlLog(
            Device,
            "Warn",
            "R0 file-integrity ioctl: request rejected, size=%lu, version=%lu, flags=0x%08X, chars=%u.",
            (unsigned long)integrityRequestSnapshot.size,
            (unsigned long)integrityRequestSnapshot.version,
            (unsigned int)integrityRequestSnapshot.flags,
            (unsigned int)integrityRequestSnapshot.pathLengthChars);
        return STATUS_SUCCESS;
    }

    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
        safetyContext.TargetProcessId = 0UL;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        safetyContext.TargetText = integrityRequestSnapshot.path;
        safetyContext.TargetTextChars = integrityRequestSnapshot.pathLengthChars;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            integrityResponse->lastStatus = status;
            KswordARKFileIoctlLog(Device, "Warn", "R0 file-integrity denied by safety policy: chars=%u, status=0x%08X.", (unsigned int)integrityRequestSnapshot.pathLengthChars, (unsigned int)status);
            return STATUS_SUCCESS;
        }
    }

    operationStatus = KswordARKFileIoctlSetIntegrityViaSystemWorker(
        Device,
        &integrityRequestSnapshot);
    integrityResponse->lastStatus = operationStatus;
    integrityResponse->status = NT_SUCCESS(operationStatus)
        ? KSWORD_ARK_FILE_INTEGRITY_STATUS_APPLIED
        : KSWORD_ARK_FILE_INTEGRITY_STATUS_FAILED;

    if (NT_SUCCESS(operationStatus)) {
        KswordARKFileIoctlLog(
            Device,
            "Info",
            "R0 file-integrity success: chars=%u, rid=0x%08lX, flags=0x%08lX.",
            (unsigned int)integrityRequestSnapshot.pathLengthChars,
            (unsigned long)integrityRequestSnapshot.integrityRid,
            (unsigned long)integrityRequestSnapshot.flags);
    }
    else {
        KswordARKFileIoctlLog(
            Device,
            "Error",
            "R0 file-integrity failed: chars=%u, rid=0x%08lX, flags=0x%08lX, status=0x%08X.",
            (unsigned int)integrityRequestSnapshot.pathLengthChars,
            (unsigned long)integrityRequestSnapshot.integrityRid,
            (unsigned long)integrityRequestSnapshot.flags,
            (unsigned int)operationStatus);
    }

    return STATUS_SUCCESS;
}
