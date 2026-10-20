#include "ServiceDock.Internal.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/AsyncOperation.h"
#include "../UI/AsyncCleanupRegistry.h"
#include "../UI/TableRowFilterSupport.h"
#include "../Internationalization/LanguageManager.h"
#include <QHideEvent>

using namespace service_dock_detail;

ServiceDock::ServiceDock(QWidget* parent)
    : QWidget(parent)
{
    m_refreshOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_refreshOperation->setStateChangedCallback([this]() { reconcileRefreshState(); });
    m_exportOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_exportOperation->setStateChangedCallback([this]() { reconcileExportReceipt(); });
    m_detailOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_detailOperation->setStateChangedCallback([this]() { reconcileDetailReceipt(); });
    initializeUi();
    initializeConnections();
    syncToolbarStateWithSelection();

    kLogEvent initEvent;
    info << initEvent << "[ServiceDock] 服务管理页面初始化完成。" << eol;
}

ServiceDock::~ServiceDock()
{
    m_detailOperation->close();
    m_exportOperation->close();
    m_refreshOperation->close();

    kLogEvent destroyEvent;
    info << destroyEvent << "[ServiceDock] 服务管理页面已析构。" << eol;
}

void ServiceDock::reconcileExportReceipt()
{
    if (m_exportPendingReceipt && !m_exportOperation->isInFlight())
    {
        m_exportPendingReceipt = false;
        QMessageBox::warning(this, QStringLiteral("服务管理"), ks::i18n::sourceText(QStringLiteral("导出未返回有效回执，请检查目标文件后再重试。")));
    }
}

void ServiceDock::showEvent(QShowEvent* event)
{
    if (m_serviceRowsDirty) rebuildServiceTable();
    QWidget::showEvent(event);
    requestCurrentDetail();
    if (!m_pendingFocusServiceName.isEmpty())
    {
        const QString pending = m_pendingFocusServiceName;
        m_pendingFocusServiceName.clear();
        focusServiceByName(pending);
    }

    if (m_initialRefreshDone)
    {
        return;
    }

    m_initialRefreshDone = true;
    if (m_summaryLabel != nullptr)
    {
        m_summaryLabel->setText(QStringLiteral("状态：首次打开，准备加载服务列表..."));
    }

    // 延后到事件循环再触发首轮刷新，保证页面先可见后加载。
    QTimer::singleShot(0, this, [this]()
        {
            requestAsyncRefresh(true);
        });
}

void ServiceDock::focusServiceByName(const QString& serviceNameText)
{
    const QString targetServiceName = serviceNameText.trimmed();
    if (targetServiceName.isEmpty())
    {
        return;
    }

    m_pendingFocusServiceName = targetServiceName;

    for (int rowIndex = 0; rowIndex < m_serviceTable->rowCount(); ++rowIndex)
    {
        QTableWidgetItem* nameItem = m_serviceTable->item(rowIndex, toServiceColumn(ServiceColumn::Name));
        if (ks::ui::IsTableRowExcludedByFilter(m_serviceTable, rowIndex) || nameItem == nullptr)
        {
            continue;
        }

        const QString rowServiceName = nameItem->data(service_dock_detail::kServiceNameRole).toString();
        if (QString::compare(rowServiceName, targetServiceName, Qt::CaseInsensitive) == 0)
        {
            m_serviceTable->selectRow(rowIndex);
            m_serviceTable->scrollToItem(nameItem, QAbstractItemView::PositionAtCenter);
            m_pendingFocusServiceName.clear();
            return;
        }
    }

    if (m_visibleServiceCount == 0 || m_refreshInProgress)
    {
        requestAsyncRefresh(true);
    }
}

void ServiceDock::reconcileRefreshState()
{
    if (m_refreshInProgress && !m_refreshOperation->isInFlight())
    {
        m_refreshInProgress = false;
        if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("状态：服务刷新未返回有效结果，保留上次列表，请重试。")));
        if (m_refreshQueued) QTimer::singleShot(0, this, [this]() {
            if (!m_refreshInProgress && m_refreshQueued) requestAsyncRefresh(false);
        });
    }
}

