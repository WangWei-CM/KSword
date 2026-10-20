#include "DiskRawFileSystemInternal.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace
{
    constexpr std::uint64_t kSuperblockOffset = 64U * 1024U;
    constexpr std::uint32_t kSuperblockSize = 4096U;
    constexpr std::uint32_t kSystemChunkArrayOffset = 0x32BU;
    constexpr std::uint32_t kSystemChunkArrayCapacity = 2048U;
    constexpr std::uint32_t kTreeHeaderSize = 101U;
    constexpr std::uint32_t kLeafItemSize = 25U;
    constexpr std::uint32_t kNodePointerSize = 33U;
    constexpr std::uint32_t kChunkHeaderSize = 48U;
    constexpr std::uint32_t kChunkStripeSize = 32U;
    constexpr std::uint32_t kDirItemHeaderSize = 30U;
    constexpr std::uint32_t kFileExtentHeaderSize = 21U;
    constexpr std::uint32_t kFileExtentRegularSize = 53U;
    constexpr std::uint64_t kFsTreeObjectId = 5U;
    constexpr std::uint64_t kRootDirectoryObjectId = 256U;
    constexpr std::uint8_t kInodeItemKey = 1U;
    constexpr std::uint8_t kDirectoryIndexKey = 96U;
    constexpr std::uint8_t kFileExtentDataKey = 108U;
    constexpr std::uint8_t kRootItemKey = 132U;
    constexpr std::uint8_t kChunkItemKey = 228U;
    constexpr std::uint64_t kBlockGroupRaid0 = 0x08U;
    constexpr std::uint64_t kBlockGroupRaid10 = 0x40U;
    constexpr std::uint64_t kBlockGroupRaid5 = 0x80U;
    constexpr std::uint64_t kBlockGroupRaid6 = 0x100U;
    constexpr std::uint8_t kExtentInline = 0U;
    constexpr std::uint8_t kExtentRegular = 1U;
    constexpr std::uint8_t kExtentPreallocated = 2U;
    constexpr std::uint32_t kMaximumVisitedTreeBlocks = 1U << 20U;
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

    bool checkedAdd(
        const std::uint64_t left,
        const std::uint64_t right,
        std::uint64_t& resultOut)
    {
        if (right > std::numeric_limits<std::uint64_t>::max() - left)
        {
            return false;
        }
        resultOut = left + right;
        return true;
    }

    struct BtrfsKey
    {
        std::uint64_t objectId = 0;
        std::uint8_t type = 0;
        std::uint64_t offset = 0;
    };

    int compareKey(const BtrfsKey& left, const BtrfsKey& right)
    {
        if (left.objectId != right.objectId)
        {
            return left.objectId < right.objectId ? -1 : 1;
        }
        if (left.type != right.type)
        {
            return left.type < right.type ? -1 : 1;
        }
        if (left.offset != right.offset)
        {
            return left.offset < right.offset ? -1 : 1;
        }
        return 0;
    }

    BtrfsKey parseKey(const unsigned char* bytes)
    {
        return BtrfsKey{
            le64(bytes),
            bytes[8U],
            le64(bytes + 9U)};
    }

    struct BtrfsChunk
    {
        std::uint64_t logical = 0;
        std::uint64_t length = 0;
        std::uint64_t physical = 0;
        std::uint64_t type = 0;
        std::uint64_t deviceId = 0;
    };

    struct BtrfsInode
    {
        std::uint64_t number = 0;
        std::uint64_t size = 0;
        std::uint64_t allocated = 0;
        std::uint64_t flags = 0;
        std::uint64_t metadataOffset = 0;
        std::uint32_t mode = 0;
    };

    struct BtrfsDirectoryChild
    {
        QString name;
        std::uint64_t inode = 0;
        std::uint8_t type = 0;
    };

    struct BtrfsFileSegment
    {
        std::uint64_t logicalOffset = 0;
        std::uint64_t logicalLength = 0;
        std::uint64_t diskLogical = 0;
        std::uint64_t diskLength = 0;
        std::uint64_t inlineAbsolute = 0;
        QByteArray inlineBytes;
        std::uint8_t compression = 0;
        std::uint8_t encryption = 0;
        std::uint16_t otherEncoding = 0;
        bool sparse = false;
        bool preallocated = false;
    };

    struct BtrfsLeafItem
    {
        BtrfsKey key;
        const unsigned char* data = nullptr;
        std::uint32_t size = 0;
        std::uint64_t absoluteOffset = 0;
    };

    class BtrfsVolume final
    {
    public:
        explicit BtrfsVolume(
            const ks::misc::rawfs::VolumeReader& reader)
            : m_reader(reader)
        {
        }

        bool mount(QString& errorText)
        {
            QByteArray superblock;
            if (!m_reader.read(
                    kSuperblockOffset,
                    kSuperblockSize,
                    superblock,
                    errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(superblock);
            if (QByteArray(
                    reinterpret_cast<const char*>(bytes + 0x40U),
                    8) != QByteArrayLiteral("_BHRfS_M"))
            {
                errorText = QStringLiteral("Btrfs 超级块魔数不匹配。");
                return false;
            }

            m_totalBytes = le64(bytes + 0x70U);
            m_deviceCount = le64(bytes + 0x88U);
            m_sectorSize = le32(bytes + 0x90U);
            m_nodeSize = le32(bytes + 0x94U);
            m_rootTreeLogical = le64(bytes + 0x50U);
            m_chunkTreeLogical = le64(bytes + 0x58U);
            m_currentDeviceId = le64(bytes + 0xC9U);
            const std::uint32_t systemArraySize =
                le32(bytes + 0xA0U);
            if (!isPowerOfTwo(m_sectorSize)
                || !isPowerOfTwo(m_nodeSize)
                || m_sectorSize < 512U
                || m_sectorSize > 65536U
                || m_nodeSize < 1024U
                || m_nodeSize > 65536U
                || m_totalBytes == 0U
                || m_totalBytes > m_reader.partitionLength()
                || m_deviceCount == 0U
                || systemArraySize > kSystemChunkArrayCapacity)
            {
                errorText = QStringLiteral("Btrfs 超级块几何参数无效。");
                return false;
            }

            if (!parseSystemChunkArray(
                    bytes + kSystemChunkArrayOffset,
                    systemArraySize,
                    errorText))
            {
                return false;
            }
            if (m_chunks.empty())
            {
                errorText = QStringLiteral("Btrfs 系统块组映射为空。");
                return false;
            }

            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            if (!walkTreeRange(
                    m_chunkTreeLogical,
                    BtrfsKey{0U, 0U, 0U},
                    BtrfsKey{
                        std::numeric_limits<std::uint64_t>::max(),
                        std::numeric_limits<std::uint8_t>::max(),
                        std::numeric_limits<std::uint64_t>::max()},
                    [&](const BtrfsLeafItem& item)
                    {
                        if (item.key.type == kChunkItemKey)
                        {
                            if (!parseChunk(
                                item.key.offset,
                                item.data,
                                item.size,
                                errorText))
                            {
                                return false;
                            }
                        }
                        return true;
                    },
                    visited,
                    errorText)
                || !errorText.isEmpty())
            {
                errorText = QStringLiteral(
                    "无法遍历 Btrfs Chunk tree：%1").arg(errorText);
                return false;
            }
            normalizeChunkMap();

            bool foundFsTree = false;
            visited.clear();
            m_walkStopRequested = false;
            if (!walkTreeRange(
                    m_rootTreeLogical,
                    BtrfsKey{kFsTreeObjectId, kRootItemKey, 0U},
                    BtrfsKey{
                        kFsTreeObjectId,
                        kRootItemKey,
                        std::numeric_limits<std::uint64_t>::max()},
                    [&](const BtrfsLeafItem& item)
                    {
                        if (item.key.objectId != kFsTreeObjectId
                            || item.key.type != kRootItemKey
                            || item.size < 239U)
                        {
                            return true;
                        }
                        m_fsTreeLogical = le64(item.data + 0xB0U);
                        foundFsTree = m_fsTreeLogical != 0U;
                        return !foundFsTree;
                    },
                    visited,
                    errorText))
            {
                return false;
            }
            if (!foundFsTree)
            {
                errorText = QStringLiteral(
                    "Btrfs Root tree 中不存在默认文件系统树。");
                return false;
            }
            return true;
        }

        bool resolvePath(
            const QString& path,
            BtrfsInode& inodeOut,
            QString& canonicalPathOut,
            QString& errorText)
        {
            BtrfsInode current;
            if (!loadInode(
                    kRootDirectoryObjectId,
                    current,
                    errorText))
            {
                return false;
            }
            canonicalPathOut = QStringLiteral("\\");
            const QStringList components =
                ks::misc::rawfs::normalizePath(path).split(
                    QChar('\\'),
                    Qt::SkipEmptyParts);
            for (const QString& component : components)
            {
                if ((current.mode & 0xF000U) != 0x4000U)
                {
                    errorText = QStringLiteral(
                        "路径中间对象不是 Btrfs 目录：%1")
                        .arg(canonicalPathOut);
                    return false;
                }
                BtrfsDirectoryChild child;
                if (!findDirectoryChild(
                        current.number,
                        component,
                        child,
                        errorText)
                    || !loadInode(child.inode, current, errorText))
                {
                    return false;
                }
                canonicalPathOut =
                    ks::misc::rawfs::childPath(
                        canonicalPathOut,
                        child.name);
            }
            inodeOut = current;
            return true;
        }

        bool listDirectory(
            const BtrfsInode& directory,
            const QString& canonicalPath,
            const std::uint32_t maximumEntries,
            ks::misc::RawDirectoryResult& result)
        {
            if ((directory.mode & 0xF000U) != 0x4000U)
            {
                result.errorText = QStringLiteral("目标对象不是 Btrfs 目录。");
                return false;
            }

            std::vector<BtrfsDirectoryChild> children;
            if (!enumerateDirectory(
                    directory.number,
                    maximumEntries,
                    children,
                    result.scannedRecords,
                    result.truncated,
                    result.errorText))
            {
                return false;
            }
            result.entries.reserve(children.size());
            for (const BtrfsDirectoryChild& child : children)
            {
                BtrfsInode inode;
                if (!loadInode(child.inode, inode, result.errorText))
                {
                    return false;
                }
                std::vector<BtrfsFileSegment> segments;
                if ((inode.mode & 0xF000U) != 0x4000U
                    && !loadFileSegments(
                        inode.number,
                        segments,
                        result.errorText))
                {
                    return false;
                }
                ks::misc::RawFileEntry entry;
                fillEntry(
                    inode,
                    directory.number,
                    child.name,
                    ks::misc::rawfs::childPath(
                        canonicalPath,
                        child.name),
                    segments,
                    entry);
                result.entries.push_back(std::move(entry));
            }
            return true;
        }

        bool readFile(
            const BtrfsInode& inode,
            const std::uint64_t offset,
            const std::uint32_t length,
            ks::misc::RawFileReadResult& result)
        {
            if ((inode.mode & 0xF000U) == 0x4000U)
            {
                result.errorText = QStringLiteral(
                    "目标对象是目录，不能按文件读取。");
                return false;
            }
            if (offset >= inode.size)
            {
                result.endOfFile = true;
                return true;
            }

            std::vector<BtrfsFileSegment> segments;
            if (!loadFileSegments(
                    inode.number,
                    segments,
                    result.errorText))
            {
                return false;
            }
            result.extents = describeSegments(inode.size, segments);
            result.extentsTruncated =
                segments.size() > ks::misc::rawfs::kMaximumExtentCount;

            const std::uint32_t bounded =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        length,
                        inode.size - offset));
            result.bytes = QByteArray(
                static_cast<qsizetype>(bounded),
                '\0');
            const std::uint64_t requestEnd = offset + bounded;
            for (const BtrfsFileSegment& segment : segments)
            {
                const std::uint64_t segmentEnd =
                    segment.logicalOffset + segment.logicalLength;
                const std::uint64_t overlapStart =
                    std::max(offset, segment.logicalOffset);
                const std::uint64_t overlapEnd =
                    std::min(requestEnd, segmentEnd);
                if (overlapStart >= overlapEnd
                    || segment.sparse
                    || segment.preallocated)
                {
                    continue;
                }
                if (segment.compression != 0U
                    || segment.encryption != 0U
                    || segment.otherEncoding != 0U)
                {
                    result.bytes.clear();
                    result.errorText = QStringLiteral(
                        "文件包含压缩、加密或未知编码的 Btrfs extent，"
                        "原始读取已安全拒绝。");
                    return false;
                }

                const std::uint64_t within =
                    overlapStart - segment.logicalOffset;
                const std::uint32_t copyLength =
                    static_cast<std::uint32_t>(
                        overlapEnd - overlapStart);
                const qsizetype destination =
                    static_cast<qsizetype>(overlapStart - offset);
                if (!segment.inlineBytes.isEmpty())
                {
                    if (within + copyLength
                        > static_cast<std::uint64_t>(
                            segment.inlineBytes.size()))
                    {
                        result.bytes.clear();
                        result.errorText = QStringLiteral(
                            "Btrfs 内联 extent 数据被截断。");
                        return false;
                    }
                    std::copy_n(
                        segment.inlineBytes.cbegin()
                            + static_cast<qsizetype>(within),
                        copyLength,
                        result.bytes.begin() + destination);
                    continue;
                }

                if (!readLogicalData(
                        segment.diskLogical + within,
                        copyLength,
                        result.bytes,
                        destination,
                        result.errorText))
                {
                    result.bytes.clear();
                    return false;
                }
            }
            result.endOfFile = requestEnd >= inode.size;
            return true;
        }

    private:
        bool parseSystemChunkArray(
            const unsigned char* array,
            const std::uint32_t length,
            QString& errorText)
        {
            std::uint32_t cursor = 0;
            while (cursor < length)
            {
                if (length - cursor < 17U + kChunkHeaderSize)
                {
                    errorText = QStringLiteral(
                        "Btrfs system chunk array 被截断。");
                    return false;
                }
                const BtrfsKey key = parseKey(array + cursor);
                if (key.type != kChunkItemKey)
                {
                    errorText = QStringLiteral(
                        "Btrfs system chunk array 包含非块组键。");
                    return false;
                }
                const unsigned char* const chunkData =
                    array + cursor + 17U;
                const std::uint16_t stripeCount =
                    le16(chunkData + 44U);
                const std::uint64_t recordLength =
                    17U + kChunkHeaderSize
                    + static_cast<std::uint64_t>(stripeCount)
                        * kChunkStripeSize;
                if (recordLength > length - cursor)
                {
                    errorText = QStringLiteral(
                        "Btrfs system chunk 条带数组越界。");
                    return false;
                }
                if (!parseChunk(
                        key.offset,
                        chunkData,
                        static_cast<std::uint32_t>(
                            recordLength - 17U),
                        errorText))
                {
                    return false;
                }
                cursor += static_cast<std::uint32_t>(recordLength);
            }
            normalizeChunkMap();
            return true;
        }

        bool parseChunk(
            const std::uint64_t logical,
            const unsigned char* data,
            const std::uint32_t size,
            QString& errorText)
        {
            if (size < kChunkHeaderSize)
            {
                errorText = QStringLiteral("Btrfs chunk item 被截断。");
                return false;
            }
            const std::uint64_t length = le64(data);
            const std::uint64_t type = le64(data + 24U);
            const std::uint16_t stripeCount = le16(data + 44U);
            const std::uint64_t required =
                kChunkHeaderSize
                + static_cast<std::uint64_t>(stripeCount)
                    * kChunkStripeSize;
            if (length == 0U || stripeCount == 0U || required > size)
            {
                errorText = QStringLiteral("Btrfs chunk item 几何参数无效。");
                return false;
            }
            if ((type & (kBlockGroupRaid0
                    | kBlockGroupRaid10
                    | kBlockGroupRaid5
                    | kBlockGroupRaid6)) != 0U)
            {
                return true;
            }

            const unsigned char* selectedStripe = nullptr;
            for (std::uint16_t index = 0; index < stripeCount; ++index)
            {
                const unsigned char* const stripe =
                    data + kChunkHeaderSize
                    + static_cast<std::uint32_t>(index)
                        * kChunkStripeSize;
                const std::uint64_t deviceId = le64(stripe);
                if (deviceId == m_currentDeviceId
                    || (m_deviceCount == 1U && index == 0U))
                {
                    selectedStripe = stripe;
                    break;
                }
            }
            if (selectedStripe == nullptr)
            {
                return true;
            }

            const std::uint64_t physical = le64(selectedStripe + 8U);
            if (physical > m_reader.partitionLength()
                || length > m_reader.partitionLength() - physical)
            {
                errorText = QStringLiteral(
                    "Btrfs chunk item 的物理范围越过所选分区。");
                return false;
            }
            m_chunks.push_back(
                BtrfsChunk{
                    logical,
                    length,
                    physical,
                    type,
                    le64(selectedStripe)});
            return true;
        }

        void normalizeChunkMap()
        {
            std::sort(
                m_chunks.begin(),
                m_chunks.end(),
                [](const BtrfsChunk& left, const BtrfsChunk& right)
                {
                    if (left.logical != right.logical)
                    {
                        return left.logical < right.logical;
                    }
                    return left.length > right.length;
                });
            m_chunks.erase(
                std::unique(
                    m_chunks.begin(),
                    m_chunks.end(),
                    [](const BtrfsChunk& left, const BtrfsChunk& right)
                    {
                        return left.logical == right.logical
                            && left.length == right.length
                            && left.physical == right.physical;
                    }),
                m_chunks.end());
        }

        bool logicalToPhysical(
            const std::uint64_t logical,
            std::uint64_t& physicalOut,
            std::uint64_t& contiguousOut,
            QString& errorText) const
        {
            for (auto iterator = m_chunks.crbegin();
                 iterator != m_chunks.crend();
                 ++iterator)
            {
                const BtrfsChunk& chunk = *iterator;
                if (logical < chunk.logical
                    || logical - chunk.logical >= chunk.length)
                {
                    continue;
                }
                if ((chunk.type & (kBlockGroupRaid0
                        | kBlockGroupRaid10
                        | kBlockGroupRaid5
                        | kBlockGroupRaid6)) != 0U)
                {
                    errorText = QStringLiteral(
                        "当前 Btrfs 逻辑地址位于不支持的条带或奇偶校验块组。");
                    return false;
                }
                const std::uint64_t within = logical - chunk.logical;
                if (!checkedAdd(chunk.physical, within, physicalOut))
                {
                    errorText = QStringLiteral("Btrfs 物理地址发生溢出。");
                    return false;
                }
                contiguousOut = chunk.length - within;
                return true;
            }
            errorText = QStringLiteral("Btrfs 逻辑地址缺少块组映射。");
            return false;
        }

        bool readLogical(
            const std::uint64_t logical,
            const std::uint32_t length,
            QByteArray& bytesOut,
            QString& errorText) const
        {
            bytesOut.resize(static_cast<qsizetype>(length));
            std::uint64_t completed = 0;
            while (completed < length)
            {
                std::uint64_t physical = 0;
                std::uint64_t contiguous = 0;
                if (!logicalToPhysical(
                        logical + completed,
                        physical,
                        contiguous,
                        errorText))
                {
                    bytesOut.clear();
                    return false;
                }
                const std::uint32_t chunkLength =
                    static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            length - completed,
                            contiguous));
                QByteArray chunk;
                if (!m_reader.read(
                        physical,
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

        bool readLogicalData(
            const std::uint64_t logical,
            const std::uint32_t length,
            QByteArray& destination,
            const qsizetype destinationOffset,
            QString& errorText) const
        {
            QByteArray bytes;
            if (!readLogical(logical, length, bytes, errorText))
            {
                return false;
            }
            std::copy(
                bytes.cbegin(),
                bytes.cend(),
                destination.begin() + destinationOffset);
            return true;
        }

        bool readTreeBlock(
            const std::uint64_t logical,
            QByteArray& blockOut,
            std::uint64_t& physicalOut,
            QString& errorText) const
        {
            std::uint64_t contiguous = 0;
            if (!logicalToPhysical(
                    logical,
                    physicalOut,
                    contiguous,
                    errorText)
                || contiguous < m_nodeSize
                || !readLogical(
                    logical,
                    m_nodeSize,
                    blockOut,
                    errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(blockOut);
            if (le64(bytes + 0x30U) != logical
                || bytes[0x64U] > ks::misc::rawfs::kMaximumTreeDepth)
            {
                errorText = QStringLiteral("Btrfs tree block 头部无效。");
                return false;
            }
            const std::uint32_t itemCount = le32(bytes + 0x60U);
            const std::uint64_t tableBytes =
                static_cast<std::uint64_t>(itemCount)
                * (bytes[0x64U] == 0U
                    ? kLeafItemSize
                    : kNodePointerSize);
            if (tableBytes > m_nodeSize - kTreeHeaderSize)
            {
                errorText = QStringLiteral("Btrfs tree block 项目表越界。");
                return false;
            }
            return true;
        }

        template<typename Callback>
        bool walkTreeRange(
            const std::uint64_t logical,
            const BtrfsKey& minimum,
            const BtrfsKey& maximum,
            Callback&& callback,
            std::vector<std::uint64_t>& visited,
            QString& errorText)
        {
            if (m_walkStopRequested)
            {
                return true;
            }
            if (visited.size() >= kMaximumVisitedTreeBlocks
                || std::find(
                    visited.cbegin(),
                    visited.cend(),
                    logical) != visited.cend())
            {
                errorText = QStringLiteral(
                    "Btrfs tree 包含循环或节点数超过安全上限。");
                return false;
            }
            visited.push_back(logical);

            QByteArray block;
            std::uint64_t physical = 0;
            if (!readTreeBlock(
                    logical,
                    block,
                    physical,
                    errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(block);
            const std::uint32_t count = le32(bytes + 0x60U);
            const std::uint8_t level = bytes[0x64U];
            if (level == 0U)
            {
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const unsigned char* const item =
                        bytes + kTreeHeaderSize
                        + index * kLeafItemSize;
                    const BtrfsKey key = parseKey(item);
                    if (compareKey(key, minimum) < 0
                        || compareKey(key, maximum) > 0)
                    {
                        continue;
                    }
                    const std::uint32_t dataOffset =
                        kTreeHeaderSize + le32(item + 17U);
                    const std::uint32_t dataSize = le32(item + 21U);
                    if (dataOffset > m_nodeSize
                        || dataSize > m_nodeSize - dataOffset)
                    {
                        errorText = QStringLiteral(
                            "Btrfs leaf item 数据范围越界。");
                        return false;
                    }
                    if (!callback(
                            BtrfsLeafItem{
                                key,
                                bytes + dataOffset,
                                dataSize,
                                m_reader.partitionOffset()
                                    + physical + dataOffset}))
                    {
                        m_walkStopRequested = true;
                        return true;
                    }
                }
                return true;
            }

            for (std::uint32_t index = 0; index < count; ++index)
            {
                const unsigned char* const pointer =
                    bytes + kTreeHeaderSize
                    + index * kNodePointerSize;
                const BtrfsKey lower = parseKey(pointer);
                if (compareKey(lower, maximum) > 0)
                {
                    break;
                }
                if (index + 1U < count)
                {
                    const BtrfsKey next = parseKey(
                        pointer + kNodePointerSize);
                    if (compareKey(next, minimum) <= 0)
                    {
                        continue;
                    }
                }
                const std::uint64_t childLogical =
                    le64(pointer + 17U);
                if (!walkTreeRange(
                        childLogical,
                        minimum,
                        maximum,
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
            }
            return true;
        }

        bool loadInode(
            const std::uint64_t inodeNumber,
            BtrfsInode& inodeOut,
            QString& errorText)
        {
            bool found = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            if (!walkTreeRange(
                    m_fsTreeLogical,
                    BtrfsKey{inodeNumber, kInodeItemKey, 0U},
                    BtrfsKey{inodeNumber, kInodeItemKey, 0U},
                    [&](const BtrfsLeafItem& item)
                    {
                        if (item.size < 160U)
                        {
                            return true;
                        }
                        BtrfsInode inode;
                        inode.number = inodeNumber;
                        inode.size = le64(item.data + 16U);
                        inode.allocated = le64(item.data + 24U);
                        inode.mode = le32(item.data + 52U);
                        inode.flags = le64(item.data + 64U);
                        inode.metadataOffset = item.absoluteOffset;
                        inodeOut = inode;
                        found = true;
                        return false;
                    },
                    visited,
                    errorText))
            {
                return false;
            }
            if (!found)
            {
                errorText = QStringLiteral(
                    "Btrfs inode 不存在：%1").arg(inodeNumber);
                return false;
            }
            return true;
        }

        bool enumerateDirectory(
            const std::uint64_t inodeNumber,
            const std::uint32_t maximumEntries,
            std::vector<BtrfsDirectoryChild>& childrenOut,
            std::uint64_t& scannedRecordsOut,
            bool& truncatedOut,
            QString& errorText)
        {
            childrenOut.clear();
            scannedRecordsOut = 0;
            truncatedOut = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            const bool walked = walkTreeRange(
                m_fsTreeLogical,
                BtrfsKey{inodeNumber, kDirectoryIndexKey, 0U},
                BtrfsKey{
                    inodeNumber,
                    kDirectoryIndexKey,
                    std::numeric_limits<std::uint64_t>::max()},
                [&](const BtrfsLeafItem& item)
                {
                    std::uint32_t cursor = 0;
                    while (cursor < item.size)
                    {
                        if (item.size - cursor < kDirItemHeaderSize)
                        {
                            errorText = QStringLiteral(
                                "Btrfs 目录项记录被截断。");
                            return false;
                        }
                        const unsigned char* const entry =
                            item.data + cursor;
                        const std::uint16_t dataLength =
                            le16(entry + 25U);
                        const std::uint16_t nameLength =
                            le16(entry + 27U);
                        const std::uint64_t recordLength =
                            kDirItemHeaderSize
                            + static_cast<std::uint64_t>(nameLength)
                            + dataLength;
                        if (recordLength > item.size - cursor)
                        {
                            errorText = QStringLiteral(
                                "Btrfs 目录项名称范围越界。");
                            return false;
                        }
                        if (++scannedRecordsOut
                            > kMaximumDirectoryRecords)
                        {
                            errorText = QStringLiteral(
                                "Btrfs 目录记录数超过安全上限。");
                            return false;
                        }
                        if (childrenOut.size() >= maximumEntries)
                        {
                            truncatedOut = true;
                            return false;
                        }
                        BtrfsDirectoryChild child;
                        child.inode = le64(entry);
                        child.type = entry[29U];
                        child.name = QString::fromUtf8(
                            reinterpret_cast<const char*>(
                                entry + kDirItemHeaderSize),
                            nameLength);
                        childrenOut.push_back(std::move(child));
                        cursor += static_cast<std::uint32_t>(recordLength);
                    }
                    return true;
                },
                visited,
                errorText);
            return walked && errorText.isEmpty();
        }

        bool findDirectoryChild(
            const std::uint64_t parentInode,
            const QString& requestedName,
            BtrfsDirectoryChild& childOut,
            QString& errorText)
        {
            std::vector<BtrfsDirectoryChild> children;
            std::uint64_t scanned = 0;
            bool truncated = false;
            if (!enumerateDirectory(
                    parentInode,
                    kMaximumDirectoryRecords,
                    children,
                    scanned,
                    truncated,
                    errorText))
            {
                return false;
            }
            const auto iterator = std::find_if(
                children.cbegin(),
                children.cend(),
                [&](const BtrfsDirectoryChild& child)
                {
                    return child.name == requestedName;
                });
            if (iterator == children.cend())
            {
                errorText = QStringLiteral(
                    "Btrfs 目录中不存在对象：%1")
                    .arg(requestedName);
                return false;
            }
            childOut = *iterator;
            return true;
        }

        bool loadFileSegments(
            const std::uint64_t inodeNumber,
            std::vector<BtrfsFileSegment>& segmentsOut,
            QString& errorText)
        {
            segmentsOut.clear();
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            if (!walkTreeRange(
                    m_fsTreeLogical,
                    BtrfsKey{inodeNumber, kFileExtentDataKey, 0U},
                    BtrfsKey{
                        inodeNumber,
                        kFileExtentDataKey,
                        std::numeric_limits<std::uint64_t>::max()},
                    [&](const BtrfsLeafItem& item)
                    {
                        if (item.size < kFileExtentHeaderSize)
                        {
                            errorText = QStringLiteral(
                                "Btrfs file extent item 被截断。");
                            return false;
                        }
                        BtrfsFileSegment segment;
                        segment.logicalOffset = item.key.offset;
                        segment.compression = item.data[16U];
                        segment.encryption = item.data[17U];
                        segment.otherEncoding = le16(item.data + 18U);
                        const std::uint8_t extentType = item.data[20U];
                        if (extentType == kExtentInline)
                        {
                            segment.inlineBytes = QByteArray(
                                reinterpret_cast<const char*>(
                                    item.data + kFileExtentHeaderSize),
                                static_cast<qsizetype>(
                                    item.size - kFileExtentHeaderSize));
                            const std::uint64_t ramBytes =
                                le64(item.data + 8U);
                            segment.logicalLength =
                                segment.compression == 0U
                                    ? std::min<std::uint64_t>(
                                        ramBytes,
                                        segment.inlineBytes.size())
                                    : ramBytes;
                            segment.inlineAbsolute =
                                item.absoluteOffset
                                + kFileExtentHeaderSize;
                        }
                        else if (extentType == kExtentRegular
                            || extentType == kExtentPreallocated)
                        {
                            if (item.size < kFileExtentRegularSize)
                            {
                                errorText = QStringLiteral(
                                    "Btrfs regular extent item 被截断。");
                                return false;
                            }
                            const std::uint64_t diskBytenr =
                                le64(item.data + 21U);
                            segment.diskLength =
                                le64(item.data + 29U);
                            const std::uint64_t extentOffset =
                                le64(item.data + 37U);
                            segment.logicalLength =
                                le64(item.data + 45U);
                            segment.sparse = diskBytenr == 0U;
                            segment.preallocated =
                                extentType == kExtentPreallocated;
                            if (!segment.sparse
                                && !checkedAdd(
                                    diskBytenr,
                                    extentOffset,
                                    segment.diskLogical))
                            {
                                errorText = QStringLiteral(
                                    "Btrfs extent 逻辑地址发生溢出。");
                                return false;
                            }
                        }
                        else
                        {
                            errorText = QStringLiteral(
                                "Btrfs file extent 类型未知。");
                            return false;
                        }
                        if (segment.logicalLength != 0U)
                        {
                            segmentsOut.push_back(std::move(segment));
                        }
                        if (segmentsOut.size()
                            > ks::misc::rawfs::kMaximumExtentCount * 16U)
                        {
                            errorText = QStringLiteral(
                                "Btrfs 文件 extent 数量超过安全上限。");
                            return false;
                        }
                        return true;
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
            std::sort(
                segmentsOut.begin(),
                segmentsOut.end(),
                [](const BtrfsFileSegment& left,
                   const BtrfsFileSegment& right)
                {
                    return left.logicalOffset < right.logicalOffset;
                });
            return true;
        }

        std::vector<ks::misc::RawFileExtent> describeSegments(
            const std::uint64_t fileSize,
            const std::vector<BtrfsFileSegment>& segments) const
        {
            std::vector<ks::misc::RawFileExtent> extents;
            for (const BtrfsFileSegment& segment : segments)
            {
                if (extents.size()
                        >= ks::misc::rawfs::kMaximumExtentCount
                    || segment.logicalOffset >= fileSize)
                {
                    break;
                }
                ks::misc::RawFileExtent extent;
                extent.logicalOffset = segment.logicalOffset;
                extent.lengthBytes = std::min<std::uint64_t>(
                    segment.logicalLength,
                    fileSize - segment.logicalOffset);
                extent.sparse = segment.sparse;
                extent.unwritten = segment.preallocated;
                extent.compressed = segment.compression != 0U;
                if (!segment.inlineBytes.isEmpty())
                {
                    extent.absoluteOffset = segment.inlineAbsolute;
                    extent.physicalMappingExact = true;
                }
                else if (!segment.sparse)
                {
                    std::uint64_t physical = 0;
                    std::uint64_t contiguous = 0;
                    QString ignored;
                    if (logicalToPhysical(
                            segment.diskLogical,
                            physical,
                            contiguous,
                            ignored))
                    {
                        extent.absoluteOffset =
                            m_reader.partitionOffset() + physical;
                        extent.physicalMappingExact =
                            contiguous >= extent.lengthBytes;
                    }
                }
                extents.push_back(extent);
            }
            return extents;
        }

        static ks::misc::RawFileObjectType objectType(
            const std::uint32_t mode)
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

        void fillEntry(
            const BtrfsInode& inode,
            const std::uint64_t parentInode,
            const QString& name,
            const QString& fullPath,
            const std::vector<BtrfsFileSegment>& segments,
            ks::misc::RawFileEntry& entry) const
        {
            entry.name = name;
            entry.fullPath = fullPath;
            entry.type = objectType(inode.mode);
            entry.objectId = inode.number;
            entry.parentObjectId = parentInode;
            entry.fileSizeBytes = inode.size;
            entry.allocatedSizeBytes = inode.allocated;
            entry.metadataOffset = inode.metadataOffset;
            entry.modeOrFlags =
                static_cast<std::uint32_t>(inode.flags)
                | inode.mode;
            entry.extents = describeSegments(inode.size, segments);
            entry.extentsTruncated =
                segments.size() > ks::misc::rawfs::kMaximumExtentCount;
        }

        const ks::misc::rawfs::VolumeReader& m_reader;
        std::uint32_t m_sectorSize = 0;
        std::uint32_t m_nodeSize = 0;
        std::uint64_t m_totalBytes = 0;
        std::uint64_t m_deviceCount = 0;
        std::uint64_t m_currentDeviceId = 0;
        std::uint64_t m_rootTreeLogical = 0;
        std::uint64_t m_chunkTreeLogical = 0;
        std::uint64_t m_fsTreeLogical = 0;
        std::vector<BtrfsChunk> m_chunks;
        bool m_walkStopRequested = false;
    };

    ks::misc::RawDirectoryResult makeDirectoryResult(
        const QString& path)
    {
        ks::misc::RawDirectoryResult result;
        result.fileSystem =
            ks::misc::ForensicFileSystemKind::Btrfs;
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
            ks::misc::ForensicFileSystemKind::Btrfs;
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
    RawDirectoryResult listBtrfs(
        const VolumeReader& reader,
        const QString& path,
        const std::uint32_t maximumEntries)
    {
        RawDirectoryResult result = makeDirectoryResult(path);
        BtrfsVolume volume(reader);
        if (!volume.mount(result.errorText))
        {
            return result;
        }
        BtrfsInode directory;
        if (!volume.resolvePath(
                path,
                directory,
                result.canonicalPath,
                result.errorText))
        {
            return result;
        }
        result.directoryObjectId = directory.number;
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

    RawFileReadResult readBtrfs(
        const VolumeReader& reader,
        const QString& path,
        const std::uint64_t offset,
        const std::uint32_t length)
    {
        struct FileState
        {
            BtrfsVolume volume;
            BtrfsInode data;
            RawFileReadResult basis;
            explicit FileState(const VolumeReader& reader) : volume(reader) {}
        };
        auto state = reader.fileSessionState<FileState>();
        if (!state)
        {
            state = std::make_shared<FileState>(reader);
            auto& volume = state->volume;
            auto& inode = state->data;

            RawFileReadResult result = makeFileResult(path, offset);
            if (!volume.mount(result.errorText))
            {
                return result;
            }
            QString canonicalPath;
            if (!volume.resolvePath(
                    path,
                    inode,
                    canonicalPath,
                    result.errorText))
            {
                return result;
            }
            result.filePath = canonicalPath;
            result.fileSizeBytes = inode.size;
            state->basis = result;
            reader.setFileSessionState(state);
        }
        RawFileReadResult result = state->basis;
        result.requestedOffset = offset;
        auto& volume = state->volume;
        auto& inode = state->data;
        if (!volume.readFile(inode, offset, length, result))
        {
            return result;
        }
        result.success = true;
        return result;
    }
}
