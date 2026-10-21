#include "StartupDock.Internal.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/AsyncOperation.h"
#include "../UI/AsyncCleanupRegistry.h"
#include <mutex>
#include <QImage>
#include <QPixmap>
#include <Objbase.h>

using namespace startup_dock_detail;

struct StartupDock::StartupRefreshSession
{
    enum class State { Running, Complete, Stopped, Failed };
    std::mutex mutex;
    std::deque<RefreshStageResult> pending;
    std::atomic_bool stopping{false};
    State state = State::Running;
};

StartupDock::StartupDock(QWidget* parent)
    : QWidget(parent)
{
    m_refreshOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_refreshOperation->setStateChangedCallback([this]() { drainRefreshSession(); });
    m_refreshDrainTimer = new QTimer(this);
    m_refreshDrainTimer->setSingleShot(true);
    connect(m_refreshDrainTimer, &QTimer::timeout, this, [this]() { drainRefreshSession(); });
    m_actionOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_actionOperation->setStateChangedCallback([this]() { reconcileStartupActionReceipt(); });
    m_exportOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_exportOperation->setStateChangedCallback([this]() { reconcileExportReceipt(); });
    m_iconOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_iconOperation->setStateChangedCallback([this]() {
        reconcileEntryIconBatch();
        scheduleEntryIconDrain();
    });
    // 初始化顺序：
    // - 先创建 UI；
    // - 再连接交互；
    // - 启动项全量枚举改为“首次显示时懒加载”，避免拖慢主窗口启动。
    initializeUi();
    initializeConnections();

    kLogEvent initEvent;
    info << initEvent
        << startupText("startup.log.initialized", QStringLiteral("[StartupDock] 启动项页初始化完成。"))
               .toStdString()
        << eol;
}

StartupDock::~StartupDock()
{
    m_exportOperation->close();
    m_iconOperation->close();
    m_destroying.store(true);
    if (m_tableRebuildTimer != nullptr)
    {
        m_tableRebuildTimer->stop();
    }
    m_actionOperation->close();
    if (m_refreshSession) m_refreshSession->stopping.store(true);
    m_refreshDrainTimer->stop();
    m_refreshOperation->close();
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->stop();
    }

    kLogEvent destroyEvent;
    info << destroyEvent
        << startupText("startup.log.destroyed", QStringLiteral("[StartupDock] 启动项页已析构。"))
               .toStdString()
        << eol;
}

void StartupDock::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event == nullptr || event->type() != QEvent::LanguageChange)
    {
        return;
    }

    applyTranslatedHeaders();
    rebuildAllTables();
    if (m_tableRebuildInProgress)
    {
        return;
    }
    if (m_statusLabel == nullptr)
    {
        return;
    }
    if (m_refreshInProgress.load())
    {
        m_statusLabel->setText(refreshStatusText(
            m_refreshQueued.load()
                ? startupText(
                    "startup.status.refresh_queued",
                    QStringLiteral("状态：后台刷新进行中，已记录新的刷新请求"))
                : startupText(
                    "startup.status.refreshing",
                    QStringLiteral("状态：后台正在枚举启动项..."))));
    }
    else if (!m_initialRefreshDone)
    {
        m_statusLabel->setText(refreshStatusText(
            startupText(
                "startup.status.initial",
                QStringLiteral("状态：首次打开该页时加载启动项"))));
    }
    else
    {
        m_statusLabel->setText(refreshSummaryText());
    }
}

int StartupDock::toStartupColumn(const StartupColumn column)
{
    return static_cast<int>(column);
}

QString StartupDock::categoryToText(const StartupCategory category)
{
    switch (category)
    {
    case StartupCategory::All:
        return startupText("startup.category.overview", QStringLiteral("总览"));
    case StartupCategory::Logon:
        return startupText("startup.category.logon", QStringLiteral("登录"));
    case StartupCategory::Services:
        return startupText("startup.category.services", QStringLiteral("服务"));
    case StartupCategory::Drivers:
        return startupText("startup.category.drivers", QStringLiteral("驱动"));
    case StartupCategory::Tasks:
        return startupText("startup.category.tasks", QStringLiteral("计划任务"));
    case StartupCategory::ImageHijack:
        return startupText("startup.category.image_hijack", QStringLiteral("映像劫持"));
    case StartupCategory::Registry:
        return startupText("startup.category.registry", QStringLiteral("高级注册表"));
    case StartupCategory::Wmi:
        return startupText("startup.category.wmi", QStringLiteral("WMI"));
    case StartupCategory::Hidden:
        return startupText("startup.category.hidden", QStringLiteral("隐藏项"));
    default:
        return startupText("startup.category.unknown", QStringLiteral("未知"));
    }
}

void StartupDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (m_initialRefreshDone)
    {
        return;
    }

    m_initialRefreshDone = true;
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(refreshStatusText(
            startupText("startup.status.first_load", QStringLiteral("状态：首次打开，正在加载启动项..."))));
    }

    // 延迟到事件循环末尾再发起首次后台枚举：
    // - 先把页签本身显示出来，避免用户感知为“点击无响应”；
    // - 同时把重活移出主窗口构造阶段，优化启动速度。
    QTimer::singleShot(0, this, [this]()
        {
            requestAsyncRefresh(true);
        });
}

StartupDock::StartupCategory StartupDock::currentCategory() const
{
    if (m_sideTabWidget == nullptr)
    {
        return StartupCategory::All;
    }

    switch (m_sideTabWidget->currentIndex())
    {
    case 0:
        return StartupCategory::All;
    case 1:
        return StartupCategory::Logon;
    case 2:
        return StartupCategory::Services;
    case 3:
        return StartupCategory::Drivers;
    case 4:
        return StartupCategory::Tasks;
    case 5:
        return StartupCategory::ImageHijack;
    case 6:
        return StartupCategory::Registry;
    case 7:
        return StartupCategory::Wmi;
    case 8:
        return StartupCategory::Hidden;
    default:
        return StartupCategory::All;
    }
}

QTableWidget* StartupDock::currentCategoryTable() const
{
    switch (currentCategory())
    {
    case StartupCategory::All:
        return m_allTable;
    case StartupCategory::Logon:
        return m_logonTable;
    case StartupCategory::Services:
        return m_servicesTable;
    case StartupCategory::Drivers:
        return m_driversTable;
    case StartupCategory::Tasks:
        return m_tasksTable;
    case StartupCategory::ImageHijack:
        return m_imageHijackTable;
    case StartupCategory::Registry:
        return nullptr;
    case StartupCategory::Wmi:
        return m_wmiTable;
    case StartupCategory::Hidden:
        return m_hiddenTable;
    default:
        return m_allTable;
    }
}

QString StartupDock::entryIconKey(const StartupEntry& entry)
{
    return entry.imagePathText.trimmed().isEmpty()
        ? QStringLiteral("type:%1").arg(entry.sourceTypeText)
        : QStringLiteral("path:%1").arg(QDir::toNativeSeparators(entry.imagePathText));
}

QIcon StartupDock::resolveEntryIcon(const StartupEntry& entry)
{
    const QString cacheKeyText = entryIconKey(entry);
    const auto cacheIt = m_iconCache.constFind(cacheKeyText);
    if (cacheIt != m_iconCache.constEnd()) return cacheIt.value();
    QIcon resolvedIcon;
    if (resolvedIcon.isNull())
    {
        if (entry.category == StartupCategory::Services)
        {
            resolvedIcon = createBlueIcon(":/Icon/process_start.svg");
        }
        else if (entry.category == StartupCategory::Drivers)
        {
            resolvedIcon = createBlueIcon(":/Icon/process_details.svg");
        }
        else if (entry.category == StartupCategory::Tasks)
        {
            resolvedIcon = createBlueIcon(":/Icon/process_refresh.svg");
        }
        else if (entry.category == StartupCategory::ImageHijack)
        {
            resolvedIcon = createBlueIcon(":/Icon/startup_image_hijack.svg");
        }
        else if (entry.category == StartupCategory::Registry)
        {
            resolvedIcon = createBlueIcon(":/Icon/file_find.svg");
        }
        else if (entry.category == StartupCategory::Hidden)
        {
            resolvedIcon = createBlueIcon(":/Icon/startup_hidden.svg");
        }
        else
        {
            resolvedIcon = createBlueIcon(":/Icon/process_main.svg");
        }
    }

    m_iconCache.insert(cacheKeyText, resolvedIcon);
    if (!entry.imagePathText.trimmed().isEmpty() && !m_requestedIconKeys.contains(cacheKeyText))
    {
        m_requestedIconKeys.insert(cacheKeyText);
        m_pendingIconPaths.insert(cacheKeyText, entry.imagePathText);
        scheduleEntryIconDrain();
    }
    return resolvedIcon;
}

