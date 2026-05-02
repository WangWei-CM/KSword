/*++

Module Name:

    network_wfp.c

Abstract:

    WFP callout registration and classify implementation for KswordARK.

Environment:

    Kernel-mode WFP

--*/

#include "network_internal.h"



// 本文件只声明当前实现实际用到的 WFP 最小 ABI，避免直接包含 fwpsk.h/ndis.h
// 在当前 WDK/项目 Werror 组合下触发第三方头文件告警。
#define KSWORD_ARK_FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4 43U
#define KSWORD_ARK_FWPS_LAYER_ALE_AUTH_CONNECT_V4 47U
#define KSWORD_ARK_FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT 4U
#define KSWORD_ARK_FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_PROTOCOL 5U
#define KSWORD_ARK_FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT 7U
#define KSWORD_ARK_FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_LOCAL_PORT 4U
#define KSWORD_ARK_FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_PROTOCOL 5U
#define KSWORD_ARK_FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_REMOTE_PORT 7U
#define KSWORD_ARK_FWPS_METADATA_FIELD_PROCESS_ID 0x00000040ULL
#define KSWORD_ARK_FWPS_RIGHT_ACTION_WRITE 0x00000001U
#define KSWORD_ARK_FWPS_CLASSIFY_OUT_FLAG_ABSORB 0x00000001U
#define KSWORD_ARK_FWPM_SESSION_FLAG_DYNAMIC 0x00000001U
#define KSWORD_ARK_FWP_EMPTY 0U

// 中文说明：下面三个 action type 是 WDK fwptypes.h 暴露给 BFE 的固定 ABI 值。
// 中文说明：FWP_ACTION_* 由低位动作编号叠加 TERMINATING/CALLOUT 标志组成，不能手工
// 写成其它组合；错误值会让 FwpmFilterAdd0 返回 STATUS_FWP_INVALID_ACTION_TYPE。
#define KSWORD_ARK_FWP_ACTION_BLOCK 0x00001001U
#define KSWORD_ARK_FWP_ACTION_PERMIT 0x00001002U
#define KSWORD_ARK_FWP_ACTION_CALLOUT_TERMINATING 0x00005003U
#ifndef RPC_C_AUTHN_WINNT
#define RPC_C_AUTHN_WINNT 10U
#endif

typedef VOID* SEC_WINNT_AUTH_IDENTITY_W_PTR;
typedef UINT32 KSWORD_ARK_FWP_ACTION_TYPE;
typedef UINT32 KSWORD_ARK_FWP_DATA_TYPE;
typedef enum _KSWORD_ARK_FWPS_CALLOUT_NOTIFY_TYPE
{
    KswordArkFwpsCalloutNotifyAddFilter = 0,
    KswordArkFwpsCalloutNotifyDeleteFilter = 1,
    KswordArkFwpsCalloutNotifyTypeMax = 2
} KSWORD_ARK_FWPS_CALLOUT_NOTIFY_TYPE;

typedef struct _KSWORD_ARK_FWP_BYTE_BLOB
{
    UINT32 size;
    UINT8* data;
} KSWORD_ARK_FWP_BYTE_BLOB;

typedef struct _KSWORD_ARK_FWP_BYTE_ARRAY16
{
    UINT8 byteArray16[16];
} KSWORD_ARK_FWP_BYTE_ARRAY16;

typedef struct _KSWORD_ARK_FWP_VALUE0
{
    KSWORD_ARK_FWP_DATA_TYPE type;
    union
    {
        UINT8 uint8;
        UINT16 uint16;
        UINT32 uint32;
        UINT64* uint64;
        INT8 int8;
        INT16 int16;
        INT32 int32;
        INT64* int64;
        float float32;
        double* double64;
        KSWORD_ARK_FWP_BYTE_ARRAY16* byteArray16;
        KSWORD_ARK_FWP_BYTE_BLOB* byteBlob;
        VOID* sid;
        UINT8* sd;
        VOID* tokenInformation;
        UINT64* tokenAccessInformation;
        LPWSTR unicodeString;
        KSWORD_ARK_FWP_BYTE_BLOB* byteBlobArray6;
        VOID* bitmapArray64;
    } value;
} KSWORD_ARK_FWP_VALUE0;

typedef struct _KSWORD_ARK_FWPS_INCOMING_VALUE0
{
    UINT16 fieldId;
    KSWORD_ARK_FWP_VALUE0 value;
} KSWORD_ARK_FWPS_INCOMING_VALUE0;

typedef struct _KSWORD_ARK_FWPS_INCOMING_VALUES0
{
    UINT16 layerId;
    UINT32 valueCount;
    KSWORD_ARK_FWPS_INCOMING_VALUE0* incomingValue;
} KSWORD_ARK_FWPS_INCOMING_VALUES0;

