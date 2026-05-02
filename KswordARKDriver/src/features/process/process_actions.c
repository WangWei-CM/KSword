/*++

Module Name:

    process_actions.c

Abstract:

    This file contains kernel process control operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "..\..\platform\process_resolver.h"
#include "process_extended.h"
#include <ntstrsafe.h>
#include <stdarg.h>

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTSYSAPI
HANDLE
NTAPI
PsGetProcessInheritedFromUniqueProcessId(
    _In_ PEPROCESS Process
    );

NTSYSAPI
PCHAR
NTAPI
PsGetProcessImageFileName(
    _In_ PEPROCESS Process
    );

#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME (0x0800)
#endif

#ifndef SE_SIGNING_LEVEL_UNCHECKED
#define SE_SIGNING_LEVEL_UNCHECKED 0x00
#endif

#ifndef SE_SIGNING_LEVEL_AUTHENTICODE
#define SE_SIGNING_LEVEL_AUTHENTICODE 0x04
#endif

#ifndef SE_SIGNING_LEVEL_STORE
#define SE_SIGNING_LEVEL_STORE 0x06
#endif

#ifndef SE_SIGNING_LEVEL_ANTIMALWARE
#define SE_SIGNING_LEVEL_ANTIMALWARE 0x07
#endif

#ifndef SE_SIGNING_LEVEL_MICROSOFT
#define SE_SIGNING_LEVEL_MICROSOFT 0x08
#endif

#ifndef SE_SIGNING_LEVEL_DYNAMIC_CODEGEN
#define SE_SIGNING_LEVEL_DYNAMIC_CODEGEN 0x0B
#endif

#ifndef SE_SIGNING_LEVEL_WINDOWS
#define SE_SIGNING_LEVEL_WINDOWS 0x0C
#endif

#ifndef SE_SIGNING_LEVEL_WINDOWS_TCB
#define SE_SIGNING_LEVEL_WINDOWS_TCB 0x0E
#endif

#define KSWORD_PS_PROTECTED_SIGNER_NONE ((UCHAR)0x00)
#define KSWORD_PS_PROTECTED_SIGNER_AUTHENTICODE ((UCHAR)0x01)
#define KSWORD_PS_PROTECTED_SIGNER_CODEGEN ((UCHAR)0x02)
#define KSWORD_PS_PROTECTED_SIGNER_ANTIMALWARE ((UCHAR)0x03)
#define KSWORD_PS_PROTECTED_SIGNER_LSA ((UCHAR)0x04)
#define KSWORD_PS_PROTECTED_SIGNER_WINDOWS ((UCHAR)0x05)
#define KSWORD_PS_PROTECTED_SIGNER_WINTCB ((UCHAR)0x06)

#define KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_PROCESS_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_ENTRY))
#define KSWORD_ARK_ENUM_PID_STEP 4UL
#define KSWORD_ARK_ENUM_SCAN_MAX_PID 0x00400000UL
#define KSWORD_ARK_ENUM_SCAN_MIN_PID KSWORD_ARK_ENUM_PID_STEP
#define KSWORD_ARK_PROCESS_HIDE_MAX_PIDS 256UL

typedef PEPROCESS(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

typedef struct _KSWORD_ARK_PROCESS_HIDE_STATE
{
    EX_PUSH_LOCK Lock;
    BOOLEAN Initialized;
    ULONG Count;
    ULONG Pids[KSWORD_ARK_PROCESS_HIDE_MAX_PIDS];
} KSWORD_ARK_PROCESS_HIDE_STATE;

static KSWORD_ARK_PROCESS_HIDE_STATE g_KswordArkProcessHideState;

static VOID
KswordARKDriverEnsureProcessHideStateInitialized(
    VOID
    )
/*++

Routine Description:

    初始化驱动内进程隐藏状态表。中文说明：该表只影响 Ksword 自身的 R0
    枚举标记和 R3 展示过滤，不修改 EPROCESS 链表，保证可恢复。

Arguments:

    None.

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (g_KswordArkProcessHideState.Initialized) {
        return;
    }

    RtlZeroMemory(&g_KswordArkProcessHideState, sizeof(g_KswordArkProcessHideState));
    ExInitializePushLock(&g_KswordArkProcessHideState.Lock);
    g_KswordArkProcessHideState.Initialized = TRUE;
}

static BOOLEAN
KswordARKDriverIsProcessHiddenByUi(
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    查询 PID 是否被 Ksword R0 可恢复隐藏表标记。中文说明：调用方只读取
    状态，不做写入，读锁保护 Count/Pids 的一致性。

Arguments:

    ProcessId - 目标 PID。

Return Value:

    TRUE 表示该 PID 当前被标记隐藏；FALSE 表示未隐藏。

--*/
{
    ULONG index = 0UL;
    BOOLEAN hidden = FALSE;

    KswordARKDriverEnsureProcessHideStateInitialized();
    ExAcquirePushLockShared(&g_KswordArkProcessHideState.Lock);
    for (index = 0UL; index < g_KswordArkProcessHideState.Count; ++index) {
        if (g_KswordArkProcessHideState.Pids[index] == ProcessId) {
            hidden = TRUE;
            break;
        }
    }
    ExReleasePushLockShared(&g_KswordArkProcessHideState.Lock);
    return hidden;
}

