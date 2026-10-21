#include "NetworkDock.InternalCommon.h"

#include <QAbstractItemModel>
#include <QItemSelection>
#include "../UI/AsyncOperation.h"
#include "../UI/CompleteExport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSet>

#include <cstddef>
#include <unordered_map>
#include <utility>

#include <objbase.h>
#include <Shellapi.h>

#pragma comment(lib, "Dnsapi.lib")

using namespace network_dock_detail;

namespace
{
    // g_pendingPacketProcessIconPidMap 作用：
    // - 记录“每个 NetworkDock 实例正在后台解析图标的 PID 集合”，用于同一 PID 去重；
    // - 只在 UI 线程读写（调度点与回投点都在 UI 线程），因此不需要加锁；
    // - 键仅作为身份标识使用，永远不会被解引用，宿主析构后由回投分支按键清理。
    std::unordered_map<const NetworkDock*, QSet<quint32>> g_pendingPacketProcessIconPidMap;

    // packetProcessPlaceholderIcon 作用：
    // - 返回报文表进程列的统一占位图标，异步解析完成前先顶上，避免空图标导致行高抖动；
    // - 入参：无；
    // - 返回：共享 QIcon 引用，只能在 UI 线程使用。
    const QIcon& packetProcessPlaceholderIcon()
    {
        static const QIcon placeholderIcon(QStringLiteral(":/Icon/process_main.svg"));
        return placeholderIcon;
    }

    // extractProcessIconImageForPid 作用：
    // - 在线程池工作线程里按 PID 解析可执行路径并向 Shell 查询小图标；
    // - SHGetFileInfoW 需要本线程自己初始化 COM，因此这里成对 CoInitializeEx/CoUninitialize；
    // - 入参 processId：目标进程 PID；
    // - 返回：可跨线程传递的 QImage，失败时返回空 QImage（调用方回退占位图标）。
    QImage extractProcessIconImageForPid(const std::uint32_t processId)
    {
        const std::string processPath = ks::process::QueryProcessPathByPid(processId);
        if (processPath.empty())
        {
            return QImage();
        }

        const HRESULT comInitializeResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comInitializedHere = SUCCEEDED(comInitializeResult);

        const QString processPathText = QString::fromUtf8(processPath.c_str());
        SHFILEINFOW shellFileInfo{};
        const DWORD_PTR shellQueryResult = ::SHGetFileInfoW(
            reinterpret_cast<const wchar_t*>(processPathText.utf16()),
            0,
            &shellFileInfo,
            sizeof(shellFileInfo),
            SHGFI_ICON | SHGFI_SMALLICON);

        QImage processIconImage;
        if (shellQueryResult != 0 && shellFileInfo.hIcon != nullptr)
        {
            // QImage::fromHICON 会复制像素数据，转换完成后必须归还 Shell 分配的 HICON。
            processIconImage = QImage::fromHICON(shellFileInfo.hIcon);
            ::DestroyIcon(shellFileInfo.hIcon);
        }

        if (comInitializedHere)
        {
            ::CoUninitialize();
        }
        return processIconImage;
    }

    // applyResolvedProcessIconToPacketRows 作用：
    // - 图标异步解析完成后，把已经落表的同 PID 行的进程列图标补齐；
    // - 入参 packetTable：报文主表；
    // - 入参 processIdColumn / processNameColumn：PID 列与进程名列索引（由调用方从列枚举换算）；
    // - 入参 processIdKey：本次解析完成的 PID；
    // - 入参 resolvedIcon：解析结果或占位图标；
    // - 返回：无。该函数只能在 UI 线程调用。
    void applyResolvedProcessIconToPacketRows(
        QTableWidget* const packetTable,
        const int processIdColumn,
        const int processNameColumn,
        const quint32 processIdKey,
        const QIcon& resolvedIcon)
    {
        if (packetTable == nullptr || processIdColumn < 0 || processNameColumn < 0)
        {
            return;
        }

        const QString processIdText = QString::number(processIdKey);
        for (int rowIndex = 0; rowIndex < packetTable->rowCount(); ++rowIndex)
        {
            const QTableWidgetItem* const processIdItem = packetTable->item(rowIndex, processIdColumn);
            if (processIdItem == nullptr || processIdItem->text() != processIdText)
            {
                continue;
            }

            QTableWidgetItem* const processNameItem = packetTable->item(rowIndex, processNameColumn);
            if (processNameItem != nullptr)
            {
                processNameItem->setIcon(resolvedIcon);
            }
        }
    }

    // kUnixMsTo100ns：
    // - Unix 毫秒时间戳转换到 100ns 时间轴单位的倍率；
    // - 网络抓包使用 Unix ms，ETW 同款时间轴使用 100ns，所以需要统一换算。
    constexpr std::uint64_t kUnixMsTo100ns = 10000ULL;

    // kFallbackTimelineRange100ns：
    // - 没有报文或起止相同的时候给时间轴提供 1 秒非零跨度；
    // - 避免时间轴控件内部坐标除零，也让初始空状态仍可见。
    constexpr std::uint64_t kFallbackTimelineRange100ns = 1ULL * 1000ULL * 1000ULL * 10ULL;

    // kTimelineSecond100ns：
    // - 1 秒在 100ns 时间轴单位下的宽度；
    // - 用于把报文聚合到“每秒上传/下载速率”桶。
    constexpr std::uint64_t kTimelineSecond100ns = 1ULL * 1000ULL * 1000ULL * 10ULL;

    // unixMsToTimeline100ns：
    // - 作用：把 Unix ms 时长或时间差转换成时间轴使用的 100ns；
    // - 参数 unixMsValue：毫秒数；
    // - 返回：100ns 时间值。
    std::uint64_t unixMsToTimeline100ns(const std::uint64_t unixMsValue)
    {
        return unixMsValue * kUnixMsTo100ns;
    }

    // packetTimelineTypeText：
    // - 作用：把 TCP/UDP 与出入站方向归一化成时间轴颜色/泳道可理解的类型；
    // - 参数 packetRecord：待映射报文；
    // - 返回：时间轴点 typeText，当前统一归入“网络”泳道。
    QString packetTimelineTypeText(const ks::network::PacketRecord& packetRecord)
    {
        (void)packetRecord;
        return QStringLiteral("网络");
    }

    // normalizeIpAddressText 作用：
    // - 输入任意合法 IPv4/IPv6 文本；
    // - 通过 InetPton/InetNtop 归一化，作为 DNS 地址缓存键；
    // - 无效文本返回空字符串。
    QString normalizeIpAddressText(const QString& addressText)
    {
        IN_ADDR ipv4Address{};
        if (InetPtonW(AF_INET, reinterpret_cast<PCWSTR>(addressText.utf16()), &ipv4Address) == 1)
        {
            wchar_t addressBuffer[INET_ADDRSTRLEN] = {};
            return InetNtopW(AF_INET, &ipv4Address, addressBuffer, static_cast<DWORD>(std::size(addressBuffer))) != nullptr
                ? QString::fromWCharArray(addressBuffer)
                : QString();
        }

        IN6_ADDR ipv6Address{};
        if (InetPtonW(AF_INET6, reinterpret_cast<PCWSTR>(addressText.utf16()), &ipv6Address) == 1)
        {
            wchar_t addressBuffer[INET6_ADDRSTRLEN] = {};
            return InetNtopW(AF_INET6, &ipv6Address, addressBuffer, static_cast<DWORD>(std::size(addressBuffer))) != nullptr
                ? QString::fromWCharArray(addressBuffer).toLower()
                : QString();
        }
        return QString();
    }

