#include "KNavigation.h"

#include "fl_draw.H"

#include <algorithm>
#include <cctype>
#include <functional>

namespace {
constexpr int kFont = 12;
constexpr int kTitleFont = 14;

// DrawText clips and paints text with FLTK fonts; input text is copied by caller-owned strings and no value is returned.
void DrawText(const std::string& text, int x, int y, int w, int h, Fl_Color color, Fl_Align align, int size = kFont) {
    if (text.empty() || w <= 0 || h <= 0) return;
    fl_color(color); fl_font(FL_HELVETICA, size); fl_draw(text.c_str(), x, y, w, h, align | FL_ALIGN_INSIDE);
}

// DrawBorder paints the square one-pixel frame used by all navigation widgets; returns no value.
void DrawBorder(int x, int y, int w, int h, Fl_Color color) { fl_color(color); fl_rect(x, y, w, h); }

// ClampIndex returns a valid index for non-empty collections, otherwise -1.
int ClampIndex(int index, std::size_t count) { return count == 0 ? -1 : std::max(0, std::min(index, static_cast<int>(count) - 1)); }

// ResolveAccent returns the current theme primary unless callers supplied an explicit accent color.
Fl_Color ResolveAccent(Fl_Color color) { return color == FL_BACKGROUND_COLOR ? KThemeManager::instance().theme().primary : color; }

// LowerCopy normalizes command text for simple case-insensitive filtering.
std::string LowerCopy(const std::string& text) {
    std::string out = text;
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// CommandMatches returns true when query appears in title, subtitle, or shortcut.
bool CommandMatches(const KCommand& command, const std::string& query) {
    if (query.empty()) return true;
    return LowerCopy(command.title + " " + command.subtitle + " " + command.shortcut).find(query) != std::string::npos;
}

// TreeNodeSelfMatches checks the searchable fields for one tree row.
bool TreeNodeSelfMatches(const KTreeNode& node, const std::string& query) {
    if (query.empty()) return true;
    return LowerCopy(node.text + " " + node.icon + " " + node.badge).find(query) != std::string::npos;
}

// TreeNodeBranchMatches checks whether a tree row or any descendant matches a filter query.
bool TreeNodeBranchMatches(const KTreeNode& node, const std::string& query) {
    if (TreeNodeSelfMatches(node, query)) return true;
    for (const KTreeNode& child : node.children) if (TreeNodeBranchMatches(child, query)) return true;
    return false;
}

// SetExpandedRecursive applies one expansion state to every node below the supplied vector.
void SetExpandedRecursive(std::vector<KTreeNode>& nodes, bool expanded) {
    for (KTreeNode& node : nodes) { node.expanded = expanded; SetExpandedRecursive(node.children, expanded); }
}
}

KBreadcrumb::KBreadcrumb(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), items_(), active_index_(-1), hover_index_(-1), accent_color_(FL_BACKGROUND_COLOR) {
    // Store only visual state; all colors are pulled from KThemeManager during draw().
    box(FL_FLAT_BOX); labelfont(FL_HELVETICA); labelsize(kFont);
}

void KBreadcrumb::setItems(const std::vector<std::string>& items) { items_ = items; active_index_ = ClampIndex(active_index_, items_.size()); hover_index_ = -1; redraw(); }
void KBreadcrumb::addItem(const char* text) { items_.push_back(text ? text : ""); if (active_index_ < 0) active_index_ = 0; redraw(); }
void KBreadcrumb::clear() { items_.clear(); active_index_ = -1; hover_index_ = -1; redraw(); }
void KBreadcrumb::setActiveIndex(int index) { active_index_ = ClampIndex(index, items_.size()); redraw(); }
int KBreadcrumb::activeIndex() const { return active_index_; }
void KBreadcrumb::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }
Fl_Color KBreadcrumb::accentColor() const { return ResolveAccent(accent_color_); }

int KBreadcrumb::itemAt(int mouse_x, int mouse_y) const {
    if (mouse_y < y() || mouse_y >= y() + h()) return -1;
    fl_font(FL_HELVETICA, kFont); int cursor = x() + 8;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const int item_w = static_cast<int>(fl_width(items_[i].c_str())) + 18;
        if (mouse_x >= cursor && mouse_x < cursor + item_w) return i;
        cursor += item_w + 14;
    }
    return -1;
}

