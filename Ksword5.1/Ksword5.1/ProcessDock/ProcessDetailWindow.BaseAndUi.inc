// ============================================================
// ProcessDetailWindow.BaseAndUi.inc
// 作用：
// - 负责构造/基础数据合并、三个 Tab 的 UI 初始化以及基础信号连接。
// - 聚焦“窗口骨架与静态展示数据”逻辑。
// ============================================================

ProcessDetailWindow::ProcessDetailWindow(const ks::process::ProcessRecord& baseRecord, QWidget* parent)
    : QWidget(parent)
    , m_baseRecord(baseRecord)
{
    // 构造入口日志：记录目标 PID 与 identity 关键字段。
    kLogEvent ctorStartEvent;
    info << ctorStartEvent
        << "[ProcessDetailWindow] 构造开始, pid="
        << m_baseRecord.pid
        << ", createTime100ns="
        << m_baseRecord.creationTime100ns
        << eol;

    // 详情窗口是独立顶层窗口：非 Dock、非模态，不阻塞主界面。
    setWindowFlag(Qt::Window, true);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1160, 760);

    // identity 用于日志和窗口复用定位。
    m_identityKey = ks::process::BuildProcessIdentityKey(
        m_baseRecord.pid,
        m_baseRecord.creationTime100ns);

    // 详情窗口创建时尽量补齐静态字段，避免首次展示出现空路径/空命令行。
    const bool needStaticQuery =
        m_baseRecord.imagePath.empty() ||
        m_baseRecord.commandLine.empty() ||
        m_baseRecord.userName.empty() ||
        m_baseRecord.signatureState.empty() ||
        m_baseRecord.signatureState == "Pending";
    if (needStaticQuery && m_baseRecord.pid != 0)
    {
        ks::process::ProcessRecord queriedRecord{};
        if (ks::process::QueryProcessStaticDetailByPid(m_baseRecord.pid, queriedRecord))
        {
            if (!queriedRecord.processName.empty()) m_baseRecord.processName = queriedRecord.processName;
            if (!queriedRecord.imagePath.empty()) m_baseRecord.imagePath = queriedRecord.imagePath;
            if (!queriedRecord.commandLine.empty()) m_baseRecord.commandLine = queriedRecord.commandLine;
            if (!queriedRecord.userName.empty()) m_baseRecord.userName = queriedRecord.userName;
            if (!queriedRecord.startTimeText.empty()) m_baseRecord.startTimeText = queriedRecord.startTimeText;
            if (!queriedRecord.architectureText.empty()) m_baseRecord.architectureText = queriedRecord.architectureText;
            if (!queriedRecord.priorityText.empty()) m_baseRecord.priorityText = queriedRecord.priorityText;
            if (!queriedRecord.signatureState.empty()) m_baseRecord.signatureState = queriedRecord.signatureState;
            if (!queriedRecord.signaturePublisher.empty()) m_baseRecord.signaturePublisher = queriedRecord.signaturePublisher;
            m_baseRecord.signatureTrusted = queriedRecord.signatureTrusted;
            m_baseRecord.isAdmin = queriedRecord.isAdmin;
            if (queriedRecord.parentPid != 0) m_baseRecord.parentPid = queriedRecord.parentPid;
            if (queriedRecord.sessionId != 0) m_baseRecord.sessionId = queriedRecord.sessionId;
            if (queriedRecord.threadCount != 0) m_baseRecord.threadCount = queriedRecord.threadCount;
            if (queriedRecord.handleCount != 0) m_baseRecord.handleCount = queriedRecord.handleCount;
            if (queriedRecord.creationTime100ns != 0) m_baseRecord.creationTime100ns = queriedRecord.creationTime100ns;
            kLogEvent ctorStaticQuerySuccessEvent;
            info << ctorStaticQuerySuccessEvent
                << "[ProcessDetailWindow] 构造阶段补齐静态信息成功, pid="
                << m_baseRecord.pid
                << eol;
        }
        else
        {
            kLogEvent ctorStaticQueryFailEvent;
            warn << ctorStaticQueryFailEvent
                << "[ProcessDetailWindow] 构造阶段静态信息查询失败, pid="
                << m_baseRecord.pid
                << eol;
        }
    }

    // 按“建 UI -> 连信号 -> 填初值 -> 首次模块刷新”顺序初始化。
    initializeUi();
    initializeConnections();
    refreshDetailTabTexts();
    requestAsyncModuleRefresh(true);

    // 构造结束日志：标记窗口初始化链路完成。
    kLogEvent ctorFinishEvent;
    info << ctorFinishEvent
        << "[ProcessDetailWindow] 构造完成, pid="
        << m_baseRecord.pid
        << ", identity="
        << m_identityKey
        << eol;
}

