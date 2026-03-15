#include "NetworkDock.h"

#include <QAction>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm> // std::min：限制预览与 hexdump 显示长度。
#include <sstream>   // std::ostringstream：详情窗口文本拼接。
#include <string>    // std::string：日志文本桥接。
#include <vector>    // std::vector：批量刷新队列临时缓存。

namespace
{
    // toQString：std::string -> QString 转换辅助。
    QString toQString(const std::string& textValue)
    {
        return QString::fromUtf8(textValue.c_str());
    }

    // formatEndpointText：把“地址 + 端口”格式化为地址端点文本。
    QString formatEndpointText(const std::string& ipAddress, const std::uint16_t portNumber)
    {
        return QString("%1:%2").arg(toQString(ipAddress)).arg(portNumber);
    }

    // formatPacketPreview：
    // - 生成报文负载的短预览文本（十六进制）；
    // - 用于列表展示，不替代完整详情查看。
    QString formatPacketPreview(const ks::network::PacketRecord& packetRecord)
    {
        if (packetRecord.packetBytes.empty() || packetRecord.payloadOffset >= packetRecord.packetBytes.size())
        {
            return QStringLiteral("<empty>");
        }

        constexpr std::size_t kPreviewBytes = 24;
        const std::size_t maxReadableLength = packetRecord.packetBytes.size() - packetRecord.payloadOffset;
        const std::size_t previewLength = std::min<std::size_t>(kPreviewBytes, maxReadableLength);

        QString previewText;
        for (std::size_t index = 0; index < previewLength; ++index)
        {
            const std::uint8_t currentByte = packetRecord.packetBytes[packetRecord.payloadOffset + index];
            previewText += QString("%1 ").arg(static_cast<unsigned>(currentByte), 2, 16, QChar('0')).toUpper();
        }

        previewText = previewText.trimmed();
        if (previewLength < maxReadableLength)
        {
            previewText += QStringLiteral(" ...");
        }
        if (packetRecord.packetBytesTruncated)
        {
            previewText += QStringLiteral(" [truncated]");
        }
        return previewText;
    }

    // buildHexDump：
    // - 构造“偏移 + 十六进制 + ASCII”三栏文本；
    // - 供报文详情窗口直接显示。
    QString buildHexDump(const ks::network::PacketRecord& packetRecord)
    {
        if (packetRecord.packetBytes.empty())
        {
            return QStringLiteral("No packet bytes captured.");
        }

        std::ostringstream stream;
        constexpr std::size_t kBytesPerLine = 16;
        for (std::size_t offset = 0; offset < packetRecord.packetBytes.size(); offset += kBytesPerLine)
        {
            // 行首偏移。
            stream << std::hex << std::uppercase;
            stream.width(6);
            stream.fill('0');
            stream << offset << "  ";

            // 十六进制区。
            for (std::size_t column = 0; column < kBytesPerLine; ++column)
            {
                const std::size_t byteIndex = offset + column;
                if (byteIndex < packetRecord.packetBytes.size())
                {
                    stream.width(2);
                    stream.fill('0');
                    stream << static_cast<unsigned>(packetRecord.packetBytes[byteIndex]) << ' ';
                }
                else
                {
                    stream << "   ";
                }
            }

            stream << " ";

            // ASCII 区。
            for (std::size_t column = 0; column < kBytesPerLine; ++column)
            {
                const std::size_t byteIndex = offset + column;
                if (byteIndex >= packetRecord.packetBytes.size())
                {
                    break;
                }
                const unsigned char currentByte = packetRecord.packetBytes[byteIndex];
                if (currentByte >= 32 && currentByte <= 126)
                {
                    stream << static_cast<char>(currentByte);
                }
                else
                {
                    stream << '.';
                }
            }

            stream << "\n";
        }
        return QString::fromStdString(stream.str());
    }

    // createPacketCell：
    // - 统一创建只读单元格；
    // - 避免每列重复设置 item flag。
    QTableWidgetItem* createPacketCell(const QString& cellText)
    {
        QTableWidgetItem* tableItem = new QTableWidgetItem(cellText);
        tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        return tableItem;
    }

