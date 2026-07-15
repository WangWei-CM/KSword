#pragma once

// Central theme helpers for all Qt UI code.
//
// A color used by a widget must be derived from a named theme role and an
// offset.  Keeping the RGB seed and the light/dark offsets here prevents a
// local literal from silently becoming unreadable when the application theme
// changes.

#include <QApplication>
#include <QColor>
#include <QString>

#include <cmath>

namespace KswordTheme
{
    inline bool IsDarkModeEnabled();

    struct RgbOffset
    {
        int red = 0;
        int green = 0;
        int blue = 0;
    };

    // ThemeRgbOffset 作用：把同一颜色角色的深色、浅色偏移量绑定为一组。
    // 两组数值必须相对于同一个基础色计算，调用方不能只传一套数值复用到两个主题。
    struct ThemeRgbOffset
    {
        RgbOffset dark;
        RgbOffset light;
    };

    inline constexpr RgbOffset UniformOffset(const int value)
    {
        return { value, value, value };
    }

    // UniformThemeOffset 作用：生成深浅主题各自独立的等量 RGB 偏移。
    // 入参分别是深色与浅色模式数值，返回可交给 ThemeOffsetColor 的成对配置。
    inline constexpr ThemeRgbOffset UniformThemeOffset(
        const int darkValue,
        const int lightValue)
    {
        return { UniformOffset(darkValue), UniformOffset(lightValue) };
    }

    inline int ClampChannel(const int channelValue)
    {
        return qBound(0, channelValue, 255);
    }

    // OffsetColor is the only place where RGB channel arithmetic is allowed.
    // Callers pass a named seed and a named/semantic offset instead of a
    // second hard-coded color for the other theme.
    inline QColor OffsetColor(
        const QColor& baseColor,
        const RgbOffset offset,
        const int alphaOverride = -1)
    {
        QColor adjustedColor(
            ClampChannel(baseColor.red() + offset.red),
            ClampChannel(baseColor.green() + offset.green),
            ClampChannel(baseColor.blue() + offset.blue),
            alphaOverride >= 0 ? ClampChannel(alphaOverride) : baseColor.alpha());
        return adjustedColor;
    }

    // ActiveThemeOffset 作用：根据当前主题只选择对应的一套 RGB 偏移量。
    // 入参为成对配置，返回深色或浅色分支，不执行任何颜色运算。
    inline RgbOffset ActiveThemeOffset(const ThemeRgbOffset& themeOffset)
    {
        return IsDarkModeEnabled() ? themeOffset.dark : themeOffset.light;
    }

    // ThemeOffsetColor 作用：使用同一基础色和两套独立偏移量生成当前主题颜色。
    // alphaOverride 为负数时保留基础色透明度，非负时覆盖透明度。
    inline QColor ThemeOffsetColor(
        const QColor& baseColor,
        const ThemeRgbOffset& themeOffset,
        const int alphaOverride = -1)
    {
        return OffsetColor(baseColor, ActiveThemeOffset(themeOffset), alphaOverride);
    }

    inline QColor OffsetColor(const QColor& baseColor, const int uniformOffset)
    {
        return OffsetColor(baseColor, UniformOffset(uniformOffset));
    }

    inline QColor WithAlpha(const QColor& baseColor, const int alphaValue)
    {
        return OffsetColor(baseColor, {}, alphaValue);
    }

    inline QColor ThemeLighterColor(const QColor& baseColor)
    {
        return ThemeOffsetColor(baseColor, UniformThemeOffset(10, 18));
    }

    inline QColor ThemeDarkerColor(const QColor& baseColor)
    {
        return ThemeOffsetColor(baseColor, UniformThemeOffset(-22, -28));
    }

    inline QColor WhiteColor(const int alphaValue = 255)
    {
        return QColor(255, 255, 255, ClampChannel(alphaValue));
    }

    inline QColor BlackColor(const int alphaValue = 255)
    {
        return QColor(0, 0, 0, ClampChannel(alphaValue));
    }

    inline QString ThemeColorName(const QColor& colorValue)
    {
        return colorValue.name(QColor::HexRgb).toUpper();
    }

    inline QString RgbaColorName(const QColor& colorValue, const int alphaValue)
    {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(colorValue.red())
            .arg(colorValue.green())
            .arg(colorValue.blue())
            .arg(ClampChannel(alphaValue));
    }

