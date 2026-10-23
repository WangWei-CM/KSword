#include "DiskRawFileSystemBrowser.h"

#include "DiskEditorBackend.h"
#include "DiskRawFileSystemInternal.h"

#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <limits>

namespace
{
    constexpr std::uint32_t kBackendTransferBytes = 256U * 1024U;

    std::uint64_t alignDown(
        const std::uint64_t value,
        const std::uint32_t alignment)
    {
        return value - (value % static_cast<std::uint64_t>(alignment));
    }

    bool alignUp(
        const std::uint64_t value,
        const std::uint32_t alignment,
        std::uint64_t& alignedOut)
    {
        const std::uint64_t remainder =
            value % static_cast<std::uint64_t>(alignment);
        if (remainder == 0U)
        {
            alignedOut = value;
            return true;
        }
        const std::uint64_t increment =
            static_cast<std::uint64_t>(alignment) - remainder;
        if (value > std::numeric_limits<std::uint64_t>::max() - increment)
        {
            return false;
        }
        alignedOut = value + increment;
        return true;
    }

    ks::misc::RawDirectoryResult unsupportedDirectoryResult(
        const ks::misc::ForensicFileSystemKind kind,
        const QString& path)
    {
        ks::misc::RawDirectoryResult result;
        result.fileSystem = kind;
        result.fileSystemName =
            ks::misc::DiskFileSystemForensics::fileSystemName(kind);
        result.requestedPath = path;
        result.errorText = QStringLiteral(
            "该文件系统的原始目录解析器未能建立可验证的挂载状态。");
        return result;
    }

    ks::misc::RawFileReadResult unsupportedFileResult(
        const ks::misc::ForensicFileSystemKind kind,
        const QString& path)
    {
        ks::misc::RawFileReadResult result;
        result.fileSystem = kind;
        result.fileSystemName =
            ks::misc::DiskFileSystemForensics::fileSystemName(kind);
        result.filePath = path;
        result.errorText = QStringLiteral(
            "该文件系统的原始文件读取器未能建立可验证的挂载状态。");
        return result;
    }
}

namespace ks::misc::rawfs
{
    VolumeReader::VolumeReader(
        const int diskIndex,
        const unsigned long backend,
        const std::uint64_t partitionOffset,
        const std::uint64_t partitionLength,
        const std::uint32_t logicalSectorSize)
        : m_diskIndex(diskIndex),
          m_backend(backend),
          m_partitionOffset(partitionOffset),
          m_partitionLength(partitionLength),
          m_logicalSectorSize(
              logicalSectorSize == 0U ? 512U : logicalSectorSize)
    {
    }

    bool VolumeReader::rangeIsValid(
        const std::uint64_t relativeOffset,
        const std::uint64_t length) const
    {
        return length != 0U
            && relativeOffset <= m_partitionLength
            && length <= m_partitionLength - relativeOffset;
    }

    bool VolumeReader::read(
        const std::uint64_t relativeOffset,
        const std::uint32_t length,
        QByteArray& bytesOut,
        QString& errorTextOut) const
    {
        bytesOut.clear();
        errorTextOut.clear();
        if (length == 0U)
        {
            return true;
        }
        if (!rangeIsValid(relativeOffset, length))
        {
            errorTextOut = QStringLiteral("读取范围越过所选分区边界。");
            return false;
        }

        const std::uint64_t alignedStart =
            alignDown(relativeOffset, m_logicalSectorSize);
        const std::uint64_t prefix = relativeOffset - alignedStart;
        if (prefix > std::numeric_limits<std::uint64_t>::max() - length)
        {
            errorTextOut = QStringLiteral("读取范围发生整数溢出。");
            return false;
        }

        std::uint64_t alignedLength = 0;
        if (!alignUp(
                prefix + static_cast<std::uint64_t>(length),
                m_logicalSectorSize,
                alignedLength)
            || !rangeIsValid(alignedStart, alignedLength)
            || m_partitionOffset
                > std::numeric_limits<std::uint64_t>::max()
                    - alignedStart
                    - alignedLength
            || alignedLength
                > static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max()))
        {
            errorTextOut = QStringLiteral("扇区对齐后的读取范围无效。");
            return false;
        }

