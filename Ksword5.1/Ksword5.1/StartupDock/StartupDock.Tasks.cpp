#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

void StartupDock::appendTaskEntries(std::vector<StartupEntry>* entryListOut)
{
    // 计划任务枚举后端已迁入 ks::startup：
    // - 后端内部负责 PowerShell 调用和 JSON 解析；
    // - UI 层只接收 std::string 记录并转换成 QString；
    // - 返回值：无，直接追加结果。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateTaskEntries());
}
