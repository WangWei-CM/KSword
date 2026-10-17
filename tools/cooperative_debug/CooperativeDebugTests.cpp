#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../shared/debug/CooperativeDebugClient.h"
#include "CooperativeDebugRuntime.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <thread>
#include <windows.h>

using ksword::cooperative::Result;
using ksword::cooperative::Session;
using ksword::cooperative::State;

namespace
{
    // 测试映射只记录自有目标的进度与断言，不承载协议令牌。
    struct Metrics
    {
        // ready/quit 为同步标记；progress 证明真实业务恢复。
        volatile LONG ready = 0;
        volatile LONG quit = 0;
        volatile LONG progress = 0;
        volatile LONG unknownHandled = 0;
        // owner 字段记录突然死亡测试；debuggerPresent 记录原生 API 结果。
        volatile LONG ownerPaused = 0;
        volatile LONG ownerProgress = 0;
        // debuggerPresent：原生调试器存在状态。
        volatile LONG debuggerPresent = 0;
        // Stop 请求及完成标记不依赖控制管道继续存活。
        volatile LONG runtimeStopRequested = 0;
        volatile LONG runtimeStopped = 0;
        // pid：目标进程标识。
        DWORD pid = 0;
        // creation：进程创建时间身份。
        std::uint64_t creation = 0;
    };
    unsigned assertions = 0;

    void Require(bool value, const char *message)
    {
        if (!value)
            throw std::runtime_error(message);
        ++assertions;
        std::printf("PASS %s\n", message);
    }

