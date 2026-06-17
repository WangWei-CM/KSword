#ifndef KSWORD_GUI_KWIDGETS_H
#define KSWORD_GUI_KWIDGETS_H

#include "KImage.h"
#include "KDataView.h"
#include "KLayout.h"
#include "KNavigation.h"
#include "KOverlay.h"
#include "KTheme.h"
#include "KVisual.h"

#include "Fl_Box.H"
#include "Fl_Button.H"
#include "Fl_Check_Button.H"
#include "Fl_Choice.H"
#include "Fl_Counter.H"
#include "Fl_Group.H"
#include "Fl_Image.H"
#include "Fl_Input.H"
#include "Fl_Light_Button.H"
#include "Fl_Menu_Button.H"
#include "Fl_Multiline_Input.H"
#include "Fl_Output.H"
#include "Fl_Progress.H"
#include "Fl_Round_Button.H"
#include "Fl_Scroll.H"
#include "Fl_Scrollbar.H"
#include "Fl_Slider.H"
#include "Fl_Table.H"
#include "Fl_Tabs.H"
#include "Fl_Text_Buffer.H"
#include "Fl_Text_Display.H"
#include "Fl_Tile.H"
#include "Fl_Toggle_Button.H"
#include "Fl_Value_Input.H"

#include <string>
#include <vector>

class Fl_Shared_Image;

// KButtonType describes the three supported square flat button roles.
enum KButtonType {
    KBUTTON_HEAVY,
    KBUTTON_LIGHT,
    KBUTTON_SIMPLE
};

// KImageViewMode selects how an image is positioned and scaled inside KImageView.
enum class KImageViewMode {
    Original,
    FitContain,
    FitCover,
    Stretch
};

// KImageFitMode mirrors common image-framework naming for optional demo adapters.
enum class KImageFitMode {
    Original,
    Contain,
    Cover,
    Stretch
};

// KButton is the canonical flat square button wrapper.
class KButton : public Fl_Button {
public:
    // Creates a themed button from geometry, label, and role; returns a live FLTK widget object.
    KButton(int x, int y, int w, int h, const char* label = nullptr, KButtonType type = KBUTTON_HEAVY);

    // Draws the current visual state using KThemeManager; returns no value.
    void draw() override;

    // Tracks hover/press events then delegates behavior to Fl_Button; returns FLTK event handling status.
    int handle(int event) override;

    // Updates the button role used by draw(); returns no value.
    void setButtonType(KButtonType type);

    // Returns the current button role.
    KButtonType buttonType() const;

private:
    KButtonType type_;
    bool hover_;
    bool pressed_;
};

// KInput applies the shared flat input field style to Fl_Input.
class KInput : public Fl_Input {
public:
    // Creates a themed single-line editable text field.
    KInput(int x, int y, int w, int h, const char* label = nullptr);
};

// KText is the flat label/static text wrapper used by demos and panels.
class KText : public Fl_Box {
public:
    // Creates a themed text label with a flat background.
    KText(int x, int y, int w, int h, const char* label = nullptr);
};

// KPanel is the square container wrapper with themed background and border.
class KPanel : public Fl_Group {
public:
    // Creates a themed container; callers add children with begin()/end().
    KPanel(int x, int y, int w, int h, const char* label = nullptr);

    // Draws the panel frame/background and child widgets; returns no value.
    void draw() override;
};

// KCard is a square flat content container with optional title and subtitle text.
class KCard : public Fl_Group {
public:
    // Creates a themed card; label is copied into the card title and callers may add children.
    KCard(int x, int y, int w, int h, const char* label = nullptr);

    // Replaces the title text drawn in the card header; returns no value.
    void setTitle(const char* title);

    // Returns the current title text owned by the card.
    const std::string& title() const;

    // Replaces the muted subtitle text drawn below the title; returns no value.
    void setSubtitle(const char* subtitle);

    // Returns the current subtitle text owned by the card.
    const std::string& subtitle() const;

    // Draws the card background, border, optional header text, and child widgets; returns no value.
    void draw() override;

private:
    std::string title_;
    std::string subtitle_;
};

// KToolbar is a square flat horizontal container intended for command buttons.
class KToolbar : public Fl_Group {
public:
    // Creates a themed toolbar; callers place child buttons or fields inside it.
    KToolbar(int x, int y, int w, int h, const char* label = nullptr);

    // Draws the toolbar surface, border, optional label, and child widgets; returns no value.
    void draw() override;
};

// KStatusBar is a square flat status strip with left and right text regions.
class KStatusBar : public Fl_Group {
public:
    // Creates a themed status bar; label is copied into the left status text.
    KStatusBar(int x, int y, int w, int h, const char* label = nullptr);