void StartupDock::trackEntryIcon(const StartupEntry& entry, const QPersistentModelIndex& index)
{
    const QString key = entryIconKey(entry);
    if (!index.isValid() || !m_requestedIconKeys.contains(key)) return;
    auto* model = const_cast<QAbstractItemModel*>(index.model());
    model->setData(index, key, Qt::UserRole + 92);
    m_iconWaiters[key].push_back(index);
}

void StartupDock::reconcileEntryIconBatch()
{
    if (!m_iconOperation->isBusy() && !m_activeIconKeys.isEmpty())
    {
        // A dropped delivery can become idle before its queued recovery callback.
        for (const auto& key : m_activeIconKeys)
        {
            m_requestedIconKeys.remove(key);
            m_iconWaiters.remove(key);
            m_iconCache.remove(key);
        }
        m_activeIconKeys.clear();
    }
}

void StartupDock::scheduleEntryIconDrain()
{
    if (m_destroying.load() || m_iconDrainScheduled || !m_iconOperation ||
        m_iconOperation->isBusy() || m_pendingIconPaths.isEmpty()) return;
    m_iconDrainScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_iconDrainScheduled = false;
        drainEntryIcons();
    });
}

void StartupDock::drainEntryIcons()
{
    if (m_destroying.load()) return;
    reconcileEntryIconBatch();
    if (m_iconOperation->isBusy() || m_pendingIconPaths.isEmpty()) return;
    using Request = std::vector<std::pair<QString, QString>>;
    using Result = std::vector<std::pair<QString, QImage>>;
    Request paths;
    constexpr std::size_t maximumPathsPerWorker = 16;
    while (!m_pendingIconPaths.isEmpty() && paths.size() < maximumPathsPerWorker)
    {
        auto it = m_pendingIconPaths.begin();
        paths.emplace_back(it.key(), it.value());
        m_pendingIconPaths.erase(it);
    }
    QStringList activeKeys;
    for (const auto& path : paths) activeKeys.push_back(path.first);
    m_activeIconKeys = activeKeys;
    m_iconOperation->submit<Request, Result>(std::move(paths),
        [](const Request& request, const ks::ui::AsyncOperationToken& token) {
            Result result;
            const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            struct ComLease { bool owned; ~ComLease() { if (owned) ::CoUninitialize(); } } com{SUCCEEDED(comResult)};
            for (const auto& [key, path] : request)
            {
                if (token.isCanceled()) break;
                SHFILEINFOW info{};
                const DWORD_PTR found = ::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), 0,
                    &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON);
                struct IconLease { HICON icon; ~IconLease() { if (icon) ::DestroyIcon(icon); } } nativeIcon{info.hIcon};
                QImage image;
                if (found != 0 && nativeIcon.icon) image = QImage::fromHICON(nativeIcon.icon);
                result.emplace_back(key, std::move(image));
            }
            return result;
        },
        [this](const Result& result, std::uint64_t) {
            for (const auto& [key, image] : result)
            {
                if (!image.isNull()) m_iconCache.insert(key, QIcon(QPixmap::fromImage(image)));
                const QIcon icon = m_iconCache.value(key);
                const auto waiters = m_iconWaiters.take(key);
                for (const auto& index : waiters)
                    if (index.isValid() && index.data(Qt::UserRole + 92).toString() == key)
                        const_cast<QAbstractItemModel*>(index.model())->setData(index, icon, Qt::DecorationRole);
                m_requestedIconKeys.remove(key);
            }
            m_activeIconKeys.clear();
            // Cache eviction affects only icons; the complete startup snapshot is never capped.
            constexpr qsizetype maximumCachedPaths = 2048;
            for (auto it = m_iconCache.begin(); it != m_iconCache.end() && m_iconCache.size() > maximumCachedPaths;)
                if (!m_requestedIconKeys.contains(it.key())) it = m_iconCache.erase(it);
                else ++it;
            return true;
        },
        [this, activeKeys](std::exception_ptr) {
            // Unresolved rows retain their category fallback; a new projection can retry these paths.
            for (const auto& key : activeKeys)
            {
                m_requestedIconKeys.remove(key);
                m_iconWaiters.remove(key);
                m_iconCache.remove(key);
            }
            m_activeIconKeys.clear();
        });
}

