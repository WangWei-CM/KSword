// Independent, opt-in system sampler. It never starts or controls KSword.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <evntrace.h>
#include <objbase.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
HANDLE cancelEvent = nullptr;
BOOL WINAPI onConsoleControl(DWORD kind)
{
    if (kind != CTRL_C_EVENT && kind != CTRL_BREAK_EVENT && kind != CTRL_CLOSE_EVENT)
        return FALSE;
    if (cancelEvent) SetEvent(cancelEvent);
    return TRUE;
}

bool writeNew(const std::filesystem::path& path, const std::string& text)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
        &written, nullptr) && written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

bool publishReady(const std::filesystem::path& path, const std::string& text)
{
    const std::filesystem::path temporary(path.wstring() + L".writing");
    if (!writeNew(temporary, text)) return false;
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary.c_str()); // Only the temporary file successfully created above.
    return false;
}

bool enableProfilePrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SYSTEM_PROFILE_NAME, &privileges.Privileges[0].Luid) != FALSE;
    if (ok) {
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr)
            && GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

constexpr GUID profileGuid{0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
constexpr GUID threadGuid{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr ULONG eventFlags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD
    | EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_PROFILE | EVENT_TRACE_FLAG_CSWITCH;

std::array<CLASSIC_EVENT_ID, 2> stackEvents()
{
    std::array<CLASSIC_EVENT_ID, 2> events{};
    events[0].EventGuid = profileGuid;
    events[0].Type = 46; // PerfInfo::SampledProfile (documented classic opcode).
    events[1].EventGuid = threadGuid;
    events[1].Type = 36; // Thread::CSwitch.
    return events;
}

bool unsignedNumber(const std::wstring& value, DWORD& out)
{
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        return false;
    try {
        const auto parsed = std::stoull(value);
        if (parsed > MAXDWORD) return false;
        out = static_cast<DWORD>(parsed);
        return true;
    } catch (...) { return false; }
}

struct StopOwnedSession {
    TRACEHANDLE handle;
    EVENT_TRACE_PROPERTIES* properties;
    ~StopOwnedSession()
    {
        // Exception fallback, including allocation failure while writing the ready marker.
        if (handle) ControlTraceW(handle, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
    }
};
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring(argv[1]) == L"--self-test") {
        DWORD number = 0;
        const auto events = stackEvents();
        const bool ok = events[0].Type == 46 && events[1].Type == 36
            && unsignedNumber(L"180", number) && number == 180
            && !unsignedNumber(L"-1", number) && !unsignedNumber(L"4294967296", number);
        std::cout << (ok ? "PASS: configuration/parser only; no ETW session started\n" : "FAIL\n");
        return ok ? 0 : 1;
    }
    std::filesystem::path output, receipt, ready;
    DWORD seconds = 0, ownerPid = 0;
    bool capture = false, valid = true;
    for (int i = 1; i < argc && valid; ++i) {
        const std::wstring option(argv[i]);
        if (option == L"--capture" && !capture) capture = true;
        else if (i + 1 == argc) valid = false;
        else if (option == L"--output" && output.empty()) output = argv[++i];
        else if (option == L"--receipt" && receipt.empty()) receipt = argv[++i];
        else if (option == L"--ready" && ready.empty()) ready = argv[++i];
        else if (option == L"--seconds" && seconds == 0) valid = unsignedNumber(argv[++i], seconds);
        else if (option == L"--owner-pid" && ownerPid == 0) valid = unsignedNumber(argv[++i], ownerPid);
        else valid = false;
    }
    if (!valid || !capture || seconds < 5 || seconds > 300 || ownerPid == 0
        || output.empty() || receipt.empty() || ready.empty()
        || !output.is_absolute() || !receipt.is_absolute() || !ready.is_absolute()
        || output == receipt || output == ready || receipt == ready
        || std::filesystem::exists(output) || std::filesystem::exists(receipt)
        || std::filesystem::exists(ready)) {
        std::cerr << "Usage: --capture --seconds 5..300 --owner-pid PID --output ABS.etl"
                     " --receipt ABS.json --ready ABS.json (new files only)\n";
        return 2;
    }
    const HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, ownerPid);
    if (!owner || !enableProfilePrivilege()) {
        if (owner) CloseHandle(owner);
        std::cerr << "Cannot observe owner or enable profiling privilege; use an elevated console.\n";
        return 3;
    }
    cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancelEvent) { CloseHandle(owner); return 3; }
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
    GUID sessionGuid{};
    wchar_t guidText[40]{};
    if (FAILED(CoCreateGuid(&sessionGuid)) || !StringFromGUID2(sessionGuid, guidText, 40)) {
        CloseHandle(owner); CloseHandle(cancelEvent); cancelEvent = nullptr; return 3;
    }
    const std::wstring sessionName = L"KSwordUI-CPU-" + std::wstring(guidText);
    const auto path = output.wstring();
    const size_t propertyBytes = sizeof(EVENT_TRACE_PROPERTIES)
        + (sessionName.size() + path.size() + 2) * sizeof(wchar_t);
    if (propertyBytes > MAXDWORD) return 3;
    std::vector<unsigned char> storage(propertyBytes, 0);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(propertyBytes);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.Guid = sessionGuid; // A fresh system logger, never NT Kernel Logger.
    properties->Wnode.ClientContext = 1; // QPC timestamps; do not alter global sampling interval.
    properties->LogFileMode = EVENT_TRACE_SYSTEM_LOGGER_MODE | EVENT_TRACE_FILE_MODE_SEQUENTIAL;
    properties->MaximumFileSize = 1024; // MB, bounded sequential file; no circular overwrite.
    properties->BufferSize = 64; // KB; ETW can increase the minimum for processor count.
    properties->MinimumBuffers = 32;
    properties->MaximumBuffers = 256;
    properties->FlushTimer = 1;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    properties->LogFileNameOffset = properties->LoggerNameOffset
        + static_cast<ULONG>((sessionName.size() + 1) * sizeof(wchar_t));
    memcpy(storage.data() + properties->LoggerNameOffset, sessionName.c_str(),
        (sessionName.size() + 1) * sizeof(wchar_t));
    memcpy(storage.data() + properties->LogFileNameOffset, path.c_str(),
        (path.size() + 1) * sizeof(wchar_t));

    LARGE_INTEGER frequency{}, started{}, stopped{};
    QueryPerformanceFrequency(&frequency);
    TRACEHANDLE session = 0;
    const ULONG startStatus = StartTraceW(&session, sessionName.c_str(), properties);
    StopOwnedSession owned{startStatus == ERROR_SUCCESS ? session : 0, properties};
    ULONG stackStatus = ERROR_NOT_READY, flagsStatus = ERROR_NOT_READY, stopStatus = ERROR_NOT_READY;
    DWORD waitResult = WAIT_FAILED;
    bool readyWritten = false;
    QueryPerformanceCounter(&started);
    if (startStatus == ERROR_SUCCESS) {
        auto events = stackEvents();
        stackStatus = TraceSetInformation(session, TraceStackTracingInfo,
            events.data(), static_cast<ULONG>(sizeof(events)));
        if (stackStatus == ERROR_SUCCESS) {
            ULONG flags[8]{eventFlags};
            flagsStatus = TraceSetInformation(session, TraceSystemTraceEnableFlagsInfo,
                flags, sizeof(flags));
        }
        if (flagsStatus == ERROR_SUCCESS) {
            readyWritten = publishReady(ready, "{\"ready\":true,\"qpc_frequency\":\""
                + std::to_string(frequency.QuadPart) + "\",\"started_qpc\":\""
                + std::to_string(started.QuadPart) + "\"}\n");
            if (readyWritten) {
                const HANDLE waits[]{cancelEvent, owner};
                waitResult = WaitForMultipleObjects(2, waits, FALSE, seconds * 1000);
            }
        }
        // The handle is owned only after a successful StartTrace. Never attach to/stop another session.
        stopStatus = ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
        if (stopStatus == ERROR_SUCCESS) owned.handle = 0;
    }
    QueryPerformanceCounter(&stopped);
    const bool clean = startStatus == 0 && stackStatus == 0 && flagsStatus == 0
        && stopStatus == 0 && readyWritten && waitResult == WAIT_TIMEOUT
        && properties->EventsLost == 0 && properties->LogBuffersLost == 0
        && properties->RealTimeBuffersLost == 0;
    const std::string report = "{\"schema_version\":1,\"complete\":" + std::string(clean ? "true" : "false")
        + ",\"start_status\":" + std::to_string(startStatus)
        + ",\"stack_status\":" + std::to_string(stackStatus)
        + ",\"flags_status\":" + std::to_string(flagsStatus)
        + ",\"stop_status\":" + std::to_string(stopStatus)
        + ",\"wait_result\":" + std::to_string(waitResult)
        + ",\"events_lost\":" + std::to_string(properties->EventsLost)
        + ",\"log_buffers_lost\":" + std::to_string(properties->LogBuffersLost)
        + ",\"realtime_buffers_lost\":" + std::to_string(properties->RealTimeBuffersLost)
        + ",\"buffers_written\":" + std::to_string(properties->BuffersWritten)
        + ",\"qpc_frequency\":\"" + std::to_string(frequency.QuadPart)
        + "\",\"started_qpc\":\"" + std::to_string(started.QuadPart)
        + "\",\"stopped_qpc\":\"" + std::to_string(stopped.QuadPart)
        + "\",\"actual_stack_coverage_verified\":false}\n";
    const bool reported = writeNew(receipt, report);
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    CloseHandle(owner); CloseHandle(cancelEvent); cancelEvent = nullptr;
    if (!reported) std::cerr << "Cannot persist capture receipt.\n";
    if (stopStatus != ERROR_SUCCESS && startStatus == ERROR_SUCCESS)
        std::wcerr << L"Owned session stop failed (" << stopStatus << L"): " << sessionName << L'\n';
    return clean && reported ? 0 : 4;
}
