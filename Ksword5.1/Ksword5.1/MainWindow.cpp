#include "MainWindow.h"
#include <QMenu>
#include <QAction>
#include <QTabWidget>
#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QPainter>
#include <QPalette>
#include <QPointF>
#include <QPixmap>
#include <QRectF>
#include <QResizeEvent>
#include <QImageReader>
#include <QMessageBox>
#include <QToolTip>
#include <QStyleHints>
#pragma warning(disable: 4996)
#include "UI/UI.css/UI_css.h"
#include "Framework.h"
#include "Framework/LogDockWidget.h"
#include "Framework/ProgressDockWidget.h"
#include "Framework/ThemedMessageBox.h"
#include "UI/CodeEditorWidget.h"
#include "theme.h"
#include <windows.h>
// 菜单栏权限按钮涉及 Windows 令牌权限查询与提权动作。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <sddl.h>

#include <array>
#include <vector>
#include <TlHelp32.h>

namespace
{
    // kTooltipStyleBeginMarker / kTooltipStyleEndMarker 作用：
    // - 在 QApplication 样式表中标记“Tooltip 主题片段”的起止位置；
    // - 便于主题切换时精准替换旧 Tooltip 样式，避免重复拼接。
    constexpr const char* kTooltipStyleBeginMarker = "/*KSWORD_TOOLTIP_STYLE_BEGIN*/";
    constexpr const char* kTooltipStyleEndMarker = "/*KSWORD_TOOLTIP_STYLE_END*/";