    // PacketDetailWindow：
    // - 报文详情独立窗口（show 非阻塞）；
    // - 用于完整查看包头/负载十六进制内容。
    class PacketDetailWindow final : public QWidget
    {
    public:
        explicit PacketDetailWindow(const ks::network::PacketRecord& packetRecord, QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setAttribute(Qt::WA_DeleteOnClose, true);
            setWindowTitle(QStringLiteral("报文详情 - #%1").arg(packetRecord.sequenceId));
            resize(920, 600);

            QVBoxLayout* rootLayout = new QVBoxLayout(this);
            rootLayout->setContentsMargins(8, 8, 8, 8);
            rootLayout->setSpacing(6);

            // 元信息区域：时间、协议、PID、端点、长度。
            QLabel* metaLabel = new QLabel(this);
            const QString timeText = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(packetRecord.captureTimestampMs)).toString("yyyy-MM-dd HH:mm:ss.zzz");
            metaLabel->setText(QStringLiteral(
                "时间: %1\n协议: %2  方向: %3\nPID: %4  进程: %5\n本地: %6\n远端: %7\n总长度: %8 bytes, 负载: %9 bytes")
                .arg(timeText)
                .arg(toQString(ks::network::PacketProtocolToString(packetRecord.protocol)))
                .arg(toQString(ks::network::PacketDirectionToString(packetRecord.direction)))
                .arg(packetRecord.processId)
                .arg(toQString(packetRecord.processName))
                .arg(formatEndpointText(packetRecord.localAddress, packetRecord.localPort))
                .arg(formatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort))
                .arg(packetRecord.totalPacketSize)
                .arg(packetRecord.payloadSize));
            metaLabel->setWordWrap(true);
            rootLayout->addWidget(metaLabel);

            // 主体区域：完整 hexdump。
            QPlainTextEdit* hexDumpEdit = new QPlainTextEdit(this);
            hexDumpEdit->setReadOnly(true);
            hexDumpEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
            hexDumpEdit->setPlainText(buildHexDump(packetRecord));
            rootLayout->addWidget(hexDumpEdit, 1);
        }
    };

    // populatePacketRow：
    // - 在指定表格行写入报文展示列；
    // - monitor/pid 两个表格复用同一渲染逻辑。
    void populatePacketRow(
        QTableWidget* tableWidget,
        const int rowIndex,
        const ks::network::PacketRecord& packetRecord,
        const std::uint64_t sequenceId,
        const QIcon& processIcon)
    {
        if (tableWidget == nullptr || rowIndex < 0)
        {
            return;
        }

        const QString timeText = QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(packetRecord.captureTimestampMs)).toString("HH:mm:ss.zzz");
        const QString protocolText = toQString(ks::network::PacketProtocolToString(packetRecord.protocol));
        const QString directionText = toQString(ks::network::PacketDirectionToString(packetRecord.direction));
        const QString pidText = QString::number(packetRecord.processId);
        const QString processNameText = toQString(packetRecord.processName);
        const QString localEndpointText = formatEndpointText(packetRecord.localAddress, packetRecord.localPort);
        const QString remoteEndpointText = formatEndpointText(packetRecord.remoteAddress, packetRecord.remotePort);
        const QString packetSizeText = QString::number(packetRecord.totalPacketSize);
        const QString payloadSizeText = QString::number(packetRecord.payloadSize);
        const QString previewText = formatPacketPreview(packetRecord);

        QTableWidgetItem* timeItem = createPacketCell(timeText);
        timeItem->setData(Qt::UserRole, static_cast<qulonglong>(sequenceId));
        tableWidget->setItem(rowIndex, 0, timeItem);
        tableWidget->setItem(rowIndex, 1, createPacketCell(protocolText));
        tableWidget->setItem(rowIndex, 2, createPacketCell(directionText));
        tableWidget->setItem(rowIndex, 3, createPacketCell(pidText));
        QTableWidgetItem* processNameItem = createPacketCell(processNameText);
        processNameItem->setIcon(processIcon);
        tableWidget->setItem(rowIndex, 4, processNameItem);
        tableWidget->setItem(rowIndex, 5, createPacketCell(localEndpointText));
        tableWidget->setItem(rowIndex, 6, createPacketCell(remoteEndpointText));
        tableWidget->setItem(rowIndex, 7, createPacketCell(packetSizeText));
        tableWidget->setItem(rowIndex, 8, createPacketCell(payloadSizeText));
        tableWidget->setItem(rowIndex, 9, createPacketCell(previewText));
    }
}

