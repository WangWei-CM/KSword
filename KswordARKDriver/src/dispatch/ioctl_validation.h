#pragma once

#include <ntddk.h>
#include <wdf.h>

EXTERN_C_START

// Shared validation helpers keep WDF buffer retrieval and common access checks
// out of feature handlers while leaving semantic checks in each feature owner.
NTSTATUS
KswordARKRetrieveRequiredInputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t RequiredLength,
    _Outptr_result_bytebuffer_(*ActualLengthOut) PVOID* BufferOut,
    _Out_ size_t* ActualLengthOut
    );

NTSTATUS
KswordARKRetrieveOptionalInputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t SuppliedInputLength,
    _In_ size_t RequiredLength,
    _Outptr_result_bytebuffer_(*ActualLengthOut) PVOID* BufferOut,
    _Out_ size_t* ActualLengthOut,
    _Out_ BOOLEAN* PresentOut
    );

NTSTATUS
KswordARKRetrieveRequiredOutputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t RequiredLength,
    _Outptr_result_bytebuffer_(*ActualLengthOut) PVOID* BufferOut,
    _Out_ size_t* ActualLengthOut
    );

NTSTATUS
KswordARKValidateUserPid(
    _In_ ULONG ProcessId
    );

NTSTATUS
KswordARKValidateDeviceIoControlWriteAccess(
    _In_ WDFREQUEST Request
    );

EXTERN_C_END
