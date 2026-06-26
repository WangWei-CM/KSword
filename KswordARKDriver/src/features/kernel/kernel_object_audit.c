/*++
Module Name:
    kernel_object_audit.c
Abstract:
    Read-only CID table, kernel object summary, and IPC summary IOCTLs.
Environment:
    Kernel-mode Driver Framework
--*/
#include "kernel_object_audit.h"
#include "ark/ark_driver.h"
#include "../process/process_crossview.h"
#include "../../dispatch/ioctl_validation.h"
#include <ntstrsafe.h>
#include <stdarg.h>
#define KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_CID_TABLE_RESPONSE) - sizeof(KSWORD_ARK_CID_TABLE_ENTRY))
#define KSW_KERNEL_OBJECT_DEFAULT_CID_VISIT_BUDGET 65536UL
#define KSW_KERNEL_OBJECT_HARD_CID_VISIT_BUDGET    262144UL
#define KSW_KERNEL_OBJECT_HARD_RETURN_COUNT        4096UL
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif
extern POBJECT_TYPE* PsProcessType;
extern POBJECT_TYPE* PsThreadType;
NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );
NTSYSAPI
NTSTATUS
NTAPI
PsLookupThreadByThreadId(
    _In_ HANDLE ThreadId,
    _Outptr_ PETHREAD* Thread
    );
NTKERNELAPI
POBJECT_TYPE
NTAPI
ObGetObjectType(
    _In_ PVOID Object
    );
static VOID
KswordARKKernelObjectIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++
Routine Description:
    Format and enqueue one kernel-object audit log line. 中文说明：日志只记录
    只读查询状态和计数，不包含任何可用于变更对象的凭据。
Arguments:
    Device - WDF device that owns the log channel.
    LevelText - Log level text.
    FormatText - printf-style format string.
    ... - Format arguments.
Return Value:
    None. Log formatting failures are ignored.
--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;
    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (VOID)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}
static ULONG
KswordARKKernelObjectNormalizeOffset(
    _In_ ULONG Offset
    )
/*++
Routine Description:
    Convert DynData unavailable sentinels into the protocol's display sentinel.
Arguments:
    Offset - Raw DynData offset.
Return Value:
    Usable offset or KSWORD_ARK_KERNEL_OBJECT_OFFSET_UNAVAILABLE.
--*/
{
    if (!KswordARKCrossViewOffsetPresent(Offset)) {
        return KSWORD_ARK_KERNEL_OBJECT_OFFSET_UNAVAILABLE;
    }
    return Offset;
}
static ULONG
KswordARKKernelObjectClampVisitBudget(
    _In_ ULONG RequestedBudget
    )
/*++
Routine Description:
    Clamp caller CID traversal budget to a nonzero, bounded value.
Arguments:
    RequestedBudget - Caller-provided budget, zero selects default.
Return Value:
    Safe traversal budget never exceeding the hard limit.
--*/
{
    ULONG budget = RequestedBudget;
    if (budget == 0UL) {
        budget = KSW_KERNEL_OBJECT_DEFAULT_CID_VISIT_BUDGET;
    }
    if (budget > KSW_KERNEL_OBJECT_HARD_CID_VISIT_BUDGET) {
        budget = KSW_KERNEL_OBJECT_HARD_CID_VISIT_BUDGET;
    }
    return budget;
}
static ULONG
KswordARKKernelObjectSanitizeCidFlags(
    _In_ ULONG RequestFlags
    )
/*++
Routine Description:
    Keep only supported CID enumeration selector bits and provide a safe default.
Arguments:
    RequestFlags - Caller flags from the request packet.
Return Value:
    Process/thread selector flags.
--*/
{
    ULONG flags = RequestFlags & KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_ALL;
    if (flags == 0UL) {
        flags = KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_ALL;
    }
    return flags;
}
typedef struct _KSW_KERNEL_OBJECT_CID_CONTEXT
{
    KSWORD_ARK_ENUM_CID_TABLE_RESPONSE* Response;
    KSWORD_ARK_CID_TABLE_ENTRY* Rows;
    ULONG RowCapacity;
    ULONG ExpectedObjectKind;
    ULONG StartCid;
    ULONG EndCid;
    BOOLEAN HasStartCid;
    BOOLEAN HasEndCid;
    BOOLEAN Truncated;
} KSW_KERNEL_OBJECT_CID_CONTEXT, *PKSW_KERNEL_OBJECT_CID_CONTEXT;
static BOOLEAN
KswordARKKernelObjectCidInRange(
    _In_ const KSW_KERNEL_OBJECT_CID_CONTEXT* Context,
    _In_ ULONG CidValue
    )