void KBreadcrumb::draw() {
    const KTheme& t = KThemeManager::instance().theme();
    const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    fl_font(FL_HELVETICA, kFont); int cursor = x() + 8;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const int item_w = static_cast<int>(fl_width(items_[i].c_str())) + 18;
        const bool active = i == active_index_, hover = i == hover_index_;
        fl_color(active ? t.selection : (hover ? t.hover : t.panelBg)); fl_rectf(cursor, y() + 4, item_w, h() - 8);
        DrawText(items_[i], cursor + 8, y(), item_w - 16, h(), active ? accent : t.text, FL_ALIGN_CENTER);
        if (i + 1 < static_cast<int>(items_.size())) DrawText(">", cursor + item_w + 2, y(), 10, h(), t.mutedText, FL_ALIGN_CENTER);
        cursor += item_w + 14;
    }
    fl_pop_clip();
}

int KBreadcrumb::handle(int event) {
    if (event == FL_MOVE || event == FL_ENTER) { hover_index_ = itemAt(Fl::event_x(), Fl::event_y()); redraw(); return 1; }
    if (event == FL_LEAVE) { hover_index_ = -1; redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        const int index = itemAt(Fl::event_x(), Fl::event_y());
        if (index >= 0) { active_index_ = index; do_callback(); redraw(); return 1; }
    }
    return Fl_Widget::handle(event);
}

KSideNav::KSideNav(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), items_(), active_index_(-1), hover_index_(-1), item_height_(34), accent_color_(FL_BACKGROUND_COLOR) {
    // The side nav owns row data only; close FLTK child capture to avoid accidental parenting.
    box(FL_FLAT_BOX); end();
}

void KSideNav::setItems(const std::vector<KNavItem>& items) { items_ = items; active_index_ = ClampIndex(active_index_, items_.size()); redraw(); }
void KSideNav::addItem(const char* text) { KNavItem item; item.text = text ? text : ""; items_.push_back(item); if (active_index_ < 0) active_index_ = 0; redraw(); }
void KSideNav::clear() { items_.clear(); active_index_ = -1; hover_index_ = -1; redraw(); }
void KSideNav::setActiveIndex(int index) { active_index_ = (index >= 0 && index < static_cast<int>(items_.size()) && items_[index].enabled) ? index : (index < 0 ? -1 : active_index_); redraw(); }
int KSideNav::activeIndex() const { return active_index_; }
void KSideNav::setItemHeight(int height) { item_height_ = std::max(20, height); redraw(); }
void KSideNav::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KSideNav::draw() {
    const KTheme& t = KThemeManager::instance().theme(); int cy = y() + 6;
    const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    if (label() && label()[0]) { DrawText(label(), x() + 10, cy, w() - 20, 18, t.mutedText, FL_ALIGN_LEFT); cy += 24; }
    for (int i = 0; i < static_cast<int>(items_.size()); ++i, cy += item_height_) {
        const bool active = i == active_index_, hover = i == hover_index_ && items_[i].enabled;
        fl_color(active ? t.selection : (hover ? t.hover : t.panelBg)); fl_rectf(x() + 4, cy, w() - 8, item_height_ - 2);
        if (active) { fl_color(accent); fl_rectf(x() + 4, cy, 3, item_height_ - 2); }
        DrawText(items_[i].text, x() + 14, cy, w() - 58, item_height_ - 2, items_[i].enabled ? (active ? accent : t.text) : t.mutedText, FL_ALIGN_LEFT);
        DrawText(items_[i].badge, x() + w() - 48, cy, 36, item_height_ - 2, t.mutedText, FL_ALIGN_RIGHT);
    }
    fl_pop_clip();
}

int KSideNav::handle(int event) {
    const int offset = (label() && label()[0]) ? 30 : 6; const int index = (Fl::event_y() - y() - offset) / item_height_;
    const bool valid = index >= 0 && index < static_cast<int>(items_.size());
    if (event == FL_MOVE || event == FL_ENTER) { hover_index_ = valid ? index : -1; redraw(); return 1; }
    if (event == FL_LEAVE) { hover_index_ = -1; redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && valid && items_[index].enabled) { active_index_ = index; do_callback(); redraw(); return 1; }
    return Fl_Group::handle(event);
}

