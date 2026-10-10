#include "../../Framework.h"
#include "./network_port_diagnostics.h"

#include <map>
#include <memory>

namespace
{
    // 每次诊断独立平衡 Winsock 引用计数，不依赖主窗口或其他网络页面的初始化顺序。
    ks::network::PortBindProbe probeBinding(std::uint16_t port, bool tcp, bool ipv6)
    {
        ks::network::PortBindProbe result{tcp, ipv6, 0};
        WSADATA data{}; // 仅持有本次 Winsock 初始化结果。
        result.error = WSAStartup(MAKEWORD(2, 2), &data);
        if (result.error != 0)
        {
            return result;
        }
        const SOCKET handle = socket(ipv6 ? AF_INET6 : AF_INET,
            tcp ? SOCK_STREAM : SOCK_DGRAM, tcp ? IPPROTO_TCP : IPPROTO_UDP);
        if (handle == INVALID_SOCKET)
        {
            result.error = WSAGetLastError();
            WSACleanup();
            return result;
        }
        // 独占模式防止检查复用/抢占现有 socket；IPv6 不重复覆盖 IPv4 检测结果。
        const int enabled = 1;
        if (setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR
            || (ipv6 && setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY,
                reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR))
        {
            result.error = WSAGetLastError();
        }
        else if (ipv6)
        {
            sockaddr_in6 address{}; // 通配 IPv6 地址只检查本机该协议端口。
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(port);
            if (bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
            {
                result.error = WSAGetLastError();
            }
        }
        else
        {
            sockaddr_in address{}; // 通配 IPv4 地址，不向其他主机发起连接。
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            if (bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
            {
                result.error = WSAGetLastError();
            }
        }
        closesocket(handle);
        WSACleanup();
        return result;
    }

    // 提前取得 PID 身份基线，再读连接表；枚举后的名称/路径必须属于同一进程实例。
    struct OwnerIdentity
    {
        std::uint64_t creation = 0; // 枚举端口之前的进程创建时间。
        std::string path;           // 同一实例的可执行路径。
    };

    std::map<std::uint32_t, OwnerIdentity> captureIdentities()
    {
        std::map<std::uint32_t, OwnerIdentity> result;
        const auto processes = ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);
        for (const auto& process : processes)
        {
            // 先保存创建时间，路径只对实际占用端口的 PID 查询。
            std::uint64_t creation = process.creationTime100ns;
            if (creation == 0)
            {
                ks::process::QueryProcessCreationTimeByPid(process.pid, &creation);
            }
            result.emplace(process.pid, OwnerIdentity{creation, {}});
        }
        return result;
    }

    // 只接受枚举前后仍匹配的实例；身份缺失保留展示，但不能执行结束进程动作。
    void fillOwnerIdentity(ks::network::PortOccupant& row,
        std::map<std::uint32_t, OwnerIdentity>& identities)
    {
        auto entry = identities.find(row.pid);
        std::uint64_t current = 0;
        if (entry == identities.end() || entry->second.creation == 0
            || !ks::process::QueryProcessCreationTimeByPid(row.pid, &current)
            || current != entry->second.creation)
        {
            return;
        }
        if (entry->second.path.empty())
        {
            entry->second.path = ks::process::QueryProcessPathByPid(row.pid);
        }
        row.creationTime100ns = current;
        row.imagePath = entry->second.path;
    }

    // 验证期间持有原进程查询句柄，操作工具再次使用创建时间校验，防止 PID 复用。
    struct ProcessHandleCloser
    {
        void operator()(void* handle) const
        {
            if (handle != nullptr)
            {
                CloseHandle(handle);
            }
        }
    };
}

ks::network::PortDiagnosticSnapshot ks::network::DiagnoseLocalPort(
    std::uint16_t port, PortProtocol protocol)
{
    PortDiagnosticSnapshot result;
    result.port = port;
    result.protocol = protocol;
    if (port == 0)
    {
        result.errors.emplace_back("端口必须在 1 到 65535 之间。");
        return result;
    }
    auto identities = captureIdentities(); // 同一轮 TCP/UDP 共用进程身份基线。
    if (protocol != PortProtocol::Udp)
    {
        std::vector<TcpConnectionRecord> records;
        std::string error;
        EnumerateTcpConnectionRecords(records, &error);
        if (!error.empty())
        {
            result.errors.push_back(error);
        }
        for (const auto& record : records)
        {
            if (record.localPort != port)
            {
                continue;
            }
            PortOccupant row;
            row.pid = record.processId;
            row.ipv6 = record.isIpv6;
            row.processName = record.processName;
            row.localAddress = record.localAddressText;
            row.state = record.tcpStateText;
            row.connection = record;
            fillOwnerIdentity(row, identities);
            result.occupants.push_back(std::move(row));
        }
        result.probes.push_back(probeBinding(port, true, false));
        result.probes.push_back(probeBinding(port, true, true));
    }
    if (protocol != PortProtocol::Tcp)
    {
        std::vector<UdpEndpointRecord> records;
        std::string error;
        EnumerateUdpEndpointRecords(records, &error);
        if (!error.empty())
        {
            result.errors.push_back(error);
        }
        for (const auto& record : records)
        {
            if (record.localPort != port)
            {
                continue;
            }
            PortOccupant row;
            row.tcp = false;
            row.pid = record.processId;
            row.ipv6 = record.isIpv6;
            row.processName = record.processName;
            row.localAddress = record.localAddressText;
            fillOwnerIdentity(row, identities);
            result.occupants.push_back(std::move(row));
        }
        result.probes.push_back(probeBinding(port, false, false));
        result.probes.push_back(probeBinding(port, false, true));
    }
    return result;
}

ks::network::PortReleaseResult ks::network::ReleaseLocalPort(const PortOccupant& occupant,
    std::uint16_t port, PortProtocol protocol, PortReleaseAction action,
    const std::function<bool()>& canceled)
{
    PortReleaseResult result;
    // 系统、当前程序与缺失身份的行不提供强制清理，不能对 TIME_WAIT 的 PID 0 结束进程。
    if (occupant.pid <= 4 || occupant.pid == GetCurrentProcessId() || occupant.creationTime100ns == 0)
    {
        result.error = "系统进程、当前程序或身份不完整的记录不能解除占用。";
        result.after = DiagnoseLocalPort(port, protocol);
        return result;
    }
    std::unique_ptr<void, ProcessHandleCloser> process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, occupant.pid));
    FILETIME created{}, exited{}, kernel{}, user{}; // 当前句柄固定的进程身份与状态。
    if (!process || !GetProcessTimes(process.get(), &created, &exited, &kernel, &user)
        || ks::str::FileTimeToUint64(created.dwHighDateTime, created.dwLowDateTime) != occupant.creationTime100ns
        || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
    {
        result.error = "进程已退出、身份已变化或无法校验，请重新诊断。";
    }
    else
    {
        // 动作前再读端口表，只处理仍持有同协议/地址族/本地地址的原进程。
        const auto current = DiagnoseLocalPort(port, occupant.tcp ? PortProtocol::Tcp : PortProtocol::Udp);
        const bool stillOwns = std::any_of(current.occupants.begin(), current.occupants.end(),
            [&occupant](const PortOccupant& row)
            {
                return row.pid == occupant.pid && row.creationTime100ns == occupant.creationTime100ns
                    && row.tcp == occupant.tcp && row.ipv6 == occupant.ipv6
                    && row.localAddress == occupant.localAddress;
            });
        if (canceled && canceled())
        {
            result.error = "操作已取消。";
        }
        // 另一地址族失败只影响空闲结论；此处已取得的同实例占用行仍是有效操作证据。
        else if (!stillOwns)
        {
            result.error = "原占用记录已变化或无法完整复核，未执行解除。";
        }
        else if (action == PortReleaseAction::DisconnectTcp)
        {
            result.performed = occupant.tcp && TerminateTcpConnectionByRecord(occupant.connection, &result.error);
        }
        else
        {
            // 不允许通过端口诊断结束关键进程；查询失败也保守拒绝。
            using CriticalQuery = BOOL(WINAPI*)(HANDLE, PBOOL);
            const auto query = reinterpret_cast<CriticalQuery>(GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "IsProcessCritical"));
            BOOL critical = TRUE;
            if (query == nullptr || !query(process.get(), &critical) || critical)
            {
                result.error = "目标是关键进程或关键状态不可确认，未结束进程。";
            }
            else if (canceled && canceled())
            {
                result.error = "操作已取消。";
            }
            else
            {
                result.performed = ks::process::TerminateProcessByWin32IfCreationTimeMatches(
                    occupant.pid, occupant.creationTime100ns, &result.error);
                if (result.performed && WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0)
                {
                    result.error = "已提交结束请求，但进程尚未退出；端口可能仍被占用。";
                }
            }
        }
    }
    result.after = DiagnoseLocalPort(port, protocol); // 不能把 API 成功当成释放成功。
    return result;
}
