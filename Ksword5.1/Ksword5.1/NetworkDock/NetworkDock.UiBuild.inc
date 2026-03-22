void NetworkDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(4);

    // 侧边栏 Tab：满足“分侧边栏 tab 实现”要求。
    m_sideTabWidget = new QTabWidget(this);
    m_sideTabWidget->setTabPosition(QTabWidget::West);
    m_rootLayout->addWidget(m_sideTabWidget, 1);

    initializeTrafficMonitorTab();
    initializeRateLimitTab();
    initializeConnectionManageTab();
    initializeManualRequestTab();
    initializeArpCacheTab();
    initializeDnsCacheTab();
    initializeAliveHostScanTab();
}

void NetworkDock::initializeTrafficMonitorTab()
{
    m_trafficMonitorPage = new QWidget(this);
    m_trafficMonitorLayout = new QVBoxLayout(m_trafficMonitorPage);
    m_trafficMonitorLayout->setContentsMargins(6, 6, 6, 6);
    m_trafficMonitorLayout->setSpacing(6);

    // 控制栏：启动/停止/清空 + 状态提示。
    m_monitorControlLayout = new QHBoxLayout();
    m_monitorControlLayout->setSpacing(6);

    m_startMonitorButton = new QPushButton(m_trafficMonitorPage);
    m_startMonitorButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_startMonitorButton->setToolTip(QStringLiteral("启动网络流量监控"));

    m_stopMonitorButton = new QPushButton(m_trafficMonitorPage);
    m_stopMonitorButton->setIcon(QIcon(":/Icon/process_pause.svg"));
    m_stopMonitorButton->setToolTip(QStringLiteral("停止网络流量监控"));

    m_clearPacketButton = new QPushButton(m_trafficMonitorPage);
    m_clearPacketButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_clearPacketButton->setToolTip(QStringLiteral("清空当前流量列表"));

    m_monitorStatusLabel = new QLabel(QStringLiteral("状态：未启动"), m_trafficMonitorPage);
    // 状态标签不再设置较大最小宽度，避免窗口变窄时挤出横向滚动条。
    m_monitorStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_monitorControlLayout->addWidget(m_startMonitorButton);
    m_monitorControlLayout->addWidget(m_stopMonitorButton);
    m_monitorControlLayout->addWidget(m_clearPacketButton);
    m_monitorControlLayout->addWidget(m_monitorStatusLabel);
    m_monitorControlLayout->addStretch(1);

    m_trafficMonitorLayout->addLayout(m_monitorControlLayout);

    // 过滤栏第 1 行：PID + 本地IP段 + 远端IP段 + 应用/清空按钮。
    // 说明：筛查功能已合并到流量监控页，且支持多条件同时启用。
    m_monitorFilterLayoutLine1 = new QHBoxLayout();
    m_monitorFilterLayoutLine1->setSpacing(6);

    QLabel* pidFilterLabel = new QLabel(QStringLiteral("PID:"), m_trafficMonitorPage);
    m_pidFilterEdit = new QLineEdit(m_trafficMonitorPage);
    m_pidFilterEdit->setMaximumWidth(110);
    m_pidFilterEdit->setMinimumWidth(66);
    m_pidFilterEdit->setPlaceholderText(QStringLiteral("可留空"));
    m_pidFilterEdit->setToolTip(QStringLiteral("按 PID 精确筛选，右键“跟踪此进程”会自动填写此项。"));

    QLabel* localIpFilterLabel = new QLabel(QStringLiteral("本地IP段:"), m_trafficMonitorPage);
    m_localIpFilterEdit = new QLineEdit(m_trafficMonitorPage);
    m_localIpFilterEdit->setMaximumWidth(240);
    m_localIpFilterEdit->setMinimumWidth(120);
    m_localIpFilterEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_localIpFilterEdit->setPlaceholderText(QStringLiteral("示例：192.168.1.0/24 或 192.168.1.10-192.168.1.50"));
    m_localIpFilterEdit->setToolTip(QStringLiteral("支持 CIDR、IP范围、单IP 三种格式。"));

    QLabel* remoteIpFilterLabel = new QLabel(QStringLiteral("远端IP段:"), m_trafficMonitorPage);
    m_remoteIpFilterEdit = new QLineEdit(m_trafficMonitorPage);
    m_remoteIpFilterEdit->setMaximumWidth(240);
    m_remoteIpFilterEdit->setMinimumWidth(120);
    m_remoteIpFilterEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_remoteIpFilterEdit->setPlaceholderText(QStringLiteral("示例：8.8.8.8 或 10.0.0.0/8"));
    m_remoteIpFilterEdit->setToolTip(QStringLiteral("支持 CIDR、IP范围、单IP 三种格式。"));

    m_applyMonitorFilterButton = new QPushButton(m_trafficMonitorPage);
    m_applyMonitorFilterButton->setIcon(QIcon(":/Icon/log_track.svg"));
    m_applyMonitorFilterButton->setToolTip(QStringLiteral("应用当前全部筛选条件（可同时生效）"));

    m_clearMonitorFilterButton = new QPushButton(m_trafficMonitorPage);
    m_clearMonitorFilterButton->setIcon(QIcon(":/Icon/log_cancel_track.svg"));
    m_clearMonitorFilterButton->setToolTip(QStringLiteral("清空全部筛选条件并恢复显示所有流量"));

    m_monitorFilterLayoutLine1->addWidget(pidFilterLabel);
    m_monitorFilterLayoutLine1->addWidget(m_pidFilterEdit);
    m_monitorFilterLayoutLine1->addWidget(localIpFilterLabel);
    m_monitorFilterLayoutLine1->addWidget(m_localIpFilterEdit);
    m_monitorFilterLayoutLine1->addWidget(remoteIpFilterLabel);
    m_monitorFilterLayoutLine1->addWidget(m_remoteIpFilterEdit);
    m_monitorFilterLayoutLine1->addWidget(m_applyMonitorFilterButton);
    m_monitorFilterLayoutLine1->addWidget(m_clearMonitorFilterButton);
    m_monitorFilterLayoutLine1->addStretch(1);
    m_trafficMonitorLayout->addLayout(m_monitorFilterLayoutLine1);

    // 过滤栏第 2 行：本地端口 + 远端端口 + 包长范围 + 当前过滤状态。
    m_monitorFilterLayoutLine2 = new QHBoxLayout();
    m_monitorFilterLayoutLine2->setSpacing(6);

    QLabel* localPortFilterLabel = new QLabel(QStringLiteral("本地端口:"), m_trafficMonitorPage);
    m_localPortFilterEdit = new QLineEdit(m_trafficMonitorPage);
    m_localPortFilterEdit->setMaximumWidth(140);
    m_localPortFilterEdit->setMinimumWidth(90);
    m_localPortFilterEdit->setPlaceholderText(QStringLiteral("如 80 或 1000-2000"));
    m_localPortFilterEdit->setToolTip(QStringLiteral("支持单端口或端口范围。"));

    QLabel* remotePortFilterLabel = new QLabel(QStringLiteral("远端端口:"), m_trafficMonitorPage);
    m_remotePortFilterEdit = new QLineEdit(m_trafficMonitorPage);
    m_remotePortFilterEdit->setMaximumWidth(140);
    m_remotePortFilterEdit->setMinimumWidth(90);
    m_remotePortFilterEdit->setPlaceholderText(QStringLiteral("如 443 或 5000-6000"));
    m_remotePortFilterEdit->setToolTip(QStringLiteral("支持单端口或端口范围。"));

    QLabel* packetSizeLabel = new QLabel(QStringLiteral("包长(B):"), m_trafficMonitorPage);
    m_packetSizeMinSpin = new QSpinBox(m_trafficMonitorPage);
    m_packetSizeMinSpin->setRange(0, 65535);
    m_packetSizeMinSpin->setValue(0);
    m_packetSizeMinSpin->setSpecialValueText(QStringLiteral("不限"));
    m_packetSizeMinSpin->setToolTip(QStringLiteral("最小包长（0 表示不限）。"));
    m_packetSizeMinSpin->setMaximumWidth(90);

    QLabel* packetSizeToLabel = new QLabel(QStringLiteral("~"), m_trafficMonitorPage);
    m_packetSizeMaxSpin = new QSpinBox(m_trafficMonitorPage);
    m_packetSizeMaxSpin->setRange(0, 65535);
    m_packetSizeMaxSpin->setValue(0);
    m_packetSizeMaxSpin->setSpecialValueText(QStringLiteral("不限"));
    m_packetSizeMaxSpin->setToolTip(QStringLiteral("最大包长（0 表示不限）。"));
    m_packetSizeMaxSpin->setMaximumWidth(90);

    QLabel* filterStateTitle = new QLabel(QStringLiteral("过滤状态:"), m_trafficMonitorPage);
    m_monitorFilterStateLabel = new QLabel(QStringLiteral("当前过滤：无"), m_trafficMonitorPage);
    // 过滤状态标签支持换行并占据剩余空间，避免窄窗口下强制撑宽父布局。
    m_monitorFilterStateLabel->setWordWrap(true);
    m_monitorFilterStateLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_monitorFilterStateLabel->setToolTip(QStringLiteral("显示当前已应用的组合筛选条件。"));

    m_monitorFilterLayoutLine2->addWidget(localPortFilterLabel);
    m_monitorFilterLayoutLine2->addWidget(m_localPortFilterEdit);
    m_monitorFilterLayoutLine2->addWidget(remotePortFilterLabel);
    m_monitorFilterLayoutLine2->addWidget(m_remotePortFilterEdit);
    m_monitorFilterLayoutLine2->addWidget(packetSizeLabel);
    m_monitorFilterLayoutLine2->addWidget(m_packetSizeMinSpin);
    m_monitorFilterLayoutLine2->addWidget(packetSizeToLabel);
    m_monitorFilterLayoutLine2->addWidget(m_packetSizeMaxSpin);
    m_monitorFilterLayoutLine2->addWidget(filterStateTitle);
    m_monitorFilterLayoutLine2->addWidget(m_monitorFilterStateLabel, 1);
    m_trafficMonitorLayout->addLayout(m_monitorFilterLayoutLine2);

    // 报文主表：展示“全部发送 UDP/TCP 包”。
    m_packetTable = new QTableWidget(m_trafficMonitorPage);
    m_packetTable->setColumnCount(toPacketColumn(PacketTableColumn::Count));
    m_packetTable->setHorizontalHeaderLabels({
        QStringLiteral("时间"),
        QStringLiteral("协议"),
        QStringLiteral("方向"),
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("远端端点"),
        QStringLiteral("总长度"),
        QStringLiteral("负载"),
        QStringLiteral("内容预览")
        });
    m_packetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 抓包列表改为支持多选，便于批量复制报文 ASCII / 十六进制内容。
    m_packetTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_packetTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_packetTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_packetTable->verticalHeader()->setVisible(false);
    // 性能与自适应说明：
    // 1) 改为 Stretch，避免 ResizeToContents 在高频插入时反复全表测量导致卡顿；
    // 2) 同时关闭横向滚动条，保证窄窗口下也保持“自适应压缩显示”。
    m_packetTable->horizontalHeader()->setStretchLastSection(true);
    m_packetTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_packetTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_trafficMonitorLayout->addWidget(m_packetTable, 1);
    m_sideTabWidget->addTab(m_trafficMonitorPage, QIcon(":/Icon/process_main.svg"), QStringLiteral("流量监控"));
}

