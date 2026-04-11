#include "WinAPIDock.h"

// ============================================================
// WinAPIDock.Actions.cpp
// 作用：
// 1) 实现 WinAPI Dock 的交互逻辑、会话控制和导出能力；
// 2) 把按钮动作、过滤与配置文件写入集中管理；
// 3) 与 Pipe 读取逻辑拆开，降低并发代码与 UI 代码耦合。
// ============================================================

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <TlHelp32.h>

namespace
{
    // toUtf8StdString：
    // - 作用：把 Qt 宽字符文本转成 UTF-8 std::string；
    // - 调用：Toolhelp 返回的进程名需要桥接到 ks::process::ProcessRecord。
    std::string toUtf8StdString(const QString& textValue)
    {
        return textValue.toUtf8().toStdString();
    }

    // collectProcessListLikeMemoryDock：
    // - 作用：复用内存页同款 Toolhelp 快照遍历，避免逐进程重型静态详情查询；
    // - 调用：WinAPI 页面进入或手动刷新时在后台线程调用。
    std::vector<ks::process::ProcessRecord> collectProcessListLikeMemoryDock()
    {
        std::vector<ks::process::ProcessRecord> processList;

        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return processList;
        }

        PROCESSENTRY32W processEntry{};
        processEntry.dwSize = sizeof(processEntry);
        if (::Process32FirstW(snapshotHandle, &processEntry) == FALSE)
        {
            ::CloseHandle(snapshotHandle);
            return processList;
        }

        do
        {
            ks::process::ProcessRecord record;
            record.pid = static_cast<std::uint32_t>(processEntry.th32ProcessID);
            record.parentPid = static_cast<std::uint32_t>(processEntry.th32ParentProcessID);
            record.threadCount = static_cast<std::uint32_t>(processEntry.cntThreads);
            record.processName = toUtf8StdString(QString::fromWCharArray(processEntry.szExeFile));
            record.imagePath = ks::process::QueryProcessPathByPid(record.pid);

            DWORD sessionId = 0;
            if (::ProcessIdToSessionId(static_cast<DWORD>(record.pid), &sessionId) != FALSE)
            {
                record.sessionId = static_cast<std::uint32_t>(sessionId);
            }

            processList.push_back(std::move(record));
        } while (::Process32NextW(snapshotHandle, &processEntry) != FALSE);

        ::CloseHandle(snapshotHandle);
        std::sort(
            processList.begin(),
            processList.end(),
            [](const ks::process::ProcessRecord& left, const ks::process::ProcessRecord& right) {
                return left.pid < right.pid;
            });
        return processList;
    }

    // extractSelectedRowPid：
    // - 作用：从进程表选中行提取 PID；
    // - 调用：当手动 PID 输入为空时，启动监控会走该辅助函数。
    bool extractSelectedRowPid(QTableWidget* tablePointer, std::uint32_t* pidOut)
    {
        if (tablePointer == nullptr || pidOut == nullptr)
        {
            return false;
        }

        const QList<QTableWidgetItem*> selectedItemList = tablePointer->selectedItems();
        if (selectedItemList.isEmpty())
        {
            return false;
        }

        QTableWidgetItem* pidItem = tablePointer->item(selectedItemList.front()->row(), WinAPIDock::ProcessColumnPid);
        if (pidItem == nullptr)
        {
            return false;
        }

        bool parseOk = false;
        const std::uint32_t pidValue = pidItem->text().trimmed().toUInt(&parseOk, 10);
        if (!parseOk || pidValue == 0)
        {
            return false;
        }

        *pidOut = pidValue;
        return true;
    }
}

