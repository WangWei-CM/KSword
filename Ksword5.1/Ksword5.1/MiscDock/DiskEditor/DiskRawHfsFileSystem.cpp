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
    constexpr std::uint16_t kHfsSignature = 0x4244U;
    constexpr std::uint16_t kHfsPlusSignature = 0x482BU;
    constexpr std::uint16_t kHfsXSignature = 0x4858U;
    constexpr std::uint32_t kRootFolderId = 2U;
    constexpr std::uint32_t kExtentsFileId = 3U;
    constexpr std::uint8_t kDataForkType = 0U;
    constexpr std::int8_t kLeafNodeKind = -1;
    constexpr std::uint32_t kMaximumBTreeNodes = 1U << 20U;
    constexpr std::uint32_t kMaximumCatalogRecords = 4U * 1024U * 1024U;

    std::uint16_t be16(const unsigned char* bytes)
    {
        return (static_cast<std::uint16_t>(bytes[0]) << 8U)
            | static_cast<std::uint16_t>(bytes[1]);
    }

    std::uint32_t be32(const unsigned char* bytes)
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24U)
            | (static_cast<std::uint32_t>(bytes[1]) << 16U)
            | (static_cast<std::uint32_t>(bytes[2]) << 8U)
            | static_cast<std::uint32_t>(bytes[3]);
    }

    std::uint64_t be64(const unsigned char* bytes)
    {
        return (static_cast<std::uint64_t>(be32(bytes)) << 32U)
            | static_cast<std::uint64_t>(be32(bytes + 4U));
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

    std::uint32_t alignEven(const std::uint32_t value)
    {
        return (value + 1U) & ~1U;
    }

    QString decodeHfsPlusName(
        const unsigned char* bytes,
        const std::uint16_t characterCount)
    {
        QString result;
        result.reserve(characterCount);
        for (std::uint16_t index = 0; index < characterCount; ++index)
        {
            result.append(QChar(be16(bytes + index * 2U)));
        }
        return result;
    }

    QString decodeMacRoman(
        const unsigned char* bytes,
        const std::uint8_t byteCount)
    {
        static constexpr std::array<std::uint16_t, 128U> kHighCharacters{
            0x00C4U, 0x00C5U, 0x00C7U, 0x00C9U, 0x00D1U, 0x00D6U,
            0x00DCU, 0x00E1U, 0x00E0U, 0x00E2U, 0x00E4U, 0x00E3U,
            0x00E5U, 0x00E7U, 0x00E9U, 0x00E8U, 0x00EAU, 0x00EBU,
            0x00EDU, 0x00ECU, 0x00EEU, 0x00EFU, 0x00F1U, 0x00F3U,
            0x00F2U, 0x00F4U, 0x00F6U, 0x00F5U, 0x00FAU, 0x00F9U,
            0x00FBU, 0x00FCU, 0x2020U, 0x00B0U, 0x00A2U, 0x00A3U,
            0x00A7U, 0x2022U, 0x00B6U, 0x00DFU, 0x00AEU, 0x00A9U,
            0x2122U, 0x00B4U, 0x00A8U, 0x2260U, 0x00C6U, 0x00D8U,
            0x221EU, 0x00B1U, 0x2264U, 0x2265U, 0x00A5U, 0x00B5U,
            0x2202U, 0x2211U, 0x220FU, 0x03C0U, 0x222BU, 0x00AAU,
            0x00BAU, 0x03A9U, 0x00E6U, 0x00F8U, 0x00BFU, 0x00A1U,
            0x00ACU, 0x221AU, 0x0192U, 0x2248U, 0x2206U, 0x00ABU,
            0x00BBU, 0x2026U, 0x00A0U, 0x00C0U, 0x00C3U, 0x00D5U,
            0x0152U, 0x0153U, 0x2013U, 0x2014U, 0x201CU, 0x201DU,
            0x2018U, 0x2019U, 0x00F7U, 0x25CAU, 0x00FFU, 0x0178U,
            0x2044U, 0x20ACU, 0x2039U, 0x203AU, 0xFB01U, 0xFB02U,
            0x2021U, 0x00B7U, 0x201AU, 0x201EU, 0x2030U, 0x00C2U,
            0x00CAU, 0x00C1U, 0x00CBU, 0x00C8U, 0x00CDU, 0x00CEU,
            0x00CFU, 0x00CCU, 0x00D3U, 0x00D4U, 0xF8FFU, 0x00D2U,
            0x00DAU, 0x00DBU, 0x00D9U, 0x0131U, 0x02C6U, 0x02DCU,
            0x00AFU, 0x02D8U, 0x02D9U, 0x02DAU, 0x00B8U, 0x02DDU,
            0x02DBU, 0x02C7U
        };

        QString result;
        result.reserve(byteCount);
        for (std::uint8_t index = 0; index < byteCount; ++index)
        {
            const std::uint8_t value = bytes[index];
            result.append(
                value < 0x80U
                    ? QChar(value)
                    : QChar(kHighCharacters[value - 0x80U]));
        }
        return result;
    }

    struct HfsExtent
    {
        std::uint64_t logicalBlock = 0;
        std::uint32_t startBlock = 0;
        std::uint32_t blockCount = 0;
    };

    struct HfsFork
    {
        std::uint32_t fileId = 0;
        std::uint8_t forkType = kDataForkType;
        std::uint64_t logicalSize = 0;
        std::uint64_t totalBlocks = 0;
        std::vector<HfsExtent> extents;
        bool overflowApplied = false;
    };

    struct HfsTreeHeader
    {
        std::uint32_t rootNode = 0;
        std::uint32_t leafRecords = 0;
        std::uint32_t firstLeafNode = 0;
        std::uint32_t lastLeafNode = 0;
        std::uint32_t totalNodes = 0;
        std::uint16_t nodeSize = 0;
        std::uint8_t keyCompareType = 0;
    };

    struct HfsCatalogRecord
    {
        QString name;
        std::uint32_t parentId = 0;
        std::uint32_t objectId = 0;
        std::uint32_t valence = 0;
        std::uint32_t modeOrFlags = 0;
        std::uint64_t metadataOffset = 0;
        ks::misc::RawFileObjectType type =
            ks::misc::RawFileObjectType::Unknown;
        HfsFork dataFork;
    };

    struct HfsOverflowRecord
    {
        std::uint32_t fileId = 0;
        std::uint8_t forkType = 0;
        std::uint64_t logicalBlock = 0;
        std::vector<HfsExtent> extents;
    };

    class HfsVolume final
    {
    public:
        HfsVolume(
            const ks::misc::rawfs::VolumeReader& reader,
            const ks::misc::ForensicFileSystemKind expectedKind)
            : m_reader(reader),
              m_expectedKind(expectedKind)
        {
        }

        bool mount(QString& errorText)
        {
            QByteArray header;
            if (!m_reader.read(1024U, 512U, header, errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(header);
            const std::uint16_t signature = be16(bytes);
            if (signature == kHfsPlusSignature
                || signature == kHfsXSignature)
            {
                if (m_expectedKind !=
                    ks::misc::ForensicFileSystemKind::HfsPlus)
                {
                    errorText = QStringLiteral(
                        "卷签名为 HFS+，与请求的文件系统类型不匹配。");
                    return false;
                }
                m_plus = true;
                m_caseSensitive = signature == kHfsXSignature;
                if (!mountPlus(bytes, errorText))
                {
                    return false;
                }
            }
            else if (signature == kHfsSignature)
            {
                if (m_expectedKind !=
                    ks::misc::ForensicFileSystemKind::Hfs)
                {
                    errorText = QStringLiteral(
                        "卷签名为经典 HFS，与请求的文件系统类型不匹配。");
                    return false;
                }
                if (!mountClassic(bytes, errorText))
                {
                    return false;
                }
            }
            else
            {
                errorText = QStringLiteral("HFS 卷头签名不匹配。");
                return false;
            }

            if (!readTreeHeader(m_extentsFork, m_extentsTree, errorText))
            {
                errorText = QStringLiteral(
                    "无法读取 HFS Extents Overflow B-tree：%1")
                    .arg(errorText);
                return false;
            }
            if (!loadOverflowIndex(errorText))
            {
                return false;
            }
            applyOverflowExtents(m_catalogFork);
            if (!readTreeHeader(m_catalogFork, m_catalogTree, errorText))
            {
                errorText = QStringLiteral(
                    "无法读取 HFS Catalog B-tree：%1").arg(errorText);
                return false;
            }
            return true;
        }

        bool resolvePath(
            const QString& path,
            HfsCatalogRecord& recordOut,
            QString& canonicalPathOut,
            QString& errorText)
        {
            HfsCatalogRecord current;
            current.objectId = kRootFolderId;
            current.type = ks::misc::RawFileObjectType::Directory;
            current.name = QStringLiteral("\\");
            canonicalPathOut = QStringLiteral("\\");

            const QStringList components =
                ks::misc::rawfs::normalizePath(path).split(
                    QChar('\\'),
                    Qt::SkipEmptyParts);
            for (const QString& component : components)
            {
                if (current.type !=
                    ks::misc::RawFileObjectType::Directory)
                {
                    errorText = QStringLiteral(
                        "路径中间对象不是 HFS 目录：%1")
                        .arg(canonicalPathOut);
                    return false;
                }
                HfsCatalogRecord child;
                if (!findCatalogChild(
                        current.objectId,
                        component,
                        child,
                        errorText))
                {
                    return false;
                }
                current = child;
                canonicalPathOut = ks::misc::rawfs::childPath(
                    canonicalPathOut,
                    current.name);
            }

            recordOut = current;
            return true;
        }

        bool listDirectory(
            const HfsCatalogRecord& directory,
            const QString& canonicalPath,
            const std::uint32_t maximumEntries,
            ks::misc::RawDirectoryResult& result)
        {
            if (directory.type !=
                ks::misc::RawFileObjectType::Directory)
            {
                result.errorText = QStringLiteral("目标对象不是 HFS 目录。");
                return false;
            }

            QString errorText;
            bool stopped = false;
            const bool scanned = scanCatalog(
                [&](HfsCatalogRecord& record)
                {
                    if (record.parentId != directory.objectId)
                    {
                        return true;
                    }
                    if (result.entries.size() >= maximumEntries)
                    {
                        result.truncated = true;
                        stopped = true;
                        return false;
                    }
                    if (record.type !=
                        ks::misc::RawFileObjectType::Directory)
                    {
                        applyOverflowExtents(record.dataFork);
                    }
                    ks::misc::RawFileEntry entry;
                    fillEntry(
                        record,
                        canonicalPath,
                        entry);
                    result.entries.push_back(std::move(entry));
                    return true;
                },
                result.scannedRecords,
                errorText);
            if (!scanned && !stopped)
            {
                result.errorText = errorText;
                return false;
            }
            return true;
        }

        bool readFile(
            HfsCatalogRecord& record,
            const std::uint64_t offset,
            const std::uint32_t length,
            ks::misc::RawFileReadResult& result)
        {
            if (record.type == ks::misc::RawFileObjectType::Directory)
            {
                result.errorText = QStringLiteral("目标对象是目录，不能按文件读取。");
                return false;
            }
            if (offset >= record.dataFork.logicalSize)
            {
                result.endOfFile = true;
                return true;
            }

            applyOverflowExtents(record.dataFork);
            const std::uint32_t boundedLength =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        length,
                        record.dataFork.logicalSize - offset));
            QString errorText;
            if (!readForkBytes(
                    record.dataFork,
                    offset,
                    boundedLength,
                    result.bytes,
                    errorText))
            {
                result.errorText = errorText;
                return false;
            }
            result.endOfFile =
                offset + boundedLength >= record.dataFork.logicalSize;
            result.extents = describeFork(record.dataFork);
            result.extentsTruncated =
                record.dataFork.extents.size()
                > ks::misc::rawfs::kMaximumExtentCount;
            return true;
        }

    private:
        bool mountPlus(
            const unsigned char* header,
            QString& errorText)
        {
            m_blockSize = be32(header + 0x28U);
            m_totalBlocks = be32(header + 0x2CU);
            if (!validateGeometry(errorText))
            {
                return false;
            }
            m_extentsFork =
                parsePlusFork(header + 0xC0U, kExtentsFileId);
            m_catalogFork =
                parsePlusFork(header + 0x110U, 4U);
            if (!validateMetadataFork(m_extentsFork, errorText)
                || !validateMetadataFork(m_catalogFork, errorText))
            {
                return false;
            }
            return true;
        }

        bool mountClassic(
            const unsigned char* header,
            QString& errorText)
        {
            const std::uint16_t allocationBlockCount =
                be16(header + 0x12U);
            m_blockSize = be32(header + 0x14U);
            m_totalBlocks = allocationBlockCount;
            m_allocationStart =
                static_cast<std::uint64_t>(be16(header + 0x1CU))
                * 512U;
            if (!validateGeometry(errorText)
                || m_allocationStart >= m_reader.partitionLength())
            {
                errorText = QStringLiteral("经典 HFS 分配区几何参数无效。");
                return false;
            }

            m_extentsFork.fileId = kExtentsFileId;
            m_extentsFork.logicalSize = be32(header + 0x7EU);
            parseClassicExtentArray(
                header + 0x82U,
                0U,
                m_extentsFork.extents);
            m_extentsFork.totalBlocks =
                countExtentBlocks(m_extentsFork.extents);

            m_catalogFork.fileId = 4U;
            m_catalogFork.logicalSize = be32(header + 0x8EU);
            parseClassicExtentArray(
                header + 0x92U,
                0U,
                m_catalogFork.extents);
            m_catalogFork.totalBlocks =
                countExtentBlocks(m_catalogFork.extents);
            if (!validateMetadataFork(m_extentsFork, errorText)
                || !validateMetadataFork(m_catalogFork, errorText))
            {
                return false;
            }
            return true;
        }

        bool validateGeometry(QString& errorText) const
        {
            if (!isPowerOfTwo(m_blockSize)
                || m_blockSize < 512U
                || m_blockSize > 1024U * 1024U
                || m_totalBlocks == 0U)
            {
                errorText = QStringLiteral("HFS 分配块大小或块总数无效。");
                return false;
            }
            std::uint64_t volumeBytes = 0;
            if (!checkedMultiply(
                    m_totalBlocks,
                    m_blockSize,
                    volumeBytes)
                || volumeBytes
                    > m_reader.partitionLength() - m_allocationStart)
            {
                errorText = QStringLiteral("HFS 卷几何范围越过所选分区。");
                return false;
            }
            return true;
        }

        bool validateMetadataFork(
            const HfsFork& fork,
            QString& errorText) const
        {
            if (fork.logicalSize == 0U || fork.extents.empty())
            {
                errorText = QStringLiteral("HFS 元数据分支为空。");
                return false;
            }
            for (const HfsExtent& extent : fork.extents)
            {
                if (extent.blockCount == 0U
                    || extent.startBlock >= m_totalBlocks
                    || extent.blockCount
                        > m_totalBlocks - extent.startBlock)
                {
                    errorText = QStringLiteral(
                        "HFS 元数据分支包含越界区段。");
                    return false;
                }
            }
            return true;
        }

        HfsFork parsePlusFork(
            const unsigned char* bytes,
            const std::uint32_t fileId) const
        {
            HfsFork fork;
            fork.fileId = fileId;
            fork.logicalSize = be64(bytes);
            fork.totalBlocks = be32(bytes + 0x0CU);
            std::uint64_t logicalBlock = 0;
            for (std::uint32_t index = 0; index < 8U; ++index)
            {
                const std::uint32_t startBlock =
                    be32(bytes + 0x10U + index * 8U);
                const std::uint32_t blockCount =
                    be32(bytes + 0x14U + index * 8U);
                if (blockCount == 0U)
                {
                    continue;
                }
                fork.extents.push_back(
                    HfsExtent{
                        logicalBlock,
                        startBlock,
                        blockCount});
                logicalBlock += blockCount;
            }
            return fork;
        }

        static void parseClassicExtentArray(
            const unsigned char* bytes,
            const std::uint64_t firstLogicalBlock,
            std::vector<HfsExtent>& extentsOut)
        {
            std::uint64_t logicalBlock = firstLogicalBlock;
            for (std::uint32_t index = 0; index < 3U; ++index)
            {
                const std::uint32_t startBlock =
                    be16(bytes + index * 4U);
                const std::uint32_t blockCount =
                    be16(bytes + index * 4U + 2U);
                if (blockCount == 0U)
                {
                    continue;
                }
                extentsOut.push_back(
                    HfsExtent{
                        logicalBlock,
                        startBlock,
                        blockCount});
                logicalBlock += blockCount;
            }
        }

        static std::uint64_t countExtentBlocks(
            const std::vector<HfsExtent>& extents)
        {
            std::uint64_t total = 0;
            for (const HfsExtent& extent : extents)
            {
                total += extent.blockCount;
            }
            return total;
        }

        bool logicalBlockToRelativeOffset(
            const HfsFork& fork,
            const std::uint64_t logicalBlock,
            std::uint64_t& relativeOffsetOut,
            std::uint64_t& contiguousBytesOut) const
        {
            for (const HfsExtent& extent : fork.extents)
            {
                if (logicalBlock < extent.logicalBlock
                    || logicalBlock
                        >= extent.logicalBlock + extent.blockCount)
                {
                    continue;
                }
                const std::uint64_t within =
                    logicalBlock - extent.logicalBlock;
                const std::uint64_t physicalBlock =
                    static_cast<std::uint64_t>(extent.startBlock)
                    + within;
                std::uint64_t blockOffset = 0;
                if (!checkedMultiply(
                        physicalBlock,
                        m_blockSize,
                        blockOffset)
                    || blockOffset
                        > std::numeric_limits<std::uint64_t>::max()
                            - m_allocationStart)
                {
                    return false;
                }
                relativeOffsetOut = m_allocationStart + blockOffset;
                contiguousBytesOut =
                    (static_cast<std::uint64_t>(extent.blockCount)
                        - within)
                    * m_blockSize;
                return true;
            }
            return false;
        }

        bool readForkBytes(
            const HfsFork& fork,
            const std::uint64_t logicalOffset,
            const std::uint32_t length,
            QByteArray& bytesOut,
            QString& errorText) const
        {
            bytesOut.clear();
            if (length == 0U)
            {
                return true;
            }
            if (logicalOffset > fork.logicalSize
                || length > fork.logicalSize - logicalOffset)
            {
                errorText = QStringLiteral("HFS 分支读取范围越过逻辑末尾。");
                return false;
            }

            bytesOut.resize(static_cast<qsizetype>(length));
            std::uint64_t completed = 0;
            while (completed < length)
            {
                const std::uint64_t current = logicalOffset + completed;
                const std::uint64_t logicalBlock = current / m_blockSize;
                const std::uint64_t withinBlock = current % m_blockSize;
                std::uint64_t relativeOffset = 0;
                std::uint64_t contiguousBytes = 0;
                if (!logicalBlockToRelativeOffset(
                        fork,
                        logicalBlock,
                        relativeOffset,
                        contiguousBytes)
                    || contiguousBytes <= withinBlock)
                {
                    bytesOut.clear();
                    errorText = QStringLiteral(
                        "HFS 分支的区段映射不完整，无法安全读取。");
                    return false;
                }
                const std::uint32_t chunkLength =
                    static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            length - completed,
                            contiguousBytes - withinBlock));
                QByteArray chunk;
                if (!m_reader.read(
                        relativeOffset + withinBlock,
                        chunkLength,
                        chunk,
                        errorText))
                {
                    bytesOut.clear();
                    return false;
                }
                std::copy(
                    chunk.cbegin(),
                    chunk.cend(),
                    bytesOut.begin()
                        + static_cast<qsizetype>(completed));
                completed += chunkLength;
            }
            return true;
        }

        bool readTreeHeader(
            const HfsFork& fork,
            HfsTreeHeader& headerOut,
            QString& errorText) const
        {
            QByteArray prefix;
            if (!readForkBytes(fork, 0U, 64U, prefix, errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(prefix);
            if (static_cast<std::int8_t>(bytes[8U]) != 1)
            {
                errorText = QStringLiteral("HFS B-tree 头节点类型无效。");
                return false;
            }
            const unsigned char* const record = bytes + 14U;
            HfsTreeHeader header;
            header.rootNode = be32(record + 2U);
            header.leafRecords = be32(record + 6U);
            header.firstLeafNode = be32(record + 10U);
            header.lastLeafNode = be32(record + 14U);
            header.nodeSize = be16(record + 18U);
            header.totalNodes = be32(record + 22U);
            header.keyCompareType = record[37U];
            if (!isPowerOfTwo(header.nodeSize)
                || header.nodeSize < 512U
                || header.nodeSize > 65536U
                || header.totalNodes == 0U
                || header.totalNodes > kMaximumBTreeNodes
                || header.firstLeafNode >= header.totalNodes
                || header.lastLeafNode >= header.totalNodes)
            {
                errorText = QStringLiteral("HFS B-tree 几何参数无效。");
                return false;
            }
            std::uint64_t treeBytes = 0;
            if (!checkedMultiply(
                    header.totalNodes,
                    header.nodeSize,
                    treeBytes)
                || treeBytes > fork.logicalSize)
            {
                errorText = QStringLiteral("HFS B-tree 节点范围越过分支。");
                return false;
            }
            headerOut = header;
            return true;
        }

        bool readTreeNode(
            const HfsFork& fork,
            const HfsTreeHeader& tree,
            const std::uint32_t nodeNumber,
            QByteArray& nodeOut,
            QString& errorText) const
        {
            if (nodeNumber >= tree.totalNodes)
            {
                errorText = QStringLiteral("HFS B-tree 节点号越界。");
                return false;
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(nodeNumber) * tree.nodeSize;
            return readForkBytes(
                fork,
                offset,
                tree.nodeSize,
                nodeOut,
                errorText);
        }

        static bool nodeRecordRanges(
            const QByteArray& node,
            std::vector<std::pair<std::uint16_t, std::uint16_t>>& rangesOut,
            QString& errorText)
        {
            rangesOut.clear();
            if (node.size() < 16)
            {
                errorText = QStringLiteral("HFS B-tree 节点被截断。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(node);
            const std::uint16_t recordCount = be16(bytes + 10U);
            const std::uint32_t tableBytes =
                (static_cast<std::uint32_t>(recordCount) + 1U) * 2U;
            if (tableBytes > static_cast<std::uint32_t>(node.size()) - 14U)
            {
                errorText = QStringLiteral("HFS B-tree 记录表越界。");
                return false;
            }

            std::vector<std::uint16_t> offsets;
            offsets.reserve(static_cast<std::size_t>(recordCount) + 1U);
            for (std::uint32_t index = 0;
                 index <= recordCount;
                 ++index)
            {
                const std::uint32_t tableOffset =
                    static_cast<std::uint32_t>(node.size())
                    - (index + 1U) * 2U;
                offsets.push_back(be16(bytes + tableOffset));
            }
            for (std::uint32_t index = 0; index < recordCount; ++index)
            {
                const std::uint16_t begin = offsets[index];
                const std::uint16_t end = offsets[index + 1U];
                if (begin < 14U || end <= begin
                    || end
                        > static_cast<std::uint32_t>(node.size())
                            - tableBytes)
                {
                    errorText = QStringLiteral(
                        "HFS B-tree 记录边界无效。");
                    rangesOut.clear();
                    return false;
                }
                rangesOut.emplace_back(begin, end);
            }
            return true;
        }

        bool loadOverflowIndex(QString& errorText)
        {
            m_overflowRecords.clear();
            std::uint32_t nodeNumber = m_extentsTree.firstLeafNode;
            std::vector<std::uint32_t> visited;
            while (nodeNumber != 0U)
            {
                if (visited.size() >= m_extentsTree.totalNodes
                    || std::find(
                        visited.cbegin(),
                        visited.cend(),
                        nodeNumber) != visited.cend())
                {
                    errorText = QStringLiteral(
                        "HFS Extents Overflow 叶链包含循环。");
                    return false;
                }
                visited.push_back(nodeNumber);

                QByteArray node;
                if (!readTreeNode(
                        m_extentsFork,
                        m_extentsTree,
                        nodeNumber,
                        node,
                        errorText))
                {
                    return false;
                }
                const unsigned char* const bytes = dataPointer(node);
                if (static_cast<std::int8_t>(bytes[8U])
                    != kLeafNodeKind)
                {
                    errorText = QStringLiteral(
                        "HFS Extents Overflow 叶链指向非叶节点。");
                    return false;
                }

                std::vector<std::pair<std::uint16_t, std::uint16_t>> ranges;
                if (!nodeRecordRanges(node, ranges, errorText))
                {
                    return false;
                }
                for (const auto& [begin, end] : ranges)
                {
                    HfsOverflowRecord record;
                    if (parseOverflowRecord(
                            bytes + begin,
                            end - begin,
                            record))
                    {
                        m_overflowRecords.push_back(std::move(record));
                    }
                }
                nodeNumber = be32(bytes);
            }
            std::sort(
                m_overflowRecords.begin(),
                m_overflowRecords.end(),
                [](const HfsOverflowRecord& left,
                   const HfsOverflowRecord& right)
                {
                    if (left.fileId != right.fileId)
                    {
                        return left.fileId < right.fileId;
                    }
                    if (left.forkType != right.forkType)
                    {
                        return left.forkType < right.forkType;
                    }
                    return left.logicalBlock < right.logicalBlock;
                });
            return true;
        }

        bool parseOverflowRecord(
            const unsigned char* record,
            const std::uint32_t recordLength,
            HfsOverflowRecord& recordOut) const
        {
            if (m_plus)
            {
                if (recordLength < 12U)
                {
                    return false;
                }
                const std::uint16_t keyLength = be16(record);
                const std::uint32_t dataOffset =
                    alignEven(static_cast<std::uint32_t>(keyLength) + 2U);
                if (keyLength < 10U
                    || dataOffset + 64U > recordLength)
                {
                    return false;
                }
                HfsOverflowRecord parsed;
                parsed.forkType = record[2U];
                parsed.fileId = be32(record + 4U);
                parsed.logicalBlock = be32(record + 8U);
                std::uint64_t logical = parsed.logicalBlock;
                for (std::uint32_t index = 0; index < 8U; ++index)
                {
                    const std::uint32_t startBlock =
                        be32(record + dataOffset + index * 8U);
                    const std::uint32_t blockCount =
                        be32(record + dataOffset + index * 8U + 4U);
                    if (blockCount == 0U)
                    {
                        continue;
                    }
                    parsed.extents.push_back(
                        HfsExtent{logical, startBlock, blockCount});
                    logical += blockCount;
                }
                if (parsed.extents.empty())
                {
                    return false;
                }
                recordOut = std::move(parsed);
                return true;
            }

            if (recordLength < 20U)
            {
                return false;
            }
            const std::uint8_t keyLength = record[0U];
            const std::uint32_t dataOffset =
                alignEven(static_cast<std::uint32_t>(keyLength) + 1U);
            if (keyLength < 7U || dataOffset + 12U > recordLength)
            {
                return false;
            }
            HfsOverflowRecord parsed;
            parsed.forkType = record[1U];
            parsed.fileId = be32(record + 2U);
            parsed.logicalBlock = be16(record + 6U);
            parseClassicExtentArray(
                record + dataOffset,
                parsed.logicalBlock,
                parsed.extents);
            if (parsed.extents.empty())
            {
                return false;
            }
            recordOut = std::move(parsed);
            return true;
        }

        void applyOverflowExtents(HfsFork& fork) const
        {
            if (fork.overflowApplied)
            {
                return;
            }
            for (const HfsOverflowRecord& record : m_overflowRecords)
            {
                if (record.fileId != fork.fileId
                    || record.forkType != fork.forkType)
                {
                    continue;
                }
                for (const HfsExtent& candidate : record.extents)
                {
                    const bool duplicate = std::any_of(
                        fork.extents.cbegin(),
                        fork.extents.cend(),
                        [&](const HfsExtent& existing)
                        {
                            return existing.logicalBlock
                                    == candidate.logicalBlock
                                && existing.startBlock
                                    == candidate.startBlock
                                && existing.blockCount
                                    == candidate.blockCount;
                        });
                    if (!duplicate)
                    {
                        fork.extents.push_back(candidate);
                    }
                }
            }
            std::sort(
                fork.extents.begin(),
                fork.extents.end(),
                [](const HfsExtent& left, const HfsExtent& right)
                {
                    return left.logicalBlock < right.logicalBlock;
                });
            fork.overflowApplied = true;
        }

        template<typename Callback>
        bool scanCatalog(
            Callback&& callback,
            std::uint64_t& scannedRecordsOut,
            QString& errorText)
        {
            scannedRecordsOut = 0;
            std::uint32_t nodeNumber = m_catalogTree.firstLeafNode;
            std::vector<std::uint32_t> visited;
            while (nodeNumber != 0U)
            {
                if (visited.size() >= m_catalogTree.totalNodes
                    || std::find(
                        visited.cbegin(),
                        visited.cend(),
                        nodeNumber) != visited.cend())
                {
                    errorText = QStringLiteral(
                        "HFS Catalog 叶链包含循环。");
                    return false;
                }
                visited.push_back(nodeNumber);

                QByteArray node;
                if (!readTreeNode(
                        m_catalogFork,
                        m_catalogTree,
                        nodeNumber,
                        node,
                        errorText))
                {
                    return false;
                }
                const unsigned char* const bytes = dataPointer(node);
                if (static_cast<std::int8_t>(bytes[8U])
                    != kLeafNodeKind)
                {
                    errorText = QStringLiteral(
                        "HFS Catalog 叶链指向非叶节点。");
                    return false;
                }
                std::vector<std::pair<std::uint16_t, std::uint16_t>> ranges;
                if (!nodeRecordRanges(node, ranges, errorText))
                {
                    return false;
                }
                for (const auto& [begin, end] : ranges)
                {
                    if (++scannedRecordsOut > kMaximumCatalogRecords)
                    {
                        errorText = QStringLiteral(
                            "HFS Catalog 记录数超过安全上限。");
                        return false;
                    }
                    HfsCatalogRecord record;
                    if (!parseCatalogRecord(
                            bytes + begin,
                            end - begin,
                            static_cast<std::uint64_t>(nodeNumber)
                                * m_catalogTree.nodeSize
                                + begin,
                            record))
                    {
                        continue;
                    }
                    if (!callback(record))
                    {
                        return false;
                    }
                }
                nodeNumber = be32(bytes);
            }
            return true;
        }

        bool parseCatalogRecord(
            const unsigned char* record,
            const std::uint32_t recordLength,
            const std::uint64_t catalogLogicalOffset,
            HfsCatalogRecord& recordOut) const
        {
            if (m_plus)
            {
                return parsePlusCatalogRecord(
                    record,
                    recordLength,
                    catalogLogicalOffset,
                    recordOut);
            }
            return parseClassicCatalogRecord(
                record,
                recordLength,
                catalogLogicalOffset,
                recordOut);
        }

        bool parsePlusCatalogRecord(
            const unsigned char* record,
            const std::uint32_t recordLength,
            const std::uint64_t catalogLogicalOffset,
            HfsCatalogRecord& recordOut) const
        {
            if (recordLength < 10U)
            {
                return false;
            }
            const std::uint16_t keyLength = be16(record);
            const std::uint32_t dataOffset =
                alignEven(static_cast<std::uint32_t>(keyLength) + 2U);
            const std::uint16_t characterCount = be16(record + 6U);
            if (keyLength < 6U
                || static_cast<std::uint32_t>(characterCount) * 2U
                    > keyLength - 6U
                || dataOffset + 2U > recordLength)
            {
                return false;
            }

            const unsigned char* const data = record + dataOffset;
            const std::uint16_t recordType = be16(data);
            if (recordType != 1U && recordType != 2U)
            {
                return false;
            }
            const std::uint32_t required =
                recordType == 1U ? 88U : 248U;
            if (dataOffset + required > recordLength)
            {
                return false;
            }

            HfsCatalogRecord parsed;
            parsed.parentId = be32(record + 2U);
            parsed.name =
                decodeHfsPlusName(record + 8U, characterCount);
            parsed.objectId = be32(data + 8U);
            parsed.modeOrFlags =
                static_cast<std::uint32_t>(be16(data + 2U)) << 16U;
            parsed.modeOrFlags |= be16(data + 0x2AU);
            parsed.valence = recordType == 1U ? be32(data + 4U) : 0U;
            parsed.metadataOffset =
                catalogRecordAbsoluteOffset(catalogLogicalOffset);
            if (recordType == 1U)
            {
                parsed.type = ks::misc::RawFileObjectType::Directory;
            }
            else
            {
                const std::uint16_t mode = be16(data + 0x2AU);
                parsed.type = objectTypeForMode(mode);
                parsed.dataFork =
                    parsePlusFork(data + 0x58U, parsed.objectId);
            }
            recordOut = std::move(parsed);
            return true;
        }

        bool parseClassicCatalogRecord(
            const unsigned char* record,
            const std::uint32_t recordLength,
            const std::uint64_t catalogLogicalOffset,
            HfsCatalogRecord& recordOut) const
        {
            if (recordLength < 8U)
            {
                return false;
            }
            const std::uint8_t keyLength = record[0U];
            const std::uint32_t dataOffset =
                alignEven(static_cast<std::uint32_t>(keyLength) + 1U);
            const std::uint8_t nameLength = record[6U];
            if (keyLength < 6U || nameLength > keyLength - 6U
                || dataOffset + 1U > recordLength)
            {
                return false;
            }

            const unsigned char* const data = record + dataOffset;
            std::uint8_t recordType = data[0U] >> 4U;
            if (recordType != 1U && recordType != 2U)
            {
                recordType = data[0U];
            }
            const std::uint32_t required =
                recordType == 1U ? 70U : 102U;
            if ((recordType != 1U && recordType != 2U)
                || dataOffset + required > recordLength)
            {
                return false;
            }

            HfsCatalogRecord parsed;
            parsed.parentId = be32(record + 2U);
            parsed.name = decodeMacRoman(record + 7U, nameLength);
            parsed.metadataOffset =
                catalogRecordAbsoluteOffset(catalogLogicalOffset);
            if (recordType == 1U)
            {
                parsed.type = ks::misc::RawFileObjectType::Directory;
                parsed.valence = be16(data + 4U);
                parsed.objectId = be32(data + 6U);
            }
            else
            {
                parsed.type = ks::misc::RawFileObjectType::RegularFile;
                parsed.objectId = be32(data + 0x14U);
                parsed.dataFork.fileId = parsed.objectId;
                parsed.dataFork.logicalSize = be32(data + 0x1AU);
                parsed.dataFork.totalBlocks =
                    (static_cast<std::uint64_t>(be32(data + 0x1EU))
                        + m_blockSize - 1U)
                    / m_blockSize;
                parseClassicExtentArray(
                    data + 0x4EU,
                    0U,
                    parsed.dataFork.extents);
            }
            recordOut = std::move(parsed);
            return true;
        }

        std::uint64_t catalogRecordAbsoluteOffset(
            const std::uint64_t logicalOffset) const
        {
            const std::uint64_t logicalBlock = logicalOffset / m_blockSize;
            const std::uint64_t withinBlock = logicalOffset % m_blockSize;
            std::uint64_t relativeOffset = 0;
            std::uint64_t contiguousBytes = 0;
            if (!logicalBlockToRelativeOffset(
                    m_catalogFork,
                    logicalBlock,
                    relativeOffset,
                    contiguousBytes)
                || contiguousBytes <= withinBlock)
            {
                return 0U;
            }
            return m_reader.partitionOffset()
                + relativeOffset + withinBlock;
        }

        bool findCatalogChild(
            const std::uint32_t parentId,
            const QString& requestedName,
            HfsCatalogRecord& childOut,
            QString& errorText)
        {
            bool found = false;
            bool stopped = false;
            std::uint64_t scannedRecords = 0;
            const bool scanned = scanCatalog(
                [&](HfsCatalogRecord& record)
                {
                    if (record.parentId == parentId
                        && namesEqual(record.name, requestedName))
                    {
                        childOut = record;
                        found = true;
                        stopped = true;
                        return false;
                    }
                    return true;
                },
                scannedRecords,
                errorText);
            if (!scanned && !stopped)
            {
                return false;
            }
            if (!found)
            {
                errorText = QStringLiteral(
                    "HFS 目录中不存在对象：%1").arg(requestedName);
                return false;
            }
            return true;
        }

        bool namesEqual(
            const QString& left,
            const QString& right) const
        {
            return QString::compare(
                left,
                right,
                m_caseSensitive
                    ? Qt::CaseSensitive
                    : Qt::CaseInsensitive) == 0;
        }

        static ks::misc::RawFileObjectType objectTypeForMode(
            const std::uint16_t mode)
        {
            switch (mode & 0xF000U)
            {
            case 0x4000U:
                return ks::misc::RawFileObjectType::Directory;
            case 0x8000U:
                return ks::misc::RawFileObjectType::RegularFile;
            case 0xA000U:
                return ks::misc::RawFileObjectType::SymbolicLink;
            default:
                return ks::misc::RawFileObjectType::Special;
            }
        }

        std::vector<ks::misc::RawFileExtent> describeFork(
            const HfsFork& fork) const
        {
            std::vector<ks::misc::RawFileExtent> extents;
            extents.reserve(
                std::min<std::size_t>(
                    fork.extents.size(),
                    ks::misc::rawfs::kMaximumExtentCount));
            for (const HfsExtent& source : fork.extents)
            {
                if (extents.size()
                    >= ks::misc::rawfs::kMaximumExtentCount)
                {
                    break;
                }
                const std::uint64_t logicalOffset =
                    source.logicalBlock * m_blockSize;
                if (logicalOffset >= fork.logicalSize)
                {
                    continue;
                }
                ks::misc::RawFileExtent extent;
                extent.logicalOffset = logicalOffset;
                extent.absoluteOffset =
                    m_reader.partitionOffset()
                    + m_allocationStart
                    + static_cast<std::uint64_t>(source.startBlock)
                        * m_blockSize;
                extent.lengthBytes = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(source.blockCount)
                        * m_blockSize,
                    fork.logicalSize - logicalOffset);
                extent.physicalMappingExact = true;
                extents.push_back(extent);
            }
            return extents;
        }

        void fillEntry(
            const HfsCatalogRecord& record,
            const QString& parentPath,
            ks::misc::RawFileEntry& entry) const
        {
            entry.name = record.name;
            entry.fullPath =
                ks::misc::rawfs::childPath(parentPath, record.name);
            entry.type = record.type;
            entry.objectId = record.objectId;
            entry.parentObjectId = record.parentId;
            entry.fileSizeBytes = record.dataFork.logicalSize;
            entry.allocatedSizeBytes =
                record.dataFork.totalBlocks * m_blockSize;
            entry.metadataOffset = record.metadataOffset;
            entry.modeOrFlags = record.modeOrFlags;
            entry.extents = describeFork(record.dataFork);
            entry.extentsTruncated =
                record.dataFork.extents.size()
                > ks::misc::rawfs::kMaximumExtentCount;
        }

        const ks::misc::rawfs::VolumeReader& m_reader;
        ks::misc::ForensicFileSystemKind m_expectedKind =
            ks::misc::ForensicFileSystemKind::Unknown;
        bool m_plus = false;
        bool m_caseSensitive = false;
        std::uint32_t m_blockSize = 0;
        std::uint64_t m_totalBlocks = 0;
        std::uint64_t m_allocationStart = 0;
        HfsFork m_extentsFork;
        HfsFork m_catalogFork;
        HfsTreeHeader m_extentsTree;
        HfsTreeHeader m_catalogTree;
        std::vector<HfsOverflowRecord> m_overflowRecords;
    };

    ks::misc::RawDirectoryResult makeDirectoryResult(
        const ks::misc::ForensicFileSystemKind kind,
        const QString& path)
    {
        ks::misc::RawDirectoryResult result;
        result.fileSystem = kind;
        result.fileSystemName =
            ks::misc::DiskFileSystemForensics::fileSystemName(kind);
        result.requestedPath = path;
        return result;
    }

    ks::misc::RawFileReadResult makeFileResult(
        const ks::misc::ForensicFileSystemKind kind,
        const QString& path,
        const std::uint64_t offset)
    {
        ks::misc::RawFileReadResult result;
        result.fileSystem = kind;
        result.fileSystemName =
            ks::misc::DiskFileSystemForensics::fileSystemName(kind);
        result.filePath = path;
        result.requestedOffset = offset;
        return result;
    }
}

namespace ks::misc::rawfs
{
    RawDirectoryResult listHfs(
        const VolumeReader& reader,
        const ForensicFileSystemKind expectedKind,
        const QString& path,
        const std::uint32_t maximumEntries)
    {
        RawDirectoryResult result =
            makeDirectoryResult(expectedKind, path);
        HfsVolume volume(reader, expectedKind);
        if (!volume.mount(result.errorText))
        {
            return result;
        }

        HfsCatalogRecord directory;
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

    RawFileReadResult readHfs(
        const VolumeReader& reader,
        const ForensicFileSystemKind expectedKind,
        const QString& path,
        const std::uint64_t offset,
        const std::uint32_t length)
    {
        struct FileState
        {
            HfsVolume volume;
            HfsCatalogRecord data;
            RawFileReadResult basis;
            explicit FileState(const VolumeReader& reader, ForensicFileSystemKind kind) : volume(reader, kind) {}
        };
        auto state = reader.fileSessionState<FileState>();
        if (!state)
        {
            state = std::make_shared<FileState>(reader, expectedKind);
            auto& volume = state->volume;
            auto& record = state->data;

            RawFileReadResult result =
                makeFileResult(expectedKind, path, offset);
            if (!volume.mount(result.errorText))
            {
                return result;
            }

            QString canonicalPath;
            if (!volume.resolvePath(
                    path,
                    record,
                    canonicalPath,
                    result.errorText))
            {
                return result;
            }
            result.filePath = canonicalPath;
            result.fileSizeBytes = record.dataFork.logicalSize;
            state->basis = result;
            reader.setFileSessionState(state);
        }
        RawFileReadResult result = state->basis;
        result.requestedOffset = offset;
        auto& volume = state->volume;
        auto& record = state->data;
        if (!volume.readFile(record, offset, length, result))
        {
            return result;
        }
        result.success = true;
        return result;
    }
}
