// QtCore 事件循环测试真实协调器；不创建 QApplication 或任何 QWidget。
#include "../Ksword5.1/Ksword5.1/UI/UiCommitCoordinator.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace
{
    int checks = 0;
    int failures = 0;

    void expect(bool condition, const char* label)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("UI_COMMIT_LOGIC_FAILURE=%s\n", label);
        }
    }

    void drain()
    {
        for (int turn = 0; turn < 12; ++turn)
        {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents(QEventLoop::AllEvents);
        }
    }

    struct Harness
    {
        bool blocked = true;
        qint64 now = 0;
        qint64 probeCost = 0;
        ks::ui::UiCommitInteractionProbes probes;
        ks::ui::UiCommitDrainOptions options;
        std::unique_ptr<ks::ui::UiCommitCoordinator> coordinator;

        explicit Harness(qint64 budget = 4'000'000)
        {
            probes.leftCtrlHeld = [this]() { now += probeCost; return blocked; };
            probes.comboPopupOpen = []() { return false; };
            options.softBudgetNanoseconds = budget;
            options.monotonicNanoseconds = [this]() { return now; };
            coordinator = std::make_unique<ks::ui::UiCommitCoordinator>(nullptr, probes, options);
        }
    };

    void budgetAndContinuation()
    {
        Harness h;
        QObject a, b, c;
        int completed = 0;
        for (QObject* owner : {&a, &b, &c})
        {
            h.coordinator->submit(owner, QStringLiteral("refresh"), {}, [&]()
            {
                ++completed;
                h.now += 2'000'000;
            });
        }
        expect(h.coordinator->pendingCount() == 3, "blocked owners accumulate independently");
        h.blocked = false;
        bool marker = false;
        QTimer::singleShot(0, QCoreApplication::instance(), [&]() { marker = true; });
        h.coordinator->flush();
        expect(completed == 2 && h.coordinator->pendingCount() == 1,
            "all owners share one four millisecond budget");
        expect(!marker, "flush yields before queued event runs");
        drain();
        expect(marker && completed == 3 && h.coordinator->pendingCount() == 0,
            "queued continuation resumes without another user event");

        // 一个不可抢占回调超预算时，必须完成它，但后续回调留给下一轮。
        h.blocked = true;
        h.coordinator->submit(&a, QStringLiteral("large"), {}, [&]() { ++completed; h.now += 40'000'000; });
        h.coordinator->submit(&b, QStringLiteral("small"), {}, [&]() { ++completed; });
        h.blocked = false;
        h.coordinator->flush();
        expect(completed == 4 && h.coordinator->pendingCount() == 1,
            "atomic callback may exceed soft budget but does not pull next callback into its turn");
        drain();
        expect(completed == 5, "callback after oversized action is retained");
    }

    void latestAndReentry()
    {
        Harness h;
        QObject owner;
        int value = 0;
        h.coordinator->submit(&owner, QStringLiteral("snapshot"), {}, [&]() { value = 1; });
        h.coordinator->submit(&owner, QStringLiteral("snapshot"), {}, [&]() { value = 2; });
        expect(h.coordinator->pendingCount() == 1, "latest state replaces same owner key");
        h.blocked = false;
        h.coordinator->flush();
        expect(value == 2, "only newest snapshot executes");

        h.blocked = true;
        h.coordinator->submit(&owner, QStringLiteral("parent"), {}, [&]()
        {
            value = 3;
            const auto result = h.coordinator->submit(&owner, QStringLiteral("child"), {}, [&]() { value = 4; });
            expect(result == ks::ui::UiCommitSubmission::Deferred && value == 3,
                "reentrant submit waits for next generation rather than executing recursively");
            h.coordinator->submit(&owner, QStringLiteral("child"), {}, [&]() { value = 5; });
            h.coordinator->flush(); // 嵌套排空仅记录复查请求。
            expect(value == 3, "nested flush never traverses queue recursively");
        });
        h.blocked = false;
        h.coordinator->flush();
        expect(value == 3 && h.coordinator->pendingCount() == 1,
            "new generations are excluded from current drain round");
        drain();
        expect(value == 5 && h.coordinator->pendingCount() == 0,
            "reentrant latest snapshot executes in continuation");

        // 真实业务的旧式 refresh 自回调必须只穿过一次 guard，不能永久自排队。
        h.blocked = true;
        int refreshes = 0;
        std::function<void()> refresh;
        refresh = [&]()
        {
            if (h.coordinator->deferIfBlocked(&owner, QStringLiteral("legacy"), {}, refresh)) return;
            ++refreshes;
            h.coordinator->submit(&owner, QStringLiteral("legacy"), {}, [&]() { ++refreshes; });
        };
        refresh();
        h.blocked = false;
        h.coordinator->flush();
        expect(refreshes == 1 && h.coordinator->pendingCount() == 1,
            "legacy self guard passes but nested submit remains deferred");
        drain();
        expect(refreshes == 2 && h.coordinator->pendingCount() == 0,
            "legacy self callback is not trapped in endless requeue");

        h.blocked = true;
        h.coordinator->submit(&owner, QStringLiteral("fresh"), {}, [&]() { value = 6; });
        h.blocked = false;
        expect(h.coordinator->submit(&owner, QStringLiteral("fresh"), {}, [&]() { value = 7; })
            == ks::ui::UiCommitSubmission::Executed, "ordinary immediate path remains compatible");
        drain();
        expect(value == 7 && h.coordinator->pendingCount() == 0,
            "fresh immediate result invalidates earlier pending result");
    }

    void lifecycleAndBarriers()
    {
        Harness h;
        auto* owner = new QObject;
        auto payload = std::make_shared<int>(9);
        const std::weak_ptr<int> weak = payload;
        h.coordinator->submit(owner, QStringLiteral("lifetime"), {}, [payload]() {});
        payload.reset();
        delete owner;
        expect(weak.expired() && h.coordinator->pendingCount() == 0,
            "destroyed owner releases indexed pending snapshot immediately");

        QObject a, b;
        int completed = 0;
        h.coordinator->submit(&a, QStringLiteral("first"), {}, [&]() { ++completed; h.blocked = true; });
        h.coordinator->submit(&b, QStringLiteral("second"), {}, [&]() { ++completed; });
        h.blocked = false;
        h.coordinator->flush();
        expect(completed == 1 && h.coordinator->pendingCount() == 1,
            "barrier reopened by callback blocks remaining owners");
        drain();
        expect(completed == 1, "blocked queue does not busy drain on zero timers");
        h.blocked = false;
        h.coordinator->notifyInteractionEnded();
        drain();
        expect(completed == 2 && h.coordinator->pendingCount() == 0,
            "interaction release resumes previously skipped pending action");

        // 预算用于屏障检查时也保存游标；真实释放事件重新核对之前跳过的项。
        h.blocked = true;
        for (int index = 0; index < 10; ++index)
            h.coordinator->submit(&a, QString::number(index), {}, [&]() { ++completed; });
        h.probeCost = 2'000'000;
        h.coordinator->flush();
        expect(completed == 2 && h.coordinator->pendingCount() == 10,
            "blocked scans respect budget without dropping entries");
        h.blocked = false;
        h.probeCost = 0;
        h.coordinator->notifyInteractionEnded();
        drain();
        expect(completed == 12 && h.coordinator->pendingCount() == 0,
            "release during continuation rechecks earlier blocked generations");

        h.blocked = true;
        auto* disposable = new ks::ui::UiCommitCoordinator(nullptr, h.probes, h.options);
        const QPointer<ks::ui::UiCommitCoordinator> guarded(disposable);
        disposable->submit(&a, QStringLiteral("self"), {}, [disposable]() { delete disposable; });
        h.blocked = false;
        disposable->flush();
        expect(guarded.isNull(), "callback can destroy coordinator without accessing deleted members");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    budgetAndContinuation();
    latestAndReentry();
    lifecycleAndBarriers();
    std::printf("UI_COMMIT_LOGIC_CHECKS=%d FAILURES=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
