/*++

Module Name:

    rxpf_runtime.c

Abstract:

    Control-path coordinator for the experimental RX/NX page-fault emulator.

Environment:

    Kernel mode, PASSIVE_LEVEL control path.  The vector-14 path is implemented
    by page_fault_manager.c and never enters this module.

--*/

#include "rxpf_runtime.h"

#include "image_protection_manager.h"
#include "page_fault_manager.h"
#include "page_state_table.h"
#include "rxpf_diagnostics.h"
#include "rxpf_self_test.h"
#include "x64_instruction_emulator.h"

typedef struct _KSW_RXPF_RUNTIME_STATE
{
    KSW_RXPF_PAGE_TABLE PageTable;
    PDRIVER_OBJECT DriverObject;
    ULONG MaximumProcessorCount;
    volatile LONG Initialized;
    volatile LONG Accepting;
    volatile LONG Generation;
    volatile LONG AllocatedSelfTestPassed;
    volatile LONG LastStatus;
} KSW_RXPF_RUNTIME_STATE;

static KSW_RXPF_RUNTIME_STATE g_KswRxpfRuntime;

static const UCHAR g_KswRxpfSelfTestCode[] = {
    0x48U, 0xC7U, 0xC0U, 0x78U, 0x56U, 0x34U, 0x12U,
    0x48U, 0x83U, 0xC0U, 0x01U,
    0xC3U
};

static NTSTATUS
KswRxpfValidateHeader(
    _In_ const KSWORD_ARK_RXPF_REQUEST_HEADER* Header,
    _In_ ULONG ExpectedSize,
    _In_ ULONG AllowedFlags
    )
{
    /* Every request is exact-version, exact-size and explicitly confirmed. */
    if (Header == NULL ||
        Header->version != KSWORD_ARK_RXPF_PROTOCOL_VERSION ||
        Header->size != ExpectedSize ||
        Header->confirmationToken != KSWORD_ARK_RXPF_CONFIRMATION_TOKEN ||
        (Header->flags & KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED) == 0UL ||
        (Header->flags & ~AllowedFlags) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static VOID
KswRxpfFillPageResponse(
    _In_opt_ const KSW_RXPF_PAGE_RECORD* Record,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    /* The caller holds the writer lock while copying mutable record fields. */
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    if (Record == NULL) {
        return;
    }
    Response->state = (ULONG)Record->State;
    Response->targetKind = Record->TargetKind;
    Response->flags = Record->Flags;
    Response->generation = Record->Generation;
    Response->referenceCount = (ULONG)Record->ReferenceCount;
    Response->emulationEnabled = (ULONG)Record->EmulationEnabled;
    Response->recordId = (ULONGLONG)Record->RecordId;
    Response->pageBase = (ULONGLONG)Record->PageBase;
    Response->writableAlias = Record->WritableAlias;
    Response->pfn = Record->Pfn;
    Response->ownerImageBase = Record->OwnerImageBase;
    Response->faultCount = (ULONGLONG)Record->FaultCount;
    Response->emulatedCount = (ULONGLONG)Record->EmulatedCount;
    Response->unsupportedCount = (ULONGLONG)Record->UnsupportedCount;
    Response->lastStatus = Record->LastStatus;
    Response->lastFailureReason = Record->LastFailureReason;
    Response->originalProtection = Record->OriginalProtection;
    Response->currentProtection = Record->CurrentProtection;
    Response->writableAliasProtection =
        Record->WritableAliasProtection;
    Response->lastWriteOffset = Record->LastWriteOffset;
    Response->lastWriteLength = Record->LastWriteLength;
    RtlCopyMemory(
        Response->lastWriteBytes,
        Record->LastWriteBytes,
        sizeof(Response->lastWriteBytes));
}

static BOOLEAN
KswRxpfRuntimeReady(
    VOID
    )
{
    /* Accepting is cleared before unload begins and is never re-enabled. */
    return InterlockedCompareExchange(
        &g_KswRxpfRuntime.Initialized,
        1,
        1) != 0 &&
        InterlockedCompareExchange(
            &g_KswRxpfRuntime.Accepting,
            1,
            1) != 0;
}

static VOID
KswRxpfSetLastStatus(
    _In_ NTSTATUS Status
    )
{
    /* Keep runtime and exported diagnostic status consistent. */
    InterlockedExchange(&g_KswRxpfRuntime.LastStatus, Status);
    KswRxpfDiagnosticsSetLastStatus(Status);
}

NTSTATUS
KswRxpfRuntimeInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    ULONG maximumProcessorCount = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Initialization is idempotent for DriverEntry rollback paths. */
    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (InterlockedCompareExchange(
            &g_KswRxpfRuntime.Initialized,
            1,
            1) != 0) {
        return STATUS_SUCCESS;
    }
    maximumProcessorCount =
        KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (maximumProcessorCount == 0UL) {
        return STATUS_NOT_SUPPORTED;
    }

    /* Allocate every exception-path structure before publishing the runtime. */
    RtlZeroMemory(&g_KswRxpfRuntime, sizeof(g_KswRxpfRuntime));
    g_KswRxpfRuntime.DriverObject = DriverObject;
    g_KswRxpfRuntime.MaximumProcessorCount = maximumProcessorCount;
    KswRxpfPageTableInitialize(&g_KswRxpfRuntime.PageTable);

    status = KswRxpfDiagnosticsInitialize(maximumProcessorCount);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = KswRxpfPageFaultManagerInitialize(
        &g_KswRxpfRuntime.PageTable,
        maximumProcessorCount);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = KswRxpfImageProtectionInitialize(DriverObject);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = KswRxpfX64RunUnitTests();
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    /* Unsupported nt builds still initialize, but all internal calls fail closed. */
    InterlockedExchange(&g_KswRxpfRuntime.Accepting, 1);
    InterlockedExchange(&g_KswRxpfRuntime.Initialized, 1);
    KswRxpfSetLastStatus(STATUS_SUCCESS);
    return STATUS_SUCCESS;

Exit:
    /* Reverse partially completed initialization without exposing IOCTL state. */
    KswRxpfImageProtectionUninitialize();
    KswRxpfPageFaultManagerUninitialize();
    KswRxpfDiagnosticsUninitialize();
    RtlZeroMemory(&g_KswRxpfRuntime, sizeof(g_KswRxpfRuntime));
    return status;
}

VOID
KswRxpfRuntimeUninitialize(
    VOID
    )
{
    ULONG index = 0UL;
    LARGE_INTEGER retryDelay;

    /* Idempotent teardown is used by both unload and DriverEntry rollback. */
    if (InterlockedCompareExchange(
            &g_KswRxpfRuntime.Initialized,
            1,
            1) == 0) {
        return;
    }

    /* Stop new control requests, then unpublish all records from vector 14. */
    InterlockedExchange(&g_KswRxpfRuntime.Accepting, 0);
    KswRxpfPageTableStopAccepting(&g_KswRxpfRuntime.PageTable);
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    for (index = 0UL; index < KSW_RXPF_PAGE_TABLE_CAPACITY; ++index) {
        PKSW_RXPF_PAGE_RECORD record =
            &g_KswRxpfRuntime.PageTable.Slots[index];

        if ((ULONGLONG)record->PageBase > KSW_RXPF_PAGE_TOMBSTONE &&
            record->State != KSWORD_ARK_RXPF_PAGE_STATE_TERMINATING) {
            KswRxpfPageTableBeginRemoveLocked(
                &g_KswRxpfRuntime.PageTable,
                record);
        }
    }
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);

    /* Restore every processor's original IDTR before releasing any code/data. */
    retryDelay.QuadPart = -100000LL;
    while (!NT_SUCCESS(KswRxpfPageFaultRestore())) {
        /* Prefer a blocked unload over freeing code still reachable by vector 14. */
        (void)KeDelayExecutionThread(KernelMode, FALSE, &retryDelay);
    }
    while (!NT_SUCCESS(KswRxpfDiagnosticsWaitForHandlers(5000UL))) {
        /* Unload cannot safely continue while a processor still reads a record. */
    }

    /* With vector 14 restored and all read-side users gone, resources are private. */
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    for (index = 0UL; index < KSW_RXPF_PAGE_TABLE_CAPACITY; ++index) {
        PKSW_RXPF_PAGE_RECORD record =
            &g_KswRxpfRuntime.PageTable.Slots[index];

        if ((ULONGLONG)record->PageBase > KSW_RXPF_PAGE_TOMBSTONE) {
            /* 卸载不得丢弃仍待恢复的映像页；释放锁后等待，允许下一次恢复重试。 */
            while (!NT_SUCCESS(KswRxpfImageProtectionReleaseRecord(record))) {
                record->State = KSWORD_ARK_RXPF_PAGE_STATE_ERROR;
                KswRxpfSetLastStatus(record->LastStatus);
                KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
                (void)KeDelayExecutionThread(KernelMode, FALSE, &retryDelay);
                KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
            }
            KswRxpfPageTableClearRemovedLocked(
                &g_KswRxpfRuntime.PageTable,
                record);
        }
    }
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);

    KswRxpfPageFaultManagerUninitialize();
    KswRxpfDiagnosticsUninitialize();
    KswRxpfImageProtectionUninitialize();
    InterlockedExchange(&g_KswRxpfRuntime.Initialized, 0);
    KeMemoryBarrier();
    RtlZeroMemory(&g_KswRxpfRuntime, sizeof(g_KswRxpfRuntime));
}