void WinAPIDock::initializeConnections()
{
    if (m_processFilterEdit != nullptr)
    {
        connect(m_processFilterEdit, &QLineEdit::textChanged, this, [this]() {
            applyProcessFilter();
        });
    }
    if (m_processRefreshButton != nullptr)
    {
        connect(m_processRefreshButton, &QPushButton::clicked, this, [this]() {
            refreshProcessListAsync();
        });
    }
    if (m_processTable != nullptr)
    {
        connect(m_processTable, &QTableWidget::itemSelectionChanged, this, [this]() {
            updateActionState();
        });
        connect(m_processTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) {
            if (!m_pipeRunning.load())
            {
                startMonitoring();
            }
        });
    }

    if (m_browseAgentDllButton != nullptr)
    {
        connect(m_browseAgentDllButton, &QPushButton::clicked, this, [this]() {
            browseAgentDllPath();
        });
    }
    if (m_agentDllPathEdit != nullptr)
    {
        connect(m_agentDllPathEdit, &QLineEdit::textChanged, this, [this]() {
            updateActionState();
        });
    }
    if (m_manualPidEdit != nullptr)
    {
        connect(m_manualPidEdit, &QLineEdit::textChanged, this, [this]() {
            updateActionState();
        });
        connect(m_manualPidEdit, &QLineEdit::returnPressed, this, [this]() {
            startMonitoring();
        });
    }
    if (m_startButton != nullptr)
    {
        connect(m_startButton, &QPushButton::clicked, this, [this]() {
            startMonitoring();
        });
    }
    if (m_stopButton != nullptr)
    {
        connect(m_stopButton, &QPushButton::clicked, this, [this]() {
            stopMonitoring();
        });
    }
    if (m_terminateHookButton != nullptr)
    {
        connect(m_terminateHookButton, &QPushButton::clicked, this, [this]() {
            terminateHooksForSelectedProcess();
        });
    }
    if (m_exportButton != nullptr)
    {
        connect(m_exportButton, &QPushButton::clicked, this, [this]() {
            exportVisibleRowsToTsv();
        });
    }
    if (m_clearEventButton != nullptr)
    {
        connect(m_clearEventButton, &QPushButton::clicked, this, [this]() {
            if (m_eventTable != nullptr && !m_pipeRunning.load())
            {
                m_eventTable->clearContents();
                m_eventTable->setRowCount(0);
                applyEventFilter();
                updateActionState();
                updateStatusLabel();
            }
        });
    }

    if (m_eventFilterEdit != nullptr)
    {
        connect(m_eventFilterEdit, &QLineEdit::textChanged, this, [this]() {
            applyEventFilter();
        });
    }
    if (m_eventFilterClearButton != nullptr)
    {
        connect(m_eventFilterClearButton, &QPushButton::clicked, this, [this]() {
            clearEventFilter();
        });
    }
    if (m_eventTable != nullptr)
    {
        connect(m_eventTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
            showEventContextMenu(position);
        });
    }
    if (m_uiFlushTimer != nullptr)
    {
        connect(m_uiFlushTimer, &QTimer::timeout, this, [this]() {
            flushPendingRows();
        });
        m_uiFlushTimer->start();
    }
}

void WinAPIDock::refreshProcessListAsync()
{
    if (m_processRefreshPending.exchange(true))
    {
        return;
    }

    if (m_processStatusLabel != nullptr)
    {
        m_processStatusLabel->setText(QStringLiteral("● 正在刷新系统进程快照..."));
        m_processStatusLabel->setStyleSheet(buildStatusStyle(monitorInfoColorHex()));
    }
    updateActionState();

    QPointer<WinAPIDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<ks::process::ProcessRecord> processList = collectProcessListLikeMemoryDock();

        QMetaObject::invokeMethod(qApp, [guardThis, processList = std::move(processList)]() {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_processRefreshPending.store(false);
            guardThis->m_lastProcessRefreshMs = QDateTime::currentMSecsSinceEpoch();
            guardThis->populateProcessTable(processList);
            guardThis->updateActionState();
        }, Qt::QueuedConnection);
    }).detach();
}

