#include "UI/CompleteExport.h"
#include "UI/AsyncCleanupRegistry.h"
#include "fake/QSaveFile"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSemaphore>
#include <QThreadPool>
#include <QRunnable>

#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace ks::ui;
static int checks = 0;
static void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class Predicate> static void pump(Predicate predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 5000)
    { QCoreApplication::processEvents(); std::this_thread::yield(); }
    check(predicate(), "completion timeout");
}

struct Values
{
    QString text;
    std::uint64_t count = 1;
};
struct PublishedValues
{
    explicit PublishedValues(QString value) : text(std::move(value)) {}
    PublishedValues(const PublishedValues&) = delete;
    PublishedValues& operator=(const PublishedValues&) = delete;
    QString text;
};
static CompleteExportInput input(QString text = QStringLiteral("whole\t文档\n"), std::uint64_t count = 1)
{
    auto value = std::make_shared<const Values>(Values{ std::move(text), count });
    return MakeCompleteExportInput(value, QStringLiteral("fake/complete"), 42, count,
        ks::i18n::SourceTextSnapshot{}, [](const Values& snapshot, CompleteExportSink& sink,
            const ks::i18n::SourceTextSnapshot& translator)
        {
            for (std::uint64_t row = 0; row < snapshot.count; ++row)
                if (!sink.writeText(translator.displayText(snapshot.text)) || !sink.rowsCompleted()) return false;
            return true;
        });
}

class Device final : public CompleteExportAtomicDevice
{
public:
    QByteArray data;
    CompleteExportFileMode mode = CompleteExportFileMode::Binary;
    bool openResult = true, flushResult = true, commitResult = true;
    int openCalls = 0, flushCalls = 0, commitCalls = 0, cancelCalls = 0;
    qint64 limit = 7;
    bool open(const QString&, CompleteExportFileMode value) override { ++openCalls; mode = value; return openResult; }
    qint64 write(const char* bytes, qint64 length) override
    {
        if (limit <= 0) return limit;
        if (limit == std::numeric_limits<qint64>::max()) return length + 1;
        auto count = std::min(limit, length); data.append(bytes, count); return count;
    }
    bool flush() override { ++flushCalls; return flushResult; }
    bool commit() override { ++commitCalls; return commitResult; }
    void cancel() override { ++cancelCalls; }
    QString errorString() const override { return QStringLiteral("injected-device-error"); }
};

