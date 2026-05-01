/*++

Module Name:

    handle_query.c

Abstract:

    Phase-4 direct EPROCESS.ObjectTable handle enumeration.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_handle.h"

#include "ark/ark_dyndata.h"
#include "handle_support.h"

#define KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE) - sizeof(KSWORD_ARK_HANDLE_ENTRY))

#define KSWORD_ARK_OBJECT_GRANTED_ACCESS_MASK 0x01ffffffUL

typedef struct _HANDLE_TABLE HANDLE_TABLE, *PHANDLE_TABLE;

typedef struct _HANDLE_TABLE_ENTRY
{
    union _KSW_HANDLE_TABLE_ENTRY_LOW
    {
        PVOID Object;
        ULONG ObAttributes;
        ULONG_PTR Value;
    } Low;
    union _KSW_HANDLE_TABLE_ENTRY_HIGH
    {
        ACCESS_MASK GrantedAccess;
        LONG NextFreeTableEntry;
    } High;
} HANDLE_TABLE_ENTRY, *PHANDLE_TABLE_ENTRY;

typedef struct _OBJECT_HEADER
{
    SSIZE_T PointerCount;
    PVOID HandleCountOrNextToFree;
    EX_PUSH_LOCK Lock;
    UCHAR TypeIndex;
    UCHAR TraceFlags;
    UCHAR InfoMask;
    UCHAR Flags;
#ifdef _WIN64
    ULONG Reserved;
#endif
    PVOID ObjectCreateInfoOrQuotaBlockCharged;
    PVOID SecurityDescriptor;
    QUAD Body;
} OBJECT_HEADER, *POBJECT_HEADER;

C_ASSERT(FIELD_OFFSET(OBJECT_HEADER, Body) == sizeof(OBJECT_HEADER) - sizeof(QUAD));

typedef
_Function_class_(EX_ENUM_HANDLE_CALLBACK)
_Must_inspect_result_
BOOLEAN
NTAPI
KSWORD_EX_ENUM_HANDLE_CALLBACK(
    _In_ PHANDLE_TABLE HandleTable,
    _Inout_ PHANDLE_TABLE_ENTRY HandleTableEntry,
    _In_ HANDLE Handle,
    _In_opt_ PVOID Context
    );

typedef KSWORD_EX_ENUM_HANDLE_CALLBACK* PKSWORD_EX_ENUM_HANDLE_CALLBACK;

NTKERNELAPI
BOOLEAN
NTAPI
ExEnumHandleTable(
    _In_ PHANDLE_TABLE HandleTable,
    _In_ PKSWORD_EX_ENUM_HANDLE_CALLBACK EnumHandleProcedure,
    _Inout_ PVOID Context,
    _Out_opt_ PHANDLE Handle
    );

typedef struct _EX_PUSH_LOCK_WAIT_BLOCK* PEX_PUSH_LOCK_WAIT_BLOCK;

NTKERNELAPI
VOID
FASTCALL
ExfUnblockPushLock(
    _Inout_ PEX_PUSH_LOCK PushLock,
    _Inout_opt_ PEX_PUSH_LOCK_WAIT_BLOCK WaitBlock
    );

NTKERNELAPI
NTSTATUS
NTAPI
PsAcquireProcessExitSynchronization(
    _In_ PEPROCESS Process
    );

NTKERNELAPI
VOID
NTAPI
PsReleaseProcessExitSynchronization(
    _In_ PEPROCESS Process
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTKERNELAPI
POBJECT_TYPE
NTAPI
ObGetObjectType(
    _In_ PVOID Object
    );

typedef struct _KSWORD_ARK_HANDLE_ENUM_CONTEXT
{
    KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE* Response;
    size_t EntryCapacity;
    ULONG ProcessId;
    ULONG RequestFlags;
    KSW_DYN_STATE DynState;
    NTSTATUS LastStatus;
} KSWORD_ARK_HANDLE_ENUM_CONTEXT, *PKSWORD_ARK_HANDLE_ENUM_CONTEXT;

static BOOLEAN
KswordARKHandleHasRequiredDynData(
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Check every Phase-4 dependency, including HtHandleContentionEvent. 中文说明：
    capability 当前把 HtHandleContentionEvent 标为 optional；但直接调用
    ExEnumHandleTable 后必须解锁 entry，因此本功能把它作为强依赖处理。

Arguments:

    DynState - Snapshot captured at IOCTL time.

Return Value:

    TRUE when all required offsets/shifts are present.

--*/
{
    if (DynState == NULL) {
        return FALSE;
    }

    return
        KswordARKHandleIsOffsetPresent(DynState->Kernel.EpObjectTable) &&
        KswordARKHandleIsOffsetPresent(DynState->Kernel.HtHandleContentionEvent) &&
        KswordARKHandleIsOffsetPresent(DynState->Kernel.ObDecodeShift) &&
        KswordARKHandleIsOffsetPresent(DynState->Kernel.ObAttributesShift) &&
        KswordARKHandleIsOffsetPresent(DynState->Kernel.OtName) &&
        KswordARKHandleIsOffsetPresent(DynState->Kernel.OtIndex);
}

