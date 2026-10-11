#include "../Framework.h"
#include "./PageControlStyle.h"
#include "./MainSurfaceStyle.h"
#include "./PageTabMotion.h"

#include <QBoxLayout>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QTimer>

namespace
{
    constexpr int kPageCornerRadius = 8; // 下沿局部凹角与已有圆角工具面共用半径，不新增侧框。

    struct PageTabColors
    {
        QColor rail; // 导航全宽底板与向下连接角的颜色。
        QColor page; // 页面底板，留给明确内容卡片更高一级的表面。
        QColor selected; // 选中页签的轻中性表面，不使用实心强调底。
        QColor hovered; // 未选中悬停时的轻变化。
    };

    // 主窗口使用工作区层级，独立窗口使用现有表面角色；子栏与父栏用不同强度。
    PageTabColors tabColors(bool mainSurface, bool child)
    {
        const ks::ui::MainSurfaceColors mainColors = ks::ui::MainWindowSurfaceColors();
        PageTabColors colors;
        colors.page = mainSurface ? mainColors.canvas : KswordTheme::WindowColor();
        // 导航与工具面必须不同色：同色补在圆外会把原工具带圆角填成方角。
        colors.rail = mainSurface ? mainColors.content : KswordTheme::SurfaceAltColor();
        colors.selected = mainSurface ? mainColors.header : KswordTheme::SurfaceAltColor();
        if (child)
        {
            // 子栏只在选中项上铺轻底面，完整底板贴近页面，避免两排导航看似同级。
            colors.rail = colors.page;
            colors.selected = mainSurface ? mainColors.toolbar : KswordTheme::SurfaceColor();
        }
        colors.hovered = KswordTheme::BlendColors(colors.rail, colors.selected, 128);
        return colors;
    }

