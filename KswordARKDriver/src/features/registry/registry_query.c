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

#ifndef STATUS_NO_MORE_ENTRIES
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#endif

#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif

#define KSWORD_ARK_REGISTRY_QUERY_TAG 'gRsK'

NTKERNELAPI
NTSTATUS
ZwRenameKey(
    _In_ HANDLE KeyHandle,
    _In_ PUNICODE_STRING NewName
    );

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

static NTSTATUS
KswordARKRegistryBuildKernelPath(
    _In_reads_(KSWORD_ARK_REGISTRY_PATH_CHARS) const WCHAR* SourcePath,
    _Out_ UNICODE_STRING* PathOut
    )
/*++

Routine Description:

    从共享协议固定路径数组构造 UNICODE_STRING。中文说明：所有 R0 注册表
    操作都必须先经过这里，统一检查 NUL、长度和 \REGISTRY\ 前缀，避免各个
    Zw* 调用点重复散落路径校验逻辑。

Arguments:

    SourcePath - 请求中的固定 WCHAR 路径数组。
    PathOut - 输出 UNICODE_STRING，Buffer 指向 SourcePath，不分配内存。

Return Value:

    STATUS_SUCCESS 或路径参数错误状态。

--*/
{
    USHORT pathChars = 0U;

    if (SourcePath == NULL || PathOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(PathOut, sizeof(*PathOut));
    pathChars = KswordARKRegistryBoundedWideLength(
        SourcePath,
        (USHORT)KSWORD_ARK_REGISTRY_PATH_CHARS);
    if (pathChars == 0U || pathChars >= KSWORD_ARK_REGISTRY_PATH_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    PathOut->Buffer = (PWSTR)SourcePath;
    PathOut->Length = (USHORT)(pathChars * sizeof(WCHAR));
    PathOut->MaximumLength = PathOut->Length;
    if (!KswordARKRegistryValidateKernelPath(PathOut)) {
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    }

    return STATUS_SUCCESS;
}

static VOID
KswordARKRegistryBuildValueName(
    _In_reads_(KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS) const WCHAR* SourceName,
    _In_ BOOLEAN NamePresent,
    _Out_ UNICODE_STRING* NameOut
    )
/*++

Routine Description:

    从共享协议固定值名数组构造 UNICODE_STRING。中文说明：默认值用空
    UNICODE_STRING 表示，ZwQueryValueKey/ZwSetValueKey/ZwDeleteValueKey 都
    接受这种形式。

Arguments:

    SourceName - 请求中的值名数组。
    NamePresent - TRUE 表示数组内是命名值；FALSE 表示默认值。
    NameOut - 输出值名 UNICODE_STRING。

Return Value:

    None. 本函数没有返回值。

--*/
{
    USHORT nameChars = 0U;

    RtlZeroMemory(NameOut, sizeof(*NameOut));
    if (!NamePresent || SourceName == NULL) {
        return;
    }

    nameChars = KswordARKRegistryBoundedWideLength(
        SourceName,
        (USHORT)KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS);
    NameOut->Buffer = (PWSTR)SourceName;
    NameOut->Length = (USHORT)(nameChars * sizeof(WCHAR));
    NameOut->MaximumLength = NameOut->Length;
}

static VOID
KswordARKRegistryPrepareOperationResponse(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Outptr_ KSWORD_ARK_REGISTRY_OPERATION_RESPONSE** ResponseOut
    )
/*++

Routine Description:

    初始化通用写操作响应。中文说明：创建/删除/重命名等动作都只需要
    聚合状态和底层 NTSTATUS，因此共用此响应结构。

Arguments:

    OutputBuffer - 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    ResponseOut - 输出响应指针。

Return Value:

    None. 本函数没有返回值。

--*/
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;

    if (ResponseOut == NULL) {
        return;
    }
    *ResponseOut = NULL;

    if (OutputBuffer == NULL ||
        OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_REGISTRY_OPERATION_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_UNKNOWN;
    response->lastStatus = STATUS_UNSUCCESSFUL;
    *ResponseOut = response;
}

static ULONG
KswordARKRegistryMapOperationStatus(
    _In_ NTSTATUS Status
    )
/*++

Routine Description:

    把 NTSTATUS 转换为 R3 易展示的注册表操作状态。中文说明：IOCTL 本身尽量
    返回 STATUS_SUCCESS，实际成败放在结构化响应内。

Arguments:

    Status - 底层 Zw* 返回状态。

Return Value:

    KSWORD_ARK_REGISTRY_OPERATION_STATUS_*。

--*/
{
    if (NT_SUCCESS(Status)) {
        return KSWORD_ARK_REGISTRY_OPERATION_STATUS_SUCCESS;
    }
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_PATH_NOT_FOUND) {
        return KSWORD_ARK_REGISTRY_OPERATION_STATUS_NOT_FOUND;
    }
    if (Status == STATUS_OBJECT_NAME_COLLISION) {
        return KSWORD_ARK_REGISTRY_OPERATION_STATUS_ALREADY_EXISTS;
    }
    return KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
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

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
        response->lastStatus = status;
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

NTSTATUS
KswordARKDriverEnumRegistryKey(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_ENUM_REGISTRY_KEY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    枚举指定注册表键的子键和值。中文说明：R3 树展开和值列表刷新在 R0 在线
    时通过该函数完成，避免同一 UI 页面混用 Win32 与 R0 读路径。

Arguments:

    OutputBuffer - 输出枚举响应。
    OutputBufferLength - 输出缓冲长度。
    Request - 枚举请求。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示结构化响应已写入；参数错误返回失败。

--*/
{
    KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    HANDLE keyHandle = NULL;
    ULONG subKeyIndex = 0UL;
    ULONG valueIndex = 0UL;
    ULONG maxSubKeys = KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS;
    ULONG maxValues = KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES;
    ULONG maxValueDataBytes = KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_UNKNOWN;
    response->lastStatus = STATUS_UNSUCCESSFUL;
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_FAILED;
        response->lastStatus = status;
        return STATUS_SUCCESS;
    }

    InitializeObjectAttributes(
        &objectAttributes,
        &keyPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = ZwOpenKey(&keyHandle, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &objectAttributes);
    if (!NT_SUCCESS(status)) {
        response->status = (status == STATUS_OBJECT_NAME_NOT_FOUND ||
            status == STATUS_OBJECT_PATH_NOT_FOUND) ?
            KSWORD_ARK_REGISTRY_ENUM_STATUS_NOT_FOUND :
            KSWORD_ARK_REGISTRY_ENUM_STATUS_FAILED;
        response->lastStatus = status;
        return STATUS_SUCCESS;
    }

    if (Request->maxSubKeys != 0UL && Request->maxSubKeys < maxSubKeys) {
        maxSubKeys = Request->maxSubKeys;
    }
    if (Request->maxValues != 0UL && Request->maxValues < maxValues) {
        maxValues = Request->maxValues;
    }
    if (Request->maxValueDataBytes != 0UL && Request->maxValueDataBytes < maxValueDataBytes) {
        maxValueDataBytes = Request->maxValueDataBytes;
    }

    if ((Request->flags & KSWORD_ARK_REGISTRY_ENUM_FLAG_INCLUDE_SUBKEYS) != 0UL) {
        for (subKeyIndex = 0UL; ; ++subKeyIndex) {
            UCHAR informationBuffer[sizeof(KEY_BASIC_INFORMATION) + (KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS * sizeof(WCHAR))] = { 0 };
            PKEY_BASIC_INFORMATION keyInformation = (PKEY_BASIC_INFORMATION)informationBuffer;
            ULONG resultLength = 0UL;
            ULONG copyBytes = 0UL;

            status = ZwEnumerateKey(
                keyHandle,
                subKeyIndex,
                KeyBasicInformation,
                keyInformation,
                sizeof(informationBuffer),
                &resultLength);
            if (status == STATUS_NO_MORE_ENTRIES) {
                break;
            }
            if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW) {
                response->lastStatus = status;
                break;
            }

            response->subKeyCount += 1UL;
            if (response->returnedSubKeyCount >= maxSubKeys ||
                response->returnedSubKeyCount >= KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS) {
                response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                continue;
            }

            copyBytes = keyInformation->NameLength;
            if (copyBytes > ((KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS - 1U) * sizeof(WCHAR))) {
                copyBytes = (KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS - 1U) * sizeof(WCHAR);
                response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
            }
            if (copyBytes != 0UL) {
                RtlCopyMemory(
                    response->subKeys[response->returnedSubKeyCount].name,
                    keyInformation->Name,
                    copyBytes);
            }
            response->returnedSubKeyCount += 1UL;
        }
    }

    if ((Request->flags & KSWORD_ARK_REGISTRY_ENUM_FLAG_INCLUDE_VALUES) != 0UL) {
        for (valueIndex = 0UL; ; ++valueIndex) {
            UCHAR informationBuffer[sizeof(KEY_VALUE_FULL_INFORMATION) +
                (KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS * sizeof(WCHAR)) +
                KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES] = { 0 };
            PKEY_VALUE_FULL_INFORMATION valueInformation = (PKEY_VALUE_FULL_INFORMATION)informationBuffer;
            ULONG resultLength = 0UL;
            ULONG nameBytes = 0UL;
            ULONG dataBytes = 0UL;

            status = ZwEnumerateValueKey(
                keyHandle,
                valueIndex,
                KeyValueFullInformation,
                valueInformation,
                sizeof(informationBuffer),
                &resultLength);
            if (status == STATUS_NO_MORE_ENTRIES) {
                break;
            }
            if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW) {
                response->lastStatus = status;
                break;
            }

            response->valueCount += 1UL;
            if (response->returnedValueCount >= maxValues ||
                response->returnedValueCount >= KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES) {
                response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                continue;
            }

            nameBytes = valueInformation->NameLength;
            if (nameBytes > ((KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS - 1U) * sizeof(WCHAR))) {
                nameBytes = (KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS - 1U) * sizeof(WCHAR);
                response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
            }
            if (nameBytes != 0UL) {
                RtlCopyMemory(
                    response->values[response->returnedValueCount].name,
                    valueInformation->Name,
                    nameBytes);
                response->values[response->returnedValueCount].flags |=
                    KSWORD_ARK_REGISTRY_ENUM_VALUE_FLAG_NAME_PRESENT;
            }

            response->values[response->returnedValueCount].valueType = valueInformation->Type;
            response->values[response->returnedValueCount].requiredBytes = valueInformation->DataLength;
            dataBytes = valueInformation->DataLength;
            if (dataBytes > maxValueDataBytes) {
                dataBytes = maxValueDataBytes;
                response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
            }
            if (dataBytes > KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES) {
                dataBytes = KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES;
                response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
            }
            if (dataBytes != 0UL &&
                valueInformation->DataOffset != 0UL &&
                valueInformation->DataOffset < sizeof(informationBuffer) &&
                dataBytes <= (sizeof(informationBuffer) - valueInformation->DataOffset)) {
                RtlCopyMemory(
                    response->values[response->returnedValueCount].data,
                    ((PUCHAR)valueInformation) + valueInformation->DataOffset,
                    dataBytes);
                response->values[response->returnedValueCount].dataBytes = dataBytes;
            }
            response->returnedValueCount += 1UL;
        }
    }

    ZwClose(keyHandle);
    if (response->status == KSWORD_ARK_REGISTRY_ENUM_STATUS_UNKNOWN) {
        response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS;
    }
    if (response->lastStatus == STATUS_UNSUCCESSFUL) {
        response->lastStatus = STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverSetRegistryValue(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    UNICODE_STRING valueName;
    HANDLE keyHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    KswordARKRegistryPrepareOperationResponse(OutputBuffer, OutputBufferLength, &response);
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION ||
        Request->dataBytes > KSWORD_ARK_REGISTRY_DATA_MAX_BYTES) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (NT_SUCCESS(status)) {
        KswordARKRegistryBuildValueName(
            Request->valueName,
            ((Request->flags & KSWORD_ARK_REGISTRY_SET_FLAG_VALUE_NAME_PRESENT) != 0UL),
            &valueName);
        InitializeObjectAttributes(&objectAttributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = ZwOpenKey(&keyHandle, KEY_SET_VALUE, &objectAttributes);
        if (NT_SUCCESS(status)) {
            status = ZwSetValueKey(
                keyHandle,
                &valueName,
                0UL,
                Request->valueType,
                (PVOID)Request->data,
                Request->dataBytes);
            ZwClose(keyHandle);
        }
    }

    response->status = KswordARKRegistryMapOperationStatus(status);
    response->lastStatus = status;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverDeleteRegistryValue(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_REGISTRY_VALUE_NAME_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    UNICODE_STRING valueName;
    HANDLE keyHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    KswordARKRegistryPrepareOperationResponse(OutputBuffer, OutputBufferLength, &response);
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (NT_SUCCESS(status)) {
        KswordARKRegistryBuildValueName(
            Request->valueName,
            ((Request->flags & KSWORD_ARK_REGISTRY_DELETE_VALUE_FLAG_NAME_PRESENT) != 0UL),
            &valueName);
        InitializeObjectAttributes(&objectAttributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = ZwOpenKey(&keyHandle, KEY_SET_VALUE, &objectAttributes);
        if (NT_SUCCESS(status)) {
            status = ZwDeleteValueKey(keyHandle, &valueName);
            ZwClose(keyHandle);
        }
    }

    response->status = KswordARKRegistryMapOperationStatus(status);
    response->lastStatus = status;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverCreateRegistryKey(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    HANDLE keyHandle = NULL;
    ULONG disposition = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    KswordARKRegistryPrepareOperationResponse(OutputBuffer, OutputBufferLength, &response);
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (NT_SUCCESS(status)) {
        InitializeObjectAttributes(&objectAttributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = ZwCreateKey(
            &keyHandle,
            KEY_READ | KEY_WRITE,
            &objectAttributes,
            0UL,
            NULL,
            REG_OPTION_NON_VOLATILE,
            &disposition);
        if (NT_SUCCESS(status)) {
            ZwClose(keyHandle);
        }
    }

    response->status = KswordARKRegistryMapOperationStatus(status);
    response->lastStatus = status;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverDeleteRegistryKey(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    HANDLE keyHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    KswordARKRegistryPrepareOperationResponse(OutputBuffer, OutputBufferLength, &response);
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (NT_SUCCESS(status)) {
        InitializeObjectAttributes(&objectAttributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = ZwOpenKey(&keyHandle, DELETE, &objectAttributes);
        if (NT_SUCCESS(status)) {
            status = ZwDeleteKey(keyHandle);
            ZwClose(keyHandle);
        }
    }

    response->status = KswordARKRegistryMapOperationStatus(status);
    response->lastStatus = status;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverRenameRegistryValue(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_RENAME_REGISTRY_VALUE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    UNICODE_STRING oldValueName;
    UNICODE_STRING newValueName;
    HANDLE keyHandle = NULL;
    ULONG resultLength = 0UL;
    PKEY_VALUE_PARTIAL_INFORMATION valueInformation = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    KswordARKRegistryPrepareOperationResponse(OutputBuffer, OutputBufferLength, &response);
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (NT_SUCCESS(status)) {
        KswordARKRegistryBuildValueName(Request->oldValueName, TRUE, &oldValueName);
        KswordARKRegistryBuildValueName(Request->newValueName, TRUE, &newValueName);
        InitializeObjectAttributes(&objectAttributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = ZwOpenKey(&keyHandle, KEY_QUERY_VALUE | KEY_SET_VALUE, &objectAttributes);
    }
    if (NT_SUCCESS(status)) {
        status = ZwQueryValueKey(keyHandle, &oldValueName, KeyValuePartialInformation, NULL, 0UL, &resultLength);
        if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW) {
#pragma warning(push)
#pragma warning(disable:4996)
            valueInformation = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePoolWithTag(
                NonPagedPoolNx,
                resultLength,
                KSWORD_ARK_REGISTRY_QUERY_TAG);
#pragma warning(pop)
            if (valueInformation == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else {
                RtlZeroMemory(valueInformation, resultLength);
                status = ZwQueryValueKey(keyHandle, &oldValueName, KeyValuePartialInformation, valueInformation, resultLength, &resultLength);
            }
        }
        if (NT_SUCCESS(status) && valueInformation != NULL) {
            status = ZwSetValueKey(
                keyHandle,
                &newValueName,
                0UL,
                valueInformation->Type,
                valueInformation->Data,
                valueInformation->DataLength);
            if (NT_SUCCESS(status)) {
                status = ZwDeleteValueKey(keyHandle, &oldValueName);
            }
        }
        if (valueInformation != NULL) {
            ExFreePoolWithTag(valueInformation, KSWORD_ARK_REGISTRY_QUERY_TAG);
        }
        ZwClose(keyHandle);
    }

    response->status = KswordARKRegistryMapOperationStatus(status);
    response->lastStatus = status;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverRenameRegistryKey(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_RENAME_REGISTRY_KEY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_REGISTRY_OPERATION_RESPONSE* response = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING keyPath;
    UNICODE_STRING newKeyName;
    HANDLE keyHandle = NULL;
    USHORT newNameChars = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_REGISTRY_OPERATION_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    KswordARKRegistryPrepareOperationResponse(OutputBuffer, OutputBufferLength, &response);
    *BytesWrittenOut = sizeof(*response);

    if (Request->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        return STATUS_SUCCESS;
    }

    newNameChars = KswordARKRegistryBoundedWideLength(
        Request->newKeyName,
        (USHORT)KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS);
    if (newNameChars == 0U || newNameChars >= KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS) {
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
        response->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }
    newKeyName.Buffer = (PWSTR)Request->newKeyName;
    newKeyName.Length = (USHORT)(newNameChars * sizeof(WCHAR));
    newKeyName.MaximumLength = newKeyName.Length;

    status = KswordARKRegistryBuildKernelPath(Request->keyPath, &keyPath);
    if (NT_SUCCESS(status)) {
        InitializeObjectAttributes(&objectAttributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = ZwOpenKey(&keyHandle, KEY_WRITE, &objectAttributes);
        if (NT_SUCCESS(status)) {
            status = ZwRenameKey(keyHandle, &newKeyName);
            ZwClose(keyHandle);
        }
    }

    response->status = KswordARKRegistryMapOperationStatus(status);
    response->lastStatus = status;
    return STATUS_SUCCESS;
}
