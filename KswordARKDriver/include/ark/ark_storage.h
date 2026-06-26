#pragma once

#include <ntddk.h>

#include "driver/KswordArkStorageIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKStorageQueryVolumeStackAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKStorageQueryBitLockerFveAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKStorageQueryMountMgrMappingAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKStorageQueryFileSystemIntegrityAudit(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_STORAGE_AUDIT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
