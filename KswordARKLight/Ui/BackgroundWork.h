#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <type_traits>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Ksword::Ui {

// Value ownership only: workers never retain a window, view or UI callback.
// The visible shell polls pending() before allowing its final DestroyWindow.
class BackgroundWorkRegistry final {
    struct State;
public:
    class Lease final {
    public:
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    private:
        friend class BackgroundWorkRegistry;
        explicit Lease(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
        bool counted_ = false;
    };

    enum class Failure { None, ThreadCreation, Operation };
    struct Cleanup final {
        bool complete() const noexcept { return complete_.load(std::memory_order_acquire); }
        Failure failure() const noexcept { return failure_.load(std::memory_order_acquire); }
    private:
        friend class BackgroundWorkRegistry;
        std::atomic_bool complete_{false};
        std::atomic<Failure> failure_{Failure::None};
        bool running = false;
        std::function<bool()> operation;
    };

    // Injectable only in pure-logic tests. A rejected launch must not execute
    // its function inline; the retained job remains available for Retry.
    using Launch = std::function<void(std::function<void()>)>;
    explicit BackgroundWorkRegistry(Launch launch = {}) : state_(std::make_shared<State>()) {
        state_->launch = launch ? std::move(launch) : Launch([](std::function<void()> work) {
            std::thread(std::move(work)).detach();
        });
    }

    static BackgroundWorkRegistry& global() {
        static BackgroundWorkRegistry registry;
        return registry;
    }

    std::shared_ptr<Lease> tryAccept() {
        auto lease = std::shared_ptr<Lease>(new Lease(state_));
        std::lock_guard lock(state_->mutex);
        if (state_->closing) return {};
        ++state_->accepted;
        lease->counted_ = true;
        return lease;
    }

    void beginClosing() {
        std::lock_guard lock(state_->mutex);
        state_->closing = true;
    }
    // Reopening admission does not undo STOP or restart any retired session.
    void cancelClosing() {
        std::lock_guard lock(state_->mutex);
        state_->closing = false;
    }
    bool closing() const {
        std::lock_guard lock(state_->mutex);
        return state_->closing;
    }
    std::size_t pending() const {
        std::lock_guard lock(state_->mutex);
        return state_->accepted + static_cast<std::size_t>(std::count_if(
            state_->cleanup.begin(), state_->cleanup.end(), [](const auto& job) { return !job->complete(); }));
    }

    std::shared_ptr<Cleanup> retire(std::function<bool()> operation) {
        auto job = std::make_shared<Cleanup>();
        job->operation = std::move(operation);
        {
            std::lock_guard lock(state_->mutex);
            state_->cleanup.push_back(job);
        }
        kick(state_);
        return job;
    }

    // A UI timer may retry resource-exhaustion launches. Failed STOP operations
    // require an explicit Retry, so errors never become a busy retry loop.
    void retry(bool includeOperationFailures = false) {
        {
            std::lock_guard lock(state_->mutex);
            for (const auto& job : state_->cleanup) {
                const auto failure = job->failure();
                if (failure == Failure::ThreadCreation ||
                    (includeOperationFailures && failure == Failure::Operation)) {
                    job->failure_.store(Failure::None, std::memory_order_release);
                }
            }
        }
        kick(state_);
    }

    // Only the shell's final driver release may be withdrawn, and only when
    // no worker could ever start it. Running/failed accepted work is immutable.
    bool withdrawUnstarted(const std::shared_ptr<Cleanup>& job) {
        if (!job) return false;
        std::function<bool()> released;
        {
            std::lock_guard lock(state_->mutex);
            if (job->running || job->complete() || job->failure() != Failure::ThreadCreation) return false;
            const auto found = std::find(state_->cleanup.begin(), state_->cleanup.end(), job);
            if (found == state_->cleanup.end()) return false;
            job->running = true; // reserve it against a concurrent Retry
            released = std::move(job->operation);
        }
        released = {};
        {
            std::lock_guard lock(state_->mutex);
            job->running = false;
            job->complete_.store(true, std::memory_order_release);
            state_->cleanup.erase(std::remove(state_->cleanup.begin(), state_->cleanup.end(), job), state_->cleanup.end());
        }
        return true;
    }

private:
    struct State final {
        std::mutex mutex;
        bool closing = false;
        bool workerRunning = false;
        std::size_t accepted = 0;
        Launch launch;
        std::vector<std::shared_ptr<Cleanup>> cleanup;
    };

