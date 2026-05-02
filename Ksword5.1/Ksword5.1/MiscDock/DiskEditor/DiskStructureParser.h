#pragma once

// ============================================================
// DiskStructureParser.h
// 作用：
// 1) 解析磁盘前部结构，包括 MBR、GPT Header、GPT Entry 和常见启动扇区；
// 2) 提供卷映射与设备健康信息采集入口；
// 3) 只做只读分析，不执行任何写盘动作。
// ============================================================

#include "DiskAdvancedModels.h"
#include "DiskEditorModels.h"

#include <QString>

namespace ks::misc
{
    // DiskStructureParser 说明：
    // - 输入：DiskDeviceInfo 和可选的前部扇区字节；
    // - 处理逻辑：按公开磁盘格式解析结构字段并做轻量一致性校验；
    // - 返回行为：通过 DiskStructureReport 返回字段、卷映射、健康探测和告警。
    class DiskStructureParser final
    {
    public:
        // buildReport：
        // - disk 为当前磁盘快照；
        // - leadingBytes 为从磁盘 0 偏移读取的前部字节，建议至少 1 MiB；
        // - errorTextOut 返回致命失败说明；
        // - 返回完整结构报告。
        static DiskStructureReport buildReport(
            const DiskDeviceInfo& disk,
            const QByteArray& leadingBytes,
            QString& errorTextOut);

        // collectVolumeMapping：
        // - 枚举 Windows 卷并匹配到指定物理磁盘；
        // - diskIndex 为 PhysicalDriveN 的 N；
        // - errorTextOut 返回非致命诊断；
        // - 返回命中的卷 extent 列表。
        static std::vector<DiskVolumeInfo> collectVolumeMapping(
            int diskIndex,
            QString& errorTextOut);

        // collectHealthItems：
        // - 查询设备能力、缓存、TRIM、SMART/NVMe 可用性等只读状态；
        // - disk 为当前磁盘快照；
        // - errorTextOut 返回非致命诊断；
        // - 返回健康与能力条目。
        static std::vector<DiskHealthItem> collectHealthItems(
            const DiskDeviceInfo& disk,
            QString& errorTextOut);
    };
}
