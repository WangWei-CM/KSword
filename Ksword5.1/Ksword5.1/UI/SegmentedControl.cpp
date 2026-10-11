#include "../Framework.h"
#include "./SegmentedControl.h"
#include "../theme.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QCursor>
#include <QElapsedTimer>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // 中性底和强调选中面都从当前主题派生；文字分别按实际绘制面校准。
    QColor segmentSurface()
    {
        return KswordTheme::BlendColors(KswordTheme::WindowColor(), KswordTheme::SurfaceAltColor(), 120);
    }

    bool clientAnimationsEnabled()
    {
        BOOL enabled = TRUE;
        return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled;
    }

    // 仅保存动画几何；快速切换从当前可见位置开始，隐藏或布局变化立即停止旧帧。
    class SegmentMotion
    {
    public:
        SegmentMotion(QWidget* owner, std::function<void()> refresh)
            : m_owner(owner), m_refresh(std::move(refresh)), m_timer(new QTimer(owner))
        {
            m_timer->setTimerType(Qt::PreciseTimer);
            QObject::connect(m_timer, &QTimer::timeout, owner, [this]()
            {
                const qreal time = std::clamp(m_elapsed.elapsed() / 160.0, 0.0, 1.0);
                const qreal progress = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(time);
                m_current = QRectF(m_start.topLeft() + (m_target.topLeft() - m_start.topLeft()) * progress,
                    m_start.size() + (m_target.size() - m_start.size()) * progress);
                if (time >= 1)
                {
                    m_current = m_target;
                    m_timer->stop();
                }
                m_refresh();
            });
        }

        ~SegmentMotion()
        {
            m_timer->stop();
        }

        void select(const QRectF& rectangle, bool animate)
        {
            if (rectangle == m_target)
            {
                return;
            }
            m_timer->stop();
            m_target = rectangle;
            if (!animate || rectangle.isEmpty() || m_current.isEmpty()
                || !m_owner->isVisible() || !clientAnimationsEnabled())
            {
                m_current = rectangle;
                m_refresh();
                return;
            }
            m_start = m_current;
            m_elapsed.restart();
            const qreal rate = m_owner->screen() == nullptr ? 60.0 : m_owner->screen()->refreshRate();
            m_timer->start(std::clamp(static_cast<int>(std::lround(1000.0 / std::max(30.0, rate))), 4, 33));
        }

        QRectF rectangle() const { return m_current; }
        void stop()
        {
            m_timer->stop();
            m_current = m_target;
        }

    private:
        QWidget* m_owner = nullptr; // 宿主先销毁派生成员，再销毁父级 QTimer。
        std::function<void()> m_refresh; // 只请求宿主及源按钮重绘，不触碰业务状态。
        QTimer* m_timer = nullptr;
        QElapsedTimer m_elapsed;
        QRectF m_start;
        QRectF m_target;
        QRectF m_current;
    };

    // 文字在滑动面内外分别裁剪绘制，避免动画中途出现黑字落在黑底或白字落在浅底。
    void paintSegment(QPainter& painter, const QRectF& bounds, const QRectF& indicator,
        const QString& text, const QIcon& icon, QFont font, bool checked, bool enabled, bool hovered, bool focus)
    {
        const QColor base = segmentSurface();
        const QColor accent = KswordTheme::ControlAccentColor();
        const QColor selected = enabled ? accent : KswordTheme::BlendColors(base, accent, 90);
        painter.save();
        painter.setClipRect(bounds, Qt::IntersectClip); // 标签适配器不能绘制到相邻选项上。
        painter.fillRect(bounds, base);
        if (hovered && enabled)
        {
            painter.fillRect(bounds, KswordTheme::BlendColors(base, accent, 16));
        }
        QPainterPath selectedPath;
        selectedPath.addRoundedRect(indicator, 5, 5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(selected);
        painter.drawPath(selectedPath);

        // 内容位置与字体只计算一次，两种前景使用完全相同的字形和图标轮廓。
        font.setWeight(checked ? QFont::DemiBold : QFont::Normal);
        const QSize iconSize(16, 16);
        const int iconWidth = icon.isNull() ? 0 : iconSize.width() + (text.isEmpty() ? 0 : 6);
        const QString displayText = QFontMetrics(font).elidedText(text, Qt::ElideRight,
            std::max(0, static_cast<int>(bounds.width()) - 20 - iconWidth));
        const qreal contentWidth = QFontMetrics(font).horizontalAdvance(displayText)
            + (icon.isNull() ? 0 : iconSize.width() + (text.isEmpty() ? 0 : 6));
        QRectF content(bounds.center().x() - contentWidth / 2.0,
            bounds.top(), contentWidth, bounds.height());
        const QRectF iconRect(content.left(), content.center().y() - 8, 16, 16);
        QRectF textRect = content;
        if (!icon.isNull())
        {
            textRect.setLeft(iconRect.right() + (text.isEmpty() ? 0 : 6));
        }
        const auto drawContent = [&](const QColor& background)
        {
            const QColor preferred = enabled ? KswordTheme::TextPrimaryColor() : KswordTheme::TextDisabledColor();
            const QColor foreground = KswordTheme::EnsureTextContrast(preferred, background, enabled ? 4.5 : 3.0);
            painter.setFont(font);
            painter.setPen(foreground);
            painter.drawText(textRect, Qt::AlignCenter | Qt::TextShowMnemonic, displayText);
            if (!icon.isNull())
            {
                // 互斥工具选项的单色图形由本组件拥有；保留源 alpha 和当前设备 DPR。
                QPixmap pixels = icon.pixmap(iconSize, painter.device()->devicePixelRatioF(), QIcon::Normal, QIcon::Off);
                if (!pixels.isNull())
                {
                    QPainter tint(&pixels);
                    tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
                    tint.fillRect(pixels.rect(), foreground);
                    tint.end();
                    painter.drawPixmap(iconRect, pixels, QRectF(pixels.rect()));
                }
            }
        };
        painter.save();
        drawContent(hovered && enabled ? KswordTheme::BlendColors(base, accent, 16) : base);
        painter.setClipPath(selectedPath, Qt::IntersectClip);
        drawContent(selected);
        painter.restore();
        if (focus)
        {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(KswordTheme::EnsureTextContrast(accent, base, 3.0), 1));
            painter.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 4, 4);
        }
        painter.restore();
    }

    class ButtonSegments final : public QWidget
    {
    public:
        explicit ButtonSegments(const QList<QAbstractButton*>& buttons, QWidget* parent)
            : QWidget(parent), m_motion(this, [this]() { repaintParts(); })
        {
            setProperty("ksword_segmented_control", true);
            setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            m_row = new QHBoxLayout(this);
            m_row->setContentsMargins(3, 2, 3, 2);
            m_row->setSpacing(0);
            for (QAbstractButton* button : buttons)
            {
                const bool explicitlyHidden = button->isHidden()
                    && button->testAttribute(Qt::WA_WState_ExplicitShowHide);
                m_buttons.push_back(button);
                m_originalWidths.push_back(button->minimumWidth());
                button->setProperty("ksword_segmented_button", true);
                button->setProperty("ksword_theme_icon_managed", true);
                button->setParent(this);
                m_row->addWidget(button);
                if (!explicitlyHidden)
                {
                    button->show(); // setParent 会隐藏原按钮，显式业务隐藏则继续保留。
                }
                button->installEventFilter(this);
                QObject::connect(button, &QAbstractButton::toggled, this, [this]() { queueSync(); });
                QObject::connect(button, &QAbstractButton::clicked, this, [this]() { queueSync(); });
            }
            sync(false);
        }

        QSize sizeHint() const override { return m_hint; }
        QSize minimumSizeHint() const override { return m_hint; }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(segmentSurface());
            painter.drawRoundedRect(QRectF(rect()), 7, 7);
        }

        void resizeEvent(QResizeEvent* event) override
        {
            QWidget::resizeEvent(event);
            for (const auto& button : m_buttons)
            {
                if (button)
                {
                    button->setFixedHeight(std::max(20, height() - 4));
                }
            }
            m_row->activate();
            sync(false);
        }

        void showEvent(QShowEvent* event) override
        {
            QWidget::showEvent(event);
            sync(false);
        }

        void hideEvent(QHideEvent* event) override
        {
            m_motion.stop();
            QWidget::hideEvent(event);
        }

        bool eventFilter(QObject* object, QEvent* event) override
        {
            auto* button = qobject_cast<QAbstractButton*>(object);
            if (button == nullptr)
            {
                return false;
            }
            if (event->type() == QEvent::Paint)
            {
                if (selectedIndex() != m_selectedIndex)
                {
                    queueSync(); // QSignalBlocker 也可能改 checked；以真实状态兜底，不依赖业务信号。
                }
                // 保留原按钮输入和可访问性，仅覆盖绘制；旧 QSS 不再铺出一排独立实色块。
                QPainter painter(button);
                painter.setRenderHint(QPainter::Antialiasing);
                const auto* tool = qobject_cast<QToolButton*>(button);
                const bool textOnly = tool != nullptr && tool->toolButtonStyle() == Qt::ToolButtonTextOnly;
                const QVariant selection = button->property("ksword_segmented_selected");
                paintSegment(painter, QRectF(button->rect()), m_motion.rectangle().translated(-button->pos()),
                    button->text(), textOnly ? QIcon() : button->icon(), button->font(),
                    selection.isValid() ? selection.toBool() : button->isChecked(), button->isEnabled(),
                    button->underMouse(), button->hasFocus() && button->testAttribute(Qt::WA_KeyboardFocusChange));
                return true;
            }
            if (event->type() == QEvent::KeyPress)
            {
                const auto* key = static_cast<QKeyEvent*>(event);
                if (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)
                {
                    const int step = (key->key() == Qt::Key_Right ? 1 : -1)
                        * (layoutDirection() == Qt::RightToLeft ? -1 : 1);
                    int index = m_buttons.indexOf(button) + step;
                    while (index >= 0 && index < m_buttons.size())
                    {
                        if (m_buttons[index] && m_buttons[index]->isEnabled())
                        {
                            m_buttons[index]->setFocus(Qt::TabFocusReason);
                            m_buttons[index]->click();
                            return true;
                        }
                        index += step;
                    }
                }
            }
            if (event->type() == QEvent::DynamicPropertyChange || event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
                || event->type() == QEvent::EnabledChange || event->type() == QEvent::Show
                || event->type() == QEvent::Hide)
            {
                queueSync();
            }
            if (event->type() == QEvent::Enter || event->type() == QEvent::Leave
                || event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            {
                button->update();
            }
            return false;
        }

    private:
        int selectedIndex() const
        {
            int selected = -1;
            for (int index = 0; index < m_buttons.size(); ++index)
            {
                const auto* button = m_buttons[index].data();
                if (button != nullptr && !button->isHidden())
                {
                    const QVariant state = button->property("ksword_segmented_selected");
                    if (state.isValid() ? state.toBool() : button->isChecked())
                    {
                        selected = index;
                    }
                }
            }
            return selected;
        }

        // 合并互斥按钮组的先取消/再选中事件，让同一轮变更从旧选中面平滑移动到新位置。
        void queueSync()
        {
            if (m_pending)
            {
                return;
            }
            m_pending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_pending = false;
                sync(true);
            });
        }

        void sync(bool animate)
        {
            int width = 6;
            int height = 28;
            QRectF selected;
            for (int index = 0; index < m_buttons.size(); ++index)
            {
                auto* button = m_buttons[index].data();
                if (button == nullptr || button->isHidden())
                {
                    continue;
                }
                QFont font = button->font();
                font.setWeight(QFont::Normal);
                QFont selectedFont = font;
                selectedFont.setWeight(QFont::DemiBold);
                const auto* tool = qobject_cast<QToolButton*>(button);
                const bool textOnly = tool != nullptr && tool->toolButtonStyle() == Qt::ToolButtonTextOnly;
                const int iconWidth = button->icon().isNull() || textOnly ? 0 : 22;
                const int measured = std::max({30, m_originalWidths[index],
                    std::max(QFontMetrics(font).horizontalAdvance(button->text()),
                        QFontMetrics(selectedFont).horizontalAdvance(button->text())) + iconWidth + 24});
                if (button->width() != measured || button->minimumWidth() != measured)
                {
                    button->setFixedWidth(measured);
                }
                width += measured;
                height = std::max(height, QFontMetrics(font).height() + 12);
                const QVariant explicitSelection = button->property("ksword_segmented_selected");
                if (explicitSelection.isValid() ? explicitSelection.toBool() : button->isChecked())
                {
                    selected = button->geometry();
                }
            }
            const QSize measuredSize(width, height);
            if (m_hint != measuredSize)
            {
                m_hint = measuredSize;
                setMinimumWidth(width);
                setFixedHeight(height);
                updateGeometry();
                m_row->activate();
                // 尺寸更改会导致按钮位置变化，下一轮布局后再取一次目标矩形。
                queueSync();
            }
            m_motion.select(selected, animate);
            m_selectedIndex = selectedIndex();
            repaintParts();
        }

        void repaintParts()
        {
            update();
            for (const auto& button : m_buttons)
            {
                if (button)
                {
                    button->update();
                }
            }
        }

        QList<QPointer<QAbstractButton>> m_buttons; // 原按钮寿命仍由本宿主管理，信号连接保持有效。
        QList<int> m_originalWidths; // 保留原页面明确设置的最小点击宽度。
        QHBoxLayout* m_row = nullptr;
        SegmentMotion m_motion;
        QSize m_hint;
        bool m_pending = false;
        int m_selectedIndex = -1; // 绘制兜底只在选择确实变化时排队，避免持续重绘循环。
    };

    // 旧自绘编辑器提供值化绘制快照；共享层仅替换 Paint，不吞掉其提示与选择事件。
    class SurfaceSegments final : public QObject
    {
    public:
        SurfaceSegments(QWidget* owner, std::function<ks::ui::SegmentedSurfaceSnapshot()> readState)
            : QObject(owner), m_owner(owner), m_readState(std::move(readState)),
              m_motion(owner, [owner]() { owner->update(); })
        {
            owner->installEventFilter(this);
        }
    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::Paint)
            {
                const auto state = m_readState(); // 调用方仍是原控件，几何与输入共用同一来源。
                const QRectF selected = state.selected >= 0 && state.selected < state.items.size()
                    ? state.items[state.selected].bounds.adjusted(2, 2, -2, -2) : QRectF();
                m_motion.select(selected, true);
                QPainter painter(m_owner);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(segmentSurface());
                painter.drawRoundedRect(QRectF(m_owner->rect()), 7, 7);
                const QPoint hover = m_owner->mapFromGlobal(QCursor::pos());
                for (int index = 0; index < state.items.size(); ++index)
                {
                    const auto& item = state.items[index];
                    paintSegment(painter, item.bounds.adjusted(2, 2, -2, -2), m_motion.rectangle(),
                        item.text, item.icon, m_owner->font(), index == state.selected,
                        item.enabled && m_owner->isEnabled(), m_owner->underMouse() && item.bounds.contains(hover),
                        m_owner->hasFocus() && index == state.selected && m_owner->testAttribute(Qt::WA_KeyboardFocusChange));
                }
                return true;
            }
            if (event->type() == QEvent::Hide || event->type() == QEvent::Resize
                || event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange)
            {
                m_motion.stop();
            }
            return false;
        }
    private:
        QWidget* m_owner = nullptr; // 控件与过滤器同寿命，不交给后台任务。
        std::function<ks::ui::SegmentedSurfaceSnapshot()> m_readState;
        SegmentMotion m_motion;
    };

    // 原生范围标签复用同一选中面和裁剪文字；索引、选择信号与键盘行为继续由 QTabBar 负责。
    class TabSegments final : public QObject
    {
    public:
        explicit TabSegments(QTabBar* bar)
            : QObject(bar), m_bar(bar), m_motion(bar, [bar]() { bar->update(); })
        {
            bar->installEventFilter(this);
            QObject::connect(bar, &QTabBar::currentChanged, this, [this]() { sync(true); });
            sync(false);
        }

    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::Paint)
            {
                if (m_bar->currentIndex() != m_selectedIndex)
                {
                    sync(true); // 搜索范围的程序切换会阻塞 currentChanged，也应移动选中面。
                }
                QPainter painter(m_bar);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(segmentSurface());
                painter.drawRoundedRect(QRectF(m_bar->rect()), 7, 7);
                const int hovered = m_bar->underMouse() ? m_bar->tabAt(m_bar->mapFromGlobal(QCursor::pos())) : -1;
                for (int index = 0; index < m_bar->count(); ++index)
                {
                    paintSegment(painter, QRectF(m_bar->tabRect(index)).adjusted(2, 2, -2, -2),
                        m_motion.rectangle(), m_bar->tabText(index), m_bar->tabIcon(index), m_bar->font(),
                        index == m_bar->currentIndex(), m_bar->isTabEnabled(index) && m_bar->isEnabled(), index == hovered,
                        m_bar->hasFocus() && index == m_bar->currentIndex()
                            && m_bar->testAttribute(Qt::WA_KeyboardFocusChange));
                }
                return true;
            }
            if (event->type() == QEvent::Show || event->type() == QEvent::Resize
                || event->type() == QEvent::FontChange || event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::PaletteChange)
            {
                if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange)
                {
                    m_motion.stop();
                }
                sync(false);
            }
            if (event->type() == QEvent::Hide)
            {
                m_motion.stop();
            }
            if (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter
                || event->type() == QEvent::Leave || event->type() == QEvent::HoverMove)
            {
                m_bar->update();
            }
            return false;
        }

    private:
        void sync(bool animate)
        {
            const int selected = m_bar->currentIndex();
            m_selectedIndex = selected;
            m_motion.select(selected < 0 ? QRectF()
                : QRectF(m_bar->tabRect(selected)).adjusted(2, 2, -2, -2), animate);
        }
        QTabBar* m_bar = nullptr;
        SegmentMotion m_motion;
        int m_selectedIndex = -1;
    };
}

