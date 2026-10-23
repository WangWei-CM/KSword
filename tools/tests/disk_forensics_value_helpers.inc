    QString hexOffset(const std::uint64_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    template<typename RowT>
    const RowT* valueAt(const ks::ui::FlatTableModel<RowT>* model, QModelIndex index)
    {
        while (index.isValid() && index.model() != model)
        {
            const auto* proxy = qobject_cast<const QAbstractProxyModel*>(index.model());
            if (proxy == nullptr)
            {
                return nullptr;
            }
            index = proxy->mapToSource(index);
        }
        return index.isValid() && index.model() == model ? model->rowAt(index.row()) : nullptr;
    }

    QVariant rawCell(const ks::misc::RawFileEntry& entry, const int column, const int role)
    {
        using namespace ks::misc;
        std::uint64_t firstPhysical = 0;
        bool firstPhysicalExact = false;
        if (column == 6)
        {
            for (const RawFileExtent& extent : entry.extents)
            {
                if (extent.physicalMappingExact && !extent.sparse)
                {
                    firstPhysical = extent.absoluteOffset;
                    firstPhysicalExact = true;
                    break;
                }
            }
        }
        if (role == Qt::UserRole)
        {
            if (column == 5) return QVariant::fromValue<qulonglong>(entry.metadataOffset);
            if (column == 6) return QVariant::fromValue<qulonglong>(firstPhysical);
        }
        if (role == Qt::UserRole + 1 && column == 6) return firstPhysicalExact;
        if (role != Qt::DisplayRole) return {};
        switch (column)
        {
        case 0: return entry.name;
        case 1: return DiskRawFileSystemBrowser::objectTypeText(entry.type);
        case 2: return QString::number(entry.fileSizeBytes);
        case 3: return QString::number(entry.allocatedSizeBytes);
        case 4: return QString::number(entry.objectId);
        case 5: return hexOffset(entry.metadataOffset);
        case 6: return firstPhysicalExact ? hexOffset(firstPhysical) : QStringLiteral("-");
        case 7:
        {
            QStringList texts;
            for (const RawFileExtent& extent : entry.extents)
            {
                const QString state = extent.sparse ? QStringLiteral("稀疏")
                    : extent.unwritten ? QStringLiteral("未写入")
                    : extent.compressed ? QStringLiteral("压缩") : QString();
                texts.push_back(QStringLiteral("%1:%2+%3%4")
                    .arg(hexOffset(extent.logicalOffset))
                    .arg(extent.physicalMappingExact ? hexOffset(extent.absoluteOffset) : QStringLiteral("-"))
                    .arg(static_cast<qulonglong>(extent.lengthBytes))
                    .arg(state.isEmpty() ? QString() : QStringLiteral("[%1]").arg(state)));
            }
            if (entry.extentsTruncated) texts.push_back(QStringLiteral("…[物理映射未完整展开]"));
            return texts.isEmpty() ? QStringLiteral("-") : texts.join(QStringLiteral("; "));
        }
        default: return {};
        }
    }

    QVariant extentCell(const ks::misc::PhysicalFileExtent& extent, const int column, const int role)
    {
        if (column == 4 && role == Qt::UserRole) return QVariant::fromValue<qulonglong>(extent.physicalOffset);
        if (column == 4 && role == Qt::UserRole + 1) return extent.physicalMappingExact;
        if (role != Qt::DisplayRole) return {};
        switch (column)
        {
        case 0: return extent.diskNumber >= 0 ? QStringLiteral("PhysicalDrive%1").arg(extent.diskNumber) : QStringLiteral("-");
        case 1: return hexOffset(extent.fileOffset);
        case 2: return QString::number(extent.startingVcn);
        case 3: return extent.sparse ? QStringLiteral("Sparse") : QString::number(extent.startingLcn);
        case 4: return extent.physicalMappingExact ? hexOffset(extent.physicalOffset) : QStringLiteral("-");
        case 5: return QString::number(extent.lengthBytes);
        case 6: return extent.sparse ? QStringLiteral("稀疏区间") : extent.physicalMappingExact
            ? QStringLiteral("可跳转") : QStringLiteral("多区间卷，仅返回卷内 LCN");
        default: return {};
        }
    }

    QVariant reverseCell(const ks::misc::ReverseClusterEntry& entry, const int column, const int role)
    {
        if (role != Qt::DisplayRole) return {};
        switch (column)
        {
        case 0: return QString::number(entry.cluster);
        case 1: return QString::number(entry.clusterCount);
        case 2: return entry.streamPath;
        default: return {};
        }
    }

    QVariant deletedCell(const ks::misc::DeletedDirectoryEntry& entry, const int column, const int role)
    {
        if (role != Qt::DisplayRole) return {};
        switch (column)
        {
        case 0: return entry.name;
        case 1: return entry.directoryPath;
        case 2: return QString::number(entry.fileSizeBytes);
        case 3: return QString::number(entry.firstCluster);
        case 4: return hexOffset(entry.directoryEntryOffset);
        case 5:
        {
            QStringList texts;
            for (const ks::misc::DeletedFileExtent& extent : entry.extents)
            {
                texts.push_back(QStringLiteral("%1+%2").arg(hexOffset(extent.physicalOffset))
                    .arg(static_cast<qulonglong>(extent.lengthBytes)));
            }
            return texts.isEmpty() ? QStringLiteral("-") : texts.join(QStringLiteral("; "));
        }
        case 6: return entry.exactExtents && entry.clustersCurrentlyFree && !entry.directory
            ? QStringLiteral("离线磁盘可擦除") : QStringLiteral("只读取证");
        case 7: return entry.evidenceText;
        default: return {};
        }
    }


namespace ks::misc {
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
