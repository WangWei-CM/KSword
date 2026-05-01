#pragma once

#include "ark/ark_alpc.h"
#include "ark/ark_dyndata.h"

EXTERN_C_START

// 中文说明：ALPC 查询内部共享结构，只保存已经 ObReferenceObject 的端口指针。
typedef struct _KSWORD_ARK_ALPC_REFERENCED_PORTS
{
    PVOID ConnectionPort;
    PVOID ServerPort;
    PVOID ClientPort;
} KSWORD_ARK_ALPC_REFERENCED_PORTS, *PKSWORD_ARK_ALPC_REFERENCED_PORTS;

BOOLEAN
KswordARKAlpcIsOffsetPresent(
    _In_ ULONG Offset
    );

BOOLEAN
KswordARKAlpcHasRequiredDynData(
    _In_ const KSW_DYN_STATE* DynState
    );

VOID
KswordARKAlpcPrepareOffsets(
    _Inout_ KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE* Response,
    _In_ const KSW_DYN_STATE* DynState
    );

NTSTATUS
KswordARKAlpcQueryTypeName(
    _In_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _Out_ BOOLEAN* TruncatedOut
    );

BOOLEAN
KswordARKAlpcIsTypeNameAlpcPort(
    _In_reads_(TypeNameChars) const WCHAR* TypeName,
    _In_ ULONG TypeNameChars
    );

NTSTATUS
KswordARKAlpcReferenceHandleObject(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG64 HandleValue,
    _Outptr_result_nullonfailure_ PVOID* ObjectOut
    );

NTSTATUS
KswordARKAlpcReadPointerField(
    _In_ PVOID Object,
    _In_ ULONG Offset,
    _Outptr_result_maybenull_ PVOID* PointerOut
    );

NTSTATUS
KswordARKAlpcPopulateBasicInfo(
    _In_ PVOID PortObject,
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_ALPC_PORT_INFO* PortInfo
    );

NTSTATUS
KswordARKAlpcPopulateNameInfo(
    _In_ PVOID PortObject,
    _Inout_ KSWORD_ARK_ALPC_PORT_INFO* PortInfo,
    _Out_ BOOLEAN* TruncatedOut
    );

NTSTATUS
KswordARKAlpcValidatePortPointer(
    _In_ PVOID CandidatePort,
    _In_ POBJECT_TYPE ExpectedType
    );

EXTERN_C_END
