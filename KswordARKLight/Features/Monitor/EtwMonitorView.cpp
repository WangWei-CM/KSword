#include "EtwMonitorView.h"

#include "../../Ui/Controls.h"
#include "../../Ui/Theme.h"
#include "EtwFilterDialog.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <vector>

namespace Ksword::Features::Monitor {
namespace {

constexpr wchar_t kEtwMonitorViewClass[] = L"KswordARKLight.EtwMonitorView";
constexpr int kStartButtonId = 52001;
constexpr int kStopButtonId = 52002;
constexpr int kFilterButtonId = 52003;
constexpr int kClearButtonId = 52004;
constexpr int kListId = 52005;
constexpr UINT kStatusMessage = WM_APP + 62;
constexpr UINT_PTR kEventFlushTimerId = 52061;
constexpr UINT kEventFlushIntervalMs = 150;
constexpr std::size_t kMaxEventRows = 5000;
constexpr std::size_t kMaxEventsPerFlush = 200;
constexpr wchar_t kEtwEventDetailClass[] = L"KswordARKLight.EtwEventDetail";
constexpr UINT kEtwMenuDetail = 52601;
constexpr UINT kEtwMenuCopyRow = 52602;
constexpr UINT kEtwMenuCopyDetail = 52603;
constexpr UINT kEtwMenuClear = 52604;
constexpr UINT kEtwMenuStart = 52605;
constexpr UINT kEtwMenuStop = 52606;
constexpr UINT kEtwMenuFilter = 52607;

void EnsureViewClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = &EtwMonitorView::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kEtwMonitorViewClass;
    ::RegisterClassW(&wc);
    registered = true;
}

// DetailWindowProc owns a small read-only detail window used by ETW row
// double-click. Inputs are ordinary Win32 messages; processing creates and
// resizes one multiline EDIT child containing the supplied text; output is the
// standard message LRESULT.
LRESULT CALLBACK DetailWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* text = static_cast<std::wstring*>(create ? create->lpCreateParams : nullptr);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(text));
        return TRUE;
    }
    case WM_CREATE: {
        auto* text = reinterpret_cast<std::wstring*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE,
            L"EDIT",
            text ? text->c_str() : L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0,
            0,
            0,
            0,
            hwnd,
            nullptr,
            ::GetModuleHandleW(nullptr),
            nullptr);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(edit));
        if (edit != nullptr) {
            ::SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
        }
        delete text;
        return 0;
    }
    case WM_SIZE: {
        HWND edit = reinterpret_cast<HWND>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (edit != nullptr) {
            ::MoveWindow(edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    }
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// EnsureDetailWindowClass registers the ETW detail window class once. Input is
// none; processing is idempotent; no value is returned.
void EnsureDetailWindowClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DetailWindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kEtwEventDetailClass;
    ::RegisterClassW(&wc);
    registered = true;
}

void InsertColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    ListView_InsertColumn(list, index, &column);
}

std::wstring NumberText(const unsigned long long value) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%llu", value);
    return buffer;
}

// CopyTextToClipboard writes Unicode text for ETW menu actions. Inputs are owner
// HWND and text; processing transfers CF_UNICODETEXT to the system clipboard;
// output reports success.
bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!::OpenClipboard(owner)) {
        return false;
    }
    ::EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }
    void* target = ::GlobalLock(memory);
    if (!target) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

} // namespace

EtwMonitorView::EtwMonitorView()
    : eventModel_(kMaxEventRows) {}

EtwMonitorView::~EtwMonitorView() {
    controller_.stop();
    if (eventImageList_ != nullptr) {
        ImageList_Destroy(eventImageList_);
        eventImageList_ = nullptr;
    }
}

bool EtwMonitorView::create(HWND parent, const RECT& bounds) {
    EnsureViewClass();
    hwnd_ = ::CreateWindowExW(
        0,
        kEtwMonitorViewClass,
        L"ETW Monitor",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        this);
    return hwnd_ != nullptr;
}

HWND EtwMonitorView::hwnd() const {
    return hwnd_;
}

void EtwMonitorView::setDeleteOnDestroy(const bool enabled) {
    deleteOnDestroy_ = enabled;
}

