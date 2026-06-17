#ifndef KSWORD_GUI_KNAVIGATION_H
#define KSWORD_GUI_KNAVIGATION_H

#include "KTheme.h"

#include "Fl_Group.H"
#include "Fl_Widget.H"
#include "Fl_Window.H"

#include <string>
#include <vector>

// KNavItem stores one navigation row; inputs are copied by widgets and no ownership is shared.
struct KNavItem {
    std::string text;
    std::string badge;
    bool enabled = true;
};

// KTreeNode stores one tree row and its nested children for lightweight tree rendering.
struct KTreeNode {
    std::string text;
    bool expanded = true;
    bool enabled = true;
    std::vector<KTreeNode> children;
    std::string icon;
    std::string badge;
};

// KCommand stores one command-palette entry including display text, help text, and shortcut text.
struct KCommand {
    std::string title;
    std::string subtitle;
    std::string shortcut;
    bool enabled = true;
};

// KBreadcrumb draws a clickable path trail with active, hover, and separator states.
class KBreadcrumb : public Fl_Widget {
public:
    // Creates a breadcrumb; geometry is used for painting and label is optional metadata.
    KBreadcrumb(int x, int y, int w, int h, const char* label = nullptr);
    // Copies path labels, clamps selection, and returns no value.
    void setItems(const std::vector<std::string>& items);
    // Appends one copied label and returns no value.
    void addItem(const char* text);
    // Removes all labels and interaction state; returns no value.
    void clear();
    // Sets the active label index or clears it when invalid; returns no value.
    void setActiveIndex(int index);
    // Returns active label index, or -1 when nothing is active.
    int activeIndex() const;
    // Sets active/hover accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Returns the accent color used during draw().
    Fl_Color accentColor() const;
    // Paints the full themed breadcrumb surface and returns no value.
    void draw() override;
    // Handles mouse hover and selection; returns FLTK event handling status.
    int handle(int event) override;
private:
    // Computes item hit test from root-window coordinates; returns -1 on miss.
    int itemAt(int mouse_x, int mouse_y) const;
    std::vector<std::string> items_;
    int active_index_;
    int hover_index_;
    Fl_Color accent_color_;
};