    inline double RelativeLuminance(const QColor& colorValue)
    {
        const auto linearize = [](const int channelValue) {
            const double channel = static_cast<double>(channelValue) / 255.0;
            return channel <= 0.03928
                ? channel / 12.92
                : std::pow((channel + 0.055) / 1.055, 2.4);
        };

        return 0.2126 * linearize(colorValue.red())
            + 0.7152 * linearize(colorValue.green())
            + 0.0722 * linearize(colorValue.blue());
    }

    inline double ContrastRatio(const QColor& firstColor, const QColor& secondColor)
    {
        const double firstLuminance = RelativeLuminance(firstColor);
        const double secondLuminance = RelativeLuminance(secondColor);
        const double brighter = qMax(firstLuminance, secondLuminance);
        const double darker = qMin(firstLuminance, secondLuminance);
        return (brighter + 0.05) / (darker + 0.05);
    }

    // EnsureTextContrast keeps the hue where possible, then moves only the
    // HSL lightness until the requested WCAG-style ratio is reached.
    inline QColor EnsureTextContrast(
        const QColor& preferredColor,
        const QColor& backgroundColor,
        const double minimumRatio = 4.5)
    {
        QColor candidate = preferredColor;
        candidate.setAlpha(255);
        if (ContrastRatio(candidate, backgroundColor) >= minimumRatio)
        {
            return candidate;
        }

        int hue = -1;
        int saturation = 0;
        int lightness = 0;
        int alpha = 255;
        candidate.getHsl(&hue, &saturation, &lightness, &alpha);

        const bool shouldLighten = RelativeLuminance(backgroundColor) < 0.5;
        const auto findAdjustedColor = [&](const bool lighten) -> QColor {
            for (int lightnessOffset = 4; lightnessOffset <= 255; lightnessOffset += 4)
            {
                QColor adjustedColor = candidate;
                const int adjustedLightness = lighten
                    ? qMin(255, lightness + lightnessOffset)
                    : qMax(0, lightness - lightnessOffset);
                adjustedColor.setHsl(hue, saturation, adjustedLightness, 255);
                if (ContrastRatio(adjustedColor, backgroundColor) >= minimumRatio)
                {
                    return adjustedColor;
                }
            }
            return QColor();
        };

        const QColor preferredDirectionColor = findAdjustedColor(shouldLighten);
        if (preferredDirectionColor.isValid())
        {
            return preferredDirectionColor;
        }

        const QColor oppositeDirectionColor = findAdjustedColor(!shouldLighten);
        if (oppositeDirectionColor.isValid())
        {
            return oppositeDirectionColor;
        }

        const QColor whiteColor = WhiteColor();
        const QColor blackColor = BlackColor();
        return ContrastRatio(whiteColor, backgroundColor) >= ContrastRatio(blackColor, backgroundColor)
            ? whiteColor
            : blackColor;
    }

    // ==============================
    // Theme state and neutral surfaces
    // ==============================

    inline const char* DarkModePropertyKey = "ksword_dark_mode_enabled";

    inline void SetDarkModeEnabled(const bool enabled)
    {
        if (qApp != nullptr)
        {
            qApp->setProperty(DarkModePropertyKey, enabled);
        }
    }

    inline bool IsDarkModeEnabled()
    {
        return qApp != nullptr && qApp->property(DarkModePropertyKey).toBool();
    }

    // 以下配置的 dark/light 分别是深色与浅色模式的独立数字。
    // 每个配置必须与其颜色函数使用的基础色保持一致，避免通道截断后变成纯黑或纯白。
    inline constexpr ThemeRgbOffset WindowOffset{
        { -245, -240, -233 },
        { -7, -4, 0 }
    };
    inline constexpr ThemeRgbOffset SurfaceOffset{
        { -238, -230, -219 },
        { 0, 0, 0 }
    };
    inline constexpr ThemeRgbOffset SurfaceAltOffset{
        { 7, 10, 14 },
        { -12, -7, 0 }
    };
    inline constexpr ThemeRgbOffset SurfaceMutedOffset{
        { 13, 18, 24 },
        { -29, -14, 0 }
    };
    inline constexpr ThemeRgbOffset BorderOffset{
        { 38, 55, 70 },
        { -65, -44, -22 }
    };
    inline constexpr ThemeRgbOffset BorderStrongOffset{
        { 55, 80, 102 },
        { -104, -65, -24 }
    };
    inline constexpr ThemeRgbOffset TextPrimaryOffset{
        { -18, -9, 0 },
        { -239, -220, -201 }
    };
    inline constexpr ThemeRgbOffset TextSecondaryOffset{
        { -76, -52, -11 },
        { -176, -156, -135 }
    };
    inline constexpr ThemeRgbOffset TextDisabledOffset{
        { -130, -109, -84 },
        { -129, -113, -95 }
    };
    inline constexpr ThemeRgbOffset PaletteDarkOffset{
        { 3, 5, 6 },
        { -111, -90, -67 }
    };