void ProcessDetailWindow::updateBaseRecord(const ks::process::ProcessRecord& baseRecord)
{
    // 更新入口日志：记录新快照 PID 与旧 identity。
    kLogEvent updateRecordStartEvent;
    info << updateRecordStartEvent
        << "[ProcessDetailWindow] updateBaseRecord: 开始更新, incomingPid="
        << baseRecord.pid
        << ", oldIdentity="
        << m_identityKey
        << eol;

    // 外部推送新快照时：
    // 1) 先保留已有的“已补齐字段”；
    // 2) 再合并新快照；
    // 3) 必要时补查静态详情，避免字段被空值覆盖。
    ks::process::ProcessRecord mergedRecord = baseRecord;
    if (mergedRecord.imagePath.empty()) mergedRecord.imagePath = m_baseRecord.imagePath;
    if (mergedRecord.commandLine.empty()) mergedRecord.commandLine = m_baseRecord.commandLine;
    if (mergedRecord.userName.empty()) mergedRecord.userName = m_baseRecord.userName;
    if (mergedRecord.startTimeText.empty()) mergedRecord.startTimeText = m_baseRecord.startTimeText;
    if (mergedRecord.signatureState.empty()) mergedRecord.signatureState = m_baseRecord.signatureState;
    if (mergedRecord.signaturePublisher.empty()) mergedRecord.signaturePublisher = m_baseRecord.signaturePublisher;
    mergedRecord.signatureTrusted = mergedRecord.signatureTrusted || m_baseRecord.signatureTrusted;

    const bool needStaticQuery =
        mergedRecord.imagePath.empty() ||
        mergedRecord.commandLine.empty() ||
        mergedRecord.userName.empty() ||
        mergedRecord.signatureState.empty() ||
        mergedRecord.signatureState == "Pending";
    if (needStaticQuery && mergedRecord.pid != 0)
    {
        ks::process::ProcessRecord queriedRecord{};
        if (ks::process::QueryProcessStaticDetailByPid(mergedRecord.pid, queriedRecord))
        {
            if (!queriedRecord.processName.empty()) mergedRecord.processName = queriedRecord.processName;
            if (!queriedRecord.imagePath.empty()) mergedRecord.imagePath = queriedRecord.imagePath;
            if (!queriedRecord.commandLine.empty()) mergedRecord.commandLine = queriedRecord.commandLine;
            if (!queriedRecord.userName.empty()) mergedRecord.userName = queriedRecord.userName;
            if (!queriedRecord.startTimeText.empty()) mergedRecord.startTimeText = queriedRecord.startTimeText;
            if (!queriedRecord.architectureText.empty()) mergedRecord.architectureText = queriedRecord.architectureText;
            if (!queriedRecord.priorityText.empty()) mergedRecord.priorityText = queriedRecord.priorityText;
            if (!queriedRecord.signatureState.empty()) mergedRecord.signatureState = queriedRecord.signatureState;
            if (!queriedRecord.signaturePublisher.empty()) mergedRecord.signaturePublisher = queriedRecord.signaturePublisher;
            mergedRecord.signatureTrusted = queriedRecord.signatureTrusted;
            mergedRecord.isAdmin = queriedRecord.isAdmin;
            if (queriedRecord.parentPid != 0) mergedRecord.parentPid = queriedRecord.parentPid;
            if (queriedRecord.sessionId != 0) mergedRecord.sessionId = queriedRecord.sessionId;
            if (queriedRecord.threadCount != 0) mergedRecord.threadCount = queriedRecord.threadCount;
            if (queriedRecord.handleCount != 0) mergedRecord.handleCount = queriedRecord.handleCount;
            if (queriedRecord.creationTime100ns != 0) mergedRecord.creationTime100ns = queriedRecord.creationTime100ns;
            kLogEvent updateRecordStaticSuccessEvent;
            dbg << updateRecordStaticSuccessEvent
                << "[ProcessDetailWindow] updateBaseRecord: 静态信息补齐成功, pid="
                << mergedRecord.pid
                << eol;
        }
        else
        {
            kLogEvent updateRecordStaticFailEvent;
            warn << updateRecordStaticFailEvent
                << "[ProcessDetailWindow] updateBaseRecord: 静态信息补齐失败, pid="
                << mergedRecord.pid
                << eol;
        }
    }

    m_baseRecord = mergedRecord;
    m_identityKey = ks::process::BuildProcessIdentityKey(
        m_baseRecord.pid,
        m_baseRecord.creationTime100ns);
    refreshDetailTabTexts();

    // 更新结束日志：输出新 identity 与关键字段状态。
    kLogEvent updateRecordFinishEvent;
    info << updateRecordFinishEvent
        << "[ProcessDetailWindow] updateBaseRecord: 完成, pid="
        << m_baseRecord.pid
        << ", newIdentity="
        << m_identityKey
        << ", signatureState="
        << m_baseRecord.signatureState
        << eol;
}

