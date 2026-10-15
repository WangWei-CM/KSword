/*++

Module Name:

    kernel_unloaded_drivers.c

Abstract:

    Read-only MmUnloadedDrivers, PiDDBCacheTable, and CI kernel-hash cache
    enumeration backed by identity-matched DynData/PDB layouts.

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

#include "kernel_unloaded_drivers.h"
#include "kernel_unloaded_layout.h" // 功能自己的地址与布局验证边界。
#include "../../platform/runtime_signature_scan.h" // 候选内存统一走 MmCopyMemory。
#include "ci_hash_fallback.h"
#include "ark/ark_dyndata.h"
#include "../dyndata/dyndata_v4_internal.h"
#include "../../platform/kernel_object_probe.h"

#define KSW_MM_UNLOADED_DRIVER_SLOTS 50UL
#define KSW_UNLOADED_DRIVER_HARD_WALK_LIMIT 4096UL

typedef struct _KSW_UNLOADED_QUERY_CONTEXT
{
    KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE* Response;
    ULONG MaxRows;
} KSW_UNLOADED_QUERY_CONTEXT;

static BOOLEAN
KswordARKUnloadedCopyName(
    _In_ const UNICODE_STRING* Source,
    _Inout_ KSWORD_ARK_UNLOADED_DRIVER_ROW* Row
    )
/*++

Routine Description:

    Copy one separately allocated kernel UNICODE_STRING into the fixed protocol
    row under guarded reads. 中文说明：名称过长会安全截断，不会读取 MaximumLength
    之外的字节。

Return Value:

    TRUE when a non-empty, even-length name was copied.

--*/
{
    ULONG copyBytes = 0UL;
    UNICODE_STRING sourceCopy;

    RtlZeroMemory(&sourceCopy, sizeof(sourceCopy));
    if (Source == NULL || Row == NULL ||
        !KswordARKRuntimeReadMemory(Source, &sourceCopy, sizeof(sourceCopy)) ||
        sourceCopy.Buffer == NULL || sourceCopy.Length == 0U ||
        sourceCopy.Length > sourceCopy.MaximumLength ||
        (sourceCopy.Length & (sizeof(WCHAR) - 1U)) != 0U) {
        return FALSE;
    }

    copyBytes = min(
        (ULONG)sourceCopy.Length,
        (ULONG)((KSWORD_ARK_UNLOADED_DRIVER_NAME_CHARS - 1U) * sizeof(WCHAR)));
    // 名称缓冲区可能随卸载记录被释放，只允许完整的安全复制。
    if (!KswordARKRuntimeReadMemory(sourceCopy.Buffer, Row->driverName, copyBytes)) {
        RtlZeroMemory(Row->driverName, sizeof(Row->driverName)); // 清除部分复制残留。
        return FALSE; // 未取得完整名称时不发布该行。
    }

    Row->driverName[copyBytes / sizeof(WCHAR)] = L'\0';
    Row->nameLengthBytes = copyBytes;
    Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_NAME;
    return TRUE;
}

static VOID
KswordARKUnloadedAppendRow(
    _Inout_ KSW_UNLOADED_QUERY_CONTEXT* Context,
    _In_ const KSWORD_ARK_UNLOADED_DRIVER_ROW* Row
    )
/*++

Routine Description:

    Count one valid source row and copy it only while the caller's bounded
    output budget still has capacity.

Return Value:

    None.

--*/
{
    KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE* response = NULL;

    if (Context == NULL || Context->Response == NULL || Row == NULL) {
        return;
    }
    response = Context->Response;
    response->totalRows += 1UL;
    if (response->returnedRows >= Context->MaxRows) {
        response->responseFlags |=
            KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_TRUNCATED;
        return;
    }

    response->rows[response->returnedRows] = *Row;
    response->returnedRows += 1UL;
}

static VOID
KswordARKUnloadedSkipInvalidRow(
    _Inout_ KSW_UNLOADED_QUERY_CONTEXT* Context,
    _In_ NTSTATUS Status
    )
/*++

Routine Description:

    Record one unreadable source entry without aborting the remainder of the
    bounded snapshot.

Return Value:

    None.

--*/
{
    if (Context == NULL || Context->Response == NULL) {
        return;
    }
    Context->Response->skippedRows += 1UL;
    Context->Response->responseFlags |=
        KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_SKIPPED_INVALID_ROW;
    Context->Response->queryStatus =
        KSWORD_ARK_UNLOADED_DRIVER_STATUS_PARTIAL;
    Context->Response->lastStatus = Status;
}

