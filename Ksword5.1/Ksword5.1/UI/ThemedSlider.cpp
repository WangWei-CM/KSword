#include "../Framework.h"
#include "./ThemedSlider.h"
#include "../theme.h"
#include <QPainter>
#include <QStyleOptionSlider>

ks::ui::ThemedSlider::ThemedSlider(Qt::Orientation orientation, QWidget* parent)
    : QSlider(orientation, parent)
{
    // 本地几何让原生拖动命中区域与新圆形把手一致；不使用另一套值到像素映射。
    setStyleSheet(QStringLiteral(
        "QSlider{background:transparent;border:0;}"
        "QSlider::groove:horizontal{height:6px;border:0;}"
        "QSlider::handle:horizontal{width:18px;height:18px;margin:-6px 0;border:0;}"
        "QSlider::groove:vertical{width:6px;border:0;}"
        "QSlider::handle:vertical{width:18px;height:18px;margin:0 -6px;border:0;}"));
    if (orientation == Qt::Horizontal)
    {
        setMinimumHeight(28);
    }
    else
    {
        setMinimumWidth(28);
    }
}

void ks::ui::ThemedSlider::paintEvent(QPaintEvent*)
{
    QStyleOptionSlider option;
    initStyleOption(&option); // 使用 QSlider 自身的 RTL、反向外观与拖动位置。
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
        QStyle::SC_SliderHandle, this);
    const bool horizontal = orientation() == Qt::Horizontal;
    const qreal radius = 9.0; // 圆把手与原生命中区域均为 18 个逻辑像素。
    const QRectF track = horizontal
        ? QRectF(radius, height() / 2.0 - 3, std::max(0.0, width() - radius * 2), 6)
        : QRectF(width() / 2.0 - 3, radius, 6, std::max(0.0, height() - radius * 2));
    QColor tail = KswordTheme::BlendColors(KswordTheme::TextPrimaryColor(), KswordTheme::SurfaceColor(), 70);
    QColor active = KswordTheme::ControlAccentColor();
    QColor thumb = QColor(Qt::white);
    if (!isEnabled())
    {
        tail = KswordTheme::BlendColors(KswordTheme::SurfaceColor(), KswordTheme::TextDisabledColor(), 75);
        active = KswordTheme::BlendColors(tail, active, 55);
        thumb = KswordTheme::BlendColors(tail, KswordTheme::TextDisabledColor(), 160);
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(tail);
    painter.drawRoundedRect(track, 3, 3);
    // 填充段使用真实把手中心；最小、最大、RTL 和 invertedAppearance 均与输入一致。
    QRectF filled = track;
    const qreal position = horizontal ? QRectF(handle).center().x() : QRectF(handle).center().y();
    if (horizontal)
    {
        option.upsideDown ? filled.setLeft(position) : filled.setRight(position);
    }
    else
    {
        option.upsideDown ? filled.setTop(position) : filled.setBottom(position);
    }
    painter.setBrush(active);
    painter.drawRoundedRect(filled.intersected(track), 3, 3);
    const QPointF center = horizontal ? QPointF(position, height() / 2.0) : QPointF(width() / 2.0, position);
    painter.setBrush(QColor(0, 0, 0, isEnabled() ? 45 : 15));
    painter.drawEllipse(center + QPointF(0, 1), radius + 1, radius + 1);
    painter.setPen(QPen(KswordTheme::BlendColors(thumb, KswordTheme::BorderColor(), 40), 1));
    painter.setBrush(thumb);
    painter.drawEllipse(center, radius, radius);
    if (hasFocus() && testAttribute(Qt::WA_KeyboardFocusChange))
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(KswordTheme::ControlAccentColor(), 1));
        painter.drawEllipse(center, radius + 2, radius + 2);
    }
}
