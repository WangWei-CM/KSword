#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "../../Ksword5.1/Ksword5.1/MemoryDock/MemoryDebugThreadSession.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#if defined(MEMORY_DEBUG_THREAD_SESSION_FAULT_TESTS)
// 故障构建只替换下方直接包含的生产实现入口；测试和自有子进程仍调用真实 Win32。
volatile LONG failNextContext = 0; // 令下次生产 GetThreadContext 返回访问拒绝。
volatile LONG failNextResume = 0; // 令下次生产 ResumeThread 返回访问拒绝。
volatile LONG failNextWaitKind = 0; // 1 为进程等待失败，2 为线程等待失败。

// MemoryDebugTestGetThreadContext：除一次显式故障外直接转发 Windows，不伪造寄存器值。
BOOL WINAPI MemoryDebugTestGetThreadContext(HANDLE thread, LPCONTEXT context)
{
    if (::InterlockedExchange(&failNextContext, 0) != 0)
    {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::GetThreadContext(thread, context);
}

// MemoryDebugTestResumeThread：故障时不减少真实计数，便于独立验证重试配平。
DWORD WINAPI MemoryDebugTestResumeThread(HANDLE thread)
{
    if (::InterlockedExchange(&failNextResume, 0) != 0)
    {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return static_cast<DWORD>(-1);
    }
    return ::ResumeThread(thread);
}

// MemoryDebugTestWaitForSingleObject：按对象种类单次失败，模拟失效句柄等待错误。
DWORD WINAPI MemoryDebugTestWaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
    const LONG kind = ::InterlockedCompareExchange(&failNextWaitKind, 0, 0); // 只在故障配置时辨认句柄类型。
    const bool matches = (kind == 1 && ::GetProcessId(handle) != 0)
        || (kind == 2 && ::GetThreadId(handle) != 0); // 不改变正常路径的 LastError。
    if (matches && ::InterlockedCompareExchange(&failNextWaitKind, 0, kind) == kind)
    {
        ::SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    return ::WaitForSingleObject(handle, milliseconds);
}
#endif

namespace
{
    using ks::ui::MemoryDebugThreadError; // 生产错误分类，供断言核对真实失败原因。
    using ks::ui::MemoryDebugThreadIdentity; // 生产强线程身份。
    using ks::ui::MemoryDebugThreadSession; // 被测的无附加线程会话。
    using ks::ui::MemoryDebugThreadState; // 生产状态分类。
    int checks = 0; // 本轮已经执行的断言数量。
    int failures = 0; // 本轮失败的断言数量。

    // SharedFixture：仅本测试父子进程共享的进度，不读取任何用户进程内存。
    struct SharedFixture
    {
        volatile LONG64 workerTicks = 0; // 被选线程循环进度。
        volatile LONG64 mainTicks = 0; // 其它线程循环进度。
        volatile LONG exitProcess = 0; // 请求子进程自行 ExitProcess，覆盖暂停中的目标退出。
        DWORD workerTid = 0; // 仅本测试创建的工作线程编号。
        HANDLE stop = nullptr; // 子进程本地有效的正常退出事件。
    };

    // Check：报告本条证据并累计失败，入参是实际条件和简短断言名。
    void Check(const bool condition, const char* name)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
        }
        std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    }

    // Creation：独立读取真实持有对象的创建时间，避免测试只重复被测函数。
    std::uint64_t Creation(const HANDLE handle, const bool thread)
    {
        FILETIME created{}; // 对象创建时间输出。
        FILETIME exited{}; // API 要求的退出时间占位。
        FILETIME kernel{}; // API 要求的内核时间占位。
        FILETIME user{}; // API 要求的用户时间占位。
        const BOOL queried = thread
            ? ::GetThreadTimes(handle, &created, &exited, &kernel, &user)
            : ::GetProcessTimes(handle, &created, &exited, &kernel, &user);
        return queried ? (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
    }

    // ReadTicks：原子取得父子共享进度，避免数据竞争掩盖实际暂停与继续。
    LONG64 ReadTicks(volatile LONG64& value)
    {
        return ::InterlockedCompareExchange64(&value, 0, 0);
    }

    // Worker：本测试的可暂停线程；不持有锁、堆或加载器资源。
    DWORD WINAPI Worker(void* parameter)
    {
        auto* shared = static_cast<SharedFixture*>(parameter); // 当前子进程打开的共享映射。
        while (::WaitForSingleObject(shared->stop, 1) == WAIT_TIMEOUT)
        {
            ::InterlockedIncrement64(&shared->workerTicks);
            if (::InterlockedCompareExchange(&shared->exitProcess, 0, 0) != 0)
            {
                ::ExitProcess(0); // 仅本测试子进程自退，不访问外部目标。
            }
        }
        return 0;
    }

    // Child：只打开父进程给出的测试对象，最多运行 60 秒后自然退出。
    int Child(const wchar_t* mappingName, const wchar_t* stopName, const wchar_t* readyName)
    {
        const HANDLE mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName); // 父测试的共享进度映射。
        const HANDLE stop = ::OpenEventW(SYNCHRONIZE, FALSE, stopName); // 正常结束事件。
        const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName); // 线程创建完成事件。
        if (mapping == nullptr || stop == nullptr || ready == nullptr)
        {
            return 90;
        }
        auto* shared = static_cast<SharedFixture*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedFixture)));
        if (shared == nullptr)
        {
            return 91;
        }
        shared->stop = stop;
        const HANDLE worker = ::CreateThread(nullptr, 0, Worker, shared, 0, &shared->workerTid); // 自有可控工作线程。
        if (worker == nullptr)
        {
            return 92;
        }
        ::SetEvent(ready);
        const ULONGLONG deadline = ::GetTickCount64() + 60000; // 故障时也不留下无限后台任务。
        while (::WaitForSingleObject(stop, 1) == WAIT_TIMEOUT && ::GetTickCount64() < deadline)
        {
            ::InterlockedIncrement64(&shared->mainTicks);
        }
        ::WaitForSingleObject(worker, 1000); // 正常测试先恢复线程，再通知结束。
        ::CloseHandle(worker);
        ::UnmapViewOfFile(shared);
        ::CloseHandle(mapping);
        ::CloseHandle(stop);
        ::CloseHandle(ready);
        return 0;
    }

    // Fixture：只启动当前测试二进制；所有句柄、映射和失败清理均限定自有子进程。
    class Fixture final
    {
    public:
        PROCESS_INFORMATION child{}; // 本对象创建的唯一子进程与初始线程句柄。
        SharedFixture* shared = nullptr; // 本测试父进程映射的进度值。
        HANDLE mapping = nullptr; // 本测试专属共享映射句柄。
        HANDLE stop = nullptr; // 自有子进程正常结束事件。
        HANDLE ready = nullptr; // 自有子进程就绪事件。
        HANDLE worker = nullptr; // 自有子进程工作线程的独立验证句柄。
        MemoryDebugThreadIdentity identity; // 从创建句柄独立构造的工作线程身份。

        // start：生成当前 PID+时间唯一对象名，并以隐藏窗口启动同一测试程序。
        bool start()
        {
            const std::wstring prefix = L"Local\\KSwordThreadSession_" + std::to_wstring(::GetCurrentProcessId())
                + L"_" + std::to_wstring(::GetTickCount64()); // 仅测试父子知道的对象名前缀。
            const std::wstring mappingName = prefix + L"_mapping"; // 共享进度对象名。
            const std::wstring stopName = prefix + L"_stop"; // 正常退出对象名。
            const std::wstring readyName = prefix + L"_ready"; // 就绪事件对象名。
            mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                sizeof(SharedFixture), mappingName.c_str());
            stop = ::CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
            ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
            if (mapping == nullptr || stop == nullptr || ready == nullptr)
            {
                return false;
            }
            shared = static_cast<SharedFixture*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedFixture)));
            if (shared == nullptr)
            {
                return false;
            }
            wchar_t executable[32768]{}; // 当前测试程序完整路径，不启动外部二进制。
            const DWORD length = ::GetModuleFileNameW(nullptr, executable, 32768);
            if (length == 0 || length >= 32768)
            {
                return false;
            }
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --fixture \"" + mappingName
                + L"\" \"" + stopName + L"\" \"" + readyName + L"\""; // 只传测试对象名。
            STARTUPINFOW startup{}; // 隐藏非交互子进程启动结构。
            startup.cb = sizeof(startup);
            if (!::CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &child) || ::WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0)
            {
                return false;
            }
            worker = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME | SYNCHRONIZE,
                FALSE, shared->workerTid); // 只对自己启动的工作线程独立校验计数。
            identity.pid = child.dwProcessId;
            identity.processCreateTime100ns = Creation(child.hProcess, false);
            identity.tid = shared->workerTid;
            identity.threadCreateTime100ns = worker == nullptr ? 0 : Creation(worker, true);
            return worker != nullptr && identity.processCreateTime100ns != 0 && identity.threadCreateTime100ns != 0;
        }

        // 析构：先请求正常结束；只有测试失败仍卡住时才结束本对象创建的那一个子进程。
        ~Fixture()
        {
            if (stop != nullptr)
            {
                ::SetEvent(stop);
            }
            if (child.hProcess != nullptr && ::WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0)
            {
                ::TerminateProcess(child.hProcess, 93);
                ::WaitForSingleObject(child.hProcess, 3000);
            }
            if (worker != nullptr)
            {
                ::CloseHandle(worker);
            }
            if (child.hThread != nullptr)
            {
                ::CloseHandle(child.hThread);
            }
            if (child.hProcess != nullptr)
            {
                ::CloseHandle(child.hProcess);
            }
            // 只有子进程已经结束后才解除共享映射，不给仍在执行的测试线程留下悬空地址。
            if (shared != nullptr)
            {
                ::UnmapViewOfFile(shared);
            }
            if (mapping != nullptr)
            {
                ::CloseHandle(mapping);
            }
            if (stop != nullptr)
            {
                ::CloseHandle(stop);
            }
            if (ready != nullptr)
            {
                ::CloseHandle(ready);
            }
        }
    };
}

