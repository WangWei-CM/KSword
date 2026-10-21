#include "CallbackMonitorView.h"

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ExportUtil.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Monitor {
namespace {

constexpr wchar_t kClassName[] = L"KswordARKLight.CallbackMonitor";
constexpr UINT kPollMessage = WM_APP + 733;
constexpr UINT kControlMessage = WM_APP + 734;
constexpr UINT_PTR kPollTimer = 1;
constexpr int kStart = 68101;
constexpr int kStop = 68102;
constexpr int kPause = 68103;
constexpr int kClear = 68104;
constexpr int kExport = 68105;
constexpr int kFilter = 68106;
constexpr int kList = 68107;
constexpr int kDetail = 68108;
constexpr int kStatus = 68109;
constexpr int kCategoryBase = 68120;
constexpr std::size_t kMaxLocalRecords = 10000;

struct PollSnapshot {
    std::uint64_t generation = 0;
    ksword::ark::CallbackMonitorReadResult read;
};

enum class MonitorCommand { Query, Start, Stop, Clear };
struct MonitorControlLifetime {
    std::mutex mutex;
    std::condition_variable idle;
    std::atomic_size_t pending{0};
    bool ownsCapture = false;
    std::atomic_bool retiring{false};
    std::atomic_ulong stopError{0};
};
struct MonitorControlResult {
    ksword::ark::CallbackMonitorStatusResult status;
    bool ownsCapture = false;
};

struct State {
    HWND hwnd = nullptr;
    HWND start = nullptr;
    HWND stop = nullptr;
    HWND pause = nullptr;
    HWND clear = nullptr;
    HWND exportButton = nullptr;
    HWND filter = nullptr;
    HWND list = nullptr;
    HWND detail = nullptr;
    HWND status = nullptr;
    std::array<HWND, 6> categories{};
    std::vector<ksword::ark::CallbackMonitorEventRow> records;
    std::vector<std::size_t> visible;
    std::wstring displayScratch;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<PollSnapshot>> pollTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<MonitorControlResult>> controlTask;
    std::shared_ptr<MonitorControlLifetime> controlLifetime = std::make_shared<MonitorControlLifetime>();
    std::shared_ptr<Ksword::Ui::BackgroundCleanup> retirement;
    bool retirementCancelled = false;
    std::uint64_t cursor = 0;
    std::uint64_t generation = 0;
    std::uint64_t dropped = 0;
    std::uint64_t lost = 0;
    bool capturing = false;
    bool ownsCapture = false;
    bool paused = false;
    std::wstring message = L"等待查询 R0 回调监控状态。";
};

const wchar_t* CategoryName(const std::uint32_t category) {
    switch (category) {
    case KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_PROCESS: return L"进程";
    case KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_THREAD: return L"线程";
    case KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_IMAGE: return L"镜像";
    case KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_REGISTRY: return L"注册表";
    case KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_OBJECT: return L"对象";
    case KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_MINIFILTER: return L"文件";
    default: return L"未知";
    }
}

std::wstring TimeText(const std::int64_t timeUtc100ns) {
    if (timeUtc100ns <= 0) { return L"-"; }
    const ULONGLONG ticks = static_cast<ULONGLONG>(timeUtc100ns);
    FILETIME utc{ static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32U) };
    FILETIME local{};
    SYSTEMTIME value{};
    if (!::FileTimeToLocalFileTime(&utc, &local) || !::FileTimeToSystemTime(&local, &value)) {
        return L"-";
    }
    wchar_t buffer[48]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute,
        value.wSecond, value.wMilliseconds);
    return buffer;
}