NTSTATUS
KswRxpfRuntimeQuerySupport(
    _Out_ KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE* Response
    )
{
    /* Support discovery itself never attempts an internal function call. */
    if (Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KswRxpfImageProtectionQuerySupport(Response);
    Response->processorCount =
        KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (KswRxpfPageFaultIsInstalled()) {
        Response->supportFlags |= KSWORD_ARK_RXPF_SUPPORT_IDT_INSTALLED;
    }
    if (InterlockedCompareExchange(
            &g_KswRxpfRuntime.AllocatedSelfTestPassed,
            1,
            1) != 0) {
        Response->supportFlags |=
            KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PASSED;
    }
    if (InterlockedCompareExchange(
            &g_KswRxpfRuntime.Initialized,
            1,
            1) == 0) {
        Response->supportFlags &= ~KSWORD_ARK_RXPF_SUPPORT_INITIALIZED;
        Response->lastStatus = STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswRxpfRuntimeRegisterPage(
    _In_ const KSWORD_ARK_RXPF_REGISTER_PAGE_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    KSW_RXPF_PAGE_RECORD source;
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Registration allocates/locks resources only on the serialized control path. */
    if (Request == NULL || Response == NULL || Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Response, sizeof(*Response));
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED |
            KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }
    /* AllocatedTest 本身就是自检入口；SelfImageTest 和 ExternalImage
       只走 MmChangeImageProtection + 可写别名——mode=1 恢复路径
       完全不碰影子 IDT，无需依赖 IDT 模拟自检通过。
       因此三者均豁免 AllocatedSelfTestPassed 门禁，
       仅 RunSelfTest 本身仍在 PageFaultInstall 层面把关。 */
    if (Request->targetKind != KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST &&
        Request->targetKind != KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST &&
        Request->targetKind != KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE &&
        InterlockedCompareExchange(
            &g_KswRxpfRuntime.AllocatedSelfTestPassed,
            1,
            1) == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = KswRxpfImageProtectionCreateRecord(
        Request->targetKind,
        Request->targetAddress,
        Request->header.flags & KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP,
        &source);
    if (!NT_SUCCESS(status)) {
        KswRxpfSetLastStatus(status);
        return status;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    status = KswRxpfPageTableInsertLocked(
        &g_KswRxpfRuntime.PageTable,
        &source,
        &record);
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&g_KswRxpfRuntime.Generation);
        KswRxpfFillPageResponse(record, Response);
    }
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    if (!NT_SUCCESS(status)) {
        KswRxpfImageProtectionReleaseRecord(&source);
    }
    KswRxpfSetLastStatus(status);
    return status;
}

NTSTATUS
KswRxpfRuntimeChangePage(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Permission transition is single-shot and protected by the writer lock. */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
    } else {
        status = KswRxpfImageProtectionChangeToRwNx(record);
        record->LastStatus = status;
        if (NT_SUCCESS(status)) {
            InterlockedIncrement(&g_KswRxpfRuntime.Generation);
        }
    }
    KswRxpfFillPageResponse(record, Response);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    KswRxpfSetLastStatus(status);
    return status;
}

NTSTATUS
KswRxpfRuntimeQueryPage(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Query uses the same lock as writers to return a coherent record snapshot. */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
    }
    KswRxpfFillPageResponse(record, Response);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    return status;
}

