/*++

Module Name:

    debug_output_capture.c

Abstract:

    Captures kernel debug-print callbacks into a fixed nonpaged ring buffer.

Environment:

    Kernel-mode Driver Framework.  The callback may run at IRQL <= DIRQL.

--*/

#include "ark/ark_driver.h"

// DbgSetDebugPrintCallback 仅允许一个静态回调，因此保存当前控制设备上下文。
static PDEVICE_CONTEXT volatile g_KswordArkDebugOutputContext = NULL;

// 原子读取当前回调上下文，避免普通指针读取与启停路径发生数据竞争。
static PDEVICE_CONTEXT KswordARKDebugOutputReadContext(VOID)
{
    return (PDEVICE_CONTEXT)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_KswordArkDebugOutputContext,
        NULL,
        NULL);
}

// 回调路径不能分配内存、等待锁或写调试日志，否则会造成递归或高 IRQL 故障。
static VOID KswordARKDebugPrintCallback(
    _In_ PSTRING Output,
    _In_ ULONG ComponentId,
    _In_ ULONG Level)
{
    PDEVICE_CONTEXT context;
    KSWORD_ARK_DEBUG_OUTPUT_SLOT* slot;
    ULONGLONG latestSequence;
    ULONGLONG sequence;
    ULONG slotIndex;
    USHORT copyLength;

    // 先读取全局上下文；停止捕获时会先清除启用位，再注销回调。
    context = KswordARKDebugOutputReadContext();
    if (context == NULL ||
        InterlockedCompareExchange(&context->DebugOutputCaptureEnabled, 0, 0) == 0 ||
        Output == NULL ||
        Output->Buffer == NULL) {
        return;
    }

    // 回调可在 DIRQL 执行，只做 try-lock；并发写入时宁可丢弃也绝不等待。
    if (InterlockedCompareExchange(&context->DebugOutputWriterLock, 1, 0) != 0) {
        InterlockedIncrement64(&context->DebugOutputDroppedCount);
        return;
    }

    // 每条记录使用单调序号定位环形槽，序号零保留给“尚无记录”。
    latestSequence = (ULONGLONG)InterlockedCompareExchange64(
        &context->DebugOutputLatestSequence,
        0,
        0);
    sequence = latestSequence + 1;
    slotIndex = (ULONG)((sequence - 1) % KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY);
    slot = &context->DebugOutputSlots[slotIndex];

    // 负提交序号表示槽正在写入，读取端只接受前后两次一致的正序号。
    InterlockedExchange64(&slot->CommitSequence, -((LONG64)sequence));
    RtlZeroMemory(&slot->Record, sizeof(slot->Record));
    slot->Record.sequence = sequence;
    slot->Record.interruptTime100ns = KeQueryInterruptTime();
    slot->Record.componentId = ComponentId;
    slot->Record.level = Level;

    // DbgPrintEx 单条输出最多 512 字节；仍预留结尾 NUL 以便用户态安全解析。
    copyLength = Output->Length;
    if (copyLength >= KSWORD_ARK_DEBUG_OUTPUT_TEXT_BYTES) {
        copyLength = KSWORD_ARK_DEBUG_OUTPUT_TEXT_BYTES - 1;
        slot->Record.flags |= KSWORD_ARK_DEBUG_OUTPUT_RECORD_FLAG_TEXT_TRUNCATED;
    }
    if (copyLength != 0) {
        RtlCopyMemory(slot->Record.text, Output->Buffer, copyLength);
    }
    slot->Record.text[copyLength] = '\0';
    slot->Record.textLengthBytes = copyLength;

    // 先发布完整记录，再发布提交序号与最新序号，保证读取端不会看到半条记录。
    KeMemoryBarrier();
    InterlockedExchange64(&slot->CommitSequence, (LONG64)sequence);
    InterlockedExchange64(&context->DebugOutputLatestSequence, (LONG64)sequence);
    InterlockedExchange(&context->DebugOutputWriterLock, 0);
}

// 仅在回调已注销或尚未注册时重置环形缓冲区。
static VOID KswordARKDebugOutputReset(_Inout_ PDEVICE_CONTEXT Context)
{
    InterlockedExchange(&Context->DebugOutputWriterLock, 0);
    InterlockedExchange64(&Context->DebugOutputLatestSequence, 0);
    InterlockedExchange64(&Context->DebugOutputDroppedCount, 0);
    RtlZeroMemory(Context->DebugOutputSlots, sizeof(Context->DebugOutputSlots));
}

