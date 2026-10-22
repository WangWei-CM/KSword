#include "ClipboardGuardPage.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/VisibleTableWidget.h"
#include "../../UI/FlatButtonTheme.h"
#include "../../UI/TableInteractionSupport.h"
#include "../../Internationalization/LanguageManager.h"
#include "../../theme.h"

#include <QAction>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QBrush>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include "../../UI/SegmentedControl.h"
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

// ============================================================
// ClipboardGuardPage.cpp
// 作用：
// 1) 构造"剪贴板保护"页的控件树：规则表 + 事件表 + 工具栏；
// 2) 实现事件表 A/B 列组（概览 / 来源与诊断），照抄 ProcessDock 线程页的
//    ThreadColumnLayout 范式；
// 3) 表头右键菜单逐列勾选显示/隐藏。
// ============================================================

namespace ks::misc
{
    namespace
    {
        enum class ClipboardGuardBodyText
        {
            AddRule, RemoveRule, Refresh, GlobalMonitor, Status, EventTitle, PresetA, PresetB
        };
        struct ClipboardGuardBodyEntry
        {
            const char* target;
            const char* source;
            bool mnemonic;
        };
        // The real controls and cold search keep one source for text and targets.
        constexpr ClipboardGuardBodyEntry kClipboardGuardBody[] = {
            {"ks_clipboard_guard_add_rule", "添加进程规则", true},
            {"ks_clipboard_guard_remove_rule", "删除规则", true},
            {"ks_clipboard_guard_refresh", "刷新", true},
            {"ks_clipboard_guard_global_monitor", "全局监控（记录所有进程的剪贴板访问）", true},
            {"ks_clipboard_guard_status", "尚未加载策略。", false},
            {"ks_clipboard_guard_event_title", "剪贴板访问事件：", false},
            {"ks_clipboard_guard_preset_a", "A", true},
            {"ks_clipboard_guard_preset_b", "B", true}
        };
        constexpr char kClipboardGuardRuleTableTarget[] = "ks_clipboard_guard_rule_table";
        constexpr char kClipboardGuardEventTableTarget[] = "ks_clipboard_guard_event_table";
        constexpr char kClipboardGuardEventActionsTarget[] = "ks_clipboard_guard_event_actions";
        const ClipboardGuardBodyEntry& clipboardGuardBodyEntry(ClipboardGuardBodyText id)
        {
            return kClipboardGuardBody[static_cast<int>(id)];
        }
        QString clipboardGuardBodySource(ClipboardGuardBodyText id)
        {
            return QString::fromUtf8(clipboardGuardBodyEntry(id).source);
        }
        QString clipboardGuardBodyTarget(ClipboardGuardBodyText id)
        {
            return QString::fromLatin1(clipboardGuardBodyEntry(id).target);
        }
        QStringList ruleTableHeaders()
        {
            return {
                QStringLiteral("目标"), QStringLiteral("匹配方式"), QStringLiteral("规则名"),
                QStringLiteral("读"), QStringLiteral("写"), QStringLiteral("枚举"), QStringLiteral("启用")
            };
        }
        QStringList eventTableHeaders()
        {
            return {
                QStringLiteral("时间"), QStringLiteral("进程"), QStringLiteral("PID"), QStringLiteral("操作"),
                QStringLiteral("格式"), QStringLiteral("结果"), QStringLiteral("TID"), QStringLiteral("会话"),
                QStringLiteral("完整性"), QStringLiteral("发起进程"), QStringLiteral("序号")
            };
        }

        // buildToolButtonStyle：工具栏按钮统一外观，颜色取自当前主题。
        QString buildToolButtonStyle()
        {
            // 纯色主题只接管颜色；保留本页按钮尺寸和业务选中状态。
            return ks::ui::BuildFlatButtonStyle(ks::ui::FlatButtonTone::Neutral,
                ks::ui::FlatButtonAppearance::Solid)
                + QStringLiteral("QPushButton{border-radius:4px;padding:4px 10px;}");

        }
    }