    static void kick(const std::shared_ptr<State>& state) {
        std::shared_ptr<Cleanup> job;
        {
            std::lock_guard lock(state->mutex);
            if (state->workerRunning) return;
            state->cleanup.erase(std::remove_if(state->cleanup.begin(), state->cleanup.end(),
                [](const auto& item) { return item->complete(); }), state->cleanup.end());
            const auto found = std::find_if(state->cleanup.begin(), state->cleanup.end(),
                [](const auto& item) { return !item->running && item->failure() == Failure::None; });
            if (found == state->cleanup.end()) return;
            job = *found;
            state->workerRunning = true;
            job->running = true;
        }
        try {
            state->launch([state, job] {
                bool success = false;
                try { success = job->operation && job->operation(); }
                catch (...) { success = false; }
                // Release controller/handle captures on this worker before
                // publishing completion. Never destroy them under the lock.
                if (success) job->operation = {};
                {
                    std::lock_guard lock(state->mutex);
                    job->running = false;
                    job->failure_.store(success ? Failure::None : Failure::Operation, std::memory_order_release);
                    job->complete_.store(success, std::memory_order_release);
                    state->workerRunning = false;
                }
                kick(state);
            });
        } catch (...) {
            std::lock_guard lock(state->mutex);
            job->running = false;
            job->failure_.store(Failure::ThreadCreation, std::memory_order_release);
            state->workerRunning = false;
        }
    }

    const std::shared_ptr<State> state_;
};

inline BackgroundWorkRegistry::Lease::~Lease() {
    if (!counted_) return;
    std::lock_guard lock(state_->mutex);
    --state_->accepted;
}

using BackgroundWorkLease = BackgroundWorkRegistry::Lease;
using BackgroundCleanup = BackgroundWorkRegistry::Cleanup;

// Captured before submission, so a rejected worker launch also returns the
// pending count. Creating this guard inside a worker would strand that count.
template <class Lifetime>
class PendingBackgroundOperation final {
public:
    explicit PendingBackgroundOperation(std::shared_ptr<Lifetime> lifetime) : lifetime_(std::move(lifetime)) {
        lifetime_->pending.fetch_add(1, std::memory_order_acq_rel);
    }
    ~PendingBackgroundOperation() {
        lifetime_->pending.fetch_sub(1, std::memory_order_acq_rel);
        lifetime_->idle.notify_all();
    }
    PendingBackgroundOperation(const PendingBackgroundOperation&) = delete;
    PendingBackgroundOperation& operator=(const PendingBackgroundOperation&) = delete;
private:
    const std::shared_ptr<Lifetime> lifetime_;
};
template <class Lifetime>
std::shared_ptr<PendingBackgroundOperation<Lifetime>> HoldPendingOperation(const std::shared_ptr<Lifetime>& lifetime) {
    return std::make_shared<PendingBackgroundOperation<Lifetime>>(lifetime);
}

inline std::shared_ptr<BackgroundWorkLease> TryAcceptBackgroundWork() {
    return BackgroundWorkRegistry::global().tryAccept();
}
inline std::size_t PendingBackgroundWork() { return BackgroundWorkRegistry::global().pending(); }
inline std::shared_ptr<BackgroundCleanup> RunBackgroundRetirement(std::function<bool()> operation) {
    return BackgroundWorkRegistry::global().retire(std::move(operation));
}

// Reserve cleanup before transferring a value. A failed worker launch remains
// in the existing retry registry; no UI wait or synchronous fallback occurs.
// The short handoff barrier prevents a fast worker from releasing its copy
// before the submitting thread has dropped the original last owner.
template<class Value>
bool RetireBackgroundValue(Value& value,BackgroundWorkRegistry& registry=BackgroundWorkRegistry::global()) {
    static_assert(std::is_nothrow_move_assignable_v<Value>);
    struct Ticket {std::mutex mutex;std::condition_variable ready;bool submitted=false;Value value{};};
    std::shared_ptr<Ticket> ticket;
    try {
        ticket=std::make_shared<Ticket>();
        registry.retire([ticket] {
            {std::unique_lock lock(ticket->mutex);ticket->ready.wait(lock,[&]{return ticket->submitted;});}
            ticket->value=Value{};return true;
        });
    } catch(...) {return false;}
    ticket->value=std::move(value);
    {std::lock_guard lock(ticket->mutex);ticket->submitted=true;}
    ticket->ready.notify_one();return true;
}

} // namespace Ksword::Ui
