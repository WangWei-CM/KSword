#include "../Framework.h"
#include "./PageControlStyle.h"
#include "./MainSurfaceStyle.h"
#include "./PageTabMotion.h"
#include "./PageTabFocusStyle.h"
#include "./AdaptivePageScroll.h"
#include "./PrimaryPageStyle.h"
#include "./PerfTrace.h"

#include <QBoxLayout>
#include <QEvent>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QTimer>
#include <typeinfo>

namespace
{
    constexpr int kPageCornerRadius = 8; // 内容组件已有圆角；外框再加等边间隔得到同心外弧。

    struct PageTabColors
    {
        QColor rail; // 导航全宽底板与向下连接角的颜色。
        QColor page; // 页面底板，留给明确内容卡片更高一级的表面。
        QColor selected; // 选中页签的轻中性表面，不使用实心强调底。
        QColor hovered; // 未选中悬停时的轻变化。
        bool childNavigation = false; // 父子导航使用紧凑圆角面，连体结果Tab保持原页签形状。
    };

    // 主窗口使用工作区层级，独立窗口使用现有表面角色；子栏与父栏用不同强度。
    PageTabColors tabColors(bool mainSurface, bool child, bool joined = false, bool transparent = false)
    {
        const ks::ui::MainSurfaceColors mainColors = ks::ui::MainWindowSurfaceColors();
        PageTabColors colors;
        colors.childNavigation = child && !joined;
        colors.page = mainSurface ? mainColors.canvas : KswordTheme::WindowColor();
        // 导航与工具面必须不同色：同色补在圆外会把原工具带圆角填成方角。
        colors.rail = mainSurface ? mainColors.content : KswordTheme::SurfaceAltColor();
        colors.selected = mainSurface ? mainColors.header : KswordTheme::SurfaceAltColor();
        if (child)
        {
            // 父子导航共享连续的内容层轨道，以紧凑选中面区分层级，不插入深色断层。
            colors.rail = mainSurface ? mainColors.content : KswordTheme::SurfaceAltColor();
            colors.selected = mainSurface ? mainColors.toolbar : KswordTheme::SurfaceColor();
        }
        if (joined)
        {
            // 结果页签属于上方工具区的末行，连续底面不再模拟另一层页面导航。
            colors.rail = mainSurface ? mainColors.toolbar : KswordTheme::SurfaceColor();
            colors.selected = mainSurface ? mainColors.header : KswordTheme::SurfaceAltColor();
        }
        colors.hovered = KswordTheme::BlendColors(colors.rail, colors.selected, 128);
        if (transparent)
        {
            colors.page = Qt::transparent;
            colors.rail.setAlpha(ks::ui::MainDataSurfaceAlpha());
            colors.selected.setAlpha(ks::ui::MainDataSurfaceAlpha());
            colors.hovered.setAlpha(ks::ui::MainDataSurfaceAlpha());
        }
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
            colors.childNavigation ? KswordTheme::ControlAccentColor() : KswordTheme::TextPrimaryColor(),
            selectedBackgrounds, 3, 4.5);
        const QColor disabledText = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextDisabledColor(), selectedBackgrounds, 3, 4.5); // 禁用文字仍可辨认。
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
            .arg(selector, text.name(), colors.hovered.name(QColor::HexArgb), hoverText.name(),
                colors.selected.name(QColor::HexArgb), selectedText.name(), marker.name(),
                disabledText.name(), colors.rail.name(QColor::HexArgb));
        if (child && colors.childNavigation)
        {
            // 子导航保留自然文字宽度；状态共享同一内缩与圆角，不复制父级贴边页签造型。
            rules += QStringLiteral(
                "%1::tab{padding:3px 10px;min-height:18px;margin:2px 1px;"
                "border:0;border-radius:5px;}").arg(selector);
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

    // 只识别项目自己的滚动壳，不递归查找任意子控件或改动专业编辑器内部布局。
    QWidget* pageLayoutOwner(QWidget* page)
    {
        // 只穿过调用页明确标记的惰性占位壳，避免对任意专业控件的单子项布局作猜测。
        for (int depth = 0; page && depth < 4 && page->property("ksword_page_frame_host").toBool(); ++depth)
        {
            QLayout* layout = page->layout();
            QWidget* child = layout && layout->count() == 1 ? layout->itemAt(0)->widget() : nullptr;
            if (!child)
            {
                break;
            }
            page = child;
        }
        if (!page || qobject_cast<QAbstractScrollArea*>(page))
        {
            return nullptr;
        }
        if (auto* scroll = page->findChild<QScrollArea*>(
            QString::fromLatin1(ks::ui::kAdaptivePageScrollObjectName), Qt::FindDirectChildrenOnly))
        {
            return scroll->widget();
        }
        return page;
    }

    // 只识别整个当前页交给下一组Tab的结构；工具条、splitter或并列面板不是转交。
    QTabWidget* delegatedPageTabs(QTabWidget* tabs)
    {
        QWidget* owner = pageLayoutOwner(tabs->currentWidget());
        QLayout* layout = owner ? owner->layout() : nullptr;
        auto* nested = qobject_cast<QTabWidget*>(owner); // 也支持直接把下一组Tab作为当前页。
        if (!nested && layout && layout->count() == 1)
        {
            nested = qobject_cast<QTabWidget*>(layout->itemAt(0)->widget());
        }
        return nested && nested->property("ksword_page_tabs_content_inset").toInt() > 0 ? nested : nullptr;
    }

    // 留白是独立职责，内嵌结果Tab也可保留；不能据此推断外面存在方角框架。
    int pageFrameInset(QTabWidget* tabs)
    {
        const int inset = tabs->property("ksword_page_tabs_content_inset").toInt();
        return inset > 0 && delegatedPageTabs(tabs) ? 0 : inset;
    }

    // 方框资格只由明确主导航提供，沿纯单子Tab链传递；内嵌结果区不能借祖先方框画角。
    bool hasSquareTabBoundary(QTabWidget* tabs)
    {
        for (int depth = 0; tabs && depth < 16; ++depth)
        {
            QTabWidget* outerTabs = nullptr; // 只检查最近的Tab宿主，不跨过一层失败的边界。
            for (QWidget* ancestor = tabs->parentWidget(); ancestor && !ancestor->isWindow();
                ancestor = ancestor->parentWidget())
            {
                if (ancestor->property("ksword_workspace_content_frame").toBool()
                    || ancestor->property("ksword_primary_content_panel").toBool()
                    || ancestor->property("ksword_parameter_surface").toBool())
                {
                    return false; // 已有整页或圆角卡片包住它，不能凭自己的登记再造一圈外框。
                }
                outerTabs = qobject_cast<QTabWidget*>(ancestor);
                if (outerTabs)
                {
                    break;
                }
            }
            if (!outerTabs)
            {
                return tabs->property("ksword_square_page_frame").toBool();
            }
            if (delegatedPageTabs(outerTabs) != tabs)
            {
                return false;
            }
            tabs = outerTabs;
        }
        return false;
    }

    // 独立页根挂进已有页面或圆角面板后已不是最外方框，不能再在内部叠一组外圆。
    bool hasSquareWorkspaceBoundary(QWidget* page, int inset)
    {
        if (inset <= 0 || !page->layout()
            || page->layout()->contentsMargins() != QMargins(inset, inset, inset, inset))
        {
            return false; // 被外围框架接管为零边距后，原页根不再拥有外角。
        }
        for (QWidget* ancestor = page->parentWidget(); ancestor && !ancestor->isWindow();
            ancestor = ancestor->parentWidget())
        {
            if (ancestor->property("ksword_page_tabs_content_inset").toInt() > 0
                || ancestor->property("ksword_workspace_content_frame").toBool()
                || ancestor->property("ksword_primary_content_panel").toBool()
                || ancestor->property("ksword_parameter_surface").toBool())
            {
                return false;
            }
        }
        return true;
    }

    // 普通pane保留原页面留白；只有明确登记的框架才由pane提供等边间距。
    void applyPaneStyle(QTabWidget* tabs)
    {
        if (!tabs || tabs->property("ksword_secondary_tabs").toBool())
        {
            return;
        }
        const bool child = tabs->tabBar()->property("ksword_page_tabs_child").toBool();
        const bool joined = tabs->property("ksword_joined_result_tabs").toBool(); // 连体结果区不重复内缩。
        const PageTabColors colors = tabColors(ks::ui::UsesMainSurfaceHierarchy(tabs), child, joined, ks::ui::UsesTransparentMainSurface(tabs));
        const int inset = pageFrameInset(tabs); // 只计最内层明确登记的留白。
        const QVariant mainSurface = tabs->window()->property("ksword_main_surface_hierarchy");
        bool parameterCanvas = false; // 参数卡片内的Tab不再新增更暗的页根底板。
        for (QWidget* owner = tabs->parentWidget(); owner && !owner->isWindow(); owner = owner->parentWidget())
        {
            if (owner->property("ksword_parameter_surface").toBool())
            {
                parameterCanvas = true;
                break;
            }
        }
        const QColor pageColor = parameterCanvas ? ks::ui::MainWindowSurfaceColors().content : colors.page;
        const QString background = mainSurface.isValid() && !mainSurface.toBool()
            ? QStringLiteral("transparent") : pageColor.name();
        QString rules = QStringLiteral(
            "QTabWidget[ksword_page_tabs_widget=\"true\"]::pane{"
            "border:0;border-radius:%3px;top:0px;background:%1;padding:%2px;}")
            .arg(background).arg(inset).arg(inset > 0 || joined ? 0 : kPageCornerRadius);
        if (joined || (tabs->property("ksword_standard_tabs_gap").toBool()
            && !child && delegatedPageTabs(tabs) == nullptr))
        {
            // 结果区保留6px接缝；直接承接子导航的父Tab不额外叠加这一层间隔。
            // 不恢复左右/底部的pane内缩，结果外边界仍与整条Tab栏对齐。
            QString separationEdge = QStringLiteral("top"); // 间隔跟随标签所在边，不改标签栏本身。
            switch (tabs->tabPosition())
            {
            case QTabWidget::South:
                separationEdge = QStringLiteral("bottom");
                break;
            case QTabWidget::West:
                separationEdge = QStringLiteral("left");
                break;
            case QTabWidget::East:
                separationEdge = QStringLiteral("right");
                break;
            default:
                break;
            }
            rules += QStringLiteral(
                "QTabWidget[ksword_page_tabs_widget=\"true\"]::pane{padding-%1:6px;}")
                .arg(separationEdge);
        }
        if (child && tabs->tabPosition() == QTabWidget::North)
        {
            // 标签起点对齐共享6px内容留白，层级缩进不改变整页和结果组件的外边界。
            rules += QStringLiteral("QTabWidget[ksword_page_tabs_widget=\"true\"]::tab-bar{left:6px;}");
        }
        const QString previous = tabs->property("ksword_page_tabs_owned_pane_style").toString();
        if (previous == rules && tabs->styleSheet().contains(rules))
        {
            return;
        }
        QString current = tabs->styleSheet();
        current.remove(previous); // 保留业务页面明确声明的专业编辑器与控件样式。
        tabs->setProperty("ksword_page_tabs_owned_pane_style", rules);
        tabs->setStyleSheet(current + rules);
    }

    // 角片只修整最外层框架；外半径=内部圆角半径+间隔，内外圆心重合以保持等宽。
    class PageContentCorner final : public QWidget
    {
    public:
        explicit PageContentCorner(QWidget* host) : QWidget(host), m_tabs(host)
        {
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setAttribute(Qt::WA_NoSystemBackground);
            setAutoFillBackground(false);
            setFocusPolicy(Qt::NoFocus);
            hide();
        }

        // opening为包含四边留白的外框，radius为外半径；内部控件的圆角不在此重画。
        void setOpeningRectangle(const QRect& opening, int radius)
        {
            const QRect localOpening = opening.translated(-pos()); // 只在角片局部形状实际变化时重画。
            if (m_opening == localOpening && m_radius == radius)
            {
                return;
            }
            m_opening = localOpening;
            m_radius = radius;
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            if (!m_tabs || m_opening.isEmpty())
            {
                return;
            }
            // 外框层级与父子栏强调程度分开，子栏也保留清楚的同心外弧。
            const PageTabColors colors = tabColors(ks::ui::UsesMainSurfaceHierarchy(m_tabs), false, false, ks::ui::UsesTransparentMainSurface(m_tabs));
            QPainterPath square; // 只填外框圆弧之外的角落，保留内部原有圆角和留白。
            square.addRect(QRectF(rect()));
            QPainterPath content;
            content.addRoundedRect(QRectF(m_opening), m_radius, m_radius);
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.rail);
            painter.drawPath(square.subtracted(content));
        }

    private:
        QPointer<QWidget> m_tabs; // 颜色从原宿主读取，可用于Tab或无内层Tab的页面。
        QRect m_opening; // 最外层框架边界转换到当前角片的局部坐标。
        int m_radius = kPageCornerRadius; // 与内部圆角同心的外框半径。
    };

    // 服务/欢迎等没有内容Tab的整页使用同一个外框几何，仅登记页根，不处理内部卡片。
    class WorkspaceFrame final : public QObject
    {
    public:
        WorkspaceFrame(QWidget* page, int inset) : QObject(page), m_page(page), m_inset(inset)
        {
            for (QPointer<PageContentCorner>& corner : m_corners)
            {
                corner = new PageContentCorner(page);
            }
            page->installEventFilter(this);
            schedule();
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            const QPointer<QObject> source(watched); // 同步overlay通知若销毁宿主，不能继续向旧对象派发。
            // Show先放好现有四角；Paint兜底仅更新overlay，不改变页面布局。
            switch (event->type())
            {
            case QEvent::Resize:
            case QEvent::Move:
            case QEvent::LayoutRequest:
            case QEvent::Hide:
                schedule();
                break;
            case QEvent::Show:
                m_surfaceDirty = true;
                refreshNow();
                break;
            case QEvent::Paint:
                if (m_geometryDirty) refreshNow();
                break;
            case QEvent::StyleChange:
            case QEvent::ParentChange:
            case QEvent::PaletteChange:
            case QEvent::ApplicationPaletteChange:
            case QEvent::DynamicPropertyChange:
                m_surfaceDirty = true; // 主题变化即使没有几何变化，也须刷新四角底色。
                schedule();
                break;
            default:
                break;
            }
            return source.isNull();
        }

    private:
        void refreshNow()
        {
            ++m_scheduleGeneration; // 同步消费后撤销旧pending，保留后来通知的新代次。
            m_pending = false;
            refreshCorners();
        }

        void schedule()
        {
            m_geometryDirty = true;
            if (m_pending) return;
            m_pending = true;
            const quint64 generation = ++m_scheduleGeneration;
            QTimer::singleShot(0, this, [this, generation]()
            {
                if (generation != m_scheduleGeneration) return;
                m_pending = false;
                refreshCorners();
            });
        }

        void refreshCorners()
        {
            if (!m_page) return;
            const QPointer<WorkspaceFrame> lifetime(this);
            m_geometryDirty = false;
            const bool repaintSurface = m_surfaceDirty; // 普通重复布局通知不强制重绘相同角片。
            m_surfaceDirty = false;
            const QRect outer = m_page->rect(); // 页根就是最外层边界，不以内侧内容定位。
            const int radius = kPageCornerRadius + m_inset;
            // 透明只改变底面合成，不改变最外方框的资格及14/8/6同心几何。
            const bool visible = hasSquareWorkspaceBoundary(m_page, m_inset)
                && m_page->isVisible()
                && outer.width() >= radius * 2 && outer.height() >= radius * 2;
            for (int index = 0; index < 4; ++index)
            {
                const QPointer<PageContentCorner> corner = m_corners[index];
                if (!corner) continue;
                const int left = (index & 1) ? outer.width() - radius : 0;
                const int top = (index & 2) ? outer.height() - radius : 0;
                if (visible)
                {
                    corner->setGeometry(left, top, radius, radius);
                    if (!lifetime || !corner) return;
                    corner->setOpeningRectangle(outer, radius);
                }
                corner->setVisible(visible);
                if (!lifetime || !corner) return;
                if (!visible) continue;
                corner->raise();
                if (!lifetime || !corner || !m_page) return;
                if (repaintSurface) corner->update();
            }
        }

        QPointer<QWidget> m_page; // 原页面拥有控制器与四个微型角片。
        int m_inset = 6; // 与此页面根布局一致的外围间距。
        QPointer<PageContentCorner> m_corners[4]; // 不创建整页透明覆盖窗口。
        bool m_pending = false; // 单次合并更新标记。
        bool m_surfaceDirty = true; // 首次显示或主题变化才额外请求角片重画。
        bool m_geometryDirty = true; // Paint仅补已存在角片的位置和可见性。
        quint64 m_scheduleGeneration = 0; // 同步消费后，排队的旧工作不会重复刷新。
    };

    // 导航底面只占标签栏，内容四角完全由外框角片负责，避免旧的首工具条补色残留。
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

    protected:
        void paintEvent(QPaintEvent*) override
        {
            if (!m_tabs)
            {
                return;
            }
            const bool child = m_tabs->tabBar()->property("ksword_page_tabs_child").toBool();
            const PageTabColors colors = tabColors(ks::ui::UsesMainSurfaceHierarchy(m_tabs), child,
                m_tabs->property("ksword_joined_result_tabs").toBool(), ks::ui::UsesTransparentMainSurface(m_tabs));
            QPainter painter(this); // 水平/竖向导航均使用控制器给出的真实栏范围。
            painter.fillRect(rect(), colors.rail);
        }

    private:
        QPointer<QTabWidget> m_tabs; // 仅保存原有业务标签容器的受控寿命。
    };

    // 明确登记的页面由此统一外围留白，原Stack、控件和业务分区保持原对象。
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
            connect(tabs, &QTabWidget::currentChanged, this, [this](int index)
            {
#if KSWORD_PERF_TRACE_ENABLED
                if (m_tabs && index == m_tabs->currentIndex()) beginNavigation(index);
#else
                Q_UNUSED(index);
#endif
                if (canPreparePage()) prepareNow(); // 首次页或真实dirty才整理结构，热切只同步现存外壳。
                else schedule();
            });
