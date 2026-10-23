// KSword-owned Win32 surface, adapted from CheatEngineExecutablePlugin/LogSurface.cpp.
// The debugger stays a top-level window. Only this control/log surface is embedded.
#include "LogSurface.h"
#include "../DebuggerBackend/KswordDebuggerFileProtocol.h"
#include "../DebuggerBackend/KswordPluginTheme.h"
#include "../TitanEnginePlugin/ControlProtocol.h"
#include <algorithm>
#include <array>
#include <cwchar>
#include <fstream>

namespace ksword::x96_log
{
    namespace
    {
        namespace wire = titan::control;
        constexpr int kToggleId = 4101, kModeId = 4102, kShadowId = 4103, kFallbackId = 4104;
        constexpr int kContextId = 4105, kSuspendId = 4106, kPagesId = 4107;
        HWND gToggle = nullptr, gMode = nullptr, gShadow = nullptr, gFallback = nullptr;
        HWND gContext = nullptr, gSuspend = nullptr, gPages = nullptr, gPagesLabel = nullptr;
        HWND gParent = nullptr, gStatus = nullptr, gLog = nullptr;
        HFONT gFont = nullptr;
        HBRUSH gWindowBrush = nullptr, gSurfaceBrush = nullptr;
        COLORREF gWindow = RGB(16, 24, 34), gSurface = RGB(20, 30, 42);
        COLORREF gText = RGB(220, 225, 232), gBorder = RGB(69, 83, 101), gAccent = RGB(51, 143, 231);
        bool gEnglish = false, gUseHvm = false, gPending = false, gAvailable = false, gCanChange = false;
        std::uint64_t gRevision = 0;
        ULONGLONG gRequestedAt = 0;
        KSWORD_DEBUGGER_OPTIONS gOptions = wire::defaults();
        std::uint32_t gNormalShadowWrites = 0;
        std::wstring gControlPath, gStatePath, gPreferencesPath;
        std::string gSessionId, gLastAckPacket;
        std::wstring gDesiredStatus;
        bool gStatusPending = false, gHaveProjectedChecks = false;
        std::array<bool, 5> gProjectedChecks{};

