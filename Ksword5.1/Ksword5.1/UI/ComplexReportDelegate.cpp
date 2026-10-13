#include "../Framework.h"
#include "./ComplexReportDelegate.h"
#include "./ComplexReportModel.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include <QCursor>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QItemSelectionModel>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QTextLayout>
#include <QTextOption>
#include <QTreeView>
#include <QTimer>
#include <QtMath>
#include <utility>

namespace
{
    constexpr int kHorizontalPadding = 14; // 行内文字留白，不重复接管页面四边留白。
    constexpr int kFieldGap = 16; // 名称与值之间保留可辨的列间距。
    constexpr int kStackedFieldWidth = 440; // 窄正文改为名称在上、值在下，不挤成逐字窄列。

    struct RowEdges
    {
        bool first = false; // 当前行是否为所在顶层分区的首行。
        bool last = false; // 当前行是否为分区最后一个展开且未过滤的后代。
        int topSpacing = 0; // 两张可见报告卡之间的唯一间距。
        int bottomSpacing = 0; // 分区底沿圆角的自然留白。
    };

    // 只读模型拓扑与展开状态判断卡片边缘，不能在 sizeHint 内查询 viewport 布局。
    RowEdges rowEdges(QTreeView* view, const QModelIndex& index)
    {
        RowEdges edges;
        if (!view || !index.isValid()) return edges;
        QModelIndex section = index;
        while (section.parent().isValid()) section = section.parent();
        if (static_cast<ks::ui::ReportNodeKind>(section.data(ks::ui::ReportKindRole).toInt())
            != ks::ui::ReportNodeKind::Section)
        {
            return edges;
        }
        edges.first = index == section;
        edges.topSpacing = edges.first && section.row() > 0 ? 8 : 0;
        // 已展开且存在接受后的子项时，本行不可能是该分区的最后可见行。
        if (view->isExpanded(index) && index.model()->rowCount(index) > 0) return edges;
        QModelIndex current = index;
        while (current != section)
        {
            const QModelIndex parent = current.parent();
            if (current.row() + 1 < current.model()->rowCount(parent)) return edges;
            current = parent;
        }
        edges.last = true;
        edges.bottomSpacing = 8;
        return edges;
    }

