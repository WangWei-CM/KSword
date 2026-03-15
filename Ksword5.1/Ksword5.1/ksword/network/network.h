#pragma once

// ============================================================
// ksword/network/network.h
// 命名空间：ks::network
// 作用：
// - 提供“发送方向 TCP/UDP 报文抓取”的基础能力（IPv4 Raw Socket）；
// - 提供“报文端口 -> 进程 PID”解析能力（IP Helper 表）；
// - 提供“按 PID 软限速（Suspend/Resume）”的运行时控制能力；
// - 提供“TCP/UDP 连接快照枚举 + TCP 连接终止”的工具能力；
// - 提供“手动构造网络请求（TCP/UDP Winsock 参数可定制）”执行能力；
// - 供 NetworkDock UI 直接调用，保持 UI 与底层抓包逻辑解耦。
// ============================================================

#include "../process/process.h"

#include <algorithm>    // std::clamp/std::sort：限速参数规整与快照排序。
#include <atomic>       // std::atomic：线程运行标记与序号生成。
#include <chrono>       // steady_clock/system_clock：时间戳与刷新节流。
#include <cstdint>      // 固定宽度整数：PID/端口/长度等。
#include <cstring>      // std::memcpy：网络字节解析。
#include <functional>   // std::function：回调接口。
#include <mutex>        // std::mutex：跨线程共享状态保护。
#include <optional>     // std::optional：可选动作事件。
#include <sstream>      // std::ostringstream：状态文本组装。
#include <string>       // std::string：跨层文本类型。
#include <thread>       // std::thread：后台抓包线程。
#include <unordered_map>// std::unordered_map：连接映射与缓存。
#include <unordered_set>// std::unordered_set：本机地址快速匹配。
#include <utility>      // std::pair：容器辅助。
#include <vector>       // std::vector：报文/表项容器。

// 连接管理工具：
// - TCP/UDP 连接快照枚举；
// - TCP 连接终止（DELETE_TCB）。
#include "network_connection_tools.h"

// 手动请求工具：
// - 提供可配置的 Winsock 请求执行封装。
#include "network_request_tools.h"

// Win32 + Winsock 头文件。
// 注意：winsock2 需要先于 windows.h 链路中的 winsock.h。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Iphlpapi.h>
#include <Mstcpip.h>

// 链接依赖：
// - Ws2_32：socket/bind/select/recv/WsaStartup；
// - Iphlpapi：GetAdaptersAddresses/GetExtendedTcpTable/GetExtendedUdpTable。
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

namespace ks::network
{
    // PacketTransportProtocol：当前仅关注 TCP/UDP（发送方向）。
    enum class PacketTransportProtocol : std::uint8_t
    {
        Tcp = 6,   // IP 协议号 6。
        Udp = 17   // IP 协议号 17。
    };

    // PacketDirection：报文方向枚举。
    enum class PacketDirection : std::uint8_t
    {
        Outbound = 0, // 出站（本机 -> 远端）。
        Inbound = 1,  // 入站（远端 -> 本机）。
        Unknown = 2   // 方向无法判定。
    };

    // RateLimitActionType：限速动作类型。
    enum class RateLimitActionType : std::uint8_t
    {
        SuspendProcess = 0, // 触发限速：挂起进程。
        ResumeProcess = 1   // 挂起窗口到期：恢复进程。
    };

    // PacketRecord：单个抓包事件在 UI 层展示所需的完整数据。
    struct PacketRecord
    {
        std::uint64_t sequenceId = 0;         // 报文序号（单调递增，UI 作为主键）。
        std::uint64_t captureTimestampMs = 0; // 抓包时间戳（Unix ms）。

        PacketTransportProtocol protocol = PacketTransportProtocol::Tcp; // 协议类型（TCP/UDP）。
        PacketDirection direction = PacketDirection::Unknown;            // 方向（当前关注 Outbound）。

        std::uint32_t processId = 0;          // 归属进程 PID（0 表示暂未解析）。
        std::string processName;              // 归属进程名（缓存查询，可能为空）。

        std::string localAddress;             // 本地地址（IPv4 文本）。
        std::uint16_t localPort = 0;          // 本地端口。
        std::string remoteAddress;            // 远端地址（IPv4 文本）。
        std::uint16_t remotePort = 0;         // 远端端口。

        std::uint32_t totalPacketSize = 0;    // IP 报文总长度（字节）。
        std::uint32_t payloadSize = 0;        // L4 负载长度（字节）。
        std::size_t payloadOffset = 0;        // 在 packetBytes 中 payload 起始偏移。

        bool packetBytesTruncated = false;    // true 表示 packetBytes 被截断保存。
        std::vector<std::uint8_t> packetBytes;// 保存的原始报文字节（用于详情窗口查看）。
    };

    // ProcessRateLimitRule：单个 PID 的限速策略配置。
    struct ProcessRateLimitRule
    {
        std::uint32_t processId = 0;          // 目标 PID。
        std::uint64_t bytesPerSecond = 0;     // 每秒允许发送的字节上限（B/s）。
        std::uint32_t suspendDurationMs = 250;// 超限后挂起时长（毫秒）。
        bool enabled = true;                  // 是否启用该规则。
    };

    // ProcessRateLimitSnapshot：UI 刷新限速表时使用的快照结构。
    struct ProcessRateLimitSnapshot
    {
        ProcessRateLimitRule rule;            // 规则配置副本。
        std::uint64_t currentWindowBytes = 0; // 当前统计窗口内累计发送字节。
        std::uint64_t triggerCount = 0;       // 触发限速次数。
        bool currentlySuspended = false;      // 当前是否处于“本组件触发的挂起”状态。
    };

    // RateLimitActionEvent：限速动作通知（挂起/恢复成功与否）。
    struct RateLimitActionEvent
    {
        std::uint64_t timestampMs = 0;            // 动作发生时间戳（Unix ms）。
        std::uint32_t processId = 0;              // 目标 PID。
        RateLimitActionType actionType = RateLimitActionType::SuspendProcess; // 动作类型。
        bool actionSucceeded = false;             // 动作是否执行成功。
        std::uint64_t configuredBytesPerSecond = 0;// 当时规则阈值（B/s）。
        std::string detailText;                   // 详细说明文本（可用于 UI 日志面板）。
    };

