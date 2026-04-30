#include "process.h"

#include "../string/string.h"

// Win32 头：进程、线程、令牌、工具快照、Shell、签名等。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <WinTrust.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <Shellapi.h>
#include <winternl.h>
#include <RestartManager.h>

#include <algorithm>   // std::max/std::clamp：衍生指标计算。
#include <chrono>      // steady_clock：跨刷新轮次计时。
#include <cstring>     // std::memset：远程结构读取前清零输出缓冲。
#include <cwchar>      // std::swprintf：拼接版本信息查询路径。
#include <filesystem>  // std::filesystem：路径存在性与目录判断。
#include <fstream>     // std::ifstream：读取 PE 头计算入口 RVA。
#include <iomanip>     // std::hex：格式化十六进制文本。
#include <iterator>    // std::size：静态数组长度。
#include <sstream>     // std::ostringstream：错误文本拼接。
#include <unordered_map> // std::unordered_map：线程枚举时缓存 PID->进程名。
#include <vector>      // std::vector：系统信息缓冲与容器。

// 链接依赖库：
// - Psapi：GetProcessMemoryInfo；
// - Wintrust/Crypt32：数字签名校验；
// - Version：读取文件版本信息中的 CompanyName（厂家兜底）。
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Wintrust.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Advapi32.lib")

namespace
{
    // STATUS_INFO_LENGTH_MISMATCH：NtQuerySystemInformation 缓冲不足返回码。
#ifndef STATUS_INFO_LENGTH_MISMATCH
    constexpr NTSTATUS StatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
#else
    constexpr NTSTATUS StatusInfoLengthMismatch = STATUS_INFO_LENGTH_MISMATCH;
#endif

    // PROCESS_SUSPEND_RESUME：旧头文件可能未定义，手动兜底。
#ifndef PROCESS_SUSPEND_RESUME
    constexpr DWORD ProcessSuspendResumeAccess = 0x0800;
#else
    constexpr DWORD ProcessSuspendResumeAccess = PROCESS_SUSPEND_RESUME;
#endif

    // ProcessBreakOnTermination：NtSetInformationProcess 的关键进程信息类。
    constexpr PROCESSINFOCLASS ProcessBreakOnTerminationInfoClass = static_cast<PROCESSINFOCLASS>(29);

    // ProcessPowerThrottling：Get/SetProcessInformation 的效率模式信息类。
    constexpr ULONG ProcessPowerThrottlingInfoClass = 4UL;
    constexpr ULONG ProcessPowerThrottlingCurrentVersion = 1UL;
    constexpr ULONG ProcessPowerThrottlingExecutionSpeed = 0x1UL;

    // ProcessProtectionLevelInfo：
    // - GetProcessInformation PROCESS_INFORMATION_CLASS 枚举值 7；
    // - 返回 PROCESS_PROTECTION_LEVEL_INFORMATION.ProtectionLevel。
    constexpr ULONG ProcessProtectionLevelInfoClass = 7UL;

    // PROTECTION_LEVEL_*：
    // - 部分构建环境的 processthreadsapi.h 未暴露这些宏；
    // - 本地兜底值仅用于解码 GetProcessInformation 的公开枚举。
    // - 0 同时用于 WINTCB_LIGHT，命名按 WinBase.h 的公开宏保持一致。
    constexpr DWORD ProcessProtectionLevelWinTcbLight = 0x00000000UL;
    constexpr DWORD ProcessProtectionLevelNone = 0xFFFFFFFEUL;
    constexpr DWORD ProcessProtectionLevelWindows = 0x00000001UL;
    constexpr DWORD ProcessProtectionLevelWindowsLight = 0x00000002UL;
    constexpr DWORD ProcessProtectionLevelAntimalwareLight = 0x00000003UL;
    constexpr DWORD ProcessProtectionLevelLsaLight = 0x00000004UL;
    constexpr DWORD ProcessProtectionLevelWinTcb = 0x00000005UL;
    constexpr DWORD ProcessProtectionLevelCodegenLight = 0x00000006UL;
    constexpr DWORD ProcessProtectionLevelAuthenticode = 0x00000007UL;
    constexpr DWORD ProcessProtectionLevelPplApp = 0x00000008UL;
    constexpr DWORD ProcessProtectionLevelSame = 0xFFFFFFFFUL;

    // Restart Manager 关闭标记：
    // - 这里统一使用本地常量，不直接依赖 SDK 是否暴露枚举名字；
    // - 语义等价于 Restart Manager 的强制关闭选项。
    constexpr ULONG RestartManagerForceShutdownFlag = 0x1UL;

    // NT_SUCCESS：判断 NTSTATUS 成功与否。
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