    // 分区卡只有最上和最下外沿圆角，中间所有行都直接连接同一无描边底板。
    QPainterPath cardRowPath(const QRectF& rect, const RowEdges& edges)
    {
        const qreal top = edges.first ? 8.0 : 0.0;
        const qreal bottom = edges.last ? 8.0 : 0.0;
        QPainterPath path;
        path.moveTo(rect.left() + top, rect.top());
        path.lineTo(rect.right() - top, rect.top());
        path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + top);
        path.lineTo(rect.right(), rect.bottom() - bottom);
        path.quadTo(rect.right(), rect.bottom(), rect.right() - bottom, rect.bottom());
        path.lineTo(rect.left() + bottom, rect.bottom());
        path.quadTo(rect.left(), rect.bottom(), rect.left(), rect.bottom() - bottom);
        path.lineTo(rect.left(), rect.top() + top);
        path.quadTo(rect.left(), rect.top(), rect.left() + top, rect.top());
        path.closeSubpath();
        return path;
    }

    // 复制入口只属于分区、类型和展开正文，普通深层字段由原上下文与 Ctrl+C 处理。
    bool hasCopyAction(ks::ui::ReportNodeKind kind, int depth)
    {
        return kind == ks::ui::ReportNodeKind::Section || kind == ks::ui::ReportNodeKind::ValueDetail
            || (kind == ks::ui::ReportNodeKind::Group && depth <= 1);
    }

    // 从原样式选项取得应用字体；只有分区与类型子组承担标题层级，深层字段组保持正文权重。
    QFont rowFont(const QStyleOptionViewItem& option, ks::ui::ReportNodeKind kind, int depth)
    {
        QFont font = option.font; // 保留用户字体家族和原有更大字号。
        if (font.pixelSize() > 0)
        {
            font.setPixelSize(qMax(14, font.pixelSize()));
        }
        else
        {
            font.setPointSizeF(qMax(10.0, font.pointSizeF()));
        }
        if (kind == ks::ui::ReportNodeKind::Section
            || (kind == ks::ui::ReportNodeKind::Group && depth <= 1))
        {
            font.setWeight(QFont::DemiBold);
        }
        else if (kind == ks::ui::ReportNodeKind::Group)
        {
            font.setWeight(QFont::Normal);
        }
        return font;
    }

    struct PropertyEdges
    {
        QModelIndex owner; // 展开属性所属的真实深层字段组，复制仍读取此规范父项。
        bool first = false; // 第一条直接属性行承载轻量父字段标题与复制动作。
        bool last = false; // 最后可见属性后代提供共同内嵌面的底沿。
        int topSpacing = 0; // 字段头与属性面之间仅留四像素。
        int bottomSpacing = 0; // 属性面底部圆角的自然留白。
        int headerHeight = 0; // 标题与复制使用固定自然高度，不覆盖第一条属性正文。
    };

    // 只按代理模型拓扑定位属性面，全文子项仍归其规范父字段，不另外维护属性控件。
    PropertyEdges propertyEdges(QTreeView* view, const QModelIndex& index)
    {
        PropertyEdges edges;
        if (!view || !index.isValid()) return edges;
        QModelIndex direct = index;
        for (QModelIndex parent = direct.parent(); parent.isValid(); direct = parent, parent = parent.parent())
        {
            if (static_cast<ks::ui::ReportNodeKind>(parent.data(ks::ui::ReportKindRole).toInt()) == ks::ui::ReportNodeKind::Group
                && parent.data(ks::ui::ReportDepthRole).toInt() >= 2)
            {
                const auto directKind = static_cast<ks::ui::ReportNodeKind>(direct.data(ks::ui::ReportKindRole).toInt());
                if (directKind != ks::ui::ReportNodeKind::Field && directKind != ks::ui::ReportNodeKind::Note) return edges;
                edges.owner = parent;
                break;
            }
        }
        if (!edges.owner.isValid()) return edges;
        edges.first = index == direct && direct.row() == 0;
        edges.topSpacing = edges.first ? 4 : 0;
        if (edges.first)
        {
            QStyleOptionViewItem option;
            option.font = view->font();
            const QFontMetrics metrics(rowFont(option, ks::ui::ReportNodeKind::Field, 0));
            edges.headerHeight = qMax(26, metrics.height() + 8) + 18;
        }
        if (view->isExpanded(index) && index.model()->rowCount(index) > 0) return edges;
        QModelIndex current = index;
        while (current != edges.owner)
        {
            if (current.row() + 1 < current.model()->rowCount(current.parent())) return edges;
            current = current.parent();
        }
        edges.last = true;
        edges.bottomSpacing = 8;
        return edges;
    }

    // 属性面相对字段头内缩，左右由同一共享几何负责；RTL 同步镜像。
    QRect propertyPanelRect(QTreeView* view, const QModelIndex& index, const QRect& row)
    {
        const RowEdges outer = rowEdges(view, index);
        const PropertyEdges property = propertyEdges(view, index);
        int levels = view->rootIsDecorated() ? 1 : 0;
        for (QModelIndex parent = property.owner.parent(); parent.isValid() && parent != view->rootIndex(); parent = parent.parent()) ++levels;
        const int inset = levels * view->indentation() + 8;
        QRect panel(0, row.top() + outer.topSpacing + property.topSpacing, view->viewport()->width(),
            row.height() - outer.topSpacing - property.topSpacing - outer.bottomSpacing);
        if (view->layoutDirection() == Qt::RightToLeft) panel.adjust(kHorizontalPadding, 0, -inset, 0);
        else panel.adjust(inset, 0, -kHorizontalPadding, 0);
        return panel;
    }

    // 行正文剔除两层边缘和首属性标题，绘制、测量与命中使用相同自然空间。
    QRect rowPayload(QTreeView* view, const QModelIndex& index, const QRect& row)
    {
        const RowEdges edges = rowEdges(view, index);
        const PropertyEdges property = propertyEdges(view, index);
        return row.adjusted(0, edges.topSpacing + property.topSpacing + property.headerHeight,
            0, -edges.bottomSpacing - property.bottomSpacing);
    }

    // 统一测量和绘制完整纯文本；显式换行与长标识符都保留，不解析 HTML。
    qreal layoutText(const QString& text, const QFont& font, int width,
        QPainter* painter = nullptr, const QPointF& origin = QPointF(), int maximumLines = 0)
    {
        qreal height = 0; // 已排出的正文总高度。
        int lineCount = 0; // 仅紧凑说明限制预览行数，全文子项不限制。
        const QStringList paragraphs = text.split(QLatin1Char('\n'));
        for (const QString& paragraph : paragraphs)
        {
            if (maximumLines > 0 && lineCount >= maximumLines)
            {
                break;
            }
            QTextLayout layout(paragraph, font); // 每个可见段落使用当前字体与真实行宽。
            QTextOption wrapping;
            wrapping.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout.setTextOption(wrapping);
            const qreal paragraphTop = height;
            layout.beginLayout();
            for (;;)
            {
                if (maximumLines > 0 && lineCount >= maximumLines)
                {
                    break;
                }
                QTextLine line = layout.createLine();
                if (!line.isValid())
                {
                    break;
                }
                line.setLineWidth(qMax(1, width));
                line.setPosition(QPointF(0, height - paragraphTop));
                height += line.height();
                ++lineCount;
            }
            layout.endLayout();
            if (height == paragraphTop)
            {
                height += QFontMetricsF(font).height(); // 空段落仍占一个真实文本行。
                ++lineCount;
            }
            if (painter)
            {
                layout.draw(painter, origin + QPointF(0, paragraphTop));
            }
        }
        return height;
    }

    // 行正文与树分支共用交互状态配方，底色由各自实际表面传入，不重复主题算法。
    QColor interactionSurface(const QStyleOptionViewItem& option, QColor surface)
    {
        if (option.state.testFlag(QStyle::State_Selected))
        {
            // 选中只给现有层级面增加轻强调，避免把整条报告行刷成实心亮蓝。
            return KswordTheme::BlendColors(surface, KswordTheme::ControlAccentColor(), 80);
        }
        if (option.state.testFlag(QStyle::State_MouseOver)
            && option.state.testFlag(QStyle::State_Enabled))
        {
            surface = KswordTheme::ControlInputHoverColor(surface);
        }
        return surface;
    }

    // 根据明确节点类型选择正文面，选中和悬停统一交给共享状态配方。
    QColor rowSurface(const QStyleOptionViewItem& option, ks::ui::ReportNodeKind kind, int depth)
    {
        QColor surface = KswordTheme::SurfaceColor(); // 深层字段与普通正文连接同一完整底板。
        if (kind == ks::ui::ReportNodeKind::ValueDetail) surface = KswordTheme::SurfaceAltColor();
        else if (kind == ks::ui::ReportNodeKind::Group && depth <= 1)
            surface = KswordTheme::BlendColors(surface, KswordTheme::SurfaceAltColor(), 90);
        return interactionSurface(option, surface);
    }

    // 所有正文，包括禁用信息，都按实际行底校准到 4.5:1。
    QColor textColor(const QStyleOptionViewItem& option, const QColor& surface, bool secondary)
    {
        QColor preferred = secondary ? KswordTheme::TextSecondaryColor() : KswordTheme::TextPrimaryColor();
        if (!option.state.testFlag(QStyle::State_Enabled))
        {
            preferred = KswordTheme::TextDisabledColor();
        }
        // 本报告使用轻强调选中面，默认 HighlightedText 属于另一种高亮底色；沿用主题正文前景再按真实底校准。
        return KswordTheme::EnsureTextContrast(preferred, surface, 4.5);
    }

    // 只绘制生产者明确登记的状态；未登记状态没有点或成功颜色。
    QColor statusColor(ks::ui::FieldNode::Status status, const QColor& surface, bool enabled)
    {
        QColor preferred; // 中性状态保持无效颜色，调用方据此不画状态点。
        switch (status)
        {
        case ks::ui::FieldNode::Status::Info: preferred = KswordTheme::InfoColor(); break;
        case ks::ui::FieldNode::Status::Success: preferred = KswordTheme::SuccessColor(); break;
        case ks::ui::FieldNode::Status::Warning: preferred = KswordTheme::WarningColor(); break;
        case ks::ui::FieldNode::Status::Error: preferred = KswordTheme::ErrorColor(); break;
        case ks::ui::FieldNode::Status::Neutral: return QColor();
        }
        if (!enabled)
        {
            preferred = KswordTheme::TextDisabledColor();
        }
        return KswordTheme::EnsureTextContrast(preferred, surface, 3.0);
    }

    // 复制动作的绘制与命中共用同一矩形，不占用树箭头和普通行选择区域。
    QRect copyButtonRect(const QRect& row, const QFont& font, bool detail = false)
    {
        const QFontMetrics metrics(font);
        const int width = metrics.horizontalAdvance(ks::i18n::sourceText(QStringLiteral("复制"))) + 20;
        const int height = qMax(26, metrics.height() + 8);
        return QRect(row.right() - kHorizontalPadding - width + 1,
            detail ? row.top() + 10 : row.center().y() - height / 2, width, height);
    }

    // 只识别模型实际提供的全文子项；普通结构子项继续使用 Qt 左侧分支操作。
    bool hasValueDetailChild(const QModelIndex& index)
    {
        const auto* model = index.model(); // 代理模型已经保留当前搜索真正可见的子项身份。
        for (int row = model->rowCount(index) - 1; row >= 0; --row)
        {
            const QModelIndex child = model->index(row, 0, index);
            if (static_cast<ks::ui::ReportNodeKind>(child.data(ks::ui::ReportKindRole).toInt())
                == ks::ui::ReportNodeKind::ValueDetail)
            {
                return true;
            }
        }
        return false;
    }

    // 展开文字只属于真实全文预览，状态仍来自原树的展开状态。
    QString expansionText(QTreeView* view, const QModelIndex& index)
    {
        if (!hasValueDetailChild(index)) return QString();
        return ks::i18n::sourceText(view && view->isExpanded(index)
            ? QStringLiteral("收起") : QStringLiteral("展开"));
    }

    // 绘制、命中和正文宽度共用一份链接几何；说明链接只占顶行，不吞掉整段正文。
    QRect expansionLinkRect(const QRect& panel, const QFont& font, const QString& text,
        ks::ui::ReportNodeKind kind)
    {
        const QFontMetrics metrics(font);
        const QRect content = panel.adjusted(kHorizontalPadding, 0, -kHorizontalPadding, 0);
        const int width = metrics.horizontalAdvance(text) + 14;
        return QRect(content.right() - width + 1,
            kind == ks::ui::ReportNodeKind::Note ? content.top() + 10 : content.top(),
            width, kind == ks::ui::ReportNodeKind::Note ? metrics.height() : content.height());
    }

    // 树和委托共用真实交互状态，不借用 QTreeView 默认高亮背景绘制。
    QStyleOptionViewItem reportOption(QTreeView* view, const QModelIndex& index,
        QStyleOptionViewItem option, const QRect& row)
    {
        if (!view) return option;
        option.state.setFlag(QStyle::State_Enabled, view->isEnabled() && index.flags().testFlag(Qt::ItemIsEnabled));
        option.state.setFlag(QStyle::State_Selected, view->selectionModel() && view->selectionModel()->isSelected(index));
        option.state.setFlag(QStyle::State_HasFocus, view->hasFocus() && view->currentIndex() == index);
        const QPoint mouse = view->viewport()->mapFromGlobal(QCursor::pos());
        const QRect payload = rowPayload(view, index, row);
        option.state.setFlag(QStyle::State_MouseOver, view->viewport()->underMouse()
            && mouse.y() >= payload.top() && mouse.y() <= payload.bottom());
        return option;
    }

    // 标题和全文正文共用同一复制动作配方，几何仍由同一矩形函数供命中读取。
    void paintCopyAction(QPainter* painter, QTreeView* view, const QStyleOptionViewItem& option,
        const QRect& button, const QColor& surface, bool pressed)
    {
        const bool hovered = option.state.testFlag(QStyle::State_Enabled) && view
            && button.contains(view->viewport()->mapFromGlobal(QCursor::pos()));
        QColor buttonSurface = KswordTheme::BlendColors(surface, KswordTheme::SurfaceMutedColor(), 90);
        if (pressed) buttonSurface = KswordTheme::ControlInputFocusColor(buttonSurface);
        else if (hovered) buttonSurface = KswordTheme::ControlInputHoverColor(buttonSurface);
        painter->setPen(Qt::NoPen);
        painter->setBrush(buttonSurface);
        painter->drawRoundedRect(button, KswordTheme::ControlCornerRadius, KswordTheme::ControlCornerRadius);
        painter->setPen(textColor(option, buttonSurface, false));
        painter->drawText(button, Qt::AlignCenter, ks::i18n::sourceText(QStringLiteral("复制")));
    }
}

