#pragma once

// ============================================================
// OtherDock.h
// 作用说明：
// 1) 构建“窗口管理”页面，包含窗口列表、筛选、分组和预览区；
// 2) 提供窗口右键操作（激活/置顶/显示隐藏/启用禁用/结束进程等）；
// 3) 支持打开“窗口详细信息”非模态对话框进行深度查看与修改。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>      // std::atomic_bool：控制后台枚举线程互斥刷新。
#include <vector>      // std::vector：窗口快照容器。

// Qt 前置声明：降低头文件编译耦合。
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSplitter;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QStatusBar;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QVBoxLayout;
class QHBoxLayout;

// ============================================================
// OtherDock
// 说明：
// - 本类聚焦窗口枚举与窗口属性操作；
// - 所有重枚举操作都放后台线程，避免阻塞 UI。
// ============================================================
class OtherDock : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化窗口列表页与工具栏，并触发首轮枚举。
    // - 参数 parent：Qt 父控件。
    explicit OtherDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止自动刷新定时器，确保后台流程退出安全。
    ~OtherDock() override;

    // WindowInfo：
    // - 作用：缓存单个窗口的关键属性，供列表与详情弹窗复用。
    struct WindowInfo
    {
        quint64 hwndValue = 0;              // 窗口句柄（整数形式）。
        quint64 parentHwndValue = 0;        // 父窗口句柄。
        quint64 ownerHwndValue = 0;         // 所有者窗口句柄。
        QString titleText;                  // 窗口标题。
        QString classNameText;              // 窗口类名。
        std::uint32_t processId = 0;        // 所属进程 PID。
        std::uint32_t threadId = 0;         // 创建线程 TID。
        QString processNameText;            // 进程名。
        QString processImagePathText;       // 进程可执行文件完整路径。
        QRect windowRect;                   // 窗口矩形（屏幕坐标）。
        quint64 styleValue = 0;             // 窗口样式位（WS_*）。
        quint64 exStyleValue = 0;           // 扩展样式位（WS_EX_*）。
        bool visible = false;               // 是否可见。
        bool enabled = false;               // 是否可用。
        bool topMost = false;               // 是否置顶。
        bool minimized = false;             // 是否最小化。
        bool maximized = false;             // 是否最大化。
        bool valid = false;                 // IsWindow 是否有效。
        int zOrder = 0;                     // 枚举顺序（近似 Z 序）。
        QString enumApiName;                // 枚举来源 API 名称。
        int alphaValue = 255;               // 分层窗口透明度（0-255）。
        bool isChildWindow = false;         // 是否子窗口。
    };

private:
    // ===================== UI 初始化 ======================
    void initializeUi();
    void initializeConnections();
    void applyViewMode();

    // ===================== 列表刷新 ======================
    void refreshWindowListAsync();
    void rebuildWindowTreeFromSnapshot();
    bool passFilter(const WindowInfo& info) const;
    void updateStatusBar();
    void updatePreviewPanel(const WindowInfo* info);
    void refreshDesktopList();
    void switchToSelectedDesktop();

    // ===================== 交互操作 ======================
    void showWindowContextMenu(const QPoint& localPos);
    void exportVisibleRowsToTsv();
    void openWindowDetailDialog(const WindowInfo& info);
    const WindowInfo* findInfoByHwnd(quint64 hwndValue) const;

private:
    // 顶层布局与工具栏。
    QVBoxLayout* m_rootLayout = nullptr;          // 根布局。
    QWidget* m_toolBarWidget = nullptr;           // 顶部工具栏容器。
    QHBoxLayout* m_toolBarLayout = nullptr;       // 顶部工具栏布局。
    QPushButton* m_refreshButton = nullptr;       // 刷新按钮。
    QCheckBox* m_autoRefreshCheck = nullptr;      // 自动刷新开关。
    QSpinBox* m_autoRefreshIntervalSpin = nullptr; // 自动刷新间隔（ms）。
    QLineEdit* m_filterEdit = nullptr;            // 关键字过滤输入。
    QComboBox* m_filterModeCombo = nullptr;       // 条件过滤下拉框。
    QComboBox* m_enumModeCombo = nullptr;         // 枚举方式下拉框。
    QComboBox* m_groupModeCombo = nullptr;        // 分组方式下拉框。
    QComboBox* m_viewModeCombo = nullptr;         // 显示样式切换。
    QPushButton* m_exportButton = nullptr;        // 导出按钮。

    // 中部主区域：主Tab（窗口列表/桌面管理）。
    QTabWidget* m_contentTabWidget = nullptr;     // 主内容 Tab 容器。
    QWidget* m_windowListPage = nullptr;          // 窗口列表页。
    QVBoxLayout* m_windowListPageLayout = nullptr;// 窗口列表页布局。

    // 窗口列表页：左树右预览。
    QSplitter* m_mainSplitter = nullptr;          // 左右分割器。
    QTreeWidget* m_windowTree = nullptr;          // 窗口树/列表。
    QWidget* m_previewWidget = nullptr;           // 右侧预览容器。
    QVBoxLayout* m_previewLayout = nullptr;       // 右侧预览布局。
    QLabel* m_thumbnailLabel = nullptr;           // 窗口缩略图。
    QPushButton* m_captureButton = nullptr;       // 截图按钮。
    QTextEdit* m_quickInfoText = nullptr;         // 关键属性摘要文本。

    // 桌面管理页：枚举可切换桌面（SwitchDesktop）。
    QWidget* m_desktopPage = nullptr;             // 桌面管理页容器。
    QVBoxLayout* m_desktopPageLayout = nullptr;   // 桌面管理页主布局。
    QHBoxLayout* m_desktopToolLayout = nullptr;   // 桌面管理工具栏布局。
    QPushButton* m_desktopRefreshButton = nullptr;// 刷新桌面列表按钮。
    QPushButton* m_desktopSwitchButton = nullptr; // 切换到选中桌面按钮。
    QTableWidget* m_desktopTable = nullptr;       // 桌面列表表格。
    QLabel* m_desktopStatusLabel = nullptr;       // 桌面状态提示标签。

    // 底部状态栏。
    QStatusBar* m_statusBar = nullptr;            // 状态栏。
    QLabel* m_totalLabel = nullptr;               // 窗口总数。
    QLabel* m_visibleLabel = nullptr;             // 可见窗口数。
    QLabel* m_systemLabel = nullptr;              // 系统窗口数。
    QLabel* m_selectedLabel = nullptr;            // 当前选中项信息。

    // 自动刷新定时器。
    QTimer* m_autoRefreshTimer = nullptr;         // 定时刷新控制。

    // 数据缓存。
    std::vector<WindowInfo> m_windowSnapshot;     // 当前窗口快照。
    std::vector<WindowInfo> m_previousSnapshot;   // 上一轮窗口快照。
    std::vector<WindowInfo> m_exitedOneRound;     // 退出窗口保留一轮。
    std::vector<quint64> m_newWindowHandles;      // 新增窗口句柄列表（用于高亮）。
    int m_refreshProgressPid = 0;                 // 刷新任务进度卡片 PID。
    std::atomic_bool m_refreshRunning{ false };   // 后台刷新进行中标志。
};