    // selector 可为全局回退或明确属性选择器；各状态采用同一几何，避免悬停时跳动。
    QString tabBarRules(const QString& selector, const PageTabColors& colors, bool child, bool animated = false)
    {
        const QColor selectedBackgrounds[]{colors.rail, colors.hovered, colors.selected};
        // 移动底面会暂时经过未选标签，三种前景都必须在所有实际中性底面上可读。
        const QColor text = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextSecondaryColor(), selectedBackgrounds, 3, 4.5);
        const QColor hoverText = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextPrimaryColor(), selectedBackgrounds, 3, 4.5);
        const QColor selectedText = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextPrimaryColor(), selectedBackgrounds, 3, 4.5);
        const QColor marker = KswordTheme::EnsureTextContrast(
            KswordTheme::ControlAccentColor(), colors.selected, 3.0);
        // 颜色均以完整 QSS 规则一次替换，不能向样式参数中再注入未替换的占位符。
        QString rules = QStringLiteral(
            "%1{background:transparent;border:0;}"
            "%1::tab{background:transparent;color:%2;border:0;border-bottom:2px solid transparent;"
            "padding:5px 12px;min-height:18px;margin:0;"
            "border-top-left-radius:5px;border-top-right-radius:5px;}"
            "%1::tab:hover:!selected,%1::tab:pressed:!selected{background:%3;color:%4;}"
            "%1::tab:selected,%1::tab:selected:hover,%1::tab:selected:pressed{"
            "background:%5;color:%6;border-bottom-color:%7;font-weight:600;}"
            "%1::tab:disabled{color:%8;}"
            "%1::tab:left,%1::tab:right{border-bottom:0;border-left:2px solid transparent;"
            "padding:5px 8px;border-radius:5px;}"
            "%1::tab:left:selected,%1::tab:right:selected{border-left-color:%7;}"
            "%1 QToolButton{background:%9;color:%2;border:0;border-radius:5px;}"
            "%1 QToolButton:hover{background:%3;color:%4;}")
            .arg(selector, text.name(), colors.hovered.name(), hoverText.name(),
                colors.selected.name(), selectedText.name(), marker.name(),
                KswordTheme::TextDisabledColor().name(), colors.rail.name());
        if (child)
        {
            // 缩小下级页签而不增加上下空白，选中标记继续保留同一强调下边线。
            rules += QStringLiteral(
                "%1::tab{padding:3px 10px;min-height:18px;"
                "border-top-left-radius:4px;border-top-right-radius:4px;}").arg(selector);
        }
        if (animated)
        {
            // 只让出选中形状，仍由 Qt 绘制图标、文字、关闭控件、焦点与原生拖动反馈。
            rules += QStringLiteral(
                "%1::tab:selected,%1::tab:selected:hover,%1::tab:selected:pressed{"
                "background:transparent;border-bottom-color:transparent;}"
                "%1::tab:left:selected,%1::tab:right:selected{border-left-color:transparent;}").arg(selector);
        }
        return rules;
    }

    // 只沿首个可见布局项读取连接对象，不遍历全部业务控件或猜测控件名称。
    // 首项是嵌套 Tab 时由其自身连接；首项透明包装只继续沿已有布局下行。
    QWidget* leadingRoundedSurface(QLayout* layout, bool& delegated, int depth = 0)
    {
        if (!layout || depth > 8)
        {
            return nullptr;
        }
        for (int index = 0; index < layout->count(); ++index)
        {
            QLayoutItem* item = layout->itemAt(index); // 原页面的顺序，不重排或调整业务间距。
            QWidget* widget = item ? item->widget() : nullptr;
            if (widget && widget->isHidden())
            {
                continue;
            }
            if (widget)
            {
                if (qobject_cast<QTabWidget*>(widget))
                {
                    delegated = true;
                    return nullptr;
                }
                if (widget->property("ksword_primary_toolbar").toBool()
                    || widget->property("ksword_primary_content_panel").toBool()
                    || widget->property("ksword_parameter_surface").toBool())
                {
                    return widget;
                }
                return leadingRoundedSurface(widget->layout(), delegated, depth + 1);
            }
            if (item && item->layout())
            {
                return leadingRoundedSurface(item->layout(), delegated, depth + 1);
            }
        }
        return nullptr;
    }

    // 普通pane保留原页面留白；只有明确登记的框架才由pane提供等边间距。
    void applyPaneStyle(QTabWidget* tabs)
    {
        if (!tabs || tabs->property("ksword_secondary_tabs").toBool())
        {
            return;
        }
        const bool child = tabs->tabBar()->property("ksword_page_tabs_child").toBool();
        const PageTabColors colors = tabColors(ks::ui::UsesMainSurfaceHierarchy(tabs), child);
        const int inset = tabs->property("ksword_page_tabs_content_inset").toInt(); // 明确登记的等边留白。
        const QVariant mainSurface = tabs->window()->property("ksword_main_surface_hierarchy");
        const QString background = mainSurface.isValid() && !mainSurface.toBool()
            ? QStringLiteral("transparent") : (inset > 0 ? colors.rail : colors.page).name();
        const QString rules = QStringLiteral(
            "QTabWidget[ksword_page_tabs_widget=\"true\"]::pane{"
            "border:0;border-radius:%3px;top:0px;background:%1;padding:%2px;}")
            .arg(background).arg(inset).arg(inset > 0 ? 0 : kPageCornerRadius);
        const QString previous = tabs->property("ksword_page_tabs_owned_pane_style").toString();
        if (previous == rules)
        {
            return;
        }
        QString current = tabs->styleSheet();
        current.remove(previous); // 保留业务页面明确声明的专业编辑器与控件样式。
        tabs->setProperty("ksword_page_tabs_owned_pane_style", rules);
        tabs->setStyleSheet(current + rules);
    }

    // 每个角只占8px方块；在原方形框架色中挖出内容的圆角，不描边也不铺侧翼。
    class PageContentCorner final : public QWidget
    {
    public:
        explicit PageContentCorner(QTabWidget* tabs) : QWidget(tabs), m_tabs(tabs)
        {
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setAttribute(Qt::WA_NoSystemBackground);
            setAutoFillBackground(false);
            setFocusPolicy(Qt::NoFocus);
            hide();
        }

        // opening传入整个页面在Tab中的真实边界；当前角的位置由控制器设置。
        void setOpeningRectangle(const QRect& opening)
        {
            m_opening = opening.translated(-pos());
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            if (!m_tabs || m_opening.isEmpty())
            {
                return;
            }
            const bool child = m_tabs->tabBar()->property("ksword_page_tabs_child").toBool();
            const PageTabColors colors = tabColors(ks::ui::UsesMainSurfaceHierarchy(m_tabs), child);
            QPainterPath square; // 只画当前小角所在区域，内容视口不会被覆盖。
            square.addRect(QRectF(rect()));
            QPainterPath content;
            content.addRoundedRect(QRectF(m_opening), kPageCornerRadius, kPageCornerRadius);
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.rail);
            painter.drawPath(square.subtracted(content));
        }

    private:
        QPointer<QTabWidget> m_tabs; // 颜色从原宿主读取，保留主题与父子栏层级。
        QRect m_opening; // 页面边界转换到当前微型角片的局部坐标。
    };

    // 导航仅延伸两个小凹角；不再沿内容高度画侧翼，不会产生第二圈包围边框。
    class PageNavigationSurface final : public QWidget
    {
    public:
        explicit PageNavigationSurface(QTabWidget* tabs) : QWidget(tabs), m_tabs(tabs)
        {
            setProperty("ksword_page_navigation_surface", true);
            setStyleSheet(QStringLiteral(
                "QWidget[ksword_page_navigation_surface=\"true\"]{background:transparent;border:0;}"));
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setAttribute(Qt::WA_NoSystemBackground);
            setAutoFillBackground(false); // 全局 QWidget 底色不能填入挖空中心覆盖业务页面。
            setFocusPolicy(Qt::NoFocus);
        }

        // target 是实际内容组件在 tabs 内的边界，绘制时映射到这个窄连接层。
        void setConnectionRectangle(const QRect& target)
        {
            m_connectionRectangle = target;
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            if (!m_tabs)
            {
                return;
            }
            const bool child = m_tabs->tabBar()->property("ksword_page_tabs_child").toBool();
            const PageTabColors colors = tabColors(ks::ui::UsesMainSurfaceHierarchy(m_tabs), child);
            QPainter painter(this); // 本行仍贯穿导航宽度；下方只画原组件顶角外侧的差集。
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.rail);
            const int railHeight = m_tabs->tabBar()->geometry().bottom() - y() + 1;
            painter.drawRect(QRect(0, 0, width(), railHeight));
            if (m_connectionRectangle.isEmpty())
            {
                return;
            }
            const QRectF opening(m_connectionRectangle.translated(-pos())); // 与真实工具带 x/y 对齐。
            const qreal joinHeight = opening.top() + kPageCornerRadius - railHeight;
            // 只连接实际工具带宽度内的窄带；有原生小间隔时，中部也必须连续。
            QPainterPath joinBand;
            joinBand.addRect(QRectF(opening.left(), railHeight, opening.width(), joinHeight));
            QPainterPath aperture;
            aperture.addRoundedRect(opening, kPageCornerRadius, kPageCornerRadius);
            // 只补真实圆角外侧的两块负空间；内侧控件和其已有圆角不会被覆盖。
            painter.drawPath(joinBand.subtracted(aperture));
        }

    private:
        QPointer<QTabWidget> m_tabs; // 仅保存原有业务标签容器的受控寿命。
        QRect m_connectionRectangle; // 原页面首个面板或有效内容区域的真实几何。
    };

    // 控制器只同步导航本行与局部顶角，原 QStackedWidget 和页面布局保持原对象。
    class PageTabEnvelope final : public QObject
    {
    public:
        explicit PageTabEnvelope(QTabWidget* tabs)
            : QObject(tabs), m_tabs(tabs), m_surface(new PageNavigationSurface(tabs))
        {
            setObjectName(QStringLiteral("ks_page_tab_envelope"));
            for (QPointer<PageContentCorner>& corner : m_contentCorners)
            {
                corner = new PageContentCorner(tabs); // 四角独立，不新增整页覆盖窗口。
            }
            tabs->installEventFilter(this);
            tabs->tabBar()->installEventFilter(this);
            if (auto* stack = tabs->findChild<QStackedWidget*>(
                QStringLiteral("qt_tabwidget_stackedwidget"), Qt::FindDirectChildrenOnly))
            {
                stack->installEventFilter(this); // 原生 pane 布局更新后同步局部连接，不争抢几何。
            }
            connect(tabs, &QTabWidget::currentChanged, this, [this]() { schedule(); });
            schedule();
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            // 惰性创建、改字号、移动宿主与标签溢出都会改变栏几何，合并到下一轮布局。
            switch (event->type())
            {
            case QEvent::Show:
            case QEvent::Resize:
            case QEvent::Move:
            case QEvent::LayoutRequest:
            case QEvent::FontChange:
            case QEvent::StyleChange:
            case QEvent::PaletteChange:
            case QEvent::ParentChange:
            case QEvent::DynamicPropertyChange:
                schedule();
                break;
            case QEvent::Hide:
                if (watched == m_tabs && m_surface)
                {
                    m_surface->hide();
                    for (const QPointer<PageContentCorner>& corner : m_contentCorners)
                    {
                        corner->hide();
                    }
                }
                break;
            default:
                break;
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        // 只有父布局全部内容就是这个 Tab 时才收紧外层顶部，绝不动页内16px留白。
        void connectOuterLayout()
        {
            QWidget* parent = m_tabs->parentWidget();
            if (!parent || parent->isWindow())
            {
                return;
            }
            QLayout* layout = parent->layout();
            if (!layout || layout->count() != 1 || layout->itemAt(0)->widget() != m_tabs)
            {
                return;
            }
            const QMargins margins = layout->contentsMargins();
            if (margins.top() != 0)
            {
                layout->setContentsMargins(margins.left(), 0, margins.right(), margins.bottom());
            }
        }

        // 观察当前页面和实际首面板，惰性创建与布局变化后再读取真正连接边界。
        void observeContent(QWidget* page, QWidget* surface)
        {
            for (QWidget* previous : {m_observedPage.data(), m_observedSurface.data()})
            {
                if (previous && previous != page && previous != surface)
                {
                    previous->removeEventFilter(this);
                }
            }
            m_observedPage = page;
            m_observedSurface = surface;
            if (page)
            {
                page->installEventFilter(this);
            }
            if (surface && surface != page)
            {
                surface->installEventFilter(this);
            }
        }

        // 导航本行维持原宽度；只有毗邻内容时补凹角，绝不靠新 padding 做人工对齐。
        void synchronize()
        {
            if (!m_tabs || !m_surface)
            {
                return;
            }
            QTabBar* bar = m_tabs->tabBar(); // 原 QTabBar 继续负责图标、文字和所有输入。
            if (m_tabs->tabPosition() != QTabWidget::North || bar->isHidden()
                || bar->property("ksword_secondary_tabs").toBool())
            {
                m_surface->hide();
                for (const QPointer<PageContentCorner>& corner : m_contentCorners)
                {
                    corner->hide();
                }
                return;
            }
            connectOuterLayout();
            applyPaneStyle(m_tabs);
            const QRect bounds = m_tabs->contentsRect();
            QWidget* page = m_tabs->currentWidget(); // 当前页可能刚惰性构造，只读其真实布局。
            const bool contentFrame = m_tabs->property("ksword_page_tabs_content_inset").toInt() > 0;
            bool delegated = false; // 首项嵌套 Tab 已负责自身衔接，外层不叠加另一层凹角。
            QWidget* surface = page && !contentFrame ? leadingRoundedSurface(page->layout(), delegated) : nullptr;
            observeContent(page, surface);
            QRect connection;
            if (page && !delegated && !contentFrame)
            {
                const QRect effective = page->layout() ? page->layout()->contentsRect() : page->contentsRect();
                connection = surface
                    ? QRect(surface->mapTo(m_tabs, QPoint()), surface->size())
                    : effective.translated(page->mapTo(m_tabs, QPoint()));
            }
            const int railBottom = bar->geometry().bottom() + 1; // 下沿来自原栏，不是页签命中矩形。
            const bool adjoining = !connection.isEmpty()
                && connection.width() >= kPageCornerRadius * 2
                && connection.top() >= railBottom - 1
                && connection.top() <= railBottom + kPageCornerRadius;
            if (!adjoining)
            {
                connection = QRect(); // 原业务分区留白较大时保留它，不用细长侧线跨过内容。
            }
            const int bottom = adjoining ? connection.top() + kPageCornerRadius : railBottom;
            const QRect band(bounds.left(), bar->y(), bounds.width(), bottom - bar->y());
            m_surface->setGeometry(band);
            m_surface->setConnectionRectangle(connection);
            m_surface->setVisible(m_tabs->isVisible());
            m_surface->raise();
            bar->raise();
            // 等边模式以整个页面四角为准，保留Tab下方的间隔，不再只连接首工具条。
            const QRect opening = page ? QRect(page->mapTo(m_tabs, QPoint()), page->size()) : QRect();
            for (int index = 0; index < 4; ++index)
            {
                PageContentCorner* corner = m_contentCorners[index];
                const bool visible = contentFrame && page && m_tabs->isVisible()
                    && opening.width() >= kPageCornerRadius * 2 && opening.height() >= kPageCornerRadius * 2;
                corner->setVisible(visible);
                if (!visible)
                {
                    continue;
                }
                const int left = (index & 1) ? opening.right() + 1 - kPageCornerRadius : opening.left();
                const int top = (index & 2) ? opening.bottom() + 1 - kPageCornerRadius : opening.top();
                corner->setGeometry(left, top, kPageCornerRadius, kPageCornerRadius);
                corner->setOpeningRectangle(opening);
                corner->raise();
            }
            // 角落的明确工具控件保留原按钮和命中；底面不应覆盖它们。
            for (Qt::Corner corner : {Qt::TopLeftCorner, Qt::TopRightCorner})
            {
                if (QWidget* tool = m_tabs->cornerWidget(corner))
                {
                    tool->raise();
                }
            }
            m_surface->update();
        }

        void schedule()
        {
            if (m_pending)
            {
                return;
            }
            m_pending = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_pending = false;
                synchronize();
            });
        }

        QPointer<QTabWidget> m_tabs; // 页面销毁后排队几何同步自动失效。
        QPointer<PageNavigationSurface> m_surface; // 仅导航行和两个小角的局部衔接层。
        QPointer<QWidget> m_observedPage; // 当前业务页，保持原有布局和边距。
        QPointer<QWidget> m_observedSurface; // 真正连接面，随其位置和尺寸变化更新小角。
        QPointer<PageContentCorner> m_contentCorners[4]; // 只在明确等边模式下显示四个凹圆角。
        bool m_pending = false; // 同一轮字体/布局/样式事件仅安排一次同步。
    };
}