void WinAPIDock::populateProcessTable(const std::vector<ks::process::ProcessRecord>& processList)
{
    m_processList = processList;
    if (m_processTable == nullptr)
    {
        return;
    }

    m_processTable->setSortingEnabled(false);
    m_processTable->clearContents();
    m_processTable->setRowCount(static_cast<int>(m_processList.size()));

    for (int row = 0; row < static_cast<int>(m_processList.size()); ++row)
    {
        const ks::process::ProcessRecord& record = m_processList[static_cast<std::size_t>(row)];
        m_processTable->setItem(row, ProcessColumnPid, createReadOnlyItem(QString::number(record.pid)));
        m_processTable->setItem(
            row,
            ProcessColumnName,
            createReadOnlyItem(QString::fromStdString(record.processName.empty() ? std::string("<Unknown>") : record.processName)));
        m_processTable->setItem(row, ProcessColumnPath, createReadOnlyItem(QString::fromStdString(record.imagePath)));
        m_processTable->setItem(row, ProcessColumnUser, createReadOnlyItem(QString::fromStdString(record.userName)));
    }

    m_processTable->setSortingEnabled(true);
    applyProcessFilter();

    if (m_processStatusLabel != nullptr)
    {
        m_processStatusLabel->setText(QStringLiteral("● 已刷新 %1 个进程").arg(m_processList.size()));
        m_processStatusLabel->setStyleSheet(buildStatusStyle(monitorSuccessColorHex()));
    }
}

void WinAPIDock::applyProcessFilter()
{
    if (m_processTable == nullptr)
    {
        return;
    }

    const QString keywordText = m_processFilterEdit != nullptr
        ? m_processFilterEdit->text().trimmed()
        : QString();

    int visibleRowCount = 0;
    for (int row = 0; row < m_processTable->rowCount(); ++row)
    {
        QStringList rowTextList;
        for (int column = 0; column < ProcessColumnCount; ++column)
        {
            QTableWidgetItem* itemPointer = m_processTable->item(row, column);
            rowTextList << (itemPointer != nullptr ? itemPointer->text() : QString());
        }

        const QString mergedText = rowTextList.join(QStringLiteral(" | "));
        const bool visible = keywordText.isEmpty()
            || mergedText.contains(keywordText, Qt::CaseInsensitive);
        m_processTable->setRowHidden(row, !visible);
        if (visible)
        {
            ++visibleRowCount;
        }
    }

    if (m_processStatusLabel != nullptr)
    {
        m_processStatusLabel->setText(
            QStringLiteral("● 可见 %1 / %2 个进程")
            .arg(visibleRowCount)
            .arg(m_processTable->rowCount()));
        m_processStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    }
}

bool WinAPIDock::currentSelectedPid(std::uint32_t* pidOut) const
{
    if (pidOut == nullptr)
    {
        return false;
    }
    *pidOut = 0;

    if (m_manualPidEdit != nullptr)
    {
        const QString manualPidText = m_manualPidEdit->text().trimmed();
        if (!manualPidText.isEmpty())
        {
            return tryParseUint32Text(manualPidText, pidOut);
        }
    }

    return extractSelectedRowPid(m_processTable, pidOut);
}

void WinAPIDock::browseAgentDllPath()
{
    const QString defaultPath = m_agentDllPathEdit != nullptr
        ? m_agentDllPathEdit->text().trimmed()
        : defaultDllPathHint();

    const QString selectedPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择 APIMonitor_x64.dll"),
        defaultPath,
        QStringLiteral("DLL 文件 (*.dll)"));
    if (selectedPath.trimmed().isEmpty())
    {
        return;
    }

    if (m_agentDllPathEdit != nullptr)
    {
        m_agentDllPathEdit->setText(QDir::cleanPath(selectedPath));
    }
}

bool WinAPIDock::prepareSessionArtifacts(const std::uint32_t pidValue, QString* errorTextOut)
{
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    const QString sessionDirectory = QString::fromStdWString(ks::winapi_monitor::buildSessionDirectory());
    QDir sessionDir;
    if (!sessionDir.mkpath(sessionDirectory))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("无法创建会话目录：%1").arg(sessionDirectory);
        }
        return false;
    }

    m_currentSessionPid = pidValue;
    m_currentPipeName = QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(pidValue));
    m_currentConfigPath = QString::fromStdWString(ks::winapi_monitor::buildConfigPathForPid(pidValue));
    m_currentStopFlagPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pidValue));

    if (QFile::exists(m_currentStopFlagPath))
    {
        QFile::remove(m_currentStopFlagPath);
    }
    return true;
}

