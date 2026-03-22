void DriverDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeOverviewTab();
    initializeOperateTab();
    initializeDebugOutputTab();
}

void DriverDock::initializeOverviewTab()
{
    m_overviewPage = new QWidget(m_tabWidget);
    m_overviewLayout = new QVBoxLayout(m_overviewPage);
    m_overviewLayout->setContentsMargins(4, 4, 4, 4);
    m_overviewLayout->setSpacing(6);

    m_overviewToolLayout = new QHBoxLayout();
    m_overviewToolLayout->setContentsMargins(0, 0, 0, 0);
    m_overviewToolLayout->setSpacing(6);

    m_refreshServiceButton = new QPushButton(m_overviewPage);
    m_refreshServiceButton->setIcon(QIcon(":/Icon/process_refresh.svg"));
    m_refreshServiceButton->setToolTip(QStringLiteral("刷新驱动服务列表"));
    m_refreshServiceButton->setFixedWidth(34);

    m_refreshModuleButton = new QPushButton(m_overviewPage);
    m_refreshModuleButton->setIcon(QIcon(":/Icon/process_details.svg"));
    m_refreshModuleButton->setToolTip(QStringLiteral("刷新已加载内核模块"));
    m_refreshModuleButton->setFixedWidth(34);

    m_serviceFilterEdit = new QLineEdit(m_overviewPage);
    m_serviceFilterEdit->setPlaceholderText(QStringLiteral("输入服务名/显示名/路径过滤"));
    m_serviceFilterEdit->setToolTip(QStringLiteral("支持按服务名、显示名、路径、描述模糊过滤"));

    m_overviewStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_overviewPage);
    m_overviewStatusLabel->setWordWrap(true);

    m_overviewToolLayout->addWidget(m_refreshServiceButton);
    m_overviewToolLayout->addWidget(m_refreshModuleButton);
    m_overviewToolLayout->addWidget(m_serviceFilterEdit, 1);
    m_overviewToolLayout->addWidget(m_overviewStatusLabel, 0);
    m_overviewLayout->addLayout(m_overviewToolLayout);

    m_overviewSplitter = new QSplitter(Qt::Vertical, m_overviewPage);
    m_overviewLayout->addWidget(m_overviewSplitter, 1);

    QWidget* serviceContainer = new QWidget(m_overviewSplitter);
    QVBoxLayout* serviceLayout = new QVBoxLayout(serviceContainer);
    serviceLayout->setContentsMargins(0, 0, 0, 0);
    serviceLayout->setSpacing(4);
    serviceLayout->addWidget(new QLabel(QStringLiteral("驱动服务（SCM）"), serviceContainer));

    m_serviceTable = new QTableWidget(serviceContainer);
    m_serviceTable->setColumnCount(7);
    m_serviceTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("服务名"),
        QStringLiteral("显示名"),
        QStringLiteral("状态"),
        QStringLiteral("启动类型"),
        QStringLiteral("错误控制"),
        QStringLiteral("镜像路径"),
        QStringLiteral("描述")
        });
    m_serviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_serviceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_serviceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_serviceTable->setAlternatingRowColors(true);
    m_serviceTable->verticalHeader()->setVisible(false);
    m_serviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_serviceTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_serviceTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    serviceLayout->addWidget(m_serviceTable, 1);

    QWidget* moduleContainer = new QWidget(m_overviewSplitter);
    QVBoxLayout* moduleLayout = new QVBoxLayout(moduleContainer);
    moduleLayout->setContentsMargins(0, 0, 0, 0);
    moduleLayout->setSpacing(4);
    moduleLayout->addWidget(new QLabel(QStringLiteral("已加载内核模块（EnumDeviceDrivers）"), moduleContainer));

    m_moduleTable = new QTableWidget(moduleContainer);
    m_moduleTable->setColumnCount(3);
    m_moduleTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("模块名"),
        QStringLiteral("基址"),
        QStringLiteral("映像路径")
        });
    m_moduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_moduleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_moduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_moduleTable->setAlternatingRowColors(true);
    m_moduleTable->verticalHeader()->setVisible(false);
    m_moduleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_moduleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    moduleLayout->addWidget(m_moduleTable, 1);

    m_overviewSplitter->addWidget(serviceContainer);
    m_overviewSplitter->addWidget(moduleContainer);
    m_overviewSplitter->setStretchFactor(0, 3);
    m_overviewSplitter->setStretchFactor(1, 2);

    m_tabWidget->addTab(m_overviewPage, QIcon(":/Icon/process_list.svg"), QStringLiteral("驱动概览"));
}

