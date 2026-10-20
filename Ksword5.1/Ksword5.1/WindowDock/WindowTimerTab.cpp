#include "WindowTimerTab.h"
#include "WindowSnapshotSupport.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TableContextMenu.h"
#include "../UI/FlatButtonTheme.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

namespace
{
    enum TimerColumn : int
    {
        ColumnObject = 0,
        ColumnInterval,
        ColumnFlags,
        ColumnCallback,
        ColumnModule,
        ColumnPid,
        ColumnTid,
        ColumnPath,
        ColumnTimerId,
        ColumnWindow,
        ColumnThreadInfo,
        ColumnSession,
        ColumnCountdown,
        ColumnTolerance,
        ColumnStatus,
        ColumnCount
    };

    struct ModuleRange
    {
        std::uint64_t baseAddress = 0;
        std::uint64_t size = 0;
        QString moduleName;
        QString modulePath;
    };

    struct ProcessMetadata
    {
        QString imagePath;
        std::vector<ModuleRange> modules;
    };

    struct TimerSnapshot
    {
        QVector<QStringList> rows;
        QString statusText;
    };

    QString timerText(const char* contextKey, const QString& sourceText)
    {
        return ks::i18n::contextText(QString::fromLatin1(contextKey), sourceText);
    }

    QString hex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1").arg(value, 16, 16, QLatin1Char('0')).toUpper();
    }

    QString hex32(const std::uint32_t value)
    {
        return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
    }

    QString layoutSourceText(const std::uint32_t source, const ks::window::SnapshotTexts& texts)
    {
        switch (source)
        {
        case KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_VALIDATED_DISASSEMBLY:
            return texts.value(QStringLiteral("window.timer.layout.exact"), QStringLiteral("精确 PE 身份"));
        case KSWORD_ARK_WIN32K_TIMER_LAYOUT_SOURCE_NEAREST_PREVIOUS:
            return texts.value(QStringLiteral("window.timer.layout.previous"), QStringLiteral("最近旧版回退"));
        default:
            return texts.value(QStringLiteral("window.timer.layout.unknown"), QStringLiteral("未知"));
        }
    }

    QString intervalText(const std::uint32_t value)
    {
        if (value == 0x7FFFFFFFU)
        {
            return QStringLiteral("0x7FFFFFFF");
        }
        return QString::number(value);
    }

    ProcessMetadata queryProcessMetadata(const DWORD processId)
    {
        ProcessMetadata metadata;
        if (processId == 0U)
        {
            return metadata;
        }

        const HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (processHandle != nullptr)
        {
            std::wstring pathBuffer(32768U, L'\0');
            DWORD pathChars = static_cast<DWORD>(pathBuffer.size());
            if (::QueryFullProcessImageNameW(processHandle, 0U, pathBuffer.data(), &pathChars) != FALSE)
            {
                pathBuffer.resize(pathChars);
                metadata.imagePath = QString::fromStdWString(pathBuffer);
            }
            ::CloseHandle(processHandle);
        }

        const HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            processId);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return metadata;
        }

        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (::Module32FirstW(snapshotHandle, &moduleEntry) != FALSE)
        {
            do
            {
                ModuleRange range;
                range.baseAddress = static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(moduleEntry.modBaseAddr));
                range.size = moduleEntry.modBaseSize;
                range.moduleName = QString::fromWCharArray(moduleEntry.szModule);
                range.modulePath = QString::fromWCharArray(moduleEntry.szExePath);
                metadata.modules.push_back(std::move(range));
                moduleEntry.dwSize = sizeof(moduleEntry);
            } while (::Module32NextW(snapshotHandle, &moduleEntry) != FALSE);
        }
        ::CloseHandle(snapshotHandle);
        return metadata;
    }

    const ModuleRange* findModuleForAddress(
        const ProcessMetadata& metadata,
        const std::uint64_t address)
    {
        if (address == 0U)
        {
            return nullptr;
        }
        for (const ModuleRange& range : metadata.modules)
        {
            if (range.baseAddress <= address &&
                address - range.baseAddress < range.size)
            {
                return &range;
            }
        }
        return nullptr;
    }

    QString statusName(const std::uint32_t status, const ks::window::SnapshotTexts& texts)
    {
        switch (status)
        {
        case KSWORD_ARK_WIN32K_STATUS_OK:
            return texts.value(QStringLiteral("window.timer.row.ok"), QStringLiteral("完整"));
        case KSWORD_ARK_WIN32K_STATUS_PARTIAL:
            return texts.value(QStringLiteral("window.timer.row.partial"), QStringLiteral("部分"));
        case KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED:
            return texts.value(QStringLiteral("window.timer.row.truncated"), QStringLiteral("截断"));
        default:
            return texts.value(QStringLiteral("window.timer.row.status_code"), QStringLiteral("状态 %1")).arg(status);
        }
    }

    TimerSnapshot collectTimers(const ks::window::SnapshotTexts& texts)
    {
        TimerSnapshot snapshot;
        const ksword::ark::Win32kTimersResult result =
            ksword::ark::DriverClient().queryWin32kTimers();
        if (!result.io.ok)
        {
            snapshot.statusText = texts.value(QStringLiteral("window.timer.status.io_failed"), QStringLiteral("状态：窗口定时器查询失败，Win32=%1，%2"))
                .arg(result.io.win32Error)
                .arg(QString::fromStdString(result.io.message));
            return snapshot;
        }

        const QString detailText = QString::fromStdWString(result.detail);
        if (result.status == KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED || result.unsupported)
        {
            snapshot.statusText = texts.value(QStringLiteral("window.timer.status.unsupported"), QStringLiteral("状态：当前 win32k 版本没有精确或可用的最近旧版 tagTIMER 布局；base=%1/%2，full=%3/%4。%5"))
                .arg(hex32(result.win32kbaseTimeDateStamp))
                .arg(hex32(result.win32kbaseImageSize))
                .arg(hex32(result.win32kfullTimeDateStamp))
                .arg(hex32(result.win32kfullImageSize))
                .arg(detailText);
            return snapshot;
        }
        if (result.status == KSWORD_ARK_WIN32K_STATUS_WIN32K_NOT_FOUND)
        {
            snapshot.statusText = texts.value(QStringLiteral("window.timer.status.win32k_missing"), QStringLiteral("状态：未定位 win32kbase/win32kfull，无法读取窗口定时器。%1"))
                .arg(detailText);
            return snapshot;
        }

        std::unordered_map<DWORD, ProcessMetadata> processCache;
        snapshot.rows.reserve(static_cast<qsizetype>(result.entries.size()));
        for (const KSWORD_ARK_WIN32K_TIMER_ENTRY& entry : result.entries)
        {
            auto processIterator = processCache.find(entry.processId);
            if (processIterator == processCache.end())
            {
                processIterator = processCache.emplace(
                    entry.processId,
                    queryProcessMetadata(entry.processId)).first;
            }
            const ProcessMetadata& metadata = processIterator->second;
            const ModuleRange* module = findModuleForAddress(metadata, entry.callbackAddress);
            QString processPath = metadata.imagePath;
            if (processPath.isEmpty() && module != nullptr)
            {
                processPath = module->modulePath;
            }

            const QString entryDetail = QString::fromWCharArray(entry.detail);
            snapshot.rows.push_back(QStringList{
                hex64(entry.timerObject),
                intervalText(entry.intervalMs),
                hex32(entry.flags),
                hex64(entry.callbackAddress),
                module == nullptr ? QString() : module->moduleName,
                entry.processId == 0U ? QString() : QString::number(entry.processId),
                entry.threadId == 0U ? QString() : QString::number(entry.threadId),
                processPath,
                hex64(entry.timerId),
                hex64(entry.windowObject),
                hex64(entry.primaryThreadInfo),
                entry.sessionId == 0U ? QStringLiteral("0") : QString::number(entry.sessionId),
                intervalText(entry.countdownMs),
                intervalText(entry.toleranceMs),
                QStringLiteral("%1 | %2").arg(statusName(entry.status, texts), entryDetail) });
        }

        snapshot.statusText = texts.value(QStringLiteral("window.timer.status.completed"), QStringLiteral("状态：Timer %1/%2，访问节点 %3，读取失败 %4，损坏桶 %5，重复 %6；gTimerHashTable=%7，tagTIMER=0x%8，布局来源=%9。%10"))
            .arg(result.returnedCount)
            .arg(result.totalCount)
            .arg(result.visitedNodeCount)
            .arg(result.readFailureCount)
            .arg(result.corruptBucketCount)
            .arg(result.duplicateCount)
            .arg(hex64(result.timerHashTable))
            .arg(result.layout.objectSize, 0, 16)
            .arg(layoutSourceText(result.layout.source, texts))
            .arg(detailText);
        return snapshot;
    }

    QTableWidgetItem* readOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }
    ks::window::SnapshotTexts freezeSnapshotTexts()
    {
        return {
            {QStringLiteral("window.timer.layout.exact"), timerText("window.timer.layout.exact", QStringLiteral("精确 PE 身份"))},
            {QStringLiteral("window.timer.layout.previous"), timerText("window.timer.layout.previous", QStringLiteral("最近旧版回退"))},
            {QStringLiteral("window.timer.layout.unknown"), timerText("window.timer.layout.unknown", QStringLiteral("未知"))},
            {QStringLiteral("window.timer.row.ok"), timerText("window.timer.row.ok", QStringLiteral("完整"))},
            {QStringLiteral("window.timer.row.partial"), timerText("window.timer.row.partial", QStringLiteral("部分"))},
            {QStringLiteral("window.timer.row.truncated"), timerText("window.timer.row.truncated", QStringLiteral("截断"))},
            {QStringLiteral("window.timer.row.status_code"), timerText("window.timer.row.status_code", QStringLiteral("状态 %1"))},
            {QStringLiteral("window.timer.status.io_failed"), timerText("window.timer.status.io_failed", QStringLiteral("状态：窗口定时器查询失败，Win32=%1，%2"))},
            {QStringLiteral("window.timer.status.unsupported"), timerText("window.timer.status.unsupported", QStringLiteral("状态：当前 win32k 版本没有精确或可用的最近旧版 tagTIMER 布局；base=%1/%2，full=%3/%4。%5"))},
            {QStringLiteral("window.timer.status.win32k_missing"), timerText("window.timer.status.win32k_missing", QStringLiteral("状态：未定位 win32kbase/win32kfull，无法读取窗口定时器。%1"))},
            {QStringLiteral("window.timer.status.completed"), timerText("window.timer.status.completed", QStringLiteral("状态：Timer %1/%2，访问节点 %3，读取失败 %4，损坏桶 %5，重复 %6；gTimerHashTable=%7，tagTIMER=0x%8，布局来源=%9。%10"))} };
    }


}

