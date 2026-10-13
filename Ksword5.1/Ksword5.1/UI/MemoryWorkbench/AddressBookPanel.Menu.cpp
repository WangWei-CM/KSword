// AddressBookPanel.Menu.cpp
// 作用：AddressBookPanel 的两个右键菜单——表格行上的业务菜单（跳转/反汇编/升级/值类型/
// 备注/复制/删除/清空搜索结果）与表头上的列显隐菜单。两个菜单都显式设置
// KswordTheme::ContextMenuStyle()（背景/文字/选中态/禁用态），不依赖默认样式——本仓库的
// QMenu 默认样式在某些页面会继承透明背景，浅色模式下可能黑底黑字（见 AGENTS.md）。

#include "AddressBookPanel.h"
#include "../TableContextMenu.h"

#include "AddressBookModel.h"

#include "../../theme.h"
#include "../../Internationalization/LanguageManager.h"

#include <QAction>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>

#include <array>
#include <algorithm>
#include <optional>

namespace ks::ui
{
    namespace
    {
        // kValueTypeOrder：值类型子菜单的固定顺序，与 ksword::memwb::ValueType 的声明顺序一致。
        constexpr std::array<ksword::memwb::ValueType, 11> kValueTypeOrder{
            ksword::memwb::ValueType::Hex8, ksword::memwb::ValueType::U8, ksword::memwb::ValueType::U16,
            ksword::memwb::ValueType::U32, ksword::memwb::ValueType::U64, ksword::memwb::ValueType::I8,
            ksword::memwb::ValueType::I16, ksword::memwb::ValueType::I32, ksword::memwb::ValueType::I64,
            ksword::memwb::ValueType::F32, ksword::memwb::ValueType::F64
        };
    }

