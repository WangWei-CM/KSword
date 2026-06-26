#pragma once

#include "../../Core/Win32Lean.h"
#include "EtwEventModel.h"
#include "EtwFilterModel.h"
#include "EtwSessionController.h"

#include <commctrl.h>

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Monitor {

// EtwMonitorView is the ETW-only monitoring page. Inputs are parent HWND and
// bounds; processing creates a Win32 child page with Start/Stop/Filter buttons
// and a ListView event table; output is the created page HWND.
class EtwMonitorView final {
public:
    EtwMonitorView();
    ~EtwMonitorView();

    EtwMonitorView(const EtwMonitorView&) = delete;
    EtwMonitorView& operator=(const EtwMonitorView&) = delete;

    // create builds the page window. Input is parent and initial bounds; output
    // is true when all child controls are created.
    bool create(HWND parent, const RECT& bounds);

    // hwnd returns the root child window. There are no inputs.
    HWND hwnd() const;

    // WndProc dispatches messages for the Win32 page class. Inputs are the
    // standard window-procedure parameters; output is the handled LRESULT.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // setDeleteOnDestroy controls ownership when the facade creates the page
    // with new. Input true means WM_NCDESTROY deletes this; no value returns.
    void setDeleteOnDestroy(bool enabled);

private:
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layout();
    void startSession();
    void stopSession();
    void openFilterDialog();
    // enqueueEventFromWorker receives ETW rows from the controller callback.
    // Input is one compact event row from the ETW worker thread; processing only
    // appends to a mutex-protected pending queue; there is no return value.
    void enqueueEventFromWorker(const EtwEvent& eventRow);

    // flushPendingEvents runs on the UI thread from WM_TIMER. Input is implicit
    // pending queue state; processing drains a bounded batch and updates the
    // ListView with redraw suppressed; there is no return value.
    void flushPendingEvents();

    // clearPendingEvents drops queued but not-yet-rendered rows. There are no
    // inputs; processing is mutex-protected; there is no return value.
    void clearPendingEvents();

    void appendEventsToView(const std::vector<EtwEvent>& events);
    void insertEventRowToList(const EtwEvent& eventRow);
    void clearEventList();
    void updateStatusText(const std::wstring& text);
    void updateButtonState();
    int iconIndexForProcessId(std::uint32_t processId);
    std::wstring processImagePath(std::uint32_t processId) const;
    void openSelectedEventDetail();
    void showEventDetail(const EtwEvent& eventRow);
    int selectedEventIndex() const;
    bool selectedEvent(EtwEvent* eventRow) const;
    std::wstring formatEventDetailText(const EtwEvent& eventRow) const;
    void copySelectedEventRow();
    void copySelectedEventDetail();
    void showEventContextMenu(POINT screenPoint);

    HWND hwnd_ = nullptr;
    HWND startButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND filterButton_ = nullptr;
    HWND clearButton_ = nullptr;
    HWND statusText_ = nullptr;
    HWND eventList_ = nullptr;
    HIMAGELIST eventImageList_ = nullptr;
    EtwEventModel eventModel_;
    EtwFilterModel filterModel_;
    EtwSessionController controller_;
    std::mutex pendingEventMutex_;
    std::deque<EtwEvent> pendingEvents_;
    std::unordered_map<std::uint32_t, int> processIconCache_;
    bool deleteOnDestroy_ = false;
};

// CreateEtwMonitorPage is the module-level facade used by the app integration
// session. Inputs are parent and bounds; output is the page HWND or null.
HWND CreateEtwMonitorPage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Monitor
