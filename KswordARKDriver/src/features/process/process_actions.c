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
#define KSWORD_ARK_PROCESS_OFFSET_SCAN_LIMIT 0x2000UL
#define KSWORD_ARK_PROCESS_HIDDEN_PID_TAG 0xE0000000UL
#define KSWORD_ARK_PROCESS_HIDDEN_PID_MASK 0x0FFFFFFCUL

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif

typedef PEPROCESS(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

typedef struct _KSWORD_ARK_PROCESS_HIDE_RECORD
{
    ULONG Pid;
    ULONG UniqueProcessIdOffset;
    ULONG ActiveProcessLinksOffset;
    PEPROCESS ProcessObject;
    HANDLE OriginalUniqueProcessId;
    HANDLE HiddenUniqueProcessId;
    LIST_ENTRY* ActiveProcessLinks;
    LIST_ENTRY* PreviousLink;
    LIST_ENTRY* NextLink;
    BOOLEAN UniqueProcessIdPatched;
    BOOLEAN ActiveListUnlinked;
} KSWORD_ARK_PROCESS_HIDE_RECORD;

typedef struct _KSWORD_ARK_PROCESS_HIDE_STATE
{
    EX_PUSH_LOCK Lock;
    BOOLEAN Initialized;
    ULONG Count;
    KSWORD_ARK_PROCESS_HIDE_RECORD Records[KSWORD_ARK_PROCESS_HIDE_MAX_PIDS];
} KSWORD_ARK_PROCESS_HIDE_STATE;

static KSWORD_ARK_PROCESS_HIDE_STATE g_KswordArkProcessHideState;

static VOID
KswordARKDriverEnsureProcessHideStateInitialized(
    VOID
    )
/*++

Routine Description:

    初始化驱动内进程隐藏状态表。中文说明：该表记录由 Ksword 执行的
    ActiveProcessLinks 摘链状态，包括目标 EPROCESS 引用和原前后节点，
    供后续取消隐藏或清空全部隐藏时恢复链表。

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

    查询 PID 是否被 Ksword R0 可恢复隐藏表记录。中文说明：真实隐藏
    通过 ActiveProcessLinks 摘链完成；该 helper 只用于枚举时补充
    KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI，方便 R3 识别和过滤。

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
        if (g_KswordArkProcessHideState.Records[index].Pid == ProcessId) {
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

static BOOLEAN
KswordARKDriverProcessListPointerAligned(
    _In_ const VOID* Pointer
    )
/*++

Routine Description:

    Validate basic LIST_ENTRY pointer alignment before touching neighbor links.
    中文说明：该函数只做廉价格式检查，避免明显错误的 DynData 偏移或
    损坏链表指针导致后续读写落在非对齐地址。

Arguments:

    Pointer - 待检查的内核指针。

Return Value:

    TRUE 表示指针非空且满足指针宽度对齐；FALSE 表示不可安全用于链表读写。

--*/
{
    return (Pointer != NULL &&
        (((ULONG_PTR)Pointer & (sizeof(PVOID) - 1U)) == 0U)) ? TRUE : FALSE;
}

static NTSTATUS
KswordARKDriverReadProcessHandleField(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG Offset,
    _Out_ HANDLE* ValueOut
    )
/*++

Routine Description:

    Safely read one HANDLE-sized EPROCESS field. 中文说明：用于读取
    _EPROCESS.UniqueProcessId，避免直接解引用错误 DynData 偏移。

Arguments:

    ProcessObject - Target EPROCESS.
    Offset - Field offset inside EPROCESS.
    ValueOut - Receives the HANDLE-sized value.

Return Value:

    STATUS_SUCCESS or validation/exception status.

--*/
{
    HANDLE handleValue = NULL;

    if (ProcessObject == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ValueOut = NULL;

    if (!KswordARKDriverProcessDynOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory(&handleValue, (PUCHAR)ProcessObject + Offset, sizeof(handleValue));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    *ValueOut = handleValue;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverWriteProcessHandleField(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG Offset,
    _In_ HANDLE Value
    )
/*++

Routine Description:

    Safely write one HANDLE-sized EPROCESS field. 中文说明：用于 Ksword
    管理的 _EPROCESS.UniqueProcessId 修改和恢复。

Arguments:

    ProcessObject - Target EPROCESS.
    Offset - Field offset inside EPROCESS.
    Value - HANDLE-sized value to write.

Return Value:

    STATUS_SUCCESS or validation/exception status.

--*/
{
    if (ProcessObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKDriverProcessDynOffsetPresent(Offset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    __try {
        RtlCopyMemory((PUCHAR)ProcessObject + Offset, &Value, sizeof(Value));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverValidateActiveProcessLinksForUnlink(
    _In_ LIST_ENTRY* Link,
    _Outptr_ LIST_ENTRY** PreviousOut,
    _Outptr_ LIST_ENTRY** NextOut
    )
/*++

Routine Description:

    Validate the target EPROCESS.ActiveProcessLinks node before DKOM unlink.
    中文说明：隐藏前必须确认目标节点仍在双向链中，且前后节点能回指
    当前节点；否则拒绝摘链，避免在已损坏链表上继续写入。

Arguments:

    Link - 目标进程的 ActiveProcessLinks 字段地址。
    PreviousOut - 返回 Link->Blink。
    NextOut - 返回 Link->Flink。

Return Value:

    STATUS_SUCCESS 表示可摘链；失败表示参数、对齐或邻居一致性检查未通过。

--*/
{
    LIST_ENTRY* next = NULL;
    LIST_ENTRY* previous = NULL;

    if (Link == NULL || PreviousOut == NULL || NextOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *PreviousOut = NULL;
    *NextOut = NULL;

    if (!KswordARKDriverProcessListPointerAligned(Link)) {
        return STATUS_DATATYPE_MISALIGNMENT;
    }

    __try {
        next = Link->Flink;
        previous = Link->Blink;
        if (!KswordARKDriverProcessListPointerAligned(next) ||
            !KswordARKDriverProcessListPointerAligned(previous) ||
            next == Link ||
            previous == Link ||
            next->Blink != Link ||
            previous->Flink != Link) {
            return STATUS_DATA_ERROR;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    *PreviousOut = previous;
    *NextOut = next;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverTryProcessListOffsets(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG ProcessId,
    _In_ ULONG UniqueProcessIdOffset,
    _In_ ULONG ActiveProcessLinksOffset
    )
/*++

Routine Description:

    Validate one candidate pair of _EPROCESS.UniqueProcessId and
    _EPROCESS.ActiveProcessLinks offsets.

Arguments:

    ProcessObject - Target EPROCESS.
    ProcessId - Original target PID.
    UniqueProcessIdOffset - Candidate UniqueProcessId offset.
    ActiveProcessLinksOffset - Candidate ActiveProcessLinks offset.

Return Value:

    STATUS_SUCCESS when the pair matches the target process and points to a
    consistent active-process list node.

--*/
{
    HANDLE uniqueProcessId = NULL;
    LIST_ENTRY* previous = NULL;
    LIST_ENTRY* next = NULL;
    LIST_ENTRY* link = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || ProcessId == 0UL || ProcessId <= 4UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKDriverProcessDynOffsetPresent(UniqueProcessIdOffset) ||
        !KswordARKDriverProcessDynOffsetPresent(ActiveProcessLinksOffset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = KswordARKDriverReadProcessHandleField(
        ProcessObject,
        UniqueProcessIdOffset,
        &uniqueProcessId);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (HandleToULong(uniqueProcessId) != ProcessId) {
        return STATUS_NOT_FOUND;
    }

    link = (LIST_ENTRY*)((PUCHAR)ProcessObject + ActiveProcessLinksOffset);
    return KswordARKDriverValidateActiveProcessLinksForUnlink(link, &previous, &next);
}

static NTSTATUS
KswordARKDriverResolveProcessListOffsets(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG ProcessId,
    _In_opt_ const KSW_DYN_STATE* DynState,
    _Out_ ULONG* UniqueProcessIdOffsetOut,
    _Out_ ULONG* ActiveProcessLinksOffsetOut
    )
/*++

Routine Description:

    Resolve _EPROCESS.UniqueProcessId and ActiveProcessLinks offsets for the
    target process. 中文说明：优先使用 DynData/PDB profile；缺失时利用这两个
    字段在 EPROCESS 中相邻的布局做保守运行时扫描，避免未刷新 Kernel
    DynData 页面时右键隐藏直接返回 STATUS_PROCEDURE_NOT_FOUND。

Arguments:

    ProcessObject - Target EPROCESS.
    ProcessId - Original target PID.
    DynState - Optional current DynData snapshot.
    UniqueProcessIdOffsetOut - Receives UniqueProcessId offset.
    ActiveProcessLinksOffsetOut - Receives ActiveProcessLinks offset.

Return Value:

    STATUS_SUCCESS or the best observed failure status.

--*/
{
    ULONG uniqueOffset = KSW_DYN_OFFSET_UNAVAILABLE;
    ULONG activeOffset = KSW_DYN_OFFSET_UNAVAILABLE;
    ULONG scanOffset = 0UL;
    NTSTATUS status = STATUS_PROCEDURE_NOT_FOUND;
    NTSTATUS lastStatus = STATUS_PROCEDURE_NOT_FOUND;

    if (ProcessObject == NULL ||
        UniqueProcessIdOffsetOut == NULL ||
        ActiveProcessLinksOffsetOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *UniqueProcessIdOffsetOut = KSW_DYN_OFFSET_UNAVAILABLE;
    *ActiveProcessLinksOffsetOut = KSW_DYN_OFFSET_UNAVAILABLE;

    if (DynState != NULL && DynState->Initialized) {
        uniqueOffset = DynState->Kernel.EpUniqueProcessId;
        activeOffset = DynState->Kernel.EpActiveProcessLinks;
    }

    if (KswordARKDriverProcessDynOffsetPresent(activeOffset) &&
        !KswordARKDriverProcessDynOffsetPresent(uniqueOffset) &&
        activeOffset >= sizeof(HANDLE)) {
        uniqueOffset = activeOffset - (ULONG)sizeof(HANDLE);
    }
    if (KswordARKDriverProcessDynOffsetPresent(uniqueOffset) &&
        !KswordARKDriverProcessDynOffsetPresent(activeOffset) &&
        uniqueOffset <= (MAXULONG - (ULONG)sizeof(HANDLE))) {
        activeOffset = uniqueOffset + (ULONG)sizeof(HANDLE);
    }

    if (KswordARKDriverProcessDynOffsetPresent(uniqueOffset) &&
        KswordARKDriverProcessDynOffsetPresent(activeOffset)) {
        status = KswordARKDriverTryProcessListOffsets(
            ProcessObject,
            ProcessId,
            uniqueOffset,
            activeOffset);
        if (NT_SUCCESS(status)) {
            *UniqueProcessIdOffsetOut = uniqueOffset;
            *ActiveProcessLinksOffsetOut = activeOffset;
            return STATUS_SUCCESS;
        }
        lastStatus = status;
    }

    for (scanOffset = 0UL;
         scanOffset + (ULONG)sizeof(HANDLE) + (ULONG)sizeof(LIST_ENTRY) <= KSWORD_ARK_PROCESS_OFFSET_SCAN_LIMIT;
         scanOffset += (ULONG)sizeof(PVOID)) {
        HANDLE candidatePid = NULL;

        status = KswordARKDriverReadProcessHandleField(ProcessObject, scanOffset, &candidatePid);
        if (!NT_SUCCESS(status)) {
            if (status != STATUS_PROCEDURE_NOT_FOUND) {
                lastStatus = status;
            }
            continue;
        }
        if (HandleToULong(candidatePid) != ProcessId) {
            continue;
        }

        uniqueOffset = scanOffset;
        activeOffset = scanOffset + (ULONG)sizeof(HANDLE);
        status = KswordARKDriverTryProcessListOffsets(
            ProcessObject,
            ProcessId,
            uniqueOffset,
            activeOffset);
        if (NT_SUCCESS(status)) {
            *UniqueProcessIdOffsetOut = uniqueOffset;
            *ActiveProcessLinksOffsetOut = activeOffset;
            return STATUS_SUCCESS;
        }
        lastStatus = status;
    }

    return (lastStatus == STATUS_NOT_FOUND) ? STATUS_PROCEDURE_NOT_FOUND : lastStatus;
}

static HANDLE
KswordARKDriverBuildHiddenUniqueProcessId(
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    Build a deterministic fake PID for the UniqueProcessId field. 中文说明：高位
    打标并保留低位 PID 信息，避免与正常低范围 PID 冲突，同时便于调试。

Arguments:

    ProcessId - Original PID.

Return Value:

    HANDLE-sized fake PID value.

--*/
{
    ULONG hiddenPid = KSWORD_ARK_PROCESS_HIDDEN_PID_TAG |
        (ProcessId & KSWORD_ARK_PROCESS_HIDDEN_PID_MASK);

    if (hiddenPid == ProcessId || hiddenPid <= 4UL) {
        hiddenPid = KSWORD_ARK_PROCESS_HIDDEN_PID_TAG;
    }
    return ULongToHandle(hiddenPid);
}

static NTSTATUS
KswordARKDriverPatchUniqueProcessIdForHide(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG ProcessId,
    _In_ ULONG UniqueProcessIdOffset,
    _Out_ HANDLE* OriginalUniqueProcessIdOut,
    _Out_ HANDLE* HiddenUniqueProcessIdOut
    )
/*++

Routine Description:

    Patch _EPROCESS.UniqueProcessId for Ksword-managed hide.

Arguments:

    ProcessObject - Target EPROCESS.
    ProcessId - Original PID.
    UniqueProcessIdOffset - Offset resolved from DynData or runtime scan.
    OriginalUniqueProcessIdOut - Receives original HANDLE value.
    HiddenUniqueProcessIdOut - Receives written fake HANDLE value.

Return Value:

    STATUS_SUCCESS when the PID field was modified.

--*/
{
    HANDLE originalUniqueProcessId = NULL;
    HANDLE hiddenUniqueProcessId = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OriginalUniqueProcessIdOut == NULL || HiddenUniqueProcessIdOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *OriginalUniqueProcessIdOut = NULL;
    *HiddenUniqueProcessIdOut = NULL;

    status = KswordARKDriverReadProcessHandleField(
        ProcessObject,
        UniqueProcessIdOffset,
        &originalUniqueProcessId);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (HandleToULong(originalUniqueProcessId) != ProcessId) {
        return STATUS_DATA_ERROR;
    }

    hiddenUniqueProcessId = KswordARKDriverBuildHiddenUniqueProcessId(ProcessId);
    status = KswordARKDriverWriteProcessHandleField(
        ProcessObject,
        UniqueProcessIdOffset,
        hiddenUniqueProcessId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *OriginalUniqueProcessIdOut = originalUniqueProcessId;
    *HiddenUniqueProcessIdOut = hiddenUniqueProcessId;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverUnlinkActiveProcessLinks(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG ActiveProcessLinksOffset,
    _Outptr_ LIST_ENTRY** LinkOut,
    _Outptr_ LIST_ENTRY** PreviousOut,
    _Outptr_ LIST_ENTRY** NextOut
    )
/*++

Routine Description:

    Remove one process from the kernel ActiveProcessLinks list. 中文说明：本函数
    只摘 ActiveProcessLinks，不删除 PspCidTable，也不关闭句柄表；因此
    普通 NtQuerySystemInformation 视图不可见，但 Ksword 仍可通过
    PsLookupProcessByProcessId/CID 扫描重新拿到 EPROCESS。

Arguments:

    ProcessObject - 已引用的目标 EPROCESS。
    ActiveProcessLinksOffset - DynData 给出的 EPROCESS.ActiveProcessLinks 偏移。
    LinkOut - 返回目标 LIST_ENTRY 地址。
    PreviousOut - 返回摘链前前驱节点。
    NextOut - 返回摘链前后继节点。

Return Value:

    STATUS_SUCCESS 表示摘链完成；失败表示偏移缺失、链表不一致或异常访问。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    LIST_ENTRY* link = NULL;
    LIST_ENTRY* previous = NULL;
    LIST_ENTRY* next = NULL;

    if (ProcessObject == NULL || LinkOut == NULL || PreviousOut == NULL || NextOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *LinkOut = NULL;
    *PreviousOut = NULL;
    *NextOut = NULL;

    if (!KswordARKDriverProcessDynOffsetPresent(ActiveProcessLinksOffset)) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    link = (LIST_ENTRY*)((PUCHAR)ProcessObject + ActiveProcessLinksOffset);
    status = KswordARKDriverValidateActiveProcessLinksForUnlink(link, &previous, &next);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        previous->Flink = next;
        next->Blink = previous;
        link->Flink = link;
        link->Blink = link;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    *LinkOut = link;
    *PreviousOut = previous;
    *NextOut = next;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverRestoreUniqueProcessIdRecord(
    _Inout_ KSWORD_ARK_PROCESS_HIDE_RECORD* Record
    )
/*++

Routine Description:

    Restore _EPROCESS.UniqueProcessId for one Ksword hide record.

Arguments:

    Record - Hide record with original/fake PID values.

Return Value:

    STATUS_SUCCESS when restored or no PID patch is pending.

--*/
{
    HANDLE currentUniqueProcessId = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Record == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Record->UniqueProcessIdPatched) {
        return STATUS_SUCCESS;
    }
    if (Record->ProcessObject == NULL ||
        !KswordARKDriverProcessDynOffsetPresent(Record->UniqueProcessIdOffset)) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKDriverReadProcessHandleField(
        Record->ProcessObject,
        Record->UniqueProcessIdOffset,
        &currentUniqueProcessId);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (currentUniqueProcessId == Record->OriginalUniqueProcessId) {
        Record->UniqueProcessIdPatched = FALSE;
        return STATUS_SUCCESS;
    }
    if (currentUniqueProcessId != Record->HiddenUniqueProcessId) {
        return STATUS_DATA_ERROR;
    }

    status = KswordARKDriverWriteProcessHandleField(
        Record->ProcessObject,
        Record->UniqueProcessIdOffset,
        Record->OriginalUniqueProcessId);
    if (NT_SUCCESS(status)) {
        Record->UniqueProcessIdPatched = FALSE;
    }
    return status;
}

static NTSTATUS
KswordARKDriverRestoreActiveProcessLinksRecord(
    _Inout_ KSWORD_ARK_PROCESS_HIDE_RECORD* Record
    )
/*++

Routine Description:

    Restore one Ksword-hidden process back into ActiveProcessLinks. 中文说明：
    恢复时要求目标节点仍保持 self-loop。若隐藏时保存的前后节点仍然
    相邻，则放回原位置；若链表期间发生正常插入导致原邻居不再相邻，
    则退化为插到 PsInitialSystemProcess 后面，避免切断当前活动链。

Arguments:

    Record - 隐藏记录，包含目标 EPROCESS、目标链表节点和保存的前后节点。

Return Value:

    STATUS_SUCCESS 表示恢复完成或记录本来未摘链；失败表示链表状态不适合恢复。

--*/
{
    LIST_ENTRY* link = NULL;
    LIST_ENTRY* previous = NULL;
    LIST_ENTRY* next = NULL;
    LIST_ENTRY* insertAfter = NULL;
    LIST_ENTRY* insertBefore = NULL;
    LIST_ENTRY* systemHead = NULL;

    if (Record == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Record->ActiveListUnlinked) {
        return STATUS_SUCCESS;
    }

    link = Record->ActiveProcessLinks;
    previous = Record->PreviousLink;
    next = Record->NextLink;

    if (!KswordARKDriverProcessListPointerAligned(link) ||
        !KswordARKDriverProcessListPointerAligned(previous) ||
        !KswordARKDriverProcessListPointerAligned(next) ||
        !KswordARKDriverProcessDynOffsetPresent(Record->ActiveProcessLinksOffset)) {
        return STATUS_DATATYPE_MISALIGNMENT;
    }

    __try {
        if (link->Flink != link || link->Blink != link) {
            return STATUS_DATA_ERROR;
        }

        /*
         * Prefer exact original placement when the saved neighbors are still
         * adjacent.  If they are not, insert immediately after the System
         * process list head.  This fallback preserves all current nodes instead
         * of overwriting previous->Flink/next->Blink across a changed chain.
         */
        if (previous->Flink == next && next->Blink == previous) {
            insertAfter = previous;
            insertBefore = next;
        }
        else {
            if (PsInitialSystemProcess == NULL) {
                return STATUS_PROCEDURE_NOT_FOUND;
            }
            systemHead = (LIST_ENTRY*)((PUCHAR)PsInitialSystemProcess + Record->ActiveProcessLinksOffset);
            if (!KswordARKDriverProcessListPointerAligned(systemHead) ||
                !KswordARKDriverProcessListPointerAligned(systemHead->Flink) ||
                systemHead->Flink->Blink != systemHead) {
                return STATUS_DATA_ERROR;
            }
            insertAfter = systemHead;
            insertBefore = systemHead->Flink;
        }

        link->Blink = insertAfter;
        link->Flink = insertBefore;
        insertAfter->Flink = link;
        insertBefore->Blink = link;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    Record->ActiveListUnlinked = FALSE;
    return STATUS_SUCCESS;
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

    Execute Ksword-owned process visibility changes. 中文说明：HIDE 会先修改
    目标 _EPROCESS.UniqueProcessId，再把 ActiveProcessLinks 从内核活动进程
    链表摘下，保留 PspCidTable；因此普通 R3 枚举不可见，而 Ksword 的 R0
    CID 扫描仍能按原 PID 读取目标并打上
    KSWORD_ARK_PROCESS_FLAG_HIDDEN_BY_KSWORD_UI。UNHIDE/CLEAR_ALL 只恢复由本
    驱动保存的 PID 和链表记录，不接受 R3 传入任何内核地址。

Arguments:

    ProcessId - 目标 PID；ClearAll 动作忽略该值。
    Action - HIDE/UNHIDE/CLEAR_ALL。
    StatusOut - 返回可读状态枚举。
    HiddenCountOut - 返回当前隐藏 PID 数量。

Return Value:

    STATUS_SUCCESS 表示动作完成；失败表示参数、查找、DynData 或链表状态异常。

--*/
{
    ULONG index = 0UL;
    ULONG foundIndex = MAXULONG;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS restoreStatus = STATUS_SUCCESS;
    PEPROCESS processObject = NULL;
    KSW_DYN_STATE dynState;
    ULONG uniqueProcessIdOffset = KSW_DYN_OFFSET_UNAVAILABLE;
    ULONG activeProcessLinksOffset = KSW_DYN_OFFSET_UNAVAILABLE;

    if (StatusOut == NULL || HiddenCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_UNKNOWN;
    *HiddenCountOut = 0UL;
    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDriverEnsureProcessHideStateInitialized();

    if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE &&
        (ProcessId == 0UL || ProcessId <= 4UL)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE &&
        (ProcessId == 0UL || ProcessId <= 4UL)) {
        *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_VISIBLE;
        ExAcquirePushLockShared(&g_KswordArkProcessHideState.Lock);
        *HiddenCountOut = g_KswordArkProcessHideState.Count;
        ExReleasePushLockShared(&g_KswordArkProcessHideState.Lock);
        return STATUS_SUCCESS;
    }
    if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE) {
        BOOLEAN alreadyHidden = FALSE;
        ExAcquirePushLockShared(&g_KswordArkProcessHideState.Lock);
        for (index = 0UL; index < g_KswordArkProcessHideState.Count; ++index) {
            if (g_KswordArkProcessHideState.Records[index].Pid == ProcessId) {
                alreadyHidden = TRUE;
                break;
            }
        }
        *HiddenCountOut = g_KswordArkProcessHideState.Count;
        ExReleasePushLockShared(&g_KswordArkProcessHideState.Lock);
        if (alreadyHidden) {
            *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
            return STATUS_SUCCESS;
        }

        KswordARKDynDataSnapshot(&dynState);
        status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        status = KswordARKDriverResolveProcessListOffsets(
            processObject,
            ProcessId,
            &dynState,
            &uniqueProcessIdOffset,
            &activeProcessLinksOffset);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(processObject);
            processObject = NULL;
            return status;
        }
    }

    ExAcquirePushLockExclusive(&g_KswordArkProcessHideState.Lock);
    __try {
        for (index = 0UL; index < g_KswordArkProcessHideState.Count; ++index) {
            if (g_KswordArkProcessHideState.Records[index].Pid == ProcessId) {
                foundIndex = index;
                break;
            }
        }

        if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_CLEAR_ALL) {
            index = 0UL;
            while (index < g_KswordArkProcessHideState.Count) {
                KSWORD_ARK_PROCESS_HIDE_RECORD* record =
                    &g_KswordArkProcessHideState.Records[index];
                restoreStatus = KswordARKDriverRestoreUniqueProcessIdRecord(record);
                if (NT_SUCCESS(restoreStatus)) {
                    restoreStatus = KswordARKDriverRestoreActiveProcessLinksRecord(record);
                }
                if (!NT_SUCCESS(restoreStatus)) {
                    if (NT_SUCCESS(status)) {
                        status = restoreStatus;
                    }
                    ++index;
                    continue;
                }

                if (record->ProcessObject != NULL) {
                    ObDereferenceObject(record->ProcessObject);
                    record->ProcessObject = NULL;
                }
                for (foundIndex = index + 1UL;
                     foundIndex < g_KswordArkProcessHideState.Count;
                     ++foundIndex) {
                    g_KswordArkProcessHideState.Records[foundIndex - 1UL] =
                        g_KswordArkProcessHideState.Records[foundIndex];
                }
                g_KswordArkProcessHideState.Count -= 1UL;
                RtlZeroMemory(
                    &g_KswordArkProcessHideState.Records[g_KswordArkProcessHideState.Count],
                    sizeof(g_KswordArkProcessHideState.Records[g_KswordArkProcessHideState.Count]));
            }
            *StatusOut = (g_KswordArkProcessHideState.Count == 0UL)
                ? KSWORD_ARK_PROCESS_VISIBILITY_STATUS_CLEARED
                : KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
        }
        else if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_HIDE) {
            if (foundIndex != MAXULONG) {
                *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
            }
            else if (g_KswordArkProcessHideState.Count >= KSWORD_ARK_PROCESS_HIDE_MAX_PIDS) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else {
                KSWORD_ARK_PROCESS_HIDE_RECORD* record =
                    &g_KswordArkProcessHideState.Records[g_KswordArkProcessHideState.Count];
                LIST_ENTRY* link = NULL;
                LIST_ENTRY* previous = NULL;
                LIST_ENTRY* next = NULL;
                HANDLE originalUniqueProcessId = NULL;
                HANDLE hiddenUniqueProcessId = NULL;

                status = KswordARKDriverPatchUniqueProcessIdForHide(
                    processObject,
                    ProcessId,
                    uniqueProcessIdOffset,
                    &originalUniqueProcessId,
                    &hiddenUniqueProcessId);
                if (!NT_SUCCESS(status)) {
                    *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_UNKNOWN;
                }
                if (NT_SUCCESS(status)) {
                    status = KswordARKDriverUnlinkActiveProcessLinks(
                        processObject,
                        activeProcessLinksOffset,
                        &link,
                        &previous,
                        &next);
                    if (!NT_SUCCESS(status)) {
                        KSWORD_ARK_PROCESS_HIDE_RECORD rollbackRecord;
                        NTSTATUS rollbackStatus = STATUS_SUCCESS;
                        RtlZeroMemory(&rollbackRecord, sizeof(rollbackRecord));
                        rollbackRecord.ProcessObject = processObject;
                        rollbackRecord.Pid = ProcessId;
                        rollbackRecord.UniqueProcessIdOffset = uniqueProcessIdOffset;
                        rollbackRecord.OriginalUniqueProcessId = originalUniqueProcessId;
                        rollbackRecord.HiddenUniqueProcessId = hiddenUniqueProcessId;
                        rollbackRecord.UniqueProcessIdPatched = TRUE;
                        rollbackStatus = KswordARKDriverRestoreUniqueProcessIdRecord(&rollbackRecord);
                        if (!NT_SUCCESS(rollbackStatus)) {
                            RtlZeroMemory(record, sizeof(*record));
                            record->Pid = ProcessId;
                            record->UniqueProcessIdOffset = uniqueProcessIdOffset;
                            record->ActiveProcessLinksOffset = activeProcessLinksOffset;
                            record->ProcessObject = processObject;
                            record->OriginalUniqueProcessId = originalUniqueProcessId;
                            record->HiddenUniqueProcessId = hiddenUniqueProcessId;
                            record->UniqueProcessIdPatched = TRUE;
                            record->ActiveListUnlinked = FALSE;
                            processObject = NULL;
                            g_KswordArkProcessHideState.Count += 1UL;
                            *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
                            status = rollbackStatus;
                        }
                    }
                }
                if (NT_SUCCESS(status)) {
                    RtlZeroMemory(record, sizeof(*record));
                    record->Pid = ProcessId;
                    record->UniqueProcessIdOffset = uniqueProcessIdOffset;
                    record->ActiveProcessLinksOffset = activeProcessLinksOffset;
                    record->ProcessObject = processObject;
                    record->OriginalUniqueProcessId = originalUniqueProcessId;
                    record->HiddenUniqueProcessId = hiddenUniqueProcessId;
                    record->ActiveProcessLinks = link;
                    record->PreviousLink = previous;
                    record->NextLink = next;
                    record->UniqueProcessIdPatched = TRUE;
                    record->ActiveListUnlinked = TRUE;
                    processObject = NULL;
                    g_KswordArkProcessHideState.Count += 1UL;
                    *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
                }
            }
        }
        else if (Action == KSWORD_ARK_PROCESS_VISIBILITY_ACTION_UNHIDE) {
            if (foundIndex != MAXULONG) {
                KSWORD_ARK_PROCESS_HIDE_RECORD* record =
                    &g_KswordArkProcessHideState.Records[foundIndex];
                status = KswordARKDriverRestoreUniqueProcessIdRecord(record);
                if (NT_SUCCESS(status)) {
                    status = KswordARKDriverRestoreActiveProcessLinksRecord(record);
                }
                if (!NT_SUCCESS(status)) {
                    *StatusOut = KSWORD_ARK_PROCESS_VISIBILITY_STATUS_HIDDEN;
                    *HiddenCountOut = g_KswordArkProcessHideState.Count;
                    __leave;
                }

                if (record->ProcessObject != NULL) {
                    ObDereferenceObject(record->ProcessObject);
                    record->ProcessObject = NULL;
                }
                for (index = foundIndex + 1UL; index < g_KswordArkProcessHideState.Count; ++index) {
                    g_KswordArkProcessHideState.Records[index - 1UL] =
                        g_KswordArkProcessHideState.Records[index];
                }
                if (g_KswordArkProcessHideState.Count > 0UL) {
                    g_KswordArkProcessHideState.Count -= 1UL;
                    RtlZeroMemory(
                        &g_KswordArkProcessHideState.Records[g_KswordArkProcessHideState.Count],
                        sizeof(g_KswordArkProcessHideState.Records[g_KswordArkProcessHideState.Count]));
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

    if (processObject != NULL) {
        ObDereferenceObject(processObject);
        processObject = NULL;
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