bool WinAPIDock::writeSessionConfigFile(QString* errorTextOut) const
{
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    QFile configFile(m_currentConfigPath);
    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("无法写入会话配置：%1").arg(m_currentConfigPath);
        }
        return false;
    }

    QTextStream outputStream(&configFile);
    outputStream << "[monitor]\n";
    outputStream << "pipe_name=" << m_currentPipeName << '\n';
    outputStream << "stop_flag_path=" << m_currentStopFlagPath << '\n';
    outputStream << "enable_file=" << ((m_hookFileCheck != nullptr && m_hookFileCheck->isChecked()) ? 1 : 0) << '\n';
    outputStream << "enable_registry=" << ((m_hookRegistryCheck != nullptr && m_hookRegistryCheck->isChecked()) ? 1 : 0) << '\n';
    outputStream << "enable_network=" << ((m_hookNetworkCheck != nullptr && m_hookNetworkCheck->isChecked()) ? 1 : 0) << '\n';
    outputStream << "enable_process=" << ((m_hookProcessCheck != nullptr && m_hookProcessCheck->isChecked()) ? 1 : 0) << '\n';
    outputStream << "enable_loader=" << ((m_hookLoaderCheck != nullptr && m_hookLoaderCheck->isChecked()) ? 1 : 0) << '\n';
    outputStream << "detail_limit=" << static_cast<int>(ks::winapi_monitor::kMaxDetailChars - 1) << '\n';
    configFile.close();
    return true;
}

void WinAPIDock::appendInternalEvent(const QString& categoryText, const QString& apiText, const QString& detailText)
{
    EventRow rowValue;
    rowValue.time100nsText = now100nsText();
    rowValue.categoryText = categoryText;
    rowValue.apiText = apiText;
    rowValue.resultText = QStringLiteral("OK");
    rowValue.pidTidText = m_currentSessionPid == 0 ? QStringLiteral("-") : QStringLiteral("%1 / -").arg(m_currentSessionPid);
    rowValue.detailText = detailText;
    rowValue.internalEvent = true;
    appendEventRow(rowValue);
    applyEventFilter();
    updateActionState();
    updateStatusLabel();
}

void WinAPIDock::startMonitoring()
{
    if (m_pipeRunning.load())
    {
        return;
    }

    std::uint32_t pidValue = 0;
    if (!currentSelectedPid(&pidValue))
    {
        QMessageBox::information(this, QStringLiteral("WinAPI 监控"), QStringLiteral("请先选择目标进程或手动输入 PID。"));
        return;
    }

    if (m_agentDllPathEdit == nullptr)
    {
        return;
    }

    const QString dllPathText = QDir::cleanPath(m_agentDllPathEdit->text().trimmed());
    const QFileInfo dllFileInfo(dllPathText);
    if (!dllFileInfo.exists() || !dllFileInfo.isFile())
    {
        QMessageBox::warning(this, QStringLiteral("WinAPI 监控"), QStringLiteral("Agent DLL 不存在：%1").arg(dllPathText));
        return;
    }

    QString errorText;
    if (!prepareSessionArtifacts(pidValue, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("WinAPI 监控"), errorText);
        return;
    }
    if (!writeSessionConfigFile(&errorText))
    {
        QMessageBox::warning(this, QStringLiteral("WinAPI 监控"), errorText);
        return;
    }

    if (m_eventTable != nullptr)
    {
        m_eventTable->clearContents();
        m_eventTable->setRowCount(0);
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.clear();
    }

    m_pipeStopFlag.store(false);
    m_pipeRunning.store(true);
    m_pipeConnected.store(false);

    if (m_sessionProgressPid == 0)
    {
        m_sessionProgressPid = kPro.add("WinAPI", "准备会话");
    }
    kPro.set(m_sessionProgressPid, "准备命名管道连接", 0, 20.0f);

    startPipeReadThread();

    std::string detailText;
    const bool injectOk = ks::process::InjectDllByPath(pidValue, dllPathText.toStdString(), &detailText);
    if (!injectOk)
    {
        appendInternalEvent(QStringLiteral("内部"), QStringLiteral("InjectDllByPath"), QString::fromStdString(detailText));
        stopMonitoringInternal(false);
        QMessageBox::warning(this, QStringLiteral("WinAPI 监控"), QStringLiteral("DLL 注入失败：%1").arg(QString::fromStdString(detailText)));
        return;
    }

    appendInternalEvent(
        QStringLiteral("内部"),
        QStringLiteral("会话已启动"),
        QStringLiteral("已写入配置并完成 DLL 注入，等待 Agent 创建命名管道。"));

    kPro.set(m_sessionProgressPid, "DLL 已注入，等待 Agent 握手", 0, 45.0f);
    updateActionState();
    updateStatusLabel();
}

