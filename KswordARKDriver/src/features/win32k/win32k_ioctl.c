/*++

Module Name:

    win32k_ioctl.c

Abstract:

    IOCTL handlers for read-only win32k GUI audit queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"
#include "win32k_query.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY))

#define KSWORD_ARK_WIN32K_WINDOW_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_WINDOW_ENTRY))

#define KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY))

#define KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY))

#define KSWORD_ARK_WIN32K_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY))

typedef NTSTATUS(*KSWORD_ARK_WIN32K_QUERY_COLLECTOR)(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

static VOID
KswordARKWin32kIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format a bounded Win32k IOCTL log message and enqueue it through the shared
    driver log channel.

Arguments:

    Device - Current framework device object.
    LevelText - Existing textual log severity label.
    FormatText - printf-style ASCII format string.
    ... - Format arguments consumed only by this routine.

Return Value:

    None. Formatting failures are intentionally dropped because logging must not
    change IOCTL completion status.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (VOID)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

static VOID
KswordARKWin32kFillDefaultRequest(
    _Out_ KSWORD_ARK_WIN32K_QUERY_REQUEST* QueryRequest
    )
/*++

Routine Description:

    Fill the default read-only Win32k query request used when R3 sends no input
    packet with a METHOD_BUFFERED query.

Arguments:

    QueryRequest - Receives the default version, diagnostic flag, and traversal
    budget.

Return Value:

    None. The caller supplies a stack request object.

--*/
{
    RtlZeroMemory(QueryRequest, sizeof(*QueryRequest));
    QueryRequest->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    QueryRequest->flags = KSWORD_ARK_WIN32K_QUERY_FLAG_INCLUDE_DIAGNOSTICS;
    QueryRequest->maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
}

static NTSTATUS
KswordARKWin32kRetrieveRequest(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Outptr_ KSWORD_ARK_WIN32K_QUERY_REQUEST** QueryRequestOut,
    _Out_ KSWORD_ARK_WIN32K_QUERY_REQUEST* DefaultRequest
    )
