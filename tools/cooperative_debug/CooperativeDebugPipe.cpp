#include "CooperativeDebugRuntimeInternal.h"
#include <bcrypt.h>
#include <cwchar>
#include <sddl.h>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace ksword::cooperative::runtime::detail
{
    namespace
    {
        // 查询用户 SID；不把令牌、SID 或会话密钥写入日志。
        bool User(HANDLE process, std::vector<BYTE> &bytes)
        {
            // token：令牌查询句柄或随机会话值。
            HANDLE token = nullptr;
            if (!OpenProcessToken(process, TOKEN_QUERY, &token))
                return false;
            // size：固定消息字节数。
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            bytes.resize(size);
            const BOOL ok = size != 0 && GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
            CloseHandle(token);
            return ok != FALSE;
        }

        // 核验管道对端真实进程与目标使用相同的用户 SID。
        bool SameUser(HANDLE process)
        {
            std::vector<BYTE> targetUser, peerUser;
            return User(GetCurrentProcess(), targetUser) && User(process, peerUser) &&
                   EqualSid(reinterpret_cast<TOKEN_USER *>(targetUser.data())->User.Sid,
                            reinterpret_cast<TOKEN_USER *>(peerUser.data())->User.Sid) != FALSE;
        }

        // 在短锁内检查控制进程对象和截止时间，不依赖 PID 字符串。
        bool ControllerAlive()
        {
            // state：当前状态或其短锁保护引用。
            Runtime &state = instance;
            AcquireSRWLockShared(&state.lock);
            // alive：控制者存活及租期判定。
            const bool alive = state.connected && state.owner &&
                               WaitForSingleObject(state.owner, 0) == WAIT_TIMEOUT &&
                               GetTickCount64() < state.deadline;
            ReleaseSRWLockShared(&state.lock);
            return alive;
        }

        // 每次 I/O 都可被 Stop、owner 死亡或租约到期打断；取消后收割完成状态。
        DWORD Finish(OVERLAPPED &pending, DWORD &bytes, DWORD timeout, bool authenticated)
        {
            // started：有界等待起始时刻。
            const ULONGLONG started = GetTickCount64();
            // events：取消及完成事件等待组。
            HANDLE events[2]{instance.stop, pending.hEvent};
            // error：本次操作系统错误。
            DWORD error = ERROR_SUCCESS;
            for (;;)
            {
                // waited：事件等待结果。
                const DWORD waited = WaitForMultipleObjects(2, events, FALSE, 50);
                if (waited == WAIT_OBJECT_0 + 1)
                {
                    if (!GetOverlappedResult(instance.pipe, &pending, &bytes, FALSE))
                        error = GetLastError();
                    return error;
                }
                if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED ||
                    GetTickCount64() - started >= timeout || (authenticated && !ControllerAlive()))
                {
                    error = waited == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT;
                    CancelIoEx(instance.pipe, &pending);
                    GetOverlappedResult(instance.pipe, &pending, &bytes, TRUE);
                    return error;
                }
            }
        }

        // 收发一条精确长度的消息，拒绝截断和超长消息。
        DWORD Transfer(void *buffer, DWORD size, bool writing, bool authenticated)
        {
            // pending：本次异步管道I/O记录。
            OVERLAPPED pending{};
            pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!pending.hEvent)
                return GetLastError();
            // bytes：本次实际传输字节数。
            DWORD bytes = 0;
            // completed：I/O是否同步完成。
            const BOOL completed = writing ? WriteFile(instance.pipe, buffer, size, &bytes, &pending)
                                           : ReadFile(instance.pipe, buffer, size, &bytes, &pending);
            // error：本次操作系统错误。
            DWORD error = completed ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_IO_PENDING)
                error = Finish(pending, bytes, authenticated ? instance.leaseMs : 2000, authenticated);
            CloseHandle(pending.hEvent);
            if (error == ERROR_SUCCESS && bytes != size)
                error = ERROR_INVALID_DATA;
            return error;
        }

        // 等待一个本地控制连接，Stop 事件始终能取消等待。
        DWORD Connect()
        {
            // pending：本次异步管道I/O记录。
            OVERLAPPED pending{};
            pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!pending.hEvent)
                return GetLastError();
            // completed：I/O是否同步完成。
            const BOOL completed = ConnectNamedPipe(instance.pipe, &pending);
            // error：本次操作系统错误。
            DWORD error = completed ? ERROR_SUCCESS : GetLastError();
            // bytes：本次实际传输字节数。
            DWORD bytes = 0;
            if (error == ERROR_PIPE_CONNECTED)
                error = ERROR_SUCCESS;
            else if (error == ERROR_IO_PENDING)
                error = Finish(pending, bytes, MAXDWORD, false);
            CloseHandle(pending.hEvent);
            return error;
        }

        // 初次握手验证真实对端身份并生成本次会话的随机令牌。
        DWORD OpenController(const Request &request)
        {
            // peerPid：管道客户端真实进程标识。
            ULONG peerPid = 0;
            if (request.tokenLow != 0 || request.tokenHigh != 0 || request.stopId != 0 ||
                request.requestId != 1 || !GetNamedPipeClientProcessId(instance.pipe, &peerPid) ||
                peerPid != request.ownerPid || peerPid == instance.pid)
                return ERROR_ACCESS_DENIED;
            // peer：管道控制者真实进程句柄。
            HANDLE peer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, peerPid);
            if (!peer)
                return GetLastError();
            if (CreationTime(peer) != request.ownerCreationTime100ns ||
                WaitForSingleObject(peer, 0) != WAIT_TIMEOUT || !SameUser(peer))
            {
                CloseHandle(peer);
                return ERROR_ACCESS_DENIED;
            }
            // token：令牌查询句柄或随机会话值。
            std::uint64_t token[2]{};
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(token), sizeof(token),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
                (token[0] | token[1]) == 0)
            {
                CloseHandle(peer);
                return ERROR_GEN_FAILURE;
            }
            AcquireSRWLockExclusive(&instance.lock);
            // 旧会话尚有 TF 或暂停回调在途时，不能让新 owner 覆盖线程所有权。
            if (instance.armedThread && WaitForSingleObject(instance.armedThread, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(instance.armedThread);
                instance.armedThread = nullptr;
                instance.armedTid = 0;
            }
            if (instance.armedTid != 0 || instance.pausedTid != 0)
            {
                ReleaseSRWLockExclusive(&instance.lock);
                CloseHandle(peer);
                return ERROR_BUSY;
            }
            instance.owner = peer;
            instance.connected = true;
            instance.tokenLow = token[0];
            instance.tokenHigh = token[1];
            instance.deadline = GetTickCount64() + instance.leaseMs;
            instance.lastRequestId = request.requestId;
            instance.lastError = 0;
            instance.command = Command::None;
            ReleaseSRWLockExclusive(&instance.lock);
            return ERROR_SUCCESS;
        }

        // 当前请求只能消费当前会话的当前停点；查询不会伪造暂停。
        DWORD Execute(const Request &request, bool &close)
        {
            // state：当前状态或其短锁保护引用。
            Runtime &state = instance;
            AcquireSRWLockExclusive(&state.lock);
            // error：本次操作系统错误。
            DWORD error = ERROR_SUCCESS;
            if (!state.connected || !state.owner || request.ownerPid != GetProcessId(state.owner) ||
                request.ownerCreationTime100ns != CreationTime(state.owner) ||
                request.tokenLow != state.tokenLow || request.tokenHigh != state.tokenHigh ||
                request.requestId != state.lastRequestId + 1 || GetTickCount64() >= state.deadline ||
                WaitForSingleObject(state.owner, 0) != WAIT_TIMEOUT)
                error = ERROR_ACCESS_DENIED;
            else
            {
                state.lastRequestId = request.requestId;
                state.deadline = GetTickCount64() + state.leaseMs;
                switch (request.operation)
                {
                case Operation::Query:
                    error = request.stopId == 0 ? state.lastError : ERROR_INVALID_PARAMETER;
                    break;
                case Operation::RequestPause:
                    if (request.stopId != 0)
                        error = ERROR_INVALID_PARAMETER;
                    else if (state.state != State::Paused)
                    {
                        state.lastError = 0;
                        state.pauseRequested = true;
                        state.state = State::PauseRequested;
                    }
                    break;
                case Operation::Step:
                case Operation::Continue:
                    if (state.state != State::Paused || state.stopId == 0 || request.stopId != state.stopId ||
                        state.command != Command::None)
                        error = ERROR_INVALID_STATE;
                    else
                    {
                        state.command =
                            request.operation == Operation::Step ? Command::Step : Command::Continue;
                        state.state = State::Running;
                        state.stopId = 0;
                        state.snapshot = {};
                        SetEvent(state.wake);
                    }
                    break;
                case Operation::Close:
                    if (request.stopId != 0)
                        error = ERROR_INVALID_PARAMETER;
                    else
                        close = true;
                    break;
                default:
                    error = ERROR_NOT_SUPPORTED;
                    break;
                }
            }
            ReleaseSRWLockExclusive(&state.lock);
            return error;
        }

        // 只将当前暂停投影为应答；身份拒绝时清空令牌和现场。
        Response Reply(const Request &request, DWORD error, bool authenticated)
        {
            // response：本次固定长度协议应答。
            Response response = MakeResponse();
            response.errorWin32 = error;
            response.targetPid = instance.pid;
            response.targetCreationTime100ns = instance.creation;
            response.requestId = request.requestId;
            // 身份或协议拒绝时不回传当前令牌和寄存器现场。
            if (!authenticated || error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_DATA)
                return response;
            AcquireSRWLockShared(&instance.lock);
            response.state = instance.state;
            response.tokenLow = instance.tokenLow;
            response.tokenHigh = instance.tokenHigh;
            if (response.state == State::Paused)
            {
                response.stopId = instance.stopId;
                response.snapshot = instance.snapshot;
            }
            ReleaseSRWLockShared(&instance.lock);
            return response;
        }
    }

    // 创建仅当前用户可用的本地首实例端点，拒绝远程连接和名称抢占。
    DWORD CreateEndpoint()
    {
        // user：用户用时或用户SID缓冲。
        std::vector<BYTE> user;
        if (!User(GetCurrentProcess(), user))
            return GetLastError();
        // sid：当前用户SID字符串。
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid))
            return GetLastError();
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        // descriptor：显式用户ACL安全描述符。
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                  nullptr))
            return GetLastError();
        // security：创建端点使用的安全属性。
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        wchar_t endpoint[160]{};
        swprintf_s(endpoint, L"\\\\.\\pipe\\KSword.CooperativeDebug.v1.%lu.%llx", instance.pid,
                   instance.creation);
        instance.pipe = CreateNamedPipeW(
            endpoint, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
            sizeof(Response), sizeof(Request), 0, &security);
        // error：本次操作系统错误。
        const DWORD error = instance.pipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        LocalFree(descriptor);
        return error;
    }

    // 独立工作线程处理命令；断连、死亡、超时均退休控制会话。
    DWORD WINAPI Worker(void *)
    {
        while (WaitForSingleObject(instance.stop, 0) == WAIT_TIMEOUT)
        {
            if (Connect() != ERROR_SUCCESS)
                break;
            // authenticated：当前连接是否完成身份认证。
            bool authenticated = false;
            // close：本轮应答后是否结束连接。
            bool close = false;
            while (!close && WaitForSingleObject(instance.stop, 0) == WAIT_TIMEOUT)
            {
                // request：本次固定长度协议请求。
                Request request = MakeRequest();
                if (Transfer(&request, sizeof(request), false, authenticated) != ERROR_SUCCESS)
                    break;
                // error：本次操作系统错误。
                DWORD error = ERROR_SUCCESS;
                if (request.magic != ProtocolMagic || request.version != ProtocolVersion ||
                    request.size != sizeof(request) || request.targetPid != instance.pid ||
                    request.targetCreationTime100ns != instance.creation || request.reserved[0] != 0 ||
                    request.reserved[1] != 0)
                    error = ERROR_INVALID_DATA;
                else if (!authenticated)
                {
                    error =
                        request.operation == Operation::Open ? OpenController(request) : ERROR_ACCESS_DENIED;
                    authenticated = error == ERROR_SUCCESS;
                }
                else
                    error = Execute(request, close);
                // response：本次固定长度协议应答。
                Response response = Reply(request, error, authenticated);
                if (Transfer(&response, sizeof(response), true, authenticated) != ERROR_SUCCESS)
                    break;
                if (!authenticated || error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_DATA)
                    break;
            }
            ReleaseController();
            DisconnectNamedPipe(instance.pipe);
        }
        ReleaseController();
        return 0;
    }
}
