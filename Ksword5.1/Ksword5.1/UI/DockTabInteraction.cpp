#include "DockTabInteraction.h"
#include "./DockTabSelectionAnimation.h"
#include "SmoothScrollSupport.h"

#include "../include/ads/DockAreaTabBar.h"
#include "../include/ads/DockAreaTitleBar.h"
#include "../include/ads/DockAreaWidget.h"
#include "../include/ads/DockContainerWidget.h"
#include "../include/ads/DockManager.h"
#include "../include/ads/DockWidgetTab.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QApplication>
#include <QHash>
#include <functional>
#include <QLayout>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <cmath>

namespace ks::ui
{
    namespace
    {
        constexpr int kWheelStepPixels = 48;
        constexpr const char* kInstalledProperty = "ksword_dock_tab_scrolling_installed";

        // 所有 Dock 区域共用一个应用级分派器，事件只沿实际宿主父链定位一次。
        class DockTabEventRouter final : public QObject
        {
        public:
            explicit DockTabEventRouter(QObject* parent) : QObject(parent) {}
            using Handler = std::function<bool(QObject*, QEvent*)>;
            void registerHost(QObject* host, QObject* owner, Handler handler)
            {
                if (host == nullptr)
                {
                    return;
                }
                const bool newHost = !m_hosts.contains(host);
                m_hosts.insert(host, Registration{owner, std::move(handler)});
                if (newHost)
                {
                    connect(host, &QObject::destroyed, this, [this, host]() { m_hosts.remove(host); });
                }
            }
            void unregisterHost(QObject* host, QObject* owner)
            {
                const auto found = m_hosts.find(host);
                if (found != m_hosts.end() && found->owner == owner)
                {
                    m_hosts.erase(found);
                }
            }
        protected:
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (event == nullptr)
                {
                    return false;
                }
                QObject* host = watched;
                while (host != nullptr)
                {
                    const auto found = m_hosts.constFind(host);
                    if (found != m_hosts.cend())
                    {
                        // 回调允许销毁区域或修改登记，先复制弱owner和函数再调用。
                        const Registration registration = *found;
                        return registration.owner != nullptr && registration.handler(watched, event);
                    }
                    if (event->type() != QEvent::Wheel)
                    {
                        break; // 普通事件只对六个明确宿主进行 O(1) 查询。
                    }
                    auto* widget = qobject_cast<QWidget*>(host);
                    if (widget == nullptr || widget->isWindow())
                    {
                        break;
                    }
                    host = widget->parentWidget();
                }
                return false;
            }
        private:
            struct Registration
            {
                QPointer<QObject> owner;
                Handler handler;
            };
            QHash<QObject*, Registration> m_hosts;
        };

        DockTabEventRouter* eventRouter()
        {
            static QPointer<DockTabEventRouter> router;
            if (router == nullptr && qApp != nullptr)
            {
                router = new DockTabEventRouter(qApp);
                qApp->installEventFilter(router);
            }
            return router.data();
        }

