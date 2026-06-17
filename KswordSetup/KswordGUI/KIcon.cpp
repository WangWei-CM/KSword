#include "KIcon.h"

#include "KTheme.h"

#include "Fl_RGB_Image.H"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>


struct NSVGimage;

// Local declaration mirrors FLTK's Fl_SVG_Image layout because this project keeps
// FLTK headers in a flat include directory while the bundled SVG header includes
// <FL/...> paths. Inputs, processing, and returns remain FLTK-owned at link time.
class FL_EXPORT Fl_SVG_Image : public Fl_RGB_Image {
private:
    typedef struct {
        NSVGimage* svg_image;
        int ref_count;
    } counted_NSVGimage;
    counted_NSVGimage* counted_svg_image_;
    bool rasterized_;
    int raster_w_;
    int raster_h_;
    bool to_desaturate_;
    Fl_Color average_color_;
    float average_weight_;
    float svg_scaling_(int W, int H);
    void rasterize_(int W, int H);
    void cache_size_(int& width, int& height) FL_OVERRIDE;
    void init_(const char* name, const unsigned char* filedata, std::size_t length);
    Fl_SVG_Image(const Fl_SVG_Image* source);

public:
    bool proportional;
    Fl_SVG_Image(const char* filename);
    Fl_SVG_Image(const char* sharedname, const char* svg_data);
    Fl_SVG_Image(const char* sharedname, const unsigned char* svg_data, std::size_t length);
    virtual ~Fl_SVG_Image();
    Fl_Image* copy(int W, int H) const FL_OVERRIDE;
    Fl_Image* copy() const {
        return Fl_Image::copy();
    }
    void resize(int width, int height);
    void desaturate() FL_OVERRIDE;
    void color_average(Fl_Color c, float i) FL_OVERRIDE;
    void draw(int X, int Y, int W, int H, int cx = 0, int cy = 0) FL_OVERRIDE;
    void draw(int X, int Y) {
        draw(X, Y, w(), h(), 0, 0);
    }
    Fl_SVG_Image* as_svg_image() FL_OVERRIDE {
        return this;
    }
    void normalize() FL_OVERRIDE;
};

namespace {
// KIconCacheEntry owns the themed SVG text and parsed FLTK image returned to callers.
struct KIconCacheEntry {
    std::string svg_text;
    std::unique_ptr<Fl_SVG_Image> image;
};

// ThemeColorEntry stores one placeholder name and its resolved SVG hex color.
struct ThemeColorEntry {
    std::string name;
    std::string hex;
};

// ImageCache maps resolved path + full color signature + size to parsed SVG images.
using ImageCache = std::map<std::string, std::shared_ptr<KIconCacheEntry>>;

// TextCache maps resolved path + full color signature to themed SVG text buffers.
using TextCache = std::map<std::string, std::string>;

// CacheMutex serializes cache reads and writes because FLTK widgets may share icons.
std::mutex& CacheMutex() {
    static std::mutex mutex;
    return mutex;
}

// CachedImages returns the process-wide cache of FLTK image objects.
ImageCache& CachedImages() {
    static ImageCache cache;
    return cache;
}

// CachedTexts returns the process-wide cache of themed SVG markup strings.
TextCache& CachedTexts() {
    static TextCache cache;
    return cache;
}

// NormalizeSlashes makes path comparisons stable across Windows and FLTK APIs.
std::string NormalizeSlashes(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

// LowerCopy returns a lower-case copy for extension and prefix checks.
std::string LowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// UpperCopy returns an upper-case copy for percent-style placeholder names.
std::string UpperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

// StartsWith performs a stable prefix test for normalized paths and tokens.
bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// IsAbsolutePath detects Windows drive paths, UNC paths, and POSIX-style roots.
bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return path.size() > 1 && path[1] == ':';
}

// HasSvgExtension checks whether the caller supplied a .svg extension.
bool HasSvgExtension(const std::string& path) {
    const std::string lower = LowerCopy(path);
    return lower.size() >= 4 && lower.substr(lower.size() - 4) == ".svg";
}

// EnsureSvgExtension appends .svg when callers pass names such as system/add_line.
std::string EnsureSvgExtension(const std::string& path) {
    return HasSvgExtension(path) ? path : path + ".svg";
}

// StripKnownIconPrefix keeps API callers free to pass KswordGUI/Icon/system/add.svg.
std::string StripKnownIconPrefix(const std::string& path) {
    const std::string normalized = NormalizeSlashes(path);
    const std::string lower = LowerCopy(normalized);
    const std::string projectPrefix = "kswordframe3.0/kswordgui/icon/";
    const std::string guiPrefix = "kswordgui/icon/";
    const std::string iconPrefix = "icon/";
    if (StartsWith(lower, projectPrefix)) {
        return normalized.substr(projectPrefix.size());
    }
    if (StartsWith(lower, guiPrefix)) {
        return normalized.substr(guiPrefix.size());
    }
    if (StartsWith(lower, iconPrefix)) {
        return normalized.substr(iconPrefix.size());
    }
    return normalized;
}

// StripAbsoluteIconPrefix accepts absolute paths only when they point below KswordGUI/Icon.
std::string StripAbsoluteIconPrefix(const std::string& path) {
    const std::string normalized = NormalizeSlashes(path);
    const std::string lower = LowerCopy(normalized);
    const std::string marker = "/kswordgui/icon/";
    const std::size_t markerPos = lower.find(marker);
    if (markerPos == std::string::npos) {
        return std::string();
    }
    return normalized.substr(markerPos + marker.size());
}

// HasUnsafeSegment rejects empty, current-directory, and parent-directory path parts.
bool HasUnsafeSegment(const std::string& path) {
    std::istringstream stream(path);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty() || segment == "." || segment == "..") {
            return true;
        }
    }
    return false;
}

