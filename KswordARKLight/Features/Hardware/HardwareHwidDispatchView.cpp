#include "HardwareHwidDispatchView.h"

#include "../AuditCommon/AuditTable.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/Theme.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <commctrl.h>
#include <windowsx.h>

#include <array>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Hardware {
namespace {

constexpr wchar_t kHwidDispatchClass[] = L"KswordARKLight.Hardware.HwidDispatchView";
constexpr int kRefreshButtonId = 61201;
constexpr int kDryRunButtonId = 61202;
constexpr int kEnableButtonId = 61203;
constexpr int kDisableAllButtonId = 61204;
constexpr int kCopyPlanButtonId = 61205;
constexpr int kConfirmCheckId = 61206;
constexpr int kDiskCheckId = 61210;
constexpr int kPartMgrCheckId = 61211;
constexpr int kMountMgrCheckId = 61212;
constexpr int kNvidiaCheckId = 61213;
constexpr int kNsiProxyCheckId = 61214;
constexpr int kDiskGuidCheckId = 61220;
constexpr int kVolumeCleanCheckId = 61221;
constexpr int kArpCleanCheckId = 61222;
constexpr int kDiskModeComboId = 61230;
constexpr int kMacModeComboId = 61231;
constexpr int kFirstEditId = 61240;
constexpr int kStatusTextId = 61260;
constexpr int kPlanEditId = 61261;
constexpr int kStatusTableId = 61262;
constexpr UINT kMsgRefreshCompleted = WM_APP + 588;
constexpr UINT kMsgControlCompleted = WM_APP + 589;

enum HwidTextField : std::size_t {
    DiskSerialField = 0,
    DiskProductField,
    DiskRevisionField,
    GpuSerialField,
    PermanentMacField,
    CurrentMacField,
    HwidTextFieldCount
};

struct HwidDispatchViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND dryRunButton = nullptr;
    HWND enableButton = nullptr;
    HWND disableAllButton = nullptr;
    HWND copyPlanButton = nullptr;
    HWND confirmCheck = nullptr;
    HWND diskCheck = nullptr;
    HWND partMgrCheck = nullptr;
    HWND mountMgrCheck = nullptr;
    HWND nvidiaCheck = nullptr;
    HWND nsiProxyCheck = nullptr;
    HWND diskGuidCheck = nullptr;
    HWND volumeCleanCheck = nullptr;
    HWND arpCleanCheck = nullptr;
    HWND diskModeCombo = nullptr;
    HWND macModeCombo = nullptr;
    HWND statusText = nullptr;
    HWND planEdit = nullptr;
    HWND statusTable = nullptr;
    std::array<HWND, HwidTextFieldCount> labels{};
    std::array<HWND, HwidTextFieldCount> edits{};
    std::wstring logText;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ksword::ark::HwidDispatchResult>> refreshTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ksword::ark::HwidDispatchResult>> controlTask;
    bool controlInProgress = false;
};

int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (required > 0) {
        std::wstring wide(static_cast<std::size_t>(required), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), wide.data(), required);
        return wide;
    }
    std::wstring fallback;
    fallback.reserve(text.size());
    for (const unsigned char ch : text) {
        fallback.push_back(static_cast<wchar_t>(ch));
    }
    return fallback;
}

std::wstring Hex32(const long value) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
           << static_cast<std::uint32_t>(value);
    return stream.str();
}

std::wstring Hex64(const unsigned long long value) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << value;
    return stream.str();
}

std::wstring FixedWide(const wchar_t* text, const std::size_t maxChars) {
    if (!text || maxChars == 0U) {
        return {};
    }
    std::size_t length = 0;
    while (length < maxChars && text[length] != L'\0') {
        ++length;
    }
    return std::wstring(text, text + length);
}

