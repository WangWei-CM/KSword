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

    inline constexpr RgbOffset UniformOffset(const int value)
    {
        return { value, value, value };
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
        return OffsetColor(baseColor, IsDarkModeEnabled() ? 10 : 18);
    }

    inline QColor ThemeDarkerColor(const QColor& baseColor)
    {
        return OffsetColor(baseColor, IsDarkModeEnabled() ? -22 : -28);
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

    inline constexpr RgbOffset WindowDarkOffset{ -245, -240, -233 };
    inline constexpr RgbOffset WindowLightOffset{ -7, -4, 0 };
    inline constexpr RgbOffset SurfaceDarkOffset{ -238, -230, -219 };
    inline constexpr RgbOffset SurfaceLightOffset{ 0, 0, 0 };
    inline constexpr RgbOffset SurfaceAltDarkOffset{ 7, 10, 14 };
    inline constexpr RgbOffset SurfaceAltLightOffset{ -12, -7, 0 };
    inline constexpr RgbOffset SurfaceMutedDarkOffset{ 13, 18, 24 };
    inline constexpr RgbOffset SurfaceMutedLightOffset{ -29, -14, 0 };
    inline constexpr RgbOffset BorderDarkOffset{ 38, 55, 70 };
    inline constexpr RgbOffset BorderLightOffset{ -65, -44, -22 };
    inline constexpr RgbOffset BorderStrongDarkOffset{ 55, 80, 102 };
    inline constexpr RgbOffset BorderStrongLightOffset{ -104, -65, -24 };
    inline constexpr RgbOffset TextPrimaryDarkOffset{ -18, -9, 0 };
    inline constexpr RgbOffset TextPrimaryLightOffset{ -239, -220, -201 };
    inline constexpr RgbOffset TextSecondaryDarkOffset{ -76, -52, -11 };
    inline constexpr RgbOffset TextSecondaryLightOffset{ -176, -156, -135 };
    inline constexpr RgbOffset TextDisabledDarkOffset{ -130, -109, -84 };
    inline constexpr RgbOffset TextDisabledLightOffset{ -129, -113, -95 };

    inline QColor WindowColor()
    {
        return OffsetColor(
            WhiteColor(),
            IsDarkModeEnabled() ? WindowDarkOffset : WindowLightOffset);
    }

    inline QColor SurfaceColor()
    {
        return OffsetColor(
            WhiteColor(),
            IsDarkModeEnabled() ? SurfaceDarkOffset : SurfaceLightOffset);
    }

    inline QColor SurfaceAltColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? SurfaceAltDarkOffset : SurfaceAltLightOffset);
    }

    inline QColor SurfaceMutedColor()
    {
        return OffsetColor(
            WhiteColor(),
            IsDarkModeEnabled() ? SurfaceMutedDarkOffset : SurfaceMutedLightOffset);
    }

    inline QColor BorderColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? BorderDarkOffset : BorderLightOffset);
    }

    inline QColor BorderStrongColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? BorderStrongDarkOffset : BorderStrongLightOffset);
    }

    inline QColor PaletteDarkColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 3, 5, 6 } : RgbOffset{ -111, -90, -67 });
    }

    inline QColor TextPrimaryColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? TextPrimaryDarkOffset : TextPrimaryLightOffset);
    }

    inline QColor TextSecondaryColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? TextSecondaryDarkOffset : TextSecondaryLightOffset);
    }

    inline QColor TextDisabledColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? TextDisabledDarkOffset : TextDisabledLightOffset);
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

    inline QColor AccentColor(const AccentRole role, const int brightnessOffset = 0)
    {
        // Dark surfaces need a lighter accent; light surfaces need a slightly
        // deeper accent. The caller may add a small role-specific offset.
        const int themeOffset = IsDarkModeEnabled() ? 18 : -8;
        return OffsetColor(AccentSeed(role), themeOffset + brightnessOffset);
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

    inline QString AccentHex(const AccentRole role, const int brightnessOffset = 0)
    {
        return ThemeColorName(AccentColor(role, brightnessOffset));
    }

    inline QColor SuccessColor() { return AccentTextColor(AccentRole::Green); }
    inline QColor WarningColor() { return AccentTextColor(AccentRole::Orange); }
    inline QColor ErrorColor() { return AccentTextColor(AccentRole::Red); }
    inline QColor InfoColor() { return AccentTextColor(AccentRole::Blue); }
    inline QString SuccessHex() { return ThemeColorName(SuccessColor()); }
    inline QString WarningHex() { return ThemeColorName(WarningColor()); }
    inline QString ErrorHex() { return ThemeColorName(ErrorColor()); }
    inline QString InfoHex() { return ThemeColorName(InfoColor()); }

    inline QColor SuccessBackgroundColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 8, 32, 14 } : RgbOffset{ -32, 0, -28 });
    }

    inline QColor WarningBackgroundColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 38, 24, 4 } : RgbOffset{ 0, -24, -62 });
    }

    inline QColor ErrorBackgroundColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 36, 4, 4 } : RgbOffset{ 0, -31, -31 });
    }

    inline QColor EditorMatchColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 10, 28, 6 } : RgbOffset{ -8, -10, -42 });
    }

    inline QColor EditorCurrentMatchColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 20, 62, 10 } : RgbOffset{ -8, -42, -1 });
    }

    inline QColor EditorSelectionColor()
    {
        return AccentColor(AccentRole::Blue, -20);
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
        switch (role)
        {
        case PerformanceRole::Cpu: return AccentColor(AccentRole::Blue, 22);
        case PerformanceRole::Memory: return AccentColor(AccentRole::Purple);
        case PerformanceRole::Disk: return AccentColor(AccentRole::Green, 22);
        case PerformanceRole::Network: return AccentColor(AccentRole::Orange, 8);
        case PerformanceRole::Gpu: return AccentColor(AccentRole::Blue, 8);
        case PerformanceRole::Read: return AccentColor(AccentRole::Blue, 12);
        case PerformanceRole::Write: return AccentColor(AccentRole::Orange, 22);
        case PerformanceRole::DedicatedMemory: return AccentColor(AccentRole::Blue, 12);
        case PerformanceRole::SharedMemory: return AccentColor(AccentRole::Cyan, 4);
        case PerformanceRole::VideoEncode: return AccentColor(AccentRole::Blue, 18);
        case PerformanceRole::VideoDecode: return AccentColor(AccentRole::Blue, 30);
        case PerformanceRole::Copy: return AccentColor(AccentRole::Cyan, 18);
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
        switch (role)
        {
        case TimelineRole::Process: return AccentColor(AccentRole::Green, 22);
        case TimelineRole::Thread: return AccentColor(AccentRole::Lime, 8);
        case TimelineRole::Image: return AccentColor(AccentRole::Cyan, 10);
        case TimelineRole::File: return AccentColor(AccentRole::Blue, 18);
        case TimelineRole::Registry: return AccentColor(AccentRole::Purple, -10);
        case TimelineRole::Network: return AccentColor(AccentRole::Orange, 18);
        case TimelineRole::Dns: return AccentColor(AccentRole::Yellow, 8);
        case TimelineRole::PowerShell: return AccentColor(AccentRole::Indigo, 18);
        case TimelineRole::Wmi: return AccentColor(AccentRole::Teal, 10);
        case TimelineRole::Security: return AccentColor(AccentRole::Red, 0);
        case TimelineRole::Storage: return AccentColor(AccentRole::Brown, 8);
        case TimelineRole::Kernel: return AccentColor(AccentRole::Slate, 8);
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

    inline QColor PrimaryBlueSubtleColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 6, 28, 47 } : RgbOffset{ -21, -11, 0 });
    }

    inline QString PrimaryBlueSubtleHex()
    {
        return ThemeColorName(PrimaryBlueSubtleColor());
    }

    inline QString PrimaryBlueSolidHoverHex()
    {
        return AccentHex(AccentRole::Blue, -12);
    }

    inline QColor PrimaryBlueSurfacePressedColor()
    {
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ -1, -1, 26 } : RgbOffset{ -41, -19, 0 });
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
        return OffsetColor(
            SurfaceColor(),
            IsDarkModeEnabled() ? RgbOffset{ 26, 28, 28 } : RgbOffset{ -19, -13, -7 });
    }
    inline QColor ExitedRowForegroundColor() { return TextSecondaryColor(); }
    inline QColor WarningAccentColor() { return WarningColor(); }
} // namespace KswordTheme
