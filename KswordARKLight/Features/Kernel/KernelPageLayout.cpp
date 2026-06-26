#include "KernelPageLayout.h"

namespace Ksword::Features::Kernel {
namespace {



const std::vector<TopLevelTabSpec> kOriginalTopLevelTabs = {
    { L"对象命名空间", KernelFeatureId::ObjectNamespaceOverview },
    { L"原子表遍历", KernelFeatureId::AtomTable },
    { L"历史NtQuery", KernelFeatureId::NtQueryLegacy },
    { L"SSDT遍历", KernelFeatureId::Ssdt },
    { L"SSSDT解析", KernelFeatureId::ShadowSsdt },
    { L"Inline Hook 检测 & 摘除", KernelFeatureId::InlineHook },
    { L"IAT/EAT 钩子检测", KernelFeatureId::IatEatHook },
    { L"驱动回调", KernelFeatureId::CallbackIntercept },
    { L"回调遍历", KernelFeatureId::CallbackEnumeration },
    { L"内核可执行内存", KernelFeatureId::KernelExecutableMemory },
    { L"CrossView", KernelFeatureId::ProcessCrossView },
    { L"内核修改审计", KernelFeatureId::MutationAudit },
    { L"键盘", KernelFeatureId::KeyboardHotkeys },
};

const std::vector<ObjectNamespaceTabSpec> kObjectNamespaceTabs = {
    { L"总览", KernelFeatureId::ObjectNamespaceOverview },
    { L"目录递归", KernelFeatureId::ObjectDirectoryRecursive },
    { L"命名管道", KernelFeatureId::NamedPipe },
    { L"BaseNamedObjects", KernelFeatureId::BaseNamedObjects },
    { L"符号链接", KernelFeatureId::SymbolicLink },
    { L"设备与驱动", KernelFeatureId::DeviceDriverObjects },
    { L"对象类型", KernelFeatureId::ObjectTypeMatrix },
    { L"通信端点", KernelFeatureId::CommunicationEndpoint },
};

const std::vector<ObjectNamespaceTabSpec> kCrossViewTabs = {
    { L"进程矩阵", KernelFeatureId::ProcessCrossView },
    { L"线程矩阵", KernelFeatureId::ThreadCrossView },
};

const std::vector<ObjectNamespaceTabSpec> kKeyboardTabs = {
    { L"热键", KernelFeatureId::KeyboardHotkeys },
    { L"钩子", KernelFeatureId::KeyboardHooks },
};

const std::vector<ObjectNamespaceTabSpec> kNoSecondaryTabs = {};

} // namespace

const std::vector<TopLevelTabSpec>& OriginalTopLevelTabs() {
    return kOriginalTopLevelTabs;
}

const std::vector<ObjectNamespaceTabSpec>& ObjectNamespaceTabs() {
    return kObjectNamespaceTabs;
}

const std::vector<ObjectNamespaceTabSpec>& SecondaryTabsForPrimary(const KernelFeatureId primaryFeatureId) {
    // SecondaryTabsForPrimary centralizes original grouped-page routing. Input
    // is the primary feature id stored by the outer tab; return value is a
    // static vector whose feature ids are resolved against the live catalog by
    // KernelPage before insertion.
    switch (primaryFeatureId) {
    case KernelFeatureId::ObjectNamespaceOverview:
        return kObjectNamespaceTabs;
    case KernelFeatureId::ProcessCrossView:
        return kCrossViewTabs;
    case KernelFeatureId::KeyboardHotkeys:
        return kKeyboardTabs;
    default:
        return kNoSecondaryTabs;
    }
}

KernelPageLayoutKind LayoutKindForFeature(const KernelFeatureId featureId) {
    switch (featureId) {
    case KernelFeatureId::ObjectNamespaceOverview:
        return KernelPageLayoutKind::TreeWithPropertyTable;
    case KernelFeatureId::ObjectDirectoryRecursive:
        return KernelPageLayoutKind::Tree;
    case KernelFeatureId::NamedPipe:
        return KernelPageLayoutKind::TableWithDetail;
    case KernelFeatureId::BaseNamedObjects:
    case KernelFeatureId::SymbolicLink:
    case KernelFeatureId::DeviceDriverObjects:
    case KernelFeatureId::ObjectTypeMatrix:
    case KernelFeatureId::CommunicationEndpoint:
    case KernelFeatureId::AtomTable:
    case KernelFeatureId::NtQueryLegacy:
        return KernelPageLayoutKind::Table;
    case KernelFeatureId::Ssdt:
    case KernelFeatureId::ShadowSsdt:
    case KernelFeatureId::InlineHook:
    case KernelFeatureId::IatEatHook:
    case KernelFeatureId::CallbackEnumeration:
    case KernelFeatureId::KernelExecutableMemory:
    case KernelFeatureId::KernelMemoryEvidence:
    case KernelFeatureId::ProcessCrossView:
    case KernelFeatureId::ThreadCrossView:
    case KernelFeatureId::DriverIntegrity:
    case KernelFeatureId::KernelCpuIntegrity:
    case KernelFeatureId::CpuHardwareSnapshot:
    case KernelFeatureId::PhysicalMemoryLayout:
    case KernelFeatureId::MutationAudit:
    case KernelFeatureId::KeyboardHotkeys:
    case KernelFeatureId::KeyboardHooks:
    case KernelFeatureId::DynDataCapabilities:
    case KernelFeatureId::MinifilterBypassPids:
        return KernelPageLayoutKind::TableWithDetail;
    case KernelFeatureId::DynData:
    case KernelFeatureId::DriverStatus:
        return KernelPageLayoutKind::DualTable;
    case KernelFeatureId::CallbackIntercept:
        return KernelPageLayoutKind::RuntimePanel;
    default:
        return KernelPageLayoutKind::Table;
    }
}

std::wstring LayoutKindText(const KernelPageLayoutKind kind) {
    switch (kind) {
    case KernelPageLayoutKind::Table: return L"表格";
    case KernelPageLayoutKind::Tree: return L"树表";
    case KernelPageLayoutKind::TreeWithPropertyTable: return L"树 + 属性表";
    case KernelPageLayoutKind::TableWithDetail: return L"表格 + 详情";
    case KernelPageLayoutKind::DualTable: return L"双表分割";
    case KernelPageLayoutKind::RuntimePanel: return L"运行态控制面板";
    default: return L"表格";
    }
}

// CanonicalColumnNames returns the original KswordARK table headers for each
// KernelDock page. Inputs are a feature id; output is the fixed Win32 ListView
// schema that should appear before any diagnostic extra columns.
std::vector<std::wstring> CanonicalColumnNames(const KernelFeatureId featureId) {
    switch (featureId) {
    case KernelFeatureId::ObjectNamespaceOverview:
        return { L"名称", L"类型", L"路径/说明", L"状态", L"符号链接目标" };
    case KernelFeatureId::ObjectDirectoryRecursive:
        return { L"名称", L"类型", L"完整路径", L"深度", L"状态" };
    case KernelFeatureId::NamedPipe:
        return { L"Pipe Name", L"NT Path", L"Attributes", L"LastWriteTime", L"Status", L"Source" };
    case KernelFeatureId::BaseNamedObjects:
        return { L"scope", L"directoryPath", L"objectName", L"objectType", L"fullPath", L"symbolicTarget", L"statusText" };
    case KernelFeatureId::SymbolicLink:
        return { L"sourceDirectory", L"linkName", L"fullPath", L"targetPath", L"dosCandidate", L"statusText" };
    case KernelFeatureId::DeviceDriverObjects:
        return { L"目录路径", L"对象名称", L"对象类型", L"完整路径", L"目标路径", L"状态", L"能力提示" };
    case KernelFeatureId::ObjectTypeMatrix:
        return { L"类型编号", L"类型名", L"对象数", L"句柄数", L"访问掩码", L"枚举策略" };
    case KernelFeatureId::CommunicationEndpoint:
        return { L"来源目录", L"名称", L"类型", L"完整路径", L"状态" };
    case KernelFeatureId::AtomTable:
        return { L"Atom值", L"十六进制", L"名称", L"来源", L"状态" };
    case KernelFeatureId::NtQueryLegacy:
        return { L"类别", L"函数", L"查询项", L"状态", L"摘要" };
    case KernelFeatureId::Ssdt:
        return { L"索引", L"服务名", L"Zw导出地址", L"表项地址", L"模块", L"状态" };
    case KernelFeatureId::ShadowSsdt:
        return { L"索引", L"服务名", L"Stub地址", L"服务例程", L"模块", L"状态" };
    case KernelFeatureId::InlineHook:
        return { L"模块", L"函数", L"函数地址", L"类型", L"目标地址", L"目标模块", L"状态", L"内存字节", L"磁盘字节", L"差异状态" };
    case KernelFeatureId::IatEatHook:
        return { L"类别", L"模块", L"导入模块", L"函数/序号", L"Thunk/EAT项", L"当前目标", L"期望目标", L"目标模块", L"状态" };
    case KernelFeatureId::DynData:
        return { L"字段", L"偏移", L"状态", L"来源", L"功能", L"Capability" };
    case KernelFeatureId::DriverStatus:
        return { L"功能", L"状态", L"策略", L"所需DynData", L"已满足DynData", L"依赖字段", L"原因" };
    case KernelFeatureId::CallbackEnumeration:
        return { L"类别", L"来源", L"可信状态", L"状态", L"移除策略", L"名称", L"回调/对象地址", L"模块", L"Altitude" };
    case KernelFeatureId::CallbackIntercept:
        return { L"Section", L"Type", L"Enabled", L"Mode", L"Rules", L"Pending", L"Status" };
    case KernelFeatureId::KernelExecutableMemory:
        return { L"VA", L"页数", L"页大小", L"权限", L"Owner", L"模块路径", L"风险标志" };
    case KernelFeatureId::KernelMemoryEvidence:
        return { L"VA", L"大小", L"类型", L"Owner", L"PTE权限", L"风险", L"text hash/diff", L"Detail" };
    case KernelFeatureId::ProcessCrossView:
    case KernelFeatureId::ThreadCrossView:
        return { L"ID", L"对象", L"进程", L"PublicWalk", L"Active/ThreadList", L"CID", L"异常", L"置信度", L"Detail" };
    case KernelFeatureId::DriverIntegrity:
        return { L"类别", L"对象", L"目标", L"Owner", L"CPU/Vector", L"风险", L"置信度", L"Detail" };
    case KernelFeatureId::KernelCpuIntegrity:
        return { L"类别", L"CPU/Vector", L"对象/寄存器", L"目标/入口", L"Owner模块", L"风险", L"置信度", L"Detail" };
    case KernelFeatureId::CpuHardwareSnapshot:
        return { L"项目", L"值", L"摘要", L"Features", L"Leaves", L"状态" };
    case KernelFeatureId::PhysicalMemoryLayout:
        return { L"范围", L"总物理内存", L"最高物理地址", L"最大连续Range", L"状态" };
    case KernelFeatureId::MutationAudit:
        return { L"Tx", L"Operation", L"Status", L"TargetKind", L"PID", L"Address", L"Bytes", L"Risk", L"Flags" };
    case KernelFeatureId::KeyboardHotkeys:
        return { L"对象", L"热键ID", L"热键", L"进程ID", L"线程ID", L"进程名", L"来源", L"VK/Mod", L"详情" };
    case KernelFeatureId::KeyboardHooks:
        return { L"对象", L"类型", L"范围", L"进程ID", L"线程ID", L"函数/偏移", L"模块", L"来源", L"Flags", L"详情" };
    case KernelFeatureId::DynDataCapabilities:
        return { L"Capability", L"状态", L"字段", L"原因" };
    case KernelFeatureId::MinifilterBypassPids:
        return { L"PID", L"进程", L"状态", L"来源" };
    default:
        return {};
    }
}

// ColumnAliases maps generic facade key names to the exact original table
// headers. Inputs are a feature id and a display column; output is a priority
// list of row keys that can populate that original column.
std::vector<std::wstring> ColumnAliases(const KernelFeatureId featureId, const std::wstring& columnName) {
    if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
        if (columnName == L"名称") return { L"Name", L"objectName", L"linkName" };
        if (columnName == L"类型") return { L"Type", L"objectType" };
        if (columnName == L"路径/说明") return { L"Path", L"fullPath", L"NtPath", L"Source", L"Detail" };
        if (columnName == L"状态") return { L"Status", L"statusText" };
        if (columnName == L"符号链接目标") return { L"Target", L"targetPath", L"symbolicTarget" };
    }
    if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
        if (columnName == L"名称") return { L"Name", L"objectName", L"linkName" };
        if (columnName == L"类型") return { L"Type", L"objectType" };
        if (columnName == L"完整路径") return { L"Path", L"fullPath", L"NtPath" };
        if (columnName == L"深度") return { L"Depth" };
        if (columnName == L"状态") return { L"Status", L"statusText" };
    }
    if (featureId == KernelFeatureId::NamedPipe) {
        if (columnName == L"Pipe Name") return { L"Pipe", L"Name", L"objectName" };
        if (columnName == L"NT Path") return { L"NtPath", L"Path", L"fullPath" };
        if (columnName == L"Attributes") return { L"Attributes" };
        if (columnName == L"LastWriteTime") return { L"LastWriteTime", L"LastWrite" };
        if (columnName == L"Status") return { L"Status", L"statusText" };
        if (columnName == L"Source") return { L"Directory", L"Source", L"Parent", L"sourceDirectory" };
    }
    if (featureId == KernelFeatureId::BaseNamedObjects) {
        if (columnName == L"scope") return { L"Source", L"Scope" };
        if (columnName == L"directoryPath") return { L"directoryPath", L"Parent", L"Directory", L"sourceDirectory" };
        if (columnName == L"objectName") return { L"objectName", L"Name" };
        if (columnName == L"objectType") return { L"objectType", L"Type" };
        if (columnName == L"fullPath") return { L"fullPath", L"Path", L"NtPath" };
        if (columnName == L"symbolicTarget") return { L"symbolicTarget", L"targetPath", L"Target" };
        if (columnName == L"statusText") return { L"statusText", L"Status" };
    }
    if (featureId == KernelFeatureId::SymbolicLink) {
        if (columnName == L"sourceDirectory") return { L"sourceDirectory", L"Source", L"Parent", L"Directory" };
        if (columnName == L"linkName") return { L"linkName", L"Name", L"objectName" };
        if (columnName == L"fullPath") return { L"fullPath", L"Path", L"NtPath" };
        if (columnName == L"targetPath") return { L"targetPath", L"symbolicTarget", L"Target" };
        if (columnName == L"dosCandidate") return { L"dosCandidate", L"DosCandidates", L"Win32Path" };
        if (columnName == L"statusText") return { L"statusText", L"Status" };
    }
    if (featureId == KernelFeatureId::DeviceDriverObjects) {
        if (columnName == L"目录路径") return { L"directoryPath", L"Parent", L"Directory", L"Source" };
        if (columnName == L"对象名称") return { L"objectName", L"Name", L"DriverName" };
        if (columnName == L"对象类型") return { L"objectType", L"Type" };
        if (columnName == L"完整路径") return { L"fullPath", L"Path", L"NtPath" };
        if (columnName == L"目标路径") return { L"targetPath", L"symbolicTarget", L"Target" };
        if (columnName == L"状态") return { L"statusText", L"Status" };
        if (columnName == L"能力提示") return { L"capabilityHintText", L"Capability", L"Detail" };
    }
    if (featureId == KernelFeatureId::ObjectTypeMatrix) {
        if (columnName == L"类型编号") return { L"TypeIndex", L"Index" };
        if (columnName == L"类型名") return { L"Type" };
        if (columnName == L"对象数") return { L"Objects" };
        if (columnName == L"句柄数") return { L"Handles" };
        if (columnName == L"访问掩码") return { L"ValidAccess" };
        if (columnName == L"枚举策略") return { L"Detail", L"Status" };
    }
    if (featureId == KernelFeatureId::CommunicationEndpoint) {
        if (columnName == L"来源目录") return { L"Source", L"Parent" };
        if (columnName == L"名称") return { L"Name" };
        if (columnName == L"类型") return { L"Type" };
        if (columnName == L"完整路径") return { L"Path" };
        if (columnName == L"状态") return { L"Status" };
    }
    if (featureId == KernelFeatureId::AtomTable) {
        if (columnName == L"Atom值") return { L"Id" };
        if (columnName == L"十六进制") return { L"Hex", L"Id" };
        if (columnName == L"名称") return { L"Name" };
        if (columnName == L"来源") return { L"Kind", L"Source" };
        if (columnName == L"状态") return { L"Status" };
    }
    if (featureId == KernelFeatureId::NtQueryLegacy) {
        if (columnName == L"类别") return { L"Category" };
        if (columnName == L"函数") return { L"Function" };
        if (columnName == L"查询项") return { L"Class", L"InfoClass" };
        if (columnName == L"状态") return { L"Status" };
        if (columnName == L"摘要") return { L"Detail" };
    }
    if (featureId == KernelFeatureId::Ssdt || featureId == KernelFeatureId::ShadowSsdt) {
        if (columnName == L"索引") return { L"Index" };
        if (columnName == L"服务名") return { L"Name", L"ServiceName" };
        if (columnName == L"Zw导出地址" || columnName == L"Stub地址") return { L"Zw", L"Stub", L"StubAddress" };
        if (columnName == L"表项地址" || columnName == L"服务地址" || columnName == L"服务例程") return { L"Service", L"ServiceAddress" };
        if (columnName == L"模块") return { L"Module" };
        if (columnName == L"状态") return { L"Status" };
    }
    if (featureId == KernelFeatureId::InlineHook) {
        if (columnName == L"模块") return { L"Module" };
        if (columnName == L"函数") return { L"Function" };
        if (columnName == L"函数地址") return { L"Address" };
        if (columnName == L"Hook类型" || columnName == L"类型") return { L"HookTypeText", L"TypeText", L"Type" };
        if (columnName == L"目标地址") return { L"Target" };
        if (columnName == L"目标模块") return { L"TargetModule" };
        if (columnName == L"状态") return { L"Status" };
        if (columnName == L"当前字节" || columnName == L"内存字节") return { L"CurrentBytes" };
        if (columnName == L"磁盘字节") return { L"DiskBytes" };
        if (columnName == L"差异" || columnName == L"差异状态") return { L"DiskDiff", L"Detail" };
    }
    if (featureId == KernelFeatureId::IatEatHook) {
        if (columnName == L"类别") return { L"ClassText", L"Class" };
        if (columnName == L"模块") return { L"Module" };
        if (columnName == L"导入模块") return { L"Import", L"ImportModule" };
        if (columnName == L"函数" || columnName == L"函数/序号") return { L"FunctionOrdinal", L"Function" };
        if (columnName == L"Thunk地址" || columnName == L"Thunk/EAT项") return { L"Thunk", L"ThunkAddress" };
        if (columnName == L"当前目标") return { L"Current", L"CurrentTarget" };
        if (columnName == L"期望目标") return { L"Expected", L"ExpectedTarget" };
        if (columnName == L"目标模块") return { L"TargetModule" };
        if (columnName == L"状态") return { L"Status" };
    }
    if (featureId == KernelFeatureId::DynData) {
        if (columnName == L"字段") return { L"Field", L"Name" };
        if (columnName == L"偏移") return { L"Offset" };
        if (columnName == L"状态") return { L"Status" };
        if (columnName == L"来源") return { L"Source" };
        if (columnName == L"功能") return { L"Feature" };
        if (columnName == L"Capability") return { L"Capability" };
    }
    if (featureId == KernelFeatureId::DriverStatus) {
        if (columnName == L"功能") return { L"Feature", L"Name", L"项目" };
        if (columnName == L"状态") return { L"Status", L"值" };
        if (columnName == L"策略") return { L"Policy" };
        if (columnName == L"所需DynData") return { L"RequiredDyn" };
        if (columnName == L"已满足DynData") return { L"PresentDyn" };
        if (columnName == L"依赖字段") return { L"Fields" };
        if (columnName == L"原因") return { L"Reason", L"Detail" };
    }
    if (featureId == KernelFeatureId::CallbackEnumeration) {
        if (columnName == L"类别") return { L"ClassText", L"Class" };
        if (columnName == L"来源") return { L"SourceText", L"Source" };
        if (columnName == L"可信状态") return { L"SourceTrust", L"TrustText" };
        if (columnName == L"状态") return { L"Status" };
        if (columnName == L"移除策略") return { L"RemovePolicy" };
        if (columnName == L"名称") return { L"Name" };
        if (columnName == L"回调/对象地址") return { L"Callback", L"Object" };
        if (columnName == L"模块") return { L"ModulePath", L"Module" };
        if (columnName == L"Altitude") return { L"Altitude" };
    }
    if (featureId == KernelFeatureId::CallbackIntercept) {
        if (columnName == L"Section") return { L"Section" };
        if (columnName == L"Type") return { L"Type", L"Name" };
        if (columnName == L"Enabled") return { L"Enabled" };
        if (columnName == L"Mode") return { L"Mode" };
        if (columnName == L"Rules") return { L"Rules", L"RuleCount" };
        if (columnName == L"Pending") return { L"Pending", L"PendingDecisions" };
        if (columnName == L"Status") return { L"Status" };
    }
    if (featureId == KernelFeatureId::KernelExecutableMemory) {
        if (columnName == L"VA") return { L"VA" };
        if (columnName == L"页数") return { L"Pages" };
        if (columnName == L"页大小") return { L"PageSize" };
        if (columnName == L"权限") return { L"PermText", L"Perm" };
        if (columnName == L"Owner") return { L"OwnerDisplay", L"OwnerKindText", L"Owner" };
        if (columnName == L"模块路径") return { L"ModulePath", L"Module" };
        if (columnName == L"风险标志") return { L"RiskText", L"Risk" };
    }
    if (featureId == KernelFeatureId::KernelMemoryEvidence) {
        if (columnName == L"VA") return { L"VA" };
        if (columnName == L"大小") return { L"SizeText", L"RegionSize" };
        if (columnName == L"类型") return { L"KindText", L"Kind" };
        if (columnName == L"Owner") return { L"OwnerDisplay", L"Owner", L"OwnerKindText" };
        if (columnName == L"PTE权限") return { L"PermText", L"Perm" };
        if (columnName == L"风险") return { L"RiskText", L"Risk" };
        if (columnName == L"text hash/diff") return { L"HashText", L"Hash" };
        if (columnName == L"Detail") return { L"Detail" };
    }
    if (featureId == KernelFeatureId::ProcessCrossView) {
        if (columnName == L"ID") return { L"ID", L"PID" };
        if (columnName == L"对象") return { L"对象", L"ProcessObject", L"Object" };
        if (columnName == L"进程") return { L"进程", L"Image" };
        if (columnName == L"PublicWalk") return { L"PublicWalk" };
        if (columnName == L"Active/ThreadList") return { L"Active/ThreadList" };
        if (columnName == L"CID") return { L"CID" };
        if (columnName == L"异常") return { L"异常", L"AnomalyText", L"Anomaly" };
        if (columnName == L"置信度") return { L"置信度", L"Confidence" };
        if (columnName == L"Detail") return { L"Detail" };
    }
    if (featureId == KernelFeatureId::ThreadCrossView) {
        if (columnName == L"ID") return { L"ID", L"TID" };
        if (columnName == L"对象") return { L"对象", L"ThreadObject", L"Object" };
        if (columnName == L"进程") return { L"进程", L"Image", L"PID" };
        if (columnName == L"PublicWalk") return { L"PublicWalk" };
        if (columnName == L"Active/ThreadList") return { L"Active/ThreadList" };
        if (columnName == L"CID") return { L"CID" };
        if (columnName == L"异常") return { L"异常", L"AnomalyText", L"Anomaly" };
        if (columnName == L"置信度") return { L"置信度", L"Confidence" };
        if (columnName == L"Detail") return { L"Detail" };
    }
    if (featureId == KernelFeatureId::DriverIntegrity) {
        if (columnName == L"类别") return { L"类别", L"ClassText", L"Class" };
        if (columnName == L"对象") return { L"对象", L"ObjectAddress", L"Object" };
        if (columnName == L"目标") return { L"目标", L"TargetAddress", L"Target" };
        if (columnName == L"Owner") return { L"Owner", L"OwnerModule" };
        if (columnName == L"CPU/Vector") return { L"CPU/Vector", L"CpuVector" };
        if (columnName == L"风险") return { L"风险", L"RiskText", L"Risk" };
        if (columnName == L"置信度") return { L"置信度", L"Confidence" };
        if (columnName == L"Detail") return { L"Detail" };
    }
    if (featureId == KernelFeatureId::KernelCpuIntegrity) {
        if (columnName == L"类别") return { L"ClassText", L"Class", L"类别" };
        if (columnName == L"CPU/Vector") return { L"CpuVector", L"CPU/Vector", L"CPU" };
        if (columnName == L"对象/寄存器") return { L"ObjectAddress", L"Object", L"对象" };
        if (columnName == L"目标/入口") return { L"TargetAddress", L"Target", L"目标" };
        if (columnName == L"Owner模块") return { L"Owner", L"OwnerModule" };
        if (columnName == L"风险") return { L"RiskText", L"Risk", L"风险" };
        if (columnName == L"置信度") return { L"Confidence", L"置信度" };
        if (columnName == L"Detail") return { L"Detail" };
    }
    if (featureId == KernelFeatureId::CpuHardwareSnapshot) {
        if (columnName == L"项目") return { L"项目" };
        if (columnName == L"值") return { L"值", L"Brand", L"Vendor" };
        if (columnName == L"摘要") return { L"摘要" };
        if (columnName == L"Features") return { L"Features", L"FeatureMask" };
        if (columnName == L"Leaves") return { L"Leaves", L"MaxBasicLeaf" };
        if (columnName == L"状态") return { L"状态", L"LastStatus", L"Unsupported" };
    }
    if (featureId == KernelFeatureId::PhysicalMemoryLayout) {
        if (columnName == L"范围") return { L"范围", L"Ranges" };
        if (columnName == L"总物理内存") return { L"总物理内存", L"TotalText", L"TotalBytes" };
        if (columnName == L"最高物理地址") return { L"最高物理地址", L"HighestAddress" };
        if (columnName == L"最大连续Range") return { L"最大连续Range", L"LargestRangeText", L"LargestRange" };
        if (columnName == L"状态") return { L"状态", L"LastStatus", L"Unsupported" };
    }
    if (featureId == KernelFeatureId::MutationAudit) {
        if (columnName == L"Tx") return { L"Tx", L"TransactionId" };
        if (columnName == L"Operation") return { L"Operation" };
        if (columnName == L"Status") return { L"Status" };
        if (columnName == L"TargetKind") return { L"TargetKind" };
        if (columnName == L"PID") return { L"PID" };
        if (columnName == L"Address") return { L"Address" };
        if (columnName == L"Bytes") return { L"Bytes" };
        if (columnName == L"Risk") return { L"RiskText", L"Risk" };
        if (columnName == L"Flags") return { L"FlagsText", L"Flags" };
    }
    if (featureId == KernelFeatureId::KeyboardHotkeys) {
        if (columnName == L"对象") return { L"对象", L"Object", L"HotkeyObject" };
        if (columnName == L"热键ID") return { L"热键ID", L"Id" };
        if (columnName == L"热键") return { L"热键", L"VK", L"Id" };
        if (columnName == L"进程ID") return { L"进程ID", L"PID" };
        if (columnName == L"线程ID") return { L"线程ID", L"TID" };
        if (columnName == L"进程名") return { L"进程名", L"Process" };
        if (columnName == L"来源") return { L"来源", L"SourceText", L"Source" };
        if (columnName == L"VK/Mod") return { L"VK/Mod", L"VkMod", L"VK" };
        if (columnName == L"详情") return { L"详情", L"Detail" };
    }
    if (featureId == KernelFeatureId::KeyboardHooks) {
        if (columnName == L"对象") return { L"对象", L"Object" };
        if (columnName == L"类型") return { L"类型", L"Hook类型", L"TypeText", L"Type" };
        if (columnName == L"范围") return { L"范围", L"ScopeText", L"Scope" };
        if (columnName == L"进程ID") return { L"进程ID", L"PID" };
        if (columnName == L"线程ID") return { L"线程ID", L"TID" };
        if (columnName == L"函数/偏移") return { L"函数/偏移", L"ProcedureDisplay", L"Procedure", L"ProcedureOffset" };
        if (columnName == L"模块") return { L"模块", L"ModuleBase", L"ModuleId" };
        if (columnName == L"来源") return { L"来源", L"SourceText", L"Source" };
        if (columnName == L"Flags") return { L"Flags" };
        if (columnName == L"详情") return { L"详情", L"Detail" };
    }
    if (featureId == KernelFeatureId::DynDataCapabilities) {
        if (columnName == L"Capability") return { L"Capability", L"CapabilityMask" };
        if (columnName == L"状态") return { L"状态", L"StatusFlags" };
        if (columnName == L"字段") return { L"字段", L"Fields", L"Field" };
        if (columnName == L"原因") return { L"原因", L"Reason", L"Detail" };
    }
    if (featureId == KernelFeatureId::MinifilterBypassPids) {
        if (columnName == L"PID") return { L"PID", L"PidCount" };
        if (columnName == L"进程") return { L"Process" };
        if (columnName == L"状态") return { L"状态", L"Status", L"Flags" };
        if (columnName == L"来源") return { L"来源", L"Index" };
    }
    return { columnName };
}


} // namespace Ksword::Features::Kernel
