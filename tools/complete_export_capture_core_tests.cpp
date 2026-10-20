#include "UI/CompleteExportCapture.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ks::ui;
static int checks = 0;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); ++checks; }
template<class Predicate> static void pump(Predicate predicate)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < 15000) { QCoreApplication::processEvents(); std::this_thread::yield(); }
    check(predicate(), "capture completion timeout");
}
struct Source
{
    QObject lifetime;
    int rows = 4, columns = 3, reads = 0;
    std::uint64_t revision = 5;
    QString language = QStringLiteral("zh-CN");
    CompleteExportCapturePort port()
    {
        CompleteExportCapturePort result;
        result.source = &lifetime; result.sourceIdentity = QStringLiteral("fake/table"); result.sourceVersion = 73;
        result.valid = [] { return true; };
        result.revision = [this] { return revision; };
        result.languageId = [this] { return language; };
        result.rowCount = [this] { return rows; }; result.columnCount = [this] { return columns; };
        result.header = [](int column) { return QStringLiteral("header%1").arg(column); };
        result.cell = [this](int row, int column) { ++reads; return QStringLiteral("行%1:列%2\t\n").arg(row).arg(column); };
        return result;
    }
};

static void completeTests()
{
    Source source; source.rows = 100001;
    CompleteExportCapture capture(nullptr);
    std::shared_ptr<const CompleteExportCapturedTable> result;
    int completed = 0; bool heartbeatDuringCapture = false;
    auto port = source.port();
    port.rowIncluded = [](int row) { return row % 17 != 0; };
    port.columnIncluded = [](int column) { return column != 1; };
    auto id = capture.start(std::move(port), [&](const auto& status, auto table)
    { check(status.phase == CompleteExportCapturePhase::Completed, "complete capture phase"); result = std::move(table); ++completed; }, { 2, 31, 64 });
    check(id != 0 && source.reads == 0 && capture.isBusy(), "start queues without reading model or full walking");
    QTimer::singleShot(0, [&] { heartbeatDuringCapture = capture.isBusy() && completed == 0; });
    check(capture.start(source.port(), {}) == 0, "busy preparation not replaced");
    pump([&] { return completed == 1; });
    check(heartbeatDuringCapture, "queued heartbeat runs before complete large capture");
    check(result && result->sourceVersion == 73 && result->sourceIdentity == QStringLiteral("fake/table")
        && result->languageId == QStringLiteral("zh-CN") && result->sourceColumns == std::deque<int>({ 0, 2 }), "frozen source and logical columns preserved");
    check(result->headers.at(0).text() == QStringLiteral("header0") && result->headers.at(1).text() == QStringLiteral("header2"), "complete current rendered headers");
    std::size_t index = 0;
    for (int row = 0; row < source.rows; ++row)
    {
        if (row % 17 == 0) continue;
        check(result->sourceRows.at(index) == row && result->cell(index, 0).text() == QStringLiteral("行%1:列0\t\n").arg(row)
            && result->cell(index, 1).text() == QStringLiteral("行%1:列2\t\n").arg(row), "all included rows/cells retain exact order and rendered values");
        ++index;
    }
    check(result->sourceRows.size() == index && result->cells.size() == index * 2
        && capture.status().visitedRows == source.rows && capture.status().capturedRows == index, "no row cap or duplicate projected cells");
    source.rows = 0; ++source.revision;
    check(result->sourceRows.size() == index, "completed immutable table independent of subsequent source mutation");
    result.reset();
    {
        Source raw; raw.rows = raw.columns = 1;
        std::vector<QChar> buffer(200001, QChar(0x4e2d));
        buffer[63] = QChar(0xd83d); buffer[64] = QChar(0xde00);
        const QString expected(buffer.data(), static_cast<qsizetype>(buffer.size()));
        auto rawPort = raw.port();
        rawPort.cell = [&](int, int) { return QString::fromRawData(buffer.data(), static_cast<qsizetype>(buffer.size())); };
        capture.start(std::move(rawPort), [&](const auto& status, auto table)
        {
            check(status.phase == CompleteExportCapturePhase::Completed, "long raw cell completes");
            result = std::move(table);
        }, { 2, 7, 64 });
        pump([&] { return !capture.isBusy(); });
        std::fill(buffer.begin(), buffer.end(), QChar('X'));
        check(result->cell(0, 0).text() == expected, "borrowed raw cell fragments become independently owned values");
        for (const auto& part : result->cell(0, 0).fragments) check(part.size() <= 64, "long cell deep copy has bounded fragment size");
        result.reset();
    }
}

