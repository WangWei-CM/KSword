#pragma once

// ============================================================
// KernelDeviceDriverObjectsWorker.h
// 作用说明：
// 1) 提供“设备与驱动”专项视图的只读后台枚举任务；
// 2) 仅使用 R3 对象管理器 API 枚举 \Device、\Driver、\FileSystem、
//    \FileSystem\Filters 等目录；
// 3) 仅解析符号链接目标，不执行任何策略写入或内核交互。
// ============================================================

#include "../Framework.h"

#include <vector> // std::vector：承载枚举结果。

// ============================================================
// KernelDeviceDriverObjectEntry
// 作用：
// - 表示“设备与驱动”专项视图中的一行只读结果；
// - 该结构专门用于 UI 展示与 TSV 导出，不包含任何可写操作字段。
// ============================================================
struct KernelDeviceDriverObjectEntry
{
    QString directoryPathText;   // directoryPathText：当前枚举来源目录，如 \Device。
    QString objectNameText;      // objectNameText：对象名称。
    QString objectTypeText;      // objectTypeText：对象类型，如 Device / Driver / Directory / SymbolicLink。
    QString fullPathText;        // fullPathText：对象完整路径。
    QString targetPathText;      // targetPathText：符号链接目标；非链接对象保持为空。
    QString statusText;          // statusText：状态文本，用于说明枚举或解析结果。
    QString capabilityHintText;  // capabilityHintText：中文能力提示，说明后续可做什么。
    QString detailText;          // detailText：补充说明文本，便于后续接入详情面板。
    long statusCode = 0;         // statusCode：原始 NTSTATUS，便于后续诊断。
    bool querySucceeded = false; // querySucceeded：该行是否成功获得对象信息。
    bool isDirectory = false;    // isDirectory：对象是否为目录。
    bool isSymbolicLink = false; // isSymbolicLink：对象是否为符号链接。
    bool isScopeEntry = false;   // isScopeEntry：是否为目录范围说明行（非实际子对象）。
};

// runKernelDeviceDriverObjectsSnapshotTask：
// - 输入：rowsOut 用于接收全部枚举结果；errorTextOut 用于返回致命错误文本；
// - 处理：加载 ntdll 中的对象管理器 API，依次枚举四个目标目录并解析符号链接；
// - 返回：true 表示任务成功执行到结束；false 表示 API 装载等致命错误。
bool runKernelDeviceDriverObjectsSnapshotTask(
    std::vector<KernelDeviceDriverObjectEntry>& rowsOut,
    QString& errorTextOut);