    inline QColor WindowColor()
    {
        return ThemeOffsetColor(WhiteColor(), WindowOffset);
    }

    inline QColor SurfaceColor()
    {
        return ThemeOffsetColor(WhiteColor(), SurfaceOffset);
    }

    inline QColor SurfaceAltColor()
    {
        return ThemeOffsetColor(SurfaceColor(), SurfaceAltOffset);
    }

    inline QColor SurfaceMutedColor()
    {
        // SurfaceMutedOffset 的两组数字都相对于 SurfaceColor 计算。
        return ThemeOffsetColor(SurfaceColor(), SurfaceMutedOffset);
    }

    inline QColor BorderColor()
    {
        return ThemeOffsetColor(SurfaceColor(), BorderOffset);
    }

    inline QColor BorderStrongColor()
    {
        return ThemeOffsetColor(SurfaceColor(), BorderStrongOffset);
    }

    inline QColor PaletteDarkColor()
    {
        return ThemeOffsetColor(SurfaceColor(), PaletteDarkOffset);
    }

    inline QColor TextPrimaryColor()
    {
        // 文字偏移量以白色为统一基准，深色模式由此得到亮色文字。
        return ThemeOffsetColor(WhiteColor(), TextPrimaryOffset);
    }

    inline QColor TextSecondaryColor()
    {
        return ThemeOffsetColor(WhiteColor(), TextSecondaryOffset);
    }

    inline QColor TextDisabledColor()
    {
        return ThemeOffsetColor(WhiteColor(), TextDisabledOffset);
    }

    inline QString WindowColorHex() { return ThemeColorName(WindowColor()); }
    inline QString SurfaceColorHex() { return ThemeColorName(SurfaceColor()); }
    inline QString SurfaceAltColorHex() { return ThemeColorName(SurfaceAltColor()); }
    inline QString SurfaceMutedColorHex() { return ThemeColorName(SurfaceMutedColor()); }
    inline QString BorderColorHex() { return ThemeColorName(BorderColor()); }
    inline QString BorderStrongColorHex() { return ThemeColorName(BorderStrongColor()); }
    inline QString TextPrimaryColorHex() { return ThemeColorName(TextPrimaryColor()); }
    inline QString TextSecondaryColorHex() { return ThemeColorName(TextSecondaryColor()); }
    inline QString TextDisabledColorHex() { return ThemeColorName(TextDisabledColor()); }

    // ==============================
    // Named accent seeds and offsets
    // ==============================

    enum class AccentRole
    {
        Blue,
        Purple,
        Green,
        Orange,
        Cyan,
        Yellow,
        Red,
        Teal,
        Indigo,
        Brown,
        Lime,
        Slate,
        Violet
    };

    inline QColor AccentSeed(const AccentRole role)
    {
        switch (role)
        {
        case AccentRole::Blue: return QColor(67, 160, 255);
        case AccentRole::Purple: return QColor(184, 99, 255);
        case AccentRole::Green: return QColor(47, 125, 50);
        case AccentRole::Orange: return QColor(217, 119, 6);
        case AccentRole::Cyan: return QColor(0, 188, 212);
        case AccentRole::Yellow: return QColor(245, 158, 11);
        case AccentRole::Red: return QColor(220, 50, 47);
        case AccentRole::Teal: return QColor(0, 150, 136);
        case AccentRole::Indigo: return QColor(63, 81, 181);
        case AccentRole::Brown: return QColor(121, 85, 72);
        case AccentRole::Lime: return QColor(139, 195, 74);
        case AccentRole::Slate: return QColor(96, 125, 139);
        case AccentRole::Violet: return QColor(121, 76, 210);
        }
        return QColor(67, 160, 255);
    }

    // AccentColor 作用：按深色、浅色两套独立亮度偏移生成强调色。
    // 调用方需要自定义亮度时必须同时传入 darkOffset 与 lightOffset，禁止复用单一数字。
    inline QColor AccentColor(
        const AccentRole role,
        const int darkOffset,
        const int lightOffset)
    {
        return ThemeOffsetColor(
            AccentSeed(role),
            UniformThemeOffset(darkOffset, lightOffset));
    }

