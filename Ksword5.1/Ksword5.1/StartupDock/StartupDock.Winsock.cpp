#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

void StartupDock::appendWinsockEntries(std::vector<StartupEntry>* entryListOut)
{
    // Winsock Provider/Catalog 注册表枚举已迁入 ks::startup：
    // - 后端返回 Catalog_Entries/Catalog_Entries64 的统一记录；
    // - UI 层继续把记录放入高级注册表树；
    // - 返回值：无，直接追加结果。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateWinsockEntries());
}
