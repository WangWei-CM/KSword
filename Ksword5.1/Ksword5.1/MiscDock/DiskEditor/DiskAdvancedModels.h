#pragma once

// ============================================================
// DiskAdvancedModels.h
// 作用：
// 1) 定义磁盘高级分析、卷映射、健康信息、搜索和镜像任务的共享模型；
// 2) 让解析器、Win32 能力探测和 UI 表格复用同一份轻量结构；
// 3) 所有字段保持 Qt 基础类型，避免把 Win32 结构泄漏到界面层。
// ============================================================

#include "DiskEditorModels.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace ks::misc
{
    // DiskStructureSeverity 作用：
    // - 标记结构解析、健康探测和工具任务的严重度；
    // - UI 可据此对行进行着色或排序。
    enum class DiskStructureSeverity : int
    {
        Info = 0,
        Warning,
        Error
    };

    // DiskStructureField 作用：
    // - 表示一个可定位的磁盘结构字段；
    // - offsetBytes/sizeBytes 使用物理磁盘绝对偏移，便于双击跳转 HEX。
    struct DiskStructureField
    {
        QString group;                                // group：MBR、GPT Header、Boot Sector 等分组。
        QString name;                                 // name：字段名称。
        QString value;                                // value：字段解析值。
        QString detail;                               // detail：校验说明或风险提示。
        std::uint64_t offsetBytes = 0;                // offsetBytes：字段起始绝对偏移。
        std::uint32_t sizeBytes = 0;                  // sizeBytes：字段长度，未知为 0。
        DiskStructureSeverity severity = DiskStructureSeverity::Info; // severity：字段严重度。
    };

    // DiskVolumeInfo 作用：
    // - 描述 Windows 卷与物理磁盘区间的对应关系；
    // - 一个卷可有多个 extent，多 extent 卷会生成多条记录。
    struct DiskVolumeInfo
    {
        QString volumeName;                           // volumeName：\\?\Volume{GUID}\ 形式。
        QString mountPoints;                          // mountPoints：盘符或挂载点列表。
        QString devicePath;                           // devicePath：\Device\HarddiskVolumeX 内核设备路径。
        QString fileSystem;                           // fileSystem：NTFS/FAT32/exFAT/ReFS 等。
        QString label;                                // label：卷标。
        int diskNumber = -1;                          // diskNumber：PhysicalDriveN 中的 N。
        std::uint64_t offsetBytes = 0;                // offsetBytes：该 extent 在物理磁盘上的起始偏移。
        std::uint64_t lengthBytes = 0;                // lengthBytes：该 extent 长度。
    };

    // DiskHealthItem 作用：
    // - 保存一次设备能力或健康状态查询结果；
    // - 失败项也作为 Warning/Error 行展示，避免用户误以为没有检测。
    struct DiskHealthItem
    {
        QString category;                             // category：能力、缓存、SMART、热插拔等类别。
        QString name;                                 // name：项目名称。
        QString value;                                // value：检测值。
        QString detail;                               // detail：补充说明或 Win32 错误码。
        DiskStructureSeverity severity = DiskStructureSeverity::Info; // severity：展示严重度。
    };

    // DiskStructureReport 作用：
    // - 聚合高级分析页一次刷新得到的全部内容；
    // - fields、volumes、healthItems 分别对应三个表格。
    struct DiskStructureReport
    {
        std::vector<DiskStructureField> fields;       // fields：MBR/GPT/启动扇区字段。
        std::vector<DiskVolumeInfo> volumes;          // volumes：卷映射结果。
        std::vector<DiskHealthItem> healthItems;      // healthItems：设备能力和健康探测结果。
        QStringList warnings;                         // warnings：整体级别告警。
    };

    // DiskSearchResult 作用：
    // - 描述一次字节搜索命中的绝对偏移和附近预览；
    // - preview 保存少量字节，UI 以 HEX/ASCII 混合方式展示。
    struct DiskSearchResult
    {
        std::uint64_t offsetBytes = 0;                // offsetBytes：命中的绝对字节偏移。
        QByteArray preview;                           // preview：命中附近的字节窗口。
    };

    // DiskRangeTaskResult 作用：
    // - 统一表示搜索、哈希、导出、导入和读扫任务的完成结果；
    // - 让后台线程只回投一个结构，减少 UI 状态分支。
    struct DiskRangeTaskResult
    {
        bool success = false;                         // success：任务是否成功完成。
        QString summary;                              // summary：面向状态栏的简短摘要。
        QString errorText;                            // errorText：失败原因，成功时为空。
        QStringList detailLines;                      // detailLines：日志或诊断明细。
        std::vector<DiskSearchResult> searchResults;  // searchResults：搜索命中列表。
        QByteArray digestBytes;                       // digestBytes：哈希任务的原始摘要。
        double mibPerSecond = 0.0;                    // mibPerSecond：读扫/导出等吞吐估算。
        std::uint64_t bytesProcessed = 0;             // bytesProcessed：实际处理字节数。
        int failureCount = 0;                         // failureCount：读扫失败块数量。
    };
}
