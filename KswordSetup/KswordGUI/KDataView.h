#ifndef KSWORD_GUI_KDATAVIEW_H
#define KSWORD_GUI_KDATAVIEW_H

#include "KTheme.h"

#include "Fl_Group.H"
#include "Fl_Widget.H"

#include <string>
#include <vector>

// KListViewItem stores one rich list row.
struct KListViewItem {
    std::string title;
    std::string detail;
    std::string meta;
    bool enabled = true;
};

// KListView draws a lightweight selectable list with title/detail/meta columns.
class KListView : public Fl_Group {
public:
    // Creates an empty list view and closes child capture immediately.
    KListView(int x, int y, int w, int h, const char* label = nullptr);
    // Copies list items and returns no value.
    void setItems(const std::vector<KListViewItem>& items);
    // Appends one item and returns no value.
    void addItem(const KListViewItem& item);
    // Clears items and selection; returns no value.
    void clear();
    // Sets selected row index and returns no value.
    void setActiveIndex(int index);
    // Returns selected row index, or -1 when none is selected.
    int activeIndex() const;
    // Returns selected title, or empty string when no row is selected.
    std::string selectedText() const;
    // Sets row height used by painting and hit testing; returns no value.
    void setItemHeight(int height);
    // Replaces empty-state text and returns no value.
    void setEmptyText(const char* text);
    // Sets selected row accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints list surface and rows; returns no value.
    void draw() override;
    // Handles click selection and returns FLTK event handling status.
    int handle(int event) override;
private:
    std::vector<KListViewItem> items_;
    int active_index_;
    int hover_index_;
    int item_height_;
    std::string empty_text_;
    Fl_Color accent_color_;
};

// KPropertyItem stores one property name/value row.
struct KPropertyItem {
    std::string name;
    std::string value;
};

// KPropertyGrid draws editable-model style property rows without editing behavior.
class KPropertyGrid : public Fl_Widget {
public:
    // Creates an empty property grid.
    KPropertyGrid(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all properties and returns no value.
    void setProperties(const std::vector<KPropertyItem>& properties);
    // Adds or updates a property by name and returns no value.
    void setProperty(const char* name, const char* value);
    // Clears all properties and returns no value.
    void clear();
    // Sets preferred name column width in pixels; non-positive restores automatic width.
    void setNameColumnWidth(int width);
    // Returns preferred name column width, or 0 when automatic width is active.
    int nameColumnWidth() const;
    // Sets row height for property rows and returns no value.
    void setRowHeight(int height);
    // Paints property rows and column divider; returns no value.
    void draw() override;
private:
    std::vector<KPropertyItem> properties_;
    int name_column_width_;
    int row_height_;
};

// KKeyValueRow stores one simple key/value table row.
struct KKeyValueRow {
    std::string key;
    std::string value;
};

// KKeyValueTable draws a two-column key/value table.
class KKeyValueTable : public Fl_Widget {
public:
    // Creates an empty key/value table.
    KKeyValueTable(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all rows and returns no value.
    void setRows(const std::vector<KKeyValueRow>& rows);
    // Appends one key/value row and returns no value.
    void addRow(const char* key, const char* value);
    // Replaces header labels and returns no value.
    void setHeaders(const char* keyHeader, const char* valueHeader);
    // Clears rows and returns no value.
    void clear();
    // Paints header, rows, grid lines, and values; returns no value.
    void draw() override;
private:
    std::vector<KKeyValueRow> rows_;
    std::string key_header_;
    std::string value_header_;
};

// KMiniChartType selects line or bar rendering.
enum class KMiniChartType { Line, Bar };

// KMiniChart draws small inline line or bar charts from numeric values.
class KMiniChart : public Fl_Widget {
public:
    // Creates an empty mini chart.
    KMiniChart(int x, int y, int w, int h, const char* label = nullptr);
    // Copies numeric values and returns no value.
    void setValues(const std::vector<double>& values);
    // Sets line or bar rendering and returns no value.
    void setType(KMiniChartType type);
    // Sets explicit min/max range and returns no value.
    void setRange(double minimum, double maximum);
    // Enables or disables automatic range calculation and returns no value.
    void setAutoRange(bool enabled);
    // Sets accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints chart frame, bars/line, and empty state; returns no value.
    void draw() override;
private:
    std::vector<double> values_;
    KMiniChartType type_;
    bool auto_range_;
    double minimum_;
    double maximum_;
    Fl_Color accent_color_;
};

// KProgressRing draws circular progress with centered text.
class KProgressRing : public Fl_Widget {
public:
    // Creates a progress ring with value 0.
    KProgressRing(int x, int y, int w, int h, const char* label = nullptr);
    // Sets progress value in 0..1 and returns no value.
    void setValue(double value);
    // Returns progress value in 0..1.
    double value() const;
    // Replaces centered text and returns no value.
    void setText(const char* text);
    // Sets ring accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints ring background, progress arc, and centered label; returns no value.
    void draw() override;
private:
    double value_;
    std::string text_;
    Fl_Color accent_color_;
};

// KStepItem stores one step label, detail, and state.
struct KStepItem {
    std::string title;
    std::string detail;
    bool completed = false;
    bool enabled = true;
};

// KStepper draws a horizontal multi-step progress and selection control.
class KStepper : public Fl_Widget {
public:
    // Creates an empty stepper.
    KStepper(int x, int y, int w, int h, const char* label = nullptr);
    // Copies all steps and returns no value.
    void setSteps(const std::vector<KStepItem>& steps);
    // Sets active step index and returns no value.
    void setActiveIndex(int index);
    // Returns active step index, or -1 when no step is active.
    int activeIndex() const;
    // Sets accent color and returns no value.
    void setAccentColor(Fl_Color color);
    // Paints step line, nodes, titles, and details; returns no value.
    void draw() override;
    // Handles click selection and returns FLTK event handling status.
    int handle(int event) override;
private:
    std::vector<KStepItem> steps_;
    int active_index_;
    Fl_Color accent_color_;
};

KListView* KCreateListView(int x, int y, int w, int h, const char* label = nullptr);
KPropertyGrid* KCreatePropertyGrid(int x, int y, int w, int h, const char* label = nullptr);
KKeyValueTable* KCreateKeyValueTable(int x, int y, int w, int h, const char* label = nullptr);
KMiniChart* KCreateMiniChart(int x, int y, int w, int h, const char* label = nullptr);
KProgressRing* KCreateProgressRing(int x, int y, int w, int h, const char* label = nullptr);
KStepper* KCreateStepper(int x, int y, int w, int h, const char* label = nullptr);

#endif // KSWORD_GUI_KDATAVIEW_H
