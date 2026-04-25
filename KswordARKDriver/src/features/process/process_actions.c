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

NTSYSAPI
PETHREAD
NTAPI
PsGetNextProcessThread(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupThreadByThreadId(
    _In_ HANDLE ThreadId,
    _Outptr_ PETHREAD* Thread
    );

NTSYSAPI
PEPROCESS
NTAPI
PsGetThreadProcess(
    _In_ PETHREAD Thread
    );

NTKERNELAPI
PVOID
NTAPI
PsGetProcessSectionBaseAddress(
    _In_ PEPROCESS Process
    );

NTKERNELAPI
NTSTATUS
ObOpenObjectByPointer(
    _In_ PVOID Object,
    _In_ ULONG HandleAttributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PHANDLE Handle
    );

NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(
    _In_ PEPROCESS FromProcess,
    _In_reads_bytes_(BufferSize) PVOID FromAddress,
    _In_ PEPROCESS ToProcess,
    _Out_writes_bytes_(BufferSize) PVOID ToAddress,
    _In_ SIZE_T BufferSize,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T NumberOfBytesCopied
    );

NTSYSAPI
NTSTATUS
NTAPI
ZwQueryVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _In_opt_ PVOID BaseAddress,
    _In_ ULONG MemoryInformationClass,
    _Out_writes_bytes_(MemoryInformationLength) PVOID MemoryInformation,
    _In_ SIZE_T MemoryInformationLength,
    _Out_opt_ PSIZE_T ReturnLength
    );

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION (0x0400)
#endif

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE (0x0001)
#endif

#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION (0x0008)
#endif

#ifndef PROCESS_VM_WRITE
#define PROCESS_VM_WRITE (0x0020)
#endif

#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME (0x0800)
#endif

#ifndef THREAD_TERMINATE
#define THREAD_TERMINATE (0x0001)
#endif

#ifndef STATUS_PROCESS_IS_TERMINATING
#define STATUS_PROCESS_IS_TERMINATING ((NTSTATUS)0xC000010AL)
#endif

#ifndef STATUS_THREAD_IS_TERMINATING
#define STATUS_THREAD_IS_TERMINATING ((NTSTATUS)0xC000004BL)
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
#define KSWORD_ARK_TERMINATE_WAIT_SLICE_MS 10UL
#define KSWORD_ARK_TERMINATE_WAIT_MAX_MS 500UL
#define KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS 800UL
#define KSWORD_ARK_TERMINATE_SCAN_CALL_MAX_BYTES 0x240UL
#define KSWORD_ARK_MEMORY_ZERO_CHUNK_BYTES 0x1000UL
#define KSWORD_ARK_MEMORY_SCAN_LOW_ADDRESS 0x10000ULL
#define KSWORD_ARK_THREAD_SCAN_MAX_ID 0x00800000UL

