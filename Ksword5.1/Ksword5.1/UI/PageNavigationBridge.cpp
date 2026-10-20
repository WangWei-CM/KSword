#include "../Framework.h"
#include "./PageNavigationBridge.h"
#include "./AsyncUiDispatcher.h"
#include "../include/ads/DockWidget.h"
#include <QAbstractItemView>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QPointer>
#include <QQueue>
#include <QRunnable>
#include <QThreadPool>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace ks::ui
{
    struct PageNavigationBridge::State
    {
        struct Route
        {
            QPointer<ads::CDockWidget> dock; // 当前实例实际拥有的一级页面。
            QPointer<QWidget> page;         // 页对象身份；不使用易变化的标题或序号跳转。
            bool pageRequired = false;     // 区分一级入口和已经销毁的子页。
            QString descriptorId;           // 静态工厂局部ID，仅在用户激活时解析真实页面。
        };
        PageNavigationBridge* owner;
        QPointer<QWidget> host;
        DockList list;
        DockAction prepare;
        DockAction nestedPrepare;
        DockAction activate;
        PageNavigationCatalogProvider catalogProvider;
        PageNavigationCatalogNavigator catalogNavigator;
        HWND client = nullptr;
        HWND server = nullptr; // 保存原生句柄，析构时不重新调用宿主 winId 创建窗口。
        QString request;
        QString epoch = QUuid::createUuid().toString(QUuid::WithoutBraces); // 当前Bridge实例身份。
        QString completedRevision;
        QCryptographicHash catalogHash{QCryptographicHash::Sha256};
        QMap<QString, Route> routes;
        QSet<QString> catalogRouteIds; // 只有当前目录代实际登记的稳定ID可路由。
        QList<QPointer<ads::CDockWidget>> pending;
        int nextDock = 0;
        quint64 generation = 0;
        bool collecting = false;

        struct Delivery
        {
            HWND destination = nullptr;
            HWND source = nullptr;
            QByteArray bytes;
            quint64 generation = 0;
        };
        std::shared_ptr<AsyncUiDispatcher> deliveryDispatcher;
        QQueue<Delivery> deliveries;
        bool deliveryInFlight = false;
        bool collectWaitingForDelivery = false;

        static bool sendDelivery(const Delivery& delivery)
        {
            // 此函数仅在worker触碰Win32句柄和值字节，不读取QObject或控件。
            if (!::GetPropW(delivery.destination, page_navigation::ClientProperty)
                || !page_navigation::IsCompanionWindow(delivery.destination, page_navigation::TaskbarFileName)
                || !::GetPropW(delivery.source, page_navigation::ServerProperty)
                || !page_navigation::IsCompanionWindow(delivery.source, page_navigation::MainFileName))
            {
                return false;
            }
            COPYDATASTRUCT packet{page_navigation::ReplyTag, static_cast<DWORD>(delivery.bytes.size()),
                const_cast<char*>(delivery.bytes.constData())};
            DWORD_PTR accepted = 0;
            return ::SendMessageTimeoutW(delivery.destination, WM_COPYDATA,
                reinterpret_cast<WPARAM>(delivery.source), reinterpret_cast<LPARAM>(&packet),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 750, &accepted) != 0 && accepted != 0;
        }

        void finishDelivery(quint64 expected, bool delivered)
        {
            deliveryInFlight = false;
            if (expected == generation && !delivered)
            {
                // 一次拒绝或超时即作废本代；不会继续等待其余目录包。
                collecting = false;
                collectWaitingForDelivery = false;
                deliveries.clear();
                ++generation;
                return;
            }
            pumpDelivery(); // 旧代ACK只释放在途槽，不改变新代收集状态。
            if (!deliveryInFlight && deliveries.isEmpty() && collecting && collectWaitingForDelivery)
            {
                collectWaitingForDelivery = false;
                const quint64 next = generation;
                QTimer::singleShot(0, owner, [this, next]() { collectNext(next); });
            }
        }

        void pumpDelivery()
        {
            if (deliveryInFlight || deliveries.isEmpty())
            {
                return;
            }
            const Delivery delivery = deliveries.dequeue();
            deliveryInFlight = true;
            const auto dispatcher = deliveryDispatcher;
            try
            {
                auto* task = QRunnable::create([delivery, dispatcher, state = this]()
                {
                    bool delivered = false;
                    try
                    {
                        delivered = sendDelivery(delivery);
                    }
                    catch (...)
                    {
                        // 路径身份验证分配失败与传输失败走同一终态，不能越出QRunnable。
                    }
                    dispatcher->post([state, expected = delivery.generation, delivered]()
                    {
                        state->finishDelivery(expected, delivered); // 仅在仍存活的GUI门禁内借用state。
                    });
                });
                QThreadPool::globalInstance()->start(task, -1);
            }
            catch (...)
            {
                finishDelivery(delivery.generation, false);
            }
        }

        // GUI只序列化小包；每次最多一个值化packet在worker中等待接收方。
        void reply(QJsonObject message)
        {
            if (!host)
            {
                collecting = false;
                return;
            }
            message.insert(QStringLiteral("request"), request);
            message.insert(QStringLiteral("version"), 1);
            const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
            if (bytes.size() > page_navigation::MaxMessageBytes)
            {
                collecting = false;
                deliveries.clear();
                collectWaitingForDelivery = false;
                ++generation;
                return;
            }
            deliveries.enqueue(Delivery{client, server, bytes, generation});
            pumpDelivery();
        }

        QString addRoute(ads::CDockWidget* dock, QWidget* page)
        {
            QObject* identity = page != nullptr ? static_cast<QObject*>(page) : static_cast<QObject*>(dock);
            QString id = identity->property("ksword_navigation_route_id").toString();
            const auto previous = routes.constFind(id);
            if (id.isEmpty() || (previous != routes.cend()
                && (previous->dock != dock || previous->page != page)))
            {
                id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                identity->setProperty("ksword_navigation_route_id", id);
            }
            routes.insert(id, Route{dock, page, page != nullptr});
            catalogRouteIds.insert(id);
            return id;
        }

        QString addDescriptorRoute(ads::CDockWidget* dock, const QString& descriptorId)
        {
            const QString dockId = addRoute(dock, nullptr);
            const QString id = QUuid::createUuidV5(QUuid(dockId), descriptorId.toUtf8())
                .toString(QUuid::WithoutBraces);
            routes.insert(id, Route{dock, nullptr, false, descriptorId});
            catalogRouteIds.insert(id);
            return id;
        }

        void removeExpiredRoutes()
        {
            for (auto route = routes.begin(); route != routes.end();)
            {
                if (!route->dock || (route->pageRequired && !route->page))
                {
                    route = routes.erase(route);
                }
                else
                {
                    ++route;
                }
            }
        }

        void hashCatalogPart(const QJsonObject& part)
        {
            // 只签名实际目录内容，不混入请求token；稳定ID允许consumer跨请求复用真实行。
            catalogHash.addData(QJsonDocument(part).toJson(QJsonDocument::Compact));
        }

        // 只收集导航页；表格单元格、弹窗和菜单不进入目录，也不切换页面触发业务动作。
        void collect(QWidget* root, ads::CDockWidget* dock, const QStringList& path,
            QJsonArray& entries, int depth = 0, bool parentEnabled = true)
        {
            if (!root || depth > 20 || entries.size() >= 2048 || qobject_cast<QAbstractItemView*>(root))
            {
                return;
            }
            if (auto* tabs = qobject_cast<QTabWidget*>(root))
            {
                for (int index = 0; index < tabs->count(); ++index)
                {
                    QWidget* page = tabs->widget(index);
                    if (!page || !tabs->isTabVisible(index))
                    {
                        continue;
                    }
                    const QString title = tabs->tabText(index).remove(QLatin1Char('&')).left(256);
                    const QStringList nextPath = path + QStringList{title};
                    const bool enabled = parentEnabled && tabs->isTabEnabled(index);
                    entries.append(QJsonObject{{QStringLiteral("id"), addRoute(dock, page)},
                        {QStringLiteral("title"), title}, {QStringLiteral("path"), nextPath.join(QStringLiteral(" / ")).left(1024)},
                        {QStringLiteral("depth"), static_cast<int>(nextPath.size() - 1)}, {QStringLiteral("enabled"), enabled},
                        {QStringLiteral("current"), dock->isCurrentTab() && page->isVisibleTo(dock)}});
                    collect(page, dock, nextPath, entries, depth + 1, enabled);
                }
                return;
            }
            // 自绘分段页显式给真实 stack 页登记标题，与 QTabWidget 共用目录/激活链。
            if (auto* stack = qobject_cast<QStackedWidget*>(root))
            {
                const QStringList titles = stack->property("ksword_navigation_page_titles").toStringList();
                if (titles.size() == stack->count())
                {
                    for (int index = 0; index < stack->count(); ++index)
                    {
                        QWidget* page = stack->widget(index);
                        const QStringList nextPath = path + QStringList{titles[index]};
                        entries.append(QJsonObject{{QStringLiteral("id"), addRoute(dock, page)},
                            {QStringLiteral("title"), titles[index].left(256)}, {QStringLiteral("path"), nextPath.join(QStringLiteral(" / ")).left(1024)},
                            {QStringLiteral("depth"), static_cast<int>(nextPath.size() - 1)}, {QStringLiteral("enabled"), parentEnabled},
                            {QStringLiteral("current"), dock->isCurrentTab() && page->isVisibleTo(dock)}});
                        collect(page, dock, nextPath, entries, depth + 1, parentEnabled);
                    }
                    return;
                }
            }
            const auto children = root->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
            for (QWidget* child : children)
            {
                // 非当前 stack 页的隐藏属于导航；其它显式隐藏业务面板不应暴露为入口。
                const bool explicitlyHidden = child->isHidden() && child->testAttribute(Qt::WA_WState_ExplicitShowHide);
                if (!child->isWindow() && (!explicitlyHidden || qobject_cast<QStackedWidget*>(root)))
                {
                    collect(child, dock, path, entries, depth + 1, parentEnabled);
                }
            }
        }

        void collectNext(quint64 expected)
        {
            if (!collecting || expected != generation || !host)
            {
                return;
            }
            if (deliveryInFlight || !deliveries.isEmpty())
            {
                // 一列的所有包确认后才构造下一列，队列规模不随整份目录累积。
                collectWaitingForDelivery = true;
                return;
            }
            if (nextDock >= pending.size())
            {
                collecting = false;
                completedRevision = QString::fromLatin1(catalogHash.result().toHex());
                reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("complete")},
                    {QStringLiteral("catalog_instance"), epoch},
                    {QStringLiteral("revision"), completedRevision}});
                return;
            }
            const QPointer<PageNavigationBridge> lifetime(owner);
            const int column = nextDock++;
            QPointer<ads::CDockWidget> dock = pending[column];
            if (dock)
            {
                std::optional<QVector<ResolvedPageNavigationDescriptor>> descriptors;
                if (catalogProvider)
                {
                    const auto provider = catalogProvider;
                    const auto catalog = provider(dock);
                    if (!lifetime || expected != generation || !collecting) return;
                    if (catalog && dock)
                        descriptors = ResolvePageNavigationCatalog(*catalog, dock->windowTitle());
                }
                if (!descriptors)
                {
                    // 未迁移或schema无效时保留真实页面回退，不以漏目录换取冷开速度。
                    if (dock) prepare(dock);
                    if (!lifetime || expected != generation || !collecting) return;
                    if (nestedPrepare && dock) nestedPrepare(dock);
                    if (!lifetime || expected != generation || !collecting) return;
                }
                if (dock && expected == generation && collecting)
                {
                    QJsonArray entries;
                    if (descriptors)
                    {
                        for (const auto& descriptor : *descriptors)
                        {
                            if (!descriptor.page.visible) continue;
                            entries.append(QJsonObject{
                                {QStringLiteral("id"), addDescriptorRoute(dock, descriptor.page.id)},
                                {QStringLiteral("title"), descriptor.page.title.left(256)},
                                {QStringLiteral("path"), descriptor.path.join(QStringLiteral(" / ")).left(1024)},
                                {QStringLiteral("depth"), descriptor.depth},
                                {QStringLiteral("enabled"), descriptor.page.enabled},
                                {QStringLiteral("current"), dock->isCurrentTab() && descriptor.page.current}});
                        }
                    }
                    else
                    {
                        collect(dock->widget(), dock, QStringList{dock->windowTitle()}, entries);
                    }
                    // 长目录按 32 项分包，单条名称/路径也有限长，不静默丢掉整列。
                    for (int start = 0; start < entries.size(); start += 32)
                    {
                        QJsonArray part;
                        for (int index = start; index < std::min(start + 32, static_cast<int>(entries.size())); ++index)
                        {
                            part.append(entries[index]);
                        }
                        const QJsonObject message{{QStringLiteral("type"), QStringLiteral("column")},
                            {QStringLiteral("column"), column}, {QStringLiteral("entries"), part}};
                        hashCatalogPart(message);
                        reply(message);
                    }
                }
            }
            QTimer::singleShot(0, owner, [this, expected]() { collectNext(expected); });
        }

        void begin(HWND sender, const QString& token)
        {
            deliveries.clear();
            collectWaitingForDelivery = false;
            client = sender;
            request = token;
            removeExpiredRoutes();
            catalogRouteIds.clear();
            catalogHash.reset();
            pending.clear();
            nextDock = 0;
            collecting = true;
            const quint64 expected = ++generation;
            QJsonArray columns; // 一级目录立即返回，后续每个事件周期填充一列。
            for (ads::CDockWidget* dock : list())
            {
                if (!dock)
                {
                    continue;
                }
                pending.append(dock);
                columns.append(QJsonObject{{QStringLiteral("title"), dock->windowTitle()},
                    {QStringLiteral("id"), addRoute(dock, nullptr)}});
            }
            hashCatalogPart(QJsonObject{{QStringLiteral("columns"), columns}});
            reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("begin")},
                {QStringLiteral("columns"), columns}, {QStringLiteral("catalog_instance"), epoch},
                {QStringLiteral("previous_revision"), completedRevision}});
            QTimer::singleShot(0, owner, [this, expected]() { collectNext(expected); });
        }

        void navigate(const QString& id)
        {
            const auto found = routes.constFind(id);
            if (found == routes.cend() || !catalogRouteIds.contains(id)
                || !found->dock || (found->pageRequired && !found->page))
            {
                reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("stale")}});
                return;
            }
            const Route target = *found;
            const QPointer<PageNavigationBridge> lifetime(owner);
            if (!target.descriptorId.isEmpty())
            {
                bool permitted = false;
                if (catalogProvider && catalogNavigator)
                {
                    const auto provider = catalogProvider;
                    const auto catalog = provider(target.dock);
                    if (!lifetime) return;
                    if (catalog && target.dock)
                    {
                        if (const auto pages = ResolvePageNavigationCatalog(*catalog, target.dock->windowTitle()))
                        {
                            for (const auto& page : *pages)
                            {
                                if (page.page.id == target.descriptorId)
                                {
                                    permitted = page.page.visible && page.page.enabled;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!permitted)
                {
                    reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("stale")}});
                    return;
                }
                const auto navigator = catalogNavigator;
                activate(target.dock); // 只有用户已选中目录项时才允许构造Dock。
                if (!lifetime) return;
                if (!target.dock || navigator(target.dock, target.descriptorId) == nullptr)
                {
                    if (lifetime) reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("stale")}});
                    return;
                }
                if (!lifetime) return;
            }
            else
            {
                QList<QPointer<QWidget>> chain;
                for (QWidget* page = target.page; page && page != target.dock; page = page->parentWidget())
                {
                    if (qobject_cast<QStackedWidget*>(page->parentWidget())) chain.prepend(page);
                }
                activate(target.dock);
                if (!lifetime) return;
                for (const QPointer<QWidget>& page : chain)
                {
                    if (!page)
                    {
                        reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("stale")}});
                        return;
                    }
                    auto* stack = qobject_cast<QStackedWidget*>(page->parentWidget());
                    auto* tabs = stack ? qobject_cast<QTabWidget*>(stack->parentWidget()) : nullptr;
                    const int index = stack ? stack->indexOf(page) : -1;
                    if (index < 0 || (tabs && (!tabs->isTabEnabled(index) || !tabs->isTabVisible(index))))
                    {
                        reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("stale")}});
                        return;
                    }
                    tabs ? tabs->setCurrentIndex(index) : stack->setCurrentIndex(index);
                    if (!lifetime) return;
                }
            }
            if (host)
            {
                if (host->isMinimized()) host->showNormal();
                if (!lifetime) return;
                host->raise();
                if (!lifetime) return;
                host->activateWindow();
                if (!lifetime) return;
            }
            reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("activated")}});
        }
    };

    PageNavigationBridge::PageNavigationBridge(QWidget* host, DockList list, DockAction prepare, DockAction activate,
        DockAction nestedPrepare)
        : QObject(host), m_state(std::make_unique<State>())
    {
        m_state->owner = this;
        m_state->deliveryDispatcher = std::make_shared<AsyncUiDispatcher>(this);
        m_state->host = host;
        m_state->list = std::move(list);
        m_state->prepare = std::move(prepare);
        m_state->activate = std::move(activate);
        m_state->nestedPrepare = std::move(nestedPrepare);
        m_state->server = reinterpret_cast<HWND>(host->winId());
    }

    void PageNavigationBridge::setPageCatalog(
        PageNavigationCatalogProvider provider, PageNavigationCatalogNavigator navigator)
    {
        m_state->catalogProvider = std::move(provider);
        m_state->catalogNavigator = std::move(navigator);
    }

    void PageNavigationBridge::setReady()
    {
        ::SetPropW(m_state->server, page_navigation::ServerProperty, reinterpret_cast<HANDLE>(1));
    }

    PageNavigationBridge::~PageNavigationBridge()
    {
        m_state->deliveryDispatcher->close(); // state与原生身份移除前先关闭worker回投门禁。
        m_state->deliveries.clear();
        if (::IsWindow(m_state->server))
        {
            ::RemovePropW(m_state->server, page_navigation::ServerProperty);
        }
    }

    bool PageNavigationBridge::handleMessage(const COPYDATASTRUCT* data, HWND sender)
    {
        if (!data || data->dwData != page_navigation::RequestTag || !data->lpData
            || data->cbData == 0 || data->cbData > 8192
            || !::GetPropW(sender, page_navigation::ClientProperty)
            || !page_navigation::IsCompanionWindow(sender, page_navigation::TaskbarFileName))
        {
            return false;
        }
        const QJsonObject request = QJsonDocument::fromJson(
            QByteArray(static_cast<const char*>(data->lpData), data->cbData)).object();
        const QString operation = request.value(QStringLiteral("op")).toString();
        const QString token = request.value(QStringLiteral("request")).toString();
        if (request.value(QStringLiteral("version")).toInt() != 1 || token.isEmpty() || token.size() > 64)
        {
            return false;
        }
        QTimer::singleShot(0, this, [this, sender, request, operation, token]()
        {
            if (operation == QStringLiteral("catalog"))
            {
                m_state->begin(sender, token);
            }
            else if (sender == m_state->client && token == m_state->request)
            {
                if (operation == QStringLiteral("cancel"))
                {
                    m_state->collecting = false;
                    m_state->deliveries.clear();
                    m_state->collectWaitingForDelivery = false;
                    ++m_state->generation;
                }
                else if (operation == QStringLiteral("navigate"))
                {
                    m_state->navigate(request.value(QStringLiteral("id")).toString());
                }
            }
        });
        return true;
    }
}
