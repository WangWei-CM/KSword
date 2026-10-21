#include "DriverThreadView.h"

#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ExportUtil.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../../Ksword5.1/Ksword5.1/ksword/process/process.h"

#include <commctrl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ksword::Features::Driver {
namespace {

constexpr wchar_t kClassName[] = L"KswordARKLight.Driver.SystemThreads";
constexpr int kRefreshId = 66801;
constexpr int kFilterId = 66802;
constexpr int kSuspendId = 66803;
constexpr int kResumeId = 66804;
constexpr int kTerminateId = 66805;
constexpr int kListId = 66806;
constexpr int kDetailId = 66807;
constexpr int kStatusId = 66808;
constexpr UINT kRefreshCompleted = WM_APP + 668;
constexpr UINT kActionCompleted = WM_APP + 669;
constexpr UINT kMenuSuspend = 66811;
constexpr UINT kMenuResume = 66812;
constexpr UINT kMenuTerminate = 66813;
constexpr UINT kMenuCopy = 66814;
constexpr std::uint32_t kSystemPid = 4U;

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemModuleInformation = 11UL;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004UL);
constexpr ULONG kMaxModuleBytes = 16UL * 1024UL * 1024UL;

struct RawModule {
    HANDLE section;
    void* mappedBase;
    void* imageBase;
    ULONG imageSize;
    ULONG flags;
    USHORT loadOrderIndex;
    USHORT initOrderIndex;
    USHORT loadCount;
    USHORT fileNameOffset;
    UCHAR fullPathName[256];
};

struct RawModules {
    ULONG count;
    RawModule entries[1];
};

struct Module {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::wstring name;
    std::wstring path;
};

struct ThreadRow {
    std::uint32_t tid = 0;
    std::uint64_t createTime = 0;
    std::uint64_t start = 0;
    int priority = 0;
    int basePriority = 0;
    std::uint32_t state = 0;
    std::uint32_t waitReason = 0;
    std::uint32_t r0Status = KSWORD_ARK_THREAD_R0_STATUS_UNAVAILABLE;
    bool workerKnown = false;
    bool activeWorker = false;
    Module module;
};

struct Snapshot {
    bool r3Available = false;
    bool r0Available = false;
    bool modulesAvailable = false;
    DWORD r0Error = ERROR_SUCCESS;
    std::vector<ThreadRow> threads;
};

struct ViewState {
    HWND hwnd = nullptr;
    HWND refresh = nullptr;
    HWND filter = nullptr;
    HWND suspend = nullptr;
    HWND resume = nullptr;
    HWND terminate = nullptr;
    HWND detail = nullptr;
    HWND status = nullptr;
    Ksword::Ui::VirtualListView list;
    std::vector<ThreadRow> threads;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<Snapshot>> task;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ksword::ark::IoResult>> actionTask;
};

std::wstring WideFromAnsi(const char* value, int length) {
    if (!value || length <= 0) return {};
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, value, length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, value, length, result.data(), needed);
    return result;
}

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

std::wstring Hex(std::uint64_t value) {
    if (value == 0) return L"—";
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return stream.str();
}

