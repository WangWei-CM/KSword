#include "ProcessDock.h"

#include "../theme.h"
#include "ProcessDetailWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QRunnable>
#include <QSlider>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <thread>
#include <unordered_set>

namespace
{
    // 列标题文本常量，索引与 ProcessDock::TableColumn 一一对应。
    const QStringList ProcessTableHeaders{
        "进程名",
        "PID",
        "CPU",
        "RAM",
        "DISK",
        "GPU",
        "Net",
        "数字签名",
        "路径",
        "父进程",
        "启动参数",
        "用户",
        "启动时间",
        "管理员"
    };

    // 常用图标路径常量（全部来自 qrc 的 /Icon 前缀资源）。
    constexpr const char* IconProcessMain = ":/Icon/process_main.svg";
    constexpr const char* IconRefresh = ":/Icon/process_refresh.svg";
    constexpr const char* IconTree = ":/Icon/process_tree.svg";
    constexpr const char* IconList = ":/Icon/process_list.svg";
    constexpr const char* IconStart = ":/Icon/process_start.svg";
    constexpr const char* IconPause = ":/Icon/process_pause.svg";

    // 默认按钮图标尺寸。
    constexpr QSize DefaultIconSize(16, 16);
    constexpr QSize CompactIconButtonSize(28, 28);

    // 当前 steady_clock 时间转 100ns（与 ks::process 差值计算规则保持一致）。
    std::uint64_t steadyNow100ns()
    {
        const auto nowDuration = std::chrono::steady_clock::now().time_since_epoch();
        const auto nowNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(nowDuration).count();
        return static_cast<std::uint64_t>(nowNanoseconds / 100);
    }

    // 从策略下拉索引映射到 ks::process 策略枚举。
    ks::process::ProcessEnumStrategy toStrategy(const int strategyIndex)
    {
        switch (strategyIndex)
        {
        case 0:
            return ks::process::ProcessEnumStrategy::SnapshotProcess32;
        case 1:
            return ks::process::ProcessEnumStrategy::NtQuerySystemInfo;
        default:
            return ks::process::ProcessEnumStrategy::Auto;
        }
    }

    // 策略枚举转可读文本：用于刷新状态标签与详细日志输出。
    const char* strategyToText(const ks::process::ProcessEnumStrategy strategy)
    {
        switch (strategy)
        {
        case ks::process::ProcessEnumStrategy::SnapshotProcess32:
            return "Toolhelp Snapshot + Process32First/Next";
        case ks::process::ProcessEnumStrategy::NtQuerySystemInfo:
            return "NtQuerySystemInformation";
        case ks::process::ProcessEnumStrategy::Auto:
            return "Auto";
        default:
            return "Unknown";
        }
    }

    // usageRatioToHighlightColor 作用：
    // - 按占用比例（0~1）返回主题蓝色透明高亮；
    // - 占用越高，alpha 越大，视觉上更“深”。
    QColor usageRatioToHighlightColor(double usageRatio)
    {
        usageRatio = std::clamp(usageRatio, 0.0, 1.0);
        const int alphaValue = static_cast<int>(24.0 + usageRatio * 146.0);
        QColor highlightColor = KswordTheme::PrimaryBlueColor;
        highlightColor.setAlpha(alphaValue);
        return highlightColor;
    }

    // 统一按钮蓝色样式，和现有主题风格保持一致。
    QString buildBlueButtonStyle(const bool iconOnlyButton)
    {
        // 图标按钮采用更紧凑 padding，避免出现多余空白。
        const QString paddingText = iconOnlyButton ? QStringLiteral("4px") : QStringLiteral("4px 10px");
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: #FFFFFF;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: %5;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(paddingText);
    }
}

ProcessDock::ProcessDock(QWidget* parent)
    : QWidget(parent)
{
    // 初始化硬件并发参数：至少按 1 核处理，避免除零。
    m_logicalCpuCount = std::max<std::uint32_t>(1, std::thread::hardware_concurrency());

    // 构造阶段按“UI -> 连接 -> 定时器 -> 首次刷新”顺序执行。
    initializeUi();
    initializeConnections();
    initializeTimer();
    requestAsyncRefresh(true);
}

void ProcessDock::initializeUi()
{
    // 根布局只容纳一个侧边栏 tab 控件。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    m_sideTabWidget = new QTabWidget(this);
    m_sideTabWidget->setTabPosition(QTabWidget::West);
    m_sideTabWidget->setDocumentMode(true);

    // “进程列表”页是本模块核心页面。
    m_processListPage = new QWidget(this);
    m_processPageLayout = new QVBoxLayout(m_processListPage);
    m_processPageLayout->setContentsMargins(6, 6, 6, 6);
    m_processPageLayout->setSpacing(6);

    // 初始化上方控制栏和下方表格。
    initializeTopControls();
    initializeProcessTable();

    m_rootLayout->addWidget(m_sideTabWidget);
}

void ProcessDock::initializeTopControls()
{
    m_controlLayout = new QHBoxLayout();
    m_controlLayout->setContentsMargins(0, 0, 0, 0);
    m_controlLayout->setSpacing(8);

    // 遍历策略下拉框：
    // 1) Toolhelp（CreateToolhelp32Snapshot + Process32First/Next）
    // 2) NtQuerySystemInformation
    // 3) Auto（优先 NtQuery，失败回退 Toolhelp）
    m_strategyCombo = new QComboBox(this);
    m_strategyCombo->addItem(QIcon(IconRefresh), "Toolhelp Snapshot + Process32First/Next");
    m_strategyCombo->addItem(QIcon(IconRefresh), "NtQuerySystemInformation");
    m_strategyCombo->addItem(QIcon(IconRefresh), "Auto");
    m_strategyCombo->setCurrentIndex(2);
    m_strategyCombo->setToolTip("指定进程遍历方案");

    // 树/列表切换按钮：按需求仅显示图标。
    m_treeToggleButton = new QPushButton(QIcon(IconTree), "", this);
    m_treeToggleButton->setIconSize(DefaultIconSize);
    m_treeToggleButton->setFixedSize(CompactIconButtonSize);
    m_treeToggleButton->setCheckable(true);
    m_treeToggleButton->setChecked(false);
    m_treeToggleButton->setToolTip("切换为树状视图");

    // 视图模式下拉框：默认监视视图。
    m_viewModeCombo = new QComboBox(this);
    m_viewModeCombo->addItem(QIcon(IconList), "监视视图");
    m_viewModeCombo->addItem(QIcon(IconProcessMain), "详细信息视图");
    m_viewModeCombo->setCurrentIndex(static_cast<int>(ViewMode::Monitor));
    m_viewModeCombo->setToolTip("切换监视视图 / 详细信息视图");

    // 开始/暂停按钮：按需求仅显示图标。
    m_startButton = new QPushButton(QIcon(IconStart), "", this);
    m_pauseButton = new QPushButton(QIcon(IconPause), "", this);
    m_startButton->setIconSize(DefaultIconSize);
    m_pauseButton->setIconSize(DefaultIconSize);
    m_startButton->setFixedSize(CompactIconButtonSize);
    m_pauseButton->setFixedSize(CompactIconButtonSize);
    m_startButton->setToolTip("开始周期性刷新进程列表");
    m_pauseButton->setToolTip("暂停周期性刷新进程列表");

    // 刷新周期滑块（秒）。
    m_refreshLabel = new QLabel("刷新间隔: 2 秒", this);
    m_refreshSlider = new QSlider(Qt::Horizontal, this);
    m_refreshSlider->setRange(1, 10);
    m_refreshSlider->setValue(2);
    m_refreshSlider->setFixedWidth(140);
    m_refreshSlider->setToolTip("设置几秒刷新一次");

    // 刷新状态标签：明确告诉用户当前是否在刷新，以及最后耗时。
    m_refreshStateLabel = new QLabel("● 空闲", this);
    m_refreshStateLabel->setStyleSheet(QStringLiteral("color:#4A4A4A; font-weight:600;"));
    m_refreshStateLabel->setToolTip("当前刷新状态");

    // 按钮统一蓝色风格（图标按钮版本）。
    const QString buttonStyle = buildBlueButtonStyle(true);
    m_treeToggleButton->setStyleSheet(buttonStyle);
    m_startButton->setStyleSheet(buttonStyle);
    m_pauseButton->setStyleSheet(buttonStyle);

    // 把控件放入控制栏。
    m_controlLayout->addWidget(m_strategyCombo);
    m_controlLayout->addWidget(m_treeToggleButton);
    m_controlLayout->addWidget(m_viewModeCombo);
    m_controlLayout->addWidget(m_startButton);
    m_controlLayout->addWidget(m_pauseButton);
    m_controlLayout->addStretch(1);
    m_controlLayout->addWidget(m_refreshLabel);
    m_controlLayout->addWidget(m_refreshSlider);
    m_controlLayout->addWidget(m_refreshStateLabel);

    m_processPageLayout->addLayout(m_controlLayout);
}