typedef struct _KSWORD_ARK_FWPS_INCOMING_METADATA_VALUES0
{
    UINT32 currentMetadataValues;
    UINT32 flags;
    UINT64 reserved;
    UINT64 flowHandle;
    UINT32 ipHeaderSize;
    UINT32 transportHeaderSize;
    VOID* processPath;
    UINT64 token;
    UINT64 processId;
    UINT32 sourceInterfaceIndex;
    UINT32 destinationInterfaceIndex;
    ULONG compartmentId;
    UINT32 fragmentMetadata;
    UINT32 pathMtu;
    UINT64 completionHandle;
    UINT64 transportEndpointHandle;
    UINT64 remoteScopeId;
    UINT64 packetDirection;
    UINT64 etherFrameLength;
    VOID* parentEndpointHandle;
    UINT32 icmpIdAndSequence;
    UINT32 localRedirectTargetPID;
    VOID* originalDestination;
    UINT64 redirectRecords;
    UINT32 currentL2MetadataValues;
    UINT32 l2Flags;
    UINT32 ethernetMacHeaderSize;
    UINT32 wiFiOperationMode;
    VOID* vSwitchSourcePortId;
    VOID* vSwitchDestinationPortId;
    UINT32 padding0;
    KSWORD_ARK_FWP_BYTE_BLOB* rawData;
} KSWORD_ARK_FWPS_INCOMING_METADATA_VALUES0;

typedef struct _KSWORD_ARK_FWPS_FILTER0
{
    UINT64 filterId;
    UINT64 weight;
    UINT16 subLayerWeight;
    UINT16 flags;
    UINT32 numFilterConditions;
    VOID* filterCondition;
    KSWORD_ARK_FWP_ACTION_TYPE actionType;
    UINT64 context;
    GUID* providerContextKey;
} KSWORD_ARK_FWPS_FILTER0;

typedef struct _KSWORD_ARK_FWPS_CLASSIFY_OUT0
{
    KSWORD_ARK_FWP_ACTION_TYPE actionType;
    UINT64 outContext;
    UINT64 filterId;
    UINT32 rights;
    UINT32 flags;
    UINT32 reserved;
} KSWORD_ARK_FWPS_CLASSIFY_OUT0;

typedef VOID (NTAPI* KSWORD_ARK_FWPS_CALLOUT_CLASSIFY_FN0)(
    _In_ const KSWORD_ARK_FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const KSWORD_ARK_FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ VOID* LayerData,
    _In_opt_ const KSWORD_ARK_FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ KSWORD_ARK_FWPS_CLASSIFY_OUT0* ClassifyOut);

typedef NTSTATUS (NTAPI* KSWORD_ARK_FWPS_CALLOUT_NOTIFY_FN0)(
    _In_ KSWORD_ARK_FWPS_CALLOUT_NOTIFY_TYPE NotifyType,
    _In_ const GUID* FilterKey,
    _Inout_ const KSWORD_ARK_FWPS_FILTER0* Filter);

typedef VOID (NTAPI* KSWORD_ARK_FWPS_CALLOUT_FLOW_DELETE_NOTIFY_FN0)(
    _In_ UINT16 LayerId,
    _In_ UINT32 CalloutId,
    _In_ UINT64 FlowContext);

typedef struct _KSWORD_ARK_FWPS_CALLOUT0
{
    GUID calloutKey;
    UINT32 flags;
    KSWORD_ARK_FWPS_CALLOUT_CLASSIFY_FN0 classifyFn;
    KSWORD_ARK_FWPS_CALLOUT_NOTIFY_FN0 notifyFn;
    KSWORD_ARK_FWPS_CALLOUT_FLOW_DELETE_NOTIFY_FN0 flowDeleteFn;
} KSWORD_ARK_FWPS_CALLOUT0;

typedef struct _KSWORD_ARK_FWPM_DISPLAY_DATA0
{
    WCHAR* name;
    WCHAR* description;
} KSWORD_ARK_FWPM_DISPLAY_DATA0;

typedef struct _KSWORD_ARK_FWPM_SESSION0
{
    GUID sessionKey;
    KSWORD_ARK_FWPM_DISPLAY_DATA0 displayData;
    UINT32 flags;
    UINT32 txnWaitTimeoutInMSec;
    UINT32 processId;
    VOID* sid;
    WCHAR* username;
    BOOLEAN kernelMode;
} KSWORD_ARK_FWPM_SESSION0;

typedef struct _KSWORD_ARK_FWPM_CALLOUT0
{
    GUID calloutKey;
    KSWORD_ARK_FWPM_DISPLAY_DATA0 displayData;
    UINT32 flags;
    GUID* providerKey;
    KSWORD_ARK_FWP_BYTE_BLOB providerData;
    GUID applicableLayer;
    UINT32 calloutId;
} KSWORD_ARK_FWPM_CALLOUT0;