void StartupDock::requestAsyncRefresh(const bool forceRefresh)
{
    if (m_refreshInProgress)
    {
        if (forceRefresh)
        {
            m_refreshQueued = true;
        }
        if (m_statusLabel != nullptr)
        {
            m_statusLabel->setText(refreshStatusText(
                startupText(
                    "startup.status.refresh_queued",
                    QStringLiteral("状态：后台刷新进行中，已记录新的刷新请求"))));
        }
        return;
    }

    auto session = std::make_shared<StartupRefreshSession>();
    auto lease = ks::ui::AsyncCleanupRegistry::global().acquire([session]() { session->stopping.store(true); });
    if (!lease)
    {
        m_refreshQueued = false;
        if (m_statusLabel) m_statusLabel->setText(refreshStatusText(ks::i18n::sourceText(QStringLiteral("状态：宿主正在关闭，启动项刷新未提交。"))));
        return;
    }
    m_refreshSession = session;
    m_currentRefreshSucceeded = false;
    m_refreshInProgress = true;
    m_refreshQueued = false;
    m_pendingRefreshStageResults.clear();
    m_backendEnumerationCompleted = false;
    m_refreshSnapshotStarted = false;
    m_activeRefreshStageIndex = 0;
    m_activeRefreshStageCount = 0;
    m_activeRefreshStageEntryCount = 0;
    m_appliedRefreshStageCount = 0;
    m_progressPid = kPro.add(
        this,
        startupText("startup.progress.title", QStringLiteral("启动项")).toStdString(),
        startupText("startup.progress.enumerate", QStringLiteral("枚举自启动项")).toStdString());
    kPro.set(
        m_progressPid,
        startupText("startup.progress.prepare_logon", QStringLiteral("准备枚举登录项")).toStdString(),
        0,
        3.0f);
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(refreshStatusText(
            startupText("startup.status.refreshing", QStringLiteral("状态：后台正在枚举启动项..."))));
    }

    const int progressPid = m_progressPid;
    // LanguageManager 的当前语言由 UI 线程切换。先在此处取得进度文案，
    // 避免后台枚举线程与语言切换并发访问翻译状态。
    const std::array<std::string, 9> enumerationProgressTexts{
        startupText("startup.progress.enumerate_logon", QStringLiteral("正在枚举登录项")).toStdString(),
        startupText("startup.progress.enumerate_services", QStringLiteral("正在枚举服务启动项")).toStdString(),
        startupText("startup.progress.enumerate_drivers", QStringLiteral("正在枚举驱动启动项")).toStdString(),
        startupText("startup.progress.enumerate_tasks", QStringLiteral("正在枚举计划任务")).toStdString(),
        startupText("startup.progress.enumerate_image_hijacks", QStringLiteral("正在检查映像劫持项")).toStdString(),
        startupText("startup.progress.enumerate_registry", QStringLiteral("正在枚举高级注册表启动项")).toStdString(),
        startupText("startup.progress.enumerate_winsock", QStringLiteral("正在枚举 Winsock 启动项")).toStdString(),
        startupText("startup.progress.enumerate_wmi", QStringLiteral("正在枚举 WMI 持久化项")).toStdString(),
        startupText("startup.progress.enumerate_hidden", QStringLiteral("正在交叉检查隐藏启动项")).toStdString()
    };
    const std::string backendCompletedProgressText = startupText(
        "startup.progress.backend_completed",
        QStringLiteral("ks::startup 后端枚举完成")).toStdString();
    struct Request {
        std::shared_ptr<StartupRefreshSession> session;
        std::shared_ptr<ks::ui::AsyncCleanupRegistry::Lease> lease;
        int progressPid;
        std::array<std::string, 9> progressTexts;
        std::string completedText;
    } request{session, std::move(lease), progressPid, enumerationProgressTexts, backendCompletedProgressText};
    struct Result {};
    const std::string stoppedProgressText = ks::i18n::sourceText(QStringLiteral("启动项刷新已停止")).toStdString();
    const auto ticket = m_refreshOperation->submit<Request, Result>(std::move(request),
        [](const Request& input, const ks::ui::AsyncOperationToken& token) {
            const auto shouldStop = [&]() { return token.isCanceled() || input.session->stopping.load(); };
            try
            {
                (void)ks::startup::EnumerateAllStartupEntries(
                    [&input](const ks::startup::StartupEnumerationStage, const std::size_t stageIndex, const std::size_t stageCount) {
                        constexpr std::array<float, 9> values{5.0f, 13.0f, 21.0f, 29.0f, 44.0f, 52.0f, 63.0f, 70.0f, 80.0f};
                        if (stageIndex < stageCount && stageIndex < values.size()) kPro.set(input.progressPid, input.progressTexts[stageIndex], 0, values[stageIndex]);
                    },
                    [&input, &shouldStop](const ks::startup::StartupEnumerationStage, const std::size_t stageIndex,
                        const std::size_t stageCount, const std::vector<ks::startup::StartupEntry>& backendEntries) {
                        if (shouldStop()) return;
                        RefreshStageResult stage;
                        stage.stageIndex = stageIndex;
                        stage.stageCount = stageCount;
                        appendBackendStartupEntries(&stage.entryList, backendEntries);
                        std::sort(stage.entryList.begin(), stage.entryList.end(), [](const StartupEntry& left, const StartupEntry& right) {
                            if (left.category != right.category) return static_cast<int>(left.category) < static_cast<int>(right.category);
                            const int name = left.itemNameText.compare(right.itemNameText, Qt::CaseInsensitive);
                            return name == 0 ? left.locationText.compare(right.locationText, Qt::CaseInsensitive) < 0 : name < 0;
                        });
                        if (shouldStop()) return;
                        std::lock_guard<std::mutex> lock(input.session->mutex);
                        input.session->pending.push_back(std::move(stage));
                    }, shouldStop);
                const bool stopped = shouldStop();
                if (!stopped) kPro.set(input.progressPid, input.completedText, 0, 93.0f);
                std::lock_guard<std::mutex> lock(input.session->mutex);
                input.session->state = stopped ? StartupRefreshSession::State::Stopped : StartupRefreshSession::State::Complete;
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(input.session->mutex);
                input.session->state = StartupRefreshSession::State::Failed;
            }
            return Result{};
        },
        [this](const Result&, std::uint64_t) { drainRefreshSession(); return true; },
        [this, session](std::exception_ptr) {
            { std::lock_guard<std::mutex> lock(session->mutex); session->state = StartupRefreshSession::State::Failed; }
            drainRefreshSession();
        },
        [progressPid, stoppedProgressText](const ks::ui::AsyncOperationOutcome outcome) {
            if (outcome == ks::ui::AsyncOperationOutcome::OwnerClosed || outcome == ks::ui::AsyncOperationOutcome::Canceled)
                kPro.set(progressPid, stoppedProgressText, 0, 100.0f);
        });
    if (!ticket) { std::lock_guard<std::mutex> lock(session->mutex); session->state = StartupRefreshSession::State::Failed; }
    m_refreshDrainTimer->start(0);
}