void ProcessDock::initializeProcessTable()
{
    m_processTable = new QTreeWidget(this);
    m_processTable->setColumnCount(static_cast<int>(TableColumn::Count));
    m_processTable->setHeaderLabels(ProcessTableHeaders);
    m_processTable->setRootIsDecorated(false);
    m_processTable->setItemsExpandable(false);
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setSortingEnabled(true);
    m_processTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTable->setAlternatingRowColors(true);

    // 表头支持拖动、右键显示/隐藏列。
    QHeaderView* headerView = m_processTable->header();
    headerView->setSectionsMovable(true);
    headerView->setStretchLastSection(false);
    headerView->setContextMenuPolicy(Qt::CustomContextMenu);
    headerView->setStyleSheet(QStringLiteral(
        "QHeaderView::section {"
        "  color: %1;"
        "  background: #FFFFFF;"
        "  border: 1px solid #E6E6E6;"
        "  padding: 4px;"
        "  font-weight: 600;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex));

    applyDefaultColumnWidths();
    applyViewMode(ViewMode::Monitor);
    m_processPageLayout->addWidget(m_processTable, 1);

    // 满足需求 3.1：侧边栏 Tab 中包含“进程列表”页签。
    m_sideTabWidget->addTab(m_processListPage, QIcon(IconProcessMain), "进程列表");
    m_sideTabWidget->setCurrentIndex(0);
}

void ProcessDock::initializeConnections()
{
    // 策略切换后立即强制刷新。
    connect(m_strategyCombo, &QComboBox::currentIndexChanged, this, [this]() {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 进程枚举策略切换为: "
            << strategyToText(toStrategy(m_strategyCombo->currentIndex()))
            << eol;
        requestAsyncRefresh(true);
    });

    // 树/列表切换：仅图标切换（不显示文字），并立即重建表格。
    connect(m_treeToggleButton, &QPushButton::toggled, this, [this](const bool checked) {
        if (checked)
        {
            m_treeToggleButton->setIcon(QIcon(IconList));
            m_treeToggleButton->setToolTip("切换为列表视图");
        }
        else
        {
            m_treeToggleButton->setIcon(QIcon(IconTree));
            m_treeToggleButton->setToolTip("切换为树状视图");
        }
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 树状显示开关变更, treeMode=" << (checked ? "true" : "false")
            << eol;
        rebuildTable();
    });

    // 视图模式切换：重置默认可见列。
    connect(m_viewModeCombo, &QComboBox::currentIndexChanged, this, [this](const int modeIndex) {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 视图模式切换, modeIndex=" << modeIndex
            << ", detailMode=" << (modeIndex == static_cast<int>(ViewMode::Detail) ? "true" : "false")
            << eol;
        applyViewMode(static_cast<ViewMode>(modeIndex));
        rebuildTable();
        requestAsyncRefresh(true);
    });

    // 开始/暂停监视：仅切换标记和定时器，不阻塞 UI。
    connect(m_startButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        info << logEvent << "[ProcessDock] 用户点击开始监视，恢复周期刷新。" << eol;
        m_monitoringEnabled = true;
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->start();
        }
        requestAsyncRefresh(true);
    });
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] 用户点击暂停监视，暂停周期刷新。" << eol;
        m_monitoringEnabled = false;
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->stop();
        }
        updateRefreshStateUi(false, "● 已暂停监视");
    });

    // 刷新间隔滑块：秒 -> 毫秒，动态应用到定时器。
    connect(m_refreshSlider, &QSlider::valueChanged, this, [this](const int seconds) {
        m_refreshLabel->setText(QString("刷新间隔: %1 秒").arg(seconds));
        if (m_refreshTimer != nullptr)
        {
            m_refreshTimer->setInterval(seconds * 1000);
        }

        kLogEvent logEvent;
        info << logEvent << "[ProcessDock] 刷新间隔变更为 " << seconds << " 秒。" << eol;
    });

    // 表格右键菜单。
    connect(m_processTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showTableContextMenu(localPosition);
    });

    // 表头右键菜单（列显示/隐藏）。
    connect(m_processTable->header(), &QHeaderView::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showHeaderContextMenu(localPosition);
    });
}

void ProcessDock::initializeTimer()
{
    // 周期刷新定时器：默认 2 秒。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(m_refreshSlider->value() * 1000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        requestAsyncRefresh(false);
    });
    m_refreshTimer->start();
}

void ProcessDock::updateRefreshStateUi(const bool refreshing, const QString& stateText)
{
    // 统一刷新状态标签更新逻辑，确保“刷新中/空闲”展示一致。
    if (m_refreshStateLabel == nullptr)
    {
        return;
    }

    if (refreshing)
    {
        m_refreshStateLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }
    else
    {
        m_refreshStateLabel->setStyleSheet(QStringLiteral("color:#2F7D32; font-weight:600;"));
    }
    m_refreshStateLabel->setText(stateText);
}

void ProcessDock::applyDefaultColumnWidths()
{
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Name), 280);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Pid), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Cpu), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Ram), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Disk), 95);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Gpu), 80);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Net), 95);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Signature), 100);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::Path), 280);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::ParentPid), 90);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::CommandLine), 320);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::User), 180);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::StartTime), 160);
    m_processTable->setColumnWidth(toColumnIndex(TableColumn::IsAdmin), 90);
}

void ProcessDock::applyViewMode(const ViewMode viewMode)
{
    // 先全部隐藏，再按视图打开目标列，保证状态可预测。
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
    {
        m_processTable->setColumnHidden(column, true);
    }

    // 监视视图：进程名 + PID + 性能计数器。
    if (viewMode == ViewMode::Monitor)
    {
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Name), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Pid), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Cpu), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Ram), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Disk), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Gpu), false);
        m_processTable->setColumnHidden(toColumnIndex(TableColumn::Net), false);
        return;
    }

    // 详细信息视图：全部列展示。
    for (int column = 0; column < static_cast<int>(TableColumn::Count); ++column)
    {
        m_processTable->setColumnHidden(column, false);
    }
}