void DriverDock::initializeOperateTab()
{
    m_operatePage = new QWidget(m_tabWidget);
    m_operateLayout = new QVBoxLayout(m_operatePage);
    m_operateLayout->setContentsMargins(4, 4, 4, 4);
    m_operateLayout->setSpacing(6);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(8);
    formLayout->setVerticalSpacing(6);
    formLayout->setColumnStretch(1, 1);
    formLayout->setColumnStretch(3, 1);

    m_serviceNameEdit = new QLineEdit(m_operatePage);
    m_serviceNameEdit->setPlaceholderText(QStringLiteral("例如 MyDriver"));
    m_displayNameEdit = new QLineEdit(m_operatePage);
    m_displayNameEdit->setPlaceholderText(QStringLiteral("例如 My Driver Service"));
    m_binaryPathEdit = new QLineEdit(m_operatePage);
    m_binaryPathEdit->setPlaceholderText(QStringLiteral("例如 C:\\Windows\\System32\\drivers\\mydrv.sys"));
    m_descriptionEdit = new QLineEdit(m_operatePage);
    m_descriptionEdit->setPlaceholderText(QStringLiteral("描述（可选）"));

    m_browsePathButton = new QPushButton(m_operatePage);
    m_browsePathButton->setIcon(QIcon(":/Icon/process_open_folder.svg"));
    m_browsePathButton->setToolTip(QStringLiteral("浏览并选择 .sys 文件"));
    m_browsePathButton->setFixedWidth(34);

    m_startTypeCombo = new QComboBox(m_operatePage);
    m_startTypeCombo->addItem(QStringLiteral("引导启动（BOOT）"), static_cast<int>(SERVICE_BOOT_START));
    m_startTypeCombo->addItem(QStringLiteral("系统启动（SYSTEM）"), static_cast<int>(SERVICE_SYSTEM_START));
    m_startTypeCombo->addItem(QStringLiteral("自动启动（AUTO）"), static_cast<int>(SERVICE_AUTO_START));
    m_startTypeCombo->addItem(QStringLiteral("手动启动（DEMAND）"), static_cast<int>(SERVICE_DEMAND_START));
    m_startTypeCombo->addItem(QStringLiteral("禁用（DISABLED）"), static_cast<int>(SERVICE_DISABLED));

    m_errorControlCombo = new QComboBox(m_operatePage);
    m_errorControlCombo->addItem(QStringLiteral("忽略（IGNORE）"), static_cast<int>(SERVICE_ERROR_IGNORE));
    m_errorControlCombo->addItem(QStringLiteral("正常（NORMAL）"), static_cast<int>(SERVICE_ERROR_NORMAL));
    m_errorControlCombo->addItem(QStringLiteral("严重（SEVERE）"), static_cast<int>(SERVICE_ERROR_SEVERE));
    m_errorControlCombo->addItem(QStringLiteral("致命（CRITICAL）"), static_cast<int>(SERVICE_ERROR_CRITICAL));

    formLayout->addWidget(new QLabel(QStringLiteral("服务名:"), m_operatePage), 0, 0);
    formLayout->addWidget(m_serviceNameEdit, 0, 1);
    formLayout->addWidget(new QLabel(QStringLiteral("显示名:"), m_operatePage), 0, 2);
    formLayout->addWidget(m_displayNameEdit, 0, 3);
    formLayout->addWidget(new QLabel(QStringLiteral("驱动路径:"), m_operatePage), 1, 0);
    formLayout->addWidget(m_binaryPathEdit, 1, 1, 1, 2);
    formLayout->addWidget(m_browsePathButton, 1, 3);
    formLayout->addWidget(new QLabel(QStringLiteral("启动类型:"), m_operatePage), 2, 0);
    formLayout->addWidget(m_startTypeCombo, 2, 1);
    formLayout->addWidget(new QLabel(QStringLiteral("错误控制:"), m_operatePage), 2, 2);
    formLayout->addWidget(m_errorControlCombo, 2, 3);
    formLayout->addWidget(new QLabel(QStringLiteral("描述:"), m_operatePage), 3, 0);
    formLayout->addWidget(m_descriptionEdit, 3, 1, 1, 3);
    m_operateLayout->addLayout(formLayout);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(6);

    m_registerOrUpdateButton = new QPushButton(m_operatePage);
    m_registerOrUpdateButton->setIcon(QIcon(":/Icon/process_refresh.svg"));
    m_registerOrUpdateButton->setToolTip(QStringLiteral("注册新服务或更新现有服务"));
    m_registerOrUpdateButton->setFixedWidth(34);

    m_loadDriverButton = new QPushButton(m_operatePage);
    m_loadDriverButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_loadDriverButton->setToolTip(QStringLiteral("挂载（启动）驱动服务"));
    m_loadDriverButton->setFixedWidth(34);

    m_unloadDriverButton = new QPushButton(m_operatePage);
    m_unloadDriverButton->setIcon(QIcon(":/Icon/process_pause.svg"));
    m_unloadDriverButton->setToolTip(QStringLiteral("卸载（停止）驱动服务"));
    m_unloadDriverButton->setFixedWidth(34);

    m_deleteServiceButton = new QPushButton(m_operatePage);
    m_deleteServiceButton->setIcon(QIcon(":/Icon/process_uncritical.svg"));
    m_deleteServiceButton->setToolTip(QStringLiteral("删除驱动服务注册"));
    m_deleteServiceButton->setFixedWidth(34);

    m_refreshStateButton = new QPushButton(m_operatePage);
    m_refreshStateButton->setIcon(QIcon(":/Icon/process_details.svg"));
    m_refreshStateButton->setToolTip(QStringLiteral("查询当前服务状态"));
    m_refreshStateButton->setFixedWidth(34);

    actionLayout->addWidget(m_registerOrUpdateButton);
    actionLayout->addWidget(m_loadDriverButton);
    actionLayout->addWidget(m_unloadDriverButton);
    actionLayout->addWidget(m_deleteServiceButton);
    actionLayout->addWidget(m_refreshStateButton);
    actionLayout->addStretch(1);
    m_operateLayout->addLayout(actionLayout);

    m_operateLogOutput = new QPlainTextEdit(m_operatePage);
    m_operateLogOutput->setReadOnly(true);
    m_operateLogOutput->setMaximumBlockCount(1200);
    m_operateLogOutput->setPlaceholderText(QStringLiteral("驱动操作日志显示在这里。"));
    m_operateLayout->addWidget(m_operateLogOutput, 1);

    m_tabWidget->addTab(m_operatePage, QIcon(":/Icon/process_main.svg"), QStringLiteral("驱动操作"));
}

