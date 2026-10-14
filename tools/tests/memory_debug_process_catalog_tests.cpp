#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "../../Ksword5.1/Ksword5.1/MemoryDock/MemoryDebugProcessCatalog.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int checks = 0; // 本轮实际执行的断言数。
    int failures = 0; // 本轮未通过的断言数。

    // Check：报告单条结果并累计失败；入参为条件与简短检查名。
    void Check(const bool condition, const char* name)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
        }
        std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    }

    // CreationTime：直接读取持有句柄的创建时间，与被测目录函数独立核对。
    std::uint64_t CreationTime(const HANDLE process)
    {
        FILETIME created{}; // 独立创建时间输出。
        FILETIME exited{}; // API 必须提供的退出时间输出。
        FILETIME kernel{}; // API 必须提供的内核时间输出。
        FILETIME user{}; // API 必须提供的用户时间输出。
        if (!::GetProcessTimes(process, &created, &exited, &kernel, &user))
        {
            return 0;
        }
        return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32)
            | created.dwLowDateTime;
    }
}

// wmain：仅测试自身与本测试创建的子进程；不会读取、写入、暂停或附加外部目标。
int wmain(const int argc, wchar_t* argv[])
{
    if (argc == 3 && std::wstring(argv[1]) == L"--child")
    {
        const HANDLE finish = ::OpenEventW(SYNCHRONIZE, FALSE, argv[2]); // 父进程允许正常退出的事件。
        if (finish == nullptr)
        {
            return 90;
        }
        const DWORD wait = ::WaitForSingleObject(finish, 60000); // 自行退出上限，不会遗留无限后台进程。
        ::CloseHandle(finish);
        return wait == WAIT_OBJECT_0 ? 0 : 91;
    }

    const DWORD selfPid = ::GetCurrentProcessId(); // 主测试进程身份。
    const std::uint64_t selfCreation = CreationTime(::GetCurrentProcess()); // 持有自身伪句柄核对时间。
    Check(selfCreation != 0, "self creation time available");
    Check(ks::ui::CanWriteMemoryDebugProcess(selfPid, selfCreation), "self same identity reports writable");
    Check(!ks::ui::CanWriteMemoryDebugProcess(selfPid, selfCreation + 1), "self wrong creation time is denied");
    Check(!ks::ui::CanWriteMemoryDebugProcess(selfPid, 0), "zero identity is denied");
    Check(!ks::ui::CanWriteMemoryDebugProcess(0, selfCreation), "zero PID is denied");
    Check(!ks::ui::CanWriteMemoryDebugProcess(0xffffffffu, selfCreation), "invalid PID is denied");

    const std::wstring eventName = L"Local\\KSwordMemoryCatalogRegression_" + std::to_wstring(selfPid); // 本进程唯一事件名。
    const HANDLE finish = ::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()); // 子进程自然退出信号。
    Check(finish != nullptr, "child completion event created");
    if (finish == nullptr)
    {
        return 1;
    }
    wchar_t executable[MAX_PATH]{}; // 本测试程序的完整路径。
    const DWORD pathLength = ::GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Check(pathLength > 0 && pathLength < MAX_PATH, "own executable path available");
    const std::wstring executablePath(executable, pathLength); // 独立核对同对象映像的实际文件名。
    const auto separator = executablePath.find_last_of(L"\\/"); // 对 Windows 与正斜杠路径均取末段。
    const std::wstring expectedName = executablePath.substr(separator == std::wstring::npos ? 0 : separator + 1);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + eventName + L"\""; // 只启动本测试二进制。
    STARTUPINFOW startup{}; // 隐藏的非交互子进程启动结构。
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{}; // 只拥有本测试启动的子进程和线程句柄。
    const BOOL launched = ::CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    Check(launched != FALSE, "own child launched");
    if (!launched)
    {
        ::CloseHandle(finish);
        return 1;
    }

    const std::uint64_t childCreation = CreationTime(child.hProcess); // 持有句柄获得独立强身份。
    Check(childCreation != 0, "child creation time available");
    Check(ks::ui::CanWriteMemoryDebugProcess(child.dwProcessId, childCreation), "child same identity reports writable");
    Check(!ks::ui::CanWriteMemoryDebugProcess(child.dwProcessId, childCreation + 1), "child stale identity is denied");
    const auto catalog = ks::ui::EnumerateMemoryDebugProcesses(); // 运行真实生产目录枚举。
    Check(catalog.error == 0, "catalog enumeration succeeds");
    const auto ownRow = std::find_if(catalog.processes.begin(), catalog.processes.end(), [selfPid](const auto& row)
    {
        return row.pid == selfPid;
    });
    Check(ownRow != catalog.processes.end() && ownRow->createTime100ns == selfCreation
        && ownRow->name == expectedName, "catalog self exact executable name and strong identity match held handle");
    const auto childRow = std::find_if(catalog.processes.begin(), catalog.processes.end(), [&child](const auto& row)
    {
        return row.pid == child.dwProcessId;
    });
    Check(childRow != catalog.processes.end() && childRow->createTime100ns == childCreation
        && childRow->name == expectedName, "catalog child exact executable name and strong identity match held handle");
    Check(std::none_of(catalog.processes.begin(), catalog.processes.end(), [](const auto& row)
    {
        return row.pid == 0;
    }), "catalog excludes zero PID");

    ::SetEvent(finish); // 子进程收到事件后自行退出，不调用 TerminateProcess。
    Check(::WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, "own child exits normally");
    DWORD exitCode = 0; // 确认测试子进程正常结束，未超时退出。
    Check(::GetExitCodeProcess(child.hProcess, &exitCode) && exitCode == 0, "child completion exit code is zero");
    ::CloseHandle(child.hThread);
    ::CloseHandle(child.hProcess);
    ::CloseHandle(finish);
    std::cout << "CATALOG_CHECKS=" << checks << " FAILURES=" << failures
        << " ENUMERATED=" << catalog.processes.size() << '\n';
    return failures == 0 ? 0 : 1;
}
