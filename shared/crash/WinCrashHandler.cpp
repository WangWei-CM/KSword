#include "WinCrashHandler.h"

#include <DbgHelp.h>
#include <Shellapi.h>
#include <TlHelp32.h>
#include <strsafe.h>

#include <cstddef>
#include <cwchar>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

namespace ks
{
    namespace crash
    {
        const wchar_t kCrashRestartWaitPidArgument[] = L"--ksword-crash-restart-wait-pid";
        const wchar_t kCrashRestartedArgument[] = L"--ksword-crash-restarted";

        namespace
        {
            using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(
                HANDLE,
                DWORD,
                HANDLE,
                MINIDUMP_TYPE,
                PMINIDUMP_EXCEPTION_INFORMATION,
                PMINIDUMP_USER_STREAM_INFORMATION,
                PMINIDUMP_CALLBACK_INFORMATION);

            struct CrashState
            {
                volatile LONG handling = 0;
                bool installed = false;
                bool chinese = false;
                bool preferLauncherReporter = false;
                bool restartAlreadyAttempted = false;
                HMODULE dbgHelpModule = nullptr;
                MiniDumpWriteDumpFunction miniDumpWriteDump = nullptr;

                wchar_t productName[128] = {};
                wchar_t dumpFilePrefix[128] = {};
                wchar_t executablePath[32768] = {};
                wchar_t executableDirectory[32768] = {};
                wchar_t launcherPath[32768] = {};
                wchar_t dumpDirectory[32768] = {};

                wchar_t title[256] = {};
                wchar_t instruction[512] = {};
                wchar_t exceptionCodeLabel[128] = {};
                wchar_t exceptionAddressLabel[128] = {};
                wchar_t dumpPathLabel[128] = {};
                wchar_t dumpUnavailableText[256] = {};
                wchar_t restartQuestion[256] = {};
                wchar_t repeatedCrashText[256] = {};
                wchar_t restartFailedText[256] = {};
            };

            CrashState g_state;

            void CopyText(wchar_t* destination, const std::size_t destinationCount, const wchar_t* source)
            {
                if (destination == nullptr || destinationCount == 0 || source == nullptr)
                {
                    return;
                }
                (void)::StringCchCopyW(destination, destinationCount, source);
            }

            bool IsChineseSystemUi()
            {
                return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
            }

            void InitializeDefaultDialogText()
            {
                if (g_state.chinese)
                {
                    CopyText(g_state.title, ARRAYSIZE(g_state.title), L"KswordARK 崩溃");
                    CopyText(g_state.instruction, ARRAYSIZE(g_state.instruction), L"KswordARK 因未处理的异常停止运行。");
                    CopyText(g_state.exceptionCodeLabel, ARRAYSIZE(g_state.exceptionCodeLabel), L"异常");
                    CopyText(g_state.exceptionAddressLabel, ARRAYSIZE(g_state.exceptionAddressLabel), L"地址");
                    CopyText(g_state.dumpPathLabel, ARRAYSIZE(g_state.dumpPathLabel), L"转储文件");
                    CopyText(g_state.dumpUnavailableText, ARRAYSIZE(g_state.dumpUnavailableText), L"转储文件未能写入。");
                    CopyText(g_state.restartQuestion, ARRAYSIZE(g_state.restartQuestion), L"选择“是”重新启动 KswordARK，选择“否”退出。");
                    CopyText(g_state.repeatedCrashText, ARRAYSIZE(g_state.repeatedCrashText), L"程序刚刚重启后再次崩溃，建议先退出并检查转储文件。");
                    CopyText(g_state.restartFailedText, ARRAYSIZE(g_state.restartFailedText), L"重新启动 KswordARK 失败，程序即将退出。");
                }
                else
                {
                    CopyText(g_state.title, ARRAYSIZE(g_state.title), L"KswordARK crashed");
                    CopyText(g_state.instruction, ARRAYSIZE(g_state.instruction), L"KswordARK stopped because of an unhandled exception.");
                    CopyText(g_state.exceptionCodeLabel, ARRAYSIZE(g_state.exceptionCodeLabel), L"Exception");
                    CopyText(g_state.exceptionAddressLabel, ARRAYSIZE(g_state.exceptionAddressLabel), L"Address");
                    CopyText(g_state.dumpPathLabel, ARRAYSIZE(g_state.dumpPathLabel), L"Dump file");
                    CopyText(g_state.dumpUnavailableText, ARRAYSIZE(g_state.dumpUnavailableText), L"The dump file could not be written.");
                    CopyText(g_state.restartQuestion, ARRAYSIZE(g_state.restartQuestion), L"Choose Yes to restart KswordARK, or No to exit.");
                    CopyText(g_state.repeatedCrashText, ARRAYSIZE(g_state.repeatedCrashText), L"The program crashed again immediately after a restart. Exit and inspect the dump file.");
                    CopyText(g_state.restartFailedText, ARRAYSIZE(g_state.restartFailedText), L"KswordARK could not be restarted and will now exit.");
                }
            }

