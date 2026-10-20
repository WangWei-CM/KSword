#pragma once

// Finished value ownership only. Reuse the existing Qt pool for potentially
// large vector/map destruction; no manager, resident thread or QObject state.
#include <QRunnable>
#include <QThreadPool>

#include <memory>
#include <utility>

namespace ks::ui
{
    template<class T> std::shared_ptr<T> OwnWorkerValue(T* value)
    {
        struct Disposal final : QRunnable
        {
            explicit Disposal(T* object) : owned(object) {}
            void run() override { owned.reset(); }
            std::unique_ptr<T> owned;
        };
        // Allocate before accepting ownership so the final GUI release only
        // submits an existing pure-value task to the existing pool.
        std::unique_ptr<T> guard(value);
        auto* task = new Disposal(value);
        guard.release();
        return std::shared_ptr<T>(value, [task](T*) noexcept
        {
            try { QThreadPool::globalInstance()->start(task); }
            catch (...)
            {
                // A failed allocator may retain this finished pure value until
                // process exit. Never move a large destruction or native
                // teardown onto the GUI as an allocation-failure fallback.
            }
        });
    }
    template<class T, class... Args> std::shared_ptr<T> MakeWorkerValue(Args&&... args)
    { return OwnWorkerValue(new T(std::forward<Args>(args)...)); }
}