static VOID
KswordARKHandlePrepareEntryDynData(
    _Inout_ KSWORD_ARK_HANDLE_ENTRY* Entry,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    Copy DynData diagnostics into one response entry. 中文说明：这些字段仅用于
    展示“本次解码使用了哪些偏移/shift”，不允许 R3 用它们作为对象凭据。

Arguments:

    Entry - Mutable response row.
    DynState - Active DynData snapshot.

Return Value:

    None.

--*/
{
    if (Entry == NULL || DynState == NULL) {
        return;
    }

    Entry->dynDataCapabilityMask = DynState->CapabilityMask;
    Entry->epObjectTableOffset = KswordARKHandleNormalizeOffset(DynState->Kernel.EpObjectTable);
    Entry->htHandleContentionEventOffset = KswordARKHandleNormalizeOffset(DynState->Kernel.HtHandleContentionEvent);
    Entry->obDecodeShift = KswordARKHandleNormalizeOffset(DynState->Kernel.ObDecodeShift);
    Entry->obAttributesShift = KswordARKHandleNormalizeOffset(DynState->Kernel.ObAttributesShift);
    Entry->otNameOffset = KswordARKHandleNormalizeOffset(DynState->Kernel.OtName);
    Entry->otIndexOffset = KswordARKHandleNormalizeOffset(DynState->Kernel.OtIndex);
}

static POBJECT_HEADER
KswordARKHandleDecodeObjectHeader(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ PHANDLE_TABLE_ENTRY HandleTableEntry
    )
/*++

Routine Description:

    Decode the encoded object header pointer stored in HANDLE_TABLE_ENTRY.
    中文说明：逻辑直接对齐 System Informer 的 KphObpDecodeObject；LA57 的差异
    由 ObDecodeShift 动态数据吸收，这里不猜测系统版本。

Arguments:

    DynState - Active DynData snapshot containing ObDecodeShift.
    HandleTableEntry - Current handle table entry supplied by ExEnumHandleTable.

Return Value:

    Decoded OBJECT_HEADER pointer, or NULL when decoding is unavailable/failed.

--*/
{
#if defined(_M_X64) || defined(_M_ARM64)
    LONG_PTR objectValue = 0;

    if (DynState == NULL || HandleTableEntry == NULL) {
        return NULL;
    }
    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.ObDecodeShift)) {
        return NULL;
    }

    objectValue = (LONG_PTR)HandleTableEntry->Low.Object;
    objectValue >>= DynState->Kernel.ObDecodeShift;
    objectValue <<= 4;
    return (POBJECT_HEADER)objectValue;
#else
    UNREFERENCED_PARAMETER(DynState);
    if (HandleTableEntry == NULL) {
        return NULL;
    }
    return (POBJECT_HEADER)((ULONG_PTR)HandleTableEntry->Low.Object & ~0x7UL);
#endif
}

static ULONG
KswordARKHandleDecodeAttributes(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ PHANDLE_TABLE_ENTRY HandleTableEntry
    )
/*++

Routine Description:

    Decode HANDLE_TABLE_ENTRY attributes. 中文说明：x64/ARM64 新格式把属性位
    编在 Value 高位，ObAttributesShift 来自 DynData，避免硬编码。

Arguments:

    DynState - Active DynData snapshot containing ObAttributesShift.
    HandleTableEntry - Current handle table entry.

Return Value:

    Low attribute bits used by UI (PROTECT/INHERIT/AUDIT subset).

--*/
{
#if defined(_M_X64) || defined(_M_ARM64)
    if (DynState == NULL || HandleTableEntry == NULL) {
        return 0UL;
    }
    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.ObAttributesShift)) {
        return 0UL;
    }

    return (ULONG)((HandleTableEntry->Low.Value >> DynState->Kernel.ObAttributesShift) & 0x3UL);
