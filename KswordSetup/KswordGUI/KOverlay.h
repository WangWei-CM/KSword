#ifndef KSWORD_GUI_KOVERLAY_H
#define KSWORD_GUI_KOVERLAY_H

#include "KTheme.h"

#include "Fl_Widget.H"
#include "Fl_Window.H"

#include <string>
#include <vector>

// KToastKind selects the semantic color used by KToast.
enum class KToastKind { Info, Success, Warning, Danger };

// KToast is a small borderless popup notification with themed semantic state.
class KToast : public Fl_Window {
public:
    // Creates a toast window; label initializes the visible message.
    KToast(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces toast message text and returns no value.
    void setText(const char* text);
    // Sets semantic state used by drawing and returns no value.
    void setKind(KToastKind kind);
    // Sets intended visible duration in seconds for caller-managed timers; returns no value.
    void setDuration(double seconds);
    // Sets explicit accent color for Info state and returns no value.
    void setAccentColor(Fl_Color color);
    // Moves and shows the toast with optional replacement text; returns no value.
    void showAt(int root_x, int root_y, const char* text = nullptr);
    // Paints toast surface, accent bar, and message; returns no value.
    void draw() override;
private:
    std::string text_;
    KToastKind kind_;
    double duration_;
    Fl_Color accent_color_;
};

// KTooltip is a borderless text hint window for hover help.
class KTooltip : public Fl_Window {
public:
    // Creates a tooltip with optional text.
    KTooltip(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces tooltip text and returns no value.
    void setText(const char* text);
    // Moves and shows tooltip near a root coordinate; returns no value.
    void showAt(int root_x, int root_y);
    // Paints tooltip body and text; returns no value.
    void draw() override;
private:
    std::string text_;
};

// KPopover is a borderless content bubble with title and body text.
class KPopover : public Fl_Window {
public:
    // Creates a popover shell with optional title text.
    KPopover(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setTitle(const char* text);
    // Replaces body text and returns no value.
    void setContent(const char* text);
    // Shows popover at root coordinates and returns no value.
    void showAt(int root_x, int root_y);
    // Paints title, body, and border; returns no value.
    void draw() override;
private:
    std::string title_;
    std::string content_;
};

// KModalDialog is a basic themed modal shell with primary and secondary actions.
class KModalDialog : public Fl_Window {
public:
    // Creates a modal dialog with title initialized from label.
    KModalDialog(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and window label; returns no value.
    void setTitle(const char* text);
    // Replaces message body text and returns no value.
    void setMessage(const char* text);
    // Replaces primary button text and returns no value.
    void setPrimaryText(const char* text);
    // Replaces secondary button text and returns no value.
    void setSecondaryText(const char* text);
    // Runs the dialog modally and returns 1 for primary, 0 for secondary, or -1 when closed.
    int runModal();
    // Returns the latest dialog result value.
    int result() const;
    // Paints modal shell, message, and buttons; returns no value.
    void draw() override;
    // Handles button clicks and escape close; returns FLTK event handling status.
    int handle(int event) override;
private:
    std::string title_;
    std::string message_;
    std::string primary_text_;
    std::string secondary_text_;
    int result_;
};

// KDrawerSide selects which screen edge a drawer is conceptually attached to.
enum class KDrawerSide { Left, Right, Top, Bottom };

// KDrawer is a flat borderless side panel shell for transient content.
class KDrawer : public Fl_Window {
public:
    // Creates a drawer shell with optional title text.
    KDrawer(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setTitle(const char* text);
    // Replaces content summary text and returns no value.
    void setContent(const char* text);
    // Sets drawer side metadata used by callers for positioning and returns no value.
    void setSide(KDrawerSide side);
    // Shows the drawer at its current geometry and returns no value.
    void showDrawer();
    // Hides the drawer and returns no value.
    void hideDrawer();
    // Paints drawer panel, title, and content; returns no value.
    void draw() override;
private:
    std::string title_;
    std::string content_;
    KDrawerSide side_;
};

// KOverlayMask draws a theme-aware blocking mask over a parent allocation.
class KOverlayMask : public Fl_Widget {
public:
    // Creates a hidden overlay mask by default.
    KOverlayMask(int x, int y, int w, int h, const char* label = nullptr);
    // Sets mask visibility and returns no value.
    void setVisible(bool visible);
    // Returns true when the mask will draw.
    bool visibleMask() const;
    // Sets opacity hint in 0..1 range and returns no value.
    void setOpacity(double opacity);
    // Paints the mask when visible; returns no value.
    void draw() override;
private:
    bool visible_;
    double opacity_;
};


// KModalMask draws a blocking modal scrim with optional centered status message.
class KModalMask : public Fl_Widget {
public:
    // Creates a hidden modal mask by default; label initializes centered message text.
    KModalMask(int x, int y, int w, int h, const char* label = nullptr);
    // Sets mask visibility and returns no value.
    void setVisible(bool visible);
    // Returns true when the mask is visible and consumes input.
    bool visibleMask() const;
    // Sets opacity hint in 0..1 range and returns no value.
    void setOpacity(double opacity);
    // Replaces centered message text and returns no value.
    void setMessage(const char* text);
    // Sets message/accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints blocking scrim and optional modal message card; returns no value.
    void draw() override;
    // Consumes mouse and keyboard events while visible; returns FLTK status.
    int handle(int event) override;
private:
    bool visible_;
    double opacity_;
    std::string message_;
    Fl_Color accent_color_;
};

// KLoadingOverlay draws a mask plus spinner/progress message while loading is true.
class KLoadingOverlay : public Fl_Widget {
public:
    // Creates a loading overlay with loading disabled by default.
    KLoadingOverlay(int x, int y, int w, int h, const char* label = nullptr);
    // Enables or disables loading visualization and returns no value.
    void setLoading(bool loading);
    // Returns true when loading visualization is active.
    bool loading() const;
    // Replaces message text and returns no value.
    void setMessage(const char* text);
    // Sets optional progress in 0..1; negative hides progress bar and returns no value.
    void setProgress(double progress);
    // Sets spinner phase for caller-managed animation and returns no value.
    void setStep(int step);
    // Paints mask, spinner, text, and optional progress; returns no value.
    void draw() override;
private:
    bool loading_;
    std::string message_;
    double progress_;
    int step_;
};

// KContextMenuItem stores one context menu row, separator, shortcut, and state.
struct KContextMenuItem {
    std::string text;
    std::string shortcut;
    bool enabled = true;
    bool separator = false;
    bool checked = false;
};

// KContextMenu is an enhanced themed popup menu with checked/disabled/separator rows.
class KContextMenu : public Fl_Window {
public:
    // Creates an empty context menu popup.
    KContextMenu(int x, int y, int w, int h, const char* label = nullptr);
    // Copies menu rows and returns no value.
    void setItems(const std::vector<KContextMenuItem>& items);
    // Appends one row and returns no value.
    void addItem(const KContextMenuItem& item);
    // Clears menu rows and selection state; returns no value.
    void clear();
    // Sets active row index and returns no value.
    void setActiveIndex(int index);
    // Returns the last selected row index, or -1 when none was selected.
    int selectedIndex() const;
    // Shows the menu modally at root coordinates and returns selected row index or -1.
    int popup(int root_x, int root_y);
    // Paints rows, separators, shortcuts, and checked state; returns no value.
    void draw() override;
    // Handles mouse/keyboard selection and dismissal; returns FLTK event handling status.
    int handle(int event) override;
private:
    std::vector<KContextMenuItem> items_;
    int active_index_;
    int selected_index_;
    int row_height_;
};

KToast* KCreateToast(int x, int y, int w, int h, const char* label = nullptr);
KTooltip* KCreateTooltip(int x, int y, int w, int h, const char* label = nullptr);
KPopover* KCreatePopover(int x, int y, int w, int h, const char* label = nullptr);
KModalDialog* KCreateModalDialog(int x, int y, int w, int h, const char* label = nullptr);
KDrawer* KCreateDrawer(int x, int y, int w, int h, const char* label = nullptr);
KOverlayMask* KCreateOverlayMask(int x, int y, int w, int h, const char* label = nullptr);
KModalMask* KCreateModalMask(int x, int y, int w, int h, const char* label = nullptr);
KLoadingOverlay* KCreateLoadingOverlay(int x, int y, int w, int h, const char* label = nullptr);
KContextMenu* KCreateContextMenu(int x, int y, int w, int h, const char* label = nullptr);

#endif // KSWORD_GUI_KOVERLAY_H
