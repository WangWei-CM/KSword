#include "DdmaPage.h"
#include "DdmaPhysicalWriteTransaction.h"
#include "../ksword/log/log.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ParameterWorkspace.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/PerfTrace.h"

#include "../theme.h"
#include "../UI/AdaptivePageScroll.h"
#include "../UI/StructuredFieldView.h"
#include "../UI/CodeTextEdit.h"
#include "../UI/MemoryWorkbench/HexView.h"
#include "../UI/MemoryWorkbench/SnapshotWorkbenchWidget.h"
#include "../UI/VisibleTableWidget.h"

#include <QCheckBox>
#include <QEvent>
#include <QFile>
#include <QFutureWatcher>
#include <QPromise>
#include <QPointer>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <memory>
#include <exception>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>

// ============================================================
// DdmaPage.cpp
// 作用：
// - 实现 DDMA 通道的配置、自身读写与"标准通道 vs DDMA"同址复核。
// ============================================================

namespace
{
    // kDiskTableColumns：磁盘表列定义。
    enum class DiskColumn : int
    {
        Index = 0,
        DeviceName,
        State,
        ProbeStatus,
        SectorSize,
        Count
    };

    // kCompareSampleRows：复核差异明细最多展示多少行，避免整页 4096 个差异
    // 把表格撑爆。超出部分只在结论里给总数。
    constexpr int kCompareSampleRows = 256;

    bool sameDdmaSession(const ksword::memory_backend::DdmaSession& left,
                         const ksword::memory_backend::DdmaSession& right)
    {
        return left.configured == right.configured && left.diskIndex == right.diskIndex
            && left.deviceName == right.deviceName && left.scratchLba == right.scratchLba
            && left.scratchLbaValid == right.scratchLbaValid
            && left.scratchAcknowledged == right.scratchAcknowledged
            && left.kernelDebuggerEnabled == right.kernelDebuggerEnabled
            && left.transferBytes == right.transferBytes
            && left.scratchSectorCount == right.scratchSectorCount;
    }

    QString ddmaSnapshotIdentity(const std::uint64_t generation,
                                 const ksword::memory_backend::DdmaSession& session)
    {
        return QStringLiteral("ddma:physical:%1:%2:%3")
            .arg(generation).arg(session.diskIndex).arg(session.scratchLba);
    }

    template<typename Value>
    struct DdmaTaskResult
    {
        Value value;
        QString failure;
    };

    // Values and ownership cross the worker boundary; no QWidget, model or
    // document is accessed by the queued callable. The watcher dies with page.
    template<typename Value, typename Work, typename Apply>
    void submitPageTransaction(QObject* page, Work work, Apply apply
#if KSWORD_PERF_TRACE_ENABLED
        , const ks::ui::PerfTraceContext traceContext = {}
#endif
        )
    {
        using Result = DdmaTaskResult<Value>;
        auto promise = std::make_shared<QPromise<Result>>();
        auto* const watcher = new QFutureWatcher<Result>(page);
        watcher->setFuture(promise->future());
#if KSWORD_PERF_TRACE_ENABLED
        QMetaObject::Connection receiverDestroyed;
        if (traceContext.requestId != 0 || traceContext.actionId != 0)
        {
            receiverDestroyed = QObject::connect(page, &QObject::destroyed,
                [traceContext]() {
                // The serialized worker is not cancelled by page destruction.
                // Its eventual return must never be reported as UI readiness.
                ks::ui::RecordPerfAsyncTrace("ddma_transaction_receiver_destroyed", traceContext);
            });
            ks::ui::RecordPerfAsyncTrace("ddma_transaction_submit", traceContext);
        }
#endif
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, page,
            [watcher, apply = std::move(apply)
#if KSWORD_PERF_TRACE_ENABLED
                , traceContext, receiverDestroyed
#endif
            ]() {
            const auto result = watcher->result();
            watcher->deleteLater();
#if KSWORD_PERF_TRACE_ENABLED
            const ks::ui::PerfTraceContextScope traceScope(traceContext);
            ks::ui::RecordPerfAsyncTrace("ddma_transaction_ui_delivery", traceContext);
#endif
            apply(result);
#if KSWORD_PERF_TRACE_ENABLED
            // Keep the bounded destruction hook through the real apply body;
            // it is a discard outcome, not a business-ready substitute.
            QObject::disconnect(receiverDestroyed);
#endif
        });
        promise->start();
        Result rejected;
        bool accepted = false;
        try
        {
            accepted = ksword::memory_backend::submitDdmaTransaction(
            [promise, work = std::move(work)
#if KSWORD_PERF_TRACE_ENABLED
                , traceContext
#endif
            ]() {
#if KSWORD_PERF_TRACE_ENABLED
            const ks::ui::PerfTraceContextScope traceScope(traceContext);
            ks::ui::RecordPerfAsyncTrace("ddma_transaction_worker_begin", traceContext);
#endif
            Result result;
            try
            {
                result.value = work();
            }
            catch (const std::exception& error)
            {
                result.failure = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result.failure = QStringLiteral("DDMA transaction failed unexpectedly.");
            }
#if KSWORD_PERF_TRACE_ENABLED
            ks::ui::RecordPerfAsyncTrace(result.failure.isEmpty()
                ? "ddma_transaction_worker_returned" : "ddma_transaction_worker_exception", traceContext);
#endif
            promise->addResult(std::move(result));
            promise->finish();
        });
        }
        catch (const std::exception& error)
        {
            rejected.failure = QString::fromUtf8(error.what());
        }
        catch (...)
        {
            rejected.failure = QStringLiteral("DDMA transaction failed unexpectedly.");
        }
        if (!accepted)
        {
#if KSWORD_PERF_TRACE_ENABLED
            ks::ui::RecordPerfAsyncTrace("ddma_transaction_queue_rejected", traceContext);
#endif
            if (rejected.failure.isEmpty())
            {
                rejected.failure = QStringLiteral("DDMA transaction queue is full; wait for the current operation and retry.");
            }
            promise->addResult(std::move(rejected));
            promise->finish();
        }
    }

    // formatNtStatus：把 NTSTATUS 渲染成 8 位大写十六进制。
    QString formatNtStatus(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(status)), 8, 16, QChar('0'))
            .toUpper()
            .replace(QStringLiteral("0X"), QStringLiteral("0x"));
    }
}

// The process retains accepted write backups and final receipts after a page
// closes. This is evidence ownership, not automatic target rollback/retry.
struct DdmaPage::PhysicalWriteState
{
    ksword::memory_backend::DdmaPhysicalWriteRequest request;
    ksword::memory_backend::DdmaPhysicalWriteResult result;
    ks::ui::DisassemblyArchitecture architecture = ks::ui::DisassemblyArchitecture::X64;
    std::uint64_t currentAddress = 0;
    bool forceApproved = false;
    bool cancelled = false;
    bool retained = false;
    bool phaseStarted = false;
};

DdmaPage::DdmaPage(QWidget* const parent)
    : QWidget(parent)
{
    initializeUi();
    refreshSessionState();
}

void DdmaPage::setSessionChangedCallback(std::function<void()> callback)
{
    m_sessionChangedCallback = std::move(callback);
}

void DdmaPage::changeEvent(QEvent* const event)
{
    QWidget::changeEvent(event);
    if (event == nullptr)
    {
        return;
    }
    // 语义色是调用瞬间的快照，深浅色切换后必须重新下发，否则颜色停在旧主题。
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::PaletteChange)
    {
        applySemanticStyles();
    }
}

void DdmaPage::showEvent(QShowEvent* const event)
{
    QWidget::showEvent(event);
    m_pageExposed = true;
    QWidget* const top = window();
    if (m_exposureWindow != top)
    {
        if (m_exposureWindow) { m_exposureWindow->removeEventFilter(this); }
        m_exposureWindow = top;
        if (top) { top->installEventFilter(this); }
        m_windowExposed = top && top->isVisible();
    }
    schedulePendingForce();
}

void DdmaPage::hideEvent(QHideEvent* const event)
{
    m_pageExposed = false; // Includes spontaneous native Hide/minimize events.
    QWidget::hideEvent(event);
}

