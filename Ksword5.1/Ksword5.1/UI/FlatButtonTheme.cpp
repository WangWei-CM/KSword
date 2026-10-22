#include "./FlatButtonTheme.h"
#include "ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QHash>
#include <QEvent>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVariant>
#include <QStringView>
#include <QStyle>
#include <memory>
#include <array>

namespace
{
    // 标记只描述本模块拥有的颜色规则，页面原有几何和其它控件样式原样保留。
    constexpr char beginMarker[] = "/*KSWORD_FLAT_BUTTON_BEGIN*/";
    constexpr char endMarker[] = "/*KSWORD_FLAT_BUTTON_END*/";

    struct ButtonOwnershipStyle
    {
        QString rawStyle;
        bool toolbarControl = false;
        bool toolbarIconOnly = false;
        std::array<QString, 4> surfaceBlocks;
        QString colorStyle;
        int tone = -1;
        int appearance = -1;
    };

    QHash<const QWidget*, ButtonOwnershipStyle>& buttonOwnershipStyles()
    {
        static QHash<const QWidget*, ButtonOwnershipStyle> styles;
        return styles;
    }

    // 只缓存按实际局部QSS/完整拥有块得出的结果，不缓存祖先结论。
    // 每次查询仍读取真实输入，reparent或同QSS下角色块属性变化也不会漏失效。
    const ButtonOwnershipStyle& buttonOwnershipStyle(const QWidget* owner)
    {
        const QString rawStyle = owner->styleSheet();
        const bool toolbarControl = owner->property("ksword_toolbar_control").toBool();
        const bool toolbarIconOnly = owner->property("ksword_toolbar_icon_only").toBool();
        const std::array<QString, 4> blocks{
            owner->property("ksword_primary_style_block").toString(),
            owner->property("ksword_parameter_surface_style_block").toString(),
            owner->property("ksword_secondary_style_block").toString(),
            owner->property("ksword_dock_root_surface_style_block").toString()};
        auto& styles = buttonOwnershipStyles();
        auto found = styles.find(owner);
        if (found == styles.end())
        {
            found = styles.insert(owner, ButtonOwnershipStyle{});
            QObject::connect(owner, &QObject::destroyed, qApp, [owner]()
            {
                buttonOwnershipStyles().remove(owner);
            });
        }
        else if (found->rawStyle == rawStyle && found->toolbarControl == toolbarControl
            && found->toolbarIconOnly == toolbarIconOnly && found->surfaceBlocks == blocks)
        {
            return found.value();
        }
        auto& result = found.value();
        result.rawStyle = rawStyle;
        result.toolbarControl = toolbarControl;
        result.toolbarIconOnly = toolbarIconOnly;
        result.surfaceBlocks = blocks;
        QString style = rawStyle;
        if (toolbarControl) style.remove(ks::ui::ToolbarControlGeometryStyle().trimmed());
        if (toolbarIconOnly) style.remove(ks::ui::ToolbarIconGeometryStyle().trimmed());
        // 仍只排除精确登记的容器/几何块，未知业务QSS不能穿透到更远祖先。
        for (const QString& block : blocks)
        {
            if (!block.isEmpty()) style.remove(block);
        }
        result.colorStyle = style.trimmed();
        result.tone = -1;
        result.appearance = -1;
        qsizetype lastTone = -1;
        qsizetype lastAppearance = -1;
        for (int candidate = 0; candidate <= 3; ++candidate)
        {
            const qsizetype position = result.colorStyle.lastIndexOf(
                QStringLiteral("/*KSWORD_FLAT_BUTTON_TONE_%1*/").arg(candidate));
            if (position > lastTone)
            {
                lastTone = position;
                result.tone = candidate;
            }
        }
        for (int candidate = 0; candidate <= static_cast<int>(ks::ui::FlatButtonAppearance::Navigation); ++candidate)
        {
            const qsizetype position = result.colorStyle.lastIndexOf(
                QStringLiteral("/*KSWORD_FLAT_BUTTON_APPEARANCE_%1*/").arg(candidate));
            if (position > lastAppearance)
            {
                lastAppearance = position;
                result.appearance = candidate;
            }
        }
        return result;
    }

    QString buttonColorOwnershipStyle(const QWidget* owner)
    {
        return buttonOwnershipStyle(owner).colorStyle;
    }

    QString replaceOwnedBlock(QString style, const QString& replacement)
    {
        if (style.isEmpty()) return replacement;
        const qsizetype begin = style.indexOf(QLatin1String(beginMarker));
        const qsizetype end = style.indexOf(QLatin1String(endMarker), begin);
        if (begin >= 0 && end >= begin)
        {
            const qsizetype length = end + qstrlen(endMarker) - begin;
            if (begin == 0 && length == style.size()) return replacement;
            if (QStringView(style).mid(begin, length) != QStringView(replacement))
                style.replace(begin, length, replacement);
        }
        else
        {
            style += replacement;
        }
        return style;
    }