    QVector<ks::ui::PageSearchBodyText> ClipboardGuardPage::coldSearchBody()
    {
        QVector<ks::ui::PageSearchBodyText> body;
        body.reserve(28);
        const auto append = [&body](ClipboardGuardBodyText id)
        {
            const auto& entry = clipboardGuardBodyEntry(id);
            body.append({ks::i18n::sourceText(clipboardGuardBodySource(id)),
                clipboardGuardBodyTarget(id), {}, entry.mnemonic});
        };
        // Layout reparenting puts the rule panel's controls and table first,
        // then the event panel's title, preset buttons and table descendants.
        for (const auto id : {ClipboardGuardBodyText::AddRule, ClipboardGuardBodyText::RemoveRule,
            ClipboardGuardBodyText::Refresh, ClipboardGuardBodyText::GlobalMonitor, ClipboardGuardBodyText::Status})
            append(id);
        for (const auto& header : ruleTableHeaders())
            body.append({ks::i18n::sourceText(header), QString::fromLatin1(kClipboardGuardRuleTableTarget), {}, false});
        for (const auto id : {ClipboardGuardBodyText::EventTitle, ClipboardGuardBodyText::PresetA, ClipboardGuardBodyText::PresetB})
            append(id);
        // Headers precede descendants in GlobalUiSearch, including the five
        // event columns hidden by preset A. Both unactivated tables have no rows.
        for (const auto& header : eventTableHeaders())
            body.append({ks::i18n::sourceText(header), QString::fromLatin1(kClipboardGuardEventTableTarget), {}, false});
        body += ks::ui::ColdCompactTableActionBody(QString::fromLatin1(kClipboardGuardEventActionsTarget));
        return body;
    }

    ClipboardGuardPage::ClipboardGuardPage(QWidget* const parent)
        : QWidget(parent)
        , m_inbox(std::make_shared<ClipboardGuardInbox>())
        , m_runtime(std::make_shared<ClipboardGuardRuntime>(m_inbox))
    {
        initializeUi();
        initializeConnections();
    }

    ClipboardGuardPage::~ClipboardGuardPage()
    {
        if (m_processPollTimer) m_processPollTimer->stop();
        if (m_uiFlushTimer) m_uiFlushTimer->stop();
        // The registered value owner retains every session/lease until STOP,
        // CancelSynchronousIo, reader join and pipe/session handle cleanup finish.
        m_runtime->requestStop();
        m_runtime.reset();
    }

    void ClipboardGuardPage::showEvent(QShowEvent* const event)
    {
        QWidget::showEvent(event);
        m_statusUpdateClock.invalidate();
        scheduleEventProjection();
    }

    void ClipboardGuardPage::notifyPageActivated()
    {
        if (m_hasActivatedOnce)
        {
            return;
        }
        m_hasActivatedOnce = true;
        loadRulesFromDriver();
        refreshProcessListAndSessionsAsync();
        if (m_processPollTimer != nullptr)
        {
            m_processPollTimer->start();
        }
    }