    // Nt 函数指针类型定义。
    using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    using GetProcessInformationFn = BOOL(WINAPI*)(HANDLE, ULONG, LPVOID, DWORD);
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, ULONG, LPVOID, DWORD);
    using NtSuspendProcessFn = NTSTATUS(NTAPI*)(HANDLE);
    using NtResumeProcessFn = NTSTATUS(NTAPI*)(HANDLE);
    using NtSetInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG);
    using NtTerminateProcessFn = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);
    using NtTerminateThreadFn = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);
    using NtTerminateJobObjectFn = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);
    using NtUnmapViewOfSectionFn = NTSTATUS(NTAPI*)(HANDLE, PVOID);

    // ProcessBasicInformation 结果结构（与 NtQueryInformationProcess 对应）。
    struct ProcessBasicInformationLocal
    {
        PVOID reserved1 = nullptr;
        PVOID pebBaseAddress = nullptr;
        PVOID reserved2[2]{};
        ULONG_PTR uniqueProcessId = 0;
        PVOID reserved3 = nullptr;
    };

    // ProcessCommandLineInformation：
    // - NtQueryInformationProcess 信息类 60；
    // - 优先走该路径可避开手工 PEB 偏移差异。
    constexpr PROCESSINFOCLASS ProcessCommandLineInformationClass =
        static_cast<PROCESSINFOCLASS>(60);

    // ProcessWow64Information：
    // - NtQueryInformationProcess 信息类 26；
    // - 64 位工具读取 32 位目标时用它获取 Wow64 PEB 地址。
    constexpr PROCESSINFOCLASS ProcessWow64InformationClass =
        static_cast<PROCESSINFOCLASS>(26);

    // 远程 UNICODE_STRING 最大读取长度：
    // - Length 来自目标进程内存，不能无条件信任；
    // - 256KB 足够覆盖正常命令行，同时避免坏指针导致超大分配。
    constexpr std::size_t RemoteUnicodeStringMaxBytes = 256 * 1024;

    // RemoteUnicodeString32：
    // - 32 位目标进程内 UNICODE_STRING 的真实布局；
    // - Buffer 是 32 位远程地址，读取前需要提升为 64 位整数。
    struct RemoteUnicodeString32
    {
        USHORT length = 0;        // 字符串字节数，不包含终止 NUL。
        USHORT maximumLength = 0; // 缓冲容量字节数。
        std::uint32_t buffer = 0; // 32 位远程 PWSTR 地址。
    };

    // Peb32CommandLineLite / Peb64CommandLineLite：
    // - 只保留 PEB 起始字段到 ProcessParameters；
    // - 避免依赖 SDK 裁剪版 PEB，也避免读取整个 PEB 结构。
    struct Peb32CommandLineLite
    {
        BYTE reserved1[2]{};
        BYTE beingDebugged = 0;
        BYTE reserved2[1]{};
        std::uint32_t mutant = 0;
        std::uint32_t imageBaseAddress = 0;
        std::uint32_t ldr = 0;
        std::uint32_t processParameters = 0;
    };

    struct Peb64CommandLineLite
    {
        BYTE reserved1[2]{};
        BYTE beingDebugged = 0;
        BYTE reserved2[1]{};
        PVOID mutant = nullptr;
        PVOID imageBaseAddress = nullptr;
        PVOID ldr = nullptr;
        PVOID processParameters = nullptr;
    };

    // RtlUserProcessParameters32CommandLine / 64CommandLine：
    // - 按公开稳定偏移直接定位 CommandLine 字段；
    // - 32 位 CommandLine 位于 0x40，64 位 CommandLine 位于 0x70。
    struct RtlUserProcessParameters32CommandLine
    {
        BYTE reservedBeforeCommandLine[0x40]{};
        RemoteUnicodeString32 commandLine{};
    };

    struct RtlUserProcessParameters64CommandLine
    {
        BYTE reservedBeforeCommandLine[0x70]{};
        UNICODE_STRING commandLine{};
    };

    // NtQuerySystemInformation(SystemProcessInformation) 的完整结构体定义。
    // 说明：
    // - 部分 SDK 的 _SYSTEM_PROCESS_INFORMATION 字段被裁剪；
    // - 这里使用兼容版定义以读取创建时间、CPU 时间与 IO 计数器。
    struct SystemProcessInformationRecord
    {
        ULONG NextEntryOffset;
        ULONG NumberOfThreads;
        LARGE_INTEGER WorkingSetPrivateSize;
        ULONG HardFaultCount;
        ULONG NumberOfThreadsHighWatermark;
        ULONGLONG CycleTime;
        LARGE_INTEGER CreateTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER KernelTime;
        UNICODE_STRING ImageName;
        LONG BasePriority;
        HANDLE UniqueProcessId;
        HANDLE InheritedFromUniqueProcessId;
        ULONG HandleCount;
        ULONG SessionId;
        ULONG_PTR UniqueProcessKey;
        SIZE_T PeakVirtualSize;
        SIZE_T VirtualSize;
        ULONG PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
        SIZE_T PrivatePageCount;
        LARGE_INTEGER ReadOperationCount;
        LARGE_INTEGER WriteOperationCount;
        LARGE_INTEGER OtherOperationCount;
        LARGE_INTEGER ReadTransferCount;
        LARGE_INTEGER WriteTransferCount;
        LARGE_INTEGER OtherTransferCount;
    };

    // NtQuerySystemInformation(SystemProcessInformation) 中的线程子结构。
    // 说明：
    // - 官方 winternl 头在不同 SDK 上字段命名存在差异；
    // - 这里使用兼容布局，保证可读出线程核心字段（TID/优先级/状态等）。
    struct SystemThreadInformationRecord
    {
        LARGE_INTEGER ReservedTime[3];  // [0]=KernelTime, [1]=UserTime, [2]=CreateTime。
        ULONG WaitTime;                 // 线程等待时长计数（原始值）。
        PVOID StartAddress;             // 线程启动地址。
        CLIENT_ID ClientId;             // 线程与所属进程标识。
        KPRIORITY Priority;             // 当前动态优先级。
        LONG BasePriority;              // 基础优先级。
        ULONG ContextSwitches;          // 上下文切换次数。
        ULONG ThreadState;              // 线程状态码（KTHREAD_STATE）。
        ULONG WaitReason;               // 等待原因码（KWAIT_REASON）。
    };

    // 格式化 Win32 错误码文本，便于 UI 提示详细失败原因。
    std::string FormatLastErrorMessage(const DWORD errorCode)
    {
        wchar_t* wideBuffer = nullptr;
        const DWORD messageLength = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&wideBuffer),
            0,
            nullptr);

        if (messageLength == 0 || wideBuffer == nullptr)
        {
            std::ostringstream stream;
            stream << "Win32Error=" << errorCode;
            return stream.str();
        }

        std::wstring wideMessage(wideBuffer, messageLength);
        ::LocalFree(wideBuffer);

        std::string utf8Message = ks::str::Utf16ToUtf8(wideMessage);
        utf8Message = ks::str::TrimCopy(utf8Message);

        std::ostringstream stream;
        stream << utf8Message << " (Code=" << errorCode << ")";
        return stream.str();
    }

    // 格式化 NTSTATUS，避免 UI 只看到“失败”而不知道具体码值。
    std::string FormatNtStatusMessage(const NTSTATUS statusCode, const char* prefixText)
    {
        std::ostringstream stream;
        stream << (prefixText == nullptr ? "NTSTATUS failed" : prefixText)
            << " (0x" << std::hex << static_cast<unsigned long>(statusCode) << ")";
        return stream.str();
    }

    // 获取 ntdll 指定导出函数地址（懒加载）。
    FARPROC GetNtdllProcAddress(const char* functionName)
    {
        HMODULE ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdllModule == nullptr)
        {
            ntdllModule = ::LoadLibraryW(L"ntdll.dll");
        }
        if (ntdllModule == nullptr)
        {
            return nullptr;
        }
        return ::GetProcAddress(ntdllModule, functionName);
    }

    // 查询是否可提升某特权（SeDebug 等），用于关键进程设置等高权限操作。
    bool EnablePrivilege(const wchar_t* privilegeName)
    {
        if (privilegeName == nullptr)
        {
            return false;
        }

        HANDLE processToken = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &processToken) == FALSE)
        {
            return false;
        }

        LUID privilegeLuid{};
        if (::LookupPrivilegeValueW(nullptr, privilegeName, &privilegeLuid) == FALSE)
        {
            ::CloseHandle(processToken);
            return false;
        }

        TOKEN_PRIVILEGES tokenPrivileges{};
        tokenPrivileges.PrivilegeCount = 1;
        tokenPrivileges.Privileges[0].Luid = privilegeLuid;
        tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        const BOOL adjustResult = ::AdjustTokenPrivileges(
            processToken,
            FALSE,
            &tokenPrivileges,
            sizeof(tokenPrivileges),
            nullptr,
            nullptr);
        const DWORD adjustError = ::GetLastError();
        ::CloseHandle(processToken);

        return adjustResult != FALSE && adjustError == ERROR_SUCCESS;
    }

    // 读取进程可执行路径（UTF-8），失败返回空串。
    std::string QueryProcessPathByHandle(const HANDLE processHandle)
    {
        if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE)
        {
            return std::string();
        }

        // 路径查询优先使用 QueryFullProcessImageNameW（支持 QUERY_LIMITED_INFORMATION）。
        std::wstring pathBuffer(32768, L'\0');
        DWORD pathLength = static_cast<DWORD>(pathBuffer.size());
        if (::QueryFullProcessImageNameW(processHandle, 0, pathBuffer.data(), &pathLength) != FALSE)
        {
            pathBuffer.resize(pathLength);
            return ks::str::Utf16ToUtf8(pathBuffer);
        }

        // 回退路径：GetModuleFileNameExW（部分进程在前者失败时可读取）。
        std::wstring modulePathBuffer(32768, L'\0');
        const DWORD modulePathLength = ::GetModuleFileNameExW(
            processHandle,
            nullptr,
            modulePathBuffer.data(),
            static_cast<DWORD>(modulePathBuffer.size()));
        if (modulePathLength > 0)
        {
            modulePathBuffer.resize(modulePathLength);
            return ks::str::Utf16ToUtf8(modulePathBuffer);
        }

        return std::string();
    }

    // QueryProcessStartTimeByPid 作用：
    // - 按 PID 读取进程创建时间（FILETIME）；
    // - 供 Restart Manager 的 RM_UNIQUE_PROCESS 结构体填充使用。
    bool QueryProcessStartTimeByPid(const std::uint32_t pid, FILETIME* const creationTimeOut)
    {
        if (creationTimeOut == nullptr || pid == 0)
        {
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(pid));
        if (processHandle == nullptr)
        {
            return false;
        }

        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        const BOOL queryResult = ::GetProcessTimes(
            processHandle,
            &creationTime,
            &exitTime,
            &kernelTime,
            &userTime);
        ::CloseHandle(processHandle);
        if (queryResult == FALSE)
        {
            return false;
        }

        *creationTimeOut = creationTime;
        return true;
    }

    // QueryModuleBaseAddressBySnapshot 作用：
    // - 通过模块快照定位目标模块基址；
    // - 用于 NtUnmapViewOfSection 定位远程 ntdll.dll 映射地址。
    bool QueryModuleBaseAddressBySnapshot(
        const std::uint32_t pid,
        const wchar_t* const moduleNameText,
        void** const baseAddressOut)
    {
        if (baseAddressOut == nullptr || moduleNameText == nullptr || pid == 0)
        {
            return false;
        }
        *baseAddressOut = nullptr;

        const HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            static_cast<DWORD>(pid));
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (::Module32FirstW(snapshotHandle, &moduleEntry) == FALSE)
        {
            ::CloseHandle(snapshotHandle);
            return false;
        }

        do
        {
            if (_wcsicmp(moduleEntry.szModule, moduleNameText) == 0)
            {
                *baseAddressOut = moduleEntry.modBaseAddr;
                ::CloseHandle(snapshotHandle);
                return true;
            }
        } while (::Module32NextW(snapshotHandle, &moduleEntry) != FALSE);

        ::CloseHandle(snapshotHandle);
        return false;
    }

    // 通过令牌查询 DOMAIN\\User 形式用户名。
    std::string QueryProcessUserNameByHandle(const HANDLE processHandle)
    {
        HANDLE processToken = nullptr;
        if (::OpenProcessToken(processHandle, TOKEN_QUERY, &processToken) == FALSE)
        {
            return std::string();
        }

        DWORD requiredLength = 0;
        ::GetTokenInformation(processToken, TokenUser, nullptr, 0, &requiredLength);
        if (requiredLength == 0)
        {
            ::CloseHandle(processToken);
            return std::string();
        }

        std::vector<BYTE> tokenBuffer(requiredLength);
        if (::GetTokenInformation(processToken, TokenUser, tokenBuffer.data(), requiredLength, &requiredLength) == FALSE)
        {
            ::CloseHandle(processToken);
            return std::string();
        }

        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
        wchar_t accountName[256] = {};
        wchar_t domainName[256] = {};
        DWORD accountNameLength = static_cast<DWORD>(std::size(accountName));
        DWORD domainNameLength = static_cast<DWORD>(std::size(domainName));
        SID_NAME_USE sidUse = SidTypeUnknown;

        const BOOL lookupResult = ::LookupAccountSidW(
            nullptr,
            tokenUser->User.Sid,
            accountName,
            &accountNameLength,
            domainName,
            &domainNameLength,
            &sidUse);
        ::CloseHandle(processToken);

        if (lookupResult == FALSE)
        {
            return std::string();
        }

        std::wstring fullUserName(domainName);
        if (!fullUserName.empty())
        {
            fullUserName += L"\\";
        }
        fullUserName += accountName;
        return ks::str::Utf16ToUtf8(fullUserName);
    }

    // 查询进程令牌是否提升（管理员）。
    bool QueryProcessIsElevatedByHandle(const HANDLE processHandle)
    {
        HANDLE processToken = nullptr;
        if (::OpenProcessToken(processHandle, TOKEN_QUERY, &processToken) == FALSE)
        {
            return false;
        }

        TOKEN_ELEVATION tokenElevation{};
        DWORD returnLength = 0;
        const BOOL queryResult = ::GetTokenInformation(
            processToken,
            TokenElevation,
            &tokenElevation,
            sizeof(tokenElevation),
            &returnLength);
        ::CloseHandle(processToken);

        if (queryResult == FALSE)
        {
            return false;
        }
        return tokenElevation.TokenIsElevated != 0;
    }

    // FileSignatureInfo：单个文件签名信息聚合结构。
    // 该结构用于同时携带：
    // 1) 厂家（Publisher）；
    // 2) 是否被 Windows 信任链接受；
    // 3) UI 直接显示文本。
    struct FileSignatureInfo
    {
        bool hasSignature = false;         // 是否检测到签名。
        bool trustedByWindows = false;     // 是否被 WinVerifyTrust 判定为受信任。
        std::string publisher;             // 证书发布者/厂家名称。
        std::string displayText;           // UI 显示文本（含厂家与可信状态）。
    };

    // QueryCompanyNameByVersion 作用：
    // - 从文件版本信息读取 CompanyName；
    // - 作为“证书发布者为空”时的厂家文本兜底。
    std::string QueryCompanyNameByVersion(const std::wstring& utf16Path)
    {
        if (utf16Path.empty())
        {
            return std::string();
        }

        DWORD versionHandle = 0;
        const DWORD versionInfoSize = ::GetFileVersionInfoSizeW(utf16Path.c_str(), &versionHandle);
        if (versionInfoSize == 0)
        {
            return std::string();
        }

        std::vector<BYTE> versionInfoBuffer(versionInfoSize, 0);
        if (::GetFileVersionInfoW(
            utf16Path.c_str(),
            0,
            versionInfoSize,
            versionInfoBuffer.data()) == FALSE)
        {
            return std::string();
        }

        // 优先读取语言代码页映射，确保拿到正确本地化字符串。
        struct LangAndCodePage
        {
            WORD language = 0;
            WORD codePage = 0;
        };
        LangAndCodePage* translation = nullptr;
        UINT translationSize = 0;
        if (::VerQueryValueW(
            versionInfoBuffer.data(),
            L"\\VarFileInfo\\Translation",
            reinterpret_cast<LPVOID*>(&translation),
            &translationSize) != FALSE &&
            translation != nullptr &&
            translationSize >= sizeof(LangAndCodePage))
        {
            wchar_t queryPath[96] = {};
            std::swprintf(
                queryPath,
                std::size(queryPath),
                L"\\StringFileInfo\\%04x%04x\\CompanyName",
                translation[0].language,
                translation[0].codePage);

            LPVOID companyValue = nullptr;
            UINT companyLength = 0;
            if (::VerQueryValueW(
                versionInfoBuffer.data(),
                queryPath,
                &companyValue,
                &companyLength) != FALSE &&
                companyValue != nullptr &&
                companyLength > 0)
            {
                return ks::str::Utf16ToUtf8(static_cast<const wchar_t*>(companyValue));
            }
        }

        // 语言映射缺失时，尝试常见英语代码页兜底。
        LPVOID fallbackValue = nullptr;
        UINT fallbackLength = 0;
        if (::VerQueryValueW(
            versionInfoBuffer.data(),
            L"\\StringFileInfo\\040904B0\\CompanyName",
            &fallbackValue,
            &fallbackLength) != FALSE &&
            fallbackValue != nullptr &&
            fallbackLength > 0)
        {
            return ks::str::Utf16ToUtf8(static_cast<const wchar_t*>(fallbackValue));
        }

        return std::string();
    }

    // QueryFileSignatureInfo 作用：
    // - 用 WinVerifyTrust 判断文件是否“Windows 信任”；
    // - 解析签名链获取发布者（厂家）；
    // - 构建统一显示文本，供表格与详情窗口直接展示。
    FileSignatureInfo QueryFileSignatureInfo(const std::string& utf8Path)
    {
        FileSignatureInfo signatureInfo{};
        signatureInfo.displayText = "Unknown";

        if (utf8Path.empty())
        {
            return signatureInfo;
        }

        const std::wstring utf16Path = ks::str::Utf8ToUtf16(utf8Path);
        if (utf16Path.empty())
        {
            return signatureInfo;
        }

        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = utf16Path.c_str();

        WINTRUST_DATA trustData{};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;

        GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG verifyResult = ::WinVerifyTrust(nullptr, &policyGuid, &trustData);

        // 尝试通过签名链提取发布者名称（厂家）。
        if (trustData.hWVTStateData != nullptr)
        {
            CRYPT_PROVIDER_DATA* providerData = ::WTHelperProvDataFromStateData(trustData.hWVTStateData);
            if (providerData != nullptr)
            {
                CRYPT_PROVIDER_SGNR* signer = ::WTHelperGetProvSignerFromChain(providerData, 0, FALSE, 0);
                if (signer != nullptr && signer->csCertChain > 0 && signer->pasCertChain != nullptr)
                {
                    const CERT_CONTEXT* certificateContext = signer->pasCertChain[0].pCert;
                    if (certificateContext != nullptr)
                    {
                        wchar_t publisherBuffer[512] = {};
                        const DWORD publisherLength = ::CertGetNameStringW(
                            certificateContext,
                            CERT_NAME_SIMPLE_DISPLAY_TYPE,
                            0,
                            nullptr,
                            publisherBuffer,
                            static_cast<DWORD>(std::size(publisherBuffer)));
                        if (publisherLength > 1)
                        {
                            signatureInfo.publisher = ks::str::Utf16ToUtf8(publisherBuffer);
                        }
                    }
                }
            }
        }

        // 无论校验结果如何，都需要关闭 WinVerifyTrust 的状态句柄，避免泄漏。
        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        ::WinVerifyTrust(nullptr, &policyGuid, &trustData);

        // 若证书链里拿不到发布者，则回退到文件版本 CompanyName。
        if (signatureInfo.publisher.empty())
        {
            signatureInfo.publisher = QueryCompanyNameByVersion(utf16Path);
        }
        signatureInfo.publisher = ks::str::TrimCopy(signatureInfo.publisher);

        if (verifyResult == ERROR_SUCCESS)
        {
            signatureInfo.hasSignature = true;
            signatureInfo.trustedByWindows = true;
            if (signatureInfo.publisher.empty())
            {
                signatureInfo.publisher = "Unknown Publisher";
            }
            signatureInfo.displayText = signatureInfo.publisher + " (Trusted)";
            return signatureInfo;
        }

        if (verifyResult == TRUST_E_NOSIGNATURE || verifyResult == TRUST_E_SUBJECT_FORM_UNKNOWN)
        {
            signatureInfo.hasSignature = false;
            signatureInfo.trustedByWindows = false;
            signatureInfo.displayText = "Unsigned";
            return signatureInfo;
        }

        // 其他失败码统一视为“有签名但不受信任”。
        signatureInfo.hasSignature = true;
        signatureInfo.trustedByWindows = false;
        if (signatureInfo.publisher.empty())
        {
            signatureInfo.publisher = "Unknown Publisher";
        }
        signatureInfo.displayText = signatureInfo.publisher + " (Untrusted)";
        return signatureInfo;
    }

    // ReadRemoteMemoryExact：
    // - 从目标进程读取固定长度内存；
    // - 输入 processHandle 为目标进程句柄，remoteAddress 为远程地址；
    // - 输入 localBuffer/localSize 为本地缓冲；
    // - 返回 true 表示完整读取 localSize 字节，false 表示失败或部分读取。
    bool ReadRemoteMemoryExact(
        const HANDLE processHandle,
        const std::uint64_t remoteAddress,
        void* localBuffer,
        const SIZE_T localSize)
    {
        if (processHandle == nullptr || remoteAddress == 0 || localBuffer == nullptr || localSize == 0)
        {
            return false;
        }

        SIZE_T bytesRead = 0;
        const BOOL readOk = ::ReadProcessMemory(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(remoteAddress)),
            localBuffer,
            localSize,
            &bytesRead);
        return readOk != FALSE && bytesRead == localSize;
    }

    // ReadRemoteStructure：
    // - 模板化读取远程结构；
    // - 读取前清空输出结构，失败后调用者不会看到脏数据；
    // - 返回 true 表示读取完整结构成功。
    template<typename T>
    bool ReadRemoteStructure(
        const HANDLE processHandle,
        const std::uint64_t remoteAddress,
        T& valueOut)
    {
        std::memset(&valueOut, 0, sizeof(T));
        return ReadRemoteMemoryExact(
            processHandle,
            remoteAddress,
            &valueOut,
            static_cast<SIZE_T>(sizeof(T)));
    }

    // ReadRemoteUnicodeStringByAddress：
    // - 通过远程 UTF-16 地址和字节长度读取字符串；
    // - 对 Length 做偶数和上限校验，避免目标进程损坏字段拖垮枚举线程；
    // - 返回 UTF-8 文本，失败返回空字符串。
    std::string ReadRemoteUnicodeStringByAddress(
        const HANDLE processHandle,
        const std::uint64_t bufferAddress,
        const USHORT lengthBytes)
    {
        if (processHandle == nullptr || bufferAddress == 0 || lengthBytes == 0)
        {
            return std::string();
        }
        if ((lengthBytes % sizeof(wchar_t)) != 0 ||
            static_cast<std::size_t>(lengthBytes) > RemoteUnicodeStringMaxBytes)
        {
            return std::string();
        }

        std::wstring textBuffer(
            static_cast<std::size_t>(lengthBytes / sizeof(wchar_t)),
            L'\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ::ReadProcessMemory(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(bufferAddress)),
            textBuffer.data(),
            static_cast<SIZE_T>(lengthBytes),
            &bytesRead);
        if (readOk == FALSE || bytesRead < sizeof(wchar_t))
        {
            return std::string();
        }

        const std::size_t charCount = static_cast<std::size_t>(
            std::min<SIZE_T>(bytesRead, static_cast<SIZE_T>(lengthBytes)) / sizeof(wchar_t));
        if (charCount < textBuffer.size())
        {
            textBuffer.resize(charCount);
        }
        return ks::str::Utf16ToUtf8(textBuffer);
    }

    // ReadRemoteUnicodeString64：
    // - 读取 64 位目标内的 UNICODE_STRING；
    // - 输入 remoteUnicode 是从远程结构体复制出的快照；
    // - 返回 UTF-8 文本，失败返回空字符串。
    std::string ReadRemoteUnicodeString64(
        const HANDLE processHandle,
        const UNICODE_STRING& remoteUnicode)
    {
        return ReadRemoteUnicodeStringByAddress(
            processHandle,
            reinterpret_cast<std::uint64_t>(remoteUnicode.Buffer),
            remoteUnicode.Length);
    }

    // ReadRemoteUnicodeString32：
    // - 读取 32 位目标内的 UNICODE_STRING；
    // - 输入 remoteUnicode 使用手工 32 位布局，避免指针宽度错位；
    // - 返回 UTF-8 文本，失败返回空字符串。
    std::string ReadRemoteUnicodeString32(
        const HANDLE processHandle,
        const RemoteUnicodeString32& remoteUnicode)
    {
        return ReadRemoteUnicodeStringByAddress(
            processHandle,
            static_cast<std::uint64_t>(remoteUnicode.buffer),
            remoteUnicode.length);
    }

    // QueryProcessCommandLineByNtInfo：
    // - 使用 ProcessCommandLineInformation 查询命令行；
    // - 新系统可直接返回 UNICODE_STRING + 字符串缓冲；
    // - 返回 UTF-8 文本，失败返回空字符串并交给 PEB 回退路径。
    std::string QueryProcessCommandLineByNtInfo(
        const NtQueryInformationProcessFn ntQueryInformationProcess,
        const HANDLE processHandle)
    {
        if (ntQueryInformationProcess == nullptr || processHandle == nullptr)
        {
            return std::string();
        }

        ULONG requiredLength = 0;
        NTSTATUS firstStatus = ntQueryInformationProcess(
            processHandle,
            ProcessCommandLineInformationClass,
            nullptr,
            0,
            &requiredLength);
        if (!NT_SUCCESS(firstStatus) && requiredLength == 0)
        {
            return std::string();
        }
        if (requiredLength < sizeof(UNICODE_STRING))
        {
            requiredLength = static_cast<ULONG>(sizeof(UNICODE_STRING) + 512);
        }

        std::vector<std::uint8_t> queryBuffer(requiredLength + sizeof(wchar_t), 0);
        NTSTATUS secondStatus = ntQueryInformationProcess(
            processHandle,
            ProcessCommandLineInformationClass,
            queryBuffer.data(),
            static_cast<ULONG>(queryBuffer.size()),
            &requiredLength);
        if (!NT_SUCCESS(secondStatus))
        {
            return std::string();
        }

        const auto* commandUnicode = reinterpret_cast<const UNICODE_STRING*>(queryBuffer.data());
        if (commandUnicode == nullptr || commandUnicode->Length == 0 || commandUnicode->Buffer == nullptr)
        {
            return std::string();
        }

        const std::uintptr_t bufferBegin = reinterpret_cast<std::uintptr_t>(queryBuffer.data());
        const std::uintptr_t bufferSize = queryBuffer.size();
        const std::uintptr_t textPtr = reinterpret_cast<std::uintptr_t>(commandUnicode->Buffer);
        const std::size_t textBytes = static_cast<std::size_t>(commandUnicode->Length);
        if ((textBytes % sizeof(wchar_t)) != 0 || textBytes > RemoteUnicodeStringMaxBytes)
        {
            return std::string();
        }

        if (textPtr >= bufferBegin &&
            textPtr - bufferBegin <= bufferSize &&
            textBytes <= bufferSize - (textPtr - bufferBegin))
        {
            const auto* wideText = reinterpret_cast<const wchar_t*>(textPtr);
            return ks::str::Utf16ToUtf8(std::wstring(
                wideText,
                wideText + static_cast<std::size_t>(commandUnicode->Length / sizeof(wchar_t))));
        }

        return ReadRemoteUnicodeString64(processHandle, *commandUnicode);
    }

    // QueryProcessCommandLineByPeb64：
    // - 按 64 位 PEB 布局读取 ProcessParameters.CommandLine；
    // - 用于 ProcessCommandLineInformation 失败后的回退；
    // - 返回 UTF-8 文本，失败返回空字符串。
    std::string QueryProcessCommandLineByPeb64(
        const HANDLE processHandle,
        const std::uint64_t pebAddress)
    {
        Peb64CommandLineLite pebSnapshot{};
        if (!ReadRemoteStructure(processHandle, pebAddress, pebSnapshot) ||
            pebSnapshot.processParameters == nullptr)
        {
            return std::string();
        }

        RtlUserProcessParameters64CommandLine processParameters{};
        if (!ReadRemoteStructure(
            processHandle,
            reinterpret_cast<std::uint64_t>(pebSnapshot.processParameters),
            processParameters))
        {
            return std::string();
        }

        return ReadRemoteUnicodeString64(processHandle, processParameters.commandLine);
    }

    // QueryProcessCommandLineByPeb32：
    // - 按 32 位 Wow64 PEB 布局读取 ProcessParameters.CommandLine；
    // - 修正 x64 控制进程读取 32 位目标时的结构偏移错位；
    // - 返回 UTF-8 文本，失败返回空字符串。
    std::string QueryProcessCommandLineByPeb32(
        const HANDLE processHandle,
        const std::uint64_t pebAddress)
    {
        Peb32CommandLineLite pebSnapshot{};
        if (!ReadRemoteStructure(processHandle, pebAddress, pebSnapshot) ||
            pebSnapshot.processParameters == 0)
        {
            return std::string();
        }

        RtlUserProcessParameters32CommandLine processParameters{};
        if (!ReadRemoteStructure(
            processHandle,
            static_cast<std::uint64_t>(pebSnapshot.processParameters),
            processParameters))
        {
            return std::string();
        }

        return ReadRemoteUnicodeString32(processHandle, processParameters.commandLine);
    }

    // 从远程进程读取命令行（读取 PEB / ProcessParameters）。
    std::string QueryProcessCommandLineByHandle(const HANDLE processHandle)
    {
        // 命令行读取顺序：
        // 1) 优先使用 ProcessCommandLineInformation，让系统处理结构差异；
        // 2) 再读 Native PEB，覆盖旧系统或受限信息类；
        // 3) 最后读 Wow64 PEB，修复 x64 工具读取 32 位目标时的偏移错位。
        const auto ntQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
            GetNtdllProcAddress("NtQueryInformationProcess"));
        if (ntQueryInformationProcess == nullptr)
        {
            return std::string();
        }

        const std::string commandLineByInfo = QueryProcessCommandLineByNtInfo(
            ntQueryInformationProcess,
            processHandle);
        if (!commandLineByInfo.empty())
        {
            return commandLineByInfo;
        }

        ProcessBasicInformationLocal basicInfo{};
        NTSTATUS queryStatus = ntQueryInformationProcess(
            processHandle,
            ProcessBasicInformation,
            &basicInfo,
            static_cast<ULONG>(sizeof(basicInfo)),
            nullptr);
        if (!NT_SUCCESS(queryStatus) || basicInfo.pebBaseAddress == nullptr)
        {
            return std::string();
        }

        const std::string commandLineByNativePeb = QueryProcessCommandLineByPeb64(
            processHandle,
            reinterpret_cast<std::uint64_t>(basicInfo.pebBaseAddress));
        if (!commandLineByNativePeb.empty())
        {
            return commandLineByNativePeb;
        }

        ULONG_PTR wow64PebAddress = 0;
        NTSTATUS wow64Status = ntQueryInformationProcess(
            processHandle,
            ProcessWow64InformationClass,
            &wow64PebAddress,
            static_cast<ULONG>(sizeof(wow64PebAddress)),
            nullptr);
        if (NT_SUCCESS(wow64Status) &&
            wow64PebAddress != 0 &&
            wow64PebAddress != reinterpret_cast<ULONG_PTR>(basicInfo.pebBaseAddress))
        {
            return QueryProcessCommandLineByPeb32(
                processHandle,
                static_cast<std::uint64_t>(wow64PebAddress));
        }

        return std::string();
    }

    // 把 PID 转成 DWORD，统一并避免隐式窄化警告。
    DWORD ToDwordPid(const std::uint32_t pid)
    {
        return static_cast<DWORD>(pid);
    }

    // 按路径提取文件名（进程名兜底用）。
    std::string ExtractFileNameFromPath(const std::string& utf8Path)
    {
        if (utf8Path.empty())
        {
            return std::string();
        }
        const std::size_t lastSlash = utf8Path.find_last_of("\\/");
        if (lastSlash == std::string::npos)
        {
            return utf8Path;
        }
        return utf8Path.substr(lastSlash + 1);
    }

    // 把进程优先级常量转换为可读文本。
    std::string PriorityClassToText(const DWORD priorityClass)
    {
        switch (priorityClass)
        {
        case IDLE_PRIORITY_CLASS:
            return "Idle";
        case BELOW_NORMAL_PRIORITY_CLASS:
            return "BelowNormal";
        case NORMAL_PRIORITY_CLASS:
            return "Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS:
            return "AboveNormal";
        case HIGH_PRIORITY_CLASS:
            return "High";
        case REALTIME_PRIORITY_CLASS:
            return "Realtime";
        default:
            return "Unknown";
        }
    }

    // 查询进程当前优先级文本。
    std::string QueryPriorityTextByHandle(const HANDLE processHandle)
    {
        const DWORD priorityClass = ::GetPriorityClass(processHandle);
        if (priorityClass == 0)
        {
            return "Unknown";
        }
        return PriorityClassToText(priorityClass);
    }
    // ProcessPowerThrottlingStateNative：
    // - 兼容旧 SDK 的本地结构声明；
    // - controlMask/stateMask 的 ExecutionSpeed 位代表 Windows 效率模式。
    struct ProcessPowerThrottlingStateNative
    {
        ULONG version = 0;
        ULONG controlMask = 0;
        ULONG stateMask = 0;
    };

    // ProcessProtectionLevelInformationNative：
    // - 兼容旧 SDK 的本地结构声明；
    // - protectionLevel 保存 PROTECTION_LEVEL_* 枚举。
    struct ProcessProtectionLevelInformationNative
    {
        DWORD protectionLevel = 0;
    };

    // ProcessProtectionLevelToText 作用：把公开枚举值翻译成 UI 可读文本。
    std::string ProcessProtectionLevelToText(const DWORD protectionLevel)
    {
        // protectionLevel 用途：GetProcessInformation 返回的原始枚举值。
        // 返回值：包含名称与十六进制值，方便用户与 WinAPI 文档对照。
        switch (protectionLevel)
        {
        case ProcessProtectionLevelNone:
            return "None (PROTECTION_LEVEL_NONE, 0xFFFFFFFE)";
        case ProcessProtectionLevelWinTcbLight:
            return "WinTcbLight (PROTECTION_LEVEL_WINTCB_LIGHT, 0x00000000)";
        case ProcessProtectionLevelWindows:
            return "Windows (PROTECTION_LEVEL_WINDOWS, 0x00000001)";
        case ProcessProtectionLevelWindowsLight:
            return "WindowsLight (PROTECTION_LEVEL_WINDOWS_LIGHT, 0x00000002)";
        case ProcessProtectionLevelAntimalwareLight:
            return "AntimalwareLight (PROTECTION_LEVEL_ANTIMALWARE_LIGHT, 0x00000003)";
        case ProcessProtectionLevelLsaLight:
            return "LsaLight (PROTECTION_LEVEL_LSA_LIGHT, 0x00000004)";
        case ProcessProtectionLevelWinTcb:
            return "WinTcb (PROTECTION_LEVEL_WINTCB, 0x00000005)";
        case ProcessProtectionLevelCodegenLight:
            return "CodegenLight (PROTECTION_LEVEL_CODEGEN_LIGHT, 0x00000006)";
        case ProcessProtectionLevelAuthenticode:
            return "Authenticode (PROTECTION_LEVEL_AUTHENTICODE, 0x00000007)";
        case ProcessProtectionLevelPplApp:
            return "PplApp (PROTECTION_LEVEL_PPL_APP, 0x00000008)";
        case ProcessProtectionLevelSame:
            return "Same (PROTECTION_LEVEL_SAME, 0xFFFFFFFF)";
        default:
            break;
        }

        std::ostringstream textBuilder;
        textBuilder << "Unknown (0x"
            << std::hex
            << std::uppercase
            << std::setw(8)
            << std::setfill('0')
            << static_cast<unsigned long>(protectionLevel)
            << ")";
        return textBuilder.str();
    }

    // QueryProcessEfficiencyModeByHandle 作用：读取目标进程效率模式状态。
    bool QueryProcessEfficiencyModeByHandle(
        const HANDLE processHandle,
        bool* const enabledOut,
        std::string* const errorMessage)
    {
        if (enabledOut != nullptr)
        {
            *enabledOut = false;
        }
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Process handle is null.";
            }
            return false;
        }

        HMODULE kernel32Module = ::GetModuleHandleW(L"kernel32.dll");
        const auto getProcessInformation = reinterpret_cast<GetProcessInformationFn>(
            kernel32Module != nullptr ? ::GetProcAddress(kernel32Module, "GetProcessInformation") : nullptr);
        if (getProcessInformation == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "GetProcessInformation(ProcessPowerThrottling) is not available.";
            }
            return false;
        }

        ProcessPowerThrottlingStateNative powerState{};
        powerState.version = ProcessPowerThrottlingCurrentVersion;
        const BOOL queryOk = getProcessInformation(
            processHandle,
            ProcessPowerThrottlingInfoClass,
            &powerState,
            static_cast<DWORD>(sizeof(powerState)));
        if (queryOk == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "GetProcessInformation(ProcessPowerThrottling) failed: "
                    + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        if (enabledOut != nullptr)
        {
            *enabledOut =
                (powerState.controlMask & ProcessPowerThrottlingExecutionSpeed) != 0 &&
                (powerState.stateMask & ProcessPowerThrottlingExecutionSpeed) != 0;
        }
        return true;
    }


    // 按 machine 常量转换为架构文本。
    std::string MachineToArchitectureText(const USHORT machineType)
    {
        switch (machineType)
        {
        case IMAGE_FILE_MACHINE_AMD64:
            return "x64";
        case IMAGE_FILE_MACHINE_I386:
            return "x86";
        case IMAGE_FILE_MACHINE_ARM64:
            return "ARM64";
        case IMAGE_FILE_MACHINE_ARM:
            return "ARM";
        default:
            return "Unknown";
        }
    }

    // 查询目标进程架构（优先 IsWow64Process2）。
    std::string QueryProcessArchitectureByHandle(const HANDLE processHandle)
    {
        using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const auto isWow64Process2Fn = reinterpret_cast<IsWow64Process2Fn>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
        if (isWow64Process2Fn != nullptr)
        {
            USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
            USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
            if (isWow64Process2Fn(processHandle, &processMachine, &nativeMachine) != FALSE)
            {
                // processMachine=UNKNOWN 表示“与系统原生架构一致”。
                if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN)
                {
                    return MachineToArchitectureText(nativeMachine);
                }
                return MachineToArchitectureText(processMachine);
            }
        }

        // 回退旧 API：只能区分 WOW64 x86 与“非 WOW64”。
        BOOL isWow64Process = FALSE;
        if (::IsWow64Process(processHandle, &isWow64Process) == FALSE)
        {
            return "Unknown";
        }