#if KSWORD_PERF_TRACE_ENABLED
            beginNavigation(tabs->currentIndex(), "initial_state");
#endif
            schedule();
        }

#if KSWORD_PERF_TRACE_ENABLED
        ~PageTabEnvelope() override
        {
            finishNavigation("owner_destroyed");
        }
#endif

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            const QPointer<QObject> source(watched); // 同步样式/布局可能销毁当前宿主。
            if (m_preparing) return source.isNull(); // 本次pane/边距准备引起的通知由末尾同步统一消费。
            if (event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildRemoved)
            {
                // ChildRemoved also arrives while Qt tears down a page/layout.
                // QPointer can still be live then; currentWidget() may already
                // reach a partially destroyed QStackedLayout. Defer the lookup.
                invalidateLayout(watched);
                m_surfaceDirty = true;
                if (m_tabs && m_tabs->isVisible()) schedule();
                return source.isNull();
            }
            QWidget* contentPage = m_contentPages.value(qobject_cast<QWidget*>(watched)).data();
            const bool hiddenContent = contentPage && (!m_tabs || contentPage != m_tabs->currentWidget());
            // 只观察已访问页及其明确外层宿主；隐藏页结构失效保留，几何通知不安排后台工作。
            switch (event->type())
            {
            case QEvent::Resize:
            case QEvent::Move:
                if (!hiddenContent) schedule();
                break;
            case QEvent::FontChange:
            case QEvent::ApplicationFontChange:
                invalidateLayout(watched);
                if (!hiddenContent) schedule();
                break;
            case QEvent::LayoutRequest:
                if (hiddenContent) break;
                if (canPreparePage() && needsLayoutPreparation()) prepareNow();
                else schedule();
                break;
            case QEvent::Show:
                if (hiddenContent) break;
                m_surfaceDirty = true;
                if (canPreparePage()
                    && (watched == m_tabs || watched == m_tabs->currentWidget()
                        || watched == m_observedSurface))
                {
                    prepareNow(); // dirty当前页在首绘前准备；已处理热页只更新角片与栏底面。
                }
                else schedule();
                break;
            case QEvent::Paint:
                if (hiddenContent) break;
                if (m_geometryDirty)
                {
                    if (!m_layoutDirty)
                    {
                        ++m_scheduleGeneration; // 仅几何工作已消费，不让旧队列重复刷新。
                        m_pending = false;
                    }
                    synchronizeSurfaces(); // Paint绝不写业务layout或QSS。
                }
                break;
            case QEvent::DynamicPropertyChange:
                if (!isLayoutProperty(static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()))
                {
                    break; // 测量pending、业务状态及本组件自己的QSS缓存不使布局失效。
                }
                [[fallthrough]];
            case QEvent::StyleChange:
            case QEvent::PaletteChange:
            case QEvent::ApplicationPaletteChange:
            case QEvent::ParentChange:
                invalidateLayout(watched);
                if (!hiddenContent)
                {
                    m_surfaceDirty = true;
                    schedule();
                }
                break;
            case QEvent::Hide:
                if (watched == m_tabs && m_surface)
                {
                    const QPointer<PageTabEnvelope> lifetime(this); // Hide可同步关闭宿主或切换当前页。
                    const QPointer<QWidget> page(m_tabs->currentWidget());
                    m_surface->hide();
                    if (!lifetime || !retainCurrentPage(page)) return source.isNull();
                    for (int index = 0; index < 4; ++index)
                    {
                        const QPointer<PageContentCorner> corner = m_contentCorners[index];
                        if (!corner) continue;
                        corner->hide();
                        if (!lifetime || !retainCurrentPage(page)) return source.isNull();
                    }
                }
                break;
            default:
                break;
            }
            return source.isNull();
        }

    private:
