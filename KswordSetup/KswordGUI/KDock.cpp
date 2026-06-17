#include "KDock.h"
#include "KTheme.h"
#include "Fl.H"
#include "fl_draw.H"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

// KDockSplitOrientation describes how one split node divides its rectangle.
// Horizontal splits allocate width to left/right children, and vertical splits
// allocate height to top/bottom children.
enum class KDockSplitOrientation {
    Horizontal,
    Vertical
};

// KDockLayoutNode is the manager's private split tree.  Leaf nodes reference a
// KDockArea tab container, while split nodes own two child nodes and a ratio.
class KDockLayoutNode {
public:
    // Creates a leaf node for one dock area; callers attach it into the tree.
    explicit KDockLayoutNode(KDockArea* leafArea)
        : is_leaf(true),
        area(leafArea),
        orientation(KDockSplitOrientation::Horizontal),
        first_ratio(500),
        parent(nullptr),
        first(nullptr),
        second(nullptr) {
    }

    // Creates a split node and connects both children back to this parent.
    KDockLayoutNode(KDockSplitOrientation splitOrientation,
        KDockLayoutNode* firstChild,
        KDockLayoutNode* secondChild,
        int ratioPermille)
        : is_leaf(false),
        area(nullptr),
        orientation(splitOrientation),
        first_ratio(ratioPermille),
        parent(nullptr),
        first(firstChild),
        second(secondChild) {
        if (first) {
            first->parent = this;
        }
        if (second) {
            second->parent = this;
        }
    }

    bool is_leaf;
    KDockArea* area;
    KDockSplitOrientation orientation;
    int first_ratio;
    KDockLayoutNode* parent;
    KDockLayoutNode* first;
    KDockLayoutNode* second;
};

// KDockFloatingWindow is the temporary top-level host used after a dock leaves
// the manager.  FLTK can deliver drag/release events to the grabbed window
// instead of the reparented KDockWidget, so this window forwards those events
// back to the dock and keeps drag-out/redock interaction deterministic.
class KDockFloatingWindow : public Fl_Window {
public:
    // Creates a regular top-level window and stores the dock that owns it.
    KDockFloatingWindow(int x, int y, int w, int h, const char* title, KDockWidget* dock)
        : Fl_Window(x, y, w, h, title),
        dock_(dock) {
    }

    // Rebinds the proxied dock when the host is reused; returns no value.
    void setDockWidget(KDockWidget* dock) {
        dock_ = dock;
    }

    // Proxies active drag events to the dock, otherwise uses normal FLTK window handling.
    int handle(int event) override {
        if (dock_ && dock_->hasActiveFloatingDrag()) {
            if (event == FL_DRAG) {
                dock_->continueFloatingDrag(Fl::event_x_root(), Fl::event_y_root());
                return 1;
            }
            if (event == FL_RELEASE) {
                dock_->finishFloatingDrag(Fl::event_x_root(), Fl::event_y_root());
                return 1;
            }
        }
        return Fl_Window::handle(event);
    }

private:
    KDockWidget* dock_;
};

