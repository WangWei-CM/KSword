/*++

Module Name:

    process_dkom.c

Abstract:

    PspCidTable-backed process DKOM operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

/* 中文说明：PspCidTable 定位依赖 ntoskrnl 中 PsLookupProcessByProcessId 的稳定引用。 */
#define KSWORD_ARK_DKOM_SCAN_BYTES 0x180UL
/* 中文说明：HANDLE_TABLE.TableCode 的低两位表示表级别。 */
#define KSWORD_ARK_DKOM_TABLE_LEVEL_MASK 0x3ULL
/* 中文说明：一层表最多 256 个 HANDLE_TABLE_ENTRY。 */
#define KSWORD_ARK_DKOM_LEVEL0_ENTRY_COUNT 256UL
/* 中文说明：二/三层表每层最多 512 个指针槽。 */
#define KSWORD_ARK_DKOM_POINTER_ENTRY_COUNT 512UL
/* 中文说明：删除操作最多清理命中项，防止异常循环。 */
#define KSWORD_ARK_DKOM_REMOVE_MAX_ENTRIES 4UL

/* 中文说明：PsLookupProcessByProcessId 用于目标对象引用和定位 PspCidTable。 */
NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

/* 中文说明：ObGetObjectType 用于确认解码对象是否为 Process 类型。 */
NTKERNELAPI
POBJECT_TYPE
NTAPI
ObGetObjectType(
    _In_ PVOID Object
    );

/* 中文说明：PsProcessType 是目标 EPROCESS 对象类型校验基准。 */
extern POBJECT_TYPE* PsProcessType;