// KSideNav draws a vertical navigation list with badges and disabled rows.
class KSideNav : public Fl_Group {
public:
    // Creates an empty side navigation control and closes child capture immediately.
    KSideNav(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all row data, clamps selection, and returns no value.
    void setItems(const std::vector<KNavItem>& items);
    // Appends one enabled text row and returns no value.
    void addItem(const char* text);
    // Clears rows, hover, and active state; returns no value.
    void clear();
    // Selects an enabled row or clears selection for negative input; returns no value.
    void setActiveIndex(int index);
    // Returns selected row index, or -1 when none is selected.
    int activeIndex() const;
    // Sets row height used by painting and hit testing; returns no value.
    void setItemHeight(int height);
    // Sets selected marker accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints side navigation shell and rows; returns no value.
    void draw() override;
    // Handles hover and row selection; returns FLTK event handling status.
    int handle(int event) override;
private:
    std::vector<KNavItem> items_;
    int active_index_;
    int hover_index_;
    int item_height_;
    Fl_Color accent_color_;
};

// KTopNav draws a measured horizontal navigation strip.
class KTopNav : public Fl_Group {
public:
    // Creates an empty top navigation strip and closes child capture immediately.
    KTopNav(int x, int y, int w, int h, const char* label = nullptr);
    // Copies item data and returns no value.
    void setItems(const std::vector<KNavItem>& items);
    // Appends one enabled text item and returns no value.
    void addItem(const char* text);
    // Clears all item and selection state; returns no value.
    void clear();
    // Selects an enabled item or clears selection for negative input; returns no value.
    void setActiveIndex(int index);
    // Returns active item index, or -1 when no item is active.
    int activeIndex() const;
    // Sets underline and active text accent color; returns no value.
    void setAccentColor(Fl_Color color);
    // Paints the flat horizontal nav bar; returns no value.
    void draw() override;
    // Handles hover and click selection; returns FLTK event handling status.
    int handle(int event) override;
private:
    // Computes item hit test from root-window coordinates; returns -1 on miss.
    int itemAt(int mouse_x, int mouse_y) const;
    std::vector<KNavItem> items_;
    int active_index_;
    int hover_index_;
    Fl_Color accent_color_;
};

// KTreeView renders nested KTreeNode data as an expandable lightweight tree.
class KTreeView : public Fl_Group {
public:
    // Creates an empty tree view and closes child capture immediately.
    KTreeView(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all root nodes and preserves only valid selection; returns no value.
    void setItems(const std::vector<KTreeNode>& items);
    // Clears roots and selection; returns no value.
    void clear();
    // Selects a visible row after expansion filtering; returns no value.
    void setActiveIndex(int index);
    // Returns selected visible row index, or -1 when no row is selected.
    int activeIndex() const;
    // Returns selected node text, or an empty string when no row is selected.
    std::string selectedText() const;
    // Sets row height for painting and hit testing; returns no value.
    void setItemHeight(int height);
    // Sets selection and expander accent color; returns no value.
    void setAccentColor(Fl_Color color);
    // Expands all stored nodes recursively and returns no value.
    void expandAll();
    // Collapses all stored nodes recursively and returns no value.
    void collapseAll();
    // Copies filter text; non-empty filters show matching ancestors and descendants.
    void setFilterText(const char* text);
    // Returns filter text owned by this tree view.
    const std::string& filterText() const;
    // Sets whether guide lines are drawn between visible tree rows; returns no value.
    void setShowRootLines(bool show);
    // Returns true when tree guide lines are enabled.
    bool showRootLines() const;
    // Returns selected node index path; empty means no selected node.
    std::vector<int> selectedPath() const;
    // Selects a node by index path when valid and returns no value.
    void setSelectedPath(const std::vector<int>& path);
    // Paints the tree rows, expanders, indentation, and selection; returns no value.
    void draw() override;
    // Handles click selection and expand/collapse; returns FLTK event handling status.
    int handle(int event) override;
private:
    struct VisibleRow;
    // Flattens expanded tree nodes into visible rows; returns copied row metadata.
    std::vector<VisibleRow> visibleRows() const;
    // Finds a mutable node by index path; returns nullptr on invalid input.
    KTreeNode* nodeAtPath(const std::vector<int>& path);
    // Finds a read-only node by index path; returns nullptr on invalid input.
    const KTreeNode* nodeAtPath(const std::vector<int>& path) const;
    std::vector<KTreeNode> roots_;
    std::vector<int> selected_path_;
    std::vector<int> hover_path_;
    int item_height_;
    Fl_Color accent_color_;
    bool show_root_lines_;
    std::string filter_text_;
};

// KCommandPalette is a borderless modal shell for command search and keyboard selection.
class KCommandPalette : public Fl_Window {
public:
    // Creates a command palette window; commands are supplied later through setItems().
    KCommandPalette(int x, int y, int w, int h, const char* label = nullptr);
    // Copies command rows, resets active selection, and returns no value.
    void setItems(const std::vector<KCommand>& commands);
    // Copies query text, resets filtered selection, and returns no value.
    void setQuery(const char* query);
    // Returns the current query owned by the palette.
    const std::string& query() const;
    // Sets empty-query placeholder text and returns no value.
    void setPlaceholder(const char* text);
    // Sets active filtered-row index and returns no value.
    void setActiveIndex(int index);
    // Returns active filtered-row index, or -1 when no row is visible.
    int activeIndex() const;
    // Shows a modal palette loop and returns selected source command index or -1.
    int runModal();
    // Returns selected source command index from the latest interaction, or -1.
    int selectedCommand() const;
    // Paints query field, filtered rows, shortcut hints, and empty state; returns no value.
    void draw() override;
    // Handles text input, keyboard navigation, enter, escape, and mouse selection.
    int handle(int event) override;
private:
    // Computes source command indexes that match query text; returns copied indexes.
    std::vector<int> filteredIndexes() const;
    std::vector<KCommand> commands_;
    std::string query_;
    std::string placeholder_;
    int active_index_;
    int selected_command_;
};

// KSectionHeader draws title, subtitle, action text, accent marker, and divider.
class KSectionHeader : public Fl_Widget {
public:
    // Creates a section header; label initializes title text.
    KSectionHeader(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setText(const char* text);
    // Returns title text owned by the header.
    const std::string& text() const;
    // Replaces subtitle text and returns no value.
    void setSubtitle(const char* text);
    // Replaces optional right-side action text and returns no value.
    void setActionText(const char* text);
    // Sets marker/action accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints header content and divider; returns no value.
    void draw() override;
    // Invokes callback when the action region is clicked; returns FLTK event status.
    int handle(int event) override;
private:
    std::string text_;
    std::string subtitle_;
    std::string action_text_;
    Fl_Color accent_color_;
};


// KCardGridItem stores one card entry for grid-style overview panels.
struct KCardGridItem {
    std::string title;
    std::string subtitle;
    std::string meta;
    bool enabled = true;
};

// KCardGrid draws a fixed-column responsive card collection without touching layout internals.
class KCardGrid : public Fl_Group {
public:
    // Creates an empty card grid and closes child capture immediately.
    KCardGrid(int x, int y, int w, int h, const char* label = nullptr);
    // Copies card items, clamps selected card, and returns no value.
    void setItems(const std::vector<KCardGridItem>& items);
    // Appends one card item and returns no value.
    void addItem(const KCardGridItem& item);
    // Clears cards and interaction state; returns no value.
    void clear();
    // Sets selected card index and returns no value.
    void setActiveIndex(int index);
    // Returns selected card index, or -1 when none is selected.
    int activeIndex() const;
    // Sets preferred column count, clamped to at least one; returns no value.
    void setColumns(int columns);
    // Sets card spacing in pixels and returns no value.
    void setGap(int gap);
    // Sets active card accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints card grid shell, cards, hover state, and selected marker; returns no value.
    void draw() override;
    // Handles hover and click selection; returns FLTK event status.
    int handle(int event) override;
private:
    // Computes card index from root-window coordinates; returns -1 on miss.
    int cardAt(int mouse_x, int mouse_y) const;
    std::vector<KCardGridItem> items_;
    int active_index_;
    int hover_index_;
    int columns_;
    int gap_;
    Fl_Color accent_color_;
};

// KSection is a titled group container with optional action text and themed header chrome.
class KSection : public Fl_Group {
public:
    // Creates a section container; label initializes title text and child capture is closed.
    KSection(int x, int y, int w, int h, const char* label = nullptr);
    // Replaces title text and returns no value.
    void setTitle(const char* text);
    // Replaces subtitle text and returns no value.
    void setSubtitle(const char* text);
    // Replaces optional action text and returns no value.
    void setActionText(const char* text);
    // Sets header height and returns no value.
    void setHeaderHeight(int height);
    // Sets accent marker color and returns no value.
    void setAccentColor(Fl_Color color);
    // Returns y coordinate where caller-managed section content should start.
    int contentY() const;
    // Paints section background/header then child widgets; returns no value.
    void draw() override;
    // Invokes callback when optional action region is clicked; returns FLTK event status.
    int handle(int event) override;
private:
    std::string title_;
    std::string subtitle_;
    std::string action_text_;
    int header_height_;
    Fl_Color accent_color_;
};

// KInspectorField stores one label/value row in an inspector section.
struct KInspectorField {
    std::string name;
    std::string value;
    std::string hint;
    bool read_only = true;
};

// KInspectorSection stores grouped fields for KInspectorPanel.
struct KInspectorSection {
    std::string title;
    std::vector<KInspectorField> fields;
    bool expanded = true;
};

// KInspectorPanel draws grouped property fields for side-inspector workflows.
class KInspectorPanel : public Fl_Group {
public:
    // Creates an empty inspector panel and closes child capture immediately.
    KInspectorPanel(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all inspector sections and returns no value.
    void setSections(const std::vector<KInspectorSection>& sections);
    // Appends one inspector section and returns no value.
    void addSection(const KInspectorSection& section);
    // Clears all sections and returns no value.
    void clear();
    // Sets section expanded state when index is valid and returns no value.
    void setSectionExpanded(int index, bool expanded);
    // Sets row height for field rows and returns no value.
    void setRowHeight(int height);
    // Sets accent color for editable markers and active headers; returns no value.
    void setAccentColor(Fl_Color color);
    // Paints inspector shell, sections, fields, and hints; returns no value.
    void draw() override;
    // Handles section expand/collapse clicks; returns FLTK event status.
    int handle(int event) override;
private:
    // Computes section index from root-window y coordinate; returns -1 on miss.
    int sectionAt(int mouse_y) const;
    std::vector<KInspectorSection> sections_;
    int row_height_;
    int header_height_;
    Fl_Color accent_color_;
};

KBreadcrumb* KCreateBreadcrumb(int x, int y, int w, int h, const char* label = nullptr);
KSideNav* KCreateSideNav(int x, int y, int w, int h, const char* label = nullptr);
KTopNav* KCreateTopNav(int x, int y, int w, int h, const char* label = nullptr);
KTreeView* KCreateTreeView(int x, int y, int w, int h, const char* label = nullptr);
KCommandPalette* KCreateCommandPalette(int x, int y, int w, int h, const char* label = nullptr);
KSectionHeader* KCreateSectionHeader(int x, int y, int w, int h, const char* label = nullptr);
KCardGrid* KCreateCardGrid(int x, int y, int w, int h, const char* label = nullptr);
KSection* KCreateSection(int x, int y, int w, int h, const char* label = nullptr);
KInspectorPanel* KCreateInspectorPanel(int x, int y, int w, int h, const char* label = nullptr);

#endif // KSWORD_GUI_KNAVIGATION_H