#else
    UNREFERENCED_PARAMETER(DynState);
    if (HandleTableEntry == NULL) {
        return 0UL;
    }
    return (ULONG)(HandleTableEntry->Low.ObAttributes & 0x7UL);
#endif
}

static VOID
KswordARKHandleUnlockEntry(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ PHANDLE_TABLE HandleTable,
    _Inout_ PHANDLE_TABLE_ENTRY HandleTableEntry
    )
/*++

Routine Description:

    Release one entry lock held by ExEnumHandleTable. 中文说明：此处照搬
    System Informer 的解锁模型，先设置 unlocked bit，再唤醒等待者。

Arguments:

    DynState - Active DynData snapshot containing HtHandleContentionEvent.
    HandleTable - Owning handle table.
    HandleTableEntry - Entry to unlock.

Return Value:

    None.

--*/
{
    PEX_PUSH_LOCK handleContentionEvent = NULL;

    if (DynState == NULL || HandleTable == NULL || HandleTableEntry == NULL) {
        return;
    }
    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.HtHandleContentionEvent)) {
        return;
    }

    #if defined(_WIN64)
    InterlockedExchangeAdd64((volatile LONG64*)&HandleTableEntry->Low.Value, 1);
#else
    InterlockedExchangeAdd((volatile LONG*)&HandleTableEntry->Low.Value, 1);
#endif
    handleContentionEvent = (PEX_PUSH_LOCK)((PUCHAR)HandleTable + DynState->Kernel.HtHandleContentionEvent);
    if (*(PULONG_PTR)handleContentionEvent != 0UL) {
        ExfUnblockPushLock(handleContentionEvent, NULL);
    }
}

static VOID
KswordARKHandleFillEntryFromTable(
    _Inout_ KSWORD_ARK_HANDLE_ENTRY* Entry,
    _In_ PHANDLE_TABLE_ENTRY HandleTableEntry,
    _In_ HANDLE Handle,
    _In_ PKSWORD_ARK_HANDLE_ENUM_CONTEXT Context
    )