            void ResolveExecutablePaths()
            {
                g_state.executablePath[0] = L'\0';
                (void)::GetModuleFileNameW(
                    nullptr,
                    g_state.executablePath,
                    static_cast<DWORD>(ARRAYSIZE(g_state.executablePath)));

                CopyText(
                    g_state.executableDirectory,
                    ARRAYSIZE(g_state.executableDirectory),
                    g_state.executablePath);
                wchar_t* const separator = wcsrchr(g_state.executableDirectory, L'\\');
                wchar_t* const alternateSeparator = wcsrchr(g_state.executableDirectory, L'/');
                wchar_t* finalSeparator = separator;
                if (finalSeparator == nullptr
                    || (alternateSeparator != nullptr && alternateSeparator > finalSeparator))
                {
                    finalSeparator = alternateSeparator;
                }
                if (finalSeparator != nullptr)
                {
                    *finalSeparator = L'\0';
                }
                else
                {
                    CopyText(g_state.executableDirectory, ARRAYSIZE(g_state.executableDirectory), L".");
                }

                (void)::StringCchPrintfW(
                    g_state.launcherPath,
                    ARRAYSIZE(g_state.launcherPath),
                    L"%s\\Launcher.exe",
                    g_state.executableDirectory);
            }

            bool EnsureCrashDirectory()
            {
                wchar_t baseDirectory[32768] = {};
                DWORD copied = ::GetEnvironmentVariableW(
                    L"LOCALAPPDATA",
                    baseDirectory,
                    static_cast<DWORD>(ARRAYSIZE(baseDirectory)));
                if (copied == 0 || copied >= ARRAYSIZE(baseDirectory))
                {
                    copied = ::GetTempPathW(
                        static_cast<DWORD>(ARRAYSIZE(baseDirectory)),
                        baseDirectory);
                    if (copied == 0 || copied >= ARRAYSIZE(baseDirectory))
                    {
                        return false;
                    }
                }

                wchar_t productDirectory[32768] = {};
                if (FAILED(::StringCchPrintfW(
                    productDirectory,
                    ARRAYSIZE(productDirectory),
                    L"%s\\KswordARK",
                    baseDirectory)))
                {
                    return false;
                }
                (void)::CreateDirectoryW(productDirectory, nullptr);

                if (FAILED(::StringCchPrintfW(
                    g_state.dumpDirectory,
                    ARRAYSIZE(g_state.dumpDirectory),
                    L"%s\\CrashDumps",
                    productDirectory)))
                {
                    return false;
                }
                if (::CreateDirectoryW(g_state.dumpDirectory, nullptr) != FALSE
                    || ::GetLastError() == ERROR_ALREADY_EXISTS)
                {
                    return true;
                }

                wchar_t temporaryDirectory[32768] = {};
                copied = ::GetTempPathW(
                    static_cast<DWORD>(ARRAYSIZE(temporaryDirectory)),
                    temporaryDirectory);
                if (copied == 0 || copied >= ARRAYSIZE(temporaryDirectory))
                {
                    return false;
                }
                if (FAILED(::StringCchPrintfW(
                    g_state.dumpDirectory,
                    ARRAYSIZE(g_state.dumpDirectory),
                    L"%sKswordARK-CrashDumps",
                    temporaryDirectory)))
                {
                    return false;
                }
                return ::CreateDirectoryW(g_state.dumpDirectory, nullptr) != FALSE
                    || ::GetLastError() == ERROR_ALREADY_EXISTS;
            }