NTSTATUS
KswRxpfRuntimeWritePage(
    _In_ const KSWORD_ARK_RXPF_WRITE_PAGE_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Instruction bytes cannot change while any CPU may emulate the page. */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }
    if (Request->length == 0UL ||
        Request->length > KSWORD_ARK_RXPF_MAX_WRITE_BYTES ||
        (ULONGLONG)Request->offset + Request->length > PAGE_SIZE ||
        (ULONGLONG)Request->offset + Request->length < Request->offset) {
        return STATUS_INVALID_PARAMETER;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
    } else if (InterlockedCompareExchange(
            &record->EmulationEnabled,
            0,
            0) != 0) {
        status = STATUS_DEVICE_BUSY;
    } else {
        /* 诊断：0x00FF 标记 runtime 即将调用 Write 处理器。 */
        record->LastFailureReason = 0x00FFU;
        status = KswRxpfImageProtectionWrite(
            record,
            Request->offset,
            Request->bytes,
            Request->length);
        if (NT_SUCCESS(status)) {
            InterlockedIncrement(&g_KswRxpfRuntime.Generation);
        }
    }
    if (record != NULL) {
        record->LastStatus = status;
    }
    KswRxpfFillPageResponse(record, Response);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    KswRxpfSetLastStatus(status);
    return status;
}

NTSTATUS
KswRxpfRuntimeSetEmulation(
    _In_ const KSWORD_ARK_RXPF_SET_EMULATION_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN restoreIdt = FALSE;

    /* Enabling is published only after every online CPU has installed its IDT. */
    if (Request == NULL || Response == NULL || Request->enable > 1UL ||
        Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }
    if (record->State != KSWORD_ARK_RXPF_PAGE_STATE_RW_NX) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Exit;
    }
    if (Request->enable != 0UL) {
        status = KswRxpfPageFaultInstall();
        if (NT_SUCCESS(status) &&
            InterlockedExchange(&record->EmulationEnabled, 1) == 0) {
            InterlockedIncrement(
                &g_KswRxpfRuntime.PageTable.EnabledCount);
            InterlockedIncrement(&g_KswRxpfRuntime.Generation);
        }
    } else if (InterlockedExchange(&record->EmulationEnabled, 0) != 0) {
        LONG enabledCount = InterlockedDecrement(
            &g_KswRxpfRuntime.PageTable.EnabledCount);

        KeMemoryBarrier();
        InterlockedIncrement(&g_KswRxpfRuntime.Generation);
        restoreIdt = enabledCount == 0;
        status = KswRxpfDiagnosticsWaitForHandlers(5000UL);
        record->LastStatus = status;
    }

Exit:
    if (record != NULL) {
        record->LastStatus = status;
    }
    KswRxpfFillPageResponse(record, Response);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    if (restoreIdt) {
        NTSTATUS restoreStatus = KswRxpfPageFaultRestore();

        if (NT_SUCCESS(status) && !NT_SUCCESS(restoreStatus)) {
            status = restoreStatus;
        }
        Response->lastStatus = status;
    }
    KswRxpfSetLastStatus(status);
    return status;
}