bool DdmaPage::eventFilter(QObject* const watched, QEvent* const event)
{
    if (watched == m_exposureWindow && event != nullptr)
    {
        if (event->type() == QEvent::Hide) { m_windowExposed = false; }
        else if (event->type() == QEvent::Show)
        {
            m_windowExposed = true;
            schedulePendingForce();
        }
        else if (event->type() == QEvent::WindowStateChange)
        {
            // Qt may deliver Show before the restored state is final. Recheck
            // after this event instead of losing the pending decision forever.
            schedulePendingForce();
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool DdmaPage::canPresentOperationPopup() const
{
    return m_pageExposed && m_windowExposed && m_exposureWindow
        && isVisible() && m_exposureWindow->isVisible() && !m_exposureWindow->isMinimized();
}

void DdmaPage::schedulePendingForce()
{
    if (m_forcePromptScheduled) { return; }
    m_forcePromptScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_forcePromptScheduled = false;
        if (m_pendingForceWrite && canPresentOperationPopup())
        {
            auto pending = std::move(m_pendingForceWrite);
            confirmPhysicalWriteForce(std::move(pending));
        }
    });
}

void DdmaPage::initializeUi()
{
    // 页面自带内部滚动壳：四个分组框纵向堆叠约有一千多像素，是内存 Dock 过长的最大单一来源。
    // 根布局建在壳的内容容器上；放不下时在页内滚动，不再撑高整个 Dock。
    QWidget* content = ks::ui::EnablePageInnerScroll(this);
    QVBoxLayout* rootLayout = new QVBoxLayout(content);
    rootLayout->setSpacing(16);
    // 专业通道、编辑器和复核各自保留，只用明确的无描边色块建立层次。
    const auto addSection = [rootLayout, content](QGroupBox* group, int stretch = 0)
    {
        auto* panel = new QWidget(content);
        ks::ui::StylePrimaryContentPanel(panel, false);
        auto* panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(16, 16, 16, 16);
        panelLayout->addWidget(group);
        rootLayout->addWidget(panel, stretch);
    };
    addSection(buildIntroGroup());
    addSection(buildChannelGroup());
    addSection(buildAccessGroup(), 1);
    addSection(buildCompareGroup());
    ks::ui::StyleWorkspacePage(this, 0);
    ks::ui::StyleWorkspacePage(content);
    ks::ui::StyleWorkspaceTextViews(content);

    applySemanticStyles();
}

QGroupBox* DdmaPage::buildIntroGroup()
{
    QGroupBox* group = new QGroupBox("DDMA 是什么", this);
    ks::ui::StyleParameterSection(group);
    QVBoxLayout* layout = new QVBoxLayout(group);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    QLabel* introLabel = new QLabel(group);
    introLabel->setWordWrap(true);
    introLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    introLabel->setText(
        "DDMA 让磁盘控制器用总线主控 DMA 直接读写物理地址。数据通路走 HBA 而不经过 CPU 页表，"
        "因此不受 SLAT / EPT 约束，能读到被上层虚拟化重定向或隐藏的物理页——"
        "标准通道在这些页上只会读到全 FF 或被替换过的内容。\n"
        "代价有三条，都无法绕开：\n"
        "一、必须借用一块磁盘扇区当中转站。本工具不提供默认扇区，必须由你显式指定 LBA 并确认；"
        "每次读写都在同一次请求内完成“备份→使用→还原”，但还原失败时磁盘上会留下脏扇区。\n"
        "二、开启内核调试的机器上会命中 MiShowBadMapper 直接蓝屏，此时整条通道被禁用。\n"
        "三、需要磁盘驱动栈接受直通命令。优先试 ATA 直通，不通再试 SCSI 直通"
        "（Windows 的 stornvme 会把它翻译成 NVMe 命令，所以 NVMe、SAS/SATA 与合成 SCSI 都走这条）；"
        "两条都被拒绝的盘用不了。另外部分 HBA 不支持 64 位寻址，高物理内存可能访问不到。");
    layout->addWidget(introLabel);

    return group;
}

QGroupBox* DdmaPage::buildChannelGroup()
{
    QGroupBox* group = new QGroupBox("通道配置", this);
    ks::ui::StyleParameterSection(group);
    QVBoxLayout* outerLayout = new QVBoxLayout(group);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    // 仅登记通道参数及其说明，磁盘选择表仍保留全宽交互空间。
    QWidget* parameterPanel = new QWidget(group);
    QGridLayout* formLayout = new QGridLayout(parameterPanel);
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->setHorizontalSpacing(8);
    formLayout->setVerticalSpacing(6);

    m_scratchLbaEdit = new QLineEdit(group);
    m_scratchLbaEdit->setPlaceholderText("必填，例如 0x100000 或 1048576");
    m_scratchLbaEdit->setClearButtonEnabled(true);
    m_scratchLbaEdit->setToolTip(
        "暂存扇区起始 LBA。DDMA 必须借磁盘扇区中转，本工具不提供默认值，"
        "留空则整条通道不可用。注意 LBA 0 起的前几个扇区是 MBR / GPT 保护扇区。");

    m_scratchImpactLabel = new QLabel(group);
    m_scratchImpactLabel->setWordWrap(true);
    m_scratchImpactLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_scratchAckCheck = new QCheckBox("我确认上述扇区上的数据可以被临时覆盖", group);
    m_scratchAckCheck->setToolTip(
        "每次 DDMA 读写都会先备份这几个扇区、用完立刻还原。"
        "但操作期间发生蓝屏或断电时，这几个扇区的原始数据会丢失。");

    m_probeButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/disk_analyze.svg")), "探测可用磁盘", group);
    m_probeButton->setToolTip(
        "枚举 \\Driver\\Disk 上的磁盘设备。已填写暂存 LBA 时，"
        "会对每块盘真的发一次 ATA DMA 读命令来判定通道是否可用（只读，不写盘）。");

    m_detectScratchButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/file_find.svg")), "侦测候选扇区", group);
    m_detectScratchButton->setEnabled(false);
    // 整串写在一行：跨行拼接会被 i18n 审计当成多个独立源串，逐段都要词条。
    m_detectScratchButton->setToolTip("读取选中磁盘的分区表，找出未分配间隙并把建议的 LBA 填进 LBA 输入框。只读不写。磁盘头部的间隙正是引导器寄居处，永远不会被选为建议值。填好之后仍然需要你自己勾选确认。");

    m_scratchDetectLabel = new QLabel(group);
    m_scratchDetectLabel->setWordWrap(true);
    m_scratchDetectLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // 扇区上下文：候选定下来之后把"这块扇区现在是什么"摊开，供再确认一次。
    // 上下文直接接受结构字段；原始磁盘字节在独立字节预览中保留。
    m_scratchContextView = new ks::ui::StructuredFieldView(group);
    m_scratchContextView->setMinimumHeight(150);
    m_scratchContextView->setDocument(ks::ui::FieldDocument{}.note(
        QStringLiteral("尚未侦测。选中一块磁盘后点击“侦测候选扇区”。")));

    m_activateButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/disk_save.svg")), "启用选中磁盘为 DDMA 通道", group);
    m_activateButton->setEnabled(false);
    m_activateButton->setToolTip(
        "把选中磁盘写入 DDMA 会话。启用后，内存搜索、内存查看器、"
        "驱动内存读写与系统内存审计四个页面都能选择 DDMA 后端。");

    m_clearButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/log_clear.svg")), "清除通道配置", group);
    m_clearButton->setToolTip("清空 DDMA 会话，所有页面立刻退回标准驱动通道。本页创建过专属暂存文件时会问你是否一并删除。");

    // LBA 独占输入行；探测动作单独成行，窄窗口不压缩地址输入。
    formLayout->addWidget(new QLabel("暂存扇区 LBA", group), 0, 0, 1, 2);
    formLayout->addWidget(m_scratchLbaEdit, 1, 0, 1, 2);
    formLayout->addWidget(m_detectScratchButton, 2, 0);
    formLayout->addWidget(m_probeButton, 2, 1);
    ks::ui::NormalizeToolbarControl(m_detectScratchButton);
    ks::ui::NormalizeToolbarControl(m_probeButton);
    formLayout->addWidget(m_scratchDetectLabel, 3, 0, 1, 2);
    formLayout->addWidget(m_scratchContextView, 4, 0, 1, 2);
    m_scratchBytesView = new CodeTextEdit(group);
    m_scratchBytesView->setReadOnly(true);
    m_scratchBytesView->setToolTip(QStringLiteral("前 128 字节十六进制预览："));
    m_scratchBytesView->setCompactMode(true);
    m_scratchBytesView->setLineNumbersVisible(false);
    m_scratchBytesView->setSyntaxLanguage(CodeTextEdit::SyntaxLanguage::PlainText);
    m_scratchBytesView->setMinimumHeight(100);
    m_scratchBytesView->setMaximumHeight(180);
    m_scratchBytesView->hide();
    formLayout->addWidget(m_scratchBytesView, 5, 0, 1, 2);
    formLayout->addWidget(m_scratchImpactLabel, 6, 0, 1, 2);
    formLayout->addWidget(m_scratchAckCheck, 7, 0, 1, 2);
    formLayout->setColumnStretch(0, 1);
    formLayout->setColumnStretch(1, 1);
    outerLayout->addWidget(parameterPanel);
    ks::ui::StyleParameterFields(parameterPanel);

    m_diskTable = new ks::ui::VisibleTableWidget(group);
    // 磁盘通道表是激活目标选择器，不是独立取证结果。
    ks::ui::SetTableActionBarMode(m_diskTable, ks::ui::TableActionBarMode::None);
    m_diskTable->setColumnCount(static_cast<int>(DiskColumn::Count));
    m_diskTable->setHorizontalHeaderLabels(
        QStringList{ "序号", "设备名", "通道状态", "探测 NTSTATUS", "扇区大小" });
    m_diskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_diskTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_diskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTable->setAlternatingRowColors(true);
    m_diskTable->verticalHeader()->setVisible(false);
    m_diskTable->verticalHeader()->setDefaultSectionSize(22);
    m_diskTable->setMinimumHeight(120);
    outerLayout->addWidget(m_diskTable);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(6);
    actionLayout->addWidget(m_activateButton);
    actionLayout->addWidget(m_clearButton);
    actionLayout->addStretch(1);
    ks::ui::NormalizeToolbarRow(actionLayout);
    outerLayout->addLayout(actionLayout);

    m_capabilityLabel = new QLabel("尚未探测。", group);
    m_capabilityLabel->setWordWrap(true);
    m_capabilityLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_capabilityLabel);

    m_sessionStateLabel = new QLabel(group);
    m_sessionStateLabel->setWordWrap(true);
    m_sessionStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_sessionStateLabel);

    connect(m_probeButton, &QPushButton::clicked, this, [this]() { probeChannels(); });
    connect(m_activateButton, &QPushButton::clicked, this, [this]() { activateSelectedDisk(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() { clearSession(); });
    connect(m_scratchLbaEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        // LBA 变了就必须重新确认：用户很可能是把目标换到了另一段扇区上。
        if (m_scratchAckCheck != nullptr && m_scratchAckCheck->isChecked())
        {
            m_scratchAckCheck->setChecked(false);
        }
        refreshSessionState();
        });
    connect(m_scratchAckCheck, &QCheckBox::toggled, this, [this](bool) { refreshSessionState(); });
    connect(m_detectScratchButton, &QPushButton::clicked, this, [this]() { detectScratchFromUi(); });
    connect(m_diskTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const bool hasRow = (m_diskTable->currentRow() >= 0);
        if (m_activateButton != nullptr)
        {
            m_activateButton->setEnabled(hasRow && !m_operationBusy);
        }
        // 侦测要读具体某一块盘的分区表，所以同样以选中行为前提。
        if (m_detectScratchButton != nullptr)
        {
            m_detectScratchButton->setEnabled(hasRow && !m_operationBusy);
        }
        });

    return group;
}

QGroupBox* DdmaPage::buildAccessGroup()
{
    QGroupBox* group = new QGroupBox("DDMA 物理读写", this);
    ks::ui::StyleParameterSection(group);
    QVBoxLayout* outerLayout = new QVBoxLayout(group);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    // 内存编辑器保留专业主视图，只统一明确的地址/长度输入区域。
    QWidget* parameterPanel = new QWidget(group);
    QGridLayout* barLayout = new QGridLayout(parameterPanel);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(6);

    m_accessAddressEdit = new QLineEdit(group);
    m_accessAddressEdit->setPlaceholderText("物理地址，例如 0x1000");
    m_accessAddressEdit->setClearButtonEnabled(true);

    m_accessLengthSpin = new QSpinBox(group);
    m_accessLengthSpin->setRange(1, 64 * 1024);
    m_accessLengthSpin->setValue(4096);
    m_accessLengthSpin->setSuffix(" B");
    m_accessLengthSpin->setToolTip(
        "读取长度。DDMA 的一次 DMA 传输就是一页，超过一页会按页边界自动切片，"
        "每一页都是一次完整的“备份→传输→还原”，所以长度越大越慢。");

    m_accessReadButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/process_details.svg")), "DDMA 读取", group);
    m_accessWriteButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/disk_save.svg")), "DDMA 写回差异", group);
    m_accessWriteButton->setEnabled(false);
    m_accessWriteButton->setToolTip(
        "把下方编辑器中改动过的字节用 DDMA 写回物理内存。"
        "非整页写入时驱动会做读-改-写，同页其它字节存在覆盖窗口。");

    barLayout->addWidget(new QLabel("物理地址", group), 0, 0, 1, 2);
    barLayout->addWidget(m_accessAddressEdit, 1, 0, 1, 2);
    barLayout->addWidget(new QLabel("长度", group), 2, 0, 1, 2);
    barLayout->addWidget(m_accessLengthSpin, 3, 0, 1, 2);
    barLayout->addWidget(m_accessReadButton, 4, 0);
    barLayout->addWidget(m_accessWriteButton, 4, 1);
    barLayout->setColumnStretch(0, 1);
    barLayout->setColumnStretch(1, 1);
    ks::ui::NormalizeToolbarControl(m_accessReadButton);
    ks::ui::NormalizeToolbarControl(m_accessWriteButton);
    outerLayout->addWidget(parameterPanel);
    ks::ui::StyleParameterFields(parameterPanel);

    m_accessMemoryEditor = new ks::ui::SnapshotWorkbenchWidget(group);
    m_accessHexEditor = m_accessMemoryEditor->hexEditor();
    m_accessMemoryEditor->setEditable(false);
    outerLayout->addWidget(m_accessMemoryEditor, 1);

    m_accessStatusLabel = new QLabel("等待读取。", group);
    m_accessStatusLabel->setWordWrap(true);
    m_accessStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_accessStatusLabel);

    connect(m_accessReadButton, &QPushButton::clicked, this, [this]() { readPhysicalFromUi(); });
    connect(m_accessWriteButton, &QPushButton::clicked, this, [this]() { writePhysicalFromUi(); });
    connect(m_accessMemoryEditor, &ks::ui::SnapshotWorkbenchWidget::bytesChanged, this,
        [this]() {
            if (m_hasSnapshot)
            {
                m_editedBytes = m_accessMemoryEditor->data();
            }
            refreshAccessEditorState();
        });

    return group;
}

QGroupBox* DdmaPage::buildCompareGroup()
{
    QGroupBox* group = new QGroupBox("标准通道 vs DDMA 同址复核", this);
    ks::ui::StyleParameterSection(group);
    QVBoxLayout* outerLayout = new QVBoxLayout(group);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    QLabel* hintLabel = new QLabel(group);
    hintLabel->setWordWrap(true);
    hintLabel->setText(
        "对同一物理地址各读一页：标准通道走 MmCopyMemory，受 SLAT 约束；"
        "DDMA 走设备 DMA，不受约束。两者不一致就是这一页被重定向或隐藏的直接证据。");
    outerLayout->addWidget(hintLabel);

    // 同址复核的参数单独登记，结果表仍沿用原有比较视图。
    QWidget* parameterPanel = new QWidget(group);
    QVBoxLayout* barLayout = new QVBoxLayout(parameterPanel);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(6);

    m_compareAddressEdit = new QLineEdit(group);
    m_compareAddressEdit->setPlaceholderText("物理页地址，例如 0x1000");
    m_compareAddressEdit->setClearButtonEnabled(true);

    m_compareButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/file_find.svg")), "复核这一页", group);

    barLayout->addWidget(new QLabel("物理地址", group));
    barLayout->addWidget(m_compareAddressEdit);
    barLayout->addWidget(m_compareButton);
    ks::ui::NormalizeToolbarControl(m_compareButton);
    outerLayout->addWidget(parameterPanel);
    ks::ui::StyleParameterFields(parameterPanel);

    m_compareResultLabel = new QLabel("尚未复核。", group);
    m_compareResultLabel->setWordWrap(true);
    m_compareResultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_compareResultLabel);

    m_compareTable = new QTableWidget(group);
    // 此表本身就是同址字节对比，小窗口不再重复显示通用对比栏。
    ks::ui::SetTableActionBarMode(m_compareTable, ks::ui::TableActionBarMode::None);
    m_compareTable->setColumnCount(3);
    m_compareTable->setHorizontalHeaderLabels(QStringList{ "偏移", "标准通道", "DDMA" });
    m_compareTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_compareTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_compareTable->setAlternatingRowColors(true);
    m_compareTable->verticalHeader()->setVisible(false);
    m_compareTable->verticalHeader()->setDefaultSectionSize(20);
    m_compareTable->setMaximumHeight(160);
    outerLayout->addWidget(m_compareTable);

    connect(m_compareButton, &QPushButton::clicked, this, [this]() { compareBackendsFromUi(); });

    return group;
}