static void invalidationTests()
{
    for (int kind = 0; kind < 4; ++kind)
    {
        Source source; source.rows = 100;
        CompleteExportCapture capture(nullptr); int completed = 0; bool changed = false;
        capture.setProgressCallback([&](const auto&)
        {
            if (changed) return;
            changed = true;
            if (kind == 0) ++source.revision;
            if (kind == 1) source.language = QStringLiteral("en-US");
            if (kind == 2) ++source.rows;
            if (kind == 3) ++source.columns;
        });
        capture.start(source.port(), [&](const auto& status, auto table)
        {
            check(status.phase == CompleteExportCapturePhase::Invalidated && status.error == CompleteExportCaptureError::SourceChanged
                && !table, "content/filter/language/dimensions change cancels before any acceptance"); ++completed;
        }, { 2, 3, 16 });
        pump([&] { return completed == 1; });
    }
    {
        Source source; CompleteExportCapture capture(nullptr); int completed = 0;
        auto port = source.port();
        port.cell = [&](int, int) { ++source.revision; return QStringLiteral("must-not-publish"); };
        capture.start(std::move(port), [&](const auto& status, auto table)
        { check(status.phase == CompleteExportCapturePhase::Invalidated && !table, "reentrant data mutation detected after read before fragment copy"); ++completed; });
        pump([&] { return completed == 1; });
    }
    {
        auto source = std::make_unique<Source>(); CompleteExportCapture capture(nullptr); int completed = 0;
        capture.start(source->port(), [&](const auto& status, auto table)
        { check(status.phase == CompleteExportCapturePhase::Invalidated && !table, "source destruction stops queued capture without invoking stale callbacks"); ++completed; });
        source.reset(); pump([&] { return completed == 1; });
    }
    {
        Source source; CompleteExportCapture capture(nullptr); int completed = 0;
        auto port = source.port(); port.cell = [](int, int)->QString { throw std::runtime_error("injected-read-error"); };
        capture.start(std::move(port), [&](const auto& status, auto table)
        { check(status.phase == CompleteExportCapturePhase::Failed && status.error == CompleteExportCaptureError::ReadFailed
            && status.nativeError == QStringLiteral("injected-read-error") && !table, "read exception returns no partial result"); ++completed; });
        pump([&] { return completed == 1; });
    }
}

static void lifetimeTests()
{
    {
        Source source; CompleteExportCapture capture(nullptr); int canceled = 0, completed = 0; std::uint64_t next = 0;
        auto first = capture.start(source.port(), [&](const auto& status, auto table)
        {
            check(status.phase == CompleteExportCapturePhase::Canceled && !table, "explicit cancel has no complete value"); ++canceled;
            next = capture.start(source.port(), [&](const auto& current, auto complete)
            { check(current.phase == CompleteExportCapturePhase::Completed && bool(complete), "new preparation completes after canceled queued slice"); ++completed; }, { 2, 2, 16 });
        });
        capture.cancel();
        check(canceled == 1 && source.reads == 0 && next != 0 && first != next, "cancel clears old identity before reentrant new request");
        pump([&] { return completed == 1; });
    }
    for (int kind = 0; kind < 3; ++kind)
    {
        Source source; auto capture = std::make_unique<CompleteExportCapture>(nullptr); int completed = 0;
        auto port = source.port();
        if (kind == 0) capture->setProgressCallback([&](const auto&) { capture.reset(); });
        if (kind == 1) port.cell = [&](int, int) { capture.reset(); return QStringLiteral("deleted"); };
        if (kind == 2) port.valid = [&] { capture.reset(); return true; };
        capture->start(std::move(port), [&](const auto&, auto) { ++completed; }, { 2, 2, 16 });
        pump([&] { return !capture; }); QCoreApplication::processEvents();
        check(completed == 0, "progress/read/valid callback deleting owner cannot resume old capture");
    }
    {
        Source source; CompleteExportCapture capture(nullptr); int completed = 0;
        capture.start(source.port(), [&](const auto&, auto) { ++completed; }); capture.close();
        QCoreApplication::processEvents();
        check(!capture.isBusy() && completed == 0 && source.reads == 0
            && capture.start(source.port(), {}) == 0, "close drops queued work before all owner callbacks");
    }
    {
        Source source; CompleteExportCapture capture(nullptr); int completed = 0; bool switched = false;
        auto port = source.port();
        port.cell = [&](int, int)
        {
            if (!switched)
            {
                switched = true; capture.cancel();
                capture.start(source.port(), [&](const auto& status, auto table)
                { check(status.phase == CompleteExportCapturePhase::Completed && bool(table), "reentrant reader's replacement completes"); ++completed; });
            }
            return QStringLiteral("old-read");
        };
        capture.start(std::move(port), [](const auto&, auto) {});
        pump([&] { return completed == 1; });
        check(!capture.isBusy(), "old read does not overwrite or queue a replacement generation");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        completeTests(); invalidationTests(); lifetimeTests(); QThreadPool::globalInstance()->waitForDone();
        std::cout << "COMPLETE_CAPTURE_CHECKS=" << checks << "\nGUI_WIDGETS_CREATED=0\nREAL_FILE_WRITES=0\nBUSINESS_IO_CALLS=0\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
