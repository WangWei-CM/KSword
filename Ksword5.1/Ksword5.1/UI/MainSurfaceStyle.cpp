#include "../Framework.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"
#include <QWidget>

namespace ks::ui
{
    MainSurfaceColors MainWindowSurfaceColors()
    {
        MainSurfaceColors colors; // 不使用固定 RGB，保留深浅模式与用户背景种子。
        colors.canvas = KswordTheme::BlendColors(
            KswordTheme::MainBackgroundColor(), KswordTheme::SurfaceColor(), 48);
        colors.content = KswordTheme::BlendColors(
            KswordTheme::SurfaceColor(), KswordTheme::SurfaceAltColor(), 48);
        colors.alternate = KswordTheme::BlendColors(
            colors.content, KswordTheme::SurfaceAltColor(), 80);
        colors.toolbar = KswordTheme::BlendColors(
            colors.content, KswordTheme::SurfaceAltColor(), 140);
        colors.header = KswordTheme::BlendColors(
            KswordTheme::SurfaceAltColor(), KswordTheme::SurfaceMutedColor(), 160);
        colors.border = KswordTheme::BlendColors(
            colors.content, KswordTheme::BorderColor(), 140);
        return colors;
    }

    bool UsesMainSurfaceHierarchy(const QWidget* widget)
    {
        return widget && widget->window()->property("ksword_main_surface_hierarchy").toBool();
    }

    QString BuildMainSurfaceStyle(bool transparentContent)
    {
        if (transparentContent)
        {
            return {}; // 不用不透明数据面覆盖用户启用的背景图/穿透策略。
        }
        const MainSurfaceColors colors = MainWindowSurfaceColors();
        const QColor text = KswordTheme::EnsureTextContrast(
            KswordTheme::TextPrimaryColor(), colors.content, 4.5);
        // 仅为主工作区的结构容器分层，不给每个 QWidget 画底，也不接管单元格语义色。
        return QStringLiteral(
            "ads--CDockAreaWidget,ads--CDockWidget > QWidget,ads--CDockWidget QTabWidget::pane{background:%1 !important;}"
            "ads--CDockWidget QListView,ads--CDockWidget QListWidget,ads--CDockWidget QTextEdit,ads--CDockWidget QPlainTextEdit{"
            "background:%2 !important;color:%3;alternate-background-color:%4;}"
            "ads--CDockWidget QFrame#card,ads--CDockWidget QWidget#card{"
            "background:%2;border-color:%5;}")
            .arg(colors.canvas.name(), colors.content.name(), text.name(),
                colors.alternate.name(), colors.border.name());
    }
}