            const wchar_t* ExceptionDescription(const DWORD exceptionCode)
            {
                if (g_state.chinese)
                {
                    switch (exceptionCode)
                    {
                    case EXCEPTION_ACCESS_VIOLATION: return L"访问冲突";
                    case EXCEPTION_ILLEGAL_INSTRUCTION: return L"非法指令";
                    case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"整数除零";
                    case EXCEPTION_STACK_OVERFLOW: return L"栈溢出";
                    case STATUS_HEAP_CORRUPTION: return L"堆损坏";
                    case 0xE06D7363u: return L"未处理的 C++ 异常";
                    default: return L"未处理异常";
                    }
                }

                switch (exceptionCode)
                {
                case EXCEPTION_ACCESS_VIOLATION: return L"Access violation";
                case EXCEPTION_ILLEGAL_INSTRUCTION: return L"Illegal instruction";
                case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"Integer divide by zero";
                case EXCEPTION_STACK_OVERFLOW: return L"Stack overflow";
                case STATUS_HEAP_CORRUPTION: return L"Heap corruption";
                case 0xE06D7363u: return L"Unhandled C++ exception";
                default: return L"Unhandled exception";
                }
            }

            bool WriteMinidump(
                PEXCEPTION_POINTERS exceptionPointers,
                wchar_t* dumpPath,
                const std::size_t dumpPathCount)
            {
                if (dumpPath == nullptr || dumpPathCount == 0 || g_state.miniDumpWriteDump == nullptr)
                {
                    return false;
                }

                SYSTEMTIME localTime = {};
                ::GetLocalTime(&localTime);
                if (FAILED(::StringCchPrintfW(
                    dumpPath,
                    dumpPathCount,
                    L"%s\\%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
                    g_state.dumpDirectory,
                    g_state.dumpFilePrefix,
                    static_cast<unsigned int>(localTime.wYear),
                    static_cast<unsigned int>(localTime.wMonth),
                    static_cast<unsigned int>(localTime.wDay),
                    static_cast<unsigned int>(localTime.wHour),
                    static_cast<unsigned int>(localTime.wMinute),
                    static_cast<unsigned int>(localTime.wSecond),
                    static_cast<unsigned long>(::GetCurrentProcessId()))))
                {
                    dumpPath[0] = L'\0';
                    return false;
                }

                HANDLE dumpFile = ::CreateFileW(
                    dumpPath,
                    GENERIC_WRITE,
                    FILE_SHARE_READ,
                    nullptr,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
                if (dumpFile == INVALID_HANDLE_VALUE)
                {
                    return false;
                }

                MINIDUMP_EXCEPTION_INFORMATION exceptionInformation = {};
                exceptionInformation.ThreadId = ::GetCurrentThreadId();
                exceptionInformation.ExceptionPointers = exceptionPointers;
                exceptionInformation.ClientPointers = FALSE;
                const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
                    MiniDumpNormal
                    | MiniDumpWithThreadInfo
                    | MiniDumpWithUnloadedModules
                    | MiniDumpWithIndirectlyReferencedMemory);
                const BOOL writeOk = g_state.miniDumpWriteDump(
                    ::GetCurrentProcess(),
                    ::GetCurrentProcessId(),
                    dumpFile,
                    dumpType,
                    exceptionPointers != nullptr ? &exceptionInformation : nullptr,
                    nullptr,
                    nullptr);
                ::CloseHandle(dumpFile);
                return writeOk != FALSE;
            }