WindowTimerTab::WindowTimerTab(QWidget* parent)
    : QWidget(parent)
{
    m_refreshOperation = new ks::ui::AsyncOperation(this);
    initializeUi();
    m_refreshOperation->setStateChangedCallback([this] { reconcileRefreshState(); });
}

WindowTimerTab::~WindowTimerTab()
{
    m_refreshOperation->close();
    ks::window::retireSnapshotValues(std::move(m_rows));
}

void WindowTimerTab::showEvent(QShowEvent* event)
{
    const QPointer<WindowTimerTab> alive(this);
    QWidget::showEvent(event);
    if (!alive) return;
    reconcileRefreshState();
    if (!alive) return;
    if (!m_firstRefreshStarted)
    {
        m_firstRefreshStarted = true;
        QMetaObject::invokeMethod(this, [this]() { refreshAsync(); }, Qt::QueuedConnection);
    }
}

void WindowTimerTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(5);

    auto* toolbar = new QHBoxLayout();
    m_refreshButton = new QPushButton(
        timerText("window.timer.refresh", QStringLiteral("刷新窗口定时器")),
        this);
    m_filterEdit = new QLineEdit(this);
    ks::ui::StyleSearchField(m_filterEdit);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(timerText(
        "window.timer.filter.placeholder",
        QStringLiteral("按对象、间隔、Flags、回调、模块、PID/TID 或路径筛选")));
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_filterEdit, 1);
    toolbar->setSpacing(8);
    ks::ui::NormalizeToolbarRow(toolbar);
    rootLayout->addLayout(toolbar);

    m_statusLabel = new QLabel(
        timerText("window.timer.status.waiting", QStringLiteral("状态：等待刷新")),
        this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));
    rootLayout->addWidget(m_statusLabel);

    m_table = new ks::ui::VisibleTableWidget(this);
    // 系统定时器现场可跨刷新比较，保留完整操作栏。
    ks::ui::SetTableActionBarMode(m_table, ks::ui::TableActionBarMode::Full);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        timerText("window.timer.header.object", QStringLiteral("定时器对象")),
        timerText("window.timer.header.interval", QStringLiteral("间隔时间")),
        timerText("window.timer.header.flags", QStringLiteral("Flag")),
        timerText("window.timer.header.callback", QStringLiteral("函数地址")),
        timerText("window.timer.header.module", QStringLiteral("模块名")),
        timerText("window.timer.header.pid", QStringLiteral("PID")),
        timerText("window.timer.header.tid", QStringLiteral("TID")),
        timerText("window.timer.header.path", QStringLiteral("路径")),
        timerText("window.timer.header.timer_id", QStringLiteral("Timer ID")),
        timerText("window.timer.header.window", QStringLiteral("窗口对象")),
        timerText("window.timer.header.thread_info", QStringLiteral("ThreadInfo")),
        timerText("window.timer.header.session", QStringLiteral("Session")),
        timerText("window.timer.header.countdown", QStringLiteral("剩余时间")),
        timerText("window.timer.header.tolerance", QStringLiteral("容差")),
        timerText("window.timer.header.status", QStringLiteral("状态")) });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    rootLayout->addWidget(m_table, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshAsync(); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString&) { rebuildTable(); });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) { showCopyMenu(position); });
}

