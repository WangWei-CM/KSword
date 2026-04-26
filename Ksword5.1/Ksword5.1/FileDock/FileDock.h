#pragma once

// ============================================================
// FileDock.h
// 作用：
// 1) 实现双栏文件资源管理器（左右面板独立）；
// 2) 提供导航、筛选、排序、基础文件操作与右键分析菜单；
// 3) 提供列管理与文件详情窗口入口。
// ============================================================

#include "../Framework.h"
#include "ManualFileSystemParser.h"

#include <QWidget>

#include <cstdint>     // std::uint64_t：文件大小统计。
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>      // std::vector：导航历史记录容器。

// Qt 前置声明：降低头文件耦合。
class QCheckBox;
class QComboBox;
class QDialog;
class QFileSystemModel;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QStandardItemModel;
class QStackedWidget;
class QSortFilterProxyModel;
class QSplitter;
class QStatusBar;
class QTabWidget;
class QTableWidget;
class QTreeView;
class QVBoxLayout;
class QWidget;

// ============================================================
// FileDock
// 说明：
// - 左右两栏都用同一套 FilePanelWidgets 结构描述；
// - 每个面板独立维护路径历史、过滤与排序状态。
// ============================================================
class FileDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化双栏 UI 并设置默认目录。
    // - 参数 parent：Qt 父控件。
    explicit FileDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：默认析构即可，所有子控件由 Qt 父子关系自动释放。
    ~FileDock() override;

    // openFileDetailByPath：
    // - 作用：对外暴露文件详情窗口入口（含属性/哈希/签名/PE 等 Tab）；
    // - 供 ServiceDock 等模块跨页联动调用。
    // - 参数 filePath：目标文件绝对路径。
    void openFileDetailByPath(const QString& filePath);

    // unlockFileByPath：
    // - 作用：对外暴露“文件解锁器”入口（单路径）；
    // - 供系统右键菜单命令启动后跨页联动调用。
    void unlockFileByPath(const QString& targetPath);

private:
    // FilePanelWidgets：
    // - 作用：聚合单个文件面板的全部控件与运行时状态。
    struct FilePanelWidgets
    {
        QWidget* rootWidget = nullptr;         // 面板根容器。
        QVBoxLayout* rootLayout = nullptr;     // 面板主布局。

        QWidget* navWidget = nullptr;          // 导航条容器。
        QHBoxLayout* navLayout = nullptr;      // 导航条布局。
        QPushButton* backButton = nullptr;     // 返回按钮。
        QPushButton* forwardButton = nullptr;  // 前进按钮。
        QPushButton* upButton = nullptr;       // 上级目录按钮。
        QPushButton* refreshButton = nullptr;  // 刷新按钮。
        QStackedWidget* pathStack = nullptr;   // 地址区域堆叠控件（面包屑/编辑框二选一）。
        QLineEdit* pathEdit = nullptr;         // 地址栏输入框（编辑模式）。
        QComboBox* driveCombo = nullptr;       // 地址栏右侧驱动器跳转下拉框。
        QWidget* breadcrumbWidget = nullptr;   // 面包屑容器（展示模式）。
        QHBoxLayout* breadcrumbLayout = nullptr; // 面包屑布局。
        QPushButton* breadcrumbEditTriggerButton = nullptr; // 面包屑末尾空白点击热区。

        QWidget* toolWidget = nullptr;         // 工具条容器。
        QHBoxLayout* toolLayout = nullptr;     // 工具条布局。
        QComboBox* viewModeCombo = nullptr;    // 视图模式选择（图标/列表/详情/树）。
        QCheckBox* showSystemCheck = nullptr;  // 显示系统文件开关。
        QCheckBox* showHiddenCheck = nullptr;  // 显示隐藏文件开关。
        QComboBox* sortModeCombo = nullptr;    // 排序方式选择。
        QComboBox* readModeCombo = nullptr;    // 读取方式（Windows API/手动解析）。
        QLineEdit* filterEdit = nullptr;       // 文件名快速过滤输入框。

        QTreeView* fileView = nullptr;         // 文件树视图。
        QFileSystemModel* fsModel = nullptr;   // 原始文件系统模型。
        QSortFilterProxyModel* proxyModel = nullptr; // 过滤代理模型。
        QStandardItemModel* manualModel = nullptr;   // 手动解析原始模型。
        QSortFilterProxyModel* manualProxyModel = nullptr; // 手动解析代理模型。

        QStatusBar* statusBar = nullptr;       // 面板状态栏。
        QLabel* pathStatusLabel = nullptr;     // 当前路径状态。
        QLabel* selectionStatusLabel = nullptr; // 选中数量/大小状态。
        QLabel* diskStatusLabel = nullptr;     // 磁盘空间状态。
        QLabel* parserStatusLabel = nullptr;   // 当前解析器状态提示。

        std::vector<QString> history;          // 路径历史列表。
        int historyIndex = -1;                 // 当前历史索引。
        QString currentPath;                   // 当前目录路径。
        QString manualLoadedPath;              // 手动解析模型当前已加载目录路径。
        QString panelNameText;                 // 面板名称（日志与提示使用）。
        QString lastStatusLogSignature;        // 状态栏日志去重签名。
        QString lastFilterLogSignature;        // 过滤参数日志去重签名。
        bool pathEditMode = false;             // 当前是否处于路径编辑模式。
        ks::file::ManualFsType lastManualFsType = ks::file::ManualFsType::Unknown; // 最近一次手动解析识别到的FS类型。
        bool manualParseInProgress = false;    // 手动解析后台任务是否正在运行。
        bool manualParsePending = false;       // 手动解析是否有待执行请求。
        bool manualParsePendingShowWarning = false; // 待执行请求是否需要失败弹框。
        int manualParseRequestSerial = 0;      // 手动解析请求序列号（用于丢弃过期结果）。
        QString manualParsingPath;             // 当前后台解析正在处理的路径（用于避免同路径重复解析）。
    };

