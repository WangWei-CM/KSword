#include "DriverDebugOutputView.h"

#include "DriverActions.h"
#include "DriverModel.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <cwctype>
#include <cwchar>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

constexpr wchar_t kDriverDebugOutputClass[] = L"KswordARKLight.DriverDebugOutputView";
constexpr int kStartButtonId = 64301;
constexpr int kStopButtonId = 64302;
constexpr int kClearButtonId = 64303;
constexpr int kQueryButtonId = 64304;
constexpr int kFilterBarId = 64305;
constexpr int kListId = 64306;
constexpr UINT_PTR kDrainTimerId = 64307;
constexpr UINT kDrainIntervalMs = 150;
constexpr std::size_t kMaximumRecords = 2000;
constexpr UINT kMsgControlCompleted = WM_APP + 600;
constexpr UINT kMsgDrainCompleted = WM_APP + 601;
constexpr UINT kMsgFilterCompleted = WM_APP + 602;
constexpr UINT kMenuCopyCell = 64351;
constexpr UINT kMenuCopyRow = 64352;
constexpr UINT kMenuCopyVisible = 64353;

struct DebugRecordRow {
    std::uint64_t sequence = 0;
    std::uint64_t interruptTime100ns = 0;
    std::uint32_t componentId = 0;
    std::uint32_t level = 0;
    std::uint32_t flags = 0;
    std::vector<std::wstring> cells;
};

struct ControlSnapshot {
    std::uint64_t requestId = 0;
    std::uint64_t captureSession = 0;
    unsigned long action = 0;
    ksword::ark::DebugOutputControlResult result;
};

struct DrainSnapshot {
    std::uint64_t captureSession = 0;
    ksword::ark::DebugOutputDrainResult result;
    std::vector<std::shared_ptr<const DebugRecordRow>> rows;
};

struct FilterSnapshot {
    std::uint64_t generation = 0;
    std::wstring query;
    bool useRegex = false;
    std::uint64_t captureSession = 0;
    std::uint64_t throughSequence = 0;
    std::vector<std::shared_ptr<const DebugRecordRow>> matches;
};

struct DriverDebugOutputViewState {
    HWND hwnd = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND clearButton = nullptr;
    HWND queryButton = nullptr;
    HWND filterBar = nullptr;
    HWND statusText = nullptr;
    HWND listView = nullptr;
    Ksword::Ui::VirtualListView virtualList;
    std::deque<std::shared_ptr<const DebugRecordRow>> records;
    std::vector<std::shared_ptr<const DebugRecordRow>> visibleRows;
    std::uint64_t filteredThrough = 0;
    std::uint64_t filterSession = 0;
    std::uint64_t appliedFilterGeneration = 0;
    bool projectionDirty = false;
    std::wstring deferredSelectedKey;
    std::wstring deferredTopKey;
    std::wstring filterQuery;
    bool filterUseRegex = false;
    std::uint64_t snapshotGeneration = 0;
    std::uint64_t controlRequestId = 0;
    std::uint64_t captureSession = 0;
    std::uint64_t nextSequence = 0;
    int contextColumn = 0;
    bool capturing = false;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ControlSnapshot>> controlTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<DrainSnapshot>> drainTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<FilterSnapshot>> filterTask;
};

DriverDebugOutputViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<DriverDebugOutputViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int sourceLength = static_cast<int>(std::min<std::size_t>(value.size(), static_cast<std::size_t>(INT_MAX)));
    int targetLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0);
    if (targetLength <= 0) {
        targetLength = ::MultiByteToWideChar(CP_ACP, 0, value.data(), sourceLength, nullptr, 0);
    }
    if (targetLength <= 0) {
        return L"<无法解码的调试输出>";
    }
    std::wstring converted(static_cast<std::size_t>(targetLength), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, converted.data(), targetLength) <= 0) {
        ::MultiByteToWideChar(CP_ACP, 0, value.data(), sourceLength, converted.data(), targetLength);
    }
    return converted;
}