void ProcessDock::requestAsyncRefresh(const bool forceRefresh)
{
    // 非强制刷新时，暂停监视或正在刷新则直接跳过。
    if (!forceRefresh)
    {
        if (!m_monitoringEnabled || m_refreshInProgress)
        {
            kLogEvent logEvent;
            dbg << logEvent
                << "[ProcessDock] 跳过非强制刷新, monitoringEnabled=" << (m_monitoringEnabled ? "true" : "false")
                << ", refreshInProgress=" << (m_refreshInProgress ? "true" : "false")
                << eol;
            return;
        }
    }

    // 强制刷新也要避免并发任务叠加。
    if (m_refreshInProgress)
    {
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDock] 跳过刷新：当前已有后台刷新任务在执行。" << eol;
        return;
    }
    m_refreshInProgress = true;

    // 创建并复用“进程刷新”进度任务，避免每轮刷新都新增新卡片。
    if (m_refreshProgressTaskPid <= 0)
    {
        m_refreshProgressTaskPid = kPro.add("进程列表刷新", "初始化刷新任务");
    }
    kPro.set(m_refreshProgressTaskPid, "准备刷新参数...", 0, 0.02f);

    // 复制当前缓存快照给后台线程，避免跨线程读写冲突。
    const int strategyIndex = m_strategyCombo->currentIndex();
    const bool detailModeEnabled = (currentViewMode() == ViewMode::Detail);
    const bool isFirstRefresh = m_cacheByIdentity.empty();
    const int staticDetailFillBudget =
        detailModeEnabled
        ? (isFirstRefresh ? 24 : 16)   // 详细视图允许更高预算，加速填充详情字段。
        : (isFirstRefresh ? 2 : 4);    // 监视视图优先速度，预算较低。
    const std::uint32_t cpuCount = m_logicalCpuCount;
    const auto previousCache = m_cacheByIdentity;
    const auto previousCounters = m_counterSampleByIdentity;

    // ticket 用于丢弃过期结果（防止乱序覆盖）。
    const std::uint64_t localTicket = ++m_refreshTicket;
    m_lastRefreshStartTime = std::chrono::steady_clock::now();
    QPointer<ProcessDock> guard(this);

    // 刷新前先更新 UI 状态与日志，给出明显“刷新中”提示。
    updateRefreshStateUi(
        true,
        QString("● 正在刷新... 策略=%1")
        .arg(QString::fromUtf8(strategyToText(toStrategy(strategyIndex)))));

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 刷新开始, ticket=" << localTicket
            << ", force=" << (forceRefresh ? "true" : "false")
            << ", strategy=" << strategyToText(toStrategy(strategyIndex))
            << ", detailMode=" << (detailModeEnabled ? "true" : "false")
            << ", staticBudget=" << staticDetailFillBudget
            << ", cacheSize=" << previousCache.size()
            << eol;
    }

    // QRunnable + 线程池：满足“异步刷新，不阻塞 GUI”。
    QRunnable* backgroundTask = QRunnable::create([
        guard,
        localTicket,
        strategyIndex,
        detailModeEnabled,
        staticDetailFillBudget,
        cpuCount,
        progressPid = m_refreshProgressTaskPid,
        previousCache,
        previousCounters]() mutable {
        const ProcessDock::RefreshResult refreshResult = ProcessDock::buildRefreshResult(
            strategyIndex,
            detailModeEnabled,
            staticDetailFillBudget,
            progressPid,
            previousCache,
            previousCounters,
            cpuCount);

        if (guard == nullptr)
        {
            return;
        }

        // 结果通过队列连接回主线程更新 UI。
        QMetaObject::invokeMethod(guard, [guard, localTicket, refreshResult]() {
            if (guard == nullptr)
            {
                return;
            }

            // 只接受最新 ticket 的结果，旧结果直接丢弃。
            if (localTicket < guard->m_refreshTicket)
            {
                guard->m_refreshInProgress = false;
                return;
            }

            guard->applyRefreshResult(refreshResult);
            guard->m_refreshInProgress = false;
        }, Qt::QueuedConnection);
    });

    backgroundTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(backgroundTask);
}

void ProcessDock::applyRefreshResult(const RefreshResult& refreshResult)
{
    // 计算主线程观测耗时，用于“刷新状态标签”和日志输出。
    const auto nowTime = std::chrono::steady_clock::now();
    const auto elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - m_lastRefreshStartTime).count());

    // 用新结果替换缓存，并重建表格。
    m_cacheByIdentity = refreshResult.nextCache;
    m_counterSampleByIdentity = refreshResult.nextCounters;

    // 把最新进程数据同步到已打开的详情窗口（若对应进程仍存在）。
    for (auto windowIt = m_detailWindowByIdentity.begin(); windowIt != m_detailWindowByIdentity.end();)
    {
        if (windowIt->second == nullptr)
        {
            windowIt = m_detailWindowByIdentity.erase(windowIt);
            continue;
        }

        const auto cacheIt = m_cacheByIdentity.find(windowIt->first);
        if (cacheIt != m_cacheByIdentity.end())
        {
            windowIt->second->updateBaseRecord(cacheIt->second.record);
        }
        ++windowIt;
    }

    rebuildTable();

    // 更新进度任务：本轮刷新完成后自动隐藏卡片。
    if (m_refreshProgressTaskPid > 0)
    {
        kPro.set(m_refreshProgressTaskPid, "刷新完成", 100, 1.0f);
    }

    // 刷新状态标签展示详细统计，明确告诉用户“刷新已完成”。
    updateRefreshStateUi(
        false,
        QString("● 刷新完成 %1 ms | 枚举:%2 新增:%3 退出:%4")
        .arg(elapsedMs)
        .arg(refreshResult.enumeratedCount)
        .arg(refreshResult.newProcessCount)
        .arg(refreshResult.exitedProcessCount));

    // 输出详细刷新日志，便于后续性能与正确性排查。
    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 刷新完成, elapsedMs(main)=" << elapsedMs
            << ", elapsedMs(worker)=" << refreshResult.workerElapsedMs
            << ", strategySelected=" << strategyToText(refreshResult.selectedStrategy)
            << ", strategyActual=" << strategyToText(refreshResult.actualStrategy)
            << ", enumerated=" << refreshResult.enumeratedCount
            << ", reused=" << refreshResult.reusedProcessCount
            << ", new=" << refreshResult.newProcessCount
            << ", exitedHold=" << refreshResult.exitedProcessCount
            << ", staticFilled=" << refreshResult.staticFilledCount
            << ", staticDeferred=" << refreshResult.staticDeferredCount
            << ", cacheNow=" << m_cacheByIdentity.size()
            << eol;
    }
}

