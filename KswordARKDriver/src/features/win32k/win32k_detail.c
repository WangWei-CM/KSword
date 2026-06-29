/*++

Module Name:

    win32k_detail.c

Abstract:

    Read-only win32k single-window detail readiness query.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"

#include <ntstrsafe.h>

typedef struct _KSWORD_ARK_WIN32K_PROFILE_ONE_ENTRY_RESPONSE
{
    KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE Header;
    KSWORD_ARK_WIN32K_SESSION_ENTRY ExtraEntry;
} KSWORD_ARK_WIN32K_PROFILE_ONE_ENTRY_RESPONSE, *PKSWORD_ARK_WIN32K_PROFILE_ONE_ENTRY_RESPONSE;

NTSTATUS
KswordARKWin32kQueryWindowDetail(
    _Out_writes_bytes_(OutputBufferLength) KSWORD_ARK_WIN32K_WINDOW_DETAIL_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_WINDOW_DETAIL_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    返回单窗口详情查询的只读 readiness 响应。中文说明：当前 R0 还没有
    已激活的 tagWND 私有 profile，因此本函数不信任 R3 提供的 HWND 去反查
    tagWND，只报告 win32k 模块、capability 和 offset readiness。

Arguments:

    Response - METHOD_BUFFERED 输出响应。
    OutputBufferLength - 输出缓冲区长度。
    Request - 固定请求，包含 HWND/PID/TID 过滤线索。
    BytesWrittenOut - 返回响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；profile 缺失通过 response.status 表达。

--*/
{
    KSWORD_ARK_WIN32K_PROFILE_ONE_ENTRY_RESPONSE profileResponse;
    KSWORD_ARK_WIN32K_QUERY_REQUEST profileRequest;
    size_t profileBytes = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (Response == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < sizeof(*Response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    *BytesWrittenOut = sizeof(*Response);
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    Response->status = KSWORD_ARK_WIN32K_STATUS_UNKNOWN;
    Response->processId = Request->processId;
    Response->threadId = Request->threadId;
    Response->flags = Request->flags;
    Response->hwnd = Request->hwnd;
    KswordARKWin32kInitializeOffsets(&Response->fieldOffsets);

    if (Request->version != KSWORD_ARK_WIN32K_PROTOCOL_VERSION) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
        Response->lastStatus = STATUS_REVISION_MISMATCH;
        KswordARKWin32kCopyWideText(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"Win32k window detail request version mismatch.");
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&profileRequest, sizeof(profileRequest));
    profileRequest.version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    profileRequest.flags = KSWORD_ARK_WIN32K_QUERY_FLAG_INCLUDE_DIAGNOSTICS;
    profileRequest.processId = Request->processId;
    profileRequest.threadId = Request->threadId;
    profileRequest.maxEntries = 1UL;

    RtlZeroMemory(&profileResponse, sizeof(profileResponse));
    status = KswordARKWin32kQueryProfileStatus(
        &profileResponse,
        sizeof(profileResponse),
        &profileRequest,
        &profileBytes);

    if (!NT_SUCCESS(status)) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_ENUM_FAILED;
        Response->lastStatus = status;
        KswordARKWin32kCopyWideText(Response->detail, KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS, L"Win32k profile readiness query failed.");
        return STATUS_SUCCESS;
    }

    Response->capabilityMask = profileResponse.Header.capabilityMask;
    Response->missingCapabilityMask = profileResponse.Header.missingCapabilityMask | KSWORD_ARK_WIN32K_CAP_TAGWND_PROFILE;
    Response->win32k = profileResponse.Header.win32k;
    Response->win32kbase = profileResponse.Header.win32kbase;
    Response->win32kfull = profileResponse.Header.win32kfull;
    Response->fieldOffsets = profileResponse.Header.fieldOffsets;
    Response->fieldFlags = KSWORD_ARK_WIN32K_FIELD_DETAIL_PROFILE | KSWORD_ARK_WIN32K_FIELD_DETAIL_OFFSETS;
    Response->lastStatus = STATUS_NOT_FOUND;

    if ((Response->capabilityMask & KSWORD_ARK_WIN32K_CAP_TAGWND_PROFILE) == 0ULL) {
        Response->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
        KswordARKWin32kCopyWideText(
            Response->detail,
            KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS,
            L"Window detail is waiting for an active tagWND PDB profile; HWND was accepted only as a display key.");
        return STATUS_SUCCESS;
    }

    Response->status = KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
    KswordARKWin32kCopyWideText(
        Response->detail,
        KSWORD_ARK_RUNTIME_DETAIL_TEXT_CHARS,
        L"tagWND profile appears present, but single-window tagWND reader has not been wired yet.");
    return STATUS_SUCCESS;
}