NetworkDock::NetworkDock(QWidget* parent)
    : QWidget(parent)
{
    // 创建后台服务对象：负责抓包、PID 映射、限速逻辑。
    m_trafficService = std::make_unique<ks::network::TrafficMonitorService>();

    // 初始化界面和连接逻辑。
    initializeUi();
    initializeConnections();

    // 限速页使用定时器轮询刷新规则状态（触发次数、当前窗口字节等）。
    m_rateLimitRefreshTimer = new QTimer(this);
    m_rateLimitRefreshTimer->setInterval(700);
    connect(m_rateLimitRefreshTimer, &QTimer::timeout, this, [this]()
        {
            refreshRateLimitTable();
        });
    m_rateLimitRefreshTimer->start();

    // 报文批量刷新定时器：
    // - 由 UI 线程周期性批量消费后台队列；
    // - 避免“每包一个 invokeMethod”把事件循环塞爆。
    m_packetFlushTimer = new QTimer(this);
    m_packetFlushTimer->setInterval(45);
    connect(m_packetFlushTimer, &QTimer::timeout, this, [this]()
        {
            flushPendingPacketsToUi();
        });
    m_packetFlushTimer->start();

    // 把后台线程回调转发到 UI 线程，保证表格操作线程安全。
    m_trafficService->SetPacketCallback([this](const ks::network::PacketRecord& packetRecord)
        {
            // 抓包线程仅执行“入队”轻量动作，不直接碰 UI 控件。
            std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
            if (m_pendingPacketQueue.size() >= kMaxPendingPacketQueueCount)
            {
                // 队列满时丢弃最旧报文，保持系统持续可用。
                m_pendingPacketQueue.pop_front();
                ++m_droppedPacketCount;
            }
            m_pendingPacketQueue.push_back(packetRecord);
        });

    m_trafficService->SetStatusCallback([this](const std::string& statusText)
        {
            QMetaObject::invokeMethod(this, [this, statusText]()
                {
                    onStatusMessageArrived(statusText);
                }, Qt::QueuedConnection);
        });

    m_trafficService->SetRateLimitActionCallback([this](const ks::network::RateLimitActionEvent& actionEvent)
        {
            QMetaObject::invokeMethod(this, [this, actionEvent]()
                {
                    onRateLimitActionArrived(actionEvent);
                }, Qt::QueuedConnection);
        });

    // 初始化日志。
    kLogEvent initializeEvent;
    info << initializeEvent << "[NetworkDock] 网络面板初始化完成。" << eol;
}

NetworkDock::~NetworkDock()
{
    // 窗口销毁前主动停止后台线程，避免析构后回调悬空。
    if (m_trafficService != nullptr)
    {
        m_trafficService->StopCapture();
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[NetworkDock] 网络面板已析构，抓包线程已停止。" << eol;
}

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
    initializePidFilterTab();
    initializeRateLimitTab();
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
    m_monitorStatusLabel->setMinimumWidth(220);

    m_monitorControlLayout->addWidget(m_startMonitorButton);
    m_monitorControlLayout->addWidget(m_stopMonitorButton);
    m_monitorControlLayout->addWidget(m_clearPacketButton);
    m_monitorControlLayout->addWidget(m_monitorStatusLabel);
    m_monitorControlLayout->addStretch(1);

    m_trafficMonitorLayout->addLayout(m_monitorControlLayout);

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
    m_packetTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_packetTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_packetTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_packetTable->verticalHeader()->setVisible(false);
    m_packetTable->horizontalHeader()->setStretchLastSection(true);
    m_packetTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    m_trafficMonitorLayout->addWidget(m_packetTable, 1);
    m_sideTabWidget->addTab(m_trafficMonitorPage, QIcon(":/Icon/process_main.svg"), QStringLiteral("流量监控"));
}