std::wstring TextFromWindow(HWND hwnd) {
    if (!hwnd) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    if (length > 0) {
        ::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void CopyTextToWideField(const std::wstring& text, wchar_t* target, const std::size_t targetChars) {
    if (!target || targetChars == 0U) {
        return;
    }
    target[0] = L'\0';
    if (!text.empty()) {
        ::wcsncpy_s(target, targetChars, text.c_str(), _TRUNCATE);
    }
}

bool WriteClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
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
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

HWND CreateCheck(HWND parent, int id, const wchar_t* text, bool checked) {
    HWND hwnd = ::CreateWindowExW(
        0,
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0,
        0,
        0,
        0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
        Button_SetCheck(hwnd, checked ? BST_CHECKED : BST_UNCHECKED);
    }
    return hwnd;
}

HWND CreateEdit(HWND parent, int id) {
    HWND hwnd = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    return hwnd;
}

void AddComboItem(HWND combo, const wchar_t* text, unsigned long value) {
    const LRESULT index = ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    if (index >= 0) {
        ::SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(value));
    }
}

unsigned long ComboData(HWND combo, unsigned long fallback) {
    if (!combo) {
        return fallback;
    }
    const LRESULT index = ::SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0) {
        return fallback;
    }
    const LRESULT data = ::SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    return data == CB_ERR ? fallback : static_cast<unsigned long>(data);
}

const wchar_t* TargetNameFromFlag(const unsigned long targetFlag) {
    switch (targetFlag) {
    case KSWORD_ARK_HWID_DISPATCH_TARGET_DISK: return L"\\Driver\\Disk";
    case KSWORD_ARK_HWID_DISPATCH_TARGET_PARTMGR: return L"\\Driver\\partmgr";
    case KSWORD_ARK_HWID_DISPATCH_TARGET_MOUNTMGR: return L"\\Driver\\mountmgr";
    case KSWORD_ARK_HWID_DISPATCH_TARGET_NVIDIA: return L"\\Driver\\nvlddmkm";
    case KSWORD_ARK_HWID_DISPATCH_TARGET_NSIPROXY: return L"\\Driver\\nsiproxy";
    default: return L"<unknown>";
    }
}

const wchar_t* OverallStatusText(const unsigned long status) {
    switch (status) {
    case KSWORD_ARK_HWID_DISPATCH_STATUS_READY: return L"Ready";
    case KSWORD_ARK_HWID_DISPATCH_STATUS_ACTIVE: return L"Active";
    case KSWORD_ARK_HWID_DISPATCH_STATUS_PARTIAL: return L"Partial";
    case KSWORD_ARK_HWID_DISPATCH_STATUS_UNSUPPORTED: return L"Unsupported";
    case KSWORD_ARK_HWID_DISPATCH_STATUS_DENIED: return L"Denied";
    case KSWORD_ARK_HWID_DISPATCH_STATUS_FAILED: return L"Failed";
    default: return L"Unknown";
    }
}

bool Checked(HWND hwnd) {
    return hwnd && Button_GetCheck(hwnd) == BST_CHECKED;
}

unsigned long SelectedTargetFlags(const HwidDispatchViewState& state) {
    unsigned long flags = 0UL;
    flags |= Checked(state.diskCheck) ? KSWORD_ARK_HWID_DISPATCH_TARGET_DISK : 0UL;
    flags |= Checked(state.partMgrCheck) ? KSWORD_ARK_HWID_DISPATCH_TARGET_PARTMGR : 0UL;
    flags |= Checked(state.mountMgrCheck) ? KSWORD_ARK_HWID_DISPATCH_TARGET_MOUNTMGR : 0UL;
    flags |= Checked(state.nvidiaCheck) ? KSWORD_ARK_HWID_DISPATCH_TARGET_NVIDIA : 0UL;
    flags |= Checked(state.nsiProxyCheck) ? KSWORD_ARK_HWID_DISPATCH_TARGET_NSIPROXY : 0UL;
    return flags;
}

