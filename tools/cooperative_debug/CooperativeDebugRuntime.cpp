#include "CooperativeDebugRuntime.h"
#include "CooperativeDebugRuntimeInternal.h"
#include <intrin.h>

#if !defined(_M_X64)
#error Cooperative debugging currently supports x64 only.
#endif

extern "C" void KswordCooperativeCheckpointTrap();
// 汇编返回内部实际 INT3，避免增量链接 ILT 函数跳板造成地址误判。
extern "C" void *KswordCooperativeCheckpointAddress();

namespace ksword::cooperative::runtime::detail
{
    Runtime instance;
    // TLS 只标记组件主动发出的检查点，不接管目标其它 INT3。
    // checkpointExpected：TLS检查点授权标记。
    thread_local bool checkpointExpected = false;
    // trapFlagOwned：只由被武装的同一线程记录自有 TF，不借用其它线程的异常。
    thread_local bool trapFlagOwned = false;
    // unsafeDepth：VEH 可在任意指令边界读取，volatile 保留获取前/释放后的实际 TLS 写入。
    // 获取锁之前进入、释放锁之后退出，禁止把持锁现场发布为暂停。
    thread_local volatile unsigned unsafeDepth = 0;
    // interruptedStep：危险区中的自有 TF 已清除，等该线程释放外层锁后再退休全局记录。
    thread_local bool interruptedStep = false;
    // interruptedToken：延迟清理仍只属于发出 TF 的原会话。
    thread_local std::uint64_t interruptedTokenLow = 0;
    thread_local std::uint64_t interruptedTokenHigh = 0;
    // ownedToken：本线程自有 TF 的会话身份，不从异线程修改 TLS。
    thread_local std::uint64_t ownedTokenLow = 0;
    thread_local std::uint64_t ownedTokenHigh = 0;
    // TrapFlag：x64单步TF位。
    constexpr DWORD TrapFlag = 0x100;
    // DebugSingleStep：DR6单步原因位。
    constexpr DWORD DebugSingleStep = 1U << 14;

    // 延迟收尾只在该线程已释放运行时外层锁后调用；它不代表成功完成单步。
    void FinishInterruptedStep()
    {
        if (!interruptedStep || unsafeDepth != 0)
            return;
        Runtime &state = instance;
        AcquireSRWLockExclusive(&state.lock);
        const bool ownsArmedRecord = state.armedTid == GetCurrentThreadId() &&
                                     state.armedTokenLow == interruptedTokenLow &&
                                     state.armedTokenHigh == interruptedTokenHigh;
        if (ownsArmedRecord)
        {
            if (state.armedThread)
                CloseHandle(state.armedThread);
            state.armedThread = nullptr;
            state.armedTid = 0;
            // 原控制者仍有效时保留明确的中断状态，无停止编号和寄存器现场。
            if (state.enabled.load() && state.connected &&
                state.tokenLow == interruptedTokenLow && state.tokenHigh == interruptedTokenHigh)
            {
                // 解锁后重新取锁期间，命令线程可能已确认新的暂停请求；不能覆盖它。
                state.state = state.pauseRequested ? State::PauseRequested : State::Interrupted;
                state.command = Command::None;
                state.stopId = 0;
                state.snapshot = {};
                state.lastError = 0;
            }
        }
        ReleaseSRWLockExclusive(&state.lock);
        interruptedStep = false;
    }

    // 短锁的 TLS 边界覆盖获取动作本身；任何自有 TF 都先放行，再于解锁后报告中断。
    class RuntimeLock final
    {
      public:
        explicit RuntimeLock(Runtime &state) : state_(state)
        {
            ++unsafeDepth;
            AcquireSRWLockExclusive(&state_.lock);
        }
        ~RuntimeLock()
        {
            ReleaseSRWLockExclusive(&state_.lock);
            --unsafeDepth;
            FinishInterruptedStep();
        }
        RuntimeLock(const RuntimeLock &) = delete;
        RuntimeLock &operator=(const RuntimeLock &) = delete;

      private:
        Runtime &state_; // state_：当前短锁宿主，析构时先解锁再处理延迟中断。
    };

