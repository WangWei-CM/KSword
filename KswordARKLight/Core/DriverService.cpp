#include "DriverService.h"

#include "Common.h"
#include "PathUtils.h"
#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <winsvc.h>

namespace Ksword::Core {
namespace {
constexpr wchar_t kDriverFileName[] = L"KswordARK.sys";
constexpr wchar_t kDriverServiceName[] = L"KswordARK";
constexpr wchar_t kDriverDisplayName[] = L"KswordARK R0 Driver";

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
            : L"KswordARK.sys not found beside the executable.";
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
