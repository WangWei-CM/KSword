#pragma once

// ============================================================
// hook/HookTargets.h
// 作用：
// 1) 实现具体 WinAPI Detour 函数；
// 2) 按当前配置安装/卸载各分类 Hook；
// 3) 在 DLL 运行期间为新加载模块补装尚未成功的 Hook。
// ============================================================

#include "pch.h"

namespace apimon
{
    bool InstallConfiguredHooks(std::wstring* errorTextOut);
    void UninstallConfiguredHooks();
    void RetryPendingHooks();
}
