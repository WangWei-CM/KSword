#include "OtherDock.h"
#include "WindowCaptureProtection.h"

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

void OtherDock::setCaptureProtectionForSelectedWindow(const bool protectedState)
{
    QTreeWidgetItem* item = m_windowTree != nullptr ? m_windowTree->currentItem() : nullptr;
    if (item == nullptr || item->data(0, Qt::UserRole + 1).toBool())
    {
        kLogEvent event;
        warn << event
            << "[OtherDock] 防截图保护操作失败：未选中有效窗口。"
            << eol;
        QMessageBox::information(
            this,
            QStringLiteral("窗口防截图保护"),
            QStringLiteral("请先选中一个窗口。"));
        return;
    }

    const quint64 hwndValue = item->data(0, Qt::UserRole).toULongLong();
    const WindowInfo* windowInfo = findInfoByHwnd(hwndValue);
    if (windowInfo == nullptr)
    {
        kLogEvent event;
        warn << event
            << "[OtherDock] 防截图保护操作失败：选中 HWND 不在当前快照, hwnd="
            << formatHwndText(hwndValue).toStdString()
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("窗口防截图保护"),
            QStringLiteral("当前窗口快照已失效，请刷新后重试。"));
        return;
    }

    setCaptureProtectionForWindow(*windowInfo, protectedState);
}

void OtherDock::setCaptureProtectionForWindow(
    const WindowInfo& windowInfo,
    const bool protectedState)
{
    kLogEvent actionEvent;
    info << actionEvent
        << "[OtherDock] 开始窗口防截图保护操作, hwnd="
        << formatHwndText(windowInfo.hwndValue).toStdString()
        << ", pid="
        << windowInfo.processId
        << ", targetProtected="
        << (protectedState ? "true" : "false")
        << eol;

    const ks::window::CaptureProtectionResult result =
        ks::window::SetWindowCaptureProtection(windowInfo.hwndValue, protectedState);
    const QString messageText = buildProtectionMessage(result);

    if (result.success)
    {
        info << actionEvent
            << "[OtherDock] 窗口防截图保护操作成功, requestedHwnd="
            << formatHwndText(result.requestedHwnd).toStdString()
            << ", appliedHwnd="
            << formatHwndText(result.appliedHwnd).toStdString()
            << ", remote="
            << (result.usedRemoteThread ? "true" : "false")
            << ", affinity=0x"
            << std::hex
            << result.appliedAffinity
            << std::dec
            << eol;
        QMessageBox::information(
            this,
            QStringLiteral("窗口防截图保护"),
            messageText);
    }
    else
    {
        err << actionEvent
            << "[OtherDock] 窗口防截图保护操作失败, requestedHwnd="
            << formatHwndText(result.requestedHwnd).toStdString()
            << ", appliedHwnd="
            << formatHwndText(result.appliedHwnd).toStdString()
            << ", remote="
            << (result.usedRemoteThread ? "true" : "false")
            << ", error="
            << result.win32Error
            << ", detail="
            << result.detail
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("窗口防截图保护"),
            messageText);
    }

    refreshWindowListAsync();
}
