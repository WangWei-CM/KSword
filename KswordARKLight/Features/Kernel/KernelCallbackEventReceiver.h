#pragma once

#include "../../Core/Win32Lean.h"
#include "../../../shared/driver/KswordArkCallbackIoctl.h"

#include <cstdint>
#include <memory>

namespace Ksword::Features::Kernel {

// CallbackEventSnapshot is an immutable callback decision packet delivered from
// the overlapped receiver to KernelPage. The UI owns and deletes it after the
// completion message, so no driver-owned buffers cross the thread boundary.
struct CallbackEventSnapshot {
    std::uint64_t generation = 0;
    KSWORD_ARK_CALLBACK_EVENT_PACKET event{};
};

// CallbackEventReceiver owns one background overlapped wait loop. Start and
// Stop never wait for driver I/O; Stop cancels the pending request and makes
// queued messages stale through a generation token.
class CallbackEventReceiver final {
public:
    struct State;

    CallbackEventReceiver(HWND owner, UINT eventMessage);
    ~CallbackEventReceiver();

    CallbackEventReceiver(const CallbackEventReceiver&) = delete;
    CallbackEventReceiver& operator=(const CallbackEventReceiver&) = delete;

    bool Start();
    void Stop() noexcept;
    void Shutdown() noexcept;
    bool running() const noexcept;
    bool stopping() const noexcept;
    bool accepts(std::uint64_t generation) const noexcept;

private:
    std::shared_ptr<State> state_;
};

} // namespace Ksword::Features::Kernel
