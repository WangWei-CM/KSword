#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

StartupDock::StartupDock(QWidget* parent)
    : QWidget(parent)
{
    // 初始化顺序：
    // - 先创建 UI；
    // - 再连接交互；
    // - 启动项全量枚举改为“首次显示时懒加载”，避免拖慢主窗口启动。
    initializeUi();
    initializeConnections();

    kLogEvent initEvent;
    info << initEvent << "[StartupDock] 启动项页初始化完成。" << eol;
}

StartupDock::~StartupDock()
{
    if (m_refreshThread != nullptr && m_refreshThread->joinable())
    {
        m_refreshThread->join();
    }
    if (m_refreshTimer != nullptr)
    {
        m_refreshTimer->stop();
    }

    kLogEvent destroyEvent;
    info << destroyEvent << "[StartupDock] 启动项页已析构。" << eol;
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
        return QStringLiteral("总览");
    case StartupCategory::Logon:
        return QStringLiteral("登录");
    case StartupCategory::Services:
        return QStringLiteral("服务");
    case StartupCategory::Drivers:
        return QStringLiteral("驱动");
    case StartupCategory::Tasks:
        return QStringLiteral("计划任务");
    case StartupCategory::Registry:
        return QStringLiteral("高级注册表");
    case StartupCategory::Wmi:
        return QStringLiteral("WMI");
    default:
        return QStringLiteral("未知");
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
        m_statusLabel->setText(QStringLiteral("状态：首次打开，正在加载启动项..."));
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
        return StartupCategory::Registry;
    case 6:
        return StartupCategory::Wmi;
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
    case StartupCategory::Registry:
        return nullptr;
    case StartupCategory::Wmi:
        return m_wmiTable;
    default:
        return m_allTable;
    }
}

QIcon StartupDock::resolveEntryIcon(const StartupEntry& entry)
{
    // 图标解析留在 UI 线程：
    // - 避免后台线程构造 QIcon / QFileIconProvider；
    // - 同一路径走缓存，降低表格重建开销。
    const QString cacheKeyText =
        entry.imagePathText.trimmed().isEmpty()
        ? QStringLiteral("type:%1").arg(entry.sourceTypeText)
        : QStringLiteral("path:%1").arg(QDir::toNativeSeparators(entry.imagePathText));

    const auto cacheIt = m_iconCache.constFind(cacheKeyText);
    if (cacheIt != m_iconCache.constEnd())
    {
        return cacheIt.value();
    }

    QIcon resolvedIcon;
    if (!entry.imagePathText.trimmed().isEmpty())
    {
        const QFileInfo fileInfo(entry.imagePathText);
        if (fileInfo.exists())
        {
            static QFileIconProvider fileIconProvider;
            resolvedIcon = fileIconProvider.icon(fileInfo);
        }
    }

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
        else if (entry.category == StartupCategory::Registry)
        {
            resolvedIcon = createBlueIcon(":/Icon/file_find.svg");
        }
        else
        {
            resolvedIcon = createBlueIcon(":/Icon/process_main.svg");
        }
    }

    m_iconCache.insert(cacheKeyText, resolvedIcon);
    return resolvedIcon;
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
            m_statusLabel->setText(QStringLiteral("状态：后台刷新进行中，已记录新的刷新请求"));
        }
        return;
    }

    if (m_refreshThread != nullptr && m_refreshThread->joinable())
    {
        m_refreshThread->join();
    }

    m_refreshInProgress = true;
    m_refreshQueued = false;
    m_progressPid = kPro.add("启动项", "枚举自启动项");
    kPro.set(m_progressPid, "准备枚举登录项", 0, 5.0f);
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(QStringLiteral("状态：后台正在枚举启动项..."));
    }

    const QPointer<StartupDock> safeThis(this);
    m_refreshThread = std::make_unique<std::thread>([safeThis]()
        {
            if (safeThis.isNull())
            {
                return;
            }

            std::vector<StartupEntry> entryList;
            entryList.reserve(256);

            kPro.set(safeThis->m_progressPid, "枚举登录项", 0, 15.0f);
            safeThis->appendLogonEntries(&entryList);

            kPro.set(safeThis->m_progressPid, "枚举服务", 0, 35.0f);
            safeThis->appendServiceEntries(&entryList);

            kPro.set(safeThis->m_progressPid, "枚举驱动", 0, 55.0f);
            safeThis->appendDriverEntries(&entryList);

            kPro.set(safeThis->m_progressPid, "枚举计划任务", 0, 75.0f);
            safeThis->appendTaskEntries(&entryList);

            kPro.set(safeThis->m_progressPid, "枚举高级注册表项", 0, 84.0f);
            safeThis->appendAdvancedRegistryEntries(&entryList);

            kPro.set(safeThis->m_progressPid, "枚举Winsock持久化项", 0, 90.0f);
            safeThis->appendWinsockEntries(&entryList);

            kPro.set(safeThis->m_progressPid, "枚举WMI持久化项", 0, 96.0f);
            safeThis->appendWmiEntries(&entryList);

            if (safeThis.isNull())
            {
                return;
            }

            QMetaObject::invokeMethod(
                safeThis,
                [safeThis, entryList = std::move(entryList)]() mutable
                {
                    if (safeThis.isNull())
                    {
                        return;
                    }
                    safeThis->applyRefreshResult(std::move(entryList));
                },
                Qt::QueuedConnection);
        });
}

void StartupDock::applyRefreshResult(std::vector<StartupEntry> entryList)
{
    std::sort(
        entryList.begin(),
        entryList.end(),
        [](const StartupEntry& left, const StartupEntry& right)
        {
            if (left.category != right.category)
            {
                return static_cast<int>(left.category) < static_cast<int>(right.category);
            }
            if (left.itemNameText.compare(right.itemNameText, Qt::CaseInsensitive) != 0)
            {
                return left.itemNameText.compare(right.itemNameText, Qt::CaseInsensitive) < 0;
            }
            return left.locationText.compare(right.locationText, Qt::CaseInsensitive) < 0;
        });

    m_entryList = std::move(entryList);
    rebuildAllTables();

    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(QStringLiteral("状态：共 %1 条，当前分类 %2")
            .arg(m_entryList.size())
            .arg(categoryToText(currentCategory())));
    }

    if (m_progressPid != 0)
    {
        kPro.set(m_progressPid, "启动项刷新完成", 0, 100.0f);
    }

    m_refreshInProgress = false;

    kLogEvent refreshEvent;
    info << refreshEvent << "[StartupDock] 后台刷新完成, count=" << m_entryList.size() << eol;

    if (m_refreshQueued)
    {
        requestAsyncRefresh(false);
    }
}
