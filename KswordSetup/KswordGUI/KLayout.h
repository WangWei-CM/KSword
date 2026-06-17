#ifndef KSWORD_GUI_KLAYOUT_H
#define KSWORD_GUI_KLAYOUT_H

#include "KTheme.h"

#include "Fl_Group.H"
#include "Fl_Scroll.H"

#include <string>
#include <vector>

// kKLayoutMaximumSize mirrors Qt's practical "unbounded" layout size value.
// It keeps maximumSizeHint() finite while still being large enough for windows.
constexpr int kKLayoutMaximumSize = 16777215;

// KSize is the small value type returned by layout size hint APIs.
struct KSize {
    // Creates a width/height pair. Negative inputs are normalized by callers.
    KSize(int width_value = 0, int height_value = 0);

    int width;
    int height;
};

// KMargins stores content insets in the same left/top/right/bottom order as Qt.
struct KMargins {
    // Creates a margin set used to reduce a layout's available content rect.
    KMargins(int left_value = 0, int top_value = 0, int right_value = 0, int bottom_value = 0);

    int left;
    int top;
    int right;
    int bottom;
};

// KSizePolicyType describes how one axis consumes the size offered by a layout.
enum class KSizePolicyType {
    Fixed,
    Minimum,
    Expanding,
    Fill
};

// KSizePolicy is the QSizePolicy-style policy object used by K layouts.
class KSizePolicy {
public:
    // Creates a per-axis policy. Stretch values are clamped to non-negative integers.
    KSizePolicy(KSizePolicyType horizontal = KSizePolicyType::Fill,
        KSizePolicyType vertical = KSizePolicyType::Fill,
        int horizontal_stretch = 0,
        int vertical_stretch = 0);

    // Factory helpers create the four common symmetric policies.
    static KSizePolicy Fixed();
    static KSizePolicy Minimum();
    static KSizePolicy Expanding(int horizontal_stretch = 1, int vertical_stretch = 1);
    static KSizePolicy Fill(int horizontal_stretch = 1, int vertical_stretch = 1);

    // Returns the horizontal policy without side effects.
    KSizePolicyType horizontalPolicy() const;
    // Returns the vertical policy without side effects.
    KSizePolicyType verticalPolicy() const;
    // Updates the horizontal policy and returns no value.
    void setHorizontalPolicy(KSizePolicyType policy);
    // Updates the vertical policy and returns no value.
    void setVerticalPolicy(KSizePolicyType policy);
    // Returns the horizontal stretch factor used by proportional layouts.
    int horizontalStretch() const;
    // Returns the vertical stretch factor used by proportional layouts.
    int verticalStretch() const;
    // Updates the horizontal stretch factor and returns no value.
    void setHorizontalStretch(int stretch);
    // Updates the vertical stretch factor and returns no value.
    void setVerticalStretch(int stretch);

private:
    KSizePolicyType horizontal_policy_;
    KSizePolicyType vertical_policy_;
    int horizontal_stretch_;
    int vertical_stretch_;
};

// KLayoutItem keeps per-child layout metadata without requiring every FLTK widget
// subclass to learn about size policies. The owning layout synchronizes this list
// with its actual FLTK child list before each layout pass.
struct KLayoutItem {
    // Creates an empty metadata record; callers normally use the widget overload.
    KLayoutItem();
    // Creates metadata for one widget using its construction geometry as a hint.
    explicit KLayoutItem(Fl_Widget* widget_value);

    Fl_Widget* widget;
    KSizePolicy size_policy;
    KSize minimum_size;
    KSize preferred_size;
    KSize maximum_size;
    int stretch;
    int row;
    int column;
    int row_span;
    int column_span;
    bool has_minimum_size;
    bool has_preferred_size;
    bool has_maximum_size;
    bool has_grid_position;
};