private:
    // ======================= UI 初始化 ========================
    // initializeUi：
    // - 作用：构建双栏分割布局并初始化左右面板。
    void initializeUi();

    // initializePanel：
    // - 作用：初始化一个文件面板的全部控件。
    // - 参数 panel：待初始化面板结构体。
    // - 参数 titleText：面板标题文本。
    void initializePanel(FilePanelWidgets& panel, const QString& titleText);

    // initializeConnections：
    // - 作用：绑定单个面板的信号槽交互逻辑。
    // - 参数 panel：目标面板。
    void initializeConnections(FilePanelWidgets& panel);

    // initializeRecoveryPage：
    // - 作用：初始化“文件恢复”竖排 Tab 页面。
    void initializeRecoveryPage();

    // ======================= 导航与状态 =======================
    // navigateToPath：
    // - 作用：切换面板目录并可选写入历史。
    // - 参数 panel：目标面板。
    // - 参数 pathText：目标目录路径。
    // - 参数 recordHistory：是否写入导航历史。
    void navigateToPath(FilePanelWidgets& panel, const QString& pathText, bool recordHistory);

    // refreshPanel：
    // - 作用：刷新当前目录并重新应用过滤/排序。
    void refreshPanel(FilePanelWidgets& panel);

    // rebuildBreadcrumb：
    // - 作用：按当前路径重建可点击面包屑。
    void rebuildBreadcrumb(FilePanelWidgets& panel);

    // setPathEditMode：
    // - 作用：切换地址区显示模式（true=编辑框，false=面包屑）。
    void setPathEditMode(FilePanelWidgets& panel, bool editMode);

    // refreshDriveCombo：
    // - 作用：刷新驱动器下拉框列表并同步当前选中项。
    void refreshDriveCombo(FilePanelWidgets& panel);

    // updatePanelStatus：
    // - 作用：更新状态栏（路径、选中数量、容量等）。
    void updatePanelStatus(FilePanelWidgets& panel);

    // applyPanelFilterAndSort：
    // - 作用：应用显示隐藏文件、名称过滤和排序模式。
    void applyPanelFilterAndSort(FilePanelWidgets& panel);

    // applyReadModeToPanel：
    // - 作用：根据读取模式切换面板模型（Windows API / 手动解析）。
    void applyReadModeToPanel(FilePanelWidgets& panel);

    // reloadManualModel：
    // - 作用：手动解析当前目录并填充模型。
    // - 参数 showWarningMessage：是否在失败时弹框提示。
    bool reloadManualModel(FilePanelWidgets& panel, bool showWarningMessage);

    // requestAsyncManualReload：
    // - 作用：异步执行手动解析，避免 UI 线程阻塞。
    // - 参数 panel：目标面板。
    // - 参数 showWarningMessage：失败时是否弹框。
    void requestAsyncManualReload(FilePanelWidgets& panel, bool showWarningMessage);

    // currentModeIsManual：
    // - 作用：判断当前面板是否处于手动解析模式。
    bool currentModeIsManual(const FilePanelWidgets& panel) const;

    // ======================= 文件操作 =========================
    // showPanelContextMenu：
    // - 作用：显示右键菜单（操作 + 分析子菜单）。
    void showPanelContextMenu(FilePanelWidgets& panel, const QPoint& localPos);

    // openSelectedItems：
    // - 作用：打开当前选中项（支持多选，目录进入或系统打开）。
    void openSelectedItems(FilePanelWidgets& panel);

    // copySelectedItemPath：
    // - 作用：复制选中项完整路径到剪贴板。
    void copySelectedItemPath(FilePanelWidgets& panel);

    // copySelectedItemKernelPath：
    // - 作用：复制选中项的内核命名空间路径（\??\...）到剪贴板。
    void copySelectedItemKernelPath(FilePanelWidgets& panel);

    // copySelectedItemShortName：
    // - 作用：复制选中项短文件名（8.3 名称）到剪贴板。
    void copySelectedItemShortName(FilePanelWidgets& panel);

    // copySelectedItems：
    // - 作用：复制选中项到内部剪贴板缓存（等待粘贴）。
    void copySelectedItems(FilePanelWidgets& panel);

    // cutSelectedItems：
    // - 作用：剪切选中项到内部剪贴板缓存（粘贴后删除源文件）。
    void cutSelectedItems(FilePanelWidgets& panel);

    // pasteClipboardItems：
    // - 作用：把内部剪贴板中的路径粘贴到当前目录。
    void pasteClipboardItems(FilePanelWidgets& panel);

    // createNewFileOrFolder：
    // - 作用：在当前目录创建新文件或新文件夹。
    void createNewFileOrFolder(FilePanelWidgets& panel, bool createFolder);

    // renameSelectedItem：
    // - 作用：重命名当前选中项。
    void renameSelectedItem(FilePanelWidgets& panel);

    // deleteSelectedItem：
    // - 作用：删除当前选中项（当前实现走普通删除）。
    void deleteSelectedItem(FilePanelWidgets& panel);

    // deleteSelectedItemByDriver：
    // - 作用：通过 KswordARK 驱动对当前选中项执行硬删除。
    // - 说明：目录删除由 R3 先展开为后序路径列表，再逐项交给驱动删除。
    void deleteSelectedItemByDriver(FilePanelWidgets& panel);

    // takeOwnershipSelectedItems：
    // - 作用：对当前选中项执行“取得所有权 + 授权完全控制”。
    // - 说明：调用系统 takeown/icacls，失败信息会汇总提示。
    void takeOwnershipSelectedItems(FilePanelWidgets& panel);

    // unlockSelectedItemsByDriver：
    // - 作用：扫描选中路径占用进程，并通过 KswordARK 驱动尝试结束占用进程；
    // - 说明：用于“文件解锁器”右键动作，不直接删除文件。
    void unlockSelectedItemsByDriver(FilePanelWidgets& panel);

    // unlockPathsByDriver：
    // - 作用：执行“文件解锁器”核心流程（扫描占用 + R0 结束进程）；
    // - 参数 triggerTag：触发来源标签（右键菜单/系统右键）。
    // - 参数 panelForRefresh：可选，仅刷新指定面板；为空时刷新左右面板。
    void unlockPathsByDriver(
        const std::vector<QString>& targetPaths,
        const QString& triggerTag,
        FilePanelWidgets* panelForRefresh);

    // showColumnManagerDialog：
    // - 作用：弹出列管理器切换列显示状态。
    void showColumnManagerDialog(FilePanelWidgets& panel);

    // showFileDetailDialog：
    // - 作用：打开文件详情窗口（多 Tab 信息展示）。
    void showFileDetailDialog(const QString& filePath);

    // openHandleUsageScanWindow：
    // - 作用：基于当前右键选中路径打开“占用句柄扫描结果”窗口；
    // - 扫描结果独立展示，不阻塞 FileDock 主界面。
    // - 参数 scanPaths：待扫描路径集合（文件或目录）。
    void openHandleUsageScanWindow(const std::vector<QString>& scanPaths);

    // ======================= 工具函数 =========================
    // currentIndexPath：
    // - 作用：获取面板当前选中索引对应的绝对路径。
    // - 返回：若无选中则返回空字符串。
    QString currentIndexPath(const FilePanelWidgets& panel) const;

    // selectedPaths：
    // - 作用：获取面板当前多选路径列表。
    std::vector<QString> selectedPaths(const FilePanelWidgets& panel) const;

    // formatSizeText：
    // - 作用：格式化字节大小（B/KB/MB/GB）。
    static QString formatSizeText(std::uint64_t sizeBytes);

    // ======================= 文件恢复 =========================
    // refreshRecoveryVolumeList：
    // - 作用：刷新可扫描卷列表（仅展示 NTFS 卷）。
    void refreshRecoveryVolumeList();

    // scanDeletedFilesForRecovery：
    // - 作用：扫描当前卷的 NTFS 删除项并展示到表格。
    void scanDeletedFilesForRecovery();

    // scanDeletedFilesForRecoveryAsync：
    // - 作用：异步扫描当前卷，避免扫描误删时阻塞 UI。
    void scanDeletedFilesForRecoveryAsync();

    // recoverSelectedDeletedFiles：
    // - 作用：对选中删除项执行恢复（当前支持 resident 数据恢复）。
    void recoverSelectedDeletedFiles();

    // recoverSelectedDeletedFilesAsync：
    // - 作用：异步恢复选中删除项，避免恢复过程阻塞 UI。
    void recoverSelectedDeletedFilesAsync();