// SourceIconRoot derives the built-in Icon directory from this source file path.
std::string SourceIconRoot() {
    const std::string sourceFile = NormalizeSlashes(__FILE__);
    const std::size_t slash = sourceFile.find_last_of('/');
    if (slash == std::string::npos) {
        return "Icon";
    }
    return sourceFile.substr(0, slash) + "/Icon";
}

// FileExists performs a simple readability check without throwing exceptions.
bool FileExists(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    return file.good();
}

// ReadTextFile loads the full SVG text into memory for placeholder replacement.
std::string ReadTextFile(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) {
        return std::string();
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

// ReplaceAll substitutes one token throughout an SVG string and advances safely.
void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

// ReplaceAttributeValue replaces role names only when used as paint attribute values.
void ReplaceAttributeValue(std::string& svg, const std::string& attribute, const std::string& role, const std::string& hex) {
    ReplaceAll(svg, attribute + "=\"" + role + "\"", attribute + "=\"" + hex + "\"");
    ReplaceAll(svg, attribute + "='" + role + "'", attribute + "='" + hex + "'");
}

// ReplaceStyleValue replaces CSS-style paint placeholders inside SVG style attributes.
void ReplaceStyleValue(std::string& svg, const std::string& property, const std::string& role, const std::string& hex) {
    ReplaceAll(svg, property + ":" + role + ";", property + ":" + hex + ";");
    ReplaceAll(svg, property + ": " + role + ";", property + ": " + hex + ";");
    ReplaceAll(svg, property + ":" + role + " ", property + ":" + hex + " ");
    ReplaceAll(svg, property + ": " + role + " ", property + ": " + hex + " ");
    ReplaceAll(svg, property + ":" + role + "\"", property + ":" + hex + "\"");
    ReplaceAll(svg, property + ": " + role + "\"", property + ": " + hex + "\"");
    ReplaceAll(svg, property + ":" + role + "'", property + ":" + hex + "'");
    ReplaceAll(svg, property + ": " + role + "'", property + ": " + hex + "'");
}

// RoleColor resolves a semantic KIconColorRole against the active KThemeManager palette.
Fl_Color RoleColor(KIconColorRole role) {
    const KTheme& theme = KThemeManager::instance().theme();
    switch (role) {
    case KIconColorRole::Primary:
        return theme.primary;
    case KIconColorRole::PrimaryLight:
        return theme.primaryLight;
    case KIconColorRole::PrimaryDark:
        return theme.primaryDark;
    case KIconColorRole::Text:
        return theme.text;
    case KIconColorRole::Muted:
        return theme.mutedText;
    case KIconColorRole::Border:
        return theme.border;
    case KIconColorRole::Danger:
        return theme.danger;
    case KIconColorRole::Warning:
        return theme.warning;
    case KIconColorRole::Success:
        return theme.success;
    case KIconColorRole::WindowBg:
        return theme.windowBg;
    case KIconColorRole::PanelBg:
        return theme.panelBg;
    case KIconColorRole::ControlBg:
        return theme.controlBg;
    default:
        break;
    }
    return theme.primary;
}

// ThemeColors snapshots every named SVG role that can appear as a placeholder.
std::vector<ThemeColorEntry> ThemeColors() {
    const KTheme& theme = KThemeManager::instance().theme();
    std::vector<ThemeColorEntry> colors;
    colors.push_back({ "primary", KIcon::ColorToHex(theme.primary) });
    colors.push_back({ "primary-light", KIcon::ColorToHex(theme.primaryLight) });
    colors.push_back({ "primary-dark", KIcon::ColorToHex(theme.primaryDark) });
    colors.push_back({ "text", KIcon::ColorToHex(theme.text) });
    colors.push_back({ "muted", KIcon::ColorToHex(theme.mutedText) });
    colors.push_back({ "muted-text", KIcon::ColorToHex(theme.mutedText) });
    colors.push_back({ "border", KIcon::ColorToHex(theme.border) });
    colors.push_back({ "danger", KIcon::ColorToHex(theme.danger) });
    colors.push_back({ "success", KIcon::ColorToHex(theme.success) });
    colors.push_back({ "warning", KIcon::ColorToHex(theme.warning) });
    colors.push_back({ "window-bg", KIcon::ColorToHex(theme.windowBg) });
    colors.push_back({ "panel-bg", KIcon::ColorToHex(theme.panelBg) });
    colors.push_back({ "control-bg", KIcon::ColorToHex(theme.controlBg) });
    return colors;
}

// ReplaceNamedThemeToken replaces one semantic color role in supported SVG placeholder forms.
void ReplaceNamedThemeToken(std::string& svg, const ThemeColorEntry& color) {
    const std::string& role = color.name;
    const std::string& hex = color.hex;
    const std::string upperRole = UpperCopy(role);
    ReplaceAll(svg, "{{" + role + "}}", hex);
    ReplaceAll(svg, "{{theme." + role + "}}", hex);
    ReplaceAll(svg, "${" + role + "}", hex);
    ReplaceAll(svg, "${theme." + role + "}", hex);
    ReplaceAll(svg, "%" + upperRole + "%", hex);
    ReplaceAll(svg, "%THEME_" + upperRole + "%", hex);
    ReplaceAll(svg, "var(--" + role + ")", hex);
    ReplaceAll(svg, "var(--color-" + role + ")", hex);
    ReplaceAll(svg, "var(--ksword-" + role + ")", hex);
    ReplaceAll(svg, "theme(" + role + ")", hex);
    ReplaceAttributeValue(svg, "fill", role, hex);
    ReplaceAttributeValue(svg, "stroke", role, hex);
    ReplaceAttributeValue(svg, "color", role, hex);
    ReplaceAttributeValue(svg, "stop-color", role, hex);
    ReplaceStyleValue(svg, "fill", role, hex);
    ReplaceStyleValue(svg, "stroke", role, hex);
    ReplaceStyleValue(svg, "color", role, hex);
    ReplaceStyleValue(svg, "stop-color", role, hex);
}

// ApplyThemeColors replaces named theme placeholders plus the default one-color tint tokens.
std::string ApplyThemeColors(std::string svg, const std::string& tintHex) {
    const std::vector<ThemeColorEntry> colors = ThemeColors();
    for (const ThemeColorEntry& color : colors) {
        ReplaceNamedThemeToken(svg, color);
    }

    ReplaceAll(svg, "#43A0FF", tintHex);
    ReplaceAll(svg, "#43a0ff", tintHex);
    ReplaceAll(svg, "#409EFF", tintHex);
    ReplaceAll(svg, "#409eff", tintHex);
    ReplaceAll(svg, "currentColor", tintHex);
    ReplaceAll(svg, "{{color}}", tintHex);
    ReplaceAll(svg, "{{themeColor}}", tintHex);
    ReplaceAll(svg, "${color}", tintHex);
    ReplaceAll(svg, "%THEME_COLOR%", tintHex);
    return svg;
}

// JoinIconName builds category/file.svg relative paths for convenience overloads.
std::string JoinIconName(const std::string& category, const std::string& fileName) {
    if (category.empty()) {
        return fileName;
    }
    if (fileName.empty()) {
        return category;
    }
    return NormalizeSlashes(category) + "/" + NormalizeSlashes(fileName);
}

// ColorSignature records every color that can affect rendered output for cache keys.
std::string ColorSignature(const std::string& tintHex) {
    std::ostringstream signature;
    signature << "tint=" << tintHex;
    const std::vector<ThemeColorEntry> colors = ThemeColors();
    for (const ThemeColorEntry& color : colors) {
        signature << ';' << color.name << '=' << color.hex;
    }
    return signature.str();
}

// TextCacheKey includes the resolved file path and all theme colors used in replacement.
std::string TextCacheKey(const std::string& resolvedPath, const std::string& colorSignature) {
    return NormalizeSlashes(resolvedPath) + "|" + colorSignature;
}

// ImageCacheKey adds the requested raster size to the themed SVG text cache key.
std::string ImageCacheKey(const std::string& resolvedPath, const std::string& colorSignature, int size) {
    std::ostringstream key;
    key << TextCacheKey(resolvedPath, colorSignature) << "|size=" << size;
    return key.str();
}

// ThemedSvgTextLocked returns cached themed SVG text and expects CacheMutex to be held.
std::string ThemedSvgTextLocked(const std::string& resolvedPath, const std::string& tintHex, const std::string& colorSignature) {
    const std::string key = TextCacheKey(resolvedPath, colorSignature);
    TextCache& cache = CachedTexts();
    auto found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }

    const std::string rawSvg = ReadTextFile(resolvedPath);
    if (rawSvg.empty()) {
        return std::string();
    }

    const std::string themedSvg = ApplyThemeColors(rawSvg, tintHex);
    cache[key] = themedSvg;
    return themedSvg;
}
} // namespace

