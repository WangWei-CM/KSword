#include "WindowControlInspectionOverlay.h"
#include "WindowListInteraction.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QtMath>
#include <QRegion>
#include <QTimer>
#include <QWidget>
#include <dwmapi.h>
#include <algorithm>
#include <set>
#include <tuple>

#pragma comment(lib, "dwmapi.lib")

namespace ks::control_inspection
{
    namespace
    {
        QString T(const char* source)
        { return ks::i18n::sourceText(QString::fromUtf8(source)); }
        HWND native(QWidget* widget) { return reinterpret_cast<HWND>(widget->winId()); }
        bool visibleWindow(HWND window)
        {
            if (!::IsWindowVisible(window) || ::IsIconic(window)) return false;
            DWORD cloaked = 0;
            return FAILED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked == 0;
        }
        QRegion borderDamage(const QRectF& rect, const qreal scale, const bool fill)
        {
            if (rect.isEmpty()) return {};
            const int margin = qCeil(4.0 / scale) + 2;
            const QRect outer = rect.toAlignedRect().adjusted(-margin, -margin, margin, margin);
            if (fill) return QRegion(outer);
            const QRect inner = rect.toAlignedRect().adjusted(margin, margin, -margin, -margin);
            return inner.isEmpty() ? QRegion(outer) : QRegion(outer).subtracted(QRegion(inner));
        }