            bool LaunchSelfForRestart()
            {
                wchar_t commandLine[32768] = {};
                if (FAILED(::StringCchPrintfW(
                    commandLine,
                    ARRAYSIZE(commandLine),
                    L"\"%s\" %s %lu %s",
                    g_state.executablePath,
                    kCrashRestartWaitPidArgument,
                    static_cast<unsigned long>(::GetCurrentProcessId()),
                    kCrashRestartedArgument)))
                {
                    return false;
                }

                STARTUPINFOW startupInfo = {};
                startupInfo.cb = sizeof(startupInfo);
                startupInfo.dwFlags = STARTF_USESHOWWINDOW;
                startupInfo.wShowWindow = SW_SHOWNORMAL;
                PROCESS_INFORMATION processInformation = {};
                const BOOL createOk = ::CreateProcessW(
                    g_state.executablePath,
                    commandLine,
                    nullptr,
                    nullptr,
                    FALSE,
                    0,
                    nullptr,
                    g_state.executableDirectory,
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

            bool LaunchExternalReporter(
                const DWORD exceptionCode,
                const ULONG_PTR exceptionAddress,
                const wchar_t* dumpPath,
                const bool dumpWritten)
            {
                if (!g_state.preferLauncherReporter
                    || g_state.launcherPath[0] == L'\0'
                    || ::GetFileAttributesW(g_state.launcherPath) == INVALID_FILE_ATTRIBUTES)
                {
                    return false;
                }

                wchar_t eventName[128] = {};
                if (FAILED(::StringCchPrintfW(
                    eventName,
                    ARRAYSIZE(eventName),
                    L"Local\\KswordARKCrashReady_%lu_%lu",
                    static_cast<unsigned long>(::GetCurrentProcessId()),
                    static_cast<unsigned long>(::GetCurrentThreadId()))))
                {
                    return false;
                }
                HANDLE readyEvent = ::CreateEventW(nullptr, TRUE, FALSE, eventName);
                if (readyEvent == nullptr)
                {
                    return false;
                }

                wchar_t commandLine[32768] = {};
                const HRESULT formatResult = ::StringCchPrintfW(
                    commandLine,
                    ARRAYSIZE(commandLine),
                    L"\"%s\" --launcher-crash-report --launcher-crash-pid %lu "
                    L"--launcher-crash-code 0x%08lX --launcher-crash-address 0x%llX "
                    L"--launcher-crash-dump \"%s\" --launcher-crash-dump-written %u "
                    L"--launcher-crash-ready-event \"%s\"%s",
                    g_state.launcherPath,
                    static_cast<unsigned long>(::GetCurrentProcessId()),
                    static_cast<unsigned long>(exceptionCode),
                    static_cast<unsigned long long>(exceptionAddress),
                    dumpPath == nullptr ? L"" : dumpPath,
                    dumpWritten ? 1u : 0u,
                    eventName,
                    g_state.restartAlreadyAttempted ? L" --launcher-crash-repeat" : L"");
                if (FAILED(formatResult))
                {
                    ::CloseHandle(readyEvent);
                    return false;
                }

                STARTUPINFOW startupInfo = {};
                startupInfo.cb = sizeof(startupInfo);
                startupInfo.dwFlags = STARTF_USESHOWWINDOW;
                startupInfo.wShowWindow = SW_SHOWNORMAL;
                PROCESS_INFORMATION processInformation = {};
                const BOOL createOk = ::CreateProcessW(
                    g_state.launcherPath,
                    commandLine,
                    nullptr,
                    nullptr,
                    FALSE,
                    0,
                    nullptr,
                    g_state.executableDirectory,
                    &startupInfo,
                    &processInformation);
                if (createOk == FALSE)
                {
                    ::CloseHandle(readyEvent);
                    return false;
                }

                const DWORD waitResult = ::WaitForSingleObject(readyEvent, 5000);
                ::CloseHandle(processInformation.hThread);
                ::CloseHandle(processInformation.hProcess);
                ::CloseHandle(readyEvent);
                return waitResult == WAIT_OBJECT_0;
            }

            int ShowLocalCrashDialog(
                const DWORD exceptionCode,
                const ULONG_PTR exceptionAddress,
                const wchar_t* dumpPath,
                const bool dumpWritten)
            {
                wchar_t message[8192] = {};
                const wchar_t* const description = ExceptionDescription(exceptionCode);
                const wchar_t* const pathText = dumpWritten && dumpPath != nullptr && dumpPath[0] != L'\0'
                    ? dumpPath
                    : g_state.dumpUnavailableText;
                const HRESULT formatResult = ::StringCchPrintfW(
                    message,
                    ARRAYSIZE(message),
                    L"%s\r\n\r\n%s: %s (0x%08lX)\r\n%s: 0x%llX\r\n%s: %s\r\n\r\n%s%s",
                    g_state.instruction,
                    g_state.exceptionCodeLabel,
                    description,
                    static_cast<unsigned long>(exceptionCode),
                    g_state.exceptionAddressLabel,
                    static_cast<unsigned long long>(exceptionAddress),
                    g_state.dumpPathLabel,
                    pathText,
                    g_state.restartAlreadyAttempted ? g_state.repeatedCrashText : L"",
                    g_state.restartAlreadyAttempted ? L"\r\n\r\n" : g_state.restartQuestion);
                if (FAILED(formatResult))
                {
                    return IDNO;
                }

                const UINT flags = (g_state.restartAlreadyAttempted ? MB_OK : MB_YESNO)
                    | MB_ICONERROR
                    | MB_DEFBUTTON2
                    | MB_SETFOREGROUND
                    | MB_TOPMOST;
                return static_cast<int>(::MessageBoxW(nullptr, message, g_state.title, flags));
            }

            LONG WINAPI TopLevelExceptionFilter(PEXCEPTION_POINTERS exceptionPointers)
            {
                if (::InterlockedCompareExchange(&g_state.handling, 1, 0) != 0)
                {
                    return EXCEPTION_EXECUTE_HANDLER;
                }

                const DWORD exceptionCode = exceptionPointers != nullptr
                    && exceptionPointers->ExceptionRecord != nullptr
                    ? exceptionPointers->ExceptionRecord->ExceptionCode
                    : ERROR_UNHANDLED_EXCEPTION;
                const ULONG_PTR exceptionAddress = exceptionPointers != nullptr
                    && exceptionPointers->ExceptionRecord != nullptr
                    ? reinterpret_cast<ULONG_PTR>(exceptionPointers->ExceptionRecord->ExceptionAddress)
                    : 0;
                wchar_t dumpPath[32768] = {};
                const bool dumpWritten = WriteMinidump(exceptionPointers, dumpPath, ARRAYSIZE(dumpPath));

                if (LaunchExternalReporter(exceptionCode, exceptionAddress, dumpPath, dumpWritten))
                {
                    return EXCEPTION_EXECUTE_HANDLER;
                }

                const int dialogResult = ShowLocalCrashDialog(
                    exceptionCode,
                    exceptionAddress,
                    dumpPath,
                    dumpWritten);
                if (dialogResult == IDYES && !g_state.restartAlreadyAttempted)
                {
                    if (!LaunchSelfForRestart())
                    {
                        (void)::MessageBoxW(
                            nullptr,
                            g_state.restartFailedText,
                            g_state.title,
                            MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
                    }
                }
                return EXCEPTION_EXECUTE_HANDLER;
            }

            bool TryReadArgumentValue(const wchar_t* argumentName, unsigned long long* value)
            {
                if (argumentName == nullptr || value == nullptr)
                {
                    return false;
                }
                int argumentCount = 0;
                LPWSTR* argumentVector = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
                if (argumentVector == nullptr)
                {
                    return false;
                }
                bool found = false;
                for (int index = 1; index + 1 < argumentCount; ++index)
                {
                    if (_wcsicmp(argumentVector[index], argumentName) == 0)
                    {
                        wchar_t* end = nullptr;
                        const unsigned long long parsed = _wcstoui64(argumentVector[index + 1], &end, 0);
                        if (end != argumentVector[index + 1] && *end == L'\0')
                        {
                            *value = parsed;
                            found = true;
                        }
                        break;
                    }
                }
                ::LocalFree(argumentVector);
                return found;
            }

            bool IsProcessDirectChildOf(const DWORD childProcessId, const DWORD expectedParentProcessId)
            {
                HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snapshot == INVALID_HANDLE_VALUE)
                {
                    return false;
                }

                PROCESSENTRY32W entry = {};
                entry.dwSize = sizeof(entry);
                bool matches = false;
                if (::Process32FirstW(snapshot, &entry) != FALSE)
                {
                    do
                    {
                        if (entry.th32ProcessID == childProcessId)
                        {
                            matches = entry.th32ParentProcessID == expectedParentProcessId;
                            break;
                        }
                    } while (::Process32NextW(snapshot, &entry) != FALSE);
                }
                ::CloseHandle(snapshot);
                return matches;
            }

            bool IsCurrentProcessDirectChildOf(const DWORD expectedParentProcessId)
            {
                return IsProcessDirectChildOf(::GetCurrentProcessId(), expectedParentProcessId);
            }

            bool IsParentOlderThanChild(const HANDLE parent, const HANDLE child)
            {
                FILETIME parentCreated{}, parentExited{}, parentKernel{}, parentUser{};
                FILETIME childCreated{}, childExited{}, childKernel{}, childUser{};
                return ::GetProcessTimes(parent, &parentCreated, &parentExited, &parentKernel, &parentUser)
                    && ::GetProcessTimes(child, &childCreated, &childExited, &childKernel, &childUser)
                    && ::CompareFileTime(&parentCreated, &childCreated) <= 0;
            }

            bool IsSameExecutableProcess(const HANDLE processHandle, bool* imageQuerySucceeded = nullptr)
            {
                wchar_t processPath[32768] = {};
                DWORD processPathCount = static_cast<DWORD>(ARRAYSIZE(processPath));
                const bool queried = ::QueryFullProcessImageNameW(
                    processHandle,
                    0,
                    processPath,
                    &processPathCount) != FALSE;
                if (imageQuerySucceeded != nullptr)
                {
                    *imageQuerySucceeded = queried;
                }
                if (!queried)
                {
                    return false;
                }
                return _wcsicmp(processPath, g_state.executablePath) == 0;
            }
        }

        void InstallCrashHandler(const Configuration& configuration)
        {
            if (g_state.installed)
            {
                return;
            }
            g_state.installed = true;
            g_state.chinese = IsChineseSystemUi();
            g_state.preferLauncherReporter = configuration.preferLauncherReporter;
            CopyText(
                g_state.productName,
                ARRAYSIZE(g_state.productName),
                configuration.productName == nullptr ? L"KswordARK" : configuration.productName);
            CopyText(
                g_state.dumpFilePrefix,
                ARRAYSIZE(g_state.dumpFilePrefix),
                configuration.dumpFilePrefix == nullptr ? L"KswordARK" : configuration.dumpFilePrefix);
            g_state.restartAlreadyAttempted = CommandLineHasArgument(kCrashRestartedArgument);
            ResolveExecutablePaths();
            (void)EnsureCrashDirectory();
            g_state.dbgHelpModule = ::LoadLibraryW(L"dbghelp.dll");
            if (g_state.dbgHelpModule != nullptr)
            {
                g_state.miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFunction>(
                    ::GetProcAddress(g_state.dbgHelpModule, "MiniDumpWriteDump"));
            }
            InitializeDefaultDialogText();
            ::SetUnhandledExceptionFilter(TopLevelExceptionFilter);
        }

        void UpdateCrashDialogText(const DialogText& text)
        {
            if (text.title != nullptr) CopyText(g_state.title, ARRAYSIZE(g_state.title), text.title);
            if (text.instruction != nullptr) CopyText(g_state.instruction, ARRAYSIZE(g_state.instruction), text.instruction);
            if (text.exceptionCodeLabel != nullptr) CopyText(g_state.exceptionCodeLabel, ARRAYSIZE(g_state.exceptionCodeLabel), text.exceptionCodeLabel);
            if (text.exceptionAddressLabel != nullptr) CopyText(g_state.exceptionAddressLabel, ARRAYSIZE(g_state.exceptionAddressLabel), text.exceptionAddressLabel);
            if (text.dumpPathLabel != nullptr) CopyText(g_state.dumpPathLabel, ARRAYSIZE(g_state.dumpPathLabel), text.dumpPathLabel);
            if (text.dumpUnavailableText != nullptr) CopyText(g_state.dumpUnavailableText, ARRAYSIZE(g_state.dumpUnavailableText), text.dumpUnavailableText);
            if (text.restartQuestion != nullptr) CopyText(g_state.restartQuestion, ARRAYSIZE(g_state.restartQuestion), text.restartQuestion);
            if (text.repeatedCrashText != nullptr) CopyText(g_state.repeatedCrashText, ARRAYSIZE(g_state.repeatedCrashText), text.repeatedCrashText);
            if (text.restartFailedText != nullptr) CopyText(g_state.restartFailedText, ARRAYSIZE(g_state.restartFailedText), text.restartFailedText);
        }

        bool CommandLineHasArgument(const wchar_t* argumentName)
        {
            if (argumentName == nullptr || argumentName[0] == L'\0')
            {
                return false;
            }
            int argumentCount = 0;
            LPWSTR* argumentVector = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
            if (argumentVector == nullptr)
            {
                return false;
            }
            bool found = false;
            for (int index = 1; index < argumentCount; ++index)
            {
                if (_wcsicmp(argumentVector[index], argumentName) == 0)
                {
                    found = true;
                    break;
                }
            }
            ::LocalFree(argumentVector);
            return found;
        }

        bool ValidateRestartChildProcess(const HANDLE childProcess)
        {
            const DWORD childId = childProcess ? ::GetProcessId(childProcess) : 0;
            DWORD exitCode = 0;
            return childId && ::GetExitCodeProcess(childProcess, &exitCode) && exitCode == STILL_ACTIVE
                && IsProcessDirectChildOf(childId, ::GetCurrentProcessId())
                && IsSameExecutableProcess(childProcess)
                && IsParentOlderThanChild(::GetCurrentProcess(), childProcess);
        }

        RestartPredecessorStatus InspectRestartPredecessorFromCommandLine(const DWORD timeoutMilliseconds)
        {
            if (!CommandLineHasArgument(kCrashRestartWaitPidArgument))
                return RestartPredecessorStatus::NotRequested;
            unsigned long long processId = 0;
            if (!TryReadArgumentValue(kCrashRestartWaitPidArgument, &processId) || processId == 0 ||
                processId > static_cast<unsigned long long>(MAXDWORD) || processId == ::GetCurrentProcessId())
                return RestartPredecessorStatus::InvalidArgument;
            const auto predecessorId = static_cast<DWORD>(processId);
            if (!IsCurrentProcessDirectChildOf(predecessorId))
                return RestartPredecessorStatus::ValidationRejected;
            HANDLE predecessor = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, predecessorId);
            if (!predecessor)
            {
                // The recorded direct parent has already gone; no arbitrary live
                // PID is accepted or waited here, and no business startup overlaps it.
                return ::GetLastError() == ERROR_INVALID_PARAMETER
                    ? RestartPredecessorStatus::PredecessorAlreadyExited
                    : RestartPredecessorStatus::ValidationRejected;
            }
            if (!IsParentOlderThanChild(predecessor, ::GetCurrentProcess()))
            {
                ::CloseHandle(predecessor);
                return RestartPredecessorStatus::ValidationRejected;
            }
            bool imageQuerySucceeded = false;
            if (!IsSameExecutableProcess(predecessor, &imageQuerySucceeded))
            {
                // An exited direct parent's process object can outlive its image.
                // Recheck after the failed query: the parent may exit during it.
                // A known image mismatch or an unqueryable live parent still fails.
                const DWORD exitProbe = imageQuerySucceeded
                    ? WAIT_TIMEOUT : ::WaitForSingleObject(predecessor, 0);
                ::CloseHandle(predecessor);
                if (exitProbe == WAIT_OBJECT_0)
                    return RestartPredecessorStatus::PredecessorAlreadyExited;
                return exitProbe == WAIT_FAILED ? RestartPredecessorStatus::WaitFailed
                    : RestartPredecessorStatus::ValidationRejected;
            }
            const DWORD result = ::WaitForSingleObject(predecessor, timeoutMilliseconds);
            ::CloseHandle(predecessor);
            if (result == WAIT_OBJECT_0) return RestartPredecessorStatus::PredecessorExited;
            if (result == WAIT_TIMEOUT) return RestartPredecessorStatus::TimedOut;
            return RestartPredecessorStatus::WaitFailed;
        }

        bool WaitForCrashRestartTargetFromCommandLine(const DWORD timeoutMilliseconds)
        {
            unsigned long long processId = 0;
            if (!TryReadArgumentValue(kCrashRestartWaitPidArgument, &processId)
                || processId == 0
                || processId > static_cast<unsigned long long>(MAXDWORD))
            {
                return false;
            }

            const DWORD predecessorProcessId = static_cast<DWORD>(processId);
            HANDLE processHandle = ::OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                predecessorProcessId);
            if (processHandle == nullptr)
            {
                return false;
            }
            if (!IsCurrentProcessDirectChildOf(predecessorProcessId)
                || !IsSameExecutableProcess(processHandle))
            {
                ::CloseHandle(processHandle);
                return false;
            }
            const DWORD waitResult = ::WaitForSingleObject(
                processHandle,
                timeoutMilliseconds);
            ::CloseHandle(processHandle);
            return waitResult == WAIT_OBJECT_0;
        }
    }
}