    // 兼容旧调用点：把原有的相对调整量立即展开成深色、浅色两套总偏移。
    // 新增颜色必须优先调用三参数版本，直接写明两种主题的独立数字。
    inline QColor AccentColor(
        const AccentRole role,
        const int legacyAdjustment)
    {
        return AccentColor(role, 18 + legacyAdjustment, -8 + legacyAdjustment);
    }

    // 默认强调色也明确保留两套数字：深色背景提高亮度，浅色背景略微压低亮度。
    inline QColor AccentColor(const AccentRole role)
    {
        return AccentColor(role, 18, -8);
    }

    inline QColor AccentTextColor(
        const AccentRole role,
        const QColor& backgroundColor = QColor())
    {
        const QColor effectiveBackground = backgroundColor.isValid()
            ? backgroundColor
            : SurfaceColor();
        return EnsureTextContrast(AccentColor(role), effectiveBackground);
    }

    inline QString AccentHex(
        const AccentRole role,
        const int darkOffset,
        const int lightOffset)
    {
        return ThemeColorName(AccentColor(role, darkOffset, lightOffset));
    }

    // 兼容旧样式构建器，内部仍然转换为两套独立总偏移后再取当前主题。
    inline QString AccentHex(
        const AccentRole role,
        const int legacyAdjustment)
    {
        return ThemeColorName(AccentColor(role, legacyAdjustment));
    }

    inline QString AccentHex(const AccentRole role)
    {
        return ThemeColorName(AccentColor(role));
    }

    inline QColor SuccessColor() { return AccentTextColor(AccentRole::Green); }
    inline QColor WarningColor() { return AccentTextColor(AccentRole::Orange); }
    inline QColor ErrorColor() { return AccentTextColor(AccentRole::Red); }
    inline QColor InfoColor() { return AccentTextColor(AccentRole::Blue); }
    inline QString SuccessHex() { return ThemeColorName(SuccessColor()); }
    inline QString WarningHex() { return ThemeColorName(WarningColor()); }
    inline QString ErrorHex() { return ThemeColorName(ErrorColor()); }
    inline QString InfoHex() { return ThemeColorName(InfoColor()); }

    // 语义背景与编辑器状态色都使用独立的深浅 RGB 偏移，基础色统一为 SurfaceColor。
    inline constexpr ThemeRgbOffset SuccessBackgroundOffset{
        { 8, 32, 14 },
        { -32, 0, -28 }
    };
    inline constexpr ThemeRgbOffset WarningBackgroundOffset{
        { 38, 24, 4 },
        { 0, -24, -62 }
    };
    inline constexpr ThemeRgbOffset ErrorBackgroundOffset{
        { 36, 4, 4 },
        { 0, -31, -31 }
    };
    inline constexpr ThemeRgbOffset EditorMatchOffset{
        { 10, 28, 6 },
        { -8, -10, -42 }
    };
    inline constexpr ThemeRgbOffset EditorCurrentMatchOffset{
        { 20, 62, 10 },
        { -8, -42, -1 }
    };

    inline QColor SuccessBackgroundColor()
    {
        return ThemeOffsetColor(SurfaceColor(), SuccessBackgroundOffset);
    }

    inline QColor WarningBackgroundColor()
    {
        return ThemeOffsetColor(SurfaceColor(), WarningBackgroundOffset);
    }

    inline QColor ErrorBackgroundColor()
    {
        return ThemeOffsetColor(SurfaceColor(), ErrorBackgroundOffset);
    }

    inline QColor EditorMatchColor()
    {
        return ThemeOffsetColor(SurfaceColor(), EditorMatchOffset);
    }

    inline QColor EditorCurrentMatchColor()
    {
        return ThemeOffsetColor(SurfaceColor(), EditorCurrentMatchOffset);
    }

    inline QColor EditorSelectionColor()
    {
        return AccentColor(AccentRole::Blue, -2, -28);
    }

    inline QColor OnAccentColor()
    {
        return EnsureTextContrast(WhiteColor(), AccentColor(AccentRole::Blue));
    }
    inline QString OnAccentHex() { return ThemeColorName(OnAccentColor()); }

    // ==============================
    // Reusable chart roles
    // ==============================

