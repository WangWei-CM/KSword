#include "WinAPIDock.h"

// ============================================================
// WinAPIDock.cpp
// 作用：
// 1) 放置 WinAPI Dock 的基础样式、构造析构和轻量工具函数；
// 2) 把与 UI 结构无关的通用逻辑集中起来；
// 3) 避免把初始化、动作和管道代码堆进同一个实现文件。
// ============================================================

#include "../theme.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>

WinAPIDock::WinAPIDock(QWidget* parent)
    : QWidget(parent)
{
    // initEvent：
    // - 作用：串起整个 Dock 初始化链路；
    // - 约束：初始化阶段统一使用同一个 kLogEvent。
    kLogEvent initEvent;
    info << initEvent << "[WinAPIDock] 开始初始化 WinAPI 监控页。" << eol;

    initializeUi();
    initializeConnections();
    updateActionState();
    updateStatusLabel();

    info << initEvent << "[WinAPIDock] WinAPI 监控页初始化完成。" << eol;
}

WinAPIDock::~WinAPIDock()
{
    stopMonitoringInternal(true);

    if (m_uiFlushTimer != nullptr)
    {
        m_uiFlushTimer->stop();
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[WinAPIDock] WinAPI 监控页已析构。" << eol;
}

void WinAPIDock::notifyPageActivated()
{
    // 首次进入页签时立即刷新；之后 5 秒内重复切换不再重复扫进程，避免无意义抖动。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool shouldRefresh = !m_hasActivatedOnce
        || (m_lastProcessRefreshMs <= 0)
        || ((nowMs - m_lastProcessRefreshMs) >= 5000);

    m_hasActivatedOnce = true;
    if (shouldRefresh)
    {
        refreshProcessListAsync();
    }
}

QString WinAPIDock::blueButtonStyle()
{
    return KswordTheme::ThemedButtonStyle();
}

QString WinAPIDock::blueInputStyle()
{
    return QStringLiteral(
        "QLineEdit,QTableWidget{border:1px solid %2;border-radius:3px;background:%3;color:%4;padding:2px 6px;}"
        "QLineEdit:focus{border:1px solid %1;}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::BorderHex())
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::TextPrimaryHex());
}

QString WinAPIDock::blueHeaderStyle()
{
    return QStringLiteral(
        "QHeaderView::section{color:%1;background:%2;border:1px solid %3;padding:4px;font-weight:600;}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex());
}

QString WinAPIDock::buildStatusStyle(const QString& colorHex)
{
    return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
}

QString WinAPIDock::monitorInfoColorHex()
{
    return KswordTheme::IsDarkModeEnabled()
        ? QStringLiteral("#8FC7FF")
        : QStringLiteral("#1F4E7A");
}

QString WinAPIDock::monitorSuccessColorHex()
{
    return KswordTheme::IsDarkModeEnabled()
        ? QStringLiteral("#7EDC8A")
        : QStringLiteral("#2F7D32");
}

QString WinAPIDock::monitorWarningColorHex()
{
    return KswordTheme::IsDarkModeEnabled()
        ? QStringLiteral("#FFD48A")
        : QStringLiteral("#AA7B1C");
}

QString WinAPIDock::monitorErrorColorHex()
{
    return KswordTheme::IsDarkModeEnabled()
        ? QStringLiteral("#FF9B9B")
        : QStringLiteral("#A43434");
}

QString WinAPIDock::monitorIdleColorHex()
{
    return KswordTheme::TextSecondaryHex();
}

QString WinAPIDock::eventCategoryText(const std::uint32_t categoryValue)
{
    return QString::fromStdWString(
        ks::winapi_monitor::eventCategoryToText(
            static_cast<ks::winapi_monitor::EventCategory>(categoryValue)));
}

QString WinAPIDock::defaultDllPathHint()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidatePathList{
        appDir.filePath(QStringLiteral("APIMonitor_x64.dll")),
        appDir.filePath(QStringLiteral("../APIMonitor_x64.dll")),
        appDir.filePath(QStringLiteral("../../APIMonitor_x64.dll")),
        appDir.filePath(QStringLiteral("../../../../APIMonitor_x64/x64/Debug/APIMonitor_x64.dll")),
        appDir.filePath(QStringLiteral("../../../../APIMonitor_x64/x64/Release/APIMonitor_x64.dll"))
    };

    for (const QString& candidatePath : candidatePathList)
    {
        const QFileInfo fileInfo(candidatePath);
        if (fileInfo.exists() && fileInfo.isFile())
        {
            return QDir::cleanPath(fileInfo.absoluteFilePath());
        }
    }

    return QDir::cleanPath(candidatePathList.front());
}

QString WinAPIDock::resultCodeText(const std::int32_t resultCodeValue)
{
    if (resultCodeValue == 0)
    {
        return QStringLiteral("OK");
    }
    return QStringLiteral("%1 (0x%2)")
        .arg(resultCodeValue)
        .arg(QString::number(static_cast<quint32>(resultCodeValue), 16).toUpper());
}

QString WinAPIDock::now100nsText()
{
    FILETIME fileTimeValue{};
    ::GetSystemTimeAsFileTime(&fileTimeValue);

    ULARGE_INTEGER largeValue{};
    largeValue.LowPart = fileTimeValue.dwLowDateTime;
    largeValue.HighPart = fileTimeValue.dwHighDateTime;
    return QString::number(static_cast<qulonglong>(largeValue.QuadPart));
}

bool WinAPIDock::tryParseUint32Text(const QString& textValue, std::uint32_t* valueOut)
{
    if (valueOut == nullptr)
    {
        return false;
    }

    bool parseOk = false;
    const QString normalizedText = textValue.trimmed();
    if (normalizedText.isEmpty())
    {
        return false;
    }

    const std::uint32_t parsedValue = normalizedText.toUInt(&parseOk, 10);
    if (!parseOk || parsedValue == 0)
    {
        return false;
    }

    *valueOut = parsedValue;
    return true;
}

QTableWidgetItem* WinAPIDock::createReadOnlyItem(const QString& textValue)
{
    QTableWidgetItem* itemPointer = new QTableWidgetItem(textValue);
    itemPointer->setFlags(itemPointer->flags() & ~Qt::ItemIsEditable);
    itemPointer->setToolTip(textValue);
    return itemPointer;
}

void WinAPIDock::updateActionState()
{
    std::uint32_t currentPidValue = 0;
    const bool hasPid = currentSelectedPid(&currentPidValue);
    const bool running = m_pipeRunning.load();
    const bool hasEvents = m_eventTable != nullptr && m_eventTable->rowCount() > 0;

    if (m_processRefreshButton != nullptr)
    {
        m_processRefreshButton->setEnabled(!m_processRefreshPending.load() && !running);
    }
    if (m_browseAgentDllButton != nullptr)
    {
        m_browseAgentDllButton->setEnabled(!running);
    }
    if (m_manualPidEdit != nullptr)
    {
        m_manualPidEdit->setEnabled(!running);
    }
    if (m_agentDllPathEdit != nullptr)
    {
        m_agentDllPathEdit->setEnabled(!running);
    }
    if (m_startButton != nullptr)
    {
        m_startButton->setEnabled(
            !running
            && hasPid
            && m_agentDllPathEdit != nullptr
            && !m_agentDllPathEdit->text().trimmed().isEmpty());
    }
    if (m_stopButton != nullptr)
    {
        m_stopButton->setEnabled(running);
    }
    if (m_terminateHookButton != nullptr)
    {
        m_terminateHookButton->setEnabled(hasPid);
    }
    if (m_exportButton != nullptr)
    {
        m_exportButton->setEnabled(hasEvents);
    }
    if (m_clearEventButton != nullptr)
    {
        m_clearEventButton->setEnabled(hasEvents && !running);
    }
}

void WinAPIDock::updateStatusLabel()
{
    if (m_sessionStatusLabel == nullptr)
    {
        return;
    }

    const int eventCount = m_eventTable != nullptr ? m_eventTable->rowCount() : 0;
    const QString pidText = m_currentSessionPid == 0
        ? QStringLiteral("-")
        : QString::number(m_currentSessionPid);

    if (m_pipeRunning.load())
    {
        if (m_pipeConnected.load())
        {
            m_sessionStatusLabel->setText(
                QStringLiteral("● 监控中  PID=%1 | 事件=%2").arg(pidText).arg(eventCount));
            m_sessionStatusLabel->setStyleSheet(buildStatusStyle(monitorInfoColorHex()));
        }
        else
        {
            m_sessionStatusLabel->setText(
                QStringLiteral("● 等待 Agent 连接  PID=%1 | 事件=%2").arg(pidText).arg(eventCount));
            m_sessionStatusLabel->setStyleSheet(buildStatusStyle(monitorWarningColorHex()));
        }
    }
    else
    {
        m_sessionStatusLabel->setText(
            QStringLiteral("● 空闲  PID=%1 | 事件=%2").arg(pidText).arg(eventCount));
        m_sessionStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    }
}