std::wstring WindowText(HWND hwnd) {
    if (!hwnd) return {};
    const int length = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::vector<Module> QueryModules(bool& available) {
    available = false;
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? reinterpret_cast<NtQuerySystemInformationFn>(
        ::GetProcAddress(ntdll, "NtQuerySystemInformation")) : nullptr;
    if (!query) return {};
    ULONG needed = 0;
    LONG status = query(kSystemModuleInformation, nullptr, 0, &needed);
    if (status != kStatusInfoLengthMismatch && status < 0) return {};
    if (needed < sizeof(ULONG) || needed > kMaxModuleBytes) return {};
    std::vector<unsigned char> bytes(static_cast<std::size_t>(needed) + 65536U);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (bytes.size() > kMaxModuleBytes) return {};
        status = query(kSystemModuleInformation, bytes.data(), static_cast<ULONG>(bytes.size()), &needed);
        if (status >= 0) break;
        if (status != kStatusInfoLengthMismatch || needed > kMaxModuleBytes) return {};
        bytes.resize(static_cast<std::size_t>(needed) + 4096U);
    }
    if (status < 0 || bytes.size() < offsetof(RawModules, entries)) return {};
    const auto* header = reinterpret_cast<const RawModules*>(bytes.data());
    const std::size_t capacity = (bytes.size() - offsetof(RawModules, entries)) / sizeof(RawModule);
    if (header->count > capacity) return {};
    std::vector<Module> result;
    result.reserve(header->count);
    for (ULONG index = 0; index < header->count; ++index) {
        const RawModule& source = header->entries[index];
        Module module;
        module.base = reinterpret_cast<std::uint64_t>(source.imageBase);
        module.size = source.imageSize;
        const auto* path = reinterpret_cast<const char*>(source.fullPathName);
        std::size_t pathLength = 0;
        while (pathLength < sizeof(source.fullPathName) && path[pathLength] != '\0') ++pathLength;
        module.path = WideFromAnsi(path, static_cast<int>(pathLength));
        const std::size_t nameOffset = std::min<std::size_t>(source.fileNameOffset, pathLength);
        module.name = WideFromAnsi(path + nameOffset, static_cast<int>(pathLength - nameOffset));
        result.push_back(std::move(module));
    }
    available = true;
    return result;
}

const Module* FindModule(const std::vector<Module>& modules, std::uint64_t address) {
    if (!address) return nullptr;
    for (const Module& module : modules) {
        const std::uint64_t end = module.base > (std::numeric_limits<std::uint64_t>::max)() - module.size
            ? (std::numeric_limits<std::uint64_t>::max)() : module.base + module.size;
        if (address >= module.base && address < end) return &module;
    }
    return nullptr;
}

Snapshot Collect() {
    Snapshot snapshot;
    bool usedNtQuery = false;
    std::string r3Message;
    const auto r3Threads = ks::process::EnumerateSystemThreads(&usedNtQuery, &r3Message);
    snapshot.r3Available = usedNtQuery;
    const ksword::ark::DriverClient client;
    const auto r0 = client.enumerateThreads(KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_WORKER_STATE, kSystemPid);
    snapshot.r0Available = r0.io.ok;
    snapshot.r0Error = r0.io.win32Error;
    std::unordered_map<std::uint32_t, const ksword::ark::ThreadEntry*> r0ByTid;
    for (const auto& row : r0.entries) {
        if (row.processId == kSystemPid && row.threadId != 0) r0ByTid[row.threadId] = &row;
    }
    bool modulesAvailable = false;
    const auto modules = QueryModules(modulesAvailable);
    snapshot.modulesAvailable = modulesAvailable;
    for (const auto& source : r3Threads) {
        if (source.ownerPid != kSystemPid || source.threadId == 0) continue;
        ThreadRow row;
        row.tid = source.threadId;
        row.createTime = source.createTime100ns;
        row.start = source.startAddress ? source.startAddress : source.win32StartAddress;
        row.priority = source.priority;
        row.basePriority = source.basePriority;
        row.state = source.threadState;
        row.waitReason = source.waitReason;
        if (const auto it = r0ByTid.find(row.tid); it != r0ByTid.end()) {
            row.r0Status = it->second->r0Status;
            row.workerKnown = (it->second->fieldFlags & KSWORD_ARK_THREAD_FIELD_ACTIVE_EX_WORKER_PRESENT) != 0;
            row.activeWorker = (it->second->flags & KSWORD_ARK_THREAD_FLAG_ACTIVE_EX_WORKER) != 0;
        }
        if (const Module* module = FindModule(modules, row.start)) row.module = *module;
        snapshot.threads.push_back(std::move(row));
    }
    std::sort(snapshot.threads.begin(), snapshot.threads.end(),
        [](const ThreadRow& left, const ThreadRow& right) { return left.tid < right.tid; });
    return snapshot;
}