// 把当前注册、捕获与丢弃状态转换为共享协议响应。
static VOID KswordARKDebugOutputFillControlResponse(
    _In_ PDEVICE_CONTEXT Context,
    _Out_ KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE* Response)
{
    PDEVICE_CONTEXT activeContext;
    ULONGLONG latestSequence;

    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    activeContext = KswordARKDebugOutputReadContext();
    latestSequence = (ULONGLONG)InterlockedCompareExchange64(
        &Context->DebugOutputLatestSequence,
        0,
        0);

    if (activeContext == Context) {
        Response->runtimeFlags |= KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_REGISTERED;
    }
    if (InterlockedCompareExchange(&Context->DebugOutputCaptureEnabled, 0, 0) != 0) {
        Response->runtimeFlags |= KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_CAPTURING;
    }
    Response->droppedCount = (ULONGLONG)InterlockedCompareExchange64(
        &Context->DebugOutputDroppedCount,
        0,
        0);
    if (Response->droppedCount != 0) {
        Response->runtimeFlags |= KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_DROPPED;
    }
    Response->latestSequence = latestSequence;
    Response->ringCapacity = KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY;
    Response->queuedCount = (ULONG)min(
        latestSequence,
        (ULONGLONG)KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY);
    Response->registrationStatus = Context->DebugOutputRegistrationStatus;
    Response->lastStatus = Context->DebugOutputLastStatus;
}

