#include "../../Core/Win32Lean.h"
#include "../../Ui/FilterBar.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <commctrl.h>

#include "ProcessDetailPage.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::ProcessDetail {
namespace {

constexpr UINT kThreadCopyCellCommand = 64101;
constexpr UINT kThreadCopyRowCommand = 64102;
constexpr UINT kThreadCopyAllCommand = 64103;
constexpr UINT kThreadShowDetailCommand = 64104;
constexpr UINT kThreadSuspendCommand = 64105;
constexpr UINT kThreadResumeCommand = 64106;
constexpr UINT kThreadTerminateCommand = 64107;
constexpr UINT kThreadR0TerminateCommand = 64108;

constexpr UINT_PTR kThreadLayoutSubclassId = 0x54485244U; // "THRD"
constexpr UINT_PTR kThreadLayoutTimerId = 0x54485245U;

struct ThreadLayoutState {
    HWND group = nullptr;
    HWND refresh = nullptr;
    HWND sample = nullptr;
    HWND stack = nullptr;
    HWND status = nullptr;
    HWND filter = nullptr;
    HWND list = nullptr;
    HWND output = nullptr;
};

int ClientWidth(HWND hwnd) {
    RECT client{};
    return hwnd && ::GetClientRect(hwnd, &client)
        ? std::max(0L, client.right - client.left)
        : 0;
}

int ClientHeight(HWND hwnd) {
    RECT client{};
    return hwnd && ::GetClientRect(hwnd, &client)
        ? std::max(0L, client.bottom - client.top)
        : 0;
}

void MoveControl(HWND hwnd, int x, int y, int width, int height) {
    if (hwnd) {
        ::MoveWindow(hwnd, x, y, std::max(0, width), std::max(0, height), TRUE);
    }
}

void LayoutThreadControls(HWND page, const ThreadLayoutState& state) {
    const int width = ClientWidth(page);
    const int height = ClientHeight(page);
    if (width <= 0 || height <= 0) {
        return;
    }

    constexpr int outerMargin = 6;
    constexpr int innerMargin = 8;
    constexpr int spacing = 6;
    constexpr int toolbarY = 25;
    constexpr int toolbarHeight = 28;
    constexpr int filterY = 59;
    constexpr int filterHeight = 26;
    constexpr int listY = filterY + filterHeight + spacing;
    constexpr int preferredOutputHeight = 220;
    constexpr int minimumOutputHeight = 90;
    constexpr int minimumListHeight = 80;

    MoveControl(
        state.group,
        outerMargin,
        outerMargin,
        width - outerMargin * 2,
        height - outerMargin * 2);

    int x = outerMargin + innerMargin;
    MoveControl(state.refresh, x, toolbarY, 92, toolbarHeight);
    x += 92 + spacing;
    MoveControl(state.sample, x, toolbarY, 112, toolbarHeight);
    x += 112 + spacing;
    MoveControl(state.stack, x, toolbarY, 108, toolbarHeight);
    x += 108 + spacing;
    MoveControl(state.status, x, toolbarY, width - x - outerMargin - innerMargin, toolbarHeight);
    MoveControl(state.filter, outerMargin + innerMargin, filterY, width - (outerMargin + innerMargin) * 2, filterHeight);

    const int contentBottom = height - outerMargin - innerMargin;
    const int availableBelowToolbar = std::max(0, contentBottom - listY);
    int outputHeight = std::min(preferredOutputHeight, std::max(minimumOutputHeight, availableBelowToolbar / 3));
    if (availableBelowToolbar < minimumListHeight + spacing + minimumOutputHeight) {
        outputHeight = std::max(48, availableBelowToolbar - minimumListHeight - spacing);
    }
    const int outputY = std::max(listY + minimumListHeight + spacing, contentBottom - outputHeight);
    const int listHeight = std::max(0, outputY - spacing - listY);
    const int contentWidth = width - (outerMargin + innerMargin) * 2;
    MoveControl(state.list, outerMargin + innerMargin, listY, contentWidth, listHeight);
    MoveControl(state.output, outerMargin + innerMargin, outputY, contentWidth, contentBottom - outputY);
}

LRESULT CALLBACK ThreadLayoutSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<ThreadLayoutState*>(referenceData);
    if (!state) {
        return ::DefSubclassProc(hwnd, message, wParam, lParam);
    }

