#pragma once

#include "ark/ark_driver.h"
#include "driver/KswordArkDeviceAuditIoctl.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSW_DEVICE_AUDIT_POOL_TAG 'aDsK'
#define KSW_DEVICE_AUDIT_RESPONSE_HEADER_SIZE \
    (FIELD_OFFSET(KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE, entries))
#define KSW_DEVICE_AUDIT_INTEGRITY_RESPONSE_HEADER_SIZE \
    (FIELD_OFFSET(KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE, entries))
#define KSW_DEVICE_AUDIT_SCRATCH_ROW_LIMIT 256UL

typedef struct _KSW_DEVICE_AUDIT_TARGET
{
    PCWSTR DriverName;
    ULONG RoleHint;
} KSW_DEVICE_AUDIT_TARGET, *PKSW_DEVICE_AUDIT_TARGET;

typedef struct _KSW_DEVICE_AUDIT_REQUEST_CONTEXT
{
    KSWORD_ARK_QUERY_DEVICE_AUDIT_REQUEST Request;
    ULONG EffectiveProfile;
    ULONG MaxRows;
    ULONG MaxAttachedDepth;
    BOOLEAN HasSingleTarget;
} KSW_DEVICE_AUDIT_REQUEST_CONTEXT, *PKSW_DEVICE_AUDIT_REQUEST_CONTEXT;

EXTERN_C_START

extern const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditDeviceTargets[];
extern const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditInputTargets[];
extern const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditUsbTargets[];
extern const KSW_DEVICE_AUDIT_TARGET g_KswDeviceAuditGpuTargets[];
extern const ULONG g_KswDeviceAuditDeviceTargetCount;
extern const ULONG g_KswDeviceAuditInputTargetCount;
extern const ULONG g_KswDeviceAuditUsbTargetCount;
extern const ULONG g_KswDeviceAuditGpuTargetCount;

VOID
KswDeviceAuditLog(_In_ WDFDEVICE Device, _In_z_ PCSTR LevelText, _In_z_ PCSTR FormatText, ...);

VOID
KswDeviceAuditZeroResponse(_Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer, _In_ size_t OutputBufferLength, _In_ ULONG ProfileFlags);

ULONG
KswDeviceAuditNormalizeMaxRows(_In_ ULONG RequestedRows);

ULONG
KswDeviceAuditNormalizeAttachedDepth(_In_ ULONG RequestedDepth);

BOOLEAN
KswDeviceAuditStringPresent(_In_reads_(MaxChars) const WCHAR* Text, _In_ ULONG MaxChars);

VOID
KswDeviceAuditCopyWide(_Out_writes_(DestinationChars) WCHAR* Destination, _In_ ULONG DestinationChars, _In_opt_z_ PCWSTR Source);

VOID
KswDeviceAuditCopyServiceName(_Out_writes_(DestinationChars) WCHAR* Destination, _In_ ULONG DestinationChars, _In_z_ PCWSTR DriverName);

ULONG
KswDeviceAuditOutputCapacity(_In_ size_t OutputBufferLength);

ULONG
KswDeviceAuditMapRiskFlags(_In_ ULONG IntegrityRiskFlags);

ULONG
KswDeviceAuditMapStatus(_In_ ULONG IntegrityStatus, _In_ ULONG IntegrityRiskFlags);

VOID
KswDeviceAuditSetResponsePartial(_Inout_ KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE* Response, _In_ NTSTATUS LastStatus);

NTSTATUS
KswDeviceAuditAppendEntry(_Inout_ KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE* Response, _In_ ULONG Capacity, _In_ ULONG MaxRows, _In_ const KSWORD_ARK_DEVICE_AUDIT_ENTRY* SourceEntry);

NTSTATUS
KswDeviceAuditExecute(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned,
    _In_ ULONG HandlerProfile,
    _In_z_ PCSTR LogName
    );

EXTERN_C_END