void WinAPIDock::stopMonitoring()
{
    stopMonitoringInternal(false);
}

void WinAPIDock::stopMonitoringInternal(const bool waitForThread)
{
    if (!m_pipeRunning.load() && (m_pipeThread == nullptr || !m_pipeThread->joinable()))
    {
        return;
    }

    m_pipeStopFlag.store(true);

    if (!m_currentStopFlagPath.trimmed().isEmpty())
    {
        QFile stopFile(m_currentStopFlagPath);
        if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            stopFile.write("stop");
            stopFile.close();
        }
    }

    const std::uintptr_t pipeHandleValue = m_pipeHandleValue.exchange(0);
    if (pipeHandleValue != 0)
    {
        ::CloseHandle(reinterpret_cast<HANDLE>(pipeHandleValue));
    }

    if (m_pipeThread != nullptr && m_pipeThread->joinable())
    {
        m_pipeThread->join();
    }
    m_pipeThread.reset();

    m_pipeRunning.store(false);
    m_pipeConnected.store(false);
    kPro.set(m_sessionProgressPid, "WinAPI 监控已停止", 0, 100.0f);

    if (!waitForThread)
    {
        appendInternalEvent(QStringLiteral("内部"), QStringLiteral("停止监控"), QStringLiteral("已发出停止标记并回收本地管道线程。"));
    }

    updateActionState();
    updateStatusLabel();
}

void WinAPIDock::terminateHooksForSelectedProcess()
{
    std::uint32_t pidValue = 0;
    if (!currentSelectedPid(&pidValue))
    {
        QMessageBox::information(this, QStringLiteral("终止 Hook"), QStringLiteral("请先选择目标进程或手动输入 PID。"));
        return;
    }

    const QString sessionDirectory = QString::fromStdWString(ks::winapi_monitor::buildSessionDirectory());
    QDir sessionDir;
    if (!sessionDir.mkpath(sessionDirectory))
    {
        QMessageBox::warning(this, QStringLiteral("终止 Hook"), QStringLiteral("无法创建会话目录：%1").arg(sessionDirectory));
        return;
    }

    const QString stopFlagPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pidValue));
    QFile stopFile(stopFlagPath);
    if (!stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, QStringLiteral("终止 Hook"), QStringLiteral("无法写入停止标记：%1").arg(stopFlagPath));
        return;
    }
    stopFile.write("stop");
    stopFile.close();

    if (m_pipeRunning.load() && m_currentSessionPid == pidValue)
    {
        stopMonitoringInternal(false);
    }
    else
    {
        appendInternalEvent(
            QStringLiteral("内部"),
            QStringLiteral("手动终止 Hook"),
            QStringLiteral("已为 PID=%1 写入停止标记。").arg(pidValue));
        updateActionState();
        updateStatusLabel();
    }
}

void WinAPIDock::applyEventFilter()
{
    if (m_eventTable == nullptr)
    {
        return;
    }

    const QString keywordText = m_eventFilterEdit != nullptr ? m_eventFilterEdit->text().trimmed() : QString();
    int visibleCount = 0;

    for (int row = 0; row < m_eventTable->rowCount(); ++row)
    {
        QStringList rowTextList;
        for (int column = 0; column < EventColumnCount; ++column)
        {
            QTableWidgetItem* itemPointer = m_eventTable->item(row, column);
            rowTextList << (itemPointer != nullptr ? itemPointer->text() : QString());
        }

        const QString mergedText = rowTextList.join(QStringLiteral(" | "));
        const bool visible = keywordText.isEmpty() || mergedText.contains(keywordText, Qt::CaseInsensitive);
        m_eventTable->setRowHidden(row, !visible);
        if (visible)
        {
            ++visibleCount;
        }
    }

    if (m_eventFilterStatusLabel != nullptr)
    {
        m_eventFilterStatusLabel->setText(
            QStringLiteral("筛选结果：%1 / %2").arg(visibleCount).arg(m_eventTable->rowCount()));
        m_eventFilterStatusLabel->setStyleSheet(buildStatusStyle(
            visibleCount > 0 ? monitorSuccessColorHex() : monitorIdleColorHex()));
    }
}

