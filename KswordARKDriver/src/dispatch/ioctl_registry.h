#pragma once

#include <ntddk.h>
#include <wdf.h>

EXTERN_C_START

// Describes one isolated IOCTL implementation. The handler receives the WDF
// device/request, raw input/output byte counts, and writes the completion size.
typedef NTSTATUS
(*KSWORD_ARK_IOCTL_HANDLER)(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

// Registry row consumed by ioctl_dispatch.c. Name is for trace/log text, while
// RequiredCapability stores a KSW_CAP_* dependency used for Phase 1 fail-closed
// gating before dispatching private-offset features.
typedef struct _KSWORD_ARK_IOCTL_ENTRY
{
    ULONG IoControlCode;
    KSWORD_ARK_IOCTL_HANDLER Handler;
    const char* Name;
    ULONG64 RequiredCapability;
    ULONG Flags;
} KSWORD_ARK_IOCTL_ENTRY;

#define KSWORD_ARK_IOCTL_FLAG_NONE 0x00000000UL
#define KSWORD_ARK_IOCTL_CAPABILITY_NONE 0ULL

_Must_inspect_result_
const KSWORD_ARK_IOCTL_ENTRY*
KswordARKLookupIoctlEntry(
    _In_ ULONG IoControlCode
    );

EXTERN_C_END