    std::uint64_t CreationTime(HANDLE process)
    {
        // created 为进程身份；其余时间是 Windows API 要求的输出参数。
        FILETIME created{};
        // exited：退出时间输出。
        FILETIME exited{};
        // kernel：内核用时输出。
        FILETIME kernel{};
        // user：用户用时或用户SID缓冲。
        FILETIME user{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
            return 0;
        return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    // 放行时保留已武装 TF 的线程身份，下一次真正 #DB 必须清理本组件 TF。
    void ReleaseController()
    {
        // state：当前状态或其短锁保护引用。
        Runtime &state = instance;
        const RuntimeLock lock(state);
        state.connected = false;
        state.pauseRequested = false;
        state.state = State::Running;
        state.command = Command::Continue;
        state.stopId = 0;
        state.snapshot = {};
        if (state.owner)
            CloseHandle(state.owner);
        state.owner = nullptr;
        state.tokenLow = 0;
        state.tokenHigh = 0;
        state.lastError = 0;
        SetEvent(state.wake);
    }

    // 固定整数快照直接来自正在处理的真实异常 CONTEXT。
    RegisterSnapshot Capture(const CONTEXT &context, DWORD tid)
    {
        // result：本次返回结果。
        RegisterSnapshot result{};
        result.threadId = tid;
        result.rip = context.Rip;
        result.rsp = context.Rsp;
        result.rflags = context.EFlags;
        result.rax = context.Rax;
        result.rbx = context.Rbx;
        result.rcx = context.Rcx;
        result.rdx = context.Rdx;
        result.rbp = context.Rbp;
        result.rsi = context.Rsi;
        result.rdi = context.Rdi;
        result.r8 = context.R8;
        result.r9 = context.R9;
        result.r10 = context.R10;
        result.r11 = context.R11;
        result.r12 = context.R12;
        result.r13 = context.R13;
        result.r14 = context.R14;
        result.r15 = context.R15;
        return result;
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS *exception)
    {
        if (!exception || !exception->ExceptionRecord || !exception->ContextRecord)
            return EXCEPTION_CONTINUE_SEARCH;
        // state：当前状态或其短锁保护引用。
        Runtime &state = instance;
        // context：当前真实异常上下文。
        CONTEXT &context = *exception->ContextRecord;
        // code：当前异常代码。
        const DWORD code = exception->ExceptionRecord->ExceptionCode;
        // tid：当前异常线程标识。
        const DWORD tid = GetCurrentThreadId();
        // checkpoint：是否为本组件明确发出的INT3。
        const bool checkpoint = code == EXCEPTION_BREAKPOINT && checkpointExpected &&
                                exception->ExceptionRecord->ExceptionAddress == state.trapAddress;
        // 无关异常不获取运行时锁，保留目标自己的嵌套/未知异常处理链。
        const bool ownedTrap = code == EXCEPTION_SINGLE_STEP && trapFlagOwned;
        if (!checkpoint && !ownedTrap)
            return EXCEPTION_CONTINUE_SEARCH;
        // 在临界区停止会让命令和租约线程永远等锁。只撤销自己设置的 TF，
        // 不在此处取锁，也不把后续多条执行冒充一次成功单步。
        if (ownedTrap && unsafeDepth != 0)
        {
            state.activeHandlers.fetch_add(1);
            trapFlagOwned = false;
            context.EFlags &= ~TrapFlag;
            context.Dr6 &= ~static_cast<DWORD64>(DebugSingleStep);
            interruptedTokenLow = ownedTokenLow;
            interruptedTokenHigh = ownedTokenHigh;
            interruptedStep = true;
            state.activeHandlers.fetch_sub(1);
            return (context.Dr6 & 0xA00FU) != 0 ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_CONTINUE_EXECUTION;
        }
        AcquireSRWLockExclusive(&state.lock);
        // 已退出的线程对象不能把复用后的 TID 授权给新的异常。
        if (state.armedThread && WaitForSingleObject(state.armedThread, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(state.armedThread);
            state.armedThread = nullptr;
            state.armedTid = 0;
        }
        // ownStep：是否为本组件武装线程的单步异常。
        const bool ownStep = ownedTrap && state.armedTid == tid && state.armedThread;
        // stepSessionMatches：TF是否仍属于当前会话。
        const bool stepSessionMatches =
            ownStep && state.armedTokenLow == state.tokenLow && state.armedTokenHigh == state.tokenHigh;
        if (!checkpoint && !ownStep)
        {
            ReleaseSRWLockExclusive(&state.lock);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        state.activeHandlers.fetch_add(1);
        if (checkpoint)
            context.Rip = reinterpret_cast<DWORD64>(state.trapAddress) + 1;
        if (ownStep)
        {
            trapFlagOwned = false;
            // 无调试对象的 Windows VEH 可给出 DR6=0；以自有 TF 线程租约识别，
            // 仍把明确的外部硬件断点、BD/BT 交回原异常处理链。
            context.EFlags &= ~TrapFlag;
            context.Dr6 &= ~static_cast<DWORD64>(DebugSingleStep);
            state.armedTid = 0;
            if (state.armedThread)
                CloseHandle(state.armedThread);
            state.armedThread = nullptr;
            // 同时命中的外部硬件断点仍归其它异常处理器，不能被组件吞掉。
            if ((context.Dr6 & 0xA00FU) != 0)
            {
                ReleaseSRWLockExclusive(&state.lock);
                state.activeHandlers.fetch_sub(1);
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
        // shouldPause：当前异常是否允许发布为停点。
        const bool shouldPause = state.enabled.load() && state.connected && tid != state.workerTid &&
                                 state.pausedTid == 0 && state.armedTid == 0 &&
                                 ((ownStep && stepSessionMatches) || (checkpoint && state.pauseRequested));
        if (!shouldPause || (checkpoint && (context.EFlags & TrapFlag) != 0))
        {
            if (shouldPause)
            {
                state.lastError = ERROR_BUSY;
                state.pauseRequested = false;
                state.state = State::Running;
            }
            ReleaseSRWLockExclusive(&state.lock);
            state.activeHandlers.fetch_sub(1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        state.pausedTid = tid;
        state.pauseRequested = false;
        state.command = Command::None;
        state.stopId = ++state.stopCounter;
        state.snapshot = Capture(context, tid);
        state.state = State::Paused;
        // pauseTokenLow：暂停现场原会话低位。
        const std::uint64_t pauseTokenLow = state.tokenLow;
        // pauseTokenHigh：暂停现场原会话高位。
        const std::uint64_t pauseTokenHigh = state.tokenHigh;
        ReleaseSRWLockExclusive(&state.lock);

        // 等待期间只有当前线程停驻；命令线程与目标其它业务线程保持可调度。
        for (;;)
        {
            WaitForSingleObject(state.wake, 100);
            AcquireSRWLockExclusive(&state.lock);
            // ownerAlive：控制进程对象仍存活。
            const bool ownerAlive = state.owner && WaitForSingleObject(state.owner, 0) == WAIT_TIMEOUT;
            // valid：现场租约及会话身份仍有效。
            const bool valid = state.enabled.load() && state.connected && ownerAlive &&
                               GetTickCount64() < state.deadline && pauseTokenLow == state.tokenLow &&
                               pauseTokenHigh == state.tokenHigh;
            if (!valid || state.command != Command::None)
            {
                // step：本次恢复是否需要真实单步。
                const bool step = valid && state.command == Command::Step;
                if (step)
                {
                    state.armedThread = OpenThread(SYNCHRONIZE, FALSE, tid);
                    if (state.armedThread)
                    {
                        state.armedTid = tid;
                        state.armedTokenLow = state.tokenLow;
                        state.armedTokenHigh = state.tokenHigh;
                        ownedTokenLow = state.tokenLow;
                        ownedTokenHigh = state.tokenHigh;
                        trapFlagOwned = true;
                        context.EFlags |= TrapFlag;
                    }
                    else
                        state.lastError = GetLastError();
                }
                state.command = Command::None;
                state.pausedTid = 0;
                state.stopId = 0;
                state.snapshot = {};
                state.state = state.pauseRequested ? State::PauseRequested : State::Running;
                ReleaseSRWLockExclusive(&state.lock);
                break;
            }
            ReleaseSRWLockExclusive(&state.lock);
        }
        state.activeHandlers.fetch_sub(1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

namespace ksword::cooperative::runtime
{
    std::uint32_t Start(const Config &config)
    {
        detail::Runtime &state = detail::instance;
        if (state.worker || state.handler)
            return ERROR_ALREADY_EXISTS;
        if (config.leaseMs < 500 || config.leaseMs > 60000)
            return ERROR_INVALID_PARAMETER;
        // debuggerPresent：原生调试器存在状态。
        BOOL debuggerPresent = FALSE;
        if (!CheckRemoteDebuggerPresent(GetCurrentProcess(), &debuggerPresent))
            return GetLastError();
        if (debuggerPresent || IsDebuggerPresent())
            return ERROR_BUSY;
        state.pid = GetCurrentProcessId();
        state.trapAddress = KswordCooperativeCheckpointAddress();
        state.creation = detail::CreationTime(GetCurrentProcess());
        state.leaseMs = config.leaseMs;
        state.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state.stop || !state.wake)
        {
            Stop();
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        // endpointError：端点创建错误。
        const DWORD endpointError = detail::CreateEndpoint();
        if (endpointError)
        {
            Stop();
            return endpointError;
        }
        state.handler = AddVectoredExceptionHandler(1, detail::Handler);
        if (!state.handler)
        {
            // error：本次操作系统错误。
            const DWORD error = GetLastError();
            Stop();
            return error;
        }
        state.enabled.store(true);
        state.worker = CreateThread(nullptr, 0, detail::Worker, nullptr, 0, &state.workerTid);
        if (!state.worker)
        {
            // error：本次操作系统错误。
            const DWORD error = GetLastError();
            Stop();
            return error;
        }
        return ERROR_SUCCESS;
    }

    void Checkpoint()
    {
        detail::Runtime &state = detail::instance;
        // requested：当前调用是否需要主动检查点。
        bool requested = false;
        {
            const detail::RuntimeLock lock(state);
            requested = state.enabled.load() && state.pauseRequested && state.connected &&
                        state.pausedTid == 0 && state.armedTid == 0 && GetCurrentThreadId() != state.workerTid;
            if (requested)
                state.activeCheckpoints.fetch_add(1);
        }
        if (!requested)
            return;
        detail::checkpointExpected = true;
        KswordCooperativeCheckpointTrap();
        detail::checkpointExpected = false;
        state.activeCheckpoints.fetch_sub(1);
    }

    void Stop()
    {
        detail::Runtime &state = detail::instance;
        {
            const detail::RuntimeLock lock(state);
            state.enabled.store(false);
        }
        if (state.stop)
            SetEvent(state.stop);
        if (state.wake)
            detail::ReleaseController();
        if (state.pipe != INVALID_HANDLE_VALUE)
            CancelIoEx(state.pipe, nullptr);
        if (state.worker)
        {
            WaitForSingleObject(state.worker, INFINITE);
            CloseHandle(state.worker);
            state.worker = nullptr;
        }
        // 静态链接组件可以保留安全 handler，直到已发出的 TF 确实返回。
        while (state.activeHandlers.load() != 0 || state.activeCheckpoints.load() != 0)
            Sleep(1);
        // safeToRemove：是否可安全撤销VEH。
        bool safeToRemove = false;
        {
            const detail::RuntimeLock lock(state);
            if (state.armedThread && WaitForSingleObject(state.armedThread, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(state.armedThread);
                state.armedThread = nullptr;
                state.armedTid = 0;
            }
            safeToRemove = state.armedTid == 0;
        }
        if (safeToRemove && state.handler)
        {
            RemoveVectoredExceptionHandler(state.handler);
            state.handler = nullptr;
        }
        if (state.pipe != INVALID_HANDLE_VALUE)
            CloseHandle(state.pipe);
        if (state.stop)
            CloseHandle(state.stop);
        if (state.wake)
            CloseHandle(state.wake);
        state.pipe = INVALID_HANDLE_VALUE;
        state.stop = state.wake = nullptr;
    }
}
