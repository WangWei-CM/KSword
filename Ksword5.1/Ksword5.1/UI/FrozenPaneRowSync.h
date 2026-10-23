#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QVector>
#include <algorithm>

namespace ks::ui
{
    // Auxiliary views keep the real model and selection model. Only their row
    // section state is projected here; initial/reset work and inserted ranges
    // are budgeted, while ordinary source changes touch explicit dirty rows.
    class FrozenPaneRowSync final
    {
    public:
        struct Row { bool hidden = false; int height = 0; };
        enum class Mode { SourceRows, FrozenRows };
        explicit FrozenPaneRowSync(Mode mode) : m_mode(mode) {}

        void reset(int rowCount)
        {
            m_rowCount = std::max(0, rowCount);
            m_spans.clear(); m_dirty.clear(); m_frozen.clear();
            queueSpan(0, m_rowCount - 1);
        }
        // Qt headers retain existing section state on a model reset and adjust
        // only the tail when the count changes. Retain our unfinished work too;
        // recurring same-size resets must not restart initial projection at zero.
        void resizeRowCount(int rowCount)
        {
            rowCount = std::max(0,rowCount);
            if (rowCount > m_rowCount) rowsInserted(m_rowCount,rowCount-m_rowCount);
            else if (rowCount < m_rowCount) rowsRemoved(rowCount,m_rowCount-rowCount);
        }
        // An arbitrary layout permutation cannot be inferred from row intervals.
        // Settled source/pane headers already share identical model updates; only
        // an unfinished mapping needs a new budgeted reconciliation pass.
        void layoutChanged()
        {
            if (pending()) queueSpan(0,m_rowCount-1);
        }
        bool pending() const { return !m_spans.isEmpty() || !m_dirty.isEmpty(); }
        void dirtyRow(int row) { if (row >= 0 && row < m_rowCount) m_dirty.insert(row); }
        void sourceRowChanged(int row)
        {
            // Unrelated source filter/height changes cannot affect a top pane
            // whose only visible rows are its small explicit frozen set.
            if (m_mode == Mode::SourceRows || m_frozen.contains(row)) dirtyRow(row);
        }
        void setFrozenRows(const QHash<int, int>& rows)
        {
            if (rows == m_frozen) return;
            for (auto it = m_frozen.cbegin(); it != m_frozen.cend(); ++it) dirtyRow(it.key());
            for (auto it = rows.cbegin(); it != rows.cend(); ++it) dirtyRow(it.key());
            m_frozen = rows;
        }
        void rowsInserted(int first, int count)
        {
            if (count <= 0) return;
            m_rowCount += count;
            for (auto& span : m_spans)
            {
                if (first <= span.first) { span.first += count; span.last += count; }
                else if (first <= span.last) span.last += count;
            }
            QSet<int> dirty;
            for (int row : m_dirty) dirty.insert(row >= first ? row + count : row);
            m_dirty = std::move(dirty);
            QHash<int,int> frozen;
            for (auto it = m_frozen.cbegin(); it != m_frozen.cend(); ++it)
                frozen.insert(it.key() >= first ? it.key() + count : it.key(), it.value());
            m_frozen = std::move(frozen);
            queueSpan(first, first + count - 1);
        }
        void rowsRemoved(int first, int count)
        {
            if (count <= 0) return;
            const int last = first + count - 1;
            m_rowCount = std::max(0, m_rowCount - count);
            QVector<Span> previous = std::move(m_spans);
            m_spans.clear();
            for (const auto& span : previous)
            {
                queueSpan(span.first, std::min(span.last, first - 1));
                queueSpan(std::max(span.first, last + 1) - count, span.last - count);
            }
            QSet<int> dirty;
            for (int row : m_dirty)
                if (row < first || row > last) dirty.insert(row > last ? row - count : row);
            m_dirty = std::move(dirty);
            QHash<int,int> frozen;
            for (auto it = m_frozen.cbegin(); it != m_frozen.cend(); ++it)
                if (it.key() < first || it.key() > last)
                    frozen.insert(it.key() > last ? it.key() - count : it.key(), it.value());
            m_frozen = std::move(frozen);
        }

        template<class Read, class Write>
        int drain(Read&& read, Write&& write, int maxMilliseconds = 4, int maxRows = 256)
        {
            QElapsedTimer budget; budget.start();
            int work = 0;
            while (pending() && work < std::max(1, maxRows)
                && budget.elapsed() < std::max(1, maxMilliseconds))
            {
                int row = -1;
                if (!m_spans.isEmpty())
                {
                    row = m_spans.first().first++;
                    if (m_spans.first().first > m_spans.first().last) m_spans.removeFirst();
                    m_dirty.remove(row);
                }
                else
                {
                    auto it = m_dirty.begin(); row = *it; m_dirty.erase(it);
                }
                if (row < 0 || row >= m_rowCount) continue;
                ++work;
                const auto frozen = m_frozen.constFind(row);
                const Row value = m_mode == Mode::SourceRows ? read(row)
                    : frozen == m_frozen.cend() ? Row{true,0} : Row{false,frozen.value()};
                if (!write(row, value)) break;
            }
            return work;
        }

    private:
        struct Span { int first = 0, last = -1; };
        void queueSpan(int first, int last)
        {
            first = std::max(0, first); last = std::min(m_rowCount - 1, last);
            if (first > last) return;
            m_spans.push_back({first,last});
            std::sort(m_spans.begin(), m_spans.end(), [](const Span& a,const Span& b) { return a.first < b.first; });
            QVector<Span> merged;
            for (const auto& span : m_spans)
            {
                if (!merged.isEmpty() && span.first <= merged.last().last + 1)
                    merged.last().last = std::max(merged.last().last, span.last);
                else merged.push_back(span);
            }
            m_spans = std::move(merged);
        }
        Mode m_mode;
        int m_rowCount = 0;
        QVector<Span> m_spans;
        QSet<int> m_dirty;
        QHash<int,int> m_frozen;
    };

    // Iterate actual viewport row identities. A blank lower viewport terminates
    // immediately; no logical-number interval or model-end fallback is scanned.
    template<class RowAt, class Position, class Height, class Visit>
    void ForEachViewportTableRow(int viewportHeight, RowAt&& rowAt,
        Position&& position, Height&& height, Visit&& visit)
    {
        int y = 0;
        while (y < viewportHeight)
        {
            const int row = rowAt(y);
            if (row < 0) break;
            visit(row);
            const qint64 next = qint64(position(row)) + std::max(1, height(row));
            y = static_cast<int>(std::min<qint64>(viewportHeight, std::max<qint64>(qint64(y) + 1, next)));
        }
    }
}