    // buildPrivilegeButtonStyle 作用：
    // - 按“当前是否具备权限”生成按钮样式；
    // - true  -> 蓝底白字；
    // - false -> 白底蓝字。
    QString buildPrivilegeButtonStyle(const bool activeState)
    {
        const QString backgroundColor = activeState
            ? KswordTheme::PrimaryBlueHex
            : KswordTheme::SurfaceHex();
        const QString textColor = activeState
            ? QStringLiteral("#FFFFFF")
            : (KswordTheme::IsDarkModeEnabled() ? KswordTheme::TextPrimaryHex() : KswordTheme::PrimaryBlueHex);
        const QString hoverColor = activeState
            ? QStringLiteral("#2E8BFF")
            : (KswordTheme::IsDarkModeEnabled() ? QStringLiteral("#2A2A2A") : QStringLiteral("#2E8BFF"));
        return QStringLiteral(
            "QPushButton {"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:2px 8px;"
            "  font-weight:600;"
            "}"
            "QPushButton:hover {"
            "  background:%4;"
            "  color:#FFFFFF;"
            "  border:1px solid %4;"
            "}"
            "QPushButton:pressed {"
            "  background:%5;"
            "  color:#FFFFFF;"
            "}")
            .arg(backgroundColor)
            .arg(textColor)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(hoverColor)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // normalizeOpacityPercent 作用：
    // - 把透明度值限制在 0~100 范围，防止非法参数影响绘制。
    // 调用方式：生成背景画刷前调用。
    // 入参 rawOpacityPercent：原始透明度值。
    // 返回：合法透明度值。
    int normalizeOpacityPercent(const int rawOpacityPercent)
    {
        if (rawOpacityPercent < 0)
        {
            return 0;
        }
        if (rawOpacityPercent > 100)
        {
            return 100;
        }
        return rawOpacityPercent;
    }

    // buildBackgroundBrush 作用：
    // - 按“纯色底 + 可选背景图 + 透明度”组合一张画刷贴图；
    // - 用于主窗口与 Dock 管理器统一背景绘制。
    // 调用方式：MainWindow::rebuildWindowBackgroundBrush 调用。
    // 入参 windowSize：目标窗口尺寸；
    // 入参 baseColor：主题基底色（深色黑、浅色白）；
    // 入参 imagePath：背景图绝对路径；
    // 入参 imageOpacityPercent：背景图透明度（0~100）。
    // 返回：可直接设置到 QPalette::Window 的画刷。
    QBrush buildBackgroundBrush(
        const QSize& windowSize,
        const QColor& baseColor,
        const QString& imagePath,
        const int imageOpacityPercent)
    {
        const QSize safeSize = windowSize.isValid() ? windowSize : QSize(1, 1);
        QPixmap composedPixmap(safeSize);
        composedPixmap.fill(baseColor);

        if (!imagePath.trimmed().isEmpty())
        {
            QPixmap sourceImage(imagePath);
            if (!sourceImage.isNull())
            {
                QPainter painter(&composedPixmap);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.setOpacity(static_cast<double>(normalizeOpacityPercent(imageOpacityPercent)) / 100.0);

                // scaledImageSize 作用：按“覆盖整个窗口”策略计算缩放尺寸。
                QSizeF scaledImageSize = sourceImage.size();
                scaledImageSize.scale(safeSize, Qt::KeepAspectRatioByExpanding);

                const QRectF targetRect(
                    (static_cast<double>(safeSize.width()) - scaledImageSize.width()) / 2.0,
                    (static_cast<double>(safeSize.height()) - scaledImageSize.height()) / 2.0,
                    scaledImageSize.width(),
                    scaledImageSize.height());

                painter.drawPixmap(targetRect, sourceImage, QRectF(QPointF(0, 0), sourceImage.size()));
            }
        }

        return QBrush(composedPixmap);
    }

    // buildGlobalTooltipStyleBlock 作用：
    // - 生成全局 Tooltip 样式片段；
    // - 片段带起止标记，供 applyGlobalTooltipStyleBlock 做替换更新。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 darkModeEnabled：当前是否深色模式。
    // 返回：可直接拼接到 QApplication 样式表的 Tooltip 片段。
    QString buildGlobalTooltipStyleBlock(const bool darkModeEnabled)
    {
        // tooltipRule 作用：QToolTip 规则本体，统一深浅色背景与文字。
        const QString tooltipRule = QStringLiteral(
            "QToolTip{"
            "  background-color:%1 !important;"
            "  color:%2 !important;"
            "  border:1px solid %3 !important;"
            "  padding:4px 6px;"
            "  border-radius:3px;"
            "}")
            .arg(darkModeEnabled ? QStringLiteral("#181818") : QStringLiteral("#FFFFFF"))
            .arg(darkModeEnabled ? QStringLiteral("#FFFFFF") : QStringLiteral("#000000"))
            .arg(KswordTheme::PrimaryBlueHex);

        return QStringLiteral("\n%1\n%2\n%3\n")
            .arg(QString::fromLatin1(kTooltipStyleBeginMarker))
            .arg(tooltipRule)
            .arg(QString::fromLatin1(kTooltipStyleEndMarker));
    }

    // applyGlobalTooltipStyleBlock 作用：
    // - 把 Tooltip 样式片段写入 QApplication 样式表；
    // - 先删除旧标记片段，再追加新片段，确保切换主题后 Tooltip 立即生效。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 tooltipStyleBlock：buildGlobalTooltipStyleBlock 生成的样式片段。
    void applyGlobalTooltipStyleBlock(const QString& tooltipStyleBlock)
    {
        QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (appInstance == nullptr)
        {
            return;
        }

        // appStyleSheetText 作用：读取并更新 QApplication 当前样式表文本。
        QString appStyleSheetText = appInstance->styleSheet();
        const QString beginMarkerText = QString::fromLatin1(kTooltipStyleBeginMarker);
        const QString endMarkerText = QString::fromLatin1(kTooltipStyleEndMarker);
        const int beginMarkerIndex = appStyleSheetText.indexOf(beginMarkerText);

        if (beginMarkerIndex >= 0)
        {
            const int endMarkerIndex = appStyleSheetText.indexOf(endMarkerText, beginMarkerIndex);
            if (endMarkerIndex >= 0)
            {
                const int removeLength = (endMarkerIndex - beginMarkerIndex) + endMarkerText.length();
                appStyleSheetText.remove(beginMarkerIndex, removeLength);
            }
        }

        appStyleSheetText += tooltipStyleBlock;
        appInstance->setStyleSheet(appStyleSheetText);
    }

    // isBackgroundImageReady 作用：
    // - 判断背景图路径是否可用（存在且可读）；
    // - 供样式层决定是否开启 Dock 全透明策略。
    // 调用方式：applyAppearanceSettings 内部调用。
    // 入参 rawImagePath：配置中的背景图路径（相对或绝对）。
    // 返回：true=背景图可加载；false=背景图不可用。
    bool isBackgroundImageReady(const QString& rawImagePath)
    {
        const QString resolvedImagePath = ks::settings::resolveBackgroundImagePathForLoad(rawImagePath);
        if (resolvedImagePath.trimmed().isEmpty())
        {
            return false;
        }

        const QFileInfo imageFileInfo(resolvedImagePath);
        return imageFileInfo.exists() && imageFileInfo.isFile() && imageFileInfo.isReadable();
    }
}

MainWindow::MainWindow(
    QWidget* parent,
    StartupProgressCallback startupProgressCallback)
    : QMainWindow(parent)
    , m_startupProgressCallback(startupProgressCallback)
{
    // 记录主窗口启动日志，便于验证日志系统与 UI 联动是否生效。
    // 注意：使用 kLogEvent，避免与 QObject::event 命名冲突。
    kLogEvent startupEvent;
    info << startupEvent << "MainWindow 构造开始，准备初始化 Dock 系统。" << eol;

    // 启动阶段细分：
    // - 主窗口外壳；
    // - 菜单；
    // - 权限按钮；
    // - Dock 内容；
    // - 外观系统。
    reportStartupProgress(32, QStringLiteral("正在初始化主窗口框架..."));

    // Dock 全局配置：
    // - 关闭所有可关闭按钮与标题栏三按钮（标签菜单/浮动/关闭）；
    // - 与用户“所有 Dock Tab 不可关闭、去掉右上角按钮”的要求一致。
    ads::CDockManager::setConfigFlag(ads::CDockManager::ActiveTabHasCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);

    // 创建ADS Dock Manager
    m_pDockManager = new ads::CDockManager(this);
    // 把 DockManager 设置为主窗口中央控件，确保 Dock 区域可见并可交互。
    setCentralWidget(m_pDockManager);

    // 浮动窗口创建后立即挂接事件过滤器：
    // - 让脱离主窗口的 Dock 容器也能同步背景图与纯色底；
    // - 后续 resize/show 时统一走 MainWindow 的外观同步逻辑。
    connect(
        m_pDockManager,
        &ads::CDockManager::floatingWidgetCreated,
        this,
        [this](ads::CFloatingDockContainer* floatingWidget)
        {
            if (floatingWidget == nullptr)
            {
                return;
            }
            floatingWidget->installEventFilter(this);
            applyFloatingDockContainerAppearance(floatingWidget);
        });

    // 显式要求“最后一个窗口关闭后退出应用”。
    // 说明：用户要求主窗口关闭时进程必须结束，这里先设置 Qt 全局退出策略。
    QApplication::setQuitOnLastWindowClosed(true);

    // 设置窗口标题和大小
    setWindowTitle("Ksword5.1");
    resize(1024, 768);

    // 初始化菜单
    reportStartupProgress(38, QStringLiteral("正在初始化菜单..."));
    initMenus();

    // 初始化权限状态按钮：
    // - 从菜单初始化中拆开；
    // - 便于启动画面展示更细的进度阶段。
    reportStartupProgress(44, QStringLiteral("正在初始化权限状态按钮..."));
    initPrivilegeStatusButtons();

    // 初始化Dock Widgets
    reportStartupProgress(48, QStringLiteral("正在创建页面组件..."));
    initDockWidgets();

    // 设置Dock布局
    reportStartupProgress(74, QStringLiteral("正在整理 Dock 布局..."));
    setupDockLayout();

    // 初始化外观设置：
    // - 读取 JSON；
    // - 绑定系统深浅色变化；
    // - 应用窗口背景色/背景图/文本颜色。
    reportStartupProgress(84, QStringLiteral("正在初始化主题与外观..."));
    initAppearanceSettings();

    // 记录初始化完成日志，方便用户在“日志输出”面板直接看到结果。
    // 注意：使用 kLogEvent，避免与 QObject::event 命名冲突。
    kLogEvent readyEvent;
    info << readyEvent << "MainWindow 初始化完成，日志面板已加载。" << eol;
    reportStartupProgress(93, QStringLiteral("主窗口初始化完成，准备显示..."));
}

MainWindow::~MainWindow()
{
    // ADS会自动管理内存，无需手动删除
}

void MainWindow::reportStartupProgress(
    const int progressPercent,
    const QString& statusText) const
{
    // 安全回调策略：
    // - 未传入回调则静默跳过；
    // - 有回调时把阶段百分比与文字原样转发给主函数。
    if (!m_startupProgressCallback)
    {
        return;
    }

    m_startupProgressCallback(progressPercent, statusText);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 关闭事件日志：用于排查“窗口关闭但进程仍驻留”的问题。
    kLogEvent closeEventLog;
    info << closeEventLog << "[MainWindow] 收到关闭事件，准备退出进程。" << eol;

    // 停止权限状态定时器，避免退出阶段继续触发 UI 更新。
    if (m_privilegeStatusTimer != nullptr)
    {
        m_privilegeStatusTimer->stop();
    }

    // 接受关闭事件并主动触发应用退出。
    // 这里同时调用 quit 与 exit(0)，确保事件循环尽快结束。
    if (event != nullptr)
    {
        event->accept();
    }
    QApplication::quit();
    QCoreApplication::exit(0);

    kLogEvent closeFinishLog;
    info << closeFinishLog << "[MainWindow] 已提交退出请求 (exit code=0)。" << eol;
}

bool MainWindow::eventFilter(QObject* watchedObject, QEvent* event)
{
    ads::CFloatingDockContainer* floatingWidget =
        qobject_cast<ads::CFloatingDockContainer*>(watchedObject);
    if (floatingWidget != nullptr && event != nullptr)
    {
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
        {
            applyFloatingDockContainerAppearance(floatingWidget);
        }
    }

    return QMainWindow::eventFilter(watchedObject, event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    rebuildWindowBackgroundBrush();
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

void MainWindow::initPrivilegeStatusButtons()
{
    // 防重复初始化：若已创建容器则只刷新一次状态。
    if (m_privilegeButtonContainer != nullptr)
    {
        refreshPrivilegeStatusButtons();
        return;
    }

    // 在菜单栏右上角放置一个水平容器，承载权限状态按钮。
    m_privilegeButtonContainer = new QWidget(menuBar());
    QHBoxLayout* buttonLayout = new QHBoxLayout(m_privilegeButtonContainer);
    buttonLayout->setContentsMargins(0, 0, 6, 0);
    buttonLayout->setSpacing(6);

    // 按钮文本采用纯文字，满足用户要求。
    m_adminStatusButton = new QPushButton("Admin", m_privilegeButtonContainer);
    m_debugStatusButton = new QPushButton("Debug", m_privilegeButtonContainer);
    m_systemStatusButton = new QPushButton("System", m_privilegeButtonContainer);
    m_tiStatusButton = new QPushButton("TI", m_privilegeButtonContainer);
    m_pplStatusButton = new QPushButton("PPL", m_privilegeButtonContainer);

    // 统一按钮尺寸，保证右上角布局整齐。
    const std::array<QPushButton*, 5> statusButtons{
        m_adminStatusButton,
        m_debugStatusButton,
        m_systemStatusButton,
        m_tiStatusButton,
        m_pplStatusButton
    };
    for (QPushButton* statusButton : statusButtons)
    {
        if (statusButton == nullptr)
        {
            continue;
        }
        statusButton->setFixedHeight(22);
        statusButton->setMinimumWidth(56);
        buttonLayout->addWidget(statusButton);
    }

    // 先占位说明：TI / PPL 状态位目前仅展示（后续可扩展实现）。
    m_tiStatusButton->setToolTip("TrustedInstaller 状态位（预留）");
    m_pplStatusButton->setToolTip("PPL 状态位（预留）");

    // 把容器挂到菜单栏右上角。
    menuBar()->setCornerWidget(m_privilegeButtonContainer, Qt::TopRightCorner);

    // Admin 按钮：
    // - 已是管理员：仅提示当前状态；
    // - 非管理员：立刻触发 runas 重启提权。
    connect(m_adminStatusButton, &QPushButton::clicked, this, [this]() {
        if (hasAdminPrivilege())
        {
            QMessageBox::information(this, "Admin", "当前已是管理员权限。");
            refreshPrivilegeStatusButtons();
            return;
        }

        kLogEvent logEvent;
        warn << logEvent << "[MainWindow] Admin 按钮触发提权重启。" << eol;
        requestAdminElevationRestart();
    });

    // Debug 按钮：
    // - 非管理员：按需求先执行 Admin 提权动作；
    // - 已管理员：申请 SeDebugPrivilege。
    connect(m_debugStatusButton, &QPushButton::clicked, this, [this]() {
        if (!hasAdminPrivilege())
        {
            kLogEvent logEvent;
            warn << logEvent << "[MainWindow] Debug 按钮检测到非管理员，转为执行 Admin 提权。" << eol;
            requestAdminElevationRestart();
            return;
        }

        std::string errorText;
        const bool enableOk = enableSeDebugPrivilege(errorText);
        if (enableOk)
        {
            kLogEvent logEvent;
            info << logEvent << "[MainWindow] SeDebugPrivilege 申请成功。" << eol;
            QMessageBox::information(this, "Debug", "SeDebugPrivilege 已启用。");
        }
        else
        {
            kLogEvent logEvent;
            err << logEvent << "[MainWindow] SeDebugPrivilege 申请失败: " << errorText << eol;
            QMessageBox::warning(this, "Debug", QString("SeDebugPrivilege 启用失败。\n%1").arg(QString::fromStdString(errorText)));
        }
        refreshPrivilegeStatusButtons();
    });

    // System 按钮仅展示状态，点击给出提示（普通用户态无法直接“切到 SYSTEM”）。
    connect(m_systemStatusButton, &QPushButton::clicked, this, [this]() {
        if (hasSystemPrivilege())
        {
            QMessageBox::information(this, "System", "当前进程已经是 LocalSystem 身份。");
        }
        else
        {
            HANDLE hToken = NULL;          // 当前进程的令牌句柄
            LUID Luid;                     // 特权局部唯一标识符
            TOKEN_PRIVILEGES tp;           // 令牌特权结构体

            // 打开当前进程的令牌，要求调整特权与查询权限
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken failed, error: " << GetLastError() << eol;
                return 1;
            }

            // 查找 SE_DEBUG_NAME 特权的 LUID
            if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Luid))
            {
                kLogEvent logEvent;
                err << logEvent << "LookupPrivilegeValue failed, error: " << GetLastError() << eol;
                CloseHandle(hToken);
                return 1;
            }

            // 设置特权属性为启用
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = Luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

            // 调整令牌特权
            if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
            {
                kLogEvent logEvent;
                err << logEvent << "AdjustTokenPrivileges failed, error: " << GetLastError() << eol;
                CloseHandle(hToken);
                return 1;
            }
            // 检查特权是否真正被启用（AdjustTokenPrivileges 可能成功但未全部分配）
            if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
            {
                kLogEvent logEvent;
                err << logEvent << "AdjustTokenPrivileges: SeDebugPrivilege not assigned" << eol;
                CloseHandle(hToken);
                return 1;
            }
            CloseHandle(hToken);  // 特权已启用，关闭临时句柄

            // ========== 第二步：枚举进程，获取 lsass.exe 和 winlogon.exe 的 PID ==========
            DWORD idL = 0;                     // 存放 lsass.exe 的进程ID
            DWORD idW = 0;                     // 存放 winlogon.exe 的进程ID
            PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };  // 进程快照条目（宽字符）
            HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);  // 进程快照句柄
            if (hSnapshot == INVALID_HANDLE_VALUE)
            {
                kLogEvent logEvent;
                err << logEvent << "CreateToolhelp32Snapshot failed, error: " << GetLastError() << eol;
                return 1;
            }

            // 遍历进程快照，匹配目标进程名
            if (Process32FirstW(hSnapshot, &pe))
            {
                do
                {
                    if (_wcsicmp(pe.szExeFile, L"lsass.exe") == 0)
                    {
                        idL = pe.th32ProcessID;
                        kLogEvent logEvent;
                        info << logEvent << "Found lsass.exe with PID: " << idL << eol;
                    }
                    else if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0)
                    {
                        idW = pe.th32ProcessID;
                        kLogEvent logEvent;
                        info << logEvent << "Found winlogon.exe with PID: " << idW << eol;
                    }
                } while (Process32NextW(hSnapshot, &pe));
            }
            CloseHandle(hSnapshot);