    if (message == WM_SIZE) {
        // The common page subclass applies the frozen placement table first.
        // Re-run the thread-specific bottom anchoring after that pass completes.
        const LRESULT result = ::DefSubclassProc(hwnd, message, wParam, lParam);
        ::SetTimer(hwnd, kThreadLayoutTimerId, 1, nullptr);
        return result;
    }
    if (message == WM_TIMER && wParam == kThreadLayoutTimerId) {
        ::KillTimer(hwnd, kThreadLayoutTimerId);
        LayoutThreadControls(hwnd, *state);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        ::KillTimer(hwnd, kThreadLayoutTimerId);
        ::RemoveWindowSubclass(hwnd, ThreadLayoutSubclassProc, subclassId);
        delete state;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

std::wstring DecimalText(std::uint64_t value) {
    return std::to_wstring(value);
}

std::wstring SignedDecimalText(LONG value) {
    return std::to_wstring(static_cast<long long>(value));
}

std::wstring HexAddressText(std::uintptr_t value) {
    if (value == 0) {
        return L"-";
    }
    std::wostringstream text;
    text << L"0x" << std::hex << std::uppercase << value;
    return text.str();
}

std::wstring Win32ErrorText(const wchar_t* operation, DWORD error) {
    wchar_t* message = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstring result = operation ? operation : L"Win32";
    result += L" 失败 (" + std::to_wstring(error) + L")";
    if (length != 0 && message) {
        std::wstring detail(message, length);
        while (!detail.empty() &&
               (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' ')) {
            detail.pop_back();
        }
        if (!detail.empty()) {
            result += L": " + detail;
        }
    }
    if (message) {
        ::LocalFree(message);
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int required = ::MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (required <= 0) {
        return { text.begin(), text.end() };
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        required);
    return result;
}

std::vector<Ksword::Ui::VirtualListRow> BuildThreadVirtualRows(const std::vector<ProcessThreadInfo>& threads) {
    std::vector<Ksword::Ui::VirtualListRow> rows;
    rows.reserve(threads.size());
    for (std::size_t index = 0; index < threads.size(); ++index) {
        const ProcessThreadInfo& thread = threads[index];
        Ksword::Ui::VirtualListRow row{};
        row.stableKey = DecimalText(thread.threadId) + L"\n" + HexAddressText(thread.startAddress);
        row.itemData = static_cast<LPARAM>(index + 1);
        row.cells = {
            DecimalText(thread.threadId),
            thread.statusText.empty() ? L"-" : thread.statusText,
            SignedDecimalText(thread.basePriority),
            L"-",
            HexAddressText(thread.startAddress),
            L"Unavailable",
            L"Unavailable",
            L"Unavailable",
            L"U:Unavailable | K:Unavailable | Unavailable",
            L"Unavailable"
        };
        rows.push_back(std::move(row));
    }
    return rows;
}

std::wstring StableKeyAt(const Ksword::Ui::VirtualListView& list, int visibleIndex) {
    if (visibleIndex < 0 || static_cast<std::size_t>(visibleIndex) >= list.visibleIndexes().size()) {
        return {};
    }
    const std::size_t rowIndex = list.visibleIndexes()[static_cast<std::size_t>(visibleIndex)];
    return rowIndex < list.rows().size() ? list.rows()[rowIndex].stableKey : std::wstring{};
}

void RestoreListPosition(
    HWND list,
    const Ksword::Ui::VirtualListView& virtualList,
    const std::wstring& selectedKey,
    const std::wstring& topKey) {
    int selectedIndex = -1;
    int topIndex = -1;
    for (std::size_t index = 0; index < virtualList.visibleIndexes().size(); ++index) {
        const std::size_t rowIndex = virtualList.visibleIndexes()[index];
        if (rowIndex >= virtualList.rows().size()) {
            continue;
        }
        const std::wstring& stableKey = virtualList.rows()[rowIndex].stableKey;
        if (!selectedKey.empty() && stableKey == selectedKey) {
            selectedIndex = static_cast<int>(index);
        }
        if (!topKey.empty() && stableKey == topKey) {
            topIndex = static_cast<int>(index);
        }
    }
    if (selectedIndex >= 0) {
        ListView_SetItemState(list, selectedIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (topIndex >= 0) {
        ListView_EnsureVisible(list, topIndex, FALSE);
    }
}

bool SelectedItemData(HWND list, std::size_t& indexOut) {
    indexOut = 0;
    if (!list) {
        return false;
    }
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) {
        return false;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(list, &item) || item.lParam <= 0) {
        return false;
    }
    indexOut = static_cast<std::size_t>(item.lParam - 1);
    return true;
}

} // namespace

bool ProcessDetailPage::CreateThreadTab() {
    const TabIndex tab = TabIndex::Threads;
    HWND group = AddGroup(tab, L"线程枚举与上下文摘要", 6, 6, -6, -6);
    HWND refresh = AddButton(tab, ThreadRefresh, L"刷新线程", 14, 25, 92, 28);
    HWND sample = AddButton(tab, ThreadSample, L"采样PDB字段", 112, 25, 112, 28);
    HWND stack = AddButton(tab, ThreadStack, L"查看调用栈", 230, 25, 108, 28);
    HWND status = AddLabel(tab, ThreadStatus, L"● 尚未刷新", 344, 25, -14, 28);
    HWND page = pages_[static_cast<std::size_t>(tab)].hwnd;
    HWND filter = Ksword::Ui::CreateFilterBar(page, ThreadFilter, L"筛选线程字段与 R0 详情", 14, 59, 100, 26);
    if (filter) {
        pages_[static_cast<std::size_t>(tab)].placements.push_back(Placement{ filter, 14, 59, -14, 26 });
    }
    HWND list = AddVirtualList(tab, ThreadList, 14, 91, -14, -244, threadVirtualList_);
    HWND output = AddEdit(
        tab,
        ThreadRuntimeOutput,
        L"选择线程行后可查看 runtime detail；当前快照未提供的字段将明确显示为 Unavailable。",
        true,
        true,
        14,
        430,
        -14,
        -14);

    if (!group || !refresh || !sample || !stack || !status || !filter || !list || !output) {
        return false;
    }

    AddListColumn(list, 0, L"ThreadID", 96);
    AddListColumn(list, 1, L"状态", 82);
    AddListColumn(list, 2, L"优先级", 72);
    AddListColumn(list, 3, L"上下文切换", 96);
    AddListColumn(list, 4, L"起始地址", 130);
    AddListColumn(list, 5, L"TEB地址", 130);
    AddListColumn(list, 6, L"亲和性", 108);
    AddListColumn(list, 7, L"寄存器", 100);
    AddListColumn(list, 8, L"R0栈边界", 260);
    AddListColumn(list, 9, L"R0详情", 360);
    listColumnCounts_[list] = 10;
    if (threadFilterRows_) {
        threadVirtualList_.setSharedRows(threadFilterRows_);
        threadVirtualList_.setVisibleIndexes(threadVisibleIndexes_);
    }

    auto* layoutState = new ThreadLayoutState{
        group,
        refresh,
        sample,
        stack,
        status,
        filter,
        list,
        output
    };
    if (!page || !::SetWindowSubclass(
            page,
            ThreadLayoutSubclassProc,
            kThreadLayoutSubclassId,
            reinterpret_cast<DWORD_PTR>(layoutState))) {
        delete layoutState;
        return false;
    }
    LayoutThreadControls(page, *layoutState);
    return true;
}

void ProcessDetailPage::PopulateThreadTab() {
    HWND list = Control(TabIndex::Threads, ThreadList);
    if (!list) {
        return;
    }
    if (threadFilterRows_) {
        threadVirtualList_.setSharedRows(threadFilterRows_);
        threadVirtualList_.setVisibleIndexes(threadVisibleIndexes_);
    }
    if (pendingThreadEntries_ || !threadFilterRows_) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 正在后台准备线程表...");
    }
}

void ProcessDetailPage::RequestThreadFilter(bool rebuildRows) {
    if (!threadFilterTask_) {
        return;
    }
    const HWND filter = Control(TabIndex::Threads, ThreadFilter);
    threadFilterQuery_ = filter ? Ksword::Ui::GetFilterBarText(filter) : threadFilterQuery_;
    const auto existingRows = threadFilterRows_;
    const auto source = pendingThreadEntries_ ? pendingThreadEntries_ : threadEntries_;
    // A newer collector snapshot remains pending until its display rows are
    // installed. Any coalesced filter request must therefore rebuild from the
    // pending immutable source instead of applying the new query to old rows.
    const bool buildRows = rebuildRows || !existingRows || pendingThreadEntries_;
    if (!source && buildRows) {
        return;
    }
    const std::uint64_t generation = threadSourceGeneration_;
    SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 正在后台筛选线程表...");
    threadFilterTask_->request(
        [source, existingRows, buildRows, generation, query = threadFilterQuery_]() mutable {
            DetailTableFilterResult result{};
            result.sourceGeneration = generation;
            result.query = std::move(query);
            result.rows = buildRows
                ? std::make_shared<const std::vector<Ksword::Ui::VirtualListRow>>(BuildThreadVirtualRows(*source))
                : existingRows;
            if (result.rows) {
                result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*result.rows, result.query);
            }
            return result;
        },
        [this](std::uint64_t, std::optional<DetailTableFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value() || result->sourceGeneration != threadSourceGeneration_ ||
                result->query != threadFilterQuery_ || !result->rows) {
                return;
            }
            const HWND list = threadVirtualList_.hwnd();
            const std::wstring selectedKey = ::IsWindow(list)
                ? StableKeyAt(threadVirtualList_, ListView_GetNextItem(list, -1, LVNI_SELECTED))
                : std::wstring{};
            const std::wstring topKey = ::IsWindow(list)
                ? StableKeyAt(threadVirtualList_, ListView_GetTopIndex(list))
                : std::wstring{};
            const bool replaceRows = result->rows != threadFilterRows_;
            threadFilterRows_ = result->rows;
            threadVisibleIndexes_ = result->visibleIndexes;
            if (replaceRows) {
                threadEntries_ = pendingThreadEntries_ ? pendingThreadEntries_ : threadEntries_;
                pendingThreadEntries_.reset();
            }
            if (::IsWindow(list)) {
                if (replaceRows) {
                    threadVirtualList_.setSharedRows(threadFilterRows_);
                }
                threadVirtualList_.setVisibleIndexes(std::move(result->visibleIndexes));
                RestoreListPosition(list, threadVirtualList_, selectedKey, topKey);
            }
            if (snapshot_.threadsSucceeded) {
                SetPageStatus(
                    TabIndex::Threads,
                    ThreadStatus,
                    L"● 刷新完成 | 线程 " + DecimalText(threadVirtualList_.rowCount()) +
                        L" / " + DecimalText(ThreadEntries().size()));
            } else {
                std::wstring status = L"● 线程刷新失败";
                if (!snapshot_.errorText.empty()) {
                    status += L" | " + snapshot_.errorText;
                }
                SetPageStatus(TabIndex::Threads, ThreadStatus, status);
            }
        });
}