    // rebuildLocalDnsAddressMap 作用：
    // - 先枚举 DnsGetCacheDataTable 中的名称，再用 DNS_QUERY_CACHE_ONLY 读取 A/AAAA；
    // - 返回 IP -> 域名映射，全程不会触发线上 DNS 查询；
    // - 该函数只在后台线程调用。
    QHash<QString, QString> rebuildLocalDnsAddressMap()
    {
        using DnsGetCacheDataTableFn = BOOL(WINAPI*)(PVOID);
        struct DnsCacheEntryRecord
        {
            DnsCacheEntryRecord* next = nullptr;
            PWSTR name = nullptr;
            WORD type = 0;
            WORD dataLength = 0;
            DWORD flags = 0;
        };

        QHash<QString, QString> addressNameMap;
        HMODULE dnsapiModule = GetModuleHandleW(L"dnsapi.dll");
        if (dnsapiModule == nullptr)
        {
            dnsapiModule = LoadLibraryW(L"dnsapi.dll");
        }
        if (dnsapiModule == nullptr)
        {
            return addressNameMap;
        }

        const auto dnsGetCacheDataTable = reinterpret_cast<DnsGetCacheDataTableFn>(
            GetProcAddress(dnsapiModule, "DnsGetCacheDataTable"));
        if (dnsGetCacheDataTable == nullptr)
        {
            return addressNameMap;
        }

        DnsCacheEntryRecord rootEntry{};
        if (dnsGetCacheDataTable(&rootEntry) == FALSE)
        {
            return addressNameMap;
        }

        int visitedNameCount = 0;
        for (DnsCacheEntryRecord* node = rootEntry.next;
            node != nullptr && visitedNameCount < 2048;
            node = node->next, ++visitedNameCount)
        {
            if (node->name == nullptr || (node->type != DNS_TYPE_A && node->type != DNS_TYPE_AAAA))
            {
                continue;
            }

            PDNS_RECORDW recordList = nullptr;
            const DNS_STATUS queryStatus = DnsQuery_W(
                node->name,
                node->type,
                DNS_QUERY_CACHE_ONLY,
                nullptr,
                &recordList,
                nullptr);
            if (queryStatus != ERROR_SUCCESS || recordList == nullptr)
            {
                continue;
            }

            const QString domainText = QString::fromWCharArray(node->name);
            for (PDNS_RECORDW record = recordList; record != nullptr; record = record->pNext)
            {
                wchar_t addressBuffer[INET6_ADDRSTRLEN] = {};
                PCWSTR convertedAddress = nullptr;
                if (record->wType == DNS_TYPE_A)
                {
                    convertedAddress = InetNtopW(
                        AF_INET,
                        &record->Data.A.IpAddress,
                        addressBuffer,
                        static_cast<DWORD>(std::size(addressBuffer)));
                }
                else if (record->wType == DNS_TYPE_AAAA)
                {
                    convertedAddress = InetNtopW(
                        AF_INET6,
                        record->Data.AAAA.Ip6Address.IP6Byte,
                        addressBuffer,
                        static_cast<DWORD>(std::size(addressBuffer)));
                }
                if (convertedAddress == nullptr)
                {
                    continue;
                }

                const QString addressKey = normalizeIpAddressText(QString::fromWCharArray(addressBuffer));
                if (!addressKey.isEmpty() && !addressNameMap.contains(addressKey))
                {
                    addressNameMap.insert(addressKey, domainText);
                }
            }
            DnsRecordListFree(recordList, DnsFreeRecordList);
        }
        return addressNameMap;
    }

    // resolveDomainNameFromLocalCache 作用：
    // - 只查询本机 DNS Client 缓存映射，不发送线上 PTR/DNS 请求；
    // - 缓存最多每 30 秒后台重建一次，避免抓包 UI 线程等待；
    // - 返回域名，缓存未命中时返回空字符串。
    QString resolveDomainNameFromLocalCache(const QString& addressText)
    {
        const QString addressKey = normalizeIpAddressText(addressText);
        if (addressKey.isEmpty()
            || addressKey == QStringLiteral("0.0.0.0")
            || addressKey == QStringLiteral("::"))
        {
            return QString();
        }

        static std::mutex dnsCacheMutex;
        static QHash<QString, QString> localDnsAddressMap;
        static ULONGLONG lastRefreshTick = 0;
        {
            std::lock_guard<std::mutex> guard(dnsCacheMutex);
            const ULONGLONG nowTick = GetTickCount64();
            if (lastRefreshTick == 0ULL || (nowTick - lastRefreshTick) >= 30000ULL)
            {
                localDnsAddressMap = rebuildLocalDnsAddressMap();
                lastRefreshTick = nowTick;
            }
            const auto cachedIterator = localDnsAddressMap.constFind(addressKey);
            if (cachedIterator != localDnsAddressMap.constEnd())
            {
                return cachedIterator.value();
            }
        }
        return QString();
    }
}

bool NetworkDock::isPacketProjectionVisible() const
{
    return isVisible() && m_trafficMonitorPage != nullptr && m_trafficMonitorPage->isVisible();
}

void NetworkDock::synchronizeVisiblePacketProjection()
{
    if (!isPacketProjectionVisible())
    {
        return;
    }
    // Canonical packets, rate buckets and NIDS continue to advance while hidden.
    // Apply the current range before rebuilding a timeline-filtered packet view.
    if (m_packetTimelineProjectionDirty)
    {
        refreshPacketTimelineRange();
        refreshPacketTimelinePoints();
        refreshPacketTimelineRatePoints();
    }
    if (m_packetTableProjectionDirty)
    {
        rebuildMonitorTableByFilter();
    }
}

void NetworkDock::onPacketCaptured(const ks::network::PacketRecord& packetRecord)
{
    // timelineTime100ns 是“只统计监控开启时间”的压缩时间：
    // - 停止监控后的等待时长不再占用横轴；
    // - 后续表格筛选、事件点、速率折线全部使用同一压缩时间。
    const std::uint64_t timelineTime100ns = packetTimelineTimeForRecord(packetRecord);

    // 缓存报文实体：用于详情窗口通过序号回查完整字节内容。
    m_packetSequenceOrder.push_back(packetRecord.sequenceId);
    m_packetBySequence[packetRecord.sequenceId] = packetRecord;
    m_packetTimelineTimeBySequence[packetRecord.sequenceId] = timelineTime100ns;

    // 同步写入时间轴轻量点：
    // - 点列表只保存时间和类型，不保存表格行号；
    // - 裁剪时与报文序号缓存同步删除，避免时间轴显示已经不存在的历史点。
    ProcessTraceTimelineEventPoint timelinePoint;
    timelinePoint.time100ns = timelineTime100ns;
    timelinePoint.typeText = packetTimelineTypeText(packetRecord);
    // 协议与收发方向由后端枚举决定，展示文案只保留兼容用途。
    timelinePoint.categoryId = ks::ui::TimelineCategoryForPacket(packetRecord.protocol, packetRecord.direction);
    m_packetTimelineEventPoints.push_back(std::move(timelinePoint));
    addPacketTimelineRateSample(packetRecord, timelineTime100ns);
    processNidsPacket(packetRecord);

    if (m_packetTableProjection.active && isPacketProjectionVisible())
        m_packetTableProjection.pending.push_back(packetRecord.sequenceId);

    // 当前报文若通过“组合过滤条件”才追加到主表（过滤关闭时默认全部通过）。
    if (!isPacketProjectionVisible() || m_packetTableProjectionDirty)
    {
        m_packetTableProjectionDirty = true;
    }
    else if (packetPassesMonitorFilter(packetRecord.sequenceId, packetRecord))
    {
        appendPacketToMonitorTable(packetRecord);
    }
    trimOldestPacketWhenNeeded();
}