// wmain：真实 Win32 回归只控制本测试启动的自有子进程，不附加、不注入、不设置上下文。
int wmain(const int argc, wchar_t* argv[])
{
    if (argc == 5 && std::wstring(argv[1]) == L"--fixture")
    {
        return Child(argv[2], argv[3], argv[4]);
    }
    Fixture fixture; // 声明在会话前，析构顺序保证先恢复会话暂停再结束目标。
    Check(fixture.start(), "own AMD64 child and worker launched");
    if (fixture.identity.threadCreateTime100ns == 0)
    {
        return 1;
    }
    MemoryDebugThreadSession session; // 被测生产会话，每次最多拥有一个线程的一次暂停。
    Check(session.status().status == MemoryDebugThreadState::Closed, "new session is closed");
    Check(session.enumerateThreads().error == MemoryDebugThreadError::NotOpen, "closed session enumeration rejected");
    Check(session.open(0, 0).error == MemoryDebugThreadError::InvalidIdentity, "zero identity rejected");
    Check(session.open(::GetCurrentProcessId(), Creation(::GetCurrentProcess(), false)).error
        == MemoryDebugThreadError::SelfTarget, "controller own process rejected");
    Check(session.open(fixture.identity.pid, fixture.identity.processCreateTime100ns + 1).error
        == MemoryDebugThreadError::ProcessIdentityMismatch, "stale process creation time rejected");
    const auto opened = session.open(fixture.identity.pid, fixture.identity.processCreateTime100ns); // 正确身份打开。
    Check(opened.success && opened.status == MemoryDebugThreadState::Ready && !opened.ownedSuspend, "exact process identity opens without pause");
    Check(session.open(fixture.identity.pid, fixture.identity.processCreateTime100ns).success, "same identity reopen is idempotent");

    const auto threads = session.enumerateThreads(); // 真实目录与独立创建句柄逐项比较。
    Check(threads.success && threads.inaccessibleThreads == 0, "strong thread enumeration succeeds");
    const auto workerRow = std::find_if(threads.threads.begin(), threads.threads.end(), [&fixture](const auto& row)
    {
        return row.identity.tid == fixture.identity.tid;
    });
    Check(workerRow != threads.threads.end() && workerRow->identity.threadCreateTime100ns
        == fixture.identity.threadCreateTime100ns, "worker creation time matches independent handle");
    const auto mainRow = std::find_if(threads.threads.begin(), threads.threads.end(), [&fixture](const auto& row)
    {
        return row.identity.tid == fixture.child.dwThreadId;
    });
    Check(mainRow != threads.threads.end() && mainRow->identity.threadCreateTime100ns
        == Creation(fixture.child.hThread, true), "initial thread creation time matches independent handle");
    auto wrongIdentity = fixture.identity; // 伪造陈旧 TID 身份，验证不会对新对象生效。
    ++wrongIdentity.threadCreateTime100ns;
    Check(session.pause(wrongIdentity).error == MemoryDebugThreadError::ThreadIdentityMismatch, "stale thread creation time rejected");
    Check(session.querySnapshot(wrongIdentity).error == MemoryDebugThreadError::ThreadIdentityMismatch, "snapshot stale thread identity rejected");
    Check(!session.status().ownedSuspend, "identity rejection leaves no suspension");

    BOOL debugged = TRUE; // 独立检查 Windows 调试端口附加状态。
    Check(::CheckRemoteDebuggerPresent(fixture.child.hProcess, &debugged) && !debugged, "child begins without Windows debugger attachment");
    const auto runningSnapshot = session.querySnapshot(fixture.identity); // 临时暂停后必须恢复。
    Check(runningSnapshot.success && runningSnapshot.hasSnapshot && runningSnapshot.snapshot.rip != 0
        && runningSnapshot.snapshot.rsp != 0 && runningSnapshot.snapshot.capturedAt100ns != 0, "running thread real AMD64 context captured");
    Check(runningSnapshot.snapshot.transientPause && !runningSnapshot.ownedSuspend
        && runningSnapshot.status == MemoryDebugThreadState::Ready, "running snapshot balances its temporary pause");
    Check(runningSnapshot.snapshot.identity.threadCreateTime100ns == fixture.identity.threadCreateTime100ns,
        "context snapshot keeps exact thread identity");

    const auto paused = session.pause(fixture.identity); // 显式暂停只影响所选工作线程。
    Check(paused.success && paused.ownedSuspend && paused.status == MemoryDebugThreadState::Paused, "explicit pause owns exactly one selected thread");
    Check(session.pause(fixture.identity).error == MemoryDebugThreadError::AlreadyPaused, "repeated pause rejected without another increment");
    if (mainRow != threads.threads.end())
    {
        Check(session.pause(mainRow->identity).error == MemoryDebugThreadError::DifferentThreadPaused, "second thread pause rejected");
        Check(session.querySnapshot(mainRow->identity).error == MemoryDebugThreadError::DifferentThreadPaused, "second thread snapshot rejected while pause owned");
    }
    const auto pausedSnapshot = session.querySnapshot(fixture.identity); // GetThreadContext 同步本次实际暂停。
    Check(pausedSnapshot.success && pausedSnapshot.hasSnapshot && pausedSnapshot.ownedSuspend
        && !pausedSnapshot.snapshot.transientPause, "paused context preserves explicit pause ownership");
    const LONG64 stoppedTicks = ReadTicks(fixture.shared->workerTicks); // 暂停已同步后取基线。
    const LONG64 otherTicks = ReadTicks(fixture.shared->mainTicks); // 其它线程必须继续运行。
    ::Sleep(100);
    Check(ReadTicks(fixture.shared->workerTicks) == stoppedTicks, "selected worker stops progressing");
    Check(ReadTicks(fixture.shared->mainTicks) > otherTicks, "other target thread continues progressing");
    const auto pausedThreads = session.enumerateThreads(); // 现场目录需准确标明唯一自有暂停。
    Check(std::count_if(pausedThreads.threads.begin(), pausedThreads.threads.end(), [](const auto& row)
        { return row.ownedPause; }) == 1, "enumeration marks exactly one owned pause");
    Check(session.resume().success && !session.status().ownedSuspend, "resume balances explicit pause");
    ::Sleep(50);
    Check(ReadTicks(fixture.shared->workerTicks) > stoppedTicks, "worker resumes real progress");
    Check(session.resume().error == MemoryDebugThreadError::NotPaused, "repeated resume cannot decrement external counts");

    const DWORD externalPrevious = ::SuspendThread(fixture.worker); // 独立所有者为测试建立一次暂停。
    Check(externalPrevious == 0, "independent test owner begins with suspend count zero");
    Check(session.pause(fixture.identity).error == MemoryDebugThreadError::ThreadAlreadySuspended
        && !session.status().ownedSuspend, "externally paused thread is refused and balanced");
    Check(session.querySnapshot(fixture.identity).error == MemoryDebugThreadError::ThreadAlreadySuspended,
        "externally paused snapshot is refused and balanced");
    Check(::ResumeThread(fixture.worker) == 1, "external owner's one count survives backend refusals");
    bool repeatedSnapshots = true; // 连续临时快照的综合返回结果。
    for (int index = 0; index < 20; ++index) // 有界运行，验证不会累计暂停计数。
    {
        const auto snapshot = session.querySnapshot(fixture.identity); // 独立临时暂停和配平。
        repeatedSnapshots = repeatedSnapshots && snapshot.success && !snapshot.ownedSuspend;
    }
    Check(repeatedSnapshots, "twenty transient captures all complete without owned pause");
    Check(::SuspendThread(fixture.worker) == 0 && ::ResumeThread(fixture.worker) == 1,
        "independent count probe confirms no transient suspend leaks");

#if defined(MEMORY_DEBUG_THREAD_SESSION_FAULT_TESTS)
    // 故障只在两三个 Win32 入口单次注入；其余身份、暂停和恢复仍在真实自有线程上执行。
    ::InterlockedExchange(&failNextContext, 1);
    const auto failedCapture = session.querySnapshot(fixture.identity); // 捕获失败也必须恢复临时暂停。
    Check(failedCapture.error == MemoryDebugThreadError::ContextFailed && failedCapture.errorWin32 == ERROR_ACCESS_DENIED
        && !failedCapture.hasSnapshot && !failedCapture.ownedSuspend, "injected capture failure balances temporary pause");
    Check(::SuspendThread(fixture.worker) == 0 && ::ResumeThread(fixture.worker) == 1, "capture failure leaves real suspend count zero");
    Check(session.pause(fixture.identity).success, "pause before injected resume failure");
    ::InterlockedExchange(&failNextResume, 1);
    const auto failedResume = session.resume(); // 恢复失败必须保留句柄与自有计数。
    Check(failedResume.error == MemoryDebugThreadError::ResumeFailed && failedResume.errorWin32 == ERROR_ACCESS_DENIED
        && failedResume.ownedSuspend && failedResume.status == MemoryDebugThreadState::RecoveryRequired
        && failedResume.selectedThread.threadCreateTime100ns == fixture.identity.threadCreateTime100ns,
        "injected resume failure retains exact owned thread and recovery state");
    Check(::SuspendThread(fixture.worker) == 1 && ::ResumeThread(fixture.worker) == 2, "failed resume preserves one real owned suspension");
    Check(session.resume().success && !session.status().ownedSuspend, "resume retry balances retained suspension");
    ::InterlockedExchange(&failNextResume, 1);
    const auto snapshotRecovery = session.querySnapshot(fixture.identity); // 快照已读成功，恢复失败需同时保留两种事实。
    Check(snapshotRecovery.error == MemoryDebugThreadError::ResumeFailed && snapshotRecovery.hasSnapshot
        && snapshotRecovery.snapshot.rip != 0 && snapshotRecovery.ownedSuspend,
        "temporary snapshot keeps captured data and ownership when resume fails");
    Check(session.close().success, "close retries temporary snapshot recovery");
    Check(session.open(fixture.identity.pid, fixture.identity.processCreateTime100ns).success, "reopen after temporary recovery");
    ::InterlockedExchange(&failNextContext, 1);
    ::InterlockedExchange(&failNextResume, 1);
    const auto doubleFailure = session.querySnapshot(fixture.identity); // 两个 API 都失败仍不得丢弃暂停所有权。
    Check(doubleFailure.error == MemoryDebugThreadError::ResumeFailed && !doubleFailure.hasSnapshot
        && doubleFailure.ownedSuspend && doubleFailure.detail.find("Context capture also failed") != std::string::npos,
        "capture plus resume failure preserves both failure facts");
    ::InterlockedExchange(&failNextResume, 1);
    Check(!session.close().success && session.status().ownedSuspend, "failed close retains session for another recovery attempt");
    Check(session.close().success, "close retry restores and clears retained session");
    Check(session.open(fixture.identity.pid, fixture.identity.processCreateTime100ns).success, "reopen before wait failures");
    ::InterlockedExchange(&failNextWaitKind, 1);
    const auto processWaitFailure = session.enumerateThreads(); // 失败等待不可伪装为成功线程表。
    Check(processWaitFailure.error == MemoryDebugThreadError::OpenProcessFailed && processWaitFailure.errorWin32 == ERROR_INVALID_HANDLE,
        "injected process wait invalid handle is reported");
    Check(session.pause(fixture.identity).success, "pause before retained thread wait failure");
    ::InterlockedExchange(&failNextWaitKind, 2);
    const auto threadWaitFailure = session.status(); // 已持有暂停时，等待失败仍必须保留恢复责任。
    Check(threadWaitFailure.error == MemoryDebugThreadError::OpenThreadFailed && threadWaitFailure.errorWin32 == ERROR_INVALID_HANDLE
        && threadWaitFailure.ownedSuspend && threadWaitFailure.status == MemoryDebugThreadState::RecoveryRequired,
        "injected thread wait invalid handle retains suspension ownership");
    Check(session.resume().success, "wait failure recovery retains valid thread handle for retry");
    {
        MemoryDebugThreadSession scopedFault; // 析构 close 首次恢复失败，由 Impl 最后一次恢复配平。
        Check(scopedFault.open(fixture.identity.pid, fixture.identity.processCreateTime100ns).success
            && scopedFault.pause(fixture.identity).success, "destructor fault scenario owns suspension");
        ::InterlockedExchange(&failNextResume, 1);
    }
    Check(::SuspendThread(fixture.worker) == 0 && ::ResumeThread(fixture.worker) == 1, "destructor retries one injected resume failure without count leak");
#endif

    Check(session.pause(fixture.identity).success, "pause before close succeeds");
    Check(session.close().success && session.status().status == MemoryDebugThreadState::Closed, "close restores then releases process session");
    Check(::SuspendThread(fixture.worker) == 0 && ::ResumeThread(fixture.worker) == 1, "close leaves target suspend count at zero");
    {
        MemoryDebugThreadSession scoped; // 验证用户关闭页面或释放最后 shared_ptr 时自动恢复。
        Check(scoped.open(fixture.identity.pid, fixture.identity.processCreateTime100ns).success
            && scoped.pause(fixture.identity).success, "destructor scenario owns a pause");
    }
    Check(::SuspendThread(fixture.worker) == 0 && ::ResumeThread(fixture.worker) == 1, "destructor restores its own pause exactly once");
    Check(::CheckRemoteDebuggerPresent(fixture.child.hProcess, &debugged) && !debugged, "all controls keep Windows debugger attachment absent");

    Check(session.open(fixture.identity.pid, fixture.identity.processCreateTime100ns).success, "reopen child before exit scenario");
    if (mainRow != threads.threads.end())
    {
        Check(session.pause(mainRow->identity).success, "initial thread pause succeeds before own process exit");
        ::InterlockedExchange(&fixture.shared->exitProcess, 1); // 正在运行的自有工作线程让本子进程自行退出。
        Check(::WaitForSingleObject(fixture.child.hProcess, 10000) == WAIT_OBJECT_0, "own worker exits process while initial thread is paused");
        const auto exited = session.status(); // 持有原对象检查，不能按 PID 查询一个替代对象。
        Check(exited.error == MemoryDebugThreadError::TargetExited && exited.status == MemoryDebugThreadState::TargetExited
            && !exited.ownedSuspend, "target exit retires dead thread suspension ownership");
        Check(session.querySnapshot(fixture.identity).error == MemoryDebugThreadError::TargetExited, "exited target cannot be queried through stale identity");
    }
    Check(session.close().success, "exited session closes cleanly");
    std::cout << "THREAD_SESSION_CHECKS=" << checks << " FAILURES=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

#if defined(MEMORY_DEBUG_THREAD_SESSION_FAULT_TESTS)
// 独立故障构建仅编译此文件，让完整生产实现使用可一次失败的真实 API 转发器。
#define GetThreadContext MemoryDebugTestGetThreadContext
#define ResumeThread MemoryDebugTestResumeThread
#define WaitForSingleObject MemoryDebugTestWaitForSingleObject
#include "../../Ksword5.1/Ksword5.1/MemoryDock/MemoryDebugThreadSession.cpp"
#undef WaitForSingleObject
#undef ResumeThread
#undef GetThreadContext
#endif