    void AddressBookPanel::showRowContextMenu(const QPoint& viewportPos)
    {
        if (m_model.isNull() || m_view == nullptr)  // 修复可疑点 #3：model 可能先于本面板销毁。
        {
            return;
        }
        const QModelIndex proxyIndexAtPos = m_view->indexAt(viewportPos);

        // 右键一个尚未被选中的行：先让它成为唯一的当前选区，这是大多数表格一致的交互，
        // 否则菜单会对着"旧的选区"生效，用户点中的行反而没被操作到。右键空白处
        // （proxyIndexAtPos 无效）时不改变既有选区。
        if (proxyIndexAtPos.isValid() && !m_view->selectionModel()->isSelected(proxyIndexAtPos))
        {
            m_view->setCurrentIndex(proxyIndexAtPos);
            m_view->selectionModel()->select(
                proxyIndexAtPos,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        else if (proxyIndexAtPos.isValid())
        {
            // 修复 D10：被右键的这一行本来就在多选范围内——不改变整个选区（"加入书签/
            // 删除"这类按整个选区生效的动作要继续作用于全部已选行），但要把当前格挪到
            // 被右键的这一行上。targetRowId() 在多选时的规则是"当前格落在选区内就是它"，
            // 原来当前格停在别处（例如之前只移动过方向键焦点，从没真正点过这一行）时，
            // "跳转/复制地址/编辑备注"这几个按单行生效的动作会落在当前格而不是被右键的
            // 行上——与用户"我右键的就是这一行"的直觉不一致。
            // 注意：这里必须直接调用 selectionModel()->setCurrentIndex(index, NoUpdate)，
            // 不能用 QAbstractItemView::setCurrentIndex(index) 这个便捷重载——后者在没有
            // 原始鼠标/键盘事件可查时会按 ExtendedSelection 的默认选择命令处理当前格切换，
            // 对"目标行已经被选中"的情况会把它 Toggle 掉（反而取消选中），实测会把选区从
            // 3 条变成 2 条，与"不改变整个选区"的意图正好相反。
            m_view->selectionModel()->setCurrentIndex(proxyIndexAtPos, QItemSelectionModel::NoUpdate);
        }

        const std::vector<std::uint64_t> selection = selectedIds();
        const bool hasSelection = !selection.empty();
        const bool singleSelection = selection.size() == 1;

        // currentKind / currentValueType：仅单选时有意义，决定"加入书签/监视"该隐藏哪一项、
        // "值类型▸"子菜单该勾选哪一项。多选时这两个 optional 保持 nullopt，子菜单不勾选
        // 任何一项（多选时的当前值本就不唯一）。
        std::optional<ksword::memwb::EntryKind> currentKind;
        std::optional<ksword::memwb::ValueType> currentValueType;
        if (singleSelection)
        {
            const QModelIndex sourceIndex = m_model->indexForId(selection.front(), AddressBookModel::ColumnKindIcon);
            if (sourceIndex.isValid())
            {
                currentKind = static_cast<ksword::memwb::EntryKind>(
                    m_model->data(sourceIndex, AddressBookModel::KindRole).toInt());
                currentValueType = static_cast<ksword::memwb::ValueType>(
                    m_model->data(sourceIndex, AddressBookModel::ValueTypeRole).toInt());
            }
        }

        // 修复 C7：菜单的使能判据改成与动作实际作用的对象一致——targetRowId()（选区恰好
        // 一条就是它，否则要求当前格落在选区内）。之前这几项按 singleSelection 使能，但
        // 跳转/反汇编/F2/复制实际读的是"当前格"，两者在"多选且当前格不在选区内"时会
        // 不一致：菜单显示可点，点了却作用在一条没被选中的行上。
        const std::uint64_t targetId = targetRowId();
        const bool hasTarget = targetId != 0;
        const bool targetIsPointerChain = hasTarget && m_model->isPointerChain(targetId);
        const bool selectionHasPointerChain = std::any_of(selection.begin(), selection.end(),
            [this](const std::uint64_t id) { return m_model->isPointerChain(id); });
        const bool targetValueIsRead =
            hasTarget && (m_model->valueState(targetId) == AddressBookModel::ValueState::Read);

        QMenu menu(this);
        menu.setStyleSheet(KswordTheme::ContextMenuStyle());
        // 悬停释义依赖这个开关才会显示（AGENTS.md 要求悬停有释义；"复制值"被置灰时正是
        // 靠这条 tooltip 告诉用户为什么点不动，见下）。
        menu.setToolTipsVisible(true);

        if (targetIsPointerChain)
        {
            QAction* const editChainAction = menu.addAction(
                ks::i18n::sourceText(QStringLiteral("编辑指针链")));
            connect(editChainAction, &QAction::triggered, this,
                [this, targetId]() { editPointerChainById(targetId); });
            QAction* const resolveChainAction = menu.addAction(
                ks::i18n::sourceText(QStringLiteral("解析指针链")));
            connect(resolveChainAction, &QAction::triggered, this,
                [this, targetId]() { resolvePointerChainById(targetId); });
            menu.addSeparator();
        }

        QAction* const jumpAction = menu.addAction(QStringLiteral("跳转"));
        jumpAction->setEnabled(hasTarget);
        connect(jumpAction, &QAction::triggered, this, &AddressBookPanel::jumpCurrentRow);

        QAction* const disasmAction = menu.addAction(QStringLiteral("在反汇编打开"));
        disasmAction->setEnabled(hasTarget);
        connect(disasmAction, &QAction::triggered, this, &AddressBookPanel::openDisassemblyCurrentRow);

        menu.addSeparator();

        // 升级动作：从不提供"降回搜索"，这不是靠 enabled 判断关闭的，而是菜单本身压根不
        // 加这一项——与 AddressBookStore::promote 的硬性拒绝是双重保险，不是互相替代。
        if (!currentKind.has_value() || *currentKind != ksword::memwb::EntryKind::Bookmark)
        {
            QAction* const bookmarkAction = menu.addAction(QStringLiteral("加入书签"));
            bookmarkAction->setEnabled(hasSelection);
            connect(bookmarkAction, &QAction::triggered, this,
                [this]() { promoteSelection(ksword::memwb::EntryKind::Bookmark); });
        }
        if (!selectionHasPointerChain
            && (!currentKind.has_value() || *currentKind != ksword::memwb::EntryKind::Watch))
        {
            QAction* const watchAction = menu.addAction(QStringLiteral("加入监视"));
            watchAction->setEnabled(hasSelection);
            connect(watchAction, &QAction::triggered, this,
                [this]() { promoteSelection(ksword::memwb::EntryKind::Watch); });
        }

        QMenu* const valueTypeMenu = menu.addMenu(QStringLiteral("值类型"));
        valueTypeMenu->setStyleSheet(KswordTheme::ContextMenuStyle());
        valueTypeMenu->setEnabled(hasSelection);
        for (const ksword::memwb::ValueType candidate : kValueTypeOrder)
        {
            QAction* const typeAction = valueTypeMenu->addAction(AddressBookModel::ValueTypeDisplayName(candidate));
            typeAction->setCheckable(true);
            typeAction->setChecked(currentValueType.has_value() && *currentValueType == candidate);
            connect(typeAction, &QAction::triggered, this,
                [this, candidate]() { setValueTypeForSelection(candidate); });
        }

        QAction* const noteAction = menu.addAction(QStringLiteral("编辑备注 (F2)"));
        noteAction->setEnabled(hasTarget);
        connect(noteAction, &QAction::triggered, this, &AddressBookPanel::editNoteCurrentRow);

        menu.addSeparator();

        QAction* const copyAddressAction = menu.addAction(QStringLiteral("复制地址"));
        copyAddressAction->setEnabled(hasTarget);
        connect(copyAddressAction, &QAction::triggered, this,
            [this]() { copyCurrentRowField(CopyField::Address); });

        QAction* const copyModuleOffsetAction = menu.addAction(QStringLiteral("复制模块+偏移"));
        copyModuleOffsetAction->setEnabled(hasTarget);
        connect(copyModuleOffsetAction, &QAction::triggered, this,
            [this]() { copyCurrentRowField(CopyField::ModuleOffset); });

        // 修复可疑点 #4：只有目标确定且值已经处于 Read 状态才允许"复制值"——占位符/过期
        // 文本没有复制的意义；禁用时给出 tooltip 说明原因（上面 setToolTipsVisible(true)
        // 让它真的显示出来）。
        QAction* const copyValueAction = menu.addAction(QStringLiteral("复制值"));
        copyValueAction->setEnabled(targetValueIsRead);
        if (!targetValueIsRead)
        {
            copyValueAction->setToolTip(
                ks::i18n::sourceText(QStringLiteral("该值尚未成功读取，暂时无法复制")));
        }
        connect(copyValueAction, &QAction::triggered, this,
            [this]() { copyCurrentRowField(CopyField::Value); });

        QAction* const copyRowAction = menu.addAction(QStringLiteral("复制行 (Ctrl+C)"));
        // 地址簿保留专业行格式，共享菜单只补单元格复制与导出。
        ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
        copyRowAction->setEnabled(hasTarget);
        connect(copyRowAction, &QAction::triggered, this,
            [this]() { copyCurrentRowField(CopyField::Row); });

        menu.addSeparator();

        QAction* const deleteAction = menu.addAction(QStringLiteral("删除 (Del)"));
        deleteAction->setEnabled(hasSelection);
        connect(deleteAction, &QAction::triggered, this, &AddressBookPanel::removeSelection);

        QAction* const clearSearchAction = menu.addAction(QStringLiteral("清空搜索结果"));
        clearSearchAction->setEnabled(m_model->kindCounts().search > 0);
        connect(clearSearchAction, &QAction::triggered, this, &AddressBookPanel::clearSearchResultsRequested);

        menu.exec(m_view->viewport()->mapToGlobal(viewportPos));
    }

    void AddressBookPanel::showHeaderContextMenu(const QPoint& headerPos)
    {
        if (m_view == nullptr)  // 修复可疑点 #3：防御性判空（本函数不直接用 m_model）。
        {
            return;
        }
        // kHeaderNames：与 AddressBookModel::headerData 的文案保持一致，下标即物理列下标。
        static const std::array<QString, static_cast<std::size_t>(AddressBookModel::ColumnCount)> kHeaderNames{
            QStringLiteral("类型"), QStringLiteral("地址"), QStringLiteral("值"), QStringLiteral("值类型"),
            QStringLiteral("备注"), QStringLiteral("模块+RVA"), QStringLiteral("目标")
        };

        QMenu menu(m_view);
        menu.setStyleSheet(KswordTheme::ContextMenuStyle());
        for (int column = 0; column < static_cast<int>(AddressBookModel::ColumnCount); ++column)
        {
            QAction* const columnAction = menu.addAction(kHeaderNames[static_cast<std::size_t>(column)]);
            columnAction->setCheckable(true);
            columnAction->setChecked(!m_view->isColumnHidden(column));
            connect(columnAction, &QAction::toggled, &menu, [this, column](const bool visible) {
                m_view->setColumnHidden(column, !visible);
                // 用户手动勾/取消勾选表头某一列：不再认为当前是 A 或 B 预设，哪怕勾出来的
                // 结果恰好等于某个预设——这与 applyColumnVisibility 程序化改列显隐时置的
                // m_applyingColumnGroup 保护是同一条规则的两面：那边改列不算"用户手动"，
                // 这里改列才算。
                if (!m_applyingColumnGroup)
                {
                    m_columnGroup = ColumnGroup::Custom;
                    updateColumnGroupButtons();
                }
            });
        }
        menu.exec(m_view->horizontalHeader()->mapToGlobal(headerPos));
    }
}