Fl_Image* KIcon::LoadThemedSvg(const std::string& name, KIconColorRole role, int size) {
    // Input is a caller icon name plus semantic role; processing resolves the role to the current theme color.
    return LoadThemedSvg(name, RoleColor(role), size);
}

Fl_Image* KIcon::LoadThemedSvg(const std::string& category, const std::string& fileName, KIconColorRole role, int size) {
    // Input is split path parts; processing joins the path and delegates to the single-name loader.
    return LoadThemedSvg(JoinIconName(category, fileName), RoleColor(role), size);
}

Fl_Image* KIcon::LoadThemedSvg(const std::string& name, Fl_Color color, int size) {
    // Input path must resolve below KswordGUI/Icon; failure returns nullptr without touching source SVG files.
    const std::string resolvedPath = ResolveSvgPath(name);
    if (resolvedPath.empty()) {
        return nullptr;
    }

    // The cache key includes both explicit tint and named theme colors to survive theme switches correctly.
    const std::string tintHex = ColorToHex(color);
    const std::string colorSignature = ColorSignature(tintHex);
    const std::string key = ImageCacheKey(resolvedPath, colorSignature, size);
    std::lock_guard<std::mutex> lock(CacheMutex());

    ImageCache& cache = CachedImages();
    auto found = cache.find(key);
    if (found != cache.end()) {
        return found->second && found->second->image ? found->second->image.get() : nullptr;
    }

    // SVG text is cached separately so text-only and image callers share replacement work.
    auto entry = std::make_shared<KIconCacheEntry>();
    entry->svg_text = ThemedSvgTextLocked(resolvedPath, tintHex, colorSignature);
    if (entry->svg_text.empty()) {
        return nullptr;
    }

    // FLTK parses the themed in-memory SVG; the original SVG file remains unchanged on disk.
    entry->image.reset(new Fl_SVG_Image(nullptr, entry->svg_text.c_str()));
    if (!entry->image || entry->image->fail() != 0) {
        entry->image.reset();
        return nullptr;
    }

    // A positive size requests a square icon while preserving SVG aspect ratio.
    if (size > 0) {
        entry->image->proportional = true;
        entry->image->resize(size, size);
    }

    Fl_Image* result = entry->image.get();
    cache[key] = entry;
    return result;
}

