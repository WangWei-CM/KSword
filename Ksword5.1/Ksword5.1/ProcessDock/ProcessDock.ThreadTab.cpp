#include "ProcessDock.h"

#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPushButton>
#include <QRunnable>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace
{
    // 线程页表头：强调线程基础身份、调度状态与时间统计。
    const QStringList ThreadTableHeaders{
        "TID",
        "PID",
        "进程",
        "启动地址",
        "动态优先级",
        "基础优先级",
        "线程状态",
        "等待原因",
        "Kernel(ms)",
        "User(ms)",
        "CPU累计(ms)",
        "WaitTick",
        "上下文切换",
        "创建时间",
        "进程路径"
    };

    // 线程页图标常量：统一从 qrc alias 读取，避免硬编码磁盘路径。
    constexpr const char* IconThreadTab = ":/Icon/process_threads.svg";
    constexpr const char* IconThreadRefresh = ":/Icon/process_refresh.svg";

    // 线程页按钮图标尺寸：与 ProcessDock 主控栏保持一致。
    constexpr QSize ThreadDefaultIconSize(16, 16);
    constexpr QSize ThreadCompactIconButtonSize(28, 28);

    // buildThreadButtonStyle 作用：
    // - 提供线程页按钮统一蓝色样式；
    // - 图标按钮和文字按钮共用同一套边框/悬停色逻辑。
    QString buildThreadButtonStyle(const bool iconOnlyButton)
    {
        const QString paddingText = iconOnlyButton ? QStringLiteral("4px") : QStringLiteral("4px 10px");
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: %6;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: %5;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(paddingText)
            .arg(KswordTheme::SurfaceHex());
    }

    // buildThreadSearchStyle 作用：统一线程页搜索框边框与焦点色。
    QString buildThreadSearchStyle()
    {
        return QStringLiteral(
            "QLineEdit {"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  background: %3;"
            "  color: %4;"
            "  padding: 3px 5px;"
            "}"
            "QLineEdit:focus {"
            "  border: 1px solid %1;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }
}

int ProcessDock::toThreadColumnIndex(const ThreadTableColumn column)
{
    return static_cast<int>(column);
}

void ProcessDock::initializeThreadPage()
{
    // 线程页结构：
    // 1) 顶部操作栏（刷新按钮 + 搜索框 + 状态）；
    // 2) 下方线程表格（支持排序、右键动作）。
    m_threadPage = new QWidget(this);
    m_threadPageLayout = new QVBoxLayout(m_threadPage);
    m_threadPageLayout->setContentsMargins(6, 6, 6, 6);
    m_threadPageLayout->setSpacing(6);

    // 顶部操作栏控件：全部采用图标按钮 + Tooltip。
    m_threadTopLayout = new QHBoxLayout();
    m_threadTopLayout->setContentsMargins(0, 0, 0, 0);
    m_threadTopLayout->setSpacing(8);

    m_threadRefreshButton = new QPushButton(QIcon(IconThreadRefresh), "", m_threadPage);
    m_threadRefreshButton->setIconSize(ThreadDefaultIconSize);
    m_threadRefreshButton->setFixedSize(ThreadCompactIconButtonSize);
    m_threadRefreshButton->setToolTip("刷新系统线程列表（优先 NtQuerySystemInformation）");
    m_threadRefreshButton->setStyleSheet(buildThreadButtonStyle(true));

    m_threadSearchLineEdit = new QLineEdit(m_threadPage);
    m_threadSearchLineEdit->setClearButtonEnabled(true);
    m_threadSearchLineEdit->setPlaceholderText("搜索 TID / PID / 进程名 / 状态 / 启动地址");
    m_threadSearchLineEdit->setToolTip("过滤当前线程列表，不触发新的系统查询");
    m_threadSearchLineEdit->setStyleSheet(buildThreadSearchStyle());
    m_threadSearchLineEdit->setMaximumWidth(360);

    m_threadStatusLabel = new QLabel("● 等待刷新线程列表", m_threadPage);
    m_threadStatusLabel->setToolTip("展示线程列表刷新状态与诊断路径");
    m_threadStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));

    m_threadTopLayout->addWidget(m_threadRefreshButton);
    m_threadTopLayout->addWidget(m_threadSearchLineEdit);
    m_threadTopLayout->addStretch(1);
    m_threadTopLayout->addWidget(m_threadStatusLabel);
    m_threadPageLayout->addLayout(m_threadTopLayout);

    // 线程表格初始化：使用 QTreeWidget 以支持首列图标与多列排序。
    m_threadTable = new QTreeWidget(m_threadPage);
    m_threadTable->setColumnCount(static_cast<int>(ThreadTableColumn::Count));
    m_threadTable->setHeaderLabels(ThreadTableHeaders);
    m_threadTable->setRootIsDecorated(false);
    m_threadTable->setItemsExpandable(false);
    m_threadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_threadTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_threadTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_threadTable->setUniformRowHeights(true);
    m_threadTable->setAlternatingRowColors(true);
    m_threadTable->setSortingEnabled(true);
    m_threadTable->setContextMenuPolicy(Qt::CustomContextMenu);

    QHeaderView* threadHeaderView = m_threadTable->header();
    threadHeaderView->setSectionsMovable(true);
    threadHeaderView->setStretchLastSection(false);
    threadHeaderView->setStyleSheet(QStringLiteral(
        "QHeaderView::section {"
        "  color: %1;"
        "  background: %2;"
        "  border: 1px solid %3;"
        "  padding: 4px;"
        "  font-weight: 600;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex()));

    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ThreadId), 90);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::OwnerPid), 90);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ProcessName), 240);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::StartAddress), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::Priority), 96);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::BasePriority), 96);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ThreadState), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::WaitReason), 170);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::KernelTimeMs), 110);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::UserTimeMs), 110);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::CpuTimeMs), 120);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::WaitTimeTick), 110);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ContextSwitches), 120);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::CreateTime), 170);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ProcessPath), 360);

    m_threadPageLayout->addWidget(m_threadTable, 1);
    m_sideTabWidget->addTab(m_threadPage, blueTintedIcon(IconThreadTab), "线程列表");
    refreshSideTabIconContrast();
}

