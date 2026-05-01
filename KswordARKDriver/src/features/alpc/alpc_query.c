/*++

Module Name:

    alpc_query.c

Abstract:

    Phase-6 ALPC Port inspection using DynData-gated private fields.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_alpc.h"

#include "ark/ark_dyndata.h"
#include "alpc_support.h"

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTKERNELAPI
POBJECT_TYPE
NTAPI
ObGetObjectType(
    _In_ PVOID Object
    );

static NTSTATUS
KswordARKAlpcReferenceCommunicationPorts(
    _In_ PVOID PortObject,
    _In_ POBJECT_TYPE ExpectedType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_ KSWORD_ARK_ALPC_REFERENCED_PORTS* PortsOut
    )
/*++

Routine Description:

    引用 ALPC communicationInfo 中的 connection/server/client 端口。中文说明：
    对齐 System Informer 的锁模型：读取 communicationInfo handle table 时持有
    AlpcHandleTableLock 共享锁，确认对象类型后 ObReferenceObject。

Arguments:

    PortObject - 已验证的 ALPC Port 对象。
    ExpectedType - 该 ALPC Port 的对象类型。
    DynState - DynData 快照。
    PortsOut - 接收引用后的三个端口对象；调用方负责释放。

Return Value:

    STATUS_SUCCESS、STATUS_NOT_FOUND 或类型/读取错误。

--*/
{
    PVOID communicationInfo = NULL;
    PVOID handleTable = NULL;
    PEX_PUSH_LOCK handleTableLock = NULL;
    PVOID connectionPort = NULL;
    PVOID serverPort = NULL;
    PVOID clientPort = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN lockHeld = FALSE;
    BOOLEAN criticalRegionEntered = FALSE;

    if (PortObject == NULL || ExpectedType == NULL || DynState == NULL || PortsOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(PortsOut, sizeof(*PortsOut));

    status = KswordARKAlpcReadPointerField(PortObject, DynState->Kernel.AlpcCommunicationInfo, &communicationInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (communicationInfo == NULL) {
        return STATUS_NOT_FOUND;
    }

    handleTable = (PUCHAR)communicationInfo + DynState->Kernel.AlpcHandleTable;
    handleTableLock = (PEX_PUSH_LOCK)((PUCHAR)handleTable + DynState->Kernel.AlpcHandleTableLock);

    __try {
        KeEnterCriticalRegion();
        criticalRegionEntered = TRUE;
        ExAcquirePushLockShared(handleTableLock);
        lockHeld = TRUE;

        if (KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcConnectionPort)) {
            RtlCopyMemory(&connectionPort, (PUCHAR)communicationInfo + DynState->Kernel.AlpcConnectionPort, sizeof(connectionPort));
            if (connectionPort != NULL) {
                status = KswordARKAlpcValidatePortPointer(connectionPort, ExpectedType);
                if (!NT_SUCCESS(status)) {
                    connectionPort = NULL;
                }
                else {
                    ObReferenceObject(connectionPort);
                }
            }
        }

        if (NT_SUCCESS(status) && KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcServerCommunicationPort)) {
            RtlCopyMemory(&serverPort, (PUCHAR)communicationInfo + DynState->Kernel.AlpcServerCommunicationPort, sizeof(serverPort));
            if (serverPort != NULL) {
                status = KswordARKAlpcValidatePortPointer(serverPort, ExpectedType);
                if (!NT_SUCCESS(status)) {
                    serverPort = NULL;
                }
                else {
                    ObReferenceObject(serverPort);
                }
            }
        }

        if (NT_SUCCESS(status) && KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcClientCommunicationPort)) {
            RtlCopyMemory(&clientPort, (PUCHAR)communicationInfo + DynState->Kernel.AlpcClientCommunicationPort, sizeof(clientPort));
            if (clientPort != NULL) {
                status = KswordARKAlpcValidatePortPointer(clientPort, ExpectedType);
                if (!NT_SUCCESS(status)) {
                    clientPort = NULL;
                }
                else {
                    ObReferenceObject(clientPort);
                }
            }
        }

        ExReleasePushLockShared(handleTableLock);
        lockHeld = FALSE;
        KeLeaveCriticalRegion();
        criticalRegionEntered = FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        if (lockHeld) {
            ExReleasePushLockShared(handleTableLock);
            lockHeld = FALSE;
        }
        if (criticalRegionEntered) {
            KeLeaveCriticalRegion();
            criticalRegionEntered = FALSE;
        }
    }

    if (!NT_SUCCESS(status)) {
        if (connectionPort != NULL) {
            ObDereferenceObject(connectionPort);
        }
        if (serverPort != NULL) {
            ObDereferenceObject(serverPort);
        }
        if (clientPort != NULL) {
            ObDereferenceObject(clientPort);
        }
        return status;
    }

    PortsOut->ConnectionPort = connectionPort;
    PortsOut->ServerPort = serverPort;
    PortsOut->ClientPort = clientPort;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKAlpcPopulatePortInfo(
    _In_ PVOID PortObject,
    _In_ ULONG Relation,
    _In_ ULONG RequestFlags,
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_ALPC_PORT_INFO* PortInfo,
    _Out_ BOOLEAN* NameTruncatedOut
    )