void DdmaPage::applySemanticStyles()
{
    // 会话状态标签的颜色由 refreshSessionState 按可用性决定，这里只保证
    // 高危按钮在两种主题下都有一致的错误色填充。
    if (m_accessWriteButton != nullptr)
    {
        m_accessWriteButton->setStyleSheet(QStringLiteral("QPushButton{border-radius:3px;padding:4px 10px;}"));
        // 危险写入保留红色语义，禁用态和热主题由统一按钮绑定刷新。
        ks::ui::ApplyFlatButtonTheme(m_accessWriteButton, ks::ui::FlatButtonTone::Danger);
    }
    refreshSessionState();
}

bool DdmaPage::parseScratchLbaFromUi(std::uint64_t& lbaOut, QString& errorTextOut) const
{
    lbaOut = 0ULL;
    errorTextOut.clear();

    if (m_scratchLbaEdit == nullptr)
    {
        errorTextOut = QStringLiteral("界面尚未初始化。");
        return false;
    }

    const QString text = m_scratchLbaEdit->text().trimmed();
    if (text.isEmpty())
    {
        // 空输入一律判为"未填写"。绝不能退化成 0——LBA 0 是 MBR 所在扇区，
        // 靠默认值选中它正是这个功能最需要避免的事。
        errorTextOut = QStringLiteral("尚未填写暂存扇区 LBA。");
        return false;
    }

    std::uint64_t value = 0ULL;
    if (!parseSectorNumberText(text, value))
    {
        errorTextOut = QStringLiteral("暂存扇区 LBA 解析失败，请填写十进制或 0x 十六进制数值。");
        return false;
    }
    // 协议按 48 位 LBA 处理，超出范围的值 R0 会拒绝，这里先本地拦下。
    constexpr std::uint64_t kLbaMax = 0x0001000000000000ULL;
    if (value >= kLbaMax)
    {
        errorTextOut = QStringLiteral("暂存扇区 LBA 超出 48 位上限。");
        return false;
    }

    lbaOut = value;
    return true;
}

void DdmaPage::probeChannels()
{
    if (m_operationBusy)
    {
        return;
    }
#if KSWORD_PERF_TRACE_ENABLED
    const auto inputContext = ks::ui::CapturePerfTraceContext();
    const ks::ui::PerfTraceContextScope inputScope(inputContext);
    const auto traceContext = ks::ui::BeginPerfAsyncTrace();
    ks::ui::RecordPerfAsyncTrace("ddma_probe_ui_accepted", traceContext);
#endif
    std::uint64_t scratchLba = 0ULL;
    QString lbaError;
    const bool lbaValid = parseScratchLbaFromUi(scratchLba, lbaError);

    if (m_capabilityLabel != nullptr)
    {
        m_capabilityLabel->setText(QStringLiteral("正在探测 DDMA 通道..."));
    }

    // 选中的那块盘要在重建表格后还原回去。"侦测候选扇区"填好 LBA 之后会自动
    // 重探一次，如果这里不记住选择，用户刚选好的盘会被取消选中，紧接着点
    // "启用"只会得到一句"请先选中一块磁盘"。必须在 m_diskCache 被新结果覆盖
    // **之前**读，否则读到的是新表里同一行号上的另一块盘。
    std::uint32_t previousDeviceIndex = 0U;
    bool hadSelection = false;
    if (m_diskTable != nullptr)
    {
        const int previousRow = m_diskTable->currentRow();
        if (previousRow >= 0 && previousRow < static_cast<int>(m_diskCache.size()))
        {
            previousDeviceIndex = m_diskCache[static_cast<std::size_t>(previousRow)].deviceIndex;
            hadSelection = true;
        }
    }

    m_operationBusy = true;
    refreshOperationState();
    submitPageTransaction<ksword::ark::DdmaCapabilityResult>(this,
        [lbaValid, scratchLba]() {
            const ksword::ark::DriverClient client;
            return client.queryDdmaCapability(lbaValid, scratchLba, lbaValid);
        },
        [this, lbaValid, lbaError, previousDeviceIndex, hadSelection
#if KSWORD_PERF_TRACE_ENABLED
            , traceContext
#endif
        ](const auto& completed) {
#if KSWORD_PERF_TRACE_ENABLED
        const QPointer<DdmaPage> perfAlive(this);
#endif
        m_operationBusy = false;
        refreshOperationState();
        auto result = completed.value;
        if (!completed.failure.isEmpty())
        {
            result.io.ok = false;
            result.io.message = completed.failure.toStdString();
        }

        if (!result.io.ok)
        {
            m_probeCompleted = false;
            m_diskCache.clear();
            if (m_diskTable != nullptr)
            {
                m_diskTable->setRowCount(0);
            }
            if (m_capabilityLabel != nullptr)
            {
                m_capabilityLabel->setText(
                    QStringLiteral("探测失败：%1").arg(QString::fromStdString(result.io.message)));
            }
            refreshSessionState();
#if KSWORD_PERF_TRACE_ENABLED
            if (perfAlive) { ks::ui::RecordPerfAsyncTrace("ddma_probe_failed", traceContext); }
#endif
            return;
        }

        m_probeCompleted = true;
        m_diskCache = result.disks;
        m_kernelDebuggerEnabled = result.kernelDebuggerEnabled();
        m_transferBytes = result.transferBytes;
        m_scratchSectorCount = result.scratchSectorCount;

        // 探测结果变了，之前启用的通道未必还成立，先把会话里的探测面收回。
        m_session.kernelDebuggerEnabled = m_kernelDebuggerEnabled;
        m_session.transferBytes = m_transferBytes;
        m_session.scratchSectorCount = m_scratchSectorCount;

        if (m_diskTable != nullptr)
        {
            m_diskTable->setRowCount(static_cast<int>(m_diskCache.size()));
            for (int row = 0; row < static_cast<int>(m_diskCache.size()); ++row)
            {
                const ksword::ark::DdmaDiskEntry& entry = m_diskCache[static_cast<std::size_t>(row)];

                // 状态里写清楚"走哪条直通"：ATA 与 SCSI 是两条完全不同的路，
                // 只说"可用"会让人无法判断这块盘到底是怎么通的。
                QString stateText;
                if (entry.ataReady() && entry.scsiReady())
                {
                    stateText = QStringLiteral("可用（ATA 与 SCSI 均可）");
                }
                else if (entry.ataReady())
                {
                    stateText = QStringLiteral("可用（ATA 直通）");
                }
                else if (entry.scsiReady())
                {
                    stateText = QStringLiteral("可用（SCSI 直通，覆盖 NVMe）");
                }
                else if ((entry.diskFlags & KSWORD_ARK_DDMA_DISK_FLAG_PROBE_SKIPPED) != 0UL)
                {
                    stateText = QStringLiteral("未探测（需先填写暂存 LBA）");
                }
                else
                {
                    stateText = QStringLiteral("不可用（两条直通都被拒绝）");
                }

                m_diskTable->setItem(row, static_cast<int>(DiskColumn::Index),
                    new QTableWidgetItem(QString::number(entry.deviceIndex)));
                m_diskTable->setItem(row, static_cast<int>(DiskColumn::DeviceName),
                    new QTableWidgetItem(entry.deviceName.empty()
                        ? QStringLiteral("(名称不可用)")
                        : QString::fromStdWString(entry.deviceName)));
                m_diskTable->setItem(row, static_cast<int>(DiskColumn::State),
                    new QTableWidgetItem(stateText));
                // 两条探测各自的状态都要看得见：只显示一个会让"哪条不行"变成猜。
                m_diskTable->setItem(row, static_cast<int>(DiskColumn::ProbeStatus),
                    new QTableWidgetItem(QStringLiteral("ATA %1 / SCSI %2")
                        .arg(formatNtStatus(entry.probeStatus))
                        .arg(formatNtStatus(entry.scsiProbeStatus))));
                m_diskTable->setItem(row, static_cast<int>(DiskColumn::SectorSize),
                    new QTableWidgetItem(QString::number(entry.sectorSize)));
            }
            m_diskTable->resizeColumnsToContents();

            // 按 deviceIndex 还原选择，而不是按行号：枚举顺序在两次探测之间不保证不变。
            if (hadSelection)
            {
                for (int row = 0; row < static_cast<int>(m_diskCache.size()); ++row)
                {
                    if (m_diskCache[static_cast<std::size_t>(row)].deviceIndex == previousDeviceIndex)
                    {
                        m_diskTable->selectRow(row);
                        break;
                    }
                }
            }
        }

        QString capabilityText = QStringLiteral(
            "枚举到 %1 块磁盘，其中 %2 块通过 DMA 探测。一次传输 %3 字节，占用 %4 个扇区。")
            .arg(result.totalDisks)
            .arg(result.readyDisks)
            .arg(result.transferBytes)
            .arg(result.scratchSectorCount);
        if (!lbaValid)
        {
            capabilityText += QStringLiteral(" 本次未做传输探测：%1").arg(lbaError);
        }
        if (m_kernelDebuggerEnabled)
        {
            capabilityText += QStringLiteral(
                " 本机启用了内核调试，DDMA 会命中 MiShowBadMapper 蓝屏，通道已被禁用。");
        }
        if (m_capabilityLabel != nullptr)
        {
            m_capabilityLabel->setText(capabilityText);
        }

#if KSWORD_PERF_TRACE_ENABLED
        const auto perfDiskCount = static_cast<qint64>(m_diskCache.size());
#endif
        refreshSessionState();
#if KSWORD_PERF_TRACE_ENABLED
        // Full canonical disk snapshot, every table cell and selection, labels,
        // session publication and its original callback have completed. This
        // says nothing about paint or compositor presentation.
        if (perfAlive) { ks::ui::RecordPerfAsyncTrace("ddma_probe_business_data_ready", traceContext, perfDiskCount); }
#endif
    }
#if KSWORD_PERF_TRACE_ENABLED
        , traceContext
#endif
    );
}

bool DdmaPage::physicalDriveIndexFromDeviceName(
    const std::wstring& deviceName,
    std::uint32_t& indexOut)
{
    indexOut = 0U;
    // 设备名形如 \Device\Harddisk0\DR0。序号紧跟在 "Harddisk" 之后，
    // 与 \\.\PhysicalDriveN 的 N 是同一个数字。拿不到名字时不能猜——猜错就是
    // 去读另一块盘的分区表，然后建议用户覆盖那块盘上的扇区。
    const std::wstring marker = L"Harddisk";
    const std::size_t position = deviceName.find(marker);
    if (position == std::wstring::npos)
    {
        return false;
    }
    std::size_t cursor = position + marker.size();
    if (cursor >= deviceName.size() || deviceName[cursor] < L'0' || deviceName[cursor] > L'9')
    {
        return false;
    }
    std::uint64_t value = 0ULL;
    while (cursor < deviceName.size() && deviceName[cursor] >= L'0' && deviceName[cursor] <= L'9')
    {
        value = (value * 10ULL) + static_cast<std::uint64_t>(deviceName[cursor] - L'0');
        if (value > 0xFFFFULL)
        {
            return false;
        }
        ++cursor;
    }
    indexOut = static_cast<std::uint32_t>(value);
    return true;
}

