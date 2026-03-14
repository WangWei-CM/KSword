#include "MainWindow.h"
#include <QMenu>
#include <QAction>
#include <QTabWidget>
#include <QApplication>
#include <QWidget>
#pragma warning(disable: 4996)
#include "UI/UI.css/UI_css.h"
#include "Framework.h"
#include "Framework/LogDockWidget.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 记录主窗口启动日志，便于验证日志系统与 UI 联动是否生效。
    // 注意：使用 kLogEvent，避免与 QObject::event 命名冲突。
    kLogEvent startupEvent;
    info << startupEvent << "MainWindow 构造开始，准备初始化 Dock 系统。" << eol;

    // 创建ADS Dock Manager
    m_pDockManager = new ads::CDockManager(this);
    // 把 DockManager 设置为主窗口中央控件，确保 Dock 区域可见并可交互。
    setCentralWidget(m_pDockManager);

    // 设置窗口标题和大小
    setWindowTitle("Ksword5.1");
    resize(1024, 768);

    // 初始化菜单
    initMenus();

    // 初始化Dock Widgets
    initDockWidgets();

    // 设置Dock布局
    setupDockLayout();

    // 应用样式表
    setStyleSheet(QSS_MainWindow_TabWidget + QSS_MainWindow_dockStyle);

    // 记录初始化完成日志，方便用户在“日志输出”面板直接看到结果。
    // 注意：使用 kLogEvent，避免与 QObject::event 命名冲突。
    kLogEvent readyEvent;
    info << readyEvent << "MainWindow 初始化完成，日志面板已加载。" << eol;
}

MainWindow::~MainWindow()
{
    // ADS会自动管理内存，无需手动删除
}

void MainWindow::initMenus()
{
    // 文件菜单
    QMenu* fileMenu = menuBar()->addMenu("文件(&F)");

    // 新建动作
    QAction* newAction = new QAction("新建(&N)", this);
    newAction->setShortcut(Qt::CTRL | Qt::Key_N);
    connect(newAction, &QAction::triggered, this, [this]() {
        // 占位实现
        });
    fileMenu->addAction(newAction);

    // 打开动作
    QAction* openAction = new QAction("打开(&O)", this);
    openAction->setShortcut(Qt::CTRL | Qt::Key_O);
    fileMenu->addAction(openAction);

    fileMenu->addSeparator();

    // 退出动作
    QAction* exitAction = new QAction("退出(&X)", this);
    exitAction->setShortcut(Qt::CTRL | Qt::Key_Q);
    connect(exitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);
    fileMenu->addAction(exitAction);

    // 其他菜单
    menuBar()->addMenu("编辑(&E)");

    // 视图菜单将在initDockWidgets后添加Dock切换动作
}

void MainWindow::initDockWidgets()
{
    // 创建自定义Widgets
    m_welcomeWidget = new WelcomeDock(this);
    m_processWidget = new ProcessDock(this);
    m_networkWidget = new NetworkDock(this);
    m_memoryWidget = new MemoryDock(this);
    m_fileWidget = new FileDock(this);
    m_driverWidget = new DriverDock(this);
    m_kernelWidget = new KernelDock(this);
    m_monitorWidget = new MonitorDock(this);
    m_privilegeWidget = new PrivilegeDock(this);
    m_settingsWidget = new SettingsDock(this);
    m_otherWidget = new OtherDock(this);
    m_logWidget = new LogDockWidget(this);

    // 使用辅助函数创建Dock Widgets
    auto createDockWidget = [this](QWidget* widget, const QString& title) -> ads::CDockWidget* {
        ads::CDockWidget* dock = new ads::CDockWidget(title);
        dock->setWidget(widget);
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetMovable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, true);
        return dock;
        };

    // 创建所有Dock Widgets
    m_dockWelcome = createDockWidget(m_welcomeWidget, "欢迎");
    m_dockProcess = createDockWidget(m_processWidget, "进程");
    m_dockNetwork = createDockWidget(m_networkWidget, "网络");
    m_dockMemory = createDockWidget(m_memoryWidget, "内存");
    m_dockFile = createDockWidget(m_fileWidget, "文件");
    m_dockDriver = createDockWidget(m_driverWidget, "驱动");
    m_dockKernel = createDockWidget(m_kernelWidget, "内核");
    m_dockMonitorTab = createDockWidget(m_monitorWidget, "监控");
    m_dockPrivilege = createDockWidget(m_privilegeWidget, "权限");
    m_dockSettings = createDockWidget(m_settingsWidget, "设置");
    m_dockOther = createDockWidget(m_otherWidget, "其它");

    // 创建右侧和底部的基本Widgets
    m_dockCurrentOp = createDockWidget(new QWidget(), "当前操作");
    m_dockLog = createDockWidget(m_logWidget, "日志输出");
    m_dockImmediate = createDockWidget(new QWidget(), "即时窗口");
    m_dockMonitor = createDockWidget(new QWidget(), "监视面板");

    // 将Dock Widget的切换动作添加到菜单
    QMenu* viewMenu = menuBar()->addMenu("视图(&V)");
    QList<ads::CDockWidget*> allDocks = {
        m_dockWelcome, m_dockProcess, m_dockNetwork, m_dockMemory,
        m_dockFile, m_dockDriver, m_dockKernel, m_dockMonitorTab,
        m_dockPrivilege, m_dockSettings, m_dockOther,
        m_dockCurrentOp, m_dockLog, m_dockImmediate, m_dockMonitor
    };

    for (auto dock : allDocks) {
        viewMenu->addAction(dock->toggleViewAction());
    }
}
#define ADS_TABIFY_DOCK_WIDGET_AVAILABLE
void MainWindow::setupDockLayout()
{
    // 1. 初始化DockManager（若未在构造函数中初始化）
    if (!m_pDockManager) {
        m_pDockManager = new ads::CDockManager(this);
        setCentralWidget(m_pDockManager);
    }

    // 2. 左侧区域：先添加第一个DockWidget，获取其所在的DockArea
    auto leftDockArea = m_pDockManager->addDockWidget(ads::LeftDockWidgetArea, m_dockWelcome);

    // 3. 使用正确的方法将其他DockWidget添加到同一个DockArea形成标签页
    // 方法1: 使用addDockWidgetTabToArea（推荐）
    m_pDockManager->addDockWidgetTabToArea(m_dockProcess, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockNetwork, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockMemory, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockFile, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockDriver, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockKernel, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockMonitorTab, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockPrivilege, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockSettings, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockOther, leftDockArea);

    // 方法2: 或者使用addDockWidget并指定CenterDockWidgetArea
    // m_pDockManager->addDockWidget(ads::CenterDockWidgetArea, m_dockProcess, leftDockArea);

    // 4. 右侧区域：同理
    auto rightDockArea = m_pDockManager->addDockWidget(ads::RightDockWidgetArea, m_dockCurrentOp);

    // 使用正确的方法添加标签页
    m_pDockManager->addDockWidgetTabToArea(m_dockLog, rightDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockImmediate, rightDockArea);

    // 5. 底部区域（单独显示，不合并到标签页）
    m_pDockManager->addDockWidget(ads::BottomDockWidgetArea, m_dockMonitor);

    // 6. 设置默认显示的标签页
    m_dockWelcome->raise();
    m_dockCurrentOp->raise();
}
