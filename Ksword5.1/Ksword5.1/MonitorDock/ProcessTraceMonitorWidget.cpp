#include "ProcessTraceEventMetadata.h"
#include "MonitorEventFilterProjection.h"
#include "ProcessTraceMonitorWidget.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ThemeStatusRole.h"
#include "../UI/MainSurfaceStyle.h"

// ============================================================
// ProcessTraceMonitorWidget.cpp
// 作用：
// 1) 承载进程定向监控控件的公共样式与基础工具函数；
// 2) 提供构造、析构与状态文本刷新等轻量逻辑；
// 3) 避免把 UI、动作、采集实现继续堆进同一个大文件。
// ============================================================

#include "../theme.h"

#include <QDateTime>
#include <QEvent>
#include <QLabel>
#include <QItemSelectionModel>
#include <QList>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QTimer>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Objbase.h>
#include <evntcons.h>
#include <tdh.h>

#pragma comment(lib, "Tdh.lib")

ProcessTraceMonitorWidget::ProcessTraceMonitorWidget(QWidget* parent)
    : QWidget(parent)
    , m_uiDispatcher(std::make_shared<ks::ui::AsyncUiDispatcher>(this))
{
    // initEvent：构造阶段统一复用同一个日志事件，便于串起整个初始化链路。
    kLogEvent initEvent;
    info << initEvent << "[ProcessTraceMonitorWidget] 开始初始化进程定向监控页。" << eol;

    initializeUi();
    if (m_targetTable) m_targetTable->installEventFilter(this);
    if (m_eventTable) m_eventTable->installEventFilter(this);
    if (m_availableTable) ks::ui::SetTableRowFilterBusinessOwnership(m_availableTable,
        ks::ui::TableRowFilterBusinessOwnership::ExclusiveSharedIdentity);
    initializeConnections();
    refreshAvailableProcessListAsync();
    updateActionState();
    updateStatusLabel();

    info << initEvent << "[ProcessTraceMonitorWidget] 进程定向监控页初始化完成。" << eol;
}