std::wstring WindowText(HWND hwnd) {
    const int length = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    ::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::array<std::wstring, 8> Cells(const ksword::ark::CallbackMonitorEventRow& row) {
    return { std::to_wstring(row.sequence), TimeText(row.timeUtc100ns),
        CategoryName(row.category), std::to_wstring(row.operation),
        std::to_wstring(row.originatingProcessId), std::to_wstring(row.targetProcessId),
        std::to_wstring(static_cast<unsigned long>(row.resultStatus)), row.path };
}

void SetStatus(State& state) {
    const std::wstring text = state.message + L" | " +
        (state.capturing ? (state.paused ? L"已暂停显示" : L"采集中") : L"未采集") +
        L" | 可见 " + std::to_wstring(state.visible.size()) +
        L" / " + std::to_wstring(state.records.size()) +
        L" | R0 丢弃 " + std::to_wstring(state.dropped) +
        L" | 游标丢失 " + std::to_wstring(state.lost);
    ::SetWindowTextW(state.status, text.c_str());
    const bool busy = state.controlLifetime->retiring.load(std::memory_order_acquire) ||
        (state.controlTask && state.controlTask->running());
    ::EnableWindow(state.start, !busy && (!state.capturing || state.paused));
    const bool retryStop = state.retirement && state.retirement->failure() == Ksword::Ui::BackgroundWorkRegistry::Failure::Operation;
    ::EnableWindow(state.stop, retryStop || (!busy && state.capturing));
    ::EnableWindow(state.clear, !busy);
    ::EnableWindow(state.pause, state.capturing);
    ::SetWindowTextW(state.pause, state.paused ? L"继续" : L"暂停");
}

void RebuildList(State& state) {
    const std::wstring query = WindowText(state.filter);
    state.visible.clear();
    for (std::size_t index = 0; index < state.records.size(); ++index) {
        if (query.empty()) { state.visible.push_back(index); continue; }
        const auto cells = Cells(state.records[index]);
        bool found = false;
        for (const auto& cell : cells) {
            if (std::search(cell.begin(), cell.end(), query.begin(), query.end(),
                    [](wchar_t left, wchar_t right) {
                        return std::towlower(left) == std::towlower(right);
                    }) != cell.end()) { found = true; break; }
        }
        if (!found) { continue; }
        state.visible.push_back(index);
    }
    ListView_SetItemCountEx(state.list, static_cast<int>(state.visible.size()),
        LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ::InvalidateRect(state.list, nullptr, FALSE);
    SetStatus(state);
}

void ShowDetail(State& state) {
    const int selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (selected < 0 || static_cast<std::size_t>(selected) >= state.visible.size()) {
        ::SetWindowTextW(state.detail, L"选择事件查看完整字段。");
        return;
    }
    const auto& row = state.records[state.visible[static_cast<std::size_t>(selected)]];
    std::wostringstream text;
    text << L"序号: " << row.sequence << L"\r\n时间: " << TimeText(row.timeUtc100ns)
         << L"\r\n类别: " << CategoryName(row.category) << L"\r\n操作: " << row.operation
         << L"\r\n来源 PID/TID: " << row.originatingProcessId << L" / " << row.originatingThreadId
         << L"\r\n目标 PID/TID: " << row.targetProcessId << L" / " << row.targetThreadId
         << L"\r\n父 PID: " << row.parentProcessId << L"\r\n会话: " << row.sessionId
         << L"\r\n原始/目标访问: " << row.originalAccess << L" / " << row.desiredAccess
         << L"\r\n结果: " << static_cast<unsigned long>(row.resultStatus)
         << L"\r\n对象类型/细节码: " << row.objectType << L" / " << row.detailCode
         << L"\r\n地址/长度: " << row.address << L" / " << row.regionSize
         << L"\r\n进程: " << row.processName << L"\r\n路径: " << row.path;
    ::SetWindowTextW(state.detail, text.str().c_str());
}

unsigned long SelectedCategoryMask(const State& state) {
    constexpr std::array<unsigned long, 6> masks{
        KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_PROCESS,
        KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_THREAD,
        KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_IMAGE,
        KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_REGISTRY,
        KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_OBJECT,
        KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_MINIFILTER };
    unsigned long mask = 0;
    for (std::size_t index = 0; index < masks.size(); ++index) {
        if (::SendMessageW(state.categories[index], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= masks[index];
        }
    }
    return mask;
}

void RequestMonitorControl(State& state, const MonitorCommand command, const unsigned long mask,
    std::function<void(const ksword::ark::CallbackMonitorStatusResult&)> completed) {
    if (!state.controlTask || state.controlTask->running() ||
        state.controlLifetime->retiring.load(std::memory_order_acquire)) return;
    const auto lifetime = state.controlLifetime;
    const auto lease = Ksword::Ui::TryAcceptBackgroundWork();
    if (!lease) return;
    const auto pending = Ksword::Ui::HoldPendingOperation(lifetime);
    state.controlTask->request(
        [lifetime, lease, pending, command, mask] {
            std::lock_guard lock(lifetime->mutex);
            MonitorControlResult result;
            const ksword::ark::DriverClient client;
            if (command == MonitorCommand::Start) {
                result.status = client.controlCallbackMonitor(KSWORD_ARK_CALLBACK_MONITOR_ACTION_START, mask);
                if (result.status.io.ok) lifetime->ownsCapture = true;
            } else if (command == MonitorCommand::Stop) {
                if (lifetime->ownsCapture) {
                    result.status = client.controlCallbackMonitor(KSWORD_ARK_CALLBACK_MONITOR_ACTION_STOP, 0);
                    if (result.status.io.ok) lifetime->ownsCapture = false;
                } else {
                    result.status.io.ok = true; // Detach from a pre-existing capture.
                }
            } else {
                result.status = client.queryCallbackMonitorStatus();
            }
            result.ownsCapture = lifetime->ownsCapture;
            return result;
        },
        [&state, completed = std::move(completed)](std::uint64_t, std::optional<MonitorControlResult>&& result, std::exception_ptr error) {
            if (error || !result) {
                state.message = L"R0 状态查询失败：Win32=" + std::to_wstring(ERROR_UNHANDLED_EXCEPTION);
                SetStatus(state);
                return;
            }
            state.ownsCapture = result->ownsCapture;
            if (state.controlLifetime->retiring.load(std::memory_order_acquire)) { SetStatus(state); return; }
            completed(result->status);
        });
    SetStatus(state);
}

void BeginRetirement(State& state) {
    if (state.controlLifetime->retiring.exchange(true, std::memory_order_acq_rel)) return;
    ::KillTimer(state.hwnd, kPollTimer);
    ++state.generation;
    const auto lifetime = state.controlLifetime;
    state.retirement = Ksword::Ui::RunBackgroundRetirement([lifetime] {
        std::unique_lock lock(lifetime->mutex);
        lifetime->idle.wait(lock, [&] { return lifetime->pending.load(std::memory_order_acquire) == 0; });
        if (!lifetime->ownsCapture) return true;
        const auto result = ksword::ark::DriverClient().controlCallbackMonitor(
            KSWORD_ARK_CALLBACK_MONITOR_ACTION_STOP, 0);
        lifetime->stopError.store(result.io.win32Error, std::memory_order_release);
        if (!result.io.ok) return false;
        lifetime->ownsCapture = false;
        return true;
    });
    state.message = L"正在停止采集；现有记录保留。 / Stopping; retained records remain available.";
    SetStatus(state);
}

bool PollRetirement(State& state) {
    if (!state.retirement) return true;
    Ksword::Ui::BackgroundWorkRegistry::global().retry();
    if (!state.retirement->complete()) {
        if (state.retirement->failure() == Ksword::Ui::BackgroundWorkRegistry::Failure::Operation) {
            state.message = L"R0 停止失败：Win32=" + std::to_wstring(state.controlLifetime->stopError.load());
            SetStatus(state);
        }
        return false;
    }
    if (state.controlTask && state.controlTask->running()) return false;
    state.capturing = false;
    state.ownsCapture = false;
    state.paused = false;
    state.message = L"已停止本页采集。 / Capture stopped; start manually to resume.";
    if (state.retirementCancelled) {
        state.controlLifetime->retiring.store(false, std::memory_order_release);
        state.retirementCancelled = false;
        state.retirement.reset();
        ::KillTimer(state.hwnd, kPollTimer);
    }
    SetStatus(state);
    return true;
}

void QueryStatus(State& state) {
    RequestMonitorControl(state, MonitorCommand::Query, 0, [&state](const auto& status) {
        if (!status.io.ok) {
            state.message = status.unsupported ? L"当前驱动不支持回调监控。" :
                L"R0 状态查询失败：Win32=" + std::to_wstring(status.io.win32Error);
        } else {
            state.capturing = (status.runtimeFlags & KSWORD_ARK_CALLBACK_MONITOR_RUNTIME_CAPTURING) != 0;
            state.cursor = status.latestSequence;
            state.dropped = status.droppedCount;
            if (state.capturing) {
                constexpr std::array<unsigned long, 6> masks{
                    KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_PROCESS,
                    KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_THREAD,
                    KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_IMAGE,
                    KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_REGISTRY,
                    KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_OBJECT,
                    KSWORD_ARK_CALLBACK_MONITOR_CATEGORY_MINIFILTER };
                for (std::size_t index = 0; index < masks.size(); ++index) {
                    ::SendMessageW(state.categories[index], BM_SETCHECK,
                        (status.categoryMask & masks[index]) ? BST_CHECKED : BST_UNCHECKED, 0);
                }
            }
            state.message = state.capturing ? L"已连接到运行中的 R0 回调监控。" : L"R0 回调监控就绪。";
        }
        if (state.capturing) ::SetTimer(state.hwnd, kPollTimer, 500, nullptr);
        SetStatus(state);
    });
}

void Start(State& state) {
    if (state.controlLifetime->retiring.load() || Ksword::Ui::BackgroundWorkRegistry::global().closing()) return;
    if (!state.controlTask || state.controlTask->running()) return;
    if (state.capturing && state.paused) {
        state.paused = false;
        state.message = L"已继续显示回调事件。";
        SetStatus(state);
        return;
    }
    if (state.capturing) { return; }
    const unsigned long mask = SelectedCategoryMask(state);
    if (mask == 0) {
        state.message = L"请至少选择一个采集类别。";
        SetStatus(state);
        return;
    }
    RequestMonitorControl(state, MonitorCommand::Start, mask, [&state](const auto& status) {
        if (!status.io.ok) {
            state.message = status.unsupported ? L"当前驱动不支持回调监控。" :
                L"启动失败：Win32=" + std::to_wstring(status.io.win32Error);
            SetStatus(state);
            return;
        }
        ++state.generation;
        state.cursor = status.latestSequence;
        state.dropped = status.droppedCount;
        state.lost = 0;
        state.records.clear();
        state.ownsCapture = true;
        state.capturing = true;
        state.paused = false;
        state.message = L"R0 回调监控已启动。";
        RebuildList(state);
        ::SetTimer(state.hwnd, kPollTimer, 500, nullptr);
    });
}

void Stop(State& state) {
    if (state.retirement && state.retirement->failure() == Ksword::Ui::BackgroundWorkRegistry::Failure::Operation) {
        Ksword::Ui::BackgroundWorkRegistry::global().retry(true);
        return;
    }
    if (state.controlLifetime->retiring.load() || Ksword::Ui::BackgroundWorkRegistry::global().closing()) return;
    if (!state.controlTask || state.controlTask->running()) return;
    ::KillTimer(state.hwnd, kPollTimer);
    ++state.generation;
    RequestMonitorControl(state, MonitorCommand::Stop, 0, [&state](const auto& status) {
        if (!status.io.ok) {
            state.message = L"R0 停止失败：Win32=" + std::to_wstring(status.io.win32Error);
            ::SetTimer(state.hwnd, kPollTimer, 500, nullptr);
            SetStatus(state);
            return;
        }
        state.dropped = status.droppedCount;
        state.capturing = false;
        state.ownsCapture = false;
        state.paused = false;
        state.message = L"已停止本页采集。";
        SetStatus(state);
    });
}

void Poll(State& state) {
    if (state.controlLifetime->retiring.load(std::memory_order_acquire) || !state.capturing || state.paused || !state.pollTask || state.pollTask->running() ||
        (state.controlTask && state.controlTask->running())) { return; }
    const auto cursor = state.cursor;
    const auto generation = state.generation;
    state.pollTask->request(
        [cursor, generation] {
            return PollSnapshot{ generation, ksword::ark::DriverClient().readCallbackMonitor(
                cursor, KSWORD_ARK_CALLBACK_MONITOR_MAX_READ_RECORDS) };
        },
        [&state](std::uint64_t, std::optional<PollSnapshot>&& snapshot, std::exception_ptr error) {
            if (error || !snapshot || snapshot->generation != state.generation) { return; }
            auto& read = snapshot->read;
            if (!read.io.ok) {
                const std::wstring failure = read.unsupported ? L"当前驱动不支持读取回调事件。" :
                    L"R0 读取失败：Win32=" + std::to_wstring(read.io.win32Error);
                Stop(state);
                state.message = failure;
                SetStatus(state);
                return;
            }
            state.cursor = read.nextSequence;
            state.dropped = read.droppedCount;
            state.lost += read.lostBeforeFirst;
            for (auto& record : read.records) {
                state.records.push_back(std::move(record));
            }
            if (state.records.size() > kMaxLocalRecords) {
                state.records.erase(state.records.begin(),
                    state.records.begin() + static_cast<std::ptrdiff_t>(state.records.size() - kMaxLocalRecords));
            }
            if (!read.records.empty()) { RebuildList(state); }
            else { SetStatus(state); }
        });
}

std::wstring ExportText(const State& state) {
    std::wstring text = L"序号\t时间\t类别\t操作\t来源PID\t目标PID\t结果\t路径\r\n";
    for (const std::size_t index : state.visible) {
        const auto cells = Cells(state.records[index]);
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (column) { text += L'\t'; }
            std::wstring cell = cells[column];
            for (wchar_t& ch : cell) {
                if (ch == L'\t' || ch == L'\r' || ch == L'\n') { ch = L' '; }
            }
            text += cell;
        }
        text += L"\r\n";
    }
    return text;
}

void Layout(State& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = (std::max)(0, static_cast<int>(rc.right - rc.left));
    const int height = (std::max)(0, static_cast<int>(rc.bottom - rc.top));
    std::array<HWND, 5> buttons{ state.start, state.stop, state.pause, state.clear, state.exportButton };
    for (int index = 0; index < static_cast<int>(buttons.size()); ++index) {
        ::MoveWindow(buttons[static_cast<std::size_t>(index)], 8 + index * 80, 8, 74, 26, TRUE);
    }
    for (int index = 0; index < static_cast<int>(state.categories.size()); ++index) {
        ::MoveWindow(state.categories[static_cast<std::size_t>(index)], 8 + index * 100, 42, 96, 24, TRUE);
    }
    ::MoveWindow(state.filter, 8, 72, (std::max)(100, width - 16), 26, TRUE);
    const int detailHeight = 116;
    const int listHeight = (std::max)(80, height - 110 - detailHeight - 32);
    ::MoveWindow(state.list, 8, 106, (std::max)(100, width - 16), listHeight, TRUE);
    ::MoveWindow(state.detail, 8, 112 + listHeight,
        (std::max)(100, width - 16), detailHeight, TRUE);
    ::MoveWindow(state.status, 8, height - 26, (std::max)(100, width - 16), 20, TRUE);
}

bool CreateControls(State& state) {
    state.start = Ksword::Ui::CreateButton(state.hwnd, kStart, L"启动", 0, 0, 1, 1);
    state.stop = Ksword::Ui::CreateButton(state.hwnd, kStop, L"停止", 0, 0, 1, 1);
    state.pause = Ksword::Ui::CreateButton(state.hwnd, kPause, L"暂停", 0, 0, 1, 1);
    state.clear = Ksword::Ui::CreateButton(state.hwnd, kClear, L"清空", 0, 0, 1, 1);
    state.exportButton = Ksword::Ui::CreateButton(state.hwnd, kExport, L"导出", 0, 0, 1, 1);
    constexpr std::array<const wchar_t*, 6> names{
        L"进程", L"线程", L"镜像", L"注册表", L"对象", L"文件" };
    for (std::size_t index = 0; index < names.size(); ++index) {
        state.categories[index] = ::CreateWindowExW(0, L"BUTTON", names[index],
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 1, 1, state.hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCategoryBase + index)),
            ::GetModuleHandleW(nullptr), nullptr);
        ::SendMessageW(state.categories[index], BM_SETCHECK, BST_CHECKED, 0);
    }
    state.filter = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 1, 1, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilter)),
        ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(state.filter, EM_SETCUEBANNER, FALSE,
        reinterpret_cast<LPARAM>(L"筛选当前事件"));
    state.list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 1, 1, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
        ::GetModuleHandleW(nullptr), nullptr);
    state.detail = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"选择事件查看完整字段。",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 1, 1, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetail)),
        ::GetModuleHandleW(nullptr), nullptr);
    state.status = Ksword::Ui::CreateText(state.hwnd, kStatus, L"", 0, 0, 1, 1);
    if (!state.start || !state.stop || !state.pause || !state.clear || !state.exportButton ||
        !state.filter || !state.list || !state.detail || !state.status) { return false; }
    for (HWND check : state.categories) { if (!check) { return false; } }
    Ksword::Ui::SetWindowFontRecursive(state.hwnd);
    ListView_SetExtendedListViewStyle(state.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    constexpr std::array<const wchar_t*, 8> titles{
        L"序号", L"时间", L"类别", L"操作", L"来源 PID", L"目标 PID", L"结果", L"路径" };
    constexpr std::array<int, 8> widths{ 80, 170, 75, 75, 95, 95, 95, 360 };
    for (int index = 0; index < static_cast<int>(titles.size()); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(titles[static_cast<std::size_t>(index)]);
        column.cx = widths[static_cast<std::size_t>(index)];
        column.iSubItem = index;
        ListView_InsertColumn(state.list, index, &column);
    }
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto owned = std::make_unique<State>();
        owned->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
        return TRUE;
    }
    case WM_CREATE:
        if (!state || !CreateControls(*state)) { return -1; }
        state->pollTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<PollSnapshot>>(hwnd, kPollMessage);
        state->controlTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<MonitorControlResult>>(hwnd, kControlMessage);
        Layout(*state);
        QueryStatus(*state);
        return 0;
    case WM_SIZE:
        if (state) { Layout(*state); }
        return 0;
    case Ksword::Ui::kBeginNativeRetirementMessage:
        if (state) { state->retirementCancelled = false; BeginRetirement(*state); }
        return 0;
    case Ksword::Ui::kPollNativeRetirementMessage:
        return state && !PollRetirement(*state) ? 1 : 0;
    case Ksword::Ui::kCancelNativeRetirementMessage:
        if (state) { state->retirementCancelled = true; PollRetirement(*state); if (state->retirementCancelled) ::SetTimer(hwnd, kPollTimer, 500, nullptr); }
        return 0;
    case WM_TIMER:
        if (state && state->retirementCancelled) { PollRetirement(*state); return 0; }
        if (state && wParam == kPollTimer) { Poll(*state); return 0; }
        break;
    case WM_COMMAND:
        if (!state) { break; }
        if (LOWORD(wParam) == kFilter && HIWORD(wParam) == EN_CHANGE) {
            RebuildList(*state);
            return 0;
        }
        if (HIWORD(wParam) != BN_CLICKED) { break; }
        switch (LOWORD(wParam)) {
        case kStart: Start(*state); return 0;
        case kStop: Stop(*state); return 0;
        case kPause:
            state->paused = !state->paused;
            state->message = state->paused ? L"本页已暂停显示。" : L"本页已继续显示。";
            SetStatus(*state);
            return 0;
        case kClear: {
            if (!state->controlTask || state->controlTask->running()) return 0;
            ++state->generation;
            RequestMonitorControl(*state, MonitorCommand::Clear, 0, [state](const auto& status) {
                if (status.io.ok) { state->cursor = status.latestSequence; state->dropped = status.droppedCount; }
                state->lost = 0;
                state->records.clear();
                ::SetWindowTextW(state->detail, L"选择事件查看完整字段。");
                state->message = L"已清空本页记录，驱动环形缓冲未清空。";
                RebuildList(*state);
            });
            return 0;
        }
        case kExport: {
            std::wstring error;
            const auto result = Ksword::Ui::SaveUtf8TextFileWithDialog(hwnd,
                L"callback-monitor.tsv", L"导出内核回调监控",
                L"TSV (*.tsv)\0*.tsv\0All Files (*.*)\0*.*\0", L"tsv",
                ExportText(*state), &error);
            state->message = result == Ksword::Ui::SaveTextFileResult::Saved ? L"已导出可见事件。"
                : result == Ksword::Ui::SaveTextFileResult::Cancelled ? L"已取消导出。"
                : L"导出失败：" + error;
            SetStatus(*state);
            return 0;
        }
        default: break;
        }
        break;
    case WM_NOTIFY:
        if (state && reinterpret_cast<NMHDR*>(lParam)->hwndFrom == state->list) {
            if (reinterpret_cast<NMHDR*>(lParam)->code == LVN_GETDISPINFOW) {
                auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
                if ((info->item.mask & LVIF_TEXT) != 0 && info->item.iItem >= 0 &&
                    static_cast<std::size_t>(info->item.iItem) < state->visible.size() &&
                    info->item.iSubItem >= 0 && info->item.iSubItem < 8) {
                    const auto cells = Cells(state->records[state->visible[
                        static_cast<std::size_t>(info->item.iItem)]]);
                    state->displayScratch = cells[static_cast<std::size_t>(info->item.iSubItem)];
                    info->item.pszText = const_cast<LPWSTR>(state->displayScratch.c_str());
                }
                return 0;
            }
            if (reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
                ShowDetail(*state);
                return 0;
            }
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd, &paint);
            RECT rc{}; ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (state) {
            ::KillTimer(hwnd, kPollTimer);
            if (state->pollTask) { state->pollTask->cancel(); }
            if (state->controlTask) state->controlTask->cancel();
            BeginRetirement(*state);
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    default:
        if (state && message == kControlMessage && state->controlTask &&
            state->controlTask->consume(hwnd, wParam, lParam)) return 0;
        if (state && message == kPollMessage && state->pollTask &&
            state->pollTask->consume(hwnd, wParam, lParam)) { return 0; }
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

HWND CreateCallbackMonitorPage(HWND parent, const RECT& bounds) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClassName;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        registered = ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) { return nullptr; }
    return ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace Ksword::Features::Monitor