typedef union _KSWORD_ARK_FWPM_ACTION0
{
    KSWORD_ARK_FWP_ACTION_TYPE type;
    GUID filterType;
    GUID calloutKey;
} KSWORD_ARK_FWPM_ACTION0;

typedef struct _KSWORD_ARK_FWPM_FILTER0
{
    GUID filterKey;
    KSWORD_ARK_FWPM_DISPLAY_DATA0 displayData;
    UINT32 flags;
    GUID* providerKey;
    KSWORD_ARK_FWP_BYTE_BLOB providerData;
    GUID layerKey;
    GUID subLayerKey;
    KSWORD_ARK_FWP_VALUE0 weight;
    UINT32 numFilterConditions;
    VOID* filterCondition;
    KSWORD_ARK_FWPM_ACTION0 action;
    union
    {
        UINT64 rawContext;
        GUID providerContextKey;
    } rawContextUnion;
    GUID* reserved;
    UINT64 filterId;
    KSWORD_ARK_FWP_VALUE0 effectiveWeight;
} KSWORD_ARK_FWPM_FILTER0;

typedef struct _KSWORD_ARK_FWPM_SUBLAYER0
{
    GUID subLayerKey;
    KSWORD_ARK_FWPM_DISPLAY_DATA0 displayData;
    UINT32 flags;
    GUID* providerKey;
    KSWORD_ARK_FWP_BYTE_BLOB providerData;
    UINT16 weight;
} KSWORD_ARK_FWPM_SUBLAYER0;

NTSYSAPI
NTSTATUS
NTAPI
FwpsCalloutRegister0(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _In_ const KSWORD_ARK_FWPS_CALLOUT0* Callout,
    _Out_opt_ UINT32* CalloutId);

NTSYSAPI
NTSTATUS
NTAPI
FwpsCalloutUnregisterById0(
    _In_ UINT32 CalloutId);

NTSYSAPI
NTSTATUS
NTAPI
FwpmEngineOpen0(
    _In_opt_ const WCHAR* ServerName,
    _In_ UINT32 AuthnService,
    _In_opt_ SEC_WINNT_AUTH_IDENTITY_W_PTR AuthIdentity,
    _In_opt_ const KSWORD_ARK_FWPM_SESSION0* Session,
    _Out_ HANDLE* EngineHandle);

NTSYSAPI
NTSTATUS
NTAPI
FwpmEngineClose0(
    _In_ HANDLE EngineHandle);

NTSYSAPI
NTSTATUS
NTAPI
FwpmTransactionBegin0(
    _In_ HANDLE EngineHandle,
    _In_ UINT32 Flags);

NTSYSAPI
NTSTATUS
NTAPI
FwpmTransactionCommit0(
    _In_ HANDLE EngineHandle);

NTSYSAPI
NTSTATUS
NTAPI
FwpmTransactionAbort0(
    _In_ HANDLE EngineHandle);

NTSYSAPI
NTSTATUS
NTAPI
FwpmSubLayerAdd0(
    _In_ HANDLE EngineHandle,
    _In_ const KSWORD_ARK_FWPM_SUBLAYER0* SubLayer,
    _In_opt_ PSECURITY_DESCRIPTOR Sd);

NTSYSAPI
NTSTATUS
NTAPI
FwpmSubLayerDeleteByKey0(
    _In_ HANDLE EngineHandle,
    _In_ const GUID* Key);

NTSYSAPI
NTSTATUS
NTAPI
FwpmCalloutAdd0(
    _In_ HANDLE EngineHandle,
    _In_ const KSWORD_ARK_FWPM_CALLOUT0* Callout,
    _In_opt_ PSECURITY_DESCRIPTOR Sd,
    _Out_opt_ UINT32* Id);

NTSYSAPI
NTSTATUS
NTAPI
FwpmCalloutDeleteByKey0(
    _In_ HANDLE EngineHandle,
    _In_ const GUID* Key);

NTSYSAPI
NTSTATUS
NTAPI
FwpmFilterAdd0(
    _In_ HANDLE EngineHandle,
    _In_ const KSWORD_ARK_FWPM_FILTER0* Filter,
    _In_opt_ PSECURITY_DESCRIPTOR Sd,
    _Out_opt_ UINT64* Id);

NTSYSAPI
NTSTATUS
NTAPI
FwpmFilterDeleteById0(
    _In_ HANDLE EngineHandle,
    _In_ UINT64 Id);

// {D2B28BC6-9E08-4D07-9F7B-2BA821D8AA51}
static const GUID KSWORD_ARK_WFP_SUBLAYER =
{ 0xd2b28bc6, 0x9e08, 0x4d07, { 0x9f, 0x7b, 0x2b, 0xa8, 0x21, 0xd8, 0xaa, 0x51 } };

// {9CEBA6FD-DC43-4E48-A013-FDB83823674B}
static const GUID KSWORD_ARK_WFP_CONNECT_CALLOUT =
{ 0x9ceba6fd, 0xdc43, 0x4e48, { 0xa0, 0x13, 0xfd, 0xb8, 0x38, 0x23, 0x67, 0x4b } };