/*++
Routine Description:
    Apply optional CID start/end filters without changing the underlying walker.
Arguments:
    Context - CID response builder state.
    CidValue - Candidate CID value from PspCidTable.
Return Value:
    TRUE when the candidate should be emitted or counted.
--*/
{
    if (Context == NULL) {
        return FALSE;
    }
    if (Context->HasStartCid && CidValue < Context->StartCid) {
        return FALSE;
    }
    if (Context->HasEndCid && CidValue > Context->EndCid) {
        return FALSE;
    }
    return TRUE;
}
static VOID
KswordARKKernelObjectCidCallback(
    _In_ const KSW_CROSSVIEW_CID_ENTRY* Entry,
    _Inout_opt_ PVOID Context
    )
/*++
Routine Description:
    Convert one read-only CID walker callback into the public response row.
Arguments:
    Entry - Type-matched CID walker payload.
    Context - KSW_KERNEL_OBJECT_CID_CONTEXT response builder.
Return Value:
    None. Rows beyond capacity are counted and marked as truncated.
--*/
{
    KSW_KERNEL_OBJECT_CID_CONTEXT* cidContext = (KSW_KERNEL_OBJECT_CID_CONTEXT*)Context;
    KSWORD_ARK_CID_TABLE_ENTRY* row = NULL;
    if (Entry == NULL || cidContext == NULL || cidContext->Response == NULL) {
        return;
    }
    if (!KswordARKKernelObjectCidInRange(cidContext, Entry->CidValue)) {
        return;
    }
    cidContext->Response->totalCount += 1UL;
    if (cidContext->Response->returnedCount >= cidContext->RowCapacity) {
        cidContext->Truncated = TRUE;
        return;
    }
    row = &cidContext->Rows[cidContext->Response->returnedCount];
    RtlZeroMemory(row, sizeof(*row));
    row->cidValue = Entry->CidValue;
    row->handleIndex = Entry->CidValue / 4UL;
    row->expectedObjectKind = cidContext->ExpectedObjectKind;
    row->lookupStatus = Entry->Referenced ?
        KSWORD_ARK_CID_ENUM_STATUS_OK :
        KSWORD_ARK_CID_ENUM_STATUS_PARTIAL;
    row->referenceStatus = Entry->ReferenceStatus;
    row->objectAddress = Entry->ObjectAddress;
    if (Entry->Referenced) {
        row->flags |= KSWORD_ARK_CID_ENTRY_FLAG_REFERENCED;
    }
    else {
        row->flags |= KSWORD_ARK_CID_ENTRY_FLAG_DANGLING;
    }
    cidContext->Response->returnedCount += 1UL;
}
static NTSTATUS
KswordARKKernelObjectWalkExpectedCidKind(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ PVOID PspCidTableAddress,
    _In_ ULONG ExpectedObjectKind,
    _In_ ULONG MaxVisitCount,
    _Inout_ KSW_KERNEL_OBJECT_CID_CONTEXT* Context,
    _Out_ ULONG* VisitedCountOut
    )