void NetworkDock::flushPendingPacketsToUi()
{
    // 在读取并消费后台队列前设置提交屏障。菜单打开时队列继续积累，
    // 关闭后只触发一次最新 flush，既不丢包也不改变菜单对应的行。
    const QPointer<NetworkDock> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("network-packet-stream-flush"),
        {m_packetTable},
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->flushPendingPacketsToUi();
            }
        }))
    {
        return;
    }

    if (isPacketProjectionVisible() && !m_packetTableProjectionDirty
        && ks::ui::DeferTablePresentationIfValueCaptureOpen(this,
            QStringLiteral("network-packet-filter-rebuild"), {m_packetTable},
            [safeThis]() { if (safeThis) safeThis->rebuildMonitorTableByFilter(); }))
    {
        m_packetTableProjectionDirty = true;
    }

    // 先在锁内把待处理报文批量转移到局部容器，缩短锁持有时间。
    std::vector<ks::network::PacketRecord> packetBatch;
    std::uint64_t droppedCountSnapshot = 0;
    // timelineIdleRefreshNeeded 用途：即使后台队列为空，只要监控还在运行，也周期推进压缩时间轴。
    const bool timelineIdleRefreshNeeded = m_packetTimelineSessionActive && m_monitorRunning;
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        if (m_pendingPacketQueue.empty() && m_droppedPacketCount == 0 && !timelineIdleRefreshNeeded)
        {
            return;
        }

        // 单次只取一小批，后续仍以时间预算兜住单帧耗时。
        constexpr std::size_t kMaxConsumePerTick = 160;
        const std::size_t consumeCount = std::min<std::size_t>(kMaxConsumePerTick, m_pendingPacketQueue.size());
        packetBatch.reserve(consumeCount);
        for (std::size_t index = 0; index < consumeCount; ++index)
        {
            packetBatch.push_back(std::move(m_pendingPacketQueue.front()));
            m_pendingPacketQueue.pop_front();
        }

        droppedCountSnapshot = m_droppedPacketCount;
    }

    // 空流量心跳：
    // - 用户开启监控但这一秒没有任何包时，也要推进时间轴右边界；
    // - 同时补一个 0 B/s 速率桶，让折线在空闲秒回落到基线。
    const bool shouldRefreshIdleTimeline = packetBatch.empty()
        && m_packetTimelineSessionActive
        && m_monitorRunning;
    if (shouldRefreshIdleTimeline)
    {
        const std::uint64_t timelineEnd100ns = currentPacketTimelineEnd100ns();
        const std::uint64_t heartbeatSecond = timelineEnd100ns / kTimelineSecond100ns;
        m_packetTimelineRangeStart100ns = 0;
        m_packetTimelineRangeEnd100ns = std::max(timelineEnd100ns, kFallbackTimelineRange100ns);
        if (heartbeatSecond != m_packetTimelineLastHeartbeatSecond)
        {
            m_packetTimelineLastHeartbeatSecond = heartbeatSecond;
            // Idle zeroes are implicit; elapsed idle duration must not allocate
            // one permanent hash node per second. Projection supplies zero edges.
            refreshPacketTimelineRange();
            refreshPacketTimelineRatePoints();
        }
        else if (m_packetTimelineWidget != nullptr
            && m_packetTimelineRangeEnd100ns > m_packetTimelineWidget->selectionEnd100ns())
        {
            refreshPacketTimelineRange();
        }
    }

    // 批量更新表格期间临时关闭重绘，减少 UI 抖动与卡顿。
    const bool tableUpdateDisabled = !packetBatch.empty() && m_packetTable != nullptr
        && isPacketProjectionVisible() && !m_packetTableProjectionDirty;
    const bool tableUpdatesEnabled = tableUpdateDisabled && m_packetTable->updatesEnabled();
    if (tableUpdateDisabled)
    {
        m_packetTable->setUpdatesEnabled(false);
    }
    QElapsedTimer budgetTimer;
    budgetTimer.start();
    std::size_t processedCount = 0;
    for (const ks::network::PacketRecord& packetRecord : packetBatch)
    {
        onPacketCaptured(packetRecord);
        ++processedCount;
        if (budgetTimer.elapsed() >= 4)
        {
            break;
        }
    }

    if (tableUpdateDisabled)
    {
        m_packetTable->setUpdatesEnabled(tableUpdatesEnabled);
        if (tableUpdatesEnabled && m_packetTable->viewport() != nullptr)
        {
            m_packetTable->viewport()->update();
        }
        if (processedCount > 0)
        {
            m_packetTable->scrollToBottom();
        }
    }

    if (processedCount < packetBatch.size())
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        for (std::size_t index = packetBatch.size(); index > processedCount; --index)
        {
            m_pendingPacketQueue.push_front(std::move(packetBatch[index - 1]));
        }
        while (m_pendingPacketQueue.size() > kMaxPendingPacketQueueCount)
        {
            m_pendingPacketQueue.pop_front();
            ++m_droppedPacketCount;
        }
    }

    // 时间轴刷新放在批量表格刷新之后：
    // - 一次 tick 只重绘一次时间轴，避免高频逐包 update；
    // - 若用户已经框选时间窗口，新包仍会进入点缓存，但表格按外层时间过滤显示。
    if (processedCount > 0)
    {
        refreshPacketTimelineRange();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if ((nowMs - m_lastPacketTimelineRefreshMs) >= 250)
        {
            refreshPacketTimelinePoints();
            refreshPacketTimelineRatePoints();
            m_lastPacketTimelineRefreshMs = nowMs;
        }
    }

    std::size_t pendingCountForInterval = 0;
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        pendingCountForInterval = m_pendingPacketQueue.size();
    }
    if (m_packetFlushTimer != nullptr)
    {
        const int targetIntervalMs = pendingCountForInterval >= 4000
            ? 80
            : (pendingCountForInterval > 0 ? 50 : 120);
        if (m_packetFlushTimer->interval() != targetIntervalMs)
        {
            m_packetFlushTimer->setInterval(targetIntervalMs);
        }
    }

    // 丢包计数只在 UI 线程展示，不在每个回调里刷屏。
    if (droppedCountSnapshot > 0 && m_monitorStatusLabel != nullptr)
    {
        const std::size_t pendingCount = pendingCountForInterval;
        m_monitorStatusLabel->setText(QStringLiteral("状态：高负载（已丢弃%1条，队列%2）")
            .arg(static_cast<qulonglong>(droppedCountSnapshot))
            .arg(static_cast<qulonglong>(pendingCount)));

        // 丢包状态至少记录一次日志，帮助用户排查“为何表格看起来少包”。
        // 这里按计数变化输出，避免每个刷新 tick 都刷屏。
        static std::uint64_t s_lastLoggedDroppedCount = 0;
        if (droppedCountSnapshot != s_lastLoggedDroppedCount)
        {
            s_lastLoggedDroppedCount = droppedCountSnapshot;

            kLogEvent droppedPacketEvent;
            warn << droppedPacketEvent
                << "[NetworkDock] 抓包队列高负载丢包, dropped=" << droppedCountSnapshot
                << ", pending=" << pendingCount
                << eol;
        }
    }
}

void NetworkDock::onStatusMessageArrived(const std::string& statusText)
{
    // R0 模式不接受已停止的 R3 服务回调覆盖来源状态。
    if (m_monitorSource == TrafficMonitorSource::R0
        || m_monitorSource == TrafficMonitorSource::Starting)
    {
        return;
    }

    const QString statusQString = toQString(statusText);
    if (m_monitorStatusLabel != nullptr)
    {
        m_monitorStatusLabel->setText(
            QStringLiteral("状态：来源 R3；%1").arg(statusQString));
    }

    // 状态回调可能表示启动失败/线程退出，因此同步刷新按钮状态。
    const bool wasMonitorRunning = m_monitorRunning;
    if (m_trafficService != nullptr)
    {
        m_monitorRunning = m_trafficService->IsRunning();
    }
    if (wasMonitorRunning && !m_monitorRunning && m_packetTimelineSessionActive)
    {
        // 后台线程若因错误或外部条件自行退出，也要关闭时间轴会话；
        // 否则后续等待时间会被误计入“监控开启时长”。
        endPacketTimelineMonitorSession();
    }
    if (!m_monitorRunning)
    {
        m_monitorSource = TrafficMonitorSource::Stopped;
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

    // 仅在限速页可见时立即刷新表格，避免隐藏页事件触发不必要重绘。
    if (m_sideTabWidget != nullptr && m_sideTabWidget->currentWidget() == m_rateLimitPage)
    {
        refreshRateLimitTable();
    }

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

    // 追加场景改用“直接扩容 rowCount”而非 insertRow：
    // - insertRow 会触发后续行移动；
    // - setRowCount 在尾部追加时更轻量，适合高频写入场景。
    const int newRow = m_packetTable->rowCount();
    m_packetTable->setRowCount(newRow + 1);
    populatePacketMonitorRow(newRow, packetRecord);
}

void NetworkDock::populatePacketMonitorRow(const int newRow, const ks::network::PacketRecord& packetRecord)
{
    if (m_packetTable == nullptr || newRow < 0 || newRow >= m_packetTable->rowCount()) return;
    scheduleDomainResolutionForPacket(
        packetRecord.sequenceId,
        toQString(packetRecord.remoteAddress));
    const auto cachedPacketIterator = m_packetBySequence.find(packetRecord.sequenceId);
    const ks::network::PacketRecord& displayRecord = cachedPacketIterator != m_packetBySequence.end()
        ? cachedPacketIterator->second
        : packetRecord;

    // 进程图标解析必须让出 50ms 批量刷新帧预算：
    // - 命中 PID 缓存时直接复用，稳态零开销；
    // - 未命中时先落占位图标，把 QueryProcessPathByPid + Shell 图标提取丢到线程池，
    //   结果回投后再按 PID 回补已经落表的行，与远端域名异步补齐是同一套模式。
    const quint32 processIdKey = static_cast<quint32>(displayRecord.processId);
    QIcon processIcon = packetProcessPlaceholderIcon();
    const auto processIconCacheIterator = m_processIconCacheByPid.constFind(processIdKey);
    if (processIconCacheIterator != m_processIconCacheByPid.constEnd())
    {
        processIcon = processIconCacheIterator.value();
    }
    else if (processIdKey != 0U)
    {
        QSet<quint32>& pendingIconPidSet = g_pendingPacketProcessIconPidMap[this];
        if (!pendingIconPidSet.contains(processIdKey))
        {
            pendingIconPidSet.insert(processIdKey);

            // ownerKey 只当身份标识用（绝不解引用），保证宿主析构后仍能定位并清理在途集合。
            const QPointer<NetworkDock> guardedSelf(this);
            const NetworkDock* const ownerKey = this;
            const int processIdColumn = toPacketColumn(PacketTableColumn::Pid);
            const int processNameColumn = toPacketColumn(PacketTableColumn::ProcessName);
            QThreadPool::globalInstance()->start(
                [guardedSelf, ownerKey, processIdKey, processIdColumn, processNameColumn]()
                {
                    QImage processIconImage = extractProcessIconImageForPid(processIdKey);
                    QCoreApplication* const appInstance = QCoreApplication::instance();
                    if (appInstance == nullptr)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        appInstance,
                        [guardedSelf,
                            ownerKey,
                            processIdKey,
                            processIdColumn,
                            processNameColumn,
                            processIconImage = std::move(processIconImage)]() mutable
                        {
                            // 无论宿主是否存活都先摘掉在途标记，避免残留条目永久挡住该 PID 的解析。
                            const auto pendingIterator = g_pendingPacketProcessIconPidMap.find(ownerKey);
                            if (pendingIterator != g_pendingPacketProcessIconPidMap.end())
                            {
                                pendingIterator->second.remove(processIdKey);
                                if (pendingIterator->second.isEmpty())
                                {
                                    g_pendingPacketProcessIconPidMap.erase(pendingIterator);
                                }
                            }
                            if (guardedSelf == nullptr)
                            {
                                return;
                            }

                            // QPixmap/QIcon 只能在 UI 线程构造，后台线程只回传 QImage。
                            const QIcon resolvedIcon = processIconImage.isNull()
                                ? packetProcessPlaceholderIcon()
                                : QIcon(QPixmap::fromImage(processIconImage));
                            guardedSelf->m_processIconCacheByPid.insert(processIdKey, resolvedIcon);
                            applyResolvedProcessIconToPacketRows(
                                guardedSelf->m_packetTable,
                                processIdColumn,
                                processNameColumn,
                                processIdKey,
                                resolvedIcon);
                        },
                        Qt::QueuedConnection);
                });
        }
    }

    populatePacketRow(
        m_packetTable,
        newRow,
        displayRecord,
        displayRecord.sequenceId,
        processIcon);
}