ks::ui::ComplexReportTree::ComplexReportTree(QWidget* parent) : QTreeView(parent)
{
    connect(this, &QTreeView::expanded, this, [this]() { scheduleReportLayout(); });
    connect(this, &QTreeView::collapsed, this, [this]() { scheduleReportLayout(); });
}

// 模型绑定保留原 Qt 路径；只监听本树的结构与文本变化，过滤代理的行变更同样重测边缘。
void ks::ui::ComplexReportTree::setModel(QAbstractItemModel* reportModel)
{
    for (const auto& connection : m_modelConnections) QObject::disconnect(connection);
    m_modelConnections.clear();
    QTreeView::setModel(reportModel);
    if (reportModel)
    {
        const auto schedule = [this]() { scheduleReportLayout(); };
        m_modelConnections.append(connect(reportModel, &QAbstractItemModel::modelReset, this, schedule));
        m_modelConnections.append(connect(reportModel, &QAbstractItemModel::rowsInserted, this, schedule));
        m_modelConnections.append(connect(reportModel, &QAbstractItemModel::rowsRemoved, this, schedule));
        m_modelConnections.append(connect(reportModel, &QAbstractItemModel::layoutChanged, this, schedule));
        m_modelConnections.append(connect(reportModel, &QAbstractItemModel::dataChanged, this, schedule));
    }
    scheduleReportLayout();
}

