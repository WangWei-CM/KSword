#include "PageTabFocusStyle.h"

#include <QEvent>
#include <QFocusEvent>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleOption>
#include <QTabBar>

namespace
{
    // 独立代理仅处理原生焦点 primitive；标签、图标、命中和动画沿用原栏。
    class PageTabFocusStyle final : public QProxyStyle
    {
    public:
        explicit PageTabFocusStyle(QTabBar* bar) : m_bar(bar)
        {
            setParent(bar); // 代理与栏一起销毁，不借用并接管 QApplication 的全局样式。
            bar->installEventFilter(this);
        }

        void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
            QPainter* painter, const QWidget* widget = nullptr) const override
        {
            // 鼠标焦点不绘键盘提示；键盘来源仍交给原平台风格绘制。
            if (element == PE_FrameFocusRect && !m_keyboardFocus)
            {
                return;
            }
            QProxyStyle::drawPrimitive(element, option, painter, widget);
        }

    protected:
        bool eventFilter(QObject* source, QEvent* event) override
        {
            if (source != m_bar || !event)
            {
                return false;
            }
            bool keyboard = m_keyboardFocus; // keyboard：本次事件决定的焦点提示来源。
            if (event->type() == QEvent::MouseButtonPress)
            {
                keyboard = false;
            }
            else if (event->type() == QEvent::KeyPress)
            {
                keyboard = true;
            }
            else if (event->type() == QEvent::FocusIn)
            {
                const auto reason = static_cast<QFocusEvent*>(event)->reason();
                keyboard = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                    || reason == Qt::ShortcutFocusReason;
            }
            if (keyboard != m_keyboardFocus)
            {
                m_keyboardFocus = keyboard;
                m_bar->update(); // 只更新这条导航，不刷新业务内容。
            }
            return false;
        }

    private:
        QPointer<QTabBar> m_bar; // 原标签栏，销毁后不再接收排队结果。
        bool m_keyboardFocus = false; // 仅键盘操作启用原生焦点提示。
    };
}

namespace ks::ui
{
    void InstallMouseAwareTabFocus(QTabBar* bar)
    {
        if (!bar || bar->property("ksword_mouse_aware_tab_focus").toBool())
        {
            return;
        }
        bar->setProperty("ksword_mouse_aware_tab_focus", true);
        auto* style = new PageTabFocusStyle(bar); // 独立平台代理由栏持有。
        bar->setStyle(style);
    }
}