void NetworkDock::scheduleDomainResolutionForPacket(
    const std::uint64_t sequenceId,
    const QString& remoteAddressText)
{
    const QString addressKey = normalizeIpAddressText(remoteAddressText);
    if (addressKey.isEmpty()
        || addressKey == QStringLiteral("0.0.0.0")
        || addressKey == QStringLiteral("::"))
    {
        auto packetIterator = m_packetBySequence.find(sequenceId);
        if (packetIterator != m_packetBySequence.end())
        {
            packetIterator->second.remoteDomain = "-";
        }
        return;
    }

    const auto cachedIterator = m_remoteDomainCache.constFind(addressKey);
    if (cachedIterator != m_remoteDomainCache.constEnd())
    {
        auto packetIterator = m_packetBySequence.find(sequenceId);
        if (packetIterator != m_packetBySequence.end())
        {
            packetIterator->second.remoteDomain = cachedIterator.value().toUtf8().constData();
        }
        return;
    }
    if (m_remoteDomainResolutionPending.contains(addressKey))
    {
        return;
    }
    m_remoteDomainResolutionPending.insert(addressKey);

    QPointer<NetworkDock> safeThis(this);
    QThreadPool::globalInstance()->start([safeThis, addressKey]()
    {
        const QString domainText = resolveDomainNameFromLocalCache(addressKey);
        if (safeThis.isNull())
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, addressKey, domainText]()
            {
                if (!safeThis.isNull())
                {
                    safeThis->applyDomainResolutionResult(addressKey, domainText);
                }
            },
            Qt::QueuedConnection);
    });
}

void NetworkDock::applyDomainResolutionResult(
    const QString& remoteAddressText,
    const QString& domainText)
{
    const QString addressKey = normalizeIpAddressText(remoteAddressText);
    if (addressKey.isEmpty())
    {
        return;
    }

    const QString displayText = domainText.trimmed().isEmpty()
        ? QStringLiteral("-")
        : domainText.trimmed();

    const QPointer<NetworkDock> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
            this,
            QStringLiteral("network-domain-resolution:%1").arg(addressKey),
            {m_packetTable},
            [safeThis, addressKey, displayText]()
            {
                if (!safeThis.isNull())
                {
                    safeThis->applyDomainResolutionResult(addressKey, displayText);
                }
            }))
    {
        return;
    }

    m_remoteDomainResolutionPending.remove(addressKey);
    m_remoteDomainCache.insert(addressKey, displayText);
    if (m_remoteDomainCache.size() > 4096)
    {
        m_remoteDomainCache.erase(m_remoteDomainCache.begin());
    }

    for (auto& packetPair : m_packetBySequence)
    {
        if (normalizeIpAddressText(toQString(packetPair.second.remoteAddress)) == addressKey)
        {
            packetPair.second.remoteDomain = displayText.toUtf8().constData();
        }
    }

    if (m_packetTable == nullptr)
    {
        return;
    }
    if (!isPacketProjectionVisible() || m_packetTableProjectionDirty)
    {
        m_packetTableProjectionDirty = true;
        return;
    }
    const int domainColumn = toPacketColumn(PacketTableColumn::RemoteDomain);
    for (int rowIndex = 0; rowIndex < m_packetTable->rowCount(); ++rowIndex)
    {
        QTableWidgetItem* domainItem = m_packetTable->item(rowIndex, domainColumn);
        if (domainItem == nullptr)
        {
            continue;
        }
        if (normalizeIpAddressText(domainItem->data(Qt::UserRole).toString()) == addressKey)
        {
            domainItem->setText(displayText);
        }
    }
}

void NetworkDock::rebuildMonitorTableByFilter()
{
    m_packetTableProjectionDirty = true;
    if (!isPacketProjectionVisible() || m_packetTable == nullptr) return;
    const QPointer<NetworkDock> self(this);
    if (ks::ui::DeferTablePresentationIfValueCaptureOpen(this,
        QStringLiteral("network-packet-filter-rebuild"), {m_packetTable},
        [self]() { if (self) self->rebuildMonitorTableByFilter(); })) return;

    auto& projection = m_packetTableProjection;
    if (!projection.active)
    {
        projection.enabled = m_packetTable->isEnabled();
        projection.cursor = m_packetTable->cursor();
        projection.sortingEnabled = m_packetTable->isSortingEnabled();
        projection.sortColumn = m_packetTable->horizontalHeader()->sortIndicatorSection();
        projection.sortOrder = m_packetTable->horizontalHeader()->sortIndicatorOrder();
        projection.selected.clear();
        if (m_packetTable->selectionModel() != nullptr)
        {
            for (const auto& index : m_packetTable->selectionModel()->selectedRows(0))
                projection.selected.insert(index.data(Qt::UserRole).toULongLong());
        }
        const auto current = m_packetTable->currentIndex();
        projection.hasCurrent = current.isValid();
        projection.currentSequence = current.isValid()
            ? current.siblingAtColumn(0).data(Qt::UserRole).toULongLong() : 0;
        projection.currentColumn = current.column();
    }
    projection.active = true;
    const auto generation = ++projection.generation;
    projection.pending = m_packetSequenceOrder;
    projection.filled.clear();
    m_packetTable->setSortingEnabled(false);
    // Partial QTableWidget rows are not actionable evidence. Keep its object,
    // shared action bar/frozen host and model identity while this short job runs.
    m_packetTable->setProperty(ks::ui::TablePresentationPreparingProperty, true);
    m_packetTable->setEnabled(false);
    m_packetTable->setCursor(Qt::WaitCursor);
    QTimer::singleShot(0, this, [self, generation]()
    {
        if (self) self->continuePacketTableProjection(generation);
    });
}

