#include "IrpFileSystemParser.h"

// ============================================================
// IrpFileSystemParser.cpp
// 作用：
// 1) 把 Win32 路径转换为驱动可打开的 NT 路径；
// 2) 调用 ArkDriverClient 的自建 IRP 目录枚举，取绕过层视图；
// 3) 再取一次栈顶视图做对照，把差集作为疑似隐藏条目上报。
// ============================================================

#include "../ArkDriverClient/ArkDriverClient.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTimeZone>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace
{
    // buildDriverNtPath 作用：把常见 Win32/长路径/UNC 路径转换为 \??\ 命名空间路径。
    QString buildDriverNtPath(const QString& pathText)
    {
        QString nativePath = QDir::toNativeSeparators(pathText.trimmed());
        if (nativePath.isEmpty())
        {
            return {};
        }
        if (nativePath.startsWith(QStringLiteral("\\??\\")))
        {
            return nativePath;
        }
        if (nativePath.startsWith(
            QStringLiteral("\\\\?\\UNC\\"),
            Qt::CaseInsensitive))
        {
            return QStringLiteral("\\??\\UNC\\") + nativePath.mid(8);
        }
        if (nativePath.startsWith(QStringLiteral("\\\\?\\")))
        {
            return QStringLiteral("\\??\\") + nativePath.mid(4);
        }
        if (nativePath.startsWith(QStringLiteral("\\\\")))
        {
            return QStringLiteral("\\??\\UNC\\") + nativePath.mid(2);
        }
        return QStringLiteral("\\??\\") + nativePath;
    }

    // manualFsTypeFromDriverName 作用：把 R0 FileFsAttributeInformation 名称映射为现有解析器枚举。
    ks::file::ManualFsType manualFsTypeFromDriverName(
        const std::wstring& fileSystemName)
    {
        const QString normalized =
            QString::fromStdWString(fileSystemName).trimmed().toUpper();
        if (normalized == QStringLiteral("NTFS"))
        {
            return ks::file::ManualFsType::Ntfs;
        }
        if (normalized == QStringLiteral("FAT32") ||
            normalized == QStringLiteral("FAT"))
        {
            return ks::file::ManualFsType::Fat32;
        }
        if (normalized == QStringLiteral("EXFAT"))
        {
            return ks::file::ManualFsType::ExFat;
        }
        return ks::file::ManualFsType::Unknown;
    }

    // fileTimeToLocal 作用：把驱动返回的 NT 100ns 时间戳转换为本地 QDateTime。
    QDateTime fileTimeToLocal(const std::int64_t fileTime100ns)
    {
        if (fileTime100ns <= 0)
        {
            return {};
        }
        constexpr qint64 EpochDeltaMsec = 11644473600000LL;
        const std::uint64_t ticks = static_cast<std::uint64_t>(fileTime100ns);
        if (ticks / 10000ULL >
            static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))
        {
            return {};
        }
        const qint64 unixMsec =
            static_cast<qint64>(ticks / 10000ULL) - EpochDeltaMsec;
        return QDateTime::fromMSecsSinceEpoch(
            unixMsec,
            QTimeZone::UTC).toLocalTime();
    }

    // buildTypeText 作用：生成与手动解析模式一致的“目录/扩展名 文件”类型文案。
    QString buildTypeText(const QString& fileName, const bool isDirectory)
    {
        if (isDirectory)
        {
            return QStringLiteral("目录");
        }
        const QString suffixText = QFileInfo(fileName).suffix().trimmed();
        return suffixText.isEmpty()
            ? QStringLiteral("文件")
            : suffixText.toUpper() + QStringLiteral(" 文件");
    }

    // statusHex 作用：统一把 NTSTATUS 格式化为固定八位十六进制诊断。
    QString statusHex(const long statusValue)
    {
        return QStringLiteral("0x%1")
            .arg(
                static_cast<qulonglong>(static_cast<unsigned long>(statusValue)),
                8,
                16,
                QChar('0'))
            .toUpper();
    }

    // convertRows 作用：把协议行映射为 FileDock 平铺模型，并回报截断信息。
    bool convertRows(
        const std::vector<ksword::ark::DirectoryEntryRecord>& sourceRows,
        const QString& currentPath,
        std::vector<ks::file::ManualDirectoryEntry>& entriesOut,
        bool& partialResult,
        const ks::file::DirectoryCancellationCallback& canceled)
    {
        entriesOut.reserve(sourceRows.size());
        for (const ksword::ark::DirectoryEntryRecord& source : sourceRows)
        {
            if (canceled && canceled()) return false;
            const QString name = QString::fromStdWString(source.name);
            if (name.isEmpty())
            {
                partialResult = true;
                continue;
            }

            ks::file::ManualDirectoryEntry entry{};
            entry.name = name;
            entry.absolutePath = QDir(currentPath).filePath(name);
            entry.isDirectory =
                (source.flags & KSWORD_ARK_DIRECTORY_ENTRY_FLAG_DIRECTORY) != 0U;
            entry.sizeBytes = source.endOfFile > 0
                ? static_cast<std::uint64_t>(source.endOfFile)
                : 0U;
            entry.modifiedTime = fileTimeToLocal(source.lastWriteTime);
            entry.typeText = buildTypeText(name, entry.isDirectory);
            entry.ntfsFileReference = source.fileId;
            entriesOut.push_back(std::move(entry));

            if ((source.flags &
                    KSWORD_ARK_DIRECTORY_ENTRY_FLAG_NAME_TRUNCATED) != 0U)
            {
                partialResult = true;
            }
        }
        return !(canceled && canceled());
    }

    // sortEntries 作用：目录优先、名称不区分大小写升序，与其它解析器保持一致。
    void sortEntries(std::vector<ks::file::ManualDirectoryEntry>& entriesOut)
    {
        std::sort(
            entriesOut.begin(),
            entriesOut.end(),
            [](const ks::file::ManualDirectoryEntry& left,
               const ks::file::ManualDirectoryEntry& right)
            {
                if (left.isDirectory != right.isDirectory)
                {
                    return left.isDirectory && !right.isDirectory;
                }
                return QString::compare(
                    left.name,
                    right.name,
                    Qt::CaseInsensitive) < 0;
            });
    }
}

