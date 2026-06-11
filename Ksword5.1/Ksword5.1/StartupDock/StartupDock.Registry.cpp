#include "StartupDock.Internal.h"

using namespace startup_dock_detail;

void StartupDock::appendLogonEntries(std::vector<StartupEntry>* entryListOut)
{
    // StartupDock 只负责把非 UI 后端记录适配为 Qt 表格模型：
    // - 输入 entryListOut：UI 缓存追加目标；
    // - 处理逻辑：调用 ks::startup 登录项枚举，再统一转换字段；
    // - 返回值：无，转换结果直接追加到 entryListOut。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateLogonEntries());
}

void StartupDock::appendAdvancedRegistryEntries(std::vector<StartupEntry>* entryListOut)
{
    // 高级注册表启动项的实际枚举已迁入 ksword/startup：
    // - UI 层不再直接遍历 Run/Explorer/Winlogon/LSA/COM 等注册表位置；
    // - 这里只保留 Qt 字符串/图标渲染前的适配边界；
    // - 返回值：无，结果追加到 entryListOut。
    appendBackendStartupEntries(
        entryListOut,
        ks::startup::EnumerateAdvancedRegistryEntries());
}
