/*++

Module Name:

    ssdt_query.c

Abstract:

    This file contains SSDT traversal snapshot helpers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntimage.h>
#include <ntstrsafe.h>

NTSYSAPI
PVOID
NTAPI
RtlPcToFileHeader(
    _In_ PVOID PcValue,
    _Outptr_ PVOID* BaseOfImage
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

typedef struct _KSWORD_ARK_SERVICE_TABLE_DESCRIPTOR
{
    PVOID serviceTableBase;
    PVOID serviceCounterTableBase;
    ULONG_PTR numberOfServices;
    PVOID paramTableBase;
} KSWORD_ARK_SERVICE_TABLE_DESCRIPTOR;

static const ULONG g_KswordArkSsdtResponseHeaderSize =
    (ULONG)(sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY));

static VOID
KswordARKDriverCopyAnsiText(
    _Out_writes_bytes_(destinationBytes) CHAR* destinationText,
    _In_ size_t destinationBytes,
    _In_opt_z_ const CHAR* sourceText
    );

static BOOLEAN
KswordARKDriverStartsWithZw(
    _In_opt_z_ const CHAR* nameText
    )
{
    if (nameText == NULL) {
        return FALSE;
    }

    if (nameText[0] != 'Z' || nameText[1] != 'w') {
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
KswordARKDriverStartsWithAnsi(
    _In_opt_z_ const CHAR* NameText,
    _In_z_ const CHAR* PrefixText
    )
/*++

Routine Description:

    检查导出名称是否拥有指定 ANSI 前缀。中文说明：SSSDT 在 System Informer
    中依赖 win32k/win32u 导出命名，这里复用同一思路，只做前缀匹配。

Arguments:

    NameText - 待检查导出名。
    PrefixText - 需要匹配的前缀。

Return Value:

    TRUE 表示前缀匹配；FALSE 表示不匹配或参数无效。

--*/
{
    ULONG index = 0UL;

    if (NameText == NULL || PrefixText == NULL) {
        return FALSE;
    }

    while (PrefixText[index] != '\0') {
        if (NameText[index] == '\0' || NameText[index] != PrefixText[index]) {
            return FALSE;
        }
        ++index;
    }

    return TRUE;
}

static BOOLEAN
KswordARKDriverAsciiEqualsInsensitive(
    _In_reads_bytes_(LeftBytes) const UCHAR* LeftText,
    _In_ ULONG LeftBytes,
    _In_z_ PCSTR RightText
    )
/*++

Routine Description:

    比较 SystemModuleInformation 中的有限长模块文件名。中文说明：模块路径不
    保证以 NUL 结束，因此必须带长度逐字节比较。

Arguments:

    LeftText - 左侧有限长 ANSI 文本。
    LeftBytes - 左侧可读字节数。
    RightText - 右侧 NUL 结束常量文本。

Return Value:

    TRUE 表示大小写不敏感相等。

--*/
{
    ULONG index = 0UL;

    if (LeftText == NULL || RightText == NULL || LeftBytes == 0UL) {
        return FALSE;
    }

    for (index = 0UL; index < LeftBytes; ++index) {
        CHAR leftChar = (CHAR)LeftText[index];
        CHAR rightChar = RightText[index];

        if (leftChar >= 'A' && leftChar <= 'Z') {
            leftChar = (CHAR)(leftChar + ('a' - 'A'));
        }
        if (rightChar >= 'A' && rightChar <= 'Z') {
            rightChar = (CHAR)(rightChar + ('a' - 'A'));
        }
        if (rightChar == '\0') {
            return leftChar == '\0';
        }
        if (leftChar == '\0' || leftChar != rightChar) {
            return FALSE;
        }
    }

    return RightText[index] == '\0';
}

static NTSTATUS
KswordARKDriverResolveLoadedModuleImage(
    _In_z_ PCSTR ModuleFileName,
    _Outptr_ PVOID* ImageBaseOut,
    _Out_ ULONG* ImageSizeOut,
    _Out_writes_bytes_(ModuleNameBytes) CHAR* ModuleNameTextOut,
    _In_ size_t ModuleNameBytes
    )