static void engineTests()
{
    {
        auto value = input(QStringLiteral("Ω:完整\n"), 100001);
        value.fileMode = CompleteExportFileMode::Binary;
        Device device;
        auto result = RunCompleteExportFile(value, QStringLiteral("fake"), device);
        check(result.receipt.succeeded() && result.receipt.rows == 100001, "complete rows exceed all legacy caps");
        check(device.data == QStringLiteral("Ω:完整\n").repeated(100001).toUtf8(), "short positive writes retain all UTF8 bytes");
        check(result.receipt.utf8Bytes == static_cast<std::uint64_t>(device.data.size()), "logical bytes exact");
        check(device.mode == CompleteExportFileMode::Binary && device.commitCalls == 1 && !device.cancelCalls, "binary atomic commit exactly once");
        check(result.receipt.sourceIdentity == value.sourceIdentity && result.receipt.sourceVersion == 42
            && result.receipt.expectedRows == 100001, "receipt preserves complete source identity");
        value.fileMode = CompleteExportFileMode::NativeText; value.utf8Bom = true;
        Device bom; auto withBom = RunCompleteExportFile(value, QStringLiteral("fake"), bom);
        check(withBom.receipt.succeeded() && bom.mode == CompleteExportFileMode::NativeText
            && bom.data == QByteArray("\xef\xbb\xbf") + device.data, "native Text and BOM are explicit per format");
        auto copy = RunCompleteExportCopy(value);
        check(copy.receipt.phase == CompleteExportPhase::ClipboardReady && !copy.receipt.terminal()
            && copy.clipboardText && *copy.clipboardText == QString::fromUtf8(device.data), "complete clipboard text worker value has no BOM");
    }
    {
        QString longText(32767, QChar('x'));
        longText += QString::fromUcs4(U"😀"); longText += QString(70001, QChar(0x4e2d));
        Device device; auto result = RunCompleteExportFile(input(longText), QStringLiteral("fake"), device);
        check(result.receipt.succeeded() && device.data == longText.toUtf8(), "UTF16 chunk boundary preserves surrogate pair");
    }
    for (const qint64 limit : { qint64(0), qint64(-1), std::numeric_limits<qint64>::max() })
    {
        Device device; device.limit = limit;
        auto result = RunCompleteExportFile(input(), QStringLiteral("fake"), device);
        check(result.receipt.phase == CompleteExportPhase::Failed && result.receipt.error == CompleteExportError::WriteFailed
            && device.cancelCalls == 1 && !device.commitCalls, "zero negative overreported write cancels temp without commit");
    }
    for (int stage = 0; stage < 3; ++stage)
    {
        Device device;
        if (stage == 0) device.openResult = false;
        if (stage == 1) device.flushResult = false;
        if (stage == 2) device.commitResult = false;
        auto result = RunCompleteExportFile(input(), QStringLiteral("fake"), device);
        const auto expected = stage == 0 ? CompleteExportError::OpenFailed
            : stage == 1 ? CompleteExportError::FlushFailed : CompleteExportError::CommitFailed;
        check(result.receipt.error == expected && result.receipt.nativeError == device.errorString()
            && device.cancelCalls == 1, "real stage error and native detail retained");
    }
    {
        Device device; auto value = input(); value.serialize = {};
        check(RunCompleteExportFile(value, QStringLiteral("fake"), device).receipt.error == CompleteExportError::InvalidInput
            && !device.openCalls, "empty formatter rejected before open");
        check(RunCompleteExportFile(input(), {}, device).receipt.error == CompleteExportError::InvalidInput, "empty path rejected");
    }
    {
        auto value = input(); value.serialize = [](CompleteExportSink&, const auto&) { return false; };
        Device device; check(RunCompleteExportFile(value, QStringLiteral("fake"), device).receipt.error == CompleteExportError::FormatFailed
            && !device.commitCalls, "formatter refusal does not commit prefix");
        value.serialize = [](CompleteExportSink&, const auto&) -> bool { throw std::runtime_error("formatter-injected"); };
        Device thrown; auto result = RunCompleteExportFile(value, QStringLiteral("fake"), thrown);
        check(result.receipt.error == CompleteExportError::UnexpectedFailure && result.receipt.nativeError == QStringLiteral("formatter-injected")
            && thrown.cancelCalls == 1 && !thrown.commitCalls, "formatter exception cancels atomic output");
        value = input(); value.expectedRows = 2;
        Device missing; check(RunCompleteExportFile(value, QStringLiteral("fake"), missing).receipt.error == CompleteExportError::IncompleteRows
            && !missing.commitCalls, "omitted row cannot report complete success");
        value.expectedRows = 0;
        Device extra; check(RunCompleteExportFile(value, QStringLiteral("fake"), extra).receipt.error == CompleteExportError::IncompleteRows
            && !extra.commitCalls, "extra row invalidates explicit complete scope");
    }
}