void NetworkDock::initializePidFilterTab()
{
    m_pidFilterPage = new QWidget(this);
    m_pidFilterLayout = new QVBoxLayout(m_pidFilterPage);
    m_pidFilterLayout->setContentsMargins(6, 6, 6, 6);
    m_pidFilterLayout->setSpacing(6);

    // 控制栏：输入 PID -> 应用/清除筛查。
    m_pidFilterControlLayout = new QHBoxLayout();
    m_pidFilterControlLayout->setSpacing(6);

    QLabel* pidInputLabel = new QLabel(QStringLiteral("目标 PID:"), m_pidFilterPage);
    m_pidFilterEdit = new QLineEdit(m_pidFilterPage);
    m_pidFilterEdit->setPlaceholderText(QStringLiteral("输入十进制 PID，例如 1234"));

    m_applyPidFilterButton = new QPushButton(m_pidFilterPage);
    m_applyPidFilterButton->setIcon(QIcon(":/Icon/log_track.svg"));
    m_applyPidFilterButton->setToolTip(QStringLiteral("按指定 PID 筛查流量"));

    m_clearPidFilterButton = new QPushButton(m_pidFilterPage);
    m_clearPidFilterButton->setIcon(QIcon(":/Icon/log_cancel_track.svg"));
    m_clearPidFilterButton->setToolTip(QStringLiteral("取消 PID 筛查并清空结果表"));

    m_pidFilterStateLabel = new QLabel(QStringLiteral("当前筛查：无"), m_pidFilterPage);
    m_pidFilterStateLabel->setMinimumWidth(180);

    m_pidFilterControlLayout->addWidget(pidInputLabel);
    m_pidFilterControlLayout->addWidget(m_pidFilterEdit, 1);
    m_pidFilterControlLayout->addWidget(m_applyPidFilterButton);
    m_pidFilterControlLayout->addWidget(m_clearPidFilterButton);
    m_pidFilterControlLayout->addWidget(m_pidFilterStateLabel);

    m_pidFilterLayout->addLayout(m_pidFilterControlLayout);

    // PID 结果表：列结构与主表一致，便于对比。
    m_pidFilterTable = new QTableWidget(m_pidFilterPage);
    m_pidFilterTable->setColumnCount(toPacketColumn(PacketTableColumn::Count));
    m_pidFilterTable->setHorizontalHeaderLabels({
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
    m_pidFilterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pidFilterTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pidFilterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pidFilterTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_pidFilterTable->verticalHeader()->setVisible(false);
    m_pidFilterTable->horizontalHeader()->setStretchLastSection(true);
    m_pidFilterTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    m_pidFilterLayout->addWidget(m_pidFilterTable, 1);
    m_sideTabWidget->addTab(m_pidFilterPage, QIcon(":/Icon/log_track.svg"), QStringLiteral("PID筛查"));
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
    m_rateLimitTable->horizontalHeader()->setStretchLastSection(true);
    m_rateLimitTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_rateLimitLayout->addWidget(m_rateLimitTable, 1);

    // 限速动作日志：便于查看挂起/恢复执行结果。
    m_rateLimitLogOutput = new QPlainTextEdit(m_rateLimitPage);
    m_rateLimitLogOutput->setReadOnly(true);
    m_rateLimitLogOutput->setMaximumBlockCount(400);
    m_rateLimitLogOutput->setPlaceholderText(QStringLiteral("限速动作日志将显示在这里..."));
    m_rateLimitLayout->addWidget(m_rateLimitLogOutput, 1);

    m_sideTabWidget->addTab(m_rateLimitPage, QIcon(":/Icon/process_priority.svg"), QStringLiteral("进程限速"));
}

void NetworkDock::initializeConnections()
{
    // 启停抓包与清空表格按钮连接。
    connect(m_startMonitorButton, &QPushButton::clicked, this, [this]()
        {
            startTrafficMonitor();
        });
    connect(m_stopMonitorButton, &QPushButton::clicked, this, [this]()
        {
            stopTrafficMonitor();
        });
    connect(m_clearPacketButton, &QPushButton::clicked, this, [this]()
        {
            clearAllPacketRows();
        });

    // PID 筛查控制连接。
    connect(m_applyPidFilterButton, &QPushButton::clicked, this, [this]()
        {
            applyPidFilter();
        });
    connect(m_clearPidFilterButton, &QPushButton::clicked, this, [this]()
        {
            clearPidFilter();
        });

    // 限速规则控制连接。
    connect(m_applyRateLimitButton, &QPushButton::clicked, this, [this]()
        {
            applyOrUpdateRateLimitRule();
        });
    connect(m_removeRateLimitButton, &QPushButton::clicked, this, [this]()
        {
            removeSelectedRateLimitRule();
        });
    connect(m_clearRateLimitButton, &QPushButton::clicked, this, [this]()
        {
            clearAllRateLimitRules();
        });

    // 双击任意报文行，打开独立详情窗口（非阻塞）。
    connect(m_packetTable, &QTableWidget::cellDoubleClicked, this,
        [this](const int row, const int /*column*/)
        {
            openPacketDetailWindowFromTableRow(m_packetTable, row);
        });
    connect(m_pidFilterTable, &QTableWidget::cellDoubleClicked, this,
        [this](const int row, const int /*column*/)
        {
            openPacketDetailWindowFromTableRow(m_pidFilterTable, row);
        });

    // 右键菜单：提供“查看报文详情”入口。
    connect(m_packetTable, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position)
        {
            const QModelIndex index = m_packetTable->indexAt(position);
            if (!index.isValid())
            {
                return;
            }

            QMenu contextMenu(this);
            QAction* detailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("查看报文详情"));
            QAction* selectedAction = contextMenu.exec(m_packetTable->viewport()->mapToGlobal(position));
            if (selectedAction == detailAction)
            {
                openPacketDetailWindowFromTableRow(m_packetTable, index.row());
            }
        });

    connect(m_pidFilterTable, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position)
        {
            const QModelIndex index = m_pidFilterTable->indexAt(position);
            if (!index.isValid())
            {
                return;
            }

            QMenu contextMenu(this);
            QAction* detailAction = contextMenu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("查看报文详情"));
            QAction* selectedAction = contextMenu.exec(m_pidFilterTable->viewport()->mapToGlobal(position));
            if (selectedAction == detailAction)
            {
                openPacketDetailWindowFromTableRow(m_pidFilterTable, index.row());
            }
        });

    // 初始化按钮可用状态。
    updateMonitorButtonState();
}

