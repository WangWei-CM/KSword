#include "pch.h"
#include "MonitorConfig.h"

namespace apimon
{
    namespace
    {
        std::wstring QueryIniText(const std::wstring& iniPath, const wchar_t* keyName, const wchar_t* defaultText)
        {
            wchar_t textBuffer[4096] = {};
            const DWORD copiedLength = ::GetPrivateProfileStringW(
                L"monitor",
                keyName,
                defaultText,
                textBuffer,
                static_cast<DWORD>(std::size(textBuffer)),
                iniPath.c_str());
            return std::wstring(textBuffer, copiedLength);
        }

        bool QueryIniBool(const std::wstring& iniPath, const wchar_t* keyName, const bool defaultValue)
        {
            const UINT rawValue = ::GetPrivateProfileIntW(
                L"monitor",
                keyName,
                defaultValue ? 1U : 0U,
                iniPath.c_str());
            return rawValue != 0;
        }
    }

    bool LoadMonitorConfigForCurrentProcess(MonitorConfig* configOut, std::wstring* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }
        if (configOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"configOut is null.";
            }
            return false;
        }

        MonitorConfig configValue;
        configValue.targetPid = static_cast<std::uint32_t>(::GetCurrentProcessId());
        configValue.configPath = ks::winapi_monitor::buildConfigPathForPid(configValue.targetPid);

        const DWORD fileAttributes = ::GetFileAttributesW(configValue.configPath.c_str());
        if (fileAttributes == INVALID_FILE_ATTRIBUTES || (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"Monitor config file does not exist: " + configValue.configPath;
            }
            return false;
        }

        configValue.pipeName = QueryIniText(
            configValue.configPath,
            L"pipe_name",
            ks::winapi_monitor::buildPipeNameForPid(configValue.targetPid).c_str());
        configValue.stopFlagPath = QueryIniText(
            configValue.configPath,
            L"stop_flag_path",
            ks::winapi_monitor::buildStopFlagPathForPid(configValue.targetPid).c_str());
        configValue.enableFile = QueryIniBool(configValue.configPath, L"enable_file", true);
        configValue.enableRegistry = QueryIniBool(configValue.configPath, L"enable_registry", true);
        configValue.enableNetwork = QueryIniBool(configValue.configPath, L"enable_network", true);
        configValue.enableProcess = QueryIniBool(configValue.configPath, L"enable_process", true);
        configValue.enableLoader = QueryIniBool(configValue.configPath, L"enable_loader", true);

        const int rawDetailLimit = static_cast<int>(::GetPrivateProfileIntW(
            L"monitor",
            L"detail_limit",
            static_cast<UINT>(ks::winapi_monitor::kMaxDetailChars - 1),
            configValue.configPath.c_str()));
        configValue.detailLimitChars = static_cast<std::size_t>(std::clamp(
            rawDetailLimit,
            64,
            static_cast<int>(ks::winapi_monitor::kMaxDetailChars - 1)));

        configValue.valid = !configValue.pipeName.empty() && !configValue.stopFlagPath.empty();
        if (!configValue.valid)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"Monitor config missing required pipe_name or stop_flag_path.";
            }
            return false;
        }

        *configOut = configValue;
        return true;
    }

    bool IsStopFlagPresent(const MonitorConfig& configValue)
    {
        if (configValue.stopFlagPath.empty())
        {
            return false;
        }

        const DWORD fileAttributes = ::GetFileAttributesW(configValue.stopFlagPath.c_str());
        return fileAttributes != INVALID_FILE_ATTRIBUTES && (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
}