        class Overlay final : public QWidget
        {
        public:
            explicit Overlay(QWidget* owner) : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint
                | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput)
            {
                setObjectName(QStringLiteral("ks_control_overlay"));
                setPalette(owner->palette()); setFont(owner->font());
                setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating);
                createWinId(); fit();
                ::SetWindowLongPtrW(native(this), GWL_EXSTYLE, ::GetWindowLongPtrW(native(this), GWL_EXSTYLE)
                    | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
                m_ready = true; refresh(true);
            }
            QMap<quintptr, QRegion> visible;
            QMap<quintptr, QRect> hostBounds;
            void fit()
            {
                const auto desktop = ks::window::physicalVirtualDesktop();
                const ks::window::PhysicalCoordinateScope dpi;
                ::SetWindowPos(native(this), HWND_TOPMOST, desktop.x(), desktop.y(), desktop.width(), desktop.height(),
                    SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                refresh(true);
            }
            bool clipToWindows(HWND root, HWND excluded)
            {
                const auto oldRoots = visible.keys();
                QMap<quintptr, QRegion> nextVisible;
                QMap<quintptr, QRect> nextBounds;
                const auto roots = relatedRoots(root, excluded);
                const ks::window::PhysicalCoordinateScope dpi;
                QRegion obstruction;
                for (HWND window = ::GetTopWindow(nullptr); window; window = ::GetWindow(window, GW_HWNDNEXT))
                {
                    if (window == native(this) || !visibleWindow(window)) continue;
                    const QRect bounds = physicalBounds(window);
                    if (bounds.isEmpty()) continue;
                    if (roots.contains(window))
                    {
                        nextVisible.insert(reinterpret_cast<quintptr>(window), QRegion(bounds).subtracted(obstruction));
                        nextBounds.insert(reinterpret_cast<quintptr>(window), bounds);
                    }
                    obstruction += bounds;
                }
                // A selected child HWND is not itself in the top-level Z list.
                if (!nextVisible.contains(reinterpret_cast<quintptr>(root)))
                {
                    HWND top = ::GetAncestor(root, GA_ROOT);
                    QRegion above;
                    for (HWND window = ::GetTopWindow(nullptr); window && window != top;
                        window = ::GetWindow(window, GW_HWNDNEXT))
                        if (window != native(this) && visibleWindow(window)) above += physicalBounds(window);
                    nextVisible.insert(reinterpret_cast<quintptr>(root), QRegion(physicalBounds(root)).subtracted(above));
                    nextBounds.insert(reinterpret_cast<quintptr>(root), physicalBounds(root));
                }
                const bool rootsChanged = oldRoots != nextVisible.keys();
                if (visible != nextVisible || hostBounds != nextBounds)
                {
                    visible.swap(nextVisible); hostBounds.swap(nextBounds);
                    refresh(true);
                }
                return rootsChanged;
            }
            void setScene(const QVector<Node>& value, const Node& hovered, const Node& selected,
                const bool all, const bool tips, const QPoint cursor, const quint64 revision)
            {
                // Page revisions avoid any whole-scene comparison at the idle
                // tick. Legacy callers without a revision retain correct updates.
                const bool sceneChanged = OverlaySceneChanged(revision, m_revision, value, m_nodes);
                const bool hoverChanged = !SameOverlayNode(m_hover, hovered);
                const bool selectedChanged = !SameOverlayNode(m_selected, selected);
                const bool tipChanged = tips != m_tips || (tips && !hovered.id.isEmpty() && cursor != m_cursor);
                const bool allChanged = all != m_all;
                if (!sceneChanged && !hoverChanged && !selectedChanged && !tipChanged && !allChanged) return;
                if (sceneChanged) { m_nodes = value; m_revision = revision; }
                m_hover = hovered; m_selected = selected; m_cursor = cursor;
                m_all = all; m_tips = tips;
                refresh(sceneChanged, hoverChanged, selectedChanged, tipChanged, allChanged);
            }
        protected:
            bool event(QEvent* event) override
            {
                const bool handled = QWidget::event(event);
                if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal)
                    QTimer::singleShot(0, this, [this] { fit(); });
                else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange
                    || event->type() == QEvent::FontChange || event->type() == QEvent::LanguageChange
                    || event->type() == QEvent::Show) refresh(true);
                return handled;
            }
            bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override
            {
                const auto* msg = static_cast<MSG*>(message);
                if (msg->message == WM_DISPLAYCHANGE) QTimer::singleShot(0, this, [this] { fit(); });
                if (msg->message == WM_NCHITTEST) { *result = HTTRANSPARENT; return true; }
                return QWidget::nativeEvent(type, message, result);
            }
            void paintEvent(QPaintEvent* event) override
            {
                QPainter painter(this);
                // Source clearing must restore transparent pixels at OLD edges,
                // but only inside the actual dirty region, never the desktop.
                painter.setClipRegion(event->region());
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.fillRect(event->rect(), Qt::transparent);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                const QColor accent = palette().color(QPalette::Active, QPalette::Highlight);
                const auto draw = [&](const Draw& shape, int alpha, int width, Qt::PenStyle style, bool fill)
                {
                    if (shape.damage.isEmpty() || !shape.damage.intersects(event->region())) return;
                    painter.save(); painter.setClipPath(m_clips.value(shape.host), Qt::IntersectClip);
                    QColor color = accent; color.setAlpha(alpha);
                    QPen pen(color, width, style); pen.setCosmetic(true); painter.setPen(pen);
                    QColor filling = accent; filling.setAlpha(20);
                    painter.setBrush(fill ? QBrush(filling) : Qt::NoBrush);
                    painter.drawRect(shape.rect.adjusted(0, 0, -1 / m_scale, -1 / m_scale)); painter.restore();
                };
                if (m_all) for (const auto& shape : m_shapes) draw(shape, 180, 3, Qt::SolidLine, false);
                draw(m_selectedShape, 235, 4, Qt::DashLine, false);
                draw(m_hoverShape, 255, 4, Qt::SolidLine, true);
                if (m_card.isEmpty() || !event->region().intersects(m_card.toAlignedRect())) return;
                QColor background = KswordTheme::SurfaceColor(); background.setAlpha(255);
                QColor text = KswordTheme::EnsureTextContrast(KswordTheme::TextPrimaryColor(), background); text.setAlpha(255);
                painter.setPen(QPen(accent, 2)); painter.setBrush(background);
                painter.drawRoundedRect(m_card, 4, 4); painter.setPen(text);
                const QFontMetrics metrics(font());
                qreal y = m_card.top() + 10;
                for (const auto& line : m_lines)
                {
                    painter.drawText(QRectF(m_card.left() + 12, y, m_card.width() - 24, metrics.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(line, Qt::ElideRight, static_cast<int>(m_card.width() - 24)));
                    y += metrics.height();
                }
            }
        private:
            struct Draw { quintptr host = 0; QRectF rect; QRegion damage; };
            QRectF local(const QRect& physical) const
            { return ks::window::physicalRectToOverlay(physical, m_origin, m_scale); }
            Draw shape(const Node& node, const bool fill) const
            {
                Draw result;
                if (node.id.isEmpty() || node.offscreen || node.bounds.isEmpty()) return result;
                result.host = reinterpret_cast<quintptr>(node.host);
                const auto clip = m_clipRegions.value(result.host);
                if (clip.isEmpty()) return result;
                const QRect live = hostBounds.value(result.host);
                const QPoint delta = node.hostBounds.isEmpty() ? QPoint() : live.topLeft() - node.hostBounds.topLeft();
                result.rect = local(node.bounds.translated(delta));
                result.damage = borderDamage(result.rect, m_scale, fill).intersected(clip);
                return result;
            }
            void rebuildCard()
            {
                m_card = {}; m_lines.clear();
                if (!m_tips || m_hover.id.isEmpty() || m_hover.offscreen) return;
                m_lines = {m_hover.name.isEmpty() ? T("<无名称>") : m_hover.name,
                    m_hover.type, T("位置：%1, %2　大小：%3 × %4").arg(m_hover.bounds.x()).arg(m_hover.bounds.y())
                        .arg(m_hover.bounds.width()).arg(m_hover.bounds.height()),
                    m_hover.enabled ? T("可用") : T("不可用")};
                const QFontMetrics metrics(font()); qreal width = 0;
                for (const auto& line : m_lines) width = std::max(width, static_cast<qreal>(metrics.horizontalAdvance(line)));
                QRect work = ks::window::physicalVirtualDesktop();
                MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
                if (::GetMonitorInfoW(::MonitorFromPoint(POINT{m_cursor.x(), m_cursor.y()}, MONITOR_DEFAULTTONEAREST), &monitor))
                    work = QRect(monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right - monitor.rcWork.left,
                        monitor.rcWork.bottom - monitor.rcWork.top);
                const QRectF canvas = local(work).adjusted(6, 6, -6, -6);
                width = std::max<qreal>(0, std::min(width + 24, canvas.width()));
                const qreal height = metrics.height() * m_lines.size() + 20;
                QPointF position((m_cursor.x() - m_origin.x()) / m_scale + 18, (m_cursor.y() - m_origin.y()) / m_scale + 22);
                if (position.x() + width > canvas.right()) position.setX((m_cursor.x() - m_origin.x()) / m_scale - width - 18);
                if (position.y() + height > canvas.bottom()) position.setY((m_cursor.y() - m_origin.y()) / m_scale - height - 18);
                position.setX(std::max(canvas.left(), std::min(position.x(), canvas.right() - width)));
                position.setY(std::max(canvas.top(), std::min(position.y(), canvas.bottom() - height)));
                m_card = QRectF(position, QSizeF(width, height));
            }
            void refresh(const bool geometry, const bool hover = true, const bool selected = true,
                const bool tip = true, const bool all = true)
            {
                if (!m_ready) return;
                QRegion damage;
                if (geometry || all) damage += m_allDamage;
                if (geometry || hover) damage += m_hoverShape.damage;
                if (geometry || selected) damage += m_selectedShape.damage;
                if ((geometry || hover || tip) && !m_card.isEmpty()) damage += m_card.toAlignedRect().adjusted(-3, -3, 3, 3);
                if (geometry)
                {
                    m_origin = physicalBounds(native(this)).topLeft(); m_scale = std::max<qreal>(devicePixelRatioF(), 0.25);
                    m_clips.clear(); m_clipRegions.clear(); m_shapes.clear();
                    for (auto it = visible.cbegin(); it != visible.cend(); ++it)
                    {
                        QPainterPath path; QRegion region;
                        for (const auto& part : it.value()) { const QRectF rect = local(part); path.addRect(rect); region += rect.toAlignedRect(); }
                        m_clips.insert(it.key(), std::move(path)); m_clipRegions.insert(it.key(), std::move(region));
                    }
                    std::set<std::tuple<quintptr, qreal, qreal, qreal, qreal>> painted;
                    for (const auto& node : m_nodes)
                    {
                        const Draw draw = shape(node, false);
                        if (!draw.damage.isEmpty() && painted.emplace(draw.host, draw.rect.x(), draw.rect.y(), draw.rect.width(), draw.rect.height()).second)
                            m_shapes.append(draw);
                    }
                }
                if (geometry || all)
                {
                    m_allDamage = {};
                    if (m_all) for (const auto& draw : m_shapes) m_allDamage += draw.damage;
                    damage += m_allDamage;
                }
                if (geometry || hover) { m_hoverShape = shape(m_hover, true); damage += m_hoverShape.damage; }
                if (geometry || selected) { m_selectedShape = shape(m_selected, false); damage += m_selectedShape.damage; }
                if (geometry || hover || tip)
                {
                    rebuildCard();
                    if (!m_card.isEmpty()) damage += m_card.toAlignedRect().adjusted(-3, -3, 3, 3);
                }
                if (!damage.isEmpty()) update(damage);
            }
            QVector<Node> m_nodes;
            Node m_hover, m_selected;
            QPoint m_cursor, m_origin;
            qreal m_scale = 1;
            quint64 m_revision = 0;
            bool m_all = true, m_tips = true;
            bool m_ready = false;
            QVector<Draw> m_shapes;
            Draw m_hoverShape, m_selectedShape;
            QMap<quintptr, QPainterPath> m_clips;
            QMap<quintptr, QRegion> m_clipRegions;
            QRegion m_allDamage;
            QRectF m_card;
            QStringList m_lines;
        };

    }
    QWidget* CreateOverlay(QWidget* owner) { return new Overlay(owner); }
    void FitOverlay(QWidget* overlay) { static_cast<Overlay*>(overlay)->fit(); }
    bool ClipOverlay(QWidget* overlay, HWND root, HWND excluded)
    { return static_cast<Overlay*>(overlay)->clipToWindows(root, excluded); }
    void UpdateOverlay(QWidget* widget, const QVector<Node>& nodes,
        const Node& hovered, const Node& selected, bool all, bool tips, QPoint cursor, quint64 revision)
    {
        static_cast<Overlay*>(widget)->setScene(nodes, hovered, selected, all, tips, cursor, revision);
    }
}