ProcessDock::RefreshResult ProcessDock::buildRefreshResult(
    const int strategyIndex,
    const bool detailModeEnabled,
    const int staticDetailFillBudget,
    const int progressTaskPid,
    const std::unordered_map<std::string, CacheEntry>& previousCache,
    const std::unordered_map<std::string, ks::process::CounterSample>& previousCounters,
    const std::uint32_t logicalCpuCount)
{
    const auto workerStartTime = std::chrono::steady_clock::now();

    RefreshResult refreshResult;
    refreshResult.nextCache.clear();
    refreshResult.nextCounters.clear();
    refreshResult.selectedStrategyIndex = strategyIndex;
    refreshResult.selectedStrategy = toStrategy(strategyIndex);
    refreshResult.actualStrategy = refreshResult.selectedStrategy;
    refreshResult.detailModeEnabled = detailModeEnabled;

    // 进度条阶段 1：开始枚举。
    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在枚举进程列表...", 10, 0.10f);
    }

    const ks::process::ProcessEnumStrategy strategy = toStrategy(strategyIndex);
    std::vector<ks::process::ProcessRecord> latestProcessList = ks::process::EnumerateProcesses(
        strategy,
        &refreshResult.actualStrategy);
    const std::uint64_t sampleTick = steadyNow100ns();
    refreshResult.enumeratedCount = latestProcessList.size();

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "正在复用缓存并计算性能计数...", 25, 0.25f);
    }

    // 静态详情预算控制：
    // - 预算用于限制“路径/命令行/用户/签名”等慢操作，避免首轮刷新过慢；
    // - 监视模式预算较低，详细模式预算较高。
    int remainingStaticBudget = std::max(0, staticDetailFillBudget);
    std::size_t processIndex = 0;

    // 先处理当前仍存在的进程，按 identity 复用旧静态详情。
    for (ks::process::ProcessRecord& processRecord : latestProcessList)
    {
        ++processIndex;

        // 若 creationTime 未取到，仍可用 0 参与 key（稳定但区分度降低）。
        const std::string identityKey = ks::process::BuildProcessIdentityKey(
            processRecord.pid,
            processRecord.creationTime100ns);

        const auto oldCacheIt = previousCache.find(identityKey);
        bool needsStaticFill = false;
        bool includeSignatureCheck = false;
        if (oldCacheIt != previousCache.end())
        {
            // 复用规则：PID + 创建时间相同则复用旧静态字段（性能计数器除外）。
            const ks::process::ProcessRecord& oldRecord = oldCacheIt->second.record;
            if (processRecord.imagePath.empty()) processRecord.imagePath = oldRecord.imagePath;
            if (processRecord.commandLine.empty()) processRecord.commandLine = oldRecord.commandLine;
            if (processRecord.userName.empty()) processRecord.userName = oldRecord.userName;
            if (processRecord.signatureState.empty()) processRecord.signatureState = oldRecord.signatureState;
            if (processRecord.startTimeText.empty()) processRecord.startTimeText = oldRecord.startTimeText;
            processRecord.isAdmin = oldRecord.isAdmin;
            processRecord.staticDetailsReady = oldRecord.staticDetailsReady;
            ++refreshResult.reusedProcessCount;

            // 旧进程若静态字段还不完整，或签名仍 Pending，在预算允许时继续补齐。
            const bool signaturePending = (processRecord.signatureState.empty() || processRecord.signatureState == "Pending");
            needsStaticFill = !processRecord.staticDetailsReady || (detailModeEnabled && signaturePending);
            includeSignatureCheck = detailModeEnabled;
        }
        else
        {
            // 新出现进程：计数 + 依据预算决定是否补齐静态详情。
            ++refreshResult.newProcessCount;
            needsStaticFill = true;
            includeSignatureCheck = detailModeEnabled;
        }

        if (needsStaticFill)
        {
            if (remainingStaticBudget > 0)
            {
                // 预算内执行静态详情填充。监视模式会跳过签名，以提升首刷速度。
                ks::process::FillProcessStaticDetails(processRecord, includeSignatureCheck);
                --remainingStaticBudget;
                ++refreshResult.staticFilledCount;
            }
            else
            {
                // 超预算时延后慢操作：保持基础数据可见，并标记后续继续处理。
                if (processRecord.signatureState.empty())
                {
                    processRecord.signatureState = detailModeEnabled ? "Unknown" : "Pending";
                }
                ++refreshResult.staticDeferredCount;
            }
        }

        // 若当前策略未填动态计数器，则显式刷新一次。
        if (!processRecord.dynamicCountersReady)
        {
            ks::process::RefreshProcessDynamicCounters(processRecord);
        }

        // 计算 CPU/DISK 衍生计数，并写入下一轮样本。
        ks::process::CounterSample nextSample{};
        const auto oldCounterIt = previousCounters.find(identityKey);
        const ks::process::CounterSample* oldSample =
            (oldCounterIt == previousCounters.end()) ? nullptr : &oldCounterIt->second;
        ks::process::UpdateDerivedCounters(
            processRecord,
            oldSample,
            nextSample,
            logicalCpuCount,
            sampleTick);
        refreshResult.nextCounters[identityKey] = nextSample;

        CacheEntry cacheEntry{};
        cacheEntry.record = std::move(processRecord);
        cacheEntry.missingRounds = 0;
        cacheEntry.isNewInLatestRound = (oldCacheIt == previousCache.end());
        cacheEntry.isExitedInLatestRound = false;
        refreshResult.nextCache.emplace(identityKey, std::move(cacheEntry));

        // 进度条阶段 2：按处理进度更新（频率做了抽样，避免过度抖动）。
        if (progressTaskPid > 0 && (processIndex % 48 == 0 || processIndex == latestProcessList.size()))
        {
            const double ratio = latestProcessList.empty()
                ? 1.0
                : (static_cast<double>(processIndex) / static_cast<double>(latestProcessList.size()));
            const float progressValue = static_cast<float>(0.30 + ratio * 0.55); // 30% -> 85%
            kPro.set(progressTaskPid, "正在处理缓存与性能差值...", 55, progressValue);
        }
    }

    // 再处理退出进程：上一轮存在、本轮不存在，则保留显示 1 轮灰底。
    for (const auto& oldPair : previousCache)
    {
        if (refreshResult.nextCache.find(oldPair.first) != refreshResult.nextCache.end())
        {
            continue;
        }

        const CacheEntry& oldEntry = oldPair.second;
        if (oldEntry.missingRounds >= 1)
        {
            // 已经保留过一轮，本次彻底移除。
            continue;
        }

        CacheEntry exitedEntry = oldEntry;
        exitedEntry.missingRounds = oldEntry.missingRounds + 1;
        exitedEntry.isNewInLatestRound = false;
        exitedEntry.isExitedInLatestRound = true;
        refreshResult.nextCache.emplace(oldPair.first, std::move(exitedEntry));
        ++refreshResult.exitedProcessCount;

        const auto oldCounterIt = previousCounters.find(oldPair.first);
        if (oldCounterIt != previousCounters.end())
        {
            refreshResult.nextCounters.emplace(oldPair.first, oldCounterIt->second);
        }
    }

    if (progressTaskPid > 0)
    {
        kPro.set(progressTaskPid, "后台刷新结果构建完成，等待主线程应用...", 90, 0.90f);
    }

    refreshResult.workerElapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - workerStartTime).count());

    return refreshResult;
}

