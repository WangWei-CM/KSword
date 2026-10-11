#include "./FlatButtonTheme.h"
#include "ThemeBinding.h"
#include "./ToolbarMetrics.h"
#include "./MainSurfaceStyle.h"
#include "../theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <memory>

namespace
{
    // 标记只描述本模块拥有的颜色规则，页面原有几何和其它控件样式原样保留。
    constexpr char beginMarker[] = "/*KSWORD_FLAT_BUTTON_BEGIN*/";
    constexpr char endMarker[] = "/*KSWORD_FLAT_BUTTON_END*/";

    // 返回影响按钮颜色所有权的剩余 QSS，不修改真实样式或接管未知业务颜色。
    // 只精确排除已登记的工具栏几何与主页面容器块；额外规则仍阻止跨层猜配色。
    QString buttonColorOwnershipStyle(const QWidget* owner)
    {
        QString style = owner->styleSheet(); // 当前控件真正拥有的完整局部样式。
        if (owner->property("ksword_toolbar_control").toBool())
        {
            style.remove(ks::ui::ToolbarControlGeometryStyle().trimmed());
        }
        if (owner->property("ksword_toolbar_icon_only").toBool())
        {
            style.remove(ks::ui::ToolbarIconGeometryStyle().trimmed());
        }
        // 容器块由 PrimaryPageStyle 记录完整内容，不能只凭属性忽略整份祖先样式。
        const QString primaryBlock = owner->property("ksword_primary_style_block").toString();
        if (!primaryBlock.isEmpty())
        {
            style.remove(primaryBlock);
        }
        return style.trimmed();
    }

