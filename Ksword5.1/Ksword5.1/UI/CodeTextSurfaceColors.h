#pragma once

#include "../theme.h"
#include <QPalette>

namespace ks::ui
{
    // Shared surface roles for the editable and virtual read-only code views.
    // The application palette owns theme/background changes; these are values.
    struct CodeTextSurfaceColors
    {
        QColor base, gutter, gutterText, currentNumber, divider, currentLine;
    };
    inline CodeTextSurfaceColors CodeTextSurfaceColorsFor(const QPalette& palette)
    {
        CodeTextSurfaceColors colors;
        colors.base = palette.color(QPalette::Base);
        const QColor text = palette.color(QPalette::Text);
        const QColor accent = palette.color(QPalette::Highlight);
        colors.gutter = KswordTheme::BlendColors(colors.base, palette.color(QPalette::Window), 100);
        colors.gutterText = KswordTheme::EnsureTextContrast(KswordTheme::BlendColors(text, colors.gutter, 110), colors.gutter);
        colors.currentNumber = KswordTheme::EnsureTextContrast(accent, colors.gutter);
        colors.divider = KswordTheme::BlendColors(text, colors.gutter, 218);
        colors.currentLine = KswordTheme::BlendColors(colors.base, accent, 18);
        return colors;
    }
}
