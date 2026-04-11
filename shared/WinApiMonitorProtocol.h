#pragma once

// ============================================================
// shared/WinApiMonitorProtocol.h
// 作用：
// 1) 定义 Ksword 主程序与 APIMonitor_x64 DLL 共用的协议常量；
// 2) 统一命名管道、配置文件、停止标记文件的命名规则；
// 3) 定义固定长度事件包，避免 UI 与 Agent 在结构布局上漂移。
// ============================================================

#include <cstdint>
#include <iterator>
#include <string>
#include <type_traits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace ks::winapi_monitor
{
    // kProtocolVersion：
    // - 作用：协议版本号；
    // - 调用：UI 和 Agent 在收发事件包时都可用于快速校验结构兼容性。
    inline constexpr std::uint32_t kProtocolVersion = 0x20260405U;

    // kMaxModuleNameChars / kMaxApiNameChars / kMaxDetailChars：
    // - 作用：定义固定长度宽字符缓冲大小；
    // - 说明：采用固定长度是为了便于命名管道直接传输整包结构。
    inline constexpr std::size_t kMaxModuleNameChars = 32;
    inline constexpr std::size_t kMaxApiNameChars = 64;
    inline constexpr std::size_t kMaxDetailChars = 320;

    // EventCategory：
    // - 作用：统一标记 API 事件所属大类；
    // - 调用：UI 侧按 category 渲染颜色、过滤和统计。
    enum class EventCategory : std::uint32_t
    {
        Unknown = 0,
        File = 1,
        Registry = 2,
        Network = 3,
        Process = 4,
        Loader = 5,
        Internal = 6
    };

    // ApiMonitorEventPacket：
    // - 作用：命名管道中传输的固定长度事件包；
    // - 调用：Agent 填充后整包 WriteFile，UI 整包 ReadFile 后直接解析。
    struct ApiMonitorEventPacket
    {
        std::uint32_t size = sizeof(ApiMonitorEventPacket);     // size：当前结构大小。
        std::uint32_t version = kProtocolVersion;               // version：协议版本号。
        std::uint32_t pid = 0;                                  // pid：触发 API 的进程 ID。
        std::uint32_t tid = 0;                                  // tid：触发 API 的线程 ID。
        std::uint64_t timestamp100ns = 0;                       // timestamp100ns：FILETIME 基准时间戳。
        std::uint32_t category = 0;                             // category：EventCategory 数值。
        std::int32_t resultCode = 0;                            // resultCode：Win32/LSTATUS/WSA 错误码。
        wchar_t moduleName[kMaxModuleNameChars] = {};           // moduleName：API 所属模块名。
        wchar_t apiName[kMaxApiNameChars] = {};                 // apiName：API 名称。
        wchar_t detailText[kMaxDetailChars] = {};               // detailText：压缩后的详情文本。
    };

    static_assert(
        std::is_trivially_copyable_v<ApiMonitorEventPacket>,
        "ApiMonitorEventPacket 必须可平凡复制，便于直接通过管道收发。");

    // trimTrailingSlash：
    // - 作用：把目录尾部多余的 '\\' 或 '/' 去掉；
    // - 调用：拼接会话目录、配置文件路径前统一规范化。
    inline std::wstring trimTrailingSlash(const std::wstring& pathText)
    {
        std::wstring normalizedText = pathText;
        while (!normalizedText.empty())
        {
            const wchar_t lastChar = normalizedText.back();
            if (lastChar != L'\\' && lastChar != L'/')
            {
                break;
            }
            normalizedText.pop_back();
        }
        return normalizedText;
    }

    // joinPath：
    // - 作用：用 Windows 风格拼接目录与文件名；
    // - 调用：构造会话目录、INI 路径、停止标记文件路径。
    inline std::wstring joinPath(const std::wstring& leftPath, const std::wstring& rightPath)
    {
        if (leftPath.empty())
        {
            return rightPath;
        }

        const std::wstring normalizedLeft = trimTrailingSlash(leftPath);
        if (normalizedLeft.empty())
        {
            return rightPath;
        }
        return normalizedLeft + L"\\" + rightPath;
    }

    // queryTempDirectory：
    // - 作用：返回当前用户临时目录；
    // - 调用：UI 写配置和停止标记文件、Agent 读取会话配置时复用。
    inline std::wstring queryTempDirectory()
    {
        wchar_t tempBuffer[4096] = {};
        const DWORD charCount = ::GetTempPathW(
            static_cast<DWORD>(std::size(tempBuffer)),
            tempBuffer);
        if (charCount == 0 || charCount >= std::size(tempBuffer))
        {
            return std::wstring(L".");
        }
        return trimTrailingSlash(std::wstring(tempBuffer, charCount));
    }

    // buildSessionDirectory：
    // - 作用：返回 WinAPI 监控会话目录；
    // - 调用：所有 pid 相关的会话文件都放在该目录下，避免散落在 Temp 根目录。
    inline std::wstring buildSessionDirectory()
    {
        return joinPath(queryTempDirectory(), L"KswordApiMon");
    }

    // buildPipeNameForPid：
    // - 作用：按 PID 生成固定命名管道名；
    // - 调用：UI 作为客户端连接，Agent 作为服务端创建。
    inline std::wstring buildPipeNameForPid(const std::uint32_t pidValue)
    {
        return std::wstring(L"\\\\.\\pipe\\KswordApiMon_") + std::to_wstring(pidValue);
    }

    // buildConfigPathForPid：
    // - 作用：按 PID 生成会话 INI 配置文件路径；
    // - 调用：UI 启动监控前写入，Agent 注入后读取。
    inline std::wstring buildConfigPathForPid(const std::uint32_t pidValue)
    {
        return joinPath(
            buildSessionDirectory(),
            std::wstring(L"config_") + std::to_wstring(pidValue) + L".ini");
    }

    // buildStopFlagPathForPid：
    // - 作用：按 PID 生成“停止标记”文件路径；
    // - 调用：UI 停止监控时创建该文件，Agent 后台轮询检测并卸载 Hook。
    inline std::wstring buildStopFlagPathForPid(const std::uint32_t pidValue)
    {
        return joinPath(
            buildSessionDirectory(),
            std::wstring(L"stop_") + std::to_wstring(pidValue) + L".flag");
    }

    // eventCategoryToText：
    // - 作用：把事件分类枚举转换为可读宽字符串；
    // - 调用：UI 渲染表格、Agent 生成内部诊断文本时复用。
    inline std::wstring eventCategoryToText(const EventCategory categoryValue)
    {
        switch (categoryValue)
        {
        case EventCategory::File:
            return L"文件";
        case EventCategory::Registry:
            return L"注册表";
        case EventCategory::Network:
            return L"网络";
        case EventCategory::Process:
            return L"进程";
        case EventCategory::Loader:
            return L"加载器";
        case EventCategory::Internal:
            return L"内部";
        default:
            break;
        }
        return L"未知";
    }
}