std::uint32_t ProcessDetailWindow::pid() const
{
    return m_baseRecord.pid;
}

void ProcessDetailWindow::initializeUi()
{
    // UI 初始化入口日志：用于排查窗口初始化顺序。
    kLogEvent initUiEvent;
    info << initUiEvent
        << "[ProcessDetailWindow] initializeUi: 创建根布局和Tab容器。"
        << eol;

    // 根布局只放一个 TabWidget，满足 4.2 的三分栏结构。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_tabWidget, 1);

    // 创建三个页面并分别初始化。
    m_detailTab = new QWidget(m_tabWidget);
    m_actionTab = new QWidget(m_tabWidget);
    m_moduleTab = new QWidget(m_tabWidget);

    initializeDetailTab();
    initializeActionTab();
    initializeModuleTab();

    // 为 Tab 指定图标与标题文本。
    m_tabWidget->addTab(m_detailTab, QIcon(":/Icon/process_details.svg"), "详细信息");
    m_tabWidget->addTab(m_actionTab, QIcon(":/Icon/process_priority.svg"), "操作");
    m_tabWidget->addTab(m_moduleTab, QIcon(":/Icon/process_list.svg"), "模块");
    m_tabWidget->setCurrentWidget(m_detailTab);

    updateWindowTitle();
}

void ProcessDetailWindow::initializeDetailTab()
{
    // 详情页初始化日志：确认详细信息面板构建开始。
    kLogEvent initDetailTabEvent;
    info << initDetailTabEvent
        << "[ProcessDetailWindow] initializeDetailTab: 构建详细信息页面。"
        << eol;

    m_detailLayout = new QVBoxLayout(m_detailTab);
    m_detailLayout->setContentsMargins(6, 6, 6, 6);
    m_detailLayout->setSpacing(8);

    // 顶部：40px 图标 + 进程名与 PID。
    QHBoxLayout* titleLayout = new QHBoxLayout();
    m_processIconLabel = new QLabel(m_detailTab);
    m_processIconLabel->setFixedSize(40, 40);
    m_processTitleLabel = new QLabel(m_detailTab);
    m_processTitleLabel->setStyleSheet("font-size:18px; font-weight:700; color:#1F1F1F;");
    titleLayout->addWidget(m_processIconLabel, 0, Qt::AlignTop);
    titleLayout->addWidget(m_processTitleLabel, 1);
    titleLayout->addStretch(1);
    m_detailLayout->addLayout(titleLayout);

    // 路径行：只读输入框 + 复制 + 打开文件夹。
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel("程序路径:", m_detailTab));
    m_pathLineEdit = new QLineEdit(m_detailTab);
    m_pathLineEdit->setReadOnly(true);
    m_copyPathButton = new QPushButton(QIcon(":/Icon/process_copy_cell.svg"), "复制", m_detailTab);
    m_openPathFolderButton = new QPushButton(QIcon(":/Icon/process_open_folder.svg"), "打开文件夹", m_detailTab);
    pathLayout->addWidget(m_pathLineEdit, 1);
    pathLayout->addWidget(m_copyPathButton);
    pathLayout->addWidget(m_openPathFolderButton);
    m_detailLayout->addLayout(pathLayout);

    // 命令行行：只读输入框 + 复制。
    QHBoxLayout* commandLayout = new QHBoxLayout();
    commandLayout->addWidget(new QLabel("启动命令行:", m_detailTab));
    m_commandLineEdit = new QLineEdit(m_detailTab);
    m_commandLineEdit->setReadOnly(true);
    m_copyCommandButton = new QPushButton(QIcon(":/Icon/process_copy_cell.svg"), "复制", m_detailTab);
    commandLayout->addWidget(m_commandLineEdit, 1);
    commandLayout->addWidget(m_copyCommandButton);
    m_detailLayout->addLayout(commandLayout);

    // 父进程行：20px 图标 + 名称 PID + 转到父进程按钮（存在时显示）。
    QHBoxLayout* parentLayout = new QHBoxLayout();
    m_parentIconLabel = new QLabel(m_detailTab);
    m_parentIconLabel->setFixedSize(20, 20);
    m_parentInfoLabel = new QLabel(m_detailTab);
    m_parentInfoLabel->setStyleSheet("color:#3F3F3F; font-weight:600;");
    m_gotoParentButton = new QPushButton(QIcon(":/Icon/process_details.svg"), "转到父进程", m_detailTab);
    m_gotoParentButton->setVisible(false);
    parentLayout->addWidget(new QLabel("父进程:", m_detailTab));
    parentLayout->addWidget(m_parentIconLabel);
    parentLayout->addWidget(m_parentInfoLabel, 1);
    parentLayout->addWidget(m_gotoParentButton);
    m_detailLayout->addLayout(parentLayout);

    // 详细字段区域：尽可能展示更多可得信息。
    QGroupBox* detailGroup = new QGroupBox("更多进程详细数据", m_detailTab);
    QFormLayout* detailFormLayout = new QFormLayout(detailGroup);
    detailFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    detailFormLayout->setHorizontalSpacing(18);
    detailFormLayout->setVerticalSpacing(6);

    m_detailStartTimeValue = new QLabel(detailGroup);
    m_detailUserValue = new QLabel(detailGroup);
    m_detailAdminValue = new QLabel(detailGroup);
    m_detailArchitectureValue = new QLabel(detailGroup);
    m_detailPriorityValue = new QLabel(detailGroup);
    m_detailSessionValue = new QLabel(detailGroup);
    m_detailThreadCountValue = new QLabel(detailGroup);
    m_detailHandleCountValue = new QLabel(detailGroup);
    m_detailCpuValue = new QLabel(detailGroup);
    m_detailRamValue = new QLabel(detailGroup);
    m_detailDiskValue = new QLabel(detailGroup);
    m_detailSignatureValue = new QLabel(detailGroup);

    detailFormLayout->addRow("启动时间", m_detailStartTimeValue);
    detailFormLayout->addRow("用户", m_detailUserValue);
    detailFormLayout->addRow("管理员", m_detailAdminValue);
    detailFormLayout->addRow("架构", m_detailArchitectureValue);
    detailFormLayout->addRow("优先级", m_detailPriorityValue);
    detailFormLayout->addRow("Session ID", m_detailSessionValue);
    detailFormLayout->addRow("线程数量", m_detailThreadCountValue);
    detailFormLayout->addRow("句柄数量", m_detailHandleCountValue);
    detailFormLayout->addRow("CPU 占用", m_detailCpuValue);
    detailFormLayout->addRow("RAM 占用", m_detailRamValue);
    detailFormLayout->addRow("DISK 吞吐", m_detailDiskValue);
    detailFormLayout->addRow("数字签名", m_detailSignatureValue);

    m_detailLayout->addWidget(detailGroup);
    m_detailLayout->addStretch(1);

    const QString buttonStyle = buildBlueButtonStyle();
    m_copyPathButton->setStyleSheet(buttonStyle);
    m_openPathFolderButton->setStyleSheet(buttonStyle);
    m_copyCommandButton->setStyleSheet(buttonStyle);
    m_gotoParentButton->setStyleSheet(buttonStyle);
}

