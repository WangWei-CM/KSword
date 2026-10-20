#pragma once

#include "../Core/Win32Lean.h"
#include "BackgroundWork.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace Ksword::Ui {

// Same-thread shell-to-owned-page lifecycle protocol; no pointer payloads.
inline constexpr UINT kBeginNativeRetirementMessage = WM_APP + 0x3D0;
inline constexpr UINT kPollNativeRetirementMessage = WM_APP + 0x3D1;
inline constexpr UINT kCancelNativeRetirementMessage = WM_APP + 0x3D2;

namespace detail {
// Shared by every Result specialization: recycled HWNDs can receive another
// task's completion message, including one with a different Result type.
inline std::atomic_uintptr_t asyncSnapshotTaskIdentity{0};
} // namespace detail

// AsyncSnapshotTask runs one value-producing operation away from the UI thread.
// Repeated requests are coalesced: only the newest request is delivered after an
// in-flight operation completes. The owner must call cancel() from WM_NCDESTROY
// and consume() for its configured completion message.
template <typename Result>
class AsyncSnapshotTask final {
public:
    using Work = std::function<Result()>;
    using Deliver = std::function<void(std::uint64_t, std::optional<Result>&&, std::exception_ptr)>;

    explicit AsyncSnapshotTask(HWND owner, UINT completionMessage)
        : state_(std::make_shared<State>(owner, completionMessage)) {
    }

    ~AsyncSnapshotTask() {
        cancel();
    }

    AsyncSnapshotTask(const AsyncSnapshotTask&) = delete;
    AsyncSnapshotTask& operator=(const AsyncSnapshotTask&) = delete;

    void resetOwner(HWND owner) {
        const std::shared_ptr<State> state = state_;
        if (!state) {
            return;
        }
        std::scoped_lock lock(state->mutex);
        state->owner = owner;
        state->alive.store(owner != nullptr, std::memory_order_release);
    }

    void request(Work work, Deliver deliver) {
        const std::shared_ptr<State> state = state_;
        if (!state || !work || !deliver || !state->alive.load(std::memory_order_acquire)) {
            return;
        }

        // Include existing refresh/action callers in the same close admission
        // and drain accounting. Only replaceable requests may be coalesced.
        const auto lease = TryAcceptBackgroundWork();
        if (!lease) return;
        work = [lease, operation = std::move(work)]() mutable { return operation(); };
        Work startWork;
        std::uint64_t generation = 0;
        bool startNow = false;
        {
            std::scoped_lock lock(state->mutex);
            if (!state->alive.load(std::memory_order_acquire)) {
                return;
            }

            state->work = std::move(work);
            state->deliver = std::move(deliver);
            generation = ++state->latestGeneration;
            if (state->running) {
                state->pending = true;
                return;
            }

            state->running = true;
            state->runningGeneration = generation;
            startWork = state->work;
            startNow = true;
        }

        if (startNow) {
            startWorker(state, std::move(startWork), generation);
        }
    }

    // Messages carry task identity and generation, never a Result pointer.
    // Call only for the completion message supplied to the constructor.
    bool consume(HWND owner, WPARAM taskIdentity, LPARAM generation) {
        const std::shared_ptr<State> state = state_;
        if (!state) {
            return true;
        }

        std::unique_ptr<Completion> completion;
        Deliver deliver;
        Work nextWork;
        std::uint64_t nextGeneration = 0;
        bool deliverCurrent = false;
        bool startNext = false;
        {
            std::scoped_lock lock(state->mutex);
            if (!state->alive.load(std::memory_order_acquire) || state->owner != owner
                || state->identity != static_cast<std::uintptr_t>(taskIdentity)
                || !state->completion
                || state->completion->generation != static_cast<std::uint64_t>(generation)) {
                return true;
            }
            completion = std::move(state->completion);

            deliverCurrent = completion->generation == state->latestGeneration;
            deliver = state->pending ? state->deliver : std::move(state->deliver);
            if (state->running && state->runningGeneration == completion->generation) {
                state->running = false;
                if (state->pending && state->alive.load(std::memory_order_acquire)) {
                    state->pending = false;
                    state->running = true;
                    state->runningGeneration = state->latestGeneration;
                    nextGeneration = state->runningGeneration;
                    nextWork = state->work;
                    startNext = true;
                }
            }
        }

        if (deliverCurrent && deliver) {
            deliver(completion->generation, std::move(completion->result), completion->error);
        }
        if (startNext) {
            startWorker(state, std::move(nextWork), nextGeneration);
        }
        return true;
    }