            // ========== 第三步：打开目标进程（优先 lsass，其次 winlogon） ==========
            HANDLE hProcess = NULL;          // 目标进程句柄
            if (idL != 0)
                hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, idL);
            if (!hProcess && idW != 0)
                hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, idW);
            if (!hProcess)
            {
                kLogEvent logEvent;
                err << logEvent << "Failed to open target process (lsass/winlogon), error: " << GetLastError() << eol;
                return 1;
            }
            {
                kLogEvent logEvent;
                info << logEvent << "Opened target process" << eol;
            }

            // ========== 第四步：打开目标进程的令牌 ==========
            HANDLE hTokenx = NULL;           // 目标进程的令牌句柄
            if (!OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hTokenx))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken on target process failed, error: " << GetLastError() << eol;
                CloseHandle(hProcess);
                return 1;
            }

            // ========== 第五步：复制令牌，获得可用的主令牌 ==========
            HANDLE hNewToken = NULL;         // 复制得到的新令牌句柄
            if (!DuplicateTokenEx(hTokenx, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hNewToken))
            {
                kLogEvent logEvent;
                err << logEvent << "DuplicateTokenEx failed, error: " << GetLastError() << eol;
                CloseHandle(hTokenx);
                CloseHandle(hProcess);
                return 1;
            }
            CloseHandle(hTokenx);
            CloseHandle(hProcess);

            // ========== 第六步：获取当前程序自身的路径 ==========
            std::wstring selfPath = ks::process::GetCurrentProcessPath();
            if (selfPath.empty())
            {
                kLogEvent logEvent;
                err << logEvent <<"Failed to get current process path" << eol;
                CloseHandle(hNewToken);
                return 1;
            }
            {
                kLogEvent logEvent;
                // pathUtf8 用途：把 UTF-16 路径转换为 UTF-8，避免日志流(std::ostringstream)直接输出 std::wstring 导致编译错误。
                const std::string pathUtf8 = ks::str::Utf16ToUtf8(selfPath);
                info << logEvent << "Current process path: " << pathUtf8 << eol;
            }

            // ========== 第七步：使用复制得到的令牌启动自身 ==========
            STARTUPINFOW si = { sizeof(STARTUPINFOW) };
            PROCESS_INFORMATION pi = { 0 };
            // 为 lpDesktop 使用可写缓冲区（避免 const wchar_t* 赋值给 LPWSTR 的编译错误）
            wchar_t desktop[] = L"winsta0\\default";
            si.lpDesktop = desktop;          // 显示在交互式桌面

            if (!CreateProcessWithTokenW(hNewToken, LOGON_NETCREDENTIALS_ONLY, selfPath.c_str(), NULL,
                NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi))
            {
                kLogEvent logEvent;
                err << logEvent << "CreateProcessWithTokenW failed, error: " << GetLastError() << eol;
                CloseHandle(hNewToken);
                return 1;
            }

            {
                kLogEvent logEvent;
                info << logEvent << "Successfully started new instance of the program. New PID: " << pi.dwProcessId << eol;
            }

            // 清理资源
            CloseHandle(hNewToken);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    });

    // TI 按钮：展示 TrustedInstaller 身份状态。
    // 说明：仅做状态检查，不提供自动令牌窃取/注入类提权逻辑。
    connect(m_tiStatusButton, &QPushButton::clicked, this, [this]() {
        if (hasTrustedInstallerPrivilege())
        {
            QMessageBox::information(this, "TI", "当前进程已经是 TrustedInstaller 身份。");
        }
        else
        {
            {
                kLogEvent logEvent;
                info << logEvent << "Starting self with TrustedInstaller privilege..." << eol;
            }

            // ========== 第 2 步：模拟 SYSTEM 账户（通过 winlogon.exe） ==========
            // 2.1 获取 winlogon.exe 的进程 ID
            DWORD winlogonPid = 0;
            HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnapshot == INVALID_HANDLE_VALUE)
            {
                kLogEvent logEvent;
                err << logEvent << "CreateToolhelp32Snapshot failed, error: " << GetLastError() << eol;
                return 1;
            }
            PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
            if (Process32FirstW(hSnapshot, &pe))
            {
                do
                {
                    if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0)
                    {
                        winlogonPid = pe.th32ProcessID;
                        break;
                    }
                } while (Process32NextW(hSnapshot, &pe));
            }
            CloseHandle(hSnapshot);
            if (winlogonPid == 0)
            {
                kLogEvent logEvent;
                err << logEvent << "winlogon.exe not found" << eol;
                return 1;
            }

            // 2.2 打开 winlogon.exe 进程
            HANDLE hWinlogon = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, winlogonPid);
            if (!hWinlogon)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcess(winlogon.exe) failed, error: " << GetLastError() << eol;
                return 1;
            }

            // 2.3 打开 winlogon 进程的令牌
            HANDLE hWinlogonToken = NULL;
            if (!OpenProcessToken(hWinlogon, MAXIMUM_ALLOWED, &hWinlogonToken))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken(winlogon.exe) failed, error: " << GetLastError() << eol;
                CloseHandle(hWinlogon);
                return 1;
            }

            // 2.4 复制 winlogon 令牌为模拟令牌
            HANDLE hSystemImpersonationToken = NULL;
            SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };
            if (!DuplicateTokenEx(hWinlogonToken, MAXIMUM_ALLOWED, &sa, SecurityImpersonation, TokenImpersonation, &hSystemImpersonationToken))
            {
                kLogEvent logEvent;
                err << logEvent << "DuplicateTokenEx(winlogon) failed, error: " << GetLastError() << eol;
                CloseHandle(hWinlogonToken);
                CloseHandle(hWinlogon);
                return 1;
            }

            // 2.5 模拟 SYSTEM 账户（当前线程获得 SYSTEM 上下文）
            if (!ImpersonateLoggedOnUser(hSystemImpersonationToken))
            {
                kLogEvent logEvent;
                err << logEvent << "ImpersonateLoggedOnUser failed, error: " << GetLastError() << eol;
                CloseHandle(hSystemImpersonationToken);
                CloseHandle(hWinlogonToken);
                CloseHandle(hWinlogon);
                return 1;
            }
            {
                kLogEvent logEvent;
                info << logEvent << "Successfully impersonated SYSTEM via winlogon.exe" << eol;
            }

            // 清理 winlogon 相关句柄（模拟令牌已生效，句柄可关闭）
            CloseHandle(hSystemImpersonationToken);
            CloseHandle(hWinlogonToken);
            CloseHandle(hWinlogon);

            // ========== 第 3 步：启动 TrustedInstaller 服务并获取其进程 ID ==========
            SC_HANDLE hSCM = OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, GENERIC_EXECUTE);
            if (!hSCM)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenSCManagerW failed, error: " << GetLastError() << eol;
                return 1;
            }
            SC_HANDLE hService = OpenServiceW(hSCM, L"TrustedInstaller", GENERIC_READ | GENERIC_EXECUTE);
            if (!hService)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenServiceW(TrustedInstaller) failed, error: " << GetLastError() << eol;
                CloseServiceHandle(hSCM);
                return 1;
            }

            DWORD tiPid = 0;
            SERVICE_STATUS_PROCESS status = { 0 };
            DWORD bytesNeeded = 0;
            // 循环等待服务进入运行状态
            while (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded))
            {
                if (status.dwCurrentState == SERVICE_RUNNING)
                {
                    tiPid = status.dwProcessId;
                    {
                        kLogEvent logEvent;
                        info << logEvent << "TrustedInstaller service is running, PID: " << tiPid << eol;
                    }
                    break;
                }
                else if (status.dwCurrentState == SERVICE_STOPPED)
                {
                    if (!StartServiceW(hService, 0, nullptr))
                    {
                        kLogEvent logEvent;
                        err << logEvent << "StartServiceW(TrustedInstaller) failed, error: " << GetLastError() << eol;
                        CloseServiceHandle(hService);
                        CloseServiceHandle(hSCM);
                        return 1;
                    }
                    {
                        kLogEvent logEvent;
                        info << logEvent << "Started TrustedInstaller service, waiting..." << eol;
                    }
                    // 继续循环等待服务启动完成
                    continue;
                }
                else if (status.dwCurrentState == SERVICE_START_PENDING || status.dwCurrentState == SERVICE_STOP_PENDING)
                {
                    Sleep(status.dwWaitHint);
                    continue;
                }
                else
                {
                    // 其他状态（异常）
                    break;
                }
            }
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            if (tiPid == 0)
            {
                kLogEvent logEvent;
                err << logEvent << "Failed to get TrustedInstaller PID" << eol;
                return 1;
            }

            // ========== 第 4 步：打开 TrustedInstaller 进程并获取其令牌 ==========
            HANDLE hTiProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, tiPid);
            if (!hTiProcess)
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcess(TrustedInstaller.exe) failed, error: " << GetLastError() << eol;
                return 1;
            }
            HANDLE hTiToken = NULL;
            if (!OpenProcessToken(hTiProcess, MAXIMUM_ALLOWED, &hTiToken))
            {
                kLogEvent logEvent;
                err << logEvent << "OpenProcessToken(TrustedInstaller.exe) failed, error: " << GetLastError() << eol;
                CloseHandle(hTiProcess);
                return 1;
            }

            // 复制 TrustedInstaller 令牌为主令牌（TokenPrimary）
            HANDLE hNewToken = NULL;
            if (!DuplicateTokenEx(hTiToken, MAXIMUM_ALLOWED, &sa, SecurityImpersonation, TokenPrimary, &hNewToken))
            {
                kLogEvent logEvent;
                err << logEvent << "DuplicateTokenEx(TrustedInstaller) failed, error: " << GetLastError() << eol;
                CloseHandle(hTiToken);
                CloseHandle(hTiProcess);
                return 1;
            }
            CloseHandle(hTiToken);
            CloseHandle(hTiProcess);

            // ========== 第 5 步：获取当前进程自身路径 ==========
            wchar_t selfPath[MAX_PATH] = { 0 };
            if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0)
            {
                kLogEvent logEvent;
                err << logEvent << "GetModuleFileNameW failed, error: " << GetLastError() << eol;
                CloseHandle(hNewToken);
                return 1;
            }

            // ========== 第 6 步：使用 TrustedInstaller 令牌启动自身 ==========
            STARTUPINFOW si = { sizeof(STARTUPINFOW) };
            wchar_t desktop[] = L"winsta0\\default";   // 可写缓冲区
            si.lpDesktop = desktop;
            PROCESS_INFORMATION pi = { 0 };
            if (!CreateProcessWithTokenW(hNewToken, LOGON_WITH_PROFILE, selfPath, nullptr,
                CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi))
            {
                kLogEvent logEvent;
                err << logEvent << "CreateProcessWithTokenW failed, error: " << GetLastError() << eol;
                CloseHandle(hNewToken);
                return 1;
            }

            // ========== 第 7 步：成功，记录日志并清理 ==========
            {
                kLogEvent logEvent;
                info << logEvent << "Successfully started self with TrustedInstaller token. New PID: " << pi.dwProcessId << eol;
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(hNewToken);
        }
    });

    // 定时刷新权限状态，保证按钮颜色与实际权限一致。
    m_privilegeStatusTimer = new QTimer(this);
    m_privilegeStatusTimer->setInterval(1500);
    connect(m_privilegeStatusTimer, &QTimer::timeout, this, [this]() {
        refreshPrivilegeStatusButtons();
    });
    m_privilegeStatusTimer->start();

    refreshPrivilegeStatusButtons();
}

