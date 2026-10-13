#include "../Framework.h"
#include "./PageTabMotion.h"

#include <QElapsedTimer>
#include <QEasingCurve>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QTabBar>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace
{
    // Windows 无法查询开关时沿用动画；系统明确关闭时立即使用最终几何。
    bool clientAnimationsEnabled()
    {
        BOOL enabled = TRUE;
        return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled;
    }

    // 只绘制原栏背景层，不替换其 paintEvent、命中位置、键盘逻辑或 close/scroll 子控件。
    class PageTabMotion final : public QObject
    {
    public:
        PageTabMotion(QTabBar* bar, std::function<ks::ui::PageTabMotionColors()> colors)
            : QObject(bar), m_bar(bar), m_colors(std::move(colors)), m_timer(new QTimer(this))
        {
            setObjectName(QStringLiteral("ks_page_tab_motion"));
            m_timer->setTimerType(Qt::PreciseTimer);
            bar->installEventFilter(this);
            connect(bar, &QTabBar::currentChanged, this, [this]() { select(true); });
            connect(bar, &QTabBar::tabMoved, this, [this]() { select(false); });
            connect(m_timer, &QTimer::timeout, this, [this]() { advance(); });
            select(false);
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (!m_bar || watched != m_bar)
            {
                return false;
            }
            switch (event->type())
            {
            case QEvent::Paint:
                // 程序阻塞信号后的选择也能同步；自动滚动等几何变化直接归位，避免旧终点。
                if (m_index != m_bar->currentIndex())
                {
                    select(true);
                }
                else if (m_target != selectedRect())
                {
                    select(false);
                }
                paintSelection();
                break;
            case QEvent::Hide:
                m_timer->stop();
                m_current = m_target;
                break;
            case QEvent::Show:
            case QEvent::Resize:
            case QEvent::LayoutRequest:
            case QEvent::FontChange:
            case QEvent::StyleChange:
            case QEvent::PaletteChange:
            case QEvent::ApplicationPaletteChange:
            case QEvent::ParentChange:
            case QEvent::ScreenChangeInternal:
                select(false);
                break;
            default:
                break;
            }
            // 不消费绘制事件：原 QTabBar 在本底面上绘制所有标签、图标、焦点与拖动反馈。
            return false;
        }

    private:
        // 目标始终来自原控件 tabRect，重排、RTL、横向溢出都使用真实视觉坐标。
        QRectF selectedRect() const
        {
            const int index = m_bar ? m_bar->currentIndex() : -1;
            return index < 0 || !m_bar->isTabVisible(index) ? QRectF() : QRectF(m_bar->tabRect(index));
        }

        // 只重绘选中面经过的小矩形；不会触发日志、表格、曲线等内容区域的绘制。
        void updateArea(const QRectF& previous)
        {
            if (m_bar)
            {
                m_bar->update(previous.united(m_current).adjusted(-2, -2, 2, 2).toAlignedRect());
            }
        }

        // 连续切换从当前插值位置重新起步，不跳回旧页签；隐藏与禁用动画则立即结束。
        void select(bool animate)
        {
            if (!m_bar)
            {
                return;
            }
            const QRectF target = selectedRect();
            m_index = m_bar->currentIndex();
            if (animate && target == m_target)
            {
                return;
            }
            const QRectF previous = m_current;
            m_timer->stop();
            m_target = target;
            if (!animate || target.isEmpty() || m_current.isEmpty()
                || !m_bar->isVisible() || !clientAnimationsEnabled())
            {
                m_current = target;
                updateArea(previous);
                return;
            }
            m_start = m_current;
            m_elapsed.restart();
            m_timer->start(frameInterval());
        }

        // 每次启动读取实际所在屏幕；精确定时器不把 120Hz 屏幕固定成旧 60Hz 间隔。
        int frameInterval() const
        {
            const qreal rate = m_bar && m_bar->screen() ? m_bar->screen()->refreshRate() : 60.0;
            return std::clamp(static_cast<int>(std::lround(1000.0 / std::max(30.0, rate))), 4, 33);
        }

        // 使用经过时间而非帧数插值，繁忙一帧或跨屏后仍在既定时长内到达终点。
        void advance()
        {
            if (!m_bar || !m_bar->isVisible() || !clientAnimationsEnabled())
            {
                select(false);
                return;
            }
            const QRectF previous = m_current;
            const qreal time = std::clamp(m_elapsed.elapsed() / 180.0, 0.0, 1.0);
            const qreal progress = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(time);
            m_current = QRectF(m_start.topLeft() + (m_target.topLeft() - m_start.topLeft()) * progress,
                m_start.size() + (m_target.size() - m_start.size()) * progress);
            if (time >= 1.0)
            {
                m_current = m_target;
                m_timer->stop();
            }
            else if (m_timer->interval() != frameInterval())
            {
                m_timer->setInterval(frameInterval());
            }
            updateArea(previous);
        }

        // Paint 过滤器发生在原生标签绘制之前；QSS 的选中 shape 透明，文字仍为原生。
        void paintSelection()
        {
            if (m_current.isEmpty() || !m_colors)
            {
                return;
            }
            const ks::ui::PageTabMotionColors colors = m_colors();
            QPainter painter(m_bar);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setClipRect(m_bar->rect());
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.selected);
            const bool vertical = m_bar->shape() == QTabBar::RoundedWest
                || m_bar->shape() == QTabBar::RoundedEast || m_bar->shape() == QTabBar::TriangularWest
                || m_bar->shape() == QTabBar::TriangularEast;
            if (m_bar->property("ksword_page_tabs_child").toBool()
                && !m_bar->property("ksword_joined_result_tabs").toBool())
            {
                // 子导航的背景与QSS悬停面使用同一内缩；复用当前插值矩形，不新增动画或命中层。
                const QRectF selection = m_current.adjusted(1, 2, -1, -2); // 对应子栏的1/2px边距。
                painter.drawRoundedRect(selection, 5, 5);
                painter.setBrush(colors.marker);
                const qreal span = std::max(qreal(0), std::min(qreal(28),
                    (vertical ? selection.height() : selection.width()) - 12)); // 短标记降低重复层级的强调。
                const QRectF marker = vertical
                    ? QRectF(selection.left() + 1, selection.center().y() - span / 2, 1.5, span)
                    : QRectF(selection.center().x() - span / 2, selection.bottom() - 2, span, 1.5);
                painter.drawRoundedRect(marker, 0.75, 0.75);
                return;
            }
            if (vertical)
            {
                painter.drawRoundedRect(m_current, 5, 5);
                painter.fillRect(QRectF(m_current.left(), m_current.top(), 2, m_current.height()), colors.marker);
                return;
            }
            // 上侧两角圆滑、下侧贴内容面，细线同步移动而不修改标签原有 2px 占位。
            QPainterPath selected;
            const qreal radius = std::min(qreal(5), m_current.width() / 2.0);
            selected.moveTo(m_current.bottomLeft());
            selected.lineTo(m_current.left(), m_current.top() + radius);
            selected.quadTo(m_current.topLeft(), QPointF(m_current.left() + radius, m_current.top()));
            selected.lineTo(m_current.right() - radius, m_current.top());
            selected.quadTo(m_current.topRight(), QPointF(m_current.right(), m_current.top() + radius));
            selected.lineTo(m_current.bottomRight());
            selected.closeSubpath();
            painter.drawPath(selected);
            painter.fillRect(QRectF(m_current.left(), m_current.bottom() - 2, m_current.width(), 2), colors.marker);
        }

        QPointer<QTabBar> m_bar; // 原业务栏，与控制器保持受控同寿命。
        std::function<ks::ui::PageTabMotionColors()> m_colors; // 主题求值由栏样式入口提供。
        QTimer* m_timer = nullptr; // 仅切换的 180ms 内运行，结束后不消耗帧。
        QElapsedTimer m_elapsed; // 与帧频无关的经过时间。
        QRectF m_start; // 最近一次选择发生时的可见选中面。
        QRectF m_target; // 原栏当前页签的真实几何。
        QRectF m_current; // 当前需要绘制的插值矩形。
        int m_index = -1; // Paint 兜底识别被阻塞信号的程序选择。
    };
}

void ks::ui::InstallPageTabMotion(QTabBar* bar, std::function<PageTabMotionColors()> colors)
{
    if (!bar || bar->property("ksword_page_tab_motion").toBool())
    {
        return;
    }
    bar->setProperty("ksword_page_tab_motion", true);
    bar->setDrawBase(false);
    new PageTabMotion(bar, std::move(colors));
}
