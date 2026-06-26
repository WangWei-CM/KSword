#pragma once

#include "../Core/Win32Lean.h"

namespace Ksword::Ui {

// Theme centralizes only the lightweight Win32 color palette. Inputs are none;
// processing creates brush resources through ensure(); consumers read handles
// and colors directly; shutdown releases resources and returns no value. Fonts
// intentionally stay outside this class so controls can follow the Windows
// system UI message font instead of a project-specific font wrapper.
class Theme final {
public:
    Theme();
    ~Theme();

    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;

    void ensure();
    void shutdown();

    HBRUSH windowBrush() const;
    HBRUSH panelBrush() const;
    HBRUSH accentBrush() const;

    COLORREF windowColor = RGB(246, 247, 249);
    COLORREF panelColor = RGB(255, 255, 255);
    COLORREF borderColor = RGB(210, 214, 220);
    COLORREF accentColor = RGB(0, 100, 251);
    COLORREF accentDarkColor = RGB(0, 76, 190);
    COLORREF textColor = RGB(31, 35, 40);
    COLORREF mutedTextColor = RGB(96, 103, 112);

private:
    HBRUSH windowBrush_;
    HBRUSH panelBrush_;
    HBRUSH accentBrush_;
};

// AppTheme returns the singleton palette. There is no input; output is the theme
// object used by every Win32 control in this project.
Theme& AppTheme();

} // namespace Ksword::Ui