void MainWindow::refreshPrivilegeStatusButtons()
{
    // 读取当前权限状态。
    const bool adminEnabled = hasAdminPrivilege();
    const bool debugEnabled = hasDebugPrivilege();
    const bool systemEnabled = hasSystemPrivilege();

    // TI/PPL 状态位：
    // - TI：检查当前令牌是否为 TrustedInstaller 服务 SID；
    // - PPL：当前版本暂不做内核态校验，仍保留为占位状态。
    const bool trustedInstallerEnabled = hasTrustedInstallerPrivilege();
    const bool pplEnabled = false;

    // 按状态更新按钮样式与提示文本。
    applyPrivilegeButtonStyle(m_adminStatusButton, adminEnabled);
    applyPrivilegeButtonStyle(m_debugStatusButton, debugEnabled);
    applyPrivilegeButtonStyle(m_systemStatusButton, systemEnabled);
    applyPrivilegeButtonStyle(m_tiStatusButton, trustedInstallerEnabled);
    applyPrivilegeButtonStyle(m_pplStatusButton, pplEnabled);

    if (m_adminStatusButton != nullptr)
    {
        m_adminStatusButton->setToolTip(adminEnabled ? "管理员权限已启用" : "点击提权到管理员（重启当前程序）");
    }
    if (m_debugStatusButton != nullptr)
    {
        m_debugStatusButton->setToolTip(debugEnabled ? "SeDebugPrivilege 已启用" : "点击启用 SeDebugPrivilege");
    }
    if (m_systemStatusButton != nullptr)
    {
        m_systemStatusButton->setToolTip(systemEnabled ? "当前运行身份：LocalSystem" : "当前运行身份：非 LocalSystem");
    }
    if (m_tiStatusButton != nullptr)
    {
        m_tiStatusButton->setToolTip(trustedInstallerEnabled ? "当前运行身份：TrustedInstaller" : "当前运行身份：非 TrustedInstaller");
    }

    // 仅在状态变化时写日志，避免定时器造成日志刷屏。
    static bool hasPreviousState = false;
    static bool previousAdmin = false;
    static bool previousDebug = false;
    static bool previousSystem = false;
    static bool previousTi = false;
    if (!hasPreviousState ||
        previousAdmin != adminEnabled ||
        previousDebug != debugEnabled ||
        previousSystem != systemEnabled ||
        previousTi != trustedInstallerEnabled)
    {
        hasPreviousState = true;
        previousAdmin = adminEnabled;
        previousDebug = debugEnabled;
        previousSystem = systemEnabled;
        previousTi = trustedInstallerEnabled;

        kLogEvent logEvent;
        info << logEvent
            << "[MainWindow] 权限状态刷新, admin=" << (adminEnabled ? "true" : "false")
            << ", debug=" << (debugEnabled ? "true" : "false")
            << ", system=" << (systemEnabled ? "true" : "false")
            << ", ti=" << (trustedInstallerEnabled ? "true" : "false")
            << eol;
    }
}

void MainWindow::applyPrivilegeButtonStyle(QPushButton* button, const bool activeState)
{
    if (button == nullptr)
    {
        return;
    }
    button->setStyleSheet(buildPrivilegeButtonStyle(activeState));
}

void MainWindow::requestAdminElevationRestart()
{
    // 获取当前可执行文件路径，作为 runas 重启目标。
    wchar_t exePathBuffer[MAX_PATH] = {};
    const DWORD pathLength = ::GetModuleFileNameW(nullptr, exePathBuffer, static_cast<DWORD>(std::size(exePathBuffer)));
    if (pathLength == 0 || pathLength >= std::size(exePathBuffer))
    {
        const DWORD lastError = ::GetLastError();
        QMessageBox::warning(this, "Admin", QString("读取当前程序路径失败，错误码: %1").arg(lastError));
        return;
    }

    // 使用 ShellExecute("runas") 触发 UAC 提权。
    HINSTANCE shellResult = ::ShellExecuteW(
        nullptr,
        L"runas",
        exePathBuffer,
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(shellResult) <= 32)
    {
        QMessageBox::warning(this, "Admin", "提权启动失败，可能被用户取消或系统策略阻止。");
        return;
    }

    // 新进程启动成功后，当前实例退出。
    kLogEvent logEvent;
    info << logEvent << "[MainWindow] 已触发管理员重启，当前实例即将退出。" << eol;
    QApplication::quit();
}

bool MainWindow::hasAdminPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    TOKEN_ELEVATION tokenElevation{};
    DWORD returnLength = 0;
    const BOOL queryOk = ::GetTokenInformation(
        tokenHandle,
        TokenElevation,
        &tokenElevation,
        sizeof(tokenElevation),
        &returnLength);
    ::CloseHandle(tokenHandle);
    return queryOk != FALSE && tokenElevation.TokenIsElevated != 0;
}

bool MainWindow::hasDebugPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    DWORD requiredLength = 0;
    ::GetTokenInformation(tokenHandle, TokenPrivileges, nullptr, 0, &requiredLength);
    if (requiredLength == 0)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    std::vector<BYTE> privilegeBuffer(requiredLength, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenPrivileges,
        privilegeBuffer.data(),
        requiredLength,
        &requiredLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    LUID debugLuid{};
    if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debugLuid) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    const TOKEN_PRIVILEGES* tokenPrivileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(privilegeBuffer.data());
    for (DWORD privilegeIndex = 0; privilegeIndex < tokenPrivileges->PrivilegeCount; ++privilegeIndex)
    {
        const LUID_AND_ATTRIBUTES& privilegeItem = tokenPrivileges->Privileges[privilegeIndex];
        if (privilegeItem.Luid.LowPart == debugLuid.LowPart &&
            privilegeItem.Luid.HighPart == debugLuid.HighPart)
        {
            const bool enabled = (privilegeItem.Attributes & SE_PRIVILEGE_ENABLED) != 0;
            ::CloseHandle(tokenHandle);
            return enabled;
        }
    }

    ::CloseHandle(tokenHandle);
    return false;
}