void ServiceDock::requestAsyncRefresh(const bool forceRefresh)
{
    reconcileRefreshState();
    if (m_refreshInProgress)
    {
        if (forceRefresh) m_refreshQueued = true;
        if (m_summaryLabel) m_summaryLabel->setText(QStringLiteral("状态：服务刷新进行中，已排队新的请求"));
        return;
    }
    struct Request {
        int progressPid;
        std::shared_ptr<std::atomic_bool> stopping;
        std::shared_ptr<ks::ui::AsyncCleanupRegistry::Lease> lease;
        QString readProgress, sortProgress;
    } request;
    request.stopping = std::make_shared<std::atomic_bool>(false);
    request.lease = ks::ui::AsyncCleanupRegistry::global().acquire([flag = request.stopping]() { flag->store(true); });
    if (!request.lease)
    {
        m_refreshQueued = false;
        if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("状态：宿主正在关闭，服务刷新未提交。")));
        return;
    }
    m_refreshInProgress = true;
    m_refreshQueued = false;
    m_progressPid = kPro.add(this, "服务管理", "枚举服务列表");
    kPro.set(m_progressPid, "打开服务控制管理器", 0, 10.0f);
    if (m_summaryLabel) m_summaryLabel->setText(QStringLiteral("状态：后台正在枚举服务..."));
    request.progressPid = m_progressPid;
    request.readProgress = ks::i18n::sourceText(QStringLiteral("读取 Win32 服务列表"));
    request.sortProgress = ks::i18n::sourceText(QStringLiteral("整理服务列表数据"));
    const kLogEvent refreshEvent;
    info << refreshEvent << "[ServiceDock] 开始后台刷新服务列表。" << eol;
    const QString stoppedProgress = ks::i18n::sourceText(QStringLiteral("服务刷新已停止"));
    const QString droppedProgress = ks::i18n::sourceText(QStringLiteral("服务刷新未返回有效结果"));
    const auto ticket = m_refreshOperation->submit<Request, ServiceRefreshResult>(std::move(request),
        [](const Request& input, const ks::ui::AsyncOperationToken& token) {
            ServiceRefreshResult result;
            result.progressPid = input.progressPid;
            result.rows = std::make_shared<std::vector<ServiceEntry>>();
            const auto shouldStop = [&]() { return token.isCanceled() || input.stopping->load(); };
            if (shouldStop()) { result.stopped = true; return result; }
            kPro.set(input.progressPid, input.readProgress.toUtf8().constData(), 0, 45.0f);
            enumerateServiceList(result.rows.get(), &result.errorText, shouldStop);
            if (shouldStop()) { result.stopped = true; return result; }
            result.success = result.errorText.trimmed().isEmpty();
            kPro.set(input.progressPid, input.sortProgress.toUtf8().constData(), 0, 88.0f);
            std::sort(result.rows->begin(), result.rows->end(), [](const ServiceEntry& left, const ServiceEntry& right) {
                const int display = QString::compare(left.displayNameText, right.displayNameText, Qt::CaseInsensitive);
                return display == 0 ? QString::compare(left.serviceNameText, right.serviceNameText, Qt::CaseInsensitive) < 0 : display < 0;
            });
            if (shouldStop()) result.stopped = true;
            return result;
        },
        [this, refreshEvent](const ServiceRefreshResult& result, const std::uint64_t generation) {
            const bool applied = applyRefreshResult(result, generation);
            if (result.stopped) return applied;
            if (result.success) info << refreshEvent << "[ServiceDock] 后台刷新服务列表成功。" << eol;
            else err << refreshEvent << "[ServiceDock] 后台刷新服务列表失败, error=" << result.errorText.toStdString() << eol;
            return applied;
        },
        [this](std::exception_ptr) {
            const bool queued = m_refreshQueued;
            m_refreshInProgress = false;
            m_refreshQueued = queued;
            if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("状态：服务刷新未返回有效结果，保留上次列表，请重试。")));
            if (queued) QTimer::singleShot(0, this, [this]() { if (!m_refreshInProgress && m_refreshQueued) requestAsyncRefresh(false); });
        },
        [pid = m_progressPid, stoppedProgress, droppedProgress](const ks::ui::AsyncOperationOutcome outcome) {
            if (outcome != ks::ui::AsyncOperationOutcome::Applied)
                kPro.set(pid, (outcome == ks::ui::AsyncOperationOutcome::DeliveryDropped ? droppedProgress : stoppedProgress).toUtf8().constData(), 0, 100.0f);
        });
    if (!ticket) reconcileRefreshState();
}