NTSTATUS
KswRxpfRuntimeQueryStats(
    _Out_ KSWORD_ARK_RXPF_STATS_RESPONSE* Response
    )
{
    /* Aggregate counters are atomic and require no state-table lock. */
    if (Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KswRxpfDiagnosticsQueryStats(
        (ULONG)InterlockedCompareExchange(
            &g_KswRxpfRuntime.Generation,
            0,
            0),
        (ULONG)InterlockedCompareExchange(
            &g_KswRxpfRuntime.PageTable.RegisteredCount,
            0,
            0),
        (ULONG)InterlockedCompareExchange(
            &g_KswRxpfRuntime.PageTable.EnabledCount,
            0,
            0),
        KswRxpfPageFaultIsInstalled() ? 1UL : 0UL,
        KswRxpfPageFaultProcessorCount(),
        Response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswRxpfRuntimeDrainEvents(
    _In_ const KSWORD_ARK_RXPF_DRAIN_EVENTS_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_DRAIN_EVENTS_RESPONSE* Response
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    /* Ring export occurs only on the ordinary IOCTL path. */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) ||
        Request->maxRows > KSWORD_ARK_RXPF_MAX_EVENT_ROWS) {
        return STATUS_INVALID_PARAMETER;
    }
    KswRxpfDiagnosticsDrain(Request, Response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswRxpfRuntimeUnregisterPage(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN restoreIdt = FALSE;

    /* Mark terminating first; only a completed read-side grace period permits free. */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        (KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED |
         KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE));
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
    } else {
        KswRxpfFillPageResponse(record, Response);
        /* Phase F：Unregister 请求若带 SKIP_BACKUP_RESTORE，让 ReleaseRecord
         * 保留 R3 写入字节不自动恢复，以便实用 payload 场景。 */
        record->SkipBackupRestoreOnRelease =
            ((Request->header.flags &
              KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE) != 0UL)
            ? TRUE : FALSE;
        KswRxpfPageTableBeginRemoveLocked(
            &g_KswRxpfRuntime.PageTable,
            record);
        restoreIdt = InterlockedCompareExchange(
            &g_KswRxpfRuntime.PageTable.EnabledCount,
            0,
            0) == 0;
    }
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (restoreIdt) {
        status = KswRxpfPageFaultRestore();
    }
    if (NT_SUCCESS(status)) {
        status = KswRxpfDiagnosticsWaitForHandlers(5000UL);
    }
    if (!NT_SUCCESS(status)) {
        /* 恢复/读侧等待失败仍保留同一记录，以便下一次注销重试。 */
        KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
        record->State = KSWORD_ARK_RXPF_PAGE_STATE_ERROR;
        record->LastStatus = status;
        KswRxpfFillPageResponse(record, Response);
        KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
        KswRxpfSetLastStatus(status);
        return STATUS_SUCCESS;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    status = KswRxpfImageProtectionReleaseRecord(record);
    if (!NT_SUCCESS(status)) {
        /* 不清槽、不释放 MDL/备份；ERROR 状态仍可按 recordId 查询和注销。 */
        record->State = KSWORD_ARK_RXPF_PAGE_STATE_ERROR;
    }
    KswRxpfFillPageResponse(record, Response);
    if (NT_SUCCESS(status)) {
        KswRxpfPageTableClearRemovedLocked(&g_KswRxpfRuntime.PageTable, record);
    }
    InterlockedIncrement(&g_KswRxpfRuntime.Generation);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    KswRxpfSetLastStatus(Response->lastStatus);
    return STATUS_SUCCESS;
}

NTSTATUS
KswRxpfRuntimeRunSelfTest(
    _In_ const KSWORD_ARK_RXPF_RECORD_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_SELF_TEST_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN enabledHere = FALSE;
    BOOLEAN restoreIdt = FALSE;
    ULONGLONG faultsBefore = 0ULL;
    ULONGLONG faultsAfter = 0ULL;
    ULONGLONG returnedValue = 0ULL;
    ULONG workerCount = 0UL;

    /* The live test is limited to either driver-owned, one-page code target. */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    Response->expectedValue = KSW_RXPF_SELF_TEST_EXPECTED_VALUE;
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        status = NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
        goto Complete;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
        goto Unlock;
    }
    Response->recordId = (ULONGLONG)record->RecordId;
    if ((record->TargetKind != KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST &&
         record->TargetKind != KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST) ||
        record->State != KSWORD_ARK_RXPF_PAGE_STATE_RW_NX ||
        record->WritableAlias == 0ULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Unlock;
    }
    if (InterlockedCompareExchange(
            &record->EmulationEnabled,
            0,
            0) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto Unlock;
    }

    status = KswRxpfImageProtectionWrite(
        record,
        0UL,
        g_KswRxpfSelfTestCode,
        sizeof(g_KswRxpfSelfTestCode));
    if (!NT_SUCCESS(status)) {
        goto Unlock;
    }
    status = KswRxpfPageFaultInstall();
    if (!NT_SUCCESS(status)) {
        goto Unlock;
    }
    InterlockedExchange(&record->EmulationEnabled, 1);
    InterlockedIncrement(&g_KswRxpfRuntime.PageTable.EnabledCount);
    enabledHere = TRUE;
    KeMemoryBarrier();
    faultsBefore = (ULONGLONG)InterlockedCompareExchange64(
        &record->FaultCount,
        0,
        0);

    /* Exercise the lock-free read side once on every active processor. */
    status = KswRxpfRunConcurrentExecutionTest(
        (PVOID)(ULONG_PTR)record->PageBase,
        KSW_RXPF_SELF_TEST_EXPECTED_VALUE,
        &returnedValue,
        &workerCount);
    faultsAfter = (ULONGLONG)InterlockedCompareExchange64(
        &record->FaultCount,
        0,
        0);
    if (enabledHere) {
        InterlockedExchange(&record->EmulationEnabled, 0);
        restoreIdt = InterlockedDecrement(
            &g_KswRxpfRuntime.PageTable.EnabledCount) == 0;
        KeMemoryBarrier();
    }
    if (NT_SUCCESS(status) &&
        (returnedValue != KSW_RXPF_SELF_TEST_EXPECTED_VALUE ||
         workerCount == 0UL ||
         faultsAfter - faultsBefore !=
            (ULONGLONG)workerCount * 3ULL)) {
        status = STATUS_DATA_ERROR;
    }
    record->LastStatus = status;
    Response->returnedValue = returnedValue;
    Response->faultsObserved = faultsAfter - faultsBefore;
    Response->instructionCount = workerCount * 3UL;
    Response->result = NT_SUCCESS(status)
        ? KSWORD_ARK_RXPF_EMULATION_SUCCESS
        : record->LastFailureReason;

Unlock:
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    if (restoreIdt) {
        NTSTATUS restoreStatus = KswRxpfPageFaultRestore();

        if (NT_SUCCESS(status) && !NT_SUCCESS(restoreStatus)) {
            status = restoreStatus;
        }
    }

    if (NT_SUCCESS(status)) {
        InterlockedExchange(&g_KswRxpfRuntime.AllocatedSelfTestPassed, 1);
    }
Complete:
    Response->lastStatus = status;
    KswRxpfSetLastStatus(status);
    return status;
}

static VOID
KswRxpfFillHookPointResponse(
    _In_opt_ const KSW_RXPF_PAGE_RECORD* Record,
    _Out_ KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE* Response
    )
{
    /* Snapshot the hook-point fields from the record under the writer lock. */
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    if (Record == NULL) {
        return;
    }
    Response->recordId = (ULONGLONG)Record->RecordId;
    Response->hookAddress = (ULONGLONG)Record->HookAddress;
    Response->hookAction = Record->HookAction;
    Response->state = (ULONG)Record->State;
    Response->redirectTarget = Record->RedirectTarget;
    Response->redirectCount = (ULONGLONG)Record->RedirectCount;
    Response->lastStatus = Record->LastStatus;
}

NTSTATUS
KswRxpfRuntimeSetHookPoint(
    _In_ const KSWORD_ARK_RXPF_SET_HOOK_POINT_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_HOOK_POINT_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Validate inputs before touching any shared state. */
    if (Request == NULL || Response == NULL ||
        Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }

    /* Only NONE, REDIRECT and LOG are valid actions. */
    if (Request->hookAction > KSWORD_ARK_RXPF_HOOK_ACTION_LOG) {
        return STATUS_INVALID_PARAMETER;
    }
    /* REDIRECT must supply both an address and a target. */
    if (Request->hookAction == KSWORD_ARK_RXPF_HOOK_ACTION_REDIRECT &&
        (Request->hookAddress == 0ULL || Request->redirectTarget == 0ULL)) {
        return STATUS_INVALID_PARAMETER;
    }
    /* LOG must supply an address. */
    if (Request->hookAction == KSWORD_ARK_RXPF_HOOK_ACTION_LOG &&
        Request->hookAddress == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }
    /* The page must be in the RW/NX state to accept a hook point. */
    if (record->State != KSWORD_ARK_RXPF_PAGE_STATE_RW_NX) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Exit;
    }
    /* The hook address must fall within the managed page. */
    if (Request->hookAction != KSWORD_ARK_RXPF_HOOK_ACTION_NONE &&
        (Request->hookAddress < (ULONGLONG)record->PageBase ||
         Request->hookAddress >= (ULONGLONG)record->PageBase + PAGE_SIZE)) {
        status = STATUS_INVALID_ADDRESS;
        goto Exit;
    }

    /* 活跃故障读者不持控制锁，禁止在仿真启用期间逐字段更改配置。 */
    if (InterlockedCompareExchange(&record->EmulationEnabled, 0L, 0L) != 0L) {
        /* 必须先显式停用并完成已有的故障处理器排空。 */
        status = STATUS_DEVICE_BUSY;
        /* 此拒绝不改变现有 Hook 地址、动作或目标。 */
        goto Exit;
    }
    /* 控制锁阻止重新启用；等待此前观察到启用状态的无锁读者完整返回。 */
    KeMemoryBarrier();
    /* 停用请求即使曾超时，也不能让残留读者观察半份新配置。 */
    status = KswRxpfDiagnosticsWaitForHandlers(5000UL);
    /* 无法证明排空时保留所有原有字段，交由调用者稍后重试。 */
    if (!NT_SUCCESS(status)) {
        /* 本次没有取得修改故障可见配置的资格。 */
        goto Exit;
    }
    /* 仿真关闭且旧读者排空后，控制锁独占配置直到完整提交。 */
    if (Request->hookAction == KSWORD_ARK_RXPF_HOOK_ACTION_NONE) {
        InterlockedExchange64(&record->HookAddress, 0);
        record->RedirectTarget = 0ULL;
        InterlockedExchange(
            (volatile LONG*)&record->HookAction,
            (LONG)KSWORD_ARK_RXPF_HOOK_ACTION_NONE);
    } else {
        record->RedirectTarget = Request->redirectTarget;
        InterlockedExchange64(
            &record->HookAddress,
            (LONG64)Request->hookAddress);
        KeMemoryBarrier();
        InterlockedExchange(
            (volatile LONG*)&record->HookAction,
            (LONG)Request->hookAction);
    }
    InterlockedIncrement(&g_KswRxpfRuntime.Generation);
    record->LastStatus = STATUS_SUCCESS;

Exit:
    if (record != NULL) {
        record->LastStatus = status;
    }
    KswRxpfFillHookPointResponse(record, Response);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    KswRxpfSetLastStatus(status);
    return status;
}