void NetworkDock::continuePacketTableProjection(const std::uint64_t generation)
{
    auto& projection = m_packetTableProjection;
    if (!projection.active || projection.generation != generation || m_packetTable == nullptr) return;
    if (!isPacketProjectionVisible()) return; // Show rebases from current canonical records.
    const QPointer<NetworkDock> self(this);
    if (ks::ui::DeferTablePresentationIfValueCaptureOpen(this,
        QStringLiteral("network-packet-filter-rebuild"), {m_packetTable},
        [self, generation]() { if (self) self->continuePacketTableProjection(generation); })) return;

    QElapsedTimer budget;
    budget.start();
    const bool updatesEnabled = m_packetTable->updatesEnabled();
    m_packetTable->setUpdatesEnabled(false);
    int units = 0;
    while (self && projection.generation == generation
        && units < 64 && budget.nsecsElapsed() < 3'000'000)
    {
        // Canonical retention may trim while GUI slices yield. Filled rows are
        // chronological until the final sort, so expired identities are a prefix.
        int expired = 0;
        while (!projection.filled.empty() && expired < std::min(32, 64 - units)
            && m_packetBySequence.find(projection.filled.front()) == m_packetBySequence.end())
        {
            projection.filled.pop_front();
            ++expired;
        }
        if (expired > 0)
        {
            m_packetTable->model()->removeRows(0, expired);
            units += expired;
            continue;
        }
        if (projection.pending.empty())
        {
            const int targetRows = static_cast<int>(projection.filled.size());
            const int surplus = m_packetTable->rowCount() - targetRows;
            if (surplus <= 0) break;
            const int remove = std::min({32, surplus, 64 - units});
            m_packetTable->model()->removeRows(m_packetTable->rowCount() - remove, remove);
            units += remove;
            continue;
        }
        const auto sequence = projection.pending.front();
        projection.pending.pop_front();
        ++units;
        const auto found = m_packetBySequence.find(sequence);
        if (found == m_packetBySequence.end()
            || !packetPassesMonitorFilter(sequence, found->second)) continue;
        // Reuse the live append path: cache/placeholder + existing asynchronous
        // icon and DNS resolution, never synchronous process/Shell lookup.
        const int writeRow = static_cast<int>(projection.filled.size());
        if (writeRow >= m_packetTable->rowCount()) m_packetTable->setRowCount(writeRow + 1);
        populatePacketMonitorRow(writeRow, found->second);
        if (!self) return;
        if (projection.generation != generation)
        {
            m_packetTable->setUpdatesEnabled(updatesEnabled);
            return;
        }
        projection.filled.push_back(sequence);
    }
    if (!self) return;
    m_packetTable->setUpdatesEnabled(updatesEnabled);
    if (projection.generation != generation) return;
    const bool expiredFront = !projection.filled.empty()
        && m_packetBySequence.find(projection.filled.front()) == m_packetBySequence.end();
    if (!projection.pending.empty() || expiredFront
        || m_packetTable->rowCount() > static_cast<int>(projection.filled.size()))
    {
        QTimer::singleShot(0, this, [self, generation]()
        {
            if (self) self->continuePacketTableProjection(generation);
        });
        return;
    }

    // The shared one-shot header policy invalidates manual sorting on data
    // changes, exactly as before. Only native continuous sorting is restored.
    if (projection.sortingEnabled && projection.sortColumn >= 0)
        m_packetTable->horizontalHeader()->setSortIndicator(projection.sortColumn, projection.sortOrder);
    if (!self || projection.generation != generation) return;
    m_packetTable->setSortingEnabled(projection.sortingEnabled);
    if (!self || projection.generation != generation) return;
    QItemSelection selection;
    QModelIndex current;
    int runStart = -1;
    const int rowCount = m_packetTable->rowCount();
    for (int row = 0; row <= rowCount; ++row)
    {
        const auto index = row < rowCount ? m_packetTable->model()->index(row, 0) : QModelIndex();
        const auto sequence = index.data(Qt::UserRole).toULongLong();
        const bool selected = index.isValid() && projection.selected.contains(sequence);
        if (selected && runStart < 0) runStart = row;
        if (!selected && runStart >= 0)
        {
            selection.select(m_packetTable->model()->index(runStart, 0),
                m_packetTable->model()->index(row - 1, m_packetTable->columnCount() - 1));
            runStart = -1;
        }
        if (projection.hasCurrent && index.isValid() && sequence == projection.currentSequence)
            current = index.siblingAtColumn(std::max(0, projection.currentColumn));
    }
    if (m_packetTable->selectionModel() != nullptr)
    {
        m_packetTable->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        if (!self || projection.generation != generation) return;
        if (current.isValid()) m_packetTable->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    }
    if (!self || projection.generation != generation) return;
    projection.active = false;
    projection.pending.clear();
    projection.filled.clear();
    projection.selected.clear();
    m_packetTableProjectionDirty = false;
    m_packetTable->setCursor(projection.cursor);
    m_packetTable->setProperty(ks::ui::TablePresentationPreparingProperty, false);
    m_packetTable->setEnabled(projection.enabled);
    if (current.isValid()) m_packetTable->scrollTo(current);
    else if (rowCount > 0) m_packetTable->scrollToBottom();
}

void NetworkDock::applyPacketTimelineSelection(
    const std::uint64_t start100ns,
    const std::uint64_t end100ns)
{
    // 时间轴控件负责把鼠标框选转换成时间戳；NetworkDock 只保存范围并重建表格。
    m_packetTimelineSelectionStart100ns = std::min(start100ns, end100ns);
    m_packetTimelineSelectionEnd100ns = std::max(start100ns, end100ns);
    m_packetTimelineUserSelectionActive = true;

    updateMonitorFilterStateLabel();
    rebuildMonitorTableByFilter();

    kLogEvent timelineEvent;
    dbg << timelineEvent
        << "[NetworkDock] 流量时间轴选区变更, start100ns="
        << m_packetTimelineSelectionStart100ns
        << ", end100ns="
        << m_packetTimelineSelectionEnd100ns
        << eol;
}

void NetworkDock::resetPacketTimelineToCurrentRange()
{
    // 清空/清筛选操作后让时间轴恢复默认全范围，避免旧选区继续影响下一轮抓包。
    m_packetTimelineUserSelectionActive = false;
    m_packetTimelineSelectionStart100ns = 0;
    m_packetTimelineSelectionEnd100ns = 0;
    m_packetTimelineRangeStart100ns = 0;
    m_packetTimelineRangeEnd100ns = currentPacketTimelineEnd100ns();
    if (m_packetTimelineRangeEnd100ns == 0)
    {
        m_packetTimelineRangeEnd100ns = kFallbackTimelineRange100ns;
    }

    if (m_packetTimelineWidget != nullptr)
    {
        m_packetTimelineWidget->resetTimeline(0);
        m_packetTimelineWidget->setCaptureRange(
            m_packetTimelineRangeStart100ns,
            m_packetTimelineRangeEnd100ns);
        m_packetTimelineSelectionStart100ns = m_packetTimelineWidget->selectionStart100ns();
        m_packetTimelineSelectionEnd100ns = m_packetTimelineWidget->selectionEnd100ns();
        refreshPacketTimelinePoints();
    }
    refreshPacketTimelineRatePoints();
}

void NetworkDock::refreshPacketTimelineRange()
{
    std::uint64_t rangeStart100ns = 0;
    // 当前累计监控时长始终不早于已缓存报文的压缩时间戳，
    // 因此不必在每个刷新周期扫描整个报文缓存来重算右边界。
    std::uint64_t rangeEnd100ns = currentPacketTimelineEnd100ns();

    if (rangeEnd100ns == 0)
    {
        // 没有任何报文且未开始监控时，使用 0~1 秒作为空时间轴。
        rangeEnd100ns = kFallbackTimelineRange100ns;
    }

    if (rangeEnd100ns <= rangeStart100ns)
    {
        rangeEnd100ns = rangeStart100ns + kFallbackTimelineRange100ns;
    }

    m_packetTimelineRangeStart100ns = rangeStart100ns;
    m_packetTimelineRangeEnd100ns = rangeEnd100ns;
    if (!isPacketProjectionVisible())
    {
        m_packetTimelineProjectionDirty = true;
        return;
    }
    if (m_packetTimelineWidget == nullptr)
    {
        return;
    }
    // The visible range is committed with its worker-prepared events and rates.
    // Canonical full-range filtering still follows acquisition immediately.
    if (!m_packetTimelineUserSelectionActive)
    {
        m_packetTimelineSelectionStart100ns = rangeStart100ns;
        m_packetTimelineSelectionEnd100ns = rangeEnd100ns;
    }
}

