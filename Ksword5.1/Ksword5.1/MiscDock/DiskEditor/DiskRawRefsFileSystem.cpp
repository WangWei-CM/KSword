#include "DiskRawFileSystemInternal.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    constexpr std::uint32_t kV1MetadataBlockSize = 16U * 1024U;
    constexpr std::uint32_t kV1MetadataHeaderSize = 48U;
    constexpr std::uint32_t kV3MetadataHeaderSize = 80U;
    constexpr std::uint32_t kV1BlockReferenceSize = 24U;
    constexpr std::uint32_t kV3BlockReferenceSize = 48U;
    constexpr std::uint32_t kV1NodeHeaderSize = 32U;
    constexpr std::uint32_t kV3NodeHeaderSize = 40U;
    constexpr std::uint32_t kV1RecordHeaderSize = 14U;
    constexpr std::uint32_t kV3RecordHeaderSize = 16U;
    constexpr std::uint64_t kRootDirectoryObjectId = 0x600U;
    constexpr std::uint32_t kObjectsTreeSlot = 0U;
    constexpr std::uint32_t kObjectsTreeCopySlot = 5U;
    constexpr std::uint16_t kRecordUnallocated = 0x0004U;
    constexpr std::uint16_t kRecordEmbeddedNode = 0x0008U;
    constexpr std::uint16_t kDirectoryEntryRecord = 0x0030U;
    constexpr std::uint16_t kMetadataEntryKind = 0U;
    constexpr std::uint16_t kFileEntryKind = 1U;
    constexpr std::uint16_t kDirectoryEntryKind = 2U;
    constexpr std::uint32_t kV3ResidentDataOffset = 0x130U;
    constexpr std::uint32_t kV3ExtentHeaderOffset = 0x1F0U;
    constexpr std::uint32_t kV3OuterNodeHeaderOffset = 0xA8U;
    constexpr std::uint32_t kV3RunSize = 24U;
    constexpr std::uint32_t kV3ContainerShift = 14U;
    constexpr std::uint64_t kV3ContainerClusters = 1ULL << 14U;
    constexpr std::uint64_t kV3BootstrapClusters = 4ULL << 14U;
    constexpr std::uint32_t kContainerScanChunkBytes = 256U * 1024U;
    constexpr std::uint32_t kMaximumRecordsPerNode = 0x4000U;
    constexpr std::uint32_t kMaximumTreeNodes = 1U << 20U;
    constexpr std::uint32_t kMaximumDirectoryRecords = 1U << 20U;

    std::uint16_t le16(const unsigned char* bytes)
    {
        return static_cast<std::uint16_t>(bytes[0])
            | (static_cast<std::uint16_t>(bytes[1]) << 8U);
    }

    std::uint32_t le32(const unsigned char* bytes)
    {
        return static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8U)
            | (static_cast<std::uint32_t>(bytes[2]) << 16U)
            | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }

    std::uint64_t le64(const unsigned char* bytes)
    {
        return static_cast<std::uint64_t>(le32(bytes))
            | (static_cast<std::uint64_t>(le32(bytes + 4U)) << 32U);
    }

    const unsigned char* dataPointer(const QByteArray& bytes)
    {
        return reinterpret_cast<const unsigned char*>(bytes.constData());
    }

    bool isPowerOfTwo(const std::uint32_t value)
    {
        return value != 0U && (value & (value - 1U)) == 0U;
    }

    bool checkedMultiply(
        const std::uint64_t left,
        const std::uint64_t right,
        std::uint64_t& resultOut)
    {
        if (left != 0U
            && right > std::numeric_limits<std::uint64_t>::max() / left)
        {
            return false;
        }
        resultOut = left * right;
        return true;
    }

    bool hasSignature(
        const unsigned char* bytes,
        const char* signature)
    {
        return bytes[0U] == static_cast<unsigned char>(signature[0])
            && bytes[1U] == static_cast<unsigned char>(signature[1])
            && bytes[2U] == static_cast<unsigned char>(signature[2])
            && bytes[3U] == static_cast<unsigned char>(signature[3]);
    }

    QString decodeUtf16Le(
        const unsigned char* bytes,
        std::uint32_t characterCount)
    {
        while (characterCount != 0U
            && le16(bytes + (characterCount - 1U) * 2U) == 0U)
        {
            --characterCount;
        }
        QString result;
        result.reserve(static_cast<qsizetype>(characterCount));
        for (std::uint32_t index = 0;
             index < characterCount;
             ++index)
        {
            result.append(QChar(le16(bytes + index * 2U)));
        }
        return result;
    }

    struct RefsBlockReference
    {
        std::array<std::uint64_t, 4U> blocks{};
    };

    struct RefsNodeHeader
    {
        std::uint32_t headerOffset = 0;
        std::uint32_t dataStart = 0;
        std::uint32_t dataEnd = 0;
        std::uint32_t recordOffsetsStart = 0;
        std::uint32_t recordCount = 0;
        std::uint8_t level = 0;
        std::uint8_t typeFlags = 0;
    };

    struct RefsRecordView
    {
        const unsigned char* key = nullptr;
        const unsigned char* value = nullptr;
        std::uint32_t keySize = 0;
        std::uint32_t valueSize = 0;
        std::uint32_t recordOffset = 0;
        std::uint16_t flags = 0;
        std::uint64_t valueAbsoluteOffset = 0;
    };

    struct RefsEntry
    {
        QString name;
        bool directory = false;
        std::uint64_t objectId = 0;
        std::uint64_t parentObjectId = 0;
        std::uint64_t dataSize = 0;
        std::uint64_t allocatedSize = 0;
        std::uint64_t metadataOffset = 0;
        std::uint32_t attributes = 0;
        RefsBlockReference directoryReference;
        QByteArray value;
    };

    struct RefsRun
    {
        std::uint64_t logicalOffset = 0;
        std::uint64_t length = 0;
        std::uint64_t physicalOffset = 0;
        bool sparse = false;
        bool resident = false;
    };

    struct RefsContainer
    {
        std::uint64_t id = 0;
        std::uint64_t physicalStart = 0;
    };

    struct RefsCheckpoint
    {
        std::uint64_t sequence = 0;
        std::array<RefsBlockReference, 13U> references{};
        std::array<bool, 13U> present{};
    };

    class RefsVolume final
    {
    public:
        explicit RefsVolume(
            const ks::misc::rawfs::VolumeReader& reader)
            : m_reader(reader)
        {
        }

        bool mount(QString& errorText)
        {
            QByteArray boot;
            if (!m_reader.read(0U, 512U, boot, errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(boot);
            if (bytes[3U] != static_cast<unsigned char>('R')
                || bytes[4U] != static_cast<unsigned char>('e')
                || bytes[5U] != static_cast<unsigned char>('F')
                || bytes[6U] != static_cast<unsigned char>('S')
                || bytes[7U] != 0U
                || bytes[8U] != 0U
                || bytes[9U] != 0U
                || bytes[10U] != 0U
                || QByteArray(
                    reinterpret_cast<const char*>(bytes + 0x10U),
                    4) != QByteArrayLiteral("FSRS"))
            {
                errorText = QStringLiteral("ReFS 引导区签名不匹配。");
                return false;
            }
            const std::uint16_t structureSize = le16(bytes + 0x14U);
            m_majorVersion = bytes[0x28U];
            m_minorVersion = bytes[0x29U];
            m_bytesPerSector = le32(bytes + 0x20U);
            m_sectorsPerCluster = le32(bytes + 0x24U);
            const std::uint64_t sectorCount = le64(bytes + 0x18U);
            if (structureSize < 0x58U
                || structureSize > boot.size()
                || (m_majorVersion != 1U
                    && m_majorVersion != 2U
                    && m_majorVersion != 3U)
                || !isPowerOfTwo(m_bytesPerSector)
                || m_bytesPerSector < 512U
                || m_bytesPerSector > 65536U
                || m_sectorsPerCluster == 0U
                || m_sectorsPerCluster > 8192U)
            {
                errorText = QStringLiteral("ReFS 引导区几何参数无效。");
                return false;
            }
            const std::uint64_t clusterSize =
                static_cast<std::uint64_t>(m_bytesPerSector)
                * m_sectorsPerCluster;
            std::uint64_t volumeBytes = 0;
            if (clusterSize > 1024U * 1024U
                || !checkedMultiply(
                    sectorCount,
                    m_bytesPerSector,
                    volumeBytes)
                || volumeBytes > m_reader.partitionLength())
            {
                errorText = QStringLiteral("ReFS 卷范围越过所选分区。");
                return false;
            }
            m_clusterSize = static_cast<std::uint32_t>(clusterSize);
            m_v3 = m_majorVersion >= 3U;
            m_metadataBlockSize = m_v3
                ? std::max<std::uint32_t>(
                    kV1MetadataBlockSize,
                    m_clusterSize * 4U)
                : kV1MetadataBlockSize;
            return m_v3
                ? mountV3(errorText)
                : mountV1(errorText);
        }

        bool resolvePath(
            const QString& path,
            RefsEntry& entryOut,
            QString& canonicalPathOut,
            QString& errorText)
        {
            RefsEntry current;
            current.name = QStringLiteral("\\");
            current.directory = true;
            current.objectId = kRootDirectoryObjectId;
            current.directoryReference = m_rootDirectoryReference;
            canonicalPathOut = QStringLiteral("\\");

            const QStringList components =
                ks::misc::rawfs::normalizePath(path).split(
                    QChar('\\'),
                    Qt::SkipEmptyParts);
            for (const QString& component : components)
            {
                if (!current.directory)
                {
                    errorText = QStringLiteral(
                        "路径中间对象不是 ReFS 目录：%1")
                        .arg(canonicalPathOut);
                    return false;
                }
                RefsEntry child;
                if (!findDirectoryChild(
                        current,
                        component,
                        child,
                        errorText))
                {
                    return false;
                }
                current = std::move(child);
                canonicalPathOut =
                    ks::misc::rawfs::childPath(
                        canonicalPathOut,
                        current.name);
            }
            entryOut = std::move(current);
            return true;
        }

        bool listDirectory(
            const RefsEntry& directory,
            const QString& canonicalPath,
            const std::uint32_t maximumEntries,
            ks::misc::RawDirectoryResult& result)
        {
            if (!directory.directory)
            {
                result.errorText = QStringLiteral("目标对象不是 ReFS 目录。");
                return false;
            }
            std::vector<RefsEntry> entries;
            if (!enumerateDirectory(
                    directory,
                    maximumEntries,
                    entries,
                    result.scannedRecords,
                    result.truncated,
                    result.errorText))
            {
                return false;
            }
            result.entries.reserve(entries.size());
            for (RefsEntry& source : entries)
            {
                std::vector<RefsRun> runs;
                if (!source.directory
                    && !loadRuns(source, runs, result.errorText))
                {
                    return false;
                }
                ks::misc::RawFileEntry entry;
                fillEntry(
                    source,
                    canonicalPath,
                    runs,
                    entry);
                result.entries.push_back(std::move(entry));
            }
            return true;
        }

        bool readFile(
            const RefsEntry& file,
            const std::uint64_t offset,
            const std::uint32_t length,
            ks::misc::RawFileReadResult& result)
        {
            if (file.directory)
            {
                result.errorText = QStringLiteral(
                    "目标对象是目录，不能按文件读取。");
                return false;
            }
            if (offset >= file.dataSize)
            {
                result.endOfFile = true;
                return true;
            }
            std::vector<RefsRun> runs;
            if (!loadRuns(file, runs, result.errorText))
            {
                return false;
            }
            result.extents = describeRuns(file, runs);
            result.extentsTruncated =
                runs.size() > ks::misc::rawfs::kMaximumExtentCount;

            const std::uint32_t bounded =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        length,
                        file.dataSize - offset));
            result.bytes = QByteArray(
                static_cast<qsizetype>(bounded),
                '\0');
            const std::uint64_t requestEnd = offset + bounded;
            for (const RefsRun& run : runs)
            {
                const std::uint64_t runEnd =
                    run.logicalOffset + run.length;
                const std::uint64_t overlapStart =
                    std::max(offset, run.logicalOffset);
                const std::uint64_t overlapEnd =
                    std::min(requestEnd, runEnd);
                if (overlapStart >= overlapEnd || run.sparse)
                {
                    continue;
                }
                const std::uint64_t within =
                    overlapStart - run.logicalOffset;
                const std::uint32_t copyLength =
                    static_cast<std::uint32_t>(
                        overlapEnd - overlapStart);
                const qsizetype destination =
                    static_cast<qsizetype>(overlapStart - offset);
                if (run.resident)
                {
                    const std::uint64_t valueOffset =
                        run.physicalOffset - file.metadataOffset;
                    if (valueOffset + within + copyLength
                        > static_cast<std::uint64_t>(file.value.size()))
                    {
                        result.bytes.clear();
                        result.errorText = QStringLiteral(
                            "ReFS 驻留数据范围越界。");
                        return false;
                    }
                    std::copy_n(
                        file.value.cbegin()
                            + static_cast<qsizetype>(
                                valueOffset + within),
                        copyLength,
                        result.bytes.begin() + destination);
                }
                else
                {
                    if (run.physicalOffset
                        < m_reader.partitionOffset())
                    {
                        result.bytes.clear();
                        result.errorText = QStringLiteral(
                            "ReFS extent 的绝对物理地址无效。");
                        return false;
                    }
                    const std::uint64_t relative =
                        run.physicalOffset
                        - m_reader.partitionOffset()
                        + within;
                    QByteArray bytes;
                    if (!m_reader.read(
                            relative,
                            copyLength,
                            bytes,
                            result.errorText))
                    {
                        result.bytes.clear();
                        return false;
                    }
                    std::copy(
                        bytes.cbegin(),
                        bytes.cend(),
                        result.bytes.begin() + destination);
                }
            }
            result.endOfFile = requestEnd >= file.dataSize;
            return true;
        }

    private:
        bool mountV1(QString& errorText)
        {
            std::vector<std::uint64_t> checkpointBlocks;
            if (!findSuperblockV1(
                    checkpointBlocks,
                    errorText))
            {
                return false;
            }
            RefsCheckpoint checkpoint;
            if (!pickCheckpointV1(
                    checkpointBlocks,
                    checkpoint,
                    errorText))
            {
                return false;
            }
            const std::uint32_t selected =
                checkpoint.present[kObjectsTreeSlot]
                    ? kObjectsTreeSlot
                    : kObjectsTreeCopySlot;
            if (!checkpoint.present[selected]
                || checkpoint.references[selected].blocks[0U] == 0U)
            {
                errorText = QStringLiteral(
                    "ReFS v1 checkpoint 缺少对象树引用。");
                return false;
            }
            m_objectsReference = checkpoint.references[selected];
            return objectLookup(
                m_objectsReference,
                kRootDirectoryObjectId,
                m_rootDirectoryReference,
                errorText);
        }

        bool mountV3(QString& errorText)
        {
            std::vector<std::uint64_t> checkpointBlocks;
            if (!findSuperblockV3(
                    checkpointBlocks,
                    errorText))
            {
                return false;
            }
            RefsCheckpoint checkpoint;
            if (!pickCheckpointV3(
                    checkpointBlocks,
                    checkpoint,
                    errorText))
            {
                return false;
            }
            std::array<std::uint32_t, 2U> candidates{
                kObjectsTreeCopySlot,
                kObjectsTreeSlot};
            for (const std::uint32_t selected : candidates)
            {
                if (!checkpoint.present[selected]
                    || checkpoint.references[selected].blocks[0U] == 0U)
                {
                    continue;
                }
                QString attemptError;
                RefsBlockReference root;
                if (objectLookup(
                        checkpoint.references[selected],
                        kRootDirectoryObjectId,
                        root,
                        attemptError))
                {
                    m_objectsReference =
                        checkpoint.references[selected];
                    m_rootDirectoryReference = root;
                    return true;
                }
                errorText = attemptError;
            }
            if (errorText.isEmpty())
            {
                errorText = QStringLiteral(
                    "ReFS v3 checkpoint 缺少可用对象树引用。");
            }
            return false;
        }

        bool findSuperblockV1(
            std::vector<std::uint64_t>& checkpointsOut,
            QString& errorText)
        {
            const std::uint64_t totalBlocks =
                m_reader.partitionLength() / kV1MetadataBlockSize;
            std::vector<std::uint64_t> candidates{30U};
            for (std::uint64_t index = 0;
                 index < 16U && index < totalBlocks;
                 ++index)
            {
                candidates.push_back(totalBlocks - 1U - index);
            }
            for (const std::uint64_t blockNumber : candidates)
            {
                QByteArray block;
                QString ignored;
                if (!readV1Block(
                        blockNumber,
                        block,
                        ignored)
                    || block.size()
                        < static_cast<qsizetype>(
                            kV1MetadataHeaderSize + 48U))
                {
                    continue;
                }
                const unsigned char* const bytes = dataPointer(block);
                const std::uint32_t body = kV1MetadataHeaderSize;
                const std::uint32_t arrayOffset =
                    le32(bytes + body + 0x20U);
                const std::uint32_t count =
                    le32(bytes + body + 0x24U);
                if (count == 0U || count > 8U
                    || arrayOffset > block.size()
                    || static_cast<std::uint64_t>(count) * 8U
                        > static_cast<std::uint64_t>(block.size())
                            - arrayOffset)
                {
                    continue;
                }
                checkpointsOut.clear();
                for (std::uint32_t index = 0;
                     index < count;
                     ++index)
                {
                    checkpointsOut.push_back(
                        le64(bytes + arrayOffset + index * 8U));
                }
                return true;
            }
            errorText = QStringLiteral("未找到有效 ReFS v1 superblock。");
            return false;
        }

        bool findSuperblockV3(
            std::vector<std::uint64_t>& checkpointsOut,
            QString& errorText)
        {
            const std::uint64_t totalClusters =
                m_reader.partitionLength() / m_clusterSize;
            std::vector<std::uint64_t> candidates{30U};
            for (std::uint64_t index = 0;
                 index < 16U && index < totalClusters;
                 ++index)
            {
                candidates.push_back(totalClusters - 1U - index);
            }
            for (const std::uint64_t physicalCluster : candidates)
            {
                QByteArray block;
                QString ignored;
                if (!readV3PhysicalBlock(
                        physicalCluster,
                        block,
                        ignored)
                    || !hasSignature(dataPointer(block), "SUPB")
                    || block.size()
                        < static_cast<qsizetype>(
                            kV3MetadataHeaderSize + 48U))
                {
                    continue;
                }
                const unsigned char* const bytes = dataPointer(block);
                const std::uint32_t body = kV3MetadataHeaderSize;
                const std::uint32_t arrayOffset =
                    le32(bytes + body + 0x20U);
                const std::uint32_t count =
                    le32(bytes + body + 0x24U);
                if (count == 0U || count > 8U
                    || arrayOffset > block.size()
                    || static_cast<std::uint64_t>(count) * 8U
                        > static_cast<std::uint64_t>(block.size())
                            - arrayOffset)
                {
                    continue;
                }
                checkpointsOut.clear();
                for (std::uint32_t index = 0;
                     index < count;
                     ++index)
                {
                    checkpointsOut.push_back(
                        le64(bytes + arrayOffset + index * 8U));
                }
                return true;
            }
            errorText = QStringLiteral("未找到有效 ReFS v3 SUPB。");
            return false;
        }

        bool pickCheckpointV1(
            const std::vector<std::uint64_t>& blocks,
            RefsCheckpoint& checkpointOut,
            QString& errorText)
        {
            bool found = false;
            for (const std::uint64_t blockNumber : blocks)
            {
                QByteArray block;
                QString ignored;
                if (!readV1Block(
                        blockNumber,
                        block,
                        ignored))
                {
                    continue;
                }
                RefsCheckpoint candidate;
                if (!parseCheckpoint(
                        block,
                        false,
                        candidate))
                {
                    continue;
                }
                if (!found
                    || candidate.sequence > checkpointOut.sequence)
                {
                    checkpointOut = candidate;
                    found = true;
                }
            }
            if (!found)
            {
                errorText = QStringLiteral(
                    "未找到有效 ReFS v1 checkpoint。");
            }
            return found;
        }

        bool pickCheckpointV3(
            const std::vector<std::uint64_t>& blocks,
            RefsCheckpoint& checkpointOut,
            QString& errorText)
        {
            bool found = false;
            for (const std::uint64_t blockNumber : blocks)
            {
                QByteArray block;
                QString ignored;
                bool read =
                    readV3PhysicalBlock(
                        blockNumber,
                        block,
                        ignored);
                if (!read || !hasSignature(dataPointer(block), "CHKP"))
                {
                    read = readV3VirtualBlock(
                        blockNumber,
                        block,
                        ignored);
                }
                if (!read)
                {
                    continue;
                }
                RefsCheckpoint candidate;
                if (!parseCheckpoint(block, true, candidate))
                {
                    continue;
                }
                if (!found
                    || candidate.sequence > checkpointOut.sequence)
                {
                    checkpointOut = candidate;
                    found = true;
                }
            }
            if (!found)
            {
                errorText = QStringLiteral(
                    "未找到有效 ReFS v3 CHKP。");
            }
            return found;
        }

        bool parseCheckpoint(
            const QByteArray& block,
            const bool v3,
            RefsCheckpoint& checkpointOut) const
        {
            const unsigned char* const bytes = dataPointer(block);
            if (v3 && !hasSignature(bytes, "CHKP"))
            {
                return false;
            }
            const std::uint32_t metadataHeader =
                v3 ? kV3MetadataHeaderSize : kV1MetadataHeaderSize;
            const std::uint32_t trailer = metadataHeader + 16U;
            const std::uint32_t countOffset =
                trailer + (v3 ? 0x30U : 0x18U);
            const std::uint32_t arrayOffset =
                trailer + (v3 ? 0x34U : 0x1CU);
            if (block.size()
                < static_cast<qsizetype>(arrayOffset))
            {
                return false;
            }
            RefsCheckpoint checkpoint;
            checkpoint.sequence = le64(bytes + trailer);
            const std::uint32_t count = std::min<std::uint32_t>(
                le32(bytes + countOffset),
                v3 ? 13U : 7U);
            if (arrayOffset
                    + static_cast<std::uint64_t>(count) * 4U
                > static_cast<std::uint64_t>(block.size()))
            {
                return false;
            }
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const std::uint32_t referenceOffset =
                    le32(bytes + arrayOffset + index * 4U);
                const std::uint32_t referenceSize =
                    v3
                        ? kV3BlockReferenceSize
                        : kV1BlockReferenceSize;
                if (referenceOffset == 0U
                    || referenceOffset > block.size()
                    || referenceSize
                        > static_cast<std::uint64_t>(block.size())
                            - referenceOffset)
                {
                    continue;
                }
                checkpoint.references[index] =
                    parseBlockReference(
                        bytes + referenceOffset,
                        v3);
                checkpoint.present[index] =
                    checkpoint.references[index].blocks[0U] != 0U;
            }
            checkpointOut = checkpoint;
            return true;
        }

        static RefsBlockReference parseBlockReference(
            const unsigned char* bytes,
            const bool v3)
        {
            RefsBlockReference reference;
            reference.blocks[0U] = le64(bytes);
            if (v3)
            {
                reference.blocks[1U] = le64(bytes + 8U);
                reference.blocks[2U] = le64(bytes + 16U);
                reference.blocks[3U] = le64(bytes + 24U);
            }
            return reference;
        }

        bool readV1Block(
            const std::uint64_t blockNumber,
            QByteArray& blockOut,
            QString& errorText) const
        {
            std::uint64_t offset = 0;
            if (!checkedMultiply(
                    blockNumber,
                    kV1MetadataBlockSize,
                    offset)
                || offset > m_reader.partitionLength()
                || kV1MetadataBlockSize
                    > m_reader.partitionLength() - offset)
            {
                errorText = QStringLiteral("ReFS v1 元数据块号越界。");
                return false;
            }
            return m_reader.read(
                offset,
                kV1MetadataBlockSize,
                blockOut,
                errorText);
        }

        bool readV3PhysicalBlock(
            const std::uint64_t physicalCluster,
            QByteArray& blockOut,
            QString& errorText) const
        {
            std::uint64_t offset = 0;
            if (!checkedMultiply(
                    physicalCluster,
                    m_clusterSize,
                    offset)
                || offset > m_reader.partitionLength()
                || m_metadataBlockSize
                    > m_reader.partitionLength() - offset)
            {
                errorText = QStringLiteral("ReFS v3 物理元数据块越界。");
                return false;
            }
            return m_reader.read(
                offset,
                m_metadataBlockSize,
                blockOut,
                errorText);
        }

        static void decodeVirtualCluster(
            const std::uint64_t virtualCluster,
            std::uint64_t& containerIdOut,
            std::uint64_t& offsetOut)
        {
            if ((virtualCluster >> 32U) != 0U)
            {
                containerIdOut = virtualCluster >> 32U;
                offsetOut = virtualCluster & 0xFFFFFFFFULL;
            }
            else
            {
                containerIdOut =
                    virtualCluster >> kV3ContainerShift;
                offsetOut =
                    virtualCluster & (kV3ContainerClusters - 1U);
            }
        }

        bool translateVirtualCluster(
            const std::uint64_t virtualCluster,
            std::uint64_t& physicalClusterOut,
            QString& errorText)
        {
            if (virtualCluster < kV3BootstrapClusters)
            {
                physicalClusterOut = virtualCluster;
                return true;
            }
            std::uint64_t containerId = 0;
            std::uint64_t withinContainer = 0;
            decodeVirtualCluster(
                virtualCluster,
                containerId,
                withinContainer);
            auto iterator = std::find_if(
                m_containers.cbegin(),
                m_containers.cend(),
                [&](const RefsContainer& entry)
                {
                    return entry.id == containerId;
                });
            if (iterator == m_containers.cend())
            {
                if (!discoverContainer(containerId, errorText))
                {
                    return false;
                }
                iterator = std::find_if(
                    m_containers.cbegin(),
                    m_containers.cend(),
                    [&](const RefsContainer& entry)
                    {
                        return entry.id == containerId;
                    });
            }
            if (iterator == m_containers.cend()
                || iterator->physicalStart
                    > std::numeric_limits<std::uint64_t>::max()
                        - withinContainer)
            {
                errorText = QStringLiteral(
                    "ReFS v3 容器映射不存在或发生溢出。");
                return false;
            }
            physicalClusterOut =
                iterator->physicalStart + withinContainer;
            return true;
        }

        bool discoverContainer(
            const std::uint64_t requestedContainer,
            QString& errorText)
        {
            const std::uint64_t partitionLength =
                m_reader.partitionLength();
            std::uint64_t offset = 0;
            while (offset + m_clusterSize <= partitionLength)
            {
                const std::uint32_t length =
                    static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            kContainerScanChunkBytes,
                            partitionLength - offset));
                if (length < m_clusterSize)
                {
                    break;
                }
                QByteArray chunk;
                QString readError;
                if (!m_reader.read(
                        offset,
                        length,
                        chunk,
                        readError))
                {
                    offset += length;
                    continue;
                }
                for (std::uint32_t within = 0;
                     within + kV3MetadataHeaderSize
                        <= static_cast<std::uint32_t>(chunk.size());
                     within += m_clusterSize)
                {
                    const unsigned char* const header =
                        dataPointer(chunk) + within;
                    if (!hasSignature(header, "SUPB")
                        && !hasSignature(header, "CHKP")
                        && !hasSignature(header, "MSB+"))
                    {
                        continue;
                    }
                    const std::uint64_t selfVirtual =
                        le64(header + 0x20U);
                    if (selfVirtual == 0U)
                    {
                        continue;
                    }
                    std::uint64_t containerId = 0;
                    std::uint64_t blockOffset = 0;
                    decodeVirtualCluster(
                        selfVirtual,
                        containerId,
                        blockOffset);
                    const std::uint64_t physicalCluster =
                        (offset + within) / m_clusterSize;
                    if (containerId == 0U
                        || blockOffset > physicalCluster)
                    {
                        continue;
                    }
                    const std::uint64_t physicalStart =
                        physicalCluster - blockOffset;
                    const auto existing = std::find_if(
                        m_containers.cbegin(),
                        m_containers.cend(),
                        [&](const RefsContainer& entry)
                        {
                            return entry.id == containerId;
                        });
                    if (existing == m_containers.cend())
                    {
                        m_containers.push_back(
                            RefsContainer{
                                containerId,
                                physicalStart});
                    }
                    if (containerId == requestedContainer)
                    {
                        return true;
                    }
                }
                offset += length;
            }
            errorText = QStringLiteral(
                "未能定位 ReFS v3 容器：%1")
                .arg(requestedContainer);
            return false;
        }

        bool readV3VirtualBlock(
            const std::uint64_t virtualCluster,
            QByteArray& blockOut,
            QString& errorText)
        {
            std::uint64_t physicalCluster = 0;
            if (!translateVirtualCluster(
                    virtualCluster,
                    physicalCluster,
                    errorText))
            {
                return false;
            }
            return readV3PhysicalBlock(
                physicalCluster,
                blockOut,
                errorText);
        }

        bool readReferencedBlock(
            const RefsBlockReference& reference,
            QByteArray& blockOut,
            std::uint64_t& physicalOffsetOut,
            QString& errorText)
        {
            if (reference.blocks[0U] == 0U)
            {
                errorText = QStringLiteral("ReFS BlockRef 为空。");
                return false;
            }
            if (!m_v3)
            {
                physicalOffsetOut =
                    reference.blocks[0U] * kV1MetadataBlockSize;
                return readV1Block(
                    reference.blocks[0U],
                    blockOut,
                    errorText);
            }
            std::uint64_t physicalCluster = 0;
            if (!translateVirtualCluster(
                    reference.blocks[0U],
                    physicalCluster,
                    errorText))
            {
                return false;
            }
            physicalOffsetOut = physicalCluster * m_clusterSize;
            return readV3PhysicalBlock(
                physicalCluster,
                blockOut,
                errorText);
        }

        bool parseNodeHeader(
            const QByteArray& block,
            RefsNodeHeader& headerOut,
            QString& errorText) const
        {
            const std::uint32_t bucket =
                m_v3
                    ? kV3MetadataHeaderSize
                    : kV1MetadataHeaderSize;
            const std::uint32_t headerSize =
                m_v3
                    ? kV3NodeHeaderSize
                    : kV1NodeHeaderSize;
            if (block.size()
                < static_cast<qsizetype>(bucket + 4U))
            {
                errorText = QStringLiteral("ReFS Ministore 节点被截断。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(block);
            if (m_v3 && !hasSignature(bytes, "MSB+"))
            {
                errorText = QStringLiteral("ReFS v3 节点签名不是 MSB+。");
                return false;
            }
            const std::uint32_t headerOffset =
                bucket + le32(bytes + bucket);
            if (headerOffset > block.size()
                || headerSize
                    > static_cast<std::uint64_t>(block.size())
                        - headerOffset)
            {
                errorText = QStringLiteral("ReFS 节点头偏移越界。");
                return false;
            }
            const unsigned char* const header =
                bytes + headerOffset;
            RefsNodeHeader parsed;
            parsed.headerOffset = headerOffset;
            parsed.dataStart =
                headerOffset + le32(header);
            parsed.dataEnd =
                headerOffset + le32(header + 4U);
            parsed.level = header[0x0CU];
            parsed.typeFlags = header[0x0DU];
            parsed.recordOffsetsStart =
                headerOffset + le32(header + 0x10U);
            parsed.recordCount = le32(header + 0x14U);
            if (parsed.dataStart > parsed.dataEnd
                || parsed.dataStart
                    < parsed.headerOffset + headerSize
                || parsed.dataEnd
                    > static_cast<std::uint32_t>(block.size())
                || parsed.recordOffsetsStart
                    > static_cast<std::uint32_t>(block.size())
                || parsed.recordCount > kMaximumRecordsPerNode
                || static_cast<std::uint64_t>(parsed.recordCount) * 4U
                    > static_cast<std::uint64_t>(block.size())
                        - parsed.recordOffsetsStart)
            {
                errorText = QStringLiteral("ReFS 节点几何参数无效。");
                return false;
            }
            headerOut = parsed;
            return true;
        }

        bool parseRecord(
            const QByteArray& block,
            const RefsNodeHeader& header,
            const std::uint32_t index,
            const std::uint64_t blockPhysicalOffset,
            RefsRecordView& recordOut,
            QString& errorText) const
        {
            if (index >= header.recordCount)
            {
                errorText = QStringLiteral("ReFS 记录索引越界。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(block);
            std::uint32_t relative = le32(
                bytes + header.recordOffsetsStart + index * 4U);
            if (m_v3)
            {
                relative &= 0xFFFFU;
            }
            const std::uint32_t recordOffset =
                header.headerOffset + relative;
            const std::uint32_t headerSize =
                m_v3
                    ? kV3RecordHeaderSize
                    : kV1RecordHeaderSize;
            if (recordOffset > block.size()
                || headerSize
                    > static_cast<std::uint64_t>(block.size())
                        - recordOffset)
            {
                errorText = QStringLiteral("ReFS 记录头越界。");
                return false;
            }
            const unsigned char* const record = bytes + recordOffset;
            const std::uint32_t recordSize = le32(record);
            const std::uint16_t keyOffset = le16(record + 4U);
            const std::uint16_t keySize = le16(record + 6U);
            const std::uint16_t flags = le16(record + 8U);
            const std::uint16_t valueOffset = le16(record + 0x0AU);
            const std::uint32_t valueSize =
                m_v3
                    ? le32(record + 0x0CU)
                    : le16(record + 0x0CU);
            if (recordSize < headerSize
                || recordSize
                    > static_cast<std::uint64_t>(block.size())
                        - recordOffset
                || keyOffset + static_cast<std::uint64_t>(keySize)
                    > recordSize
                || valueOffset
                        + static_cast<std::uint64_t>(valueSize)
                    > recordSize)
            {
                errorText = QStringLiteral("ReFS 记录键值范围越界。");
                return false;
            }
            RefsRecordView view;
            view.key = keySize == 0U
                ? nullptr
                : record + keyOffset;
            view.value = valueSize == 0U
                ? nullptr
                : record + valueOffset;
            view.keySize = keySize;
            view.valueSize = valueSize;
            view.recordOffset = recordOffset;
            view.flags = flags;
            view.valueAbsoluteOffset =
                m_reader.partitionOffset()
                + blockPhysicalOffset
                + recordOffset
                + valueOffset;
            recordOut = view;
            return true;
        }

        template<typename Callback>
        bool walkTree(
            const RefsBlockReference& root,
            Callback&& callback,
            std::vector<std::uint64_t>& visited,
            QString& errorText)
        {
            if (m_walkStopRequested)
            {
                return true;
            }
            const std::uint64_t identity = root.blocks[0U];
            if (visited.size() >= kMaximumTreeNodes
                || std::find(
                    visited.cbegin(),
                    visited.cend(),
                    identity) != visited.cend())
            {
                errorText = QStringLiteral(
                    "ReFS Ministore 树包含循环或节点数超过安全上限。");
                return false;
            }
            visited.push_back(identity);

            QByteArray block;
            std::uint64_t physicalOffset = 0;
            if (!readReferencedBlock(
                    root,
                    block,
                    physicalOffset,
                    errorText))
            {
                return false;
            }
            RefsNodeHeader header;
            if (!parseNodeHeader(block, header, errorText))
            {
                return false;
            }
            const bool v3Branch =
                m_v3 && (header.typeFlags & 0x01U) != 0U;
            for (std::uint32_t index = 0;
                 index < header.recordCount;
                 ++index)
            {
                RefsRecordView record;
                if (!parseRecord(
                        block,
                        header,
                        index,
                        physicalOffset,
                        record,
                        errorText))
                {
                    return false;
                }
                if ((record.flags & kRecordUnallocated) != 0U)
                {
                    continue;
                }
                const bool branch =
                    v3Branch
                    || (!m_v3
                        && (record.flags & kRecordEmbeddedNode) != 0U);
                if (branch)
                {
                    const std::uint32_t required =
                        m_v3
                            ? kV3BlockReferenceSize
                            : kV1BlockReferenceSize;
                    if (record.value == nullptr
                        || record.valueSize < required)
                    {
                        errorText = QStringLiteral(
                            "ReFS 分支记录缺少 BlockRef。");
                        return false;
                    }
                    const RefsBlockReference child =
                        parseBlockReference(record.value, m_v3);
                    if (!walkTree(
                            child,
                            callback,
                            visited,
                            errorText))
                    {
                        return false;
                    }
                    if (m_walkStopRequested)
                    {
                        return true;
                    }
                    continue;
                }
                if (!callback(record))
                {
                    m_walkStopRequested = true;
                    return true;
                }
            }
            return true;
        }

        bool objectLookup(
            const RefsBlockReference& objects,
            const std::uint64_t objectId,
            RefsBlockReference& referenceOut,
            QString& errorText)
        {
            bool found = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            if (!walkTree(
                    objects,
                    [&](const RefsRecordView& record)
                    {
                        if (record.key == nullptr
                            || record.keySize < 16U
                            || le64(record.key + 8U) != objectId)
                        {
                            return true;
                        }
                        const std::uint32_t valueOffset =
                            m_v3 ? 0x20U : 0U;
                        const std::uint32_t required =
                            valueOffset
                            + (m_v3
                                ? kV3BlockReferenceSize
                                : kV1BlockReferenceSize);
                        if (record.value == nullptr
                            || record.valueSize < required)
                        {
                            errorText = QStringLiteral(
                                "ReFS 对象记录缺少 BlockRef。");
                            return false;
                        }
                        referenceOut = parseBlockReference(
                            record.value + valueOffset,
                            m_v3);
                        found =
                            referenceOut.blocks[0U] != 0U;
                        return false;
                    },
                    visited,
                    errorText))
            {
                return false;
            }
            if (!errorText.isEmpty())
            {
                return false;
            }
            if (!found)
            {
                errorText = QStringLiteral(
                    "ReFS 对象表中不存在对象：%1")
                    .arg(objectId);
            }
            return found;
        }

        bool parseDirectoryEntry(
            const RefsRecordView& record,
            const std::uint64_t parentObjectId,
            RefsEntry& entryOut,
            QString& errorText)
        {
            if (record.key == nullptr
                || record.keySize < 4U
                || le16(record.key) != kDirectoryEntryRecord)
            {
                return false;
            }
            const std::uint16_t kind = le16(record.key + 2U);
            const std::uint32_t nameCharacters =
                (record.keySize - 4U) / 2U;
            RefsEntry entry;
            entry.name = decodeUtf16Le(
                record.key + 4U,
                nameCharacters);
            entry.parentObjectId = parentObjectId;
            entry.metadataOffset = record.valueAbsoluteOffset;
            if (kind == kDirectoryEntryKind)
            {
                const std::uint32_t minimum =
                    m_v3 ? 0x48U : 72U;
                if (record.value == nullptr
                    || record.valueSize < minimum)
                {
                    errorText = QStringLiteral(
                        "ReFS 子目录记录被截断。");
                    return false;
                }
                entry.directory = true;
                entry.objectId = le64(
                    record.value + (m_v3 ? 8U : 0U));
                entry.attributes = le32(record.value + 0x40U);
                if (!objectLookup(
                        m_objectsReference,
                        entry.objectId,
                        entry.directoryReference,
                        errorText))
                {
                    return false;
                }
            }
            else if (kind == kFileEntryKind
                || kind == kMetadataEntryKind)
            {
                entry.directory = false;
                if (m_v3)
                {
                    if (record.value == nullptr
                        || record.valueSize < 0x68U)
                    {
                        errorText = QStringLiteral(
                            "ReFS v3 文件记录被截断。");
                        return false;
                    }
                    entry.attributes = le32(record.value + 0x48U);
                    entry.dataSize = le64(record.value + 0x58U);
                    entry.allocatedSize = le64(record.value + 0x60U);
                }
                else
                {
                    if (record.value == nullptr
                        || record.valueSize < 0x28U + 128U)
                    {
                        errorText = QStringLiteral(
                            "ReFS v1 文件记录被截断。");
                        return false;
                    }
                    entry.attributes =
                        le32(record.value + 0x28U + 0x20U);
                    entry.dataSize =
                        le64(record.value + 0x28U + 0x40U);
                    entry.allocatedSize =
                        le64(record.value + 0x28U + 0x48U);
                }
                entry.value = QByteArray(
                    reinterpret_cast<const char*>(record.value),
                    static_cast<qsizetype>(record.valueSize));
                entry.objectId =
                    (parentObjectId << 32U)
                    ^ record.valueAbsoluteOffset;
            }
            else
            {
                return false;
            }
            entryOut = std::move(entry);
            return true;
        }

        bool enumerateDirectory(
            const RefsEntry& directory,
            const std::uint32_t maximumEntries,
            std::vector<RefsEntry>& entriesOut,
            std::uint64_t& scannedRecordsOut,
            bool& truncatedOut,
            QString& errorText)
        {
            entriesOut.clear();
            scannedRecordsOut = 0;
            truncatedOut = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            const bool walked = walkTree(
                directory.directoryReference,
                [&](const RefsRecordView& record)
                {
                    if (++scannedRecordsOut > kMaximumDirectoryRecords)
                    {
                        errorText = QStringLiteral(
                            "ReFS 目录记录数超过安全上限。");
                        return false;
                    }
                    if (record.key == nullptr
                        || record.keySize < 2U
                        || le16(record.key) != kDirectoryEntryRecord)
                    {
                        return true;
                    }
                    if (entriesOut.size() >= maximumEntries)
                    {
                        truncatedOut = true;
                        return false;
                    }
                    RefsEntry entry;
                    if (!parseDirectoryEntry(
                            record,
                            directory.objectId,
                            entry,
                            errorText))
                    {
                        return errorText.isEmpty();
                    }
                    entriesOut.push_back(std::move(entry));
                    return true;
                },
                visited,
                errorText);
            return walked && errorText.isEmpty();
        }

        bool findDirectoryChild(
            const RefsEntry& directory,
            const QString& requestedName,
            RefsEntry& childOut,
            QString& errorText)
        {
            bool found = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            if (!walkTree(
                    directory.directoryReference,
                    [&](const RefsRecordView& record)
                    {
                        if (record.key == nullptr
                            || record.keySize < 4U
                            || le16(record.key)
                                != kDirectoryEntryRecord)
                        {
                            return true;
                        }
                        const QString name = decodeUtf16Le(
                            record.key + 4U,
                            (record.keySize - 4U) / 2U);
                        if (QString::compare(
                                name,
                                requestedName,
                                Qt::CaseInsensitive) != 0)
                        {
                            return true;
                        }
                        if (!parseDirectoryEntry(
                                record,
                                directory.objectId,
                                childOut,
                                errorText))
                        {
                            return false;
                        }
                        found = true;
                        return false;
                    },
                    visited,
                    errorText))
            {
                return false;
            }
            if (!errorText.isEmpty())
            {
                return false;
            }
            if (!found)
            {
                errorText = QStringLiteral(
                    "ReFS 目录中不存在对象：%1")
                    .arg(requestedName);
            }
            return found;
        }

        bool loadRuns(
            const RefsEntry& file,
            std::vector<RefsRun>& runsOut,
            QString& errorText)
        {
            runsOut.clear();
            if (file.dataSize == 0U)
            {
                return true;
            }
            if (m_v3)
            {
                if (file.dataSize <= 0x10000U
                    && kV3ResidentDataOffset + file.dataSize
                        <= static_cast<std::uint64_t>(file.value.size()))
                {
                    runsOut.push_back(
                        RefsRun{
                            0U,
                            file.dataSize,
                            file.metadataOffset
                                + kV3ResidentDataOffset,
                            false,
                            true});
                    return true;
                }
                if (loadV3InlineRuns(file, runsOut))
                {
                    return translateV3Runs(runsOut, errorText);
                }
                if (!loadV3ChildRuns(file, runsOut, errorText))
                {
                    return false;
                }
                return translateV3Runs(runsOut, errorText);
            }
            return loadV1Runs(file, runsOut, errorText);
        }

        bool loadV1Runs(
            const RefsEntry& file,
            std::vector<RefsRun>& runsOut,
            QString& errorText) const
        {
            if (file.value.size()
                < static_cast<qsizetype>(
                    0x28U + 128U + kV1NodeHeaderSize))
            {
                errorText = QStringLiteral(
                    "ReFS v1 文件值缺少外层 extent 节点。");
                return false;
            }
            const unsigned char* const value =
                dataPointer(file.value);
            const std::uint32_t outerHeaderOffset = le32(value);
            RefsNodeHeader outer;
            if (!parseEmbeddedNodeHeader(
                    file.value,
                    outerHeaderOffset,
                    false,
                    outer,
                    errorText))
            {
                return false;
            }
            QByteArray embedded;
            for (std::uint32_t index = 0;
                 index < outer.recordCount;
                 ++index)
            {
                RefsRecordView record;
                if (!parseEmbeddedRecord(
                        file.value,
                        outer,
                        index,
                        false,
                        record,
                        errorText))
                {
                    return false;
                }
                if ((record.flags & kRecordEmbeddedNode) != 0U
                    && record.value != nullptr
                    && record.valueSize != 0U)
                {
                    embedded = QByteArray(
                        reinterpret_cast<const char*>(record.value),
                        static_cast<qsizetype>(record.valueSize));
                    break;
                }
            }
            if (embedded.isEmpty())
            {
                errorText = QStringLiteral(
                    "ReFS v1 文件值中不存在内嵌 extent 节点。");
                return false;
            }
            const std::uint32_t innerHeaderOffset =
                le32(dataPointer(embedded));
            RefsNodeHeader inner;
            if (!parseEmbeddedNodeHeader(
                    embedded,
                    innerHeaderOffset,
                    false,
                    inner,
                    errorText))
            {
                return false;
            }
            for (std::uint32_t index = 0;
                 index < inner.recordCount;
                 ++index)
            {
                RefsRecordView record;
                if (!parseEmbeddedRecord(
                        embedded,
                        inner,
                        index,
                        false,
                        record,
                        errorText))
                {
                    return false;
                }
                if ((record.flags
                        & (kRecordUnallocated
                            | kRecordEmbeddedNode)) != 0U
                    || record.value == nullptr
                    || record.valueSize < 24U)
                {
                    continue;
                }
                const std::uint64_t blockLength =
                    le64(record.value + 8U);
                if (blockLength == 0U)
                {
                    continue;
                }
                runsOut.push_back(
                    RefsRun{
                        le64(record.value) * kV1MetadataBlockSize,
                        blockLength * kV1MetadataBlockSize,
                        m_reader.partitionOffset()
                            + le64(record.value + 16U)
                                * kV1MetadataBlockSize,
                        le64(record.value + 16U) == 0U,
                        false});
            }
            if (runsOut.empty())
            {
                errorText = QStringLiteral(
                    "ReFS v1 文件 extent 列表为空。");
                return false;
            }
            sortRuns(runsOut);
            return true;
        }

        bool loadV3InlineRuns(
            const RefsEntry& file,
            std::vector<RefsRun>& runsOut) const
        {
            if (file.value.size()
                < static_cast<qsizetype>(
                    kV3ExtentHeaderOffset
                    + kV3NodeHeaderSize))
            {
                return false;
            }
            const unsigned char* const value =
                dataPointer(file.value);
            const unsigned char* const header =
                value + kV3ExtentHeaderOffset;
            const std::uint32_t offsetsStart =
                le32(header + 0x10U);
            const std::uint32_t count =
                le32(header + 0x14U);
            const std::uint64_t arrayOffset =
                kV3ExtentHeaderOffset
                + static_cast<std::uint64_t>(offsetsStart);
            if (count == 0U || count > 0x1000U
                || arrayOffset > static_cast<std::uint64_t>(file.value.size())
                || static_cast<std::uint64_t>(count) * 4U
                    > static_cast<std::uint64_t>(file.value.size())
                        - arrayOffset)
            {
                return false;
            }
            for (std::uint32_t index = 0;
                 index < count;
                 ++index)
            {
                const std::uint16_t relative = le16(
                    value + arrayOffset + index * 4U);
                const std::uint64_t runOffset =
                    kV3ExtentHeaderOffset
                    + static_cast<std::uint64_t>(relative);
                if (runOffset + kV3RunSize
                    > static_cast<std::uint64_t>(file.value.size()))
                {
                    continue;
                }
                const unsigned char* const run = value + runOffset;
                const std::uint64_t startVirtual = le64(run);
                const std::uint32_t clusterCount =
                    le32(run + 0x14U);
                if (clusterCount == 0U)
                {
                    continue;
                }
                runsOut.push_back(
                    RefsRun{
                        le64(run + 0x0CU) * m_clusterSize,
                        static_cast<std::uint64_t>(clusterCount)
                            * m_clusterSize,
                        startVirtual,
                        startVirtual == 0U
                            || startVirtual
                                == std::numeric_limits<std::uint64_t>::max(),
                        false});
            }
            sortRuns(runsOut);
            return !runsOut.empty();
        }

        bool loadV3ChildRuns(
            const RefsEntry& file,
            std::vector<RefsRun>& runsOut,
            QString& errorText)
        {
            RefsNodeHeader outer;
            if (!parseEmbeddedNodeHeader(
                    file.value,
                    kV3OuterNodeHeaderOffset,
                    false,
                    outer,
                    errorText))
            {
                return false;
            }
            RefsBlockReference childReference;
            bool foundChild = false;
            for (std::uint32_t index = 0;
                 index < outer.recordCount;
                 ++index)
            {
                RefsRecordView record;
                if (!parseEmbeddedRecord(
                        file.value,
                        outer,
                        index,
                        false,
                        record,
                        errorText))
                {
                    return false;
                }
                if ((record.flags & kRecordEmbeddedNode) != 0U
                    && record.value != nullptr
                    && record.valueSize >= kV3BlockReferenceSize)
                {
                    childReference =
                        parseBlockReference(record.value, true);
                    foundChild =
                        childReference.blocks[0U] != 0U;
                    if (foundChild)
                    {
                        break;
                    }
                }
            }
            if (!foundChild)
            {
                errorText = QStringLiteral(
                    "ReFS v3 文件值中不存在子 extent 节点。");
                return false;
            }

            QByteArray child;
            std::uint64_t physicalOffset = 0;
            if (!readReferencedBlock(
                    childReference,
                    child,
                    physicalOffset,
                    errorText))
            {
                return false;
            }
            RefsNodeHeader header;
            if (!parseNodeHeader(child, header, errorText))
            {
                return false;
            }
            for (std::uint32_t index = 0;
                 index < header.recordCount;
                 ++index)
            {
                RefsRecordView record;
                if (!parseRecord(
                        child,
                        header,
                        index,
                        physicalOffset,
                        record,
                        errorText))
                {
                    return false;
                }
                if ((record.flags
                        & (kRecordUnallocated
                            | kRecordEmbeddedNode)) != 0U
                    || record.key == nullptr
                    || record.keySize < 12U
                    || le32(record.key + 8U) != 0x80U
                    || record.value == nullptr
                    || record.valueSize <= 0xB0U)
                {
                    continue;
                }
                std::uint32_t offset = 0xB0U;
                while (offset + kV3RunSize <= record.valueSize)
                {
                    const unsigned char* const run =
                        record.value + offset;
                    const std::uint64_t startVirtual = le64(run);
                    const std::uint32_t clusterCount =
                        le32(run + 0x14U);
                    if (clusterCount == 0U)
                    {
                        break;
                    }
                    runsOut.push_back(
                        RefsRun{
                            le64(run + 0x0CU) * m_clusterSize,
                            static_cast<std::uint64_t>(clusterCount)
                                * m_clusterSize,
                            startVirtual,
                            startVirtual == 0U
                                || startVirtual
                                    == std::numeric_limits<std::uint64_t>::max(),
                            false});
                    offset += kV3RunSize;
                }
                break;
            }
            if (runsOut.empty())
            {
                errorText = QStringLiteral(
                    "ReFS v3 子节点未提供数据 extent。");
                return false;
            }
            sortRuns(runsOut);
            return true;
        }

        bool translateV3Runs(
            std::vector<RefsRun>& runs,
            QString& errorText)
        {
            for (RefsRun& run : runs)
            {
                if (run.sparse)
                {
                    run.physicalOffset = 0U;
                    continue;
                }
                std::uint64_t physicalCluster = 0;
                if (!translateVirtualCluster(
                        run.physicalOffset,
                        physicalCluster,
                        errorText))
                {
                    return false;
                }
                run.physicalOffset =
                    m_reader.partitionOffset()
                    + physicalCluster * m_clusterSize;
            }
            return true;
        }

        static bool parseEmbeddedNodeHeader(
            const QByteArray& buffer,
            const std::uint32_t headerOffset,
            const bool v3Header,
            RefsNodeHeader& headerOut,
            QString& errorText)
        {
            const std::uint32_t headerSize =
                v3Header
                    ? kV3NodeHeaderSize
                    : kV1NodeHeaderSize;
            if (headerOffset > buffer.size()
                || headerSize
                    > static_cast<std::uint64_t>(buffer.size())
                        - headerOffset)
            {
                errorText = QStringLiteral(
                    "ReFS 内嵌节点头偏移越界。");
                return false;
            }
            const unsigned char* const header =
                dataPointer(buffer) + headerOffset;
            RefsNodeHeader parsed;
            parsed.headerOffset = headerOffset;
            parsed.dataStart =
                headerOffset + le32(header);
            parsed.dataEnd =
                headerOffset + le32(header + 4U);
            parsed.level = header[0x0CU];
            parsed.typeFlags = header[0x0DU];
            parsed.recordOffsetsStart =
                headerOffset + le32(header + 0x10U);
            parsed.recordCount = le32(header + 0x14U);
            if (parsed.dataStart > parsed.dataEnd
                || parsed.dataStart
                    < parsed.headerOffset + headerSize
                || parsed.dataEnd
                    > static_cast<std::uint32_t>(buffer.size())
                || parsed.recordOffsetsStart
                    > static_cast<std::uint32_t>(buffer.size())
                || parsed.recordCount > 0x1000U
                || static_cast<std::uint64_t>(parsed.recordCount) * 4U
                    > static_cast<std::uint64_t>(buffer.size())
                        - parsed.recordOffsetsStart)
            {
                errorText = QStringLiteral(
                    "ReFS 内嵌节点几何参数无效。");
                return false;
            }
            headerOut = parsed;
            return true;
        }

        static bool parseEmbeddedRecord(
            const QByteArray& buffer,
            const RefsNodeHeader& header,
            const std::uint32_t index,
            const bool v3Record,
            RefsRecordView& recordOut,
            QString& errorText)
        {
            if (index >= header.recordCount)
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(buffer);
            std::uint32_t relative = le32(
                bytes + header.recordOffsetsStart + index * 4U);
            if (v3Record)
            {
                relative &= 0xFFFFU;
            }
            std::uint32_t recordOffset =
                header.headerOffset + relative;
            if (recordOffset < header.headerOffset)
            {
                recordOffset += header.headerOffset;
            }
            const std::uint32_t recordHeaderSize =
                v3Record
                    ? kV3RecordHeaderSize
                    : kV1RecordHeaderSize;
            if (recordOffset > buffer.size()
                || recordHeaderSize
                    > static_cast<std::uint64_t>(buffer.size())
                        - recordOffset)
            {
                errorText = QStringLiteral(
                    "ReFS 内嵌记录头越界。");
                return false;
            }
            const unsigned char* const record =
                bytes + recordOffset;
            const std::uint32_t recordSize = le32(record);
            const std::uint16_t keyOffset = le16(record + 4U);
            const std::uint16_t keySize = le16(record + 6U);
            const std::uint16_t valueOffset = le16(record + 0x0AU);
            const std::uint32_t valueSize =
                v3Record
                    ? le32(record + 0x0CU)
                    : le16(record + 0x0CU);
            if (recordSize < recordHeaderSize
                || recordSize
                    > static_cast<std::uint64_t>(buffer.size())
                        - recordOffset
                || keyOffset + static_cast<std::uint64_t>(keySize)
                    > recordSize
                || valueOffset + static_cast<std::uint64_t>(valueSize)
                    > recordSize)
            {
                errorText = QStringLiteral(
                    "ReFS 内嵌记录键值范围越界。");
                return false;
            }
            RefsRecordView view;
            view.key = keySize == 0U
                ? nullptr
                : record + keyOffset;
            view.value = valueSize == 0U
                ? nullptr
                : record + valueOffset;
            view.keySize = keySize;
            view.valueSize = valueSize;
            view.recordOffset = recordOffset;
            view.flags = le16(record + 8U);
            recordOut = view;
            return true;
        }

        static void sortRuns(std::vector<RefsRun>& runs)
        {
            std::sort(
                runs.begin(),
                runs.end(),
                [](const RefsRun& left, const RefsRun& right)
                {
                    return left.logicalOffset < right.logicalOffset;
                });
        }

        std::vector<ks::misc::RawFileExtent> describeRuns(
            const RefsEntry& file,
            const std::vector<RefsRun>& runs) const
        {
            std::vector<ks::misc::RawFileExtent> extents;
            for (const RefsRun& run : runs)
            {
                if (extents.size()
                        >= ks::misc::rawfs::kMaximumExtentCount
                    || run.logicalOffset >= file.dataSize)
                {
                    break;
                }
                ks::misc::RawFileExtent extent;
                extent.logicalOffset = run.logicalOffset;
                extent.lengthBytes = std::min<std::uint64_t>(
                    run.length,
                    file.dataSize - run.logicalOffset);
                extent.sparse = run.sparse;
                if (!run.sparse)
                {
                    extent.absoluteOffset = run.physicalOffset;
                }
                extent.physicalMappingExact = !run.sparse;
                extents.push_back(extent);
            }
            return extents;
        }

        void fillEntry(
            const RefsEntry& source,
            const QString& parentPath,
            const std::vector<RefsRun>& runs,
            ks::misc::RawFileEntry& entry) const
        {
            entry.name = source.name;
            entry.fullPath =
                ks::misc::rawfs::childPath(
                    parentPath,
                    source.name);
            entry.type = source.directory
                ? ks::misc::RawFileObjectType::Directory
                : ks::misc::RawFileObjectType::RegularFile;
            entry.objectId = source.objectId;
            entry.parentObjectId = source.parentObjectId;
            entry.fileSizeBytes = source.dataSize;
            entry.allocatedSizeBytes = source.allocatedSize;
            entry.metadataOffset = source.metadataOffset;
            entry.modeOrFlags = source.attributes;
            entry.extents = describeRuns(source, runs);
            entry.extentsTruncated =
                runs.size() > ks::misc::rawfs::kMaximumExtentCount;
        }

        const ks::misc::rawfs::VolumeReader& m_reader;
        bool m_v3 = false;
        bool m_walkStopRequested = false;
        std::uint8_t m_majorVersion = 0;
        std::uint8_t m_minorVersion = 0;
        std::uint32_t m_bytesPerSector = 0;
        std::uint32_t m_sectorsPerCluster = 0;
        std::uint32_t m_clusterSize = 0;
        std::uint32_t m_metadataBlockSize = 0;
        RefsBlockReference m_objectsReference;
        RefsBlockReference m_rootDirectoryReference;
        std::vector<RefsContainer> m_containers;
    };

    ks::misc::RawDirectoryResult makeDirectoryResult(
        const QString& path)
    {
        ks::misc::RawDirectoryResult result;
        result.fileSystem =
            ks::misc::ForensicFileSystemKind::ReFs;
        result.fileSystemName =
            ks::misc::DiskFileSystemForensics::fileSystemName(
                result.fileSystem);
        result.requestedPath = path;
        return result;
    }

    ks::misc::RawFileReadResult makeFileResult(
        const QString& path,
        const std::uint64_t offset)
    {
        ks::misc::RawFileReadResult result;
        result.fileSystem =
            ks::misc::ForensicFileSystemKind::ReFs;
        result.fileSystemName =
            ks::misc::DiskFileSystemForensics::fileSystemName(
                result.fileSystem);
        result.filePath = path;
        result.requestedOffset = offset;
        return result;
    }
}

namespace ks::misc::rawfs
{
    RawDirectoryResult listRefs(
        const VolumeReader& reader,
        const QString& path,
        const std::uint32_t maximumEntries)
    {
        RawDirectoryResult result = makeDirectoryResult(path);
        RefsVolume volume(reader);
        if (!volume.mount(result.errorText))
        {
            return result;
        }
        RefsEntry directory;
        if (!volume.resolvePath(
                path,
                directory,
                result.canonicalPath,
                result.errorText))
        {
            return result;
        }
        result.directoryObjectId = directory.objectId;
        if (!volume.listDirectory(
                directory,
                result.canonicalPath,
                maximumEntries,
                result))
        {
            return result;
        }
        result.success = true;
        return result;
    }

    RawFileReadResult readRefs(
        const VolumeReader& reader,
        const QString& path,
        const std::uint64_t offset,
        const std::uint32_t length)
    {
        struct FileState
        {
            RefsVolume volume;
            RefsEntry data;
            RawFileReadResult basis;
            explicit FileState(const VolumeReader& reader) : volume(reader) {}
        };
        auto state = reader.fileSessionState<FileState>();
        if (!state)
        {
            state = std::make_shared<FileState>(reader);
            auto& volume = state->volume;
            auto& file = state->data;

            RawFileReadResult result = makeFileResult(path, offset);
            if (!volume.mount(result.errorText))
            {
                return result;
            }
            QString canonicalPath;
            if (!volume.resolvePath(
                    path,
                    file,
                    canonicalPath,
                    result.errorText))
            {
                return result;
            }
            result.filePath = canonicalPath;
            result.fileSizeBytes = file.dataSize;
            state->basis = result;
            reader.setFileSessionState(state);
        }
        RawFileReadResult result = state->basis;
        result.requestedOffset = offset;
        auto& volume = state->volume;
        auto& file = state->data;
        if (!volume.readFile(file, offset, length, result))
        {
            return result;
        }
        result.success = true;
        return result;
    }
}
