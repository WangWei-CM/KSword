#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

void StartupDock::appendWmiEntries(std::vector<StartupEntry>* entryListOut)
{
    // WMI persistence 查询和 JSON 解析已迁入 ks::startup：
    // - 后端内部封装 PowerShell/WMI 输出为 std::vector<StartupEntry>；
    // - UI 层只负责展示、筛选和右键菜单；
    // - 返回值：无，直接追加结果。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateWmiEntries());
}
