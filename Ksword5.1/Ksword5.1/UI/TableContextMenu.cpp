#include "./TableContextMenu.h"

#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QIcon>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTableView>
#include <QVariant>

#include <utility>

namespace
{
    // 标记属于业务动作的能力，以及仅由共享层维护的临时动作/分隔线。
    constexpr char kRoleProperty[] = "ksword_table_context_action_role";
    constexpr char kGeneratedProperty[] = "ksword_table_context_action_generated";

    // 输入菜单和角色；返回业务菜单（含子菜单）是否已经提供该能力。
    // 禁用项仍保留业务依赖语义，隐藏项则不阻止共享层提供入口。
    bool hasBusinessAction(const QMenu* menu, const ks::ui::TableContextActionRole role)
    {
        for (const QAction* action : menu->actions())
        {
            if (!action->isVisible() || action->property(kGeneratedProperty).toBool())
            {
                continue;
            }
            if (action->property(kRoleProperty).toInt() == static_cast<int>(role))
            {
                return true;
            }
            // 只遍历当前菜单的动作树，不扫描页面或预先创建隐藏菜单。
            if (action->menu() != nullptr && hasBusinessAction(action->menu(), role))
            {
                return true;
            }
        }
        return false;
    }

    // 输入菜单；删除上次显示时生成的动作，业务动作和回调保持原有身份。
    void clearGeneratedActions(QMenu* menu)
    {
        const QList<QAction*> previousActions = menu->actions(); // 保留删除前的动作列表。
        for (QAction* action : previousActions)
        {
            if (action->property(kGeneratedProperty).toBool())
            {
                menu->removeAction(action);
                delete action;
            }
        }
    }

    // 输入菜单、图标和文本；返回已标记所有权的新动作，供调用方绑定回调。
    QAction* addSharedAction(QMenu* menu, const char* iconPath, const char* sourceText)
    {
        QAction* action = menu->addAction( // 共享复制/导出动作沿用现有主题图标和双语文本。
            QIcon(QString::fromUtf8(iconPath)),
            ks::i18n::sourceText(QString::fromUtf8(sourceText)));
        action->setProperty(kGeneratedProperty, true);
        return action;
    }
}

namespace ks::ui
{
    void SetTableContextActionRole(QAction* action, const TableContextActionRole role)
    {
        if (action != nullptr)
        {
            action->setProperty(kRoleProperty, static_cast<int>(role));
        }
    }

    void PopulateTableContextMenu(
        QMenu* menu,
        QTableView* table,
        const QModelIndex& clickedIndex,
        const bool hasRows,
        std::function<void()> copyRows,
        std::function<void()> exportRows)
    {
        if (menu == nullptr || table == nullptr)
        {
            return;
        }
        clearGeneratedActions(menu);

        // 三种能力分别判定，已有专用复制行不会同时吞掉缺失的单元格复制或导出。
        const bool needsCell = !hasBusinessAction(menu, TableContextActionRole::CopyCell);
        const bool needsRows = !hasBusinessAction(menu, TableContextActionRole::CopyRows);
        const bool needsExport = !hasBusinessAction(menu, TableContextActionRole::ExportRows);
        if (!needsCell && !needsRows && !needsExport)
        {
            return;
        }

        // 共享新增菜单必须有不透明底和完整状态；已有业务样式继续由原拥有者管理。
        if (menu->styleSheet().trimmed().isEmpty())
        {
            menu->setStyleSheet(KswordTheme::ContextMenuStyle());
        }
        if (!menu->actions().isEmpty() && !menu->actions().last()->isSeparator())
        {
            QAction* separator = menu->addSeparator(); // 仅在两组动作之间添加一条分隔线。
            separator->setProperty(kGeneratedProperty, true);
        }

        if (needsCell)
        {
            // 捕获右键位置而非 currentIndex，保留代理模型/排序后的真实单元格身份。
            const QPointer<QTableView> guardedTable(table); // 菜单打开时来源表可能被销毁。
            const QPersistentModelIndex guardedCell(clickedIndex); // 模型重置时自动失效。
            QAction* cellAction = addSharedAction(menu, ":/Icon/process_copy_cell.svg", "复制单元格");
            cellAction->setObjectName(QStringLiteral("ksword_table_copy_cell_action"));
            cellAction->setEnabled(clickedIndex.isValid() && clickedIndex.model() == table->model());
            QObject::connect(cellAction, &QAction::triggered, menu, [guardedTable, guardedCell]()
            {
                if (guardedTable.isNull() || !guardedCell.isValid()
                    || guardedCell.model() != guardedTable->model())
                {
                    return;
                }
                // 只复制显示值，保留原有换行/制表符；有效空单元格允许清空剪贴板。
                if (QClipboard* clipboard = QApplication::clipboard())
                {
                    clipboard->setText(guardedCell.data(Qt::DisplayRole).toString());
                }
            });
        }

        // 行复制和导出仍调用原共享实现，保留选区、筛选、冻结和 TSV 语义。
        if (needsRows)
        {
            QAction* rowAction = addSharedAction(menu, ":/Icon/log_copy.svg", "复制选中行（TSV）");
            rowAction->setEnabled(hasRows && static_cast<bool>(copyRows));
            QObject::connect(rowAction, &QAction::triggered, menu, [copyRows = std::move(copyRows)]()
            {
                if (copyRows)
                {
                    copyRows();
                }
            });
        }
        if (needsExport)
        {
            QAction* exportAction = addSharedAction(menu, ":/Icon/log_export.svg", "导出选中行（TSV）");
            exportAction->setEnabled(hasRows && static_cast<bool>(exportRows));
            QObject::connect(exportAction, &QAction::triggered, menu, [exportRows = std::move(exportRows)]()
            {
                if (exportRows)
                {
                    exportRows();
                }
            });
        }
    }
}