DdmaPage::ScratchDetection DdmaPage::detectScratchByOwnedFile(
    const std::uint32_t driveIndex,
    const std::uint32_t sectorSize)
{
    ScratchDetection detection;
    detection.sourceText = QStringLiteral("专属暂存文件");

    if (sectorSize == 0U)
    {
        detection.summaryText = QStringLiteral("扇区大小未知，无法换算 LBA。");
        return detection;
    }

    // 第一步：找出落在这块物理磁盘上的卷。只认固定磁盘上的卷；
    // 换算 LBA 需要"卷在磁盘上的起始偏移"，只有单 extent 的卷能可靠给出。
    wchar_t driveStrings[512] = { 0 };
    const DWORD driveStringsLength =
        ::GetLogicalDriveStringsW(static_cast<DWORD>(std::size(driveStrings) - 1U), driveStrings);
    if (driveStringsLength == 0UL)
    {
        detection.summaryText = QStringLiteral("枚举卷失败，Win32 错误 %1。").arg(::GetLastError());
        return detection;
    }

    // chosenRoot 形如 "D:" 加一个反斜杠。注意：行注释绝不能以反斜杠结尾，
    // 那是续行符，会把下一行的声明整个吞进注释里。
    QString chosenRoot;
    std::uint64_t volumeStartOffset = 0ULL;
    for (const wchar_t* cursor = driveStrings; *cursor != L'\0'; cursor += wcslen(cursor) + 1U)
    {
        const QString root = QString::fromWCharArray(cursor);
        if (::GetDriveTypeW(cursor) != DRIVE_FIXED)
        {
            continue;
        }
        // \\.\X: 形式打开卷设备，问它落在哪块物理磁盘的哪个偏移上。
        const QString volumePath = QStringLiteral("\\\\.\\%1").arg(root.left(2));
        const HANDLE volumeHandle = ::CreateFileW(
            reinterpret_cast<LPCWSTR>(volumePath.utf16()),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            continue;
        }
        std::vector<std::uint8_t> extentBuffer(4096U, 0U);
        DWORD returned = 0UL;
        const BOOL extentOk = ::DeviceIoControl(
            volumeHandle,
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            nullptr,
            0UL,
            extentBuffer.data(),
            static_cast<DWORD>(extentBuffer.size()),
            &returned,
            nullptr);
        ::CloseHandle(volumeHandle);
        if (extentOk == FALSE || returned < sizeof(VOLUME_DISK_EXTENTS))
        {
            continue;
        }
        const auto* extents =
            reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extentBuffer.data());
        // 跨多块盘的卷（跨区/条带）拿不到单一起始偏移，换算会错，直接跳过。
        if (extents->NumberOfDiskExtents != 1UL)
        {
            continue;
        }
        if (extents->Extents[0].DiskNumber != driveIndex)
        {
            continue;
        }
        chosenRoot = root;
        volumeStartOffset =
            static_cast<std::uint64_t>(extents->Extents[0].StartingOffset.QuadPart);
        break;
    }

    if (chosenRoot.isEmpty())
    {
        detection.summaryText = QStringLiteral(
            "这块磁盘上没有找到可写入的单区间固定卷，无法建立专属暂存文件。");
        return detection;
    }

    // 第二步：簇大小。LCN 是按簇计的，换算成 LBA 必须知道一簇多少字节。
    DWORD sectorsPerCluster = 0UL;
    DWORD bytesPerSector = 0UL;
    DWORD freeClusters = 0UL;
    DWORD totalClusters = 0UL;
    if (::GetDiskFreeSpaceW(
            reinterpret_cast<LPCWSTR>(chosenRoot.utf16()),
            &sectorsPerCluster,
            &bytesPerSector,
            &freeClusters,
            &totalClusters) == FALSE ||
        sectorsPerCluster == 0UL || bytesPerSector == 0UL)
    {
        detection.summaryText = QStringLiteral(
            "读取 %1 的簇大小失败，Win32 错误 %2。").arg(chosenRoot).arg(::GetLastError());
        return detection;
    }
    const std::uint64_t clusterBytes =
        static_cast<std::uint64_t>(sectorsPerCluster) * bytesPerSector;

    // 第三步：建暂存文件并落盘。大小取一次传输的 16 倍，确保它一定是非驻留的
    // ——NTFS 会把很小的文件直接塞进 MFT 记录，那种文件没有任何 retrieval
    // pointer，拿不到 LCN。
    const std::uint64_t scratchBytes =
        static_cast<std::uint64_t>(ksword::memory_backend::ddmaTransferBytes()) * 16ULL;
    const QString filePath = chosenRoot + QStringLiteral("KSwordDdmaScratch.bin");
    {
        const HANDLE fileHandle = ::CreateFileW(
            reinterpret_cast<LPCWSTR>(filePath.utf16()),
            GENERIC_READ | GENERIC_WRITE,
            0UL,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            detection.summaryText = QStringLiteral(
                "创建暂存文件 %1 失败，Win32 错误 %2。").arg(filePath).arg(::GetLastError());
            return detection;
        }
        const std::vector<std::uint8_t> filler(static_cast<std::size_t>(scratchBytes), 0U);
        DWORD written = 0UL;
        const BOOL writeOk = ::WriteFile(
            fileHandle,
            filler.data(),
            static_cast<DWORD>(filler.size()),
            &written,
            nullptr);
        ::FlushFileBuffers(fileHandle);
        ::CloseHandle(fileHandle);
        if (writeOk == FALSE || written != filler.size())
        {
            detection.summaryText = QStringLiteral(
                "写入暂存文件失败，Win32 错误 %1。").arg(::GetLastError());
            return detection;
        }
    }

    // 第四步：取 retrieval pointers 拿第一个 extent 的 LCN。
    std::uint64_t firstLcn = 0ULL;
    std::uint64_t extentClusters = 0ULL;
    {
        const HANDLE fileHandle = ::CreateFileW(
            reinterpret_cast<LPCWSTR>(filePath.utf16()),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0UL,
            nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            detection.summaryText = QStringLiteral(
                "重新打开暂存文件失败，Win32 错误 %1。").arg(::GetLastError());
            return detection;
        }
        STARTING_VCN_INPUT_BUFFER input{};
        input.StartingVcn.QuadPart = 0;
        std::vector<std::uint8_t> output(8192U, 0U);
        DWORD returned = 0UL;
        const BOOL ok = ::DeviceIoControl(
            fileHandle,
            FSCTL_GET_RETRIEVAL_POINTERS,
            &input,
            static_cast<DWORD>(sizeof(input)),
            output.data(),
            static_cast<DWORD>(output.size()),
            &returned,
            nullptr);
        const DWORD lastError = ::GetLastError();
        ::CloseHandle(fileHandle);
        if (ok == FALSE && lastError != ERROR_MORE_DATA)
        {
            detection.summaryText = QStringLiteral(
                "读取暂存文件簇映射失败，Win32 错误 %1。文件可能是驻留在 MFT 里的小文件。")
                .arg(lastError);
            return detection;
        }
        const auto* pointers =
            reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(output.data());
        if (pointers->ExtentCount == 0UL)
        {
            detection.summaryText = QStringLiteral(
                "暂存文件没有任何簇映射（可能是驻留文件或稀疏文件），无法换算 LBA。");
            return detection;
        }
        if (pointers->Extents[0].Lcn.QuadPart < 0)
        {
            detection.summaryText = QStringLiteral("暂存文件第一段没有实际分配的簇。");
            return detection;
        }
        firstLcn = static_cast<std::uint64_t>(pointers->Extents[0].Lcn.QuadPart);
        extentClusters =
            static_cast<std::uint64_t>(pointers->Extents[0].NextVcn.QuadPart) -
            static_cast<std::uint64_t>(pointers->StartingVcn.QuadPart);
    }

    // 第五步：LCN → 卷内字节偏移 → 磁盘字节偏移 → 磁盘 LBA。
    const std::uint64_t diskByteOffset = volumeStartOffset + (firstLcn * clusterBytes);
    if ((diskByteOffset % sectorSize) != 0ULL)
    {
        detection.summaryText = QStringLiteral(
            "换算出的磁盘偏移没有落在扇区边界上，放弃这条路线。");
        return detection;
    }

    detection.ok = true;
    detection.suggestedLba = diskByteOffset / sectorSize;
    detection.scratchFilePath = filePath;
    detection.summaryText = QStringLiteral(
        "已在 %1 上建立专属暂存文件并占用它自己的簇：LCN %2，连续 %3 簇（每簇 %4 字节）。这块扇区归这个文件所有，不会有别的文件住在这里，也不会被系统分配给别人。通道使用期间不要删除这个文件——删掉它会把这些簇交还系统，可能立刻被分配给别的文件，而暂存 LBA 还指着原处。")
        .arg(filePath)
        .arg(firstLcn)
        .arg(extentClusters)
        .arg(clusterBytes);
    return detection;
}

DdmaPage::ScratchDetection DdmaPage::detectScratchCandidatesForDisk(
    const ksword::ark::DdmaDiskEntry& entry)
{
    ScratchDetection detection;

    std::uint32_t driveIndex = 0U;
    if (entry.deviceName.empty() ||
        !physicalDriveIndexFromDeviceName(entry.deviceName, driveIndex))
    {
        detection.summaryText = QStringLiteral(
            "无法从设备名推出物理磁盘序号，侦测中止。设备名=%1")
            .arg(entry.deviceName.empty()
                ? QStringLiteral("(不可用)")
                : QString::fromStdWString(entry.deviceName));
        return detection;
    }

    // 优先走"专属暂存文件"：那块扇区归我们所有，可证没有别的文件住在那里，
    // 也不存在"读完位图到真正 DMA 之间被系统分配出去"的竞态。只有这条路线走
    // 不通（磁盘上没有可写卷、文件驻留在 MFT 里等）时才退回未分配间隙。
    {
        ScratchDetection owned =
            detectScratchByOwnedFile(driveIndex, (entry.sectorSize != 0U) ? entry.sectorSize : 512U);
        if (owned.ok)
        {
            return owned;
        }
        // 失败原因要带到下一条路线的结论里，否则用户只会看到"用了间隙"，
        // 不知道更安全的那条为什么没走成。
        const QString ownedFailure = owned.summaryText;
        ScratchDetection fallback = detectScratchCandidatesByGap(driveIndex, entry);
        fallback.summaryText = QStringLiteral("未能使用专属暂存文件（%1）改用未分配间隙：%2")
            .arg(ownedFailure)
            .arg(fallback.summaryText);
        return fallback;
    }
}