/*++
Routine Description:
    Select the Process or Thread object type and invoke the existing read-only
    CID table walker. 中文说明：这里不复制 walker 逻辑，避免引入第二份解码路径。
Arguments:
    DynState - Current DynData snapshot.
    PspCidTableAddress - Address of the PspCidTable global variable.
    ExpectedObjectKind - Process or Thread selector.
    MaxVisitCount - Bounded leaf-entry visit budget.
    Context - Mutable response builder.
    VisitedCountOut - Receives visited leaf entries for this pass.
Return Value:
    STATUS_SUCCESS, STATUS_BUFFER_OVERFLOW, or the walker failure status.
--*/
{
    POBJECT_TYPE expectedType = NULL;
    if (VisitedCountOut != NULL) {
        *VisitedCountOut = 0UL;
    }
    if (DynState == NULL || PspCidTableAddress == NULL || Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ExpectedObjectKind == KSWORD_ARK_CID_OBJECT_KIND_PROCESS) {
        expectedType = (PsProcessType != NULL) ? *PsProcessType : NULL;
    }
    else if (ExpectedObjectKind == KSWORD_ARK_CID_OBJECT_KIND_THREAD) {
        expectedType = (PsThreadType != NULL) ? *PsThreadType : NULL;
    }
    else {
        return STATUS_INVALID_PARAMETER;
    }
    if (expectedType == NULL) {
        return STATUS_NOT_FOUND;
    }
    Context->ExpectedObjectKind = ExpectedObjectKind;
    return KswordARKCrossViewWalkCidTable(
        DynState,
        PspCidTableAddress,
        expectedType,
        MaxVisitCount,
        KswordARKKernelObjectCidCallback,
        Context,
        VisitedCountOut);
}
NTSTATUS
KswordARKDriverEnumerateCidTable(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_CID_TABLE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Enumerate process/thread objects observed through PspCidTable in read-only
    mode. 中文说明：所有遍历均由 maxVisitCount 控制，输出可截断。
Arguments:
    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Output buffer size in bytes.
    Request - Optional request; NULL selects process+thread default.
    BytesWrittenOut - Receives bytes written.
Return Value:
    STATUS_SUCCESS when a response header was produced; hard buffer failures
    return an NTSTATUS error before touching private fields.
--*/
{
    KSWORD_ARK_ENUM_CID_TABLE_RESPONSE* response = (KSWORD_ARK_ENUM_CID_TABLE_RESPONSE*)OutputBuffer;
    KSW_KERNEL_OBJECT_CID_CONTEXT context;
    KSW_DYN_STATE dynState;
    KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS fieldOffsets;
    PVOID pspCidTableAddress = NULL;
    ULONG64 missingCapabilityMask = 0ULL;
    ULONG flags = KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_ALL;
    ULONG maxVisitCount = KSW_KERNEL_OBJECT_DEFAULT_CID_VISIT_BUDGET;
    ULONG availableRows = 0UL;
    ULONG totalVisited = 0UL;
    BOOLEAN usedDynDataGlobal = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS passStatus = STATUS_SUCCESS;
    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL || OutputBufferLength < KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(response, OutputBufferLength);
    response->version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_CID_ENUM_STATUS_UNAVAILABLE;
    response->entrySize = sizeof(KSWORD_ARK_CID_TABLE_ENTRY);
    KswordARKDynDataSnapshot(&dynState);
    response->dynDataCapabilityMask = dynState.CapabilityMask;
    response->htTableCodeOffset = KswordARKKernelObjectNormalizeOffset(dynState.Kernel.HtTableCode);
    response->hteLowValueOffset = KswordARKKernelObjectNormalizeOffset(dynState.Kernel.HteLowValue);
    if (Request != NULL) {
        flags = KswordARKKernelObjectSanitizeCidFlags(Request->flags);
        maxVisitCount = KswordARKKernelObjectClampVisitBudget(Request->maxVisitCount);
    }
    response->flags = flags;
    response->maxVisitCount = maxVisitCount;
    if (OutputBufferLength > KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE) {
        availableRows = (ULONG)((OutputBufferLength - KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_CID_TABLE_ENTRY));
    }
    if (Request != NULL && Request->maxEntries != 0UL && availableRows > Request->maxEntries) {
        availableRows = Request->maxEntries;
    }
    if (availableRows > KSW_KERNEL_OBJECT_HARD_RETURN_COUNT) {
        availableRows = KSW_KERNEL_OBJECT_HARD_RETURN_COUNT;
    }
    RtlZeroMemory(&fieldOffsets, sizeof(fieldOffsets));
    RtlZeroMemory(&context, sizeof(context));
    context.Response = response;
    context.Rows = response->entries;
    context.RowCapacity = availableRows;
    if (Request != NULL) {
        context.StartCid = Request->startCid;
        context.EndCid = Request->endCid;
        context.HasStartCid = (Request->startCid != 0UL) ? TRUE : FALSE;
        context.HasEndCid = (Request->endCid != 0UL) ? TRUE : FALSE;
    }
    KswordARKCrossViewFillFieldOffsets(&dynState, &fieldOffsets);
    status = KswordARKCrossViewResolvePspCidTableAddress(
        &dynState,
        &fieldOffsets,
        &pspCidTableAddress,
        &missingCapabilityMask,
        &usedDynDataGlobal);
    UNREFERENCED_PARAMETER(usedDynDataGlobal);
    if (!NT_SUCCESS(status) || pspCidTableAddress == NULL) {
        response->lastStatus = status;
        response->status = (missingCapabilityMask != 0ULL) ?
            KSWORD_ARK_CID_ENUM_STATUS_DYNDATA_MISSING :
            KSWORD_ARK_CID_ENUM_STATUS_PSPCID_UNAVAILABLE;
        *BytesWrittenOut = KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    response->pspCidTableAddress = (ULONG64)(ULONG_PTR)pspCidTableAddress;
    if ((flags & KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_PROCESS) != 0UL) {
        ULONG visitedThisPass = 0UL;
        passStatus = KswordARKKernelObjectWalkExpectedCidKind(
            &dynState,
            pspCidTableAddress,
            KSWORD_ARK_CID_OBJECT_KIND_PROCESS,
            maxVisitCount,
            &context,
            &visitedThisPass);
        totalVisited += visitedThisPass;
        if (!NT_SUCCESS(passStatus) && NT_SUCCESS(status)) {
            status = passStatus;
        }
    }
    if ((flags & KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_THREAD) != 0UL) {
        ULONG visitedThisPass = 0UL;
        passStatus = KswordARKKernelObjectWalkExpectedCidKind(
            &dynState,
            pspCidTableAddress,
            KSWORD_ARK_CID_OBJECT_KIND_THREAD,
            maxVisitCount,
            &context,
            &visitedThisPass);
        totalVisited += visitedThisPass;
        if (!NT_SUCCESS(passStatus) && NT_SUCCESS(status)) {
            status = passStatus;
        }
    }
    response->visitedCount = totalVisited;
    response->lastStatus = status;
    if (context.Truncated) {
        response->status = KSWORD_ARK_CID_ENUM_STATUS_BUFFER_TRUNCATED;
    }
    else if (status == STATUS_BUFFER_OVERFLOW) {
        response->status = KSWORD_ARK_CID_ENUM_STATUS_BUDGET_EXHAUSTED;
    }
    else if (!NT_SUCCESS(status)) {
        response->status = (response->returnedCount != 0UL) ?
            KSWORD_ARK_CID_ENUM_STATUS_PARTIAL :
            KSWORD_ARK_CID_ENUM_STATUS_TYPE_UNAVAILABLE;
    }
    else {
        response->status = KSWORD_ARK_CID_ENUM_STATUS_OK;
    }
    *BytesWrittenOut = KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_CID_TABLE_ENTRY));
    return STATUS_SUCCESS;
}
static VOID
KswordARKKernelObjectCopyUnicodeStringToFixed(
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _In_opt_ const UNICODE_STRING* Source
    )
