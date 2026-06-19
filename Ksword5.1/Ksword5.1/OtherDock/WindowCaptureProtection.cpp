#include "WindowCaptureProtection.h"

// ============================================================
// WindowCaptureProtection.cpp
// 作用说明：
// 1) 封装 SetWindowDisplayAffinity 的本进程和跨进程调用；
// 2) 跨进程路径仅支持同架构 x64 GUI 进程；
// 3) 失败时尽量保留系统错误码，便于 UI 给出可操作诊断。
// ============================================================

#include <array>
#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

namespace
{
    // RemoteAffinityParameter 布局：
    // - 偏移必须和 x64 shellcode 中的读取偏移保持一致；
    // - 只使用整数类型，避免远程进程 ABI 和结构体填充差异。
    constexpr std::size_t kRemoteParamSize = 0x28;
    constexpr std::size_t kOffsetSetWindowDisplayAffinity = 0x00;
    constexpr std::size_t kOffsetGetLastError = 0x08;
    constexpr std::size_t kOffsetTargetHwnd = 0x10;
    constexpr std::size_t kOffsetAffinity = 0x18;
    constexpr std::size_t kOffsetCallResult = 0x1C;
    constexpr std::size_t kOffsetLastError = 0x20;

    // HandleGuard 作用：
    // - 用 RAII 关闭 Win32 HANDLE；
    // - 调用：进程、快照、远程线程句柄都通过该类托管。
    class HandleGuard
    {
    public:
        explicit HandleGuard(const HANDLE handleValue = nullptr)
            : m_handle(handleValue)
        {
        }

        ~HandleGuard()
        {
            reset(nullptr);
        }

        HandleGuard(const HandleGuard&) = delete;
        HandleGuard& operator=(const HandleGuard&) = delete;

        HANDLE get() const
        {
            return m_handle;
        }