DdmaPage::ScratchDetection DdmaPage::detectScratchCandidatesByGap(
    const std::uint32_t driveIndex,
    const ksword::ark::DdmaDiskEntry& entry)
{
    ScratchDetection detection;
    detection.sourceText = QStringLiteral("未分配间隙");

    const QString drivePath = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(driveIndex);
    const HANDLE diskHandle = ::CreateFileW(
        reinterpret_cast<LPCWSTR>(drivePath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (diskHandle == INVALID_HANDLE_VALUE)
    {
        detection.summaryText = QStringLiteral(
            "打开 %1 失败，Win32 错误 %2。侦测需要管理员权限。")
            .arg(drivePath)
            .arg(::GetLastError());
        return detection;
    }

    // 用 RAII 之外的简单收尾：下面每条返回路径都必须关掉句柄，所以统一在末尾关。
    std::vector<ksword::evidence::DdmaScratchOccupiedRange> occupied;
    std::uint64_t diskSectorCount = 0ULL;
    std::uint32_t sectorSize = (entry.sectorSize != 0U) ? entry.sectorSize : 512U;

    // 磁盘几何：拿扇区大小与总扇区数。
    {
        DISK_GEOMETRY_EX geometry{};
        DWORD returned = 0UL;
        if (::DeviceIoControl(
                diskHandle,
                IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                nullptr,
                0UL,
                &geometry,
                static_cast<DWORD>(sizeof(geometry)),
                &returned,
                nullptr) != FALSE)
        {
            if (geometry.Geometry.BytesPerSector != 0UL)
            {
                sectorSize = geometry.Geometry.BytesPerSector;
            }
            if (sectorSize != 0U && geometry.DiskSize.QuadPart > 0)
            {
                diskSectorCount =
                    static_cast<std::uint64_t>(geometry.DiskSize.QuadPart) / sectorSize;
            }
        }
    }
    if (diskSectorCount == 0ULL)
    {
        ::CloseHandle(diskHandle);
        detection.summaryText = QStringLiteral("读取磁盘几何失败，无法计算间隙。");
        return detection;
    }

    // 分区表：只取起止，风险分级交给纯算术模块。
    {
        std::vector<std::uint8_t> layoutBuffer(16384U, 0U);
        DWORD returned = 0UL;
        if (::DeviceIoControl(
                diskHandle,
                IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                nullptr,
                0UL,
                layoutBuffer.data(),
                static_cast<DWORD>(layoutBuffer.size()),
                &returned,
                nullptr) == FALSE ||
            returned < sizeof(DRIVE_LAYOUT_INFORMATION_EX))
        {
            ::CloseHandle(diskHandle);
            detection.summaryText = QStringLiteral(
                "读取分区表失败，Win32 错误 %1。").arg(::GetLastError());
            return detection;
        }
        const auto* layout =
            reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuffer.data());
        for (DWORD index = 0UL; index < layout->PartitionCount; ++index)
        {
            const PARTITION_INFORMATION_EX& partition = layout->PartitionEntry[index];
            if (partition.PartitionLength.QuadPart <= 0)
            {
                continue;
            }
            // MBR 表里未使用的槽位长度为零或类型为 0，上面那一条已经滤掉。
            ksword::evidence::DdmaScratchOccupiedRange range;
            range.startSector =
                static_cast<std::uint64_t>(partition.StartingOffset.QuadPart) / sectorSize;
            range.sectorCount =
                static_cast<std::uint64_t>(partition.PartitionLength.QuadPart) / sectorSize;
            occupied.push_back(range);
        }
    }

    const std::uint32_t requiredSectors =
        ksword::memory_backend::ddmaTransferBytes() / sectorSize;
    const std::vector<ksword::evidence::DdmaScratchCandidate> candidates =
        ksword::evidence::planDdmaScratchCandidates(diskSectorCount, occupied, requiredSectors);

    const ksword::evidence::DdmaScratchCandidate* chosen = nullptr;
    for (const ksword::evidence::DdmaScratchCandidate& candidate : candidates)
    {
        if (candidate.usable && ksword::evidence::ddmaScratchRiskIsSelectable(candidate.risk))
        {
            chosen = &candidate;
            break;
        }
    }

    if (chosen == nullptr)
    {
        ::CloseHandle(diskHandle);
        // 说清楚"为什么没有"，而不是只说没有。磁盘头部间隙常常是唯一的空档，
        // 而那正是绝不能自动推荐的一段。
        detection.summaryText = QStringLiteral(
            "这块磁盘上没有可自动推荐的暂存区间（共 %1 段未分配空间）。"
            "磁盘头部的间隙是引导器寄居处，尾部是 GPT 备份分区表，两者都不会被推荐；"
            "分区之间若没有留出至少一次传输的空档，就只能由你手工指定一个 LBA。")
            .arg(candidates.size());
        return detection;
    }

    // 第二重证据：分区表说"未分配"只是没人登记，真正读一遍才知道那里是不是空的。
    // 全零基本可以断定无人使用；非零一定要显著告警，因为那多半是没有分区表项的
    // 引导器或厂商数据。
    bool allZero = false;
    bool contentRead = false;
    {
        std::vector<std::uint8_t> sample(ksword::memory_backend::ddmaTransferBytes(), 0U);
        LARGE_INTEGER offset{};
        offset.QuadPart =
            static_cast<LONGLONG>(chosen->startSector * static_cast<std::uint64_t>(sectorSize));
        if (::SetFilePointerEx(diskHandle, offset, nullptr, FILE_BEGIN) != FALSE)
        {
            DWORD readBytes = 0UL;
            if (::ReadFile(
                    diskHandle,
                    sample.data(),
                    static_cast<DWORD>(sample.size()),
                    &readBytes,
                    nullptr) != FALSE &&
                readBytes == sample.size())
            {
                contentRead = true;
                allZero = std::all_of(
                    sample.begin(),
                    sample.end(),
                    [](const std::uint8_t value) { return value == 0U; });
            }
        }
    }
    ::CloseHandle(diskHandle);

    detection.ok = true;
    detection.suggestedLba = chosen->startSector;
    detection.contentAllZero = allZero;

    const QString riskText =
        (chosen->risk == ksword::evidence::DdmaScratchRisk::InteriorGap)
            ? QStringLiteral("分区之间的未分配间隙")
            : QStringLiteral("最后一个分区之后的未分配空间");
    QString summary = QStringLiteral(
        "建议 LBA %1（%2）。所在间隙 %3 - %4，共 %5 个扇区；一次传输占 %6 个。")
        .arg(chosen->startSector)
        .arg(riskText)
        .arg(chosen->gapStartSector)
        .arg(chosen->gapStartSector + chosen->gapSectorCount - 1ULL)
        .arg(chosen->gapSectorCount)
        .arg(requiredSectors);
    if (!contentRead)
    {
        summary += QStringLiteral(
            " 未能读回该区间内容做复核，无法确认它当前是否空闲，请自行判断。");
    }
    else if (allZero)
    {
        summary += QStringLiteral(" 已读回复核：该区间当前全为 0，没有观察到使用痕迹。");
    }
    else
    {
        summary += QStringLiteral(
            " 警告：已读回复核发现该区间**不是全零**。分区表说它未分配，但那里确实有数据——"
            "可能是没有分区表项的引导器或厂商保留数据。除非你清楚那是什么，否则不要用它。");
    }
    detection.summaryText = summary;
    return detection;
}

DdmaPage::ScratchContext DdmaPage::buildScratchContext(
    const std::uint32_t driveIndex,
    const std::uint64_t startLba,
    const std::uint32_t sectorSize,
    bool& allZeroOut)
{
    allZeroOut = false;
    ScratchContext context;
    ks::ui::FieldDocument& document = context.fields;
    document.section(QStringLiteral("扇区上下文"));

    const std::uint32_t transferBytes = ksword::memory_backend::ddmaTransferBytes();
    const std::uint32_t sectorCount =
        (sectorSize != 0U) ? (transferBytes / sectorSize) : 0U;
    const std::uint64_t byteOffset =
        startLba * static_cast<std::uint64_t>(sectorSize);

    document.field(QStringLiteral("目标磁盘"), QStringLiteral("\\\\.\\PhysicalDrive%1").arg(driveIndex));
    document.field(QStringLiteral("扇区大小"), QStringLiteral("%1 字节").arg(sectorSize));
    document.field(QStringLiteral("起始 LBA"), QStringLiteral("%1  (0x%2)")
                 .arg(startLba)
                 .arg(startLba, 0, 16));
    document.field(QStringLiteral("覆盖范围"), QStringLiteral("LBA %1 - %2，共 %3 个扇区")
                 .arg(startLba)
                 .arg(startLba + sectorCount - 1ULL)
                 .arg(sectorCount));
    document.field(QStringLiteral("磁盘字节偏移"), QStringLiteral("%1  (0x%2)")
                 .arg(byteOffset)
                 .arg(byteOffset, 0, 16));

    // 归属：这段偏移落在哪个分区里，还是落在分区之外。这一条最能让人一眼看出
    // "我是不是要去覆盖一个正在用的分区"。
    const QString drivePath = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(driveIndex);
    const HANDLE diskHandle = ::CreateFileW(
        reinterpret_cast<LPCWSTR>(drivePath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (diskHandle == INVALID_HANDLE_VALUE)
    {
        document.field(QStringLiteral("归属"), QStringLiteral("打不开磁盘，无法核对（Win32 %1）")
                     .arg(::GetLastError()));
        return context;
    }

    {
        std::vector<std::uint8_t> layoutBuffer(16384U, 0U);
        DWORD returned = 0UL;
        QString ownerText = QStringLiteral("未落在任何分区内（未分配空间）");
        if (::DeviceIoControl(
                diskHandle,
                IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                nullptr,
                0UL,
                layoutBuffer.data(),
                static_cast<DWORD>(layoutBuffer.size()),
                &returned,
                nullptr) != FALSE &&
            returned >= sizeof(DRIVE_LAYOUT_INFORMATION_EX))
        {
            const auto* layout =
                reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuffer.data());
            for (DWORD index = 0UL; index < layout->PartitionCount; ++index)
            {
                const PARTITION_INFORMATION_EX& partition = layout->PartitionEntry[index];
                if (partition.PartitionLength.QuadPart <= 0)
                {
                    continue;
                }
                const std::uint64_t start =
                    static_cast<std::uint64_t>(partition.StartingOffset.QuadPart);
                const std::uint64_t end =
                    start + static_cast<std::uint64_t>(partition.PartitionLength.QuadPart);
                if (byteOffset >= start && byteOffset < end)
                {
                    ownerText = QStringLiteral("分区 %1（起始字节 %2，长度 %3）")
                                    .arg(partition.PartitionNumber)
                                    .arg(start)
                                    .arg(end - start);
                    break;
                }
            }
        }
        document.field(QStringLiteral("归属"), QStringLiteral("%1").arg(ownerText));
    }

    // 内容预览：真读一遍。分区表说"未分配"只是没人登记，读回来才知道是不是空的。
    std::vector<std::uint8_t> sample(transferBytes, 0U);
    bool contentRead = false;
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(byteOffset);
    if (::SetFilePointerEx(diskHandle, offset, nullptr, FILE_BEGIN) != FALSE)
    {
        DWORD readBytes = 0UL;
        if (::ReadFile(
                diskHandle,
                sample.data(),
                static_cast<DWORD>(sample.size()),
                &readBytes,
                nullptr) != FALSE &&
            readBytes == sample.size())
        {
            contentRead = true;
        }
    }
    ::CloseHandle(diskHandle);

    if (!contentRead)
    {
        document.field(QStringLiteral("当前内容"), QStringLiteral("读取失败，无法复核"));
        return context;
    }

    allZeroOut = std::all_of(
        sample.begin(), sample.end(), [](const std::uint8_t value) { return value == 0U; });
    document.field(QStringLiteral("当前内容"), QStringLiteral("%1")
                 .arg(allZeroOut
                          ? QStringLiteral("全部为 0，没有观察到使用痕迹")
                          : QStringLiteral("非全零 —— 那里确实有数据，用之前请弄清是什么")));



    QStringList byteLines;
    // 逐行 16 字节，带偏移与 ASCII 侧栏。
    const std::size_t previewBytes = std::min<std::size_t>(sample.size(), 128U);
    for (std::size_t base = 0U; base < previewBytes; base += 16U)
    {
        QString hexPart;
        QString asciiPart;
        for (std::size_t column = 0U; column < 16U; ++column)
        {
            if (base + column >= previewBytes)
            {
                hexPart += QStringLiteral("   ");
                continue;
            }
            const std::uint8_t value = sample[base + column];
            hexPart += QStringLiteral("%1 ").arg(value, 2, 16, QChar('0')).toUpper();
            asciiPart += (value >= 0x20U && value < 0x7FU)
                ? QChar(static_cast<char16_t>(value))
                : QChar(QLatin1Char('.'));
        }
        byteLines << QStringLiteral("  +%1  %2 |%3|")
                     .arg(base, 4, 16, QChar('0'))
                     .arg(hexPart)
                     .arg(asciiPart);
    }

    context.bytePreview = byteLines.join(QLatin1Char('\n'));
    return context;
}

void DdmaPage::detectScratchFromUi()
{
    if (m_operationBusy)
    {
        return;
    }
    if (m_scratchDetectLabel != nullptr)
    {
        m_scratchDetectLabel->setText(QStringLiteral("正在侦测候选扇区..."));
    }

    const int row = m_diskTable != nullptr ? m_diskTable->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(m_diskCache.size()))
    {
        m_scratchDetectLabel->setText(QStringLiteral("请先在上表里选中一块磁盘。"));
        return;
    }
    const auto entry = m_diskCache[static_cast<std::size_t>(row)];
#if KSWORD_PERF_TRACE_ENABLED
    const auto inputContext = ks::ui::CapturePerfTraceContext();
    const ks::ui::PerfTraceContextScope inputScope(inputContext);
    const auto traceContext = ks::ui::BeginPerfAsyncTrace();
    ks::ui::RecordPerfAsyncTrace("ddma_scratch_ui_accepted", traceContext);
#endif
    struct ScratchResult
    {
        ScratchDetection detection;
        ScratchContext context;
        bool allZero = false;
    };
    m_operationBusy = true;
    refreshOperationState();
    submitPageTransaction<ScratchResult>(this,
        [entry]() {
        ScratchResult result;
        result.detection = detectScratchCandidatesForDisk(entry);
        if (!result.detection.ok)
        {
            return result;
        }
        std::uint32_t driveIndex = 0U;
        physicalDriveIndexFromDeviceName(entry.deviceName, driveIndex);
        const auto sectorSize = entry.sectorSize != 0U ? entry.sectorSize : 512U;
        // Re-read context after discovery, in this same serialized transaction.
        result.context = buildScratchContext(driveIndex,
            result.detection.suggestedLba, sectorSize, result.allZero);
        ks::ui::FieldNode sourceField;
        sourceField.kind = ks::ui::FieldNode::Kind::Field;
        sourceField.name = QStringLiteral("来源");
        sourceField.value = !result.detection.scratchFilePath.isEmpty()
            ? QStringLiteral("专属暂存文件 %1").arg(result.detection.scratchFilePath)
            : result.detection.sourceText;
        sourceField.translateValue = true;
        result.context.fields.nodes.first().children.prepend(sourceField);
        return result;
    }, [this
#if KSWORD_PERF_TRACE_ENABLED
        , traceContext
#endif
    ](const auto& completed) {
        m_operationBusy = false;
        refreshOperationState();
        const auto& detection = completed.value.detection;
        if (!completed.failure.isEmpty() || !detection.ok)
        {
            m_scratchDetectLabel->setText(!completed.failure.isEmpty()
                ? completed.failure : detection.summaryText);
            m_scratchDetectLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
            if (m_scratchContextView != nullptr)
            {
                m_scratchContextView->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("侦测未得出候选。")));
            }
            if (m_scratchBytesView != nullptr) { m_scratchBytesView->clear(); m_scratchBytesView->hide(); }
#if KSWORD_PERF_TRACE_ENABLED
            ks::ui::RecordPerfAsyncTrace("ddma_scratch_failed", traceContext);
#endif
            return;
        }
        const auto& context = completed.value.context;
        const bool allZero = completed.value.allZero;

        if (m_scratchBytesView != nullptr)
        {
            m_scratchBytesView->setPlainText(context.bytePreview);
            m_scratchBytesView->setVisible(!context.bytePreview.isEmpty());
        }
        m_scratchFilePath = detection.scratchFilePath;
        if (m_scratchContextView != nullptr)
        {
            m_scratchContextView->setDocument(context.fields);
        }
        if (m_scratchDetectLabel != nullptr)
        {
            m_scratchDetectLabel->setText(detection.summaryText);
            m_scratchDetectLabel->setStyleSheet(
                QStringLiteral("color:%1;")
                    .arg(allZero ? KswordTheme::SuccessHex() : KswordTheme::WarningHex()));
        }

        // 只填 LBA。确认框保持不动——自动算出候选是为了省掉手算，
        // 不是替用户承担"这块扇区可以被覆盖"这个判断。
        if (m_scratchLbaEdit != nullptr)
        {
            m_scratchLbaEdit->setText(QString::number(detection.suggestedLba));
        }
#if KSWORD_PERF_TRACE_ENABLED
        // Only the candidate's complete canonical document/preview/LBA are
        // published here. The required automatic channel probe is still pending
        // and receives a child request ID from this apply context below.
        ks::ui::RecordPerfAsyncTrace("ddma_scratch_candidate_snapshot_ready", traceContext);
        ks::ui::RecordPerfAsyncTrace("ddma_scratch_followup_probe", traceContext);
#endif

        // 填完 LBA 立刻重探一次。没有这一步，磁盘表里留着的还是"未探测（需先填写
        // 暂存 LBA）"的旧结果，用户接着点"启用选中磁盘"就会撞上一句
        // "这块磁盘没有通过 DMA 探测"——而他刚刚才把 LBA 填好，那句话只会让人困惑。
        // probeChannels 内部会还原选中行，并在末尾刷新会话状态。
        probeChannels();
    }
#if KSWORD_PERF_TRACE_ENABLED
        , traceContext
#endif
    );
}