// {75FCE1D8-0E28-4C58-9957-90181B75B6AA}
static const GUID KSWORD_ARK_WFP_RECV_ACCEPT_CALLOUT =
{ 0x75fce1d8, 0x0e28, 0x4c58, { 0x99, 0x57, 0x90, 0x18, 0x1b, 0x75, 0xb6, 0xaa } };

// {C38D57D1-05A7-4C33-904F-7FBCEEE60E82}
static const GUID KSWORD_ARK_FWPM_LAYER_ALE_AUTH_CONNECT_V4 =
{ 0xc38d57d1, 0x05a7, 0x4c33, { 0x90, 0x4f, 0x7f, 0xbc, 0xee, 0xe6, 0x0e, 0x82 } };

// {E1CD9FE7-F4B5-4273-96C0-592E487B8650}
static const GUID KSWORD_ARK_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4 =
{ 0xe1cd9fe7, 0xf4b5, 0x4273, { 0x96, 0xc0, 0x59, 0x2e, 0x48, 0x7b, 0x86, 0x50 } };

static UINT16
KswordARKNetworkUshortHostOrder(
    _In_ UINT16 NetworkOrderValue
    )
/*++

Routine Description:

    将 WFP 元数据中的网络序端口转换为主机序。中文说明：WFP ALE 字段通常使用
    网络字节序，规则协议使用人类可读端口值，因此 classify 前统一转换。

Arguments:

    NetworkOrderValue - 网络字节序 16 位端口。

Return Value:

    主机字节序端口。

--*/
{
    return (UINT16)(((NetworkOrderValue & 0x00FFU) << 8) | ((NetworkOrderValue & 0xFF00U) >> 8));
}

static ULONG
KswordARKNetworkReadUint32Field(
    _In_ const KSWORD_ARK_FWPS_INCOMING_VALUES0* Values,
    _In_ UINT32 FieldIndex
    )
/*++

Routine Description:

    从 WFP incoming values 读取 UINT32 字段。中文说明：调用方传入层对应字段索引，
    函数只做越界与类型宽度保护。

Arguments:

    Values - WFP classify 输入字段集合。
    FieldIndex - 字段索引。

Return Value:

    字段值；字段不存在时返回 0。

--*/
{
    if (Values == NULL || FieldIndex >= Values->valueCount) {
        return 0UL;
    }
    return Values->incomingValue[FieldIndex].value.value.uint32;
}

static ULONG
KswordARKNetworkReadUint16Field(
    _In_ const KSWORD_ARK_FWPS_INCOMING_VALUES0* Values,
    _In_ UINT32 FieldIndex
    )
/*++

Routine Description:

    从 WFP incoming values 读取 UINT16 字段。中文说明：端口字段按 WFP 定义读取，
    再由调用方决定是否做字节序转换。

Arguments:

    Values - WFP classify 输入字段集合。
    FieldIndex - 字段索引。

Return Value:

    字段值；字段不存在时返回 0。

--*/
{
    if (Values == NULL || FieldIndex >= Values->valueCount) {
        return 0UL;
    }
    return (ULONG)Values->incomingValue[FieldIndex].value.value.uint16;
}

static ULONG
KswordARKNetworkReadProcessId(
    _In_ const KSWORD_ARK_FWPS_INCOMING_METADATA_VALUES0* Metadata
    )
/*++

Routine Description:

    从 WFP metadata 提取进程 ID。中文说明：不是所有层都有 processId，缺失时返回
    0，让仅按端口/协议匹配的规则仍可工作。

Arguments:

    Metadata - WFP metadata。

Return Value:

    进程 ID；未知时返回 0。

--*/
{
    if (Metadata == NULL) {
        return 0UL;
    }
    if ((Metadata->currentMetadataValues & KSWORD_ARK_FWPS_METADATA_FIELD_PROCESS_ID) == 0ULL) {
        return 0UL;
    }
    return (ULONG)(ULONG_PTR)Metadata->processId;
}

static BOOLEAN
KswordARKNetworkClassifyExtractTuple(
    _In_ const KSWORD_ARK_FWPS_INCOMING_VALUES0* Values,
    _In_ const KSWORD_ARK_FWPS_INCOMING_METADATA_VALUES0* Metadata,
    _Out_ ULONG* DirectionOut,
    _Out_ ULONG* ProtocolOut,
    _Out_ USHORT* LocalPortOut,
    _Out_ USHORT* RemotePortOut,
    _Out_ ULONG* ProcessIdOut
    )