    enum class PerformanceRole
    {
        Cpu,
        Memory,
        Disk,
        Network,
        Gpu,
        Read,
        Write,
        DedicatedMemory,
        SharedMemory,
        VideoEncode,
        VideoDecode,
        Copy
    };

    inline QColor PerformanceColor(const PerformanceRole role)
    {
        // 每个性能角色显式写出深色/浅色两套总偏移，避免共享亮度参数导致主题失真。
        switch (role)
        {
        case PerformanceRole::Cpu: return AccentColor(AccentRole::Blue, 40, 14);
        case PerformanceRole::Memory: return AccentColor(AccentRole::Purple);
        case PerformanceRole::Disk: return AccentColor(AccentRole::Green, 40, 14);
        case PerformanceRole::Network: return AccentColor(AccentRole::Orange, 26, 0);
        case PerformanceRole::Gpu: return AccentColor(AccentRole::Blue, 26, 0);
        case PerformanceRole::Read: return AccentColor(AccentRole::Blue, 30, 4);
        case PerformanceRole::Write: return AccentColor(AccentRole::Orange, 40, 14);
        case PerformanceRole::DedicatedMemory: return AccentColor(AccentRole::Blue, 30, 4);
        case PerformanceRole::SharedMemory: return AccentColor(AccentRole::Cyan, 22, -4);
        case PerformanceRole::VideoEncode: return AccentColor(AccentRole::Blue, 36, 10);
        case PerformanceRole::VideoDecode: return AccentColor(AccentRole::Blue, 48, 22);
        case PerformanceRole::Copy: return AccentColor(AccentRole::Cyan, 36, 10);
        }
        return AccentColor(AccentRole::Blue);
    }

    enum class TimelineRole
    {
        Process,
        Thread,
        Image,
        File,
        Registry,
        Network,
        Dns,
        PowerShell,
        Wmi,
        Security,
        Storage,
        Kernel
    };

    inline QColor TimelineColor(const TimelineRole role)
    {
        // 时间线角色同样独立配置两种主题，所有数值都是相对于 AccentSeed 的总偏移。
        switch (role)
        {
        case TimelineRole::Process: return AccentColor(AccentRole::Green, 40, 14);
        case TimelineRole::Thread: return AccentColor(AccentRole::Lime, 26, 0);
        case TimelineRole::Image: return AccentColor(AccentRole::Cyan, 28, 2);
        case TimelineRole::File: return AccentColor(AccentRole::Blue, 36, 10);
        case TimelineRole::Registry: return AccentColor(AccentRole::Purple, 8, -18);
        case TimelineRole::Network: return AccentColor(AccentRole::Orange, 36, 10);
        case TimelineRole::Dns: return AccentColor(AccentRole::Yellow, 26, 0);
        case TimelineRole::PowerShell: return AccentColor(AccentRole::Indigo, 36, 10);
        case TimelineRole::Wmi: return AccentColor(AccentRole::Teal, 28, 2);
        case TimelineRole::Security: return AccentColor(AccentRole::Red);
        case TimelineRole::Storage: return AccentColor(AccentRole::Brown, 26, 0);
        case TimelineRole::Kernel: return AccentColor(AccentRole::Slate, 26, 0);
        }
        return AccentColor(AccentRole::Blue);
    }

    // ==============================
    // Compatibility helpers used by existing style builders
    // ==============================

    inline const QColor PrimaryBlueColor = AccentSeed(AccentRole::Blue);
    // These compatibility values are intentionally palette roles: existing QSS
    // builders therefore follow the active light/dark palette at render time.
    inline const QString PrimaryBlueHex = QStringLiteral("palette(highlight)");
    inline const QString PrimaryBlueHoverHex = QStringLiteral("palette(highlight)");
    inline const QString PrimaryBluePressedHex = QStringLiteral("palette(highlight)");
    inline const QString PrimaryBlueBorderHex = QStringLiteral("palette(highlight)");
    inline const QString PrimaryBlueActiveHex = QStringLiteral("palette(highlight)");

    inline constexpr ThemeRgbOffset PrimaryBlueSubtleOffset{
        { 6, 28, 47 },
        { -21, -11, 0 }
    };
    inline constexpr ThemeRgbOffset PrimaryBlueSurfacePressedOffset{
        { -1, -1, 26 },
        { -41, -19, 0 }
    };
    inline constexpr ThemeRgbOffset ExitedRowBackgroundOffset{
        { 26, 28, 28 },
        { -19, -13, -7 }
    };