        QByteArray alignedBytes(
            static_cast<qsizetype>(alignedLength),
            Qt::Uninitialized);
        std::uint64_t completed = 0;
        while (completed < alignedLength)
        {
            const std::uint64_t remaining = alignedLength - completed;
            const std::uint32_t chunkLength = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    remaining,
                    kBackendTransferBytes));
            QByteArray chunk;
            if (!DiskEditorBackend::readBytesWithBackend(
                    m_diskIndex,
                    m_backend,
                    m_partitionOffset + alignedStart + completed,
                    chunkLength,
                    chunk,
                    errorTextOut))
            {
                bytesOut.clear();
                return false;
            }
            if (chunk.size() != static_cast<qsizetype>(chunkLength))
            {
                bytesOut.clear();
                errorTextOut = QStringLiteral("磁盘后端返回了截断的数据。");
                return false;
            }
            std::copy(
                chunk.cbegin(),
                chunk.cend(),
                alignedBytes.begin() + static_cast<qsizetype>(completed));
            completed += chunkLength;
        }

        bytesOut = alignedBytes.mid(
            static_cast<qsizetype>(prefix),
            static_cast<qsizetype>(length));
        return true;
    }

    std::uint64_t VolumeReader::partitionOffset() const
    {
        return m_partitionOffset;
    }

    std::uint64_t VolumeReader::partitionLength() const
    {
        return m_partitionLength;
    }

    QString normalizePath(const QString& path)
    {
        QString normalized = path.trimmed();
        normalized.replace(QChar('/'), QChar('\\'));
        if (normalized.isEmpty())
        {
            return QStringLiteral("\\");
        }
        if (!normalized.startsWith(QChar('\\')))
        {
            normalized.prepend(QChar('\\'));
        }
        while (normalized.contains(QStringLiteral("\\\\")))
        {
            normalized.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
        if (normalized.size() > 1 && normalized.endsWith(QChar('\\')))
        {
            normalized.chop(1);
        }
        return normalized;
    }

    QString childPath(const QString& parent, const QString& name)
    {
        const QString normalizedParent = normalizePath(parent);
        return normalizedParent == QStringLiteral("\\")
            ? QStringLiteral("\\") + name
            : normalizedParent + QStringLiteral("\\") + name;
    }
}

namespace ks::misc
{
    RawDirectoryResult DiskRawFileSystemBrowser::listDirectory(
        const int diskIndex,
        const unsigned long backend,
        const std::uint64_t partitionOffset,
        const std::uint64_t partitionLength,
        const std::uint32_t logicalSectorSize,
        const ForensicFileSystemKind fileSystem,
        const QString& path,
        const std::uint32_t maximumEntries)
    {
        const rawfs::VolumeReader reader(
            diskIndex,
            backend,
            partitionOffset,
            partitionLength,
            logicalSectorSize);
        const QString normalized = rawfs::normalizePath(path);
        const std::uint32_t bounded =
            std::clamp(maximumEntries, 1U, 65536U);

        switch (fileSystem)
        {
        case ForensicFileSystemKind::Ntfs:
            return rawfs::listNtfs(reader, normalized, bounded);
        case ForensicFileSystemKind::Fat12:
        case ForensicFileSystemKind::Fat16:
        case ForensicFileSystemKind::Fat32:
            return rawfs::listFat(
                reader,
                fileSystem,
                normalized,
                bounded);
        case ForensicFileSystemKind::ExFat:
            return rawfs::listExFat(
                reader,
                normalized,
                bounded);
        case ForensicFileSystemKind::Ext2:
        case ForensicFileSystemKind::Ext3:
        case ForensicFileSystemKind::Ext4:
            return rawfs::listExt(reader, fileSystem, normalized, bounded);
        case ForensicFileSystemKind::Btrfs:
            return rawfs::listBtrfs(reader, normalized, bounded);
        case ForensicFileSystemKind::Apfs:
            return rawfs::listApfs(reader, normalized, bounded);
        case ForensicFileSystemKind::Hfs:
        case ForensicFileSystemKind::HfsPlus:
            return rawfs::listHfs(reader, fileSystem, normalized, bounded);
        case ForensicFileSystemKind::ReFs:
            return rawfs::listRefs(reader, normalized, bounded);
        default:
            return unsupportedDirectoryResult(fileSystem, normalized);
        }
    }

    namespace
    {
        RawFileReadResult readFileUsingReader(const rawfs::VolumeReader& reader,
            const ForensicFileSystemKind fileSystem, const QString& normalized,
            const std::uint64_t offset, const std::uint32_t length)
        {
        if (length == 0U || length > rawfs::kMaximumSingleReadBytes)
        {
            RawFileReadResult result =
                unsupportedFileResult(fileSystem, normalized);
            result.errorText = QStringLiteral(
                "单次文件读取长度必须在 1 字节到 16 MiB 之间。");
            return result;
        }
        switch (fileSystem)
        {
        case ForensicFileSystemKind::Ntfs:
            return rawfs::readNtfs(
                reader,
                normalized,
                offset,
                length);
        case ForensicFileSystemKind::Fat12:
        case ForensicFileSystemKind::Fat16:
        case ForensicFileSystemKind::Fat32:
            return rawfs::readFat(
                reader,
                fileSystem,
                normalized,
                offset,
                length);
        case ForensicFileSystemKind::ExFat:
            return rawfs::readExFat(
                reader,
                normalized,
                offset,
                length);
        case ForensicFileSystemKind::Ext2:
        case ForensicFileSystemKind::Ext3:
        case ForensicFileSystemKind::Ext4:
            return rawfs::readExt(
                reader, fileSystem, normalized, offset, length);
        case ForensicFileSystemKind::Btrfs:
            return rawfs::readBtrfs(reader, normalized, offset, length);
        case ForensicFileSystemKind::Apfs:
            return rawfs::readApfs(reader, normalized, offset, length);
        case ForensicFileSystemKind::Hfs:
        case ForensicFileSystemKind::HfsPlus:
            return rawfs::readHfs(
                reader, fileSystem, normalized, offset, length);
        case ForensicFileSystemKind::ReFs:
            return rawfs::readRefs(reader, normalized, offset, length);
        default:
            return unsupportedFileResult(fileSystem, normalized);
        }
    }
    }

