#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QDockWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDragMoveEvent>
#include <QResizeEvent>
#include <QPushButton>
#include <QShowEvent>
#include <QEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QString>
#include <QByteArray>
#include <QToolButton>

#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

// ADS头文件
#include "include/ads/DockManager.h"
#include "include/ads/DockWidget.h"
#include "include/ads/DockAreaWidget.h"

// 自定义Dock头文件
#include "WelcomeDock/WelcomeDock.h"
#include "ProcessDock/ProcessDock.h"
#include "NetworkDock/NetworkDock.h"
#include "MemoryDock/MemoryDock.h"
#include "FileDock/FileDock.h"
#include "DriverDock/DriverDock.h"
#include "KernelDock/KernelDock.h"
#include "MonitorDock/MonitorDock.h"
#include "MonitorDock/MonitorPanelWidget.h"
#include "HardwareDock/HardwareDock.h"
#include "PrivilegeDock/PrivilegeDock.h"
#include "SettingsDock/SettingsDock.h"
#include "SettingsDock/AppearanceSettings.h"
#include "StartupDock/StartupDock.h"
#include "ServerDock/ServiceDock.h"
#include "WindowDock/WindowDock.h"
#include "RegistryDock/RegistryDock.h"
#include "MiscDock/MiscDock.h"
#include "句柄/HandleDock.h"

class LogDockWidget; // 前置声明：日志 Dock 面板类型。
class ProgressDockWidget; // 前置声明：当前操作进度面板类型。
class CodeEditorWidget; // 前置声明：即时窗口可复用代码编辑器组件。
namespace ks::ui
{
    class CustomTitleBar; // 前置声明：主窗口自绘标题栏组件。
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // StartupProgressCallback 作用：
    // - 主窗口构造期间把细分阶段进度回传给启动画面；
    // - 主函数可传入 lambda，把文字与百分比同步到 splash。
    using StartupProgressCallback = std::function<void(int, const QString&)>;

    // 构造函数作用：
    // - 初始化主窗口菜单、Dock、布局与外观；
    // - 可选地持续回传启动阶段进度给 splash。
    // 参数 parent：Qt 父对象。
    // 参数 startupProgressCallback：启动进度回调；为空时忽略。
    explicit MainWindow(
        QWidget* parent = nullptr,
        StartupProgressCallback startupProgressCallback = StartupProgressCallback());
    ~MainWindow();

public slots:
    // focusHandleDockByPid 作用：
    // - 将“句柄”Dock 置顶并切换 PID 过滤；
    // - 供进程详情窗口发起“跳转到句柄视图”时调用。
    // 调用方式：QMetaObject::invokeMethod(mainWindow, "focusHandleDockByPid", ... )。
    // 入参 pid：目标进程 PID。
    void focusHandleDockByPid(quint32 pid);

    // openProcessDetailByPid 作用：
    // - 将“进程”Dock 置顶并打开指定 PID 的进程详情窗口；
    // - 供 FileDock 的“占用句柄扫描结果”窗口跳转调用。
    // 调用方式：QMetaObject::invokeMethod(mainWindow, "openProcessDetailByPid", ... )。
    // 入参 pid：目标进程 PID。
    void openProcessDetailByPid(quint32 pid);

    // focusServiceDockByName 作用：
    // - 将“服务”Dock 置顶并按服务名定位到目标行；
    // - 供 StartupDock 的“转到服务管理”入口调用。
    // 入参 serviceNameText：目标服务短名。
    void focusServiceDockByName(const QString& serviceNameText);

    // openFileDetailDockByPath 作用：
    // - 置顶“文件”Dock 并打开指定文件的详情分析窗口；
    // - 供 ServiceDock 的 BinaryPath/ServiceDll 联动调用。
    // 入参 filePath：目标文件路径。
    void openFileDetailDockByPath(const QString& filePath);

