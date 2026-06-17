#include "KWidgets.h"
#include "KTitleBar.h"

#include "Fl_Window.H"
#include "fl_draw.H"

#include <algorithm>
#include <cstdint>

namespace {
constexpr int kPopupFontSize = 12;

// CloseMessageCallback hides the modal message window passed in user data.
void CloseMessageCallback(Fl_Widget*, void* data) {
    Fl_Window* win = static_cast<Fl_Window*>(data);
    if (win) {
        win->hide();
    }
}

// PopupMenuState carries the selected index out of the modal popup loop.
struct PopupMenuState {
    int selected = -1;
    Fl_Window* window = nullptr;
};

// PopupMenuWindow closes when the user clicks outside its bounds.
class PopupMenuWindow : public Fl_Window {
public:
    // Creates a borderless popup at root-screen coordinates.
    explicit PopupMenuWindow(int x, int y, int w, int h)
        : Fl_Window(x, y, w, h) {
    }

    // Handles outside-click dismissal and delegates other events to Fl_Window.
    int handle(int event) override {
        if (event == FL_PUSH) {
            const int ex = Fl::event_x_root();
            const int ey = Fl::event_y_root();
            if (ex < x() || ex >= x() + w() || ey < y() || ey >= y() + h()) {
                hide();
                return 1;
            }
        }
        return Fl_Window::handle(event);
    }
};

// PopupMenuItemCallback stores the clicked item index and closes the popup.
void PopupMenuItemCallback(Fl_Widget* widget, void* data) {
    PopupMenuState* state = static_cast<PopupMenuState*>(data);
    if (!state || !state->window) {
        return;
    }
    state->selected = static_cast<int>(reinterpret_cast<intptr_t>(widget->user_data()));
    state->window->hide();
}
}

KButton* KCreateButton(int x, int y, int w, int h, const char* label, KButtonType type) {
    return new KButton(x, y, w, h, label, type);
}

KInput* KCreateInput(int x, int y, int w, int h, const char* label) {
    return new KInput(x, y, w, h, label);
}

KCheckBox* KCreateCheckBox(int x, int y, int w, int h, const char* label) {
    return new KCheckBox(x, y, w, h, label);
}

KSlider* KCreateSlider(int x, int y, int w, int h) {
    return new KSlider(x, y, w, h);
}

KText* KCreateText(int x, int y, int w, int h, const char* label) {
    return new KText(x, y, w, h, label);
}

KTextBox* KCreateTextBox(int x, int y, int w, int h, const char* label) {
    return new KTextBox(x, y, w, h, label);
}

KTable* KCreateTable(int x, int y, int w, int h, const char* label) {
    return new KTable(x, y, w, h, label);
}

KPanel* KCreatePanel(int x, int y, int w, int h, const char* label) {
    return new KPanel(x, y, w, h, label);
}

KCard* KCreateCard(int x, int y, int w, int h, const char* label) {
    // Factory returns an empty themed card container; callers add child widgets as needed.
    return new KCard(x, y, w, h, label);
}

KToolbar* KCreateToolbar(int x, int y, int w, int h, const char* label) {
    // Factory returns a square flat toolbar suitable for KButton and KInput children.
    return new KToolbar(x, y, w, h, label);
}

KStatusBar* KCreateStatusBar(int x, int y, int w, int h, const char* label) {
    // Factory returns a status strip with left text initialized from the optional label.
    return new KStatusBar(x, y, w, h, label);
}

KBadge* KCreateBadge(int x, int y, int w, int h, const char* label) {
    // Factory returns a compact state/count badge with theme primary accent.
    return new KBadge(x, y, w, h, label);
}

KSeparator* KCreateSeparator(int x, int y, int w, int h, KSeparatorOrientation orientation) {
    // Factory returns a paint-only divider in the requested horizontal or vertical orientation.
    return new KSeparator(x, y, w, h, orientation);
}

KChoice* KCreateChoice(int x, int y, int w, int h, const char* label) {
    return new KChoice(x, y, w, h, label);
}

void KChoiceSetItems(Fl_Choice* choice, const std::vector<std::string>& items, int selected) {
    if (!choice) {
        return;
    }
    choice->clear();
    for (const std::string& item : items) {
        choice->add(item.c_str());
    }
    if (!items.empty()) {
        selected = std::max(0, std::min(selected, static_cast<int>(items.size()) - 1));
        choice->value(selected);
    }
}

