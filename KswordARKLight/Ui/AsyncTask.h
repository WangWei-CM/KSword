#pragma once

#include "../Core/Win32Lean.h"

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

    // consume takes ownership of lParam when it belongs to this task. Call it
    // only for the completion message supplied to the constructor.
    bool consume(HWND owner, WPARAM, LPARAM lParam) {
        std::unique_ptr<Completion> completion(reinterpret_cast<Completion*>(lParam));
        if (!completion) {
            return false;
        }

        const std::shared_ptr<State> state = completion->state.lock();
        if (!state || !state->alive.load(std::memory_order_acquire)) {
            return true;
        }

        Deliver deliver;
        Work nextWork;
        std::uint64_t nextGeneration = 0;
        bool deliverCurrent = false;
        bool startNext = false;
        {
            std::scoped_lock lock(state->mutex);
            if (!state->alive.load(std::memory_order_acquire) || state->owner != owner) {
                return true;
            }

            deliverCurrent = completion->generation == state->latestGeneration;
            deliver = state->deliver;
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
    struct State final {
        State(HWND target, UINT message)
            : owner(target), completionMessage(message), alive(target != nullptr) {
        }

        std::mutex mutex;
        HWND owner = nullptr;
        UINT completionMessage = 0;
        std::atomic_bool alive = false;
        bool running = false;
        bool pending = false;
        std::uint64_t latestGeneration = 0;
        std::uint64_t runningGeneration = 0;
        Work work;
        Deliver deliver;
    };

    struct Completion final {
        std::weak_ptr<State> state;
        std::uint64_t generation = 0;
        std::optional<Result> result;
        std::exception_ptr error;
    };

    static void startWorker(const std::shared_ptr<State>& state, Work work, const std::uint64_t generation) {
        std::thread([state, work = std::move(work), generation]() mutable {
            auto completion = std::make_unique<Completion>();
            completion->state = state;
            completion->generation = generation;
            try {
                completion->result.emplace(work());
            } catch (...) {
                completion->error = std::current_exception();
            }

            HWND owner = nullptr;
            UINT message = 0;
            bool shouldPost = false;
            {
                std::scoped_lock lock(state->mutex);
                shouldPost = state->alive.load(std::memory_order_acquire);
                owner = state->owner;
                message = state->completionMessage;
            }

            if (shouldPost && owner && ::PostMessageW(owner, message, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(completion.get()))) {
                completion.release();
                return;
            }

            finishWithoutDelivery(state, generation);
        }).detach();
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