namespace ks::ui
{
    QColor PageTabRailColor(const QTabBar* bar)
    {
        const bool child = bar && bar->property("ksword_page_tabs_child").toBool();
        return tabColors(UsesMainSurfaceHierarchy(bar), child).rail;
    }

    QColor PageTabIconContrastColor(const QColor& preferred)
    {
        const PageTabColors main = tabColors(true, false); // 主窗口父导航的实际三个底面。
        const PageTabColors mainChild = tabColors(true, true); // 主窗口子导航的轻底面。
        const PageTabColors standalone = tabColors(false, false); // 独立窗口父导航。
        const PageTabColors standaloneChild = tabColors(false, true); // 独立窗口子导航。
        const QColor backgrounds[]{main.rail, main.selected, main.hovered,
            mainChild.rail, mainChild.selected, mainChild.hovered,
            standalone.rail, standalone.selected, standalone.hovered,
            standaloneChild.rail, standaloneChild.selected, standaloneChild.hovered};
        return KswordTheme::EnsureTextContrastForBackgrounds(preferred, backgrounds, 12, 3.0);
    }

    QString BuildPageTabsStyleSheet(bool mainSurface)
    {
        const PageTabColors colors = tabColors(mainSurface, false);
        return tabBarRules(QStringLiteral("QTabBar"), colors, false)
            + QStringLiteral("QTabWidget::pane{border:0;border-radius:8px;top:0px;background:%1;}")
                .arg(colors.page.name());
    }