bool MainWindow::hasSystemPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    DWORD requiredLength = 0;
    ::GetTokenInformation(tokenHandle, TokenUser, nullptr, 0, &requiredLength);
    if (requiredLength == 0)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    std::vector<BYTE> userBuffer(requiredLength, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenUser,
        userBuffer.data(),
        requiredLength,
        &requiredLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    BYTE systemSidBuffer[SECURITY_MAX_SID_SIZE] = {};
    DWORD systemSidLength = static_cast<DWORD>(std::size(systemSidBuffer));
    if (::CreateWellKnownSid(
        WinLocalSystemSid,
        nullptr,
        systemSidBuffer,
        &systemSidLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
    const bool isSystem = (::EqualSid(tokenUser->User.Sid, systemSidBuffer) != FALSE);
    ::CloseHandle(tokenHandle);
    return isSystem;
}

bool MainWindow::hasTrustedInstallerPrivilege() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return false;
    }

    DWORD requiredLength = 0;
    ::GetTokenInformation(tokenHandle, TokenUser, nullptr, 0, &requiredLength);
    if (requiredLength == 0)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    std::vector<BYTE> userBuffer(requiredLength, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenUser,
        userBuffer.data(),
        requiredLength,
        &requiredLength) == FALSE)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    PSID trustedInstallerSid = nullptr;
    const BOOL sidOk = ::ConvertStringSidToSidW(
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
        &trustedInstallerSid);
    if (sidOk == FALSE || trustedInstallerSid == nullptr)
    {
        ::CloseHandle(tokenHandle);
        return false;
    }

    const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
    const bool isTrustedInstaller = (::EqualSid(tokenUser->User.Sid, trustedInstallerSid) != FALSE);

    ::LocalFree(trustedInstallerSid);
    ::CloseHandle(tokenHandle);
    return isTrustedInstaller;
}

bool MainWindow::enableSeDebugPrivilege(std::string& errorTextOut) const
{
    errorTextOut.clear();

    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(
        ::GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        &tokenHandle) == FALSE)
    {
        errorTextOut = "OpenProcessToken failed, error=" + std::to_string(::GetLastError());
        return false;
    }

    LUID debugLuid{};
    if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debugLuid) == FALSE)
    {
        errorTextOut = "LookupPrivilegeValue(SE_DEBUG_NAME) failed, error=" + std::to_string(::GetLastError());
        ::CloseHandle(tokenHandle);
        return false;
    }

    TOKEN_PRIVILEGES tokenPrivileges{};
    tokenPrivileges.PrivilegeCount = 1;
    tokenPrivileges.Privileges[0].Luid = debugLuid;
    tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (::AdjustTokenPrivileges(
        tokenHandle,
        FALSE,
        &tokenPrivileges,
        sizeof(tokenPrivileges),
        nullptr,
        nullptr) == FALSE)
    {
        errorTextOut = "AdjustTokenPrivileges failed, error=" + std::to_string(::GetLastError());
        ::CloseHandle(tokenHandle);
        return false;
    }

    const DWORD adjustError = ::GetLastError();
    ::CloseHandle(tokenHandle);
    if (adjustError != ERROR_SUCCESS)
    {
        errorTextOut = "AdjustTokenPrivileges returned error=" + std::to_string(adjustError);
        return false;
    }
    return true;
}