Fl_Image* KIcon::LoadThemedSvg(const std::string& category, const std::string& fileName, Fl_Color color, int size) {
    // Input is split path parts plus explicit color; return value is the cached FLTK image pointer.
    return LoadThemedSvg(JoinIconName(category, fileName), color, size);
}

std::string KIcon::ThemedSvgText(const std::string& name, KIconColorRole role) {
    // Input role is resolved at call time so theme changes produce a new cache signature.
    return ThemedSvgText(name, RoleColor(role));
}

std::string KIcon::ThemedSvgText(const std::string& name, Fl_Color color) {
    // Input path resolves inside Icon; return value is a copy of cached themed SVG markup.
    const std::string resolvedPath = ResolveSvgPath(name);
    if (resolvedPath.empty()) {
        return std::string();
    }

    const std::string tintHex = ColorToHex(color);
    const std::string colorSignature = ColorSignature(tintHex);
    std::lock_guard<std::mutex> lock(CacheMutex());
    return ThemedSvgTextLocked(resolvedPath, tintHex, colorSignature);
}

std::string KIcon::ResolveSvgPath(const std::string& name) {
    // Empty input cannot identify an SVG file and resolves to an empty result.
    if (name.empty()) {
        return std::string();
    }

    // Absolute inputs are accepted only when they already point below KswordGUI/Icon.
    const std::string normalized = NormalizeSlashes(name);
    const std::string stripped = IsAbsolutePath(normalized) ? StripAbsoluteIconPrefix(normalized) : StripKnownIconPrefix(normalized);
    if (stripped.empty()) {
        return std::string();
    }

    // Reject traversal so the icon loader only reads original resources from the Icon directory.
    const std::string relativeName = EnsureSvgExtension(stripped);
    if (IsAbsolutePath(relativeName) || HasUnsafeSegment(relativeName)) {
        return std::string();
    }

    // Candidate roots cover source-tree, project-root, and current-working-directory layouts.
    std::vector<std::string> candidates;
    candidates.push_back(SourceIconRoot() + "/" + relativeName);
    candidates.push_back("KswordGUI/Icon/" + relativeName);
    candidates.push_back("Icon/" + relativeName);
    candidates.push_back("KswordFrame3.0/KswordGUI/Icon/" + relativeName);

    for (const std::string& candidate : candidates) {
        if (FileExists(candidate)) {
            return NormalizeSlashes(candidate);
        }
    }
    return std::string();
}

