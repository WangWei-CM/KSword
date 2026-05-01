/*++

Module Name:

    file_actions.c

Abstract:

    This file contains kernel file operations, including deletion and
    read-only Phase-10 file information queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntstrsafe.h>

#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000UL
#endif

#ifndef FILE_DISPOSITION_DELETE
#define FILE_DISPOSITION_DELETE 0x00000001UL
#endif

#ifndef FILE_DISPOSITION_POSIX_SEMANTICS
#define FILE_DISPOSITION_POSIX_SEMANTICS 0x00000002UL
#endif

#ifndef FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK
#define FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK 0x00000004UL
#endif

#ifndef FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE 0x00000010UL
#endif

// Use the documented value 64 for FileDispositionInformationEx to support
// older WDK headers where the enum constant may be missing.
#define KSWORD_FILE_DISPOSITION_INFORMATION_EX_CLASS_VALUE ((FILE_INFORMATION_CLASS)64)

typedef struct _KSWORD_FILE_DISPOSITION_INFORMATION_EX
{
    ULONG Flags;
} KSWORD_FILE_DISPOSITION_INFORMATION_EX, *PKSWORD_FILE_DISPOSITION_INFORMATION_EX;

typedef PVOID
(NTAPI* KSWORD_ARK_FILE_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

static PVOID
KswordARKDriverFileAllocateNonPaged(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    分配文件查询临时缓冲。中文说明：优先动态解析 ExAllocatePool2，旧系统或
    旧 WDK 兼容路径回退到 ExAllocatePoolWithTag，避免驱动因为导入缺失加载失败。

Arguments:

    BufferBytes - 需要分配的字节数。

Return Value:

    非分页池指针，失败返回 NULL。

--*/
{
    static volatile LONG allocatorResolved = 0;
    static KSWORD_ARK_FILE_EX_ALLOCATE_POOL2_FN exAllocatePool2Fn = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&allocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        exAllocatePool2Fn = (KSWORD_ARK_FILE_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (exAllocatePool2Fn != NULL) {
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, 'fOsK');
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, 'fOsK');
#pragma warning(pop)
}

static VOID
KswordARKDriverCopyWideStringToFixedBuffer(
    _Out_writes_(DestinationChars) PWSTR Destination,
    _In_ USHORT DestinationChars,
    _In_reads_opt_(SourceChars) PCWSTR Source,
    _In_ USHORT SourceChars
    )