    // openFileUnlockerDockByPath 作用：
    // - 置顶“文件”Dock 并触发“文件解锁器”处理指定路径；
    // - 供系统右键菜单命令启动后的自动联动调用。
    // 入参 filePath：目标文件或目录路径。
    void openFileUnlockerDockByPath(const QString& filePath);

protected:
    // eventFilter 作用：
    // - 监听 ADS 浮动 Dock 窗口的显示与尺寸变化；
    // - 在浮动窗口脱离主窗口后，同步应用主界面同款纯色/背景图填充。
    bool eventFilter(QObject* watchedObject, QEvent* event) override;

    // closeEvent 作用：
    // - 在主窗口关闭时明确触发应用退出；
    // - 避免浮动 Dock 或后台窗口残留导致进程未结束。
    // 调用方式：Qt 关闭窗口时自动回调。
    // 入参 event：关闭事件对象，函数内会 accept 并触发 quit/exit。
    void closeEvent(QCloseEvent* event) override;

    // resizeEvent 作用：
    // - 窗口尺寸变化时重新生成背景画刷，避免背景图拉伸失真；
    // - 在深浅色切换后保持背景覆盖全窗口。
    // 调用方式：Qt 在窗口尺寸改变时自动回调。
    // 入参 event：尺寸变化事件对象。
    void resizeEvent(QResizeEvent* event) override;

    // showEvent 作用：
    // - 主窗口首次显示后再启动延迟页面补载；
    // - 保证窗口能先出现，再继续补齐剩余 Dock 内容。
    void showEvent(QShowEvent* event) override;

    // changeEvent 作用：
    // - 监听窗口最大化/还原状态变化；
    // - 同步刷新自绘标题栏中的最大化按钮图标状态。
    void changeEvent(QEvent* event) override;

    // nativeEvent 作用：
    // - 处理无边框窗口命中测试（拖动与边缘缩放）；
    // - 在自绘标题栏可拖动区域返回 HTCAPTION。
    // 调用方式：Qt 在 Windows 消息循环中自动回调。
    // 入参 eventType/message/result：原生消息参数。
    // 返回：true=已处理；false=走基类默认处理。
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void initMenus();
    void initPrivilegeStatusButtons();
    void refreshPrivilegeStatusButtons();
    void applyPrivilegeButtonStyle(QPushButton* button, bool activeState);
    void handleR0StatusButtonClicked();
    bool queryR0DriverServiceRunning(bool& runningOut, bool fatalOnError);
    bool startR0DriverService();
    bool stopR0DriverService(bool suppressErrorDialog = false);
    void startR0DriverLogPoller();
    void stopR0DriverLogPoller();
    void runR0DriverLogPollerLoop();
    void dispatchR0DriverLogRecord(const std::string& logRecordText);
    bool showUnsignedDriverFailureDialog(unsigned long errorCode, const QString& operationText);
    bool enableWindowsTestModeAndPromptReboot();
    bool isR0DriverSignatureFailure(unsigned long errorCode) const;
    void showR0FatalError(const QString& stageText, unsigned long errorCode, const QString& detailText = QString());
    void requestAdminElevationRestart();
    bool hasAdminPrivilege() const;
    bool hasDebugPrivilege() const;
    bool hasSystemPrivilege() const;
    bool hasTrustedInstallerPrivilege() const;
    bool enableSeDebugPrivilege(std::string& errorTextOut) const;
    void initDockWidgets();
    QWidget* createDockPlaceholderWidget(const QString& titleText) const;
    void ensureDockContentInitialized(ads::CDockWidget* dockWidget);

    // showSettingsPanelFromMenu：
    // - 作用：从顶部菜单栏打开设置内容，替代主 Dock Tab 中的“设置”页签。
    void showSettingsPanelFromMenu();

    void initializeNextDeferredDock();

    // reportStartupProgress 作用：
    // - 安全调用启动进度回调；
    // - 让 MainWindow 内部各阶段都能主动更新 splash 文案。
    // 入参 progressPercent：阶段进度百分比。
    // 入参 statusText：阶段说明文本。
    void reportStartupProgress(int progressPercent, const QString& statusText) const;

    // initAppearanceSettings 作用：
    // - 读取 SettingsDock/JSON 的外观配置；
    // - 绑定系统深浅色变化回调；
    // - 启动时立即应用一次外观。
    // 调用方式：MainWindow 构造末尾调用。
    void initAppearanceSettings();

