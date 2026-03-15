#pragma once

// ============================================================
// ksword/network/network_connection_tools.h
// 命名空间：ks::network
// 作用：
// 1) 枚举 TCP 连接快照（含 PID、进程名、本地/远端端点、状态）；
// 2) 枚举 UDP 端点快照（含 PID、进程名、本地端点）；
// 3) 支持按指定 TCP 连接参数执行 DELETE_TCB 终止连接。
//
// 设计说明：
// - 本文件采用“头文件内联实现”，避免新增 cpp 文件后还需修改工程文件；
// - 供 NetworkDock 与其它工具模块直接 include 使用。
// ============================================================

#include "../process/process.h"

#include <algorithm>    // std::sort：快照排序，便于 UI 稳定显示。
#include <cstddef>      // std::size_t：容器索引与长度。
#include <cstdint>      // 固定宽度整数：PID/IP/Port 等字段。
#include <cstring>      // std::memset：结构体清零保护。
#include <string>       // std::string：跨层文本表达。
#include <unordered_map>// PID -> 进程名缓存，避免重复查询。
#include <utility>      // std::move：记录对象移动进容器。
#include <vector>       // 快照输出容器。

// Winsock / IP Helper 头：
// - WinSock2 需要先于 windows.h 链路中的 winsock.h。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Iphlpapi.h>

