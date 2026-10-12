#include "../Framework.h"
#include "./StandardWidgetTheme.h"
#include "./FlatButtonTheme.h"
#include "./GlobalUiBaseStyle.h"
#include "./MainSurfaceStyle.h"
#include "./PageControlStyle.h"
#include "./ParameterWorkspace.h"
#include "./PrimaryPageStyle.h"
#include "./ThemeBinding.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QLineEdit>
#include <QGroupBox>
#include <QPainter>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QScreen>
#include <QTimer>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

#include <algorithm>

namespace
{
    // 不把组合框内部编辑器、表格单元格编辑器和专业视口当作独立表单控件。
    bool isControlInterior(const QWidget* widget)
    {
        for (const QWidget* current = widget; current; current = current->parentWidget())
        {
            if (current->property(ks::ui::StandardThemeExemptProperty).toBool()
                || current->property("ksword_detail_keep_layout").toBool())
            {
                return true;
            }
            if (current != widget && (qobject_cast<const QAbstractSpinBox*>(current)
                || qobject_cast<const QComboBox*>(current)
                || qobject_cast<const QAbstractItemView*>(current)))
            {
                return true;
            }
            if (current->isWindow()) break;
        }
        return false;
    }

    // 默认度量由Qt样式查询提供，页面显式尺寸仍优先；不遍历或重新排列业务layout。
    class StandardControlStyle final : public QProxyStyle
    {
    public:
        explicit StandardControlStyle(QStyle* base) : QProxyStyle(base) {}

        int pixelMetric(PixelMetric metric, const QStyleOption* option,
            const QWidget* widget) const override
        {
            if (!widget || !isControlInterior(widget))
            {
                switch (metric)
                {
                case PM_LayoutHorizontalSpacing:
                case PM_LayoutVerticalSpacing:
                    return 8;
                case PM_LayoutLeftMargin:
                case PM_LayoutTopMargin:
                case PM_LayoutRightMargin:
                case PM_LayoutBottomMargin:
                    return 6;
                default:
                    break;
                }
            }
            return QProxyStyle::pixelMetric(metric, option, widget);
        }

        QSize sizeFromContents(ContentsType type, const QStyleOption* option,
            const QSize& contents, const QWidget* widget) const override
        {
            QSize result = QProxyStyle::sizeFromContents(type, option, contents, widget);
            if (!widget || isControlInterior(widget)
                || widget->minimumHeight() == widget->maximumHeight())
            {
                return result;
            }
            const int height = std::max(28, QFontMetrics(widget->font()).height() + 12);
            if (widget->maximumHeight() < height)
            {
                return result; // 标题栏/编辑器等明确紧凑尺寸优先，不用全局min-height撑大它们。
            }
            switch (type)
            {
            case CT_LineEdit:
            case CT_SpinBox:
            case CT_ComboBox:
            case CT_PushButton:
                result.setHeight(std::max(result.height(), height));
                break;
            case CT_ToolButton:
                if (const auto* tool = qobject_cast<const QToolButton*>(widget);
                    tool && !tool->autoRaise())
                {
                    result.setHeight(std::max(result.height(), height));
                }
                break;
            default:
                break;
            }
            return result;
        }
    };

    // 标准容器只拥有自己的底板；剔除完整已登记块之后仍有QSS即视为业务自有外观。
    bool acceptsDefaultBinding(QWidget* widget)
    {
        if (ks::ui::HasWidgetThemeBinding(widget) || isControlInterior(widget)) return false;
        for (QWidget* owner = widget; owner; owner = owner->parentWidget())
        {
            if (owner != widget && owner->isWindow()
                && owner->property("ksword_main_surface_hierarchy").isValid())
            {
                break; // 主窗口/浮动宿主的公共基线不是某个控件的业务配色。
            }
            QString style = owner->styleSheet();
            for (const char* blockName : {"ksword_primary_style_block",
                "ksword_parameter_surface_style_block", "ksword_secondary_style_block",
                "ksword_dock_root_surface_style_block", "ksword_page_tabs_owned_pane_style"})
            {
                const QString block = owner->property(blockName).toString();
                if (!block.isEmpty()) style.remove(block);
            }
            if (!style.trimmed().isEmpty()) return false;
            if (owner->isWindow()) break;
        }
        return true;
    }