// KStack is a page stack container that shows exactly one child at a time.
class KStack : public Fl_Group {
public:
    // Creates a stack container; callers add child pages with normal FLTK grouping.
    KStack(int x, int y, int w, int h, const char* label = nullptr);
    // Adds a page widget, stores optional stretch metadata, and returns no value.
    void addWidget(Fl_Widget* widget, int stretch = 0);
    // Sets content margins around every page and returns no value.
    void setContentsMargins(int left, int top, int right, int bottom);
    // Sets a child policy record; the widget is added when it is not already a child.
    void setSizePolicy(Fl_Widget* widget, const KSizePolicy& policy);
    // Sets explicit child hints consumed by sizeHint() and layout constraints.
    void setSizeHints(Fl_Widget* widget, const KSize& minimum, const KSize& preferred, const KSize& maximum);
    // Marks cached child metadata dirty and returns no value.
    void invalidate();
    // Synchronizes metadata and lays out children from the current stack size.
    bool activate();
    // Returns the maximum minimum size of all pages plus contents margins.
    KSize minimumSizeHint() const;
    // Returns the maximum preferred size of all pages plus contents margins.
    KSize preferredSizeHint() const;
    // Returns the maximum page limit plus contents margins.
    KSize maximumSizeHint() const;
    // Returns the preferred size hint, matching Qt naming.
    KSize sizeHint() const;
    // Sets visible page index, clamps to children, and returns no value.
    void setActiveIndex(int index);
    // Returns visible page index, or -1 when there are no children.
    int activeIndex() const;
    // Updates child visibility and geometry from the current parent rect; returns no value.
    void layoutChildren();
    // Paints stack background and active child; returns no value.
    void draw() override;
    // Resizes the stack and active page; returns no value.
    void resize(int x, int y, int w, int h) override;
private:
    KLayoutItem* itemFor(Fl_Widget* widget);
    const KLayoutItem* itemFor(const Fl_Widget* widget) const;

    int active_index_;
    KMargins margins_;
    bool dirty_;
    std::vector<KLayoutItem> items_;
};

// KGridLayout arranges children in a row/column grid with stretch, spacing, and margins.
class KGridLayout : public Fl_Group {
public:
    // Creates a grid container with one row and one column by default.
    KGridLayout(int x, int y, int w, int h, const char* label = nullptr);
    // Sets row/column counts, each clamped to at least one, and returns no value.
    void setGrid(int rows, int columns);
    // Sets spacing between cells and returns no value.
    void setSpacing(int spacing);
    // Compatibility wrapper for setSpacing(); returns no value.
    void setGap(int gap);
    // Sets content margins in left/top/right/bottom order and returns no value.
    void setContentsMargins(int left, int top, int right, int bottom);
    // Compatibility wrapper that applies the same inset on every side.
    void setPadding(int padding);
    // Adds a widget at the next row-major cell and stores optional stretch metadata.
    void addWidget(Fl_Widget* widget, int stretch = 0);
    // Adds a widget at an explicit grid cell and returns no value.
    void addWidget(Fl_Widget* widget, int row, int column, int row_span = 1, int column_span = 1, int stretch = 0);
    // Sets one child stretch by row-major index and returns no value.
    void setStretch(int index, int stretch);
    // Sets one child stretch by widget pointer and returns no value.
    void setStretch(Fl_Widget* widget, int stretch);
    // Sets one row stretch factor and returns no value.
    void setRowStretch(int row, int stretch);
    // Sets one column stretch factor and returns no value.
    void setColumnStretch(int column, int stretch);
    // Sets one row minimum height and returns no value.
    void setRowMinimumHeight(int row, int height);
    // Sets one column minimum width and returns no value.
    void setColumnMinimumWidth(int column, int width);
    // Sets a child size policy; the widget is added when necessary.
    void setSizePolicy(Fl_Widget* widget, const KSizePolicy& policy);
    // Sets explicit child hints consumed during row/column sizing.
    void setSizeHints(Fl_Widget* widget, const KSize& minimum, const KSize& preferred, const KSize& maximum);
    // Marks the grid dirty; the next activate()/draw()/resize recomputes geometry.
    void invalidate();
    // Recomputes geometry from the current parent size and returns whether it was dirty.
    bool activate();
    // Returns the minimum grid size including row/column minima, spacing, and margins.
    KSize minimumSizeHint() const;
    // Returns the preferred grid size including child preferred hints.
    KSize preferredSizeHint() const;
    // Returns the maximum grid size; unbounded axes use kKLayoutMaximumSize.
    KSize maximumSizeHint() const;
    // Returns preferredSizeHint(), matching Qt naming.
    KSize sizeHint() const;
    // Lays out child widgets into grid cells and returns no value.
    void layoutChildren();
    // Paints grid surface and children; returns no value.
    void draw() override;
    // Resizes and relays out children; returns no value.
    void resize(int x, int y, int w, int h) override;
private:
    KLayoutItem* itemFor(Fl_Widget* widget);
    const KLayoutItem* itemFor(const Fl_Widget* widget) const;