void DdmaPage::activateSelectedDisk()
{
    if (m_operationBusy)
    {
        return;
    }
    if (m_diskTable == nullptr)
    {
        return;
    }
    const int row = m_diskTable->currentRow();
    if (row < 0 || row >= static_cast<int>(m_diskCache.size()))
    {
        QMessageBox::warning(this, QStringLiteral("DDMA"), QStringLiteral("请先选中一块磁盘。"));
        return;
    }

    const ksword::ark::DdmaDiskEntry& entry = m_diskCache[static_cast<std::size_t>(row)];
    if (!entry.ready())
    {
        // "压根没探测过"与"探测跑了但被拒绝"是两回事，给的下一步也完全不同：
        // 前者去填 LBA 再探一次就行，后者说明这块盘根本不接受 ATA 直通，
        // 再探多少次也没用。把两者混成一句话会让用户在死路上反复尝试。
        const bool probeSkipped =
            (entry.diskFlags & KSWORD_ARK_DDMA_DISK_FLAG_PROBE_SKIPPED) != 0UL;
        const QString message = probeSkipped
            ? QStringLiteral("这块磁盘还没有做过 DMA 传输探测。请先填写或侦测暂存扇区 LBA，再点“探测可用磁盘”。")
            : QStringLiteral("这块磁盘的两条直通都被拒绝，不能作为 DDMA 通道，重复探测也不会改变结果。ATA 直通 NTSTATUS=%1，SCSI 直通 NTSTATUS=%2。SCSI 直通覆盖 NVMe 与 SAS/SATA；两条都不通通常意味着这块盘的驱动栈不接受任何直通命令。")
                  .arg(formatNtStatus(entry.probeStatus))
                  .arg(formatNtStatus(entry.scsiProbeStatus));
        QMessageBox::warning(this, QStringLiteral("DDMA"), message);
        return;
    }

    std::uint64_t scratchLba = 0ULL;
    QString lbaError;
    if (!parseScratchLbaFromUi(scratchLba, lbaError))
    {
        QMessageBox::warning(this, QStringLiteral("DDMA"), lbaError);
        return;
    }

    m_session.configured = true;
    m_session.diskIndex = entry.deviceIndex;
    m_session.deviceName = entry.deviceName;
    m_session.scratchLba = scratchLba;
    m_session.scratchLbaValid = true;
    m_session.scratchAcknowledged =
        (m_scratchAckCheck != nullptr) && m_scratchAckCheck->isChecked();
    m_session.kernelDebuggerEnabled = m_kernelDebuggerEnabled;
    m_session.transferBytes = m_transferBytes;
    m_session.scratchSectorCount = m_scratchSectorCount;

    refreshSessionState();
}

