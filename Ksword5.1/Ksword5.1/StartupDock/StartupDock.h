#pragma once

// ============================================================
// StartupDock.h
// 作用：
// 1) 提供类似 Autoruns 的“启动项”集中管理入口；
// 2) 统一展示登录项、服务、驱动、计划任务与高级注册表持久化项；
// 3) 提供刷新、导出、复制、打开文件位置/注册表位置等操作。
// ============================================================

#include "../Framework.h"

#include <QHash>
#include <QIcon>
#include <QWidget>

#include <atomic>   // std::atomic_bool：后台刷新运行状态。
#include <cstdint>  // std::uint32_t：PID、状态值与标识字段。
#include <memory>   // std::unique_ptr：后台线程对象托管。
#include <thread>   // std::thread：后台枚举线程。
#include <vector>   // std::vector：启动项缓存与分页表格重建。

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTabWidget;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class StartupDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 UI、连接交互并执行首轮刷新。
    // - 参数 parent：Qt 父控件。
    explicit StartupDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止刷新定时器并释放资源。
    ~StartupDock() override;

protected:
    // showEvent：
    // - 作用：在页签首次真正显示时再触发首轮枚举；
    // - 避免主窗口启动阶段被启动项全量扫描阻塞。
    void showEvent(QShowEvent* event) override;

public:
    // StartupCategory：
    // - 作用：定义启动项所属大类，供总览与分标签过滤复用。
    enum class StartupCategory : int
    {
        All = 0,       // 总览。
        Logon,         // 登录项（Run/RunOnce/Startup Folder）。
        Services,      // 自动启动服务。
        Drivers,       // 自动/系统/引导驱动。
        Tasks,         // 计划任务。
        Registry,      // 高级注册表持久化项。
        Wmi            // WMI 持久化项。
    };

    // StartupEntry：
    // - 作用：统一承载一条启动项记录；
    // - 供所有枚举器输出和所有表格渲染复用。
    struct StartupEntry
    {
        QString uniqueIdText;           // uniqueIdText：全局唯一键，便于缓存定位。
        StartupCategory category = StartupCategory::All; // category：所属分类。
        QString categoryText;           // categoryText：分类显示文本。
        QString itemNameText;           // itemNameText：条目名称。
        QString publisherText;          // publisherText：发布者/公司名。
        QString imagePathText;          // imagePathText：目标文件路径或宿主路径。
        QString commandText;            // commandText：命令行或数据文本。
        QString locationText;           // locationText：来源位置（注册表键/文件夹/任务路径）。
        QString locationGroupText;      // locationGroupText：注册表树一级节点对应的注册表位置。
        QString registryValueNameText;  // registryValueNameText：注册表值删除时使用的真实值名。
        QString userText;               // userText：归属用户或上下文。
        QString detailText;             // detailText：补充说明。
        QString sourceTypeText;         // sourceTypeText：来源类型（Run/Service/Task...）。
        bool enabled = true;            // enabled：是否启用。
        bool canOpenFileLocation = false; // canOpenFileLocation：是否支持打开文件位置。
        bool canOpenRegistryLocation = false; // canOpenRegistryLocation：是否支持打开注册表位置。
        bool canDelete = false;         // canDelete：是否支持删除来源项。
        bool deleteRegistryTree = false; // deleteRegistryTree：删除时是否删除整个注册表子键。
        QIcon itemIcon;                 // itemIcon：条目图标。
    };

    // StartupColumn：
    // - 作用：统一定义表格列索引，避免硬编码魔法数。
    enum class StartupColumn : int
    {
        Name = 0,       // 名称。
        Publisher,      // 发布者。
        ImagePath,      // 镜像路径。
        Command,        // 命令行/数据。
        Location,       // 来源位置。
        User,           // 用户/上下文。
        Enabled,        // 启用状态。
        Type,           // 来源类型。
        Detail,         // 补充说明。
        Count           // 列总数。
    };

    // toStartupColumn：
    // - 作用：把列枚举转换为 int 索引，供多实现文件共用。
    static int toStartupColumn(StartupColumn column);