    int rows_;
    int columns_;
    int spacing_;
    KMargins margins_;
    bool dirty_;
    std::vector<int> row_stretches_;
    std::vector<int> column_stretches_;
    std::vector<int> row_minimum_heights_;
    std::vector<int> column_minimum_widths_;
    std::vector<KLayoutItem> items_;
};

// KVBox lays out child widgets vertically.
class KVBox : public Fl_Group {
public:
    // Creates a vertical box container.
    KVBox(int x, int y, int w, int h, const char* label = nullptr);
    // Sets spacing between children and returns no value.
    void setSpacing(int spacing);
    // Compatibility wrapper for setSpacing(); returns no value.
    void setGap(int gap);
    // Sets content margins in left/top/right/bottom order and returns no value.
    void setContentsMargins(int left, int top, int right, int bottom);
    // Compatibility wrapper that applies the same inset on every side.
    void setPadding(int padding);
    // Adds a child widget with an optional vertical stretch factor.
    void addWidget(Fl_Widget* widget, int stretch = 0);
    // Sets child stretch by visible child index and returns no value.
    void setStretch(int index, int stretch);
    // Sets child stretch by widget pointer and returns no value.
    void setStretch(Fl_Widget* widget, int stretch);
    // Sets a child size policy; the widget is added when necessary.
    void setSizePolicy(Fl_Widget* widget, const KSizePolicy& policy);
    // Sets explicit child hints consumed by distribution and sizeHint().
    void setSizeHints(Fl_Widget* widget, const KSize& minimum, const KSize& preferred, const KSize& maximum);
    // Marks the layout dirty and returns no value.
    void invalidate();
    // Recomputes geometry from current parent size and returns whether it was dirty.
    bool activate();
    // Returns sum of child vertical minima plus margins and spacing.
    KSize minimumSizeHint() const;
    // Returns sum of child preferred heights plus margins and spacing.
    KSize preferredSizeHint() const;
    // Returns the combined maximum size; unbounded axes use kKLayoutMaximumSize.
    KSize maximumSizeHint() const;
    // Returns preferredSizeHint(), matching Qt naming.
    KSize sizeHint() const;
    // Lays out visible children top-to-bottom and returns no value.
    void layoutChildren();
    // Paints box surface and children; returns no value.
    void draw() override;
    // Resizes and relays out children; returns no value.
    void resize(int x, int y, int w, int h) override;
private:
    KLayoutItem* itemFor(Fl_Widget* widget);
    const KLayoutItem* itemFor(const Fl_Widget* widget) const;

    int spacing_;
    KMargins margins_;
    bool dirty_;
    std::vector<KLayoutItem> items_;
};

