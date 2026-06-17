#ifndef KSWORD_GUI_KTITLEBAR_H
#define KSWORD_GUI_KTITLEBAR_H

#include "Fl_Group.H"
#include "Fl_Image.H"
#include "Fl_Window.H"

#include <memory>
#include <string>

// KTitleBarStyle selects one of the four branded title-bar fills requested by
// the application shell. Each style changes only the title-bar background; the
// icon, logo, drag behavior, and caption buttons keep the same contract.
enum class KTitleBarStyle {
    Trapezoid,
    Fade,
    Solid,
    Plain
};

// KTitleBar is an FLTK widget that replaces the native non-client title area.
// Inputs are normal FLTK geometry, a title fallback, a fill style, and whether
// the maximize/restore button should be active. It draws app branding plus
// minimize/maximize/close buttons and returns standard FLTK event status codes.
class KTitleBar : public Fl_Group {
public:
    // Creates a self-drawing title bar. It does not install itself into a window;
    // callers use KInstallTitleBar() so existing content can be shifted safely.
    KTitleBar(int x, int y, int w, int h, const char* title, KTitleBarStyle style, bool showMaximize);

    // Releases loaded FLTK image objects. No external pointers are owned.
    ~KTitleBar() override;

    // Draws the selected background, app icon/logo, bottom divider, and caption buttons.
    void draw() override;

    // Handles hover, click, drag, minimize, maximize/restore, and close actions.
    int handle(int event) override;

    // Updates the visual fill style and repaints; input is the new style, return is none.
    void setStyle(KTitleBarStyle style);

    // Returns the current visual fill style; input is none.
    KTitleBarStyle style() const;

    // Enables or disables the maximize/restore button and repaints; return is none.
    void setShowMaximize(bool showMaximize);

    // Returns true when the maximize/restore button is displayed.
    bool showMaximize() const;

    // Synchronizes Win32 resize/maximize notifications back into the custom
    // chrome. Inputs are the owner width and native maximized flag; return is none.
    void syncFromNativeWindow(int ownerWidth, bool maximized);

    // Small rectangle helper so button hit-tests do not depend on Win32 RECT.
    struct Rect {
        int x;
        int y;
        int w;
        int h;
    };

private:
    // Internal caption-button identifier used by hit-testing and drawing.
    enum class Button {
        None,
        Minimize,
        Maximize,
        Close
    };

    // Tracks the custom chrome placement mode so restore, snap, and button
    // drawing stay synchronized without relying on native non-client state.
    enum class ChromeState {
        Normal,
        Maximized,
        SnappedLeft,
        SnappedRight
    };

    // Loads embedded or project-local icon/logo images lazily; returns no value.
    void ensureImages();

    // Draws the requested background fill over the full title-bar rectangle.
    void drawBackground();

    // Draws app icon and logo, with a text fallback if images cannot be loaded.
    void drawBrand();

    // Draws one caption button in its hover/pressed state.
    void drawButton(Button button);

    // Computes the absolute title-bar rectangle for one caption button.
    Rect buttonRect(Button button) const;

    // Returns which caption button contains the absolute FLTK event coordinate.
    Button hitButton(int px, int py) const;

    // Runs the action attached to a clicked caption button; returns no value.
    void triggerButton(Button button);

    // Starts a manual window move based on root-screen mouse coordinates.
    void beginMoveDrag();

    // Moves the owning window while the title bar is dragged.
    void moveWindowForDrag();

    // Finishes a drag and applies edge snap if the cursor ended on a monitor edge.
    void finishMoveDrag();

    // Minimizes the owning window using Win32 when available or FLTK otherwise.
    void minimizeWindow();

    // Toggles maximized/restored geometry for the owning window.
    void toggleMaximize();

    // Applies monitor work-area maximization and records restore geometry.
    void maximizeWindow();

    // Restores the last normal geometry recorded before maximize or snap.
    void restoreWindow();

    // Applies left/right half-screen snap using the current monitor work area.
    void snapWindow(ChromeState snapState);

    // Restores a maximized/snapped window at drag time and keeps it under the cursor.
    void restoreForInteractiveDrag();

    // Records normal bounds before entering a maximized or snapped chrome state.
    void storeRestoreGeometry();

    // Applies a window rectangle through the chrome wrapper and syncs the title bar.
    void applyWindowBounds(const Rect& bounds);

    // Hides the owning window. This is the close action for custom captions.
    void closeWindow();

    // Draws a compact fallback K mark when the external icon cannot be loaded.
    void drawFallbackIcon(int px, int py, int size);

    // Returns true when the absolute point is inside the drag-safe title area.
    bool inDraggableArea(int px, int py) const;

private:
    KTitleBarStyle style_;
    bool show_maximize_;
    bool dragging_;
    bool drag_started_from_normal_;
    bool restore_valid_;
    ChromeState chrome_state_;
    int drag_offset_x_;
    int drag_offset_y_;
    int drag_start_x_;
    int drag_start_y_;
    int drag_start_w_;
    int drag_start_h_;
    int restore_x_;
    int restore_y_;
    int restore_w_;
    int restore_h_;
    Button hover_button_;
    Button pressed_button_;
    std::string title_;
    std::unique_ptr<Fl_Image> icon_image_;
    std::unique_ptr<Fl_Image> icon_scaled_;
    std::unique_ptr<Fl_Image> logo_image_;
    std::unique_ptr<Fl_Image> logo_scaled_;
};

// Returns the fixed custom title-bar height used when shifting window content.
int KTitleBarHeight();

// Installs a custom title bar into a top-level window. Input is the target
// window, style, and maximize-button policy; output is true when installed or
// updated. Existing children are shifted down exactly once.
bool KInstallTitleBar(Fl_Window* window, KTitleBarStyle style = KTitleBarStyle::Trapezoid, bool showMaximize = true);

// Applies the executable icon resource to a shown Win32 window. Input may be
// null or not-yet-shown; in those cases it is ignored and no value is returned.
void KApplyWindowIcon(Fl_Window* window);

#endif // KSWORD_GUI_KTITLEBAR_H