KTopNav::KTopNav(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), items_(), active_index_(-1), hover_index_(-1), accent_color_(FL_BACKGROUND_COLOR) {
    // Top nav is a self-painted strip with measured row boxes.
    box(FL_FLAT_BOX); end();
}

void KTopNav::setItems(const std::vector<KNavItem>& items) { items_ = items; active_index_ = ClampIndex(active_index_, items_.size()); redraw(); }
void KTopNav::addItem(const char* text) { KNavItem item; item.text = text ? text : ""; items_.push_back(item); if (active_index_ < 0) active_index_ = 0; redraw(); }
void KTopNav::clear() { items_.clear(); active_index_ = -1; hover_index_ = -1; redraw(); }
void KTopNav::setActiveIndex(int index) { active_index_ = (index >= 0 && index < static_cast<int>(items_.size()) && items_[index].enabled) ? index : (index < 0 ? -1 : active_index_); redraw(); }
int KTopNav::activeIndex() const { return active_index_; }
void KTopNav::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

int KTopNav::itemAt(int mouse_x, int mouse_y) const {
    if (mouse_y < y() || mouse_y >= y() + h()) return -1;
    fl_font(FL_HELVETICA, kFont); int cursor = x() + 8;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) { const int iw = static_cast<int>(fl_width(items_[i].text.c_str())) + 34; if (mouse_x >= cursor && mouse_x < cursor + iw) return i; cursor += iw; }
    return -1;
}

void KTopNav::draw() {
    const KTheme& t = KThemeManager::instance().theme(); fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    const Fl_Color accent = ResolveAccent(accent_color_);
    fl_font(FL_HELVETICA, kFont); int cursor = x() + 8;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const int iw = static_cast<int>(fl_width(items_[i].text.c_str())) + 34; const bool active = i == active_index_, hover = i == hover_index_ && items_[i].enabled;
        fl_color(active ? t.selection : (hover ? t.hover : t.panelBg)); fl_rectf(cursor, y() + 3, iw, h() - 6);
        DrawText(items_[i].text, cursor + 12, y(), iw - 24, h(), items_[i].enabled ? (active ? accent : t.text) : t.mutedText, FL_ALIGN_CENTER);
        if (active) { fl_color(accent); fl_rectf(cursor + 8, y() + h() - 4, iw - 16, 2); }
        cursor += iw;
    }
    fl_pop_clip();
}

int KTopNav::handle(int event) {
    if (event == FL_MOVE || event == FL_ENTER) { hover_index_ = itemAt(Fl::event_x(), Fl::event_y()); redraw(); return 1; }
    if (event == FL_LEAVE) { hover_index_ = -1; redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) { const int index = itemAt(Fl::event_x(), Fl::event_y()); if (index >= 0 && items_[index].enabled) { active_index_ = index; do_callback(); redraw(); return 1; } }
    return Fl_Group::handle(event);
}

struct KTreeView::VisibleRow { const KTreeNode* node = nullptr; std::vector<int> path; int depth = 0; };

KTreeView::KTreeView(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), roots_(), selected_path_(), hover_path_(), item_height_(24), accent_color_(FL_BACKGROUND_COLOR), show_root_lines_(true), filter_text_() {
    // Tree view is a leaf group so callers can create widgets after it safely.
    box(FL_FLAT_BOX); end();
}