void DdmaPage::clearSession()
{
    if (m_operationBusy)
    {
        return;
    }
    // Freeze the owned path and decision on GUI; do not ask the filesystem here.
    // Deletion is queued after earlier accepted DDMA transactions have restored
    // their scratch. Clearing configuration itself never waits for that queue.
    const QString scratchFilePath = m_scratchFilePath;
    const auto sessionAtPrompt = m_session;
    const auto generationAtPrompt = ksword::memory_backend::ddmaSessionGeneration();
    m_operationBusy = true;
    refreshOperationState();
    bool deleteScratchFile = false;
    if (!scratchFilePath.isEmpty())
    {
        const QPointer<DdmaPage> alive(this);
        const auto answer = QMessageBox::question(this, QStringLiteral("DDMA"),
            QStringLiteral("是否同时删除本页创建的专属暂存文件？\n%1\n\n删除后这些簇会交还系统。如果还有别处正在用这个 LBA，请选择“否”。")
                .arg(scratchFilePath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (!alive) { return; }
        if (generationAtPrompt != ksword::memory_backend::ddmaSessionGeneration()
            || !sameDdmaSession(sessionAtPrompt, m_session) || m_scratchFilePath != scratchFilePath)
        {
            m_operationBusy = false;
            refreshOperationState();
            return;
        }
        deleteScratchFile = answer == QMessageBox::Yes;
    }
    m_operationBusy = deleteScratchFile;
    m_scratchFilePath.clear();
    if (m_scratchBytesView != nullptr) { m_scratchBytesView->clear(); m_scratchBytesView->hide(); }
    if (m_scratchContextView != nullptr)
    {
        m_scratchContextView->setDocument(ks::ui::FieldDocument{}.note(
            QStringLiteral("尚未侦测。选中一块磁盘后点击“侦测候选扇区”。")));
    }

    m_session = ksword::memory_backend::DdmaSession{};
    // 探测出来的机器属性与会话是否启用无关，保留下来供状态展示继续使用。
    m_session.kernelDebuggerEnabled = m_kernelDebuggerEnabled;
    m_session.transferBytes = m_transferBytes;
    m_session.scratchSectorCount = m_scratchSectorCount;
    refreshSessionState();
    if (deleteScratchFile)
    {
        submitPageTransaction<bool>(this,
            [scratchFilePath]() {
                const bool removed = !QFile::exists(scratchFilePath) || QFile::remove(scratchFilePath);
                if (removed) { info << "DDMA scratch-file cleanup: path=" << scratchFilePath << " removed=1" << eol; }
                else { warn << "DDMA scratch-file cleanup: path=" << scratchFilePath << " removed=0" << eol; }
                return removed;
            }, [this, scratchFilePath](const auto& completed) {
            m_operationBusy = false;
            refreshOperationState();
            if (m_scratchDetectLabel == nullptr) { return; }
            const bool removed = completed.failure.isEmpty() && completed.value;
            m_scratchDetectLabel->setText(removed
                ? QStringLiteral("已删除专属暂存文件 %1。").arg(scratchFilePath)
                : QStringLiteral("删除专属暂存文件失败：%1。").arg(scratchFilePath));
            m_scratchDetectLabel->setStyleSheet(QStringLiteral("color:%1;")
                .arg(removed ? KswordTheme::TextSecondaryHex() : KswordTheme::WarningHex()));
        });
    }

}

void DdmaPage::refreshSessionState()
{
    // 勾选状态随时可变，会话里的这两项始终跟随控件，不需要重新点“启用”。
    if (m_session.configured)
    {
        m_session.scratchAcknowledged =
            (m_scratchAckCheck != nullptr) && m_scratchAckCheck->isChecked();

        std::uint64_t scratchLba = 0ULL;
        QString lbaError;
        if (parseScratchLbaFromUi(scratchLba, lbaError))
        {
            m_session.scratchLba = scratchLba;
            m_session.scratchLbaValid = true;
        }
        else
        {
            m_session.scratchLbaValid = false;
        }
    }

    // 影响范围文案要随输入实时更新，用户改 LBA 时立刻能看到会动哪几个扇区。
    if (m_scratchImpactLabel != nullptr)
    {
        std::uint64_t scratchLba = 0ULL;
        QString lbaError;
        const std::uint32_t sectorCount = (m_scratchSectorCount != 0U)
            ? m_scratchSectorCount
            : static_cast<std::uint32_t>(KSWORD_ARK_DDMA_SCRATCH_SECTOR_COUNT);
        if (parseScratchLbaFromUi(scratchLba, lbaError))
        {
            QString impactText = QStringLiteral(
                "每次 DDMA 操作会临时覆盖 LBA %1 到 %2 共 %3 个扇区（字节偏移 %4 起）。")
                .arg(scratchLba)
                .arg(scratchLba + sectorCount - 1ULL)
                .arg(sectorCount)
                .arg(formatAddress(scratchLba * KSWORD_ARK_DDMA_SECTOR_SIZE));
            if (scratchLba < sectorCount)
            {
                impactText += QStringLiteral(
                    " 警告：这段范围覆盖了 LBA 0，也就是 MBR / GPT 保护扇区。"
                    "操作期间发生蓝屏或断电会导致磁盘无法引导。");
            }
            m_scratchImpactLabel->setText(impactText);
        }
        else
        {
            m_scratchImpactLabel->setText(lbaError);
        }
    }

    // 本页是进程级会话的唯一写入者：本地状态一旦变动就立刻推上去，
    // 右上角的常驻指示灯与其它页面的后端下拉都从那一份读。
    // 主题刷新和重复填写相同值不创建新会话；关闭再启用仍经过两个真实变更。
    if (!sameDdmaSession(ksword::memory_backend::currentDdmaSession(), m_session))
    {
        ksword::memory_backend::setCurrentDdmaSession(m_session);
    }

    QString reason;
    const bool usable = ksword::memory_backend::isDdmaUsable(m_session, &reason);

    if (m_sessionStateLabel != nullptr)
    {
        if (usable)
        {
            m_sessionStateLabel->setText(QStringLiteral(
                "DDMA 通道已就绪：磁盘 #%1 %2，暂存 LBA %3。"
                "内存搜索、内存查看器、驱动内存读写与系统内存审计四个页面现在都可以选择 DDMA 后端。")
                .arg(m_session.diskIndex)
                .arg(m_session.deviceName.empty()
                    ? QStringLiteral("(名称不可用)")
                    : QString::fromStdWString(m_session.deviceName))
                .arg(m_session.scratchLba));
            m_sessionStateLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(KswordTheme::SuccessHex()));
        }
        else
        {
            m_sessionStateLabel->setText(QStringLiteral("DDMA 通道不可用：%1").arg(reason));
            // 内核调试那一条是"用了会蓝屏"，比其它"还没配好"严重，用错误色。
            m_sessionStateLabel->setStyleSheet(
                QStringLiteral("color:%1;")
                    .arg(m_session.kernelDebuggerEnabled
                        ? KswordTheme::ErrorHex()
                        : KswordTheme::WarningHex()));
        }
    }

    refreshOperationState();

    if (m_sessionChangedCallback)
    {
        m_sessionChangedCallback();
    }
}

void DdmaPage::refreshOperationState()
{
    const bool usable = ksword::memory_backend::isDdmaUsable(m_session, nullptr);
    const bool idle = !m_operationBusy;
    const bool selected = m_diskTable != nullptr && m_diskTable->currentRow() >= 0;
    if (m_diskTable != nullptr) { m_diskTable->setEnabled(idle); }
    if (m_probeButton != nullptr) { m_probeButton->setEnabled(idle); }
    if (m_detectScratchButton != nullptr) { m_detectScratchButton->setEnabled(idle && selected); }
    if (m_activateButton != nullptr) { m_activateButton->setEnabled(idle && selected); }
    if (m_clearButton != nullptr) { m_clearButton->setEnabled(idle); }
    if (m_scratchLbaEdit != nullptr) { m_scratchLbaEdit->setEnabled(idle); }
    if (m_scratchAckCheck != nullptr) { m_scratchAckCheck->setEnabled(idle); }
    if (m_accessReadButton != nullptr) { m_accessReadButton->setEnabled(idle && usable); }
    if (m_compareButton != nullptr) { m_compareButton->setEnabled(idle && usable); }
    refreshAccessEditorState();
}

void DdmaPage::resetAccessSnapshot()
{
    m_hasSnapshot = false;
    m_originalBytes.clear();
    m_editedBytes.clear();
    m_snapshotAddress = 0;
    m_snapshotSession = {};
    m_snapshotSessionGeneration = 0;
    m_accessMemoryEditor->clear();
    refreshAccessEditorState();
}

void DdmaPage::refreshAccessEditorState()
{
    const bool editable = !m_operationBusy && m_hasSnapshot && sameDdmaSession(m_snapshotSession, m_session)
        && m_snapshotSessionGeneration == ksword::memory_backend::ddmaSessionGeneration()
        && ksword::memory_backend::isDdmaUsable(m_session, nullptr);
    if (m_accessMemoryEditor != nullptr)
    {
        m_accessMemoryEditor->setEditable(editable);
    }
    if (m_accessWriteButton != nullptr)
    {
        m_accessWriteButton->setEnabled(editable && m_editedBytes != m_originalBytes);
    }
}

void DdmaPage::readPhysicalFromUi()
{
    if (m_operationBusy)
    {
        return;
    }
    if (m_accessAddressEdit == nullptr || m_accessLengthSpin == nullptr)
    {
        return;
    }

    std::uint64_t physicalAddress = 0ULL;
    if (!parseAddressText(m_accessAddressEdit->text(), physicalAddress))
    {
        QMessageBox::warning(
            this, QStringLiteral("DDMA"), QStringLiteral("物理地址解析失败，请填写 0x 十六进制地址。"));
        return;
    }
    // Freeze the complete request and pending edit witness before entering a
    // modal loop. Busy is acquired now so reentrant actions cannot start I/O.
    const std::uint64_t lengthBytes = static_cast<std::uint64_t>(m_accessLengthSpin->value());
    const auto readSession = m_session;
    const auto generation = ksword::memory_backend::ddmaSessionGeneration();
    const auto pendingAtPrompt = m_accessMemoryEditor->data();
    const auto baselineAtPrompt = m_accessMemoryEditor->originalBytes();
    const bool needsDiscardDecision = m_accessMemoryEditor->hasChanges();
    m_operationBusy = true;
    refreshOperationState();
    const QPointer<DdmaPage> alive(this);
    const bool discardAllowed = !needsDiscardDecision
        || QMessageBox::question(this, QStringLiteral("DDMA"),
            QStringLiteral("当前缓存存在未应用的改动。是否丢弃改动并读取新快照？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    if (!alive) { return; }
    if (!discardAllowed)
    {
        m_operationBusy = false;
        refreshOperationState();
        return;
    }
    if (generation != ksword::memory_backend::ddmaSessionGeneration()
        || !sameDdmaSession(readSession, m_session)
        || m_accessMemoryEditor->data() != pendingAtPrompt
        || m_accessMemoryEditor->originalBytes() != baselineAtPrompt)
    {
        m_operationBusy = false;
        refreshOperationState();
        m_accessStatusLabel->setText(QStringLiteral("DDMA 会话或内存快照已变化，请重新读取后再写入。"));
        return;
    }
    m_accessStatusLabel->setText(QStringLiteral("正在通过 DDMA 读取物理内存..."));
    submitPageTransaction<ksword::memory_backend::AccessOutcome>(this,
        [readSession, generation, physicalAddress, lengthBytes]() {
        if (generation != ksword::memory_backend::ddmaSessionGeneration())
        {
            ksword::memory_backend::AccessOutcome stale;
            stale.failureText = QStringLiteral("DDMA session changed; read a new snapshot before retrying.");
            return stale;
        }
        return ksword::memory_backend::readPhysical(
            ksword::memory_backend::MemoryAccessBackend::Ddma,
            readSession, physicalAddress, lengthBytes);
    }, [this, readSession, generation, physicalAddress, lengthBytes](const auto& completed) {
        m_operationBusy = false;
        refreshOperationState();
        if (generation != ksword::memory_backend::ddmaSessionGeneration())
        {
            m_accessStatusLabel->setText(QStringLiteral("DDMA session changed; read a new snapshot before retrying."));
            return;
        }
        auto outcome = completed.value;
        if (!completed.failure.isEmpty()) { outcome.failureText = completed.failure; }

        if (!outcome.ok || outcome.partial || outcome.data.size() != static_cast<qsizetype>(lengthBytes))
        {
            resetAccessSnapshot();
            const QString failure = !outcome.failureText.isEmpty() ? outcome.failureText
                : QStringLiteral("DDMA 读取长度不足：%1/%2 字节。")
                    .arg(outcome.data.size()).arg(lengthBytes);
            QString statusText = QStringLiteral("DDMA 读取失败：%1").arg(failure);
            if (outcome.scratchDirty)
            {
                statusText += QStringLiteral(
                    " 严重告警：暂存扇区未能还原，磁盘上留下了脏扇区，请立即检查 LBA %1 起的内容。")
                    .arg(readSession.scratchLba);
            }
            m_accessStatusLabel->setText(statusText);
            if (canPresentOperationPopup()) { QMessageBox::warning(this, QStringLiteral("DDMA"), failure); }
            return;
        }

        m_snapshotAddress = physicalAddress;
        m_snapshotSession = readSession;
        m_snapshotSessionGeneration = generation;
        m_originalBytes = outcome.data;
        m_editedBytes = m_originalBytes;
        m_hasSnapshot = true;
        const auto architecture = m_accessMemoryEditor->currentArchitecture();
        m_accessMemoryEditor->setSnapshot(m_originalBytes, m_snapshotAddress,
            architecture, m_snapshotAddress, ddmaSnapshotIdentity(generation, readSession));
        refreshAccessEditorState();

        QString statusText = QStringLiteral("DDMA 读取成功，共 %1 字节。").arg(m_originalBytes.size());
        if (outcome.scratchDirty)
        {
            statusText += QStringLiteral(
                " 严重告警：暂存扇区未能还原，磁盘上留下了脏扇区，请立即检查 LBA %1 起的内容。")
                .arg(readSession.scratchLba);
        }
        m_accessStatusLabel->setText(statusText);
    });
}

bool DdmaPage::accessWriteContextMatches(const PhysicalWriteState& state) const
{
    const auto& request = state.request;
    return m_hasSnapshot && m_snapshotAddress == request.snapshotAddress
        && m_snapshotSessionGeneration == request.generation
        && ksword::memory_backend::ddmaSessionGeneration() == request.generation
        && m_originalBytes == request.original && m_editedBytes == request.edited
        && m_accessMemoryEditor->baseAddress() == request.snapshotAddress
        && m_accessMemoryEditor->originalBytes() == request.original
        && m_accessMemoryEditor->data() == request.edited
        && sameDdmaSession(m_session, request.session)
        && ksword::memory_backend::isDdmaUsable(m_session, nullptr);
}

void DdmaPage::writePhysicalFromUi()
{
    if (m_operationBusy || !m_hasSnapshot || m_editedBytes == m_originalBytes) { return; }
    auto state = std::make_shared<PhysicalWriteState>();
    auto& request = state->request;
    request.session = m_snapshotSession;
    request.generation = m_snapshotSessionGeneration;
    request.snapshotAddress = m_snapshotAddress;
    request.original = m_originalBytes;
    request.edited = m_editedBytes;
    state->architecture = m_accessMemoryEditor->currentArchitecture();
    state->currentAddress = m_accessHexEditor->bufferSize() == 0
        ? m_accessHexEditor->baseAddress() : m_accessHexEditor->caretAddress();
    if (!accessWriteContextMatches(*state))
    {
        m_accessStatusLabel->setText(QStringLiteral("DDMA 会话或内存快照已变化，请重新读取后再写入。"));
        return;
    }
    while (request.firstDiff < request.edited.size()
        && request.edited[request.firstDiff] == request.original[request.firstDiff])
    {
        ++request.firstDiff;
    }
    qsizetype lastDiff = request.edited.size() - 1;
    while (lastDiff > request.firstDiff && request.edited[lastDiff] == request.original[lastDiff]) { --lastDiff; }
    request.payload = request.edited.mid(request.firstDiff, lastDiff - request.firstDiff + 1);
    request.targetAddress = request.snapshotAddress + static_cast<std::uint64_t>(request.firstDiff);

    // Block reentrant actions before the modal decision, while keeping GUI free
    // for other pages. No worker waits for this dialog or holds the I/O gate.
    m_operationBusy = true;
    refreshOperationState();
    const QPointer<DdmaPage> alive(this);
    const auto confirm = QMessageBox::warning(this, QStringLiteral("DDMA 写入确认"),
        QStringLiteral(
            "即将用磁盘 DMA 直接写入物理内存。\n"
            "起始物理地址: %1\n"
            "长度: %2 字节\n\n"
            "这条路径没有事务与回滚，非整页写入还会触发读-改-写，"
            "同页其它字节存在覆盖窗口。确认继续？")
            .arg(formatAddress(request.snapshotAddress)).arg(request.edited.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!alive) { return; }
    if (confirm != QMessageBox::Yes || !accessWriteContextMatches(*state))
    {
        m_operationBusy = false;
        refreshOperationState();
        return;
    }
    m_accessStatusLabel->setText(QStringLiteral("正在执行 DDMA 写入与读回校验..."));
    submitPhysicalWrite(std::move(state));
}

void DdmaPage::submitPhysicalWrite(std::shared_ptr<PhysicalWriteState> state)
{
    state->phaseStarted = false;
    submitPageTransaction<std::shared_ptr<PhysicalWriteState>>(this,
        [state]() {
        state->phaseStarted = true;
        // Intentional process lifetime: static destruction cannot erase backups
        // before the process queue finishes its accepted work during shutdown.
        static auto* const receipts = new std::vector<std::shared_ptr<PhysicalWriteState>>;
        if (!state->retained) { receipts->push_back(state); state->retained = true; }
        const auto& request = state->request;
        ksword::memory_backend::DdmaPhysicalWritePort port;
        port.isCurrent = [&request]() {
            return request.generation == ksword::memory_backend::ddmaSessionGeneration();
        };
        port.readActual = [&request]() {
            return ksword::memory_backend::readPhysical(
                ksword::memory_backend::MemoryAccessBackend::Ddma, request.session,
                request.snapshotAddress, static_cast<std::uint64_t>(request.original.size()));
        };
        port.write = [&request](const bool force) {
            return ksword::memory_backend::writePhysical(
                ksword::memory_backend::MemoryAccessBackend::Ddma, request.session,
                request.targetAddress, request.payload, force);
        };
        state->result = ksword::memory_backend::RunDdmaPhysicalWritePhase(
            request, port, state->forceApproved, state->cancelled);
        info << "DDMA physical write receipt: generation=" << request.generation
             << " disk=" << request.session.diskIndex << " lba=" << request.session.scratchLba
             << " physical=" << request.targetAddress << " bytes=" << request.payload.size()
             << " stage=" << static_cast<int>(state->result.stage)
             << " cancelled=" << state->result.cancelled << " writeAttempted=" << state->result.writeAttempted
             << " bytesDone=" << state->result.write.bytesDone << " scratchDirty=" << state->result.scratchDirty
             << " actualBytes=" << state->result.actual.data.size()
             << " failure=" << state->result.write.failureText << " exception=" << state->result.failureText << eol;
        return state;
    }, [this, state](const auto& completed) {
        auto& result = state->result;
        const auto& request = state->request;
        if (!completed.failure.isEmpty() && !state->phaseStarted)
        {
            // Queue rejection accepts no work and must not discard staged edits.
            m_operationBusy = false;
            refreshOperationState();
            m_accessStatusLabel->setText(QStringLiteral("DDMA 写入失败：%1").arg(completed.failure));
            return;
        }
        if (!completed.failure.isEmpty()) { result.failureText = completed.failure; }
        if (result.stage == ksword::memory_backend::DdmaPhysicalWriteStage::NeedForce
            && completed.failure.isEmpty())
        {
            confirmPhysicalWriteForce(state);
            return;
        }

        m_operationBusy = false;
        refreshOperationState();
        if (!accessWriteContextMatches(*state))
        {
            m_accessStatusLabel->setText(QStringLiteral("DDMA 写入事务已结束，但当前会话已变化。原事务结果保留在日志中；请重新读取。"));
            return;
        }
        const auto& actual = result.actual;
        const auto refreshActual = [&]() {
            if (!actual.ok || actual.partial || actual.data.size() != request.original.size())
            {
                resetAccessSnapshot();
                return false;
            }
            m_originalBytes = actual.data;
            m_editedBytes = actual.data;
            m_accessMemoryEditor->setSnapshot(actual.data, request.snapshotAddress, state->architecture,
                state->currentAddress, ddmaSnapshotIdentity(request.generation, request.session));
            refreshAccessEditorState();
            return true;
        };
        QString statusText;
        if (result.stage == ksword::memory_backend::DdmaPhysicalWriteStage::Stale)
        {
            statusText = QStringLiteral("DDMA 会话或内存快照已变化，请重新读取后再写入。");
        }
        else if (result.stage == ksword::memory_backend::DdmaPhysicalWriteStage::BeforeChanged)
        {
            const bool loaded = refreshActual();
            statusText = loaded && !result.scratchDirty
                ? QStringLiteral("目标内存已变化，已刷新实际字节；请重新编辑后再写入。")
                : QStringLiteral("DDMA 回读校验失败：%1").arg(result.before.failureText);
        }
        else
        {
            const bool cancelledWithoutWrite = result.cancelled && actual.ok && !actual.partial
                && actual.data == request.original;
            const bool loaded = cancelledWithoutWrite ? true : refreshActual();
            const bool verified = loaded && actual.data.mid(request.firstDiff, request.payload.size()) == request.payload;
            const bool written = result.write.ok && !result.write.partial
                && result.write.bytesDone == static_cast<std::uint64_t>(request.payload.size());
            if (result.cancelled)
            {
                statusText = QStringLiteral("已取消 DDMA 强制写入。");
            }
            else if (written && verified && result.failureText.isEmpty())
            {
                statusText = QStringLiteral("DDMA 写入成功，共 %1 字节。").arg(result.write.bytesDone);
            }
            else
            {
                const QString failure = !result.failureText.isEmpty() ? result.failureText
                    : (!result.write.ok && !result.write.failureText.isEmpty() ? result.write.failureText
                        : (!written ? QStringLiteral("写入未完成。")
                            : (!loaded ? QStringLiteral("DDMA 回读校验失败：%1").arg(actual.failureText)
                                : QStringLiteral("DDMA 回读字节与写入内容不一致。"))));
                statusText = QStringLiteral("DDMA 写入失败：%1").arg(failure);
            }
        }
        if (result.write.lostUpdateWindow)
        {
            statusText += QStringLiteral(" 本次为非整页写入，驱动做了读-改-写，同页其它字节存在覆盖窗口。");
        }
        if (result.scratchDirty)
        {
            statusText += QStringLiteral(" 严重告警：暂存扇区未能还原，磁盘上留下了脏扇区，请立即检查 LBA %1 起的内容。")
                .arg(request.session.scratchLba);
        }
        m_accessStatusLabel->setText(statusText);
    });
}

void DdmaPage::confirmPhysicalWriteForce(std::shared_ptr<PhysicalWriteState> state)
{
    // Hiding defers only the visual decision. No implicit approval/cancellation,
    // and no worker or transaction gate waits while the page is hidden.
    if (!canPresentOperationPopup()) { m_pendingForceWrite = std::move(state); return; }
    const QPointer<DdmaPage> alive(this);
    const auto forceConfirm = QMessageBox::warning(this, QStringLiteral("DDMA 强制写入"),
        QStringLiteral("驱动要求对本次 DDMA 写入附加强制标志。确认继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!alive) { return; }
    if (!accessWriteContextMatches(*state))
    {
        m_operationBusy = false;
        refreshOperationState();
        m_accessStatusLabel->setText(QStringLiteral("DDMA 会话或内存快照已变化，请重新读取后再写入。"));
        return;
    }
    state->forceApproved = forceConfirm == QMessageBox::Yes;
    state->cancelled = !state->forceApproved;
    submitPhysicalWrite(std::move(state));
}

void DdmaPage::compareBackendsFromUi()
{
    if (m_operationBusy)
    {
        return;
    }
    if (m_compareAddressEdit == nullptr || m_compareTable == nullptr)
    {
        return;
    }

    std::uint64_t physicalAddress = 0ULL;
    if (!parseAddressText(m_compareAddressEdit->text(), physicalAddress))
    {
        QMessageBox::warning(
            this, QStringLiteral("DDMA"), QStringLiteral("物理地址解析失败，请填写 0x 十六进制地址。"));
        return;
    }
    // 复核固定比一整页，两个后端读同一段才有可比性。
    const std::uint64_t pageBase =
        physicalAddress & ~static_cast<std::uint64_t>(KSWORD_ARK_DDMA_TRANSFER_BYTES - 1UL);

    m_compareTable->setRowCount(0);
    if (m_compareResultLabel != nullptr)
    {
        m_compareResultLabel->setText(QStringLiteral("正在复核..."));
    }

    const auto session = m_session;
    const auto generation = ksword::memory_backend::ddmaSessionGeneration();
    struct CompareResult
    {
        ksword::memory_backend::AccessOutcome standard;
        ksword::memory_backend::AccessOutcome ddma;
    };
    m_operationBusy = true;
    refreshOperationState();
    submitPageTransaction<CompareResult>(this,
        [session, generation, pageBase]() {
        CompareResult result;
        if (generation != ksword::memory_backend::ddmaSessionGeneration())
        {
            result.standard.failureText = QStringLiteral("DDMA session changed; read a new snapshot before retrying.");
            return result;
        }
        result.standard = ksword::memory_backend::readPhysical(
            ksword::memory_backend::MemoryAccessBackend::StandardDriver,
            session, pageBase, KSWORD_ARK_DDMA_TRANSFER_BYTES);
        result.ddma = ksword::memory_backend::readPhysical(
            ksword::memory_backend::MemoryAccessBackend::Ddma,
            session, pageBase, KSWORD_ARK_DDMA_TRANSFER_BYTES);
        return result;
    }, [this, generation, pageBase](const auto& completed) {
        m_operationBusy = false;
        refreshOperationState();
        if (generation != ksword::memory_backend::ddmaSessionGeneration())
        {
            m_compareResultLabel->setText(QStringLiteral("DDMA session changed; read a new snapshot before retrying."));
            return;
        }
        auto standardOutcome = completed.value.standard;
        const auto& ddmaOutcome = completed.value.ddma;
        if (!completed.failure.isEmpty()) { standardOutcome.failureText = completed.failure; }

        if (!standardOutcome.ok || !ddmaOutcome.ok || standardOutcome.partial || ddmaOutcome.partial
            || standardOutcome.data.size() != static_cast<qsizetype>(KSWORD_ARK_DDMA_TRANSFER_BYTES)
            || ddmaOutcome.data.size() != static_cast<qsizetype>(KSWORD_ARK_DDMA_TRANSFER_BYTES) || ddmaOutcome.scratchDirty)
        {
            // 一侧失败时不能下"两者一致"或"两者不一致"的结论，只能如实说哪一侧没读到。
            const QString detail = ddmaOutcome.scratchDirty
                ? QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe.")
                : (!standardOutcome.ok ? QStringLiteral("标准通道读取失败：%1").arg(standardOutcome.failureText)
                    : QStringLiteral("DDMA 读取失败：%1").arg(ddmaOutcome.failureText.isEmpty()
                        ? QStringLiteral("DDMA transaction returned incomplete data.") : ddmaOutcome.failureText));
            if (m_compareResultLabel != nullptr)
            {
                m_compareResultLabel->setText(
                    QStringLiteral("无法比对，%1").arg(detail));
                m_compareResultLabel->setStyleSheet(
                    QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
            }
            return;
        }

        const qsizetype compareLength =
            std::min<qsizetype>(standardOutcome.data.size(), ddmaOutcome.data.size());
        qsizetype diffCount = 0;
        int shownRows = 0;
        for (qsizetype index = 0; index < compareLength; ++index)
        {
            if (standardOutcome.data[index] == ddmaOutcome.data[index])
            {
                continue;
            }
            ++diffCount;
            if (shownRows >= kCompareSampleRows)
            {
                continue;
            }
            const int row = m_compareTable->rowCount();
            m_compareTable->insertRow(row);
            m_compareTable->setItem(row, 0,
                new QTableWidgetItem(formatAddress(pageBase + static_cast<std::uint64_t>(index))));
            m_compareTable->setItem(row, 1, new QTableWidgetItem(
                QStringLiteral("%1").arg(
                    static_cast<std::uint8_t>(standardOutcome.data[index]), 2, 16, QChar('0')).toUpper()));
            m_compareTable->setItem(row, 2, new QTableWidgetItem(
                QStringLiteral("%1").arg(
                    static_cast<std::uint8_t>(ddmaOutcome.data[index]), 2, 16, QChar('0')).toUpper()));
            ++shownRows;
        }
        m_compareTable->resizeColumnsToContents();

        if (m_compareResultLabel == nullptr)
        {
            return;
        }
        if (diffCount == 0)
        {
            // "一致"在一张退化页上什么都不证明：整页只有同一个字节时，一条彻底
            // 坏掉的 DDMA（比如什么都没搬、缓冲区留着全零）同样会与标准通道逐字节
            // 相等，于是这里报绿。而这条判据本身正是用来判断 DDMA 通路成不成立
            // 的——拿它当证据之前，必须先排除"两边都没读到有意义的内容"。
            bool uniformPage = true;
            for (qsizetype index = 1; index < compareLength; ++index)
            {
                if (standardOutcome.data[index] != standardOutcome.data[0])
                {
                    uniformPage = false;
                    break;
                }
            }
            if (compareLength > 0 && uniformPage)
            {
                m_compareResultLabel->setText(QStringLiteral("物理页 %1：整页 %2 字节都是同一个值 0x%3，两个后端一致不构成 DDMA 通路成立的证据——什么都没搬过来的实现也会得到同样的结果。请换一张内容有区分度的页再比对，例如某个已加载模块 PE 头所在的物理页。")
                    .arg(formatAddress(pageBase))
                    .arg(compareLength)
                    .arg(static_cast<std::uint8_t>(standardOutcome.data[0]), 2, 16, QChar('0')));
                m_compareResultLabel->setStyleSheet(
                    QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
            }
            else
            {
                m_compareResultLabel->setText(QStringLiteral(
                    "物理页 %1：两个后端读到的 %2 字节完全一致，没有观察到重定向迹象。")
                    .arg(formatAddress(pageBase))
                    .arg(compareLength));
                m_compareResultLabel->setStyleSheet(
                    QStringLiteral("color:%1;").arg(KswordTheme::SuccessHex()));
            }
        }
        else
        {
            m_compareResultLabel->setText(QStringLiteral(
                "物理页 %1：共 %2 字节中有 %3 字节不一致（表中最多展示 %4 条）。"
                "标准通道受 SLAT 约束，DDMA 不受约束，这种差异通常意味着该页被上层虚拟化重定向或隐藏。")
                .arg(formatAddress(pageBase))
                .arg(compareLength)
                .arg(diffCount)
                .arg(kCompareSampleRows));
            m_compareResultLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
        }
    });
}

bool DdmaPage::parseAddressText(const QString& text, std::uint64_t& valueOut)
{
    // 物理地址：无前缀按十六进制。本页的地址提示词写的就是 0x1000，回显也带
    // 0x，输入却按十进制解释的话，用户会去读另一个物理页而完全不会收到提示。
    valueOut = 0ULL;
    const auto parsed = ksword::evidence::ParseNumericText(
        text.trimmed().toStdString(),
        ksword::evidence::NumericTextDefaultRadix::Hexadecimal);
    if (!parsed.ok)
    {
        return false;
    }
    valueOut = parsed.value;
    return true;
}

bool DdmaPage::parseSectorNumberText(const QString& text, std::uint64_t& valueOut)
{
    // 扇区 LBA 是**数量**不是地址：它是从 0 数起的扇区序号，分区表、磁盘管理
    // 工具和本页的提示词（"例如 0x100000 或 1048576"）都按十进制念。所以这里
    // 必须保留十进制默认，不能跟着地址一起改成十六进制——那会让已经填好的
    // LBA 在下次打开时指向另一个扇区，而 LBA 指错的后果是覆盖别处的数据。
    valueOut = 0ULL;
    const auto parsed = ksword::evidence::ParseNumericText(
        text.trimmed().toStdString(),
        ksword::evidence::NumericTextDefaultRadix::Decimal);
    if (!parsed.ok)
    {
        return false;
    }
    valueOut = parsed.value;
    return true;
}

QString DdmaPage::formatAddress(const std::uint64_t address)
{
    return QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(address), 16, 16, QChar('0'))
        .toUpper()
        .replace(QStringLiteral("0X"), QStringLiteral("0x"));
}
