/*++

Module Name:

    kernel_ioctl.c

Abstract:

    IOCTL handlers for KswordARK kernel inspection operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY))

static VOID
KswordARKKernelIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format and enqueue one kernel-handler log message.

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
KswordARKKernelIoctlEnumSsdt(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_SSDT. Optional input preserves the legacy
    default request, and the feature function owns SSDT enumeration details.

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Supplied input bytes; shorter input selects defaults.
    OutputBufferLength - Supplied output bytes; checked by WDF output retrieval.
    BytesReturned - Receives the feature-written response byte count.

Return Value:

    NTSTATUS from buffer retrieval or KswordARKDriverEnumerateSsdt.

--*/
{
    KSWORD_ARK_ENUM_SSDT_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_SSDT_REQUEST defaultRequest = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveOptionalInputBuffer(Request, InputBufferLength, sizeof(KSWORD_ARK_ENUM_SSDT_REQUEST), &inputBuffer, &actualInputLength, &hasInput);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-ssdt ioctl: input buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (hasInput) {
        enumRequest = (KSWORD_ARK_ENUM_SSDT_REQUEST*)inputBuffer;
    }
    else {
        enumRequest = &defaultRequest;
        enumRequest->flags = KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED;
        enumRequest->reserved = 0UL;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(Request, KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE, &outputBuffer, &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-ssdt ioctl: output buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateSsdt(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKKernelIoctlLog(Device, "Error", "R0 enum-ssdt failed: status=0x%08X, outBytes=%Iu.", (unsigned int)status, *BytesReturned);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_ENUM_SSDT_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_SSDT_RESPONSE* responseHeader = (KSWORD_ARK_ENUM_SSDT_RESPONSE*)outputBuffer;
        KswordARKKernelIoctlLog(Device, "Info", "R0 enum-ssdt success: total=%lu, returned=%lu, outBytes=%Iu.", (unsigned long)responseHeader->totalCount, (unsigned long)responseHeader->returnedCount, *BytesReturned);
    }
    else {
        KswordARKKernelIoctlLog(Device, "Warn", "R0 enum-ssdt success: outBytes=%Iu (header partial).", *BytesReturned);
    }

    return status;
}