    void StylePageTabBar(QTabBar* bar)
    {
        if (!bar || bar->property("ksword_secondary_tabs").toBool()
            || bar->property("ksword_segmented_tabs").toBool())
        {
            return;
        }
        const QPointer<QTabBar> safeBar(bar); // 动画和主题刷新不得访问销毁后的标签栏。
        InstallPageTabMotion(bar, [safeBar]()
        {
            if (!safeBar)
            {
                return PageTabMotionColors{};
            }
            const bool child = safeBar->property("ksword_page_tabs_child").toBool();
            const PageTabColors colors = tabColors(UsesMainSurfaceHierarchy(safeBar), child);
            return PageTabMotionColors{colors.selected, KswordTheme::EnsureTextContrast(
                KswordTheme::ControlAccentColor(), colors.selected, 3.0)};
        });
        if (bar->property("ksword_page_tabs_style").toBool())
        {
            const bool child = bar->property("ksword_page_tabs_child").toBool();
            bar->setStyleSheet(tabBarRules(QStringLiteral("QTabBar[ksword_page_tabs_style=\"true\"]"),
                tabColors(UsesMainSurfaceHierarchy(bar), child), child, true));
            return;
        }
        bar->setProperty("ksword_page_tabs_style", true);
        bar->setExpanding(false);
        bar->setUsesScrollButtons(true);
        BindWidgetTheme(bar, [safeBar]()
        {
            if (!safeBar || safeBar->property("ksword_secondary_tabs").toBool())
            {
                return;
            }
            const bool child = safeBar->property("ksword_page_tabs_child").toBool();
            const QString rules = tabBarRules(QStringLiteral("QTabBar[ksword_page_tabs_style=\"true\"]"),
                tabColors(UsesMainSurfaceHierarchy(safeBar), child), child, true);
            if (safeBar->styleSheet() != rules)
            {
                safeBar->setStyleSheet(rules);
            }
        });
    }