    // Auto 只按控件已存在的工具/操作语义区分，页面可明确覆盖，不从文字猜动作。
    bool toolPresentation(const QAbstractButton* button)
    {
        if (qobject_cast<const QToolButton*>(button) != nullptr)
        {
            return true;
        }
        const auto* push = qobject_cast<const QPushButton*>(button);
        // 图标替代动作文字不会改变按钮语义；只有页面明确 setFlat 才使用透明工具外观。
        return push != nullptr && push->isFlat();
    }

    ks::ui::FlatButtonAppearance resolvedAppearance(ks::ui::FlatButtonTone tone,
        ks::ui::FlatButtonAppearance appearance, bool toolButton)
    {
        if (appearance != ks::ui::FlatButtonAppearance::Auto)
        {
            return appearance;
        }
        return tone == ks::ui::FlatButtonTone::Neutral && toolButton
            ? ks::ui::FlatButtonAppearance::Flat : ks::ui::FlatButtonAppearance::Solid;
    }

    // 同一配色被大量按钮和逐帧图标查询重复使用，按真实颜色、状态与主题代次复用计算。
    // QColor 保留完整精度参与相等判定，哈希只负责索引，不以近似 RGB 代替实际输入。
    struct ButtonColorKey
    {
        std::array<QColor, 6> colors; // 配方读取的六个 palette 角色，顺序固定。
        quint64 generation; // 全局语义种子与回退色的有效代次。
        ks::ui::FlatButtonTone tone; // 页面声明的动作色。
        ks::ui::FlatButtonAppearance appearance; // 当前 Solid/Flat/导航语义。
        ks::ui::FlatButtonState state; // 悬停、按下、选中与禁用分别缓存。

        bool operator==(const ButtonColorKey& other) const
        {
            return colors == other.colors && generation == other.generation
                && tone == other.tone && appearance == other.appearance && state == other.state;
        }
    };

    // qHash 只消费稳定输入；色彩哈希碰撞仍由 QColor 的完整相等检查消解。
    size_t qHash(const ButtonColorKey& key, size_t seed = 0)
    {
        seed = qHashMulti(seed, key.generation, static_cast<int>(key.tone),
            static_cast<int>(key.appearance), static_cast<int>(key.state));
        for (const QColor& color : key.colors)
        {
            seed = qHashMulti(seed, static_cast<quint64>(color.rgba64()), color.isValid());
        }
        return seed;
    }

    // 每线程有限缓存不持有任何控件；预览阶段旁路，避免尚未应用的种子污染正式颜色。
    QHash<ButtonColorKey, ks::ui::FlatButtonStateColors>& buttonColorCache()
    {
        static thread_local QHash<ButtonColorKey, ks::ui::FlatButtonStateColors> cache;
        return cache;
    }

    // 透明 QSS 会将父 Base/Button/Window 改成透明黑；向上找真实表面，最后使用应用色板。
    QPalette buttonPalette(const QAbstractButton* button)
    {
        QPalette colors = button != nullptr && button->parentWidget() != nullptr
            ? button->parentWidget()->palette() : QApplication::palette();
        // 主窗口操作底色读取固定内容/表头层，避免嵌套卡片继承后再次加亮。
        if (ks::ui::UsesMainSurfaceHierarchy(button) || ks::ui::UsesTransparentMainSurface(button))
        {
            const ks::ui::MainSurfaceColors surfaces = ks::ui::MainWindowSurfaceColors();
            colors.setColor(QPalette::Base, ks::ui::MainControlSurfaceColor(surfaces.content, button));
            colors.setColor(QPalette::AlternateBase, surfaces.header);
            return colors;
        }
        const QWidget* parent = button == nullptr ? nullptr : button->parentWidget();
        const QColor parentBackground = parent == nullptr ? QColor()
            : colors.color(QPalette::Active, parent->backgroundRole());
        if (parentBackground.isValid() && parentBackground.alpha() == 255)
        {
            colors.setColor(QPalette::Base, parentBackground);
        }
        else if (colors.color(QPalette::Active, QPalette::Base).alpha() == 0)
        {
            QColor surface = QApplication::palette().color(QPalette::Active, QPalette::Window);
            for (const QWidget* owner = button == nullptr ? nullptr : button->parentWidget();
                owner != nullptr; owner = owner->parentWidget())
            {
                const QColor candidate = owner->palette().color(QPalette::Active, owner->backgroundRole());
                if (candidate.alpha() == 255)
                {
                    surface = candidate;
                    break;
                }
            }
            colors.setColor(QPalette::Base, surface);
        }
        return colors;
    }

