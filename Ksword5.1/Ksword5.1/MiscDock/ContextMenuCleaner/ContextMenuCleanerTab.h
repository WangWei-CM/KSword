#pragma once

// ============================================================
// ContextMenuCleanerTab.h
// 作用：
// 1) 提供“右键菜单清理”杂项页；
// 2) 按 IE、桌面、文件三类 Shell/IE 右键菜单注册表入口分组展示；
// 3) 支持刷新、筛选、复制注册表位置，并在用户确认后删除选中注册表子树。
// ============================================================

#include "../../Framework.h"

#include <QVector>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;

namespace ks::misc
{
    // ContextMenuCleanerTab：
    // - 输入：由 Qt 父控件传入 parent，运行时读取当前系统注册表；
    // - 处理：枚举常见 IE/桌面/文件右键菜单注册表位置，填充三个子页表格；
    // - 输出：本控件不返回值，清理动作通过注册表删除、界面刷新与日志反馈结果。
    class ContextMenuCleanerTab final : public QWidget
    {
    public:
        // 构造函数：
        // - 参数 parent：Qt 父控件，可为空；
        // - 处理逻辑：创建三类子 Tab，并立即执行一次注册表枚举；
        // - 返回值：无。
        explicit ContextMenuCleanerTab(QWidget* parent = nullptr);
        ~ContextMenuCleanerTab() override = default;

    private:
        // MenuArea：标识当前操作的右键菜单分区。
        enum class MenuArea
        {
            InternetExplorer, // IE 右键菜单：Internet Explorer MenuExt。
            Desktop,          // 桌面右键菜单：DesktopBackground/Directory Background。
            File              // 文件右键菜单：*、AllFilesystemObjects、Directory/Folder/Drive。
        };

        // ContextMenuEntry：单条右键菜单注册表项快照。
        struct ContextMenuEntry
        {
            MenuArea area = MenuArea::File;       // area：所属子页。
            HKEY rootKey = nullptr;               // rootKey：真实注册表根键，删除时复用。
            QString rootLabel;                    // rootLabel：显示用根键，如 HKCU/HKLM(64位)。
            QString subKeyPath;                   // subKeyPath：待删除的完整子键路径。
            REGSAM viewFlag = 0;                  // viewFlag：WOW64 视图标记，保证枚举/删除同一视图。
            QString sourceGroup;                  // sourceGroup：来源分类，如“文件 *”或“桌面背景”。
            QString entryKind;                    // entryKind：shell/shellex/IE MenuExt。
            QString itemName;                     // itemName：注册表子键名。
            QString displayName;                  // displayName：菜单显示名，缺失时回退子键名。
            QString commandOrHandler;             // commandOrHandler：命令、脚本路径、CLSID 或 COM Server。
            QString clsidText;                    // clsidText：COM 右键处理器 CLSID。
            QString detailText;                   // detailText：状态标记、Icon、AppliesTo 等补充信息。
            QString statusText;                   // statusText：启用/禁用/扩展菜单等状态。
            bool canDelete = false;               // canDelete：当前行是否允许从 UI 发起删除。
        };

        // AreaWidgets：每个子页拥有一组独立控件与数据缓存。
        struct AreaWidgets
        {
            QWidget* page = nullptr;              // page：子页根控件。
            QVBoxLayout* layout = nullptr;        // layout：子页根布局。
            QWidget* toolbarWidget = nullptr;     // toolbarWidget：刷新/删除/复制/筛选工具栏。
            QPushButton* refreshButton = nullptr; // refreshButton：刷新当前分类按钮。
            QPushButton* deleteButton = nullptr;  // deleteButton：删除选中项按钮。
            QPushButton* copyButton = nullptr;    // copyButton：复制选中注册表路径按钮。
            QLineEdit* filterEdit = nullptr;      // filterEdit：当前分类关键词筛选框。
            QTableWidget* table = nullptr;        // table：右键菜单项列表。
            QLabel* statusLabel = nullptr;        // statusLabel：当前分类统计与提示。
            QVector<ContextMenuEntry> entries;    // entries：当前分类最近一次完整枚举结果。
        };

