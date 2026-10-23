#pragma once

#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <windows.h>
#include <shellapi.h>

#include "../Internationalization/LanguageManager.h"
#include "../UI/CoordinatedApplicationQuit.h"
#include "../../../shared/crash/WinCrashHandler.h"

namespace ks::ui
{
    inline bool isPrivilegeRequiredError(const unsigned long errorCode)
    {
        return errorCode == ERROR_ACCESS_DENIED ||
            errorCode == ERROR_PRIVILEGE_NOT_HELD ||
            errorCode == ERROR_ELEVATION_REQUIRED ||
            errorCode == ERROR_NOT_ALL_ASSIGNED;
    }

    inline bool isCurrentProcessElevated()
    {
        HANDLE tokenHandle = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
        {
            return false;
        }

        TOKEN_ELEVATION elevation{};
        DWORD returnedBytes = 0;
        const BOOL queryOk = ::GetTokenInformation(
            tokenHandle,
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &returnedBytes);
        ::CloseHandle(tokenHandle);
        return queryOk != FALSE && elevation.TokenIsElevated != 0;
    }

    inline QString quoteWindowsArgument(const QString& argument)
    {
        QString quoted = argument;
        quoted.replace(QStringLiteral("\""), QStringLiteral("\\\""));
        return QStringLiteral("\"%1\"").arg(quoted);
    }

    // requestAdministratorRestartForFeature：
    // - 在功能已明确需要管理员权限、而当前实例尚未提升时调用；
    // - 对用户说明当前功能与重启影响，确认后以 runas 重新启动整个 KSword；
    // - 返回 false，调用方应立即停止当前功能，用户可在管理员实例中重试。
    inline bool requestAdministratorRestartForFeature(
        QWidget* const parent,
        const QString& featureName)
    {
        if (isCurrentProcessElevated())
        {
            return true;
        }

        // 私有桌面只通过继承句柄接入；runas 无法继承这些句柄，必须保留当前救援会话。
        if (QCoreApplication::instance() != nullptr
            && QCoreApplication::instance()->property("ksword_rescue_desktop").toBool())
        {
            QMessageBox notice(parent); // 使用不透明背景，避免继承主窗口的透明样式。
            notice.setIcon(QMessageBox::Information);
            notice.setWindowTitle(ks::i18n::text(
                QStringLiteral("rescue.desktop.status"), QStringLiteral("救援桌面")));
            notice.setText(ks::i18n::text(QStringLiteral("rescue.desktop.privilege"),
                QStringLiteral("救援实例保持当前权限。请返回原桌面后切换权限，再进入救援桌面。")));
            notice.setStyleSheet(QStringLiteral(
                "QMessageBox { background: palette(window); color: palette(window-text); }"
                "QMessageBox QLabel { background: transparent; color: palette(window-text); }"));
            notice.exec();
            return false;
        }

        QMessageBox prompt(parent);
        prompt.setIcon(QMessageBox::Information);
        prompt.setWindowTitle(ks::i18n::text(
            QStringLiteral("privilege.admin_required.title"),
            QStringLiteral("需要管理员权限")));
        prompt.setText(ks::i18n::text(
            QStringLiteral("privilege.admin_required.message"),
            QStringLiteral("“%1”需要管理员权限。\n\n是否以管理员身份重新启动 Ksword？")).arg(featureName));
        prompt.setInformativeText(ks::i18n::text(
            QStringLiteral("privilege.admin_required.hint"),
            QStringLiteral("确认 Windows UAC 后，当前实例会退出；请在新实例中重试该功能。")));
        QPushButton* const elevateButton = prompt.addButton(
            ks::i18n::text(
                QStringLiteral("privilege.admin_required.elevate"),
                QStringLiteral("以管理员身份重启")),
            QMessageBox::AcceptRole);
        prompt.addButton(
            ks::i18n::text(QStringLiteral("privilege.admin_required.cancel"), QStringLiteral("取消")),
            QMessageBox::RejectRole);
        prompt.exec();
        if (prompt.clickedButton() != elevateButton)
        {
            return false;
        }

        const QStringList argumentList = ArgumentsWithRestartPredecessorWait(
            QCoreApplication::arguments(), QCoreApplication::applicationPid());
        QStringList parameterList;
        for (int index = 1; index < argumentList.size(); ++index)
        {
            parameterList.push_back(QuoteCoordinatedRestartArgument(argumentList.at(index)));
        }
        if (!parameterList.contains(QStringLiteral("\"--ksword-privilege-restart\""), Qt::CaseInsensitive))
        {
            parameterList.push_back(QStringLiteral("--ksword-privilege-restart"));
        }

        const std::wstring executablePath = QCoreApplication::applicationFilePath().toStdWString();
        const std::wstring parameterText = parameterList.join(QLatin1Char(' ')).toStdWString();
        const QPointer<QWidget> parentGuard(parent);
        RequestCoordinatedApplicationRestart([parentGuard, executablePath, parameterText]()
        {
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        launch.hwnd = parentGuard ? reinterpret_cast<HWND>(parentGuard->winId()) : nullptr;
        launch.lpVerb = L"runas";
        launch.lpFile = executablePath.c_str();
        launch.lpParameters = parameterText.empty() ? nullptr : parameterText.c_str();
        launch.nShow = SW_SHOWNORMAL;
        const bool created = ::ShellExecuteExW(&launch) != FALSE;
        const bool validChild = created && ks::crash::ValidateRestartChildProcess(launch.hProcess);
        if (launch.hProcess) ::CloseHandle(launch.hProcess);
        if (!validChild)
        {
            QMessageBox::warning(
                parentGuard.data(),
                ks::i18n::text(
                    QStringLiteral("privilege.admin_required.restart_failed.title"),
                    QStringLiteral("管理员重启失败")),
                ks::i18n::text(
                    QStringLiteral("privilege.admin_required.restart_failed.message"),
                    QStringLiteral("管理员重启未启动，可能被取消或被系统策略阻止。")));
            return false;
        }
        return true;
        });
        return false;
    }