KOutput* KCreateOutput(int x, int y, int w, int h, const char* label) {
    return new KOutput(x, y, w, h, label);
}

KProgressBar* KCreateProgress(int x, int y, int w, int h, const char* label) {
    return new KProgressBar(x, y, w, h, label);
}

KRadioButton* KCreateRadioButton(int x, int y, int w, int h, const char* label) {
    return new KRadioButton(x, y, w, h, label);
}

KToggleButton* KCreateToggleButton(int x, int y, int w, int h, const char* label) {
    return new KToggleButton(x, y, w, h, label);
}

KLightButton* KCreateLightButton(int x, int y, int w, int h, const char* label) {
    return new KLightButton(x, y, w, h, label);
}

KScrollArea* KCreateScroll(int x, int y, int w, int h, const char* label) {
    return new KScrollArea(x, y, w, h, label);
}

KScrollBar* KCreateScrollBar(int x, int y, int w, int h, int type) {
    return new KScrollBar(x, y, w, h, type);
}

KTabs* KCreateTabs(int x, int y, int w, int h, const char* label) {
    return new KTabs(x, y, w, h, label);
}

KPanel* KCreateTabPage(int x, int y, int w, int h, const char* label) {
    return new KPanel(x, y, w, h, label);
}

KScrollArea* KCreateScrollArea(int x, int y, int w, int h, const char* label) {
    return new KScrollArea(x, y, w, h, label);
}

KTextDisplay* KCreateTextDisplay(int x, int y, int w, int h, const char* label) {
    return new KTextDisplay(x, y, w, h, label);
}

KImageView* KCreateImageView(int x, int y, int w, int h, const char* label) {
    // Factory returns a themed image view with no loaded image; callers set path or Fl_Image later.
    return new KImageView(x, y, w, h, label);
}

KImageView* KCreateImageViewFromFile(int x, int y, int w, int h, const char* path, const char* label, KImageFitMode mode) {
    // Factory creates the view, applies the requested fit mode, then loads through KImageResource.
    KImageView* view = new KImageView(x, y, w, h, label);
    view->set_fit_mode(mode);
    view->setImagePath(path);
    return view;
}

KBrowser* KCreateBrowser(int x, int y, int w, int h, const char* label) {
    return new KBrowser(x, y, w, h, label);
}

KMenuButton* KCreateMenuButton(int x, int y, int w, int h, const char* label) {
    return new KMenuButton(x, y, w, h, label);
}

KCounter* KCreateCounter(int x, int y, int w, int h, const char* label) {
    return new KCounter(x, y, w, h, label);
}

KSpinner* KCreateSpinner(int x, int y, int w, int h, const char* label) {
    return new KSpinner(x, y, w, h, label);
}

KValueInput* KCreateValueInput(int x, int y, int w, int h, const char* label) {
    return new KValueInput(x, y, w, h, label);
}

KSplitter* KCreateSplitter(int x, int y, int w, int h, const char* label) {
    return new KSplitter(x, y, w, h, label);
}

KBreadcrumb* KCreateBreadcrumb(int x, int y, int w, int h, const char* label) {
    // Factory returns a themed breadcrumb path widget with no initial items.
    return new KBreadcrumb(x, y, w, h, label);
}

KSideNav* KCreateSideNav(int x, int y, int w, int h, const char* label) {
    // Factory returns a vertical navigation list; callers populate rows with setItems().
    return new KSideNav(x, y, w, h, label);
}

KTopNav* KCreateTopNav(int x, int y, int w, int h, const char* label) {
    // Factory returns a horizontal navigation strip; callers populate rows with setItems().
    return new KTopNav(x, y, w, h, label);
}

KTreeView* KCreateTreeView(int x, int y, int w, int h, const char* label) {
    // Factory returns a lightweight expandable tree; callers provide KTreeNode roots.
    return new KTreeView(x, y, w, h, label);
}

KCommandPalette* KCreateCommandPalette(int x, int y, int w, int h, const char* label) {
    // Factory returns a modal command-palette shell without showing it immediately.
    return new KCommandPalette(x, y, w, h, label);
}

KSectionHeader* KCreateSectionHeader(int x, int y, int w, int h, const char* label) {
    // Factory returns a paint-only section header with optional action text support.
    return new KSectionHeader(x, y, w, h, label);
}

