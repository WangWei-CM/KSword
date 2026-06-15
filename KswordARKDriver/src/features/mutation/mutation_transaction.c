/*++

Module Name:

    mutation_transaction.c

Abstract:

    Controlled kernel mutation transaction backend.

Environment:

    Kernel-mode Driver Framework

--*/

#include "mutation_transaction.h"
#include "ark/ark_dyndata.h"
#include "ark/ark_log.h"
#include "ark/ark_safety.h"

#include <ntstrsafe.h>

#ifndef STATUS_REQUEST_NOT_ACCEPTED
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

#define KSWORD_ARK_MUTATION_INITING 1L
#define KSWORD_ARK_MUTATION_READY 2L
#define KSWORD_ARK_MUTATION_FNV_OFFSET 14695981039346656037ULL
#define KSWORD_ARK_MUTATION_FNV_PRIME 1099511628211ULL
#define KSWORD_ARK_MUTATION_USER_TOP 0x00007FFFFFFFFFFFULL
#define KSWORD_ARK_MUTATION_KERNEL_BASE 0xFFFF800000000000ULL

typedef struct _KSWORD_ARK_MUTATION_SLOT
{
    BOOLEAN InUse;
    ULONG Flags;
    ULONG Status;
    ULONG TargetKind;
    ULONG ProcessId;
    ULONG Bytes;
    ULONG RiskFlags;
    NTSTATUS LastStatus;
    ULONGLONG TransactionId;
    ULONGLONG TargetAddress;
    ULONGLONG TargetContext;
    ULONGLONG BeforeHash;
    ULONGLONG AfterHash;
    ULONGLONG TimestampTick;
    UCHAR BeforeBytes[KSWORD_ARK_MUTATION_MAX_BYTES];
    UCHAR AfterBytes[KSWORD_ARK_MUTATION_MAX_BYTES];
} KSWORD_ARK_MUTATION_SLOT;

typedef struct _KSWORD_ARK_MUTATION_STATE
{
    EX_PUSH_LOCK Lock;
    ULONGLONG NextTransactionId;
    ULONGLONG NextAuditSequence;
    KSWORD_ARK_MUTATION_SLOT Slots[KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY];
    KSWORD_ARK_MUTATION_AUDIT_ENTRY Audit[KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY];
} KSWORD_ARK_MUTATION_STATE;

static KSWORD_ARK_MUTATION_STATE g_KswordArkMutationState;
static volatile LONG g_KswordArkMutationInitState;

NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId(_In_ HANDLE ProcessId, _Outptr_ PEPROCESS* Process);

static VOID
KswordARKMutationEnsureInitialized(VOID)
/*++ Routine Description:
     Input none; initializes global lock, transaction ids, and audit sequence once.
     Processing is interlocked and wait-free after initialization. Return none. --*/
{
    LONG oldState = InterlockedCompareExchange((volatile LONG*)&g_KswordArkMutationInitState, KSWORD_ARK_MUTATION_INITING, 0L);
    if (oldState == 0L) {
        RtlZeroMemory(&g_KswordArkMutationState, sizeof(g_KswordArkMutationState));
        ExInitializePushLock(&g_KswordArkMutationState.Lock);
        g_KswordArkMutationState.NextTransactionId = 1ULL;
        g_KswordArkMutationState.NextAuditSequence = 1ULL;
        InterlockedExchange((volatile LONG*)&g_KswordArkMutationInitState, KSWORD_ARK_MUTATION_READY);
        return;
    }
    while (InterlockedCompareExchange((volatile LONG*)&g_KswordArkMutationInitState, KSWORD_ARK_MUTATION_READY, KSWORD_ARK_MUTATION_READY) != KSWORD_ARK_MUTATION_READY) {
        KeYieldProcessor();
    }
}

VOID
KswordARKMutationInitialize(VOID)
/*++ Routine Description:
     Input none; exposes explicit initialization for future DriverEntry integration.
     Processing delegates to lazy init. Return none. --*/
{
    KswordARKMutationEnsureInitialized();
}

static ULONGLONG
KswordARKMutationTick(VOID)
/*++ Routine Description:
     Input none; reads a monotonic kernel tick for audit timestamps. Returns tick. --*/
{
    LARGE_INTEGER tick;
    KeQueryTickCount(&tick);
    return (ULONGLONG)tick.QuadPart;
}

static ULONGLONG
KswordARKMutationHash(_In_reads_bytes_opt_(ByteCount) const UCHAR* Bytes, _In_ ULONG ByteCount)
/*++ Routine Description:
     Input is a bounded byte buffer; computes FNV-1a as a compact non-crypto
     before/after marker. Returns a 64-bit hash. --*/
{
    ULONGLONG hashValue = KSWORD_ARK_MUTATION_FNV_OFFSET;
    ULONG index = 0UL;
    if (Bytes == NULL || ByteCount == 0UL) {
        return hashValue;
    }
    for (index = 0UL; index < ByteCount; index += 1UL) {
        hashValue ^= (ULONGLONG)Bytes[index];
        hashValue *= KSWORD_ARK_MUTATION_FNV_PRIME;
    }
    return hashValue;
}

