#pragma once

#include "MonitorEventFilterProjection.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/StructuredFieldView.h"
#include "UI/TableSearchFilterSnapshot.h"

#include <QCollator>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_set>
#include <vector>

namespace ks::monitor
{
    // Value-only accepted event. The complete field document is the sole source
    // for details and full exports; the display/search excerpt keeps its old limit.
    struct ProcessTraceCapturedEventRow
    {
        std::uint64_t time100ns = 0;
        QString time100nsText, typeText, providerText;
        int eventId = 0;
        QString eventName, pidText, processText, rootPidText, relationText;
        ks::ui::FieldDocument detailDocument;
        QString activityIdText;
    };

    struct ProcessTraceEventRecord final
    {
        const ProcessTraceCapturedEventRow value;
        const std::uint64_t sequence;
        struct SearchCache { QString detailText, processText, eventText, globalText; };

        ProcessTraceEventRecord(ProcessTraceCapturedEventRow row, std::uint64_t sequence)
            : value(std::move(row)), sequence(sequence) {}

        // The GUI binds its immutable language value before canonical publication.
        // ETW only constructs/queues values; it never queries a language QObject.
        void prepareForArchive(const ks::i18n::SourceTextSnapshot& language) const
        { std::call_once(m_languageOnce, [this, language]() { m_language = language; }); }

        // Acceptance is only a value move. Hidden flushes do not render a full
        // document or construct search/display strings. The first visible batch
        // or export worker computes the original excerpt once from frozen values.
        const SearchCache& searchCache() const
        {
            std::call_once(m_languageOnce, []() {});
            std::call_once(m_cacheOnce, [this]() {
                m_cache.detailText = value.detailDocument.toPlainText([this](const QString& source) {
                    return m_language.sourceText(source);
                }).simplified().left(6000);
                m_cache.processText = QStringLiteral("%1 | %2 | %3 | %4")
                    .arg(value.pidText, value.processText, value.rootPidText, value.relationText);
                m_cache.eventText = QStringLiteral("%1 | %2").arg(value.eventName, QString::number(value.eventId));
                m_cache.globalText = QStringLiteral("%1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10")
                    .arg(value.typeText, value.providerText, QString::number(value.eventId), value.eventName,
                        value.pidText, value.processText, value.rootPidText, value.relationText,
                        m_cache.detailText, value.activityIdText);
            });
            return m_cache;
        }
        QStringList rawCells() const
        {
            return {value.time100nsText, value.typeText, value.providerText, QString::number(value.eventId),
                value.eventName, value.pidText, value.processText, value.rootPidText, value.relationText,
                searchCache().detailText, value.activityIdText};
        }
    private:
        mutable ks::i18n::SourceTextSnapshot m_language;
        mutable std::once_flag m_languageOnce;
        mutable std::once_flag m_cacheOnce;
        mutable SearchCache m_cache;
    };
    using ProcessTraceEventRef = std::shared_ptr<const ProcessTraceEventRecord>;

    // GUI-owned canonical retention is independent of visibility, menus, sorting
    // and the display projection. References frozen before eviction remain valid.
    class ProcessTraceEventArchive final
    {
    public:
        static constexpr std::size_t capacity = 12000;
        ProcessTraceEventRef append(ProcessTraceEventRef row)
        {
            if (!row) return {};
            const auto [present, inserted] = m_present.insert(row.get());
            if (!inserted) return {};
            try { m_rows.push_back(row); }
            catch (...) { m_present.erase(present); throw; }
            ++m_revision;
            if (m_rows.size() <= capacity) return {};
            auto evicted = std::move(m_rows.front()); m_rows.pop_front();
            m_present.erase(evicted.get()); ++m_evicted;
            return evicted;
        }
        void clear() { m_rows.clear(); m_present.clear(); m_evicted = 0; ++m_revision; }
        bool contains(const ProcessTraceEventRef& row) const { return row && m_present.contains(row.get()); }
        std::size_t size() const { return m_rows.size(); }
        std::uint64_t revision() const { return m_revision; }
        std::uint64_t evicted() const { return m_evicted; }
        std::vector<ProcessTraceEventRef> snapshot() const { return {m_rows.begin(), m_rows.end()}; }
    private:
        std::deque<ProcessTraceEventRef> m_rows;
        std::unordered_set<const ProcessTraceEventRecord*> m_present;
        std::uint64_t m_revision = 0, m_evicted = 0;
    };

    // Freeze contains candidate records, never a GUI-computed 12k regex result.
    // Worker users evaluate both business conditions and the shared search value.
    struct ProcessTraceEventExportSnapshot
    {
        std::vector<ProcessTraceEventRef> rows;
        std::shared_ptr<const CompiledEventFilter> businessFilter;
        ks::ui::TableSearchResultFilterSpec search;
        ks::i18n::SourceTextSnapshot language;
        QStringList headers;
        std::uint64_t sessionGeneration = 0, archiveRevision = 0, pendingDroppedRows = 0, evictedRows = 0;
        bool continuousSort = false;
        int sortColumn = -1;
        Qt::SortOrder sortOrder = Qt::AscendingOrder;
        QCollator collator;

        QStringList displayedCells(const ProcessTraceEventRef& record) const
        {
            if (!record) return {};
            auto cells = record->rawCells();
            for (auto& cell : cells) cell = language.displayText(cell);
            return cells;
        }
        bool matches(const ProcessTraceEventRef& record) const
        {
            if (!record) return false;
            const auto cells = displayedCells(record);
            EventFilterRow filterRow;
            // These are the exact legacy resolver roles: translated visible
            // provider/type/detail, original process/event/global UserRole caches.
            filterRow.fields = {cells.value(2), record->searchCache().processText, record->searchCache().eventText,
                cells.value(9), record->searchCache().globalText};
            filterRow.type = cells.value(1); filterRow.time100ns = record->value.time100ns;
            return (!businessFilter || businessFilter->matches(filterRow))
                && ks::ui::TableSearchResultFilterMatches(search, cells);
        }
        std::vector<ProcessTraceEventRef> orderedRows() const
        {
            if (!continuousSort || sortColumn < 0 || sortColumn >= 11) return rows;
            std::vector<std::pair<ProcessTraceEventRef, QString>> keyed;
            keyed.reserve(rows.size());
            for (const auto& row : rows) keyed.emplace_back(row, displayedCells(row).value(sortColumn));
            std::stable_sort(keyed.begin(), keyed.end(), [this](const auto& left, const auto& right) {
                const int order = collator.compare(left.second, right.second);
                return sortOrder == Qt::AscendingOrder ? order < 0 : order > 0;
            });
            std::vector<ProcessTraceEventRef> ordered; ordered.reserve(keyed.size());
            for (auto& [row, key] : keyed) ordered.push_back(std::move(row));
            return ordered;
        }
    };
}