std::wstring Hex32(const std::uint32_t value) {
    wchar_t buffer[16]{};
    std::swprintf(buffer, std::size(buffer), L"0x%08X", static_cast<unsigned int>(value));
    return buffer;
}

std::wstring DebugStableKey(const DebugRecordRow& row) {
    return std::to_wstring(row.sequence);
}

const std::vector<std::wstring>& DebugCells(const DebugRecordRow& row) {
    return row.cells;
}

bool MatchesDebugRecord(const DebugRecordRow& row, const std::wstring& query, const std::wregex* pattern) {
    if (query.empty()) return true;
    for (const auto& cell : row.cells) {
        if (pattern ? std::regex_search(cell, *pattern) :
            std::search(cell.begin(), cell.end(), query.begin(), query.end(), [](wchar_t a, wchar_t b) {
                return std::towlower(a) == std::towlower(b);
            }) != cell.end()) return true;
    }
    return false;
}

std::wstring StableKeyAt(const DriverDebugOutputViewState& state, const int visibleIndex) {
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(state.visibleRows.size())) {
        return {};
    }
    return DebugStableKey(*state.visibleRows[static_cast<std::size_t>(visibleIndex)]);
}

void RestoreListPosition(DriverDebugOutputViewState& state, const std::wstring& selectedKey, const std::wstring& topKey) {
    const auto findKey = [&state](const std::wstring& key) {
        if (key.empty()) return -1;
        wchar_t* end = nullptr;
        const auto sequence = std::wcstoull(key.c_str(), &end, 10);
        if (end == key.c_str() || *end != L'\0') return -1;
        const auto found = std::lower_bound(state.visibleRows.begin(), state.visibleRows.end(), sequence,
            [](const auto& row, const auto value) { return row->sequence < value; });
        return found != state.visibleRows.end() && (*found)->sequence == sequence
            ? static_cast<int>(found - state.visibleRows.begin()) : -1;
    };
    const int selectedIndex = findKey(selectedKey);
    const int topIndex = findKey(topKey);
    if (selectedIndex >= 0) {
        ListView_SetItemState(state.listView, selectedIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (topIndex >= 0) {
        ListView_EnsureVisible(state.listView, topIndex, FALSE);
    }
}

void SetStatus(DriverDebugOutputViewState& state, const std::wstring& text) {
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, text.c_str());
    }
}

void UpdateControls(DriverDebugOutputViewState& state) {
    const bool controlBusy = state.controlTask && state.controlTask->running();
    ::EnableWindow(state.startButton, !controlBusy && !state.capturing);
    ::EnableWindow(state.stopButton, !controlBusy && state.capturing);
    ::EnableWindow(state.queryButton, !controlBusy);
}

std::wstring ControlStatusText(const ksword::ark::DebugOutputControlResult& result) {
    if (!result.io.ok) {
        if (result.unsupported) {
            return L"当前 KswordARK 驱动不支持内核调试输出 IOCTL。";
        }
        return L"调试输出控制失败：" + Utf8ToWide(result.io.message) +
            L"（Win32=" + std::to_wstring(result.io.win32Error) + L"）。";
    }
    return L"调试输出能力：环容量=" + std::to_wstring(result.ringCapacity) +
        L"，排队=" + std::to_wstring(result.queuedCount) +
        L"，最新序号=" + std::to_wstring(result.latestSequence) +
        L"，累计丢弃=" + std::to_wstring(result.droppedCount) + L"。";
}

std::wstring BuildVisibleTsv(const DriverDebugOutputViewState& state, const bool selectedOnly) {
    std::wstring text = L"序号\t中断时间(100ns)\t组件\t级别\t标记\t文本\r\n";
    const int selected = ListView_GetNextItem(state.listView, -1, LVNI_SELECTED);
    const int start = selectedOnly ? selected : 0;
    const int end = selectedOnly ? selected + 1 : static_cast<int>(state.visibleRows.size());
    for (int index = start; index >= 0 && index < end; ++index) {
        const auto& cells = DebugCells(*state.visibleRows[static_cast<std::size_t>(index)]);
        for (std::size_t cell = 0; cell < cells.size(); ++cell) {
            if (cell != 0) {
                text += L'\t';
            }
            text += DriverModel::sanitizeTsvCell(cells[cell]);
        }
        text += L"\r\n";
    }
    return text;
}

