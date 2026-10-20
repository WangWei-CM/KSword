#include "ContextMenuCleanerTab.h"
#include "../../UI/PageControlStyle.h"
#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/FlatButtonTheme.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../Framework/PrivilegeElevationPrompt.h"
#include "../../UI/VisibleTableWidget.h"
#include "../../UI/TableInteractionSupport.h"
#include "../../UI/StructuredFieldView.h"
#include "../../UI/DetailLayoutRegistry.h"
#include "../../UI/AsyncOperation.h"
#include "../../Internationalization/LanguageManager.h"
#include <QElapsedTimer>
#include <QShowEvent>
#include <QHash>
#include <QPointer>
#include "../../UI/RetainedCleanupJob.h"

#include "ContextMenuCleanerTab.Internal.h"
#include "../../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <exception>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QStringList>

#include <array>

namespace {
const ks::ui::PageNavigationTabDefinition kContextMenuNavigation[] = {
    {"ie", "", "", "IE右键菜单", true},
    {"desktop", "", "", "桌面右键菜单", false},
    {"file", "", "", "文件右键菜单", false},
    {"url", "", "", "URL 绑定", false},
    {"open_with", "", "", "文件打开方式", false},
    {"format", "", "", "格式右键菜单", false},
    {"explorer", "", "", "资源管理器主页第三方程序", false}
};
}

namespace ks::misc
{

using namespace context_menu_cleaner_detail;

ContextMenuCleanerTab::ContextMenuCleanerTab(QWidget* parent)
    : QWidget(parent)
{
    // One value gate survives page close/reopen while accepted work drains.
    static const auto registrySerial = std::make_shared<std::mutex>();
    m_registrySerial = registrySerial;
    m_shellOperation = new ks::ui::AsyncOperation(this);
    m_shellOperation->setStateChangedCallback([this]() { settleShellTransaction(); });
    initializeUi();
    refreshArea(MenuArea::InternetExplorer);

    kLogEvent event;
    info << event << "[ContextMenuCleanerTab] 右键菜单清理页初始化完成。" << eol;
}

ContextMenuCleanerTab::~ContextMenuCleanerTab()
{
    if (m_latencySession) m_latencySession->stop.store(true);
    if (m_latencyCleanup && !m_latencyCleanup->isCleanupRequested()) m_latencyCleanup->requestCleanup();
    m_shellCompletion = {};
    if (m_shellOperation != nullptr) m_shellOperation->close();
    const std::array<MenuArea, 7> areas{MenuArea::InternetExplorer, MenuArea::Desktop,
        MenuArea::File, MenuArea::UrlBinding, MenuArea::OpenWith, MenuArea::FormatMenu,
        MenuArea::ExplorerHome};
    for (const MenuArea area : areas)
    {
        AreaWidgets* widgets = widgetsForArea(area);
        if (widgets->queryOperation != nullptr) widgets->queryOperation->close();
        if (widgets->presentationTimer != nullptr) widgets->presentationTimer->stop();
    }
}

void ContextMenuCleanerTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    AreaWidgets* widgets = widgetsForArea(currentArea());
    if (widgets != nullptr && widgets->presentationTimer != nullptr
        && (widgets->tableBuilding || widgets->pendingSnapshot.has_value()))
        widgets->presentationTimer->start(0);
}