bool ProcessDetailPage::HandleThreadCommand(int controlId) {
    switch (controlId) {
    case ThreadRefresh:
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 正在后台刷新线程细节...");
        RefreshAll();
        return true;

    case ThreadFilter:
        RequestThreadFilter(false);
        return true;

    case ThreadSample: {
        HWND list = Control(TabIndex::Threads, ThreadList);
        std::size_t index = 0;
        const auto& threads = ThreadEntries();
        if (!SelectedItemData(list, index) || index >= threads.size()) {
            SetControlText(TabIndex::Threads, ThreadRuntimeOutput, L"请先在线程表中选择一条线程记录。");
            SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 请先选择一个线程。");
            return true;
        }
        const ProcessThreadInfo& thread = threads[index];
        std::wostringstream text;
        text << L"[Selected Thread Runtime Context]\r\n"
             << L"TID/PID: " << thread.threadId << L"/" << thread.ownerProcessId << L"\r\n"
             << L"Start/Win32Start: " << HexAddressText(thread.startAddress) << L" / Unavailable\r\n"
             << L"TEB: Unavailable\r\n"
             << L"R0 fixed detail: Unavailable\r\n\r\n"
             << L"当前 Light 快照未包含 PDB deep runtime 字段，未执行伪造采样。";
        SetControlText(TabIndex::Threads, ThreadRuntimeOutput, text.str());
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 当前线程 PDB 字段不可用");
        return true;
    }

    case ThreadStack:
        ShowSelectedThreadSummary();
        return true;

    default:
        return false;
    }
}