void NetworkDock::initializeRateLimitTab()
{
    m_rateLimitPage = new QWidget(this);
    m_rateLimitLayout = new QVBoxLayout(m_rateLimitPage);
    m_rateLimitLayout->setContentsMargins(6, 6, 6, 6);
    m_rateLimitLayout->setSpacing(6);

    // 控制栏：输入规则参数并管理规则。
    m_rateLimitControlLayout = new QHBoxLayout();
    m_rateLimitControlLayout->setSpacing(6);

    QLabel* pidLabel = new QLabel(QStringLiteral("PID:"), m_rateLimitPage);
    m_rateLimitPidEdit = new QLineEdit(m_rateLimitPage);
    m_rateLimitPidEdit->setPlaceholderText(QStringLiteral("进程 PID"));
    m_rateLimitPidEdit->setMaximumWidth(110);
    m_rateLimitPidEdit->setMinimumWidth(66);

    QLabel* kbpsLabel = new QLabel(QStringLiteral("限速KB/s:"), m_rateLimitPage);
    m_rateLimitKBpsSpin = new QSpinBox(m_rateLimitPage);
    m_rateLimitKBpsSpin->setRange(1, 1024 * 1024);
    m_rateLimitKBpsSpin->setValue(256);
    m_rateLimitKBpsSpin->setToolTip(QStringLiteral("每秒允许发送流量上限"));

    QLabel* suspendMsLabel = new QLabel(QStringLiteral("挂起时长ms:"), m_rateLimitPage);
    m_rateLimitSuspendMsSpin = new QSpinBox(m_rateLimitPage);
    m_rateLimitSuspendMsSpin->setRange(50, 2000);
    m_rateLimitSuspendMsSpin->setValue(250);
    m_rateLimitSuspendMsSpin->setToolTip(QStringLiteral("超限后挂起时长"));

    m_applyRateLimitButton = new QPushButton(m_rateLimitPage);
    m_applyRateLimitButton->setIcon(QIcon(":/Icon/process_priority.svg"));
    m_applyRateLimitButton->setToolTip(QStringLiteral("新增或更新限速规则"));

    m_removeRateLimitButton = new QPushButton(m_rateLimitPage);
    m_removeRateLimitButton->setIcon(QIcon(":/Icon/process_uncritical.svg"));
    m_removeRateLimitButton->setToolTip(QStringLiteral("删除选中的限速规则"));

    m_clearRateLimitButton = new QPushButton(m_rateLimitPage);
    m_clearRateLimitButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_clearRateLimitButton->setToolTip(QStringLiteral("清空全部限速规则"));

    m_rateLimitControlLayout->addWidget(pidLabel);
    m_rateLimitControlLayout->addWidget(m_rateLimitPidEdit);
    m_rateLimitControlLayout->addWidget(kbpsLabel);
    m_rateLimitControlLayout->addWidget(m_rateLimitKBpsSpin);
    m_rateLimitControlLayout->addWidget(suspendMsLabel);
    m_rateLimitControlLayout->addWidget(m_rateLimitSuspendMsSpin);
    m_rateLimitControlLayout->addWidget(m_applyRateLimitButton);
    m_rateLimitControlLayout->addWidget(m_removeRateLimitButton);
    m_rateLimitControlLayout->addWidget(m_clearRateLimitButton);
    m_rateLimitControlLayout->addStretch(1);

    m_rateLimitLayout->addLayout(m_rateLimitControlLayout);

    // 规则表：展示 PID、阈值、触发计数、当前状态。
    m_rateLimitTable = new QTableWidget(m_rateLimitPage);
    m_rateLimitTable->setColumnCount(toRateLimitColumn(RateLimitTableColumn::Count));
    m_rateLimitTable->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("限速KB/s"),
        QStringLiteral("挂起ms"),
        QStringLiteral("触发次数"),
        QStringLiteral("窗口字节"),
        QStringLiteral("状态")
        });
    m_rateLimitTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rateLimitTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rateLimitTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rateLimitTable->verticalHeader()->setVisible(false);
    // 限速表也使用 Stretch，降低定时刷新时的 UI 重排开销。
    m_rateLimitTable->horizontalHeader()->setStretchLastSection(true);
    m_rateLimitTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_rateLimitTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rateLimitLayout->addWidget(m_rateLimitTable, 1);

    // 限速动作日志：便于查看挂起/恢复执行结果。
    m_rateLimitLogOutput = new QPlainTextEdit(m_rateLimitPage);
    m_rateLimitLogOutput->setReadOnly(true);
    m_rateLimitLogOutput->setMaximumBlockCount(400);
    // 日志框使用按控件宽度自动换行，避免窄窗口出现横向滚动条。
    m_rateLimitLogOutput->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_rateLimitLogOutput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rateLimitLogOutput->setPlaceholderText(QStringLiteral("限速动作日志将显示在这里..."));
    m_rateLimitLayout->addWidget(m_rateLimitLogOutput, 1);

    m_sideTabWidget->addTab(m_rateLimitPage, QIcon(":/Icon/process_priority.svg"), QStringLiteral("进程限速"));
}

