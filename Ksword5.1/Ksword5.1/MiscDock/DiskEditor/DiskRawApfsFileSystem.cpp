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
    constexpr std::uint32_t kNxMagic = 0x4253584EU;
    constexpr std::uint32_t kVolumeMagic = 0x42535041U;
    constexpr std::uint32_t kObjectTypeMask = 0x0000FFFFU;
    constexpr std::uint32_t kObjectStorageMask = 0xC0000000U;
    constexpr std::uint32_t kPhysicalStorage = 0x40000000U;
    constexpr std::uint32_t kBtreeObject = 0x0002U;
    constexpr std::uint32_t kBtreeNodeObject = 0x0003U;
    constexpr std::uint32_t kOmapObject = 0x000BU;
    constexpr std::uint16_t kNodeRoot = 0x0001U;
    constexpr std::uint16_t kNodeFixedKeyValue = 0x0004U;
    constexpr std::uint32_t kBtreePhysical = 0x00000010U;
    constexpr std::uint64_t kObjectIdMask = 0x0FFFFFFFFFFFFFFFULL;
    constexpr std::uint8_t kObjectTypeShift = 60U;
    constexpr std::uint8_t kInodeRecordType = 3U;
    constexpr std::uint8_t kFileExtentRecordType = 8U;
    constexpr std::uint8_t kDirectoryRecordType = 9U;
    constexpr std::uint64_t kRootDirectoryId = 2U;
    constexpr std::uint64_t kCaseInsensitiveFeature = 0x1U;
    constexpr std::uint64_t kEncryptionRolledFeature = 0x4U;
    constexpr std::uint64_t kNormalizationInsensitiveFeature = 0x8U;
    constexpr std::uint32_t kCompressedBsdFlag = 0x20U;
    constexpr std::uint8_t kDstreamExtendedField = 8U;
    constexpr std::uint32_t kNodeHeaderSize = 56U;
    constexpr std::uint32_t kRootInfoSize = 40U;
    constexpr std::uint32_t kMaximumObjectMapDepth = 32U;
    constexpr std::uint32_t kMaximumVisitedNodes = 1U << 20U;
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

    std::uint64_t recordPrefix(
        const std::uint64_t objectId,
        const std::uint8_t type)
    {
        return (static_cast<std::uint64_t>(type) << kObjectTypeShift)
            | (objectId & kObjectIdMask);
    }

    std::uint64_t prefixObjectId(const std::uint64_t prefix)
    {
        return prefix & kObjectIdMask;
    }

    std::uint8_t prefixType(const std::uint64_t prefix)
    {
        return static_cast<std::uint8_t>(prefix >> kObjectTypeShift);
    }

    int comparePrefix(
        const std::uint64_t left,
        const std::uint64_t right)
    {
        const std::uint64_t leftObject = prefixObjectId(left);
        const std::uint64_t rightObject = prefixObjectId(right);
        if (leftObject != rightObject)
        {
            return leftObject < rightObject ? -1 : 1;
        }
        const std::uint8_t leftType = prefixType(left);
        const std::uint8_t rightType = prefixType(right);
        if (leftType != rightType)
        {
            return leftType < rightType ? -1 : 1;
        }
        return 0;
    }

    struct ApfsNodeLayout
    {
        std::uint16_t flags = 0;
        std::uint16_t level = 0;
        std::uint32_t keyCount = 0;
        std::uint32_t tableOffset = 0;
        std::uint32_t tableLength = 0;
        std::uint32_t keysOffset = 0;
        std::uint32_t valuesEnd = 0;
        bool fixed = false;
    };

    struct ApfsRecordView
    {
        const unsigned char* key = nullptr;
        std::uint16_t keyLength = 0;
        const unsigned char* value = nullptr;
        std::uint16_t valueLength = 0;
        std::uint64_t valueAbsoluteOffset = 0;
    };

    struct ApfsInode
    {
        std::uint64_t number = 0;
        std::uint64_t parentId = 0;
        std::uint64_t size = 0;
        std::uint64_t allocated = 0;
        std::uint64_t internalFlags = 0;
        std::uint64_t metadataOffset = 0;
        std::uint32_t bsdFlags = 0;
        std::uint16_t mode = 0;
    };

    struct ApfsDirectoryChild
    {
        QString name;
        std::uint64_t inode = 0;
        std::uint16_t flags = 0;
    };

    struct ApfsExtent
    {
        std::uint64_t logicalOffset = 0;
        std::uint64_t length = 0;
        std::uint64_t physicalBlock = 0;
        std::uint64_t cryptoId = 0;
        std::uint64_t metadataOffset = 0;
        std::uint8_t flags = 0;
    };

    class ApfsVolume final
    {
    public:
        explicit ApfsVolume(
            const ks::misc::rawfs::VolumeReader& reader)
            : m_reader(reader)
        {
        }

        bool mount(QString& errorText)
        {
            QByteArray prefix;
            if (!m_reader.read(0U, 4096U, prefix, errorText))
            {
                return false;
            }
            const unsigned char* bytes = dataPointer(prefix);
            if (le32(bytes + 0x20U) != kNxMagic)
            {
                errorText = QStringLiteral("APFS 容器超级块魔数不匹配。");
                return false;
            }
            m_blockSize = le32(bytes + 0x24U);
            m_blockCount = le64(bytes + 0x28U);
            std::uint64_t containerBytes = 0;
            if (!isPowerOfTwo(m_blockSize)
                || m_blockSize < 512U
                || m_blockSize > 65536U
                || m_blockCount == 0U
                || !checkedMultiply(
                    m_blockCount,
                    m_blockSize,
                    containerBytes)
                || containerBytes > m_reader.partitionLength())
            {
                errorText = QStringLiteral("APFS 容器几何参数无效。");
                return false;
            }

            QByteArray containerHeader;
            if (m_blockSize == 4096U)
            {
                containerHeader = prefix;
            }
            else if (!m_reader.read(
                    0U,
                    m_blockSize,
                    containerHeader,
                    errorText))
            {
                return false;
            }
            bytes = dataPointer(containerHeader);
            const std::uint64_t containerOmapObject =
                le64(bytes + 0xA0U);
            const std::uint32_t maximumFileSystems =
                std::min<std::uint32_t>(le32(bytes + 0xB4U), 100U);
            if (maximumFileSystems == 0U
                || 0xB8U
                    + static_cast<std::uint64_t>(maximumFileSystems) * 8U
                    > m_blockSize)
            {
                errorText = QStringLiteral("APFS 卷对象数组无效。");
                return false;
            }
            std::uint64_t volumeObject = 0;
            for (std::uint32_t index = 0;
                 index < maximumFileSystems;
                 ++index)
            {
                volumeObject = le64(bytes + 0xB8U + index * 8U);
                if (volumeObject != 0U)
                {
                    break;
                }
            }
            if (volumeObject == 0U)
            {
                errorText = QStringLiteral("APFS 容器不包含可用卷对象。");
                return false;
            }

            if (!loadObjectMapRoot(
                    containerOmapObject,
                    m_containerOmapRoot,
                    errorText))
            {
                return false;
            }
            std::uint64_t volumePhysical = 0;
            if (!objectMapLookup(
                    m_containerOmapRoot,
                    volumeObject,
                    std::numeric_limits<std::uint64_t>::max(),
                    volumePhysical,
                    errorText))
            {
                return false;
            }

            QByteArray volumeHeader;
            if (!readBlock(volumePhysical, volumeHeader, errorText))
            {
                return false;
            }
            bytes = dataPointer(volumeHeader);
            if (le32(bytes + 0x20U) != kVolumeMagic)
            {
                errorText = QStringLiteral("APFS 卷超级块魔数不匹配。");
                return false;
            }
            m_incompatibilityFlags = le64(bytes + 0x38U);
            m_caseInsensitive =
                (m_incompatibilityFlags
                    & (kCaseInsensitiveFeature
                        | kNormalizationInsensitiveFeature)) != 0U;
            m_encryptionRolled =
                (m_incompatibilityFlags
                    & kEncryptionRolledFeature) != 0U;
            const std::uint64_t volumeOmapObject =
                le64(bytes + 0x80U);
            const std::uint64_t fileSystemTreeObject =
                le64(bytes + 0x88U);
            if (!loadObjectMapRoot(
                    volumeOmapObject,
                    m_volumeOmapRoot,
                    errorText)
                || !objectMapLookup(
                    m_volumeOmapRoot,
                    fileSystemTreeObject,
                    std::numeric_limits<std::uint64_t>::max(),
                    m_fileSystemTreeRoot,
                    errorText))
            {
                return false;
            }

            QByteArray root;
            if (!readBtreeNode(
                    m_fileSystemTreeRoot,
                    root,
                    errorText))
            {
                return false;
            }
            const unsigned char* const rootBytes = dataPointer(root);
            const std::uint16_t flags = le16(rootBytes + 0x20U);
            if ((flags & kNodeRoot) != 0U
                && root.size() >= static_cast<qsizetype>(kRootInfoSize))
            {
                const std::uint32_t infoOffset =
                    static_cast<std::uint32_t>(root.size())
                    - kRootInfoSize;
                m_fileSystemTreePhysical =
                    (le32(rootBytes + infoOffset) & kBtreePhysical) != 0U;
            }
            else
            {
                m_fileSystemTreePhysical =
                    (le32(rootBytes + 0x18U)
                        & kObjectStorageMask) == kPhysicalStorage;
            }
            return true;
        }

        bool resolvePath(
            const QString& path,
            ApfsInode& inodeOut,
            QString& canonicalPathOut,
            QString& errorText)
        {
            ApfsInode current;
            if (!loadInode(kRootDirectoryId, current, errorText))
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
                        "路径中间对象不是 APFS 目录：%1")
                        .arg(canonicalPathOut);
                    return false;
                }
                ApfsDirectoryChild child;
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
            const ApfsInode& directory,
            const QString& canonicalPath,
            const std::uint32_t maximumEntries,
            ks::misc::RawDirectoryResult& result)
        {
            if ((directory.mode & 0xF000U) != 0x4000U)
            {
                result.errorText = QStringLiteral("目标对象不是 APFS 目录。");
                return false;
            }
            std::vector<ApfsDirectoryChild> children;
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
            for (const ApfsDirectoryChild& child : children)
            {
                ApfsInode inode;
                if (!loadInode(child.inode, inode, result.errorText))
                {
                    return false;
                }
                std::vector<ApfsExtent> extents;
                if ((inode.mode & 0xF000U) != 0x4000U
                    && !loadExtents(
                        inode.number,
                        extents,
                        result.errorText))
                {
                    return false;
                }
                ks::misc::RawFileEntry entry;
                fillEntry(
                    inode,
                    child.name,
                    ks::misc::rawfs::childPath(
                        canonicalPath,
                        child.name),
                    extents,
                    entry);
                result.entries.push_back(std::move(entry));
            }
            return true;
        }

        bool readFile(
            const ApfsInode& inode,
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
            if ((inode.bsdFlags & kCompressedBsdFlag) != 0U)
            {
                result.errorText = QStringLiteral(
                    "该 APFS 文件使用压缩数据流，原始读取已安全拒绝。");
                return false;
            }
            if (m_encryptionRolled)
            {
                result.errorText = QStringLiteral(
                    "该 APFS 卷启用了滚动加密状态，原始读取已安全拒绝。");
                return false;
            }
            if (offset >= inode.size)
            {
                result.endOfFile = true;
                return true;
            }

            std::vector<ApfsExtent> extents;
            if (!loadExtents(
                    inode.number,
                    extents,
                    result.errorText))
            {
                return false;
            }
            result.extents = describeExtents(inode, extents);
            result.extentsTruncated =
                extents.size() > ks::misc::rawfs::kMaximumExtentCount;

            const std::uint32_t bounded =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        length,
                        inode.size - offset));
            result.bytes = QByteArray(
                static_cast<qsizetype>(bounded),
                '\0');
            const std::uint64_t requestEnd = offset + bounded;
            for (const ApfsExtent& extent : extents)
            {
                const std::uint64_t extentEnd =
                    extent.logicalOffset + extent.length;
                const std::uint64_t overlapStart =
                    std::max(offset, extent.logicalOffset);
                const std::uint64_t overlapEnd =
                    std::min(requestEnd, extentEnd);
                if (overlapStart >= overlapEnd
                    || extent.physicalBlock == 0U)
                {
                    continue;
                }
                if (extent.cryptoId != 0U)
                {
                    result.bytes.clear();
                    result.errorText = QStringLiteral(
                        "该 APFS 文件 extent 受加密保护，原始读取已安全拒绝。");
                    return false;
                }
                const std::uint64_t within =
                    overlapStart - extent.logicalOffset;
                std::uint64_t physical = 0;
                if (!checkedMultiply(
                        extent.physicalBlock,
                        m_blockSize,
                        physical)
                    || physical
                        > std::numeric_limits<std::uint64_t>::max()
                            - within)
                {
                    result.bytes.clear();
                    result.errorText = QStringLiteral(
                        "APFS extent 物理地址发生溢出。");
                    return false;
                }
                const std::uint32_t copyLength =
                    static_cast<std::uint32_t>(
                        overlapEnd - overlapStart);
                QByteArray bytes;
                if (!m_reader.read(
                        physical + within,
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
                    result.bytes.begin()
                        + static_cast<qsizetype>(
                            overlapStart - offset));
            }
            result.endOfFile = requestEnd >= inode.size;
            return true;
        }

    private:
        bool readBlock(
            const std::uint64_t physicalBlock,
            QByteArray& blockOut,
            QString& errorText) const
        {
            if (physicalBlock >= m_blockCount)
            {
                errorText = QStringLiteral("APFS 物理块号越界。");
                return false;
            }
            std::uint64_t offset = 0;
            if (!checkedMultiply(
                    physicalBlock,
                    m_blockSize,
                    offset))
            {
                errorText = QStringLiteral("APFS 物理块地址发生溢出。");
                return false;
            }
            return m_reader.read(
                offset,
                m_blockSize,
                blockOut,
                errorText);
        }

        bool readBtreeNode(
            const std::uint64_t physicalBlock,
            QByteArray& blockOut,
            QString& errorText) const
        {
            if (!readBlock(physicalBlock, blockOut, errorText))
            {
                return false;
            }
            const std::uint32_t type =
                le32(dataPointer(blockOut) + 0x18U)
                & kObjectTypeMask;
            if (type != kBtreeObject && type != kBtreeNodeObject)
            {
                errorText = QStringLiteral(
                    "APFS 对象不是 B-tree 节点。");
                return false;
            }
            return true;
        }

        bool loadObjectMapRoot(
            const std::uint64_t objectPhysical,
            std::uint64_t& treeRootOut,
            QString& errorText) const
        {
            QByteArray object;
            if (!readBlock(objectPhysical, object, errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(object);
            if ((le32(bytes + 0x18U) & kObjectTypeMask)
                    != kOmapObject
                || object.size() < 0x38)
            {
                errorText = QStringLiteral("APFS OMAP 对象头无效。");
                return false;
            }
            treeRootOut = le64(bytes + 0x30U);
            if (treeRootOut == 0U || treeRootOut >= m_blockCount)
            {
                errorText = QStringLiteral("APFS OMAP 根节点块号无效。");
                return false;
            }
            return true;
        }

        bool parseNodeLayout(
            const QByteArray& block,
            ApfsNodeLayout& layoutOut,
            QString& errorText) const
        {
            if (block.size() != static_cast<qsizetype>(m_blockSize)
                || block.size()
                    < static_cast<qsizetype>(kNodeHeaderSize))
            {
                errorText = QStringLiteral("APFS B-tree 节点被截断。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(block);
            ApfsNodeLayout layout;
            layout.flags = le16(bytes + 0x20U);
            layout.level = le16(bytes + 0x22U);
            layout.keyCount = le32(bytes + 0x24U);
            layout.tableOffset =
                kNodeHeaderSize + le16(bytes + 0x28U);
            layout.tableLength = le16(bytes + 0x2AU);
            layout.keysOffset =
                layout.tableOffset + layout.tableLength;
            layout.valuesEnd =
                m_blockSize
                - ((layout.flags & kNodeRoot) != 0U
                    ? kRootInfoSize
                    : 0U);
            layout.fixed =
                (layout.flags & kNodeFixedKeyValue) != 0U;
            const std::uint64_t tocBytes =
                static_cast<std::uint64_t>(layout.keyCount)
                * (layout.fixed ? 4U : 8U);
            if (layout.level > ks::misc::rawfs::kMaximumTreeDepth
                || layout.tableOffset < kNodeHeaderSize
                || layout.tableOffset > layout.valuesEnd
                || layout.tableLength > layout.valuesEnd - layout.tableOffset
                || layout.keysOffset > layout.valuesEnd
                || tocBytes > layout.tableLength)
            {
                errorText = QStringLiteral("APFS B-tree 节点布局无效。");
                return false;
            }
            layoutOut = layout;
            return true;
        }

        bool fixedRecord(
            const QByteArray& block,
            const ApfsNodeLayout& layout,
            const std::uint32_t index,
            const std::uint16_t keySize,
            const std::uint16_t valueSize,
            ApfsRecordView& recordOut,
            QString& errorText) const
        {
            if (index >= layout.keyCount)
            {
                errorText = QStringLiteral("APFS 固定记录索引越界。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(block);
            const unsigned char* const toc =
                bytes + layout.tableOffset + index * 4U;
            const std::uint32_t keyOffset =
                layout.keysOffset + le16(toc);
            const std::uint16_t valueOffset = le16(toc + 2U);
            if (keyOffset > layout.valuesEnd
                || keySize > layout.valuesEnd - keyOffset
                || valueOffset > layout.valuesEnd
                || valueSize > layout.valuesEnd - valueOffset)
            {
                errorText = QStringLiteral("APFS 固定记录范围越界。");
                return false;
            }
            const std::uint32_t valueBegin =
                layout.valuesEnd - valueOffset;
            if (valueBegin > layout.valuesEnd
                || valueSize > layout.valuesEnd - valueBegin)
            {
                errorText = QStringLiteral("APFS 固定记录值范围越界。");
                return false;
            }
            recordOut.key = bytes + keyOffset;
            recordOut.keyLength = keySize;
            recordOut.value = bytes + valueBegin;
            recordOut.valueLength = valueSize;
            return true;
        }

        bool variableRecord(
            const QByteArray& block,
            const ApfsNodeLayout& layout,
            const std::uint32_t index,
            const std::uint64_t physicalBlock,
            ApfsRecordView& recordOut,
            QString& errorText) const
        {
            if (index >= layout.keyCount)
            {
                errorText = QStringLiteral("APFS 可变记录索引越界。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(block);
            const unsigned char* const toc =
                bytes + layout.tableOffset + index * 8U;
            const std::uint16_t keyRelative = le16(toc);
            const std::uint16_t keyLength = le16(toc + 2U);
            const std::uint16_t valueRelative = le16(toc + 4U);
            const std::uint16_t valueLength = le16(toc + 6U);
            const std::uint32_t keyOffset =
                layout.keysOffset + keyRelative;
            if (keyOffset > layout.valuesEnd
                || keyLength > layout.valuesEnd - keyOffset
                || valueRelative > layout.valuesEnd)
            {
                errorText = QStringLiteral("APFS 可变记录键值范围越界。");
                return false;
            }
            const std::uint32_t valueOffset =
                layout.valuesEnd - valueRelative;
            if (valueOffset > layout.valuesEnd
                || valueLength > layout.valuesEnd - valueOffset)
            {
                errorText = QStringLiteral("APFS 可变记录值范围越界。");
                return false;
            }
            recordOut.key = bytes + keyOffset;
            recordOut.keyLength = keyLength;
            recordOut.value = bytes + valueOffset;
            recordOut.valueLength = valueLength;
            recordOut.valueAbsoluteOffset =
                m_reader.partitionOffset()
                + physicalBlock * m_blockSize
                + valueOffset;
            return true;
        }

        bool objectMapLookup(
            const std::uint64_t treeRoot,
            const std::uint64_t requestedObject,
            const std::uint64_t requestedTransaction,
            std::uint64_t& physicalOut,
            QString& errorText) const
        {
            std::uint64_t current = treeRoot;
            for (std::uint32_t depth = 0;
                 depth < kMaximumObjectMapDepth;
                 ++depth)
            {
                QByteArray block;
                if (!readBtreeNode(current, block, errorText))
                {
                    return false;
                }
                ApfsNodeLayout layout;
                if (!parseNodeLayout(block, layout, errorText)
                    || !layout.fixed)
                {
                    errorText = QStringLiteral(
                        "APFS OMAP 节点不是固定键值布局。");
                    return false;
                }

                std::int64_t best = -1;
                std::uint64_t bestObject = 0;
                std::uint64_t bestTransaction = 0;
                for (std::uint32_t index = 0;
                     index < layout.keyCount;
                     ++index)
                {
                    ApfsRecordView record;
                    const std::uint16_t valueSize =
                        layout.level == 0U ? 16U : 8U;
                    if (!fixedRecord(
                            block,
                            layout,
                            index,
                            16U,
                            valueSize,
                            record,
                            errorText))
                    {
                        return false;
                    }
                    const std::uint64_t objectId = le64(record.key);
                    const std::uint64_t transaction =
                        le64(record.key + 8U);
                    if (objectId > requestedObject
                        || (objectId == requestedObject
                            && transaction > requestedTransaction))
                    {
                        continue;
                    }
                    if (best < 0
                        || objectId > bestObject
                        || (objectId == bestObject
                            && transaction >= bestTransaction))
                    {
                        best = index;
                        bestObject = objectId;
                        bestTransaction = transaction;
                    }
                }
                if (best < 0)
                {
                    best = 0;
                }
                ApfsRecordView selected;
                if (!fixedRecord(
                        block,
                        layout,
                        static_cast<std::uint32_t>(best),
                        16U,
                        layout.level == 0U ? 16U : 8U,
                        selected,
                        errorText))
                {
                    return false;
                }
                if (layout.level == 0U)
                {
                    if (le64(selected.key) != requestedObject)
                    {
                        errorText = QStringLiteral(
                            "APFS OMAP 中不存在请求的对象。");
                        return false;
                    }
                    physicalOut = le64(selected.value + 8U);
                    if (physicalOut == 0U
                        || physicalOut >= m_blockCount)
                    {
                        errorText = QStringLiteral(
                            "APFS OMAP 返回了无效物理块号。");
                        return false;
                    }
                    return true;
                }
                current = le64(selected.value);
                if (current == 0U || current >= m_blockCount)
                {
                    errorText = QStringLiteral(
                        "APFS OMAP 内部节点引用无效。");
                    return false;
                }
            }
            errorText = QStringLiteral("APFS OMAP 深度超过安全上限。");
            return false;
        }

        template<typename Callback>
        bool walkPrefix(
            const std::uint64_t physicalBlock,
            const std::uint64_t requestedPrefix,
            Callback&& callback,
            std::vector<std::uint64_t>& visited,
            QString& errorText)
        {
            if (m_walkStopRequested)
            {
                return true;
            }
            if (visited.size() >= kMaximumVisitedNodes
                || std::find(
                    visited.cbegin(),
                    visited.cend(),
                    physicalBlock) != visited.cend())
            {
                errorText = QStringLiteral(
                    "APFS 文件系统树包含循环或节点数超过安全上限。");
                return false;
            }
            visited.push_back(physicalBlock);

            QByteArray block;
            if (!readBtreeNode(physicalBlock, block, errorText))
            {
                return false;
            }
            ApfsNodeLayout layout;
            if (!parseNodeLayout(block, layout, errorText)
                || layout.fixed)
            {
                errorText = QStringLiteral(
                    "APFS 文件系统树节点布局无效。");
                return false;
            }

            if (layout.level == 0U)
            {
                for (std::uint32_t index = 0;
                     index < layout.keyCount;
                     ++index)
                {
                    ApfsRecordView record;
                    if (!variableRecord(
                            block,
                            layout,
                            index,
                            physicalBlock,
                            record,
                            errorText))
                    {
                        return false;
                    }
                    if (record.keyLength < 8U)
                    {
                        continue;
                    }
                    const int comparison =
                        comparePrefix(le64(record.key), requestedPrefix);
                    if (comparison < 0)
                    {
                        continue;
                    }
                    if (comparison > 0)
                    {
                        break;
                    }
                    if (!callback(record))
                    {
                        m_walkStopRequested = true;
                        return true;
                    }
                }
                return true;
            }

            for (std::uint32_t index = 0;
                 index < layout.keyCount;
                 ++index)
            {
                ApfsRecordView record;
                if (!variableRecord(
                        block,
                        layout,
                        index,
                        physicalBlock,
                        record,
                        errorText))
                {
                    return false;
                }
                if (record.keyLength < 8U
                    || record.valueLength < 8U)
                {
                    continue;
                }
                const int currentComparison =
                    comparePrefix(le64(record.key), requestedPrefix);
                if (currentComparison > 0)
                {
                    break;
                }
                if (index + 1U < layout.keyCount)
                {
                    ApfsRecordView next;
                    if (!variableRecord(
                            block,
                            layout,
                            index + 1U,
                            physicalBlock,
                            next,
                            errorText))
                    {
                        return false;
                    }
                    if (next.keyLength >= 8U
                        && comparePrefix(
                            le64(next.key),
                            requestedPrefix) < 0)
                    {
                        continue;
                    }
                }
                const std::uint64_t childReference =
                    le64(record.value);
                std::uint64_t childPhysical = childReference;
                if (!m_fileSystemTreePhysical
                    && !objectMapLookup(
                        m_volumeOmapRoot,
                        childReference,
                        std::numeric_limits<std::uint64_t>::max(),
                        childPhysical,
                        errorText))
                {
                    return false;
                }
                if (!walkPrefix(
                        childPhysical,
                        requestedPrefix,
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
            ApfsInode& inodeOut,
            QString& errorText)
        {
            bool found = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            const std::uint64_t prefix =
                recordPrefix(inodeNumber, kInodeRecordType);
            if (!walkPrefix(
                    m_fileSystemTreeRoot,
                    prefix,
                    [&](const ApfsRecordView& record)
                    {
                        if (record.valueLength < 92U)
                        {
                            errorText = QStringLiteral(
                                "APFS inode 记录被截断。");
                            return false;
                        }
                        ApfsInode inode;
                        inode.number = inodeNumber;
                        inode.parentId = le64(record.value);
                        inode.internalFlags =
                            le64(record.value + 48U);
                        inode.bsdFlags =
                            le32(record.value + 68U);
                        inode.mode = le16(record.value + 80U);
                        inode.metadataOffset =
                            record.valueAbsoluteOffset;
                        parseDstream(
                            record.value,
                            record.valueLength,
                            inode);
                        inodeOut = inode;
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
                    "APFS inode 不存在：%1").arg(inodeNumber);
                return false;
            }
            return true;
        }

        static void parseDstream(
            const unsigned char* value,
            const std::uint16_t valueLength,
            ApfsInode& inode)
        {
            if (valueLength < 96U)
            {
                return;
            }
            const unsigned char* const blob = value + 92U;
            const std::uint16_t fieldCount = le16(blob);
            if (fieldCount > 64U)
            {
                return;
            }
            const std::uint32_t descriptorsEnd =
                96U + static_cast<std::uint32_t>(fieldCount) * 4U;
            if (descriptorsEnd > valueLength)
            {
                return;
            }
            const unsigned char* data = value + descriptorsEnd;
            const unsigned char* const end = value + valueLength;
            for (std::uint16_t index = 0;
                 index < fieldCount;
                 ++index)
            {
                const unsigned char* const field = blob + 4U + index * 4U;
                const std::uint8_t type = field[0U];
                const std::uint16_t size = le16(field + 2U);
                const std::uint32_t alignedSize =
                    (static_cast<std::uint32_t>(size) + 7U) & ~7U;
                if (data > end
                    || static_cast<std::uint64_t>(end - data)
                        < alignedSize)
                {
                    return;
                }
                if (type == kDstreamExtendedField && size >= 40U)
                {
                    inode.size = le64(data);
                    inode.allocated = le64(data + 8U);
                }
                data += alignedSize;
            }
        }

        bool enumerateDirectory(
            const std::uint64_t parentInode,
            const std::uint32_t maximumEntries,
            std::vector<ApfsDirectoryChild>& childrenOut,
            std::uint64_t& scannedRecordsOut,
            bool& truncatedOut,
            QString& errorText)
        {
            childrenOut.clear();
            scannedRecordsOut = 0;
            truncatedOut = false;
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            const std::uint64_t prefix =
                recordPrefix(parentInode, kDirectoryRecordType);
            const bool walked = walkPrefix(
                m_fileSystemTreeRoot,
                prefix,
                [&](const ApfsRecordView& record)
                {
                    if (++scannedRecordsOut > kMaximumDirectoryRecords)
                    {
                        errorText = QStringLiteral(
                            "APFS 目录记录数超过安全上限。");
                        return false;
                    }
                    ApfsDirectoryChild child;
                    if (!parseDirectoryRecord(record, child, errorText))
                    {
                        return false;
                    }
                    if (childrenOut.size() >= maximumEntries)
                    {
                        truncatedOut = true;
                        return false;
                    }
                    childrenOut.push_back(std::move(child));
                    return true;
                },
                visited,
                errorText);
            return walked && errorText.isEmpty();
        }

        bool parseDirectoryRecord(
            const ApfsRecordView& record,
            ApfsDirectoryChild& childOut,
            QString& errorText) const
        {
            const std::uint32_t keyHeader =
                m_caseInsensitive ? 12U : 10U;
            if (record.keyLength < keyHeader
                || record.valueLength < 18U)
            {
                errorText = QStringLiteral("APFS 目录记录被截断。");
                return false;
            }
            std::uint32_t nameLength =
                m_caseInsensitive
                    ? le32(record.key + 8U) & 0x3FFU
                    : le16(record.key + 8U);
            if (nameLength > record.keyLength - keyHeader)
            {
                errorText = QStringLiteral("APFS 目录名称范围越界。");
                return false;
            }
            if (nameLength != 0U
                && record.key[keyHeader + nameLength - 1U] == 0U)
            {
                --nameLength;
            }
            ApfsDirectoryChild child;
            child.name = QString::fromUtf8(
                reinterpret_cast<const char*>(
                    record.key + keyHeader),
                static_cast<qsizetype>(nameLength));
            child.inode = le64(record.value);
            child.flags = le16(record.value + 16U);
            childOut = std::move(child);
            return true;
        }

        bool findDirectoryChild(
            const std::uint64_t parentInode,
            const QString& requestedName,
            ApfsDirectoryChild& childOut,
            QString& errorText)
        {
            std::vector<ApfsDirectoryChild> children;
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
                [&](const ApfsDirectoryChild& child)
                {
                    return QString::compare(
                        child.name,
                        requestedName,
                        m_caseInsensitive
                            ? Qt::CaseInsensitive
                            : Qt::CaseSensitive) == 0;
                });
            if (iterator == children.cend())
            {
                errorText = QStringLiteral(
                    "APFS 目录中不存在对象：%1")
                    .arg(requestedName);
                return false;
            }
            childOut = *iterator;
            return true;
        }

        bool loadExtents(
            const std::uint64_t inodeNumber,
            std::vector<ApfsExtent>& extentsOut,
            QString& errorText)
        {
            extentsOut.clear();
            std::vector<std::uint64_t> visited;
            m_walkStopRequested = false;
            const std::uint64_t prefix =
                recordPrefix(inodeNumber, kFileExtentRecordType);
            if (!walkPrefix(
                    m_fileSystemTreeRoot,
                    prefix,
                    [&](const ApfsRecordView& record)
                    {
                        if (record.keyLength < 16U
                            || record.valueLength < 24U)
                        {
                            errorText = QStringLiteral(
                                "APFS file extent 记录被截断。");
                            return false;
                        }
                        const std::uint64_t lengthAndFlags =
                            le64(record.value);
                        ApfsExtent extent;
                        extent.logicalOffset =
                            le64(record.key + 8U);
                        extent.length =
                            lengthAndFlags & kObjectIdMask;
                        extent.flags = static_cast<std::uint8_t>(
                            lengthAndFlags >> kObjectTypeShift);
                        extent.physicalBlock =
                            le64(record.value + 8U);
                        extent.cryptoId =
                            le64(record.value + 16U);
                        extent.metadataOffset =
                            record.valueAbsoluteOffset;
                        if (extent.length != 0U)
                        {
                            extentsOut.push_back(extent);
                        }
                        if (extentsOut.size()
                            > ks::misc::rawfs::kMaximumExtentCount * 16U)
                        {
                            errorText = QStringLiteral(
                                "APFS 文件 extent 数量超过安全上限。");
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
                extentsOut.begin(),
                extentsOut.end(),
                [](const ApfsExtent& left, const ApfsExtent& right)
                {
                    return left.logicalOffset < right.logicalOffset;
                });
            return true;
        }

        std::vector<ks::misc::RawFileExtent> describeExtents(
            const ApfsInode& inode,
            const std::vector<ApfsExtent>& extents) const
        {
            std::vector<ks::misc::RawFileExtent> result;
            for (const ApfsExtent& source : extents)
            {
                if (result.size()
                        >= ks::misc::rawfs::kMaximumExtentCount
                    || source.logicalOffset >= inode.size)
                {
                    break;
                }
                ks::misc::RawFileExtent extent;
                extent.logicalOffset = source.logicalOffset;
                extent.lengthBytes = std::min<std::uint64_t>(
                    source.length,
                    inode.size - source.logicalOffset);
                extent.sparse = source.physicalBlock == 0U;
                extent.compressed =
                    (inode.bsdFlags & kCompressedBsdFlag) != 0U;
                if (!extent.sparse)
                {
                    std::uint64_t relative = 0;
                    if (checkedMultiply(
                            source.physicalBlock,
                            m_blockSize,
                            relative))
                    {
                        extent.absoluteOffset =
                            m_reader.partitionOffset() + relative;
                        extent.physicalMappingExact = true;
                    }
                }
                result.push_back(extent);
            }
            return result;
        }

        static ks::misc::RawFileObjectType objectType(
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

        void fillEntry(
            const ApfsInode& inode,
            const QString& name,
            const QString& fullPath,
            const std::vector<ApfsExtent>& extents,
            ks::misc::RawFileEntry& entry) const
        {
            entry.name = name;
            entry.fullPath = fullPath;
            entry.type = objectType(inode.mode);
            entry.objectId = inode.number;
            entry.parentObjectId = inode.parentId;
            entry.fileSizeBytes = inode.size;
            entry.allocatedSizeBytes = inode.allocated;
            entry.metadataOffset = inode.metadataOffset;
            entry.modeOrFlags =
                inode.bsdFlags
                | static_cast<std::uint32_t>(inode.mode)
                | static_cast<std::uint32_t>(inode.internalFlags);
            entry.extents = describeExtents(inode, extents);
            entry.extentsTruncated =
                extents.size() > ks::misc::rawfs::kMaximumExtentCount;
        }

        const ks::misc::rawfs::VolumeReader& m_reader;
        std::uint32_t m_blockSize = 0;
        std::uint64_t m_blockCount = 0;
        std::uint64_t m_incompatibilityFlags = 0;
        std::uint64_t m_containerOmapRoot = 0;
        std::uint64_t m_volumeOmapRoot = 0;
        std::uint64_t m_fileSystemTreeRoot = 0;
        bool m_caseInsensitive = false;
        bool m_encryptionRolled = false;
        bool m_fileSystemTreePhysical = false;
        bool m_walkStopRequested = false;
    };

    ks::misc::RawDirectoryResult makeDirectoryResult(
        const QString& path)
    {
        ks::misc::RawDirectoryResult result;
        result.fileSystem =
            ks::misc::ForensicFileSystemKind::Apfs;
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
            ks::misc::ForensicFileSystemKind::Apfs;
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
    RawDirectoryResult listApfs(
        const VolumeReader& reader,
        const QString& path,
        const std::uint32_t maximumEntries)
    {
        RawDirectoryResult result = makeDirectoryResult(path);
        ApfsVolume volume(reader);
        if (!volume.mount(result.errorText))
        {
            return result;
        }
        ApfsInode directory;
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

    RawFileReadResult readApfs(
        const VolumeReader& reader,
        const QString& path,
        const std::uint64_t offset,
        const std::uint32_t length)
    {
        struct FileState
        {
            ApfsVolume volume;
            ApfsInode data;
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
