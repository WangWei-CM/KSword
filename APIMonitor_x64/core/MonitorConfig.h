#pragma once

// ============================================================
// core/MonitorConfig.h
// 作用：
// 1) 定义 Agent 运行时使用的会话配置结构；
// 2) 负责从 Temp\KswordApiMon\config_<pid>.ini 读取监控参数；
// 3) 提供停止标记文件检测，供后台线程轮询退出。
// ============================================================

#include "pch.h"
#include "../WinApiMonitorProtocol.h"

namespace apimon
{
    // MonitorConfig：
    // - 作用：保存当前目标进程 API 监控配置；
    // - 字段：覆盖命名管道名、停止标记路径、各分类启停和详情长度上限。
    struct MonitorConfig
    {
        std::uint32_t targetPid = 0;            // targetPid：当前被注入进程 PID。
        std::wstring configPath;                // configPath：INI 配置文件完整路径。
        std::wstring pipeName;                  // pipeName：命名管道名。
        std::wstring stopFlagPath;              // stopFlagPath：停止标记文件路径。
        std::wstring agentDllPath;              // agentDllPath：当前 APIMonitor_x64.dll 路径，供子进程自动注入复用。
        bool enableFile = true;                 // enableFile：是否启用文件 API Hook。
        bool enableRegistry = true;             // enableRegistry：是否启用注册表 API Hook。
        bool enableNetwork = true;              // enableNetwork：是否启用网络 API Hook。
        bool enableProcess = true;              // enableProcess：是否启用进程 API Hook。
        bool enableLoader = true;               // enableLoader：是否启用加载器 API Hook。
        bool autoInjectChild = false;           // autoInjectChild：CreateProcessW 成功后是否自动注入新子进程。
        std::size_t detailLimitChars = 256;     // detailLimitChars：详情文本截断长度。
        bool valid = false;                     // valid：当前配置是否通过基本校验。
    };

    bool LoadMonitorConfigForCurrentProcess(MonitorConfig* configOut, std::wstring* errorTextOut);
    bool IsStopFlagPresent(const MonitorConfig& configValue);
}