#if KSWORD_PERF_TRACE_ENABLED
        void finishNavigation(const char* outcome)
        {
            if (!m_navigation.id) return;
            ks::ui::RecordPerfNavigationStage(m_navigation, "end", "tab_envelope", outcome);
            m_navigation = {};
            m_navigationPage.clear();
        }

        void beginNavigation(int index, const char* origin = nullptr)
        {
            finishNavigation("superseded_before_shell_ready");
            if (!m_tabs || index < 0 || !m_tabs->widget(index)) return;
            m_navigationPage = m_tabs->widget(index);
            m_navigation = ks::ui::BeginPerfNavigationTrace("page_tab",
                ks::ui::CapturePerfNavigationTarget(m_tabs, m_navigationPage, index), "selected", origin);
        }

        void recordNavigationPreparation()
        {
            if (!m_navigation.id || !m_navigationPage || !hasVisibleFramePage()
                || m_tabs->currentWidget() != m_navigationPage
                || m_tabs->currentIndex() != m_navigation.target.index
                || m_preparing || needsLayoutPreparation()) return;
            // Actual existing layout/pane/surface work has returned for this
            // same target. This says nothing about its business data or paint.
            ks::ui::RecordPerfNavigationStage(m_navigation, "ready",
                "tab_envelope_layout_and_surfaces", "shell_prepared");
            finishNavigation("shell_prepared");
        }
