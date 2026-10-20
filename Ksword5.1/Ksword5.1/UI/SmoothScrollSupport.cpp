#include "SmoothScrollSupport.h"

#include "./PerfTrace.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QGroupBox>
#include <QLineEdit>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPointingDevice>
#include <QPaintEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QTextBlock>
#include <QTextLayout>
#include <QVariant>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr char kInstalledProperty[] = "KSWORD_SMOOTH_SCROLL_SUPPORT_INSTALLED";
    constexpr char kEnabledProperty[] = "ksword_smooth_scrolling_enabled";
    constexpr char kOriginalVerticalModeProperty[] =
        "KSWORD_SMOOTH_SCROLL_ORIGINAL_VERTICAL_MODE";
    constexpr char kOriginalHorizontalModeProperty[] =
        "KSWORD_SMOOTH_SCROLL_ORIGINAL_HORIZONTAL_MODE";
    constexpr char kFrozenPaneAuxiliaryProperty[] =
        "KSWORD_TABLE_INTERACTION_FROZEN_PANE_AUXILIARY";
    constexpr int kWheelAnimationDurationMs = 180;
    constexpr int kPixelAnimationDurationMs = 100;
    constexpr int kTabWheelStepPixels = 48;
#if KSWORD_PERF_TRACE_ENABLED
    constexpr qint64 kViewportProbeMinimumNs = 2000000;
#endif
    thread_local const QEvent* g_tabScrollFrame = nullptr;

#if KSWORD_PERF_TRACE_ENABLED
    QString scrollAreaPerfLabel(const QAbstractScrollArea* area)
    {
        if (area == nullptr)
        {
            return QString();
        }
        QString label = QString::fromUtf8(area->metaObject()->className());
        if (!area->objectName().isEmpty())
        {
            label += QLatin1Char('/') + area->objectName();
        }
        for (const QWidget* parent = area->parentWidget(); parent != nullptr;
            parent = parent->parentWidget())
        {
            if (!parent->windowTitle().isEmpty())
            {
                label += QStringLiteral(" [") + parent->windowTitle() + QLatin1Char(']');
                break;
            }
        }
        return label;
    }
