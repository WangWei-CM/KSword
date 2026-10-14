#pragma once
#include <QStyledItemDelegate>

namespace ks::ui
{
    // 紧凑字段表只补充状态点与自然行高；列、选择、焦点和复制使用原生视图语义。
    class ComplexReportDelegate final : public QStyledItemDelegate
    {
    public:
        explicit ComplexReportDelegate(QObject* parent = nullptr);
        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };
}
