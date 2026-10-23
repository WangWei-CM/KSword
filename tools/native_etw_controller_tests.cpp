#include "native_etw_controller_fake_api.h"
#include "../KswordARKLight/Ui/BackgroundWork.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

namespace {
struct Backend {
    std::mutex mutex;
    std::condition_variable changed;
    std::wstring identity;
    bool session = false, consumer = false, entered = false, endConsumer = false;
    unsigned failStop = 0, failClose = 0, starts = 0;
    ULONG enableError = ERROR_SUCCESS, processError = ERROR_CANCELLED;
} backend;
template<class Predicate> void Eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("fake controller timeout");
        std::this_thread::yield();
    }
}
void Reset() {
    std::lock_guard lock(backend.mutex);
    assert(!backend.session && !backend.consumer);
    backend.entered = backend.endConsumer = false;
    backend.failStop = backend.failClose = 0;
    backend.enableError = ERROR_SUCCESS;
    backend.processError = ERROR_CANCELLED;
}
ks::r3::monitor::EtwFilterState Filter() {
    ks::r3::monitor::EtwFilterState filter;
    filter.providers.push_back({L"fake provider", {}});
    return filter;
}
void WaitConsumer() { Eventually([] { std::lock_guard lock(backend.mutex); return backend.entered; }); }
} // namespace

ULONG WINAPI NativeControllerStartTraceW(PTRACEHANDLE handle, LPCWSTR name, PEVENT_TRACE_PROPERTIES) {
    std::lock_guard lock(backend.mutex);
    assert(!backend.session);
    backend.session = true;
    backend.entered = backend.endConsumer = false;
    backend.identity = name;
    ++backend.starts;
    *handle = 17;
    return ERROR_SUCCESS;
}
ULONG WINAPI NativeControllerControlTraceW(TRACEHANDLE handle, LPCWSTR name, PEVENT_TRACE_PROPERTIES, ULONG action) {
    std::lock_guard lock(backend.mutex);
    assert(handle == 17 && name == backend.identity && action == EVENT_TRACE_CONTROL_STOP);
    if (backend.failStop) { --backend.failStop; return ERROR_ACCESS_DENIED; }
    const bool existed = backend.session;
    backend.session = false;
    backend.changed.notify_all();
    return existed ? ERROR_SUCCESS : ERROR_WMI_INSTANCE_NOT_FOUND;
}
ULONG WINAPI NativeControllerEnableTraceEx2(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, PENABLE_TRACE_PARAMETERS) {
    std::lock_guard lock(backend.mutex);
    return backend.enableError;
}
TRACEHANDLE WINAPI NativeControllerOpenTraceW(PEVENT_TRACE_LOGFILEW) {
    std::lock_guard lock(backend.mutex);
    backend.consumer = true;
    return 23;
}
ULONG WINAPI NativeControllerCloseTrace(TRACEHANDLE handle) {
    std::lock_guard lock(backend.mutex);
    assert(handle == 23);
    if (backend.failClose) { --backend.failClose; return ERROR_GEN_FAILURE; }
    backend.consumer = false;
    backend.changed.notify_all();
    return ERROR_SUCCESS;
}
ULONG WINAPI NativeControllerProcessTrace(TRACEHANDLE* handle, ULONG count, LPFILETIME, LPFILETIME) {
    std::unique_lock lock(backend.mutex);
    assert(count == 1 && *handle == 23);
    backend.entered = true;
    backend.changed.notify_all();
    backend.changed.wait(lock, [] { return !backend.session || !backend.consumer || backend.endConsumer; });
    return backend.processError;
}

// Ownership tests isolate the unchanged event formatting/filter layer.
namespace ks::r3::monitor {
bool EventMatchesFilter(std::uint32_t, std::uint8_t, const EtwFilterState&) { return true; }
std::wstring GuidToString(const GUID&) { return L"{fake-guid}"; }
std::wstring FileTimeToLocalText(const LARGE_INTEGER&) { return L"fake-time"; }
}

int main() {
    using ks::r3::monitor::EtwSessionController;
    {
        EtwSessionController controller;
        assert(controller.start(Filter()));
        WaitConsumer();
        unsigned before = 0;
        { std::lock_guard lock(backend.mutex); backend.failStop = 8; before = backend.starts; }
        assert(!controller.stopChecked());
        assert(!controller.evidence().sessionStopped && controller.cleanupPending());
        assert(!controller.start(Filter()));
        { std::lock_guard lock(backend.mutex); assert(backend.starts == before && backend.session); backend.failStop = 0; }
        assert(controller.stopChecked());
        assert(controller.evidence().sessionStopped && !controller.cleanupPending());
    }
    Reset();
    {
        // STOP succeeds but both final consumer-close attempts fail. The old
        // sessionStopped-only completion predicate must not report readiness.
        EtwSessionController controller;
        assert(controller.start(Filter()));
        WaitConsumer();
        { std::lock_guard lock(backend.mutex); backend.failClose = 2; }
        assert(!controller.stopChecked());
        assert(controller.evidence().sessionStopped && controller.evidence().closeStatus == ERROR_GEN_FAILURE && controller.cleanupPending());
        assert(controller.stopChecked());
    }
    Reset();
    {
        auto controller = std::make_shared<EtwSessionController>();
        const std::weak_ptr<EtwSessionController> weak = controller;
        assert(controller->start(Filter()));
        WaitConsumer();
        { std::lock_guard lock(backend.mutex); backend.failStop = 1; backend.failClose = 1; }
        Ksword::Ui::BackgroundWorkRegistry registry;
        registry.beginClosing();
        auto cleanup = registry.retire([controller] { return controller->stopChecked(); });
        controller.reset();
        Eventually([&] { return cleanup->failure() == Ksword::Ui::BackgroundWorkRegistry::Failure::Operation; });
        registry.cancelClosing();
        assert(!weak.expired() && registry.pending() == 1);
        { std::lock_guard lock(backend.mutex); assert(backend.session && backend.consumer); }
        registry.retry(true);
        Eventually([&] { return cleanup->complete(); });
        assert(weak.expired() && registry.pending() == 0);
    }
    Reset();
    {
        EtwSessionController controller;
        assert(controller.start(Filter()));
        WaitConsumer();
        { std::lock_guard lock(backend.mutex); backend.failStop = 8; backend.endConsumer = true; backend.processError = ERROR_INVALID_DATA; backend.changed.notify_all(); }
        Eventually([&] { return !controller.running(); });
        assert(!controller.stopChecked());
        { std::lock_guard lock(backend.mutex); backend.failStop = 0; }
        assert(controller.stopChecked());
    }
    Reset();
    {
        EtwSessionController controller;
        { std::lock_guard lock(backend.mutex); backend.enableError = ERROR_NOT_SUPPORTED; backend.failStop = 2; }
        assert(!controller.start(Filter()));
        assert(controller.evidence().startSucceeded && !controller.evidence().sessionStopped);
        assert(controller.stopChecked());
    }
    Reset();
    std::cout << "PASS: actual controller STOP+Close failure retention, successful STOP with failed Close, natural exit, canceled close retry, provider compensation\n";
}
