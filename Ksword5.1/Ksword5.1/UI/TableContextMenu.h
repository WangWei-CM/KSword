#pragma once

#include <QModelIndex>
#include <functional>

class QAction;
class QMenu;
class QTableView;

namespace ks::ui
{
    // 业务菜单声明已提供的能力；子菜单同样生效，判定不依赖语言或动作是否禁用。
    enum class TableContextActionRole
    {
        CopyCell = 1,
        CopyRows,
        ExportRows
    };

    // 输入：业务 QAction 与其复制/导出角色；输出：无。
    // 调用：创建动作后、显示菜单前登记，阻止共享层追加同类动作。
    void SetTableContextActionRole(QAction* action, TableContextActionRole role);

    // 输入：菜单、来源表、右键命中的单元格、行可用性及原有复制/导出回调；输出：无。
    // 调用：每次显示前只补缺失能力，复用菜单时替换上次生成的动作与来源。
    // clickedIndex 无效表示点在空白处；不能回退到先前选中的单元格。
    void PopulateTableContextMenu(
        QMenu* menu,
        QTableView* table,
        const QModelIndex& clickedIndex,
        bool hasRows,
        std::function<void()> copyRows,
        std::function<void()> exportRows);
}
