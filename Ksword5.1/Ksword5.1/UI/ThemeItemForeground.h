#pragma once

#include <Qt> // Qt::UserRole：语义标签使用业务角色之外的专用数据槽。

class QAbstractItemView;
class QTableWidgetItem;
class QTreeWidgetItem;
class QModelIndex;
class QStyleOptionViewItem;

namespace ks::ui
{
    // ItemForegroundRole 保存颜色的用途，切主题后从当前 token 重新求色，不猜测旧 RGB。
    enum class ItemForegroundRole
    {
        Accent = 1,
        Secondary,
        Info,
        Warning,
        Error
    };

    // kThemeItemForegroundRole 专用于本模块；不占用各表格已使用的 UserRole/+1 等业务字段。
    inline constexpr int kThemeItemForegroundRole = Qt::UserRole + 4095;

    // ApplyThemeItemForeground 标记表格项并立即取当前语义色；item 非拥有，role 表示展示用途。
    void ApplyThemeItemForeground(QTableWidgetItem* item, ItemForegroundRole role);

    // ApplyThemeItemForeground 标记树节点的指定列；不修改节点文本、层级或业务缓存索引。
    void ApplyThemeItemForeground(QTreeWidgetItem* item, int column, ItemForegroundRole role);

    // 已安装本组件委托时仅请求绘制，按真实可见行取主题；其它委托兼容刷新专属角色画刷。
    // 不重建模型、筛选、选择或业务数据。
    // 调用者在 palette 传播完成后排队调用；view 为非拥有指针，空指针安全透传。
    void RefreshThemeItemForegrounds(QAbstractItemView* view);

    // ApplyThemeItemForegroundToStyleOption 按真实 Alternate/Selected 绘制底求前景，不猜视觉行号。
    // option 为当前绘制选项，index 提供专属语义；未标记项完全不改动。
    void ApplyThemeItemForegroundToStyleOption(QStyleOptionViewItem* option, const QModelIndex& index);

    // InstallThemeItemForegroundDelegate 在业务视图创建时安装默认 Qt 绘制的语义取色扩展。
    // 不改变编辑/尺寸/选择行为；调用者已确认该视图没有其它自定义 delegate。
    void InstallThemeItemForegroundDelegate(QAbstractItemView* view);
}
