#pragma once

#include <ntddk.h>
#include "driver/KswordArkFileIoctl.h"

EXTERN_C_START

NTSTATUS
KswordARKDriverDeletePath(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory
    );

NTSTATUS
KswordARKDriverQueryFileInfo(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_FILE_INFO_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

/*
 * KswordARKDriverSetFileIntegrity
 * Inputs:
 * - Request contains a kernel/NT-style path and target S-1-16-* RID.
 * Processing:
 * - Opens the file object and calls ZwSetSecurityObject with
 *   LABEL_SECURITY_INFORMATION. It does not patch filesystem/private objects.
 * Return behavior:
 * - Returns the NTSTATUS from path open, security descriptor construction, or
 *   ZwSetSecurityObject.
 */
NTSTATUS
KswordARKDriverSetFileIntegrity(
    _In_ const KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST* Request
    );

EXTERN_C_END