#if defined(_WIN64)
        return isWow64Process ? "x86" : "x64";
#else
        return isWow64Process ? "x86" : "x86";
#endif
    }

    // 读取 PE 头并返回 AddressOfEntryPoint（RVA）。
    std::uint32_t QueryImageEntryPointRvaByPath(const std::string& modulePath)
    {
        if (modulePath.empty())
        {
            return 0;
        }

        std::ifstream moduleFile(modulePath, std::ios::binary);
        if (!moduleFile.is_open())
        {
            return 0;
        }

        IMAGE_DOS_HEADER dosHeader{};
        moduleFile.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
        if (!moduleFile.good() || dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }

        moduleFile.seekg(static_cast<std::streamoff>(dosHeader.e_lfanew), std::ios::beg);
        DWORD ntSignature = 0;
        moduleFile.read(reinterpret_cast<char*>(&ntSignature), sizeof(ntSignature));
        if (!moduleFile.good() || ntSignature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }

        IMAGE_FILE_HEADER fileHeader{};
        moduleFile.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
        if (!moduleFile.good())
        {
            return 0;
        }

        WORD optionalMagic = 0;
        moduleFile.read(reinterpret_cast<char*>(&optionalMagic), sizeof(optionalMagic));
        if (!moduleFile.good())
        {
            return 0;
        }
        moduleFile.seekg(-static_cast<std::streamoff>(sizeof(optionalMagic)), std::ios::cur);

        if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER64 optionalHeader{};
            moduleFile.read(reinterpret_cast<char*>(&optionalHeader), sizeof(optionalHeader));
            if (!moduleFile.good())
            {
                return 0;
            }
            return optionalHeader.AddressOfEntryPoint;
        }

        if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 optionalHeader{};
            moduleFile.read(reinterpret_cast<char*>(&optionalHeader), sizeof(optionalHeader));
            if (!moduleFile.good())
            {
                return 0;
            }
            return optionalHeader.AddressOfEntryPoint;
        }

        return 0;
    }

    // 把线程 ID 列表压缩为字符串，避免单元格过长。
    std::string BuildThreadIdSummaryText(const std::vector<std::uint32_t>& threadIds)
    {
        if (threadIds.empty())
        {
            return "-";
        }

        constexpr std::size_t MaxDisplayCount = 8;
        std::ostringstream stream;
        const std::size_t displayCount = std::min(MaxDisplayCount, threadIds.size());
        for (std::size_t index = 0; index < displayCount; ++index)
        {
            if (index > 0)
            {
                stream << ", ";
            }
            stream << threadIds[index];
        }
        if (threadIds.size() > MaxDisplayCount)
        {
            stream << " ... (+" << (threadIds.size() - MaxDisplayCount) << ")";
        }
        return stream.str();
    }

    // 路径是否指向目录。
    bool IsDirectoryPath(const std::string& utf8Path)
    {
        if (utf8Path.empty())
        {
            return false;
        }

        const std::wstring utf16Path = ks::str::Utf8ToUtf16(utf8Path);
        if (utf16Path.empty())
        {
            return false;
        }

        std::error_code errorCode;
        return std::filesystem::is_directory(std::filesystem::path(utf16Path), errorCode);
    }

    // 在 Explorer 中打开文件夹或定位文件。
    bool OpenInExplorerByPath(const std::string& targetPath, std::string* const errorMessage)
    {
        if (targetPath.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Target path is empty.";
            }
            return false;
        }

        const std::wstring utf16Path = ks::str::Utf8ToUtf16(targetPath);
        if (utf16Path.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Path UTF-8 -> UTF-16 conversion failed.";
            }
            return false;
        }

        HINSTANCE shellResult = nullptr;
        if (IsDirectoryPath(targetPath))
        {
            shellResult = ::ShellExecuteW(
                nullptr,
                L"open",
                utf16Path.c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
        }
        else
        {
            std::wstring parameters = L"/select,\"" + utf16Path + L"\"";
            shellResult = ::ShellExecuteW(
                nullptr,
                L"open",
                L"explorer.exe",
                parameters.c_str(),
                nullptr,
                SW_SHOWNORMAL);
        }

        if (reinterpret_cast<std::intptr_t>(shellResult) > 32)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            std::ostringstream stream;
            stream << "ShellExecute(explorer) failed, code=" << reinterpret_cast<std::intptr_t>(shellResult);
            *errorMessage = stream.str();
        }
        return false;
    }

    // 统一默认令牌访问掩码（用于“按 PID 打开并创建新进程”场景）。
    constexpr DWORD DefaultTokenDesiredAccess =
        TOKEN_QUERY |
        TOKEN_DUPLICATE |
        TOKEN_ASSIGN_PRIMARY |
        TOKEN_ADJUST_PRIVILEGES |
        TOKEN_ADJUST_DEFAULT |
        TOKEN_ADJUST_SESSIONID;

    // 把 64 位整数和 HANDLE 互转，避免重复 reinterpret_cast。
    HANDLE Uint64ToHandle(const std::uint64_t rawValue)
    {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(rawValue));
    }

    std::uint64_t HandleToUint64(const HANDLE handleValue)
    {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handleValue));
    }

    // 把 UI 传入的多行 KEY=VALUE 转为 Unicode 环境块（双 \0 结尾）。
    std::vector<wchar_t> BuildUnicodeEnvironmentBlock(const std::vector<std::string>& entries)
    {
        std::vector<wchar_t> environmentBlock;
        for (const std::string& entryText : entries)
        {
            const std::string trimmedEntry = ks::str::TrimCopy(entryText);
            if (trimmedEntry.empty())
            {
                continue;
            }

            const std::wstring wideEntry = ks::str::Utf8ToUtf16(trimmedEntry);
            if (wideEntry.empty())
            {
                continue;
            }

            environmentBlock.insert(environmentBlock.end(), wideEntry.begin(), wideEntry.end());
            environmentBlock.push_back(L'\0');
        }

        // 环境块必须以双 \0 结束，即使没有任何条目也要保证格式正确。
        environmentBlock.push_back(L'\0');
        if (environmentBlock.size() == 1)
        {
            environmentBlock.push_back(L'\0');
        }
        return environmentBlock;
    }

    // SECURITY_ATTRIBUTES 构建：false 表示调用层应传 nullptr。
    bool BuildSecurityAttributes(
        const ks::process::SecurityAttributesInput& inputValue,
        SECURITY_ATTRIBUTES& outputValue)
    {
        if (!inputValue.useValue)
        {
            return false;
        }

        outputValue = {};
        outputValue.nLength = inputValue.nLength == 0
            ? static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES))
            : static_cast<DWORD>(inputValue.nLength);
        outputValue.lpSecurityDescriptor = reinterpret_cast<LPVOID>(
            static_cast<std::uintptr_t>(inputValue.securityDescriptor));
        outputValue.bInheritHandle = inputValue.inheritHandle ? TRUE : FALSE;
        return true;
    }

    // STARTUPINFOW 字符串缓存，保证 CreateProcess 调用期间指针有效。
    struct StartupInfoBufferSet
    {
        std::wstring reservedText;
        std::wstring desktopText;
        std::wstring titleText;
    };

    // STARTUPINFOW 构建：false 表示调用层应传 nullptr。
    bool BuildStartupInfo(
        const ks::process::StartupInfoInput& inputValue,
        STARTUPINFOW& outputValue,
        StartupInfoBufferSet& bufferSet)
    {
        if (!inputValue.useValue)
        {
            return false;
        }

        outputValue = {};
        outputValue.cb = inputValue.cb == 0
            ? static_cast<DWORD>(sizeof(STARTUPINFOW))
            : static_cast<DWORD>(inputValue.cb);

        if (!inputValue.lpReserved.empty())
        {
            bufferSet.reservedText = ks::str::Utf8ToUtf16(inputValue.lpReserved);
            if (!bufferSet.reservedText.empty())
            {
                outputValue.lpReserved = bufferSet.reservedText.data();
            }
        }
        if (!inputValue.lpDesktop.empty())
        {
            bufferSet.desktopText = ks::str::Utf8ToUtf16(inputValue.lpDesktop);
            if (!bufferSet.desktopText.empty())
            {
                outputValue.lpDesktop = bufferSet.desktopText.data();
            }
        }
        if (!inputValue.lpTitle.empty())
        {
            bufferSet.titleText = ks::str::Utf8ToUtf16(inputValue.lpTitle);
            if (!bufferSet.titleText.empty())
            {
                outputValue.lpTitle = bufferSet.titleText.data();
            }
        }

        outputValue.dwX = static_cast<DWORD>(inputValue.dwX);
        outputValue.dwY = static_cast<DWORD>(inputValue.dwY);
        outputValue.dwXSize = static_cast<DWORD>(inputValue.dwXSize);
        outputValue.dwYSize = static_cast<DWORD>(inputValue.dwYSize);
        outputValue.dwXCountChars = static_cast<DWORD>(inputValue.dwXCountChars);
        outputValue.dwYCountChars = static_cast<DWORD>(inputValue.dwYCountChars);
        outputValue.dwFillAttribute = static_cast<DWORD>(inputValue.dwFillAttribute);
        outputValue.dwFlags = static_cast<DWORD>(inputValue.dwFlags);
        outputValue.wShowWindow = static_cast<WORD>(inputValue.wShowWindow);
        outputValue.cbReserved2 = static_cast<WORD>(inputValue.cbReserved2);
        outputValue.lpReserved2 = reinterpret_cast<LPBYTE>(
            static_cast<std::uintptr_t>(inputValue.lpReserved2));
        outputValue.hStdInput = Uint64ToHandle(inputValue.hStdInput);
        outputValue.hStdOutput = Uint64ToHandle(inputValue.hStdOutput);
        outputValue.hStdError = Uint64ToHandle(inputValue.hStdError);
        return true;
    }

    // PROCESS_INFORMATION 构建：false 表示调用层应传 nullptr。
    bool BuildProcessInfoInput(
        const ks::process::ProcessInformationInput& inputValue,
        PROCESS_INFORMATION& outputValue)
    {
        if (!inputValue.useValue)
        {
            return false;
        }

        outputValue = {};
        outputValue.hProcess = Uint64ToHandle(inputValue.hProcess);
        outputValue.hThread = Uint64ToHandle(inputValue.hThread);
        outputValue.dwProcessId = static_cast<DWORD>(inputValue.dwProcessId);
        outputValue.dwThreadId = static_cast<DWORD>(inputValue.dwThreadId);
        return true;
    }

    // 打开指定 PID 的进程令牌。
    bool OpenTokenByProcessPid(
        const std::uint32_t pid,
        const DWORD desiredAccess,
        HANDLE& tokenOut,
        std::string* const errorMessage)
    {
        tokenOut = nullptr;
        if (pid == 0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "tokenSourcePid cannot be 0.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for token) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const BOOL openTokenOk = ::OpenProcessToken(processHandle, desiredAccess, &tokenOut);
        const DWORD openTokenError = ::GetLastError();
        ::CloseHandle(processHandle);
        if (openTokenOk == FALSE || tokenOut == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcessToken failed: " + FormatLastErrorMessage(openTokenError);
            }
            return false;
        }
        return true;
    }

    // 可选把源令牌复制成 Primary Token（CreateProcessAsUserW 常用）。
    bool DuplicatePrimaryToken(
        const HANDLE sourceToken,
        const DWORD desiredAccess,
        HANDLE& duplicatedTokenOut,
        std::string* const errorMessage)
    {
        duplicatedTokenOut = nullptr;
        const BOOL duplicateOk = ::DuplicateTokenEx(
            sourceToken,
            desiredAccess,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &duplicatedTokenOut);
        if (duplicateOk == FALSE || duplicatedTokenOut == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "DuplicateTokenEx(TokenPrimary) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }
        return true;
    }

    // 对单个特权执行 AdjustTokenPrivileges 调整。
    bool ApplySinglePrivilegeEdit(
        const HANDLE tokenHandle,
        const ks::process::TokenPrivilegeEdit& privilegeEdit,
        std::string* const errorMessage)
    {
        if (privilegeEdit.action == ks::process::TokenPrivilegeAction::Keep)
        {
            return true;
        }

        const std::string privilegeNameUtf8 = ks::str::TrimCopy(privilegeEdit.privilegeName);
        if (privilegeNameUtf8.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Privilege name is empty.";
            }
            return false;
        }

        const std::wstring privilegeNameWide = ks::str::Utf8ToUtf16(privilegeNameUtf8);
        if (privilegeNameWide.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Privilege name UTF-8 -> UTF-16 conversion failed: " + privilegeNameUtf8;
            }
            return false;
        }

        LUID privilegeLuid{};
        if (::LookupPrivilegeValueW(nullptr, privilegeNameWide.c_str(), &privilegeLuid) == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "LookupPrivilegeValue failed(" + privilegeNameUtf8 + "): " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        TOKEN_PRIVILEGES tokenPrivileges{};
        tokenPrivileges.PrivilegeCount = 1;
        tokenPrivileges.Privileges[0].Luid = privilegeLuid;
        switch (privilegeEdit.action)
        {
        case ks::process::TokenPrivilegeAction::Enable:
            tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            break;
        case ks::process::TokenPrivilegeAction::Disable:
            tokenPrivileges.Privileges[0].Attributes = 0;
            break;
        case ks::process::TokenPrivilegeAction::Remove:
            tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;
            break;
        case ks::process::TokenPrivilegeAction::Keep:
        default:
            tokenPrivileges.Privileges[0].Attributes = 0;
            break;
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::AdjustTokenPrivileges(
            tokenHandle,
            FALSE,
            &tokenPrivileges,
            static_cast<DWORD>(sizeof(tokenPrivileges)),
            nullptr,
            nullptr) == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "AdjustTokenPrivileges failed(" + privilegeNameUtf8 + "): " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const DWORD adjustError = ::GetLastError();
        if (adjustError != ERROR_SUCCESS)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "AdjustTokenPrivileges returned error(" + privilegeNameUtf8 + "): " + FormatLastErrorMessage(adjustError);
            }
            return false;
        }
        return true;
    }

    // 批量应用特权调整。
    bool ApplyPrivilegeEdits(
        const HANDLE tokenHandle,
        const std::vector<ks::process::TokenPrivilegeEdit>& edits,
        std::string* const errorMessage)
    {
        for (std::size_t editIndex = 0; editIndex < edits.size(); ++editIndex)
        {
            if (!ApplySinglePrivilegeEdit(tokenHandle, edits[editIndex], errorMessage))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "PrivilegeEdit[" + std::to_string(editIndex) + "] failed: " + *errorMessage;
                }
                return false;
            }
        }
        return true;
    }
}