/*++

Routine Description:

    Retrieve an optional Win32k query packet or synthesize a bounded default
    request for legacy callers that provide output-only METHOD_BUFFERED IOCTLs.

Arguments:

    Request - Current framework request.
    InputBufferLength - Input length reported by the central dispatch callback.
    QueryRequestOut - Receives the caller packet or DefaultRequest.
    DefaultRequest - Stack storage for a synthesized request.

Return Value:

    STATUS_SUCCESS when a usable request is available; otherwise a buffer or
    protocol validation status.

--*/
{
    PVOID inputBuffer = NULL;
    size_t actualInputLength = 0U;
    BOOLEAN hasInput = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (QueryRequestOut == NULL || DefaultRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *QueryRequestOut = NULL;
    status = KswordARKRetrieveOptionalInputBuffer(
        Request,
        InputBufferLength,
        sizeof(KSWORD_ARK_WIN32K_QUERY_REQUEST),
        &inputBuffer,
        &actualInputLength,
        &hasInput);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (hasInput) {
        *QueryRequestOut = (KSWORD_ARK_WIN32K_QUERY_REQUEST*)inputBuffer;
        if ((*QueryRequestOut)->version != KSWORD_ARK_WIN32K_PROTOCOL_VERSION) {
            return STATUS_REVISION_MISMATCH;
        }
        UNREFERENCED_PARAMETER(actualInputLength);
        return STATUS_SUCCESS;
    }

    KswordARKWin32kFillDefaultRequest(DefaultRequest);
    *QueryRequestOut = DefaultRequest;
    UNREFERENCED_PARAMETER(actualInputLength);
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKWin32kIoctlQueryCommon(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t RequiredOutputLength,
    _In_z_ PCSTR OperationName,
    _In_ KSWORD_ARK_WIN32K_QUERY_COLLECTOR Collector,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Common adapter for read-only Win32k IOCTLs. It performs WDF buffer retrieval,
    optional request handling, collector dispatch, and compact success/error
    logging while leaving all audit logic inside src/features/win32k.

Arguments:

    Device - Current framework device object.
    Request - Current framework request.
    InputBufferLength - Input length reported by dispatch.
    RequiredOutputLength - Fixed response header size required by the collector.
    OperationName - Short ASCII operation name for logs.
    Collector - Feature collector routine that fills the response packet.
    BytesReturned - Receives bytes written by the collector.

Return Value:

    STATUS_SUCCESS when the collector produced a response; otherwise the first
    validation or collector failure status.

--*/
{
    KSWORD_ARK_WIN32K_QUERY_REQUEST* queryRequest = NULL;
    KSWORD_ARK_WIN32K_QUERY_REQUEST defaultRequest;
    PVOID outputBuffer = NULL;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesReturned == NULL || OperationName == NULL || Collector == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKWin32kRetrieveRequest(
        Request,
        InputBufferLength,
        &queryRequest,
        &defaultRequest);
    if (!NT_SUCCESS(status)) {
        KswordARKWin32kIoctlLog(Device, "Error", "R0 win32k-%s ioctl: input invalid, status=0x%08X.", OperationName, (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        RequiredOutputLength,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKWin32kIoctlLog(Device, "Error", "R0 win32k-%s ioctl: output invalid, status=0x%08X.", OperationName, (unsigned int)status);
        return status;
    }

    status = Collector(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKWin32kIoctlLog(Device, "Error", "R0 win32k-%s query failed: status=0x%08X, outBytes=%Iu.", OperationName, (unsigned int)status, *BytesReturned);
        return status;
    }

    KswordARKWin32kIoctlLog(Device, "Info", "R0 win32k-%s query success: outBytes=%Iu.", OperationName, *BytesReturned);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKWin32kIoctlQueryProfileStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the read-only win32k profile/status IOCTL.

Arguments:

    Device - Current framework device object.
    Request - Current framework request.
    InputBufferLength - Optional query request size.
    OutputBufferLength - Dispatch-supplied output length; WDF performs the final
    buffer retrieval in the common adapter.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from the common Win32k IOCTL adapter.

--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    return KswordARKWin32kIoctlQueryCommon(
        Device,
        Request,
        InputBufferLength,
        KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE,
        "profile-status",
        KswordARKWin32kQueryProfileStatus,
        BytesReturned);
}

NTSTATUS
KswordARKWin32kIoctlQueryWindows(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the read-only HWND/tagWND snapshot IOCTL.

Arguments:

    Device - Current framework device object.
    Request - Current framework request.
    InputBufferLength - Optional query request size.
    OutputBufferLength - Dispatch-supplied output length, referenced only by the
    common WDF output buffer retrieval path.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from the common Win32k IOCTL adapter.

--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    return KswordARKWin32kIoctlQueryCommon(
        Device,
        Request,
        InputBufferLength,
        KSWORD_ARK_WIN32K_WINDOW_RESPONSE_HEADER_SIZE,
        "windows",
        KswordARKWin32kQueryWindowSnapshot,
        BytesReturned);
}

NTSTATUS
KswordARKWin32kIoctlQueryGuiThreads(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the read-only GUI thread/tagQ snapshot IOCTL.

Arguments:

    Device - Current framework device object.
    Request - Current framework request.
    InputBufferLength - Optional query request size.
    OutputBufferLength - Dispatch-supplied output length, referenced only by the
    common WDF output buffer retrieval path.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from the common Win32k IOCTL adapter.

--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    return KswordARKWin32kIoctlQueryCommon(
        Device,
        Request,
        InputBufferLength,
        KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE,
        "gui-threads",
        KswordARKWin32kQueryGuiThreadSnapshot,
        BytesReturned);
}

NTSTATUS
KswordARKWin32kIoctlQueryHotkeysPdb(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the read-only PDB-backed hotkey snapshot skeleton IOCTL.

Arguments:

    Device - Current framework device object.
    Request - Current framework request.
    InputBufferLength - Optional query request size.
    OutputBufferLength - Dispatch-supplied output length, referenced only by the
    common WDF output buffer retrieval path.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from the common Win32k IOCTL adapter.

--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    return KswordARKWin32kIoctlQueryCommon(
        Device,
        Request,
        InputBufferLength,
        KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE,
        "hotkeys-pdb",
        KswordARKWin32kQueryHotkeySnapshot,
        BytesReturned);
}

NTSTATUS
KswordARKWin32kIoctlQueryHooksPdb(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle the read-only PDB-backed hook snapshot skeleton IOCTL.

Arguments:

    Device - Current framework device object.
    Request - Current framework request.
    InputBufferLength - Optional query request size.
    OutputBufferLength - Dispatch-supplied output length, referenced only by the
    common WDF output buffer retrieval path.
    BytesReturned - Receives response bytes.

Return Value:

    NTSTATUS from the common Win32k IOCTL adapter.

--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    return KswordARKWin32kIoctlQueryCommon(
        Device,
        Request,
        InputBufferLength,
        KSWORD_ARK_WIN32K_HOOK_RESPONSE_HEADER_SIZE,
        "hooks-pdb",
        KswordARKWin32kQueryHookSnapshot,
        BytesReturned);
}
