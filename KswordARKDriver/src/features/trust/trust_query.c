/*++

Module Name:

    trust_query.c

Abstract:

    Phase-14 kernel Code Integrity and cached image trust diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntstrsafe.h>

#define KSWORD_ARK_TRUST_SYSTEM_CODEINTEGRITY_INFORMATION 103UL
#define KSWORD_ARK_TRUST_SYSTEM_SECUREBOOT_INFORMATION 145UL
#define KSWORD_ARK_TRUST_POOL_TAG 'tIsK'

typedef struct _KSWORD_ARK_SYSTEM_CODEINTEGRITY_INFORMATION
{
    ULONG Length;
    ULONG CodeIntegrityOptions;
} KSWORD_ARK_SYSTEM_CODEINTEGRITY_INFORMATION;

typedef struct _KSWORD_ARK_SYSTEM_SECUREBOOT_INFORMATION
{
    BOOLEAN SecureBootEnabled;
    BOOLEAN SecureBootCapable;
} KSWORD_ARK_SYSTEM_SECUREBOOT_INFORMATION;

typedef UCHAR KSWORD_ARK_SE_SIGNING_LEVEL;
typedef KSWORD_ARK_SE_SIGNING_LEVEL* PKSWORD_ARK_SE_SIGNING_LEVEL;

typedef PVOID
(NTAPI* KSWORD_ARK_TRUST_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

typedef NTSTATUS
(NTAPI* KSWORD_ARK_SE_GET_CACHED_SIGNING_LEVEL_FN)(
    _In_ PFILE_OBJECT FileObject,
    _Out_ PULONG Flags,
    _Out_ PKSWORD_ARK_SE_SIGNING_LEVEL SigningLevel,
    _Out_writes_bytes_to_opt_(*ThumbprintSize, *ThumbprintSize) PUCHAR Thumbprint,
    _Inout_opt_ PULONG ThumbprintSize,
    _Out_opt_ PULONG ThumbprintAlgorithm
    );

typedef NTSTATUS
(NTAPI* KSWORD_ARK_ZW_QUERY_SYSTEM_INFORMATION_FN)(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

static volatile LONG g_KswordArkTrustAllocatorResolved = 0;
static KSWORD_ARK_TRUST_EX_ALLOCATE_POOL2_FN g_KswordArkTrustExAllocatePool2 = NULL;
static volatile LONG g_KswordArkTrustSeSigningResolved = 0;
static KSWORD_ARK_SE_GET_CACHED_SIGNING_LEVEL_FN g_KswordArkTrustSeGetCachedSigningLevel = NULL;
static volatile LONG g_KswordArkTrustCiValidateResolved = 0;
static BOOLEAN g_KswordArkTrustCiValidatePresent = FALSE;
static KSWORD_ARK_SYSTEM_CODEINTEGRITY_INFORMATION g_KswordArkTrustCodeIntegrity;
static KSWORD_ARK_SYSTEM_SECUREBOOT_INFORMATION g_KswordArkTrustSecureBoot;
static NTSTATUS g_KswordArkTrustCodeIntegrityStatus = STATUS_NOT_SUPPORTED;
static NTSTATUS g_KswordArkTrustSecureBootStatus = STATUS_NOT_SUPPORTED;

static PVOID
KswordARKTrustAllocateNonPaged(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    分配 trust 查询临时缓冲。中文说明：优先使用 ExAllocatePool2，旧系统回退到
    ExAllocatePoolWithTag，保证驱动导入表不依赖新内核导出。

Arguments:

    BufferBytes - 请求分配的字节数。

Return Value:

    返回非分页池指针；失败返回 NULL。

--*/
{
    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&g_KswordArkTrustAllocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        g_KswordArkTrustExAllocatePool2 =
            (KSWORD_ARK_TRUST_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (g_KswordArkTrustExAllocatePool2 != NULL) {
        return g_KswordArkTrustExAllocatePool2(POOL_FLAG_NON_PAGED, BufferBytes, KSWORD_ARK_TRUST_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSWORD_ARK_TRUST_POOL_TAG);
#pragma warning(pop)
}

static VOID
KswordARKTrustCopyWideStringToFixedBuffer(
    _Out_writes_(DestinationChars) PWSTR Destination,
    _In_ USHORT DestinationChars,
    _In_reads_opt_(SourceChars) PCWSTR Source,
    _In_ USHORT SourceChars
    )
/*++

Routine Description:

    把请求路径复制到固定响应数组。中文说明：协议固定数组始终 NUL 结尾，
    R3 可以直接构造 std::wstring，不需要额外扫描未初始化内存。

Arguments:

    Destination - 响应宽字符数组。
    DestinationChars - 响应数组容量。
    Source - 输入宽字符数组，可为空。
    SourceChars - 输入字符数，不含 NUL。

Return Value:

    None. 本函数没有返回值。

--*/
{
    USHORT copyChars = 0U;

    if (Destination == NULL || DestinationChars == 0U) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL || SourceChars == 0U) {
        return;
    }

    copyChars = SourceChars;
    if (copyChars >= DestinationChars) {
        copyChars = DestinationChars - 1U;
    }

    RtlCopyMemory(Destination, Source, (SIZE_T)copyChars * sizeof(WCHAR));
    Destination[copyChars] = L'\0';
}

static KSWORD_ARK_SE_GET_CACHED_SIGNING_LEVEL_FN
KswordARKTrustResolveSeGetCachedSigningLevel(
    VOID
    )
/*++

Routine Description:

    动态解析 SeGetCachedSigningLevel。中文说明：不同 WDK/系统导出可用性不同，
    因此不把它放进静态导入表。

Arguments:

    None.

Return Value:

    返回函数指针；不可用时返回 NULL。

--*/
{
    if (InterlockedCompareExchange(&g_KswordArkTrustSeSigningResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"SeGetCachedSigningLevel");
        g_KswordArkTrustSeGetCachedSigningLevel =
            (KSWORD_ARK_SE_GET_CACHED_SIGNING_LEVEL_FN)MmGetSystemRoutineAddress(&routineName);
    }

    return g_KswordArkTrustSeGetCachedSigningLevel;
}

static BOOLEAN
KswordARKTrustResolveCiValidateExportPresence(
    VOID
    )
/*++

Routine Description:

    探测 CI 验证导出是否存在。中文说明：Phase-14 当前不直接调用
    CiValidateFileObject，只暴露可用性，避免把私有 CI policy 结构固化进协议。

Arguments:

    None.

Return Value:

    TRUE 表示 ci.dll/ci.sys 当前导出 CiValidateFileObject。

--*/
{
    if (InterlockedCompareExchange(&g_KswordArkTrustCiValidateResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"CiValidateFileObject");
        g_KswordArkTrustCiValidatePresent =
            (MmGetSystemRoutineAddress(&routineName) != NULL) ? TRUE : FALSE;
    }

    return g_KswordArkTrustCiValidatePresent;
}

static NTSTATUS
KswordARKTrustOpenFileForTrustQuery(
    _In_reads_(PathLengthChars) PCWSTR PathText,
    _In_ USHORT PathLengthChars,
    _In_ ULONG Flags,
    _Out_ HANDLE* FileHandleOut
    )
/*++

Routine Description:

    为 trust 查询打开目标文件。中文说明：这里只读取属性并获取 FILE_OBJECT，
    不申请写权限，不改变文件内容，也不触发删除/占用修复逻辑。

Arguments:

    PathText - NT 路径，例如 \??\C:\Windows\System32\ntoskrnl.exe。
    PathLengthChars - 路径字符数，不含 NUL。
    Flags - KSWORD_ARK_TRUST_QUERY_FLAG_*。
    FileHandleOut - 接收内核句柄，调用方负责 ZwClose。

Return Value:

    ZwCreateFile 返回的 NTSTATUS。

--*/
{
    UNICODE_STRING targetPath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    ULONG createOptions = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileHandleOut = NULL;
    if (PathText == NULL || PathLengthChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&targetPath, sizeof(targetPath));
    targetPath.Buffer = (PWCH)PathText;
    targetPath.Length = (USHORT)(PathLengthChars * sizeof(WCHAR));
    targetPath.MaximumLength = targetPath.Length + sizeof(WCHAR);

    InitializeObjectAttributes(
        &objectAttributes,
        &targetPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    createOptions = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_NON_DIRECTORY_FILE;
    if ((Flags & KSWORD_ARK_TRUST_QUERY_FLAG_OPEN_REPARSE_POINT) != 0UL) {
        createOptions |= FILE_OPEN_REPARSE_POINT;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        FileHandleOut,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        createOptions,
        NULL,
        0U);

    return status;
}

static NTSTATUS
KswordARKTrustReferenceFileObject(
    _In_ HANDLE FileHandle,
    _Outptr_ PFILE_OBJECT* FileObjectOut
    )
/*++

Routine Description:

    从内核句柄引用 FILE_OBJECT。中文说明：后续 signing level 查询需要
    FILE_OBJECT，引用成功后调用方必须 ObDereferenceObject。

Arguments:

    FileHandle - ZwCreateFile 返回的内核句柄。
    FileObjectOut - 接收 FILE_OBJECT 指针。

Return Value:

    STATUS_SUCCESS 或 ObReferenceObjectByHandle 返回值。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (FileObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileObjectOut = NULL;
    if (FileHandle == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ObReferenceObjectByHandle(
        FileHandle,
        0,
        NULL,
        KernelMode,
        (PVOID*)FileObjectOut,
        NULL);

    return status;
}

static NTSTATUS
KswordARKTrustQueryCachedSigningLevel(
    _In_ PFILE_OBJECT FileObject,
    _Inout_ KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE* Response
    )
/*++

Routine Description:

    查询文件对象的内核缓存 signing level。中文说明：该值代表内核 CI 视角，
    不是完整 Authenticode 证书链结果；R3 页面必须和 WinVerifyTrust 结果分栏展示。

Arguments:

    FileObject - 已引用的 FILE_OBJECT。
    Response - 响应包，接收 signing level、flags 和 thumbprint。

Return Value:

    STATUS_SUCCESS 或底层 SeGetCachedSigningLevel 状态。

--*/
{
    KSWORD_ARK_SE_GET_CACHED_SIGNING_LEVEL_FN seGetCachedSigningLevel = NULL;
    KSWORD_ARK_SE_SIGNING_LEVEL signingLevel = KSWORD_ARK_SIGNING_LEVEL_UNCHECKED;
    UCHAR thumbprint[KSWORD_ARK_TRUST_THUMBPRINT_MAX_BYTES] = { 0 };
    ULONG thumbprintSize = sizeof(thumbprint);
    ULONG thumbprintAlgorithm = 0UL;
    ULONG signingFlags = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileObject == NULL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    seGetCachedSigningLevel = KswordARKTrustResolveSeGetCachedSigningLevel();
    if (seGetCachedSigningLevel == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    status = seGetCachedSigningLevel(
        FileObject,
        &signingFlags,
        &signingLevel,
        thumbprint,
        &thumbprintSize,
        &thumbprintAlgorithm);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Response->signingLevel = signingLevel;
    Response->signingLevelFlags = signingFlags;
    Response->thumbprintAlgorithm = thumbprintAlgorithm;
    Response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_SIGNING_LEVEL_PRESENT;
    if (thumbprintSize > sizeof(Response->thumbprint)) {
        thumbprintSize = sizeof(Response->thumbprint);
    }
    Response->thumbprintSize = thumbprintSize;
    if (thumbprintSize > 0UL) {
        RtlCopyMemory(Response->thumbprint, thumbprint, thumbprintSize);
        Response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_THUMBPRINT_PRESENT;
    }

    return STATUS_SUCCESS;
}

VOID
KswordARKTrustInitialize(
    VOID
    )
/*++

Routine Description:

    初始化 Phase-14 全局 CI/SecureBoot 快照。中文说明：参考 System Informer
    DriverEntry 中对 SystemCodeIntegrityInformation 的处理；查询失败时保留状态码。

Arguments:

    None.

Return Value:

    None. 本函数没有返回值。

--*/
{
    RtlZeroMemory(&g_KswordArkTrustCodeIntegrity, sizeof(g_KswordArkTrustCodeIntegrity));
    RtlZeroMemory(&g_KswordArkTrustSecureBoot, sizeof(g_KswordArkTrustSecureBoot));

    g_KswordArkTrustCodeIntegrity.Length = sizeof(g_KswordArkTrustCodeIntegrity);
    g_KswordArkTrustCodeIntegrityStatus = ZwQuerySystemInformation(
        KSWORD_ARK_TRUST_SYSTEM_CODEINTEGRITY_INFORMATION,
        &g_KswordArkTrustCodeIntegrity,
        sizeof(g_KswordArkTrustCodeIntegrity),
        NULL);
    if (!NT_SUCCESS(g_KswordArkTrustCodeIntegrityStatus)) {
        RtlZeroMemory(&g_KswordArkTrustCodeIntegrity, sizeof(g_KswordArkTrustCodeIntegrity));
    }

    g_KswordArkTrustSecureBootStatus = ZwQuerySystemInformation(
        KSWORD_ARK_TRUST_SYSTEM_SECUREBOOT_INFORMATION,
        &g_KswordArkTrustSecureBoot,
        sizeof(g_KswordArkTrustSecureBoot),
        NULL);
    if (!NT_SUCCESS(g_KswordArkTrustSecureBootStatus)) {
        RtlZeroMemory(&g_KswordArkTrustSecureBoot, sizeof(g_KswordArkTrustSecureBoot));
    }
}

NTSTATUS
KswordARKDriverQueryImageTrust(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询镜像信任的内核侧状态。中文说明：该接口只读，返回全局 CI 策略、
    Secure Boot 状态和可选文件 cached signing level；证书主体/颁发者/Catalog
    仍由 R3 Authenticode 查询负责。

Arguments:

    OutputBuffer - METHOD_BUFFERED 输出缓冲。
    OutputBufferLength - 输出缓冲长度。
    Request - 查询请求。
    BytesWrittenOut - 接收写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；具体失败原因放在 response->queryStatus。

--*/
{
    KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE* response = NULL;
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    ULONG requestFlags = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->pathLengthChars >= KSWORD_ARK_TRUST_PATH_MAX_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->pathLengthChars != 0U &&
        Request->path[Request->pathLengthChars] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_TRUST_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->queryStatus = KSWORD_ARK_TRUST_STATUS_UNAVAILABLE;
    response->trustSource = KSWORD_ARK_TRUST_SOURCE_NONE;
    response->signingLevel = KSWORD_ARK_SIGNING_LEVEL_UNCHECKED;
    response->codeIntegrityStatus = g_KswordArkTrustCodeIntegrityStatus;
    response->secureBootStatus = g_KswordArkTrustSecureBootStatus;
    response->openStatus = STATUS_NOT_SUPPORTED;
    response->objectStatus = STATUS_NOT_SUPPORTED;
    response->signingLevelStatus = STATUS_NOT_SUPPORTED;
    response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_AUTHENTICODE_DEFERRED_R3;

    requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_ALL : Request->flags;
    if ((requestFlags & KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_GLOBAL_CI) != 0UL) {
        if (NT_SUCCESS(g_KswordArkTrustCodeIntegrityStatus)) {
            response->codeIntegrityOptions = g_KswordArkTrustCodeIntegrity.CodeIntegrityOptions;
            response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_GLOBAL_CI_PRESENT;
            response->trustSource = KSWORD_ARK_TRUST_SOURCE_SYSTEM_CODE_INTEGRITY;
        }
        if (NT_SUCCESS(g_KswordArkTrustSecureBootStatus)) {
            response->secureBootEnabled = (ULONG)g_KswordArkTrustSecureBoot.SecureBootEnabled;
            response->secureBootCapable = (ULONG)g_KswordArkTrustSecureBoot.SecureBootCapable;
            response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_SECURE_BOOT_PRESENT;
        }
        if (KswordARKTrustResolveCiValidateExportPresence()) {
            response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_CI_VALIDATE_EXPORT_PRESENT;
        }
    }

    if (Request->pathLengthChars != 0U) {
        response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_REQUEST_PATH_PRESENT;
        KswordARKTrustCopyWideStringToFixedBuffer(
            response->ntPath,
            KSWORD_ARK_TRUST_PATH_MAX_CHARS,
            Request->path,
            Request->pathLengthChars);
    }

    if ((requestFlags & KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_FILE_SIGNING_LEVEL) != 0UL &&
        Request->pathLengthChars != 0U) {
        status = KswordARKTrustOpenFileForTrustQuery(
            Request->path,
            Request->pathLengthChars,
            requestFlags,
            &fileHandle);
        response->openStatus = status;
        if (!NT_SUCCESS(status)) {
            response->queryStatus = KSWORD_ARK_TRUST_STATUS_FILE_OPEN_FAILED;
            *BytesWrittenOut = sizeof(*response);
            return STATUS_SUCCESS;
        }

        response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_FILE_OPENED;
        status = KswordARKTrustReferenceFileObject(fileHandle, &fileObject);
        response->objectStatus = status;
        if (NT_SUCCESS(status)) {
            response->fileObjectAddress = (ULONG64)(ULONG_PTR)fileObject;
            response->fieldFlags |= KSWORD_ARK_TRUST_FIELD_FILE_OBJECT_PRESENT;
            status = KswordARKTrustQueryCachedSigningLevel(fileObject, response);
            response->signingLevelStatus = status;
            if (NT_SUCCESS(status)) {
                response->trustSource = KSWORD_ARK_TRUST_SOURCE_SE_CACHED_SIGNING_LEVEL;
            }
        }
    }

    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    if (fileHandle != NULL) {
        ZwClose(fileHandle);
    }

    if ((response->fieldFlags & KSWORD_ARK_TRUST_FIELD_SIGNING_LEVEL_PRESENT) != 0UL ||
        (response->fieldFlags & KSWORD_ARK_TRUST_FIELD_GLOBAL_CI_PRESENT) != 0UL) {
        response->queryStatus = KSWORD_ARK_TRUST_STATUS_OK;
    }
    else if ((requestFlags & KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_FILE_SIGNING_LEVEL) != 0UL &&
        Request->pathLengthChars != 0U) {
        response->queryStatus = KSWORD_ARK_TRUST_STATUS_SIGNING_LEVEL_UNAVAILABLE;
    }
    else if ((requestFlags & KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_GLOBAL_CI) != 0UL) {
        response->queryStatus = KSWORD_ARK_TRUST_STATUS_CI_UNAVAILABLE;
    }
    else {
        response->queryStatus = KSWORD_ARK_TRUST_STATUS_INVALID_REQUEST;
    }

    if ((response->queryStatus == KSWORD_ARK_TRUST_STATUS_OK) &&
        ((response->fieldFlags & KSWORD_ARK_TRUST_FIELD_AUTHENTICODE_DEFERRED_R3) != 0UL)) {
        response->queryStatus = KSWORD_ARK_TRUST_STATUS_PARTIAL;
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