private:
    // 根布局控件。
    QVBoxLayout* m_rootLayout = nullptr;       // FileDock 根布局。
    QTabWidget* m_rootTabWidget = nullptr;     // 竖排根 Tab（文件管理 / 文件恢复）。
    QWidget* m_fileManagerPage = nullptr;      // 文件管理页容器。
    QWidget* m_fileRecoveryPage = nullptr;     // 文件恢复页容器。
    QSplitter* m_mainSplitter = nullptr;       // 左右分栏分割器。

    // 文件恢复页控件。
    QComboBox* m_recoveryVolumeCombo = nullptr; // 恢复卷选择下拉框。
    QPushButton* m_recoveryRefreshButton = nullptr; // 刷新卷按钮。
    QPushButton* m_recoveryScanButton = nullptr;    // 扫描按钮。
    QPushButton* m_recoveryExportButton = nullptr;  // 恢复导出按钮。
    QTableWidget* m_recoveryTable = nullptr;        // 删除项结果表格。
    QLabel* m_recoveryStatusLabel = nullptr;        // 扫描状态标签。
    std::vector<ks::file::NtfsDeletedFileEntry> m_deletedRecoveryItems; // 删除项缓存（含 resident 数据）。
    bool m_recoveryScanInProgress = false;          // 误删扫描后台任务运行状态。
    bool m_recoveryRecoverInProgress = false;       // 误删恢复后台任务运行状态。

    // 左右面板实例。
    FilePanelWidgets m_leftPanel;              // 左侧面板状态。
    FilePanelWidgets m_rightPanel;             // 右侧面板状态。

    // 跨面板复制/剪切缓存。
    std::vector<QString> m_clipboardPaths;     // 剪贴板中的路径列表。
    bool m_clipboardCutMode = false;           // true=剪切；false=复制。

    // 文件解锁器后台线程状态。
    std::thread m_unlockerWorkerThread;
    std::atomic_bool m_unlockerWorkerStopRequested{ false };
    std::atomic_bool m_unlockerWorkerRunning{ false };
    mutable std::mutex m_unlockerWorkerMutex;
};