void ProcessDock::initializeThreadPageConnections()
{
    // 刷新按钮：用户主动刷新线程列表，忽略监控开关。
    if (m_threadRefreshButton != nullptr)
    {
        connect(m_threadRefreshButton, &QPushButton::clicked, this, [this]() {
            requestAsyncThreadRefresh(true);
        });
    }

    // 搜索框：仅过滤当前缓存结果，不触发系统调用。
    if (m_threadSearchLineEdit != nullptr)
    {
        connect(m_threadSearchLineEdit, &QLineEdit::textChanged, this, [this](const QString&) {
            rebuildThreadTable();
        });
    }

    // 线程表右键菜单：挂起/恢复/结束线程 + 转到进程详情。
    if (m_threadTable != nullptr)
    {
        connect(m_threadTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
            showThreadTableContextMenu(localPosition);
        });
    }
}

void ProcessDock::applyThreadStatusUi(const bool refreshing, const QString& stateText)
{
    if (m_threadStatusLabel == nullptr)
    {
        return;
    }

    if (refreshing)
    {
        m_threadStatusLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }
    else
    {
        const QString idleColor = KswordTheme::IsDarkModeEnabled()
            ? QStringLiteral("#6ECF7A")
            : QStringLiteral("#2F7D32");
        m_threadStatusLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:600;").arg(idleColor));
    }
    m_threadStatusLabel->setText(stateText);
}