void KTreeView::setItems(const std::vector<KTreeNode>& items) { roots_ = items; if (!nodeAtPath(selected_path_)) selected_path_.clear(); redraw(); }
void KTreeView::clear() { roots_.clear(); selected_path_.clear(); redraw(); }
void KTreeView::setActiveIndex(int index) { const auto rows = visibleRows(); selected_path_ = (index >= 0 && index < static_cast<int>(rows.size())) ? rows[index].path : std::vector<int>(); redraw(); }
int KTreeView::activeIndex() const { const auto rows = visibleRows(); for (int i = 0; i < static_cast<int>(rows.size()); ++i) if (rows[i].path == selected_path_) return i; return -1; }
std::string KTreeView::selectedText() const { const KTreeNode* n = nodeAtPath(selected_path_); return n ? n->text : std::string(); }
void KTreeView::setItemHeight(int height) { item_height_ = std::max(18, height); redraw(); }
void KTreeView::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }
void KTreeView::expandAll() { SetExpandedRecursive(roots_, true); redraw(); }
void KTreeView::collapseAll() { SetExpandedRecursive(roots_, false); redraw(); }
void KTreeView::setFilterText(const char* text) { filter_text_ = text ? text : ""; selected_path_.clear(); hover_path_.clear(); redraw(); }
const std::string& KTreeView::filterText() const { return filter_text_; }
void KTreeView::setShowRootLines(bool show) { show_root_lines_ = show; redraw(); }
bool KTreeView::showRootLines() const { return show_root_lines_; }
std::vector<int> KTreeView::selectedPath() const { return selected_path_; }
void KTreeView::setSelectedPath(const std::vector<int>& path) { selected_path_ = nodeAtPath(path) ? path : std::vector<int>(); redraw(); }

std::vector<KTreeView::VisibleRow> KTreeView::visibleRows() const {
    std::vector<VisibleRow> rows; const std::string query = LowerCopy(filter_text_);
    // Recursion copies only row metadata while node storage stays owned by roots_; filters force matching branches open visually.
    std::function<void(const std::vector<KTreeNode>&, std::vector<int>, int)> collect;
    collect = [&](const std::vector<KTreeNode>& nodes, std::vector<int> path, int depth) -> void {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            if (!TreeNodeBranchMatches(nodes[i], query)) continue;
            std::vector<int> p = path; p.push_back(i); VisibleRow row; row.node = &nodes[i]; row.path = p; row.depth = depth; rows.push_back(row);
            if ((nodes[i].expanded || !query.empty()) && !nodes[i].children.empty()) collect(nodes[i].children, p, depth + 1);
        }
    };
    collect(roots_, std::vector<int>(), 0); return rows;
}

KTreeNode* KTreeView::nodeAtPath(const std::vector<int>& path) { std::vector<KTreeNode>* level = &roots_; KTreeNode* node = nullptr; for (int index : path) { if (index < 0 || index >= static_cast<int>(level->size())) return nullptr; node = &(*level)[index]; level = &node->children; } return node; }
const KTreeNode* KTreeView::nodeAtPath(const std::vector<int>& path) const { const std::vector<KTreeNode>* level = &roots_; const KTreeNode* node = nullptr; for (int index : path) { if (index < 0 || index >= static_cast<int>(level->size())) return nullptr; node = &(*level)[index]; level = &node->children; } return node; }

void KTreeView::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const auto rows = visibleRows(); int cy = y() + 4;
    const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.controlBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    if (rows.empty()) { DrawText(filter_text_.empty() ? "No nodes" : "No matching nodes", x(), y(), w(), h(), t.mutedText, FL_ALIGN_CENTER); fl_pop_clip(); return; }
    for (const VisibleRow& row : rows) {
        if (cy >= y() + h()) break; const bool selected = row.path == selected_path_; const bool hover = row.path == hover_path_; const int indent = row.depth * 16; const bool has_children = row.node && !row.node->children.empty();
        fl_color(selected ? t.selection : (hover && row.node && row.node->enabled ? t.hover : t.controlBg)); fl_rectf(x() + 1, cy, w() - 2, item_height_);
        if (show_root_lines_ && row.depth > 0) { fl_color(t.border); const int guide_x = x() + 15 + (row.depth - 1) * 16; fl_line(guide_x, cy, guide_x, cy + item_height_); fl_line(guide_x, cy + item_height_ / 2, guide_x + 12, cy + item_height_ / 2); }
        DrawText(has_children ? (row.node->expanded || !filter_text_.empty() ? "-" : "+") : "", x() + 8 + indent, cy, 14, item_height_, accent, FL_ALIGN_CENTER);
        const int icon_w = row.node && !row.node->icon.empty() ? 18 : 0; DrawText(row.node ? row.node->icon : std::string(), x() + 26 + indent, cy, icon_w, item_height_, accent, FL_ALIGN_CENTER);
        DrawText(row.node ? row.node->text : std::string(), x() + 28 + indent + icon_w, cy, w() - 76 - indent - icon_w, item_height_, row.node && row.node->enabled ? t.text : t.mutedText, FL_ALIGN_LEFT);
        DrawText(row.node ? row.node->badge : std::string(), x() + w() - 46, cy, 36, item_height_, t.mutedText, FL_ALIGN_RIGHT); cy += item_height_;
    }
    fl_pop_clip();
}