// KHBox lays out child widgets horizontally.
class KHBox : public Fl_Group {
public:
    // Creates a horizontal box container.
    KHBox(int x, int y, int w, int h, const char* label = nullptr);
    // Sets spacing between children and returns no value.
    void setSpacing(int spacing);
    // Compatibility wrapper for setSpacing(); returns no value.
    void setGap(int gap);
    // Sets content margins in left/top/right/bottom order and returns no value.
    void setContentsMargins(int left, int top, int right, int bottom);
    // Compatibility wrapper that applies the same inset on every side.
    void setPadding(int padding);
    // Adds a child widget with an optional horizontal stretch factor.
    void addWidget(Fl_Widget* widget, int stretch = 0);
    // Sets child stretch by visible child index and returns no value.
    void setStretch(int index, int stretch);
    // Sets child stretch by widget pointer and returns no value.
    void setStretch(Fl_Widget* widget, int stretch);
    // Sets a child size policy; the widget is added when necessary.
    void setSizePolicy(Fl_Widget* widget, const KSizePolicy& policy);
    // Sets explicit child hints consumed by distribution and sizeHint().
    void setSizeHints(Fl_Widget* widget, const KSize& minimum, const KSize& preferred, const KSize& maximum);
    // Marks the layout dirty and returns no value.
    void invalidate();
    // Recomputes geometry from current parent size and returns whether it was dirty.
    bool activate();
    // Returns sum of child horizontal minima plus margins and spacing.
    KSize minimumSizeHint() const;
    // Returns sum of child preferred widths plus margins and spacing.
    KSize preferredSizeHint() const;
    // Returns the combined maximum size; unbounded axes use kKLayoutMaximumSize.
    KSize maximumSizeHint() const;
    // Returns preferredSizeHint(), matching Qt naming.
    KSize sizeHint() const;
    // Lays out visible children left-to-right and returns no value.
    void layoutChildren();
    // Paints box surface and children; returns no value.
    void draw() override;
    // Resizes and relays out children; returns no value.
    void resize(int x, int y, int w, int h) override;
private:
    KLayoutItem* itemFor(Fl_Widget* widget);
    const KLayoutItem* itemFor(const Fl_Widget* widget) const;

    int spacing_;
    KMargins margins_;
    bool dirty_;
    std::vector<KLayoutItem> items_;
};

// KSplitterPaneOrientation selects horizontal or vertical split behavior.
enum class KSplitterPaneOrientation { Horizontal, Vertical };

// KSplitterPane is an enhanced two-child splitter with persistent ratio and themed divider.
class KSplitterPane : public Fl_Group {
public:
    // Creates a splitter container; first two children are arranged around the divider.
    KSplitterPane(int x, int y, int w, int h, const char* label = nullptr);
    // Adds a splitter child and optionally maps first two stretch factors to ratio.
    void addWidget(Fl_Widget* widget, int stretch = 0);
    // Sets orientation and returns no value.
    void setOrientation(KSplitterPaneOrientation orientation);
    // Sets split ratio in 0.1..0.9 and returns no value.
    void setRatio(double ratio);
    // Returns current split ratio.
    double ratio() const;
    // Sets divider thickness in pixels and returns no value.
    void setSpacing(int spacing);
    // Sets content margins around the splitter panes and returns no value.
    void setContentsMargins(int left, int top, int right, int bottom);
    // Sets one of the first two pane stretch factors and returns no value.
    void setStretch(int index, int stretch);
    // Sets one pane stretch by widget pointer and returns no value.
    void setStretch(Fl_Widget* widget, int stretch);
    // Sets a child size policy; the widget is added when necessary.
    void setSizePolicy(Fl_Widget* widget, const KSizePolicy& policy);
    // Sets explicit child hints used for minimum clamping and size hints.
    void setSizeHints(Fl_Widget* widget, const KSize& minimum, const KSize& preferred, const KSize& maximum);
    // Marks splitter layout data dirty and returns no value.
    void invalidate();
    // Recomputes pane geometry from the current parent size and stored ratio.
    bool activate();
    // Returns the minimum size of both panes plus divider and margins.
    KSize minimumSizeHint() const;
    // Returns the preferred size of both panes plus divider and margins.
    KSize preferredSizeHint() const;
    // Returns the maximum splitter size; unbounded axes use kKLayoutMaximumSize.
    KSize maximumSizeHint() const;
    // Returns preferredSizeHint(), matching Qt naming.
    KSize sizeHint() const;
    // Lays out the first two child panes and returns no value.
    void layoutChildren();
    // Paints divider and children; returns no value.
    void draw() override;
    // Handles divider drag and returns FLTK event handling status.
    int handle(int event) override;
    // Resizes and relays out children; returns no value.
    void resize(int x, int y, int w, int h) override;
private:
    KLayoutItem* itemFor(Fl_Widget* widget);
    const KLayoutItem* itemFor(const Fl_Widget* widget) const;
    void updateRatioFromStretch();