    void StylePageTabs(QTabWidget* tabs)
    {
        if (!tabs)
        {
            return;
        }
        StylePageTabBar(tabs->tabBar());
        if (tabs->property("ksword_page_tabs_widget").toBool())
        {
            return;
        }
        tabs->setProperty("ksword_page_tabs_widget", true);
        new PageTabEnvelope(tabs);
        const QPointer<QTabWidget> safeTabs(tabs); // 外壳仅在原标签容器存活时刷新。
        BindWidgetTheme(tabs, [safeTabs]()
        {
            if (!safeTabs || safeTabs->property("ksword_secondary_tabs").toBool())
            {
                return;
            }
            applyPaneStyle(safeTabs);
        });
    }

    void StyleChildPageTabs(QTabWidget* tabs)
    {
        if (!tabs)
        {
            return;
        }
        tabs->tabBar()->setProperty("ksword_page_tabs_child", true);
        tabs->setContentsMargins(8, 0, 0, 0);
        StylePageTabs(tabs);
        InstallChildTabTransition(tabs);
    }

    void StylePageTabsContentFrame(QTabWidget* tabs, int inset)
    {
        if (!tabs)
        {
            return;
        }
        // 留白只在pane登记一次；调用页的外围layout必须为0，避免两层间距累加。
        tabs->setProperty("ksword_page_tabs_content_inset", qMax(0, inset));
        StylePageTabs(tabs);
        applyPaneStyle(tabs);
    }
}