void KIcon::ClearCache() {
    // Input is none; processing destroys all cached text and images, invalidating prior returned pointers.
    std::lock_guard<std::mutex> lock(CacheMutex());
    CachedImages().clear();
    CachedTexts().clear();
}

void KIcon::clear_cache() {
    // Input is none; processing preserves compatibility by delegating to the canonical cache clearer.
    ClearCache();
}

bool KIcon::Cached(const std::string& name, KIconColorRole role, int size) {
    // Input path and role are resolved exactly like LoadThemedSvg so cache checks match loading behavior.
    const std::string resolvedPath = ResolveSvgPath(name);
    if (resolvedPath.empty()) {
        return false;
    }

    const std::string tintHex = ColorToHex(RoleColor(role));
    const std::string key = ImageCacheKey(resolvedPath, ColorSignature(tintHex), size);
    std::lock_guard<std::mutex> lock(CacheMutex());
    auto found = CachedImages().find(key);
    return found != CachedImages().end() && found->second && found->second->image;
}

std::string KIcon::ColorToHex(Fl_Color color) {
    // Input FLTK color is converted through FLTK so indexed and RGB colors both resolve correctly.
    unsigned char red = 0;
    unsigned char green = 0;
    unsigned char blue = 0;
    Fl::get_color(color, red, green, blue);

    // Return value is uppercase #RRGGBB text suitable for SVG fill, stroke, and CSS color fields.
    std::ostringstream hex;
    hex << "#"
        << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<int>(red)
        << std::setw(2) << static_cast<int>(green)
        << std::setw(2) << static_cast<int>(blue);
    return hex.str();
}
