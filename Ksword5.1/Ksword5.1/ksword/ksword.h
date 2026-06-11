#pragma once

// ============================================================
// ksword/ksword.h
// 作用：
// - 作为 ks 命名空间顶层聚合头；
// - 一级一级包含子命名空间头文件；
// - 外部通过 Ksword.h -> ksword/ksword.h 完成递归引入。
// ============================================================

// 字符串工具（UTF8/UTF16 转换、时间文本格式化等）。
#include "string/string.h"

// 日志工具（kLogEntry/事件追踪/流式日志输出）。
#include "log/log.h"

// 进程工具（枚举、详情、控制、优先级等 Win32 封装）。
#include "process/process.h"

// 文件工具（路径规范化、句柄扫描、PE 基础解析等非 UI 后端）。
#include "file/file.h"

// 启动项工具（注册表、服务、计划任务、Winsock、WMI 非 UI 枚举后端）。
#include "startup/startup.h"

// 服务工具（Win32 SCM 枚举、查询、控制与配置写入封装）。
#include "service/service.h"

// 网络工具（发送流量抓包、PID 映射、进程限速等）。
#include "network/network.h"

// 网络格式化工具（端点、IPv4范围、payload预览、字节/时间文本等）。
#include "network/network_format_tools.h"

// 网络诊断工具（ARP/DNS/ICMP结果纯格式化与扫描范围计算）。
#include "network/network_diagnostics_tools.h"

// 网络下载工具（Range分段计划、Range头、进度与速度计算）。
#include "network/network_download_tools.h"
