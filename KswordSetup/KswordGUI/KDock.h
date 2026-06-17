#ifndef KSWORD_GUI_KDOCK_H
#define KSWORD_GUI_KDOCK_H

// Keep FLTK legacy aliases quiet in projects that compile with stricter warnings.
#define FL_NO_DEPRECATED

#include "Fl_Group.H"
#include "Fl_Window.H"
#include "Fl_Widget.H"

#include <istream>
#include <string>
#include <vector>

// KDockAreaPosition names the five stable docking targets managed by KDockManager.
enum class KDockAreaPosition {
    Left,
    Right,
    Top,
    Bottom,
    Center
};

class KDockArea;
class KDockFloatingWindow;
class KDockLayoutNode;
class KDockManager;
class KDockWidget;

// KDockDropSide identifies the side selected by the drag overlay guides.
enum class KDockDropSide {
    None,
    Left,
    Right,
    Top,
    Bottom,
    Center
};

// KDockDropScope separates main-window edge drops from dock-local split drops.
enum class KDockDropScope {
    None,
    MainWindow,
    DockWidget
};

// KDockDropTarget stores one computed drag target and the highlighted guide side.
struct KDockDropTarget {
    // Builds an invalid drop target; callers fill fields when a hit is found.
    KDockDropTarget();

    bool valid;
    KDockDropScope scope;
    KDockDropSide side;
    KDockArea* area;
    KDockWidget* dock;
    KDockAreaPosition position;
};

// KDockWidget is a lightweight FLTK-native dock panel with a draggable title bar.
class KDockWidget : public Fl_Group {
public:
    // Creates a dock widget and optionally adopts content into its client area.
    KDockWidget(int x, int y, int w, int h, const char* title = nullptr, Fl_Widget* content = nullptr);

    // Destroys the floating host window if one is still attached; returns no value.
    ~KDockWidget() override;

    // Draws the frame, title bar, title text, and close/float buttons.
    void draw() override;

    // Handles title-button clicks and drag-release docking through the manager.
    int handle(int event) override;

    // Replaces the content widget and reparents it under this dock; returns no value.
    void setContent(Fl_Widget* content);

    // Returns the current content widget pointer, or nullptr when empty.
    Fl_Widget* content() const;

    // Updates the stable title used by tabs and save/restore layout; returns no value.
    void setTitle(const std::string& title);

    // Returns the stable title used by tabs and save/restore layout.
    const std::string& title() const;

    // Stores an icon identifier for future resource-backed title/tab drawing; returns no value.
    void setIconName(const std::string& iconName);

    // Returns the reserved icon identifier, or an empty string when none was assigned.
    const std::string& iconName() const;

    // Sets the reserved pin state used by future auto-hide behavior; returns no value.
    void setPinned(bool pinned);

    // Returns true when the dock is currently marked pinned.
    bool isPinned() const;

    // Toggles the reserved pin state and redraws the title/tab chrome; returns no value.
    void togglePinned();

    // Assigns the docking manager that resolves future drag drops; returns no value.
    void setDockManager(KDockManager* manager);

    // Returns the current docking manager pointer, or nullptr when unmanaged.
    KDockManager* dockManager() const;

    // Assigns the area currently hosting this dock; returns no value.
    void setDockArea(KDockArea* area);

    // Returns the current dock area pointer, or nullptr when hidden or floating.
    KDockArea* dockArea() const;

    // Moves this dock into its own top-level Fl_Window; returns no value.
    void floatDock();

    // Hides this dock and removes it from the active area; returns no value.
    void closeDock();

    // Makes this dock the active tab inside its current area; returns no value.
    void activateDock();

    // Recomputes content geometry after a dock resize or tab activation; returns no value.
    void layoutContent();

    // Returns true when this dock is hosted by a floating Fl_Window.
    bool isFloating() const;

    // Writes floating host geometry into the references and returns true when floating.
    bool floatingGeometry(int& root_x, int& root_y, int& width, int& height) const;

    // Keeps the title bar and content region aligned when FLTK resizes the dock.
    void resize(int x, int y, int w, int h) override;

private:
    // Allows KDockArea to clear the temporary floating host during redocking.
    friend class KDockArea;
    friend class KDockFloatingWindow;
    friend class KDockManager;

    // Detaches and deletes the temporary floating host window; returns no value.
    void detachFloatingWindow();

    // Creates a floating host at the requested root position; returns no value.
    void createFloatingWindowAt(int root_x, int root_y, int width, int height);

    // Promotes a title-drag operation to a floating drag host; returns no value.
    void beginFloatingDrag(int root_x, int root_y);

    // Moves the floating host so the cursor keeps its title-bar offset.
    void moveFloatingWindowForDrag(int root_x, int root_y);

    // Continues an active drag after the dock has been reparented to a floating window.
    void continueFloatingDrag(int root_x, int root_y);