/*
 * Phase G：读 Record->Backup 的子段。
 * - 校验 header（UI_CONFIRMED only）、runtime ready
 * - 校验 offset/length 不超 PAGE_SIZE、无溢出、length<=MAX_WRITE_BYTES
 * - 持 PageTable 独占锁查 record；Backup=NULL 返 STATUS_NOT_FOUND 并清 hasBackup
 * - 否则拷贝 [offset, offset+length) 到 response.bytes，hasBackup=1
 */
NTSTATUS
KswRxpfRuntimeQueryBackup(
    _In_ const KSWORD_ARK_RXPF_QUERY_BACKUP_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_QUERY_BACKUP_RESPONSE* Response
    )
{
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }

    /* 严格 bounds check：避免溢出与越页读 */
    if (Request->length == 0UL ||
        Request->length > KSWORD_ARK_RXPF_MAX_WRITE_BYTES ||
        (ULONGLONG)Request->offset + (ULONGLONG)Request->length > (ULONGLONG)PAGE_SIZE ||
        (ULONGLONG)Request->offset + (ULONGLONG)Request->length < (ULONGLONG)Request->offset) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    Response->recordId = Request->recordId;
    Response->offset = Request->offset;
    Response->length = Request->length;

    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    record = KswRxpfPageTableFindByIdLocked(
        &g_KswRxpfRuntime.PageTable,
        Request->recordId);
    if (record == NULL) {
        status = STATUS_NOT_FOUND;
    } else if (record->Backup == NULL) {
        /* 未带 CAPTURE_BACKUP flag 注册的记录没有原页快照 */
        Response->hasBackup = 0UL;
        status = STATUS_NOT_FOUND;
    } else {
        Response->hasBackup = 1UL;
        RtlCopyMemory(
            Response->bytes,
            (PUCHAR)record->Backup + Request->offset,
            Request->length);
    }
    Response->lastStatus = status;
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    return status;
}

/*
 * 原子提交：Register→Change(mode=2)→Write→Unregister(SKIP_BACKUP_RESTORE)
 * 全部在单次 IOCTL handler 内完成；这只缩短窗口，不提供跨 CPU 取指隔离，
 * 活跃代码页仍可能在临时 NX 期间触发 BugCheck 0xFC。
 *
 * 仅支持 EXTERNAL_IMAGE 目标。不分配备份（CAPTURE_BACKUP 不需要），
 * 不启用模拟（无需 IDT 安装/恢复/handler 等待）。
 * 失败时自动清理 record，恢复原始页面保护。
 */
