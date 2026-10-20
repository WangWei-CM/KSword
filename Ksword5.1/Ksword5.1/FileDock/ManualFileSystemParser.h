#pragma once

// ============================================================
// ManualFileSystemParser.h
// 作用：
// 1) 提供“手动解析文件系统”能力，支持 NTFS/FAT32/exFAT；
// 2) 提供目录项枚举接口，供 FileDock 手动模式直接展示；
// 3) 提供 NTFS 误删项扫描，以及驻留/完整非驻留数据恢复能力。
// ============================================================

#include "../Framework.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <vector>

namespace ks::file
{
    // 目录任务以廉价、无副作用回调判断是否过期；空回调保留同步调用行为。
    using DirectoryCancellationCallback = std::function<bool()>;

    // ManualFsType 作用：
    // - 表示解析器识别出的卷文件系统类型；
    // - Unknown 表示当前路径不支持手动解析。
    enum class ManualFsType : int
    {
        Unknown = 0,
        Ntfs = 1,
        Fat32 = 2,
        ExFat = 3
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

    // MftScanDiagnostics 作用：
    // - 记录"纯 $MFT 视图"与"Windows API 视图"的差异；
    // - mftOnly 集合是本功能的核心产出：只有直接解析 $MFT 才能看到、
    //   而 FindFirstFile/目录索引看不到的条目，正是过滤层隐藏文件的典型特征；
    // - winApiOnly 集合用于反向排查，通常来自扫描窗口不足或扫描期间的目录变化。
    struct MftScanDiagnostics
    {
        bool comparisonAvailable = false;  // Windows API 对照是否成功执行。
        int mftEntryCount = 0;             // 纯 MFT 视图条目数。
        int winApiEntryCount = 0;          // Windows API 视图条目数。
        QStringList mftOnlyNames;          // 只出现在 MFT 视图中的名称。
        QStringList winApiOnlyNames;       // 只出现在 Windows API 视图中的名称。
    };

    // NtfsRecoveryCapability 作用：
    // - 明确区分可安全导出的数据类型与仅可定位的元数据；
    // - 非驻留流只有在数据段完整、卷位图确认尚未复用时才标记为可恢复。
    enum class NtfsRecoveryCapability : int
    {
        MetadataOnly = 0,          // 仅有 MFT 元数据，没有可用主数据流。
        Resident = 1,              // 主数据流驻留于 MFT 记录，可直接恢复。
        NonResidentIntact = 2,     // 非驻留数据段完整且扫描时全部簇仍空闲。
        NonResidentAtRisk = 3,     // 非驻留数据簇已复用或完整度未知，禁止自动导出。
        UnsupportedStream = 4      // 压缩、加密或多段属性布局当前无法安全重建。
    };

    // NtfsDeletedFileEntry 作用：
    // - 表示扫描到的 NTFS 误删候选项；
    // - 保存恢复前二次校验所需的 MFT 序列号和恢复能力。
    struct NtfsDeletedFileEntry
    {
        QString fileName;                  // 删除项文件名。
        QString pathHint;                  // 删除前路径提示（尽力重建）。
        std::uint64_t sizeBytes = 0;       // 文件大小（字节）。
        QDateTime modifiedTime;            // 文件最后修改时间。
        std::uint64_t fileReference = 0;   // MFT 记录号（用于二次定位）。
        std::uint16_t sequenceNumber = 0;  // MFT 序列号（防止记录删除后被复用）。
        int estimatedIntegrityPercent = -1; // 估计完整度百分比，-1 表示当前无法评估。
        bool hasOriginalName = true;       // 是否保留了原始文件名，false 表示仅能生成占位名。
        bool residentDataReady = false;    // 是否已提取驻留数据。
        QByteArray residentData;           // 驻留数据内容（扫描阶段可为空，恢复时按需回读）。
        NtfsRecoveryCapability recoveryCapability =
            NtfsRecoveryCapability::MetadataOnly; // 扫描时评估出的恢复能力。
    };

