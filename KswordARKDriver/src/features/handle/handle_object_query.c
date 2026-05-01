/*++

Module Name:

    handle_object_query.c

Abstract:

    Object type/name and restricted proxy query for PID+handle inputs.

Environment:

    Kernel-mode Driver Framework

--*/

#include "handle_support.h"

#define KSWORD_ARK_HANDLE_POOL_TAG 'hOsK'
#define KSWORD_ARK_OBJECT_PROXY_ALLOWED_ACCESS \
    (READ_CONTROL | SYNCHRONIZE | 0x00000001UL | 0x00000002UL | 0x00000004UL | 0x00000008UL | 0x00000010UL)
#define KSWORD_ARK_OBJECT_PROXY_ALLOWED_TYPE_MASK 0x0000007FUL

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif

#ifndef OBJ_KERNEL_HANDLE
#define OBJ_KERNEL_HANDLE 0x00000200L
#endif

typedef PVOID
(NTAPI* KSWORD_ARK_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

NTKERNELAPI
VOID
KeStackAttachProcess(
    _Inout_ PVOID Process,
    _Out_ PVOID ApcState
    );

NTKERNELAPI
VOID
KeUnstackDetachProcess(
    _In_ PVOID ApcState
    );

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

NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

NTKERNELAPI
NTSTATUS
ObOpenObjectByPointer(
    _In_ PVOID Object,
    _In_ ULONG HandleAttributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PHANDLE Handle
    );

static PVOID
KswordARKHandleAllocateNonPaged(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    Allocate a temporary nonpaged buffer for object-name queries. 中文说明：
    优先使用 ExAllocatePool2；旧 WDK/旧系统回退到 ExAllocatePoolWithTag。

Arguments:

    BufferBytes - Number of bytes to allocate.

Return Value:

    Nonpaged buffer pointer or NULL.

--*/
{
    static volatile LONG allocatorResolved = 0;
    static KSWORD_ARK_EX_ALLOCATE_POOL2_FN exAllocatePool2Fn = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&allocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        exAllocatePool2Fn = (KSWORD_ARK_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (exAllocatePool2Fn != NULL) {
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, KSWORD_ARK_HANDLE_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSWORD_ARK_HANDLE_POOL_TAG);
#pragma warning(pop)
}

static VOID
KswordARKHandleCopyUnicodeStringToFixed(
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _In_opt_ const UNICODE_STRING* Source,
    _Out_opt_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    Copy a counted Unicode string into a fixed protocol buffer. 中文说明：所有
    输出字符串都强制 NUL 结尾，并显式报告截断状态。

Arguments:

    Destination - Fixed WCHAR buffer in the response packet.
    DestinationChars - Capacity of Destination in WCHARs.
    Source - Optional counted source string.
    TruncatedOut - Optional truncation flag.

Return Value:

    None.

--*/
{
    ULONG sourceChars = 0UL;
    ULONG copyChars = 0UL;

    if (TruncatedOut != NULL) {
        *TruncatedOut = FALSE;
    }
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0U) {
        return;
    }

    sourceChars = (ULONG)(Source->Length / sizeof(WCHAR));
    copyChars = sourceChars;
    if (copyChars >= DestinationChars) {
        copyChars = DestinationChars - 1UL;
        if (TruncatedOut != NULL) {
            *TruncatedOut = TRUE;
        }
    }

    RtlCopyMemory(Destination, Source->Buffer, (SIZE_T)copyChars * sizeof(WCHAR));
    Destination[copyChars] = L'\0';
}

NTSTATUS
KswordARKHandleQueryTypeName(
    _In_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    Read OBJECT_TYPE.Name by DynData offset. 中文说明：System Informer 的 OtName
    字段提供 UNICODE_STRING 偏移；缺失时本查询降级失败但不影响基础对象信息。

Arguments:

    ObjectType - Object type pointer returned by ObGetObjectType.
    DynState - Active DynData snapshot.
    Destination - Fixed output buffer.
    DestinationChars - Destination capacity in WCHARs.
    TruncatedOut - Receives truncation flag.

Return Value:

    STATUS_SUCCESS when type name was copied; otherwise a failure status.

--*/
{
    UNICODE_STRING typeName;

    if (TruncatedOut != NULL) {
        *TruncatedOut = FALSE;
    }
    if (ObjectType == NULL || DynState == NULL || Destination == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKHandleIsOffsetPresent(DynState->Kernel.OtName)) {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&typeName, sizeof(typeName));
    __try {
        RtlCopyMemory(&typeName, (PUCHAR)ObjectType + DynState->Kernel.OtName, sizeof(typeName));
        KswordARKHandleCopyUnicodeStringToFixed(Destination, DestinationChars, &typeName, TruncatedOut);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKHandleQueryObjectName(
    _In_ PVOID Object,
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    Query object name with ObQueryNameString. 中文说明：先询问所需长度，再分配
    临时缓冲读取；返回空名称仍视为成功，由 UI 显示 unnamed。

Arguments:

    Object - Referenced object body.
    Destination - Fixed output buffer.
    DestinationChars - Destination capacity in WCHARs.
    TruncatedOut - Receives truncation flag.

Return Value:

    STATUS_SUCCESS when the object-name query completed; otherwise NTSTATUS.

--*/
{
    POBJECT_NAME_INFORMATION nameInfo = NULL;
    ULONG requiredBytes = 0UL;
    ULONG allocationBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (TruncatedOut != NULL) {
        *TruncatedOut = FALSE;
    }
    if (Object == NULL || Destination == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    Destination[0] = L'\0';
    status = ObQueryNameString(Object, NULL, 0, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW) {
        if (NT_SUCCESS(status)) {
            return STATUS_SUCCESS;
        }
        return status;
    }

    allocationBytes = requiredBytes;
    if (allocationBytes < sizeof(OBJECT_NAME_INFORMATION) + sizeof(WCHAR)) {
        allocationBytes = sizeof(OBJECT_NAME_INFORMATION) + sizeof(WCHAR);
    }
    if (allocationBytes > (64UL * 1024UL)) {
        allocationBytes = 64UL * 1024UL;
    }

    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKHandleAllocateNonPaged(allocationBytes);
    if (nameInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(nameInfo, allocationBytes);
    status = ObQueryNameString(Object, nameInfo, allocationBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        KswordARKHandleCopyUnicodeStringToFixed(Destination, DestinationChars, &nameInfo->Name, TruncatedOut);
    }
    ExFreePoolWithTag(nameInfo, KSWORD_ARK_HANDLE_POOL_TAG);

    return status;
}

static BOOLEAN
KswordARKHandleIsProxyTypeAllowed(
    _In_ ULONG ObjectTypeIndex
    )
/*++

Routine Description:

    Apply a conservative first-pass proxy whitelist. 中文说明：第一版仅允许极低
    type index 范围并且仍会降权；未知/过大的类型默认拒绝，不做猜测。

Arguments:

    ObjectTypeIndex - Object type index decoded from OBJECT_TYPE.

Return Value:

    TRUE when the proxy policy may attempt a downgraded open.

--*/
{
    if (ObjectTypeIndex == 0UL || ObjectTypeIndex >= 64UL) {
        return FALSE;
    }

    return ((KSWORD_ARK_OBJECT_PROXY_ALLOWED_TYPE_MASK & (1UL << ObjectTypeIndex)) != 0UL) ? TRUE : FALSE;
}

VOID
KswordARKHandleMaybeOpenProxyHandle(
    _In_ PVOID Object,
    _In_ POBJECT_TYPE ObjectType,
    _In_ ULONG ObjectTypeIndex,
    _Inout_ KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* Response
    )
/*++

Routine Description:

    Optionally open a downgraded kernel proxy handle for whitelisted object
    types. 中文说明：第一版不把任意内核对象转成高权用户句柄；只有策略允许时
    才尝试低权限打开，失败时在响应中保留明确原因。

Arguments:

    Object - Referenced object body.
    ObjectType - Object type pointer.
    ObjectTypeIndex - Decoded object type index.
    Response - Mutable response carrying policy/result fields.

Return Value:

    None.

--*/
{
    ACCESS_MASK desiredAccess = 0;
    HANDLE kernelHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Response == NULL) {
        return;
    }

    Response->proxyStatus = KSWORD_ARK_OBJECT_PROXY_STATUS_NOT_REQUESTED;
    if (Object == NULL || ObjectType == NULL) {
        return;
    }
    if (!KswordARKHandleIsProxyTypeAllowed(ObjectTypeIndex)) {
        Response->proxyStatus = KSWORD_ARK_OBJECT_PROXY_STATUS_DENIED_BY_POLICY;
        return;
    }

    desiredAccess = Response->requestedAccess & KSWORD_ARK_OBJECT_PROXY_ALLOWED_ACCESS;
    if (desiredAccess == 0UL) {
        desiredAccess = READ_CONTROL;
    }
    Response->actualGrantedAccess = desiredAccess;
    Response->proxyPolicyFlags =
        KSWORD_ARK_OBJECT_PROXY_POLICY_DOWNGRADED |
        KSWORD_ARK_OBJECT_PROXY_POLICY_TYPE_WHITELISTED;

    status = ObOpenObjectByPointer(
        Object,
        OBJ_KERNEL_HANDLE,
        NULL,
        desiredAccess,
        ObjectType,
        KernelMode,
        &kernelHandle);
    if (!NT_SUCCESS(status)) {
        Response->proxyStatus = KSWORD_ARK_OBJECT_PROXY_STATUS_OPEN_FAILED;
        Response->proxyNtStatus = status;
        return;
    }

    Response->proxyHandle = (ULONG64)(ULONG_PTR)kernelHandle;
    Response->proxyStatus = KSWORD_ARK_OBJECT_PROXY_STATUS_OPENED;
    Response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_PROXY_HANDLE_PRESENT;

    //
    // 当前协议只展示代理策略是否能打开；不把内核句柄传给 R3 使用。后续若要
    // 返回用户句柄，必须用 ZwDuplicateObject 到请求进程并继续执行降权策略。
    //
    ZwClose(kernelHandle);
    Response->proxyHandle = 0ULL;
    Response->fieldFlags &= ~KSWORD_ARK_OBJECT_INFO_FIELD_PROXY_HANDLE_PRESENT;
}

NTSTATUS
KswordARKDriverQueryHandleObject(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Query object type/name details through a caller-supplied PID + handle value.
    中文说明：函数不接受任意 object address，因此不会把 Phase-4 展示地址升级
    为后续操作凭据。

Arguments:

    OutputBuffer - Fixed response buffer.
    OutputBufferLength - Capacity of OutputBuffer.
    Request - Query request containing target PID and handle value.
    BytesWrittenOut - Receives sizeof(response) on success/failure with packet.

Return Value:

    STATUS_SUCCESS when the response packet is populated, or validation failure.

--*/
{
    KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* response = NULL;
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    PVOID object = NULL;
    POBJECT_TYPE objectType = NULL;
    OBJECT_HANDLE_INFORMATION handleInformation;
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    BOOLEAN attached = FALSE;
    BOOLEAN truncated = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requestFlags = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL || Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->processId == 0UL || Request->handleValue == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_HANDLE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->processId = Request->processId;
    response->handleValue = Request->handleValue;
    response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE;
    response->proxyStatus = KSWORD_ARK_OBJECT_PROXY_STATUS_NOT_REQUESTED;
    response->requestedAccess = Request->requestedAccess;

    RtlZeroMemory(&dynState, sizeof(dynState));
    RtlZeroMemory(&handleInformation, sizeof(handleInformation));
    KswordARKDynDataSnapshot(&dynState);
    KswordARKHandlePrepareObjectDynData(response, &dynState);
    requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL : Request->flags;

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_PROCESS_LOOKUP_FAILED;
        response->objectReferenceStatus = status;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(attachState, sizeof(attachState));
    __try {
        KeStackAttachProcess((PVOID)processObject, attachState);
        attached = TRUE;
        status = ObReferenceObjectByHandle(
            (HANDLE)(ULONG_PTR)Request->handleValue,
            0,
            NULL,
            UserMode,
            &object,
            &handleInformation);
        KeUnstackDetachProcess(attachState);
        attached = FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        if (attached) {
            KeUnstackDetachProcess(attachState);
            attached = FALSE;
        }
    }
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_HANDLE_REFERENCE_FAILED;
        response->objectReferenceStatus = status;
        ObDereferenceObject(processObject);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    response->objectReferenceStatus = STATUS_SUCCESS;
    response->objectAddress = (ULONG64)(ULONG_PTR)object;
    response->actualGrantedAccess = handleInformation.GrantedAccess;
    response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_PRESENT;

    __try {
        objectType = ObGetObjectType(object);
        if (objectType != NULL && KswordARKHandleIsOffsetPresent(dynState.Kernel.OtIndex)) {
            response->objectTypeIndex = (ULONG)(*(PUCHAR)((PUCHAR)objectType + dynState.Kernel.OtIndex));
            response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_TYPE_INDEX_PRESENT;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        response->typeStatus = GetExceptionCode();
        response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_TYPE_QUERY_FAILED;
    }

    if ((requestFlags & KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_TYPE_NAME) != 0UL) {
        truncated = FALSE;
        status = KswordARKHandleQueryTypeName(
            objectType,
            &dynState,
            response->typeName,
            KSWORD_ARK_OBJECT_TYPE_NAME_CHARS,
            &truncated);
        response->typeStatus = status;
        if (NT_SUCCESS(status)) {
            response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_TYPE_NAME_PRESENT;
            if (truncated) {
                response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_TRUNCATED;
            }
        }
        else if (response->queryStatus == KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_TYPE_QUERY_FAILED;
        }
    }

    if ((requestFlags & KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_OBJECT_NAME) != 0UL) {
        truncated = FALSE;
        status = KswordARKHandleQueryObjectName(
            object,
            response->objectName,
            KSWORD_ARK_OBJECT_NAME_CHARS,
            &truncated);
        response->nameStatus = status;
        if (NT_SUCCESS(status)) {
            response->fieldFlags |= KSWORD_ARK_OBJECT_INFO_FIELD_OBJECT_NAME_PRESENT;
            if (truncated) {
                response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_TRUNCATED;
            }
        }
        else if (response->queryStatus == KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_QUERY_FAILED;
        }
    }

    if ((requestFlags & KSWORD_ARK_QUERY_OBJECT_FLAG_REQUEST_PROXY_HANDLE) != 0UL) {
        KswordARKHandleMaybeOpenProxyHandle(object, objectType, response->objectTypeIndex, response);
    }

    if (response->queryStatus == KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE) {
        response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_OK;
    }
    if (response->queryStatus != KSWORD_ARK_OBJECT_QUERY_STATUS_OK &&
        response->queryStatus != KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_TRUNCATED &&
        response->fieldFlags != 0UL) {
        response->queryStatus = KSWORD_ARK_OBJECT_QUERY_STATUS_PARTIAL;
    }

    ObDereferenceObject(object);
    ObDereferenceObject(processObject);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

