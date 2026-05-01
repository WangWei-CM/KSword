#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkSafetyIoctl.h"

EXTERN_C_START

typedef struct _KSWORD_ARK_SAFETY_CONTEXT
{
    ULONG Operation;
    ULONG TargetProcessId;
    ULONG ContextFlags;
    PCWSTR TargetText;
    USHORT TargetTextChars;
} KSWORD_ARK_SAFETY_CONTEXT, *PKSWORD_ARK_SAFETY_CONTEXT;

VOID
KswordARKSafetyInitialize(
    VOID
    );

NTSTATUS
KswordARKSafetyEvaluate(
    _In_ WDFDEVICE Device,
    _In_ const KSWORD_ARK_SAFETY_CONTEXT* Context
    );

NTSTATUS
KswordARKSafetyQueryPolicy(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKSafetySetPolicy(
    _In_ const KSWORD_ARK_SET_SAFETY_POLICY_REQUEST* Request,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