void MainWindow::initDockWidgets()
{
    // 第一批轻量页面：先创建基础页签，尽快推进启动进度。
    reportStartupProgress(50, QStringLiteral("正在创建基础页面..."));
    m_welcomeWidget = new WelcomeDock(this);
    m_processWidget = new ProcessDock(this);
    m_networkWidget = new NetworkDock(this);
    m_memoryWidget = new MemoryDock(this);

    // 第二批核心页面：文件/驱动/内核通常更重，单独给出阶段提示。
    reportStartupProgress(56, QStringLiteral("正在创建核心分析页面..."));
    m_fileWidget = new FileDock(this);
    m_driverWidget = new DriverDock(this);
    m_kernelWidget = new KernelDock(this);
    m_monitorWidget = new MonitorDock(this);

    // 第三批功能页面：监视面板、硬件与设置页。
    reportStartupProgress(62, QStringLiteral("正在创建监控与设置页面..."));
    // 监视面板使用独立组件承载四宫格性能图，避免与 WMI/ETW 页面耦合。
    m_monitorPanelWidget = new MonitorPanelWidget(this);
    m_hardwareWidget = new HardwareDock(this);
    m_privilegeWidget = new PrivilegeDock(this);
    m_settingsWidget = new SettingsDock(this);

    // 第四批辅助页面：窗口、注册表、日志、当前操作、即时窗口。
    reportStartupProgress(66, QStringLiteral("正在创建辅助页面..."));
    m_windowWidget = new WindowDock(this);
    m_registryWidget = new RegistryDock(this);
    m_handleWidget = new HandleDock(this);
    m_startupWidget = new StartupDock(this);
    m_logWidget = new LogDockWidget(this);
    m_progressWidget = new ProgressDockWidget(this);
    m_immediateEditorWidget = new CodeEditorWidget(this);

    // 创建 Dock 容器前再推进一次启动进度，避免长时间停留在单一文案。
    reportStartupProgress(68, QStringLiteral("正在封装 Dock 容器..."));

    // 使用辅助函数创建Dock Widgets。
    auto createDockWidget = [this](QWidget* widget, const QString& title) -> ads::CDockWidget* {
        ads::CDockWidget* dock = new ads::CDockWidget(title);
        dock->setWidget(widget);
        // DockWidgetClosable 禁用：统一去掉每个 Dock 标签旁边的关闭叉。
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
        dock->setFeature(ads::CDockWidget::DockWidgetMovable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, true);

        // Dock 背景属性初始化：
        // - 默认关闭 Dock 与内容根控件的自动背景填充；
        // - 避免背景图模式下被黑/白纯色底覆盖。
        dock->setAutoFillBackground(false);
        dock->setAttribute(Qt::WA_StyledBackground, false);
        if (widget != nullptr)
        {
            widget->setAutoFillBackground(false);
            widget->setAttribute(Qt::WA_StyledBackground, false);
        }
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
    m_dockHardware = createDockWidget(m_hardwareWidget, "硬件");
    m_dockPrivilege = createDockWidget(m_privilegeWidget, "权限");
    m_dockSettings = createDockWidget(m_settingsWidget, "设置");
    m_dockWindow = createDockWidget(m_windowWidget, "窗口");
    m_dockRegistry = createDockWidget(m_registryWidget, "注册表");
    m_dockHandle = createDockWidget(m_handleWidget, "句柄");
    m_dockStartup = createDockWidget(m_startupWidget, "启动项");

    // 创建右侧和底部的基本Widgets
    m_dockCurrentOp = createDockWidget(m_progressWidget, "当前操作");
    m_dockLog = createDockWidget(m_logWidget, "日志输出");
    m_dockImmediate = createDockWidget(m_immediateEditorWidget, "即时窗口");
    // 左下角“监视面板”接入独立性能图组件（CPU每核/内存/磁盘/网络）。
    m_dockMonitor = createDockWidget(m_monitorPanelWidget, "监视面板");

    // 当前操作 Dock 专项透明策略：
    // - Dock 自身背景透明；
    // - 内容容器背景透明；
    // - 避免 ADS 默认底色盖住壁纸或主题底图。
    if (m_dockCurrentOp != nullptr)
    {
        m_dockCurrentOp->setObjectName(QStringLiteral("ksCurrentOperationDock"));
        m_dockCurrentOp->setStyleSheet(
            QStringLiteral(
            "#ksCurrentOperationDock,"
            "#ksCurrentOperationDock > QWidget,"
            "#ksCurrentOperationDock QScrollArea,"
            "#ksCurrentOperationDock QAbstractScrollArea,"
            "#ksCurrentOperationDock QAbstractScrollArea::viewport{"
            "  background:transparent;"
            "  background-color:transparent;"
            "  border:none;"
            "}"));
    }

    // 将Dock Widget的切换动作添加到菜单
    reportStartupProgress(72, QStringLiteral("正在注册视图菜单..."));
    QMenu* viewMenu = menuBar()->addMenu("视图(&V)");
    QList<ads::CDockWidget*> allDocks = {
        m_dockWelcome, m_dockProcess, m_dockNetwork, m_dockMemory,
        m_dockFile, m_dockDriver, m_dockKernel, m_dockMonitorTab, m_dockHardware,
        m_dockPrivilege, m_dockSettings, m_dockWindow, m_dockRegistry, m_dockHandle, m_dockStartup,
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
    m_pDockManager->addDockWidgetTabToArea(m_dockHardware, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockPrivilege, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockSettings, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockWindow, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockRegistry, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockHandle, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockStartup, leftDockArea);

    // 方法2: 或者使用addDockWidget并指定CenterDockWidgetArea
    // m_pDockManager->addDockWidget(ads::CenterDockWidgetArea, m_dockProcess, leftDockArea);

    // 4. 右侧区域：同理
    auto rightDockArea = m_pDockManager->addDockWidget(ads::RightDockWidgetArea, m_dockCurrentOp);

    // 右侧仅保留“当前操作 + 即时窗口”。
    m_pDockManager->addDockWidgetTabToArea(m_dockImmediate, rightDockArea);

    // 5. 底部区域：按“监视面板 + 日志输出”左右分栏并占满底部。
    auto bottomDockArea = m_pDockManager->addDockWidget(ads::BottomDockWidgetArea, m_dockMonitor);
    m_pDockManager->addDockWidget(ads::RightDockWidgetArea, m_dockLog, bottomDockArea);

    // 6. 设置默认显示的标签页
    m_dockWelcome->raise();
    m_dockCurrentOp->raise();
    m_dockMonitor->raise();
}

void MainWindow::focusHandleDockByPid(const quint32 pid)
{
    // 跳转入口日志：记录来源请求 PID，便于串联调用链审计。
    kLogEvent focusHandleEvent;
    info << focusHandleEvent
        << "[MainWindow] focusHandleDockByPid: pid="
        << pid
        << eol;

    if (m_handleWidget != nullptr)
    {
        m_handleWidget->focusProcessId(static_cast<std::uint32_t>(pid), true);
    }
    if (m_dockHandle != nullptr)
    {
        m_dockHandle->raise();
        m_dockHandle->setVisible(true);
    }
}

void MainWindow::initAppearanceSettings()
{
    // appearanceInitEvent 作用：统一追踪外观系统初始化流程日志。
    kLogEvent appearanceInitEvent;
    info << appearanceInitEvent << "[MainWindow] 开始初始化外观设置系统。" << eol;

    // raiseStartupDockByKey 作用：
    // - 按配置 key 激活启动默认主页签；
    // - key 非法或目标 Dock 不可用时自动回退到欢迎页。
    const auto raiseStartupDockByKey = [this](const QString& startupDockKey, const QString& triggerReason) {
        const QString normalizedKey = startupDockKey.trimmed().toLower();
        ads::CDockWidget* targetDock = nullptr;
        QString targetName = QStringLiteral("欢迎");

        if (normalizedKey == QStringLiteral("process"))
        {
            targetDock = m_dockProcess;
            targetName = QStringLiteral("进程");
        }
        else if (normalizedKey == QStringLiteral("network"))
        {
            targetDock = m_dockNetwork;
            targetName = QStringLiteral("网络");
        }
        else if (normalizedKey == QStringLiteral("memory"))
        {
            targetDock = m_dockMemory;
            targetName = QStringLiteral("内存");
        }
        else if (normalizedKey == QStringLiteral("file"))
        {
            targetDock = m_dockFile;
            targetName = QStringLiteral("文件");
        }
        else if (normalizedKey == QStringLiteral("driver"))
        {
            targetDock = m_dockDriver;
            targetName = QStringLiteral("驱动");
        }
        else if (normalizedKey == QStringLiteral("kernel"))
        {
            targetDock = m_dockKernel;
            targetName = QStringLiteral("内核");
        }
        else if (normalizedKey == QStringLiteral("monitor"))
        {
            targetDock = m_dockMonitorTab;
            targetName = QStringLiteral("监控");
        }
        else if (normalizedKey == QStringLiteral("hardware"))
        {
            targetDock = m_dockHardware;
            targetName = QStringLiteral("硬件");
        }
        else if (normalizedKey == QStringLiteral("privilege"))
        {
            targetDock = m_dockPrivilege;
            targetName = QStringLiteral("权限");
        }
        else if (normalizedKey == QStringLiteral("settings"))
        {
            targetDock = m_dockSettings;
            targetName = QStringLiteral("设置");
        }
        else if (normalizedKey == QStringLiteral("window"))
        {
            targetDock = m_dockWindow;
            targetName = QStringLiteral("窗口");
        }
        else if (normalizedKey == QStringLiteral("registry"))
        {
            targetDock = m_dockRegistry;
            targetName = QStringLiteral("注册表");
        }
        else if (normalizedKey == QStringLiteral("handle"))
        {
            targetDock = m_dockHandle;
            targetName = QStringLiteral("句柄");
        }
        else if (normalizedKey == QStringLiteral("startup"))
        {
            targetDock = m_dockStartup;
            targetName = QStringLiteral("启动项");
        }
        else
        {
            targetDock = m_dockWelcome;
            targetName = QStringLiteral("欢迎");
        }

        if (targetDock == nullptr)
        {
            targetDock = m_dockWelcome;
            targetName = QStringLiteral("欢迎");
        }

        if (targetDock != nullptr)
        {
            targetDock->raise();
            kLogEvent startupDockEvent;
            info << startupDockEvent
                << "[MainWindow] 已激活启动默认页签, trigger="
                << triggerReason.toStdString()
                << ", key="
                << normalizedKey.toStdString()
                << ", tab="
                << targetName.toStdString()
                << eol;
        }
    };

    // 启动画面细分：
    // - 先绑定设置页/系统主题变化；
    // - 再真正应用首轮主题样式。
    reportStartupProgress(86, QStringLiteral("正在绑定外观设置源..."));

    if (m_settingsWidget != nullptr)
    {
        connect(
            m_settingsWidget,
            &SettingsDock::appearanceSettingsChanged,
            this,
            [this](const ks::settings::AppearanceSettings& settings) {
                applyAppearanceSettings(settings, QStringLiteral("设置页变更"));
            });

        m_currentAppearanceSettings = m_settingsWidget->currentAppearanceSettings();
    }
    else
    {
        m_currentAppearanceSettings = ks::settings::loadAppearanceSettings();
    }

    QStyleHints* styleHints = QGuiApplication::styleHints();
    if (styleHints != nullptr)
    {
        connect(styleHints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme /*newScheme*/) {
            if (m_currentAppearanceSettings.themeMode == ks::settings::ThemeMode::FollowSystem)
            {
                applyAppearanceSettings(m_currentAppearanceSettings, QStringLiteral("系统颜色方案变化"));
            }
            });
    }

    reportStartupProgress(90, QStringLiteral("正在应用主界面主题..."));
    applyAppearanceSettings(m_currentAppearanceSettings, QStringLiteral("初始化加载"));
    raiseStartupDockByKey(m_currentAppearanceSettings.startupDefaultTabKey, QStringLiteral("初始化加载"));
    info << appearanceInitEvent << "[MainWindow] 外观设置系统初始化完成。" << eol;
}

void MainWindow::applyAppearanceSettings(
    const ks::settings::AppearanceSettings& settings,
    const QString& triggerReason)
{
    // appearanceApplyEvent 作用：本次“外观应用”过程统一日志事件对象。
    kLogEvent appearanceApplyEvent;

    m_currentAppearanceSettings = settings;

    const bool darkModeEnabled = isDarkModeEffective(settings);
    const QColor windowBackgroundColor = darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255);
    const QColor textColor = darkModeEnabled ? QColor(255, 255, 255) : QColor(0, 0, 0);
    const QColor baseColor = darkModeEnabled ? QColor(22, 22, 22) : QColor(255, 255, 255);
    const QColor alternateBaseColor = darkModeEnabled ? QColor(30, 30, 30) : QColor(247, 249, 252);
    const QColor midColor = darkModeEnabled ? QColor(86, 86, 86) : QColor(180, 180, 180);

    // enableDockTransparencyForBackgroundImage 作用：
    // - 当背景图可用时，把 Dock 系统背景整体切换为透明；
    // - 满足“加载背景图后 Dock 黑白底必须全部透明”的需求。
    const bool enableDockTransparencyForBackgroundImage =
        isBackgroundImageReady(settings.backgroundImagePath);

    // 把深浅色状态写入全局属性，供各 Dock 在绘制/着色时读取。
    KswordTheme::SetDarkModeEnabled(darkModeEnabled);

    // Windows 11 背景控制要求：
    // 即使是纯黑/纯白，也必须显式设置 Window 颜色，避免系统接管背景。
    QPalette mainPalette = palette();
    mainPalette.setColor(QPalette::Window, windowBackgroundColor);
    mainPalette.setColor(QPalette::WindowText, textColor);
    mainPalette.setColor(QPalette::Base, baseColor);
    mainPalette.setColor(QPalette::AlternateBase, alternateBaseColor);
    mainPalette.setColor(QPalette::Mid, midColor);
    mainPalette.setColor(QPalette::Midlight, darkModeEnabled ? QColor(116, 116, 116) : QColor(210, 210, 210));
    mainPalette.setColor(QPalette::Dark, darkModeEnabled ? QColor(48, 48, 48) : QColor(120, 120, 120));
    mainPalette.setColor(QPalette::Text, textColor);
    mainPalette.setColor(QPalette::Button, darkModeEnabled ? QColor(30, 30, 30) : QColor(255, 255, 255));
    mainPalette.setColor(QPalette::ButtonText, textColor);
    mainPalette.setColor(QPalette::ToolTipBase, darkModeEnabled ? QColor(24, 24, 24) : QColor(255, 255, 255));
    mainPalette.setColor(QPalette::ToolTipText, textColor);
    mainPalette.setColor(QPalette::Highlight, QColor(67, 160, 255));
    mainPalette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    QApplication::setPalette(mainPalette);
    setPalette(mainPalette);
    setAutoFillBackground(true);

    // 全局提示框样式：
    // - 通过 QToolTip 静态调色板强制应用深浅色提示框；
    // - 修复深色模式下 Tooltip 仍是白底的问题。
    QPalette toolTipPalette = mainPalette;
    toolTipPalette.setColor(QPalette::ToolTipBase, darkModeEnabled ? QColor(24, 24, 24) : QColor(255, 255, 255));
    toolTipPalette.setColor(QPalette::ToolTipText, textColor);
    QToolTip::setPalette(toolTipPalette);
    QToolTip::setFont(QApplication::font());

    // 同步写入 QApplication 样式表中的 QToolTip 规则：
    // - 覆盖所有顶层窗口（含浮动 Dock 与后续新建窗口）；
    // - 修复“部分按钮 Tooltip 仍白底”的残留问题。
    applyGlobalTooltipStyleBlock(buildGlobalTooltipStyleBlock(darkModeEnabled));

    // 全局 QMessageBox 主题刷新：
    // - 深浅色切换后，已打开的消息框也同步改为当前主题；
    // - 修复深色模式下消息框仍残留白底的问题。
    ks::ui::RefreshGlobalMessageBoxTheme();

    if (menuBar() != nullptr)
    {
        QPalette menuPalette = menuBar()->palette();
        menuPalette.setColor(QPalette::Window, windowBackgroundColor);
        menuPalette.setColor(QPalette::WindowText, textColor);
        menuPalette.setColor(QPalette::Text, textColor);
        menuBar()->setPalette(menuPalette);
        menuBar()->setAutoFillBackground(true);
    }

    if (m_pDockManager != nullptr)
    {
        QPalette dockPalette = m_pDockManager->palette();
        dockPalette.setColor(QPalette::Window, windowBackgroundColor);
        dockPalette.setColor(QPalette::WindowText, textColor);
        dockPalette.setColor(QPalette::Text, textColor);
        m_pDockManager->setPalette(dockPalette);
        m_pDockManager->setAutoFillBackground(true);
    }

    // appearanceStyleSheet 作用：
    // - 聚合基础样式与深浅色覆盖样式；
    // - 同时应用到 MainWindow 与 ADS DockManager，覆盖其默认白色样式。
    const QString appearanceStyleSheet =
        QSS_MainWindow_TabWidget
        + QSS_MainWindow_dockStyle
        + buildAppearanceOverlayStyleSheet(
            darkModeEnabled,
            enableDockTransparencyForBackgroundImage);

    setStyleSheet(appearanceStyleSheet);
    if (m_pDockManager != nullptr)
    {
        // DockManager 清空局部样式表，改为继承 MainWindow 的统一样式；
        // 避免局部样式覆盖背景画刷导致背景图不可见。
        m_pDockManager->setStyleSheet(QString());
        m_pDockManager->setAttribute(Qt::WA_StyledBackground, false);
    }

    // 主题切换后主动刷新“按主题着色的表格行”，避免墨绿色残留到浅色模式。
    if (m_processWidget != nullptr)
    {
        m_processWidget->refreshThemeVisuals();
    }

    rebuildWindowBackgroundBrush();
    refreshPrivilegeStatusButtons();

    const QString effectiveModeText = darkModeEnabled ? QStringLiteral("dark") : QStringLiteral("light");
    info << appearanceApplyEvent
        << "[MainWindow] 已应用外观设置，触发来源="
        << triggerReason.toStdString()
        << "，theme_mode="
        << ks::settings::themeModeToJsonText(settings.themeMode).toStdString()
        << "，effective_mode="
        << effectiveModeText.toStdString()
        << "，dock_transparent="
        << (enableDockTransparencyForBackgroundImage ? "true" : "false")
        << "，background_path="
        << settings.backgroundImagePath.toStdString()
        << "，opacity="
        << settings.backgroundOpacityPercent
        << "%"
        << eol;
}