#endif

    // 滚动条仍使用原生整数像素值；独立时钟避免 QWidget 动画驱动约 60Hz 的默认节拍。
    // GPU 视口由呈现回调驱动；普通视口每次 tick 读取所属屏幕。
    class ScrollBarAnimator final : public QObject
    {
    public:
        explicit ScrollBarAnimator(QScrollBar* bar, QObject* parent)
            : QObject(parent), m_bar(bar)
        {
            m_timer.setTimerType(Qt::PreciseTimer);
            connect(&m_timer, &QTimer::timeout, this, [this]()
            {
                // GPU 定时器仅作回调停滞时的看门狗，不与交换节拍同时推进滚动。
                if (m_frameSource && m_lastFrameClock.elapsed() < watchdogInterval())
                {
                    return;
                }
                advance();
            });
            connect(bar, &QScrollBar::sliderPressed, this, [this]() { stop(); });
            connect(bar, &QScrollBar::valueChanged, this, [this]()
            {
                // 键盘、业务定位及拖动优先；不能被仍在运行的滚轮动画拉回旧终点。
                if (!m_writing)
                {
                    stop();
                }
            });
            connect(bar, &QScrollBar::rangeChanged, this, [this](int minimum, int maximum)
            {
                m_target = std::clamp(m_target, minimum, maximum);
            });
        }

        bool running() const { return m_running; }
        int target() const { return m_target; }
        void stop(const char* outcome = "canceled")
        {
#if KSWORD_PERF_TRACE_ENABLED
            // 采集：动画结束或被新滚动打断时，记录总时长与驱动帧数。
            if (m_running && m_perfActive)
            {
                // 只是动画墙钟生命周期；包含正常帧等待，不能减去同步 work 推导阻塞。
                ks::ui::RecordPerfSpanNs("perf_scroll_animation_elapsed",
                    (m_bar.isNull() ? QString() : m_bar->objectName()) +
                        QLatin1Char('/') + QString::fromLatin1(outcome),
                    m_clock.isValid() ? m_clock.nsecsElapsed() : 0, m_perfFrames);
            }
            m_perfActive = false;
            m_perfFrames = 0;
#endif
            m_running = false;
            m_timer.stop();
            disconnect(m_frameConnection);
            m_frameSource.clear();
            ++m_generation;
        }

        void start(int targetValue, int durationMs)
        {
            stop("replaced");
            if (m_bar.isNull())
            {
                return;
            }
            m_start = m_bar->value();
            m_target = std::clamp(targetValue, m_bar->minimum(), m_bar->maximum());
            m_durationMs = std::max(1, durationMs);
            if (m_start == m_target)
            {
                return;
            }
            m_clock.start();
            m_running = true;
#if KSWORD_PERF_TRACE_ENABLED
            // 采集：本段动画开始累计帧数，由 advance 推进。
            m_perfActive = true;
            m_perfFrames = 0;
#endif
            for (QWidget* parent = m_bar->parentWidget(); parent; parent = parent->parentWidget())
            {
                if (auto* area = qobject_cast<QAbstractScrollArea*>(parent))
                {
                    m_frameSource = qobject_cast<QOpenGLWidget*>(area->viewport());
                    break;
                }
            }
            if (m_frameSource)
            {
                const quint64 generation = m_generation;
                m_frameConnection = connect(m_frameSource, &QOpenGLWidget::frameSwapped,
                    this, [this, generation]()
                {
                    // 用 queued 连接退出合成栈，再更新滚动值；旧动画排队帧不得驱动新动画。
                    if (m_running && generation == m_generation)
                    {
                        m_lastFrameClock.restart();
                        advance();
                    }
                }, Qt::QueuedConnection);
                m_lastFrameClock.start();
                m_timer.start(watchdogInterval());
                m_frameSource->update();
            }
            else
            {
                m_timer.start(frameInterval());
            }
        }

    private:
        int watchdogInterval() const
        {
            return std::max(32, frameInterval() * 4);
        }

        int frameInterval() const
        {
            const QScreen* screen = m_bar.isNull() ? nullptr : m_bar->screen();
            const double reportedRate = screen ? screen->refreshRate() : 60.0;
            const double rate = std::isfinite(reportedRate) && reportedRate > 1.0
                ? reportedRate : 60.0;
            // 向下取整避免 144Hz 被 7ms 定时器压至 142Hz，实际呈现由窗口合成器决定。
            return std::max(1, static_cast<int>(std::floor(1000.0 / rate)));
        }

        void advance()
        {
#if KSWORD_PERF_TRACE_ENABLED
            // 同步驱动区间单独记录，包含最后一帧；不测异步 paint/合成/呈现。
            ks::ui::PerfScope framePerf("perf_scroll_driver_sync_work",
                m_bar.isNull() ? QString() : m_bar->objectName());
            framePerf.setItemCount(0);
            if (m_perfActive)
                ++m_perfFrames; // 推进尝试次数，不是呈现帧数或 FPS。
#endif
            if (!m_running || m_bar.isNull() || !m_bar->isEnabled() || !m_bar->window()->isVisible())
            {
                stop();
                return;
            }
            const double progress = std::clamp(
                m_clock.nsecsElapsed() / (m_durationMs * 1000000.0), 0.0, 1.0);
            const double remaining = 1.0 - progress;
            const double eased = 1.0 - remaining * remaining * remaining;
            const int value = static_cast<int>(std::lround(
                m_start + (static_cast<double>(m_target) - m_start) * eased));
            // 先停止计时，再发最后一帧信号，允许业务连接在信号内开始新滚动。
            if (progress >= 1.0)
            {
                stop("completed");
            }
            else if (const int interval = m_frameSource ? watchdogInterval() : frameInterval();
                m_timer.interval() != interval)
            {
                m_timer.setInterval(interval);
            }
            const QScopedValueRollback<bool> writingGuard(m_writing, true);
#if KSWORD_PERF_TRACE_ENABLED
            const int previousValue = m_bar->value();
#endif
            m_bar->setValue(std::clamp(value, m_bar->minimum(), m_bar->maximum()));
            if (m_running && m_frameSource)
            {
                // 整数像素缓动尾段可能连续两帧 value 相同，也必须维持下一次呈现回调。
                m_frameSource->update();
            }
#if KSWORD_PERF_TRACE_ENABLED
            // count 为本次驱动是否实际改变整数滚动值；仍不等同物理呈现。
            framePerf.setItemCount(!m_bar.isNull() && m_bar->value() != previousValue ? 1 : 0);
#endif
        }

        QPointer<QScrollBar> m_bar;
        QTimer m_timer;
        QElapsedTimer m_clock;
        QElapsedTimer m_lastFrameClock;
        QPointer<QOpenGLWidget> m_frameSource;
        QMetaObject::Connection m_frameConnection;
        quint64 m_generation = 0;
        int m_start = 0;
        int m_target = 0;
        int m_durationMs = kWheelAnimationDurationMs;
        bool m_writing = false;
        bool m_running = false;
#if KSWORD_PERF_TRACE_ENABLED
        bool m_perfActive = false;      // 采集：本段动画是否正在累计帧数。
        int m_perfFrames = 0;           // 采集：本段动画已推进的帧数。
#endif
    };

    // 保留原 QTabBar 和连接，借助 Qt 的像素滚动路径移动标签，避免切页触发懒加载。
    class TabStripAnimator final : public QObject
    {
    public:
        explicit TabStripAnimator(QTabBar* tabBar)
            : QObject(tabBar), m_tabBar(tabBar), m_animation(this),
              m_pixelDevice(QStringLiteral("KSWORD_TAB_SCROLL_DEVICE"), 0,
                  QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
                  QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 1, 0)
        {
            connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value)
                {
                    const int position = value.toInt();
                    movePixels(position - m_delivered);
                    m_delivered = position;
                });
            connect(tabBar, &QTabBar::currentChanged, this, [this]() { stop(); });
            connect(tabBar, &QTabBar::tabMoved, this, [this]() { stop(); });
        }

        bool forwarding() const { return m_forwarding; }

        void stop()
        {
            m_animation.stop();
            m_remainder = 0;
        }

        void wheel(QWheelEvent* event, bool smooth)
        {
            const QPoint delta = event->pixelDelta().isNull() ? event->angleDelta() : event->pixelDelta();
            const int axis = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
            const bool pixels = !event->pixelDelta().isNull();
            const qreal distance = -qreal(axis) * (event->inverted() ? -1 : 1) *
                (pixels ? 1.0 : kTabWheelStepPixels / 120.0);
            if (event->phase() == Qt::ScrollBegin || distance * m_remainder < 0)
                m_remainder = 0;
            m_remainder += distance;
            const int wholePixels = static_cast<int>(std::trunc(m_remainder + std::copysign(1e-9, m_remainder)));
            m_remainder -= wholePixels;
            if (ks::ui::IsTabWheelSwitchingEnabled())
            {
                m_animation.stop();
                m_switchRemainder += distance;
                if (distance * (m_switchRemainder - distance) < 0)
                    m_switchRemainder = distance;
                const int steps = static_cast<int>(m_switchRemainder / kTabWheelStepPixels);
                m_switchRemainder -= steps * kTabWheelStepPixels;
                for (int step = 0; step < std::abs(steps); ++step)
                {
                    const int direction = steps > 0 ? 1 : -1;
                    for (int index = m_tabBar->currentIndex() + direction;
                        index >= 0 && index < m_tabBar->count(); index += direction)
                    {
                        if (m_tabBar->isTabEnabled(index) && m_tabBar->isTabVisible(index))
                        {
                            m_tabBar->setCurrentIndex(index);
                            break;
                        }
                    }
                }
            }
            else
            {
                m_switchRemainder = 0;
                scroll(wholePixels, smooth, pixels ? kPixelAnimationDurationMs : kWheelAnimationDurationMs);
            }
            if (event->phase() == Qt::ScrollEnd)
                m_remainder = m_switchRemainder = 0;
            // 无溢出、修饰键和边界事件也归标签栏处理，禁止向内容区传播或回退到切页。
            event->accept();
        }

    private:
        bool vertical() const
        {
            return m_tabBar->shape() == QTabBar::RoundedWest || m_tabBar->shape() == QTabBar::RoundedEast ||
                m_tabBar->shape() == QTabBar::TriangularWest || m_tabBar->shape() == QTabBar::TriangularEast;
        }

        void movePixels(int distance)
        {
            if (!distance || !m_tabBar->isVisible())
                return;
            const QPoint local = m_tabBar->rect().center();
            const int delta = m_tabBar->isRightToLeft() ? distance : -distance;
            const QPoint pixels = vertical() ? QPoint(0, delta) : QPoint(delta, 0);
            QWheelEvent frame(local, m_tabBar->mapToGlobal(local), pixels, pixels,
                Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false,
                Qt::MouseEventSynthesizedByApplication, &m_pixelDevice);
            m_forwarding = true;
            const QScopedValueRollback<const QEvent*> frameGuard(g_tabScrollFrame, &frame);
            QCoreApplication::sendEvent(m_tabBar, &frame);
            m_forwarding = false;
        }

        void scroll(int distance, bool smooth, int duration)
        {
            if (!distance)
                return;
            const int extent = vertical() ? m_tabBar->height() : m_tabBar->width();
            const int pending = m_animation.state() == QAbstractAnimation::Running
                ? m_animation.endValue().toInt() - m_delivered : 0;
            const int target = std::clamp(distance + (distance * qint64(pending) > 0 ? pending : 0),
                -std::max(1, extent), std::max(1, extent));
            m_animation.stop();
            m_delivered = 0;
            if (!smooth)
            {
                movePixels(target);
                return;
            }
            {
                // 重设已结束的动画时，Qt 可能按旧 currentTime 发出 valueChanged；这不是滚动帧。
                const QSignalBlocker blocker(&m_animation);
                m_animation.setDuration(duration);
                m_animation.setStartValue(0);
                m_animation.setEndValue(target);
                m_animation.setCurrentTime(0);
                m_animation.setEasingCurve(QEasingCurve::OutCubic);
            }
            m_animation.start();
        }

        QTabBar* m_tabBar;
        QVariantAnimation m_animation;
        QPointingDevice m_pixelDevice;
        qreal m_remainder = 0;
        qreal m_switchRemainder = 0;
        int m_delivered = 0;
        bool m_forwarding = false;
    };

    class GlobalSmoothScrollFilter;
    QPointer<GlobalSmoothScrollFilter> g_installedFilter;

    class GlobalSmoothScrollFilter final : public QObject
    {
    public:
        explicit GlobalSmoothScrollFilter(QObject* parentObject)
            : QObject(parentObject)
        {
        }

        void applyEnabledStateToAllWidgets(const bool enabled)
        {
            const QWidgetList widgetList = QApplication::allWidgets();
            for (QWidget* widget : widgetList)
            {
                QAbstractScrollArea* scrollArea =
                    qobject_cast<QAbstractScrollArea*>(widget);
                if (scrollArea != nullptr &&
                    !scrollArea->property(kFrozenPaneAuxiliaryProperty).toBool())
                {
                    configureScrollArea(scrollArea, enabled);
                }
            }
            // 冻结覆盖视图必须在主表切换完滚动模式后再同步，防止关闭平滑滚动时单位不一致。
            for (QWidget* widget : widgetList)
            {
                QAbstractScrollArea* scrollArea =
                    qobject_cast<QAbstractScrollArea*>(widget);
                if (scrollArea != nullptr &&
                    scrollArea->property(kFrozenPaneAuxiliaryProperty).toBool())
                {
                    configureScrollArea(scrollArea, enabled);
                }
            }
            if (!enabled)
            {
                stopAllAnimations();
                for (TabStripAnimator* animator : std::as_const(m_tabAnimators))
                    animator->stop();
            }
        }

        void scrollTabStripByPixels(QAbstractScrollArea* area, int distance, int duration = kWheelAnimationDurationMs)
        {
            if (!area)
                return;
            QScrollBar* bar = area->horizontalScrollBar();
            const int maximumDistance = std::max(1, area->viewport()->width());
            ScrollBarAnimator* animation = animationForScrollBar(bar);
            const qint64 current = bar->value();
            const qint64 pending = animation->running() ? animation->target() : current;
            const int target = static_cast<int>(std::clamp(
                (distance * (pending - current) > 0 ? pending : current) + distance,
                std::max<qint64>(bar->minimum(), current - maximumDistance),
                std::min<qint64>(bar->maximum(), current + maximumDistance)));
            animation->stop();
            if (!enabled())
                bar->setValue(target);
            else if (target != current)
            {
                animation->start(target, duration);
            }
        }

        void stopTabStripScrolling(QAbstractScrollArea* area)
        {
            if (area)
                stopAnimation(area->horizontalScrollBar());
        }

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (eventObject == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            if (eventObject == g_tabScrollFrame && !qobject_cast<QTabBar*>(watchedObject))
            {
                // Qt 像素滚动到边界会 ignore；内部动画帧不能再传给外层页面。
                eventObject->accept();
                return true;
            }

            if (eventObject->type() == QEvent::Show ||
                eventObject->type() == QEvent::Polish)
            {
                if (auto* tabBar = qobject_cast<QTabBar*>(watchedObject))
                    configureTabBar(tabBar);
                if (QAbstractScrollArea* scrollArea =
                    qobject_cast<QAbstractScrollArea*>(watchedObject))
                {
                    configureScrollArea(scrollArea,
                        enabled() && !isSmoothScrollDisabled(scrollArea));
                }
            }

#if KSWORD_PERF_TRACE_ENABLED
            // 全局过滤器位于 QWidget::event() 前；只量入口至 queued 回调的墙钟延迟。
            // 此区间可与其它探针重叠，包含其它 UI 工作，不能当作 Paint/Layout 耗时或求和。
            // 只采集 QTableWidget/QScrollArea 及其 viewport，避免把整棵控件树变成日志热点。
            if (eventObject->type() == QEvent::Paint ||
                eventObject->type() == QEvent::LayoutRequest)
            {
                scheduleViewportProbe(watchedObject, eventObject->type());
            }
#endif

            const bool tabStructureEvent = eventObject->type() == QEvent::Resize ||
                eventObject->type() == QEvent::Hide || eventObject->type() == QEvent::StyleChange ||
                eventObject->type() == QEvent::LayoutRequest || eventObject->type() == QEvent::FontChange;
            if (eventObject->type() == QEvent::Wheel || tabStructureEvent ||
                eventObject->type() == QEvent::MouseButtonPress)
            {
                if (QTabBar* tabBar = tabBarForEventObject(watchedObject))
                {
                    TabStripAnimator* animator = tabAnimator(tabBar);
                    if (eventObject->type() == QEvent::Wheel)
                    {
                        if (animator->forwarding())
                            return false;
                        if (tabBar->isEnabled() && tabBar->isVisible())
                        {
                            configureTabBar(tabBar);
                            animator->wheel(static_cast<QWheelEvent*>(eventObject), enabled());
                            return true;
                        }
                    }
                    else if ((watchedObject == tabBar && tabStructureEvent) ||
                        eventObject->type() == QEvent::MouseButtonPress)
                        animator->stop();
                }
            }

            if (eventObject->type() == QEvent::Resize)
            {
                if (QAbstractScrollArea* scrollArea = scrollAreaForEventObject(watchedObject))
                {
                    // 缩小视口后，旧动画的终点可能超过新的可见范围。
                    stopAnimation(scrollArea->verticalScrollBar());
                    stopAnimation(scrollArea->horizontalScrollBar());
                }
            }

            if (eventObject->type() != QEvent::Wheel)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QAbstractScrollArea* scrollArea = scrollAreaForEventObject(watchedObject, true);
            if (scrollArea == nullptr ||
                scrollArea->property(kFrozenPaneAuxiliaryProperty).toBool() ||
                isSmoothScrollDisabled(qobject_cast<QWidget*>(watchedObject)))
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

#if KSWORD_PERF_TRACE_ENABLED
            // 采集：滚轮在全局过滤器内的同步处理耗时；逐帧动画由动画记录单独统计。
            ks::ui::PerfScope wheelPerf("perf_scroll_wheel",
                scrollAreaPerfLabel(scrollArea));
            // 滚轮本身通常很快；标记它作为后续重绘停顿的最近用户操作。
            ks::ui::RecordPerfMark("perf_scroll_wheel", scrollAreaPerfLabel(scrollArea));
#endif

            auto* wheelEvent = static_cast<QWheelEvent*>(eventObject);
            if (wheelEvent->modifiers().testFlag(Qt::ControlModifier) ||
                wheelEvent->modifiers().testFlag(Qt::AltModifier))
            {
                // 保留 Ctrl+滚轮缩放和业务自定义 Alt+滚轮行为。
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const QPoint pixelDelta = wheelEvent->pixelDelta();
            const QPoint angleDelta = wheelEvent->angleDelta();
            const QScrollBar* directScrollBar = qobject_cast<QScrollBar*>(watchedObject);
            const bool horizontal = directScrollBar != nullptr
                ? directScrollBar->orientation() == Qt::Horizontal
                : (wheelEvent->modifiers().testFlag(Qt::ShiftModifier) ||
                    std::abs(pixelDelta.x()) > std::abs(pixelDelta.y()) ||
                    std::abs(angleDelta.x()) > std::abs(angleDelta.y()));

            const QRect visibleRect = scrollArea->viewport()->visibleRegion().boundingRect();
            // QPlainTextEdit 纵向 value/pageStep 是视觉行，不是像素；通常沿用 Qt
            // 对换行、触控板增量和一页上限的处理，禁止对行号做像素动画。
            if (!horizontal)
            {
                if (QPlainTextEdit* plainEdit = qobject_cast<QPlainTextEdit*>(scrollArea))
                {
                    // 结构报告中的固定高度代码块可能被外层滚动区裁切。Qt 的 pageStep
                    // 仍按代码块完整视口计算，此时额外以真正露出的视觉行数限幅。
                    return scrollClippedPlainText(plainEdit, visibleRect, wheelEvent);
                }
            }
            if (!enabled())
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }
            if (QAbstractItemView* itemView = qobject_cast<QAbstractItemView*>(scrollArea))
            {
                const auto mode = horizontal
                    ? itemView->horizontalScrollMode() : itemView->verticalScrollMode();
                if (mode != QAbstractItemView::ScrollPerPixel)
                {
                    return QObject::eventFilter(watchedObject, eventObject);
                }
            }
            QScrollBar* scrollBar = horizontal
                ? scrollArea->horizontalScrollBar()
                : scrollArea->verticalScrollBar();
            if (scrollBar == nullptr || scrollBar->minimum() == scrollBar->maximum())
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const int rawPixelDelta = horizontal
                ? (pixelDelta.x() != 0 ? pixelDelta.x() : pixelDelta.y())
                : (pixelDelta.y() != 0 ? pixelDelta.y() : pixelDelta.x());
            const int rawAngleDelta = horizontal
                ? (angleDelta.x() != 0 ? angleDelta.x() : angleDelta.y())
                : (angleDelta.y() != 0 ? angleDelta.y() : angleDelta.x());
            if (rawPixelDelta == 0 && rawAngleDelta == 0)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const int directionMultiplier = wheelEvent->inverted() ? -1 : 1;
            const int visibleExtent = horizontal ? visibleRect.width() : visibleRect.height();
            if (visibleExtent <= 0 || scrollBar->pageStep() <= 0)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }
            const int pageExtent = std::min(visibleExtent, scrollBar->pageStep());
            const int overlap = std::min(scrollArea->fontMetrics().lineSpacing(), pageExtent / 2);
            const int maximumDistance = std::max(1, pageExtent - overlap);
            double requestedDistance = 0.0;
            int durationMs = kWheelAnimationDurationMs;
            if (rawPixelDelta != 0)
            {
                requestedDistance = -static_cast<double>(rawPixelDelta) * directionMultiplier;
                durationMs = kPixelAnimationDurationMs;
            }
            else
            {
                const double wheelSteps =
                    static_cast<double>(rawAngleDelta) * directionMultiplier / 120.0;
                const double pixelsPerStep = std::clamp(
                    static_cast<double>(scrollBar->singleStep()) * 3.0,
                    48.0,
                    120.0);
                requestedDistance = -wheelSteps * pixelsPerStep;
            }
            const int distance = static_cast<int>(std::lround(std::clamp(
                requestedDistance, -static_cast<double>(maximumDistance),
                static_cast<double>(maximumDistance))));
            if (distance == 0)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            ScrollBarAnimator* animation = animationForScrollBar(scrollBar);
            const qint64 currentValue = scrollBar->value();
            const qint64 pendingTarget = animation->running() ? animation->target() : currentValue;
            // 反向滚动立即从当前位置反向；连续事件的待滚距离也不能超过一屏。
            const bool sameDirection = distance > 0
                ? pendingTarget > currentValue : pendingTarget < currentValue;
            const qint64 accumulatedStart = sameDirection
                ? pendingTarget : currentValue;
            const int targetValue = static_cast<int>(std::clamp(
                accumulatedStart + distance,
                std::max<qint64>(scrollBar->minimum(), currentValue - maximumDistance),
                std::min<qint64>(scrollBar->maximum(), currentValue + maximumDistance)));
            if (targetValue == scrollBar->value())
            {
                // 到达边界时让未消费的滚轮事件继续向父滚动区域传播。
                animation->stop();
                return QObject::eventFilter(watchedObject, eventObject);
            }

            animation->start(targetValue, durationMs);
            wheelEvent->accept();
            return true;
        }

    private:
        static void configureTabBar(QTabBar* bar)
        {
            if (!bar->usesScrollButtons())
                bar->setUsesScrollButtons(true);
            // setElideMode 即使写相同值也会重排并定位当前标签，不能每档滚轮都调用。
            if (bar->elideMode() != Qt::ElideNone)
                bar->setElideMode(Qt::ElideNone);
        }

        static QTabBar* tabBarForEventObject(QObject* object)
        {
            for (QWidget* widget = qobject_cast<QWidget*>(object); widget; widget = widget->parentWidget())
            {
                if (auto* bar = qobject_cast<QTabBar*>(widget))
                    return bar;
                if (widget->isWindow() || qobject_cast<QAbstractScrollArea*>(widget))
                    break;
            }
            return nullptr;
        }

