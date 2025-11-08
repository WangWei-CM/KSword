#include "MainWindow.h"
#include <QMenu>
#include <QAction>
#include <QTabWidget>
#include <QDockWidget>
#include <QApplication>
#include <QWidget>

#include "UI/UI.css/UI_css.h"
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    QWidget* centralWidget = takeCentralWidget();
        if (centralWidget) delete centralWidget;
    initMenus();
    initCentralTab();
    initDockWidgets();
    setWindowTitle("Ksword5.1");
    resize(1024, 768); // 初始窗口大小
    setStyleSheet(QSS_MainWindow_TabWidget + QSS_MainWindow_dockStyle);
}

MainWindow::~MainWindow()
{
    // Qt父对象会自动释放子部件，此处可省略手动delete
}

void MainWindow::initMenus()
{
    // 文件菜单
    QMenu* fileMenu = menuBar()->addMenu("文件(&F)");

    // 新建动作：占位，无具体行为
    QAction* newAction = new QAction("新建(&N)", this);
    newAction->setShortcut(Qt::CTRL | Qt::Key_N);
    connect(newAction, &QAction::triggered, this, [this]() {
        // 当前不创建新的标签页，保留为占位实现
    });
    fileMenu->addAction(newAction);

    // 打开动作（预留）
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
    menuBar()->addMenu("视图(&V)");
    menuBar()->addMenu("关于(&H)");
}

void MainWindow::initCentralTab()
{
    // 创建11个dock，内容为自定义Widget（空实现）并添加到左侧，然后tabify为一个堆叠
    m_dockWelcome = new QDockWidget("欢迎", this);
    m_dockWelcome->setObjectName("Dock_Welcome");
    m_dockWelcome->setWidget(new WelcomeDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockWelcome);

    m_dockProcess = new QDockWidget("进程", this);
    m_dockProcess->setObjectName("Dock_Process");
    m_dockProcess->setWidget(new ProcessDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockProcess);

    m_dockNetwork = new QDockWidget("网络", this);
    m_dockNetwork->setObjectName("Dock_Network");
    m_dockNetwork->setWidget(new NetworkDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockNetwork);

    m_dockMemory = new QDockWidget("内存", this);
    m_dockMemory->setObjectName("Dock_Memory");
    m_dockMemory->setWidget(new MemoryDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockMemory);

    m_dockFile = new QDockWidget("文件", this);
    m_dockFile->setObjectName("Dock_File");
    m_dockFile->setWidget(new FileDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockFile);

    m_dockDriver = new QDockWidget("驱动", this);
    m_dockDriver->setObjectName("Dock_Driver");
    m_dockDriver->setWidget(new DriverDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockDriver);

    m_dockKernel = new QDockWidget("内核", this);
    m_dockKernel->setObjectName("Dock_Kernel");
    m_dockKernel->setWidget(new KernelDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockKernel);

    m_dockMonitorTab = new QDockWidget("监控", this);
    m_dockMonitorTab->setObjectName("Dock_MonitorTab");
    m_dockMonitorTab->setWidget(new MonitorDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockMonitorTab);

    m_dockPrivilege = new QDockWidget("权限", this);
    m_dockPrivilege->setObjectName("Dock_Privilege");
    m_dockPrivilege->setWidget(new PrivilegeDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockPrivilege);

    m_dockSettings = new QDockWidget("设置", this);
    m_dockSettings->setObjectName("Dock_Settings");
    m_dockSettings->setWidget(new SettingsDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockSettings);

    m_dockOther = new QDockWidget("其它", this);
    m_dockOther->setObjectName("Dock_Other");
    m_dockOther->setWidget(new OtherDock(this));
    addDockWidget(Qt::LeftDockWidgetArea, m_dockOther);

    // 将其堆叠为tab（以欢迎为基准）
    tabifyDockWidget(m_dockWelcome, m_dockProcess);
    tabifyDockWidget(m_dockWelcome, m_dockNetwork);
    tabifyDockWidget(m_dockWelcome, m_dockMemory);
    tabifyDockWidget(m_dockWelcome, m_dockFile);
    tabifyDockWidget(m_dockWelcome, m_dockDriver);
    tabifyDockWidget(m_dockWelcome, m_dockKernel);
    tabifyDockWidget(m_dockWelcome, m_dockMonitorTab);
    tabifyDockWidget(m_dockWelcome, m_dockPrivilege);
    tabifyDockWidget(m_dockWelcome, m_dockSettings);
    tabifyDockWidget(m_dockWelcome, m_dockOther);
    m_dockWelcome->raise(); // 默认显示第一个tab

    // 统一样式与特性
    QList<QDockWidget*> docDocks = { m_dockWelcome, m_dockProcess, m_dockNetwork, m_dockMemory, m_dockFile, m_dockDriver, m_dockKernel, m_dockMonitorTab, m_dockPrivilege, m_dockSettings, m_dockOther };
    for (auto dock : docDocks) {
        dock->setStyleSheet(QSS_MainWindow_dockStyle);
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
        if (dock->widget()) {
            dock->widget()->setStyleSheet("background-color: white; border: none;");
        }
    }
}

