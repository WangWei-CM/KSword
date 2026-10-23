#include "HvmEventDialog.h"
#include "./SecondaryPageLayout.h"
#include "./ToolbarMetrics.h"
#include "./VisibleTableWidget.h"

#include "HvmControl.h"
#include "../Internationalization/LanguageManager.h"

#include <QCheckBox>
#include <QEvent>
#include <QShowEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <thread>

namespace
{
    // 轮询间隔：足够跟上人眼，又不至于把驱动侧状态锁打满。
    constexpr int kPollIntervalMs = 500;
    // 表格行数上限：长时间观察时裁掉最旧的行。
    constexpr int kMaxTableRows = 2000;

    // describeEventType：把事件类型翻译成一句话。
    QString describeEventType(const unsigned long type)
    {
        switch (type)
        {
        case KSWORD_ARK_HVM_EVENT_TYPE_VMEXIT:
            return ks::i18n::sourceText(QStringLiteral("VM-exit"));
        case KSWORD_ARK_HVM_EVENT_TYPE_EPT_VIOLATION:
            return ks::i18n::sourceText(QStringLiteral("EPT 违例"));
        case KSWORD_ARK_HVM_EVENT_TYPE_NESTED_VMX:
            return ks::i18n::sourceText(QStringLiteral("嵌套 VMX"));
        case KSWORD_ARK_HVM_EVENT_TYPE_FATAL_EXIT:
            return ks::i18n::sourceText(QStringLiteral("致命退出"));
        case KSWORD_ARK_HVM_EVENT_TYPE_LIFECYCLE:
            return ks::i18n::sourceText(QStringLiteral("生命周期"));
        default:
            break;
        }
        return ks::i18n::sourceText(QStringLiteral("未知类型"));
    }

    // describeAccess：把 EPT 访问类型翻译成 RWX 简写。
    QString describeAccess(const unsigned long access)
    {
        QString text;
        if ((access & KSWORD_ARK_HVM_EPT_ACCESS_READ) != 0)
        {
            text += QLatin1Char('R');
        }
        if ((access & KSWORD_ARK_HVM_EPT_ACCESS_WRITE) != 0)
        {
            text += QLatin1Char('W');
        }
        if ((access & KSWORD_ARK_HVM_EPT_ACCESS_EXECUTE) != 0)
        {
            text += QLatin1Char('X');
        }
        return text;
    }
}

HvmEventDialog::HvmEventDialog(QWidget* const parent)
    : QDialog(parent)
{
    setWindowTitle(ks::i18n::sourceText(QStringLiteral("HVM 事件流")));
    setObjectName(QStringLiteral("HvmEventDialog"));
    buildUi();
    // 打开时立刻取一次，不必等第一个轮询周期。
    poll();
}

void HvmEventDialog::buildUi()
{
    // 事件流属于持续采集工具窗，套用共享二级窗口底面与内容排版。
    ks::ui::StyleSecondaryWindow(this);
    QVBoxLayout* const rootLayout = new QVBoxLayout(this);
    ks::ui::StyleSecondaryContentLayout(rootLayout);

    QLabel* const hintLabel = new QLabel(
        ks::i18n::sourceText(QStringLiteral("事件环是有界的：丢弃计数非零说明消费跟不上产生速度，中间有缺口。")),
        this);
    hintLabel->setWordWrap(true);
    rootLayout->addWidget(hintLabel);

    m_eventTable = new QTableWidget(0, 8, this);
    // 事件本身已经是时间序列，保留复制/导出即可，不额外占用快照对比工具条。
    ks::ui::SetTableActionBarMode(m_eventTable, ks::ui::TableActionBarMode::Compact);
    m_eventTable->setHorizontalHeaderLabels(QStringList()
        << ks::i18n::sourceText(QStringLiteral("序号"))
        << ks::i18n::sourceText(QStringLiteral("类型"))
        << ks::i18n::sourceText(QStringLiteral("退出原因"))
        << ks::i18n::sourceText(QStringLiteral("访问"))
        << ks::i18n::sourceText(QStringLiteral("物理地址"))
        << ks::i18n::sourceText(QStringLiteral("Guest RIP"))
        << ks::i18n::sourceText(QStringLiteral("规则/视图"))
        << ks::i18n::sourceText(QStringLiteral("处理器")));
    m_eventTable->horizontalHeader()->setStretchLastSection(true);
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rootLayout->addWidget(m_eventTable, 1);

    QGridLayout* const buttonLayout = new QGridLayout();
    m_followCheck = new QCheckBox(
        ks::i18n::sourceText(QStringLiteral("自动滚动到最新")), this);
    m_followCheck->setChecked(true);
    m_pauseButton = new QPushButton(
        ks::i18n::sourceText(QStringLiteral("暂停")), this);
    m_clearButton = new QPushButton(
        ks::i18n::sourceText(QStringLiteral("清空")), this);
    buttonLayout->addWidget(m_followCheck, 0, 0);
    buttonLayout->addWidget(m_pauseButton, 0, 1);
    buttonLayout->addWidget(m_clearButton, 0, 2);
    buttonLayout->setHorizontalSpacing(8);
    ks::ui::NormalizeToolbarControl(m_pauseButton);
    ks::ui::NormalizeToolbarControl(m_clearButton);
    rootLayout->addLayout(buttonLayout);

    m_statusLabel = new QLabel(QString(), this);
    m_statusLabel->setWordWrap(true);
    rootLayout->addWidget(m_statusLabel);

    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        m_paused = !m_paused;
        m_pauseButton->setText(m_paused
            ? ks::i18n::sourceText(QStringLiteral("继续"))
            : ks::i18n::sourceText(QStringLiteral("暂停")));
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        // 只清界面，不清驱动侧环：环里的历史对别的消费者仍然有意义。
        m_events.clear();
        m_presented.clear();
        m_eventTable->setRowCount(0);
        m_droppedTotal = 0;
    });

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        poll();
    });
    m_pollTimer->start();
    resize(900, 560);
}