/*++

Routine Description:

    Decode one handle table row into the shared response entry. 中文说明：失败不会
    丢弃该行，DecodeStatus 会记录保守状态，方便 UI 做差异分析。

Arguments:

    Entry - Mutable response entry.
    HandleTableEntry - Raw kernel handle table entry.
    Handle - Numeric handle value supplied by ExEnumHandleTable.
    Context - Enumeration context with DynData snapshot and request flags.

Return Value:

    None.

--*/
{
    POBJECT_HEADER objectHeader = NULL;
    PVOID objectBody = NULL;
    POBJECT_TYPE objectType = NULL;

    if (Entry == NULL || HandleTableEntry == NULL || Context == NULL) {
        return;
    }

    Entry->processId = Context->ProcessId;
    Entry->handleValue = HandleToULong(Handle);
    Entry->grantedAccess = ((ULONG)HandleTableEntry->High.GrantedAccess) & KSWORD_ARK_OBJECT_GRANTED_ACCESS_MASK;
    Entry->attributes = KswordARKHandleDecodeAttributes(&Context->DynState, HandleTableEntry);
    Entry->decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_OK;
    Entry->fieldFlags |= KSWORD_ARK_HANDLE_FIELD_GRANTED_ACCESS_PRESENT;
    Entry->fieldFlags |= KSWORD_ARK_HANDLE_FIELD_ATTRIBUTES_PRESENT;
    KswordARKHandlePrepareEntryDynData(Entry, &Context->DynState);

    if ((Context->RequestFlags & KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_OBJECT) != 0UL) {
        objectHeader = KswordARKHandleDecodeObjectHeader(&Context->DynState, HandleTableEntry);
        if (objectHeader != NULL) {
            objectBody = &objectHeader->Body;
            Entry->objectAddress = (ULONG64)(ULONG_PTR)objectBody;
            Entry->fieldFlags |= KSWORD_ARK_HANDLE_FIELD_OBJECT_PRESENT;
        }
        else {
            Entry->decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_OBJECT_DECODE_FAILED;
        }
    }

    if ((Context->RequestFlags & KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_TYPE_INDEX) != 0UL) {
        if (objectBody == NULL) {
            Entry->decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_OBJECT_DECODE_FAILED;
            return;
        }

        __try {
            objectType = ObGetObjectType(objectBody);
            if (objectType != NULL && KswordARKHandleIsOffsetPresent(Context->DynState.Kernel.OtIndex)) {
                Entry->objectTypeIndex = (ULONG)(*(PUCHAR)((PUCHAR)objectType + Context->DynState.Kernel.OtIndex));
                Entry->fieldFlags |= KSWORD_ARK_HANDLE_FIELD_TYPE_INDEX_PRESENT;
            }
            else if (Entry->decodeStatus == KSWORD_ARK_HANDLE_DECODE_STATUS_OK) {
                Entry->decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_TYPE_DECODE_FAILED;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Context->LastStatus = GetExceptionCode();
            if (Entry->decodeStatus == KSWORD_ARK_HANDLE_DECODE_STATUS_OK) {
                Entry->decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_TYPE_DECODE_FAILED;
            }
        }
    }

    if (Entry->decodeStatus == KSWORD_ARK_HANDLE_DECODE_STATUS_OK &&
        (Entry->fieldFlags & KSWORD_ARK_HANDLE_FIELD_TYPE_INDEX_PRESENT) == 0UL &&
        (Context->RequestFlags & KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_TYPE_INDEX) != 0UL) {
        Entry->decodeStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_PARTIAL;
    }
}

static BOOLEAN NTAPI
KswordARKHandleEnumCallback(
    _In_ PHANDLE_TABLE HandleTable,
    _Inout_ PHANDLE_TABLE_ENTRY HandleTableEntry,
    _In_ HANDLE Handle,
    _In_opt_ PVOID Context
    )
/*++

Routine Description:

    ExEnumHandleTable callback. 中文说明：即使输出缓冲不足也继续统计 totalCount，
    每个 entry 回调结束前必须解锁当前 HandleTableEntry。

Arguments:

    HandleTable - Owning handle table.
    HandleTableEntry - Current entry locked by ExEnumHandleTable.
    Handle - Numeric handle value.
    Context - KSWORD_ARK_HANDLE_ENUM_CONTEXT pointer.

Return Value:

    FALSE to continue enumeration.

--*/
{
    PKSWORD_ARK_HANDLE_ENUM_CONTEXT enumContext = (PKSWORD_ARK_HANDLE_ENUM_CONTEXT)Context;
    KSWORD_ARK_HANDLE_ENTRY* entry = NULL;

    if (enumContext == NULL || enumContext->Response == NULL) {
        return FALSE;
    }

    if (enumContext->Response->totalCount != MAXULONG) {
        enumContext->Response->totalCount += 1UL;
    }

    if ((size_t)enumContext->Response->returnedCount < enumContext->EntryCapacity) {
        entry = &enumContext->Response->entries[enumContext->Response->returnedCount];
        RtlZeroMemory(entry, sizeof(*entry));
        KswordARKHandleFillEntryFromTable(entry, HandleTableEntry, Handle, enumContext);
        enumContext->Response->returnedCount += 1UL;
    }

    KswordARKHandleUnlockEntry(&enumContext->DynState, HandleTable, HandleTableEntry);
    return FALSE;
}

static NTSTATUS
KswordARKHandleReadProcessHandleTable(
    _In_ PEPROCESS ProcessObject,
    _In_ const KSW_DYN_STATE* DynState,
    _Outptr_result_nullonfailure_ PHANDLE_TABLE* HandleTableOut
    )
/*++

Routine Description:

    Acquire process-exit synchronization and read EPROCESS.ObjectTable. 中文说明：
    成功返回后调用方必须 PsReleaseProcessExitSynchronization。

Arguments:

    ProcessObject - Referenced target EPROCESS.
    DynState - Active DynData snapshot containing EpObjectTable.
    HandleTableOut - Receives the raw handle table pointer.

Return Value:

    STATUS_SUCCESS with the process exit lock held, or failure status.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PHANDLE_TABLE handleTable = NULL;

    if (ProcessObject == NULL || DynState == NULL || HandleTableOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *HandleTableOut = NULL;

    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.EpObjectTable)) {
        return STATUS_NOT_SUPPORTED;
    }

    status = PsAcquireProcessExitSynchronization(ProcessObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        RtlCopyMemory(&handleTable, (PUCHAR)ProcessObject + DynState->Kernel.EpObjectTable, sizeof(handleTable));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    if (!NT_SUCCESS(status) || handleTable == NULL) {
        PsReleaseProcessExitSynchronization(ProcessObject);
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

    *HandleTableOut = handleTable;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnumerateProcessHandles(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Enumerate a target process HandleTable directly from kernel mode. 中文说明：
    本函数只返回显示/差异分析字段；对象地址是诊断值，不是后续 IOCTL 的凭据。

Arguments:

    OutputBuffer - Caller output packet.
    OutputBufferLength - Output packet capacity.
    Request - Required request containing a non-zero PID.
    BytesWrittenOut - Receives actual bytes written.

Return Value:

    STATUS_SUCCESS when the response header is valid; private-field or lookup
    failures are reflected in response->overallStatus and may also be returned.

--*/
{
    KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE* response = NULL;
    KSWORD_ARK_HANDLE_ENUM_CONTEXT enumContext;
    PEPROCESS processObject = NULL;
    PHANDLE_TABLE handleTable = NULL;
    size_t entryCapacity = 0U;
    size_t totalBytesWritten = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL || Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->processId == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_HANDLE_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_HANDLE_ENTRY);
    response->processId = Request->processId;
    response->overallStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_UNAVAILABLE;
    response->lastStatus = STATUS_SUCCESS;

    RtlZeroMemory(&enumContext, sizeof(enumContext));
    enumContext.Response = response;
    enumContext.ProcessId = Request->processId;
    enumContext.RequestFlags = (Request->flags == 0UL) ? KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL : Request->flags;
    enumContext.LastStatus = STATUS_SUCCESS;
    KswordARKDynDataSnapshot(&enumContext.DynState);

    if (!KswordARKHandleHasRequiredDynData(&enumContext.DynState)) {
        response->overallStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_DYNDATA_MISSING;
        response->lastStatus = STATUS_NOT_SUPPORTED;
        totalBytesWritten = KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE;
        *BytesWrittenOut = totalBytesWritten;
        return STATUS_NOT_SUPPORTED;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        response->overallStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_LOOKUP_FAILED;
        response->lastStatus = status;
        totalBytesWritten = KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE;
        *BytesWrittenOut = totalBytesWritten;
        return status;
    }

    status = KswordARKHandleReadProcessHandleTable(processObject, &enumContext.DynState, &handleTable);
    if (!NT_SUCCESS(status)) {
        response->overallStatus = (status == STATUS_NOT_FOUND) ?
            KSWORD_ARK_HANDLE_DECODE_STATUS_HANDLE_TABLE_MISSING :
            KSWORD_ARK_HANDLE_DECODE_STATUS_PROCESS_EXITING;
        response->lastStatus = status;
        ObDereferenceObject(processObject);
        totalBytesWritten = KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE;
        *BytesWrittenOut = totalBytesWritten;
        return status;
    }

    entryCapacity = (OutputBufferLength - KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_HANDLE_ENTRY);
    enumContext.EntryCapacity = entryCapacity;

    __try {
        (VOID)ExEnumHandleTable(handleTable, KswordARKHandleEnumCallback, &enumContext, NULL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        enumContext.LastStatus = status;
    }

    PsReleaseProcessExitSynchronization(processObject);
    ObDereferenceObject(processObject);

    if (!NT_SUCCESS(status)) {
        response->overallStatus = KSWORD_ARK_HANDLE_DECODE_STATUS_READ_FAILED;
        response->lastStatus = status;
        totalBytesWritten = KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE +
            ((size_t)response->returnedCount * sizeof(KSWORD_ARK_HANDLE_ENTRY));
        *BytesWrittenOut = totalBytesWritten;
        return status;
    }

    response->overallStatus = (response->returnedCount < response->totalCount) ?
        KSWORD_ARK_HANDLE_DECODE_STATUS_BUFFER_TOO_SMALL :
        KSWORD_ARK_HANDLE_DECODE_STATUS_OK;
    response->lastStatus = enumContext.LastStatus;
    totalBytesWritten = KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_HANDLE_ENTRY));
    *BytesWrittenOut = totalBytesWritten;

    return STATUS_SUCCESS;
}