/*++

Routine Description:

    从 ALE connect/recv-accept 层提取规则匹配所需摘要。中文说明：当前仅覆盖 IPv4
    TCP/UDP，IPv6 可以后续用同一规则结构扩展。

Arguments:

    Values - WFP classify 输入字段集合。
    Metadata - WFP metadata。
    DirectionOut - 返回入站/出站方向。
    ProtocolOut - 返回协议号。
    LocalPortOut - 返回本地端口。
    RemotePortOut - 返回远端端口。
    ProcessIdOut - 返回进程 ID。

Return Value:

    TRUE 表示字段提取成功；FALSE 表示层不受支持。

--*/
{
    UINT16 localPortNetwork = 0U;
    UINT16 remotePortNetwork = 0U;

    if (Values == NULL || DirectionOut == NULL || ProtocolOut == NULL ||
        LocalPortOut == NULL || RemotePortOut == NULL || ProcessIdOut == NULL) {
        return FALSE;
    }

    if (Values->layerId == KSWORD_ARK_FWPS_LAYER_ALE_AUTH_CONNECT_V4) {
        *DirectionOut = KSWORD_ARK_NETWORK_DIRECTION_OUTBOUND;
        *ProtocolOut = KswordARKNetworkReadUint32Field(Values, KSWORD_ARK_FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_PROTOCOL);
        localPortNetwork = (UINT16)KswordARKNetworkReadUint16Field(Values, KSWORD_ARK_FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT);
        remotePortNetwork = (UINT16)KswordARKNetworkReadUint16Field(Values, KSWORD_ARK_FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT);
    }
    else if (Values->layerId == KSWORD_ARK_FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4) {
        *DirectionOut = KSWORD_ARK_NETWORK_DIRECTION_INBOUND;
        *ProtocolOut = KswordARKNetworkReadUint32Field(Values, KSWORD_ARK_FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_PROTOCOL);
        localPortNetwork = (UINT16)KswordARKNetworkReadUint16Field(Values, KSWORD_ARK_FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_LOCAL_PORT);
        remotePortNetwork = (UINT16)KswordARKNetworkReadUint16Field(Values, KSWORD_ARK_FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_IP_REMOTE_PORT);
    }
    else {
        return FALSE;
    }

    *LocalPortOut = KswordARKNetworkUshortHostOrder(localPortNetwork);
    *RemotePortOut = KswordARKNetworkUshortHostOrder(remotePortNetwork);
    *ProcessIdOut = KswordARKNetworkReadProcessId(Metadata);
    return TRUE;
}

static BOOLEAN
KswordARKNetworkShouldBlockClassify(
    _In_ ULONG Direction,
    _In_ ULONG Protocol,
    _In_ USHORT LocalPort,
    _In_ USHORT RemotePort,
    _In_ ULONG ProcessId
    )
/*++

Routine Description:

    在规则快照中查找阻断规则。中文说明：allow 规则优先返回 FALSE，block 规则命中
    返回 TRUE，hide-port 规则不影响网络放行。

Arguments:

    Direction - 当前方向。
    Protocol - 协议号。
    LocalPort - 本地端口。
    RemotePort - 远端端口。
    ProcessId - 进程 ID。

Return Value:

    TRUE 表示阻断；FALSE 表示放行。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    ULONG ruleIndex = 0UL;
    BOOLEAN shouldBlock = FALSE;

    if ((runtime->RuntimeFlags & KSWORD_ARK_NETWORK_RUNTIME_RULES_ACTIVE) == 0UL) {
        return FALSE;
    }

    ExAcquirePushLockShared(&runtime->Lock);
    for (ruleIndex = 0UL; ruleIndex < KSWORD_ARK_NETWORK_MAX_RULES; ++ruleIndex) {
        const KSWORD_ARK_NETWORK_RULE* rule = &runtime->Rules[ruleIndex];
        if (rule->action != KSWORD_ARK_NETWORK_RULE_ACTION_ALLOW &&
            rule->action != KSWORD_ARK_NETWORK_RULE_ACTION_BLOCK) {
            continue;
        }
        if (!KswordARKNetworkRuleMatchesLocked(
            rule,
            Direction,
            Protocol,
            LocalPort,
            RemotePort,
            ProcessId)) {
            continue;
        }
        shouldBlock = (rule->action == KSWORD_ARK_NETWORK_RULE_ACTION_BLOCK) ? TRUE : FALSE;
        break;
    }
    ExReleasePushLockShared(&runtime->Lock);

    return shouldBlock;
}

VOID NTAPI
KswordARKNetworkClassifyFn(
    _In_ const KSWORD_ARK_FWPS_INCOMING_VALUES0* inFixedValues,
    _In_ const KSWORD_ARK_FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ VOID* layerData,
    _In_opt_ const KSWORD_ARK_FWPS_FILTER0* filter,
    _In_ UINT64 flowContext,
    _Inout_ KSWORD_ARK_FWPS_CLASSIFY_OUT0* classifyOut
    )
/*++

Routine Description:

    WFP ALE classify 回调。中文说明：只在规则命中 block 时设置 BLOCK，其他情况保持
    PERMIT，且遵守 KSWORD_ARK_FWPS_RIGHT_ACTION_WRITE 权限。

Arguments:

    inFixedValues - WFP 输入字段。
    inMetaValues - WFP metadata。
    layerData - 未使用。
    filter - 未使用。
    flowContext - 未使用。
    classifyOut - WFP 动作输出。

Return Value:

    None. 本函数没有返回值。

--*/
{
    KSWORD_ARK_NETWORK_RUNTIME* runtime = KswordARKNetworkGetRuntime();
    ULONG direction = 0UL;
    ULONG protocol = 0UL;
    USHORT localPort = 0U;
    USHORT remotePort = 0U;
    ULONG processId = 0UL;
    BOOLEAN shouldBlock = FALSE;

    UNREFERENCED_PARAMETER(layerData);
    UNREFERENCED_PARAMETER(filter);
    UNREFERENCED_PARAMETER(flowContext);

    if (classifyOut == NULL || (classifyOut->rights & KSWORD_ARK_FWPS_RIGHT_ACTION_WRITE) == 0U) {
        return;
    }

    InterlockedIncrement64(&runtime->ClassifyCount);
    if (!KswordARKNetworkClassifyExtractTuple(
        inFixedValues,
        inMetaValues,
        &direction,
        &protocol,
        &localPort,
        &remotePort,
        &processId)) {
        classifyOut->actionType = KSWORD_ARK_FWP_ACTION_PERMIT;
        return;
    }

    shouldBlock = KswordARKNetworkShouldBlockClassify(
        direction,
        protocol,
        localPort,
        remotePort,
        processId);
    if (shouldBlock) {
        classifyOut->actionType = KSWORD_ARK_FWP_ACTION_BLOCK;
        classifyOut->rights &= ~KSWORD_ARK_FWPS_RIGHT_ACTION_WRITE;
        classifyOut->flags |= KSWORD_ARK_FWPS_CLASSIFY_OUT_FLAG_ABSORB;
        InterlockedIncrement64(&runtime->BlockedCount);
    }
    else {
        classifyOut->actionType = KSWORD_ARK_FWP_ACTION_PERMIT;
    }
}

