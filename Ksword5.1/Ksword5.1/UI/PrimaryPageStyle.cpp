#include "../Framework.h"
#include "./PrimaryPageStyle.h"
#include "./ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"

#include <QComboBox>
#include <QAbstractSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QBoxLayout>
#include <QFont>

namespace
{
    // 各控件只替换自己拥有的样式片段，避免丢掉页面既有几何规则。
    void ReplacePrimaryStyle(QWidget* widget, const QString& rules)
    {
        const QString begin = QStringLiteral("/* ks_primary_begin */");
        const QString end = QStringLiteral("/* ks_primary_end */");
        QString style = widget->styleSheet(); // 当前页面专用样式保留在拥有块之外。
        const qsizetype first = style.indexOf(begin);
        const qsizetype last = first < 0 ? -1 : style.indexOf(end, first);
        if (first >= 0 && last >= first)
        {
            style.remove(first, last + end.size() - first);
        }
        const QString ownedBlock = begin + rules + end; // 仅本模块的容器/输入规则，不拥有按钮颜色。
        widget->setProperty("ksword_primary_style_block", ownedBlock);
        style = style.trimmed() + QLatin1Char('\n') + ownedBlock;
        if (style != widget->styleSheet())
        {
            widget->setStyleSheet(style);
        }
    }
}

namespace ks::ui
{
    void StyleStandaloneInput(QWidget* input)
    {
        if (!input || (!qobject_cast<QComboBox*>(input)
            && !qobject_cast<QAbstractSpinBox*>(input) && !qobject_cast<QLineEdit*>(input)))
        {
            return;
        }
        // 内部编辑器属于外层输入面，单独加圆角会产生重复底块和接缝。
        if (qobject_cast<QLineEdit*>(input)
            && (qobject_cast<QComboBox*>(input->parentWidget())
                || qobject_cast<QAbstractSpinBox*>(input->parentWidget())))
        {
            return;
        }
        input->setProperty("ksword_standalone_input", true);
        const QString geometry = QStringLiteral(
            "\nQComboBox[ksword_standalone_input=\"true\"],"
            "QAbstractSpinBox[ksword_standalone_input=\"true\"],"
            "QLineEdit[ksword_standalone_input=\"true\"]{border-radius:5px;}"
            "QAbstractSpinBox[ksword_standalone_input=\"true\"]::up-button{border-top-right-radius:4px;}"
            "QAbstractSpinBox[ksword_standalone_input=\"true\"]::down-button{border-bottom-right-radius:4px;}");
        if (!input->styleSheet().contains(geometry))
        {
            input->setStyleSheet(input->styleSheet() + geometry);
        }
    }

    void StylePrimaryCombo(QComboBox* combo)
    {
        if (combo == nullptr || combo->property("ksword_primary_combo").toBool())
        {
            return;
        }
        combo->setProperty("ksword_primary_combo", true);
        NormalizeToolbarControl(combo);
        const QPointer<QComboBox> target(combo); // 页面销毁后取消延迟主题刷新。
        BindWidgetTheme(combo, [target]()
        {
            if (target)
            {
                // 控件自身的完整状态规则优先于 Dock 透明祖先，热切换不留下旧色快照。
                ReplacePrimaryStyle(target, KswordTheme::ThemedComboBoxStyle());
                if (target->property("ksword_standalone_input").toBool())
                {
                    StyleStandaloneInput(target);
                }
            }
        });
    }

    void StylePrimaryGroup(QGroupBox* group)
    {
        if (group == nullptr || group->property("ksword_primary_group").toBool())
        {
            return;
        }
        group->setProperty("ksword_primary_group", true);
        const QPointer<QGroupBox> target(group);
        BindWidgetTheme(group, [target]()
        {
            if (!target)
            {
                return;
            }
            const QColor surface = UsesMainSurfaceHierarchy(target)
                ? MainWindowSurfaceColors().content : KswordTheme::SurfaceColor();
            const QColor title = KswordTheme::EnsureTextContrast(
                KswordTheme::TextSecondaryColor(), surface, 4.5);
            const QColor disabled = KswordTheme::EnsureTextContrast(
                KswordTheme::TextDisabledColor(), surface, 3.0);
            // 参数工作区用标题与细分隔线表达分区，不在两栏面板内重复堆大边框。
            if (target->property("ksword_parameter_section").toBool())
            {
                const bool hasTitle = !target->title().isEmpty();
                const QColor separator = KswordTheme::BlendColors(surface, KswordTheme::BorderColor(), 70);
                ReplacePrimaryStyle(target, QStringLiteral(
                    "QGroupBox[ksword_parameter_section=\"true\"]{background:transparent;"
                    "border:0;border-top:%4px solid %1;border-radius:0;margin-top:%5px;padding-top:0;}"
                    "QGroupBox[ksword_parameter_section=\"true\"]::title{"
                    "subcontrol-origin:margin;subcontrol-position:top left;left:0;"
                    "padding:0 0 5px 0;background:transparent;color:%2;font-weight:600;}"
                    "QGroupBox[ksword_parameter_section=\"true\"]::title:disabled{color:%3;}")
                    .arg(separator.name(), title.name(), disabled.name())
                    .arg(hasTitle ? 1 : 0).arg(hasTitle ? 18 : 0));
                return;
            }
            // 仅命中此分组自身，嵌套业务控件、表格语义底色及选中态保持原所有权。
            ReplacePrimaryStyle(target, QStringLiteral(
                "QGroupBox[ksword_primary_group=\"true\"]{background:%1;"
                "border:1px solid %2;border-radius:6px;margin-top:10px;padding-top:6px;}"
                "QGroupBox[ksword_primary_group=\"true\"]::title{"
                "subcontrol-origin:margin;subcontrol-position:top left;left:10px;"
                "padding:0 5px;background:%1;color:%3;font-weight:600;}"
                "QGroupBox[ksword_primary_group=\"true\"]::title:disabled{color:%4;}")
                .arg(surface.name(), KswordTheme::BorderColor().name(), title.name(), disabled.name()));
        });
    }