    // 两种按钮分别输出状态规则；不声明尺寸，内部 clear/menu 等子控件不在规则中猜布局。
    QString buildStyle(ks::ui::FlatButtonTone tone, ks::ui::FlatButtonAppearance appearance,
        const QPalette& palette, const QAbstractButton* button = nullptr)
    {
        using namespace ks::ui;
        const bool transparentSurface = UsesTransparentMainSurface(button);
        const auto backgroundName = [transparentSurface](const QColor& color) {
            return MainDataSurfaceStyleColor(color, transparentSurface);
        };
        QString style = QStringLiteral("/*KSWORD_FLAT_BUTTON_TONE_%1*/"
            "/*KSWORD_FLAT_BUTTON_APPEARANCE_%2*/").arg(static_cast<int>(tone)).arg(static_cast<int>(appearance));
        std::array<FlatButtonStateColors, 5> recipe;
        const FlatButtonState states[] = {FlatButtonState::Normal, FlatButtonState::Hover,
            FlatButtonState::Pressed, FlatButtonState::Checked, FlatButtonState::Disabled};
        FlatButtonAppearance recipeAppearance = FlatButtonAppearance::Auto;
        bool hasRecipe = false;
        for (int kind = 0; kind < 2; ++kind)
        {
            const QString selector = kind == 0 ? QStringLiteral("QPushButton") : QStringLiteral("QToolButton");
            const FlatButtonAppearance actual = resolvedAppearance(tone, appearance,
                button != nullptr ? toolPresentation(button) : kind == 1);
            // 两个selector的实际外观相同时复用五态颜色；Auto工具/操作外观不同仍独立计算。
            if (!hasRecipe || recipeAppearance != actual)
            {
                for (std::size_t index = 0; index < recipe.size(); ++index)
                {
                    recipe[index] = FlatButtonColorsForState(palette, tone, actual, states[index]);
                }
                recipeAppearance = actual;
                hasRecipe = true;
            }
            const auto& normal = recipe[0];
            const auto& hover = recipe[1];
            const auto& pressed = recipe[2];
            const auto& checked = recipe[3];
            const auto& disabled = recipe[4];
            style += QStringLiteral(
                "%1{background-color:%2;color:%3;border:none;}"
                "%1:hover,%1:focus{background-color:%4;color:%5;border:none;}"
                "%1:pressed{background-color:%6;color:%7;border:none;}"
                "%1:checked,%1:checked:hover{background-color:%8;color:%9;border:none;}"
                "%1:disabled,%1:checked:disabled{background-color:%10;color:%11;border:none;}")
                .arg(selector).arg(normal.transparent ? QStringLiteral("transparent") : backgroundName(normal.background))
                .arg(normal.foreground.name()).arg(backgroundName(hover.background)).arg(hover.foreground.name())
                .arg(backgroundName(pressed.background)).arg(pressed.foreground.name())
                .arg(backgroundName(checked.background)).arg(checked.foreground.name())
                .arg(disabled.transparent ? QStringLiteral("transparent") : backgroundName(disabled.background))
                .arg(disabled.foreground.name());
        }
        return style + QStringLiteral("QToolButton::menu-button{background:transparent;border:none;}");
    }

    struct ButtonStyleKey
    {
        ButtonColorKey inputs; // Same complete palette/seed inputs as the color recipe.
        int controlKind; // -1: global Auto recipe; 0/1: actual push/tool presentation.
        bool operator==(const ButtonStyleKey& other) const
        {
            return inputs == other.inputs && controlKind == other.controlKind;
        }
    };

    size_t qHash(const ButtonStyleKey& key, size_t seed = 0)
    {
        return qHashMulti(qHash(key.inputs, seed), key.controlKind);
    }

    // Share immutable QSS strings only. Each widget keeps its existing owner,
    // palette, binding and selectors; this does not change the cascade.
    QString ownedStyle(ks::ui::FlatButtonTone tone, ks::ui::FlatButtonAppearance appearance,
        const QPalette& colors, const QAbstractButton* button = nullptr)
    {
        const ButtonStyleKey key{{{colors.color(QPalette::Active, QPalette::Base),
            colors.color(QPalette::Active, QPalette::Highlight),
            colors.color(QPalette::Active, QPalette::AlternateBase),
            colors.color(QPalette::Active, QPalette::Text),
            colors.color(QPalette::Active, QPalette::HighlightedText),
            colors.color(QPalette::Disabled, QPalette::Text)},
            KswordTheme::ThemeSeedGeneration, tone, appearance, ks::ui::FlatButtonState::Normal},
            button == nullptr ? -1 : toolPresentation(button) ? 1 : 0};
        static thread_local QHash<ButtonStyleKey, QString> cache;
        static thread_local quint64 generation = 0;
        const bool cacheable = KswordTheme::ActiveThemePreviewSeeds == nullptr;
        if (cacheable)
        {
            if (generation != key.inputs.generation)
            {
                cache.clear(); // Do not retain old theme history.
                generation = key.inputs.generation;
            }
            const auto found = cache.constFind(key);
            if (found != cache.constEnd()) return found.value();
        }
        const QString style = QLatin1String(beginMarker) + buildStyle(tone, appearance, colors, button)
            + QLatin1String(endMarker);
        if (cacheable)
        {
            if (cache.size() >= 128) cache.clear();
            cache.insert(key, style);
        }
        return style;
    }

    QPalette sharedMainButtonPalette()
    {
        QPalette colors = QApplication::palette();
        const auto surfaces = ks::ui::MainWindowSurfaceColors();
        colors.setColor(QPalette::Base, surfaces.content);
        colors.setColor(QPalette::AlternateBase, surfaces.header);
        return colors;
    }

    QString withoutButtonBlock(QString style)
    {
        const qsizetype begin = style.indexOf(QLatin1String(beginMarker));
        const qsizetype end = style.indexOf(QLatin1String(endMarker), begin);
        if (begin >= 0 && end >= begin) style.remove(begin, end + qstrlen(endMarker) - begin);
        return style;
    }