typedef PETHREAD(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

typedef PVOID(NTAPI* KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN)(
    _In_ PEPROCESS Process
    );

typedef NTSTATUS(NTAPI* KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)(
    _In_ PETHREAD Thread,
    _In_ NTSTATUS ExitStatus,
    _In_ BOOLEAN SelfTerminate,
    _In_opt_ PVOID Reserved
    );

typedef NTSTATUS(NTAPI* KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN)(
    _In_opt_ HANDLE ThreadHandle,
    _In_ NTSTATUS ExitStatus
    );

typedef struct _KSWORD_ARK_MEMORY_BASIC_INFORMATION
{
    PVOID BaseAddress;
    PVOID AllocationBase;
    ULONG AllocationProtect;
    SIZE_T RegionSize;
    ULONG State;
    ULONG Protect;
    ULONG Type;
} KSWORD_ARK_MEMORY_BASIC_INFORMATION;

typedef PEPROCESS(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

static VOID
KswordARKDriverLogTerminateMessageV(
    _In_opt_ WDFDEVICE device,
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR formatText,
    _In_ va_list arguments
    )
{
    CHAR messageBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };

    if (device == NULL || levelText == NULL || formatText == NULL) {
        return;
    }

    if (NT_SUCCESS(RtlStringCbVPrintfA(
        messageBuffer,
        sizeof(messageBuffer),
        formatText,
        arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(device, levelText, messageBuffer);
    }
}

static VOID
KswordARKDriverLogTerminateMessage(
    _In_opt_ WDFDEVICE device,
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR formatText,
    ...
    )
{
    va_list arguments;

    va_start(arguments, formatText);
    KswordARKDriverLogTerminateMessageV(device, levelText, formatText, arguments);
    va_end(arguments);
}

static BOOLEAN
KswordARKDriverIsResolverMissingStatus(
    _In_ NTSTATUS status
    )
{
    return (status == STATUS_PROCEDURE_NOT_FOUND ||
        status == STATUS_NOT_IMPLEMENTED ||
        status == STATUS_NOT_FOUND) ? TRUE : FALSE;
}

static VOID
KswordARKDriverMergeTerminateFailure(
    _In_ NTSTATUS candidateStatus,
    _Inout_ NTSTATUS* aggregateStatus
    )
{
    if (aggregateStatus == NULL) {
        return;
    }
    if (NT_SUCCESS(candidateStatus)) {
        return;
    }
    if (candidateStatus == STATUS_PROCESS_IS_TERMINATING ||
        candidateStatus == STATUS_THREAD_IS_TERMINATING) {
        return;
    }

    if (NT_SUCCESS(*aggregateStatus) || *aggregateStatus == STATUS_UNSUCCESSFUL) {
        *aggregateStatus = candidateStatus;
        return;
    }

    if (KswordARKDriverIsResolverMissingStatus(*aggregateStatus) &&
        !KswordARKDriverIsResolverMissingStatus(candidateStatus)) {
        *aggregateStatus = candidateStatus;
        return;
    }
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

static KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKDriverResolvePsGetNextProcessThread(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    return (KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN
KswordARKDriverResolvePsGetProcessSectionBaseAddress(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsGetProcessSectionBaseAddress");
    return (KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN
KswordARKDriverResolvePspTerminateThreadByPointer(
    VOID
    )
{
    static KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN cachedRoutine = NULL;
    static BOOLEAN hasResolved = FALSE;
    UNICODE_STRING routineName;
    PUCHAR ntTerminateThreadAddress = NULL;
    ULONG scanOffset = 0UL;

    if (hasResolved) {
        return cachedRoutine;
    }
    hasResolved = TRUE;

    // Try direct lookup first (some private symbols can surface on test kernels).
    RtlInitUnicodeString(&routineName, L"PspTerminateThreadByPointer");
    cachedRoutine = (KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)MmGetSystemRoutineAddress(&routineName);
    if (cachedRoutine != NULL) {
        return cachedRoutine;
    }

    // Dynamic locate fallback: scan NtTerminateThread for near-call target in ntoskrnl text.
    RtlInitUnicodeString(&routineName, L"NtTerminateThread");
    ntTerminateThreadAddress = (PUCHAR)MmGetSystemRoutineAddress(&routineName);
    if (ntTerminateThreadAddress == NULL) {
        return NULL;
    }

    for (scanOffset = 0UL; scanOffset + 5UL < KSWORD_ARK_TERMINATE_SCAN_CALL_MAX_BYTES; ++scanOffset) {
        LONG relativeOffset = 0;
        PUCHAR targetAddress = NULL;
        ULONG_PTR ntAddress = (ULONG_PTR)ntTerminateThreadAddress;
        ULONG_PTR targetValue = 0;

        if (ntTerminateThreadAddress[scanOffset] == 0xC3U) {
            break;
        }
        if (ntTerminateThreadAddress[scanOffset] != 0xE8U) {
            continue;
        }

        RtlCopyMemory(
            &relativeOffset,
            ntTerminateThreadAddress + scanOffset + 1UL,
            sizeof(relativeOffset));

        targetAddress = ntTerminateThreadAddress + scanOffset + 5UL + relativeOffset;
        targetValue = (ULONG_PTR)targetAddress;
        if (targetValue <= ntAddress) {
            continue;
        }
        if ((targetValue - ntAddress) > 0x200000UL) {
            continue;
        }
        if (!MmIsAddressValid(targetAddress)) {
            continue;
        }

        cachedRoutine = (KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN)targetAddress;
        break;
    }

    return cachedRoutine;
}

static KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN
KswordARKDriverResolveZwOrNtTerminateThread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwTerminateThread");
    {
        KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN routineAddress =
            (KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
        if (routineAddress != NULL) {
            return routineAddress;
        }
    }

    RtlInitUnicodeString(&routineName, L"NtTerminateThread");
    return (KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
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
    _In_opt_z_ const CHAR* imageName
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

// PPLcontrol-style fallback: patch EPROCESS protection/signature bytes directly.
static NTSTATUS
KswordARKDriverPatchProcessProtectionStateByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    )
{
    const LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();
    const LONG signatureLevelOffset = KswordARKDriverResolveProcessSignatureLevelOffset();
    const LONG sectionSignatureLevelOffset = KswordARKDriverResolveProcessSectionSignatureLevelOffset();
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR signerType = (UCHAR)((protectionLevel & 0xF0U) >> 4U);
    UCHAR signatureLevel = SE_SIGNING_LEVEL_UNCHECKED;
    UCHAR sectionSignatureLevel = SE_SIGNING_LEVEL_UNCHECKED;

    if (protectionOffset <= 0 ||
        signatureLevelOffset <= 0 ||
        sectionSignatureLevelOffset <= 0) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

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

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        PUCHAR processBase = (PUCHAR)processObject;
        volatile UCHAR* protectionByte = (volatile UCHAR*)(processBase + (ULONG)protectionOffset);
        volatile UCHAR* signatureByte = (volatile UCHAR*)(processBase + (ULONG)signatureLevelOffset);
        volatile UCHAR* sectionSignatureByte =
            (volatile UCHAR*)(processBase + (ULONG)sectionSignatureLevelOffset);

        *protectionByte = protectionLevel;
        *signatureByte = signatureLevel;
        *sectionSignatureByte = sectionSignatureLevel;

        if (*protectionByte != protectionLevel ||
            *signatureByte != signatureLevel ||
            *sectionSignatureByte != sectionSignatureLevel) {
            status = STATUS_UNSUCCESSFUL;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ObDereferenceObject(processObject);
    return status;
}

static NTSTATUS
KswordARKDriverOpenProcessHandleForTerminate(
    _In_ ULONG processId,
    _Out_ HANDLE* processHandleOut
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (processHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *processHandleOut = NULL;

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        PROCESS_TERMINATE,
        &objectAttributes,
        &clientId);
    if (NT_SUCCESS(status)) {
        *processHandleOut = processHandle;
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ObOpenObjectByPointer(
        processObject,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_TERMINATE,
        *PsProcessType,
        KernelMode,
        &processHandle);
    ObDereferenceObject(processObject);

    if (NT_SUCCESS(status)) {
        *processHandleOut = processHandle;
    }
    return status;
}

static BOOLEAN
KswordARKDriverIsProcessPresentByPid(
    _In_ ULONG processId
    )
{
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    ObDereferenceObject(processObject);
    return TRUE;
}

static NTSTATUS
KswordARKDriverWaitProcessExitByPid(
    _In_ ULONG processId,
    _In_ ULONG timeoutMs
    )
{
    ULONG elapsedMs = 0U;
    LARGE_INTEGER delayInterval;

    if (!KswordARKDriverIsProcessPresentByPid(processId)) {
        return STATUS_SUCCESS;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_PROCESS_IS_TERMINATING;
    }

    delayInterval.QuadPart = -((LONGLONG)KSWORD_ARK_TERMINATE_WAIT_SLICE_MS * 10 * 1000);
    while (elapsedMs < timeoutMs) {
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delayInterval);
        if (!KswordARKDriverIsProcessPresentByPid(processId)) {
            return STATUS_SUCCESS;
        }
        elapsedMs += KSWORD_ARK_TERMINATE_WAIT_SLICE_MS;
    }

    return STATUS_PROCESS_IS_TERMINATING;
}

static NTSTATUS
KswordARKDriverOpenProcessHandleForMemoryZero(
    _In_ ULONG processId,
    _Out_ HANDLE* processHandleOut,
    _Out_ PEPROCESS* processObjectOut
    )
{
    OBJECT_ATTRIBUTES objectAttributes;
    CLIENT_ID clientId;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    const ACCESS_MASK desiredAccess =
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE;

    if (processHandleOut == NULL || processObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *processHandleOut = NULL;
    *processObjectOut = NULL;

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    InitializeObjectAttributes(&objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ULongToHandle(processId);
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(
        &processHandle,
        desiredAccess,
        &objectAttributes,
        &clientId);
    if (!NT_SUCCESS(status)) {
        status = ObOpenObjectByPointer(
            processObject,
            OBJ_KERNEL_HANDLE,
            NULL,
            desiredAccess,
            *PsProcessType,
            KernelMode,
            &processHandle);
    }

    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(processObject);
        return status;
    }

    *processHandleOut = processHandle;
    *processObjectOut = processObject;
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKDriverIsWritableProtection(
    _In_ ULONG protectionFlags
    )
{
    const ULONG baseProtect = protectionFlags & 0xFFUL;

    switch (baseProtect) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS
KswordARKDriverTerminateProcessThreadsByPointer(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
{
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_PSP_TERMINATE_THREAD_BY_POINTER_FN pspTerminateThreadByPointer = NULL;
    KSWORD_ZW_OR_NT_TERMINATE_THREAD_FN zwOrNtTerminateThread = NULL;
    PEPROCESS processObject = NULL;
    PETHREAD threadCursor = NULL;
    ULONG threadVisitedCount = 0UL;
    ULONG threadTerminatedCount = 0UL;
    ULONG threadFailureCount = 0UL;
    BOOLEAN useCidThreadScan = FALSE;
    BOOLEAN hasTerminateRoutine = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS lastFailureStatus = STATUS_UNSUCCESSFUL;

    psGetNextProcessThread = KswordARKDriverResolvePsGetNextProcessThread();
    pspTerminateThreadByPointer = KswordARKDriverResolvePspTerminateThreadByPointer();
    if (pspTerminateThreadByPointer == NULL) {
        zwOrNtTerminateThread = KswordARKDriverResolveZwOrNtTerminateThread();
    }
    hasTerminateRoutine =
        (pspTerminateThreadByPointer != NULL || zwOrNtTerminateThread != NULL) ? TRUE : FALSE;

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate fallback#2 resolver: pid=%lu, PsGetNextProcessThread=%p, PspTerminateThreadByPointer=%p, ZwOrNtTerminateThread=%p.",
        (unsigned long)processId,
        psGetNextProcessThread,
        pspTerminateThreadByPointer,
        zwOrNtTerminateThread);

    if (!hasTerminateRoutine) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#2 unavailable: pid=%lu, reason=%s.",
            (unsigned long)processId,
            "thread terminate routine missing");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    useCidThreadScan = (psGetNextProcessThread == NULL) ? TRUE : FALSE;
    if (useCidThreadScan) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#2 switching to CID thread scan: pid=%lu, maxTid=0x%08X.",
            (unsigned long)processId,
            (unsigned int)KSWORD_ARK_THREAD_SCAN_MAX_ID);
    }

    status = PsLookupProcessByProcessId(ULongToHandle(processId), &processObject);
    if (!NT_SUCCESS(status)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#2 lookup failed: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)status);
        return status;
    }

    if (!useCidThreadScan) {
        threadCursor = psGetNextProcessThread(processObject, NULL);
        while (threadCursor != NULL) {
            PETHREAD nextThread = psGetNextProcessThread(processObject, threadCursor);
            NTSTATUS terminateStatus = STATUS_UNSUCCESSFUL;
            threadVisitedCount += 1UL;

            if (pspTerminateThreadByPointer != NULL) {
                terminateStatus = pspTerminateThreadByPointer(
                    threadCursor,
                    exitStatus,
                    FALSE,
                    NULL);
            }
            else {
                HANDLE threadHandle = NULL;
                terminateStatus = ObOpenObjectByPointer(
                    threadCursor,
                    OBJ_KERNEL_HANDLE,
                    NULL,
                    THREAD_TERMINATE,
                    *PsThreadType,
                    KernelMode,
                    &threadHandle);
                if (NT_SUCCESS(terminateStatus)) {
                    if (zwOrNtTerminateThread != NULL) {
                        terminateStatus = zwOrNtTerminateThread(threadHandle, exitStatus);
                    }
                    else {
                        terminateStatus = STATUS_PROCEDURE_NOT_FOUND;
                    }
                    ZwClose(threadHandle);
                }
            }

            if (NT_SUCCESS(terminateStatus) ||
                terminateStatus == STATUS_THREAD_IS_TERMINATING ||
                terminateStatus == STATUS_PROCESS_IS_TERMINATING) {
                threadTerminatedCount += 1UL;
            }
            else {
                threadFailureCount += 1UL;
                lastFailureStatus = terminateStatus;
            }

            ObDereferenceObject(threadCursor);
            threadCursor = nextThread;
        }
    }
    else {
        ULONG scanThreadId = 4UL;
        for (;;) {
            PETHREAD threadObject = NULL;
            NTSTATUS lookupThreadStatus = PsLookupThreadByThreadId(ULongToHandle(scanThreadId), &threadObject);
            if (NT_SUCCESS(lookupThreadStatus)) {
                if (PsGetThreadProcess(threadObject) == processObject) {
                    NTSTATUS terminateStatus = STATUS_UNSUCCESSFUL;
                    threadVisitedCount += 1UL;

                    if (pspTerminateThreadByPointer != NULL) {
                        terminateStatus = pspTerminateThreadByPointer(
                            threadObject,
                            exitStatus,
                            FALSE,
                            NULL);
                    }
                    else {
                        HANDLE threadHandle = NULL;
                        terminateStatus = ObOpenObjectByPointer(
                            threadObject,
                            OBJ_KERNEL_HANDLE,
                            NULL,
                            THREAD_TERMINATE,
                            *PsThreadType,
                            KernelMode,
                            &threadHandle);
                        if (NT_SUCCESS(terminateStatus)) {
                            if (zwOrNtTerminateThread != NULL) {
                                terminateStatus = zwOrNtTerminateThread(threadHandle, exitStatus);
                            }
                            else {
                                terminateStatus = STATUS_PROCEDURE_NOT_FOUND;
                            }
                            ZwClose(threadHandle);
                        }
                    }

                    if (NT_SUCCESS(terminateStatus) ||
                        terminateStatus == STATUS_THREAD_IS_TERMINATING ||
                        terminateStatus == STATUS_PROCESS_IS_TERMINATING) {
                        threadTerminatedCount += 1UL;
                    }
                    else {
                        threadFailureCount += 1UL;
                        lastFailureStatus = terminateStatus;
                    }
                }
                ObDereferenceObject(threadObject);
            }

            if ((KSWORD_ARK_THREAD_SCAN_MAX_ID - scanThreadId) < KSWORD_ARK_ENUM_PID_STEP) {
                break;
            }
            scanThreadId += KSWORD_ARK_ENUM_PID_STEP;
        }
    }

    ObDereferenceObject(processObject);

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate fallback#2 result: pid=%lu, visited=%lu, terminated=%lu, failed=%lu, lastFailure=0x%08X.",
        (unsigned long)processId,
        (unsigned long)threadVisitedCount,
        (unsigned long)threadTerminatedCount,
        (unsigned long)threadFailureCount,
        (unsigned int)lastFailureStatus);

    if (threadTerminatedCount > 0UL) {
        return STATUS_SUCCESS;
    }
    if (lastFailureStatus != STATUS_UNSUCCESSFUL) {
        return lastFailureStatus;
    }
    if (threadVisitedCount == 0UL) {
        return STATUS_NOT_FOUND;
    }
    return lastFailureStatus;
}

static NTSTATUS
KswordARKDriverZeroProcessUserMemoryByPid(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId
    )
{
    KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN psGetSectionBaseAddress = NULL;
    HANDLE processHandle = NULL;
    PEPROCESS processObject = NULL;
    PUCHAR zeroChunkBuffer = NULL;
    ULONG_PTR queryAddress = KSWORD_ARK_MEMORY_SCAN_LOW_ADDRESS;
    ULONG_PTR upperUserAddress = 0;
    ULONG_PTR sectionBase = 0;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS finalStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS firstFailureStatus = STATUS_UNSUCCESSFUL;
    BOOLEAN anyWriteSucceeded = FALSE;
    ULONG scannedRegionCount = 0UL;
    ULONG writableRegionCount = 0UL;
    ULONG successfulWriteCount = 0UL;
    SIZE_T totalBytesZeroed = 0U;

    status = KswordARKDriverOpenProcessHandleForMemoryZero(
        processId,
        &processHandle,
        &processObject);
    if (!NT_SUCCESS(status)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate fallback#3 open failed: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)status);
        return status;
    }

    psGetSectionBaseAddress = KswordARKDriverResolvePsGetProcessSectionBaseAddress();
    if (psGetSectionBaseAddress != NULL) {
        sectionBase = (ULONG_PTR)psGetSectionBaseAddress(processObject);
        if (sectionBase >= KSWORD_ARK_MEMORY_SCAN_LOW_ADDRESS) {
            queryAddress = sectionBase;
        }
    }

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate fallback#3 resolver: pid=%lu, PsGetProcessSectionBaseAddress=%p, sectionBase=0x%p.",
        (unsigned long)processId,
        psGetSectionBaseAddress,
        (PVOID)sectionBase);

    upperUserAddress = (ULONG_PTR)MmUserProbeAddress;
    if (upperUserAddress <= queryAddress) {
        queryAddress = KSWORD_ARK_MEMORY_SCAN_LOW_ADDRESS;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    zeroChunkBuffer = (PUCHAR)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        KSWORD_ARK_MEMORY_ZERO_CHUNK_BYTES,
        'zKsK');
#pragma warning(pop)
    if (zeroChunkBuffer == NULL) {
        finalStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    RtlZeroMemory(zeroChunkBuffer, KSWORD_ARK_MEMORY_ZERO_CHUNK_BYTES);

    while (queryAddress < upperUserAddress) {
        KSWORD_ARK_MEMORY_BASIC_INFORMATION memoryInfo;
        SIZE_T returnedBytes = 0;
        ULONG_PTR regionBase = 0;
        SIZE_T regionSize = 0;
        ULONG_PTR nextAddress = 0;
        scannedRegionCount += 1UL;

        RtlZeroMemory(&memoryInfo, sizeof(memoryInfo));
        status = ZwQueryVirtualMemory(
            processHandle,
            (PVOID)queryAddress,
            0UL,
            &memoryInfo,
            sizeof(memoryInfo),
            &returnedBytes);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_INVALID_PARAMETER || status == STATUS_ACCESS_DENIED) {
                queryAddress += PAGE_SIZE;
                continue;
            }
            if (firstFailureStatus == STATUS_UNSUCCESSFUL) {
                firstFailureStatus = status;
            }
            break;
        }

        regionBase = (ULONG_PTR)memoryInfo.BaseAddress;
        regionSize = (SIZE_T)memoryInfo.RegionSize;
        if (regionSize == 0U) {
            queryAddress += PAGE_SIZE;
            continue;
        }

        nextAddress = regionBase + regionSize;
        if (nextAddress <= queryAddress) {
            break;
        }

        if (memoryInfo.State == MEM_COMMIT &&
            (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0UL &&
            KswordARKDriverIsWritableProtection(memoryInfo.Protect)) {
            ULONG_PTR writeAddress = regionBase;
            const ULONG_PTR regionEndAddress = regionBase + regionSize;
            writableRegionCount += 1UL;

            while (writeAddress < regionEndAddress) {
                SIZE_T bytesToWrite = (SIZE_T)(regionEndAddress - writeAddress);
                SIZE_T bytesCopied = 0;
                NTSTATUS copyStatus = STATUS_UNSUCCESSFUL;

                if (bytesToWrite > KSWORD_ARK_MEMORY_ZERO_CHUNK_BYTES) {
                    bytesToWrite = KSWORD_ARK_MEMORY_ZERO_CHUNK_BYTES;
                }

                copyStatus = MmCopyVirtualMemory(
                    PsGetCurrentProcess(),
                    zeroChunkBuffer,
                    processObject,
                    (PVOID)writeAddress,
                    bytesToWrite,
                    KernelMode,
                    &bytesCopied);
                if (!NT_SUCCESS(copyStatus) || bytesCopied == 0U) {
                    if (firstFailureStatus == STATUS_UNSUCCESSFUL) {
                        firstFailureStatus = copyStatus;
                    }
                    break;
                }

                anyWriteSucceeded = TRUE;
                successfulWriteCount += 1UL;
                totalBytesZeroed += bytesCopied;
                writeAddress += bytesCopied;
            }
        }

        queryAddress = nextAddress;
    }

    if (anyWriteSucceeded) {
        finalStatus = STATUS_SUCCESS;
    }
    else if (firstFailureStatus != STATUS_UNSUCCESSFUL) {
        finalStatus = firstFailureStatus;
    }
    else {
        finalStatus = STATUS_NOT_FOUND;
    }

    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(finalStatus) ? "Info" : "Warn",
        "R0 terminate fallback#3 result: pid=%lu, status=0x%08X, scannedRegions=%lu, writableRegions=%lu, successfulWrites=%lu, bytesZeroed=%Iu.",
        (unsigned long)processId,
        (unsigned int)finalStatus,
        (unsigned long)scannedRegionCount,
        (unsigned long)writableRegionCount,
        (unsigned long)successfulWriteCount,
        totalBytesZeroed);

Exit:
    if (zeroChunkBuffer != NULL) {
        ExFreePoolWithTag(zeroChunkBuffer, 'zKsK');
        zeroChunkBuffer = NULL;
    }
    if (processHandle != NULL) {
        ZwClose(processHandle);
        processHandle = NULL;
    }
    if (processObject != NULL) {
        ObDereferenceObject(processObject);
        processObject = NULL;
    }

    return finalStatus;
}

NTSTATUS
KswordARKDriverTerminateProcessByPid(
    _In_opt_ WDFDEVICE device,
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    )
/*++

Routine Description:

    Open the target process by PID and terminate it via ZwTerminateProcess.

Arguments:

    processId - Target process ID.
    exitStatus - Exit status to report.

Return Value:

    NTSTATUS

--*/
{
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS waitStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS threadTerminateStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS memoryZeroStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS aggregateFailureStatus = STATUS_UNSUCCESSFUL;
    BOOLEAN processStillPresent = FALSE;

    if (processId == 0U || processId <= 4U) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate rejected: pid=%lu.",
            (unsigned long)processId);
        return STATUS_INVALID_PARAMETER;
    }

    KswordARKDriverLogTerminateMessage(
        device,
        "Info",
        "R0 terminate pipeline begin: pid=%lu, exit=0x%08X.",
        (unsigned long)processId,
        (unsigned int)exitStatus);

    status = KswordARKDriverOpenProcessHandleForTerminate(processId, &processHandle);
    if (!NT_SUCCESS(status)) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Warn",
            "R0 terminate open failed: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)status);
        return status;
    }

    status = ZwTerminateProcess(processHandle, exitStatus);
    ZwClose(processHandle);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(status) ? "Info" : "Warn",
        "R0 terminate stage#1 ZwTerminateProcess: pid=%lu, status=0x%08X.",
        (unsigned long)processId,
        (unsigned int)status);
    if (NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }
    KswordARKDriverMergeTerminateFailure(status, &aggregateFailureStatus);

    if (status == STATUS_PROCESS_IS_TERMINATING) {
        waitStatus = KswordARKDriverWaitProcessExitByPid(
            processId,
            KSWORD_ARK_TERMINATE_WAIT_MAX_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#1 wait: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus)) {
            return STATUS_SUCCESS;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = KswordARKDriverIsProcessPresentByPid(processId);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process already gone after stage#1: pid=%lu.",
            (unsigned long)processId);
        return STATUS_SUCCESS;
    }

    // Fallback method #2:
    // dynamic-resolve PspTerminateThreadByPointer and terminate every thread.
    threadTerminateStatus = KswordARKDriverTerminateProcessThreadsByPointer(
        device,
        processId,
        exitStatus);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(threadTerminateStatus) ? "Info" : "Warn",
        "R0 terminate stage#2 thread-sweep: pid=%lu, status=0x%08X.",
        (unsigned long)processId,
        (unsigned int)threadTerminateStatus);
    KswordARKDriverMergeTerminateFailure(threadTerminateStatus, &aggregateFailureStatus);

    if (NT_SUCCESS(threadTerminateStatus)) {
        waitStatus = KswordARKDriverWaitProcessExitByPid(
            processId,
            KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#2 wait: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus)) {
            return STATUS_SUCCESS;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = KswordARKDriverIsProcessPresentByPid(processId);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process gone after stage#2: pid=%lu.",
            (unsigned long)processId);
        return STATUS_SUCCESS;
    }

    // Fallback method #3:
    // zero writable user memory regions to force process unusable.
    memoryZeroStatus = KswordARKDriverZeroProcessUserMemoryByPid(device, processId);
    KswordARKDriverLogTerminateMessage(
        device,
        NT_SUCCESS(memoryZeroStatus) ? "Info" : "Warn",
        "R0 terminate stage#3 memory-zero: pid=%lu, status=0x%08X.",
        (unsigned long)processId,
        (unsigned int)memoryZeroStatus);
    KswordARKDriverMergeTerminateFailure(memoryZeroStatus, &aggregateFailureStatus);

    if (NT_SUCCESS(memoryZeroStatus)) {
        waitStatus = KswordARKDriverWaitProcessExitByPid(
            processId,
            KSWORD_ARK_TERMINATE_WAIT_FALLBACK_MS);
        KswordARKDriverLogTerminateMessage(
            device,
            NT_SUCCESS(waitStatus) ? "Info" : "Warn",
            "R0 terminate stage#3 wait: pid=%lu, status=0x%08X.",
            (unsigned long)processId,
            (unsigned int)waitStatus);
        if (NT_SUCCESS(waitStatus) || !KswordARKDriverIsProcessPresentByPid(processId)) {
            return STATUS_SUCCESS;
        }
        KswordARKDriverMergeTerminateFailure(waitStatus, &aggregateFailureStatus);
    }

    processStillPresent = KswordARKDriverIsProcessPresentByPid(processId);
    if (!processStillPresent) {
        KswordARKDriverLogTerminateMessage(
            device,
            "Info",
            "R0 terminate process gone after stage#3: pid=%lu.",
            (unsigned long)processId);
        return STATUS_SUCCESS;
    }

    if (aggregateFailureStatus == STATUS_UNSUCCESSFUL) {
        aggregateFailureStatus = status;
    }
    KswordARKDriverLogTerminateMessage(
        device,
        "Error",
        "R0 terminate pipeline failed: pid=%lu, final=0x%08X, stage1=0x%08X, stage2=0x%08X, stage3=0x%08X, wait=0x%08X.",
        (unsigned long)processId,
        (unsigned int)aggregateFailureStatus,
        (unsigned int)status,
        (unsigned int)threadTerminateStatus,
        (unsigned int)memoryZeroStatus,
        (unsigned int)waitStatus);
    return aggregateFailureStatus;
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
            const ULONG processFlags = KSWORD_ARK_PROCESS_FLAG_KERNEL_ENUMERATED;
            PEPROCESS nextProcess = NULL;

            KswordARKDriverAppendProcessEntry(
                response,
                entryCapacity,
                processId,
                parentProcessId,
                processFlags,
                imageName);

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

                    KswordARKDriverAppendProcessEntry(
                        response,
                        entryCapacity,
                        scanPid,
                        parentProcessId,
                        processFlags,
                        imageName);
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