    // promptForPrivilegeFailure：
    // - 把“功能执行后才发现权限不足”的错误统一转换为功能级提权提示；
    // - 已经是管理员时返回 false，让调用方继续展示原始错误，不掩盖驱动/对象自身的拒绝。
    inline bool promptForPrivilegeFailure(
        QWidget* const parent,
        const QString& featureName,
        const unsigned long errorCode)
    {
        if (!isPrivilegeRequiredError(errorCode) || isCurrentProcessElevated())
        {
            return false;
        }
        (void)requestAdministratorRestartForFeature(parent, featureName);
        return true;
    }

    inline bool promptForPrivilegeFailure(
        QWidget* const parent,
        const QString& featureName,
        const QString& errorText)
    {
        const QString normalized = errorText.trimmed();
        if (isCurrentProcessElevated() ||
            !(normalized.contains(QStringLiteral("access is denied"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("拒绝访问"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("权限不足"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("error=5"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("win32=5"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("0x80070005"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("0x80320005"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("0x00000005"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("code=5"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("错误码=5"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("错误码：5"), Qt::CaseInsensitive) ||
                normalized.contains(QStringLiteral("错误码 5"), Qt::CaseInsensitive)))
        {
            return false;
        }
        (void)requestAdministratorRestartForFeature(parent, featureName);
        return true;
    }

    inline bool promptForPrivilegeNtStatus(
        QWidget* const parent,
        const QString& featureName,
        const long statusValue)
    {
        const unsigned long status = static_cast<unsigned long>(statusValue);
        if (isCurrentProcessElevated() ||
            (status != 0xC0000022UL && // STATUS_ACCESS_DENIED
                status != 0xC0000061UL && // STATUS_PRIVILEGE_NOT_HELD
                status != 0xC000042CUL)) // STATUS_INVALID_IMAGE_HASH/elevation policy denial
        {
            return false;
        }
        (void)requestAdministratorRestartForFeature(parent, featureName);
        return true;
    }
}
