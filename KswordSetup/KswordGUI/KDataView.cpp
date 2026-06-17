#include "KDataView.h"

#include "fl_draw.H"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kFont = 12;
constexpr int kTitleFont = 14;

// DrawText paints clipped text using common K typography and returns no value.
void DrawText(const std::string& text, int x, int y, int w, int h, Fl_Color color, Fl_Align align, int size = kFont) {
    if (text.empty() || w <= 0 || h <= 0) return;
    fl_color(color); fl_font(FL_HELVETICA, size); fl_draw(text.c_str(), x, y, w, h, align | FL_ALIGN_INSIDE);
}

// DrawBorder paints the flat square border used by data widgets.
void DrawBorder(int x, int y, int w, int h, Fl_Color color) { fl_color(color); fl_rect(x, y, w, h); }

// ClampIndex returns a valid index for non-empty collections, otherwise -1.
int ClampIndex(int index, std::size_t count) { return count == 0 ? -1 : std::max(0, std::min(index, static_cast<int>(count) - 1)); }

// Normalize maps a value into 0..1 and protects against zero range.
double Normalize(double value, double minimum, double maximum) {
    if (maximum <= minimum) return 0.0;
    return std::max(0.0, std::min(1.0, (value - minimum) / (maximum - minimum)));
}

// ResolveAccent follows the current theme unless an explicit caller accent was set.
Fl_Color ResolveAccent(Fl_Color color) { return color == FL_BACKGROUND_COLOR ? KThemeManager::instance().theme().primary : color; }
}

KListView::KListView(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), items_(), active_index_(-1), hover_index_(-1), item_height_(44), empty_text_("No items"), accent_color_(FL_BACKGROUND_COLOR) {
    // ListView is self-painted and intentionally not a child container.
    box(FL_FLAT_BOX); end();
}

void KListView::setItems(const std::vector<KListViewItem>& items) { items_ = items; active_index_ = ClampIndex(active_index_, items_.size()); redraw(); }
void KListView::addItem(const KListViewItem& item) { items_.push_back(item); if (active_index_ < 0) active_index_ = 0; redraw(); }
void KListView::clear() { items_.clear(); active_index_ = -1; hover_index_ = -1; redraw(); }
void KListView::setActiveIndex(int index) { active_index_ = ClampIndex(index, items_.size()); redraw(); }
int KListView::activeIndex() const { return active_index_; }
std::string KListView::selectedText() const { return active_index_ >= 0 && active_index_ < static_cast<int>(items_.size()) ? items_[active_index_].title : std::string(); }
void KListView::setItemHeight(int height) { item_height_ = std::max(28, height); redraw(); }
void KListView::setEmptyText(const char* text) { empty_text_ = text ? text : ""; redraw(); }
void KListView::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KListView::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.controlBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    if (items_.empty()) { DrawText(empty_text_, x(), y(), w(), h(), t.mutedText, FL_ALIGN_CENTER); fl_pop_clip(); return; }
    int cy = y() + 1; for (int i = 0; i < static_cast<int>(items_.size()) && cy < y() + h(); ++i, cy += item_height_) {
        const bool selected = i == active_index_, hover = i == hover_index_ && items_[i].enabled; fl_color(selected ? t.selection : (hover ? t.hover : (i % 2 ? t.controlAltBg : t.controlBg))); fl_rectf(x() + 1, cy, w() - 2, item_height_);
        if (selected) { fl_color(accent); fl_rectf(x() + 1, cy, 3, item_height_); }
        DrawText(items_[i].title, x() + 10, cy + 4, w() - 100, 18, items_[i].enabled ? (selected ? accent : t.text) : t.mutedText, FL_ALIGN_LEFT, kTitleFont);
        DrawText(items_[i].detail, x() + 10, cy + 23, w() - 100, 16, t.mutedText, FL_ALIGN_LEFT); DrawText(items_[i].meta, x() + w() - 86, cy, 76, item_height_, t.mutedText, FL_ALIGN_RIGHT);
    }
    fl_pop_clip();
}

int KListView::handle(int event) {
    const int row = (Fl::event_y() - y() - 1) / item_height_; const bool valid = row >= 0 && row < static_cast<int>(items_.size());
    if (event == FL_MOVE || event == FL_ENTER) { hover_index_ = valid ? row : -1; redraw(); return 1; }
    if (event == FL_LEAVE) { hover_index_ = -1; redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        if (valid && items_[row].enabled) { active_index_ = row; do_callback(); redraw(); return 1; }
    }
    return Fl_Group::handle(event);
}

KPropertyGrid::KPropertyGrid(int x, int y, int w, int h, const char* label) : Fl_Widget(x, y, w, h, label), properties_(), name_column_width_(0), row_height_(24) {
    // PropertyGrid stores simple name/value pairs and does not edit them directly.
    box(FL_FLAT_BOX);
}