bool MainWindow::isDarkModeEffective(const ks::settings::AppearanceSettings& settings) const
{
    if (settings.themeMode == ks::settings::ThemeMode::Dark)
    {
        return true;
    }
    if (settings.themeMode == ks::settings::ThemeMode::Light)
    {
        return false;
    }

    QStyleHints* styleHints = QGuiApplication::styleHints();
    if (styleHints == nullptr)
    {
        return false;
    }
    return styleHints->colorScheme() == Qt::ColorScheme::Dark;
}

void MainWindow::rebuildWindowBackgroundBrush()
{
    const bool darkModeEnabled = isDarkModeEffective(m_currentAppearanceSettings);
    const QColor baseColor = darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255);

    // resolvedImagePath 作用：把配置路径解析成可加载的绝对路径。
    const QString resolvedImagePath = ks::settings::resolveBackgroundImagePathForLoad(
        m_currentAppearanceSettings.backgroundImagePath);

    // 背景图路径诊断日志：
    // - 记录解析后的背景图路径与可读性；
    // - 便于快速定位“背景图不显示”是路径问题还是样式覆盖问题。
    {
        kLogEvent backgroundResolveEvent;
        const QFileInfo backgroundFileInfo(resolvedImagePath);

        // imageReader 作用：读取图片头信息（宽高与格式）用于日志诊断。
        QImageReader imageReader(resolvedImagePath);
        const QSize imagePixelSize = imageReader.size();
        const QByteArray imageFormatBytes = imageReader.format();
        info << backgroundResolveEvent
            << "[MainWindow] rebuildWindowBackgroundBrush: configuredPath="
            << m_currentAppearanceSettings.backgroundImagePath.toStdString()
            << ", resolvedPath="
            << resolvedImagePath.toStdString()
            << ", fileName="
            << backgroundFileInfo.fileName().toStdString()
            << ", exists="
            << (backgroundFileInfo.exists() ? "true" : "false")
            << ", readable="
            << (backgroundFileInfo.isReadable() ? "true" : "false")
            << ", sizeBytes="
            << backgroundFileInfo.size()
            << ", imageSize="
            << imagePixelSize.width()
            << "x"
            << imagePixelSize.height()
            << ", format="
            << imageFormatBytes.toStdString()
            << eol;
    }

    const QBrush backgroundBrush = buildBackgroundBrush(
        size(),
        baseColor,
        resolvedImagePath,
        m_currentAppearanceSettings.backgroundOpacityPercent);

    QPalette mainPalette = palette();
    mainPalette.setColor(QPalette::Window, baseColor);
    mainPalette.setBrush(QPalette::Window, backgroundBrush);
    setPalette(mainPalette);
    setAutoFillBackground(true);

    if (m_pDockManager != nullptr)
    {
        QPalette dockPalette = m_pDockManager->palette();
        dockPalette.setColor(QPalette::Window, baseColor);
        dockPalette.setBrush(QPalette::Window, backgroundBrush);
        m_pDockManager->setPalette(dockPalette);
        m_pDockManager->setAutoFillBackground(true);

        // 同步重建所有当前浮动窗口背景：
        // - 主窗口 resize 或背景图切换后，浮动容器也要刷新自己的背景画刷；
        // - 否则会继续保留旧尺寸/旧主题下的纯黑背景。
        const QList<ads::CFloatingDockContainer*> floatingWidgetList = m_pDockManager->floatingWidgets();
        for (ads::CFloatingDockContainer* floatingWidget : floatingWidgetList)
        {
            applyFloatingDockContainerAppearance(floatingWidget);
        }
    }
}

void MainWindow::applyFloatingDockContainerAppearance(ads::CFloatingDockContainer* floatingWidget) const
{
    if (floatingWidget == nullptr)
    {
        return;
    }

    const bool darkModeEnabled = isDarkModeEffective(m_currentAppearanceSettings);
    const QColor baseColor = darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255);
    const QString resolvedImagePath = ks::settings::resolveBackgroundImagePathForLoad(
        m_currentAppearanceSettings.backgroundImagePath);
    const bool enableDockTransparencyForBackgroundImage =
        isBackgroundImageReady(m_currentAppearanceSettings.backgroundImagePath);

    // floatingSize 用途：浮动容器当前尺寸；无效时至少给 1x1，避免画刷构造失败。
    const QSize floatingSize = floatingWidget->size().isValid() ? floatingWidget->size() : QSize(1, 1);
    const QBrush backgroundBrush = buildBackgroundBrush(
        floatingSize,
        baseColor,
        resolvedImagePath,
        m_currentAppearanceSettings.backgroundOpacityPercent);

    QPalette floatingPalette = floatingWidget->palette();
    floatingPalette.setColor(QPalette::Window, baseColor);
    floatingPalette.setBrush(QPalette::Window, backgroundBrush);
    floatingPalette.setColor(QPalette::WindowText, darkModeEnabled ? QColor(255, 255, 255) : QColor(0, 0, 0));
    floatingWidget->setPalette(floatingPalette);
    floatingWidget->setAutoFillBackground(true);
    floatingWidget->setAttribute(Qt::WA_StyledBackground, false);

    // 浮动窗口是独立顶层窗口，不继承 MainWindow 的局部样式表；
    // 这里显式复用同一套外观样式，确保 Tab/TitleBar/内容区规则一致。
    const QString appearanceStyleSheet =
        QSS_MainWindow_TabWidget
        + QSS_MainWindow_dockStyle
        + buildAppearanceOverlayStyleSheet(
            darkModeEnabled,
            enableDockTransparencyForBackgroundImage);
    floatingWidget->setStyleSheet(appearanceStyleSheet);

    ads::CDockContainerWidget* dockContainer = floatingWidget->dockContainer();
    if (dockContainer != nullptr)
    {
        QPalette dockContainerPalette = dockContainer->palette();
        dockContainerPalette.setColor(QPalette::Window, baseColor);
        dockContainerPalette.setBrush(QPalette::Window, backgroundBrush);
        dockContainer->setPalette(dockContainerPalette);
        dockContainer->setAutoFillBackground(true);
        dockContainer->setAttribute(Qt::WA_StyledBackground, false);
    }
}

