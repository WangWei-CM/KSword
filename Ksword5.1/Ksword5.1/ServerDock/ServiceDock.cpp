#include "ServiceDock.Internal.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TableRowFilterSupport.h"
#include "../Internationalization/LanguageManager.h"

using namespace service_dock_detail;

ServiceDock::ServiceDock(QWidget* parent)
    : QWidget(parent)
{
    m_exportOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    m_exportOperation->setStateChangedCallback([this]() { reconcileExportReceipt(); });
    initializeUi();
    initializeConnections();
    syncToolbarStateWithSelection();

    kLogEvent initEvent;
    info << initEvent << "[ServiceDock] 服务管理页面初始化完成。" << eol;
}

ServiceDock::~ServiceDock()
{
    m_exportOperation->close();
    if (m_refreshThread != nullptr && m_refreshThread->joinable())
    {
        m_refreshThread->join();
    }

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

void ServiceDock::requestAsyncRefresh(const bool forceRefresh)
{
    if (m_refreshInProgress)
    {
        if (forceRefresh)
        {
            m_refreshQueued = true;
        }
        if (m_summaryLabel != nullptr)
        {
            m_summaryLabel->setText(QStringLiteral("状态：服务刷新进行中，已排队新的请求"));
        }
        return;
    }

    if (m_refreshThread != nullptr && m_refreshThread->joinable())
    {
        m_refreshThread->join();
    }

    m_refreshInProgress = true;
    m_refreshQueued = false;
    m_progressPid = kPro.add(this, "服务管理", "枚举服务列表");
    kPro.set(m_progressPid, "打开服务控制管理器", 0, 10.0f);
    if (m_summaryLabel != nullptr)
    {
        m_summaryLabel->setText(QStringLiteral("状态：后台正在枚举服务..."));
    }

    // 整个刷新链路统一使用一个 kLogEvent，便于后续按 GUID 跟踪完整过程。
    const kLogEvent refreshEvent;
    info << refreshEvent << "[ServiceDock] 开始后台刷新服务列表。" << eol;

    const QPointer<ServiceDock> safeThis(this);
    m_refreshThread = std::make_unique<std::thread>([safeThis, refreshEvent]()
        {
            if (safeThis.isNull())
            {
                return;
            }

            std::vector<ServiceEntry> serviceList;
            serviceList.reserve(512);
            QString errorText;
            bool success = true;

            kPro.set(safeThis->m_progressPid, "读取 Win32 服务列表", 0, 45.0f);
            safeThis->enumerateServiceList(&serviceList, &errorText);
            if (!errorText.trimmed().isEmpty())
            {
                success = false;
            }

            kPro.set(safeThis->m_progressPid, "整理服务列表数据", 0, 88.0f);
            if (safeThis.isNull())
            {
                return;
            }

            QMetaObject::invokeMethod(
                safeThis,
                [safeThis, refreshEvent, serviceList = std::move(serviceList), errorText, success]() mutable
                {
                    if (safeThis.isNull())
                    {
                        return;
                    }

                    safeThis->applyRefreshResult(std::move(serviceList), errorText, success);
                    if (success)
                    {
                        info << refreshEvent << "[ServiceDock] 后台刷新服务列表成功。" << eol;
                    }
                    else
                    {
                        err << refreshEvent
                            << "[ServiceDock] 后台刷新服务列表失败, error="
                            << errorText.toStdString()
                            << eol;
                    }
                },
                Qt::QueuedConnection);
        });
}

void ServiceDock::applyRefreshResult(
    std::vector<ServiceEntry> serviceList,
    const QString& errorText,
    const bool success)
{
    if (ks::ui::IsTableUiCommitBlockedByContextMenu({m_serviceTable}))
    {
        const QPointer<ServiceDock> safeThis(this);
        ks::ui::DeferTableUiCommitIfContextMenuOpen(
            this,
            QStringLiteral("service-table-refresh-apply"),
            {m_serviceTable},
            [safeThis,
                serviceList = std::move(serviceList),
                errorText,
                success]() mutable
            {
                if (!safeThis.isNull())
                {
                    safeThis->applyRefreshResult(
                        std::move(serviceList),
                        errorText,
                        success);
                }
            });
        return;
    }

    std::sort(
        serviceList.begin(),
        serviceList.end(),
        [](const ServiceEntry& left, const ServiceEntry& right)
        {
            const int displayCompareResult = QString::compare(
                left.displayNameText,
                right.displayNameText,
                Qt::CaseInsensitive);
            if (displayCompareResult != 0)
            {
                return displayCompareResult < 0;
            }
            return QString::compare(left.serviceNameText, right.serviceNameText, Qt::CaseInsensitive) < 0;
        });

    m_serviceList = std::move(serviceList);
    m_serviceIndexByName.clear();
    m_serviceIndexByName.reserve(static_cast<qsizetype>(m_serviceList.size()));
    for (int index = 0; index < static_cast<int>(m_serviceList.size()); ++index)
    {
        const QString key = m_serviceList[static_cast<std::size_t>(index)].serviceNameText.toCaseFolded();
        if (!m_serviceIndexByName.contains(key)) m_serviceIndexByName.insert(key, index);
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
    }

    m_refreshInProgress = false;

    if (m_refreshQueued)
    {
        requestAsyncRefresh(false);
    }
}

void ServiceDock::applyServiceUpdateToCache(const ServiceEntry& updatedEntry)
{
    const int entryIndex = findServiceIndexByName(updatedEntry.serviceNameText);
    if (entryIndex >= 0 && entryIndex < static_cast<int>(m_serviceList.size()))
    {
        m_serviceList[static_cast<std::size_t>(entryIndex)] = updatedEntry;
    }
}
