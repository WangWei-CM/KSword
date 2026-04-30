/*++

Module Name:

    ioctl_validation.c

Abstract:

    Shared IOCTL validation helpers for KswordARK handler modules.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ioctl_validation.h"

#include <wdm.h>

NTSTATUS
KswordARKRetrieveRequiredInputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t RequiredLength,
    _Outptr_result_bytebuffer_(*ActualLengthOut) PVOID* BufferOut,
    _Out_ size_t* ActualLengthOut
    )
/*++

Routine Description:

    Retrieve a required METHOD_BUFFERED input buffer from a WDF request. The
    helper clears output parameters before WDF is called.

Arguments:

    Request - Current WDF request.
    RequiredLength - Minimum accepted input length in bytes.
    BufferOut - Receives the input buffer pointer.
    ActualLengthOut - Receives the full input buffer length supplied by WDF.

Return Value:

    NTSTATUS from parameter validation or WdfRequestRetrieveInputBuffer.

--*/
{
    if (BufferOut == NULL || ActualLengthOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BufferOut = NULL;
    *ActualLengthOut = 0;
    return WdfRequestRetrieveInputBuffer(
        Request,
        RequiredLength,
        BufferOut,
        ActualLengthOut);
}

NTSTATUS
KswordARKRetrieveOptionalInputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t SuppliedInputLength,
    _In_ size_t RequiredLength,
    _Outptr_result_bytebuffer_(*ActualLengthOut) PVOID* BufferOut,
    _Out_ size_t* ActualLengthOut,
    _Out_ BOOLEAN* PresentOut
    )
/*++

Routine Description:

    Retrieve an optional input buffer only when the caller supplied enough bytes.
    This preserves legacy IOCTL behavior where missing request packets select a
    handler-specific default request.

Arguments:

    Request - Current WDF request.
    SuppliedInputLength - InputBufferLength from the dispatch callback.
    RequiredLength - Minimum length that makes the optional buffer present.
    BufferOut - Receives the input buffer when present; NULL otherwise.
    ActualLengthOut - Receives the WDF buffer length when present; zero otherwise.
    PresentOut - Receives TRUE when a buffer was retrieved.

Return Value:

    STATUS_SUCCESS for absent optional input, or WDF retrieval status when present.

--*/
{
    if (BufferOut == NULL || ActualLengthOut == NULL || PresentOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BufferOut = NULL;
    *ActualLengthOut = 0;
    *PresentOut = FALSE;
    if (SuppliedInputLength < RequiredLength) {
        return STATUS_SUCCESS;
    }

    *PresentOut = TRUE;
    return WdfRequestRetrieveInputBuffer(
        Request,
        RequiredLength,
        BufferOut,
        ActualLengthOut);
}

NTSTATUS
KswordARKRetrieveRequiredOutputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t RequiredLength,
    _Outptr_result_bytebuffer_(*ActualLengthOut) PVOID* BufferOut,
    _Out_ size_t* ActualLengthOut
    )
/*++

Routine Description:

    Retrieve a required METHOD_BUFFERED output buffer from a WDF request before a
    handler writes its fixed or variable-size response.

Arguments:

    Request - Current WDF request.
    RequiredLength - Minimum accepted output length in bytes.
    BufferOut - Receives the output buffer pointer.
    ActualLengthOut - Receives the full output buffer length supplied by WDF.

Return Value:

    NTSTATUS from parameter validation or WdfRequestRetrieveOutputBuffer.

--*/
{
    if (BufferOut == NULL || ActualLengthOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BufferOut = NULL;
    *ActualLengthOut = 0;
    return WdfRequestRetrieveOutputBuffer(
        Request,
        RequiredLength,
        BufferOut,
        ActualLengthOut);
}

NTSTATUS
KswordARKValidateUserPid(
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    Apply the existing user-action PID guard. PID 0 and core system pseudo/system
    PIDs up to 4 are rejected before feature code runs.

Arguments:

    ProcessId - Target process identifier supplied by user mode.

Return Value:

    STATUS_SUCCESS when accepted; STATUS_INVALID_PARAMETER when rejected.

--*/
{
    if (ProcessId == 0UL || ProcessId <= 4UL) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKValidateDeviceIoControlWriteAccess(
    _In_ WDFREQUEST Request
    )
/*++

Routine Description:

    Validate that the caller opened the device handle with FILE_WRITE_ACCESS for
    high-risk IOCTL paths.

Arguments:

    Request - Current WDF request whose underlying IRP carries access state.

Return Value:

    STATUS_SUCCESS when the IRP has write access, or the access-check status.

--*/
{
    return IoValidateDeviceIoControlAccess(
        WdfRequestWdmGetIrp(Request),
        FILE_WRITE_ACCESS);
}
