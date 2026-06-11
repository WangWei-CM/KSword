#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

void StartupDock::appendServiceEntries(std::vector<StartupEntry>* entryListOut)
{
    // 服务枚举后端已迁入 ks::startup：
    // - 输入 entryListOut：StartupDock 全量缓存；
    // - 处理逻辑：枚举自动启动 Win32 服务并转换为 UI 记录；
    // - 返回值：无，直接追加结果。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateServiceEntries());
}

void StartupDock::appendDriverEntries(std::vector<StartupEntry>* entryListOut)
{
    // 驱动服务枚举后端已迁入 ks::startup：
    // - 输入 entryListOut：StartupDock 全量缓存；
    // - 处理逻辑：枚举 boot/system/auto 驱动项并转换为 UI 记录；
    // - 返回值：无，直接追加结果。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateDriverEntries());
}