void ProcessDetailWindow::initializeActionTab()
{
    // 操作页初始化日志：确认动作按钮区域构建。
    kLogEvent initActionTabEvent;
    info << initActionTabEvent
        << "[ProcessDetailWindow] initializeActionTab: 构建进程操作页面。"
        << eol;

    m_actionLayout = new QVBoxLayout(m_actionTab);
    m_actionLayout->setContentsMargins(6, 6, 6, 6);
    m_actionLayout->setSpacing(8);

    // 进程控制操作组：与右键菜单能力保持对齐。
    QGroupBox* processActionGroup = new QGroupBox("进程控制", m_actionTab);
    QGridLayout* processActionGrid = new QGridLayout(processActionGroup);
    processActionGrid->setHorizontalSpacing(8);
    processActionGrid->setVerticalSpacing(8);

    m_taskKillButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), "Taskkill", processActionGroup);
    m_taskKillForceButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), "Taskkill /f", processActionGroup);
    m_terminateProcessButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), "TerminateProcess", processActionGroup);
    m_terminateThreadsButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), "TerminateThread(全部线程)", processActionGroup);
    m_suspendProcessButton = new QPushButton(QIcon(":/Icon/process_suspend.svg"), "挂起进程", processActionGroup);
    m_resumeProcessButton = new QPushButton(QIcon(":/Icon/process_resume.svg"), "恢复进程", processActionGroup);
    m_setCriticalButton = new QPushButton(QIcon(":/Icon/process_critical.svg"), "设为关键进程", processActionGroup);
    m_clearCriticalButton = new QPushButton(QIcon(":/Icon/process_uncritical.svg"), "取消关键进程", processActionGroup);
    m_injectInvalidShellcodeButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), "注入无效shellcode", processActionGroup);

    processActionGrid->addWidget(m_taskKillButton, 0, 0);
    processActionGrid->addWidget(m_taskKillForceButton, 0, 1);
    processActionGrid->addWidget(m_terminateProcessButton, 1, 0);
    processActionGrid->addWidget(m_terminateThreadsButton, 1, 1);
    processActionGrid->addWidget(m_suspendProcessButton, 2, 0);
    processActionGrid->addWidget(m_resumeProcessButton, 2, 1);
    processActionGrid->addWidget(m_setCriticalButton, 3, 0);
    processActionGrid->addWidget(m_clearCriticalButton, 3, 1);
    processActionGrid->addWidget(m_injectInvalidShellcodeButton, 4, 0, 1, 2);
    m_actionLayout->addWidget(processActionGroup);

    // 优先级设置组。
    QGroupBox* priorityGroup = new QGroupBox("进程优先级", m_actionTab);
    QHBoxLayout* priorityLayout = new QHBoxLayout(priorityGroup);
    m_priorityCombo = new QComboBox(priorityGroup);
    m_priorityCombo->addItem("Idle", 0);
    m_priorityCombo->addItem("Below Normal", 1);
    m_priorityCombo->addItem("Normal", 2);
    m_priorityCombo->addItem("Above Normal", 3);
    m_priorityCombo->addItem("High", 4);
    m_priorityCombo->addItem("Realtime", 5);
    m_priorityCombo->setCurrentIndex(2);
    m_applyPriorityButton = new QPushButton(QIcon(":/Icon/process_priority.svg"), "应用优先级", priorityGroup);
    priorityLayout->addWidget(m_priorityCombo, 1);
    priorityLayout->addWidget(m_applyPriorityButton);
    m_actionLayout->addWidget(priorityGroup);

    // DLL 注入组。
    QGroupBox* dllInjectGroup = new QGroupBox("DLL 注入器", m_actionTab);
    QHBoxLayout* dllInjectLayout = new QHBoxLayout(dllInjectGroup);
    m_dllPathLineEdit = new QLineEdit(dllInjectGroup);
    m_dllPathLineEdit->setPlaceholderText("请选择要注入的 DLL 路径");
    m_browseDllButton = new QPushButton(QIcon(":/Icon/process_open_folder.svg"), "浏览", dllInjectGroup);
    m_injectDllButton = new QPushButton(QIcon(":/Icon/process_start.svg"), "注入 DLL", dllInjectGroup);
    dllInjectLayout->addWidget(m_dllPathLineEdit, 1);
    dllInjectLayout->addWidget(m_browseDllButton);
    dllInjectLayout->addWidget(m_injectDllButton);
    m_actionLayout->addWidget(dllInjectGroup);

    // shellcode 注入组。
    QGroupBox* shellcodeGroup = new QGroupBox("Shellcode 加载器", m_actionTab);
    QHBoxLayout* shellcodeLayout = new QHBoxLayout(shellcodeGroup);
    m_shellcodePathLineEdit = new QLineEdit(shellcodeGroup);
    m_shellcodePathLineEdit->setPlaceholderText("请选择原始 shellcode 二进制文件");
    m_browseShellcodeButton = new QPushButton(QIcon(":/Icon/process_open_folder.svg"), "浏览", shellcodeGroup);
    m_injectShellcodeButton = new QPushButton(QIcon(":/Icon/process_start.svg"), "注入 shellcode", shellcodeGroup);
    shellcodeLayout->addWidget(m_shellcodePathLineEdit, 1);
    shellcodeLayout->addWidget(m_browseShellcodeButton);
    shellcodeLayout->addWidget(m_injectShellcodeButton);
    m_actionLayout->addWidget(shellcodeGroup);

    m_actionLayout->addStretch(1);

    // 统一按钮主题样式。
    const QString buttonStyle = buildBlueButtonStyle();
    const std::vector<QPushButton*> actionButtons{
        m_taskKillButton,
        m_taskKillForceButton,
        m_terminateProcessButton,
        m_terminateThreadsButton,
        m_suspendProcessButton,
        m_resumeProcessButton,
        m_setCriticalButton,
        m_clearCriticalButton,
        m_injectInvalidShellcodeButton,
        m_applyPriorityButton,
        m_browseDllButton,
        m_injectDllButton,
        m_browseShellcodeButton,
        m_injectShellcodeButton
    };
    for (QPushButton* buttonItem : actionButtons)
    {
        if (buttonItem != nullptr)
        {
            buttonItem->setStyleSheet(buttonStyle);
        }
    }
}