void ProcessDock::requestAsyncThreadRefresh(const bool forceRefresh)
{
    if (m_threadTable == nullptr)
    {
        return;
    }

    // 非强制刷新时，遵守监控开关和菜单冻结规则。
    if (!forceRefresh)
    {
        if (!m_monitoringEnabled || m_threadContextMenuVisible)
        {
            return;
        }
    }

    // 并发保护：线程刷新同一时刻只允许一个后台任务。
    if (m_threadRefreshInProgress)
    {
        return;
    }
    m_threadRefreshInProgress = true;
    const std::uint64_t localTicket = ++m_threadRefreshTicket;
    applyThreadStatusUi(true, QStringLiteral("● 正在刷新线程列表..."));

    if (m_threadRefreshButton != nullptr)
    {
        m_threadRefreshButton->setEnabled(false);
    }

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 线程列表刷新开始, ticket=" << localTicket
            << ", force=" << (forceRefresh ? "true" : "false")
            << eol;
    }

    QPointer<ProcessDock> guardThis(this);
    QRunnable* backgroundTask = QRunnable::create([guardThis, localTicket]() {
        bool usedNtQuery = false;
        std::string diagnosticText;
        std::vector<ks::process::SystemThreadRecord> threadList =
            ks::process::EnumerateSystemThreads(&usedNtQuery, &diagnosticText);

        QMetaObject::invokeMethod(guardThis, [guardThis, localTicket, usedNtQuery, diagnosticText, threadList = std::move(threadList)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            if (guardThis->m_threadRefreshTicket != localTicket)
            {
                guardThis->m_threadRefreshInProgress = false;
                if (guardThis->m_threadRefreshButton != nullptr)
                {
                    guardThis->m_threadRefreshButton->setEnabled(true);
                }
                return;
            }

            guardThis->m_threadRecordList = std::move(threadList);
            guardThis->m_threadDiagnosticText = diagnosticText;
            guardThis->rebuildThreadTable();

            const QString strategyText = usedNtQuery
                ? QStringLiteral("NtQuerySystemInformation")
                : QStringLiteral("Toolhelp Fallback");
            QString statusText = QString("● 刷新完成 | 线程:%1 | 方法:%2")
                .arg(guardThis->m_threadRecordList.size())
                .arg(strategyText);
            if (!guardThis->m_threadDiagnosticText.empty())
            {
                statusText += QString(" | %1").arg(QString::fromStdString(guardThis->m_threadDiagnosticText));
            }
            guardThis->applyThreadStatusUi(false, statusText);

            {
                kLogEvent logEvent;
                info << logEvent
                    << "[ProcessDock] 线程列表刷新完成, ticket=" << localTicket
                    << ", count=" << guardThis->m_threadRecordList.size()
                    << ", usedNtQuery=" << (usedNtQuery ? "true" : "false")
                    << ", diagnostic=" << guardThis->m_threadDiagnosticText
                    << eol;
            }

            guardThis->m_threadRefreshInProgress = false;
            if (guardThis->m_threadRefreshButton != nullptr)
            {
                guardThis->m_threadRefreshButton->setEnabled(true);
            }
        }, Qt::QueuedConnection);
    });
    backgroundTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(backgroundTask);
}

bool ProcessDock::threadRecordMatchesSearch(const ks::process::SystemThreadRecord& threadRecord) const
{
    if (m_threadSearchLineEdit == nullptr)
    {
        return true;
    }

    const QString searchText = m_threadSearchLineEdit->text().trimmed();
    if (searchText.isEmpty())
    {
        return true;
    }

    const QStringList searchableFields{
        QString::number(threadRecord.threadId),
        QString::number(threadRecord.ownerPid),
        QString::fromStdString(threadRecord.ownerProcessName),
        QString("0x%1").arg(static_cast<qulonglong>(threadRecord.startAddress), 0, 16).toUpper(),
        threadStateText(threadRecord.threadState),
        threadWaitReasonText(threadRecord.waitReason)
    };
    for (const QString& fieldText : searchableFields)
    {
        if (fieldText.contains(searchText, Qt::CaseInsensitive))
        {
            return true;
        }
    }

    return false;
}