    bool canShareMainButtonStyle(const QAbstractButton* button, ks::ui::FlatButtonTone tone,
        ks::ui::FlatButtonAppearance appearance, const QPalette& colors)
    {
        using namespace ks::ui;
        if (tone != FlatButtonTone::Neutral || appearance != FlatButtonAppearance::Auto
            || KswordTheme::ActiveThemePreviewSeeds || !button->objectName().isEmpty()
            || !UsesMainSurfaceHierarchy(button)) return false;
        const auto* push = qobject_cast<const QPushButton*>(button);
        if (push && push->isFlat()) return false;
        // Compound controls keep their original descendant cascade. Future
        // child buttons still enroll through FlatButtonFilter's ancestor marker.
        for (QObject* child : button->children())
            if (qobject_cast<QWidget*>(child)) return false;
        for (const char* role : {"ksword_primary_toolbar", "ksword_primary_content_panel",
            "ksword_workspace_page", "ksword_parameter_surface", "ksword_secondary_window",
            "ksword_secondary_footer", "ksword_dock_content_root"})
            if (button->property(role).toBool()) return false;
        const QWidget* root = button->window();
        const QString rootStyle = root->property("ksword_shared_button_root_style").toString();
        if (rootStyle.isEmpty()
            || root->property("ksword_shared_button_generation").toULongLong() != KswordTheme::ThemeSeedGeneration
            || rootStyle != root->styleSheet()) return false;
        const QPalette shared = root->property("ksword_shared_button_palette").value<QPalette>();
        for (const auto group : {QPalette::Active, QPalette::Disabled})
            for (const auto role : {QPalette::Base, QPalette::Highlight, QPalette::AlternateBase,
                QPalette::Text, QPalette::HighlightedText})
                if (colors.color(group, role) != shared.color(group, role)) return false;
        for (const QWidget* owner = button; owner && owner != root; owner = owner->parentWidget())
        {
            if (owner != button && qobject_cast<const QAbstractButton*>(owner)) return false;
            if (owner->inherits("ads::CDockAreaTitleBar") || owner->inherits("ads::CDockWidgetTab")) return false;
            QString style = owner == button ? withoutButtonBlock(owner->styleSheet()) : owner->styleSheet();
            for (const char* blockName : {"ksword_primary_style_block", "ksword_parameter_surface_style_block",
                "ksword_secondary_style_block", "ksword_dock_root_surface_style_block", "ksword_page_tabs_owned_pane_style"})
            {
                const QString block = owner->property(blockName).toString();
                if (!block.isEmpty()) style.remove(block);
            }
            if (owner->property("ksword_toolbar_control").toBool()) style.remove(ToolbarControlGeometryStyle().trimmed());
            if (owner->property("ksword_toolbar_icon_only").toBool()) style.remove(ToolbarIconGeometryStyle().trimmed());
            if (!style.trimmed().isEmpty()) return false;
        }
        return true;
    }

    // 只对明确使用本组件标记的本地/父级样式登记绑定，不猜测未审核业务的动作色。
    class FlatButtonFilter final : public QObject
    {
    public:
        explicit FlatButtonFilter(QApplication* app) : QObject(app) {}