void NetworkDock::refreshPacketTimelinePoints()
{
    // Events, rate values and range are one complete projection snapshot.
    refreshPacketTimelineRatePoints();
}

void NetworkDock::refreshPacketTimelineRatePoints()
{
    m_packetTimelineProjectionDirty = true;
    if (!isPacketProjectionVisible() || m_packetTimelineWidget == nullptr
        || m_packetTimelineProjectionQueued) return;
    m_packetTimelineProjectionQueued = true;
    const QPointer<NetworkDock> self(this);
    QTimer::singleShot(0, this, [self]()
    {
        if (!self) return;
        if (!self->isPacketProjectionVisible() || self->m_packetTimelineWidget == nullptr)
        {
            self->m_packetTimelineProjectionQueued = false;
            return;
        }
        struct Sample
        {
            ProcessTraceTimelineEventPoint point;
            std::uint64_t uploadBytes = 0;
            std::uint64_t downloadBytes = 0;
        };
        struct Input
        {
            std::vector<Sample> samples;
            ProcessTraceTimelineWidget::EventProjectionParameters projection;
            std::uint64_t epoch = 0;
            std::uint64_t monitorGeneration = 0;
            std::uint64_t firstSequence = 0;
            std::uint64_t lastSequence = 0;
            std::size_t count = 0;
        };
        struct Output
        {
            ProcessTraceTimelineWidget::PreparedEventPoints events;
            ProcessTraceTimelineWidget::PreparedRateOverlay rates;
        };
        auto owner = std::make_shared<Input>();
        // The mutable alias is used only by GUI capture. Publishing below hands
        // the same complete value to AsyncOperation without a second vector copy.
        auto input = std::shared_ptr<Input>(ks::ui::RetainCompleteExportSnapshot(
            std::shared_ptr<const Input>(owner)), owner.get());
        owner.reset();
        self->refreshPacketTimelineRange();
        input->projection = self->m_packetTimelineWidget->eventProjectionParameters(
            self->m_packetTimelineRangeStart100ns, self->m_packetTimelineRangeEnd100ns);
        input->epoch = self->m_packetTimelineProjectionEpoch;
        input->monitorGeneration = self->m_monitorGeneration.load();
        input->count = self->m_packetTimelineEventPoints.size();
        if (!self->m_packetSequenceOrder.empty())
        {
            input->firstSequence = self->m_packetSequenceOrder.front();
            input->lastSequence = self->m_packetSequenceOrder.back();
        }
        input->samples.reserve(input->count);
        const QPointer<ProcessTraceTimelineWidget> timeline(self->m_packetTimelineWidget);
        const auto sourceMatches = [self, timeline, input]()
        {
            return self && timeline && self->m_packetTimelineWidget == timeline
                && input->epoch == self->m_packetTimelineProjectionEpoch
                && input->monitorGeneration == self->m_monitorGeneration.load()
                && self->m_packetSequenceOrder.size() >= input->count
                && self->m_packetTimelineEventPoints.size() >= input->count
                && (input->count == 0 || (self->m_packetSequenceOrder.front() == input->firstSequence
                    && self->m_packetSequenceOrder[input->count - 1] == input->lastSequence));
        };
        // A weak continuation owns no cycle. Every queued slice retains its
        // complete partial value through the existing off-GUI retirement lane.
        auto capture = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> nextCapture(capture);
        *capture = [self, timeline, input, sourceMatches, nextCapture]()
        {
            if (!self) return;
            const auto retry = [self]()
            {
                if (!self) return;
                self->m_packetTimelineProjectionQueued = false;
                self->refreshPacketTimelineRatePoints();
            };
            if (!sourceMatches() || !self->isPacketProjectionVisible())
            {
                retry();
                return;
            }
            QElapsedTimer budget;
            budget.start();
            const std::size_t stop = std::min(input->count, input->samples.size() + 256);
            while (input->samples.size() < stop)
            {
                const auto index = input->samples.size();
                const auto found = self->m_packetBySequence.find(self->m_packetSequenceOrder[index]);
                if (found == self->m_packetBySequence.end()) { retry(); return; }
                Sample sample;
                sample.point = self->m_packetTimelineEventPoints[index];
                if (found->second.direction == ks::network::PacketDirection::Outbound)
                    sample.uploadBytes = found->second.totalPacketSize;
                else if (found->second.direction == ks::network::PacketDirection::Inbound)
                    sample.downloadBytes = found->second.totalPacketSize;
                input->samples.push_back(std::move(sample));
                if (budget.elapsed() >= 3) break;
            }
            if (input->samples.size() != input->count)
            {
                if (const auto next = nextCapture.lock())
                    QTimer::singleShot(0, self.data(), [next]() { (*next)(); });
                return;
            }
            self->m_packetTimelineProjectionQueued = false;
            if (self->m_packetTimelineProjectionOperation == nullptr)
                self->m_packetTimelineProjectionOperation = new ks::ui::AsyncOperation(self.data());
            self->m_packetTimelineProjectionOperation->submit<std::shared_ptr<const Input>,
                std::shared_ptr<const Output>>(input,
                [](const std::shared_ptr<const Input>& value,
                    const ks::ui::AsyncOperationToken& token) -> std::shared_ptr<const Output>
                {
                    Output output;
                    std::vector<ProcessTraceTimelineEventPoint> points;
                    std::unordered_map<std::uint64_t, PacketTimelineRateBucket> buckets;
                    points.reserve(value->samples.size());
                    for (const auto& sample : value->samples)
                    {
                        if (token.isCanceled()) return {};
                        points.push_back(sample.point);
                        auto& bucket = buckets[sample.point.time100ns / kTimelineSecond100ns];
                        bucket.uploadBytes += sample.uploadBytes;
                        bucket.downloadBytes += sample.downloadBytes;
                    }
                    output.events = ProcessTraceTimelineWidget::prepareEventPoints(
                        std::move(points), value->projection);
                    const auto endSecond = value->projection.end100ns / kTimelineSecond100ns;
                    std::vector<std::uint64_t> seconds;
                    seconds.reserve(buckets.size() * 3 + 2);
                    for (const auto& bucket : buckets)
                    {
                        if (token.isCanceled()) return {};
                        const auto second = bucket.first;
                        seconds.push_back(second);
                        if (second > 0) seconds.push_back(second - 1);
                        if (second < endSecond) seconds.push_back(second + 1);
                    }
                    seconds.push_back(0);
                    seconds.push_back(endSecond);
                    std::sort(seconds.begin(), seconds.end());
                    seconds.erase(std::unique(seconds.begin(), seconds.end()), seconds.end());
                    std::vector<ProcessTraceTimelineRatePoint> rates;
                    rates.reserve(seconds.size());
                    for (const auto second : seconds)
                    {
                        if (token.isCanceled()) return {};
                        const auto found = buckets.find(second);
                        ProcessTraceTimelineRatePoint point;
                        point.time100ns = second * kTimelineSecond100ns;
                        if (found != buckets.end())
                        {
                            point.uploadBytesPerSecond = static_cast<double>(found->second.uploadBytes);
                            point.downloadBytesPerSecond = static_cast<double>(found->second.downloadBytes);
                        }
                        rates.push_back(point);
                    }
                    output.rates = ProcessTraceTimelineWidget::prepareRateOverlayPoints(std::move(rates));
                    return std::make_shared<Output>(std::move(output));
                },
                [self, timeline, input, sourceMatches](const std::shared_ptr<const Output>& output,
                    const std::uint64_t ticket)
                {
                    if (!self || !output) return true;
                    const auto apply = [self, timeline, input, sourceMatches](const Output& value)
                    {
                        if (!self || !timeline) return;
                        if (!sourceMatches() || !self->isPacketProjectionVisible()
                            || !timeline->applyPreparedEventPoints(value.events))
                        {
                            self->refreshPacketTimelineRatePoints();
                            return;
                        }
                        timeline->applyRateOverlayPoints(value.rates);
                        self->m_packetTimelineSelectionStart100ns = timeline->selectionStart100ns();
                        self->m_packetTimelineSelectionEnd100ns = timeline->selectionEnd100ns();
                        self->m_packetTimelineProjectionDirty = false;
                    };
                    if (ks::ui::DeferTablePresentationIfValueCaptureOpen(self.data(),
                        QStringLiteral("network-packet-timeline"), {self->m_packetTable},
                        [self, output, ticket, apply]()
                        {
                            if (!self || self->m_packetTimelineProjectionOperation == nullptr) return;
                            if (self->m_packetTimelineProjectionOperation->isCurrent(ticket)) apply(*output);
                            self->m_packetTimelineProjectionOperation->completeDeferred(ticket);
                        })) return false;
                    apply(*output);
                    return true;
                },
                [self](std::exception_ptr)
                {
                    if (self) self->m_packetTimelineProjectionDirty = true;
                });
        };
        (*capture)();
    });
}

