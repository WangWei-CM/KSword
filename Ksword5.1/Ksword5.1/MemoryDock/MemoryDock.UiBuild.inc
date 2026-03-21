// ============================================================
// MemoryDock.UiBuild.inc
// 作用：承载构造/析构与 UI 结构初始化代码。
// ============================================================

// ============================================================
// MemoryDock.UiBuild.inc（由原 UiLifecycle 拆分）
// 作用：
// - 承载 MemoryDock 的构造/析构、UI 结构初始化、信号槽连接等生命周期逻辑。
// - 聚焦“界面与交互绑定”职责，避免与扫描算法、读写工具函数混杂。
// ============================================================

MemoryDock::MemoryDock(QWidget* parent)
    : QWidget(parent)
{
    // 记录构造起点日志：用于追踪内存页控件的生命周期。
    kLogEvent constructStartEvent;
    info << constructStartEvent
        << "[MemoryDock] 开始构造内存页面控件。"
        << eol;

    // 构造阶段按固定顺序执行，确保 UI 控件先创建再绑定信号。
    initializeUi();
    initializeConnections();
    initializeBookmarkRefreshTimer();
    refreshProcessList(false);
    updateStatusBarText();

    // 记录构造结束日志：确认初始化链路已执行完毕。
    kLogEvent constructFinishEvent;
    info << constructFinishEvent
        << "[MemoryDock] 构造完成，已初始化 UI、连接、定时器与进程列表。"
        << eol;
}

MemoryDock::~MemoryDock()
{
    // 析构开始日志：便于定位窗口关闭时的后台任务状态。
    kLogEvent destroyStartEvent;
    info << destroyStartEvent
        << "[MemoryDock] 开始析构，准备取消扫描并分离进程。"
        << eol;

    // 析构前先取消扫描，避免后台线程继续使用已销毁控件。
    cancelCurrentScan();
    detachProcess();

    // 析构完成日志：标记控件资源回收流程结束。
    kLogEvent destroyFinishEvent;
    info << destroyFinishEvent
        << "[MemoryDock] 析构完成。"
        << eol;
}

void MemoryDock::initializeUi()
{
    // 初始化根 UI 结构时输出日志，便于定位 UI 组件构造顺序问题。
    kLogEvent uiInitEvent;
    info << uiInitEvent
        << "[MemoryDock] initializeUi: 开始构建根布局。"
        << eol;

    // 根布局只做三件事：顶部工具栏、中部 Tab、底部状态栏。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(4);

    initializeToolbar();
    initializeTabs();
    initializeStatusBar();
}

void MemoryDock::initializeToolbar()
{
    // 记录工具栏初始化日志：明确顶部控件何时创建。
    kLogEvent toolbarInitEvent;
    info << toolbarInitEvent
        << "[MemoryDock] initializeToolbar: 创建进程工具栏控件。"
        << eol;

    // 顶部工具栏放在独立容器内，便于统一 margin 和 spacing。
    QWidget* toolbarContainer = new QWidget(this);
    m_toolbarLayout = new QHBoxLayout(toolbarContainer);
    m_toolbarLayout->setContentsMargins(6, 6, 6, 2);
    m_toolbarLayout->setSpacing(6);

    const QString buttonStyle = buildBlueButtonStyle();
    const QString comboStyle = buildBlueComboStyle();

    m_processCombo = new QComboBox(toolbarContainer);
    m_processCombo->setMinimumWidth(280);
    m_processCombo->setStyleSheet(comboStyle);
    m_processCombo->setToolTip("选择目标进程（进程名 + PID）。");

    m_attachButton = new QPushButton("附加", toolbarContainer);
    m_detachButton = new QPushButton("分离", toolbarContainer);
    m_refreshButton = new QPushButton("刷新", toolbarContainer);
    m_settingsButton = new QPushButton("设置", toolbarContainer);
    m_attachButton->setStyleSheet(buttonStyle);
    m_detachButton->setStyleSheet(buttonStyle);
    m_refreshButton->setStyleSheet(buttonStyle);
    m_settingsButton->setStyleSheet(buttonStyle);

    m_toolbarLayout->addWidget(new QLabel("进程:", toolbarContainer));
    m_toolbarLayout->addWidget(m_processCombo, 1);
    m_toolbarLayout->addWidget(m_attachButton);
    m_toolbarLayout->addWidget(m_detachButton);
    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_settingsButton);
    m_toolbarLayout->addStretch(1);

    m_rootLayout->addWidget(toolbarContainer);
}