ProcessTraceMonitorWidget::~ProcessTraceMonitorWidget()
{
    if (m_eventFilterProjection) m_eventFilterProjection->close();
    if (m_targetLookup) m_targetLookup->close();
    m_uiDispatcher->close();
    // Only signal and enqueue retained value cleanup; the GUI never joins capture workers.
    stopMonitoringInternal(true);

    if (m_uiUpdateTimer != nullptr)
    {
        m_uiUpdateTimer->stop();
    }

    if (m_eventFilterDebounceTimer != nullptr)
    {
        m_eventFilterDebounceTimer->stop();
    }

    if (m_runtimeRefreshTimer != nullptr)
    {
        m_runtimeRefreshTimer->stop();
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[ProcessTraceMonitorWidget] 进程定向监控页已析构。" << eol;
}

QString ProcessTraceMonitorWidget::blueButtonStyle()
{
    return KswordTheme::ThemedButtonStyle();
}

QString ProcessTraceMonitorWidget::blueInputStyle()
{
    return QStringLiteral(
        "QLineEdit{border:1px solid %2;border-radius:5px;background:%3;color:%4;padding:2px 8px;}"
        "QLineEdit:focus{border:1px solid %1;}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::BorderHex())
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::TextPrimaryHex())
        + KswordTheme::ThemedComboBoxStyle();
}

QString ProcessTraceMonitorWidget::blueHeaderStyle()
{
    return QStringLiteral(
        "QHeaderView::section{color:%1;background:%2;border:0;border-bottom:1px solid %3;padding:4px 8px;font-weight:600;}")
        .arg(KswordTheme::TextPrimaryHex())
        .arg(KswordTheme::SurfaceAltHex())
        .arg(KswordTheme::BorderHex());
}

QString ProcessTraceMonitorWidget::collapsePanelStyle(const QWidget* panel)
{
    // 样式复用 MonitorDock 已有独立 Collapse 约定：
    // - 外层明确使用数据面和圆角，内层宿主不继续叠加更亮的卡片底；
    // - 内容宿主使用 kswordCollapseContent 标记，避免嵌套区域重复描边。
    return QStringLiteral(
        "QWidget[kswordCollapsePanel=\"true\"]{"
        "  background:%1;"
        "  color:%2;"
        "  border:0;"
        "  border-radius:8px;"
        "}"
        "QWidget[kswordCollapseContent=\"true\"]{"
        "  background:transparent;"
        "  background-color:transparent;"
        "  color:%2;"
        "  border:none;"
        "}")
        .arg(ks::ui::UsesMainSurfaceHierarchy(panel)
            ? ks::ui::MainWindowSurfaceColors().content.name() : KswordTheme::SurfaceHex())
        .arg(KswordTheme::TextPrimaryHex());
}

QString ProcessTraceMonitorWidget::collapseHeaderButtonStyle()
{
    // 纯色主题只接管颜色；保留本页按钮尺寸和业务选中状态。
    return ks::ui::BuildFlatButtonStyle(ks::ui::FlatButtonTone::Neutral)
        + QStringLiteral("QToolButton{border-radius:5px;padding:5px 8px;font-weight:600;text-align:left;}");

}

QString ProcessTraceMonitorWidget::providerTypeFromName(const QString& providerNameText)
{
    const QString nameText = providerNameText.trimmed();
    if (nameText.contains(QStringLiteral("Kernel-Process"), Qt::CaseInsensitive))
    {
        return QStringLiteral("进程");
    }
    if (nameText.contains(QStringLiteral("Kernel-Thread"), Qt::CaseInsensitive))
    {
        return QStringLiteral("线程");
    }
    if (nameText.contains(QStringLiteral("Kernel-Image"), Qt::CaseInsensitive))
    {
        return QStringLiteral("镜像");
    }
    if (nameText.contains(QStringLiteral("Kernel-File"), Qt::CaseInsensitive))
    {
        return QStringLiteral("文件");
    }
    if (nameText.contains(QStringLiteral("Kernel-Registry"), Qt::CaseInsensitive))
    {
        return QStringLiteral("注册表");
    }
    if (nameText.contains(QStringLiteral("DNS-Client"), Qt::CaseInsensitive))
    {
        return QStringLiteral("DNS");
    }
    if (nameText.contains(QStringLiteral("TCPIP"), Qt::CaseInsensitive)
        || nameText.contains(QStringLiteral("AFD"), Qt::CaseInsensitive))
    {
        return QStringLiteral("网络");
    }
    if (nameText.contains(QStringLiteral("PowerShell"), Qt::CaseInsensitive))
    {
        return QStringLiteral("PowerShell");
    }
    if (nameText.contains(QStringLiteral("WMI-Activity"), Qt::CaseInsensitive))
    {
        return QStringLiteral("WMI");
    }
    if (nameText.contains(QStringLiteral("TaskScheduler"), Qt::CaseInsensitive))
    {
        return QStringLiteral("计划任务");
    }
    if (nameText.contains(QStringLiteral("Security-Auditing"), Qt::CaseInsensitive))
    {
        return QStringLiteral("安全审计");
    }
    if (nameText.contains(QStringLiteral("Defender"), Qt::CaseInsensitive))
    {
        return QStringLiteral("Defender");
    }
    return QStringLiteral("其他");
}

QString ProcessTraceMonitorWidget::now100nsText()
{
    return QString::number(static_cast<qulonglong>(currentSystemTime100ns()));
}

std::uint64_t ProcessTraceMonitorWidget::currentSystemTime100ns()
{
    FILETIME fileTimeValue{};
    ::GetSystemTimeAsFileTime(&fileTimeValue);

    ULARGE_INTEGER largeValue{};
    largeValue.LowPart = fileTimeValue.dwLowDateTime;
    largeValue.HighPart = fileTimeValue.dwHighDateTime;
    return static_cast<std::uint64_t>(largeValue.QuadPart);
}

QString ProcessTraceMonitorWidget::guidToText(const GUID& guidValue)
{
    wchar_t guidBuffer[64] = {};
    if (::StringFromGUID2(guidValue, guidBuffer, static_cast<int>(std::size(guidBuffer))) <= 0)
    {
        return QStringLiteral("{00000000-0000-0000-0000-000000000000}");
    }
    return QString::fromWCharArray(guidBuffer);
}

QString ProcessTraceMonitorWidget::queryEtwEventName(const struct _EVENT_RECORD* eventRecordPtr)
{
    const auto* record = reinterpret_cast<const EVENT_RECORD*>(eventRecordPtr);
    return ks::monitor::ProcessTraceEventMetadata::eventName(ks::monitor::processTraceMetadataForThread().get(record));
}

bool ProcessTraceMonitorWidget::textMatch(
    const QString& sourceText,
    const QString& patternText,
    const bool useRegex,
    const Qt::CaseSensitivity caseSensitivity)
{
    if (patternText.trimmed().isEmpty())
    {
        return true;
    }

    if (!useRegex)
    {
        return sourceText.contains(patternText, caseSensitivity);
    }

    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
    if (caseSensitivity == Qt::CaseInsensitive)
    {
        options |= QRegularExpression::CaseInsensitiveOption;
    }

    const QRegularExpression regex(patternText, options);
    if (!regex.isValid())
    {
        return false;
    }
    return regex.match(sourceText).hasMatch();
}

bool ProcessTraceMonitorWidget::tryParseUint32Text(const QString& textValue, std::uint32_t* valueOut)
{
    if (valueOut == nullptr)
    {
        return false;
    }

    bool parseOk = false;
    QString normalizedText = textValue.trimmed();
    if (normalizedText.isEmpty())
    {
        return false;
    }

    std::uint32_t parsedValue = 0;
    if (normalizedText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    {
        parsedValue = normalizedText.mid(2).toUInt(&parseOk, 16);
    }
    else
    {
        parsedValue = normalizedText.toUInt(&parseOk, 10);
        if (!parseOk)
        {
            parsedValue = normalizedText.toUInt(&parseOk, 16);
        }
    }

    if (!parseOk || parsedValue == 0)
    {
        return false;
    }

    *valueOut = parsedValue;
    return true;
}

QTableWidgetItem* ProcessTraceMonitorWidget::createReadOnlyItem(const QString& textValue)
{
    QTableWidgetItem* itemPointer = new QTableWidgetItem(textValue);
    itemPointer->setFlags(itemPointer->flags() & ~Qt::ItemIsEditable);
    itemPointer->setToolTip(textValue);
    return itemPointer;
}

void ProcessTraceMonitorWidget::updateActionState()
{
    const bool hasTargets = !m_targetProcessList.empty();
    const bool running = m_capture->m_captureRunning.load();
    const bool lookingUp = m_targetLookup && m_targetLookup->isInFlight();
    const bool paused = m_capture->m_capturePaused.load();

    if (m_availableRefreshButton != nullptr)
    {
        m_availableRefreshButton->setEnabled(!running && !m_availableRefreshPending.load());
    }
    if (m_createTargetButton != nullptr)
    {
        m_createTargetButton->setEnabled(!running);
    }
    if (m_addSelectedButton != nullptr)
    {
        m_addSelectedButton->setEnabled(!running && m_availableTable != nullptr && m_availableTable->selectedItems().size() > 0);
    }
    if (m_addManualPidButton != nullptr)
    {
        m_addManualPidButton->setEnabled(!running && !lookingUp);
    }
    if (m_removeTargetButton != nullptr)
    {
        m_removeTargetButton->setEnabled(!running && m_targetTable != nullptr && m_targetTable->selectedItems().size() > 0);
    }
    if (m_clearTargetButton != nullptr)
    {
        m_clearTargetButton->setEnabled(!running && hasTargets);
    }
    if (m_startButton != nullptr)
    {
        m_startButton->setEnabled((!running && hasTargets && !lookingUp) || (running && paused));
        m_startButton->setIcon(QIcon(paused
            ? QStringLiteral(":/Icon/process_resume.svg")
            : QStringLiteral(":/Icon/process_start.svg")));
        m_startButton->setToolTip(paused
            ? QStringLiteral("继续处理与目标进程相关的事件")
            : QStringLiteral("开始监控已选择的目标进程"));
    }
    if (m_stopButton != nullptr)
    {
        m_stopButton->setEnabled(running || (m_captureCleanup && !m_captureCleanup->isComplete()));
    }
    if (m_pauseButton != nullptr)
    {
        m_pauseButton->setEnabled(running && !paused);
        m_pauseButton->setIcon(QIcon(QStringLiteral(":/Icon/process_pause.svg")));
        m_pauseButton->setToolTip(QStringLiteral("暂停处理与目标进程相关的事件"));
    }
    if (m_exportButton != nullptr)
    {
        m_exportButton->setEnabled(m_eventTable != nullptr && m_eventTable->rowCount() > 0 && !eventProjectionPending()
            && (!m_eventFilterProjection || !m_eventFilterProjection->isPending()));
    }
}

void ProcessTraceMonitorWidget::updateStatusLabel()
{
    if (m_statusLabel == nullptr)
    {
        return;
    }

    std::size_t trackedCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_capture->m_runtimeMutex);
        trackedCount = m_capture->m_trackedProcessMap.size();
    }

    const auto eventCount = m_eventArchive.size();
    const QString summaryText = QStringLiteral("目标=%1 | 进程树=%2 | 事件=%3")
        .arg(m_targetProcessList.size())
        .arg(static_cast<qulonglong>(trackedCount))
        .arg(eventCount);

    if (m_captureCleanup && m_captureCleanup->hasFailed())
    {
        m_statusLabel->setText(QStringLiteral("● 停止失败，可重试（错误码=%1）  %2").arg(m_capture->operationError.load()).arg(summaryText));
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Warning);
    }
    else if (m_captureCleanup && m_captureCleanup->isCleanupRequested() && !m_captureCleanup->isComplete())
    {
        m_statusLabel->setText(QStringLiteral("● 正在停止  %1").arg(summaryText));
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Info);
    }
    else if (m_capture->m_captureRunning.load())
    {
        if (!m_capture->processStarted.load())
        {
            m_statusLabel->setText(QStringLiteral("● 准备中  %1").arg(summaryText));
            ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Info);
        }
        else if (m_capture->m_capturePaused.load())
        {
            m_statusLabel->setText(QStringLiteral("● 已暂停  %1").arg(summaryText));
            ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Warning);
        }
        else
        {
            m_statusLabel->setText(QStringLiteral("● 监听中  %1").arg(summaryText));
            ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Info);
        }
    }
    else if (const auto error = m_capture->startupError.load() ? m_capture->startupError.load() : m_capture->processStatus.load())
    {
        m_statusLabel->setText(QStringLiteral("ProcessTrace 结束，状态码=%1。").arg(error));
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Warning);
    }
    else
    {
        m_statusLabel->setText(QStringLiteral("● 空闲  %1").arg(summaryText));
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Idle);
    }
}

bool ProcessTraceMonitorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_targetTable && event->type() == QEvent::Show)
        QTimer::singleShot(0, this, [this]() { if (m_targetProjectionDirty) refreshTargetTable(); });
    if (watched == m_eventTable && event->type() == QEvent::Hide)
    {
        m_restoreSelectedEvents.clear();
        if (m_eventTable->selectionModel())
            for (const auto& index : m_eventTable->selectionModel()->selectedRows())
                if (auto record = eventRecordAt(index.row())) m_restoreSelectedEvents.push_back(std::move(record));
        m_restoreCurrentEvent = eventRecordAt(m_eventTable->currentRow());
        m_restoreTopEvent = eventRecordAt(m_eventTable->indexAt(QPoint(0,0)).row());
        m_eventProjectionStale = true; m_eventProjectionQueue.clear(); m_expiredEventProjection.clear();
        if (m_eventFilterProjection) m_eventFilterProjection->setSuspended(true);
        updateActionState();
    }
    if (watched == m_eventTable && event->type() == QEvent::Show)
        QTimer::singleShot(0, this, [this]() { flushPendingRows(); });
    return QWidget::eventFilter(watched, event);
}