bool NetworkDock::isPacketTimelineFilterActive() const
{
    if (!m_packetTimelineUserSelectionActive
        || m_packetTimelineSelectionEnd100ns == 0
        || m_packetTimelineSelectionEnd100ns <= m_packetTimelineSelectionStart100ns)
    {
        return false;
    }

    if (m_packetTimelineRangeEnd100ns <= m_packetTimelineRangeStart100ns)
    {
        return false;
    }

    // 全范围选区不视为过滤；这能让实时抓包默认持续显示新包。
    return m_packetTimelineSelectionStart100ns > m_packetTimelineRangeStart100ns
        || m_packetTimelineSelectionEnd100ns < m_packetTimelineRangeEnd100ns;
}

bool NetworkDock::packetPassesTimelineFilter(const ks::network::PacketRecord& packetRecord) const
{
    if (!isPacketTimelineFilterActive())
    {
        return true;
    }

    const std::uint64_t packetTime100ns = packetTimelineTimeForRecord(packetRecord);
    return packetTime100ns >= m_packetTimelineSelectionStart100ns
        && packetTime100ns <= m_packetTimelineSelectionEnd100ns;
}

bool NetworkDock::packetPassesTimelineFilter(
    const std::uint64_t sequenceId,
    const ks::network::PacketRecord& packetRecord) const
{
    if (!isPacketTimelineFilterActive())
    {
        return true;
    }

    const std::uint64_t packetTime100ns = packetTimelineTimeForSequence(sequenceId, packetRecord);
    return packetTime100ns >= m_packetTimelineSelectionStart100ns
        && packetTime100ns <= m_packetTimelineSelectionEnd100ns;
}

void NetworkDock::beginPacketTimelineMonitorSession()
{
    // 已存在活动会话时不重复登记，避免双击开始按钮导致累计时间错位。
    if (m_packetTimelineSessionActive)
    {
        return;
    }

    // 如果上一个会话还处于“未闭合”状态，先用当前时间闭合它；
    // 这样即使存在异常状态回调顺序，也不会让停机间隔混入下一段监控时长。
    if (!m_packetTimelineSessionList.empty()
        && m_packetTimelineSessionList.back().endUnixMs == 0)
    {
        PacketTimelineCaptureSession& previousSession = m_packetTimelineSessionList.back();
        previousSession.endUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        const std::uint64_t previousDurationMs = previousSession.endUnixMs > previousSession.startUnixMs
            ? (previousSession.endUnixMs - previousSession.startUnixMs)
            : 0;
        m_packetTimelineAccumulatedActive100ns =
            previousSession.baseStart100ns + unixMsToTimeline100ns(previousDurationMs);
    }

    PacketTimelineCaptureSession session;
    session.startUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    session.endUnixMs = 0;
    session.baseStart100ns = m_packetTimelineAccumulatedActive100ns;
    m_packetTimelineSessionList.push_back(session);
    m_packetTimelineSessionActive = true;
    m_packetTimelineLastHeartbeatSecond = session.baseStart100ns / kTimelineSecond100ns;

    refreshPacketTimelineRange();
    refreshPacketTimelineRatePoints();
}

void NetworkDock::endPacketTimelineMonitorSession()
{
    if (!m_packetTimelineSessionActive || m_packetTimelineSessionList.empty())
    {
        return;
    }

    PacketTimelineCaptureSession& session = m_packetTimelineSessionList.back();
    if (session.endUnixMs == 0)
    {
        session.endUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    }

    const std::uint64_t sessionDurationMs = session.endUnixMs > session.startUnixMs
        ? (session.endUnixMs - session.startUnixMs)
        : 0;
    m_packetTimelineAccumulatedActive100ns =
        session.baseStart100ns + unixMsToTimeline100ns(sessionDurationMs);
    m_packetTimelineSessionActive = false;
    m_packetTimelineLastHeartbeatSecond = m_packetTimelineAccumulatedActive100ns / kTimelineSecond100ns;

    refreshPacketTimelineRange();
    refreshPacketTimelineRatePoints();
}

void NetworkDock::resetPacketTimelineClockForCurrentState()
{
    ++m_packetTimelineProjectionEpoch;
    if (m_packetTimelineProjectionOperation != nullptr) m_packetTimelineProjectionOperation->cancel();
    // wasRunning 用途：在清理会话列表前捕获当前运行状态，避免下面清空状态后误判。
    const bool wasRunning = m_monitorRunning
        || (m_trafficService != nullptr && m_trafficService->IsRunning());

    // 清空报文意味着开始一条新的逻辑时间轴：
    // - 历史会话与报文都丢弃；
    // - 如果用户当前仍在监控，则立即从 0 秒重新登记活动会话。
    m_packetTimelineSessionList.clear();
    m_packetTimelineTimeBySequence.clear();
    m_packetTimelineRateBucketBySecond.clear();
    m_packetTimelineAccumulatedActive100ns = 0;
    m_packetTimelineLastHeartbeatSecond = 0;
    m_packetTimelineSessionActive = false;

    if (wasRunning)
    {
        beginPacketTimelineMonitorSession();
    }
}

std::uint64_t NetworkDock::packetTimelineTimeForRecord(const ks::network::PacketRecord& packetRecord) const
{
    if (packetRecord.captureTimestampMs == 0)
    {
        return currentPacketTimelineEnd100ns();
    }

    if (m_packetTimelineSessionList.empty()
        && m_packetTimelineSessionActive
        && m_packetTimelineAccumulatedActive100ns == 0)
    {
        // 理论上 beginPacketTimelineMonitorSession 会先登记会话；
        // 若异常顺序导致列表为空，则把报文放在 0 秒位置，仍然不使用真实系统时间。
        return 0;
    }

    for (const PacketTimelineCaptureSession& session : m_packetTimelineSessionList)
    {
        const std::uint64_t sessionEndUnixMs = session.endUnixMs == 0
            ? std::max<std::uint64_t>(
                packetRecord.captureTimestampMs,
                static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()))
            : session.endUnixMs;
        if (packetRecord.captureTimestampMs < session.startUnixMs
            || packetRecord.captureTimestampMs > sessionEndUnixMs)
        {
            continue;
        }

        const std::uint64_t elapsedMs = packetRecord.captureTimestampMs > session.startUnixMs
            ? (packetRecord.captureTimestampMs - session.startUnixMs)
            : 0;
        return session.baseStart100ns + unixMsToTimeline100ns(elapsedMs);
    }

    if (m_packetTimelineSessionActive && !m_packetTimelineSessionList.empty())
    {
        // 极少数情况下报文可能早于 UI 登记会话（线程启动竞争）：
        // - 归入当前活动会话起点，而不是当前时间；
        // - 这样第一批报文不会被压到右边界造成瞬间速率尖峰。
        return m_packetTimelineSessionList.back().baseStart100ns;
    }

    // 停止后才到达的滞后报文归入累计末尾，确保不会落到真实系统时间造成横轴跳变。
    return currentPacketTimelineEnd100ns();
}

std::uint64_t NetworkDock::packetTimelineTimeForSequence(
    const std::uint64_t sequenceId,
    const ks::network::PacketRecord& packetRecord) const
{
    const auto iterator = m_packetTimelineTimeBySequence.find(sequenceId);
    if (iterator != m_packetTimelineTimeBySequence.end())
    {
        return iterator->second;
    }
    return packetTimelineTimeForRecord(packetRecord);
}

std::uint64_t NetworkDock::currentPacketTimelineEnd100ns() const
{
    if (m_packetTimelineSessionActive && !m_packetTimelineSessionList.empty())
    {
        const PacketTimelineCaptureSession& session = m_packetTimelineSessionList.back();
        const std::uint64_t nowUnixMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        const std::uint64_t elapsedMs = nowUnixMs > session.startUnixMs
            ? (nowUnixMs - session.startUnixMs)
            : 0;
        return session.baseStart100ns + unixMsToTimeline100ns(elapsedMs);
    }

    return m_packetTimelineAccumulatedActive100ns;
}