NTSTATUS
KswRxpfRuntimeAtomicCommit(
    _In_ const KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_PAGE_RESPONSE* Response
    )
{
    KSW_RXPF_PAGE_RECORD source;
    PKSW_RXPF_PAGE_RECORD record = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* 基本参数校验 */
    if (Request == NULL || Response == NULL || Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Response, sizeof(*Response));
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        return NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
    }

    /* 只接受 EXTERNAL_IMAGE 目标——原子提交的唯一使用场景 */
    if (Request->targetKind != KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE) {
        return STATUS_INVALID_PARAMETER;
    }

    /* 写入参数 bounds check：长度非零、不超 MAX_WRITE_BYTES、不越页 */
    if (Request->length == 0UL ||
        Request->length > KSWORD_ARK_RXPF_MAX_WRITE_BYTES ||
        (ULONGLONG)Request->offset + Request->length > PAGE_SIZE ||
        (ULONGLONG)Request->offset + Request->length < Request->offset) {
        return STATUS_INVALID_PARAMETER;
    }

    /* ── 阶段 1：创建 record（无 CAPTURE_BACKUP，原子流程不需要备份）── */
    status = KswRxpfImageProtectionCreateRecord(
        Request->targetKind,
        Request->targetAddress,
        0UL,        /* flags：不含 CAPTURE_BACKUP */
        &source);
    if (!NT_SUCCESS(status)) {
        KswRxpfSetLastStatus(status);
        return status;
    }

    /* ── 阶段 2：插入页表 ── */
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    status = KswRxpfPageTableInsertLocked(
        &g_KswRxpfRuntime.PageTable,
        &source,
        &record);
    if (!NT_SUCCESS(status)) {
        KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
        KswRxpfImageProtectionReleaseRecord(&source);
        KswRxpfSetLastStatus(status);
        return status;
    }

    /* ── 阶段 3：Change(mode=2) — NX 窗口从此开始 ── */
    status = KswRxpfImageProtectionChangeToRwNx(record);
    record->LastStatus = status;
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    /* ── 阶段 4：Write payload — 窗口仍开着 ── */
    record->LastFailureReason = 0x00FFU;    /* 诊断标记：即将 Write */
    status = KswRxpfImageProtectionWrite(
        record,
        Request->offset,
        Request->bytes,
        Request->length);
    if (!NT_SUCCESS(status)) {
        record->LastStatus = status;
        goto Cleanup;
    }

    /* ── 阶段 5：标记 SKIP_BACKUP_RESTORE → Unregister → mode=1 恢复 RX ── */
    /* payload 写入成功，标记跳过备份恢复使 payload 在 ReleaseRecord 后保留 */
    record->SkipBackupRestoreOnRelease = TRUE;

Cleanup:
    /* 无论成功/失败都要清理 record：BeginRemove → Release → ClearRemoved */

    /* emulation 从未启用——无需 IDT 恢复、无需 handler 等待 */
    KswRxpfPageTableBeginRemoveLocked(
        &g_KswRxpfRuntime.PageTable,
        record);
    /* 保持同一控制锁直到恢复完成，避免 NX 窗口夹入其他控制请求的等待。
     * 此锁只序列化控制请求，不能阻止其他 CPU 的正常取指。 */

    /* 失败路径：不跳过备份恢复（虽然没有备份，ReleaseRecord 会原样恢复 PTE） */
    if (!NT_SUCCESS(status)) {
        record->SkipBackupRestoreOnRelease = FALSE;
    }

    {
        /* releaseStatus：清理失败时保留记录，优先向调用方报告恢复失败。 */
        NTSTATUS releaseStatus = KswRxpfImageProtectionReleaseRecord(record);
        if (!NT_SUCCESS(releaseStatus)) {
            record->State = KSWORD_ARK_RXPF_PAGE_STATE_ERROR;
            status = releaseStatus;
        }
    }
    /* ── NX 窗口在 ReleaseRecord 中 mode=1 调用后关闭 ── */

    /* 在 ClearRemovedLocked 销毁槽之前捕获最终状态 */
    Response->lastStatus = record->LastStatus;
    KswRxpfFillPageResponse(record, Response);
    if (record->State != KSWORD_ARK_RXPF_PAGE_STATE_ERROR) {
        KswRxpfPageTableClearRemovedLocked(&g_KswRxpfRuntime.PageTable, record);
    }
    InterlockedIncrement(&g_KswRxpfRuntime.Generation);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    KswRxpfSetLastStatus(NT_SUCCESS(status) ? Response->lastStatus : status);
    /* 定长响应保留实际状态及可能需要重试注销的 recordId。 */
    Response->lastStatus = status;
    return STATUS_SUCCESS;
}

/*
 * KswRxpfRuntimeExecProof：一次 IOCTL 走完 custom-payload emulated execution。
 *
 * 调用链：
 *   Create ALLOCATED_TEST record  → PageTableInsertLocked
 *   → ChangeToRwNx                → ImageProtectionWrite(custom payload)
 *   → PageFaultInstall (shadow IDT)
 *   → EmulationEnabled = 1        → RunConcurrentExecutionTest (call rcx 走 #PF → emulator)
 *   → EmulationEnabled = 0        → PageFaultRestore (如 EnabledCount 归零)
 *   → BeginRemoveLocked → ReleaseRecord → ClearRemovedLocked
 *
 * 页始终 NX；每次 CPU fetch 都触发 #PF → shadow handler → x64_instruction_emulator
 * 把 payload 字节一个一个 decode 并把 effect 写回 trap frame。走完 ret 后
 * worker 的 RAX = payload 真实执行结果。
 * targetKind 仅接受 ALLOCATED_TEST（driver-owned MDL，不受 VTL1 digest 约束）。
 */