QString MainWindow::buildAppearanceOverlayStyleSheet(
    const bool darkModeEnabled,
    const bool enableDockTransparencyForBackgroundImage) const
{
    // tooltipStyle 作用：
    // - 强制全局提示框采用主题化背景和文字；
    // - 修复深色模式下 Tooltip 仍为白底的问题。
    const QString tooltipStyle = QStringLiteral(
        "QToolTip{"
        "  background-color:%1 !important;"
        "  color:%2 !important;"
        "  border:1px solid %3 !important;"
        "  padding:4px 6px;"
        "  border-radius:3px;"
        "}")
        .arg(darkModeEnabled ? QStringLiteral("#181818") : QStringLiteral("#FFFFFF"))
        .arg(darkModeEnabled ? QStringLiteral("#FFFFFF") : QStringLiteral("#000000"))
        .arg(KswordTheme::PrimaryBlueHex);

    // dockBackgroundPolicyStyle 作用：
    // - 背景图可用时：Dock 相关容器全部透明，让底图完整透出；
    // - 背景图不可用时：保持 DockManager 使用 palette(window) 作为背景底色。
    const QString dockBackgroundPolicyStyle = enableDockTransparencyForBackgroundImage
        ? QStringLiteral(
            "QDockWidget,"
            "QDockWidget::title,"
            "QDockWidget > QWidget,"
            "ads--CDockManager,"
            "ads--CDockContainerWidget,"
            "ads--CDockAreaWidget,"
            "ads--CDockAreaTitleBar,"
            "ads--CFloatingDockContainer,"
            "ads--CDockAreaTabBar{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "  color:%1 !important;"
            "}")
        : QStringLiteral(
            "ads--CDockManager{"
            "  background-color:palette(window) !important;"
            "  color:%1 !important;"
            "}"
            "ads--CDockContainerWidget,"
            "ads--CDockAreaWidget,"
            "ads--CFloatingDockContainer{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "}"
            "ads--CDockAreaTitleBar{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "  color:%1 !important;"
            "}");

    // rootStyle 作用：
    // - 主窗口继续使用 palette(window)（含背景图画刷）作为底图来源；
    // - 再叠加 Dock 背景策略样式。
    const QString rootStyle = QStringLiteral(
        "QMainWindow{"
        "  background-color:palette(window) !important;"
        "  color:%1;"
        "}")
        .arg(darkModeEnabled ? QStringLiteral("#FFFFFF") : QStringLiteral("#000000"))
        + dockBackgroundPolicyStyle.arg(
            darkModeEnabled ? QStringLiteral("#FFFFFF") : QStringLiteral("#000000"));

    // sharedOverlayStyle 作用：
    // - 统一 hover/pressed 与 Tab 高亮主题色；
    // - 维持滚动条蓝色手柄与 Dock 选中态白字。
    const QString sharedOverlayStyle = QStringLiteral(
        "QPushButton:hover,QToolButton:hover{"
        "  background-color:#2E8BFF !important;"
        "  color:#FFFFFF !important;"
        "  border-color:#2E8BFF !important;"
        "}"
        "QPushButton:pressed,QToolButton:pressed{"
        "  background-color:#1F78D0 !important;"
        "  color:#FFFFFF !important;"
        "  border-color:#1F78D0 !important;"
        "}"
        "QTabBar::tab:selected{"
        "  background-color:#43A0FF !important;"
        "  color:#FFFFFF !important;"
        "  border:none !important;"
        "}"
        "QTabBar::tab:hover:!selected{"
        "  background-color:#2E8BFF !important;"
        "  color:#FFFFFF !important;"
        "}"
        "QTabBar::tab{"
        "  border:none !important;"
        "}"
        "ads--CDockAreaTabBar{"
        "  background:transparent !important;"
        "}"
        "ads--CDockWidgetTab,ads--CAutoHideTab{"
        "  background-color:palette(base) !important;"
        "  color:palette(text) !important;"
        "}"
        "ads--CDockWidgetTab[activeTab=\"true\"],ads--CAutoHideTab[activeTab=\"true\"]{"
        "  background-color:#43A0FF !important;"
        "  color:#FFFFFF !important;"
        "  border-color:#43A0FF !important;"
        "}"
        "ads--CDockWidgetTab[activeTab=\"true\"] QLabel,ads--CAutoHideTab[activeTab=\"true\"] QLabel{"
        "  color:#FFFFFF !important;"
        "  font-weight:600;"
        "}"
        "ads--CDockWidgetTab:hover,ads--CAutoHideTab:hover{"
        "  background-color:#2E8BFF !important;"
        "  color:#FFFFFF !important;"
        "}"
        "ads--CDockAreaTitleBar QToolButton,ads--CDockAreaTitleBar QPushButton{"
        "  border:none !important;"
        "  background:transparent !important;"
        "}"
        "QScrollBar::handle:vertical,QScrollBar::handle:horizontal{"
        "  background-color:#43A0FF !important;"
        "}"
        "QScrollBar::handle:vertical:hover,QScrollBar::handle:horizontal:hover{"
        "  background-color:#2E8BFF !important;"
        "}");

    // dockContentTransparentStyle 作用：
    // - 背景图可用时，把 Dock 内容区域常见容器背景全部改为透明；
    // - 修复“Dock 面板整体仍是黑底/白底，背景图只能从缝隙看到”的问题。
    // 注意：该片段只作用于 ads--CDockWidget 后代，不影响菜单栏等全局区域。
    const QString dockContentTransparentStyle = enableDockTransparencyForBackgroundImage
        ? QStringLiteral(
            "ads--CDockWidget,"
            "ads--CDockWidget > QWidget,"
            "ads--CDockWidget QFrame,"
            "ads--CDockWidget QTabWidget::pane,"
            "ads--CDockWidget QStackedWidget,"
            "ads--CDockWidget QStackedWidget > QWidget,"
            "ads--CDockWidget QSplitter,"
            "ads--CDockWidget QSplitter::handle,"
            "ads--CDockWidget QScrollArea,"
            "ads--CDockWidget QAbstractScrollArea,"
            "ads--CDockWidget QAbstractScrollArea::viewport,"
            "ads--CDockWidget QTableView,"
            "ads--CDockWidget QTableWidget,"
            "ads--CDockWidget QTreeView,"
            "ads--CDockWidget QTreeWidget,"
            "ads--CDockWidget QListView,"
            "ads--CDockWidget QListWidget,"
            "ads--CDockWidget QTextEdit,"
            "ads--CDockWidget QPlainTextEdit,"
            "ads--CDockWidget QGroupBox{"
            "  background:transparent !important;"
            "  background-color:transparent !important;"
            "}"
            "ads--CDockWidget QTableView,"
            "ads--CDockWidget QTableWidget,"
            "ads--CDockWidget QTreeView,"
            "ads--CDockWidget QTreeWidget,"
            "ads--CDockWidget QListView,"
            "ads--CDockWidget QListWidget{"
            "  alternate-background-color:transparent !important;"
            "}")
        : QString();

    if (!darkModeEnabled)
    {
        return rootStyle
            + QStringLiteral(
                "QMenuBar,QMenu{background-color:#FFFFFF;color:#000000;}"
                "QStatusBar{background-color:#FFFFFF;color:#000000;}"
                "QLineEdit,QTextEdit,QPlainTextEdit,QTableWidget,QTreeWidget,QListWidget,QComboBox,QSpinBox,QDoubleSpinBox{"
                "  background-color:#FFFFFF !important;"
                "  color:#000000 !important;"
                "  border:1px solid #C7D4E5;"
                "}"
                "QPushButton,QToolButton{"
                "  background-color:#EDF5FF !important;"
                "  color:#1F4E88 !important;"
                "  border:1px solid #8ABFF5 !important;"
                "}"
                "QTabWidget::pane{"
                "  background:transparent !important;"
                "  border:1px solid #C7D4E5 !important;"
                "}"
                "QTabBar{"
                "  background:transparent !important;"
                "}"
                "QTabBar::tab{"
                "  background-color:#FFFFFF !important;"
                "  color:#2F3A47 !important;"
                "  border:none !important;"
                "}"
                "QTableView,QTableWidget,QTreeView,QTreeWidget,QListView,QListWidget{"
                "  background:#FFFFFF !important;"
                "  alternate-background-color:#F3F7FC !important;"
                "  color:#000000 !important;"
                "  gridline-color:#D5DFEB;"
                "}"
                "QTableView::item:selected,QTableWidget::item:selected,QTreeView::item:selected,QTreeWidget::item:selected{"
                "  background:#2E8BFF !important;"
                "  color:#FFFFFF !important;"
                "}"
                "QHeaderView::section{"
                "  background:#F4F8FD !important;"
                "  color:#1A2A3A !important;"
                "  border:1px solid #CBD6E2;"
                "}"
                "QTableCornerButton::section{"
                "  background:#F4F8FD !important;"
                "  border:none !important;"
                "}"
                "QScrollBar:vertical,QScrollBar:horizontal{"
                "  background:#F3F7FC !important;"
                "  border:none !important;"
                "}")
            + sharedOverlayStyle
            + tooltipStyle
            + dockContentTransparentStyle;
    }

    return rootStyle
        + QStringLiteral(
            "QMenuBar,QMenu{background-color:#000000;color:#FFFFFF;}"
            "QStatusBar{background-color:#000000;color:#FFFFFF;}"
            "QLineEdit,QTextEdit,QPlainTextEdit,QTableWidget,QTreeWidget,QListWidget,QComboBox,QSpinBox,QDoubleSpinBox{"
            "  background-color:#111111 !important;"
            "  color:#FFFFFF !important;"
            "  border:1px solid #3A3A3A;"
            "}"
            "QPushButton,QToolButton{"
            "  background-color:#1A1A1A !important;"
            "  color:#FFFFFF !important;"
            "  border:1px solid #5A5A5A !important;"
            "}"
            "QTabWidget::pane{"
            "  background:transparent !important;"
            "  border:1px solid #3A3A3A !important;"
            "}"
            "QTabBar{"
            "  background:transparent !important;"
            "}"
            "QTabBar::tab{"
            "  background-color:#000000 !important;"
            "  color:#DDDDDD !important;"
            "  border:none !important;"
            "}"
            "QTableView,QTableWidget,QTreeView,QTreeWidget,QListView,QListWidget{"
            "  background:#121212 !important;"
            "  alternate-background-color:#1D1D1D !important;"
            "  color:#F0F0F0 !important;"
            "  gridline-color:#2E2E2E;"
            "}"
            "QTableView::item:selected,QTableWidget::item:selected,QTreeView::item:selected,QTreeWidget::item:selected{"
            "  background:#2E8BFF !important;"
            "  color:#FFFFFF !important;"
            "}"
            "QHeaderView::section{"
            "  background:#1A1A1A !important;"
            "  color:#EAEAEA !important;"
            "  border:1px solid #333333;"
            "}"
            "QTableCornerButton::section{"
            "  background:#1A1A1A !important;"
            "  border:none !important;"
            "}"
            "QScrollBar:vertical,QScrollBar:horizontal{"
            "  background:#141414 !important;"
            "  border:none !important;"
            "}")
        + sharedOverlayStyle
        + tooltipStyle
        + dockContentTransparentStyle;
}