    // ManualFileSystemParser 作用：
    // - 封装 NTFS/FAT32/exFAT 的底层读取与解析；
    // - 对外输出统一结构，避免 UI 层直接处理底层格式。
    class ManualFileSystemParser final
    {
    public:
        // detectFileSystemType 作用：
        // - 识别路径所在卷的文件系统类型（NTFS/FAT32/exFAT）。
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
        // 出参 usedWinApiFallbackOut：
        // - true 表示 NTFS 结果已发生 Windows API 回退或补齐；
        // - false 表示本次结果仍完全来自手动解析流程。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        // 入参 canceled：
        // - 返回 true 时中止当前目录任务；调用方须丢弃本次未完成输出。
        // 入参 strictMftOnly：
        // - true 时禁用全部 Windows API / FSCTL 回退与补齐，结果必须完全来自
        //   原始 $MFT 字节；仅对 NTFS 有意义，其它文件系统忽略该参数。
        static bool enumerateDirectory(
            const QString& pathText,
            std::vector<ManualDirectoryEntry>& entriesOut,
            ManualFsType& fsTypeOut,
            QString& errorTextOut,
            bool* usedWinApiFallbackOut = nullptr,
            ManualFsType requestedFsType = ManualFsType::Unknown,
            bool strictMftOnly = false,
            const DirectoryCancellationCallback& canceled = {});

        // enumerateDirectoryByMft 作用：
        // - 只用原始 $MFT 解析目录，全程不经过文件系统驱动与 Windows API：
        //   卷偏移直读 $MFT，禁用 FSCTL_GET_NTFS_FILE_RECORD 与任何 WinAPI 补齐；
        // - 额外跑一次 Windows API 枚举做**对照**（不并入结果），把两个视图的差集
        //   写入 diagnosticsOut，供 UI 标记疑似被隐藏的条目。
        // 调用方法：
        // - FileDock 选择"作为MFT解析"后在后台线程调用。
        // 入参 pathText：
        // - NTFS 卷上的目标目录路径。
        // 出参 entriesOut：
        // - 纯 MFT 视图的目录条目。
        // 出参 errorTextOut：
        // - 失败时返回原因；非 NTFS 卷会明确报告不支持。
        // 出参 diagnosticsOut：
        // - 可选，接收与 Windows API 视图的差异统计。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        // 入参 canceled：返回 true 时中止目录扫描或对照，不发布部分诊断。
        static bool enumerateDirectoryByMft(
            const QString& pathText,
            std::vector<ManualDirectoryEntry>& entriesOut,
            QString& errorTextOut,
            MftScanDiagnostics* diagnosticsOut = nullptr,
            const DirectoryCancellationCallback& canceled = {});

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
        // 入参 progressCallback：
        // - 可选进度回调，percent 范围约定为 0~100；
        // - stageText 用于向 UI 展示当前阶段说明。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        static bool enumerateNtfsDeletedFiles(
            const QString& volumeRootPath,
            std::vector<NtfsDeletedFileEntry>& deletedOut,
            QString& errorTextOut,
            const std::function<void(int, const QString&)>& progressCallback = {});

        // recoverNtfsDeletedFile 作用：
        // - 对单个误删项执行安全恢复；
        // - 支持驻留数据，以及卷位图前后两次校验均通过的完整非驻留数据。
        // 调用方法：
        // - FileDock“文件恢复”页中选择一行后调用。
        // 入参 volumeRootPath：
        // - 卷根路径（例如 C:\\），用于记录日志与路径归属校验。
        // 入参 deletedEntry：
        // - 待恢复条目；函数会按记录号和序列号重新读取 MFT，不信任旧扫描快照。
        // 入参 targetFilePath：
        // - 导出的目标文件路径。
        // 出参 errorTextOut：
        // - 失败时返回原因文本。
        // 入参 progressCallback：
        // - 可选恢复进度回调，percent 范围为 0~100。
        // 返回值：
        // - 成功返回 true，失败返回 false。
        static bool recoverNtfsDeletedFile(
            const QString& volumeRootPath,
            const NtfsDeletedFileEntry& deletedEntry,
            const QString& targetFilePath,
            QString& errorTextOut,
            const std::function<void(int, const QString&)>& progressCallback = {});

        // recoverNtfsResidentFile 作用：
        // - 兼容旧调用方的包装入口，内部统一走 recoverNtfsDeletedFile。
        static bool recoverNtfsResidentFile(
            const QString& volumeRootPath,
            const NtfsDeletedFileEntry& deletedEntry,
            const QString& targetFilePath,
            QString& errorTextOut);
    };
}