/* 中文说明：读取内核指针，统一加 SEH 防护。 */
static NTSTATUS
KswordARKDkomReadPointer(
    _In_ PVOID Address,
    _Out_ PVOID* PointerOut
    )
{
    PVOID pointerValue = NULL;

    if (Address == NULL || PointerOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *PointerOut = NULL;

    __try {
        RtlCopyMemory(&pointerValue, Address, sizeof(pointerValue));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    *PointerOut = pointerValue;
    return STATUS_SUCCESS;
}

/* 中文说明：写入 64 位 entry，单独封装便于异常隔离。 */
static NTSTATUS
KswordARKDkomWriteUlong64(
    _In_ PVOID Address,
    _In_ ULONGLONG Value
    )
{
    if (Address == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        *((volatile ULONGLONG*)Address) = Value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

/* 中文说明：读取 64 位 entry，失败时返回异常状态。 */
static NTSTATUS
KswordARKDkomReadUlong64(
    _In_ PVOID Address,
    _Out_ ULONGLONG* ValueOut
    )
{
    ULONGLONG value = 0ULL;

    if (Address == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ValueOut = 0ULL;

    __try {
        value = *((volatile ULONGLONG*)Address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    *ValueOut = value;
    return STATUS_SUCCESS;
}

/* 中文说明：从 x64 RIP-relative 指令解析全局变量地址。 */
static NTSTATUS
KswordARKDkomResolveRelativeAddress(
    _In_reads_bytes_(InstructionLength) const UCHAR* InstructionAddress,
    _In_ ULONG DisplacementOffset,
    _In_ ULONG InstructionLength,
    _Out_ PVOID* AddressOut
    )
{
    LONG relativeOffset = 0;
    ULONG_PTR resolvedAddress = 0UL;

    if (InstructionAddress == NULL || AddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *AddressOut = NULL;

    __try {
        RtlCopyMemory(
            &relativeOffset,
            InstructionAddress + DisplacementOffset,
            sizeof(relativeOffset));
        resolvedAddress =
            (ULONG_PTR)InstructionAddress +
            (ULONG_PTR)InstructionLength +
            (ULONG_PTR)relativeOffset;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    if (resolvedAddress == 0UL) {
        return STATUS_NOT_FOUND;
    }
    *AddressOut = (PVOID)resolvedAddress;
    return STATUS_SUCCESS;
}

/* 中文说明：匹配 mov rax/qword ptr [rip+disp32] 形式。 */
static NTSTATUS
KswordARKDkomTryResolveMovRaxRip(
    _In_reads_bytes_(KSWORD_ARK_DKOM_SCAN_BYTES) const UCHAR* ScanBase,
    _In_ ULONG Offset,
    _Out_ PVOID* PspCidTableAddressOut
    )
{
    if (ScanBase == NULL || PspCidTableAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *PspCidTableAddressOut = NULL;

    if ((Offset + 7UL) > KSWORD_ARK_DKOM_SCAN_BYTES) {
        return STATUS_NOT_FOUND;
    }

    __try {
        if (ScanBase[Offset] == 0x48U &&
            ScanBase[Offset + 1UL] == 0x8BU &&
            ScanBase[Offset + 2UL] == 0x05U) {
            return KswordARKDkomResolveRelativeAddress(
                ScanBase + Offset,
                3UL,
                7UL,
                PspCidTableAddressOut);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_NOT_FOUND;
}

/* 中文说明：匹配 mov rcx/qword ptr [rip+disp32] 形式。 */
static NTSTATUS
KswordARKDkomTryResolveMovRcxRip(
    _In_reads_bytes_(KSWORD_ARK_DKOM_SCAN_BYTES) const UCHAR* ScanBase,
    _In_ ULONG Offset,
    _Out_ PVOID* PspCidTableAddressOut
    )
{
    if (ScanBase == NULL || PspCidTableAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *PspCidTableAddressOut = NULL;

    if ((Offset + 7UL) > KSWORD_ARK_DKOM_SCAN_BYTES) {
        return STATUS_NOT_FOUND;
    }

    __try {
        if (ScanBase[Offset] == 0x48U &&
            ScanBase[Offset + 1UL] == 0x8BU &&
            ScanBase[Offset + 2UL] == 0x0DU) {
            return KswordARKDkomResolveRelativeAddress(
                ScanBase + Offset,
                3UL,
                7UL,
                PspCidTableAddressOut);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_NOT_FOUND;
}

/* 中文说明：解析 call PspReferenceCidTableEntry 后的小范围 mov 指令。 */
static NTSTATUS
KswordARKDkomTryResolveThroughCallTarget(
    _In_reads_bytes_(KSWORD_ARK_DKOM_SCAN_BYTES) const UCHAR* ScanBase,
    _In_ ULONG Offset,
    _Out_ PVOID* PspCidTableAddressOut
    )
{
    PVOID callTarget = NULL;
    const UCHAR* targetBytes = NULL;
    ULONG targetOffset = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ScanBase == NULL || PspCidTableAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *PspCidTableAddressOut = NULL;

    if ((Offset + 5UL) > KSWORD_ARK_DKOM_SCAN_BYTES) {
        return STATUS_NOT_FOUND;
    }

    __try {
        if (ScanBase[Offset] != 0xE8U) {
            return STATUS_NOT_FOUND;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    status = KswordARKDkomResolveRelativeAddress(
        ScanBase + Offset,
        1UL,
        5UL,
        &callTarget);
    if (!NT_SUCCESS(status) || callTarget == NULL) {
        return status;
    }

    targetBytes = (const UCHAR*)callTarget;
    for (targetOffset = 0UL; targetOffset < 0x40UL; ++targetOffset) {
        PVOID resolvedAddress = NULL;
        NTSTATUS movStatus = STATUS_SUCCESS;

        movStatus = KswordARKDkomTryResolveMovRaxRip(
            targetBytes,
            targetOffset,
            &resolvedAddress);
        if (NT_SUCCESS(movStatus) && resolvedAddress != NULL) {
            *PspCidTableAddressOut = resolvedAddress;
            return STATUS_SUCCESS;
        }

        movStatus = KswordARKDkomTryResolveMovRcxRip(
            targetBytes,
            targetOffset,
            &resolvedAddress);
        if (NT_SUCCESS(movStatus) && resolvedAddress != NULL) {
            *PspCidTableAddressOut = resolvedAddress;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/* 中文说明：定位 PspCidTable 全局变量地址，失败时返回 STATUS_NOT_FOUND。 */
static NTSTATUS
KswordARKDkomResolvePspCidTableAddress(
    _Out_ PVOID* PspCidTableAddressOut
    )
{
    const UCHAR* lookupBytes = (const UCHAR*)PsLookupProcessByProcessId;
    ULONG offset = 0UL;
    NTSTATUS lastStatus = STATUS_NOT_FOUND;

    if (PspCidTableAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *PspCidTableAddressOut = NULL;

    if (lookupBytes == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    for (offset = 0UL; offset < KSWORD_ARK_DKOM_SCAN_BYTES; ++offset) {
        PVOID resolvedAddress = NULL;
        NTSTATUS status = STATUS_SUCCESS;

        status = KswordARKDkomTryResolveMovRaxRip(
            lookupBytes,
            offset,
            &resolvedAddress);
        if (NT_SUCCESS(status) && resolvedAddress != NULL) {
            *PspCidTableAddressOut = resolvedAddress;
            return STATUS_SUCCESS;
        }
        if (status != STATUS_NOT_FOUND) {
            lastStatus = status;
        }

        status = KswordARKDkomTryResolveMovRcxRip(
            lookupBytes,
            offset,
            &resolvedAddress);
        if (NT_SUCCESS(status) && resolvedAddress != NULL) {
            *PspCidTableAddressOut = resolvedAddress;
            return STATUS_SUCCESS;
        }
        if (status != STATUS_NOT_FOUND) {
            lastStatus = status;
        }

        status = KswordARKDkomTryResolveThroughCallTarget(
            lookupBytes,
            offset,
            &resolvedAddress);
        if (NT_SUCCESS(status) && resolvedAddress != NULL) {
            *PspCidTableAddressOut = resolvedAddress;
            return STATUS_SUCCESS;
        }
        if (status != STATUS_NOT_FOUND) {
            lastStatus = status;
        }
    }

    return lastStatus;
}

/* 中文说明：从 HANDLE_TABLE 读取 TableCode 并拆出根表地址。 */
static NTSTATUS
KswordARKDkomReadCidTableRoot(
    _In_ PVOID PspCidTableAddress,
    _Out_ ULONGLONG* TableRootOut,
    _Out_ ULONG* TableLevelOut
    )
{
    PVOID handleTable = NULL;
    ULONGLONG tableCode = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (PspCidTableAddress == NULL || TableRootOut == NULL || TableLevelOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *TableRootOut = 0ULL;
    *TableLevelOut = 0UL;

    status = KswordARKDkomReadPointer(PspCidTableAddress, &handleTable);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (handleTable == NULL) {
        return STATUS_NOT_FOUND;
    }

    status = KswordARKDkomReadUlong64((PUCHAR)handleTable + sizeof(PVOID), &tableCode);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *TableLevelOut = (ULONG)(tableCode & KSWORD_ARK_DKOM_TABLE_LEVEL_MASK);
    *TableRootOut = tableCode & ~KSWORD_ARK_DKOM_TABLE_LEVEL_MASK;
    return (*TableRootOut == 0ULL) ? STATUS_NOT_FOUND : STATUS_SUCCESS;
}

/* 中文说明：解码 HANDLE_TABLE_ENTRY.LowValue 中的对象体指针。 */
static PVOID
KswordARKDkomDecodeCidEntryObject(
    _In_ ULONGLONG EntryValue
    )
{
    ULONGLONG objectValue = 0ULL;

    if (EntryValue == 0ULL) {
        return NULL;
    }

#if defined(_WIN64)
    /* 中文说明：现代 x64 HANDLE_TABLE_ENTRY 使用右移 0x10 的编码对象指针。 */
    objectValue = (ULONGLONG)(((LONGLONG)EntryValue) >> 0x10);
    objectValue &= 0xFFFFFFFFFFFFFFF0ULL;
#else
    objectValue = EntryValue & ~(ULONGLONG)0x7U;
#endif

    return (PVOID)(ULONG_PTR)objectValue;
}

/* 中文说明：校验候选对象是否就是目标 EPROCESS。 */
static BOOLEAN
KswordARKDkomEntryMatchesProcess(
    _In_ ULONGLONG EntryValue,
    _In_ PEPROCESS ProcessObject
    )
{
    PVOID objectBody = NULL;
    POBJECT_TYPE objectType = NULL;
    BOOLEAN matches = FALSE;

    if (EntryValue == 0ULL || ProcessObject == NULL) {
        return FALSE;
    }

    objectBody = KswordARKDkomDecodeCidEntryObject(EntryValue);
    if (objectBody != (PVOID)ProcessObject) {
        return FALSE;
    }

    __try {
        objectType = ObGetObjectType(objectBody);
        matches = (PsProcessType != NULL && objectType == *PsProcessType) ? TRUE : FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        matches = FALSE;
    }

    return matches;
}

/* 中文说明：在一层 entry 表内查找并清零目标进程对象。 */
static NTSTATUS
KswordARKDkomRemoveFromLevel0Table(
    _In_ ULONGLONG TableAddress,
    _In_ PEPROCESS ProcessObject,
    _Inout_ ULONG* RemovedEntries
    )
{
    ULONG entryIndex = 0UL;
    NTSTATUS lastStatus = STATUS_NOT_FOUND;

    if (TableAddress == 0ULL || ProcessObject == NULL || RemovedEntries == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (entryIndex = 0UL; entryIndex < KSWORD_ARK_DKOM_LEVEL0_ENTRY_COUNT; ++entryIndex) {
        PVOID entryAddress = (PVOID)(ULONG_PTR)(TableAddress + ((ULONGLONG)entryIndex * 16ULL));
        ULONGLONG entryValue = 0ULL;
        NTSTATUS status = STATUS_SUCCESS;

        status = KswordARKDkomReadUlong64(entryAddress, &entryValue);
        if (!NT_SUCCESS(status)) {
            lastStatus = status;
            continue;
        }

        if (!KswordARKDkomEntryMatchesProcess(entryValue, ProcessObject)) {
            continue;
        }

        status = KswordARKDkomWriteUlong64(entryAddress, 0ULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        if (*RemovedEntries != MAXULONG) {
            *RemovedEntries += 1UL;
        }
        lastStatus = STATUS_SUCCESS;
        if (*RemovedEntries >= KSWORD_ARK_DKOM_REMOVE_MAX_ENTRIES) {
            break;
        }
    }

    return lastStatus;
}

/* 中文说明：遍历二层表并调用一层清理。 */
static NTSTATUS
KswordARKDkomRemoveFromLevel1Table(
    _In_ ULONGLONG TableAddress,
    _In_ PEPROCESS ProcessObject,
    _Inout_ ULONG* RemovedEntries
    )
{
    ULONG pointerIndex = 0UL;
    NTSTATUS lastStatus = STATUS_NOT_FOUND;

    if (TableAddress == 0ULL || ProcessObject == NULL || RemovedEntries == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (pointerIndex = 0UL; pointerIndex < KSWORD_ARK_DKOM_POINTER_ENTRY_COUNT; ++pointerIndex) {
        ULONGLONG childTable = 0ULL;
        NTSTATUS status = STATUS_SUCCESS;

        status = KswordARKDkomReadUlong64(
            (PVOID)(ULONG_PTR)(TableAddress + ((ULONGLONG)pointerIndex * sizeof(PVOID))),
            &childTable);
        if (!NT_SUCCESS(status) || childTable == 0ULL) {
            if (!NT_SUCCESS(status)) {
                lastStatus = status;
            }
            continue;
        }

        status = KswordARKDkomRemoveFromLevel0Table(
            childTable,
            ProcessObject,
            RemovedEntries);
        if (NT_SUCCESS(status)) {
            lastStatus = STATUS_SUCCESS;
            if (*RemovedEntries >= KSWORD_ARK_DKOM_REMOVE_MAX_ENTRIES) {
                break;
            }
        }
        else if (status != STATUS_NOT_FOUND) {
            lastStatus = status;
        }
    }

    return lastStatus;
}

/* 中文说明：遍历三层表并调用二层清理。 */
static NTSTATUS
KswordARKDkomRemoveFromLevel2Table(
    _In_ ULONGLONG TableAddress,
    _In_ PEPROCESS ProcessObject,
    _Inout_ ULONG* RemovedEntries
    )
{
    ULONG pointerIndex = 0UL;
    NTSTATUS lastStatus = STATUS_NOT_FOUND;

    if (TableAddress == 0ULL || ProcessObject == NULL || RemovedEntries == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (pointerIndex = 0UL; pointerIndex < KSWORD_ARK_DKOM_POINTER_ENTRY_COUNT; ++pointerIndex) {
        ULONGLONG childTable = 0ULL;
        NTSTATUS status = STATUS_SUCCESS;

        status = KswordARKDkomReadUlong64(
            (PVOID)(ULONG_PTR)(TableAddress + ((ULONGLONG)pointerIndex * sizeof(PVOID))),
            &childTable);
        if (!NT_SUCCESS(status) || childTable == 0ULL) {
            if (!NT_SUCCESS(status)) {
                lastStatus = status;
            }
            continue;
        }

        status = KswordARKDkomRemoveFromLevel1Table(
            childTable,
            ProcessObject,
            RemovedEntries);
        if (NT_SUCCESS(status)) {
            lastStatus = STATUS_SUCCESS;
            if (*RemovedEntries >= KSWORD_ARK_DKOM_REMOVE_MAX_ENTRIES) {
                break;
            }
        }
        else if (status != STATUS_NOT_FOUND) {
            lastStatus = status;
        }
    }

    return lastStatus;
}

/* 中文说明：按 HANDLE_TABLE.TableCode 级别分发清理逻辑。 */
static NTSTATUS
KswordARKDkomRemoveProcessFromPspCidTable(
    _In_ PEPROCESS ProcessObject,
    _In_ PVOID PspCidTableAddress,
    _Out_ ULONG* RemovedEntriesOut
    )
{
    ULONGLONG tableRoot = 0ULL;
    ULONG tableLevel = 0UL;
    ULONG removedEntries = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || PspCidTableAddress == NULL || RemovedEntriesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *RemovedEntriesOut = 0UL;

    status = KswordARKDkomReadCidTableRoot(
        PspCidTableAddress,
        &tableRoot,
        &tableLevel);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (tableLevel == 0UL) {
        status = KswordARKDkomRemoveFromLevel0Table(
            tableRoot,
            ProcessObject,
            &removedEntries);
    }
    else if (tableLevel == 1UL) {
        status = KswordARKDkomRemoveFromLevel1Table(
            tableRoot,
            ProcessObject,
            &removedEntries);
    }
    else if (tableLevel == 2UL) {
        status = KswordARKDkomRemoveFromLevel2Table(
            tableRoot,
            ProcessObject,
            &removedEntries);
    }
    else {
        status = STATUS_NOT_SUPPORTED;
    }

    *RemovedEntriesOut = removedEntries;
    if (removedEntries != 0UL) {
        return STATUS_SUCCESS;
    }
    return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
}

NTSTATUS
KswordARKDriverDkomProcess(
    _In_ ULONG ProcessId,
    _In_ ULONG Action,
    _In_ ULONG Flags,
    _Out_ ULONG* OperationStatusOut,
    _Out_ ULONG* RemovedEntriesOut,
    _Out_ ULONG64* PspCidTableAddressOut,
    _Out_ ULONG64* ProcessObjectAddressOut
    )
/*++

Routine Description:

    Execute process DKOM actions. 中文说明：当前实现 PspCidTable 删除，目标对象
    由 PID 在内核中解析，不接受 R3 传入的任意 EPROCESS 地址。

Arguments:

    ProcessId - 目标 PID。
    Action - KSWORD_ARK_PROCESS_DKOM_ACTION_*。
    Flags - 预留策略位。
    OperationStatusOut - 返回协议状态。
    RemovedEntriesOut - 返回清理的 CID entry 数。
    PspCidTableAddressOut - 返回诊断用 PspCidTable 变量地址。
    ProcessObjectAddressOut - 返回诊断用 EPROCESS 地址。

Return Value:

    STATUS_SUCCESS 或定位/写入失败状态。

--*/
{
    PEPROCESS processObject = NULL;
    PVOID pspCidTableAddress = NULL;
    ULONG removedEntries = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Flags);

    if (OperationStatusOut == NULL ||
        RemovedEntriesOut == NULL ||
        PspCidTableAddressOut == NULL ||
        ProcessObjectAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_UNKNOWN;
    *RemovedEntriesOut = 0UL;
    *PspCidTableAddressOut = 0ULL;
    *ProcessObjectAddressOut = 0ULL;

    if (ProcessId == 0UL || ProcessId <= 4UL) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_LOOKUP_FAILED;
        return STATUS_INVALID_PARAMETER;
    }
    if (Action != KSWORD_ARK_PROCESS_DKOM_ACTION_REMOVE_FROM_PSP_CID_TABLE) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_OPERATION_FAILED;
        return STATUS_INVALID_PARAMETER;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_LOOKUP_FAILED;
        return status;
    }
    *ProcessObjectAddressOut = (ULONG64)(ULONG_PTR)processObject;

    status = KswordARKDkomResolvePspCidTableAddress(&pspCidTableAddress);
    if (!NT_SUCCESS(status) || pspCidTableAddress == NULL) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_UNSUPPORTED;
        ObDereferenceObject(processObject);
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }
    *PspCidTableAddressOut = (ULONG64)(ULONG_PTR)pspCidTableAddress;

    status = KswordARKDkomRemoveProcessFromPspCidTable(
        processObject,
        pspCidTableAddress,
        &removedEntries);
    *RemovedEntriesOut = removedEntries;

    if (NT_SUCCESS(status)) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_REMOVED;
    }
    else if (status == STATUS_NOT_FOUND) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_NOT_FOUND;
    }
    else if (status == STATUS_NOT_SUPPORTED || status == STATUS_PROCEDURE_NOT_FOUND) {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_UNSUPPORTED;
    }
    else {
        *OperationStatusOut = KSWORD_ARK_PROCESS_DKOM_STATUS_OPERATION_FAILED;
    }

    ObDereferenceObject(processObject);
    return status;
}
