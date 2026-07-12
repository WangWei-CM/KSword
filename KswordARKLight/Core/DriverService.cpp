#include "DriverService.h"

#include "Common.h"
#include "PathUtils.h"
#include "resource.h"
#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <winsvc.h>

namespace Ksword::Core {
namespace {
constexpr wchar_t kDriverFileName[] = L"KswordARK.sys";
constexpr wchar_t kDriverServiceName[] = L"KswordARK";
constexpr wchar_t kDriverDisplayName[] = L"KswordARK R0 Driver";
constexpr DWORD kDriverIoChunkBytes = 64 * 1024;

// EmbeddedDriverPayload describes the immutable bytes loaded from the EXE
// resource table. Inputs are provided by LoadEmbeddedDriverPayload; processing
// keeps only a pointer/size view owned by the module; there is no destructor work
// because Win32 owns the mapped resource for the lifetime of the process.
struct EmbeddedDriverPayload {
    const std::uint8_t* bytes = nullptr;
    DWORD size = 0;
};

// LoadEmbeddedDriverPayload locates the RCDATA driver payload in this module.
// There is no input; processing calls FindResource/LoadResource/LockResource;
// output is true with a valid byte view, or false with a user-facing error.
bool LoadEmbeddedDriverPayload(EmbeddedDriverPayload& payload, std::wstring& error) {
    payload = {};
    error.clear();

    HMODULE module = ::GetModuleHandleW(nullptr);
    if (!module) {
        error = L"GetModuleHandleW failed: " + LastErrorMessage();
        return false;
    }

    HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(IDR_KSWORDARKLIGHT_DRIVER_SYS), RT_RCDATA);
    if (!resource) {
        error = L"FindResourceW(IDR_KSWORDARKLIGHT_DRIVER_SYS) failed: " + LastErrorMessage();
        return false;
    }

    const DWORD size = ::SizeofResource(module, resource);
    if (size == 0) {
        error = L"Embedded KswordARK.sys resource is empty.";
        return false;
    }

    HGLOBAL loaded = ::LoadResource(module, resource);
    if (!loaded) {
        error = L"LoadResource(IDR_KSWORDARKLIGHT_DRIVER_SYS) failed: " + LastErrorMessage();
        return false;
    }

    const void* bytes = ::LockResource(loaded);
    if (!bytes) {
        error = L"LockResource(IDR_KSWORDARKLIGHT_DRIVER_SYS) returned no data.";
        return false;
    }

