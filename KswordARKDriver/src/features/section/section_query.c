/*++

Module Name:

    section_query.c

Abstract:

    Phase-7 process SectionObject and ControlArea mapping inspection.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_section.h"

#include "ark/ark_dyndata.h"
#include "section_support.h"

#define KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE) - sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY))

#define KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE) - sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY))

#ifndef RTL_NUMBER_OF
#define RTL_NUMBER_OF(A) (sizeof(A) / sizeof((A)[0]))
#endif

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

static NTSTATUS
KswordARKSectionOpenFileObjectByPath(
    _In_reads_(PathLengthChars) PCWSTR PathText,
    _In_ USHORT PathLengthChars,
    _Outptr_result_maybenull_ PFILE_OBJECT* FileObjectOut,
    _Out_opt_ HANDLE* FileHandleOut
    )
/*++

Routine Description:

    用 R3 提供的 NT 路径打开并引用 FILE_OBJECT。中文说明：调用方只传
    文件路径，不传内核 FILE_OBJECT 地址；返回的对象引用必须释放。

Arguments:

    PathText - NT 路径，例如 \??\C:\Windows\System32\kernel32.dll。
    PathLengthChars - 不含 NUL 的字符数。
    FileObjectOut - 接收引用后的 FILE_OBJECT。
    FileHandleOut - 可选接收内核句柄，调用者必须 ZwClose。

Return Value:

    STATUS_SUCCESS 或 ZwCreateFile/ObReferenceObjectByHandle 错误码。

--*/
{
    UNICODE_STRING filePath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileObjectOut != NULL) {
        *FileObjectOut = NULL;
    }
    if (FileHandleOut != NULL) {
        *FileHandleOut = NULL;
    }
    if (PathText == NULL ||
        PathLengthChars == 0U ||
        PathLengthChars >= KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS ||
        FileObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&filePath, sizeof(filePath));
    filePath.Buffer = (PWCH)PathText;
    filePath.Length = (USHORT)(PathLengthChars * sizeof(WCHAR));
    filePath.MaximumLength = filePath.Length + sizeof(WCHAR);

    InitializeObjectAttributes(
        &objectAttributes,
        &filePath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        &fileHandle,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
        NULL,
        0U);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ObReferenceObjectByHandle(
        fileHandle,
        0,
        NULL,
        KernelMode,
        (PVOID*)&fileObject,
        NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(fileHandle);
        return status;
    }

    *FileObjectOut = fileObject;
    if (FileHandleOut != NULL) {
        *FileHandleOut = fileHandle;
    }
    else {
        ZwClose(fileHandle);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverQueryProcessSection(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询目标进程主映像 SectionObject、ControlArea 和映射摘要。中文说明：输入
    只接受 PID，所有返回地址均为诊断展示值，不作为后续 IOCTL 凭据。

Arguments:

    OutputBuffer - 输出响应包。
    OutputBufferLength - 输出缓冲容量。
    Request - 请求包，包含 PID 和最大映射条目数。
    BytesWrittenOut - 接收实际写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应头有效；查询失败细节写入 response->queryStatus。

--*/
{
    KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE* response = NULL;
    KSW_DYN_STATE dynState;
    PEPROCESS processObject = NULL;
    PVOID sectionObject = NULL;
    PVOID controlArea = NULL;
    BOOLEAN remoteUnsupported = FALSE;
    ULONG requestFlags = 0UL;
    ULONG maxMappings = 0UL;
    size_t entryCapacity = 0U;
    size_t totalBytesWritten = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->processId == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    response = (KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_SECTION_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY);
    response->processId = Request->processId;
    response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_UNAVAILABLE;
    response->lastStatus = STATUS_SUCCESS;
    KswordARKSectionPrepareOffsets(response, &dynState);

    requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_ALL : Request->flags;
    maxMappings = Request->maxMappings;
    if (maxMappings == 0UL) {
        maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT;
    }
    if (maxMappings > KSWORD_ARK_SECTION_MAPPING_LIMIT_MAX) {
        maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_MAX;
    }

    entryCapacity = (OutputBufferLength - KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY);
    if (entryCapacity > (size_t)maxMappings) {
        entryCapacity = (size_t)maxMappings;
    }

    if (!KswordARKSectionHasRequiredDynData(&dynState)) {
        response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_DYNDATA_MISSING;
        response->lastStatus = STATUS_NOT_SUPPORTED;
        *BytesWrittenOut = KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_PROCESS_LOOKUP_FAILED;
        response->lastStatus = status;
        *BytesWrittenOut = KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    status = KswordARKSectionReadProcessSectionObject(processObject, &dynState, &sectionObject);
    response->lastStatus = status;
    if (!NT_SUCCESS(status) || sectionObject == NULL) {
        response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_SECTION_OBJECT_MISSING;
        ObDereferenceObject(processObject);
        *BytesWrittenOut = KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    response->sectionObjectAddress = (ULONG64)(ULONG_PTR)sectionObject;
    response->fieldFlags |= KSWORD_ARK_SECTION_FIELD_SECTION_OBJECT_PRESENT;

    if ((requestFlags & KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_CONTROL_AREA) != 0UL) {
        status = KswordARKSectionReadControlArea(sectionObject, &dynState, &controlArea, &remoteUnsupported);
        response->lastStatus = status;
        if (controlArea != NULL) {
            response->controlAreaAddress = (ULONG64)(ULONG_PTR)controlArea;
            response->fieldFlags |= KSWORD_ARK_SECTION_FIELD_CONTROL_AREA_PRESENT;
        }
        if (remoteUnsupported) {
            response->fieldFlags |= KSWORD_ARK_SECTION_FIELD_REMOTE_MAPPING_UNSUPPORTED;
            response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_REMOTE_UNSUPPORTED;
        }
        else if (!NT_SUCCESS(status)) {
            response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_CONTROL_AREA_MISSING;
        }
    }

    if ((requestFlags & KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_MAPPINGS) != 0UL &&
        controlArea != NULL &&
        !remoteUnsupported) {
        status = KswordARKSectionEnumerateMappings(controlArea, &dynState, response, entryCapacity);
        response->lastStatus = status;
        if (!NT_SUCCESS(status)) {
            response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED;
        }
    }

    if (response->queryStatus == KSWORD_ARK_SECTION_QUERY_STATUS_UNAVAILABLE) {
        response->queryStatus = ((response->fieldFlags & KSWORD_ARK_SECTION_FIELD_MAPPING_TRUNCATED) != 0UL) ?
            KSWORD_ARK_SECTION_QUERY_STATUS_BUFFER_TOO_SMALL :
            KSWORD_ARK_SECTION_QUERY_STATUS_OK;
    }
    else if (response->queryStatus != KSWORD_ARK_SECTION_QUERY_STATUS_OK &&
        response->fieldFlags != 0UL) {
        response->queryStatus = KSWORD_ARK_SECTION_QUERY_STATUS_PARTIAL;
    }

    ObDereferenceObject(processObject);
    totalBytesWritten = KSWORD_ARK_SECTION_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY));
    *BytesWrittenOut = totalBytesWritten;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverQueryFileSectionMappings(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询文件当前 Data/Image ControlArea 的进程映射关系。中文说明：输入仅为
    文件路径，驱动自行打开 FILE_OBJECT 并读取 SectionObjectPointer；ControlArea
    地址只用于诊断展示，不接受 R3 反传。

Arguments:

    OutputBuffer - 输出响应包。
    OutputBufferLength - 输出缓冲容量。
    Request - 请求包，包含 NT 路径、flags 和最大映射条目数。
    BytesWrittenOut - 接收实际写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应头有效；失败细节写入 response->queryStatus。

--*/
{
    KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE* response = NULL;
    KSW_DYN_STATE dynState;
    PFILE_OBJECT fileObject = NULL;
    HANDLE fileHandle = NULL;
    PSECTION_OBJECT_POINTERS sectionPointers = NULL;
    PVOID dataControlArea = NULL;
    PVOID imageControlArea = NULL;
    ULONG requestFlags = 0UL;
    ULONG maxMappings = 0UL;
    size_t entryCapacity = 0U;
    size_t totalBytesWritten = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS mappingStatus = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->pathLengthChars == 0U ||
        Request->pathLengthChars >= KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS ||
        Request->path[Request->pathLengthChars] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    response = (KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_SECTION_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY);
    response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_UNAVAILABLE;
    response->lastStatus = STATUS_SUCCESS;
    response->dynDataCapabilityMask = dynState.CapabilityMask;
    response->mmControlAreaListHeadOffset = KswordARKSectionNormalizeOffset(dynState.Kernel.MmControlAreaListHead);
    response->mmControlAreaLockOffset = KswordARKSectionNormalizeOffset(dynState.Kernel.MmControlAreaLock);

    requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL : Request->flags;
    maxMappings = Request->maxMappings;
    if (maxMappings == 0UL) {
        maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT;
    }
    if (maxMappings > KSWORD_ARK_SECTION_MAPPING_LIMIT_MAX) {
        maxMappings = KSWORD_ARK_SECTION_MAPPING_LIMIT_MAX;
    }

    entryCapacity = (OutputBufferLength - KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY);
    if (entryCapacity > (size_t)maxMappings) {
        entryCapacity = (size_t)maxMappings;
    }

    if (!KswordARKSectionHasRequiredDynData(&dynState)) {
        response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_DYNDATA_MISSING;
        response->lastStatus = STATUS_NOT_SUPPORTED;
        *BytesWrittenOut = KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    status = KswordARKSectionOpenFileObjectByPath(
        Request->path,
        Request->pathLengthChars,
        &fileObject,
        &fileHandle);
    response->lastStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OPEN_FAILED;
        *BytesWrittenOut = KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    response->fileObjectAddress = (ULONG64)(ULONG_PTR)fileObject;
    response->fieldFlags |= KSWORD_ARK_FILE_SECTION_FIELD_FILE_OBJECT_PRESENT;

    __try {
        sectionPointers = fileObject->SectionObjectPointer;
        response->sectionObjectPointersAddress = (ULONG64)(ULONG_PTR)sectionPointers;
        if (sectionPointers != NULL) {
            dataControlArea = sectionPointers->DataSectionObject;
            imageControlArea = sectionPointers->ImageSectionObject;
            response->dataControlAreaAddress = (ULONG64)(ULONG_PTR)dataControlArea;
            response->imageControlAreaAddress = (ULONG64)(ULONG_PTR)imageControlArea;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        response->lastStatus = status;
        response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OBJECT_FAILED;
    }

    if (response->queryStatus == KSWORD_ARK_FILE_SECTION_QUERY_STATUS_FILE_OBJECT_FAILED) {
        ObDereferenceObject(fileObject);
        ZwClose(fileHandle);
        *BytesWrittenOut = KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    if (sectionPointers == NULL) {
        response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_SECTION_POINTERS_MISSING;
        ObDereferenceObject(fileObject);
        ZwClose(fileHandle);
        *BytesWrittenOut = KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    response->fieldFlags |= KSWORD_ARK_FILE_SECTION_FIELD_SECTION_POINTERS_PRESENT;
    if (dataControlArea != NULL) {
        response->fieldFlags |= KSWORD_ARK_FILE_SECTION_FIELD_DATA_CONTROL_AREA_PRESENT;
    }
    if (imageControlArea != NULL) {
        response->fieldFlags |= KSWORD_ARK_FILE_SECTION_FIELD_IMAGE_CONTROL_AREA_PRESENT;
    }

    if ((requestFlags & KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_DATA_IMAGE) != 0UL &&
        dataControlArea != NULL) {
        mappingStatus = KswordARKSectionEnumerateFileControlAreaMappings(
            dataControlArea,
            KSWORD_ARK_FILE_SECTION_KIND_DATA,
            &dynState,
            response,
            entryCapacity);
        response->lastStatus = mappingStatus;
        if (!NT_SUCCESS(mappingStatus)) {
            response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED;
        }
    }

    if ((requestFlags & KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_IMAGE) != 0UL &&
        imageControlArea != NULL) {
        mappingStatus = KswordARKSectionEnumerateFileControlAreaMappings(
            imageControlArea,
            KSWORD_ARK_FILE_SECTION_KIND_IMAGE,
            &dynState,
            response,
            entryCapacity);
        response->lastStatus = mappingStatus;
        if (!NT_SUCCESS(mappingStatus) &&
            response->queryStatus == KSWORD_ARK_FILE_SECTION_QUERY_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_MAPPING_QUERY_FAILED;
        }
    }

    if (dataControlArea == NULL && imageControlArea == NULL) {
        response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_CONTROL_AREA_MISSING;
        response->lastStatus = STATUS_NOT_FOUND;
    }
    else if (response->queryStatus == KSWORD_ARK_FILE_SECTION_QUERY_STATUS_UNAVAILABLE) {
        response->queryStatus = ((response->fieldFlags & KSWORD_ARK_FILE_SECTION_FIELD_MAPPING_TRUNCATED) != 0UL) ?
            KSWORD_ARK_FILE_SECTION_QUERY_STATUS_BUFFER_TOO_SMALL :
            KSWORD_ARK_FILE_SECTION_QUERY_STATUS_OK;
    }
    else if (response->queryStatus != KSWORD_ARK_FILE_SECTION_QUERY_STATUS_OK &&
        (response->fieldFlags & KSWORD_ARK_FILE_SECTION_FIELD_MAPPING_LIST_PRESENT) != 0UL) {
        response->queryStatus = KSWORD_ARK_FILE_SECTION_QUERY_STATUS_PARTIAL;
    }

    ObDereferenceObject(fileObject);
    ZwClose(fileHandle);
    totalBytesWritten = KSWORD_ARK_FILE_SECTION_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY));
    *BytesWrittenOut = totalBytesWritten;
    return STATUS_SUCCESS;
}
