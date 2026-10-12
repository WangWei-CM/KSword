#include "../Framework.h"
#include "./RoundedViewSurface.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include <array>
#include <cmath>

namespace
{
    constexpr char kControllerProperty[] = "ksword_rounded_view_surface_controller";
    constexpr int kRadius = 8; // 逻辑像素半径随屏幕 DPR 绘制，不改变原表格的尺寸建议。

    enum class Corner
    {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };

    // 此小控件覆盖圆弧外的极角；鼠标完全穿透，不建立整张表格的 mask。
    class RoundedCornerSurface final : public QWidget
    {
    public:
        RoundedCornerSurface(QAbstractItemView* view, Corner corner)
            : QWidget(view), m_view(view), m_corner(corner)
        {
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setAttribute(Qt::WA_NoSystemBackground);
            setAutoFillBackground(false);
            // 局部透明规则避免页面的 QWidget 通配底色先把整个角片填实。
            setProperty("ksword_rounded_view_corner", true);
            setStyleSheet(QStringLiteral(
                "QWidget[ksword_rounded_view_corner=\"true\"]{background:transparent;border:0;}"));
            setFocusPolicy(Qt::NoFocus);
            hide();
        }

    protected:
        // 仅绘制圆角外区域；表头、风险热度、选区和 GPU 表格绘制继续由原视图负责。
        void paintEvent(QPaintEvent*) override
        {
            if (m_view == nullptr || width() <= 0 || height() <= 0)
            {
                return;
            }
            const qreal radius = width(); // 四个角片始终使用相同的正方形逻辑尺寸。
            const bool right = m_corner == Corner::TopRight || m_corner == Corner::BottomRight;
            const bool bottom = m_corner == Corner::BottomLeft || m_corner == Corner::BottomRight;
            const QRectF ellipse(right ? -radius : 0.0, bottom ? -radius : 0.0,
                radius * 2.0, radius * 2.0); // 圆心在表格内侧，圆外才是父页面底色。
            QPainterPath outside; // 圆外路径只占极角，滚动条箭头和滑块主体不被覆盖。
            outside.addRect(QRectF(rect()));
            QPainterPath inside;
            inside.addEllipse(ellipse);
            outside = outside.subtracted(inside);

            // 背景缓存只有本角片大小；不调用 QWidget::render，避免祖先/子控件递归重绘。
            const QImage backdrop = paintAncestorBackdrop();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QBrush(backdrop));
            painter.drawPath(outside);
        }

    private:
        // 按真实坐标绘制祖先本身的背景，支持局部 QSS、深浅色和背景图；返回微型 DPR 图像。
        QImage paintAncestorBackdrop() const
        {
            const qreal ratio = devicePixelRatioF(); // 跨屏缩放后每次按当前 DPR 求物理像素。
            QImage image(QSize(static_cast<int>(std::ceil(width() * ratio)),
                static_cast<int>(std::ceil(height() * ratio))), QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(ratio);
            const bool mainSurface = ks::ui::UsesMainSurfaceHierarchy(m_view);
            const ks::ui::MainSurfaceColors colors = ks::ui::MainWindowSurfaceColors();
            image.fill(mainSurface ? colors.canvas : palette().color(QPalette::Window));
            QPainter painter(&image);
            QVector<QWidget*> ancestors; // 从窗口到最近父面板依序叠加自身背景，不绘制其子树。
            for (QWidget* ancestor = m_view->parentWidget(); ancestor != nullptr;
                ancestor = ancestor->parentWidget())
            {
                ancestors.prepend(ancestor);
            }

            for (QWidget* ancestor : ancestors)
            {
                // 没有 QSS 底面的传统祖先才补 palette；有圆角 QSS 的面板交 PE_Widget 绘制。
                // 不能提前铺满某个面板角色，否则父面板本身的圆外区域也会被误填实。
                if (ancestor->autoFillBackground())
                {
                    painter.fillRect(image.rect(), ancestor->palette().brush(ancestor->backgroundRole()));
                }

                // PE_Widget 只处理该祖先的样式背景，画笔始终被 8px 图像边界裁剪。
                painter.save();
                const QPoint origin = mapTo(ancestor, QPoint()); // 将背景图/渐变原点映射到当前极角。
                painter.translate(-origin);
                QStyleOption option;
                option.initFrom(ancestor);
                ancestor->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, ancestor);
                painter.restore();
            }
            return image;
        }

