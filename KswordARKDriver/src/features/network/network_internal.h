#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>

#include "ark/ark_network.h"
#include "ark/ark_log.h"

#ifndef RPC_C_AUTHN_WINNT
#define RPC_C_AUTHN_WINNT 10U
#endif

typedef struct _KSWORD_ARK_NETWORK_RUNTIME
{
    EX_PUSH_LOCK Lock;
    WDFDEVICE Device;
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT DeviceObject;
    HANDLE EngineHandle;
    UINT32 ConnectCalloutId;
    UINT32 RecvAcceptCalloutId;
    UINT64 ConnectFilterId;
    UINT64 RecvAcceptFilterId;
    NTSTATUS RegisterStatus;
    NTSTATUS EngineStatus;
    ULONG RuntimeFlags;
    ULONG RuleCount;
    ULONG BlockedRuleCount;
    ULONG HiddenPortRuleCount;
    ULONG Generation;
    volatile LONG64 ClassifyCount;
    volatile LONG64 BlockedCount;
    KSWORD_ARK_NETWORK_RULE Rules[KSWORD_ARK_NETWORK_MAX_RULES];
} KSWORD_ARK_NETWORK_RUNTIME;

EXTERN_C_START

KSWORD_ARK_NETWORK_RUNTIME*
KswordARKNetworkGetRuntime(
    VOID
    );

VOID
KswordARKNetworkLogFormat(
    _In_z_ PCSTR LevelText,
    _In_z_ _Printf_format_string_ PCSTR FormatText,
    ...
    );

BOOLEAN
KswordARKNetworkRuleMatchesLocked(
    _In_ const KSWORD_ARK_NETWORK_RULE* Rule,
    _In_ ULONG Direction,
    _In_ ULONG Protocol,
    _In_ USHORT LocalPort,
    _In_ USHORT RemotePort,
    _In_ ULONG ProcessId
    );

NTSTATUS
KswordARKNetworkWfpRegister(
    _Inout_ KSWORD_ARK_NETWORK_RUNTIME* Runtime
    );

VOID
KswordARKNetworkWfpUnregister(
    _Inout_ KSWORD_ARK_NETWORK_RUNTIME* Runtime
    );

EXTERN_C_END