    // Finishes an active drag, then either redocks or keeps the dock floating.
    void finishFloatingDrag(int root_x, int root_y);

    // Returns the root-screen point at the floating host center when available.
    bool floatingCenterRootPoint(int& root_x, int& root_y) const;

    // Returns true while a title/tab drag should be proxied by the floating window.
    bool hasActiveFloatingDrag() const;

    // Returns true when a local point is inside the title bar drag region.
    bool inTitleBar(int local_x, int local_y) const;

    // Returns true when a local point is inside the close button rectangle.
    bool inCloseButton(int local_x, int local_y) const;

    // Returns true when a local point is inside the float button rectangle.
    bool inFloatButton(int local_x, int local_y) const;

    // Returns true when a local point is inside the reserved pin button rectangle.
    bool inPinButton(int local_x, int local_y) const;

private:
    std::string title_;
    std::string icon_name_;
    Fl_Widget* content_;
    KDockManager* manager_;
    KDockArea* area_;
    Fl_Window* floating_window_;
    bool pinned_;
    bool dragging_;
    bool drag_started_;
    bool layouting_content_;
    int drag_start_root_x_;
    int drag_start_root_y_;
    int drag_offset_x_;
    int drag_offset_y_;
};

// KDockArea hosts several dock widgets as a compact tab set in one manager region.
class KDockArea : public Fl_Group {
public:
    // Creates one docking area for a fixed manager position.
    KDockArea(int x, int y, int w, int h, KDockAreaPosition position, const char* label = nullptr);

    // Draws the area background, border, and tab strip before active dock content.
    void draw() override;

    // Handles tab selection clicks and delegates all other input to Fl_Group.
    int handle(int event) override;

    // Adds a dock widget to this tab set and makes it active; returns no value.
    void addDockWidget(KDockWidget* widget);

    // Inserts a dock widget at a tab index and makes it active; returns no value.
    void insertDockWidget(KDockWidget* widget, int index);

    // Removes a dock widget without deleting it; returns no value.
    void removeDockWidget(KDockWidget* widget);

    // Activates an existing dock widget by pointer; returns no value.
    void setActiveDockWidget(KDockWidget* widget);

    // Returns the active dock widget pointer, or nullptr when the area is empty.
    KDockWidget* activeDockWidget() const;

    // Returns the active tab index, or -1 when no dock is present.
    int activeIndex() const;

    // Sets the active tab by index when valid; returns no value.
    void setActiveIndex(int index);

    // Returns the number of dock widgets currently tabbed in this area.
    int dockCount() const;

    // Returns the dock widget at index, or nullptr when index is out of range.
    KDockWidget* dockAt(int index) const;

    // Returns the index for a dock widget, or -1 when it is not in this area.
    int indexOfDockWidget(KDockWidget* widget) const;

    // Returns the fixed manager position represented by this area.
    KDockAreaPosition position() const;

    // Stores a stable layout id used by save/restore; returns no value.
    void setLayoutId(const std::string& layoutId);

    // Returns the stable layout id used by save/restore.
    const std::string& layoutId() const;

    // Marks whether this is one of the manager's five persistent default areas.
    void setDefaultArea(bool defaultArea);

    // Returns true for persistent main-window areas that are hidden instead of deleted.
    bool isDefaultArea() const;

    // Recalculates tab/content geometry and active visibility; returns no value.
    void layoutDocks();

    // Keeps the tab/content layout valid when FLTK resizes the area.
    void resize(int x, int y, int w, int h) override;

private:
    // Computes which tab contains a local point, or -1 when none matches.
    int tabIndexAt(int local_x, int local_y) const;

private:
    KDockAreaPosition position_;
    std::string layout_id_;
    std::vector<KDockWidget*> docks_;
    int active_index_;
    int hover_index_;
    bool default_area_;
    bool layouting_docks_;
    bool tab_dragging_;
    int tab_drag_index_;
    int tab_drag_start_root_x_;
    int tab_drag_start_root_y_;
    KDockWidget* tab_drag_widget_;
};

// KDockManager arranges persistent main targets plus dynamic split areas.
class KDockManager : public Fl_Group {
public:
    // Creates all five dock areas and lays them out inside the given rectangle.
    KDockManager(int x, int y, int w, int h, const char* label = nullptr);

    // Releases split-tree bookkeeping; FLTK keeps normal child widget ownership.
    ~KDockManager() override;

    // Draws child dock areas and the optional drag/drop overlay guides.
    void draw() override;

    // Adds a dock widget to the requested area and records it for restore lookup.
    void addDockWidget(KDockAreaPosition position, KDockWidget* widget);

    // Tabs second dock into the same area as first dock; returns no value.
    void tabifyDockWidget(KDockWidget* first, KDockWidget* second);

    // Splits first dock's area and places second dock on the requested side.
    void splitDockWidget(KDockWidget* first, KDockWidget* second, KDockDropSide side);

