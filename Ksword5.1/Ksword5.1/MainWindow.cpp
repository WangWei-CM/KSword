#include "MainWindow.h"
#include <QMenu>
#include <QAction>
#include <QTabWidget>
#include <QApplication>
#include <QCoreApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QMessageBox>
#pragma warning(disable: 4996)
#include "UI/UI.css/UI_css.h"
#include "Framework.h"
#include "Framework/LogDockWidget.h"
#include "Framework/ProgressDockWidget.h"
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
    // buildPrivilegeButtonStyle 作用：
    // - 按“当前是否具备权限”生成按钮样式；
    // - true  -> 蓝底白字；
    // - false -> 白底蓝字。
    QString buildPrivilegeButtonStyle(const bool activeState)
    {
        const QString backgroundColor = activeState ? KswordTheme::PrimaryBlueHex : QStringLiteral("#FFFFFF");
        const QString textColor = activeState ? QStringLiteral("#FFFFFF") : KswordTheme::PrimaryBlueHex;
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
            "}"
            "QPushButton:pressed {"
            "  background:%5;"
            "  color:#FFFFFF;"
            "}")
            .arg(backgroundColor)
            .arg(textColor)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }
}

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

    // 显式要求“最后一个窗口关闭后退出应用”。
    // 说明：用户要求主窗口关闭时进程必须结束，这里先设置 Qt 全局退出策略。
    QApplication::setQuitOnLastWindowClosed(true);

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

    // 菜单栏右侧权限状态按钮（Admin/Debug/System）。
    initPrivilegeStatusButtons();
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
    m_windowWidget = new WindowDock(this);
    m_registryWidget = new RegistryDock(this);
    m_logWidget = new LogDockWidget(this);
    m_progressWidget = new ProgressDockWidget(this);

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
    m_dockWindow = createDockWidget(m_windowWidget, "窗口");
    m_dockRegistry = createDockWidget(m_registryWidget, "注册表");

    // 创建右侧和底部的基本Widgets
    m_dockCurrentOp = createDockWidget(m_progressWidget, "当前操作");
    m_dockLog = createDockWidget(m_logWidget, "日志输出");
    m_dockImmediate = createDockWidget(new QWidget(), "即时窗口");
    m_dockMonitor = createDockWidget(new QWidget(), "监视面板");

    // 将Dock Widget的切换动作添加到菜单
    QMenu* viewMenu = menuBar()->addMenu("视图(&V)");
    QList<ads::CDockWidget*> allDocks = {
        m_dockWelcome, m_dockProcess, m_dockNetwork, m_dockMemory,
        m_dockFile, m_dockDriver, m_dockKernel, m_dockMonitorTab,
        m_dockPrivilege, m_dockSettings, m_dockWindow, m_dockRegistry,
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
    m_pDockManager->addDockWidgetTabToArea(m_dockWindow, leftDockArea);
    m_pDockManager->addDockWidgetTabToArea(m_dockRegistry, leftDockArea);

    // 方法2: 或者使用addDockWidget并指定CenterDockWidgetArea
    // m_pDockManager->addDockWidget(ads::CenterDockWidgetArea, m_dockProcess, leftDockArea);

    // 4. 右侧区域：同理
    auto rightDockArea = m_pDockManager->addDockWidget(ads::RightDockWidgetArea, m_dockCurrentOp);

    // 右侧仅保留“当前操作 + 即时窗口”。
    m_pDockManager->addDockWidgetTabToArea(m_dockImmediate, rightDockArea);

    // 5. 底部区域：把“日志输出”与“监视面板”合并为同一组标签页。
    auto bottomDockArea = m_pDockManager->addDockWidget(ads::BottomDockWidgetArea, m_dockMonitor);
    m_pDockManager->addDockWidgetTabToArea(m_dockLog, bottomDockArea);

    // 6. 设置默认显示的标签页
    m_dockWelcome->raise();
    m_dockCurrentOp->raise();
    m_dockMonitor->raise();
}