int KTreeView::handle(int event) {
    const int index = (Fl::event_y() - y() - 4) / item_height_; const auto rows = visibleRows(); const bool valid = index >= 0 && index < static_cast<int>(rows.size());
    if (event == FL_MOVE || event == FL_ENTER) { hover_path_ = valid ? rows[index].path : std::vector<int>(); redraw(); return 1; }
    if (event == FL_LEAVE) { hover_path_.clear(); redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && valid) {
        KTreeNode* node = nodeAtPath(rows[index].path); if (!node || !node->enabled) return 1;
        if (filter_text_.empty() && !node->children.empty() && Fl::event_x() < x() + 24 + rows[index].depth * 16) node->expanded = !node->expanded; else { selected_path_ = rows[index].path; do_callback(); }
        redraw(); return 1;
    }
    return Fl_Group::handle(event);
}

KCommandPalette::KCommandPalette(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : "Command Palette"), commands_(), query_(), placeholder_("Type a command"), active_index_(0), selected_command_(-1) {
    // Palette is modal and self-painted; no title bar or child controls are required.
    border(0); set_modal(); box(FL_FLAT_BOX); end();
}

void KCommandPalette::setItems(const std::vector<KCommand>& commands) { commands_ = commands; active_index_ = 0; selected_command_ = -1; redraw(); }
void KCommandPalette::setQuery(const char* query) { query_ = query ? query : ""; active_index_ = 0; redraw(); }
const std::string& KCommandPalette::query() const { return query_; }
void KCommandPalette::setPlaceholder(const char* text) { placeholder_ = text ? text : ""; redraw(); }
void KCommandPalette::setActiveIndex(int index) { active_index_ = ClampIndex(index, filteredIndexes().size()); redraw(); }
int KCommandPalette::activeIndex() const { return active_index_; }
int KCommandPalette::runModal() { selected_command_ = -1; show(); while (shown()) Fl::wait(); return selected_command_; }
int KCommandPalette::selectedCommand() const { return selected_command_; }

std::vector<int> KCommandPalette::filteredIndexes() const {
    std::vector<int> indexes; const std::string query = LowerCopy(query_);
    for (int i = 0; i < static_cast<int>(commands_.size()); ++i) if (CommandMatches(commands_[i], query)) indexes.push_back(i);
    return indexes;
}

void KCommandPalette::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const std::vector<int> visible = filteredIndexes(); active_index_ = ClampIndex(active_index_, visible.size());
    fl_push_clip(0, 0, w(), h()); fl_color(t.panelBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    fl_color(t.controlBg); fl_rectf(12, 12, w() - 24, 34); DrawBorder(12, 12, w() - 24, 34, t.border);
    DrawText(query_.empty() ? placeholder_ : query_, 22, 12, w() - 44, 34, query_.empty() ? t.mutedText : t.text, FL_ALIGN_LEFT);
    int cy = 56; if (visible.empty()) DrawText("No commands", 12, cy, w() - 24, 42, t.mutedText, FL_ALIGN_CENTER);
    for (int row = 0; row < static_cast<int>(visible.size()) && cy + 42 <= h() - 8; ++row, cy += 42) {
        const KCommand& c = commands_[visible[row]]; fl_color(row == active_index_ ? t.selection : t.panelBg); fl_rectf(12, cy, w() - 24, 40);
        DrawText(c.title, 22, cy + 4, w() - 132, 18, c.enabled ? t.text : t.mutedText, FL_ALIGN_LEFT, kTitleFont);
        DrawText(c.subtitle, 22, cy + 22, w() - 132, 16, t.mutedText, FL_ALIGN_LEFT); DrawText(c.shortcut, w() - 112, cy, 90, 40, t.mutedText, FL_ALIGN_RIGHT);
    }
    fl_pop_clip();
}