void DriverDock::initializeDebugOutputTab()
{
    m_debugOutputPage = new QWidget(m_tabWidget);
    m_debugOutputLayout = new QVBoxLayout(m_debugOutputPage);
    m_debugOutputLayout->setContentsMargins(4, 4, 4, 4);
    m_debugOutputLayout->setSpacing(6);

    QLabel* hintLabel = new QLabel(
        QStringLiteral("说明：调试输出使用 DBWIN 机制。该机制主要捕获 OutputDebugString，"
                       "驱动 DbgPrint 是否可见依赖系统调试配置。"),
        m_debugOutputPage);
    hintLabel->setWordWrap(true);
    m_debugOutputLayout->addWidget(hintLabel);

    m_debugToolLayout = new QHBoxLayout();
    m_debugToolLayout->setContentsMargins(0, 0, 0, 0);
    m_debugToolLayout->setSpacing(6);

    m_startCaptureButton = new QPushButton(m_debugOutputPage);
    m_startCaptureButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_startCaptureButton->setToolTip(QStringLiteral("启动调试输出捕获"));
    m_startCaptureButton->setFixedWidth(34);

    m_stopCaptureButton = new QPushButton(m_debugOutputPage);
    m_stopCaptureButton->setIcon(QIcon(":/Icon/process_pause.svg"));
    m_stopCaptureButton->setToolTip(QStringLiteral("停止调试输出捕获"));
    m_stopCaptureButton->setFixedWidth(34);

    m_clearDebugOutputButton = new QPushButton(m_debugOutputPage);
    m_clearDebugOutputButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_clearDebugOutputButton->setToolTip(QStringLiteral("清空调试输出"));
    m_clearDebugOutputButton->setFixedWidth(34);

    m_copyDebugOutputButton = new QPushButton(m_debugOutputPage);
    m_copyDebugOutputButton->setIcon(QIcon(":/Icon/log_copy.svg"));
    m_copyDebugOutputButton->setToolTip(QStringLiteral("复制全部调试输出"));
    m_copyDebugOutputButton->setFixedWidth(34);

    m_debugCaptureStatusLabel = new QLabel(QStringLiteral("状态：未启动"), m_debugOutputPage);
    m_debugCaptureStatusLabel->setWordWrap(true);

    m_debugToolLayout->addWidget(m_startCaptureButton);
    m_debugToolLayout->addWidget(m_stopCaptureButton);
    m_debugToolLayout->addWidget(m_clearDebugOutputButton);
    m_debugToolLayout->addWidget(m_copyDebugOutputButton);
    m_debugToolLayout->addWidget(m_debugCaptureStatusLabel, 1);
    m_debugOutputLayout->addLayout(m_debugToolLayout);

    m_debugOutputEdit = new QPlainTextEdit(m_debugOutputPage);
    m_debugOutputEdit->setReadOnly(true);
    m_debugOutputEdit->setMaximumBlockCount(2000);
    m_debugOutputEdit->setPlaceholderText(QStringLiteral("调试输出会实时显示在这里。"));
    m_debugOutputLayout->addWidget(m_debugOutputEdit, 1);

    m_tabWidget->addTab(m_debugOutputPage, QIcon(":/Icon/log_track.svg"), QStringLiteral("调试输出"));
}