std::wstring BuildPlanText(const HwidDispatchViewState& state) {
    std::wostringstream stream;
    stream << L"HWID Dispatch 派遣函数接入计划\r\n"
           << L"来源: FiYHer/EASY-HWID-SPOOFER dispatch-only 方案\r\n"
           << L"目标 flags: 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << SelectedTargetFlags(state) << std::dec << L"\r\n"
           << L"- \\Driver\\Disk: " << (Checked(state.diskCheck) ? L"启用" : L"跳过") << L"\r\n"
           << L"- \\Driver\\partmgr: " << (Checked(state.partMgrCheck) ? L"启用" : L"跳过") << L"\r\n"
           << L"- \\Driver\\mountmgr: " << (Checked(state.mountMgrCheck) ? L"启用" : L"跳过") << L"\r\n"
           << L"- \\Driver\\nvlddmkm: " << (Checked(state.nvidiaCheck) ? L"启用" : L"跳过") << L"\r\n"
           << L"- \\Driver\\nsiproxy: " << (Checked(state.nsiProxyCheck) ? L"启用" : L"跳过") << L"\r\n"
           << L"磁盘模式: " << ComboData(state.diskModeCombo, KSWORD_ARK_HWID_DISPATCH_DISK_MODE_CUSTOM) << L"\r\n"
           << L"MAC 模式(预留): " << ComboData(state.macModeCombo, KSWORD_ARK_HWID_DISPATCH_MAC_MODE_RANDOM) << L"\r\n"
           << L"磁盘序列号: " << TextFromWindow(state.edits[DiskSerialField]) << L"\r\n"
           << L"磁盘产品名: " << TextFromWindow(state.edits[DiskProductField]) << L"\r\n"
           << L"磁盘固件值: " << TextFromWindow(state.edits[DiskRevisionField]) << L"\r\n"
           << L"GPU 序列号: " << TextFromWindow(state.edits[GpuSerialField]) << L"\r\n"
           << L"永久 MAC: " << TextFromWindow(state.edits[PermanentMacField]) << L"\r\n"
           << L"当前 MAC: " << TextFromWindow(state.edits[CurrentMacField]) << L"\r\n"
           << L"GPT GUID 随机化: " << (Checked(state.diskGuidCheck) ? L"是" : L"否") << L"\r\n"
           << L"卷唯一标识清理: " << (Checked(state.volumeCleanCheck) ? L"是" : L"否") << L"\r\n"
           << L"ARP Table 清理: " << (Checked(state.arpCleanCheck) ? L"是" : L"否") << L"\r\n"
           << L"风险: 启用/卸载 Dispatch hook 可能蓝屏；真实操作需要勾选确认并二次确认。";
    if (!state.logText.empty()) {
        stream << L"\r\n\r\n--- 日志 ---\r\n" << state.logText;
    }
    return stream.str();
}

void UpdatePlanText(HwidDispatchViewState& state) {
    if (state.planEdit) {
        const std::wstring plan = BuildPlanText(state);
        ::SetWindowTextW(state.planEdit, plan.c_str());
    }
}

void AppendLog(HwidDispatchViewState& state, const std::wstring& line) {
    if (!state.logText.empty()) {
        state.logText += L"\r\n";
    }
    state.logText += line;
    UpdatePlanText(state);
}