    inline QColor PrimaryBlueSubtleColor()
    {
        return ThemeOffsetColor(SurfaceColor(), PrimaryBlueSubtleOffset);
    }

    inline QString PrimaryBlueSubtleHex()
    {
        return ThemeColorName(PrimaryBlueSubtleColor());
    }

    inline QString PrimaryBlueSolidHoverHex()
    {
        return AccentHex(AccentRole::Blue, 6, -20);
    }

    inline QColor PrimaryBlueSurfacePressedColor()
    {
        return ThemeOffsetColor(SurfaceColor(), PrimaryBlueSurfacePressedOffset);
    }

    inline QString SurfaceHex() { return QStringLiteral("palette(base)"); }
    inline QString SurfaceAltHex() { return QStringLiteral("palette(alternate-base)"); }
    inline QString BorderHex() { return QStringLiteral("palette(mid)"); }
    inline QString TextPrimaryHex() { return QStringLiteral("palette(text)"); }
    inline QString TextSecondaryHex() { return QStringLiteral("palette(mid)"); }

    inline QString ThemedButtonStyle()
    {
        return QStringLiteral(
            "QPushButton,QToolButton{"
            "background-color:%1 !important;color:%2 !important;border:1px solid %3 !important;"
            "border-radius:3px;padding:4px 10px;font-weight:600;}"
            "QPushButton:hover,QToolButton:hover{background-color:%4 !important;color:%5 !important;border-color:%4 !important;}"
            "QPushButton:pressed,QToolButton:pressed{background-color:%6 !important;color:%5 !important;border-color:%6 !important;}"
            "QPushButton:disabled,QToolButton:disabled{background-color:%1 !important;color:%7 !important;border-color:%3 !important;}")
            .arg(SurfaceAltHex())
            .arg(TextPrimaryHex())
            .arg(BorderHex())
            .arg(PrimaryBlueSolidHoverHex())
            .arg(OnAccentHex())
            .arg(PrimaryBluePressedHex)
            .arg(TextSecondaryHex());
    }

    inline QString ContextMenuStyle()
    {
        return QStringLiteral(
            "QMenu{background-color:%1 !important;color:%2 !important;border:1px solid %3 !important;padding:3px;}"
            "QMenu::item{color:%2 !important;padding:5px 18px 5px 14px;background-color:transparent !important;}"
            "QMenu::item:selected{background-color:%4 !important;color:%5 !important;}"
            "QMenu::item:disabled{color:%6 !important;background-color:transparent !important;}"
            "QMenu::separator{height:1px;background-color:%3;margin:2px 6px;}")
            .arg(SurfaceColorHex())
            .arg(TextPrimaryColorHex())
            .arg(BorderColorHex())
            .arg(PrimaryBlueHex)
            .arg(OnAccentHex())
            .arg(TextDisabledColorHex());
    }

    inline QString OpaqueDialogStyle(const QString& dialogObjectName)
    {
        if (dialogObjectName.trimmed().isEmpty())
        {
            return QString();
        }

        return QStringLiteral(
            "QDialog#%1{background-color:palette(window) !important;color:palette(text) !important;}"
            "QDialog#%1 QPlainTextEdit,QDialog#%1 QTextEdit,QDialog#%1 QTreeWidget,"
            "QDialog#%1 QTableWidget,QDialog#%1 QAbstractScrollArea,QDialog#%1 QAbstractScrollArea::viewport{"
            "background-color:palette(base) !important;color:palette(text) !important;}"
            "QDialog#%1 QHeaderView::section{background-color:palette(window) !important;color:palette(text) !important;}"
            "QDialog#%1 QMenu{background-color:palette(base) !important;color:palette(text) !important;border:1px solid palette(mid) !important;}"
            "QDialog#%1 QMenu::item:selected{background-color:%2 !important;color:%3 !important;}"
            "QDialog#%1 QMenu::separator{height:1px;background-color:palette(mid) !important;}")
            .arg(dialogObjectName)
            .arg(PrimaryBlueHex)
            .arg(OnAccentHex());
    }

    inline QColor NewRowBackgroundColor() { return SuccessBackgroundColor(); }
    inline QColor ExitedRowBackgroundColor()
    {
        return ThemeOffsetColor(SurfaceColor(), ExitedRowBackgroundOffset);
    }
    inline QColor ExitedRowForegroundColor() { return TextSecondaryColor(); }
    inline QColor WarningAccentColor() { return WarningColor(); }
} // namespace KswordTheme