ViewState* State(HWND hwnd) {
    return reinterpret_cast<ViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

const ThreadRow* Selected(const ViewState& state) {
    const int selected = ListView_GetNextItem(state.list.hwnd(), -1, LVNI_SELECTED);
    if (selected < 0) return nullptr;
    const auto& visible = state.list.visibleIndexes();
    if (static_cast<std::size_t>(selected) >= visible.size()) return nullptr;
    const std::size_t source = visible[static_cast<std::size_t>(selected)];
    return source < state.threads.size() ? &state.threads[source] : nullptr;
}

void UpdateDetail(ViewState& state) {
    const ThreadRow* row = Selected(state);
    const BOOL enabled = row && (!state.actionTask || !state.actionTask->running()) ? TRUE : FALSE;
    ::EnableWindow(state.suspend, enabled);
    ::EnableWindow(state.resume, enabled);
    ::EnableWindow(state.terminate, enabled);
    if (!row) {
        ::SetWindowTextW(state.detail, L"选择一条 System(PID 4) 线程查看身份、调度及驱动归属证据。");
        return;
    }
    std::wostringstream text;
    text << L"TID: " << row->tid
         << L"\r\nCreateTime100ns: " << row->createTime
         << L"\r\nStartRoutine: " << Hex(row->start)
         << L"\r\nPriority/BasePriority: " << row->priority << L"/" << row->basePriority
         << L"\r\nState/WaitReason: " << row->state << L"/" << row->waitReason
         << L"\r\nR0Status: " << row->r0Status
         << L"\r\nActiveExWorker: " << (row->workerKnown ? (row->activeWorker ? L"true" : L"false") : L"unknown")
         << L"\r\nModule: " << (row->module.name.empty() ? L"<未解析>" : row->module.name)
         << L"\r\nModuleBase/Size: " << Hex(row->module.base) << L" / " << row->module.size
         << L"\r\nModulePath: " << row->module.path
         << L"\r\n\r\n操作时驱动会复核 TID，以及当前可用的启动地址和创建时间。";
    ::SetWindowTextW(state.detail, text.str().c_str());
}

void ApplyFilter(ViewState& state) {
    const auto visible = Ksword::Ui::VirtualListView::FilterRowIndexes(state.list.rows(), WindowText(state.filter));
    state.list.setVisibleIndexes(visible);
    UpdateDetail(state);
}

void Install(ViewState& state, Snapshot&& snapshot) {
    state.threads = std::move(snapshot.threads);
    std::vector<Ksword::Ui::VirtualListRow> rows;
    rows.reserve(state.threads.size());
    for (const auto& thread : state.threads) {
        Ksword::Ui::VirtualListRow row;
        row.stableKey = std::to_wstring(thread.tid);
        row.cells = {
            std::to_wstring(thread.tid), Hex(thread.start),
            thread.module.name.empty() ? L"<未解析>" : thread.module.name,
            std::to_wstring(thread.state), std::to_wstring(thread.waitReason),
            std::to_wstring(thread.priority) + L"/" + std::to_wstring(thread.basePriority),
            thread.workerKnown ? (thread.activeWorker ? L"是" : L"否") : L"未知",
            std::to_wstring(thread.r0Status), std::to_wstring(thread.createTime),
            thread.module.path
        };
        rows.push_back(std::move(row));
    }
    state.list.setRows(std::move(rows));
    ApplyFilter(state);
    std::wstring status = L"System(PID 4)：读取 " + std::to_wstring(state.threads.size()) + L" 条线程";
    if (!snapshot.r3Available) status += L"；R3 枚举不可用";
    if (!snapshot.r0Available) status += L"；R0 不可用，Win32=" + std::to_wstring(snapshot.r0Error);
    if (!snapshot.modulesAvailable) status += L"；模块归属不可用";
    ::SetWindowTextW(state.status, status.c_str());
}

void BeginRefresh(ViewState& state) {
    if (!state.task) return;
    ::EnableWindow(state.refresh, FALSE);
    ::SetWindowTextW(state.status, L"正在读取系统线程与驱动归属…");
    state.task->request([] { return Collect(); },
        [&state](std::uint64_t, std::optional<Snapshot>&& snapshot, std::exception_ptr error) {
            ::EnableWindow(state.refresh, TRUE);
            if (error || !snapshot) {
                ::SetWindowTextW(state.status, L"系统线程快照读取异常结束。");
                return;
            }
            Install(state, std::move(*snapshot));
        });
}

bool Confirm(HWND hwnd, const std::wstring& title, const std::wstring& body) {
    return ::MessageBoxW(hwnd, body.c_str(), title.c_str(),
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

void RunAction(ViewState& state, unsigned long action) {
    if (!state.actionTask || state.actionTask->running()) return;
    const ThreadRow* selected = Selected(state);
    if (!selected) return;
    const ThreadRow row = *selected;
    const bool terminating = action == KSWORD_ARK_DRIVER_THREAD_ACTION_TERMINATE;
    const wchar_t* verb = terminating ? L"终止" :
        (action == KSWORD_ARK_DRIVER_THREAD_ACTION_SUSPEND ? L"挂起" : L"恢复");
    std::wstring identity = L"TID=" + std::to_wstring(row.tid) + L"\nStart=" + Hex(row.start) +
        L"\nCreateTime100ns=" + std::to_wstring(row.createTime) +
        L"\nModule=" + (row.module.name.empty() ? L"<未解析>" : row.module.name);
    const std::wstring warning = std::wstring(L"即将") + verb +
        L" System(PID 4) 驱动线程。此操作可能造成设备失效、数据丢失、死锁或蓝屏。\n\n" +
        identity + L"\n\n确认继续？";
    if (!Confirm(state.hwnd, std::wstring(verb) + L"系统线程", warning)) return;
    if (terminating && !Confirm(state.hwnd, L"最终终止确认",
        L"终止系统线程不可逆，目标驱动可能无法恢复。\n\n再次确认终止 TID " +
        std::to_wstring(row.tid) + L"？")) return;
    const unsigned long method = terminating
        ? KSWORD_ARK_DRIVER_THREAD_TERMINATE_METHOD_NORMAL_APC
        : KSWORD_ARK_DRIVER_THREAD_TERMINATE_METHOD_NONE;
    const auto lease = Ksword::Ui::TryAcceptBackgroundWork();
    if (!lease) return;
    const std::wstring verbText = verb;
    state.actionTask->request(
        [row, action, method, lease] {
            return ksword::ark::DriverClient().controlDriverThread(
                row.tid, row.start, row.createTime, action, method,
                action != KSWORD_ARK_DRIVER_THREAD_ACTION_RESUME);
        },
        [&state, verbText](std::uint64_t, std::optional<ksword::ark::IoResult>&& result, std::exception_ptr error) {
            UpdateDetail(state);
            if (error || !result) return;
            const std::wstring message = result->ok
                ? verbText + L"系统线程成功。"
                : verbText + L"系统线程失败：Win32=" + std::to_wstring(result->win32Error) +
                    L"\n" + WideFromUtf8(result->message);
            if (Ksword::Ui::BackgroundWorkRegistry::global().closing()) {
                ::SetWindowTextW(state.status, message.c_str());
                ::OutputDebugStringW(message.c_str());
                return;
            }
            ::MessageBoxW(state.hwnd, message.c_str(), L"系统线程操作",
                MB_OK | (result->ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            BeginRefresh(state);
        });
    UpdateDetail(state);
}

void ShowRowMenu(ViewState& state, POINT screen) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    const bool selected = Selected(state) != nullptr;
    ::AppendMenuW(menu, MF_STRING | (selected ? 0U : MF_GRAYED), kMenuSuspend, L"挂起系统线程");
    ::AppendMenuW(menu, MF_STRING | (selected ? 0U : MF_GRAYED), kMenuResume, L"恢复系统线程");
    ::AppendMenuW(menu, MF_STRING | (selected ? 0U : MF_GRAYED), kMenuTerminate, L"终止系统线程");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (selected ? 0U : MF_GRAYED), kMenuCopy, L"复制详情");
    const UINT command = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, state.hwnd, nullptr));
    ::DestroyMenu(menu);
    switch (command) {
    case kMenuSuspend: RunAction(state, KSWORD_ARK_DRIVER_THREAD_ACTION_SUSPEND); break;
    case kMenuResume: RunAction(state, KSWORD_ARK_DRIVER_THREAD_ACTION_RESUME); break;
    case kMenuTerminate: RunAction(state, KSWORD_ARK_DRIVER_THREAD_ACTION_TERMINATE); break;
    case kMenuCopy:
        Ksword::Ui::CopyTextToClipboard(state.hwnd, WindowText(state.detail));
        break;
    default: break;
    }
}

void Layout(ViewState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = std::max(0L, rc.right - rc.left);
    const int height = std::max(0L, rc.bottom - rc.top);
    constexpr int gap = 7;
    constexpr int toolHeight = 25;
    ::MoveWindow(state.refresh, gap, gap, 58, toolHeight, TRUE);
    ::MoveWindow(state.suspend, 72, gap, 58, toolHeight, TRUE);
    ::MoveWindow(state.resume, 137, gap, 58, toolHeight, TRUE);
    ::MoveWindow(state.terminate, 202, gap, 62, toolHeight, TRUE);
    ::MoveWindow(state.filter, 272, gap, std::max(80, width - 279), toolHeight, TRUE);
    const int tableTop = 39;
    const int detailHeight = std::clamp(height / 3, 110, 220);
    const int tableHeight = std::max(50, height - tableTop - detailHeight - 32);
    ::MoveWindow(state.list.hwnd(), gap, tableTop, std::max(50, width - 2 * gap), tableHeight, TRUE);
    ::MoveWindow(state.detail, gap, tableTop + tableHeight + gap,
        std::max(50, width - 2 * gap), detailHeight, TRUE);
    ::MoveWindow(state.status, gap, height - 20, std::max(50, width - 2 * gap), 18, TRUE);
}

bool CreateChildren(ViewState& state) {
    state.refresh = Ksword::Ui::CreateButton(state.hwnd, kRefreshId, L"刷新", 0, 0, 0, 0);
    state.suspend = Ksword::Ui::CreateButton(state.hwnd, kSuspendId, L"挂起", 0, 0, 0, 0);
    state.resume = Ksword::Ui::CreateButton(state.hwnd, kResumeId, L"恢复", 0, 0, 0, 0);
    state.terminate = Ksword::Ui::CreateButton(state.hwnd, kTerminateId, L"终止", 0, 0, 0, 0);
    state.filter = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterId)),
        ::GetModuleHandleW(nullptr), nullptr);
    state.detail = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailId)),
        ::GetModuleHandleW(nullptr), nullptr);
    state.status = Ksword::Ui::CreateText(state.hwnd, kStatusId, L"等待刷新。", 0, 0, 0, 0);
    if (!state.refresh || !state.suspend || !state.resume || !state.terminate ||
        !state.filter || !state.detail || !state.status ||
        !state.list.create(state.hwnd, kListId, 0, 0, 0, 0)) return false;
    state.list.addColumns({
        { 0, 80, LVCFMT_LEFT, L"TID" },
        { 1, 150, LVCFMT_LEFT, L"启动地址" },
        { 2, 150, LVCFMT_LEFT, L"模块" },
        { 3, 65, LVCFMT_LEFT, L"状态" },
        { 4, 85, LVCFMT_LEFT, L"等待原因" },
        { 5, 80, LVCFMT_LEFT, L"优先级" },
        { 6, 85, LVCFMT_LEFT, L"工作线程" },
        { 7, 80, LVCFMT_LEFT, L"R0状态" },
        { 8, 150, LVCFMT_LEFT, L"创建时间" },
        { 9, 320, LVCFMT_LEFT, L"模块路径" }
    });
    Ksword::Ui::SetWindowFontRecursive(state.hwnd);
    UpdateDetail(state);
    return true;
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ViewState* state = State(hwnd);
    if (message == WM_NCCREATE) {
        state = new ViewState();
        state->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_CREATE:
        if (!state || !CreateChildren(*state)) return -1;
        state->task = std::make_unique<Ksword::Ui::AsyncSnapshotTask<Snapshot>>(hwnd, kRefreshCompleted);
        state->actionTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ksword::ark::IoResult>>(hwnd, kActionCompleted);
        Layout(*state);
        BeginRefresh(*state);
        return 0;
    case WM_SIZE:
        if (state) Layout(*state);
        return 0;
    case kActionCompleted:
        if (state && state->actionTask && state->actionTask->consume(hwnd, wParam, lParam)) return 0;
        break;
    case kRefreshCompleted:
        if (state && state->task && state->task->consume(hwnd, wParam, lParam)) return 0;
        break;
    case WM_COMMAND:
        if (state) {
            if (LOWORD(wParam) == kFilterId && HIWORD(wParam) == EN_CHANGE) {
                ApplyFilter(*state);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case kRefreshId: BeginRefresh(*state); return 0;
                case kSuspendId: RunAction(*state, KSWORD_ARK_DRIVER_THREAD_ACTION_SUSPEND); return 0;
                case kResumeId: RunAction(*state, KSWORD_ARK_DRIVER_THREAD_ACTION_RESUME); return 0;
                case kTerminateId: RunAction(*state, KSWORD_ARK_DRIVER_THREAD_ACTION_TERMINATE); return 0;
                default: break;
                }
            }
        }
        break;
    case WM_NOTIFY:
        if (state) {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header && header->hwndFrom == state->list.hwnd()) {
                LRESULT result = 0;
                if (state->list.handleNotify(*header, result)) return result;
                if (header->code == LVN_ITEMCHANGED || header->code == NM_CLICK) {
                    UpdateDetail(*state);
                    return 0;
                }
                if (header->code == NM_RCLICK) {
                    POINT point{};
                    ::GetCursorPos(&point);
                    POINT local = point;
                    ::ScreenToClient(state->list.hwnd(), &local);
                    LVHITTESTINFO hit{};
                    hit.pt = local;
                    const int row = ListView_SubItemHitTest(state->list.hwnd(), &hit);
                    if (row >= 0) {
                        ListView_SetItemState(state->list.hwnd(), row,
                            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        UpdateDetail(*state);
                    }
                    ShowRowMenu(*state, point);
                    return 0;
                }
            }
        }
        break;
    case WM_CTLCOLORSTATIC:
        ::SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        ::SetTextColor(reinterpret_cast<HDC>(wParam), Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    case WM_NCDESTROY:
        if (state) {
            if (state->task) state->task->cancel();
            if (state->actionTask) state->actionTask->cancel();
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ViewProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kClassName;
    registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreateDriverThreadView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterClass()) return nullptr;
    return ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

std::wstring ExportDriverThreadViewTsv(HWND page) {
    ViewState* state = page ? State(page) : nullptr;
    if (!state) return {};
    return Ksword::Ui::BuildVisibleVirtualListTsv({
        L"TID", L"启动地址", L"模块", L"状态", L"等待原因",
        L"优先级", L"工作线程", L"R0状态", L"创建时间", L"模块路径"
    }, state->list);
}

} // namespace Ksword::Features::Driver
