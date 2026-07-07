#include "pch.h"
#include "MonitorAgent.h"
#include "core/MonitorPipe.h"
#include "hook/HookEngine.h"
#include "hook/HookTargets.h"

namespace apimon
{
    namespace
    {
        std::atomic_bool g_stopRequested{ false };     // g_stopRequested：全局停止标志。
        MonitorConfig g_activeConfig{};                // g_activeConfig：当前已加载的会话配置。

        void TraceAgentFailure(const std::wstring& errorText)
        {
            if (errorText.empty())
            {
                return;
            }

            const std::wstring outputText = L"[APIMonitor_x64] " + errorText + L"\n";
            ::OutputDebugStringW(outputText.c_str());
        }

        DWORD WINAPI MonitorWorkerThread(LPVOID parameterValue)
        {
            (void)parameterValue;

            MonitorConfig configValue;
            std::wstring errorText;
            if (!LoadMonitorConfigForCurrentProcess(&configValue, &errorText))
            {
                TraceAgentFailure(errorText);
                return 0;
            }

            ReplaceActiveConfig(configValue);
            if (!StartMonitorPipeServer(configValue, &errorText))
            {
                TraceAgentFailure(errorText);
                return 0;
            }

            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Internal,
                L"Agent",
                L"SessionReady",
                0,
                L"Agent connected and pipe server is ready.");

            if (!InstallConfiguredHooks(&errorText))
            {
                SendMonitorEvent(
                    ks::winapi_monitor::EventCategory::Internal,
                    L"Agent",
                    L"InstallHooksFailed",
                    1,
                    errorText);
                StopMonitorPipeServer();
                return 0;
            }
            if (!errorText.empty())
            {
                SendMonitorEvent(
                    ks::winapi_monitor::EventCategory::Internal,
                    L"Agent",
                    L"HooksPartial",
                    0,
                    errorText);
            }

            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Internal,
                L"Agent",
                L"HooksInstalled",
                0,
                L"Configured inline hooks are now active.");

            // agentBypassScope：
            // - 输入：无；
            // - 处理：Hook 生效后，Agent worker 线程自己的 stop-flag 轮询、事件发送和卸载流程不再进入监控事件流；
            // - 返回：无返回值，作用域持续到 worker 退出。
            // - 原因：内部控制线程不是被测业务线程，监控它会引入 Wait/File/Loader 自递归噪声，并可能放大为退出期崩溃。
            ScopedInlineHookInternalBypass agentBypassScope;
            while (!StopRequested())
            {
                if (IsStopFlagPresent(ActiveConfig()))
                {
                    RequestStop();
                    break;
                }
                ::Sleep(250);
            }

            UninstallConfiguredHooks();
            SendMonitorEvent(
                ks::winapi_monitor::EventCategory::Internal,
                L"Agent",
                L"HooksRemoved",
                0,
                L"Inline hooks removed and agent will disconnect.");
            StopMonitorPipeServer();
            return 0;
        }
    }

    void OnProcessAttach(const HMODULE moduleHandle)
    {
        ::DisableThreadLibraryCalls(moduleHandle);
        g_stopRequested.store(false);

        HANDLE workerHandle = ::CreateThread(
            nullptr,
            0,
            &MonitorWorkerThread,
            nullptr,
            0,
            nullptr);
        if (workerHandle != nullptr)
        {
            ::CloseHandle(workerHandle);
        }
        else
        {
            TraceAgentFailure(L"CreateThread for monitor worker failed.");
        }
    }

    void OnProcessDetach()
    {
        RequestStop();
    }

    const MonitorConfig& ActiveConfig()
    {
        return g_activeConfig;
    }

    void ReplaceActiveConfig(const MonitorConfig& configValue)
    {
        g_activeConfig = configValue;
    }

    bool StopRequested()
    {
        return g_stopRequested.load();
    }

    void RequestStop()
    {
        g_stopRequested.store(true);
    }
}
