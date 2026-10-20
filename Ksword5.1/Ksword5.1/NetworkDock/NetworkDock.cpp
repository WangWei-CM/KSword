#include "NetworkDock.NavigationData.h"
#include "../UI/ThemeBinding.h"
#include "../UI/AsyncUiDispatcher.h"
#include "NetworkDock.InternalCommon.h"
#include "HttpsProxyService.h"
#include "../ArkDriverClient/ArkDriverClient.h"

#include <QMessageBox>
#include "../UI/AsyncOperation.h"

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
    // 在任何网络页初始化和新代理应用之前，先恢复上次崩溃/强杀遗留的代理事务。
    bool recoveredPreviousProxy = false;
    QString recoveryErrorText;
    const bool recoveryOk = recoverPendingHttpsSystemProxyTransaction(
        &recoveredPreviousProxy,
        &recoveryErrorText);

    // 创建后台服务对象：负责抓包、PID 映射、限速逻辑。
    m_trafficService = std::make_unique<ks::network::TrafficMonitorService>();

    // 初始化界面和连接逻辑。
    initializeUi();
    initializeConnections();
    loadMonitorFilterConfigFromDefaultPath();

    if (recoveredPreviousProxy)
    {
        appendHttpsProxyLogLine(QStringLiteral(
            "检测到上次异常退出遗留的 HTTPS 系统代理，已自动恢复原配置。"));
        kLogEvent recoveryEvent;
        info << recoveryEvent
            << "[NetworkDock] 已从持久化事务恢复上次 HTTPS 系统代理配置。"
            << eol;
    }
    else if (!recoveryOk)
    {
        appendHttpsProxyLogLine(
            QStringLiteral("自动恢复上次 HTTPS 系统代理失败：%1")
                .arg(recoveryErrorText));
        kLogEvent recoveryEvent;
        warn << recoveryEvent
            << "[NetworkDock] 启动时恢复 HTTPS 系统代理失败："
            << recoveryErrorText
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("HTTPS 系统代理恢复失败"),
            QStringLiteral(
                "检测到未完成的 HTTPS 代理恢复事务，但自动恢复失败：\n%1\n\n"
                "在恢复成功前将禁止再次应用 HTTPS 系统代理，以免覆盖原始配置。")
                .arg(recoveryErrorText));
    }

    // “进程限速”页当前不暴露给用户，因此不创建该页专用的轮询刷新定时器。
    // 处理逻辑：
    // - 输入为空：无用户可见的限速页，不需要定时拉取规则快照；
    // - 处理：保持 m_rateLimitRefreshTimer 为 nullptr；
    // - 返回：构造函数继续初始化其它网络功能页。

    // 报文批量刷新定时器：
    // - 由 UI 线程周期性批量消费后台队列；
    // - 避免“每包一个 invokeMethod”把事件循环塞爆。
    m_packetFlushTimer = new QTimer(this);
    m_packetFlushTimer->setInterval(50);
    connect(m_packetFlushTimer, &QTimer::timeout, this, [this]()
        {
            flushPendingPacketsToUi();
        });
    m_packetFlushTimer->start();

    // R0 流量模式轮询：
    // - 仅在 R0 模式运行时按 sequence cursor 查询新增 WFP IPv4/IPv6 逐包记录；
    // - 查询失败会在 refreshR0TrafficSnapshotAsync 中无缝回退 R3；
    // - 独立“连接管理”页已移除，不再创建其旧轮询器。
    m_r0TrafficRefreshTimer = new QTimer(this);
    m_r0TrafficRefreshTimer->setInterval(250);
    connect(m_r0TrafficRefreshTimer, &QTimer::timeout, this, [this]()
        {
            if (!m_monitorRunning || m_monitorSource != TrafficMonitorSource::R0)
            {
                return;
            }
            refreshR0TrafficSnapshotAsync(m_monitorGeneration.load(), false);
        });

    // 多线程下载刷新定时器：
    // - 周期刷新任务表/分段表/总进度条；
    // - 下载后台独立运行；只有下载页实际可见时才构造任务表和分段表。
    m_multiDownloadRefreshTimer = new QTimer(this);
    m_multiDownloadRefreshTimer->setInterval(180);
    connect(m_multiDownloadRefreshTimer, &QTimer::timeout, this, [this]()
        {
            if (!isVisible() || m_sideTabWidget == nullptr
                || m_sideTabWidget->currentWidget() != m_multiThreadDownloadPage)
            {
                return;
            }
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

}

NetworkDock::~NetworkDock()
{
    if (m_nidsAnalysisOperation != nullptr) m_nidsAnalysisOperation->close();
    retireNidsAnalysisSession();
    // Detach the GUI consumer before any service teardown can deliver callbacks.
    if (m_httpsProxyService != nullptr)
    {
        m_httpsProxyService->setParsedCallback({});
    }
    if (m_multiDownloadDispatcher != nullptr)
    {
        m_multiDownloadDispatcher->close();
    }
    // Close delivery before member teardown; read workers only retain value state.
    if (m_routeReadOperation != nullptr) m_routeReadOperation->close();
    if (m_arpReadOperation != nullptr) m_arpReadOperation->close();
    if (m_dnsReadOperation != nullptr) m_dnsReadOperation->close();
    m_monitorGeneration.fetch_add(1);
    m_monitorSource = TrafficMonitorSource::Stopped;
    // 析构必须先停用 R0 逐包数据面；仅停止轮询会让内核继续复制所有流量。
    {
        const ksword::ark::DriverClient driverClient;
        (void)driverClient.controlNetworkTrafficCapture(false);
    }
    if (m_r0TrafficRefreshTimer != nullptr)
    {
        m_r0TrafficRefreshTimer->stop();
    }

    // 先收尾 ICMP 扫描，避免析构后的后台 worker 继续访问 NetworkDock 成员。
    cancelAndWaitForAliveHostScan();

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
    if (m_httpsSystemProxySnapshotCaptured)
    {
        QString restoreErrorText;
        if (!restoreHttpsSystemProxySnapshot(&restoreErrorText))
        {
            kLogEvent restoreEvent;
            warn << restoreEvent << "[NetworkDock] HTTPS 系统代理恢复失败：" << restoreErrorText << eol;
        }
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[NetworkDock] 网络面板已析构，抓包线程已停止。" << eol;
}

void NetworkDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    initializeVisibleDiagnosticPage();
    synchronizeVisiblePacketProjection();
    synchronizeVisibleNidsProjection();

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
                // The service dispatcher already delivers on this GUI thread.
                // A second queued hop copies the event and adds avoidable backlog.
                onHttpsProxyParsedEntryArrived(parsedEntry);
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

void NetworkDock::registerNavigationTab(const QString& id, QTabWidget* tabs, QWidget* page)
{
    if (network_navigation_detail::Find(id) != nullptr && tabs != nullptr && page != nullptr)
        m_navigationTargets.insert(id, {tabs, page});
}

ks::ui::PageNavigationCatalog NetworkDock::navigationCatalog(const NetworkDock* existing)
{
    ks::ui::PageNavigationCatalog catalog;
    catalog.reserve(static_cast<int>(std::size(network_navigation_detail::Definitions)));
    for (const auto& definition : network_navigation_detail::Definitions)
    {
        ks::ui::PageNavigationDescriptor page;
        page.id = QString::fromLatin1(definition.id);
        page.parentId = QString::fromLatin1(definition.parent);
        page.title = network_navigation_detail::Title(definition);
        page.searchTexts = {QString::fromWCharArray(definition.sourceTitle), page.id};
        page.visible = definition.exposed;
        if (existing != nullptr)
        {
            const auto target = existing->m_navigationTargets.constFind(page.id);
            if (target == existing->m_navigationTargets.cend() || !target->tabs || !target->page)
            {
                page.visible = false;
                page.enabled = false;
            }
            else
            {
                const int index = target->tabs->indexOf(target->page);
                page.visible = index >= 0 && target->tabs->isTabVisible(index);
                page.enabled = index >= 0 && target->tabs->isTabEnabled(index);
                page.current = index >= 0 && target->tabs->currentIndex() == index;
                if (index >= 0) page.title = target->tabs->tabText(index);
            }
        }
        catalog.push_back(std::move(page));
    }
    return catalog;
}

QWidget* NetworkDock::activateNavigationDescriptor(const QString& id)
{
    const QPointer<NetworkDock> guard(this);
    const auto* definition = network_navigation_detail::Find(id);
    if (definition == nullptr) return nullptr;
    QVector<NavigationTarget> path;
    while (definition != nullptr)
    {
        const auto target = m_navigationTargets.constFind(QString::fromLatin1(definition->id));
        if (target == m_navigationTargets.cend() || !target->tabs || !target->page) return nullptr;
        const int index = target->tabs->indexOf(target->page);
        if (index < 0 || !target->tabs->isTabVisible(index) || !target->tabs->isTabEnabled(index)) return nullptr;
        path.prepend(*target);
        if (*definition->parent == '\0') definition = nullptr;
        else
        {
            definition = network_navigation_detail::Find(QString::fromLatin1(definition->parent));
            if (definition == nullptr) return nullptr;
        }
    }
    QPointer<QWidget> targetPage;
    for (const auto& target : path)
    {
        if (!guard || !target.tabs || !target.page) return nullptr;
        const int index = target.tabs->indexOf(target.page);
        if (index < 0 || !target.tabs->isTabVisible(index) || !target.tabs->isTabEnabled(index)) return nullptr;
        target.tabs->setCurrentWidget(target.page);
        if (!guard || !target.tabs || !target.page) return nullptr;
        ks::ui::PrepareWidgetThemeBindingsForShow(target.page);
        if (!guard || !target.tabs || !target.page || target.tabs->currentWidget() != target.page) return nullptr;
        targetPage = target.page;
    }
    for (const auto& target : path)
    {
        if (!guard || !target.tabs || !target.page) return nullptr;
        const int index = target.tabs->indexOf(target.page);
        if (index < 0 || !target.tabs->isTabVisible(index) || !target.tabs->isTabEnabled(index)
            || target.tabs->currentWidget() != target.page) return nullptr;
    }
    return guard ? targetPage.data() : nullptr;
}

namespace
{
    const bool kRegisterNetworkNavigationCatalog =
        ks::ui::RegisterTypedPageNavigationCatalog<NetworkDock>(QStringLiteral("network"));
}
