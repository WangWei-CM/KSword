#pragma once

// ============================================================
// ManualFileSystemParser.h
// 作用：
// 1) 提供“手动解析文件系统”能力，支持 NTFS/FAT32；
// 2) 提供目录项枚举接口，供 FileDock 手动模式直接展示；
// 3) 提供 NTFS 误删项扫描与驻留数据恢复能力。
// ============================================================

#include "../Framework.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <cstdint>
#include <vector>

namespace ks::file
{
    // ManualFsType 作用：
    // - 表示解析器识别出的卷文件系统类型；
    // - Unknown 表示当前路径不支持手动解析。
    enum class ManualFsType : int
    {
        Unknown = 0,
        Ntfs = 1,
        Fat32 = 2
    };

    // ManualDirectoryEntry 作用：
    // - 表示手动解析得到的目录项；
    // - FileDock 会将该结构映射为表格每一行。
    struct ManualDirectoryEntry
    {
        QString name;                      // 条目名称（文件名或目录名）。
        QString absolutePath;              // 该条目的绝对路径。
        bool isDirectory = false;          // 是否目录。
        std::uint64_t sizeBytes = 0;       // 文件大小（目录通常为 0）。
        QDateTime modifiedTime;            // 最后修改时间（无效时为空）。
        QString typeText;                  // 类型提示文本（目录/文件扩展名）。
        std::uint64_t ntfsFileReference = 0; // NTFS 场景下的文件引用号（用于恢复功能）。
    };

    // NtfsDeletedFileEntry 作用：
    // - 表示扫描到的 NTFS 误删候选项；
    // - 支持驻留数据直接恢复到目标文件。
    struct NtfsDeletedFileEntry
    {
        QString fileName;                  // 删除项文件名。
        QString pathHint;                  // 删除前路径提示（尽力重建）。
        std::uint64_t sizeBytes = 0;       // 文件大小（字节）。
        QDateTime modifiedTime;            // 文件最后修改时间。
        std::uint64_t fileReference = 0;   // MFT 记录号（用于二次定位）。
        bool residentDataReady = false;    // 是否已提取驻留数据。
        QByteArray residentData;           // 驻留数据内容（仅 resident 文件可用）。
    };

    // ManualFileSystemParser 作用：
    // - 封装 NTFS/FAT32 的底层读取与解析；
    // - 对外输出统一结构，避免 UI 层直接处理底层格式。
    class ManualFileSystemParser final
    {
    public:
        // detectFileSystemType 作用：
        // - 识别路径所在卷的文件系统类型（NTFS/FAT32）。
        // 调用方法：
        // - UI 在切换目录或切换读取模式时调用。
        // 入参 pathText：
        // - 任意本地路径（例：C:\\Windows）。
        // 返回值：
        // - ManualFsType 枚举值。
        static ManualFsType detectFileSystemType(const QString& pathText);

        // enumerateDirectory 作用：
        // - 使用“手动解析”列出指定目录的子项。
        // 调用方法：
        // - FileDock 手动模式下调用。
        // 入参 pathText：
        // - 目标目录路径。
        // 出参 entriesOut：
        // - 返回目录条目集合。
        // 出参 fsTypeOut：
        // - 返回本次实际解析到的文件系统类型。
        // 出参 errorTextOut：
        // - 失败时返回可读错误描述。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        static bool enumerateDirectory(
            const QString& pathText,
            std::vector<ManualDirectoryEntry>& entriesOut,
            ManualFsType& fsTypeOut,
            QString& errorTextOut);

        // enumerateNtfsDeletedFiles 作用：
        // - 扫描指定 NTFS 卷内“已删除”文件候选项。
        // 调用方法：
        // - FileDock“文件恢复”页点击扫描时调用。
        // 入参 volumeRootPath：
        // - 卷根路径（例如 C:\\）。
        // 出参 deletedOut：
        // - 返回误删候选列表。
        // 出参 errorTextOut：
        // - 失败时返回错误文本。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        static bool enumerateNtfsDeletedFiles(
            const QString& volumeRootPath,
            std::vector<NtfsDeletedFileEntry>& deletedOut,
            QString& errorTextOut);

        // recoverNtfsResidentFile 作用：
        // - 对单个误删项执行“驻留数据恢复”。
        // 调用方法：
        // - FileDock“文件恢复”页中选择一行后调用。
        // 入参 volumeRootPath：
        // - 卷根路径（例如 C:\\），用于记录日志与路径归属校验。
        // 入参 deletedEntry：
        // - 待恢复条目（要求 residentDataReady=true）。
        // 入参 targetFilePath：
        // - 导出的目标文件路径。
        // 出参 errorTextOut：
        // - 失败时返回原因文本。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        static bool recoverNtfsResidentFile(
            const QString& volumeRootPath,
            const NtfsDeletedFileEntry& deletedEntry,
            const QString& targetFilePath,
            QString& errorTextOut);
    };
}

