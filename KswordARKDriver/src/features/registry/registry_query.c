/*++

Module Name:

    registry_query.c

Abstract:

    R0 registry read helpers for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntstrsafe.h>

#define KSWORD_ARK_REGISTRY_QUERY_TAG 'gRsK'

static USHORT
KswordARKRegistryBoundedWideLength(
    _In_reads_(MaxChars) const WCHAR* Text,
    _In_ USHORT MaxChars
    )
/*++

Routine Description:

    计算共享协议定长 WCHAR 字符串的实际长度。中文说明：R3 传入的是固定数组，
    不能假设一定 NUL 结尾，因此必须显式限制扫描范围。

Arguments:

    Text - 固定数组首地址。
    MaxChars - 最多允许扫描的字符数。

Return Value:

    不包含 NUL 的字符数；空指针或空容量返回 0。

--*/
{
    USHORT index = 0U;

    if (Text == NULL || MaxChars == 0U) {
        return 0U;
    }

    for (index = 0U; index < MaxChars; ++index) {
        if (Text[index] == L'\0') {
            break;
        }
    }
    return index;
}

static BOOLEAN
KswordARKRegistryValidateKernelPath(
    _In_ const UNICODE_STRING* KeyPath
    )
/*++

Routine Description:

    校验注册表路径是否是 NT 内核命名空间路径。中文说明：驱动只接受
    \REGISTRY\...，避免 R3 把 HKLM/HKCU 这种 UI 路径直接传入导致歧义。

Arguments:

    KeyPath - 待检查键路径。

Return Value:

    TRUE 表示可继续 ZwOpenKey；FALSE 表示路径非法。

--*/
{
    UNICODE_STRING prefix;

    if (KeyPath == NULL || KeyPath->Buffer == NULL || KeyPath->Length == 0U) {
        return FALSE;
    }

    RtlInitUnicodeString(&prefix, L"\\REGISTRY\\");
    return RtlPrefixUnicodeString(&prefix, KeyPath, TRUE);
}

static VOID
KswordARKRegistryPrepareResponse(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Outptr_ KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE** ResponseOut
    )
/*++

Routine Description:

    初始化固定响应包。中文说明：所有失败路径也尽量返回结构化响应，R3 可以
    展示 lastStatus 和状态枚举，而不是只看到 DeviceIoControl 失败。

Arguments:

    OutputBuffer - WDF 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    ResponseOut - 返回响应结构指针。

Return Value:

    None. 本函数没有返回值。

--*/
{
    KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE* response = NULL;

    if (ResponseOut == NULL) {
        return;
    }
    *ResponseOut = NULL;

    if (OutputBuffer == NULL ||
        OutputBufferLength < sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE)) {
        return;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_REGISTRY_READ_STATUS_UNKNOWN;
    response->lastStatus = STATUS_UNSUCCESSFUL;
    *ResponseOut = response;
}