        const wchar_t* text(const wchar_t* zh, const wchar_t* en) { return gEnglish ? en : zh; }
        COLORREF color(const wchar_t* name, COLORREF fallback)
        {
            wchar_t value[16]{};
            if (GetEnvironmentVariableW(name, value, _countof(value)) != 7 || value[0] != L'#') return fallback;
            wchar_t* end = nullptr; const unsigned long rgb = std::wcstoul(value + 1, &end, 16);
            if (end == nullptr || *end != 0 || rgb > 0xFFFFFFUL) return fallback;
            return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
        }
        bool checked(int id);
        void refresh()
        {
            // Keep consuming state while hidden; visual work waits until the
            // surface is shown/painted, and uses the newest confirmed values.
            if (gParent == nullptr || !IsWindowVisible(gParent)) return;
            const bool enabled = gAvailable && gCanChange && !gPending;
            for (const HWND child : {gToggle, gMode, gShadow, gFallback, gContext, gSuspend, gPages})
            {
                const bool childEnabled = enabled && !(child == gShadow && gOptions.mode == KSWORD_DEBUGGER_MODE_STEALTH);
                if ((IsWindowEnabled(child) != FALSE) != childEnabled)
                    EnableWindow(child, childEnabled ? TRUE : FALSE);
            }
            const auto mode = static_cast<LRESULT>(gOptions.mode);
            const auto pages = static_cast<LRESULT>(gOptions.maxShadowPages - 1);
            if (SendMessageW(gMode, CB_GETCURSEL, 0, 0) != mode)
                SendMessageW(gMode, CB_SETCURSEL, static_cast<WPARAM>(mode), 0);
            if (SendMessageW(gPages, CB_GETCURSEL, 0, 0) != pages)
                SendMessageW(gPages, CB_SETCURSEL, static_cast<WPARAM>(pages), 0);
            const std::array<HWND, 5> toggles{gToggle, gShadow, gFallback, gContext, gSuspend};
            const std::array<int, 5> ids{kToggleId, kShadowId, kFallbackId, kContextId, kSuspendId};
            for (std::size_t index = 0; index < toggles.size(); ++index)
            {
                const bool value = checked(ids[index]);
                if (!gHaveProjectedChecks || gProjectedChecks[index] != value)
                    InvalidateRect(toggles[index], nullptr, TRUE);
                gProjectedChecks[index] = value;
            }
            gHaveProjectedChecks = true;
            if (gStatusPending && gStatus != nullptr)
            {
                SetWindowTextW(gStatus, gDesiredStatus.c_str());
                gStatusPending = false;
            }
        }
        const wchar_t* label(int id)
        {
            switch (id)
            {
            case kToggleId: return text(L"使用 KSword HVM", L"Use KSword HVM");
            case kShadowId: return text(L"内存写入使用 ShadowPage", L"ShadowPage memory writes");
            case kFallbackId: return text(L"允许明确记录的 fallback", L"Allow logged fallback");
            case kContextId: return text(L"允许原生上下文 fallback", L"Allow native context fallback");
            default: return text(L"允许原生暂停 fallback", L"Allow native suspend fallback");
            }
        }
        bool checked(int id)
        {
            switch (id)
            {
            case kToggleId: return gUseHvm;
            case kShadowId: return gOptions.shadowMemoryWrites != 0 || gOptions.mode == KSWORD_DEBUGGER_MODE_STEALTH;
            case kFallbackId: return gOptions.allowFallback != 0;
            case kContextId: return gOptions.nativeContextFallback != 0;
            default: return gOptions.nativeSuspendFallback != 0;
            }
        }
        void draw(const DRAWITEMSTRUCT& item)
        {
            const bool combo = item.CtlType == ODT_COMBOBOX;
            FillRect(item.hDC, &item.rcItem, combo ? gSurfaceBrush : gWindowBrush);
            SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, gText);
            HGDIOBJ previousFont = SelectObject(item.hDC, gFont);
            RECT content = item.rcItem; content.left += 4;
            std::wstring value;
            if (combo)
            {
                if (item.itemID != static_cast<UINT>(-1))
                    value = item.CtlID == kModeId ? (item.itemID == 0 ? text(L"常规 HVM", L"Normal HVM") : text(L"隐蔽 HVM", L"Stealth HVM")) : std::to_wstring(item.itemID + 1);
            }
            else
            {
                const int top = item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - 16) / 2;
                RECT box{content.left, top, content.left + 16, top + 16};
                HBRUSH border = CreateSolidBrush(gBorder); FrameRect(item.hDC, &box, border); DeleteObject(border);
                if (checked(item.CtlID))
                {
                    HPEN pen = CreatePen(PS_SOLID, 2, gAccent); HGDIOBJ previous = SelectObject(item.hDC, pen);
                    MoveToEx(item.hDC, box.left + 3, top + 8, nullptr);
                    LineTo(item.hDC, box.left + 7, top + 12); LineTo(item.hDC, box.left + 13, top + 4);
                    SelectObject(item.hDC, previous); DeleteObject(pen);
                }
                content.left += 24; value = label(item.CtlID);
            }
            DrawTextW(item.hDC, value.c_str(), -1, &content, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            if ((item.itemState & ODS_FOCUS) != 0) DrawFocusRect(item.hDC, &content);
            SelectObject(item.hDC, previousFont);
        }
        void request(bool selected, const KSWORD_DEBUGGER_OPTIONS& options)
        {
            gLastAckPacket.clear();
            if (gControlPath.empty() || gPending || !gAvailable || !gCanChange) { refresh(); return; }
            if (gRevision == UINT64_MAX) { setStatus(text(L"请求序号已耗尽，请重新打开页面。", L"Request sequence exhausted; reopen the Tab.")); refresh(); return; }
            const auto revision = gRevision + 1;
            std::ofstream stream(gControlPath + L".new", std::ios::binary | std::ios::trunc);
            stream << wire::request(gSessionId, revision, selected ? 1 : 0, options); stream.close();
            if (!stream || !MoveFileExW((gControlPath + L".new").c_str(), gControlPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { setStatus(text(L"后端策略请求写入失败", L"Cannot write backend policy request")); refresh(); return; }
            gRevision = revision; gPending = true; gRequestedAt = GetTickCount64(); refresh();
            setStatus(text(L"等待调试器确认实际策略…", L"Waiting for the debugger to confirm the actual policy..."));
        }
        void savePreferences()
        {
            if (gPreferencesPath.empty()) return;
            std::ofstream stream(gPreferencesPath + L".new", std::ios::binary | std::ios::trunc);
            stream << wire::preferences(gOptions); stream.close();
            if (!stream || !MoveFileExW((gPreferencesPath + L".new").c_str(), gPreferencesPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                append(text(L"实际策略已生效，但保存配置失败。", L"Actual policy applied, but saving preferences failed."));
        }
        void timeout()
        {
            if (!gPending || GetTickCount64() - gRequestedAt < 5000) return;
            gPending = false; refresh();
            setStatus(text(L"后端未确认请求；显示值仍是最后实际状态。", L"Backend did not acknowledge; values remain the last confirmed state."));
        }
    }
    HWND create(HWND parent)
    {
        gParent = parent;
        gLastAckPacket.clear();
        gHaveProjectedChecks = false;
        wchar_t language[32]{}; (void)GetEnvironmentVariableW(L"KSWORD_PLUGIN_LANGUAGE", language, _countof(language));
        gEnglish = std::wcsncmp(language, L"en", 2) == 0;
        gWindow = color(L"KSWORD_PLUGIN_COLOR_WINDOW", gWindow); gSurface = color(L"KSWORD_PLUGIN_COLOR_SURFACE", gSurface);
        gText = color(L"KSWORD_PLUGIN_COLOR_TEXT_PRIMARY", gText); gBorder = color(L"KSWORD_PLUGIN_COLOR_BORDER", gBorder); gAccent = color(L"KSWORD_PLUGIN_COLOR_ACCENT", gAccent);
        gWindowBrush = CreateSolidBrush(gWindow); gSurfaceBrush = CreateSolidBrush(gSurface);
        HDC dc = GetDC(parent); const int dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(parent, dc);
        gFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        const auto button = [parent](int id) { return CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr); };
        const auto combo = [parent](int id) { return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
            0, 0, 1, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr); };
        gToggle = button(kToggleId); gShadow = button(kShadowId); gFallback = button(kFallbackId);
        gContext = button(kContextId); gSuspend = button(kSuspendId); gMode = combo(kModeId); gPages = combo(kPagesId);
        SendMessageW(gMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal")); SendMessageW(gMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Stealth"));
        for (int i = 1; i <= 32; ++i) { const auto value = std::to_wstring(i); SendMessageW(gPages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str())); }
        gPagesLabel = CreateWindowExW(0, L"STATIC", text(L"Shadow 页上限（1–32）", L"Shadow page limit (1–32)"), WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        gDesiredStatus = text(L"等待 TitanEngine 后端…", L"Waiting for the TitanEngine backend...");
        gStatusPending = false;
        gStatus = CreateWindowExW(0, L"STATIC", gDesiredStatus.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        gLog = CreateWindowExW(0, L"EDIT", text(L"x64dbg 保持独立窗口。隐蔽模式优先隐藏执行视图，不代表完全隐身。fallback 始终记录。\r\n", L"x64dbg runs independently. Stealth mode prefers a hidden execution view; it does not guarantee invisibility. Fallback is always logged.\r\n"),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (gFont == nullptr || gWindowBrush == nullptr || gSurfaceBrush == nullptr) return nullptr;
        for (const HWND child : {gToggle, gMode, gShadow, gFallback, gContext, gSuspend, gPages, gPagesLabel, gStatus, gLog})
        { if (child == nullptr) return nullptr; (void)SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE); }
        for (const HWND child : {gMode, gPages}) { SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 22); SendMessageW(child, CB_SETITEMHEIGHT, 0, 22); }
        refresh(); resize(parent); return gLog;
    }
    void resize(HWND parent)
    {
        RECT client{}; GetClientRect(parent, &client);
        const int width = (std::max)(1L, client.right), height = (std::max)(1L, client.bottom);
        const int half = (std::max)(1, (width - 24) / 2);
        MoveWindow(gToggle, 8, 4, half, 30, TRUE); MoveWindow(gMode, half + 16, 6, half, 150, TRUE);
        MoveWindow(gShadow, 8, 38, half, 30, TRUE); MoveWindow(gFallback, half + 16, 38, half, 30, TRUE);
        MoveWindow(gContext, 8, 70, half, 30, TRUE); MoveWindow(gSuspend, half + 16, 70, half, 30, TRUE);
        MoveWindow(gPagesLabel, 12, 109, half, 24, TRUE); MoveWindow(gPages, half + 16, 105, half, 220, TRUE);
        MoveWindow(gStatus, 8, 140, (std::max)(1, width - 16), 48, TRUE);
        MoveWindow(gLog, 0, 192, width, (std::max)(1, height - 192), TRUE);
    }
    bool message(HWND parent, UINT messageId, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        // 宿主热更新只影响自有日志页，不修改独立调试器或任何后端配置。
        if (messageId == WM_COPYDATA)
        {
            ksword::plugin_theme::Packet packet;
            result = FALSE;
            if (!ksword::plugin_theme::DecodeCopyData(parent, wParam, lParam, packet))
            {
                return true;
            }
            // 成对创建后交换画刷，避免资源失败留下半套主题。
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
            // 子控件下次绘制读取新颜色，既有选择、日志和确认状态不变。
            RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            result = TRUE;
            return true;
        }
        if (messageId == WM_SHOWWINDOW || messageId == WM_PAINT || messageId == WM_DRAWITEM)
            refresh();
        const int id = LOWORD(wParam);
        if (messageId == WM_COMMAND && id >= kToggleId && id <= kPagesId)
        {
            gLastAckPacket.clear();
            auto options = gOptions; bool selected = gUseHvm;
            if (HIWORD(wParam) == BN_CLICKED)
            {
                if (id == kToggleId) selected = !selected;
                else if (id == kShadowId) options.shadowMemoryWrites ^= 1;
                else if (id == kFallbackId) options.allowFallback ^= 1;
                else if (id == kContextId) options.nativeContextFallback ^= 1;
                else if (id == kSuspendId) options.nativeSuspendFallback ^= 1;
                else { result = 0; return true; }
            }
            else if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                const auto index = SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0);
                if (index == CB_ERR) { result = 0; return true; }
                if (id == kModeId)
                {
                    options.mode = static_cast<std::uint32_t>(index);
                    if (options.mode == KSWORD_DEBUGGER_MODE_STEALTH) { options.shadowMemoryWrites = 1; options.allowFallback = 0; }
                    else options.shadowMemoryWrites = gNormalShadowWrites;
                }
                else if (id == kPagesId) options.maxShadowPages = static_cast<std::uint32_t>(index + 1);
                else { result = 0; return true; }
            }
            else { result = 0; return true; }
            request(selected, options); result = 0; return true;
        }
        if (messageId == WM_MEASUREITEM && wParam >= kModeId && wParam <= kPagesId && lParam != 0)
        { reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 22; result = TRUE; return true; }
        if (messageId == WM_DRAWITEM && wParam >= kToggleId && wParam <= kPagesId && lParam != 0)
        { draw(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); result = TRUE; return true; }
        if (messageId == WM_CTLCOLORSTATIC || messageId == WM_CTLCOLOREDIT || messageId == WM_CTLCOLORLISTBOX)
        {
            const HDC dc = reinterpret_cast<HDC>(wParam); const bool editor = reinterpret_cast<HWND>(lParam) == gLog || messageId == WM_CTLCOLORLISTBOX;
            SetTextColor(dc, gText); SetBkColor(dc, editor ? gSurface : gWindow);
            result = reinterpret_cast<LRESULT>(editor ? gSurfaceBrush : gWindowBrush); return true;
        }
        if (messageId == WM_ERASEBKGND)
        { RECT client{}; GetClientRect(parent, &client); FillRect(reinterpret_cast<HDC>(wParam), &client, gWindowBrush); result = TRUE; return true; }
        return false;
    }
    void setSession(const std::wstring& logPath, const std::string& sessionId, const std::wstring& preferencesPath)
    {
        gLastAckPacket.clear();
        gControlPath = logPath.empty() ? L"" : logPath + L".control"; gStatePath = logPath.empty() ? L"" : logPath + L".state";
        gPreferencesPath = preferencesPath; gSessionId = sessionId;
        // The launcher writes revision 1 before starting the debugger. HVM remains
        // opt-in per session; only a real ACK can make saved preferences visible.
        gRevision = logPath.empty() ? 0 : 1; gPending = !logPath.empty(); gRequestedAt = GetTickCount64();
        gUseHvm = false; gAvailable = false; gCanChange = false; gOptions = wire::defaults(); gNormalShadowWrites = 0; refresh();
        setStatus(logPath.empty() ? text(L"调试器已退出", L"Debugger has exited") : text(L"等待实际策略确认…", L"Waiting for actual policy confirmation..."));
    }
    void pollState()
    {
        if (gStatePath.empty()) return;
        std::string packet; wire::Ack ack;
        if (!debugger::readControlPacket(gStatePath, packet)) { timeout(); refresh(); return; }
        // Same revision can publish different counts/capability. Only an exact
        // repeat of the last successfully validated/consumed packet is skipped.
        if (!gPending && !gLastAckPacket.empty() && packet == gLastAckPacket)
        { refresh(); return; }
        if (!wire::parseAck(packet, ack) || ack.session != gSessionId || ack.revision < gRevision)
        { timeout(); refresh(); return; }
        const bool acknowledged = gPending;
        gRevision = ack.revision; gPending = false; gUseHvm = ack.selected != 0;
        gAvailable = ack.extended; gCanChange = ack.extended && ack.policy.canChangeOptions != 0;
        const bool enteredStealth = ack.extended && ack.policy.options.mode == KSWORD_DEBUGGER_MODE_STEALTH &&
            (!gAvailable || gOptions.mode != KSWORD_DEBUGGER_MODE_STEALTH);
        if (ack.extended)
        {
            gOptions = ack.policy.options;
            if (gOptions.mode == KSWORD_DEBUGGER_MODE_NORMAL) gNormalShadowWrites = gOptions.shadowMemoryWrites;
        }
        if (enteredStealth) append(text(L"实际策略：隐蔽模式强制使用 Shadow 执行视图；Windows 调试事件通道仍存在。", L"Effective policy: stealth forces the Shadow execution view; Windows debug-event transport remains active."));
        if (acknowledged && ack.error == ERROR_SUCCESS && ack.extended) savePreferences();
        std::wstring status;
        if (!ack.extended) status = text(L"后端协议不支持策略选项，请更新 TitanEngine 插件。", L"Backend lacks policy options; update the TitanEngine plugin.");
        else if (ack.error == ERROR_BUSY)
        {
            status = text(L"策略未更改（170）；仍使用实际已确认设置。请先暂停调试器", L"Policy unchanged (170); confirmed actual settings retained. Pause the debugger");
            if (ack.policy.activeBreakpoints != 0)
                status += std::wstring(text(L"，清除活动断点 ", L", clear active breakpoints: ")) + std::to_wstring(ack.policy.activeBreakpoints);
            if (ack.policy.shadowWritePages != 0)
                status += std::wstring(text(L"，恢复 Shadow 写入页 ", L", restore Shadow write pages: ")) + std::to_wstring(ack.policy.shadowWritePages);
            if (ack.policy.activeBreakpoints == 0 && ack.policy.shadowWritePages == 0 && !gCanChange)
                status += text(L"，并等待当前调试/断点操作完成", L", and wait for the current debug/breakpoint operation to complete");
            status += text(L"后重试。", L", then retry.");
            if (acknowledged) append(status);
        }
        else if (ack.error != 0) status = std::wstring(text(L"策略请求失败，错误 ", L"Policy request failed, error ")) + std::to_wstring(ack.error);
        else status = gUseHvm ? text(L"HVM 已选择", L"HVM selected") : text(L"原生 TitanEngine", L"Native TitanEngine");
        status += ack.driver ? text(L" | R0 已连接", L" | R0 connected") : text(L" | R0 未连接", L" | R0 unavailable");
        if (ack.extended)
        {
            status += L" | "; status += ack.policy.activePath == KSWORD_DEBUGGER_PATH_SHADOW ? L"ShadowPage" : ack.policy.activePath == KSWORD_DEBUGGER_PATH_EPT ? L"EPT" : text(L"原生路径", L"native path");
            if (gOptions.mode == KSWORD_DEBUGGER_MODE_STEALTH) status += text(L" | 隐蔽策略强制 Shadow", L" | stealth forces Shadow");
            status += std::wstring(text(L"\r\n断点 ", L"\r\nBreakpoints ")) + std::to_wstring(ack.policy.activeBreakpoints) + text(L" | Shadow 页 ", L" | Shadow pages ") + std::to_wstring(ack.policy.shadowWritePages);
            status += std::wstring(L" | fallback ") + std::to_wstring(ack.policy.fallbackCount) + L" (" + std::to_wstring(ack.policy.lastFallbackError) + L")";
            if (!gCanChange) status += text(L" | 暂停并清除断点/Shadow 写入后可修改", L" | Pause and clear breakpoints/Shadow writes to change");
        }
        setStatus(status); refresh();
        gLastAckPacket = packet;
    }
    void setStatus(const std::wstring& value)
    {
        if (gDesiredStatus != value)
        {
            gDesiredStatus = value;
            gStatusPending = true;
        }
        refresh();
    }
    void append(const std::wstring& value)
    {
        if (!IsWindow(gLog)) return;
        if (GetWindowTextLengthW(gLog) > 262144) SetWindowTextW(gLog, L"");
        const int length = GetWindowTextLengthW(gLog); SendMessageW(gLog, EM_SETSEL, length, length);
        const std::wstring line = value + L"\r\n"; SendMessageW(gLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    }
    void destroy()
    {
        if (gFont != nullptr) DeleteObject(gFont); if (gWindowBrush != nullptr) DeleteObject(gWindowBrush); if (gSurfaceBrush != nullptr) DeleteObject(gSurfaceBrush);
        gFont = nullptr; gWindowBrush = nullptr; gSurfaceBrush = nullptr;
        gToggle = gMode = gShadow = gFallback = gContext = gSuspend = gPages = gPagesLabel = gStatus = gLog = nullptr;
        gParent = nullptr; gLastAckPacket.clear(); gDesiredStatus.clear();
        gStatusPending = false; gHaveProjectedChecks = false;
    }
}