#if KSWORD_PERF_TRACE_ENABLED
        void scheduleViewportProbe(QObject* watchedObject, QEvent::Type eventType)
        {
            QWidget* watchedWidget = qobject_cast<QWidget*>(watchedObject);
            if (watchedWidget == nullptr)
            {
                return;
            }

            QAbstractScrollArea* area = nullptr;
            if (auto* directArea = qobject_cast<QAbstractScrollArea*>(watchedWidget))
            {
                area = directArea;
            }
            else
            {
                for (QWidget* parent = watchedWidget->parentWidget(); parent != nullptr;
                    parent = parent->parentWidget())
                {
                    auto* candidate = qobject_cast<QAbstractScrollArea*>(parent);
                    if (candidate != nullptr &&
                        (candidate->viewport() == watchedWidget ||
                         (qobject_cast<QScrollArea*>(candidate) != nullptr &&
                          qobject_cast<QScrollArea*>(candidate)->widget() == watchedWidget)))
                    {
                        area = candidate;
                        break;
                    }
                }
            }
            if (area == nullptr ||
                (qobject_cast<QTableWidget*>(area) == nullptr &&
                 qobject_cast<QScrollArea*>(area) == nullptr))
            {
                return;
            }
            const auto* scrollArea = qobject_cast<QScrollArea*>(area);
            if (watchedWidget != area && watchedWidget != area->viewport() &&
                (scrollArea == nullptr || scrollArea->widget() != watchedWidget))
            {
                return;
            }

            // 探针状态只属于采集器，不写 QWidget 动态属性：属性事件会被视觉观察器当作失效。
            const quint8 pendingBit = eventType == QEvent::Paint ? 1 : 2;
            auto pending = m_viewportProbePending.find(area);
            if (pending == m_viewportProbePending.end())
            {
                pending = m_viewportProbePending.insert(area, 0);
                connect(area, &QObject::destroyed, this, [this, area]()
                {
                    m_viewportProbePending.remove(area);
                });
            }
            if ((*pending & pendingBit) != 0)
            {
                return;
            }
            *pending |= pendingBit;
            const char* operationToken = eventType == QEvent::Paint
                ? "perf_scroll_paint_queue_latency" : "perf_scroll_layout_queue_latency";
            const QString targetLabel = scrollAreaPerfLabel(area) +
                (watchedWidget == area->viewport() ? QStringLiteral("/viewport") :
                    QStringLiteral("/content"));
            const qint64 startNs = ks::ui::PerfTraceNowNs();
            QPointer<QAbstractScrollArea> guard(area);
            QTimer::singleShot(0, this, [this, guard, pendingBit, targetLabel, operationToken, startNs]()
            {
                if (guard == nullptr)
                {
                    return;
                }
                auto pending = m_viewportProbePending.find(guard.data());
                if (pending == m_viewportProbePending.end())
                {
                    return;
                }
                *pending &= ~pendingBit;
                const qint64 elapsedNs = std::max<qint64>(
                    0, ks::ui::PerfTraceNowNs() - startNs);
                if (elapsedNs >= kViewportProbeMinimumNs)
                {
                    ks::ui::RecordPerfSpanNs(operationToken, targetLabel, elapsedNs);
                }
            });
        }

