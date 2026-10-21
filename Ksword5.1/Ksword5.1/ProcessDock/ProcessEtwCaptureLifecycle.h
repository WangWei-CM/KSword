#pragma once

#include <cstdint>
#include <mutex>

namespace ks::process
{
    struct ProcessEtwCaptureDemand
    {
        bool monitoring = false;
        bool processListVisible = false;
        bool background = false;
        bool visibleDetail = false;
        bool cpuColumnVisible = false;

        bool RefreshAllowed() const
        {
            return monitoring && (processListVisible || background || visibleDetail);
        }
        bool RecordingAllowed() const
        {
            return monitoring && (processListVisible || background);
        }
        bool CpuCaptureAllowed() const
        {
            return RefreshAllowed() && (cpuColumnVisible || visibleDetail);
        }
    };

    // One worker owns all Start/Stop calls. Requests are cheap and never wait for ETW.
    // A failed start is retried only after a new false -> true demand generation.
    class ProcessEtwCaptureLifecycle final
    {
    public:
        enum class Operation { None, Start, Stop };

        bool Request(const bool desired)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (m_desired != desired)
            {
                m_desired = desired;
                ++m_generation;
                if (!desired && (m_active || m_workerScheduled))
                {
                    m_stopRequired = true;
                }
            }
            if (m_workerScheduled || (!m_desired && !m_active) ||
                (m_desired && (m_active || m_attemptedGeneration == m_generation)))
            {
                return false;
            }
            m_workerScheduled = true;
            return true;
        }

        Operation NextOperation()
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (m_active && (!m_desired || m_stopRequired))
            {
                return Operation::Stop;
            }
            if (m_desired && !m_active && m_attemptedGeneration != m_generation)
            {
                m_attemptedGeneration = m_generation;
                return Operation::Start;
            }
            m_workerScheduled = false;
            return Operation::None;
        }

        void CompleteStart(const bool started)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_active = started;
            if (!started)
            {
                m_stopRequired = false;
            }
        }

        void CompleteStop()
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_active = false;
            m_stopRequired = false;
        }

        // Background only. Never acquire this gate from GUI request/visibility handlers.
        std::unique_lock<std::mutex> LockBackgroundOperation()
        {
            return std::unique_lock<std::mutex>(m_operationMutex);
        }

        template<typename Callback>
        bool SnapshotIfCurrent(const std::uint64_t generation, Callback callback)
        {
            const auto operationGuard = LockBackgroundOperation();
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                if (generation != m_generation || !m_active || !m_desired || m_workerScheduled)
                {
                    return false;
                }
            }
            // Start/Stop cannot replace the session between this check and consumption.
            // Request still takes only m_mutex and never waits for this callback.
            callback();
            return true;
        }

        bool IsSettledActive() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_active && m_desired && !m_workerScheduled;
        }

        std::uint64_t Generation() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_generation;
        }

    private:
        mutable std::mutex m_mutex;
        std::mutex m_operationMutex; // Lock order: operation -> state; Request only locks state.
        bool m_desired = false;
        bool m_active = false;
        bool m_stopRequired = false;
        bool m_workerScheduled = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_attemptedGeneration = 0;
    };
}
