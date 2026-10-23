#pragma once

#include "./OfflineHiveWorker.h"
#include "../UI/RetainedCleanupJob.h"
#include <QDebug>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace ks::registry::offline
{
    class Session final
    {
        struct Gate
        {
            std::mutex mutex;
            std::size_t reservations = 0;
            bool closing = false;
            bool discard = false;
            std::weak_ptr<ks::ui::RetainedCleanupJob> job;
            void stop(bool discardUnsaved = false)
            {
                std::lock_guard<std::mutex> lock(mutex);
                closing = true;
                discard = discard || discardUnsaved;
            }
            bool ready()
            {
                std::lock_guard<std::mutex> lock(mutex);
                return closing && reservations == 0;
            }
            bool mayDiscard()
            {
                std::lock_guard<std::mutex> lock(mutex);
                return discard;
            }
        };
    public:
        struct Reservation final
        {
            ~Reservation()
            {
                if (!armed) return;
                std::shared_ptr<ks::ui::RetainedCleanupJob> cleanupJob;
                {
                    std::lock_guard<std::mutex> lock(gate->mutex);
                    --gate->reservations;
                    if (gate->closing && gate->reservations == 0) cleanupJob = retainedJob;
                }
                if (cleanupJob) cleanupJob->notifyReadyToAttempt();
            }
            Reservation(const Reservation&) = delete;
            Reservation& operator=(const Reservation&) = delete;
        private:
            explicit Reservation(std::shared_ptr<Gate> owner, std::shared_ptr<ks::ui::RetainedCleanupJob> job)
                : gate(std::move(owner)), retainedJob(std::move(job)) {}
            std::shared_ptr<Gate> gate;
            std::shared_ptr<ks::ui::RetainedCleanupJob> retainedJob;
            bool armed = false;
            friend class Session;
        };
        // Must succeed before Open is submitted. Native resources are still empty
        // at admission; a shutdown-raced null never starts a Hive or file copy.
        static std::shared_ptr<Session> create()
        {
            auto native = std::make_shared<State>();
            auto gate = std::make_shared<Gate>();
            auto job = ks::ui::RetainedCleanupJob::create([native, gate]() {
                const bool done = cleanup(*native, gate->mayDiscard());
                const auto receipt = cleanupReceipt(*native);
                if (done && receipt.retained)
                    try { qWarning().noquote() << "KSword::OfflineHive retained unsaved working copy:" << receipt.retainedPath; } catch (...) {}
                return done;
            }, 0, [gate]() { gate->stop(); }, [gate]() { return gate->ready(); });
            if (!job) return {};
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->job = job;
            }
            return std::shared_ptr<Session>(new Session(std::move(native), std::move(gate), std::move(job)));
        }
        ~Session() { m_gate->stop(); }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        std::shared_ptr<Reservation> reserve()
        {
            auto reservation = std::shared_ptr<Reservation>(new Reservation(m_gate, m_job));
            std::lock_guard<std::mutex> lock(m_gate->mutex);
            if (m_gate->closing) return {};
            ++m_gate->reservations;
            reservation->armed = true;
            return reservation;
        }
        void stop(bool discardUnsaved = false)
        {
            m_gate->stop(discardUnsaved);
            m_job->requestCleanup();
        }
        const std::shared_ptr<State>& native() const { return m_native; }
        const std::shared_ptr<ks::ui::RetainedCleanupJob>& job() const { return m_job; }
    private:
        Session(std::shared_ptr<State> native, std::shared_ptr<Gate> gate, std::shared_ptr<ks::ui::RetainedCleanupJob> job)
            : m_native(std::move(native)), m_gate(std::move(gate)), m_job(std::move(job)) {}
        const std::shared_ptr<State> m_native;
        const std::shared_ptr<Gate> m_gate;
        const std::shared_ptr<ks::ui::RetainedCleanupJob> m_job;
    };
}