void KPropertyGrid::setProperties(const std::vector<KPropertyItem>& properties) { properties_ = properties; redraw(); }
void KPropertyGrid::setProperty(const char* name, const char* value) {
    const std::string key = name ? name : ""; for (KPropertyItem& item : properties_) if (item.name == key) { item.value = value ? value : ""; redraw(); return; }
    properties_.push_back(KPropertyItem{ key, value ? value : "" }); redraw();
}
void KPropertyGrid::clear() { properties_.clear(); redraw(); }
void KPropertyGrid::setNameColumnWidth(int width) { name_column_width_ = std::max(0, width); redraw(); }
int KPropertyGrid::nameColumnWidth() const { return name_column_width_; }
void KPropertyGrid::setRowHeight(int height) { row_height_ = std::max(18, height); redraw(); }

void KPropertyGrid::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const int row_h = row_height_, key_w = name_column_width_ > 0 ? std::min(name_column_width_, std::max(40, w() - 60)) : w() / 3;
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border); fl_color(t.border); fl_line(x() + key_w, y(), x() + key_w, y() + h());
    for (int i = 0; i < static_cast<int>(properties_.size()); ++i) { const int cy = y() + i * row_h; if (cy >= y() + h()) break; fl_color(i % 2 ? t.controlAltBg : t.controlBg); fl_rectf(x() + 1, cy + 1, w() - 2, row_h - 1); DrawText(properties_[i].name, x() + 8, cy, key_w - 12, row_h, t.mutedText, FL_ALIGN_LEFT); DrawText(properties_[i].value, x() + key_w + 8, cy, w() - key_w - 16, row_h, t.text, FL_ALIGN_LEFT); }
    fl_pop_clip();
}

KKeyValueTable::KKeyValueTable(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), rows_(), key_header_("Key"), value_header_("Value") {
    // KeyValueTable is read-only and intentionally independent from KTable.
    box(FL_FLAT_BOX);
}

void KKeyValueTable::setRows(const std::vector<KKeyValueRow>& rows) { rows_ = rows; redraw(); }
void KKeyValueTable::addRow(const char* key, const char* value) { rows_.push_back(KKeyValueRow{ key ? key : "", value ? value : "" }); redraw(); }
void KKeyValueTable::setHeaders(const char* keyHeader, const char* valueHeader) { key_header_ = keyHeader ? keyHeader : ""; value_header_ = valueHeader ? valueHeader : ""; redraw(); }
void KKeyValueTable::clear() { rows_.clear(); redraw(); }

void KKeyValueTable::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const int row_h = 24, split = w() / 2;
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    fl_color(t.controlAltBg); fl_rectf(x() + 1, y() + 1, w() - 2, row_h); DrawText(key_header_, x() + 8, y(), split - 12, row_h, t.text, FL_ALIGN_LEFT, kTitleFont); DrawText(value_header_, x() + split + 8, y(), w() - split - 16, row_h, t.text, FL_ALIGN_LEFT, kTitleFont);
    fl_color(t.border); fl_line(x() + split, y(), x() + split, y() + h()); for (int i = 0; i < static_cast<int>(rows_.size()); ++i) { const int cy = y() + row_h * (i + 1); if (cy >= y() + h()) break; fl_color(i % 2 ? t.controlAltBg : t.controlBg); fl_rectf(x() + 1, cy + 1, w() - 2, row_h - 1); DrawText(rows_[i].key, x() + 8, cy, split - 12, row_h, t.mutedText, FL_ALIGN_LEFT); DrawText(rows_[i].value, x() + split + 8, cy, w() - split - 16, row_h, t.text, FL_ALIGN_LEFT); }
    fl_pop_clip();
}

KMiniChart::KMiniChart(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), values_(), type_(KMiniChartType::Line), auto_range_(true), minimum_(0.0), maximum_(1.0), accent_color_(FL_BACKGROUND_COLOR) {
    // MiniChart is data-only and draws line/bar variants from the same value vector.
    box(FL_FLAT_BOX);
}