KToast* KCreateToast(int x, int y, int w, int h, const char* label) {
    // Factory returns a borderless toast popup; callers call showAt() when needed.
    return new KToast(x, y, w, h, label);
}

KTooltip* KCreateTooltip(int x, int y, int w, int h, const char* label) {
    // Factory returns a borderless tooltip popup; callers place it near the target.
    return new KTooltip(x, y, w, h, label);
}

KPopover* KCreatePopover(int x, int y, int w, int h, const char* label) {
    // Factory returns a titled popover shell with setContent() body text.
    return new KPopover(x, y, w, h, label);
}

KModalDialog* KCreateModalDialog(int x, int y, int w, int h, const char* label) {
    // Factory returns a self-drawn modal dialog shell; callers run it with runModal().
    return new KModalDialog(x, y, w, h, label);
}

KDrawer* KCreateDrawer(int x, int y, int w, int h, const char* label) {
    // Factory returns a themed drawer shell; callers decide edge positioning.
    return new KDrawer(x, y, w, h, label);
}

KOverlayMask* KCreateOverlayMask(int x, int y, int w, int h, const char* label) {
    // Factory returns a hidden overlay mask; callers enable it with setVisible().
    return new KOverlayMask(x, y, w, h, label);
}

KLoadingOverlay* KCreateLoadingOverlay(int x, int y, int w, int h, const char* label) {
    // Factory returns a loading overlay with caller-controlled loading/progress state.
    return new KLoadingOverlay(x, y, w, h, label);
}

KContextMenu* KCreateContextMenu(int x, int y, int w, int h, const char* label) {
    // Factory returns an enhanced context menu supporting disabled, checked, and separator rows.
    return new KContextMenu(x, y, w, h, label);
}

KStack* KCreateStack(int x, int y, int w, int h, const char* label) {
    // Factory returns a page stack container; callers add child pages with FLTK grouping.
    return new KStack(x, y, w, h, label);
}

KGridLayout* KCreateGridLayout(int x, int y, int w, int h, const char* label) {
    // Factory returns a fixed grid layout container.
    return new KGridLayout(x, y, w, h, label);
}

KVBox* KCreateVBox(int x, int y, int w, int h, const char* label) {
    // Factory returns a vertical box layout container.
    return new KVBox(x, y, w, h, label);
}

KHBox* KCreateHBox(int x, int y, int w, int h, const char* label) {
    // Factory returns a horizontal box layout container.
    return new KHBox(x, y, w, h, label);
}

KSplitterPane* KCreateSplitterPane(int x, int y, int w, int h, const char* label) {
    // Factory returns an enhanced two-pane splitter with draggable ratio.
    return new KSplitterPane(x, y, w, h, label);
}

KAccordion* KCreateAccordion(int x, int y, int w, int h, const char* label) {
    // Factory returns a text-section accordion; callers provide sections with setSections().
    return new KAccordion(x, y, w, h, label);
}

KExpander* KCreateExpander(int x, int y, int w, int h, const char* label) {
    // Factory returns a collapsible group container.
    return new KExpander(x, y, w, h, label);
}

KGroupBox* KCreateGroupBox(int x, int y, int w, int h, const char* label) {
    // Factory returns a titled group container.
    return new KGroupBox(x, y, w, h, label);
}

KScrollablePanel* KCreateScrollablePanel(int x, int y, int w, int h, const char* label) {
    // Factory returns a themed scroll container with padding metadata.
    return new KScrollablePanel(x, y, w, h, label);
}

KIconButton* KCreateIconButton(int x, int y, int w, int h, const char* label) {
    // Factory returns an icon-capable flat button.
    return new KIconButton(x, y, w, h, label);
}

KAvatar* KCreateAvatar(int x, int y, int w, int h, const char* label) {
    // Factory returns a square avatar with initials derived from label/name.
    return new KAvatar(x, y, w, h, label);
}

KTag* KCreateTag(int x, int y, int w, int h, const char* label) {
    // Factory returns a compact non-interactive tag.
    return new KTag(x, y, w, h, label);
}

KChip* KCreateChip(int x, int y, int w, int h, const char* label) {
    // Factory returns a selectable chip token.
    return new KChip(x, y, w, h, label);
}

KAlert* KCreateAlert(int x, int y, int w, int h, const char* label) {
    // Factory returns a semantic alert block.
    return new KAlert(x, y, w, h, label);
}