void ProcessDock::rebuildTable()
{
    // 记录用户当前排序列与顺序，解决“刷新后被重置为 PID 排序”的问题。
    const int previousSortColumn = m_processTable->header()->sortIndicatorSection();
    const Qt::SortOrder previousSortOrder = m_processTable->header()->sortIndicatorOrder();

    m_processTable->clear();

    // 树状模式下保持父子顺序，禁用自动排序。
    const bool enableSorting = !isTreeModeEnabled();
    m_processTable->setSortingEnabled(false);

    const std::vector<DisplayRow> displayRows = buildDisplayOrder();

    // 先预计算 RAM/DISK/NET 的本轮最大值，用于把绝对值映射成“占用比例高亮”。
    double maxRamMB = 0.0;
    double maxDiskMBps = 0.0;
    double maxNetKBps = 0.0;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr)
        {
            continue;
        }
        maxRamMB = std::max(maxRamMB, displayRow.record->ramMB);
        maxDiskMBps = std::max(maxDiskMBps, displayRow.record->diskMBps);
        maxNetKBps = std::max(maxNetKBps, displayRow.record->netKBps);
    }

    // applyUsageHighlight 作用：
    // - 对目标单元格按占用比例绘制主题蓝背景；
    // - 占用越高，背景越明显。
    const auto applyUsageHighlight = [this](QTreeWidgetItem* item, const int columnIndex, double usageRatio)
        {
            if (item == nullptr)
            {
                return;
            }
            usageRatio = std::clamp(usageRatio, 0.0, 1.0);
            const QColor usageColor = usageRatioToHighlightColor(usageRatio);
            item->setBackground(columnIndex, usageColor);

            // 高占用时将文本置白，保证可读性。
            if (usageRatio >= 0.70)
            {
                item->setForeground(columnIndex, QColor(255, 255, 255));
            }
        };

    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr)
        {
            continue;
        }

        QTreeWidgetItem* rowItem = new QTreeWidgetItem();
        const ks::process::ProcessRecord& processRecord = *displayRow.record;
        const std::string identityKey = ks::process::BuildProcessIdentityKey(
            processRecord.pid,
            processRecord.creationTime100ns);
        rowItem->setData(0, Qt::UserRole, QString::fromStdString(identityKey));

        // 每一列文本填充，Name 列附加图标。
        for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
        {
            const TableColumn column = static_cast<TableColumn>(columnIndex);
            rowItem->setText(columnIndex, formatColumnText(processRecord, column, displayRow.depth));
        }

        // 监视视图优先刷新速度：默认使用统一图标，避免首轮大量提取系统图标导致卡顿。
        if (currentViewMode() == ViewMode::Monitor)
        {
            rowItem->setIcon(toColumnIndex(TableColumn::Name), QIcon(":/Icon/process_main.svg"));
        }
        else
        {
            rowItem->setIcon(toColumnIndex(TableColumn::Name), resolveProcessIcon(processRecord));
        }

        // 新增进程绿色高亮；退出保留进程灰色高亮。
        if (displayRow.isNew)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, QColor(218, 255, 226));
            }
        }
        else if (displayRow.isExited)
        {
            for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
            {
                rowItem->setBackground(columnIndex, QColor(236, 236, 236));
                rowItem->setForeground(columnIndex, QColor(88, 88, 88));
            }
        }
        else
        {
            // 常规行按占用比例做主题色高亮（CPU/RAM/DISK/GPU/NET）。
            const double cpuUsageRatio = std::clamp(processRecord.cpuPercent / 100.0, 0.0, 1.0);
            const double ramUsageRatio = (maxRamMB > 0.0)
                ? std::clamp(processRecord.ramMB / maxRamMB, 0.0, 1.0)
                : 0.0;
            const double diskUsageRatio = (maxDiskMBps > 0.0)
                ? std::clamp(processRecord.diskMBps / maxDiskMBps, 0.0, 1.0)
                : 0.0;
            const double gpuUsageRatio = std::clamp(processRecord.gpuPercent / 100.0, 0.0, 1.0);
            const double netUsageRatio = (maxNetKBps > 0.0)
                ? std::clamp(processRecord.netKBps / maxNetKBps, 0.0, 1.0)
                : 0.0;

            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Cpu), cpuUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Ram), ramUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Disk), diskUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Gpu), gpuUsageRatio);
            applyUsageHighlight(rowItem, toColumnIndex(TableColumn::Net), netUsageRatio);
        }

        m_processTable->addTopLevelItem(rowItem);
    }

    m_processTable->setSortingEnabled(enableSorting);
    if (enableSorting)
    {
        // 恢复用户上一次排序选择，而不是强制回到 PID 升序。
        const int safeSortColumn =
            (previousSortColumn >= 0 && previousSortColumn < static_cast<int>(TableColumn::Count))
            ? previousSortColumn
            : toColumnIndex(TableColumn::Pid);
        m_processTable->header()->setSortIndicator(safeSortColumn, previousSortOrder);
        m_processTable->sortItems(safeSortColumn, previousSortOrder);
    }

    // 根据本轮数据刷新标题栏“占用总和”。
    updateUsageSummaryInHeader(displayRows);

    // 重建完成后输出一条细粒度日志，便于分析 UI 刷新开销与排序状态。
    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] rebuildTable 完成, rows=" << displayRows.size()
        << ", treeMode=" << (isTreeModeEnabled() ? "true" : "false")
        << ", sortingEnabled=" << (enableSorting ? "true" : "false")
        << ", sortColumn=" << (enableSorting ? m_processTable->header()->sortIndicatorSection() : -1)
        << eol;
}