    void setupDockLayout();

    // initCustomTitleBar 作用：
    // - 初始化主窗口自绘标题栏并替代系统标题栏；
    // - 绑定置顶/窗口控制/命令输入三类交互信号。
    void initCustomTitleBar();

    // syncCustomTitleBarMaximizedState 作用：
    // - 统一计算主窗口“是否处于最大化态”并刷新标题栏第二按钮图标；
    // - 兼容 Qt 状态与 Win32 Zoomed 状态，避免切换瞬间图标不一致。
    void syncCustomTitleBarMaximizedState();

    // ensureNativeFramelessWindowStyle 作用：
    // - 在无边框模式下补齐 Win32 可缩放/可最大化样式位；
    // - 修复 Win+↑/Win+↓ 与系统最大化链路偶发失效问题。
    void ensureNativeFramelessWindowStyle();

    // applyNativeWindowFrameVisualStyle 作用：
    // - 向 DWM 同步当前深浅色状态并关闭系统可见边框；
    // - 修复主窗口在焦点切换时瞬间闪出白色边框的问题。
    // 调用方式：窗口句柄创建完成后、主题切换后调用。
    void applyNativeWindowFrameVisualStyle();

    // isWindowActuallyMaximized 作用：
    // - 综合 Qt 状态与 Win32 IsZoomed 判断真实最大化状态；
    // - 避免仅依赖 isMaximized() 造成“按钮状态漂移”。
    bool isWindowActuallyMaximized() const;

    // setWindowMaximizedBySystemCommand 作用：
    // - 通过 Win32 原生窗口状态 API 执行最大化/还原；
    // - 避免在标题栏双击鼠标消息处理中同步 SendMessage 重入，造成闪动与状态错乱。
    // 入参 targetMaximizedState：true=最大化，false=还原。
    void setWindowMaximizedBySystemCommand(bool targetMaximizedState);

    // setPinnedWindowState 作用：
    // - 设置主窗口置顶状态并同步标题栏图钉图标；
    // - 内部使用 SetWindowPos 切换 HWND_TOPMOST/HWND_NOTOPMOST。
    // 入参 pinnedState：目标置顶状态。
    // 入参 emitLog：是否记录状态切换日志。
    void setPinnedWindowState(bool pinnedState, bool emitLog = true);

    // togglePinnedWindowState 作用：
    // - 在当前置顶状态基础上做取反切换。
    void togglePinnedWindowState();

    // executeCommandInNewConsole 作用：
    // - 使用 CREATE_NEW_CONSOLE 打开可见 cmd 并执行 /K 命令；
    // - 命令执行后控制台不会自动关闭。
    // 入参 commandText：要执行的命令文本（用户输入）。
    void executeCommandInNewConsole(const QString& commandText);

    // applyAppearanceSettings 作用：
    // - 把主题模式、背景图、透明度应用到主窗口；
    // - 强制设置窗口背景色，规避 Win11 自动接管背景问题。
    // 调用方式：初始化、设置页变更、系统颜色变化时调用。
    // 入参 settings：外观配置结构体。
    // 入参 triggerReason：触发来源文本（日志用途）。
    void applyAppearanceSettings(const ks::settings::AppearanceSettings& settings, const QString& triggerReason);

    // isDarkModeEffective 作用：
    // - 根据“手动深浅色/跟随系统”计算当前最终是否深色。
    // 调用方式：应用样式或重建背景时调用。
    // 入参 settings：外观配置结构体。
    // 返回：true=深色；false=浅色。
    bool isDarkModeEffective(const ks::settings::AppearanceSettings& settings) const;

    // rebuildWindowBackgroundBrush 作用：
    // - 依据当前配置重建窗口背景画刷（纯色 + 背景图透明叠加）。
    // 调用方式：外观变更与窗口 resize 后调用。
    void rebuildWindowBackgroundBrush();

    // scheduleWindowBackgroundBrushRebuild 作用：
    // - 对窗口 resize 触发的背景重建做短延迟合并；
    // - 避免最大化拖下还原或连续缩放时每帧都同步重建整窗背景。
    void scheduleWindowBackgroundBrushRebuild();