void ContextMenuCleanerTab::initializeUi()
{
    // 根布局：上方风险说明，下方七类 Shell 关联子页。
    m_rootLayout = new QVBoxLayout(this);
    ks::ui::StyleWorkspacePage(this);
    m_rootLayout->setSpacing(0); // 跨分类动作与分类结果连体，避免Tab下的表格重复内缩。

    // 页面级工具栏：整份菜单备份 / 恢复备份 / 当前分类耗时测量。
    // 放在子页签上方，因为这三件事都是跨分类的操作（备份与恢复覆盖全部七类）。
    m_pageToolbar = new QWidget(this);
    QHBoxLayout* pageToolbarLayout = new QHBoxLayout(m_pageToolbar);
    pageToolbarLayout->setContentsMargins(16, 12, 16, 12);
    pageToolbarLayout->setSpacing(6);
    ks::ui::StylePrimaryToolbar(m_pageToolbar);

    m_backupButton = new QPushButton(QIcon(QStringLiteral(":/Icon/log_copy.svg")),
        QStringLiteral("备份右键菜单"), m_pageToolbar);
    m_backupButton->setToolTip(QStringLiteral(
        "把全部七类 Shell 关联注册表导出成 JSON 文件，存放在 exe 同级目录的 ContextMenuBackups 下。\n"
        "测量耗时会自动先做一次同样的备份。"));
    m_restoreBackupButton = new QPushButton(QIcon(QStringLiteral(":/Icon/codeeditor_undo.svg")),
        QStringLiteral("恢复备份"), m_pageToolbar);
    m_restoreBackupButton->setToolTip(QStringLiteral(
        "选择一份备份文件恢复右键菜单。恢复等于遗弃当前状态，因此恢复前会自动把当前状态另存一份。"));
    m_latencyButton = new QPushButton(QIcon(QStringLiteral(":/Icon/log_track.svg")),
        QStringLiteral("测量本页耗时"), m_pageToolbar);
    m_latencyButton->setToolTip(QStringLiteral(
        "测量当前分类对应右键菜单的耗时，并逐条算出各项的耗时贡献（差值法）。\n"
        "仅桌面右键菜单可程序化测量；测量会短暂弹出菜单并自动关闭，不注入鼠标键盘。"));
    m_latencyStopButton = new QPushButton(QStringLiteral("停止测量"), m_pageToolbar);
    m_latencyStopButton->setEnabled(false);
    m_latencyStopButton->setToolTip(QStringLiteral("请求中止当前测量，已测结果会保留"));
    // 只有桌面右键菜单能程序化测量：其余分类干脆不显示测量入口，不给用户假按钮。
    // 初次可见性由页签切换回调统一维护（默认停在 IE 右键菜单，属于不可测分类）。
    m_latencyButton->setVisible(false);
    m_latencyStopButton->setVisible(false);
    m_latencyStatusLabel = new QLabel(QStringLiteral("尚未测量。"), m_pageToolbar);
    // 测量结果可在剩余工具区换行，长状态不抬高整个专业页的最小宽度。
    m_latencyStatusLabel->setWordWrap(true);
    m_latencyStatusLabel->setMinimumWidth(0);
    m_latencyStatusLabel->setStyleSheet(
        QStringLiteral("QLabel{color:%1;}").arg(KswordTheme::TextSecondaryHex()));

    // 常规操作明确实底，测量的可见性、恢复确认和后台状态仍由业务维护。
    for (QPushButton* button : { m_backupButton, m_restoreBackupButton, m_latencyButton, m_latencyStopButton })
    {
        ks::ui::ApplyFlatButtonTheme(button, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
    }

    pageToolbarLayout->addWidget(m_backupButton);
    pageToolbarLayout->addWidget(m_restoreBackupButton);
    // 操作间距统一由工具条决定，不额外插入大空白。
    pageToolbarLayout->addWidget(m_latencyButton);
    pageToolbarLayout->addWidget(m_latencyStopButton);
    pageToolbarLayout->addWidget(m_latencyStatusLabel, 1);
    ks::ui::NormalizeToolbarRow(pageToolbarLayout);
    m_rootLayout->addWidget(m_pageToolbar);

    connect(m_backupButton, &QPushButton::clicked, this, [this]() {
        backupContextMenuToFile(true);
    });
    connect(m_restoreBackupButton, &QPushButton::clicked, this, [this]() {
        restoreContextMenuFromFile();
    });
    connect(m_latencyButton, &QPushButton::clicked, this, [this]() {
        startLatencyMeasurement();
    });
    connect(m_latencyStopButton, &QPushButton::clicked, this, [this]() {
        cancelLatencyMeasurement();
    });

    m_areaTabWidget = new QTabWidget(this);
    ks::ui::StyleJoinedResultTabs(m_areaTabWidget, m_pageToolbar);
    m_areaTabWidget->setObjectName(QStringLiteral("ksContextMenuCleanerAreaTabs"));
    m_rootLayout->addWidget(m_areaTabWidget, 1);

    createAreaPage(MenuArea::InternetExplorer);
    createAreaPage(MenuArea::Desktop);
    createAreaPage(MenuArea::File);
    createAreaPage(MenuArea::UrlBinding);
    createAreaPage(MenuArea::OpenWith);
    createAreaPage(MenuArea::FormatMenu);
    createAreaPage(MenuArea::ExplorerHome);

    // 页签按需加载：
    // - URL/格式菜单会扫描大量 Classes 子键，不在杂项页构造时一次性阻塞 UI；
    // - 用户首次切换到分类时枚举一次，之后仅由刷新按钮主动重扫。
    const std::array<MenuArea, 7> orderedAreas{
        MenuArea::InternetExplorer,
        MenuArea::Desktop,
        MenuArea::File,
        MenuArea::UrlBinding,
        MenuArea::OpenWith,
        MenuArea::FormatMenu,
        MenuArea::ExplorerHome
    };
    connect(
        m_areaTabWidget,
        &QTabWidget::currentChanged,
        this,
        [this, orderedAreas](const int tabIndex) {
            if (tabIndex < 0
                || tabIndex >= static_cast<int>(orderedAreas.size()))
            {
                return;
            }
            const MenuArea area = orderedAreas[static_cast<std::size_t>(tabIndex)];
            AreaWidgets* areaWidgets = widgetsForArea(area);
            if (areaWidgets != nullptr && !areaWidgets->hasLoaded && !areaWidgets->queryPending)
            {
                refreshArea(area);
            }

            if (areaWidgets != nullptr && (areaWidgets->tableBuilding || areaWidgets->pendingSnapshot.has_value()))
                areaWidgets->presentationTimer->start(0);

            // 测量入口只在该分类真的存在可触发的右键菜单时出现（当前仅桌面右键菜单可测）。
            const bool measurable = areaSupportsLatencyProbe(area);
            if (m_latencyButton != nullptr)
            {
                m_latencyButton->setVisible(measurable);
            }
            if (m_latencyStopButton != nullptr)
            {
                m_latencyStopButton->setVisible(measurable);
            }
        });
}

void ContextMenuCleanerTab::createAreaPage(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr)
    {
        return;
    }

    areaWidgets->page = new QWidget(m_areaTabWidget);
    areaWidgets->layout = new QVBoxLayout(areaWidgets->page);
    areaWidgets->layout->setContentsMargins(16, 16, 16, 16);
    areaWidgets->layout->setSpacing(12);
    // 每类 Shell 列表仍使用既有详情宿主，仅给专业工作区补齐无描边底面。
    ks::ui::StylePrimaryContentPanel(areaWidgets->page, false);

    areaWidgets->toolbarWidget = new QWidget(areaWidgets->page);
    QHBoxLayout* toolbarLayout = new QHBoxLayout(areaWidgets->toolbarWidget);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);
    areaWidgets->layout->addWidget(areaWidgets->toolbarWidget);

    areaWidgets->refreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), areaWidgets->toolbarWidget);
    areaWidgets->refreshButton->setToolTip(QStringLiteral("重新枚举当前分类的 Shell 关联注册表项目"));
    areaWidgets->deleteButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_terminate.svg")), QStringLiteral("删除选中"), areaWidgets->toolbarWidget);
    // 备份口径按分类不同，挂在删除按钮上比页首一段通用说明更贴近实际动作。
    areaWidgets->deleteButton->setToolTip(
        area == MenuArea::UrlBinding
            ? QStringLiteral("删除表格选中项对应的注册表子树或值。本分类删除前会自动备份，可用「恢复上次删除」还原；更改后通常需要重启 Explorer 或相关程序才会完全刷新。")
            : QStringLiteral("删除表格选中项对应的注册表子树或值。本分类不会自动备份；更改后通常需要重启 Explorer 或相关程序才会完全刷新。"));
    if (area == MenuArea::UrlBinding)
    {
        areaWidgets->restoreButton = new QPushButton(
            QIcon(QStringLiteral(":/Icon/codeeditor_undo.svg")),
            QStringLiteral("恢复上次删除"),
            areaWidgets->toolbarWidget);
        areaWidgets->restoreButton->setToolTip(QStringLiteral("恢复上一次 URL 绑定删除前自动保存的注册表树"));
    }
    areaWidgets->copyButton = new QPushButton(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制路径"), areaWidgets->toolbarWidget);
    areaWidgets->copyButton->setToolTip(QStringLiteral("复制选中项的注册表路径"));
    areaWidgets->filterEdit = new QLineEdit(areaWidgets->toolbarWidget);
    areaWidgets->filterEdit->setClearButtonEnabled(true);
    areaWidgets->filterEdit->setPlaceholderText(QStringLiteral("筛选：名称/显示名/命令/注册表路径/CLSID"));
    ks::ui::StyleSearchField(areaWidgets->filterEdit);

    ks::ui::ApplyFlatButtonTheme(areaWidgets->refreshButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);
    ks::ui::ApplyFlatButtonTheme(areaWidgets->deleteButton, ks::ui::FlatButtonTone::Danger,
        ks::ui::FlatButtonAppearance::Solid);
    if (areaWidgets->restoreButton != nullptr)
    {
        ks::ui::ApplyFlatButtonTheme(areaWidgets->restoreButton, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
    }
    ks::ui::ApplyFlatButtonTheme(areaWidgets->copyButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);

    toolbarLayout->addWidget(areaWidgets->refreshButton);
    toolbarLayout->addWidget(areaWidgets->deleteButton);
    if (areaWidgets->restoreButton != nullptr)
    {
        toolbarLayout->addWidget(areaWidgets->restoreButton);
    }
    toolbarLayout->addWidget(areaWidgets->copyButton);
    toolbarLayout->addWidget(areaWidgets->filterEdit, 1);
    ks::ui::NormalizeToolbarRow(toolbarLayout);

    // 表格与详情编辑器必须放进同一个竖直分隔器：
    // DetailLayoutHost 靠它识别“表格 + 详情”这一对控件，并接管四种详情布局。
    QSplitter* splitter = new QSplitter(Qt::Vertical, areaWidgets->page);
    areaWidgets->table = new ks::ui::VisibleTableWidget(splitter);
    // 七类关联页已有备份/恢复/删除工具，保留紧凑复制导出以免重复。
    ks::ui::SetTableActionBarMode(areaWidgets->table, ks::ui::TableActionBarMode::Compact);
    areaWidgets->table->setColumnCount(kColumnCount);
    areaWidgets->table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("名称"),
        QStringLiteral("显示名"),
        QStringLiteral("类型"),
        QStringLiteral("来源"),
        QStringLiteral("命令/处理器"),
        QStringLiteral("注册表位置"),
        QStringLiteral("状态"),
        QStringLiteral("详情"),
        QStringLiteral("耗时") });
    areaWidgets->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    areaWidgets->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    areaWidgets->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    areaWidgets->table->setAlternatingRowColors(true);
    areaWidgets->table->setContextMenuPolicy(Qt::CustomContextMenu);
    areaWidgets->table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    areaWidgets->table->setSortingEnabled(true);
    areaWidgets->table->verticalHeader()->setVisible(false);
    areaWidgets->table->horizontalHeader()->setStyleSheet(buildHeaderStyle());
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnName, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnDisplayName, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnKind, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnSource, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnCommandOrHandler, QHeaderView::Stretch);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnRegistryPath, QHeaderView::Stretch);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnStatus, QHeaderView::ResizeToContents);
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnDetail, QHeaderView::Stretch);
    // 耗时列：由"测量本页耗时"填充（逐条差值法），未测量时为空。
    areaWidgets->table->horizontalHeader()->setSectionResizeMode(kColumnLatency, QHeaderView::ResizeToContents);

    // 详情编辑器：只读，承载当前选中行的完整信息。
    // 这里只用既有字段拼文本，不新增表格列，也不改枚举逻辑。
    areaWidgets->detailEditor = new ks::ui::StructuredFieldView(splitter);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 2);
    areaWidgets->layout->addWidget(splitter, 1);

    // 注册进详情布局系统：四种布局（下方折叠 / 行内展开 / 独立窗口 / 右侧）由全局设置决定，
    // 本页只负责把选中行的文本写进 detailEditor，其余布局与展开状态全部由宿主维护。
    ks::ui::DetailLayoutRegistry::registerStructuredHost(
        areaWidgets->table,
        areaWidgets->detailEditor,
        areaWidgets->page);

    areaWidgets->statusLabel = new QLabel(QStringLiteral("尚未刷新。"), areaWidgets->page);
    areaWidgets->statusLabel->setWordWrap(true);
    areaWidgets->layout->addWidget(areaWidgets->statusLabel);

    connect(areaWidgets->refreshButton, &QPushButton::clicked, this, [this, area]() {
        refreshArea(area);
    });
    connect(areaWidgets->deleteButton, &QPushButton::clicked, this, [this, area]() {
        deleteSelectedEntries(area);
    });
    if (areaWidgets->restoreButton != nullptr)
    {
        connect(areaWidgets->restoreButton, &QPushButton::clicked, this, [this]() {
            restoreLastUrlBindingBackup();
        });
    }
    connect(areaWidgets->copyButton, &QPushButton::clicked, this, [this, area]() {
        copySelectedEntries(area);
    });
    areaWidgets->filterTimer = new QTimer(areaWidgets->page);
    areaWidgets->filterTimer->setSingleShot(true);
    areaWidgets->filterTimer->setInterval(80);
    connect(areaWidgets->filterEdit, &QLineEdit::textChanged, areaWidgets->filterTimer,
        qOverload<>(&QTimer::start));
    connect(areaWidgets->filterTimer, &QTimer::timeout, this, [this, area]() {
        applyAreaFilter(area);
    });
    connect(areaWidgets->table, &QTableWidget::customContextMenuRequested, this, [this, area](const QPoint& localPosition) {
        showAreaContextMenu(area, localPosition);
    });
    // 选中行变化 → 重写详情文本；详情宿主随后按当前布局方案把它镜像出去
    // （下方折叠展开该区、行内插入合成行、独立窗口刷新、右侧面板刷新）。
    connect(areaWidgets->table, &QTableWidget::itemSelectionChanged, this, [this, area]() {
        updateAreaDetail(area);
    });

    areaWidgets->queryOperation = new ks::ui::AsyncOperation(this);
    areaWidgets->queryOperation->setStateChangedCallback([this, area]()
    {
        AreaWidgets* widgets = widgetsForArea(area);
        if (!widgets->queryOperation->isInFlight() && widgets->queryPending)
        {
            widgets->queryPending = false;
            widgets->statusLabel->setText(ks::i18n::text(QStringLiteral("misc.async.receipt_missing"),
                QStringLiteral("后台任务未返回有效回执，请刷新状态。")));
        }
    });
    areaWidgets->presentationTimer = new QTimer(this);
    areaWidgets->presentationTimer->setSingleShot(true);
    connect(areaWidgets->presentationTimer, &QTimer::timeout, this,
        [this, area]() { processAreaPresentation(area); });
    m_navigationTabs.add(kContextMenuNavigation[static_cast<int>(area)], m_areaTabWidget,
        areaWidgets->page, QIcon(areaIconPath(area)));
}