void NetworkDock::startTrafficMonitor()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    // 每次启动前清零“后台队列丢包计数”，便于观察本次运行状态。
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        m_droppedPacketCount = 0;
    }

    const bool startIssued = m_trafficService->StartCapture();
    m_monitorRunning = startIssued && m_trafficService->IsRunning();
    if (!startIssued)
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：启动失败"));
    }
    else
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：启动中..."));
    }

    updateMonitorButtonState();

    kLogEvent startEvent;
    info << startEvent << "[NetworkDock] 用户触发网络监控启动。" << eol;
}

void NetworkDock::stopTrafficMonitor()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    m_trafficService->StopCapture();
    m_monitorRunning = false;
    m_monitorStatusLabel->setText(QStringLiteral("状态：已停止"));
    updateMonitorButtonState();

    kLogEvent stopEvent;
    info << stopEvent << "[NetworkDock] 用户触发网络监控停止。" << eol;
}

void NetworkDock::updateMonitorButtonState()
{
    if (m_startMonitorButton != nullptr)
    {
        m_startMonitorButton->setEnabled(!m_monitorRunning);
    }
    if (m_stopMonitorButton != nullptr)
    {
        m_stopMonitorButton->setEnabled(m_monitorRunning);
    }
}

void NetworkDock::onPacketCaptured(const ks::network::PacketRecord& packetRecord)
{
    // 缓存报文实体：用于详情窗口通过序号回查完整字节内容。
    m_packetSequenceOrder.push_back(packetRecord.sequenceId);
    m_packetBySequence[packetRecord.sequenceId] = packetRecord;

    appendPacketToMonitorTable(packetRecord);
    appendPacketToPidFilterTableIfNeeded(packetRecord);
    trimOldestPacketWhenNeeded();
}

void NetworkDock::flushPendingPacketsToUi()
{
    // 先在锁内把待处理报文批量转移到局部容器，缩短锁持有时间。
    std::vector<ks::network::PacketRecord> packetBatch;
    std::uint64_t droppedCountSnapshot = 0;
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        if (m_pendingPacketQueue.empty() && m_droppedPacketCount == 0)
        {
            return;
        }

        // 单次最多处理固定数量，避免 UI 一帧执行过久。
        constexpr std::size_t kMaxConsumePerTick = 240;
        const std::size_t consumeCount = std::min<std::size_t>(kMaxConsumePerTick, m_pendingPacketQueue.size());
        packetBatch.reserve(consumeCount);
        for (std::size_t index = 0; index < consumeCount; ++index)
        {
            packetBatch.push_back(std::move(m_pendingPacketQueue.front()));
            m_pendingPacketQueue.pop_front();
        }

        droppedCountSnapshot = m_droppedPacketCount;
    }

    // 批量更新表格期间临时关闭重绘，减少 UI 抖动与卡顿。
    if (m_packetTable != nullptr)
    {
        m_packetTable->setUpdatesEnabled(false);
    }
    if (m_pidFilterTable != nullptr)
    {
        m_pidFilterTable->setUpdatesEnabled(false);
    }

    for (const ks::network::PacketRecord& packetRecord : packetBatch)
    {
        onPacketCaptured(packetRecord);
    }

    if (m_packetTable != nullptr)
    {
        m_packetTable->setUpdatesEnabled(true);
        if (!packetBatch.empty())
        {
            m_packetTable->scrollToBottom();
        }
    }
    if (m_pidFilterTable != nullptr)
    {
        m_pidFilterTable->setUpdatesEnabled(true);
        if (!packetBatch.empty() && m_activePidFilter.has_value())
        {
            m_pidFilterTable->scrollToBottom();
        }
    }

    // 丢包计数只在 UI 线程展示，不在每个回调里刷屏。
    if (droppedCountSnapshot > 0 && m_monitorStatusLabel != nullptr)
    {
        const std::size_t pendingCount = [&]()
            {
                std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
                return m_pendingPacketQueue.size();
            }();
        m_monitorStatusLabel->setText(QStringLiteral("状态：高负载（已丢弃%1条，队列%2）")
            .arg(static_cast<qulonglong>(droppedCountSnapshot))
            .arg(static_cast<qulonglong>(pendingCount)));
    }
}

void NetworkDock::onStatusMessageArrived(const std::string& statusText)
{
    const QString statusQString = toQString(statusText);
    if (m_monitorStatusLabel != nullptr)
    {
        m_monitorStatusLabel->setText(QStringLiteral("状态：%1").arg(statusQString));
    }

    // 状态回调可能表示启动失败/线程退出，因此同步刷新按钮状态。
    if (m_trafficService != nullptr)
    {
        m_monitorRunning = m_trafficService->IsRunning();
    }
    updateMonitorButtonState();

    kLogEvent statusEvent;
    info << statusEvent << "[NetworkDock] 抓包状态: " << statusText << eol;
}