void NetworkDock::addPacketTimelineRateSample(
    const ks::network::PacketRecord& packetRecord,
    const std::uint64_t timelineTime100ns)
{
    const std::uint64_t secondIndex = timelineTime100ns / kTimelineSecond100ns;
    PacketTimelineRateBucket& bucket = m_packetTimelineRateBucketBySecond[secondIndex];

    if (packetRecord.direction == ks::network::PacketDirection::Outbound)
    {
        bucket.uploadBytes += packetRecord.totalPacketSize;
    }
    else if (packetRecord.direction == ks::network::PacketDirection::Inbound)
    {
        bucket.downloadBytes += packetRecord.totalPacketSize;
    }
}

void NetworkDock::removePacketTimelineRateSample(
    const ks::network::PacketRecord& packetRecord,
    const std::uint64_t timelineTime100ns)
{
    const std::uint64_t secondIndex = timelineTime100ns / kTimelineSecond100ns;
    const auto iterator = m_packetTimelineRateBucketBySecond.find(secondIndex);
    if (iterator == m_packetTimelineRateBucketBySecond.end())
    {
        return;
    }

    PacketTimelineRateBucket& bucket = iterator->second;
    if (packetRecord.direction == ks::network::PacketDirection::Outbound)
    {
        bucket.uploadBytes = bucket.uploadBytes > packetRecord.totalPacketSize
            ? bucket.uploadBytes - packetRecord.totalPacketSize
            : 0;
    }
    else if (packetRecord.direction == ks::network::PacketDirection::Inbound)
    {
        bucket.downloadBytes = bucket.downloadBytes > packetRecord.totalPacketSize
            ? bucket.downloadBytes - packetRecord.totalPacketSize
            : 0;
    }

    if (bucket.uploadBytes == 0 && bucket.downloadBytes == 0)
    {
        m_packetTimelineRateBucketBySecond.erase(iterator);
    }
}

void NetworkDock::trimOldestPacketWhenNeeded()
{
    if (m_packetSequenceOrder.size() <= kMaxPacketCacheCount)
    {
        return;
    }

    // 批量裁剪策略：
    // - 避免“每来一包就 removeRow(0)”导致持续 O(n) 行移动；
    // - 改为累计到阈值后一次性移除一批，显著降低 UI 抖动和卡顿。
    constexpr std::size_t kTrimBatchCount = 320;
    const std::size_t overflowCount = m_packetSequenceOrder.size() - kMaxPacketCacheCount;

    // 只有当“溢出量达到批处理阈值”时才执行批量裁剪，
    // 避免刚超 1 条就频繁触发重排。
    if (overflowCount < kTrimBatchCount &&
        m_packetSequenceOrder.size() < (kMaxPacketCacheCount + kTrimBatchCount))
    {
        return;
    }

    const std::size_t trimCount = std::max<std::size_t>(overflowCount, kTrimBatchCount);

    std::size_t visibleTrimCount = 0;
    std::size_t timelineTrimCount = 0;
    for (std::size_t trimIndex = 0; trimIndex < trimCount; ++trimIndex)
    {
        if (m_packetSequenceOrder.empty())
        {
            break;
        }

        const std::uint64_t oldestSequenceId = m_packetSequenceOrder.front();
        m_packetSequenceOrder.pop_front();
        ++timelineTrimCount;

        const auto oldestIterator = m_packetBySequence.find(oldestSequenceId);
        if (oldestIterator == m_packetBySequence.end())
        {
            m_packetTimelineTimeBySequence.erase(oldestSequenceId);
            continue;
        }

        const std::uint64_t timelineTime100ns =
            packetTimelineTimeForSequence(oldestSequenceId, oldestIterator->second);
        removePacketTimelineRateSample(oldestIterator->second, timelineTime100ns);
        m_packetTimelineTimeBySequence.erase(oldestSequenceId);

        if (packetPassesMonitorFilter(oldestSequenceId, oldestIterator->second))
        {
            ++visibleTrimCount;
        }
        m_packetBySequence.erase(oldestIterator);
    }

    const std::size_t timelineEraseCount = std::min(timelineTrimCount, m_packetTimelineEventPoints.size());
    if (timelineEraseCount > 0)
    {
        m_packetTimelineEventPoints.erase(
            m_packetTimelineEventPoints.begin(),
            m_packetTimelineEventPoints.begin() + static_cast<std::ptrdiff_t>(timelineEraseCount));
    }

    // Presentation backlog keeps only identities still retained by the
    // canonical cache, even if arrivals outrun a temporarily blocked GUI job.
    while (!m_packetTableProjection.pending.empty()
        && m_packetBySequence.find(m_packetTableProjection.pending.front()) == m_packetBySequence.end())
        m_packetTableProjection.pending.pop_front();

    if (!isPacketProjectionVisible() || m_packetTableProjectionDirty)
    {
        m_packetTableProjectionDirty = true;
        m_packetTimelineProjectionDirty = true;
        return;
    }

    // 仅在当前表确有可见裁剪时才删除行，减少不必要的 UI 操作。
    if (visibleTrimCount > 0 && m_packetTable != nullptr && m_packetTable->rowCount() > 0)
    {
        const int removeRows = std::min<int>(static_cast<int>(visibleTrimCount), m_packetTable->rowCount());
        const bool updatesEnabled = m_packetTable->updatesEnabled();
        m_packetTable->setUpdatesEnabled(false);
        if (m_packetTable->model() != nullptr)
        {
            m_packetTable->model()->removeRows(0, removeRows);
        }
        m_packetTable->setUpdatesEnabled(updatesEnabled);

        kLogEvent trimEvent;
        dbg << trimEvent
            << "[NetworkDock] 报文缓存批量裁剪, trimCount=" << trimCount
            << ", visibleTrimCount=" << visibleTrimCount
            << ", remainCache=" << m_packetSequenceOrder.size()
            << eol;
    }

    refreshPacketTimelineRange();
    refreshPacketTimelinePoints();
    refreshPacketTimelineRatePoints();
}

void NetworkDock::clearAllPacketRows()
{
    const QPointer<NetworkDock> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("network-packet-clear"),
        {m_packetTable, m_nidsAlertTable},
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->clearAllPacketRows();
            }
        }))
    {
        return;
    }

    ++m_packetTableProjection.generation;
    m_packetTableProjectionDirty = true;
    auto& projection = m_packetTableProjection;
    if (m_packetTable != nullptr)
    {
        if (!projection.active)
        {
            projection.enabled = m_packetTable->isEnabled();
            projection.cursor = m_packetTable->cursor();
            projection.sortingEnabled = m_packetTable->isSortingEnabled();
            projection.sortColumn = m_packetTable->horizontalHeader()->sortIndicatorSection();
            projection.sortOrder = m_packetTable->horizontalHeader()->sortIndicatorOrder();
        }
        // Canonical clear is immediate. Until the pure presentation commit can
        // run (including an export capture lease), old rows must not be actions.
        projection.active = true;
        m_packetTable->setProperty(ks::ui::TablePresentationPreparingProperty, true);
        m_packetTable->setEnabled(false);
        m_packetTable->setCursor(Qt::WaitCursor);
    }
    projection.pending.clear();
    projection.filled.clear();
    projection.selected.clear();
    projection.currentSequence = 0;
    projection.hasCurrent = false;

    // 同时清空后台待刷新队列，避免“刚清空又回填旧数据”。
    {
        std::lock_guard<std::mutex> guard(m_pendingPacketMutex);
        m_pendingPacketQueue.clear();
        m_droppedPacketCount = 0;
    }

    m_packetSequenceOrder.clear();
    m_packetBySequence.clear();
    m_packetTimelineEventPoints.clear();
    m_lastPacketTimelineRefreshMs = 0;
    clearNidsAlerts();
    resetPacketTimelineClockForCurrentState();
    resetPacketTimelineToCurrentRange();
    refreshPacketTimelinePoints();
    refreshPacketTimelineRatePoints();
    updateMonitorFilterStateLabel();

    m_packetTableProjectionDirty = true;
    rebuildMonitorTableByFilter();

    kLogEvent clearEvent;
    info << clearEvent << "[NetworkDock] 用户清空了网络报文列表。" << eol;
}