void WinAPIDock::clearEventFilter()
{
    if (m_eventFilterEdit != nullptr)
    {
        m_eventFilterEdit->clear();
    }
    applyEventFilter();
}

void WinAPIDock::exportVisibleRowsToTsv()
{
    if (m_eventTable == nullptr || m_eventTable->rowCount() == 0)
    {
        QMessageBox::information(this, QStringLiteral("导出 WinAPI 事件"), QStringLiteral("当前没有可导出的事件。"));
        return;
    }

    int visibleCount = 0;
    for (int row = 0; row < m_eventTable->rowCount(); ++row)
    {
        if (!m_eventTable->isRowHidden(row))
        {
            ++visibleCount;
        }
    }
    if (visibleCount == 0)
    {
        QMessageBox::information(this, QStringLiteral("导出 WinAPI 事件"), QStringLiteral("当前筛选结果为空。"));
        return;
    }

    const QString defaultFileName = QStringLiteral("winapi_events_%1.tsv")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString exportPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出 WinAPI 事件"),
        defaultFileName,
        QStringLiteral("TSV 文件 (*.tsv);;文本文件 (*.txt)"));
    if (exportPath.trimmed().isEmpty())
    {
        return;
    }

    QFile exportFile(exportPath);
    if (!exportFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        QMessageBox::warning(this, QStringLiteral("导出 WinAPI 事件"), QStringLiteral("无法写入文件：%1").arg(exportPath));
        return;
    }

    QTextStream outputStream(&exportFile);
    QStringList headerTextList;
    for (int column = 0; column < EventColumnCount; ++column)
    {
        QTableWidgetItem* headerItem = m_eventTable->horizontalHeaderItem(column);
        headerTextList << (headerItem != nullptr ? headerItem->text() : QString());
    }
    outputStream << headerTextList.join('\t') << '\n';

    for (int row = 0; row < m_eventTable->rowCount(); ++row)
    {
        if (m_eventTable->isRowHidden(row))
        {
            continue;
        }

        QStringList rowTextList;
        for (int column = 0; column < EventColumnCount; ++column)
        {
            QTableWidgetItem* itemPointer = m_eventTable->item(row, column);
            rowTextList << (itemPointer != nullptr ? itemPointer->text().replace('\t', ' ') : QString());
        }
        outputStream << rowTextList.join('\t') << '\n';
    }
    exportFile.close();
}

void WinAPIDock::showEventContextMenu(const QPoint& position)
{
    if (m_eventTable == nullptr)
    {
        return;
    }

    const QModelIndex indexValue = m_eventTable->indexAt(position);
    if (!indexValue.isValid())
    {
        return;
    }

    const int row = indexValue.row();
    const int column = indexValue.column();

    QMenu menu(this);
    QAction* copyCellAction = menu.addAction(QStringLiteral("复制单元格"));
    QAction* copyRowAction = menu.addAction(QStringLiteral("复制整行"));

    QAction* selectedAction = menu.exec(m_eventTable->viewport()->mapToGlobal(position));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == copyCellAction)
    {
        QTableWidgetItem* itemPointer = m_eventTable->item(row, column);
        if (itemPointer != nullptr)
        {
            QApplication::clipboard()->setText(itemPointer->text());
        }
        return;
    }

    if (selectedAction == copyRowAction)
    {
        QStringList rowTextList;
        for (int currentColumn = 0; currentColumn < EventColumnCount; ++currentColumn)
        {
            QTableWidgetItem* itemPointer = m_eventTable->item(row, currentColumn);
            rowTextList << (itemPointer != nullptr ? itemPointer->text() : QString());
        }
        QApplication::clipboard()->setText(rowTextList.join('\t'));
    }
}