        class DockTabScroller final : public QObject
        {
        public:
            explicit DockTabScroller(ads::CDockAreaTitleBar* titleBar)
                : QObject(titleBar->tabBar()), m_tabBar(titleBar->tabBar()),
                  m_titleBar(titleBar), m_viewport(m_tabBar->viewport()),
                  m_tabsContainer(m_tabBar->widget())
            {
                // Ignored 保留可收缩的零最小宽度，但必须同时设置拉伸权重。
                // 否则 ADS 标题栏的 Expanding 占位控件会吃掉全部余量，标签栏被压成零宽。
                m_tabBar->setMinimumWidth(0);
                QSizePolicy tabBarPolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                tabBarPolicy.setHorizontalStretch(1);
                m_tabBar->setSizePolicy(tabBarPolicy);
                m_tabBar->setProperty("ksword_disable_smooth_scroll", true);
                m_tabBar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                m_tabBar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

                m_left = new QToolButton(titleBar);
                m_right = new QToolButton(titleBar);
                m_left->setObjectName(QStringLiteral("ks_dock_scroll_left"));
                m_right->setObjectName(QStringLiteral("ks_dock_scroll_right"));
                m_left->setArrowType(Qt::LeftArrow);
                m_right->setArrowType(Qt::RightArrow);
                for (QToolButton* button : { m_left.data(), m_right.data() })
                {
                    KswordTheme::ApplyCompactIconButtonMetrics(button);
                    button->setAutoRepeat(true);
                    button->setAutoRaise(true);
                    button->hide();
                }
                auto& language = ks::i18n::LanguageManager::instance();
                language.bindToolTip(m_left, QStringLiteral("dock.tabs.scroll_left"), QStringLiteral("向左滚动标签"));
                language.bindToolTip(m_right, QStringLiteral("dock.tabs.scroll_right"), QStringLiteral("向右滚动标签"));
                titleBar->insertWidget(titleBar->indexOf(m_tabBar), m_left);
                titleBar->insertWidget(titleBar->indexOf(m_tabBar) + 1, m_right);

                QScrollBar* const bar = m_tabBar->horizontalScrollBar();
                connect(m_left.data(), &QToolButton::clicked, this, [this]() { scrollByPage(-1); });
                connect(m_right.data(), &QToolButton::clicked, this, [this]() { scrollByPage(1); });
                connect(bar, &QScrollBar::rangeChanged, this, [this]() { scheduleButtons(); });
                connect(bar, &QScrollBar::valueChanged, this, [this]() { scheduleButtons(); });
                connect(m_tabBar.data(), &ads::CDockAreaTabBar::tabInserted, this, [this]() { scheduleButtons(); });
                connect(m_tabBar.data(), &ads::CDockAreaTabBar::removingTab, this, [this]() { scheduleButtons(); });
                connect(m_tabBar.data(), &ads::CDockAreaTabBar::tabOpened, this, [this]() { scheduleButtons(); });
                connect(m_tabBar.data(), &ads::CDockAreaTabBar::tabClosed, this, [this]() { scheduleButtons(); });
                connect(m_tabBar.data(), &ads::CDockAreaTabBar::tabMoved, this, [this]() { scheduleButtons(); });
                connect(m_tabBar.data(), &ads::CDockAreaTabBar::currentChanged, this, [this]()
                    {
                        StopTabStripScrolling(m_tabBar);
                        m_revealCurrent = true;
                        scheduleButtons();
                    });

                // 在 viewportEvent 转发前捕获滚轮；动态文字/图标沿父链归属本栏。
                m_router = eventRouter();
                if (m_router != nullptr)
                {
                    for (QObject* host : eventHosts())
                    {
                        m_router->registerHost(host, this,
                            [this](QObject* watched, QEvent* event) { return handleEvent(watched, event); });
                    }
                }
                scheduleButtons();
            }

            ~DockTabScroller() override
            {
                if (m_router != nullptr)
                {
                    for (QObject* host : eventHosts())
                    {
                        m_router->unregisterHost(host, this);
                    }
                }
                // 箭头属于标题栏；单独销毁/替换标签栏时也要移除其附属按钮。
                for (QToolButton* button : { m_left.data(), m_right.data() })
                {
                    if (button != nullptr)
                    {
                        button->deleteLater();
                    }
                }
            }

        private:
            QList<QObject*> eventHosts() const
            {
                return {m_tabBar.data(), m_titleBar.data(), m_viewport.data(),
                    m_tabsContainer.data(), m_left.data(), m_right.data()};
            }

