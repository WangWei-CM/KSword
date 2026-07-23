/*++

Module Name:

    ioctl_registry_query.c

Abstract:

    Read-only export of the KswordARK static IOCTL registry.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_registry.h"
#include "../../dispatch/ioctl_validation.h"

// 中文说明：把静态注册表名称安全复制到固定协议字段。
static VOID
KswordARKCopyIoctlRegistryName(
    _Out_writes_(DestinationChars) PCHAR Destination,
    _In_ SIZE_T DestinationChars,
    _In_opt_z_ PCSTR Source
    )
{
    // 中文说明：使用独立索引，确保所有出口都保留结尾零字符。
    SIZE_T characterIndex = 0U;

    // 中文说明：无效目标缓冲不能写入，直接返回。
    if (Destination == NULL || DestinationChars == 0U) {
        return;
    }

    // 中文说明：先清空首字符，使 NULL 来源得到空字符串。
    Destination[0] = '\0';

    // 中文说明：空来源已经由上一步表示为空字符串。
    if (Source == NULL) {
        return;
    }

    // 中文说明：最多复制 DestinationChars - 1 个字符，避免越界。
    while ((characterIndex + 1U) < DestinationChars && Source[characterIndex] != '\0') {
        Destination[characterIndex] = Source[characterIndex];
        characterIndex += 1U;
    }

    // 中文说明：始终写入结尾零字符，便于 R3 固定长度解析。
    Destination[characterIndex] = '\0';
}

// 中文说明：处理只读 IOCTL registry 查询，业务数据直接来自统一 dispatch 注册表。
NTSTATUS
KswordARKKernelIoctlQueryIoctlRegistry(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    中文说明：验证请求版本和输出预算，返回控制码、名称、能力、flags 与 handler 地址。

Arguments:

    Device - WDF 设备对象；本查询不修改设备状态。
    Request - 当前 WDF 请求。
    InputBufferLength - R3 提供的输入长度。
    OutputBufferLength - R3 提供的输出长度。
    BytesReturned - 实际返回字节数。

Return Value:

    STATUS_SUCCESS 或参数、缓冲区验证错误。

--*/
{
    // 中文说明：requestPointer 指向 METHOD_BUFFERED 输入，请在清空输出前复制。
    KSWORD_ARK_QUERY_IOCTL_REGISTRY_REQUEST* requestPointer = NULL;
    // 中文说明：requestCopy 保存经验证的固定输入，避免输入输出共用 SystemBuffer。
    KSWORD_ARK_QUERY_IOCTL_REGISTRY_REQUEST requestCopy;
    // 中文说明：outputBuffer 接收 WDF 已验证的输出缓冲地址。
    PVOID outputBuffer = NULL;
    // 中文说明：response 指向协议响应头和尾随行数组。
    KSWORD_ARK_QUERY_IOCTL_REGISTRY_RESPONSE* response = NULL;
    // 中文说明：actualInputLength 保存 WDF 返回的真实输入长度。
    size_t actualInputLength = 0U;
    // 中文说明：actualOutputLength 保存 WDF 返回的真实输出容量。
    size_t actualOutputLength = 0U;
    // 中文说明：capacityCount 是输出缓冲可容纳的行数。
    ULONG capacityCount = 0UL;
    // 中文说明：requestedCount 是 R3 限制后的目标行数。
    ULONG requestedCount = 0UL;
    // 中文说明：returnedCount 是最终写入行数。
    ULONG returnedCount = 0UL;
    // 中文说明：totalCount 是静态 registry 的完整行数。
    ULONG totalCount = 0UL;
    // 中文说明：entryIndex 遍历稳定 registry 索引。
    ULONG entryIndex = 0UL;
    // 中文说明：status 保存 WDF 验证状态。
    NTSTATUS status = STATUS_SUCCESS;

    // 中文说明：该参数仅用于统一 handler 签名，实际容量由 WDF 再确认。
    UNREFERENCED_PARAMETER(Device);
    // 中文说明：输入最小长度由统一 WDF helper 再确认。
    UNREFERENCED_PARAMETER(InputBufferLength);
    // 中文说明：该参数仅用于统一 handler 签名，实际容量由 WDF 再确认。
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // 中文说明：调用方必须提供完成字节计数地址。
    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // 中文说明：所有失败路径默认不返回响应字节。
    *BytesReturned = 0U;

    // 中文说明：读取固定请求并拒绝短输入。
    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_IOCTL_REGISTRY_REQUEST),
        (PVOID*)&requestPointer,
        &actualInputLength);
    // 中文说明：WDF 输入验证失败时原样返回状态。
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // 中文说明：复制输入后才可清空 METHOD_BUFFERED 输出区。
    RtlCopyMemory(&requestCopy, requestPointer, sizeof(requestCopy));

    // 中文说明：只接受当前协议版本，防止字段布局误解。
    if (requestCopy.version != KSWORD_ARK_IOCTL_REGISTRY_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    // 中文说明：输出至少要容纳固定响应头。
    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_IOCTL_REGISTRY_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    // 中文说明：WDF 输出验证失败时原样返回状态。
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // 中文说明：清空整个输出，避免泄露未初始化内核字节。
    RtlZeroMemory(outputBuffer, actualOutputLength);
    // 中文说明：响应头位于输出缓冲起始位置。
    response = (KSWORD_ARK_QUERY_IOCTL_REGISTRY_RESPONSE*)outputBuffer;
    // 中文说明：计算尾随数组的真实容量。
    capacityCount = (ULONG)((actualOutputLength - KSWORD_ARK_IOCTL_REGISTRY_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_IOCTL_REGISTRY_ENTRY));
    // 中文说明：取得统一 dispatch registry 的完整元素数。
    totalCount = KswordARKGetRegisteredIoctlCount();
    // 中文说明：0 表示采用协议上限，其余值被限制到协议上限。
    requestedCount = requestCopy.maxEntries == 0UL
        ? KSWORD_ARK_IOCTL_REGISTRY_MAX_ENTRIES
        : min(requestCopy.maxEntries, KSWORD_ARK_IOCTL_REGISTRY_MAX_ENTRIES);
    // 中文说明：最终行数同时受完整数、请求预算和输出容量约束。
    returnedCount = min(totalCount, min(requestedCount, capacityCount));

    // 中文说明：填充固定响应摘要。
    response->version = KSWORD_ARK_IOCTL_REGISTRY_PROTOCOL_VERSION;
    // 中文说明：返回不完整时明确标记截断。
    response->status = returnedCount < totalCount
        ? KSWORD_ARK_IOCTL_REGISTRY_STATUS_TRUNCATED
        : KSWORD_ARK_IOCTL_REGISTRY_STATUS_OK;
    // 中文说明：保存 registry 完整行数。
    response->totalCount = totalCount;
    // 中文说明：保存本次实际返回行数。
    response->returnedCount = returnedCount;
    // 中文说明：发布尾随行结构大小供 R3 严格校验。
    response->entrySize = (ULONG)sizeof(KSWORD_ARK_IOCTL_REGISTRY_ENTRY);
    // 中文说明：发布重复控制码诊断计数。
    response->duplicateCount = KswordARKGetDuplicateIoctlCount();
    // 中文说明：查询本身成功时 lastStatus 为 STATUS_SUCCESS。
    response->lastStatus = STATUS_SUCCESS;

    // 中文说明：按静态数组顺序复制只读注册信息。
    for (entryIndex = 0UL; entryIndex < returnedCount; ++entryIndex) {
        // 中文说明：按索引获取只读 registry 项。
        const KSWORD_ARK_IOCTL_ENTRY* sourceEntry = KswordARKGetIoctlEntryByIndex(entryIndex);
        // 中文说明：destinationEntry 指向当前协议输出行。
        KSWORD_ARK_IOCTL_REGISTRY_ENTRY* destinationEntry = &response->entries[entryIndex];

        // 中文说明：理论上索引已受 totalCount 约束；NULL 时停止并报告截断。
        if (sourceEntry == NULL) {
            response->returnedCount = entryIndex;
            response->status = KSWORD_ARK_IOCTL_REGISTRY_STATUS_TRUNCATED;
            returnedCount = entryIndex;
            break;
        }

        // 中文说明：复制完整控制码。
        destinationEntry->ioControlCode = sourceEntry->IoControlCode;
        // 中文说明：从 CTL_CODE 提取 12 位 function number。
        destinationEntry->functionNumber = (sourceEntry->IoControlCode >> 2U) & 0x0FFFUL;
        // 中文说明：从 CTL_CODE 提取传输 method。
        destinationEntry->method = sourceEntry->IoControlCode & 0x3UL;
        // 中文说明：从 CTL_CODE 提取访问要求。
        destinationEntry->access = (sourceEntry->IoControlCode >> 14U) & 0x3UL;
        // 中文说明：复制 dispatch registry flags。
        destinationEntry->flags = sourceEntry->Flags;
        // 中文说明：复制 DynData 能力门槛位图。
        destinationEntry->requiredCapability = sourceEntry->RequiredCapability;
        // 中文说明：仅在请求明确允许时返回 handler 诊断地址。
        destinationEntry->handlerAddress =
            (requestCopy.flags & KSWORD_ARK_IOCTL_REGISTRY_FLAG_INCLUDE_HANDLER) != 0UL
            ? (ULONG64)(ULONG_PTR)sourceEntry->Handler
            : 0ULL;
        // 中文说明：复制固定长度的可读 IOCTL 名称。
        KswordARKCopyIoctlRegistryName(
            destinationEntry->name,
            KSWORD_ARK_IOCTL_REGISTRY_NAME_CHARS,
            sourceEntry->Name);
    }

    // 中文说明：返回固定头和实际行的精确字节数。
    *BytesReturned = KSWORD_ARK_IOCTL_REGISTRY_RESPONSE_HEADER_SIZE +
        ((size_t)returnedCount * sizeof(KSWORD_ARK_IOCTL_REGISTRY_ENTRY));
    // 中文说明：协议级截断仍是成功响应，由 status 字段表达。
    return STATUS_SUCCESS;
}