void ProcessDetailWindow::initializeModuleTab()
{
    // 模块页初始化日志：确认模块表与工具栏创建。
    kLogEvent initModuleTabEvent;
    info << initModuleTabEvent
        << "[ProcessDetailWindow] initializeModuleTab: 构建模块页面。"
        << eol;

    m_moduleLayout = new QVBoxLayout(m_moduleTab);
    m_moduleLayout->setContentsMargins(6, 6, 6, 6);
    m_moduleLayout->setSpacing(6);

    // 顶部工具栏：刷新按钮 + 签名校验选项 + 状态标签。
    m_moduleTopBarLayout = new QHBoxLayout();
    m_moduleTopBarLayout->setContentsMargins(0, 0, 0, 0);
    m_moduleTopBarLayout->setSpacing(8);
    m_refreshModuleButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), "刷新模块", m_moduleTab);
    m_signatureCheckBox = new QCheckBox("刷新时校验签名", m_moduleTab);
    m_signatureCheckBox->setChecked(true);
    m_signatureCheckBox->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }"
        "QCheckBox::indicator:checked { background:%1; border:1px solid %1; }")
        .arg(KswordTheme::PrimaryBlueHex));
    m_moduleStatusLabel = new QLabel("● 等待首次刷新", m_moduleTab);
    m_moduleStatusLabel->setStyleSheet("color:#5F5F5F; font-weight:600;");
    m_moduleTopBarLayout->addWidget(m_refreshModuleButton);
    m_moduleTopBarLayout->addWidget(m_signatureCheckBox);
    m_moduleTopBarLayout->addStretch(1);
    m_moduleTopBarLayout->addWidget(m_moduleStatusLabel);
    m_moduleLayout->addLayout(m_moduleTopBarLayout);

    // 模块列表表格。
    m_moduleTable = new QTreeWidget(m_moduleTab);
    m_moduleTable->setColumnCount(static_cast<int>(ModuleColumn::Count));
    m_moduleTable->setHeaderLabels(ModuleHeaders);
    m_moduleTable->setRootIsDecorated(false);
    m_moduleTable->setItemsExpandable(false);
    m_moduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_moduleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_moduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_moduleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_moduleTable->setSortingEnabled(true);
    m_moduleTable->setAlternatingRowColors(true);
    m_moduleLayout->addWidget(m_moduleTable, 1);

    // 列宽初始化。
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::Path), 560);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::Size), 110);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::Signature), 260);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::EntryOffset), 120);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::State), 90);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::ThreadId), 180);

    // 表头蓝色主题。
    m_moduleTable->header()->setStyleSheet(QStringLiteral(
        "QHeaderView::section {"
        "  color:%1;"
        "  background:#FFFFFF;"
        "  border:1px solid #E6E6E6;"
        "  padding:4px;"
        "  font-weight:600;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex));

    m_refreshModuleButton->setStyleSheet(buildBlueButtonStyle());
}

void ProcessDetailWindow::initializeConnections()
{
    // 连接初始化日志：用于确认所有按钮信号都已挂接。
    kLogEvent initConnectionsEvent;
    info << initConnectionsEvent
        << "[ProcessDetailWindow] initializeConnections: 开始连接信号槽。"
        << eol;

    // 复制路径按钮。
    connect(m_copyPathButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_pathLineEdit->text());
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDetailWindow] 复制程序路径, pid=" << m_baseRecord.pid << eol;
    });

    // 打开路径按钮。
    connect(m_openPathFolderButton, &QPushButton::clicked, this, [this]() {
        std::string detailText;
        const bool actionOk = ks::process::OpenFolderByPath(m_baseRecord.imagePath, &detailText);
        showActionResultMessage("打开程序路径", actionOk, detailText);
    });

    // 复制命令行按钮。
    connect(m_copyCommandButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_commandLineEdit->text());
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDetailWindow] 复制命令行, pid=" << m_baseRecord.pid << eol;
    });

    // 转到父进程按钮。
    connect(m_gotoParentButton, &QPushButton::clicked, this, [this]() {
        const QVariant parentPidVariant = m_gotoParentButton->property("parent_pid");
        if (!parentPidVariant.isValid())
        {
            return;
        }
        const std::uint32_t parentPid = parentPidVariant.toUInt();
        emit requestOpenProcessByPid(parentPid);
    });

    // 操作页按钮连接。
    connect(m_taskKillButton, &QPushButton::clicked, this, [this]() { executeTaskKillAction(false); });
    connect(m_taskKillForceButton, &QPushButton::clicked, this, [this]() { executeTaskKillAction(true); });
    connect(m_terminateProcessButton, &QPushButton::clicked, this, [this]() { executeTerminateProcessAction(); });
    connect(m_terminateThreadsButton, &QPushButton::clicked, this, [this]() { executeTerminateThreadsAction(); });
    connect(m_suspendProcessButton, &QPushButton::clicked, this, [this]() { executeSuspendProcessAction(); });
    connect(m_resumeProcessButton, &QPushButton::clicked, this, [this]() { executeResumeProcessAction(); });
    connect(m_setCriticalButton, &QPushButton::clicked, this, [this]() { executeSetCriticalAction(true); });
    connect(m_clearCriticalButton, &QPushButton::clicked, this, [this]() { executeSetCriticalAction(false); });
    connect(m_injectInvalidShellcodeButton, &QPushButton::clicked, this, [this]() { executeInjectInvalidShellcodeAction(); });
    connect(m_applyPriorityButton, &QPushButton::clicked, this, [this]() { executeSetPriorityAction(); });
    connect(m_injectDllButton, &QPushButton::clicked, this, [this]() { executeInjectDllAction(); });
    connect(m_injectShellcodeButton, &QPushButton::clicked, this, [this]() { executeInjectShellcodeAction(); });

    // 浏览 DLL 路径。
    connect(m_browseDllButton, &QPushButton::clicked, this, [this]() {
        const QString filePath = QFileDialog::getOpenFileName(
            this,
            "选择要注入的 DLL",
            QString(),
            "DLL Files (*.dll);;All Files (*)");
        if (!filePath.isEmpty())
        {
            m_dllPathLineEdit->setText(filePath);
        }
    });

    // 浏览 shellcode 文件路径。
    connect(m_browseShellcodeButton, &QPushButton::clicked, this, [this]() {
        const QString filePath = QFileDialog::getOpenFileName(
            this,
            "选择 shellcode 文件",
            QString(),
            "Binary Files (*.bin *.dat);;All Files (*)");
        if (!filePath.isEmpty())
        {
            m_shellcodePathLineEdit->setText(filePath);
        }
    });

    // 模块刷新按钮。
    connect(m_refreshModuleButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDetailWindow] 用户点击“刷新模块”, pid=" << m_baseRecord.pid
            << eol;
        requestAsyncModuleRefresh(true);
    });

    // 模块表右键菜单。
    connect(m_moduleTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showModuleContextMenu(localPosition);
    });
}