void MemoryDock::initializeTabs()
{
    // 记录 Tab 初始化日志：便于排查某个页面未创建的问题。
    kLogEvent tabInitEvent;
    info << tabInitEvent
        << "[MemoryDock] initializeTabs: 开始创建 5 个功能页。"
        << eol;

    // 五个子页面统一由 QTabWidget 承载。
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setDocumentMode(true);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeProcessModuleTab();
    initializeMemoryRegionTab();
    initializeMemorySearchTab();
    initializeMemoryViewerTab();
    initializeBreakpointBookmarkTab();
}

void MemoryDock::initializeProcessModuleTab()
{
    // Tab1 初始化日志：记录“进程与模块”页的构建过程。
    kLogEvent tab1InitEvent;
    info << tab1InitEvent
        << "[MemoryDock] initializeProcessModuleTab: 构建进程与模块页面。"
        << eol;

    // Tab1：进程与模块。
    m_tabProcessModule = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabProcessModule);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    // 上下分割布局：上进程表，下模块表。
    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabProcessModule);

    QWidget* processPanel = new QWidget(splitter);
    QVBoxLayout* processLayout = new QVBoxLayout(processPanel);
    processLayout->setContentsMargins(0, 0, 0, 0);
    processLayout->setSpacing(4);
    processLayout->addWidget(new QLabel("进程列表（双击自动附加）", processPanel));

    m_processTable = new QTableWidget(processPanel);
    m_processTable->setColumnCount(5);
    m_processTable->setHorizontalHeaderLabels(QStringList{ "进程名", "PID", "会话ID", "CPU(可选)", "工作集" });
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTable->setSortingEnabled(true);
    m_processTable->setAlternatingRowColors(true);
    m_processTable->verticalHeader()->setVisible(false);
    m_processTable->verticalHeader()->setDefaultSectionSize(20);
    // 进程图标统一缩放到 16x16，确保行高可以保持更紧凑。
    m_processTable->setIconSize(QSize(16, 16));
    m_processTable->setColumnHidden(2, true);
    m_processTable->setColumnHidden(3, true);
    m_processTable->setColumnHidden(4, true);
    m_processTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_processTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_processTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_processTable->setShowGrid(true);
    processLayout->addWidget(m_processTable, 1);

    QWidget* modulePanel = new QWidget(splitter);
    QVBoxLayout* moduleLayout = new QVBoxLayout(modulePanel);
    moduleLayout->setContentsMargins(0, 0, 0, 0);
    moduleLayout->setSpacing(4);

    // 模块区域布局对齐 ProcessDetailWindow：刷新按钮 + 签名选项 + 状态 + 模块表。
    QHBoxLayout* moduleTopBarLayout = new QHBoxLayout();
    moduleTopBarLayout->setContentsMargins(0, 0, 0, 0);
    moduleTopBarLayout->setSpacing(8);

    m_moduleRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), "刷新模块", modulePanel);
    m_moduleRefreshButton->setStyleSheet(buildBlueButtonStyle());
    m_moduleSignatureCheck = new QCheckBox("刷新时校验签名", modulePanel);
    m_moduleSignatureCheck->setChecked(true);
    m_moduleSignatureCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }"
        "QCheckBox::indicator:checked { background:%1; border:1px solid %1; }")
        .arg(KswordTheme::PrimaryBlueHex));

    moduleTopBarLayout->addWidget(m_moduleRefreshButton);
    moduleTopBarLayout->addWidget(m_moduleSignatureCheck);

    m_moduleFilterEdit = new QLineEdit(modulePanel);
    m_moduleFilterEdit->setPlaceholderText("按模块路径过滤关键字");
    m_moduleFilterEdit->setStyleSheet(buildBlueInputStyle());
    moduleTopBarLayout->addWidget(m_moduleFilterEdit, 1);

    m_moduleStatusLabel = new QLabel("● 待刷新", modulePanel);
    m_moduleStatusLabel->setStyleSheet("color:#5F5F5F; font-weight:600;");
    moduleTopBarLayout->addWidget(m_moduleStatusLabel);
    moduleLayout->addLayout(moduleTopBarLayout);

    m_moduleTable = new QTreeWidget(modulePanel);
    m_moduleTable->setColumnCount(static_cast<int>(ModuleTreeColumn::Count));
    m_moduleTable->setHeaderLabels(ModuleTreeHeaders);
    m_moduleTable->setRootIsDecorated(false);
    m_moduleTable->setItemsExpandable(false);
    m_moduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_moduleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_moduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_moduleTable->setAlternatingRowColors(true);
    m_moduleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_moduleTable->setSortingEnabled(true);
    m_moduleTable->header()->setStyleSheet(buildBlueTableHeaderStyle());
    m_moduleTable->setColumnWidth(toModuleTreeColumnIndex(ModuleTreeColumn::Path), 460);
    m_moduleTable->setColumnWidth(toModuleTreeColumnIndex(ModuleTreeColumn::Size), 110);
    m_moduleTable->setColumnWidth(toModuleTreeColumnIndex(ModuleTreeColumn::Signature), 220);
    m_moduleTable->setColumnWidth(toModuleTreeColumnIndex(ModuleTreeColumn::EntryOffset), 120);
    m_moduleTable->setColumnWidth(toModuleTreeColumnIndex(ModuleTreeColumn::State), 100);
    m_moduleTable->setColumnWidth(toModuleTreeColumnIndex(ModuleTreeColumn::ThreadId), 180);
    moduleLayout->addWidget(m_moduleTable, 1);

    splitter->addWidget(processPanel);
    splitter->addWidget(modulePanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 5);

    tabLayout->addWidget(splitter, 1);
    m_tabWidget->addTab(m_tabProcessModule, "进程与模块");
}

