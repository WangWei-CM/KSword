#include "KLayout.h"

#include "fl_draw.H"

#include <algorithm>

namespace {
constexpr int kFont = 12;
constexpr int kTitleFont = 14;

// DrawText renders clipped text with the shared K typography; returns no value.
void DrawText(const std::string& text, int x, int y, int w, int h, Fl_Color color, Fl_Align align, int size = kFont) {
    if (text.empty() || w <= 0 || h <= 0) return;
    fl_color(color); fl_font(FL_HELVETICA, size); fl_draw(text.c_str(), x, y, w, h, align | FL_ALIGN_INSIDE);
}

// DrawBorder renders the one-pixel square frame used by layout containers.
void DrawBorder(int x, int y, int w, int h, Fl_Color color) { fl_color(color); fl_rect(x, y, w, h); }

// ClampRatio keeps splitter panes usable even with extreme input.
double ClampRatio(double ratio) { return std::max(0.1, std::min(0.9, ratio)); }

// HasOnlyChildDamage prevents container backgrounds from being repainted during
// child-only updates such as text caret blinking, input focus, or table cell
// selection. Inputs are live widgets; output is true when only child damage is set.
bool HasOnlyChildDamage(const Fl_Widget* widget) {
    if (!widget) {
        return false;
    }
    const unsigned int damage = widget->damage();
    return (damage & FL_DAMAGE_CHILD) != 0 &&
        (damage & (FL_DAMAGE_ALL | FL_DAMAGE_EXPOSE)) == 0;
}
}

KSize::KSize(int width_value, int height_value)
    : width(width_value),
    height(height_value) {
    // Value type constructor stores caller-provided dimensions directly.  The
    // layout algorithms clamp when consuming hints, so this constructor returns
    // no value and performs no hidden policy decisions.
}

KMargins::KMargins(int left_value, int top_value, int right_value, int bottom_value)
    : left(left_value),
    top(top_value),
    right(right_value),
    bottom(bottom_value) {
    // Margins are stored in Qt-compatible left/top/right/bottom order and are
    // later subtracted from the available layout rectangle.
}

KSizePolicy::KSizePolicy(KSizePolicyType horizontal, KSizePolicyType vertical, int horizontal_stretch, int vertical_stretch)
    : horizontal_policy_(horizontal),
    vertical_policy_(vertical),
    horizontal_stretch_(std::max(0, horizontal_stretch)),
    vertical_stretch_(std::max(0, vertical_stretch)) {
    // The policy object only stores metadata; layouts decide how each policy
    // affects geometry during activate()/layoutChildren().
}

KSizePolicy KSizePolicy::Fixed() { return KSizePolicy(KSizePolicyType::Fixed, KSizePolicyType::Fixed, 0, 0); }
KSizePolicy KSizePolicy::Minimum() { return KSizePolicy(KSizePolicyType::Minimum, KSizePolicyType::Minimum, 0, 0); }
KSizePolicy KSizePolicy::Expanding(int horizontal_stretch, int vertical_stretch) { return KSizePolicy(KSizePolicyType::Expanding, KSizePolicyType::Expanding, horizontal_stretch, vertical_stretch); }
KSizePolicy KSizePolicy::Fill(int horizontal_stretch, int vertical_stretch) { return KSizePolicy(KSizePolicyType::Fill, KSizePolicyType::Fill, horizontal_stretch, vertical_stretch); }
KSizePolicyType KSizePolicy::horizontalPolicy() const { return horizontal_policy_; }
KSizePolicyType KSizePolicy::verticalPolicy() const { return vertical_policy_; }
void KSizePolicy::setHorizontalPolicy(KSizePolicyType policy) { horizontal_policy_ = policy; }
void KSizePolicy::setVerticalPolicy(KSizePolicyType policy) { vertical_policy_ = policy; }
int KSizePolicy::horizontalStretch() const { return horizontal_stretch_; }
int KSizePolicy::verticalStretch() const { return vertical_stretch_; }
void KSizePolicy::setHorizontalStretch(int stretch) { horizontal_stretch_ = std::max(0, stretch); }
void KSizePolicy::setVerticalStretch(int stretch) { vertical_stretch_ = std::max(0, stretch); }