/*++

Routine Description:

    把内核 UNICODE_STRING 或请求路径复制到固定响应缓冲。中文说明：复制时
    始终保留 NUL 结尾，长度不足时截断，避免 R3 解析固定数组越界。

Arguments:

    Destination - 响应包中的固定宽字符数组。
    DestinationChars - Destination 的元素数量。
    Source - 来源宽字符指针，可为空。
    SourceChars - 来源字符数，不包含 NUL。

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

static NTSTATUS
KswordARKDriverQueryFileObjectName(
    _In_ PFILE_OBJECT FileObject,
    _Out_writes_(ObjectNameChars) PWSTR ObjectName,
    _In_ USHORT ObjectNameChars
    )
/*++

Routine Description:

    查询 FILE_OBJECT 对象名。中文说明：采用 ObQueryNameString 两段式查询，
    第一段获取长度，第二段分配 NonPagedPoolNx 并复制到响应固定数组。

Arguments:

    FileObject - 已引用的文件对象。
    ObjectName - 响应包对象名数组。
    ObjectNameChars - ObjectName 数组长度。

Return Value:

    STATUS_SUCCESS 或 ObQueryNameString/内存分配错误。

--*/
{
    POBJECT_NAME_INFORMATION nameInfo = NULL;
    ULONG requiredBytes = 0UL;
    ULONG allocationBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ObjectName != NULL && ObjectNameChars > 0U) {
        ObjectName[0] = L'\0';
    }
    if (FileObject == NULL || ObjectName == NULL || ObjectNameChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ObQueryNameString(FileObject, NULL, 0UL, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH &&
        status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL &&
        !NT_SUCCESS(status)) {
        return status;
    }
    if (requiredBytes < sizeof(OBJECT_NAME_INFORMATION)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    allocationBytes = requiredBytes + sizeof(WCHAR);
    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKDriverFileAllocateNonPaged(allocationBytes);
    if (nameInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(nameInfo, allocationBytes);
    status = ObQueryNameString(FileObject, nameInfo, allocationBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        const USHORT sourceChars = (USHORT)(nameInfo->Name.Length / sizeof(WCHAR));
        KswordARKDriverCopyWideStringToFixedBuffer(
            ObjectName,
            ObjectNameChars,
            nameInfo->Name.Buffer,
            sourceChars);
    }

    ExFreePoolWithTag(nameInfo, 'fOsK');
    return status;
}

static NTSTATUS
KswordARKDriverOpenFileForQuery(
    _In_reads_(PathLengthChars) PCWSTR PathText,
    _In_ USHORT PathLengthChars,
    _In_ ULONG Flags,
    _Out_ HANDLE* FileHandleOut
    )
/*++

Routine Description:

    为只读文件信息查询打开目标路径。中文说明：打开权限只包含
    FILE_READ_ATTRIBUTES/SYNCHRONIZE，并允许 READ/WRITE/DELETE 共享，避免
    查询动作本身改变占用关系或阻塞被其它进程持有的文件。

Arguments:

    PathText - NT 路径，通常是 \??\C:\...。
    PathLengthChars - 路径字符数，不含 NUL。
    Flags - KSWORD_ARK_QUERY_FILE_INFO_FLAG_*。
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

    createOptions = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT;
    if ((Flags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_OPEN_REPARSE_POINT) != 0UL) {
        createOptions |= FILE_OPEN_REPARSE_POINT;
    }
    if ((Flags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_DIRECTORY) != 0UL) {
        createOptions |= FILE_DIRECTORY_FILE;
    }
    else {
        createOptions |= FILE_NON_DIRECTORY_FILE;
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

NTSTATUS
KswordARKDriverQueryFileInfo(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_FILE_INFO_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询文件基础信息。中文说明：本函数是 Phase-10 文件对象信息的 R0 后端，
    只做只读属性查询；对象地址和 SectionObjectPointer 仅用于 UI 诊断展示，
    不允许 R3 在后续 IOCTL 中把这些地址作为凭据传回。

Arguments:

    OutputBuffer - 响应包缓冲区。
    OutputBufferLength - 响应包缓冲区长度。
    Request - 请求包，包含 NT 路径和查询 flags。
    BytesWrittenOut - 接收 sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE)。

Return Value:

    STATUS_SUCCESS 表示响应包有效；失败细节写入 response->queryStatus。

--*/
{
    KSWORD_ARK_QUERY_FILE_INFO_RESPONSE* response = NULL;
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    FILE_BASIC_INFORMATION basicInformation;
    FILE_STANDARD_INFORMATION standardInformation;
    IO_STATUS_BLOCK ioStatusBlock;
    PSECTION_OBJECT_POINTERS sectionPointers = NULL;
    ULONG requestFlags = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->pathLengthChars == 0U ||
        Request->pathLengthChars >= KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS ||
        Request->path[Request->pathLengthChars] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&basicInformation, sizeof(basicInformation));
    RtlZeroMemory(&standardInformation, sizeof(standardInformation));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    response = (KSWORD_ARK_QUERY_FILE_INFO_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FILE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE;
    response->openStatus = STATUS_SUCCESS;
    response->basicStatus = STATUS_NOT_SUPPORTED;
    response->standardStatus = STATUS_NOT_SUPPORTED;
    response->objectStatus = STATUS_NOT_SUPPORTED;
    response->nameStatus = STATUS_NOT_SUPPORTED;
    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_REQUEST_PATH_PRESENT;
    KswordARKDriverCopyWideStringToFixedBuffer(
        response->ntPath,
        KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS,
        Request->path,
        Request->pathLengthChars);

    requestFlags = Request->flags;
    if (requestFlags == 0UL) {
        requestFlags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL;
    }

    status = KswordARKDriverOpenFileForQuery(
        Request->path,
        Request->pathLengthChars,
        requestFlags,
        &fileHandle);
    response->openStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OPEN_FAILED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatusBlock,
        &basicInformation,
        (ULONG)sizeof(basicInformation),
        FileBasicInformation);
    response->basicStatus = status;
    if (NT_SUCCESS(status)) {
        response->fileAttributes = basicInformation.FileAttributes;
        response->creationTime = basicInformation.CreationTime.QuadPart;
        response->lastAccessTime = basicInformation.LastAccessTime.QuadPart;
        response->lastWriteTime = basicInformation.LastWriteTime.QuadPart;
        response->changeTime = basicInformation.ChangeTime.QuadPart;
        response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_BASIC_PRESENT;
        if ((basicInformation.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0UL) {
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DIRECTORY;
        }
    }
    else {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_BASIC_FAILED;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatusBlock,
        &standardInformation,
        (ULONG)sizeof(standardInformation),
        FileStandardInformation);
    response->standardStatus = status;
    if (NT_SUCCESS(status)) {
        response->allocationSize = standardInformation.AllocationSize.QuadPart;
        response->endOfFile = standardInformation.EndOfFile.QuadPart;
        response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_STANDARD_PRESENT;
        if (standardInformation.Directory != FALSE) {
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DIRECTORY;
        }
    }
    else if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_STANDARD_FAILED;
    }

    if ((requestFlags & (KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME |
        KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS)) != 0UL) {
        status = ObReferenceObjectByHandle(
            fileHandle,
            0,
            NULL,
            KernelMode,
            (PVOID*)&fileObject,
            NULL);
        response->objectStatus = status;
        if (NT_SUCCESS(status)) {
            response->fileObjectAddress = (ULONG64)(ULONG_PTR)fileObject;
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_FILE_OBJECT_PRESENT;
        }
        else if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED;
        }
    }

    if (fileObject != NULL &&
        (requestFlags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME) != 0UL) {
        status = KswordARKDriverQueryFileObjectName(
            fileObject,
            response->objectName,
            KSWORD_ARK_FILE_INFO_OBJECT_NAME_MAX_CHARS);
        response->nameStatus = status;
        if (NT_SUCCESS(status)) {
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_OBJECT_NAME_PRESENT;
        }
        else if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_NAME_FAILED;
        }
    }

    if (fileObject != NULL &&
        (requestFlags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS) != 0UL) {
        __try {
            sectionPointers = fileObject->SectionObjectPointer;
            response->sectionObjectPointersAddress = (ULONG64)(ULONG_PTR)sectionPointers;
            if (sectionPointers != NULL) {
                response->dataSectionObjectAddress = (ULONG64)(ULONG_PTR)sectionPointers->DataSectionObject;
                response->imageSectionObjectAddress = (ULONG64)(ULONG_PTR)sectionPointers->ImageSectionObject;
                response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_SECTION_POINTERS_PRESENT;
                if (sectionPointers->DataSectionObject != NULL) {
                    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DATA_SECTION_PRESENT;
                }
                if (sectionPointers->ImageSectionObject != NULL) {
                    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_IMAGE_SECTION_PRESENT;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            response->objectStatus = GetExceptionCode();
            if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
                response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED;
            }
        }
    }

    if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OK;
    }
    else if (response->fieldFlags != 0UL &&
        response->queryStatus != KSWORD_ARK_FILE_INFO_STATUS_OK) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_PARTIAL;
    }

    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    ZwClose(fileHandle);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKDriverShouldRetryDeleteWithDispositionEx(
    _In_ NTSTATUS deleteStatus,
    _In_ BOOLEAN isDirectory
    )
