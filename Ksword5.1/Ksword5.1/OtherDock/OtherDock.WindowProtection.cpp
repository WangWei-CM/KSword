#include "OtherDock.h"
#include "../UI/AsyncOperation.h"
#include "../UI/CodeTextEdit.h"
#include "WindowCaptureProtection.h"
#include "WindowListInteraction.h"
#include "../Framework/PrivilegeElevationPrompt.h"

// ============================================================
// OtherDock.WindowProtection.cpp
// 作用说明：
// 1) 连接窗口列表页的防截图保护 UI 操作；
// 2) 将选中窗口 HWND 交给 WindowCaptureProtection helper；
// 3) 统一输出日志、消息框和刷新动作。
// ============================================================

#include <QMessageBox>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <iomanip>
#include <atomic>
#include <thread>
#include <QStatusBar>
#include <QPushButton>

namespace
{
    // formatHwndText 作用：
    // - 将 HWND 整数值格式化为大写十六进制；
    // - 调用：消息框与日志摘要中展示目标窗口。
    QString formatHwndText(const quint64 hwndValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(hwndValue), 0, 16)
            .toUpper();
    }

    // affinityText 作用：
    // - 将 helper 返回的 affinity 值转换成 UI 文本；
    // - 传入 affinityValue：WDA_* 原始数值。
    QString affinityText(const std::uint32_t affinityValue)
    {
        return QString::fromStdString(ks::window::DisplayAffinityName(affinityValue));
    }

    // buildProtectionMessage 作用：
    // - 生成用户可读的成功/失败摘要；
    // - 传入 result：防截图写入结果；
    // - 传出：QString 消息正文。
    QString buildProtectionMessage(const ks::window::CaptureProtectionResult& result)
    {
        const QString actionText = result.requestedProtection
            ? QStringLiteral("启用防截图保护")
            : QStringLiteral("取消防截图保护");
        const QString routeText = result.usedRemoteThread
            ? QStringLiteral("跨进程远程调用")
            : QStringLiteral("本进程直接调用");

        QString messageText;
        messageText += result.success
            ? QStringLiteral("%1成功。\n").arg(actionText)
            : QStringLiteral("%1失败。\n").arg(actionText);
        messageText += QStringLiteral("选择窗口: %1\n").arg(formatHwndText(result.requestedHwnd));
        messageText += QStringLiteral("实际窗口: %1\n").arg(formatHwndText(result.appliedHwnd));
        messageText += QStringLiteral("目标 PID: %1\n").arg(result.processId);
        messageText += QStringLiteral("调用路径: %1\n").arg(routeText);
        const QString affinityHexText =
            QString::number(static_cast<qulonglong>(result.appliedAffinity), 16).toUpper();
        messageText += QStringLiteral("DisplayAffinity: 0x%1 (%2)\n")
            .arg(affinityHexText)
            .arg(affinityText(result.appliedAffinity));

        if (result.usedRootWindow)
        {
            messageText += QStringLiteral("说明: 选中的是子窗口，已对所属顶层窗口执行。\n");
        }
        if (!result.success)
        {
            messageText += QStringLiteral("错误码: %1\n").arg(result.win32Error);
            messageText += QStringLiteral("诊断: %1\n").arg(QString::fromStdString(result.detail));
            messageText += QStringLiteral("限制: 更高权限、受保护进程、32 位目标进程或非顶层窗口可能被系统拒绝。");
        }
        return messageText;
    }
}