NTSTATUS NTAPI
KswordARKNetworkNotifyFn(
    _In_ KSWORD_ARK_FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_ const GUID* filterKey,
    _Inout_ const KSWORD_ARK_FWPS_FILTER0* filter
    )
/*++

Routine Description:

    WFP notify 回调。中文说明：当前不维护 flow 上下文，因此只接受通知并返回成功。

Arguments:

    notifyType - 通知类型。
    filterKey - 过滤器 key。
    filter - 过滤器对象。

Return Value:

    STATUS_SUCCESS。

--*/
{
    UNREFERENCED_PARAMETER(notifyType);
    UNREFERENCED_PARAMETER(filterKey);
    UNREFERENCED_PARAMETER(filter);
    return STATUS_SUCCESS;
}

VOID NTAPI
KswordARKNetworkFlowDeleteFn(
    _In_ UINT16 layerId,
    _In_ UINT32 calloutId,
    _In_ UINT64 flowContext
    )
/*++

Routine Description:

    WFP flow-delete 回调。中文说明：当前不分配 flow context，因此无需释放资源。

Arguments:

    layerId - WFP 层 ID。
    calloutId - callout ID。
    flowContext - flow 上下文。

Return Value:

    None. 本函数没有返回值。

--*/
{
    UNREFERENCED_PARAMETER(layerId);
    UNREFERENCED_PARAMETER(calloutId);
    UNREFERENCED_PARAMETER(flowContext);
}

static NTSTATUS
KswordARKNetworkRegisterCallout(
    _In_ KSWORD_ARK_NETWORK_RUNTIME* Runtime,
    _In_ const GUID* CalloutKey,
    _Out_ UINT32* CalloutIdOut
    )
/*++

Routine Description:

    调用 FwpsCalloutRegister 注册内核 classify 入口。中文说明：两个 ALE 层复用同一
    classify 函数，通过 layerId 区分方向。

Arguments:

    Runtime - 网络运行时。
    CalloutKey - callout GUID。
    CalloutIdOut - 返回 callout id。

Return Value:

    FwpsCalloutRegister0 返回状态。

--*/
{
    KSWORD_ARK_FWPS_CALLOUT0 callout;

    if (Runtime == NULL || Runtime->DeviceObject == NULL || CalloutKey == NULL || CalloutIdOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&callout, sizeof(callout));
    callout.calloutKey = *CalloutKey;
    callout.classifyFn = KswordARKNetworkClassifyFn;
    callout.notifyFn = KswordARKNetworkNotifyFn;
    callout.flowDeleteFn = KswordARKNetworkFlowDeleteFn;
    return FwpsCalloutRegister0(
        Runtime->DeviceObject,
        &callout,
        CalloutIdOut);
}