// 展开全部、过滤和规范提交各自可能连续发信号，排队合并后再统一重算当前展开行。
void ks::ui::ComplexReportTree::scheduleReportLayout()
{
    if (m_layoutPending) return;
    m_layoutPending = true;
    QTimer::singleShot(0, this, [this]()
    {
        m_layoutPending = false;
        doItemsLayout(); // sizeHint 的边缘键随真实拓扑改变，不读取或递归进入 viewport 布局。
        viewport()->update();
    });
}

// 单列报告自行绘整行卡面，随后把分支与正文送到 Qt 原有列和缩进区域。
void ks::ui::ComplexReportTree::drawRow(QPainter* painter, const QStyleOptionViewItem& supplied,
    const QModelIndex& index) const
{
    QStyleOptionViewItem option = reportOption(const_cast<ComplexReportTree*>(this), index, supplied, supplied.rect);
    const RowEdges edges = rowEdges(const_cast<ComplexReportTree*>(this), index);
    const PropertyEdges property = propertyEdges(const_cast<ComplexReportTree*>(this), index);
    const auto kind = static_cast<ReportNodeKind>(index.data(ReportKindRole).toInt());
    const int depth = index.data(ReportDepthRole).toInt();
    const QRect fullRow(0, supplied.rect.top(), viewport()->width(), supplied.rect.height());
    const QRect cardRow = fullRow.adjusted(0, edges.topSpacing, 0, 0);
    const QPainterPath card = cardRowPath(QRectF(cardRow), edges);
    const QColor surface = kind == ReportNodeKind::ValueDetail || property.owner.isValid()
        ? KswordTheme::SurfaceColor() : rowSurface(option, kind, depth);
    painter->save();
    painter->setClipRect(fullRow, Qt::IntersectClip);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->fillRect(fullRow, palette().color(QPalette::Base)); // 只留下卡片间隙与真实外圆角。
    painter->fillPath(card, surface);
    painter->setClipPath(card, Qt::IntersectClip);
    if (property.owner.isValid())
    {
        RowEdges propertyCorners; // 全体属性共享一个内嵌面，只有其首末边缘允许圆角。
        propertyCorners.first = property.first;
        propertyCorners.last = property.last;
        const QRect panel = propertyPanelRect(const_cast<ComplexReportTree*>(this), index, fullRow);
        painter->fillPath(cardRowPath(QRectF(panel), propertyCorners),
            interactionSurface(option, KswordTheme::SurfaceAltColor()));
    }

    // 同原生单列树，缩进槽累计祖先层数并在 RTL 下镜像，点击仍由 QTreeView 处理。
    int levels = rootIsDecorated() ? 1 : 0;
    for (QModelIndex parent = index.parent(); parent.isValid() && parent != rootIndex(); parent = parent.parent()) ++levels;
    const int position = columnViewportPosition(0);
    const int width = columnWidth(0);
    const int branchesWidth = qMin(width, levels * indentation());
    const bool reverse = layoutDirection() == Qt::RightToLeft;
    const QRect branches(reverse ? position + width - branchesWidth : position, fullRow.top(), branchesWidth, fullRow.height());
    if (branchesWidth > 0) drawBranches(painter, branches, index);
    option.rect = QRect(reverse ? position : position + branchesWidth, fullRow.top(),
        qMax(0, width - branchesWidth), fullRow.height());
    itemDelegateForIndex(index)->paint(painter, option, index);
    painter->restore();
}