void NetworkDock::initializeConnectionManageTab()
{
    // 连接管理页职责：
    // 1) 展示 TCP 连接表；
    // 2) 展示 UDP 端点表；
    // 3) 提供终止 TCP 连接与刷新控制能力。
    m_connectionManagePage = new QWidget(this);
    m_connectionManageLayout = new QVBoxLayout(m_connectionManagePage);
    m_connectionManageLayout->setContentsMargins(6, 6, 6, 6);
    m_connectionManageLayout->setSpacing(6);

    // 顶部控制栏：手动刷新 / 自动刷新开关 / 终止连接按钮 / 状态标签。
    m_connectionControlLayout = new QHBoxLayout();
    m_connectionControlLayout->setSpacing(6);

    m_refreshConnectionButton = new QPushButton(m_connectionManagePage);
    m_refreshConnectionButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_refreshConnectionButton->setToolTip(QStringLiteral("立即刷新 TCP/UDP 连接快照"));

    m_autoRefreshConnectionButton = new QPushButton(m_connectionManagePage);
    m_autoRefreshConnectionButton->setCheckable(true);
    m_autoRefreshConnectionButton->setChecked(true);
    m_autoRefreshConnectionButton->setIcon(QIcon(":/Icon/process_pause.svg"));
    m_autoRefreshConnectionButton->setToolTip(QStringLiteral("自动刷新开关（已开启）"));

    m_terminateTcpButton = new QPushButton(m_connectionManagePage);
    m_terminateTcpButton->setIcon(QIcon(":/Icon/process_uncritical.svg"));
    m_terminateTcpButton->setToolTip(QStringLiteral("终止当前选中的 TCP 连接（DELETE_TCB）"));

    m_connectionStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_connectionManagePage);
    m_connectionStatusLabel->setWordWrap(true);
    m_connectionStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_connectionControlLayout->addWidget(m_refreshConnectionButton);
    m_connectionControlLayout->addWidget(m_autoRefreshConnectionButton);
    m_connectionControlLayout->addWidget(m_terminateTcpButton);
    m_connectionControlLayout->addWidget(m_connectionStatusLabel, 1);
    m_connectionManageLayout->addLayout(m_connectionControlLayout);

    // 子页签：分别展示 TCP 与 UDP。
    m_connectionSubTabWidget = new QTabWidget(m_connectionManagePage);
    m_connectionSubTabWidget->setTabPosition(QTabWidget::North);
    m_connectionManageLayout->addWidget(m_connectionSubTabWidget, 1);

    // TCP 表：状态、PID、进程、本地端点、远端端点。
    m_tcpConnectionTable = new QTableWidget(m_connectionManagePage);
    m_tcpConnectionTable->setColumnCount(toTcpConnectionColumn(TcpConnectionTableColumn::Count));
    m_tcpConnectionTable->setHorizontalHeaderLabels({
        QStringLiteral("状态"),
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点"),
        QStringLiteral("远端端点")
        });
    m_tcpConnectionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tcpConnectionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tcpConnectionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tcpConnectionTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tcpConnectionTable->verticalHeader()->setVisible(false);
    m_tcpConnectionTable->horizontalHeader()->setStretchLastSection(true);
    m_tcpConnectionTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_tcpConnectionTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_connectionSubTabWidget->addTab(m_tcpConnectionTable, QIcon(":/Icon/process_main.svg"), QStringLiteral("TCP"));

    // UDP 表：PID、进程、本地端点。
    m_udpEndpointTable = new QTableWidget(m_connectionManagePage);
    m_udpEndpointTable->setColumnCount(toUdpEndpointColumn(UdpEndpointTableColumn::Count));
    m_udpEndpointTable->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("进程"),
        QStringLiteral("本地端点")
        });
    m_udpEndpointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_udpEndpointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_udpEndpointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_udpEndpointTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_udpEndpointTable->verticalHeader()->setVisible(false);
    m_udpEndpointTable->horizontalHeader()->setStretchLastSection(true);
    m_udpEndpointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_udpEndpointTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_connectionSubTabWidget->addTab(m_udpEndpointTable, QIcon(":/Icon/process_main.svg"), QStringLiteral("UDP"));

    m_sideTabWidget->addTab(m_connectionManagePage, QIcon(":/Icon/process_details.svg"), QStringLiteral("连接管理"));

    kLogEvent initConnectionTabEvent;
    info << initConnectionTabEvent << "[NetworkDock] 连接管理页初始化完成（TCP/UDP）。" << eol;
}

