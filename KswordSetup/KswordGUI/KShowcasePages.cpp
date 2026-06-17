#include "KShowcasePages.h"

#include "KDataView.h"
#include "KLayout.h"
#include "KNavigation.h"
#include "KOverlay.h"
#include "KVisual.h"
#include "KWidgets.h"

#include "fl_draw.H"

#include <algorithm>

namespace {
constexpr int kGap = 14;
constexpr int kHeaderH = 52;

// CreatePage prepares a themed root group; callers add children before end() is called by each page factory.
Fl_Group* CreatePage(int x, int y, int w, int h, const char* label) {
    Fl_Group* page = new Fl_Group(x, y, w, h, label);
    page->box(FL_FLAT_BOX);
    page->color(KThemeManager::instance().theme().panelBg);
    page->begin();
    return page;
}

// FinishPage closes FLTK child capture and returns the same page pointer for fluent factory code.
Fl_Group* FinishPage(Fl_Group* page) {
    page->end();
    return page;
}

// AddHeader creates a consistent page title and explanatory subtitle; the header is parented to current group.
KSectionHeader* AddHeader(int x, int y, int w, const char* title, const char* subtitle) {
    KSectionHeader* header = new KSectionHeader(x, y, w, kHeaderH, title);
    header->setSubtitle(subtitle);
    header->setActionText("Detached");
    return header;
}

// AddSection creates a titled sample container and returns it opened for optional caller children.
KSection* AddSection(int x, int y, int w, int h, const char* title, const char* subtitle) {
    KSection* section = new KSection(x, y, w, h, title);
    section->setSubtitle(subtitle);
    return section;
}
}