void NetworkDock::onRateLimitActionArrived(const ks::network::RateLimitActionEvent& actionEvent)
{
    // 把限速动作以时间行追加到日志框，便于排查“为何某进程被挂起/恢复”。
    const QString timeText = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(actionEvent.timestampMs)).toString("HH:mm:ss.zzz");
    const QString actionText = toQString(ks::network::RateLimitActionTypeToString(actionEvent.actionType));
    const QString resultText = actionEvent.actionSucceeded ? QStringLiteral("成功") : QStringLiteral("失败");
    const QString logLine = QStringLiteral("[%1] PID=%2, 动作=%3, 结果=%4, 详情=%5")
        .arg(timeText)
        .arg(actionEvent.processId)
        .arg(actionText)
        .arg(resultText)
        .arg(toQString(actionEvent.detailText));
    appendRateLimitActionLogLine(logLine);

    refreshRateLimitTable();

    // 动作事件使用 warn 级别，方便在日志面板中显眼查看。
    kLogEvent actionEventLog;
    warn << actionEventLog
        << "[NetworkDock] 限速动作, pid=" << actionEvent.processId
        << ", action=" << ks::network::RateLimitActionTypeToString(actionEvent.actionType)
        << ", ok=" << (actionEvent.actionSucceeded ? "true" : "false")
        << ", detail=" << actionEvent.detailText
        << eol;
}

void NetworkDock::appendPacketToMonitorTable(const ks::network::PacketRecord& packetRecord)
{
    if (m_packetTable == nullptr)
    {
        return;
    }

    const int newRow = m_packetTable->rowCount();
    m_packetTable->insertRow(newRow);
    const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
    populatePacketRow(m_packetTable, newRow, packetRecord, packetRecord.sequenceId, processIcon);
}

void NetworkDock::appendPacketToPidFilterTableIfNeeded(const ks::network::PacketRecord& packetRecord)
{
    if (m_pidFilterTable == nullptr || !m_activePidFilter.has_value())
    {
        return;
    }

    if (packetRecord.processId != m_activePidFilter.value())
    {
        return;
    }

    const int newRow = m_pidFilterTable->rowCount();
    m_pidFilterTable->insertRow(newRow);
    const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
    populatePacketRow(m_pidFilterTable, newRow, packetRecord, packetRecord.sequenceId, processIcon);
}

void NetworkDock::rebuildPidFilterTable()
{
    if (m_pidFilterTable == nullptr)
    {
        return;
    }

    m_pidFilterTable->setRowCount(0);
    if (!m_activePidFilter.has_value())
    {
        return;
    }

    const std::uint32_t expectedPid = m_activePidFilter.value();
    int writeRow = 0;
    for (const std::uint64_t sequenceId : m_packetSequenceOrder)
    {
        const auto iterator = m_packetBySequence.find(sequenceId);
        if (iterator == m_packetBySequence.end())
        {
            continue;
        }
        const ks::network::PacketRecord& packetRecord = iterator->second;
        if (packetRecord.processId != expectedPid)
        {
            continue;
        }

        m_pidFilterTable->insertRow(writeRow);
        const QIcon processIcon = resolveProcessIconByPid(packetRecord.processId, packetRecord.processName);
        populatePacketRow(m_pidFilterTable, writeRow, packetRecord, sequenceId, processIcon);
        ++writeRow;
    }
}

void NetworkDock::trimOldestPacketWhenNeeded()
{
    if (m_packetSequenceOrder.size() <= kMaxPacketCacheCount)
    {
        return;
    }

    // 删除最老序号对应的缓存。
    const std::uint64_t oldestSequenceId = m_packetSequenceOrder.front();
    m_packetSequenceOrder.pop_front();
    m_packetBySequence.erase(oldestSequenceId);

    // 主表按插入顺序显示，删除首行即可。
    if (m_packetTable != nullptr && m_packetTable->rowCount() > 0)
    {
        m_packetTable->removeRow(0);
    }

    // 筛查表可能也包含被删除的报文，这里稳妥重建一次。
    if (m_activePidFilter.has_value())
    {
        rebuildPidFilterTable();
    }
}

void NetworkDock::clearAllPacketRows()
{
    // 同时清空后台待刷新队列，避免“刚清空又回填旧数据”。
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        m_pendingPacketQueue.clear();
        m_droppedPacketCount = 0;
    }

    m_packetSequenceOrder.clear();
    m_packetBySequence.clear();

    if (m_packetTable != nullptr)
    {
        m_packetTable->setRowCount(0);
    }
    if (m_pidFilterTable != nullptr)
    {
        m_pidFilterTable->setRowCount(0);
    }

    kLogEvent clearEvent;
    info << clearEvent << "[NetworkDock] 用户清空了网络报文列表。" << eol;
}

