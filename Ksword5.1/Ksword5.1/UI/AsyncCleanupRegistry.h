#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ks::ui
{
    // 只登记已接受工作；不持QObject，不等待线程。stop只置停止标志并唤醒。
    class AsyncCleanupRegistry final
    {
        struct State
        {
            std::mutex mutex;
            std::uint64_t nextId = 0;
            bool shuttingDown = false;
            std::unordered_map<std::uint64_t, std::function<void()>> pending;
        };
    public:
        class Lease final
        {
        public:
            ~Lease()
            {
                if (!m_id) return;
                std::function<void()> stopped;
                std::lock_guard<std::mutex> lock(m_state->mutex);
                const auto it = m_state->pending.find(m_id);
                if (it != m_state->pending.end())
                {
                    stopped = std::move(it->second);
                    m_state->pending.erase(it);
                }
            }
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
        private:
            friend class AsyncCleanupRegistry;
            Lease(std::shared_ptr<State> state, std::uint64_t id)
                : m_state(std::move(state)), m_id(id) {}
            const std::shared_ptr<State> m_state;
            std::uint64_t m_id;
        };

        static AsyncCleanupRegistry& global()
        {
            static AsyncCleanupRegistry registry;
            return registry;
        }

        // stop闭包不能强持有同时拥有该lease的对象；调用方在工作完全收尾后归还lease。
        std::shared_ptr<Lease> acquire(std::function<void()> requestStop = {})
        {
            // 分配失败或map插入失败时，未登记lease不尝试重入同一把锁。
            auto lease = std::shared_ptr<Lease>(new Lease(m_state, 0));
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->shuttingDown) return {};
            const auto id = ++m_state->nextId;
            m_state->pending.emplace(id, std::move(requestStop));
            lease->m_id = id;
            return lease;
        }

        void beginShutdown()
        {
            std::vector<std::function<void()>> stops;
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                if (m_state->shuttingDown) return;
                for (const auto& entry : m_state->pending)
                {
                    if (entry.second) stops.push_back(entry.second);
                }
                m_state->shuttingDown = true;
            }
            for (const auto& stop : stops)
            {
                try { stop(); } catch (...) {}
            }
        }

        // 取消退出只重新开放接纳，已请求STOP的会话仍正常收尾。
        void cancelShutdown()
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->shuttingDown = false;
        }
        std::size_t pending() const
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->pending.size();
        }
    private:
        const std::shared_ptr<State> m_state = std::make_shared<State>();
    };

    // GUI以定时器轮询该值状态机；超时不许可终止未完成副作用。
    class AsyncShutdownWait final
    {
    public:
        enum class Status { Waiting, Ready, TimedOut };
        void start(std::int64_t nowMs, std::int64_t timeoutMs)
        {
            m_startedMs = nowMs;
            m_timeoutMs = timeoutMs;
        }
        Status poll(std::int64_t nowMs, std::size_t pending) const
        {
            if (!pending) return Status::Ready;
            return nowMs - m_startedMs >= m_timeoutMs ? Status::TimedOut : Status::Waiting;
        }
    private:
        std::int64_t m_startedMs = 0;
        std::int64_t m_timeoutMs = 0;
    };
}