void MainWindow::initDockWidgets()
{
    // 右侧停靠窗口：当前操作
    m_dockCurrentOp = new QDockWidget("当前操作", this);
    m_dockCurrentOp->setWidget(new QWidget());
    m_dockCurrentOp->setStyleSheet(QSS_MainWindow_dockStyle);  // 应用样式
    addDockWidget(Qt::RightDockWidgetArea, m_dockCurrentOp);

    // 设置允许 tab 化 与 嵌套
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    // 使用左侧欢迎dock为分割锚点，保证右侧dock不侵占文档区
    splitDockWidget(m_dockWelcome, m_dockCurrentOp, Qt::Horizontal);

    // 右侧停靠窗口：日志输出（与当前操作合并为标签）
    m_dockLog = new QDockWidget("日志输出", this);
    m_dockLog->setWidget(new QWidget());
    m_dockLog->setStyleSheet(QSS_MainWindow_dockStyle);  // 应用样式
    tabifyDockWidget(m_dockCurrentOp, m_dockLog); // QMainWindow的方法

    // 右侧停靠窗口：即时窗口
    m_dockImmediate = new QDockWidget("即时窗口", this);
    m_dockImmediate->setWidget(new QWidget());
    m_dockImmediate->setStyleSheet(QSS_MainWindow_dockStyle);  // 应用样式
    tabifyDockWidget(m_dockLog, m_dockImmediate);

    // 底部停靠窗口：监视面板
    m_dockMonitor = new QDockWidget("监视面板", this);
    m_dockMonitor->setWidget(new QWidget());
    m_dockMonitor->setStyleSheet(QSS_MainWindow_dockStyle);  // 应用样式
    addDockWidget(Qt::BottomDockWidgetArea, m_dockMonitor);

    QList<QDockWidget*> docks = { m_dockCurrentOp, m_dockLog, m_dockImmediate, m_dockMonitor };
    auto dockFeatures = QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetClosable |
        QDockWidget::DockWidgetFloatable;

    for (auto dock : docks) {
        dock->setStyleSheet(QSS_MainWindow_dockStyle); // 统一应用样式
        dock->setFeatures(dockFeatures);              // 统一设置特性
        if (dock->widget()) {
            dock->widget()->setStyleSheet("background-color: white; border: none;");
        }
    }

    m_dockCurrentOp->setFeatures(dockFeatures);
    m_dockLog->setFeatures(dockFeatures);
    m_dockImmediate->setFeatures(dockFeatures);
    m_dockMonitor->setFeatures(dockFeatures);

    // Ensure right/bottom docks are visible (in case they were closed or hidden by layout)
    m_dockCurrentOp->show();
    m_dockLog->show();
    m_dockImmediate->show();
    m_dockMonitor->show();
}