void ProcessDock::updateUsageSummaryInHeader(const std::vector<DisplayRow>& displayRows)
{
    // 标题栏展示占用总和：对当前可见数据（排除退出保留行）做聚合。
    if (m_processTable == nullptr || m_processTable->headerItem() == nullptr)
    {
        return;
    }

    double totalCpuPercent = 0.0;
    double totalRamMB = 0.0;
    double totalDiskMBps = 0.0;
    double totalGpuPercent = 0.0;
    double totalNetKBps = 0.0;
    for (const DisplayRow& displayRow : displayRows)
    {
        if (displayRow.record == nullptr || displayRow.isExited)
        {
            continue;
        }
        totalCpuPercent += displayRow.record->cpuPercent;
        totalRamMB += displayRow.record->ramMB;
        totalDiskMBps += displayRow.record->diskMBps;
        totalGpuPercent += displayRow.record->gpuPercent;
        totalNetKBps += displayRow.record->netKBps;
    }

    // 非占用列保持原始列名；占用列追加 Σ 汇总值。
    QTreeWidgetItem* headerItem = m_processTable->headerItem();
    headerItem->setText(toColumnIndex(TableColumn::Name), ProcessTableHeaders.at(toColumnIndex(TableColumn::Name)));
    headerItem->setText(toColumnIndex(TableColumn::Pid), ProcessTableHeaders.at(toColumnIndex(TableColumn::Pid)));
    headerItem->setText(
        toColumnIndex(TableColumn::Cpu),
        QString("CPU Σ%1%").arg(totalCpuPercent, 0, 'f', 2));
    headerItem->setText(
        toColumnIndex(TableColumn::Ram),
        QString("RAM Σ%1 MB").arg(totalRamMB, 0, 'f', 1));
    headerItem->setText(
        toColumnIndex(TableColumn::Disk),
        QString("DISK Σ%1 MB/s").arg(totalDiskMBps, 0, 'f', 2));
    headerItem->setText(
        toColumnIndex(TableColumn::Gpu),
        QString("GPU Σ%1%").arg(totalGpuPercent, 0, 'f', 1));
    headerItem->setText(
        toColumnIndex(TableColumn::Net),
        QString("Net Σ%1 KB/s").arg(totalNetKBps, 0, 'f', 2));
    headerItem->setText(toColumnIndex(TableColumn::Signature), ProcessTableHeaders.at(toColumnIndex(TableColumn::Signature)));
    headerItem->setText(toColumnIndex(TableColumn::Path), ProcessTableHeaders.at(toColumnIndex(TableColumn::Path)));
    headerItem->setText(toColumnIndex(TableColumn::ParentPid), ProcessTableHeaders.at(toColumnIndex(TableColumn::ParentPid)));
    headerItem->setText(toColumnIndex(TableColumn::CommandLine), ProcessTableHeaders.at(toColumnIndex(TableColumn::CommandLine)));
    headerItem->setText(toColumnIndex(TableColumn::User), ProcessTableHeaders.at(toColumnIndex(TableColumn::User)));
    headerItem->setText(toColumnIndex(TableColumn::StartTime), ProcessTableHeaders.at(toColumnIndex(TableColumn::StartTime)));
    headerItem->setText(toColumnIndex(TableColumn::IsAdmin), ProcessTableHeaders.at(toColumnIndex(TableColumn::IsAdmin)));
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildDisplayOrder() const
{
    return isTreeModeEnabled() ? buildTreeDisplayOrder() : buildListDisplayOrder();
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildListDisplayOrder() const
{
    std::vector<DisplayRow> displayRows;
    displayRows.reserve(m_cacheByIdentity.size());

    for (const auto& cachePair : m_cacheByIdentity)
    {
        DisplayRow displayRow{};
        displayRow.record = const_cast<ks::process::ProcessRecord*>(&cachePair.second.record);
        displayRow.depth = 0;
        displayRow.isNew = cachePair.second.isNewInLatestRound;
        displayRow.isExited = cachePair.second.isExitedInLatestRound;
        displayRows.push_back(displayRow);
    }

    std::sort(displayRows.begin(), displayRows.end(), [](const DisplayRow& leftRow, const DisplayRow& rightRow) {
        if (leftRow.record == nullptr || rightRow.record == nullptr)
        {
            return false;
        }
        return leftRow.record->pid < rightRow.record->pid;
    });
    return displayRows;
}

std::vector<ProcessDock::DisplayRow> ProcessDock::buildTreeDisplayOrder() const
{
    // Step1：把缓存转换成便于处理的指针数组。
    struct Node
    {
        const std::string* identityKey = nullptr;
        const CacheEntry* cacheEntry = nullptr;
    };
    std::vector<Node> nodes;
    nodes.reserve(m_cacheByIdentity.size());
    for (const auto& cachePair : m_cacheByIdentity)
    {
        nodes.push_back(Node{ &cachePair.first, &cachePair.second });
    }

    // Step2：构建 parentPid -> child 节点列表。
    std::unordered_map<std::uint32_t, std::vector<Node>> childrenByParentPid;
    std::unordered_set<std::uint32_t> existingPidSet;
    for (const Node& node : nodes)
    {
        if (node.cacheEntry == nullptr)
        {
            continue;
        }
        existingPidSet.insert(node.cacheEntry->record.pid);
        childrenByParentPid[node.cacheEntry->record.parentPid].push_back(node);
    }

    // 子列表按 PID 排序，保证同层稳定顺序。
    for (auto& pair : childrenByParentPid)
    {
        auto& childNodes = pair.second;
        std::sort(childNodes.begin(), childNodes.end(), [](const Node& leftNode, const Node& rightNode) {
            return leftNode.cacheEntry->record.pid < rightNode.cacheEntry->record.pid;
        });
    }

    // Step3：找到根节点（父 PID 不存在或为 0）。
    std::vector<Node> rootNodes;
    for (const Node& node : nodes)
    {
        const std::uint32_t parentPid = node.cacheEntry->record.parentPid;
        if (parentPid == 0 || existingPidSet.find(parentPid) == existingPidSet.end())
        {
            rootNodes.push_back(node);
        }
    }
    std::sort(rootNodes.begin(), rootNodes.end(), [](const Node& leftNode, const Node& rightNode) {
        return leftNode.cacheEntry->record.pid < rightNode.cacheEntry->record.pid;
    });

    // Step4：DFS 生成“树状列表顺序 + 缩进深度”。
    std::vector<DisplayRow> displayRows;
    std::unordered_set<std::string> visitedIdentitySet;

    std::function<void(const Node&, int)> appendNode;
    appendNode = [&](const Node& node, const int depth)
        {
            if (node.identityKey == nullptr || node.cacheEntry == nullptr)
            {
                return;
            }
            if (visitedIdentitySet.find(*node.identityKey) != visitedIdentitySet.end())
            {
                return;
            }
            visitedIdentitySet.insert(*node.identityKey);

            DisplayRow displayRow{};
            displayRow.record = const_cast<ks::process::ProcessRecord*>(&node.cacheEntry->record);
            displayRow.depth = depth;
            displayRow.isNew = node.cacheEntry->isNewInLatestRound;
            displayRow.isExited = node.cacheEntry->isExitedInLatestRound;
            displayRows.push_back(displayRow);

            const auto childIt = childrenByParentPid.find(node.cacheEntry->record.pid);
            if (childIt == childrenByParentPid.end())
            {
                return;
            }
            for (const Node& childNode : childIt->second)
            {
                appendNode(childNode, depth + 1);
            }
        };

    for (const Node& rootNode : rootNodes)
    {
        appendNode(rootNode, 0);
    }

    // 兜底：若仍有未访问节点（极端 parent 环），直接平铺补入。
    for (const Node& node : nodes)
    {
        if (node.identityKey == nullptr || node.cacheEntry == nullptr)
        {
            continue;
        }
        if (visitedIdentitySet.find(*node.identityKey) != visitedIdentitySet.end())
        {
            continue;
        }
        DisplayRow fallbackRow{};
        fallbackRow.record = const_cast<ks::process::ProcessRecord*>(&node.cacheEntry->record);
        fallbackRow.depth = 0;
        fallbackRow.isNew = node.cacheEntry->isNewInLatestRound;
        fallbackRow.isExited = node.cacheEntry->isExitedInLatestRound;
        displayRows.push_back(fallbackRow);
    }

    return displayRows;
}

void ProcessDock::showTableContextMenu(const QPoint& localPosition)
{
    QTreeWidgetItem* clickedItem = m_processTable->itemAt(localPosition);
    if (clickedItem == nullptr)
    {
        return;
    }
    m_processTable->setCurrentItem(clickedItem);

    QMenu contextMenu(this);
    QAction* copyCellAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_copy_cell.svg"), "复制单元格");
    QAction* copyRowAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_copy_row.svg"), "复制行");
    contextMenu.addSeparator();

    // 结束进程二级菜单。
    QMenu* killSubMenu = contextMenu.addMenu(blueTintedIcon(":/Icon/process_terminate.svg"), "结束进程");
    QAction* taskkillAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "Taskkill");
    QAction* taskkillForceAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "Taskkill /f");
    QAction* terminateProcessAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "TerminateProcess");
    QAction* terminateThreadsAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "TerminateThread(全部线程)");
    QAction* injectInvalidShellcodeAction = killSubMenu->addAction(blueTintedIcon(":/Icon/process_terminate.svg"), "注入无效shellcode");

    QAction* suspendAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_suspend.svg"), "挂起进程");
    QAction* resumeAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_resume.svg"), "恢复进程");
    QAction* setCriticalAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_critical.svg"), "设为关键进程");
    QAction* clearCriticalAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_uncritical.svg"), "取消关键进程");
    QAction* openFolderAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_open_folder.svg"), "打开所在目录");

    // 优先级二级菜单。
    QMenu* prioritySubMenu = contextMenu.addMenu(blueTintedIcon(":/Icon/process_priority.svg"), "设置进程优先级");
    QAction* idlePriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Idle");
    QAction* belowNormalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Below Normal");
    QAction* normalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Normal");
    QAction* aboveNormalPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Above Normal");
    QAction* highPriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "High");
    QAction* realtimePriority = prioritySubMenu->addAction(blueTintedIcon(":/Icon/process_priority.svg"), "Realtime");
    idlePriority->setData(0);
    belowNormalPriority->setData(1);
    normalPriority->setData(2);
    aboveNormalPriority->setData(3);
    highPriority->setData(4);
    realtimePriority->setData(5);

    QAction* detailsAction = contextMenu.addAction(blueTintedIcon(":/Icon/process_details.svg"), "进程详细信息");

    QAction* selectedAction = contextMenu.exec(m_processTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 右键菜单执行动作: " << selectedAction->text().toStdString()
            << eol;
    }

    if (selectedAction == copyCellAction) { copyCurrentCell(); return; }
    if (selectedAction == copyRowAction) { copyCurrentRow(); return; }
    if (selectedAction == taskkillAction) { executeTaskKillAction(false); return; }
    if (selectedAction == taskkillForceAction) { executeTaskKillAction(true); return; }
    if (selectedAction == terminateProcessAction) { executeTerminateProcessAction(); return; }
    if (selectedAction == terminateThreadsAction) { executeTerminateThreadsAction(); return; }
    if (selectedAction == injectInvalidShellcodeAction) { executeInjectInvalidShellcodeAction(); return; }
    if (selectedAction == suspendAction) { executeSuspendAction(); return; }
    if (selectedAction == resumeAction) { executeResumeAction(); return; }
    if (selectedAction == setCriticalAction) { executeSetCriticalAction(true); return; }
    if (selectedAction == clearCriticalAction) { executeSetCriticalAction(false); return; }
    if (selectedAction == openFolderAction) { executeOpenFolderAction(); return; }
    if (selectedAction == detailsAction) { openProcessDetailsPlaceholder(); return; }
    if (selectedAction->parent() == prioritySubMenu)
    {
        executeSetPriorityAction(selectedAction->data().toInt());
    }
}