    void cancel() noexcept {
        const std::shared_ptr<State> state = state_;
        if (!state) {
            return;
        }
        std::scoped_lock lock(state->mutex);
        state->alive.store(false, std::memory_order_release);
        state->owner = nullptr;
        state->pending = false;
        state->work = {};
        state->deliver = {};
        // A queued message may be discarded when its window is destroyed.
        // The task, rather than the message queue, owns the completed result.
        state->completion.reset();
    }

    bool running() const noexcept {
        const std::shared_ptr<State> state = state_;
        if (!state) {
            return false;
        }
        std::scoped_lock lock(state->mutex);
        return state->running || state->pending;
    }

private:
    struct Completion final {
        std::uint64_t generation = 0;
        std::optional<Result> result;
        std::exception_ptr error;
    };

    struct State final {
        State(HWND target, UINT message)
            : owner(target), completionMessage(message), alive(target != nullptr) {
        }

        std::mutex mutex;
        const std::uintptr_t identity = detail::asyncSnapshotTaskIdentity.fetch_add(1, std::memory_order_relaxed) + 1U;
        HWND owner = nullptr;
        UINT completionMessage = 0;
        std::atomic_bool alive = false;
        bool running = false;
        bool pending = false;
        std::uint64_t latestGeneration = 0;
        std::uint64_t runningGeneration = 0;
        Work work;
        Deliver deliver;
        std::unique_ptr<Completion> completion;
    };

    static void startWorker(const std::shared_ptr<State>& state, Work work, const std::uint64_t generation) {
        auto operation = [state, work = std::move(work), generation]() mutable {
            auto completion = std::make_unique<Completion>();
            completion->generation = generation;
            try {
                completion->result.emplace(work());
            } catch (...) {
                completion->error = std::current_exception();
            }

            // Work may own an accepted-transaction lease or controller. Do not
            // retain it until another request or window destruction.
            work = {};
            publish(state, generation, std::move(completion));
        };
        try {
            std::thread(std::move(operation)).detach();
        } catch (...) {
            auto completion = std::make_unique<Completion>();
            completion->generation = generation;
            completion->error = std::current_exception();
            publish(state, generation, std::move(completion));
        }
    }

    static void publish(const std::shared_ptr<State>& state, const std::uint64_t generation,
        std::unique_ptr<Completion> completion) {
        Work releasedWork;
        {
            std::scoped_lock lock(state->mutex);
            if (generation == state->latestGeneration && !state->pending) {
                releasedWork = std::move(state->work);
            }
        }
        releasedWork = {};
        {
            std::scoped_lock lock(state->mutex);
            if (state->alive.load(std::memory_order_acquire) && state->owner) {
                state->completion = std::move(completion);
                // Serialize publishing with cancel so a retiring worker cannot
                // post to a recycled owner after cancellation has returned.
                if (::PostMessageW(state->owner, state->completionMessage,
                    static_cast<WPARAM>(state->identity), static_cast<LPARAM>(generation))) return;
                state->completion.reset();
            }
        }
        finishWithoutDelivery(state, generation);
    }

    static void finishWithoutDelivery(const std::shared_ptr<State>& state, const std::uint64_t generation) {
        Work nextWork;
        std::uint64_t nextGeneration = 0;
        bool startNext = false;
        {
            std::scoped_lock lock(state->mutex);
            if (state->running && state->runningGeneration == generation) {
                state->running = false;
                if (state->pending && state->alive.load(std::memory_order_acquire)) {
                    state->pending = false;
                    state->running = true;
                    state->runningGeneration = state->latestGeneration;
                    nextGeneration = state->runningGeneration;
                    nextWork = state->work;
                    startNext = true;
                }
            }
        }
        if (startNext) {
            startWorker(state, std::move(nextWork), nextGeneration);
        }
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace Ksword::Ui