            bool handleEvent(QObject* watched, QEvent* event)
            {
                if (event == nullptr || !m_tabBar || !m_titleBar || !m_viewport)
                {
                    return false;
                }
                if (watched == m_viewport || watched == m_tabBar ||
                    watched == m_titleBar || watched == m_tabsContainer)
                {
                    switch (event->type())
                    {
                    case QEvent::Resize:
                        StopTabStripScrolling(m_tabBar);
                        m_revealCurrent = m_revealCurrent || watched == m_viewport;
                        scheduleButtons();
                        break;
                    case QEvent::Show:
                    case QEvent::Hide:
                    case QEvent::LayoutRequest:
                    case QEvent::FontChange:
                    case QEvent::StyleChange:
                    case QEvent::LanguageChange:
                        scheduleButtons();
                        break;
                    default:
                        break;
                    }
                }
                if (event->type() != QEvent::Wheel)
                {
                    return false;
                }
                auto* const widget = qobject_cast<QWidget*>(watched);
                if (widget == nullptr || !m_tabBar->isVisible() || !m_tabBar->isEnabled() ||
                    (widget != m_tabBar && !m_tabBar->isAncestorOf(widget) &&
                        widget != m_left && widget != m_right))
                {
                    return false;
                }

                auto* const wheel = static_cast<QWheelEvent*>(event);
                if (!IsTabWheelSwitchingEnabled())
                {
                    m_remainder = 0;
                    ScrollTabStripWithWheel(m_tabBar, wheel);
                    return true;
                }
                StopTabStripScrolling(m_tabBar);
                if (wheel->phase() == Qt::ScrollBegin)
                {
                    m_remainder = 0;
                }
                const QPoint pixelDelta = wheel->pixelDelta();
                const QPoint angleDelta = wheel->angleDelta();
                const bool pixels = !pixelDelta.isNull();
                const QPoint delta = pixels ? pixelDelta : angleDelta;
                const int axisDelta = delta.x() != 0 ? delta.x() : delta.y();
                if (axisDelta == 0)
                {
                    if (wheel->phase() == Qt::ScrollEnd)
                        m_remainder = 0;
                    // 触控板开始/结束事件可能没有位移，不再交给 ADS 二次转发。
                    wheel->accept();
                    return true;
                }
                const qreal distance = qreal(axisDelta) * (wheel->inverted() ? -1 : 1) /
                    (pixels ? qreal(kWheelStepPixels) : 120.0);
                // 只有用户启用既有滚轮调值选项后才允许切页；高分辨率输入累计整档。
                if (distance * m_remainder < 0)
                {
                    m_remainder = 0;
                }
                m_remainder += distance;
                const int steps = static_cast<int>(std::trunc(m_remainder));
                m_remainder -= steps;
                for (int step = 0; step < std::abs(steps); ++step)
                {
                    const int direction = steps > 0 ? -1 : 1;
                    for (int index = m_tabBar->currentIndex() + direction;
                        index >= 0 && index < m_tabBar->count(); index += direction)
                    {
                        if (m_tabBar->isTabOpen(index) && m_tabBar->tab(index)->isEnabled())
                        {
                            m_tabBar->setCurrentIndex(index);
                            break;
                        }
                    }
                }
                if (wheel->phase() == Qt::ScrollEnd)
                    m_remainder = 0;
                // 到边界仍归标签栏处理，防止 ADS 改为切页或滚动下方内容。
                wheel->accept();
                return true;
            }

        private:
            void scrollByPage(int direction)
            {
                if (!m_tabBar || !m_viewport)
                {
                    return;
                }
                m_remainder = 0;
                ScrollTabStripByPixels(m_tabBar,
                    direction * qMax(kWheelStepPixels, m_viewport->width() / 2));
            }