namespace ks::process
{
    std::string BuildProcessIdentityKey(const std::uint32_t pid, const std::uint64_t creationTime100ns)
    {
        // identity 规则：PID#CreationTime100ns。
        return std::to_string(pid) + "#" + std::to_string(creationTime100ns);
    }

    bool RefreshProcessDynamicCounters(ProcessRecord& processRecord)
    {
        // PID 为 0/4 等系统保留进程时，很多 API 可能无法打开句柄。
        if (processRecord.pid == 0)
        {
            processRecord.dynamicCountersReady = false;
            processRecord.ramMB = 0.0;
            processRecord.diskMBps = 0.0;
            processRecord.cpuPercent = 0.0;
            processRecord.gpuPercent = 0.0;
            processRecord.netKBps = 0.0;
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            ToDwordPid(processRecord.pid));
        if (processHandle == nullptr)
        {
            // 降级路径：
            // - 某些受保护进程拒绝 PROCESS_VM_READ，但仍可能允许受限查询；
            // - 资源视图的“句柄数”只依赖 PROCESS_QUERY_LIMITED_INFORMATION，单独尝试一次。
            const HANDLE limitedProcessHandle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                ToDwordPid(processRecord.pid));
            if (limitedProcessHandle != nullptr)
            {
                DWORD processHandleCount = 0;
                if (::GetProcessHandleCount(limitedProcessHandle, &processHandleCount) != FALSE)
                {
                    processRecord.handleCount = static_cast<std::uint32_t>(processHandleCount);
                }
                ::CloseHandle(limitedProcessHandle);
            }
            processRecord.dynamicCountersReady = false;
            return false;
        }

        // 读取创建时间 + CPU 累计时间。
        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (::GetProcessTimes(processHandle, &creationTime, &exitTime, &kernelTime, &userTime) != FALSE)
        {
            processRecord.creationTime100ns = ks::str::FileTimeToUint64(
                creationTime.dwHighDateTime,
                creationTime.dwLowDateTime);
            processRecord.rawCpuTime100ns =
                ks::str::FileTimeToUint64(kernelTime.dwHighDateTime, kernelTime.dwLowDateTime) +
                ks::str::FileTimeToUint64(userTime.dwHighDateTime, userTime.dwLowDateTime);
            processRecord.startTimeText = ks::str::FileTime100nsToLocalText(processRecord.creationTime100ns);
        }