    payload.bytes = static_cast<const std::uint8_t*>(bytes);
    payload.size = size;
    return true;
}

// EmbeddedDriverPayloadAvailable checks whether the EXE carries the driver
// resource. There is no input; processing performs the same resource lookup used
// by extraction; output is a boolean for status text only.
bool EmbeddedDriverPayloadAvailable() {
    EmbeddedDriverPayload payload;
    std::wstring ignoredError;
    return LoadEmbeddedDriverPayload(payload, ignoredError);
}

// ExistingDriverMatchesPayload compares the on-disk driver with the embedded
// resource without rewriting it. Inputs are the path and payload view; processing
// reads the file in bounded chunks; output is true when comparison succeeded and
// writes the equality result to matches.
bool ExistingDriverMatchesPayload(
    const std::wstring& path,
    const EmbeddedDriverPayload& payload,
    bool& matches,
    std::wstring& error) {
    matches = false;
    error.clear();

    UniqueHandle file(::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.valid()) {
        error = L"CreateFileW for existing driver failed: " + LastErrorMessage();
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(file.get(), &fileSize)) {
        error = L"GetFileSizeEx for existing driver failed: " + LastErrorMessage();
        return false;
    }
    if (fileSize.QuadPart != static_cast<LONGLONG>(payload.size)) {
        return true;
    }

    std::vector<std::uint8_t> buffer(kDriverIoChunkBytes);
    DWORD offset = 0;
    while (offset < payload.size) {
        const DWORD expected = std::min<DWORD>(payload.size - offset, kDriverIoChunkBytes);
        DWORD read = 0;
        if (!::ReadFile(file.get(), buffer.data(), expected, &read, nullptr)) {
            error = L"ReadFile for existing driver failed: " + LastErrorMessage();
            return false;
        }
        if (read != expected) {
            return true;
        }
        if (std::memcmp(buffer.data(), payload.bytes + offset, read) != 0) {
            return true;
        }
        offset += read;
    }

    matches = true;
    return true;
}

// WritePayloadAtomically writes the embedded driver to disk through a temporary
// file. Inputs are the target path and payload; processing writes all bytes,
// flushes them, and replaces the final file with MoveFileEx; output is true only
// after the final path contains the embedded bytes.
bool WritePayloadAtomically(
    const std::wstring& targetPath,
    const EmbeddedDriverPayload& payload,
    std::wstring& error) {
    error.clear();

    const std::wstring tempPath = targetPath + L".embedded.tmp";
    ::DeleteFileW(tempPath.c_str());

    UniqueHandle file(::CreateFileW(
        tempPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.valid()) {
        error = L"CreateFileW for temporary embedded driver failed: " + LastErrorMessage();
        return false;
    }

    DWORD offset = 0;
    while (offset < payload.size) {
        const DWORD chunkBytes = std::min<DWORD>(payload.size - offset, kDriverIoChunkBytes);
        DWORD written = 0;
        if (!::WriteFile(file.get(), payload.bytes + offset, chunkBytes, &written, nullptr)) {
            const DWORD writeError = ::GetLastError();
            file.reset();
            ::DeleteFileW(tempPath.c_str());
            error = L"WriteFile for temporary embedded driver failed: " + LastErrorMessage(writeError);
            return false;
        }
        if (written == 0) {
            file.reset();
            ::DeleteFileW(tempPath.c_str());
            error = L"WriteFile for temporary embedded driver wrote zero bytes.";
            return false;
        }
        offset += written;
    }

    if (!::FlushFileBuffers(file.get())) {
        const DWORD flushError = ::GetLastError();
        file.reset();
        ::DeleteFileW(tempPath.c_str());
        error = L"FlushFileBuffers for temporary embedded driver failed: " + LastErrorMessage(flushError);
        return false;
    }
    file.reset();

    if (!::MoveFileExW(tempPath.c_str(), targetPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = ::GetLastError();
        ::DeleteFileW(tempPath.c_str());
        error = L"MoveFileExW failed while installing embedded driver payload: " + LastErrorMessage(moveError);
        return false;
    }

    return true;
}

// EnsureDriverFileFromEmbeddedResource makes KswordARK.sys available beside the
// executable. Input is the final service ImagePath; processing compares any
// existing file with the embedded resource and rewrites only when bytes differ;
// output is true when the final file can be used by SCM.
bool EnsureDriverFileFromEmbeddedResource(const std::wstring& driverPath, std::wstring& note, std::wstring& error) {
    note.clear();
    error.clear();
    if (driverPath.empty()) {
        error = L"driver output path is empty.";
        return false;
    }

    EmbeddedDriverPayload payload;
    std::wstring loadError;
    if (!LoadEmbeddedDriverPayload(payload, loadError)) {
        if (FileExists(driverPath)) {
            return true;
        }
        error = loadError;
        return false;
    }

    if (FileExists(driverPath)) {
        bool matches = false;
        std::wstring compareError;
        if (!ExistingDriverMatchesPayload(driverPath, payload, matches, compareError)) {
            error = compareError;
            return false;
        }
        if (matches) {
            return true;
        }
    }

    std::wstring writeError;
    if (!WritePayloadAtomically(driverPath, payload, writeError)) {
        error = writeError;
        return false;
    }

    note = L"Embedded KswordARK.sys was written beside the executable.";
    return true;
}

// UniqueServiceHandle owns an SCM handle. Inputs are SC_HANDLE values returned
// by service APIs; processing calls CloseServiceHandle on reset/destruction;
// get() returns the raw handle without transferring ownership.
class UniqueServiceHandle final : public NonCopyable {
public:
    UniqueServiceHandle() noexcept : handle_(nullptr) {}
    explicit UniqueServiceHandle(SC_HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueServiceHandle() { reset(); }

    UniqueServiceHandle(UniqueServiceHandle&& other) noexcept : handle_(other.release()) {}
    UniqueServiceHandle& operator=(UniqueServiceHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    void reset(SC_HANDLE handle = nullptr) noexcept {
        if (handle_) {
            ::CloseServiceHandle(handle_);
        }
        handle_ = handle;
    }

    SC_HANDLE release() noexcept {
        SC_HANDLE out = handle_;
        handle_ = nullptr;
        return out;
    }

    SC_HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr; }

private:
    SC_HANDLE handle_;
};

// OpenScm opens the service-control manager. Input is requested access;
// processing calls OpenSCManagerW; output is an owning handle or an invalid one.
UniqueServiceHandle OpenScm(DWORD access) {
    return UniqueServiceHandle(::OpenSCManagerW(nullptr, nullptr, access));
}

// OpenDriverService opens the KswordARK kernel service. Inputs are an SCM handle
// and desired access; processing calls OpenServiceW; output is an owning handle.
UniqueServiceHandle OpenDriverService(SC_HANDLE scm, DWORD access) {
    if (!scm) {
        return UniqueServiceHandle();
    }
    return UniqueServiceHandle(::OpenServiceW(scm, kDriverServiceName, access));
}

// FillServiceState copies QueryServiceStatusEx output into the UI state. Inputs
// are a service handle and mutable status; processing tolerates query failure;
// there is no return value because the caller already knows if the service opens.
void FillServiceState(SC_HANDLE service, DriverRuntimeStatus& status) {
    SERVICE_STATUS_PROCESS processStatus{};
    DWORD needed = 0;
    if (::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&processStatus), sizeof(processStatus), &needed)) {
        status.serviceState = processStatus.dwCurrentState;
        status.serviceRunning = processStatus.dwCurrentState == SERVICE_RUNNING;
    }
}

// ProbeControlDevice verifies the runtime control path used by every feature
// module. Inputs are the status object already filled from SCM; processing opens
// the shared ArkDriverClient handle instead of touching CreateFileW directly;
// no value is returned because fields are written into status.
void ProbeControlDevice(DriverRuntimeStatus& status) {
    const ksword::ark::DriverClient client;
    ksword::ark::DriverHandle handle = client.open(GENERIC_READ | GENERIC_WRITE);
    status.controlDeviceOpen = handle.isValid();
    status.controlDeviceError = status.controlDeviceOpen ? ERROR_SUCCESS : ::GetLastError();
}

// AppendControlDeviceMessage adds the control-device state to the user-facing
// status text. Input is a partially formatted message and the runtime status;
// output is the combined message shown in the status bar and message boxes.
std::wstring AppendControlDeviceMessage(std::wstring message, const DriverRuntimeStatus& status) {
    if (status.controlDeviceOpen) {
        if (!message.empty()) {
            message += L" ";
        }
        message += L"Control device is open.";
        return message;
    }

    if (!message.empty()) {
        message += L" ";
    }
    message += L"Control device unavailable: " + LastErrorMessage(status.controlDeviceError);
    return message;
}
} // namespace

std::wstring ResolveDriverPath() {
    return JoinPath(ModuleDirectory(), kDriverFileName);
}

DriverRuntimeStatus QueryDriverStatus() {
    DriverRuntimeStatus status;
    status.driverPath = ResolveDriverPath();
    status.driverFilePresent = FileExists(status.driverPath);

    UniqueServiceHandle scm = OpenScm(SC_MANAGER_CONNECT);
    if (!scm.valid()) {
        status.message = L"SCM unavailable: " + LastErrorMessage();
        ProbeControlDevice(status);
        status.message = AppendControlDeviceMessage(status.message, status);
        return status;
    }

    UniqueServiceHandle service = OpenDriverService(scm.get(), SERVICE_QUERY_STATUS);
    if (!service.valid()) {
        status.message = status.driverFilePresent
            ? L"Driver file found; service is not installed."
            : (EmbeddedDriverPayloadAvailable()
                ? L"KswordARK.sys is embedded in this executable and will be written beside the executable during installation."
                : L"KswordARK.sys not found beside the executable.");
        ProbeControlDevice(status);
        status.message = AppendControlDeviceMessage(status.message, status);
        return status;
    }

    status.serviceInstalled = true;
    FillServiceState(service.get(), status);
    ProbeControlDevice(status);
    status.message = status.serviceRunning ? L"R0 driver service is running." : L"R0 driver service is installed but stopped.";
    status.message = AppendControlDeviceMessage(status.message, status);
    return status;
}

DriverRuntimeStatus InstallAndStartDriver() {
    DriverRuntimeStatus status = QueryDriverStatus();
    std::wstring driverPreparationNote;
    std::wstring driverPreparationError;
    // The running-driver case deliberately avoids replacing an already loaded
    // image. Inputs are the status flags from QueryDriverStatus; processing only
    // materializes the embedded payload when start/install still needs a file;
    // there is no return value because errors are folded into status.message.
    if ((!status.serviceRunning || !status.driverFilePresent)
        && !EnsureDriverFileFromEmbeddedResource(status.driverPath, driverPreparationNote, driverPreparationError)) {
        status.driverFilePresent = FileExists(status.driverPath);
        status.message = L"Cannot prepare R0 driver file from embedded resource: " + driverPreparationError;
        return status;
    }
    status.driverFilePresent = FileExists(status.driverPath);
    if (!status.driverFilePresent) {
        status.message = L"Cannot install R0 driver because KswordARK.sys was not found: " + status.driverPath;
        return status;
    }

    UniqueServiceHandle scm = OpenScm(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm.valid()) {
        status.message = L"OpenSCManager failed: " + LastErrorMessage();
        return status;
    }

    UniqueServiceHandle service = OpenDriverService(scm.get(), SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | SERVICE_CHANGE_CONFIG);
    if (!service.valid()) {
        SC_HANDLE created = ::CreateServiceW(
            scm.get(),
            kDriverServiceName,
            kDriverDisplayName,
            SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | SERVICE_CHANGE_CONFIG,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            status.driverPath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (!created) {
            status.message = L"CreateService failed: " + LastErrorMessage();
            return status;
        }
        service.reset(created);
    } else {
        ::ChangeServiceConfigW(
            service.get(),
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            status.driverPath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            kDriverDisplayName);
    }

    status.serviceInstalled = true;
    FillServiceState(service.get(), status);
    if (!status.serviceRunning) {
        if (!::StartServiceW(service.get(), 0, nullptr)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING) {
                status.message = L"StartService failed: " + LastErrorMessage(err);
                FillServiceState(service.get(), status);
                return status;
            }
        }
    }

    FillServiceState(service.get(), status);
    ProbeControlDevice(status);
    status.message = status.serviceRunning ? L"R0 driver installed and running." : L"R0 driver installed; start state is pending or stopped.";
    if (!driverPreparationNote.empty()) {
        status.message = driverPreparationNote + L" " + status.message;
    }
    status.message = AppendControlDeviceMessage(status.message, status);
    return status;
}

DriverRuntimeStatus StopDriverService() {
    DriverRuntimeStatus status = QueryDriverStatus();
    UniqueServiceHandle scm = OpenScm(SC_MANAGER_CONNECT);
    if (!scm.valid()) {
        status.message = L"OpenSCManager failed: " + LastErrorMessage();
        return status;
    }
    UniqueServiceHandle service = OpenDriverService(scm.get(), SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!service.valid()) {
        status.message = L"R0 driver service is not installed.";
        return status;
    }
    SERVICE_STATUS serviceStatus{};
    if (!::ControlService(service.get(), SERVICE_CONTROL_STOP, &serviceStatus)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_SERVICE_NOT_ACTIVE) {
            status.message = L"ControlService stop failed: " + LastErrorMessage(err);
            return status;
        }
    }
    FillServiceState(service.get(), status);
    ProbeControlDevice(status);
    status.message = status.serviceRunning ? L"R0 driver stop requested." : L"R0 driver service is stopped.";
    status.message = AppendControlDeviceMessage(status.message, status);
    return status;
}

} // namespace Ksword::Core