private:
    // ===================== 初始化 =====================
    void initializeUi();
    void initializeToolbar();
    void initializeTabs();
    void initializeConnections();

    // ===================== 枚举与刷新 =====================
    void refreshAllStartupEntries();
    void requestAsyncRefresh(bool forceRefresh);
    void applyRefreshResult(std::vector<StartupEntry> entryList);
    void rebuildAllTables();
    void rebuildTableForCategory(StartupCategory category, QTableWidget* tableWidget);
    void rebuildRegistryTree();

    // ===================== 枚举器 =====================
    void appendLogonEntries(std::vector<StartupEntry>* entryListOut);
    void appendServiceEntries(std::vector<StartupEntry>* entryListOut);
    void appendDriverEntries(std::vector<StartupEntry>* entryListOut);
    void appendTaskEntries(std::vector<StartupEntry>* entryListOut);
    void appendAdvancedRegistryEntries(std::vector<StartupEntry>* entryListOut);
    void appendWinsockEntries(std::vector<StartupEntry>* entryListOut);
    void appendWmiEntries(std::vector<StartupEntry>* entryListOut);

    // ===================== 交互 =====================
    void showEntryContextMenu(StartupCategory category, QTableWidget* tableWidget, const QPoint& localPos);
    void showRegistryContextMenu(const QPoint& localPos);
    void showSelectedEntryDetails(StartupCategory category, QTableWidget* tableWidget);
    void openSelectedFileLocation(StartupCategory category, QTableWidget* tableWidget);
    void openSelectedFileProperties(StartupCategory category, QTableWidget* tableWidget);
    void openSelectedRegistryLocation(StartupCategory category, QTableWidget* tableWidget);
    void copySelectedRow(StartupCategory category, QTableWidget* tableWidget);
    void deleteSelectedEntry(StartupCategory category, QTableWidget* tableWidget);
    void exportCurrentView();
    void applyFilterAndRefresh();

    // ===================== 工具 =====================
    static QString categoryToText(StartupCategory category);
    int findEntryIndexByTableRow(StartupCategory category, int row) const;
    int findEntryIndexByRegistryTreeItem(const QTreeWidgetItem* treeItem) const;
    bool isRegistryBackedStartupEntry(const StartupEntry& entry) const;
    bool entryMatchesCurrentFilter(const StartupEntry& entry) const;
    QTableWidget* currentCategoryTable() const;
    StartupCategory currentCategory() const;
    QIcon resolveEntryIcon(const StartupEntry& entry);
    void appendEntryRow(QTableWidget* tableWidget, int rowIndex, const StartupEntry& entry, int entryIndex);
    void appendRegistryTreeLeaf(QTreeWidgetItem* parentItem, const StartupEntry& entry, int entryIndex);

private:
    // ===================== 顶层布局 =====================
    QVBoxLayout* m_rootLayout = nullptr;      // m_rootLayout：根布局。
    QWidget* m_toolbarWidget = nullptr;       // m_toolbarWidget：顶部工具栏容器。
    QHBoxLayout* m_toolbarLayout = nullptr;   // m_toolbarLayout：顶部工具栏布局。
    QTabWidget* m_sideTabWidget = nullptr;    // m_sideTabWidget：左侧分类标签容器。

    // ===================== 工具栏控件 =====================
    QPushButton* m_refreshButton = nullptr;   // m_refreshButton：刷新按钮。
    QPushButton* m_exportButton = nullptr;    // m_exportButton：导出按钮。
    QPushButton* m_copyButton = nullptr;      // m_copyButton：复制按钮。
    QLineEdit* m_filterEdit = nullptr;        // m_filterEdit：关键词过滤输入框。
    QCheckBox* m_hideMicrosoftCheck = nullptr; // m_hideMicrosoftCheck：隐藏微软项开关。
    QLabel* m_statusLabel = nullptr;          // m_statusLabel：摘要状态文本。

    // ===================== 分类页表格 =====================
    QWidget* m_allPage = nullptr;             // m_allPage：总览页。
    QWidget* m_logonPage = nullptr;           // m_logonPage：登录项页。
    QWidget* m_servicesPage = nullptr;        // m_servicesPage：服务页。
    QWidget* m_driversPage = nullptr;         // m_driversPage：驱动页。
    QWidget* m_tasksPage = nullptr;           // m_tasksPage：计划任务页。
    QWidget* m_registryPage = nullptr;        // m_registryPage：高级注册表页。
    QWidget* m_wmiPage = nullptr;             // m_wmiPage：WMI 持久化页。

    QTableWidget* m_allTable = nullptr;       // m_allTable：总览表。
    QTableWidget* m_logonTable = nullptr;     // m_logonTable：登录项表。
    QTableWidget* m_servicesTable = nullptr;  // m_servicesTable：服务表。
    QTableWidget* m_driversTable = nullptr;   // m_driversTable：驱动表。
    QTableWidget* m_tasksTable = nullptr;     // m_tasksTable：任务表。
    QTreeWidget* m_registryTree = nullptr;    // m_registryTree：高级注册表树（按注册表位置分组）。
    QTableWidget* m_wmiTable = nullptr;       // m_wmiTable：WMI 持久化表。

    // ===================== 数据缓存 =====================
    std::vector<StartupEntry> m_entryList;    // m_entryList：全部启动项缓存。
    QHash<QString, QIcon> m_iconCache;        // m_iconCache：路径到图标缓存。
    QTimer* m_refreshTimer = nullptr;         // m_refreshTimer：自动刷新定时器。
    bool m_initialRefreshDone = false;        // m_initialRefreshDone：是否已完成首次懒加载。
    std::atomic_bool m_refreshInProgress{ false }; // m_refreshInProgress：后台刷新是否进行中。
    std::atomic_bool m_refreshQueued{ false };     // m_refreshQueued：刷新过程中是否又收到新请求。
    std::unique_ptr<std::thread> m_refreshThread;  // m_refreshThread：后台枚举线程对象。
    int m_progressPid = 0;                         // m_progressPid：启动项枚举进度任务 PID。
};