        // 读取 RAM 工作集。
        PROCESS_MEMORY_COUNTERS_EX memoryCounters{};
        if (::GetProcessMemoryInfo(
            processHandle,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryCounters),
            sizeof(memoryCounters)) != FALSE)
        {
            processRecord.rawWorkingSetBytes = static_cast<std::uint64_t>(memoryCounters.WorkingSetSize);
            processRecord.rawPrivateBytes = static_cast<std::uint64_t>(memoryCounters.PrivateUsage);
            if (processRecord.rawPrivateBytes == 0)
            {
                processRecord.rawPrivateBytes = static_cast<std::uint64_t>(memoryCounters.PagefileUsage);
            }
            processRecord.workingSetMB = static_cast<double>(processRecord.rawWorkingSetBytes) / (1024.0 * 1024.0);
            processRecord.ramMB = static_cast<double>(processRecord.rawPrivateBytes) / (1024.0 * 1024.0);
        }

        // 读取 IO 累计字节。
        IO_COUNTERS ioCounters{};
        if (::GetProcessIoCounters(processHandle, &ioCounters) != FALSE)
        {
            processRecord.rawIoBytes =
                static_cast<std::uint64_t>(ioCounters.ReadTransferCount) +
                static_cast<std::uint64_t>(ioCounters.WriteTransferCount) +
                static_cast<std::uint64_t>(ioCounters.OtherTransferCount);
        }

        // 若路径未填，顺带补一次路径。
        if (processRecord.imagePath.empty())
        {
            processRecord.imagePath = QueryProcessPathByHandle(processHandle);
        }

        // 动态刷新阶段顺带更新优先级/架构文本，便于详情窗口实时展示。
        processRecord.priorityText = QueryPriorityTextByHandle(processHandle);
        processRecord.efficiencyModeSupported = QueryProcessEfficiencyModeByHandle(
            processHandle,
            &processRecord.efficiencyModeEnabled,
            nullptr);
        if (processRecord.architectureText.empty() || processRecord.architectureText == "Unknown")
        {
            processRecord.architectureText = QueryProcessArchitectureByHandle(processHandle);
        }

        // 句柄数量是资源视图动态列，必须在关闭 processHandle 之前读取。
        DWORD processHandleCount = 0;
        if (::GetProcessHandleCount(processHandle, &processHandleCount) != FALSE)
        {
            processRecord.handleCount = static_cast<std::uint32_t>(processHandleCount);
        }
        ::CloseHandle(processHandle);

        // GPU/Net 当前预留，统一置零。
        processRecord.gpuPercent = 0.0;
        processRecord.netKBps = 0.0;
        processRecord.dynamicCountersReady = true;
        return true;
    }

    bool QueryProcessProtectionLevelByPid(
        const std::uint32_t pid,
        std::uint32_t* const levelOut,
        std::string* const displayTextOut,
        std::string* const errorMessageOut)
    {
        // 输出参数先清零，保证失败路径不会留下上一轮脏值。
        if (levelOut != nullptr)
        {
            *levelOut = 0;
        }
        if (displayTextOut != nullptr)
        {
            displayTextOut->clear();
        }
        if (errorMessageOut != nullptr)
        {
            errorMessageOut->clear();
        }

        // PID 0 没有常规进程句柄，直接返回公开枚举中的 None。
        if (pid == 0)
        {
            if (levelOut != nullptr)
            {
                *levelOut = ProcessProtectionLevelNone;
            }
            if (displayTextOut != nullptr)
            {
                *displayTextOut = ProcessProtectionLevelToText(ProcessProtectionLevelNone);
            }
            return true;
        }

        // GetProcessInformation 从 kernel32 动态解析，兼容较旧 SDK/运行环境。
        HMODULE kernel32Module = ::GetModuleHandleW(L"kernel32.dll");
        const auto getProcessInformation = reinterpret_cast<GetProcessInformationFn>(
            kernel32Module != nullptr ? ::GetProcAddress(kernel32Module, "GetProcessInformation") : nullptr);
        if (getProcessInformation == nullptr)
        {
            if (errorMessageOut != nullptr)
            {
                *errorMessageOut = "GetProcessInformation(ProcessProtectionLevelInfo) is not available.";
            }
            return false;
        }

        // 查询 PPL 枚举只需要受限查询权限，避免无谓请求 VM_READ。
        const HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessageOut != nullptr)
            {
                *errorMessageOut = "OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) failed: "
                    + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        ProcessProtectionLevelInformationNative protectionInfo{};
        const BOOL queryOk = getProcessInformation(
            processHandle,
            ProcessProtectionLevelInfoClass,
            &protectionInfo,
            static_cast<DWORD>(sizeof(protectionInfo)));
        const DWORD queryError = ::GetLastError();
        ::CloseHandle(processHandle);

        if (queryOk == FALSE)
        {
            if (errorMessageOut != nullptr)
            {
                *errorMessageOut = "GetProcessInformation(ProcessProtectionLevelInfo) failed: "
                    + FormatLastErrorMessage(queryError);
            }
            return false;
        }

        if (levelOut != nullptr)
        {
            *levelOut = static_cast<std::uint32_t>(protectionInfo.protectionLevel);
        }
        if (displayTextOut != nullptr)
        {
            *displayTextOut = ProcessProtectionLevelToText(protectionInfo.protectionLevel);
        }
        return true;
    }

    bool FillProcessStaticDetails(ProcessRecord& processRecord, const bool includeSignatureCheck)
    {
        // 静态详情阶段需要更多权限用于读取命令行/令牌信息。
        if (processRecord.pid == 0)
        {
            // PID 0（Idle）没有常规用户态映像，签名逻辑也不适用。
            processRecord.signatureState = includeSignatureCheck ? "Unsigned" : "Pending";
            processRecord.signaturePublisher = "System";
            processRecord.signatureTrusted = false;
            processRecord.architectureText = "N/A";
            processRecord.priorityText = "Idle";
            processRecord.staticDetailsReady = true;
            return true;
        }

        // 先尝试 VM_READ 版本句柄（可读取命令行）；失败后回退到 limited 句柄。
        HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            ToDwordPid(processRecord.pid));
        if (processHandle == nullptr)
        {
            processHandle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                ToDwordPid(processRecord.pid));
        }
        if (processHandle == nullptr)
        {
            processRecord.staticDetailsReady = false;
            return false;
        }

        // 可执行路径。
        if (processRecord.imagePath.empty())
        {
            processRecord.imagePath = QueryProcessPathByHandle(processHandle);
        }

        // 进程名兜底：从路径提取文件名。
        if (processRecord.processName.empty())
        {
            processRecord.processName = ExtractFileNameFromPath(processRecord.imagePath);
        }

        // 命令行、用户、管理员状态。
        processRecord.commandLine = QueryProcessCommandLineByHandle(processHandle);
        processRecord.userName = QueryProcessUserNameByHandle(processHandle);
        processRecord.isAdmin = QueryProcessIsElevatedByHandle(processHandle);
        processRecord.architectureText = QueryProcessArchitectureByHandle(processHandle);
        processRecord.priorityText = QueryPriorityTextByHandle(processHandle);
        processRecord.efficiencyModeSupported = QueryProcessEfficiencyModeByHandle(
            processHandle,
            &processRecord.efficiencyModeEnabled,
            nullptr);
        if (processRecord.sessionId == 0)
        {
            DWORD sessionId = 0;
            if (::ProcessIdToSessionId(ToDwordPid(processRecord.pid), &sessionId) != FALSE)
            {
                processRecord.sessionId = static_cast<std::uint32_t>(sessionId);
            }
        }

        // 若创建时间尚未填，尽量补齐。
        if (processRecord.creationTime100ns == 0)
        {
            FILETIME creationTime{};
            FILETIME exitTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            if (::GetProcessTimes(processHandle, &creationTime, &exitTime, &kernelTime, &userTime) != FALSE)
            {
                processRecord.creationTime100ns = ks::str::FileTimeToUint64(
                    creationTime.dwHighDateTime,
                    creationTime.dwLowDateTime);
            }
        }

        if (!processRecord.startTimeText.empty() || processRecord.creationTime100ns == 0)
        {
            // 无需重复格式化。
        }
        else
        {
            processRecord.startTimeText = ks::str::FileTime100nsToLocalText(processRecord.creationTime100ns);
        }

        ::CloseHandle(processHandle);

        // 数字签名校验通常相对耗时：
        // - 详细模式：执行真实签名校验；
        // - 快速模式：仅标记为 Pending，后续详细模式再补齐。
        if (includeSignatureCheck)
        {
            const FileSignatureInfo signatureInfo = QueryFileSignatureInfo(processRecord.imagePath);
            processRecord.signatureState = signatureInfo.displayText.empty() ? "Unknown" : signatureInfo.displayText;
            processRecord.signaturePublisher = signatureInfo.publisher;
            processRecord.signatureTrusted = signatureInfo.trustedByWindows;
        }
        else if (processRecord.signatureState.empty())
        {
            processRecord.signatureState = "Pending";
            processRecord.signaturePublisher.clear();
            processRecord.signatureTrusted = false;
        }

        // 清理不可见字符，防止表格显示异常。
        ks::str::ReplaceAllInPlace(processRecord.commandLine, "\r", " ");
        ks::str::ReplaceAllInPlace(processRecord.commandLine, "\n", " ");
        processRecord.commandLine = ks::str::TrimCopy(processRecord.commandLine);
        processRecord.userName = ks::str::TrimCopy(processRecord.userName);
        processRecord.signaturePublisher = ks::str::TrimCopy(processRecord.signaturePublisher);

        // staticDetailsReady 在这里表示“基础静态字段可用”，
        // 即便签名处于 Pending 也算可展示，后续可按需补签名。
        processRecord.staticDetailsReady = true;
        return true;
    }

    void UpdateDerivedCounters(
        ProcessRecord& processRecord,
        const CounterSample* previousSample,
        CounterSample& nextSampleOut,
        const std::uint32_t logicalCpuCount,
        const std::uint64_t currentTick100ns)
    {
        // 先写入下一轮样本，保证调用方无论成功失败都可更新基准。
        nextSampleOut.cpuTime100ns = processRecord.rawCpuTime100ns;
        nextSampleOut.ioBytes = processRecord.rawIoBytes;
        nextSampleOut.sampleTick100ns = currentTick100ns;

        // RAM 同时保留申请内存与工作集，便于 UI 展示“申请/使用”两组数值。
        processRecord.workingSetMB = static_cast<double>(processRecord.rawWorkingSetBytes) / (1024.0 * 1024.0);
        processRecord.ramMB = static_cast<double>(processRecord.rawPrivateBytes) / (1024.0 * 1024.0);

        // 无历史样本时，CPU/Disk 无法计算差值，置 0。
        if (previousSample == nullptr)
        {
            processRecord.cpuPercent = 0.0;
            processRecord.diskMBps = 0.0;
            processRecord.gpuPercent = 0.0;
            processRecord.netKBps = 0.0;
            return;
        }

        // 采样间隔过小或时钟回退时，避免除零。
        if (currentTick100ns <= previousSample->sampleTick100ns)
        {
            processRecord.cpuPercent = 0.0;
            processRecord.diskMBps = 0.0;
            processRecord.gpuPercent = 0.0;
            processRecord.netKBps = 0.0;
            return;
        }

        const std::uint64_t deltaTick100ns = currentTick100ns - previousSample->sampleTick100ns;
        const std::uint64_t deltaCpu100ns =
            (processRecord.rawCpuTime100ns >= previousSample->cpuTime100ns)
            ? (processRecord.rawCpuTime100ns - previousSample->cpuTime100ns)
            : 0;
        const std::uint64_t deltaIoBytes =
            (processRecord.rawIoBytes >= previousSample->ioBytes)
            ? (processRecord.rawIoBytes - previousSample->ioBytes)
            : 0;

        // CPU 百分比换算：
        // deltaCpu / deltaWall / logicalCpuCount * 100。
        const double cpuCountSafe = std::max(1u, logicalCpuCount);
        const double cpuPercent = (static_cast<double>(deltaCpu100ns) / static_cast<double>(deltaTick100ns)) * (100.0 / cpuCountSafe);
        processRecord.cpuPercent = std::clamp(cpuPercent, 0.0, 100.0);

        // 为避免“小于显示精度但非零”的进程看起来恒为 0，
        // 只要 CPU 增量确实大于 0，就给一个最小可见值 0.01%。
        if (deltaCpu100ns > 0 && processRecord.cpuPercent < 0.01)
        {
            processRecord.cpuPercent = 0.01;
        }

        // Disk 吞吐换算：deltaIoBytes / deltaSeconds -> MB/s。
        const double deltaSeconds = static_cast<double>(deltaTick100ns) / 10000000.0;
        if (deltaSeconds > 0.0)
        {
            processRecord.diskMBps = (static_cast<double>(deltaIoBytes) / deltaSeconds) / (1024.0 * 1024.0);
        }
        else
        {
            processRecord.diskMBps = 0.0;
        }

        // GPU/Net 当前预留，暂置 0；后续可接入 ETW/PDH。
        processRecord.gpuPercent = 0.0;
        processRecord.netKBps = 0.0;
    }

    std::vector<ProcessRecord> EnumerateProcesses(
        const ProcessEnumStrategy strategy,
        ProcessEnumStrategy* const actualStrategyOut)
    {
        // 内部 lambda：Toolhelp 路径。
        const auto enumerateBySnapshot = []() -> std::vector<ProcessRecord>
            {
                std::vector<ProcessRecord> processList;

                HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snapshotHandle == INVALID_HANDLE_VALUE)
                {
                    return processList;
                }

                PROCESSENTRY32W processEntry{};
                processEntry.dwSize = sizeof(processEntry);
                if (::Process32FirstW(snapshotHandle, &processEntry) == FALSE)
                {
                    ::CloseHandle(snapshotHandle);
                    return processList;
                }

                // 遍历快照并补动态计数器。
                do
                {
                    ProcessRecord processRecord{};
                    processRecord.pid = static_cast<std::uint32_t>(processEntry.th32ProcessID);
                    processRecord.parentPid = static_cast<std::uint32_t>(processEntry.th32ParentProcessID);
                    processRecord.threadCount = static_cast<std::uint32_t>(processEntry.cntThreads);
                    processRecord.processName = ks::str::Utf16ToUtf8(processEntry.szExeFile);

                    DWORD sessionId = 0;
                    if (::ProcessIdToSessionId(ToDwordPid(processRecord.pid), &sessionId) != FALSE)
                    {
                        processRecord.sessionId = static_cast<std::uint32_t>(sessionId);
                    }

                    // Snapshot 本身不给出 CPU/RAM/IO，需要额外查询。
                    RefreshProcessDynamicCounters(processRecord);
                    processList.push_back(std::move(processRecord));
                } while (::Process32NextW(snapshotHandle, &processEntry) != FALSE);

                ::CloseHandle(snapshotHandle);
                return processList;
            };

        // 内部 lambda：NtQuerySystemInformation 路径。
        const auto enumerateByNtQuery = []() -> std::vector<ProcessRecord>
            {
                std::vector<ProcessRecord> processList;

                const auto ntQuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
                    GetNtdllProcAddress("NtQuerySystemInformation"));
                if (ntQuerySystemInformation == nullptr)
                {
                    return processList;
                }

                // 缓冲区按返回码动态扩容。
                ULONG bufferLength = 1 * 1024 * 1024;
                std::vector<BYTE> informationBuffer(bufferLength);
                NTSTATUS queryStatus = ntQuerySystemInformation(
                    SystemProcessInformation,
                    informationBuffer.data(),
                    bufferLength,
                    &bufferLength);

                while (queryStatus == StatusInfoLengthMismatch)
                {
                    bufferLength = (bufferLength * 3) / 2 + 64 * 1024;
                    informationBuffer.resize(bufferLength);
                    queryStatus = ntQuerySystemInformation(
                        SystemProcessInformation,
                        informationBuffer.data(),
                        bufferLength,
                        &bufferLength);
                }

                if (!NT_SUCCESS(queryStatus))
                {
                    return processList;
                }

                // 按 NextEntryOffset 遍历链式结构。
                BYTE* currentPointer = informationBuffer.data();
                while (currentPointer != nullptr)
                {
                    const auto* processInfo = reinterpret_cast<const SystemProcessInformationRecord*>(currentPointer);
                    ProcessRecord processRecord{};

                    processRecord.pid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(processInfo->UniqueProcessId));
                    processRecord.parentPid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(processInfo->InheritedFromUniqueProcessId));
                    processRecord.threadCount = static_cast<std::uint32_t>(processInfo->NumberOfThreads);
                    processRecord.handleCount = static_cast<std::uint32_t>(processInfo->HandleCount);
                    processRecord.sessionId = static_cast<std::uint32_t>(processInfo->SessionId);
                    processRecord.creationTime100ns = static_cast<std::uint64_t>(processInfo->CreateTime.QuadPart);
                    processRecord.rawCpuTime100ns = static_cast<std::uint64_t>(processInfo->KernelTime.QuadPart + processInfo->UserTime.QuadPart);
                    processRecord.rawWorkingSetBytes = static_cast<std::uint64_t>(processInfo->WorkingSetSize);
                    processRecord.rawPrivateBytes = static_cast<std::uint64_t>(processInfo->PrivatePageCount);
                    processRecord.rawIoBytes =
                        static_cast<std::uint64_t>(processInfo->ReadTransferCount.QuadPart) +
                        static_cast<std::uint64_t>(processInfo->WriteTransferCount.QuadPart) +
                        static_cast<std::uint64_t>(processInfo->OtherTransferCount.QuadPart);
                    processRecord.workingSetMB = static_cast<double>(processRecord.rawWorkingSetBytes) / (1024.0 * 1024.0);
                    processRecord.ramMB = static_cast<double>(processRecord.rawPrivateBytes) / (1024.0 * 1024.0);
                    processRecord.startTimeText = ks::str::FileTime100nsToLocalText(processRecord.creationTime100ns);
                    processRecord.dynamicCountersReady = true;

                    if (processInfo->ImageName.Buffer != nullptr && processInfo->ImageName.Length > 0)
                    {
                        const std::wstring imageName(
                            processInfo->ImageName.Buffer,
                            static_cast<std::size_t>(processInfo->ImageName.Length / sizeof(wchar_t)));
                        processRecord.processName = ks::str::Utf16ToUtf8(imageName);
                    }
                    else
                    {
                        // 系统内核进程常见兜底命名。
                        if (processRecord.pid == 0)
                        {
                            processRecord.processName = "System Idle Process";
                        }
                        else if (processRecord.pid == 4)
                        {
                            processRecord.processName = "System";
                        }
                        else
                        {
                            processRecord.processName = "Unknown";
                        }
                    }

                    processList.push_back(std::move(processRecord));

                    if (processInfo->NextEntryOffset == 0)
                    {
                        break;
                    }
                    currentPointer += processInfo->NextEntryOffset;
                }

                return processList;
            };

        // 按策略执行并处理回退逻辑。
        if (strategy == ProcessEnumStrategy::SnapshotProcess32)
        {
            if (actualStrategyOut != nullptr)
            {
                *actualStrategyOut = ProcessEnumStrategy::SnapshotProcess32;
            }
            return enumerateBySnapshot();
        }
        if (strategy == ProcessEnumStrategy::NtQuerySystemInfo)
        {
            if (actualStrategyOut != nullptr)
            {
                *actualStrategyOut = ProcessEnumStrategy::NtQuerySystemInfo;
            }
            return enumerateByNtQuery();
        }

        // Auto：优先 NtQuery，失败回退 Snapshot。
        std::vector<ProcessRecord> processList = enumerateByNtQuery();
        if (!processList.empty())
        {
            if (actualStrategyOut != nullptr)
            {
                *actualStrategyOut = ProcessEnumStrategy::NtQuerySystemInfo;
            }
            return processList;
        }
        if (actualStrategyOut != nullptr)
        {
            *actualStrategyOut = ProcessEnumStrategy::SnapshotProcess32;
        }
        return enumerateBySnapshot();
    }

    std::vector<SystemThreadRecord> EnumerateSystemThreads(
        bool* const usedNtQueryOut,
        std::string* const diagnosticTextOut)
    {
        // usedNtQueryOut 用途：向调用方回报本轮是否走 NtQuery 路径。
        if (usedNtQueryOut != nullptr)
        {
            *usedNtQueryOut = false;
        }

        // diagnosticTextOut 用途：返回本轮路径说明或失败原因。
        if (diagnosticTextOut != nullptr)
        {
            diagnosticTextOut->clear();
        }

        // 第一阶段：优先尝试 NtQuerySystemInformation(SystemProcessInformation)。
        const auto ntQuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
            GetNtdllProcAddress("NtQuerySystemInformation"));
        if (ntQuerySystemInformation != nullptr)
        {
            ULONG bufferLength = 2 * 1024 * 1024;
            std::vector<BYTE> informationBuffer(bufferLength);
            NTSTATUS queryStatus = ntQuerySystemInformation(
                SystemProcessInformation,
                informationBuffer.data(),
                bufferLength,
                &bufferLength);

            while (queryStatus == StatusInfoLengthMismatch)
            {
                bufferLength = (bufferLength * 3) / 2 + 64 * 1024;
                informationBuffer.resize(bufferLength);
                queryStatus = ntQuerySystemInformation(
                    SystemProcessInformation,
                    informationBuffer.data(),
                    bufferLength,
                    &bufferLength);
            }

            if (NT_SUCCESS(queryStatus))
            {
                std::vector<SystemThreadRecord> threadList;

                // currentPointer 用途：顺序遍历 NextEntryOffset 链表。
                BYTE* currentPointer = informationBuffer.data();
                while (currentPointer != nullptr)
                {
                    const auto* processInfo = reinterpret_cast<const SystemProcessInformationRecord*>(currentPointer);
                    const std::uint32_t processPid = static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(processInfo->UniqueProcessId));

                    // processNameText 用途：给该进程下所有线程复用同一进程名文本。
                    std::string processNameText;
                    if (processInfo->ImageName.Buffer != nullptr && processInfo->ImageName.Length > 0)
                    {
                        const std::wstring imageName(
                            processInfo->ImageName.Buffer,
                            static_cast<std::size_t>(processInfo->ImageName.Length / sizeof(wchar_t)));
                        processNameText = ks::str::Utf16ToUtf8(imageName);
                    }
                    else if (processPid == 0)
                    {
                        processNameText = "System Idle Process";
                    }
                    else if (processPid == 4)
                    {
                        processNameText = "System";
                    }
                    else
                    {
                        processNameText = "Unknown";
                    }

                    // threadArrayPointer 用途：定位到当前进程条目后紧跟的线程数组。
                    const auto* threadArrayPointer =
                        reinterpret_cast<const SystemThreadInformationRecord*>(processInfo + 1);
                    for (ULONG threadIndex = 0; threadIndex < processInfo->NumberOfThreads; ++threadIndex)
                    {
                        const SystemThreadInformationRecord& threadInfo = threadArrayPointer[threadIndex];

                        SystemThreadRecord threadRecord{};
                        threadRecord.threadId = static_cast<std::uint32_t>(
                            reinterpret_cast<std::uintptr_t>(threadInfo.ClientId.UniqueThread));
                        threadRecord.ownerPid = static_cast<std::uint32_t>(
                            reinterpret_cast<std::uintptr_t>(threadInfo.ClientId.UniqueProcess));
                        if (threadRecord.ownerPid == 0)
                        {
                            threadRecord.ownerPid = processPid;
                        }
                        threadRecord.ownerProcessName = processNameText;
                        threadRecord.startAddress = reinterpret_cast<std::uint64_t>(threadInfo.StartAddress);
                        threadRecord.priority = static_cast<int>(threadInfo.Priority);
                        threadRecord.basePriority = static_cast<int>(threadInfo.BasePriority);
                        threadRecord.threadState = static_cast<std::uint32_t>(threadInfo.ThreadState);
                        threadRecord.waitReason = static_cast<std::uint32_t>(threadInfo.WaitReason);
                        threadRecord.kernelTime100ns = static_cast<std::uint64_t>(threadInfo.ReservedTime[0].QuadPart);
                        threadRecord.userTime100ns = static_cast<std::uint64_t>(threadInfo.ReservedTime[1].QuadPart);
                        threadRecord.createTime100ns = static_cast<std::uint64_t>(threadInfo.ReservedTime[2].QuadPart);
                        threadRecord.waitTimeTick = static_cast<std::uint32_t>(threadInfo.WaitTime);
                        threadRecord.contextSwitchCount = static_cast<std::uint32_t>(threadInfo.ContextSwitches);
                        threadList.push_back(std::move(threadRecord));
                    }

                    if (processInfo->NextEntryOffset == 0)
                    {
                        break;
                    }
                    currentPointer += processInfo->NextEntryOffset;
                }

                // 统一排序：先 PID，再 TID，保证 UI 每轮刷新顺序稳定。
                std::sort(
                    threadList.begin(),
                    threadList.end(),
                    [](const SystemThreadRecord& leftRecord, const SystemThreadRecord& rightRecord)
                    {
                        if (leftRecord.ownerPid != rightRecord.ownerPid)
                        {
                            return leftRecord.ownerPid < rightRecord.ownerPid;
                        }
                        return leftRecord.threadId < rightRecord.threadId;
                    });

                if (usedNtQueryOut != nullptr)
                {
                    *usedNtQueryOut = true;
                }
                if (diagnosticTextOut != nullptr)
                {
                    *diagnosticTextOut =
                        "NtQuerySystemInformation(SystemProcessInformation) success";
                }
                return threadList;
            }

            if (diagnosticTextOut != nullptr)
            {
                *diagnosticTextOut = FormatNtStatusMessage(
                    queryStatus,
                    "NtQuerySystemInformation(SystemProcessInformation) failed");
            }
        }
        else if (diagnosticTextOut != nullptr)
        {
            *diagnosticTextOut = "NtQuerySystemInformation not available";
        }

        // 第二阶段：Nt 路径不可用时回退 Toolhelp（保证功能可用性）。
        std::vector<SystemThreadRecord> threadList;
        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            if (diagnosticTextOut != nullptr)
            {
                *diagnosticTextOut += " | Toolhelp fallback failed: "
                    + FormatLastErrorMessage(::GetLastError());
            }
            return threadList;
        }

        // processNameCacheByPid 用途：避免为同 PID 重复调用 GetProcessNameByPID。
        std::unordered_map<std::uint32_t, std::string> processNameCacheByPid;

        THREADENTRY32 threadEntry{};
        threadEntry.dwSize = sizeof(threadEntry);
        if (::Thread32First(snapshotHandle, &threadEntry) == FALSE)
        {
            if (diagnosticTextOut != nullptr)
            {
                *diagnosticTextOut += " | Thread32First failed: "
                    + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(snapshotHandle);
            return threadList;
        }

        do
        {
            SystemThreadRecord threadRecord{};
            threadRecord.threadId = static_cast<std::uint32_t>(threadEntry.th32ThreadID);
            threadRecord.ownerPid = static_cast<std::uint32_t>(threadEntry.th32OwnerProcessID);
            threadRecord.priority = static_cast<int>(threadEntry.tpBasePri);
            threadRecord.basePriority = static_cast<int>(threadEntry.tpBasePri);
            threadRecord.threadState = std::numeric_limits<std::uint32_t>::max();
            threadRecord.waitReason = std::numeric_limits<std::uint32_t>::max();

            auto processNameIt = processNameCacheByPid.find(threadRecord.ownerPid);
            if (processNameIt == processNameCacheByPid.end())
            {
                std::string processNameText = GetProcessNameByPID(threadRecord.ownerPid);
                if (processNameText.empty())
                {
                    processNameText = "Unknown";
                }
                processNameIt = processNameCacheByPid.emplace(
                    threadRecord.ownerPid,
                    std::move(processNameText)).first;
            }
            threadRecord.ownerProcessName = processNameIt->second;
            threadList.push_back(std::move(threadRecord));
        } while (::Thread32Next(snapshotHandle, &threadEntry) != FALSE);

        ::CloseHandle(snapshotHandle);

        std::sort(
            threadList.begin(),
            threadList.end(),
            [](const SystemThreadRecord& leftRecord, const SystemThreadRecord& rightRecord)
            {
                if (leftRecord.ownerPid != rightRecord.ownerPid)
                {
                    return leftRecord.ownerPid < rightRecord.ownerPid;
                }
                return leftRecord.threadId < rightRecord.threadId;
            });

        if (diagnosticTextOut != nullptr)
        {
            *diagnosticTextOut += " | Toolhelp fallback success";
        }
        return threadList;
    }

    std::string QueryProcessPathByPid(const std::uint32_t pid)
    {
        if (pid == 0)
        {
            return std::string();
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            return std::string();
        }

        std::string processPath = QueryProcessPathByHandle(processHandle);
        ::CloseHandle(processHandle);
        return processPath;
    }

    std::string GetProcessNameByPID(const std::uint32_t pid)
    {
        // 优先路径提取文件名，失败再回退快照枚举。
        const std::string processPath = QueryProcessPathByPid(pid);
        if (!processPath.empty())
        {
            return ExtractFileNameFromPath(processPath);
        }

        const std::vector<ProcessRecord> processList = EnumerateProcesses(ProcessEnumStrategy::SnapshotProcess32);
        for (const ProcessRecord& processRecord : processList)
        {
            if (processRecord.pid == pid)
            {
                return processRecord.processName;
            }
        }
        return std::string();
    }

    bool ExecuteTaskKill(const std::uint32_t pid, const bool forceKill, std::string* const errorMessage)
    {
        // 通过 cmd /C taskkill 执行，确保行为与用户手工命令一致。
        std::wstring commandLine = L"cmd.exe /C taskkill /PID " + std::to_wstring(pid);
        if (forceKill)
        {
            commandLine += L" /F";
        }

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};

        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        const BOOL createResult = ::CreateProcessW(
            nullptr,
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo);
        if (createResult == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateProcess(taskkill) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        ::WaitForSingleObject(processInfo.hProcess, 10000);
        DWORD exitCode = 0;
        ::GetExitCodeProcess(processInfo.hProcess, &exitCode);
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);

        if (exitCode == 0)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            std::ostringstream stream;
            stream << "taskkill failed, exit code=" << exitCode;
            *errorMessage = stream.str();
        }
        return false;
    }

    bool TerminateProcessByWin32(const std::uint32_t pid, std::string* const errorMessage)
    {
        const HANDLE processHandle = ::OpenProcess(PROCESS_TERMINATE, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_TERMINATE) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const BOOL terminateResult = ::TerminateProcess(processHandle, 1);
        const DWORD terminateError = ::GetLastError();
        ::CloseHandle(processHandle);

        if (terminateResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "TerminateProcess failed: " + FormatLastErrorMessage(terminateError);
        }
        return false;
    }

    bool TerminateProcessByNtNative(const std::uint32_t pid, std::string* const errorMessage)
    {
        // NtTerminateProcess 优先，缺失时回退 ZwTerminateProcess。
        auto ntTerminateProcess = reinterpret_cast<NtTerminateProcessFn>(
            GetNtdllProcAddress("NtTerminateProcess"));
        if (ntTerminateProcess == nullptr)
        {
            ntTerminateProcess = reinterpret_cast<NtTerminateProcessFn>(
                GetNtdllProcAddress("ZwTerminateProcess"));
        }
        if (ntTerminateProcess == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtTerminateProcess / ZwTerminateProcess not available.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(PROCESS_TERMINATE, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_TERMINATE) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const NTSTATUS terminateStatus = ntTerminateProcess(processHandle, static_cast<NTSTATUS>(1));
        ::CloseHandle(processHandle);
        if (NT_SUCCESS(terminateStatus))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = FormatNtStatusMessage(terminateStatus, "NtTerminateProcess failed");
        }
        return false;
    }

    bool TerminateProcessByWtsApi(const std::uint32_t pid, std::string* const errorMessage)
    {
        using WtsTerminateProcessFn = BOOL(WINAPI*)(HANDLE, DWORD, DWORD);

        static HMODULE wtsApiModule = ::LoadLibraryW(L"Wtsapi32.dll");
        if (wtsApiModule == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "LoadLibrary(Wtsapi32.dll) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        static WtsTerminateProcessFn wtsTerminateProcess = reinterpret_cast<WtsTerminateProcessFn>(
            ::GetProcAddress(wtsApiModule, "WTSTerminateProcess"));
        if (wtsTerminateProcess == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "GetProcAddress(WTSTerminateProcess) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        // 传入 nullptr 表示当前服务器（等价 WTS_CURRENT_SERVER_HANDLE）。
        const BOOL terminateResult = wtsTerminateProcess(nullptr, ToDwordPid(pid), 1);
        if (terminateResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "WTSTerminateProcess failed: " + FormatLastErrorMessage(::GetLastError());
        }
        return false;
    }

    bool TerminateProcessByWinStationApi(const std::uint32_t pid, std::string* const errorMessage)
    {
        using WinStationTerminateProcessFn = BOOLEAN(WINAPI*)(HANDLE, ULONG, ULONG);

        static HMODULE winstaModule = ::LoadLibraryW(L"winsta.dll");
        if (winstaModule == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "LoadLibrary(winsta.dll) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        static WinStationTerminateProcessFn winStationTerminateProcess = reinterpret_cast<WinStationTerminateProcessFn>(
            ::GetProcAddress(winstaModule, "WinStationTerminateProcess"));
        if (winStationTerminateProcess == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "GetProcAddress(WinStationTerminateProcess) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const BOOLEAN terminateResult = winStationTerminateProcess(nullptr, static_cast<ULONG>(pid), 1UL);
        if (terminateResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "WinStationTerminateProcess failed: " + FormatLastErrorMessage(::GetLastError());
        }
        return false;
    }

    bool TerminateProcessByJobObject(const std::uint32_t pid, std::string* const errorMessage)
    {
        const HANDLE processHandle = ::OpenProcess(
            PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for job terminate) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const HANDLE jobHandle = ::CreateJobObjectW(nullptr, nullptr);
        if (jobHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateJobObjectW failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(processHandle);
            return false;
        }

        const BOOL assignResult = ::AssignProcessToJobObject(jobHandle, processHandle);
        const DWORD assignError = ::GetLastError();
        if (assignResult == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "AssignProcessToJobObject failed: " + FormatLastErrorMessage(assignError);
            }
            ::CloseHandle(jobHandle);
            ::CloseHandle(processHandle);
            return false;
        }

        const BOOL terminateResult = ::TerminateJobObject(jobHandle, 1);
        const DWORD terminateError = ::GetLastError();
        ::CloseHandle(jobHandle);
        ::CloseHandle(processHandle);
        if (terminateResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "TerminateJobObject failed: " + FormatLastErrorMessage(terminateError);
        }
        return false;
    }

    bool TerminateProcessByNtJobObject(const std::uint32_t pid, std::string* const errorMessage)
    {
        // NtTerminateJobObject 优先，缺失时回退 ZwTerminateJobObject。
        auto ntTerminateJobObject = reinterpret_cast<NtTerminateJobObjectFn>(
            GetNtdllProcAddress("NtTerminateJobObject"));
        if (ntTerminateJobObject == nullptr)
        {
            ntTerminateJobObject = reinterpret_cast<NtTerminateJobObjectFn>(
                GetNtdllProcAddress("ZwTerminateJobObject"));
        }
        if (ntTerminateJobObject == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtTerminateJobObject / ZwTerminateJobObject not available.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for nt job terminate) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const HANDLE jobHandle = ::CreateJobObjectW(nullptr, nullptr);
        if (jobHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateJobObjectW failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(processHandle);
            return false;
        }

        const BOOL assignResult = ::AssignProcessToJobObject(jobHandle, processHandle);
        const DWORD assignError = ::GetLastError();
        if (assignResult == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "AssignProcessToJobObject failed: " + FormatLastErrorMessage(assignError);
            }
            ::CloseHandle(jobHandle);
            ::CloseHandle(processHandle);
            return false;
        }

        const NTSTATUS terminateStatus = ntTerminateJobObject(jobHandle, static_cast<NTSTATUS>(1));
        ::CloseHandle(jobHandle);
        ::CloseHandle(processHandle);
        if (NT_SUCCESS(terminateStatus))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = FormatNtStatusMessage(terminateStatus, "NtTerminateJobObject failed");
        }
        return false;
    }

    bool TerminateProcessByRestartManager(
        const std::uint32_t pid,
        const bool forceShutdown,
        std::string* const errorMessage)
    {
        using RmStartSessionFn = DWORD(WINAPI*)(DWORD*, DWORD, WCHAR*);
        using RmRegisterResourcesFn = DWORD(WINAPI*)(DWORD, UINT, LPCWSTR*, UINT, RM_UNIQUE_PROCESS*, UINT, LPCWSTR*);
        using RmShutdownFn = DWORD(WINAPI*)(DWORD, ULONG, RM_WRITE_STATUS_CALLBACK);
        using RmEndSessionFn = DWORD(WINAPI*)(DWORD);

        static HMODULE restartManagerModule = ::LoadLibraryW(L"Rstrtmgr.dll");
        if (restartManagerModule == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "LoadLibrary(Rstrtmgr.dll) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        static RmStartSessionFn rmStartSession = reinterpret_cast<RmStartSessionFn>(
            ::GetProcAddress(restartManagerModule, "RmStartSession"));
        static RmRegisterResourcesFn rmRegisterResources = reinterpret_cast<RmRegisterResourcesFn>(
            ::GetProcAddress(restartManagerModule, "RmRegisterResources"));
        static RmShutdownFn rmShutdown = reinterpret_cast<RmShutdownFn>(
            ::GetProcAddress(restartManagerModule, "RmShutdown"));
        static RmEndSessionFn rmEndSession = reinterpret_cast<RmEndSessionFn>(
            ::GetProcAddress(restartManagerModule, "RmEndSession"));
        if (rmStartSession == nullptr ||
            rmRegisterResources == nullptr ||
            rmShutdown == nullptr ||
            rmEndSession == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Restart Manager function export missing.";
            }
            return false;
        }

        DWORD sessionHandle = 0;
        WCHAR sessionKey[CCH_RM_SESSION_KEY + 1] = {};
        const DWORD startResult = rmStartSession(&sessionHandle, 0, sessionKey);
        if (startResult != ERROR_SUCCESS)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "RmStartSession failed: code=" + std::to_string(startResult);
            }
            return false;
        }

        FILETIME processStartTime{};
        if (!QueryProcessStartTimeByPid(pid, &processStartTime))
        {
            rmEndSession(sessionHandle);
            if (errorMessage != nullptr)
            {
                *errorMessage = "QueryProcessStartTimeByPid failed.";
            }
            return false;
        }

        RM_UNIQUE_PROCESS uniqueProcess{};
        uniqueProcess.dwProcessId = ToDwordPid(pid);
        uniqueProcess.ProcessStartTime = processStartTime;

        const DWORD registerResult = rmRegisterResources(
            sessionHandle,
            0,
            nullptr,
            1,
            &uniqueProcess,
            0,
            nullptr);
        if (registerResult != ERROR_SUCCESS)
        {
            rmEndSession(sessionHandle);
            if (errorMessage != nullptr)
            {
                *errorMessage = "RmRegisterResources failed: code=" + std::to_string(registerResult);
            }
            return false;
        }

        const ULONG shutdownFlags = forceShutdown ? RestartManagerForceShutdownFlag : 0UL;
        const DWORD shutdownResult = rmShutdown(sessionHandle, shutdownFlags, nullptr);
        const DWORD endResult = rmEndSession(sessionHandle);
        if (shutdownResult == ERROR_SUCCESS)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            std::ostringstream stream;
            stream << "RmShutdown failed: code=" << shutdownResult
                << ", RmEndSession=" << endResult;
            *errorMessage = stream.str();
        }
        return false;
    }

    bool TerminateProcessByDuplicateHandlePseudo(const std::uint32_t pid, std::string* const errorMessage)
    {
        // 先以“复制句柄权限”打开目标进程。
        const HANDLE targetProcessHandle = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, ToDwordPid(pid));
        if (targetProcessHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_DUP_HANDLE) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        // 复制目标进程中的伪句柄(-1)到当前进程，得到目标进程真实句柄。
        HANDLE duplicatedProcessHandle = nullptr;
        const BOOL duplicateResult = ::DuplicateHandle(
            targetProcessHandle,
            reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-1)),
            ::GetCurrentProcess(),
            &duplicatedProcessHandle,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS);
        const DWORD duplicateError = ::GetLastError();
        ::CloseHandle(targetProcessHandle);
        if (duplicateResult == FALSE || duplicatedProcessHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "DuplicateHandle(-1 pseudo handle) failed: " + FormatLastErrorMessage(duplicateError);
            }
            return false;
        }

        const BOOL terminateResult = ::TerminateProcess(duplicatedProcessHandle, 1);
        const DWORD terminateError = ::GetLastError();
        ::CloseHandle(duplicatedProcessHandle);
        if (terminateResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "TerminateProcess(duplicated handle) failed: " + FormatLastErrorMessage(terminateError);
        }
        return false;
    }

    bool TerminateAllThreadsByPid(const std::uint32_t pid, std::string* const errorMessage)
    {
        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateToolhelp32Snapshot(THREAD) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        THREADENTRY32 threadEntry{};
        threadEntry.dwSize = sizeof(threadEntry);
        if (::Thread32First(snapshotHandle, &threadEntry) == FALSE)
        {
            ::CloseHandle(snapshotHandle);
            if (errorMessage != nullptr)
            {
                *errorMessage = "Thread32First failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        std::uint32_t terminatedCount = 0;
        do
        {
            if (threadEntry.th32OwnerProcessID != pid)
            {
                continue;
            }

            const HANDLE threadHandle = ::OpenThread(THREAD_TERMINATE, FALSE, threadEntry.th32ThreadID);
            if (threadHandle == nullptr)
            {
                continue;
            }

            if (::TerminateThread(threadHandle, 1) != FALSE)
            {
                ++terminatedCount;
            }
            ::CloseHandle(threadHandle);
        } while (::Thread32Next(snapshotHandle, &threadEntry) != FALSE);

        ::CloseHandle(snapshotHandle);
        if (terminatedCount > 0)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "No thread terminated (possible access denied or process has exited).";
        }
        return false;
    }

    bool TerminateAllThreadsByPidNtNative(const std::uint32_t pid, std::string* const errorMessage)
    {
        // NtTerminateThread 优先，缺失时回退 ZwTerminateThread。
        auto ntTerminateThread = reinterpret_cast<NtTerminateThreadFn>(
            GetNtdllProcAddress("NtTerminateThread"));
        if (ntTerminateThread == nullptr)
        {
            ntTerminateThread = reinterpret_cast<NtTerminateThreadFn>(
                GetNtdllProcAddress("ZwTerminateThread"));
        }
        if (ntTerminateThread == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtTerminateThread / ZwTerminateThread not available.";
            }
            return false;
        }

        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateToolhelp32Snapshot(THREAD) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        THREADENTRY32 threadEntry{};
        threadEntry.dwSize = sizeof(threadEntry);
        if (::Thread32First(snapshotHandle, &threadEntry) == FALSE)
        {
            ::CloseHandle(snapshotHandle);
            if (errorMessage != nullptr)
            {
                *errorMessage = "Thread32First failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        std::uint32_t terminatedCount = 0;
        do
        {
            if (threadEntry.th32OwnerProcessID != pid)
            {
                continue;
            }

            const HANDLE threadHandle = ::OpenThread(THREAD_TERMINATE, FALSE, threadEntry.th32ThreadID);
            if (threadHandle == nullptr)
            {
                continue;
            }

            const NTSTATUS terminateStatus = ntTerminateThread(threadHandle, static_cast<NTSTATUS>(1));
            if (NT_SUCCESS(terminateStatus))
            {
                ++terminatedCount;
            }
            ::CloseHandle(threadHandle);
        } while (::Thread32Next(snapshotHandle, &threadEntry) != FALSE);

        ::CloseHandle(snapshotHandle);
        if (terminatedCount > 0)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "No thread terminated via NtTerminateThread.";
        }
        return false;
    }

    bool TerminateProcessByDebugAttach(const std::uint32_t pid, std::string* const errorMessage)
    {
        // 调试附加通常需要 SeDebugPrivilege，先尝试启用。
        EnablePrivilege(SE_DEBUG_NAME);

        const BOOL attachResult = ::DebugActiveProcess(ToDwordPid(pid));
        if (attachResult == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "DebugActiveProcess failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        // 按示例链路设置 DebugSetProcessKillOnExit(0)。
        const BOOL setKillOnExitResult = ::DebugSetProcessKillOnExit(FALSE);
        const DWORD setKillOnExitError = ::GetLastError();

        // 避免当前进程长期占用调试关系，这里立即尝试解除附加。
        const BOOL stopResult = ::DebugActiveProcessStop(ToDwordPid(pid));
        const DWORD stopError = ::GetLastError();

        if (setKillOnExitResult != FALSE && stopResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            std::ostringstream stream;
            if (setKillOnExitResult == FALSE)
            {
                stream << "DebugSetProcessKillOnExit(FALSE) failed: "
                    << FormatLastErrorMessage(setKillOnExitError);
            }
            if (stopResult == FALSE)
            {
                if (!stream.str().empty())
                {
                    stream << "; ";
                }
                stream << "DebugActiveProcessStop failed: "
                    << FormatLastErrorMessage(stopError);
            }
            *errorMessage = stream.str();
        }
        return false;
    }

    bool TerminateProcessByNtsdCommand(const std::uint32_t pid, std::string* const errorMessage)
    {
        // 命令形态：ntsd -c q -p <pid>，附加后立即执行 q 退出。
        std::wstring commandLine = L"cmd.exe /C ntsd -c q -p " + std::to_wstring(pid);
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};
        const BOOL createResult = ::CreateProcessW(
            nullptr,
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo);
        if (createResult == FALSE)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateProcess(ntsd) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const DWORD waitResult = ::WaitForSingleObject(processInfo.hProcess, 15000);
        DWORD exitCode = 0;
        ::GetExitCodeProcess(processInfo.hProcess, &exitCode);
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);
        if (waitResult != WAIT_OBJECT_0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "ntsd wait timeout or failed, waitResult=" + std::to_string(waitResult);
            }
            return false;
        }
        if (exitCode == 0)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "ntsd exit code=" + std::to_string(exitCode);
        }
        return false;
    }

    bool TerminateProcessByNtUnmapNtdll(const std::uint32_t pid, std::string* const errorMessage)
    {
        auto ntUnmapViewOfSection = reinterpret_cast<NtUnmapViewOfSectionFn>(
            GetNtdllProcAddress("NtUnmapViewOfSection"));
        if (ntUnmapViewOfSection == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtUnmapViewOfSection not available.";
            }
            return false;
        }

        void* ntdllBaseAddress = nullptr;
        if (!QueryModuleBaseAddressBySnapshot(pid, L"ntdll.dll", &ntdllBaseAddress) || ntdllBaseAddress == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "QueryModuleBaseAddressBySnapshot(ntdll.dll) failed.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(PROCESS_VM_OPERATION, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_VM_OPERATION) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const NTSTATUS unmapStatus = ntUnmapViewOfSection(processHandle, ntdllBaseAddress);
        ::CloseHandle(processHandle);
        if (NT_SUCCESS(unmapStatus))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = FormatNtStatusMessage(unmapStatus, "NtUnmapViewOfSection failed");
        }
        return false;
    }

    bool InjectInvalidShellcode(const std::uint32_t pid, std::string* const errorMessage)
    {
        const HANDLE processHandle = ::OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for injection) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        // 无效 shellcode：UD2 + RET，用于触发非法指令异常。
        const BYTE invalidShellcode[] = { 0x0F, 0x0B, 0xC3 };
        void* remoteMemory = ::VirtualAllocEx(
            processHandle,
            nullptr,
            sizeof(invalidShellcode),
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (remoteMemory == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "VirtualAllocEx failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(processHandle);
            return false;
        }

        SIZE_T bytesWritten = 0;
        const BOOL writeResult = ::WriteProcessMemory(
            processHandle,
            remoteMemory,
            invalidShellcode,
            sizeof(invalidShellcode),
            &bytesWritten);
        if (writeResult == FALSE || bytesWritten != sizeof(invalidShellcode))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "WriteProcessMemory failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::VirtualFreeEx(processHandle, remoteMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        // 启动远程线程执行无效代码。
        const HANDLE remoteThread = ::CreateRemoteThread(
            processHandle,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteMemory),
            nullptr,
            0,
            nullptr);
        if (remoteThread == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateRemoteThread failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::VirtualFreeEx(processHandle, remoteMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        ::WaitForSingleObject(remoteThread, 1500);
        ::CloseHandle(remoteThread);
        ::CloseHandle(processHandle);

        // 这里不释放 remoteMemory：目标进程可能已崩溃或退出，释放意义不大。
        return true;
    }

    bool SuspendProcess(const std::uint32_t pid, std::string* const errorMessage)
    {
        const auto ntSuspendProcess = reinterpret_cast<NtSuspendProcessFn>(
            GetNtdllProcAddress("NtSuspendProcess"));
        if (ntSuspendProcess == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtSuspendProcess not available.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(ProcessSuspendResumeAccess, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_SUSPEND_RESUME) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const NTSTATUS suspendStatus = ntSuspendProcess(processHandle);
        ::CloseHandle(processHandle);
        if (NT_SUCCESS(suspendStatus))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = FormatNtStatusMessage(suspendStatus, "NtSuspendProcess failed");
        }
        return false;
    }

    bool ResumeProcess(const std::uint32_t pid, std::string* const errorMessage)
    {
        const auto ntResumeProcess = reinterpret_cast<NtResumeProcessFn>(
            GetNtdllProcAddress("NtResumeProcess"));
        if (ntResumeProcess == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtResumeProcess not available.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(ProcessSuspendResumeAccess, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_SUSPEND_RESUME) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const NTSTATUS resumeStatus = ntResumeProcess(processHandle);
        ::CloseHandle(processHandle);
        if (NT_SUCCESS(resumeStatus))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = FormatNtStatusMessage(resumeStatus, "NtResumeProcess failed");
        }
        return false;
    }

    bool SetProcessCriticalFlag(const std::uint32_t pid, const bool enableCritical, std::string* const errorMessage)
    {
        const auto ntSetInformationProcess = reinterpret_cast<NtSetInformationProcessFn>(
            GetNtdllProcAddress("NtSetInformationProcess"));
        if (ntSetInformationProcess == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "NtSetInformationProcess not available.";
            }
            return false;
        }

        // 设置关键进程通常需要 SeDebugPrivilege。
        EnablePrivilege(SE_DEBUG_NAME);

        const HANDLE processHandle = ::OpenProcess(PROCESS_SET_INFORMATION, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_SET_INFORMATION) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        ULONG criticalFlag = enableCritical ? 1UL : 0UL;
        const NTSTATUS setStatus = ntSetInformationProcess(
            processHandle,
            ProcessBreakOnTerminationInfoClass,
            &criticalFlag,
            static_cast<ULONG>(sizeof(criticalFlag)));
        ::CloseHandle(processHandle);

        if (NT_SUCCESS(setStatus))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = FormatNtStatusMessage(
                setStatus,
                enableCritical ? "Set critical process failed" : "Clear critical process failed");
        }
        return false;
    }

    bool SetProcessPriority(const std::uint32_t pid, const ProcessPriorityLevel priorityLevel, std::string* const errorMessage)
    {
        DWORD priorityClass = NORMAL_PRIORITY_CLASS;
        switch (priorityLevel)
        {
        case ProcessPriorityLevel::Idle:
            priorityClass = IDLE_PRIORITY_CLASS;
            break;
        case ProcessPriorityLevel::BelowNormal:
            priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
            break;
        case ProcessPriorityLevel::Normal:
            priorityClass = NORMAL_PRIORITY_CLASS;
            break;
        case ProcessPriorityLevel::AboveNormal:
            priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
            break;
        case ProcessPriorityLevel::High:
            priorityClass = HIGH_PRIORITY_CLASS;
            break;
        case ProcessPriorityLevel::Realtime:
            priorityClass = REALTIME_PRIORITY_CLASS;
            break;
        default:
            priorityClass = NORMAL_PRIORITY_CLASS;
            break;
        }

        const HANDLE processHandle = ::OpenProcess(PROCESS_SET_INFORMATION, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_SET_INFORMATION) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const BOOL setResult = ::SetPriorityClass(processHandle, priorityClass);
        const DWORD setError = ::GetLastError();
        ::CloseHandle(processHandle);
        if (setResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "SetPriorityClass failed: " + FormatLastErrorMessage(setError);
        }
        return false;
    }

    bool SetProcessEfficiencyMode(
        const std::uint32_t pid,
        const bool enableEfficiencyMode,
        std::string* const errorMessage)
    {
        HMODULE kernel32Module = ::GetModuleHandleW(L"kernel32.dll");
        const auto setProcessInformation = reinterpret_cast<SetProcessInformationFn>(
            kernel32Module != nullptr ? ::GetProcAddress(kernel32Module, "SetProcessInformation") : nullptr);
        if (setProcessInformation == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "SetProcessInformation(ProcessPowerThrottling) is not available.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(PROCESS_SET_INFORMATION, FALSE, ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(PROCESS_SET_INFORMATION) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        ProcessPowerThrottlingStateNative powerState{};
        powerState.version = ProcessPowerThrottlingCurrentVersion;
        powerState.controlMask = ProcessPowerThrottlingExecutionSpeed;
        powerState.stateMask = enableEfficiencyMode ? ProcessPowerThrottlingExecutionSpeed : 0UL;
        const BOOL setOk = setProcessInformation(
            processHandle,
            ProcessPowerThrottlingInfoClass,
            &powerState,
            static_cast<DWORD>(sizeof(powerState)));
        const DWORD setError = ::GetLastError();
        ::CloseHandle(processHandle);
        if (setOk != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "SetProcessInformation(ProcessPowerThrottling) failed: "
                + FormatLastErrorMessage(setError);
        }
        return false;
    }

    bool OpenProcessFolder(const std::uint32_t pid, std::string* const errorMessage)
    {
        const std::string processPath = QueryProcessPathByPid(pid);
        if (processPath.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Process path is empty or inaccessible.";
            }
            return false;
        }

        return OpenInExplorerByPath(processPath, errorMessage);
    }

    bool QueryProcessStaticDetailByPid(const std::uint32_t pid, ProcessRecord& outRecord)
    {
        outRecord = ProcessRecord{};
        outRecord.pid = pid;
        outRecord.processName = GetProcessNameByPID(pid);

        // 先刷新动态计数器，保证创建时间/内存等字段尽可能可用。
        RefreshProcessDynamicCounters(outRecord);

        // 再补静态详情（签名默认开启）。
        const bool staticOk = FillProcessStaticDetails(outRecord, true);

        // 最后做一次兜底：若进程名仍为空，用 PID 文本占位。
        if (outRecord.processName.empty())
        {
            outRecord.processName = "PID_" + std::to_string(pid);
        }
        return staticOk;
    }

    ProcessModuleSnapshot EnumerateProcessModulesAndThreads(
        const std::uint32_t pid,
        const bool includeSignatureCheck)
    {
        ProcessModuleSnapshot snapshot;
        snapshot.modules.clear();
        snapshot.threads.clear();
        snapshot.diagnosticText.clear();

        // appendDiagnostic 作用：
        // - 累积模块刷新诊断文本；
        // - 多段错误通过 " | " 拼接，便于 UI 一次展示完整上下文。
        const auto appendDiagnostic = [&snapshot](const std::string& text)
            {
                if (text.empty())
                {
                    return;
                }
                if (!snapshot.diagnosticText.empty())
                {
                    snapshot.diagnosticText += " | ";
                }
                snapshot.diagnosticText += text;
            };

        // fillModuleSignature 作用：
        // - 统一填充模块签名显示文本、厂家、可信标记；
        // - includeSignatureCheck=false 时保持 Pending（快速模式）。
        const auto fillModuleSignature = [includeSignatureCheck](ProcessModuleRecord& moduleRecord)
            {
                if (includeSignatureCheck)
                {
                    const FileSignatureInfo signatureInfo = QueryFileSignatureInfo(moduleRecord.modulePath);
                    moduleRecord.signatureState = signatureInfo.displayText.empty() ? "Unknown" : signatureInfo.displayText;
                    moduleRecord.signaturePublisher = signatureInfo.publisher;
                    moduleRecord.signatureTrusted = signatureInfo.trustedByWindows;
                    return;
                }
                moduleRecord.signatureState = "Pending";
                moduleRecord.signaturePublisher.clear();
                moduleRecord.signatureTrusted = false;
            };

        // 先枚举线程，供模块行填充 ThreadID 信息。
        std::vector<std::uint32_t> threadIdList;
        HANDLE threadSnapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (threadSnapshotHandle != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 threadEntry{};
            threadEntry.dwSize = sizeof(threadEntry);
            if (::Thread32First(threadSnapshotHandle, &threadEntry) != FALSE)
            {
                do
                {
                    if (threadEntry.th32OwnerProcessID != pid)
                    {
                        continue;
                    }

                    ProcessThreadRecord threadRecord{};
                    threadRecord.threadId = static_cast<std::uint32_t>(threadEntry.th32ThreadID);
                    threadRecord.ownerPid = static_cast<std::uint32_t>(threadEntry.th32OwnerProcessID);
                    threadRecord.basePriority = static_cast<int>(threadEntry.tpBasePri);
                    threadRecord.stateText = "Running";
                    snapshot.threads.push_back(threadRecord);
                    threadIdList.push_back(threadRecord.threadId);
                } while (::Thread32Next(threadSnapshotHandle, &threadEntry) != FALSE);
            }
            ::CloseHandle(threadSnapshotHandle);
        }

        // 第一优先：Toolhelp 模块枚举（常规路径）。
        bool moduleEnumerated = false;
        HANDLE moduleSnapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ToDwordPid(pid));
        if (moduleSnapshotHandle == INVALID_HANDLE_VALUE)
        {
            appendDiagnostic("CreateToolhelp32Snapshot(module) failed: " + FormatLastErrorMessage(::GetLastError()));
        }
        else
        {
            MODULEENTRY32W moduleEntry{};
            moduleEntry.dwSize = sizeof(moduleEntry);
            if (::Module32FirstW(moduleSnapshotHandle, &moduleEntry) == FALSE)
            {
                appendDiagnostic("Module32FirstW failed: " + FormatLastErrorMessage(::GetLastError()));
            }
            else
            {
                do
                {
                    ProcessModuleRecord moduleRecord{};
                    moduleRecord.moduleName = ks::str::Utf16ToUtf8(moduleEntry.szModule);
                    moduleRecord.modulePath = ks::str::Utf16ToUtf8(moduleEntry.szExePath);
                    moduleRecord.moduleBaseAddress = reinterpret_cast<std::uint64_t>(moduleEntry.modBaseAddr);
                    moduleRecord.moduleSizeBytes = static_cast<std::uint32_t>(moduleEntry.modBaseSize);
                    moduleRecord.entryPointRva = QueryImageEntryPointRvaByPath(moduleRecord.modulePath);
                    moduleRecord.runningState = "Loaded";
                    fillModuleSignature(moduleRecord);

                    moduleRecord.representativeThreadId = threadIdList.empty() ? 0 : threadIdList.front();
                    moduleRecord.threadIdText = BuildThreadIdSummaryText(threadIdList);
                    snapshot.modules.push_back(std::move(moduleRecord));
                } while (::Module32NextW(moduleSnapshotHandle, &moduleEntry) != FALSE);
                moduleEnumerated = true;
                appendDiagnostic("Module source: Toolhelp");
            }
            ::CloseHandle(moduleSnapshotHandle);
        }

        // Toolhelp 失败时，回退 PSAPI，解决“模块始终为 0”问题。
        if (!moduleEnumerated)
        {
            const HANDLE processHandle = ::OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                FALSE,
                ToDwordPid(pid));
            if (processHandle == nullptr)
            {
                appendDiagnostic("PSAPI fallback open process failed: " + FormatLastErrorMessage(::GetLastError()));
                return snapshot;
            }

            std::vector<HMODULE> moduleHandleBuffer(2048);
            DWORD bytesNeeded = 0;
            if (::EnumProcessModulesEx(
                processHandle,
                moduleHandleBuffer.data(),
                static_cast<DWORD>(moduleHandleBuffer.size() * sizeof(HMODULE)),
                &bytesNeeded,
                LIST_MODULES_ALL) == FALSE)
            {
                appendDiagnostic("EnumProcessModulesEx failed: " + FormatLastErrorMessage(::GetLastError()));
                ::CloseHandle(processHandle);
                return snapshot;
            }

            const std::size_t moduleCount = static_cast<std::size_t>(bytesNeeded / sizeof(HMODULE));
            if (moduleCount > moduleHandleBuffer.size())
            {
                moduleHandleBuffer.resize(moduleCount);
                if (::EnumProcessModulesEx(
                    processHandle,
                    moduleHandleBuffer.data(),
                    static_cast<DWORD>(moduleHandleBuffer.size() * sizeof(HMODULE)),
                    &bytesNeeded,
                    LIST_MODULES_ALL) == FALSE)
                {
                    appendDiagnostic("EnumProcessModulesEx(second pass) failed: " + FormatLastErrorMessage(::GetLastError()));
                    ::CloseHandle(processHandle);
                    return snapshot;
                }
            }

            for (std::size_t moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex)
            {
                const HMODULE moduleHandle = moduleHandleBuffer[moduleIndex];
                if (moduleHandle == nullptr)
                {
                    continue;
                }

                wchar_t modulePathBuffer[32768] = {};
                const DWORD modulePathLength = ::GetModuleFileNameExW(
                    processHandle,
                    moduleHandle,
                    modulePathBuffer,
                    static_cast<DWORD>(std::size(modulePathBuffer)));
                if (modulePathLength == 0)
                {
                    continue;
                }

                MODULEINFO moduleInfo{};
                if (::GetModuleInformation(
                    processHandle,
                    moduleHandle,
                    &moduleInfo,
                    static_cast<DWORD>(sizeof(moduleInfo))) == FALSE)
                {
                    continue;
                }

                ProcessModuleRecord moduleRecord{};
                moduleRecord.modulePath = ks::str::Utf16ToUtf8(std::wstring(modulePathBuffer, modulePathLength));
                moduleRecord.moduleName = ExtractFileNameFromPath(moduleRecord.modulePath);
                moduleRecord.moduleBaseAddress = reinterpret_cast<std::uint64_t>(moduleInfo.lpBaseOfDll);
                moduleRecord.moduleSizeBytes = static_cast<std::uint32_t>(moduleInfo.SizeOfImage);

                const std::uint64_t entryPointAddress = reinterpret_cast<std::uint64_t>(moduleInfo.EntryPoint);
                const std::uint64_t baseAddress = moduleRecord.moduleBaseAddress;
                if (entryPointAddress >= baseAddress)
                {
                    moduleRecord.entryPointRva = static_cast<std::uint32_t>(entryPointAddress - baseAddress);
                }
                else
                {
                    moduleRecord.entryPointRva = QueryImageEntryPointRvaByPath(moduleRecord.modulePath);
                }

                moduleRecord.runningState = "Loaded";
                fillModuleSignature(moduleRecord);
                moduleRecord.representativeThreadId = threadIdList.empty() ? 0 : threadIdList.front();
                moduleRecord.threadIdText = BuildThreadIdSummaryText(threadIdList);
                snapshot.modules.push_back(std::move(moduleRecord));
            }

            moduleEnumerated = true;
            appendDiagnostic("Module source: PSAPI fallback");
            ::CloseHandle(processHandle);
        }

        if (!moduleEnumerated)
        {
            appendDiagnostic("No module enumeration path succeeded.");
        }
        else if (snapshot.modules.empty())
        {
            appendDiagnostic("Module enumeration succeeded but module count is 0.");
        }

        return snapshot;
    }

    bool UnloadModuleByBaseAddress(
        const std::uint32_t pid,
        const std::uint64_t moduleBaseAddress,
        std::string* const errorMessage)
    {
        const HANDLE processHandle = ::OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for unload module) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        FARPROC freeLibraryAddress = ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "FreeLibrary");
        if (freeLibraryAddress == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "GetProcAddress(FreeLibrary) failed.";
            }
            ::CloseHandle(processHandle);
            return false;
        }

        HANDLE remoteThread = ::CreateRemoteThread(
            processHandle,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(freeLibraryAddress),
            reinterpret_cast<LPVOID>(moduleBaseAddress),
            0,
            nullptr);
        if (remoteThread == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateRemoteThread(FreeLibrary) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(processHandle);
            return false;
        }

        ::WaitForSingleObject(remoteThread, 5000);
        DWORD threadExitCode = 0;
        ::GetExitCodeThread(remoteThread, &threadExitCode);
        ::CloseHandle(remoteThread);
        ::CloseHandle(processHandle);

        if (threadExitCode != 0)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "FreeLibrary returned 0, module may not be unloaded.";
        }
        return false;
    }

    bool SuspendThreadById(const std::uint32_t threadId, std::string* const errorMessage)
    {
        const HANDLE threadHandle = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadId);
        if (threadHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenThread(THREAD_SUSPEND_RESUME) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const DWORD suspendResult = ::SuspendThread(threadHandle);
        ::CloseHandle(threadHandle);
        if (suspendResult != static_cast<DWORD>(-1))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "SuspendThread failed: " + FormatLastErrorMessage(::GetLastError());
        }
        return false;
    }

    bool ResumeThreadById(const std::uint32_t threadId, std::string* const errorMessage)
    {
        const HANDLE threadHandle = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadId);
        if (threadHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenThread(THREAD_SUSPEND_RESUME) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const DWORD resumeResult = ::ResumeThread(threadHandle);
        ::CloseHandle(threadHandle);
        if (resumeResult != static_cast<DWORD>(-1))
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "ResumeThread failed: " + FormatLastErrorMessage(::GetLastError());
        }
        return false;
    }

    bool TerminateThreadById(const std::uint32_t threadId, std::string* const errorMessage)
    {
        const HANDLE threadHandle = ::OpenThread(THREAD_TERMINATE, FALSE, threadId);
        if (threadHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenThread(THREAD_TERMINATE) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const BOOL terminateResult = ::TerminateThread(threadHandle, 1);
        const DWORD terminateError = ::GetLastError();
        ::CloseHandle(threadHandle);
        if (terminateResult != FALSE)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "TerminateThread failed: " + FormatLastErrorMessage(terminateError);
        }
        return false;
    }

    bool InjectDllByPath(const std::uint32_t pid, const std::string& dllPath, std::string* const errorMessage)
    {
        if (dllPath.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "DLL path is empty.";
            }
            return false;
        }

        const std::wstring dllPathWide = ks::str::Utf8ToUtf16(dllPath);
        if (dllPathWide.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "DLL path UTF-8 -> UTF-16 conversion failed.";
            }
            return false;
        }

        const DWORD pathAttributes = ::GetFileAttributesW(dllPathWide.c_str());
        if (pathAttributes == INVALID_FILE_ATTRIBUTES || (pathAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "DLL path does not exist or points to a directory.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for DLL inject) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        const std::size_t byteCount = (dllPathWide.size() + 1) * sizeof(wchar_t);
        void* remotePathMemory = ::VirtualAllocEx(
            processHandle,
            nullptr,
            byteCount,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (remotePathMemory == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "VirtualAllocEx(for DLL path) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(processHandle);
            return false;
        }

        SIZE_T bytesWritten = 0;
        if (::WriteProcessMemory(
            processHandle,
            remotePathMemory,
            dllPathWide.c_str(),
            byteCount,
            &bytesWritten) == FALSE ||
            bytesWritten != byteCount)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "WriteProcessMemory(DLL path) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        FARPROC loadLibraryAddress = ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (loadLibraryAddress == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "GetProcAddress(LoadLibraryW) failed.";
            }
            ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        HANDLE remoteThread = ::CreateRemoteThread(
            processHandle,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddress),
            remotePathMemory,
            0,
            nullptr);
        if (remoteThread == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateRemoteThread(LoadLibraryW) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        ::WaitForSingleObject(remoteThread, 10000);
        DWORD threadExitCode = 0;
        ::GetExitCodeThread(remoteThread, &threadExitCode);
        ::CloseHandle(remoteThread);
        ::VirtualFreeEx(processHandle, remotePathMemory, 0, MEM_RELEASE);
        ::CloseHandle(processHandle);

        if (threadExitCode != 0)
        {
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "LoadLibraryW returned NULL, DLL may not be loaded.";
        }
        return false;
    }

    bool InjectShellcodeBuffer(
        const std::uint32_t pid,
        const std::vector<std::uint8_t>& shellcodeBuffer,
        std::string* const errorMessage)
    {
        if (shellcodeBuffer.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Shellcode buffer is empty.";
            }
            return false;
        }

        const HANDLE processHandle = ::OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            ToDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "OpenProcess(for shellcode inject) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            return false;
        }

        void* remoteMemory = ::VirtualAllocEx(
            processHandle,
            nullptr,
            shellcodeBuffer.size(),
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (remoteMemory == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "VirtualAllocEx(for shellcode) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::CloseHandle(processHandle);
            return false;
        }

        SIZE_T bytesWritten = 0;
        if (::WriteProcessMemory(
            processHandle,
            remoteMemory,
            shellcodeBuffer.data(),
            shellcodeBuffer.size(),
            &bytesWritten) == FALSE ||
            bytesWritten != shellcodeBuffer.size())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "WriteProcessMemory(shellcode) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::VirtualFreeEx(processHandle, remoteMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        HANDLE remoteThread = ::CreateRemoteThread(
            processHandle,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteMemory),
            nullptr,
            0,
            nullptr);
        if (remoteThread == nullptr)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "CreateRemoteThread(shellcode) failed: " + FormatLastErrorMessage(::GetLastError());
            }
            ::VirtualFreeEx(processHandle, remoteMemory, 0, MEM_RELEASE);
            ::CloseHandle(processHandle);
            return false;
        }

        ::WaitForSingleObject(remoteThread, 2000);
        ::CloseHandle(remoteThread);
        ::CloseHandle(processHandle);

        // 为避免 shellcode 仍在执行时释放内存导致异常，此处不主动释放 remoteMemory。
        return true;
    }

    bool OpenFolderByPath(const std::string& targetPath, std::string* const errorMessage)
    {
        return OpenInExplorerByPath(targetPath, errorMessage);
    }

    bool ApplyTokenPrivilegeEditsByPid(
        const std::uint32_t sourcePid,
        const std::uint32_t tokenDesiredAccess,
        const bool duplicatePrimaryToken,
        const std::vector<TokenPrivilegeEdit>& edits,
        std::string* const errorMessage)
    {
        const DWORD desiredAccess = tokenDesiredAccess == 0
            ? DefaultTokenDesiredAccess
            : static_cast<DWORD>(tokenDesiredAccess);

        HANDLE sourceToken = nullptr;
        if (!OpenTokenByProcessPid(sourcePid, desiredAccess, sourceToken, errorMessage))
        {
            return false;
        }

        HANDLE workingToken = sourceToken;
        HANDLE duplicatedToken = nullptr;
        if (duplicatePrimaryToken)
        {
            if (!DuplicatePrimaryToken(sourceToken, desiredAccess, duplicatedToken, errorMessage))
            {
                ::CloseHandle(sourceToken);
                return false;
            }
            workingToken = duplicatedToken;
        }

        const bool adjustOk = ApplyPrivilegeEdits(workingToken, edits, errorMessage);

        if (duplicatedToken != nullptr)
        {
            ::CloseHandle(duplicatedToken);
        }
        if (sourceToken != nullptr)
        {
            ::CloseHandle(sourceToken);
        }
        if (adjustOk && errorMessage != nullptr)
        {
            std::ostringstream stream;
            stream << "AdjustTokenPrivileges succeeded, edited privileges=" << edits.size();
            *errorMessage = stream.str();
        }
        return adjustOk;
    }

    bool LaunchProcess(
        const CreateProcessRequest& request,
        CreateProcessResult* const resultOut)
    {
        CreateProcessResult localResult{};
        localResult.usedTokenPath = request.tokenModeEnabled;

        std::wstring applicationNameWide;
        LPCWSTR applicationNamePtr = nullptr;
        if (request.useApplicationName)
        {
            applicationNameWide = ks::str::Utf8ToUtf16(request.applicationName);
            applicationNamePtr = applicationNameWide.empty() ? nullptr : applicationNameWide.c_str();
        }

        std::wstring commandLineWide;
        std::vector<wchar_t> commandLineBuffer;
        LPWSTR commandLinePtr = nullptr;
        if (request.useCommandLine)
        {
            commandLineWide = ks::str::Utf8ToUtf16(request.commandLine);
            commandLineBuffer.assign(commandLineWide.begin(), commandLineWide.end());
            commandLineBuffer.push_back(L'\0');
            commandLinePtr = commandLineBuffer.data();
        }

        SECURITY_ATTRIBUTES processSecurityAttributes{};
        SECURITY_ATTRIBUTES threadSecurityAttributes{};
        SECURITY_ATTRIBUTES* processSecurityPtr = BuildSecurityAttributes(
            request.processAttributes,
            processSecurityAttributes) ? &processSecurityAttributes : nullptr;
        SECURITY_ATTRIBUTES* threadSecurityPtr = BuildSecurityAttributes(
            request.threadAttributes,
            threadSecurityAttributes) ? &threadSecurityAttributes : nullptr;

        std::vector<wchar_t> environmentBlock;
        LPVOID environmentPtr = nullptr;
        DWORD creationFlags = static_cast<DWORD>(request.creationFlags);
        if (request.useEnvironment)
        {
            environmentBlock = BuildUnicodeEnvironmentBlock(request.environmentEntries);
            environmentPtr = environmentBlock.empty() ? nullptr : environmentBlock.data();
            if (request.environmentUnicode)
            {
                creationFlags |= CREATE_UNICODE_ENVIRONMENT;
            }
        }

        std::wstring currentDirectoryWide;
        LPCWSTR currentDirectoryPtr = nullptr;
        if (request.useCurrentDirectory)
        {
            currentDirectoryWide = ks::str::Utf8ToUtf16(request.currentDirectory);
            currentDirectoryPtr = currentDirectoryWide.empty() ? nullptr : currentDirectoryWide.c_str();
        }

        STARTUPINFOW startupInfo{};
        StartupInfoBufferSet startupBufferSet{};
        STARTUPINFOW* startupInfoPtr = BuildStartupInfo(
            request.startupInfo,
            startupInfo,
            startupBufferSet) ? &startupInfo : nullptr;

        PROCESS_INFORMATION processInfo{};
        PROCESS_INFORMATION* processInfoPtr = BuildProcessInfoInput(
            request.processInfo,
            processInfo) ? &processInfo : nullptr;

        auto finalizeSuccess = [&localResult, processInfoPtr]() {
            localResult.success = true;
            if (processInfoPtr == nullptr)
            {
                return;
            }

            localResult.processInfoAvailable = true;
            localResult.hProcess = HandleToUint64(processInfoPtr->hProcess);
            localResult.hThread = HandleToUint64(processInfoPtr->hThread);
            localResult.dwProcessId = static_cast<std::uint32_t>(processInfoPtr->dwProcessId);
            localResult.dwThreadId = static_cast<std::uint32_t>(processInfoPtr->dwThreadId);

            if (processInfoPtr->hThread != nullptr)
            {
                ::CloseHandle(processInfoPtr->hThread);
                processInfoPtr->hThread = nullptr;
            }
            if (processInfoPtr->hProcess != nullptr)
            {
                ::CloseHandle(processInfoPtr->hProcess);
                processInfoPtr->hProcess = nullptr;
            }
        };

        if (!request.tokenModeEnabled)
        {
            const BOOL createOk = ::CreateProcessW(
                applicationNamePtr,
                commandLinePtr,
                processSecurityPtr,
                threadSecurityPtr,
                request.inheritHandles ? TRUE : FALSE,
                creationFlags,
                environmentPtr,
                currentDirectoryPtr,
                startupInfoPtr,
                processInfoPtr);

            if (createOk == FALSE)
            {
                localResult.win32Error = static_cast<std::uint32_t>(::GetLastError());
                localResult.detailText = "CreateProcessW failed: " + FormatLastErrorMessage(localResult.win32Error);
                if (resultOut != nullptr)
                {
                    *resultOut = localResult;
                }
                return false;
            }

            localResult.detailText = "CreateProcessW succeeded.";
            finalizeSuccess();
            if (resultOut != nullptr)
            {
                *resultOut = localResult;
            }
            return true;
        }

        const DWORD desiredAccess = request.tokenDesiredAccess == 0
            ? DefaultTokenDesiredAccess
            : static_cast<DWORD>(request.tokenDesiredAccess);
        HANDLE sourceToken = nullptr;
        std::string tokenError;
        if (!OpenTokenByProcessPid(request.tokenSourcePid, desiredAccess, sourceToken, &tokenError))
        {
            localResult.win32Error = static_cast<std::uint32_t>(::GetLastError());
            localResult.detailText = tokenError;
            if (resultOut != nullptr)
            {
                *resultOut = localResult;
            }
            return false;
        }

        HANDLE workingToken = sourceToken;
        HANDLE duplicatedPrimaryToken = nullptr;
        if (request.duplicatePrimaryToken)
        {
            if (!DuplicatePrimaryToken(sourceToken, desiredAccess, duplicatedPrimaryToken, &tokenError))
            {
                ::CloseHandle(sourceToken);
                localResult.win32Error = static_cast<std::uint32_t>(::GetLastError());
                localResult.detailText = tokenError;
                if (resultOut != nullptr)
                {
                    *resultOut = localResult;
                }
                return false;
            }
            workingToken = duplicatedPrimaryToken;
        }

        if (!ApplyPrivilegeEdits(workingToken, request.tokenPrivilegeEdits, &tokenError))
        {
            if (duplicatedPrimaryToken != nullptr)
            {
                ::CloseHandle(duplicatedPrimaryToken);
            }
            ::CloseHandle(sourceToken);
            localResult.win32Error = static_cast<std::uint32_t>(::GetLastError());
            localResult.detailText = tokenError;
            if (resultOut != nullptr)
            {
                *resultOut = localResult;
            }
            return false;
        }

        const BOOL createAsUserOk = ::CreateProcessAsUserW(
            workingToken,
            applicationNamePtr,
            commandLinePtr,
            processSecurityPtr,
            threadSecurityPtr,
            request.inheritHandles ? TRUE : FALSE,
            creationFlags,
            environmentPtr,
            currentDirectoryPtr,
            startupInfoPtr,
            processInfoPtr);
        if (createAsUserOk != FALSE)
        {
            localResult.detailText = "CreateProcessAsUserW succeeded.";
            finalizeSuccess();
            if (duplicatedPrimaryToken != nullptr)
            {
                ::CloseHandle(duplicatedPrimaryToken);
            }
            ::CloseHandle(sourceToken);
            if (resultOut != nullptr)
            {
                *resultOut = localResult;
            }
            return true;
        }

        const DWORD createAsUserError = ::GetLastError();
        // 兼容性回退：部分环境缺少 SeAssignPrimaryTokenPrivilege 时尝试 CreateProcessWithTokenW。
        const BOOL createWithTokenOk = ::CreateProcessWithTokenW(
            workingToken,
            LOGON_WITH_PROFILE,
            applicationNamePtr,
            commandLinePtr,
            creationFlags,
            environmentPtr,
            currentDirectoryPtr,
            startupInfoPtr,
            processInfoPtr);
        if (createWithTokenOk != FALSE)
        {
            localResult.usedCreateProcessWithTokenFallback = true;
            localResult.detailText = "CreateProcessAsUserW failed, fallback CreateProcessWithTokenW succeeded.";
            finalizeSuccess();
            if (duplicatedPrimaryToken != nullptr)
            {
                ::CloseHandle(duplicatedPrimaryToken);
            }
            ::CloseHandle(sourceToken);
            if (resultOut != nullptr)
            {
                *resultOut = localResult;
            }
            return true;
        }

        const DWORD fallbackError = ::GetLastError();
        std::ostringstream detailStream;
        detailStream
            << "CreateProcessAsUserW failed: " << FormatLastErrorMessage(createAsUserError)
            << " | CreateProcessWithTokenW fallback failed: " << FormatLastErrorMessage(fallbackError);
        localResult.win32Error = static_cast<std::uint32_t>(fallbackError);
        localResult.detailText = detailStream.str();

        if (duplicatedPrimaryToken != nullptr)
        {
            ::CloseHandle(duplicatedPrimaryToken);
        }
        ::CloseHandle(sourceToken);
        if (resultOut != nullptr)
        {
            *resultOut = localResult;
        }
        return false;
    }
    std::wstring GetCurrentProcessPath()
    {
        wchar_t szPath[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(NULL, szPath, MAX_PATH) == 0)
        {
            return L"";
        }
        return std::wstring(szPath);
    }
}