/*++

Routine Description:

    Decide whether delete failure should trigger FileDispositionInformationEx
    fallback. This fallback is file-only and targets common "in-use" failures.

Arguments:

    deleteStatus - First delete attempt status.
    isDirectory - TRUE when target is directory.

Return Value:

    BOOLEAN

--*/
{
    if (isDirectory) {
        return FALSE;
    }

    switch (deleteStatus) {
    case STATUS_ACCESS_DENIED:
    case STATUS_SHARING_VIOLATION:
    case STATUS_CANNOT_DELETE:
    case STATUS_USER_MAPPED_FILE:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS
KswordARKDriverDeleteFileWithDispositionEx(
    _In_ HANDLE fileHandle
    )
/*++

Routine Description:

    Retry delete by FileDispositionInformationEx with stronger semantics:
    POSIX delete + force image section check + ignore readonly attribute.

Arguments:

    fileHandle - Open file handle.

Return Value:

    NTSTATUS

--*/
{
    KSWORD_FILE_DISPOSITION_INFORMATION_EX dispositionInformationEx;
    IO_STATUS_BLOCK ioStatusBlock;

    RtlZeroMemory(&dispositionInformationEx, sizeof(dispositionInformationEx));
    dispositionInformationEx.Flags =
        FILE_DISPOSITION_DELETE
        | FILE_DISPOSITION_POSIX_SEMANTICS
        | FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK
        | FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE;

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    return ZwSetInformationFile(
        fileHandle,
        &ioStatusBlock,
        &dispositionInformationEx,
        (ULONG)sizeof(dispositionInformationEx),
        KSWORD_FILE_DISPOSITION_INFORMATION_EX_CLASS_VALUE);
}

static NTSTATUS
KswordARKDriverNormalizeReadOnlyAttribute(
    _In_ HANDLE fileHandle
    )
/*++

Routine Description:

    Clear FILE_ATTRIBUTE_READONLY before delete so driver delete can handle
    read-only files without an extra user-mode retry.

Arguments:

    fileHandle - Open file or directory handle.

Return Value:

    NTSTATUS

--*/
{
    FILE_BASIC_INFORMATION basicInformation;
    IO_STATUS_BLOCK ioStatusBlock;
    NTSTATUS status;

    RtlZeroMemory(&basicInformation, sizeof(basicInformation));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatusBlock,
        &basicInformation,
        (ULONG)sizeof(basicInformation),
        FileBasicInformation);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if ((basicInformation.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0U) {
        return STATUS_SUCCESS;
    }

    basicInformation.FileAttributes &= ~((ULONG)FILE_ATTRIBUTE_READONLY);
    if (basicInformation.FileAttributes == 0U) {
        basicInformation.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    return ZwSetInformationFile(
        fileHandle,
        &ioStatusBlock,
        &basicInformation,
        (ULONG)sizeof(basicInformation),
        FileBasicInformation);
}

NTSTATUS
KswordARKDriverDeletePath(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory
    )
/*++

Routine Description:

    Delete a single NT path via ZwCreateFile + FileDispositionInformation.
    Directories must already be empty; recursive ordering is handled by R3.

Arguments:

    pathText - Target NT path, for example \??\C:\Temp\a.txt.
    pathLengthChars - Character length excluding trailing null.
    isDirectory - TRUE when target should be opened as directory.

Return Value:

    NTSTATUS

--*/
{
    UNICODE_STRING targetPath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    HANDLE fileHandle = NULL;
    FILE_DISPOSITION_INFORMATION dispositionInformation;
    ACCESS_MASK desiredAccess;
    ULONG createOptions;
    NTSTATUS status;
    NTSTATUS firstDeleteStatus = STATUS_SUCCESS;

    if (pathText == NULL || pathLengthChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&targetPath, pathText);
    if (targetPath.Length == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (targetPath.Length != (USHORT)(pathLengthChars * sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }

    InitializeObjectAttributes(
        &objectAttributes,
        &targetPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    desiredAccess = DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    createOptions = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT;
    if (isDirectory) {
        createOptions |= FILE_DIRECTORY_FILE;
    }
    else {
        createOptions |= FILE_NON_DIRECTORY_FILE;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        &fileHandle,
        desiredAccess,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        createOptions,
        NULL,
        0U);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDriverNormalizeReadOnlyAttribute(fileHandle);
    if (!NT_SUCCESS(status) && status != STATUS_INVALID_PARAMETER) {
        ZwClose(fileHandle);
        return status;
    }

    dispositionInformation.DeleteFile = TRUE;
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwSetInformationFile(
        fileHandle,
        &ioStatusBlock,
        &dispositionInformation,
        (ULONG)sizeof(dispositionInformation),
        FileDispositionInformation);
    firstDeleteStatus = status;

    if (!NT_SUCCESS(status) &&
        KswordARKDriverShouldRetryDeleteWithDispositionEx(status, isDirectory)) {
        const NTSTATUS fallbackStatus = KswordARKDriverDeleteFileWithDispositionEx(fileHandle);
        if (NT_SUCCESS(fallbackStatus)) {
            status = fallbackStatus;
        }
        else if (fallbackStatus != STATUS_INVALID_INFO_CLASS &&
            fallbackStatus != STATUS_NOT_SUPPORTED &&
            fallbackStatus != STATUS_INVALID_PARAMETER) {
            // Return explicit fallback failure; keep first status for unsupported cases.
            status = fallbackStatus;
        }
        else {
            status = firstDeleteStatus;
        }
    }

    ZwClose(fileHandle);
    return status;
}