void ProcessDetailWindow::refreshDetailTabTexts()
{
    // 详情刷新入口日志：记录当前 PID 与进程名。
    kLogEvent refreshDetailEvent;
    dbg << refreshDetailEvent
        << "[ProcessDetailWindow] refreshDetailTabTexts: pid="
        << m_baseRecord.pid
        << ", processName="
        << m_baseRecord.processName
        << eol;

    // 顶部标题与图标。
    m_processTitleLabel->setText(
        QString("%1  (PID: %2)")
        .arg(QString::fromStdString(m_baseRecord.processName.empty() ? "Unknown" : m_baseRecord.processName))
        .arg(m_baseRecord.pid));
    m_processIconLabel->setPixmap(resolveProcessIcon(m_baseRecord.imagePath, 40).pixmap(40, 40));

    // 路径与命令行。
    QString processPathText = QString::fromStdString(m_baseRecord.imagePath);
    if (processPathText.trimmed().isEmpty() && m_baseRecord.pid != 0)
    {
        // 兜底再查一次路径，避免 UI 出现“路径始终为空”。
        processPathText = QString::fromStdString(ks::process::QueryProcessPathByPid(m_baseRecord.pid));
        if (!processPathText.trimmed().isEmpty())
        {
            m_baseRecord.imagePath = processPathText.toStdString();
        }
    }
    m_pathLineEdit->setText(processPathText.trimmed().isEmpty() ? "-" : processPathText);
    m_commandLineEdit->setText(QString::fromStdString(m_baseRecord.commandLine.empty() ? "-" : m_baseRecord.commandLine));

    // 详细字段赋值。
    m_detailStartTimeValue->setText(QString::fromStdString(m_baseRecord.startTimeText.empty() ? "-" : m_baseRecord.startTimeText));
    m_detailUserValue->setText(QString::fromStdString(m_baseRecord.userName.empty() ? "-" : m_baseRecord.userName));
    m_detailAdminValue->setText(m_baseRecord.isAdmin ? "■ 是" : "■ 否");
    m_detailAdminValue->setStyleSheet(m_baseRecord.isAdmin ? "color:#228B22; font-weight:700;" : "color:#DC322F; font-weight:700;");
    m_detailArchitectureValue->setText(QString::fromStdString(m_baseRecord.architectureText.empty() ? "Unknown" : m_baseRecord.architectureText));
    m_detailPriorityValue->setText(QString::fromStdString(m_baseRecord.priorityText.empty() ? "Unknown" : m_baseRecord.priorityText));
    m_detailSessionValue->setText(QString::number(m_baseRecord.sessionId));
    m_detailThreadCountValue->setText(QString::number(m_baseRecord.threadCount));
    m_detailHandleCountValue->setText(QString::number(m_baseRecord.handleCount));
    m_detailCpuValue->setText(formatDoubleText(m_baseRecord.cpuPercent, 2) + "%");
    m_detailRamValue->setText(formatDoubleText(m_baseRecord.ramMB, 1) + " MB");
    m_detailDiskValue->setText(formatDoubleText(m_baseRecord.diskMBps, 2) + " MB/s");
    m_detailSignatureValue->setText(QString::fromStdString(m_baseRecord.signatureState.empty() ? "Unknown" : m_baseRecord.signatureState));
    if (!m_baseRecord.signatureTrusted && m_baseRecord.signatureState != "Pending")
    {
        m_detailSignatureValue->setStyleSheet("color:#DC322F; font-weight:700;");
    }
    else if (m_baseRecord.signatureTrusted)
    {
        m_detailSignatureValue->setStyleSheet("color:#228B22; font-weight:700;");
    }
    else
    {
        m_detailSignatureValue->setStyleSheet("color:#6F6F6F; font-weight:600;");
    }

    // 刷新父进程信息区。
    refreshParentProcessSection();
    updateWindowTitle();

    // 详情刷新完成日志：确认核心字段已落到 UI。
    kLogEvent refreshDetailFinishEvent;
    dbg << refreshDetailFinishEvent
        << "[ProcessDetailWindow] refreshDetailTabTexts: 完成, signatureState="
        << m_baseRecord.signatureState
        << ", user="
        << m_baseRecord.userName
        << eol;
}