void KMiniChart::setValues(const std::vector<double>& values) { values_ = values; redraw(); }
void KMiniChart::setType(KMiniChartType type) { type_ = type; redraw(); }
void KMiniChart::setRange(double minimum, double maximum) { minimum_ = minimum; maximum_ = maximum; auto_range_ = false; redraw(); }
void KMiniChart::setAutoRange(bool enabled) { auto_range_ = enabled; redraw(); }
void KMiniChart::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KMiniChart::draw() {
    const KTheme& t = KThemeManager::instance().theme(); fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    if (values_.empty()) { DrawText("No data", x(), y(), w(), h(), t.mutedText, FL_ALIGN_CENTER); fl_pop_clip(); return; }
    double mn = minimum_, mx = maximum_; if (auto_range_) { mn = *std::min_element(values_.begin(), values_.end()); mx = *std::max_element(values_.begin(), values_.end()); }
    const int left = x() + 6, top = y() + 6, cw = std::max(1, w() - 12), ch = std::max(1, h() - 12); fl_color(ResolveAccent(accent_color_));
    if (type_ == KMiniChartType::Bar) { const int bw = std::max(2, cw / static_cast<int>(values_.size())); for (int i = 0; i < static_cast<int>(values_.size()); ++i) { const int bh = static_cast<int>(Normalize(values_[i], mn, mx) * ch); fl_rectf(left + i * bw, top + ch - bh, std::max(1, bw - 2), bh); } }
    else { for (int i = 1; i < static_cast<int>(values_.size()); ++i) { const int x1 = left + (i - 1) * cw / std::max(1, static_cast<int>(values_.size()) - 1), x2 = left + i * cw / std::max(1, static_cast<int>(values_.size()) - 1); const int y1 = top + ch - static_cast<int>(Normalize(values_[i - 1], mn, mx) * ch), y2 = top + ch - static_cast<int>(Normalize(values_[i], mn, mx) * ch); fl_line(x1, y1, x2, y2); } }
    fl_pop_clip();
}

KProgressRing::KProgressRing(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), value_(0.0), text_(label ? label : ""), accent_color_(FL_BACKGROUND_COLOR) {
    // Ring uses repeated arcs for thickness while keeping FLTK-only drawing.
    box(FL_FLAT_BOX);
}

void KProgressRing::setValue(double value) { value_ = std::max(0.0, std::min(1.0, value)); redraw(); }
double KProgressRing::value() const { return value_; }
void KProgressRing::setText(const char* text) { text_ = text ? text : ""; redraw(); }
void KProgressRing::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KProgressRing::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const int size = std::max(1, std::min(w(), h()) - 8); const int ox = x() + (w() - size) / 2, oy = y() + (h() - size) / 2;
    const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h());
    for (int i = 0; i < 4; ++i) { fl_color(t.border); fl_arc(ox + i, oy + i, size - i * 2, size - i * 2, 0, 360); fl_color(accent); fl_arc(ox + i, oy + i, size - i * 2, size - i * 2, 90, 90 - 360 * value_); }
    const std::string label = text_.empty() ? std::to_string(static_cast<int>(value_ * 100.0 + 0.5)) + "%" : text_; DrawText(label, x(), y(), w(), h(), t.text, FL_ALIGN_CENTER, kTitleFont); fl_pop_clip();
}

KStepper::KStepper(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), steps_(), active_index_(-1), accent_color_(FL_BACKGROUND_COLOR) {
    // Stepper stores a sequence of steps and paints them horizontally.
    box(FL_FLAT_BOX);
}

void KStepper::setSteps(const std::vector<KStepItem>& steps) { steps_ = steps; active_index_ = ClampIndex(active_index_, steps_.size()); redraw(); }
void KStepper::setActiveIndex(int index) { active_index_ = ClampIndex(index, steps_.size()); redraw(); }
int KStepper::activeIndex() const { return active_index_; }
void KStepper::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KStepper::draw() {
    const KTheme& t = KThemeManager::instance().theme(); fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    const Fl_Color accent = ResolveAccent(accent_color_);
    if (steps_.empty()) { DrawText("No steps", x(), y(), w(), h(), t.mutedText, FL_ALIGN_CENTER); fl_pop_clip(); return; }
    const int count = static_cast<int>(steps_.size()), cy = y() + 26; for (int i = 0; i < count; ++i) {
        const int cx = x() + 20 + (count == 1 ? 0 : i * (w() - 40) / (count - 1)); if (i > 0) { const int px = x() + 20 + (i - 1) * (w() - 40) / std::max(1, count - 1); fl_color((i <= active_index_ || steps_[i - 1].completed) ? accent : t.border); fl_line(px + 6, cy, cx - 6, cy); }
        fl_color(i == active_index_ || steps_[i].completed ? accent : t.controlAltBg); fl_rectf(cx - 6, cy - 6, 12, 12); DrawBorder(cx - 6, cy - 6, 12, 12, t.border);
        DrawText(steps_[i].title, cx - 50, cy + 12, 100, 18, steps_[i].enabled ? t.text : t.mutedText, FL_ALIGN_CENTER); DrawText(steps_[i].detail, cx - 50, cy + 30, 100, 18, t.mutedText, FL_ALIGN_CENTER);
    }
    fl_pop_clip();
}

int KStepper::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && !steps_.empty()) {
        const int count = static_cast<int>(steps_.size()); int best = -1, best_dist = 999999;
        for (int i = 0; i < count; ++i) { const int cx = x() + 20 + (count == 1 ? 0 : i * (w() - 40) / (count - 1)); const int dist = std::abs(Fl::event_x() - cx); if (dist < best_dist) { best = i; best_dist = dist; } }
        if (best >= 0 && best_dist <= 24 && steps_[best].enabled) { active_index_ = best; do_callback(); redraw(); return 1; }
    }
    return Fl_Widget::handle(event);
}