void CommitVisibleRows(DriverDebugOutputViewState& state, const std::wstring& selected, const std::wstring& top) {
    if (!::IsWindowVisible(state.hwnd)) {
        if (!state.projectionDirty) {
            state.deferredSelectedKey = selected;
            state.deferredTopKey = top;
        }
        state.projectionDirty = true;
        return;
    }
    state.projectionDirty = false;
    ListView_SetItemCountEx(state.listView, static_cast<int>(state.visibleRows.size()),
        LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    RestoreListPosition(state, selected, top);
    ::InvalidateRect(state.listView, nullptr, FALSE);
}

void RequestFilter(DriverDebugOutputViewState& state, std::wstring query) {
    if (!state.filterTask) return;
    const bool useRegex = Ksword::Ui::GetFilterBarRegexEnabled(state.filterBar);
    const bool changed = query != state.filterQuery || useRegex != state.filterUseRegex ||
        state.filterSession != state.captureSession;
    state.filterQuery = std::move(query);
    state.filterUseRegex = useRegex;
    if (changed) {
        ++state.snapshotGeneration;
        state.filterSession = state.captureSession;
        state.filteredThrough = 0;
    }
    if (!changed && state.filterTask->running()) return;
    const auto first = std::upper_bound(state.records.begin(), state.records.end(), state.filteredThrough,
        [](const auto sequence, const auto& row) { return sequence < row->sequence; });
    auto rows = std::make_shared<std::vector<std::shared_ptr<const DebugRecordRow>>>(first, state.records.end());
    if (rows->empty()) return;
    const auto generation = state.snapshotGeneration;
    const auto session = state.captureSession;
    state.filterTask->request(
        [rows, generation, session, useRegex, query = state.filterQuery] {
            FilterSnapshot result;
            result.generation = generation;
            result.captureSession = session;
            result.query = query;
            result.useRegex = useRegex;
            result.throughSequence = rows->back()->sequence;
            std::wregex compiled;
            bool regexReady = false;
            if (useRegex && !query.empty()) {
                try { compiled.assign(query, std::regex_constants::ECMAScript | std::regex_constants::icase); regexReady = true; }
                catch (const std::regex_error&) { }
            }
            for (const auto& row : *rows) {
                if (MatchesDebugRecord(*row, query, regexReady ? &compiled : nullptr)) result.matches.push_back(row);
            }
            return result;
        },
        [&state](std::uint64_t, std::optional<FilterSnapshot>&& result, std::exception_ptr error) {
            if (error || !result) return;
            if (result->generation != state.snapshotGeneration || result->captureSession != state.captureSession ||
                result->query != state.filterQuery || result->useRegex != state.filterUseRegex) {
                RequestFilter(state, state.filterQuery);
                return;
            }
            const auto selected = state.projectionDirty ? state.deferredSelectedKey :
                StableKeyAt(state, ListView_GetNextItem(state.listView, -1, LVNI_SELECTED));
            const auto top = state.projectionDirty ? state.deferredTopKey : StableKeyAt(state, ListView_GetTopIndex(state.listView));
            if (state.appliedFilterGeneration != result->generation) {
                state.visibleRows.clear();
                state.appliedFilterGeneration = result->generation;
            }
            const auto first = state.records.empty() ? UINT64_MAX : state.records.front()->sequence;
            for (auto& row : result->matches) if (row->sequence >= first) state.visibleRows.push_back(std::move(row));
            state.filteredThrough = (std::max)(state.filteredThrough, result->throughSequence);
            CommitVisibleRows(state, selected, top);
            RequestFilter(state, state.filterQuery);
        });
}

void RebuildVirtualRows(DriverDebugOutputViewState& state) {
    RequestFilter(state, state.filterBar ? Ksword::Ui::GetFilterBarText(state.filterBar) : state.filterQuery);
}

void ClearRows(DriverDebugOutputViewState& state) {
    state.records.clear();
    state.visibleRows.clear();
    ++state.snapshotGeneration;
    state.filteredThrough = 0;
    state.appliedFilterGeneration = state.snapshotGeneration;
    CommitVisibleRows(state, {}, {});
}

void RequestDrain(DriverDebugOutputViewState& state) {
    if (!state.capturing || !state.drainTask || state.drainTask->running()) {
        return;
    }
    const std::uint64_t session = state.captureSession;
    const std::uint64_t afterSequence = state.nextSequence;
    state.drainTask->request(
        [session, afterSequence] {
            DrainSnapshot snapshot{};
            snapshot.captureSession = session;
            ksword::ark::DriverClient client;
            ksword::ark::DriverHandle handle = client.open();
            if (!handle.isValid()) {
                snapshot.result.io.ok = false;
                snapshot.result.io.win32Error = ::GetLastError();
                snapshot.result.io.message = "unable to open KswordARK device for debug-output drain";
                return snapshot;
            }
            snapshot.result = client.drainDebugOutput(handle, afterSequence, KSWORD_ARK_DEBUG_OUTPUT_MAX_DRAIN_RECORDS);
            for (const auto& record : snapshot.result.records) {
                auto row = std::make_shared<DebugRecordRow>();
                row->sequence = record.sequence;
                row->interruptTime100ns = record.interruptTime100ns;
                row->componentId = record.componentId;
                row->level = record.level;
                row->flags = record.flags;
                row->cells = { std::to_wstring(row->sequence), std::to_wstring(row->interruptTime100ns),
                    Hex32(row->componentId), Hex32(row->level), Hex32(row->flags), Utf8ToWide(record.text) };
                snapshot.rows.push_back(std::move(row));
            }
            return snapshot;
        },
        [&state](std::uint64_t, std::optional<DrainSnapshot>&& snapshot, std::exception_ptr error) {
            if (error || !snapshot.has_value() || snapshot->captureSession != state.captureSession || !state.capturing) {
                return;
            }
            const ksword::ark::DebugOutputDrainResult& result = snapshot->result;
            if (!result.io.ok) {
                state.capturing = false;
                ::KillTimer(state.hwnd, kDrainTimerId);
                SetStatus(state, result.unsupported
                    ? L"当前 KswordARK 驱动不支持内核调试输出读取 IOCTL。"
                    : L"读取内核调试输出失败：" + Utf8ToWide(result.io.message));
                UpdateControls(state);
                return;
            }
            state.nextSequence = result.nextSequence;
            const auto selected = state.projectionDirty ? state.deferredSelectedKey :
                StableKeyAt(state, ListView_GetNextItem(state.listView, -1, LVNI_SELECTED));
            const auto top = state.projectionDirty ? state.deferredTopKey : StableKeyAt(state, ListView_GetTopIndex(state.listView));
            for (auto& row : snapshot->rows) state.records.push_back(std::move(row));
            while (state.records.size() > kMaximumRecords) state.records.pop_front();
            const auto first = state.records.empty() ? UINT64_MAX : state.records.front()->sequence;
            state.visibleRows.erase(std::remove_if(state.visibleRows.begin(), state.visibleRows.end(),
                [first](const auto& row) { return row->sequence < first; }), state.visibleRows.end());
            CommitVisibleRows(state, selected, top);
            if (!result.records.empty()) {
                RebuildVirtualRows(state);
            }
            SetStatus(state, L"正在捕获内核调试输出：本次=" + std::to_wstring(result.records.size()) +
                L"，缓存=" + std::to_wstring(state.records.size()) + L"/" + std::to_wstring(kMaximumRecords) +
                L"，驱动丢弃=" + std::to_wstring(result.droppedCount) +
                L"，游标覆盖=" + std::to_wstring(result.lostBeforeFirst) + L"。");
        });
}

void RequestControl(DriverDebugOutputViewState& state, const unsigned long action, const std::uint64_t session) {
    if (!state.controlTask) {
        return;
    }
    const std::uint64_t requestId = ++state.controlRequestId;
    UpdateControls(state);
    state.controlTask->request(
        [requestId, session, action] {
            ControlSnapshot snapshot{};
            snapshot.requestId = requestId;
            snapshot.captureSession = session;
            snapshot.action = action;
            ksword::ark::DriverClient client;
            ksword::ark::DriverHandle handle = client.open();
            if (!handle.isValid()) {
                snapshot.result.io.ok = false;
                snapshot.result.io.win32Error = ::GetLastError();
                snapshot.result.io.message = "unable to open KswordARK device for debug-output control";
                return snapshot;
            }
            snapshot.result = client.controlDebugOutput(handle, action);
            return snapshot;
        },
        [&state](std::uint64_t, std::optional<ControlSnapshot>&& snapshot, std::exception_ptr error) {
            if (error || !snapshot.has_value() || snapshot->requestId != state.controlRequestId) {
                SetStatus(state, L"调试输出后台控制异常结束。");
                UpdateControls(state);
                return;
            }
            const ControlSnapshot& completed = *snapshot;
            SetStatus(state, ControlStatusText(completed.result));
            if (completed.action == KSWORD_ARK_DEBUG_OUTPUT_ACTION_START && completed.result.io.ok &&
                completed.captureSession == state.captureSession) {
                state.capturing = true;
                state.nextSequence = 0;
                ::SetTimer(state.hwnd, kDrainTimerId, kDrainIntervalMs, nullptr);
                RequestDrain(state);
            }
            if (completed.action == KSWORD_ARK_DEBUG_OUTPUT_ACTION_STOP && completed.captureSession == state.captureSession) {
                state.capturing = false;
                ::KillTimer(state.hwnd, kDrainTimerId);
            }
            UpdateControls(state);
        });
    UpdateControls(state);
}

void StartCapture(DriverDebugOutputViewState& state) {
    if (state.capturing) {
        return;
    }
    ++state.captureSession;
    state.nextSequence = 0;
    ClearRows(state);
    SetStatus(state, L"正在后台注册 R0 内核调试输出回调…");
    RequestControl(state, KSWORD_ARK_DEBUG_OUTPUT_ACTION_START, state.captureSession);
}

void StopCapture(DriverDebugOutputViewState& state) {
    ++state.captureSession;
    state.capturing = false;
    ::KillTimer(state.hwnd, kDrainTimerId);
    SetStatus(state, L"正在后台停止 R0 内核调试输出回调…");
    RequestControl(state, KSWORD_ARK_DEBUG_OUTPUT_ACTION_STOP, state.captureSession);
}

void QueryCapability(DriverDebugOutputViewState& state) {
    SetStatus(state, L"正在后台查询调试输出能力…");
    RequestControl(state, KSWORD_ARK_DEBUG_OUTPUT_ACTION_QUERY, state.captureSession);
}

std::vector<Ksword::Ui::ListViewColumn> Columns() {
    return {
        { 0, 110, LVCFMT_RIGHT, L"序号" },
        { 1, 160, LVCFMT_RIGHT, L"中断时间 (100ns)" },
        { 2, 120, LVCFMT_LEFT, L"组件" },
        { 3, 120, LVCFMT_LEFT, L"级别" },
        { 4, 120, LVCFMT_LEFT, L"标记" },
        { 5, 760, LVCFMT_LEFT, L"调试文本" },
    };
}

void ShowContextMenu(DriverDebugOutputViewState& state, POINT screenPoint) {
    POINT client = screenPoint;
    ::ScreenToClient(state.listView, &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    const int item = ListView_HitTest(state.listView, &hit);
    state.contextColumn = std::max(0, hit.iSubItem);
    if (item >= 0) {
        ListView_SetItemState(state.listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(state.listView, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    const int selected = ListView_GetNextItem(state.listView, -1, LVNI_SELECTED);
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING | (selected >= 0 ? 0U : MF_GRAYED), kMenuCopyCell, L"复制单元格");
    ::AppendMenuW(menu, MF_STRING | (selected >= 0 ? 0U : MF_GRAYED), kMenuCopyRow, L"复制行");
    ::AppendMenuW(menu, MF_STRING | (!state.visibleRows.empty() ? 0U : MF_GRAYED), kMenuCopyVisible, L"复制可见结果");
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == kMenuCopyCell && selected >= 0 && selected < static_cast<int>(state.visibleRows.size())) {
        const auto& cells = DebugCells(*state.visibleRows[static_cast<std::size_t>(selected)]);
        const std::size_t column = static_cast<std::size_t>(std::max(0, state.contextColumn));
        DriverActions::CopyTextToClipboard(state.hwnd, column < cells.size() ? cells[column] : std::wstring{});
    } else if (command == kMenuCopyRow && selected >= 0) {
        DriverActions::CopyTextToClipboard(state.hwnd, BuildVisibleTsv(state, true));
    } else if (command == kMenuCopyVisible) {
        DriverActions::CopyTextToClipboard(state.hwnd, BuildVisibleTsv(state, false));
    }
}

void Layout(DriverDebugOutputViewState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = std::max(0, static_cast<int>(rc.right - rc.left));
    const int height = std::max(0, static_cast<int>(rc.bottom - rc.top));
    const int margin = 6;
    const int buttonWidth = 82;
    const int gap = 5;
    const int toolbarHeight = 28;
    ::MoveWindow(state.startButton, margin, margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.stopButton, margin + (buttonWidth + gap), margin, buttonWidth, 24, TRUE);
    ::MoveWindow(state.clearButton, margin + (buttonWidth + gap) * 2, margin, 64, 24, TRUE);
    ::MoveWindow(state.queryButton, margin + (buttonWidth + gap) * 2 + 69, margin, 88, 24, TRUE);
    ::MoveWindow(state.statusText, margin + (buttonWidth + gap) * 2 + 162, margin + 2,
        std::max(100, width - margin * 2 - (buttonWidth + gap) * 2 - 162), 20, TRUE);
    ::MoveWindow(state.filterBar, margin, margin + toolbarHeight, std::max(100, width - margin * 2), 28, TRUE);
    ::MoveWindow(state.listView, 0, margin + toolbarHeight + 32, width,
        std::max(40, height - margin - toolbarHeight - 32), TRUE);
}

bool CreateChildControls(DriverDebugOutputViewState& state) {
    state.startButton = Ksword::Ui::CreateButton(state.hwnd, kStartButtonId, L"开始捕获", 0, 0, 0, 0);
    state.stopButton = Ksword::Ui::CreateButton(state.hwnd, kStopButtonId, L"停止捕获", 0, 0, 0, 0);
    state.clearButton = Ksword::Ui::CreateButton(state.hwnd, kClearButtonId, L"清空", 0, 0, 0, 0);
    state.queryButton = Ksword::Ui::CreateButton(state.hwnd, kQueryButtonId, L"查询能力", 0, 0, 0, 0);
    state.statusText = Ksword::Ui::CreateText(state.hwnd, 0, L"准备查询内核调试输出能力。", 0, 0, 0, 0);
    state.filterBar = Ksword::Ui::CreateFilterBar(state.hwnd, kFilterBarId,
        L"筛选序号、组件、级别、标记和调试文本", 0, 0, 0, 0);
    if (!state.startButton || !state.stopButton || !state.clearButton || !state.queryButton || !state.statusText || !state.filterBar ||
        !state.virtualList.create(state.hwnd, kListId, 0, 0, 0, 0, LVS_SHOWSELALWAYS | LVS_SINGLESEL)) {
        return false;
    }
    state.listView = state.virtualList.hwnd();
    state.virtualList.addColumns(Columns());
    state.controlTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ControlSnapshot>>(state.hwnd, kMsgControlCompleted);
    state.drainTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<DrainSnapshot>>(state.hwnd, kMsgDrainCompleted);
    state.filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<FilterSnapshot>>(state.hwnd, kMsgFilterCompleted);
    Ksword::Ui::SetWindowFontRecursive(state.hwnd);
    RebuildVirtualRows(state);
    UpdateControls(state);
    return true;
}

bool RegisterViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kDriverDebugOutputClass;
    wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
        DriverDebugOutputViewState* state = StateFromWindow(hwnd);
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            state = create ? static_cast<DriverDebugOutputViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }
        switch (message) {
        case WM_CREATE:
            if (!state || !CreateChildControls(*state)) {
                return -1;
            }
            Layout(*state);
            QueryCapability(*state);
            return 0;
        case WM_SHOWWINDOW:
            if (state && wParam && state->projectionDirty)
                CommitVisibleRows(*state, state->deferredSelectedKey, state->deferredTopKey);
            break;
        case WM_SIZE:
            if (state) {
                Layout(*state);
            }
            return 0;
        case WM_TIMER:
            if (state && wParam == kDrainTimerId) {
                RequestDrain(*state);
                return 0;
            }
            break;
        case kMsgControlCompleted:
            if (state && state->controlTask && state->controlTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case kMsgDrainCompleted:
            if (state && state->drainTask && state->drainTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case kMsgFilterCompleted:
            if (state && state->filterTask && state->filterTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case WM_COMMAND:
            if (!state) {
                break;
            }
            if (LOWORD(wParam) == kFilterBarId && HIWORD(wParam) == EN_CHANGE) {
                RequestFilter(*state, Ksword::Ui::GetFilterBarText(state->filterBar));
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case kStartButtonId: StartCapture(*state); return 0;
                case kStopButtonId: StopCapture(*state); return 0;
                case kClearButtonId: ClearRows(*state); SetStatus(*state, L"已清空本地有界调试输出缓存。"); return 0;
                case kQueryButtonId: QueryCapability(*state); return 0;
                default: break;
                }
            }
            break;
        case WM_NOTIFY:
            if (state) {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (header && header->hwndFrom == state->listView && header->code == LVN_GETDISPINFOW) {
                    auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
                    if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 && info->item.iSubItem >= 0 &&
                        static_cast<std::size_t>(info->item.iItem) < state->visibleRows.size()) {
                        const auto& cells = state->visibleRows[static_cast<std::size_t>(info->item.iItem)]->cells;
                        if (static_cast<std::size_t>(info->item.iSubItem) < cells.size())
                            info->item.pszText = const_cast<LPWSTR>(cells[static_cast<std::size_t>(info->item.iSubItem)].c_str());
                    }
                    return 0;
                }
                if (header && header->hwndFrom == state->listView && header->code == NM_RCLICK) {
                    POINT point{};
                    ::GetCursorPos(&point);
                    ShowContextMenu(*state, point);
                    return 0;
                }
            }
            break;
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->listView) {
                POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (point.x == -1 && point.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->listView, &rc);
                    point = { rc.left + 24, rc.top + 24 };
                }
                ShowContextMenu(*state, point);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            if (state) {
                if (state->capturing) {
                    std::thread([] {
                        ksword::ark::DriverClient client;
                        ksword::ark::DriverHandle handle = client.open();
                        if (handle.isValid()) {
                            (void)client.controlDebugOutput(handle, KSWORD_ARK_DEBUG_OUTPUT_ACTION_STOP);
                        }
                    }).detach();
                }
                ++state->captureSession;
                ::KillTimer(hwnd, kDrainTimerId);
                if (state->controlTask) state->controlTask->cancel();
                if (state->drainTask) state->drainTask->cancel();
                if (state->filterTask) state->filterTask->cancel();
            }
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    };
    registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreateDriverDebugOutputView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterViewClass()) {
        return nullptr;
    }
    auto* state = new DriverDebugOutputViewState();
    HWND hwnd = ::CreateWindowExW(0, kDriverDebugOutputClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

void ResizeDriverDebugOutputView(HWND view, const RECT& bounds) {
    if (view) {
        ::MoveWindow(view, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    }
}

} // namespace Ksword::Features::Driver
