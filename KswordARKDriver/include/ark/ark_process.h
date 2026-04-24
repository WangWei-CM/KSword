#pragma once

#include <ntddk.h>

EXTERN_C_START

NTSTATUS
KswordARKDriverTerminateProcessByPid(
    _In_ ULONG processId,
    _In_ NTSTATUS exitStatus
    );

NTSTATUS
KswordARKDriverSuspendProcessByPid(
    _In_ ULONG processId
    );

NTSTATUS
KswordARKDriverSetProcessPplLevelByPid(
    _In_ ULONG processId,
    _In_ UCHAR protectionLevel
    );

EXTERN_C_END