        QAbstractItemView* m_view; // 父视图拥有角片，绘制生命周期不会越过视图。
        Corner m_corner; // 决定圆心在当前角片的内侧方向。
    };

    // 同一视图只安装一次；Show 仅同步现有角片几何，其余结构/主题通知仍合并处理。
    class RoundedViewController final : public QObject
    {
    public:
        explicit RoundedViewController(QAbstractItemView* view)
            : QObject(view), m_view(view)
        {
            constexpr std::array<Corner, 4> corners = {Corner::TopLeft, Corner::TopRight,
                Corner::BottomLeft, Corner::BottomRight};
            for (std::size_t index = 0; index < corners.size(); ++index)
            {
                m_corners[index] = new RoundedCornerSurface(view, corners[index]);
            }
            view->installEventFilter(this);
        }

        // false 仅隐藏外角装饰，不改变视图/视口层次；custom opt-out 随时可恢复原外观。
        void setEnabled(bool enabled)
        {
            if (m_enabled == enabled)
            {
                return; // 表格重复应用相同外观时，不再排队重画四个角片。
            }
            m_enabled = enabled;
            m_surfaceDirty = true;
            const QPointer<RoundedViewController> lifetime(this); // 外角Hide可同步销毁原视图。
            if (!enabled)
            {
                for (std::size_t index = 0; index < m_corners.size(); ++index)
                {
                    const QPointer<RoundedCornerSurface> corner = m_corners[index];
                    if (corner != nullptr)
                    {
                        corner->hide();
                        if (!lifetime || !m_view || m_enabled != enabled) return;
                    }
                }
            }
            if (m_view && m_view->isVisible())
            {
                refreshNow(); // 首次有效展示只摆放现有角片，不重入视图 polish 或业务布局。
            }
            else
            {
                scheduleRefresh();
            }
        }

    protected:
        // viewport/GPU 后端替换也会产生 ChildAdded；只重排角片，不接管该后端的绘制或计时。
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            const QPointer<QObject> source(watched); // 现有角片Show通知可能使业务关闭宿主。
            if (watched == m_view && event != nullptr)
            {
                switch (event->type())
                {
                case QEvent::Resize:
                case QEvent::LayoutRequest:
                case QEvent::ContentsRectChange:
                    scheduleRefresh();
                    break;
                case QEvent::Show:
                    m_surfaceDirty = true;
                    refreshNow();
                    break;
                case QEvent::Paint:
                    if (m_geometryDirty) refreshNow(); // 仅补纯角片几何，不改视图/viewport 布局。
                    break;
                case QEvent::Move:
                case QEvent::ParentChange:
                case QEvent::ChildAdded:
                case QEvent::StyleChange:
                case QEvent::PaletteChange:
                case QEvent::ApplicationPaletteChange:
                case QEvent::DevicePixelRatioChange:
                    m_surfaceDirty = true; // 祖先背景坐标/主题改变，即使角片大小相同也要重绘。
                    scheduleRefresh();
                    break;
                default:
                    break;
                }
            }
            return source.isNull();
        }

    private:
        void refreshNow()
        {
            ++m_scheduleGeneration; // 同步已经消费旧工作，旧singleShot不能清掉新的pending。
            m_pending = false;
            refreshCorners();
        }

        // 只保存一次排队刷新；控件生命周期由父视图和 QTimer 上下文共同守护。
        void scheduleRefresh()
        {
            m_geometryDirty = true;
            if (m_pending)
            {
                return;
            }
            m_pending = true;
            const quint64 generation = ++m_scheduleGeneration;
            QTimer::singleShot(0, this, [this, generation]()
            {
                if (generation != m_scheduleGeneration) return;
                m_pending = false;
                refreshCorners();
            });
        }

        // 覆盖视图整个外壳的四个极角，header 和 viewport 仍保持原几何与鼠标命中。
        void refreshCorners()
        {
            if (m_view == nullptr)
            {
                return;
            }
            const QPointer<RoundedViewController> lifetime(this);
            m_geometryDirty = false;
            const int radius = qMin(kRadius, qMin(m_view->width(), m_view->height()) / 2);
            const bool repaintSurface = m_surfaceDirty; // 普通同几何LayoutRequest不重采祖先背景。
            m_surfaceDirty = false;
            const std::array<QPoint, 4> positions = {QPoint(0, 0),
                QPoint(m_view->width() - radius, 0), QPoint(0, m_view->height() - radius),
                QPoint(m_view->width() - radius, m_view->height() - radius)};
            for (std::size_t index = 0; index < m_corners.size(); ++index)
            {
                const QPointer<RoundedCornerSurface> corner = m_corners[index];
                if (!corner)
                {
                    continue;
                }
                const QRect geometry(positions[index], QSize(radius, radius));
                const bool geometryChanged = corner->geometry() != geometry;
                if (geometryChanged) corner->setGeometry(geometry);
                if (!lifetime || !corner || !m_view) return;
                corner->setVisible(m_enabled && radius > 0 && m_view->isVisible());
                if (!lifetime || !corner || !m_view) return;
                if (m_enabled && (repaintSurface || geometryChanged))
                {
                    corner->raise();
                    if (!lifetime || !corner || !m_view) return;
                    corner->update();
                }
            }
        }

        QPointer<QAbstractItemView> m_view; // 视图销毁后不访问其几何或父链。
        std::array<QPointer<RoundedCornerSurface>, 4> m_corners; // 原视图拥有的四个极小装饰。
        bool m_enabled = false; // 显式开关，由 TablePresentation 的普通/自绘判断设置。
        bool m_pending = false; // 合并 Show、Resize 和主题变更产生的重复请求。
        bool m_surfaceDirty = true; // 首次展示、重挂或主题变化才强制重采微型背景。
        bool m_geometryDirty = true; // Paint兜底只摆放已存在的四角，不写业务样式或布局。
        quint64 m_scheduleGeneration = 0; // 已同步消费的旧定时任务自动失效。
    };
}

namespace ks::ui
{
    // 调用方明确给出是否使用普通圆角外壳；返回值为空，所有权保持在原视图。
    void SetRoundedViewSurfaceEnabled(QAbstractItemView* view, bool enabled)
    {
        if (view == nullptr)
        {
            return;
        }
        auto* controller = static_cast<RoundedViewController*>(
            view->property(kControllerProperty).value<QObject*>()); // 此私有属性只存本模块创建的对象。
        if (controller == nullptr && enabled)
        {
            controller = new RoundedViewController(view);
            view->setProperty(kControllerProperty, QVariant::fromValue(static_cast<QObject*>(controller)));
        }
        if (controller != nullptr)
        {
            controller->setEnabled(enabled);
        }
    }
}
