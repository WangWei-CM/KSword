#pragma once

#include <cstdint>
#include <mutex>

namespace ks::process
{
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

        std::uint64_t Generation() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_generation;
        }

    private:
        mutable std::mutex m_mutex;
        bool m_desired = false;
        bool m_active = false;
        bool m_stopRequired = false;
        bool m_workerScheduled = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_attemptedGeneration = 0;
    };
}
