#pragma once

#include <cstdint>
#include <type_traits>

// 协作调试协议只服务主动启用组件的 x64 目标；不创建 Windows 调试对象。
namespace ksword::cooperative
{
    // ProtocolVersion：当前协议版本常量。
    constexpr std::uint32_t ProtocolVersion = 1;
    // ProtocolMagic：协议标识常量。
    constexpr std::uint32_t ProtocolMagic = 0x4344534b;
    // DefaultLeaseMs：默认控制租期毫秒数。
    constexpr std::uint32_t DefaultLeaseMs = 10000;

    // 状态只描述当前会话；历史寄存器不能充当新的暂停现场。
    enum class State : std::uint32_t
    {
        Unavailable = 0,
        Running = 1,
        PauseRequested = 2,
        Paused = 3,
        Exited = 4
    };

    // 操作采用固定长度消息；Step 和 Continue 必须带当前 stopId。
    enum class Operation : std::uint32_t
    {
        Open = 1,
        Query = 2,
        RequestPause = 3,
        Step = 4,
        Continue = 5,
        Close = 6
    };

    // 快照只承诺整数寄存器，不声称支持 XSTATE 或寄存器编辑。
    struct RegisterSnapshot
    {
        // threadId：当前真实停点所属线程。
        std::uint32_t threadId;
        // reserved：保留字段必须为零。
        std::uint32_t reserved;
        // rip：下一条实际待执行指令地址。
        std::uint64_t rip;
        // rsp：当前用户栈指针。
        std::uint64_t rsp;
        // rflags：移除组件自有 TF 后的标志快照。
        std::uint64_t rflags;
        // rax：整数寄存器 RAX。
        std::uint64_t rax;
        // rbx：整数寄存器 RBX。
        std::uint64_t rbx;
        // rcx：整数寄存器 RCX。
        std::uint64_t rcx;
        // rdx：整数寄存器 RDX。
        std::uint64_t rdx;
        // rbp：整数寄存器 RBP。
        std::uint64_t rbp;
        // rsi：整数寄存器 RSI。
        std::uint64_t rsi;
        // rdi：整数寄存器 RDI。
        std::uint64_t rdi;
        // r8：整数寄存器 R8。
        std::uint64_t r8;
        // r9：整数寄存器 R9。
        std::uint64_t r9;
        // r10：整数寄存器 R10。
        std::uint64_t r10;
        // r11：整数寄存器 R11。
        std::uint64_t r11;
        // r12：整数寄存器 R12。
        std::uint64_t r12;
        // r13：整数寄存器 R13。
        std::uint64_t r13;
        // r14：整数寄存器 R14。
        std::uint64_t r14;
        // r15：整数寄存器 R15。
        std::uint64_t r15;
    };

    struct Request
    {
        // magic：协议魔数。
        std::uint32_t magic;
        // version：协议版本。
        std::uint32_t version;
        // size：固定消息字节数。
        std::uint32_t size;
        Operation operation;
        // 目标与控制者均使用 PID 和创建时间配对，避免标识复用。
        // targetPid：目标进程标识。
        std::uint32_t targetPid;
        // ownerPid：控制进程标识。
        std::uint32_t ownerPid;
        // targetCreationTime100ns：目标创建时间。
        std::uint64_t targetCreationTime100ns;
        // ownerCreationTime100ns：控制者创建时间。
        std::uint64_t ownerCreationTime100ns;
        // 随机会话令牌只在本地管道传递，不记录到日志。
        // tokenLow：随机会话令牌低位。
        std::uint64_t tokenLow;
        // tokenHigh：随机会话令牌高位。
        std::uint64_t tokenHigh;
        // stopId：只有 Step/Continue 可以消费当前非零停点。
        std::uint64_t stopId;
        // requestId：会话内严格递增的请求编号。
        std::uint64_t requestId;
        // reserved：保留字段，必须清零。
        std::uint64_t reserved[2];
    };

    struct Response
    {
        // magic：协议魔数。
        std::uint32_t magic;
        // version：协议版本。
        std::uint32_t version;
        // size：固定消息字节数。
        std::uint32_t size;
        // errorWin32：明确的 Win32 错误码。
        std::uint32_t errorWin32;
        State state;
        // targetPid：目标进程标识。
        std::uint32_t targetPid;
        // targetCreationTime100ns：目标创建时间。
        std::uint64_t targetCreationTime100ns;
        // 令牌回显只用于匹配当前控制会话。
        // tokenLow：随机会话令牌低位。
        std::uint64_t tokenLow;
        // tokenHigh：随机会话令牌高位。
        std::uint64_t tokenHigh;
        // stopId：当前可消费停点。
        std::uint64_t stopId;
        // requestId：当前请求序号。
        std::uint64_t requestId;
        // snapshot：当前暂停的整数现场。
        RegisterSnapshot snapshot;
        // reserved：保留字段，必须清零。
        std::uint64_t reserved[2];
    };

    // 工厂只负责清零并填协议头；通信结构本身保持严格 POD。
    inline Request MakeRequest()
    {
        // request：本次固定长度协议请求。
        Request request{};
        request.magic = ProtocolMagic;
        request.version = ProtocolVersion;
        request.size = sizeof(Request);
        request.operation = Operation::Query;
        return request;
    }

    inline Response MakeResponse()
    {
        // response：本次固定长度协议应答。
        Response response{};
        response.magic = ProtocolMagic;
        response.version = ProtocolVersion;
        response.size = sizeof(Response);
        return response;
    }

    static_assert(std::is_trivial_v<Request> && std::is_trivial_v<Response>);
    static_assert(std::is_trivial_v<RegisterSnapshot>);
    static_assert(std::is_trivially_copyable_v<Request> && std::is_standard_layout_v<Request>);
    static_assert(std::is_trivially_copyable_v<Response> && std::is_standard_layout_v<Response>);
    static_assert(sizeof(Request) == 88);
    static_assert(sizeof(RegisterSnapshot) == 152);
    static_assert(sizeof(Response) == 232);
}