void HvmEventDialog::trimRows()
{
    while (m_events.size() > static_cast<std::size_t>(kMaxTableRows)) m_events.pop_front();
}

void HvmEventDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    presentEvents();
}

void HvmEventDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) presentEvents();
}

void HvmEventDialog::presentEvents()
{
    if (!m_projectionDirty || !isVisible() || isMinimized()) return;
    m_projectionDirty = false;
    QTableWidget* const table = m_eventTable;
    int removed = 0;
    while (!m_presented.empty() && (m_events.empty() || m_presented.front() < m_events.front().sequence))
    {
        m_presented.pop_front();
        ++removed;
    }
    // Preserve the original table/model and surviving items/selections. One
    // model notification removes the whole expired prefix, including catch-up.
    if (removed) table->model()->removeRows(0, removed);
    const int existing = static_cast<int>(m_presented.size());
    const int added = static_cast<int>(m_events.size()) - existing;
    if (added > 0) table->model()->insertRows(existing, added);
    for (int row = existing; row < static_cast<int>(m_events.size()); ++row)
    {
        const auto& entry = m_events[static_cast<std::size_t>(row)];
        table->setItem(row, 0, new QTableWidgetItem(QString::number(entry.sequence)));
        table->setItem(row, 1, new QTableWidgetItem(describeEventType(entry.type)));
        table->setItem(row, 2, new QTableWidgetItem(QString::number(entry.exitReason)));
        table->setItem(row, 3, new QTableWidgetItem(describeAccess(entry.access)));
        table->setItem(row, 4, new QTableWidgetItem(QStringLiteral("0x%1").arg(entry.guestPhysicalAddress, 0, 16)));
        table->setItem(row, 5, new QTableWidgetItem(QStringLiteral("0x%1").arg(entry.guestRip, 0, 16)));
        table->setItem(row, 6, new QTableWidgetItem(entry.ruleId ? QString::number(entry.ruleId) : QString()));
        table->setItem(row, 7, new QTableWidgetItem(QStringLiteral("%1:%2").arg(entry.processorGroup).arg(entry.processorNumber)));
        m_presented.push_back(entry.sequence);
    }
    // A state-only poll never drags a reader back to the tail.
    if (added > 0 && m_followCheck->isChecked()) table->scrollToBottom();
    if (m_statusLabel->text() != m_latestStatus) m_statusLabel->setText(m_latestStatus);
}

void HvmEventDialog::poll()
{
    // 暂停时不发 IOCTL，也不推进游标——继续后能接着看未读的事件。
    if (m_paused || m_pollInFlight)
    {
        return;
    }
    m_pollInFlight = true;
    QPointer<HvmEventDialog> safeThis(this);
    const unsigned long long afterSequence = m_afterSequence;
    std::thread([safeThis, afterSequence]() {
        const ksword::hvm::HvmEventResult result =
            ksword::hvm::readEvents(afterSequence, false);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_pollInFlight = false;
                if (!result.ok)
                {
                    safeThis->m_latestStatus = result.message;
                    safeThis->m_projectionDirty = true;
                    safeThis->presentEvents();
                    return;
                }
                // Canonical bounded history and cursor advance even when the
                // window is hidden/minimized. Presentation owns no collection.
                for (const auto& entry : result.events)
                {
                    if (entry.sequence <= safeThis->m_afterSequence) continue;
                    safeThis->m_events.push_back(entry);
                    safeThis->m_afterSequence = entry.sequence;
                }
                safeThis->m_droppedTotal += result.droppedRows;
                safeThis->trimRows();
                safeThis->m_latestStatus =
                    ks::i18n::sourceText(QStringLiteral("已显示 %1 行，环内待取 %2 行，累计丢弃 %3 行。"))
                        .arg(safeThis->m_events.size()).arg(result.availableRows).arg(safeThis->m_droppedTotal);
                safeThis->m_projectionDirty = true;
                safeThis->presentEvents();
            },
            Qt::QueuedConnection);
    }).detach();
}