        bool eventFilter(QObject* object, QEvent* event) override
        {
            if (event == nullptr || (event->type() != QEvent::Polish
                && event->type() != QEvent::Show && event->type() != QEvent::StyleChange))
            {
                return false;
            }
            auto* button = qobject_cast<QAbstractButton*>(object);
            if (button == nullptr || button->inherits("QLineEditIconButton")
                || button->property("ksword_segmented_button").toBool() || (!qobject_cast<QPushButton*>(button)
                && !qobject_cast<QToolButton*>(button)) || (ks::ui::HasWidgetThemeBinding(button)
                && !button->property("ksword_flat_button_managed").toBool()))
            {
                return false;
            }
            // 已真正应用的完整局部规则具有自己的选择器优先级；没有输入变化时
            // 无须每次Show再次剥容器块/解析marker，也不依赖StyleChange是否发出。
            const QString appliedStyle = button->property("ksword_flat_button_applied_style").toString();
            if (!button->property("ksword_flat_button_pending").toBool()
                && button->property("ksword_flat_button_managed").toBool()
                && (!appliedStyle.isEmpty() || button->property("ksword_flat_button_shared_owner").toBool())
                && button->styleSheet() == appliedStyle
                && button->property("ksword_flat_button_tone")
                    == button->property("ksword_flat_button_applied_tone")
                && button->property("ksword_flat_button_appearance")
                    == button->property("ksword_flat_button_applied_appearance"))
            {
                return false;
            }
            // 显式 API 尚未刷新时，旧的本地/父级 marker 不得把 Danger 降回 Neutral。
            // 尺寸归一可能在排队期间追加几何片段，不应因此放弃显式页面声明。
            if (button->property("ksword_flat_button_pending").toBool()
                && buttonColorOwnershipStyle(button)
                    == button->property("ksword_flat_button_initial_color_style").toString())
            {
                return false;
            }
            for (QWidget* owner = button; owner != nullptr; owner = owner->parentWidget())
            {
                if (owner != button && owner->property("ksword_flat_button_shared_owner").toBool())
                {
                    // ChildAdded already dirtied this explicit parent. Restore
                    // its original compound-control cascade before child polish.
                    const QPointer<QAbstractButton> target(button);
                    const QPointer<QWidget> parentOwner(owner);
                    ks::ui::PrepareWidgetThemeBindingsForShow(owner);
                    if (!target) return true;
                    if (!parentOwner) return false;
                }
                const auto& ownership = buttonOwnershipStyle(owner);
                const QString& style = ownership.colorStyle;
                const bool sharedOwner = style.isEmpty() && owner->property("ksword_flat_button_shared_owner").toBool();
                const int latestTone = sharedOwner ? static_cast<int>(ks::ui::FlatButtonTone::Neutral) : ownership.tone;
                // 未标记的本地或祖先覆盖属于页面，不跨过它猜更远父级/全局按钮归一。
                // 例如数据颜色面板可在父 QWidget 给按钮定义底色，而按钮自身 QSS 为空。
                if (!style.isEmpty() && latestTone < 0)
                {
                    return false;
                }
                if (latestTone >= 0)
                {
                    int latestAppearance = sharedOwner || ownership.appearance < 0
                        ? static_cast<int>(ks::ui::FlatButtonAppearance::Auto) : ownership.appearance;
                    // 旧工厂的 Auto 标记不能撤销页面明确选择；显式 API 调用仍能重新声明外观。
                    const QVariant declared = button->property("ksword_flat_button_appearance");
                    if (latestAppearance == static_cast<int>(ks::ui::FlatButtonAppearance::Auto)
                        && button->property("ksword_flat_button_managed").toBool() && declared.isValid())
                    {
                        latestAppearance = declared.toInt();
                    }
                    if (!button->property("ksword_flat_button_managed").toBool()
                        || button->property("ksword_flat_button_tone").toInt() != latestTone
                        || button->property("ksword_flat_button_appearance").toInt() != latestAppearance)
                    {
                        ks::ui::ApplyFlatButtonTheme(button,
                            static_cast<ks::ui::FlatButtonTone>(latestTone),
                            static_cast<ks::ui::FlatButtonAppearance>(latestAppearance));
                    }
                    return false;
                }
            }
            return false;
        }
    };
}

ks::ui::FlatButtonStateColors ks::ui::FlatButtonColorsForState(const QPalette& palette,
    FlatButtonTone tone, FlatButtonAppearance appearance, FlatButtonState state)
{
    const ButtonColorKey key{{palette.color(QPalette::Active, QPalette::Base),
        palette.color(QPalette::Active, QPalette::Highlight),
        palette.color(QPalette::Active, QPalette::AlternateBase),
        palette.color(QPalette::Active, QPalette::Text),
        palette.color(QPalette::Active, QPalette::HighlightedText),
        palette.color(QPalette::Disabled, QPalette::Text)},
        KswordTheme::ThemeSeedGeneration, tone, appearance, state};
    const bool cacheable = KswordTheme::ActiveThemePreviewSeeds == nullptr;
    auto& cache = buttonColorCache(); // 所有返回颜色由相同真实输入限定，不冻结交互状态。
    if (cacheable)
    {
        const auto cached = cache.constFind(key);
        if (cached != cache.constEnd())
        {
            return cached.value();
        }
    }
    // 透明父级可能给出 alpha=0 的黑；使用表面角色回退，不能只把 alpha 强制改成255。
    QColor surface = palette.color(QPalette::Active, QPalette::Base);
    if (!surface.isValid() || surface.alpha() == 0)
    {
        surface = KswordTheme::SurfaceColor();
    }
    surface.setAlpha(255);
    QColor accent = palette.color(QPalette::Active, QPalette::Highlight);
    if (!accent.isValid() || accent.alpha() == 0)
    {
        accent = KswordTheme::PrimaryAccentColor();
    }
    accent.setAlpha(255);
    const QColor checked = KswordTheme::EnsureTextContrast(accent, surface, 3.0);
    QColor interaction = tone == FlatButtonTone::Danger ? KswordTheme::ErrorColor()
        : tone == FlatButtonTone::Success ? KswordTheme::SuccessColor() : checked;
    interaction = KswordTheme::EnsureTextContrast(interaction, surface, 3.0);
    interaction.setAlpha(255);
    QColor alternate = palette.color(QPalette::Active, QPalette::AlternateBase);
    if (!alternate.isValid() || alternate.alpha() == 0)
    {
        alternate = KswordTheme::SurfaceAltColor();
    }
    alternate.setAlpha(255);
    // 普通操作与输入框共用中性表面，主题色集中在交互和选中状态，避免满页蓝色方块。
    const QColor neutral = KswordTheme::ControlInputSurfaceColor(surface, alternate);
    const bool navigation = appearance == FlatButtonAppearance::Navigation;
    const bool flat = appearance == FlatButtonAppearance::Flat || navigation;
    FlatButtonStateColors result;
    result.background = tone == FlatButtonTone::Neutral ? neutral : interaction;
    QColor preferred = palette.color(QPalette::Active, QPalette::Text);
    if (!preferred.isValid() || preferred.alpha() == 0)
    {
        preferred = KswordTheme::TextPrimaryColor();
    }
    if (state == FlatButtonState::Normal && flat)
    {
        result.background = surface;
        result.transparent = true;
    }
    else if (state == FlatButtonState::Hover)
    {
        result.background = navigation ? KswordTheme::BlendColors(surface, alternate, 96) : interaction;
        if (tone != FlatButtonTone::Neutral && !flat)
        {
            // 已有实色的主操作也保留悬停反馈，不让 Hover 与普通态完全相同。
            const QColor hover = KswordTheme::RelativeLuminance(surface) < 0.25
                ? interaction.lighter(120) : interaction.darker(112);
            result.background = KswordTheme::EnsureTextContrast(hover, surface, 3.0);
        }
    }
    else if (state == FlatButtonState::Pressed)
    {
        result.background = navigation ? KswordTheme::BlendColors(surface, alternate, 210)
            : KswordTheme::EnsureTextContrast(interaction.darker(118), surface, 3.0);
    }
    else if (state == FlatButtonState::Checked)
    {
        // 导航选中保留中性底面，仅文字/图标强调，避免侧栏大面积亮蓝抢占信息层级。
        result.background = navigation ? KswordTheme::BlendColors(surface, alternate, 170) : checked;
        preferred = navigation ? accent : palette.color(QPalette::Active, QPalette::HighlightedText);
    }
    else if (state == FlatButtonState::Disabled)
    {
        result.background = flat ? surface : KswordTheme::EnsureTextContrast(alternate, surface, 1.2);
        result.transparent = flat;
        preferred = palette.color(QPalette::Disabled, QPalette::Text);
    }
    result.foreground = KswordTheme::EnsureTextContrast(preferred, result.background,
        state == FlatButtonState::Disabled ? 3.0 : 4.5);
    if (cacheable)
    {
        if (cache.size() >= 512)
        {
            cache.clear(); // 边界有限，用户长期试色不会累积无限历史配方。
        }
        cache.insert(key, result);
    }
    return result;
}