void MemoryDock::initializeMemoryRegionTab()
{
    // Tab2 初始化日志：用于追踪区域页控件初始化时机。
    kLogEvent tab2InitEvent;
    info << tab2InitEvent
        << "[MemoryDock] initializeMemoryRegionTab: 构建内存区域页面。"
        << eol;

    // Tab2：内存区域。
    m_tabRegions = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabRegions);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(10);
    m_regionCommittedOnlyCheck = new QCheckBox("仅已提交(MEM_COMMIT)", m_tabRegions);
    m_regionImageOnlyCheck = new QCheckBox("仅映像(IMAGE)", m_tabRegions);
    m_regionReadableOnlyCheck = new QCheckBox("仅可读", m_tabRegions);
    m_regionCommittedOnlyCheck->setChecked(true);
    m_regionReadableOnlyCheck->setChecked(true);
    filterLayout->addWidget(m_regionCommittedOnlyCheck);
    filterLayout->addWidget(m_regionImageOnlyCheck);
    filterLayout->addWidget(m_regionReadableOnlyCheck);
    filterLayout->addStretch(1);
    tabLayout->addLayout(filterLayout);

    m_regionTable = new QTableWidget(m_tabRegions);
    m_regionTable->setColumnCount(6);
    m_regionTable->setHorizontalHeaderLabels(QStringList{
        "基址", "大小", "保护属性", "状态", "类型", "映射文件"
        });
    m_regionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_regionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_regionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_regionTable->setAlternatingRowColors(true);
    m_regionTable->setSortingEnabled(true);
    m_regionTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_regionTable->verticalHeader()->setVisible(false);
    m_regionTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_regionTable->horizontalHeader()->setStretchLastSection(true);
    tabLayout->addWidget(m_regionTable, 1);

    m_tabWidget->addTab(m_tabRegions, "内存区域");
}