void DriverDock::initializeConnections()
{
    // 概览页：刷新与过滤连接。
    connect(m_refreshServiceButton, &QPushButton::clicked, this, [this]()
        {
            refreshDriverServiceRecords();
        });
    connect(m_refreshModuleButton, &QPushButton::clicked, this, [this]()
        {
            refreshLoadedKernelModuleRecords();
        });
    connect(m_serviceFilterEdit, &QLineEdit::textChanged, this, [this](const QString&)
        {
            rebuildDriverServiceTableByFilter();
        });

    // 服务列表：选择变更后回填操作页。
    connect(m_serviceTable, &QTableWidget::itemSelectionChanged, this, [this]()
        {
            syncOperateFormBySelectedService();
        });
    connect(m_serviceTable, &QTableWidget::cellDoubleClicked, this, [this](const int, const int)
        {
            syncOperateFormBySelectedService();
            if (m_tabWidget != nullptr && m_operatePage != nullptr)
            {
                m_tabWidget->setCurrentWidget(m_operatePage);
            }
        });

    // 操作页：浏览路径、注册更新、挂载卸载、删除、状态查询。
    connect(m_browsePathButton, &QPushButton::clicked, this, [this]()
        {
            const QString filePath = QFileDialog::getOpenFileName(
                this,
                QStringLiteral("选择驱动文件"),
                QString(),
                QStringLiteral("Driver Files (*.sys);;All Files (*.*)"));
            if (!filePath.isEmpty() && m_binaryPathEdit != nullptr)
            {
                m_binaryPathEdit->setText(filePath);
            }
        });
    connect(m_registerOrUpdateButton, &QPushButton::clicked, this, [this]()
        {
            registerOrUpdateDriverService();
        });
    connect(m_loadDriverButton, &QPushButton::clicked, this, [this]()
        {
            loadSelectedDriverService();
        });
    connect(m_unloadDriverButton, &QPushButton::clicked, this, [this]()
        {
            unloadSelectedDriverService();
        });
    connect(m_deleteServiceButton, &QPushButton::clicked, this, [this]()
        {
            deleteSelectedDriverService();
        });
    connect(m_refreshStateButton, &QPushButton::clicked, this, [this]()
        {
            refreshSelectedServiceStateToForm();
        });

    // 调试输出：启动、停止、清空、复制。
    connect(m_startCaptureButton, &QPushButton::clicked, this, [this]()
        {
            startDebugOutputCapture();
        });
    connect(m_stopCaptureButton, &QPushButton::clicked, this, [this]()
        {
            stopDebugOutputCapture();
        });
    connect(m_clearDebugOutputButton, &QPushButton::clicked, this, [this]()
        {
            if (m_debugOutputEdit != nullptr)
            {
                m_debugOutputEdit->clear();
            }
        });
    connect(m_copyDebugOutputButton, &QPushButton::clicked, this, [this]()
        {
            if (m_debugOutputEdit == nullptr || QGuiApplication::clipboard() == nullptr)
            {
                return;
            }
            QGuiApplication::clipboard()->setText(m_debugOutputEdit->toPlainText());
        });
}

