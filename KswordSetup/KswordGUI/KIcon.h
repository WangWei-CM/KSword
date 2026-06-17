#ifndef KSWORD_GUI_KICON_H
#define KSWORD_GUI_KICON_H

// Keep FLTK deprecated aliases disabled for newer FLTK headers and strict builds.
#define FL_NO_DEPRECATED

#include "Fl.H"

#include <string>

class Fl_Image;

// KIconColorRole names a semantic KTheme palette slot used by SVG placeholders.
enum class KIconColorRole {
    Primary,
    PrimaryLight,
    PrimaryDark,
    Text,
    Muted,
    MutedText = Muted,
    Border,
    Danger,
    Warning,
    Success,
    WindowBg,
    PanelBg,
    ControlBg
};

// KIcon is a static SVG icon loader for files stored under KswordGUI/Icon.
class KIcon {
public:
    // Input is a path below KswordGUI/Icon, such as "system/settings_1_line.svg".
    // The loader reads the original SVG, replaces theme placeholders with the
    // current KThemeManager colors, scales to size when size > 0, and returns a
    // cached FLTK image pointer. The caller does not own the returned object.
    static Fl_Image* LoadThemedSvg(const std::string& name, KIconColorRole role = KIconColorRole::Primary, int size = 24);

    // Input is category plus file name, such as "system" and "settings_1_line".
    // Processing is identical to the single-name overload after joining the
    // two path parts. The returned pointer is cached and owned by KIcon.
    static Fl_Image* LoadThemedSvg(const std::string& category, const std::string& fileName, KIconColorRole role, int size = 24);

    // Input is a path below KswordGUI/Icon plus an explicit FLTK color used for
    // generic SVG tint tokens and the built-in icon accent color. Named theme
    // placeholders still resolve from KThemeManager. The returned image is cached.
    static Fl_Image* LoadThemedSvg(const std::string& name, Fl_Color color, int size = 24);

    // Input is category plus file name and an explicit FLTK tint color. The
    // method joins the parts, themes the SVG, and returns a cached image owned
    // by KIcon or nullptr when the file or SVG data cannot be loaded.
    static Fl_Image* LoadThemedSvg(const std::string& category, const std::string& fileName, Fl_Color color, int size = 24);

    // Input is a path below KswordGUI/Icon and a semantic tint role. The method
    // returns themed SVG text for callers that need markup instead of a FLTK
    // image. The returned string is a copy; cached storage remains internal.
    static std::string ThemedSvgText(const std::string& name, KIconColorRole role = KIconColorRole::Primary);

    // Input is a path below KswordGUI/Icon and an explicit tint color. The method
    // applies named KThemeManager placeholders plus the explicit generic tint and
    // returns themed SVG markup as a copied string.
    static std::string ThemedSvgText(const std::string& name, Fl_Color color);

    // Input is a caller-provided icon name or path. The method normalizes it,
    // keeps lookup inside KswordGUI/Icon, appends .svg when needed, and returns
    // an existing SVG path or an empty string when no safe file is found.
    static std::string ResolveSvgPath(const std::string& name);

    // Clears all cached themed SVG text and FLTK image objects. Existing image
    // pointers returned earlier become invalid after this call. Cache keys also
    // include the active theme colors, so normal theme changes may simply create
    // new entries; call this method when reclaiming old themed images is needed.
    static void ClearCache();

    // Compatibility wrapper for code that uses snake_case helper naming. Input
    // is none, processing delegates to ClearCache, and no value is returned.
    static void clear_cache();

    // Input is an icon path, role, and size. The method checks whether the exact
    // resolved path, active theme color signature, tint role, and size already
    // have a cached image. It returns true only for a live cached FLTK image.
    static bool Cached(const std::string& name, KIconColorRole role = KIconColorRole::Primary, int size = 24);

    // Input is any FLTK color value. The method resolves RGB channels through
    // FLTK and returns uppercase SVG-compatible #RRGGBB text.
    static std::string ColorToHex(Fl_Color color);

private:
    // KIcon has no instance state; construction is disabled for utility usage.
    KIcon();
};

// FlatIcon keeps older flat-style naming code source-compatible with KIcon.
using FlatIcon = KIcon;

#endif // KSWORD_GUI_KICON_H