    KSplitterPaneOrientation orientation_;
    double ratio_;
    bool dragging_;
    int divider_size_;
    KMargins margins_;
    bool dirty_;
    std::vector<KLayoutItem> items_;
};

// KAccordionSection stores one expandable text section.
struct KAccordionSection {
    std::string title;
    std::string content;
    bool expanded = false;
    bool enabled = true;
};

// KAccordion draws a compact list of expandable text sections.
class KAccordion : public Fl_Group {
public:
    // Creates an empty accordion and closes child capture immediately.
    KAccordion(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all sections and returns no value.
    void setSections(const std::vector<KAccordionSection>& sections);
    // Toggles one section when valid and enabled; returns no value.
    void toggleSection(int index);
    // Sets active section index and returns no value.
    void setActiveIndex(int index);
    // Returns active section index, or -1 when none is active.
    int activeIndex() const;
    // Paints accordion headers and expanded content; returns no value.
    void draw() override;
    // Handles header clicks and returns FLTK event handling status.
    int handle(int event) override;
private:
    std::vector<KAccordionSection> sections_;
    int active_index_;
    int header_height_;
};

// KExpander is a titled collapsible group container.
class KExpander : public Fl_Group {
public:
    // Creates an expander with title initialized from label.
    KExpander(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setText(const char* text);
    // Sets expanded state, updates child visibility, and returns no value.
    void setExpanded(bool expanded);
    // Returns true when content is visible.
    bool expanded() const;
    // Lays out child content below the header and returns no value.
    void layoutChildren();
    // Paints header, shell, and children when expanded; returns no value.
    void draw() override;
    // Handles header click toggle and returns FLTK event handling status.
    int handle(int event) override;
private:
    std::string text_;
    bool expanded_;
    int header_height_;
};

// KGroupBox is a titled themed container with square border.
class KGroupBox : public Fl_Group {
public:
    // Creates a group box with title initialized from label.
    KGroupBox(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setTitle(const char* title);
    // Returns title text owned by the group box.
    const std::string& title() const;
    // Paints title, border, background, and children; returns no value.
    void draw() override;
private:
    std::string title_;
};

// KScrollablePanel is a themed scroll container with padding metadata.
class KScrollablePanel : public Fl_Scroll {
public:
    // Creates a scrollable panel; callers add content as FLTK children.
    KScrollablePanel(int x, int y, int w, int h, const char* label = nullptr);
    // Stores content padding hint and returns no value.
    void setContentPadding(int padding);
    // Sets explicit background color and returns no value.
    void setBackgroundColor(Fl_Color color);
    // Paints themed scroll surface and children; returns no value.
    void draw() override;
private:
    int padding_;
    Fl_Color background_color_;
};

KStack* KCreateStack(int x, int y, int w, int h, const char* label = nullptr);
KGridLayout* KCreateGridLayout(int x, int y, int w, int h, const char* label = nullptr);
KVBox* KCreateVBox(int x, int y, int w, int h, const char* label = nullptr);
KHBox* KCreateHBox(int x, int y, int w, int h, const char* label = nullptr);
KSplitterPane* KCreateSplitterPane(int x, int y, int w, int h, const char* label = nullptr);
KAccordion* KCreateAccordion(int x, int y, int w, int h, const char* label = nullptr);
KExpander* KCreateExpander(int x, int y, int w, int h, const char* label = nullptr);
KGroupBox* KCreateGroupBox(int x, int y, int w, int h, const char* label = nullptr);
KScrollablePanel* KCreateScrollablePanel(int x, int y, int w, int h, const char* label = nullptr);

#endif // KSWORD_GUI_KLAYOUT_H
