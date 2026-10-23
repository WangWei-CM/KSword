#include "EmbeddedRowDelegate.h"
#include "EmbeddedCellLayout.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QIcon>
#include <QStyle>
#include <QWidget>
#include <QEvent>
#include <QHelpEvent>
#include <QPainter>

namespace ks::ui
{
    EmbeddedRowDelegate::EmbeddedRowDelegate(
        QAbstractItemDelegate* sourceDelegate,
        OriginalHeightProvider originalHeightProvider,
        QObject* parent)
        : QAbstractItemDelegate(parent),
          m_sourceDelegate(sourceDelegate),
          m_originalHeightProvider(std::move(originalHeightProvider))
    {
        // 包装器复用原 delegate 的编辑器信号，保证双击编辑等页面行为不改变。
        if (m_sourceDelegate != nullptr)
        {
            connect(
                m_sourceDelegate.data(),
                &QAbstractItemDelegate::closeEditor,
                this,
                &QAbstractItemDelegate::closeEditor);
            connect(
                m_sourceDelegate.data(),
                &QAbstractItemDelegate::commitData,
                this,
                &QAbstractItemDelegate::commitData);
            connect(
                m_sourceDelegate.data(),
                &QAbstractItemDelegate::sizeHintChanged,
                this,
                &QAbstractItemDelegate::sizeHintChanged);
        }
    }

    QStyleOptionViewItem EmbeddedRowDelegate::sourceOption(
        const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        QStyleOptionViewItem result(option);
        const int originalHeight = m_originalHeightProvider ? m_originalHeightProvider(index) : -1;
        result.rect = LayoutEmbeddedCell(option.rect, originalHeight, index.column() == 0,
            option.direction == Qt::RightToLeft).source;
        return result;
    }

    void EmbeddedRowDelegate::paint(QPainter* painter,
        const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        if (!painter || m_sourceDelegate.isNull()) return;
        const int originalHeight = m_originalHeightProvider ? m_originalHeightProvider(index) : -1;
        const auto layout = LayoutEmbeddedCell(option.rect, originalHeight, index.column() == 0,
            option.direction == Qt::RightToLeft);
        if (layout.band.isEmpty()) return;
        QStyleOptionViewItem clipped(option);
        clipped.rect = layout.band;
        painter->save();
        painter->setClipRect(layout.band, Qt::IntersectClip);
        const QStyle* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &clipped, painter, option.widget);
        clipped.rect = layout.source;
        m_sourceDelegate->paint(painter, clipped, index); // original DecorationRole remains intact
        if (!layout.indicator.isEmpty())
        {
            static const QIcon collapsed(QStringLiteral(":/Icon/detail_node_collapsed.svg"));
            static const QIcon expanded(QStringLiteral(":/Icon/detail_node_expanded.svg"));
            const QIcon& icon = originalHeight > 0 ? expanded : collapsed;
            const auto mode = !(option.state & QStyle::State_Enabled) ? QIcon::Disabled
                : option.state & QStyle::State_Selected ? QIcon::Selected : QIcon::Normal;
            icon.paint(painter, layout.indicator, Qt::AlignCenter, mode);
        }
        painter->restore();
    }

    QSize EmbeddedRowDelegate::sizeHint(
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const
    {
        // 尺寸计算直接转发，展开行高由 QTableWidget/QTreeWidget 原有接口控制。
        return m_sourceDelegate.isNull()
            ? QSize()
            : m_sourceDelegate->sizeHint(option, index) + QSize(index.column() == 0 ? 20 : 0,0);
    }

    QWidget* EmbeddedRowDelegate::createEditor(
        QWidget* parent,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const
    {
        return m_sourceDelegate.isNull()
            ? nullptr
            : m_sourceDelegate->createEditor(parent, sourceOption(option,index), index);
    }

    void EmbeddedRowDelegate::destroyEditor(QWidget* editor, const QModelIndex& index) const
    {
        if (!m_sourceDelegate.isNull())
        {
            m_sourceDelegate->destroyEditor(editor, index);
        }
    }

    void EmbeddedRowDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
    {
        if (!m_sourceDelegate.isNull())
        {
            m_sourceDelegate->setEditorData(editor, index);
        }
    }

    void EmbeddedRowDelegate::setModelData(
        QWidget* editor,
        QAbstractItemModel* model,
        const QModelIndex& index) const
    {
        if (!m_sourceDelegate.isNull())
        {
            m_sourceDelegate->setModelData(editor, model, index);
        }
    }

    void EmbeddedRowDelegate::updateEditorGeometry(
        QWidget* editor,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const
    {
        if (!m_sourceDelegate.isNull())
        {
            m_sourceDelegate->updateEditorGeometry(editor, sourceOption(option,index), index);
        }
    }

    bool EmbeddedRowDelegate::editorEvent(
        QEvent* event,
        QAbstractItemModel* model,
        const QStyleOptionViewItem& option,
        const QModelIndex& index)
    {
        // 事件区域仍交给原 delegate，避免复用委托后改变复选框等交互。
        return !m_sourceDelegate.isNull()
            && m_sourceDelegate->editorEvent(event, model, sourceOption(option,index), index);
    }

    bool EmbeddedRowDelegate::helpEvent(
        QHelpEvent* event,
        QAbstractItemView* view,
        const QStyleOptionViewItem& option,
        const QModelIndex& index)
    {
        // 工具提示与原 delegate 保持一致，行内模式只影响绘制几何。
        return !m_sourceDelegate.isNull()
            && m_sourceDelegate->helpEvent(event, view, sourceOption(option,index), index);
    }
}