void ProcessDock::rebuildThreadTable()
{
    if (m_threadTable == nullptr)
    {
        return;
    }

    QSignalBlocker tableSignalBlocker(m_threadTable);
    m_threadTable->setSortingEnabled(false);
    m_threadTable->clear();

    // processRecordByPid 用途：按 PID 复用进程缓存中的路径与图标信息。
    std::unordered_map<std::uint32_t, const ks::process::ProcessRecord*> processRecordByPid;
    processRecordByPid.reserve(m_cacheByIdentity.size());
    for (const auto& cachePair : m_cacheByIdentity)
    {
        const ks::process::ProcessRecord& processRecord = cachePair.second.record;
        const auto pidIt = processRecordByPid.find(processRecord.pid);
        if (pidIt == processRecordByPid.end())
        {
            processRecordByPid.emplace(processRecord.pid, &processRecord);
            continue;
        }

        // 选择 imagePath 非空的一条，提升线程页进程图标命中率。
        if (pidIt->second != nullptr &&
            pidIt->second->imagePath.empty() &&
            !processRecord.imagePath.empty())
        {
            processRecordByPid[processRecord.pid] = &processRecord;
        }
    }

    // 逐行构建线程列表：
    // - 先按搜索词过滤；
    // - 再填充文本和图标；
    // - 最后写入 TID/PID 到 UserRole，供右键动作读取。
    for (const ks::process::SystemThreadRecord& threadRecord : m_threadRecordList)
    {
        if (!threadRecordMatchesSearch(threadRecord))
        {
            continue;
        }

        QTreeWidgetItem* rowItem = new QTreeWidgetItem();
        for (int columnIndex = 0; columnIndex < static_cast<int>(ThreadTableColumn::Count); ++columnIndex)
        {
            const ThreadTableColumn column = static_cast<ThreadTableColumn>(columnIndex);
            rowItem->setText(columnIndex, formatThreadColumnText(threadRecord, column));
        }

        // 进程列图标：优先使用进程缓存中的 imagePath，保证同进程图标可复用缓存。
        ks::process::ProcessRecord iconSourceRecord{};
        const auto processIt = processRecordByPid.find(threadRecord.ownerPid);
        if (processIt != processRecordByPid.end() && processIt->second != nullptr)
        {
            iconSourceRecord = *(processIt->second);
        }
        else
        {
            iconSourceRecord.pid = threadRecord.ownerPid;
            iconSourceRecord.processName = threadRecord.ownerProcessName;
        }
        rowItem->setIcon(toThreadColumnIndex(ThreadTableColumn::ProcessName), resolveProcessIcon(iconSourceRecord));

        rowItem->setData(
            toThreadColumnIndex(ThreadTableColumn::ThreadId),
            Qt::UserRole,
            QVariant::fromValue(static_cast<qulonglong>(threadRecord.threadId)));
        rowItem->setData(
            toThreadColumnIndex(ThreadTableColumn::ThreadId),
            Qt::UserRole + 1,
            QVariant::fromValue(static_cast<qulonglong>(threadRecord.ownerPid)));

        // 已终止线程统一灰色提示，便于快速识别不可操作项。
        if (threadRecord.threadState == 4)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(ThreadTableColumn::Count); ++columnIndex)
            {
                rowItem->setForeground(columnIndex, QBrush(KswordTheme::ExitedRowForegroundColor()));
            }
        }

        m_threadTable->addTopLevelItem(rowItem);
    }

    m_threadTable->setSortingEnabled(true);
    m_threadTable->sortItems(toThreadColumnIndex(ThreadTableColumn::OwnerPid), Qt::AscendingOrder);
}

