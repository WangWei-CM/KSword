#pragma once

// ============================================================
// KernelObjectDirectoryDeepWorker.h
// 作用说明：
// 1) 提供 R3 Object Manager Directory 递归枚举接口；
// 2) 使用 NtOpenDirectoryObject + NtQueryDirectoryObject；
// 3) 仅对 TypeName == Directory 的对象继续递归，其它对象作为叶子返回。
// ============================================================

#include <QString>

#include <cstddef>
#include <vector>

// KernelObjectDirectoryDeepEntry
// - 作用：保存目录递归枚举的一条对象记录。
// - 字段：覆盖根路径、目录路径、对象名、类型、完整路径、深度、状态和是否目录。
struct KernelObjectDirectoryDeepEntry
{
    QString rootPath;       // rootPath：本次递归任务的根路径，例如 "\" 或 "\Device"。
    QString directoryPath;  // directoryPath：当前对象所在目录路径。
    QString objectName;     // objectName：当前对象名；根目录打开失败时可为根路径叶子名。
    QString objectType;     // objectType：NtQueryDirectoryObject 返回的 TypeName。
    QString fullPath;       // fullPath：directoryPath + objectName 拼接得到的完整对象路径。
    int depth = 0;          // depth：相对 rootPath 的递归深度，根目录子项为 0。
    QString statusText;     // statusText：枚举状态、失败 NTSTATUS 或安全上限说明。
    bool querySucceeded = false; // querySucceeded：该记录对应的查询/打开动作是否成功。
    bool isDirectory = false;    // isDirectory：objectType 是否为 Directory。
};

// KernelObjectDirectoryDeepOptions
// - 作用：控制递归枚举的安全边界，防止异常对象树导致 UI 长时间无响应。
// - 调用方：Widget 根据输入框生成，Worker 负责再做范围修正。
struct KernelObjectDirectoryDeepOptions
{
    QString rootPath = QStringLiteral("\\"); // rootPath：起始 Object Manager 目录。
    int maxDepth = 4;                        // maxDepth：Directory 继续下钻的最大深度。
    std::size_t maxEntriesPerDirectory = 4096; // maxEntriesPerDirectory：单目录最多读取条目。
    std::size_t maxTotalEntries = 50000;       // maxTotalEntries：总输出记录上限。
};

// KernelObjectDirectoryDeepResult
// - 作用：聚合递归枚举结果和整体诊断。
// - success=false 表示 Nt API 装载失败等致命错误；访问单个目录失败仍以 rows 记录返回。
struct KernelObjectDirectoryDeepResult
{
    bool success = false;              // success：任务是否成功启动并完成扫描流程。
    QString errorText;                 // errorText：致命错误文本；成功时为空。
    QString normalizedRootPath;        // normalizedRootPath：Worker 规范化后的根路径。
    std::size_t visitedDirectoryCount = 0; // visitedDirectoryCount：成功尝试枚举的目录数。
    std::size_t failedDirectoryCount = 0;  // failedDirectoryCount：打开/查询失败目录数。
    bool totalLimitReached = false;        // totalLimitReached：是否触达总条目上限。
    bool depthLimitReached = false;        // depthLimitReached：是否遇到深度上限。
    bool perDirectoryLimitReached = false; // perDirectoryLimitReached：是否遇到单目录条目上限。
    std::vector<KernelObjectDirectoryDeepEntry> rows; // rows：递归枚举输出记录。
};

// runKernelObjectDirectoryDeepSnapshotTask：
// - 作用：按 options.rootPath 递归枚举 Object Manager Directory。
// - 输入 options：根路径与安全上限；函数会对空路径/越界上限做保守修正。
// - 处理逻辑：打开目录 -> 查询条目 -> 记录所有对象 -> 对 Directory 子项继续递归。
// - 返回：KernelObjectDirectoryDeepResult；非致命访问失败写入 rows，不抛异常。
KernelObjectDirectoryDeepResult runKernelObjectDirectoryDeepSnapshotTask(
    const KernelObjectDirectoryDeepOptions& options);