void WindowTimerTab::reconcileRefreshState()
{
    if (!m_refreshing || m_refreshOperation->isBusy() || m_refreshOperation->isInFlight()) return;
    const QPointer<WindowTimerTab> alive(this);
    m_refreshing = false;
    m_refreshButton->setEnabled(true);
    if (alive && !m_refreshOperation->isBusy() && !m_refreshOperation->isInFlight()) m_statusLabel->setText(ks::i18n::sourceText(QStringLiteral("后台任务发生异常。")));
}

void WindowTimerTab::refreshAsync()
{
    const QPointer<WindowTimerTab> refreshOwner(this);
    reconcileRefreshState();
    if (!refreshOwner) return;
    if (m_refreshing)
    {
        return;
    }
    const QPointer<WindowTimerTab> alive(this);
    m_refreshing = true;
    m_firstRefreshStarted = true;
    m_refreshButton->setEnabled(false);
    if (!alive) return;
    m_statusLabel->setText(timerText(
        "window.timer.status.refreshing",
        QStringLiteral("状态：正在通过 R0 读取 gTimerHashTable...")));
    if (!alive) return;
    m_refreshOperation->submit<ks::window::SnapshotTexts, std::shared_ptr<const TimerSnapshot>>(
        freezeSnapshotTexts(),
        [](const ks::window::SnapshotTexts& texts, const ks::ui::AsyncOperationToken&) {
            return ks::window::makeSnapshotValue<TimerSnapshot>(collectTimers(texts));
        },
        [this](const std::shared_ptr<const TimerSnapshot>& snapshot, std::uint64_t ticket) {
            return applySnapshot(snapshot->rows, snapshot->statusText, ticket);
        },
        [this](std::exception_ptr) {
            const QPointer<WindowTimerTab> alive(this);
            const auto failedGeneration = m_refreshOperation->generation();
            m_refreshing = false;
            m_refreshButton->setEnabled(true);
            if (alive && m_refreshOperation->isCurrent(failedGeneration)) m_statusLabel->setText(ks::i18n::sourceText(QStringLiteral("后台任务发生异常。")));
        });
}

