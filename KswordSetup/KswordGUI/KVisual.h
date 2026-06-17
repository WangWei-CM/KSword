#ifndef KSWORD_GUI_KVISUAL_H
#define KSWORD_GUI_KVISUAL_H

#include "KTheme.h"
#include "KIcon.h"

#include "Fl_Button.H"
#include "Fl_Widget.H"

#include <string>
#include <vector>

// KIconButton is a compact square flat button with text/icon glyph support.
class KIconButton : public Fl_Button {
public:
    // Creates a button; label initializes text and icon is empty by default.
    KIconButton(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces fallback icon glyph/name text and returns no value.
    void setIcon(const char* icon);
    // Replaces SVG icon name below KswordGUI/Icon, keeps glyph fallback, and returns no value.
    void setIconName(const char* iconName);
    // Returns the SVG icon name owned by this button; empty means glyph fallback only.
    const std::string& iconName() const;
    // Sets semantic theme role used to tint SVG icons and returns no value.
    void setIconColorRole(KIconColorRole role);
    // Sets explicit SVG tint color and returns no value.
    void setIconTint(Fl_Color color);
    // Clears explicit SVG tint so the semantic theme role is used; returns no value.
    void clearIconTint();
    // Sets requested SVG icon size in pixels and returns no value.
    void setIconSize(int size);
    // Replaces caption text and returns no value.
    void setText(const char* text);
    // Sets checked visual state and returns no value.
    void setChecked(bool checked);
    // Returns true when checked visual state is active.
    bool checked() const;
    // Sets accent color used by checked state and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints icon, text, hover, pressed, and checked states; returns no value.
    void draw() override;
    // Tracks hover/press and delegates activation to Fl_Button; returns FLTK event status.
    int handle(int event) override;
private:
    std::string icon_;
    std::string icon_name_;
    std::string text_;
    KIconColorRole icon_role_;
    Fl_Color icon_tint_;
    bool use_icon_tint_;
    int icon_size_;
    bool hover_;
    bool pressed_;
    bool checked_;
    Fl_Color accent_color_;
};

// KAvatar draws initials or image-placeholder style user identity.
class KAvatar : public Fl_Widget {
public:
    // Creates an avatar; label initializes display name.
    KAvatar(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces display name and recomputes initials during draw; returns no value.
    void setName(const char* name);
    // Replaces explicit initials and returns no value.
    void setInitials(const char* initials);
    // Sets accent fill color and returns no value.
    void setAccentColor(Fl_Color color);
    // Sets online/offline status dot visibility and returns no value.
    void setOnline(bool online);
    // Paints square avatar, initials, and optional status dot; returns no value.
    void draw() override;
private:
    std::string name_;
    std::string initials_;
    Fl_Color accent_color_;
    bool online_;
};

// KTag is a small non-interactive label with semantic accent color.
class KTag : public Fl_Widget {
public:
    // Creates a tag with optional label text.
    KTag(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces text and returns no value.
    void setText(const char* text);
    // Returns tag text owned by this widget.
    const std::string& text() const;
    // Sets accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints filled tag and centered text; returns no value.
    void draw() override;
private:
    std::string text_;
    Fl_Color accent_color_;
};

// KChip is a selectable compact token with optional close affordance.
class KChip : public Fl_Widget {
public:
    // Creates a chip with optional text.
    KChip(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces chip text and returns no value.
    void setText(const char* text);
    // Sets selected state and returns no value.
    void setSelected(bool selected);
    // Returns selected state.
    bool selected() const;
    // Enables/disables close marker drawing and returns no value.
    void setClosable(bool closable);
    // Sets accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints chip state, text, and close marker; returns no value.
    void draw() override;
    // Toggles selected state or invokes callback on close area; returns FLTK event status.
    int handle(int event) override;
private:
    std::string text_;
    bool selected_;
    bool closable_;
    Fl_Color accent_color_;
};

// KAlertKind selects KAlert semantic state.
enum class KAlertKind { Info, Success, Warning, Danger };

// KAlert draws a full-width informational message block.
class KAlert : public Fl_Widget {
public:
    // Creates an alert; label initializes title text.
    KAlert(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setText(const char* text);
    // Replaces detail text and returns no value.
    void setDescription(const char* text);
    // Sets semantic kind and returns no value.
    void setKind(KAlertKind kind);
    // Paints alert shell, semantic stripe, title, and description; returns no value.
    void draw() override;
private:
    std::string text_;
    std::string description_;
    KAlertKind kind_;
};

// KEmptyState renders an illustration placeholder plus title, description, and action label.
class KEmptyState : public Fl_Widget {
public:
    // Creates an empty-state panel; label initializes title.
    KEmptyState(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setText(const char* text);
    // Replaces description text and returns no value.
    void setDescription(const char* text);
    // Replaces action text and returns no value.
    void setActionText(const char* text);
    // Paints empty illustration, text, and action affordance; returns no value.
    void draw() override;
    // Invokes callback when action area is clicked; returns FLTK event status.
    int handle(int event) override;
private:
    std::string text_;
    std::string description_;
    std::string action_text_;
};

// KSkeleton draws placeholder bars/cards for loading content.
class KSkeleton : public Fl_Widget {
public:
    // Creates skeleton placeholder with three rows by default.
    KSkeleton(int x, int y, int w, int h, const char* label = nullptr);
    // Sets row count and returns no value.
    void setLineCount(int count);
    // Sets animation phase hint in pixels and returns no value.
    void setPhase(int phase);
    // Enables avatar placeholder block and returns no value.
    void setAvatarVisible(bool visible);
    // Paints themed skeleton rows and optional avatar; returns no value.
    void draw() override;
private:
    int line_count_;
    int phase_;
    bool avatar_visible_;
};

// KTimelineItem stores one timeline event.
struct KTimelineItem {
    std::string title;
    std::string detail;
    bool active = false;
};

// KTimeline draws a vertical activity timeline.
class KTimeline : public Fl_Widget {
public:
    // Creates an empty timeline.
    KTimeline(int x, int y, int w, int h, const char* label = nullptr);
    // Copies timeline items and returns no value.
    void setItems(const std::vector<KTimelineItem>& items);
    // Appends one item and returns no value.
    void addItem(const KTimelineItem& item);
    // Sets accent color for active dots and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints vertical line, dots, titles, and details; returns no value.
    void draw() override;
private:
    std::vector<KTimelineItem> items_;
    Fl_Color accent_color_;
};

// KMetricCard draws a compact KPI summary with value and trend text.
class KMetricCard : public Fl_Widget {
public:
    // Creates a metric card; label initializes title text.
    KMetricCard(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setTitle(const char* text);
    // Replaces value text and returns no value.
    void setValue(const char* text);
    // Replaces trend text and positive/negative state; returns no value.
    void setTrend(const char* text, bool positive = true);
    // Sets accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints KPI card, title, value, trend, and accent marker; returns no value.
    void draw() override;
private:
    std::string title_;
    std::string value_;
    std::string trend_;
    bool trend_positive_;
    Fl_Color accent_color_;
};


// KStatCard draws a richer KPI card with optional SVG icon, caption, and accent bar.
class KStatCard : public Fl_Widget {
public:
    // Creates a stat card; label initializes title text and value defaults to "0".
    KStatCard(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setTitle(const char* text);
    // Replaces primary value text and returns no value.
    void setValue(const char* text);
    // Replaces secondary caption text and returns no value.
    void setCaption(const char* text);
    // Replaces SVG icon name below KswordGUI/Icon and returns no value.
    void setIconName(const char* iconName);
    // Sets semantic theme role used for icon and accent tint; returns no value.
    void setIconColorRole(KIconColorRole role);
    // Sets explicit accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints card shell, icon fallback, value, title, caption, and accent; returns no value.
    void draw() override;
private:
    std::string title_;
    std::string value_;
    std::string caption_;
    std::string icon_name_;
    KIconColorRole icon_role_;
    Fl_Color accent_color_;
};

// KSearchBox is a lightweight self-painted search input with placeholder and clear affordance.
class KSearchBox : public Fl_Widget {
public:
    // Creates a search box; label initializes placeholder text.
    KSearchBox(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces query text and returns no value.
    void setText(const char* text);
    // Returns query text owned by the search box.
    const std::string& text() const;
    // Replaces placeholder text and returns no value.
    void setPlaceholder(const char* text);
    // Clears query text, invokes callback only through user interaction, and returns no value.
    void clear();
    // Sets focus and active-border accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints input frame, search glyph, query/placeholder, and clear affordance; returns no value.
    void draw() override;
    // Handles focus, text input, backspace, escape, enter, and clear click; returns FLTK status.
    int handle(int event) override;
private:
    std::string text_;
    std::string placeholder_;
    bool focused_;
    bool hover_;
    Fl_Color accent_color_;
};

// KSegmentedControl draws a horizontal exclusive-choice selector.
class KSegmentedControl : public Fl_Widget {
public:
    // Creates an empty segmented control.
    KSegmentedControl(int x, int y, int w, int h, const char* label = nullptr);
    // Copies segment labels, clamps active state, and returns no value.
    void setItems(const std::vector<std::string>& items);
    // Appends one segment label and returns no value.
    void addItem(const char* text);
    // Clears all segments and interaction state; returns no value.
    void clear();
    // Sets active segment index and returns no value.
    void setActiveIndex(int index);
    // Returns active segment index, or -1 when no segment is active.
    int activeIndex() const;
    // Returns active segment text, or an empty string when nothing is active.
    std::string activeText() const;
    // Sets active segment accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints segmented shell, hover state, active state, and labels; returns no value.
    void draw() override;
    // Handles hover and click selection; returns FLTK event status.
    int handle(int event) override;
private:
    // Computes segment hit test from root-window coordinates; returns -1 on miss.
    int segmentAt(int mouse_x, int mouse_y) const;
    std::vector<std::string> items_;
    int active_index_;
    int hover_index_;
    Fl_Color accent_color_;
};

// KSwitch draws an accessible click/keyboard boolean toggle.
class KSwitch : public Fl_Widget {
public:
    // Creates a switch; label is optional metadata while on/off text defaults are compact.
    KSwitch(int x, int y, int w, int h, const char* label = nullptr);
    // Sets checked state and returns no value.
    void setChecked(bool checked);
    // Returns true when switch is checked.
    bool checked() const;
    // Replaces optional on/off captions and returns no value.
    void setTexts(const char* onText, const char* offText);
    // Sets checked accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints track, thumb, state text, and focus/hover states; returns no value.
    void draw() override;
    // Handles mouse and keyboard toggling; returns FLTK event status.
    int handle(int event) override;
private:
    bool checked_;
    bool hover_;
    std::string on_text_;
    std::string off_text_;
    Fl_Color accent_color_;
};

// KColorSwatch displays one selectable color sample with optional label.
class KColorSwatch : public Fl_Widget {
public:
    // Creates a color swatch; label initializes caption text.
    KColorSwatch(int x, int y, int w, int h, const char* label = nullptr);
    // Sets swatch color and returns no value.
    void setSwatchColor(Fl_Color color);
    // Returns the stored swatch color.
    Fl_Color swatchColor() const;
    // Replaces caption text and returns no value.
    void setText(const char* text);
    // Sets selected state and returns no value.
    void setSelected(bool selected);
    // Returns true when selected visual state is active.
    bool selected() const;
    // Paints color sample, label, and selected marker; returns no value.
    void draw() override;
    // Toggles selected state on click and invokes callback; returns FLTK event status.
    int handle(int event) override;
private:
    std::string text_;
    Fl_Color color_;
    bool selected_;
};

KIconButton* KCreateIconButton(int x, int y, int w, int h, const char* label = nullptr);
KAvatar* KCreateAvatar(int x, int y, int w, int h, const char* label = nullptr);
KTag* KCreateTag(int x, int y, int w, int h, const char* label = nullptr);
KChip* KCreateChip(int x, int y, int w, int h, const char* label = nullptr);
KAlert* KCreateAlert(int x, int y, int w, int h, const char* label = nullptr);
KEmptyState* KCreateEmptyState(int x, int y, int w, int h, const char* label = nullptr);
KSkeleton* KCreateSkeleton(int x, int y, int w, int h, const char* label = nullptr);
KTimeline* KCreateTimeline(int x, int y, int w, int h, const char* label = nullptr);
KMetricCard* KCreateMetricCard(int x, int y, int w, int h, const char* label = nullptr);
KStatCard* KCreateStatCard(int x, int y, int w, int h, const char* label = nullptr);
KSearchBox* KCreateSearchBox(int x, int y, int w, int h, const char* label = nullptr);
KSegmentedControl* KCreateSegmentedControl(int x, int y, int w, int h, const char* label = nullptr);
KSwitch* KCreateSwitch(int x, int y, int w, int h, const char* label = nullptr);
KColorSwatch* KCreateColorSwatch(int x, int y, int w, int h, const char* label = nullptr);

#endif // KSWORD_GUI_KVISUAL_H
