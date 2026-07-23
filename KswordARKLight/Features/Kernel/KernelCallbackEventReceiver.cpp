#include "KernelCallbackEventReceiver.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace Ksword::Features::Kernel {
namespace {

constexpr DWORD kReceiverPollMilliseconds = 100;
constexpr DWORD kReceiverRetryMilliseconds = 250;

bool IsReceiverRunning(const std::shared_ptr<CallbackEventReceiver::State>& state, const std::uint64_t generation);
void RunReceiver(const std::shared_ptr<CallbackEventReceiver::State>& state, std::uint64_t generation);

} // namespace

struct CallbackEventReceiver::State final {
    State(const HWND initialOwner, const UINT completionMessage)
        : owner(initialOwner), eventMessage(completionMessage) {
    }

    mutable std::mutex mutex;
    HWND owner = nullptr;
    UINT eventMessage = 0;
    std::atomic_uint64_t generation = 0;
    std::atomic_bool active = false;
    std::atomic_bool workerRunning = false;
    ksword::ark::DriverHandle handle;
};

namespace {

bool IsReceiverRunning(const std::shared_ptr<CallbackEventReceiver::State>& state, const std::uint64_t generation) {
    return state && state->active.load(std::memory_order_acquire) && state->generation.load(std::memory_order_acquire) == generation;
}

void ResetHandle(const std::shared_ptr<CallbackEventReceiver::State>& state) {
    std::scoped_lock lock(state->mutex);
    state->handle.reset();
}

void PostEvent(
    const std::shared_ptr<CallbackEventReceiver::State>& state,
    const std::uint64_t generation,
    const KSWORD_ARK_CALLBACK_EVENT_PACKET& event) {
    HWND owner = nullptr;
    UINT message = 0;
    {
        std::scoped_lock lock(state->mutex);
        if (!IsReceiverRunning(state, generation) || state->owner == nullptr) {
            return;
        }
        owner = state->owner;
        message = state->eventMessage;
    }

    auto snapshot = std::make_unique<CallbackEventSnapshot>();
    snapshot->generation = generation;
    snapshot->event = event;
    if (::PostMessageW(owner, message, 0, reinterpret_cast<LPARAM>(snapshot.get()))) {
        snapshot.release();
    }
}

void RunReceiver(const std::shared_ptr<CallbackEventReceiver::State>& state, const std::uint64_t generation) {
    const ksword::ark::DriverClient client;
    while (IsReceiverRunning(state, generation)) {
        HANDLE nativeHandle = INVALID_HANDLE_VALUE;
        {
            std::scoped_lock lock(state->mutex);
            nativeHandle = state->handle.native();
        }
        if (nativeHandle == nullptr || nativeHandle == INVALID_HANDLE_VALUE) {
            ksword::ark::DriverHandle opened = client.openOverlapped();
            if (!opened.isValid()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReceiverRetryMilliseconds));
                continue;
            }
            {
                std::scoped_lock lock(state->mutex);
                if (!IsReceiverRunning(state, generation)) {
                    break;
                }
                state->handle = std::move(opened);
                nativeHandle = state->handle.native();
            }
        }