void ProcessDock::showHeaderContextMenu(const QPoint& localPosition)
{
    Q_UNUSED(localPosition);

    // 每列一个勾选动作，允许用户动态显示/隐藏。
    QMenu columnMenu(this);
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        QAction* toggleAction = columnMenu.addAction(ProcessTableHeaders.at(columnIndex));
        toggleAction->setCheckable(true);
        toggleAction->setChecked(!m_processTable->isColumnHidden(columnIndex));
        toggleAction->setData(columnIndex);
    }

    QAction* selectedAction = columnMenu.exec(QCursor::pos());
    if (selectedAction == nullptr)
    {
        return;
    }

    const int columnIndex = selectedAction->data().toInt();
    const bool shouldShow = selectedAction->isChecked();
    m_processTable->setColumnHidden(columnIndex, !shouldShow);

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 列显示状态变更, column=" << columnIndex
        << ", header=" << ProcessTableHeaders.value(columnIndex).toStdString()
        << ", visible=" << (shouldShow ? "true" : "false")
        << eol;
}

void ProcessDock::copyCurrentCell()
{
    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }

    const int currentColumn = m_processTable->currentColumn();
    if (currentColumn < 0)
    {
        return;
    }
    QApplication::clipboard()->setText(currentItem->text(currentColumn));

    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] 复制单元格, column=" << currentColumn
        << ", text=" << currentItem->text(currentColumn).toStdString()
        << eol;
}

void ProcessDock::copyCurrentRow()
{
    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return;
    }

    QStringList rowFields;
    rowFields.reserve(static_cast<int>(TableColumn::Count));
    for (int columnIndex = 0; columnIndex < static_cast<int>(TableColumn::Count); ++columnIndex)
    {
        rowFields.push_back(currentItem->text(columnIndex));
    }
    QApplication::clipboard()->setText(rowFields.join("\t"));

    kLogEvent logEvent;
    dbg << logEvent
        << "[ProcessDock] 复制整行, text=" << rowFields.join("\t").toStdString()
        << eol;
}

std::string ProcessDock::selectedIdentityKey() const
{
    QTreeWidgetItem* currentItem = m_processTable->currentItem();
    if (currentItem == nullptr)
    {
        return std::string();
    }
    return currentItem->data(0, Qt::UserRole).toString().toStdString();
}

ks::process::ProcessRecord* ProcessDock::selectedRecord()
{
    const std::string identityKey = selectedIdentityKey();
    if (identityKey.empty())
    {
        return nullptr;
    }

    auto cacheIt = m_cacheByIdentity.find(identityKey);
    if (cacheIt == m_cacheByIdentity.end())
    {
        return nullptr;
    }
    return &cacheIt->second.record;
}

QString ProcessDock::formatColumnText(const ks::process::ProcessRecord& processRecord, const TableColumn column, const int depth) const
{
    switch (column)
    {
    case TableColumn::Name:
        return QString::fromStdString(buildRulerPrefix(depth) + processRecord.processName);
    case TableColumn::Pid:
        return QString::number(processRecord.pid);
    case TableColumn::Cpu:
        // CPU 改为两位小数，避免低占用进程全部显示 0.0 的视觉误差。
        return QString::number(processRecord.cpuPercent, 'f', 2) + "%";
    case TableColumn::Ram:
        return QString::number(processRecord.ramMB, 'f', 1) + " MB";
    case TableColumn::Disk:
        return QString::number(processRecord.diskMBps, 'f', 2) + " MB/s";
    case TableColumn::Gpu:
        return QString::number(processRecord.gpuPercent, 'f', 1) + "%";
    case TableColumn::Net:
        return QString::number(processRecord.netKBps, 'f', 2) + " KB/s";
    case TableColumn::Signature:
        return QString::fromStdString(processRecord.signatureState.empty() ? "Unknown" : processRecord.signatureState);
    case TableColumn::Path:
        return QString::fromStdString(processRecord.imagePath);
    case TableColumn::ParentPid:
        return QString::number(processRecord.parentPid);
    case TableColumn::CommandLine:
        return QString::fromStdString(processRecord.commandLine);
    case TableColumn::User:
        return QString::fromStdString(processRecord.userName);
    case TableColumn::StartTime:
        return QString::fromStdString(processRecord.startTimeText);
    case TableColumn::IsAdmin:
        return processRecord.isAdmin ? "是" : "否";
    default:
        return QString();
    }
}

QIcon ProcessDock::resolveProcessIcon(const ks::process::ProcessRecord& processRecord)
{
    const QString pathText = QString::fromStdString(processRecord.imagePath);
    if (pathText.isEmpty())
    {
        return QIcon(":/Icon/process_main.svg");
    }

    auto iconIt = m_iconCacheByPath.find(pathText);
    if (iconIt != m_iconCacheByPath.end())
    {
        return iconIt.value();
    }

    QFileIconProvider iconProvider;
    QIcon processIcon = iconProvider.icon(QFileInfo(pathText));
    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }
    m_iconCacheByPath.insert(pathText, processIcon);
    return processIcon;
}

QIcon ProcessDock::blueTintedIcon(const char* iconPath, const QSize& iconSize) const
{
    QSvgRenderer renderer(QString::fromUtf8(iconPath));
    if (!renderer.isValid())
    {
        return QIcon(QString::fromUtf8(iconPath));
    }

    QPixmap iconPixmap(iconSize);
    iconPixmap.fill(Qt::transparent);

    QPainter painter(&iconPixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(iconPixmap.rect(), KswordTheme::PrimaryBlueColor);
    painter.end();
    return QIcon(iconPixmap);
}

void ProcessDock::showActionResultMessage(const QString& title, const bool actionOk, const std::string& detailText)
{
    const QString detail = QString::fromStdString(detailText.empty() ? "无附加信息" : detailText);
    if (actionOk)
    {
        QMessageBox::information(this, title, "操作成功。\n" + detail);
    }
    else
    {
        QMessageBox::warning(this, title, "操作失败。\n" + detail);
    }
}

std::string ProcessDock::buildRulerPrefix(const int depth)
{
    if (depth <= 0)
    {
        return std::string();
    }

    std::string prefixText;
    for (int index = 0; index < depth; ++index)
    {
        prefixText += (index + 1 == depth) ? "└─ " : "│  ";
    }
    return prefixText;
}

int ProcessDock::toColumnIndex(const TableColumn column)
{
    return static_cast<int>(column);
}

bool ProcessDock::isTreeModeEnabled() const
{
    return m_treeToggleButton != nullptr && m_treeToggleButton->isChecked();
}

ProcessDock::ViewMode ProcessDock::currentViewMode() const
{
    return static_cast<ViewMode>(m_viewModeCombo->currentIndex());
}

void ProcessDock::executeTaskKillAction(const bool forceKill)
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTaskKillAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::ExecuteTaskKill(processRecord->pid, forceKill, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] TaskKill action, pid=" << processRecord->pid
            << ", force=" << (forceKill ? "true" : "false")
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage(forceKill ? "Taskkill /f" : "Taskkill", actionOk, detailText);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeTerminateProcessAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTerminateProcessAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::TerminateProcessByWin32(processRecord->pid, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] TerminateProcess action, pid=" << processRecord->pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("TerminateProcess", actionOk, detailText);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeTerminateThreadsAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeTerminateThreadsAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::TerminateAllThreadsByPid(processRecord->pid, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] TerminateThreads action, pid=" << processRecord->pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("TerminateThread(全部线程)", actionOk, detailText);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeInjectInvalidShellcodeAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeInjectInvalidShellcodeAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::InjectInvalidShellcode(processRecord->pid, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? warn : err) << logEvent
            << "[ProcessDock] InjectInvalidShellcode action, pid=" << processRecord->pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("注入无效shellcode", actionOk, detailText);
    if (actionOk) requestAsyncRefresh(true);
}