void NetworkDock::applyPidFilter()
{
    if (m_pidFilterEdit == nullptr || m_pidFilterStateLabel == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(m_pidFilterEdit->text(), targetPid))
    {
        QMessageBox::warning(this, QStringLiteral("PID筛查"), QStringLiteral("请输入有效的十进制 PID。"));
        return;
    }

    m_activePidFilter = targetPid;
    m_pidFilterStateLabel->setText(QStringLiteral("当前筛查：PID=%1").arg(targetPid));
    rebuildPidFilterTable();

    kLogEvent filterEvent;
    info << filterEvent << "[NetworkDock] 应用 PID 筛查, pid=" << targetPid << eol;
}

void NetworkDock::clearPidFilter()
{
    m_activePidFilter.reset();
    if (m_pidFilterStateLabel != nullptr)
    {
        m_pidFilterStateLabel->setText(QStringLiteral("当前筛查：无"));
    }
    if (m_pidFilterTable != nullptr)
    {
        m_pidFilterTable->setRowCount(0);
    }
}

void NetworkDock::applyOrUpdateRateLimitRule()
{
    if (m_trafficService == nullptr || m_rateLimitPidEdit == nullptr ||
        m_rateLimitKBpsSpin == nullptr || m_rateLimitSuspendMsSpin == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(m_rateLimitPidEdit->text(), targetPid))
    {
        QMessageBox::warning(this, QStringLiteral("进程限速"), QStringLiteral("请输入有效的 PID。"));
        return;
    }

    ks::network::ProcessRateLimitRule limitRule;
    limitRule.processId = targetPid;
    limitRule.bytesPerSecond = static_cast<std::uint64_t>(m_rateLimitKBpsSpin->value()) * 1024ULL;
    limitRule.suspendDurationMs = static_cast<std::uint32_t>(m_rateLimitSuspendMsSpin->value());
    limitRule.enabled = true;

    m_trafficService->UpsertRateLimitRule(limitRule);
    refreshRateLimitTable();

    appendRateLimitActionLogLine(QStringLiteral("更新限速规则：PID=%1, %2 KB/s, suspend=%3 ms")
        .arg(targetPid)
        .arg(m_rateLimitKBpsSpin->value())
        .arg(m_rateLimitSuspendMsSpin->value()));

    kLogEvent limitEvent;
    info << limitEvent
        << "[NetworkDock] 设置限速规则, pid=" << targetPid
        << ", bytesPerSecond=" << limitRule.bytesPerSecond
        << ", suspendMs=" << limitRule.suspendDurationMs
        << eol;
}

void NetworkDock::removeSelectedRateLimitRule()
{
    if (m_trafficService == nullptr || m_rateLimitTable == nullptr)
    {
        return;
    }

    const int selectedRow = m_rateLimitTable->currentRow();
    if (selectedRow < 0)
    {
        QMessageBox::information(this, QStringLiteral("进程限速"), QStringLiteral("请先选中一条规则。"));
        return;
    }

    QTableWidgetItem* pidItem = m_rateLimitTable->item(selectedRow, toRateLimitColumn(RateLimitTableColumn::Pid));
    if (pidItem == nullptr)
    {
        return;
    }

    std::uint32_t targetPid = 0;
    if (!tryParsePidText(pidItem->text(), targetPid))
    {
        return;
    }

    m_trafficService->RemoveRateLimitRule(targetPid);
    refreshRateLimitTable();
    appendRateLimitActionLogLine(QStringLiteral("删除限速规则：PID=%1").arg(targetPid));
}

void NetworkDock::clearAllRateLimitRules()
{
    if (m_trafficService == nullptr)
    {
        return;
    }

    const int userChoice = QMessageBox::question(
        this,
        QStringLiteral("进程限速"),
        QStringLiteral("确认清空全部限速规则吗？"));
    if (userChoice != QMessageBox::Yes)
    {
        return;
    }

    m_trafficService->ClearRateLimitRules();
    refreshRateLimitTable();
    appendRateLimitActionLogLine(QStringLiteral("已清空全部限速规则。"));
}