KLayoutItem::KLayoutItem()
    : widget(nullptr),
    size_policy(KSizePolicy::Fill()),
    minimum_size(0, 0),
    preferred_size(0, 0),
    maximum_size(kKLayoutMaximumSize, kKLayoutMaximumSize),
    stretch(0),
    row(0),
    column(0),
    row_span(1),
    column_span(1),
    has_minimum_size(false),
    has_preferred_size(false),
    has_maximum_size(false) {
    // Empty metadata is safe for vector storage and ignored until a widget is set.
}

KLayoutItem::KLayoutItem(Fl_Widget* widget_value)
    : KLayoutItem() {
    // Capture construction geometry as the initial preferred size.  Callers can
    // later override these hints with setSizeHints().
    widget = widget_value;
    if (widget) {
        preferred_size = KSize(widget->w(), widget->h());
        has_preferred_size = true;
    }
}

KStack::KStack(int x, int y, int w, int h, const char* label) : Fl_Group(x, y, w, h, label), active_index_(0) {
    // Stack is a normal FLTK container; callers add page children between begin()/end().
    box(FL_FLAT_BOX); resizable(this);
}

void KStack::setActiveIndex(int index) { active_index_ = children() == 0 ? -1 : std::max(0, std::min(index, children() - 1)); layoutChildren(); redraw(); }
int KStack::activeIndex() const { return children() == 0 ? -1 : active_index_; }

void KStack::layoutChildren() {
    // Only the active page is visible; every page is resized to fill the stack content box.
    if (children() == 0) { active_index_ = -1; return; }
    active_index_ = std::max(0, std::min(active_index_, children() - 1));
    for (int i = 0; i < children(); ++i) { child(i)->resize(x() + 1, y() + 1, std::max(0, w() - 2), std::max(0, h() - 2)); i == active_index_ ? child(i)->show() : child(i)->hide(); }
}