bool ProcessDetailPage::HandleThreadContextMenu(POINT screenPoint) {
    HWND list = Control(TabIndex::Threads, ThreadList);
    if (!list || SelectedListRow(list) < 0) {
        return true;
    }

    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return true;
    }
    ::AppendMenuW(menu, MF_STRING, kThreadCopyCellCommand, L"复制当前单元格");
    ::AppendMenuW(menu, MF_STRING, kThreadCopyRowCommand, L"复制当前行");
    ::AppendMenuW(menu, MF_STRING, kThreadCopyAllCommand, L"复制全部");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kThreadShowDetailCommand, L"线程详细信息");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kThreadSuspendCommand, L"挂起线程");
    ::AppendMenuW(menu, MF_STRING, kThreadResumeCommand, L"恢复线程");
    ::AppendMenuW(menu, MF_STRING, kThreadTerminateCommand, L"终止线程");
    ::AppendMenuW(menu, MF_STRING, kThreadR0TerminateCommand, L"R0结束线程");

    const UINT command = ::TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        hwnd_,
        nullptr);
    ::DestroyMenu(menu);

    switch (command) {
    case kThreadCopyCellCommand: CopyListCell(list); break;
    case kThreadCopyRowCommand: CopyListRow(list); break;
    case kThreadCopyAllCommand: CopyListAll(list); break;
    case kThreadShowDetailCommand: ShowSelectedThreadSummary(); break;
    case kThreadSuspendCommand: SuspendSelectedThread(); break;
    case kThreadResumeCommand: ResumeSelectedThread(); break;
    case kThreadTerminateCommand: TerminateSelectedThread(); break;
    case kThreadR0TerminateCommand: TerminateSelectedThreadByR0(); break;
    default: break;
    }
    return true;
}

