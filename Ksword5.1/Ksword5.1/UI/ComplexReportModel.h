#pragma once

#include "./StructuredFieldView.h"
#include <QAbstractItemModel>
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
        ReportSummaryRole
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
}
