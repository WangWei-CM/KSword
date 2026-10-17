#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "CooperativeDebugRuntime.h"
#include <atomic>
#include <cstdio>
#include <windows.h>

namespace
{
    // 控制台停止只置退出标记；组件在业务循环结束后安全撤销。
    std::atomic<bool> quit{false};
    BOOL WINAPI Control(DWORD)
    {
        quit.store(true);
        return TRUE;
    }
}

int main()
{
    // error：本次操作系统错误。
    const DWORD error = ksword::cooperative::runtime::Start();
    if (error)
    {
        std::printf("START_ERROR=%lu\n", error);
        return 1;
    }
    // created 为进程身份；其余时间是 Windows API 要求的输出参数。
    FILETIME created{};
    // exited：退出时间输出。
    FILETIME exited{};
    // kernel：内核用时输出。
    FILETIME kernel{};
    // user：用户用时或用户SID缓冲。
    FILETIME user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const unsigned long long creation =
        (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    std::printf("COOPERATIVE_TARGET_PID=%lu CREATION_TIME_100NS=%llu\n", GetCurrentProcessId(), creation);
    std::printf("Target opted in. Pause requests take effect at the next Checkpoint. Ctrl+C exits.\n");
    std::fflush(stdout);
    SetConsoleCtrlHandler(Control, TRUE);
    unsigned long long counter = 0;
    while (!quit.load())
    {
        ksword::cooperative::runtime::Checkpoint();
        ++counter;
        Sleep(10);
    }
    ksword::cooperative::runtime::Stop();
    std::printf("COUNTER=%llu\n", counter);
    return 0;
}
