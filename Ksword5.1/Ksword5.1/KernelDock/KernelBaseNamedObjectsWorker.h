#pragma once

// ============================================================
// KernelBaseNamedObjectsWorker.h
// 作用：
// 1) 定义 BaseNamedObjects 专项视图的 R3 采集结果模型；
// 2) 暴露只读快照采集入口；
// 3) 不依赖 KswordARK 驱动、不新增 IOCTL、不执行句柄枚举。
// ============================================================

#include "../Framework.h"

#include <vector>

// KernelBaseNamedObjectEntry 说明：
// - 输入来源：NtOpenDirectoryObject + NtQueryDirectoryObject 枚举出的对象目录项；
// - 处理逻辑：worker 按 scope/session/type/category 补充展示字段；
// - 返回行为：纯数据结构，由 UI 表格消费。
struct KernelBaseNamedObjectEntry
{
    QString scopeText;          // scopeText：Global / Current Session / Session N。
    QString directoryPathText;  // directoryPathText：被枚举的对象目录。
    QString objectNameText;     // objectNameText：目录项名称。
    QString objectTypeText;     // objectTypeText：原始 NT 对象类型名。
    QString typeCategoryText;   // typeCategoryText：Event/Mutant/Semaphore/Section/Timer/Job/Directory/SymbolicLink/Other。
    QString fullPathText;       // fullPathText：directoryPath + objectName。
    QString symbolicTargetText; // symbolicTargetText：SymbolicLink 目标；非符号链接为空。
    QString statusText;         // statusText：目录枚举/符号链接解析状态。
    unsigned long sessionId = 0; // sessionId：会话 ID；Global 用 ULONG_MAX 表示非会话目录。
    bool hasSessionId = false;  // hasSessionId：sessionId 是否有效。
    bool canEnumerate = false;  // canEnumerate：Directory 类型是否可继续枚举。
};

// runBaseNamedObjectsSnapshotTask：
// - 输入 rowsOut：输出列表，函数开始时清空；
// - 输入 errorTextOut：致命错误文本，成功时清空；
// - 处理逻辑：枚举 \BaseNamedObjects、当前 Session、以及 \Sessions 下可发现数字 Session；
// - 返回：true 表示采集流程可用，false 表示 Nt API 装载等致命错误。
bool runBaseNamedObjectsSnapshotTask(
    std::vector<KernelBaseNamedObjectEntry>& rowsOut,
    QString& errorTextOut);

