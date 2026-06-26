/*++

Module Name:

    dyndata_v4_query.c

Abstract:

    DynData v4 multi-module PDB profile query IOCTL handlers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "dyndata_v4_internal.h"
#include "ark/ark_log.h"
#include "../../dispatch/ioctl_validation.h"

#define KSW_DYN_V4_IOCTL_POOL_TAG '4DsK'

typedef PVOID
(NTAPI* KSW_DYN_V4_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

static PVOID
KswordARKDynDataV4AllocateRequestCopy(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    Allocate the nonpaged METHOD_BUFFERED input copy used by v4 apply. The copy
    keeps the request stable because buffered IOCTL input and output can point to
    the same WDF system buffer.

Arguments:

    BufferBytes - Number of request bytes to preserve.

Return Value:

    Nonpaged allocation on success; NULL on zero length or allocation failure.

--*/
{
    static volatile LONG allocatorResolved = 0;
    static KSW_DYN_V4_EX_ALLOCATE_POOL2_FN exAllocatePool2Fn = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&allocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        exAllocatePool2Fn = (KSW_DYN_V4_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (exAllocatePool2Fn != NULL) {
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, KSW_DYN_V4_IOCTL_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSW_DYN_V4_IOCTL_POOL_TAG);
#pragma warning(pop)
}

NTSTATUS
KswordARKDynDataV4QueryModules(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return cached v4 module profile status rows.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable byte count.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when the response header is written.

--*/
{
    KSW_QUERY_DYN_V4_MODULES_RESPONSE* response = NULL;
    ULONG index = 0UL;
    ULONG totalCount = 0UL;
    ULONG returnedCount = 0UL;
    ULONG capacity = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSW_QUERY_DYN_V4_MODULES_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSW_QUERY_DYN_V4_MODULES_RESPONSE*)OutputBuffer;
    response->version = KSW_DYN_V4_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSW_DYN_V4_MODULE_STATUS_ENTRY);
    capacity = (ULONG)((OutputBufferLength - KSW_QUERY_DYN_V4_MODULES_RESPONSE_HEADER_SIZE) / sizeof(KSW_DYN_V4_MODULE_STATUS_ENTRY));

    ExAcquirePushLockShared(&g_KswordDynDataV4Lock);
    for (index = 0UL; index < KSW_DYN_V4_MAX_MODULES; ++index) {
        if (!g_KswordDynDataV4State.Modules[index].Occupied) {
            continue;
        }
        totalCount += 1UL;
        if (returnedCount < capacity) {
            response->entries[returnedCount] = g_KswordDynDataV4State.Modules[index].PublicEntry;
            returnedCount += 1UL;
        }
    }
    ExReleasePushLockShared(&g_KswordDynDataV4Lock);

    response->totalCount = totalCount;
    response->returnedCount = returnedCount;
    response->size = (ULONG)(KSW_QUERY_DYN_V4_MODULES_RESPONSE_HEADER_SIZE + ((size_t)returnedCount * sizeof(KSW_DYN_V4_MODULE_STATUS_ENTRY)));
    *BytesWrittenOut = response->size;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDynDataV4QueryCapabilityGroups(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return cached v4 capability group coverage rows.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable byte count.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when the response header is written.

--*/
{
    KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE* response = NULL;
    ULONG moduleIndex = 0UL;
    ULONG groupIndex = 0UL;
    ULONG totalCount = 0UL;
    ULONG returnedCount = 0UL;
    ULONG capacity = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE*)OutputBuffer;
    response->version = KSW_DYN_V4_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY);
    capacity = (ULONG)((OutputBufferLength - KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE_HEADER_SIZE) / sizeof(KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY));

    ExAcquirePushLockShared(&g_KswordDynDataV4Lock);
    for (moduleIndex = 0UL; moduleIndex < KSW_DYN_V4_MAX_MODULES; ++moduleIndex) {
        if (!g_KswordDynDataV4State.Modules[moduleIndex].Occupied) {
            continue;
        }
        for (groupIndex = 0UL; groupIndex < g_KswordDynDataV4State.Modules[moduleIndex].PublicEntry.capabilityGroupCount; ++groupIndex) {
            totalCount += 1UL;
            if (returnedCount < capacity) {
                response->entries[returnedCount] = g_KswordDynDataV4State.Modules[moduleIndex].Groups[groupIndex].PublicEntry;
                returnedCount += 1UL;
            }
        }
    }
    ExReleasePushLockShared(&g_KswordDynDataV4Lock);

    response->totalCount = totalCount;
    response->returnedCount = returnedCount;
    response->size = (ULONG)(KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE_HEADER_SIZE + ((size_t)returnedCount * sizeof(KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY)));
    *BytesWrittenOut = response->size;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDynDataV4QueryMissingItems(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return cached v4 required/optional missing item summary rows.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable byte count.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when the response header is written.

--*/
{
    KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE* response = NULL;
    ULONG index = 0UL;
    ULONG returnedCount = 0UL;
    ULONG capacity = 0UL;
    ULONG totalCount = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE*)OutputBuffer;
    response->version = KSW_DYN_V4_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSW_DYN_V4_MISSING_ITEM_ENTRY);
    capacity = (ULONG)((OutputBufferLength - KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE_HEADER_SIZE) / sizeof(KSW_DYN_V4_MISSING_ITEM_ENTRY));

    ExAcquirePushLockShared(&g_KswordDynDataV4Lock);
    totalCount = g_KswordDynDataV4State.MissingCount;
    for (index = 0UL; index < totalCount && returnedCount < capacity; ++index) {
        response->entries[returnedCount] = g_KswordDynDataV4State.Missing[index];
        returnedCount += 1UL;
    }
    ExReleasePushLockShared(&g_KswordDynDataV4Lock);

    response->totalCount = totalCount;
    response->returnedCount = returnedCount;
    response->size = (ULONG)(KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE_HEADER_SIZE + ((size_t)returnedCount * sizeof(KSW_DYN_V4_MISSING_ITEM_ENTRY)));
    *BytesWrittenOut = response->size;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDynDataV4RetrieveOutput(
    _In_ WDFREQUEST Request,
    _In_ size_t HeaderBytes,
    _Outptr_result_bytebuffer_(*ActualOutputLength) PVOID* OutputBuffer,
    _Out_ size_t* ActualOutputLength
    )
/*++

Routine Description:

    Retrieve a v4 variable or fixed output buffer through the shared helper.

Arguments:

    Request - WDF IOCTL request.
    HeaderBytes - Minimum response header size.
    OutputBuffer - Receives output buffer.
    ActualOutputLength - Receives actual writable length.

Return Value:

    NTSTATUS from common output-buffer validation.

--*/
{
    return KswordARKRetrieveRequiredOutputBuffer(Request, HeaderBytes, OutputBuffer, ActualOutputLength);
}

NTSTATUS
KswordARKDynDataIoctlApplyProfileV4(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the buffered v4 module profile apply IOCTL.

Arguments:

    Device - WDF device used only for diagnostic logging.
    Request - WDF IOCTL request.
    InputBufferLength - Dispatcher-supplied input length.
    OutputBufferLength - Dispatcher-supplied output length.
    BytesReturned - Receives response bytes.

Return Value:

    STATUS_SUCCESS when a handled response is produced; otherwise validation status.

--*/
{
    KSW_APPLY_DYN_PROFILE_V4_REQUEST* inputBuffer = NULL;
    KSW_APPLY_DYN_PROFILE_V4_REQUEST* inputCopy = NULL;
    KSW_APPLY_DYN_PROFILE_V4_RESPONSE* outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    size_t copyLength = 0U;
    const size_t maxProfileRequestBytes =
        KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE +
        ((size_t)KSW_DYN_V4_MAX_ITEMS_PER_MODULE * sizeof(KSW_DYN_V4_ITEM_PACKET));
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKRetrieveRequiredInputBuffer(Request, KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE, (PVOID*)&inputBuffer, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    copyLength = actualInputLength;
    if (copyLength > maxProfileRequestBytes) {
        copyLength = maxProfileRequestBytes;
    }
    inputCopy = (KSW_APPLY_DYN_PROFILE_V4_REQUEST*)KswordARKDynDataV4AllocateRequestCopy(copyLength);
    if (inputCopy == NULL) {
        (VOID)KswordARKDriverEnqueueLogFrame(Device, "Error", "DynData v4 profile apply input copy allocation failed.");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(inputCopy, inputBuffer, copyLength);

    status = KswordARKRetrieveRequiredOutputBuffer(Request, sizeof(KSW_APPLY_DYN_PROFILE_V4_RESPONSE), (PVOID*)&outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(inputCopy, KSW_DYN_V4_IOCTL_POOL_TAG);
        return status;
    }

    status = KswordARKDynDataV4ApplyProfile(inputCopy, copyLength, outputBuffer, actualOutputLength, BytesReturned);
    ExFreePoolWithTag(inputCopy, KSW_DYN_V4_IOCTL_POOL_TAG);
    (VOID)KswordARKDriverEnqueueLogFrame(Device, NT_SUCCESS(status) ? "Info" : "Warn", "DynData v4 profile apply completed.");
    return (*BytesReturned >= sizeof(KSW_APPLY_DYN_PROFILE_V4_RESPONSE)) ? STATUS_SUCCESS : status;
}

NTSTATUS
KswordARKDynDataIoctlQueryV4Modules(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the v4 applied-module status query IOCTL.

Arguments:

    Device - WDF device reserved for parity with other handlers.
    Request - WDF IOCTL request.
    InputBufferLength - Unused query input length.
    OutputBufferLength - Dispatcher-supplied output length.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from buffer retrieval or query construction.

--*/
{
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    status = KswordARKDynDataV4RetrieveOutput(Request, KSW_QUERY_DYN_V4_MODULES_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return KswordARKDynDataV4QueryModules(outputBuffer, actualOutputLength, BytesReturned);
}

NTSTATUS
KswordARKDynDataIoctlQueryV4CapabilityGroups(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the v4 capability group coverage query IOCTL.

Arguments:

    Device - WDF device reserved for parity with other handlers.
    Request - WDF IOCTL request.
    InputBufferLength - Unused query input length.
    OutputBufferLength - Dispatcher-supplied output length.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from buffer retrieval or query construction.

--*/
{
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    status = KswordARKDynDataV4RetrieveOutput(Request, KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return KswordARKDynDataV4QueryCapabilityGroups(outputBuffer, actualOutputLength, BytesReturned);
}

NTSTATUS
KswordARKDynDataIoctlQueryV4MissingItems(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the v4 missing required/optional summary query IOCTL.

Arguments:

    Device - WDF device reserved for parity with other handlers.
    Request - WDF IOCTL request.
    InputBufferLength - Unused query input length.
    OutputBufferLength - Dispatcher-supplied output length.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from buffer retrieval or query construction.

--*/
{
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    status = KswordARKDynDataV4RetrieveOutput(Request, KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return KswordARKDynDataV4QueryMissingItems(outputBuffer, actualOutputLength, BytesReturned);
}
