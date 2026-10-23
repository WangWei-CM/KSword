#include <QCoreApplication>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include "MonitorDock/ProcessTraceEventArchive.h"

// Only the immutable language value transport is faked. Archive, record cache,
// business regex/time/invert and shared search matcher are production methods.
static std::atomic<int> translatedSources{0};
namespace ks::i18n
{
    QString SourceTextSnapshot::sourceText(const QString& source) const { ++translatedSources; return source; }
    QString SourceTextSnapshot::displayText(const QString& source) const
    {
        if (source == QStringLiteral("ProviderRaw")) return QStringLiteral("Provider display");
        if (source == QStringLiteral("进程")) return QStringLiteral("Process display");
        if (source == QStringLiteral("DetailOnly")) return QStringLiteral("Detail display");
        return source;
    }
    LanguageManager& LanguageManager::instance() { std::abort(); }
    QString LanguageManager::sourceText(const QString& source) const { return source; }
}

static std::atomic<int> assertions{0};
#define CHECK(x) do { ++assertions; if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
using namespace ks::monitor;
static ProcessTraceEventRef makeRow(std::uint64_t sequence, QString name = QStringLiteral("evt"))
{
    ProcessTraceCapturedEventRow value;
    value.time100ns = sequence; value.time100nsText = QString::number(sequence);
    value.typeText = QStringLiteral("进程"); value.providerText = QStringLiteral("ProviderRaw"); value.eventId = 7;
    value.eventName = std::move(name); value.pidText = QStringLiteral("11 / 12");
    value.processText = QStringLiteral("RawProcess"); value.rootPidText = QStringLiteral("11");
    value.relationText = QStringLiteral("root"); value.detailDocument.title = QStringLiteral("DetailOnly");
    auto row = std::make_shared<const ProcessTraceEventRecord>(std::move(value), sequence);
    row->prepareForArchive({});
    return row;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessTraceEventArchive archive;
    std::vector<ProcessTraceEventRef> held;
    // A hidden/menu-blocked producer does no display or full-document rendering.
    for (std::uint64_t i = 1; i <= 30000; ++i)
    {
        auto row = makeRow(i);
        if (i <= 3) held.push_back(row);
        const auto expired = archive.append(row);
        CHECK(bool(expired) == (i > ProcessTraceEventArchive::capacity));
    }
    CHECK(translatedSources == 0);
    CHECK(archive.size() == 12000); CHECK(archive.evicted() == 18000);
    auto retained = archive.snapshot();
    CHECK(retained.front()->sequence == 18001); CHECK(retained.back()->sequence == 30000);
    for (std::size_t i = 1; i < retained.size(); ++i) CHECK(retained[i]->sequence == retained[i-1]->sequence + 1);
    const auto revision = archive.revision(); archive.append(retained.back()); CHECK(archive.revision() == revision);
    CHECK(!archive.contains(held.front())); CHECK(held.front()->value.eventName == QStringLiteral("evt"));

    ProcessTraceEventExportSnapshot frozen;
    frozen.rows = retained; frozen.sessionGeneration = 19; frozen.pendingDroppedRows = 37; frozen.evictedRows = archive.evicted();
    archive.clear(); archive.append(makeRow(1, QStringLiteral("new session")));
    CHECK(frozen.rows.size() == 12000); CHECK(frozen.rows.front()->sequence == 18001);
    CHECK(frozen.sessionGeneration == 19); CHECK(frozen.pendingDroppedRows == 37); CHECK(frozen.evictedRows == 18000);

    auto sample = makeRow(20);
    EventFilterSpec spec;
    spec.patterns = {QStringLiteral("^Provider display$"), QStringLiteral("RawProcess"), QStringLiteral("evt.*7"),
        QStringLiteral("^Detail display$"), QStringLiteral("进程.*ProviderRaw.*DetailOnly")};
    spec.fieldIndexes = {0,1,2,3,4}; spec.regex = true; spec.type = QStringLiteral("Process display");
    spec.timeline = true; spec.start100ns = 20; spec.end100ns = 20;
    frozen.businessFilter = std::make_shared<const CompiledEventFilter>(spec);
    frozen.search.active = true; frozen.search.query = QStringLiteral("DISPLAY"); frozen.search.columns = {2};
    CHECK(frozen.matches(sample));
    CHECK(frozen.displayedCells(sample).value(2) == QStringLiteral("Provider display"));
    CHECK(sample->searchCache().globalText.contains(QStringLiteral("ProviderRaw")));
    CHECK(!sample->searchCache().globalText.contains(QStringLiteral("Provider display")));
    const int renderedOnce = translatedSources.load();
    std::vector<std::thread> workers;
    for (int n = 0; n < 8; ++n) workers.emplace_back([&]() { for (int i = 0; i < 500; ++i) CHECK(frozen.matches(sample)); });
    for (auto& worker : workers) worker.join();
    CHECK(translatedSources == renderedOnce); CHECK(frozen.businessFilter->compilationCount == 5);
    frozen.search.columns = {99, -1}; CHECK(!frozen.matches(sample));
    frozen.search.active = false; CHECK(frozen.matches(sample));
    CHECK(!frozen.matches(makeRow(21))); // Range is inclusive and outside inversion.
    spec.patterns = {QStringLiteral("missing")}; spec.fieldIndexes = {0}; spec.invert = true;
    frozen.businessFilter = std::make_shared<const CompiledEventFilter>(spec);
    CHECK(frozen.matches(sample)); CHECK(!frozen.matches(makeRow(0))); CHECK(!frozen.matches(makeRow(21)));
    spec.patterns = {QStringLiteral("[")}; spec.invert = false;
    frozen.businessFilter = std::make_shared<const CompiledEventFilter>(spec); CHECK(!frozen.matches(sample));

    ProcessTraceCapturedEventRow full;
    full.detailDocument.field(QStringLiteral("Payload"), QString(7000, QLatin1Char('x')));
    auto fullRecord = std::make_shared<const ProcessTraceEventRecord>(std::move(full), 99); fullRecord->prepareForArchive({});
    CHECK(fullRecord->searchCache().detailText.size() == 6000);
    CHECK(fullRecord->value.detailDocument.toPlainText(false).size() > 7000);
    CHECK(fullRecord->value.detailDocument.nodes.front().value.size() == 7000);
    frozen.rows = {makeRow(2, QStringLiteral("b")), makeRow(3, QStringLiteral("a")), makeRow(4, QStringLiteral("a"))};
    frozen.continuousSort = true; frozen.sortColumn = 4;
    const auto sorted = frozen.orderedRows(); CHECK(sorted[0]->sequence == 3); CHECK(sorted[1]->sequence == 4); CHECK(sorted[2]->sequence == 2);
    frozen.sortOrder = Qt::DescendingOrder; CHECK(frozen.orderedRows()[0]->sequence == 2);
    frozen.continuousSort = false; CHECK(frozen.orderedRows()[0]->sequence == 2);
    std::cout << "PASS " << assertions << " production archive/value assertions; 30000 events, 12000 retention, no hidden render, frozen complete documents\n";
}
