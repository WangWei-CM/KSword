#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QDockWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>       // 补充QAction头文件（菜单动作需要）
#include <QApplication>  // 补充QApplication头文件
// 新增各dock头文件
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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private:
    void initMenus();
    void initCentralTab();
    void initDockWidgets();

    // 文档tab区域的dock
    QDockWidget* m_dockWelcome;
    QDockWidget* m_dockProcess;
    QDockWidget* m_dockNetwork;
    QDockWidget* m_dockMemory;
    QDockWidget* m_dockFile;
    QDockWidget* m_dockDriver;
    QDockWidget* m_dockKernel;
    QDockWidget* m_dockMonitorTab;
    QDockWidget* m_dockPrivilege;
    QDockWidget* m_dockSettings;
    QDockWidget* m_dockOther;

    // 右侧和底部原有dock
    QDockWidget* m_dockCurrentOp;
    QDockWidget* m_dockLog;
    QDockWidget* m_dockImmediate;
    QDockWidget* m_dockMonitor;
};

#endif // MAINWINDOW_H