/*++

Routine Description:

    填充单个 ALPC 端口的信息包。中文说明：基础字段和名称字段分开受 flags
    控制，避免 UI 只需要关系对象时强制执行可能较慢的名称查询。

Arguments:

    PortObject - ALPC Port 对象。
    Relation - query/connection/server/client 关系枚举。
    RequestFlags - 用户请求 flags。
    DynState - DynData 快照。
    PortInfo - 可写端口信息。
    NameTruncatedOut - 接收名称是否截断。

Return Value:

    STATUS_SUCCESS 或第一个关键失败状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS fieldStatus = STATUS_SUCCESS;

    if (NameTruncatedOut != NULL) {
        *NameTruncatedOut = FALSE;
    }
    if (PortObject == NULL || DynState == NULL || PortInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    PortInfo->relation = Relation;
    PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_OBJECT_PRESENT;
    PortInfo->objectAddress = (ULONG64)(ULONG_PTR)PortObject;

    if ((RequestFlags & KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_BASIC) != 0UL) {
        fieldStatus = KswordARKAlpcPopulateBasicInfo(PortObject, DynState, PortInfo);
        PortInfo->basicStatus = fieldStatus;
        if (!NT_SUCCESS(fieldStatus)) {
            status = fieldStatus;
        }
    }

    if ((RequestFlags & KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_NAMES) != 0UL) {
        fieldStatus = KswordARKAlpcPopulateNameInfo(PortObject, PortInfo, NameTruncatedOut);
        if (!NT_SUCCESS(fieldStatus) && NT_SUCCESS(status)) {
            status = fieldStatus;
        }
    }

    return status;
}

static VOID
KswordARKAlpcReleaseReferencedPorts(
    _Inout_ KSWORD_ARK_ALPC_REFERENCED_PORTS* Ports
    )
/*++

Routine Description:

    释放 communication port 引用。中文说明：集中释放避免多个退出路径重复
    写 ObDereferenceObject，并把指针清零防止误用。

Arguments:

    Ports - 端口引用集合。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Ports == NULL) {
        return;
    }

    if (Ports->ConnectionPort != NULL) {
        ObDereferenceObject(Ports->ConnectionPort);
        Ports->ConnectionPort = NULL;
    }
    if (Ports->ServerPort != NULL) {
        ObDereferenceObject(Ports->ServerPort);
        Ports->ServerPort = NULL;
    }
    if (Ports->ClientPort != NULL) {
        ObDereferenceObject(Ports->ClientPort);
        Ports->ClientPort = NULL;
    }
}

NTSTATUS
KswordARKDriverQueryAlpcPort(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_ALPC_PORT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询一个 PID+Handle 指向的 ALPC Port，并返回 owner/state/flags 以及
    connection/server/client 关系。中文说明：本函数不接受 object address，
    ALPC 私有字段读取全部受 KSW_CAP_ALPC_FIELDS 门控。

Arguments:

    OutputBuffer - 固定响应包输出缓冲。
    OutputBufferLength - 输出缓冲容量。
    Request - 查询请求，包含 PID 和目标 handle。
    BytesWrittenOut - 接收写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应包已填充；校验失败返回对应 NTSTATUS。

--*/
{
    KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE* response = NULL;
    KSW_DYN_STATE dynState;
    KSWORD_ARK_ALPC_REFERENCED_PORTS referencedPorts;
    PEPROCESS processObject = NULL;
    PVOID portObject = NULL;
    POBJECT_TYPE portObjectType = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS fieldStatus = STATUS_SUCCESS;
    ULONG requestFlags = 0UL;
    BOOLEAN truncated = FALSE;
    BOOLEAN anyTruncated = FALSE;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->processId == 0UL || Request->handleValue == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&dynState, sizeof(dynState));
    RtlZeroMemory(&referencedPorts, sizeof(referencedPorts));

    response = (KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_ALPC_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->processId = Request->processId;
    response->handleValue = Request->handleValue;
    response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE;
    response->queryPort.relation = KSWORD_ARK_ALPC_PORT_RELATION_QUERY;
    response->connectionPort.relation = KSWORD_ARK_ALPC_PORT_RELATION_CONNECTION;
    response->serverPort.relation = KSWORD_ARK_ALPC_PORT_RELATION_SERVER;
    response->clientPort.relation = KSWORD_ARK_ALPC_PORT_RELATION_CLIENT;

    KswordARKDynDataSnapshot(&dynState);
    KswordARKAlpcPrepareOffsets(response, &dynState);
    requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_ALL : Request->flags;

    if (!KswordARKAlpcHasRequiredDynData(&dynState)) {
        response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_DYNDATA_MISSING;
        response->objectReferenceStatus = STATUS_NOT_SUPPORTED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_PROCESS_LOOKUP_FAILED;
        response->objectReferenceStatus = status;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    status = KswordARKAlpcReferenceHandleObject(processObject, Request->handleValue, &portObject);
    response->objectReferenceStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_HANDLE_REFERENCE_FAILED;
        ObDereferenceObject(processObject);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    response->fieldFlags |= KSWORD_ARK_ALPC_RESPONSE_FIELD_OBJECT_PRESENT;
    response->queryPort.objectAddress = (ULONG64)(ULONG_PTR)portObject;
    response->queryPort.fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_OBJECT_PRESENT;

    __try {
        portObjectType = ObGetObjectType(portObject);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        response->typeStatus = GetExceptionCode();
        response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_TYPE_MISMATCH;
        ObDereferenceObject(portObject);
        ObDereferenceObject(processObject);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    fieldStatus = KswordARKAlpcQueryTypeName(
        portObjectType,
        &dynState,
        response->typeName,
        KSWORD_ARK_ALPC_TYPE_NAME_CHARS,
        &truncated);
    response->typeStatus = fieldStatus;
    if (NT_SUCCESS(fieldStatus)) {
        response->fieldFlags |= KSWORD_ARK_ALPC_RESPONSE_FIELD_TYPE_NAME_PRESENT;
        anyTruncated = truncated ? TRUE : anyTruncated;
    }
    if (!NT_SUCCESS(fieldStatus) || !KswordARKAlpcIsTypeNameAlpcPort(response->typeName, KSWORD_ARK_ALPC_TYPE_NAME_CHARS)) {
        response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_TYPE_MISMATCH;
        ObDereferenceObject(portObject);
        ObDereferenceObject(processObject);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    if ((requestFlags & (KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_BASIC | KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_NAMES)) != 0UL) {
        truncated = FALSE;
        fieldStatus = KswordARKAlpcPopulatePortInfo(
            portObject,
            KSWORD_ARK_ALPC_PORT_RELATION_QUERY,
            requestFlags,
            &dynState,
            &response->queryPort,
            &truncated);
        response->basicStatus = response->queryPort.basicStatus;
        response->nameStatus = response->queryPort.nameStatus;
        response->fieldFlags |= KSWORD_ARK_ALPC_RESPONSE_FIELD_QUERY_PORT_PRESENT;
        if (truncated) {
            anyTruncated = TRUE;
        }
        if (!NT_SUCCESS(fieldStatus) && response->queryStatus == KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE) {
            response->queryStatus = ((requestFlags & KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_BASIC) != 0UL) ?
                KSWORD_ARK_ALPC_QUERY_STATUS_BASIC_QUERY_FAILED :
                KSWORD_ARK_ALPC_QUERY_STATUS_NAME_QUERY_FAILED;
        }
    }

    if ((requestFlags & KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_COMMUNICATION) != 0UL) {
        fieldStatus = KswordARKAlpcReferenceCommunicationPorts(portObject, portObjectType, &dynState, &referencedPorts);
        response->communicationStatus = fieldStatus;
        if (NT_SUCCESS(fieldStatus)) {
            if (referencedPorts.ConnectionPort != NULL) {
                truncated = FALSE;
                status = KswordARKAlpcPopulatePortInfo(
                    referencedPorts.ConnectionPort,
                    KSWORD_ARK_ALPC_PORT_RELATION_CONNECTION,
                    requestFlags,
                    &dynState,
                    &response->connectionPort,
                    &truncated);
                response->fieldFlags |= KSWORD_ARK_ALPC_RESPONSE_FIELD_CONNECTION_PRESENT;
                anyTruncated = truncated ? TRUE : anyTruncated;
                if (!NT_SUCCESS(status) && response->queryStatus == KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE) {
                    response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL;
                }
            }

            if (referencedPorts.ServerPort != NULL) {
                truncated = FALSE;
                status = KswordARKAlpcPopulatePortInfo(
                    referencedPorts.ServerPort,
                    KSWORD_ARK_ALPC_PORT_RELATION_SERVER,
                    requestFlags,
                    &dynState,
                    &response->serverPort,
                    &truncated);
                response->fieldFlags |= KSWORD_ARK_ALPC_RESPONSE_FIELD_SERVER_PRESENT;
                anyTruncated = truncated ? TRUE : anyTruncated;
                if (!NT_SUCCESS(status) && response->queryStatus == KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE) {
                    response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL;
                }
            }

            if (referencedPorts.ClientPort != NULL) {
                truncated = FALSE;
                status = KswordARKAlpcPopulatePortInfo(
                    referencedPorts.ClientPort,
                    KSWORD_ARK_ALPC_PORT_RELATION_CLIENT,
                    requestFlags,
                    &dynState,
                    &response->clientPort,
                    &truncated);
                response->fieldFlags |= KSWORD_ARK_ALPC_RESPONSE_FIELD_CLIENT_PRESENT;
                anyTruncated = truncated ? TRUE : anyTruncated;
                if (!NT_SUCCESS(status) && response->queryStatus == KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE) {
                    response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL;
                }
            }
        }
        else if (fieldStatus != STATUS_NOT_FOUND && response->queryStatus == KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_COMMUNICATION_FAILED;
        }
    }

    if (response->queryStatus == KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE) {
        response->queryStatus = anyTruncated ?
            KSWORD_ARK_ALPC_QUERY_STATUS_NAME_TRUNCATED :
            KSWORD_ARK_ALPC_QUERY_STATUS_OK;
    }
    else if (response->queryStatus != KSWORD_ARK_ALPC_QUERY_STATUS_OK &&
        response->queryStatus != KSWORD_ARK_ALPC_QUERY_STATUS_NAME_TRUNCATED &&
        response->fieldFlags != 0UL) {
        response->queryStatus = KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL;
    }

    KswordARKAlpcReleaseReferencedPorts(&referencedPorts);
    ObDereferenceObject(portObject);
    ObDereferenceObject(processObject);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