QString ProcessDock::formatThreadColumnText(
    const ks::process::SystemThreadRecord& threadRecord,
    const ThreadTableColumn column) const
{
    switch (column)
    {
    case ThreadTableColumn::ThreadId:
        return QString::number(threadRecord.threadId);
    case ThreadTableColumn::OwnerPid:
        return QString::number(threadRecord.ownerPid);
    case ThreadTableColumn::ProcessName:
        return QString::fromStdString(threadRecord.ownerProcessName.empty() ? "Unknown" : threadRecord.ownerProcessName);
    case ThreadTableColumn::StartAddress:
        return QString("0x%1").arg(static_cast<qulonglong>(threadRecord.startAddress), 0, 16).toUpper();
    case ThreadTableColumn::Priority:
        return QString::number(threadRecord.priority);
    case ThreadTableColumn::BasePriority:
        return QString::number(threadRecord.basePriority);
    case ThreadTableColumn::ThreadState:
        return threadStateText(threadRecord.threadState);
    case ThreadTableColumn::WaitReason:
        return threadWaitReasonText(threadRecord.waitReason);
    case ThreadTableColumn::KernelTimeMs:
        return QString::number(static_cast<double>(threadRecord.kernelTime100ns) / 10000.0, 'f', 2);
    case ThreadTableColumn::UserTimeMs:
        return QString::number(static_cast<double>(threadRecord.userTime100ns) / 10000.0, 'f', 2);
    case ThreadTableColumn::CpuTimeMs:
        return QString::number(
            static_cast<double>(threadRecord.kernelTime100ns + threadRecord.userTime100ns) / 10000.0,
            'f',
            2);
    case ThreadTableColumn::WaitTimeTick:
        return QString::number(threadRecord.waitTimeTick);
    case ThreadTableColumn::ContextSwitches:
        return QString::number(threadRecord.contextSwitchCount);
    case ThreadTableColumn::CreateTime:
        if (threadRecord.createTime100ns == 0)
        {
            return QStringLiteral("-");
        }
        return QString::fromStdString(ks::str::FileTime100nsToLocalText(threadRecord.createTime100ns));
    case ThreadTableColumn::ProcessPath:
    {
        for (const auto& cachePair : m_cacheByIdentity)
        {
            if (cachePair.second.record.pid == threadRecord.ownerPid &&
                !cachePair.second.record.imagePath.empty())
            {
                return QString::fromStdString(cachePair.second.record.imagePath);
            }
        }
        return QStringLiteral("-");
    }
    default:
        return QString();
    }
}

QString ProcessDock::threadStateText(const std::uint32_t stateValue) const
{
    if (stateValue == std::numeric_limits<std::uint32_t>::max())
    {
        return QStringLiteral("N/A");
    }

    switch (stateValue)
    {
    case 0: return QStringLiteral("Initialized(0)");
    case 1: return QStringLiteral("Ready(1)");
    case 2: return QStringLiteral("Running(2)");
    case 3: return QStringLiteral("Standby(3)");
    case 4: return QStringLiteral("Terminated(4)");
    case 5: return QStringLiteral("Waiting(5)");
    case 6: return QStringLiteral("Transition(6)");
    case 7: return QStringLiteral("DeferredReady(7)");
    case 8: return QStringLiteral("GateWait(8)");
    case 9: return QStringLiteral("WaitingForProcessInSwap(9)");
    default:
        return QString("Unknown(%1)").arg(stateValue);
    }
}