QString StartupDock::refreshStatusText(const QString& status) const
{
    if (m_refreshTerminalState == RefreshTerminalState::None) return status;
    const QString terminal = ks::i18n::sourceText(m_refreshTerminalState == RefreshTerminalState::Stopped
        ? QStringLiteral("状态：启动项枚举已停止，已显示结果可能不完整。")
        : QStringLiteral("状态：启动项枚举失败，已显示结果可能不完整，请重试。"));
    return terminal + QStringLiteral(" | ") + status;
}

QString StartupDock::refreshSummaryText() const
{
    return refreshStatusText(startupText("startup.status.summary", QStringLiteral("状态：共 %1 条，当前分类 %2"))
        .arg(m_entryList.size()).arg(categoryToText(currentCategory())));
}

void StartupDock::drainRefreshSession()
{
    if (m_destroying.load() || !m_refreshSession || !m_refreshInProgress) return;
    const auto session = m_refreshSession;
    RefreshStageResult stage;
    bool hasStage = false;
    StartupRefreshSession::State state;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->pending.empty()) { stage = std::move(session->pending.front()); session->pending.pop_front(); hasStage = true; }
        state = session->state;
    }
    if (hasStage)
    {
        const QPointer<StartupDock> guarded(this);
        enqueueRefreshStageResult(stage.stageIndex, stage.stageCount, std::move(stage.entryList));
        if (guarded) m_refreshDrainTimer->start(0);
        return;
    }
    if (state == StartupRefreshSession::State::Running)
    {
        m_refreshDrainTimer->start(16);
        return;
    }
    m_currentRefreshSucceeded = state == StartupRefreshSession::State::Complete;
    if (!m_currentRefreshSucceeded)
        m_refreshTerminalState = state == StartupRefreshSession::State::Stopped ? RefreshTerminalState::Stopped : RefreshTerminalState::Failed;
    m_refreshSession.reset();
    markBackendEnumerationCompleted();
}