QVector<ContextMenuCleanerTab::ContextMenuEntry> ContextMenuCleanerTab::enumerateAreaSnapshot(const MenuArea area)
{
    QVector<ContextMenuEntry> entries = enumerateEntriesForArea(area);
    QHash<QString, QPair<QString, QString>> clsidCache;
    for (ContextMenuEntry& entry : entries)
    {
        if (!entry.clsidText.isEmpty())
        {
            auto it = clsidCache.constFind(entry.clsidText);
            if (it == clsidCache.cend())
                it = clsidCache.insert(entry.clsidText,
                    qMakePair(queryClsidFriendlyName(entry.clsidText), queryClsidServerPath(entry.clsidText)));
            entry.clsidFriendlyName = it->first;
            entry.clsidServerPath = it->second;
        }
        entry.detailText = entry.details.toPlainText();
        entry.registryPathText = registryTargetPathText(entry.rootLabel, entry.subKeyPath,
            entry.deleteKind == DeleteKind::RegistryValue, entry.valueName);
        entry.searchText = QStringList{entry.itemName, entry.displayName, entry.entryKind,
            entry.sourceGroup, entry.commandOrHandler, entry.registryPathText,
            entry.statusText, entry.detailText, entry.clsidText}.join('\n').toLower();
    }
    return entries;
}

