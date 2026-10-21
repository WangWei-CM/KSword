#include "LogSurface.h"
#include "../DebuggerBackend/KswordDebuggerFileProtocol.h"
#include "../DebuggerBackend/KswordPluginTheme.h"
#include <algorithm>
#include <array>
#include <cwchar>
#include <fstream>
#include <sstream>

namespace ksword::ce_log
{
    namespace
    {
        constexpr int kFirstToggleId = 7101, kApplyId = 7110, kRestoreId = 7111, kLimitId = 7112;
        struct Options { unsigned long mode = 0, shadow = 0, fallback = 1, context = 1, suspend = 1, limit = 32; };
        HWND gParent = nullptr, gLog = nullptr, gStatus = nullptr, gExplanation = nullptr;
        HWND gApply = nullptr, gRestore = nullptr, gLimit = nullptr, gLimitLabel = nullptr;
        std::array<HWND, 6> gToggles{};
        HFONT gFont = nullptr;
        HBRUSH gWindowBrush = nullptr, gSurfaceBrush = nullptr;
        COLORREF gWindow = RGB(16, 24, 34), gSurface = RGB(20, 30, 42);
        COLORREF gText = RGB(220, 225, 232), gBorder = RGB(69, 83, 101), gAccent = RGB(51, 143, 231);
        bool gEnglish = false, gUseHvm = false, gDirty = false, gAvailable = false, gCanChange = false, gPending = false;
        ULONGLONG gRequestAt = 0;
        unsigned long gRevision = 0, gAwaitingRevision = 0;
        Options gActual{}, gDraft{};
        std::string gLastAckPacket;
        std::wstring gDesiredStatus;
        bool gStatusPending = false, gDraftProjectionPending = false, gApplyingDraft = false;
        bool gHaveProjectedChecks = false;
        std::array<bool, 6> gProjectedChecks{};
        std::wstring gControlPath, gStatePath;
        const wchar_t* text(const wchar_t* zh, const wchar_t* en) { return gEnglish ? en : zh; }
        COLORREF color(const wchar_t* name, COLORREF fallback)
        {
            wchar_t value[16]{};
            if (GetEnvironmentVariableW(name, value, _countof(value)) != 7 || value[0] != L'#') return fallback;
            wchar_t* end = nullptr;
            const unsigned long rgb = std::wcstoul(value + 1, &end, 16);
            if (end == nullptr || *end != 0 || rgb > 0xFFFFFFUL) return fallback;
            return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
        }
        bool checked(std::size_t index)
        {
            switch (index)
            {
            case 0: return gUseHvm;
            case 1: return gDraft.shadow != 0 || gDraft.mode != 0;
            case 2: return gDraft.fallback != 0;
            case 3: return gDraft.mode != 0;
            case 4: return gDraft.context != 0;
            default: return gDraft.suspend != 0;
            }
        }
        const wchar_t* label(std::size_t index)
        {
            switch (index)
            {
            case 0: return text(L"使用 HVM 后端", L"Use HVM backend");
            case 1: return text(L"影子执行页代码修改（需 HVM）", L"Shadow execution patches (HVM)");
            case 2: return text(L"允许显式回退（含数据写入）", L"Allow explicit/data fallback");
            case 3: return text(L"隐蔽断点模式", L"Stealth breakpoint mode");
            case 4: return text(L"Windows 上下文回退", L"Windows context fallback");
            default: return text(L"Windows 挂起/恢复回退", L"Windows suspend fallback");
            }
        }
        void drawButton(const DRAWITEMSTRUCT& item)
        {
            FillRect(item.hDC, &item.rcItem, gWindowBrush);
            SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, (item.itemState & ODS_DISABLED) != 0 ? gBorder : gText);
            HGDIOBJ previous = SelectObject(item.hDC, gFont);
            RECT caption = item.rcItem;
            const int index = static_cast<int>(item.CtlID) - kFirstToggleId;
            if (index >= 0 && index < static_cast<int>(gToggles.size()))
            {
                const int left = item.rcItem.left + 4, top = item.rcItem.top + 7;
                RECT box{left, top, left + 16, top + 16};
                HBRUSH border = CreateSolidBrush(gBorder); FrameRect(item.hDC, &box, border); DeleteObject(border);
                if (checked(static_cast<std::size_t>(index)))
                {
                    HPEN pen = CreatePen(PS_SOLID, 2, gAccent); HGDIOBJ prior = SelectObject(item.hDC, pen);
                    MoveToEx(item.hDC, left + 3, top + 8, nullptr);
                    LineTo(item.hDC, left + 7, top + 12); LineTo(item.hDC, left + 13, top + 4);
                    SelectObject(item.hDC, prior); DeleteObject(pen);
                }
                caption.left += 28;
                DrawTextW(item.hDC, label(static_cast<std::size_t>(index)), -1, &caption, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            else
            {
                HBRUSH border = CreateSolidBrush(gBorder); FrameRect(item.hDC, &caption, border); DeleteObject(border);
                DrawTextW(item.hDC, item.CtlID == kApplyId ? text(L"应用选项", L"Apply options") : text(L"恢复 Shadow 写入", L"Restore Shadow writes"),
                    -1, &caption, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            if ((item.itemState & ODS_FOCUS) != 0) DrawFocusRect(item.hDC, &caption);
            SelectObject(item.hDC, previous);
        }
        void setEnabled(HWND child, bool enabled)
        {
            if (child != nullptr && (IsWindowEnabled(child) != FALSE) != enabled)
                EnableWindow(child, enabled ? TRUE : FALSE);
        }
        void enableControls()
        {
            // State/log polling continues while hidden. Only the latest visual
            // projection is applied before the surface is shown/painted again.
            if (gParent == nullptr || !IsWindowVisible(gParent)) return;
            if (gDraftProjectionPending && gLimit != nullptr)
            {
                const auto limit = std::to_wstring(gDraft.limit);
                wchar_t current[16]{}; GetWindowTextW(gLimit, current, _countof(current));
                gApplyingDraft = true;
                if (limit != current) SetWindowTextW(gLimit, limit.c_str());
                gApplyingDraft = false;
                gDraftProjectionPending = false;
            }
            if (gStatusPending && gStatus != nullptr)
            {
                SetWindowTextW(gStatus, gDesiredStatus.c_str());
                gStatusPending = false;
            }
            setEnabled(gToggles[0], gAvailable && !gPending);
            for (std::size_t index = 1; index < gToggles.size(); ++index)
                setEnabled(gToggles[index], gAvailable && gCanChange && !gPending && (index != 1 || gDraft.mode == 0));
            setEnabled(gLimit, gAvailable && gCanChange && !gPending);
            setEnabled(gApply, gAvailable && gCanChange && !gPending && gDirty);
            setEnabled(gRestore, gAvailable && !gPending);
            for (std::size_t index = 0; index < gToggles.size(); ++index)
            {
                const bool value = checked(index);
                if (!gHaveProjectedChecks || gProjectedChecks[index] != value)
                    InvalidateRect(gToggles[index], nullptr, TRUE);
                gProjectedChecks[index] = value;
            }
            gHaveProjectedChecks = true;
        }
        void setStatus(const std::wstring& value)
        {
            if (gDesiredStatus != value)
            {
                gDesiredStatus = value;
                gStatusPending = true;
            }
            enableControls();
        }
        void request(unsigned long action)
        {
            if (gControlPath.empty() || gPending) return;
            gLastAckPacket.clear();
            wchar_t limit[16]{}; GetWindowTextW(gLimit, limit, _countof(limit));
            wchar_t* end = nullptr; const unsigned long count = std::wcstoul(limit, &end, 10);
            if (action == 1 && (end == limit || *end != 0 || count < 1 || count > 32))
            { setStatus(text(L"Shadow 页上限必须为 1–32", L"Shadow page limit must be 1–32")); return; }
            if (action == 1) gDraft.limit = count;
            const Options& options = action == 1 ? gDraft : gActual;
            std::ofstream output(gControlPath + L".new", std::ios::binary | std::ios::trunc);
            output << "CE2 " << ++gRevision << ' ' << action << ' ' << (action == 0 ? !gUseHvm : gUseHvm)
                << ' ' << options.mode << ' ' << options.shadow << ' ' << options.fallback << ' ' << options.context << ' ' << options.suspend << ' ' << options.limit << '\n';
            output.close();
            if (!output || !MoveFileExW((gControlPath + L".new").c_str(), gControlPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { setStatus(text(L"后端请求写入失败", L"Cannot write backend request")); return; }
            gPending = true; gAwaitingRevision = gRevision; gRequestAt = GetTickCount64(); enableControls();
            setStatus(text(L"等待后端确认；HVM 仍显示已确认状态…", L"Waiting for backend acknowledgement; HVM still shows its confirmed state..."));
        }
        void updateDraft()
        {
            gDraft = gActual; gDirty = false;
            gDraftProjectionPending = true;
            enableControls();
        }
    }
    HWND create(HWND parent)
    {
        gParent = parent;
        gHaveProjectedChecks = false;
        gLastAckPacket.clear();
        wchar_t language[32]{}; (void)GetEnvironmentVariableW(L"KSWORD_PLUGIN_LANGUAGE", language, _countof(language));
        gEnglish = std::wcsncmp(language, L"en", 2) == 0;
        gWindow = color(L"KSWORD_PLUGIN_COLOR_WINDOW", gWindow); gSurface = color(L"KSWORD_PLUGIN_COLOR_SURFACE", gSurface);
        gText = color(L"KSWORD_PLUGIN_COLOR_TEXT_PRIMARY", gText); gBorder = color(L"KSWORD_PLUGIN_COLOR_BORDER", gBorder); gAccent = color(L"KSWORD_PLUGIN_COLOR_ACCENT", gAccent);
        gWindowBrush = CreateSolidBrush(gWindow); gSurfaceBrush = CreateSolidBrush(gSurface);
        HDC dc = GetDC(parent); const int dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(parent, dc);
        gFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        const HINSTANCE module = GetModuleHandleW(nullptr);
        for (std::size_t index = 0; index < gToggles.size(); ++index)
            gToggles[index] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFirstToggleId + index)), module, nullptr);
        gApply = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyId)), module, nullptr);
        gRestore = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRestoreId)), module, nullptr);
        gLimitLabel = CreateWindowExW(0, L"STATIC", text(L"Shadow 页上限", L"Shadow page limit"), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 1, 1, parent, nullptr, module, nullptr);
        gLimit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"32", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 0, 0, 1, 1,
            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLimitId)), module, nullptr);
        SendMessageW(gLimit, EM_SETLIMITTEXT, 2, 0);
        gExplanation = CreateWindowExW(0, L"STATIC", text(
            L"影子执行页只改变代码执行；RX/RWX 页上的数字冻结不会改变数据读取。\n隐蔽模式强制 Shadow；每次回退均写日志，不隐藏 Windows 调试端口。",
            L"Shadow patches execution views only. Numeric freeze on RX/RWX pages does not change data reads.\nStealth forces Shadow. Every fallback is logged; the Windows debug port stays visible."),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 1, 1, parent, nullptr, module, nullptr);
        gDesiredStatus = text(L"等待 CE 调试后端…", L"Waiting for the CE debugger backend...");
        gStatusPending = false;
        gStatus = CreateWindowExW(0, L"STATIC", gDesiredStatus.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 1, 1, parent, nullptr, module, nullptr);
        gLog = CreateWindowExW(0, L"EDIT", text(L"CE 保持独立窗口；此页接收后端日志。\r\n", L"CE runs in its own window; this page receives backend logs.\r\n"),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 1, 1, parent, nullptr, module, nullptr);
        if (gFont == nullptr || gLog == nullptr || gStatus == nullptr || gExplanation == nullptr || gApply == nullptr || gRestore == nullptr || gLimit == nullptr || gLimitLabel == nullptr) return nullptr;
        for (HWND child : gToggles) { if (child == nullptr) return nullptr; SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE); }
        for (HWND child : {gLog, gStatus, gExplanation, gApply, gRestore, gLimit, gLimitLabel}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
        enableControls(); resize(parent); return gLog;
    }
    void resize(HWND parent)
    {
        RECT client{}; GetClientRect(parent, &client);
        const int width = (std::max)(1L, client.right), height = (std::max)(1L, client.bottom);
        const int columns = width >= 540 ? 2 : 1, columnWidth = (std::max)(1, (width - 16) / columns);
        for (std::size_t index = 0; index < gToggles.size(); ++index)
            MoveWindow(gToggles[index], 8 + static_cast<int>(index % columns) * columnWidth, 4 + static_cast<int>(index / columns) * 30, columnWidth, 30, TRUE);
        const int bottom = 4 + (6 / columns) * 30, extraRow = width < 540 ? 32 : 0;
        MoveWindow(gLimitLabel, 8, bottom + 6, 145, 24, TRUE); MoveWindow(gLimit, 154, bottom + 2, 38, 27, TRUE);
        MoveWindow(gApply, extraRow != 0 ? 8 : 200, bottom + 2 + extraRow, 108, 28, TRUE);
        MoveWindow(gRestore, extraRow != 0 ? 124 : 316, bottom + 2 + extraRow, (std::max)(1, width - (extraRow != 0 ? 132 : 324)), 28, TRUE);
        MoveWindow(gExplanation, 8, bottom + 36 + extraRow, (std::max)(1, width - 16), 68, TRUE);
        MoveWindow(gStatus, 8, bottom + 106 + extraRow, (std::max)(1, width - 16), 72, TRUE);
        const int logTop = bottom + 182 + extraRow; MoveWindow(gLog, 0, logTop, width, (std::max)(1, height - logTop), TRUE);
    }
    bool message(HWND parent, UINT messageId, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        // 只替换日志页颜色，不改日志、光标、后端选项或确认状态。
        if (messageId == WM_COPYDATA)
        {
            ksword::plugin_theme::Packet packet;
            result = FALSE;
            if (!ksword::plugin_theme::DecodeCopyData(parent, wParam, lParam, packet))
            {
                return true;
            }
            // 两个画刷都创建成功后才提交新快照，失败时保留旧色与有效画刷。
            HBRUSH windowBrush = CreateSolidBrush(packet.window);
            HBRUSH surfaceBrush = CreateSolidBrush(packet.surface);
            if (windowBrush == nullptr || surfaceBrush == nullptr)
            {
                if (windowBrush != nullptr) DeleteObject(windowBrush);
                if (surfaceBrush != nullptr) DeleteObject(surfaceBrush);
                return true;
            }
            DeleteObject(gWindowBrush);
            DeleteObject(gSurfaceBrush);
            gWindowBrush = windowBrush;
            gSurfaceBrush = surfaceBrush;
            gWindow = packet.window;
            gSurface = packet.surface;
            gText = packet.text;
            gBorder = packet.border;
            gAccent = packet.accent;
            // 包含已有编辑框、静态标签与owner-draw按钮，存量日志内容保持。
            RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            result = TRUE;
            return true;
        }
        if (messageId == WM_SHOWWINDOW || messageId == WM_PAINT || messageId == WM_DRAWITEM)
            enableControls();
        if (messageId == WM_COMMAND)
        {
            const int id = LOWORD(wParam);
            if (id >= kFirstToggleId && id <= kLimitId && !gApplyingDraft) gLastAckPacket.clear();
            if (HIWORD(wParam) == BN_CLICKED && id >= kFirstToggleId && id < kFirstToggleId + 6)
            {
                switch (id - kFirstToggleId)
                {
                case 0: request(0); result = 0; return true;
                case 1: gDraft.shadow ^= 1; break; case 2: gDraft.fallback ^= 1; break;
                case 3: gDraft.mode ^= 1; break;
                case 4: gDraft.context ^= 1; break; default: gDraft.suspend ^= 1; break;
                }
                gDirty = true; enableControls();
                setStatus(text(L"选项待应用；后端仍使用已确认设置", L"Options pending Apply; backend still uses its acknowledged settings"));
                result = 0; return true;
            }
            if (HIWORD(wParam) == BN_CLICKED && (id == kApplyId || id == kRestoreId)) { request(id == kApplyId ? 1UL : 2UL); result = 0; return true; }
            if (id == kLimitId && HIWORD(wParam) == EN_CHANGE)
            { if (!gApplyingDraft) { gDirty = true; enableControls(); } result = 0; return true; }
        }
        if (messageId == WM_DRAWITEM) { drawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); result = TRUE; return true; }
        if (messageId == WM_CTLCOLORSTATIC || messageId == WM_CTLCOLOREDIT)
        {
            const HDC dc = reinterpret_cast<HDC>(wParam); const HWND child = reinterpret_cast<HWND>(lParam);
            const bool editor = child == gLog || child == gLimit; SetTextColor(dc, gText); SetBkColor(dc, editor ? gSurface : gWindow);
            result = reinterpret_cast<LRESULT>(editor ? gSurfaceBrush : gWindowBrush); return true;
        }
        if (messageId == WM_ERASEBKGND)
        { RECT client{}; GetClientRect(parent, &client); FillRect(reinterpret_cast<HDC>(wParam), &client, gWindowBrush); result = TRUE; return true; }
        return false;
    }
    void setSession(const std::wstring& logPath)
    {
        gLastAckPacket.clear();
        gControlPath = logPath.empty() ? L"" : logPath + L".control"; gStatePath = logPath.empty() ? L"" : logPath + L".state";
        gRevision = 0; gAwaitingRevision = 0; gPending = false; gAvailable = false; gCanChange = false; gDirty = false; enableControls();
        if (logPath.empty()) setStatus(text(L"CE 已退出", L"CE has exited"));
    }
    void pollState()
    {
        if (gStatePath.empty()) return;
        if (gPending && GetTickCount64() - gRequestAt >= 5000)
        {
            gPending = false; enableControls();
            setStatus(text(L"后端确认超时；实际状态未变，可重试", L"Backend acknowledgement timed out; actual state unchanged; retry is available"));
        }
        std::string packet;
        if (!ksword::debugger::readControlPacket(gStatePath, packet)) { enableControls(); return; }
        // Compare the complete validated packet, not revision alone: counters and
        // capability can change without a new policy request/revision.
        if (!gPending && !gLastAckPacket.empty() && packet == gLastAckPacket)
        { enableControls(); return; }
        std::istringstream state(packet);
        unsigned long revision = 0, error = 0, selected = 0, window = 0, resident = 0, protocol = 0;
        if (!(state >> revision >> error >> selected >> window >> resident >> protocol) || revision < gRevision) return;
        std::string marker; unsigned long driver = 0, valid = 0, path = 0, pages = 0, breakpoints = 0, canChange = 0;
        unsigned long fallbackCount = 0, fallbackError = 0, persistenceError = 0; Options actual{};
        if (!(state >> marker >> driver >> valid >> actual.mode >> actual.shadow >> actual.fallback >> actual.context >> actual.suspend >> actual.limit
            >> path >> pages >> breakpoints >> canChange >> fallbackCount >> fallbackError >> persistenceError))
        {
            gLastAckPacket.clear();
            gAvailable = false; gPending = false; enableControls();
            setStatus(text(L"CE 选项后端不可用，查看日志", L"CE options backend unavailable; see log")); return;
        }
        if (marker != "CE2" || valid != 1 ||
            selected > 1 || driver > 1 || actual.mode > 1 || actual.shadow > 1 || actual.fallback > 1 || actual.context > 1 ||
            actual.suspend > 1 || actual.limit < 1 || actual.limit > 32 || canChange > 1 || path > 2) return;
        const bool acknowledged = gAwaitingRevision != 0 && revision >= gAwaitingRevision;
        if (acknowledged) gAwaitingRevision = 0;
        gRevision = revision; gUseHvm = selected != 0; gActual = actual; gAvailable = true; gCanChange = canChange != 0; gPending = false;
        if (acknowledged || !gDirty) updateDraft();
        std::wstring status;
        if (error != 0) status = std::wstring(text(L"请求失败，错误 ", L"Request failed, error ")) + std::to_wstring(error) + L" | ";
        status += driver == 0 ? text(L"CE 原生路由（驱动不可用）", L"CE native route (driver unavailable)") : (gUseHvm ? text(L"HVM 已确认", L"HVM acknowledged") : text(L"R0 已确认", L"R0 acknowledged"));
        status += text(L" | 断点路由 ", L" | breakpoint route ");
        status += path == 2 ? L"Shadow" : (path == 1 ? L"EPT" : text(L"原生", L"native"));
        status += std::wstring(text(L" | Shadow 写入页 ", L" | Shadow write pages ")) + std::to_wstring(pages) + text(L" | 回退 ", L" | fallbacks ") + std::to_wstring(fallbackCount);
        if (fallbackError != 0) status += std::wstring(text(L" | 最近回退原因 ", L" | last fallback reason ")) + std::to_wstring(fallbackError);
        if (gUseHvm && window == 0) status += text(L" | 私有窗口不可用", L" | private window unavailable");
        if (gUseHvm && resident == 0) status += text(L" | 尚未常驻", L" | not resident yet");
        if (!gCanChange) status += text(L" | 先移除断点/恢复 Shadow 写入再改选项", L" | clear breakpoints/restore Shadow writes before changing options");
        if (persistenceError != 0) status += text(L" | 保存设置失败，查看日志", L" | settings save failed; see log");
        if (gDirty) status += text(L" | 有待应用选项", L" | options pending Apply");
        setStatus(status);
        gLastAckPacket = packet;
    }
    void destroy()
    {
        if (gFont != nullptr) DeleteObject(gFont); if (gWindowBrush != nullptr) DeleteObject(gWindowBrush); if (gSurfaceBrush != nullptr) DeleteObject(gSurfaceBrush);
        gFont = nullptr; gWindowBrush = nullptr; gSurfaceBrush = nullptr; gToggles.fill(nullptr); gLog = nullptr; gStatus = nullptr;
        gParent = nullptr; gLastAckPacket.clear(); gDesiredStatus.clear();
        gStatusPending = false; gDraftProjectionPending = false; gHaveProjectedChecks = false;
        gExplanation = nullptr; gApply = nullptr; gRestore = nullptr; gLimit = nullptr; gLimitLabel = nullptr;
    }
}