namespace {
// Dock metrics are intentionally small so the module stays lightweight and native.
const int kTitleHeight = 24;
const int kTabHeight = 24;
const int kButtonSize = 16;
const int kButtonGap = 4;
const int kTitleButtonCount = 3;
const int kMargin = 3;
const int kMinClientSize = 24;
const int kDragThreshold = 4;
const int kDropGuideSize = 32;
const int kDropGuideInset = 12;
const int kDropPreviewMin = 36;

// HasOnlyChildDamage mirrors the container guard used by KPanel/KCard. Inputs
// are live widgets; output is true when FLTK is repainting only damaged children
// and the dock shell must not clear its whole rectangle.
bool HasOnlyChildDamage(const Fl_Widget* widget) {
    if (!widget) {
        return false;
    }
    const uchar damage = widget->damage();
    return (damage & FL_DAMAGE_CHILD) != 0 &&
        (damage & (FL_DAMAGE_ALL | FL_DAMAGE_EXPOSE)) == 0;
}

// Dock palette helpers read the active runtime theme without storing stale colors.
Fl_Color kColorWindow() { return KThemeManager::instance().theme().windowBg; }
Fl_Color kColorPanel() { return KThemeManager::instance().theme().panelBg; }
Fl_Color kColorBorder() { return KThemeManager::instance().theme().border; }
Fl_Color kColorPrimary() { return KThemeManager::instance().theme().primary; }
Fl_Color kColorPrimaryDark() { return KThemeManager::instance().theme().primaryDark; }
Fl_Color kColorText() { return KThemeManager::instance().theme().text; }
Fl_Color kColorMuted() { return KThemeManager::instance().theme().mutedText; }
// Converts an area enum into a stable token used by saveLayout().
const char* PositionToToken(KDockAreaPosition position) {
    switch (position) {
    case KDockAreaPosition::Left:
        return "Left";
    case KDockAreaPosition::Right:
        return "Right";
    case KDockAreaPosition::Top:
        return "Top";
    case KDockAreaPosition::Bottom:
        return "Bottom";
    case KDockAreaPosition::Center:
    default:
        return "Center";
    }
}
// Parses a stable area token used by restoreLayout().
bool TokenToPosition(const std::string& token, KDockAreaPosition& position) {
    if (token == "Left") {
        position = KDockAreaPosition::Left;
        return true;
    }
    if (token == "Right") {
        position = KDockAreaPosition::Right;
        return true;
    }
    if (token == "Top") {
        position = KDockAreaPosition::Top;
        return true;
    }
    if (token == "Bottom") {
        position = KDockAreaPosition::Bottom;
        return true;
    }
    if (token == "Center") {
        position = KDockAreaPosition::Center;
        return true;
    }
    return false;
}
// Converts a drop overlay side into the current fixed KDockManager region.
KDockAreaPosition SideToPosition(KDockDropSide side) {
    switch (side) {
    case KDockDropSide::Left:
        return KDockAreaPosition::Left;
    case KDockDropSide::Right:
        return KDockAreaPosition::Right;
    case KDockDropSide::Top:
        return KDockAreaPosition::Top;
    case KDockDropSide::Bottom:
        return KDockAreaPosition::Bottom;
    case KDockDropSide::Center:
        return KDockAreaPosition::Center;
    case KDockDropSide::None:
    default:
        return KDockAreaPosition::Center;
    }
}
// Picks a center target when the pointer is away from edges, otherwise returns
// the nearest side.  The caller passes manager or dock-local coordinates.
KDockDropSide SideForPointInBox(int local_x, int local_y, int width, int height) {
    int safe_w = std::max(1, width);
    int safe_h = std::max(1, height);
    int center_left = safe_w / 3;
    int center_right = safe_w - center_left;
    int center_top = safe_h / 3;
    int center_bottom = safe_h - center_top;
    if (local_x >= center_left && local_x < center_right &&
        local_y >= center_top && local_y < center_bottom) {
        return KDockDropSide::Center;
    }
    int left_distance = std::max(0, local_x);
    int right_distance = std::max(0, safe_w - 1 - local_x);
    int top_distance = std::max(0, local_y);
    int bottom_distance = std::max(0, safe_h - 1 - local_y);
    int best = std::min(std::min(left_distance, right_distance), std::min(top_distance, bottom_distance));
    if (best == left_distance) {
        return KDockDropSide::Left;
    }
    if (best == right_distance) {
        return KDockDropSide::Right;
    }
    if (best == top_distance) {
        return KDockDropSide::Top;
    }
    return KDockDropSide::Bottom;
}
// Escapes layout text so titles can contain separators without breaking restore.
std::string EscapeTitle(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '|' || ch == '\n' || ch == '\r') {
            out.push_back('\\');
            if (ch == '\n') {
                out.push_back('n');
            }
            else if (ch == '\r') {
                out.push_back('r');
            }
            else {
                out.push_back(ch);
            }
        }
        else {
            out.push_back(ch);
        }
    }
    return out;
}
// Reverses EscapeTitle() and tolerates simple unknown escape sequences.
std::string UnescapeTitle(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            ++i;
            if (value[i] == 'n') {
                out.push_back('\n');
            }
            else if (value[i] == 'r') {
                out.push_back('\r');
            }
            else {
                out.push_back(value[i]);
            }
        }
        else {
            out.push_back(value[i]);
        }
    }
    return out;
}
// Draws a compact square title-button with one printable glyph.
void DrawTitleButton(int x, int y, const char* glyph, Fl_Color bg) {
    fl_color(bg);
    fl_rectf(x, y, kButtonSize, kButtonSize);
    fl_color(kColorBorder());
    fl_rect(x, y, kButtonSize, kButtonSize);
    fl_color(kColorText());
    fl_font(FL_HELVETICA_BOLD, 11);
    fl_draw(glyph, x, y, kButtonSize, kButtonSize, FL_ALIGN_CENTER);
}
// Draws one square docking guide marker and highlights the active target side.
void DrawDropGuide(int x, int y, const char* label, bool active) {
    Fl_Color fill = active ? kColorPrimary() : fl_color_average(kColorPanel(), kColorWindow(), 0.65f);
    Fl_Color text = active ? FL_WHITE : kColorText();
    fl_color(fill);
    fl_rectf(x, y, kDropGuideSize, kDropGuideSize);
    fl_color(active ? kColorPrimaryDark() : kColorBorder());
    fl_rect(x, y, kDropGuideSize, kDropGuideSize);
    fl_color(text);
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_draw(label, x, y, kDropGuideSize, kDropGuideSize, FL_ALIGN_CENTER);
}
// Draws the five target markers around either the manager or a hovered dock widget.
void DrawDropGuideSet(int box_x, int box_y, int box_w, int box_h, KDockDropSide active_side) {
    int safe_w = std::max(kDropGuideSize, box_w);
    int safe_h = std::max(kDropGuideSize, box_h);
    int center_x = box_x + safe_w / 2 - kDropGuideSize / 2;
    int center_y = box_y + safe_h / 2 - kDropGuideSize / 2;
    DrawDropGuide(center_x, box_y + kDropGuideInset, "T", active_side == KDockDropSide::Top);
    DrawDropGuide(center_x, box_y + safe_h - kDropGuideInset - kDropGuideSize, "B", active_side == KDockDropSide::Bottom);
    DrawDropGuide(box_x + kDropGuideInset, center_y, "L", active_side == KDockDropSide::Left);
    DrawDropGuide(box_x + safe_w - kDropGuideInset - kDropGuideSize, center_y, "R", active_side == KDockDropSide::Right);
    DrawDropGuide(center_x, center_y, "C", active_side == KDockDropSide::Center);
}
// Draws a lightweight preview rectangle for the currently selected docking side.
void DrawDropPreview(int box_x, int box_y, int box_w, int box_h, KDockDropSide side) {
    int preview_x = box_x;
    int preview_y = box_y;
    int preview_w = std::max(1, box_w);
    int preview_h = std::max(1, box_h);
    int side_w = std::max(kDropPreviewMin, preview_w / 4);
    int side_h = std::max(kDropPreviewMin, preview_h / 4);
    if (side == KDockDropSide::Left) {
        preview_w = side_w;
    }
    else if (side == KDockDropSide::Right) {
        preview_x = box_x + std::max(0, box_w - side_w);
        preview_w = side_w;
    }
    else if (side == KDockDropSide::Top) {
        preview_h = side_h;
    }
    else if (side == KDockDropSide::Bottom) {
        preview_y = box_y + std::max(0, box_h - side_h);
        preview_h = side_h;
    }
    else if (side == KDockDropSide::Center) {
        preview_x = box_x + preview_w / 4;
        preview_y = box_y + preview_h / 4;
        preview_w = std::max(kDropPreviewMin, preview_w / 2);
        preview_h = std::max(kDropPreviewMin, preview_h / 2);
    }
    else {
        return;
    }
    Fl_Color preview_fill = fl_color_average(kColorPrimary(), kColorWindow(), 0.28f);
    fl_color(preview_fill);
    fl_rectf(preview_x, preview_y, preview_w, preview_h);
    fl_color(kColorPrimaryDark());
    fl_rect(preview_x, preview_y, preview_w, preview_h);
    fl_rect(preview_x + 1, preview_y + 1, std::max(1, preview_w - 2), std::max(1, preview_h - 2));
    // A light hatch pattern makes the preview visible even on similar themes.
    for (int line_x = preview_x - preview_h; line_x < preview_x + preview_w; line_x += 10) {
        fl_line(line_x, preview_y + preview_h, line_x + preview_h, preview_y);
    }
}
// Moves a widget from any current parent to a new group using normal FLTK ownership APIs.
void ReparentWidget(Fl_Widget* widget, Fl_Group* target) {
    if (!widget || !target) {
        return;
    }
    Fl_Group* old_parent = widget->parent();
    if (old_parent == target) {
        return;
    }
    if (old_parent) {
        old_parent->remove(widget);
    }
    target->add(widget);
}
// Computes root-screen coordinates for a widget by combining window root and widget local offsets.
void WidgetRootBox(const Fl_Widget* widget, int& root_x, int& root_y, int& width, int& height) {
    root_x = 0;
    root_y = 0;
    width = 0;
    height = 0;
    if (!widget) {
        return;
    }
    width = widget->w();
    height = widget->h();
    root_x = widget->x();
    root_y = widget->y();
    Fl_Window* top = widget->top_window();
    if (top) {
        root_x += top->x_root();
        root_y += top->y_root();
    }
}
// Returns a bounded integer so fixed side sizes never consume the whole manager.
int ClampSize(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}
// Converts a split orientation into a compact token for saveLayout().
const char* OrientationToToken(KDockSplitOrientation orientation) {
    return orientation == KDockSplitOrientation::Vertical ? "V" : "H";
}
// Parses one split orientation token during restoreLayout().
bool TokenToOrientation(const std::string& token, KDockSplitOrientation& orientation) {
    if (token == "H") {
        orientation = KDockSplitOrientation::Horizontal;
        return true;
    }
    if (token == "V") {
        orientation = KDockSplitOrientation::Vertical;
        return true;
    }
    return false;
}
// Splits non-escaped layout metadata fields; titles are always stored as the
// final field on lines that need escaping, so node metadata can remain simple.
std::vector<std::string> SplitPlainFields(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    for (char ch : line) {
        if (ch == '|') {
            fields.push_back(field);
            field.clear();
        }
        else {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}
// Finds the leaf node that owns an area; returns nullptr if the area is absent.
KDockLayoutNode* FindLeafForArea(KDockLayoutNode* node, KDockArea* dockArea) {
    if (!node || !dockArea) {
        return nullptr;
    }
    if (node->is_leaf) {
        return node->area == dockArea ? node : nullptr;
    }
    KDockLayoutNode* found = FindLeafForArea(node->first, dockArea);
    if (found) {
        return found;
    }
    return FindLeafForArea(node->second, dockArea);
}
// Returns true when a layout node subtree contains a dock area as one of its leaves.
bool LayoutContainsArea(KDockLayoutNode* node, KDockArea* dockArea) {
    return FindLeafForArea(node, dockArea) != nullptr;
}
// Compares two overlay targets so mouse-move handling can skip redundant full
// dock-manager redraws while the pointer remains over the same guide target.
bool DropTargetsEqual(const KDockDropTarget& left, const KDockDropTarget& right) {
    return left.valid == right.valid &&
        left.scope == right.scope &&
        left.side == right.side &&
        left.area == right.area &&
        left.dock == right.dock &&
        left.position == right.position;
}
// Serializes one layout-tree node and returns the generated node id.
std::string WriteLayoutNode(const KDockLayoutNode* node, std::ostringstream& out, int& next_node_id) {
    std::string node_id = "N" + std::to_string(next_node_id++);
    if (!node) {
        out << "NODE|" << node_id << "|LEAF|Center\n";
        return node_id;
    }
    if (node->is_leaf) {
        const char* layout_id = (node->area ? node->area->layoutId().c_str() : "Center");
        out << "NODE|" << node_id << "|LEAF|" << layout_id << "\n";
        return node_id;
    }
    std::string first_id = WriteLayoutNode(node->first, out, next_node_id);
    std::string second_id = WriteLayoutNode(node->second, out, next_node_id);
    out << "NODE|" << node_id << "|SPLIT|" << OrientationToToken(node->orientation)
        << "|" << ClampSize(node->first_ratio, 100, 900)
        << "|" << first_id << "|" << second_id << "\n";
    return node_id;
}
// Returns the numeric suffix from a dynamic area id, or -1 when not present.
int DynamicAreaNumber(const std::string& layout_id) {
    const std::string prefix = "Dynamic";
    if (layout_id.compare(0, prefix.size(), prefix) != 0) {
        return -1;
    }
    const char* digits = layout_id.c_str() + prefix.size();
    if (!*digits) {
        return -1;
    }
    return std::atoi(digits);
}
// IsAncestor walks FLTK parent links to detect an accidental containment cycle.
bool IsAncestor(const Fl_Widget* possible_ancestor, const Fl_Widget* widget) {
    for (const Fl_Widget* current = widget; current; current = current->parent()) {
        if (current == possible_ancestor) {
            return true;
        }
    }
    return false;
}
// ScopedLayoutFlag marks a manual layout pass and restores the flag on every exit.
class ScopedLayoutFlag {
public:
    // Stores a reference to the caller-owned flag and sets it for this scope.
    explicit ScopedLayoutFlag(bool& flag)
        : flag_(flag) {
        flag_ = true;
    }

    // Clears the caller-owned flag when the guarded layout pass finishes.
    ~ScopedLayoutFlag() {
        flag_ = false;
    }

private:
    bool& flag_;
};
} // namespace
KDockDropTarget::KDockDropTarget()
    : valid(false),
    scope(KDockDropScope::None),
    side(KDockDropSide::None),
    area(nullptr),
    dock(nullptr),
    position(KDockAreaPosition::Center) {
}

KDockWidget::KDockWidget(int x, int y, int w, int h, const char* title, Fl_Widget* content)
    : Fl_Group(x, y, w, h, title),
    title_(title ? title : "Dock"),
    icon_name_(),
    content_(nullptr),
    manager_(nullptr),
    area_(nullptr),
    floating_window_(nullptr),
    pinned_(true),
    dragging_(false),
    drag_started_(false),
    layouting_content_(false),
    drag_start_root_x_(0),
    drag_start_root_y_(0),
    drag_offset_x_(0),
    drag_offset_y_(0) {
    box(FL_FLAT_BOX);
    color(kColorPanel());
    copy_label(title_.c_str());
    // KDockWidget lays out its single content child explicitly in layoutContent().
    // Disabling Fl_Group's proportional child resizing prevents a second resize
    // policy from feeding content geometry back into this dock during resize().
    resizable(nullptr);
    begin();
    end();
    setContent(content);
}
KDockWidget::~KDockWidget() {
    // The floating window is a temporary host; detach this widget before deleting it.
    if (floating_window_) {
        floating_window_->remove(this);
        Fl_Window* host = floating_window_;
        floating_window_ = nullptr;
        delete host;
    }
}
void KDockWidget::draw() {
    if (HasOnlyChildDamage(this)) {
        // Child-only damage comes from embedded editors, tables, and text views.
        // Avoid clearing the dock chrome when FLTK will repaint only that child.
        draw_children();
        return;
    }

    // Draw the panel body first so hidden content never leaks through the title bar.
    fl_color(kColorPanel());
    fl_rectf(x(), y(), w(), h());
    fl_color(kColorBorder());
    fl_rect(x(), y(), w(), h());
    // Title bar carries drag semantics and uses active-tab color to show focus.
    bool active = !area_ || area_->activeDockWidget() == this || isFloating();
    Fl_Color title_fill = active ? fl_color_average(kColorPrimary(), kColorWindow(), 0.22f) : kColorWindow();
    fl_color(title_fill);
    fl_rectf(x() + 1, y() + 1, std::max(0, w() - 2), kTitleHeight - 1);
    fl_color(active ? kColorPrimary() : kColorBorder());
    fl_rectf(x() + 1, y() + 1, 3, kTitleHeight - 1);
    fl_color(kColorBorder());
    fl_line(x(), y() + kTitleHeight, x() + w() - 1, y() + kTitleHeight);
    // Icon support is intentionally string-backed until resources are wired in.
    int title_left = x() + 9;
    if (!icon_name_.empty()) {
        fl_color(active ? kColorPrimary() : kColorBorder());
        fl_rectf(title_left, y() + 5, 14, 14);
        fl_color(active ? FL_WHITE : kColorMuted());
        fl_font(FL_HELVETICA_BOLD, 9);
        fl_draw(icon_name_.substr(0, 1).c_str(), title_left, y() + 5, 14, 14, FL_ALIGN_CENTER);
        title_left += 19;
    }
    // Keep title text clipped away from pin, float, and close buttons.
    int buttons_w = kButtonSize * kTitleButtonCount + kButtonGap * (kTitleButtonCount + 1);
    int text_right = x() + w() - buttons_w;
    int text_w = std::max(0, text_right - title_left);
    fl_color(kColorText());
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_draw(title_.c_str(), title_left, y(), text_w, kTitleHeight, FL_ALIGN_LEFT | FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    // Buttons are drawn with ASCII glyphs for portability across configured fonts.
    DrawTitleButton(x() + w() - kButtonSize * 3 - kButtonGap * 3, y() + 4, pinned_ ? "P" : "p", kColorWindow());
    DrawTitleButton(x() + w() - kButtonSize * 2 - kButtonGap * 2, y() + 4, "^", kColorWindow());
    DrawTitleButton(x() + w() - kButtonSize - kButtonGap, y() + 4, "x", kColorWindow());
    // Draw only the active content child; KDockArea controls show/hide before redraw.
    draw_children();
}
int KDockWidget::handle(int event) {
    int local_x = Fl::event_x() - x();
    int local_y = Fl::event_y() - y();
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        if (inCloseButton(local_x, local_y)) {
            closeDock();
            return 1;
        }
        if (inFloatButton(local_x, local_y)) {
            floatDock();
            return 1;
        }
        if (inPinButton(local_x, local_y)) {
            togglePinned();
            return 1;
        }
        if (inTitleBar(local_x, local_y)) {
            dragging_ = true;
            drag_started_ = false;
            drag_start_root_x_ = Fl::event_x_root();
            drag_start_root_y_ = Fl::event_y_root();
            drag_offset_x_ = local_x;
            drag_offset_y_ = local_y;
            take_focus();
            return 1;
        }
    }
    if (event == FL_DRAG && dragging_) {
        int root_x = Fl::event_x_root();
        int root_y = Fl::event_y_root();
        int dx = root_x - drag_start_root_x_;
        int dy = root_y - drag_start_root_y_;
        if (!drag_started_ && (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold)) {
            drag_started_ = true;
            beginFloatingDrag(root_x, root_y);
        }
        if (drag_started_) {
            continueFloatingDrag(root_x, root_y);
        }
        return 1;
    }
    if (event == FL_RELEASE && dragging_) {
        finishFloatingDrag(Fl::event_x_root(), Fl::event_y_root());
        return 1;
    }
    return Fl_Group::handle(event);
}
void KDockWidget::setContent(Fl_Widget* content) {
    if (content == this) {
        return;
    }
    if (content_ == content) {
        layoutContent();
        return;
    }
    // Detach old content but do not delete it; ownership stays with caller/FLTK tree.
    if (content_ && content_->parent() == this) {
        remove(content_);
    }
    content_ = content;
    if (content_) {
        // Some content widgets are Fl_Group subclasses. If such a widget was the
        // current FLTK group while this dock was constructed, FLTK may already
        // have inserted this dock below the future content. Break that accidental
        // ancestor relation before making the content a child of the dock.
        if (IsAncestor(content_, this)) {
            Fl_Group* dock_parent = parent();
            if (dock_parent) {
                dock_parent->remove(this);
            }
        }
        ReparentWidget(content_, this);
        content_->show();
    }
    layoutContent();
}
Fl_Widget* KDockWidget::content() const {
    return content_;
}
void KDockWidget::setTitle(const std::string& title) {
    title_ = title;
    copy_label(title_.c_str());
    redraw();
}
const std::string& KDockWidget::title() const {
    return title_;
}
void KDockWidget::setIconName(const std::string& iconName) {
    // The dock stores only a logical icon id so callers can wire resources later.
    icon_name_ = iconName;
    redraw();
    if (area_) {
        area_->redraw();
    }
}
const std::string& KDockWidget::iconName() const {
    return icon_name_;
}
void KDockWidget::setPinned(bool pinned) {
    // Pinning is reserved state today; drawing updates make the state observable.
    pinned_ = pinned;
    redraw();
}
bool KDockWidget::isPinned() const {
    return pinned_;
}
void KDockWidget::togglePinned() {
    setPinned(!pinned_);
}
void KDockWidget::setDockManager(KDockManager* manager) {
    manager_ = manager;
}
KDockManager* KDockWidget::dockManager() const {
    return manager_;
}
void KDockWidget::setDockArea(KDockArea* area) {
    area_ = area;
}
KDockArea* KDockWidget::dockArea() const {
    return area_;
}
void KDockWidget::floatDock() {
    if (floating_window_) {
        floating_window_->show();
        floating_window_->take_focus();
        return;
    }
    int root_x = 0;
    int root_y = 0;
    int old_w = 0;
    int old_h = 0;
    WidgetRootBox(this, root_x, root_y, old_w, old_h);
    int float_w = std::max(260, old_w > 0 ? old_w : 320);
    int float_h = std::max(180, old_h > 0 ? old_h : 240);
    createFloatingWindowAt(root_x + 32, root_y + 32, float_w, float_h);
}
void KDockWidget::closeDock() {
    // Close means hidden and removed from its area, not deleted.
    KDockManager* active_manager = manager_;
    if (area_) {
        area_->removeDockWidget(this);
    }
    if (floating_window_) {
        floating_window_->hide();
        floating_window_->remove(this);
        Fl_Window* host = floating_window_;
        floating_window_ = nullptr;
        delete host;
    }
    if (parent()) {
        parent()->remove(this);
    }
    area_ = nullptr;
    hide();
    if (active_manager) {
        active_manager->cleanupEmptyAreas();
        active_manager->layoutAreas();
        active_manager->redraw();
    }
}
void KDockWidget::activateDock() {
    // Activation is area-local; floating docks already own their visible host.
    if (area_) {
        area_->setActiveDockWidget(this);
    }
    else {
        show();
        redraw();
    }
}
void KDockWidget::layoutContent() {
    if (!content_ || layouting_content_) {
        return;
    }
    ScopedLayoutFlag guard(layouting_content_);
    // Content uses the full area below the native title bar.
    int client_x = x() + kMargin;
    int client_y = y() + kTitleHeight + kMargin;
    int client_w = std::max(kMinClientSize, w() - kMargin * 2);
    int client_h = std::max(kMinClientSize, h() - kTitleHeight - kMargin * 2);
    // Only issue a real resize when geometry changed. This avoids needless
    // virtual resize callbacks from child Fl_Group widgets during tab changes.
    const bool geometry_changed = content_->x() != client_x || content_->y() != client_y ||
        content_->w() != client_w || content_->h() != client_h;
    const bool was_hidden = !content_->visible();
    if (geometry_changed) {
        content_->resize(client_x, client_y, client_w, client_h);
    }
    content_->show();
    if (geometry_changed || was_hidden) {
        redraw();
    }
}
bool KDockWidget::isFloating() const {
    return floating_window_ != nullptr;
}
bool KDockWidget::floatingGeometry(int& root_x, int& root_y, int& width, int& height) const {
    if (!floating_window_) {
        root_x = 0;
        root_y = 0;
        width = 0;
        height = 0;
        return false;
    }
    root_x = floating_window_->x();
    root_y = floating_window_->y();
    width = floating_window_->w();
    height = floating_window_->h();
    return true;
}
bool KDockWidget::floatingCenterRootPoint(int& root_x, int& root_y) const {
    // Inputs are output references. Processing uses the host window bounds
    // because users often release the mouse over the floating title bar while
    // the host body covers a dock target. The return value says whether a
    // floating host exists and a center point was produced.
    int fx = 0;
    int fy = 0;
    int fw = 0;
    int fh = 0;
    if (!floatingGeometry(fx, fy, fw, fh)) {
        root_x = 0;
        root_y = 0;
        return false;
    }
    root_x = fx + std::max(1, fw) / 2;
    root_y = fy + std::max(1, fh) / 2;
    return true;
}
void KDockWidget::resize(int x, int y, int w, int h) {
    Fl_Group::resize(x, y, w, h);
    layoutContent();
}
void KDockWidget::detachFloatingWindow() {
    if (!floating_window_) {
        return;
    }
    // The dock itself survives; only the temporary host window is destroyed.
    Fl_Window* host = floating_window_;
    floating_window_ = nullptr;
    if (KDockFloatingWindow* floating_host = dynamic_cast<KDockFloatingWindow*>(host)) {
        floating_host->setDockWidget(nullptr);
    }
    Fl::grab(nullptr);
    host->hide();
    if (parent() == host) {
        host->remove(this);
    }
    Fl::delete_widget(host);
}
void KDockWidget::createFloatingWindowAt(int root_x, int root_y, int width, int height) {
    int float_w = std::max(260, width);
    int float_h = std::max(180, height);
    // Existing floating hosts are reused so drag operations do not churn windows.
    if (floating_window_) {
        if (KDockFloatingWindow* host = dynamic_cast<KDockFloatingWindow*>(floating_window_)) {
            host->setDockWidget(this);
        }
        floating_window_->resize(root_x, root_y, float_w, float_h);
        resize(0, 0, float_w, float_h);
        show();
        floating_window_->show();
        floating_window_->take_focus();
        return;
    }
    // Removing through the area keeps tab bookkeeping consistent before reparenting.
    KDockManager* active_manager = manager_;
    if (area_) {
        area_->removeDockWidget(this);
    }
    else if (parent()) {
        parent()->remove(this);
    }
    area_ = nullptr;
    // Runtime construction can happen while another FLTK group is current.
    // Clearing current() prevents the floating host from becoming a child of
    // the old dock area, which would make it invisible to normal window routing.
    Fl_Group* previous_current = Fl_Group::current();
    Fl_Group::current(nullptr);
    floating_window_ = new KDockFloatingWindow(root_x, root_y, float_w, float_h, title_.c_str(), this);
    Fl_Group::current(previous_current);
    floating_window_->begin();
    floating_window_->add(this);
    resize(0, 0, float_w, float_h);
    floating_window_->resizable(this);
    floating_window_->end();
    show();
    floating_window_->show();
    floating_window_->take_focus();
    // The manager must hide now-empty side areas as soon as a dock leaves them.
    if (active_manager) {
        active_manager->cleanupEmptyAreas();
        active_manager->layoutAreas();
        active_manager->redraw();
    }
}
void KDockWidget::beginFloatingDrag(int root_x, int root_y) {
    int old_w = std::max(260, w());
    int old_h = std::max(180, h());
    int float_x = root_x - drag_offset_x_;
    int float_y = root_y - drag_offset_y_;
    createFloatingWindowAt(float_x, float_y, old_w, old_h);
    // Root cause fix for "dragging out from inside" failures: the widget is
    // reparented during FL_DRAG, so explicitly grab the new top-level host to
    // keep subsequent drag/release events in the floating dock window.
    if (floating_window_) {
        Fl::grab(floating_window_);
    }
}
void KDockWidget::moveFloatingWindowForDrag(int root_x, int root_y) {
    if (!floating_window_) {
        return;
    }
    int float_x = root_x - drag_offset_x_;
    int float_y = root_y - drag_offset_y_;
    // Moving the top-level host gives the user immediate visual separation.
    floating_window_->position(float_x, float_y);
}
void KDockWidget::continueFloatingDrag(int root_x, int root_y) {
    // This method is intentionally usable from both the dock widget and the
    // floating host.  Inputs are root-screen coordinates, processing keeps the
    // floating window under the cursor and refreshes the manager overlay, and
    // there is no return value.
    if (!dragging_ || !drag_started_) {
        return;
    }
    moveFloatingWindowForDrag(root_x, root_y);
    if (manager_) {
        manager_->updateDockDrag(this, root_x, root_y);
    }
}
void KDockWidget::finishFloatingDrag(int root_x, int root_y) {
    // Finish is centralized so FL_RELEASE is handled correctly even when FLTK
    // sends the event to the grabbed floating window instead of this child.
    // Inputs are root-screen coordinates; processing clears grab/drag state and
    // either docks through the manager or leaves the existing floating host.
    const bool was_drag = drag_started_;
    dragging_ = false;
    drag_started_ = false;
    Fl::grab(nullptr);
    if (was_drag && manager_) {
        moveFloatingWindowForDrag(root_x, root_y);
        manager_->dockWidgetFromDrag(this, root_x, root_y);
        return;
    }
    if (manager_) {
        manager_->clearDockDrag();
    }
}
bool KDockWidget::hasActiveFloatingDrag() const {
    // The floating host only needs to proxy events after a drag crossed the
    // threshold and created/reused a host window.
    return dragging_ && drag_started_;
}
bool KDockWidget::inTitleBar(int local_x, int local_y) const {
    return local_x >= 0 && local_x < w() && local_y >= 0 && local_y < kTitleHeight;
}
bool KDockWidget::inCloseButton(int local_x, int local_y) const {
    int bx = w() - kButtonSize - kButtonGap;
    int by = 4;
    return local_x >= bx && local_x < bx + kButtonSize && local_y >= by && local_y < by + kButtonSize;
}
bool KDockWidget::inFloatButton(int local_x, int local_y) const {
    int bx = w() - kButtonSize * 2 - kButtonGap * 2;
    int by = 4;
    return local_x >= bx && local_x < bx + kButtonSize && local_y >= by && local_y < by + kButtonSize;
}
bool KDockWidget::inPinButton(int local_x, int local_y) const {
    int bx = w() - kButtonSize * 3 - kButtonGap * 3;
    int by = 4;
    return local_x >= bx && local_x < bx + kButtonSize && local_y >= by && local_y < by + kButtonSize;
}
KDockArea::KDockArea(int x, int y, int w, int h, KDockAreaPosition position, const char* label)
    : Fl_Group(x, y, w, h, label),
    position_(position),
    layout_id_(PositionToToken(position)),
    docks_(),
    active_index_(-1),
    hover_index_(-1),
    default_area_(false),
    layouting_docks_(false),
    tab_dragging_(false),
    tab_drag_index_(-1),
    tab_drag_start_root_x_(0),
    tab_drag_start_root_y_(0),
    tab_drag_widget_(nullptr) {
    box(FL_FLAT_BOX);
    color(kColorWindow());
    // Dock areas perform tab/content placement in layoutDocks(); FLTK's
    // default group-resize policy would otherwise move docks a second time.
    resizable(nullptr);
    begin();
    end();
}
void KDockArea::draw() {
    if (HasOnlyChildDamage(this)) {
        // The active dock owns its child repaint. Redrawing the tab area here
        // would erase inactive tabs or sibling regions during focused typing.
        draw_children();
        return;
    }

    fl_color(kColorWindow());
    fl_rectf(x(), y(), w(), h());
    fl_color(kColorBorder());
    fl_rect(x(), y(), w(), h());
    // Draw tabs before children; inactive docks are hidden by layoutDocks().
    int count = dockCount();
    if (count > 0) {
        int tab_w = std::max(40, w() / count);
        for (int i = 0; i < count; ++i) {
            int tx = x() + i * tab_w;
            int tw = (i == count - 1) ? (x() + w() - tx) : tab_w;
            bool selected = (i == active_index_);
            bool hovered = (i == hover_index_);
            Fl_Color tab_fill = selected ? kColorPrimary()
                : (hovered ? fl_color_average(kColorPanel(), kColorPrimary(), 0.18f) : kColorPanel());
            fl_color(tab_fill);
            fl_rectf(tx, y(), std::max(0, tw), kTabHeight);
            fl_color(kColorBorder());
            fl_rect(tx, y(), std::max(0, tw), kTabHeight);
            if (selected) {
                fl_color(kColorPrimaryDark());
                fl_rectf(tx, y() + kTabHeight - 3, std::max(0, tw), 3);
            }
            fl_color(selected ? FL_WHITE : (hovered ? kColorText() : kColorMuted()));
            fl_font(FL_HELVETICA, 12);
            const char* tab_label = docks_[i] ? docks_[i]->title().c_str() : "";
            fl_draw(tab_label, tx + 6, y(), std::max(0, tw - 12), kTabHeight, FL_ALIGN_LEFT | FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
        }
    }
    draw_children();
}
int KDockArea::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        int local_x = Fl::event_x() - x();
        int local_y = Fl::event_y() - y();
        int index = tabIndexAt(local_x, local_y);
        if (index >= 0 && index < dockCount()) {
            // A tab press both activates the dock and arms a delayed drag.  The
            // actual floating host is created only after a small movement so a
            // normal click never accidentally tears the tab out of the area.
            active_index_ = index;
            tab_dragging_ = true;
            tab_drag_index_ = index;
            tab_drag_start_root_x_ = Fl::event_x_root();
            tab_drag_start_root_y_ = Fl::event_y_root();
            tab_drag_widget_ = dockAt(index);
            layoutDocks();
            redraw();
            return 1;
        }
    }

    if (event == FL_DRAG && tab_dragging_) {
        int root_x = Fl::event_x_root();
        int root_y = Fl::event_y_root();
        int dx = root_x - tab_drag_start_root_x_;
        int dy = root_y - tab_drag_start_root_y_;
        if (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold) {
            KDockWidget* drag_widget = tab_drag_widget_;
            KDockManager* drag_manager = drag_widget ? drag_widget->dockManager() : nullptr;
            const int local_x = Fl::event_x() - x();

            // Clear area-side drag state before reparenting.  beginFloatingDrag()
            // can make an empty dynamic area eligible for deletion, so no code
            // below relies on KDockArea members after the floating host appears.
            tab_dragging_ = false;
            tab_drag_index_ = -1;
            tab_drag_widget_ = nullptr;

            if (drag_widget) {
                drag_widget->dragging_ = true;
                drag_widget->drag_started_ = true;
                drag_widget->drag_start_root_x_ = tab_drag_start_root_x_;
                drag_widget->drag_start_root_y_ = tab_drag_start_root_y_;
                drag_widget->drag_offset_x_ = ClampSize(local_x, 8, std::max(8, drag_widget->w() - 8));
                drag_widget->drag_offset_y_ = kTitleHeight / 2;
                drag_widget->beginFloatingDrag(root_x, root_y);
                drag_widget->moveFloatingWindowForDrag(root_x, root_y);
                if (drag_manager) {
                    drag_manager->updateDockDrag(drag_widget, root_x, root_y);
                }
            }
            return 1;
        }
        return 1;
    }

    if (event == FL_RELEASE && tab_dragging_) {
        // Release without crossing the drag threshold is just a tab selection.
        tab_dragging_ = false;
        tab_drag_index_ = -1;
        tab_drag_widget_ = nullptr;
        return 1;
    }

    if (event == FL_MOVE) {
        int local_x = Fl::event_x() - x();
        int local_y = Fl::event_y() - y();
        int index = tabIndexAt(local_x, local_y);
        if (index != hover_index_) {
            hover_index_ = index;
            redraw();
        }
    }
    if (event == FL_LEAVE && hover_index_ != -1 && !tab_dragging_) {
        hover_index_ = -1;
        redraw();
    }
    return Fl_Group::handle(event);
}
void KDockArea::addDockWidget(KDockWidget* widget) {
    insertDockWidget(widget, dockCount());
}
void KDockArea::insertDockWidget(KDockWidget* widget, int index) {
    if (!widget) {
        return;
    }
    // Avoid duplicate tabs while still making the requested dock active.
    auto existing = std::find(docks_.begin(), docks_.end(), widget);
    if (existing == docks_.end()) {
        int safe_index = ClampSize(index, 0, dockCount());
        docks_.insert(docks_.begin() + safe_index, widget);
    }
    active_index_ = static_cast<int>(std::find(docks_.begin(), docks_.end(), widget) - docks_.begin());
    if (widget->isFloating()) {
        widget->detachFloatingWindow();
    }
    ReparentWidget(widget, this);
    widget->setDockArea(this);
    widget->show();
    layoutDocks();
    redraw();
}
void KDockArea::removeDockWidget(KDockWidget* widget) {
    if (!widget) {
        return;
    }
    auto it = std::find(docks_.begin(), docks_.end(), widget);
    if (it == docks_.end()) {
        return;
    }
    int removed_index = static_cast<int>(it - docks_.begin());
    docks_.erase(it);
    if (widget->parent() == this) {
        remove(widget);
    }
    widget->setDockArea(nullptr);
    widget->hide();
    if (docks_.empty()) {
        active_index_ = -1;
    }
    else if (active_index_ >= removed_index) {
        active_index_ = std::max(0, active_index_ - 1);
    }
    if (active_index_ >= dockCount()) {
        active_index_ = dockCount() - 1;
    }
    if (hover_index_ >= dockCount()) {
        hover_index_ = -1;
    }
    layoutDocks();
    redraw();
}
void KDockArea::setActiveDockWidget(KDockWidget* widget) {
    auto it = std::find(docks_.begin(), docks_.end(), widget);
    if (it == docks_.end()) {
        return;
    }
    active_index_ = static_cast<int>(it - docks_.begin());
    layoutDocks();
    redraw();
}
KDockWidget* KDockArea::activeDockWidget() const {
    if (active_index_ < 0 || active_index_ >= dockCount()) {
        return nullptr;
    }
    return docks_[active_index_];
}
int KDockArea::activeIndex() const {
    return active_index_;
}
void KDockArea::setActiveIndex(int index) {
    if (index < 0 || index >= dockCount()) {
        return;
    }
    active_index_ = index;
    layoutDocks();
    redraw();
}
int KDockArea::dockCount() const {
    return static_cast<int>(docks_.size());
}
KDockWidget* KDockArea::dockAt(int index) const {
    if (index < 0 || index >= dockCount()) {
        return nullptr;
    }
    return docks_[index];
}
int KDockArea::indexOfDockWidget(KDockWidget* widget) const {
    auto it = std::find(docks_.begin(), docks_.end(), widget);
    if (it == docks_.end()) {
        return -1;
    }
    return static_cast<int>(it - docks_.begin());
}
KDockAreaPosition KDockArea::position() const {
    return position_;
}
void KDockArea::setLayoutId(const std::string& layoutId) {
    layout_id_ = layoutId;
}
const std::string& KDockArea::layoutId() const {
    return layout_id_;
}
void KDockArea::setDefaultArea(bool defaultArea) {
    default_area_ = defaultArea;
}
bool KDockArea::isDefaultArea() const {
    return default_area_;
}
void KDockArea::layoutDocks() {
    if (layouting_docks_) {
        return;
    }
    ScopedLayoutFlag guard(layouting_docks_);
    int content_x = x() + 1;
    int content_y = y() + (dockCount() > 0 ? kTabHeight : 1);
    int content_w = std::max(kMinClientSize, w() - 2);
    int content_h = std::max(kMinClientSize, h() - (dockCount() > 0 ? kTabHeight : 1) - 1);
    for (int i = 0; i < dockCount(); ++i) {
        KDockWidget* dock = docks_[i];
        if (!dock) {
            continue;
        }
        // Geometry checks keep inactive tabs from receiving redundant resize
        // calls and reduce the chance of reentrant FLTK group layout.
        if (dock->x() != content_x || dock->y() != content_y ||
            dock->w() != content_w || dock->h() != content_h) {
            dock->resize(content_x, content_y, content_w, content_h);
        }
        dock->setDockArea(this);
        if (i == active_index_) {
            dock->show();
        }
        else {
            dock->hide();
        }
    }
}
void KDockArea::resize(int x, int y, int w, int h) {
    Fl_Group::resize(x, y, w, h);
    layoutDocks();
}
int KDockArea::tabIndexAt(int local_x, int local_y) const {
    if (local_y < 0 || local_y >= kTabHeight || local_x < 0 || local_x >= w()) {
        return -1;
    }
    int count = dockCount();
    if (count <= 0) {
        return -1;
    }
    int tab_w = std::max(40, w() / count);
    int index = local_x / tab_w;
    if (index >= count) {
        index = count - 1;
    }
    return index;
}
KDockManager::KDockManager(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label),
    left_(nullptr),
    right_(nullptr),
    top_(nullptr),
    bottom_(nullptr),
    center_(nullptr),
    root_node_(nullptr),
    all_areas_(),
    known_docks_(),
    active_drop_target_(),
    dragging_widget_(nullptr),
    next_area_id_(1),
    left_width_(220),
    right_width_(220),
    top_height_(150),
    bottom_height_(150),
    layouting_areas_(false) {
    box(FL_FLAT_BOX);
    color(kColorWindow());
    // The five default areas are persistent API targets.  They are hidden when
    // empty, while dynamic split areas can be deleted after their last dock goes
    // floating or closed.
    Fl_Group* previous_current = Fl_Group::current();
    begin();
    left_ = new KDockArea(0, 0, 1, 1, KDockAreaPosition::Left, "Left");
    right_ = new KDockArea(0, 0, 1, 1, KDockAreaPosition::Right, "Right");
    top_ = new KDockArea(0, 0, 1, 1, KDockAreaPosition::Top, "Top");
    bottom_ = new KDockArea(0, 0, 1, 1, KDockAreaPosition::Bottom, "Bottom");
    center_ = new KDockArea(0, 0, 1, 1, KDockAreaPosition::Center, "Center");
    end();
    Fl_Group::current(previous_current);
    KDockArea* defaults[] = { left_, right_, top_, bottom_, center_ };
    const char* ids[] = { "Left", "Right", "Top", "Bottom", "Center" };
    for (int i = 0; i < 5; ++i) {
        defaults[i]->setDefaultArea(true);
        defaults[i]->setLayoutId(ids[i]);
        defaults[i]->hide();
        registerArea(defaults[i]);
    }
    // Start with the center leaf so the manager always has a valid root target.
    root_node_ = new KDockLayoutNode(center_);
    resizable(nullptr);
    layoutAreas();
}
KDockManager::~KDockManager() {
    // Split nodes are bookkeeping only; FLTK owns and destroys child areas.
    deleteLayoutTree(root_node_);
    root_node_ = nullptr;
}
void KDockManager::draw() {
    if (HasOnlyChildDamage(this) && !active_drop_target_.valid) {
        // Dock child focus updates should not repaint the whole manager. Drag
        // overlays still force a full pass because guides are manager-level UI.
        draw_children();
        return;
    }

    // The manager owns the final overlay pass, so draw children before guides.
    fl_color(kColorWindow());
    fl_rectf(x(), y(), w(), h());
    fl_color(kColorBorder());
    fl_rect(x(), y(), w(), h());
    draw_children();
    drawDropOverlay();
}
void KDockManager::addDockWidget(KDockAreaPosition position, KDockWidget* widget) {
    if (!widget) {
        return;
    }
    // Public API drops target the stable main-window areas.  The tree is grown
    // lazily, so empty side areas do not consume space until first use.
    KDockArea* target = ensureMainAreaInTree(position);
    dockWidgetToArea(widget, target ? target : center_);
}
void KDockManager::tabifyDockWidget(KDockWidget* first, KDockWidget* second) {
    // Mirrors Qt's tabifyDockWidget: inputs are the existing anchor dock and
    // the dock being moved; processing reuses the anchor's tab area, and no
    // value is returned.
    if (!second) {
        return;
    }
    KDockArea* target = first ? first->dockArea() : nullptr;
    if (!target) {
        target = ensureMainAreaInTree(KDockAreaPosition::Center);
    }
    dockWidgetToArea(second, target ? target : center_);
}
void KDockManager::splitDockWidget(KDockWidget* first, KDockWidget* second, KDockDropSide side) {
    // Mirrors Qt's splitDockWidget at the dock-area level.  Center means tabify;
    // side drops create a dynamic area beside the anchor area.
    if (!second) {
        return;
    }
    if (side == KDockDropSide::Center) {
        tabifyDockWidget(first, second);
        return;
    }
    KDockArea* source = first ? first->dockArea() : nullptr;
    if (!source) {
        source = ensureMainAreaInTree(KDockAreaPosition::Center);
    }
    KDockArea* target = splitAreaForDrop(source, side);
    dockWidgetToArea(second, target ? target : source);
}
KDockArea* KDockManager::area(KDockAreaPosition position) const {
    switch (position) {
    case KDockAreaPosition::Left:
        return left_;
    case KDockAreaPosition::Right:
        return right_;
    case KDockAreaPosition::Top:
        return top_;
    case KDockAreaPosition::Bottom:
        return bottom_;
    case KDockAreaPosition::Center:
    default:
        return center_;
    }
}
void KDockManager::dockWidgetFromDrag(KDockWidget* widget, int root_x, int root_y) {
    if (!widget) {
        return;
    }
    Fl::grab(nullptr);
    KDockDropTarget target = dropTargetForFloatingWidget(widget, root_x, root_y);
    active_drop_target_ = target;
    // Invalid targets intentionally leave the dock in its floating host.
    if (!target.valid) {
        if (!widget->isFloating()) {
            widget->floatDock();
        }
        clearDockDrag();
        return;
    }
    KDockArea* target_area = nullptr;
    if (target.scope == KDockDropScope::DockWidget && target.area) {
        // Center merges as a tab in the hovered area; sides create a real split.
        target_area = (target.side == KDockDropSide::Center)
            ? target.area
            : splitAreaForDrop(target.area, target.side);
    }
    else {
        target_area = ensureMainAreaInTree(SideToPosition(target.side));
    }
    dockWidgetToArea(widget, target_area ? target_area : center_);
    clearDockDrag();
}
void KDockManager::updateDockDrag(KDockWidget* widget, int root_x, int root_y) {
    dragging_widget_ = widget;
    KDockDropTarget next_target = dropTargetForFloatingWidget(widget, root_x, root_y);
    if (!DropTargetsEqual(active_drop_target_, next_target)) {
        active_drop_target_ = next_target;
        redraw();
    }
}
void KDockManager::clearDockDrag() {
    const bool had_overlay = active_drop_target_.valid;
    active_drop_target_ = KDockDropTarget();
    dragging_widget_ = nullptr;
    if (had_overlay) {
        redraw();
    }
}
std::string KDockManager::saveLayout() const {
    std::ostringstream out;
    out << "KDockLayout 2\n";
    int next_node_id = 0;
    std::string root_id = WriteLayoutNode(root_node_, out, next_node_id);
    out << "ROOT|" << root_id << "\n";
    // Area lines preserve tab order by following with TAB lines, and also store
    // the active dock title so restore can reselect the right tab.
    for (KDockArea* dock_area : all_areas_) {
        if (!dock_area || (!areaIsInLayout(dock_area) && dock_area->dockCount() <= 0)) {
            continue;
        }
        KDockWidget* active = dock_area->activeDockWidget();
        out << "AREA|" << dock_area->layoutId() << "|" << PositionToToken(dock_area->position())
            << "|" << EscapeTitle(active ? active->title() : std::string()) << "\n";
        for (int i = 0; i < dock_area->dockCount(); ++i) {
            KDockWidget* dock = dock_area->dockAt(i);
            if (dock) {
                out << "TAB|" << dock_area->layoutId() << "|" << i
                    << "|" << EscapeTitle(dock->title()) << "\n";
            }
        }
    }
    // Floating docks are saved independently with their host geometry.
    for (KDockWidget* dock : known_docks_) {
        int fx = 0;
        int fy = 0;
        int fw = 0;
        int fh = 0;
        if (dock && dock->floatingGeometry(fx, fy, fw, fh)) {
            out << "FLOAT|" << fx << "|" << fy << "|" << fw << "|" << fh
                << "|" << EscapeTitle(dock->title()) << "\n";
        }
    }
    return out.str();
}
bool KDockManager::restoreLayout(const std::string& layoutText) {
    std::istringstream in(layoutText);
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    if (line == "KDockLayout 1") {
        return restoreLayoutV1(in);
    }
    if (line == "KDockLayout 2") {
        return restoreLayoutV2(in);
    }
    return false;
}
void KDockManager::layoutAreas() {
    if (layouting_areas_) {
        return;
    }
    ScopedLayoutFlag guard(layouting_areas_);
    if (!root_node_) {
        root_node_ = new KDockLayoutNode(center_);
    }
    // Hide leaves not present in the split tree so stale default areas cannot
    // draw or receive events after their last tab moved away.
    for (KDockArea* dock_area : all_areas_) {
        if (dock_area && !areaIsInLayout(dock_area)) {
            dock_area->hide();
            dock_area->resize(0, 0, 1, 1);
        }
    }
    layoutNode(root_node_, x(), y(), std::max(1, w()), std::max(1, h()));
}
void KDockManager::resize(int x, int y, int w, int h) {
    Fl_Group::resize(x, y, w, h);
    layoutAreas();
}
void KDockManager::rememberDock(KDockWidget* widget) {
    if (!widget) {
        return;
    }
    if (std::find(known_docks_.begin(), known_docks_.end(), widget) == known_docks_.end()) {
        known_docks_.push_back(widget);
    }
}
KDockWidget* KDockManager::findDockByTitle(const std::string& title) const {
    for (KDockWidget* dock : known_docks_) {
        if (dock && dock->title() == title) {
            return dock;
        }
    }
    return nullptr;
}
void KDockManager::removeDockFromAreas(KDockWidget* widget) {
    if (!widget) {
        return;
    }
    std::vector<KDockArea*> areas = all_areas_;
    for (KDockArea* dock_area : areas) {
        if (dock_area) {
            dock_area->removeDockWidget(widget);
        }
    }
}
void KDockManager::dockWidgetToArea(KDockWidget* widget, KDockArea* target) {
    if (!widget || !target) {
        return;
    }
    rememberDock(widget);
    widget->setDockManager(this);
    if (!areaIsInLayout(target)) {
        ensureMainAreaInTree(target->position());
    }
    if (!widget->isFloating() && target->indexOfDockWidget(widget) >= 0) {
        target->setActiveDockWidget(widget);
        layoutAreas();
        redraw();
        return;
    }
    removeDockFromAreas(widget);
    target->addDockWidget(widget);
    cleanupEmptyAreas();
    layoutAreas();
    redraw();
}
KDockArea* KDockManager::ensureMainAreaInTree(KDockAreaPosition position) {
    KDockArea* target = area(position);
    if (!target) {
        target = center_;
    }
    if (!root_node_) {
        root_node_ = new KDockLayoutNode(center_);
    }
    if (areaIsInLayout(target)) {
        return target;
    }
    KDockLayoutNode* old_root = root_node_;
    KDockLayoutNode* target_leaf = new KDockLayoutNode(target);
    KDockLayoutNode* split = nullptr;
    if (position == KDockAreaPosition::Left) {
        split = new KDockLayoutNode(KDockSplitOrientation::Horizontal, target_leaf, old_root, 250);
    }
    else if (position == KDockAreaPosition::Right) {
        split = new KDockLayoutNode(KDockSplitOrientation::Horizontal, old_root, target_leaf, 750);
    }
    else if (position == KDockAreaPosition::Top) {
        split = new KDockLayoutNode(KDockSplitOrientation::Vertical, target_leaf, old_root, 250);
    }
    else if (position == KDockAreaPosition::Bottom) {
        split = new KDockLayoutNode(KDockSplitOrientation::Vertical, old_root, target_leaf, 750);
    }
    else {
        split = new KDockLayoutNode(KDockSplitOrientation::Horizontal, old_root, target_leaf, 500);
    }
    root_node_ = split;
    root_node_->parent = nullptr;
    return target;
}
KDockArea* KDockManager::splitAreaForDrop(KDockArea* sourceArea, KDockDropSide side) {
    if (!sourceArea || side == KDockDropSide::None || side == KDockDropSide::Center) {
        return sourceArea;
    }
    KDockLayoutNode* source_leaf = FindLeafForArea(root_node_, sourceArea);
    if (!source_leaf) {
        return sourceArea;
    }
    KDockArea* target_area = createDynamicArea(sourceArea->position());
    KDockLayoutNode* target_leaf = new KDockLayoutNode(target_area);
    KDockLayoutNode* old_parent = source_leaf->parent;
    KDockSplitOrientation orientation = (side == KDockDropSide::Left || side == KDockDropSide::Right)
        ? KDockSplitOrientation::Horizontal
        : KDockSplitOrientation::Vertical;
    KDockLayoutNode* split = nullptr;
    if (side == KDockDropSide::Left || side == KDockDropSide::Top) {
        split = new KDockLayoutNode(orientation, target_leaf, source_leaf, 500);
    }
    else {
        split = new KDockLayoutNode(orientation, source_leaf, target_leaf, 500);
    }
    split->parent = old_parent;
    if (!old_parent) {
        root_node_ = split;
    }
    else if (old_parent->first == source_leaf) {
        old_parent->first = split;
    }
    else {
        old_parent->second = split;
    }
    return target_area;
}
KDockArea* KDockManager::createDynamicArea(KDockAreaPosition position) {
    std::string layout_id = "Dynamic" + std::to_string(next_area_id_++);
    return createAreaForRestore(layout_id, position);
}
KDockArea* KDockManager::createAreaForRestore(const std::string& layoutId, KDockAreaPosition position) {
    KDockArea* existing = areaByLayoutId(layoutId);
    if (existing) {
        int number = DynamicAreaNumber(layoutId);
        if (number >= next_area_id_) {
            next_area_id_ = number + 1;
        }
        return existing;
    }
    Fl_Group* previous_current = Fl_Group::current();
    begin();
    KDockArea* dock_area = new KDockArea(0, 0, 1, 1, position, nullptr);
    end();
    Fl_Group::current(previous_current);
    dock_area->setDefaultArea(false);
    dock_area->setLayoutId(layoutId);
    dock_area->copy_label(layoutId.c_str());
    dock_area->hide();
    registerArea(dock_area);
    int number = DynamicAreaNumber(layoutId);
    if (number >= next_area_id_) {
        next_area_id_ = number + 1;
    }
    return dock_area;
}
void KDockManager::registerArea(KDockArea* dockArea) {
    if (!dockArea) {
        return;
    }
    if (std::find(all_areas_.begin(), all_areas_.end(), dockArea) == all_areas_.end()) {
        all_areas_.push_back(dockArea);
    }
}
KDockArea* KDockManager::areaByLayoutId(const std::string& layoutId) const {
    for (KDockArea* dock_area : all_areas_) {
        if (dock_area && dock_area->layoutId() == layoutId) {
            return dock_area;
        }
    }
    return nullptr;
}
bool KDockManager::areaIsInLayout(KDockArea* dockArea) const {
    return LayoutContainsArea(root_node_, dockArea);
}
void KDockManager::cleanupEmptyAreas() {
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<KDockArea*> areas = all_areas_;
        for (KDockArea* dock_area : areas) {
            if (!dock_area || dock_area == center_ || dock_area->dockCount() > 0 || !areaIsInLayout(dock_area)) {
                continue;
            }
            KDockLayoutNode* leaf = FindLeafForArea(root_node_, dock_area);
            if (!leaf) {
                continue;
            }
            KDockLayoutNode* parent = leaf->parent;
            if (!parent) {
                delete leaf;
                root_node_ = new KDockLayoutNode(center_);
            }
            else {
                KDockLayoutNode* sibling = (parent->first == leaf) ? parent->second : parent->first;
                KDockLayoutNode* grand = parent->parent;
                if (sibling) {
                    sibling->parent = grand;
                }
                if (!grand) {
                    root_node_ = sibling;
                }
                else if (grand->first == parent) {
                    grand->first = sibling;
                }
                else {
                    grand->second = sibling;
                }
                parent->first = nullptr;
                parent->second = nullptr;
                delete leaf;
                delete parent;
            }
            dock_area->hide();
            dock_area->resize(0, 0, 1, 1);
            if (!dock_area->isDefaultArea()) {
                if (dock_area->parent() == this) {
                    remove(dock_area);
                }
                all_areas_.erase(std::remove(all_areas_.begin(), all_areas_.end(), dock_area), all_areas_.end());
                delete dock_area;
            }
            changed = true;
            break;
        }
    }
}
void KDockManager::deleteLayoutTree(KDockLayoutNode* node) {
    if (!node) {
        return;
    }
    deleteLayoutTree(node->first);
    deleteLayoutTree(node->second);
    delete node;
}
void KDockManager::deleteDynamicAreas() {
    std::vector<KDockArea*> keep;
    for (KDockArea* dock_area : all_areas_) {
        if (!dock_area) {
            continue;
        }
        if (dock_area->isDefaultArea()) {
            keep.push_back(dock_area);
            dock_area->hide();
            dock_area->resize(0, 0, 1, 1);
            continue;
        }
        if (dock_area->parent() == this) {
            remove(dock_area);
        }
        delete dock_area;
    }
    all_areas_ = keep;
}
void KDockManager::resetDocksForRestore() {
    // First remove every dock from every area so area vectors and widget parents
    // stay synchronized before any layout nodes are deleted.
    std::vector<KDockArea*> areas = all_areas_;
    for (KDockArea* dock_area : areas) {
        while (dock_area && dock_area->dockCount() > 0) {
            dock_area->removeDockWidget(dock_area->dockAt(0));
        }
    }
    for (KDockWidget* dock : known_docks_) {
        if (!dock) {
            continue;
        }
        if (dock->isFloating()) {
            dock->detachFloatingWindow();
        }
        if (dock->parent()) {
            dock->parent()->remove(dock);
        }
        dock->setDockArea(nullptr);
        dock->setDockManager(this);
        dock->hide();
    }
    deleteLayoutTree(root_node_);
    root_node_ = nullptr;
    deleteDynamicAreas();
    root_node_ = new KDockLayoutNode(center_);
}
bool KDockManager::restoreLayoutV1(std::istream& input) {
    std::string line;
    bool restored_any = false;
    resetDocksForRestore();
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::size_t separator = line.find('|');
        if (separator == std::string::npos) {
            continue;
        }
        KDockAreaPosition position = KDockAreaPosition::Center;
        if (!TokenToPosition(line.substr(0, separator), position)) {
            continue;
        }
        KDockWidget* dock = findDockByTitle(UnescapeTitle(line.substr(separator + 1)));
        if (!dock) {
            continue;
        }
        addDockWidget(position, dock);
        restored_any = true;
    }
    cleanupEmptyAreas();
    layoutAreas();
    redraw();
    return restored_any;
}
bool KDockManager::restoreLayoutV2(std::istream& input) {
    struct NodeSpec {
        bool leaf;
        std::string area_id;
        KDockSplitOrientation orientation;
        int ratio;
        std::string first;
        std::string second;
    };
    struct TabSpec {
        std::string area_id;
        int index;
        std::string title;
    };
    struct FloatSpec {
        int x;
        int y;
        int w;
        int h;
        std::string title;
    };
    std::string line;
    std::string root_id;
    std::map<std::string, NodeSpec> nodes;
    std::map<std::string, KDockAreaPosition> area_positions;
    std::map<std::string, std::string> active_titles;
    std::vector<TabSpec> tabs;
    std::vector<FloatSpec> floats;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.compare(0, 5, "NODE|") == 0) {
            std::vector<std::string> fields = SplitPlainFields(line);
            if (fields.size() >= 4 && fields[2] == "LEAF") {
                NodeSpec spec;
                spec.leaf = true;
                spec.area_id = fields[3];
                spec.orientation = KDockSplitOrientation::Horizontal;
                spec.ratio = 500;
                nodes[fields[1]] = spec;
            }
            else if (fields.size() >= 7 && fields[2] == "SPLIT") {
                KDockSplitOrientation orientation = KDockSplitOrientation::Horizontal;
                if (!TokenToOrientation(fields[3], orientation)) {
                    continue;
                }
                NodeSpec spec;
                spec.leaf = false;
                spec.orientation = orientation;
                spec.ratio = ClampSize(std::atoi(fields[4].c_str()), 100, 900);
                spec.first = fields[5];
                spec.second = fields[6];
                nodes[fields[1]] = spec;
            }
            continue;
        }
        if (line.compare(0, 5, "ROOT|") == 0) {
            root_id = line.substr(5);
            continue;
        }
        if (line.compare(0, 5, "AREA|") == 0) {
            std::size_t p1 = line.find('|');
            std::size_t p2 = line.find('|', p1 + 1);
            std::size_t p3 = line.find('|', p2 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
                continue;
            }
            std::string area_id = line.substr(p1 + 1, p2 - p1 - 1);
            KDockAreaPosition position = KDockAreaPosition::Center;
            TokenToPosition(line.substr(p2 + 1, p3 - p2 - 1), position);
            area_positions[area_id] = position;
            active_titles[area_id] = UnescapeTitle(line.substr(p3 + 1));
            continue;
        }
        if (line.compare(0, 4, "TAB|") == 0) {
            std::size_t p1 = line.find('|');
            std::size_t p2 = line.find('|', p1 + 1);
            std::size_t p3 = line.find('|', p2 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
                continue;
            }
            TabSpec spec;
            spec.area_id = line.substr(p1 + 1, p2 - p1 - 1);
            spec.index = std::atoi(line.substr(p2 + 1, p3 - p2 - 1).c_str());
            spec.title = UnescapeTitle(line.substr(p3 + 1));
            tabs.push_back(spec);
            continue;
        }
        if (line.compare(0, 6, "FLOAT|") == 0) {
            std::size_t p1 = line.find('|');
            std::size_t p2 = line.find('|', p1 + 1);
            std::size_t p3 = line.find('|', p2 + 1);
            std::size_t p4 = line.find('|', p3 + 1);
            std::size_t p5 = line.find('|', p4 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos ||
                p4 == std::string::npos || p5 == std::string::npos) {
                continue;
            }
            FloatSpec spec;
            spec.x = std::atoi(line.substr(p1 + 1, p2 - p1 - 1).c_str());
            spec.y = std::atoi(line.substr(p2 + 1, p3 - p2 - 1).c_str());
            spec.w = std::atoi(line.substr(p3 + 1, p4 - p3 - 1).c_str());
            spec.h = std::atoi(line.substr(p4 + 1, p5 - p4 - 1).c_str());
            spec.title = UnescapeTitle(line.substr(p5 + 1));
            floats.push_back(spec);
        }
    }
    if (root_id.empty() || nodes.find(root_id) == nodes.end()) {
        return false;
    }
    resetDocksForRestore();
    deleteLayoutTree(root_node_);
    root_node_ = nullptr;
    std::map<std::string, bool> building;
    std::function<KDockLayoutNode*(const std::string&)> build_node = [&](const std::string& node_id) -> KDockLayoutNode* {
        auto spec_it = nodes.find(node_id);
        if (spec_it == nodes.end() || building[node_id]) {
            return nullptr;
        }
        building[node_id] = true;
        const NodeSpec& spec = spec_it->second;
        KDockLayoutNode* node = nullptr;
        if (spec.leaf) {
            KDockAreaPosition position = KDockAreaPosition::Center;
            auto pos_it = area_positions.find(spec.area_id);
            if (pos_it != area_positions.end()) {
                position = pos_it->second;
            }
            KDockArea* dock_area = createAreaForRestore(spec.area_id, position);
            node = new KDockLayoutNode(dock_area);
        }
        else {
            KDockLayoutNode* first = build_node(spec.first);
            KDockLayoutNode* second = build_node(spec.second);
            if (first && second) {
                node = new KDockLayoutNode(spec.orientation, first, second, spec.ratio);
            }
            else {
                deleteLayoutTree(first);
                deleteLayoutTree(second);
            }
        }
        building[node_id] = false;
        return node;
    };
    root_node_ = build_node(root_id);
    if (!root_node_) {
        root_node_ = new KDockLayoutNode(center_);
    }
    root_node_->parent = nullptr;
    std::sort(tabs.begin(), tabs.end(), [](const TabSpec& a, const TabSpec& b) {
        if (a.area_id == b.area_id) {
            return a.index < b.index;
        }
        return a.area_id < b.area_id;
    });
    bool restored_any = false;
    for (const TabSpec& spec : tabs) {
        KDockArea* dock_area = areaByLayoutId(spec.area_id);
        KDockWidget* dock = findDockByTitle(spec.title);
        if (!dock_area || !dock) {
            continue;
        }
        rememberDock(dock);
        dock->setDockManager(this);
        dock_area->addDockWidget(dock);
        restored_any = true;
    }
    for (const auto& item : active_titles) {
        if (item.second.empty()) {
            continue;
        }
        KDockArea* dock_area = areaByLayoutId(item.first);
        KDockWidget* active = findDockByTitle(item.second);
        if (dock_area && active && dock_area->indexOfDockWidget(active) >= 0) {
            dock_area->setActiveDockWidget(active);
        }
    }
    for (const FloatSpec& spec : floats) {
        KDockWidget* dock = findDockByTitle(spec.title);
        if (!dock) {
            continue;
        }
        rememberDock(dock);
        dock->setDockManager(this);
        removeDockFromAreas(dock);
        dock->createFloatingWindowAt(spec.x, spec.y, spec.w, spec.h);
        restored_any = true;
    }
    cleanupEmptyAreas();
    layoutAreas();
    redraw();
    return restored_any;
}
KDockAreaPosition KDockManager::dropPositionForRootPoint(int root_x, int root_y) const {
    int root_box_x = 0;
    int root_box_y = 0;
    int root_box_w = 0;
    int root_box_h = 0;
    WidgetRootBox(this, root_box_x, root_box_y, root_box_w, root_box_h);
    int local_x = root_x - root_box_x;
    int local_y = root_y - root_box_y;
    return SideToPosition(SideForPointInBox(local_x, local_y, root_box_w, root_box_h));
}
KDockDropTarget KDockManager::dropTargetForRootPoint(KDockWidget* widget, int root_x, int root_y) const {
    KDockDropTarget target;
    int manager_root_x = 0;
    int manager_root_y = 0;
    int manager_w = 0;
    int manager_h = 0;
    WidgetRootBox(this, manager_root_x, manager_root_y, manager_w, manager_h);
    // Outside the manager is intentionally invalid so the dock remains floating.
    if (root_x < manager_root_x || root_x >= manager_root_x + manager_w ||
        root_y < manager_root_y || root_y >= manager_root_y + manager_h) {
        return target;
    }
    for (KDockArea* dock_area : all_areas_) {
        if (!dock_area || !dock_area->visible() || dock_area->dockCount() <= 0) {
            continue;
        }
        KDockWidget* dock = dock_area->activeDockWidget();
        if (!dock || dock == widget || !dock->visible() || dock->isFloating()) {
            continue;
        }
        int dock_root_x = 0;
        int dock_root_y = 0;
        int dock_w = 0;
        int dock_h = 0;
        WidgetRootBox(dock, dock_root_x, dock_root_y, dock_w, dock_h);
        if (root_x < dock_root_x || root_x >= dock_root_x + dock_w ||
            root_y < dock_root_y || root_y >= dock_root_y + dock_h) {
            continue;
        }
        target.valid = true;
        target.scope = KDockDropScope::DockWidget;
        target.side = SideForPointInBox(root_x - dock_root_x, root_y - dock_root_y, dock_w, dock_h);
        target.area = dock_area;
        target.dock = dock;
        target.position = dock_area->position();
        return target;
    }
    target.valid = true;
    target.scope = KDockDropScope::MainWindow;
    target.side = SideForPointInBox(root_x - manager_root_x, root_y - manager_root_y, manager_w, manager_h);
    target.position = SideToPosition(target.side);
    target.area = area(target.position);
    return target;
}
KDockDropTarget KDockManager::dropTargetForFloatingWidget(KDockWidget* widget, int root_x, int root_y) const {
    // First use the real cursor point so precise guide selection still works.
    // If the cursor is outside the dock manager, fall back to the floating host
    // center. That matches user intent when the window body is visibly over the
    // dock surface but the mouse is over the floating title bar or outside the
    // small manager rectangle. The return value is the best available target.
    KDockDropTarget target = dropTargetForRootPoint(widget, root_x, root_y);
    if (target.valid || !widget || !widget->isFloating()) {
        return target;
    }
    int center_x = 0;
    int center_y = 0;
    if (!widget->floatingCenterRootPoint(center_x, center_y)) {
        return target;
    }
    return dropTargetForRootPoint(widget, center_x, center_y);
}
KDockAreaPosition KDockManager::positionForDropTarget(const KDockDropTarget& target) const {
    if (!target.valid) {
        return KDockAreaPosition::Center;
    }
    return SideToPosition(target.side);
}
void KDockManager::drawDropOverlay() {
    if (!active_drop_target_.valid) {
        return;
    }
    // Main-window guides remain visible for context; dock-local drops also draw
    // a second guide set over the hovered dock to clarify split/tab scope.
    KDockDropSide main_side = active_drop_target_.scope == KDockDropScope::MainWindow
        ? active_drop_target_.side
        : KDockDropSide::None;
    DrawDropGuideSet(x(), y(), w(), h(), main_side);
    if (active_drop_target_.scope == KDockDropScope::MainWindow) {
        DrawDropPreview(x(), y(), w(), h(), active_drop_target_.side);
        return;
    }
    if (active_drop_target_.scope != KDockDropScope::DockWidget || !active_drop_target_.dock) {
        return;
    }
    int window_root_x = 0;
    int window_root_y = 0;
    if (top_window()) {
        window_root_x = top_window()->x_root();
        window_root_y = top_window()->y_root();
    }
    int dock_root_x = 0;
    int dock_root_y = 0;
    int dock_w = 0;
    int dock_h = 0;
    WidgetRootBox(active_drop_target_.dock, dock_root_x, dock_root_y, dock_w, dock_h);
    int dock_x = dock_root_x - window_root_x;
    int dock_y = dock_root_y - window_root_y;
    DrawDropPreview(dock_x, dock_y, dock_w, dock_h, active_drop_target_.side);
    DrawDropGuideSet(dock_x, dock_y, dock_w, dock_h, active_drop_target_.side);
}
void KDockManager::layoutNode(KDockLayoutNode* node, int node_x, int node_y, int node_w, int node_h) {
    if (!node) {
        return;
    }
    if (node->is_leaf) {
        KDockArea* dock_area = node->area;
        if (!dock_area) {
            return;
        }
        bool should_show = (dock_area == center_) || dock_area->dockCount() > 0;
        if (dock_area->x() != node_x || dock_area->y() != node_y ||
            dock_area->w() != node_w || dock_area->h() != node_h) {
            dock_area->resize(node_x, node_y, std::max(1, node_w), std::max(1, node_h));
        }
        should_show ? dock_area->show() : dock_area->hide();
        dock_area->layoutDocks();
        return;
    }
    int ratio = ClampSize(node->first_ratio, 100, 900);
    if (node->orientation == KDockSplitOrientation::Horizontal) {
        int first_w = (node_w <= kMinClientSize * 2)
            ? std::max(1, node_w / 2)
            : ClampSize(node_w * ratio / 1000, kMinClientSize, node_w - kMinClientSize);
        int second_w = std::max(1, node_w - first_w);
        layoutNode(node->first, node_x, node_y, first_w, node_h);
        layoutNode(node->second, node_x + first_w, node_y, second_w, node_h);
    }
    else {
        int first_h = (node_h <= kMinClientSize * 2)
            ? std::max(1, node_h / 2)
            : ClampSize(node_h * ratio / 1000, kMinClientSize, node_h - kMinClientSize);
        int second_h = std::max(1, node_h - first_h);
        layoutNode(node->first, node_x, node_y, node_w, first_h);
        layoutNode(node->second, node_x, node_y + first_h, node_w, second_h);
    }
}
KDockManager* KCreateDockManager(int x, int y, int w, int h) {
    return new KDockManager(x, y, w, h);
}
KDockWidget* KCreateDockWidget(const char* title, Fl_Widget* content) {
    // Some FLTK group-derived content widgets may still be the current group
    // after construction. Creating the dock while that is true can make the
    // dock a child of its own future content, so remember a safe caller group.
    Fl_Group* previous_current = Fl_Group::current();
    Fl_Group* restore_current = previous_current;
    if (content && IsAncestor(content, previous_current)) {
        restore_current = content->parent();
    }
    Fl_Group::current(restore_current);
    int content_w = content ? std::max(260, content->w() + kMargin * 2) : 320;
    int content_h = content ? std::max(180, content->h() + kTitleHeight + kMargin * 2) : 240;
    KDockWidget* dock = new KDockWidget(0, 0, content_w, content_h, title, content);
    // Restore the caller's construction context after the composite dock closes
    // its own group. Never restore to a group now contained by the new dock.
    if (restore_current == dock || IsAncestor(dock, restore_current)) {
        restore_current = dock->parent();
    }
    Fl_Group::current(restore_current);
    return dock;
}