void ContextMenuCleanerTab::refreshArea(const MenuArea area)
{
    AreaWidgets* widgets = widgetsForArea(area);
    if (widgets == nullptr) return;
    if (shellBusy())
    {
        widgets->refreshDeferred = true;
        return;
    }
    widgets->refreshDeferred = false;
    const std::uint64_t generation = ++widgets->refreshGeneration;
    widgets->pendingSnapshot.reset();
    widgets->queryPending = true;
    widgets->statusLabel->setText(ks::i18n::text(QStringLiteral("misc.shell.refreshing"),
        QStringLiteral("正在刷新注册表快照…")));
    const auto serial = m_registrySerial;
    const auto accepted = widgets->queryOperation->submit<MenuArea, QVector<ContextMenuEntry>>(
        area,
        [serial](const MenuArea& requestedArea, const ks::ui::AsyncOperationToken& token)
        {
            std::lock_guard<std::mutex> lock(*serial);
            if (token.isCanceled()) return QVector<ContextMenuEntry>();
            return enumerateAreaSnapshot(requestedArea);
        },
        [this, area, generation](const QVector<ContextMenuEntry>& entries, std::uint64_t)
        {
            AreaWidgets* current = widgetsForArea(area);
            if (generation != current->refreshGeneration) return true; // Stale presentation is consumed, never left awaitingCommit.
            current->queryPending = false;
            applyAreaSnapshot(area, generation, entries);
            return true;
        },
        [this, area](std::exception_ptr)
        {
            AreaWidgets* current = widgetsForArea(area);
            current->queryPending = false;
            current->statusLabel->setText(ks::i18n::text(QStringLiteral("misc.async.receipt_missing"),
                QStringLiteral("后台任务未返回有效回执，请刷新状态。")));
        });
    if (accepted == 0) widgets->queryPending = false;
}

void ContextMenuCleanerTab::applyAreaSnapshot(const MenuArea area, const std::uint64_t generation,
    QVector<ContextMenuEntry> entries)
{
    AreaWidgets* widgets = widgetsForArea(area);
    if (generation != widgets->refreshGeneration) return;
    widgets->hasLoaded = true;
    if (shellBusy() || !widgets->page->isVisible())
    {
        widgets->pendingSnapshot = std::move(entries);
        widgets->pendingSnapshotGeneration = generation;
        return;
    }
    // Modal confirmation and table menus retain the exact target snapshot until they return.
    if (QApplication::activeModalWidget() != nullptr
        || ks::ui::IsTableUiCommitBlockedByContextMenu({widgets->table}))
    {
        widgets->pendingSnapshot = std::move(entries);
        widgets->pendingSnapshotGeneration = generation;
        widgets->presentationTimer->start(25);
        return;
    }
    widgets->entries = std::move(entries);
    widgets->hasLoaded = true;
    // Resetting source rows already clears all latency cells from the old snapshot.
    rebuildAreaTable(area);
    kLogEvent event;
    info << event << "[ContextMenuCleanerTab] 刷新分类完成, area="
        << areaTitle(area).toStdString() << ", count=" << widgets->entries.size() << eol;
}

void ContextMenuCleanerTab::flushDeferredAreaRefreshes()
{
    const std::array<MenuArea, 7> areas{MenuArea::InternetExplorer, MenuArea::Desktop,
        MenuArea::File, MenuArea::UrlBinding, MenuArea::OpenWith, MenuArea::FormatMenu,
        MenuArea::ExplorerHome};
    for (const MenuArea area : areas)
        if (widgetsForArea(area)->refreshDeferred) refreshArea(area);
}

void ContextMenuCleanerTab::rebuildAreaTable(const MenuArea area)
{
    AreaWidgets* widgets = widgetsForArea(area);
    if (widgets == nullptr || widgets->table == nullptr) return;
    widgets->filterTimer->stop();
    widgets->presentationTimer->stop();
    widgets->entryAnchors.clear();
    widgets->entryAnchors.resize(widgets->entries.size());
    widgets->searchTexts.clear();
    widgets->searchTexts.reserve(widgets->entries.size());
    ks::ui::DetailLayoutRegistry::prepareDataRebuild(widgets->detailEditor);
    widgets->table->setSortingEnabled(false);
    widgets->table->setRowCount(0);
    widgets->table->setRowCount(widgets->entries.size());
    widgets->tableBuilding = true;
    widgets->nextPresentationRow = 0;
    widgets->table->setEnabled(false);
    widgets->deleteButton->setEnabled(false);
    widgets->copyButton->setEnabled(false);
    if (currentArea() == area) m_latencyButton->setEnabled(false);
    processAreaPresentation(area);
}