void MemoryDock::initializeMemorySearchTab()
{
    // Tab3 初始化日志：用于扫描页控件异常时定位阶段。
    kLogEvent tab3InitEvent;
    info << tab3InitEvent
        << "[MemoryDock] initializeMemorySearchTab: 构建内存搜索页面。"
        << eol;

    // Tab3：内存搜索。
    m_tabSearch = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabSearch);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    const QString inputStyle = buildBlueInputStyle();
    const QString comboStyle = buildBlueComboStyle();
    const QString buttonStyle = buildBlueButtonStyle();

    // 搜索条件面板。
    QGroupBox* conditionGroup = new QGroupBox("搜索条件", m_tabSearch);
    QGridLayout* conditionLayout = new QGridLayout(conditionGroup);
    conditionLayout->setHorizontalSpacing(8);
    conditionLayout->setVerticalSpacing(6);

    m_searchTypeCombo = new QComboBox(conditionGroup);
    m_searchTypeCombo->addItem("字节", static_cast<int>(SearchValueType::Byte));
    m_searchTypeCombo->addItem("2字节", static_cast<int>(SearchValueType::Int16));
    m_searchTypeCombo->addItem("4字节", static_cast<int>(SearchValueType::Int32));
    m_searchTypeCombo->addItem("8字节", static_cast<int>(SearchValueType::Int64));
    m_searchTypeCombo->addItem("浮点数", static_cast<int>(SearchValueType::Float32));
    m_searchTypeCombo->addItem("双精度", static_cast<int>(SearchValueType::Float64));
    m_searchTypeCombo->addItem("字节数组(支持??)", static_cast<int>(SearchValueType::ByteArray));
    m_searchTypeCombo->addItem("ASCII字符串", static_cast<int>(SearchValueType::StringAscii));
    m_searchTypeCombo->addItem("Unicode字符串", static_cast<int>(SearchValueType::StringUnicode));
    m_searchTypeCombo->setStyleSheet(comboStyle);

    m_searchValueEdit = new QLineEdit(conditionGroup);
    m_searchValueEdit->setPlaceholderText("输入搜索值");
    m_searchValueEdit->setStyleSheet(inputStyle);

    m_searchRangeCombo = new QComboBox(conditionGroup);
    m_searchRangeCombo->addItem("整个内存");
    m_searchRangeCombo->addItem("自定义范围");
    m_searchRangeCombo->setStyleSheet(comboStyle);

    m_searchRangeStartEdit = new QLineEdit(conditionGroup);
    m_searchRangeEndEdit = new QLineEdit(conditionGroup);
    m_searchRangeStartEdit->setPlaceholderText("起始地址");
    m_searchRangeEndEdit->setPlaceholderText("结束地址");
    m_searchRangeStartEdit->setStyleSheet(inputStyle);
    m_searchRangeEndEdit->setStyleSheet(inputStyle);
    m_searchRangeStartEdit->setEnabled(false);
    m_searchRangeEndEdit->setEnabled(false);

    m_searchImageOnlyCheck = new QCheckBox("仅映像", conditionGroup);
    m_searchHeapOnlyCheck = new QCheckBox("仅堆(近似)", conditionGroup);
    m_searchStackOnlyCheck = new QCheckBox("仅栈(近似)", conditionGroup);

    m_firstScanButton = new QPushButton("首次扫描", conditionGroup);
    m_nextScanButton = new QPushButton("再次扫描", conditionGroup);
    m_resetScanButton = new QPushButton("重置", conditionGroup);
    m_cancelScanButton = new QPushButton("取消扫描", conditionGroup);
    m_firstScanButton->setStyleSheet(buttonStyle);
    m_nextScanButton->setStyleSheet(buttonStyle);
    m_resetScanButton->setStyleSheet(buttonStyle);
    m_cancelScanButton->setStyleSheet(buttonStyle);
    m_nextScanButton->setEnabled(false);
    m_cancelScanButton->setEnabled(false);

    conditionLayout->addWidget(new QLabel("数据类型", conditionGroup), 0, 0);
    conditionLayout->addWidget(m_searchTypeCombo, 0, 1);
    conditionLayout->addWidget(new QLabel("值", conditionGroup), 0, 2);
    conditionLayout->addWidget(m_searchValueEdit, 0, 3, 1, 3);
    conditionLayout->addWidget(new QLabel("范围", conditionGroup), 1, 0);
    conditionLayout->addWidget(m_searchRangeCombo, 1, 1);
    conditionLayout->addWidget(m_searchRangeStartEdit, 1, 2);
    conditionLayout->addWidget(m_searchRangeEndEdit, 1, 3);
    conditionLayout->addWidget(m_searchImageOnlyCheck, 1, 4);
    conditionLayout->addWidget(m_searchHeapOnlyCheck, 1, 5);
    conditionLayout->addWidget(m_searchStackOnlyCheck, 1, 6);
    conditionLayout->addWidget(m_firstScanButton, 2, 1);
    conditionLayout->addWidget(m_nextScanButton, 2, 2);
    conditionLayout->addWidget(m_resetScanButton, 2, 3);
    conditionLayout->addWidget(m_cancelScanButton, 2, 4);

    tabLayout->addWidget(conditionGroup);

    QGroupBox* compareGroup = new QGroupBox("再次扫描过滤", m_tabSearch);
    QHBoxLayout* compareLayout = new QHBoxLayout(compareGroup);
    compareLayout->setContentsMargins(8, 6, 8, 6);
    compareLayout->setSpacing(8);

    m_nextScanCompareCombo = new QComboBox(compareGroup);
    m_nextScanCompareCombo->addItem("等于", static_cast<int>(SearchCompareMode::Equal));
    m_nextScanCompareCombo->addItem("大于", static_cast<int>(SearchCompareMode::Greater));
    m_nextScanCompareCombo->addItem("小于", static_cast<int>(SearchCompareMode::Less));
    m_nextScanCompareCombo->addItem("介于", static_cast<int>(SearchCompareMode::Between));
    m_nextScanCompareCombo->addItem("变化", static_cast<int>(SearchCompareMode::Changed));
    m_nextScanCompareCombo->addItem("未变化", static_cast<int>(SearchCompareMode::Unchanged));
    m_nextScanCompareCombo->addItem("增加", static_cast<int>(SearchCompareMode::Increased));
    m_nextScanCompareCombo->addItem("减少", static_cast<int>(SearchCompareMode::Decreased));
    m_nextScanCompareCombo->setStyleSheet(comboStyle);

    m_nextScanValueEdit = new QLineEdit(compareGroup);
    m_nextScanValueBEdit = new QLineEdit(compareGroup);
    m_nextScanValueEdit->setPlaceholderText("值A");
    m_nextScanValueBEdit->setPlaceholderText("值B");
    m_nextScanValueEdit->setStyleSheet(inputStyle);
    m_nextScanValueBEdit->setStyleSheet(inputStyle);
    m_nextScanValueBEdit->setVisible(false);

    compareLayout->addWidget(new QLabel("条件", compareGroup));
    compareLayout->addWidget(m_nextScanCompareCombo);
    compareLayout->addWidget(new QLabel("值", compareGroup));
    compareLayout->addWidget(m_nextScanValueEdit, 1);
    compareLayout->addWidget(m_nextScanValueBEdit, 1);
    tabLayout->addWidget(compareGroup);

    m_searchResultTable = new QTableWidget(m_tabSearch);
    m_searchResultTable->setColumnCount(4);
    m_searchResultTable->setHorizontalHeaderLabels(QStringList{ "地址", "当前值", "前次值", "备注" });
    m_searchResultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_searchResultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_searchResultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_searchResultTable->setAlternatingRowColors(true);
    m_searchResultTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_searchResultTable->verticalHeader()->setVisible(false);
    m_searchResultTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_searchResultTable->horizontalHeader()->setStretchLastSection(true);
    tabLayout->addWidget(m_searchResultTable, 1);

    QHBoxLayout* progressLayout = new QHBoxLayout();
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);
    m_scanProgressBar = new QProgressBar(m_tabSearch);
    m_scanProgressBar->setRange(0, 100);
    m_scanStatusLabel = new QLabel("就绪", m_tabSearch);
    progressLayout->addWidget(m_scanProgressBar, 1);
    progressLayout->addWidget(m_scanStatusLabel);
    tabLayout->addLayout(progressLayout);

    m_tabWidget->addTab(m_tabSearch, "内存搜索");
}

