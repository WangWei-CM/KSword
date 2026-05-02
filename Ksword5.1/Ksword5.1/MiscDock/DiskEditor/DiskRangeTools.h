#pragma once

// ============================================================
// DiskRangeTools.h
// 作用：
// 1) 提供磁盘范围搜索、哈希、镜像导出/导入、文件差异和坏块读扫能力；
// 2) 所有函数均为同步执行，调用方应放入后台线程；
// 3) 写入类能力保留扇区对齐参数，由 UI 继续做二次确认。
// ============================================================

#include "DiskAdvancedModels.h"

#include <QCryptographicHash>
#include <QString>

#include <cstdint>

namespace ks::misc
{
    // DiskSearchPatternMode 作用：
    // - 描述搜索输入如何解释；
    // - HexBytes 支持 AA BB ?? 通配符，AsciiText 和 Utf16Text 用于文本搜索。
    enum class DiskSearchPatternMode : int
    {
        HexBytes = 0,
        AsciiText,
        Utf16Text
    };

    // DiskRangeTools 说明：
    // - 输入：物理磁盘路径、偏移、长度、文件路径或搜索模式；
    // - 处理逻辑：按块顺序读取/写入，避免一次性占用大内存；
    // - 返回行为：通过 DiskRangeTaskResult 返回摘要、明细、错误和结果集。
    class DiskRangeTools final
    {
    public:
        // searchRange：
        // - 在磁盘范围内搜索字节模式；
        // - maxResults 限制命中数量，避免 UI 被海量结果拖慢；
        // - 返回任务结果。
        static DiskRangeTaskResult searchRange(
            const QString& devicePath,
            std::uint64_t offsetBytes,
            std::uint64_t lengthBytes,
            const QString& patternText,
            DiskSearchPatternMode mode,
            int maxResults);

        // hashRange：
        // - 对磁盘范围计算哈希；
        // - algorithm 为 QCryptographicHash 支持的算法；
        // - 返回任务结果，digestBytes 保存原始摘要。
        static DiskRangeTaskResult hashRange(
            const QString& devicePath,
            std::uint64_t offsetBytes,
            std::uint64_t lengthBytes,
            QCryptographicHash::Algorithm algorithm);

        // exportRangeToFile：
        // - 把磁盘范围导出为镜像文件；
        // - filePath 为目标文件路径；
        // - 返回任务结果。
        static DiskRangeTaskResult exportRangeToFile(
            const QString& devicePath,
            std::uint64_t offsetBytes,
            std::uint64_t lengthBytes,
            const QString& filePath);

        // importFileToRange：
        // - 把文件内容写入磁盘指定偏移；
        // - requireSectorAligned=true 时要求偏移和文件长度按扇区对齐；
        // - 返回任务结果。
        static DiskRangeTaskResult importFileToRange(
            const QString& devicePath,
            std::uint64_t offsetBytes,
            const QString& filePath,
            std::uint32_t bytesPerSector,
            bool requireSectorAligned);

        // compareRangeWithFile：
        // - 把磁盘范围与文件逐块比较；
        // - maxDifferences 限制记录的差异数量；
        // - 返回任务结果。
        static DiskRangeTaskResult compareRangeWithFile(
            const QString& devicePath,
            std::uint64_t offsetBytes,
            std::uint64_t lengthBytes,
            const QString& filePath,
            int maxDifferences);

        // scanReadableBlocks：
        // - 以 blockBytes 为单位做快速读扫；
        // - 只检测读取是否成功和吞吐，不解释数据；
        // - 返回任务结果。
        static DiskRangeTaskResult scanReadableBlocks(
            const QString& devicePath,
            std::uint64_t offsetBytes,
            std::uint64_t lengthBytes,
            std::uint32_t blockBytes);
    };
}