void ContextMenuCleanerTab::processAreaPresentation(const MenuArea area)
{
    AreaWidgets* widgets = widgetsForArea(area);
    if (shellBusy()) return;
    if (widgets->pendingSnapshot.has_value())
    {
        QVector<ContextMenuEntry> entries = std::move(*widgets->pendingSnapshot);
        widgets->pendingSnapshot.reset();
        applyAreaSnapshot(area, widgets->pendingSnapshotGeneration, std::move(entries));
        return;
    }
    if (!widgets->tableBuilding || !widgets->page->isVisible()) return;
    if (QApplication::activeModalWidget() != nullptr
        || ks::ui::IsTableUiCommitBlockedByContextMenu({widgets->table}))
    {
        widgets->presentationTimer->start(25);
        return;
    }
    QElapsedTimer budget;
    budget.start();
    const int startRow = widgets->nextPresentationRow;
    while (widgets->nextPresentationRow < widgets->entries.size()
        && widgets->nextPresentationRow - startRow < 160
        && (widgets->nextPresentationRow == startRow || budget.elapsed() < 4))
    {
        const int row = widgets->nextPresentationRow++;
        const ContextMenuEntry& entry = widgets->entries.at(row);
        widgets->searchTexts.push_back(entry.searchText);
        const auto makeItem = [row](const QString& text)
        {
            auto* item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, row);
            item->setToolTip(text);
            return item;
        };
        auto* anchor = makeItem(entry.itemName);
        widgets->entryAnchors[row] = anchor;
        widgets->table->setItem(row, kColumnName, anchor);
        widgets->table->setItem(row, kColumnDisplayName, makeItem(entry.displayName));
        widgets->table->setItem(row, kColumnKind, makeItem(entry.entryKind));
        widgets->table->setItem(row, kColumnSource, makeItem(entry.sourceGroup));
        widgets->table->setItem(row, kColumnCommandOrHandler, makeItem(entry.commandOrHandler));
        widgets->table->setItem(row, kColumnRegistryPath, makeItem(entry.registryPathText));
        widgets->table->setItem(row, kColumnStatus, makeItem(entry.statusText));
        widgets->table->setItem(row, kColumnDetail, makeItem(entry.detailText));
    }
    if (widgets->nextPresentationRow < widgets->entries.size())
    {
        widgets->presentationTimer->start(0);
        return;
    }
    widgets->tableBuilding = false;
    widgets->table->setSortingEnabled(true);
    widgets->table->setEnabled(!shellBusy());
    widgets->deleteButton->setEnabled(!shellBusy());
    widgets->copyButton->setEnabled(!shellBusy());
    if (currentArea() == area) m_latencyButton->setEnabled(!shellBusy());
    applyAreaFilter(area);
    if (widgets->queryPending)
        widgets->statusLabel->setText(ks::i18n::text(QStringLiteral("misc.shell.refreshing"),
            QStringLiteral("正在刷新注册表快照…")));
    updateAreaDetail(area);
}

void ContextMenuCleanerTab::applyAreaFilter(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->table == nullptr || areaWidgets->tableBuilding)
    {
        return;
    }
    const QString filterText = areaWidgets->filterEdit != nullptr
        ? areaWidgets->filterEdit->text().trimmed().toLower() : QString();
    const QSignalBlocker tableSignals(areaWidgets->table);
    int visibleCount = 0;
    QSet<QPersistentModelIndex> excludedRows;
    for (int row = 0; row < areaWidgets->table->rowCount(); ++row)
    {
        QTableWidgetItem* anchor = areaWidgets->table->item(row, kColumnName);
        if (anchor == nullptr || !anchor->data(Qt::UserRole).isValid())
        {
            continue;
        }
        const int index = anchor->data(Qt::UserRole).toInt();
        const bool visible = index >= 0 && index < areaWidgets->searchTexts.size()
            && (filterText.isEmpty() || areaWidgets->searchTexts.at(index).contains(filterText));
        if (visible)
        {
            ++visibleCount;
        }
        else
        {
            excludedRows.insert(QPersistentModelIndex(areaWidgets->table->model()->index(row, 0)));
            areaWidgets->table->selectionModel()->select(
                areaWidgets->table->model()->index(row, 0),
                QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
        }
    }
    ks::ui::ApplyTableRowFilter(areaWidgets->table, excludedRows);
    if (areaWidgets->statusLabel != nullptr)
    {
        areaWidgets->statusLabel->setText(QStringLiteral("%1：共枚举 %2 项，当前显示 %3 项。")
            .arg(areaTitle(area)).arg(areaWidgets->entries.size()).arg(visibleCount));
    }
    const int currentRow = areaWidgets->table->currentRow();
    if (currentRow >= 0 && ks::ui::IsTableRowExcludedByFilter(areaWidgets->table, currentRow))
    {
        areaWidgets->table->setCurrentCell(-1, -1);
        updateAreaDetail(area);
    }
}

void ContextMenuCleanerTab::showAreaContextMenu(const MenuArea area, const QPoint& localPosition)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->table == nullptr)
    {
        return;
    }

    QMenu menu(areaWidgets->table);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* copyAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制注册表路径"));
    QAction* deleteAction = menu.addAction(QIcon(QStringLiteral(":/Icon/process_terminate.svg")), QStringLiteral("删除选中项"));

    const QVector<int> selectedIndexes = selectedEntryIndexes(area);
    bool hasDeleteableEntry = false;
    for (const int entryIndex : selectedIndexes)
    {
        if (entryIndex >= 0
            && entryIndex < areaWidgets->entries.size()
            && areaWidgets->entries.at(entryIndex).canDelete)
        {
            hasDeleteableEntry = true;
            break;
        }
    }
    copyAction->setEnabled(!selectedIndexes.isEmpty());
    deleteAction->setEnabled(hasDeleteableEntry);

    QAction* selectedAction = menu.exec(areaWidgets->table->viewport()->mapToGlobal(localPosition));
    if (selectedAction == copyAction)
    {
        copySelectedEntries(area);
    }
    else if (selectedAction == deleteAction)
    {
        deleteSelectedEntries(area);
    }
}