static BOOLEAN
KswordARKUnloadedReadMmRow(
    _In_ const KSW_MM_UNLOADED_LAYOUT* Layout,
    _In_ ULONG Index,
    _Out_ KSWORD_ARK_UNLOADED_DRIVER_ROW* Row
    )
/*++

Routine Description:

    Read one fixed MmUnloadedDrivers slot using PDB or runtime-validated bounded offsets.

Return Value:

    TRUE for a populated row; FALSE for empty or unreadable slots.

--*/
{
    const UCHAR* record = NULL;
    UNICODE_STRING name;
    PVOID startAddress = NULL;
    PVOID endAddress = NULL;
    LARGE_INTEGER currentTime;

    if (Layout == NULL || Row == NULL ||
        Index >= KSW_MM_UNLOADED_DRIVER_SLOTS) {
        return FALSE;
    }
    RtlZeroMemory(Row, sizeof(*Row));
    RtlZeroMemory(&name, sizeof(name));
    RtlZeroMemory(&currentTime, sizeof(currentTime));
    record = (const UCHAR*)Layout->Records +
        ((SIZE_T)Index * Layout->RecordSize);

    // 所有记录字段逐项安全读取；记录数组不是被锁定的内核对象。
    if (!KswordARKRuntimeReadMemory(record + Layout->NameOffset, &name, sizeof(name)) ||
        !KswordARKRuntimeReadMemory(record + Layout->StartAddressOffset, &startAddress, sizeof(startAddress)) ||
        !KswordARKRuntimeReadMemory(record + Layout->EndAddressOffset, &endAddress, sizeof(endAddress)) ||
        !KswordARKRuntimeReadMemory(record + Layout->CurrentTimeOffset, &currentTime, sizeof(currentTime))) {
        return FALSE; // 并发失效或不可读记录不得进入输出。
    }

    if (!KswordARKUnloadedCopyName(&name, Row)) {
        return FALSE;
    }

    Row->source = KSWORD_ARK_UNLOADED_DRIVER_SOURCE_MM_UNLOADED_DRIVERS;
    Row->entryAddress = (ULONGLONG)(ULONG_PTR)record;
    if (startAddress != NULL) {
        Row->baseAddress = (ULONGLONG)(ULONG_PTR)startAddress;
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_BASE;
    }
    if ((ULONG_PTR)endAddress > (ULONG_PTR)startAddress) {
        Row->imageSize =
            (ULONGLONG)((ULONG_PTR)endAddress - (ULONG_PTR)startAddress);
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_SIZE;
    }
    if (currentTime.QuadPart != 0LL) {
        Row->unloadTime = (ULONGLONG)currentTime.QuadPart;
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_UNLOAD_TIME;
    }
    return TRUE;
}

static NTSTATUS
KswordARKUnloadedEnumerateMm(
    _Inout_ KSW_UNLOADED_QUERY_CONTEXT* Context
    )
