#pragma once

#include "AsyncCleanupRegistry.h"
#include <QRunnable>
#include <QThreadPool>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ks::ui
{
    namespace retained_cleanup_detail
    {
        struct State;
        struct Store
        {
            std::mutex mutex;
            std::unordered_map<State*, std::shared_ptr<State>> jobs;
        };
        inline Store& store() { static Store value; return value; }

        inline void forget(State* state) noexcept
        {
            try
            {
                auto& retained = store();
                std::lock_guard<std::mutex> lock(retained.mutex);
                retained.jobs.erase(state);
            }
            catch (...) {}
        }

        struct State final : std::enable_shared_from_this<State>
        {
            std::function<bool()> attempt; // 强持纯值owner，不得持QObject或反持job。
            std::function<void()> requestStop; // 仅短原子/唤醒，不得I/O、join或持GUI。
            std::shared_ptr<AsyncCleanupRegistry::Lease> lease;
            int workerPriority = 0;
            std::atomic_bool ready{ false };
            std::atomic_bool requested{ false };
            std::atomic_bool stopSignaled{ false };
            std::atomic_bool initialScheduled{ false };
            std::atomic_bool running{ false };
            std::atomic_bool failed{ false };
            std::atomic_bool complete{ false };
            std::atomic<std::uint64_t> attempts{ 0 };

            bool requestCleanup(bool initialOnly = false) noexcept
            {
                const bool initial = !requested.exchange(true);
                if (initialOnly && !initial) return false;
                if (initial)
                {
                    try { if (requestStop) requestStop(); } catch (...) {}
                    stopSignaled.store(true);
                }
                return scheduleAttempt(!initial && !initialOnly);
            }

            // 创建方与首次request共同消费一次初始调度机会；快失败不能重开该机会。
            bool scheduleAttempt(bool explicitRetry) noexcept
            {
                if (!stopSignaled.load() || !ready.load() || complete.load()) return false;
                if (initialScheduled.exchange(true) && (!explicitRetry || !failed.load())) return false;
                bool idle = false;
                if (!running.compare_exchange_strong(idle, true)) return false;
                if (complete.load())
                {
                    running.store(false);
                    return false;
                }
                failed.store(false);
                try
                {
                    const auto self = shared_from_this();
                    auto task = std::unique_ptr<QRunnable>(QRunnable::create([self]() { self->run(); }));
                    task->setAutoDelete(true);
                    QThreadPool::globalInstance()->start(task.get(), workerPriority);
                    task.release();
                    return true;
                }
                catch (...)
                {
                    failed.store(true);
                    running.store(false);
                    return false; // store强持owner与lease；显式重试，不抛出Qt/析构边界。
                }
            }

            bool publishReady(bool cleanupRequired = false) noexcept
            {
                ready.store(true);
                if (cleanupRequired) requestCleanup(true);
                if (!requested.load()) return true;
                scheduleAttempt(false); // 仅交接首次机会，绝不是一次外部Retry。
                return false;
            }

            void run() noexcept
            {
                attempts.fetch_add(1);
                bool succeeded = false;
                {
                    // 局部副本也必须在归还lease前于后台销毁，不能仅清成员闭包。
                    try
                    {
                        const auto operation = attempt;
                        succeeded = operation();
                    }
                    catch (...) {}
                    if (succeeded)
                    {
                        attempt = {};
                        requestStop = {};
                    }
                }
                if (!succeeded)
                {
                    failed.store(true);
                    running.store(false);
                    return; // 不轮询、不自动重复；accepted资源仍由store中的本状态拥有。
                }
                complete.store(true);
                running.store(false);
                lease.reset();
                forget(this);
            }
        };
    }

    // 在首次session Start前创建；create失败不能启动业务。
    // null也可能表示已接纳的旧epoch正在后台收尾，调用者必须废弃该owner/lifecycle，不能Reopen复用。
    // attempt只持值owner，必须后台证明STOP/consumer close/join/最终写入均完成才返回true。
    class RetainedCleanupJob final
    {
    public:
        using Attempt = std::function<bool()>;
        using RequestStop = std::function<void()>;
        static std::shared_ptr<RetainedCleanupJob> create(Attempt attempt, int workerPriority = 0,
            RequestStop requestStop = {}) noexcept
        {
            if (!attempt) return {};
            std::shared_ptr<retained_cleanup_detail::State> state;
            try
            {
                state = std::make_shared<retained_cleanup_detail::State>();
                state->attempt = std::move(attempt);
                state->requestStop = std::move(requestStop);
                state->workerPriority = workerPriority;
                {
                    auto& retained = retained_cleanup_detail::store();
                    std::lock_guard<std::mutex> lock(retained.mutex);
                    retained.jobs.emplace(state.get(), state);
                }
                const std::weak_ptr<retained_cleanup_detail::State> weak = state;
                state->lease = AsyncCleanupRegistry::global().acquire([weak]()
                {
                    if (const auto active = weak.lock()) active->requestCleanup(true);
                });
                if (!state->lease)
                {
                    retained_cleanup_detail::forget(state.get());
                    return {};
                }
                auto job = std::shared_ptr<RetainedCleanupJob>(new RetainedCleanupJob(state));
                if (!state->publishReady()) return {};
                return job;
            }
            catch (...)
            {
                if (state && state->lease)
                {
                    state->publishReady(true);
                }
                else if (state) retained_cleanup_detail::forget(state.get());
                return {};
            }
        }

        ~RetainedCleanupJob()
        {
            // 显式request后析构不算第二次外部重试，即使前次已经快速失败。
            m_state->requestCleanup(true);
        }
        RetainedCleanupJob(const RetainedCleanupJob&) = delete;
        RetainedCleanupJob& operator=(const RetainedCleanupJob&) = delete;
        bool requestCleanup() noexcept { return m_state->requestCleanup(); }
        bool isCleanupRequested() const { return m_state->requested.load(); }
        bool isInFlight() const { return m_state->running.load(); }
        bool hasFailed() const { return m_state->failed.load(); }
        bool isComplete() const { return m_state->complete.load(); }
        std::uint64_t attemptCount() const { return m_state->attempts.load(); }

    private:
        explicit RetainedCleanupJob(std::shared_ptr<retained_cleanup_detail::State> state)
            : m_state(std::move(state)) {}
        const std::shared_ptr<retained_cleanup_detail::State> m_state;
    };

    // 只响应用户/外部显式重试；不停止尚未request的active业务，不排第二个in-flight任务。
    inline std::size_t RetryRetainedCleanupJobs() noexcept
    {
        try
        {
            std::vector<std::shared_ptr<retained_cleanup_detail::State>> jobs;
            {
                auto& retained = retained_cleanup_detail::store();
                std::lock_guard<std::mutex> lock(retained.mutex);
                for (const auto& entry : retained.jobs) jobs.push_back(entry.second);
            }
            std::size_t started = 0;
            for (const auto& job : jobs)
            {
                if (job->requested.load() && job->failed.load() && !job->complete.load()
                    && job->requestCleanup()) ++started;
            }
            return started;
        }
        catch (...) { return 0; }
    }
}
