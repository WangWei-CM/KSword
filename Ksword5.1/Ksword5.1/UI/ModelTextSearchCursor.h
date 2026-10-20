#pragma once

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QString>
#include <QVector>
#include <utility>
#include <functional>

namespace ks::ui
{
    // GUI-thread model reads, cooperatively bounded between cells. No widget or
    // business enumeration is involved. Match order follows Qt's default
    // MatchContains | MatchRecursive: one column, depth first, then next column.
    class ModelTextSearchCursor final : public QObject
    {
    public:
        struct Match { QPersistentModelIndex index; QString text; };
        enum class Status { Pending, Complete, Cancelled };

        ModelTextSearchCursor(QAbstractItemModel* model, QString query, QVector<int> columns, int maxHits,
            std::function<bool(const QModelIndex&)> accept = {}, const QModelIndex& root = {})
            : m_model(model), m_query(std::move(query)), m_columns(std::move(columns)), m_maxHits(maxHits), m_accept(std::move(accept)), m_root(root)
        {
            if (model == nullptr) return;
            const auto changed = [this]() { m_restart = true; };
            connect(model, &QAbstractItemModel::modelReset, this, changed);
            connect(model, &QAbstractItemModel::layoutChanged, this, changed);
            connect(model, &QAbstractItemModel::rowsInserted, this, changed);
            connect(model, &QAbstractItemModel::rowsRemoved, this, changed);
            connect(model, &QAbstractItemModel::rowsMoved, this, changed);
            connect(model, &QAbstractItemModel::columnsInserted, this, changed);
            connect(model, &QAbstractItemModel::columnsRemoved, this, changed);
            connect(model, &QAbstractItemModel::columnsMoved, this, changed);
            connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (roles.isEmpty() || roles.contains(Qt::DisplayRole)) m_restart = true;
                });
        }

        QAbstractItemModel* model() const { return m_model.data(); }
        const QVector<Match>& matches() const { return m_matches; }
        void invalidate() { m_restart = true; }
        void setRoot(const QModelIndex& root)
        { if (root != m_root) { m_root = root; m_restart = true; } }
        void setColumns(const QVector<int>& columns)
        {
            if (columns != m_columns) { m_columns = columns; m_restart = true; }
        }

        Status advance(const int maxMilliseconds = 4, const int maxWorkUnits = 256)
        {
            if (m_model.isNull()) return Status::Cancelled;
            QElapsedTimer timer;
            timer.start();
            int work = 0;
            while (work < qMax(1, maxWorkUnits) && timer.elapsed() < qMax(1, maxMilliseconds))
            {
                ++work;
                if (m_restart)
                {
                    m_restart = false;
                    m_matches.clear();
                    m_frames.clear();
                    m_column = 0;
                    m_startedColumn = false;
                }
                if (m_query.isEmpty() || m_maxHits <= 0 || m_matches.size() >= m_maxHits
                    || m_column >= m_columns.size()) return Status::Complete;
                const int column = m_columns.at(m_column);
                if (!m_startedColumn)
                {
                    m_frames.push_back({m_root, 0, m_model->rowCount(m_root)});
                    m_startedColumn = true;
                }
                if (m_frames.isEmpty())
                {
                    ++m_column;
                    m_startedColumn = false;
                    continue;
                }
                Frame& frame = m_frames.last();
                if (frame.nextRow >= frame.rowCount) { m_frames.removeLast(); continue; }
                const QModelIndex index = m_model->index(frame.nextRow++, column, frame.parent);
                if (!index.isValid() || (m_accept && !m_accept(index))) continue;
                const QString text = index.data(Qt::DisplayRole).toString();
                if (text.contains(m_query, Qt::CaseInsensitive))
                    m_matches.push_back({QPersistentModelIndex(index), text});
                if (m_matches.size() >= m_maxHits) return Status::Complete;
                const QModelIndex childParent = column != 0 ? index.sibling(index.row(), 0) : index;
                if (m_model->hasChildren(childParent))
                    m_frames.push_back({QPersistentModelIndex(childParent), 0, m_model->rowCount(childParent)});
            }
            return Status::Pending;
        }

    private:
        struct Frame { QPersistentModelIndex parent; int nextRow; int rowCount; };
        QPointer<QAbstractItemModel> m_model;
        QString m_query;
        QVector<int> m_columns;
        QVector<Frame> m_frames;
        QVector<Match> m_matches;
        int m_maxHits = 0;
        int m_column = 0;
        bool m_startedColumn = false;
        bool m_restart = true;
        std::function<bool(const QModelIndex&)> m_accept;
        QPersistentModelIndex m_root;
    };
}
