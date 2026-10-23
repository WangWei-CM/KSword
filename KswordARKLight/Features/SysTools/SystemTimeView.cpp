#include "SystemTimeView.h"

#include "SystemTimeInfo.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/TextFindSupport.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Ksword::Features::SysTools {
namespace {

constexpr wchar_t kSystemTimeViewClass[] = L"KswordARKLight.SysTools.SystemTimeView";

constexpr int kRefreshButtonId = 67401;
constexpr int kCopyButtonId = 67402;
constexpr int kClockTextId = 67403;
constexpr int kReportEditId = 67404;
constexpr int kLoadingOverlayId = 67405;
constexpr int kQueryButtonId = 67406;
constexpr int kApplyButtonId = 67407;
constexpr int kResetButtonId = 67408;
constexpr int kBackendComboId = 67409;
constexpr int kResolutionComboId = 67410;
constexpr int kSpeedComboId = 67411;
constexpr int kFactorEditId = 67412;
constexpr int kAcknowledgeId = 67413;
constexpr int kRiskTextId = 67414;
constexpr int kR0StatusTextId = 67415;

constexpr UINT kMsgCollectCompleted = WM_APP + 715;
constexpr UINT kMsgStatusCompleted = WM_APP + 716;
constexpr UINT kMsgControlCompleted = WM_APP + 717;

// kClockTimerId drives the header line only. The full report is not re-rendered
// on the tick because it would fight the user's selection and the find bar in
// the report pane every single second.
constexpr UINT_PTR kClockTimerId = 1;
constexpr UINT kClockIntervalMs = 1000;

constexpr int kGap = 6;
constexpr int kRowHeight = 24;
constexpr int kHeaderHeight = 192;
constexpr int kStatusHeight = 22;

int Width(const RECT& rc) {
    return rc.right > rc.left ? static_cast<int>(rc.right - rc.left) : 0;
}

int Height(const RECT& rc) {
    return rc.bottom > rc.top ? static_cast<int>(rc.bottom - rc.top) : 0;
}

struct TimeControlResult final {
    bool freshAvailable = false;
    ksword::ark::SystemTimeControlResult result;
};

struct SystemTimeViewState final {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND copyButton = nullptr;
    HWND queryButton = nullptr;
    HWND applyButton = nullptr;
    HWND resetButton = nullptr;
    HWND backendCombo = nullptr;
    HWND resolutionCombo = nullptr;
    HWND speedCombo = nullptr;
    HWND factorEdit = nullptr;
    HWND acknowledge = nullptr;
    HWND riskText = nullptr;
    HWND r0StatusText = nullptr;
    HWND clockText = nullptr;
    HWND reportEdit = nullptr;
    HWND loadingOverlay = nullptr;
    std::wstring reportText;
    std::wstring statusText = L"系统时间报告与 R0 全局变速控制。";
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<SystemTimeInfoSnapshot>> collectTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ksword::ark::SystemTimeQueryResult>> statusTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<TimeControlResult>> controlTask;
    std::uint64_t controlRevision = 0;
    bool supported = false;
    bool hypervAvailable = false;
    bool active = false;
    unsigned long generation = 0;
    unsigned long currentBackend = KSWORD_ARK_SYSTEM_TIME_BACKEND_HYPERV_SHARED_QPC;
    unsigned int clockTicks = 0;
};

bool Checked(HWND checkbox) {
    return ::SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void UpdateControlState(SystemTimeViewState& state) {
    const bool busy = state.controlTask && state.controlTask->running();
    const bool backendHyperv = ::SendMessageW(state.backendCombo, CB_GETCURSEL, 0, 0) == 0;
    ::EnableWindow(state.applyButton,
        !busy && state.supported && Checked(state.acknowledge) && (!backendHyperv || state.hypervAvailable));
    ::EnableWindow(state.resetButton, !busy && state.supported);
    ::EnableWindow(state.backendCombo, !busy && state.supported && !state.active);
    ::EnableWindow(state.resolutionCombo, !busy && state.supported && !state.active);
}

void BeginQueryStatus(SystemTimeViewState& state, const bool force = false) {
    if (!state.statusTask || (!force && state.statusTask->running())) { return; }
    const auto revision = state.controlRevision;
    state.statusTask->request(
        [] { return ksword::ark::DriverClient().querySystemTime(); },
        [&state, revision](std::uint64_t, std::optional<ksword::ark::SystemTimeQueryResult>&& query,
            std::exception_ptr error) {
            if (revision != state.controlRevision) return;
            if (error || !query || !query->io.ok) {
                state.supported = false;
                ::SetWindowTextW(state.r0StatusText,
                    query && query->unsupported ? L"R0 变速不可用：请更新驱动。" : L"R0 计时状态查询失败。");
                UpdateControlState(state);
                return;
            }
            const auto& response = query->response;
            state.generation = response.generation;
            state.currentBackend = response.backend;
            state.supported = (response.stateFlags & KSWORD_ARK_SYSTEM_TIME_STATE_SUPPORTED) != 0;
            state.active = (response.stateFlags & KSWORD_ARK_SYSTEM_TIME_STATE_ACTIVE) != 0;
            state.hypervAvailable = (response.stateFlags &
                (KSWORD_ARK_SYSTEM_TIME_STATE_HYPERV_PRESENT |
                    KSWORD_ARK_SYSTEM_TIME_STATE_HYPERV_SHARED_PAGE)) ==
                (KSWORD_ARK_SYSTEM_TIME_STATE_HYPERV_PRESENT |
                    KSWORD_ARK_SYSTEM_TIME_STATE_HYPERV_SHARED_PAGE);
            if (state.active) {
                ::SendMessageW(state.backendCombo, CB_SETCURSEL,
                    response.backend == KSWORD_ARK_SYSTEM_TIME_BACKEND_HAL_COMPAT ? 1 : 0, 0);
                ::SendMessageW(state.resolutionCombo, CB_SETCURSEL,
                    response.resolutionMode == KSWORD_ARK_SYSTEM_TIME_RESOLUTION_GUARDED ? 1 : 0, 0);
            }
            std::wstring status = state.supported ? L"R0 计时：" : L"R0 当前系统不支持安全接管；";
            status += state.active ? L"正在变速" : L"原始速度 1x";
            status += L"；倍率=" + std::to_wstring(response.factor) +
                L"；代次=" + std::to_wstring(response.generation) +
                L"；状态=" + std::to_wstring(response.status);
            ::SetWindowTextW(state.r0StatusText, status.c_str());
            UpdateControlState(state);
        });
}

void ControlSpeed(SystemTimeViewState& state, bool reset) {
    if (!state.supported || !state.controlTask || state.controlTask->running()) { return; }
    unsigned long factor = 1;
    if (!reset) {
        if (!Checked(state.acknowledge)) { return; }
        wchar_t factorText[24]{};
        ::GetWindowTextW(state.factorEdit, factorText, static_cast<int>(std::size(factorText)));
        wchar_t* end = nullptr;
        factor = std::wcstoul(factorText, &end, 10);
        if (end == factorText || *end != L'\0' || factor < KSWORD_ARK_SYSTEM_TIME_MIN_FACTOR ||
            factor > KSWORD_ARK_SYSTEM_TIME_MAX_FACTOR) {
            ::MessageBoxW(state.hwnd, L"倍率必须是 2 到 64 的整数。", L"系统全局变速", MB_OK | MB_ICONWARNING);
            return;
        }
        const wchar_t* warning =
            L"系统全局变速会接管内核性能计数器，也可能改写 Hyper-V 共享 QPC 页。\n"
            L"动画、超时、音视频、网络和安全软件可能异常，严重时可能冻结或蓝屏。\n"
            L"请先保存工作，并准备在异常时恢复 1x。是否继续？";
        if (::MessageBoxW(state.hwnd, warning, L"系统全局变速风险确认",
                MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) { return; }
        if (::MessageBoxW(state.hwnd, L"最终确认修改整个系统的时间行为？",
                L"最终确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) { return; }
    }
    const unsigned long backend = reset ? state.currentBackend :
        (::SendMessageW(state.backendCombo, CB_GETCURSEL, 0, 0) == 0
            ? KSWORD_ARK_SYSTEM_TIME_BACKEND_HYPERV_SHARED_QPC
            : KSWORD_ARK_SYSTEM_TIME_BACKEND_HAL_COMPAT);
    const unsigned long resolution = ::SendMessageW(state.resolutionCombo, CB_GETCURSEL, 0, 0) == 1
        ? KSWORD_ARK_SYSTEM_TIME_RESOLUTION_GUARDED
        : KSWORD_ARK_SYSTEM_TIME_RESOLUTION_ORIGINAL_COMPAT;
    const unsigned long command = reset ? KSWORD_ARK_SYSTEM_TIME_COMMAND_RESET :
        (::SendMessageW(state.speedCombo, CB_GETCURSEL, 0, 0) == 0
            ? KSWORD_ARK_SYSTEM_TIME_COMMAND_SPEED_UP
            : KSWORD_ARK_SYSTEM_TIME_COMMAND_SLOW_DOWN);
    ++state.controlRevision;
    const auto lease = Ksword::Ui::TryAcceptBackgroundWork();
    if (!lease) return;
    state.controlTask->request(
        [command, factor, backend, resolution, reset, lease] {
            TimeControlResult outcome;
            const ksword::ark::DriverClient client;
            const auto fresh = client.querySystemTime();
            outcome.freshAvailable = fresh.io.ok;
            if (outcome.freshAvailable) {
                outcome.result = client.controlSystemTime(command, factor, backend, resolution,
                    fresh.response.generation, !reset);
            }
            return outcome;
        },
        [&state, reset](std::uint64_t, std::optional<TimeControlResult>&& outcome, std::exception_ptr error) {
            if (error || !outcome) {
                state.statusText = L"R0 计时状态查询失败。";
            } else if (!outcome->freshAvailable) {
                state.statusText = L"控制前无法读取 R0 状态，未执行修改。";
            } else {
                const auto& result = outcome->result;
                if (!result.io.ok || result.response.status != KSWORD_ARK_SYSTEM_TIME_STATUS_OK) {
                    state.statusText = L"R0 计时控制失败：Win32=" + std::to_wstring(result.io.win32Error) +
                        L"，状态=" + std::to_wstring(result.response.status) +
                        L"，NTSTATUS=" + std::to_wstring(static_cast<unsigned long>(result.response.lastStatus));
                } else {
                    state.statusText = reset ? L"已请求恢复连续计数 1x；关闭页面不会自动恢复变速。"
                        : L"已应用全局变速；关闭页面不会自动恢复，请用“恢复 1x”。";
                    ::SendMessageW(state.acknowledge, BM_SETCHECK, BST_UNCHECKED, 0);
                }
            }
            BeginQueryStatus(state, true);
            UpdateControlState(state);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
    UpdateControlState(state);
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
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

void UpdateClockLine(SystemTimeViewState& state) {
    if (state.clockText) {
        ::SetWindowTextW(state.clockText, FormatLiveClockLine().c_str());
    }
}

void BeginCollect(SystemTimeViewState& state) {
    if (!state.collectTask) {
        return;
    }
    if (state.refreshButton) {
        ::EnableWindow(state.refreshButton, FALSE);
    }
    Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在读取时间与 W32Time 配置…");
    state.statusText = L"正在后台读取时区与 NTP 配置…";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);

    state.collectTask->request(
        [] { return CollectSystemTimeInfo(); },
        [&state](std::uint64_t, std::optional<SystemTimeInfoSnapshot>&& snapshot, std::exception_ptr error) {
            if (state.refreshButton) {
                ::EnableWindow(state.refreshButton, TRUE);
            }
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !snapshot.has_value()) {
                state.statusText = L"系统时间信息读取异常结束。";
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            state.reportText = RenderSystemTimeReport(*snapshot);
            if (state.reportEdit) {
                ::SetWindowTextW(state.reportEdit, state.reportText.c_str());
            }
            state.statusText = L"时间报告已刷新。按 Ctrl+F 可在报告中查找。";
            UpdateClockLine(state);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
}

void LayoutView(SystemTimeViewState& state) {
    RECT client{};
    ::GetClientRect(state.hwnd, &client);
    const int width = Width(client);
    const int height = Height(client);

    const int firstRowY = kGap;
    if (state.refreshButton) {
        ::MoveWindow(state.refreshButton, kGap, firstRowY, 64, kRowHeight, TRUE);
    }
    if (state.copyButton) {
        ::MoveWindow(state.copyButton, kGap * 2 + 64, firstRowY, 96, kRowHeight, TRUE);
    }
    ::MoveWindow(state.queryButton, 174, firstRowY, 80, kRowHeight, TRUE);
    ::MoveWindow(state.applyButton, 260, firstRowY, 88, kRowHeight, TRUE);
    ::MoveWindow(state.resetButton, 354, firstRowY, 88, kRowHeight, TRUE);

    const int secondRowY = firstRowY + kRowHeight + kGap;
    if (state.clockText) {
        ::MoveWindow(state.clockText, kGap, secondRowY + 2, (std::max)(120, width - kGap * 2), kRowHeight - 2, TRUE);
    }
    ::MoveWindow(state.riskText, kGap, 62, (std::max)(120, width - kGap * 2), 48, TRUE);
    ::MoveWindow(state.backendCombo, kGap, 116, 185, 180, TRUE);
    ::MoveWindow(state.resolutionCombo, 197, 116, 140, 180, TRUE);
    ::MoveWindow(state.speedCombo, 343, 116, 125, 180, TRUE);
    ::MoveWindow(state.factorEdit, 474, 116, 52, kRowHeight, TRUE);
    ::MoveWindow(state.acknowledge, kGap, 145, (std::max)(120, width - kGap * 2), kRowHeight, TRUE);
    ::MoveWindow(state.r0StatusText, kGap, 170, (std::max)(120, width - kGap * 2), 20, TRUE);

    const int reportTop = kHeaderHeight;
    const int reportHeight = (std::max)(0, height - reportTop - kStatusHeight - kGap);
    if (state.reportEdit) {
        ::MoveWindow(state.reportEdit, kGap, reportTop, (std::max)(0, width - kGap * 2), reportHeight, TRUE);
    }
    if (state.loadingOverlay) {
        ::MoveWindow(state.loadingOverlay, kGap, reportTop, (std::max)(0, width - kGap * 2), reportHeight, TRUE);
    }
}

bool CreateChildControls(SystemTimeViewState& state) {
    HWND hwnd = state.hwnd;
    state.refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.copyButton = Ksword::Ui::CreateButton(hwnd, kCopyButtonId, L"复制报告", 0, 0, 0, 0);
    state.queryButton = Ksword::Ui::CreateButton(hwnd, kQueryButtonId, L"R0状态", 0, 0, 0, 0);
    state.applyButton = Ksword::Ui::CreateButton(hwnd, kApplyButtonId, L"应用变速", 0, 0, 0, 0);
    state.resetButton = Ksword::Ui::CreateButton(hwnd, kResetButtonId, L"恢复 1x", 0, 0, 0, 0);
    state.clockText = Ksword::Ui::CreateText(hwnd, kClockTextId, L"", 0, 0, 0, 0);
    state.riskText = Ksword::Ui::CreateText(hwnd, kRiskTextId,
        L"⚠ 系统全局变速会接管内核性能计数器，Hyper-V 后端还会改写共享 QPC 页。"
        L"可能导致系统冻结或蓝屏；请先保存工作，建议仅在虚拟机中使用。关闭页面不会自动恢复。",
        0, 0, 0, 0);
    state.r0StatusText = Ksword::Ui::CreateText(hwnd, kR0StatusTextId,
        L"R0 状态等待查询…", 0, 0, 0, 0);
    auto createCombo = [hwnd](int id) {
        return ::CreateWindowExW(0, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            0, 0, 1, 180, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            ::GetModuleHandleW(nullptr), nullptr);
    };
    state.backendCombo = createCombo(kBackendComboId);
    state.resolutionCombo = createCombo(kResolutionComboId);
    state.speedCombo = createCombo(kSpeedComboId);
    state.factorEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFactorEditId)),
        ::GetModuleHandleW(nullptr), nullptr);
    state.acknowledge = ::CreateWindowExW(0, L"BUTTON",
        L"我已保存工作，并理解该功能可能使系统不稳定",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAcknowledgeId)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!state.refreshButton || !state.copyButton || !state.queryButton ||
        !state.applyButton || !state.resetButton || !state.clockText || !state.riskText ||
        !state.r0StatusText || !state.backendCombo || !state.resolutionCombo ||
        !state.speedCombo || !state.factorEdit || !state.acknowledge) {
        return false;
    }
    auto addCombo = [](HWND combo, const wchar_t* text) {
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    };
    addCombo(state.backendCombo, L"Hyper-V 共享 QPC");
    addCombo(state.backendCombo, L"HAL 兼容后端");
    addCombo(state.resolutionCombo, L"兼容模式");
    addCombo(state.resolutionCombo, L"安全模式");
    addCombo(state.speedCombo, L"加速 N 倍");
    addCombo(state.speedCombo, L"减速到 1/N");
    ::SendMessageW(state.backendCombo, CB_SETCURSEL, 0, 0);
    ::SendMessageW(state.resolutionCombo, CB_SETCURSEL, 0, 0);
    ::SendMessageW(state.speedCombo, CB_SETCURSEL, 0, 0);

    state.reportEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"正在读取系统时间信息…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReportEditId)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!state.reportEdit) {
        return false;
    }
    ::SendMessageW(state.reportEdit, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    // The W32Time parameter block alone can run to dozens of lines, so the pane
    // gets the shared Ctrl+F find bar instead of leaving the reader to scroll.
    Ksword::Ui::AttachTextFindSupport(state.reportEdit);

    state.loadingOverlay = Ksword::Ui::CreateLoadingOverlay(hwnd, kLoadingOverlayId, { 0, 0, 1, 1 });
    if (!state.loadingOverlay) {
        return false;
    }

    Ksword::Ui::SetWindowFontRecursive(hwnd);
    UpdateControlState(state);
    return true;
}

LRESULT CALLBACK SystemTimeViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SystemTimeViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto owned = std::make_unique<SystemTimeViewState>();
        owned->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
        return TRUE;
    }
    case WM_CREATE:
        if (state) {
            if (!CreateChildControls(*state)) {
                return -1;
            }
            state->collectTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<SystemTimeInfoSnapshot>>(hwnd, kMsgCollectCompleted);
            state->statusTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ksword::ark::SystemTimeQueryResult>>(hwnd, kMsgStatusCompleted);
            state->controlTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<TimeControlResult>>(hwnd, kMsgControlCompleted);
            LayoutView(*state);
            UpdateClockLine(*state);
            ::SetTimer(hwnd, kClockTimerId, kClockIntervalMs, nullptr);
            BeginCollect(*state);
            BeginQueryStatus(*state);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            LayoutView(*state);
        }
        return 0;
    case WM_TIMER:
        if (state && wParam == kClockTimerId) {
            UpdateClockLine(*state);
            if (++state->clockTicks % 2 == 0) { BeginQueryStatus(*state); }
            return 0;
        }
        break;
    case WM_COMMAND:
        if (state && LOWORD(wParam) == kBackendComboId && HIWORD(wParam) == CBN_SELCHANGE) {
            UpdateControlState(*state);
            return 0;
        }
        if (state && HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case kAcknowledgeId:
                UpdateControlState(*state);
                return 0;
            case kQueryButtonId:
                BeginQueryStatus(*state);
                return 0;
            case kApplyButtonId:
                ControlSpeed(*state, false);
                return 0;
            case kResetButtonId:
                ControlSpeed(*state, true);
                return 0;
            case kRefreshButtonId:
                BeginCollect(*state);
                return 0;
            case kCopyButtonId:
                state->statusText = CopyText(hwnd, state->reportText) ? L"已复制系统时间报告。" : L"复制失败。";
                ::InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd, &paint);
            RECT client{};
            ::GetClientRect(hwnd, &client);
            ::FillRect(dc, &client, Ksword::Ui::AppTheme().windowBrush());
            RECT statusRect{ kGap, client.bottom - kStatusHeight, client.right - kGap, client.bottom };
            Ksword::Ui::DrawTextLine(dc, state->statusText, statusRect,
                Ksword::Ui::AppTheme().mutedTextColor, Ksword::Ui::SystemUIFont(),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    default:
        if (state && msg == kMsgCollectCompleted && state->collectTask) {
            state->collectTask->consume(hwnd, wParam, lParam);
            return 0;
        }
        if (state && msg == kMsgControlCompleted && state->controlTask) {
            state->controlTask->consume(hwnd, wParam, lParam);
            return 0;
        }
        if (state && msg == kMsgStatusCompleted && state->statusTask) {
            state->statusTask->consume(hwnd, wParam, lParam);
            return 0;
        }
        if (msg == WM_NCDESTROY && state) {
            ::KillTimer(hwnd, kClockTimerId);
            if (state->collectTask) {
                state->collectTask->cancel();
            }
            if (state->statusTask) { state->statusTask->cancel(); }
            if (state->controlTask) { state->controlTask->cancel(); }
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureSystemTimeViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = SystemTimeViewProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    windowClass.lpszClassName = kSystemTimeViewClass;
    registered = ::RegisterClassW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreateSystemTimeView(HWND parent, const RECT& bounds) {
    if (!parent || !EnsureSystemTimeViewClass()) {
        return nullptr;
    }
    return ::CreateWindowExW(
        0, kSystemTimeViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace Ksword::Features::SysTools
