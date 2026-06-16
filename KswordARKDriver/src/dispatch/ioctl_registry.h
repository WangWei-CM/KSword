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
// 中文说明：查询型高频 IOCTL 可设置该位，dispatch 层仅在失败/拒绝时记录完成日志。
#define KSWORD_ARK_IOCTL_FLAG_QUIET_SUCCESS 0x00000001UL
#define KSWORD_ARK_IOCTL_CAPABILITY_NONE 0ULL

_Must_inspect_result_
const KSWORD_ARK_IOCTL_ENTRY*
KswordARKLookupIoctlEntry(
    _In_ ULONG IoControlCode
    );

ULONG
KswordARKGetRegisteredIoctlCount(
    VOID
    );

ULONG
KswordARKGetDuplicateIoctlCount(
    VOID
    );

EXTERN_C_END