QString ProcessDock::threadWaitReasonText(const std::uint32_t waitReasonValue) const
{
    if (waitReasonValue == std::numeric_limits<std::uint32_t>::max())
    {
        return QStringLiteral("N/A");
    }

    switch (waitReasonValue)
    {
    case 0: return QStringLiteral("Executive(0)");
    case 1: return QStringLiteral("FreePage(1)");
    case 2: return QStringLiteral("PageIn(2)");
    case 3: return QStringLiteral("PoolAllocation(3)");
    case 4: return QStringLiteral("DelayExecution(4)");
    case 5: return QStringLiteral("Suspended(5)");
    case 6: return QStringLiteral("UserRequest(6)");
    case 7: return QStringLiteral("WrExecutive(7)");
    case 8: return QStringLiteral("WrFreePage(8)");
    case 9: return QStringLiteral("WrPageIn(9)");
    case 10: return QStringLiteral("WrPoolAllocation(10)");
    case 11: return QStringLiteral("WrDelayExecution(11)");
    case 12: return QStringLiteral("WrSuspended(12)");
    case 13: return QStringLiteral("WrUserRequest(13)");
    case 14: return QStringLiteral("WrEventPair(14)");
    case 15: return QStringLiteral("WrQueue(15)");
    case 16: return QStringLiteral("WrLpcReceive(16)");
    case 17: return QStringLiteral("WrLpcReply(17)");
    case 18: return QStringLiteral("WrVirtualMemory(18)");
    case 19: return QStringLiteral("WrPageOut(19)");
    case 20: return QStringLiteral("WrRendezvous(20)");
    case 21: return QStringLiteral("Spare2(21)");
    case 22: return QStringLiteral("Spare3(22)");
    case 23: return QStringLiteral("Spare4(23)");
    case 24: return QStringLiteral("Spare5(24)");
    case 25: return QStringLiteral("WrCalloutStack(25)");
    case 26: return QStringLiteral("WrKernel(26)");
    case 27: return QStringLiteral("WrResource(27)");
    case 28: return QStringLiteral("WrPushLock(28)");
    case 29: return QStringLiteral("WrMutex(29)");
    case 30: return QStringLiteral("WrQuantumEnd(30)");
    case 31: return QStringLiteral("WrDispatchInt(31)");
    case 32: return QStringLiteral("WrPreempted(32)");
    case 33: return QStringLiteral("WrYieldExecution(33)");
    case 34: return QStringLiteral("WrFastMutex(34)");
    case 35: return QStringLiteral("WrGuardedMutex(35)");
    case 36: return QStringLiteral("WrRundown(36)");
    case 37: return QStringLiteral("WrAlertByThreadId(37)");
    case 38: return QStringLiteral("WrDeferredPreempt(38)");
    case 39: return QStringLiteral("WrPhysicalFault(39)");
    case 40: return QStringLiteral("WrIoRing(40)");
    case 41: return QStringLiteral("WrMdlCache(41)");
    case 42: return QStringLiteral("WrRcu(42)");
    default:
        return QString("Unknown(%1)").arg(waitReasonValue);
    }
}

QString ProcessDock::buildThreadContextMenuStyle() const
{
    // 右键菜单高风险点处理：
    // - 明确写死深浅色下背景与文字颜色；
    // - 禁止依赖默认样式，避免浅色模式出现黑底黑字问题。
    const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
    const QString menuBackgroundColor = darkModeEnabled
        ? QStringLiteral("#111111")
        : QStringLiteral("#FFFFFF");
    const QString menuTextColor = darkModeEnabled
        ? QStringLiteral("#F3F3F3")
        : QStringLiteral("#111111");
    const QString menuBorderColor = darkModeEnabled
        ? QStringLiteral("#2C2C2C")
        : QStringLiteral("#D5DCE4");
    const QString menuDisabledColor = darkModeEnabled
        ? QStringLiteral("#7A7A7A")
        : QStringLiteral("#9AA3AD");

    return QStringLiteral(
        "QMenu{"
        "  background:%1;"
        "  color:%2;"
        "  border:1px solid %3;"
        "}"
        "QMenu::item{"
        "  padding:4px 16px 4px 12px;"
        "  background:transparent;"
        "}"
        "QMenu::item:selected{"
        "  background:%4;"
        "  color:#FFFFFF;"
        "}"
        "QMenu::item:disabled{"
        "  color:%5;"
        "  background:transparent;"
        "}"
        "QMenu::separator{"
        "  height:1px;"
        "  background:%3;"
        "  margin:2px 6px;"
        "}")
        .arg(menuBackgroundColor)
        .arg(menuTextColor)
        .arg(menuBorderColor)
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(menuDisabledColor);
}