namespace ks::window
{
    struct WindowOperationProgress
    {
        std::atomic_size_t completed{0};
        std::atomic_bool waitingForRemote{false};
        size_t total = 0;
    };
}
namespace
{
    std::atomic_bool& windowActionGate()
    { static std::atomic_bool busy{false}; return busy; }
    struct WindowActionAdmission
    {
        bool acquired = false;
        std::atomic_bool released{false};
        WindowActionAdmission()
        {
            bool expected = false;
            acquired = windowActionGate().compare_exchange_strong(expected, true);
        }
        void release()
        {
            if (acquired && !released.exchange(true)) windowActionGate().store(false);
        }
        ~WindowActionAdmission() { release(); }
    };
    struct WindowActionCompletion
    {
        std::shared_ptr<WindowActionAdmission> admission;
        ~WindowActionCompletion() { if (admission) admission->release(); }
    };
    struct WindowActionDesktop
    {
        HDESK handle = nullptr;
        DWORD error = ERROR_INVALID_HANDLE;
        ~WindowActionDesktop() { if (handle) ::CloseDesktop(handle); }
    };
    struct WindowActionTarget
    {
        OtherDock::WindowInfo info;
        ks::window::CaptureProtectionIdentity identity;
        bool admitted = false;
    };
    struct WindowActionRequest
    {
        std::vector<WindowActionTarget> targets;
        std::shared_ptr<WindowActionAdmission> admission;
        std::shared_ptr<WindowActionDesktop> desktop;
        std::shared_ptr<ks::window::WindowOperationProgress> progress;
        bool protection = false, testMessage = false;
        size_t selectedCount = 0;
    };
    struct WindowActionReply
    {
        std::vector<ks::window::CaptureProtectionResult> results;
        bool testMessage = false;
        size_t selectedCount = 0;
    };
    quint64 actionProcessCreated(DWORD pid)
    {
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return 0;
        FILETIME start{}, end{}, kernel{}, user{};
        const bool ready = ::GetProcessTimes(process, &start, &end, &kernel, &user) != FALSE;
        ::CloseHandle(process);
        return ready ? (static_cast<quint64>(start.dwHighDateTime) << 32) | start.dwLowDateTime : 0;
    }
    WindowActionReply runWindowAction(const WindowActionRequest& request)
    {
        WindowActionCompletion completion{request.admission};
        WindowActionReply reply;
        reply.results.reserve(request.targets.size());
        reply.testMessage = request.testMessage;
        reply.selectedCount = request.selectedCount;
        // A new OS thread can bind the frozen caller desktop. Thread-pool
        // workers may already own native message windows and must not rebind.
        std::exception_ptr failure;
        std::thread native([&] {
            const HDESK previous = ::GetThreadDesktop(::GetCurrentThreadId());
            const bool bound = request.desktop && request.desktop->handle &&
                ::SetThreadDesktop(request.desktop->handle) != FALSE;
            const DWORD bindError = bound ? ERROR_SUCCESS :
                (request.desktop && request.desktop->handle ? ::GetLastError() :
                    request.desktop ? request.desktop->error : ERROR_INVALID_HANDLE);
            try {
                for (const auto& target : request.targets) {
                    ks::window::CaptureProtectionResult result;
                    result.requestedHwnd = target.info.hwndValue;
                    result.appliedHwnd = target.identity.rootHwnd;
                    result.processId = target.identity.rootPid;
                    result.requestedProtection = request.protection;
                    if (!bound) {
                        result.win32Error = bindError ? bindError : ERROR_INVALID_HANDLE;
                        result.detail = "caller desktop binding failed";
                    } else if (!target.admitted || !ks::window::windowIdentityMatches(
                        target.info.hwndValue, target.info.processId, target.info.threadId,
                        target.info.processCreationTime100ns)) {
                        result.win32Error = ERROR_INVALID_WINDOW_HANDLE;
                        result.detail = "frozen selected window identity changed";
                    } else if (request.testMessage) {
                        DWORD_PTR messageResult = 0;
                        ::SetLastError(ERROR_SUCCESS);
                        result.success = ::SendMessageTimeoutW(
                            reinterpret_cast<HWND>(static_cast<quintptr>(target.info.hwndValue)),
                            WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1200, &messageResult) != 0;
                        result.win32Error = result.success ? ERROR_SUCCESS : ::GetLastError();
                        if (!result.success && !result.win32Error) result.win32Error = ERROR_TIMEOUT;
                        result.detail = result.success ? "WM_NULL completed" : "WM_NULL failed or timed out";
                    } else {
                        result = ks::window::SetWindowCaptureProtection(target.info.hwndValue,
                            request.protection, target.identity, [progress=request.progress] {
                                progress->waitingForRemote.store(true, std::memory_order_relaxed);
                            });
                    }
                    // The native API result is the transaction receipt. A
                    // later identity change cannot undo or invent its success.
                    if (result.success && !ks::window::windowIdentityMatches(
                        target.info.hwndValue, target.info.processId, target.info.threadId,
                        target.info.processCreationTime100ns))
                        result.detail += "; target identity changed after native completion";
                    reply.results.push_back(std::move(result));
                    request.progress->waitingForRemote.store(false, std::memory_order_relaxed);
                    request.progress->completed.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) { failure = std::current_exception(); }
            if (bound) ::SetThreadDesktop(previous);
        });
        native.join(); // only the pool worker waits, never the GUI owner
        if (failure) std::rethrow_exception(failure);
        return reply;
    }
}

void OtherDock::initializeWindowTransactions()
{
    m_windowActionOperation = new ks::ui::AsyncOperation(this);
    m_windowActionTimer = new QTimer(this);
    m_windowActionTimer->setInterval(100);
    connect(m_windowActionTimer, &QTimer::timeout, this, [this] {
        if (!m_windowActionProgress) return;
        const auto& progress = *m_windowActionProgress;
        m_statusBar->showMessage(progress.waitingForRemote.load(std::memory_order_relaxed)
            ? QStringLiteral("操作仍在目标中执行，正在等待安全收尾…")
            : QStringLiteral("正在执行窗口操作：%1/%2")
                .arg(progress.completed.load(std::memory_order_relaxed)).arg(progress.total));
    });
    m_windowActionOperation->setStateChangedCallback([this] {
        if (m_windowActionOperation->isBusy()) m_windowActionTimer->start();
        else { m_windowActionTimer->stop(); m_windowActionProgress.reset(); }
        scheduleWindowSelectionUi();
    });
}

bool OtherDock::windowTransactionBusy() const
{ return windowActionGate().load() || (m_windowActionOperation && m_windowActionOperation->isBusy()); }

void OtherDock::setCaptureProtectionForSelectedWindows(const bool protectedState)
{
    const auto windows = selectedWindowSnapshots();
    if (windows.empty()) {
        QMessageBox::information(this, QStringLiteral("窗口防截图保护"), QStringLiteral("请先选中一个窗口。"));
        return;
    }
    setCaptureProtectionForWindows(windows, protectedState);
}

void OtherDock::setCaptureProtectionForWindows(const std::vector<WindowInfo>& windows, bool protectedState)
{ submitWindowTransaction(windows, protectedState, false); }

void OtherDock::sendTestMessagesForWindows(const std::vector<WindowInfo>& windows)
{ submitWindowTransaction(windows, false, true); }

void OtherDock::submitWindowTransaction(const std::vector<WindowInfo>& windows, bool protection, bool testMessage)
{
    if (windows.empty()) return;
    if (windowTransactionBusy()) {
        m_statusBar->showMessage(QStringLiteral("已有窗口操作正在执行，请等待安全收尾。"));
        return;
    }
    WindowActionRequest request;
    request.admission = std::make_shared<WindowActionAdmission>();
    if (!request.admission->acquired) return;
    request.protection = protection; request.testMessage = testMessage; request.selectedCount = windows.size();
    request.progress = std::make_shared<ks::window::WindowOperationProgress>();
    request.desktop = std::make_shared<WindowActionDesktop>();
    HANDLE duplicate = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), ::GetThreadDesktop(::GetCurrentThreadId()),
        ::GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        request.desktop->handle = static_cast<HDESK>(duplicate);
    else request.desktop->error = ::GetLastError();
    QSet<quint64> roots;
    for (const auto& window : windows) {
        WindowActionTarget target; target.info = window;
        auto& identity = target.identity;
        identity.requestedPid = window.processId; identity.requestedTid = window.threadId;
        identity.requestedCreated = window.processCreationTime100ns;
        target.admitted = window.valid && ks::window::windowIdentityMatches(window.hwndValue,
            window.processId, window.threadId, window.processCreationTime100ns);
        const HWND selected = reinterpret_cast<HWND>(static_cast<quintptr>(window.hwndValue));
        const HWND root = target.admitted ? ::GetAncestor(selected, GA_ROOT) : nullptr;
        identity.rootHwnd = root ? static_cast<quint64>(reinterpret_cast<quintptr>(root)) : window.hwndValue;
        if (!testMessage && target.admitted && roots.contains(identity.rootHwnd)) continue;
        if (target.admitted) roots.insert(identity.rootHwnd);
        DWORD pid = 0;
        identity.rootTid = target.admitted ? ::GetWindowThreadProcessId(root ? root : selected, &pid) : 0;
        identity.rootPid = pid;
        identity.rootCreated = pid == window.processId && window.processCreationTime100ns
            ? window.processCreationTime100ns : actionProcessCreated(pid);
        target.admitted = target.admitted && identity.rootTid != 0;
        request.targets.push_back(std::move(target));
    }
    request.progress->total = request.targets.size();
    m_windowActionProgress = request.progress;
    m_windowActionOperation->submitTransaction<WindowActionRequest, WindowActionReply>(std::move(request),
        runWindowAction,
        [this](const WindowActionReply& reply, std::uint64_t) {
            int succeeded = 0; QStringList failures;
            for (const auto& result : reply.results) {
                if (result.success) ++succeeded;
                else failures.push_back(formatHwndText(result.requestedHwnd));
                kLogEvent event;
                info << event << "[OtherDock] accepted window operation completed, hwnd="
                    << result.requestedHwnd << ", success=" << result.success
                    << ", error=" << result.win32Error << ", detail=" << result.detail << eol;
            }
            const QPointer<OtherDock> alive(this);
            refreshWindowListAsync();
            if (!alive) return true;
            if (!reply.testMessage && reply.selectedCount == 1 && reply.results.size() == 1) {
                const auto& result = reply.results.front();
                bool handled = !result.success && ks::ui::promptForPrivilegeFailure(this,
                    result.requestedProtection ? QStringLiteral("启用窗口防截图保护") : QStringLiteral("取消窗口防截图保护"), result.win32Error);
                if (!alive) return true;
                if (!result.success && !handled) handled = ks::ui::promptForPrivilegeFailure(this,
                    result.requestedProtection ? QStringLiteral("启用窗口防截图保护") : QStringLiteral("取消窗口防截图保护"), QString::fromStdString(result.detail));
                if (!alive || handled) return true;
                if (result.success) QMessageBox::information(this, QStringLiteral("窗口防截图保护"), buildProtectionMessage(result));
                else QMessageBox::warning(this, QStringLiteral("窗口防截图保护"), buildProtectionMessage(result));
            } else {
                const QString action = reply.testMessage ? QStringLiteral("发送测试消息")
                    : (!reply.results.empty() && reply.results.front().requestedProtection
                        ? QStringLiteral("启用防截图保护") : QStringLiteral("取消防截图保护"));
                const QString message = QStringLiteral("操作：%1\n成功：%2\n失败或已失效：%3\n%4")
                    .arg(action).arg(succeeded).arg(failures.size()).arg(failures.join('\n'));
                if (failures.empty()) QMessageBox::information(this, action, message);
                else QMessageBox::warning(this, action, message);
            }
            return true;
        }, [this](std::exception_ptr) {
            QMessageBox::warning(this, QStringLiteral("窗口防截图保护"), QStringLiteral("刷新失败"));
        });
}

// WindowLayerDiagnostics.inl is intentionally included here so the feature is
// compiled once without expanding the already large OtherDock.cpp translation unit.
#include "WindowLayerDiagnostics.inl"
