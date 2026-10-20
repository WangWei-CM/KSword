#pragma once
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace ks::monitor
{
    // Publication never shares the archive writer/IO mutex. The immutable session
    // identity remains alive for readers while only the accepted watermark moves.
    class EtwArchiveMetadata
    {
    public:
        struct Snapshot { QString directory; std::uint64_t acceptedSequence = 0; };
        void beginSession(QString directory)
        {
            m_session.store(
                std::make_shared<const Session>(std::move(directory)), std::memory_order_release);
        }
        // Called under the archive writer lock only after append has succeeded.
        void accepted(const std::uint64_t sequence)
        {
            const auto session = m_session.load(std::memory_order_acquire);
            if (session) session->acceptedSequence.store(sequence, std::memory_order_release);
        }
        Snapshot snapshot() const
        {
            const auto session = m_session.load(std::memory_order_acquire);
            return session ? Snapshot{ session->directory,
                session->acceptedSequence.load(std::memory_order_acquire) } : Snapshot{};
        }
    private:
        struct Session
        {
            explicit Session(QString value) : directory(std::move(value)) {}
            const QString directory;
            mutable std::atomic<std::uint64_t> acceptedSequence{ 0 };
        };
        std::atomic<std::shared_ptr<const Session>> m_session;
    };
}
