#include "../Framework.h"
#include "./PageControlStyle.h"
#include "./MainSurfaceStyle.h"

#include <QBoxLayout>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QTimer>

namespace
{
    constexpr int kPageCornerRadius = 8; // 导航下缘与内容组件共用的圆角半径。

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
        colors.rail = mainSurface ? mainColors.toolbar : KswordTheme::SurfaceColor();
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
    QString tabBarRules(const QString& selector, const PageTabColors& colors, bool child)
    {
        const QColor text = KswordTheme::EnsureTextContrast(
            KswordTheme::TextSecondaryColor(), colors.rail, 4.5);
        const QColor hoverText = KswordTheme::EnsureTextContrast(
            KswordTheme::TextPrimaryColor(), colors.hovered, 4.5);
        const QColor selectedText = KswordTheme::EnsureTextContrast(
            KswordTheme::TextPrimaryColor(), colors.selected, 4.5);
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
        return rules;
    }

    // 全宽导航底面仅覆盖栏位及下方两处圆角，不读取页面数据，也不抢占命中区域。
    class PageNavigationSurface final : public QWidget
    {
    public:
        explicit PageNavigationSurface(QTabWidget* tabs) : QWidget(tabs), m_tabs(tabs)
        {
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setAttribute(Qt::WA_NoSystemBackground);
            setFocusPolicy(Qt::NoFocus);
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
            QPainter painter(this); // 独立底面使只有少量页签时仍有贯穿页面的导航带。
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.rail);
            const qreal bandHeight = height() - kPageCornerRadius;
            painter.drawRect(QRectF(0, 0, width(), bandHeight));

            // 内容顶角由小凹角包住；中间区域始终透明，不遮挡日志、曲线或编辑器。
            const qreal radius = std::min(qreal(kPageCornerRadius), width() / 2.0);
            QPainterPath corners;
            corners.moveTo(0, bandHeight);
            corners.lineTo(radius, bandHeight);
            corners.quadTo(0, bandHeight, 0, bandHeight + radius);
            corners.closeSubpath();
            corners.moveTo(width(), bandHeight);
            corners.lineTo(width() - radius, bandHeight);
            corners.quadTo(width(), bandHeight, width(), bandHeight + radius);
            corners.closeSubpath();
            painter.drawPath(corners);
        }

    private:
        QPointer<QTabWidget> m_tabs; // 仅保存原有业务标签容器的受控寿命。
    };

    // 控制器只同步明确登记标签的外壳几何，原 QStackedWidget 和页面布局保持原对象。
    class PageTabEnvelope final : public QObject
    {
    public:
        explicit PageTabEnvelope(QTabWidget* tabs)
            : QObject(tabs), m_tabs(tabs), m_surface(new PageNavigationSurface(tabs))
        {
            setObjectName(QStringLiteral("ks_page_tab_envelope"));
            tabs->installEventFilter(this);
            tabs->tabBar()->installEventFilter(this);
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

        // 按原栏位真实边界铺底，绘制顺序只把底面置于栏下、页面顶角之上。
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
                return;
            }
            connectOuterLayout();
            const QRect bounds = m_tabs->contentsRect();
            const QRect band(bounds.left(), bar->y(), bounds.width(), bar->height() + kPageCornerRadius);
            m_surface->setGeometry(band);
            m_surface->setVisible(m_tabs->isVisible());
            m_surface->raise();
            bar->raise();
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
        QPointer<PageNavigationSurface> m_surface; // 不透明栏与透明中心的局部衔接层。
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
        if (!bar || bar->property("ksword_secondary_tabs").toBool())
        {
            return;
        }
        if (bar->property("ksword_page_tabs_style").toBool())
        {
            const bool child = bar->property("ksword_page_tabs_child").toBool();
            bar->setStyleSheet(tabBarRules(QStringLiteral("QTabBar[ksword_page_tabs_style=\"true\"]"),
                tabColors(UsesMainSurfaceHierarchy(bar), child), child));
            return;
        }
        bar->setProperty("ksword_page_tabs_style", true);
        bar->setExpanding(false);
        bar->setUsesScrollButtons(true);
        const QPointer<QTabBar> safeBar(bar); // 主题刷新不得访问销毁后的标签栏。
        BindWidgetTheme(bar, [safeBar]()
        {
            if (!safeBar || safeBar->property("ksword_secondary_tabs").toBool())
            {
                return;
            }
            const bool child = safeBar->property("ksword_page_tabs_child").toBool();
            const QString rules = tabBarRules(QStringLiteral("QTabBar[ksword_page_tabs_style=\"true\"]"),
                tabColors(UsesMainSurfaceHierarchy(safeBar), child), child);
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
            const PageTabColors colors = tabColors(UsesMainSurfaceHierarchy(safeTabs), false);
            const QVariant mainSurface = safeTabs->window()->property("ksword_main_surface_hierarchy");
            const QString background = mainSurface.isValid() && !mainSurface.toBool()
                ? QStringLiteral("transparent") : colors.page.name();
            const QString rules = QStringLiteral(
                "QTabWidget[ksword_page_tabs_widget=\"true\"]::pane{"
                "border:0;border-radius:8px;top:0px;background:%1;}").arg(background);
            // 仅替换本组件拥有的pane规则，保留页面原有专业编辑器和布局样式。
            QString current = safeTabs->styleSheet();
            current.remove(safeTabs->property("ksword_page_tabs_owned_pane_style").toString());
            safeTabs->setProperty("ksword_page_tabs_owned_pane_style", rules);
            safeTabs->setStyleSheet(current + rules);
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
}
