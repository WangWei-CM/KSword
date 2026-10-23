#pragma once

// AdaptivePageScroll：让"控件很多、天然比 Dock 高"的页面在自己内部滚动。
//
// 问题根源：
//   QTabWidget 内部的 QStackedLayout 会对所有页面（包括当前没显示的页）取
//   minimumSize 的最大值，所以只要某一页的控件叠得够高，整个 QTabWidget 乃至外面的
//   Dock 就有一个无法再压缩的最小高度。Dock 被塞进 ADS 的外层 QScrollArea 时，
//   视口一旦小于这个最小高度，被滚走的是整个 Dock（标题栏、工具栏、页签栏、状态栏），
//   用户看到的就是"窗口过长，整页在滚动"。
//
// 解法分两半，必须成对使用：
//   1. EnablePageInnerScroll：每个页面自己带一个"对外最小尺寸为 0"的滚动壳，
//      页面内容放不下时在壳里滚动，而不是把 Dock 撑高；
//   2. 宿主再去掉外层滚动（例如 ADS 的 ForceNoScrollArea）。
//   只做第 2 步而页面没包壳，超高页的底部会被静默裁掉，比原来更糟，
//   所以调用方必须先包壳、后去外层滚动。
//
// 本文件保留 header-only 布局入口；主题与主表面策略复用生产共享实现。
// 本文件没有任何用户可见文本，因此不涉及语言包；objectName 使用蛇形小写，
// 不会被 i18n 审计当成待翻译串。

