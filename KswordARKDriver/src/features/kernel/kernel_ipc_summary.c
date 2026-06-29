/*++
Module Name:
    kernel_ipc_summary.c
Abstract:
    Read-only IPC summary implementation for ALPC, Named Pipe, and Mailslot handles.
Environment:
    Kernel-mode Driver Framework
--*/
#include "kernel_ipc_summary.h"
#include "ark/ark_alpc.h"
#include "ark/ark_dyndata.h"
#include <ntstrsafe.h>
#define KSW_KERNEL_IPC_NAME_CHARS 160UL
#define KSW_KERNEL_IPC_POOL_TAG   'iKsK'
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
typedef PVOID
(NTAPI* KSW_KERNEL_IPC_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );
NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );
NTKERNELAPI
VOID
KeStackAttachProcess(
    _Inout_ PVOID Process,
    _Out_ PVOID ApcState
    );
NTKERNELAPI
VOID
KeUnstackDetachProcess(
    _In_ PVOID ApcState
    );
NTKERNELAPI
NTSTATUS
ObReferenceObjectByHandle(
    _In_ HANDLE Handle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PVOID* Object,
    _Out_opt_ POBJECT_HANDLE_INFORMATION HandleInformation
    );
NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );
static PVOID
KswordARKKernelIpcAllocateNonPaged(
    _In_ SIZE_T BufferBytes
    )
