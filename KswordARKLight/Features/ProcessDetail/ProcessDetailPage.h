#pragma once

#include "ProcessDetailTypes.h"

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::ProcessDetail {

// ProcessDetailPage owns the lightweight Win32 process-detail dock page. Inputs
// are a parent HWND, target PID and initial bounds; processing creates one root
// window with exactly three tabs (Basic, Threads, Modules); output is the root
// HWND and all child lifetime is tied to that HWND.
class ProcessDetailPage final {
public:
    // Create registers the page class if needed and creates a page instance.
    // Input is parent, target processId and parent-client bounds; processing
    // allocates a ProcessDetailPage owned by WM_NCDESTROY; output is root HWND.
    static HWND Create(HWND parent, DWORD processId, const RECT& bounds);

    // WindowProc is the Win32 class procedure used by RegisterClassW. Inputs
    // are standard Win32 message values; processing forwards to the page
    // instance stored in GWLP_USERDATA; output is the message LRESULT.
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    explicit ProcessDetailPage(DWORD processId);
    ~ProcessDetailPage() = default;

    ProcessDetailPage(const ProcessDetailPage&) = delete;
    ProcessDetailPage& operator=(const ProcessDetailPage&) = delete;

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    // Initialize creates child controls and performs the first snapshot refresh.
    // Input is the newly created root HWND; processing creates toolbar, tab and
    // list views; return value reports whether controls were created.
    bool Initialize(HWND hwnd);

    // Refresh reads target process data and repopulates all three pages. There
    // is no input beyond processId_; processing calls ProcessDetailCollector;
    // no value is returned.
    void Refresh();

    // Layout positions toolbar, tab and active page. Input is current client
    // size read from hwnd_; processing moves child HWNDs; no value is returned.
    void Layout();

    // UpdateVisiblePage shows only the selected tab child. Input is tab index
    // from the control; processing toggles child visibility; no return.
    void UpdateVisiblePage();

    // PopulateBasic renders the Basic tab. Input is snapshot_.basic; processing
    // rewrites the two-column list view; no value is returned.
    void PopulateBasic();

    // PopulateThreads renders the Threads tab. Input is snapshot_.threads;
    // processing rewrites the thread list view; no value is returned.
    void PopulateThreads();

    // PopulateModules renders the Modules tab. Input is snapshot_.modules;
    // processing rewrites the module list view; no value is returned.
    void PopulateModules();

    // HandleListContextMenu routes thread/module right-click and keyboard menu
    // requests. Inputs are the list HWND and screen point; processing selects
    // the hit row and opens the proper popup menu; output reports handled state.
    bool HandleListContextMenu(HWND list, POINT screenPoint);

    // ShowThreadContextMenu displays thread actions for the selected thread.
    // Input is the menu screen point; processing executes the chosen Win32
    // thread operation or copy action; no value is returned.
    void ShowThreadContextMenu(POINT screenPoint);

    // ShowModuleContextMenu displays module actions for the selected module.
    // Input is the menu screen point; processing executes copy/open-folder or
    // clearly reports unsupported retained actions; no value is returned.
    void ShowModuleContextMenu(POINT screenPoint);

    // SelectedListIndex returns the focused/selected row in a list-view. Input
    // is a list HWND; output is -1 when nothing is selected.
    int SelectedListIndex(HWND list) const;

    // SelectedListColumn returns the subitem column captured from the last
    // context-menu hit test. Input is the source list-view; processing chooses
    // thread/module cached state and clamps negative values to zero; output is
    // the column copied by CopySelectedListCell.
    int SelectedListColumn(HWND list) const;

    // CopySelectedListCell copies the first selected cell to the clipboard.
    // Input is the source list-view; processing uses the focused column when
    // available and falls back to column zero; no value is returned.
    void CopySelectedListCell(HWND list);

    // CopySelectedListRow copies the whole selected row. Inputs are source list
    // and known column count; processing writes tab-separated Unicode text to
    // the clipboard; no value is returned.
    void CopySelectedListRow(HWND list, int columnCount);

    // Thread action helpers apply Win32 operations to the selected thread row.
    // Inputs are implicit through the selected list row; processing uses
    // OpenThread/SuspendThread/ResumeThread/TerminateThread; no value is returned.
    void SuspendSelectedThread();
    void ResumeSelectedThread();
    void TerminateSelectedThread();

    // ShowSelectedThreadSummary opens a compact Win32 detail dialog for the
    // selected thread. Input is current thread-list selection; processing uses
    // already-collected TID/start/status columns and clipboard-safe text; no
    // value is returned.
    void ShowSelectedThreadSummary();

    // OpenSelectedModuleFolder opens Explorer at the selected module path. Input
    // is implicit through the selected module row; processing calls ShellExecuteW;
    // no value is returned.
    void OpenSelectedModuleFolder();

    // FocusSelectedModule reports and copies the selected module identity.
    // Input is the module list selection; processing copies module path for
    // cross-page search by other docks; no value is returned.
    void FocusSelectedModule();

    // UnloadSelectedModule requests a remote FreeLibrary for the selected
    // module. Input is the selected module row; processing prompts for
    // confirmation, resolves remote kernel32!FreeLibrary, creates one remote
    // thread, waits briefly, and reports the thread exit code; no value returns.
    void UnloadSelectedModule();

    // Module-thread helpers apply the original module-page Thread actions to
    // the representative ThreadID discovered for the selected module row.
    // Inputs are implicit through module selection; processing uses OpenThread
    // and SuspendThread/ResumeThread/TerminateThread; no value is returned.
    void SuspendSelectedModuleThread();
    void ResumeSelectedModuleThread();
    void TerminateSelectedModuleThread();

    // SetStatusLine writes concise operation feedback. Input is UI text;
    // processing updates statusText_ when available; no value is returned.
    void SetStatusLine(const std::wstring& text);

private:
    DWORD processId_ = 0;
    HWND hwnd_ = nullptr;
    HWND refreshButton_ = nullptr;
    HWND statusText_ = nullptr;
    HWND tab_ = nullptr;
    HWND basicList_ = nullptr;
    HWND threadsList_ = nullptr;
    HWND modulesList_ = nullptr;
    int lastThreadContextColumn_ = 0;
    int lastModuleContextColumn_ = 0;
    ProcessDetailSnapshot snapshot_{};
};

} // namespace Ksword::Features::ProcessDetail
