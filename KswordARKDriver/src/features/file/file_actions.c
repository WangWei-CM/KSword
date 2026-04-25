/*++

Module Name:

    file_actions.c

Abstract:

    This file contains kernel file deletion operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

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
