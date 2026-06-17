#ifndef KSWORD_GUI_KTHEME_H
#define KSWORD_GUI_KTHEME_H

// Keep FLTK deprecated aliases disabled for newer FLTK headers and strict builds.
#define FL_NO_DEPRECATED

#include "Fl.H"
#include "Fl_Widget.H"
#include "Fl_Window.H"

#include <vector>

// KThemeMode selects the neutral palette while preserving the runtime accent color.
enum class KThemeMode {
    Light,
    Dark
};

// KTheme stores the resolved colors consumed by all K* widgets.
struct KTheme {
    Fl_Color primary = 0;
    Fl_Color primaryLight = 0;
    Fl_Color primaryDark = 0;
    Fl_Color windowBg = 0;
    Fl_Color panelBg = 0;
    Fl_Color controlBg = 0;
    Fl_Color controlAltBg = 0;
    Fl_Color text = 0;
    Fl_Color mutedText = 0;
    Fl_Color border = 0;
    Fl_Color hover = 0;
    Fl_Color pressed = 0;
    Fl_Color selection = 0;
    Fl_Color danger = 0;
    Fl_Color warning = 0;
    Fl_Color success = 0;
};

// KThemeManager owns the active theme and reapplies it to registered windows.
class KThemeManager {
public:
    // Returns the process-wide singleton; input is none, output is a stable manager reference.
    static KThemeManager& instance();

    // Returns the active resolved theme; input is none, output is a read-only palette reference.
    const KTheme& theme() const;

    // Returns the current light/dark mode; input is none, output is the active mode value.
    KThemeMode Mode() const;

    // Sets the neutral palette mode, rebuilds colors, and refreshes windows; returns no value.
    void SetMode(KThemeMode mode);

    // Toggles light/dark mode, rebuilds colors, and refreshes windows; returns no value.
    void ToggleMode();

    // Sets RGB accent channels, rebuilds derived accent colors, and refreshes windows; returns no value.
    void SetPrimaryColor(unsigned char red, unsigned char green, unsigned char blue);

    // Applies the active palette to one widget tree without scheduling redraw; widget may be null and no value is returned.
    void ApplyTo(Fl_Widget* widget);

    // Registers a live top-level window for dynamic theme refreshes without repainting it; returns no value.
    void RegisterWindow(Fl_Window* window);

    // Removes a top-level window from refresh tracking; returns no value.
    void UnregisterWindow(Fl_Window* window);

    // Reapplies the active palette and redraws each registered top-level window once; returns no value.
    void RefreshAll();

private:
    // Initializes default light mode and blue accent; construction has no external input.
    KThemeManager();

    // Recomputes every resolved color after mode/accent changes; returns no value.
    void RebuildTheme();

    // Checks if a window is already tracked; input is a pointer, output is true when present.
    bool IsRegistered(Fl_Window* window) const;

private:
    KThemeMode mode_;
    unsigned char primary_r_;
    unsigned char primary_g_;
    unsigned char primary_b_;
    KTheme theme_;
    std::vector<Fl_Window*> windows_;
};

// Compatibility macros keep older code compiling while reading the dynamic theme.
// Win32 defines COLOR_BACKGROUND as a system color index. The legacy Ksword GUI
// layer used the same macro name for theme background, so undefine it here before
// reintroducing the GUI compatibility macro.
#ifdef COLOR_BACKGROUND
#undef COLOR_BACKGROUND
#endif
#define COLOR_PRIMARY        (KThemeManager::instance().theme().primary)
#define COLOR_PRIMARY_LIGHT  (KThemeManager::instance().theme().primaryLight)
#define COLOR_PRIMARY_DARK   (KThemeManager::instance().theme().primaryDark)
#define COLOR_BACKGROUND     (KThemeManager::instance().theme().panelBg)
#define COLOR_WINDOW_BG      (KThemeManager::instance().theme().windowBg)
#define COLOR_TEXT           (KThemeManager::instance().theme().text)
#define COLOR_BORDER         (KThemeManager::instance().theme().border)
#define COLOR_BORDER_BLUE    (KThemeManager::instance().theme().primary)
#define COLOR_BORDER_BLACK   (KThemeManager::instance().theme().text)
#define COLOR_PUSHED_DOWN    (KThemeManager::instance().theme().pressed)

// Initializes global FLTK font and theme defaults; returns no value.
void InitFlatThemeGlobal();

// Applies/registers a themed window and schedules one top-level redraw; null input is ignored and no value is returned.
void SetWindowStyle(Fl_Window* win);

#endif // KSWORD_GUI_KTHEME_H