#endif

        struct PreparedPage
        {
            QPointer<QWidget> owner;
            QPointer<QLayout> layout;
            QPointer<QWidget> pageParent;
            QPointer<QWidget> ownerParent;
            QLayout::SizeConstraint constraint = QLayout::SetDefaultConstraint;
            quint64 revision = 0;
            bool dirty = true;
        };

        static bool isLayoutProperty(const QByteArray& name)
        {
            return name == "ksword_secondary_tabs" || name == "ksword_page_tabs_child"
                || name == "ksword_joined_result_tabs" || name == "ksword_standard_tabs_gap"
                || name == "ksword_page_tabs_content_inset" || name == "ksword_square_page_frame"
                || name == "ksword_workspace_content_frame" || name == "ksword_primary_content_panel"
                || name == "ksword_parameter_surface" || name == "ksword_page_frame_host"
                || name == "ksword_main_surface_hierarchy";
        }

        void invalidateLayout(QObject* source)
        {
            QWidget* page = m_contentPages.value(qobject_cast<QWidget*>(source)).data();
            if (page)
            {
                const auto prepared = m_preparedPages.find(page);
                if (prepared != m_preparedPages.end()) prepared->dirty = true;
            }
            else
            {
                ++m_layoutRevision; // 标签容器/栏的主题、字体或挂载归属改变，所有缓存页需重新消费。
                m_layoutDirty = true;
            }
        }

        // 几何/Show/Hide/ZOrderChange可同步换页；旧页快照不能继续覆盖新页的外角。
        bool retainCurrentPage(const QPointer<QWidget>& page)
        {
            if (!m_tabs || !m_surface) return false;
            if (m_tabs->currentWidget() == page.data()) return true;
            m_layoutDirty = true;
            m_surfaceDirty = true;
            schedule(); // 新页按最新归属整理，不继续消费已失效的角片坐标。
            return false;
        }

        bool hasVisibleFramePage() const
        {
            return m_tabs && m_tabs->isVisible() && m_tabs->currentWidget()
                && !m_tabs->currentWidget()->isHidden() && !m_tabs->tabBar()->isHidden()
                && !m_tabs->tabBar()->property("ksword_secondary_tabs").toBool();
        }

        bool canPreparePage() const
        {
            // 原生Popup继续排队，不在Qt遍历其内部child的Show栈重入样式或布局。
            return hasVisibleFramePage() && m_tabs->window()->windowType() != Qt::Popup;
        }

        bool needsLayoutPreparation() const
        {
            QWidget* page = m_tabs->currentWidget();
            QWidget* owner = pageLayoutOwner(page);
            QLayout* layout = owner ? owner->layout() : nullptr;
            const auto prepared = m_preparedPages.constFind(page);
            return m_layoutDirty || prepared == m_preparedPages.cend() || prepared->dirty
                || prepared->revision != m_layoutRevision || prepared->owner != owner
                || prepared->layout != layout || prepared->pageParent != page->parentWidget()
                || prepared->ownerParent != (owner ? owner->parentWidget() : nullptr)
                || (layout && prepared->constraint != layout->sizeConstraint())
                || m_preparedInset != pageFrameInset(m_tabs)
                || m_preparedDelegation != (delegatedPageTabs(m_tabs) != nullptr)
                || (layout && ownsPageMargin(owner)
                    && layout->contentsMargins() != QMargins());
        }

        // 连体模式只整理普通页壳；专业控件即使没有Q_OBJECT，也不能把其内部布局当成外围。
        bool ownsPageMargin(QWidget* owner) const
        {
            if (!owner)
            {
                return false;
            }
            if (m_tabs->property("ksword_joined_result_tabs").toBool())
            {
                return typeid(*owner) == typeid(QWidget)
                    && !owner->property("ksword_primary_content_panel").toBool()
                    && !owner->property("ksword_parameter_surface").toBool();
            }
            return m_tabs->property("ksword_page_tabs_content_inset").toInt() > 0;
        }

        void prepareNow()
        {
            if (m_preparing) return;
            ++m_scheduleGeneration; // 同步消费后，旧回调不能清理后来的新pending。
            m_pending = false;
            // Show本身不使结构失效；真实结构/主题请求和宿主签名决定是否同步准备。
            synchronize();
        }

        // 登记框架只接管当前页最外层layout；卡片/表单内部16px等业务留白不动。
        void normalizePageMargin(QWidget* owner)
        {
            if (!ownsPageMargin(owner))
            {
                return;
            }
            QLayout* layout = owner->layout();
            if (!layout)
            {
                return;
            }
            if (layout->contentsMargins() != QMargins())
            {
                layout->setContentsMargins(0, 0, 0, 0);
            }
        }

        // 观察当前页及其明确滚动内容；惰性页面建好布局后再消除重复的外围间距。
        void observeContent(QWidget* page, QWidget* surface)
        {
            m_observedPage = page;
            m_observedSurface = surface;
            for (QWidget* widget : {page, surface})
            {
                if (!widget) continue;
                if (!m_contentPages.contains(widget))
                {
                    widget->installEventFilter(this);
                    connect(widget, &QObject::destroyed, this, [this, widget]()
                    {
                        m_preparedPages.remove(widget);
                        for (auto entry = m_contentPages.begin(); entry != m_contentPages.end();)
                        {
                            if (entry.key() == widget || !entry.value())
                            {
                                if (entry.key() != widget) entry.key()->removeEventFilter(this);
                                entry = m_contentPages.erase(entry);
                            }
                            else ++entry;
                        }
                    });
                }
                m_contentPages.insert(widget, page);
            }
        }

        // 仅有效当前页Show/布局之后处理留白和pane；隐藏或半构造页不在此改布局。
        void synchronize(bool fromQueue = false)
        {
            if (!m_tabs || !m_surface || m_preparing)
            {
                return;
            }
#if KSWORD_PERF_TRACE_ENABLED
            const QPointer<PageTabEnvelope> navigationLifetime(this);
#endif
            const bool visiblePage = hasVisibleFramePage();
            if ((canPreparePage() || (fromQueue && visiblePage)) && needsLayoutPreparation())
            {
                const QPointer<PageTabEnvelope> lifetime(this);
                const QPointer<QWidget> page(m_tabs->currentWidget());
                const QPointer<QWidget> owner(pageLayoutOwner(page));
                m_preparing = true;
                observeContent(page, owner);
                normalizePageMargin(owner);
                if (!lifetime || !m_tabs) return;
                applyPaneStyle(m_tabs);
                if (!lifetime || !m_tabs) return;
                if (m_tabs->layout()) m_tabs->layout()->activate();
                if (!lifetime || !m_tabs) return;
                if (owner && owner->layout()) owner->layout()->activate();
                if (!lifetime || !m_tabs) return;
                // 最后一页被同步销毁后，空指针相等不代表仍是原准备对象。
                if (!page || m_tabs->currentWidget() != page || pageLayoutOwner(page) != owner)
                {
                    m_preparing = false;
                    m_layoutDirty = true;
                    schedule(); // 真正换页/换惰性内容后，不能把旧归属标成新页面已准备。
                    return;
                }
                PreparedPage prepared;
                prepared.owner = owner;
                prepared.layout = owner ? owner->layout() : nullptr;
                prepared.pageParent = page->parentWidget();
                prepared.ownerParent = owner ? owner->parentWidget() : nullptr;
                prepared.constraint = owner && owner->layout()
                    ? owner->layout()->sizeConstraint() : QLayout::SetDefaultConstraint;
                prepared.revision = m_layoutRevision;
                prepared.dirty = false;
                m_preparedPages.insert(page, prepared);
                m_preparedInset = pageFrameInset(m_tabs);
                m_preparedDelegation = delegatedPageTabs(m_tabs) != nullptr;
                m_layoutDirty = false;
                m_preparing = false;
            }
            else if (visiblePage)
            {
                observeContent(m_tabs->currentWidget(), pageLayoutOwner(m_tabs->currentWidget()));
            }
            synchronizeSurfaces();
#if KSWORD_PERF_TRACE_ENABLED
            if (navigationLifetime) recordNavigationPreparation();
#endif
        }

        // 只摆放既有导航底面及四角，可在firstPaint兜底；绝不写layout、pane或创建覆盖层。
        void synchronizeSurfaces()
        {
            if (!m_tabs || !m_surface) return;
            const QPointer<PageTabEnvelope> lifetime(this);
            m_geometryDirty = false;
            const bool repaintSurface = m_surfaceDirty; // 本轮只在主题或宿主变化时强制重画底面。
            m_surfaceDirty = false;
            const QPointer<QTabBar> bar(m_tabs->tabBar()); // 原 QTabBar 继续负责图标、文字和所有输入。
            const QPointer<QWidget> page(m_tabs->currentWidget()); // 页面在外部几何回调中可能被删除。
            if (bar->isHidden() || bar->property("ksword_secondary_tabs").toBool())
            {
                m_surface->hide();
                if (!lifetime || !retainCurrentPage(page)) return;
                for (int index = 0; index < 4; ++index)
                {
                    const QPointer<PageContentCorner> corner = m_contentCorners[index];
                    if (!corner) continue;
                    corner->hide();
                    if (!lifetime || !retainCurrentPage(page)) return;
                }
                return;
            }
            const QRect bounds = m_tabs->contentsRect();
            const bool vertical = m_tabs->tabPosition() == QTabWidget::West
                || m_tabs->tabPosition() == QTabWidget::East;
            const QRect band = vertical
                ? QRect(bar->x(), bounds.top(), bar->width(), bounds.height())
                : QRect(bounds.left(), bar->y(), bounds.width(), bar->height());
            const bool bandChanged = m_surface->geometry() != band;
            if (bandChanged) m_surface->setGeometry(band);
            if (!lifetime || !m_surface || !bar || !retainCurrentPage(page)) return;
            m_surface->setVisible(m_tabs->isVisible());
            if (!lifetime || !m_surface || !bar || !retainCurrentPage(page)) return;
            m_surface->raise();
            if (!lifetime || !m_surface || !bar || !retainCurrentPage(page)) return;
            bar->raise();
            if (!lifetime || !m_surface || !bar || !retainCurrentPage(page)) return;
            // 页面已被pane内缩；向外还原这层间隔，角片必须放在外框而不是页面控件上。
            const int inset = pageFrameInset(m_tabs);
            // 透明模式也保留最外层过渡，颜色仍由导航底面角色提供，不铺内部内容或新增描边。
            const bool squareBoundary = hasSquareTabBoundary(m_tabs); // 有留白不等于外面有方框。
            const int outerRadius = kPageCornerRadius + inset; // 8+6=14，圆角处也保持6px径向间隔。
            const QRect opening = page ? QRect(page->mapTo(m_tabs, QPoint()), page->size())
                .adjusted(-inset, -inset, inset, inset) : QRect();
            for (int index = 0; index < 4; ++index)
            {
                const QPointer<PageContentCorner> corner = m_contentCorners[index];
                const bool visible = squareBoundary && inset > 0 && page && m_tabs->isVisible()
                    && opening.width() >= outerRadius * 2 && opening.height() >= outerRadius * 2;
                if (!corner) continue;
                if (!visible)
                {
                    corner->hide();
                    if (!lifetime || !retainCurrentPage(page)) return;
                    continue;
                }
                const int left = (index & 1) ? opening.right() + 1 - outerRadius : opening.left();
                const int top = (index & 2) ? opening.bottom() + 1 - outerRadius : opening.top();
                corner->setGeometry(left, top, outerRadius, outerRadius);
                if (!lifetime || !corner || !retainCurrentPage(page)) return;
                corner->setOpeningRectangle(opening, outerRadius);
                corner->show(); // 几何先就绪，再允许现有角片首次绘制。
                if (!lifetime || !corner || !retainCurrentPage(page)) return;
                corner->raise();
                if (!lifetime || !corner || !retainCurrentPage(page)) return;
                if (repaintSurface) corner->update();
            }
            // 角落的明确工具控件保留原按钮和命中；底面不应覆盖它们。
            for (Qt::Corner corner : {Qt::TopLeftCorner, Qt::TopRightCorner})
            {
                if (QWidget* tool = m_tabs->cornerWidget(corner))
                {
                    tool->raise();
                    if (!lifetime || !m_surface || !retainCurrentPage(page)) return;
                }
            }
            if (repaintSurface || bandChanged) m_surface->update();
        }

        void schedule()
        {
            m_geometryDirty = true;
            if (m_pending || m_preparing)
            {
                return;
            }
            m_pending = true;
            const quint64 generation = ++m_scheduleGeneration;
            QTimer::singleShot(0, this, [this, generation]()
            {
                if (generation != m_scheduleGeneration) return;
                m_pending = false;
                synchronize(true); // Popup在排队安全点仍处理既有布局，不在Show栈同步处理。
            });
        }

        QPointer<QTabWidget> m_tabs; // 页面销毁后排队几何同步自动失效。
        QPointer<PageNavigationSurface> m_surface; // 仅覆盖横/竖导航栏。
        QPointer<QWidget> m_observedPage; // 当前业务页，保留控件和布局身份。
        QPointer<QWidget> m_observedSurface; // 原页面或明确的自适应滚动内容。
        QPointer<PageContentCorner> m_contentCorners[4]; // 等边模式的最外层四角，不覆盖内部组件圆角。
        bool m_pending = false; // 同一轮字体/布局/样式事件仅安排一次同步。
        bool m_surfaceDirty = true; // 保留主题刷新，普通同几何同步不重复重画底面。
        bool m_geometryDirty = true; // Paint仅处理现有overlay的几何。
        bool m_layoutDirty = true; // 真正布局/主题工作不被Paint兜底错误消费。
        bool m_preparing = false; // 自身pane/layout事件不回投新的同步或排队任务。
        QHash<QWidget*, PreparedPage> m_preparedPages; // 每个已访问页保存外层布局契约，A→B→A不重做准备。
        QHash<QWidget*, QPointer<QWidget>> m_contentPages; // 明确页/滚动宿主到页的身份映射，不扫描控件树。
        quint64 m_layoutRevision = 0; // 共享主题、字体和Tab挂载失效代次。
        int m_preparedInset = -1; // pane属于整个Tab，切到不同留白关系仍须更新一次。
        bool m_preparedDelegation = false; // 纯子Tab转交与普通页的pane接缝不同。
        quint64 m_scheduleGeneration = 0; // 同步已消费的旧回调自动失效。
