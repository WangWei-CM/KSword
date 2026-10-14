#include "../Framework.h"
#include "./ComplexReportDelegate.h"
#include "./ComplexReportModel.h"
#include "../theme.h"
#include <QPainter>
#include <QPixmap>
#include <QApplication>
#include <QStyle>

namespace ks::ui
{
    ComplexReportDelegate::ComplexReportDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    // 三个真实列统一对齐；只有分区与类型加字重，普通字段保持连续阅读密度。
    void ComplexReportDelegate::paint(QPainter* painter, const QStyleOptionViewItem& supplied,
        const QModelIndex& index) const
    {
        QStyleOptionViewItem option(supplied);
        initStyleOption(&option, index);
        if (!option.state.testFlag(QStyle::State_KeyboardFocusChange)) option.state &= ~QStyle::State_HasFocus;
        const auto purpose = static_cast<FieldNode::ReportRole>(index.data(ReportPurposeRole).toInt());
        const auto kind = static_cast<ReportNodeKind>(index.data(ReportKindRole).toInt());
        if (kind == ReportNodeKind::Section || purpose == FieldNode::ReportRole::Collection)
            option.font.setWeight(QFont::DemiBold);
        option.features &= ~QStyleOptionViewItem::WrapText;
        const auto status = static_cast<FieldNode::Status>(index.data(ReportStatusRole).toInt());
        if (index.column() == 0 && status != FieldNode::Status::Neutral)
        {
            // 状态点随真实来源变化，普通目录和字段不制造成功色；文字始终由共享表格主题绘制。
            QColor color = KswordTheme::InfoColor();
            if (status == FieldNode::Status::Success) color = KswordTheme::SuccessColor();
            if (status == FieldNode::Status::Warning) color = KswordTheme::WarningColor();
            if (status == FieldNode::Status::Error) color = KswordTheme::ErrorColor();
            QPixmap mark(14, 14);
            mark.fill(Qt::transparent);
            QPainter marker(&mark);
            marker.setRenderHint(QPainter::Antialiasing);
            marker.setPen(Qt::NoPen);
            marker.setBrush(KswordTheme::EnsureTextContrast(color,
                option.state.testFlag(QStyle::State_Selected)
                    ? option.palette.color(QPalette::Highlight) : option.palette.color(QPalette::Base), 3.0));
            marker.drawEllipse(QRectF(4, 4, 6, 6));
            marker.end();
            QIcon icon;
            for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected}) icon.addPixmap(mark, mode);
            option.icon = icon;
            option.decorationSize = QSize(14, 14);
            option.features |= QStyleOptionViewItem::HasDecoration;
        }
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, widget);
    }

    // 表格沿用应用字体；每行固定自然高度，不为元数据和折叠标题额外垫大块留白。
    QSize ComplexReportDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const
    {
        return QSize(80, qMax(22, QFontMetrics(option.font).height() + 4));
    }
}