void ProcessDock::bindThreadContextActionToItem(QTreeWidgetItem* clickedItem)
{
    clearThreadContextActionBinding();
    if (clickedItem == nullptr)
    {
        return;
    }

    m_threadContextActionTid = clickedItem->data(
        toThreadColumnIndex(ThreadTableColumn::ThreadId),
        Qt::UserRole).toULongLong();
    m_threadContextActionPid = clickedItem->data(
        toThreadColumnIndex(ThreadTableColumn::ThreadId),
        Qt::UserRole + 1).toULongLong();
}

void ProcessDock::clearThreadContextActionBinding()
{
    m_threadContextActionTid = 0;
    m_threadContextActionPid = 0;
    m_threadContextMenuVisible = false;
}

const ks::process::SystemThreadRecord* ProcessDock::selectedThreadRecord() const
{
    std::uint32_t targetTid = m_threadContextActionTid;
    std::uint32_t targetPid = m_threadContextActionPid;

    // 若右键绑定为空，则回退当前选中行（键盘触发时会走这里）。
    if ((targetTid == 0 || targetPid == 0) && m_threadTable != nullptr)
    {
        QTreeWidgetItem* currentItem = m_threadTable->currentItem();
        if (currentItem != nullptr)
        {
            targetTid = currentItem->data(
                toThreadColumnIndex(ThreadTableColumn::ThreadId),
                Qt::UserRole).toULongLong();
            targetPid = currentItem->data(
                toThreadColumnIndex(ThreadTableColumn::ThreadId),
                Qt::UserRole + 1).toULongLong();
        }
    }

    if (targetTid == 0 || targetPid == 0)
    {
        return nullptr;
    }

    for (const ks::process::SystemThreadRecord& threadRecord : m_threadRecordList)
    {
        if (threadRecord.threadId == targetTid && threadRecord.ownerPid == targetPid)
        {
            return &threadRecord;
        }
    }
    return nullptr;
}

void ProcessDock::showThreadTableContextMenu(const QPoint& localPosition)
{
    if (m_threadTable == nullptr)
    {
        return;
    }

    QTreeWidgetItem* clickedItem = m_threadTable->itemAt(localPosition);
    if (clickedItem == nullptr)
    {
        clearThreadContextActionBinding();
        return;
    }

    m_threadTable->setCurrentItem(clickedItem);
    const int clickedColumn = m_threadTable->columnAt(localPosition.x());
    if (clickedColumn >= 0)
    {
        m_threadTable->setCurrentItem(clickedItem, clickedColumn);
    }
    bindThreadContextActionToItem(clickedItem);

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(buildThreadContextMenuStyle());

    QAction* copyCellAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_copy_cell.svg"),
        "复制单元格");
    QAction* copyRowAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_copy_row.svg"),
        "复制行");
    contextMenu.addSeparator();
    QAction* detailAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_details.svg"),
        "转到进程详细信息");
    QAction* suspendAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_suspend.svg"),
        "挂起线程");
    QAction* resumeAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_resume.svg"),
        "恢复线程");
    QAction* terminateAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_terminate.svg"),
        "结束线程");

    m_threadContextMenuVisible = true;
    QAction* selectedAction = contextMenu.exec(m_threadTable->viewport()->mapToGlobal(localPosition));
    m_threadContextMenuVisible = false;
    if (selectedAction == nullptr)
    {
        clearThreadContextActionBinding();
        return;
    }

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 线程右键菜单执行动作: " << selectedAction->text().toStdString()
            << eol;
    }

    if (selectedAction == copyCellAction) { copyCurrentThreadCell(); }
    else if (selectedAction == copyRowAction) { copyCurrentThreadRow(); }
    else if (selectedAction == detailAction) { openThreadOwnerProcessDetails(); }
    else if (selectedAction == suspendAction) { executeSuspendThreadAction(); }
    else if (selectedAction == resumeAction) { executeResumeThreadAction(); }
    else if (selectedAction == terminateAction) { executeTerminateThreadAction(); }

    clearThreadContextActionBinding();
}