    // Replaces the left status text; returns no value.
    void setText(const char* text);

    // Returns the current left status text owned by the status bar.
    const std::string& text() const;

    // Replaces the right aligned status text; returns no value.
    void setRightText(const char* text);

    // Returns the current right aligned status text owned by the status bar.
    const std::string& rightText() const;

    // Draws the status bar background, top border, status text, and child widgets; returns no value.
    void draw() override;

private:
    std::string text_;
    std::string right_text_;
};

// KBadge is a compact square flat label for counts, states, or short tags.
class KBadge : public Fl_Widget {
public:
    // Creates a themed badge; label is copied into the badge text.
    KBadge(int x, int y, int w, int h, const char* label = nullptr);

    // Replaces the badge text; returns no value.
    void setText(const char* text);

    // Returns the current badge text owned by the widget.
    const std::string& text() const;

    // Stores a numeric count as badge text; negative counts clear the badge and return no value.
    void setCount(int count);

    // Sets the accent fill color used when the badge is active; returns no value.
    void setAccentColor(Fl_Color color);

    // Returns the active accent fill color.
    Fl_Color accentColor() const;

    // Draws the badge as a square flat filled rectangle with centered text; returns no value.
    void draw() override;

private:
    std::string text_;
    Fl_Color accent_color_;
};

// KSeparatorOrientation selects the flat separator line direction.
enum class KSeparatorOrientation {
    Horizontal,
    Vertical
};

// KSeparator is a non-interactive one-pixel divider for panels, cards, and toolbars.
class KSeparator : public Fl_Widget {
public:
    // Creates a themed separator with caller-selected orientation.
    KSeparator(int x, int y, int w, int h, KSeparatorOrientation orientation = KSeparatorOrientation::Horizontal);

    // Updates the separator direction and requests repaint; returns no value.
    void setOrientation(KSeparatorOrientation orientation);

    // Returns the current separator direction.
    KSeparatorOrientation orientation() const;

    // Draws a centered horizontal or vertical one-pixel theme border line; returns no value.
    void draw() override;

private:
    KSeparatorOrientation orientation_;
};

// KCheckBox applies shared typography and theme colors to Fl_Check_Button.
class KCheckBox : public Fl_Check_Button {
public:
    // Creates a themed checkbox.
    KCheckBox(int x, int y, int w, int h, const char* label = nullptr);
};

// KRadioButton applies shared typography and theme colors to Fl_Round_Button.
class KRadioButton : public Fl_Round_Button {
public:
    // Creates a themed radio button.
    KRadioButton(int x, int y, int w, int h, const char* label = nullptr);
};

// KToggleButton draws toggle state with the same square flat language as KButton.
class KToggleButton : public Fl_Toggle_Button {
public:
    // Creates a themed toggle button.
    KToggleButton(int x, int y, int w, int h, const char* label = nullptr);

    // Draws selected/unselected state using the active theme; returns no value.
    void draw() override;
};

// KLightButton is a themed indicator button wrapper.
class KLightButton : public Fl_Light_Button {
public:
    // Creates a themed light/indicator button.
    KLightButton(int x, int y, int w, int h, const char* label = nullptr);
};

// KSlider is a flat themed slider wrapper.
class KSlider : public Fl_Slider {
public:
    // Creates a horizontal themed slider by default.
    KSlider(int x, int y, int w, int h);
};

// KTextBox is a themed multiline editor with convenience text helpers.
class KTextBox : public Fl_Multiline_Input {
public:
    // Creates a themed multiline editable text field.
    KTextBox(int x, int y, int w, int h, const char* label = nullptr);

    // Removes text on the current cursor line; returns no value.
    void clear_current_line();

    // Removes text on the line before the cursor; returns no value.
    void clear_previous_line();

    // Clears all text and resets the cursor; returns no value.
    void clear_all();

    // Appends text at the end and moves the cursor to the new end; returns no value.
    void append_text(const char* text);
};

// KTextDisplay is a read-only themed multiline text viewer.
class KTextDisplay : public Fl_Text_Display {
public:
    // Creates a themed read-only text display backed by an owned buffer.
    KTextDisplay(int x, int y, int w, int h, const char* label = nullptr);

    // Replaces the buffer content with the provided text; returns no value.
    void set_text(const char* text);

private:
    Fl_Text_Buffer buffer_;
};

// KImageView is a themed image display widget with safe empty and failed-load states.
class KImageView : public Fl_Widget {
public:
    // Creates an image view from geometry and optional label; no image is loaded by default.
    KImageView(int x, int y, int w, int h, const char* label = nullptr);

