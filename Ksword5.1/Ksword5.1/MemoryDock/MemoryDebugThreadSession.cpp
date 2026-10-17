#include "MemoryDebugThreadSession.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <mutex>
#include <utility>

namespace ks::ui
{
    namespace
    {
        // ScopedHandle：自动关闭真正的 Win32 句柄，不接受进程伪句柄。
        class ScopedHandle final
        {
        public:
            // 构造时接管一个真实句柄；空值表示当前无对象。
            explicit ScopedHandle(HANDLE value = nullptr) : value_(value)
            {
            }
            // 析构只释放句柄；线程暂停的配平由上层 Impl 负责。
            ~ScopedHandle()
            {
                reset();
            }
            ScopedHandle(const ScopedHandle&) = delete;
            ScopedHandle& operator=(const ScopedHandle&) = delete;
            // get：返回借用句柄，不转移所有权。
            HANDLE get() const
            {
                return value_;
            }
            HANDLE release() // 交给持久会话管理，使旧对象不再关闭句柄。
            {
                const HANDLE previous = value_; // 需要转移的原始句柄。
                value_ = nullptr;
                return previous;
            }
            void reset(HANDLE value = nullptr) // 关闭旧句柄并接管新句柄。
            {
                if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(value_);
                }
                value_ = value;
            }
        private:
            HANDLE value_ = nullptr; // 当前独占持有的真实句柄。
        };

        // TimeValue：把 FILETIME 转为身份比较使用的无符号 100ns 值。
        std::uint64_t TimeValue(const FILETIME& value)
        {
            return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        }

        // LastFailure：保留真实 API 失败码，防止异常的零值被界面误认为成功。
        DWORD LastFailure()
        {
            const DWORD error = ::GetLastError(); // 当前失败 API 立即留下的错误。
            return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
        }

        // CreationTime：根据对象类型读取创建时间；输出只在成功时生效。
        bool CreationTime(const HANDLE handle, const bool thread, std::uint64_t& value)
        {
            FILETIME created{}; // 查询到的对象创建时间。
            FILETIME exited{}; // API 要求的退出时间占位。
            FILETIME kernel{}; // API 要求的内核 CPU 时间占位。
            FILETIME user{}; // API 要求的用户 CPU 时间占位。
            const BOOL queried = thread
                ? ::GetThreadTimes(handle, &created, &exited, &kernel, &user)
                : ::GetProcessTimes(handle, &created, &exited, &kernel, &user);
            if (!queried)
            {
                return false;
            }
            value = TimeValue(created);
            return value != 0;
        }

        // SameIdentity：比较四个身份字段，不依赖可能被复用的单独 PID/TID。
        bool SameIdentity(const MemoryDebugThreadIdentity& left, const MemoryDebugThreadIdentity& right)
        {
            return left.pid == right.pid && left.processCreateTime100ns == right.processCreateTime100ns
                && left.tid == right.tid && left.threadCreateTime100ns == right.threadCreateTime100ns;
        }