            void updateButtons()
            {
                if (!m_tabBar || !m_titleBar || !m_viewport || !m_tabsContainer || !m_left || !m_right)
                {
                    return;
                }
                QLayout* const tabsLayout = m_tabsContainer->layout();
                QLayout* const titleLayout = m_titleBar->layout();
                if (tabsLayout == nullptr || titleLayout == nullptr)
                {
                    return;
                }

                // ADS 默认可把文字标签压到最小宽度；保留完整自然宽度，溢出才有真实滚动范围。
                const int contentWidth = tabsLayout->sizeHint().width();
                if (m_tabsContainer->minimumWidth() != contentWidth)
                    m_tabsContainer->setMinimumWidth(contentWidth);

                // 使用完整标签的自然宽度，不用可能滞后的滚动条 range 推算内容宽度。
                // 加回已显示箭头及其布局间距，以“不放箭头时能否容纳全部标签”为判据。
                const int spacing = qMax(0, titleLayout->spacing());
                int availableWidth = m_viewport->width();
                for (QToolButton* button : { m_left.data(), m_right.data() })
                {
                    if (!button->isHidden())
                    {
                        availableWidth += button->width() + spacing;
                    }
                }
                const bool overflowing = m_tabBar->isVisible() && availableWidth > 0 &&
                    contentWidth > availableWidth;
                const bool visibilityChanged = m_left->isHidden() == overflowing ||
                    m_right->isHidden() == overflowing;
                if (visibilityChanged)
                {
                    m_left->setVisible(overflowing);
                    m_right->setVisible(overflowing);
                    // 箭头改变可视宽度后，等布局完成再读取滚动范围和定位当前标签。
                    scheduleButtons();
                    return;
                }

                QScrollBar* const bar = m_tabBar->horizontalScrollBar();
                if (!overflowing)
                {
                    m_remainder = 0;
                    StopTabStripScrolling(m_tabBar);
                    bar->setValue(bar->minimum());
                }
                if (m_revealCurrent && m_tabBar->isVisible())
                {
                    m_revealCurrent = false;
                    if (ads::CDockWidgetTab* current = m_tabBar->currentTab())
                    {
                        m_tabBar->ensureWidgetVisible(current, 0, 0);
                    }
                }
                m_left->setEnabled(overflowing && bar->value() > bar->minimum());
                m_right->setEnabled(overflowing && bar->value() < bar->maximum());
            }

            void scheduleButtons()
            {
                if (m_updatePending)
                {
                    return;
                }
                m_updatePending = true;
                QTimer::singleShot(0, this, [this]()
                    {
                        m_updatePending = false;
                        updateButtons();
                    });
            }

            QPointer<DockTabEventRouter> m_router;
            QPointer<ads::CDockAreaTabBar> m_tabBar;
            QPointer<ads::CDockAreaTitleBar> m_titleBar;
            QPointer<QWidget> m_viewport;
            QPointer<QWidget> m_tabsContainer;
            QPointer<QToolButton> m_left;
            QPointer<QToolButton> m_right;
            qreal m_remainder = 0;
            bool m_updatePending = false;
            bool m_revealCurrent = false;
        };

        void installOnTabBar(ads::CDockAreaWidget* dockArea)
        {
            if (dockArea == nullptr || dockArea->titleBar() == nullptr)
            {
                return;
            }
            ads::CDockAreaTabBar* const tabBar = dockArea->titleBar()->tabBar();
            if (tabBar == nullptr || dockArea->titleBar()->indexOf(tabBar) < 0 ||
                tabBar->property(kInstalledProperty).toBool())
            {
                return;
            }
            // ADS 标题栏与所属内容使用同一外壳，中间不另留布局缝隙。
            // 不触碰任何业务页面自己的内容留白、滚动或专业分栏。
            if (QLayout* areaLayout = dockArea->layout())
            {
                areaLayout->setContentsMargins(0, 0, 0, 0);
                areaLayout->setSpacing(0);
            }
            tabBar->setProperty(kInstalledProperty, true);
            // 与既有滚动控制共用原标签栏；动画仅改变导航底面，不改页面或鼠标行为。
            InstallDockTabSelectionAnimation(tabBar);
            new DockTabScroller(dockArea->titleBar());
        }
    }

    void installDockTabWheelScrolling(ads::CDockManager* dockManager)
    {
        if (dockManager == nullptr || dockManager->property(kInstalledProperty).toBool())
        {
            return;
        }
        dockManager->setProperty(kInstalledProperty, true);
        for (ads::CDockContainerWidget* container : dockManager->dockContainers())
        {
            if (container != nullptr)
            {
                for (ads::CDockAreaWidget* area : container->openedDockAreas())
                {
                    installOnTabBar(area);
                }
            }
        }
        QObject::connect(dockManager, &ads::CDockManager::dockAreaCreated,
            dockManager, [](ads::CDockAreaWidget* area) { installOnTabBar(area); });
    }

    bool discardSavedDockLayout(const QString& layoutConfigPath)
    {
        if (layoutConfigPath.isEmpty())
        {
            return false;
        }
        const QFileInfo info(layoutConfigPath);
        if (!info.exists())
        {
            return true;
        }
        return QFile::remove(layoutConfigPath);
    }
}
