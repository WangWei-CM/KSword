/*++

Module Name:

    trust_query.c

Abstract:

    Phase-14 kernel Code Integrity and cached image trust diagnostics.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntimage.h>
#include <ntstrsafe.h>

#define KSWORD_ARK_TRUST_SYSTEM_CODEINTEGRITY_INFORMATION 103UL
#define KSWORD_ARK_TRUST_SYSTEM_SECUREBOOT_INFORMATION 145UL
#define KSWORD_ARK_TRUST_POOL_TAG 'tIsK'
#define KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION_CLASS 11UL
#define KSWORD_ARK_SIGNATURE_READ_CHUNK_BYTES 4096UL
#define KSWORD_ARK_SIGNATURE_MAX_ENUMERATED_ENTRIES 4096UL
#define KSWORD_ARK_SIGNATURE_FNV1A64_OFFSET 14695981039346656037ULL
#define KSWORD_ARK_SIGNATURE_FNV1A64_PRIME 1099511628211ULL

#ifndef STATUS_INVALID_IMAGE_NOT_MZ
#define STATUS_INVALID_IMAGE_NOT_MZ ((NTSTATUS)0xC000012FUL)
#endif

typedef struct _KSWORD_ARK_SIGNATURE_WIN_CERTIFICATE_HEADER
{
    ULONG Length;
    USHORT Revision;
    USHORT CertificateType;
} KSWORD_ARK_SIGNATURE_WIN_CERTIFICATE_HEADER;

typedef struct _KSWORD_ARK_TRUST_SYSTEM_MODULE_ENTRY
{
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} KSWORD_ARK_TRUST_SYSTEM_MODULE_ENTRY;

typedef struct _KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSWORD_ARK_TRUST_SYSTEM_MODULE_ENTRY Modules[1];
} KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION;

static const UCHAR g_KswordArkNestedSignatureOidDer[] = {
    0x06U, 0x0AU, 0x2BU, 0x06U, 0x01U, 0x04U,
    0x01U, 0x82U, 0x37U, 0x02U, 0x04U, 0x01U
};


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
    _In_ BOOLEAN ReadFileData,
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
    ReadFileData - TRUE 时同时申请 FILE_READ_DATA，供证书表直接读取。
    FileHandleOut - 接收内核句柄，调用方负责 ZwClose。

Return Value:

    ZwCreateFile 返回的 NTSTATUS。

--*/
{
    UNICODE_STRING targetPath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    ULONG createOptions = 0UL;
    ULONG shareAccess = FILE_SHARE_READ | FILE_SHARE_DELETE;
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
    if (!ReadFileData) {
        shareAccess |= FILE_SHARE_WRITE;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        FileHandleOut,
        (ReadFileData ? FILE_READ_DATA : 0UL) |
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        shareAccess,
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

static NTSTATUS
KswordARKTrustQueryFileSize(
    _In_ HANDLE FileHandle,
    _Out_ ULONGLONG* FileSizeOut
    )
{
    FILE_STANDARD_INFORMATION standardInformation;
    IO_STATUS_BLOCK ioStatusBlock;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileHandle == NULL || FileSizeOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileSizeOut = 0ULL;
    RtlZeroMemory(&standardInformation, sizeof(standardInformation));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwQueryInformationFile(
        FileHandle,
        &ioStatusBlock,
        &standardInformation,
        sizeof(standardInformation),
        FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (standardInformation.EndOfFile.QuadPart < 0) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    *FileSizeOut = (ULONGLONG)standardInformation.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKTrustReadFileAt(
    _In_ HANDLE FileHandle,
    _In_ ULONGLONG FileOffset,
    _Out_writes_bytes_(BufferBytes) PVOID Buffer,
    _In_ ULONG BufferBytes
    )
{
    LARGE_INTEGER byteOffset;
    IO_STATUS_BLOCK ioStatusBlock;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileHandle == NULL || Buffer == NULL || BufferBytes == 0UL ||
        FileOffset > 0x7FFFFFFFFFFFFFFFULL) {
        return STATUS_INVALID_PARAMETER;
    }
    byteOffset.QuadPart = (LONGLONG)FileOffset;
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwReadFile(
        FileHandle,
        NULL,
        NULL,
        NULL,
        &ioStatusBlock,
        Buffer,
        BufferBytes,
        &byteOffset,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (ioStatusBlock.Information != BufferBytes) {
        return STATUS_END_OF_FILE;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKTrustIsKnownCertificateType(
    _In_ USHORT CertificateType
    )
{
    return CertificateType == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_TYPE_X509 ||
        CertificateType == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_TYPE_PKCS_SIGNED_DATA ||
        CertificateType == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_TYPE_RESERVED_1 ||
        CertificateType == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_TYPE_TS_STACK_SIGNED ||
        CertificateType == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_TYPE_PKCS1_SIGN;
}

static NTSTATUS
KswordARKTrustScanCertificateContent(
    _In_ HANDLE FileHandle,
    _In_ ULONGLONG ContentOffset,
    _In_ ULONG ContentBytes,
    _Out_writes_bytes_(ScratchBytes) PUCHAR Scratch,
    _In_ ULONG ScratchBytes,
    _Out_ ULONG* NestedSignatureCountOut,
    _Out_ ULONGLONG* ContentHashOut,
    _Out_ ULONG* BytesScannedOut,
    _Out_ UCHAR* FirstByteOut
    )
{
    const ULONG patternBytes = (ULONG)sizeof(g_KswordArkNestedSignatureOidDer);
    ULONG remainingBytes = ContentBytes;
    ULONG consumedBytes = 0UL;
    ULONG carryBytes = 0UL;
    ULONG nestedCount = 0UL;
    ULONGLONG hashValue = KSWORD_ARK_SIGNATURE_FNV1A64_OFFSET;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileHandle == NULL || Scratch == NULL ||
        ScratchBytes < (KSWORD_ARK_SIGNATURE_READ_CHUNK_BYTES + patternBytes) ||
        NestedSignatureCountOut == NULL || ContentHashOut == NULL ||
        BytesScannedOut == NULL || FirstByteOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *NestedSignatureCountOut = 0UL;
    *ContentHashOut = hashValue;
    *BytesScannedOut = 0UL;
    *FirstByteOut = 0U;

    while (remainingBytes != 0UL) {
        ULONG readBytes = remainingBytes;
        ULONG totalBytes = 0UL;
        ULONG index = 0UL;
        if (readBytes > KSWORD_ARK_SIGNATURE_READ_CHUNK_BYTES) {
            readBytes = KSWORD_ARK_SIGNATURE_READ_CHUNK_BYTES;
        }
        status = KswordARKTrustReadFileAt(
            FileHandle,
            ContentOffset + consumedBytes,
            Scratch + carryBytes,
            readBytes);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (consumedBytes == 0UL && readBytes != 0UL) {
            *FirstByteOut = Scratch[carryBytes];
        }
        for (index = 0UL; index < readBytes; ++index) {
            hashValue ^= Scratch[carryBytes + index];
            hashValue *= KSWORD_ARK_SIGNATURE_FNV1A64_PRIME;
        }
        totalBytes = carryBytes + readBytes;
        if (totalBytes >= patternBytes) {
            for (index = 0UL; index <= (totalBytes - patternBytes); ++index) {
                if (RtlCompareMemory(
                    Scratch + index,
                    g_KswordArkNestedSignatureOidDer,
                    patternBytes) == patternBytes) {
                    nestedCount += 1UL;
                }
            }
        }
        carryBytes = (totalBytes < (patternBytes - 1UL)) ? totalBytes : (patternBytes - 1UL);
        if (carryBytes != 0UL) {
            RtlMoveMemory(Scratch, Scratch + totalBytes - carryBytes, carryBytes);
        }
        consumedBytes += readBytes;
        remainingBytes -= readBytes;
    }

    *NestedSignatureCountOut = nestedCount;
    *ContentHashOut = hashValue;
    *BytesScannedOut = consumedBytes;
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKTrustCertificatePaddingNonzero(
    _In_ HANDLE FileHandle,
    _In_ ULONGLONG PaddingOffset,
    _In_ ULONG PaddingBytes,
    _Out_ NTSTATUS* ReadStatusOut
    )
{
    UCHAR padding[8] = { 0 };
    ULONG index = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ReadStatusOut == NULL) {
        return FALSE;
    }
    *ReadStatusOut = STATUS_SUCCESS;
    if (PaddingBytes == 0UL) {
        return FALSE;
    }
    if (PaddingBytes > sizeof(padding)) {
        *ReadStatusOut = STATUS_INVALID_PARAMETER;
        return FALSE;
    }
    status = KswordARKTrustReadFileAt(FileHandle, PaddingOffset, padding, PaddingBytes);
    *ReadStatusOut = status;
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }
    for (index = 0UL; index < PaddingBytes; ++index) {
        if (padding[index] != 0U) {
            return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS
KswordARKTrustEnumerateCertificateTable(
    _In_ HANDLE FileHandle,
    _Inout_ KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE* Response
    )
{
    PUCHAR scratch = NULL;
    ULONGLONG cursor = Response->certificateTableOffset;
    ULONGLONG remaining = Response->certificateTableSize;
    ULONG totalScannedBytes = 0UL;
    NTSTATUS resultStatus = STATUS_SUCCESS;

    scratch = (PUCHAR)KswordARKTrustAllocateNonPaged(
        KSWORD_ARK_SIGNATURE_READ_CHUNK_BYTES + sizeof(g_KswordArkNestedSignatureOidDer));
    if (scratch == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (remaining != 0ULL) {
        KSWORD_ARK_SIGNATURE_WIN_CERTIFICATE_HEADER certificateHeader;
        KSWORD_ARK_IMAGE_SIGNATURE_CERTIFICATE_ENTRY localEntry;
        KSWORD_ARK_IMAGE_SIGNATURE_CERTIFICATE_ENTRY* entry = &localEntry;
        ULONGLONG alignedLength = 0ULL;
        ULONG contentBytes = 0UL;
        ULONG scanBytes = 0UL;
        ULONG scanBudget = 0UL;
        ULONG paddingBytes = 0UL;
        UCHAR firstContentByte = 0U;
        NTSTATUS status = STATUS_SUCCESS;

        if (Response->certificateCount >= KSWORD_ARK_SIGNATURE_MAX_ENUMERATED_ENTRIES) {
            Response->structuralFlags |=
                KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_OUTPUT_TRUNCATED |
                KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_SCAN_LIMIT_REACHED;
            resultStatus = STATUS_BUFFER_OVERFLOW;
            break;
        }
        if (remaining < sizeof(certificateHeader)) {
            Response->structuralFlags |=
                KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_HEADER_TRUNCATED |
                KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_TRAILING_BYTES;
            resultStatus = STATUS_FILE_CORRUPT_ERROR;
            break;
        }

        RtlZeroMemory(&certificateHeader, sizeof(certificateHeader));
        status = KswordARKTrustReadFileAt(
            FileHandle,
            cursor,
            &certificateHeader,
            sizeof(certificateHeader));
        if (!NT_SUCCESS(status)) {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERTIFICATE_READ_FAILED;
            resultStatus = status;
            break;
        }
        if (certificateHeader.Length < sizeof(certificateHeader)) {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_LENGTH_INVALID;
            resultStatus = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        alignedLength = ((ULONGLONG)certificateHeader.Length + 7ULL) & ~7ULL;
        if (alignedLength > 0xFFFFFFFFULL ||
            certificateHeader.Length > remaining || alignedLength > remaining) {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_RANGE_INVALID;
            resultStatus = STATUS_FILE_CORRUPT_ERROR;
            break;
        }

        RtlZeroMemory(&localEntry, sizeof(localEntry));
        if (Response->returnedCertificateCount < KSWORD_ARK_IMAGE_SIGNATURE_MAX_ENTRIES) {
            entry = &Response->certificates[Response->returnedCertificateCount];
            Response->returnedCertificateCount += 1UL;
        }
        else {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_OUTPUT_TRUNCATED;
        }
        RtlZeroMemory(entry, sizeof(*entry));
        entry->fileOffset = cursor;
        entry->length = certificateHeader.Length;
        entry->alignedLength = (ULONG)alignedLength;
        entry->revision = certificateHeader.Revision;
        entry->certificateType = certificateHeader.CertificateType;
        entry->readStatus = STATUS_NOT_SUPPORTED;
        if ((certificateHeader.Length & 7UL) == 0UL) {
            entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_LENGTH_ALIGNED;
        }
        if (certificateHeader.Revision == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_REVISION_1_0 ||
            certificateHeader.Revision == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_REVISION_2_0) {
            entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_REVISION_VALID;
        }
        else {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_UNKNOWN_REVISION;
        }
        if (!KswordARKTrustIsKnownCertificateType(certificateHeader.CertificateType)) {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_UNKNOWN_TYPE;
        }
        if (certificateHeader.CertificateType == KSWORD_ARK_IMAGE_SIGNATURE_WIN_CERT_TYPE_PKCS_SIGNED_DATA) {
            entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_PKCS_SIGNED_DATA;
            Response->pkcs7CertificateCount += 1UL;
        }

        contentBytes = certificateHeader.Length - (ULONG)sizeof(certificateHeader);
        scanBudget = (totalScannedBytes < KSWORD_ARK_IMAGE_SIGNATURE_MAX_SCAN_BYTES)
            ? (KSWORD_ARK_IMAGE_SIGNATURE_MAX_SCAN_BYTES - totalScannedBytes)
            : 0UL;
        scanBytes = (contentBytes < scanBudget) ? contentBytes : scanBudget;
        if (scanBytes != 0UL) {
            status = KswordARKTrustScanCertificateContent(
                FileHandle,
                cursor + sizeof(certificateHeader),
                scanBytes,
                scratch,
                KSWORD_ARK_SIGNATURE_READ_CHUNK_BYTES + (ULONG)sizeof(g_KswordArkNestedSignatureOidDer),
                &entry->nestedSignatureCount,
                &entry->contentHashFnv1a64,
                &entry->contentBytesScanned,
                &firstContentByte);
            entry->readStatus = status;
            if (!NT_SUCCESS(status)) {
                Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERTIFICATE_READ_FAILED;
                resultStatus = status;
                break;
            }
            entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_CONTENT_READ;
            if (firstContentByte == 0x30U) {
                entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_DER_SEQUENCE;
            }
            if (entry->nestedSignatureCount != 0UL) {
                entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_NESTED_SIGNATURE_OID;
                Response->nestedSignatureCount += entry->nestedSignatureCount;
            }
            totalScannedBytes += entry->contentBytesScanned;
        }
        else if (contentBytes == 0UL) {
            entry->readStatus = STATUS_SUCCESS;
            entry->contentHashFnv1a64 = KSWORD_ARK_SIGNATURE_FNV1A64_OFFSET;
            entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_CONTENT_READ;
        }
        if (scanBytes < contentBytes) {
            entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_SCAN_LIMITED;
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_SCAN_LIMIT_REACHED;
        }

        paddingBytes = (ULONG)alignedLength - certificateHeader.Length;
        if (paddingBytes != 0UL) {
            NTSTATUS paddingStatus = STATUS_SUCCESS;
            if (KswordARKTrustCertificatePaddingNonzero(
                FileHandle,
                cursor + certificateHeader.Length,
                paddingBytes,
                &paddingStatus)) {
                entry->flags |= KSWORD_ARK_IMAGE_SIGNATURE_ENTRY_PADDING_NONZERO;
                Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_PADDING_NONZERO;
            }
            if (!NT_SUCCESS(paddingStatus)) {
                entry->readStatus = paddingStatus;
                Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERTIFICATE_READ_FAILED;
                resultStatus = paddingStatus;
                break;
            }
        }

        Response->certificateCount += 1UL;
        cursor += alignedLength;
        remaining -= alignedLength;
    }

    Response->certificateBytesScanned = totalScannedBytes;
    Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_CERTIFICATES_ENUMERATED;
    if (Response->nestedSignatureCount != 0UL) {
        Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_NESTED_SIGNATURE_PRESENT;
    }
    if (Response->pkcs7CertificateCount > 1UL) {
        Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_MULTIPLE_PKCS7_ENTRIES;
    }
    ExFreePoolWithTag(scratch, KSWORD_ARK_TRUST_POOL_TAG);
    return resultStatus;
}

static NTSTATUS
KswordARKTrustParsePeCertificateTable(
    _In_ HANDLE FileHandle,
    _Inout_ KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE* Response
    )
{
    IMAGE_DOS_HEADER dosHeader;
    IMAGE_FILE_HEADER fileHeader;
    IMAGE_DATA_DIRECTORY securityDirectory;
    ULONGLONG optionalHeaderOffset = 0ULL;
    ULONG peSignature = 0UL;
    USHORT optionalMagic = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(&dosHeader, sizeof(dosHeader));
    status = KswordARKTrustReadFileAt(FileHandle, 0ULL, &dosHeader, sizeof(dosHeader));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        return STATUS_INVALID_IMAGE_NOT_MZ;
    }
    Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_DOS_HEADER;
    if (dosHeader.e_lfanew <= 0 ||
        (ULONGLONG)dosHeader.e_lfanew > Response->fileSize ||
        (Response->fileSize - (ULONGLONG)dosHeader.e_lfanew) <
            (sizeof(peSignature) + sizeof(fileHeader) + sizeof(optionalMagic))) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    Response->peHeaderOffset = (ULONGLONG)dosHeader.e_lfanew;
    status = KswordARKTrustReadFileAt(
        FileHandle,
        Response->peHeaderOffset,
        &peSignature,
        sizeof(peSignature));
    if (!NT_SUCCESS(status) || peSignature != IMAGE_NT_SIGNATURE) {
        return NT_SUCCESS(status) ? STATUS_INVALID_IMAGE_FORMAT : status;
    }
    RtlZeroMemory(&fileHeader, sizeof(fileHeader));
    status = KswordARKTrustReadFileAt(
        FileHandle,
        Response->peHeaderOffset + sizeof(peSignature),
        &fileHeader,
        sizeof(fileHeader));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    optionalHeaderOffset = Response->peHeaderOffset + sizeof(peSignature) + sizeof(fileHeader);
    if (optionalHeaderOffset > Response->fileSize ||
        fileHeader.SizeOfOptionalHeader > (Response->fileSize - optionalHeaderOffset)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    status = KswordARKTrustReadFileAt(
        FileHandle,
        optionalHeaderOffset,
        &optionalMagic,
        sizeof(optionalMagic));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&securityDirectory, sizeof(securityDirectory));
    Response->peMachine = fileHeader.Machine;
    Response->optionalHeaderMagic = optionalMagic;
    if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 optionalHeader32;
        ULONG requiredBytes = FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32, DataDirectory) +
            ((IMAGE_DIRECTORY_ENTRY_SECURITY + 1UL) * sizeof(IMAGE_DATA_DIRECTORY));
        ULONG readBytes = fileHeader.SizeOfOptionalHeader;
        if (fileHeader.SizeOfOptionalHeader < requiredBytes) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        if (readBytes > sizeof(optionalHeader32)) {
            readBytes = sizeof(optionalHeader32);
        }
        RtlZeroMemory(&optionalHeader32, sizeof(optionalHeader32));
        status = KswordARKTrustReadFileAt(FileHandle, optionalHeaderOffset, &optionalHeader32, readBytes);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Response->sizeOfHeaders = optionalHeader32.SizeOfHeaders;
        if (optionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_SECURITY) {
            Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_NT_HEADERS;
            Response->certificateStatus = STATUS_NOT_FOUND;
            return STATUS_SUCCESS;
        }
        securityDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    }
    else if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 optionalHeader64;
        ULONG requiredBytes = FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
            ((IMAGE_DIRECTORY_ENTRY_SECURITY + 1UL) * sizeof(IMAGE_DATA_DIRECTORY));
        ULONG readBytes = fileHeader.SizeOfOptionalHeader;
        if (fileHeader.SizeOfOptionalHeader < requiredBytes) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        if (readBytes > sizeof(optionalHeader64)) {
            readBytes = sizeof(optionalHeader64);
        }
        RtlZeroMemory(&optionalHeader64, sizeof(optionalHeader64));
        status = KswordARKTrustReadFileAt(FileHandle, optionalHeaderOffset, &optionalHeader64, readBytes);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Response->sizeOfHeaders = optionalHeader64.SizeOfHeaders;
        if (optionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_SECURITY) {
            Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_NT_HEADERS;
            Response->certificateStatus = STATUS_NOT_FOUND;
            return STATUS_SUCCESS;
        }
        securityDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    }
    else {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    Response->fieldFlags |=
        KSWORD_ARK_IMAGE_SIGNATURE_FIELD_NT_HEADERS |
        KSWORD_ARK_IMAGE_SIGNATURE_FIELD_SECURITY_DIRECTORY;
    Response->certificateTableOffset = securityDirectory.VirtualAddress;
    Response->certificateTableSize = securityDirectory.Size;
    if (securityDirectory.VirtualAddress == 0UL && securityDirectory.Size == 0UL) {
        Response->certificateStatus = STATUS_NOT_FOUND;
        return STATUS_SUCCESS;
    }
    if (securityDirectory.VirtualAddress == 0UL || securityDirectory.Size == 0UL) {
        Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_OUT_OF_RANGE;
        Response->certificateStatus = STATUS_FILE_CORRUPT_ERROR;
        return STATUS_FILE_CORRUPT_ERROR;
    }
    if ((securityDirectory.VirtualAddress & 7UL) != 0UL) {
        Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_UNALIGNED;
    }
    if (Response->sizeOfHeaders != 0UL && securityDirectory.VirtualAddress < Response->sizeOfHeaders) {
        Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_OVERLAPS_HEADERS;
    }
    if ((ULONGLONG)securityDirectory.VirtualAddress > Response->fileSize ||
        (ULONGLONG)securityDirectory.Size >
            (Response->fileSize - (ULONGLONG)securityDirectory.VirtualAddress)) {
        Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_OUT_OF_RANGE;
        Response->certificateStatus = STATUS_FILE_CORRUPT_ERROR;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_CERTIFICATE_TABLE;
    status = KswordARKTrustEnumerateCertificateTable(FileHandle, Response);
    Response->certificateStatus = status;
    return status;
}

static BOOLEAN
KswordARKTrustLoadedModuleNameMatches(
    _In_reads_(RequestPathChars) PCWSTR RequestPath,
    _In_ USHORT RequestPathChars,
    _In_ const KSWORD_ARK_TRUST_SYSTEM_MODULE_ENTRY* ModuleEntry
    )
{
    ANSI_STRING ansiName;
    UNICODE_STRING moduleName;
    UNICODE_STRING requestName;
    CHAR boundedName[257] = { 0 };
    USHORT requestNameStart = RequestPathChars;
    ULONG sourceOffset = 0UL;
    ULONG copyBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN matches = FALSE;

    if (RequestPath == NULL || RequestPathChars == 0U || ModuleEntry == NULL) {
        return FALSE;
    }
    while (requestNameStart != 0U) {
        WCHAR character = RequestPath[requestNameStart - 1U];
        if (character == L'\\' || character == L'/') {
            break;
        }
        requestNameStart -= 1U;
    }
    sourceOffset = (ModuleEntry->OffsetToFileName < sizeof(ModuleEntry->FullPathName))
        ? ModuleEntry->OffsetToFileName
        : 0UL;
    while (sourceOffset + copyBytes < sizeof(ModuleEntry->FullPathName) &&
        copyBytes < (sizeof(boundedName) - 1UL) &&
        ModuleEntry->FullPathName[sourceOffset + copyBytes] != '\0') {
        boundedName[copyBytes] = (CHAR)ModuleEntry->FullPathName[sourceOffset + copyBytes];
        copyBytes += 1UL;
    }
    if (copyBytes == 0UL) {
        return FALSE;
    }
    boundedName[copyBytes] = '\0';
    RtlInitAnsiString(&ansiName, boundedName);
    RtlZeroMemory(&moduleName, sizeof(moduleName));
    status = RtlAnsiStringToUnicodeString(&moduleName, &ansiName, TRUE);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }
    requestName.Buffer = (PWCH)(RequestPath + requestNameStart);
    requestName.Length = (USHORT)((RequestPathChars - requestNameStart) * sizeof(WCHAR));
    requestName.MaximumLength = requestName.Length;
    matches = RtlEqualUnicodeString(&requestName, &moduleName, TRUE);
    RtlFreeUnicodeString(&moduleName);
    return matches;
}

static NTSTATUS
KswordARKTrustMatchLoadedModule(
    _In_ const KSWORD_ARK_QUERY_IMAGE_SIGNATURE_REQUEST* Request,
    _Inout_ KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE* Response
    )
{
    KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION* moduleInformation = NULL;
    ULONG requiredBytes = 0UL;
    ULONG index = 0UL;
    ULONG boundedCount = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    status = ZwQuerySystemInformation(
        KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION_CLASS,
        NULL,
        0UL,
        &requiredBytes);
    if (requiredBytes < (ULONG)FIELD_OFFSET(KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION, Modules) ||
        requiredBytes > (16UL * 1024UL * 1024UL)) {
        return NT_SUCCESS(status) ? STATUS_INFO_LENGTH_MISMATCH : status;
    }
    moduleInformation = (KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION*)
        KswordARKTrustAllocateNonPaged(requiredBytes);
    if (moduleInformation == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(moduleInformation, requiredBytes);
    status = ZwQuerySystemInformation(
        KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION_CLASS,
        moduleInformation,
        requiredBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(moduleInformation, KSWORD_ARK_TRUST_POOL_TAG);
        return status;
    }
    boundedCount = (requiredBytes - FIELD_OFFSET(KSWORD_ARK_TRUST_SYSTEM_MODULE_INFORMATION, Modules)) /
        sizeof(KSWORD_ARK_TRUST_SYSTEM_MODULE_ENTRY);
    if (boundedCount > moduleInformation->NumberOfModules) {
        boundedCount = moduleInformation->NumberOfModules;
    }
    status = STATUS_NOT_FOUND;
    for (index = 0UL; index < boundedCount; ++index) {
        const KSWORD_ARK_TRUST_SYSTEM_MODULE_ENTRY* moduleEntry = &moduleInformation->Modules[index];
        if ((ULONGLONG)(ULONG_PTR)moduleEntry->ImageBase != Request->expectedModuleBase) {
            continue;
        }
        Response->matchedModuleBase = (ULONGLONG)(ULONG_PTR)moduleEntry->ImageBase;
        Response->matchedModuleSize = moduleEntry->ImageSize;
        Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_LOADED_MODULE;
        if (KswordARKTrustLoadedModuleNameMatches(
            Request->path,
            Request->pathLengthChars,
            moduleEntry)) {
            Response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_LOADED_MODULE_NAME_MATCH;
        }
        else {
            Response->structuralFlags |= KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_LOADED_NAME_MISMATCH;
            status = STATUS_DATA_ERROR;
        }
        if ((Response->fieldFlags & KSWORD_ARK_IMAGE_SIGNATURE_FIELD_LOADED_MODULE_NAME_MATCH) != 0UL) {
            status = STATUS_SUCCESS;
        }
        break;
    }
    ExFreePoolWithTag(moduleInformation, KSWORD_ARK_TRUST_POOL_TAG);
    return status;
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
            FALSE,
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

NTSTATUS
KswordARKDriverQueryImageSignature(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_IMAGE_SIGNATURE_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Read PE certificate-table evidence and cached Code Integrity signing state
    entirely from kernel mode. The structural result does not claim that a
    certificate chain is trusted; the cached signing level remains a separate
    field group.

--*/
{
    KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE* response = NULL;
    KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE cachedSigning;
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    ULONG requestFlags = 0UL;
    ULONG openFlags = 0UL;
    BOOLEAN anySuccess = FALSE;
    BOOLEAN anyFailure = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Request->pathLengthChars == 0U ||
        Request->pathLengthChars >= KSWORD_ARK_TRUST_PATH_MAX_CHARS ||
        Request->path[Request->pathLengthChars] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    requestFlags = (Request->flags == 0UL)
        ? KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_DEFAULT
        : Request->flags;
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_IMAGE_SIGNATURE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->requestFlags = requestFlags;
    response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_UNAVAILABLE;
    response->openStatus = STATUS_NOT_SUPPORTED;
    response->fileSizeStatus = STATUS_NOT_SUPPORTED;
    response->objectStatus = STATUS_NOT_SUPPORTED;
    response->parseStatus = STATUS_NOT_SUPPORTED;
    response->certificateStatus = STATUS_NOT_SUPPORTED;
    response->signingLevelStatus = STATUS_NOT_SUPPORTED;
    response->loadedModuleStatus = STATUS_NOT_SUPPORTED;
    response->signingLevel = KSWORD_ARK_SIGNING_LEVEL_UNCHECKED;
    response->expectedModuleBase = Request->expectedModuleBase;
    response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_REQUEST_PATH;
    KswordARKTrustCopyWideStringToFixedBuffer(
        response->ntPath,
        KSWORD_ARK_TRUST_PATH_MAX_CHARS,
        Request->path,
        Request->pathLengthChars);

    if ((requestFlags & KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_MATCH_LOADED_MODULE) != 0UL) {
        response->loadedModuleStatus = KswordARKTrustMatchLoadedModule(Request, response);
        if (NT_SUCCESS(response->loadedModuleStatus)) {
            anySuccess = TRUE;
        }
        else {
            anyFailure = TRUE;
        }
    }

    if ((requestFlags & KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_OPEN_REPARSE_POINT) != 0UL) {
        openFlags |= KSWORD_ARK_TRUST_QUERY_FLAG_OPEN_REPARSE_POINT;
    }
    status = KswordARKTrustOpenFileForTrustQuery(
        Request->path,
        Request->pathLengthChars,
        openFlags,
        TRUE,
        &fileHandle);
    response->openStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_FILE_OPEN_FAILED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_FILE_OPENED;

    response->fileSizeStatus = KswordARKTrustQueryFileSize(fileHandle, &response->fileSize);
    if (NT_SUCCESS(response->fileSizeStatus)) {
        response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_FILE_SIZE;
    }

    response->objectStatus = KswordARKTrustReferenceFileObject(fileHandle, &fileObject);
    if (NT_SUCCESS(response->objectStatus)) {
        /* Keep fileObjectAddress reserved to avoid adding a new kernel-pointer leak. */
        response->fileObjectAddress = 0ULL;
    }

    if ((requestFlags & KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_INCLUDE_PE_CERTIFICATE_TABLE) != 0UL) {
        if (NT_SUCCESS(response->fileSizeStatus)) {
            response->parseStatus = KswordARKTrustParsePeCertificateTable(fileHandle, response);
        }
        else {
            response->parseStatus = response->fileSizeStatus;
        }
        if (NT_SUCCESS(response->parseStatus)) {
            anySuccess = TRUE;
        }
        else {
            anyFailure = TRUE;
        }
    }

    if ((requestFlags & KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_INCLUDE_CACHED_SIGNING_LEVEL) != 0UL) {
        if (fileObject != NULL) {
            RtlZeroMemory(&cachedSigning, sizeof(cachedSigning));
            response->signingLevelStatus =
                KswordARKTrustQueryCachedSigningLevel(fileObject, &cachedSigning);
            if (NT_SUCCESS(response->signingLevelStatus)) {
                response->signingLevel = cachedSigning.signingLevel;
                response->signingLevelFlags = cachedSigning.signingLevelFlags;
                response->thumbprintAlgorithm = cachedSigning.thumbprintAlgorithm;
                response->thumbprintSize = cachedSigning.thumbprintSize;
                if (response->thumbprintSize > sizeof(response->thumbprint)) {
                    response->thumbprintSize = sizeof(response->thumbprint);
                }
                if (response->thumbprintSize != 0UL) {
                    RtlCopyMemory(
                        response->thumbprint,
                        cachedSigning.thumbprint,
                        response->thumbprintSize);
                    response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_THUMBPRINT;
                }
                response->fieldFlags |= KSWORD_ARK_IMAGE_SIGNATURE_FIELD_SIGNING_LEVEL;
                anySuccess = TRUE;
            }
            else {
                anyFailure = TRUE;
            }
        }
        else {
            response->signingLevelStatus = response->objectStatus;
            anyFailure = TRUE;
        }
    }

    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    ZwClose(fileHandle);

    if ((response->structuralFlags &
        (KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_UNALIGNED |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_OUT_OF_RANGE |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_HEADER_TRUNCATED |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_LENGTH_INVALID |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_RANGE_INVALID |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_UNKNOWN_REVISION |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_UNKNOWN_TYPE |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_ENTRY_OUTPUT_TRUNCATED |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_TRAILING_BYTES |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_PADDING_NONZERO |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_SCAN_LIMIT_REACHED |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERTIFICATE_READ_FAILED |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_LOADED_NAME_MISMATCH |
         KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_CERT_TABLE_OVERLAPS_HEADERS)) != 0UL) {
        anyFailure = TRUE;
    }

    if ((requestFlags & KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_INCLUDE_PE_CERTIFICATE_TABLE) != 0UL &&
        response->parseStatus == STATUS_INVALID_IMAGE_NOT_MZ) {
        response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_NOT_PE;
    }
    else if ((requestFlags & KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_INCLUDE_PE_CERTIFICATE_TABLE) != 0UL &&
        !NT_SUCCESS(response->parseStatus)) {
        response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_MALFORMED_PE;
    }
    else if (anySuccess && anyFailure) {
        response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_PARTIAL;
    }
    else if (anySuccess) {
        response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_OK;
    }
    else {
        response->queryStatus = KSWORD_ARK_IMAGE_SIGNATURE_STATUS_UNAVAILABLE;
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