void StartupDock::enqueueRefreshStageResult(
    const std::size_t stageIndex,
    const std::size_t stageCount,
    std::vector<StartupEntry> entryList)
{
    if (m_destroying.load()
        || !m_refreshInProgress.load()
        || stageCount == 0U
        || stageIndex >= stageCount)
    {
        return;
    }

    RefreshStageResult stageResult;
    stageResult.stageIndex = stageIndex;
    stageResult.stageCount = stageCount;
    stageResult.entryList = std::move(entryList);
    m_pendingRefreshStageResults.push_back(std::move(stageResult));
    processNextRefreshStageResult();
}

void StartupDock::processNextRefreshStageResult()
{
    if (m_destroying.load()
        || !m_refreshInProgress.load()
        || m_tableRebuildInProgress)
    {
        return;
    }

    if (m_pendingRefreshStageResults.empty())
    {
        if (m_backendEnumerationCompleted)
        {
            completeRefreshAfterUiCommit();
        }
        return;
    }

    const QList<QAbstractItemView*> startupViews = {
        m_allTable,
        m_logonTable,
        m_servicesTable,
        m_driversTable,
        m_tasksTable,
        m_imageHijackTable,
        m_wmiTable,
        m_hiddenTable,
        m_registryTree
    };
    if (ks::ui::IsItemViewUiCommitBlockedByContextMenu(startupViews))
    {
        // 八张分类表和注册表树共享同一 m_entryList 索引。菜单关闭后才能追加下一批，
        // 避免当前菜单持有的行索引在共享缓存切换时失效。
        const QPointer<StartupDock> safeThis(this);
        ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
            this,
            QStringLiteral("startup-tables-stage-result-apply"),
            startupViews,
            [safeThis]()
            {
                if (!safeThis.isNull())
                {
                    safeThis->processNextRefreshStageResult();
                }
            });
        return;
    }

    RefreshStageResult stageResult = std::move(m_pendingRefreshStageResults.front());
    m_pendingRefreshStageResults.pop_front();

    // 首批结果到达时才切换离开旧快照；之后每个后端阶段都只向累计结果追加。
    // 每批落表完成前不处理下一批，保证用户看到的顺序与后端枚举顺序一致。
    if (!m_refreshSnapshotStarted)
    {
        m_entryList.clear();
        m_refreshSnapshotStarted = true;
    }
    const std::size_t stageEntryCount = stageResult.entryList.size();
    m_entryList.reserve(m_entryList.size() + stageEntryCount);
    for (StartupEntry& entry : stageResult.entryList)
    {
        m_entryList.push_back(std::move(entry));
    }

    m_activeRefreshStageIndex = stageResult.stageIndex;
    m_activeRefreshStageCount = stageResult.stageCount;
    m_activeRefreshStageEntryCount = stageEntryCount;
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(refreshStatusText(
            startupText(
                "startup.status.applying_stage_results",
                QStringLiteral("状态：正在添加第 %1/%2 阶段结果，本阶段 %3 条..."))
                .arg(m_activeRefreshStageIndex + 1U)
                .arg(m_activeRefreshStageCount)
                .arg(stageEntryCount)));
    }
    rebuildAllTables(true);
}

void StartupDock::markBackendEnumerationCompleted()
{
    if (m_destroying.load() || !m_refreshInProgress.load())
    {
        return;
    }

    m_backendEnumerationCompleted = true;
    processNextRefreshStageResult();
}

void StartupDock::completeRefreshAfterUiCommit()
{
    if (m_destroying.load()
        || !m_refreshInProgress.load()
        || !m_backendEnumerationCompleted
        || !m_pendingRefreshStageResults.empty()
        || m_tableRebuildInProgress)
    {
        return;
    }

    if (m_currentRefreshSucceeded) m_refreshTerminalState = RefreshTerminalState::None;
    if (m_statusLabel != nullptr) m_statusLabel->setText(refreshSummaryText());

    const int completedProgressPid = m_progressPid;
    m_progressPid = 0;
    if (completedProgressPid != 0)
    {
        kPro.set(
            completedProgressPid,
            (m_refreshTerminalState == RefreshTerminalState::None ? startupText("startup.progress.completed", QStringLiteral("启动项刷新完成")) : refreshSummaryText()).toStdString(),
            0,
            100.0f);
    }

    m_refreshInProgress = false;

    kLogEvent refreshEvent;
    if (m_refreshTerminalState == RefreshTerminalState::None)
        info << refreshEvent
            << startupText("startup.log.refresh.completed", QStringLiteral("[StartupDock] 后台刷新完成, count=")).toStdString()
            << m_entryList.size() << eol;
    else
        err << refreshEvent << refreshSummaryText().toStdString() << eol;

    if (m_refreshQueued)
    {
        requestAsyncRefresh(false);
    }
}