void ProcessDetailPage::SuspendSelectedThread() {
    HWND list = Control(TabIndex::Threads, ThreadList);
    std::size_t index = 0;
    const auto& threads = ThreadEntries();
    if (!SelectedItemData(list, index) || index >= threads.size()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 没有选中线程。");
        return;
    }
    const DWORD threadId = threads[index].threadId;
    if (threadId == ::GetCurrentThreadId()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 拒绝挂起当前 Light UI 线程。");
        return;
    }

    ExecuteBackgroundAction(
        TabIndex::Threads,
        ThreadStatus,
        L"● 正在后台挂起线程 " + DecimalText(threadId) + L"…",
        [threadId] {
            ProcessDetailActionResult result{};
            HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadId);
            if (!thread) {
                result.statusText = L"● " + Win32ErrorText(L"OpenThread", ::GetLastError());
                return result;
            }
            const DWORD previousCount = ::SuspendThread(thread);
            const DWORD error = previousCount == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
            ::CloseHandle(thread);
            if (error != ERROR_SUCCESS) {
                result.statusText = L"● " + Win32ErrorText(L"SuspendThread", error);
                return result;
            }
            result.refreshRequired = true;
            result.statusText = L"● 已挂起线程 " + DecimalText(threadId) +
                L"（原挂起计数 " + DecimalText(previousCount) + L"）";
            return result;
        });
}

void ProcessDetailPage::ResumeSelectedThread() {
    HWND list = Control(TabIndex::Threads, ThreadList);
    std::size_t index = 0;
    const auto& threads = ThreadEntries();
    if (!SelectedItemData(list, index) || index >= threads.size()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 没有选中线程。");
        return;
    }
    const DWORD threadId = threads[index].threadId;
    ExecuteBackgroundAction(
        TabIndex::Threads,
        ThreadStatus,
        L"● 正在后台恢复线程 " + DecimalText(threadId) + L"…",
        [threadId] {
            ProcessDetailActionResult result{};
            HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadId);
            if (!thread) {
                result.statusText = L"● " + Win32ErrorText(L"OpenThread", ::GetLastError());
                return result;
            }
            const DWORD previousCount = ::ResumeThread(thread);
            const DWORD error = previousCount == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
            ::CloseHandle(thread);
            if (error != ERROR_SUCCESS) {
                result.statusText = L"● " + Win32ErrorText(L"ResumeThread", error);
                return result;
            }
            result.refreshRequired = true;
            result.statusText = L"● 已恢复线程 " + DecimalText(threadId) +
                L"（原挂起计数 " + DecimalText(previousCount) + L"）";
            return result;
        });
}

void ProcessDetailPage::TerminateSelectedThread() {
    HWND list = Control(TabIndex::Threads, ThreadList);
    std::size_t index = 0;
    const auto& threads = ThreadEntries();
    if (!SelectedItemData(list, index) || index >= threads.size()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 没有选中线程。");
        return;
    }
    const DWORD threadId = threads[index].threadId;
    if (threadId == ::GetCurrentThreadId()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 拒绝终止当前 Light UI 线程。");
        return;
    }
    const int answer = ::MessageBoxW(
        hwnd_,
        L"确定要终止选中的线程吗？这可能导致目标进程崩溃。",
        L"终止线程",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 已取消终止线程。");
        return;
    }

    ExecuteBackgroundAction(
        TabIndex::Threads,
        ThreadStatus,
        L"● 正在后台终止线程 " + DecimalText(threadId) + L"…",
        [threadId] {
            ProcessDetailActionResult result{};
            HANDLE thread = ::OpenThread(THREAD_TERMINATE, FALSE, threadId);
            if (!thread) {
                result.statusText = L"● " + Win32ErrorText(L"OpenThread", ::GetLastError());
                return result;
            }
            const BOOL terminated = ::TerminateThread(thread, 1);
            const DWORD error = terminated ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(thread);
            if (!terminated) {
                result.statusText = L"● " + Win32ErrorText(L"TerminateThread", error);
                return result;
            }
            result.refreshRequired = true;
            result.statusText = L"● 已请求终止线程 " + DecimalText(threadId);
            return result;
        });
}

