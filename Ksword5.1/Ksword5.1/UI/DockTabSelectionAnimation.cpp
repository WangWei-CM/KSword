#include "../Framework.h"
#include "./DockTabSelectionAnimation.h"
#include "./MainSurfaceStyle.h"
#include "./UiAnimationGate.h"
#include "../include/ads/DockAreaTabBar.h"
#include "../include/ads/DockWidgetTab.h"
#include "../theme.h"

#include <QElapsedTimer>
#include <QEasingCurve>
#include <QEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace ks::ui
{
    namespace
    {
        // 唯一属性只保存本模块拥有的控制器，标签移动到另一栏时明确撤销旧登记。
        constexpr char kControllerProperty[] = "ksword_dock_tab_selection_controller";
        constexpr qreal kDurationMilliseconds = 180.0; // 正常切页动画时长，连续反向仍从当前帧出发。

        // 读取 Windows 客户区动画偏好；查询失败保留系统默认启用行为。
        bool clientAnimationsEnabled()
        {
            BOOL enabled = TRUE; // 操作系统返回的客户端动画启用值。
            // 系统“减少动画”与设置页“界面动画”相互独立，任一关闭即不播放动画。
            return (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled)
                && ks::ui::AreUiAnimationsEnabled();
        }

        // 一栏对应一个控制器，仅负责导航几何；不截图、移动或触发业务内容页。
        class DockTabSelectionMotion final : public QObject
        {
        public:
            explicit DockTabSelectionMotion(ads::CDockAreaTabBar* tabBar)
                : QObject(tabBar), m_tabBar(tabBar), m_container(tabBar->widget()),
                  m_viewport(tabBar->viewport()), m_timer(new QTimer(this))
            {
                m_timer->setTimerType(Qt::PreciseTimer);
                connect(m_timer, &QTimer::timeout, this, [this]() { advanceFrame(); });
                connect(tabBar, &ads::CDockAreaTabBar::currentChanging, this, [this]()
                    {
                        // 在 ADS 更新 activeTab 之前保存旧位置，切换布局不能覆盖这个起点。
                        sampleFrame();
                        m_timer->stop();
                        m_switchOrigin = m_selected;
                        m_switchStart = m_current;
                        m_switchPending = true;
                    });
                connect(tabBar, &ads::CDockAreaTabBar::currentChanged, this,
                    [this]() { scheduleSynchronize(); });
                connect(tabBar, &ads::CDockAreaTabBar::tabInserted, this,
                    [this]() { registerTabs(); scheduleSynchronize(); });
                connect(tabBar, &ads::CDockAreaTabBar::removingTab, this, [this](int index)
                    {
                        unregisterTab(m_tabBar ? m_tabBar->tab(index) : nullptr);
                        cancelSwitch();
                        scheduleSynchronize();
                    });

                // 结构变化与滚动只复位导航几何，保留 ADS 拖动、浮动和可见标签逻辑。
                connect(tabBar, &ads::CDockAreaTabBar::tabMoved, this,
                    [this]() { cancelSwitch(); scheduleSynchronize(); });
                connect(tabBar, &ads::CDockAreaTabBar::tabOpened, this,
                    [this]() { cancelSwitch(); scheduleSynchronize(); });
                connect(tabBar, &ads::CDockAreaTabBar::tabClosed, this,
                    [this]() { cancelSwitch(); scheduleSynchronize(); });
                connect(tabBar->horizontalScrollBar(), &QScrollBar::valueChanged, this,
                    [this]() { cancelSwitch(); synchronize(); });
                for (QWidget* surface : { static_cast<QWidget*>(tabBar), m_container.data(), m_viewport.data() })
                {
                    if (surface != nullptr)
                    {
                        surface->installEventFilter(this);
                    }
                }
                registerTabs();
                synchronize();
            }

            ~DockTabSelectionMotion() override
            {
                m_timer->stop();
                // 控制器先于 QObject 子对象析构；清掉移出的原标签上所有残留指针。
                for (const QPointer<ads::CDockWidgetTab>& tab : m_registeredTabs)
                {
                    unregisterTab(tab);
                }
            }

            // 整面重绘覆盖原 QSS 的静态 activeTab 边，不让两个选中标记同时出现。
            // 标签文字和图标仍由原 ADS 子控件绘制，输入、拖动及命中区域完全不变。
            bool paint(ads::CDockWidgetTab* tab, QPainter& painter)
            {
                if (tab == nullptr || !m_tabBar || !m_container || tab->parentWidget() != m_container)
                {
                    return false;
                }
                const bool hovered = tab->isEnabled() && tab->underMouse(); // 原 hover 状态仍可见。
                const auto baseState = hovered ? KswordTheme::DockTabState::Hover
                    : KswordTheme::DockTabState::Inactive;
                const QPoint origin = tab->mapTo(m_container, QPoint()); // 当前标签在公共栏内的位置。
                const QRectF localSelection = m_current.translated(-origin); // 动画面映射到原标签坐标。

                painter.save();
                painter.setClipRect(tab->rect(), Qt::IntersectClip);
                painter.fillRect(tab->rect(), MainControlSurfaceColor(KswordTheme::DockTabBackgroundColor(baseState), tab));
                if (!localSelection.isEmpty())
                {
                    painter.fillRect(localSelection,
                        MainControlSurfaceColor(KswordTheme::DockTabBackgroundColor(KswordTheme::DockTabState::Active), tab));
                    const QRectF underline(localSelection.left(), qMax(0, tab->height() - 2),
                        localSelection.width(), qMin(2, tab->height())); // 当前帧的连续 2px 下划线。
                    painter.fillRect(underline, KswordTheme::DockTabHighlightColor());
                }
                painter.restore();
                return true;
            }

        protected:
            // 标签、容器与视口的几何改变只排队合并一次；正常 Paint 不触发重布局。
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (event == nullptr || !m_tabBar)
                {
                    return false;
                }
                auto* tab = qobject_cast<ads::CDockWidgetTab*>(watched); // 当前事件对应原标签。
                if (tab != nullptr && event->type() == QEvent::ParentChange)
                {
                    if (tab->parentWidget() != m_container)
                    {
                        unregisterTab(tab);
                    }
                    cancelSwitch();
                    scheduleSynchronize();
                    return false;
                }
                switch (event->type())
                {
                case QEvent::Hide:
                    cancelSwitch();
                    synchronize();
                    break;
                case QEvent::Resize:
                case QEvent::Move:
                case QEvent::LayoutRequest:
                case QEvent::Show:
                case QEvent::FontChange:
                case QEvent::StyleChange:
                case QEvent::LanguageChange:
                case QEvent::ScreenChangeInternal:
                case QEvent::ParentChange:
                    scheduleSynchronize();
                    break;
                default:
                    break;
                }
                return false;
            }

        private:
            // 将现存原标签登记给本控制器，后加入标签复用相同路径，避免浮动栏遗漏。
            void registerTabs()
            {
                if (!m_tabBar || !m_container)
                {
                    return;
                }
                // 已销毁标签不累积在刷新列表内；来回浮动复用相同弱引用而不重复登记。
                for (auto entry = m_registeredTabs.begin(); entry != m_registeredTabs.end();)
                {
                    entry = entry->isNull() ? m_registeredTabs.erase(entry) : std::next(entry);
                }
                for (int index = 0; index < m_tabBar->count(); ++index)
                {
                    ads::CDockWidgetTab* tab = m_tabBar->tab(index); // 本栏公开 API 提供的实际标签。
                    if (tab == nullptr || tab->parentWidget() != m_container
                        || tab->property(kControllerProperty).value<QObject*>() == this)
                    {
                        continue;
                    }
                    if (QObject* previous = tab->property(kControllerProperty).value<QObject*>())
                    {
                        tab->removeEventFilter(previous);
                    }
                    tab->setProperty(kControllerProperty, QVariant::fromValue<QObject*>(this));
                    tab->installEventFilter(this);
                    if (!m_registeredTabs.contains(tab))
                    {
                        m_registeredTabs.append(tab);
                    }
                }
            }

            // 只清掉自己写的属性，新栏已经接管的标签不会被旧控制器回写。
            void unregisterTab(ads::CDockWidgetTab* tab)
            {
                if (tab != nullptr && tab->property(kControllerProperty).value<QObject*>() == this)
                {
                    tab->removeEventFilter(this);
                    tab->setProperty(kControllerProperty, QVariant());
                    tab->update();
                }
            }

            // 读取当前标签实际可见矩形；隐藏栏或失效当前页不继续绘制旧选中面。
            QRectF selectedRectangle() const
            {
                ads::CDockWidgetTab* tab = m_tabBar ? m_tabBar->currentTab() : nullptr;
                if (tab == nullptr || !m_tabBar->isVisible() || !m_container
                    || tab->parentWidget() != m_container || !tab->isVisible())
                {
                    return {};
                }
                return QRectF(tab->mapTo(m_container, QPoint()), tab->size());
            }

            // 单调时钟决定真实进度，不依赖默认 16ms 动画统一时钟或帧数累计。
            void sampleFrame()
            {
                if (!m_timer->isActive())
                {
                    return;
                }
                const qreal time = std::clamp(m_elapsed.elapsed() / kDurationMilliseconds, 0.0, 1.0);
                const qreal amount = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(time);
                m_current = QRectF(m_start.topLeft() + (m_target.topLeft() - m_start.topLeft()) * amount,
                    m_start.size() + (m_target.size() - m_start.size()) * amount);
                if (time >= 1.0)
                {
                    m_current = m_target;
                    m_timer->stop();
                }
            }

            // 逐帧只重绘导航标签；动画偏好运行时关闭后立即收敛至当前目标。
            void advanceFrame()
            {
                if (!m_tabBar || !m_tabBar->isVisible() || !clientAnimationsEnabled())
                {
                    cancelSwitch();
                    synchronize();
                    return;
                }
                sampleFrame();
                refreshTabs();
            }

            // 应用布局完成后的真实目标；同一目标的重复 LayoutRequest 不取消动画。
            void synchronize()
            {
                if (!m_tabBar)
                {
                    return;
                }
                registerTabs();
                const QRectF target = selectedRectangle(); // 当前 ADS 页面最终布局位置。
                QPointer<ads::CDockWidgetTab> selected = m_tabBar->currentTab(); // 用对象身份区分索引变化。
                const bool animate = m_switchPending && selected != m_switchOrigin
                    && !target.isEmpty() && !m_switchStart.isEmpty() && clientAnimationsEnabled();
                const bool targetChanged = target != m_target || selected != m_selected;
                if (!m_switchPending && !targetChanged)
                {
                    return;
                }
                m_timer->stop();
                m_selected = selected;
                m_target = target;
                if (animate)
                {
                    m_start = m_switchStart;
                    m_current = m_start;
                    m_elapsed.restart();
                    const qreal rate = m_tabBar->screen() ? m_tabBar->screen()->refreshRate() : 60.0;
                    const int interval = std::clamp(static_cast<int>(std::lround(
                        1000.0 / std::max(30.0, rate))), 4, 33); // 使用所在屏幕节拍，120Hz 约 8ms。
                    m_timer->start(interval);
                }
                else
                {
                    m_current = m_target;
                }
                m_switchPending = false;
                m_switchOrigin.clear();
                refreshTabs();
            }

            // 当前选择立即复位；滚动、隐藏和结构变化后不能保留过期动画起点。
            void cancelSwitch()
            {
                m_timer->stop();
                m_switchPending = false;
                m_switchOrigin.clear();
                m_current = selectedRectangle();
                m_target = m_current;
                m_selected = m_tabBar ? m_tabBar->currentTab() : nullptr;
                refreshTabs();
            }

            // 合并字体、布局和选中态带来的同步请求；计时器上下文保证析构后不回调。
            void scheduleSynchronize()
            {
                if (m_syncPending)
                {
                    return;
                }
                m_syncPending = true;
                QTimer::singleShot(0, this, [this]()
                    {
                        m_syncPending = false;
                        synchronize();
                    });
            }

            // 只更新当前栏真实标签，不请求主内容区域重绘或再次触发懒加载。
            void refreshTabs()
            {
                for (const QPointer<ads::CDockWidgetTab>& tab : m_registeredTabs)
                {
                    if (tab && tab->property(kControllerProperty).value<QObject*>() == this)
                    {
                        tab->update();
                    }
                }
            }

            QPointer<ads::CDockAreaTabBar> m_tabBar; // 生命周期宿主，栏销毁后所有回调停止。
            QPointer<QWidget> m_container; // ADS 原标签容器，动画始终以此坐标计算。
            QPointer<QWidget> m_viewport; // 原裁剪视口，不创建遮挡点击的覆盖控件。
            QList<QPointer<ads::CDockWidgetTab>> m_registeredTabs; // 登记过的弱标签引用。
            QPointer<ads::CDockWidgetTab> m_selected; // 当前 ADS 逻辑标签身份。
            QPointer<ads::CDockWidgetTab> m_switchOrigin; // 切换前标签身份，索引重排不当作切换。
            QTimer* m_timer = nullptr; // 精确导航帧计时器，由控制器管理生命周期。
            QElapsedTimer m_elapsed; // 当前动画单调起点。
            QRectF m_start; // 正在播放的起始几何。
            QRectF m_target; // 布局稳定后的当前标签矩形。
            QRectF m_current; // 当前实际绘制矩形，连续切换使用该帧。
            QRectF m_switchStart; // activeTab 更新前保存的连续起点。
            bool m_switchPending = false; // 等待当前选择与原标签布局完成。
            bool m_syncPending = false; // 同一事件循环只排队一次几何同步。
        };
    }

    // 保留 ADS 原 TabBar、factory 与页面信号，只添加每栏一个视觉控制器。
    void InstallDockTabSelectionAnimation(ads::CDockAreaTabBar* tabBar)
    {
        if (tabBar == nullptr || tabBar->property(kControllerProperty).value<QObject*>() != nullptr)
        {
            return;
        }
        auto* motion = new DockTabSelectionMotion(tabBar); // 原栏父对象自动销毁对应视觉状态。
        tabBar->setProperty(kControllerProperty, QVariant::fromValue<QObject*>(motion));
    }

    // 原 factory 通过明确拥有属性取回控制器；无控制器时继续原静态绘制。
    bool PaintAnimatedDockTabSelection(ads::CDockWidgetTab* tab, QPainter& painter)
    {
        QObject* controller = tab ? tab->property(kControllerProperty).value<QObject*>() : nullptr;
        return controller != nullptr
            && static_cast<DockTabSelectionMotion*>(controller)->paint(tab, painter);
    }
}
