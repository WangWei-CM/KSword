#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QDockWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QDragMoveEvent>

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
#include "OtherDock/OtherDock.h"

class LogDockWidget; // 前置声明：日志 Dock 面板类型。
class ProgressDockWidget; // 前置声明：当前操作进度面板类型。

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private:
    void initMenus();
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
    ads::CDockWidget* m_dockOther;
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
    OtherDock* m_otherWidget;
    LogDockWidget* m_logWidget; // 日志输出 Dock 的可视化日志面板。
    ProgressDockWidget* m_progressWidget; // 当前操作 Dock 的任务进度卡片面板。
};

#endif // MAINWINDOW_H