KEmptyState* KCreateEmptyState(int x, int y, int w, int h, const char* label) {
    // Factory returns an empty-state panel with optional action callback area.
    return new KEmptyState(x, y, w, h, label);
}

KSkeleton* KCreateSkeleton(int x, int y, int w, int h, const char* label) {
    // Factory returns a loading skeleton placeholder.
    return new KSkeleton(x, y, w, h, label);
}

KTimeline* KCreateTimeline(int x, int y, int w, int h, const char* label) {
    // Factory returns a vertical timeline display.
    return new KTimeline(x, y, w, h, label);
}

KMetricCard* KCreateMetricCard(int x, int y, int w, int h, const char* label) {
    // Factory returns a compact KPI card.
    return new KMetricCard(x, y, w, h, label);
}

KListView* KCreateListView(int x, int y, int w, int h, const char* label) {
    // Factory returns a lightweight rich list view.
    return new KListView(x, y, w, h, label);
}

KPropertyGrid* KCreatePropertyGrid(int x, int y, int w, int h, const char* label) {
    // Factory returns a property-name/value grid.
    return new KPropertyGrid(x, y, w, h, label);
}

KKeyValueTable* KCreateKeyValueTable(int x, int y, int w, int h, const char* label) {
    // Factory returns a read-only two-column key/value table.
    return new KKeyValueTable(x, y, w, h, label);
}

KMiniChart* KCreateMiniChart(int x, int y, int w, int h, const char* label) {
    // Factory returns a mini chart that can draw line or bar data.
    return new KMiniChart(x, y, w, h, label);
}

KProgressRing* KCreateProgressRing(int x, int y, int w, int h, const char* label) {
    // Factory returns a circular progress indicator.
    return new KProgressRing(x, y, w, h, label);
}

KStepper* KCreateStepper(int x, int y, int w, int h, const char* label) {
    // Factory returns a horizontal multi-step progress indicator.
    return new KStepper(x, y, w, h, label);
}

void KShowMessage(const char* title, const char* message) {
    Fl_Window dialog(380, 170, title ? title : "Message");
    dialog.set_modal();
    SetWindowStyle(&dialog);
    dialog.begin();
    KText msg(20, 20, 340, 75, message ? message : "");
    msg.align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE);
    KButton ok(140, 115, 100, 34, u8"确定", KBUTTON_HEAVY);
    ok.callback(CloseMessageCallback, &dialog);
    dialog.end();
    // Dialogs use the same custom caption implementation but hide maximize so
    // modal message geometry stays predictable and lightweight.
    KInstallTitleBar(&dialog, KTitleBarStyle::Fade, false);
    dialog.show();
    KApplyWindowIcon(&dialog);
    while (dialog.shown()) {
        Fl::wait();
    }
}

int KShowPopupMenu(int x, int y, const std::vector<std::string>& items) {
    if (items.empty()) {
        return -1;
    }

    const int item_height = 28;
    const int padding_x = 12;
    fl_font(FL_HELVETICA, kPopupFontSize);
    int max_label_width = 0;
    for (const std::string& item : items) {
        max_label_width = std::max(max_label_width, static_cast<int>(fl_width(item.c_str())));
    }

    const int menu_width = max_label_width + padding_x * 2 + 20;
    const int menu_height = static_cast<int>(items.size()) * item_height;
    int screen_x = 0;
    int screen_y = 0;
    int screen_w = 0;
    int screen_h = 0;
    Fl::screen_xywh(screen_x, screen_y, screen_w, screen_h, x, y);
    x = std::max(screen_x, std::min(x, screen_x + screen_w - menu_width));
    y = std::max(screen_y, std::min(y, screen_y + screen_h - menu_height));

    PopupMenuWindow menu_window(x, y, menu_width, menu_height);
    menu_window.border(0);
    SetWindowStyle(&menu_window);
    menu_window.set_modal();
    PopupMenuState state;
    state.window = &menu_window;

    menu_window.begin();
    for (std::size_t i = 0; i < items.size(); ++i) {
        KButton* button = new KButton(0, static_cast<int>(i) * item_height, menu_width, item_height, items[i].c_str(), KBUTTON_LIGHT);
        button->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        button->user_data(reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        button->callback(PopupMenuItemCallback, &state);
    }
    menu_window.end();

    menu_window.show();
    Fl::grab(&menu_window);
    while (menu_window.shown()) {
        Fl::wait();
    }
    Fl::grab(nullptr);
    return state.selected;
}