    void ClipboardGuardPage::initializeUi()
    {
        m_rootLayout = new QVBoxLayout(this);
        ks::ui::StyleWorkspacePage(this);
        m_rootLayout->setSpacing(16);

        // 策略与持续事件分别占据独立色面，保留原表格及 A/B 列预设对象。
        auto* rulePanel = new QWidget(this); // 上方策略面板保持自然高度。
        auto* ruleLayout = new QVBoxLayout(rulePanel);
        ruleLayout->setContentsMargins(16, 16, 16, 16);
        ruleLayout->setSpacing(8);
        ks::ui::StylePrimaryContentPanel(rulePanel, false);
        m_rootLayout->addWidget(rulePanel);

        // ---- 工具栏 ----
        m_toolbarLayout = new QHBoxLayout();
        m_toolbarLayout->setContentsMargins(0, 0, 0, 0);
        m_toolbarLayout->setSpacing(8);

        m_addRuleButton = new QPushButton(QIcon(QStringLiteral(":/Icon/plus.svg")), clipboardGuardBodySource(ClipboardGuardBodyText::AddRule), this);
        m_addRuleButton->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::AddRule));
        m_addRuleButton->setToolTip(QStringLiteral("按进程名或完整路径新增一条剪贴板策略规则，规则会下发到驱动持久保存。"));
        m_addRuleButton->setStyleSheet(buildToolButtonStyle());

        m_removeRuleButton = new QPushButton(QIcon(QStringLiteral(":/Icon/log_clear.svg")), clipboardGuardBodySource(ClipboardGuardBodyText::RemoveRule), this);
        m_removeRuleButton->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::RemoveRule));
        m_removeRuleButton->setToolTip(QStringLiteral("删除规则表里当前选中的一条规则。"));
        m_removeRuleButton->setStyleSheet(buildToolButtonStyle());

        m_refreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), clipboardGuardBodySource(ClipboardGuardBodyText::Refresh), this);
        m_refreshButton->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::Refresh));
        m_refreshButton->setToolTip(QStringLiteral("立即重新扫描进程列表并按当前规则注入/移除保护，不必等待下一次自动轮询。"));
        m_refreshButton->setStyleSheet(buildToolButtonStyle());

        // 全局监控：勾选后新增一条 targetKind=ALL 的规则，覆盖系统上几乎全部
        // 进程；默认三个方向都只是"仅记录"，不新增任何拦截面，避免一勾选就
        // 打断全机复制粘贴。具体进程规则（PID/名称/路径）优先级更高，两者
        // 都命中时以具体规则的动作为准，见 findMatchingRule 的判定顺序。
        m_globalMonitorCheck = new QCheckBox(clipboardGuardBodySource(ClipboardGuardBodyText::GlobalMonitor), this);
        m_globalMonitorCheck->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::GlobalMonitor));
        m_globalMonitorCheck->setToolTip(QStringLiteral(
            "注入到系统上几乎所有进程以观察剪贴板访问，事件表里能看到任意进程的读/写/枚举；"
            "默认只记录不拦截，需要拦截某个进程时仍然用规则表单独设置该进程的动作。"
            "注入的进程数量可能达到一两百个，会有一定性能开销。"));

        m_statusLabel = new QLabel(this);
        m_statusLabel->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::Status));
        m_statusLabel->setText(clipboardGuardBodySource(ClipboardGuardBodyText::Status));

        m_toolbarLayout->addWidget(m_addRuleButton);
        m_toolbarLayout->addWidget(m_removeRuleButton);
        m_toolbarLayout->addWidget(m_refreshButton);
        m_toolbarLayout->addStretch(1);
        ks::ui::NormalizeToolbarRow(m_toolbarLayout);
        ruleLayout->addLayout(m_toolbarLayout);

        // 长监控选项与动作分行，状态在自身一行换行，窄窗口不挤压全部按钮。
        m_statusLabel->setWordWrap(true);
        ruleLayout->addWidget(m_globalMonitorCheck);
        ruleLayout->addWidget(m_statusLabel);

        // ---- 规则表：PID/映像/规则名/读/写/枚举/启用 ----
        m_ruleTable = new QTableWidget(0, 7, this);
        m_ruleTable->setObjectName(QString::fromLatin1(kClipboardGuardRuleTableTarget));
        // 规则表是小型配置选择器，不需要快照与冻结操作栏。
        ks::ui::SetTableActionBarMode(m_ruleTable, ks::ui::TableActionBarMode::None);
        m_ruleTable->setHorizontalHeaderLabels(ruleTableHeaders());
        m_ruleTable->horizontalHeader()->setStretchLastSection(false);
        m_ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_ruleTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_ruleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_ruleTable->setAlternatingRowColors(true);
        m_ruleTable->setMaximumHeight(160);
        ruleLayout->addWidget(m_ruleTable, 0);
        ks::ui::StyleParameterFields(rulePanel);

        auto* eventPanel = new QWidget(this); // 下方事件占满可用高度。
        auto* eventLayout = new QVBoxLayout(eventPanel);
        eventLayout->setContentsMargins(16, 16, 16, 16);
        eventLayout->setSpacing(8);
        ks::ui::StylePrimaryContentPanel(eventPanel, false);
        m_rootLayout->addWidget(eventPanel, 1);

        // ---- 事件表工具栏：A/B 列组预设 ----
        QHBoxLayout* const eventTopLayout = new QHBoxLayout();
        eventTopLayout->setContentsMargins(0, 0, 0, 0);
        eventTopLayout->setSpacing(8);
        auto* eventSectionTitle = new QLabel(clipboardGuardBodySource(ClipboardGuardBodyText::EventTitle), this);
        eventSectionTitle->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::EventTitle));
        ks::ui::StylePrimarySectionTitle(eventSectionTitle);
        eventTopLayout->addWidget(eventSectionTitle);

        m_columnPresetWidget = new QWidget(this);
        QHBoxLayout* const presetLayout = new QHBoxLayout(m_columnPresetWidget);
        presetLayout->setContentsMargins(0, 0, 0, 0);
        presetLayout->setSpacing(0);
        m_columnPresetAButton = new QPushButton(clipboardGuardBodySource(ClipboardGuardBodyText::PresetA), m_columnPresetWidget);
        m_columnPresetBButton = new QPushButton(clipboardGuardBodySource(ClipboardGuardBodyText::PresetB), m_columnPresetWidget);
        m_columnPresetAButton->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::PresetA));
        m_columnPresetBButton->setObjectName(clipboardGuardBodyTarget(ClipboardGuardBodyText::PresetB));
        m_columnPresetAButton->setCheckable(true);
        m_columnPresetBButton->setCheckable(true);
        m_columnPresetAButton->setToolTip(QStringLiteral("列预设 A：概览（时间/进程/PID/操作/格式/结果）。"));
        m_columnPresetBButton->setToolTip(QStringLiteral("列预设 B：来源与诊断（时间/PID/TID/会话/完整性/发起进程/序号）。"));
        presetLayout->addWidget(m_columnPresetAButton);
        presetLayout->addWidget(m_columnPresetBButton);
        ks::ui::InstallSegmentedButtons(presetLayout, {m_columnPresetAButton, m_columnPresetBButton});
        ks::ui::NormalizeToolbarRow(presetLayout, 0);
        eventTopLayout->addWidget(m_columnPresetWidget);
        eventTopLayout->addStretch(1);
        ks::ui::NormalizeToolbarRow(eventTopLayout);
        eventLayout->addLayout(eventTopLayout);

        // ---- 事件表 ----
        m_eventTable = new QTableWidget(0, ColumnCount, this);
        m_eventTable->setObjectName(QString::fromLatin1(kClipboardGuardEventTableTarget));
        // 剪贴板事件已有时间序列与业务右键操作，保留紧凑复制导出。
        ks::ui::SetTableActionBarMode(m_eventTable, ks::ui::TableActionBarMode::Compact);
        ks::ui::SetCompactTableSearchTargetPrefix(m_eventTable, QString::fromLatin1(kClipboardGuardEventActionsTarget));
        m_eventTable->setHorizontalHeaderLabels(eventTableHeaders());
        m_eventTable->horizontalHeader()->setStretchLastSection(true);
        m_eventTable->horizontalHeader()->setSectionsMovable(true);
        m_eventTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_eventTable->setAlternatingRowColors(true);
        m_eventTable->setContextMenuPolicy(Qt::CustomContextMenu);
        applyColumnLayout(ClipboardGuardColumnLayout::PresetA);
        eventLayout->addWidget(m_eventTable, 1);

        m_uiFlushTimer = new QTimer(this);
        m_uiFlushTimer->setInterval(200);

        m_processPollTimer = new QTimer(this);
        // 1 秒轮询：比初版的 2 秒更贴近"实时"，新启动的进程能更快被发现/注入；
        // 扫描本身在 QThreadPool 后台线程执行，缩短间隔不会卡 UI 线程，
        // 代价只是后台扫描更频繁（单次扫描本身通常在数十毫秒量级）。
        m_processPollTimer->setInterval(1000);
    }

    void ClipboardGuardPage::initializeConnections()
    {
        connect(m_addRuleButton, &QPushButton::clicked, this, &ClipboardGuardPage::openAddProcessRuleDialog);
        connect(m_removeRuleButton, &QPushButton::clicked, this, &ClipboardGuardPage::removeSelectedRule);
        connect(m_refreshButton, &QPushButton::clicked, this, &ClipboardGuardPage::refreshProcessListAndSessionsAsync);
        connect(m_globalMonitorCheck, &QCheckBox::toggled, this, &ClipboardGuardPage::toggleGlobalMonitor);
        connect(m_columnPresetAButton, &QPushButton::clicked, this, [this]() { applyColumnLayout(ClipboardGuardColumnLayout::PresetA); });
        connect(m_columnPresetBButton, &QPushButton::clicked, this, [this]() { applyColumnLayout(ClipboardGuardColumnLayout::PresetB); });
        connect(m_eventTable, &QWidget::customContextMenuRequested, this, &ClipboardGuardPage::showEventTableContextMenu);
        connect(m_eventTable->horizontalHeader(), &QWidget::customContextMenuRequested, this, &ClipboardGuardPage::showHeaderContextMenu);
        connect(m_uiFlushTimer, &QTimer::timeout, this, &ClipboardGuardPage::flushPendingRows);
        connect(m_processPollTimer, &QTimer::timeout, this, &ClipboardGuardPage::refreshProcessListAndSessionsAsync);
        m_uiFlushTimer->start();
    }

    void ClipboardGuardPage::applyColumnLayout(const ClipboardGuardColumnLayout layout)
    {
        if (m_eventTable == nullptr || layout == ClipboardGuardColumnLayout::Custom)
        {
            m_columnLayout = layout;
            updateColumnPresetButtons();
            return;
        }

        static constexpr std::array<int, 6> kPresetAColumns{
            ColumnTime, ColumnProcess, ColumnPid, ColumnOperation, ColumnFormat, ColumnResult
        };
        static constexpr std::array<int, 7> kPresetBColumns{
            ColumnTime, ColumnPid, ColumnTid, ColumnSession, ColumnIntegrity, ColumnOwner, ColumnSeq
        };
        const auto columnVisible = [layout](const int columnIndex) {
            if (layout == ClipboardGuardColumnLayout::PresetA)
            {
                return std::find(kPresetAColumns.cbegin(), kPresetAColumns.cend(), columnIndex) != kPresetAColumns.cend();
            }
            return std::find(kPresetBColumns.cbegin(), kPresetBColumns.cend(), columnIndex) != kPresetBColumns.cend();
        };

        m_applyingColumnLayout = true;
        for (int columnIndex = 0; columnIndex < static_cast<int>(ColumnCount); ++columnIndex)
        {
            m_eventTable->setColumnHidden(columnIndex, !columnVisible(columnIndex));
        }
        m_applyingColumnLayout = false;
        m_columnLayout = layout;
        updateColumnPresetButtons();
    }

    void ClipboardGuardPage::clearColumnPresetSelection()
    {
        if (m_applyingColumnLayout)
        {
            return;
        }
        m_columnLayout = ClipboardGuardColumnLayout::Custom;
        updateColumnPresetButtons();
    }

    void ClipboardGuardPage::updateColumnPresetButtons()
    {
        if (m_columnPresetAButton == nullptr || m_columnPresetBButton == nullptr)
        {
            return;
        }
        const QSignalBlocker blockerA(m_columnPresetAButton);
        const QSignalBlocker blockerB(m_columnPresetBButton);
        m_columnPresetAButton->setChecked(m_columnLayout == ClipboardGuardColumnLayout::PresetA);
        m_columnPresetBButton->setChecked(m_columnLayout == ClipboardGuardColumnLayout::PresetB);
        // 信号屏蔽期间显式同步选中状态，绘制仍只由共享分段控件拥有。
        ks::ui::SetSegmentedButtonSelected(m_columnPresetAButton,
            m_columnLayout == ClipboardGuardColumnLayout::PresetA);
        ks::ui::SetSegmentedButtonSelected(m_columnPresetBButton,
            m_columnLayout == ClipboardGuardColumnLayout::PresetB);
    }

    QTableWidgetItem* ClipboardGuardPage::createReadOnlyItem(const QString& textValue)
    {
        QTableWidgetItem* const itemValue = new QTableWidgetItem(textValue);
        itemValue->setFlags(itemValue->flags() & ~Qt::ItemIsEditable);
        return itemValue;
    }

    void ClipboardGuardPage::showHeaderContextMenu(const QPoint& position)
    {
        if (m_eventTable == nullptr)
        {
            return;
        }
        QMenu columnMenu(m_eventTable);
        columnMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
        static const std::array<QString, ColumnCount> kHeaderNames{
            QStringLiteral("时间"), QStringLiteral("进程"), QStringLiteral("PID"), QStringLiteral("操作"),
            QStringLiteral("格式"), QStringLiteral("结果"), QStringLiteral("TID"), QStringLiteral("会话"),
            QStringLiteral("完整性"), QStringLiteral("发起进程"), QStringLiteral("序号")
        };
        for (int columnIndex = 0; columnIndex < static_cast<int>(ColumnCount); ++columnIndex)
        {
            QAction* const columnAction = columnMenu.addAction(kHeaderNames[static_cast<std::size_t>(columnIndex)]);
            columnAction->setCheckable(true);
            columnAction->setChecked(!m_eventTable->isColumnHidden(columnIndex));
            connect(columnAction, &QAction::toggled, &columnMenu, [this, columnIndex](const bool visible) {
                m_eventTable->setColumnHidden(columnIndex, !visible);
                clearColumnPresetSelection();
            });
        }
        columnMenu.exec(m_eventTable->horizontalHeader()->mapToGlobal(position));
    }
}
