#pragma once

// ============================================================
// hook/HookEngine.h
// 作用：
// 1) 提供 x64 Inline Hook 的基础安装与卸载能力；
// 2) 负责为目标函数构造 trampoline 并保存原始字节；
// 3) 供 HookTargets 层批量安装 WinAPI detour 时复用。
// ============================================================

#include "pch.h"

namespace apimon
{
    enum class InlineHookInstallResult
    {
        Installed,
        RetryableFailure,
        PermanentFailure
    };

    struct InlineHookRecord
    {
        void* targetAddress = nullptr;                       // targetAddress：最终被打补丁的目标地址。
        void* detourAddress = nullptr;                       // detourAddress：Detour 函数地址。
        void* trampolineAddress = nullptr;                   // trampolineAddress：原始指令跳板地址。
        std::array<unsigned char, 32> originalBytes = {};   // originalBytes：被覆盖前的原始字节。
        std::size_t patchSize = 0;                          // patchSize：当前覆盖的前导字节数。
        bool installed = false;                             // installed：当前 Hook 是否已生效。
        bool permanentlyDisabled = false;                   // permanentlyDisabled：是否因不可安全安装而永久禁用。
    };

    InlineHookInstallResult InstallInlineHook(
        const wchar_t* moduleName,
        const char* procName,
        void* detourAddress,
        InlineHookRecord* hookOut,
        void** originalOut,
        std::wstring* errorTextOut);

    void UninstallInlineHook(InlineHookRecord* hookValue);
}
