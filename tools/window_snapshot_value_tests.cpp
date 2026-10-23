#include "../Ksword5.1/Ksword5.1/WindowDock/WindowSnapshotSupport.h"
#include <QCoreApplication>
#include <QThread>
#include <QSemaphore>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <cstdio>
#include <thread>
#include <stdexcept>

namespace {
int checks = 0, failures = 0;
void expect(bool value, const char* name) { ++checks; if (!value) { ++failures; std::printf("FAIL %s\n", name); } }
struct Tracked {
    QThread* owner = nullptr;
    std::shared_ptr<std::atomic<int>> deleted;
    std::unique_ptr<int> identity = std::make_unique<int>(1);
    QVector<QStringList> rows;
    Tracked(QThread* thread, std::shared_ptr<std::atomic<int>> result) : owner(thread), deleted(std::move(result)) {}
    Tracked(Tracked&&) = default;
    ~Tracked() { if (identity) deleted->store(QThread::currentThread() == owner ? 1 : 2); }
};
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto* owner = QThread::currentThread();
    ks::window::SnapshotTexts original{{QStringLiteral("status"), QStringLiteral("frozen %1")}};
    const auto frozen = original;
    original[QStringLiteral("status")] = QStringLiteral("new language %1");
    QString rendered;
    std::thread format([&] { rendered = frozen.value(QStringLiteral("status")).arg(65536); });
    format.join();
    expect(rendered == QStringLiteral("frozen 65536"), "worker observes dispatch-time frozen context value");
    expect(original.value(QStringLiteral("status")) == QStringLiteral("new language %1"), "freeze does not alias later owner mutation");
    auto deleted = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<const Tracked> value;
    std::thread worker([&] {
        Tracked payload(owner, deleted);
        payload.rows.reserve(65536);
        for (int row = 0; row < 65536; ++row) {
            QStringList cells;
            for (int column = 0; column < 15; ++column) cells.append(QStringLiteral("%1:%2").arg(row).arg(column));
            payload.rows.append(std::move(cells));
        }
        value = ks::window::makeSnapshotValue(std::move(payload));
    });
    worker.join();
    expect(value->rows.size() == 65536 && value->rows.last().size() == 15, "complete maximum GUI handle value shape retained");
    expect(value->rows.last().last() == QStringLiteral("65535:14"), "last field survives worker publication");
    expect(deleted->load() == 0, "moved producer does not destroy accepted snapshot");
    value.reset();
    QThreadPool::globalInstance()->waitForDone();
    expect(deleted->load() == 2, "GUI final release schedules complete payload deletion off owner thread");
    auto cacheDeleted = std::make_shared<std::atomic<int>>(0);
    auto cache = std::make_shared<Tracked>(owner, cacheDeleted);
    ks::window::retireSnapshotValues(std::move(cache));
    QThreadPool::globalInstance()->waitForDone();
    expect(!cache && cacheDeleted->load() == 2, "replacement cache transfer retires on worker");
    {
        QThreadPool pool;
        pool.setMaxThreadCount(1);
        QSemaphore started, release;
        pool.start([&] { started.release(); release.acquire(); });
        started.acquire();
        auto cleared = std::make_shared<std::atomic<int>>(0);
        ks::window::detail::retireWithScheduler([&](QRunnable* task) { pool.start(task); },
            std::make_unique<Tracked>(owner, cleared));
        expect(cleared->load() == 0, "queued retirement owns complete payload before run");
        pool.clear();
        expect(cleared->load() == 1, "discarded queued runnable releases owned payload without leaking");
        release.release(); pool.waitForDone();
        expect(cleared->load() == 1, "queue clear releases payload exactly once");
    }
    {
        auto rejected = std::make_shared<std::atomic<int>>(0);
        ks::window::detail::retireWithScheduler([](QRunnable*) { throw std::runtime_error("synthetic queue failure"); },
            std::make_unique<Tracked>(owner, rejected));
        expect(rejected->load() == 1, "scheduling exception cannot escape deleter and releases payload");
    }
    std::printf("WINDOW_SNAPSHOT_VALUE_CHECKS=%d FAILURES=%d\n", checks, failures);
    return failures ? 1 : 0;
}
