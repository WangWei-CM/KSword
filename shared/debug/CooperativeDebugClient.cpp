#include "CooperativeDebugClient.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <mutex>
#include <sstream>
#include <windows.h>

namespace ksword::cooperative
{
    namespace
    {
        // 进程对象创建时间用于端点身份核验，失败返回零并保留 Win32 错误。
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

        // 每条固定消息都有有界等待；取消后先收割 I/O，避免栈上 OVERLAPPED 悬空。
        DWORD Transfer(HANDLE pipe, HANDLE cancel, void *buffer, DWORD size, bool writing)
        {
            // pending：本次异步管道I/O记录。
            OVERLAPPED pending{};
            pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!pending.hEvent)
                return GetLastError();
            // bytes：本次实际传输字节数。
            DWORD bytes = 0;
            // completed：I/O是否同步完成。
            BOOL completed = writing ? WriteFile(pipe, buffer, size, &bytes, &pending)
                                     : ReadFile(pipe, buffer, size, &bytes, &pending);
            // error：本次操作系统错误。
            DWORD error = completed ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                // events：取消及完成事件等待组。
                HANDLE events[2]{cancel, pending.hEvent};
                // waited：事件等待结果。
                const DWORD waited = WaitForMultipleObjects(2, events, FALSE, 2000);
                if (waited != WAIT_OBJECT_0 + 1)
                {
                    CancelIoEx(pipe, &pending);
                    GetOverlappedResult(pipe, &pending, &bytes, TRUE);
                    error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
                }
                else if (!GetOverlappedResult(pipe, &pending, &bytes, FALSE))
                    error = GetLastError();
                else
                    error = ERROR_SUCCESS;
            }
            CloseHandle(pending.hEvent);
            if (error == ERROR_SUCCESS && bytes != size)
                error = ERROR_INVALID_DATA;
            return error;
        }

        // 统一构造明确失败，调用方可以同时读取错误和进程状态。
        Result Failure(DWORD error, const char *detail, State state = State::Unavailable)
        {
            // result：本次返回结果。
            Result result;
            result.errorWin32 = error;
            result.state = state;
            result.detail = detail;
            return result;
        }
    }

    struct Session::Impl
    {
        // mutex：串行化本对象调用的锁。
        std::recursive_mutex mutex;
        // 关闭代次避免 open 重置取消事件时丢失并发到达的 UI 关闭请求。
        std::atomic<std::uint64_t> cancellationGeneration{0};
        std::uint64_t connectionGeneration = 0;
        // cancel：本地关闭取消事件。
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // pipe：当前独占管道句柄。
        HANDLE pipe = INVALID_HANDLE_VALUE;
        // target：保留目标身份的进程句柄。
        HANDLE target = nullptr;
        // pid：目标进程标识。
        std::uint32_t pid = 0;
        // 创建时间、会话令牌与请求编号分别固定进程和每次命令身份。
        // creation：进程创建时间身份。
        std::uint64_t creation = 0;
        // tokenLow：随机会话令牌低位。
        std::uint64_t tokenLow = 0;
        // tokenHigh：随机会话令牌高位。
        std::uint64_t tokenHigh = 0;
        // requestId：当前请求序号。
        std::uint64_t requestId = 0;

        // 只关闭本对象持有的句柄，不终止或附加目标进程。
        void reset()
        {
            if (pipe != INVALID_HANDLE_VALUE)
                CloseHandle(pipe);
            if (target)
                CloseHandle(target);
            pipe = INVALID_HANDLE_VALUE;
            target = nullptr;
            pid = 0;
            connectionGeneration = 0;
            creation = tokenLow = tokenHigh = requestId = 0;
        }
    };

    // PID 和创建时间共同组成确定端点名称。
    std::wstring EndpointName(std::uint32_t pid, std::uint64_t creationTime100ns)
    {
        // text：端点名称格式化缓冲。
        std::wostringstream text;
        text << L"\\\\.\\pipe\\KSword.CooperativeDebug.v1." << pid << L'.' << std::hex << creationTime100ns;
        return text.str();
    }

    Session::Session() : impl_(std::make_unique<Impl>())
    {
    }
    Session::~Session()
    {
        close();
        // 外部必须保持调用生命周期；析构只等待已取消的调用收割其 I/O。
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        impl_->reset();
        if (impl_->cancel)
            CloseHandle(impl_->cancel);
    }

    // 保留真实目标句柄、核验管道服务端，再进行版本化握手。
    Result Session::open(std::uint32_t pid, std::uint64_t creationTime100ns)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        // generation：本次连接代次；并发 close 会让后续握手立即失败。
        const std::uint64_t generation = impl_->cancellationGeneration.fetch_add(1) + 1;
        impl_->reset();
        if (!impl_->cancel)
            return Failure(ERROR_NOT_ENOUGH_MEMORY, "Cannot allocate cooperative cancellation event");
        ResetEvent(impl_->cancel);
        impl_->connectionGeneration = generation;
        if (pid == 0 || creationTime100ns == 0 || pid == GetCurrentProcessId())
            return Failure(ERROR_INVALID_PARAMETER, "Invalid cooperative target identity");
        impl_->target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!impl_->target)
            return Failure(GetLastError(), "Cannot retain target process identity");
        if (CreationTime(impl_->target) != creationTime100ns ||
            WaitForSingleObject(impl_->target, 0) != WAIT_TIMEOUT)
        {
            impl_->reset();
            return Failure(ERROR_INVALID_PARAMETER, "Target process identity changed");
        }
        impl_->pid = pid;
        impl_->creation = creationTime100ns;
        // endpoint：包含PID和创建时间的本地端点。
        const std::wstring endpoint = EndpointName(pid, creationTime100ns);
        impl_->pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
        if (impl_->pipe == INVALID_HANDLE_VALUE)
        {
            // error：本次操作系统错误。
            const DWORD error = GetLastError();
            impl_->reset();
            return Failure(error, "Target has not opted into cooperative debugging or is already controlled");
        }
        // serverPid：管道服务端真实PID。
        ULONG serverPid = 0;
        // mode：消息管道读取模式。
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!GetNamedPipeServerProcessId(impl_->pipe, &serverPid) || serverPid != pid ||
            CreationTime(impl_->target) != creationTime100ns ||
            !SetNamedPipeHandleState(impl_->pipe, &mode, nullptr, nullptr))
        {
            impl_->reset();
            return Failure(ERROR_ACCESS_DENIED, "Cooperative endpoint identity rejected");
        }
        // result：本次返回结果。
        Result result = exchange(Operation::Open);
        if (!result.ok())
            impl_->reset();
        return result;
    }

    // 发送一个有界请求，严格核验完整应答并传播实际错误。
    Result Session::exchange(Operation operation, std::uint64_t stopId)
    {
        std::unique_lock<std::recursive_mutex> lock(impl_->mutex);
        // 先解锁再检查取消，覆盖 close() 在最终应答检查后才到达的竞态。
        struct CancellationCleanup
        {
            Impl &state;
            std::unique_lock<std::recursive_mutex> &callLock;
            ~CancellationCleanup()
            {
                callLock.unlock();
                if (WaitForSingleObject(state.cancel, 0) == WAIT_OBJECT_0)
                {
                    std::lock_guard<std::recursive_mutex> cleanupLock(state.mutex);
                    if (WaitForSingleObject(state.cancel, 0) == WAIT_OBJECT_0)
                        state.reset();
                }
            }
        } cleanup{*impl_, lock};
        if (impl_->pipe == INVALID_HANDLE_VALUE)
            return Failure(ERROR_INVALID_HANDLE, "No cooperative session");
        if (impl_->cancellationGeneration.load() != impl_->connectionGeneration)
        {
            impl_->reset();
            return Failure(ERROR_OPERATION_ABORTED, "Cooperative session was closed");
        }
        // request：本次固定长度协议请求。
        Request request = MakeRequest();
        request.operation = operation;
        request.targetPid = impl_->pid;
        request.targetCreationTime100ns = impl_->creation;
        request.ownerPid = GetCurrentProcessId();
        request.ownerCreationTime100ns = CreationTime(GetCurrentProcess());
        request.tokenLow = impl_->tokenLow;
        request.tokenHigh = impl_->tokenHigh;
        request.stopId = stopId;
        request.requestId = ++impl_->requestId;
        // response：本次固定长度协议应答。
        Response response = MakeResponse();
        // error：本次操作系统错误。
        DWORD error = Transfer(impl_->pipe, impl_->cancel, &request, sizeof(request), true);
        if (error == ERROR_SUCCESS)
            error = Transfer(impl_->pipe, impl_->cancel, &response, sizeof(response), false);
        if (WaitForSingleObject(impl_->cancel, 0) == WAIT_OBJECT_0 ||
            impl_->cancellationGeneration.load() != impl_->connectionGeneration)
            error = ERROR_OPERATION_ABORTED;
        if (error == ERROR_SUCCESS &&
            (response.magic != ProtocolMagic || response.version != ProtocolVersion ||
             response.size != sizeof(response) || response.requestId != request.requestId ||
             response.targetPid != impl_->pid || response.targetCreationTime100ns != impl_->creation ||
             response.reserved[0] != 0 || response.reserved[1] != 0 || response.snapshot.reserved != 0 ||
             (response.state == State::Paused &&
              (response.stopId == 0 || response.snapshot.threadId == 0 || response.snapshot.rip == 0)) ||
             (response.state != State::Paused && response.stopId != 0) ||
             static_cast<std::uint32_t>(response.state) > static_cast<std::uint32_t>(State::Exited) ||
             (operation != Operation::Open &&
              (response.tokenLow != impl_->tokenLow || response.tokenHigh != impl_->tokenHigh)) ||
             (operation == Operation::Open && response.errorWin32 == 0 &&
              (response.tokenLow | response.tokenHigh) == 0)))
            error = ERROR_INVALID_DATA;
        if (error != ERROR_SUCCESS)
        {
            const State state = impl_->target && WaitForSingleObject(impl_->target, 0) == WAIT_OBJECT_0
                                    ? State::Exited
                                    : State::Unavailable;
            impl_->reset();
            return Failure(error, "Cooperative transport lost; target releases its pause automatically",
                           state);
        }
        impl_->tokenLow = response.tokenLow;
        impl_->tokenHigh = response.tokenHigh;
        // result：本次返回结果。
        Result result;
        result.state = response.state;
        result.errorWin32 = response.errorWin32;
        result.stopId = response.state == State::Paused ? response.stopId : 0;
        result.snapshot = response.state == State::Paused ? response.snapshot : RegisterSnapshot{};
        result.detail =
            response.errorWin32 ? "Cooperative command rejected" : "Cooperative command acknowledged";
        return result;
    }

    // 查询不消费停点，只返回当前状态。
    Result Session::query()
    {
        return exchange(Operation::Query);
    }
    // 请求下一检查点暂停，不挂起命令线程。
    Result Session::requestPause()
    {
        return exchange(Operation::RequestPause);
    }
    // 以当前 stopId 请求真实 TF 单步。
    Result Session::step(std::uint64_t stopId)
    {
        return exchange(Operation::Step, stopId);
    }
    // 以当前 stopId 放行业务线程。
    Result Session::resume(std::uint64_t stopId)
    {
        return exchange(Operation::Continue, stopId);
    }

    void Session::close()
    {
        // UI 关闭只通知取消并断连；不等待服务端 Close ACK 或租约。
        impl_->cancellationGeneration.fetch_add(1);
        if (impl_->cancel)
            SetEvent(impl_->cancel);
        std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
        if (lock.owns_lock())
            impl_->reset();
    }
}