// 链接依赖：
// - Iphlpapi：GetExtendedTcpTable/GetExtendedUdpTable/SetTcpEntry；
// - Ws2_32：inet_ntop/htons/htonl 等网络转换函数。
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace ks::network
{
    // TcpConnectionRecord：
    // - TCP 连接快照记录；
    // - 同时保留文本地址与主机序地址，兼顾 UI 展示与连接控制。
    struct TcpConnectionRecord
    {
        std::uint32_t processId = 0;           // 所属进程 PID。
        std::string processName;               // 所属进程名（可能为空）。

        std::uint32_t localIpv4HostOrder = 0;  // 本地 IPv4（主机序，终止连接时使用）。
        std::uint16_t localPort = 0;           // 本地端口（主机序）。
        std::string localAddressText;          // 本地 IPv4 文本（UI 直接显示）。

        std::uint32_t remoteIpv4HostOrder = 0; // 远端 IPv4（主机序，终止连接时使用）。
        std::uint16_t remotePort = 0;          // 远端端口（主机序）。
        std::string remoteAddressText;         // 远端 IPv4 文本（UI 直接显示）。

        std::uint32_t tcpStateCode = 0;        // MIB_TCP_STATE_* 原始值。
        std::string tcpStateText;              // 状态文本（ESTABLISHED/LISTEN 等）。
    };

    // UdpEndpointRecord：
    // - UDP 端点快照记录；
    // - UDP 无“连接状态”，仅保留本地端点 + 进程归属。
    struct UdpEndpointRecord
    {
        std::uint32_t processId = 0;           // 所属进程 PID。
        std::string processName;               // 所属进程名（可能为空）。

        std::uint32_t localIpv4HostOrder = 0;  // 本地 IPv4（主机序）。
        std::uint16_t localPort = 0;           // 本地端口（主机序）。
        std::string localAddressText;          // 本地 IPv4 文本（UI 直接显示）。
    };

    namespace connection_detail
    {
        // Ipv4NetworkOrderToText：
        // - 把“网络序 IPv4”转换为点分十进制文本；
        // - 失败时回退到 "0.0.0.0"。
        inline std::string Ipv4NetworkOrderToText(const std::uint32_t ipv4NetworkOrder)
        {
            in_addr ipv4Address{};
            ipv4Address.s_addr = ipv4NetworkOrder;

            char ipv4TextBuffer[INET_ADDRSTRLEN] = {};
            const PCSTR convertResult = ::inet_ntop(
                AF_INET,
                &ipv4Address,
                ipv4TextBuffer,
                static_cast<socklen_t>(sizeof(ipv4TextBuffer)));
            if (convertResult == nullptr)
            {
                return std::string("0.0.0.0");
            }
            return std::string(ipv4TextBuffer);
        }

        // TcpStateCodeToText：
        // - 把 MIB_TCP_STATE_* 状态值转换为易读文本；
        // - 未识别状态统一返回 "UNKNOWN"。
        inline std::string TcpStateCodeToText(const std::uint32_t stateCode)
        {
            switch (stateCode)
            {
            case MIB_TCP_STATE_CLOSED:      return "CLOSED";
            case MIB_TCP_STATE_LISTEN:      return "LISTEN";
            case MIB_TCP_STATE_SYN_SENT:    return "SYN_SENT";
            case MIB_TCP_STATE_SYN_RCVD:    return "SYN_RECV";
            case MIB_TCP_STATE_ESTAB:       return "ESTABLISHED";
            case MIB_TCP_STATE_FIN_WAIT1:   return "FIN_WAIT1";
            case MIB_TCP_STATE_FIN_WAIT2:   return "FIN_WAIT2";
            case MIB_TCP_STATE_CLOSE_WAIT:  return "CLOSE_WAIT";
            case MIB_TCP_STATE_CLOSING:     return "CLOSING";
            case MIB_TCP_STATE_LAST_ACK:    return "LAST_ACK";
            case MIB_TCP_STATE_TIME_WAIT:   return "TIME_WAIT";
            case MIB_TCP_STATE_DELETE_TCB:  return "DELETE_TCB";
            default:                        return "UNKNOWN";
            }
        }

        // FillProcessNameCacheIfNeeded：
        // - 延迟查询 PID 对应进程名并写入缓存；
        // - 避免在大连接表里重复调用 Query API 导致性能浪费。
        inline const std::string& FillProcessNameCacheIfNeeded(
            const std::uint32_t processId,
            std::unordered_map<std::uint32_t, std::string>& processNameCache)
        {
            const auto cacheIterator = processNameCache.find(processId);
            if (cacheIterator != processNameCache.end())
            {
                return cacheIterator->second;
            }

            const std::string processName = ks::process::GetProcessNameByPID(processId);
            const auto insertResult = processNameCache.insert({ processId, processName });
            return insertResult.first->second;
        }
    } // namespace connection_detail

    // EnumerateTcpConnectionRecords：
    // - 枚举当前系统 IPv4 TCP 连接快照；
    // - 成功时返回 true，并填充 recordsOut；
    // - 失败时返回 false，并在 errorTextOut 写入错误说明。
    inline bool EnumerateTcpConnectionRecords(
        std::vector<TcpConnectionRecord>& recordsOut,
        std::string* errorTextOut = nullptr)
    {
        recordsOut.clear();
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        // 先查询所需缓冲区大小。
        ULONG requiredBufferLength = 0;
        DWORD queryResult = ::GetExtendedTcpTable(
            nullptr,
            &requiredBufferLength,
            TRUE,
            AF_INET,
            TCP_TABLE_OWNER_PID_ALL,
            0);
        if (queryResult != ERROR_INSUFFICIENT_BUFFER || requiredBufferLength == 0)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = "GetExtendedTcpTable(size query) failed, code=" + std::to_string(queryResult);
            }
            return false;
        }

        // 再按大小分配缓冲并执行真实查询。
        std::vector<std::uint8_t> tableBuffer(requiredBufferLength, 0);
        auto* tcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(tableBuffer.data());
        queryResult = ::GetExtendedTcpTable(
            tcpTable,
            &requiredBufferLength,
            TRUE,
            AF_INET,
            TCP_TABLE_OWNER_PID_ALL,
            0);
        if (queryResult != NO_ERROR)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = "GetExtendedTcpTable(data query) failed, code=" + std::to_string(queryResult);
            }
            return false;
        }

        // PID -> 进程名缓存，避免对同一 PID 重复查询。
        std::unordered_map<std::uint32_t, std::string> processNameCache;
        processNameCache.reserve(tcpTable->dwNumEntries);

        // 遍历原生表并转换成 Dock 可直接消费的结构。
        recordsOut.reserve(tcpTable->dwNumEntries);
        for (DWORD rowIndex = 0; rowIndex < tcpTable->dwNumEntries; ++rowIndex)
        {
            const MIB_TCPROW_OWNER_PID& row = tcpTable->table[rowIndex];

            TcpConnectionRecord record;
            record.processId = static_cast<std::uint32_t>(row.dwOwningPid);
            record.processName = connection_detail::FillProcessNameCacheIfNeeded(record.processId, processNameCache);

            // TCP 表中的地址字段是网络序，转换为主机序后便于比较。
            record.localIpv4HostOrder = ntohl(row.dwLocalAddr);
            record.remoteIpv4HostOrder = ntohl(row.dwRemoteAddr);
            record.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            record.remotePort = ntohs(static_cast<u_short>(row.dwRemotePort));

            // UI 展示文本保留原始地址语义。
            record.localAddressText = connection_detail::Ipv4NetworkOrderToText(row.dwLocalAddr);
            record.remoteAddressText = connection_detail::Ipv4NetworkOrderToText(row.dwRemoteAddr);
            record.tcpStateCode = static_cast<std::uint32_t>(row.dwState);
            record.tcpStateText = connection_detail::TcpStateCodeToText(record.tcpStateCode);
            recordsOut.push_back(std::move(record));
        }

        // 按 PID/本地端点/远端端点排序，保证 UI 每次刷新行顺序稳定。
        std::sort(
            recordsOut.begin(),
            recordsOut.end(),
            [](const TcpConnectionRecord& left, const TcpConnectionRecord& right)
            {
                if (left.processId != right.processId)
                {
                    return left.processId < right.processId;
                }
                if (left.localIpv4HostOrder != right.localIpv4HostOrder)
                {
                    return left.localIpv4HostOrder < right.localIpv4HostOrder;
                }
                if (left.localPort != right.localPort)
                {
                    return left.localPort < right.localPort;
                }
                if (left.remoteIpv4HostOrder != right.remoteIpv4HostOrder)
                {
                    return left.remoteIpv4HostOrder < right.remoteIpv4HostOrder;
                }
                return left.remotePort < right.remotePort;
            });

        return true;
    }

    // EnumerateUdpEndpointRecords：
    // - 枚举当前系统 IPv4 UDP 端点快照；
    // - 成功时返回 true，并填充 recordsOut；
    // - 失败时返回 false，并在 errorTextOut 写入错误说明。
    inline bool EnumerateUdpEndpointRecords(
        std::vector<UdpEndpointRecord>& recordsOut,
        std::string* errorTextOut = nullptr)
    {
        recordsOut.clear();
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        // 先查询缓冲长度，随后一次性拉取全部 UDP 端点数据。
        ULONG requiredBufferLength = 0;
        DWORD queryResult = ::GetExtendedUdpTable(
            nullptr,
            &requiredBufferLength,
            TRUE,
            AF_INET,
            UDP_TABLE_OWNER_PID,
            0);
        if (queryResult != ERROR_INSUFFICIENT_BUFFER || requiredBufferLength == 0)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = "GetExtendedUdpTable(size query) failed, code=" + std::to_string(queryResult);
            }
            return false;
        }

        std::vector<std::uint8_t> tableBuffer(requiredBufferLength, 0);
        auto* udpTable = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(tableBuffer.data());
        queryResult = ::GetExtendedUdpTable(
            udpTable,
            &requiredBufferLength,
            TRUE,
            AF_INET,
            UDP_TABLE_OWNER_PID,
            0);
        if (queryResult != NO_ERROR)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = "GetExtendedUdpTable(data query) failed, code=" + std::to_string(queryResult);
            }
            return false;
        }

        // PID 名称缓存，减少重复进程查询。
        std::unordered_map<std::uint32_t, std::string> processNameCache;
        processNameCache.reserve(udpTable->dwNumEntries);

        recordsOut.reserve(udpTable->dwNumEntries);
        for (DWORD rowIndex = 0; rowIndex < udpTable->dwNumEntries; ++rowIndex)
        {
            const MIB_UDPROW_OWNER_PID& row = udpTable->table[rowIndex];

            UdpEndpointRecord record;
            record.processId = static_cast<std::uint32_t>(row.dwOwningPid);
            record.processName = connection_detail::FillProcessNameCacheIfNeeded(record.processId, processNameCache);
            record.localIpv4HostOrder = ntohl(row.dwLocalAddr);
            record.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            record.localAddressText = connection_detail::Ipv4NetworkOrderToText(row.dwLocalAddr);
            recordsOut.push_back(std::move(record));
        }

        // UDP 端点排序规则：PID -> 本地端点，保证 UI 行稳定。
        std::sort(
            recordsOut.begin(),
            recordsOut.end(),
            [](const UdpEndpointRecord& left, const UdpEndpointRecord& right)
            {
                if (left.processId != right.processId)
                {
                    return left.processId < right.processId;
                }
                if (left.localIpv4HostOrder != right.localIpv4HostOrder)
                {
                    return left.localIpv4HostOrder < right.localIpv4HostOrder;
                }
                return left.localPort < right.localPort;
            });

        return true;
    }

    // TerminateTcpConnectionByRecord：
    // - 终止指定 TCP 连接（调用 SetTcpEntry + DELETE_TCB）；
    // - 仅对 IPv4 TCP 行有效；
    // - detailTextOut 会返回可读说明，便于 UI 直接提示用户。
    inline bool TerminateTcpConnectionByRecord(
        const TcpConnectionRecord& connectionRecord,
        std::string* detailTextOut = nullptr)
    {
        if (detailTextOut != nullptr)
        {
            detailTextOut->clear();
        }

        // 按 Win32 要求组装 MIB_TCPROW，地址/端口都需转换为网络序。
        MIB_TCPROW tcpRow{};
        std::memset(&tcpRow, 0, sizeof(tcpRow));
        tcpRow.dwState = MIB_TCP_STATE_DELETE_TCB;
        tcpRow.dwLocalAddr = htonl(connectionRecord.localIpv4HostOrder);
        tcpRow.dwRemoteAddr = htonl(connectionRecord.remoteIpv4HostOrder);
        tcpRow.dwLocalPort = static_cast<DWORD>(htons(connectionRecord.localPort));
        tcpRow.dwRemotePort = static_cast<DWORD>(htons(connectionRecord.remotePort));

        // SetTcpEntry 返回 NO_ERROR 表示成功，其它值均视为失败。
        const DWORD setResult = ::SetTcpEntry(&tcpRow);
        if (setResult != NO_ERROR)
        {
            if (detailTextOut != nullptr)
            {
                *detailTextOut = "SetTcpEntry failed, code=" + std::to_string(setResult);
            }
            return false;
        }

        if (detailTextOut != nullptr)
        {
            *detailTextOut = "SetTcpEntry succeeded.";
        }
        return true;
    }
} // namespace ks::network