#include "./MainSurfaceStyle.h"
#include "./ThemeBinding.h"
#include "../theme.h"
#include <QFrame>
#include <QEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QScrollArea>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace ks::ui
{
    // 普通主工作区的滚动正文原本透出同一canvas实色；这里显式全矩形填充，
    // 使Qt能够复用backingstore移动。真正透明宿主与未登记角色的独立窗保持原策略。
    class AdaptivePageCanvas final : public QWidget
    {
    public:
        explicit AdaptivePageCanvas(QWidget* parent) : QWidget(parent) {}
        void prepareSurface()
        {
            // DetailDialogChrome明确将所有QScrollArea/viewport设为WindowColor实底；
            // 仅识别当前顶层的共享shell标记，不沿祖先跨进其它独立窗口猜测颜色。
            const bool detailSurface = window()->property("ksword_detail_shell").toBool();
            const QColor color = detailSurface ? KswordTheme::WindowColor() : MainWindowSurfaceColors().canvas;
            const bool solid = (UsesMainSurfaceHierarchy(this) || detailSurface) && color.alpha() == 255;
            const bool changed = m_solid != solid || m_surface != color;
            m_solid = solid;
            m_surface = color;
            if (testAttribute(Qt::WA_OpaquePaintEvent) != m_solid)
            {
                setAttribute(Qt::WA_OpaquePaintEvent, m_solid);
            }
            if (changed) update();
        }
    protected:
        bool event(QEvent* event) override
        {
            const bool handled = QWidget::event(event);
            switch (event->type())
            {
            case QEvent::Show:
            case QEvent::ParentChange:
            case QEvent::StyleChange:
            case QEvent::Polish:
            case QEvent::ApplicationPaletteChange:
            case QEvent::PaletteChange:
                prepareSurface(); // 宿主/背景策略建立后、首绘前维护局部opaque契约。
                break;
            default:
                break;
            }
            return handled;
        }
        void paintEvent(QPaintEvent* event) override
        {
            if (m_solid)
            {
                QPainter painter(this);
                painter.fillRect(rect(), m_surface); // 每个像素实填，包括局部QSS的圆角区域。
                return;
            }
            QWidget::paintEvent(event);
        }
    private:
        QColor m_surface;
        bool m_solid = false;
    };

    // kAdaptivePageScrollObjectName：页面内部滚动壳（QScrollArea）的 objectName。
    // 测试与静态门禁靠它定位壳，所以名字必须稳定。
    inline constexpr char kAdaptivePageScrollObjectName[] = "ks_adaptive_page_scroll";

    // kAdaptivePageContentObjectName：滚动壳里内容容器的 objectName。
    inline constexpr char kAdaptivePageContentObjectName[] = "ks_adaptive_page_content";

    // IsolateMinimumSize：
    // - 作用：让一个控件对父布局"最小尺寸贡献为 0"，自身的 minimumSizeHint 不再向上传播；
    // - 调用方式：对 QTabWidget、嵌入的复合控件等"内部页面高度不可控"的容器调用；
    // - 参数 widget：要隔离的控件，传空指针时什么都不做；
    // - 为什么要同时设 Ignored：qSmartMinSize 只有在策略为 Ignored 时才会把对应方向的
    //   下限置 0，单独 setMinimumSize(0, 0) 压不住 minimumSizeHint 的传播；
    // - 副作用：该控件的 sizeHint 也被布局忽略，它只会拿到父布局分给它的空间。
    inline void IsolateMinimumSize(QWidget* const widget)
    {
        // 空指针直接返回，调用方不必为"控件可能没创建"单独判空。
        if (widget == nullptr)
        {
            return;
        }

        // 显式最小尺寸清零，再把两个方向的策略都设成 Ignored。
        widget->setMinimumSize(0, 0);
        widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }

    // EnablePageInnerScroll：
    // - 作用：给一个"刚创建、还没有布局"的页面装上内部滚动壳，并返回内容容器；
    // - 调用方式：
    //     m_tabX = new QWidget(m_tabWidget);
    //     QWidget* const tabContent = ks::ui::EnablePageInnerScroll(m_tabX);
    //     QVBoxLayout* tabLayout = new QVBoxLayout(tabContent);   // 原来是 (m_tabX)
    //   类页面在构造函数里写 new QVBoxLayout(ks::ui::EnablePageInnerScroll(this))；
    // - 参数 page：页面自身，必须还没有布局；页面指针身份不变，所以
    //   QTabWidget::indexOf、setCurrentWidget、bindTab 等按页面指针引用的代码都不受影响；
    // - 返回：内容容器。调用方在它上面建自己原来的布局；page 为空时返回空指针；
    // - 幂等：同一页面重复调用会直接返回已有的内容容器，不会套第二层壳；
    // - 页面已经有布局时没法安全包壳（Qt 不允许给同一控件装第二个布局），
    //   这种情况退回 page 本身，行为与改动前一致，同时 Q_ASSERT 提醒调用方顺序错了。
    inline QWidget* EnablePageInnerScroll(QWidget* const page)
    {
        // 空指针没有可包的页面。
        if (page == nullptr)
        {
            return nullptr;
        }

        // 幂等：已经装过壳就返回现成的内容容器。
        QScrollArea* const existingShell = page->findChild<QScrollArea*>(
            QString::fromLatin1(kAdaptivePageScrollObjectName),
            Qt::FindDirectChildrenOnly);
        if (existingShell != nullptr && existingShell->widget() != nullptr)
        {
            return existingShell->widget();
        }

        // 页面已有布局：无法再装外壳布局，退回原行为并在调试构建里报出来。
        Q_ASSERT_X(
            page->layout() == nullptr,
            "ks::ui::EnablePageInnerScroll",
            "page already has a layout; call EnablePageInnerScroll before creating the page layout");
        if (page->layout() != nullptr)
        {
            return page;
        }

        // 页面自身只保留一个零边距的外壳布局，滚动壳独占整个页面。
        QVBoxLayout* const outerLayout = new QVBoxLayout(page);
        outerLayout->setContentsMargins(0, 0, 0, 0);
        outerLayout->setSpacing(0);

        // 滚动壳：无边框、内容可随视口缩放；高于视口时出现滚动条。
        QScrollArea* const scrollArea = new QScrollArea(page);
        scrollArea->setObjectName(QString::fromLatin1(kAdaptivePageScrollObjectName));
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidgetResizable(true);
        // 壳本身不抢键盘焦点，避免给页面多出一个无意义的 Tab 键停靠点。
        scrollArea->setFocusPolicy(Qt::NoFocus);
        // 整个方案的关键：壳对外的最小宽高贡献为 0，内容再高也不会撑大 Dock。
        IsolateMinimumSize(scrollArea);

        // 内容容器：调用方的真实布局建在它上面。
        auto* const contentWidget = new AdaptivePageCanvas(scrollArea);
        contentWidget->setObjectName(QString::fromLatin1(kAdaptivePageContentObjectName));
        scrollArea->setWidget(contentWidget);

        // QScrollArea::setWidget 会把内容容器的 autoFillBackground 置真，
        // QScrollArea 的视口默认也自动填充背景；壁纸/毛玻璃模式下它们会在页面
        // 里盖出一块实色，所以必须关回去。注意要在 setWidget 之后关，否则会被它改回来。
        contentWidget->setAutoFillBackground(false);
        scrollArea->viewport()->setAutoFillBackground(false);
        // 绑定放在独立滚动壳上，业务对content调用StyleWorkspacePage不会替换此策略。
        // 普通主宿主canvas各层同色；内容全矩形实填不改变外部卡片/页根的8px外观。
        const QPointer<AdaptivePageCanvas> canvas(contentWidget);
        BindWidgetTheme(scrollArea, [canvas]()
        {
            if (canvas != nullptr) canvas->prepareSurface();
        });

        // 滚动壳占满页面的全部空间。
        outerLayout->addWidget(scrollArea, 1);
        return contentWidget;
    }
}