void ContextMenuCleanerTab::deleteSelectedEntries(const MenuArea area)
{
    const QPointer<ContextMenuCleanerTab> alive(this);
    settleShellTransaction();
    if (!alive || shellBusy()) return;
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->tableBuilding)
    {
        return;
    }

    const QVector<int> selectedIndexes = selectedEntryIndexes(area);
    if (selectedIndexes.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Shell 关联管理"), QStringLiteral("请先选择需要删除的注册表项目。"));
        return;
    }

    QStringList targetPaths;
    QVector<ContextMenuEntry> targets;
    int machineScopeTargetCount = 0;
    for (const int entryIndex : selectedIndexes)
    {
        if (entryIndex < 0 || entryIndex >= areaWidgets->entries.size())
        {
            continue;
        }
        const ContextMenuEntry& entry = areaWidgets->entries.at(entryIndex);
        if (!entry.canDelete)
        {
            continue;
        }
        targets.push_back(entry);
        if (entry.rootKey == HKEY_LOCAL_MACHINE)
        {
            ++machineScopeTargetCount;
        }
        targetPaths.push_back(registryTargetPathText(
            entry.rootLabel,
            entry.subKeyPath,
            entry.deleteKind == DeleteKind::RegistryValue,
            entry.valueName));
    }
    if (targets.isEmpty())
    {
        QMessageBox::information(
            this,
            QStringLiteral("Shell 关联管理"),
            QStringLiteral("选中项属于受保护的系统注册，当前页面不允许删除。"));
        return;
    }

    const QString previewText = targetPaths.mid(0, 8).join('\n');
    const QString moreText = targetPaths.size() > 8
        ? QStringLiteral("\n... 另有 %1 项").arg(targetPaths.size() - 8)
        : QString();
    const QString machineScopeWarning = machineScopeTargetCount > 0
        ? QStringLiteral("\n\n高风险警告：其中 %1 项位于 HKLM，修改会影响所有用户，错误删除可能使协议、应用入口或系统功能失效。KSword 不限制此操作，但只应在确认目标属于第三方软件时继续；恢复 HKLM 备份需要管理员权限。")
            .arg(machineScopeTargetCount)
        : QString();
    const QString confirmationText = area == MenuArea::UrlBinding
        ? QStringLiteral("将删除 %1 个 URL 绑定注册表子树。删除前会自动备份，可通过“恢复上次删除”还原。%2\n\n%3%4")
            .arg(targetPaths.size())
            .arg(machineScopeWarning)
            .arg(previewText)
            .arg(moreText)
        : QStringLiteral("将删除 %1 个注册表子树或值。此操作不会自动备份，删除后通常需要重启 Explorer 或相关程序才会完全生效。\n\n%2%3")
            .arg(targetPaths.size())
            .arg(previewText)
            .arg(moreText);
    if (!beginShellAction()) return;
    const auto confirmButton = QMessageBox::warning(this,
        QStringLiteral("确认删除注册表项目"), confirmationText,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!alive) return;
    if (confirmButton != QMessageBox::Yes) { endShellAction(); return; }
    submitShellTransaction([targets, area]() {
        ShellTransactionResult result;
        if (area == MenuArea::UrlBinding && !createUrlBindingBackup(targets, &result.error))
        { result.stage = ShellTransactionStage::BackupFailed; return result; } // No target mutation before a complete published backup.
        result.stage = ShellTransactionStage::Completed;
        for (const ContextMenuEntry& entry : targets)
        {
            QString errorText;
            const QString path = registryTargetPathText(entry.rootLabel, entry.subKeyPath,
                entry.deleteKind == DeleteKind::RegistryValue, entry.valueName);
            bool ok = entry.canDelete;
            if (area == MenuArea::UrlBinding) ok = ok && isUrlBindingDeletionAllowed(entry);
            if (ok)
                ok = entry.deleteKind == DeleteKind::RegistryValue
                    ? deleteRegistryValueWithView(entry.rootKey, entry.subKeyPath, entry.valueName,
                        entry.viewFlag, entry.cleanupOpenWithMru, &errorText)
                    : deleteRegistryTreeWithView(entry.rootKey, entry.subKeyPath, entry.viewFlag, &errorText);
            else errorText = QStringLiteral("执行时安全校验拒绝删除");
            if (ok) ++result.count;
            else result.failures.push_back(QStringLiteral("%1：%2").arg(path, errorText));
            kLogEvent event;
            if (ok) warn << event << "[ContextMenuCleanerTab] 删除 Shell 关联注册表项目成功, path="
                << path.toStdString() << eol;
            else err << event << "[ContextMenuCleanerTab] 删除 Shell 关联注册表项目失败, path="
                << path.toStdString() << ", error=" << errorText.toStdString() << eol;
        }
        result.ok = result.failures.isEmpty();
        return result;
    }, [this, area](const ShellTransactionResult& result) {
        const QPointer<ContextMenuCleanerTab> alive(this);
        if (result.stage == ShellTransactionStage::BackupFailed)
        {
            QMessageBox::critical(this, QStringLiteral("URL 绑定备份失败"),
                QStringLiteral("删除已取消，因为无法建立可恢复备份：\n\n%1").arg(result.error));
            return;
        }
        refreshArea(area);
        if (result.stage != ShellTransactionStage::Completed)
        {
            QMessageBox::warning(this, QStringLiteral("Shell 关联管理"),
                result.stage == ShellTransactionStage::NotAccepted
                    ? QStringLiteral("事务未被接受，未执行注册表修改。")
                    : QStringLiteral("后台事务未返回可确认的完成状态；可能已部分执行。请刷新后核对实际注册表：%1").arg(result.error));
            return;
        }
        if (result.failures.isEmpty())
            QMessageBox::information(this, QStringLiteral("Shell 关联管理"),
                QStringLiteral("已删除 %1 项。建议重启 Explorer 或相关程序后确认变化。").arg(result.count));
        else
        {
            const bool handled = ks::ui::promptForPrivilegeFailure(this,
                QStringLiteral("清理 Shell 关联注册表项目"), result.failures.first());
            if (!alive) return;
            if (!handled) QMessageBox::warning(this, QStringLiteral("Shell 关联管理"),
                QStringLiteral("成功删除 %1 项，失败 %2 项：\n\n%3")
                    .arg(result.count).arg(result.failures.size()).arg(result.failures.join('\n')));
        }
    });
}

void ContextMenuCleanerTab::copySelectedEntries(const MenuArea area) const
{
    const AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->tableBuilding)
    {
        return;
    }

    const QVector<int> selectedIndexes = selectedEntryIndexes(area);
    if (selectedIndexes.isEmpty())
    {
        QMessageBox::information(const_cast<ContextMenuCleanerTab*>(this), QStringLiteral("复制注册表路径"), QStringLiteral("请先选择需要复制的行。"));
        return;
    }

    QStringList lines;
    for (const int entryIndex : selectedIndexes)
    {
        if (entryIndex < 0 || entryIndex >= areaWidgets->entries.size())
        {
            continue;
        }
        const ContextMenuEntry& entry = areaWidgets->entries.at(entryIndex);
        lines.push_back(registryTargetPathText(
            entry.rootLabel,
            entry.subKeyPath,
            entry.deleteKind == DeleteKind::RegistryValue,
            entry.valueName));
    }

    if (QClipboard* clipboard = QApplication::clipboard())
    {
        clipboard->setText(lines.join('\n'));
    }
}


QVector<int> ContextMenuCleanerTab::selectedEntryIndexes(const MenuArea area) const
{
    const AreaWidgets* areaWidgets = widgetsForArea(area);
    QVector<int> indexes;
    if (areaWidgets == nullptr || areaWidgets->table == nullptr || areaWidgets->table->selectionModel() == nullptr)
    {
        return indexes;
    }

    const QModelIndexList selectedRows = areaWidgets->table->selectionModel()->selectedRows();
    for (const QModelIndex& modelIndex : selectedRows)
    {
        if (ks::ui::IsTableRowExcludedByFilter(areaWidgets->table, modelIndex.row()))
        {
            continue;
        }
        const QTableWidgetItem* item = areaWidgets->table->item(modelIndex.row(), kColumnName);
        if (item == nullptr)
        {
            continue;
        }
        const int entryIndex = item->data(Qt::UserRole).toInt();
        if (!indexes.contains(entryIndex))
        {
            indexes.push_back(entryIndex);
        }
    }
    return indexes;
}

