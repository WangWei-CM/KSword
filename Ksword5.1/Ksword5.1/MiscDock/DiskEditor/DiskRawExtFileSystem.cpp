#include "DiskRawFileSystemInternal.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    constexpr std::uint16_t kExtMagic = 0xEF53U;
    constexpr std::uint16_t kExtentMagic = 0xF30AU;
    constexpr std::uint32_t kExtentsFlag = 0x00080000U;
    constexpr std::uint32_t kFileTypeMask = 0xF000U;
    constexpr std::uint32_t kRegularFileMode = 0x8000U;
    constexpr std::uint32_t kDirectoryMode = 0x4000U;
    constexpr std::uint32_t kSymbolicLinkMode = 0xA000U;
    constexpr std::uint32_t kMaximumLogicalBlocks = 2U * 1024U * 1024U;

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

    std::uint64_t combine32(
        const std::uint32_t low,
        const std::uint32_t high)
    {
        return static_cast<std::uint64_t>(low)
            | (static_cast<std::uint64_t>(high) << 32U);
    }

    const unsigned char* dataPointer(const QByteArray& bytes)
    {
        return reinterpret_cast<const unsigned char*>(bytes.constData());
    }

    struct ExtInode
    {
        std::uint64_t number = 0;
        std::uint64_t metadataOffset = 0;
        std::uint64_t sizeBytes = 0;
        std::uint64_t allocatedBytes = 0;
        std::uint32_t flags = 0;
        std::uint16_t mode = 0;
        std::array<unsigned char, 60U> blockData{};
    };

    struct ExtBlockMapping
    {
        bool mapped = false;
        bool unwritten = false;
        std::uint64_t physicalBlock = 0;
    };

    struct ExtLogicalRun
    {
        std::uint64_t logicalBlock = 0;
        std::uint64_t physicalBlock = 0;
        std::uint64_t blockCount = 0;
        bool sparse = false;
        bool unwritten = false;
    };

    class ExtVolume final
    {
    public:
        explicit ExtVolume(const ks::misc::rawfs::VolumeReader& reader)
            : m_reader(reader)
        {
        }

        bool mount(QString& errorText)
        {
            QByteArray superblock;
            if (!m_reader.read(1024U, 1024U, superblock, errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(superblock);
            if (le16(bytes + 0x38U) != kExtMagic)
            {
                errorText = QStringLiteral("Ext 超级块魔数不匹配。");
                return false;
            }

            const std::uint32_t logarithm = le32(bytes + 0x18U);
            if (logarithm > 6U)
            {
                errorText = QStringLiteral("Ext 块大小指数无效。");
                return false;
            }
            m_blockSize = 1024U << logarithm;
            m_firstDataBlock = le32(bytes + 0x14U);
            m_blocksPerGroup = le32(bytes + 0x20U);
            m_inodesPerGroup = le32(bytes + 0x28U);
            m_inodeCount = le32(bytes + 0x00U);
            m_incompatFeatures = le32(bytes + 0x60U);
            m_inodeSize = le16(bytes + 0x58U);
            if (m_inodeSize == 0U)
            {
                m_inodeSize = 128U;
            }
            m_descriptorSize = le16(bytes + 0xFEU);
            if (m_descriptorSize < 32U)
            {
                m_descriptorSize = 32U;
            }
            if (m_descriptorSize > m_blockSize || m_inodeSize > m_blockSize
                || m_inodeSize < 128U || m_blocksPerGroup == 0U
                || m_inodesPerGroup == 0U)
            {
                errorText = QStringLiteral("Ext 几何参数超出安全边界。");
                return false;
            }

            const std::uint32_t blocksHigh =
                (m_incompatFeatures & 0x80U) != 0U
                ? le32(bytes + 0x150U)
                : 0U;
            m_blockCount = combine32(le32(bytes + 0x04U), blocksHigh);
            if (m_blockCount <= m_firstDataBlock
                || m_blockCount
                    > m_reader.partitionLength()
                        / static_cast<std::uint64_t>(m_blockSize))
            {
                errorText = QStringLiteral("Ext 块总数越过所选分区。");
                return false;
            }

            const std::uint64_t dataBlocks =
                m_blockCount - m_firstDataBlock;
            m_groupCount =
                (dataBlocks + m_blocksPerGroup - 1U) / m_blocksPerGroup;
            if (m_groupCount == 0U || m_groupCount > 0x1000000ULL)
            {
                errorText = QStringLiteral("Ext 块组数量无效。");
                return false;
            }
            return true;
        }

        bool resolvePath(
            const QString& path,
            ExtInode& inodeOut,
            QString& canonicalPathOut,
            QString& errorText)
        {
            ExtInode current;
            if (!loadInode(2U, current, errorText))
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
                if ((current.mode & kFileTypeMask) != kDirectoryMode)
                {
                    errorText = QStringLiteral(
                        "路径中间对象不是目录：%1").arg(canonicalPathOut);
                    return false;
                }
                std::uint64_t childInode = 0;
                if (!findDirectoryChild(
                        current,
                        component,
                        childInode,
                        errorText))
                {
                    return false;
                }
                if (!loadInode(childInode, current, errorText))
                {
                    return false;
                }
                canonicalPathOut =
                    ks::misc::rawfs::childPath(
                        canonicalPathOut,
                        component);
            }
            inodeOut = current;
            return true;
        }

        bool listDirectory(
            const ExtInode& directory,
            const QString& canonicalPath,
            const std::uint32_t maximumEntries,
            ks::misc::RawDirectoryResult& result)
        {
            if ((directory.mode & kFileTypeMask) != kDirectoryMode)
            {
                result.errorText = QStringLiteral("目标对象不是 Ext 目录。");
                return false;
            }

            const std::uint64_t logicalBlocks =
                (directory.sizeBytes + m_blockSize - 1U) / m_blockSize;
            const std::uint64_t boundedBlocks =
                std::min<std::uint64_t>(
                    logicalBlocks,
                    kMaximumLogicalBlocks);
            QString errorText;
            for (std::uint64_t blockIndex = 0;
                 blockIndex < boundedBlocks;
                 ++blockIndex)
            {
                ExtBlockMapping mapping;
                if (!mapBlock(directory, blockIndex, mapping, errorText))
                {
                    result.errorText = errorText;
                    return false;
                }
                if (!mapping.mapped || mapping.unwritten)
                {
                    continue;
                }

                QByteArray block;
                if (!readBlock(mapping.physicalBlock, block, errorText))
                {
                    result.errorText = errorText;
                    return false;
                }
                const unsigned char* const bytes = dataPointer(block);
                std::uint32_t offset = 0;
                while (offset + 8U <= m_blockSize)
                {
                    const std::uint32_t inodeNumber =
                        le32(bytes + offset);
                    const std::uint16_t recordLength =
                        le16(bytes + offset + 4U);
                    const std::uint8_t nameLength =
                        bytes[offset + 6U];
                    if (recordLength < 8U
                        || (recordLength & 3U) != 0U
                        || offset + recordLength > m_blockSize)
                    {
                        break;
                    }
                    ++result.scannedRecords;
                    if (inodeNumber != 0U
                        && nameLength != 0U
                        && nameLength <= recordLength - 8U)
                    {
                        const QString name = QString::fromUtf8(
                            reinterpret_cast<const char*>(
                                bytes + offset + 8U),
                            nameLength);
                        if (name != QStringLiteral(".")
                            && name != QStringLiteral(".."))
                        {
                            if (result.entries.size() >= maximumEntries)
                            {
                                result.truncated = true;
                                return true;
                            }
                            ExtInode child;
                            if (!loadInode(
                                    inodeNumber,
                                    child,
                                    errorText))
                            {
                                result.errorText = errorText;
                                return false;
                            }
                            ks::misc::RawFileEntry entry;
                            fillEntry(
                                child,
                                directory.number,
                                name,
                                ks::misc::rawfs::childPath(
                                    canonicalPath,
                                    name),
                                entry);
                            if (!collectExtents(
                                    child,
                                    entry.extents,
                                    entry.extentsTruncated,
                                    errorText))
                            {
                                result.errorText = errorText;
                                return false;
                            }
                            for (const auto& extent : entry.extents)
                            {
                                if (!extent.sparse)
                                {
                                    entry.allocatedSizeBytes +=
                                        extent.lengthBytes;
                                }
                            }
                            result.entries.push_back(std::move(entry));
                        }
                    }
                    offset += recordLength;
                }
            }
            result.truncated = logicalBlocks > boundedBlocks;
            return true;
        }

        bool readFile(
            const ExtInode& inode,
            const std::uint64_t offset,
            const std::uint32_t requestedLength,
            ks::misc::RawFileReadResult& result)
        {
            const std::uint32_t type = inode.mode & kFileTypeMask;
            if (type != kRegularFileMode && type != kSymbolicLinkMode)
            {
                result.errorText = QStringLiteral(
                    "目标 Ext 对象不是可读取的文件或符号链接。");
                return false;
            }
            result.fileSizeBytes = inode.sizeBytes;
            result.requestedOffset = offset;
            if (offset >= inode.sizeBytes)
            {
                result.success = true;
                result.endOfFile = true;
                return collectExtents(
                    inode,
                    result.extents,
                    result.extentsTruncated,
                    result.errorText);
            }

            const std::uint64_t available = inode.sizeBytes - offset;
            const std::uint32_t length = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    available,
                    requestedLength));
            result.bytes = QByteArray(
                static_cast<qsizetype>(length),
                '\0');

            if (type == kSymbolicLinkMode
                && inode.sizeBytes <= inode.blockData.size()
                && (inode.flags & kExtentsFlag) == 0U)
            {
                std::memcpy(
                    result.bytes.data(),
                    inode.blockData.data()
                        + static_cast<std::size_t>(offset),
                    length);
            }
            else
            {
                std::uint64_t completed = 0;
                QString errorText;
                while (completed < length)
                {
                    const std::uint64_t fileOffset = offset + completed;
                    const std::uint64_t logicalBlock =
                        fileOffset / m_blockSize;
                    const std::uint32_t offsetInBlock =
                        static_cast<std::uint32_t>(
                            fileOffset % m_blockSize);
                    const std::uint32_t take =
                        static_cast<std::uint32_t>(
                            std::min<std::uint64_t>(
                                length - completed,
                                m_blockSize - offsetInBlock));
                    ExtBlockMapping mapping;
                    if (!mapBlock(
                            inode,
                            logicalBlock,
                            mapping,
                            errorText))
                    {
                        result.errorText = errorText;
                        return false;
                    }
                    if (mapping.mapped && !mapping.unwritten)
                    {
                        QByteArray bytes;
                        if (!m_reader.read(
                                mapping.physicalBlock * m_blockSize
                                    + offsetInBlock,
                                take,
                                bytes,
                                errorText))
                        {
                            result.errorText = errorText;
                            return false;
                        }
                        std::copy(
                            bytes.cbegin(),
                            bytes.cend(),
                            result.bytes.begin()
                                + static_cast<qsizetype>(completed));
                    }
                    completed += take;
                }
            }

            if (!collectExtents(
                    inode,
                    result.extents,
                    result.extentsTruncated,
                    result.errorText))
            {
                return false;
            }
            result.endOfFile =
                offset + static_cast<std::uint64_t>(length)
                >= inode.sizeBytes;
            result.success = true;
            return true;
        }

    private:
        bool readBlock(
            const std::uint64_t block,
            QByteArray& bytes,
            QString& errorText) const
        {
            if (block >= m_blockCount)
            {
                errorText = QStringLiteral("Ext 块号越过文件系统边界。");
                return false;
            }
            return m_reader.read(
                block * m_blockSize,
                m_blockSize,
                bytes,
                errorText);
        }

        bool loadInode(
            const std::uint64_t inodeNumber,
            ExtInode& inodeOut,
            QString& errorText)
        {
            if (inodeNumber == 0U || inodeNumber > m_inodeCount)
            {
                errorText = QStringLiteral("Ext inode 编号越界。");
                return false;
            }
            const std::uint64_t zeroBased = inodeNumber - 1U;
            const std::uint64_t group = zeroBased / m_inodesPerGroup;
            const std::uint64_t index = zeroBased % m_inodesPerGroup;
            if (group >= m_groupCount)
            {
                errorText = QStringLiteral("Ext inode 块组越界。");
                return false;
            }

            const std::uint64_t descriptorTableBlock =
                static_cast<std::uint64_t>(m_firstDataBlock) + 1U;
            const std::uint64_t descriptorOffset =
                descriptorTableBlock * m_blockSize
                + group * m_descriptorSize;
            QByteArray descriptor;
            if (!m_reader.read(
                    descriptorOffset,
                    m_descriptorSize,
                    descriptor,
                    errorText))
            {
                return false;
            }
            const unsigned char* const descriptorBytes =
                dataPointer(descriptor);
            const std::uint32_t inodeTableLow =
                le32(descriptorBytes + 8U);
            const std::uint32_t inodeTableHigh =
                m_descriptorSize >= 64U
                ? le32(descriptorBytes + 40U)
                : 0U;
            const std::uint64_t inodeTable =
                combine32(inodeTableLow, inodeTableHigh);
            if (inodeTable == 0U || inodeTable >= m_blockCount)
            {
                errorText = QStringLiteral("Ext inode 表块号无效。");
                return false;
            }

            const std::uint64_t inodeOffset =
                inodeTable * m_blockSize + index * m_inodeSize;
            QByteArray inodeBytes;
            if (!m_reader.read(
                    inodeOffset,
                    m_inodeSize,
                    inodeBytes,
                    errorText))
            {
                return false;
            }
            const unsigned char* const bytes = dataPointer(inodeBytes);
            ExtInode inode;
            inode.number = inodeNumber;
            inode.metadataOffset =
                m_reader.partitionOffset() + inodeOffset;
            inode.mode = le16(bytes + 0U);
            inode.flags = le32(bytes + 32U);
            const std::uint32_t sizeHigh =
                (inode.mode & kFileTypeMask) == kRegularFileMode
                || (inode.mode & kFileTypeMask) == kSymbolicLinkMode
                ? le32(bytes + 108U)
                : 0U;
            inode.sizeBytes = combine32(le32(bytes + 4U), sizeHigh);
            inode.allocatedBytes =
                static_cast<std::uint64_t>(le32(bytes + 28U)) * 512U;
            std::copy_n(
                bytes + 40U,
                inode.blockData.size(),
                inode.blockData.begin());
            inodeOut = inode;
            return true;
        }

        bool findDirectoryChild(
            const ExtInode& directory,
            const QString& wantedName,
            std::uint64_t& inodeOut,
            QString& errorText)
        {
            const std::uint64_t logicalBlocks =
                (directory.sizeBytes + m_blockSize - 1U) / m_blockSize;
            if (logicalBlocks > kMaximumLogicalBlocks)
            {
                errorText = QStringLiteral(
                    "Ext 目录数据超过安全扫描上限。");
                return false;
            }
            for (std::uint64_t blockIndex = 0;
                 blockIndex < logicalBlocks;
                 ++blockIndex)
            {
                ExtBlockMapping mapping;
                if (!mapBlock(directory, blockIndex, mapping, errorText))
                {
                    return false;
                }
                if (!mapping.mapped || mapping.unwritten)
                {
                    continue;
                }
                QByteArray block;
                if (!readBlock(mapping.physicalBlock, block, errorText))
                {
                    return false;
                }
                const unsigned char* const bytes = dataPointer(block);
                std::uint32_t offset = 0;
                while (offset + 8U <= m_blockSize)
                {
                    const std::uint32_t child = le32(bytes + offset);
                    const std::uint16_t recordLength =
                        le16(bytes + offset + 4U);
                    const std::uint8_t nameLength =
                        bytes[offset + 6U];
                    if (recordLength < 8U
                        || (recordLength & 3U) != 0U
                        || offset + recordLength > m_blockSize)
                    {
                        break;
                    }
                    if (child != 0U && nameLength != 0U
                        && nameLength <= recordLength - 8U)
                    {
                        const QString name = QString::fromUtf8(
                            reinterpret_cast<const char*>(
                                bytes + offset + 8U),
                            nameLength);
                        if (name == wantedName)
                        {
                            inodeOut = child;
                            return true;
                        }
                    }
                    offset += recordLength;
                }
            }
            errorText = QStringLiteral("Ext 路径不存在：%1")
                .arg(wantedName);
            return false;
        }

        bool mapExtentNode(
            const QByteArray& node,
            const std::uint64_t logicalBlock,
            const std::uint32_t recursionDepth,
            ExtBlockMapping& mapping,
            QString& errorText)
        {
            if (node.size() < 12 || recursionDepth > 8U)
            {
                errorText = QStringLiteral("Ext extent 树深度无效。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(node);
            if (le16(bytes) != kExtentMagic)
            {
                errorText = QStringLiteral("Ext extent 节点魔数无效。");
                return false;
            }
            const std::uint16_t entries = le16(bytes + 2U);
            const std::uint16_t maximum = le16(bytes + 4U);
            const std::uint16_t depth = le16(bytes + 6U);
            if (entries > maximum
                || 12U + static_cast<std::uint32_t>(entries) * 12U
                    > static_cast<std::uint32_t>(node.size()))
            {
                errorText = QStringLiteral("Ext extent 节点记录越界。");
                return false;
            }

            if (depth == 0U)
            {
                for (std::uint16_t index = 0; index < entries; ++index)
                {
                    const unsigned char* const extent =
                        bytes + 12U + index * 12U;
                    const std::uint64_t logical = le32(extent);
                    const std::uint16_t encodedLength =
                        le16(extent + 4U);
                    const bool unwritten =
                        (encodedLength & 0x8000U) != 0U;
                    std::uint64_t blockCount =
                        encodedLength & 0x7FFFU;
                    if (blockCount == 0U)
                    {
                        blockCount = 32768U;
                    }
                    if (logicalBlock >= logical
                        && logicalBlock - logical < blockCount)
                    {
                        const std::uint64_t physical =
                            static_cast<std::uint64_t>(
                                le16(extent + 6U)) << 32U
                            | le32(extent + 8U);
                        if (physical + logicalBlock - logical
                            >= m_blockCount)
                        {
                            errorText = QStringLiteral(
                                "Ext extent 物理块越界。");
                            return false;
                        }
                        mapping.mapped = true;
                        mapping.unwritten = unwritten;
                        mapping.physicalBlock =
                            physical + logicalBlock - logical;
                        return true;
                    }
                }
                return true;
            }

            const unsigned char* selected = nullptr;
            for (std::uint16_t index = 0; index < entries; ++index)
            {
                const unsigned char* const entry =
                    bytes + 12U + index * 12U;
                if (le32(entry) > logicalBlock)
                {
                    break;
                }
                selected = entry;
            }
            if (selected == nullptr)
            {
                return true;
            }
            const std::uint64_t childBlock =
                static_cast<std::uint64_t>(
                    le16(selected + 8U)) << 32U
                | le32(selected + 4U);
            QByteArray child;
            if (!readBlock(childBlock, child, errorText))
            {
                return false;
            }
            return mapExtentNode(
                child,
                logicalBlock,
                recursionDepth + 1U,
                mapping,
                errorText);
        }

        bool readIndirectPointer(
            const std::uint64_t block,
            const std::uint64_t index,
            std::uint64_t& pointerOut,
            QString& errorText)
        {
            const std::uint64_t pointersPerBlock = m_blockSize / 4U;
            if (block == 0U)
            {
                pointerOut = 0U;
                return true;
            }
            if (block >= m_blockCount || index >= pointersPerBlock)
            {
                errorText = QStringLiteral("Ext 间接块索引越界。");
                return false;
            }
            QByteArray pointerBytes;
            if (!m_reader.read(
                    block * m_blockSize + index * 4U,
                    4U,
                    pointerBytes,
                    errorText))
            {
                return false;
            }
            pointerOut = le32(dataPointer(pointerBytes));
            if (pointerOut >= m_blockCount && pointerOut != 0U)
            {
                errorText = QStringLiteral("Ext 间接块指针越界。");
                return false;
            }
            return true;
        }

        bool mapIndirectBlock(
            const ExtInode& inode,
            const std::uint64_t logicalBlock,
            ExtBlockMapping& mapping,
            QString& errorText)
        {
            const unsigned char* const pointers =
                inode.blockData.data();
            if (logicalBlock < 12U)
            {
                mapping.physicalBlock =
                    le32(pointers + logicalBlock * 4U);
                mapping.mapped = mapping.physicalBlock != 0U;
                return mapping.physicalBlock < m_blockCount;
            }

            const std::uint64_t perBlock = m_blockSize / 4U;
            std::uint64_t relative = logicalBlock - 12U;
            std::uint64_t current = 0;
            if (relative < perBlock)
            {
                return readIndirectPointer(
                    le32(pointers + 12U * 4U),
                    relative,
                    current,
                    errorText)
                    && ((mapping.physicalBlock = current),
                        (mapping.mapped = current != 0U),
                        true);
            }

            relative -= perBlock;
            if (perBlock != 0U
                && relative < perBlock * perBlock)
            {
                if (!readIndirectPointer(
                        le32(pointers + 13U * 4U),
                        relative / perBlock,
                        current,
                        errorText)
                    || !readIndirectPointer(
                        current,
                        relative % perBlock,
                        current,
                        errorText))
                {
                    return false;
                }
                mapping.physicalBlock = current;
                mapping.mapped = current != 0U;
                return true;
            }

            const std::uint64_t doubleCapacity =
                perBlock * perBlock;
            relative -= doubleCapacity;
            if (perBlock == 0U
                || relative / perBlock / perBlock >= perBlock)
            {
                errorText = QStringLiteral(
                    "Ext 逻辑块超过三级间接块范围。");
                return false;
            }
            if (!readIndirectPointer(
                    le32(pointers + 14U * 4U),
                    relative / doubleCapacity,
                    current,
                    errorText))
            {
                return false;
            }
            const std::uint64_t secondIndex =
                (relative % doubleCapacity) / perBlock;
            if (!readIndirectPointer(
                    current,
                    secondIndex,
                    current,
                    errorText)
                || !readIndirectPointer(
                    current,
                    relative % perBlock,
                    current,
                    errorText))
            {
                return false;
            }
            mapping.physicalBlock = current;
            mapping.mapped = current != 0U;
            return true;
        }

        bool mapBlock(
            const ExtInode& inode,
            const std::uint64_t logicalBlock,
            ExtBlockMapping& mapping,
            QString& errorText)
        {
            mapping = {};
            if ((inode.flags & kExtentsFlag) != 0U)
            {
                const QByteArray root(
                    reinterpret_cast<const char*>(
                        inode.blockData.data()),
                    static_cast<qsizetype>(inode.blockData.size()));
                return mapExtentNode(
                    root,
                    logicalBlock,
                    0U,
                    mapping,
                    errorText);
            }
            return mapIndirectBlock(
                inode,
                logicalBlock,
                mapping,
                errorText);
        }

        bool collectExtentNode(
            const QByteArray& node,
            const std::uint32_t recursionDepth,
            std::vector<ExtLogicalRun>& runs,
            bool& truncated,
            QString& errorText)
        {
            if (node.size() < 12 || recursionDepth > 8U)
            {
                errorText = QStringLiteral("Ext extent 树深度无效。");
                return false;
            }
            const unsigned char* const bytes = dataPointer(node);
            if (le16(bytes) != kExtentMagic)
            {
                errorText = QStringLiteral("Ext extent 节点魔数无效。");
                return false;
            }
            const std::uint16_t entries = le16(bytes + 2U);
            const std::uint16_t maximum = le16(bytes + 4U);
            const std::uint16_t depth = le16(bytes + 6U);
            if (entries > maximum
                || 12U + static_cast<std::uint32_t>(entries) * 12U
                    > static_cast<std::uint32_t>(node.size()))
            {
                errorText = QStringLiteral("Ext extent 节点记录越界。");
                return false;
            }
            for (std::uint16_t index = 0; index < entries; ++index)
            {
                if (runs.size() >= ks::misc::rawfs::kMaximumExtentCount)
                {
                    truncated = true;
                    return true;
                }
                const unsigned char* const entry =
                    bytes + 12U + index * 12U;
                if (depth == 0U)
                {
                    const std::uint16_t encodedLength =
                        le16(entry + 4U);
                    std::uint64_t blockCount =
                        encodedLength & 0x7FFFU;
                    if (blockCount == 0U)
                    {
                        blockCount = 32768U;
                    }
                    ExtLogicalRun run;
                    run.logicalBlock = le32(entry);
                    run.physicalBlock =
                        static_cast<std::uint64_t>(
                            le16(entry + 6U)) << 32U
                        | le32(entry + 8U);
                    run.blockCount = blockCount;
                    run.unwritten =
                        (encodedLength & 0x8000U) != 0U;
                    if (run.physicalBlock >= m_blockCount
                        || run.blockCount
                            > m_blockCount - run.physicalBlock)
                    {
                        errorText = QStringLiteral(
                            "Ext extent 物理范围越界。");
                        return false;
                    }
                    runs.push_back(run);
                }
                else
                {
                    const std::uint64_t childBlock =
                        static_cast<std::uint64_t>(
                            le16(entry + 8U)) << 32U
                        | le32(entry + 4U);
                    QByteArray child;
                    if (!readBlock(childBlock, child, errorText)
                        || !collectExtentNode(
                            child,
                            recursionDepth + 1U,
                            runs,
                            truncated,
                            errorText))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        static void appendRun(
            const ExtLogicalRun& run,
            std::vector<ExtLogicalRun>& runs)
        {
            if (!runs.empty())
            {
                ExtLogicalRun& previous = runs.back();
                const bool logicalContiguous =
                    previous.logicalBlock + previous.blockCount
                    == run.logicalBlock;
                const bool physicalContiguous =
                    previous.sparse
                    || previous.physicalBlock + previous.blockCount
                        == run.physicalBlock;
                if (logicalContiguous && physicalContiguous
                    && previous.sparse == run.sparse
                    && previous.unwritten == run.unwritten)
                {
                    previous.blockCount += run.blockCount;
                    return;
                }
            }
            runs.push_back(run);
        }

        bool collectExtents(
            const ExtInode& inode,
            std::vector<ks::misc::RawFileExtent>& extents,
            bool& truncated,
            QString& errorText)
        {
            extents.clear();
            truncated = false;
            const std::uint64_t logicalBlockCount =
                (inode.sizeBytes + m_blockSize - 1U) / m_blockSize;
            if (logicalBlockCount == 0U)
            {
                return true;
            }

            std::vector<ExtLogicalRun> mappedRuns;
            if ((inode.flags & kExtentsFlag) != 0U)
            {
                const QByteArray root(
                    reinterpret_cast<const char*>(
                        inode.blockData.data()),
                    static_cast<qsizetype>(inode.blockData.size()));
                if (!collectExtentNode(
                        root,
                        0U,
                        mappedRuns,
                        truncated,
                        errorText))
                {
                    return false;
                }
                std::sort(
                    mappedRuns.begin(),
                    mappedRuns.end(),
                    [](const ExtLogicalRun& left,
                       const ExtLogicalRun& right)
                    {
                        return left.logicalBlock < right.logicalBlock;
                    });
            }
            else
            {
                const std::uint64_t boundedBlocks =
                    std::min<std::uint64_t>(
                        logicalBlockCount,
                        kMaximumLogicalBlocks);
                for (std::uint64_t logical = 0;
                     logical < boundedBlocks;
                     ++logical)
                {
                    ExtBlockMapping mapping;
                    if (!mapIndirectBlock(
                            inode,
                            logical,
                            mapping,
                            errorText))
                    {
                        return false;
                    }
                    ExtLogicalRun run;
                    run.logicalBlock = logical;
                    run.blockCount = 1U;
                    run.physicalBlock = mapping.physicalBlock;
                    run.sparse = !mapping.mapped;
                    appendRun(run, mappedRuns);
                    if (mappedRuns.size()
                        >= ks::misc::rawfs::kMaximumExtentCount)
                    {
                        truncated = true;
                        break;
                    }
                }
                truncated = truncated
                    || logicalBlockCount > boundedBlocks;
            }

            std::uint64_t cursor = 0;
            for (const ExtLogicalRun& mapped : mappedRuns)
            {
                if (mapped.logicalBlock >= logicalBlockCount)
                {
                    break;
                }
                if (mapped.logicalBlock > cursor)
                {
                    ExtLogicalRun hole;
                    hole.logicalBlock = cursor;
                    hole.blockCount = mapped.logicalBlock - cursor;
                    hole.sparse = true;
                    appendRawExtent(
                        hole,
                        inode.sizeBytes,
                        extents);
                }
                appendRawExtent(
                    mapped,
                    inode.sizeBytes,
                    extents);
                cursor = std::max(
                    cursor,
                    mapped.logicalBlock + mapped.blockCount);
                if (extents.size()
                    >= ks::misc::rawfs::kMaximumExtentCount)
                {
                    truncated = true;
                    break;
                }
            }
            if (!truncated && cursor < logicalBlockCount)
            {
                ExtLogicalRun hole;
                hole.logicalBlock = cursor;
                hole.blockCount = logicalBlockCount - cursor;
                hole.sparse = true;
                appendRawExtent(hole, inode.sizeBytes, extents);
            }
            return true;
        }

        void appendRawExtent(
            const ExtLogicalRun& run,
            const std::uint64_t fileSize,
            std::vector<ks::misc::RawFileExtent>& extents) const
        {
            const std::uint64_t logicalOffset =
                run.logicalBlock * m_blockSize;
            if (logicalOffset >= fileSize)
            {
                return;
            }
            ks::misc::RawFileExtent extent;
            extent.logicalOffset = logicalOffset;
            extent.lengthBytes = std::min<std::uint64_t>(
                run.blockCount * m_blockSize,
                fileSize - logicalOffset);
            extent.sparse = run.sparse;
            extent.unwritten = run.unwritten;
            extent.physicalMappingExact = !run.sparse;
            if (!run.sparse)
            {
                extent.absoluteOffset =
                    m_reader.partitionOffset()
                    + run.physicalBlock * m_blockSize;
            }
            extents.push_back(extent);
        }

        static ks::misc::RawFileObjectType objectType(
            const std::uint16_t mode)
        {
            switch (mode & kFileTypeMask)
            {
            case kRegularFileMode:
                return ks::misc::RawFileObjectType::RegularFile;
            case kDirectoryMode:
                return ks::misc::RawFileObjectType::Directory;
            case kSymbolicLinkMode:
                return ks::misc::RawFileObjectType::SymbolicLink;
            default:
                return ks::misc::RawFileObjectType::Special;
            }
        }

        static void fillEntry(
            const ExtInode& inode,
            const std::uint64_t parent,
            const QString& name,
            const QString& fullPath,
            ks::misc::RawFileEntry& entry)
        {
            entry.name = name;
            entry.fullPath = fullPath;
            entry.type = objectType(inode.mode);
            entry.objectId = inode.number;
            entry.parentObjectId = parent;
            entry.fileSizeBytes = inode.sizeBytes;
            entry.allocatedSizeBytes = inode.allocatedBytes;
            entry.metadataOffset = inode.metadataOffset;
            entry.modeOrFlags =
                static_cast<std::uint32_t>(inode.mode)
                | inode.flags;
        }

        const ks::misc::rawfs::VolumeReader& m_reader;
        std::uint32_t m_blockSize = 0;
        std::uint32_t m_firstDataBlock = 0;
        std::uint32_t m_blocksPerGroup = 0;
        std::uint32_t m_inodesPerGroup = 0;
        std::uint32_t m_inodeCount = 0;
        std::uint32_t m_incompatFeatures = 0;
        std::uint16_t m_inodeSize = 0;
        std::uint16_t m_descriptorSize = 0;
        std::uint64_t m_blockCount = 0;
        std::uint64_t m_groupCount = 0;
    };
}

namespace ks::misc::rawfs
{
    RawDirectoryResult listExt(
        const VolumeReader& reader,
        const ForensicFileSystemKind expectedKind,
        const QString& path,
        const std::uint32_t maximumEntries)
    {
        RawDirectoryResult result;
        result.fileSystem = expectedKind;
        result.fileSystemName =
            DiskFileSystemForensics::fileSystemName(expectedKind);
        result.requestedPath = path;

        ExtVolume volume(reader);
        if (!volume.mount(result.errorText))
        {
            return result;
        }
        ExtInode directory;
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

    RawFileReadResult readExt(
        const VolumeReader& reader,
        const ForensicFileSystemKind expectedKind,
        const QString& path,
        const std::uint64_t offset,
        const std::uint32_t length)
    {
        struct FileState
        {
            ExtVolume volume;
            ExtInode data;
            RawFileReadResult basis;
            explicit FileState(const VolumeReader& reader) : volume(reader) {}
        };
        auto state = reader.fileSessionState<FileState>();
        if (!state)
        {
            state = std::make_shared<FileState>(reader);
            auto& volume = state->volume;
            auto& inode = state->data;

            RawFileReadResult result;
            result.fileSystem = expectedKind;
            result.fileSystemName =
                DiskFileSystemForensics::fileSystemName(expectedKind);
            result.filePath = path;

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
            state->basis = result;
            reader.setFileSessionState(state);
        }
        RawFileReadResult result = state->basis;
        result.requestedOffset = offset;
        auto& volume = state->volume;
        auto& inode = state->data;
        volume.readFile(inode, offset, length, result);
        return result;
    }
}