#endif

        TabStripAnimator* tabAnimator(QTabBar* bar)
        {
            if (auto* animator = m_tabAnimators.value(bar, nullptr))
                return animator;
            auto* animator = new TabStripAnimator(bar);
            m_tabAnimators.insert(bar, animator);
            connect(bar, &QObject::destroyed, this, [this, bar]() { m_tabAnimators.remove(bar); });
            return animator;
        }

        bool scrollClippedPlainText(QPlainTextEdit* edit, const QRect& visibleRect,
            QWheelEvent* event)
        {
            if (visibleRect.isEmpty() || visibleRect.height() >= edit->viewport()->height())
            {
                return false;
            }
            QScrollBar* bar = edit->verticalScrollBar();
            const int direction = event->inverted() ? -1 : 1;
            const int pixelDelta = event->pixelDelta().y();
            const double requested = pixelDelta != 0
                ? -static_cast<double>(pixelDelta) * direction /
                    std::max(1, edit->fontMetrics().lineSpacing())
                : -static_cast<double>(event->angleDelta().y()) * direction / 120.0 *
                    QApplication::wheelScrollLines() * bar->singleStep();
            if (requested == 0.0 ||
                (requested < 0 && bar->value() == bar->minimum()) ||
                (requested > 0 && bar->value() == bar->maximum()))
            {
                bar->setProperty("ksword_clipped_scroll_remainder", 0.0);
                return false;
            }

            const auto visualLineAt = [edit](const QPoint& point)
            {
                const QTextCursor cursor = edit->cursorForPosition(point);
                const QTextBlock block = cursor.block();
                const QTextLine line = block.layout()->lineForTextPosition(cursor.positionInBlock());
                return block.firstLineNumber() + std::max(0, line.lineNumber());
            };
            const int visibleLines = visualLineAt(visibleRect.bottomLeft()) -
                visualLineAt(visibleRect.topLeft());
            const int maximumDistance = std::max(1, visibleLines - 1);
            double remainder = bar->property("ksword_clipped_scroll_remainder").toDouble();
            if (remainder * requested < 0)
            {
                remainder = 0.0;
            }
            const double bounded = std::clamp(remainder + requested,
                -static_cast<double>(maximumDistance), static_cast<double>(maximumDistance));
            const int distance = static_cast<int>(bounded);
            bar->setProperty("ksword_clipped_scroll_remainder", bounded - distance);
            bar->setValue(static_cast<int>(std::clamp<qint64>(
                static_cast<qint64>(bar->value()) + distance, bar->minimum(), bar->maximum())));
            event->accept();
            return true;
        }

        bool isSmoothScrollDisabled(const QWidget* widget) const
        {
            for (const QWidget* current = widget; current != nullptr;
                current = current->parentWidget())
            {
                if (current->property("ksword_disable_smooth_scroll").toBool())
                {
                    return true;
                }
            }
            return false;
        }

        void stopAnimation(QScrollBar* scrollBar)
        {
            if (ScrollBarAnimator* animation = m_animations.value(scrollBar, nullptr))
            {
                animation->stop();
            }
        }

        bool enabled() const
        {
            QApplication* appInstance =
                qobject_cast<QApplication*>(QCoreApplication::instance());
            return appInstance != nullptr &&
                appInstance->property(kEnabledProperty).toBool();
        }

        QAbstractScrollArea* scrollAreaForEventObject(QObject* watchedObject, bool routeContent = false) const
        {
            if (QAbstractScrollArea* directArea =
                qobject_cast<QAbstractScrollArea*>(watchedObject))
            {
                return directArea;
            }
            if (QScrollBar* scrollBar = qobject_cast<QScrollBar*>(watchedObject))
            {
                for (QWidget* parent = scrollBar->parentWidget(); parent != nullptr;
                    parent = parent->parentWidget())
                {
                    if (QAbstractScrollArea* area = qobject_cast<QAbstractScrollArea*>(parent))
                    {
                        return scrollBar == area->verticalScrollBar() ||
                            scrollBar == area->horizontalScrollBar()
                            ? area : nullptr;
                    }
                }
                return nullptr;
            }
            QAbstractScrollArea* parentArea = qobject_cast<QAbstractScrollArea*>(
                watchedObject != nullptr ? watchedObject->parent() : nullptr);
            if (parentArea != nullptr && parentArea->viewport() == watchedObject)
            {
                return parentArea;
            }
            if (routeContent)
            {
                // 详情表单的滚轮先送到标签/内容容器，并不总会再送到 viewport。
                // 只接管被动内容；编辑器、组合框、自定义画布仍先处理自己的滚轮。
                for (QWidget* widget = qobject_cast<QWidget*>(watchedObject); widget;
                    widget = widget->parentWidget())
                {
                    if (auto* area = qobject_cast<QAbstractScrollArea*>(widget->parentWidget());
                        area && area->viewport() == widget)
                    {
                        return area;
                    }
                    const auto* lineEdit = qobject_cast<QLineEdit*>(widget);
                    const bool passive = widget->metaObject() == &QWidget::staticMetaObject
                        || qobject_cast<QLabel*>(widget) || qobject_cast<QGroupBox*>(widget)
                        || (lineEdit && lineEdit->isReadOnly());
                    if (!passive || widget->isWindow())
                    {
                        break;
                    }
                }
            }
            return nullptr;
        }

        void configureScrollArea(QAbstractScrollArea* scrollArea, const bool enabledState)
        {
            QAbstractItemView* itemView = qobject_cast<QAbstractItemView*>(scrollArea);
            if (itemView == nullptr)
            {
                return;
            }
            if (itemView->property(kFrozenPaneAuxiliaryProperty).toBool())
            {
                // 冻结窗格的偏移由 TableFrozenPaneController 按像素直接写入滚动条，
                // 跟随主表切到按整行滚动会让冻结区与主表错行，因此这里固定按像素。
                itemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                itemView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
                return;
            }

            if (enabledState && !isSmoothScrollDisabled(scrollArea))
            {
                if (!itemView->property(kOriginalVerticalModeProperty).isValid())
                {
                    itemView->setProperty(
                        kOriginalVerticalModeProperty,
                        static_cast<int>(itemView->verticalScrollMode()));
                    itemView->setProperty(
                        kOriginalHorizontalModeProperty,
                        static_cast<int>(itemView->horizontalScrollMode()));
                }
                itemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                itemView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
                return;
            }

            const QVariant originalVerticalMode =
                itemView->property(kOriginalVerticalModeProperty);
            const QVariant originalHorizontalMode =
                itemView->property(kOriginalHorizontalModeProperty);
            if (originalVerticalMode.isValid())
            {
                itemView->setVerticalScrollMode(
                    static_cast<QAbstractItemView::ScrollMode>(
                        originalVerticalMode.toInt()));
                itemView->setProperty(kOriginalVerticalModeProperty, QVariant());
            }
            if (originalHorizontalMode.isValid())
            {
                itemView->setHorizontalScrollMode(
                    static_cast<QAbstractItemView::ScrollMode>(
                        originalHorizontalMode.toInt()));
                itemView->setProperty(kOriginalHorizontalModeProperty, QVariant());
            }
        }

        ScrollBarAnimator* animationForScrollBar(QScrollBar* scrollBar)
        {
            ScrollBarAnimator* animation = m_animations.value(scrollBar, nullptr);
            if (animation != nullptr)
            {
                return animation;
            }

            animation = new ScrollBarAnimator(scrollBar, this);
            m_animations.insert(scrollBar, animation);
            connect(scrollBar, &QObject::destroyed, this, [this, scrollBar]()
                {
                    if (ScrollBarAnimator* removedAnimation =
                        m_animations.take(scrollBar))
                    {
                        removedAnimation->stop();
                        removedAnimation->deleteLater();
                    }
                });
            return animation;
        }

        void stopAllAnimations()
        {
            for (ScrollBarAnimator* animation : std::as_const(m_animations))
            {
                if (animation != nullptr)
                {
                    animation->stop();
                }
            }
        }