QString ks::file::IrpFileSystemParser::layerDisplayText(
    const unsigned long layerValue)
{
    switch (layerValue)
    {
    case KSWORD_ARK_FILE_IRP_LAYER_RELATED:
        return QStringLiteral("设备栈顶");
    case KSWORD_ARK_FILE_IRP_LAYER_BASE_FS:
        return QStringLiteral("基础文件系统设备");
    case KSWORD_ARK_FILE_IRP_LAYER_VPB_FS:
        return QStringLiteral("VPB 挂载文件系统");
    case KSWORD_ARK_FILE_IRP_LAYER_DEVICE:
        return QStringLiteral("卷设备");
    default:
        return QStringLiteral("未知层(%1)").arg(layerValue);
    }
}

bool ks::file::IrpFileSystemParser::enumerateDirectory(
    const QString& pathText,
    std::vector<ManualDirectoryEntry>& entriesOut,
    ManualFsType& fsTypeOut,
    QString& errorTextOut,
    bool* partialOut,
    QString* sourceDetailOut,
    IrpScanDiagnostics* diagnosticsOut,
    const DirectoryCancellationCallback& canceled)
{
    entriesOut.clear();
    fsTypeOut = ManualFsType::Unknown;
    errorTextOut.clear();
    if (partialOut != nullptr)
    {
        *partialOut = false;
    }
    if (sourceDetailOut != nullptr)
    {
        sourceDetailOut->clear();
    }
    if (diagnosticsOut != nullptr)
    {
        *diagnosticsOut = IrpScanDiagnostics{};
    }

    if (canceled && canceled()) return false;

    const QString driverPath = buildDriverNtPath(pathText);
    if (driverPath.isEmpty())
    {
        errorTextOut = QStringLiteral("目录路径为空，无法执行 R0 IRP 解析。");
        return false;
    }

    const ksword::ark::DriverClient client;
    // 主视图固定请求基础文件系统设备：这是本模式存在的意义，
    // 请求层与生效层的差别会在诊断里如实回报。
    const ksword::ark::FileIrpDirectoryResult bypassResult =
        client.enumerateDirectoryByIrp(
            driverPath.toStdWString(),
            KSWORD_ARK_FILE_IRP_LAYER_BASE_FS);
    // Complete the current kernel call, then abandon stale comparisons and conversion.
    if (canceled && canceled()) return false;
    if (!bypassResult.io.ok)
    {
        errorTextOut = bypassResult.unsupported
            ? QStringLiteral("当前 KswordARK 驱动不支持 R0 IRP 目录枚举，请重新部署本次构建的驱动。")
            : QStringLiteral("R0 IRP 目录枚举通信失败：Win32=%1；%2")
                .arg(bypassResult.io.win32Error)
                .arg(QString::fromStdString(bypassResult.io.message));
        return false;
    }

    const bool semanticSuccess =
        bypassResult.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_OK ||
        bypassResult.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL;
    if (!semanticSuccess)
    {
        errorTextOut = QStringLiteral(
            "R0 无法用自建 IRP 枚举目录：queryStatus=%1；create=%2；last=%3。")
            .arg(bypassResult.queryStatus)
            .arg(statusHex(bypassResult.openStatus))
            .arg(statusHex(bypassResult.lastStatus));
        return false;
    }

    fsTypeOut = manualFsTypeFromDriverName(bypassResult.fileSystemName);
    bool partialResult = bypassResult.capped ||
        bypassResult.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL;
    const QString currentPath =
        QDir::toNativeSeparators(QDir::cleanPath(pathText));
    if (!convertRows(bypassResult.entries, currentPath, entriesOut, partialResult, canceled)) return false;
    if (canceled && canceled()) return false;
    sortEntries(entriesOut);
    if (canceled && canceled()) return false;

    const bool layerBypassed =
        bypassResult.resolvedLayer != KSWORD_ARK_FILE_IRP_LAYER_RELATED;

    if (diagnosticsOut != nullptr)
    {
        diagnosticsOut->requestedLayer = bypassResult.requestedLayer;
        diagnosticsOut->resolvedLayer = bypassResult.resolvedLayer;
        diagnosticsOut->layerBypassed = layerBypassed;
        diagnosticsOut->bypassEntryCount = static_cast<int>(entriesOut.size());
        diagnosticsOut->bypassDriverName =
            QString::fromStdWString(bypassResult.driverName);

        /*
         * 只有真正投递到更深的层，栈顶对照才有意义：R0 在目标层不可用时会回退到
         * 栈顶，此时两次请求同源，差集必然为空，把它当作"没有隐藏项"是错误结论。
         */
        if (layerBypassed && !(canceled && canceled()))
        {
            const ksword::ark::FileIrpDirectoryResult topResult =
                client.enumerateDirectoryByIrp(
                    driverPath.toStdWString(),
                    KSWORD_ARK_FILE_IRP_LAYER_RELATED);
            if (canceled && canceled()) return false;
            const bool topSemanticOk = topResult.io.ok &&
                (topResult.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_OK ||
                 topResult.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL);
            if (topSemanticOk)
            {
                std::vector<ManualDirectoryEntry> topEntries;
                bool topPartial = false;
                if (!convertRows(topResult.entries, currentPath, topEntries, topPartial, canceled)) return false;

                diagnosticsOut->comparisonAvailable = true;
                diagnosticsOut->topLayerEntryCount =
                    static_cast<int>(topEntries.size());
                diagnosticsOut->topLayerDriverName =
                    QString::fromStdWString(topResult.driverName);

                QSet<QString> bypassNameSet;
                bypassNameSet.reserve(static_cast<int>(entriesOut.size()) + 16);
                for (const ManualDirectoryEntry& itemValue : entriesOut)
                {
                    if (canceled && canceled()) return false;
                    bypassNameSet.insert(itemValue.name.toCaseFolded());
                }
                QSet<QString> topNameSet;
                topNameSet.reserve(static_cast<int>(topEntries.size()) + 16);
                for (const ManualDirectoryEntry& itemValue : topEntries)
                {
                    if (canceled && canceled()) return false;
                    topNameSet.insert(itemValue.name.toCaseFolded());
                }

                for (const ManualDirectoryEntry& itemValue : entriesOut)
                {
                    if (canceled && canceled()) return false;
                    if (!topNameSet.contains(itemValue.name.toCaseFolded()))
                    {
                        diagnosticsOut->bypassOnlyNames.append(itemValue.name);
                    }
                }
                for (const ManualDirectoryEntry& itemValue : topEntries)
                {
                    if (canceled && canceled()) return false;
                    if (!bypassNameSet.contains(itemValue.name.toCaseFolded()))
                    {
                        diagnosticsOut->topLayerOnlyNames.append(itemValue.name);
                    }
                }
            }
        }
    }

    if (canceled && canceled()) return false;
    if (partialOut != nullptr)
    {
        *partialOut = partialResult;
    }
    if (sourceDetailOut != nullptr)
    {
        const QString fsName = bypassResult.fileSystemName.empty()
            ? QStringLiteral("未知")
            : QString::fromStdWString(bypassResult.fileSystemName);
        const QString driverText = bypassResult.driverName.empty()
            ? QStringLiteral("未知驱动")
            : QString::fromStdWString(bypassResult.driverName);
        QString detailText =
            QStringLiteral("R0 IRP 解析%1；目标层=%2；接收驱动=%3；文件系统=%4；条目=%5")
                .arg(partialResult ? QStringLiteral("（部分结果）") : QString())
                .arg(layerDisplayText(bypassResult.resolvedLayer))
                .arg(driverText)
                .arg(fsName)
                .arg(entriesOut.size());
        if (!layerBypassed)
        {
            detailText += QStringLiteral("；已回退到栈顶，本次未绕过过滤层");
        }
        else if (diagnosticsOut != nullptr &&
            diagnosticsOut->comparisonAvailable &&
            !diagnosticsOut->bypassOnlyNames.isEmpty())
        {
            detailText += QStringLiteral("；栈顶不可见条目=%1")
                .arg(diagnosticsOut->bypassOnlyNames.size());
        }
        *sourceDetailOut = detailText;
    }
    return true;
}