ContextMenuCleanerTab::AreaWidgets* ContextMenuCleanerTab::widgetsForArea(const MenuArea area)
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return &m_ieWidgets;
    case MenuArea::Desktop:
        return &m_desktopWidgets;
    case MenuArea::File:
        return &m_fileWidgets;
    case MenuArea::UrlBinding:
        return &m_urlBindingWidgets;
    case MenuArea::OpenWith:
        return &m_openWithWidgets;
    case MenuArea::FormatMenu:
        return &m_formatMenuWidgets;
    case MenuArea::ExplorerHome:
        return &m_explorerHomeWidgets;
    }
    return &m_fileWidgets;
}

const ContextMenuCleanerTab::AreaWidgets* ContextMenuCleanerTab::widgetsForArea(const MenuArea area) const
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return &m_ieWidgets;
    case MenuArea::Desktop:
        return &m_desktopWidgets;
    case MenuArea::File:
        return &m_fileWidgets;
    case MenuArea::UrlBinding:
        return &m_urlBindingWidgets;
    case MenuArea::OpenWith:
        return &m_openWithWidgets;
    case MenuArea::FormatMenu:
        return &m_formatMenuWidgets;
    case MenuArea::ExplorerHome:
        return &m_explorerHomeWidgets;
    }
    return &m_fileWidgets;
}

QString ContextMenuCleanerTab::areaTitle(const MenuArea area)
{
    const int index = static_cast<int>(area);
    const int safeIndex = index >= 0 && index < static_cast<int>(std::size(kContextMenuNavigation))
        ? index : static_cast<int>(MenuArea::File);
    return QString::fromUtf8(kContextMenuNavigation[safeIndex].sourceTitle);
}

QString ContextMenuCleanerTab::areaIconPath(const MenuArea area)
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return QStringLiteral(":/Icon/process_list.svg");
    case MenuArea::Desktop:
        return QStringLiteral(":/Icon/desktop_switch.svg");
    case MenuArea::File:
        return QStringLiteral(":/Icon/process_open_folder.svg");
    case MenuArea::UrlBinding:
        return QStringLiteral(":/Icon/log_track.svg");
    case MenuArea::OpenWith:
        return QStringLiteral(":/Icon/process_open_folder.svg");
    case MenuArea::FormatMenu:
        return QStringLiteral(":/Icon/process_list.svg");
    case MenuArea::ExplorerHome:
        return QStringLiteral(":/Icon/desktop_switch.svg");
    }
    return QStringLiteral(":/Icon/process_open_folder.svg");
}

void ContextMenuCleanerTab::updateAreaDetail(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr ||
        areaWidgets->table == nullptr ||
        areaWidgets->detailEditor == nullptr)
    {
        return;
    }

    // 行 → 条目的映射沿用表格项 Qt::UserRole 里保存的 entries 下标：
    // 排序或筛选后 visual row 会变，但 UserRole 始终指向原始快照，因此这里不需要额外的对应表。
    const int currentRow = areaWidgets->table->currentRow();
    if (currentRow < 0)
    {
        applyAreaDetailPlaceholder(area);
        return;
    }

    const QTableWidgetItem* anchorItem = areaWidgets->table->item(currentRow, kColumnName);
    const int entryIndex = anchorItem != nullptr ? anchorItem->data(Qt::UserRole).toInt() : -1;
    if (entryIndex < 0 || entryIndex >= areaWidgets->entries.size())
    {
        applyAreaDetailPlaceholder(area);
        return;
    }

    // setText 在只读模式下按“程序生成的报告”处理：随语言切换重绘，也不会被误当成用户文件原文。
    // 详情文本一变，DetailLayoutHost 就会按当前布局方案把它镜像到下方折叠区 / 行内 / 独立窗口。
    areaWidgets->detailEditor->setDocument(buildEntryDetailDocument(areaWidgets->entries.at(entryIndex)));
}

void ContextMenuCleanerTab::applyAreaDetailPlaceholder(const MenuArea area)
{
    AreaWidgets* areaWidgets = widgetsForArea(area);
    if (areaWidgets == nullptr || areaWidgets->detailEditor == nullptr)
    {
        return;
    }

    // 未刷新或未选中任何行时的引导文本：说明这一页能给出哪些信息，避免详情区一片空白。
    // 整段写成一个字面量（不跨行拼接），这样 i18n 审计只需要登记一条源串。
    areaWidgets->detailEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("【%1 详情】\n\n在上方表格中选择一行，这里显示该条 Shell 关联的完整信息：注册表根键与精确位置、命令或 CLSID 处理器、状态标记与删除粒度。\n\n详情显示方式可在设置中切换：下方折叠（默认）、行内展开、独立窗口、右侧面板。").arg(areaTitle(area))));
}

ks::ui::FieldDocument ContextMenuCleanerTab::buildEntryDetailDocument(const ContextMenuEntry& entry)
{
    // 只用枚举阶段已经采集到的字段拼装文本，不新增数据来源，也不改表格列；
    // CLSID 友好名与服务器路径复用 Internal.h 里既有的 HKCR 查询 helper。
    ks::ui::FieldDocument lines;
    lines.note(QStringLiteral("【%1】").arg(
        entry.displayName.isEmpty() ? entry.itemName : entry.displayName));

    lines.field(QStringLiteral("分类"), QStringLiteral("%1").arg(areaTitle(entry.area)));
    lines.field(QStringLiteral("名称"), QStringLiteral("%1").arg(entry.itemName));
    lines.field(QStringLiteral("显示名"), QStringLiteral("%1").arg(entry.displayName));
    lines.field(QStringLiteral("类型"), QStringLiteral("%1").arg(entry.entryKind));
    lines.field(QStringLiteral("来源"), QStringLiteral("%1").arg(entry.sourceGroup));
    lines.field(QStringLiteral("根键"), QStringLiteral("%1").arg(entry.rootLabel));
    lines.field(QStringLiteral("注册表位置"), QStringLiteral("%1").arg(registryTargetPathText(
        entry.rootLabel,
        entry.subKeyPath,
        entry.deleteKind == DeleteKind::RegistryValue,
        entry.valueName)));
    lines.field(QStringLiteral("命令/处理器"), QStringLiteral("%1").arg(entry.commandOrHandler));
    lines.field(QStringLiteral("状态"), QStringLiteral("%1").arg(entry.statusText));

    if (!entry.clsidText.isEmpty())
    {

        lines.field(QStringLiteral("CLSID"), QStringLiteral("%1").arg(entry.clsidText));
        const QString& friendlyName = entry.clsidFriendlyName;
        if (!friendlyName.isEmpty())
        {
            lines.field(QStringLiteral("CLSID 名称"), QStringLiteral("%1").arg(friendlyName));
        }
        const QString& serverPath = entry.clsidServerPath;
        if (!serverPath.isEmpty())
        {
            lines.field(QStringLiteral("服务器"), QStringLiteral("%1").arg(serverPath));
        }
    }

    if (!entry.details.isEmpty())
    {

        lines.field(QStringLiteral("补充详情"), QStringLiteral(""));
        for (const auto& node : entry.details.nodes) lines.nodes.push_back(node);
    }


    lines.field(QStringLiteral("删除粒度"), QStringLiteral("%1").arg(
        entry.deleteKind == DeleteKind::RegistryValue
            ? QStringLiteral("仅删除注册表值 %1").arg(entry.valueName)
            : QStringLiteral("删除整棵子键树")));
    lines.field(QStringLiteral("本页可删除"), QStringLiteral("%1").arg(
        entry.canDelete
            ? QStringLiteral("是")
            : QStringLiteral("否（系统保护项或受策略限制）")));

    return lines;
}

