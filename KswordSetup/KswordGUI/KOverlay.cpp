#include "KOverlay.h"

#include "fl_draw.H"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kFont = 12;
constexpr int kTitleFont = 14;

// DrawText paints clipped text using the shared flat typography; returns no value.
void DrawText(const std::string& text, int x, int y, int w, int h, Fl_Color color, Fl_Align align, int size = kFont) {
    if (text.empty() || w <= 0 || h <= 0) return;
    fl_color(color); fl_font(FL_HELVETICA, size); fl_draw(text.c_str(), x, y, w, h, align | FL_ALIGN_INSIDE);
}

// DrawBorder paints the square one-pixel border used by overlay shells; returns no value.
void DrawBorder(int x, int y, int w, int h, Fl_Color color) { fl_color(color); fl_rect(x, y, w, h); }

// SemanticColor maps toast kind to current theme colors; output is an FLTK color.
Fl_Color SemanticColor(KToastKind kind, Fl_Color info) {
    const KTheme& t = KThemeManager::instance().theme();
    if (kind == KToastKind::Success) return t.success;
    if (kind == KToastKind::Warning) return t.warning;
    if (kind == KToastKind::Danger) return t.danger;
    return info == FL_BACKGROUND_COLOR ? t.primary : info;
}

// ClampIndex returns a valid row index for non-empty item lists, otherwise -1.
int ClampIndex(int index, std::size_t count) { return count == 0 ? -1 : std::max(0, std::min(index, static_cast<int>(count) - 1)); }

// ResolveAccent returns current theme primary unless caller supplied an explicit accent color.
Fl_Color ResolveAccent(Fl_Color color) { return color == FL_BACKGROUND_COLOR ? KThemeManager::instance().theme().primary : color; }

// NextEnabledMenuRow walks selectable context-menu rows while skipping separators and disabled items.
int NextEnabledMenuRow(const std::vector<KContextMenuItem>& items, int start, int delta) {
    if (items.empty()) return -1;
    int index = ClampIndex(start, items.size());
    for (int count = 0; count < static_cast<int>(items.size()); ++count) { index = (index + delta + static_cast<int>(items.size())) % static_cast<int>(items.size()); if (items[index].enabled && !items[index].separator) return index; }
    return -1;
}
}

KToast::KToast(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : ""), text_(label ? label : ""), kind_(KToastKind::Info), duration_(3.0), accent_color_(FL_BACKGROUND_COLOR) {
    // Toasts are borderless self-painted windows, so title-bar and Dock code are not touched.
    border(0); box(FL_FLAT_BOX); end();
}

void KToast::setText(const char* text) { text_ = text ? text : ""; label(text_.c_str()); redraw(); }
void KToast::setKind(KToastKind kind) { kind_ = kind; redraw(); }
void KToast::setDuration(double seconds) { duration_ = std::max(0.0, seconds); redraw(); }
void KToast::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }
void KToast::showAt(int root_x, int root_y, const char* text) { if (text) setText(text); position(root_x, root_y); show(); }

void KToast::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const Fl_Color accent = SemanticColor(kind_, accent_color_);
    fl_push_clip(0, 0, w(), h()); fl_color(t.panelBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    fl_color(accent); fl_rectf(0, 0, 4, h()); DrawText(text_, 14, 0, w() - 22, h(), t.text, FL_ALIGN_LEFT); fl_pop_clip();
}

KTooltip::KTooltip(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : ""), text_(label ? label : "") {
    // Tooltips are paint-only popups with no children.
    border(0); box(FL_FLAT_BOX); end();
}

void KTooltip::setText(const char* text) { text_ = text ? text : ""; label(text_.c_str()); redraw(); }
void KTooltip::showAt(int root_x, int root_y) { position(root_x, root_y); show(); }