int KCommandPalette::handle(int event) {
    if (event == FL_KEYDOWN) {
        const std::vector<int> visible = filteredIndexes();
        if (Fl::event_key() == FL_Escape) { hide(); return 1; }
        if (Fl::event_key() == FL_Up) { setActiveIndex(active_index_ - 1); return 1; }
        if (Fl::event_key() == FL_Down) { setActiveIndex(active_index_ + 1); return 1; }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) { if (active_index_ >= 0 && active_index_ < static_cast<int>(visible.size()) && commands_[visible[active_index_]].enabled) { selected_command_ = visible[active_index_]; do_callback(); hide(); } return 1; }
        if (Fl::event_key() == FL_BackSpace) { if (!query_.empty()) { query_.pop_back(); active_index_ = 0; redraw(); } return 1; }
        const char* text = Fl::event_text(); if (text && Fl::event_length() > 0 && static_cast<unsigned char>(text[0]) >= 32) { query_.append(text, static_cast<std::size_t>(Fl::event_length())); active_index_ = 0; redraw(); return 1; }
    }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) { const int row = (Fl::event_y() - 56) / 42; const std::vector<int> visible = filteredIndexes(); if (row >= 0 && row < static_cast<int>(visible.size()) && commands_[visible[row]].enabled) { active_index_ = row; selected_command_ = visible[row]; do_callback(); hide(); return 1; } }
    return Fl_Window::handle(event);
}

KSectionHeader::KSectionHeader(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), text_(label ? label : ""), subtitle_(), action_text_(), accent_color_(FL_BACKGROUND_COLOR) {
    // Header is a paint-only title block except for optional action hit testing.
    box(FL_FLAT_BOX);
}

void KSectionHeader::setText(const char* text) { text_ = text ? text : ""; redraw(); }
const std::string& KSectionHeader::text() const { return text_; }
void KSectionHeader::setSubtitle(const char* text) { subtitle_ = text ? text : ""; redraw(); }
void KSectionHeader::setActionText(const char* text) { action_text_ = text ? text : ""; redraw(); }
void KSectionHeader::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KSectionHeader::draw() {
    const KTheme& t = KThemeManager::instance().theme(); fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h());
    const Fl_Color accent = ResolveAccent(accent_color_);
    fl_color(accent); fl_rectf(x(), y() + 9, 3, std::max(10, h() - 18));
    DrawText(text_, x() + 12, y() + 4, w() - 130, 20, t.text, FL_ALIGN_LEFT, kTitleFont); DrawText(subtitle_, x() + 12, y() + 24, w() - 130, 18, t.mutedText, FL_ALIGN_LEFT);
    DrawText(action_text_, x() + w() - 110, y(), 98, h(), accent, FL_ALIGN_RIGHT); fl_color(t.border); fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1); fl_pop_clip();
}

int KSectionHeader::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && !action_text_.empty() && Fl::event_x() >= x() + w() - 120) { do_callback(); return 1; }
    return Fl_Widget::handle(event);
}


KCardGrid::KCardGrid(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), items_(), active_index_(-1), hover_index_(-1), columns_(3), gap_(10), accent_color_(FL_BACKGROUND_COLOR) {
    // CardGrid is self-painted and does not participate in lower-level layout algorithms.
    box(FL_FLAT_BOX); end();
}

void KCardGrid::setItems(const std::vector<KCardGridItem>& items) { items_ = items; active_index_ = ClampIndex(active_index_, items_.size()); hover_index_ = -1; redraw(); }
void KCardGrid::addItem(const KCardGridItem& item) { items_.push_back(item); if (active_index_ < 0) active_index_ = 0; redraw(); }
void KCardGrid::clear() { items_.clear(); active_index_ = -1; hover_index_ = -1; redraw(); }
void KCardGrid::setActiveIndex(int index) { active_index_ = ClampIndex(index, items_.size()); redraw(); }
int KCardGrid::activeIndex() const { return active_index_; }
void KCardGrid::setColumns(int columns) { columns_ = std::max(1, columns); redraw(); }
void KCardGrid::setGap(int gap) { gap_ = std::max(0, gap); redraw(); }
void KCardGrid::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