static BOOLEAN
KswordARKMutationOffsetPresent(_In_ ULONG Offset)
/*++ Routine Description:
     Input is a DynData offset; filters unavailable sentinels before private field
     access. Returns TRUE only for usable offsets. --*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKMutationKernelAddress(_In_ ULONGLONG Address)
/*++ Routine Description:
     Input is a virtual address integer; checks x64 canonical kernel half only.
     Returns TRUE for accepted kernel addresses. --*/
{
    if (Address <= KSWORD_ARK_MUTATION_USER_TOP) {
        return FALSE;
    }
    if (Address < KSWORD_ARK_MUTATION_KERNEL_BASE) {
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN
KswordARKMutationRangeReadable(_In_ ULONGLONG Address, _In_ ULONG Bytes)
/*++ Routine Description:
     Inputs are address and byte count; verifies canonical, non-wrapping, nonpaged
     small kernel range. It never maps physical pages, writes PTEs, or changes CR0
     WP. Returns TRUE when the whole range is safe to snapshot. --*/
{
    ULONG index = 0UL;
    if (Bytes == 0UL || Bytes > KSWORD_ARK_MUTATION_MAX_BYTES) {
        return FALSE;
    }
    if ((ULONGLONG)Bytes > (((ULONGLONG)-1) - Address + 1ULL)) {
        return FALSE;
    }
    if (!KswordARKMutationKernelAddress(Address) || !KswordARKMutationKernelAddress(Address + (ULONGLONG)Bytes - 1ULL)) {
        return FALSE;
    }
    for (index = 0UL; index < Bytes; index += 1UL) {
        if (!MmIsNonPagedSystemAddressValid((PVOID)(ULONG_PTR)(Address + (ULONGLONG)index))) {
            return FALSE;
        }
    }
    return TRUE;
}

static NTSTATUS
KswordARKMutationReadKernelBytes(_In_ ULONGLONG Address, _Out_writes_bytes_(Bytes) UCHAR* Buffer, _In_ ULONG Bytes)
/*++ Routine Description:
     Inputs are kernel virtual address and byte count; copies a checked nonpaged
     range with MmCopyMemory virtual mode. Processing is read-only. Returns full
     copy status or a validation failure. --*/
{
    MM_COPY_ADDRESS copyAddress;
    SIZE_T copied = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    if (Buffer == NULL || !KswordARKMutationRangeReadable(Address, Bytes)) {
        return STATUS_ACCESS_VIOLATION;
    }
    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.VirtualAddress = (PVOID)(ULONG_PTR)Address;
    status = MmCopyMemory(Buffer, copyAddress, (SIZE_T)Bytes, MM_COPY_MEMORY_VIRTUAL, &copied);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return (copied == (SIZE_T)Bytes) ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}

static NTSTATUS
KswordARKMutationReadPplBytes(_In_ PEPROCESS ProcessObject, _In_ const KSW_DYN_STATE* DynState, _Out_writes_bytes_(Bytes) UCHAR* Buffer, _In_ ULONG Bytes)
/*++ Routine Description:
     Inputs are EPROCESS, DynData, output buffer, and count. Processing reads
     logical Protection, SignatureLevel, SectionSignatureLevel bytes by DynData
     offsets without contiguous-layout assumptions. Returns NTSTATUS. --*/
{
    ULONG offsets[KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES] = { 0UL };
    ULONG index = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    if (ProcessObject == NULL || DynState == NULL || Buffer == NULL || Bytes == 0UL || Bytes > KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }
    offsets[0] = DynState->Kernel.EpProtection;
    offsets[1] = DynState->Kernel.EpSignatureLevel;
    offsets[2] = DynState->Kernel.EpSectionSignatureLevel;
    __try {
        for (index = 0UL; index < Bytes; index += 1UL) {
            if (!KswordARKMutationOffsetPresent(offsets[index])) {
                status = STATUS_PROCEDURE_NOT_FOUND;
                break;
            }
            RtlCopyMemory(&Buffer[index], (PUCHAR)ProcessObject + offsets[index], sizeof(Buffer[index]));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    return status;
}

static NTSTATUS
KswordARKMutationWritePplBytes(_In_ PEPROCESS ProcessObject, _In_ const KSW_DYN_STATE* DynState, _In_reads_bytes_(Bytes) const UCHAR* Buffer, _In_ ULONG Bytes)
/*++ Routine Description:
     Inputs are EPROCESS, DynData, source bytes, and count. Processing writes only
     DynData-confirmed process protection fields and verifies them; it does not
     touch CR0 WP, PTEs, or physical memory. Returns write/verify status. --*/
{
    ULONG offsets[KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES] = { 0UL };
    UCHAR verify[KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES] = { 0U };
    ULONG index = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    if (ProcessObject == NULL || DynState == NULL || Buffer == NULL || Bytes == 0UL || Bytes > KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }
    offsets[0] = DynState->Kernel.EpProtection;
    offsets[1] = DynState->Kernel.EpSignatureLevel;
    offsets[2] = DynState->Kernel.EpSectionSignatureLevel;
    __try {
        for (index = 0UL; index < Bytes; index += 1UL) {
            if (!KswordARKMutationOffsetPresent(offsets[index])) {
                status = STATUS_PROCEDURE_NOT_FOUND;
                break;
            }
            RtlCopyMemory((PUCHAR)ProcessObject + offsets[index], &Buffer[index], sizeof(Buffer[index]));
        }
        if (NT_SUCCESS(status)) {
            for (index = 0UL; index < Bytes; index += 1UL) {
                RtlCopyMemory(&verify[index], (PUCHAR)ProcessObject + offsets[index], sizeof(verify[index]));
                if (verify[index] != Buffer[index]) {
                    status = STATUS_UNSUCCESSFUL;
                    break;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    return status;
}

static NTSTATUS
KswordARKMutationGetPplTarget(_In_ ULONG ProcessId, _In_ ULONG Bytes, _In_ ULONGLONG ExpectedAddress, _Out_ ULONGLONG* TargetAddressOut, _Out_ ULONGLONG* TargetContextOut, _Out_writes_bytes_(Bytes) UCHAR* CurrentBytesOut)
/*++ Routine Description:
     Inputs are PID, byte count, optional address, and outputs. Processing checks
     DynData capability, resolves EPROCESS, requires optional address to equal
     EPROCESS+EpProtection, and reads the snapshot. Returns NTSTATUS. --*/
{
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    ULONGLONG processAddress = 0ULL;
    ULONGLONG protectionAddress = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (ProcessId == 0UL || Bytes == 0UL || Bytes > KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES || TargetAddressOut == NULL || TargetContextOut == NULL || CurrentBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);
    if ((dynState.CapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) != KSW_CAP_PROCESS_PROTECTION_PATCH) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKMutationOffsetPresent(dynState.Kernel.EpProtection) || !KswordARKMutationOffsetPresent(dynState.Kernel.EpSignatureLevel) || !KswordARKMutationOffsetPresent(dynState.Kernel.EpSectionSignatureLevel)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    processAddress = (ULONGLONG)(ULONG_PTR)processObject;
    protectionAddress = processAddress + (ULONGLONG)dynState.Kernel.EpProtection;
    if (ExpectedAddress != 0ULL && ExpectedAddress != protectionAddress) {
        status = STATUS_INVALID_PARAMETER;
    }
    else {
        status = KswordARKMutationReadPplBytes(processObject, &dynState, CurrentBytesOut, Bytes);
    }
    if (NT_SUCCESS(status)) {
        *TargetAddressOut = protectionAddress;
        *TargetContextOut = processAddress;
    }
    ObDereferenceObject(processObject);
    return status;
}

static NTSTATUS
KswordARKMutationReadPplForSlot(_In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _Out_writes_bytes_(Slot->Bytes) UCHAR* CurrentBytesOut)
/*++ Routine Description:
     Inputs are a slot and output buffer. Processing re-resolves PID, refreshes
     DynData, rejects PID reuse by EPROCESS/address mismatch, then reads bytes.
     Returns NTSTATUS. --*/
{
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    ULONGLONG processAddress = 0ULL;
    ULONGLONG protectionAddress = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (Slot == NULL || CurrentBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);
    if ((dynState.CapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) != KSW_CAP_PROCESS_PROTECTION_PATCH) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKMutationOffsetPresent(dynState.Kernel.EpProtection) || !KswordARKMutationOffsetPresent(dynState.Kernel.EpSignatureLevel) || !KswordARKMutationOffsetPresent(dynState.Kernel.EpSectionSignatureLevel)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    status = PsLookupProcessByProcessId(ULongToHandle(Slot->ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    processAddress = (ULONGLONG)(ULONG_PTR)processObject;
    protectionAddress = processAddress + (ULONGLONG)dynState.Kernel.EpProtection;
    if (processAddress != Slot->TargetContext || protectionAddress != Slot->TargetAddress) {
        status = STATUS_REVISION_MISMATCH;
    }
    else {
        status = KswordARKMutationReadPplBytes(processObject, &dynState, CurrentBytesOut, Slot->Bytes);
    }
    ObDereferenceObject(processObject);
    return status;
}

static NTSTATUS
KswordARKMutationWritePplForSlot(_In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _In_reads_bytes_(Slot->Bytes) const UCHAR* Bytes)
/*++ Routine Description:
     Inputs are a slot and source bytes. Processing revalidates PID, DynData, and
     target address before writing PPL fields. Returns guarded write status. --*/
{
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    ULONGLONG processAddress = 0ULL;
    ULONGLONG protectionAddress = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (Slot == NULL || Bytes == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);
    if ((dynState.CapabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) != KSW_CAP_PROCESS_PROTECTION_PATCH) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKMutationOffsetPresent(dynState.Kernel.EpProtection) || !KswordARKMutationOffsetPresent(dynState.Kernel.EpSignatureLevel) || !KswordARKMutationOffsetPresent(dynState.Kernel.EpSectionSignatureLevel)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    status = PsLookupProcessByProcessId(ULongToHandle(Slot->ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    processAddress = (ULONGLONG)(ULONG_PTR)processObject;
    protectionAddress = processAddress + (ULONGLONG)dynState.Kernel.EpProtection;
    if (processAddress != Slot->TargetContext || protectionAddress != Slot->TargetAddress) {
        status = STATUS_REVISION_MISMATCH;
    }
    else {
        status = KswordARKMutationWritePplBytes(processObject, &dynState, Bytes, Slot->Bytes);
    }
    ObDereferenceObject(processObject);
    return status;
}

static NTSTATUS
KswordARKMutationReadSlotBytes(_In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _Out_writes_bytes_(Slot->Bytes) UCHAR* CurrentBytesOut)
/*++ Routine Description:
     Inputs are prepared slot and output buffer. Processing dispatches to PPL or
     kernel snapshot readers. Returns target-specific read status. --*/
{
    if (Slot == NULL || CurrentBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Slot->TargetKind == KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES) {
        return KswordARKMutationReadPplForSlot(Slot, CurrentBytesOut);
    }
    if (Slot->TargetKind == KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL || Slot->TargetKind == KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN) {
        return KswordARKMutationReadKernelBytes(Slot->TargetAddress, CurrentBytesOut, Slot->Bytes);
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS
KswordARKMutationWriteSlotBytes(_In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _In_reads_bytes_(Slot->Bytes) const UCHAR* Bytes)
/*++ Routine Description:
     Inputs are prepared slot and source bytes. Processing writes only supported
     guarded target kinds; all others fail closed. Returns NTSTATUS. --*/
{
    if (Slot == NULL || Bytes == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Slot->TargetKind == KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES) {
        return KswordARKMutationWritePplForSlot(Slot, Bytes);
    }
    return STATUS_NOT_SUPPORTED;
}

static ULONG
KswordARKMutationTargetRisk(_In_ ULONG TargetKind)
/*++ Routine Description:
     Input is target kind; maps it to stable audit risk flags. Returns bitmask. --*/
{
    if (TargetKind == KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL) {
        return KSWORD_ARK_MUTATION_RISK_KERNEL_PATCH_SURFACE | KSWORD_ARK_MUTATION_RISK_CANONICAL_REQUIRED | KSWORD_ARK_MUTATION_RISK_NONPAGED_REQUIRED | KSWORD_ARK_MUTATION_RISK_SIZE_LIMITED | KSWORD_ARK_MUTATION_RISK_WRITE_BLOCKED_BY_DESIGN;
    }
    if (TargetKind == KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES) {
        return KSWORD_ARK_MUTATION_RISK_PROCESS_PROTECTION_SURFACE | KSWORD_ARK_MUTATION_RISK_DYNDATA_REQUIRED | KSWORD_ARK_MUTATION_RISK_DYNDATA_CONFIRMED | KSWORD_ARK_MUTATION_RISK_SIZE_LIMITED;
    }
    if (TargetKind == KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN) {
        return KSWORD_ARK_MUTATION_RISK_CALLBACK_UNLINK_SURFACE | KSWORD_ARK_MUTATION_RISK_PLAN_ONLY | KSWORD_ARK_MUTATION_RISK_WRITE_BLOCKED_BY_DESIGN | KSWORD_ARK_MUTATION_RISK_SIZE_LIMITED;
    }
    return KSWORD_ARK_MUTATION_RISK_NONE;
}

static ULONG
KswordARKMutationSafetyOp(_In_ ULONG TargetKind)
/*++ Routine Description:
     Input is target kind; maps it to central safety operation id. Returns id. --*/
{
    if (TargetKind == KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL) {
        return KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
    }
    if (TargetKind == KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES) {
        return KSWORD_ARK_SAFETY_OPERATION_PROCESS_SET_PROTECTION;
    }
    if (TargetKind == KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN) {
        return KSWORD_ARK_SAFETY_OPERATION_CALLBACK_REMOVE_EXTERNAL;
    }
    return KSWORD_ARK_SAFETY_OPERATION_NONE;
}

static ULONG
KswordARKMutationFailureStatus(_In_ NTSTATUS Status, _In_ ULONG DefaultStatus)
/*++ Routine Description:
     Inputs are NTSTATUS and fallback status; maps known failures into shared
     mutation status codes. Returns shared status. --*/
{
    if (Status == STATUS_REVISION_MISMATCH) {
        return KSWORD_ARK_MUTATION_STATUS_REJECTED_TARGET_CHANGED;
    }
    if (Status == STATUS_NOT_SUPPORTED) {
        return KSWORD_ARK_MUTATION_STATUS_REJECTED_UNSUPPORTED_TARGET;
    }
    if (Status == STATUS_NOT_FOUND) {
        return KSWORD_ARK_MUTATION_STATUS_REJECTED_NOT_FOUND;
    }
    if (Status == STATUS_DEVICE_BUSY) {
        return KSWORD_ARK_MUTATION_STATUS_REJECTED_BUSY;
    }
    return DefaultStatus;
}

static VOID
KswordARKMutationFillResponse(_Out_ KSWORD_ARK_MUTATION_RESPONSE* Response, _In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _In_ ULONG Status, _In_ NTSTATUS LastStatus, _In_ ULONG RiskFlags)
/*++ Routine Description:
     Inputs are response, slot, status, NTSTATUS, and risk flags. Processing copies
     bounded transaction metadata and snapshots. Return none. --*/
{
    RtlZeroMemory(Response, sizeof(*Response));
    Response->size = sizeof(*Response);
    Response->version = KSWORD_ARK_MUTATION_PROTOCOL_VERSION;
    Response->status = Status;
    Response->targetKind = Slot->TargetKind;
    Response->processId = Slot->ProcessId;
    Response->bytes = Slot->Bytes;
    Response->riskFlags = RiskFlags;
    Response->lastStatus = LastStatus;
    Response->transactionId = Slot->TransactionId;
    Response->targetAddress = Slot->TargetAddress;
    Response->targetContext = Slot->TargetContext;
    Response->beforeHash = Slot->BeforeHash;
    Response->afterHash = Slot->AfterHash;
    Response->timestampTick = Slot->TimestampTick;
    RtlCopyMemory(Response->beforeBytes, Slot->BeforeBytes, sizeof(Response->beforeBytes));
    RtlCopyMemory(Response->afterBytes, Slot->AfterBytes, sizeof(Response->afterBytes));
}

static VOID
KswordARKMutationAuditLocked(_In_ ULONG Operation, _In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _In_ ULONG Status, _In_ NTSTATUS LastStatus, _In_ ULONG Flags, _In_ ULONG RiskFlags, _In_reads_bytes_opt_(Slot->Bytes) const UCHAR* ByteData)
/*++ Routine Description:
     Inputs are event metadata and optional bytes. Processing appends one entry to
     the audit ring while caller holds the write lock. Return none. --*/
{
    ULONGLONG sequence = g_KswordArkMutationState.NextAuditSequence;
    ULONG index = (ULONG)(sequence % KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY);
    KSWORD_ARK_MUTATION_AUDIT_ENTRY* entry = &g_KswordArkMutationState.Audit[index];
    g_KswordArkMutationState.NextAuditSequence += 1ULL;
    RtlZeroMemory(entry, sizeof(*entry));
    entry->size = sizeof(*entry);
    entry->version = KSWORD_ARK_MUTATION_PROTOCOL_VERSION;
    entry->operation = Operation;
    entry->status = Status;
    entry->lastStatus = LastStatus;
    entry->targetKind = Slot->TargetKind;
    entry->riskFlags = RiskFlags;
    entry->flags = Flags;
    entry->processId = Slot->ProcessId;
    entry->bytes = Slot->Bytes;
    entry->transactionId = Slot->TransactionId;
    entry->sequence = sequence;
    entry->targetAddress = Slot->TargetAddress;
    entry->targetContext = Slot->TargetContext;
    entry->beforeHash = Slot->BeforeHash;
    entry->afterHash = Slot->AfterHash;
    entry->timestampTick = KswordARKMutationTick();
    if (ByteData != NULL && Slot->Bytes <= KSWORD_ARK_MUTATION_MAX_BYTES) {
        RtlCopyMemory(entry->byteData, ByteData, Slot->Bytes);
    }
}

static KSWORD_ARK_MUTATION_SLOT*
KswordARKMutationFindSlotLocked(_In_ ULONGLONG TransactionId)
/*++ Routine Description:
     Input is transaction id; scans active slots under caller-held lock. Returns
     matching slot pointer or NULL. --*/
{
    ULONG index = 0UL;
    if (TransactionId == 0ULL) {
        return NULL;
    }
    for (index = 0UL; index < KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY; index += 1UL) {
        if (g_KswordArkMutationState.Slots[index].InUse && g_KswordArkMutationState.Slots[index].TransactionId == TransactionId) {
            return &g_KswordArkMutationState.Slots[index];
        }
    }
    return NULL;
}

static NTSTATUS
KswordARKMutationValidatePrepare(_In_ const KSWORD_ARK_MUTATION_PREPARE_REQUEST* Request, _Out_ ULONG* ProtocolStatusOut, _Out_ ULONG* RiskFlagsOut)
/*++ Routine Description:
     Inputs are PREPARE request and output status fields. Processing validates
     size, version, flags, target kind, and length limits. Returns NTSTATUS. --*/
{
    if (ProtocolStatusOut == NULL || RiskFlagsOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_REJECTED_INVALID_REQUEST;
    *RiskFlagsOut = KSWORD_ARK_MUTATION_RISK_NONE;
    if (Request == NULL || Request->size < sizeof(KSWORD_ARK_MUTATION_PREPARE_REQUEST) || Request->version != KSWORD_ARK_MUTATION_PROTOCOL_VERSION || Request->reserved != 0UL || Request->reserved2 != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((Request->flags & ~(KSWORD_ARK_MUTATION_FLAG_FORCE | KSWORD_ARK_MUTATION_FLAG_UI_CONFIRMED | KSWORD_ARK_MUTATION_FLAG_DRY_RUN | KSWORD_ARK_MUTATION_FLAG_EXPECTED_BEFORE_PRESENT)) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->targetKind != KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL && Request->targetKind != KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES && Request->targetKind != KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN) {
        *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_REJECTED_UNKNOWN_TARGET;
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->bytes == 0UL || Request->bytes > KSWORD_ARK_MUTATION_MAX_BYTES) {
        *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_REJECTED_SIZE_LIMIT;
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->targetKind == KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES && Request->bytes > KSWORD_ARK_MUTATION_PROCESS_PROTECTION_MAX_BYTES) {
        *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_REJECTED_SIZE_LIMIT;
        return STATUS_INVALID_PARAMETER;
    }
    *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_PREPARED;
    *RiskFlagsOut = KSWORD_ARK_MUTATION_RISK_READ_SNAPSHOT_TAKEN | KswordARKMutationTargetRisk(Request->targetKind);
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKMutationPrepareSnapshot(_In_ const KSWORD_ARK_MUTATION_PREPARE_REQUEST* Request, _Out_ ULONGLONG* TargetAddressOut, _Out_ ULONGLONG* TargetContextOut, _Out_writes_bytes_(Request->bytes) UCHAR* BeforeBytesOut, _Out_ ULONG* ProtocolStatusOut, _Inout_ ULONG* RiskFlagsInOut)
/*++ Routine Description:
     Inputs are request and outputs. Processing validates target-specific address
     rules, reads before bytes, and checks optional expected-before. Returns status. --*/
{
    NTSTATUS status = STATUS_SUCCESS;
    if (Request == NULL || TargetAddressOut == NULL || TargetContextOut == NULL || BeforeBytesOut == NULL || ProtocolStatusOut == NULL || RiskFlagsInOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *TargetAddressOut = Request->targetAddress;
    *TargetContextOut = Request->targetContext;
    if (Request->targetKind == KSWORD_ARK_MUTATION_TARGET_PROCESS_PROTECTION_BYTES) {
        status = KswordARKMutationGetPplTarget(Request->processId, Request->bytes, Request->targetAddress, TargetAddressOut, TargetContextOut, BeforeBytesOut);
    }
    else if (Request->targetKind == KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL) {
        status = KswordARKMutationReadKernelBytes(Request->targetAddress, BeforeBytesOut, Request->bytes);
    }
    else if (Request->targetKind == KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN) {
        *RiskFlagsInOut |= KSWORD_ARK_MUTATION_RISK_PLAN_ONLY;
        status = KswordARKMutationReadKernelBytes(Request->targetAddress, BeforeBytesOut, Request->bytes);
    }
    else {
        *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_REJECTED_UNKNOWN_TARGET;
        return STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(status)) {
        *ProtocolStatusOut = KswordARKMutationFailureStatus(status, KSWORD_ARK_MUTATION_STATUS_READ_FAILED);
        return status;
    }
    if ((Request->flags & KSWORD_ARK_MUTATION_FLAG_EXPECTED_BEFORE_PRESENT) != 0UL && RtlCompareMemory(BeforeBytesOut, Request->expectedBeforeBytes, Request->bytes) != Request->bytes) {
        *RiskFlagsInOut |= KSWORD_ARK_MUTATION_RISK_BEFORE_MISMATCH;
        *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH;
        return STATUS_REVISION_MISMATCH;
    }
    *ProtocolStatusOut = KSWORD_ARK_MUTATION_STATUS_PREPARED;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMutationPrepare(_In_opt_ WDFDEVICE Device, _In_ const KSWORD_ARK_MUTATION_PREPARE_REQUEST* Request, _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer, _In_ size_t OutputBufferLength, _Out_ size_t* BytesWrittenOut)
/*++ Routine Description:
     Inputs are device, PREPARE request, output buffer, and length. Processing
     validates target, snapshots before bytes, assigns transactionId, and records
     audit without writing target memory. Returns NTSTATUS and response bytes. --*/
{
    KSWORD_ARK_MUTATION_RESPONSE* response = NULL;
    KSWORD_ARK_MUTATION_SLOT slot;
    KSWORD_ARK_MUTATION_SLOT* storedSlot = NULL;
    ULONG protocolStatus = KSWORD_ARK_MUTATION_STATUS_UNKNOWN;
    ULONG riskFlags = KSWORD_ARK_MUTATION_RISK_NONE;
    ULONG index = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    KswordARKMutationEnsureInitialized();
    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_MUTATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_MUTATION_RESPONSE*)OutputBuffer;
    RtlZeroMemory(&slot, sizeof(slot));
    status = KswordARKMutationValidatePrepare(Request, &protocolStatus, &riskFlags);
    if (NT_SUCCESS(status)) {
        status = KswordARKMutationPrepareSnapshot(Request, &slot.TargetAddress, &slot.TargetContext, slot.BeforeBytes, &protocolStatus, &riskFlags);
    }
    slot.InUse = TRUE;
    slot.Flags = (Request != NULL) ? Request->flags : 0UL;
    slot.Status = protocolStatus;
    slot.TargetKind = (Request != NULL) ? Request->targetKind : KSWORD_ARK_MUTATION_TARGET_UNKNOWN;
    slot.ProcessId = (Request != NULL) ? Request->processId : 0UL;
    slot.Bytes = (Request != NULL && Request->bytes <= KSWORD_ARK_MUTATION_MAX_BYTES) ? Request->bytes : 0UL;
    slot.RiskFlags = riskFlags;
    slot.LastStatus = status;
    slot.TimestampTick = KswordARKMutationTick();
    if (Request != NULL && slot.Bytes != 0UL) {
        RtlCopyMemory(slot.AfterBytes, Request->afterBytes, slot.Bytes);
    }
    slot.BeforeHash = KswordARKMutationHash(slot.BeforeBytes, slot.Bytes);
    slot.AfterHash = KswordARKMutationHash(slot.AfterBytes, slot.Bytes);
    if (NT_SUCCESS(status)) {
        ExAcquirePushLockExclusive(&g_KswordArkMutationState.Lock);
        slot.TransactionId = g_KswordArkMutationState.NextTransactionId;
        g_KswordArkMutationState.NextTransactionId += 1ULL;
        if (g_KswordArkMutationState.NextTransactionId == 0ULL) {
            g_KswordArkMutationState.NextTransactionId = 1ULL;
        }
        index = (ULONG)(slot.TransactionId % KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY);
        storedSlot = &g_KswordArkMutationState.Slots[index];
        *storedSlot = slot;
        KswordARKMutationAuditLocked(KSWORD_ARK_MUTATION_OPERATION_PREPARE, storedSlot, KSWORD_ARK_MUTATION_STATUS_PREPARED, STATUS_SUCCESS, Request->flags, storedSlot->RiskFlags, storedSlot->BeforeBytes);
        ExReleasePushLockExclusive(&g_KswordArkMutationState.Lock);
    }
    KswordARKMutationFillResponse(response, &slot, protocolStatus, status, riskFlags);
    *BytesWrittenOut = sizeof(*response);
    if (Device != NULL) {
        CHAR message[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
        if (NT_SUCCESS(RtlStringCbPrintfA(message, sizeof(message), "Mutation prepare: tx=%I64u kind=%lu target=0x%I64X bytes=%lu status=%lu last=0x%08X.", response->transactionId, (unsigned long)response->targetKind, response->targetAddress, (unsigned long)response->bytes, (unsigned long)response->status, (unsigned int)response->lastStatus))) {
            (VOID)KswordARKDriverEnqueueLogFrame(Device, NT_SUCCESS(status) ? "Info" : "Warn", message);
        }
    }
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

static NTSTATUS
KswordARKMutationSafety(_In_opt_ WDFDEVICE Device, _In_ const KSWORD_ARK_MUTATION_SLOT* Slot, _In_ ULONG RequestFlags)
/*++ Routine Description:
     Inputs are optional device, transaction slot, and request flags. Processing
     maps target kind to safety policy and sets UI_CONFIRMED only when FORCE is
     present. Returns policy status. --*/
{
    KSWORD_ARK_SAFETY_CONTEXT context;
    ULONG operation = KSWORD_ARK_SAFETY_OPERATION_NONE;
    if (Slot == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    operation = KswordARKMutationSafetyOp(Slot->TargetKind);
    if (operation == KSWORD_ARK_SAFETY_OPERATION_NONE) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(&context, sizeof(context));
    context.Operation = operation;
    context.TargetProcessId = Slot->ProcessId;
    context.ContextFlags = ((RequestFlags & KSWORD_ARK_MUTATION_FLAG_FORCE) != 0UL) ? KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED : 0UL;
    return KswordARKSafetyEvaluate(Device, &context);
}

static NTSTATUS
KswordARKMutationCommitRollback(_In_opt_ WDFDEVICE Device, _In_ const KSWORD_ARK_MUTATION_TRANSACTION_REQUEST* Request, _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer, _In_ size_t OutputBufferLength, _Out_ size_t* BytesWrittenOut, _In_ BOOLEAN Rollback)
/*++ Routine Description:
     Inputs are device, transaction request, output buffer, and rollback selector.
     Processing loads PREPARE state by transactionId, dry-runs without FORCE,
     enforces before-match and safety policy with FORCE, performs supported writes,
     verifies, and appends audit. Returns response status. --*/
{
    KSWORD_ARK_MUTATION_RESPONSE* response = NULL;
    KSWORD_ARK_MUTATION_SLOT slot;
    KSWORD_ARK_MUTATION_SLOT* storedSlot = NULL;
    UCHAR current[KSWORD_ARK_MUTATION_MAX_BYTES] = { 0U };
    UCHAR verify[KSWORD_ARK_MUTATION_MAX_BYTES] = { 0U };
    const UCHAR* desired = NULL;
    ULONG eventCode = Rollback ? KSWORD_ARK_MUTATION_OPERATION_ROLLBACK : KSWORD_ARK_MUTATION_OPERATION_COMMIT;
    ULONG statusCode = KSWORD_ARK_MUTATION_STATUS_UNKNOWN;
    ULONG riskFlags = KSWORD_ARK_MUTATION_RISK_NONE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS lastStatus = STATUS_SUCCESS;
    KswordARKMutationEnsureInitialized();
    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_MUTATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request == NULL || Request->size < sizeof(KSWORD_ARK_MUTATION_TRANSACTION_REQUEST) || Request->version != KSWORD_ARK_MUTATION_PROTOCOL_VERSION || Request->reserved != 0UL || Request->transactionId == 0ULL || ((Request->flags & ~(KSWORD_ARK_MUTATION_FLAG_FORCE | KSWORD_ARK_MUTATION_FLAG_UI_CONFIRMED | KSWORD_ARK_MUTATION_FLAG_DRY_RUN)) != 0UL)) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_MUTATION_RESPONSE*)OutputBuffer;
    RtlZeroMemory(&slot, sizeof(slot));
    ExAcquirePushLockShared(&g_KswordArkMutationState.Lock);
    storedSlot = KswordARKMutationFindSlotLocked(Request->transactionId);
    if (storedSlot != NULL) {
        slot = *storedSlot;
    }
    ExReleasePushLockShared(&g_KswordArkMutationState.Lock);
    if (storedSlot == NULL) {
        slot.TransactionId = Request->transactionId;
        KswordARKMutationFillResponse(response, &slot, KSWORD_ARK_MUTATION_STATUS_REJECTED_NOT_FOUND, STATUS_NOT_FOUND, KSWORD_ARK_MUTATION_RISK_NONE);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    desired = Rollback ? slot.BeforeBytes : slot.AfterBytes;
    riskFlags = slot.RiskFlags;
    if ((Request->flags & KSWORD_ARK_MUTATION_FLAG_FORCE) == 0UL) {
        riskFlags |= KSWORD_ARK_MUTATION_RISK_FORCE_REQUIRED | KSWORD_ARK_MUTATION_RISK_DRY_RUN;
        statusCode = KSWORD_ARK_MUTATION_STATUS_DRY_RUN;
        lastStatus = STATUS_REQUEST_NOT_ACCEPTED;
    }
    else {
        riskFlags |= KSWORD_ARK_MUTATION_RISK_FORCE_USED | KSWORD_ARK_MUTATION_RISK_POLICY_REQUIRED;
        status = KswordARKMutationReadSlotBytes(&slot, current);
        if (!NT_SUCCESS(status)) {
            riskFlags |= (status == STATUS_REVISION_MISMATCH) ? KSWORD_ARK_MUTATION_RISK_TARGET_CHANGED : 0UL;
            statusCode = KswordARKMutationFailureStatus(status, KSWORD_ARK_MUTATION_STATUS_READ_FAILED);
            lastStatus = status;
        }
        else if (Rollback && RtlCompareMemory(current, slot.BeforeBytes, slot.Bytes) == slot.Bytes) {
            riskFlags |= KSWORD_ARK_MUTATION_RISK_ROLLBACK_IDEMPOTENT;
            statusCode = KSWORD_ARK_MUTATION_STATUS_ALREADY_AT_BEFORE;
            lastStatus = STATUS_SUCCESS;
        }
        else if (!Rollback && RtlCompareMemory(current, slot.BeforeBytes, slot.Bytes) != slot.Bytes) {
            riskFlags |= KSWORD_ARK_MUTATION_RISK_BEFORE_MISMATCH;
            statusCode = KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH;
            lastStatus = STATUS_REVISION_MISMATCH;
        }
        else if (slot.TargetKind == KSWORD_ARK_MUTATION_TARGET_CALLBACK_ENTRY_UNLINK_PLAN) {
            riskFlags |= KSWORD_ARK_MUTATION_RISK_PLAN_ONLY | KSWORD_ARK_MUTATION_RISK_WRITE_BLOCKED_BY_DESIGN;
            statusCode = KSWORD_ARK_MUTATION_STATUS_REJECTED_PLAN_ONLY;
            lastStatus = STATUS_NOT_SUPPORTED;
        }
        else if (slot.TargetKind == KSWORD_ARK_MUTATION_TARGET_KERNEL_VIRTUAL_BYTES_SMALL) {
            riskFlags |= KSWORD_ARK_MUTATION_RISK_WRITE_BLOCKED_BY_DESIGN;
            statusCode = KSWORD_ARK_MUTATION_STATUS_REJECTED_UNSUPPORTED_TARGET;
            lastStatus = STATUS_NOT_SUPPORTED;
        }
        else {
            status = KswordARKMutationSafety(Device, &slot, Request->flags);
            if (!NT_SUCCESS(status)) {
                riskFlags |= KSWORD_ARK_MUTATION_RISK_POLICY_DENIED;
                statusCode = KSWORD_ARK_MUTATION_STATUS_REJECTED_SAFETY_POLICY;
                lastStatus = status;
            }
            else {
                status = KswordARKMutationWriteSlotBytes(&slot, desired);
                if (!NT_SUCCESS(status)) {
                    statusCode = KswordARKMutationFailureStatus(status, KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED);
                    lastStatus = status;
                }
                else {
                    status = KswordARKMutationReadSlotBytes(&slot, verify);
                    if (!NT_SUCCESS(status)) {
                        statusCode = KSWORD_ARK_MUTATION_STATUS_READ_FAILED;
                        lastStatus = status;
                    }
                    else if (RtlCompareMemory(verify, desired, slot.Bytes) != slot.Bytes) {
                        statusCode = KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED;
                        lastStatus = STATUS_UNSUCCESSFUL;
                    }
                    else {
                        statusCode = Rollback ? KSWORD_ARK_MUTATION_STATUS_ROLLED_BACK : KSWORD_ARK_MUTATION_STATUS_COMMITTED;
                        lastStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }
    slot.Status = statusCode;
    slot.RiskFlags = riskFlags;
    slot.LastStatus = lastStatus;
    slot.TimestampTick = KswordARKMutationTick();
    ExAcquirePushLockExclusive(&g_KswordArkMutationState.Lock);
    storedSlot = KswordARKMutationFindSlotLocked(Request->transactionId);
    if (storedSlot != NULL) {
        storedSlot->Status = slot.Status;
        storedSlot->RiskFlags = slot.RiskFlags;
        storedSlot->LastStatus = slot.LastStatus;
        storedSlot->TimestampTick = slot.TimestampTick;
        KswordARKMutationAuditLocked(eventCode, storedSlot, statusCode, lastStatus, Request->flags, riskFlags, desired);
    }
    ExReleasePushLockExclusive(&g_KswordArkMutationState.Lock);
    KswordARKMutationFillResponse(response, &slot, statusCode, lastStatus, riskFlags);
    *BytesWrittenOut = sizeof(*response);
    if (Device != NULL) {
        CHAR message[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
        if (NT_SUCCESS(RtlStringCbPrintfA(message, sizeof(message), "Mutation %s: tx=%I64u kind=%lu status=%lu last=0x%08X.", Rollback ? "rollback" : "commit", response->transactionId, (unsigned long)response->targetKind, (unsigned long)response->status, (unsigned int)response->lastStatus))) {
            (VOID)KswordARKDriverEnqueueLogFrame(Device, (response->status == KSWORD_ARK_MUTATION_STATUS_COMMITTED || response->status == KSWORD_ARK_MUTATION_STATUS_ROLLED_BACK || response->status == KSWORD_ARK_MUTATION_STATUS_ALREADY_AT_BEFORE) ? "Info" : "Warn", message);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMutationCommit(_In_opt_ WDFDEVICE Device, _In_ const KSWORD_ARK_MUTATION_TRANSACTION_REQUEST* Request, _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer, _In_ size_t OutputBufferLength, _Out_ size_t* BytesWrittenOut)
/*++ Routine Description:
     Inputs are device, transaction request, and output buffer. Processing commits
     only by transactionId through shared commit/rollback logic. Returns NTSTATUS. --*/
{
    return KswordARKMutationCommitRollback(Device, Request, OutputBuffer, OutputBufferLength, BytesWrittenOut, FALSE);
}

NTSTATUS
KswordARKMutationRollback(_In_opt_ WDFDEVICE Device, _In_ const KSWORD_ARK_MUTATION_TRANSACTION_REQUEST* Request, _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer, _In_ size_t OutputBufferLength, _Out_ size_t* BytesWrittenOut)
/*++ Routine Description:
     Inputs are device, transaction request, and output buffer. Processing restores
     the before snapshot when needed and reports idempotent success if already
     restored. Returns NTSTATUS. --*/
{
    return KswordARKMutationCommitRollback(Device, Request, OutputBuffer, OutputBufferLength, BytesWrittenOut, TRUE);
}

NTSTATUS
KswordARKMutationQueryAudit(_Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer, _In_ size_t OutputBufferLength, _In_opt_ const KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST* Request, _Out_ size_t* BytesWrittenOut)
/*++ Routine Description:
     Inputs are output buffer, length, optional query request, and byte counter.
     Processing copies recent audit entries from the ring; byteData is redacted
     unless INCLUDE_BYTES is set. Returns NTSTATUS and response bytes. --*/
{
    KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE* response = NULL;
    ULONGLONG startSequence = 0ULL;
    ULONGLONG oldestSequence = 0ULL;
    ULONGLONG nextSequence = 0ULL;
    ULONGLONG sequence = 0ULL;
    ULONG capacity = 0UL;
    ULONG maxEntries = KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY;
    ULONG returned = 0UL;
    ULONG total = 0UL;
    ULONG lost = 0UL;
    BOOLEAN includeBytes = FALSE;
    KswordARKMutationEnsureInitialized();
    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request != NULL) {
        if (Request->size < sizeof(KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST) || Request->version != KSWORD_ARK_MUTATION_PROTOCOL_VERSION || ((Request->flags & ~KSWORD_ARK_MUTATION_QUERY_AUDIT_FLAG_INCLUDE_BYTES) != 0UL)) {
            return STATUS_INVALID_PARAMETER;
        }
        if (Request->maxEntries != 0UL && Request->maxEntries < maxEntries) {
            maxEntries = Request->maxEntries;
        }
        startSequence = Request->startSequence;
        includeBytes = ((Request->flags & KSWORD_ARK_MUTATION_QUERY_AUDIT_FLAG_INCLUDE_BYTES) != 0UL) ? TRUE : FALSE;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE*)OutputBuffer;
    response->size = KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE;
    response->version = KSWORD_ARK_MUTATION_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_MUTATION_AUDIT_ENTRY);
    capacity = (ULONG)((OutputBufferLength - KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_MUTATION_AUDIT_ENTRY));
    if (capacity < maxEntries) {
        maxEntries = capacity;
    }
    ExAcquirePushLockShared(&g_KswordArkMutationState.Lock);
    nextSequence = g_KswordArkMutationState.NextAuditSequence;
    oldestSequence = (nextSequence > KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY) ? (nextSequence - KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY) : 1ULL;
    if (startSequence == 0ULL || startSequence < oldestSequence) {
        if (startSequence != 0ULL && startSequence < oldestSequence) {
            lost = (ULONG)(oldestSequence - startSequence);
        }
        startSequence = oldestSequence;
    }
    if (nextSequence > oldestSequence) {
        total = (ULONG)(nextSequence - oldestSequence);
    }
    for (sequence = startSequence; sequence < nextSequence && returned < maxEntries; sequence += 1ULL) {
        ULONG auditIndex = (ULONG)(sequence % KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY);
        const KSWORD_ARK_MUTATION_AUDIT_ENTRY* source = &g_KswordArkMutationState.Audit[auditIndex];
        KSWORD_ARK_MUTATION_AUDIT_ENTRY* destination = &response->entries[returned];
        if (source->sequence != sequence) {
            continue;
        }
        RtlCopyMemory(destination, source, sizeof(*destination));
        if (!includeBytes) {
            RtlZeroMemory(destination->byteData, sizeof(destination->byteData));
        }
        returned += 1UL;
    }
    ExReleasePushLockShared(&g_KswordArkMutationState.Lock);
    response->totalCount = total;
    response->returnedCount = returned;
    response->lostCount = lost;
    response->oldestSequence = oldestSequence;
    response->nextSequence = nextSequence;
    *BytesWrittenOut = KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE + ((size_t)returned * sizeof(KSWORD_ARK_MUTATION_AUDIT_ENTRY));
    return STATUS_SUCCESS;
}
