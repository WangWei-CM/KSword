#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>
#include <iterator>

namespace ks::page_navigation
{
    // R3 页面导航协议：UTF-8 JSON，只有目录读取和已登记页面跳转，不执行命令或业务动作。
    inline constexpr ULONG_PTR RequestTag = 0x4B53504E;
    inline constexpr ULONG_PTR ReplyTag = 0x4B535052;
    inline constexpr DWORD MaxMessageBytes = 256 * 1024;
    inline constexpr wchar_t ServerProperty[] = L"KswordARK.PageNavigation.v1";
    inline constexpr wchar_t ClientProperty[] = L"KswordARK.PageOverview.v1";
    inline constexpr wchar_t TaskbarFileName[] = L"Taskbar.exe";
    inline constexpr wchar_t MainFileName[] = L"Ksword5.1.exe";

    // 只接收同一发行目录的配套进程窗口；查询权限失败时返回 false。
    inline bool IsCompanionWindow(HWND window, const wchar_t* fileName)
    {
        DWORD processId = 0; // 来源窗口所属进程，不接受调用方传入的 PID。
        if (!window || !::IsWindow(window) || !::GetWindowThreadProcessId(window, &processId))
        {
            return false;
        }
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
        {
            return false;
        }
        wchar_t peerPath[32768] = {};
        DWORD peerLength = static_cast<DWORD>(std::size(peerPath));
        const BOOL queried = ::QueryFullProcessImageNameW(process, 0, peerPath, &peerLength);
        ::CloseHandle(process);
        wchar_t ownPath[32768] = {};
        const DWORD ownLength = ::GetModuleFileNameW(nullptr, ownPath, static_cast<DWORD>(std::size(ownPath)));
        if (!queried || ownLength == 0 || ownLength >= std::size(ownPath))
        {
            return false;
        }
        std::wstring expected(ownPath, ownLength); // 配套程序必须在当前 exe 的同级目录。
        const auto separator = expected.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            return false;
        }
        expected.resize(separator + 1);
        expected += fileName;
        return _wcsicmp(expected.c_str(), peerPath) == 0;
    }
}