// 输入 Qt 原生分支区域与真实索引；只绘有子项的空心折角，不替换展开或命中逻辑。
void ks::ui::ComplexReportTree::drawBranches(QPainter* painter, const QRect& rect,
    const QModelIndex& index) const
{
    QStyleOptionViewItem supplied;
    supplied.initFrom(this);
    const QStyleOptionViewItem option = reportOption(const_cast<ComplexReportTree*>(this), index, supplied, rect);
    const auto kind = static_cast<ReportNodeKind>(index.data(ReportKindRole).toInt());
    const PropertyEdges property = propertyEdges(const_cast<ComplexReportTree*>(this), index);
    const QColor surface = property.owner.isValid() ? interactionSurface(option, KswordTheme::SurfaceAltColor())
        : rowSurface(option, kind, index.data(ReportDepthRole).toInt());
    const QRect payload = rowPayload(const_cast<ComplexReportTree*>(this), index, rect);
    painter->save();
    painter->setClipRect(payload, Qt::IntersectClip); // 分支不铺第二份底板，避免割裂分区的完整背景。
    if (!model() || !model()->hasChildren(index) || rect.isEmpty())
    {
        painter->restore();
        return; // 叶节点不画假展开符号，全文节点也不重复制造第二个分支。
    }

    // 折角放在靠近正文的最后一个缩进槽内，沿用 Qt 默认分支点击范围及 RTL 镜像。
    const qreal centerX = layoutDirection() == Qt::RightToLeft
        ? rect.left() + indentation() / 2.0 : rect.right() - indentation() / 2.0 + 1.0;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(centerX, payload.center().y());
    painter->rotate(isExpanded(index) ? 90.0 : layoutDirection() == Qt::RightToLeft ? 180.0 : 0.0);
    const qreal extent = qBound(3.0, QFontMetricsF(font()).height() / 4.0, 5.0);
    QPainterPath chevron;
    chevron.moveTo(-extent * 0.45, -extent * 0.75);
    chevron.lineTo(extent * 0.45, 0);
    chevron.lineTo(-extent * 0.45, extent * 0.75);
    // 共用正文的实际底色校准，分支前景达到 4.5:1，也满足主要图形的 3:1 下限。
    painter->setPen(QPen(textColor(option, surface, true), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(chevron);
    painter->restore();
}

ks::ui::ComplexReportDelegate::ComplexReportDelegate(QTreeView* view)
    : QStyledItemDelegate(view), m_view(view)
{
}

// 输入可见模型项与原视图选项；只绘当前行，不生成单元格 QWidget 或查询业务数据。
void ks::ui::ComplexReportDelegate::paint(QPainter* painter,
    const QStyleOptionViewItem& supplied, const QModelIndex& index) const
{
    QStyleOptionViewItem option(supplied);
    initStyleOption(&option, index);
    option = reportOption(m_view, index, option, supplied.rect);
    const auto kind = static_cast<ReportNodeKind>(index.data(ReportKindRole).toInt());
    const int depth = index.data(ReportDepthRole).toInt();
    const QFont font = rowFont(option, kind, depth);
    const QFontMetrics metrics(font);
    const PropertyEdges property = propertyEdges(m_view, index);
    const QColor surface = property.owner.isValid() ? interactionSurface(option, KswordTheme::SurfaceAltColor())
        : rowSurface(option, kind, depth);
    const QString name = index.data(ReportNameRole).toString();
    const QString value = index.data(ReportValueRole).toString();
    const bool heading = kind == ReportNodeKind::Section || kind == ReportNodeKind::Group;
    const bool enabled = option.state.testFlag(QStyle::State_Enabled);

    painter->save();
    painter->setClipRect(option.rect, Qt::IntersectClip);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setFont(font);
    painter->setPen(Qt::NoPen);
    painter->setBrush(surface);
    const QRect payload = rowPayload(m_view, index, option.rect);
    const QRect panel = kind == ReportNodeKind::ValueDetail ? payload.adjusted(6, 4, -6, -4) : payload;
    if (property.first)
    {
        const QRect properties = propertyPanelRect(m_view, index, option.rect);
        const QRect copy = copyButtonRect(properties, font, true);
        paintCopyAction(painter, m_view, option, copy, surface, m_pressedCopy == index);
        QString title = property.owner.data(ReportNameRole).toString(); // 轻量路径来自原节点，不解析展示文本。
        const QString group = property.owner.parent().data(ReportNameRole).toString();
        if (!group.isEmpty()) title.prepend(group + QStringLiteral(" / "));
        const QRect titleArea(properties.left() + kHorizontalPadding, copy.top(),
            qMax(1, copy.left() - properties.left() - kHorizontalPadding - 8), copy.height());
        painter->setPen(textColor(option, surface, true));
        painter->drawText(titleArea, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(title, Qt::ElideRight, titleArea.width()));
    }
    if (kind == ReportNodeKind::ValueDetail)
    {
        painter->drawRoundedRect(panel, 8, 8); // 只有全文正文画内嵌面，普通行由整卡底板负责。
    }
    QRect content = panel.adjusted(kHorizontalPadding, 0, -kHorizontalPadding, 0);
    if (heading)
    {
        if (hasCopyAction(kind, depth))
        {
            const QRect copy = copyButtonRect(panel, font);
            paintCopyAction(painter, m_view, option, copy, surface, m_pressedCopy == index);
            content.setRight(copy.left() - 10);
        }

        const int count = index.data(ReportCountRole).toInt(); // 数量来自规范子树，不从可见过滤行数猜测。
        if (count > 0 && depth <= 1)
        {
            const QString countText = kind == ReportNodeKind::Section
                ? ks::i18n::sourceText(QStringLiteral("%1 项")).arg(count) : QString::number(count);
            const int countWidth = metrics.horizontalAdvance(countText);
            const QRect countArea(content.right() - countWidth + 1, content.top(), countWidth, content.height());
            painter->setPen(textColor(option, surface, true));
            painter->drawText(countArea, Qt::AlignVCenter | Qt::AlignRight, countText);
            content.setRight(countArea.left() - 12);
        }
        const QColor status = statusColor(static_cast<FieldNode::Status>(index.data(ReportStatusRole).toInt()),
            surface, enabled);
        if (status.isValid())
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(status);
            painter->drawEllipse(QPointF(content.left() + 4, content.center().y()), 4, 4);
            content.setLeft(content.left() + 16);
        }
        QRect nameArea = content; // 普通分区保留完整标题空间，字段组摘要只使用足够宽的正文。
        if (kind == ReportNodeKind::Group && !value.isEmpty() && content.width() >= 380)
        {
            const int maximum = qMax(180, qRound(metrics.height() * 13.0));
            const int nameWidth = qBound(96, metrics.horizontalAdvance(name) + 12,
                qMin(content.width() / 2, maximum));
            nameArea.setWidth(nameWidth);
            QRect preview = content;
            preview.setLeft(nameArea.right() + kFieldGap + 1);
            QFont previewFont = font; // 地址与类型摘要是次级正文，不跟随选中标题增加字重。
            previewFont.setWeight(QFont::Normal);
            painter->setFont(previewFont);
            painter->setPen(textColor(option, surface, true));
            painter->drawText(preview, Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(previewFont).elidedText(value.simplified(), Qt::ElideMiddle, qMax(1, preview.width())));
            painter->setFont(font);
        }
        painter->setPen(textColor(option, surface, false));
        painter->drawText(nameArea, Qt::AlignVCenter | Qt::AlignLeft,
            metrics.elidedText(name, Qt::ElideRight, qMax(1, nameArea.width())));
    }
    else if (kind == ReportNodeKind::Field)
    {
        // 行内风险同样来自显式状态；普通字段不因为值文本而制造成功或失败标记。
        const QColor status = statusColor(static_cast<FieldNode::Status>(index.data(ReportStatusRole).toInt()),
            surface, enabled);
        if (status.isValid())
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(status);
            painter->drawEllipse(QPointF(content.left() + 4, content.center().y()), 4, 4);
            content.setLeft(content.left() + 16);
        }
        const QString hint = expansionText(m_view, index);
        const bool stacked = option.rect.width() < kStackedFieldWidth;
        const QRect hintArea = expansionLinkRect(panel, font, hint, kind);
        if (!hint.isEmpty())
        {
            painter->setPen(KswordTheme::EnsureTextContrast(enabled
                ? KswordTheme::ControlAccentColor() : KswordTheme::TextDisabledColor(), surface, 4.5));
            painter->drawText(hintArea, Qt::AlignVCenter | Qt::AlignRight, hint);
            content.setRight(hintArea.left() - 8);
        }
        QRect keyArea = content; // 窄窗给名称完整一行，宽窗把名称列限制在合理占比。
        QRect valueArea = content;
        if (stacked)
        {
            keyArea.setTop(content.top() + 5);
            keyArea.setHeight(metrics.height());
            valueArea.setTop(keyArea.bottom() + 4);
            valueArea.setHeight(metrics.height());
        }
        else
        {
            const int keyMaximum = qMax(180, qRound(metrics.height() * 13.0)); // 默认约 220 逻辑像素，放大字体同步增长。
            const int keyWidth = qBound(120, content.width() * 2 / 5, keyMaximum);
            keyArea.setWidth(qMin(keyWidth, qMax(1, content.width() - 100 - kFieldGap)));
            valueArea.setLeft(keyArea.right() + kFieldGap + 1);
        }
        painter->setPen(textColor(option, surface, true));
        painter->drawText(keyArea, Qt::AlignVCenter | Qt::AlignLeft,
            metrics.elidedText(name, Qt::ElideRight, qMax(1, keyArea.width())));
        painter->setPen(textColor(option, surface, false));
        painter->drawText(valueArea, Qt::AlignVCenter | Qt::AlignLeft,
            metrics.elidedText(value.simplified(), Qt::ElideRight, qMax(1, valueArea.width())));
    }
    else
    {
        content.adjust(0, 10, 0, -10);
        if (kind == ReportNodeKind::ValueDetail)
        {
            const QRect copy = copyButtonRect(panel, font, true);
            paintCopyAction(painter, m_view, option, copy, surface, m_pressedCopy == index);
            if (!name.isEmpty())
            {
                painter->setPen(textColor(option, surface, true));
                const QRect label(content.left(), content.top(), qMax(1, copy.left() - content.left() - 8), copy.height());
                painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                    metrics.elidedText(name, Qt::ElideRight, label.width()));
            }
            content.setTop(content.top() + copy.height() + 8);
        }
        const QString hint = kind == ReportNodeKind::Note ? expansionText(m_view, index) : QString();
        if (!hint.isEmpty())
        {
            const QRect hintArea = expansionLinkRect(panel, font, hint, kind);
            painter->setPen(KswordTheme::EnsureTextContrast(enabled
                ? KswordTheme::ControlAccentColor() : KswordTheme::TextDisabledColor(), surface, 4.5));
            painter->drawText(hintArea, Qt::AlignRight | Qt::AlignVCenter, hint);
            content.setRight(hintArea.left() - 8);
        }
        painter->setPen(textColor(option, surface, kind == ReportNodeKind::Note));
        layoutText(value.isEmpty() ? name : value, font, content.width(), painter, content.topLeft(),
            kind == ReportNodeKind::Note && !hint.isEmpty() ? 2 : 0);
    }
    // 只保留键盘导航焦点；鼠标选择不额外叠焦点矩形，也不改变原有焦点策略。
    if (option.state.testFlag(QStyle::State_HasFocus)
        && option.state.testFlag(QStyle::State_KeyboardFocusChange) && m_view)
    {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(option);
        focus.rect = panel.adjusted(2, 2, -2, -2);
        focus.backgroundColor = surface;
        m_view->style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, painter, m_view);
    }
    painter->restore();
}