#if KSWORD_PERF_TRACE_ENABLED
        QHash<QAbstractScrollArea*, quint8> m_viewportProbePending;
#endif
        QHash<QScrollBar*, ScrollBarAnimator*> m_animations;
        QHash<QTabBar*, TabStripAnimator*> m_tabAnimators;
    };

    GlobalSmoothScrollFilter* installedFilter()
    {
        return g_installedFilter.data();
    }
}

void ks::ui::InstallGlobalSmoothScrollSupport(QApplication* appInstance)
{
    if (appInstance == nullptr || appInstance->property(kInstalledProperty).toBool())
    {
        return;
    }

    auto* filter = new GlobalSmoothScrollFilter(appInstance);
    filter->setObjectName(QStringLiteral("KSWORD_GLOBAL_SMOOTH_SCROLL_FILTER"));
    g_installedFilter = filter;
    appInstance->installEventFilter(filter);
    appInstance->setProperty(kInstalledProperty, true);
    filter->applyEnabledStateToAllWidgets(
        appInstance->property(kEnabledProperty).toBool());
}

void ks::ui::SetGlobalSmoothScrollingEnabled(const bool enabled)
{
    QApplication* appInstance =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    if (appInstance == nullptr)
    {
        return;
    }
    appInstance->setProperty(kEnabledProperty, enabled);
    if (GlobalSmoothScrollFilter* filter = installedFilter())
    {
        filter->applyEnabledStateToAllWidgets(enabled);
    }
}