    void StylePrimaryToolbar(QWidget* toolbar)
    {
        if (toolbar == nullptr || toolbar->property("ksword_primary_toolbar").toBool())
        {
            return;
        }
        toolbar->setProperty("ksword_primary_toolbar", true);
        toolbar->setAttribute(Qt::WA_StyledBackground, true);
        const QPointer<QWidget> target(toolbar);
        BindWidgetTheme(toolbar, [target]()
        {
            if (target)
            {
                const bool mainSurface = UsesMainSurfaceHierarchy(target);
                const MainSurfaceColors surfaces = MainWindowSurfaceColors();
                // 工具带只画底面和一条下边界，不把所有按钮包成重复卡片。
                ReplacePrimaryStyle(target, QStringLiteral(
                    "QWidget[ksword_primary_toolbar=\"true\"]{"
                    "background:%1;border:0;border-bottom:1px solid %2;}")
                    .arg(mainSurface ? surfaces.toolbar.name() : KswordTheme::SurfaceColor().name(),
                        mainSurface ? surfaces.border.name() : KswordTheme::BorderColor().name()));
            }
        });
    }

    void StylePrimaryContentPanel(QWidget* panel, bool outline)
    {
        if (!panel)
        {
            return;
        }
        const bool changed = panel->property("ksword_primary_content_panel_outlined").isValid()
            && panel->property("ksword_primary_content_panel_outlined").toBool() != outline;
        panel->setProperty("ksword_primary_content_panel_outlined", outline);
        if (panel->property("ksword_primary_content_panel").toBool())
        {
            if (changed) RefreshWidgetThemeBindings();
            return;
        }
        panel->setProperty("ksword_primary_content_panel", true);
        panel->setAttribute(Qt::WA_StyledBackground, true);
        const QPointer<QWidget> target(panel); // 沿用原面板对象，图表采样和绘图逻辑不变。
        BindWidgetTheme(panel, [target]()
        {
            if (!target)
            {
                return;
            }
            const bool mainSurface = UsesMainSurfaceHierarchy(target);
            const MainSurfaceColors surfaces = MainWindowSurfaceColors();
            const bool outlined = target->property("ksword_primary_content_panel_outlined").toBool();
            const QString background = mainSurface ? surfaces.content.name()
                : outlined ? QStringLiteral("transparent") : KswordTheme::SurfaceColor().name();
            const QString rules = outlined ? QStringLiteral(
                "QWidget[ksword_primary_content_panel=\"true\"]{background:%1;border:1px solid %2;border-radius:6px;}")
                    .arg(background, mainSurface ? surfaces.border.name() : KswordTheme::BorderColor().name())
                : QStringLiteral(
                    "QWidget[ksword_primary_content_panel=\"true\"]{background:%1;border:0;border-radius:6px;}").arg(background);
            ReplacePrimaryStyle(target, rules);
        });
    }

    void StyleWorkspacePage(QWidget* page, int margin)
    {
        if (!page) return;
        page->setProperty("ksword_workspace_page", true);
        page->setAttribute(Qt::WA_StyledBackground);
        QFont body = page->font();
        if (body.pixelSize() > 0) body.setPixelSize(qMax(14, body.pixelSize()));
        else body.setPointSizeF(qMax(10.0, body.pointSizeF()));
        page->setFont(body);
        if (page->layout())
        {
            page->layout()->setContentsMargins(margin, margin, margin, margin);
            if (auto* box = qobject_cast<QBoxLayout*>(page->layout())) box->setSpacing(12);
        }
        const QPointer<QWidget> target(page);
        BindWidgetTheme(page, [target]()
        {
            if (!target) return;
            const QColor canvas = UsesMainSurfaceHierarchy(target)
                ? MainWindowSurfaceColors().canvas : KswordTheme::WindowColor();
            ReplacePrimaryStyle(target, QStringLiteral(
                "QWidget[ksword_workspace_page=\"true\"]{background:%1;border:0;}").arg(canvas.name()));
        });
    }

    void StylePrimarySectionTitle(QLabel* title)
    {
        if (title == nullptr || title->property("ksword_primary_title").toBool())
        {
            return;
        }
        title->setProperty("ksword_primary_title", true);
        const QPointer<QLabel> target(title);
        BindWidgetTheme(title, [target]()
        {
            if (target)
            {
                // 标题既可能位于工具带，也可能直接位于页面，两个背景都保持可读。
                const bool mainSurface = UsesMainSurfaceHierarchy(target);
                const MainSurfaceColors surfaces = MainWindowSurfaceColors();
                const QColor backgrounds[] = {
                    mainSurface ? surfaces.canvas : KswordTheme::WindowColor(),
                    mainSurface ? surfaces.toolbar : KswordTheme::SurfaceColor()};
                const QColor text = KswordTheme::EnsureTextContrastForBackgrounds(
                    KswordTheme::TextSecondaryColor(), backgrounds, 2, 4.5);
                ReplacePrimaryStyle(target, QStringLiteral(
                    "QLabel[ksword_primary_title=\"true\"]{"
                    "background:transparent;color:%1;font-weight:600;}").arg(text.name()));
            }
        });
    }
}
