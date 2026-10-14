#pragma once

#include "./StructuredFieldView.h"
#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <memory>

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
        struct State;
        std::unique_ptr<State> m_state; // 本模型独占节点索引和翻译缓存，避免跨快照的裸节点引用。
    };

    // 紧凑浏览过滤器：只隐藏已出现在列中的资料，搜索按多个词匹配完整规范上下文。
    class ComplexReportFilter final : public QSortFilterProxyModel
    {
    public:
        explicit ComplexReportFilter(QObject* parent = nullptr);
        void setQuery(const QString& query);
        void setDiagnosticsOnly(bool enabled);
        void setScopePath(const QString& path);
    protected:
        bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    private:
        QStringList m_terms; // 空白分隔的搜索词，全部命中才接受该行。
        bool m_diagnosticsOnly = false; // 诊断入口排除目录分区，不改变原模型。
        QString m_scopePath; // 范围过滤独立于视图根，空匹配不能退回其它分区。
    };
}