        KSWORD_ARK_CALLBACK_WAIT_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
        request.waiterTag = static_cast<unsigned long>(generation & 0xFFFFFFFFULL);
        KSWORD_ARK_CALLBACK_EVENT_PACKET event{};
        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReceiverRetryMilliseconds));
            continue;
        }

        DWORD bytesReturned = 0;
        const ksword::ark::AsyncIoResult issued = client.waitCallbackEventAsync(state->handle, request, event, &overlapped);
        bytesReturned = issued.bytesReturned;
        if (!issued.issued) {
            if (issued.win32Error == ERROR_IO_PENDING) {
                while (IsReceiverRunning(state, generation)) {
                    const DWORD wait = ::WaitForSingleObject(overlapped.hEvent, kReceiverPollMilliseconds);
                    if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
                        break;
                    }
                }
                if (!IsReceiverRunning(state, generation)) {
                    (void)::CancelIoEx(nativeHandle, &overlapped);
                    (void)::WaitForSingleObject(overlapped.hEvent, kReceiverRetryMilliseconds);
                }
                if (::GetOverlappedResult(nativeHandle, &overlapped, &bytesReturned, FALSE) == FALSE) {
                    const DWORD error = ::GetLastError();
                    ::CloseHandle(overlapped.hEvent);
                    if (error == ERROR_OPERATION_ABORTED || error == ERROR_INVALID_HANDLE ||
                        error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_FILE_NOT_FOUND) {
                        ResetHandle(state);
                    }
                    if (IsReceiverRunning(state, generation)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(kReceiverRetryMilliseconds));
                    }
                    continue;
                }
            } else {
                ::CloseHandle(overlapped.hEvent);
                if (issued.win32Error == ERROR_INVALID_HANDLE || issued.win32Error == ERROR_DEVICE_NOT_CONNECTED ||
                    issued.win32Error == ERROR_FILE_NOT_FOUND) {
                    ResetHandle(state);
                }
                if (IsReceiverRunning(state, generation)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kReceiverRetryMilliseconds));
                }
                continue;
            }
        }
        ::CloseHandle(overlapped.hEvent);

        if (!IsReceiverRunning(state, generation)) {
            break;
        }
        if (bytesReturned >= sizeof(event) && event.size >= sizeof(event) &&
            event.version == KSWORD_ARK_CALLBACK_PROTOCOL_VERSION) {
            PostEvent(state, generation, event);
        }
    }
    ResetHandle(state);
    state->workerRunning.store(false, std::memory_order_release);
}

} // namespace

CallbackEventReceiver::CallbackEventReceiver(const HWND owner, const UINT eventMessage)
    : state_(std::make_shared<State>(owner, eventMessage)) {
}

CallbackEventReceiver::~CallbackEventReceiver() {
    Shutdown();
}

bool CallbackEventReceiver::Start() {
    const std::shared_ptr<State> state = state_;
    if (!state) {
        return false;
    }
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(state->mutex);
        if (state->owner == nullptr || state->active.load(std::memory_order_acquire) ||
            state->workerRunning.load(std::memory_order_acquire)) {
            return false;
        }
        generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1U;
        state->active.store(true, std::memory_order_release);
        state->workerRunning.store(true, std::memory_order_release);
    }
    std::thread([state, generation] { RunReceiver(state, generation); }).detach();
    return true;
}

void CallbackEventReceiver::Stop() noexcept {
    const std::shared_ptr<State> state = state_;
    if (!state) {
        return;
    }
    HANDLE nativeHandle = INVALID_HANDLE_VALUE;
    {
        std::scoped_lock lock(state->mutex);
        state->active.store(false, std::memory_order_release);
        state->generation.fetch_add(1, std::memory_order_acq_rel);
        nativeHandle = state->handle.native();
    }
    if (nativeHandle != nullptr && nativeHandle != INVALID_HANDLE_VALUE) {
        (void)::CancelIoEx(nativeHandle, nullptr);
    }
}

void CallbackEventReceiver::Shutdown() noexcept {
    Stop();
    const std::shared_ptr<State> state = state_;
    if (state) {
        std::scoped_lock lock(state->mutex);
        state->owner = nullptr;
    }
}

bool CallbackEventReceiver::running() const noexcept {
    const std::shared_ptr<State> state = state_;
    return state && state->active.load(std::memory_order_acquire);
}

bool CallbackEventReceiver::stopping() const noexcept {
    const std::shared_ptr<State> state = state_;
    return state && !state->active.load(std::memory_order_acquire) &&
        state->workerRunning.load(std::memory_order_acquire);
}

bool CallbackEventReceiver::accepts(const std::uint64_t generation) const noexcept {
    const std::shared_ptr<State> state = state_;
    return state && state->active.load(std::memory_order_acquire) && state->generation.load(std::memory_order_acquire) == generation;
}

} // namespace Ksword::Features::Kernel