int KCardGrid::cardAt(int mouse_x, int mouse_y) const {
    if (mouse_x < x() || mouse_x >= x() + w() || mouse_y < y() || mouse_y >= y() + h() || items_.empty()) return -1;
    const int cols = std::max(1, columns_); const int card_w = std::max(1, (w() - gap_ * (cols + 1)) / cols); const int card_h = 78;
    const int rel_x = mouse_x - x() - gap_, rel_y = mouse_y - y() - gap_; if (rel_x < 0 || rel_y < 0) return -1;
    const int col = rel_x / (card_w + gap_), row = rel_y / (card_h + gap_); if (col < 0 || col >= cols || rel_x % (card_w + gap_) >= card_w || rel_y % (card_h + gap_) >= card_h) return -1;
    const int index = row * cols + col; return index >= 0 && index < static_cast<int>(items_.size()) ? index : -1;
}

void KCardGrid::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    if (items_.empty()) { DrawText("No cards", x(), y(), w(), h(), t.mutedText, FL_ALIGN_CENTER); fl_pop_clip(); return; }
    const int cols = std::max(1, columns_); const int card_w = std::max(1, (w() - gap_ * (cols + 1)) / cols); const int card_h = 78;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) { const int row = i / cols, col = i % cols; const int cx = x() + gap_ + col * (card_w + gap_), cy = y() + gap_ + row * (card_h + gap_); if (cy >= y() + h()) break;
        const bool active = i == active_index_, hover = i == hover_index_ && items_[i].enabled; fl_color(active ? t.selection : (hover ? t.hover : t.controlBg)); fl_rectf(cx, cy, card_w, card_h); DrawBorder(cx, cy, card_w, card_h, active ? accent : t.border);
        if (active) { fl_color(accent); fl_rectf(cx, cy, 4, card_h); } DrawText(items_[i].title, cx + 12, cy + 10, card_w - 24, 20, items_[i].enabled ? t.text : t.mutedText, FL_ALIGN_LEFT, kTitleFont);
        DrawText(items_[i].subtitle, cx + 12, cy + 32, card_w - 24, 18, t.mutedText, FL_ALIGN_LEFT); DrawText(items_[i].meta, cx + 12, cy + card_h - 26, card_w - 24, 18, t.mutedText, FL_ALIGN_RIGHT); }
    fl_pop_clip();
}

int KCardGrid::handle(int event) {
    if (event == FL_MOVE || event == FL_ENTER) { hover_index_ = cardAt(Fl::event_x(), Fl::event_y()); redraw(); return 1; }
    if (event == FL_LEAVE) { hover_index_ = -1; redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) { const int index = cardAt(Fl::event_x(), Fl::event_y()); if (index >= 0 && items_[index].enabled) { active_index_ = index; do_callback(); redraw(); return 1; } }
    return Fl_Group::handle(event);
}

KSection::KSection(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), title_(label ? label : ""), subtitle_(), action_text_(), header_height_(54), accent_color_(FL_BACKGROUND_COLOR) {
    // Section hosts caller children below a painted header without imposing layout rules.
    box(FL_FLAT_BOX); end();
}

void KSection::setTitle(const char* text) { title_ = text ? text : ""; redraw(); }
void KSection::setSubtitle(const char* text) { subtitle_ = text ? text : ""; redraw(); }
void KSection::setActionText(const char* text) { action_text_ = text ? text : ""; redraw(); }
void KSection::setHeaderHeight(int height) { header_height_ = std::max(32, height); redraw(); }
void KSection::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }
int KSection::contentY() const { return y() + header_height_; }

void KSection::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const Fl_Color accent = ResolveAccent(accent_color_);
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border); fl_color(accent); fl_rectf(x(), y(), 4, header_height_);
    DrawText(title_, x() + 14, y() + 8, w() - 126, 20, t.text, FL_ALIGN_LEFT, kTitleFont); DrawText(subtitle_, x() + 14, y() + 30, w() - 126, 18, t.mutedText, FL_ALIGN_LEFT); DrawText(action_text_, x() + w() - 108, y(), 96, header_height_, accent, FL_ALIGN_RIGHT);
    fl_color(t.border); fl_line(x(), y() + header_height_, x() + w(), y() + header_height_); fl_pop_clip(); draw_children();
}