/*++

Routine Description:

    Enumerate all 50 bounded MmUnloadedDrivers slots. The undocumented writer
    lock is intentionally not guessed; guarded reads report a racy partial
    snapshot when a slot changes concurrently.

Return Value:

    Semantic enumeration status.

--*/
{
    KSW_MM_UNLOADED_LAYOUT layout;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG index = 0UL;

    RtlZeroMemory(&layout, sizeof(layout));
    status = KswordARKUnloadedResolveMmLayout(&layout);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Context->Response->responseFlags |=
        KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_SNAPSHOT_RACY;
    for (index = 0UL; index < KSW_MM_UNLOADED_DRIVER_SLOTS; ++index) {
        KSWORD_ARK_UNLOADED_DRIVER_ROW row;

        RtlZeroMemory(&row, sizeof(row));
        if (KswordARKUnloadedReadMmRow(&layout, index, &row)) {
            KswordARKUnloadedAppendRow(Context, &row);
        }
    }
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKUnloadedReadPiDdbRow(
    _In_ const KSW_PIDDB_QUERY_LAYOUT* Layout,
    _In_ PVOID Entry,
    _Out_ KSWORD_ARK_UNLOADED_DRIVER_ROW* Row
    )
/*++

Routine Description:

    Project one AVL element into the unified read-only row.

Return Value:

    TRUE when every required PiDDB field was readable.

--*/
{
    UNICODE_STRING name;

    if (Layout == NULL || Entry == NULL || Row == NULL) {
        return FALSE;
    }
    RtlZeroMemory(Row, sizeof(*Row));
    RtlZeroMemory(&name, sizeof(name));

    __try {
        if (!KswordARKRuntimeReadMemory(
                (const UCHAR*)Entry + Layout->DriverNameOffset,
                &name,
                sizeof(name)) ||
            !KswordARKRuntimeReadMemory(
                (const UCHAR*)Entry + Layout->TimeDateStampOffset,
                &Row->timeDateStamp,
                sizeof(Row->timeDateStamp)) ||
            !KswordARKRuntimeReadMemory(
                (const UCHAR*)Entry + Layout->LoadStatusOffset,
                &Row->loadStatus,
                sizeof(Row->loadStatus))) {
            return FALSE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    if (!KswordARKUnloadedCopyName(&name, Row)) {
        return FALSE;
    }

    Row->source = KSWORD_ARK_UNLOADED_DRIVER_SOURCE_PIDDB_CACHE_TABLE;
    Row->entryAddress = (ULONGLONG)(ULONG_PTR)Entry;
    Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_TIMESTAMP |
        KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_LOAD_STATUS;
    return TRUE;
}

static NTSTATUS
KswordARKUnloadedEnumeratePiDdb(
    _Inout_ KSW_UNLOADED_QUERY_CONTEXT* Context
    )
/*++

Routine Description:

    Traverse PiDDB with RtlEnumerateGenericTableWithoutSplayingAvl while holding
    the exact PDB-resolved resource shared. No table field is changed.

Return Value:

    Semantic enumeration status.

--*/
{
    KSW_PIDDB_QUERY_LAYOUT layout;
    NTSTATUS status = STATUS_SUCCESS;
    PVOID restartKey = NULL;
    PVOID entry = NULL;
    ULONG walkedRows = 0UL;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(&layout, sizeof(layout));
    status = KswordARKUnloadedResolvePiDdbLayout(&layout);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    // 中文说明：RVA 可能来自运行时启发式解析，取锁前必须确认它确实是资源。
    if (!KswordARKKernelProbeResourceIsSystemResource((ULONG_PTR)layout.Lock)) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    KeEnterCriticalRegion();
    if (!ExAcquireResourceSharedLite(layout.Lock, TRUE)) {
        KeLeaveCriticalRegion();
        return STATUS_LOCK_NOT_GRANTED;
    }

    __try {
        while ((entry = RtlEnumerateGenericTableWithoutSplayingAvl(
                    layout.Table,
                    &restartKey)) != NULL) {
            KSWORD_ARK_UNLOADED_DRIVER_ROW row;

            // 损坏的 AVL 元数据不能让只读查询无限枚举；达到硬上限后保留
            // 已取得的行，并通过 truncated/partial 明确告知 R3。
            if (walkedRows >= KSW_UNLOADED_DRIVER_HARD_WALK_LIMIT) {
                Context->Response->responseFlags |=
                    KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_TRUNCATED;
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }
            RtlZeroMemory(&row, sizeof(row));
            if (KswordARKUnloadedReadPiDdbRow(&layout, entry, &row)) {
                KswordARKUnloadedAppendRow(Context, &row);
            }
            else {
                KswordARKUnloadedSkipInvalidRow(Context, STATUS_DATA_ERROR);
            }
            walkedRows += 1UL;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ExReleaseResourceLite(layout.Lock);
    KeLeaveCriticalRegion();
    return status;
}

static BOOLEAN
KswordARKUnloadedReadCiHashRow(
    _In_ const KSW_DYN_V4_CI_KERNEL_HASH_LAYOUT* Layout,
    _In_ PVOID Entry,
    _Out_ PVOID* NextEntryOut,
    _Out_ KSWORD_ARK_UNLOADED_DRIVER_ROW* Row
    )
/*++

Routine Description:

    Read one CI hash entry using the PDB-reported singly linked Next and
    DriverName offsets. Optional diagnostic fields are sampled only when present.

Return Value:

    TRUE for a readable named entry.

--*/
{
    const UCHAR* entryBytes = NULL;
    UNICODE_STRING name;
    PVOID nextEntry = NULL;
    PVOID imageBase = NULL;
    ULONG imageSize = 0UL;

    if (Layout == NULL || Entry == NULL || NextEntryOut == NULL || Row == NULL) {
        return FALSE;
    }
    RtlZeroMemory(Row, sizeof(*Row));
    RtlZeroMemory(&name, sizeof(name));
    *NextEntryOut = NULL;
    entryBytes = (const UCHAR*)Entry;

    // Every address comes from a profile or runtime candidate.  Do not use a
    // guarded raw memcpy here: MmCopyMemory must establish a complete read.
    if (!KswordARKRuntimeReadMemory(
            entryBytes + Layout->EntryNext,
            &nextEntry,
            sizeof(nextEntry)) ||
        !KswordARKRuntimeReadMemory(
            entryBytes + Layout->EntryDriverName,
            &name,
            sizeof(name))) {
        return FALSE;
    }
    if (Layout->EntryTimeDateStamp != KSW_DYN_OFFSET_UNAVAILABLE) {
        if (!KswordARKRuntimeReadMemory(
                entryBytes + Layout->EntryTimeDateStamp,
                &Row->timeDateStamp,
                sizeof(Row->timeDateStamp))) {
            return FALSE;
        }
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_TIMESTAMP;
    }
    if (Layout->EntryLoadStatus != KSW_DYN_OFFSET_UNAVAILABLE) {
        if (!KswordARKRuntimeReadMemory(
                entryBytes + Layout->EntryLoadStatus,
                &Row->loadStatus,
                sizeof(Row->loadStatus))) {
            return FALSE;
        }
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_LOAD_STATUS;
    }
    if (Layout->EntryImageBase != KSW_DYN_OFFSET_UNAVAILABLE &&
        !KswordARKRuntimeReadMemory(
            entryBytes + Layout->EntryImageBase,
            &imageBase,
            sizeof(imageBase))) {
        return FALSE;
    }
    if (Layout->EntryImageSize != KSW_DYN_OFFSET_UNAVAILABLE &&
        !KswordARKRuntimeReadMemory(
            entryBytes + Layout->EntryImageSize,
            &imageSize,
            sizeof(imageSize))) {
        return FALSE;
    }
    if (!KswordARKUnloadedCopyName(&name, Row)) {
        return FALSE;
    }

    Row->source =
        KSWORD_ARK_UNLOADED_DRIVER_SOURCE_KERNEL_HASH_BUCKET_LIST;
    Row->entryAddress = (ULONGLONG)(ULONG_PTR)Entry;
    if (imageBase != NULL) {
        Row->baseAddress = (ULONGLONG)(ULONG_PTR)imageBase;
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_BASE;
    }
    if (imageSize != 0UL) {
        Row->imageSize = imageSize;
        Row->flags |= KSWORD_ARK_UNLOADED_DRIVER_ROW_FLAG_HAS_SIZE;
    }
    *NextEntryOut = nextEntry;
    return TRUE;
}

static NTSTATUS
KswordARKUnloadedEnumerateCiHash(
    _Inout_ KSW_UNLOADED_QUERY_CONTEXT* Context
    )
/*++

Routine Description:

    Traverse the singly linked g_KernelHashBucketList under its CI resource. Both
    globals and every entry offset come from an identity-checked v4 profile, or
    from the runtime resolver when no profile matches.  Either way the lock is
    proven to be a live ERESOURCE before it is acquired: this path blocks with
    Wait=TRUE and is reachable from an IOCTL, so a misidentified object would
    fault inside the kernel's lock path on every call.

Return Value:

    Semantic enumeration status.

--*/
{
    KSW_DYN_V4_CI_KERNEL_HASH_LAYOUT layout;
    PVOID listGlobal = NULL;
    PERESOURCE hashLock = NULL;
    PVOID current = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG walkedRows = 0UL;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(&layout, sizeof(layout));
    status = KswordARKDynDataV4SnapshotCiKernelHashLayout(&layout);
    if (!NT_SUCCESS(status)) {
        status = KswordARKCiHashResolveRuntimeLayout(&layout);
        if (!NT_SUCCESS(status)) {
            // No matching PDB is not the whole explanation: fallback also ran.
            // Preserve busy/read/resource failures; an unresolved layout has its
            // own semantic status rather than pretending no scan was attempted.
            return (status == STATUS_NOT_FOUND || status == STATUS_NOT_SUPPORTED)
                ? STATUS_NOT_SUPPORTED : status;
        }
    }
    listGlobal = (PVOID)(ULONG_PTR)(
        layout.ModuleBase + layout.KernelHashBucketListRva);
    hashLock = (PERESOURCE)(ULONG_PTR)(
        layout.ModuleBase + layout.HashCacheLockRva);
    if (!KswordARKKernelProbeResourceIsSystemResource((ULONG_PTR)hashLock)) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    KeEnterCriticalRegion();
    if (!ExAcquireResourceSharedLite(hashLock, TRUE)) {
        KeLeaveCriticalRegion();
        return STATUS_LOCK_NOT_GRANTED;
    }
    if (!KswordARKRuntimeReadMemory(
            listGlobal,
            &current,
            sizeof(current))) {
        ExReleaseResourceLite(hashLock);
        KeLeaveCriticalRegion();
        return STATUS_PARTIAL_COPY;
    }
    while (current != NULL) {
        PVOID nextEntry = NULL;
        KSWORD_ARK_UNLOADED_DRIVER_ROW row;

        if (walkedRows >= KSW_UNLOADED_DRIVER_HARD_WALK_LIMIT) {
            Context->Response->responseFlags |=
                KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_TRUNCATED;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        RtlZeroMemory(&row, sizeof(row));
        if (KswordARKUnloadedReadCiHashRow(
                &layout,
                current,
                &nextEntry,
                &row)) {
            KswordARKUnloadedAppendRow(Context, &row);
        }
        else {
            KswordARKUnloadedSkipInvalidRow(Context, STATUS_DATA_ERROR);
            status = STATUS_DATA_ERROR;
            break;
        }
        if (nextEntry == current) {
            status = STATUS_DATA_ERROR;
            break;
        }
        current = nextEntry;
        walkedRows += 1UL;
    }
    ExReleaseResourceLite(hashLock);
    KeLeaveCriticalRegion();
    return status;
}

static VOID
KswordARKUnloadedFinalizeResponse(
    _Inout_ KSW_UNLOADED_QUERY_CONTEXT* Context,
    _In_ NTSTATUS EnumerationStatus
    )
/*++

Routine Description:

    Convert one backend NTSTATUS into the stable semantic response while
    preserving an already-recorded partial-row diagnostic.

Return Value:

    None.

--*/
{
    KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE* response = NULL;

    if (Context == NULL || Context->Response == NULL) {
        return;
    }
    response = Context->Response;

    // 输出容量为 0 或后端达到硬遍历上限时，即使没有复制出一行，也必须
    // 返回明确的 partial/truncated 语义，不能误报为普通 read failure。
    if ((response->responseFlags &
            KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_TRUNCATED) != 0UL) {
        response->queryStatus = KSWORD_ARK_UNLOADED_DRIVER_STATUS_PARTIAL;
        response->lastStatus = NT_SUCCESS(EnumerationStatus)
            ? STATUS_BUFFER_OVERFLOW
            : EnumerationStatus;
        return;
    }

    if (NT_SUCCESS(EnumerationStatus)) {
        if (response->queryStatus !=
            KSWORD_ARK_UNLOADED_DRIVER_STATUS_PARTIAL) {
            response->queryStatus =
                (response->responseFlags &
                    KSWORD_ARK_UNLOADED_DRIVER_RESPONSE_FLAG_TRUNCATED) != 0UL
                ? KSWORD_ARK_UNLOADED_DRIVER_STATUS_PARTIAL
                : KSWORD_ARK_UNLOADED_DRIVER_STATUS_OK;
            response->lastStatus = STATUS_SUCCESS;
        }
        return;
    }

    response->lastStatus = EnumerationStatus;
    if (EnumerationStatus == STATUS_DEVICE_NOT_READY) {
        response->queryStatus =
            KSWORD_ARK_UNLOADED_DRIVER_STATUS_DYNDATA_UNAVAILABLE;
    }
    else if (EnumerationStatus == STATUS_REVISION_MISMATCH) {
        response->queryStatus =
            KSWORD_ARK_UNLOADED_DRIVER_STATUS_MODULE_PROFILE_UNAVAILABLE;
    }
    else if (EnumerationStatus == STATUS_NOT_SUPPORTED) {
        response->queryStatus =
            KSWORD_ARK_UNLOADED_DRIVER_STATUS_LAYOUT_UNAVAILABLE;
    }
    else if (response->returnedRows != 0UL) {
        response->queryStatus = KSWORD_ARK_UNLOADED_DRIVER_STATUS_PARTIAL;
    }
    else {
        response->queryStatus =
            KSWORD_ARK_UNLOADED_DRIVER_STATUS_READ_FAILED;
    }
}

NTSTATUS
KswordARKQueryUnloadedDrivers(
    _In_ const KSWORD_ARK_QUERY_UNLOADED_DRIVERS_REQUEST* Request,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWritten)
        KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE* Response,
    _In_ SIZE_T OutputBufferLength,
    _Out_ SIZE_T* BytesWritten
    )
/*++

Routine Description:

    Validate the variable-row protocol, dispatch one of the three read-only
    sources, and always return a bounded semantic response.

Return Value:

    STATUS_SUCCESS after writing a semantic response; invalid output buffers
    return transport errors.

--*/
{
    KSW_UNLOADED_QUERY_CONTEXT context;
    ULONG rowCapacity = 0UL;
    ULONG requestedRows = 0UL;
    SIZE_T rowCapacitySize = 0U;
    NTSTATUS enumerationStatus = STATUS_SUCCESS;

    if (Request == NULL || Response == NULL || BytesWritten == NULL ||
        OutputBufferLength <
            KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE_HEADER_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWritten = 0U;
    RtlZeroMemory(Response, OutputBufferLength);
    Response->version = KSWORD_ARK_UNLOADED_DRIVER_PROTOCOL_VERSION;
    Response->size =
        KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE_HEADER_SIZE;
    Response->rowSize = sizeof(KSWORD_ARK_UNLOADED_DRIVER_ROW);
    Response->source = Request->source;
    Response->queryStatus =
        KSWORD_ARK_UNLOADED_DRIVER_STATUS_INVALID_REQUEST;
    Response->lastStatus = STATUS_INVALID_PARAMETER;

    if (Request->version != KSWORD_ARK_UNLOADED_DRIVER_PROTOCOL_VERSION ||
        Request->size != sizeof(*Request) ||
        Request->flags != 0UL ||
        Request->reserved != 0UL ||
        (Request->source !=
            KSWORD_ARK_UNLOADED_DRIVER_SOURCE_MM_UNLOADED_DRIVERS &&
         Request->source !=
            KSWORD_ARK_UNLOADED_DRIVER_SOURCE_PIDDB_CACHE_TABLE &&
         Request->source !=
            KSWORD_ARK_UNLOADED_DRIVER_SOURCE_KERNEL_HASH_BUCKET_LIST)) {
        *BytesWritten =
            KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    rowCapacitySize =
        (OutputBufferLength -
            KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_UNLOADED_DRIVER_ROW);
    rowCapacity = rowCapacitySize > KSWORD_ARK_UNLOADED_DRIVER_MAX_ROWS
        ? KSWORD_ARK_UNLOADED_DRIVER_MAX_ROWS
        : (ULONG)rowCapacitySize;
    requestedRows = Request->maxRows == 0UL
        ? KSWORD_ARK_UNLOADED_DRIVER_DEFAULT_ROWS
        : min(Request->maxRows, KSWORD_ARK_UNLOADED_DRIVER_MAX_ROWS);
    RtlZeroMemory(&context, sizeof(context));
    context.Response = Response;
    context.MaxRows = min(rowCapacity, requestedRows);

    switch (Request->source) {
    case KSWORD_ARK_UNLOADED_DRIVER_SOURCE_MM_UNLOADED_DRIVERS:
        enumerationStatus = KswordARKUnloadedEnumerateMm(&context);
        break;
    case KSWORD_ARK_UNLOADED_DRIVER_SOURCE_PIDDB_CACHE_TABLE:
        enumerationStatus = KswordARKUnloadedEnumeratePiDdb(&context);
        break;
    case KSWORD_ARK_UNLOADED_DRIVER_SOURCE_KERNEL_HASH_BUCKET_LIST:
        enumerationStatus = KswordARKUnloadedEnumerateCiHash(&context);
        break;
    default:
        enumerationStatus = STATUS_INVALID_PARAMETER;
        break;
    }

    KswordARKUnloadedFinalizeResponse(&context, enumerationStatus);
    *BytesWritten =
        KSWORD_ARK_QUERY_UNLOADED_DRIVERS_RESPONSE_HEADER_SIZE +
        ((SIZE_T)Response->returnedRows *
            sizeof(KSWORD_ARK_UNLOADED_DRIVER_ROW));
    Response->size = (ULONG)*BytesWritten;
    return STATUS_SUCCESS;
}
