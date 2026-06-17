#pragma once

#include <ntddk.h>

#include "driver/KswordArkKernelIoctl.h"

EXTERN_C_START

#define KSW_DRIVER_UNLOAD_DIAG_STAGE_REFERENCE     0x00000001UL
#define KSW_DRIVER_UNLOAD_DIAG_STAGE_PREFLIGHT     0x00000002UL
#define KSW_DRIVER_UNLOAD_DIAG_STAGE_ZW            0x00000004UL
#define KSW_DRIVER_UNLOAD_DIAG_STAGE_ZW_VERIFY     0x00000008UL
#define KSW_DRIVER_UNLOAD_DIAG_STAGE_DIRECT        0x00000010UL
#define KSW_DRIVER_UNLOAD_DIAG_STAGE_DIRECT_VERIFY 0x00000020UL

typedef struct _KSW_DRIVER_UNLOAD_DIAGNOSTICS
{
    ULONG stages;
    ULONG requestedFlags;
    ULONG sanitizedFlags;
    ULONG finalFlags;
    NTSTATUS referenceStatus;
    NTSTATUS preflightBuildStatus;
    NTSTATUS preflightStatus;
    NTSTATUS preflightDenyStatus;
    NTSTATUS zwRunStatus;
    NTSTATUS zwWaitStatus;
    NTSTATUS zwUnloadStatus;
    NTSTATUS zwVerifyStatus;
    NTSTATUS directRunStatus;
    NTSTATUS directWaitStatus;
    NTSTATUS directUnloadStatus;
    NTSTATUS directCleanupStatus;
    NTSTATUS directVerifyStatus;
    BOOLEAN allowZwUnload;
    BOOLEAN allowDirectUnload;
    BOOLEAN allowDestructiveCleanup;
    BOOLEAN hasServiceRegistryPath;
    BOOLEAN hasDriverUnload;
    BOOLEAN hasValidDynData;
    BOOLEAN hasPdbBackedDynData;
    BOOLEAN hasValidDriverObjectOffsets;
    BOOLEAN hasValidLoaderEvidence;
    BOOLEAN hasDeviceChain;
    BOOLEAN hasCrossDriverAttach;
    BOOLEAN hasDeviceLoop;
    BOOLEAN hasAttachedDevice;
    BOOLEAN hasBusyDeviceReference;
    BOOLEAN isCoreKernelModule;
    BOOLEAN isSelfModule;
    ULONGLONG driverStart;
    ULONGLONG loaderEntryAddress;
    ULONGLONG loaderDllBase;
    ULONG loaderSizeOfImage;
} KSW_DRIVER_UNLOAD_DIAGNOSTICS, *PKSW_DRIVER_UNLOAD_DIAGNOSTICS;

NTSTATUS
KswordARKDriverEnumerateSsdt(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_SSDT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryDriverObject(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_ const KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryDriverIntegrity(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_QUERY_DRIVER_INTEGRITY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryCpuHardware(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryPhysicalMemoryLayout(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverEnumerateShadowSsdt(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_SSDT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    );

NTSTATUS
KswordARKDriverScanInlineHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverPatchInlineHook(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverEnumerateIatEatHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverForceUnloadDriver(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut,
    _Out_opt_ KSW_DRIVER_UNLOAD_DIAGNOSTICS* Diagnostics
    );

EXTERN_C_END