KSWORD_ARK_HWID_DISPATCH_CONTROL_REQUEST BuildControlRequest(
    const HwidDispatchViewState& state,
    const unsigned long action,
    const bool dryRun) {
    KSWORD_ARK_HWID_DISPATCH_CONTROL_REQUEST request{};
    request.size = sizeof(request);
    request.version = KSWORD_ARK_HWID_DISPATCH_PROTOCOL_VERSION;
    request.action = action;
    request.requestFlags = KSWORD_ARK_HWID_DISPATCH_REQUEST_FLAG_UI_CONFIRMED;
    if (dryRun) {
        request.requestFlags |= KSWORD_ARK_HWID_DISPATCH_REQUEST_FLAG_DRY_RUN;
    }
    request.profile.size = sizeof(request.profile);
    request.profile.version = KSWORD_ARK_HWID_DISPATCH_PROTOCOL_VERSION;
    request.profile.targetFlags = SelectedTargetFlags(state);
    request.profile.diskMode = ComboData(state.diskModeCombo, KSWORD_ARK_HWID_DISPATCH_DISK_MODE_CUSTOM);
    request.profile.macMode = ComboData(state.macModeCombo, KSWORD_ARK_HWID_DISPATCH_MAC_MODE_RANDOM);
    request.profile.behaviorFlags =
        (Checked(state.diskGuidCheck) ? KSWORD_ARK_HWID_DISPATCH_FLAG_DISK_GUID_RANDOM : 0UL) |
        (Checked(state.volumeCleanCheck) ? KSWORD_ARK_HWID_DISPATCH_FLAG_VOLUME_ID_CLEAN : 0UL) |
        (Checked(state.arpCleanCheck) ? KSWORD_ARK_HWID_DISPATCH_FLAG_ARP_TABLE_CLEAN : 0UL);
    CopyTextToWideField(TextFromWindow(state.edits[DiskSerialField]), request.profile.diskSerial, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS);
    CopyTextToWideField(TextFromWindow(state.edits[DiskProductField]), request.profile.diskProduct, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS);
    CopyTextToWideField(TextFromWindow(state.edits[DiskRevisionField]), request.profile.diskRevision, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS);
    CopyTextToWideField(TextFromWindow(state.edits[GpuSerialField]), request.profile.gpuSerial, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS);
    CopyTextToWideField(TextFromWindow(state.edits[PermanentMacField]), request.profile.permanentMac, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS);
    CopyTextToWideField(TextFromWindow(state.edits[CurrentMacField]), request.profile.currentMac, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS);
    return request;
}

void ApplyResponseToUi(HwidDispatchViewState& state, const ksword::ark::HwidDispatchResult& result) {
    if (state.statusText) {
        std::wostringstream status;
        status << L"状态：" << (result.unsupported ? L"驱动未注册 HWID Dispatch IOCTL" : (result.io.ok ? L"IOCTL 成功" : L"IOCTL 失败"))
               << L"，Overall=" << OverallStatusText(result.response.overallStatus)
               << L"，Win32=" << result.io.win32Error
               << L"，NT=" << Hex32(result.response.lastStatus)
               << L"，Active=0x" << std::hex << std::uppercase << result.response.activeTargetFlags;
        ::SetWindowTextW(state.statusText, status.str().c_str());
    }

    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(KSWORD_ARK_HWID_DISPATCH_ENTRY_COUNT);
    for (std::size_t index = 0; index < KSWORD_ARK_HWID_DISPATCH_ENTRY_COUNT; ++index) {
        const KSWORD_ARK_HWID_DISPATCH_ENTRY& entry = result.response.entries[index];
        rows.push_back({
            TargetNameFromFlag(entry.targetFlag),
            FixedWide(entry.driverName, KSWORD_ARK_HWID_DISPATCH_TEXT_CHARS),
            entry.active != 0UL ? L"是" : L"否",
            Hex32(entry.lastStatus),
            Hex64(entry.driverObjectAddress),
            Hex64(entry.originalDispatchAddress),
            Hex64(entry.currentDispatchAddress)
        });
    }
    Ksword::Features::AuditCommon::ReplaceAuditTableRows(state.statusTable, rows);
    AppendLog(state, Utf8ToWide(result.io.message));
}

void SetDispatchControlsEnabled(HwidDispatchViewState& state, bool enabled) {
    for (HWND control : { state.refreshButton, state.dryRunButton, state.enableButton, state.disableAllButton }) {
        if (control) {
            ::EnableWindow(control, enabled);
        }
    }
}

