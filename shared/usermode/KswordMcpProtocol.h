#pragma once

// 本地 R3 GUI 桥协议：与驱动 IOCTL 无关，sidecar 只使用固定 JSON 方法。
namespace ks::mcp
{
    inline constexpr int SchemaVersion = 1; // 应用层响应版本，不是 MCP 协议版本。
    inline constexpr int MaxRequestBytes = 64 * 1024; // 单次 UTF-8 请求及换行的上限。
    inline constexpr int MaxResponseBytes = 2 * 1024 * 1024; // 单次 UTF-8 响应及换行的上限。
    inline constexpr int MaxConnections = 8; // 同时存在的本地请求连接上限。
    inline constexpr int ConnectionTimeoutMs = 15000; // 未完成连接的总存活时间上限。
    inline constexpr char EndpointPrefix[] = "ksword-mcp-"; // 后接当前主程序 PID。
}