#pragma once

#include "../Core/DriverService.h"
#include "../Core/Privilege.h"
#include "../Docking/DockManager.h"
#include "../Ui/PlaceholderPage.h"

#include "../Core/Win32Lean.h"

#include <memory>
#include <vector>

namespace Ksword::App {

// MainWindow owns the top-level Win32 shell. Inputs are provided through create;
// processing creates toolbar controls and the full-width docking host; run()
// returns the process message-loop exit code.
class MainWindow final {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(HINSTANCE instance, int showCommand);
    int run();

    // WndProc is public only so the Win32 class registration helper can bind it.
    // Inputs are normal Win32 window-procedure values; processing dispatches to
    // the owning MainWindow; output is a Win32 LRESULT.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
private:
    struct DockSlot {
        int dockIndex = -1;
        HWND page = nullptr;
        bool materialized = false;
    };

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // CommandEditProc subclasses the compact command input. Inputs are normal
    // edit-control window-procedure values; processing intercepts Enter and
    // forwards to executeCommandInput; output is a Win32 LRESULT.
    static LRESULT CALLBACK CommandEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void createMenuBar();
    void createChildControls();
    // createCommandInput creates the compact owned edit window used for command
    // launching. There is no input; processing subclasses the edit and positions
    // it beside the topmost menu item; no value is returned.
    void createCommandInput();
    void createModuleDocks();
    // createModulePlaceholderPage creates a lightweight tab body used during
    // startup. Inputs are a module descriptor and initial bounds; processing
    // avoids touching feature code/enumerators; output is a child HWND.
    HWND createModulePlaceholderPage(const Ksword::Ui::ModuleDescriptor& module, const RECT& bounds) const;
    // createModulePage creates the real feature page registered in
    // FeatureRegistry. Inputs are the module descriptor and initial bounds;
    // processing calls the descriptor factory and falls back to a diagnostic
    // fallback page if the module cannot create a child HWND; output is HWND.
    HWND createModulePage(const Ksword::Ui::ModuleDescriptor& module, const RECT& bounds) const;
    // materializeDockForDockIndex replaces a startup placeholder with the real
    // feature page the first time a dock is activated. Input is the dock index
    // posted by DockManager; processing keeps the tab/split/floating location
    // intact; no value is returned.
    void materializeDockForDockIndex(int dockIndex);
    void rebuildWindowMenuChecks();
    void toggleModuleDock(int moduleIndex);
    bool moduleDockVisible(int moduleIndex) const;
    // isTopmost reads the current extended window style. There is no input;
    // processing checks WS_EX_TOPMOST on the native top-level HWND; output is
    // true only when the shell window is currently topmost.
    bool isTopmost() const;
    // toggleTopmost switches the native topmost state. There is no input;
    // processing calls SetWindowPos with HWND_TOPMOST/HWND_NOTOPMOST, refreshes
    // the menu text, and writes a compact status line; no value is returned.
    void toggleTopmost();
    // refreshTopmostMenuText keeps the menu command text synchronized with the
    // window state. There is no input; processing rewrites the top-level menu
    // item to either "置顶" or "取消置顶"; no value is returned.
    void refreshTopmostMenuText();
    // positionCommandInput keeps the command edit visually on the menu bar.
    // There is no input; processing uses GetMenuItemRect on the topmost item and
    // moves the edit to its left; no value is returned.
    void positionCommandInput();
    // enableStartupPrivileges applies common token privileges at startup. There
    // is no input; processing stores per-privilege results and updates status
    // text/tooltips without modal dialogs; no value is returned.
    void enableStartupPrivileges();
    // executeCommandInput starts cmd.exe /k with the current edit text. There is
    // no input; processing ignores blank input, creates a new console, clears
    // the edit, and never waits for the child process; no value is returned.
    void executeCommandInput();
    void layout();
    void refreshPrivilegeText();
    void refreshDriverText(const Ksword::Core::DriverRuntimeStatus& status);
    // queryDriverStatusDeferred performs the first SCM/control-device probe
    // after the window has already been created and painted. There is no input;
    // processing updates cached driver state and menu text; no value is
    // returned.
    void queryDriverStatusDeferred();
    void handleUiAccessButtonClicked();
    void installDriverFromButton();
    void paint(HDC dc);

private:
    HINSTANCE instance_;
    HWND hwnd_;
    HWND commandEdit_;
    HWND statusText_;
    HMENU mainMenu_;
    HMENU windowMenu_;
    WNDPROC commandEditProc_;
    std::unique_ptr<Ksword::Docking::DockManager> dockManager_;
    std::vector<Ksword::Ui::ModuleDescriptor> modules_;
    std::vector<DockSlot> dockSlots_;
    std::vector<Ksword::Core::PrivilegeEnableResult> startupPrivilegeResults_;
    std::wstring startupPrivilegeSummary_;
    Ksword::Core::DriverRuntimeStatus driverStatus_;
    bool driverStatusKnown_ = false;
};

} // namespace Ksword::App