    // PacketProtocolToString：协议枚举转字符串。
    inline std::string PacketProtocolToString(const PacketTransportProtocol protocol)
    {
        switch (protocol)
        {
        case PacketTransportProtocol::Tcp:
            return "TCP";
        case PacketTransportProtocol::Udp:
            return "UDP";
        default:
            return "UNKNOWN";
        }
    }

    // PacketDirectionToString：方向枚举转字符串。
    inline std::string PacketDirectionToString(const PacketDirection direction)
    {
        switch (direction)
        {
        case PacketDirection::Outbound:
            return "Outbound";
        case PacketDirection::Inbound:
            return "Inbound";
        case PacketDirection::Unknown:
        default:
            return "Unknown";
        }
    }

    // RateLimitActionTypeToString：限速动作枚举转字符串。
    inline std::string RateLimitActionTypeToString(const RateLimitActionType actionType)
    {
        switch (actionType)
        {
        case RateLimitActionType::SuspendProcess:
            return "SuspendProcess";
        case RateLimitActionType::ResumeProcess:
            return "ResumeProcess";
        default:
            return "UnknownAction";
        }
    }

    namespace detail
    {
        // kMaxRetainedPacketBytes：
        // - 防止单包完整保存导致内存膨胀；
        // - 仍保留足够报文头 + 主要 payload，满足“内容查看”功能。
        constexpr std::size_t kMaxRetainedPacketBytes = 4096;

        // NowTickMs：返回当前 Unix 毫秒时间戳。
        inline std::uint64_t NowTickMs()
        {
            const auto now = std::chrono::system_clock::now();
            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            return static_cast<std::uint64_t>(epochMs.count());
        }

        // MakeWinSockErrorText：把 WinSock 错误码组装成可读文本。
        inline std::string MakeWinSockErrorText(const int errorCode)
        {
            std::ostringstream stream;
            stream << "WSAError=" << errorCode;
            return stream.str();
        }

        // ReadNetworkUInt16：从网络字节序读取 16 位整数并转主机序。
        inline std::uint16_t ReadNetworkUInt16(const std::uint8_t* dataPtr)
        {
            std::uint16_t valueNetwork = 0;
            std::memcpy(&valueNetwork, dataPtr, sizeof(valueNetwork));
            return ntohs(valueNetwork);
        }

        // ReadNetworkUInt32：从网络字节序读取 32 位整数并转主机序。
        inline std::uint32_t ReadNetworkUInt32(const std::uint8_t* dataPtr)
        {
            std::uint32_t valueNetwork = 0;
            std::memcpy(&valueNetwork, dataPtr, sizeof(valueNetwork));
            return ntohl(valueNetwork);
        }

        // Ipv4HostToString：主机序 IPv4 转点分十进制文本。
        inline std::string Ipv4HostToString(const std::uint32_t hostOrderIpv4)
        {
            in_addr address{};
            address.s_addr = htonl(hostOrderIpv4);

            char textBuffer[INET_ADDRSTRLEN] = {};
            const PCSTR convertResult = ::inet_ntop(AF_INET, &address, textBuffer, static_cast<socklen_t>(sizeof(textBuffer)));
            if (convertResult == nullptr)
            {
                return std::string("0.0.0.0");
            }
            return std::string(textBuffer);
        }

        // LocalEndpointKey：本地地址+端口组合键（用于 UDP 与 TCP fallback）。
        inline std::uint64_t LocalEndpointKey(const std::uint32_t localIpv4HostOrder, const std::uint16_t localPortHostOrder)
        {
            return (static_cast<std::uint64_t>(localIpv4HostOrder) << 16) |
                static_cast<std::uint64_t>(localPortHostOrder);
        }

        // TcpEndpointKey：TCP 四元组键。
        struct TcpEndpointKey
        {
            std::uint32_t localIpv4 = 0;   // 本地 IPv4（主机序）。
            std::uint16_t localPort = 0;   // 本地端口（主机序）。
            std::uint32_t remoteIpv4 = 0;  // 远端 IPv4（主机序）。
            std::uint16_t remotePort = 0;  // 远端端口（主机序）。

            bool operator==(const TcpEndpointKey& right) const
            {
                return localIpv4 == right.localIpv4 &&
                    localPort == right.localPort &&
                    remoteIpv4 == right.remoteIpv4 &&
                    remotePort == right.remotePort;
            }
        };

        // TcpEndpointKeyHasher：TCP 四元组哈希器。
        struct TcpEndpointKeyHasher
        {
            std::size_t operator()(const TcpEndpointKey& key) const
            {
                const std::uint64_t leftPart =
                    (static_cast<std::uint64_t>(key.localIpv4) << 32) |
                    (static_cast<std::uint64_t>(key.localPort) << 16) |
                    static_cast<std::uint64_t>(key.remotePort);
                const std::uint64_t rightPart = static_cast<std::uint64_t>(key.remoteIpv4);
                return static_cast<std::size_t>(leftPart ^ (rightPart * 0x9E3779B97F4A7C15ULL));
            }
        };

        // CaptureSocketEntry：单个绑定接口的 Raw Socket 信息。
        struct CaptureSocketEntry
        {
            SOCKET socketValue = INVALID_SOCKET;    // Raw Socket 句柄。
            std::uint32_t localIpv4HostOrder = 0;   // 绑定地址（主机序）。
            std::string localIpv4Text;              // 绑定地址文本（日志展示）。
        };

        // EnumerateActiveIpv4Addresses：
        // - 枚举“当前启用”的 IPv4 单播地址；
        // - 供抓包 socket 按接口逐个 bind。
        inline std::vector<std::uint32_t> EnumerateActiveIpv4Addresses()
        {
            std::vector<std::uint32_t> ipv4List;

            ULONG requiredBufferLength = 0;
            const ULONG queryFlags =
                GAA_FLAG_SKIP_ANYCAST |
                GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER;

            const ULONG firstQueryResult = ::GetAdaptersAddresses(
                AF_INET,
                queryFlags,
                nullptr,
                nullptr,
                &requiredBufferLength);

            if (firstQueryResult != ERROR_BUFFER_OVERFLOW || requiredBufferLength == 0)
            {
                return ipv4List;
            }

            std::vector<std::uint8_t> buffer(requiredBufferLength, 0);
            auto* adapterList = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            const ULONG secondQueryResult = ::GetAdaptersAddresses(
                AF_INET,
                queryFlags,
                nullptr,
                adapterList,
                &requiredBufferLength);

            if (secondQueryResult != NO_ERROR)
            {
                return ipv4List;
            }

            std::unordered_set<std::uint32_t> uniqueAddressSet;
            for (IP_ADAPTER_ADDRESSES* adapter = adapterList; adapter != nullptr; adapter = adapter->Next)
            {
                // 只采集“已启动”网卡，避免无效 bind。
                if (adapter->OperStatus != IfOperStatusUp)
                {
                    continue;
                }

                for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
                    unicast != nullptr;
                    unicast = unicast->Next)
                {
                    if (unicast->Address.lpSockaddr == nullptr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET)
                    {
                        continue;
                    }

                    auto* ipv4Address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                    const std::uint32_t hostOrderAddress = ntohl(ipv4Address->sin_addr.s_addr);
                    if (hostOrderAddress == 0)
                    {
                        continue;
                    }
                    uniqueAddressSet.insert(hostOrderAddress);
                }
            }