QString ks::ui::BuildFlatButtonStyle(FlatButtonTone tone, FlatButtonAppearance appearance, const QAbstractButton* button)
{
    return ownedStyle(tone, appearance, button ? buttonPalette(button) : QApplication::palette(), button);
}

QString ks::ui::BuildSharedMainSurfaceButtonStyle()
{
    QString style = buildStyle(FlatButtonTone::Neutral, FlatButtonAppearance::Auto, sharedMainButtonPalette());
    // The scoped root rules do not declare the entire ancestor a button owner.
    const qsizetype markerEnd = style.indexOf(QStringLiteral("*/"), style.indexOf(QStringLiteral("*/")) + 2);
    style.remove(0, markerEnd + 2);
    style.replace(QStringLiteral("QPushButton"), QStringLiteral("QPushButton[ksword_flat_button_shared_owner=\"true\"]"));
    style.replace(QStringLiteral("QToolButton"), QStringLiteral("QToolButton[ksword_flat_button_shared_owner=\"true\"]"));
    return style;
}

void ks::ui::RegisterSharedMainSurfaceButtonStyle(QWidget* root, const QString& completeStyle)
{
    const QPalette colors = sharedMainButtonPalette();
    if (root->property("ksword_shared_button_root_style").toString() == completeStyle
        && root->property("ksword_shared_button_generation").toULongLong() == KswordTheme::ThemeSeedGeneration
        && root->property("ksword_shared_button_palette").value<QPalette>() == colors) return;
    // A cached floating-host QSS can predate a palette change. Never certify
    // new colors unless their exact shared rules are present in these bytes.
    root->setProperty("ksword_shared_button_root_style",
        completeStyle.contains(BuildSharedMainSurfaceButtonStyle()) ? completeStyle : QString());
    root->setProperty("ksword_shared_button_palette", QVariant::fromValue(colors));
    root->setProperty("ksword_shared_button_generation", QVariant::fromValue(KswordTheme::ThemeSeedGeneration));
}

