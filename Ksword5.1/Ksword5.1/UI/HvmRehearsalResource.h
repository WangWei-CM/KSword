#pragma once

#include "RetainedCleanupJob.h"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace ks::ui
{
    // Values only: reservations outlive the wizard and retain the cleanup job.
    // A removed view is the proof required before returning its physical page.
    class HvmRehearsalResource final : public std::enable_shared_from_this<HvmRehearsalResource>
    {
    public:
        using RemoveView = std::function<bool(unsigned long)>;
        using ReleasePage = std::function<bool(void*)>;
        struct Reservation final
        {
            std::shared_ptr<HvmRehearsalResource> resource;
            std::shared_ptr<RetainedCleanupJob> cleanup;
            bool admitted = false;
            void recordInstalledView(unsigned long viewId)
            {
                resource->recordInstalledView(viewId);
            }
            ~Reservation()
            {
                if (!admitted) return;
                bool idle = false;
                {
                    std::lock_guard<std::mutex> lock(resource->m_gate);
                    idle = --resource->m_active == 0;
                }
                // A readiness notification is not permission to retry failure.
                if (idle) cleanup->notifyReadyToAttempt();
            }
        };

        HvmRehearsalResource(RemoveView removeView, ReleasePage releasePage)
            : m_removeView(std::move(removeView)), m_releasePage(std::move(releasePage)) {}

        void* allocatePage(const std::function<void*()>& allocate)
        {
            std::lock_guard<std::mutex> lock(m_gate);
            if (m_closing || m_page) return nullptr;
            // One local page, with no event pump or driver I/O. Closing cannot
            // pass the atomic publication of allocation ownership.
            m_page = allocate();
            return m_page;
        }
        std::shared_ptr<Reservation> reserve(const std::shared_ptr<RetainedCleanupJob>& cleanup)
        {
            if (!cleanup) return {};
            auto token = std::make_shared<Reservation>();
            token->resource = shared_from_this();
            token->cleanup = cleanup;
            std::lock_guard<std::mutex> lock(m_gate);
            if (m_closing) return {};
            ++m_active;
            token->admitted = true;
            return token;
        }
        void close()
        {
            std::lock_guard<std::mutex> lock(m_gate);
            m_closing = true;
        }
        bool isIdle() const
        {
            std::lock_guard<std::mutex> lock(m_gate);
            return m_closing && m_active == 0;
        }
        void recordInstalledView(unsigned long viewId)
        {
            if (!viewId) return;
            std::lock_guard<std::mutex> lock(m_gate);
            for (std::size_t i = 0; i < m_viewCount; ++i)
                if (m_views[i] == viewId) return;
            // More distinct accepted views than the protocol capacity is an
            // uncertain ownership state: retain the page instead of freeing it.
            if (m_viewCount == m_views.size()) { m_uncertain = true; return; }
            m_views[m_viewCount++] = viewId;
        }
        bool cleanup()
        {
            {
                std::lock_guard<std::mutex> lock(m_gate);
                if (!m_closing || m_active != 0 || m_uncertain) return false;
            }
            // No accepted worker remains and closing prevents new reservations.
            // The retained job serializes attempts; never hold the gate over I/O.
            while (m_viewCount != 0)
            {
                if (!m_removeView(m_views[m_viewCount - 1])) return false;
                --m_viewCount;
            }
            if (m_page && !m_releasePage(m_page)) return false;
            m_page = nullptr;
            return true;
        }
    private:
        mutable std::mutex m_gate;
        bool m_closing = false;
        bool m_uncertain = false;
        std::size_t m_active = 0;
        void* m_page = nullptr;
        // KSWORD_ARK_HVM_MAX_VIEWS is currently 32. The consumer asserts this.
        std::array<unsigned long, 32> m_views{};
        std::size_t m_viewCount = 0;
        RemoveView m_removeView;
        ReleasePage m_releasePage;
    };
}
