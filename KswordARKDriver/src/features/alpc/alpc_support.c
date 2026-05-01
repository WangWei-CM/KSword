/*++

Module Name:

    alpc_support.c

Abstract:

    Shared helper routines for Phase-6 ALPC Port inspection.

Environment:

    Kernel-mode Driver Framework

--*/

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

#define KSWORD_ARK_ALPC_POOL_TAG 'pAsK'

#ifdef _X86_
#define KSWORD_ARK_KERNEL_HANDLE_BIT ((ULONG_PTR)0x80000000UL)
#else
#define KSWORD_ARK_KERNEL_HANDLE_BIT ((ULONG_PTR)0xFFFFFFFF80000000ULL)
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

BOOLEAN
KswordARKAlpcIsOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    判断一个 DynData offset/shift 是否可用。中文说明：System Informer 和
    Ksword 都使用固定哨兵表示字段缺失，这里统一过滤，避免调用点误读
    ALPC_PORT 私有结构。

Arguments:

    Offset - DynData 中的原始字段偏移。

Return Value:

    TRUE 表示可以读取该私有字段；FALSE 表示字段缺失。

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

static ULONG
KswordARKAlpcNormalizeOffset(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    把内部 DynData 缺失哨兵转换成 ALPC 协议哨兵。中文说明：UI 只把这些
    offset 用作诊断展示，不允许后续把 offset 或 object address 当凭据。

Arguments:

    Offset - 原始 DynData offset。

Return Value:

    可展示 offset，或 KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE。

--*/
{
    if (!KswordARKAlpcIsOffsetPresent(Offset)) {
        return KSWORD_ARK_ALPC_OFFSET_UNAVAILABLE;
    }

    return Offset;
}

BOOLEAN
KswordARKAlpcHasRequiredDynData(
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    检查 Phase-6 ALPC 查询依赖字段。中文说明：ALPC 关系读取会触碰
    communicationInfo、handleTable 和 port lock；任何字段缺失都 fail closed。

Arguments:

    DynState - IOCTL 入口处截取的 DynData 快照。

Return Value:

    TRUE 表示 ALPC capability 完整；FALSE 表示不可执行私有字段读取。

--*/
{
    if (DynState == NULL) {
        return FALSE;
    }

    return ((DynState->CapabilityMask & KSW_CAP_ALPC_FIELDS) == KSW_CAP_ALPC_FIELDS) ? TRUE : FALSE;
}

VOID
KswordARKAlpcPrepareOffsets(
    _Inout_ KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE* Response,
    _In_ const KSW_DYN_STATE* DynState
    )
/*++

Routine Description:

    复制 ALPC DynData 诊断字段到响应包。中文说明：这些字段用于解释查询为何
    可用/不可用，不参与 R3 后续操作。

Arguments:

    Response - 可写响应包。
    DynState - DynData 快照。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Response == NULL || DynState == NULL) {
        return;
    }

    Response->dynDataCapabilityMask = DynState->CapabilityMask;
    Response->alpcCommunicationInfoOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcCommunicationInfo);
    Response->alpcOwnerProcessOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcOwnerProcess);
    Response->alpcConnectionPortOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcConnectionPort);
    Response->alpcServerCommunicationPortOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcServerCommunicationPort);
    Response->alpcClientCommunicationPortOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcClientCommunicationPort);
    Response->alpcHandleTableOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcHandleTable);
    Response->alpcHandleTableLockOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcHandleTableLock);
    Response->alpcAttributesOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcAttributes);
    Response->alpcAttributesFlagsOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcAttributesFlags);
    Response->alpcPortContextOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcPortContext);
    Response->alpcPortObjectLockOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcPortObjectLock);
    Response->alpcSequenceNoOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcSequenceNo);
    Response->alpcStateOffset = KswordARKAlpcNormalizeOffset(DynState->Kernel.AlpcState);
}

static PVOID
KswordARKAlpcAllocateNonPaged(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    分配临时 NonPagedPool 缓冲区。中文说明：对象名查询可能在旧系统上没有
    ExAllocatePool2，因此保持和现有 handle 查询一致的兼容分配策略。

Arguments:

    BufferBytes - 需要分配的字节数。

Return Value:

    成功时返回缓冲区指针，失败时返回 NULL。

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
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, KSWORD_ARK_ALPC_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSWORD_ARK_ALPC_POOL_TAG);
#pragma warning(pop)
}

static VOID
KswordARKAlpcCopyUnicodeStringToFixed(
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _In_opt_ const UNICODE_STRING* Source,
    _Out_opt_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    把 UNICODE_STRING 复制到固定协议缓冲。中文说明：所有 R3 可见字符串都
    强制 NUL 结尾，并通过 TruncatedOut 说明是否被截断。

Arguments:

    Destination - 目标 WCHAR 数组。
    DestinationChars - 目标数组容量，以 WCHAR 为单位。
    Source - 可选源字符串。
    TruncatedOut - 可选截断标记输出。

Return Value:

    None. 本函数没有返回值。

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

static NTSTATUS
KswordARKAlpcQueryObjectName(
    _In_ PVOID Object,
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    使用 ObQueryNameString 查询对象名。中文说明：ALPC Port 可能无名称，空名
    仍视为成功；只有 API 失败或缓冲分配失败才返回错误。

Arguments:

    Object - 已引用对象。
    Destination - 固定输出缓冲。
    DestinationChars - 输出缓冲容量。
    TruncatedOut - 接收是否截断。

Return Value:

    STATUS_SUCCESS 或对象名查询 NTSTATUS。

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
        return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
    }

    allocationBytes = requiredBytes;
    if (allocationBytes < sizeof(OBJECT_NAME_INFORMATION) + sizeof(WCHAR)) {
        allocationBytes = sizeof(OBJECT_NAME_INFORMATION) + sizeof(WCHAR);
    }
    if (allocationBytes > (64UL * 1024UL)) {
        allocationBytes = 64UL * 1024UL;
    }

    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKAlpcAllocateNonPaged(allocationBytes);
    if (nameInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(nameInfo, allocationBytes);
    status = ObQueryNameString(Object, nameInfo, allocationBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        KswordARKAlpcCopyUnicodeStringToFixed(Destination, DestinationChars, &nameInfo->Name, TruncatedOut);
    }

    ExFreePoolWithTag(nameInfo, KSWORD_ARK_ALPC_POOL_TAG);
    return status;
}

NTSTATUS
KswordARKAlpcQueryTypeName(
    _In_ POBJECT_TYPE ObjectType,
    _In_ const KSW_DYN_STATE* DynState,
    _Out_writes_(DestinationChars) WCHAR* Destination,
    _In_ ULONG DestinationChars,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    根据 OBJECT_TYPE.Name DynData 偏移读取类型名。中文说明：用于验证传入
    handle 是否确实是 ALPC/Port，避免把任意对象解释成 ALPC_PORT。

Arguments:

    ObjectType - ObGetObjectType 返回的类型对象。
    DynState - DynData 快照。
    Destination - 固定输出缓冲。
    DestinationChars - 输出缓冲容量。
    TruncatedOut - 接收是否截断。

Return Value:

    STATUS_SUCCESS 表示已复制类型名；否则返回失败状态。

--*/
{
    UNICODE_STRING typeName;

    if (TruncatedOut != NULL) {
        *TruncatedOut = FALSE;
    }
    if (ObjectType == NULL || DynState == NULL || Destination == NULL || DestinationChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKAlpcIsOffsetPresent(DynState->Kernel.OtName)) {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&typeName, sizeof(typeName));
    __try {
        RtlCopyMemory(&typeName, (PUCHAR)ObjectType + DynState->Kernel.OtName, sizeof(typeName));
        KswordARKAlpcCopyUnicodeStringToFixed(Destination, DestinationChars, &typeName, TruncatedOut);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

BOOLEAN
KswordARKAlpcIsTypeNameAlpcPort(
    _In_reads_(TypeNameChars) const WCHAR* TypeName,
    _In_ ULONG TypeNameChars
    )
/*++

Routine Description:

    检查对象类型名是否表示 ALPC Port。中文说明：Windows 版本间可能显示为
    "ALPC Port" 或 "Port"，这里仅接受这两个已知类型，其他类型 fail closed。

Arguments:

    TypeName - 固定类型名缓冲。
    TypeNameChars - 类型名缓冲容量。

Return Value:

    TRUE 表示允许继续读取 ALPC 私有字段；FALSE 表示类型不匹配。

--*/
{
    UNICODE_STRING actualName;
    UNICODE_STRING alpcPortName;
    UNICODE_STRING portName;
    ULONG actualChars = 0UL;

    if (TypeName == NULL || TypeNameChars == 0UL || TypeName[0] == L'\0') {
        return FALSE;
    }

    while (actualChars < TypeNameChars && TypeName[actualChars] != L'\0') {
        actualChars += 1UL;
    }

    actualName.Buffer = (PWCHAR)TypeName;
    actualName.Length = (USHORT)(actualChars * sizeof(WCHAR));
    actualName.MaximumLength = actualName.Length;
    RtlInitUnicodeString(&alpcPortName, L"ALPC Port");
    RtlInitUnicodeString(&portName, L"Port");

    if (RtlEqualUnicodeString(&actualName, &alpcPortName, TRUE)) {
        return TRUE;
    }
    if (RtlEqualUnicodeString(&actualName, &portName, TRUE)) {
        return TRUE;
    }

    return FALSE;
}

static HANDLE
KswordARKAlpcMakeKernelHandle(
    _In_ HANDLE HandleValue
    )
/*++

Routine Description:

    把 System 进程中的句柄值转换成内核句柄格式。中文说明：逻辑对齐
    System Informer 的 MakeKernelHandle，只用于 PsInitialSystemProcess 特例。

Arguments:

    HandleValue - 用户请求中的原始句柄值。

Return Value:

    标记了 kernel handle bit 的 HANDLE。

--*/
{
    return (HANDLE)((ULONG_PTR)HandleValue | KSWORD_ARK_KERNEL_HANDLE_BIT);
}

NTSTATUS
KswordARKAlpcReferenceHandleObject(
    _In_ PEPROCESS ProcessObject,
    _In_ ULONG64 HandleValue,
    _Outptr_result_nullonfailure_ PVOID* ObjectOut
    )
/*++

Routine Description:

    在目标进程上下文中引用 PID+Handle 指向的对象。中文说明：函数不接收
    object address，因此不会把 Phase-4 展示地址升级为内核对象凭据。

Arguments:

    ProcessObject - 已引用目标进程对象。
    HandleValue - 目标进程内的句柄值。
    ObjectOut - 接收引用后的对象；调用者必须 ObDereferenceObject。

Return Value:

    STATUS_SUCCESS 或 ObReferenceObjectByHandle 的错误状态。

--*/
{
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    HANDLE targetHandle = (HANDLE)(ULONG_PTR)HandleValue;
    BOOLEAN attached = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObject == NULL || ObjectOut == NULL || HandleValue == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ObjectOut = NULL;

    if (ProcessObject == PsInitialSystemProcess) {
        targetHandle = KswordARKAlpcMakeKernelHandle(targetHandle);
    }

    RtlZeroMemory(attachState, sizeof(attachState));
    __try {
        KeStackAttachProcess((PVOID)ProcessObject, attachState);
        attached = TRUE;
        status = ObReferenceObjectByHandle(
            targetHandle,
            0,
            NULL,
            (ProcessObject == PsInitialSystemProcess) ? KernelMode : UserMode,
            ObjectOut,
            NULL);
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

    return status;
}

NTSTATUS
KswordARKAlpcReadPointerField(
    _In_ PVOID Object,
    _In_ ULONG Offset,
    _Outptr_result_maybenull_ PVOID* PointerOut
    )
/*++

Routine Description:

    安全读取 ALPC 私有结构中的指针字段。中文说明：所有私有字段读取都包裹
    SEH，目标对象退出/结构异常时返回异常码而不是崩溃。

Arguments:

    Object - ALPC_PORT 或 ALPC_COMMUNICATION_INFO 基址。
    Offset - DynData 字段偏移。
    PointerOut - 接收读取出的指针。

Return Value:

    STATUS_SUCCESS 或异常 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID value = NULL;

    if (Object == NULL || PointerOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKAlpcIsOffsetPresent(Offset)) {
        return STATUS_NOT_SUPPORTED;
    }

    __try {
        RtlCopyMemory(&value, (PUCHAR)Object + Offset, sizeof(value));
        *PointerOut = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

static NTSTATUS
KswordARKAlpcReadUlongField(
    _In_ PVOID Object,
    _In_ ULONG Offset,
    _Out_ ULONG* ValueOut
    )
/*++

Routine Description:

    安全读取 ALPC 私有结构中的 ULONG 字段。中文说明：Flags、SequenceNo、
    State 都按 ULONG 输出到共享协议。

Arguments:

    Object - 结构基址。
    Offset - DynData 字段偏移。
    ValueOut - 接收 ULONG 值。

Return Value:

    STATUS_SUCCESS 或异常 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Object == NULL || ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKAlpcIsOffsetPresent(Offset)) {
        return STATUS_NOT_SUPPORTED;
    }

    __try {
        RtlCopyMemory(ValueOut, (PUCHAR)Object + Offset, sizeof(*ValueOut));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    return status;
}

NTSTATUS
KswordARKAlpcPopulateBasicInfo(
    _In_ PVOID PortObject,
    _In_ const KSW_DYN_STATE* DynState,
    _Inout_ KSWORD_ARK_ALPC_PORT_INFO* PortInfo
    )
/*++

Routine Description:

    读取一个 ALPC_PORT 的 owner/flags/context/sequence/state。中文说明：
    OwnerProcess 带有效位语义，必须在 PortObjectLock 共享锁保护下读取。

Arguments:

    PortObject - 已引用或当前稳定的 ALPC Port 对象。
    DynState - DynData 快照。
    PortInfo - 可写端口信息包。

Return Value:

    STATUS_SUCCESS 表示基础字段读取完成；否则返回失败状态。

--*/
{
    PVOID ownerProcessRaw = NULL;
    PEPROCESS ownerProcess = NULL;
    PEX_PUSH_LOCK portObjectLock = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS fieldStatus = STATUS_SUCCESS;
    ULONG flagsValue = 0UL;
    ULONG stateValue = 0UL;
    ULONG sequenceValue = 0UL;
    PVOID pointerValue = NULL;
    BOOLEAN criticalRegionEntered = FALSE;
    BOOLEAN lockHeld = FALSE;

    if (PortObject == NULL || DynState == NULL || PortInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcOwnerProcess) ||
        !KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcPortObjectLock)) {
        return STATUS_NOT_SUPPORTED;
    }

    PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_OBJECT_PRESENT;
    PortInfo->objectAddress = (ULONG64)(ULONG_PTR)PortObject;
    portObjectLock = (PEX_PUSH_LOCK)((PUCHAR)PortObject + DynState->Kernel.AlpcPortObjectLock);

    __try {
        KeEnterCriticalRegion();
        criticalRegionEntered = TRUE;
        ExAcquirePushLockShared(portObjectLock);
        lockHeld = TRUE;
        RtlCopyMemory(&ownerProcessRaw, (PUCHAR)PortObject + DynState->Kernel.AlpcOwnerProcess, sizeof(ownerProcessRaw));
        ExReleasePushLockShared(portObjectLock);
        lockHeld = FALSE;
        KeLeaveCriticalRegion();
        criticalRegionEntered = FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        if (lockHeld) {
            ExReleasePushLockShared(portObjectLock);
            lockHeld = FALSE;
        }
        if (criticalRegionEntered) {
            KeLeaveCriticalRegion();
            criticalRegionEntered = FALSE;
        }
        return status;
    }

    if (ownerProcessRaw != NULL && (((ULONG_PTR)ownerProcessRaw & 1ULL) == 0ULL)) {
        ownerProcess = (PEPROCESS)ownerProcessRaw;
        PortInfo->ownerProcessId = HandleToULong(PsGetProcessId(ownerProcess));
        PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_OWNER_PID_PRESENT;
    }

    if (KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcAttributes) &&
        KswordARKAlpcIsOffsetPresent(DynState->Kernel.AlpcAttributesFlags)) {
        fieldStatus = KswordARKAlpcReadUlongField(
            (PUCHAR)PortObject + DynState->Kernel.AlpcAttributes,
            DynState->Kernel.AlpcAttributesFlags,
            &flagsValue);
        if (NT_SUCCESS(fieldStatus)) {
            PortInfo->flags = flagsValue;
            PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_FLAGS_PRESENT;
        }
        else if (NT_SUCCESS(status)) {
            status = fieldStatus;
        }
    }

    fieldStatus = KswordARKAlpcReadPointerField(PortObject, DynState->Kernel.AlpcPortContext, &pointerValue);
    if (NT_SUCCESS(fieldStatus)) {
        PortInfo->portContext = (ULONG64)(ULONG_PTR)pointerValue;
        PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_CONTEXT_PRESENT;
    }
    else if (fieldStatus != STATUS_NOT_SUPPORTED && NT_SUCCESS(status)) {
        status = fieldStatus;
    }

    fieldStatus = KswordARKAlpcReadUlongField(PortObject, DynState->Kernel.AlpcSequenceNo, &sequenceValue);
    if (NT_SUCCESS(fieldStatus)) {
        PortInfo->sequenceNo = sequenceValue;
        PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_SEQUENCE_PRESENT;
    }
    else if (fieldStatus != STATUS_NOT_SUPPORTED && NT_SUCCESS(status)) {
        status = fieldStatus;
    }

    fieldStatus = KswordARKAlpcReadUlongField(PortObject, DynState->Kernel.AlpcState, &stateValue);
    if (NT_SUCCESS(fieldStatus)) {
        PortInfo->state = stateValue;
        PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_STATE_PRESENT;
    }
    else if (fieldStatus != STATUS_NOT_SUPPORTED && NT_SUCCESS(status)) {
        status = fieldStatus;
    }

    return status;
}

NTSTATUS
KswordARKAlpcPopulateNameInfo(
    _In_ PVOID PortObject,
    _Inout_ KSWORD_ARK_ALPC_PORT_INFO* PortInfo,
    _Out_ BOOLEAN* TruncatedOut
    )
/*++

Routine Description:

    查询并填充一个 ALPC Port 的对象名。中文说明：名称查询失败不会影响对象
    关系字段展示，但会在 nameStatus 中保留原始 NTSTATUS。

Arguments:

    PortObject - 已引用或当前稳定的 ALPC Port 对象。
    PortInfo - 可写端口信息。
    TruncatedOut - 接收是否截断。

Return Value:

    STATUS_SUCCESS 或 ObQueryNameString 相关错误。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (TruncatedOut != NULL) {
        *TruncatedOut = FALSE;
    }
    if (PortObject == NULL || PortInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKAlpcQueryObjectName(
        PortObject,
        PortInfo->portName,
        KSWORD_ARK_ALPC_PORT_NAME_CHARS,
        TruncatedOut);
    PortInfo->nameStatus = status;
    if (NT_SUCCESS(status)) {
        PortInfo->fieldFlags |= KSWORD_ARK_ALPC_PORT_FIELD_NAME_PRESENT;
    }

    return status;
}

NTSTATUS
KswordARKAlpcValidatePortPointer(
    _In_ PVOID CandidatePort,
    _In_ POBJECT_TYPE ExpectedType
    )
/*++

Routine Description:

    验证 communicationInfo 中的端口指针类型。中文说明：System Informer 使用
    AlpcPortObjectType 直接比对；这里使用已验证 handle 的 object type 作为
    同类型锚点，避免引入额外未公开导出。

Arguments:

    CandidatePort - communicationInfo 中读取的候选端口对象。
    ExpectedType - 查询 handle 的对象类型。

Return Value:

    STATUS_SUCCESS 表示类型匹配；否则返回类型错误。

--*/
{
    POBJECT_TYPE candidateType = NULL;

    if (CandidatePort == NULL || ExpectedType == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        candidateType = ObGetObjectType(CandidatePort);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    if (candidateType != ExpectedType) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    return STATUS_SUCCESS;
}

