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

typedef struct _KSWORD_ARK_SERVICE_TABLE_DESCRIPTOR
{
    PVOID serviceTableBase;
    PVOID serviceCounterTableBase;
    ULONG_PTR numberOfServices;
    PVOID paramTableBase;
} KSWORD_ARK_SERVICE_TABLE_DESCRIPTOR;

static const ULONG g_KswordArkSsdtResponseHeaderSize =
    (ULONG)(sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY));

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