    RawFileReadResult DiskRawFileSystemBrowser::readFile(
        const int diskIndex,
        const unsigned long backend,
        const std::uint64_t partitionOffset,
        const std::uint64_t partitionLength,
        const std::uint32_t logicalSectorSize,
        const ForensicFileSystemKind fileSystem,
        const QString& path,
        const std::uint64_t offset,
        const std::uint32_t length)
    {
        const QString normalized = rawfs::normalizePath(path);
        const rawfs::VolumeReader reader(
            diskIndex,
            backend,
            partitionOffset,
            partitionLength,
            logicalSectorSize);

        return readFileUsingReader(reader, fileSystem, normalized, offset, length);
    }

    DiskRawFileSystemBrowser::FileReadSession DiskRawFileSystemBrowser::createReadSession(
        const int diskIndex, const unsigned long backend, const std::uint64_t partitionOffset,
        const std::uint64_t partitionLength, const std::uint32_t logicalSectorSize,
        const ForensicFileSystemKind fileSystem, const QString& path)
    {
        const auto reader = std::make_shared<rawfs::VolumeReader>(diskIndex, backend,
            partitionOffset, partitionLength, logicalSectorSize);
        reader->enableFileSession();
        const QString normalized = rawfs::normalizePath(path);
        return [reader, fileSystem, normalized](const std::uint64_t offset, const std::uint32_t length) {
            return readFileUsingReader(*reader, fileSystem, normalized, offset, length);
        };
    }

    RawFileExportResult DiskRawFileSystemBrowser::exportFile(
        const int diskIndex,
        const unsigned long backend,
        const std::uint64_t partitionOffset,
        const std::uint64_t partitionLength,
        const std::uint32_t logicalSectorSize,
        const ForensicFileSystemKind fileSystem,
        const QString& sourcePath,
        const QString& destinationPath)
    {
        RawFileExportResult result;
        result.filePath = rawfs::normalizePath(sourcePath);
        result.destinationPath = QFileInfo(destinationPath).absoluteFilePath();
        if (destinationPath.trimmed().isEmpty())
        {
            result.errorText = QStringLiteral("导出目标路径为空。");
            return result;
        }

        QSaveFile destination(result.destinationPath);
        if (!destination.open(QIODevice::WriteOnly))
        {
            result.errorText = QStringLiteral("无法创建导出文件：%1")
                .arg(destination.errorString());
            return result;
        }

        std::uint64_t offset = 0;
        for (;;)
        {
            const RawFileReadResult chunk = readFile(
                diskIndex,
                backend,
                partitionOffset,
                partitionLength,
                logicalSectorSize,
                fileSystem,
                result.filePath,
                offset,
                rawfs::kExportChunkBytes);
            if (!chunk.success)
            {
                destination.cancelWriting();
                result.errorText = chunk.errorText;
                return result;
            }

            result.fileSizeBytes = chunk.fileSizeBytes;
            if (!chunk.bytes.isEmpty())
            {
                const qint64 written = destination.write(chunk.bytes);
                if (written != static_cast<qint64>(chunk.bytes.size()))
                {
                    destination.cancelWriting();
                    result.errorText = QStringLiteral(
                        "写入导出文件失败：%1")
                        .arg(destination.errorString());
                    return result;
                }
                offset += static_cast<std::uint64_t>(chunk.bytes.size());
                result.bytesWritten = offset;
            }
            if (chunk.endOfFile || chunk.bytes.isEmpty())
            {
                break;
            }
        }

        if (!destination.commit())
        {
            result.errorText = QStringLiteral("提交导出文件失败：%1")
                .arg(destination.errorString());
            return result;
        }
        result.success = true;
        return result;
    }

    QString DiskRawFileSystemBrowser::objectTypeText(
        const RawFileObjectType type)
    {
        switch (type)
        {
        case RawFileObjectType::RegularFile:
            return QStringLiteral("文件");
        case RawFileObjectType::Directory:
            return QStringLiteral("目录");
        case RawFileObjectType::SymbolicLink:
            return QStringLiteral("符号链接");
        case RawFileObjectType::Special:
            return QStringLiteral("特殊对象");
        case RawFileObjectType::NamedStream:
            return QStringLiteral("NTFS 命名流");
        default:
            return QStringLiteral("未知");
        }
    }
}