    // 只查询本测试创建的目标，同时验证 DebugPort 与 DebugObject 身份。
    void NoDebugObject(HANDLE target)
    {
        using Query = LONG(NTAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
        const FARPROC address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
        // query：原生调试状态查询函数。
        Query query = nullptr;
        static_assert(sizeof(query) == sizeof(address));
        std::memcpy(&query, &address, sizeof(query));
        Require(query != nullptr, "native debug-object query available");
        // port：目标DebugPort查询输出。
        ULONG_PTR port = ~static_cast<ULONG_PTR>(0);
        // object：目标DebugObject查询输出。
        HANDLE object = nullptr;
        // portStatus：DebugPort查询返回状态。
        const LONG portStatus = query(target, 7, &port, sizeof(port), nullptr);
        // objectStatus：DebugObject查询返回状态。
        const LONG objectStatus = query(target, 0x1E, &object, sizeof(object), nullptr);
        std::printf("DEBUG_STATE PortStatus=%08lx Port=%llu ObjectStatus=%08lx ObjectNull=%u\n",
                    static_cast<unsigned long>(portStatus), static_cast<unsigned long long>(port),
                    static_cast<unsigned long>(objectStatus), object == nullptr ? 1U : 0U);
        Require(portStatus == 0 && port == 0, "ProcessDebugPort is zero");
        Require(static_cast<ULONG>(objectStatus) == 0xC0000353UL && object == nullptr,
                "ProcessDebugObjectHandle reports STATUS_PORT_NOT_SET and null");
    }

    // 子进程只启动当前测试程序，保存真实句柄以约束清理对象。
    HANDLE Launch(const std::wstring &arguments)
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
            return nullptr;
        // command：已消费停点对应的运行命令。
        std::wstring command = L"\"" + std::wstring(path) + L"\" " + arguments;
        // startup：自有子进程启动参数。
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // process：刚创建的自有子进程信息。
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &process))
            return nullptr;
        CloseHandle(process.hThread);
        return process.hProcess;
    }

    // 独立自有线程调用 Stop，验证业务线程正暂停时也能被安全释放。
    DWORD WINAPI StopMonitor(void *argument)
    {
        // metrics：仅自有测试使用的共享状态。
        auto *metrics = static_cast<Metrics *>(argument);
        while (InterlockedCompareExchange(&metrics->quit, 0, 0) == 0)
        {
            if (InterlockedCompareExchange(&metrics->runtimeStopRequested, 0, 0) != 0)
            {
                ksword::cooperative::runtime::Stop();
                InterlockedExchange(&metrics->runtimeStopped, 1);
                return 0;
            }
            Sleep(5);
        }
        return 0;
    }

    int Target(const wchar_t *name)
    {
        // mapping：仅自有测试使用的共享映射。
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!mapping)
            return 10;
        // metrics：仅自有测试使用的共享状态。
        auto *metrics =
            static_cast<Metrics *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Metrics)));
        if (!metrics)
        {
            CloseHandle(mapping);
            return 11;
        }
        ksword::cooperative::runtime::Config config;
        config.leaseMs = 1200;
        // error：本次操作系统错误。
        const DWORD error = ksword::cooperative::runtime::Start(config);
        if (error)
            return static_cast<int>(error);
        // stopMonitor：执行Stop回归的自有线程。
        HANDLE stopMonitor = CreateThread(nullptr, 0, StopMonitor, metrics, 0, nullptr);
        if (!stopMonitor)
        {
            ksword::cooperative::runtime::Stop();
            return 12;
        }
        // 未知异常应由目标自身 SEH 处理，组件 VEH 不能将其吞掉。
        __try
        {
            RaiseException(0xE0421234, 0, 0, nullptr);
        }
        __except (GetExceptionCode() == 0xE0421234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            InterlockedIncrement(&metrics->unknownHandled);
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
        metrics->pid = GetCurrentProcessId();
        metrics->creation =
            (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        // present：原生调试器查询输出。
        BOOL present = TRUE;
        CheckRemoteDebuggerPresent(GetCurrentProcess(), &present);
        metrics->debuggerPresent = present || IsDebuggerPresent();
        InterlockedExchange(&metrics->ready, 1);
        while (InterlockedCompareExchange(&metrics->quit, 0, 0) == 0)
        {
            ksword::cooperative::runtime::Checkpoint();
            InterlockedIncrement(&metrics->progress);
            Sleep(5);
        }
        WaitForSingleObject(stopMonitor, 5000);
        CloseHandle(stopMonitor);
        ksword::cooperative::runtime::Stop();
        UnmapViewOfFile(metrics);
        CloseHandle(mapping);
        return 0;
    }

    Result OpenEventually(Session &session, DWORD pid, std::uint64_t creation)
    {
        // result：本次返回结果。
        Result result;
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            result = session.open(pid, creation);
            if (result.ok())
                return result;
            Sleep(10);
        }
        return result;
    }

    Result Paused(Session &session)
    {
        // result：本次返回结果。
        Result result;
        for (unsigned attempt = 0; attempt < 200; ++attempt)
        {
            result = session.query();
            if (!result.ok() || result.state == State::Paused)
                return result;
            Sleep(5);
        }
        return result;
    }

    bool Progresses(Metrics *metrics, LONG before, DWORD timeout = 3000)
    {
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < timeout)
        {
            if (InterlockedCompareExchange(&metrics->progress, 0, 0) > before)
                return true;
            Sleep(5);
        }
        return false;
    }

    int Owner(const wchar_t *name, bool afterStep)
    {
        // mapping：仅自有测试使用的共享映射。
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!mapping)
            return 20;
        // metrics：仅自有测试使用的共享状态。
        auto *metrics =
            static_cast<Metrics *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Metrics)));
        if (!metrics)
            return 21;
        Session session;
        if (!OpenEventually(session, metrics->pid, metrics->creation).ok() || !session.requestPause().ok())
            return 22;
        // stopped：当前真实暂停应答。
        const Result stopped = Paused(session);
        if (!stopped.ok() || stopped.state != State::Paused)
            return 23;
        InterlockedExchange(&metrics->ownerProgress, metrics->progress);
        if (afterStep && !session.step(stopped.stopId).ok())
            return 24;
        InterlockedExchange(&metrics->ownerPaused, 1);
        // 故意跳过析构，验证 owner 死亡和已武装 TF 的恢复路径。
        TerminateProcess(GetCurrentProcess(), 0);
        return 25;
    }

    void RejectMalformed(DWORD pid, std::uint64_t creation)
    {
        // pipe：当前独占管道句柄。
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt < 100 && pipe == INVALID_HANDLE_VALUE; ++attempt)
        {
            pipe = CreateFileW(ksword::cooperative::EndpointName(pid, creation).c_str(),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
                Sleep(10);
        }
        Require(pipe != INVALID_HANDLE_VALUE, "raw protocol connects only to opt-in endpoint");
        ksword::cooperative::Request request = ksword::cooperative::MakeRequest();
        request.version += 1;
        request.targetPid = pid;
        request.targetCreationTime100ns = creation;
        request.requestId = 1;
        ksword::cooperative::Response response = ksword::cooperative::MakeResponse();
        // written：实际写入字节数。
        DWORD written = 0;
        // read：实际读出字节数。
        DWORD read = 0;
        Require(WriteFile(pipe, &request, sizeof(request), &written, nullptr) != FALSE,
                "malformed protocol packet sent");
        Require(ReadFile(pipe, &response, sizeof(response), &read, nullptr) != FALSE &&
                    read == sizeof(response) && response.errorWin32 == ERROR_INVALID_DATA,
                "protocol version mismatch rejected");
        CloseHandle(pipe);
    }

    // 同一端点上的错误令牌也不能取得现场或新令牌，随后连接会被撤销。
    void RejectWrongToken(DWORD pid, std::uint64_t creation)
    {
        // pipe：当前独占管道句柄。
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt < 100 && pipe == INVALID_HANDLE_VALUE; ++attempt)
        {
            pipe = CreateFileW(ksword::cooperative::EndpointName(pid, creation).c_str(),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
                Sleep(10);
        }
        Require(pipe != INVALID_HANDLE_VALUE, "token test opened exact target endpoint");
        // request：本次固定长度协议请求。
        auto request = ksword::cooperative::MakeRequest();
        request.operation = ksword::cooperative::Operation::Open;
        request.targetPid = pid;
        request.targetCreationTime100ns = creation;
        request.ownerPid = GetCurrentProcessId();
        // created：创建时间输出。
        FILETIME created{};
        // exited：退出时间输出。
        FILETIME exited{};
        // kernel：内核用时输出。
        FILETIME kernel{};
        // user：用户用时或用户SID缓冲。
        FILETIME user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        request.ownerCreationTime100ns =
            (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        request.requestId = 1;
        // response：本次固定长度协议应答。
        auto response = ksword::cooperative::MakeResponse();
        // bytes：本次实际传输字节数。
        DWORD bytes = 0;
        Require(WriteFile(pipe, &request, sizeof(request), &bytes, nullptr) &&
                    ReadFile(pipe, &response, sizeof(response), &bytes, nullptr) && response.errorWin32 == 0,
                "raw owner identity authenticated");
        request.operation = ksword::cooperative::Operation::Query;
        request.requestId = 2;
        request.tokenLow = response.tokenLow ^ 1;
        request.tokenHigh = response.tokenHigh;
        Require(WriteFile(pipe, &request, sizeof(request), &bytes, nullptr) &&
                    ReadFile(pipe, &response, sizeof(response), &bytes, nullptr) &&
                    response.errorWin32 == ERROR_ACCESS_DENIED && response.tokenLow == 0 &&
                    response.tokenHigh == 0 && response.stopId == 0 && response.snapshot.rip == 0,
                "wrong token rejected without leaking live token or registers");
        CloseHandle(pipe);
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--target") == 0)
        return Target(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--owner") == 0)
        return Owner(argv[2], false);
    if (argc == 3 && wcscmp(argv[1], L"--owner-step") == 0)
        return Owner(argv[2], true);
    const std::wstring name =
        L"Local\\KSword.CooperativeDebug.Test." + std::to_wstring(GetCurrentProcessId());
    // mapping：仅自有测试使用的共享映射。
    HANDLE mapping =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Metrics), name.c_str());
    // metrics：仅自有测试使用的共享状态。
    auto *metrics =
        mapping ? static_cast<Metrics *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Metrics)))
                : nullptr;
    // target：保留目标身份的进程句柄。
    HANDLE target = nullptr;
    try
    {
        Require(metrics != nullptr, "self-owned test state created");
        target = Launch(L"--target " + name);
        Require(target != nullptr, "self-owned cooperative target launched");
        for (unsigned attempt = 0; attempt < 200 && metrics->ready == 0; ++attempt)
            Sleep(10);
        Require(metrics->ready != 0 && metrics->pid != 0 && metrics->creation != 0,
                "target opted in and published exact process identity");
        Require(metrics->unknownHandled == 1, "unrelated exception remained with target SEH");
        // present：原生调试器查询输出。
        BOOL present = TRUE;
        Require(CheckRemoteDebuggerPresent(target, &present) && !present && metrics->debuggerPresent == 0,
                "target has no Windows debug object before stepping");
        NoDebugObject(target);
        Session session;
        Require(!session.open(metrics->pid, metrics->creation + 1).ok(),
                "reused or mismatched process creation time rejected");
        Require(OpenEventually(session, metrics->pid, metrics->creation).ok(),
                "authenticated cooperative session opened");
        Require(session.query().state == State::Running, "new session starts running");
        Require(session.requestPause().ok(), "pause requested for next target checkpoint");
        // stopped：当前真实暂停应答。
        Result stopped = Paused(session);
        std::printf("PAUSE_DIAGNOSTIC state=%u error=%u stop=%llu rip=%llx progress=%ld detail=%s\n",
                    static_cast<unsigned>(stopped.state), stopped.errorWin32, stopped.stopId,
                    stopped.snapshot.rip, metrics->progress, stopped.detail.c_str());
        Require(stopped.ok() && stopped.state == State::Paused && stopped.stopId != 0 &&
                    stopped.snapshot.rip != 0,
                "real target checkpoint paused with current registers");
        for (unsigned index = 0; index < 2; ++index)
        {
            // previous：刚消费的旧停点。
            const auto previous = stopped;
            Require(session.step(previous.stopId).ok(), "current stop consumed by real TF step");
            Require(!session.resume(previous.stopId).ok(), "stale stopId cannot continue newer execution");
            stopped = Paused(session);
            Require(stopped.ok() && stopped.state == State::Paused && stopped.stopId > previous.stopId,
                    "single-step produced a new current stop");
            std::printf("STEP_RIP=%llx->%llx\n", previous.snapshot.rip, stopped.snapshot.rip);
            Require(stopped.snapshot.rip != previous.snapshot.rip, "real instruction pointer advanced");
        }
        Require(CheckRemoteDebuggerPresent(target, &present) && !present,
                "two steps did not create a Windows debug object");
        NoDebugObject(target);
        // beforeResume：继续前真实业务进度。
        const LONG beforeResume = metrics->progress;
        Require(session.resume(stopped.stopId).ok() && Progresses(metrics, beforeResume),
                "continue resumed real target work");
        Require(session.requestPause().ok() && Paused(session).state == State::Paused,
                "second checkpoint pause available");
        // beforeClose：关闭前真实业务进度。
        const LONG beforeClose = metrics->progress;
        // 并发查询取消验证 UI 关闭不等待 2 秒管道应答或租约。
        std::thread reader(
            [&session]()
            {
                for (unsigned attempt = 0; attempt < 1000; ++attempt)
                    if (!session.query().ok())
                        break;
            });
        // closeStarted：本地关闭起始时刻。
        const ULONGLONG closeStarted = GetTickCount64();
        session.close();
        // closeElapsed：本地关闭耗时。
        const ULONGLONG closeElapsed = GetTickCount64() - closeStarted;
        reader.join();
        Require(closeElapsed < 100, "close returns promptly during controller polling");
        Require(Progresses(metrics, beforeClose), "closing controller releases paused target");
        RejectMalformed(metrics->pid, metrics->creation);
        RejectWrongToken(metrics->pid, metrics->creation);
        Require(OpenEventually(session, metrics->pid, metrics->creation).ok() &&
                    session.requestPause().ok() && Paused(session).state == State::Paused,
                "fresh session works after malformed protocol");
        // beforeLease：租约到期前业务进度。
        const LONG beforeLease = metrics->progress;
        Sleep(1600);
        Require(Progresses(metrics, beforeLease),
                "lease expiry releases target without further controller commands");
        Require(!session.query().ok(), "expired transport cannot reuse previous stop or token");
        session.close();
        for (const wchar_t *mode : {L"--owner ", L"--owner-step "})
        {
            InterlockedExchange(&metrics->ownerPaused, 0);
            // owner：认证控制者进程句柄。
            HANDLE owner = Launch(std::wstring(mode) + name);
            Require(owner != nullptr && WaitForSingleObject(owner, 5000) == WAIT_OBJECT_0,
                    "abrupt controller child exited");
            // exit：自有子进程退出码。
            DWORD exit = 1;
            GetExitCodeProcess(owner, &exit);
            CloseHandle(owner);
            Require(exit == 0 && metrics->ownerPaused == 1,
                    "abrupt owner reached a real pause or armed step");
            Require(Progresses(metrics, metrics->ownerProgress),
                    "owner death releases pause and clears owned TF");
        }
        Require(OpenEventually(session, metrics->pid, metrics->creation).ok() &&
                    session.requestPause().ok() && Paused(session).state == State::Paused,
                "runtime Stop test reached a real paused target");
        // beforeStop：Stop前业务进度。
        const LONG beforeStop = metrics->progress;
        InterlockedExchange(&metrics->runtimeStopRequested, 1);
        Require(Progresses(metrics, beforeStop), "runtime Stop releases paused business thread");
        for (unsigned attempt = 0; attempt < 200 && metrics->runtimeStopped == 0; ++attempt)
            Sleep(5);
        Require(metrics->runtimeStopped == 1, "runtime Stop removed worker and drained handler");
        Require(!session.query().ok(), "stopped runtime no longer accepts a controller");
        session.close();
        InterlockedExchange(&metrics->quit, 1);
        Require(WaitForSingleObject(target, 5000) == WAIT_OBJECT_0,
                "runtime Stop cancelled worker and target exited normally");
        // exit：自有子进程退出码。
        DWORD exit = 1;
        GetExitCodeProcess(target, &exit);
        Require(exit == 0, "target exited without unhandled debug exception");
        std::printf("COOPERATIVE_TEST_RESULT=PASS ASSERTIONS=%u\n", assertions);
    }
    catch (const std::exception &error)
    {
        std::printf("COOPERATIVE_TEST_RESULT=FAIL DETAIL=%s WIN32=%lu\n", error.what(), GetLastError());
        if (metrics)
            InterlockedExchange(&metrics->quit, 1);
        if (target && WaitForSingleObject(target, 2000) == WAIT_TIMEOUT)
            TerminateProcess(target, 99);
        if (target)
            CloseHandle(target);
        if (metrics)
            UnmapViewOfFile(metrics);
        if (mapping)
            CloseHandle(mapping);
        return 1;
    }
    if (target)
        CloseHandle(target);
    UnmapViewOfFile(metrics);
    CloseHandle(mapping);
    return 0;
}
