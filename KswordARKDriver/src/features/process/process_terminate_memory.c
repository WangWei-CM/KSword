/*++

Module Name:

    process_terminate_memory.c

Abstract:

    Memory-zero fallback stage used by the KswordARK process termination pipeline.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

// 中文说明：本文件只承载终止流程的内存清零兜底阶段。
// 中文说明：入口导出给 process_terminate.c，避免单文件继续超过协作上限。

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
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

#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION (0x0008)
#endif

#ifndef PROCESS_VM_WRITE
#define PROCESS_VM_WRITE (0x0020)
#endif

#define KSWORD_ARK_MEMORY_ZERO_CHUNK_BYTES 0x1000UL
#define KSWORD_ARK_MEMORY_SCAN_LOW_ADDRESS 0x10000ULL

typedef PVOID(NTAPI* KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN)(
    _In_ PEPROCESS Process
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

VOID
KswordARKDriverLogTerminateMessage(
    _In_opt_ WDFDEVICE device,
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR formatText,
    ...
    );

static KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN
KswordARKDriverResolvePsGetProcessSectionBaseAddress(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsGetProcessSectionBaseAddress");
    return (KSWORD_PS_GET_PROCESS_SECTION_BASE_ADDRESS_FN)MmGetSystemRoutineAddress(&routineName);
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

NTSTATUS
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

