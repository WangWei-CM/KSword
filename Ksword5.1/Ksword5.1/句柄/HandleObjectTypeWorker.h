#pragma once

// ============================================================
// HandleObjectTypeWorker.h
// 作用：
// - 提供“对象类型快照”后台采集能力；
// - 供句柄 Dock 的“对象类型”页与句柄列表类型映射共同复用；
// - 把内核对象类型解析逻辑集中到句柄模块，避免 UI 文件膨胀。
// ============================================================

#include "../Framework.h"

#include <QString>

#include <cstdint>
#include <unordered_map>
#include <vector>

// ============================================================
// HandleObjectTypeEntry
// 作用：
// - 表示一条对象类型快照记录；
// - 用于“对象类型页表格展示 + 句柄类型索引映射”。
// ============================================================
struct HandleObjectTypeEntry
{
    std::uint32_t typeIndex = 0;              // typeIndex：对象类型编号。
    QString typeNameText;                     // typeNameText：对象类型名（File/Process/Key 等）。
    std::uint64_t totalObjectCount = 0;       // totalObjectCount：对象总数。
    std::uint64_t totalHandleCount = 0;       // totalHandleCount：句柄总数。
    std::uint32_t validAccessMask = 0;        // validAccessMask：有效访问掩码。
    bool securityRequired = false;            // securityRequired：是否需要安全检查。
    bool maintainHandleCount = false;         // maintainHandleCount：是否维护句柄计数。
    std::uint32_t poolType = 0;               // poolType：池类型值。
    std::uint32_t defaultPagedPoolCharge = 0; // defaultPagedPoolCharge：默认分页池配额。
    std::uint32_t defaultNonPagedPoolCharge = 0; // defaultNonPagedPoolCharge：默认非分页池配额。
};

// runHandleObjectTypeSnapshotTask：
// - 作用：在后台线程中采集系统对象类型快照；
// - 调用：句柄 Dock 发起对象类型刷新时调用；
// - 传入 rowsOut：输出对象类型列表（函数内会先清空再写入）；
// - 传入 errorTextOut：失败说明文本（成功时清空）；
// - 返回：true=成功；false=失败。
bool runHandleObjectTypeSnapshotTask(
    std::vector<HandleObjectTypeEntry>& rowsOut,
    QString& errorTextOut);

// buildTypeNameMapFromObjectTypeRows：
// - 作用：把对象类型行列表转换为 typeIndex -> typeName 映射；
// - 调用：句柄列表枚举前可直接复用该映射，避免显示 Type#50；
// - 传入 rows：对象类型行列表；
// - 返回：类型索引映射表（UTF-8 字符串）。
std::unordered_map<std::uint16_t, std::string> buildTypeNameMapFromObjectTypeRows(
    const std::vector<HandleObjectTypeEntry>& rows);

