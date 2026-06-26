#pragma once

#include <ntddk.h>
#include "driver/KswordArkSecurityAuditIoctl.h"

EXTERN_C_START

// Query CI/VBS/Secure Kernel/SKCI posture into a fixed METHOD_BUFFERED response.
NTSTATUS
KswordARKSecurityAuditQuerySecurityStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

// Query loaded-driver trust cross-view rows into a bounded variable response.
NTSTATUS
KswordARKSecurityAuditQueryDriverTrustView(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

// Query Hyper-V layer availability and module-backed status skeleton.
NTSTATUS
KswordARKSecurityAuditQueryHyperVSummary(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

// Query AppID/AppLocker/mssecflt presence and owner status skeleton.
NTSTATUS
KswordARKSecurityAuditQueryAppControlStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
