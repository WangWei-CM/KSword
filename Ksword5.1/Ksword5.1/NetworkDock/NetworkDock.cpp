#include "NetworkDock.InternalCommon.h"
#include "HttpsProxyService.h"

// ============================================================
// NetworkDock.cpp
// 作用：
// 1) 保留 NetworkDock 构造/析构和后台回调接线；
// 2) 具体 UI 与业务函数分拆在各独立 .cpp；
// 3) 在不改变功能前提下替换旧的 .inc 聚合结构。
// ============================================================

NetworkDock::NetworkDock(QWidget* parent)
    : QWidget(parent)
{
    // 创建后台服务对象：负责抓包、PID 映射、限速逻辑。
    m_trafficService = std::make_unique<ks::network::TrafficMonitorService>();

    // 初始化界面和连接逻辑。
    initializeUi();
    initializeConnections();
    loadMonitorFilterConfigFromDefaultPath();

    // “进程限速”页当前不暴露给用户，因此不创建该页专用的轮询刷新定时器。
    // 处理逻辑：
    // - 输入为空：无用户可见的限速页，不需要定时拉取规则快照；
    // - 处理：保持 m_rateLimitRefreshTimer 为 nullptr；
    // - 返回：构造函数继续初始化其它网络功能页。

    // 报文批量刷新定时器：
    // - 由 UI 线程周期性批量消费后台队列；
    // - 避免“每包一个 invokeMethod”把事件循环塞爆。
    m_packetFlushTimer = new QTimer(this);
    m_packetFlushTimer->setInterval(20);
    connect(m_packetFlushTimer, &QTimer::timeout, this, [this]()
        {
            flushPendingPacketsToUi();
        });
    m_packetFlushTimer->start();

    // 连接管理刷新定时器：
    // - 用于周期更新 TCP/UDP 表；
    // - 自动刷新关闭或页面不可见时，定时器回调会直接跳过；
    // - 目的是避免隐藏页持续枚举连接造成 UI 卡顿。
    m_connectionRefreshTimer = new QTimer(this);
    m_connectionRefreshTimer->setInterval(2200);
    connect(m_connectionRefreshTimer, &QTimer::timeout, this, [this]()
        {
            if (m_sideTabWidget == nullptr || m_sideTabWidget->currentWidget() != m_connectionManagePage)
            {
                return;
            }
            if (m_autoRefreshConnectionButton != nullptr && !m_autoRefreshConnectionButton->isChecked())
            {
                return;
            }
            refreshConnectionTables();
        });
    m_connectionRefreshTimer->start();

    // 多线程下载刷新定时器：
    // - 周期刷新任务表/分段表/总进度条；
    // - 即使当前不在下载页，只要存在运行中任务也维持刷新。
    m_multiDownloadRefreshTimer = new QTimer(this);
    m_multiDownloadRefreshTimer->setInterval(180);
    connect(m_multiDownloadRefreshTimer, &QTimer::timeout, this, [this]()
        {
            refreshMultiThreadDownloadUi();
        });
    m_multiDownloadRefreshTimer->start();

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

    // 首次加载不再强制立刻枚举连接：
    // - 如果当前就处于连接管理页，则仍执行一次首刷；
    // - 否则延迟到用户切入该页后由自动刷新触发，减少主线程启动负担。
    if (m_sideTabWidget != nullptr && m_sideTabWidget->currentWidget() == m_connectionManagePage)
    {
        refreshConnectionTables();
    }
    else if (m_connectionStatusLabel != nullptr)
    {
        m_connectionStatusLabel->setText(QStringLiteral("状态：进入此页面后自动刷新"));
    }
}

NetworkDock::~NetworkDock()
{
    // 析构前请求取消全部下载任务，避免窗口释放后继续长期下载。
    {
        std::lock_guard<std::mutex> guard(m_multiDownloadTaskMutex);
        for (const std::shared_ptr<MultiThreadDownloadTaskState>& taskState : m_multiDownloadTaskList)
        {
            if (taskState != nullptr)
            {
                taskState->canceled.store(true);
                taskState->cancelRequested.store(true);
                taskState->pauseRequested.store(false);
            }
        }
    }

    // 若有异步停止线程在执行，析构时同步等待一次，确保服务对象仍然有效。
    if (m_monitorStopThread != nullptr && m_monitorStopThread->joinable())
    {
        m_monitorStopThread->join();
    }
    m_monitorStopThread.reset();

    // 窗口销毁前主动停止后台线程，避免析构后回调悬空。
    if (m_trafficService != nullptr)
    {
        m_trafficService->StopCapture();
    }
    if (m_httpsProxyService != nullptr)
    {
        m_httpsProxyService->stop();
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[NetworkDock] 网络面板已析构，抓包线程已停止。" << eol;
}

void NetworkDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (m_httpsProxyServiceInitialized)
    {
        return;
    }

    m_httpsProxyServiceInitialized = true;
    m_httpsProxyService = std::make_unique<ks::network::HttpsMitmProxyService>();
    if (m_httpsProxyService != nullptr)
    {
        m_httpsProxyService->setParsedCallback([this](const ks::network::HttpsProxyParsedEntry& parsedEntry)
            {
                QMetaObject::invokeMethod(this, [this, parsedEntry]()
                    {
                        onHttpsProxyParsedEntryArrived(parsedEntry);
                    }, Qt::QueuedConnection);
            });
        m_httpsProxyService->setStatusCallback([this](const QString& statusText)
            {
                QMetaObject::invokeMethod(this, [this, statusText]()
                    {
                        appendHttpsProxyLogLine(statusText);
                        if (!statusText.isEmpty())
                        {
                            updateHttpsProxyStatusLabel(statusText);
                        }
                    }, Qt::QueuedConnection);
            });
    }
}
