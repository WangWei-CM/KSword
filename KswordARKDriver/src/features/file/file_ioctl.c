/*++

Module Name:

    file_ioctl.c

Abstract:

    IOCTL handlers for KswordARK file operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKFileIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one file-handler log message.

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style ANSI message template.
    ... - Template arguments.

Return Value:

    None. Formatting or enqueue failures are ignored.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKFileIoctlDeletePath(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_DELETE_PATH. The handler validates flags, path
    length, and null termination before invoking the file delete feature.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length; consumed by WDF retrieval.
    OutputBufferLength - Caller output length; unused for this IOCTL.
    BytesReturned - Receives sizeof(request) on success and zero on failure.

Return Value:

    NTSTATUS from validation or KswordARKDriverDeletePath.

--*/
{
    KSWORD_ARK_DELETE_PATH_REQUEST* deleteRequest = NULL;
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0;
    BOOLEAN isDirectory = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(KSWORD_ARK_DELETE_PATH_REQUEST), &inputBuffer, &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Error", "R0 delete ioctl: input buffer invalid, status=0x%08X", (unsigned int)status);
        return status;
    }

    deleteRequest = (KSWORD_ARK_DELETE_PATH_REQUEST*)inputBuffer;
    isDirectory = ((deleteRequest->flags & KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY) != 0UL) ? TRUE : FALSE;

    if ((deleteRequest->flags & (~KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY)) != 0UL) {
        KswordARKFileIoctlLog(Device, "Warn", "R0 delete ioctl: flags rejected, flags=0x%08X.", (unsigned int)deleteRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }

    if (deleteRequest->pathLengthChars == 0U || deleteRequest->pathLengthChars >= KSWORD_ARK_DELETE_PATH_MAX_CHARS) {
        KswordARKFileIoctlLog(Device, "Warn", "R0 delete ioctl: path length rejected, chars=%u.", (unsigned int)deleteRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }

    if (deleteRequest->path[deleteRequest->pathLengthChars] != L'\0') {
        KswordARKFileIoctlLog(Device, "Warn", "R0 delete ioctl: path not null-terminated, chars=%u.", (unsigned int)deleteRequest->pathLengthChars);
        return STATUS_INVALID_PARAMETER;
    }

    KswordARKFileIoctlLog(Device, "Info", "R0 delete ioctl: chars=%u, directory=%u.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)isDirectory);
    status = KswordARKDriverDeletePath(deleteRequest->path, deleteRequest->pathLengthChars, isDirectory);
    if (NT_SUCCESS(status)) {
        KswordARKFileIoctlLog(Device, "Info", "R0 delete success: chars=%u, directory=%u.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)isDirectory);
        *BytesReturned = sizeof(KSWORD_ARK_DELETE_PATH_REQUEST);
    }
    else {
        KswordARKFileIoctlLog(Device, "Error", "R0 delete failed: chars=%u, directory=%u, status=0x%08X.", (unsigned int)deleteRequest->pathLengthChars, (unsigned int)isDirectory, (unsigned int)status);
    }

    return status;
}
