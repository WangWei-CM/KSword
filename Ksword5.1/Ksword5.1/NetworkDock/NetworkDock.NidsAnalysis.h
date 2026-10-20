#pragma once
#include "../ksword/network/network_nids.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

namespace network_dock_detail
{
    struct NidsAnalysisBatch
    {
        std::uint64_t analyzedPackets = 0;
        std::vector<ks::network::NidsAlert> alerts;
        bool failed = false;
    };

    // A value-only FIFO session. Exactly one worker invokes analyzeNextBatch;
    // enqueue/pendingCount may run on the GUI thread. The legacy engine's packet
    // order and timestamp semantics are unchanged; no lateness cutoff is enabled.
    class NidsAnalysisSession final
    {
    public:
        void enqueue(const ks::network::PacketRecord& packet)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pending.push_back(Input{false, packet});
            ++m_pendingPackets;
        }
        void enqueueReset()
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pending.push_back(Input{true, {}}); // Ordered after all already accepted packets.
        }
        std::size_t pendingPacketCount() const
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            return m_pendingPackets + m_activePacketRemaining.load();
        }
        std::size_t pendingCount() const
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            return m_pending.size() + m_activeRemaining.load() + (m_unacknowledged ? 1 : 0);
        }
        bool failed() const { return m_failed.load(); }
        void acknowledge(const std::shared_ptr<NidsAnalysisBatch>& batch)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_unacknowledged == batch) m_unacknowledged.reset();
        }

        // A delayed lookup must never attach the identity of a PID born after
        // this packet. Sub-millisecond ambiguity/clock rollback is conservative:
        // keep the endpoint evidence and leave application identity unavailable.
        static bool identityPredatesPacket(std::uint64_t creation100ns, std::uint64_t packetUnixMs)
        {
            constexpr std::uint64_t epoch100ns = 116444736000000000ULL;
            constexpr std::uint64_t unitsPerMs = 10000ULL;
            if (creation100ns == 0 || packetUnixMs == 0
                || packetUnixMs > (std::numeric_limits<std::uint64_t>::max() - epoch100ns) / unitsPerMs) return false;
            return creation100ns <= epoch100ns + packetUnixMs * unitsPerMs;
        }

        template<class CaptureIdentity>
        std::shared_ptr<NidsAnalysisBatch> analyzeNextBatch(CaptureIdentity captureIdentity, std::size_t limit = 128)
        {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                // The engine must never replay input already analyzed. If its
                // UI receipt vanished, deliver the exact same complete result.
                if (m_unacknowledged) return m_unacknowledged;
            }
            auto result = std::make_shared<NidsAnalysisBatch>();
            if (failed()) { result->failed = true; return result; }
            // Allocate before removing accepted inputs. Active inputs stay owned
            // by the session if analysis fails; only an explicit clear discards.
            m_active.clear();
            m_active.reserve(limit);
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                const auto count = (std::min)(limit, m_pending.size());
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (!m_pending.front().reset) --m_pendingPackets;
                    m_active.push_back(std::move(m_pending.front()));
                    m_pending.pop_front();
                }
                m_activeRemaining.store(m_active.size());
                m_activePacketRemaining.store(static_cast<std::size_t>(std::count_if(m_active.begin(), m_active.end(),
                    [](const Input& input) { return !input.reset; })));
            }
            for (const auto& input : m_active)
            {
                const auto priorAlerts = result->alerts.size();
                try
                {
                    if (input.reset)
                    {
                        m_engine.Reset();
                        m_activeRemaining.fetch_sub(1);
                        continue;
                    }
                    auto alerts = m_engine.AnalyzePacket(input.packet);
                    for (auto& alert : alerts) captureIdentity(alert);
                    result->alerts.insert(result->alerts.end(),
                        std::make_move_iterator(alerts.begin()), std::make_move_iterator(alerts.end()));
                    ++result->analyzedPackets;
                    m_activeRemaining.fetch_sub(1);
                    m_activePacketRemaining.fetch_sub(1);
                }
                catch (...)
                {
                    // An allocating engine may have changed partial internal
                    // state. Preserve its inputs, publish the completed prefix,
                    // and require explicit clear rather than replaying twice.
                    result->alerts.resize(priorAlerts);
                    m_failed.store(true);
                    result->failed = true;
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    m_unacknowledged = result;
                    return result;
                }
            }
            m_active.clear();
            if (result->analyzedPackets != 0 || result->failed)
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_unacknowledged = result;
            }
            return result;
        }

    private:
        mutable std::mutex m_queueMutex;
        struct Input { bool reset = false; ks::network::PacketRecord packet; };
        std::deque<Input> m_pending;
        std::size_t m_pendingPackets = 0;
        std::shared_ptr<NidsAnalysisBatch> m_unacknowledged; // complete values until canonical GUI acceptance
        std::vector<Input> m_active; // worker only; retained after fault
        std::atomic<std::size_t> m_activeRemaining{0};
        std::atomic<std::size_t> m_activePacketRemaining{0};
        std::atomic_bool m_failed{false};
        ks::network::NidsEngine m_engine; // worker only; default legacy semantics
    };
}
