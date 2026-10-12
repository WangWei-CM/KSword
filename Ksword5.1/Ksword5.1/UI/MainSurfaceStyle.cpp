#include "../Framework.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"
#include <QWidget>

namespace ks::ui
{
    MainSurfaceColors MainWindowSurfaceColors()
    {
        // 固定层级只依赖主题种子；同主题的所有按钮/表格复用同一求值结果。
        // 未应用的设置预览必须旁路缓存，不能污染生产主题的代次。
        static thread_local MainSurfaceColors cachedColors;
        static thread_local quint64 cachedGeneration = 0;
        const bool preview = KswordTheme::ActiveThemePreviewSeeds != nullptr;
        if (!preview && cachedGeneration == KswordTheme::ThemeSeedGeneration)
        {
            return cachedColors;
        }
        MainSurfaceColors colors; // 不使用固定 RGB，保留深浅模式与用户背景种子。
        colors.canvas = KswordTheme::BlendColors(
            KswordTheme::MainBackgroundColor(), KswordTheme::SurfaceColor(), 16);
        colors.content = KswordTheme::BlendColors(
            KswordTheme::MainBackgroundColor(), KswordTheme::SurfaceColor(), 175);
        // 内容、工具带和表头直接从同一组种子求值，嵌套再深也不逐层抬高明度。
        colors.alternate = KswordTheme::BlendColors(
            colors.content, KswordTheme::SurfaceAltColor(), 48);
        colors.toolbar = KswordTheme::BlendColors(
            colors.content, KswordTheme::SurfaceAltColor(), 72);
        colors.header = KswordTheme::BlendColors(
            colors.content, KswordTheme::SurfaceAltColor(), 112);
        colors.input = KswordTheme::ControlInputSurfaceColor(colors.content, colors.header);
        colors.border = KswordTheme::BlendColors(
            colors.content, KswordTheme::BorderColor(), 90);
        if (!preview)
        {
            cachedColors = colors;
            cachedGeneration = KswordTheme::ThemeSeedGeneration;
        }
        return colors;
    }

    bool UsesMainSurfaceHierarchy(const QWidget* widget)
    {
        return widget && widget->window()->property("ksword_main_surface_hierarchy").toBool();
    }

    bool UsesTransparentMainSurface(const QWidget* widget)
    {
        if (!widget) return false;
        const QVariant mode = widget->window()->property("ksword_main_surface_hierarchy");
        return mode.isValid() && !mode.toBool();
    }

    QString MainDataSurfaceStyleColor(const QColor& color, bool transparent)
    {
        // 主底板继续透出背景；数据区保留轻透衬底，避免文字直接压在任意壁纸/桌面上。
        QColor surface = color;
        if (transparent) surface.setAlpha(224);
        return surface.name(transparent ? QColor::HexArgb : QColor::HexRgb);
    }

    QString BuildMainSurfaceStyle(bool transparentContent)
    {
        const MainSurfaceColors colors = MainWindowSurfaceColors();
        if (transparentContent)
        {
            // 只为数据面补可读性，不给普通QWidget/Stack/页根铺底，不改变窗口透明度。
            const QColor backgrounds[] = {KswordTheme::BlendColors(colors.content, QColor(Qt::black), 31),
                KswordTheme::BlendColors(colors.content, QColor(Qt::white), 31)};
            const QColor text = KswordTheme::EnsureTextContrastForBackgrounds(
                KswordTheme::TextPrimaryColor(), backgrounds, 2, 4.5);
            return QStringLiteral(R"qss(
                ads--CDockWidget QTableView,ads--CDockWidget QTreeView,
                ads--CDockWidget QListView,ads--CDockWidget QTextEdit,ads--CDockWidget QPlainTextEdit,
                ads--CDockWidget QStackedWidget > QTableView,ads--CDockWidget QStackedWidget > QTreeView,
                ads--CDockWidget QStackedWidget > QListView,ads--CDockWidget QStackedWidget > QTextEdit,
                ads--CDockWidget QStackedWidget > QPlainTextEdit,ads--CDockWidget QGroupBox{
                    background:%1 !important;color:%2;alternate-background-color:%3 !important;border-radius:8px;
                })qss")
                .arg(MainDataSurfaceStyleColor(colors.content, true), text.name(),
                    MainDataSurfaceStyleColor(colors.alternate, true));
        }
        const QColor text = KswordTheme::EnsureTextContrast(
            KswordTheme::TextPrimaryColor(), colors.content, 4.5);
        // 仅为主工作区的结构容器分层，不给每个 QWidget 画底，也不接管单元格语义色。
        return QStringLiteral(
            // 最终主/浮动宿主规则也取消 ADS 外壳描边，不能只换底色留下贯穿页面的竖线。
            "ads--CDockAreaWidget{border:0 !important;}"
            "ads--CDockAreaWidget,ads--CDockWidget > QWidget{background:%1 !important;}"
            "ads--CDockWidget QTabWidget > QStackedWidget{background:%1;}"
            "ads--CDockWidget QTabWidget > QStackedWidget > QWidget{background:%1;}"
            "ads--CDockWidget QListView,ads--CDockWidget QListWidget,ads--CDockWidget QTextEdit,ads--CDockWidget QPlainTextEdit{"
            "background:%2 !important;color:%3;alternate-background-color:%4;border-radius:8px;}"
            "ads--CDockWidget QGroupBox{background:%2;border-radius:8px;}"
            "ads--CDockWidget QFrame#card,ads--CDockWidget QWidget#card{"
            "background:%2;border-color:%5;border-radius:8px;}")
            .arg(colors.canvas.name(), colors.content.name(), text.name(),
                colors.alternate.name(), colors.border.name());
    }
}