bool ServiceDock::applyRefreshResult(const ServiceRefreshResult& result, const std::uint64_t ticket)
{
    if (!m_refreshOperation->isCurrent(ticket)) return true;
    if (result.stopped)
    {
        m_refreshInProgress = false;
        m_refreshQueued = false;
        kPro.set(result.progressPid, ks::i18n::sourceText(QStringLiteral("服务刷新已停止")).toUtf8().constData(), 0, 100.0f);
        if (m_summaryLabel) m_summaryLabel->setText(ks::i18n::sourceText(QStringLiteral("服务刷新已停止")));
        return true;
    }
    if (ks::ui::IsTableUiCommitBlockedByContextMenu({m_serviceTable}))
    {
        const QPointer<ServiceDock> safeThis(this);
        ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("service-table-refresh-apply"), {m_serviceTable},
            [safeThis, result, ticket]() {
                if (!safeThis || !safeThis->m_refreshOperation->isCurrent(ticket)) return;
                if (safeThis->applyRefreshResult(result, ticket) && safeThis) safeThis->m_refreshOperation->completeDeferred(ticket);
            });
        return false;
    }
    const QPointer<ServiceDock> guarded(this);
    const QString& errorText = result.errorText;
    const bool success = result.success;
    m_serviceList = std::move(*result.rows);
    m_detailServiceStates.clear();
    m_serviceIndexByName.clear();
    m_serviceIndexByName.reserve(static_cast<qsizetype>(m_serviceList.size()));
    for (int index = 0; index < static_cast<int>(m_serviceList.size()); ++index)
    {
        const QString key = m_serviceList[static_cast<std::size_t>(index)].serviceNameText.toCaseFolded();
        if (!m_serviceIndexByName.contains(key)) m_serviceIndexByName.insert(key, index);
        m_detailServiceStates.insert(key, m_serviceList[static_cast<std::size_t>(index)].currentState);
    }
    rebuildServiceTable();

    if (isVisible() && !m_pendingFocusServiceName.trimmed().isEmpty())
    {
        const QString pendingNameText = m_pendingFocusServiceName;
        m_pendingFocusServiceName.clear();
        focusServiceByName(pendingNameText);
    }

    if (success)
    {
        if (m_summaryLabel != nullptr)
        {
            m_summaryLabel->setText(QStringLiteral("状态：服务刷新完成，共 %1 条").arg(m_serviceList.size()));
        }
        if (m_progressPid != 0)
        {
            kPro.set(m_progressPid, "服务刷新完成", 0, 100.0f);
        }
    }
    else
    {
        if (m_summaryLabel != nullptr)
        {
            m_summaryLabel->setText(QStringLiteral("状态：刷新失败，%1").arg(errorText));
        }
        if (m_progressPid != 0)
        {
            kPro.set(m_progressPid, "服务刷新失败", 0, 100.0f);
        }
        QMessageBox::warning(
            this,
            QStringLiteral("服务管理"),
            QStringLiteral("服务列表刷新失败：\n%1").arg(errorText));
        if (!guarded) return true;
    }

    m_refreshInProgress = false;

    if (m_refreshQueued)
    {
        requestAsyncRefresh(false);
    }
    return true;
}

void ServiceDock::applyServiceUpdateToCache(const ServiceEntry& updatedEntry)
{
    const int entryIndex = findServiceIndexByName(updatedEntry.serviceNameText);
    if (entryIndex >= 0 && entryIndex < static_cast<int>(m_serviceList.size()))
    {
        m_serviceList[static_cast<std::size_t>(entryIndex)] = updatedEntry;
        m_detailServiceStates.insert(updatedEntry.serviceNameText.toCaseFolded(), updatedEntry.currentState);
    }
}

void ServiceDock::hideEvent(QHideEvent* event)
{
    m_detailPendingReceipt = false;
    m_detailOperation->cancel();
    QWidget::hideEvent(event);
}
