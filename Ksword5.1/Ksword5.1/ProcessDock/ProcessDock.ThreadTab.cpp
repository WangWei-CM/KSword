#include "ProcessDock.h"
#include "ThreadStackWindow.h"

#include "../ArkDriverClient/ArkDriverClient.h"
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
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace
{
    // 线程页表头：强调线程基础身份、调度状态与时间统计。
    const QStringList ThreadTableHeaders{
        "TID",
        "PID",
        "进程",
        "启动地址",
        "Win32Start",
        "TEB",
        "UserStackBase",
        "UserStackLimit",
        "KernelStack",
        "KStackBase",
        "KStackLimit",
        "InitialStack",
        "ReadOps",
        "WriteOps",
        "OtherOps",
        "ReadBytes",
        "WriteBytes",
        "OtherBytes",
        "R0状态",
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

    // ThreadIdentityKey：按 PID/TID 合并 R3 基础线程与 R0 KTHREAD 扩展。
    struct ThreadIdentityKey
    {
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;

        bool operator==(const ThreadIdentityKey& other) const noexcept
        {
            return processId == other.processId && threadId == other.threadId;
        }
    };

    // ThreadIdentityKeyHash：为 unordered_map 提供轻量哈希。
    struct ThreadIdentityKeyHash
    {
        std::size_t operator()(const ThreadIdentityKey& key) const noexcept
        {
            const std::uint64_t combined =
                (static_cast<std::uint64_t>(key.processId) << 32U) |
                static_cast<std::uint64_t>(key.threadId);
            return std::hash<std::uint64_t>{}(combined);
        }
    };

    // hexPointerText 作用：统一格式化地址；不可用地址按调用方要求显示 Unavailable。
    QString hexPointerText(const std::uint64_t addressValue, const bool zeroAsUnavailable)
    {
        if (zeroAsUnavailable && addressValue == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 0, 16)
            .toUpper();
    }

    // threadR0StatusText 作用：把共享协议状态转换为线程表短文本。
    QString threadR0StatusText(const std::uint32_t statusValue)
    {
        switch (statusValue)
        {
        case KSWORD_ARK_THREAD_R0_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_THREAD_R0_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_THREAD_R0_STATUS_DYNDATA_MISSING:
            return QStringLiteral("DynData missing");
        case KSWORD_ARK_THREAD_R0_STATUS_READ_FAILED:
            return QStringLiteral("Read failed");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // threadR0DiagnosticText 作用：
    // - 选中线程或悬停 R0 状态列时展示字段来源与原始偏移；
    // - Phase 3 暂不新增详情页，先用状态栏/tooltip 暴露诊断信息。
    QString threadR0DiagnosticText(const ks::process::SystemThreadRecord& threadRecord)
    {
        auto offsetText = [](const std::uint32_t offsetValue) -> QString {
            if (offsetValue == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE)
            {
                return QStringLiteral("Unavailable");
            }
            return QStringLiteral("0x%1").arg(offsetValue, 0, 16).toUpper();
        };

        QStringList offsetParts;
        offsetParts
            << QStringLiteral("Initial=%1").arg(offsetText(threadRecord.r0KtInitialStackOffset))
            << QStringLiteral("StackLimit=%1").arg(offsetText(threadRecord.r0KtStackLimitOffset))
            << QStringLiteral("StackBase=%1").arg(offsetText(threadRecord.r0KtStackBaseOffset))
            << QStringLiteral("KernelStack=%1").arg(offsetText(threadRecord.r0KtKernelStackOffset))
            << QStringLiteral("ReadOps=%1").arg(offsetText(threadRecord.r0KtReadOperationCountOffset))
            << QStringLiteral("WriteOps=%1").arg(offsetText(threadRecord.r0KtWriteOperationCountOffset))
            << QStringLiteral("OtherOps=%1").arg(offsetText(threadRecord.r0KtOtherOperationCountOffset))
            << QStringLiteral("ReadBytes=%1").arg(offsetText(threadRecord.r0KtReadTransferCountOffset))
            << QStringLiteral("WriteBytes=%1").arg(offsetText(threadRecord.r0KtWriteTransferCountOffset))
            << QStringLiteral("OtherBytes=%1").arg(offsetText(threadRecord.r0KtOtherTransferCountOffset));

        return QStringLiteral("R0=%1 | StackSource=%2 | IoSource=%3 | Cap=0x%4 | Offsets: %5")
            .arg(threadR0StatusText(threadRecord.r0ThreadStatus))
            .arg(threadRecord.r0StackFieldSource)
            .arg(threadRecord.r0IoFieldSource)
            .arg(static_cast<qulonglong>(threadRecord.r0ThreadDynDataCapabilityMask), 0, 16)
            .arg(offsetParts.join(QStringLiteral(", ")));
    }

    // mergeThreadR0Entry 作用：把 ArkDriverClient 解析后的 R0 行覆盖到 R3 线程记录中。
    void mergeThreadR0Entry(
        ks::process::SystemThreadRecord& threadRecord,
        const ksword::ark::ThreadEntry& r0Entry)
    {
        threadRecord.r0ThreadFieldFlags = r0Entry.fieldFlags;
        threadRecord.r0ThreadStatus = r0Entry.r0Status;
        threadRecord.r0StackFieldSource = r0Entry.stackFieldSource;
        threadRecord.r0IoFieldSource = r0Entry.ioFieldSource;
        threadRecord.r0InitialStack = r0Entry.initialStack;
        threadRecord.r0StackLimit = r0Entry.stackLimit;
        threadRecord.r0StackBase = r0Entry.stackBase;
        threadRecord.r0KernelStack = r0Entry.kernelStack;
        threadRecord.r0ReadOperationCount = r0Entry.readOperationCount;
        threadRecord.r0WriteOperationCount = r0Entry.writeOperationCount;
        threadRecord.r0OtherOperationCount = r0Entry.otherOperationCount;
        threadRecord.r0ReadTransferCount = r0Entry.readTransferCount;
        threadRecord.r0WriteTransferCount = r0Entry.writeTransferCount;
        threadRecord.r0OtherTransferCount = r0Entry.otherTransferCount;
        threadRecord.r0KtInitialStackOffset = r0Entry.ktInitialStackOffset;
        threadRecord.r0KtStackLimitOffset = r0Entry.ktStackLimitOffset;
        threadRecord.r0KtStackBaseOffset = r0Entry.ktStackBaseOffset;
        threadRecord.r0KtKernelStackOffset = r0Entry.ktKernelStackOffset;
        threadRecord.r0KtReadOperationCountOffset = r0Entry.ktReadOperationCountOffset;
        threadRecord.r0KtWriteOperationCountOffset = r0Entry.ktWriteOperationCountOffset;
        threadRecord.r0KtOtherOperationCountOffset = r0Entry.ktOtherOperationCountOffset;
        threadRecord.r0KtReadTransferCountOffset = r0Entry.ktReadTransferCountOffset;
        threadRecord.r0KtWriteTransferCountOffset = r0Entry.ktWriteTransferCountOffset;
        threadRecord.r0KtOtherTransferCountOffset = r0Entry.ktOtherTransferCountOffset;
        threadRecord.r0ThreadDynDataCapabilityMask = r0Entry.dynDataCapabilityMask;
    }

    // mergeThreadR0Snapshot 作用：
    // - R3 基础线程列表一定先保留；
    // - 驱动缺失或 IOCTL 失败时仅追加诊断，不清空 R3 结果；
    // - 成功时按 PID/TID 合并 KTHREAD 栈字段和 I/O counter。
    bool mergeThreadR0Snapshot(
        std::vector<ks::process::SystemThreadRecord>& threadList,
        std::string* diagnosticTextOut)
    {
        const ksword::ark::DriverClient driverClient;
        const ksword::ark::ThreadEnumResult r0Result =
            driverClient.enumerateThreads(KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL);
        if (!r0Result.io.ok)
        {
            if (diagnosticTextOut != nullptr)
            {
                if (!diagnosticTextOut->empty())
                {
                    *diagnosticTextOut += " | ";
                }
                *diagnosticTextOut += "R0 thread extension unavailable: " + r0Result.io.message;
            }
            return false;
        }

        std::unordered_map<ThreadIdentityKey, const ksword::ark::ThreadEntry*, ThreadIdentityKeyHash> r0ByThread;
        r0ByThread.reserve(r0Result.entries.size());
        for (const ksword::ark::ThreadEntry& r0Entry : r0Result.entries)
        {
            if (r0Entry.threadId == 0U)
            {
                continue;
            }
            r0ByThread.insert_or_assign(
                ThreadIdentityKey{ r0Entry.processId, r0Entry.threadId },
                &r0Entry);
        }

        std::size_t mergedCount = 0;
        for (ks::process::SystemThreadRecord& threadRecord : threadList)
        {
            const auto r0It = r0ByThread.find(ThreadIdentityKey{ threadRecord.ownerPid, threadRecord.threadId });
            if (r0It == r0ByThread.end() || r0It->second == nullptr)
            {
                continue;
            }
            mergeThreadR0Entry(threadRecord, *(r0It->second));
            ++mergedCount;
        }

        if (diagnosticTextOut != nullptr)
        {
            if (!diagnosticTextOut->empty())
            {
                *diagnosticTextOut += " | ";
            }
            std::ostringstream stream;
            stream << "R0 thread extension merged: " << mergedCount
                << "/" << threadList.size()
                << ", " << r0Result.io.message;
            *diagnosticTextOut += stream.str();
        }
        return true;
    }

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
            .arg(KswordTheme::PrimaryBlueSolidHoverHex())
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
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::Win32StartAddress), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::TebBaseAddress), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::UserStackBase), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::UserStackLimit), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::KernelStack), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::KStackBase), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::KStackLimit), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::InitialStack), 140);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ReadOps), 100);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::WriteOps), 100);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::OtherOps), 100);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ReadBytes), 120);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::WriteBytes), 120);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::OtherBytes), 120);
    m_threadTable->setColumnWidth(toThreadColumnIndex(ThreadTableColumn::ThreadR0Status), 130);
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
        const bool r0ThreadExtensionMerged = mergeThreadR0Snapshot(threadList, &diagnosticText);

        QMetaObject::invokeMethod(guardThis, [guardThis, localTicket, usedNtQuery, r0ThreadExtensionMerged, diagnosticText, threadList = std::move(threadList)]() mutable {
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
            statusText += r0ThreadExtensionMerged
                ? QStringLiteral(" | R0扩展:已合并")
                : QStringLiteral(" | R0扩展:Unavailable");
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
                    << ", r0ThreadExtensionMerged=" << (r0ThreadExtensionMerged ? "true" : "false")
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
        QString("0x%1").arg(static_cast<qulonglong>(threadRecord.win32StartAddress), 0, 16).toUpper(),
        QString("0x%1").arg(static_cast<qulonglong>(threadRecord.tebBaseAddress), 0, 16).toUpper(),
        QString("0x%1").arg(static_cast<qulonglong>(threadRecord.r0KernelStack), 0, 16).toUpper(),
        threadStateText(threadRecord.threadState),
        threadWaitReasonText(threadRecord.waitReason),
        threadR0StatusText(threadRecord.r0ThreadStatus)
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
        rowItem->setToolTip(
            toThreadColumnIndex(ThreadTableColumn::ThreadR0Status),
            threadR0DiagnosticText(threadRecord));

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
        return hexPointerText(threadRecord.startAddress, false);
    case ThreadTableColumn::Win32StartAddress:
        return hexPointerText(threadRecord.win32StartAddress, true);
    case ThreadTableColumn::TebBaseAddress:
        return hexPointerText(threadRecord.tebBaseAddress, true);
    case ThreadTableColumn::UserStackBase:
        return hexPointerText(threadRecord.stackBase, true);
    case ThreadTableColumn::UserStackLimit:
        return hexPointerText(threadRecord.stackLimit, true);
    case ThreadTableColumn::KernelStack:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_KERNEL_STACK_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return hexPointerText(threadRecord.r0KernelStack, true);
    case ThreadTableColumn::KStackBase:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_STACK_BASE_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return hexPointerText(threadRecord.r0StackBase, true);
    case ThreadTableColumn::KStackLimit:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_STACK_LIMIT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return hexPointerText(threadRecord.r0StackLimit, true);
    case ThreadTableColumn::InitialStack:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_INITIAL_STACK_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return hexPointerText(threadRecord.r0InitialStack, true);
    case ThreadTableColumn::ReadOps:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_READ_OPERATION_COUNT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QString::number(static_cast<qulonglong>(threadRecord.r0ReadOperationCount));
    case ThreadTableColumn::WriteOps:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_WRITE_OPERATION_COUNT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QString::number(static_cast<qulonglong>(threadRecord.r0WriteOperationCount));
    case ThreadTableColumn::OtherOps:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_OTHER_OPERATION_COUNT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QString::number(static_cast<qulonglong>(threadRecord.r0OtherOperationCount));
    case ThreadTableColumn::ReadBytes:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_READ_TRANSFER_COUNT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QString::number(static_cast<qulonglong>(threadRecord.r0ReadTransferCount));
    case ThreadTableColumn::WriteBytes:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_WRITE_TRANSFER_COUNT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QString::number(static_cast<qulonglong>(threadRecord.r0WriteTransferCount));
    case ThreadTableColumn::OtherBytes:
        if ((threadRecord.r0ThreadFieldFlags & KSWORD_ARK_THREAD_FIELD_OTHER_TRANSFER_COUNT_PRESENT) == 0U)
        {
            return QStringLiteral("Unavailable");
        }
        return QString::number(static_cast<qulonglong>(threadRecord.r0OtherTransferCount));
    case ThreadTableColumn::ThreadR0Status:
        return threadR0StatusText(threadRecord.r0ThreadStatus);
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
    const QString menuBackgroundColor = KswordTheme::SurfaceColorHex();
    const QString menuTextColor = KswordTheme::TextPrimaryColorHex();
    const QString menuBorderColor = KswordTheme::BorderColorHex();
    const QString menuDisabledColor = KswordTheme::TextDisabledColorHex();

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
    QAction* stackAction = contextMenu.addAction(
        blueTintedIcon(":/Icon/process_threads.svg"),
        "查看调用栈");
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
    else if (selectedAction == stackAction) { openThreadStackWindow(); }
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

void ProcessDock::openThreadStackWindow()
{
    // Phase-8 调用栈入口：
    // - 从当前线程行构造 ThreadStackTarget；
    // - 用户态调用栈由 ThreadStackWindow 捕获；
    // - R0 KTHREAD 字段只作为内核栈边界诊断展示。
    const ks::process::SystemThreadRecord* threadRecord = selectedThreadRecord();
    if (threadRecord == nullptr)
    {
        return;
    }

    ThreadStackTarget target{};
    target.processId = threadRecord->ownerPid;
    target.threadId = threadRecord->threadId;
    target.processName = QString::fromStdString(threadRecord->ownerProcessName);
    target.startAddress = threadRecord->startAddress;
    target.win32StartAddress = threadRecord->win32StartAddress;
    target.tebBaseAddress = threadRecord->tebBaseAddress;
    target.userStackBase = threadRecord->stackBase;
    target.userStackLimit = threadRecord->stackLimit;
    target.r0KernelStack = threadRecord->r0KernelStack;
    target.r0StackBase = threadRecord->r0StackBase;
    target.r0StackLimit = threadRecord->r0StackLimit;
    target.r0InitialStack = threadRecord->r0InitialStack;
    target.r0ThreadStatus = threadRecord->r0ThreadStatus;
    target.r0CapabilityMask = threadRecord->r0ThreadDynDataCapabilityMask;

    for (const auto& cachePair : m_cacheByIdentity)
    {
        if (cachePair.second.record.pid == threadRecord->ownerPid &&
            !cachePair.second.record.imagePath.empty())
        {
            target.processPath = QString::fromStdString(cachePair.second.record.imagePath);
            break;
        }
    }

    auto* stackWindow = new ThreadStackWindow(target, this);
    stackWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    stackWindow->show();
    stackWindow->raise();
    stackWindow->activateWindow();

    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDock] openThreadStackWindow: pid=" << target.processId
        << ", tid=" << target.threadId
        << eol;
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
