#pragma once

// A token belongs to one explicit parse request. Hiding a page never cancels it.
// Scopes are worker-local and restore the previous token on every unwind path.
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace ks::minidump
{
    using DumpCancellationToken = std::shared_ptr<std::atomic_bool>;

    class DumpWorkCancelled final : public std::exception
    {
    public:
        const char* what() const noexcept override { return "dump work cancelled"; }
    };

    inline thread_local const std::atomic_bool* g_dumpCancellation = nullptr;

    inline void CheckDumpCancellation()
    {
        if (g_dumpCancellation != nullptr &&
            g_dumpCancellation->load(std::memory_order_relaxed))
        {
            throw DumpWorkCancelled();
        }
    }

    class ScopedDumpCancellation final
    {
    public:
        explicit ScopedDumpCancellation(const DumpCancellationToken& token) noexcept
            : m_previous(g_dumpCancellation), m_token(token)
        {
            g_dumpCancellation = m_token.get();
        }
        ~ScopedDumpCancellation() { g_dumpCancellation = m_previous; }
        ScopedDumpCancellation(const ScopedDumpCancellation&) = delete;
        ScopedDumpCancellation& operator=(const ScopedDumpCancellation&) = delete;

    private:
        const std::atomic_bool* m_previous;
        DumpCancellationToken m_token;
    };

    // Keep the shared DbgHelp mutex type and serialization contract. Only waiting
    // for ownership is interruptible; an API already executing is never killed.
    inline std::unique_lock<std::mutex> LockDumpMutex(std::mutex& mutex)
    {
        std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
        for (;;)
        {
            CheckDumpCancellation();
            if (lock.try_lock())
            {
                CheckDumpCancellation();
                return lock;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