/*++
Routine Description:
    Copy a counted Unicode string into a fixed protocol field.
Arguments:
    Destination - Destination WCHAR array.
    DestinationChars - Destination capacity in characters.
    Source - Optional counted source string.
Return Value:
    None. The destination is always NUL-terminated when capacity is nonzero.
--*/
{
    ULONG copyChars = 0UL;
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0U) {
        return;
    }
    copyChars = (ULONG)(Source->Length / sizeof(WCHAR));
    if (copyChars >= DestinationChars) {
        copyChars = DestinationChars - 1UL;
    }
    RtlCopyMemory(Destination, Source->Buffer, (SIZE_T)copyChars * sizeof(WCHAR));
    Destination[copyChars] = L'\0';
}
static NTSTATUS
KswordARKKernelObjectReadTypeInfo(
    _In_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE* Response
    )
/*++
Routine Description:
    Read OBJECT_TYPE name/index using DynData-gated offsets only.
Arguments:
    ObjectType - Object type returned by ObGetObjectType.
    DynState - DynData snapshot containing OtName/OtIndex.
    Response - Mutable object summary response.
Return Value:
    STATUS_SUCCESS when at least one requested type field was decoded.
--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN decodedAny = FALSE;
    if (ObjectType == NULL || DynState == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    Response->objectTypeAddress = (ULONG64)(ULONG_PTR)ObjectType;
    Response->fieldFlags |= KSWORD_ARK_OBJECT_SUMMARY_FIELD_TYPE_PRESENT;
    __try {
        if (KswordARKCrossViewOffsetPresent(DynState->Kernel.OtName)) {
            UNICODE_STRING typeName;
            RtlZeroMemory(&typeName, sizeof(typeName));
            RtlCopyMemory(&typeName, (PUCHAR)ObjectType + DynState->Kernel.OtName, sizeof(typeName));
            KswordARKKernelObjectCopyUnicodeStringToFixed(
                Response->typeName,
                KSWORD_ARK_KERNEL_OBJECT_TYPE_NAME_CHARS,
                &typeName);
            Response->fieldFlags |= KSWORD_ARK_OBJECT_SUMMARY_FIELD_TYPE_NAME_PRESENT;
            decodedAny = TRUE;
        }
        if (KswordARKCrossViewOffsetPresent(DynState->Kernel.OtIndex)) {
            RtlCopyMemory(&Response->typeIndex, (PUCHAR)ObjectType + DynState->Kernel.OtIndex, sizeof(Response->typeIndex));
            Response->fieldFlags |= KSWORD_ARK_OBJECT_SUMMARY_FIELD_TYPE_INDEX_PRESENT;
            decodedAny = TRUE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    if (!decodedAny && NT_SUCCESS(status)) {
        status = STATUS_NOT_SUPPORTED;
    }
    return status;
}
static NTSTATUS
KswordARKKernelObjectReferenceByCid(
    _In_ ULONG TargetKind,
    _In_ ULONG CidValue,
    _Outptr_result_nullonfailure_ PVOID* ObjectOut
    )
/*++
Routine Description:
    Safely obtain a referenced process or thread object from a CID value.
Arguments:
    TargetKind - Process or Thread selector.
    CidValue - PID/TID value.
    ObjectOut - Receives a referenced object on success.
Return Value:
    NTSTATUS from PsLookupProcessByProcessId or PsLookupThreadByThreadId.
--*/
{
    if (ObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ObjectOut = NULL;
    if (CidValue == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (TargetKind == KSWORD_ARK_CID_OBJECT_KIND_PROCESS) {
        return PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)CidValue, (PEPROCESS*)ObjectOut);
    }
    if (TargetKind == KSWORD_ARK_CID_OBJECT_KIND_THREAD) {
        return PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)CidValue, (PETHREAD*)ObjectOut);
    }
    return STATUS_NOT_SUPPORTED;
}
NTSTATUS
KswordARKDriverQueryKernelObjectSummary(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Query a kernel object summary through a safe PID/TID lookup path.
Arguments:
    OutputBuffer - METHOD_BUFFERED response buffer.
    OutputBufferLength - Output buffer length.
    Request - Query request. The object address is diagnostic-only.
    BytesWrittenOut - Receives fixed response size.
Return Value:
    STATUS_SUCCESS when a response was produced.
--*/
{
    KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE* response =
        (KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE*)OutputBuffer;
    KSW_DYN_STATE dynState;
    PVOID object = NULL;
    POBJECT_TYPE objectType = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL || OutputBufferLength < sizeof(*response) || Request == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(response, sizeof(*response));
    response->version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->targetKind = Request->targetKind;
    response->cidValue = Request->cidValue;
    response->expectedObjectAddress = Request->expectedObjectAddress;
    response->status = KSWORD_ARK_OBJECT_SUMMARY_STATUS_UNAVAILABLE;
    response->objectHeaderStatus = KSWORD_ARK_OBJECT_HEADER_STATUS_PROFILE_MISSING;
    KswordARKDynDataSnapshot(&dynState);
    response->dynDataCapabilityMask = dynState.CapabilityMask;
    response->otNameOffset = KswordARKKernelObjectNormalizeOffset(dynState.Kernel.OtName);
    response->otIndexOffset = KswordARKKernelObjectNormalizeOffset(dynState.Kernel.OtIndex);
    status = KswordARKKernelObjectReferenceByCid(Request->targetKind, Request->cidValue, &object);
    response->lookupStatus = status;
    if (!NT_SUCCESS(status) || object == NULL) {
        response->status = KSWORD_ARK_OBJECT_SUMMARY_STATUS_LOOKUP_FAILED;
        (VOID)RtlStringCchPrintfW(
            response->detail,
            KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS,
            L"CID lookup failed; targetKind=%lu, cid=%lu, status=0x%08X.",
            Request->targetKind,
            Request->cidValue,
            (unsigned int)status);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    response->objectAddress = (ULONG64)(ULONG_PTR)object;
    response->fieldFlags |= KSWORD_ARK_OBJECT_SUMMARY_FIELD_OBJECT_PRESENT;
    __try {
        objectType = ObGetObjectType(object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        objectType = NULL;
    }
    if (objectType != NULL) {
        status = KswordARKKernelObjectReadTypeInfo(objectType, &dynState, response);
        response->typeStatus = status;
        response->status = NT_SUCCESS(status) ?
            KSWORD_ARK_OBJECT_SUMMARY_STATUS_OK :
            KSWORD_ARK_OBJECT_SUMMARY_STATUS_PARTIAL;
    }
    else {
        response->typeStatus = status;
        response->status = KSWORD_ARK_OBJECT_SUMMARY_STATUS_TYPE_QUERY_FAILED;
    }
    response->counterStatus = STATUS_NOT_SUPPORTED;
    response->objectHeaderStatus = KSWORD_ARK_OBJECT_HEADER_STATUS_PROFILE_MISSING;
    if ((Request->flags & KSWORD_ARK_OBJECT_SUMMARY_FLAG_INCLUDE_COUNTERS) != 0UL &&
        response->status == KSWORD_ARK_OBJECT_SUMMARY_STATUS_OK) {
        response->status = KSWORD_ARK_OBJECT_SUMMARY_STATUS_PARTIAL;
    }
    (VOID)RtlStringCchPrintfW(
        response->detail,
        KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS,
        L"Summary is read-only and CID-backed; ObjectHeader counters require future PDB fields.");
    ObDereferenceObject(object);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
NTSTATUS
KswordARKDriverQueryIpcSummary(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_IPC_SUMMARY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Build a read-only IPC summary. ALPC can delegate to the existing ALPC query;
    Named Pipe and Mailslot are explicit stubs for future object-manager work.
Arguments:
    OutputBuffer - METHOD_BUFFERED response buffer.
    OutputBufferLength - Output buffer length.
    Request - Query selector.
    BytesWrittenOut - Receives fixed response size.
Return Value:
    STATUS_SUCCESS when a response packet was produced.
--*/
{
    KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE* response =
        (KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE*)OutputBuffer;
    KSW_DYN_STATE dynState;
    ULONG flags = KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL;
    NTSTATUS status = STATUS_SUCCESS;
    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL || OutputBufferLength < sizeof(*response) || Request == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(response, sizeof(*response));
    response->version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->processId = Request->processId;
    response->handleValue = Request->handleValue;
    response->status = KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    KswordARKDynDataSnapshot(&dynState);
    response->dynDataCapabilityMask = dynState.CapabilityMask;
    flags = Request->flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL;
    if (flags == 0UL) {
        flags = KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL;
    }
    if ((flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALPC) != 0UL &&
        Request->processId != 0UL &&
        Request->handleValue != 0ULL) {
        KSWORD_ARK_QUERY_ALPC_PORT_REQUEST alpcRequest;
        KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE alpcResponse;
        size_t alpcBytes = 0U;
        RtlZeroMemory(&alpcRequest, sizeof(alpcRequest));
        RtlZeroMemory(&alpcResponse, sizeof(alpcResponse));
        alpcRequest.flags = KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_BASIC | KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_NAMES;
        alpcRequest.processId = Request->processId;
        alpcRequest.handleValue = Request->handleValue;
        status = KswordARKDriverQueryAlpcPort(&alpcResponse, sizeof(alpcResponse), &alpcRequest, &alpcBytes);
        response->lastStatus = status;
        response->alpcStatus = NT_SUCCESS(status) ?
            alpcResponse.queryStatus :
            KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED;
        if (NT_SUCCESS(status)) {
            response->alpcObjectAddress = alpcResponse.queryPort.objectAddress;
            response->dynDataCapabilityMask = alpcResponse.dynDataCapabilityMask;
            RtlCopyMemory(response->alpcTypeName, alpcResponse.typeName, sizeof(response->alpcTypeName));
        }
    }
    else {
        response->alpcStatus = KSWORD_ARK_IPC_SUMMARY_STATUS_STUB;
    }
    response->namedPipeStatus = ((flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_PIPE) != 0UL) ?
        KSWORD_ARK_IPC_SUMMARY_STATUS_STUB :
        KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    response->mailslotStatus = ((flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_MAILSLOT) != 0UL) ?
        KSWORD_ARK_IPC_SUMMARY_STATUS_STUB :
        KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    response->status =
        (response->alpcStatus == KSWORD_ARK_ALPC_QUERY_STATUS_OK ||
            response->alpcStatus == KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL) ?
        KSWORD_ARK_IPC_SUMMARY_STATUS_PARTIAL :
        KSWORD_ARK_IPC_SUMMARY_STATUS_STUB;
    (VOID)RtlStringCchPrintfW(
        response->detail,
        KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS,
        L"IPC summary is read-only; Named Pipe and Mailslot are protocol stubs in this phase.");
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
NTSTATUS
KswordARKKernelObjectIoctlEnumCidTable(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++
Routine Description:
    IOCTL wrapper for read-only CID table enumeration.
Arguments:
    Device - WDF device used for diagnostics.
    Request - Current WDF request.
    InputBufferLength - Optional input length.
    OutputBufferLength - Output length supplied by caller.
    BytesReturned - Receives response bytes.
Return Value:
    NTSTATUS from buffer validation or backend enumeration.
--*/
{
    KSWORD_ARK_ENUM_CID_TABLE_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_CID_TABLE_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(OutputBufferLength);
    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;
    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_ENUM_CID_TABLE_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Error", "R0 enum-cid ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }
    if (hasInput) {
        enumRequest = (KSWORD_ARK_ENUM_CID_TABLE_REQUEST*)inputBuffer;
    }
    else {
        enumRequest = &defaultRequest;
        enumRequest->version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
        enumRequest->flags = KSWORD_ARK_CID_ENUM_FLAG_INCLUDE_ALL;
        enumRequest->maxVisitCount = KSW_KERNEL_OBJECT_DEFAULT_CID_VISIT_BUDGET;
    }
    status = KswordARKRetrieveRequiredOutputBuffer(Request, KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Error", "R0 enum-cid ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }
    status = KswordARKDriverEnumerateCidTable(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (NT_SUCCESS(status) && *BytesReturned >= KSW_KERNEL_OBJECT_CID_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_CID_TABLE_RESPONSE* response = (KSWORD_ARK_ENUM_CID_TABLE_RESPONSE*)outputBuffer;
        KswordARKKernelObjectIoctlLog(
            Device,
            "Info",
            "R0 enum-cid success: status=%lu, total=%lu, returned=%lu, visited=%lu.",
            (unsigned long)response->status,
            (unsigned long)response->totalCount,
            (unsigned long)response->returnedCount,
            (unsigned long)response->visitedCount);
    }
    return status;
}
NTSTATUS
KswordARKKernelObjectIoctlQueryObjectSummary(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++
Routine Description:
    IOCTL wrapper for CID-backed kernel object summary queries.
Arguments:
    Device - WDF device used for diagnostics.
    Request - Current WDF request.
    InputBufferLength - Required input length.
    OutputBufferLength - Required output length.
    BytesReturned - Receives fixed response size.
Return Value:
    NTSTATUS from validation or backend query.
--*/
{
    KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(OutputBufferLength);
    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;
    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(*queryRequest), (PVOID*)&queryRequest, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Error", "R0 query-object-summary ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }
    status = KswordARKRetrieveRequiredOutputBuffer(Request, sizeof(KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_RESPONSE), &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Error", "R0 query-object-summary ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }
    status = KswordARKDriverQueryKernelObjectSummary(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Info", "R0 query-object-summary completed: bytes=%Iu.", *BytesReturned);
    }
    return status;
}
NTSTATUS
KswordARKKernelObjectIoctlQueryIpcSummary(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++
Routine Description:
    IOCTL wrapper for read-only IPC summary queries.
Arguments:
    Device - WDF device used for diagnostics.
    Request - Current WDF request.
    InputBufferLength - Required input length.
    OutputBufferLength - Required output length.
    BytesReturned - Receives fixed response size.
Return Value:
    NTSTATUS from validation or backend query.
--*/
{
    KSWORD_ARK_QUERY_IPC_SUMMARY_REQUEST* queryRequest = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(OutputBufferLength);
    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;
    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(*queryRequest), (PVOID*)&queryRequest, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Error", "R0 query-ipc-summary ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }
    status = KswordARKRetrieveRequiredOutputBuffer(Request, sizeof(KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE), &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Error", "R0 query-ipc-summary ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }
    status = KswordARKDriverQueryIpcSummary(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (NT_SUCCESS(status)) {
        KswordARKKernelObjectIoctlLog(Device, "Info", "R0 query-ipc-summary completed: bytes=%Iu.", *BytesReturned);
    }
    return status;
}
