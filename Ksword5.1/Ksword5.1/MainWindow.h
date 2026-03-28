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
#include <QTimer>
#include <string>

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
#include "PrivilegeDock/PrivilegeDock.h"
#include "SettingsDock/SettingsDock.h"
#include "SettingsDock/AppearanceSettings.h"
#include "WindowDock/WindowDock.h"
#include "RegistryDock/RegistryDock.h"

class LogDockWidget; // 前置声明：日志 Dock 面板类型。
class ProgressDockWidget; // 前置声明：当前操作进度面板类型。
class CodeEditorWidget; // 前置声明：即时窗口可复用代码编辑器组件。

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
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

private:
    void initMenus();
    void initPrivilegeStatusButtons();
    void refreshPrivilegeStatusButtons();
    void applyPrivilegeButtonStyle(QPushButton* button, bool activeState);
    void requestAdminElevationRestart();
    bool hasAdminPrivilege() const;
    bool hasDebugPrivilege() const;
    bool hasSystemPrivilege() const;
    bool hasTrustedInstallerPrivilege() const;
    bool enableSeDebugPrivilege(std::string& errorTextOut) const;
    void initDockWidgets();

    // initAppearanceSettings 作用：
    // - 读取 SettingsDock/JSON 的外观配置；
    // - 绑定系统深浅色变化回调；
    // - 启动时立即应用一次外观。
    // 调用方式：MainWindow 构造末尾调用。
    void initAppearanceSettings();

    void setupDockLayout();

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

    // buildAppearanceOverlayStyleSheet 作用：
    // - 生成深色/浅色覆盖样式字符串，叠加在基础 QSS 之后。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 darkModeEnabled：是否使用深色样式。
    // 返回：拼接后的 QSS 片段。
    QString buildAppearanceOverlayStyleSheet(bool darkModeEnabled) const;

    // ADS Dock Manager
    ads::CDockManager* m_pDockManager;

    // Dock Widgets
    ads::CDockWidget* m_dockWelcome;
    ads::CDockWidget* m_dockProcess;
    ads::CDockWidget* m_dockNetwork;
    ads::CDockWidget* m_dockMemory;
    ads::CDockWidget* m_dockFile;
    ads::CDockWidget* m_dockDriver;
    ads::CDockWidget* m_dockKernel;
    ads::CDockWidget* m_dockMonitorTab;
    ads::CDockWidget* m_dockPrivilege;
    ads::CDockWidget* m_dockSettings;
    ads::CDockWidget* m_dockWindow;
    ads::CDockWidget* m_dockRegistry;
    ads::CDockWidget* m_dockCurrentOp;
    ads::CDockWidget* m_dockLog;
    ads::CDockWidget* m_dockImmediate;
    ads::CDockWidget* m_dockMonitor;

    // 自定义Widgets
    WelcomeDock* m_welcomeWidget;
    ProcessDock* m_processWidget;
    NetworkDock* m_networkWidget;
    MemoryDock* m_memoryWidget;
    FileDock* m_fileWidget;
    DriverDock* m_driverWidget;
    KernelDock* m_kernelWidget;
    MonitorDock* m_monitorWidget;
    PrivilegeDock* m_privilegeWidget;
    SettingsDock* m_settingsWidget;
    WindowDock* m_windowWidget;
    RegistryDock* m_registryWidget;
    LogDockWidget* m_logWidget; // 日志输出 Dock 的可视化日志面板。
    ProgressDockWidget* m_progressWidget; // 当前操作 Dock 的任务进度卡片面板。
    CodeEditorWidget* m_immediateEditorWidget = nullptr; // 即时窗口统一代码编辑器组件。

    // 顶部菜单栏右侧权限按钮（纯文字）：
    // - Admin：管理员权限状态与提权入口；
    // - Debug：SeDebugPrivilege 状态与申请入口；
    // - System：是否 LocalSystem 身份；
    // - TI/PPL：预留权限状态位（当前仅展示状态，不做切换）。
    QWidget* m_privilegeButtonContainer = nullptr;
    QPushButton* m_adminStatusButton = nullptr;
    QPushButton* m_debugStatusButton = nullptr;
    QPushButton* m_systemStatusButton = nullptr;
    QPushButton* m_tiStatusButton = nullptr;
    QPushButton* m_pplStatusButton = nullptr;
    QTimer* m_privilegeStatusTimer = nullptr;

    // m_currentAppearanceSettings 作用：缓存当前外观配置（主题/背景图/透明度）。
    ks::settings::AppearanceSettings m_currentAppearanceSettings;
};

#endif // MAINWINDOW_H