void MemoryDock::initializeMemoryViewerTab()
{
    // Tab4 初始化日志：标记十六进制查看器控件创建。
    kLogEvent tab4InitEvent;
    info << tab4InitEvent
        << "[MemoryDock] initializeMemoryViewerTab: 构建内存查看器页面。"
        << eol;

    // Tab4：内存查看器。
    m_tabViewer = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabViewer);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* navLayout = new QHBoxLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(8);
    navLayout->addWidget(new QLabel("地址:", m_tabViewer));
    m_viewAddressEdit = new QLineEdit(m_tabViewer);
    m_viewAddressEdit->setPlaceholderText("输入地址后跳转");
    m_viewAddressEdit->setStyleSheet(buildBlueInputStyle());
    m_viewJumpButton = new QPushButton("跳转", m_tabViewer);
    m_viewJumpButton->setStyleSheet(buildBlueButtonStyle());
    m_viewProtectLabel = new QLabel("保护属性: -", m_tabViewer);
    navLayout->addWidget(m_viewAddressEdit, 1);
    navLayout->addWidget(m_viewJumpButton);
    navLayout->addWidget(m_viewProtectLabel);
    tabLayout->addLayout(navLayout);

    // 16进制表格：地址 + 16字节 + ASCII。
    m_hexTable = new QTableWidget(m_tabViewer);
    m_hexTable->setRowCount(kHexRowCount);
    m_hexTable->setColumnCount(kHexBytesPerRow + 2);
    QStringList headers;
    headers << "地址";
    for (int column = 0; column < kHexBytesPerRow; ++column)
    {
        headers << QString("%1").arg(column, 2, 16, QChar('0')).toUpper();
    }
    headers << "ASCII";
    m_hexTable->setHorizontalHeaderLabels(headers);
    m_hexTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_hexTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_hexTable->setAlternatingRowColors(true);
    m_hexTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_hexTable->verticalHeader()->setVisible(false);
    m_hexTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_hexTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_hexTable->horizontalHeader()->setStretchLastSection(true);
    m_hexTable->viewport()->installEventFilter(this);
    tabLayout->addWidget(m_hexTable, 1);

    m_viewerStatusLabel = new QLabel("未附加进程。", m_tabViewer);
    tabLayout->addWidget(m_viewerStatusLabel);

    m_tabWidget->addTab(m_tabViewer, "内存查看器");
}