void ProcessDetailPage::TerminateSelectedThreadByR0() {
    HWND list = Control(TabIndex::Threads, ThreadList);
    std::size_t index = 0;
    const auto& threads = ThreadEntries();
    if (!SelectedItemData(list, index) || index >= threads.size()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 没有选中线程。");
        return;
    }

    const DWORD threadId = threads[index].threadId;
    const DWORD processId = threads[index].ownerProcessId;
    if (threadId == 0 || processId == 0 || processId <= 4) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 拒绝结束系统或无效 PID 的线程。");
        return;
    }
    if (threadId == ::GetCurrentThreadId()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 拒绝终止当前 Light UI 线程。");
        return;
    }
    const std::wstring confirmationText =
        L"将通过 R0 结束 PID " + DecimalText(processId) +
        L" 的线程 " + DecimalText(threadId) + L"。该操作不可撤销，是否继续？";
    const int answer = ::MessageBoxW(
        hwnd_,
        confirmationText.c_str(),
        L"R0结束线程",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 已取消 R0 结束线程。");
        return;
    }

    ExecuteBackgroundAction(
        TabIndex::Threads,
        ThreadStatus,
        L"● 正在后台通过 R0 结束线程 " + DecimalText(threadId) + L"…",
        [threadId, processId] {
            ProcessDetailActionResult action{};
            const ksword::ark::IoResult result = ksword::ark::DriverClient().terminateThread(
                threadId,
                processId,
                static_cast<long>(0xC0000005u));
            if (!result.ok) {
                action.statusText = L"● R0结束线程失败 | " + Utf8ToWide(result.message);
                return action;
            }
            action.refreshRequired = true;
            action.statusText = L"● R0 已请求结束线程 " + DecimalText(threadId) + L"。";
            return action;
        });
}

void ProcessDetailPage::ShowSelectedThreadSummary() {
    HWND list = Control(TabIndex::Threads, ThreadList);
    std::size_t index = 0;
    const auto& threads = ThreadEntries();
    if (!SelectedItemData(list, index) || index >= threads.size()) {
        SetPageStatus(TabIndex::Threads, ThreadStatus, L"● 没有选中线程。");
        SetControlText(TabIndex::Threads, ThreadRuntimeOutput, L"请选择一条线程记录查看 runtime detail。");
        return;
    }

    const ProcessThreadInfo& thread = threads[index];
    std::wostringstream detail;
    detail << L"[Thread Runtime Detail]\r\n"
           << L"TID/PID: " << thread.threadId << L"/" << thread.ownerProcessId << L"\r\n"
           << L"State: " << (thread.statusText.empty() ? L"-" : thread.statusText) << L"\r\n"
           << L"Priority: " << thread.basePriority << L" (Delta " << thread.deltaPriority << L")\r\n"
           << L"Context switches: Unavailable\r\n"
           << L"Start/Win32Start: " << HexAddressText(thread.startAddress) << L" / Unavailable\r\n"
           << L"TEB: Unavailable\r\n"
           << L"Affinity: Unavailable\r\n"
           << L"Registers: Unavailable\r\n"
           << L"User/R0 stack: Unavailable\r\n"
           << L"R0 detail: Unavailable\r\n\r\n"
           << L"当前快照没有调用栈帧数据，调用栈功能暂不可用。";
    const std::wstring detailText = detail.str();
    SetControlText(TabIndex::Threads, ThreadRuntimeOutput, detailText);
    ::MessageBoxW(hwnd_, detailText.c_str(), L"线程详细信息", MB_OK | MB_ICONINFORMATION);
    SetPageStatus(
        TabIndex::Threads,
        ThreadStatus,
        L"● 已显示线程 " + DecimalText(thread.threadId) + L" 的详细信息");
}

} // namespace Ksword::Features::ProcessDetail