// 输入模型路径、字体及实际视口宽；按需计算自然高度，不遍历其它节点。
QSize ks::ui::ComplexReportDelegate::sizeHint(const QStyleOptionViewItem& supplied,
    const QModelIndex& index) const
{
    QStyleOptionViewItem option(supplied);
    initStyleOption(&option, index);
    const auto kind = static_cast<ReportNodeKind>(index.data(ReportKindRole).toInt());
    const int depth = index.data(ReportDepthRole).toInt();
    const QFont font = rowFont(option, kind, depth);
    int indentation = 0; // Qt 委托行矩形不含原生树分支，这里测量时扣掉相同空间。
    int width = qMax(1, option.rect.width());
    if (m_view)
    {
        width = m_view->viewport()->width();
        int depth = m_view->rootIsDecorated() ? 1 : 0;
        for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) ++depth;
        indentation = depth * m_view->indentation();
    }
    width = qMax(1, width - indentation);
    const QString path = index.data(ReportPathRole).toString();
    const QString name = index.data(ReportNameRole).toString();
    const QString value = index.data(ReportValueRole).toString();
    const bool children = hasValueDetailChild(index); // 只有全文子项允许把长说明缩为预览。
    const QString expand = ks::i18n::sourceText(QStringLiteral("展开"));
    const QString collapse = ks::i18n::sourceText(QStringLiteral("收起"));
    const QString actions = kind == ReportNodeKind::Note && children
        ? expand + QLatin1Char('\n') + collapse : QString();
    const RowEdges edges = rowEdges(m_view, index);
    const PropertyEdges property = propertyEdges(m_view, index);
    const int topSpacing = edges.topSpacing + property.topSpacing + property.headerHeight;
    const int bottomSpacing = edges.bottomSpacing + property.bottomSpacing;
    const auto cached = m_measurements.constFind(path);
    if (cached != m_measurements.constEnd() && cached->width == width
        && cached->kind == static_cast<int>(kind) && cached->children == children
        && cached->topSpacing == topSpacing && cached->bottomSpacing == bottomSpacing
        && cached->font == font && cached->name == name && cached->value == value
        && cached->actions == actions)
    {
        return cached->hint;
    }
    const QFontMetrics metrics(font);
    int height = metrics.height() + 10; // 普通行保持自然紧凑高度，不均分可用空间。
    if (kind == ReportNodeKind::Section) height = metrics.height() + 22;
    else if (kind == ReportNodeKind::Group) height = metrics.height() + (depth <= 1 ? 14 : 10);
    else if (kind == ReportNodeKind::Field && width < kStackedFieldWidth) height = metrics.height() * 2 + 14;
    else if (kind == ReportNodeKind::Note || kind == ReportNodeKind::ValueDetail)
    {
        int textWidth = qMax(1, width - kHorizontalPadding * 2);
        if (kind == ReportNodeKind::ValueDetail) textWidth = qMax(1, textWidth - 12);
        else if (children)
        {
            const int hintWidth = qMax(metrics.horizontalAdvance(expand), metrics.horizontalAdvance(collapse)) + 22;
            textWidth = qMax(1, textWidth - hintWidth);
        }
        height = qCeil(layoutText(value.isEmpty() ? name : value, font, textWidth, nullptr, QPointF(),
            kind == ReportNodeKind::Note && children ? 2 : 0)) + 20;
        if (kind == ReportNodeKind::ValueDetail)
        {
            height += 8; // 正文外卡片四边留白与绘制矩形同步。
            height += qMax(26, metrics.height() + 8) + 8; // 固定复制行不会压到全文或改变导出范围。
        }
    }
    const QSize hint(width, qMax(28, height) + topSpacing + bottomSpacing);
    m_measurements.insert(path, {width, static_cast<int>(kind), children,
        topSpacing, bottomSpacing, font, name, value, actions, hint});
    return hint;
}