    // Releases any internally loaded shared image; caller-provided images are not deleted.
    ~KImageView() override;

    // Loads an image from a filesystem path using FLTK shared-image handlers; returns true on success.
    bool setImagePath(const char* path);

    // Copies a cached image resource into this view and returns true when it can be drawn.
    bool setResource(const KImageResource& resource);

    // Copies a cached image resource using snake_case compatibility; returns true when drawable.
    bool set_resource(const KImageResource& resource);

    // Uses a caller-provided image pointer without taking ownership; returns no value.
    void setImage(Fl_Image* image);

    // Returns the current image pointer, or nullptr when empty or failed.
    Fl_Image* currentImage() const;

    // Returns the current image pointer using concise compatibility naming.
    Fl_Image* image() const;

    // Returns the resource currently held by this view, or an empty resource for caller-owned images.
    const KImageResource& resource() const;

    // Sets the scaling/positioning mode used during draw(); returns no value.
    void setDisplayMode(KImageViewMode mode);

    // Sets the scaling/positioning mode using optional image-framework naming; returns no value.
    void set_fit_mode(KImageFitMode mode);

    // Sets the scaling/positioning mode using camelCase optional image-framework naming; returns no value.
    void setFitMode(KImageFitMode mode);

    // Returns the current image scaling/positioning mode.
    KImageViewMode displayMode() const;

    // Sets the background fill color used before image drawing; returns no value.
    void setBackgroundColor(Fl_Color color);

    // Returns the current background fill color.
    Fl_Color backgroundColor() const;

    // Sets the one-pixel border color; returns no value.
    void setBorderColor(Fl_Color color);

    // Returns the current border color.
    Fl_Color borderColor() const;

    // Enables or disables border drawing; returns no value.
    void setBorderVisible(bool visible);

    // Returns true when the border is drawn.
    bool borderVisible() const;

    // Applies theme paint slots without requesting redraw; used by KThemeManager so one top-level refresh controls repaint.
    void applyThemePalette(Fl_Color background, Fl_Color border, Fl_Color emptyText, bool borderVisible);

    // Sets text drawn when no image is assigned and loading did not fail; returns no value.
    void setEmptyText(const char* text);

    // Sets text drawn when no image is assigned using snake_case compatibility; returns no value.
    void set_empty_text(const std::string& text);

    // Sets text drawn when no image is assigned using camelCase compatibility; returns no value.
    void setEmptyText(const std::string& text);

    // Returns the current empty-state text.
    const std::string& emptyText() const;

    // Sets text drawn when setImagePath() cannot load an image; returns no value.
    void setLoadFailedText(const char* text);

    // Returns the current failed-load text.
    const std::string& loadFailedText() const;

    // Draws background, optional border, status text, and image according to display mode.
    void draw() override;

    // Requests redraw after size changes so image placement stays correct; returns no value.
    void resize(int x, int y, int w, int h) override;

private:
    // Releases a shared image loaded by setImagePath(); caller-owned images are left untouched.
    void releaseSharedImage();

    // Draws centered status text for empty or failed states; returns no value.
    void drawStateText(const char* text, Fl_Color color);

    // Draws the current image using Original/FitContain/FitCover/Stretch strategy.
    void drawImageInContent(int content_x, int content_y, int content_w, int content_h);

private:
    Fl_Image* image_;
    Fl_Shared_Image* shared_image_;
    KImageViewMode display_mode_;
    Fl_Color background_color_;
    Fl_Color border_color_;
    bool border_visible_;
    bool load_failed_;
    std::string image_path_;
    KImageResource resource_;
    std::string load_error_text_;
    std::string empty_text_;
    std::string load_failed_text_;
};

// KTabs customizes FLTK tabs into the square flat design language.
class KTabs : public Fl_Tabs {
public:
    // Creates a themed tabs container.
    KTabs(int x, int y, int w, int h, const char* label = nullptr);

protected:
    // Draws an individual tab using active theme colors; returns no value.
    void draw_tab(int x1, int x2, int W, int H, Fl_Widget* child, int flags, int selected) override;

    // Draws tab pages and separator corrections; returns no value.
    void draw() override;
};

// KTable is a lightweight themed data table with context-menu callbacks.
class KTable : public Fl_Table {
public:
    typedef void (*ContextMenuCallback)(KTable* table, int row, int col, void* user_data);

private:
    // ContextMenuBinding stores an optional callback and caller payload.
    struct ContextMenuBinding {
        ContextMenuCallback callback;
        void* user_data;
        ContextMenuBinding();
    };

public:
    // Creates a themed table with column headers enabled.
    KTable(int x, int y, int w, int h, const char* label = nullptr);