#if KSWORD_PERF_TRACE_ENABLED
        ks::ui::PerfNavigationTrace m_navigation;
        QPointer<QWidget> m_navigationPage;
#endif
    };
}

namespace ks::ui
{
    QColor PageTabRailColor(const QTabBar* bar)
    {
        const bool child = bar && bar->property("ksword_page_tabs_child").toBool();
        return tabColors(UsesMainSurfaceHierarchy(bar), child,
            bar && bar->property("ksword_joined_result_tabs").toBool(), ks::ui::UsesTransparentMainSurface(bar)).rail;
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
        InstallMouseAwareTabFocus(bar); // 鼠标激活页签不显示键盘焦点矩形。
        const QPointer<QTabBar> safeBar(bar); // 动画和主题刷新不得访问销毁后的标签栏。
        InstallPageTabMotion(bar, [safeBar]()
        {
            if (!safeBar)
            {
                return PageTabMotionColors{};
            }
            const bool child = safeBar->property("ksword_page_tabs_child").toBool();
            const PageTabColors colors = tabColors(UsesMainSurfaceHierarchy(safeBar), child,
                safeBar->property("ksword_joined_result_tabs").toBool(), ks::ui::UsesTransparentMainSurface(safeBar));
            return PageTabMotionColors{colors.selected, KswordTheme::EnsureTextContrast(
                KswordTheme::ControlAccentColor(), colors.selected, 3.0)};
        });
        if (bar->property("ksword_page_tabs_style").toBool())
        {
            const bool child = bar->property("ksword_page_tabs_child").toBool();
            const QString rules = tabBarRules(QStringLiteral("QTabBar[ksword_page_tabs_style=\"true\"]"),
                tabColors(UsesMainSurfaceHierarchy(bar), child,
                    bar->property("ksword_joined_result_tabs").toBool(), ks::ui::UsesTransparentMainSurface(bar)), child, true);
            if (bar->styleSheet() != rules)
            {
                bar->setStyleSheet(rules); // 重复明确接入不再触发整栏polish和外壳StyleChange。
            }
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
                tabColors(UsesMainSurfaceHierarchy(safeBar), child,
                    safeBar->property("ksword_joined_result_tabs").toBool(), ks::ui::UsesTransparentMainSurface(safeBar)), child, true);
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
        tabs->setContentsMargins(0, 0, 0, 0); // 子栏缩进由tab-bar子控件承担，内容四边保持等宽。
        StylePageTabs(tabs);
        InstallChildTabTransition(tabs);
    }

