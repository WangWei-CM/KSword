#include "ThemeItemForeground.h"
#include "../theme.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QBrush>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTableWidgetItem>
#include <QTreeWidgetItem>
#include <QVector>

namespace
{
    // foregroundColor 读取当前主题；未知标签返回无效色，保留外部模块或数据原色。
    QColor preferredForegroundColor(const int roleValue)
    {
        QColor preferredColor; // 先保留语义色相，再共同校准表面/交替行两种背景。
        switch (static_cast<ks::ui::ItemForegroundRole>(roleValue))
        {
        case ks::ui::ItemForegroundRole::Accent:
            preferredColor = KswordTheme::PrimaryAccentColor();
            break;
        case ks::ui::ItemForegroundRole::Secondary:
            preferredColor = KswordTheme::TextSecondaryColor();
            break;
        case ks::ui::ItemForegroundRole::Info:
            preferredColor = KswordTheme::InfoColor();
            break;
        case ks::ui::ItemForegroundRole::Warning:
            preferredColor = KswordTheme::WarningAccentColor();
            break;
        case ks::ui::ItemForegroundRole::Error:
            preferredColor = KswordTheme::ErrorColor();
            break;
        }
        return preferredColor;
    }

    // foregroundColor 保留模型画刷的共同最佳色；真实绘制选项会按当前行底再次校准。
    QColor foregroundColor(const int roleValue)
    {
        const QColor preferredColor = preferredForegroundColor(roleValue);
        if (!preferredColor.isValid())
        {
            return QColor();
        }
        const QColor backgrounds[] = {KswordTheme::SurfaceColor(), KswordTheme::SurfaceAltColor()};
        return KswordTheme::EnsureTextContrastForBackgrounds(preferredColor, backgrounds, 2, 4.5);
    }

    // SemanticForegroundDelegate 只扩展默认 Qt 的前景选项，保留原有几何、编辑和选中绘制。
    class SemanticForegroundDelegate final : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

    protected:
        void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
        {
            QStyledItemDelegate::initStyleOption(option, index);
            ks::ui::ApplyThemeItemForegroundToStyleOption(option, index);
        }
    };
}

namespace ks::ui
{
    void ApplyThemeItemForegroundToStyleOption(
        QStyleOptionViewItem* option, const QModelIndex& index)
    {
        if (option == nullptr)
        {
            return;
        }
        const QVariant role = index.data(kThemeItemForegroundRole);
        const QColor preferredColor = role.isValid() ? preferredForegroundColor(role.toInt()) : QColor();
        if (!preferredColor.isValid())
        {
            return;
        }
        // Alternate 来自真实 view 绘制选项；树展开/隐藏或排序后也不会猜错视觉交替行。
        const bool selected = option->state.testFlag(QStyle::State_Selected);
        const QColor background = selected ? option->palette.color(QPalette::Highlight)
            : option->features.testFlag(QStyleOptionViewItem::Alternate)
                ? option->palette.color(QPalette::AlternateBase) : option->palette.color(QPalette::Base);
        const QColor foreground = KswordTheme::EnsureTextContrast(preferredColor, background, 4.5);
        option->palette.setColor(QPalette::Text, foreground);
        option->palette.setColor(QPalette::HighlightedText, foreground);
    }

    void InstallThemeItemForegroundDelegate(QAbstractItemView* view)
    {
        if (view != nullptr && dynamic_cast<SemanticForegroundDelegate*>(view->itemDelegate()) == nullptr)
        {
            view->setItemDelegate(new SemanticForegroundDelegate(view));
        }
    }

    void ApplyThemeItemForeground(QTableWidgetItem* item, const ItemForegroundRole role)
    {
        if (item != nullptr)
        {
            item->setData(kThemeItemForegroundRole, static_cast<int>(role));
            item->setForeground(QBrush(foregroundColor(static_cast<int>(role))));
        }
    }

    void ApplyThemeItemForeground(
        QTreeWidgetItem* item, const int column, const ItemForegroundRole role)
    {
        if (item != nullptr)
        {
            item->setData(column, kThemeItemForegroundRole, static_cast<int>(role));
            item->setForeground(column, QBrush(foregroundColor(static_cast<int>(role))));
        }
    }

    void RefreshThemeItemForegrounds(QAbstractItemView* view)
    {
        if (view == nullptr || view->model() == nullptr)
        {
            return;
        }

        // 已安装本组件委托时，语义role在真正绘制的行上按当前主题求值。
        // 不再为每次Palette广播扫描整个事件表/命名空间树并回写展示画刷。
        // 冻结窗格复用源委托和模型；非本组件的委托继续走下面的兼容刷新。
        if (dynamic_cast<SemanticForegroundDelegate*>(view->itemDelegate()) != nullptr)
        {
            if (view->viewport() != nullptr)
            {
                view->viewport()->update();
            }
            return;
        }

        // model 为原模型；阻断颜色写入信号，防止 itemChanged/排序/搜索监听误判为业务数据变化。
        QAbstractItemModel* const model = view->model();
        {
            const QSignalBlocker modelSignals(model);
            // pendingParents 迭代遍历树的已有层级，避免深层命名空间导致递归栈增长。
            QVector<QModelIndex> pendingParents{QModelIndex()};
            while (!pendingParents.isEmpty())
            {
                const QModelIndex parent = pendingParents.takeLast();
                const int rowCount = model->rowCount(parent); // 当前层已有行数，不 fetchMore 或枚举。
                const int columnCount = model->columnCount(parent); // 保留当前列布局和隐藏状态。
                for (int row = 0; row < rowCount; ++row)
                {
                    for (int column = 0; column < columnCount; ++column)
                    {
                        const QModelIndex index = model->index(row, column, parent);
                        const QVariant role = model->data(index, kThemeItemForegroundRole);
                        if (!role.isValid())
                        {
                            continue;
                        }
                        const QColor color = foregroundColor(role.toInt()); // 仅专属语义标签参与刷新。
                        if (color.isValid())
                        {
                            model->setData(index, QBrush(color), Qt::ForegroundRole);
                        }
                    }
                    const QModelIndex childParent = model->index(row, 0, parent);
                    if (model->hasChildren(childParent))
                    {
                        pendingParents.push_back(childParent);
                    }
                }
            }
        }
        // model 信号恢复后仅重绘 viewport；不会重设当前行、滚动位置或展开/隐藏状态。
        if (view->viewport() != nullptr)
        {
            view->viewport()->update();
        }
    }
}
