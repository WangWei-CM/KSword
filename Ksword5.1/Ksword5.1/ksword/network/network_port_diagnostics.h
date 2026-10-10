#pragma once

#include "./network_connection_tools.h"
#include <functional>

namespace ks::network
{
    // 诊断的协议范围；只查询本地端口，不把远端同号端口当作本机占用。
    enum class PortProtocol { Both, Tcp, Udp };
    enum class PortReleaseAction { TerminateOwner, DisconnectTcp };

    // 一条占用证据与原始 TCP 四元组；创建时间用于动作前防止 PID 复用。
    struct PortOccupant
    {
        bool tcp = true;                     // 是否来自 TCP 连接表。
        bool ipv6 = false;                   // 地址族，IPv6 与 IPv4 独立呈现。
        std::uint32_t pid = 0;               // 快照中报告的占用进程。
        std::uint64_t creationTime100ns = 0;  // 已核对的进程实例身份，缺失时禁用解除。
        std::string processName;             // 进程名，用于确认目标。
        std::string imagePath;               // 映像路径，用于说明目标。
        std::string localAddress;            // 本地地址，不含端口。
        std::string state;                   // TCP 状态，UDP 使用无连接状态。
        TcpConnectionRecord connection;     // TCP 断开保留枚举时的原始参数。
    };

    // 独占绑定探测不监听、不收发数据，检查后立即关闭临时 socket。
    struct PortBindProbe
    {
        bool tcp = true;                     // 本次检查的协议。
        bool ipv6 = false;                   // 本次检查的地址族；IPv6 只检查 v6。
        int error = 0;                       // Winsock 原始错误码，0 表示可绑定。
    };

    struct PortDiagnosticSnapshot
    {
        std::uint16_t port = 0;              // 本次诊断的端口，不随 UI 编辑变化。
        PortProtocol protocol = PortProtocol::Both; // 本次协议范围。
        std::vector<PortOccupant> occupants; // 所有本地同号端口记录。
        std::vector<PortBindProbe> probes;   // 所选协议的 IPv4/IPv6 独占绑定结果。
        std::vector<std::string> errors;     // 枚举失败/部分失败，不把缺失证据当空闲。
    };

    struct PortReleaseResult
    {
        bool performed = false;             // 动作是否成功提交，不能据此推断端口已释放。
        std::string error;                  // 失败原因或等待退出超时的说明。
        PortDiagnosticSnapshot after;       // 操作后重新查询占用与实际绑定能力。
    };

    // 后台线程调用：输入本地端口与协议，返回占用快照和瞬时绑定结果，不修改目标进程。
    PortDiagnosticSnapshot DiagnoseLocalPort(std::uint16_t port, PortProtocol protocol);
    // 用户确认后后台调用：只操作仍匹配快照身份且仍占用该端口的目标，随后重新诊断。
    PortReleaseResult ReleaseLocalPort(const PortOccupant& occupant,
        std::uint16_t port, PortProtocol protocol, PortReleaseAction action,
        const std::function<bool()>& canceled = {});
}