    // 使用现有组合框配方自动绑定颜色，不调用会覆盖显式高度的工具行归一函数。
    void bindDefaultCombo(QComboBox* combo)
    {
        const QPointer<QComboBox> target(combo);
        ks::ui::BindWidgetTheme(combo, [target]()
        {
            if (!target) return;
            const QString previous = target->property("ksword_default_combo_owned_style").toString();
            if (target->styleSheet() != previous) return; // 后设的专业样式有自己的拥有者。
            const QColor surface = ks::ui::UsesMainSurfaceHierarchy(target)
                ? ks::ui::MainWindowSurfaceColors().input : KswordTheme::ControlInputSurfaceColor();
            // QSS的min-height指内容高度；保留2px上下padding后，实际单行高度必须仍一致。
            const int height = std::max(28, QFontMetrics(target->font()).height() + 12);
            QString style = KswordTheme::ThemedComboBoxStyle(surface);
            if (target->maximumHeight() >= height
                && target->minimumHeight() != target->maximumHeight())
            {
                style += QStringLiteral("QComboBox{min-height:%1px;}").arg(height - 4);
            }
            target->setProperty("ksword_default_combo_owned_style", style);
            if (target && target->styleSheet() != style) target->setStyleSheet(style);
        });
    }

    // 复用原QProgressBar的数据、信号与可访问性，只将绘制改为细轨道＋独立数值。
    class StandardProgressSurface final : public QObject
    {
    public:
        explicit StandardProgressSurface(QProgressBar* bar) : QObject(bar), m_bar(bar)
        {
            bar->setProperty("ksword_standard_progress", true);
            bar->installEventFilter(this);
            m_elapsed.start();
            connect(&m_timer, &QTimer::timeout, this, [this]()
            {
                if (m_bar) m_bar->update();
            });
            const QPointer<QProgressBar> target(bar);
            ks::ui::BindWidgetTheme(bar, [target]()
            {
                if (!target) return;
                const QString style = QStringLiteral(
                    "QProgressBar[ksword_standard_progress=\"true\"]{"
                    "background:transparent;border:0;padding:0;}");
                if (target->styleSheet() != style) target->setStyleSheet(style);
                target->update();
            });
        }

    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (!m_bar) return false;
            if (event->type() == QEvent::Hide) m_timer.stop();
            if (event->type() != QEvent::Paint) return false;
            // 只有可见的忙进度动画需要刷新；确定进度只随业务value更新重绘。
            const bool busy = m_bar->minimum() == 0 && m_bar->maximum() == 0;
            BOOL animations = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
            if (busy && animations && m_bar->isVisible())
            {
                const qreal rate = m_bar->screen() ? m_bar->screen()->refreshRate() : 60.0;
                const int interval = qBound(8, qRound(1000.0 / qMax(30.0, rate)), 33);
                if (!m_timer.isActive() || m_timer.interval() != interval) m_timer.start(interval);
            }
            else m_timer.stop();

            QPainter painter(m_bar);
            painter.setRenderHint(QPainter::Antialiasing);
            const bool horizontal = m_bar->orientation() == Qt::Horizontal;
            QRectF area = m_bar->rect().adjusted(1, 1, -1, -1);
            const QString text = !busy && m_bar->isTextVisible() ? m_bar->text() : QString();
            const int textWidth = text.isEmpty() ? 0 : QFontMetrics(m_bar->font()).horizontalAdvance(text) + 12;
            QRectF label = area;
            if (horizontal && !text.isEmpty() && textWidth < area.width())
            {
                label.setLeft(area.right() - textWidth);
                area.setRight(label.left() - 8);
            }
            else if (!horizontal && !text.isEmpty())
            {
                label.setTop(area.bottom() - QFontMetrics(m_bar->font()).height());
                area.setBottom(label.top() - 8);
            }
            QRectF track = horizontal
                ? QRectF(area.left(), area.center().y() - 3, qMax(0.0, area.width()), 6)
                : QRectF(area.center().x() - 3, area.top(), 6, area.height());
            const QColor surface = ks::ui::UsesMainSurfaceHierarchy(m_bar)
                ? ks::ui::MainWindowSurfaceColors().content : KswordTheme::SurfaceColor();
            const QColor trail = KswordTheme::BlendColors(surface, KswordTheme::TextSecondaryColor(), 25);
            const QColor accent = m_bar->isEnabled() ? KswordTheme::ControlAccentColor()
                : KswordTheme::TextDisabledColor();
            painter.setPen(Qt::NoPen);
            painter.setBrush(trail);
            painter.drawRoundedRect(track, 3, 3);
            const double span = double(m_bar->maximum()) - double(m_bar->minimum());
            const qreal fraction = busy ? 0.25 : (span > 0
                ? qBound(0.0, (double(m_bar->value()) - m_bar->minimum()) / span, 1.0) : 0.0);
            QRectF fill = track;
            const bool reversed = m_bar->invertedAppearance()
                != (horizontal && m_bar->layoutDirection() == Qt::RightToLeft);
            if (horizontal)
            {
                fill.setWidth(track.width() * fraction);
                if (reversed) fill.moveRight(track.right());
            }
            else
            {
                fill.setHeight(track.height() * fraction);
                if (!reversed) fill.moveBottom(track.bottom());
            }
            if (busy)
            {
                const qreal phase = animations ? (m_elapsed.elapsed() % 1600) / 1600.0 : 0.5;
                if (horizontal) fill.moveLeft(track.left() + (track.width() - fill.width()) * phase);
                else fill.moveTop(track.top() + (track.height() - fill.height()) * phase);
            }
            painter.setBrush(accent);
            if (fraction > 0) painter.drawRoundedRect(fill, 3, 3);
            // 数值始终位于填充条之外，避免短进度值被亮蓝色矩形抢走视觉重点。
            if (!text.isEmpty())
            {
                painter.setPen(KswordTheme::EnsureTextContrast(m_bar->isEnabled()
                    ? KswordTheme::TextPrimaryColor() : KswordTheme::TextDisabledColor(), surface, 4.5));
                painter.drawText(label, Qt::AlignCenter, text);
            }
            return true;
        }

