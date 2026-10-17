#pragma once

#include "CooperativeDebugProtocol.h"
#include <memory>
#include <string>

namespace ksword::cooperative
{
    // Qt-free 结果：成功状态和系统错误分开，调用方只在 Paused 时显示现场。
    struct Result
    {
        State state = State::Unavailable;
        // snapshot：当前暂停的整数现场。
        RegisterSnapshot snapshot{};
        // stopId：当前可消费停点。
        std::uint64_t stopId = 0;
        // errorWin32：明确的 Win32 错误码。
        std::uint32_t errorWin32 = 0;
        std::string detail;
        bool ok() const noexcept
        {
            return errorWin32 == 0 && state != State::Unavailable && state != State::Exited;
        }
    };

    // 每个对象独占一个控制连接；关闭、断连或租约过期会放行目标线程。
    class Session final
    {
      public:
        // 创建独立控制对象；不会自动寻找或附加目标。
        Session();
        // 析构前调用方必须保证后台调用生命周期仍然有效。
        ~Session();
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
        // 建立精确进程身份的连接并认证，失败返回非零系统错误。
        Result open(std::uint32_t pid, std::uint64_t creationTime100ns);
        // 即时查询当前状态，只有 Paused 才提供可用现场。
        Result query();
        // 请求目标在下一次主动检查点暂停。
        Result requestPause();
        // 消费当前停点，真实执行一条机器指令。
        Result step(std::uint64_t stopId);
        // 消费当前停点，恢复业务执行。
        Result resume(std::uint64_t stopId);
        // 本地取消并断连，不等待服务端 ACK。
        void close();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        Result exchange(Operation operation, std::uint64_t stopId = 0);
    };

    // 端点同时包含 PID 与创建时间，禁止用复用后的 PID 连接另一目标。
    std::wstring EndpointName(std::uint32_t pid, std::uint64_t creationTime100ns);
}