void KTooltip::draw() {
    const KTheme& t = KThemeManager::instance().theme();
    fl_push_clip(0, 0, w(), h()); fl_color(t.controlAltBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    DrawText(text_, 8, 0, w() - 16, h(), t.text, FL_ALIGN_CENTER); fl_pop_clip();
}

KPopover::KPopover(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : ""), title_(label ? label : ""), content_() {
    // Popovers provide a themed content bubble without assuming child controls.
    border(0); box(FL_FLAT_BOX); end();
}

void KPopover::setTitle(const char* text) { title_ = text ? text : ""; label(title_.c_str()); redraw(); }
void KPopover::setContent(const char* text) { content_ = text ? text : ""; redraw(); }
void KPopover::showAt(int root_x, int root_y) { position(root_x, root_y); show(); }

void KPopover::draw() {
    const KTheme& t = KThemeManager::instance().theme();
    fl_push_clip(0, 0, w(), h()); fl_color(t.panelBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    DrawText(title_, 12, 8, w() - 24, 22, t.text, FL_ALIGN_LEFT, kTitleFont); DrawText(content_, 12, 34, w() - 24, h() - 42, t.mutedText, FL_ALIGN_LEFT | FL_ALIGN_TOP); fl_pop_clip();
}

KModalDialog::KModalDialog(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : "Dialog"), title_(label ? label : "Dialog"), message_(), primary_text_("OK"), secondary_text_("Cancel"), result_(-1) {
    // The dialog is modal and self-drawn; it avoids the custom title-bar subsystem by design.
    set_modal(); border(0); box(FL_FLAT_BOX); end();
}

void KModalDialog::setTitle(const char* text) { title_ = text ? text : ""; label(title_.c_str()); redraw(); }
void KModalDialog::setMessage(const char* text) { message_ = text ? text : ""; redraw(); }
void KModalDialog::setPrimaryText(const char* text) { primary_text_ = text ? text : ""; redraw(); }
void KModalDialog::setSecondaryText(const char* text) { secondary_text_ = text ? text : ""; redraw(); }
int KModalDialog::runModal() { result_ = -1; show(); while (shown()) Fl::wait(); return result_; }
int KModalDialog::result() const { return result_; }

void KModalDialog::draw() {
    const KTheme& t = KThemeManager::instance().theme(); const int by = h() - 48;
    fl_push_clip(0, 0, w(), h()); fl_color(t.panelBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    DrawText(title_, 16, 12, w() - 32, 24, t.text, FL_ALIGN_LEFT, kTitleFont); DrawText(message_, 16, 46, w() - 32, by - 54, t.mutedText, FL_ALIGN_LEFT | FL_ALIGN_TOP);
    fl_color(t.primary); fl_rectf(w() - 104, by, 84, 30); DrawBorder(w() - 104, by, 84, 30, t.primaryDark); DrawText(primary_text_, w() - 104, by, 84, 30, FL_WHITE, FL_ALIGN_CENTER);
    fl_color(t.controlBg); fl_rectf(w() - 198, by, 84, 30); DrawBorder(w() - 198, by, 84, 30, t.border); DrawText(secondary_text_, w() - 198, by, 84, 30, t.text, FL_ALIGN_CENTER); fl_pop_clip();
}

int KModalDialog::handle(int event) {
    if (event == FL_KEYDOWN && Fl::event_key() == FL_Escape) { result_ = -1; hide(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        const int by = h() - 48, ex = Fl::event_x(), ey = Fl::event_y();
        if (ey >= by && ey < by + 30 && ex >= w() - 104 && ex < w() - 20) { result_ = 1; do_callback(); hide(); return 1; }
        if (ey >= by && ey < by + 30 && ex >= w() - 198 && ex < w() - 114) { result_ = 0; do_callback(); hide(); return 1; }
    }
    return Fl_Window::handle(event);
}

KDrawer::KDrawer(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : "Drawer"), title_(label ? label : "Drawer"), content_(), side_(KDrawerSide::Right) {
    // Drawers are self-painted side panels and can host caller children if added manually later.
    border(0); box(FL_FLAT_BOX); end();
}

void KDrawer::setTitle(const char* text) { title_ = text ? text : ""; label(title_.c_str()); redraw(); }
void KDrawer::setContent(const char* text) { content_ = text ? text : ""; redraw(); }
void KDrawer::setSide(KDrawerSide side) { side_ = side; redraw(); }
void KDrawer::showDrawer() { show(); }
void KDrawer::hideDrawer() { hide(); }

void KDrawer::draw() {
    const KTheme& t = KThemeManager::instance().theme(); (void)side_;
    fl_push_clip(0, 0, w(), h()); fl_color(t.panelBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    DrawText(title_, 14, 12, w() - 28, 26, t.text, FL_ALIGN_LEFT, kTitleFont); fl_color(t.border); fl_line(0, 48, w(), 48);
    DrawText(content_, 14, 60, w() - 28, h() - 74, t.mutedText, FL_ALIGN_LEFT | FL_ALIGN_TOP); fl_pop_clip(); draw_children();
}

KOverlayMask::KOverlayMask(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), visible_(false), opacity_(0.45) {
    // The mask does not hide the FLTK widget itself; visible_ controls painting only.
    box(FL_FLAT_BOX);
}

void KOverlayMask::setVisible(bool visible) { visible_ = visible; redraw(); }
bool KOverlayMask::visibleMask() const { return visible_; }
void KOverlayMask::setOpacity(double opacity) { opacity_ = std::max(0.0, std::min(1.0, opacity)); redraw(); }

void KOverlayMask::draw() {
    if (!visible_) return; const KTheme& t = KThemeManager::instance().theme(); (void)opacity_;
    fl_push_clip(x(), y(), w(), h()); fl_color(t.pressed); fl_rectf(x(), y(), w(), h()); fl_pop_clip();
}


KModalMask::KModalMask(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), visible_(false), opacity_(0.55), message_(label ? label : ""), accent_color_(FL_BACKGROUND_COLOR) {
    // ModalMask is paint-only and blocks interaction by consuming events while visible_.
    box(FL_FLAT_BOX);
}

void KModalMask::setVisible(bool visible) { visible_ = visible; redraw(); }
bool KModalMask::visibleMask() const { return visible_; }
void KModalMask::setOpacity(double opacity) { opacity_ = std::max(0.0, std::min(1.0, opacity)); redraw(); }
void KModalMask::setMessage(const char* text) { message_ = text ? text : ""; redraw(); }
void KModalMask::setAccentColor(Fl_Color color) { accent_color_ = color; redraw(); }

void KModalMask::draw() {
    if (!visible_) return; const KTheme& t = KThemeManager::instance().theme(); const Fl_Color accent = ResolveAccent(accent_color_); (void)opacity_;
    fl_push_clip(x(), y(), w(), h()); fl_color(t.pressed); fl_rectf(x(), y(), w(), h());
    if (!message_.empty()) { const int card_w = std::min(300, std::max(160, w() - 60)); const int card_h = 72; const int card_x = x() + (w() - card_w) / 2; const int card_y = y() + (h() - card_h) / 2;
        fl_color(t.panelBg); fl_rectf(card_x, card_y, card_w, card_h); DrawBorder(card_x, card_y, card_w, card_h, t.border); fl_color(accent); fl_rectf(card_x, card_y, 4, card_h); DrawText(message_, card_x + 16, card_y, card_w - 28, card_h, t.text, FL_ALIGN_CENTER, kTitleFont); }
    fl_pop_clip();
}

int KModalMask::handle(int event) {
    if (!visible_) return Fl_Widget::handle(event);
    if (event == FL_PUSH || event == FL_RELEASE || event == FL_DRAG || event == FL_MOVE || event == FL_KEYDOWN || event == FL_SHORTCUT) return 1;
    return Fl_Widget::handle(event);
}

KLoadingOverlay::KLoadingOverlay(int x, int y, int w, int h, const char* label)
    : Fl_Widget(x, y, w, h, label), loading_(false), message_(label ? label : "Loading"), progress_(-1.0), step_(0) {
    // Loading overlays are paint-only; callers can animate by changing step_.
    box(FL_FLAT_BOX);
}

void KLoadingOverlay::setLoading(bool loading) { loading_ = loading; redraw(); }
bool KLoadingOverlay::loading() const { return loading_; }
void KLoadingOverlay::setMessage(const char* text) { message_ = text ? text : ""; redraw(); }
void KLoadingOverlay::setProgress(double progress) { progress_ = progress < 0.0 ? -1.0 : std::max(0.0, std::min(1.0, progress)); redraw(); }
void KLoadingOverlay::setStep(int step) { step_ = step; redraw(); }

void KLoadingOverlay::draw() {
    if (!loading_) return; const KTheme& t = KThemeManager::instance().theme(); const int cx = x() + w() / 2, cy = y() + h() / 2 - 12;
    fl_push_clip(x(), y(), w(), h()); fl_color(t.pressed); fl_rectf(x(), y(), w(), h());
    fl_color(t.panelBg); fl_rectf(cx - 82, cy - 44, 164, progress_ >= 0.0 ? 104 : 82); DrawBorder(cx - 82, cy - 44, 164, progress_ >= 0.0 ? 104 : 82, t.border);
    for (int i = 0; i < 8; ++i) { fl_color(i == (step_ % 8) ? t.primary : t.border); const double a = (i * 3.14159265358979323846) / 4.0; fl_line(cx, cy, cx + static_cast<int>(std::cos(a) * 18), cy + static_cast<int>(std::sin(a) * 18)); }
    DrawText(message_, cx - 70, cy + 26, 140, 20, t.text, FL_ALIGN_CENTER);
    if (progress_ >= 0.0) { fl_color(t.controlBg); fl_rectf(cx - 62, cy + 54, 124, 8); fl_color(t.primary); fl_rectf(cx - 62, cy + 54, static_cast<int>(124 * progress_), 8); DrawBorder(cx - 62, cy + 54, 124, 8, t.border); }
    fl_pop_clip();
}

KContextMenu::KContextMenu(int x, int y, int w, int h, const char* label)
    : Fl_Window(x, y, w, h, label ? label : ""), items_(), active_index_(-1), selected_index_(-1), row_height_(28) {
    // Context menus are modal popups with keyboard and mouse selection.
    border(0); box(FL_FLAT_BOX); end();
}

void KContextMenu::setItems(const std::vector<KContextMenuItem>& items) { items_ = items; active_index_ = ClampIndex(active_index_, items_.size()); selected_index_ = -1; redraw(); }
void KContextMenu::addItem(const KContextMenuItem& item) { items_.push_back(item); if (active_index_ < 0 && !item.separator && item.enabled) active_index_ = static_cast<int>(items_.size()) - 1; redraw(); }
void KContextMenu::clear() { items_.clear(); active_index_ = -1; selected_index_ = -1; redraw(); }
void KContextMenu::setActiveIndex(int index) { active_index_ = ClampIndex(index, items_.size()); redraw(); }
int KContextMenu::selectedIndex() const { return selected_index_; }

int KContextMenu::popup(int root_x, int root_y) {
    selected_index_ = -1; const int menu_h = std::max(row_height_, static_cast<int>(items_.size()) * row_height_ + 2);
    resize(root_x, root_y, w(), menu_h); set_modal(); show(); Fl::grab(this); while (shown()) Fl::wait(); Fl::grab(nullptr); return selected_index_;
}

void KContextMenu::draw() {
    const KTheme& t = KThemeManager::instance().theme(); fl_push_clip(0, 0, w(), h()); fl_color(t.panelBg); fl_rectf(0, 0, w(), h()); DrawBorder(0, 0, w(), h(), t.border);
    int cy = 1; for (int i = 0; i < static_cast<int>(items_.size()); ++i, cy += row_height_) {
        const KContextMenuItem& item = items_[i]; if (item.separator) { fl_color(t.border); fl_line(8, cy + row_height_ / 2, w() - 8, cy + row_height_ / 2); continue; }
        fl_color(i == active_index_ && item.enabled ? t.hover : t.panelBg); fl_rectf(1, cy, w() - 2, row_height_);
        DrawText(item.checked ? "*" : "", 8, cy, 14, row_height_, t.primary, FL_ALIGN_CENTER);
        DrawText(item.text, 26, cy, w() - 106, row_height_, item.enabled ? t.text : t.mutedText, FL_ALIGN_LEFT);
        DrawText(item.shortcut, w() - 76, cy, 66, row_height_, t.mutedText, FL_ALIGN_RIGHT);
    }
    fl_pop_clip();
}

int KContextMenu::handle(int event) {
    if (event == FL_KEYDOWN) {
        if (Fl::event_key() == FL_Escape) { hide(); return 1; }
        if (Fl::event_key() == FL_Up) { active_index_ = NextEnabledMenuRow(items_, active_index_, -1); redraw(); return 1; }
        if (Fl::event_key() == FL_Down) { active_index_ = NextEnabledMenuRow(items_, active_index_, 1); redraw(); return 1; }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) { if (active_index_ >= 0 && active_index_ < static_cast<int>(items_.size()) && items_[active_index_].enabled && !items_[active_index_].separator) { selected_index_ = active_index_; do_callback(); hide(); } return 1; }
    }
    if (event == FL_MOVE) { const int row = ClampIndex((Fl::event_y() - 1) / row_height_, items_.size()); active_index_ = (row >= 0 && items_[row].enabled && !items_[row].separator) ? row : -1; redraw(); return 1; }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        const int row = (Fl::event_y() - 1) / row_height_;
        if (row >= 0 && row < static_cast<int>(items_.size()) && items_[row].enabled && !items_[row].separator) { selected_index_ = row; do_callback(); hide(); return 1; }
        hide(); return 1;
    }
    return Fl_Window::handle(event);
}

KModalMask* KCreateModalMask(int x, int y, int w, int h, const char* label) { return new KModalMask(x, y, w, h, label); }