namespace KShowcasePages {

Fl_Group* CreateNavigationPage(int x, int y, int w, int h) {
    Fl_Group* page = CreatePage(x, y, w, h, "Navigation");
    AddHeader(x + kGap, y + kGap, w - kGap * 2, "Navigation", "Breadcrumb, top/side navigation, tree, stepper, and command-palette entry points.");

    KBreadcrumb* crumbs = new KBreadcrumb(x + kGap, y + 78, w - kGap * 2, 34);
    crumbs->setItems({ "Workspace", "KswordGUI", "Showcase" });
    crumbs->setActiveIndex(2);

    KTopNav* top = new KTopNav(x + kGap, y + 124, w - kGap * 2, 42);
    top->setItems({ { "Overview", "", true }, { "Components", "24", true }, { "Docs", "", true }, { "Disabled", "", false } });
    top->setActiveIndex(1);

    KSideNav* side = new KSideNav(x + kGap, y + 182, 190, h - 196, "Sections");
    side->setItems({ { "Navigation", "5", true }, { "Containers", "4", true }, { "Overlay", "6", true }, { "Data", "4", true }, { "Visual", "8", true } });
    side->setActiveIndex(0);

    KTreeView* tree = new KTreeView(x + 218, y + 182, std::max(220, w / 2 - 120), h - 196, "Tree");
    tree->setItems({
        { "Navigation", true, true, { { "Breadcrumb", true, true, {}, ">", "" }, { "TopNav", true, true, {}, "-", "" }, { "SideNav", true, true, {}, "|", "" } }, "", "3" },
        { "Command", true, true, { { "Palette", true, true, {}, "#", "Ctrl+K" } }, "", "1" }
    });
    tree->setShowRootLines(true);

    KStepper* steps = new KStepper(x + w - 362, y + 182, 348, 106);
    steps->setSteps({ { "Discover", "scan", true, true }, { "Design", "API", true, true }, { "Integrate", "main", false, true } });
    steps->setActiveIndex(1);

    KCardGrid* commandCard = new KCardGrid(x + w - 362, y + 302, 348, 132, "Command Palette");
    commandCard->setColumns(1);
    commandCard->setItems({ { "KCommandPalette", "Window-based Ctrl+K command search", "modal", true } });
    commandCard->setActiveIndex(0);
    return FinishPage(page);
}

Fl_Group* CreateContainerPage(int x, int y, int w, int h) {
    Fl_Group* page = CreatePage(x, y, w, h, "Containers");
    AddHeader(x + kGap, y + kGap, w - kGap * 2, "Containers", "Section, card grid, inspector panel, and existing accordion composition.");

    KSection* hero = AddSection(x + kGap, y + 78, w - kGap * 2, 118, "KSection", "Header, action slot, and child content region.");
    hero->setActionText("Action");
    hero->begin();
    KTag* tag = new KTag(x + kGap + 18, y + 144, 92, 28, "Content");
    tag->setAccentColor(KThemeManager::instance().theme().success);
    KColorSwatch* swatch = new KColorSwatch(x + kGap + 124, y + 144, 136, 28, "Theme primary");
    swatch->setSelected(true);
    hero->end();

    KCardGrid* cards = new KCardGrid(x + kGap, y + 212, std::max(300, w / 2 - 22), h - 226, "Cards");
    cards->setColumns(2);
    cards->setItems({
        { "Reusable", "Self-painted card shell", "API", true },
        { "Readable", "Consistent spacing and hierarchy", "UI", true },
        { "Selectable", "Hover and active states", "UX", true },
        { "Disabled", "Muted but stable", "state", false }
    });
    cards->setActiveIndex(1);

    const int right_x = x + w / 2 + 8; const int right_w = w / 2 - 22; const int inspector_h = std::max(136, h - 226 - 118 - kGap);
    KInspectorPanel* inspector = new KInspectorPanel(right_x, y + 212, right_w, inspector_h, "Inspector");
    inspector->setSections({
        { "Appearance", { { "Theme", "Light", "token", true }, { "Accent", "Primary", "bind", false }, { "Density", "Comfort", "", true } }, true },
        { "Behavior", { { "Callbacks", "FLTK", "event", true }, { "Ownership", "Caller", "ptr", true } }, true }
    });

    KAccordion* accordion = new KAccordion(right_x, y + 212 + inspector_h + kGap, right_w, 118, "Accordion");
    accordion->setSections({ { "Existing KAccordion", "Defined in KLayout and intentionally left untouched.", true, true }, { "Integration", "Use KCreateAccordion from the main factory layer.", false, true } });
    return FinishPage(page);
}

Fl_Group* CreateOverlayPage(int x, int y, int w, int h) {
    Fl_Group* page = CreatePage(x, y, w, h, "Overlay");
    AddHeader(x + kGap, y + kGap, w - kGap * 2, "Overlay", "Toast, tooltip, popover, modal mask, drawer, context menu, and loading examples.");

    KCardGrid* overlays = new KCardGrid(x + kGap, y + 78, w - kGap * 2, 172, "Overlay types");
    overlays->setColumns(3);
    overlays->setItems({
        { "KToast", "Transient notification window", "popup", true },
        { "KTooltip", "Compact hover help", "hint", true },
        { "KPopover", "Rich anchored content", "bubble", true },
        { "KDrawer", "Side panel shell", "panel", true },
        { "KContextMenu", "Checked and shortcut menu rows", "menu", true },
        { "KModalMask", "Blocking scrim widget", "mask", true }
    });
    overlays->setActiveIndex(5);

    KSection* maskHost = AddSection(x + kGap, y + 266, w / 2 - 22, h - 280, "KModalMask", "Visible in this bounded preview panel.");
    maskHost->begin();
    KModalMask* mask = new KModalMask(x + kGap + 16, y + 332, w / 2 - 54, 126, "Modal work in progress");
    mask->setVisible(true);
    maskHost->end();

    KSection* loadingHost = AddSection(x + w / 2 + 8, y + 266, w / 2 - 22, h - 280, "KLoadingOverlay", "Caller-driven step/progress animation state.");
    loadingHost->begin();
    KLoadingOverlay* loading = new KLoadingOverlay(x + w / 2 + 24, y + 332, w / 2 - 54, 126, "Loading assets");
    loading->setLoading(true);
    loading->setProgress(0.64);
    loading->setStep(3);
    loadingHost->end();
    return FinishPage(page);
}

Fl_Group* CreateDataPage(int x, int y, int w, int h) {
    Fl_Group* page = CreatePage(x, y, w, h, "Data");
    AddHeader(x + kGap, y + kGap, w - kGap * 2, "Data display", "Property grid, list view, enhanced tree view, and empty state.");

    KListView* list = new KListView(x + kGap, y + 78, w / 2 - 22, 174, "List");
    list->setItems({ { "Design token", "Primary color changed", "now", true }, { "SVG cache", "Theme-aware icon loaded", "1m", true }, { "Disabled row", "Read-only state", "old", false } });
    list->setActiveIndex(0);

    KPropertyGrid* props = new KPropertyGrid(x + w / 2 + 8, y + 78, w / 2 - 22, 174, "Properties");
    props->setNameColumnWidth(128);
    props->setProperties({ { "Component", "KPropertyGrid" }, { "Rows", "Configurable" }, { "Editing", "Host-managed" }, { "Theme", "KThemeManager" } });

    KTreeView* tree = new KTreeView(x + kGap, y + 268, w / 2 - 22, h - 282, "Enhanced tree");
    tree->setItems({ { "Root", true, true, { { "Filtered child", true, true, {}, "*", "match" }, { "Muted child", true, false, {}, "", "off" } }, "", "2" } });
    tree->setFilterText("child");

    KEmptyState* empty = new KEmptyState(x + w / 2 + 8, y + 268, w / 2 - 22, h - 282, "No results");
    empty->setDescription("Use KEmptyState when list, grid, or search data is not available.");
    empty->setActionText("Reset");
    return FinishPage(page);
}

Fl_Group* CreateVisualPage(int x, int y, int w, int h) {
    Fl_Group* page = CreatePage(x, y, w, h, "Visual");
    AddHeader(x + kGap, y + kGap, w - kGap * 2, "Visual", "Stat card, timeline, progress ring, mini chart, badge, tag, avatar, and icon button.");

    KStatCard* stat = new KStatCard(x + kGap, y + 78, 210, 112, "Revenue");
    stat->setValue("$42.8K");
    stat->setCaption("+12.4% this cycle");
    stat->setIconName("business/chart_bar_line");

    KMetricCard* metric = new KMetricCard(x + 238, y + 78, 210, 112, "Latency");
    metric->setValue("128 ms");
    metric->setTrend("-8 ms", true);

    KProgressRing* ring = new KProgressRing(x + 462, y + 78, 126, 112, "64%");
    ring->setValue(0.64);

    KMiniChart* chart = new KMiniChart(x + 602, y + 78, std::max(160, w - 616), 112, "Chart");
    chart->setValues({ 12, 18, 15, 24, 22, 30, 28, 34 });

    KTimeline* timeline = new KTimeline(x + kGap, y + 206, w / 2 - 22, h - 220, "Timeline");
    timeline->setItems({ { "Scaffold", "Create detached showcase pages", true }, { "Review", "Mainline wires tab or route later", false }, { "Ship", "No Main changes in this task", false } });

    KSection* identity = AddSection(x + w / 2 + 8, y + 206, w / 2 - 22, h - 220, "Identity and labels", "Avatar, badge, tag, and SVG icon button.");
    identity->begin();
    KAvatar* avatar = new KAvatar(x + w / 2 + 28, y + 274, 48, 48, "Ada Lovelace");
    avatar->setOnline(true);
    KBadge* badge = new KBadge(x + w / 2 + 92, y + 282, 56, 28, "12");
    KTag* tag = new KTag(x + w / 2 + 164, y + 282, 88, 28, "Stable");
    KIconButton* icon = new KIconButton(x + w / 2 + 268, y + 278, 150, 36, "SVG icon");
    icon->setIconName("system/settings_6_line");
    icon->setIcon("#");
    identity->end();
    return FinishPage(page);
}

Fl_Group* CreateInputPage(int x, int y, int w, int h) {
    Fl_Group* page = CreatePage(x, y, w, h, "Input helpers");
    AddHeader(x + kGap, y + kGap, w - kGap * 2, "Input helpers", "Search box, segmented control, switch, color swatch, and SVG icon button.");

    KSearchBox* search = new KSearchBox(x + kGap, y + 86, std::max(260, w / 2 - 22), 36, "Search components");
    search->setText("theme");

    KSegmentedControl* segments = new KSegmentedControl(x + kGap, y + 142, std::max(260, w / 2 - 22), 38);
    segments->setItems({ "All", "Stable", "New", "Deprecated" });
    segments->setActiveIndex(1);

    KSwitch* sw = new KSwitch(x + kGap, y + 202, 180, 36, "Feature flag");
    sw->setChecked(true);
    sw->setTexts("Enabled", "Disabled");

    KColorSwatch* primary = new KColorSwatch(x + 214, y + 202, 170, 36, "Primary");
    primary->setSelected(true);
    KColorSwatch* danger = new KColorSwatch(x + 398, y + 202, 170, 36, "Danger");
    danger->setSwatchColor(KThemeManager::instance().theme().danger);

    KSection* iconApi = AddSection(x + w / 2 + 8, y + 86, w / 2 - 22, 152, "SVG icon API", "Uses KIcon::LoadThemedSvg with fallback glyph drawing.");
    iconApi->begin();
    KIconButton* settings = new KIconButton(x + w / 2 + 28, y + 154, 164, 38, "Settings");
    settings->setIconName("system/settings_6_line");
    settings->setIcon("#");
    KIconButton* warning = new KIconButton(x + w / 2 + 206, y + 154, 164, 38, "Warning");
    warning->setIconName("system/warning_line");
    warning->setIconColorRole(KIconColorRole::Warning);
    warning->setIcon("!");
    iconApi->end();
    return FinishPage(page);
}

std::vector<Fl_Group*> CreateAllPages(int x, int y, int w, int h) {
    std::vector<Fl_Group*> pages;
    // The order mirrors the component taxonomy so Main can attach the pages to tabs or side nav later.
    pages.push_back(CreateNavigationPage(x, y, w, h));
    pages.push_back(CreateContainerPage(x, y, w, h));
    pages.push_back(CreateOverlayPage(x, y, w, h));
    pages.push_back(CreateDataPage(x, y, w, h));
    pages.push_back(CreateVisualPage(x, y, w, h));
    pages.push_back(CreateInputPage(x, y, w, h));
    return pages;
}

} // namespace KShowcasePages
