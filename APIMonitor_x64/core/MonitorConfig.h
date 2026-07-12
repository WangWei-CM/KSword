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
    enum class FakeSuccessReturnType : std::uint32_t
    {
        Scalar = 0,
        Bool = 1,
        Handle = 2,
        Dword = 3,
        NtStatus = 4,
        HResult = 5,
        LStatus = 6,
        SocketInt = 7
    };

    enum class FakeSuccessLastErrorKind : std::uint32_t
    {
        None = 0,
        Win32 = 1,
        Wsa = 2
    };

    struct FakeSuccessRule
    {
        std::wstring moduleName;                                    // moduleName：规则匹配的模块名，含或不含 .dll 均可由匹配层规范化。
        std::wstring apiName;                                       // apiName：规则匹配的导出 API 名，大小写不敏感。
        FakeSuccessReturnType returnType = FakeSuccessReturnType::Scalar; // returnType：UI 模板选择后的返回语义。
        std::uint64_t returnValue = 0;                              // returnValue：写入 RAX 的标量返回值。
        FakeSuccessLastErrorKind lastErrorKind = FakeSuccessLastErrorKind::None; // lastErrorKind：是否设置 Win32/WSA 错误码。
        std::uint32_t lastErrorValue = 0;                           // lastErrorValue：SetLastError/WSASetLastError 的值。
    };

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
        bool enableRawFallback = false;         // enableRawFallback：是否对强类型表之外的导出安装 Raw ABI 兜底 Hook。
        bool rawUseDefaultDenyList = true;      // rawUseDefaultDenyList：是否启用内置高频/高风险 API 黑名单。
        std::vector<std::wstring> rawModuleList;// rawModuleList：Raw 兜底扫描的模块名目录。
        std::vector<std::wstring> rawDenyList;  // rawDenyList：用户额外 Raw Hook 黑名单；默认黑名单由 rawUseDefaultDenyList 单独控制。
        bool fakeSuccessEnabled = false;        // fakeSuccessEnabled：是否启用 Fake Success 规则，让命中 API 跳过原函数。
        bool fakeSuccessRawFallback = false;    // fakeSuccessRawFallback：是否允许未强类型覆盖 API 使用通用 RAX fake-return stub。
        std::vector<FakeSuccessRule> fakeSuccessRules; // fakeSuccessRules：按 module!api 精确匹配的伪返回规则。
        std::wstring fakeSuccessRulesText;      // fakeSuccessRulesText：原始规则文本，用于自动注入子进程继承。
        std::size_t detailLimitChars = 256;     // detailLimitChars：详情文本截断长度。
        bool valid = false;                     // valid：当前配置是否通过基本校验。
    };

    bool LoadMonitorConfigForCurrentProcess(MonitorConfig* configOut, std::wstring* errorTextOut);
    bool IsStopFlagPresent(const MonitorConfig& configValue);
}