            ipv4List.assign(uniqueAddressSet.begin(), uniqueAddressSet.end());
            return ipv4List;
        }

        // OpenCaptureSockets：
        // - 为每个活动 IPv4 地址创建 Raw Socket；
        // - 开启 SIO_RCVALL 抓取 IP 层报文。
        inline std::vector<CaptureSocketEntry> OpenCaptureSockets(std::string* errorTextOut)
        {
            if (errorTextOut != nullptr)
            {
                errorTextOut->clear();
            }

            std::vector<CaptureSocketEntry> socketList;
            const std::vector<std::uint32_t> localIpv4List = EnumerateActiveIpv4Addresses();
            if (localIpv4List.empty())
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = "未枚举到可用 IPv4 接口。";
                }
                return socketList;
            }

            for (const std::uint32_t localIpv4 : localIpv4List)
            {
                SOCKET rawSocket = ::socket(AF_INET, SOCK_RAW, IPPROTO_IP);
                if (rawSocket == INVALID_SOCKET)
                {
                    continue;
                }

                sockaddr_in bindAddress{};
                bindAddress.sin_family = AF_INET;
                bindAddress.sin_port = 0;
                bindAddress.sin_addr.s_addr = htonl(localIpv4);
                if (::bind(rawSocket, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR)
                {
                    ::closesocket(rawSocket);
                    continue;
                }

                DWORD receiveAllFlag = RCVALL_ON;
                DWORD bytesReturned = 0;
                if (::WSAIoctl(
                    rawSocket,
                    SIO_RCVALL,
                    &receiveAllFlag,
                    sizeof(receiveAllFlag),
                    nullptr,
                    0,
                    &bytesReturned,
                    nullptr,
                    nullptr) == SOCKET_ERROR)
                {
                    ::closesocket(rawSocket);
                    continue;
                }

                // 设为非阻塞：配合 select 周期轮询，避免线程卡死。
                u_long nonBlockingEnabled = 1;
                ::ioctlsocket(rawSocket, FIONBIO, &nonBlockingEnabled);

                CaptureSocketEntry socketEntry;
                socketEntry.socketValue = rawSocket;
                socketEntry.localIpv4HostOrder = localIpv4;
                socketEntry.localIpv4Text = Ipv4HostToString(localIpv4);
                socketList.push_back(socketEntry);
            }

            if (socketList.empty() && errorTextOut != nullptr)
            {
                const int lastError = ::WSAGetLastError();
                std::ostringstream stream;
                stream << "Raw Socket 打开失败，通常需要管理员权限。"
                    << " " << MakeWinSockErrorText(lastError);
                *errorTextOut = stream.str();
            }

            return socketList;
        }

        // CloseCaptureSockets：关闭全部 Raw Socket。
        inline void CloseCaptureSockets(std::vector<CaptureSocketEntry>& socketList)
        {
            for (CaptureSocketEntry& socketEntry : socketList)
            {
                if (socketEntry.socketValue != INVALID_SOCKET)
                {
                    ::closesocket(socketEntry.socketValue);
                    socketEntry.socketValue = INVALID_SOCKET;
                }
            }
            socketList.clear();
        }

        // ParseOutboundIpv4Packet：
        // - 解析 IPv4 报文中的 TCP/UDP 头；
        // - 只返回“本机发送方向”的数据（满足流量监控需求）。
        inline bool ParseOutboundIpv4Packet(
            const std::uint8_t* packetBuffer,
            const std::size_t packetBufferLength,
            const std::unordered_set<std::uint32_t>& localIpv4Set,
            PacketRecord& packetOut)
        {
            if (packetBuffer == nullptr || packetBufferLength < 20)
            {
                return false;
            }

            // IPv4 首字节：高 4 位 Version，低 4 位 IHL(32bit words)。
            const std::uint8_t versionAndHeaderLength = packetBuffer[0];
            const std::uint8_t ipVersion = static_cast<std::uint8_t>(versionAndHeaderLength >> 4);
            const std::size_t ipHeaderLength = static_cast<std::size_t>(versionAndHeaderLength & 0x0F) * 4ULL;
            if (ipVersion != 4 || ipHeaderLength < 20 || packetBufferLength < ipHeaderLength)
            {
                return false;
            }

            // IP Total Length 是网络字节序，且可能小于接收缓冲长度。
            std::size_t totalLength = static_cast<std::size_t>(ReadNetworkUInt16(packetBuffer + 2));
            if (totalLength < ipHeaderLength)
            {
                return false;
            }
            totalLength = std::min(totalLength, packetBufferLength);

            // 协议字段：6=TCP，17=UDP。
            const std::uint8_t protocolField = packetBuffer[9];
            PacketTransportProtocol protocol = PacketTransportProtocol::Tcp;
            if (protocolField == IPPROTO_TCP)
            {
                protocol = PacketTransportProtocol::Tcp;
            }
            else if (protocolField == IPPROTO_UDP)
            {
                protocol = PacketTransportProtocol::Udp;
            }
            else
            {
                return false;
            }

            // 源/目的地址按主机序存储，便于后续比较与哈希。
            const std::uint32_t sourceIpv4 = ReadNetworkUInt32(packetBuffer + 12);
            const std::uint32_t destinationIpv4 = ReadNetworkUInt32(packetBuffer + 16);

            // 方向判定：要求源地址属于本机地址集合，判定为 Outbound。
            const bool sourceIsLocal = (localIpv4Set.find(sourceIpv4) != localIpv4Set.end());
            const bool destinationIsLocal = (localIpv4Set.find(destinationIpv4) != localIpv4Set.end());
            PacketDirection direction = PacketDirection::Unknown;
            if (sourceIsLocal)
            {
                direction = PacketDirection::Outbound;
            }
            else if (destinationIsLocal)
            {
                direction = PacketDirection::Inbound;
            }

            // 本功能明确只收集“发送方向”，因此非 Outbound 直接忽略。
            if (direction != PacketDirection::Outbound)
            {
                return false;
            }

            std::uint16_t localPort = 0;
            std::uint16_t remotePort = 0;
            std::size_t payloadOffset = 0;

            if (protocol == PacketTransportProtocol::Tcp)
            {
                // TCP 最小头 20 字节。
                const std::size_t tcpOffset = ipHeaderLength;
                if (totalLength < tcpOffset + 20)
                {
                    return false;
                }

                localPort = ReadNetworkUInt16(packetBuffer + tcpOffset);
                remotePort = ReadNetworkUInt16(packetBuffer + tcpOffset + 2);

                const std::uint8_t tcpDataOffsetField = packetBuffer[tcpOffset + 12];
                const std::size_t tcpHeaderLength = static_cast<std::size_t>((tcpDataOffsetField >> 4) & 0x0F) * 4ULL;
                if (tcpHeaderLength < 20 || totalLength < tcpOffset + tcpHeaderLength)
                {
                    return false;
                }
                payloadOffset = tcpOffset + tcpHeaderLength;
            }
            else
            {
                // UDP 固定 8 字节头。
                const std::size_t udpOffset = ipHeaderLength;
                if (totalLength < udpOffset + 8)
                {
                    return false;
                }

                localPort = ReadNetworkUInt16(packetBuffer + udpOffset);
                remotePort = ReadNetworkUInt16(packetBuffer + udpOffset + 2);
                payloadOffset = udpOffset + 8;
            }

            // 组装输出结构，供 UI 直接使用。
            packetOut.protocol = protocol;
            packetOut.direction = direction;
            packetOut.processId = 0;
            packetOut.processName.clear();
            packetOut.localAddress = Ipv4HostToString(sourceIpv4);
            packetOut.localPort = localPort;
            packetOut.remoteAddress = Ipv4HostToString(destinationIpv4);
            packetOut.remotePort = remotePort;
            packetOut.totalPacketSize = static_cast<std::uint32_t>(totalLength);
            packetOut.payloadOffset = payloadOffset;
            packetOut.payloadSize = (payloadOffset <= totalLength)
                ? static_cast<std::uint32_t>(totalLength - payloadOffset)
                : 0U;

            const std::size_t retainedLength = std::min(totalLength, kMaxRetainedPacketBytes);
            packetOut.packetBytes.assign(packetBuffer, packetBuffer + retainedLength);
            packetOut.packetBytesTruncated = (retainedLength < totalLength);

            return true;
        }

        // ConnectionPidResolver：
        // - 周期刷新 TCP/UDP owning PID 表；
        // - 提供“连接四元组/本地端点 -> PID”映射能力。
        class ConnectionPidResolver final
        {
        public:
            // ResolveProcessId：按协议与端点信息解析归属 PID。
            std::uint32_t ResolveProcessId(
                const PacketTransportProtocol protocol,
                const std::uint32_t localIpv4,
                const std::uint16_t localPort,
                const std::uint32_t remoteIpv4,
                const std::uint16_t remotePort)
            {
                const std::uint64_t nowTickMs = NowTickMs();
                RefreshIfRequired(nowTickMs);

                if (protocol == PacketTransportProtocol::Tcp)
                {
                    // 先做 TCP 四元组精确匹配。
                    const TcpEndpointKey exactKey{ localIpv4, localPort, remoteIpv4, remotePort };
                    const auto exactIterator = m_tcpPidByQuad.find(exactKey);
                    if (exactIterator != m_tcpPidByQuad.end())
                    {
                        return exactIterator->second;
                    }

                    // 再回退到本地端点匹配（例如某些瞬时连接阶段）。
                    const std::uint64_t localExactKey = LocalEndpointKey(localIpv4, localPort);
                    const auto localExactIterator = m_tcpPidByLocalEndpoint.find(localExactKey);
                    if (localExactIterator != m_tcpPidByLocalEndpoint.end())
                    {
                        return localExactIterator->second;
                    }

                    // 再尝试 0.0.0.0:port 的占位映射。
                    const std::uint64_t localWildcardKey = LocalEndpointKey(0, localPort);
                    const auto localWildcardIterator = m_tcpPidByLocalEndpoint.find(localWildcardKey);
                    if (localWildcardIterator != m_tcpPidByLocalEndpoint.end())
                    {
                        return localWildcardIterator->second;
                    }
                    return 0;
                }

                // UDP 主要按本地端点映射。
                const std::uint64_t udpLocalKey = LocalEndpointKey(localIpv4, localPort);
                const auto udpIterator = m_udpPidByLocalEndpoint.find(udpLocalKey);
                if (udpIterator != m_udpPidByLocalEndpoint.end())
                {
                    return udpIterator->second;
                }

                // 同样提供 0.0.0.0:port fallback。
                const std::uint64_t udpWildcardKey = LocalEndpointKey(0, localPort);
                const auto udpWildcardIterator = m_udpPidByLocalEndpoint.find(udpWildcardKey);
                if (udpWildcardIterator != m_udpPidByLocalEndpoint.end())
                {
                    return udpWildcardIterator->second;
                }
                return 0;
            }

        private:
            // RefreshIfRequired：连接表刷新节流，避免每包都调用 IPHLPAPI。
            void RefreshIfRequired(const std::uint64_t nowTickMs)
            {
                constexpr std::uint64_t kRefreshIntervalMs = 900;
                if (m_lastRefreshTickMs != 0 &&
                    nowTickMs - m_lastRefreshTickMs < kRefreshIntervalMs)
                {
                    return;
                }

                RefreshTcpTable();
                RefreshUdpTable();
                m_lastRefreshTickMs = nowTickMs;
            }

            // RefreshTcpTable：刷新 TCP 四元组映射。
            void RefreshTcpTable()
            {
                DWORD requiredLength = 0;
                ::GetExtendedTcpTable(
                    nullptr,
                    &requiredLength,
                    FALSE,
                    AF_INET,
                    TCP_TABLE_OWNER_PID_ALL,
                    0);

                if (requiredLength == 0)
                {
                    return;
                }

                std::vector<std::uint8_t> tableBuffer(requiredLength, 0);
                DWORD tableLength = requiredLength;
                const DWORD result = ::GetExtendedTcpTable(
                    tableBuffer.data(),
                    &tableLength,
                    FALSE,
                    AF_INET,
                    TCP_TABLE_OWNER_PID_ALL,
                    0);
                if (result != NO_ERROR)
                {
                    return;
                }

                auto* tcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(tableBuffer.data());
                if (tcpTable == nullptr)
                {
                    return;
                }

                m_tcpPidByQuad.clear();
                m_tcpPidByLocalEndpoint.clear();
                m_tcpPidByQuad.reserve(tcpTable->dwNumEntries);
                m_tcpPidByLocalEndpoint.reserve(tcpTable->dwNumEntries);

                for (DWORD index = 0; index < tcpTable->dwNumEntries; ++index)
                {
                    const MIB_TCPROW_OWNER_PID& row = tcpTable->table[index];
                    const std::uint32_t localIpv4 = ntohl(row.dwLocalAddr);
                    const std::uint16_t localPort = ntohs(static_cast<u_short>(row.dwLocalPort & 0xFFFFu));
                    const std::uint32_t remoteIpv4 = ntohl(row.dwRemoteAddr);
                    const std::uint16_t remotePort = ntohs(static_cast<u_short>(row.dwRemotePort & 0xFFFFu));
                    const std::uint32_t ownerPid = row.dwOwningPid;

                    const TcpEndpointKey quadKey{ localIpv4, localPort, remoteIpv4, remotePort };
                    m_tcpPidByQuad[quadKey] = ownerPid;

                    const std::uint64_t localKey = LocalEndpointKey(localIpv4, localPort);
                    m_tcpPidByLocalEndpoint[localKey] = ownerPid;
                }
            }

            // RefreshUdpTable：刷新 UDP 本地端点映射。
            void RefreshUdpTable()
            {
                DWORD requiredLength = 0;
                ::GetExtendedUdpTable(
                    nullptr,
                    &requiredLength,
                    FALSE,
                    AF_INET,
                    UDP_TABLE_OWNER_PID,
                    0);

                if (requiredLength == 0)
                {
                    return;
                }

                std::vector<std::uint8_t> tableBuffer(requiredLength, 0);
                DWORD tableLength = requiredLength;
                const DWORD result = ::GetExtendedUdpTable(
                    tableBuffer.data(),
                    &tableLength,
                    FALSE,
                    AF_INET,
                    UDP_TABLE_OWNER_PID,
                    0);
                if (result != NO_ERROR)
                {
                    return;
                }

                auto* udpTable = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(tableBuffer.data());
                if (udpTable == nullptr)
                {
                    return;
                }

                m_udpPidByLocalEndpoint.clear();
                m_udpPidByLocalEndpoint.reserve(udpTable->dwNumEntries);

                for (DWORD index = 0; index < udpTable->dwNumEntries; ++index)
                {
                    const MIB_UDPROW_OWNER_PID& row = udpTable->table[index];
                    const std::uint32_t localIpv4 = ntohl(row.dwLocalAddr);
                    const std::uint16_t localPort = ntohs(static_cast<u_short>(row.dwLocalPort & 0xFFFFu));
                    const std::uint32_t ownerPid = row.dwOwningPid;
                    const std::uint64_t localKey = LocalEndpointKey(localIpv4, localPort);
                    m_udpPidByLocalEndpoint[localKey] = ownerPid;
                }
            }

        private:
            std::uint64_t m_lastRefreshTickMs = 0; // 上次刷新连接表时间戳。

            // TCP 映射（四元组精确）。
            std::unordered_map<TcpEndpointKey, std::uint32_t, TcpEndpointKeyHasher> m_tcpPidByQuad;

            // TCP 映射（本地端点 fallback）。
            std::unordered_map<std::uint64_t, std::uint32_t> m_tcpPidByLocalEndpoint;

            // UDP 映射（本地端点）。
            std::unordered_map<std::uint64_t, std::uint32_t> m_udpPidByLocalEndpoint;
        };

        // ProcessNameResolver：
        // - 为 PID 提供轻量缓存，避免每包都跨进程查询进程名。
        class ProcessNameResolver final
        {
        public:
            // ResolveProcessName：解析并缓存进程名。
            std::string ResolveProcessName(const std::uint32_t processId)
            {
                if (processId == 0)
                {
                    return std::string();
                }

                const std::uint64_t nowTickMs = NowTickMs();
                const auto cacheIterator = m_cacheByPid.find(processId);
                if (cacheIterator != m_cacheByPid.end())
                {
                    constexpr std::uint64_t kCacheTtlMs = 2500;
                    if (nowTickMs - cacheIterator->second.lastUpdateTickMs <= kCacheTtlMs)
                    {
                        return cacheIterator->second.processName;
                    }
                }

                std::string processName = ks::process::GetProcessNameByPID(processId);
                if (processName.empty())
                {
                    processName = std::string("PID-") + std::to_string(processId);
                }

                CacheEntry cacheEntry;
                cacheEntry.processName = processName;
                cacheEntry.lastUpdateTickMs = nowTickMs;
                m_cacheByPid[processId] = cacheEntry;
                return processName;
            }

        private:
            // CacheEntry：进程名缓存条目。
            struct CacheEntry
            {
                std::string processName;           // 缓存的进程名。
                std::uint64_t lastUpdateTickMs = 0;// 更新时间戳。
            };

            std::unordered_map<std::uint32_t, CacheEntry> m_cacheByPid; // PID -> 名称缓存。
        };
    } // namespace detail

    // TrafficMonitorService：
    // - 对外提供 Start/Stop 与回调注册；
    // - 内部线程负责抓包、PID 解析、限速控制。
    class TrafficMonitorService final
    {
    public:
        // PacketCallback：上报单条报文记录。
        using PacketCallback = std::function<void(const PacketRecord&)>;

        // StatusCallback：上报状态文本（启动失败、线程停止、权限提示等）。
        using StatusCallback = std::function<void(const std::string&)>;

        // RateLimitActionCallback：上报限速动作事件（挂起/恢复）。
        using RateLimitActionCallback = std::function<void(const RateLimitActionEvent&)>;

    public:
        // 构造：初始化状态，不启动线程。
        TrafficMonitorService() = default;

        // 析构：确保线程停止并释放资源。
        ~TrafficMonitorService()
        {
            StopCapture();
        }

        // SetPacketCallback：设置报文回调。
        void SetPacketCallback(PacketCallback callback)
        {
            std::lock_guard<std::mutex> guard(m_stateMutex);
            m_packetCallback = std::move(callback);
        }

        // SetStatusCallback：设置状态回调。
        void SetStatusCallback(StatusCallback callback)
        {
            std::lock_guard<std::mutex> guard(m_stateMutex);
            m_statusCallback = std::move(callback);
        }

        // SetRateLimitActionCallback：设置限速动作回调。
        void SetRateLimitActionCallback(RateLimitActionCallback callback)
        {
            std::lock_guard<std::mutex> guard(m_stateMutex);
            m_rateLimitActionCallback = std::move(callback);
        }

        // StartCapture：
        // - 启动后台抓包线程；
        // - 若已运行则直接返回 true。
        bool StartCapture()
        {
            if (m_running.load(std::memory_order_relaxed))
            {
                return true;
            }

            // 如果上一次线程已自然退出但尚未 join，这里先清理。
            if (m_captureThread.joinable())
            {
                m_captureThread.join();
            }

            m_running.store(true, std::memory_order_relaxed);
            try
            {
                m_captureThread = std::thread([this]() { runCaptureThread(); });
            }
            catch (...)
            {
                m_running.store(false, std::memory_order_relaxed);
                emitStatus("抓包线程创建失败。");
                return false;
            }
            return true;
        }

        // StopCapture：
        // - 请求线程退出；
        // - 阻塞等待线程收尾（包括恢复被限速挂起的进程）。
        void StopCapture()
        {
            m_running.store(false, std::memory_order_relaxed);
            if (m_captureThread.joinable())
            {
                m_captureThread.join();
            }
        }

        // IsRunning：查询抓包线程是否处于运行状态。
        bool IsRunning() const
        {
            return m_running.load(std::memory_order_relaxed);
        }

        // UpsertRateLimitRule：
        // - 新增或更新某 PID 的限速规则；
        // - bytesPerSecond==0 或 pid==0 会忽略。
        void UpsertRateLimitRule(const ProcessRateLimitRule& inputRule)
        {
            if (inputRule.processId == 0 || inputRule.bytesPerSecond == 0)
            {
                return;
            }

            ProcessRateLimitRule sanitizedRule = inputRule;
            sanitizedRule.suspendDurationMs = std::clamp<std::uint32_t>(sanitizedRule.suspendDurationMs, 50, 2000);

            bool needResumeExistingProcess = false;
            {
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                RateLimitRuntime& runtime = m_rateLimitByPid[sanitizedRule.processId];
                needResumeExistingProcess = runtime.currentlySuspended;
                runtime.rule = sanitizedRule;
                runtime.windowStartTickMs = detail::NowTickMs();
                runtime.currentWindowBytes = 0;
                runtime.currentlySuspended = false;
                runtime.resumeTickMs = 0;
                // triggerCount 保留历史，方便 UI 查看总触发次数。
            }

            // 如果旧规则处于挂起状态，更新规则时先恢复进程，避免误留挂起态。
            if (needResumeExistingProcess)
            {
                std::string ignoredDetailText;
                ks::process::ResumeProcess(sanitizedRule.processId, &ignoredDetailText);
            }
        }

        // RemoveRateLimitRule：移除指定 PID 的限速规则。
        bool RemoveRateLimitRule(const std::uint32_t processId)
        {
            if (processId == 0)
            {
                return false;
            }

            bool needResumeProcess = false;
            {
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                const auto iterator = m_rateLimitByPid.find(processId);
                if (iterator == m_rateLimitByPid.end())
                {
                    return false;
                }
                needResumeProcess = iterator->second.currentlySuspended;
                m_rateLimitByPid.erase(iterator);
            }

            // 删除规则时若曾挂起，则主动恢复，避免残留状态。
            if (needResumeProcess)
            {
                std::string ignoredDetailText;
                ks::process::ResumeProcess(processId, &ignoredDetailText);
            }
            return true;
        }

        // ClearRateLimitRules：清空全部限速规则。
        void ClearRateLimitRules()
        {
            std::vector<std::uint32_t> processIdsNeedResume;
            {
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                processIdsNeedResume.reserve(m_rateLimitByPid.size());
                for (const auto& [processId, runtime] : m_rateLimitByPid)
                {
                    if (runtime.currentlySuspended)
                    {
                        processIdsNeedResume.push_back(processId);
                    }
                }
                m_rateLimitByPid.clear();
            }

            // 批量清空规则时，同步恢复此前被挂起的进程。
            for (const std::uint32_t processId : processIdsNeedResume)
            {
                std::string ignoredDetailText;
                ks::process::ResumeProcess(processId, &ignoredDetailText);
            }
        }

        // SnapshotRateLimitRules：
        // - 返回限速规则快照（线程安全）；
        // - UI 可直接用于表格刷新。
        std::vector<ProcessRateLimitSnapshot> SnapshotRateLimitRules() const
        {
            std::vector<ProcessRateLimitSnapshot> snapshotList;
            std::lock_guard<std::mutex> guard(m_rateLimitMutex);
            snapshotList.reserve(m_rateLimitByPid.size());

            for (const auto& [processId, runtime] : m_rateLimitByPid)
            {
                (void)processId;
                ProcessRateLimitSnapshot snapshot;
                snapshot.rule = runtime.rule;
                snapshot.currentWindowBytes = runtime.currentWindowBytes;
                snapshot.triggerCount = runtime.triggerCount;
                snapshot.currentlySuspended = runtime.currentlySuspended;
                snapshotList.push_back(snapshot);
            }

            std::sort(
                snapshotList.begin(),
                snapshotList.end(),
                [](const ProcessRateLimitSnapshot& left, const ProcessRateLimitSnapshot& right)
                {
                    return left.rule.processId < right.rule.processId;
                });

            return snapshotList;
        }

    private:
        // RateLimitRuntime：限速规则的运行时状态。
        struct RateLimitRuntime
        {
            ProcessRateLimitRule rule;          // 用户配置。
            std::uint64_t windowStartTickMs = 0;// 当前统计窗口起始 ms。
            std::uint64_t currentWindowBytes = 0;// 当前窗口累计发送字节。
            bool currentlySuspended = false;    // 当前是否由本组件挂起。
            std::uint64_t resumeTickMs = 0;     // 计划恢复时间戳。
            std::uint64_t triggerCount = 0;     // 历史触发次数。
        };

    private:
        // runCaptureThread：后台线程主循环。
        void runCaptureThread()
        {
            // 线程启动后先初始化 WinSock。
            WSADATA wsaData{};
            const int startupResult = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
            if (startupResult != 0)
            {
                std::ostringstream stream;
                stream << "WSAStartup 失败: " << detail::MakeWinSockErrorText(startupResult);
                emitStatus(stream.str());
                m_running.store(false, std::memory_order_relaxed);
                return;
            }

            // 打开全部可用 Raw Socket（按活动 IPv4 接口逐一绑定）。
            std::string socketOpenError;
            std::vector<detail::CaptureSocketEntry> captureSockets = detail::OpenCaptureSockets(&socketOpenError);
            if (captureSockets.empty())
            {
                std::ostringstream stream;
                stream << "网络监控启动失败: " << socketOpenError;
                emitStatus(stream.str());
                ::WSACleanup();
                m_running.store(false, std::memory_order_relaxed);
                return;
            }

            {
                std::ostringstream stream;
                stream << "网络监控已启动，绑定接口数: " << captureSockets.size();
                emitStatus(stream.str());
            }

            // 构建本机地址集合，用于快速判定报文方向。
            std::unordered_set<std::uint32_t> localIpv4Set;
            for (const detail::CaptureSocketEntry& socketEntry : captureSockets)
            {
                localIpv4Set.insert(socketEntry.localIpv4HostOrder);
            }

            detail::ConnectionPidResolver pidResolver;
            detail::ProcessNameResolver processNameResolver;
            std::vector<std::uint8_t> receiveBuffer(65536, 0);

            // 抓包循环：select 等待 + recv 读取 + 解析 + 回调。
            while (m_running.load(std::memory_order_relaxed))
            {
                const std::uint64_t nowTickMs = detail::NowTickMs();

                // 每轮先处理“应恢复”的限速任务，避免进程长时间挂起。
                consumeResumeActions(nowTickMs);

                fd_set readSet{};
                FD_ZERO(&readSet);
                for (const detail::CaptureSocketEntry& socketEntry : captureSockets)
                {
                    if (socketEntry.socketValue != INVALID_SOCKET)
                    {
                        FD_SET(socketEntry.socketValue, &readSet);
                    }
                }

                // 150ms 轮询周期：平衡实时性与 CPU 占用。
                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 150000;
                const int selectResult = ::select(0, &readSet, nullptr, nullptr, &timeout);
                if (selectResult == SOCKET_ERROR)
                {
                    const int selectError = ::WSAGetLastError();
                    std::ostringstream stream;
                    stream << "select 失败: " << detail::MakeWinSockErrorText(selectError);
                    emitStatus(stream.str());
                    break;
                }

                if (selectResult <= 0)
                {
                    continue;
                }

                for (const detail::CaptureSocketEntry& socketEntry : captureSockets)
                {
                    if (socketEntry.socketValue == INVALID_SOCKET)
                    {
                        continue;
                    }
                    if (FD_ISSET(socketEntry.socketValue, &readSet) == 0)
                    {
                        continue;
                    }

                    const int receivedLength = ::recv(
                        socketEntry.socketValue,
                        reinterpret_cast<char*>(receiveBuffer.data()),
                        static_cast<int>(receiveBuffer.size()),
                        0);
                    if (receivedLength <= 0)
                    {
                        const int recvError = ::WSAGetLastError();
                        if (recvError == WSAEWOULDBLOCK || recvError == WSAETIMEDOUT)
                        {
                            continue;
                        }
                        continue;
                    }

                    PacketRecord packetRecord;
                    const bool parseOk = detail::ParseOutboundIpv4Packet(
                        receiveBuffer.data(),
                        static_cast<std::size_t>(receivedLength),
                        localIpv4Set,
                        packetRecord);
                    if (!parseOk)
                    {
                        continue;
                    }

                    // 填充序号与时间戳，保证 UI 按抓包顺序展示。
                    packetRecord.sequenceId = m_packetSequence.fetch_add(1, std::memory_order_relaxed) + 1;
                    packetRecord.captureTimestampMs = detail::NowTickMs();

                    // 端口映射 PID + 名称缓存解析。
                    const std::uint32_t localIpv4Host = detail::ReadNetworkUInt32(receiveBuffer.data() + 12);
                    const std::uint32_t remoteIpv4Host = detail::ReadNetworkUInt32(receiveBuffer.data() + 16);
                    packetRecord.processId = pidResolver.ResolveProcessId(
                        packetRecord.protocol,
                        localIpv4Host,
                        packetRecord.localPort,
                        remoteIpv4Host,
                        packetRecord.remotePort);
                    packetRecord.processName = processNameResolver.ResolveProcessName(packetRecord.processId);

                    // 先执行限速判定，再上报报文给 UI。
                    consumeRateLimitForPacket(packetRecord);
                    emitPacket(packetRecord);
                }
            }

            // 线程退出前恢复所有“仍被本模块挂起”的进程，避免残留影响。
            forceResumeAllSuspendedProcesses();

            detail::CloseCaptureSockets(captureSockets);
            ::WSACleanup();

            m_running.store(false, std::memory_order_relaxed);
            emitStatus("网络监控已停止。");
        }

        // consumeRateLimitForPacket：
        // - 对单包更新对应 PID 的发送窗口统计；
        // - 若超限则触发 SuspendProcess。
        void consumeRateLimitForPacket(const PacketRecord& packetRecord)
        {
            if (packetRecord.processId == 0)
            {
                return;
            }

            std::optional<RateLimitActionEvent> suspendEvent;
            {
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                const auto iterator = m_rateLimitByPid.find(packetRecord.processId);
                if (iterator == m_rateLimitByPid.end())
                {
                    return;
                }

                RateLimitRuntime& runtime = iterator->second;
                if (!runtime.rule.enabled || runtime.rule.bytesPerSecond == 0)
                {
                    return;
                }

                const std::uint64_t nowTickMs = packetRecord.captureTimestampMs;
                if (runtime.windowStartTickMs == 0 || nowTickMs - runtime.windowStartTickMs >= 1000)
                {
                    runtime.windowStartTickMs = nowTickMs;
                    runtime.currentWindowBytes = 0;
                }

                runtime.currentWindowBytes += packetRecord.totalPacketSize;
                if (runtime.currentlySuspended)
                {
                    return;
                }

                if (runtime.currentWindowBytes > runtime.rule.bytesPerSecond)
                {
                    runtime.currentlySuspended = true;
                    runtime.resumeTickMs = nowTickMs + runtime.rule.suspendDurationMs;
                    runtime.triggerCount += 1;

                    RateLimitActionEvent event;
                    event.timestampMs = nowTickMs;
                    event.processId = runtime.rule.processId;
                    event.actionType = RateLimitActionType::SuspendProcess;
                    event.actionSucceeded = false;
                    event.configuredBytesPerSecond = runtime.rule.bytesPerSecond;
                    event.detailText = "触发限速，准备挂起进程。";
                    suspendEvent = event;
                }
            }

            if (!suspendEvent.has_value())
            {
                return;
            }

            std::string detailText;
            const bool suspendOk = ks::process::SuspendProcess(packetRecord.processId, &detailText);
            suspendEvent->actionSucceeded = suspendOk;
            if (suspendOk)
            {
                suspendEvent->detailText = "限速触发：进程已挂起。";
            }
            else
            {
                suspendEvent->detailText = "限速触发失败（挂起失败）: " + detailText;

                // 挂起失败则立即回滚“挂起状态”，避免后续逻辑误判。
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                const auto iterator = m_rateLimitByPid.find(packetRecord.processId);
                if (iterator != m_rateLimitByPid.end())
                {
                    iterator->second.currentlySuspended = false;
                    iterator->second.resumeTickMs = 0;
                }
            }

            emitRateLimitAction(*suspendEvent);
        }

        // consumeResumeActions：
        // - 扫描全部规则，找出“已到恢复时间”的 PID；
        // - 执行 ResumeProcess 并上报动作事件。
        void consumeResumeActions(const std::uint64_t nowTickMs)
        {
            std::vector<std::uint32_t> processIdsToResume;
            {
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                for (auto& [processId, runtime] : m_rateLimitByPid)
                {
                    if (!runtime.currentlySuspended)
                    {
                        continue;
                    }
                    if (runtime.resumeTickMs > nowTickMs)
                    {
                        continue;
                    }

                    runtime.currentlySuspended = false;
                    runtime.resumeTickMs = 0;
                    processIdsToResume.push_back(processId);
                }
            }

            for (const std::uint32_t processId : processIdsToResume)
            {
                std::string detailText;
                const bool resumeOk = ks::process::ResumeProcess(processId, &detailText);

                RateLimitActionEvent event;
                event.timestampMs = nowTickMs;
                event.processId = processId;
                event.actionType = RateLimitActionType::ResumeProcess;
                event.actionSucceeded = resumeOk;
                event.configuredBytesPerSecond = 0;
                event.detailText = resumeOk
                    ? "限速窗口结束：进程已恢复。"
                    : ("进程恢复失败: " + detailText);
                emitRateLimitAction(event);
            }
        }

        // forceResumeAllSuspendedProcesses：
        // - 在线程退出阶段执行；
        // - 保证不会遗留“被限速挂起”的进程。
        void forceResumeAllSuspendedProcesses()
        {
            std::vector<std::uint32_t> processIdsToResume;
            {
                std::lock_guard<std::mutex> guard(m_rateLimitMutex);
                for (auto& [processId, runtime] : m_rateLimitByPid)
                {
                    if (!runtime.currentlySuspended)
                    {
                        continue;
                    }
                    runtime.currentlySuspended = false;
                    runtime.resumeTickMs = 0;
                    processIdsToResume.push_back(processId);
                }
            }

            for (const std::uint32_t processId : processIdsToResume)
            {
                std::string detailText;
                const bool resumeOk = ks::process::ResumeProcess(processId, &detailText);

                RateLimitActionEvent event;
                event.timestampMs = detail::NowTickMs();
                event.processId = processId;
                event.actionType = RateLimitActionType::ResumeProcess;
                event.actionSucceeded = resumeOk;
                event.configuredBytesPerSecond = 0;
                event.detailText = resumeOk
                    ? "监控停止：已恢复此前挂起的进程。"
                    : ("监控停止：恢复进程失败: " + detailText);
                emitRateLimitAction(event);
            }
        }

        // emitPacket：触发报文回调（线程安全复制回调对象）。
        void emitPacket(const PacketRecord& packetRecord) const
        {
            PacketCallback callbackCopy;
            {
                std::lock_guard<std::mutex> guard(m_stateMutex);
                callbackCopy = m_packetCallback;
            }
            if (callbackCopy)
            {
                callbackCopy(packetRecord);
            }
        }

        // emitStatus：触发状态回调。
        void emitStatus(const std::string& statusText) const
        {
            StatusCallback callbackCopy;
            {
                std::lock_guard<std::mutex> guard(m_stateMutex);
                callbackCopy = m_statusCallback;
            }
            if (callbackCopy)
            {
                callbackCopy(statusText);
            }
        }

        // emitRateLimitAction：触发限速动作回调。
        void emitRateLimitAction(const RateLimitActionEvent& actionEvent) const
        {
            RateLimitActionCallback callbackCopy;
            {
                std::lock_guard<std::mutex> guard(m_stateMutex);
                callbackCopy = m_rateLimitActionCallback;
            }
            if (callbackCopy)
            {
                callbackCopy(actionEvent);
            }
        }

    private:
        mutable std::mutex m_stateMutex; // 保护回调函数对象。
        PacketCallback m_packetCallback; // 报文事件回调。
        StatusCallback m_statusCallback; // 状态文本回调。
        RateLimitActionCallback m_rateLimitActionCallback; // 限速动作回调。

        std::atomic<bool> m_running{ false }; // 抓包线程运行标记。
        std::thread m_captureThread;          // 后台抓包线程对象。
        std::atomic<std::uint64_t> m_packetSequence{ 0 }; // 报文序号生成器。

        mutable std::mutex m_rateLimitMutex; // 保护限速规则容器。
        std::unordered_map<std::uint32_t, RateLimitRuntime> m_rateLimitByPid; // PID -> 规则运行态。
    };
} // namespace ks::network