static KSWORD_PS_GET_NEXT_PROCESS_FN
KswordARKDriverResolvePsGetNextProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcess");
    return (KSWORD_PS_GET_NEXT_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

static ULONG
KswordARKDriverAlignPidToStep(
    _In_ ULONG pidValue
    )
{
    return pidValue - (pidValue % KSWORD_ARK_ENUM_PID_STEP);
}

static BOOLEAN
KswordARKDriverPidInScanRange(
    _In_ ULONG pidValue,
    _In_ ULONG scanStartPid,
    _In_ ULONG scanEndPid
    )
{
    if (pidValue < scanStartPid || pidValue > scanEndPid) {
        return FALSE;
    }
    return ((pidValue % KSWORD_ARK_ENUM_PID_STEP) == 0U) ? TRUE : FALSE;
}

static VOID
KswordARKDriverBitmapSetPid(
    _Inout_updates_bytes_(bitmapBytes) PUCHAR bitmap,
    _In_ size_t bitmapBytes,
    _In_ ULONG pidValue
    )
{
    size_t bitIndex = 0;
    size_t byteIndex = 0;
    UCHAR bitMask = 0;

    if (bitmap == NULL || bitmapBytes == 0U) {
        return;
    }

    bitIndex = (size_t)(pidValue / KSWORD_ARK_ENUM_PID_STEP);
    byteIndex = (bitIndex >> 3);
    if (byteIndex >= bitmapBytes) {
        return;
    }

    bitMask = (UCHAR)(1U << (bitIndex & 0x07U));
    bitmap[byteIndex] = (UCHAR)(bitmap[byteIndex] | bitMask);
}

static BOOLEAN
KswordARKDriverBitmapHasPid(
    _In_reads_bytes_(bitmapBytes) const UCHAR* bitmap,
    _In_ size_t bitmapBytes,
    _In_ ULONG pidValue
    )
{
    size_t bitIndex = 0;
    size_t byteIndex = 0;
    UCHAR bitMask = 0;

    if (bitmap == NULL || bitmapBytes == 0U) {
        return FALSE;
    }

    bitIndex = (size_t)(pidValue / KSWORD_ARK_ENUM_PID_STEP);
    byteIndex = (bitIndex >> 3);
    if (byteIndex >= bitmapBytes) {
        return FALSE;
    }

    bitMask = (UCHAR)(1U << (bitIndex & 0x07U));
    return ((bitmap[byteIndex] & bitMask) != 0U) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKDriverProcessDynOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    判断 DynData 提供的 EPROCESS 偏移是否可以用于安全读取。

Arguments:

    Offset - 来自统一 DynData 状态的候选偏移。

Return Value:

    偏移有效时返回 TRUE；偏移缺失或为旧版哨兵值时返回 FALSE。

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static NTSTATUS
KswordARKDriverReadProcessPointerField(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG Offset,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    按 EPROCESS 偏移读取一个指针字段，并用 SEH 防护异常地址访问。

Arguments:

    ProcessObject - 目标 EPROCESS 对象。
    Offset - 目标字段在 EPROCESS 内部的偏移。
    ValueOut - 返回读取到的指针值，统一扩展为 ULONG64。

Return Value:

    读取成功返回 STATUS_SUCCESS；参数错误、偏移缺失或异常读取返回对应状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID pointerValue = NULL;

    if (ProcessObject == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ValueOut = 0ULL;
    if (!KswordARKDriverProcessDynOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory(&pointerValue, (PUCHAR)ProcessObject + Offset, sizeof(pointerValue));
        *ValueOut = (ULONG64)(ULONG_PTR)pointerValue;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

static BOOLEAN
KswordARKDriverShouldSkipTerminatingProcess(
    _In_ PEPROCESS ProcessObject,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    参照 SKT64 的进程遍历逻辑，通过 EPROCESS.ObjectTable 过滤已经退出的进程。

Arguments:

    ProcessObject - 候选进程 EPROCESS。
    DynState - 本次枚举开始时截取的统一 DynData 状态。

Return Value:

    ObjectTable 可读且为 NULL 时返回 TRUE，表示该进程处于 terminating/exited 状态。
    偏移不可用或读取失败时返回 FALSE，避免因为诊断数据缺失误隐藏正常进程。

--*/
{
    ULONG64 objectTableAddress = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || DynState == NULL) {
        return FALSE;
    }
    if (!DynState->Initialized || !KswordARKDriverProcessDynOffsetPresent(DynState->Kernel.EpObjectTable)) {
        return FALSE;
    }

    status = KswordARKDriverReadProcessPointerField(
        ProcessObject,
        DynState->Kernel.EpObjectTable,
        &objectTableAddress);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    return (objectTableAddress == 0ULL) ? TRUE : FALSE;
}

static VOID
KswordARKDriverCopyImageName(
    _Out_writes_all_(16) CHAR destinationImageName[16],
    _In_opt_z_ const CHAR* sourceImageName
    )
{
    ULONG copyIndex = 0;

    if (destinationImageName == NULL) {
        return;
    }

    RtlZeroMemory(destinationImageName, 16);
    if (sourceImageName == NULL) {
        return;
    }

    for (copyIndex = 0; copyIndex < 15U; ++copyIndex) {
        destinationImageName[copyIndex] = sourceImageName[copyIndex];
        if (sourceImageName[copyIndex] == '\0') {
            break;
        }
    }
    destinationImageName[15] = '\0';
}

static VOID
KswordARKDriverAppendProcessEntry(
    _Inout_ KSWORD_ARK_ENUM_PROCESS_RESPONSE* response,
    _In_ size_t entryCapacity,
    _In_ ULONG processId,
    _In_ ULONG parentProcessId,
    _In_ ULONG processFlags,
    _In_opt_z_ const CHAR* imageName,
    _In_ PEPROCESS processObject
    )
{
    KSWORD_ARK_PROCESS_ENTRY* entry = NULL;

    if (response == NULL) {
        return;
    }

    if (response->totalCount != MAXULONG) {
        response->totalCount += 1UL;
    }

    if ((size_t)response->returnedCount >= entryCapacity) {
        return;
    }

    entry = &response->entries[response->returnedCount];
    RtlZeroMemory(entry, sizeof(*entry));
    entry->processId = processId;
    entry->parentProcessId = parentProcessId;
    entry->flags = processFlags;
    KswordARKDriverCopyImageName(entry->imageName, imageName);
    KswordARKProcessPopulateExtendedEntry(entry, processObject);
    response->returnedCount += 1UL;
}

static NTSTATUS
KswordARKDriverResolveSignatureLevelsFromSigner(
    _In_ UCHAR signerType,
    _Out_ UCHAR* signatureLevel,
    _Out_ UCHAR* sectionSignatureLevel
    )
{
    if (signatureLevel == NULL || sectionSignatureLevel == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (signerType) {
    case KSWORD_PS_PROTECTED_SIGNER_NONE:
        *signatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_AUTHENTICODE:
        *signatureLevel = SE_SIGNING_LEVEL_AUTHENTICODE;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_AUTHENTICODE;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_CODEGEN:
        *signatureLevel = SE_SIGNING_LEVEL_DYNAMIC_CODEGEN;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_STORE;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_ANTIMALWARE:
        *signatureLevel = SE_SIGNING_LEVEL_ANTIMALWARE;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_ANTIMALWARE;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_LSA:
        *signatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_MICROSOFT;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_WINDOWS:
        *signatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        return STATUS_SUCCESS;
    case KSWORD_PS_PROTECTED_SIGNER_WINTCB:
        *signatureLevel = SE_SIGNING_LEVEL_WINDOWS_TCB;
        *sectionSignatureLevel = SE_SIGNING_LEVEL_WINDOWS;
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

// PPLcontrol-style fallback: calculate target bytes here, then let Phase-2
// DynData-owned process_extended.c perform the actual EPROCESS patch.
static NTSTATUS
KswordARKDriverPatchProcessProtectionStateByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR signerType = (UCHAR)((protectionLevel & 0xF0U) >> 4U);
    UCHAR signatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
    UCHAR sectionSignatureLevel = SE_SIGNING_LEVEL_UNCHECKED;

    if (protectionLevel == 0U) {
        signerType = KSWORD_PS_PROTECTED_SIGNER_NONE;
    }

    status = KswordARKDriverResolveSignatureLevelsFromSigner(
        signerType,
        &signatureLevel,
        &sectionSignatureLevel);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return KswordARKProcessPatchProtectionByDynData(
        processId,
        protectionLevel,
        signatureLevel,
        sectionSignatureLevel);
}

NTSTATUS
KswordARKDriverSetProcessVisibility(
    _In_ ULONG ProcessId,
    _In_ ULONG Action,
    _Out_ ULONG* StatusOut,
    _Out_ ULONG* HiddenCountOut
    )
/*++

Routine Description:

    更新 Ksword 驱动内维护的进程隐藏标记。中文说明：这不是 DKOM 断链，
    只改变本驱动枚举响应中的 KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI，
    R3 进程列表可据此过滤/恢复，避免不可控内核链表破坏。

Arguments:

    ProcessId - 目标 PID；ClearAll 动作忽略该值。
    Action - HIDE/UNHIDE/CLEAR_ALL。
    StatusOut - 返回可读状态枚举。
    HiddenCountOut - 返回当前隐藏 PID 数量。

Return Value:

    STATUS_SUCCESS 或参数/资源状态。

--*/
{
    ULONG index = 0UL;
    ULONG foundIndex = MAXULONG;
    NTSTATUS status = STATUS_SUCCESS;

    if (StatusOut == NULL || HiddenCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_UNKNOWN;
    *HiddenCountOut = 0UL;
    KswordARKDriverEnsureProcessHideStateInitialized();

    if (Action != KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL &&
        (ProcessId == 0UL || ProcessId <= 4UL)) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquirePushLockExclusive(&g_KswordArkProcessHideState.Lock);
    __try {
        for (index = 0UL; index < g_KswordArkProcessHideState.Count; ++index) {
            if (g_KswordArkProcessHideState.Pids[index] == ProcessId) {
                foundIndex = index;
                break;
            }
        }

        if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL) {
            RtlZeroMemory(
                g_KswordArkProcessHideState.Pids,
                sizeof(g_KswordArkProcessHideState.Pids));
            g_KswordArkProcessHideState.Count = 0UL;
            *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED;
        }
        else if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE) {
            if (foundIndex != MAXULONG) {
                *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
            }
            else if (g_KswordArkProcessHideState.Count >= KSWORD_ARK_PROCESS_HIDE_MAX_PIDS) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else {
                g_KswordArkProcessHideState.Pids[g_KswordArkProcessHideState.Count] = ProcessId;
                g_KswordArkProcessHideState.Count += 1UL;
                *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
            }
        }
        else if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE) {
            if (foundIndex != MAXULONG) {
                for (index = foundIndex + 1UL; index < g_KswordArkProcessHideState.Count; ++index) {
                    g_KswordArkProcessHideState.Pids[index - 1UL] =
                        g_KswordArkProcessHideState.Pids[index];
                }
                if (g_KswordArkProcessHideState.Count > 0UL) {
                    g_KswordArkProcessHideState.Count -= 1UL;
                    g_KswordArkProcessHideState.Pids[g_KswordArkProcessHideState.Count] = 0UL;
                }
            }
            *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_VISIBLE;
        }
        else {
            status = STATUS_INVALID_PARAMETER;
        }

        *HiddenCountOut = g_KswordArkProcessHideState.Count;
    }
    __finally {
        ExReleasePushLockExclusive(&g_KswordArkProcessHideState.Lock);
    }

    return status;
}

NTSTATUS
KswordARKDriverSuspendProcessByPid(
    _In_ ULONG processId
    )
/*++

Routine Description:

    Suspend target process by PID (PsSuspendProcess preferred, Zw/Nt fallback).

Arguments:

    processId - Target process ID.

Return Value:

    NTSTATUS

--*/
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    KSWORD_PS_SUSPEND_PROCESS_FN psSuspendProcess = NULL;
    KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN zwOrNtSuspendProcess = NULL;

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    // Prefer PsSuspendProcess with PEPROCESS input for wider compatibility.
    psSuspendProcess = KswordARKDriverResolvePsSuspendProcess();
    if (psSuspendProcess != NULL) {
        status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        status = psSuspendProcess(processObject);
        ObDereferenceObject(processObject);
        return status;
    }

    // Fallback to Zw/NtSuspendProcess with process-handle input.
    zwOrNtSuspendProcess = KswordARKDriverResolveZwOrNtSuspendProcess();
    if (zwOrNtSuspendProcess == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_SUSPEND_RESUME,
        &objectAttributes,
        &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = zwOrNtSuspendProcess(processHandle);
    ZwClose(processHandle);
    return status;
}

NTSTATUS
KswordARKDriverSetProcessPplLevelByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
/*++

Routine Description:

    Set target process protection state by PID using PPLcontrol-style direct
    EPROCESS patching (Protection + SignatureLevel + SectionSignatureLevel).

Arguments:

    processId - Target process ID.
    protectionLevel - Target protection level byte.

Return Value:

    NTSTATUS

--*/
{
    const UCHAR protectionType = (UCHAR)(protectionLevel & 0x07U);
    const UCHAR signerType = (UCHAR)((protectionLevel & 0xF0U) >> 4U);

    if (processId == 0U || processId <= 4U) {
        return STATUS_INVALID_PARAMETER;
    }

    // PPLcontrol-compatible validation:
    // - 0x00 disables PPL and clears signature levels;
    // - non-zero requires PPL Type==1 and non-zero signer.
    if (protectionLevel == 0U) {
        return KswordARKDriverPatchProcessProtectionStateByPid(processId, 0U);
    }

    if (protectionType != 0x01U || signerType == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    return KswordARKDriverPatchProcessProtectionStateByPid(processId, protectionLevel);
}

NTSTATUS
KswordARKDriverEnumerateProcesses(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_PROCESS_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_PROCESS_RESPONSE* response = NULL;
    size_t entryCapacity = 0;
    size_t totalBytesWritten = 0;
    ULONG requestFlags = 0;
    ULONG scanStartPid = KSWORD_ARK_ENUM_SCAN_MIN_PID;
    ULONG scanEndPid = KSWORD_ARK_ENUM_SCAN_MAX_PID;
    BOOLEAN scanCidTable = FALSE;
    UCHAR* pidBitmap = NULL;
    size_t pidBitmapBytes = 0;
    ULONG scanPid = 0;
    KSWORD_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    PEPROCESS processCursor = NULL;
    KSW_DYN_STATE dynState;

    if (outputBuffer == NULL || bytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *bytesWrittenOut = 0;
    if (outputBufferLength < KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(outputBuffer, outputBufferLength);
    response = (KSWORD_ARK_ENUM_PROCESS_RESPONSE*)outputBuffer;
    response->version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_PROCESS_ENTRY);
    entryCapacity =
        (outputBufferLength - KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_PROCESS_ENTRY);

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    if (request != NULL) {
        requestFlags = request->flags;
        if (request->startPid != 0U) {
            scanStartPid = request->startPid;
        }
        if (request->endPid != 0U) {
            scanEndPid = request->endPid;
        }
    }

    if (scanStartPid < KSWORD_ARK_ENUM_SCAN_MIN_PID) {
        scanStartPid = KSWORD_ARK_ENUM_SCAN_MIN_PID;
    }
    if (scanEndPid < scanStartPid) {
        scanEndPid = scanStartPid;
    }
    if (scanEndPid > KSWORD_ARK_ENUM_SCAN_MAX_PID) {
        scanEndPid = KSWORD_ARK_ENUM_SCAN_MAX_PID;
    }

    scanStartPid = KswordARKDriverAlignPidToStep(scanStartPid);
    if (scanStartPid < KSWORD_ARK_ENUM_SCAN_MIN_PID) {
        scanStartPid = KSWORD_ARK_ENUM_SCAN_MIN_PID;
    }
    scanEndPid = KswordARKDriverAlignPidToStep(scanEndPid);
    if (scanEndPid < scanStartPid) {
        scanEndPid = scanStartPid;
    }

    scanCidTable = ((requestFlags & KSWORD_ARK_ENUM_PROCESS_FLAG_SCAN_CID_TABLE) != 0U) ? TRUE : FALSE;
    psGetNextProcess = KswordARKDriverResolvePsGetNextProcess();
    if (psGetNextProcess == NULL) {
        // Fallback: force CID scan when PsGetNextProcess is unavailable.
        scanCidTable = TRUE;
    }

    if (scanCidTable) {
        const size_t bitmapBitCount = (size_t)(scanEndPid / KSWORD_ARK_ENUM_PID_STEP) + 1U;
        pidBitmapBytes = (bitmapBitCount + 7U) >> 3;
#pragma warning(push)
#pragma warning(disable:4996)
        pidBitmap = (UCHAR*)ExAllocatePoolWithTag(NonPagedPoolNx, pidBitmapBytes, 'pKsK');
#pragma warning(pop)
        if (pidBitmap == NULL) {
            scanCidTable = FALSE;
            pidBitmapBytes = 0U;
        }
        else {
            RtlZeroMemory(pidBitmap, pidBitmapBytes);
        }
    }

    if (psGetNextProcess != NULL) {
        processCursor = psGetNextProcess(NULL);
        while (processCursor != NULL) {
            const ULONG processId = HandleToULong(PsGetProcessId(processCursor));
            const ULONG parentProcessId =
                HandleToULong(PsGetProcessInheritedFromUniqueProcessId(processCursor));
            const CHAR* imageName = PsGetProcessImageFileName(processCursor);
            ULONG processFlags = KSWORD_ARK_PROCESS_FLAG_KERNEL_ENUMERATED;
            PEPROCESS nextProcess = NULL;
            const BOOLEAN skipTerminatingProcess =
                KswordARKDriverShouldSkipTerminatingProcess(processCursor, &dynState);

            if (!skipTerminatingProcess) {
                if (KswordARKDriverIsProcessHiddenByUi(processId)) {
                    processFlags |= KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI;
                }
                KswordARKDriverAppendProcessEntry(
                    response,
                    entryCapacity,
                    processId,
                    parentProcessId,
                    processFlags,
                    imageName,
                    processCursor);
            }

            if (scanCidTable && KswordARKDriverPidInScanRange(processId, scanStartPid, scanEndPid)) {
                KswordARKDriverBitmapSetPid(pidBitmap, pidBitmapBytes, processId);
            }

            nextProcess = psGetNextProcess(processCursor);
            ObDereferenceObject(processCursor);
            processCursor = nextProcess;
        }
    }

    if (scanCidTable) {
        scanPid = scanStartPid;
        for (;;) {
            PEPROCESS hiddenProcessObject = NULL;
            NTSTATUS lookupStatus = STATUS_UNSUCCESSFUL;
            BOOLEAN presentInActiveList = FALSE;

            if (psGetNextProcess != NULL && pidBitmap != NULL) {
                presentInActiveList = KswordARKDriverBitmapHasPid(pidBitmap, pidBitmapBytes, scanPid);
            }

            if (!presentInActiveList) {
                lookupStatus = PsLookupProcessByProcessId(ULongToHandle(scanPid), &hiddenProcessObject);
                if (NT_SUCCESS(lookupStatus)) {
                    const ULONG parentProcessId =
                        HandleToULong(PsGetProcessInheritedFromUniqueProcessId(hiddenProcessObject));
                    const CHAR* imageName = PsGetProcessImageFileName(hiddenProcessObject);
                    ULONG processFlags = KSWORD_ARK_PROCESS_FLAG_KERNEL_ENUMERATED;

                    if (psGetNextProcess != NULL && pidBitmap != NULL) {
                        processFlags |= KSWORD_ARK_PROCESS_FLAG_HIDDEN_FROM_ACTIVE_LIST;
                    }
                    if (KswordARKDriverIsProcessHiddenByUi(scanPid)) {
                        processFlags |= KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI;
                    }

                    if (!KswordARKDriverShouldSkipTerminatingProcess(hiddenProcessObject, &dynState)) {
                        KswordARKDriverAppendProcessEntry(
                            response,
                            entryCapacity,
                            scanPid,
                            parentProcessId,
                            processFlags,
                            imageName,
                            hiddenProcessObject);
                    }
                    ObDereferenceObject(hiddenProcessObject);
                }
            }

            if ((scanEndPid - scanPid) < KSWORD_ARK_ENUM_PID_STEP) {
                break;
            }
            scanPid += KSWORD_ARK_ENUM_PID_STEP;
        }
    }

    if (pidBitmap != NULL) {
        ExFreePoolWithTag(pidBitmap, 'pKsK');
        pidBitmap = NULL;
    }

    totalBytesWritten =
        KSWORD_ARK_ENUM_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_PROCESS_ENTRY));
    *bytesWrittenOut = totalBytesWritten;
    return STATUS_SUCCESS;
}