void ks::ui::ApplyFlatButtonTheme(QAbstractButton* button, FlatButtonTone tone, FlatButtonAppearance appearance)
{
    if (button == nullptr || button->inherits("QLineEditIconButton") || (!qobject_cast<QPushButton*>(button)
        && !qobject_cast<QToolButton*>(button)))
    {
        return;
    }
    button->setProperty("ksword_flat_button_managed", true);
    button->setProperty("ksword_flat_button_tone", static_cast<int>(tone));
    button->setProperty("ksword_flat_button_appearance", static_cast<int>(appearance));
    const QPointer<QAbstractButton> guardedButton(button);
    // 比较的是配色所有权，不是整份 QSS；后续 ToolbarMetrics 追加的尺寸规则不撤销授权。
    const QString initialColorStyle = buttonColorOwnershipStyle(button);
    button->setProperty("ksword_flat_button_pending", true);
    button->setProperty("ksword_flat_button_initial_color_style", initialColorStyle);
    // ThemeBinding 探活时复制回调；首次许可必须跨闭包副本共享，不能只用 mutable bool。
    const auto applied = std::make_shared<bool>(false);
    BindWidgetTheme(button, [guardedButton, tone, appearance, initialColorStyle, applied]()
    {
        if (guardedButton.isNull())
        {
            return;
        }
        const QString currentStyle = guardedButton->styleSheet();
        // 页面后来改成未标记的数据色时，旧绑定不得重新追加共享规则夺回所有权。
        // 首次显式接管也要核对等待队列期间页面是否已经换过样式。
        const QString currentColorStyle = buttonColorOwnershipStyle(guardedButton.data());
        if (!currentColorStyle.isEmpty() && !currentStyle.contains(QLatin1String(beginMarker))
            && (*applied || currentColorStyle != initialColorStyle))
        {
            if (guardedButton->property("ksword_flat_button_shared_owner").isValid())
            {
                guardedButton->setProperty("ksword_flat_button_shared_owner", QVariant());
                if (!guardedButton) return;
                guardedButton->style()->unpolish(guardedButton.data());
                if (guardedButton) guardedButton->style()->polish(guardedButton.data());
                if (guardedButton) guardedButton->update();
            }
            if (!guardedButton) return;
            guardedButton->setProperty("ksword_flat_button_pending", false);
            return;
        }
        const QPalette colors = buttonPalette(guardedButton.data());
        const auto actualAppearance = resolvedAppearance(tone, appearance, toolPresentation(guardedButton.data()));
        const bool shared = canShareMainButtonStyle(guardedButton.data(), tone, appearance, colors);
        const bool ownershipChanged = guardedButton->property("ksword_flat_button_shared_owner").toBool() != shared;
        if (ownershipChanged)
            guardedButton->setProperty("ksword_flat_button_shared_owner", shared ? QVariant(true) : QVariant());
        if (!guardedButton) return;
        const QString style = shared ? withoutButtonBlock(currentStyle)
            : replaceOwnedBlock(currentStyle, ownedStyle(tone, appearance, colors, guardedButton.data()));
        *applied = true;
        if (style != guardedButton->styleSheet())
        {
            guardedButton->setStyleSheet(style);
        }
        else if (ownershipChanged)
        {
            guardedButton->style()->unpolish(guardedButton.data());
            if (guardedButton) guardedButton->style()->polish(guardedButton.data());
            if (guardedButton) guardedButton->update();
        }
        if (!guardedButton.isNull() && guardedButton->styleSheet() == style)
        {
            // 排队换肤期间局部 QSS 可能仍是上一主题；图标须按已经真正下发的五态底色校准。
            QVariantList appliedBackgrounds; // 保存真实正常/悬停/按下/选中/禁用底面，而非当前全局种子。
            if (!shared)
                for (const auto state : {FlatButtonState::Normal, FlatButtonState::Hover,
                    FlatButtonState::Pressed, FlatButtonState::Checked, FlatButtonState::Disabled})
                {
                    appliedBackgrounds.append(QVariant::fromValue(
                        MainControlSurfaceColor(FlatButtonColorsForState(colors, tone, actualAppearance, state).background, guardedButton)));
                }
            // 记录最后实际下发的完整样式及配方，逐帧图标无需再扫描同一祖先 QSS 链。
            // 页面后来追加/替换任意规则时精确字符串不相等，查询立即回到所有权检查。
            guardedButton->setProperty("ksword_flat_button_applied_style", style.isEmpty() ? QVariant() : QVariant(style));
            guardedButton->setProperty("ksword_flat_button_applied_tone", static_cast<int>(tone));
            guardedButton->setProperty("ksword_flat_button_applied_appearance", static_cast<int>(appearance));
            guardedButton->setProperty("ksword_flat_button_applied_backgrounds",
                shared ? QVariant() : QVariant(appliedBackgrounds));
            guardedButton->setProperty("ksword_flat_button_pending", false);
            guardedButton->setProperty("ksword_flat_button_initial_color_style", QVariant());
            if (guardedButton->property("ksword_flat_button_shared_dirty").isValid())
                guardedButton->setProperty("ksword_flat_button_shared_dirty", QVariant());
        }
    }, ThemePalettePolicy::PreserveLocal);
}

void ks::ui::InstallGlobalFlatButtonTheme(QApplication* app)
{
    if (app == nullptr || app->property("ksword_flat_button_filter").toBool())
    {
        return;
    }
    app->setProperty("ksword_flat_button_filter", true);
    app->installEventFilter(new FlatButtonFilter(app));
}

