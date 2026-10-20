#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace ksword::evidence
{
    // The owning process/service must outlive every submitting requester. Tasks
    // own their non-GUI dependencies; destroying a requester never cancels an
    // accepted transaction. Destroy this executor from its owner's thread,
    // never from one of its tasks or its failure callback.
    class SerialTransactionQueue final
    {
    public:
        using Task = std::function<void()>;
        using FailureHandler = std::function<void(std::exception_ptr)>;

        // maxPending bounds waiting work, excluding the one running task.
        // The failure handler runs on the worker and must not touch GUI state.
        explicit SerialTransactionQueue(const std::size_t maxPending = 32,
            FailureHandler failureHandler = {})
            : maxPending_(maxPending), failureHandler_(std::move(failureHandler)),
              worker_([this] { run(); })
        {
        }

        ~SerialTransactionQueue()
        {
            stopAccepting();
            worker_.join(); // Every accepted task and its failure report drains.
        }

        SerialTransactionQueue(const SerialTransactionQueue&) = delete;
        SerialTransactionQueue& operator=(const SerialTransactionQueue&) = delete;

        // true means admitted, not completed or successful. No accepted task is
        // replaced or discarded. Empty tasks, a full queue, and stopped admission
        // return false without execution. An allocation failure may throw before
        // admission; it cannot leave a partially accepted task.
        [[nodiscard]] bool trySubmit(Task task)
        {
            if (!task)
            {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!accepting_ || pending_.size() >= maxPending_)
                {
                    return false;
                }
                pending_.push_back(std::move(task));
            }
            available_.notify_one();
            return true;
        }

        // May race with submitters while the executor is still alive. This does
        // not wait, cancel running work, or remove already admitted work.
        void stopAccepting()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = false;
            }
            available_.notify_one();
        }

        // Escaping exceptions are failures even if the reporting callback throws.
        // Domain tasks should convert their own exceptions into failure results;
        // this count and the optional callback cover a missed wrapper.
        [[nodiscard]] std::size_t unhandledTaskFailures() const noexcept
        {
            return unhandledTaskFailures_.load(std::memory_order_relaxed);
        }

    private:
        void reportFailure(std::exception_ptr failure) noexcept
        {
            unhandledTaskFailures_.fetch_add(1, std::memory_order_relaxed);
            if (failureHandler_)
            {
                try
                {
                    failureHandler_(failure);
                    return;
                }
                catch (...)
                {
                    // A faulty reporter must not terminate accepted work either.
                }
            }
        }

        void run()
        {
            for (;;)
            {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    available_.wait(lock, [this] { return !accepting_ || !pending_.empty(); });
                    if (pending_.empty())
                    {
                        return;
                    }
                    task = std::move(pending_.front());
                    pending_.pop_front();
                }
                try
                {
                    task();
                }
                catch (...)
                {
                    reportFailure(std::current_exception());
                }
            }
        }

        const std::size_t maxPending_;
        const FailureHandler failureHandler_;
        std::mutex mutex_;
        std::condition_variable available_;
        std::deque<Task> pending_;
        bool accepting_ = true;
        std::atomic<std::size_t> unhandledTaskFailures_{0};
        std::thread worker_;
    };
}