LRESULT CALLBACK EtwMonitorView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    EtwMonitorView* view = reinterpret_cast<EtwMonitorView*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        view = cs ? static_cast<EtwMonitorView*>(cs->lpCreateParams) : nullptr;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        if (view != nullptr) {
            view->hwnd_ = hwnd;
        }
    }
    return view != nullptr ? view->handleMessage(hwnd, msg, wParam, lParam) : ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT EtwMonitorView::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        createControls();
        layout();
        ::SetTimer(hwnd_, kEventFlushTimerId, kEventFlushIntervalMs, nullptr);
        updateButtonState();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kStartButtonId) {
            startSession();
            return 0;
        }
        if (LOWORD(wParam) == kStopButtonId) {
            stopSession();
            return 0;
        }
        if (LOWORD(wParam) == kFilterButtonId) {
            openFilterDialog();
            return 0;
        }
        if (LOWORD(wParam) == kClearButtonId) {
            clearPendingEvents();
            eventModel_.clear();
            clearEventList();
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            header != nullptr && header->hwndFrom == eventList_ && header->code == NM_DBLCLK) {
            openSelectedEventDetail();
            return 0;
        }
        if (const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            header != nullptr && header->hwndFrom == eventList_ && header->code == NM_RCLICK) {
            POINT pt{};
            ::GetCursorPos(&pt);
            showEventContextMenu(pt);
            return 0;
        }
        break;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == eventList_) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                ::GetWindowRect(eventList_, &rc);
                pt = { rc.left + 24, rc.top + 24 };
            }
            showEventContextMenu(pt);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kEventFlushTimerId) {
            flushPendingEvents();
            return 0;
        }
        break;
    case kStatusMessage: {
        auto* statusText = reinterpret_cast<std::wstring*>(lParam);
        if (statusText != nullptr) {
            updateStatusText(*statusText);
            delete statusText;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    case WM_DESTROY:
        ::KillTimer(hwnd_, kEventFlushTimerId);
        controller_.stop();
        return 0;
    case WM_NCDESTROY:
        if (deleteOnDestroy_) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete this;
            return 0;
        }
        break;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EtwMonitorView::createControls() {
    startButton_ = Ksword::Ui::CreateButton(hwnd_, kStartButtonId, L"开始", 12, 12, 78, 28);
    stopButton_ = Ksword::Ui::CreateButton(hwnd_, kStopButtonId, L"停止", 96, 12, 78, 28);
    filterButton_ = Ksword::Ui::CreateButton(hwnd_, kFilterButtonId, L"筛选器...", 180, 12, 96, 28);
    clearButton_ = Ksword::Ui::CreateButton(hwnd_, kClearButtonId, L"清空", 282, 12, 78, 28);
    statusText_ = Ksword::Ui::CreateText(hwnd_, 0, L"ETW 已停止。筛选器在弹窗中配置。", 376, 18, 520, 22);

    eventList_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        12,
        52,
        860,
        400,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    ListView_SetExtendedListViewStyle(eventList_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    eventImageList_ = ImageList_Create(::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON),
        ILC_COLOR32 | ILC_MASK,
        64,
        64);
    if (eventImageList_ != nullptr) {
        ListView_SetImageList(eventList_, eventImageList_, LVSIL_SMALL);
    }
    InsertColumn(eventList_, 0, L"PID", 96);
    InsertColumn(eventList_, 1, L"时间", 160);
    InsertColumn(eventList_, 2, L"Provider", 285);
    InsertColumn(eventList_, 3, L"TID", 70);
    InsertColumn(eventList_, 4, L"EventId", 70);
    InsertColumn(eventList_, 5, L"Level", 60);
    InsertColumn(eventList_, 6, L"摘要", 420);
    ::SendMessageW(eventList_, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);

    controller_.setEventCallback([this](const EtwEvent& eventRow) {
        enqueueEventFromWorker(eventRow);
    });
    controller_.setStatusCallback([this](const std::wstring& text) {
        if (hwnd_ != nullptr) {
            ::PostMessageW(hwnd_, kStatusMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
        }
    });
}

void EtwMonitorView::layout() {
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    ::MoveWindow(statusText_, 376, 17, width - 388, 24, TRUE);
    ::MoveWindow(eventList_, 12, 52, width - 24, height - 64, TRUE);
}

void EtwMonitorView::startSession() {
    if (controller_.running()) {
        return;
    }
    updateStatusText(L"正在启动 ETW...");
    if (!controller_.start(filterModel_.state())) {
        updateStatusText(controller_.lastError());
    }
    updateButtonState();
}

void EtwMonitorView::stopSession() {
    controller_.stop();
    flushPendingEvents();
    updateButtonState();
}

void EtwMonitorView::openFilterDialog() {
    EtwFilterState editedState = filterModel_.state();
    EtwFilterDialog dialog(hwnd_, editedState);
    if (dialog.showModal(editedState)) {
        filterModel_.setState(editedState);
        updateStatusText(controller_.running()
            ? L"筛选器已更新；重新开始 ETW 后对 Provider 生效。"
            : L"筛选器已更新。");
    }
}

void EtwMonitorView::enqueueEventFromWorker(const EtwEvent& eventRow) {
    // enqueueEventFromWorker intentionally does not touch HWND/ListView state:
    // - input: one ETW row on the ETW processing thread;
    // - processing: bounded append to a private queue protected by a mutex;
    // - return: none.  WM_TIMER later performs all UI work on the UI thread.
    std::lock_guard<std::mutex> lock(pendingEventMutex_);
    pendingEvents_.push_back(eventRow);
    while (pendingEvents_.size() > kMaxEventRows) {
        pendingEvents_.pop_front();
    }
}

void EtwMonitorView::flushPendingEvents() {
    // flushPendingEvents is the only high-volume UI update path:
    // - input: queued ETW rows accumulated by worker callbacks;
    // - processing: drain at most kMaxEventsPerFlush rows per timer tick;
    // - return: none.  Remaining rows stay queued for the next timer tick.
    std::vector<EtwEvent> batch;
    {
        std::lock_guard<std::mutex> lock(pendingEventMutex_);
        const std::size_t takeCount = std::min<std::size_t>(pendingEvents_.size(), kMaxEventsPerFlush);
        batch.reserve(takeCount);
        for (std::size_t index = 0; index < takeCount; ++index) {
            batch.push_back(std::move(pendingEvents_.front()));
            pendingEvents_.pop_front();
        }
    }

    if (!batch.empty()) {
        appendEventsToView(batch);
    }
}

void EtwMonitorView::clearPendingEvents() {
    // clearPendingEvents supports the explicit "clear list" command only:
    // - input: none;
    // - processing: discard queued rows that have not reached the ListView;
    // - return: none.  Historical rendered rows are cleared by the caller.
    std::lock_guard<std::mutex> lock(pendingEventMutex_);
    pendingEvents_.clear();
}

void EtwMonitorView::appendEventsToView(const std::vector<EtwEvent>& events) {
    if (events.empty() || eventList_ == nullptr) {
        return;
    }

    const std::size_t modelCountBefore = eventModel_.rowCount();
    for (const EtwEvent& eventRow : events) {
        eventModel_.append(eventRow);
    }
    const std::size_t modelCountAfter = eventModel_.rowCount();
    const std::size_t trimmedCount =
        (modelCountBefore + events.size() > modelCountAfter)
        ? (modelCountBefore + events.size() - modelCountAfter)
        : 0;

    ::SendMessageW(eventList_, WM_SETREDRAW, FALSE, 0);
    for (std::size_t index = 0; index < trimmedCount && ListView_GetItemCount(eventList_) > 0; ++index) {
        ListView_DeleteItem(eventList_, 0);
    }
    for (const EtwEvent& eventRow : events) {
        insertEventRowToList(eventRow);
    }
    ::SendMessageW(eventList_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(eventList_, nullptr, FALSE);
}

void EtwMonitorView::insertEventRowToList(const EtwEvent& eventRow) {
    const int rowIndex = ListView_GetItemCount(eventList_);
    const std::wstring pidText = NumberText(eventRow.processId);
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    item.iItem = rowIndex;
    item.pszText = const_cast<LPWSTR>(pidText.c_str());
    item.iImage = iconIndexForProcessId(eventRow.processId);
    item.lParam = static_cast<LPARAM>(rowIndex);
    ListView_InsertItem(eventList_, &item);

    const std::wstring tidText = NumberText(eventRow.threadId);
    const std::wstring eventIdText = NumberText(eventRow.eventId);
    const std::wstring levelText = NumberText(eventRow.level);
    ListView_SetItemText(eventList_, rowIndex, 1, const_cast<LPWSTR>(eventRow.timeText.c_str()));
    ListView_SetItemText(eventList_, rowIndex, 2, const_cast<LPWSTR>(eventRow.providerText.c_str()));
    ListView_SetItemText(eventList_, rowIndex, 3, const_cast<LPWSTR>(tidText.c_str()));
    ListView_SetItemText(eventList_, rowIndex, 4, const_cast<LPWSTR>(eventIdText.c_str()));
    ListView_SetItemText(eventList_, rowIndex, 5, const_cast<LPWSTR>(levelText.c_str()));
    ListView_SetItemText(eventList_, rowIndex, 6, const_cast<LPWSTR>(eventRow.summary.c_str()));
}

void EtwMonitorView::clearEventList() {
    // clearEventList is used only by explicit user clear actions:
    // - input: none;
    // - processing: delete visible ListView rows with redraw disabled;
    // - return: none.  It never snapshots and reinserts historical events.
    ::SendMessageW(eventList_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(eventList_);
    ::SendMessageW(eventList_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(eventList_, nullptr, FALSE);
}

void EtwMonitorView::updateStatusText(const std::wstring& text) {
    if (statusText_ != nullptr) {
        ::SetWindowTextW(statusText_, text.c_str());
    }
}

void EtwMonitorView::updateButtonState() {
    const BOOL running = controller_.running() ? TRUE : FALSE;
    ::EnableWindow(startButton_, running ? FALSE : TRUE);
    ::EnableWindow(stopButton_, running ? TRUE : FALSE);
}

int EtwMonitorView::iconIndexForProcessId(std::uint32_t processId) {
    if (eventImageList_ == nullptr) {
        return -1;
    }

    const auto cached = processIconCache_.find(processId);
    if (cached != processIconCache_.end()) {
        return cached->second;
    }

    const std::wstring imagePath = processImagePath(processId);
    SHFILEINFOW shellInfo{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    const wchar_t* queryPath = imagePath.empty() ? L".exe" : imagePath.c_str();
    if (imagePath.empty()) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }

    int imageIndex = -1;
    if (::SHGetFileInfoW(queryPath,
            imagePath.empty() ? FILE_ATTRIBUTE_NORMAL : 0,
            &shellInfo,
            sizeof(shellInfo),
            flags) && shellInfo.hIcon != nullptr) {
        imageIndex = ImageList_AddIcon(eventImageList_, shellInfo.hIcon);
        ::DestroyIcon(shellInfo.hIcon);
    }

    processIconCache_[processId] = imageIndex;
    return imageIndex;
}

std::wstring EtwMonitorView::processImagePath(std::uint32_t processId) const {
    if (processId == 0) {
        return {};
    }

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr) {
        return {};
    }

    std::wstring path;
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (::QueryFullProcessImageNameW(process, 0, buffer.data(), &length) && length > 0) {
        path.assign(buffer.data(), buffer.data() + length);
    }
    ::CloseHandle(process);
    return path;
}

void EtwMonitorView::openSelectedEventDetail() {
    EtwEvent eventRow;
    if (!selectedEvent(&eventRow)) {
        return;
    }
    showEventDetail(eventRow);
}

void EtwMonitorView::showEventDetail(const EtwEvent& eventRow) {
    EnsureDetailWindowClass();

    auto* text = new std::wstring(formatEventDetailText(eventRow));
    HWND detailWindow = ::CreateWindowExW(WS_EX_APPWINDOW,
        kEtwEventDetailClass,
        L"ETW 监控项详细信息",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        720,
        520,
        hwnd_,
        nullptr,
        ::GetModuleHandleW(nullptr),
        text);
    if (detailWindow == nullptr) {
        delete text;
        updateStatusText(L"创建监控项详细信息窗口失败。");
        return;
    }

    ::ShowWindow(detailWindow, SW_SHOWNORMAL);
    ::UpdateWindow(detailWindow);
    updateStatusText(L"已打开监控项详细信息。");
}

int EtwMonitorView::selectedEventIndex() const {
    if (eventList_ == nullptr) {
        return -1;
    }
    return ListView_GetNextItem(eventList_, -1, LVNI_SELECTED);
}

bool EtwMonitorView::selectedEvent(EtwEvent* eventRow) const {
    const int selected = selectedEventIndex();
    if (selected < 0) {
        const_cast<EtwMonitorView*>(this)->updateStatusText(L"没有选中监控项。");
        return false;
    }
    const std::vector<EtwEvent> rows = eventModel_.snapshot();
    if (selected >= static_cast<int>(rows.size())) {
        const_cast<EtwMonitorView*>(this)->updateStatusText(L"监控项索引已过期，请刷新列表后重试。");
        return false;
    }
    if (eventRow) {
        *eventRow = rows[static_cast<std::size_t>(selected)];
    }
    return true;
}

std::wstring EtwMonitorView::formatEventDetailText(const EtwEvent& eventRow) const {
    std::wostringstream detail;
    detail << L"时间: " << eventRow.timeText << L"\r\n"
           << L"Provider: " << eventRow.providerText << L"\r\n"
           << L"PID: " << eventRow.processId << L"\r\n"
           << L"TID: " << eventRow.threadId << L"\r\n"
           << L"EventId: " << eventRow.eventId << L"\r\n"
           << L"Version: " << static_cast<unsigned int>(eventRow.version) << L"\r\n"
           << L"Level: " << static_cast<unsigned int>(eventRow.level) << L"\r\n"
           << L"Opcode: " << static_cast<unsigned int>(eventRow.opcode) << L"\r\n"
           << L"Task: " << eventRow.task << L"\r\n"
           << L"Keyword: 0x" << std::hex << std::uppercase << eventRow.keyword << std::dec << L"\r\n\r\n"
           << L"摘要:\r\n" << eventRow.summary << L"\r\n";
    return detail.str();
}

void EtwMonitorView::copySelectedEventRow() {
    EtwEvent eventRow;
    if (!selectedEvent(&eventRow)) {
        return;
    }
    std::wostringstream row;
    row << eventRow.timeText << L'\t'
        << eventRow.providerText << L'\t'
        << eventRow.processId << L'\t'
        << eventRow.threadId << L'\t'
        << eventRow.eventId << L'\t'
        << static_cast<unsigned int>(eventRow.level) << L'\t'
        << eventRow.summary;
    updateStatusText(CopyTextToClipboard(hwnd_, row.str()) ? L"已复制 ETW 行。" : L"复制 ETW 行失败。");
}

void EtwMonitorView::copySelectedEventDetail() {
    EtwEvent eventRow;
    if (!selectedEvent(&eventRow)) {
        return;
    }
    updateStatusText(CopyTextToClipboard(hwnd_, formatEventDetailText(eventRow)) ? L"已复制 ETW 详情。" : L"复制 ETW 详情失败。");
}

void EtwMonitorView::showEventContextMenu(POINT screenPoint) {
    // showEventContextMenu keeps the ETW-only monitor page compact by grouping
    // event actions and session actions under submenus. Inputs are a screen
    // coordinate from mouse or keyboard context-menu invocation; processing
    // updates the selected row, creates transient HMENU objects, then dispatches
    // the returned command. There is no return value.
    if (eventList_ == nullptr) {
        return;
    }
    POINT clientPoint = screenPoint;
    ::ScreenToClient(eventList_, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int hitRow = ListView_HitTest(eventList_, &hit);
    if (hitRow >= 0) {
        ListView_SetItemState(eventList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(eventList_, hitRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    const bool hasSelection = selectedEventIndex() >= 0;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    HMENU eventMenu = ::CreatePopupMenu();
    if (eventMenu) {
        ::AppendMenuW(eventMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kEtwMenuDetail, L"详细信息");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(eventMenu), L"事件");
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kEtwMenuCopyRow, L"复制当前行");
        ::AppendMenuW(copyMenu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kEtwMenuCopyDetail, L"复制详情");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }
    HMENU sessionMenu = ::CreatePopupMenu();
    if (sessionMenu) {
        ::AppendMenuW(sessionMenu, MF_STRING | (controller_.running() ? MF_GRAYED : 0U), kEtwMenuStart, L"开始");
        ::AppendMenuW(sessionMenu, MF_STRING | (controller_.running() ? 0U : MF_GRAYED), kEtwMenuStop, L"停止");
        ::AppendMenuW(sessionMenu, MF_STRING, kEtwMenuFilter, L"筛选器...");
        ::AppendMenuW(sessionMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(sessionMenu, MF_STRING, kEtwMenuClear, L"清空列表");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sessionMenu), L"ETW 会话");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        hwnd_,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kEtwMenuDetail:
        openSelectedEventDetail();
        break;
    case kEtwMenuCopyRow:
        copySelectedEventRow();
        break;
    case kEtwMenuCopyDetail:
        copySelectedEventDetail();
        break;
    case kEtwMenuClear:
        clearPendingEvents();
        eventModel_.clear();
        clearEventList();
        updateStatusText(L"ETW 列表已清空。");
        break;
    case kEtwMenuStart:
        startSession();
        break;
    case kEtwMenuStop:
        stopSession();
        break;
    case kEtwMenuFilter:
        openFilterDialog();
        break;
    default:
        break;
    }
}

HWND CreateEtwMonitorPage(HWND parent, const RECT& bounds) {
    auto* view = new EtwMonitorView();
    if (!view->create(parent, bounds)) {
        delete view;
        return nullptr;
    }
    view->setDeleteOnDestroy(true);
    return view->hwnd();
}

} // namespace Ksword::Features::Monitor