    void StylePageTabsContentFrame(QTabWidget* tabs, int inset)
    {
        if (!tabs)
        {
            return;
        }
        // 明确登记后由pane接管一次外围留白；当前页及惰性页的最外层layout由控制器归零。
        tabs->setProperty("ksword_page_tabs_content_inset", qMax(0, inset));
        StylePageTabs(tabs);
        applyPaneStyle(tabs);
    }

    void StyleSquarePageTabsFrame(QTabWidget* tabs, int inset)
    {
        if (!tabs)
        {
            return;
        }
        // 仅真正直接承接方形工作区的主导航调用；普通结果Tab不声明这项资格。
        tabs->setProperty("ksword_square_page_frame", true);
        StylePageTabsContentFrame(tabs, inset);
    }

    void StyleJoinedResultTabs(QTabWidget* tabs, QWidget* header)
    {
        if (!tabs)
        {
            return;
        }
        // 调用页明确决定上方组件与结果区的布局接缝，此处只统一形状、颜色和结果宽度。
        tabs->setProperty("ksword_joined_result_tabs", true);
        tabs->tabBar()->setProperty("ksword_joined_result_tabs", true);
        tabs->setProperty("ksword_square_page_frame", false);
        if (header)
        {
            header->setProperty("ksword_primary_toolbar_joined_tabs", true);
            StylePrimaryToolbar(header); // 工具带只保留顶部外圆角，底部与页签连续。
        }
        StylePageTabsContentFrame(tabs, 0);
    }

    void StyleWorkspaceContentFrame(QWidget* page, int inset)
    {
        if (!page || page->property("ksword_workspace_content_frame").toBool()) return;
        page->setProperty("ksword_workspace_content_frame", true);
        const int spacing = qMax(0, inset); // 页面明确选定的等边间隔，只登记一次。
        if (page->layout()) page->layout()->setContentsMargins(spacing, spacing, spacing, spacing);
        new WorkspaceFrame(page, spacing);
    }
}