    // applyFloatingDockContainerAppearance 作用：
    // - 把当前主题色、背景图与样式同步到指定浮动 Dock 容器；
    // - 解决“拖出后浮动窗口背景变成纯黑未填充”的问题。
    void applyFloatingDockContainerAppearance(ads::CFloatingDockContainer* floatingWidget) const;

    // buildAppearanceOverlayStyleSheet 作用：
    // - 生成深色/浅色覆盖样式字符串，叠加在基础 QSS 之后。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 darkModeEnabled：是否使用深色样式。
    // 入参 enableDockTransparencyForBackgroundImage：背景图可用时是否强制 Dock 背景透明。
    // 返回：拼接后的 QSS 片段。
    QString buildAppearanceOverlayStyleSheet(
        const ks::settings::AppearanceSettings& settings,
        bool darkModeEnabled,
        bool enableDockTransparencyForBackgroundImage) const;

    // ADS Dock Manager
    QWidget* m_mainRootContainer = nullptr; // m_mainRootContainer：主窗口根容器（承载标题栏+Dock 管理器）。
    QVBoxLayout* m_mainRootLayout = nullptr; // m_mainRootLayout：主窗口根容器纵向布局。
    ads::CDockManager* m_pDockManager = nullptr; // m_pDockManager：ADS Dock 管理器主对象。

    // Dock Widgets
    ads::CDockWidget* m_dockWelcome = nullptr; // m_dockWelcome：欢迎页 Dock。
    ads::CDockWidget* m_dockProcess = nullptr; // m_dockProcess：进程页 Dock。
    ads::CDockWidget* m_dockNetwork = nullptr; // m_dockNetwork：网络页 Dock。
    ads::CDockWidget* m_dockMemory = nullptr; // m_dockMemory：内存页 Dock。
    ads::CDockWidget* m_dockFile = nullptr; // m_dockFile：文件页 Dock。
    ads::CDockWidget* m_dockDriver = nullptr; // m_dockDriver：驱动页 Dock。
    ads::CDockWidget* m_dockKernel = nullptr; // m_dockKernel：内核页 Dock。
    ads::CDockWidget* m_dockMonitorTab = nullptr; // m_dockMonitorTab：监控页 Dock。
    ads::CDockWidget* m_dockPrivilege = nullptr; // m_dockPrivilege：权限页 Dock。
    ads::CDockWidget* m_dockWindow = nullptr; // m_dockWindow：窗口页 Dock。
    ads::CDockWidget* m_dockRegistry = nullptr; // m_dockRegistry：注册表页 Dock。
    ads::CDockWidget* m_dockHandle = nullptr;
    ads::CDockWidget* m_dockStartup = nullptr; // m_dockStartup：启动项页 Dock。
    ads::CDockWidget* m_dockService = nullptr;
    ads::CDockWidget* m_dockMisc = nullptr;
    ads::CDockWidget* m_dockHardware = nullptr; // m_dockHardware：硬件页 Dock。
    ads::CDockWidget* m_dockCurrentOp = nullptr; // m_dockCurrentOp：当前操作 Dock。
    ads::CDockWidget* m_dockLog = nullptr; // m_dockLog：日志输出 Dock。
    ads::CDockWidget* m_dockImmediate = nullptr; // m_dockImmediate：即时窗口 Dock。
    ads::CDockWidget* m_dockMonitor = nullptr; // m_dockMonitor：监视面板 Dock。