void ProcessDock::executeSuspendAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSuspendAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::SuspendProcess(processRecord->pid, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] SuspendProcess action, pid=" << processRecord->pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("挂起进程", actionOk, detailText);
}

void ProcessDock::executeResumeAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeResumeAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::ResumeProcess(processRecord->pid, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] ResumeProcess action, pid=" << processRecord->pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("恢复进程", actionOk, detailText);
}

void ProcessDock::executeSetCriticalAction(const bool enableCritical)
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetCriticalAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::SetProcessCriticalFlag(processRecord->pid, enableCritical, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] SetCritical action, pid=" << processRecord->pid
            << ", enable=" << (enableCritical ? "true" : "false")
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage(enableCritical ? "设为关键进程" : "取消关键进程", actionOk, detailText);
}

void ProcessDock::executeSetPriorityAction(const int priorityActionId)
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeSetPriorityAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    ks::process::ProcessPriorityLevel priorityLevel = ks::process::ProcessPriorityLevel::Normal;
    switch (priorityActionId)
    {
    case 0: priorityLevel = ks::process::ProcessPriorityLevel::Idle; break;
    case 1: priorityLevel = ks::process::ProcessPriorityLevel::BelowNormal; break;
    case 2: priorityLevel = ks::process::ProcessPriorityLevel::Normal; break;
    case 3: priorityLevel = ks::process::ProcessPriorityLevel::AboveNormal; break;
    case 4: priorityLevel = ks::process::ProcessPriorityLevel::High; break;
    case 5: priorityLevel = ks::process::ProcessPriorityLevel::Realtime; break;
    default: break;
    }

    std::string detailText;
    const bool actionOk = ks::process::SetProcessPriority(processRecord->pid, priorityLevel, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] SetPriority action, pid=" << processRecord->pid
            << ", actionId=" << priorityActionId
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("设置进程优先级", actionOk, detailText);
}

void ProcessDock::executeOpenFolderAction()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] executeOpenFolderAction 被忽略：当前没有选中进程。" << eol;
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::OpenProcessFolder(processRecord->pid, &detailText);
    {
        kLogEvent logEvent;
        (actionOk ? info : err) << logEvent
            << "[ProcessDock] OpenFolder action, pid=" << processRecord->pid
            << ", ok=" << (actionOk ? "true" : "false")
            << ", detail=" << detailText
            << eol;
    }
    showActionResultMessage("打开所在目录", actionOk, detailText);
}

void ProcessDock::openProcessDetailsPlaceholder()
{
    ks::process::ProcessRecord* processRecord = selectedRecord();
    if (processRecord == nullptr)
    {
        kLogEvent logEvent;
        warn << logEvent << "[ProcessDock] 打开进程详细信息失败：当前没有选中进程。" << eol;
        QMessageBox::warning(this, "进程详细信息", "请先在表格中选中一个进程。");
        return;
    }

    // identityKey 用于“一进程一窗口”复用逻辑。
    const std::string identityKey = ks::process::BuildProcessIdentityKey(
        processRecord->pid,
        processRecord->creationTime100ns);

    auto existingWindowIt = m_detailWindowByIdentity.find(identityKey);
    if (existingWindowIt != m_detailWindowByIdentity.end() && existingWindowIt->second != nullptr)
    {
        existingWindowIt->second->updateBaseRecord(*processRecord);
        existingWindowIt->second->show();
        existingWindowIt->second->raise();
        existingWindowIt->second->activateWindow();

        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDock] 复用已存在进程详情窗口, pid=" << processRecord->pid
            << ", identity=" << identityKey
            << eol;
        return;
    }

    // 创建新的独立窗口（不属于 Docking System，可并行打开多个）。
    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(*processRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    m_detailWindowByIdentity[identityKey] = detailWindow;

    // 详情窗口销毁后，从缓存移除，防止悬空指针。
    connect(detailWindow, &QObject::destroyed, this, [this, identityKey]() {
        m_detailWindowByIdentity.erase(identityKey);
    });

    // “转到父进程”由详情窗口发信号到这里统一处理。
    connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
        openProcessDetailWindowByPid(parentPid);
    });

    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();

    kLogEvent logEvent;
    info << logEvent
        << "[ProcessDock] 创建新的进程详情窗口, pid=" << processRecord->pid
        << ", identity=" << identityKey
        << eol;
}

void ProcessDock::openProcessDetailWindowByPid(const std::uint32_t pid)
{
    // 优先从当前缓存中查找对应 PID（可避免额外系统调用）。
    for (const auto& cachePair : m_cacheByIdentity)
    {
        const ks::process::ProcessRecord& cacheRecord = cachePair.second.record;
        if (cacheRecord.pid != pid)
        {
            continue;
        }

        auto existingWindowIt = m_detailWindowByIdentity.find(cachePair.first);
        if (existingWindowIt != m_detailWindowByIdentity.end() && existingWindowIt->second != nullptr)
        {
            existingWindowIt->second->updateBaseRecord(cacheRecord);
            existingWindowIt->second->show();
            existingWindowIt->second->raise();
            existingWindowIt->second->activateWindow();
            return;
        }

        ProcessDetailWindow* detailWindow = new ProcessDetailWindow(cacheRecord, nullptr);
        detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_detailWindowByIdentity[cachePair.first] = detailWindow;
        connect(detailWindow, &QObject::destroyed, this, [this, identityKey = cachePair.first]() {
            m_detailWindowByIdentity.erase(identityKey);
        });
        connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
            openProcessDetailWindowByPid(parentPid);
        });
        detailWindow->show();
        detailWindow->raise();
        detailWindow->activateWindow();
        return;
    }

    // 缓存不存在时，尝试实时查询该 PID 的最小详情并打开窗口。
    ks::process::ProcessRecord queriedRecord{};
    if (!ks::process::QueryProcessStaticDetailByPid(pid, queriedRecord))
    {
        queriedRecord.pid = pid;
        queriedRecord.processName = ks::process::GetProcessNameByPID(pid);
    }
    if (queriedRecord.processName.empty())
    {
        queriedRecord.processName = "PID_" + std::to_string(pid);
    }

    const std::string identityKey = ks::process::BuildProcessIdentityKey(
        queriedRecord.pid,
        queriedRecord.creationTime100ns);

    ProcessDetailWindow* detailWindow = new ProcessDetailWindow(queriedRecord, nullptr);
    detailWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    m_detailWindowByIdentity[identityKey] = detailWindow;
    connect(detailWindow, &QObject::destroyed, this, [this, identityKey]() {
        m_detailWindowByIdentity.erase(identityKey);
    });
    connect(detailWindow, &ProcessDetailWindow::requestOpenProcessByPid, this, [this](const std::uint32_t parentPid) {
        openProcessDetailWindowByPid(parentPid);
    });
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();
}