NTSTATUS
KswordARKDriverReadRegistryValue(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    读取指定注册表值。中文说明：该函数只做只读 ZwOpenKey/ZwQueryValueKey，
    数据最多复制 KSWORD_ARK_REGISTRY_DATA_MAX_BYTES，超出时返回截断状态。

Arguments:

    OutputBuffer - 输出响应缓冲。
    OutputBufferLength - 输出缓冲长度。
    Request - 共享协议请求。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示已写入结构化响应；参数/输出缓冲错误返回失败。

--*/
{
    KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    UNICODE_STRING valueName;
    HANDLE keyHandle = NULL;
    ULONG resultLength = 0UL;
    ULONG queryLength = 0UL;
    ULONG maxDataBytes = KSWORD_ARK_REGISTRY_DATA_MAX_BYTES;
    ULONG boundedQueryLength = 0UL;
    PKEY_VALUE_PARTIAL_INFORMATION valueInformation = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    USHORT keyPathChars = 0U;
    USHORT valueNameChars = 0U;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KswordARKRegistryPrepareResponse(OutputBuffer, OutputBufferLength, &response);
    if (response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    keyPathChars = KswordARKRegistryBoundedWideLength(
        Request->keyPath,
        (USHORT)KSWORD_ARK_REGISTRY_PATH_CHARS);
    if (keyPathChars == 0U || keyPathChars >= KSWORD_ARK_REGISTRY_PATH_CHARS) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    keyPath.Buffer = (PWSTR)Request->keyPath;
    keyPath.Length = (USHORT)(keyPathChars * sizeof(WCHAR));
    keyPath.MaximumLength = keyPath.Length;
    if (!KswordARKRegistryValidateKernelPath(&keyPath)) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = STATUS_OBJECT_PATH_SYNTAX_BAD;
        return STATUS_SUCCESS;
    }

    if ((Request->flags & KSWORD_ARK_REGISTRY_READ_FLAG_VALUE_NAME_PRESENT) != 0UL) {
        valueNameChars = KswordARKRegistryBoundedWideLength(
            Request->valueName,
            (USHORT)KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS);
        if (valueNameChars >= KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS) {
            response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
            response->lastStatus = STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;
        }
    }
    else {
        valueNameChars = 0U;
    }

    valueName.Buffer = (PWSTR)Request->valueName;
    valueName.Length = (USHORT)(valueNameChars * sizeof(WCHAR));
    valueName.MaximumLength = valueName.Length;

    InitializeObjectAttributes(
        &objectAttributes,
        &keyPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = ZwOpenKey(&keyHandle, KEY_QUERY_VALUE, &objectAttributes);
    if (!NT_SUCCESS(status)) {
        response->status = (status == STATUS_OBJECT_NAME_NOT_FOUND ||
            status == STATUS_OBJECT_PATH_NOT_FOUND) ?
            KSWORD_ARK_REGISTRY_READ_STATUS_NOT_FOUND :
            KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = status;
        return STATUS_SUCCESS;
    }

    status = ZwQueryValueKey(
        keyHandle,
        &valueName,
        KeyValuePartialInformation,
        NULL,
        0UL,
        &resultLength);
    if (resultLength == 0UL &&
        status != STATUS_BUFFER_TOO_SMALL &&
        status != STATUS_BUFFER_OVERFLOW) {
        response->status = (status == STATUS_OBJECT_NAME_NOT_FOUND) ?
            KSWORD_ARK_REGISTRY_READ_STATUS_NOT_FOUND :
            KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = status;
        ZwClose(keyHandle);
        return STATUS_SUCCESS;
    }

    if (resultLength < (ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data)) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = STATUS_INFO_LENGTH_MISMATCH;
        ZwClose(keyHandle);
        return STATUS_SUCCESS;
    }

    if (Request->maxDataBytes != 0UL &&
        Request->maxDataBytes < maxDataBytes) {
        maxDataBytes = Request->maxDataBytes;
    }
    if (maxDataBytes > KSWORD_ARK_REGISTRY_DATA_MAX_BYTES) {
        maxDataBytes = KSWORD_ARK_REGISTRY_DATA_MAX_BYTES;
    }

    boundedQueryLength =
        (ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
        maxDataBytes;
    if (boundedQueryLength < (ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data)) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = STATUS_INTEGER_OVERFLOW;
        ZwClose(keyHandle);
        return STATUS_SUCCESS;
    }
    if (boundedQueryLength > resultLength) {
        boundedQueryLength = resultLength;
    }
    if (boundedQueryLength < (ULONG)sizeof(KEY_VALUE_PARTIAL_INFORMATION)) {
        boundedQueryLength = (ULONG)sizeof(KEY_VALUE_PARTIAL_INFORMATION);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    valueInformation = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        boundedQueryLength,
        KSWORD_ARK_REGISTRY_QUERY_TAG);
#pragma warning(pop)
    if (valueInformation == NULL) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        ZwClose(keyHandle);
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(valueInformation, boundedQueryLength);

    queryLength = boundedQueryLength;
    status = ZwQueryValueKey(
        keyHandle,
        &valueName,
        KeyValuePartialInformation,
        valueInformation,
        queryLength,
        &resultLength);
    ZwClose(keyHandle);

    if (!NT_SUCCESS(status) &&
        status != STATUS_BUFFER_TOO_SMALL &&
        status != STATUS_BUFFER_OVERFLOW) {
        response->status = (status == STATUS_OBJECT_NAME_NOT_FOUND) ?
            KSWORD_ARK_REGISTRY_READ_STATUS_NOT_FOUND :
            KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = status;
        ExFreePoolWithTag(valueInformation, KSWORD_ARK_REGISTRY_QUERY_TAG);
        return STATUS_SUCCESS;
    }

    if (status == STATUS_BUFFER_TOO_SMALL) {
        response->requiredBytes = (resultLength > (ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data)) ?
            (resultLength - (ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data)) :
            0UL;
        response->dataBytes = 0UL;
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL;
        response->lastStatus = STATUS_BUFFER_TOO_SMALL;
        ExFreePoolWithTag(valueInformation, KSWORD_ARK_REGISTRY_QUERY_TAG);
        return STATUS_SUCCESS;
    }

    response->valueType = valueInformation->Type;
    response->requiredBytes = valueInformation->DataLength;

    response->dataBytes = valueInformation->DataLength;
    if (response->dataBytes > maxDataBytes) {
        response->dataBytes = maxDataBytes;
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL;
        response->lastStatus = STATUS_BUFFER_TOO_SMALL;
    }
    else {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS;
        response->lastStatus = STATUS_SUCCESS;
    }

    if (response->dataBytes != 0UL) {
        RtlCopyMemory(response->data, valueInformation->Data, response->dataBytes);
    }

    ExFreePoolWithTag(valueInformation, KSWORD_ARK_REGISTRY_QUERY_TAG);
    return STATUS_SUCCESS;
}