bool ks::ui::TryGetFlatButtonBackground(const QAbstractButton* button,
    QIcon::Mode mode, QIcon::State state, QColor* background)
{
    if (button == nullptr || button->inherits("QLineEditIconButton") || background == nullptr
        || (!qobject_cast<const QPushButton*>(button) && !qobject_cast<const QToolButton*>(button)))
    {
        return false;
    }
    int tone = -1; // 只从仍实际存在的拥有标记读取，避免旧 managed 属性覆盖页面数据色。
    FlatButtonAppearance appearance = FlatButtonAppearance::Auto;
    const QString appliedStyle = button->property("ksword_flat_button_applied_style").toString();
    const bool shared = button->property("ksword_flat_button_shared_owner").toBool();
    const bool exactAppliedStyle = (shared || !appliedStyle.isEmpty()) && button->styleSheet() == appliedStyle;
    const QWidget* sharedRoot = shared ? button->window() : nullptr;
    if (shared && (button->property("ksword_flat_button_shared_dirty").toBool()
        || !exactAppliedStyle || !UsesMainSurfaceHierarchy(button)
        || sharedRoot->property("ksword_shared_button_generation").toULongLong() != KswordTheme::ThemeSeedGeneration
        || sharedRoot->property("ksword_shared_button_root_style").toString() != sharedRoot->styleSheet())) return false;
    if (exactAppliedStyle)
    {
        // 完整局部块仍存在且未被页面重写，局部选择器优先于祖先，直接使用实际已下发的配方。
        tone = button->property("ksword_flat_button_applied_tone").toInt();
        appearance = static_cast<FlatButtonAppearance>(
            button->property("ksword_flat_button_applied_appearance").toInt());
    }
    else
    {
        for (const QWidget* owner = button; owner != nullptr; owner = owner->parentWidget())
        {
            const auto& ownership = buttonOwnershipStyle(owner);
            const QString& style = ownership.colorStyle;
            if (ownership.tone >= 0) tone = ownership.tone;
            // 祖先未知 QSS 可能比应用基线更优先；必须和自动绑定使用相同所有权边界。
            if (!style.isEmpty() && tone < 0)
            {
                return false;
            }
            if (tone >= 0)
            {
                break;
            }
        }
        if (tone < 0)
        {
            // 没有本地覆盖的应用默认按钮使用全局同一 Neutral 块。
            if (!buttonColorOwnershipStyle(button).isEmpty() || QApplication::instance() == nullptr
                || !qApp->styleSheet().contains(QLatin1String(beginMarker)))
            {
                return false;
            }
            tone = static_cast<int>(FlatButtonTone::Neutral);
        }
        for (const QWidget* owner = button; owner != nullptr; owner = owner->parentWidget())
        {
            const auto& ownership = buttonOwnershipStyle(owner);
            const QString& style = ownership.colorStyle;
            if (ownership.appearance >= 0)
            {
                appearance = static_cast<FlatButtonAppearance>(ownership.appearance);
                break;
            }
            if (!style.isEmpty())
            {
                break;
            }
        }
    }
    appearance = resolvedAppearance(static_cast<FlatButtonTone>(tone), appearance,
        toolPresentation(button));
    FlatButtonState actualState = FlatButtonState::Normal;
    if (mode == QIcon::Active || button->underMouse() || button->hasFocus())
    {
        actualState = FlatButtonState::Hover;
    }
    if (button->isDown())
    {
        actualState = FlatButtonState::Pressed;
    }
    if (button->isChecked() || state == QIcon::On || mode == QIcon::Selected)
    {
        actualState = FlatButtonState::Checked;
    }
    if (!button->isEnabled() || mode == QIcon::Disabled)
    {
        actualState = FlatButtonState::Disabled;
    }
    if (exactAppliedStyle && !shared)
    {
        const QVariantList backgrounds = button->property("ksword_flat_button_applied_backgrounds").toList();
        const qsizetype stateIndex = static_cast<qsizetype>(actualState);
        if (backgrounds.size() == 5 && backgrounds.at(stateIndex).value<QColor>().isValid())
        {
            *background = backgrounds.at(stateIndex).value<QColor>();
            return true;
        }
    }
    const FlatButtonStateColors colors = FlatButtonColorsForState(shared
            ? sharedRoot->property("ksword_shared_button_palette").value<QPalette>() : buttonPalette(button),
        static_cast<FlatButtonTone>(tone), appearance, actualState);
    *background = MainControlSurfaceColor(colors.background, button);
    return true;
}

bool ks::ui::IsFlatButtonBackgroundTransparent(const QAbstractButton* button,
    QIcon::Mode mode, QIcon::State state)
{
    if (button == nullptr)
    {
        return false;
    }
    const bool disabled = !button->isEnabled() || mode == QIcon::Disabled;
    if (!disabled && (mode == QIcon::Active || mode == QIcon::Selected || state == QIcon::On
        || button->isChecked() || button->isDown() || button->underMouse() || button->hasFocus()))
    {
        return false;
    }
    QColor background;
    if (!TryGetFlatButtonBackground(button, mode, state, &background))
    {
        return false;
    }
    const QString style = button->styleSheet();
    const bool explicitFlat = style.contains(QStringLiteral("/*KSWORD_FLAT_BUTTON_APPEARANCE_2*/"));
    const bool explicitSolid = style.contains(QStringLiteral("/*KSWORD_FLAT_BUTTON_APPEARANCE_1*/"));
    return explicitFlat || (!explicitSolid && toolPresentation(button)
        && button->property("ksword_flat_button_tone").toInt() == static_cast<int>(FlatButtonTone::Neutral));
}
