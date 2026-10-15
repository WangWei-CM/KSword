#include "../Framework.h"
#include "./PageNavigationBridge.h"
#include "../include/ads/DockWidget.h"
#include <QAbstractItemView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
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
        };
        PageNavigationBridge* owner;
        QPointer<QWidget> host;
        DockList list;
        DockAction prepare;
        DockAction nestedPrepare;
        DockAction activate;
        HWND client = nullptr;
        HWND server = nullptr; // 保存原生句柄，析构时不重新调用宿主 winId 创建窗口。
        QString request;
        QString epoch;
        QMap<QString, Route> routes;
        QList<QPointer<ads::CDockWidget>> pending;
        int nextDock = 0;
        quint64 generation = 0;
        bool collecting = false;

        // 消息在原生处理栈外发送，接收方停止响应时最多等待短时间。
        void reply(QJsonObject message)
        {
            if (!host || !::GetPropW(client, page_navigation::ClientProperty)
                || !page_navigation::IsCompanionWindow(client, page_navigation::TaskbarFileName))
            {
                collecting = false;
                return;
            }
            message.insert(QStringLiteral("request"), request);
            message.insert(QStringLiteral("version"), 1);
            const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
            if (bytes.size() > page_navigation::MaxMessageBytes)
            {
                return;
            }
            COPYDATASTRUCT packet{page_navigation::ReplyTag, static_cast<DWORD>(bytes.size()),
                const_cast<char*>(bytes.constData())};
            DWORD_PTR result = 0;
            ::SendMessageTimeoutW(client, WM_COPYDATA, static_cast<WPARAM>(host->winId()),
                reinterpret_cast<LPARAM>(&packet), SMTO_ABORTIFHUNG | SMTO_BLOCK, 750, &result);
        }

        QString addRoute(ads::CDockWidget* dock, QWidget* page)
        {
            const QString id = epoch + QLatin1Char(':') + QString::number(routes.size());
            routes.insert(id, Route{dock, page, page != nullptr});
            return id;
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
            if (nextDock >= pending.size())
            {
                collecting = false;
                reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("complete")}});
                return;
            }
            const int column = nextDock++;
            QPointer<ads::CDockWidget> dock = pending[column];
            if (dock)
            {
                prepare(dock);
                if (nestedPrepare)
                {
                    nestedPrepare(dock);
                }
                if (dock && expected == generation && collecting)
                {
                    QJsonArray entries;
                    collect(dock->widget(), dock, QStringList{dock->windowTitle()}, entries);
                    // 长目录按 32 项分包，单条名称/路径也有限长，不静默丢掉整列。
                    for (int start = 0; start < entries.size(); start += 32)
                    {
                        QJsonArray part;
                        for (int index = start; index < std::min(start + 32, static_cast<int>(entries.size())); ++index)
                        {
                            part.append(entries[index]);
                        }
                        reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("column")},
                            {QStringLiteral("column"), column}, {QStringLiteral("entries"), part}});
                    }
                }
            }
            QTimer::singleShot(0, owner, [this, expected]() { collectNext(expected); });
        }

        void begin(HWND sender, const QString& token)
        {
            client = sender;
            request = token;
            epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
            routes.clear();
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
            reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("begin")},
                {QStringLiteral("columns"), columns}});
            QTimer::singleShot(0, owner, [this, expected]() { collectNext(expected); });
        }

        void navigate(const QString& id)
        {
            const auto found = routes.constFind(id);
            if (found == routes.cend() || !found->dock || (found->pageRequired && !found->page))
            {
                reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("stale")}});
                return;
            }
            const Route target = *found;
            // 激活前存外到内的导航链，切父页时可能触发惰性构建，逐步重新核验对象身份。
            QList<QPointer<QWidget>> chain;
            for (QWidget* page = target.page; page && page != target.dock; page = page->parentWidget())
            {
                if (qobject_cast<QStackedWidget*>(page->parentWidget()))
                {
                    chain.prepend(page);
                }
            }
            activate(target.dock);
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
            }
            if (host)
            {
                if (host->isMinimized()) host->showNormal();
                host->raise();
                host->activateWindow();
            }
            reply(QJsonObject{{QStringLiteral("type"), QStringLiteral("activated")}});
        }
    };

    PageNavigationBridge::PageNavigationBridge(QWidget* host, DockList list, DockAction prepare, DockAction activate,
        DockAction nestedPrepare)
        : QObject(host), m_state(std::make_unique<State>())
    {
        m_state->owner = this;
        m_state->host = host;
        m_state->list = std::move(list);
        m_state->prepare = std::move(prepare);
        m_state->activate = std::move(activate);
        m_state->nestedPrepare = std::move(nestedPrepare);
        m_state->server = reinterpret_cast<HWND>(host->winId());
    }

    void PageNavigationBridge::setReady()
    {
        ::SetPropW(m_state->server, page_navigation::ServerProperty, reinterpret_cast<HANDLE>(1));
    }

    PageNavigationBridge::~PageNavigationBridge()
    {
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
