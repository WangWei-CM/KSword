#pragma once

#include "./StructuredFieldView.h"
#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QBitArray>
#include <memory>
#include <functional>

namespace ks::ui
{
    // 同一份规范文档的报告投影角色，供视图绘制与可访问性读取，不维护隐藏字段控件。
    enum ComplexReportRole
    {
        ReportKindRole = Qt::UserRole + 420,
        ReportNameRole,
        ReportValueRole,
        ReportDepthRole,
        ReportPathRole,
        ReportCountRole,
        ReportInitiallyExpandedRole,
        ReportStatusRole,
        ReportSearchRole,
        ReportSummaryRole,
        ReportPurposeRole,
        ReportReportedCountRole,
        ReportPreferredScopeRole
    };
    enum class ReportNodeKind { Section, Group, Field, Note, ValueDetail };

    // 树模型持有隐式共享的文档快照；数值更新保留索引，只有真实结构改变才重置模型。
    class ComplexReportModel final : public QAbstractItemModel
    {
    public:
        // Prepared values contain no model/QObject and may be built on a worker.
        struct State;
        using Snapshot = std::shared_ptr<const State>;
        using Translator = std::function<QString(const QString&)>;
        struct ChangedRange { int parent; int first; int last; };
        struct PreparedDocument
        {
            Snapshot base;
            Snapshot next;
            QVector<ChangedRange> changes;
            bool reset = false;
        };
        struct FilterState
        {
            Snapshot source;
            QString query;
            QString scopePath;
            bool diagnosticsOnly = false;
            QBitArray accepted;
            QVector<int> matchesBelow;
            QVector<int> childRows;
            QStringList expansionPaths;
            int rootMatches = 0;
        };
        using FilterSnapshot = std::shared_ptr<const FilterState>;
        static FilterSnapshot prepareFilter(Snapshot source, const QString& query, const QString& scopePath,
            bool diagnosticsOnly, const std::function<bool()>& cancelled = {});
        static QString preferredScopePath(const Snapshot& source);
        static bool containsPath(const Snapshot& source, const QString& path);
        static PreparedDocument prepareDocument(const FieldDocument& document, const Translator& translate,
            Snapshot previous = {}, const std::function<bool()>& cancelled = {});
        Snapshot snapshot() const { return m_state; }
        enum class InstallResult { Rejected, Updated, Reset };
        InstallResult installPrepared(const PreparedDocument& prepared);
        explicit ComplexReportModel(QObject* parent = nullptr);
        ~ComplexReportModel() override;
        bool setDocument(const FieldDocument& document);
        void relocalize();
        QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
        QModelIndex parent(const QModelIndex& child) const override;
        int rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;
        QModelIndex indexForPath(const QString& path) const;
        QVector<QModelIndex> initialExpansion() const;
        QVector<QModelIndex> sectionIndexes() const;
        QString textForIndex(const QModelIndex& index) const;
        const FieldDocument& document() const;
    private:
        Snapshot m_state; // 本模型独占节点索引和翻译缓存，避免跨快照的裸节点引用。
    };

    // 紧凑浏览过滤器：只隐藏已出现在列中的资料，搜索按多个词匹配完整规范上下文。
    class ComplexReportFilter final : public QSortFilterProxyModel
    {
    public:
        explicit ComplexReportFilter(QObject* parent = nullptr);
        void setSourceModel(QAbstractItemModel* model) override;
        void setPreparedSearchEnabled(bool enabled);
        void stagePrepared(ComplexReportModel::FilterSnapshot prepared);
        void applyPrepared(ComplexReportModel::FilterSnapshot prepared);
        int matchCount(const QModelIndex& proxyRoot = {}) const;
        QStringList expansionPaths() const;
        ComplexReportModel::FilterSnapshot preparedSnapshot() const { return m_prepared; }
        void setQuery(const QString& query);
        void setDiagnosticsOnly(bool enabled);
        void setScopePath(const QString& path);
    protected:
        bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    private:
        void rebuildPrepared();
        const ComplexReportModel::FilterState* activePrepared() const;
        ComplexReportModel::FilterSnapshot m_prepared;
        ComplexReportModel::FilterSnapshot m_staged;
        QVector<QMetaObject::Connection> m_sourceConnections;
        QString m_query;
        bool m_preparedSearch = false;
        bool m_diagnosticsOnly = false; // 诊断入口排除目录分区，不改变原模型。
        QString m_scopePath; // 范围过滤独立于视图根，空匹配不能退回其它分区。
    };
}
