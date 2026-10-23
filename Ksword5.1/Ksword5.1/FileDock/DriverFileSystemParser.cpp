#include "DriverFileSystemParser.h"

// ============================================================
// DriverFileSystemParser.cpp
// 作用：
// 1) 把 Win32 路径转换为驱动可打开的 NT 路径；
// 2) 调用 ArkDriverClient 合并 R0 分页目录快照；
// 3) 映射名称、属性、大小、时间和 FileId 到 FileDock 平铺模型。
// ============================================================

#include "../ArkDriverClient/ArkDriverClient.h"

#include <QDir>
#include <QFileInfo>
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
        const std::uint64_t ticks =
            static_cast<std::uint64_t>(fileTime100ns);
        if (ticks / 10000ULL >
            static_cast<std::uint64_t>(
                std::numeric_limits<qint64>::max()))
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
    QString buildTypeText(
        const QString& fileName,
        const bool isDirectory)
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
                static_cast<qulonglong>(
                    static_cast<unsigned long>(statusValue)),
                8,
                16,
                QChar('0'))
            .toUpper();
    }
}

bool ks::file::DriverFileSystemParser::enumerateDirectory(
    const QString& pathText,
    std::vector<ManualDirectoryEntry>& entriesOut,
    ManualFsType& fsTypeOut,
    QString& errorTextOut,
    bool* partialOut,
    QString* sourceDetailOut,
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

    if (canceled && canceled()) return false;

    const QString driverPath = buildDriverNtPath(pathText);
    if (driverPath.isEmpty())
    {
        errorTextOut = QStringLiteral("目录路径为空，无法执行 R0 驱动解析。");
        return false;
    }

    const ksword::ark::DirectoryEnumerationResult driverResult =
        ksword::ark::DriverClient().enumerateDirectory(
            driverPath.toStdWString());
    // A synchronous kernel call owns its request until completion. Skip all stale conversion.
    if (canceled && canceled()) return false;
    if (!driverResult.io.ok)
    {
        errorTextOut = driverResult.unsupported
            ? QStringLiteral("当前 KswordARK 驱动不支持 R0 目录枚举，请重新部署本次构建的驱动。")
            : QStringLiteral("R0 目录枚举通信失败：Win32=%1；%2")
                .arg(driverResult.io.win32Error)
                .arg(QString::fromStdString(driverResult.io.message));
        return false;
    }

    const bool semanticSuccess =
        driverResult.queryStatus ==
            KSWORD_ARK_DIRECTORY_ENUM_STATUS_OK ||
        driverResult.queryStatus ==
            KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL;
    if (!semanticSuccess)
    {
        errorTextOut = QStringLiteral(
            "R0 无法枚举目录：queryStatus=%1；open=%2；last=%3。")
            .arg(driverResult.queryStatus)
            .arg(statusHex(driverResult.openStatus))
            .arg(statusHex(driverResult.lastStatus));
        return false;
    }

    fsTypeOut = manualFsTypeFromDriverName(
        driverResult.fileSystemName);
    bool partialResult = driverResult.capped ||
        driverResult.queryStatus ==
            KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL;
    const QString currentPath =
        QDir::toNativeSeparators(QDir::cleanPath(pathText));
    entriesOut.reserve(driverResult.entries.size());
    for (const ksword::ark::DirectoryEntryRecord& source :
        driverResult.entries)
    {
        if (canceled && canceled()) return false;
        const QString name = QString::fromStdWString(source.name);
        if (name.isEmpty())
        {
            partialResult = true;
            continue;
        }

        ManualDirectoryEntry entry{};
        entry.name = name;
        entry.absolutePath = QDir(currentPath).filePath(name);
        entry.isDirectory =
            (source.flags &
                KSWORD_ARK_DIRECTORY_ENTRY_FLAG_DIRECTORY) != 0U;
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

    if (canceled && canceled()) return false;
    std::sort(
        entriesOut.begin(),
        entriesOut.end(),
        [](const ManualDirectoryEntry& left,
           const ManualDirectoryEntry& right)
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

    if (canceled && canceled()) return false;
    if (partialOut != nullptr)
    {
        *partialOut = partialResult;
    }
    if (sourceDetailOut != nullptr)
    {
        const QString fsName = driverResult.fileSystemName.empty()
            ? QStringLiteral("未知")
            : QString::fromStdWString(driverResult.fileSystemName);
        *sourceDetailOut = partialResult
            ? QStringLiteral("R0 驱动解析（部分结果）；文件系统=%1；条目=%2")
                .arg(fsName)
                .arg(entriesOut.size())
            : QStringLiteral("R0 驱动解析；文件系统=%1；条目=%2")
                .arg(fsName)
                .arg(entriesOut.size());
    }
    return true;
}