    // Ensures backing vectors are synchronized before drawing; returns no value.
    void draw() override;

    // Draws headers/cells with current theme colors; returns no value.
    void draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) override;

    // Handles right-click callbacks before default table processing; returns FLTK event status.
    int handle(int event) override;

    // Resizes logical dimensions and backing data; returns no value.
    void set_size(int rows, int cols);

    // Sets a column header label; returns no value.
    void set_col_header_label(int col, const char* label);

    // Returns a stable header string or an empty string.
    const char* col_header_label(int col) const;

    // Sets cell text, growing the model as needed; returns no value.
    void set_cell(int row, int col, const char* text);

    // Returns a stable cell string or an empty string.
    const char* cell(int row, int col) const;

    // Sets all row heights when positive; returns no value.
    void set_row_height(int h);

    // Sets all column widths when positive; returns no value.
    void set_col_width(int w);

    // Registers a row-level context callback; returns no value.
    void set_row_context_menu_callback(int row, ContextMenuCallback callback, void* user_data = nullptr);

    // Registers a cell-level context callback; returns no value.
    void set_cell_context_menu_callback(int row, int col, ContextMenuCallback callback, void* user_data = nullptr);

    // Writes the last context-menu root-screen position into x/y; returns no value.
    void context_menu_position(int& x, int& y) const;

private:
    // Expands or shrinks backing vectors to match dimensions; returns no value.
    void ensure_size(int rows, int cols);

    // Invokes matching callbacks and returns true when one ran.
    bool trigger_context_menu_callback(int row, int col);

private:
    std::vector<std::string> col_labels_;
    std::vector<std::vector<std::string>> cells_;
    std::vector<ContextMenuBinding> row_context_menu_callbacks_;
    std::vector<std::vector<ContextMenuBinding>> cell_context_menu_callbacks_;
    int row_height_;
    int col_width_;
    int context_menu_x_;
    int context_menu_y_;
};

// KChoice is a themed dropdown selector.
class KChoice : public Fl_Choice {
public:
    // Creates a themed choice widget.
    KChoice(int x, int y, int w, int h, const char* label = nullptr);
};

// KOutput is a themed read-only single-line output field.
class KOutput : public Fl_Output {
public:
    // Creates a themed output field.
    KOutput(int x, int y, int w, int h, const char* label = nullptr);
};

// KProgressBar is a themed progress bar with 0..100 default range.
class KProgressBar : public Fl_Progress {
public:
    // Creates a themed progress bar.
    KProgressBar(int x, int y, int w, int h, const char* label = nullptr);
};

// KScrollArea is a themed scrollable container.
class KScrollArea : public Fl_Scroll {
public:
    // Creates a themed scroll area.
    KScrollArea(int x, int y, int w, int h, const char* label = nullptr);
};

// KScrollBar is a themed scrollbar wrapper.
class KScrollBar : public Fl_Scrollbar {
public:
    // Creates a themed scrollbar with caller-selected orientation.
    KScrollBar(int x, int y, int w, int h, int type = FL_VERTICAL);
};

// KBrowser is a lightweight themed list/browser widget without FLTK browser dependencies.
class KBrowser : public Fl_Group {
public:
    // Creates a themed list control with internal string storage.
    KBrowser(int x, int y, int w, int h, const char* label = nullptr);

    // Adds one visible list item; input is copied and no value is returned.
    void add(const char* item);

    // Clears all list items and resets selection; returns no value.
    void clear();

    // Returns the selected zero-based index, or -1 when nothing is selected.
    int value() const;

    // Sets the selected zero-based index if valid; returns no value.
    void value(int index);

    // Returns the item text at index or an empty string when out of range.
    const char* text(int index) const;

    // Draws list rows using the active theme; returns no value.
    void draw() override;

    // Handles click selection before delegating other events; returns FLTK event status.
    int handle(int event) override;

private:
    std::vector<std::string> items_;
    int selected_index_;
};

// KMenuButton is a themed menu button wrapper.
class KMenuButton : public Fl_Menu_Button {
public:
    // Creates a themed popup menu button.
    KMenuButton(int x, int y, int w, int h, const char* label = nullptr);
};

// KCounter is a themed numeric counter wrapper.
class KCounter : public Fl_Counter {
public:
    // Creates a themed counter control.
    KCounter(int x, int y, int w, int h, const char* label = nullptr);
};

