#pragma once

// ============================================================
// DiskEditorModels.h
// 作用：
// 1) 定义杂项 Dock“磁盘编辑”页使用的磁盘、分区与展示模型；
// 2) 让 Win32 采集层、柱形图控件和页面 UI 共享同一份轻量数据契约；
// 3) 避免 UI 文件直接暴露 Windows 结构体，降低后续接入驱动能力时的耦合。
// ============================================================

#include <QColor>
#include <QString>

#include <cstdint>
#include <vector>

namespace ks::misc
{
    // DiskPartitionStyle 作用：
    // - 统一描述磁盘或分区表样式；
    // - UI 层只认该枚举，不直接依赖 Win32 PARTITION_STYLE。
    enum class DiskPartitionStyle : int
    {
        Unknown = 0,
        Raw,
        Mbr,
        Gpt
    };

    // DiskPartitionKind 作用：
    // - 为柱形磁盘图提供语义颜色分类；
    // - 不是精确文件系统类型，只服务于“第一眼识别”。
    enum class DiskPartitionKind : int
    {
        Unknown = 0,
        BasicData,
        System,
        Reserved,
        Recovery,
        Linux,
        Unallocated
    };

    // DiskPartitionInfo 作用：
    // - 描述物理磁盘上的一个分区；
    // - offsetBytes/lengthBytes 均为物理磁盘绝对字节偏移。
    struct DiskPartitionInfo
    {
        int tableIndex = -1;                         // tableIndex：UI 表格与柱形图使用的零基索引。
        int partitionNumber = 0;                     // partitionNumber：Windows 分区号，未分配区域为 0。
        DiskPartitionStyle style = DiskPartitionStyle::Unknown; // style：分区样式来源。
        DiskPartitionKind kind = DiskPartitionKind::Unknown;    // kind：展示语义分类。
        QString name;                                // name：GPT 名称或 UI 生成名称。
        QString typeText;                            // typeText：MBR 类型或 GPT 类型说明。
        QString uniqueIdText;                        // uniqueIdText：GPT PartitionId 或 MBR 签名相关文本。
        QString volumeHint;                          // volumeHint：命中的盘符/卷挂载提示，可能为空。
        QString flagsText;                           // flagsText：启动、隐藏、属性等简短标记。
        std::uint64_t offsetBytes = 0;               // offsetBytes：分区起始字节偏移。
        std::uint64_t lengthBytes = 0;               // lengthBytes：分区长度字节数。
        bool bootIndicator = false;                  // bootIndicator：MBR 活动分区标记。
        bool recognized = false;                     // recognized：Windows 是否识别该分区。
        QColor color;                                // color：柱形图填充色。
    };

    // DiskDeviceInfo 作用：
    // - 描述一个 PhysicalDrive 设备及其分区表快照；
    // - 枚举失败时也可保留 openErrorText 让 UI 给出诊断。
    struct DiskDeviceInfo
    {
        int diskIndex = -1;                          // diskIndex：PhysicalDriveN 中的 N。
        QString devicePath;                          // devicePath：形如 \\.\PhysicalDrive0。
        QString displayName;                         // displayName：组合后的友好名称。
        QString vendor;                              // vendor：存储设备厂商。
        QString model;                               // model：产品型号。
        QString serial;                              // serial：序列号，驱动可能返回空。
        QString busType;                             // busType：SATA/NVMe/USB 等总线提示。
        QString mediaType;                           // mediaType：固定磁盘/可移动磁盘等。
        QString openErrorText;                       // openErrorText：打开失败诊断。
        DiskPartitionStyle partitionStyle = DiskPartitionStyle::Unknown; // partitionStyle：整盘分区表类型。
        std::uint64_t sizeBytes = 0;                 // sizeBytes：整盘总容量。
        std::uint32_t bytesPerSector = 512;          // bytesPerSector：逻辑扇区大小。
        std::uint32_t physicalBytesPerSector = 0;    // physicalBytesPerSector：物理扇区大小，未知为 0。
        bool canRead = false;                        // canRead：枚举阶段是否成功以读权限打开。
        bool removable = false;                      // removable：设备是否报告为可移动介质。
        std::vector<DiskPartitionInfo> partitions;   // partitions：有效分区列表，不包含空槽。
    };
}