bool WindowTimerTab::applySnapshot(QVector<QStringList> rows, const QString& statusText, std::uint64_t ticket)
{
    if (!m_refreshOperation->isCurrent(ticket)) return true;
    // 后台快照到达时先保护缓存和表格；QVector/QString 为隐式共享，
    // latest-wins 延迟不会为未打开菜单的常规路径制造深拷贝。
    const QPointer<WindowTimerTab> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("window-timer-snapshot-apply"),
        { m_table },
        [safeThis, rows, statusText, ticket]() mutable
        {
            if (!safeThis || !safeThis->m_refreshOperation->isCurrent(ticket)) return;
            if (safeThis->applySnapshot(std::move(rows), statusText, ticket) && safeThis)
                safeThis->m_refreshOperation->completeDeferred(ticket);
        }))
    {
        return false;
    }
    if (!safeThis || !safeThis->m_refreshOperation->isCurrent(ticket)) return true;

    m_refreshing = false;
    m_refreshButton->setEnabled(true);
    if (!safeThis || !safeThis->m_refreshOperation->isCurrent(ticket)) return true;
    ks::window::retireSnapshotValues(std::move(m_rows));
    m_rows = std::move(rows);
    m_statusLabel->setText(statusText);
    if (!safeThis || !safeThis->m_refreshOperation->isCurrent(ticket)) return true;
    rebuildTable();
    return true;
}