// KSpinner is a themed numeric spinner-style wrapper backed by Fl_Value_Input.
class KSpinner : public Fl_Value_Input {
public:
    // Creates a themed numeric spinner-style control without depending on Fl_Spinner.H.
    KSpinner(int x, int y, int w, int h, const char* label = nullptr);
};

// KValueInput is a themed numeric input wrapper.
class KValueInput : public Fl_Value_Input {
public:
    // Creates a themed numeric value input.
    KValueInput(int x, int y, int w, int h, const char* label = nullptr);
};

// KSplitter exposes FLTK tile behavior under the K component naming scheme.
class KSplitter : public Fl_Tile {
public:
    // Creates a themed splitter/tile container.
    KSplitter(int x, int y, int w, int h, const char* label = nullptr);
};

// Compatibility aliases keep old code compiling while K* names become canonical.
using FlatButton = KButton;
using FlatSlider = KSlider;
using FlatTextBox = KTextBox;
using FlatTextDisplay = KTextDisplay;
using FlatImageView = KImageView;
using FlatTabs = KTabs;
using FlatTable = KTable;

// Factory helpers route every widget through the canonical K* classes.
KButton* KCreateButton(int x, int y, int w, int h, const char* label = nullptr, KButtonType type = KBUTTON_HEAVY);
KInput* KCreateInput(int x, int y, int w, int h, const char* label = nullptr);
KCheckBox* KCreateCheckBox(int x, int y, int w, int h, const char* label = nullptr);
KSlider* KCreateSlider(int x, int y, int w, int h);
KText* KCreateText(int x, int y, int w, int h, const char* label = nullptr);
KTextBox* KCreateTextBox(int x, int y, int w, int h, const char* label = nullptr);
KTable* KCreateTable(int x, int y, int w, int h, const char* label = nullptr);
KPanel* KCreatePanel(int x, int y, int w, int h, const char* label = nullptr);
KCard* KCreateCard(int x, int y, int w, int h, const char* label = nullptr);
KToolbar* KCreateToolbar(int x, int y, int w, int h, const char* label = nullptr);
KStatusBar* KCreateStatusBar(int x, int y, int w, int h, const char* label = nullptr);
KBadge* KCreateBadge(int x, int y, int w, int h, const char* label = nullptr);
KSeparator* KCreateSeparator(int x, int y, int w, int h, KSeparatorOrientation orientation = KSeparatorOrientation::Horizontal);
KChoice* KCreateChoice(int x, int y, int w, int h, const char* label = nullptr);
void KChoiceSetItems(Fl_Choice* choice, const std::vector<std::string>& items, int selected = 0);
KOutput* KCreateOutput(int x, int y, int w, int h, const char* label = nullptr);
KProgressBar* KCreateProgress(int x, int y, int w, int h, const char* label = nullptr);
KRadioButton* KCreateRadioButton(int x, int y, int w, int h, const char* label = nullptr);
KToggleButton* KCreateToggleButton(int x, int y, int w, int h, const char* label = nullptr);
KLightButton* KCreateLightButton(int x, int y, int w, int h, const char* label = nullptr);
KScrollArea* KCreateScroll(int x, int y, int w, int h, const char* label = nullptr);
KScrollBar* KCreateScrollBar(int x, int y, int w, int h, int type = FL_VERTICAL);
KTabs* KCreateTabs(int x, int y, int w, int h, const char* label = nullptr);
KPanel* KCreateTabPage(int x, int y, int w, int h, const char* label = nullptr);
KScrollArea* KCreateScrollArea(int x, int y, int w, int h, const char* label = nullptr);
KTextDisplay* KCreateTextDisplay(int x, int y, int w, int h, const char* label = nullptr);
KImageView* KCreateImageView(int x, int y, int w, int h, const char* label = nullptr);
KImageView* KCreateImageViewFromFile(int x, int y, int w, int h, const char* path, const char* label = nullptr, KImageFitMode mode = KImageFitMode::Contain);
KBrowser* KCreateBrowser(int x, int y, int w, int h, const char* label = nullptr);
KMenuButton* KCreateMenuButton(int x, int y, int w, int h, const char* label = nullptr);
KCounter* KCreateCounter(int x, int y, int w, int h, const char* label = nullptr);
KSpinner* KCreateSpinner(int x, int y, int w, int h, const char* label = nullptr);
KValueInput* KCreateValueInput(int x, int y, int w, int h, const char* label = nullptr);
KSplitter* KCreateSplitter(int x, int y, int w, int h, const char* label = nullptr);

// Popup helpers provide consistent themed modal UI.
void KShowMessage(const char* title, const char* message);
int KShowPopupMenu(int x, int y, const std::vector<std::string>& items);

#endif // KSWORD_GUI_KWIDGETS_H