QWidget* ks::ui::InstallSegmentedButtons(QBoxLayout* layout, const QList<QAbstractButton*>& buttons)
{
    if (layout == nullptr || buttons.size() < 2)
    {
        return nullptr;
    }
    int first = layout->count();
    for (QAbstractButton* button : buttons)
    {
        if (button == nullptr || button->property("ksword_segmented_button").toBool()
            || layout->indexOf(button) < 0)
        {
            return nullptr;
        }
        first = std::min(first, layout->indexOf(button));
    }
    QWidget* owner = layout->parentWidget() == nullptr ? buttons.front()->parentWidget() : layout->parentWidget();
    for (QAbstractButton* button : buttons)
    {
        layout->removeWidget(button);
    }
    auto* control = new ButtonSegments(buttons, owner);
    layout->insertWidget(first, control);
    control->show();
    return control;
}

void ks::ui::InstallSegmentedTabBar(QTabBar* bar)
{
    if (bar == nullptr || bar->property("ksword_segmented_tabs").toBool())
    {
        return;
    }
    bar->setProperty("ksword_segmented_tabs", true);
    bar->setProperty("ksword_adaptive_tabbar_configured", true);
    bar->setDrawBase(false);
    bar->setMouseTracking(true);
    bar->setAttribute(Qt::WA_Hover, true);
    bar->setExpanding(true);
    bar->setStyleSheet(QStringLiteral(
        "QTabBar[ksword_segmented_tabs=\"true\"]{background:transparent;border:0;}"
        "QTabBar[ksword_segmented_tabs=\"true\"]::tab{background:transparent;border:0;"
        "padding:4px 12px;min-height:20px;margin:0;}"));
    new TabSegments(bar);
}

void ks::ui::SetSegmentedButtonSelected(QAbstractButton* button, bool selected)
{
    if (button == nullptr)
    {
        return;
    }
    button->setProperty("ksword_segmented_selected", selected);
    const QSignalBlocker blocker(button); // 显式同步外观不重放业务 toggled 回调。
    button->setCheckable(true);
    button->setChecked(selected);
}

void ks::ui::InstallSegmentedSurface(QWidget* owner, std::function<SegmentedSurfaceSnapshot()> readState)
{
    if (owner == nullptr || !readState || owner->property("ksword_segmented_surface").toBool())
    {
        return;
    }
    owner->setProperty("ksword_segmented_surface", true);
    new SurfaceSegments(owner, std::move(readState));
}