    // 自定义Widgets
    WelcomeDock* m_welcomeWidget = nullptr; // m_welcomeWidget：欢迎页内容控件。
    ProcessDock* m_processWidget = nullptr; // m_processWidget：进程页内容控件。
    NetworkDock* m_networkWidget = nullptr; // m_networkWidget：网络页内容控件。
    MemoryDock* m_memoryWidget = nullptr; // m_memoryWidget：内存页内容控件。
    FileDock* m_fileWidget = nullptr; // m_fileWidget：文件页内容控件。
    DriverDock* m_driverWidget = nullptr; // m_driverWidget：驱动页内容控件。
    KernelDock* m_kernelWidget = nullptr; // m_kernelWidget：内核页内容控件。
    MonitorDock* m_monitorWidget = nullptr; // m_monitorWidget：监控页内容控件。
    MonitorPanelWidget* m_monitorPanelWidget = nullptr; // m_monitorPanelWidget：左下角监视面板四宫格性能图组件。
    HardwareDock* m_hardwareWidget = nullptr; // m_hardwareWidget：硬件页内容控件。
    PrivilegeDock* m_privilegeWidget = nullptr; // m_privilegeWidget：权限页内容控件。
    StartupDock* m_startupWidget = nullptr; // m_startupWidget：启动项页内容控件。
    ServiceDock* m_serviceWidget = nullptr;
    MiscDock* m_miscWidget = nullptr;
    WindowDock* m_windowWidget = nullptr; // m_windowWidget：窗口页内容控件。
    RegistryDock* m_registryWidget = nullptr; // m_registryWidget：注册表页内容控件。
    HandleDock* m_handleWidget = nullptr;
    LogDockWidget* m_logWidget = nullptr; // 日志输出 Dock 的可视化日志面板。
    ProgressDockWidget* m_progressWidget = nullptr; // 当前操作 Dock 的任务进度卡片面板。
    CodeEditorWidget* m_immediateEditorWidget = nullptr; // 即时窗口统一代码编辑器组件。
    ks::ui::CustomTitleBar* m_customTitleBar = nullptr; // m_customTitleBar：主窗口自绘标题栏组件。
    bool m_windowPinned = false;                        // m_windowPinned：主窗口当前是否置顶。

    // 顶部菜单栏右侧权限按钮（纯文字）：
    // - Admin：管理员权限状态与提权入口；
    // - Debug：SeDebugPrivilege 状态与申请入口；
    // - System：是否 LocalSystem 身份；
    // - TI/R0：TrustedInstaller 与驱动服务快捷开关。
    QWidget* m_privilegeButtonContainer = nullptr;
    QWidget* m_topActionRowWidget = nullptr;     // m_topActionRowWidget：标题栏下方的功能条容器（文件 + 权限按钮）。
    QHBoxLayout* m_topActionRowLayout = nullptr; // m_topActionRowLayout：功能条水平布局。
    QToolButton* m_fileMenuButton = nullptr;     // m_fileMenuButton：功能条左侧“文件”按钮。
    QToolButton* m_settingsMenuButton = nullptr; // m_settingsMenuButton：功能条左侧“设置”入口按钮。
    QPushButton* m_adminStatusButton = nullptr;
    QPushButton* m_debugStatusButton = nullptr;
    QPushButton* m_systemStatusButton = nullptr;
    QPushButton* m_tiStatusButton = nullptr;
    QPushButton* m_r0StatusButton = nullptr;
    bool m_r0DriverServiceRunning = false;      // m_r0DriverServiceRunning：KswordARK 驱动服务当前是否运行。
    std::atomic_bool m_r0DriverLogPollerRunning{ false }; // m_r0DriverLogPollerRunning：R0 日志轮询线程运行标记。
    std::unique_ptr<std::thread> m_r0DriverLogPollerThread; // m_r0DriverLogPollerThread：R0 日志轮询线程对象。
    QTimer* m_privilegeStatusTimer = nullptr;
    QTimer* m_backgroundRebuildTimer = nullptr; // m_backgroundRebuildTimer：窗口背景重建合并计时器。

    // m_currentAppearanceSettings 作用：缓存当前外观配置（主题/背景图/透明度）。
    ks::settings::AppearanceSettings m_currentAppearanceSettings;
    StartupProgressCallback m_startupProgressCallback; // m_startupProgressCallback：主窗口启动阶段进度回调。
    bool m_deferredDockInitializationStarted = false; // m_deferredDockInitializationStarted：是否已启动显示后补载流程。
    std::size_t m_nextDeferredDockIndex = 0;          // m_nextDeferredDockIndex：下一个待补载 Dock 队列索引。
    std::vector<ads::CDockWidget*> m_deferredDockLoadQueue; // m_deferredDockLoadQueue：显示后依次补载的 Dock 队列。
};

#endif // MAINWINDOW_H