void ProcessDetailWindow::refreshParentProcessSection()
{
    // 父进程区域刷新日志：记录父 PID。
    kLogEvent refreshParentEvent;
    dbg << refreshParentEvent
        << "[ProcessDetailWindow] refreshParentProcessSection: parentPid="
        << m_baseRecord.parentPid
        << eol;

    // 默认先隐藏“转到父进程”，只有父进程仍存在才显示。
    m_gotoParentButton->setVisible(false);
    m_gotoParentButton->setProperty("parent_pid", QVariant());

    if (m_baseRecord.parentPid == 0)
    {
        m_parentInfoLabel->setText("无父进程信息");
        m_parentIconLabel->setPixmap(QIcon(":/Icon/process_main.svg").pixmap(20, 20));
        return;
    }

    const std::uint32_t parentPid = m_baseRecord.parentPid;
    const std::string parentName = ks::process::GetProcessNameByPID(parentPid);
    const bool parentAlive = !parentName.empty();

    if (parentAlive)
    {
        m_parentInfoLabel->setText(
            QString("%1 (PID: %2)")
            .arg(QString::fromStdString(parentName))
            .arg(parentPid));
        const std::string parentPath = ks::process::QueryProcessPathByPid(parentPid);
        m_parentIconLabel->setPixmap(resolveProcessIcon(parentPath, 20).pixmap(20, 20));
        m_gotoParentButton->setVisible(true);
        m_gotoParentButton->setProperty("parent_pid", QVariant::fromValue(parentPid));
        kLogEvent refreshParentAliveEvent;
        dbg << refreshParentAliveEvent
            << "[ProcessDetailWindow] refreshParentProcessSection: 父进程可访问, parentPid="
            << parentPid
            << eol;
    }
    else
    {
        m_parentInfoLabel->setText(QString("父进程已退出或不可访问 (PID: %1)").arg(parentPid));
        m_parentIconLabel->setPixmap(QIcon(":/Icon/process_main.svg").pixmap(20, 20));
        kLogEvent refreshParentDeadEvent;
        warn << refreshParentDeadEvent
            << "[ProcessDetailWindow] refreshParentProcessSection: 父进程不可访问, parentPid="
            << parentPid
            << eol;
    }
}

void ProcessDetailWindow::updateWindowTitle()
{
    // 标题更新日志：便于多窗口场景排查标题错乱。
    kLogEvent updateTitleEvent;
    dbg << updateTitleEvent
        << "[ProcessDetailWindow] updateWindowTitle: pid="
        << m_baseRecord.pid
        << eol;

    setWindowTitle(
        QString("进程详细信息 - %1 (PID %2)")
        .arg(QString::fromStdString(m_baseRecord.processName.empty() ? "Unknown" : m_baseRecord.processName))
        .arg(m_baseRecord.pid));
}

