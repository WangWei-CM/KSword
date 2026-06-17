#ifndef KSWORD_GUI_KLAYEREDWINDOW_H
#define KSWORD_GUI_KLAYEREDWINDOW_H

// Keep FLTK deprecated aliases disabled for newer FLTK headers and strict builds.
#define FL_NO_DEPRECATED

#include "Fl_Window.H"

#include <string>

struct HWND__;
using HWND = HWND__*;

// KLayeredImageWindow is a Win32 per-pixel-alpha popup for PNG surfaces.
// It complements FLTK windows when true transparent edges are required and
// Fl_Window::shape() is not enough because shape masks are binary regions.
class KLayeredImageWindow {
public:
    // Creates an empty native layered image window wrapper; no HWND exists yet.
    KLayeredImageWindow();

    // Destroys the native HWND if one was created; no value is returned.
    ~KLayeredImageWindow();

    KLayeredImageWindow(const KLayeredImageWindow&) = delete;
    KLayeredImageWindow& operator=(const KLayeredImageWindow&) = delete;

    // Creates or updates the native window from a UTF-8 PNG path and screen geometry.
    // The image is scaled into width/height using GDI+ high-quality interpolation.
    // Returns true when UpdateLayeredWindow succeeds and the window is visible.
    bool showPngAt(const std::string& pngPathUtf8, int screenX, int screenY, int width, int height, HWND owner = nullptr);

    // Creates or updates the native window from a UTF-16 PNG path and screen geometry.
    // Input path is read by GDI+, output indicates whether the layered window was updated.
    bool showPngAt(const std::wstring& pngPath, int screenX, int screenY, int width, int height, HWND owner = nullptr);

    // Shows a PNG beside an FLTK owner window. Offset is relative to owner->x/y().
    // The FLTK window must already be shown so a native HWND exists.
    bool showPngForWindow(Fl_Window* owner, const std::string& pngPathUtf8, int offsetX, int offsetY, int width, int height);

    // Moves the existing layered window without reloading pixels. Returns false when no HWND exists.
    bool moveTo(int screenX, int screenY);

    // Hides the native window if it exists; no value is returned.
    void hide();

    // Destroys the native window and clears size/owner state; no value is returned.
    void destroy();

    // Enables or disables click-through behavior for the whole layered surface.
    // Input true is useful for decorative character art; no value is returned.
    void setClickThrough(bool enabled);

    // Returns true when the wrapper currently has a live HWND.
    bool valid() const;

    // Returns the native HWND, or nullptr when the window has not been created.
    HWND hwnd() const;

    // Returns the latest width passed to showPngAt/showPngForWindow.
    int width() const;

    // Returns the latest height passed to showPngAt/showPngForWindow.
    int height() const;

private:
    // Ensures the Win32 window class is registered and the HWND exists.
    bool ensureWindow(HWND owner);

    // Applies the current extended style bits to the live HWND.
    void updateExtendedStyle();

private:
    HWND hwnd_;
    HWND owner_;
    int width_;
    int height_;
    bool click_through_;
};

#endif // KSWORD_GUI_KLAYEREDWINDOW_H
