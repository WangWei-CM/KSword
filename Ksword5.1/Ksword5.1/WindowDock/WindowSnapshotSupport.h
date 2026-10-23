#pragma once
#include <QHash>
#include <QRunnable>
#include <QString>
#include <QThreadPool>
#include <memory>
#include <tuple>
#include <utility>

namespace ks::window
{
    // Finite context templates are resolved by the GUI owner before dispatch.
    // Worker copies read only these values; no language QObject/singleton is borrowed.
    using SnapshotTexts = QHash<QString, QString>;

    namespace detail
    {
        template<typename Scheduler, typename... Values>
        void retireWithScheduler(Scheduler&& schedule, Values... values) noexcept
        {
            // The runnable owns the complete values even if a queued task is discarded.
            // If allocation/queueing fails, scope ownership still releases them exactly once.
            try
            {
                auto retired = std::make_tuple(std::move(values)...);
                auto task = std::unique_ptr<QRunnable>(QRunnable::create([retired = std::move(retired)] {}));
                schedule(task.get());
                task.release();
            }
            catch (...) {}
        }
    }

    template<typename... Values> void retireSnapshotValues(Values... values) noexcept
    {
        detail::retireWithScheduler([](QRunnable* task) { QThreadPool::globalInstance()->start(task); },
            std::move(values)...);
    }

    template<typename Value> std::shared_ptr<const Value> makeSnapshotValue(Value value)
    {
        return std::shared_ptr<const Value>(new Value(std::move(value)), [](const Value* previous) noexcept {
            retireSnapshotValues(std::unique_ptr<const Value>(previous));
        });
    }
}