bool ks::ui::IsGlobalSmoothScrollingEnabled()
{
    QApplication* appInstance =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    return appInstance != nullptr &&
        appInstance->property(kEnabledProperty).toBool();
}

bool ks::ui::IsTabWheelSwitchingEnabled()
{
    return qApp && qApp->property("ksword_slider_wheel_adjust_enabled").toBool();
}

void ks::ui::ScrollTabStripWithWheel(QAbstractScrollArea* area, QWheelEvent* event)
{
    if (!area || !event)
        return;
    const bool pixels = !event->pixelDelta().isNull();
    const QPoint delta = pixels ? event->pixelDelta() : event->angleDelta();
    const int axis = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
    qreal remainder = area->property("ksword_tab_scroll_remainder").toDouble();
    const qreal distance = -qreal(axis) * (event->inverted() ? -1 : 1) *
        (pixels ? 1.0 : kTabWheelStepPixels / 120.0);
    if (event->phase() == Qt::ScrollBegin || remainder * distance < 0)
        remainder = 0;
    remainder += distance;
    const int whole = static_cast<int>(std::trunc(remainder + std::copysign(1e-9, remainder)));
    area->setProperty("ksword_tab_scroll_remainder", event->phase() == Qt::ScrollEnd ? 0.0 : remainder - whole);
    if (whole && installedFilter())
        installedFilter()->scrollTabStripByPixels(area, whole,
            pixels ? kPixelAnimationDurationMs : kWheelAnimationDurationMs);
    event->accept();
}

void ks::ui::ScrollTabStripByPixels(QAbstractScrollArea* area, int distance)
{
    if (installedFilter())
        installedFilter()->scrollTabStripByPixels(area, distance);
}

void ks::ui::StopTabStripScrolling(QAbstractScrollArea* area)
{
    if (installedFilter())
        installedFilter()->stopTabStripScrolling(area);
    if (area)
        area->setProperty("ksword_tab_scroll_remainder", 0.0);
}
