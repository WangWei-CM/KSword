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
#include "WindowDock/WindowDock.h"
#include "RegistryDock/RegistryDock.h"

class LogDockWidget; // 前置声明：日志 Dock 面板类型。
class ProgressDockWidget; // 前置声明：当前操作进度面板类型。

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
    void setupDockLayout();

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
};

#endif // MAINWINDOW_H