NTSTATUS
KswRxpfRuntimeExecProof(
    _In_ const KSWORD_ARK_RXPF_EXEC_PROOF_REQUEST* Request,
    _Out_ KSWORD_ARK_RXPF_EXEC_PROOF_RESPONSE* Response
    )
{
    /* 本地 record 源：用于 CreateRecord 输出。 */
    KSW_RXPF_PAGE_RECORD source;
    /* 已挂到 PageTable 的 record 指针；NULL 表示尚未 insert 成功。 */
    PKSW_RXPF_PAGE_RECORD record = NULL;
    /* 最终 NTSTATUS；setup 阶段任一步失败即 goto 进 cleanup。 */
    NTSTATUS status = STATUS_SUCCESS;
    /* 页基址供 call rcx 用；页始终 NX，fetch 由 shadow IDT emulator 服务。
     * Release 前 record 还活着，但 Release 后 MDL 可能被 free，所以要早取。 */
    ULONGLONG executionCallTarget = 0ULL;
    /* 单 worker 的返回 RAX。 */
    ULONGLONG returnedValue = 0ULL;
    /* 本次只有一个 worker（当前线程 inline call），用于响应兼容字段。 */
    ULONG workerCount = 0UL;
    /* 本次 verdict。 */
    ULONG verdict = KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SETUP_FAILED;
    /* setup 阶段的 NTSTATUS 快照（独立于最终 status，便于调用方区分哪一步失败）。 */
    NTSTATUS setupStatus = STATUS_SUCCESS;
    /* call rcx 的 SEH 捕获码，0 表示无异常。 */
    NTSTATUS executionException = STATUS_SUCCESS;
    /* shadow IDT install 是否成功；成功时 Cleanup 要调 Restore。 */
    BOOLEAN needPageFaultRestore = FALSE;
    /* emulator 实际服务的 #PF 次数；零则 shadow IDT 未真的接管 payload fetch。 */
    ULONGLONG faultsObserved = 0ULL;
    /* 本次记录身份快照；ClearRemovedLocked 会清空槽位。 */
    ULONGLONG recordId = 0ULL;
    ULONGLONG pageBase = 0ULL;
    /* cleanupStatus：恢复 IDT、等待读侧及释放 MDL 的实际结果。 */
    NTSTATUS cleanupStatus = STATUS_SUCCESS;

    /* 基本参数校验：Request/Response 非空。 */
    if (Request == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* 协议字段使用真实版本/大小，避免诊断哨兵冒充正式响应。 */
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    /* 预先把期望 RAX 写入响应，便于 R3 不解析请求也能回放。 */
    Response->expectedReturnValue = Request->expectedReturnValue;
    /* 预先把 targetKind 和 length 写入响应，与请求对齐。 */
    Response->targetKind = Request->targetKind;
    Response->length = Request->length;

    /* 严格校验请求头：版本/大小/token/UI_CONFIRMED。 */
    status = KswRxpfValidateHeader(
        &Request->header,
        sizeof(*Request),
        KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED);
    if (!NT_SUCCESS(status) || !KswRxpfRuntimeReady()) {
        /* Runtime 未就绪时统一返回 STATUS_DELETE_PENDING，与其他 handler 一致。 */
        status = NT_SUCCESS(status) ? STATUS_DELETE_PENDING : status;
        goto Complete;
    }

    /* 仅接受 ALLOCATED_TEST：这是 driver-owned MDL，不受 VTL1 SHA-512 hash enforcement。 */
    if (Request->targetKind != KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST) {
        status = STATUS_INVALID_PARAMETER;
        goto Complete;
    }
    /* payload 必须非空且不超过协议上限 64 字节，emulator 一次最多走完 64 字节序列。 */
    if (Request->length == 0UL ||
        Request->length > KSWORD_ARK_RXPF_MAX_WRITE_BYTES) {
        status = STATUS_INVALID_PARAMETER;
        goto Complete;
    }

    /* Step 1: 创建 ALLOCATED_TEST record（分配 1 个物理页 + MDL + 两种映射）。 */
    status = KswRxpfImageProtectionCreateRecord(
        KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST,
        0ULL,
        0UL,
        &source);
    setupStatus = status;
    if (!NT_SUCCESS(status)) {
        /* 分配失败：verdict 保持 SETUP_FAILED，调用方据 setupStatus 定位。 */
        goto Complete;
    }

    /* Step 2: 把 source record 插入 PageTable；之后用 record 指针访问。 */
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    status = KswRxpfPageTableInsertLocked(
        &g_KswRxpfRuntime.PageTable,
        &source,
        &record);
    setupStatus = status;
    if (!NT_SUCCESS(status)) {
        /* 插入失败：source 的 MDL/映射归还，走 ReleaseRecord 清理 source。 */
        KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
        (void)KswRxpfImageProtectionReleaseRecord(&source);
        goto Complete;
    }
    recordId = (ULONGLONG)record->RecordId;
    pageBase = (ULONGLONG)record->PageBase;

    /* Step 3: Change(mode=2) 把页改为 RW/NX，建 WritableAlias，开出 NX 窗口。 */
    status = KswRxpfImageProtectionChangeToRwNx(record);
    record->LastStatus = status;
    setupStatus = status;
    if (!NT_SUCCESS(status)) {
        /* Change 失败：直接进 Cleanup，不走 Write/Install。 */
        goto Cleanup;
    }

    /* Step 4: 把调用方 payload 字节写入 writable alias 的 offset 0。 */
    status = KswRxpfImageProtectionWrite(
        record,
        0UL,
        Request->bytes,
        Request->length);
    record->LastStatus = status;
    setupStatus = status;
    if (!NT_SUCCESS(status)) {
        /* Write 失败：直接进 Cleanup。 */
        goto Cleanup;
    }

    /* Step 5: 页**保持 RW/NX**；CPU fetch payload 字节必触发 #PF，由 shadow IDT
     * emulator 服务。此前尝试过 MmProtectMdlSystemAddress(PAGE_EXECUTE_READ) 走
     * direct execute 路径，在 HVCI 下结构性 fail（API 要么拒 EXECUTE 要么表面
     * success 但 PTE 保持 NX，call rcx 必 #PF (NX) → __except 捕获 0xC0000005
     * 或 bugcheck 0xFC）。shadow IDT + emulator 是 HVCI 下唯一可行路径。 */
    executionCallTarget = (ULONGLONG)record->PageBase;

    /* Step 6: 持 PageTable 锁下 call KswRxpfPageFaultInstall（它拿自己的
     * g_KswRxpfPageFault.ControlLock + IPI 到所有 CPU 装 shadow IDT）。
     * VM 下 CPUID "Microsoft Hv" gate 会直接 return STATUS_NOT_SUPPORTED
     * （裸 LIDT 在 Hyper-V 下 0x1E 蓝屏），EXEC_PROOF 的 verdict 回 setup_failed。 */
    status = KswRxpfPageFaultInstall();
    setupStatus = status;
    record->LastStatus = status;
    if (!NT_SUCCESS(status)) {
        /* VM 下预期落点：Hyper-V gate 挡住 LIDT 装载。实体机才可能走通。 */
        goto Cleanup;
    }
    needPageFaultRestore = TRUE;

    /* Step 7: 开启此 record 的 emulation 并增全局 EnabledCount，让 shadow IDT
     * dispatcher 知道该 record 的 #PF 要走 emulator（否则 CHAIN 到原 handler）。 */
    InterlockedExchange(&record->EmulationEnabled, 1);
    InterlockedIncrement(&g_KswRxpfRuntime.PageTable.EnabledCount);
    InterlockedIncrement(&g_KswRxpfRuntime.Generation);

    /* Step 8: 持 PageTable 排它锁，call rcx。CPU fetch payload 第一字节 → #PF
     * → shadow IDT entry → KswRxpfPageFaultStub → KswRxpfPageFaultDispatch →
     * x64_instruction_emulator 从 WritableAlias 读 payload 字节 decode 并
     * emulate，更新 Frame.Rip 到下一条指令，iretq 继续。dispatcher 不取
     * PageTable 控制锁；保持此锁防止并行 IOCTL 注销临时记录或复用槽。 */
    workerCount = 1UL;
    __try {
        returnedValue = KswRxpfInvokeTestPage(
            (PVOID)(ULONG_PTR)executionCallTarget);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        executionException = GetExceptionCode();
        returnedValue = 0ULL;
    }

    /* Step 9: 关闭此 record 的 emulation 并减全局 EnabledCount。 */
    InterlockedExchange(&record->EmulationEnabled, 0);
    InterlockedDecrement(&g_KswRxpfRuntime.PageTable.EnabledCount);
    InterlockedIncrement(&g_KswRxpfRuntime.Generation);

    /* Step 10: 记录 emulator 实际服务的 #PF 次数（verdict 判定要用）。 */
    faultsObserved = (ULONGLONG)InterlockedCompareExchange64(
        (volatile LONG64*)&record->FaultCount, 0LL, 0LL);

    /* 汇总判定。verdict=SUCCESS 要求 (RAX 匹配) AND (fault count 非零)——后者
     * 证明 shadow IDT emulator 真的介入了 payload 执行。faultsObserved==0 时
     * 不管 RAX 对不对，都 NO_FAULTS（可能 shadow IDT 没装上，或 CPU 没 fetch）。 */
    if (executionException != STATUS_SUCCESS) {
        /* SEH 捕获了异常：shadow IDT dispatcher decline (CHAIN)，Windows 的
         * KiPageFault 继续处理 → __except 收 0xC0000005；或 emulator 内部 fail。 */
        verdict = KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_EXEC_FAILED;
        status = executionException;
    } else if (faultsObserved == 0ULL) {
        /* emulator 从没服务过 fetch → shadow IDT 未真的接管 → 不能证明 CPU 执行。 */
        verdict = KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_NO_FAULTS;
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (returnedValue != Request->expectedReturnValue) {
        /* emulator 跑了但 RAX 不符：payload 字节可能被 revert、decode 错、
         * 或 instruction 未全走完 ret。 */
        verdict = KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_RETURN_MISMATCH;
        status = STATUS_DATA_ERROR;
    } else {
        /* RAX 匹配且 fault count 非零：shadow IDT emulator 服务了 CPU fetch 且
         * payload 字节产生了期望结果。CPU 真的执行了我们写入的字节的直接证据。 */
        verdict = KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SUCCESS;
        status = STATUS_SUCCESS;
    }

Cleanup:
    /* 先撤销发布，再恢复 IDT 并等待读侧；只有成功后才释放记录所有权。 */
    KswRxpfPageTableBeginRemoveLocked(
        &g_KswRxpfRuntime.PageTable,
        record);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);
    if ((needPageFaultRestore || KswRxpfPageFaultIsInstalled()) &&
        InterlockedCompareExchange(
            &g_KswRxpfRuntime.PageTable.EnabledCount, 0, 0) == 0) {
        cleanupStatus = KswRxpfPageFaultRestore();
    }
    if (NT_SUCCESS(cleanupStatus)) {
        cleanupStatus = KswRxpfDiagnosticsWaitForHandlers(5000UL);
    }
    KswRxpfPageTableAcquireExclusive(&g_KswRxpfRuntime.PageTable);
    if (NT_SUCCESS(cleanupStatus)) {
        /* ReleaseRecord 失败时保留可按 recordId 重试的页面和 MDL。 */
        cleanupStatus = KswRxpfImageProtectionReleaseRecord(record);
    }
    if (NT_SUCCESS(cleanupStatus)) {
        KswRxpfPageTableClearRemovedLocked(
            &g_KswRxpfRuntime.PageTable,
            record);
    } else {
        record->State = KSWORD_ARK_RXPF_PAGE_STATE_ERROR;
        record->LastStatus = cleanupStatus;
        status = cleanupStatus;
        verdict = KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_EXEC_FAILED;
    }
    InterlockedIncrement(&g_KswRxpfRuntime.Generation);
    KswRxpfPageTableReleaseExclusive(&g_KswRxpfRuntime.PageTable);

Complete:
    /* 填响应：worker、RAX、verdict、lastStatus、setupStatus、faultsObserved。
     * faultsObserved 为 emulator 实际服务 fetch 的次数，verdict 判定要用。 */
    Response->workerCount = workerCount;
    Response->returnedValue = returnedValue;
    Response->faultsObserved = faultsObserved;
    Response->verdict = verdict;
    Response->lastStatus = status;
    Response->setupStatus = setupStatus;
    Response->recordId = recordId;
    Response->pageBase = pageBase;
    /* 全局 last status 更新，供 QUERY_SUPPORT 调试显示。 */
    KswRxpfSetLastStatus(status);
    /* 固定响应在实验失败时仍返回完整判定；业务状态位于 lastStatus。 */
    return STATUS_SUCCESS;
}