    private:
        QPointer<QProgressBar> m_bar; // 保留原业务控件，不替换其对象或信号。
        QTimer m_timer; // 忙状态可见时使用，静态/隐藏状态停止。
        QElapsedTimer m_elapsed; // 使用经过时间计算动画位置，避免按帧累加漂移。
    };

    // QSS接管输入框后可能绕过基础style的sizeFromContents，补默认最小高度而不锁死尺寸。
    void applyDefaultHeight(const QPointer<QWidget>& widget)
    {
        const QVariant owned = widget->property("ksword_standard_min_height");
        const int previous = owned.isValid() ? owned.toInt() : 0;
        if (widget->minimumHeight() != previous || widget->minimumHeight() == widget->maximumHeight()) return;
        const int height = std::max(28, QFontMetrics(widget->font()).height() + 12);
        if (widget->maximumHeight() < height) return; // 已明确的紧凑高度有优先权。
        widget->setProperty("ksword_standard_min_height", height);
        if (widget && widget->minimumHeight() != height) widget->setMinimumHeight(height);
    }

    // 只处理收到生命周期事件的控件，既不扫描全树，也不提前创建隐藏Popup/业务页。
    class DefaultControlBindings final : public QObject
    {
    public:
        explicit DefaultControlBindings(QApplication* app) : QObject(app) {}

    protected:
        bool eventFilter(QObject* object, QEvent* event) override
        {
            if (!event || (event->type() != QEvent::Polish && event->type() != QEvent::Show
                && event->type() != QEvent::FontChange))
            {
                return false;
            }
            const QPointer<QWidget> widget(qobject_cast<QWidget*>(object));
            if (!widget || m_enrolling) return false;
            if (event->type() == QEvent::FontChange)
            {
                if (widget->property("ksword_standard_min_height").isValid())
                {
                    m_enrolling = true;
                    applyDefaultHeight(widget);
                    m_enrolling = false;
                }
                return widget.isNull();
            }
            auto* button = qobject_cast<QAbstractButton*>(widget.data());
            auto* field = qobject_cast<QLineEdit*>(widget.data());
            auto* spin = qobject_cast<QAbstractSpinBox*>(widget.data());
            auto* combo = qobject_cast<QComboBox*>(widget.data());
            auto* bar = qobject_cast<QTabBar*>(widget.data());
            auto* slider = qobject_cast<QSlider*>(widget.data());
            auto* group = qobject_cast<QGroupBox*>(widget.data());
            auto* progress = qobject_cast<QProgressBar*>(widget.data());
            auto* tabs = qobject_cast<QTabWidget*>(widget.data());
            const bool action = button && (qobject_cast<QPushButton*>(button)
                || qobject_cast<QToolButton*>(button));
            if (!action && !field && !spin && !combo && !bar && !slider && !group && !progress && !tabs) return false;
            if (!acceptsDefaultBinding(widget)) return false;
            if (widget->window()->windowType() == Qt::Popup
                || widget->window()->windowType() == Qt::ToolTip) return false;

            m_enrolling = true; // 绑定产生的Polish/Show只由原路径继续处理，避免嵌套重复登记。
            widget->setProperty("ksword_standard_control", true); // 几何基线只命中默认接管的独立控件。
            if (!widget)
            {
                m_enrolling = false;
                return true;
            }
            const auto* tool = qobject_cast<QToolButton*>(button);
            const auto* push = qobject_cast<QPushButton*>(button);
            if (field || spin || combo || (push && !push->isFlat())
                || (tool && !tool->autoRaise()) || (slider && slider->orientation() == Qt::Horizontal))
            {
                applyDefaultHeight(widget);
            }
            if (!widget)
            {
                m_enrolling = false;
                return true;
            }
            if (action && !button->property("ksword_segmented_button").toBool()
                && !button->inherits("QLineEditIconButton"))
            {
                ks::ui::ApplyFlatButtonTheme(button);
            }
            else if (field)
            {
                ks::ui::BindLineEditTheme(field);
            }
            else if (spin)
            {
                ks::ui::BindSpinBoxTheme(spin);
            }
            else if (combo)
            {
                bindDefaultCombo(combo);
            }
            else if (group)
            {
                bool parameterSection = false;
                for (QWidget* parent = group->parentWidget(); parent && !parent->isWindow(); parent = parent->parentWidget())
                {
                    if (parent->property("ksword_parameter_body").toBool())
                    {
                        parameterSection = true;
                        break;
                    }
                }
                if (parameterSection) ks::ui::StyleParameterSection(group);
                else ks::ui::StylePrimaryGroup(group);
            }
            else if (progress)
            {
                new StandardProgressSurface(progress);
            }
            else if (tabs && !tabs->property("ksword_page_tabs_widget").toBool()
                && !tabs->property("ksword_secondary_tabs").toBool())
            {
                // 普通Tab保留6px接缝，但不猜测它与上方组件存在父子或连体语义。
                tabs->setProperty("ksword_standard_tabs_gap", true);
                ks::ui::StylePageTabs(tabs);
            }
            else if (bar && qobject_cast<QTabWidget*>(bar->parentWidget())
                && !bar->property("ksword_page_tabs_style").toBool()
                && !bar->property("ksword_secondary_tabs").toBool())
            {
                ks::ui::StylePageTabBar(bar); // 自动普通导航外观/动画，不猜父子/结果页的布局语义。
            }
            m_enrolling = false;
            return widget.isNull(); // 共享回调可能关闭窗口；已删除的对象不再接收原事件。
        }

    private:
        bool m_enrolling = false; // GUI线程内的重入保护，不持有控件或业务对象。
    };
}