int KSection::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && !action_text_.empty() && Fl::event_y() < y() + header_height_ && Fl::event_x() >= x() + w() - 120) { do_callback(); return 1; }
    return Fl_Group::handle(event);
}

KInspectorPanel::KInspectorPanel(int x, int y, int w, int h, const char* label)
    : Fl_Group(x, y, w, h, label), sections_(), row_height_(26), header_height_(30), accent_color_(FL_BACKGROUND_COLOR) {
    // InspectorPanel is read-only by default; editable intent is indicated per row for host integration.
    box(FL_FLAT_BOX); end();
}

void KInspectorPanel::setSections(const std::vector<KInspectorSection>& sections) { sections_ = sections; redraw(); }
void KInspectorPanel::addSection(const KInspectorSection& section) { sections_.push_back(section); redraw(); }
void KInspectorPanel::clear() { sections_.clear(); redraw(); }
void KInspectorPanel::setSectionExpanded(int index, bool expanded) { if (index >= 0 && index < static_cast<int>(sections_.size())) { sections_[index].expanded = expanded; redraw(); } }
void KInspectorPanel::setRowHeight(int height) { row_height_ = std::max(20, height); redraw(); }
void KInspectorPanel::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

int KInspectorPanel::sectionAt(int mouse_y) const {
    int cy = y() + 1; for (int i = 0; i < static_cast<int>(sections_.size()); ++i) { if (mouse_y >= cy && mouse_y < cy + header_height_) return i; cy += header_height_; if (sections_[i].expanded) cy += static_cast<int>(sections_[i].fields.size()) * row_height_; }
    return -1;
}

void KInspectorPanel::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const Fl_Color accent = ResolveAccent(accent_color_); int cy = y() + 1;
    fl_push_clip(x(), y(), w(), h()); fl_color(t.panelBg); fl_rectf(x(), y(), w(), h()); DrawBorder(x(), y(), w(), h(), t.border);
    if (sections_.empty()) { DrawText("No inspector data", x(), y(), w(), h(), t.mutedText, FL_ALIGN_CENTER); fl_pop_clip(); return; }
    for (int i = 0; i < static_cast<int>(sections_.size()) && cy < y() + h(); ++i) { const KInspectorSection& section = sections_[i]; fl_color(t.controlAltBg); fl_rectf(x() + 1, cy, w() - 2, header_height_); DrawText(section.expanded ? "-" : "+", x() + 8, cy, 16, header_height_, accent, FL_ALIGN_CENTER); DrawText(section.title, x() + 30, cy, w() - 40, header_height_, t.text, FL_ALIGN_LEFT, kTitleFont); cy += header_height_;
        if (!section.expanded) continue; for (int r = 0; r < static_cast<int>(section.fields.size()) && cy < y() + h(); ++r, cy += row_height_) { const KInspectorField& field = section.fields[r]; fl_color(r % 2 ? t.controlAltBg : t.controlBg); fl_rectf(x() + 1, cy, w() - 2, row_height_); DrawText(field.name, x() + 12, cy, w() / 3, row_height_, t.mutedText, FL_ALIGN_LEFT); DrawText(field.value, x() + w() / 3 + 16, cy, w() * 2 / 3 - 28, row_height_, field.read_only ? t.text : accent, FL_ALIGN_LEFT); DrawText(field.hint, x() + w() - 72, cy, 60, row_height_, t.mutedText, FL_ALIGN_RIGHT); } }
    fl_pop_clip();
}

int KInspectorPanel::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) { const int index = sectionAt(Fl::event_y()); if (index >= 0) { sections_[index].expanded = !sections_[index].expanded; do_callback(); redraw(); return 1; } }
    return Fl_Group::handle(event);
}

KCardGrid* KCreateCardGrid(int x, int y, int w, int h, const char* label) { return new KCardGrid(x, y, w, h, label); }
KSection* KCreateSection(int x, int y, int w, int h, const char* label) { return new KSection(x, y, w, h, label); }
KInspectorPanel* KCreateInspectorPanel(int x, int y, int w, int h, const char* label) { return new KInspectorPanel(x, y, w, h, label); }
