#include "Framework.h"
#include "MainWindow.h"
#include "Internationalization/LanguageManager.h"
#include "UI/McpLocalBridge.h"
#include "UI/McpHexCanvasAdapter.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

// 输入本次会话目标状态；不保存偏好。停止销毁旧控制器，下次启用建立全新对象身份。
void MainWindow::setMcpServiceEnabled(bool enabled)
{
    kLogEvent mcpEvent; // 本次启停调用链的统一日志事件，不包含用户输入或令牌。
    if (!enabled)
    {
        if (m_mcpLocalBridge)
        {
            // close 先取消排队动作并释放管道；延迟销毁保护从 MCP 动作回调进入的栈。
            m_mcpLocalBridge->close();
            m_mcpLocalBridge->deleteLater();
            m_mcpLocalBridge = nullptr;
            info << mcpEvent << ks::i18n::sourceText(QStringLiteral("本地 MCP 服务已停止。"))
                .toUtf8().toStdString() << eol;
        }
        m_mcpServiceError.clear();
        emit mcpServiceStateChanged(false, QString(), QString());
        return;
    }
    if (m_mcpLocalBridge && m_mcpLocalBridge->isListening())
    {
        // 重复启用只回报现状，不能创建第二个服务或重置仍有效的对象 ID。
        emit mcpServiceStateChanged(true, m_mcpLocalBridge->endpoint(), QString());
        return;
    }

    if (m_mcpLocalBridge)
    {
        // 非监听实例也先清理，保证重试不会遗留旧控制器或排队连接。
        m_mcpLocalBridge->close();
        m_mcpLocalBridge->deleteLater();
        m_mcpLocalBridge = nullptr;
    }

    // 每次启用都借用真实 Dock 目录和懒加载导航，不提前创建所有业务页面。
    m_mcpLocalBridge = new ks::ui::McpLocalBridge(this, [this]()
    {
        QJsonArray pages; // 当前主程序已登记的一级 Dock 目录。
        for (ads::CDockWidget* dock : collectSearchableDockWidgets())
        {
            if (!dock)
            {
                continue;
            }
            const QString id = dock->objectName(); // 原布局恢复使用的稳定英文键。
            const bool initialized = !dock->property("ks_lazy_key").isValid()
                || dock->property("ks_lazy_initialized").toBool();
            pages.append(QJsonObject{{QStringLiteral("id"), id},
                {QStringLiteral("title"), dock->windowTitle()},
                {QStringLiteral("initialized"), initialized}});
        }
        return pages;
    }, [this](const QString& pageId) -> QWidget*
    {
        // 原有业务导航保持不变，初始化期间的占位页不可作为成功打开的页面。
        for (ads::CDockWidget* dock : collectSearchableDockWidgets())
        {
            if (dock && dock->objectName() == pageId)
            {
                if (dock->property("ks_lazy_initializing").toBool())
                {
                    return nullptr;
                }
                QPointer<ads::CDockWidget> guardedDock(dock); // 初始化期间允许事件循环重入。
                activateDockForSearchNavigation(dock);
                if (!guardedDock || (guardedDock->property("ks_lazy_key").isValid()
                    && !guardedDock->property("ks_lazy_initialized").toBool()))
                {
                    return nullptr;
                }
                return guardedDock->widget();
            }
        }
        return nullptr;
    });
    ks::ui::registerMcpHexCanvasAdapter(m_mcpLocalBridge); // 自绘画布仍只读取现有缓存。
    const bool started = m_mcpLocalBridge->start(); // 设置页按真实 listen 结果显示状态。
    m_mcpServiceError = started ? QString() : m_mcpLocalBridge->lastError();
    const QString endpoint = m_mcpLocalBridge->endpoint(); // 监听成功后用于诊断和设置页。
    const QString diagnostic = started
        ? ks::i18n::sourceText(QStringLiteral("本地 MCP 桥已启用：%1")).arg(endpoint)
        : ks::i18n::sourceText(QStringLiteral("本地 MCP 桥启动失败：%1")).arg(m_mcpServiceError);
    if (started)
    {
        info << mcpEvent << diagnostic.toUtf8().toStdString() << eol;
    }
    else
    {
        // 失败也释放旧服务；重试时创建新控制器，避免重用已关闭对象。
        m_mcpLocalBridge->close();
        m_mcpLocalBridge->deleteLater();
        m_mcpLocalBridge = nullptr;
        warn << mcpEvent << diagnostic.toUtf8().toStdString() << eol;
    }
    emit mcpServiceStateChanged(started, endpoint, m_mcpServiceError);
}
