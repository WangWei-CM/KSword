#pragma once

#include <wdf.h>

// Maximum bytes per single log frame (including trailing NUL room).
#define KSWORD_ARK_LOG_ENTRY_MAX_BYTES 512

// Ring queue capacity in log-frame units.
#define KSWORD_ARK_LOG_RING_CAPACITY 64

EXTERN_C_START

NTSTATUS
KswordARKDriverInitializeLogChannel(
    _In_ WDFDEVICE Device
    );

NTSTATUS
KswordARKDriverEnqueueLogLine(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR FormattedLogLine
    );

NTSTATUS
KswordARKDriverEnqueueLogFrame(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR MessageText
    );

NTSTATUS
KswordARKDriverReadNextLogLine(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