        // NativeAmd64：仅接纳原生 AMD64；不把 WOW64 或 ARM64 上的仿真线程交给 x64 CONTEXT。
        bool NativeAmd64(const HANDLE process, DWORD& error)
        {
            using QueryMachine = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*); // 新版 Windows 体系结构查询入口。
            const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll"); // 已加载系统模块的借用句柄。
            const auto queryMachine = reinterpret_cast<QueryMachine>(::GetProcAddress(kernel, "IsWow64Process2"));
            if (queryMachine != nullptr)
            {
                USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN; // 仿真体系结构；原生为 UNKNOWN。
                USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN; // 当前 Windows 内核体系结构。
                if (!queryMachine(process, &processMachine, &nativeMachine))
                {
                    error = LastFailure();
                    return false;
                }
                error = ERROR_NOT_SUPPORTED;
                return processMachine == IMAGE_FILE_MACHINE_UNKNOWN && nativeMachine == IMAGE_FILE_MACHINE_AMD64;
            }
            // 旧 Windows 无新入口时，先确认操作系统确为 AMD64，再排除 WOW64 目标。
            SYSTEM_INFO system{}; // 原生 Windows 系统体系结构。
            ::GetNativeSystemInfo(&system);
            BOOL wow64 = FALSE; // 目标是否在 WOW64 中执行。
            if (!::IsWow64Process(process, &wow64))
            {
                error = LastFailure();
                return false;
            }
            error = ERROR_NOT_SUPPORTED;
            return system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 && !wow64;
        }
    }

    // Impl：互斥锁同时保护句柄、强身份与一次暂停的所有权，避免关闭和 worker 操作竞争。
    struct MemoryDebugThreadSession::Impl
    {
        std::mutex mutex; // 所有公共调用的串行化锁。
        ScopedHandle process; // 保留进程对象，避免 PID 重用后误操作新进程。
        ScopedHandle thread; // 只在有自有暂停时保留的线程对象。
        std::uint32_t pid = 0; // 已绑定目标进程编号。
        std::uint64_t processCreation = 0; // 已绑定目标进程创建时间。
        MemoryDebugThreadIdentity selected; // 自有暂停对应的线程强身份。
        MemoryDebugThreadState state = MemoryDebugThreadState::Closed; // 当前可核对状态。
        bool owned = false; // 是否仍欠该线程一次 ResumeThread。

        // ~Impl：关闭调用恢复失败后的最后一次尽力恢复，不进行额外计数归零。
        ~Impl()
        {
            if (owned && thread.get() != nullptr && ::WaitForSingleObject(thread.get(), 0) != WAIT_OBJECT_0)
            {
                ::ResumeThread(thread.get());
            }
        }

        // result：生成独立值，供 UI 在不持锁、不借用句柄的情况下展示。
        MemoryDebugThreadResult result(const MemoryDebugThreadError error = MemoryDebugThreadError::None,
            const DWORD win32 = ERROR_SUCCESS, std::string detail = {}) const
        {
            MemoryDebugThreadResult output; // 当前调用独立拥有的结果值。
            output.success = error == MemoryDebugThreadError::None;
            output.error = error;
            output.errorWin32 = win32;
            output.detail = std::move(detail);
            output.status = state;
            output.pid = pid;
            output.processCreateTime100ns = processCreation;
            output.ownedSuspend = owned;
            output.selectedThread = selected;
            return output;
        }

        // forgetThread：只用于已经配平或已经退出的线程，绝不暗中丢弃待恢复所有权。
        void forgetThread()
        {
            owned = false;
            selected = {};
            thread.reset();
            state = process.get() == nullptr ? MemoryDebugThreadState::Closed : MemoryDebugThreadState::Ready;
        }

        // live：检查保留的进程对象和创建时间；线程退出时退休其无意义的暂停所有权。
        MemoryDebugThreadResult live()
        {
            if (process.get() == nullptr)
            {
                return result(MemoryDebugThreadError::NotOpen, ERROR_INVALID_HANDLE, "No process session is open.");
            }
            const DWORD wait = ::WaitForSingleObject(process.get(), 0); // 零等待，不阻塞 worker 生命周期。
            if (wait == WAIT_OBJECT_0)
            {
                forgetThread();
                state = MemoryDebugThreadState::TargetExited;
                return result(MemoryDebugThreadError::TargetExited, ERROR_PROCESS_ABORTED, "The retained process exited.");
            }
            if (wait == WAIT_FAILED)
            {
                return result(MemoryDebugThreadError::OpenProcessFailed, LastFailure(), "WaitForSingleObject(process) failed.");
            }
            // 即使句柄已经锚定对象，也复核创建时间，让调用方冻结的进程身份始终可追溯。
            std::uint64_t actualCreation = 0; // 当前保留对象的真实创建时间。
            if (!CreationTime(process.get(), false, actualCreation))
            {
                return result(MemoryDebugThreadError::OpenProcessFailed, LastFailure(), "GetProcessTimes failed.");
            }
            if (actualCreation != processCreation)
            {
                return result(MemoryDebugThreadError::ProcessIdentityMismatch, ERROR_INVALID_DATA, "Process creation time changed.");
            }
            if (owned)
            {
                const DWORD threadWait = ::WaitForSingleObject(thread.get(), 0); // 失败不能被当作线程仍在正常暂停。
                if (threadWait == WAIT_OBJECT_0)
                {
                    forgetThread();
                }
                else if (threadWait == WAIT_FAILED)
                {
                    state = MemoryDebugThreadState::RecoveryRequired;
                    return result(MemoryDebugThreadError::OpenThreadFailed, LastFailure(), "WaitForSingleObject(retained thread) failed; suspension ownership is retained.");
                }
            }
            return result();
        }

        // verifyThread：核对句柄实际 owner、TID、创建时间和运行状态；不会修改目标。
        MemoryDebugThreadResult verifyThread(const HANDLE handle, const MemoryDebugThreadIdentity& identity)
        {
            if (identity.pid != pid || identity.processCreateTime100ns != processCreation
                || identity.tid == 0 || identity.threadCreateTime100ns == 0)
            {
                return result(MemoryDebugThreadError::ThreadIdentityMismatch, ERROR_INVALID_PARAMETER, "Thread process identity does not match.");
            }
            const DWORD ownerPid = ::GetProcessIdOfThread(handle); // 在比较前区分 API 失败与真实 owner 错配。
            if (ownerPid == 0)
            {
                return result(MemoryDebugThreadError::OpenThreadFailed, LastFailure(), "GetProcessIdOfThread failed.");
            }
            const DWORD actualTid = ::GetThreadId(handle); // 用保留句柄确认 TID，而非只相信目录快照。
            if (actualTid == 0)
            {
                return result(MemoryDebugThreadError::OpenThreadFailed, LastFailure(), "GetThreadId failed.");
            }
            if (ownerPid != pid || actualTid != identity.tid)
            {
                return result(MemoryDebugThreadError::ThreadIdentityMismatch, ERROR_INVALID_DATA, "Thread owner or TID does not match.");
            }
            std::uint64_t actualCreation = 0; // 持有线程对象的真实创建时间。
            if (!CreationTime(handle, true, actualCreation))
            {
                return result(MemoryDebugThreadError::OpenThreadFailed, LastFailure(), "GetThreadTimes failed.");
            }
            if (actualCreation != identity.threadCreateTime100ns)
            {
                return result(MemoryDebugThreadError::ThreadIdentityMismatch, ERROR_INVALID_DATA, "Thread creation time does not match.");
            }
            const DWORD wait = ::WaitForSingleObject(handle, 0); // 判断原线程是否已经结束。
            if (wait == WAIT_OBJECT_0)
            {
                return result(MemoryDebugThreadError::ThreadExited, ERROR_THREAD_NOT_IN_PROCESS, "The retained thread exited.");
            }
            if (wait == WAIT_FAILED)
            {
                return result(MemoryDebugThreadError::OpenThreadFailed, LastFailure(), "WaitForSingleObject(thread) failed.");
            }
            return result();
        }

        // releaseSuspend：只调用一次 ResumeThread；失败时保留所有权和句柄供后续重试。
        MemoryDebugThreadResult releaseSuspend()
        {
            if (!owned)
            {
                return result(MemoryDebugThreadError::NotPaused, ERROR_INVALID_STATE, "No session-owned suspension exists.");
            }
            if (::WaitForSingleObject(thread.get(), 0) == WAIT_OBJECT_0)
            {
                forgetThread();
                return result();
            }
            const DWORD previous = ::ResumeThread(thread.get()); // 只配平本会话增加的一次，不清空外部计数。
            if (previous == static_cast<DWORD>(-1))
            {
                const DWORD error = LastFailure(); // 在其它 API 前保留恢复失败原因。
                if (::WaitForSingleObject(thread.get(), 0) == WAIT_OBJECT_0)
                {
                    forgetThread();
                    return result();
                }
                state = MemoryDebugThreadState::RecoveryRequired;
                return result(MemoryDebugThreadError::ResumeFailed, error, "ResumeThread failed; the owned suspension is retained for retry.");
            }
            // 外部调用者可能已经恢复过线程。previous 为零时也绝不再减一次。
            forgetThread();
            return result();
        }

        // acquireSuspend：打开并验证指定线程后仅暂停一次，外部已有暂停时立即撤销自己的一次。
        MemoryDebugThreadResult acquireSuspend(const MemoryDebugThreadIdentity& identity)
        {
            const auto checked = live(); // 首先验证进程仍是会话原对象。
            if (!checked.success)
            {
                return checked;
            }
            if (owned)
            {
                return result(SameIdentity(selected, identity) ? MemoryDebugThreadError::AlreadyPaused
                    : MemoryDebugThreadError::DifferentThreadPaused, ERROR_BUSY, "A session-owned thread suspension already exists.");
            }
            if (identity.pid != pid || identity.processCreateTime100ns != processCreation
                || identity.tid == 0 || identity.threadCreateTime100ns == 0)
            {
                return result(MemoryDebugThreadError::ThreadIdentityMismatch, ERROR_INVALID_PARAMETER, "Requested thread identity is invalid for this process.");
            }
            ScopedHandle candidate(::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, identity.tid)); // 仅申请控制及读取必要权限。
            if (candidate.get() == nullptr)
            {
                return result(MemoryDebugThreadError::OpenThreadFailed, LastFailure(), "OpenThread failed.");
            }
            const auto verified = verifyThread(candidate.get(), identity); // 先用对象身份复核，再执行暂停。
            if (!verified.success)
            {
                return verified;
            }
            const DWORD previous = ::SuspendThread(candidate.get()); // 返回增加前的计数，不能用于全进程停止声明。
            if (previous == static_cast<DWORD>(-1))
            {
                return result(MemoryDebugThreadError::SuspendFailed, LastFailure(), "SuspendThread failed.");
            }
            // 从成功增加计数开始，所有出口都必须保留所有权，或完整配平这一次暂停。
            thread.reset(candidate.release());
            selected = identity;
            owned = true;
            state = MemoryDebugThreadState::Paused;
            if (previous != 0)
            {
                const auto restored = releaseSuspend(); // 已有暂停属于外部，本会话立即退出而不继续读取。
                if (!restored.success)
                {
                    return restored;
                }
                return result(MemoryDebugThreadError::ThreadAlreadySuspended, ERROR_BUSY, "Thread was already suspended; the added count was balanced.");
            }
            return result();
        }

        // readContext：仅 GetThreadContext，不设置 TF/DR/断点，不接管异常或 Windows 调试端口。
        MemoryDebugThreadResult readContext(const bool transient)
        {
            const auto verified = verifyThread(thread.get(), selected); // 捕获前再次检查保留线程的强身份。
            if (!verified.success)
            {
                if (verified.error == MemoryDebugThreadError::ThreadExited)
                {
                    forgetThread();
                    return result(verified.error, verified.errorWin32, verified.detail);
                }
                return verified;
            }
#if defined(_M_X64) || defined(__x86_64__)
            CONTEXT context{}; // Windows 要求的对齐上下文结构，始终只用于读取。
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
            if (!::GetThreadContext(thread.get(), &context))
            {
                return result(MemoryDebugThreadError::ContextFailed, LastFailure(), "GetThreadContext failed.");
            }
            auto output = result(); // 快照与会话状态一起返回，但不向外暴露 CONTEXT 或句柄。
            output.hasSnapshot = true;
            auto& snapshot = output.snapshot; // 本次独立结果中的寄存器输出。
            snapshot.identity = selected;
            snapshot.rax = context.Rax;
            snapshot.rbx = context.Rbx;
            snapshot.rcx = context.Rcx;
            snapshot.rdx = context.Rdx;
            snapshot.rsi = context.Rsi;
            snapshot.rdi = context.Rdi;
            snapshot.rbp = context.Rbp;
            snapshot.rsp = context.Rsp;
            // 保留所有 AMD64 通用寄存器，避免把部分上下文显示成完整调试器停止状态。
            snapshot.r8 = context.R8;
            snapshot.r9 = context.R9;
            snapshot.r10 = context.R10;
            snapshot.r11 = context.R11;
            snapshot.r12 = context.R12;
            snapshot.r13 = context.R13;
            snapshot.r14 = context.R14;
            snapshot.r15 = context.R15;
            snapshot.rip = context.Rip;
            snapshot.rflags = context.EFlags;
            // 调试寄存器仅提供观测值；本会话不创建、删除或移动任何硬件断点。
            snapshot.dr0 = context.Dr0;
            snapshot.dr1 = context.Dr1;
            snapshot.dr2 = context.Dr2;
            snapshot.dr3 = context.Dr3;
            snapshot.dr6 = context.Dr6;
            snapshot.dr7 = context.Dr7;
            FILETIME captured{}; // 本次上下文读取完成后的系统时间。
            ::GetSystemTimeAsFileTime(&captured);
            snapshot.capturedAt100ns = TimeValue(captured);
            snapshot.transientPause = transient;
            return output;
#else
            static_cast<void>(transient);
            return result(MemoryDebugThreadError::UnsupportedArchitecture, ERROR_NOT_SUPPORTED, "The controller must be built for AMD64.");
#endif
        }
    };

    // 构造函数：只建立内部所有权容器，不预先打开或暂停任何目标。
    MemoryDebugThreadSession::MemoryDebugThreadSession() : impl_(std::make_unique<Impl>())
    {
    }

    // 析构函数：关闭失败也保留至 Impl 最后一次恢复尝试，不能把恢复失败向析构外抛出。
    MemoryDebugThreadSession::~MemoryDebugThreadSession()
    {
        try
        {
            close();
        }
        catch (...)
        {
            // 无异常析构契约；Impl 仍有句柄与 owned 标记并执行最后的尽力配平。
        }
    }

    // open：输入必须是选择时冻结的强身份；任何验证失败都不接管新目标。
    MemoryDebugThreadResult MemoryDebugThreadSession::open(const std::uint32_t pid,
        const std::uint64_t processCreateTime100ns)
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 保护会话绑定与正在进行的恢复。
        if (pid == 0 || processCreateTime100ns == 0)
        {
            return impl_->result(MemoryDebugThreadError::InvalidIdentity, ERROR_INVALID_PARAMETER, "A nonzero PID and process creation time are required.");
        }
        if (pid == ::GetCurrentProcessId())
        {
            return impl_->result(MemoryDebugThreadError::SelfTarget, ERROR_NOT_SUPPORTED, "The controller cannot suspend its own process threads.");
        }
        if (impl_->process.get() != nullptr)
        {
            if (pid == impl_->pid && processCreateTime100ns == impl_->processCreation)
            {
                return impl_->live();
            }
            return impl_->result(MemoryDebugThreadError::InvalidIdentity, ERROR_BUSY, "Close the existing process session before opening another identity.");
        }
        ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)); // 查询与生命周期权限。
        if (process.get() == nullptr)
        {
            return impl_->result(MemoryDebugThreadError::OpenProcessFailed, LastFailure(), "OpenProcess failed.");
        }
        std::uint64_t actualCreation = 0; // 用新打开对象读出的创建时间，而不是再次信任 PID。
        if (!CreationTime(process.get(), false, actualCreation))
        {
            return impl_->result(MemoryDebugThreadError::OpenProcessFailed, LastFailure(), "GetProcessTimes failed.");
        }
        if (actualCreation != processCreateTime100ns)
        {
            return impl_->result(MemoryDebugThreadError::ProcessIdentityMismatch, ERROR_INVALID_DATA, "Selected process creation time does not match.");
        }
        DWORD machineError = ERROR_SUCCESS; // 体系结构查询失败或不支持的实际原因。
        if (!NativeAmd64(process.get(), machineError))
        {
            return impl_->result(MemoryDebugThreadError::UnsupportedArchitecture, machineError, "Only native AMD64 process contexts are supported.");
        }
        impl_->process.reset(process.release());
        impl_->pid = pid;
        impl_->processCreation = processCreateTime100ns;
        impl_->state = MemoryDebugThreadState::Ready;
        return impl_->live();
    }

    // enumerateThreads：只保留属于当前进程对象且可核对创建时间的线程，拒绝裸 TID 行。
    MemoryDebugThreadResult MemoryDebugThreadSession::enumerateThreads()
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 防止枚举中目标被替换。
        auto output = impl_->live(); // 退出的目标不返回冒充成功的空线程表。
        if (!output.success)
        {
            return output;
        }
        ScopedHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)); // Toolhelp 线程目录只读快照。
        if (snapshot.get() == INVALID_HANDLE_VALUE)
        {
            return impl_->result(MemoryDebugThreadError::EnumerateFailed, LastFailure(), "CreateToolhelp32Snapshot failed.");
        }
        THREADENTRY32 entry{}; // 每一项仅从目录中取得 owner/TID/优先级线索。
        entry.dwSize = sizeof(entry);
        BOOL present = ::Thread32First(snapshot.get(), &entry); // 开始枚举全局目录，不打开其它进程线程。
        while (present)
        {
            if (entry.th32OwnerProcessID == impl_->pid)
            {
                ScopedHandle candidate(::OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                    FALSE, entry.th32ThreadID)); // 只为本目标的候选线程读取强身份。
                MemoryDebugThreadRecord record; // 独立线程值，不保留枚举句柄。
                record.identity.pid = impl_->pid;
                record.identity.processCreateTime100ns = impl_->processCreation;
                record.identity.tid = entry.th32ThreadID;
                record.basePriority = entry.tpBasePri;
                if (candidate.get() != nullptr
                    && CreationTime(candidate.get(), true, record.identity.threadCreateTime100ns)
                    && impl_->verifyThread(candidate.get(), record.identity).success)
                {
                    record.ownedPause = impl_->owned && SameIdentity(record.identity, impl_->selected);
                    output.threads.push_back(record);
                }
                else
                {
                    ++output.inaccessibleThreads;
                }
            }
            present = ::Thread32Next(snapshot.get(), &entry);
        }
        const DWORD enumerationError = ::GetLastError(); // 必须在其它 API 之前核对枚举结束原因。
        if (enumerationError != ERROR_NO_MORE_FILES)
        {
            output.success = false;
            output.error = MemoryDebugThreadError::EnumerateFailed;
            output.errorWin32 = enumerationError;
            output.detail = "Thread32First/Thread32Next failed before enumeration completed.";
        }
        const auto finalStatus = impl_->live(); // 目录完成后再检查目标，避免退出被显示成成功空表。
        return finalStatus.success ? output : finalStatus;
    }

    // pause：外部只需给出枚举行冻结的 identity，本会话负责计数与句柄所有权。
    MemoryDebugThreadResult MemoryDebugThreadSession::pause(const MemoryDebugThreadIdentity& identity)
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 暂停、快照和恢复不能交叉执行。
        return impl_->acquireSuspend(identity);
    }

    // querySnapshot：临时暂停的所有失败出口也恢复；只有显式 pause 获得的暂停会保留。
    MemoryDebugThreadResult MemoryDebugThreadSession::querySnapshot(const MemoryDebugThreadIdentity& identity)
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 整个捕获和恢复过程保持单一所有权。
        const auto checked = impl_->live(); // 优先拒绝已退出目标及无会话调用。
        if (!checked.success)
        {
            return checked;
        }
        const bool transient = !impl_->owned; // 本次是否需要创建并立即释放暂停。
        if (impl_->owned && !SameIdentity(identity, impl_->selected))
        {
            return impl_->result(MemoryDebugThreadError::DifferentThreadPaused, ERROR_BUSY, "Resume the session-owned thread before querying another thread.");
        }
        if (transient)
        {
            const auto paused = impl_->acquireSuspend(identity); // 增加计数失败或发现外部暂停时立即返回。
            if (!paused.success)
            {
                return paused;
            }
        }
        auto output = impl_->readContext(transient); // 捕获失败也继续走下方临时暂停恢复路径。
        if (transient && impl_->owned)
        {
            auto restored = impl_->releaseSuspend(); // 每个临时暂停恰好执行一次配平尝试。
            if (!restored.success)
            {
                restored.hasSnapshot = output.hasSnapshot;
                restored.snapshot = output.snapshot;
                if (!output.success)
                {
                    restored.detail += " Context capture also failed, Win32=" + std::to_string(output.errorWin32) + ".";
                }
                return restored;
            }
            output.status = restored.status;
            output.ownedSuspend = restored.ownedSuspend;
            output.selectedThread = restored.selectedThread;
        }
        return output;
    }

    // resume：不使用循环恢复，不减少任何本会话未拥有的暂停计数。
    MemoryDebugThreadResult MemoryDebugThreadSession::resume()
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 与 close/querySnapshot 串行配平。
        const auto checked = impl_->live(); // 进程退出会自动退休已经失效的线程暂停。
        return checked.success ? impl_->releaseSuspend() : checked;
    }

    // close：恢复失败时保持会话供用户重试；成功后才清空进程身份与所有句柄。
    MemoryDebugThreadResult MemoryDebugThreadSession::close()
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 防止关闭释放仍在 worker 使用的句柄。
        if (impl_->owned)
        {
            const auto restored = impl_->releaseSuspend(); // 对死线程可直接退休，对活线程只配平一次。
            if (!restored.success)
            {
                return restored;
            }
        }
        impl_->thread.reset();
        impl_->process.reset();
        impl_->selected = {};
        impl_->pid = 0;
        impl_->processCreation = 0;
        impl_->state = MemoryDebugThreadState::Closed;
        return impl_->result();
    }

    // status：没有会话视为成功关闭；有会话时使用保留对象查询生命周期。
    MemoryDebugThreadResult MemoryDebugThreadSession::status()
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex); // 状态读取也可能退休已经退出的线程。
        return impl_->process.get() == nullptr ? impl_->result() : impl_->live();
    }
}