void NetworkDock::refreshRateLimitTable()
{
    if (m_rateLimitTable == nullptr || m_trafficService == nullptr)
    {
        return;
    }

    const std::vector<ks::network::ProcessRateLimitSnapshot> snapshots =
        m_trafficService->SnapshotRateLimitRules();

    m_rateLimitTable->setRowCount(0);
    int rowIndex = 0;
    for (const ks::network::ProcessRateLimitSnapshot& snapshot : snapshots)
    {
        m_rateLimitTable->insertRow(rowIndex);

        const std::uint32_t processId = snapshot.rule.processId;
        const std::string processName = ks::process::GetProcessNameByPID(processId);
        const QString stateText = snapshot.currentlySuspended ? QStringLiteral("已挂起") : QStringLiteral("运行中");

        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::Pid),
            createPacketCell(QString::number(processId)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::ProcessName),
            createPacketCell(toQString(processName)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::LimitKBps),
            createPacketCell(QString::number(snapshot.rule.bytesPerSecond / 1024ULL)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::SuspendMs),
            createPacketCell(QString::number(snapshot.rule.suspendDurationMs)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::TriggerCount),
            createPacketCell(QString::number(snapshot.triggerCount)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::CurrentWindowBytes),
            createPacketCell(QString::number(snapshot.currentWindowBytes)));
        m_rateLimitTable->setItem(rowIndex, toRateLimitColumn(RateLimitTableColumn::State),
            createPacketCell(stateText));
        ++rowIndex;
    }
}

void NetworkDock::appendRateLimitActionLogLine(const QString& logLine)
{
    if (m_rateLimitLogOutput == nullptr)
    {
        return;
    }

    const QString timePrefix = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_rateLimitLogOutput->appendPlainText(QStringLiteral("[%1] %2").arg(timePrefix, logLine));
}

void NetworkDock::openPacketDetailWindowFromTableRow(QTableWidget* tableWidget, const int row)
{
    if (tableWidget == nullptr || row < 0)
    {
        return;
    }

    QTableWidgetItem* firstItem = tableWidget->item(row, toPacketColumn(PacketTableColumn::Time));
    if (firstItem == nullptr)
    {
        return;
    }

    const QVariant sequenceVariant = firstItem->data(Qt::UserRole);
    if (!sequenceVariant.isValid())
    {
        return;
    }

    const std::uint64_t sequenceId = static_cast<std::uint64_t>(sequenceVariant.toULongLong());
    openPacketDetailWindowBySequenceId(sequenceId);
}

void NetworkDock::openPacketDetailWindowBySequenceId(const std::uint64_t sequenceId)
{
    const auto iterator = m_packetBySequence.find(sequenceId);
    if (iterator == m_packetBySequence.end())
    {
        QMessageBox::information(this, QStringLiteral("报文详情"), QStringLiteral("该报文已被清理，无法查看详情。"));
        return;
    }

    // 详情窗口使用 show() 非模态弹出，不阻塞主 UI。
    PacketDetailWindow* detailWindow = new PacketDetailWindow(iterator->second, this);
    detailWindow->show();
    detailWindow->raise();
    detailWindow->activateWindow();
}

QIcon NetworkDock::resolveProcessIconByPid(const std::uint32_t processId, const std::string& processName)
{
    if (processId == 0)
    {
        return QIcon(":/Icon/process_main.svg");
    }

    const quint32 pidKey = static_cast<quint32>(processId);
    const auto cacheIterator = m_processIconCacheByPid.constFind(pidKey);
    if (cacheIterator != m_processIconCacheByPid.constEnd())
    {
        return cacheIterator.value();
    }

    QIcon processIcon;

    // 先尝试按可执行路径提取系统文件图标。
    const std::string processPath = ks::process::QueryProcessPathByPid(processId);
    if (!processPath.empty())
    {
        static QFileIconProvider fileIconProvider;
        const QString processPathText = QString::fromUtf8(processPath.c_str());
        processIcon = fileIconProvider.icon(QFileInfo(processPathText));
    }

    // 如果路径不可用，则回退到统一图标，保证列内始终有可视图标。
    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }

    m_processIconCacheByPid.insert(pidKey, processIcon);

    // 对于名称为空的异常条目，也在日志记录一次，方便定位来源。
    if (processName.empty())
    {
        kLogEvent iconEvent;
        warn << iconEvent << "[NetworkDock] 进程名为空，使用默认图标, pid=" << processId << eol;
    }
    return processIcon;
}

int NetworkDock::toPacketColumn(const PacketTableColumn column)
{
    return static_cast<int>(column);
}

int NetworkDock::toRateLimitColumn(const RateLimitTableColumn column)
{
    return static_cast<int>(column);
}

bool NetworkDock::tryParsePidText(const QString& pidText, std::uint32_t& pidOut)
{
    bool parseOk = false;
    const unsigned long pidValue = pidText.trimmed().toULong(&parseOk, 10);
    if (!parseOk || pidValue == 0 || pidValue > 0xFFFFFFFFUL)
    {
        return false;
    }

    pidOut = static_cast<std::uint32_t>(pidValue);
    return true;
}