// 初始化控制设备上下文；此处不注册回调，只有用户显式开始捕获时才注册。
NTSTATUS KswordARKDebugOutputInitialize(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT context;

    if (Device == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    context = DeviceGetContext(Device);
    context->DebugOutputCaptureEnabled = 0;
    context->DebugOutputWriterLock = 0;
    context->DebugOutputLatestSequence = 0;
    context->DebugOutputDroppedCount = 0;
    context->DebugOutputRegistrationStatus = STATUS_NOT_SUPPORTED;
    context->DebugOutputLastStatus = STATUS_SUCCESS;
    RtlZeroMemory(context->DebugOutputSlots, sizeof(context->DebugOutputSlots));
    return STATUS_SUCCESS;
}

// 执行开始、停止或查询操作，并始终返回可诊断的运行时状态。
NTSTATUS KswordARKDebugOutputControl(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST* Request,
    _Out_ KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE* Response)
{
    PDEVICE_CONTEXT context;
    PDEVICE_CONTEXT activeContext;
    NTSTATUS status;

    if (Device == NULL || Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->version != KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION ||
        Request->size < sizeof(*Request)) {
        return STATUS_REVISION_MISMATCH;
    }

    context = DeviceGetContext(Device);
    status = STATUS_SUCCESS;

    switch (Request->action) {
    case KSWORD_ARK_DEBUG_OUTPUT_ACTION_START:
        activeContext = KswordARKDebugOutputReadContext();
        if (activeContext == context &&
            InterlockedCompareExchange(&context->DebugOutputCaptureEnabled, 0, 0) != 0) {
            status = STATUS_SUCCESS;
            break;
        }
        if (activeContext != NULL) {
            status = STATUS_DEVICE_BUSY;
            break;
        }

        // 在发布上下文前清空上一轮数据，保证新会话从序号一开始。
        KswordARKDebugOutputReset(context);
        InterlockedExchange(&context->DebugOutputCaptureEnabled, 1);
        activeContext = (PDEVICE_CONTEXT)InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_KswordArkDebugOutputContext,
            context,
            NULL);
        if (activeContext != NULL) {
            InterlockedExchange(&context->DebugOutputCaptureEnabled, 0);
            status = STATUS_DEVICE_BUSY;
            break;
        }

        // 使用 WDK 支持的回调接口捕获进入内核调试管线的消息，不修改全局筛选器。
        status = DbgSetDebugPrintCallback(KswordARKDebugPrintCallback, TRUE);
        context->DebugOutputRegistrationStatus = status;
        if (!NT_SUCCESS(status)) {
            InterlockedExchange(&context->DebugOutputCaptureEnabled, 0);
            InterlockedCompareExchangePointer(
                (PVOID volatile*)&g_KswordArkDebugOutputContext,
                NULL,
                context);
        }
        break;

    case KSWORD_ARK_DEBUG_OUTPUT_ACTION_STOP:
        activeContext = KswordARKDebugOutputReadContext();
        if (activeContext == context) {
            // 先禁止回调写入，再向内核注销回调，避免停止窗口出现新记录。
            InterlockedExchange(&context->DebugOutputCaptureEnabled, 0);
            status = DbgSetDebugPrintCallback(KswordARKDebugPrintCallback, FALSE);
            context->DebugOutputRegistrationStatus = status;
            if (NT_SUCCESS(status)) {
                InterlockedCompareExchangePointer(
                    (PVOID volatile*)&g_KswordArkDebugOutputContext,
                    NULL,
                    context);
            }
        } else {
            InterlockedExchange(&context->DebugOutputCaptureEnabled, 0);
            status = STATUS_SUCCESS;
        }
        break;

    case KSWORD_ARK_DEBUG_OUTPUT_ACTION_QUERY:
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    context->DebugOutputLastStatus = status;
    KswordARKDebugOutputFillControlResponse(context, Response);
    return status;
}

// 按序号增量读取稳定快照；环形覆盖、并发改写均通过标志与计数显式报告。
NTSTATUS KswordARKDebugOutputDrain(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned)
{
    PDEVICE_CONTEXT context;
    KSWORD_ARK_DEBUG_OUTPUT_SLOT* slot;
    const size_t responseHeaderSize = FIELD_OFFSET(KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE, records);
    ULONGLONG latestSequence;
    ULONGLONG earliestSequence;
    ULONGLONG afterSequence;
    ULONGLONG nextSequence;
    ULONGLONG sequence;
    ULONG outputCapacity;
    ULONG requestedCount;
    ULONG recordCount;
    LONG64 commitBefore;
    LONG64 commitAfter;

    if (Device == NULL || Request == NULL || Response == NULL || BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;
    if (Request->version != KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION ||
        Request->size < sizeof(*Request)) {
        return STATUS_REVISION_MISMATCH;
    }
    if (OutputBufferLength < responseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    context = DeviceGetContext(Device);
    outputCapacity = (ULONG)((OutputBufferLength - responseHeaderSize) /
        sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD));
    requestedCount = Request->maxRecords;
    if (requestedCount == 0) {
        requestedCount = KSWORD_ARK_DEBUG_OUTPUT_DEFAULT_DRAIN_RECORDS;
    }
    requestedCount = min(requestedCount, KSWORD_ARK_DEBUG_OUTPUT_MAX_DRAIN_RECORDS);
    requestedCount = min(requestedCount, outputCapacity);

    RtlZeroMemory(Response, responseHeaderSize +
        ((size_t)requestedCount * sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD)));
    Response->version = KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION;
    Response->size = (ULONG)responseHeaderSize;
    Response->entrySize = sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD);
    Response->ringCapacity = KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY;
    if (KswordARKDebugOutputReadContext() == context) {
        Response->runtimeFlags |= KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_REGISTERED;
    }
    if (InterlockedCompareExchange(&context->DebugOutputCaptureEnabled, 0, 0) != 0) {
        Response->runtimeFlags |= KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_CAPTURING;
    }
    Response->droppedCount = (ULONGLONG)InterlockedCompareExchange64(
        &context->DebugOutputDroppedCount,
        0,
        0);
    if (Response->droppedCount != 0) {
        Response->runtimeFlags |= KSWORD_ARK_DEBUG_OUTPUT_RUNTIME_DROPPED;
    }

    latestSequence = (ULONGLONG)InterlockedCompareExchange64(
        &context->DebugOutputLatestSequence,
        0,
        0);
    Response->latestSequence = latestSequence;
    earliestSequence = latestSequence > KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY
        ? latestSequence - KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY + 1
        : (latestSequence == 0 ? 0 : 1);
    Response->firstAvailableSequence = earliestSequence;

    afterSequence = Request->afterSequence;
    if (afterSequence > latestSequence) {
        // 驱动重启或新会话后旧游标可能大于当前序号，按从头读取处理。
        afterSequence = 0;
        Response->responseFlags |= KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_OVERFLOW;
    }
    nextSequence = afterSequence;
    if (earliestSequence != 0 && afterSequence + 1 < earliestSequence) {
        Response->lostBeforeFirst = earliestSequence - (afterSequence + 1);
        Response->responseFlags |= KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_OVERFLOW;
        nextSequence = earliestSequence - 1;
    }

    recordCount = 0;
    sequence = nextSequence + 1;
    while (sequence != 0 && sequence <= latestSequence && recordCount < requestedCount) {
        slot = &context->DebugOutputSlots[(sequence - 1) % KSWORD_ARK_DEBUG_OUTPUT_RING_CAPACITY];
        commitBefore = InterlockedCompareExchange64(&slot->CommitSequence, 0, 0);
        if (commitBefore == (LONG64)sequence) {
            RtlCopyMemory(
                &Response->records[recordCount],
                &slot->Record,
                sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD));
            KeMemoryBarrier();
            commitAfter = InterlockedCompareExchange64(&slot->CommitSequence, 0, 0);
            if (commitAfter == commitBefore &&
                Response->records[recordCount].sequence == sequence) {
                recordCount++;
            } else {
                RtlZeroMemory(
                    &Response->records[recordCount],
                    sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD));
                Response->responseFlags |= KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_SNAPSHOT_RACE;
            }
        } else {
            Response->responseFlags |= KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_SNAPSHOT_RACE;
        }
        nextSequence = sequence;
        sequence++;
    }

    Response->returnedCount = recordCount;
    Response->nextSequence = nextSequence;
    if (nextSequence < latestSequence) {
        Response->responseFlags |= KSWORD_ARK_DEBUG_OUTPUT_DRAIN_FLAG_MORE_AVAILABLE;
    }
    Response->size = (ULONG)(responseHeaderSize +
        ((size_t)recordCount * sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD)));
    *BytesReturned = Response->size;
    return STATUS_SUCCESS;
}

// 驱动卸载前注销回调；必须先完成本步骤，避免内核保留指向已卸载代码的函数指针。
VOID KswordARKDebugOutputUninitialize(VOID)
{
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    context = KswordARKDebugOutputReadContext();
    if (context == NULL) {
        return;
    }

    InterlockedExchange(&context->DebugOutputCaptureEnabled, 0);
    status = DbgSetDebugPrintCallback(KswordARKDebugPrintCallback, FALSE);
    context->DebugOutputRegistrationStatus = status;
    context->DebugOutputLastStatus = status;
    if (NT_SUCCESS(status)) {
        InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_KswordArkDebugOutputContext,
            NULL,
            context);
    }
}