        void reset(const HANDLE newHandle)
        {
            if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_handle);
            }
            m_handle = newHandle;
        }

    private:
        HANDLE m_handle = nullptr; // m_handle：当前托管的 Win32 句柄。
    };

    // appendDetail 作用：
    // - 拼接诊断文本；
    // - 传入 partText：单段诊断；
    // - 传出 detailText：用分号分隔的累计诊断。
    void appendDetail(std::string& detailText, const std::string& partText)
    {
        if (partText.empty())
        {
            return;
        }

        if (!detailText.empty())
        {
            detailText += "; ";
        }
        detailText += partText;
    }

    // formatWin32Error 作用：
    // - 将 Win32 错误码格式化为稳定日志文本；
    // - 仅输出数字，避免不同系统语言导致日志难以比对。
    std::string formatWin32Error(const char* const operationName, const DWORD errorCode)
    {
        std::string operationText = operationName != nullptr ? operationName : "Win32";
        operationText += " failed, error=";
        operationText += std::to_string(static_cast<unsigned long>(errorCode));
        return operationText;
    }

    // hwndFromValue 作用：
    // - 将 UI 保存的整数 HWND 恢复为 Win32 HWND；
    // - 传入 hwndValue：quint64/std::uint64_t 形式窗口句柄。
    HWND hwndFromValue(const std::uint64_t hwndValue)
    {
        return reinterpret_cast<HWND>(static_cast<UINT_PTR>(hwndValue));
    }

    // hwndToValue 作用：
    // - 将 Win32 HWND 转成稳定整数；
    // - 调用：结果结构里不暴露 Windows.h 类型。
    std::uint64_t hwndToValue(const HWND windowHandle)
    {
        return static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(windowHandle));
    }

    // writeUInt64 作用：
    // - 按小端写入远程参数块；
    // - 传入 offsetValue：参数块偏移；
    // - 传入 integerValue：要写入的 64 位值。
    void writeUInt64(
        std::array<std::uint8_t, kRemoteParamSize>& parameterBytes,
        const std::size_t offsetValue,
        const std::uint64_t integerValue)
    {
        for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index)
        {
            parameterBytes[offsetValue + index] =
                static_cast<std::uint8_t>((integerValue >> (index * 8)) & 0xFF);
        }
    }

    // writeUInt32 作用：
    // - 按小端写入远程参数块；
    // - 传入 offsetValue：参数块偏移；
    // - 传入 integerValue：要写入的 32 位值。
    void writeUInt32(
        std::array<std::uint8_t, kRemoteParamSize>& parameterBytes,
        const std::size_t offsetValue,
        const std::uint32_t integerValue)
    {
        for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index)
        {
            parameterBytes[offsetValue + index] =
                static_cast<std::uint8_t>((integerValue >> (index * 8)) & 0xFF);
        }
    }

    // readUInt32 作用：
    // - 从远程参数块按小端读取 32 位值；
    // - 调用：远程线程结束后读取 callResult/lastError。
    std::uint32_t readUInt32(
        const std::array<std::uint8_t, kRemoteParamSize>& parameterBytes,
        const std::size_t offsetValue)
    {
        std::uint32_t integerValue = 0;
        for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index)
        {
            integerValue |= static_cast<std::uint32_t>(parameterBytes[offsetValue + index]) << (index * 8);
        }
        return integerValue;
    }

    // queryRootWindow 作用：
    // - SetWindowDisplayAffinity 只接受顶层窗口；
    // - 子窗口输入会归并到 GA_ROOT，保证“选中任意控件”也能保护所属顶层窗口。
    HWND queryRootWindow(const HWND requestedWindowHandle, bool& usedRootWindow)
    {
        usedRootWindow = false;
        HWND rootWindowHandle = ::GetAncestor(requestedWindowHandle, GA_ROOT);
        if (rootWindowHandle == nullptr)
        {
            return requestedWindowHandle;
        }

        if (rootWindowHandle != requestedWindowHandle)
        {
            usedRootWindow = true;
        }
        return rootWindowHandle;
    }

    // queryRemoteModuleBase 作用：
    // - 从目标进程模块快照中定位指定 DLL 基址；
    // - 传入 pid：目标进程 PID；
    // - 返回 0 表示快照失败或模块不存在。
    std::uint64_t queryRemoteModuleBase(
        const DWORD pid,
        const wchar_t* const moduleName,
        std::string& detailText)
    {
        HandleGuard snapshotHandle(::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            pid));
        if (snapshotHandle.get() == INVALID_HANDLE_VALUE)
        {
            appendDetail(detailText, formatWin32Error("CreateToolhelp32Snapshot(module)", ::GetLastError()));
            return 0;
        }

        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (::Module32FirstW(snapshotHandle.get(), &moduleEntry) == FALSE)
        {
            appendDetail(detailText, formatWin32Error("Module32FirstW", ::GetLastError()));
            return 0;
        }

        do
        {
            if (_wcsicmp(moduleEntry.szModule, moduleName) == 0)
            {
                return reinterpret_cast<std::uint64_t>(moduleEntry.modBaseAddr);
            }
        } while (::Module32NextW(snapshotHandle.get(), &moduleEntry) != FALSE);

        appendDetail(detailText, "target module not found");
        return 0;
    }

    // resolveRemoteProcedure 作用：
    // - 用“本机函数 RVA + 目标模块基址”计算远程函数地址；
    // - 目标与当前进程使用同一系统 DLL 版本时该策略稳定；
    // - 调用前会先校验目标进程架构。
    std::uint64_t resolveRemoteProcedure(
        const DWORD pid,
        const wchar_t* const moduleName,
        const char* const procedureName,
        std::string& detailText)
    {
        HMODULE localModuleHandle = ::GetModuleHandleW(moduleName);
        if (localModuleHandle == nullptr)
        {
            localModuleHandle = ::LoadLibraryW(moduleName);
        }
        if (localModuleHandle == nullptr)
        {
            appendDetail(detailText, formatWin32Error("LoadLibraryW(local module)", ::GetLastError()));
            return 0;
        }

        FARPROC localProcedureAddress = ::GetProcAddress(localModuleHandle, procedureName);
        if (localProcedureAddress == nullptr)
        {
            appendDetail(detailText, "GetProcAddress(local procedure) failed");
            return 0;
        }

        const std::uint64_t remoteModuleBase = queryRemoteModuleBase(pid, moduleName, detailText);
        if (remoteModuleBase == 0)
        {
            return 0;
        }

        const auto localBaseValue = reinterpret_cast<std::uint64_t>(localModuleHandle);
        const auto localProcedureValue = reinterpret_cast<std::uint64_t>(localProcedureAddress);
        return remoteModuleBase + (localProcedureValue - localBaseValue);
    }

    // queryProcessMachine 作用：
    // - 优先调用 IsWow64Process2 判断进程架构；
    // - 返回 true 表示已得到可比较的机器类型。
    bool queryProcessMachine(
        const HANDLE processHandle,
        USHORT& processMachine,
        USHORT& nativeMachine)
    {
        using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const HMODULE kernel32Handle = ::GetModuleHandleW(L"kernel32.dll");
        const auto isWow64Process2 = kernel32Handle != nullptr
            ? reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel32Handle, "IsWow64Process2"))
            : nullptr;
        if (isWow64Process2 != nullptr)
        {
            return isWow64Process2(processHandle, &processMachine, &nativeMachine) != FALSE;
        }

        BOOL isWow64 = FALSE;
        if (::IsWow64Process(processHandle, &isWow64) == FALSE)
        {
            return false;
        }

        nativeMachine = sizeof(void*) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
        processMachine = isWow64 != FALSE ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_UNKNOWN;
        return true;
    }

    // isTargetCompatibleArchitecture 作用：
    // - 当前实现的远程线程 stub 是 x64 指令；
    // - 若目标是 WOW64/32 位进程，则返回 false 并让 UI 展示限制原因。
    bool isTargetCompatibleArchitecture(
        const HANDLE targetProcessHandle,
        std::string& detailText)
    {
        USHORT currentProcessMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT currentNativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT targetProcessMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT targetNativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;

        if (!queryProcessMachine(::GetCurrentProcess(), currentProcessMachine, currentNativeMachine) ||
            !queryProcessMachine(targetProcessHandle, targetProcessMachine, targetNativeMachine))
        {
            appendDetail(detailText, "query process architecture failed");
            return false;
        }

        const bool currentIsWow64 = currentProcessMachine != IMAGE_FILE_MACHINE_UNKNOWN;
        const bool targetIsWow64 = targetProcessMachine != IMAGE_FILE_MACHINE_UNKNOWN;
        if (currentIsWow64 || targetIsWow64)
        {
            appendDetail(detailText, "remote capture protection only supports same-architecture x64 targets");
            return false;
        }

        if (currentNativeMachine != targetNativeMachine)
        {
            appendDetail(detailText, "native machine mismatch");
            return false;
        }
        return true;
    }

    // RemoteAffinityCallResult：
    // - 作用：区分“远程线程基础设施失败”和“远程 API 返回 FALSE”；
    // - 调用：启用保护时只有 API 返回 FALSE 才继续尝试 WDA_MONITOR 回退。
    struct RemoteAffinityCallResult
    {
        bool infrastructureOk = false;      // infrastructureOk：远程分配/写入/线程/读取是否完成。
        bool apiOk = false;                 // apiOk：SetWindowDisplayAffinity 返回值。
        DWORD apiLastError = 0;             // apiLastError：目标进程内 GetLastError 结果。
        std::string detail;                 // detail：基础设施失败时的诊断文本。
    };

    // remoteAffinityShellcode 作用：
    // - x64 远程线程入口，参数 RCX 指向 RemoteAffinityParameter；
    // - 调用目标进程内 user32!SetWindowDisplayAffinity；
    // - 失败时调用目标进程内 ntdll!RtlGetLastWin32Error 并写回参数块。
    const std::vector<std::uint8_t>& remoteAffinityShellcode()
    {
        static const std::vector<std::uint8_t> kShellcode = {
            0x53,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0x89, 0xCB,
            0x48, 0x8B, 0x03,
            0x48, 0x8B, 0x4B, 0x10,
            0x8B, 0x53, 0x18,
            0xFF, 0xD0,
            0x89, 0x43, 0x1C,
            0x85, 0xC0,
            0x75, 0x0B,
            0x48, 0x8B, 0x43, 0x08,
            0xFF, 0xD0,
            0x89, 0x43, 0x20,
            0xEB, 0x07,
            0xC7, 0x43, 0x20, 0x00, 0x00, 0x00, 0x00,
            0x8B, 0x43, 0x1C,
            0x48, 0x83, 0xC4, 0x20,
            0x5B,
            0xC3
        };
        return kShellcode;
    }

    // executeRemoteSetAffinity 作用：
    // - 在目标进程内执行 SetWindowDisplayAffinity(hwnd, affinity)；
    // - 传入 processHandle：具备远程线程和 VM 权限的进程句柄；
    // - 返回 RemoteAffinityCallResult，保留远程 API 与基础设施状态。
    RemoteAffinityCallResult executeRemoteSetAffinity(
        const HANDLE processHandle,
        const DWORD pid,
        const HWND windowHandle,
        const DWORD affinityValue)
    {
        RemoteAffinityCallResult callResult;
        std::string resolveDetail;
        const std::uint64_t remoteSetAffinityAddress = resolveRemoteProcedure(
            pid,
            L"user32.dll",
            "SetWindowDisplayAffinity",
            resolveDetail);
        const std::uint64_t remoteGetLastErrorAddress = resolveRemoteProcedure(
            pid,
            L"ntdll.dll",
            "RtlGetLastWin32Error",
            resolveDetail);
        if (remoteSetAffinityAddress == 0 || remoteGetLastErrorAddress == 0)
        {
            callResult.detail = resolveDetail.empty() ? "remote procedure resolve failed" : resolveDetail;
            return callResult;
        }

        std::array<std::uint8_t, kRemoteParamSize> parameterBytes{};
        writeUInt64(parameterBytes, kOffsetSetWindowDisplayAffinity, remoteSetAffinityAddress);
        writeUInt64(parameterBytes, kOffsetGetLastError, remoteGetLastErrorAddress);
        writeUInt64(parameterBytes, kOffsetTargetHwnd, hwndToValue(windowHandle));
        writeUInt32(parameterBytes, kOffsetAffinity, static_cast<std::uint32_t>(affinityValue));

        const std::vector<std::uint8_t>& shellcode = remoteAffinityShellcode();
        const SIZE_T shellcodeSize = static_cast<SIZE_T>(shellcode.size());
        const SIZE_T parameterSize = static_cast<SIZE_T>(parameterBytes.size());
        void* remoteBlock = ::VirtualAllocEx(
            processHandle,
            nullptr,
            shellcodeSize + parameterSize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (remoteBlock == nullptr)
        {
            callResult.detail = formatWin32Error("VirtualAllocEx(remote affinity block)", ::GetLastError());
            return callResult;
        }

        auto cleanupRemoteBlock = [&processHandle, &remoteBlock]() {
            if (remoteBlock != nullptr)
            {
                ::VirtualFreeEx(processHandle, remoteBlock, 0, MEM_RELEASE);
                remoteBlock = nullptr;
            }
        };

        SIZE_T bytesWritten = 0;
        if (::WriteProcessMemory(processHandle, remoteBlock, shellcode.data(), shellcodeSize, &bytesWritten) == FALSE ||
            bytesWritten != shellcodeSize)
        {
            callResult.detail = formatWin32Error("WriteProcessMemory(remote shellcode)", ::GetLastError());
            cleanupRemoteBlock();
            return callResult;
        }

        auto* const remoteParameter = static_cast<std::uint8_t*>(remoteBlock) + shellcodeSize;
        bytesWritten = 0;
        if (::WriteProcessMemory(processHandle, remoteParameter, parameterBytes.data(), parameterSize, &bytesWritten) == FALSE ||
            bytesWritten != parameterSize)
        {
            callResult.detail = formatWin32Error("WriteProcessMemory(remote parameter)", ::GetLastError());
            cleanupRemoteBlock();
            return callResult;
        }

        HandleGuard remoteThread(::CreateRemoteThread(
            processHandle,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBlock),
            remoteParameter,
            0,
            nullptr));
        if (remoteThread.get() == nullptr)
        {
            callResult.detail = formatWin32Error("CreateRemoteThread(remote affinity)", ::GetLastError());
            cleanupRemoteBlock();
            return callResult;
        }

        const DWORD waitResult = ::WaitForSingleObject(remoteThread.get(), 3000);
        if (waitResult != WAIT_OBJECT_0)
        {
            callResult.detail = waitResult == WAIT_TIMEOUT
                ? "remote affinity thread timed out"
                : formatWin32Error("WaitForSingleObject(remote affinity)", ::GetLastError());
            return callResult;
        }

        SIZE_T bytesRead = 0;
        if (::ReadProcessMemory(processHandle, remoteParameter, parameterBytes.data(), parameterSize, &bytesRead) == FALSE ||
            bytesRead != parameterSize)
        {
            callResult.detail = formatWin32Error("ReadProcessMemory(remote parameter)", ::GetLastError());
            cleanupRemoteBlock();
            return callResult;
        }

        cleanupRemoteBlock();
        callResult.infrastructureOk = true;
        callResult.apiOk = readUInt32(parameterBytes, kOffsetCallResult) != 0;
        callResult.apiLastError = readUInt32(parameterBytes, kOffsetLastError);
        return callResult;
    }

    // applyAffinityDirect 作用：
    // - 在当前进程内直接调用 SetWindowDisplayAffinity；
    // - 启用保护时先试 EXCLUDEFROMCAPTURE，再回退 MONITOR。
    bool applyAffinityDirect(
        const HWND windowHandle,
        const bool enableProtection,
        DWORD& appliedAffinity,
        DWORD& win32Error)
    {
        win32Error = 0;
        appliedAffinity = ks::window::kDisplayAffinityAllowCapture;
        if (!enableProtection)
        {
            if (::SetWindowDisplayAffinity(windowHandle, appliedAffinity) != FALSE)
            {
                return true;
            }
            win32Error = ::GetLastError();
            return false;
        }

        appliedAffinity = ks::window::kDisplayAffinityExcludeFromCapture;
        if (::SetWindowDisplayAffinity(windowHandle, appliedAffinity) != FALSE)
        {
            return true;
        }

        appliedAffinity = ks::window::kDisplayAffinityMonitorOnly;
        if (::SetWindowDisplayAffinity(windowHandle, appliedAffinity) != FALSE)
        {
            return true;
        }

        win32Error = ::GetLastError();
        return false;
    }

    // applyAffinityRemote 作用：
    // - 在外部进程中执行 DisplayAffinity 写入；
    // - 传入 pid/windowHandle/enableProtection；
    // - 失败时返回详细诊断，成功时写出实际 affinity。
    bool applyAffinityRemote(
        const DWORD pid,
        const HWND windowHandle,
        const bool enableProtection,
        DWORD& appliedAffinity,
        DWORD& win32Error,
        std::string& detailText)
    {
        HandleGuard processHandle(::OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid));
        if (processHandle.get() == nullptr)
        {
            win32Error = ::GetLastError();
            appendDetail(detailText, formatWin32Error("OpenProcess(remote affinity)", win32Error));
            return false;
        }

        if (!isTargetCompatibleArchitecture(processHandle.get(), detailText))
        {
            win32Error = ERROR_NOT_SUPPORTED;
            return false;
        }

        appliedAffinity = enableProtection
            ? ks::window::kDisplayAffinityExcludeFromCapture
            : ks::window::kDisplayAffinityAllowCapture;
        RemoteAffinityCallResult remoteCall = executeRemoteSetAffinity(
            processHandle.get(),
            pid,
            windowHandle,
            appliedAffinity);
        if (!remoteCall.infrastructureOk)
        {
            appendDetail(detailText, remoteCall.detail);
            win32Error = ERROR_GEN_FAILURE;
            return false;
        }
        if (remoteCall.apiOk)
        {
            win32Error = 0;
            return true;
        }

        if (!enableProtection)
        {
            win32Error = remoteCall.apiLastError;
            appendDetail(detailText, formatWin32Error("remote SetWindowDisplayAffinity(WDA_NONE)", win32Error));
            return false;
        }

        appliedAffinity = ks::window::kDisplayAffinityMonitorOnly;
        remoteCall = executeRemoteSetAffinity(
            processHandle.get(),
            pid,
            windowHandle,
            appliedAffinity);
        if (!remoteCall.infrastructureOk)
        {
            appendDetail(detailText, remoteCall.detail);
            win32Error = ERROR_GEN_FAILURE;
            return false;
        }
        if (remoteCall.apiOk)
        {
            win32Error = 0;
            return true;
        }

        win32Error = remoteCall.apiLastError;
        appendDetail(detailText, formatWin32Error("remote SetWindowDisplayAffinity(fallback WDA_MONITOR)", win32Error));
        return false;
    }
}