    private:
        // initializeUi：
        // - 输入：无；
        // - 处理：创建整体布局、说明文本、三类子页；
        // - 返回：无。
        void initializeUi();

        // createAreaPage：
        // - 输入 area：需要创建的右键菜单分区；
        // - 处理：初始化该分区工具栏、表格、状态栏和信号连接；
        // - 返回：无。
        void createAreaPage(MenuArea area);

        // refreshAllAreas：
        // - 输入：无；
        // - 处理：依次刷新 IE、桌面、文件三个分区；
        // - 返回：无。
        void refreshAllAreas();

        // refreshArea：
        // - 输入 area：目标分区；
        // - 处理：重新枚举该分区注册表项并重建表格；
        // - 返回：无。
        void refreshArea(MenuArea area);

        // rebuildAreaTable：
        // - 输入 area：目标分区；
        // - 处理：按当前筛选文本把缓存 entries 渲染到表格；
        // - 返回：无。
        void rebuildAreaTable(MenuArea area);

        // showAreaContextMenu：
        // - 输入 area：目标分区；localPosition：表格视口内右键坐标；
        // - 处理：弹出复制/删除右键菜单，并执行用户选择；
        // - 返回：无。
        void showAreaContextMenu(MenuArea area, const QPoint& localPosition);

        // deleteSelectedEntries：
        // - 输入 area：目标分区；
        // - 处理：收集表格选中行，确认后删除对应注册表子树并刷新；
        // - 返回：无，失败信息通过 QMessageBox 与日志反馈。
        void deleteSelectedEntries(MenuArea area);

        // copySelectedEntries：
        // - 输入 area：目标分区；
        // - 处理：把选中行的注册表路径复制到剪贴板；
        // - 返回：无。
        void copySelectedEntries(MenuArea area) const;

        // enumerateEntriesForArea：
        // - 输入 area：目标分区；
        // - 处理：按内置注册表目录清单枚举右键菜单项；
        // - 返回：该分区当前可见的注册表项快照。
        QVector<ContextMenuEntry> enumerateEntriesForArea(MenuArea area) const;

        // selectedEntryIndexes：
        // - 输入 area：目标分区；
        // - 处理：读取表格选中行绑定的 entries 下标，自动去重；
        // - 返回：选中的 entries 下标数组。
        QVector<int> selectedEntryIndexes(MenuArea area) const;

        // widgetsForArea：
        // - 输入 area：目标分区；
        // - 处理：返回该分区控件组指针；
        // - 返回：AreaWidgets 指针，area 有效时永不为空。
        AreaWidgets* widgetsForArea(MenuArea area);
        const AreaWidgets* widgetsForArea(MenuArea area) const;

        // areaTitle：
        // - 输入 area：目标分区；
        // - 处理：转换为中文页签标题；
        // - 返回：用于 Tab 与日志的标题文本。
        static QString areaTitle(MenuArea area);

        // areaIconPath：
        // - 输入 area：目标分区；
        // - 处理：按分区选择现有 qrc 图标；
        // - 返回：图标资源路径。
        static QString areaIconPath(MenuArea area);

    private:
        QVBoxLayout* m_rootLayout = nullptr;   // m_rootLayout：本页根布局。
        QLabel* m_hintLabel = nullptr;         // m_hintLabel：注册表清理风险说明。
        QTabWidget* m_areaTabWidget = nullptr; // m_areaTabWidget：IE/桌面/文件三个子 Tab 容器。
        AreaWidgets m_ieWidgets;               // m_ieWidgets：IE 右键菜单子页控件组。
        AreaWidgets m_desktopWidgets;          // m_desktopWidgets：桌面右键菜单子页控件组。
        AreaWidgets m_fileWidgets;             // m_fileWidgets：文件右键菜单子页控件组。
    };
}