static NTSTATUS
KswordARKNetworkAddCalloutToEngine(
    _In_ KSWORD_ARK_NETWORK_RUNTIME* Runtime,
    _In_ const GUID* CalloutKey,
    _In_ const GUID* LayerKey,
    _In_z_ PCWSTR DisplayName
    )
/*++

Routine Description:

    向 BFE 添加 callout 对象。中文说明：FWPM callout 绑定到具体 layer，随后 filter
    才能引用该 callout。

Arguments:

    Runtime - 网络运行时。
    CalloutKey - callout GUID。
    LayerKey - WFP layer GUID。
    DisplayName - 显示名称。

Return Value:

    FwpmCalloutAdd0 返回状态。

--*/
{
    KSWORD_ARK_FWPM_CALLOUT0 callout;

    if (Runtime == NULL || Runtime->EngineHandle == NULL || CalloutKey == NULL || LayerKey == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&callout, sizeof(callout));
    callout.calloutKey = *CalloutKey;
    callout.displayData.name = (PWSTR)DisplayName;
    callout.applicableLayer = *LayerKey;
    return FwpmCalloutAdd0(Runtime->EngineHandle, &callout, NULL, NULL);
}

static NTSTATUS
KswordARKNetworkAddFilter(
    _In_ KSWORD_ARK_NETWORK_RUNTIME* Runtime,
    _In_ const GUID* CalloutKey,
    _In_ const GUID* LayerKey,
    _In_z_ PCWSTR DisplayName,
    _Out_ UINT64* FilterIdOut
    )
/*++

Routine Description:

    添加匹配所有流量的 callout filter。中文说明：具体放行/阻断由 classify 内部的
    KswordARK 规则表决定，filter 本身不保存用户规则。

Arguments:

    Runtime - 网络运行时。
    CalloutKey - callout GUID。
    LayerKey - WFP layer GUID。
    DisplayName - 显示名称。
    FilterIdOut - 返回 filter id。

Return Value:

    FwpmFilterAdd0 返回状态。

--*/
{
    KSWORD_ARK_FWPM_FILTER0 filter;

    if (Runtime == NULL || Runtime->EngineHandle == NULL || CalloutKey == NULL ||
        LayerKey == NULL || FilterIdOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&filter, sizeof(filter));
    filter.layerKey = *LayerKey;
    filter.displayData.name = (PWSTR)DisplayName;
    filter.action.type = KSWORD_ARK_FWP_ACTION_CALLOUT_TERMINATING;
    filter.action.calloutKey = *CalloutKey;
    filter.subLayerKey = KSWORD_ARK_WFP_SUBLAYER;
    filter.weight.type = KSWORD_ARK_FWP_EMPTY;
    filter.numFilterConditions = 0U;
    filter.filterCondition = NULL;
    return FwpmFilterAdd0(Runtime->EngineHandle, &filter, NULL, FilterIdOut);
}

static NTSTATUS
KswordARKNetworkAddSublayer(
    _In_ KSWORD_ARK_NETWORK_RUNTIME* Runtime
    )
/*++

Routine Description:

    向 BFE 添加 KswordARK 子层。中文说明：子层权重设置为中等值，避免压过系统关键
    安全策略，同时保证本驱动 filter 可分组清理。

Arguments:

    Runtime - 网络运行时。

Return Value:

    FwpmSubLayerAdd0 返回状态。

--*/
{
    KSWORD_ARK_FWPM_SUBLAYER0 subLayer;

    if (Runtime == NULL || Runtime->EngineHandle == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&subLayer, sizeof(subLayer));
    subLayer.subLayerKey = KSWORD_ARK_WFP_SUBLAYER;
    subLayer.displayData.name = L"KswordARK Network Filter";
    subLayer.weight = 0x4000U;
    return FwpmSubLayerAdd0(Runtime->EngineHandle, &subLayer, NULL);
}

NTSTATUS
KswordARKNetworkWfpRegister(
    _Inout_ KSWORD_ARK_NETWORK_RUNTIME* Runtime
    )