namespace ks::window
{
    CaptureProtectionResult SetWindowCaptureProtection(
        const std::uint64_t hwndValue,
        const bool enableProtection)
    {
        CaptureProtectionResult result;
        result.requestedProtection = enableProtection;
        result.requestedHwnd = hwndValue;
        result.appliedHwnd = hwndValue;
        result.appliedAffinity = enableProtection
            ? kDisplayAffinityExcludeFromCapture
            : kDisplayAffinityAllowCapture;

        HWND requestedWindowHandle = hwndFromValue(hwndValue);
        if (requestedWindowHandle == nullptr || ::IsWindow(requestedWindowHandle) == FALSE)
        {
            result.win32Error = ERROR_INVALID_WINDOW_HANDLE;
            result.detail = "invalid HWND";
            return result;
        }

        bool usedRootWindow = false;
        HWND targetWindowHandle = queryRootWindow(requestedWindowHandle, usedRootWindow);
        if (targetWindowHandle == nullptr || ::IsWindow(targetWindowHandle) == FALSE)
        {
            result.win32Error = ERROR_INVALID_WINDOW_HANDLE;
            result.detail = "root HWND invalid";
            return result;
        }

        DWORD processId = 0;
        ::GetWindowThreadProcessId(targetWindowHandle, &processId);
        result.processId = static_cast<std::uint32_t>(processId);
        result.appliedHwnd = hwndToValue(targetWindowHandle);
        result.usedRootWindow = usedRootWindow;

        DWORD appliedAffinity = result.appliedAffinity;
        DWORD win32Error = 0;
        if (processId == ::GetCurrentProcessId())
        {
            result.success = applyAffinityDirect(
                targetWindowHandle,
                enableProtection,
                appliedAffinity,
                win32Error);
        }
        else
        {
            result.usedRemoteThread = true;
            result.success = applyAffinityRemote(
                processId,
                targetWindowHandle,
                enableProtection,
                appliedAffinity,
                win32Error,
                result.detail);
        }

        result.appliedAffinity = appliedAffinity;
        result.win32Error = win32Error;
        if (result.success)
        {
            result.detail = enableProtection
                ? "capture protection applied"
                : "capture protection removed";
        }
        else if (result.detail.empty())
        {
            result.detail = formatWin32Error("SetWindowDisplayAffinity", result.win32Error);
        }
        return result;
    }

