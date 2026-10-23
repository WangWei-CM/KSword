/* Compile the actual registry/dispatcher; do not recreate the role decision. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "dispatch_protocol_replay.h"

#ifndef EXTERN_C_START
#define EXTERN_C_START
#define EXTERN_C_END
#endif
#define NT_SUCCESS(s) ((LONG)(s) >= 0)
#define WDF_NO_HANDLE NULL
#define TRACE_LEVEL_INFORMATION 1U
#define TRACE_LEVEL_ERROR 2U
#define TRACE_QUEUE 1U
#define KSWORD_ARK_LOG_ENTRY_MAX_BYTES 1024U
#define KSWORD_BUGCHECK_TRACE_KIND_IOCTL_BEGIN 1U
#define KSWORD_BUGCHECK_TRACE_KIND_IOCTL_END 2U
#define KSWORD_BUGCHECK_EVIDENCE_EVENT_TEXT 64U

typedef LONG NTSTATUS;
typedef struct { BOOLEAN controller; } TEST_DEVICE;
typedef TEST_DEVICE* WDFDEVICE;
typedef TEST_DEVICE* WDFQUEUE;
typedef struct { ULONG completions; NTSTATUS status; size_t bytes; UCHAR output[64]; } TEST_REQUEST;
typedef TEST_REQUEST* WDFREQUEST;
#include "dispatch_registry_replay.h"

static ULONG checks, failures, coreEnters, coreLeaves, coreReaders;
static ULONG genericCalls, controllerCalls, logs, capabilityCalls, evidenceCalls;
static ULONG controllerOperations[4];
static ULONG readCalls;
static BOOLEAN coreAvailable, capabilityAllowed, outputAvailable;
static NTSTATUS genericStatus, controllerStatus;
static size_t lastInput, lastOutput;
static TEST_DEVICE mainDevice = { FALSE }, controllerDevice = { TRUE };
static TEST_REQUEST request;

#define CHECK(name, expression) do { ++checks; if (!(expression)) { \
    ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static VOID Reset(VOID)
{
    memset(&request, 0, sizeof(request));
    memset(controllerOperations, 0, sizeof(controllerOperations));
    coreEnters = coreLeaves = coreReaders = genericCalls = controllerCalls = 0U;
    logs = capabilityCalls = evidenceCalls = 0U;
    readCalls = 0U;
    coreAvailable = capabilityAllowed = outputAvailable = TRUE;
    genericStatus = controllerStatus = STATUS_SUCCESS;
    lastInput = lastOutput = 0U;
}

static VOID WdfRequestCompleteWithInformation(WDFREQUEST req, NTSTATUS status, size_t bytes)
{
    ++req->completions; req->status = status; req->bytes = bytes;
}
static WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE queue) { return queue; }
static NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST req, size_t minimum, PVOID* buffer, size_t* bytes)
{
    if (!outputAvailable || minimum > sizeof(req->output)) return STATUS_BUFFER_TOO_SMALL;
    *buffer = req->output; *bytes = sizeof(req->output); return STATUS_SUCCESS;
}
static NTSTATUS KswordARKDriverReadNextLogLine(WDFDEVICE device, PVOID buffer, size_t bytes, size_t* written)
{
    (void)buffer; (void)bytes; ++readCalls; *written = 5U;
    CHECK("log read uses primary context with rundown", !device->controller && coreReaders == 1U);
    return STATUS_SUCCESS;
}
static BOOLEAN KswordARKStorageControllerIsDevice(WDFDEVICE device) { return device->controller; }
static BOOLEAN KswordARKDriverCoreEnterRequest(VOID)
{
    ++coreEnters;
    if (!coreAvailable) return FALSE;
    ++coreReaders; return TRUE;
}
static VOID KswordARKDriverCoreLeaveRequest(VOID)
{
    ++coreLeaves;
    CHECK("core leave balances admission", coreReaders == 1U);
    --coreReaders;
}
static VOID TraceEvents(ULONG level, ULONG category, const char* format, ...)
{ (void)level; (void)category; (void)format; }
static NTSTATUS RtlStringCbVPrintfA(char* text, size_t bytes, const char* format, va_list args)
{ (void)format; (void)args; if (bytes) text[0] = 0; return STATUS_SUCCESS; }
static NTSTATUS KswordARKDriverEnqueueLogFrame(WDFDEVICE device, const char* level, const char* text)
{
    (void)level; (void)text; ++logs;
    CHECK("primary context is accessed only with core admission", !device->controller && coreReaders == 1U);
    return STATUS_SUCCESS;
}
static BOOLEAN KswordARKCapabilityIsIoctlAllowed(ULONG64 required, NTSTATUS* status)
{
    (void)required; ++capabilityCalls;
    CHECK("capability gate holds core admission", coreReaders == 1U);
    if (!capabilityAllowed) { *status = STATUS_NOT_SUPPORTED; return FALSE; }
    return TRUE;
}
static VOID KswordARKCapabilityRecordLastError(NTSTATUS status, const char* source, const char* detail)
{ (void)status; (void)source; (void)detail; CHECK("error record holds admission", coreReaders == 1U); }
static VOID KswordARKBugcheckTraceRecord(ULONG kind, ULONG ioctl, NTSTATUS status, const char* name, ULONG bytes)
{
    (void)kind; (void)ioctl; (void)status; (void)name; (void)bytes; ++evidenceCalls;
    CHECK("evidence holds core admission", coreReaders == 1U);
}
static VOID KswordARKBugcheckContextOperation(VOID)
{ ++evidenceCalls; CHECK("context capture holds core admission", coreReaders == 1U); }
static NTSTATUS GenericHandler(WDFDEVICE device, WDFREQUEST req, size_t input, size_t output, size_t* bytes)
{
    (void)req; ++genericCalls; lastInput = input; lastOutput = output; *bytes = 17U;
    CHECK("generic handler has the primary role and rundown", !device->controller && coreReaders == 1U);
    return genericStatus;
}
static VOID ControllerHandler(WDFDEVICE device, WDFREQUEST req, ULONG operation)
{
    ++controllerCalls; ++controllerOperations[operation];
    CHECK("controller bypasses unrelated primary state", device->controller && coreReaders == 0U);
    WdfRequestCompleteWithInformation(req, controllerStatus, 23U);
}
static VOID KswordARKStorageControllerQuery(WDFDEVICE device, WDFREQUEST req) { ControllerHandler(device, req, 0U); }
static VOID KswordARKStorageControllerControl(WDFDEVICE device, WDFREQUEST req) { ControllerHandler(device, req, 1U); }
static VOID KswordARKStorageControllerTransfer(WDFDEVICE device, WDFREQUEST req) { ControllerHandler(device, req, 2U); }
static VOID KswordARKStorageControllerAudit(WDFDEVICE device, WDFREQUEST req) { ControllerHandler(device, req, 3U); }

#include "dispatch_feature_stubs.h"
#include "dispatch_source_replay.h"

static VOID Dispatch(WDFDEVICE device, ULONG code)
{
    KswordARKDriverEvtIoDeviceControl(device, &request, 83U, 41U, code);
}

int main(VOID)
{
    ULONG index, controllerEntries = 0U;
    const ULONG count = KswordARKGetRegisteredIoctlCount();
    const ULONG operations[] = { IOCTL_KSWORD_ARK_QUERY_STORAGE_CONTROLLER,
        IOCTL_KSWORD_ARK_CONTROL_STORAGE_CONTROLLER, IOCTL_KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER,
        IOCTL_KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT };
    const KSWORD_ARK_IOCTL_ENTRY* generic = KswordARKLookupIoctlEntry(IOCTL_KSWORD_ARK_TERMINATE_PROCESS);
    const KSWORD_ARK_IOCTL_ENTRY* backup = KswordARKLookupIoctlEntry(IOCTL_KSWORD_ARK_RXPF_QUERY_BACKUP);
    const KSWORD_ARK_IOCTL_ENTRY* fileSubmit = KswordARKLookupIoctlEntry(IOCTL_KSWORD_ARK_FILE_IRP_SUBMIT);
    CHECK("registry count covers every production row", count == KSWORD_TEST_EXPECTED_REGISTRY_COUNT);
    CHECK("real registry has no duplicate code", KswordARKGetDuplicateIoctlCount() == 0U);
    CHECK("registry lookup is bounded", KswordARKGetIoctlEntryByIndex(count) == NULL);
    for (index = 0U; index < count; ++index) {
        const KSWORD_ARK_IOCTL_ENTRY* entry = KswordARKGetIoctlEntryByIndex(index);
        const BOOLEAN controller = (entry->Flags & KSWORD_ARK_IOCTL_FLAG_CONTROLLER_ONLY) != 0U;
        CHECK("each full control code routes to its exact registry row", KswordARKLookupIoctlEntry(entry->IoControlCode) == entry);
        if (controller) ++controllerEntries;
        Reset(); Dispatch(&controllerDevice, entry->IoControlCode);
        CHECK("controller endpoint completes each request once", request.completions == 1U);
        CHECK("controller endpoint can reach only controller entries", controllerCalls == (controller ? 1U : 0U) && genericCalls == 0U);
        CHECK("controller endpoint never touches primary state", coreEnters == 0U && coreLeaves == 0U && logs == 0U && capabilityCalls == 0U && evidenceCalls == 0U);
        CHECK("wrong controller role returns deterministic denial", request.status == (controller ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST));
        Reset(); Dispatch(&mainDevice, entry->IoControlCode);
        CHECK("primary endpoint never reaches controller backend", controllerCalls == 0U);
        CHECK("primary endpoint admits only generic entries", genericCalls == (controller ? 0U : 1U));
        CHECK("primary endpoint completes exactly once", request.completions == 1U);
        CHECK("primary rundown balances all rows", coreReaders == 0U && coreLeaves == (controller ? 0U : 1U));
        if (controller) CHECK("primary rejects controller before any primary read", coreEnters == 0U && logs == 0U && capabilityCalls == 0U && evidenceCalls == 0U);
    }
    CHECK("backup query has a distinct full control code from file submit", IOCTL_KSWORD_ARK_RXPF_QUERY_BACKUP != IOCTL_KSWORD_ARK_FILE_IRP_SUBMIT);
    CHECK("backup query routes to the backup handler", backup != NULL && backup->Handler == KswordARKRxpfIoctlQueryBackup);
    CHECK("file submit keeps its original handler", fileSubmit != NULL && fileSubmit->Handler == KswordARKFileIrpIoctlSubmit);
    CHECK("exactly four controller-only entries", controllerEntries == 4U);
    for (index = 0U; index < 4U; ++index) {
        Reset(); controllerStatus = STATUS_INVALID_PARAMETER; Dispatch(&controllerDevice, operations[index]);
        CHECK("correct controller wrapper owns failed completion", controllerOperations[index] == 1U && request.completions == 1U && request.status == STATUS_INVALID_PARAMETER && request.bytes == 23U);
    }
    Reset(); Dispatch(&controllerDevice, 0xFFFFFFFFUL);
    CHECK("unknown controller IOCTL does not read primary context", request.completions == 1U && request.status == STATUS_INVALID_DEVICE_REQUEST && logs == 0U && coreEnters == 0U);
    Reset(); Dispatch(&mainDevice, 0xFFFFFFFFUL);
    CHECK("unknown primary IOCTL logs receipt and denial with rundown", request.completions == 1U && request.status == STATUS_INVALID_DEVICE_REQUEST && coreEnters == 1U && coreLeaves == 1U && logs == 2U);
    Reset(); capabilityAllowed = FALSE; Dispatch(&mainDevice, generic->IoControlCode);
    CHECK("capability denial releases rundown without handler", genericCalls == 0U && coreLeaves == 1U && request.completions == 1U && request.status == STATUS_NOT_SUPPORTED);
    Reset(); coreAvailable = FALSE; Dispatch(&mainDevice, generic->IoControlCode);
    CHECK("core shutdown gate precedes all primary reads", request.status == STATUS_DELETE_PENDING && request.completions == 1U && genericCalls == 0U && logs == 0U && evidenceCalls == 0U && capabilityCalls == 0U && coreLeaves == 0U);
    Reset(); coreAvailable = FALSE; Dispatch(&controllerDevice, operations[0]);
    CHECK("controller-only startup does not need the primary runtime", controllerCalls == 1U && request.status == STATUS_SUCCESS && coreEnters == 0U);
    Reset(); genericStatus = STATUS_PENDING; Dispatch(&mainDevice, generic->IoControlCode);
    CHECK("pending generic completion remains backend owned", request.completions == 0U && coreLeaves == 1U && coreReaders == 0U);
    Reset(); genericStatus = STATUS_ACCESS_DENIED; Dispatch(&mainDevice, generic->IoControlCode);
    CHECK("synchronous generic failure completes once and releases rundown", request.completions == 1U && request.status == STATUS_ACCESS_DENIED && request.bytes == 17U && coreLeaves == 1U);
    CHECK("production handler sees input/output counts in correct order", lastInput == 41U && lastOutput == 83U);
    Reset(); KswordARKDriverEvtIoRead(&mainDevice, &request, 64U);
    CHECK("log read completes once and releases rundown", readCalls == 1U && request.completions == 1U && request.bytes == 5U && coreLeaves == 1U);
    Reset(); outputAvailable = FALSE; KswordARKDriverEvtIoRead(&mainDevice, &request, 64U);
    CHECK("log buffer failure releases rundown", readCalls == 0U && request.completions == 1U && request.status == STATUS_BUFFER_TOO_SMALL && coreLeaves == 1U);
    Reset(); coreAvailable = FALSE; KswordARKDriverEvtIoRead(&mainDevice, &request, 64U);
    CHECK("log read shutdown never accesses context", readCalls == 0U && request.completions == 1U && request.status == STATUS_DELETE_PENDING && coreLeaves == 0U);
    Reset(); KswordARKDriverEvtIoRead(&controllerDevice, &request, 64U);
    CHECK("controller endpoint cannot read the primary log ring", readCalls == 0U && request.completions == 1U && request.status == STATUS_INVALID_DEVICE_REQUEST && coreEnters == 0U);
    printf("Controller dispatcher role regression: %lu checks, %lu failures, %lu registry entries\n", checks, failures, count);
    return failures ? 1 : 0;
}
