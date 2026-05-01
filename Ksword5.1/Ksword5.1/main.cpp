#include "MainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include "Framework.h"
#include "Framework/ThemedMessageBox.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace
{
    constexpr wchar_t kUnlockerKeyName[] = L"Ksword.FileUnlocker";

    // kStartupScaleRecommendedLogicalWidth 作用：
    // - 定义启动分辨率审查的最低建议逻辑宽度；
    // - 低于该宽度时才弹出缩放推荐对话框。
    constexpr int kStartupScaleRecommendedLogicalWidth = 1920;

    // queryCurrentDirectoryPath 作用：
    // - 在 QApplication 创建前直接读取当前工作目录；
    // - 用于启动追踪中确认双击启动时的工作目录是否符合预期。
    // 返回：当前工作目录绝对路径；失败返回空字符串。
    std::wstring queryCurrentDirectoryPath()
    {
        std::vector<wchar_t> directoryBuffer(1024, L'\0');
        while (directoryBuffer.size() < 32768)
        {
            const DWORD copiedLength = ::GetCurrentDirectoryW(
                static_cast<DWORD>(directoryBuffer.size()),
                directoryBuffer.data());
            if (copiedLength == 0)
            {
                return std::wstring();
            }
            if (copiedLength < directoryBuffer.size())
            {
                return std::wstring(directoryBuffer.data(), copiedLength);
            }
            directoryBuffer.resize(static_cast<std::size_t>(copiedLength) + 1, L'\0');
        }
        return std::wstring();
    }

    // buildStartupTraceFilePath 作用：
    // - 为“启动原生追踪”选择稳定输出文件；
    // - 优先写入 %TEMP%，便于即使控制台秒退也能保留日志。
    // 返回：追踪日志文件完整路径。
    std::wstring buildStartupTraceFilePath()
    {
        wchar_t tempPathBuffer[MAX_PATH] = {};
        const DWORD tempLength = ::GetTempPathW(MAX_PATH, tempPathBuffer);
        if (tempLength > 0 && tempLength < MAX_PATH)
        {
            std::wstring tempDirectory(tempPathBuffer, tempLength);
            if (!tempDirectory.empty() && tempDirectory.back() != L'\\' && tempDirectory.back() != L'/')
            {
                tempDirectory.push_back(L'\\');
            }
            return tempDirectory + L"Ksword5.1-startup-trace.log";
        }

        const std::wstring currentDirectory = queryCurrentDirectoryPath();
        if (!currentDirectory.empty())
        {
            return currentDirectory + L"\\Ksword5.1-startup-trace.log";
        }
        return L"Ksword5.1-startup-trace.log";
    }

    // appendStartupTraceFile 作用：
    // - 把原生启动追踪文本附加写入文件；
    // - 避免控制台窗口闪退时丢失关键定位信息。
    // 入参 traceLineText：已带换行的一整行追踪文本（UTF-8）。
    void appendStartupTraceFile(const std::string& traceLineText)
    {
        const std::wstring traceFilePath = buildStartupTraceFilePath();
        FILE* traceFileHandle = nullptr;
        if (_wfopen_s(&traceFileHandle, traceFilePath.c_str(), L"ab") != 0 || traceFileHandle == nullptr)
        {
            return;
        }

        std::fwrite(traceLineText.data(), 1, traceLineText.size(), traceFileHandle);
        std::fclose(traceFileHandle);
    }

    // startupTraceRaw 作用：
    // - 在 kLog 之外提供一条“原生启动追踪”旁路；
    // - 同时写控制台、OutputDebugStringA 与临时文件；
    // - 用于定位“main 早期即退出，kLog 尚未来得及显示”的问题。
    // 入参 traceText：单行追踪文本（不含换行）。
    void startupTraceRaw(const std::string& traceText)
    {
        SYSTEMTIME localTime = {};
        ::GetLocalTime(&localTime);

        char timePrefixBuffer[64] = {};
        std::snprintf(
            timePrefixBuffer,
            sizeof(timePrefixBuffer),
            "[trace][%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            static_cast<unsigned int>(localTime.wYear),
            static_cast<unsigned int>(localTime.wMonth),
            static_cast<unsigned int>(localTime.wDay),
            static_cast<unsigned int>(localTime.wHour),
            static_cast<unsigned int>(localTime.wMinute),
            static_cast<unsigned int>(localTime.wSecond),
            static_cast<unsigned int>(localTime.wMilliseconds));

        std::string fullTraceLine = std::string(timePrefixBuffer) + traceText + "\r\n";
        appendStartupTraceFile(fullTraceLine);
        ::OutputDebugStringA(fullTraceLine.c_str());

    }

    // queryCurrentExecutablePath 作用：
    // - 动态获取当前进程可执行文件绝对路径；
    // - 兼容长路径，避免固定 MAX_PATH 缓冲区截断问题。
    // 返回：成功返回非空绝对路径，失败返回空字符串。
    std::wstring queryCurrentExecutablePath()
    {
        std::vector<wchar_t> pathBuffer(1024, L'\0');
        while (pathBuffer.size() < 32768)
        {
            ::SetLastError(ERROR_SUCCESS);
            const DWORD copiedLength = ::GetModuleFileNameW(
                nullptr,
                pathBuffer.data(),
                static_cast<DWORD>(pathBuffer.size()));
            const DWORD lastError = ::GetLastError();
            if (copiedLength == 0)
            {
                return std::wstring();
            }

            if (copiedLength > 0
                && copiedLength < pathBuffer.size()
                && lastError != ERROR_INSUFFICIENT_BUFFER)
            {
                return std::wstring(pathBuffer.data(), copiedLength);
            }

            pathBuffer.resize(pathBuffer.size() * 2, L'\0');
        }
        return std::wstring();
    }

    // resolveExecutableDirectoryPath 作用：
    // - 从 exe 完整路径解析工作目录；
    // - CreateProcess 显式传入目录，避免自动重启后当前目录漂移。
    // 入参 executablePath：当前 exe 绝对路径。
    // 返回：exe 所在目录，无法解析时返回空字符串。
    std::wstring resolveExecutableDirectoryPath(const std::wstring& executablePath)
    {
        const std::size_t slashPosition = executablePath.find_last_of(L"\\/");
        if (slashPosition == std::wstring::npos)
        {
            return std::wstring();
        }
        return executablePath.substr(0, slashPosition);
    }

    bool writeRegistryString(
        HKEY rootKey,
        const std::wstring& subKeyPath,
        const wchar_t* valueName,
        const std::wstring& valueText)
    {
        HKEY keyHandle = nullptr;
        const LONG createResult = ::RegCreateKeyExW(
            rootKey,
            subKeyPath.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &keyHandle,
            nullptr);
        if (createResult != ERROR_SUCCESS)
        {
            return false;
        }

        const DWORD valueSizeBytes = static_cast<DWORD>((valueText.size() + 1) * sizeof(wchar_t));
        const LONG setResult = ::RegSetValueExW(
            keyHandle,
            valueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(valueText.c_str()),
            valueSizeBytes);
        ::RegCloseKey(keyHandle);
        return setResult == ERROR_SUCCESS;
    }

    void deleteRegistryTreeBestEffort(HKEY rootKey, const std::wstring& subKeyPath)
    {
        ::RegDeleteTreeW(rootKey, subKeyPath.c_str());
    }

    bool registerUnlockerContextMenu(const std::wstring& executablePath)
    {
        const std::wstring commandForFile = L"\"" + executablePath + L"\" --unlock \"%1\"";
        const std::wstring commandForBackground = L"\"" + executablePath + L"\" --unlock \"%V\"";

        const std::wstring baseStar = L"Software\\Classes\\*\\shell\\" + std::wstring(kUnlockerKeyName);
        const std::wstring baseDirectory = L"Software\\Classes\\Directory\\shell\\" + std::wstring(kUnlockerKeyName);
        const std::wstring baseDrive = L"Software\\Classes\\Drive\\shell\\" + std::wstring(kUnlockerKeyName);
        const std::wstring baseBackground =
            L"Software\\Classes\\Directory\\Background\\shell\\" + std::wstring(kUnlockerKeyName);

        const bool starOk =
            writeRegistryString(HKEY_CURRENT_USER, baseStar, nullptr, L"使用 Ksword 文件解锁器(R3/R0)")
            && writeRegistryString(HKEY_CURRENT_USER, baseStar, L"Icon", executablePath)
            && writeRegistryString(HKEY_CURRENT_USER, baseStar + L"\\command", nullptr, commandForFile);
        const bool directoryOk =
            writeRegistryString(HKEY_CURRENT_USER, baseDirectory, nullptr, L"使用 Ksword 文件解锁器(R3/R0)")
            && writeRegistryString(HKEY_CURRENT_USER, baseDirectory, L"Icon", executablePath)
            && writeRegistryString(HKEY_CURRENT_USER, baseDirectory + L"\\command", nullptr, commandForFile);
        const bool driveOk =
            writeRegistryString(HKEY_CURRENT_USER, baseDrive, nullptr, L"使用 Ksword 文件解锁器(R3/R0)")
            && writeRegistryString(HKEY_CURRENT_USER, baseDrive, L"Icon", executablePath)
            && writeRegistryString(HKEY_CURRENT_USER, baseDrive + L"\\command", nullptr, commandForFile);
        const bool backgroundOk =
            writeRegistryString(HKEY_CURRENT_USER, baseBackground, nullptr, L"使用 Ksword 文件解锁器(R3/R0)")
            && writeRegistryString(HKEY_CURRENT_USER, baseBackground, L"Icon", executablePath)
            && writeRegistryString(HKEY_CURRENT_USER, baseBackground + L"\\command", nullptr, commandForBackground);

        return starOk && directoryOk && driveOk && backgroundOk;
    }

    void unregisterUnlockerContextMenu()
    {
        deleteRegistryTreeBestEffort(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\*\\shell\\" + std::wstring(kUnlockerKeyName));
        deleteRegistryTreeBestEffort(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\Directory\\shell\\" + std::wstring(kUnlockerKeyName));
        deleteRegistryTreeBestEffort(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\Drive\\shell\\" + std::wstring(kUnlockerKeyName));
        deleteRegistryTreeBestEffort(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\Directory\\Background\\shell\\" + std::wstring(kUnlockerKeyName));
    }

    // kNativeAppIconResourceName 作用：
    // - 指向 AppIcon.rc 内声明的主图标资源名；
    // - 用于把原生图标同步到 Qt 主窗口标题栏和任务栏。
    constexpr wchar_t kNativeAppIconResourceName[] = L"IDI_APP_ICON";

    // isCurrentProcessElevated 作用：
    // - 判断当前进程是否已经运行在管理员权限下；
    // - 供“启动时自动请求管理员权限”逻辑复用。
    // 返回：true=已提权；false=普通权限。
    bool isCurrentProcessElevated()
    {
        HANDLE tokenHandle = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
        {
            return false;
        }

        // tokenElevation 作用：承接 TokenElevation 查询结果。
        TOKEN_ELEVATION tokenElevation{};
        DWORD returnLength = 0;
        const BOOL queryOk = ::GetTokenInformation(
            tokenHandle,
            TokenElevation,
            &tokenElevation,
            sizeof(tokenElevation),
            &returnLength);
        ::CloseHandle(tokenHandle);
        return queryOk != FALSE && tokenElevation.TokenIsElevated != 0;
    }

    // extractCurrentProcessParameterText 作用：
    // - 从当前命令行提取“exe 之后”的参数文本；
    // - runas 重启时原样透传，避免行为差异。
    // 返回：可传入 ShellExecuteW 的参数字符串。
    std::wstring extractCurrentProcessParameterText()
    {
        const wchar_t* commandLineText = ::GetCommandLineW();
        if (commandLineText == nullptr)
        {
            return std::wstring();
        }

        // cursorPointer 作用：遍历命令行并定位到第一个参数起始位置。
        const wchar_t* cursorPointer = commandLineText;
        bool insideQuotes = false;
        while (*cursorPointer != L'\0')
        {
            if (*cursorPointer == L'"')
            {
                insideQuotes = !insideQuotes;
            }
            else if (!insideQuotes && std::iswspace(static_cast<wint_t>(*cursorPointer)) != 0)
            {
                break;
            }
            ++cursorPointer;
        }

        while (std::iswspace(static_cast<wint_t>(*cursorPointer)) != 0)
        {
            ++cursorPointer;
        }

        return std::wstring(cursorPointer);
    }

    // tryLaunchElevatedSelfBeforeSplash 作用：
    // - 在启动页出现前尝试 runas 拉起管理员实例；
    // - 成功则当前普通权限实例直接退出，避免双开。
    // 返回：true=管理员实例已拉起；false=失败，继续普通启动。
    bool tryLaunchElevatedSelfBeforeSplash()
    {
        const std::wstring executablePath = queryCurrentExecutablePath();
        if (executablePath.empty())
        {
            return false;
        }

        // parameterText 作用：保存当前命令行参数，传递给新启动实例。
        const std::wstring parameterText = extractCurrentProcessParameterText();
        const std::wstring executableDirectory = resolveExecutableDirectoryPath(executablePath);
        HINSTANCE shellResult = ::ShellExecuteW(
            nullptr,
            L"runas",
            executablePath.c_str(),
            parameterText.empty() ? nullptr : parameterText.c_str(),
            executableDirectory.empty() ? nullptr : executableDirectory.c_str(),
            SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(shellResult) > 32;
    }

    // tryCreateRestartedSelfBeforeSplash 作用：
    // - 在 QApplication 创建前使用 CreateProcessW 重新启动当前程序；
    // - 只依赖配置文件承载缩放结果，不通过参数传递缩放状态；
    // - 当前进程只负责启动新实例，退出由 main 根据返回值执行。
    // 返回：true=新实例已拉起；false=启动失败，调用方继续当前启动流程。
    bool tryCreateRestartedSelfBeforeSplash()
    {
        const std::wstring executablePath = queryCurrentExecutablePath();
        if (executablePath.empty())
        {
            return false;
        }

        // commandLineText 作用：CreateProcessW 要求可写命令行缓冲区。
        std::wstring commandLineText = L"\"" + executablePath + L"\"";
        const std::wstring parameterText = extractCurrentProcessParameterText();
        if (!parameterText.empty())
        {
            commandLineText += L" ";
            commandLineText += parameterText;
        }

        const std::wstring executableDirectory = resolveExecutableDirectoryPath(executablePath);
        STARTUPINFOW startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_SHOWNORMAL;

        PROCESS_INFORMATION processInformation = {};
        const BOOL createOk = ::CreateProcessW(
            executablePath.c_str(),
            &commandLineText[0],
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            executableDirectory.empty() ? nullptr : executableDirectory.c_str(),
            &startupInfo,
            &processInformation);
        if (createOk == FALSE)
        {
            return false;
        }

        ::CloseHandle(processInformation.hThread);
        ::CloseHandle(processInformation.hProcess);
        return true;
    }

    // initializeProcessDpiAwareness 作用：
    // - 在主函数最早阶段设置进程 DPI 感知；
    // - 保证启动页与 Qt 主窗口的缩放行为稳定。
    void initializeProcessDpiAwareness()
    {
        HMODULE user32ModuleHandle = ::GetModuleHandleW(L"user32.dll");
        if (user32ModuleHandle != nullptr)
        {
            using SetDpiAwarenessContextFunction = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            const SetDpiAwarenessContextFunction setContextFunction =
                reinterpret_cast<SetDpiAwarenessContextFunction>(
                    ::GetProcAddress(user32ModuleHandle, "SetProcessDpiAwarenessContext"));
            if (setContextFunction != nullptr)
            {
                if (setContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                {
                    return;
                }
                if (setContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
                {
                    return;
                }
            }
        }

        // 旧系统回退方案：至少启用系统 DPI 感知。
        ::SetProcessDPIAware();
    }

    // querySystemScalePercent 作用：
    // - 读取当前系统缩放百分比（100/125/150...）；
    // - 新系统优先 GetDpiForSystem，旧系统回退 GDI 计算。
    // 返回：系统缩放百分比（最小 100）。
    int querySystemScalePercent()
    {
        HMODULE user32ModuleHandle = ::GetModuleHandleW(L"user32.dll");
        if (user32ModuleHandle != nullptr)
        {
            using GetDpiForSystemFunction = UINT(WINAPI*)();
            const GetDpiForSystemFunction getDpiForSystemFunction =
                reinterpret_cast<GetDpiForSystemFunction>(
                    ::GetProcAddress(user32ModuleHandle, "GetDpiForSystem"));
            if (getDpiForSystemFunction != nullptr)
            {
                const UINT dpiValue = getDpiForSystemFunction();
                if (dpiValue >= 96U)
                {
                    const int scalePercent = static_cast<int>(
                        std::lround((static_cast<double>(dpiValue) * 100.0) / 96.0));
                    return std::max(100, scalePercent);
                }
            }
        }

        HDC screenDeviceContext = ::GetDC(nullptr);
        if (screenDeviceContext == nullptr)
        {
            return 100;
        }
        const int logPixelsX = ::GetDeviceCaps(screenDeviceContext, LOGPIXELSX);
        ::ReleaseDC(nullptr, screenDeviceContext);
        if (logPixelsX <= 0)
        {
            return 100;
        }
        const int scalePercent = static_cast<int>(
            std::lround((static_cast<double>(logPixelsX) * 100.0) / 96.0));
        return std::max(100, scalePercent);
    }

    // buildPercentText 作用：
    // - 把比例值转为百分比文本；
    // - 供启动前推荐缩放弹窗显示。
    // 入参 ratioValue：比例值（1.0=100%）。
    // 返回：宽字符串百分比文本。
    std::wstring buildPercentText(const double ratioValue)
    {
        const int percentValue = static_cast<int>(std::lround(ratioValue * 100.0));
        return std::to_wstring(percentValue) + L"%";
    }

    // showStartupScaleRecommendationDialog 作用：
    // - 在首次低分辨率启动前提示是否应用推荐缩放；
    // - 仅提供“应用推荐缩放”与“保持当前缩放”两种结果。
    // 入参 logicalClientWidth：按“物理像素/系统缩放”得到的可用宽度。
    // 入参 currentScaleFactor：当前配置缩放因子。
    // 入参 recommendedScaleFactor：推荐缩放因子。
    // 出参 applyRecommendedOut：是否应用推荐值。
    // 返回：true=弹窗成功返回；false=弹窗调用失败。
    bool showStartupScaleRecommendationDialog(
        const int logicalClientWidth,
        const double currentScaleFactor,
        const double recommendedScaleFactor,
        bool* applyRecommendedOut)
    {
        if (applyRecommendedOut == nullptr)
        {
            return false;
        }
        *applyRecommendedOut = false;

        const std::wstring currentScaleText = buildPercentText(currentScaleFactor);
        const std::wstring recommendedScaleText = buildPercentText(recommendedScaleFactor);
        const std::wstring dialogContentText =
            L"当前可用宽度约 " + std::to_wstring(logicalClientWidth) + L"px，小于推荐的 "
            + std::to_wstring(kStartupScaleRecommendedLogicalWidth) + L"px。\n"
            L"当前缩放：" + currentScaleText + L"\n"
            L"推荐缩放：" + recommendedScaleText + L"\n\n"
            L"是否应用推荐缩放？Ksword 将保存设置并立即重启。";

        // dialogButtons 作用：TaskDialog 自定义按钮集合。
        TASKDIALOG_BUTTON dialogButtons[] =
        {
            { IDYES, L"应用并重启" },
            { IDNO, L"保持当前设置" }
        };

        TASKDIALOGCONFIG dialogConfig = {};
        dialogConfig.cbSize = sizeof(dialogConfig);
        dialogConfig.hInstance = ::GetModuleHandleW(nullptr);
        dialogConfig.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        dialogConfig.pszWindowTitle = L"Ksword5.1 启动缩放建议";
        dialogConfig.pszMainInstruction = L"检测到当前显示可用宽度偏小";
        dialogConfig.pszContent = dialogContentText.c_str();
        dialogConfig.pButtons = dialogButtons;
        dialogConfig.cButtons = ARRAYSIZE(dialogButtons);
        dialogConfig.nDefaultButton = IDYES;

        int pressedButtonId = 0;
        const HRESULT dialogResult = ::TaskDialogIndirect(
            &dialogConfig,
            &pressedButtonId,
            nullptr,
            nullptr);
        if (FAILED(dialogResult))
        {
            return false;
        }

        *applyRecommendedOut = (pressedButtonId == IDYES);
        return true;
    }

    // maybeApplyStartupScaleRecommendation 作用：
    // - 仅在“没有检测到配置文件”且低分辨率时提示推荐缩放；
    // - 把用户选择直接写回 startupSettings，随后由当前进程继续完成 Qt 初始化；
    // - 最佳努力保存配置文件，但保存失败时也不能阻断主窗口启动。
    // 调用方式：QApplication 创建前调用。
    // 入参 startupSettings：启动配置对象（按需被更新）。
    // 返回：true=当前进程已被其它实例接管，应立即退出；false=继续当前启动流程。
    bool maybeApplyStartupScaleRecommendation(ks::settings::AppearanceSettings* startupSettings)
    {
        startupTraceRaw("enter maybeApplyStartupScaleRecommendation");
        if (startupSettings == nullptr)
        {
            startupTraceRaw("maybeApplyStartupScaleRecommendation: startupSettings is null");
            return false;
        }

        // scaleDecisionEvent 作用：串联启动缩放决策链路日志。
        kLogEvent scaleDecisionEvent;
        if (ks::settings::settingsJsonFileExistsForRead())
        {
            startupTraceRaw("maybeApplyStartupScaleRecommendation: settings file exists, skip first-run prompt");
            info << scaleDecisionEvent
                << "[main] 已检测到配置文件，按配置文件直接启动。"
                << eol;
            return false;
        }

        const int physicalScreenWidth = std::max(1, ::GetSystemMetrics(SM_CXSCREEN));
        const int systemScalePercent = querySystemScalePercent();
        const int logicalClientWidth = static_cast<int>(
            std::lround((static_cast<double>(physicalScreenWidth) * 100.0) / static_cast<double>(systemScalePercent)));
        if (logicalClientWidth >= kStartupScaleRecommendedLogicalWidth)
        {
            startupTraceRaw(
                std::string("maybeApplyStartupScaleRecommendation: logical width >= threshold, logicalWidth=")
                + std::to_string(logicalClientWidth)
                + ", threshold="
                + std::to_string(kStartupScaleRecommendedLogicalWidth));
            info << scaleDecisionEvent
                << "[main] 可用宽度满足要求，跳过推荐缩放。 logicalWidth="
                << logicalClientWidth
                << ", systemScalePercent="
                << systemScalePercent
                << eol;
            return false;
        }

        const double currentScaleFactor = ks::settings::normalizeWindowScaleFactor(
            startupSettings->startupWindowScaleFactor);
        const double recommendedScaleFactor = ks::settings::normalizeWindowScaleFactor(
            std::min(
                1.0,
                static_cast<double>(logicalClientWidth) / static_cast<double>(kStartupScaleRecommendedLogicalWidth)));
        if (recommendedScaleFactor >= currentScaleFactor - 0.0001)
        {
            startupTraceRaw(
                std::string("maybeApplyStartupScaleRecommendation: recommended scale not smaller than current, current=")
                + std::to_string(currentScaleFactor)
                + ", recommended="
                + std::to_string(recommendedScaleFactor));
            info << scaleDecisionEvent
                << "[main] 当前缩放已不大于推荐值，无需提示。 current="
                << currentScaleFactor
                << ", recommended="
                << recommendedScaleFactor
                << eol;
            return false;
        }

        bool applyRecommendedScale = false;
        const bool promptHandled = showStartupScaleRecommendationDialog(
            logicalClientWidth,
            currentScaleFactor,
            recommendedScaleFactor,
            &applyRecommendedScale);
        startupTraceRaw(
            std::string("maybeApplyStartupScaleRecommendation: dialog returned, handled=")
            + (promptHandled ? "true" : "false")
            + ", applyRecommendedScale="
            + (applyRecommendedScale ? "true" : "false"));
        if (!promptHandled)
        {
            warn << scaleDecisionEvent
                << "[main] 首次启动缩放推荐弹窗调用失败，改为按当前缩放继续启动。"
                << eol;
            return false;
        }

        // 直接更新内存中的启动配置：
        // - “应用推荐缩放”会让后续 QT_SCALE_FACTOR 使用推荐值；
        // - “保持当前设置”则沿用当前缩放，但同样落盘首启配置，避免重复弹窗。
        startupSettings->startupWindowScaleFactor = applyRecommendedScale
            ? recommendedScaleFactor
            : currentScaleFactor;
        startupSettings->startupScaleRecommendPromptDisabled = false;

        QString saveErrorText;
        const bool saveOk = ks::settings::saveAppearanceSettings(*startupSettings, &saveErrorText);
        startupTraceRaw(
            std::string("maybeApplyStartupScaleRecommendation: saveAppearanceSettings returned ")
            + (saveOk ? "true" : "false"));
        if (!saveOk)
        {
            // 保存失败只影响“下次启动是否仍会再次提示”：
            // - 当前这次启动仍可继续；
            // - 不能因为配置文件写入失败就让主窗口完全不出来。
            warn << scaleDecisionEvent
                << "[main] 首次启动缩放配置保存失败，本次按内存中的缩放继续启动, error="
                << saveErrorText.toStdString()
                << eol;
            return false;
        }

        info << scaleDecisionEvent
            << "[main] 首次启动缩放配置已写入, logicalWidth="
            << logicalClientWidth
            << ", current="
            << currentScaleFactor
            << ", recommended="
            << recommendedScaleFactor
            << ", applied="
            << (applyRecommendedScale ? "true" : "false")
            << eol;

        // 当前函数运行时 QApplication 尚未创建：
        // - 后续 main 仍会根据 startupSettings 设置 QT_SCALE_FACTOR；
        // - 因此这里无需重启，直接继续当前启动链即可。
        info << scaleDecisionEvent
            << "[main] 首次启动缩放决策完成，继续当前实例启动。"
            << eol;
        startupTraceRaw("leave maybeApplyStartupScaleRecommendation with false");
        return false;
    }

    // applyQtScaleFactorEnvironment 作用：
    // - 在 QApplication 创建前设置 QT_SCALE_FACTOR；
    // - 该值会参与整个 Qt 主窗口缩放计算。
    // 入参 scaleFactor：目标缩放因子（0.50~2.00）。
    void applyQtScaleFactorEnvironment(const double scaleFactor)
    {
        const double normalizedScaleFactor = ks::settings::normalizeWindowScaleFactor(scaleFactor);
        const QByteArray scaleFactorText = QByteArray::number(normalizedScaleFactor, 'f', 3);
        qputenv("QT_SCALE_FACTOR", scaleFactorText);

        kLogEvent scaleEnvEvent;
        info << scaleEnvEvent
            << "[main] 已设置 QT_SCALE_FACTOR="
            << scaleFactorText.toStdString()
            << eol;
    }

    // loadSharedIconFromResource 作用：
    // - 按指定尺寸从可执行文件资源加载共享图标；
    // - 供主窗口设置任务栏/标题栏图标。
    // 入参 widthValue/heightValue：目标图标尺寸。
    // 返回：图标句柄；失败返回 nullptr。
    HICON loadSharedIconFromResource(const int widthValue, const int heightValue)
    {
        return reinterpret_cast<HICON>(
            ::LoadImageW(
                ::GetModuleHandleW(nullptr),
                kNativeAppIconResourceName,
                IMAGE_ICON,
                widthValue,
                heightValue,
                LR_DEFAULTCOLOR | LR_SHARED));
    }

    // applyNativeAppIconToWidget 作用：
    // - 把 AppIcon.rc 图标显式同步到 Qt 顶层窗口；
    // - 修复“窗口图标未跟随 exe 图标”的问题。
    // 入参 targetWidget：目标顶层窗口。
    void applyNativeAppIconToWidget(QWidget* targetWidget)
    {
        if (targetWidget == nullptr)
        {
            return;
        }

        const HWND targetWindowHandle = reinterpret_cast<HWND>(targetWidget->winId());
        if (targetWindowHandle == nullptr)
        {
            return;
        }

        const int bigIconWidth = ::GetSystemMetrics(SM_CXICON);
        const int bigIconHeight = ::GetSystemMetrics(SM_CYICON);
        const int smallIconWidth = ::GetSystemMetrics(SM_CXSMICON);
        const int smallIconHeight = ::GetSystemMetrics(SM_CYSMICON);

        HICON bigIconHandle = loadSharedIconFromResource(bigIconWidth, bigIconHeight);
        HICON smallIconHandle = loadSharedIconFromResource(smallIconWidth, smallIconHeight);

        if (bigIconHandle != nullptr)
        {
            ::SendMessageW(
                targetWindowHandle,
                WM_SETICON,
                static_cast<WPARAM>(ICON_BIG),
                reinterpret_cast<LPARAM>(bigIconHandle));
        }
        if (smallIconHandle != nullptr)
        {
            ::SendMessageW(
                targetWindowHandle,
                WM_SETICON,
                static_cast<WPARAM>(ICON_SMALL),
                reinterpret_cast<LPARAM>(smallIconHandle));
            ::SendMessageW(
                targetWindowHandle,
                WM_SETICON,
                static_cast<WPARAM>(ICON_SMALL2),
                reinterpret_cast<LPARAM>(smallIconHandle));
        }
    }

    // FirstFrameSplashHider 作用：
    // - 监听主窗口首帧相关事件（Show/Paint/UpdateRequest）；
    // - 一旦确认主窗口已可见，立即隐藏启动页；
    // - 提供定时兜底，规避平台消息差异导致的漏判。
    class FirstFrameSplashHider final : public QObject
    {
    public:
        // 构造函数作用：
        // - 保存主窗口和启动页控制器指针；
        // - 后续由 eventFilter 与定时器统一执行隐藏。
        // 入参 targetWindow：主窗口对象。
        // 入参 splashWindow：启动页控制器对象。
        FirstFrameSplashHider(
            MainWindow* targetWindow,
            kStartupSplash* splashWindow)
            : QObject(targetWindow)
            , m_targetWindow(targetWindow)
            , m_targetCentralWidget(targetWindow != nullptr ? targetWindow->centralWidget() : nullptr)
            , m_splashWindow(splashWindow)
        {
            // 启动兜底：窗口进入可见状态后若事件链漏判，1.5 秒后也强制收起启动页。
            QTimer::singleShot(1500, this, [this]()
                {
                    if (m_hidden || m_targetWindow == nullptr)
                    {
                        return;
                    }
                    if (m_targetWindow->isVisible() && !m_targetWindow->isMinimized())
                    {
                        hideSplashAndDetach();
                    }
                });
        }

    protected:
        // eventFilter 作用：
        // - 捕获首帧相关事件后隐藏启动页；
        // - 同时监听主窗口与中央控件，避免仅监听主窗导致漏判。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (m_hidden || eventObject == nullptr || m_targetWindow == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            // watchedTargetWindow 作用：判断本次事件是否来自主窗口。
            const bool watchedTargetWindow = (watchedObject == m_targetWindow);
            // watchedTargetCentral 作用：判断本次事件是否来自中央内容控件。
            const bool watchedTargetCentral = (watchedObject == m_targetCentralWidget);
            if (!watchedTargetWindow && !watchedTargetCentral)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            // eventType 作用：统一承接本次事件类型，便于扩展首帧判定条件。
            const QEvent::Type eventType = eventObject->type();
            // isFrameReadySignal 作用：判定是否是“窗口首帧已到”相关事件。
            const bool isFrameReadySignal =
                eventType == QEvent::Show ||
                eventType == QEvent::Paint ||
                eventType == QEvent::UpdateRequest;

            if (!m_hidden
                && isFrameReadySignal
                && m_targetWindow->isVisible()
                && !m_targetWindow->isMinimized())
            {
                hideSplashAndDetach();
            }
            return QObject::eventFilter(watchedObject, eventObject);
        }

    private:
        // hideSplashAndDetach 作用：
        // - 执行一次性隐藏动作并移除事件过滤器；
        // - 保证启动页只隐藏一次，避免重复调用。
        void hideSplashAndDetach()
        {
            m_hidden = true;
            if (m_splashWindow != nullptr)
            {
                m_splashWindow->progress("启动完成", 100);
                m_splashWindow->hide();
            }
            if (m_targetWindow != nullptr)
            {
                m_targetWindow->removeEventFilter(this);
            }
            if (m_targetCentralWidget != nullptr)
            {
                m_targetCentralWidget->removeEventFilter(this);
            }
        }

        MainWindow* m_targetWindow = nullptr;     // m_targetWindow：主窗口对象指针。
        QWidget* m_targetCentralWidget = nullptr; // m_targetCentralWidget：主窗口中央内容控件指针。
        kStartupSplash* m_splashWindow = nullptr; // m_splashWindow：启动页控制器指针。
        bool m_hidden = false;                    // m_hidden：是否已经执行过隐藏动作。
    };
}

int main(int argc, char* argv[])
{
    // 启动流程：
    // 1) 初始化 DPI 感知；
    // 2) 读取配置并处理推荐缩放；
    // 3) 按配置处理自动提权；
    // 4) 设置 QT_SCALE_FACTOR；
    // 5) 显示 Framework 启动页并创建主窗口。
    startupTraceRaw("startup trace initialized without console binding");
    initializeProcessDpiAwareness();
    startupTraceRaw("initializeProcessDpiAwareness finished");

    {
        kLogEvent startupMainEvent;
        info << startupMainEvent
            << "[main] 进入主函数。 argc="
            << argc
            << eol;
    }

    // startupSettings 作用：缓存本次启动所需配置快照。
    const std::wstring executablePathForTrace = queryCurrentExecutablePath();
    const std::wstring currentDirectoryForTrace = queryCurrentDirectoryPath();
    const QString settingsReadPath = ks::settings::resolveSettingsJsonPathForRead();
    const bool settingsFileExists = ks::settings::settingsJsonFileExistsForRead();
    startupTraceRaw(
        std::string("startup paths: cwd=")
        + QString::fromStdWString(currentDirectoryForTrace).toUtf8().toStdString()
        + ", exe="
        + QString::fromStdWString(executablePathForTrace).toUtf8().toStdString()
        + ", settings_path="
        + settingsReadPath.toUtf8().toStdString()
        + ", settings_exists="
        + (settingsFileExists ? "true" : "false"));

    ks::settings::AppearanceSettings startupSettings = ks::settings::loadAppearanceSettings();
    startupTraceRaw(
        std::string("startup settings loaded: scale_factor=")
        + std::to_string(startupSettings.startupWindowScaleFactor)
        + ", startup_tab="
        + startupSettings.startupDefaultTabKey.toUtf8().toStdString()
        + ", startup_maximized="
        + (startupSettings.launchMaximizedOnStartup ? "true" : "false")
        + ", auto_admin="
        + (startupSettings.autoRequestAdminOnStartup ? "true" : "false"));
    {
        kLogEvent settingsEvent;
        info << settingsEvent
            << "[main] 启动配置已加载。 startup_tab="
            << startupSettings.startupDefaultTabKey
            << ", startup_maximized="
            << (startupSettings.launchMaximizedOnStartup ? "true" : "false")
            << ", auto_admin="
            << (startupSettings.autoRequestAdminOnStartup ? "true" : "false")
            << ", startup_scale_factor="
            << startupSettings.startupWindowScaleFactor
            << ", scale_prompt_disabled="
            << (startupSettings.startupScaleRecommendPromptDisabled ? "true" : "false")
            << eol;
    }

    startupTraceRaw("before maybeApplyStartupScaleRecommendation");
    if (maybeApplyStartupScaleRecommendation(&startupSettings))
    {
        startupTraceRaw("maybeApplyStartupScaleRecommendation returned true, exiting current instance");
        kLogEvent restartTakeoverEvent;
        warn << restartTakeoverEvent
            << "[main] maybeApplyStartupScaleRecommendation 返回 true，当前实例提前退出。"
            << eol;
        return 0;
    }

    if (startupSettings.autoRequestAdminOnStartup && !isCurrentProcessElevated())
    {
        startupTraceRaw("autoRequestAdminOnStartup enabled and process not elevated");
        kLogEvent adminRequestEvent;
        info << adminRequestEvent
            << "[main] 检测到启用自动管理员请求，准备在 splash 前尝试提权重启。"
            << eol;
        const bool elevatedLaunchStarted = tryLaunchElevatedSelfBeforeSplash();
        if (elevatedLaunchStarted)
        {
            startupTraceRaw("tryLaunchElevatedSelfBeforeSplash succeeded, exiting current instance");
            warn << adminRequestEvent
                << "[main] 管理员实例已启动，当前普通权限实例退出。"
                << eol;
            return 0;
        }
        warn << adminRequestEvent
            << "[main] 自动管理员请求未拉起新实例，将继续当前实例启动。"
            << eol;
    }

    startupTraceRaw("before applyQtScaleFactorEnvironment");
    applyQtScaleFactorEnvironment(startupSettings.startupWindowScaleFactor);
    startupTraceRaw("after applyQtScaleFactorEnvironment");

    const bool splashReady = kSplash.show();
    startupTraceRaw(std::string("kSplash.show finished, result=") + (splashReady ? "true" : "false"));
    {
        kLogEvent splashEvent;
        info << splashEvent
            << "[main] 启动画面 show 结果="
            << (splashReady ? "true" : "false")
            << eol;
    }
    if (splashReady)
    {
        kSplash.progress("正在初始化 Qt 运行时...", 6);
    }

    startupTraceRaw("before QApplication construction");
    QApplication app(argc, argv);
    startupTraceRaw("QApplication constructed");
    ks::ui::InstallGlobalMessageBoxTheme(&app);
    startupTraceRaw("InstallGlobalMessageBoxTheme finished");
    const QStringList argumentList = QCoreApplication::arguments();
    startupTraceRaw(
        std::string("QCoreApplication::arguments fetched, count=")
        + std::to_string(argumentList.size()));
    {
        kLogEvent argumentEvent;
        info << argumentEvent
            << "[main] QApplication 已创建。 argument_count="
            << argumentList.size()
            << eol;
    }

    const bool shouldRegisterUnlockerMenu = argumentList.contains(QStringLiteral("--register-unlocker-context-menu"));
    const bool shouldUnregisterUnlockerMenu = argumentList.contains(QStringLiteral("--unregister-unlocker-context-menu"));
    if (shouldRegisterUnlockerMenu || shouldUnregisterUnlockerMenu)
    {
        if (shouldUnregisterUnlockerMenu)
        {
            unregisterUnlockerContextMenu();
            startupSettings.unlockerShellContextMenuEnabled = false;
            QString saveErrorText;
            ks::settings::saveAppearanceSettings(startupSettings, &saveErrorText);
            QMessageBox::information(
                nullptr,
                QStringLiteral("Ksword 文件解锁器"),
                QStringLiteral("已移除系统右键菜单中的“Ksword 文件解锁器(R3/R0)”项。"));
            return 0;
        }

        const std::wstring executablePath = queryCurrentExecutablePath();
        if (executablePath.empty())
        {
            QMessageBox::warning(
                nullptr,
                QStringLiteral("Ksword 文件解锁器"),
                QStringLiteral("读取程序路径失败，无法注册系统右键菜单。"));
            return 1;
        }

        const bool registerOk = registerUnlockerContextMenu(executablePath);
        if (registerOk)
        {
            startupSettings.unlockerShellContextMenuEnabled = true;
            QString saveErrorText;
            ks::settings::saveAppearanceSettings(startupSettings, &saveErrorText);
            QMessageBox::information(
                nullptr,
                QStringLiteral("Ksword 文件解锁器"),
                QStringLiteral("已注册系统右键菜单，可在文件/目录/磁盘和目录空白处右键触发。"));
            return 0;
        }

        QMessageBox::warning(
            nullptr,
            QStringLiteral("Ksword 文件解锁器"),
            QStringLiteral("注册系统右键菜单失败，请确认当前账户对 HKCU\\Software\\Classes 具备写入权限。"));
        return 1;
    }

    // 常规启动下：根据设置页开关同步系统右键“文件解锁器”菜单。
    {
        const std::wstring executablePath = queryCurrentExecutablePath();
        if (!executablePath.empty())
        {
            if (startupSettings.unlockerShellContextMenuEnabled)
            {
                registerUnlockerContextMenu(executablePath);
            }
            else
            {
                unregisterUnlockerContextMenu();
            }
        }
    }

    QStringList unlockPathList;
    for (int index = 1; index < argumentList.size(); ++index)
    {
        if (argumentList[index] == QStringLiteral("--unlock"))
        {
            if (index + 1 < argumentList.size())
            {
                unlockPathList.push_back(argumentList[index + 1]);
                index += 1;
            }
        }
    }

    // startupProgressCallback 作用：
    // - 接收 MainWindow 构造阶段回调；
    // - 持续回写启动页文案和进度百分比。
    const MainWindow::StartupProgressCallback startupProgressCallback =
        [splashReady](const int progressPercent, const QString& statusText)
        {
            if (!splashReady)
            {
                return;
            }
            kSplash.progress(statusText.toUtf8().toStdString(), progressPercent);
            kLogEvent splashProgressEvent;
            info << splashProgressEvent
                << "[main] StartupProgressCallback progress="
                << progressPercent
                << ", status="
                << statusText
                << eol;
        };

    if (splashReady)
    {
        kSplash.progress("正在准备应用环境...", 18);
        kSplash.progress("正在创建主窗口...", 28);
    }

    MainWindow window(nullptr, startupProgressCallback);
    startupTraceRaw("MainWindow constructed");
    {
        kLogEvent windowConstructEvent;
        info << windowConstructEvent
            << "[main] MainWindow 构造已完成。 visible="
            << (window.isVisible() ? "true" : "false")
            << ", geometry="
            << window.geometry().x()
            << ","
            << window.geometry().y()
            << " "
            << window.geometry().width()
            << "x"
            << window.geometry().height()
            << eol;
    }
    FirstFrameSplashHider firstFrameHider(&window, &kSplash);
    window.installEventFilter(&firstFrameHider);
    if (window.centralWidget() != nullptr)
    {
        window.centralWidget()->installEventFilter(&firstFrameHider);
    }

    if (splashReady)
    {
        kSplash.progress("正在显示主窗口...", 95);
    }

    window.show();
    startupTraceRaw("window.show invoked");
    {
        kLogEvent showEvent;
        const QRect visibleGeometry = window.frameGeometry();
        info << showEvent
            << "[main] 已调用 window.show()。 isVisible="
            << (window.isVisible() ? "true" : "false")
            << ", isMinimized="
            << (window.isMinimized() ? "true" : "false")
            << ", frame="
            << visibleGeometry.x()
            << ","
            << visibleGeometry.y()
            << " "
            << visibleGeometry.width()
            << "x"
            << visibleGeometry.height()
            << eol;
    }
    if (startupSettings.launchMaximizedOnStartup)
    {
        QTimer::singleShot(0, &window, [&window]()
            {
                // windowHandle 用途：启动后执行一次原生最大化命令，避免 Qt/Win32 状态漂移。
                const HWND windowHandle = reinterpret_cast<HWND>(window.winId());
                if (windowHandle != nullptr && ::IsWindow(windowHandle) != FALSE)
                {
                    ::SendMessageW(
                        windowHandle,
                        WM_SYSCOMMAND,
                        static_cast<WPARAM>(SC_MAXIMIZE),
                        0);
                }
                else
                {
                    window.showMaximized();
                }

                kLogEvent maximizeEvent;
                const QRect maximizedFrame = window.frameGeometry();
                info << maximizeEvent
                    << "[main] 启动最大化请求已执行。 frame="
                    << maximizedFrame.x()
                    << ","
                    << maximizedFrame.y()
                    << " "
                    << maximizedFrame.width()
                    << "x"
                    << maximizedFrame.height()
                    << eol;
            });
    }
    applyNativeAppIconToWidget(&window);

    if (!unlockPathList.isEmpty())
    {
        for (const QString& targetPath : unlockPathList)
        {
            QTimer::singleShot(0, &window, [&window, targetPath]()
                {
                    window.openFileUnlockerDockByPath(targetPath);
                });
        }
    }

    if (splashReady)
    {
        kSplash.progress("正在等待首帧渲染...", 98);

        // 兜底策略：
        // - 若首帧事件异常未触发；
        // - 4 秒后强制隐藏启动页。
        QTimer::singleShot(4000, &window, []()
            {
                kLogEvent splashFallbackEvent;
                warn << splashFallbackEvent
                    << "[main] 首帧隐藏 splash 的 4 秒兜底计时器触发。"
                    << eol;
                kSplash.hide();
            });
    }

    QTimer::singleShot(200, &window, [&window]()
        {
            kLogEvent firstSnapshotEvent;
            const QRect snapshotFrame = window.frameGeometry();
            info << firstSnapshotEvent
                << "[main] 启动后 200ms 窗口快照。 visible="
                << (window.isVisible() ? "true" : "false")
                << ", minimized="
                << (window.isMinimized() ? "true" : "false")
                << ", active="
                << (window.isActiveWindow() ? "true" : "false")
                << ", frame="
                << snapshotFrame.x()
                << ","
                << snapshotFrame.y()
                << " "
                << snapshotFrame.width()
                << "x"
                << snapshotFrame.height()
                << eol;
        });

    QTimer::singleShot(1200, &window, [&window]()
        {
            kLogEvent secondSnapshotEvent;
            const QRect snapshotFrame = window.frameGeometry();
            info << secondSnapshotEvent
                << "[main] 启动后 1200ms 窗口快照。 visible="
                << (window.isVisible() ? "true" : "false")
                << ", minimized="
                << (window.isMinimized() ? "true" : "false")
                << ", active="
                << (window.isActiveWindow() ? "true" : "false")
                << ", frame="
                << snapshotFrame.x()
                << ","
                << snapshotFrame.y()
                << " "
                << snapshotFrame.width()
                << "x"
                << snapshotFrame.height()
                << eol;
        });

    const int exitCode = app.exec();
    startupTraceRaw(std::string("QApplication::exec returned, exitCode=") + std::to_string(exitCode));
    {
        kLogEvent exitLoopEvent;
        info << exitLoopEvent
            << "[main] QApplication::exec() 已返回。 exitCode="
            << exitCode
            << eol;
    }
    kSplash.hide();
    return exitCode;
}