    bool QueryWindowDisplayAffinity(
        const std::uint64_t hwndValue,
        std::uint32_t& affinityOut,
        std::uint32_t* const win32ErrorOut)
    {
        affinityOut = kDisplayAffinityAllowCapture;
        if (win32ErrorOut != nullptr)
        {
            *win32ErrorOut = 0;
        }

        HWND windowHandle = hwndFromValue(hwndValue);
        if (windowHandle == nullptr || ::IsWindow(windowHandle) == FALSE)
        {
            if (win32ErrorOut != nullptr)
            {
                *win32ErrorOut = ERROR_INVALID_WINDOW_HANDLE;
            }
            return false;
        }

        DWORD affinityValue = 0;
        if (::GetWindowDisplayAffinity(windowHandle, &affinityValue) == FALSE)
        {
            if (win32ErrorOut != nullptr)
            {
                *win32ErrorOut = ::GetLastError();
            }
            return false;
        }

        affinityOut = static_cast<std::uint32_t>(affinityValue);
        return true;
    }

    std::string DisplayAffinityName(const std::uint32_t affinityValue)
    {
        switch (affinityValue)
        {
        case kDisplayAffinityAllowCapture:
            return "WDA_NONE / 允许截图";
        case kDisplayAffinityMonitorOnly:
            return "WDA_MONITOR / 截图黑屏";
        case kDisplayAffinityExcludeFromCapture:
            return "WDA_EXCLUDEFROMCAPTURE / 从截图中隐藏";
        default:
            return "未知 DisplayAffinity";
        }
    }
}