/*++

Routine Description:

    从 SystemModuleInformation 查找已加载内核模块。中文说明：SSSDT 需要
    win32k.sys/win32u.dll 等图形子系统模块基址；这里不加载文件，只使用已加载映像。

Arguments:

    ModuleFileName - 目标模块文件名。
    ImageBaseOut - 返回加载基址。
    ImageSizeOut - 返回映像大小。
    ModuleNameTextOut - 返回模块名。
    ModuleNameBytes - 模块名缓冲字节数。

Return Value:

    STATUS_SUCCESS 或查询/未找到状态。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    PVOID rawBuffer = NULL;
    ULONG moduleIndex = 0UL;

    typedef struct _KSW_SSDT_SYSTEM_MODULE_ENTRY {
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
    } KSW_SSDT_SYSTEM_MODULE_ENTRY;
    typedef struct _KSW_SSDT_SYSTEM_MODULE_INFORMATION {
        ULONG NumberOfModules;
        KSW_SSDT_SYSTEM_MODULE_ENTRY Modules[1];
    } KSW_SSDT_SYSTEM_MODULE_INFORMATION;

    if (ImageBaseOut == NULL || ImageSizeOut == NULL || ModuleFileName == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ImageBaseOut = NULL;
    *ImageSizeOut = 0UL;
    if (ModuleNameTextOut != NULL && ModuleNameBytes > 0U) {
        ModuleNameTextOut[0] = '\0';
    }

    status = ZwQuerySystemInformation(11UL, NULL, 0UL, &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    rawBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, requiredBytes, 'sSsK');
#pragma warning(pop)
    if (rawBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySystemInformation(11UL, rawBuffer, requiredBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        KSW_SSDT_SYSTEM_MODULE_INFORMATION* moduleInfo =
            (KSW_SSDT_SYSTEM_MODULE_INFORMATION*)rawBuffer;
        for (moduleIndex = 0UL; moduleIndex < moduleInfo->NumberOfModules; ++moduleIndex) {
            const KSW_SSDT_SYSTEM_MODULE_ENTRY* moduleEntry = &moduleInfo->Modules[moduleIndex];
            const UCHAR* fileName = moduleEntry->FullPathName;
            ULONG fileNameBytes = (ULONG)sizeof(moduleEntry->FullPathName);

            if (moduleEntry->OffsetToFileName < sizeof(moduleEntry->FullPathName)) {
                fileName = moduleEntry->FullPathName + moduleEntry->OffsetToFileName;
                fileNameBytes = (ULONG)(sizeof(moduleEntry->FullPathName) - moduleEntry->OffsetToFileName);
            }

            if (!KswordARKDriverAsciiEqualsInsensitive(fileName, fileNameBytes, ModuleFileName)) {
                continue;
            }

            *ImageBaseOut = moduleEntry->ImageBase;
            *ImageSizeOut = moduleEntry->ImageSize;
            KswordARKDriverCopyAnsiText(ModuleNameTextOut, ModuleNameBytes, ModuleFileName);
            ExFreePoolWithTag(rawBuffer, 'sSsK');
            return STATUS_SUCCESS;
        }
        status = STATUS_NOT_FOUND;
    }

    ExFreePoolWithTag(rawBuffer, 'sSsK');
    return status;
}

static VOID
KswordARKDriverCopyAnsiText(
    _Out_writes_bytes_(destinationBytes) CHAR* destinationText,
    _In_ size_t destinationBytes,
    _In_opt_z_ const CHAR* sourceText
    )
{
    if (destinationText == NULL || destinationBytes == 0U) {
        return;
    }

    destinationText[0] = '\0';
    if (sourceText == NULL) {
        return;
    }

    (VOID)RtlStringCbCopyNA(
        destinationText,
        destinationBytes,
        sourceText,
        destinationBytes - 1U);
    destinationText[destinationBytes - 1U] = '\0';
}

static BOOLEAN
KswordARKDriverFindServiceIndexFromStub(
    _In_reads_bytes_(stubLengthBytes) const UCHAR* stubBytes,
    _In_ ULONG stubLengthBytes,
    _Out_ ULONG* serviceIndexOut
    )
{
    ULONG scanOffset = 0;

    if (stubBytes == NULL || serviceIndexOut == NULL) {
        return FALSE;
    }

    *serviceIndexOut = 0U;
    if (stubLengthBytes < 5U) {
        return FALSE;
    }

    for (scanOffset = 0U; scanOffset + 5U <= stubLengthBytes; ++scanOffset) {
        if (stubBytes[scanOffset] == 0xB8U) {
            ULONG serviceIndex = 0U;
            RtlCopyMemory(&serviceIndex, stubBytes + scanOffset + 1U, sizeof(serviceIndex));
            *serviceIndexOut = serviceIndex;
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
KswordARKDriverValidateRvaRange(
    _In_ ULONG rvaValue,
    _In_ ULONG dataLength,
    _In_ ULONG imageSize
    )
{
    if (rvaValue >= imageSize) {
        return FALSE;
    }

    if (dataLength > imageSize) {
        return FALSE;
    }

    if (rvaValue > (imageSize - dataLength)) {
        return FALSE;
    }

    return TRUE;
}

static NTSTATUS
KswordARKDriverResolveKernelImage(
    _Outptr_ PVOID* imageBaseOut,
    _Out_ ULONG* imageSizeOut,
    _Out_writes_bytes_(moduleNameBytes) CHAR* moduleNameTextOut,
    _In_ size_t moduleNameBytes
    )
{
    UNICODE_STRING routineName;
    PVOID ntOpenProcessAddress = NULL;
    PVOID imageBase = NULL;
    PIMAGE_DOS_HEADER dosHeader = NULL;
    PIMAGE_NT_HEADERS ntHeaders = NULL;

    if (imageBaseOut == NULL || imageSizeOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *imageBaseOut = NULL;
    *imageSizeOut = 0U;
    if (moduleNameTextOut != NULL && moduleNameBytes > 0U) {
        moduleNameTextOut[0] = '\0';
    }

    RtlInitUnicodeString(&routineName, L"NtOpenProcess");
    ntOpenProcessAddress = MmGetSystemRoutineAddress(&routineName);
    if (ntOpenProcessAddress == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (RtlPcToFileHeader(ntOpenProcessAddress, &imageBase) == NULL || imageBase == NULL) {
        return STATUS_NOT_FOUND;
    }

    dosHeader = (PIMAGE_DOS_HEADER)imageBase;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)imageBase + (ULONG)dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    *imageBaseOut = imageBase;
    *imageSizeOut = ntHeaders->OptionalHeader.SizeOfImage;

    if (moduleNameTextOut != NULL && moduleNameBytes > 0U) {
        const IMAGE_DATA_DIRECTORY* exportDirectory =
            &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exportDirectory->VirtualAddress != 0U &&
            KswordARKDriverValidateRvaRange(exportDirectory->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), *imageSizeOut)) {
            const PIMAGE_EXPORT_DIRECTORY exportHeader =
                (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)imageBase + exportDirectory->VirtualAddress);
            if (exportHeader->Name != 0U &&
                KswordARKDriverValidateRvaRange(exportHeader->Name, 2U, *imageSizeOut)) {
                const CHAR* exportModuleName = (const CHAR*)((PUCHAR)imageBase + exportHeader->Name);
                KswordARKDriverCopyAnsiText(moduleNameTextOut, moduleNameBytes, exportModuleName);
            }
        }

        if (moduleNameTextOut[0] == '\0') {
            KswordARKDriverCopyAnsiText(moduleNameTextOut, moduleNameBytes, "ntoskrnl.exe");
        }
    }

    return STATUS_SUCCESS;
}

static VOID
KswordARKDriverTryResolveServiceTable(
    _Outptr_result_maybenull_ PVOID* tableBaseOut,
    _Out_ ULONG* serviceCountOut
    )
{
    UNICODE_STRING tableName;
    PVOID descriptorAddress = NULL;

    if (tableBaseOut == NULL || serviceCountOut == NULL) {
        return;
    }

    *tableBaseOut = NULL;
    *serviceCountOut = 0U;

    RtlInitUnicodeString(&tableName, L"KeServiceDescriptorTable");
    descriptorAddress = MmGetSystemRoutineAddress(&tableName);
    if (descriptorAddress != NULL) {
        const KSWORD_ARK_SERVICE_TABLE_DESCRIPTOR* descriptor =
            (const KSWORD_ARK_SERVICE_TABLE_DESCRIPTOR*)descriptorAddress;
        if (descriptor->serviceTableBase != NULL &&
            descriptor->numberOfServices > 0U &&
            descriptor->numberOfServices <= MAXULONG) {
            *tableBaseOut = descriptor->serviceTableBase;
            *serviceCountOut = (ULONG)descriptor->numberOfServices;
            return;
        }
    }
}

static ULONG_PTR
KswordARKDriverResolveServiceRoutineAddress(
    _In_opt_ PVOID serviceTableBase,
    _In_ ULONG serviceCount,
    _In_ ULONG serviceIndex
    )
{
    if (serviceTableBase == NULL || serviceCount == 0U) {
        return 0U;
    }

    if (serviceIndex >= serviceCount) {
        return 0U;
    }

#if defined(_M_AMD64)
    {
        const LONG entryValue = ((volatile LONG*)serviceTableBase)[serviceIndex];
        const LONG_PTR signedOffset = ((LONG_PTR)entryValue) >> 4;
        return (ULONG_PTR)((PUCHAR)serviceTableBase + signedOffset);
    }
#elif defined(_M_IX86)
    return ((volatile ULONG_PTR*)serviceTableBase)[serviceIndex];
#else
    UNREFERENCED_PARAMETER(serviceTableBase);
    UNREFERENCED_PARAMETER(serviceCount);
    UNREFERENCED_PARAMETER(serviceIndex);
    return 0U;
#endif
}

NTSTATUS
KswordARKDriverEnumerateSsdt(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_SSDT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_SSDT_RESPONSE* responseHeader = NULL;
    ULONG entryCapacity = 0U;
    ULONG requestFlags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
    PVOID imageBase = NULL;
    ULONG imageSize = 0U;
    CHAR moduleNameText[KSWORD_ARK_SSDT_ENTRY_MAX_MODULE] = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    PIMAGE_DOS_HEADER dosHeader = NULL;
    PIMAGE_NT_HEADERS ntHeaders = NULL;
    const IMAGE_DATA_DIRECTORY* exportDirectory = NULL;
    PIMAGE_EXPORT_DIRECTORY exportHeader = NULL;
    PULONG nameRvaArray = NULL;
    PUSHORT nameOrdinalArray = NULL;
    PULONG functionRvaArray = NULL;
    ULONG exportNameIndex = 0U;
    PVOID serviceTableBase = NULL;
    ULONG serviceCountFromTable = 0U;

    if (outputBuffer == NULL || bytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *bytesWrittenOut = 0U;
    if (outputBufferLength < g_KswordArkSsdtResponseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (request != NULL) {
        requestFlags = request->flags;
    }

    RtlZeroMemory(outputBuffer, outputBufferLength);
    responseHeader = (KSWORD_ARK_ENUM_SSDT_RESPONSE*)outputBuffer;
    responseHeader->version = KSWORD_ARK_ENUM_SSDT_PROTOCOL_VERSION;
    responseHeader->entrySize = sizeof(KSWORD_ARK_SSDT_ENTRY);
    entryCapacity = (ULONG)((outputBufferLength - g_KswordArkSsdtResponseHeaderSize) / sizeof(KSWORD_ARK_SSDT_ENTRY));

    status = KswordARKDriverResolveKernelImage(
        &imageBase,
        &imageSize,
        moduleNameText,
        sizeof(moduleNameText));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KswordARKDriverTryResolveServiceTable(&serviceTableBase, &serviceCountFromTable);
    responseHeader->serviceTableBase = (ULONGLONG)(ULONG_PTR)serviceTableBase;
    responseHeader->serviceCountFromTable = serviceCountFromTable;

    dosHeader = (PIMAGE_DOS_HEADER)imageBase;
    ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)imageBase + (ULONG)dosHeader->e_lfanew);
    exportDirectory = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDirectory->VirtualAddress == 0U ||
        !KswordARKDriverValidateRvaRange(exportDirectory->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), imageSize)) {
        return STATUS_NOT_FOUND;
    }

    exportHeader = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)imageBase + exportDirectory->VirtualAddress);
    if (exportHeader->AddressOfNames == 0U ||
        exportHeader->AddressOfNameOrdinals == 0U ||
        exportHeader->AddressOfFunctions == 0U) {
        return STATUS_NOT_FOUND;
    }

    if (!KswordARKDriverValidateRvaRange(
        exportHeader->AddressOfNames,
        exportHeader->NumberOfNames * sizeof(ULONG),
        imageSize)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    if (!KswordARKDriverValidateRvaRange(
        exportHeader->AddressOfNameOrdinals,
        exportHeader->NumberOfNames * sizeof(USHORT),
        imageSize)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    if (!KswordARKDriverValidateRvaRange(
        exportHeader->AddressOfFunctions,
        exportHeader->NumberOfFunctions * sizeof(ULONG),
        imageSize)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    nameRvaArray = (PULONG)((PUCHAR)imageBase + exportHeader->AddressOfNames);
    nameOrdinalArray = (PUSHORT)((PUCHAR)imageBase + exportHeader->AddressOfNameOrdinals);
    functionRvaArray = (PULONG)((PUCHAR)imageBase + exportHeader->AddressOfFunctions);

    for (exportNameIndex = 0U; exportNameIndex < exportHeader->NumberOfNames; ++exportNameIndex) {
        const ULONG nameRva = nameRvaArray[exportNameIndex];
        const USHORT ordinalIndex = nameOrdinalArray[exportNameIndex];
        const CHAR* exportNameText = NULL;
        ULONG functionRva = 0U;
        const UCHAR* stubBytes = NULL;
        ULONG serviceIndex = 0U;
        BOOLEAN indexResolved = FALSE;
        ULONG_PTR serviceRoutineAddress = 0U;
        KSWORD_ARK_SSDT_ENTRY* entry = NULL;

        if (ordinalIndex >= exportHeader->NumberOfFunctions) {
            continue;
        }

        if (!KswordARKDriverValidateRvaRange(nameRva, 2U, imageSize)) {
            continue;
        }

        exportNameText = (const CHAR*)((PUCHAR)imageBase + nameRva);
        if (!KswordARKDriverStartsWithZw(exportNameText)) {
            continue;
        }

        functionRva = functionRvaArray[ordinalIndex];
        if (functionRva >= exportDirectory->VirtualAddress &&
            functionRva < exportDirectory->VirtualAddress + exportDirectory->Size) {
            // Forwarded export points into export directory string area.
            continue;
        }
        if (!KswordARKDriverValidateRvaRange(functionRva, 16U, imageSize)) {
            continue;
        }

        stubBytes = (const UCHAR*)((PUCHAR)imageBase + functionRva);
        indexResolved = KswordARKDriverFindServiceIndexFromStub(stubBytes, 32U, &serviceIndex);
        if (!indexResolved &&
            (requestFlags & KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED) == 0U) {
            continue;
        }

        responseHeader->totalCount += 1UL;
        if (responseHeader->returnedCount >= entryCapacity) {
            continue;
        }

        entry = &responseHeader->entries[responseHeader->returnedCount];
        RtlZeroMemory(entry, sizeof(*entry));
        entry->zwRoutineAddress = (ULONGLONG)(ULONG_PTR)stubBytes;
        if (indexResolved) {
            entry->serviceIndex = serviceIndex;
            entry->flags |= KSWORD_ARK_SSDT_ENTRY_FLAG_INDEX_RESOLVED;

            serviceRoutineAddress = KswordARKDriverResolveServiceRoutineAddress(
                serviceTableBase,
                serviceCountFromTable,
                serviceIndex);
            if (serviceRoutineAddress != 0U) {
                entry->serviceRoutineAddress = (ULONGLONG)serviceRoutineAddress;
                entry->flags |= KSWORD_ARK_SSDT_ENTRY_FLAG_TABLE_ADDRESS_VALID;
            }
        }
        else {
            entry->serviceIndex = 0U;
        }

        KswordARKDriverCopyAnsiText(entry->serviceName, sizeof(entry->serviceName), exportNameText);
        KswordARKDriverCopyAnsiText(entry->moduleName, sizeof(entry->moduleName), moduleNameText);

        responseHeader->returnedCount += 1UL;
    }

    *bytesWrittenOut = g_KswordArkSsdtResponseHeaderSize +
        ((size_t)responseHeader->returnedCount * sizeof(KSWORD_ARK_SSDT_ENTRY));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnumerateShadowSsdt(
    _Out_writes_bytes_to_(outputBufferLength, *bytesWrittenOut) PVOID outputBuffer,
    _In_ size_t outputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_SSDT_REQUEST* request,
    _Out_ size_t* bytesWrittenOut
    )
/*++

Routine Description:

    枚举图形子系统影子系统调用导出。中文说明：参考 System Informer 的
    ksyscall.c，优先扫描已加载 win32k.sys 的 __win32kstub_ 导出；旧系统或不同
    拆分布局下再扫描 win32u.dll 的 Nt* 导出，作为用户友好的 SSSDT 名称视图。

Arguments:

    outputBuffer - 响应缓冲。
    outputBufferLength - 响应缓冲长度。
    request - 可选请求，flags 控制是否包含未解析项。
    bytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；底层失败返回 NTSTATUS。

--*/
{
    KSWORD_ARK_ENUM_SSDT_RESPONSE* responseHeader = NULL;
    ULONG entryCapacity = 0U;
    ULONG requestFlags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
    const CHAR* moduleCandidates[] = { "win32k.sys", "win32u.dll" };
    ULONG candidateIndex = 0UL;
    NTSTATUS lastStatus = STATUS_NOT_FOUND;

    if (outputBuffer == NULL || bytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *bytesWrittenOut = 0U;
    if (outputBufferLength < g_KswordArkSsdtResponseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (request != NULL) {
        requestFlags = request->flags;
    }

    RtlZeroMemory(outputBuffer, outputBufferLength);
    responseHeader = (KSWORD_ARK_ENUM_SSDT_RESPONSE*)outputBuffer;
    responseHeader->version = KSWORD_ARK_ENUM_SSDT_PROTOCOL_VERSION;
    responseHeader->entrySize = sizeof(KSWORD_ARK_SSDT_ENTRY);
    responseHeader->serviceTableBase = 0ULL;
    responseHeader->serviceCountFromTable = 0UL;
    entryCapacity = (ULONG)((outputBufferLength - g_KswordArkSsdtResponseHeaderSize) / sizeof(KSWORD_ARK_SSDT_ENTRY));

    for (candidateIndex = 0UL; candidateIndex < RTL_NUMBER_OF(moduleCandidates); ++candidateIndex) {
        PVOID imageBase = NULL;
        ULONG imageSize = 0U;
        CHAR moduleNameText[KSWORD_ARK_SSDT_ENTRY_MAX_MODULE] = { 0 };
        PIMAGE_DOS_HEADER dosHeader = NULL;
        PIMAGE_NT_HEADERS ntHeaders = NULL;
        const IMAGE_DATA_DIRECTORY* exportDirectory = NULL;
        PIMAGE_EXPORT_DIRECTORY exportHeader = NULL;
        PULONG nameRvaArray = NULL;
        PUSHORT nameOrdinalArray = NULL;
        PULONG functionRvaArray = NULL;
        ULONG exportNameIndex = 0UL;
        NTSTATUS status = STATUS_SUCCESS;

        status = KswordARKDriverResolveLoadedModuleImage(
            moduleCandidates[candidateIndex],
            &imageBase,
            &imageSize,
            moduleNameText,
            sizeof(moduleNameText));
        lastStatus = status;
        if (!NT_SUCCESS(status) || imageBase == NULL || imageSize == 0UL) {
            continue;
        }

        __try {
            dosHeader = (PIMAGE_DOS_HEADER)imageBase;
            if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
                continue;
            }
            ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)imageBase + (ULONG)dosHeader->e_lfanew);
            if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
                continue;
            }
            exportDirectory = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (exportDirectory->VirtualAddress == 0U ||
                !KswordARKDriverValidateRvaRange(exportDirectory->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), imageSize)) {
                continue;
            }
            exportHeader = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)imageBase + exportDirectory->VirtualAddress);
            if (exportHeader->AddressOfNames == 0U ||
                exportHeader->AddressOfNameOrdinals == 0U ||
                exportHeader->AddressOfFunctions == 0U) {
                continue;
            }
            if (!KswordARKDriverValidateRvaRange(exportHeader->AddressOfNames, exportHeader->NumberOfNames * sizeof(ULONG), imageSize) ||
                !KswordARKDriverValidateRvaRange(exportHeader->AddressOfNameOrdinals, exportHeader->NumberOfNames * sizeof(USHORT), imageSize) ||
                !KswordARKDriverValidateRvaRange(exportHeader->AddressOfFunctions, exportHeader->NumberOfFunctions * sizeof(ULONG), imageSize)) {
                continue;
            }

            nameRvaArray = (PULONG)((PUCHAR)imageBase + exportHeader->AddressOfNames);
            nameOrdinalArray = (PUSHORT)((PUCHAR)imageBase + exportHeader->AddressOfNameOrdinals);
            functionRvaArray = (PULONG)((PUCHAR)imageBase + exportHeader->AddressOfFunctions);

            for (exportNameIndex = 0UL; exportNameIndex < exportHeader->NumberOfNames; ++exportNameIndex) {
                const ULONG nameRva = nameRvaArray[exportNameIndex];
                const USHORT ordinalIndex = nameOrdinalArray[exportNameIndex];
                const CHAR* exportNameText = NULL;
                ULONG functionRva = 0UL;
                const UCHAR* stubBytes = NULL;
                ULONG serviceIndex = 0UL;
                BOOLEAN indexResolved = FALSE;
                KSWORD_ARK_SSDT_ENTRY* entry = NULL;

                if (ordinalIndex >= exportHeader->NumberOfFunctions ||
                    !KswordARKDriverValidateRvaRange(nameRva, 2U, imageSize)) {
                    continue;
                }

                exportNameText = (const CHAR*)((PUCHAR)imageBase + nameRva);
                if (!KswordARKDriverStartsWithAnsi(exportNameText, "__win32kstub_") &&
                    !KswordARKDriverStartsWithAnsi(exportNameText, "Nt")) {
                    continue;
                }

                functionRva = functionRvaArray[ordinalIndex];
                if (functionRva >= exportDirectory->VirtualAddress &&
                    functionRva < exportDirectory->VirtualAddress + exportDirectory->Size) {
                    continue;
                }
                if (!KswordARKDriverValidateRvaRange(functionRva, 16U, imageSize)) {
                    continue;
                }

                stubBytes = (const UCHAR*)((PUCHAR)imageBase + functionRva);
                indexResolved = KswordARKDriverFindServiceIndexFromStub(stubBytes, 32U, &serviceIndex);
                if (!indexResolved &&
                    (requestFlags & KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED) == 0U) {
                    continue;
                }

                responseHeader->totalCount += 1UL;
                if (responseHeader->returnedCount >= entryCapacity) {
                    continue;
                }

                entry = &responseHeader->entries[responseHeader->returnedCount];
                RtlZeroMemory(entry, sizeof(*entry));
                entry->zwRoutineAddress = (ULONGLONG)(ULONG_PTR)stubBytes;
                entry->serviceRoutineAddress = 0ULL;
                entry->flags = KSWORD_ARK_SSDT_ENTRY_FLAG_SHADOW_TABLE | KSWORD_ARK_SSDT_ENTRY_FLAG_STUB_EXPORT;
                if (indexResolved) {
                    entry->serviceIndex = serviceIndex & 0x0FFFUL;
                    entry->flags |= KSWORD_ARK_SSDT_ENTRY_FLAG_INDEX_RESOLVED;
                }
                KswordARKDriverCopyAnsiText(entry->serviceName, sizeof(entry->serviceName), exportNameText);
                if (KswordARKDriverStartsWithAnsi(entry->serviceName, "__win32kstub_")) {
                    RtlMoveMemory(
                        entry->serviceName,
                        entry->serviceName + 13,
                        sizeof(entry->serviceName) - 13);
                    entry->serviceName[sizeof(entry->serviceName) - 1U] = '\0';
                }
                KswordARKDriverCopyAnsiText(entry->moduleName, sizeof(entry->moduleName), moduleNameText);
                responseHeader->returnedCount += 1UL;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            lastStatus = GetExceptionCode();
            continue;
        }

        if (responseHeader->totalCount > 0UL) {
            *bytesWrittenOut = g_KswordArkSsdtResponseHeaderSize +
                ((size_t)responseHeader->returnedCount * sizeof(KSWORD_ARK_SSDT_ENTRY));
            return STATUS_SUCCESS;
        }
    }

    *bytesWrittenOut = g_KswordArkSsdtResponseHeaderSize;
    return NT_SUCCESS(lastStatus) ? STATUS_NOT_FOUND : lastStatus;
}