void WindowTimerTab::rebuildTable()
{
    const QPointer<WindowTimerTab> alive(this);
    const QString keyword = m_filterEdit->text().trimmed();
    m_table->setSortingEnabled(false);
    if (!alive) return;
    m_table->setRowCount(0);
    if (!alive) return;
    for (const QStringList& sourceRow : m_rows)
    {
        if (sourceRow.size() < ColumnCount)
        {
            continue;
        }
        if (!keyword.isEmpty() && !sourceRow.join(QLatin1Char(' ')).contains(keyword, Qt::CaseInsensitive))
        {
            continue;
        }
        const int tableRow = m_table->rowCount();
        m_table->insertRow(tableRow);
        if (!alive) return;
        for (int column = 0; column < ColumnCount; ++column)
        {
            m_table->setItem(tableRow, column, readOnlyItem(sourceRow.at(column)));
            if (!alive) return;
        }
    }
    m_table->setSortingEnabled(true);
    if (!alive) return;
    m_table->resizeColumnsToContents();
}

QString WindowTimerTab::rowClipboardText(QTableWidget* table, const int row, const bool includeHeader)
{
    if (table == nullptr || row < 0 || row >= table->rowCount())
    {
        return {};
    }
    QStringList lines;
    if (includeHeader)
    {
        QStringList headers;
        for (int column = 0; column < table->columnCount(); ++column)
        {
            headers << (table->horizontalHeaderItem(column) == nullptr ? QString() : table->horizontalHeaderItem(column)->text());
        }
        lines << headers.join(QLatin1Char('\t'));
    }
    QStringList values;
    for (int column = 0; column < table->columnCount(); ++column)
    {
        values << (table->item(row, column) == nullptr ? QString() : table->item(row, column)->text());
    }
    lines << values.join(QLatin1Char('\t'));
    return lines.join(QLatin1Char('\n'));
}

void WindowTimerTab::showCopyMenu(const QPoint& position)
{
    const QModelIndex index = m_table->indexAt(position);
    const int row = index.isValid() ? index.row() : m_table->currentRow();
    QMenu menu(this);
    QAction* copyCell = menu.addAction(timerText("window.timer.copy.cell", QStringLiteral("复制单元格")));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyCell, ks::ui::TableContextActionRole::CopyCell);
    QAction* copyRow = menu.addAction(timerText("window.timer.copy.row", QStringLiteral("复制当前行")));
    // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
    ks::ui::SetTableContextActionRole(copyRow, ks::ui::TableContextActionRole::CopyRows);
    QAction* copyAll = menu.addAction(timerText("window.timer.copy.all", QStringLiteral("复制全部行")));
    const QTableWidgetItem* processIdItem = row >= 0 ? m_table->item(row, ColumnPid) : nullptr;
    bool processIdOk = false;
    const quint32 processId = processIdItem != nullptr
        ? processIdItem->text().trimmed().toUInt(&processIdOk, 10)
        : 0U;
    QAction* openProcessAction = menu.addAction(
        QIcon(QStringLiteral(":/Icon/process_details.svg")),
        QStringLiteral("转到进程详细信息"));
    copyCell->setEnabled(index.isValid());
    copyRow->setEnabled(row >= 0);
    copyAll->setEnabled(m_table->rowCount() > 0);
    openProcessAction->setEnabled(processIdOk && processId != 0U);
    QAction* selected = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (selected == copyCell && index.isValid())
    {
        const QTableWidgetItem* item = m_table->item(index.row(), index.column());
        QApplication::clipboard()->setText(item == nullptr ? QString() : item->text());
    }
    else if (selected == copyRow)
    {
        QApplication::clipboard()->setText(rowClipboardText(m_table, row, true));
    }
    else if (selected == copyAll)
    {
        QStringList lines;
        for (int tableRow = 0; tableRow < m_table->rowCount(); ++tableRow)
        {
            lines << rowClipboardText(m_table, tableRow, tableRow == 0);
        }
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    }
    else if (selected == openProcessAction)
    {
        ks::ui::OpenProcessDetailByPid(processId);
    }
}
