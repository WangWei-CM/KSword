#pragma once

// ============================================================
// WindowCaptureProtection.h
// 作用说明：
// 1) 为窗口管理页提供 HWND 级防截图保护封装；
// 2) 本进程窗口直接调用 SetWindowDisplayAffinity；
// 3) 外部 x64 进程窗口通过短远程线程在目标进程内调用。
// ============================================================

#include <cstdint>
#include <string>

namespace ks::window
{
    // DisplayAffinity 常量：
    // - AllowCapture：恢复普通截图/录屏；
    // - MonitorOnly：旧系统回退，截图中显示黑屏；
    // - ExcludeFromCapture：新系统优先策略，截图/录屏中隐藏窗口。
    constexpr std::uint32_t kDisplayAffinityAllowCapture = 0x00000000;
    constexpr std::uint32_t kDisplayAffinityMonitorOnly = 0x00000001;
    constexpr std::uint32_t kDisplayAffinityExcludeFromCapture = 0x00000011;

    // CaptureProtectionResult：
    // - 作用：承载一次防截图保护写入的完整结果；
    // - 调用：OtherDock 根据 success/detail 展示日志和提示。
    struct CaptureProtectionResult
    {
        bool success = false;                       // success：SetWindowDisplayAffinity 是否最终成功。
        bool requestedProtection = false;           // requestedProtection：本次是否请求启用保护。
        bool usedRemoteThread = false;              // usedRemoteThread：是否走跨进程远程线程调用。
        bool usedRootWindow = false;                // usedRootWindow：是否从子窗口归并到根窗口执行。
        std::uint64_t requestedHwnd = 0;             // requestedHwnd：用户选中的原始 HWND。
        std::uint64_t appliedHwnd = 0;               // appliedHwnd：真实写入 DisplayAffinity 的 HWND。
        std::uint32_t processId = 0;                 // processId：目标根窗口所属进程 PID。
        std::uint32_t appliedAffinity = 0;           // appliedAffinity：最终请求/应用的 WDA_* 值。
        std::uint32_t win32Error = 0;                // win32Error：失败时保留 Win32 错误码。
        std::string detail;                         // detail：面向日志/提示的诊断文本。
    };

    // SetWindowCaptureProtection 作用：
    // - 对给定 HWND 启用或取消防截图保护；
    // - 传入 hwndValue：窗口句柄整数值；
    // - 传入 enableProtection：true=启用，false=取消；
    // - 返回 CaptureProtectionResult，调用方负责展示结果。
    CaptureProtectionResult SetWindowCaptureProtection(
        std::uint64_t hwndValue,
        bool enableProtection);

    // QueryWindowDisplayAffinity 作用：
    // - 尝试读取窗口当前 DisplayAffinity；
    // - 传入 hwndValue：窗口句柄整数值；
    // - 传出 affinityOut：成功时写入 WDA_* 值；
    // - 返回 true 表示读取成功，false 表示系统拒绝或窗口不支持。
    bool QueryWindowDisplayAffinity(
        std::uint64_t hwndValue,
        std::uint32_t& affinityOut,
        std::uint32_t* win32ErrorOut);

    // DisplayAffinityName 作用：
    // - 把 WDA_* 数值转换为中文说明；
    // - 调用：UI 摘要、日志、消息框均可复用。
    std::string DisplayAffinityName(std::uint32_t affinityValue);
}