void NetworkDock::initializeManualRequestTab()
{
    // 请求构造页采用“分组参数 + 执行结果”布局，风格对齐 CreateProcess 页。
    m_manualRequestPage = new QWidget(this);
    m_manualRequestLayout = new QVBoxLayout(m_manualRequestPage);
    m_manualRequestLayout->setContentsMargins(6, 6, 6, 6);
    m_manualRequestLayout->setSpacing(6);

    // 关键改动（响应“窄窗口不出现滚动条”）：
    // 1) 移除外层 QScrollArea，避免页面在宽度变化时出现额外滚动条；
    // 2) 改为直接把各功能分组挂到页面主布局，交给布局系统自适应压缩。
    QWidget* contentWidget = m_manualRequestPage;
    QVBoxLayout* contentLayout = m_manualRequestLayout;
    contentLayout->setContentsMargins(2, 2, 2, 2);
    contentLayout->setSpacing(8);

    // 1) API 与 socket 参数分组：支持模式切换和手动覆盖底层参数。
    QGroupBox* apiGroup = new QGroupBox(QStringLiteral("API 与 Socket 参数"), contentWidget);
    QGridLayout* apiLayout = new QGridLayout(apiGroup);
    apiLayout->setHorizontalSpacing(8);
    apiLayout->setVerticalSpacing(6);
    apiLayout->setColumnStretch(1, 1);
    apiLayout->setColumnStretch(3, 1);

    m_manualApiCombo = new QComboBox(apiGroup);
    m_manualApiCombo->addItem(QStringLiteral("WinSock TCP 请求"), static_cast<int>(ks::network::ManualNetworkApiKind::WinSockTcp));
    m_manualApiCombo->addItem(QStringLiteral("WinSock UDP 请求"), static_cast<int>(ks::network::ManualNetworkApiKind::WinSockUdp));
    m_manualApiCombo->setToolTip(QStringLiteral("切换请求模式：TCP（connect/send/recv）或 UDP。"));

    m_manualOverrideSocketParameterCheck = new QCheckBox(QStringLiteral("手动覆盖 socket 参数"), apiGroup);
    m_manualOverrideSocketParameterCheck->setToolTip(
        QStringLiteral("关闭时按模式自动填充 socketType/protocol；开启后使用你填写的底层参数。"));

    m_manualAddressFamilyEdit = new QLineEdit(QStringLiteral("2"), apiGroup);     // AF_INET
    m_manualSocketTypeEdit = new QLineEdit(QStringLiteral("1"), apiGroup);        // SOCK_STREAM
    m_manualProtocolEdit = new QLineEdit(QStringLiteral("6"), apiGroup);          // IPPROTO_TCP
    m_manualSocketFlagsEdit = new QLineEdit(QStringLiteral("0x1"), apiGroup);     // WSA_FLAG_OVERLAPPED
    m_manualAddressFamilyEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_manualSocketTypeEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_manualProtocolEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_manualSocketFlagsEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    apiLayout->addWidget(new QLabel(QStringLiteral("请求模式:"), apiGroup), 0, 0);
    apiLayout->addWidget(m_manualApiCombo, 0, 1, 1, 3);
    apiLayout->addWidget(m_manualOverrideSocketParameterCheck, 1, 0, 1, 4);
    apiLayout->addWidget(new QLabel(QStringLiteral("addressFamily:"), apiGroup), 2, 0);
    apiLayout->addWidget(m_manualAddressFamilyEdit, 2, 1);
    apiLayout->addWidget(new QLabel(QStringLiteral("socketType:"), apiGroup), 2, 2);
    apiLayout->addWidget(m_manualSocketTypeEdit, 2, 3);
    apiLayout->addWidget(new QLabel(QStringLiteral("protocol:"), apiGroup), 3, 0);
    apiLayout->addWidget(m_manualProtocolEdit, 3, 1);
    apiLayout->addWidget(new QLabel(QStringLiteral("socketFlags:"), apiGroup), 3, 2);
    apiLayout->addWidget(m_manualSocketFlagsEdit, 3, 3);
    contentLayout->addWidget(apiGroup);

    // 2) 端点与行为参数分组：覆盖 bind/connect/超时/选项等细节。
    QGroupBox* endpointGroup = new QGroupBox(QStringLiteral("端点与行为参数"), contentWidget);
    QGridLayout* endpointLayout = new QGridLayout(endpointGroup);
    endpointLayout->setHorizontalSpacing(8);
    endpointLayout->setVerticalSpacing(6);
    endpointLayout->setColumnStretch(1, 1);
    endpointLayout->setColumnStretch(3, 1);

    m_manualEnableBindCheck = new QCheckBox(QStringLiteral("启用本地 bind"), endpointGroup);
    m_manualEnableBindCheck->setToolTip(QStringLiteral("勾选后先 bind(localAddress:localPort) 再执行连接/发送。"));
    m_manualLocalAddressEdit = new QLineEdit(QStringLiteral("0.0.0.0"), endpointGroup);
    m_manualLocalAddressEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_manualLocalPortSpin = new QSpinBox(endpointGroup);
    m_manualLocalPortSpin->setRange(0, 65535);
    m_manualLocalPortSpin->setValue(0);

    m_manualRemoteAddressEdit = new QLineEdit(QStringLiteral("127.0.0.1"), endpointGroup);
    m_manualRemoteAddressEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_manualRemotePortSpin = new QSpinBox(endpointGroup);
    m_manualRemotePortSpin->setRange(0, 65535);
    m_manualRemotePortSpin->setValue(80);

    m_manualConnectBeforeSendCheck = new QCheckBox(QStringLiteral("发送前 connect"), endpointGroup);
    m_manualConnectBeforeSendCheck->setChecked(true);
    m_manualConnectBeforeSendCheck->setToolTip(QStringLiteral("UDP 关闭该项时会改用 sendto/recvfrom。"));

    m_manualReuseAddressCheck = new QCheckBox(QStringLiteral("SO_REUSEADDR"), endpointGroup);
    m_manualNoDelayCheck = new QCheckBox(QStringLiteral("TCP_NODELAY"), endpointGroup);

    m_manualSendTimeoutSpin = new QSpinBox(endpointGroup);
    m_manualSendTimeoutSpin->setRange(0, 60 * 60 * 1000);
    m_manualSendTimeoutSpin->setValue(3000);
    m_manualRecvTimeoutSpin = new QSpinBox(endpointGroup);
    m_manualRecvTimeoutSpin->setRange(0, 60 * 60 * 1000);
    m_manualRecvTimeoutSpin->setValue(3000);

    m_manualSendFlagsEdit = new QLineEdit(QStringLiteral("0"), endpointGroup);
    m_manualReceiveFlagsEdit = new QLineEdit(QStringLiteral("0"), endpointGroup);
    m_manualSendFlagsEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_manualReceiveFlagsEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    endpointLayout->addWidget(m_manualEnableBindCheck, 0, 0, 1, 2);
    endpointLayout->addWidget(new QLabel(QStringLiteral("本地地址:"), endpointGroup), 1, 0);
    endpointLayout->addWidget(m_manualLocalAddressEdit, 1, 1);
    endpointLayout->addWidget(new QLabel(QStringLiteral("本地端口:"), endpointGroup), 1, 2);
    endpointLayout->addWidget(m_manualLocalPortSpin, 1, 3);
    endpointLayout->addWidget(new QLabel(QStringLiteral("远端地址:"), endpointGroup), 2, 0);
    endpointLayout->addWidget(m_manualRemoteAddressEdit, 2, 1);
    endpointLayout->addWidget(new QLabel(QStringLiteral("远端端口:"), endpointGroup), 2, 2);
    endpointLayout->addWidget(m_manualRemotePortSpin, 2, 3);
    endpointLayout->addWidget(m_manualConnectBeforeSendCheck, 3, 0, 1, 2);
    endpointLayout->addWidget(m_manualReuseAddressCheck, 3, 2);
    endpointLayout->addWidget(m_manualNoDelayCheck, 3, 3);
    endpointLayout->addWidget(new QLabel(QStringLiteral("发送超时(ms):"), endpointGroup), 4, 0);
    endpointLayout->addWidget(m_manualSendTimeoutSpin, 4, 1);
    endpointLayout->addWidget(new QLabel(QStringLiteral("接收超时(ms):"), endpointGroup), 4, 2);
    endpointLayout->addWidget(m_manualRecvTimeoutSpin, 4, 3);
    endpointLayout->addWidget(new QLabel(QStringLiteral("sendFlags:"), endpointGroup), 5, 0);
    endpointLayout->addWidget(m_manualSendFlagsEdit, 5, 1);
    endpointLayout->addWidget(new QLabel(QStringLiteral("recvFlags:"), endpointGroup), 5, 2);
    endpointLayout->addWidget(m_manualReceiveFlagsEdit, 5, 3);
    contentLayout->addWidget(endpointGroup);

    // 3) 载荷与接收参数分组：支持文本/HEX 输入与响应读取开关。
    QGroupBox* payloadGroup = new QGroupBox(QStringLiteral("载荷与响应读取"), contentWidget);
    QGridLayout* payloadLayout = new QGridLayout(payloadGroup);
    payloadLayout->setHorizontalSpacing(8);
    payloadLayout->setVerticalSpacing(6);
    payloadLayout->setColumnStretch(1, 1);
    payloadLayout->setColumnStretch(4, 1);

    m_manualPayloadFormatCombo = new QComboBox(payloadGroup);
    m_manualPayloadFormatCombo->addItem(QStringLiteral("ASCII 文本"), static_cast<int>(ks::network::ManualPayloadFormat::AsciiText));
    m_manualPayloadFormatCombo->addItem(QStringLiteral("十六进制字节"), static_cast<int>(ks::network::ManualPayloadFormat::HexBytes));
    m_manualPayloadFormatCombo->setToolTip(QStringLiteral("十六进制模式示例：48 65 6C 6C 6F"));

    m_manualPayloadEditor = new QPlainTextEdit(payloadGroup);
    m_manualPayloadEditor->setPlaceholderText(QStringLiteral("在此输入请求载荷。"));
    m_manualPayloadEditor->setFixedHeight(96);
    m_manualPayloadEditor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_manualPayloadEditor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_manualReceiveAfterSendCheck = new QCheckBox(QStringLiteral("发送后读取响应"), payloadGroup);
    m_manualReceiveAfterSendCheck->setChecked(true);
    m_manualReceiveMaxBytesSpin = new QSpinBox(payloadGroup);
    m_manualReceiveMaxBytesSpin->setRange(1, 1024 * 1024);
    m_manualReceiveMaxBytesSpin->setValue(4096);
    m_manualShutdownSendCheck = new QCheckBox(QStringLiteral("发送后 shutdown(SD_SEND)"), payloadGroup);
    m_manualShutdownSendCheck->setChecked(false);

    payloadLayout->addWidget(new QLabel(QStringLiteral("载荷格式:"), payloadGroup), 0, 0);
    payloadLayout->addWidget(m_manualPayloadFormatCombo, 0, 1);
    payloadLayout->addWidget(m_manualReceiveAfterSendCheck, 0, 2);
    payloadLayout->addWidget(new QLabel(QStringLiteral("最大读取字节:"), payloadGroup), 0, 3);
    payloadLayout->addWidget(m_manualReceiveMaxBytesSpin, 0, 4);
    payloadLayout->addWidget(m_manualShutdownSendCheck, 1, 0, 1, 2);
    payloadLayout->addWidget(m_manualPayloadEditor, 2, 0, 1, 5);
    contentLayout->addWidget(payloadGroup);

    // 4) 执行与结果分组：执行请求、重置参数、输出结果日志。
    QGroupBox* actionGroup = new QGroupBox(QStringLiteral("执行与结果"), contentWidget);
    QVBoxLayout* actionLayout = new QVBoxLayout(actionGroup);
    QHBoxLayout* actionButtonLayout = new QHBoxLayout();

    m_manualExecuteButton = new QPushButton(QStringLiteral("执行请求"), actionGroup);
    m_manualExecuteButton->setIcon(QIcon(":/Icon/process_start.svg"));
    m_manualExecuteButton->setToolTip(QStringLiteral("按上方参数立即执行一次网络请求。"));

    m_manualResetButton = new QPushButton(QStringLiteral("恢复默认参数"), actionGroup);
    m_manualResetButton->setIcon(QIcon(":/Icon/log_clear.svg"));
    m_manualResetButton->setToolTip(QStringLiteral("重置请求构造页全部参数。"));

    actionButtonLayout->addWidget(m_manualExecuteButton);
    actionButtonLayout->addWidget(m_manualResetButton);
    actionButtonLayout->addStretch(1);

    m_manualResultOutput = new QPlainTextEdit(actionGroup);
    m_manualResultOutput->setReadOnly(true);
    m_manualResultOutput->setMaximumBlockCount(800);
    m_manualResultOutput->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_manualResultOutput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_manualResultOutput->setPlaceholderText(QStringLiteral("执行结果、响应摘要和错误码会显示在这里。"));

    actionLayout->addLayout(actionButtonLayout);
    actionLayout->addWidget(m_manualResultOutput, 1);
    contentLayout->addWidget(actionGroup, 1);

    // 构建完成后先按默认模式回填一组“可直接执行”的参数。
    resetManualRequestForm();

    m_sideTabWidget->addTab(m_manualRequestPage, QIcon(":/Icon/process_main.svg"), QStringLiteral("请求构造"));

    kLogEvent initManualTabEvent;
    info << initManualTabEvent << "[NetworkDock] 请求构造页初始化完成。" << eol;
}