bool ContextMenuCleanerTab::shellBusy() const
{
    return m_shellPromptActive || m_shellPending || m_latencyRunning.load()
        || (m_latencySession && m_latencySession->recoveryPending.load());
}

void ContextMenuCleanerTab::suspendShellQueries()
{
    const std::array<MenuArea, 7> areas{MenuArea::InternetExplorer, MenuArea::Desktop,
        MenuArea::File, MenuArea::UrlBinding, MenuArea::OpenWith, MenuArea::FormatMenu, MenuArea::ExplorerHome};
    for (const MenuArea area : areas)
    {
        auto* widgets = widgetsForArea(area);
        if (widgets->queryOperation->isInFlight() || widgets->pendingSnapshot || widgets->tableBuilding)
            widgets->refreshDeferred = true;
        widgets->queryPending = false;
        ++widgets->refreshGeneration;
        widgets->queryOperation->cancel();
        widgets->pendingSnapshot.reset();
        widgets->presentationTimer->stop();
    }
}

bool ContextMenuCleanerTab::beginShellAction()
{
    const QPointer<ContextMenuCleanerTab> alive(this);
    settleShellTransaction();
    if (!alive || shellBusy()) return false;
    m_shellPromptActive = true;
    suspendShellQueries();
    setLatencyUiRunning(m_latencyRunning.load());
    return true;
}

void ContextMenuCleanerTab::endShellAction()
{
    m_shellPromptActive = false;
    setLatencyUiRunning(m_latencyRunning.load());
}

void ContextMenuCleanerTab::settleShellTransaction()
{
    if (!m_shellPending || m_shellOperation->isInFlight()) return;
    ShellTransactionResult result;
    if (m_shellReceipt)
    {
        std::lock_guard<std::mutex> lock(m_shellReceipt->mutex);
        if (m_shellReceipt->result) result = *m_shellReceipt->result;
        else result.error = QStringLiteral("后台任务未返回有效回执，请刷新状态。");
    }
    else result.error = QStringLiteral("后台任务未返回有效回执，请刷新状态。");
    // Consume old state before any callback/modal can accept a new transaction.
    auto completed = std::move(m_shellCompletion);
    m_shellReceipt.reset();
    m_shellPending = false;
    m_shellPromptActive = false;
    setLatencyUiRunning(m_latencyRunning.load());
    if (completed) completed(result);
}

void ContextMenuCleanerTab::submitShellTransaction(
    std::function<ShellTransactionResult()> worker,
    std::function<void(const ShellTransactionResult&)> completed)
{
    auto receipt = std::make_shared<ShellTransactionReceipt>();
    const auto serial = m_registrySerial;
    m_shellPromptActive = false;
    m_shellPending = true;
    m_shellReceipt = receipt;
    m_shellCompletion = std::move(completed);
    setLatencyUiRunning(m_latencyRunning.load());
    const QPointer<ContextMenuCleanerTab> alive(this);
    std::uint64_t accepted = 0;
    bool submitReturned = false;
    try
    {
        accepted = m_shellOperation->submitTransaction<int, ShellTransactionResult>(0,
            [serial, receipt, worker = std::move(worker)](const int&) {
            ShellTransactionResult result;
            {
                std::lock_guard<std::mutex> lock(*serial);
                try { result = worker(); }
                catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
                catch (...) { result.error = QStringLiteral("后台任务未返回有效回执，请刷新状态。"); }
            }
            kLogEvent event;
            info << event << "[ContextMenuCleanerTab] accepted transaction receipt: success="
                << result.ok << ", count=" << result.count << ", path=" << result.path.toStdString()
                << ", error=" << result.error.toStdString() << ", failures="
                << result.failures.join('\n').toStdString() << eol;
            { std::lock_guard<std::mutex> lock(receipt->mutex); receipt->result = result; }
            return result;
        }, [](const ShellTransactionResult&, std::uint64_t) { return true; },
        [receipt](std::exception_ptr) {
            std::lock_guard<std::mutex> lock(receipt->mutex);
            ShellTransactionResult failed;
            failed.error = QStringLiteral("后台任务未返回有效回执，请刷新状态。");
            receipt->result = std::move(failed);
        });
        submitReturned = true;
    }
    catch (...) {}
    if (!alive) return;
    if (accepted == 0)
    {
        if (submitReturned && m_shellReceipt == receipt)
        {
            std::lock_guard<std::mutex> lock(receipt->mutex);
            ShellTransactionResult rejected;
            rejected.stage = ShellTransactionStage::NotAccepted;
            rejected.error = QStringLiteral("事务未被接受，未执行注册表修改。");
            receipt->result = std::move(rejected);
        }
        settleShellTransaction();
    }
}

} // namespace ks::misc

ks::ui::PageNavigationCatalog ks::misc::ContextMenuCleanerTab::navigationCatalog(const ContextMenuCleanerTab* existing)
{
    ks::ui::PageNavigationCatalog schema;
    for (const auto& definition : kContextMenuNavigation)
        schema.append(ks::ui::PageNavigationTabDescriptor(definition));
    return existing ? existing->m_navigationTabs.describe(std::move(schema)) : schema;
}
QWidget* ks::misc::ContextMenuCleanerTab::activateNavigationDescriptor(const QString& id)
{
    return m_navigationTabs.activate(id, navigationCatalog(this));
}
namespace { const bool kContextMenuNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<ks::misc::ContextMenuCleanerTab>(QStringLiteral("misc.context")); }