void RefreshDispatchState(HwidDispatchViewState& state) {
    if (!state.refreshTask) {
        return;
    }
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, state.refreshTask->running() ? L"状态：HWID Dispatch 查询已排队。" : L"状态：正在后台查询 HWID Dispatch。 ");
    }
    ::EnableWindow(state.refreshButton, FALSE);
    state.refreshTask->request(
        [] {
            const ksword::ark::DriverClient client;
            return client.queryHwidDispatchState();
        },
        [&state](std::uint64_t, std::optional<ksword::ark::HwidDispatchResult>&& result, std::exception_ptr error) {
            ::EnableWindow(state.refreshButton, TRUE);
            if (error || !result.has_value()) {
                AppendLog(state, L"HWID Dispatch 后台查询异常结束。");
                return;
            }
            ApplyResponseToUi(state, *result);
        });
}

void SendControlRequest(HwidDispatchViewState& state, const unsigned long action, const bool dryRun) {
    if (state.refreshTask && state.refreshTask->running()) {
        AppendLog(state, L"HWID Dispatch 状态查询尚未完成，请稍后再执行操作。");
        return;
    }
    if (!dryRun && !Checked(state.confirmCheck)) {
        ::MessageBoxW(
            state.hwnd,
            L"真实启用/卸载 Dispatch hook 前必须勾选风险确认。",
            L"HWID Dispatch",
            MB_OK | MB_ICONWARNING);
        return;
    }
    if (!dryRun) {
        const int answer = ::MessageBoxW(
            state.hwnd,
            L"即将修改或恢复内核驱动 MajorFunction 派遣函数，可能立即蓝屏。是否继续？",
            L"确认 HWID Dispatch 真实操作",
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING);
        if (answer != IDYES) {
            AppendLog(state, L"用户取消真实 Dispatch 操作。");
            return;
        }
    }

    const KSWORD_ARK_HWID_DISPATCH_CONTROL_REQUEST request = BuildControlRequest(state, action, dryRun);
    if (!state.controlTask || state.controlInProgress) {
        AppendLog(state, L"HWID Dispatch 操作正在执行。");
        return;
    }
    state.controlInProgress = true;
    SetDispatchControlsEnabled(state, false);
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, L"状态：正在后台执行 HWID Dispatch 操作…");
    }
    state.controlTask->request(
        [request] {
            const ksword::ark::DriverClient client;
            return client.controlHwidDispatch(request);
        },
        [&state](std::uint64_t, std::optional<ksword::ark::HwidDispatchResult>&& result, std::exception_ptr error) {
            state.controlInProgress = false;
            SetDispatchControlsEnabled(state, true);
            if (error || !result.has_value()) {
                AppendLog(state, L"HWID Dispatch 操作异常结束。");
                return;
            }
            ApplyResponseToUi(state, *result);
        });
}