static void operationTests()
{
    auto journal = std::make_shared<CompleteExportJournal>();
    auto* pool = QThreadPool::globalInstance();
    {
        CompleteExportOperation operation(nullptr, journal);
        int completed = 0; QString published;
        auto id = operation.startCopy(input(), [&](const QString& text) { published = text; return true; },
            [&](const auto& receipt) { check(receipt.succeeded(), "copy truthful publication receipt"); ++completed; });
        check(id != 0 && operation.isBusy(), "copy accepted with distinct receipt");
        pump([&] { return !operation.isBusy(); });
        check(completed == 1 && published == QStringLiteral("whole\t文档\n"), "GUI publishes complete value once");
        check(journal->receipt(id)->phase == CompleteExportPhase::ClipboardPublished && journal->acknowledge(id)
            && !journal->receipt(id), "terminal metadata acknowledged explicitly");
    }
    {
        auto file = fakeFile(QStringLiteral("native"));
        CompleteExportOperation operation(nullptr, journal); int completed = 0;
        auto id = operation.startFile(QStringLiteral("native"), input(), [&](const auto& receipt)
        { check(receipt.succeeded(), "file completion reports commit"); ++completed; });
        pool->waitForDone();
        check(journal->receipt(id)->phase == CompleteExportPhase::FileCommitted && completed == 0,
            "independent journal committed before any Qt delivery");
        check(file->data == QStringLiteral("whole\t文档\r\n").toUtf8() && file->committed && !file->directFallback
            && file->workerThread != std::this_thread::get_id(), "real controller creates atomic Text device on worker only");
        auto* receiver = operation.children().front();
        QCoreApplication::removePostedEvents(receiver, QEvent::MetaCall);
        operation.reconcile();
        check(completed == 1 && !operation.isBusy() && journal->receipt(id)->succeeded(),
            "drop recovers truthful committed receipt without duplicate side effect");
        pump([&] { return !operation.isBusy(); });
        check(file->commitCalls == 1 && completed == 1, "late idle recovery never repeats old commit or callback");
    }
    {
        QSemaphore entered, release;
        pool->start(QRunnable::create([&] { entered.release(); release.acquire(); }));
        check(entered.tryAcquire(1, 5000), "blocked pool admitted");
        auto operation = std::make_unique<CompleteExportOperation>(nullptr, journal);
        auto file = fakeFile(QStringLiteral("closed"));
        int completed = 0;
        operation->setStateChangedCallback([&] { operation.reset(); });
        auto root = std::make_shared<const PublishedValues>(QStringLiteral("published-root"));
        std::weak_ptr<const PublishedValues> lifetime = root;
        auto value = MakeCompleteExportInput(root, QStringLiteral("fake/published"), 77, 1,
            ks::i18n::SourceTextSnapshot{}, [](const PublishedValues& snapshot, CompleteExportSink& sink, const auto&)
            { return sink.writeText(snapshot.text) && sink.rowsCompleted(); });
        root.reset();
        auto id = operation->startFile(QStringLiteral("closed"), std::move(value), [&](const auto&) { ++completed; });
        check(id != 0 && !operation && !journal->receipt(id)->terminal() && !journal->acknowledge(id),
            "accepted write survives synchronous owner destruction and cannot evict pending journal");
        check(!lifetime.expired(), "O1 noncopyable published root retained by queued accepted work");
        release.release(); pool->waitForDone(); QCoreApplication::processEvents();
        check(file->committed && journal->receipt(id)->succeeded() && completed == 0,
            "closed owner receives no UI callback but committed receipt remains independently");
        check(AsyncCleanupRegistry::global().pending() == 0, "accepted cleanup exit lease drains only after work");
        check(lifetime.expired(), "journal retains metadata without retaining full source root");
    }
    {
        CompleteExportOperation operation(nullptr, journal);
        auto file = fakeFile(QStringLiteral("binary")); auto value = input(); value.fileMode = CompleteExportFileMode::Binary;
        auto id = operation.startFile(QStringLiteral("binary"), std::move(value), {});
        check(operation.startCopy(input(), [](const auto&) { return true; }, {}) == 0, "busy request cannot replace accepted file");
        pump([&] { return !operation.isBusy(); });
        check(file->data == QStringLiteral("whole\t文档\n").toUtf8() && journal->receipt(id)->succeeded(), "binary JSON mode has no Text conversion");
    }
    {
        CompleteExportOperation operation(nullptr, journal); int published = 0, completed = 0;
        auto id = operation.startCopy(input(), [&](const auto&) { ++published; return true; },
            [&](const auto& receipt) { check(receipt.phase == CompleteExportPhase::ClipboardNotPublished, "copy drop never claims publication"); ++completed; });
        pool->waitForDone();
        check(journal->receipt(id)->phase == CompleteExportPhase::ClipboardReady, "worker ready is distinct from GUI publication");
        auto* receiver = operation.children().front();
        QCoreApplication::removePostedEvents(receiver, QEvent::MetaCall); operation.reconcile();
        check(published == 0 && completed == 1 && !operation.isBusy(), "dropped copy has one truthful not-published completion");
    }
    {
        CompleteExportOperation operation(nullptr, journal); int completed = 0;
        auto first = operation.startCopy(input(), [](const auto&) { return true; }, [&](const auto&) { ++completed; });
        pool->waitForDone();
        QCoreApplication::removePostedEvents(operation.children().front(), QEvent::MetaCall);
        // Leave the old queued idle recovery posted while a new accepted job owns the slot.
        QSemaphore entered, release;
        pool->start(QRunnable::create([&] { entered.release(); release.acquire(); }));
        check(entered.tryAcquire(1, 5000), "late idle blocker entered");
        auto second = operation.startCopy(input(QStringLiteral("new")), [](const auto&) { return true; }, [&](const auto&) { ++completed; });
        QCoreApplication::processEvents();
        const bool preserved = second != 0 && second != first && operation.isBusy()
            && operation.currentReceipt()->id == second && completed == 1;
        release.release();
        check(preserved, "late old recovery cannot clear new receipt identity or physical slot");
        pump([&] { return !operation.isBusy(); });
        check(completed == 2 && journal->receipt(second)->succeeded(), "new accepted copy completes exactly once after old drop");
    }
    {
        auto operation = std::make_unique<CompleteExportOperation>(nullptr, journal);
        auto id = operation->startCopy(input(), [&](const auto&) { operation.reset(); return true; }, {});
        pump([&] { return !operation; });
        check(journal->receipt(id)->phase == CompleteExportPhase::ClipboardPublished, "publisher deletes owner after success without losing truth");
    }
    {
        CompleteExportOperation operation(nullptr, journal); int completed = 0; std::uint64_t second = 0;
        auto first = operation.startCopy(input(), [](const auto&) { return false; }, [&](const auto& receipt)
        {
            check(receipt.error == CompleteExportError::ClipboardUnavailable, "failed GUI publication recorded"); ++completed;
            second = operation.startCopy(input(QStringLiteral("second")), [](const auto&) { return true; }, [&](const auto&) { ++completed; });
        });
        pump([&] { return completed == 2; });
        check(second != 0 && second != first && !operation.isBusy() && journal->receipt(second)->succeeded(),
            "reentrant completion starts new identity after clearing old active state");
    }
    {
        CompleteExportOperation operation(nullptr, journal); int completed = 0;
        auto& registry = AsyncCleanupRegistry::global(); registry.beginShutdown();
        auto id = operation.startFile(QStringLiteral("rejected"), input(), [&](const auto&) { ++completed; });
        check(id == 0 && completed == 0 && !operation.isBusy(), "shutdown admission zero has no fabricated completion");
        registry.cancelShutdown(); operation.close();
        check(operation.startCopy(input(), [](const auto&) { return true; }, {}) == 0, "closed operation rejects without writer");
    }
    {
        CompleteExportOperation operation(nullptr, journal); int completed = 0;
        throwFakeFileConstruction.store(true);
        auto id = operation.startFile(QStringLiteral("allocation-fault"), input(), [&](const auto& receipt)
        {
            check(receipt.phase == CompleteExportPhase::OutcomeUnavailable
                && receipt.error == CompleteExportError::ResultUnavailable, "worker exception outside engine is uncertainty, never device failure");
            ++completed;
        });
        pump([&] { return !operation.isBusy(); });
        check(id != 0 && completed == 1 && journal->receipt(id)->terminal(), "unexpected accepted result still settles independent receipt");
    }
    {
        CompleteExportOperation operation(nullptr, journal);
        auto value = MakeCompleteExportInput(std::shared_ptr<const Values>{}, {}, 0, 0,
            ks::i18n::SourceTextSnapshot{}, [](const Values&, CompleteExportSink&, const auto&) { return true; });
        check(operation.startCopy(std::move(value), [](const auto&) { return true; }, {}) == 0, "missing published root rejected before admission");
    }
    pool->waitForDone(); QCoreApplication::processEvents();
    for (const auto& receipt : journal->receipts()) check(receipt.terminal(), "all admitted journal records settle independently");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        auto* pool = QThreadPool::globalInstance(); const int threads = pool->maxThreadCount(); pool->setMaxThreadCount(1);
        engineTests(); operationTests(); pool->setMaxThreadCount(threads);
        std::cout << "COMPLETE_EXPORT_CHECKS=" << checks << "\nGUI_WIDGETS_CREATED=0\nREAL_FILE_WRITES=0\nBUSINESS_IO_CALLS=0\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