// 只拦截复制按钮和全文链接内的完整点击，其余事件交给 Qt 分支、选择与键盘路径。
bool ks::ui::ComplexReportDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
    const QStyleOptionViewItem& supplied, const QModelIndex& index)
{
    // 拖到其它行释放也清理两个动作，不能让下一次普通行点击触发旧按下状态。
    const QPersistentModelIndex pressedCopy = m_pressedCopy;
    const QPersistentModelIndex pressedExpand = m_pressedExpand;
    const bool leftRelease = event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton;
    if (leftRelease && (pressedCopy.isValid() || pressedExpand.isValid()))
    {
        m_pressedCopy = QPersistentModelIndex();
        m_pressedExpand = QPersistentModelIndex();
        if (m_view) m_view->viewport()->update();
    }
    const auto kind = static_cast<ReportNodeKind>(index.data(ReportKindRole).toInt());
    const int depth = index.data(ReportDepthRole).toInt();
    const PropertyEdges property = propertyEdges(m_view, index);
    const bool copyAction = hasCopyAction(kind, depth) || property.first;
    const bool preview = kind == ReportNodeKind::Field || kind == ReportNodeKind::Note;
    const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
    if (!enabled || (!copyAction && !preview))
    {
        if (leftRelease && (pressedCopy.isValid() || pressedExpand.isValid())) return true;
        return QStyledItemDelegate::editorEvent(event, model, supplied, index);
    }
    QStyleOptionViewItem option(supplied);
    initStyleOption(&option, index);
    option = reportOption(m_view, index, option, supplied.rect);
    const QRect payload = rowPayload(m_view, index, option.rect);
    const QRect panel = kind == ReportNodeKind::ValueDetail ? payload.adjusted(6, 4, -6, -4) : payload;
    const QFont font = rowFont(option, kind, depth);
    const QRect copy = property.first
        ? copyButtonRect(propertyPanelRect(m_view, index, option.rect), font, true)
        : copyButtonRect(panel, font, kind == ReportNodeKind::ValueDetail);
    const QString hint = preview ? expansionText(m_view, index) : QString();
    const QRect expand = expansionLinkRect(panel, font, hint, kind);
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
        || event->type() == QEvent::MouseButtonDblClick)
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            const bool copyHit = copyAction && m_copy && copy.contains(mouse->position().toPoint());
            const bool expandHit = preview && m_view && !hint.isEmpty()
                && expand.contains(mouse->position().toPoint());
            if (event->type() == QEvent::MouseButtonPress)
            {
                m_pressedCopy = QPersistentModelIndex();
                m_pressedExpand = QPersistentModelIndex();
                if (copyHit)
                {
                    m_pressedCopy = index;
                    if (m_view) m_view->viewport()->update(copy);
                    return true;
                }
                if (expandHit)
                {
                    m_pressedExpand = index;
                    m_view->viewport()->update(expand);
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonDblClick && (copyHit || expandHit))
            {
                return true; // 行内动作双击不额外进入 Qt 的整行双击展开路径。
            }
            if (event->type() == QEvent::MouseButtonRelease && pressedCopy.isValid())
            {
                const bool trigger = copyHit && pressedCopy == index;
                if (trigger)
                {
                    const auto copyHandler = m_copy; // 回调可销毁视图和本委托，调用后不再访问成员。
                    copyHandler(property.first ? property.owner : index);
                    return true;
                }
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease && pressedExpand.isValid())
            {
                if (expandHit && pressedExpand == index)
                {
                    QTreeView* view = m_view; // 展开信号可能销毁视图，本调用之后不再访问成员。
                    const bool next = !view->isExpanded(pressedExpand);
                    view->setExpanded(pressedExpand, next);
                    return true;
                }
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, supplied, index);
}

// 视图登记唯一子树复制入口；不复制或解析当前显示的省略文字。
void ks::ui::ComplexReportDelegate::setCopyHandler(std::function<void(const QModelIndex&)> handler)
{
    m_copy = std::move(handler);
}

// 只有宽度、字体或规范结构需要重新测量时由视图调用，颜色刷新不扫描模型。
void ks::ui::ComplexReportDelegate::invalidateMetrics()
{
    m_measurements.clear();
}