void ProcessDock::copyCurrentThreadCell()
{
    if (m_threadTable == nullptr)
    {
        return;
    }

    QTreeWidgetItem* currentItem = m_threadTable->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }

    const int currentColumn = m_threadTable->currentColumn();
    if (currentColumn < 0)
    {
        return;
    }

    QApplication::clipboard()->setText(currentItem->text(currentColumn));
}

void ProcessDock::copyCurrentThreadRow()
{
    if (m_threadTable == nullptr)
    {
        return;
    }

    QTreeWidgetItem* currentItem = m_threadTable->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }

    QStringList rowFields;
    rowFields.reserve(static_cast<int>(ThreadTableColumn::Count));
    for (int columnIndex = 0; columnIndex < static_cast<int>(ThreadTableColumn::Count); ++columnIndex)
    {
        rowFields.push_back(currentItem->text(columnIndex));
    }
    QApplication::clipboard()->setText(rowFields.join("\t"));
}

void ProcessDock::openThreadOwnerProcessDetails()
{
    const ks::process::SystemThreadRecord* threadRecord = selectedThreadRecord();
    if (threadRecord == nullptr)
    {
        return;
    }

    kLogEvent actionEvent;
    if (threadRecord->ownerPid == 0)
    {
        const std::string detailText = "当前线程没有可用的所属进程 PID。";
        warn << actionEvent << "[ProcessDock] openThreadOwnerProcessDetails: ownerPid=0" << eol;
        showActionResultMessage("转到进程详细信息", false, detailText, actionEvent);
        return;
    }

    info << actionEvent
        << "[ProcessDock] openThreadOwnerProcessDetails: pid=" << threadRecord->ownerPid
        << ", tid=" << threadRecord->threadId
        << eol;
    openProcessDetailWindowByPid(threadRecord->ownerPid);
}

void ProcessDock::executeSuspendThreadAction()
{
    const ks::process::SystemThreadRecord* threadRecord = selectedThreadRecord();
    if (threadRecord == nullptr)
    {
        return;
    }

    kLogEvent actionEvent;
    std::string detailText;
    const bool actionOk = ks::process::SuspendThreadById(threadRecord->threadId, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] executeSuspendThreadAction: tid=" << threadRecord->threadId
        << ", actionOk=" << (actionOk ? "true" : "false")
        << eol;
    showActionResultMessage("挂起线程", actionOk, detailText, actionEvent);
    requestAsyncThreadRefresh(true);
}

void ProcessDock::executeResumeThreadAction()
{
    const ks::process::SystemThreadRecord* threadRecord = selectedThreadRecord();
    if (threadRecord == nullptr)
    {
        return;
    }

    kLogEvent actionEvent;
    std::string detailText;
    const bool actionOk = ks::process::ResumeThreadById(threadRecord->threadId, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] executeResumeThreadAction: tid=" << threadRecord->threadId
        << ", actionOk=" << (actionOk ? "true" : "false")
        << eol;
    showActionResultMessage("恢复线程", actionOk, detailText, actionEvent);
    requestAsyncThreadRefresh(true);
}

void ProcessDock::executeTerminateThreadAction()
{
    const ks::process::SystemThreadRecord* threadRecord = selectedThreadRecord();
    if (threadRecord == nullptr)
    {
        return;
    }

    kLogEvent actionEvent;
    std::string detailText;
    const bool actionOk = ks::process::TerminateThreadById(threadRecord->threadId, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDock] executeTerminateThreadAction: tid=" << threadRecord->threadId
        << ", actionOk=" << (actionOk ? "true" : "false")
        << eol;
    showActionResultMessage("结束线程", actionOk, detailText, actionEvent);
    requestAsyncThreadRefresh(true);
}