void KStack::draw() {
    if (HasOnlyChildDamage(this)) {
        // Child repaint only: keep the stack shell untouched and let FLTK draw
        // the damaged page widgets so sibling content is not erased.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    layoutChildren();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    draw_children();
}
void KStack::resize(int x, int y, int w, int h) { Fl_Group::resize(x, y, w, h); layoutChildren(); }

KGridLayout::KGridLayout(int x, int y, int w, int h, const char* label) : Fl_Group(x, y, w, h, label), rows_(1), columns_(1), spacing_(8), margins_(8, 8, 8, 8), dirty_(true) {
    // Grid metadata is simple and deterministic so child ordering matches FLTK child order.
    box(FL_FLAT_BOX); resizable(this);
}

void KGridLayout::setGrid(int rows, int columns) { rows_ = std::max(1, rows); columns_ = std::max(1, columns); layoutChildren(); redraw(); }
void KGridLayout::setSpacing(int spacing) { spacing_ = std::max(0, spacing); dirty_ = true; layoutChildren(); redraw(); }
void KGridLayout::setGap(int gap) { setSpacing(gap); }
void KGridLayout::setContentsMargins(int left, int top, int right, int bottom) { margins_ = KMargins(std::max(0, left), std::max(0, top), std::max(0, right), std::max(0, bottom)); dirty_ = true; layoutChildren(); redraw(); }
void KGridLayout::setPadding(int padding) { const int safe = std::max(0, padding); setContentsMargins(safe, safe, safe, safe); }

void KGridLayout::layoutChildren() {
    // Children beyond rows*columns are left at their current geometry but hidden to avoid overlap.
    const int content_w = w() - margins_.left - margins_.right - spacing_ * (columns_ - 1);
    const int content_h = h() - margins_.top - margins_.bottom - spacing_ * (rows_ - 1);
    const int cell_w = std::max(1, content_w / columns_);
    const int cell_h = std::max(1, content_h / rows_);
    for (int i = 0; i < children(); ++i) {
        if (i >= rows_ * columns_) { child(i)->hide(); continue; }
        const int row = i / columns_, col = i % columns_; child(i)->show();
        child(i)->resize(x() + margins_.left + col * (cell_w + spacing_), y() + margins_.top + row * (cell_h + spacing_), cell_w, cell_h);
    }
}

void KGridLayout::draw() {
    if (HasOnlyChildDamage(this)) {
        // Grid geometry is unchanged during child-only updates; avoid clearing
        // the layout surface underneath focused inputs or tables.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    layoutChildren();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    draw_children();
}
void KGridLayout::resize(int x, int y, int w, int h) { Fl_Group::resize(x, y, w, h); layoutChildren(); }

KVBox::KVBox(int x, int y, int w, int h, const char* label) : Fl_Group(x, y, w, h, label), spacing_(8), margins_(8, 8, 8, 8), dirty_(true) {
    // VBox distributes available height evenly across currently visible children.
    box(FL_FLAT_BOX); resizable(this);
}

void KVBox::setSpacing(int spacing) { spacing_ = std::max(0, spacing); dirty_ = true; layoutChildren(); redraw(); }
void KVBox::setGap(int gap) { setSpacing(gap); }
void KVBox::setContentsMargins(int left, int top, int right, int bottom) { margins_ = KMargins(std::max(0, left), std::max(0, top), std::max(0, right), std::max(0, bottom)); dirty_ = true; layoutChildren(); redraw(); }
void KVBox::setPadding(int padding) { const int safe = std::max(0, padding); setContentsMargins(safe, safe, safe, safe); }

void KVBox::layoutChildren() {
    int visible_count = 0; for (int i = 0; i < children(); ++i) if (child(i)->visible()) ++visible_count;
    const int content_h = h() - margins_.top - margins_.bottom - spacing_ * (visible_count - 1);
    const int item_h = visible_count == 0 ? 0 : std::max(1, content_h / visible_count);
    int cy = y() + margins_.top; for (int i = 0; i < children(); ++i) if (child(i)->visible()) { child(i)->resize(x() + margins_.left, cy, std::max(1, w() - margins_.left - margins_.right), item_h); cy += item_h + spacing_; }
}

void KVBox::draw() {
    if (HasOnlyChildDamage(this)) {
        // A child editor can redraw independently; repainting the VBox panel
        // would wipe neighboring children until the next full expose.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    layoutChildren();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    draw_children();
}
void KVBox::resize(int x, int y, int w, int h) { Fl_Group::resize(x, y, w, h); layoutChildren(); }

KHBox::KHBox(int x, int y, int w, int h, const char* label) : Fl_Group(x, y, w, h, label), spacing_(8), margins_(8, 8, 8, 8), dirty_(true) {
    // HBox distributes available width evenly across currently visible children.
    box(FL_FLAT_BOX); resizable(this);
}

void KHBox::setSpacing(int spacing) { spacing_ = std::max(0, spacing); dirty_ = true; layoutChildren(); redraw(); }
void KHBox::setGap(int gap) { setSpacing(gap); }
void KHBox::setContentsMargins(int left, int top, int right, int bottom) { margins_ = KMargins(std::max(0, left), std::max(0, top), std::max(0, right), std::max(0, bottom)); dirty_ = true; layoutChildren(); redraw(); }
void KHBox::setPadding(int padding) { const int safe = std::max(0, padding); setContentsMargins(safe, safe, safe, safe); }

void KHBox::layoutChildren() {
    int visible_count = 0; for (int i = 0; i < children(); ++i) if (child(i)->visible()) ++visible_count;
    const int content_w = w() - margins_.left - margins_.right - spacing_ * (visible_count - 1);
    const int item_w = visible_count == 0 ? 0 : std::max(1, content_w / visible_count);
    int cx = x() + margins_.left; for (int i = 0; i < children(); ++i) if (child(i)->visible()) { child(i)->resize(cx, y() + margins_.top, item_w, std::max(1, h() - margins_.top - margins_.bottom)); cx += item_w + spacing_; }
}

void KHBox::draw() {
    if (HasOnlyChildDamage(this)) {
        // Preserve the HBox background and siblings during child-only focus or
        // selection repaints.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    layoutChildren();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    draw_children();
}
void KHBox::resize(int x, int y, int w, int h) { Fl_Group::resize(x, y, w, h); layoutChildren(); }

KSplitterPane::KSplitterPane(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), orientation_(KSplitterPaneOrientation::Horizontal), ratio_(0.5), dragging_(false) {
    // Splitter arranges only the first two children; extra children retain caller geometry.
    box(FL_FLAT_BOX); resizable(this);
}

void KSplitterPane::setOrientation(KSplitterPaneOrientation orientation) { orientation_ = orientation; layoutChildren(); redraw(); }
void KSplitterPane::setRatio(double ratio) { ratio_ = ClampRatio(ratio); layoutChildren(); redraw(); }
double KSplitterPane::ratio() const { return ratio_; }

void KSplitterPane::layoutChildren() {
    if (children() < 2) return; const int divider = 6;
    if (orientation_ == KSplitterPaneOrientation::Horizontal) {
        const int first_w = static_cast<int>((w() - divider) * ratio_); child(0)->resize(x(), y(), first_w, h()); child(1)->resize(x() + first_w + divider, y(), w() - first_w - divider, h());
    } else {
        const int first_h = static_cast<int>((h() - divider) * ratio_); child(0)->resize(x(), y(), w(), first_h); child(1)->resize(x(), y() + first_h + divider, w(), h() - first_h - divider);
    }
}

void KSplitterPane::draw() {
    if (HasOnlyChildDamage(this) && !dragging_) {
        // While not dragging, child-only updates belong entirely to pane
        // contents; do not repaint the split surface or divider.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    layoutChildren();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    draw_children();
    if (children() >= 2) {
        fl_color(dragging_ ? t.primary : t.border);
        if (orientation_ == KSplitterPaneOrientation::Horizontal) {
            const int dx = child(0)->x() + child(0)->w();
            fl_rectf(dx, y(), 6, h());
        }
        else {
            const int dy = child(0)->y() + child(0)->h();
            fl_rectf(x(), dy, w(), 6);
        }
    }
}

int KSplitterPane::handle(int event) {
    if (children() < 2) return Fl_Group::handle(event); const int divider = 6; const int dx = child(0)->x() + child(0)->w(); const int dy = child(0)->y() + child(0)->h();
    const bool hit = orientation_ == KSplitterPaneOrientation::Horizontal ? (Fl::event_x() >= dx && Fl::event_x() <= dx + divider) : (Fl::event_y() >= dy && Fl::event_y() <= dy + divider);
    if (event == FL_PUSH && hit) { dragging_ = true; return 1; }
    if (event == FL_RELEASE && dragging_) { dragging_ = false; redraw(); return 1; }
    if (event == FL_DRAG && dragging_) { setRatio(orientation_ == KSplitterPaneOrientation::Horizontal ? static_cast<double>(Fl::event_x() - x()) / std::max(1, w()) : static_cast<double>(Fl::event_y() - y()) / std::max(1, h())); return 1; }
    return Fl_Group::handle(event);
}

void KSplitterPane::resize(int x, int y, int w, int h) { Fl_Group::resize(x, y, w, h); layoutChildren(); }

KAccordion::KAccordion(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), sections_(), active_index_(-1), header_height_(30) {
    // Accordion owns text sections only and is not a child container.
    box(FL_FLAT_BOX); end();
}

void KAccordion::setSections(const std::vector<KAccordionSection>& sections) { sections_ = sections; active_index_ = sections_.empty() ? -1 : std::max(0, std::min(active_index_, static_cast<int>(sections_.size()) - 1)); redraw(); }
void KAccordion::toggleSection(int index) { if (index >= 0 && index < static_cast<int>(sections_.size()) && sections_[index].enabled) { sections_[index].expanded = !sections_[index].expanded; active_index_ = index; redraw(); } }
void KAccordion::setActiveIndex(int index) { active_index_ = (index >= 0 && index < static_cast<int>(sections_.size())) ? index : -1; redraw(); }
int KAccordion::activeIndex() const { return active_index_; }

void KAccordion::draw() {
    const KTheme& t = KThemeManager::instance().theme(); int cy = y(); fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    for (int i = 0; i < static_cast<int>(sections_.size()) && cy < y() + h(); ++i) {
        const auto& s = sections_[i]; fl_color(i == active_index_ ? t.selection : t.controlBg); fl_rectf(x() + 1, cy + 1, w() - 2, header_height_ - 1);
        DrawText(s.expanded ? "-" : "+", x() + 10, cy, 16, header_height_, t.primary, FL_ALIGN_CENTER); DrawText(s.title, x() + 32, cy, w() - 42, header_height_, s.enabled ? t.text : t.mutedText, FL_ALIGN_LEFT, kTitleFont); cy += header_height_;
        if (s.expanded) { DrawText(s.content, x() + 14, cy + 4, w() - 28, 48, t.mutedText, FL_ALIGN_LEFT | FL_ALIGN_TOP); cy += 56; }
    }
    fl_pop_clip();
}

int KAccordion::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) { int cy = y(); for (int i = 0; i < static_cast<int>(sections_.size()); ++i) { if (Fl::event_y() >= cy && Fl::event_y() < cy + header_height_) { toggleSection(i); do_callback(); return 1; } cy += header_height_ + (sections_[i].expanded ? 56 : 0); } }
    return Fl_Group::handle(event);
}

KExpander::KExpander(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), text_(label ? label : ""), expanded_(true), header_height_(28) {
    // Expander is a real container; child visibility follows expanded_.
    box(FL_FLAT_BOX); resizable(this);
}

void KExpander::setText(const char* text) { text_ = text ? text : ""; redraw(); }
void KExpander::setExpanded(bool expanded) { expanded_ = expanded; layoutChildren(); redraw(); }
bool KExpander::expanded() const { return expanded_; }

void KExpander::layoutChildren() {
    for (int i = 0; i < children(); ++i) { expanded_ ? child(i)->show() : child(i)->hide(); if (expanded_) child(i)->resize(x() + 8, y() + header_height_ + 6, std::max(1, w() - 16), std::max(1, h() - header_height_ - 14)); }
}

void KExpander::draw() {
    if (HasOnlyChildDamage(this)) {
        // The header did not change; repaint only expanded children.
        if (expanded_) {
            draw_children();
        }
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    layoutChildren();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    fl_color(t.controlAltBg);
    fl_rectf(x() + 1, y() + 1, w() - 2, header_height_);
    DrawText(expanded_ ? "-" : "+", x() + 8, y(), 18, header_height_, t.primary, FL_ALIGN_CENTER);
    DrawText(text_, x() + 30, y(), w() - 38, header_height_, t.text, FL_ALIGN_LEFT, kTitleFont);
    if (expanded_) {
        draw_children();
    }
}

int KExpander::handle(int event) { if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && Fl::event_y() >= y() && Fl::event_y() < y() + header_height_) { setExpanded(!expanded_); do_callback(); return 1; } return Fl_Group::handle(event); }

KGroupBox::KGroupBox(int x, int y, int w, int h, const char* label) : Fl_Group(x, y, w, h, label), title_(label ? label : "") {
    // GroupBox is a normal child container with a title row.
    box(FL_FLAT_BOX); resizable(this);
}

void KGroupBox::setTitle(const char* title) { title_ = title ? title : ""; label(title_.c_str()); redraw(); }
const std::string& KGroupBox::title() const { return title_; }

void KGroupBox::draw() {
    if (HasOnlyChildDamage(this)) {
        // Keep titled frame pixels stable during child-only redraws.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    fl_color(t.panelBg);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    fl_color(t.controlAltBg);
    fl_rectf(x() + 1, y() + 1, w() - 2, 26);
    DrawText(title_, x() + 10, y() + 1, w() - 20, 26, t.text, FL_ALIGN_LEFT, kTitleFont);
    draw_children();
}

KScrollablePanel::KScrollablePanel(int x, int y, int w, int h, const char* label)
    : Fl_Scroll(x, y, w, h, label), padding_(8), background_color_(FL_BACKGROUND_COLOR) {
    // Scroll panel uses FLTK scrolling while painting its background from KThemeManager.
    box(FL_FLAT_BOX); resizable(this);
}

void KScrollablePanel::setContentPadding(int padding) { padding_ = std::max(0, padding); redraw(); }
void KScrollablePanel::setBackgroundColor(Fl_Color color) { background_color_ = color; redraw(); }

void KScrollablePanel::draw() {
    if (HasOnlyChildDamage(this)) {
        // Scroll children repaint themselves; avoid filling the viewport over
        // partially redrawn editors or table cells.
        draw_children();
        return;
    }
    const KTheme& t = KThemeManager::instance().theme();
    (void)padding_;
    fl_color(background_color_ == FL_BACKGROUND_COLOR ? t.panelBg : background_color_);
    fl_rectf(x(), y(), w(), h());
    DrawBorder(x(), y(), w(), h(), t.border);
    Fl_Scroll::draw();
}