    QString replaceOwnedBlock(QString style, const QString& replacement)
    {
        const qsizetype begin = style.indexOf(QLatin1String(beginMarker));
        const qsizetype end = style.indexOf(QLatin1String(endMarker), begin);
        if (begin >= 0 && end >= begin)
        {
            style.replace(begin, end + qstrlen(endMarker) - begin, replacement);
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
        return push != nullptr && (push->isFlat()
            || (push->text().isEmpty() && !push->icon().isNull()));
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

    // 透明 QSS 会将父 Base/Button/Window 改成透明黑；向上找真实表面，最后使用应用色板。
    QPalette buttonPalette(const QAbstractButton* button)
    {
        QPalette colors = button != nullptr && button->parentWidget() != nullptr
            ? button->parentWidget()->palette() : QApplication::palette();
        // 主窗口操作底色读取固定内容/表头层，避免嵌套卡片继承后再次加亮。
        if (ks::ui::UsesMainSurfaceHierarchy(button))
        {
            const ks::ui::MainSurfaceColors surfaces = ks::ui::MainWindowSurfaceColors();
            colors.setColor(QPalette::Base, surfaces.content);
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
        QString style = QStringLiteral("/*KSWORD_FLAT_BUTTON_TONE_%1*/"
            "/*KSWORD_FLAT_BUTTON_APPEARANCE_%2*/").arg(static_cast<int>(tone)).arg(static_cast<int>(appearance));
        for (int kind = 0; kind < 2; ++kind)
        {
            const QString selector = kind == 0 ? QStringLiteral("QPushButton") : QStringLiteral("QToolButton");
            const FlatButtonAppearance actual = resolvedAppearance(tone, appearance,
                button != nullptr ? toolPresentation(button) : kind == 1);
            const FlatButtonStateColors normal = FlatButtonColorsForState(palette, tone, actual, FlatButtonState::Normal);
            const FlatButtonStateColors hover = FlatButtonColorsForState(palette, tone, actual, FlatButtonState::Hover);
            const FlatButtonStateColors pressed = FlatButtonColorsForState(palette, tone, actual, FlatButtonState::Pressed);
            const FlatButtonStateColors checked = FlatButtonColorsForState(palette, tone, actual, FlatButtonState::Checked);
            const FlatButtonStateColors disabled = FlatButtonColorsForState(palette, tone, actual, FlatButtonState::Disabled);
            style += QStringLiteral(
                "%1{background-color:%2;color:%3;border:none;}"
                "%1:hover,%1:focus{background-color:%4;color:%5;border:none;}"
                "%1:pressed{background-color:%6;color:%7;border:none;}"
                "%1:checked,%1:checked:hover{background-color:%8;color:%9;border:none;}"
                "%1:disabled,%1:checked:disabled{background-color:%10;color:%11;border:none;}")
                .arg(selector).arg(normal.transparent ? QStringLiteral("transparent") : normal.background.name())
                .arg(normal.foreground.name()).arg(hover.background.name()).arg(hover.foreground.name())
                .arg(pressed.background.name()).arg(pressed.foreground.name())
                .arg(checked.background.name()).arg(checked.foreground.name())
                .arg(disabled.transparent ? QStringLiteral("transparent") : disabled.background.name())
                .arg(disabled.foreground.name());
        }
        return style + QStringLiteral("QToolButton::menu-button{background:transparent;border:none;}");
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
                const QString style = buttonColorOwnershipStyle(owner);
                int latestTone = -1; // 后声明的页面状态优先，不把首次 Neutral 锁成永久色调。
                qsizetype latestPosition = -1;
                for (int tone = 0; tone <= 3; ++tone)
                {
                    const qsizetype position = style.lastIndexOf(
                        QStringLiteral("/*KSWORD_FLAT_BUTTON_TONE_%1*/").arg(tone));
                    if (position > latestPosition)
                    {
                        latestPosition = position;
                        latestTone = tone;
                    }
                }
                // 未标记的本地或祖先覆盖属于页面，不跨过它猜更远父级/全局按钮归一。
                // 例如数据颜色面板可在父 QWidget 给按钮定义底色，而按钮自身 QSS 为空。
                if (!style.isEmpty() && latestTone < 0)
                {
                    return false;
                }
                if (latestTone >= 0)
                {
                    int latestAppearance = 0; // 未声明时沿用 Auto；独立 Flat/Solid 由页面明确拥有。
                    qsizetype appearancePosition = -1;
                    for (int candidate = 0; candidate <= static_cast<int>(ks::ui::FlatButtonAppearance::Navigation); ++candidate)
                    {
                        const qsizetype position = style.lastIndexOf(
                            QStringLiteral("/*KSWORD_FLAT_BUTTON_APPEARANCE_%1*/").arg(candidate));
                        if (position > appearancePosition)
                        {
                            appearancePosition = position;
                            latestAppearance = candidate;
                        }
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
        result.background = navigation ? KswordTheme::BlendColors(surface, checked, 32) : interaction;
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
        result.background = navigation ? KswordTheme::BlendColors(surface, checked, 80)
            : KswordTheme::EnsureTextContrast(interaction.darker(118), surface, 3.0);
    }
    else if (state == FlatButtonState::Checked)
    {
        result.background = navigation ? KswordTheme::BlendColors(surface, checked, 210) : checked;
        preferred = palette.color(QPalette::Active, QPalette::HighlightedText);
    }
    else if (state == FlatButtonState::Disabled)
    {
        result.background = flat ? surface : KswordTheme::EnsureTextContrast(alternate, surface, 1.2);
        result.transparent = flat;
        preferred = palette.color(QPalette::Disabled, QPalette::Text);
    }
    result.foreground = KswordTheme::EnsureTextContrast(preferred, result.background,
        state == FlatButtonState::Disabled ? 3.0 : 4.5);
    return result;
}

QString ks::ui::BuildFlatButtonStyle(FlatButtonTone tone, FlatButtonAppearance appearance)
{
    return QLatin1String(beginMarker) + buildStyle(tone, appearance, QApplication::palette())
        + QLatin1String(endMarker);
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
        if (!currentStyle.isEmpty() && !currentStyle.contains(QLatin1String(beginMarker))
            && (*applied || buttonColorOwnershipStyle(guardedButton.data()) != initialColorStyle))
        {
            guardedButton->setProperty("ksword_flat_button_pending", false);
            return;
        }
        const QPalette colors = buttonPalette(guardedButton.data());
        const QString block = QLatin1String(beginMarker) + buildStyle(tone, appearance, colors, guardedButton.data())
            + QLatin1String(endMarker);
        const QString style = replaceOwnedBlock(currentStyle, block);
        *applied = true;
        if (style != guardedButton->styleSheet())
        {
            guardedButton->setStyleSheet(style);
        }
        if (!guardedButton.isNull())
        {
            guardedButton->setProperty("ksword_flat_button_pending", false);
            guardedButton->setProperty("ksword_flat_button_initial_color_style", QVariant());
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
    for (const QWidget* owner = button; owner != nullptr; owner = owner->parentWidget())
    {
        const QString style = buttonColorOwnershipStyle(owner);
        qsizetype latestPosition = -1;
        for (int candidate = 0; candidate <= 3; ++candidate)
        {
            const qsizetype position = style.lastIndexOf(
                QStringLiteral("/*KSWORD_FLAT_BUTTON_TONE_%1*/").arg(candidate));
            if (position > latestPosition)
            {
                latestPosition = position;
                tone = candidate;
            }
        }
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
    FlatButtonAppearance appearance = FlatButtonAppearance::Auto;
    for (const QWidget* owner = button; owner != nullptr; owner = owner->parentWidget())
    {
        const QString style = buttonColorOwnershipStyle(owner);
        qsizetype latest = -1;
        for (int candidate = 0; candidate <= static_cast<int>(FlatButtonAppearance::Navigation); ++candidate)
        {
            const qsizetype position = style.lastIndexOf(
                QStringLiteral("/*KSWORD_FLAT_BUTTON_APPEARANCE_%1*/").arg(candidate));
            if (position > latest)
            {
                latest = position;
                appearance = static_cast<FlatButtonAppearance>(candidate);
            }
        }
        if (latest >= 0)
        {
            break;
        }
        if (!style.isEmpty())
        {
            break;
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
    const FlatButtonStateColors colors = FlatButtonColorsForState(buttonPalette(button),
        static_cast<FlatButtonTone>(tone), appearance, actualState);
    *background = colors.background;
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
