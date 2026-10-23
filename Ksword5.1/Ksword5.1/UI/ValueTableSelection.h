#pragma once

#include <QSet>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <utility>

namespace ks::ui
{
    // Qt range deltas are captured on GUI without enumerating their rows. Resolve
    // against immutable projected canonical keys on a worker before filtering.
    class ValueTableSelection
    {
    public:
        struct Range { int first = 0; int last = -1; };
        using Keys = std::shared_ptr<const QVector<QString>>;
        using Selected = std::shared_ptr<const QSet<QString>>;
        struct Delta { Keys keys; QVector<Range> selected, deselected; };

        void append(Keys keys, QVector<Range> selected, QVector<Range> deselected)
        { m_pending.push_back({std::move(keys), std::move(selected), std::move(deselected)}); }
        void setResolved(Selected selected) { m_selected = std::move(selected); m_pending.clear(); }

        // Null means obsolete; an empty non-null set is a complete empty selection.
        Selected resolve(const std::function<bool()>& cancelled) const
        {
            auto selected = std::make_shared<QSet<QString>>(m_selected ? *m_selected : QSet<QString>{});
            int work = 0;
            for (const auto& delta : m_pending)
            {
                if (!delta.keys) continue;
                const auto apply = [&](const QVector<Range>& ranges, bool select) {
                    for (const auto& range : ranges)
                        for (int row = qMax(0, range.first); row <= range.last && row < delta.keys->size(); ++row)
                        {
                            if ((++work & 127) == 0 && cancelled()) return false;
                            if (select) selected->insert((*delta.keys)[row]);
                            else selected->remove((*delta.keys)[row]);
                        }
                    return true;
                };
                if (!apply(delta.deselected, false) || !apply(delta.selected, true)) return {};
            }
            return cancelled() ? Selected{} : selected;
        }
    private:
        Selected m_selected;
        QVector<Delta> m_pending;
    };
}