void MemoryDock::initializeBreakpointBookmarkTab()
{
    // Tab5 初始化日志：记录断点与书签页开始构建。
    kLogEvent tab5InitEvent;
    info << tab5InitEvent
        << "[MemoryDock] initializeBreakpointBookmarkTab: 构建断点与书签页面。"
        << eol;

    // Tab5：断点与书签。
    m_tabBpBookmark = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabBpBookmark);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabBpBookmark);
    const QString buttonStyle = buildBlueButtonStyle();

    QWidget* breakpointPanel = new QWidget(splitter);
    QVBoxLayout* breakpointLayout = new QVBoxLayout(breakpointPanel);
    breakpointLayout->setContentsMargins(0, 0, 0, 0);
    breakpointLayout->setSpacing(4);

    QHBoxLayout* bpButtonLayout = new QHBoxLayout();
    bpButtonLayout->setContentsMargins(0, 0, 0, 0);
    bpButtonLayout->setSpacing(6);
    m_addBreakpointButton = new QPushButton("添加断点", breakpointPanel);
    m_removeBreakpointButton = new QPushButton("删除断点", breakpointPanel);
    m_toggleBreakpointButton = new QPushButton("启用/禁用", breakpointPanel);
    m_addBreakpointButton->setStyleSheet(buttonStyle);
    m_removeBreakpointButton->setStyleSheet(buttonStyle);
    m_toggleBreakpointButton->setStyleSheet(buttonStyle);
    bpButtonLayout->addWidget(m_addBreakpointButton);
    bpButtonLayout->addWidget(m_removeBreakpointButton);
    bpButtonLayout->addWidget(m_toggleBreakpointButton);
    bpButtonLayout->addStretch(1);
    breakpointLayout->addLayout(bpButtonLayout);

    m_breakpointTable = new QTableWidget(breakpointPanel);
    m_breakpointTable->setColumnCount(5);
    m_breakpointTable->setHorizontalHeaderLabels(QStringList{ "地址", "原字节", "状态", "命中次数", "描述" });
    m_breakpointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_breakpointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_breakpointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_breakpointTable->setAlternatingRowColors(true);
    m_breakpointTable->verticalHeader()->setVisible(false);
    m_breakpointTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_breakpointTable->horizontalHeader()->setStretchLastSection(true);
    breakpointLayout->addWidget(m_breakpointTable, 1);

    QWidget* bookmarkPanel = new QWidget(splitter);
    QVBoxLayout* bookmarkLayout = new QVBoxLayout(bookmarkPanel);
    bookmarkLayout->setContentsMargins(0, 0, 0, 0);
    bookmarkLayout->setSpacing(4);

    QHBoxLayout* bmButtonLayout = new QHBoxLayout();
    bmButtonLayout->setContentsMargins(0, 0, 0, 0);
    bmButtonLayout->setSpacing(6);
    m_addBookmarkButton = new QPushButton("添加书签", bookmarkPanel);
    m_removeBookmarkButton = new QPushButton("删除书签", bookmarkPanel);
    m_refreshBookmarkButton = new QPushButton("刷新值", bookmarkPanel);
    m_jumpBookmarkButton = new QPushButton("跳转", bookmarkPanel);
    m_addBookmarkButton->setStyleSheet(buttonStyle);
    m_removeBookmarkButton->setStyleSheet(buttonStyle);
    m_refreshBookmarkButton->setStyleSheet(buttonStyle);
    m_jumpBookmarkButton->setStyleSheet(buttonStyle);
    bmButtonLayout->addWidget(m_addBookmarkButton);
    bmButtonLayout->addWidget(m_removeBookmarkButton);
    bmButtonLayout->addWidget(m_refreshBookmarkButton);
    bmButtonLayout->addWidget(m_jumpBookmarkButton);
    bmButtonLayout->addStretch(1);
    bookmarkLayout->addLayout(bmButtonLayout);

    m_bookmarkTable = new QTableWidget(bookmarkPanel);
    m_bookmarkTable->setColumnCount(4);
    m_bookmarkTable->setHorizontalHeaderLabels(QStringList{ "地址", "当前值", "备注", "添加时间" });
    m_bookmarkTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bookmarkTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_bookmarkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bookmarkTable->setAlternatingRowColors(true);
    m_bookmarkTable->verticalHeader()->setVisible(false);
    m_bookmarkTable->horizontalHeader()->setStyleSheet(buildBlueTableHeaderStyle());
    m_bookmarkTable->horizontalHeader()->setStretchLastSection(true);
    bookmarkLayout->addWidget(m_bookmarkTable, 1);

    splitter->addWidget(breakpointPanel);
    splitter->addWidget(bookmarkPanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 5);
    tabLayout->addWidget(splitter, 1);

    m_tabWidget->addTab(m_tabBpBookmark, "断点与书签");
}