/*++

Routine Description:

    注册 KswordARK WFP callout、sublayer 和 filter。中文说明：任何一步失败都会
    调用注销路径清理已创建对象，避免残留 BFE 项。

Arguments:

    Runtime - 网络运行时。

Return Value:

    STATUS_SUCCESS 或 WFP API 返回状态。

--*/
{
    KSWORD_ARK_FWPM_SESSION0 session;
    NTSTATUS status = STATUS_SUCCESS;

    if (Runtime == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKNetworkRegisterCallout(
        Runtime,
        &KSWORD_ARK_WFP_CONNECT_CALLOUT,
        &Runtime->ConnectCalloutId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKNetworkRegisterCallout(
        Runtime,
        &KSWORD_ARK_WFP_RECV_ACCEPT_CALLOUT,
        &Runtime->RecvAcceptCalloutId);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkWfpUnregister(Runtime);
        return status;
    }

    RtlZeroMemory(&session, sizeof(session));
    session.flags = KSWORD_ARK_FWPM_SESSION_FLAG_DYNAMIC;
    status = FwpmEngineOpen0(
        NULL,
        RPC_C_AUTHN_WINNT,
        NULL,
        &session,
        &Runtime->EngineHandle);
    Runtime->EngineStatus = status;
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkWfpUnregister(Runtime);
        return status;
    }

    status = FwpmTransactionBegin0(Runtime->EngineHandle, 0U);
    if (!NT_SUCCESS(status)) {
        KswordARKNetworkWfpUnregister(Runtime);
        return status;
    }

    status = KswordARKNetworkAddSublayer(Runtime);
    if (NT_SUCCESS(status)) {
        status = KswordARKNetworkAddCalloutToEngine(
            Runtime,
            &KSWORD_ARK_WFP_CONNECT_CALLOUT,
            &KSWORD_ARK_FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            L"KswordARK ALE connect callout");
    }
    if (NT_SUCCESS(status)) {
        status = KswordARKNetworkAddCalloutToEngine(
            Runtime,
            &KSWORD_ARK_WFP_RECV_ACCEPT_CALLOUT,
            &KSWORD_ARK_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            L"KswordARK ALE recv-accept callout");
    }
    if (NT_SUCCESS(status)) {
        status = KswordARKNetworkAddFilter(
            Runtime,
            &KSWORD_ARK_WFP_CONNECT_CALLOUT,
            &KSWORD_ARK_FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            L"KswordARK ALE connect filter",
            &Runtime->ConnectFilterId);
    }
    if (NT_SUCCESS(status)) {
        status = KswordARKNetworkAddFilter(
            Runtime,
            &KSWORD_ARK_WFP_RECV_ACCEPT_CALLOUT,
            &KSWORD_ARK_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            L"KswordARK ALE recv-accept filter",
            &Runtime->RecvAcceptFilterId);
    }

    if (NT_SUCCESS(status)) {
        status = FwpmTransactionCommit0(Runtime->EngineHandle);
        if (NT_SUCCESS(status)) {
            Runtime->RuntimeFlags |= KSWORD_ARK_NETWORK_RUNTIME_WFP_STARTED;
            return STATUS_SUCCESS;
        }
    }
    else {
        (VOID)FwpmTransactionAbort0(Runtime->EngineHandle);
    }

    KswordARKNetworkWfpUnregister(Runtime);
    return status;
}

VOID
KswordARKNetworkWfpUnregister(
    _Inout_ KSWORD_ARK_NETWORK_RUNTIME* Runtime
    )
/*++

Routine Description:

    注销 WFP filter、callout 和引擎句柄。中文说明：所有删除操作都按非空/非零判断
    执行，保证初始化失败路径和正常卸载路径都可重复调用。

Arguments:

    Runtime - 网络运行时。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Runtime == NULL) {
        return;
    }

    if (Runtime->EngineHandle != NULL) {
        if (Runtime->ConnectFilterId != 0ULL) {
            (VOID)FwpmFilterDeleteById0(Runtime->EngineHandle, Runtime->ConnectFilterId);
            Runtime->ConnectFilterId = 0ULL;
        }
        if (Runtime->RecvAcceptFilterId != 0ULL) {
            (VOID)FwpmFilterDeleteById0(Runtime->EngineHandle, Runtime->RecvAcceptFilterId);
            Runtime->RecvAcceptFilterId = 0ULL;
        }
        (VOID)FwpmCalloutDeleteByKey0(Runtime->EngineHandle, &KSWORD_ARK_WFP_CONNECT_CALLOUT);
        (VOID)FwpmCalloutDeleteByKey0(Runtime->EngineHandle, &KSWORD_ARK_WFP_RECV_ACCEPT_CALLOUT);
        (VOID)FwpmSubLayerDeleteByKey0(Runtime->EngineHandle, &KSWORD_ARK_WFP_SUBLAYER);
        FwpmEngineClose0(Runtime->EngineHandle);
        Runtime->EngineHandle = NULL;
    }

    if (Runtime->ConnectCalloutId != 0U) {
        (VOID)FwpsCalloutUnregisterById0(Runtime->ConnectCalloutId);
        Runtime->ConnectCalloutId = 0U;
    }
    if (Runtime->RecvAcceptCalloutId != 0U) {
        (VOID)FwpsCalloutUnregisterById0(Runtime->RecvAcceptCalloutId);
        Runtime->RecvAcceptCalloutId = 0U;
    }

    Runtime->RuntimeFlags &= ~(
        KSWORD_ARK_NETWORK_RUNTIME_WFP_REGISTERED |
        KSWORD_ARK_NETWORK_RUNTIME_WFP_STARTED);
}