    // Returns the dock area for a position; the pointer is owned by the manager.
    KDockArea* area(KDockAreaPosition position) const;

    // Docks or floats a title-bar drag release using root-screen coordinates.
    void dockWidgetFromDrag(KDockWidget* widget, int root_x, int root_y);

    // Updates overlay state while a dock title bar is being dragged.
    void updateDockDrag(KDockWidget* widget, int root_x, int root_y);

    // Clears any active drag overlay and redraws the manager.
    void clearDockDrag();

    // Serializes current membership as a stable text format keyed by dock title.
    std::string saveLayout() const;

    // Restores a saveLayout() string by matching registered dock titles.
    bool restoreLayout(const std::string& layoutText);

    // Recomputes the split-tree geometry and area visibility; returns no value.
    void layoutAreas();

    // Keeps all dock areas aligned with the manager after FLTK resizes it.
    void resize(int x, int y, int w, int h) override;

private:
    // Lets dock widgets notify the manager when floating/closing leaves areas empty.
    friend class KDockWidget;

    // Adds a dock pointer to the registry once so restoreLayout can find it later.
    void rememberDock(KDockWidget* widget);

    // Finds a registered dock by exact title match, or nullptr if absent.
    KDockWidget* findDockByTitle(const std::string& title) const;

    // Removes a dock from every managed area before moving it elsewhere.
    void removeDockFromAreas(KDockWidget* widget);

    // Moves a dock into a specific leaf area without creating duplicate tabs.
    void dockWidgetToArea(KDockWidget* widget, KDockArea* target);

    // Ensures the persistent main edge/center area exists in the split tree.
    KDockArea* ensureMainAreaInTree(KDockAreaPosition position);

    // Splits an existing leaf area and returns the newly-created target area.
    KDockArea* splitAreaForDrop(KDockArea* sourceArea, KDockDropSide side);

    // Creates a manager-owned dynamic area with an automatically generated id.
    KDockArea* createDynamicArea(KDockAreaPosition position);

    // Creates or reuses a manager-owned area with a restore-provided layout id.
    KDockArea* createAreaForRestore(const std::string& layoutId, KDockAreaPosition position);

    // Adds an area pointer to the manager registry once; returns no value.
    void registerArea(KDockArea* dockArea);

    // Returns an area by layout id, or nullptr when no area is registered.
    KDockArea* areaByLayoutId(const std::string& layoutId) const;

    // Returns true when the area currently appears as a leaf in the split tree.
    bool areaIsInLayout(KDockArea* dockArea) const;

    // Removes empty non-center areas from the split tree and deletes dynamic ones.
    void cleanupEmptyAreas();

    // Recursively deletes split-tree nodes without deleting FLTK dock areas.
    void deleteLayoutTree(KDockLayoutNode* node);

    // Deletes all dynamic areas after their docks have been detached for restore.
    void deleteDynamicAreas();

    // Resets registered docks to a hidden, unmanaged state before restore.
    void resetDocksForRestore();

    // Restores the original v1 layout format for backward compatibility.
    bool restoreLayoutV1(std::istream& input);

    // Restores the v2 layout format including split nodes and floating geometry.
    bool restoreLayoutV2(std::istream& input);

    // Maps root-screen coordinates into the nearest manager side position.
    KDockAreaPosition dropPositionForRootPoint(int root_x, int root_y) const;

    // Computes the precise overlay/drop target for one root-screen point.
    KDockDropTarget dropTargetForRootPoint(KDockWidget* widget, int root_x, int root_y) const;

    // Computes a tolerant drop target from the dragged dock's floating window.
    KDockDropTarget dropTargetForFloatingWidget(KDockWidget* widget, int root_x, int root_y) const;

    // Converts a computed target into the nearest stable main-window position.
    KDockAreaPosition positionForDropTarget(const KDockDropTarget& target) const;

    // Draws the active drop overlay guide set when a drag target is valid.
    void drawDropOverlay();

    // Recursively lays out one split-tree node inside a manager-local rectangle.
    void layoutNode(KDockLayoutNode* node, int node_x, int node_y, int node_w, int node_h);

private:
    KDockArea* left_;
    KDockArea* right_;
    KDockArea* top_;
    KDockArea* bottom_;
    KDockArea* center_;
    KDockLayoutNode* root_node_;
    std::vector<KDockArea*> all_areas_;
    std::vector<KDockWidget*> known_docks_;
    KDockDropTarget active_drop_target_;
    KDockWidget* dragging_widget_;
    int next_area_id_;
    int left_width_;
    int right_width_;
    int top_height_;
    int bottom_height_;
    bool layouting_areas_;
};

// Factory creates a dock manager with the public API's compact allocation syntax.
KDockManager* KCreateDockManager(int x, int y, int w, int h);

// Factory creates a dock widget using a title and caller-provided content widget.
KDockWidget* KCreateDockWidget(const char* title, Fl_Widget* content);

#endif // KSWORD_GUI_KDOCK_H