void ks::ui::InstallStandardWidgetTheme(QApplication* application)
{
    if (!application || application->property("ksword_standard_widget_theme_installed").toBool()) return;
    application->setProperty("ksword_standard_widget_theme_installed", true);
    // 固定跨平台基础绘制，不再让遗漏局部QSS的新控件退回Windows原始立体外观。
    if (QStyle* base = QStyleFactory::create(QString::fromLatin1("fusion")))
    {
        auto* standard = new StandardControlStyle(base);
        standard->setObjectName(QStringLiteral("fusion")); // 保留依赖QStyleFactory重建基础style的兼容路径。
        application->setStyle(standard);
    }
    // 启动期尚未构建MainWindow，原生标签/复选框也必须与默认控件底色成对可读。
    QPalette colors = application->palette();
    colors.setColor(QPalette::Window, KswordTheme::MainBackgroundColor());
    colors.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
    colors.setColor(QPalette::Base, KswordTheme::SurfaceColor());
    colors.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
    colors.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
    colors.setColor(QPalette::Button, KswordTheme::SurfaceAltColor());
    colors.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
    colors.setColor(QPalette::Highlight, KswordTheme::ControlAccentColor());
    colors.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
    colors.setColor(QPalette::PlaceholderText, KswordTheme::TextSecondaryColor());
    colors.setColor(QPalette::Disabled, QPalette::WindowText, KswordTheme::TextDisabledColor());
    colors.setColor(QPalette::Disabled, QPalette::Text, KswordTheme::TextDisabledColor());
    colors.setColor(QPalette::Disabled, QPalette::ButtonText, KswordTheme::TextDisabledColor());
    application->setPalette(colors);
    // 首个业务窗口创建前已有基线；外观设置刷新仍由原标记替换链统一接管。
    application->setStyleSheet(application->styleSheet() + BuildGlobalBaseControlStyleBlock());
    application->installEventFilter(new DefaultControlBindings(application));
}