/*++
Routine Description:
    Allocate transient nonpaged memory for IPC object-name queries.
Arguments:
    BufferBytes - Number of bytes requested.
Return Value:
    Allocation pointer or NULL. Caller frees with ExFreePoolWithTag.
--*/
{
    UNICODE_STRING routineName;
    static KSW_KERNEL_IPC_EX_ALLOCATE_POOL2_FN allocatePool2 = NULL;
    if (BufferBytes == 0U) {
        return NULL;
    }
    if (allocatePool2 == NULL) {
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        allocatePool2 = (KSW_KERNEL_IPC_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }
    if (allocatePool2 != NULL) {
        return allocatePool2(POOL_FLAG_NON_PAGED, BufferBytes, KSW_KERNEL_IPC_POOL_TAG);
    }
#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSW_KERNEL_IPC_POOL_TAG);
#pragma warning(pop)
}
static VOID
KswordARKKernelIpcCopyUnicodeStringToFixed(
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _In_opt_ PCUNICODE_STRING Source
    )
/*++
Routine Description:
    Copy a counted UNICODE_STRING into a fixed NUL-terminated WCHAR buffer.
Arguments:
    Destination - Fixed output buffer.
    DestinationChars - Destination capacity in WCHARs.
    Source - Optional counted source string.
Return Value:
    None. Destination is always NUL-terminated when capacity is nonzero.
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
static HANDLE
KswordARKKernelIpcMakeKernelHandle(
    _In_ HANDLE HandleValue
    )
/*++
Routine Description:
    Convert a System-process handle value to the kernel-handle namespace.
Arguments:
    HandleValue - Raw handle value from the IPC summary request.
Return Value:
    Handle value with the architecture-specific kernel-handle bit set.
--*/
{
#ifdef _X86_
    return (HANDLE)((ULONG_PTR)HandleValue | (ULONG_PTR)0x80000000UL);
#else
    return (HANDLE)((ULONG_PTR)HandleValue | (ULONG_PTR)0xFFFFFFFF80000000ULL);
#endif
}
static NTSTATUS
KswordARKKernelIpcReferenceHandleObject(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG64 HandleValue,
    _Outptr_result_nullonfailure_ PVOID* ObjectOut
    )
/*++
Routine Description:
    Reference the object identified by a PID-owned handle.
Arguments:
    ProcessObject - Referenced process object from PsLookupProcessByProcessId.
    HandleValue - Handle value in that process.
    ObjectOut - Receives referenced object body on success.
Return Value:
    STATUS_SUCCESS or ObReferenceObjectByHandle failure status.
--*/
{
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    HANDLE targetHandle = (HANDLE)(ULONG_PTR)HandleValue;
    BOOLEAN attached = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    if (ObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ObjectOut = NULL;
    if (ProcessObject == NULL || HandleValue == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ProcessObject == PsInitialSystemProcess) {
        targetHandle = KswordARKKernelIpcMakeKernelHandle(targetHandle);
    }
    RtlZeroMemory(attachState, sizeof(attachState));
    __try {
        KeStackAttachProcess((PVOID)ProcessObject, attachState);
        attached = TRUE;
        status = ObReferenceObjectByHandle(
            targetHandle,
            0,
            NULL,
            (ProcessObject == PsInitialSystemProcess) ? KernelMode : UserMode,
            ObjectOut,
            NULL);
        KeUnstackDetachProcess(attachState);
        attached = FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        if (attached) {
            KeUnstackDetachProcess(attachState);
            attached = FALSE;
        }
    }
    return status;
}
static NTSTATUS
KswordARKKernelIpcQueryObjectName(
    _In_ PVOID Object,
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars
    )
/*++
Routine Description:
    Query a referenced object's object-manager name into a fixed buffer.
Arguments:
    Object - Referenced object body.
    Destination - Fixed WCHAR output buffer.
    DestinationChars - Destination capacity in WCHARs.
Return Value:
    STATUS_SUCCESS when ObQueryNameString completed, otherwise NTSTATUS.
--*/
{
    POBJECT_NAME_INFORMATION nameInfo = NULL;
    ULONG requiredBytes = 0UL;
    ULONG allocationBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    if (Object == NULL || Destination == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    Destination[0] = L'\0';
    status = ObQueryNameString(Object, NULL, 0UL, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH &&
        status != STATUS_BUFFER_TOO_SMALL &&
        status != STATUS_BUFFER_OVERFLOW) {
        return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
    }
    allocationBytes = requiredBytes;
    if (allocationBytes < sizeof(OBJECT_NAME_INFORMATION) + sizeof(WCHAR)) {
        allocationBytes = sizeof(OBJECT_NAME_INFORMATION) + sizeof(WCHAR);
    }
    if (allocationBytes > (64UL * 1024UL)) {
        allocationBytes = 64UL * 1024UL;
    }
    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKKernelIpcAllocateNonPaged(allocationBytes);
    if (nameInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(nameInfo, allocationBytes);
    status = ObQueryNameString(Object, nameInfo, allocationBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        KswordARKKernelIpcCopyUnicodeStringToFixed(Destination, DestinationChars, &nameInfo->Name);
    }
    ExFreePoolWithTag(nameInfo, KSW_KERNEL_IPC_POOL_TAG);
    return status;
}
static BOOLEAN
KswordARKKernelIpcNameStartsWith(
    _In_reads_(ObjectNameChars) const WCHAR* ObjectName,
    _In_ ULONG ObjectNameChars,
    _In_z_ PCWSTR PrefixText
    )
/*++
Routine Description:
    Check whether an object name starts with a known IPC device prefix.
Arguments:
    ObjectName - Fixed object-manager name buffer.
    ObjectNameChars - Capacity of ObjectName in WCHARs.
    PrefixText - NUL-terminated prefix such as \Device\NamedPipe.
Return Value:
    TRUE when ObjectName begins with PrefixText, case-insensitively.
--*/
{
    UNICODE_STRING actualName;
    UNICODE_STRING prefixName;
    SIZE_T actualChars = 0U;
    SIZE_T prefixChars = 0U;
    if (ObjectName == NULL || ObjectNameChars == 0UL || PrefixText == NULL) {
        return FALSE;
    }
    while (actualChars < (SIZE_T)ObjectNameChars && ObjectName[actualChars] != L'\0') {
        actualChars += 1U;
    }
    if (actualChars == 0U) {
        return FALSE;
    }
    RtlInitUnicodeString(&prefixName, PrefixText);
    prefixChars = (SIZE_T)(prefixName.Length / sizeof(WCHAR));
    if (actualChars < prefixChars) {
        return FALSE;
    }
    actualName.Buffer = (PWCHAR)ObjectName;
    actualName.Length = prefixName.Length;
    actualName.MaximumLength = prefixName.Length;
    return RtlEqualUnicodeString(&actualName, &prefixName, TRUE) ? TRUE : FALSE;
}
static VOID
KswordARKKernelIpcClassifyObjectName(
    _In_reads_(ObjectNameChars) const WCHAR* ObjectName,
    _In_ ULONG ObjectNameChars,
    _In_ ULONG Flags,
    _Inout_ KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE* Response
    )
/*++
Routine Description:
    Classify the referenced handle name as NamedPipe or Mailslot evidence.
Arguments:
    ObjectName - Object-manager name queried from the referenced object.
    ObjectNameChars - Capacity of ObjectName in WCHARs.
    Flags - Sanitized IPC selector flags.
    Response - Mutable IPC summary response.
Return Value:
    None. Status fields are updated in Response.
--*/
{
    if (Response == NULL) {
        return;
    }
    if ((Flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_PIPE) != 0UL) {
        Response->namedPipeStatus = KswordARKKernelIpcNameStartsWith(
            ObjectName,
            ObjectNameChars,
            L"\\Device\\NamedPipe") ?
            KSWORD_ARK_IPC_SUMMARY_STATUS_OK :
            KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    }
    if ((Flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_MAILSLOT) != 0UL) {
        Response->mailslotStatus = KswordARKKernelIpcNameStartsWith(
            ObjectName,
            ObjectNameChars,
            L"\\Device\\Mailslot") ?
            KSWORD_ARK_IPC_SUMMARY_STATUS_OK :
            KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    }
}
static VOID
KswordARKKernelIpcApplyNameFailure(
    _In_ ULONG Flags,
    _Inout_ KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE* Response
    )
/*++
Routine Description:
    Mark handle-backed pipe/mailslot checks as failed after lookup/name failure.
Arguments:
    Flags - Sanitized IPC selector flags.
    Response - Mutable IPC summary response.
Return Value:
    None.
--*/
{
    if (Response == NULL) {
        return;
    }
    if ((Flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_PIPE) != 0UL) {
        Response->namedPipeStatus = KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED;
    }
    if ((Flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_MAILSLOT) != 0UL) {
        Response->mailslotStatus = KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED;
    }
}
static VOID
KswordARKKernelIpcFinalizeStatus(
    _Inout_ KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE* Response
    )
/*++
Routine Description:
    Collapse per-family IPC statuses into the response-level status.
Arguments:
    Response - Mutable IPC summary response.
Return Value:
    None.
--*/
{
    if (Response == NULL) {
        return;
    }
    if (Response->alpcStatus == KSWORD_ARK_ALPC_QUERY_STATUS_OK ||
        Response->namedPipeStatus == KSWORD_ARK_IPC_SUMMARY_STATUS_OK ||
        Response->mailslotStatus == KSWORD_ARK_IPC_SUMMARY_STATUS_OK) {
        Response->status = KSWORD_ARK_IPC_SUMMARY_STATUS_OK;
        return;
    }
    if (Response->alpcStatus == KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL ||
        Response->namedPipeStatus == KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED ||
        Response->mailslotStatus == KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED) {
        Response->status = KSWORD_ARK_IPC_SUMMARY_STATUS_PARTIAL;
        return;
    }
    Response->status = KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
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
    Build a read-only IPC summary from caller-supplied PID+handle evidence.
Arguments:
    OutputBuffer - METHOD_BUFFERED response buffer.
    OutputBufferLength - Output buffer length.
    Request - Query selector.
    BytesWrittenOut - Receives sizeof(response).
Return Value:
    STATUS_SUCCESS when a response packet was produced.
--*/
{
    KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE* response = (KSWORD_ARK_QUERY_IPC_SUMMARY_RESPONSE*)OutputBuffer;
    KSW_DYN_STATE dynState;
    ULONG flags = KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS nameStatus = STATUS_SUCCESS;
    PEPROCESS processObject = NULL;
    PVOID ipcObject = NULL;
    WCHAR objectName[KSW_KERNEL_IPC_NAME_CHARS];
    BOOLEAN hasHandleInput = FALSE;
    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL || OutputBufferLength < sizeof(*response) || Request == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(response, sizeof(*response));
    RtlZeroMemory(objectName, sizeof(objectName));
    response->version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->processId = Request->processId;
    response->handleValue = Request->handleValue;
    response->status = KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    response->alpcStatus = KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    response->namedPipeStatus = KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    response->mailslotStatus = KSWORD_ARK_IPC_SUMMARY_STATUS_UNAVAILABLE;
    KswordARKDynDataSnapshot(&dynState);
    response->dynDataCapabilityMask = dynState.CapabilityMask;
    flags = Request->flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL;
    if (flags == 0UL) {
        flags = KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALL;
    }
    hasHandleInput = (Request->processId != 0UL && Request->handleValue != 0ULL) ? TRUE : FALSE;
    if ((flags & KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_ALPC) != 0UL && hasHandleInput) {
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
        response->alpcStatus = NT_SUCCESS(status) ? alpcResponse.queryStatus : KSWORD_ARK_IPC_SUMMARY_STATUS_FAILED;
        if (NT_SUCCESS(status)) {
            response->alpcObjectAddress = alpcResponse.queryPort.objectAddress;
            response->dynDataCapabilityMask = alpcResponse.dynDataCapabilityMask;
            RtlCopyMemory(response->alpcTypeName, alpcResponse.typeName, sizeof(response->alpcTypeName));
        }
    }
    if (((flags & (KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_PIPE | KSWORD_ARK_IPC_QUERY_FLAG_INCLUDE_MAILSLOT)) != 0UL) &&
        hasHandleInput) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
        response->lastStatus = status;
        if (NT_SUCCESS(status)) {
            status = KswordARKKernelIpcReferenceHandleObject(processObject, Request->handleValue, &ipcObject);
            response->lastStatus = status;
        }
        if (NT_SUCCESS(status) && ipcObject != NULL) {
            nameStatus = KswordARKKernelIpcQueryObjectName(ipcObject, objectName, KSW_KERNEL_IPC_NAME_CHARS);
            response->lastStatus = nameStatus;
            if (NT_SUCCESS(nameStatus)) {
                KswordARKKernelIpcClassifyObjectName(objectName, KSW_KERNEL_IPC_NAME_CHARS, flags, response);
            }
            else {
                KswordARKKernelIpcApplyNameFailure(flags, response);
            }
        }
        else {
            KswordARKKernelIpcApplyNameFailure(flags, response);
        }
        if (ipcObject != NULL) {
            ObDereferenceObject(ipcObject);
        }
        if (processObject != NULL) {
            ObDereferenceObject(processObject);
        }
    }
    KswordARKKernelIpcFinalizeStatus(response);
    (VOID)RtlStringCchPrintfW(
        response->detail,
        KSWORD_ARK_KERNEL_OBJECT_DETAIL_CHARS,
        L"IPC summary is read-only and handle-backed; objectName=%ws.",
        objectName[0] != L'\0' ? objectName : L"<unavailable>");
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