bool CreateChildControls(HwidDispatchViewState& state) {
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kRefreshButtonId, L"查询状态", 0, 0, 0, 0);
    state.dryRunButton = Ksword::Ui::CreateButton(state.hwnd, kDryRunButtonId, L"干跑验证", 0, 0, 0, 0);
    state.enableButton = Ksword::Ui::CreateButton(state.hwnd, kEnableButtonId, L"启用派遣函数", 0, 0, 0, 0);
    state.disableAllButton = Ksword::Ui::CreateButton(state.hwnd, kDisableAllButtonId, L"卸载全部", 0, 0, 0, 0);
    state.copyPlanButton = Ksword::Ui::CreateButton(state.hwnd, kCopyPlanButtonId, L"复制计划", 0, 0, 0, 0);
    state.confirmCheck = CreateCheck(state.hwnd, kConfirmCheckId, L"我已确认真实 Dispatch hook 操作可能蓝屏，已准备恢复方案。", false);
    state.diskCheck = CreateCheck(state.hwnd, kDiskCheckId, L"\\Driver\\Disk", true);
    state.partMgrCheck = CreateCheck(state.hwnd, kPartMgrCheckId, L"\\Driver\\partmgr", true);
    state.mountMgrCheck = CreateCheck(state.hwnd, kMountMgrCheckId, L"\\Driver\\mountmgr", true);
    state.nvidiaCheck = CreateCheck(state.hwnd, kNvidiaCheckId, L"\\Driver\\nvlddmkm", false);
    state.nsiProxyCheck = CreateCheck(state.hwnd, kNsiProxyCheckId, L"\\Driver\\nsiproxy", false);
    state.diskGuidCheck = CreateCheck(state.hwnd, kDiskGuidCheckId, L"随机化 GPT GUID 查询结果", false);
    state.volumeCleanCheck = CreateCheck(state.hwnd, kVolumeCleanCheckId, L"清理 MountMgr 卷唯一标识查询结果", false);
    state.arpCleanCheck = CreateCheck(state.hwnd, kArpCleanCheckId, L"清理 ARP Table 查询结果", false);

    state.diskModeCombo = ::CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, 0, 0, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDiskModeComboId)), ::GetModuleHandleW(nullptr), nullptr);
    state.macModeCombo = ::CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, 0, 0, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMacModeComboId)), ::GetModuleHandleW(nullptr), nullptr);
    if (state.diskModeCombo) {
        ::SendMessageW(state.diskModeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
        AddComboItem(state.diskModeCombo, L"自定义序列号/产品/固件", KSWORD_ARK_HWID_DISPATCH_DISK_MODE_CUSTOM);
        AddComboItem(state.diskModeCombo, L"随机化序列号", KSWORD_ARK_HWID_DISPATCH_DISK_MODE_RANDOM);
        AddComboItem(state.diskModeCombo, L"清空序列号", KSWORD_ARK_HWID_DISPATCH_DISK_MODE_NULL);
        ::SendMessageW(state.diskModeCombo, CB_SETCURSEL, 0, 0);
    }
    if (state.macModeCombo) {
        ::SendMessageW(state.macModeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
        AddComboItem(state.macModeCombo, L"随机化物理 MAC(预留)", KSWORD_ARK_HWID_DISPATCH_MAC_MODE_RANDOM);
        AddComboItem(state.macModeCombo, L"自定义物理 MAC(预留)", KSWORD_ARK_HWID_DISPATCH_MAC_MODE_CUSTOM);
        ::SendMessageW(state.macModeCombo, CB_SETCURSEL, 0, 0);
    }

    const std::array<const wchar_t*, HwidTextFieldCount> labelTexts{
        L"磁盘序列号",
        L"磁盘产品名",
        L"磁盘固件值",
        L"GPU 序列号",
        L"永久 MAC",
        L"当前 MAC"
    };
    for (std::size_t index = 0; index < HwidTextFieldCount; ++index) {
        state.labels[index] = Ksword::Ui::CreateText(state.hwnd, kFirstEditId + static_cast<int>(index) + 100, labelTexts[index], 0, 0, 0, 0);
        state.edits[index] = CreateEdit(state.hwnd, kFirstEditId + static_cast<int>(index));
    }

    state.statusText = Ksword::Ui::CreateText(state.hwnd, kStatusTextId, L"状态：等待查询。", 0, 0, 0, 0);
    state.planEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 0, 0, state.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlanEditId)), ::GetModuleHandleW(nullptr), nullptr);
    if (state.planEdit) {
        ::SendMessageW(state.planEdit, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    RECT tableBounds{ 0, 0, 100, 100 };
    state.statusTable = Ksword::Features::AuditCommon::CreateReadOnlyAuditTable(
        state.hwnd,
        kStatusTableId,
        tableBounds,
        {
            { L"目标", 150, LVCFMT_LEFT },
            { L"驱动对象", 190, LVCFMT_LEFT },
            { L"Active", 70, LVCFMT_LEFT },
            { L"LastStatus", 110, LVCFMT_LEFT },
            { L"DriverObject", 150, LVCFMT_LEFT },
            { L"OriginalDispatch", 150, LVCFMT_LEFT },
            { L"CurrentDispatch", 150, LVCFMT_LEFT },
        });

    const bool ok = state.refreshButton && state.dryRunButton && state.enableButton &&
        state.disableAllButton && state.copyPlanButton && state.confirmCheck &&
        state.diskCheck && state.partMgrCheck && state.mountMgrCheck &&
        state.nvidiaCheck && state.nsiProxyCheck && state.diskGuidCheck &&
        state.volumeCleanCheck && state.arpCleanCheck && state.diskModeCombo &&
        state.macModeCombo && state.statusText && state.planEdit && state.statusTable;
    if (ok) {
        UpdatePlanText(state);
    }
    return ok;
}

void LayoutChildren(HwidDispatchViewState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    const int margin = 8;
    const int gap = 6;
    const int buttonH = 26;
    int x = margin;
    auto moveNextButton = [&](HWND hwnd, int buttonW) {
        ::MoveWindow(hwnd, x, margin, buttonW, buttonH, TRUE);
        x += buttonW + gap;
    };
    moveNextButton(state.refreshButton, 86);
    moveNextButton(state.dryRunButton, 86);
    moveNextButton(state.enableButton, 110);
    moveNextButton(state.disableAllButton, 94);
    moveNextButton(state.copyPlanButton, 86);
    ::MoveWindow(state.statusText, x, margin + 3, std::max(80, width - x - margin), 22, TRUE);

    const int planW = width >= 980 ? 380 : 0;
    const int leftW = planW > 0 ? std::max(320, width - planW - margin * 3) : std::max(320, width - margin * 2);
    ::MoveWindow(state.confirmCheck, margin, 40, leftW, 22, TRUE);

    int y = 66;
    const int checkW = 150;
    ::MoveWindow(state.diskCheck, margin, y, checkW, 22, TRUE);
    ::MoveWindow(state.partMgrCheck, margin + checkW, y, checkW, 22, TRUE);
    ::MoveWindow(state.mountMgrCheck, margin + checkW * 2, y, checkW, 22, TRUE);
    ::MoveWindow(state.nvidiaCheck, margin, y + 24, checkW, 22, TRUE);
    ::MoveWindow(state.nsiProxyCheck, margin + checkW, y + 24, checkW, 22, TRUE);

    y = 116;
    const int labelW = 84;
    const int editW = std::max(110, (leftW - labelW * 2 - gap * 5) / 2);
    const int col1 = margin;
    const int col2 = margin + labelW + editW + gap * 3;
    ::MoveWindow(state.labels[DiskSerialField], col1, y, labelW, 22, TRUE);
    ::MoveWindow(state.edits[DiskSerialField], col1 + labelW, y, editW, 22, TRUE);
    ::MoveWindow(state.labels[GpuSerialField], col2, y, labelW, 22, TRUE);
    ::MoveWindow(state.edits[GpuSerialField], col2 + labelW, y, editW, 22, TRUE);
    y += 26;
    ::MoveWindow(state.labels[DiskProductField], col1, y, labelW, 22, TRUE);
    ::MoveWindow(state.edits[DiskProductField], col1 + labelW, y, editW, 22, TRUE);
    ::MoveWindow(state.labels[PermanentMacField], col2, y, labelW, 22, TRUE);
    ::MoveWindow(state.edits[PermanentMacField], col2 + labelW, y, editW, 22, TRUE);
    y += 26;
    ::MoveWindow(state.labels[DiskRevisionField], col1, y, labelW, 22, TRUE);
    ::MoveWindow(state.edits[DiskRevisionField], col1 + labelW, y, editW, 22, TRUE);
    ::MoveWindow(state.labels[CurrentMacField], col2, y, labelW, 22, TRUE);
    ::MoveWindow(state.edits[CurrentMacField], col2 + labelW, y, editW, 22, TRUE);
    y += 28;
    ::MoveWindow(state.diskModeCombo, col1, y, labelW + editW, 120, TRUE);
    ::MoveWindow(state.macModeCombo, col2, y, labelW + editW, 120, TRUE);
    y += 28;
    ::MoveWindow(state.diskGuidCheck, margin, y, 210, 22, TRUE);
    ::MoveWindow(state.volumeCleanCheck, margin + 215, y, 250, 22, TRUE);
    ::MoveWindow(state.arpCleanCheck, margin + 470, y, 180, 22, TRUE);

    const int topH = 226;
    if (planW > 0) {
        ::MoveWindow(state.planEdit, width - planW - margin, 40, planW, topH - 48, TRUE);
    } else {
        ::MoveWindow(state.planEdit, margin, topH, width - margin * 2, 1, TRUE);
    }
    ::MoveWindow(state.statusTable, margin, topH, std::max(80, width - margin * 2), std::max(80, height - topH - margin), TRUE);
}

HwidDispatchViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<HwidDispatchViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

bool RegisterHwidDispatchClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        HwidDispatchViewState* state = StateFromWindow(hwnd);
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<HwidDispatchViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
        }
        switch (msg) {
        case WM_CREATE:
            if (state) {
                if (!CreateChildControls(*state)) {
                    delete state;
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return -1;
                }
                state->refreshTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ksword::ark::HwidDispatchResult>>(hwnd, kMsgRefreshCompleted);
                state->controlTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ksword::ark::HwidDispatchResult>>(hwnd, kMsgControlCompleted);
                LayoutChildren(*state);
                RefreshDispatchState(*state);
            }
            return 0;
        case WM_SIZE:
            if (state) {
                LayoutChildren(*state);
            }
            return 0;
        case kMsgRefreshCompleted:
            if (state && state->refreshTask && state->refreshTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case kMsgControlCompleted:
            if (state && state->controlTask && state->controlTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case WM_COMMAND:
            if (state) {
                switch (LOWORD(wParam)) {
                case kRefreshButtonId:
                    RefreshDispatchState(*state);
                    return 0;
                case kDryRunButtonId:
                    SendControlRequest(*state, KSWORD_ARK_HWID_DISPATCH_ACTION_ENABLE, true);
                    return 0;
                case kEnableButtonId:
                    SendControlRequest(*state, KSWORD_ARK_HWID_DISPATCH_ACTION_ENABLE, false);
                    return 0;
                case kDisableAllButtonId:
                    SendControlRequest(*state, KSWORD_ARK_HWID_DISPATCH_ACTION_DISABLE_ALL, false);
                    return 0;
                case kCopyPlanButtonId:
                    AppendLog(*state, WriteClipboardText(hwnd, BuildPlanText(*state)) ? L"已复制 HWID Dispatch 计划。" : L"复制 HWID Dispatch 计划失败。");
                    return 0;
                default:
                    if ((HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) >= kFirstEditId && LOWORD(wParam) < kFirstEditId + static_cast<int>(HwidTextFieldCount)) ||
                        HIWORD(wParam) == CBN_SELCHANGE ||
                        HIWORD(wParam) == BN_CLICKED) {
                        UpdatePlanText(*state);
                    }
                    break;
                }
            }
            break;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (state && header && header->hwndFrom == state->statusTable && header->code == NM_RCLICK) {
                POINT pt{};
                ::GetCursorPos(&pt);
                Ksword::Features::AuditCommon::ShowAuditTableContextMenu(state->hwnd, state->statusTable, pt);
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->statusTable) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->statusTable, &rc);
                    pt.x = rc.left + 24;
                    pt.y = rc.top + 24;
                }
                Ksword::Features::AuditCommon::ShowAuditTableContextMenu(state->hwnd, state->statusTable, pt);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().windowBrush());
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            if (state) {
                if (state->refreshTask) state->refreshTask->cancel();
                if (state->controlTask) state->controlTask->cancel();
            }
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kHwidDispatchClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateHardwareHwidDispatchView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterHwidDispatchClass()) {
        return nullptr;
    }
    auto* state = new HwidDispatchViewState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kHwidDispatchClass,
        L"HWID Dispatch",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Hardware
