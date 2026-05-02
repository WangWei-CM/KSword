#include "DiskEditorBackend.h"

// ============================================================
// DiskEditorBackend.cpp
// 作用：
// 1) 使用 Win32 API 读取物理磁盘几何、长度、描述符和分区表；
// 2) 提供按偏移读写字节的统一入口；
// 3) UI 层只消费 DiskDeviceInfo，不直接处理底层 Windows 结构。
// ============================================================

#include <QColor>
#include <QStringList>
#include <QVariantMap>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <string>

namespace
{
    using ks::misc::DiskDeviceInfo;
    using ks::misc::DiskPartitionInfo;
    using ks::misc::DiskPartitionKind;
    using ks::misc::DiskPartitionStyle;

    // 常用 GPT 分区类型 GUID：
    // - 某些 SDK 头文件不稳定暴露这些常量；
    // - 本地定义 const GUID 只用于比较，不污染全局符号。
    constexpr GUID kGptBasicDataGuid =
        { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } };
    constexpr GUID kGptEfiSystemGuid =
        { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
    constexpr GUID kGptMsReservedGuid =
        { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };
    constexpr GUID kGptRecoveryGuid =
        { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } };

    // kMaxPhysicalDriveProbeCount：
    // - 枚举 PhysicalDrive 的探测上限；
    // - Windows 桌面环境通常远小于该值。
    constexpr int kMaxPhysicalDriveProbeCount = 64;

    // toWide：
    // - QString 转 Win32 宽字符串；
    // - text 为 Qt 字符串；
    // - 返回 std::wstring。
    std::wstring toWide(const QString& text)
    {
        return std::wstring(reinterpret_cast<const wchar_t*>(text.utf16()));
    }

    // lastWin32ErrorText：
    // - 格式化最近 Win32 错误码；
    // - prefix 为操作说明；
    // - 返回中文诊断文本。
    QString lastWin32ErrorText(const QString& prefix)
    {
        return QStringLiteral("%1失败，Win32错误码=%2").arg(prefix).arg(::GetLastError());
    }

    // guidToText：
    // - 把 GUID 转为标准字符串；
    // - guidValue 为输入 GUID；
    // - 返回 {xxxxxxxx-...} 文本；
    // - 使用纯格式化实现，避免为 StringFromGUID2 额外引入 ole32 链接依赖。
    QString guidToText(const GUID& guidValue)
    {
        return QStringLiteral("{%1-%2-%3-%4%5-%6%7%8%9%10%11}")
            .arg(static_cast<qulonglong>(guidValue.Data1), 8, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data2), 4, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data3), 4, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[0]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[1]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[2]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[3]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[4]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[5]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[6]), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(guidValue.Data4[7]), 2, 16, QChar('0'))
            .toUpper();
    }

    // trimStorageString：
    // - 清理 STORAGE_DEVICE_DESCRIPTOR 中的 ASCII 字段；
    // - text 为原始字符串；
    // - 返回去掉控制字符和多余空格后的文本。
    QString trimStorageString(const QString& text)
    {
        QString result = text;
        result.replace(QChar('\0'), QChar(' '));
        result = result.simplified();
        return result == QStringLiteral(".") ? QString() : result;
    }

    // storageBusTypeText：
    // - 把 STORAGE_BUS_TYPE 转为友好总线名称；
    // - busType 为 Win32 总线枚举值；
    // - 返回 SATA/NVMe/USB 等文本。
    QString storageBusTypeText(const STORAGE_BUS_TYPE busType)
    {
        const int rawBusType = static_cast<int>(busType);
        switch (rawBusType)
        {
        case 0x01: return QStringLiteral("SCSI");
        case 0x02: return QStringLiteral("ATAPI");
        case 0x03: return QStringLiteral("ATA");
        case 0x04: return QStringLiteral("IEEE1394");
        case 0x05: return QStringLiteral("SSA");
        case 0x06: return QStringLiteral("Fibre");
        case 0x07: return QStringLiteral("USB");
        case 0x08: return QStringLiteral("RAID");
        case 0x09: return QStringLiteral("iSCSI");
        case 0x0A: return QStringLiteral("SAS");
        case 0x0B: return QStringLiteral("SATA");
        case 0x0C: return QStringLiteral("SD");
        case 0x0D: return QStringLiteral("MMC");
        case 0x0E: return QStringLiteral("Virtual");
        case 0x0F: return QStringLiteral("FileBackedVirtual");
        case 0x10: return QStringLiteral("Storage Spaces");
        case 0x11: return QStringLiteral("NVMe");
        case 0x12: return QStringLiteral("SCM");
        case 0x13: return QStringLiteral("UFS");
        default: break;
        }
        return QStringLiteral("未知");
    }

    // diskStyleFromWin32：
    // - 把 PARTITION_STYLE 转为项目枚举；
    // - style 为 Win32 样式；
    // - 返回 DiskPartitionStyle。
    DiskPartitionStyle diskStyleFromWin32(const int style)
    {
        switch (static_cast<int>(style))
        {
        case 0: return DiskPartitionStyle::Mbr;
        case 1: return DiskPartitionStyle::Gpt;
        case 2: return DiskPartitionStyle::Raw;
        default: break;
        }
        return DiskPartitionStyle::Unknown;
    }

    // isGuidEqual：
    // - 比较两个 GUID 是否一致；
    // - left/right 为输入 GUID；
    // - 返回 true 表示完全一致。
    bool isGuidEqual(const GUID& left, const GUID& right)
    {
        return ::IsEqualGUID(left, right) != FALSE;
    }

    // partitionKindFromGptType：
    // - 根据 GPT 类型 GUID 分类；
    // - typeGuid 为 GPT PartitionType；
    // - 返回用于 UI 着色的分类。
    DiskPartitionKind partitionKindFromGptType(const GUID& typeGuid)
    {
        if (isGuidEqual(typeGuid, kGptEfiSystemGuid))
        {
            return DiskPartitionKind::System;
        }
        if (isGuidEqual(typeGuid, kGptMsReservedGuid))
        {
            return DiskPartitionKind::Reserved;
        }
        if (isGuidEqual(typeGuid, kGptRecoveryGuid))
        {
            return DiskPartitionKind::Recovery;
        }
        if (isGuidEqual(typeGuid, kGptBasicDataGuid))
        {
            return DiskPartitionKind::BasicData;
        }
        return DiskPartitionKind::Unknown;
    }

    // partitionTypeTextFromGpt：
    // - GPT 类型 GUID 转中文说明；
    // - typeGuid 为 GPT PartitionType；
    // - 返回可展示文本。
    QString partitionTypeTextFromGpt(const GUID& typeGuid)
    {
        if (isGuidEqual(typeGuid, kGptBasicDataGuid))
        {
            return QStringLiteral("GPT 基本数据");
        }
        if (isGuidEqual(typeGuid, kGptEfiSystemGuid))
        {
            return QStringLiteral("EFI 系统分区");
        }
        if (isGuidEqual(typeGuid, kGptMsReservedGuid))
        {
            return QStringLiteral("Microsoft 保留分区");
        }
        if (isGuidEqual(typeGuid, kGptRecoveryGuid))
        {
            return QStringLiteral("Windows 恢复分区");
        }
        return QStringLiteral("GPT %1").arg(guidToText(typeGuid));
    }

    // partitionKindFromMbrType：
    // - 根据 MBR 类型字节分类；
    // - typeByte 为 MBR PartitionType；
    // - 返回用于 UI 着色的分类。
    DiskPartitionKind partitionKindFromMbrType(const BYTE typeByte)
    {
        switch (typeByte)
        {
        case 0x01:
        case 0x04:
        case 0x06:
        case 0x07:
        case 0x0B:
        case 0x0C:
        case 0x0E:
        case 0x0F:
            return DiskPartitionKind::BasicData;
        case 0xEF:
            return DiskPartitionKind::System;
        case 0x27:
            return DiskPartitionKind::Recovery;
        case 0x42:
            return DiskPartitionKind::Reserved;
        case 0x82:
        case 0x83:
        case 0x8E:
            return DiskPartitionKind::Linux;
        default:
            break;
        }
        return DiskPartitionKind::Unknown;
    }

    // partitionTypeTextFromMbr：
    // - MBR 类型字节转文本；
    // - typeByte 为 MBR PartitionType；
    // - 返回可展示文本。
    QString partitionTypeTextFromMbr(const BYTE typeByte)
    {
        switch (typeByte)
        {
        case 0x01: return QStringLiteral("MBR FAT12 (0x01)");
        case 0x04: return QStringLiteral("MBR FAT16 (0x04)");
        case 0x06: return QStringLiteral("MBR FAT16 扩展 (0x06)");
        case 0x07: return QStringLiteral("MBR NTFS/exFAT/HPFS (0x07)");
        case 0x0B: return QStringLiteral("MBR FAT32 (0x0B)");
        case 0x0C: return QStringLiteral("MBR FAT32 LBA (0x0C)");
        case 0x0E: return QStringLiteral("MBR FAT16 LBA (0x0E)");
        case 0x0F: return QStringLiteral("MBR 扩展 LBA (0x0F)");
        case 0xEF: return QStringLiteral("MBR EFI 系统 (0xEF)");
        case 0x27: return QStringLiteral("MBR 恢复/OEM (0x27)");
        case 0x82: return QStringLiteral("MBR Linux Swap (0x82)");
        case 0x83: return QStringLiteral("MBR Linux 文件系统 (0x83)");
        case 0x8E: return QStringLiteral("MBR Linux LVM (0x8E)");
        default: break;
        }
        return QStringLiteral("MBR 类型 0x%1").arg(static_cast<unsigned int>(typeByte), 2, 16, QChar('0')).toUpper();
    }

    // partitionColor：
    // - 根据分区类型返回柱形图颜色；
    // - kind 为语义分类；
    // - 返回 QColor。
    QColor partitionColor(const DiskPartitionKind kind)
    {
        switch (kind)
        {
        case DiskPartitionKind::BasicData: return QColor(67, 160, 255);
        case DiskPartitionKind::System: return QColor(34, 197, 94);
        case DiskPartitionKind::Reserved: return QColor(148, 163, 184);
        case DiskPartitionKind::Recovery: return QColor(245, 158, 11);
        case DiskPartitionKind::Linux: return QColor(168, 85, 247);
        case DiskPartitionKind::Unallocated: return QColor(107, 114, 128);
        default: break;
        }
        return QColor(20, 184, 166);
    }

    // openDiskHandle：
    // - 打开物理磁盘；
    // - writeAccess 控制是否请求写权限；
    // - errorTextOut 返回失败诊断；
    // - 返回 Win32 HANDLE，失败为 INVALID_HANDLE_VALUE。
    HANDLE openDiskHandle(
        const QString& devicePath,
        const bool writeAccess,
        QString& errorTextOut)
    {
        const DWORD desiredAccess = writeAccess
            ? (GENERIC_READ | GENERIC_WRITE)
            : GENERIC_READ;
        const std::wstring devicePathWide = toWide(devicePath);
        HANDLE handleValue = ::CreateFileW(
            devicePathWide.c_str(),
            desiredAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handleValue == INVALID_HANDLE_VALUE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("打开 %1").arg(devicePath));
        }
        return handleValue;
    }

    // readStorageDescriptor：
    // - 读取 STORAGE_DEVICE_DESCRIPTOR 并填充型号、厂商、序列号和总线；
    // - handleValue 为已打开磁盘句柄；
    // - diskInfo 为待填充结构；
    // - 无返回值，失败时保持字段为空。
    void readStorageDescriptor(const HANDLE handleValue, DiskDeviceInfo& diskInfo)
    {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        STORAGE_DESCRIPTOR_HEADER header{};
        DWORD returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            &header,
            static_cast<DWORD>(sizeof(header)),
            &returnedBytes,
            nullptr) == FALSE
            || header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        {
            return;
        }

        std::vector<std::uint8_t> buffer(header.Size + 8ULL);
        returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &returnedBytes,
            nullptr) == FALSE)
        {
            return;
        }

        const STORAGE_DEVICE_DESCRIPTOR* descriptor =
            reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
        const char* basePtr = reinterpret_cast<const char*>(buffer.data());
        const auto readOffsetString = [&](const DWORD offsetValue) -> QString
        {
            if (offsetValue == 0 || offsetValue >= returnedBytes)
            {
                return QString();
            }

            const char* textStart = basePtr + offsetValue;
            const DWORD remainingBytes = returnedBytes - offsetValue;
            DWORD textLength = 0;
            while (textLength < remainingBytes && textStart[textLength] != '\0')
            {
                ++textLength;
            }
            return trimStorageString(QString::fromLatin1(textStart, static_cast<int>(textLength)));
        };

        diskInfo.vendor = readOffsetString(descriptor->VendorIdOffset);
        diskInfo.model = readOffsetString(descriptor->ProductIdOffset);
        diskInfo.serial = readOffsetString(descriptor->SerialNumberOffset);
        diskInfo.busType = storageBusTypeText(descriptor->BusType);
        diskInfo.removable = descriptor->RemovableMedia != FALSE;
    }

    // readDiskGeometry：
    // - 读取磁盘几何和扇区大小；
    // - handleValue 为磁盘句柄；
    // - diskInfo 为待填充结构；
    // - 无返回值。
    void readDiskGeometry(const HANDLE handleValue, DiskDeviceInfo& diskInfo)
    {
        DISK_GEOMETRY_EX geometry{};
        DWORD returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr,
            0,
            &geometry,
            static_cast<DWORD>(sizeof(geometry)),
            &returnedBytes,
            nullptr) != FALSE)
        {
            diskInfo.sizeBytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
            diskInfo.bytesPerSector = geometry.Geometry.BytesPerSector == 0
                ? 512U
                : geometry.Geometry.BytesPerSector;
            switch (geometry.Geometry.MediaType)
            {
            case FixedMedia: diskInfo.mediaType = QStringLiteral("固定磁盘"); break;
            case RemovableMedia: diskInfo.mediaType = QStringLiteral("可移动磁盘"); break;
            default: diskInfo.mediaType = QStringLiteral("介质类型 %1").arg(static_cast<int>(geometry.Geometry.MediaType)); break;
            }
        }

        // 物理扇区大小：
        // - 不同 SDK 对 STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR 暴露条件不完全一致；
        // - 当前页面写入保护以逻辑扇区为准，物理扇区后续可在专用兼容层中补充。
        diskInfo.physicalBytesPerSector = diskInfo.bytesPerSector;
    }

    // appendUnallocatedRanges：
    // - 根据分区占用区间补齐未分配区域；
    // - diskInfo 为当前磁盘信息；
    // - 无返回值，直接追加到 partitions。
    void appendUnallocatedRanges(DiskDeviceInfo& diskInfo)
    {
        if (diskInfo.sizeBytes == 0)
        {
            return;
        }

        std::sort(
            diskInfo.partitions.begin(),
            diskInfo.partitions.end(),
            [](const DiskPartitionInfo& left, const DiskPartitionInfo& right)
            {
                return left.offsetBytes < right.offsetBytes;
            });

        std::vector<DiskPartitionInfo> withGaps;
        std::uint64_t cursor = 0;
        int generatedIndex = 0;
        for (DiskPartitionInfo partition : diskInfo.partitions)
        {
            if (partition.offsetBytes > cursor)
            {
                DiskPartitionInfo gap;
                gap.tableIndex = generatedIndex++;
                gap.partitionNumber = 0;
                gap.style = diskInfo.partitionStyle;
                gap.kind = DiskPartitionKind::Unallocated;
                gap.name = QStringLiteral("未分配");
                gap.typeText = QStringLiteral("未分配空间");
                gap.offsetBytes = cursor;
                gap.lengthBytes = partition.offsetBytes - cursor;
                gap.color = partitionColor(gap.kind);
                withGaps.push_back(gap);
            }

            partition.tableIndex = generatedIndex++;
            partition.color = partitionColor(partition.kind);
            withGaps.push_back(partition);
            const std::uint64_t endOffset = partition.offsetBytes + partition.lengthBytes;
            cursor = std::max(cursor, endOffset);
        }

        if (cursor < diskInfo.sizeBytes)
        {
            DiskPartitionInfo gap;
            gap.tableIndex = generatedIndex++;
            gap.partitionNumber = 0;
            gap.style = diskInfo.partitionStyle;
            gap.kind = DiskPartitionKind::Unallocated;
            gap.name = QStringLiteral("未分配");
            gap.typeText = QStringLiteral("未分配空间");
            gap.offsetBytes = cursor;
            gap.lengthBytes = diskInfo.sizeBytes - cursor;
            gap.color = partitionColor(gap.kind);
            withGaps.push_back(gap);
        }

        diskInfo.partitions = std::move(withGaps);
    }

    // readDriveLayout：
    // - 读取 DRIVE_LAYOUT_INFORMATION_EX 并转换为 DiskPartitionInfo；
    // - handleValue 为磁盘句柄；
    // - diskInfo 为待填充结构；
    // - 无返回值，失败时保留已有基本信息。
    void readDriveLayout(const HANDLE handleValue, DiskDeviceInfo& diskInfo)
    {
        DWORD bufferBytes = 64U * 1024U;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            std::vector<std::uint8_t> buffer(bufferBytes);
            DWORD returnedBytes = 0;
            const BOOL layoutOk = ::DeviceIoControl(
                handleValue,
                IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                nullptr,
                0,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &returnedBytes,
                nullptr);
            if (layoutOk == FALSE)
            {
                const DWORD errorCode = ::GetLastError();
                if (errorCode == ERROR_INSUFFICIENT_BUFFER || errorCode == ERROR_MORE_DATA)
                {
                    bufferBytes *= 2U;
                    continue;
                }
                return;
            }

            if (returnedBytes < sizeof(DRIVE_LAYOUT_INFORMATION_EX))
            {
                return;
            }

            const DRIVE_LAYOUT_INFORMATION_EX* layout =
                reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
            diskInfo.partitionStyle = diskStyleFromWin32(static_cast<int>(layout->PartitionStyle));
            diskInfo.partitions.clear();
            diskInfo.partitions.reserve(layout->PartitionCount);

            for (DWORD index = 0; index < layout->PartitionCount; ++index)
            {
                const PARTITION_INFORMATION_EX& source = layout->PartitionEntry[index];
                if (source.PartitionLength.QuadPart <= 0)
                {
                    continue;
                }

                DiskPartitionInfo partition;
                partition.tableIndex = static_cast<int>(diskInfo.partitions.size());
                partition.partitionNumber = static_cast<int>(source.PartitionNumber);
                partition.style = diskStyleFromWin32(static_cast<int>(source.PartitionStyle));
                partition.offsetBytes = static_cast<std::uint64_t>(source.StartingOffset.QuadPart);
                partition.lengthBytes = static_cast<std::uint64_t>(source.PartitionLength.QuadPart);
                partition.name = QStringLiteral("分区 %1").arg(partition.partitionNumber);

                if (static_cast<int>(source.PartitionStyle) == 0)
                {
                    partition.kind = partitionKindFromMbrType(source.Mbr.PartitionType);
                    partition.typeText = partitionTypeTextFromMbr(source.Mbr.PartitionType);
                    partition.bootIndicator = source.Mbr.BootIndicator != FALSE;
                    partition.recognized = source.Mbr.RecognizedPartition != FALSE;
                    QStringList flags;
                    if (partition.bootIndicator)
                    {
                        flags << QStringLiteral("活动");
                    }
                    if (source.Mbr.HiddenSectors != 0)
                    {
                        flags << QStringLiteral("隐藏扇区=%1").arg(source.Mbr.HiddenSectors);
                    }
                    if (partition.recognized)
                    {
                        flags << QStringLiteral("已识别");
                    }
                    partition.flagsText = flags.join(QStringLiteral(", "));
                }
                else if (static_cast<int>(source.PartitionStyle) == 1)
                {
                    partition.kind = partitionKindFromGptType(source.Gpt.PartitionType);
                    partition.typeText = partitionTypeTextFromGpt(source.Gpt.PartitionType);
                    partition.uniqueIdText = guidToText(source.Gpt.PartitionId);
                    const QString gptName = QString::fromWCharArray(source.Gpt.Name, 36).trimmed();
                    if (!gptName.isEmpty())
                    {
                        partition.name = gptName;
                    }
                    QStringList flags;
                    if (source.Gpt.Attributes != 0)
                    {
                        flags << QStringLiteral("属性=0x%1")
                            .arg(static_cast<qulonglong>(source.Gpt.Attributes), 16, 16, QChar('0'));
                    }
                    partition.flagsText = flags.join(QStringLiteral(", "));
                    partition.recognized = true;
                }

                partition.color = partitionColor(partition.kind);
                diskInfo.partitions.push_back(std::move(partition));
            }

            appendUnallocatedRanges(diskInfo);
            return;
        }
    }

    // buildDisplayName：
    // - 组合磁盘友好名称；
    // - diskInfo 为磁盘信息；
    // - 返回 UI 下拉框文本。
    QString buildDisplayName(const DiskDeviceInfo& diskInfo)
    {
        const QString modelText = diskInfo.model.isEmpty()
            ? QStringLiteral("未知型号")
            : diskInfo.model;
        const QString sizeText = ks::misc::DiskEditorBackend::formatBytes(diskInfo.sizeBytes);
        return QStringLiteral("PhysicalDrive%1  %2  %3")
            .arg(diskInfo.diskIndex)
            .arg(modelText)
            .arg(sizeText);
    }

    // openVolumeHandle：
    // - 打开 \\?\Volume{...} 路径对应的卷句柄；
    // - volumeName 来自 FindFirstVolumeW；
    // - errorTextOut 返回失败诊断；
    // - 返回 Win32 HANDLE。
    HANDLE openVolumeHandle(const QString& volumeName, QString& errorTextOut)
    {
        QString path = volumeName;
        if (path.endsWith(QChar('\\')))
        {
            path.chop(1);
        }
        HANDLE handleValue = ::CreateFileW(
            toWide(path).c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handleValue == INVALID_HANDLE_VALUE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("打开卷 %1").arg(volumeName));
        }
        return handleValue;
    }

    // collectMountPoints：
    // - 查询卷挂载点和盘符；
    // - volumeName 为 \\?\Volume{GUID}\；
    // - 返回拼接后的挂载点文本。
    QString collectMountPoints(const QString& volumeName)
    {
        DWORD requiredChars = 0;
        ::GetVolumePathNamesForVolumeNameW(
            toWide(volumeName).c_str(),
            nullptr,
            0,
            &requiredChars);
        if (requiredChars == 0)
        {
            return QString();
        }

        std::vector<wchar_t> buffer(requiredChars + 2U);
        if (::GetVolumePathNamesForVolumeNameW(
            toWide(volumeName).c_str(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &requiredChars) == FALSE)
        {
            return QString();
        }

        QStringList paths;
        const wchar_t* cursor = buffer.data();
        while (*cursor != L'\0')
        {
            const QString path = QString::fromWCharArray(cursor);
            if (!path.isEmpty())
            {
                paths << path;
            }
            cursor += path.size() + 1;
        }
        return paths.join(QStringLiteral(", "));
    }

    // collectVolumeInformation：
    // - 读取卷标和文件系统名称；
    // - volumeName 为 \\?\Volume{GUID}\；
    // - labelOut/fileSystemOut 接收结果。
    void collectVolumeInformation(const QString& volumeName, QString& labelOut, QString& fileSystemOut)
    {
        std::array<wchar_t, MAX_PATH + 1> labelBuffer{};
        std::array<wchar_t, MAX_PATH + 1> fsBuffer{};
        DWORD serialNumber = 0;
        DWORD maxComponentLength = 0;
        DWORD flags = 0;
        if (::GetVolumeInformationW(
            toWide(volumeName).c_str(),
            labelBuffer.data(),
            static_cast<DWORD>(labelBuffer.size()),
            &serialNumber,
            &maxComponentLength,
            &flags,
            fsBuffer.data(),
            static_cast<DWORD>(fsBuffer.size())) != FALSE)
        {
            labelOut = QString::fromWCharArray(labelBuffer.data());
            fileSystemOut = QString::fromWCharArray(fsBuffer.data());
        }
    }

    // collectVolumeDevicePath：
    // - 使用 QueryDosDevice 查询卷 GUID 对应的 NT 设备路径；
    // - volumeName 为 \\?\Volume{GUID}\；
    // - 返回 \Device\HarddiskVolumeX 文本，失败返回空。
    QString collectVolumeDevicePath(const QString& volumeName)
    {
        QString dosName = volumeName;
        if (dosName.startsWith(QStringLiteral("\\\\?\\")))
        {
            dosName = dosName.mid(4);
        }
        if (dosName.endsWith(QChar('\\')))
        {
            dosName.chop(1);
        }

        std::array<wchar_t, 1024> targetBuffer{};
        const DWORD chars = ::QueryDosDeviceW(
            toWide(dosName).c_str(),
            targetBuffer.data(),
            static_cast<DWORD>(targetBuffer.size()));
        if (chars == 0)
        {
            return QString();
        }
        return QString::fromWCharArray(targetBuffer.data());
    }

    // boolText：
    // - 把布尔值转是/否；
    // - value 为输入布尔；
    // - 返回中文文本。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    // addHealthMap：
    // - 向 QVariantMap 健康列表追加一行；
    // - severity 约定 0=信息、1=警告、2=错误；
    // - 无返回值。
    void addHealthMap(
        std::vector<QVariantMap>& items,
        const QString& category,
        const QString& name,
        const QString& value,
        const QString& detail,
        const int severity = 0)
    {
        QVariantMap item;
        item.insert(QStringLiteral("category"), category);
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("value"), value);
        item.insert(QStringLiteral("detail"), detail);
        item.insert(QStringLiteral("severity"), severity);
        items.push_back(std::move(item));
    }
}

namespace ks::misc
{
    bool DiskEditorBackend::enumerateDisks(
        std::vector<DiskDeviceInfo>& disksOut,
        QString& errorTextOut)
    {
        disksOut.clear();
        errorTextOut.clear();
        int missingStreak = 0;

        for (int diskIndex = 0; diskIndex < kMaxPhysicalDriveProbeCount; ++diskIndex)
        {
            DiskDeviceInfo diskInfo;
            diskInfo.diskIndex = diskIndex;
            diskInfo.devicePath = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskIndex);

            QString openErrorText;
            HANDLE handleValue = openDiskHandle(diskInfo.devicePath, false, openErrorText);
            if (handleValue == INVALID_HANDLE_VALUE)
            {
                const DWORD errorCode = ::GetLastError();
                if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND)
                {
                    ++missingStreak;
                    if (missingStreak >= 8 && !disksOut.empty())
                    {
                        break;
                    }
                    continue;
                }

                diskInfo.openErrorText = openErrorText;
                diskInfo.displayName = QStringLiteral("PhysicalDrive%1  打开失败").arg(diskIndex);
                disksOut.push_back(std::move(diskInfo));
                missingStreak = 0;
                continue;
            }

            missingStreak = 0;
            diskInfo.canRead = true;
            readStorageDescriptor(handleValue, diskInfo);
            readDiskGeometry(handleValue, diskInfo);
            readDriveLayout(handleValue, diskInfo);
            diskInfo.displayName = buildDisplayName(diskInfo);
            ::CloseHandle(handleValue);

            disksOut.push_back(std::move(diskInfo));
        }

        if (disksOut.empty())
        {
            errorTextOut = QStringLiteral("未枚举到 PhysicalDrive 设备。");
            return false;
        }
        return true;
    }

    bool DiskEditorBackend::readBytes(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const std::uint32_t bytesToRead,
        QByteArray& bytesOut,
        QString& errorTextOut)
    {
        bytesOut.clear();
        errorTextOut.clear();
        if (devicePath.trimmed().isEmpty())
        {
            errorTextOut = QStringLiteral("磁盘设备路径为空。");
            return false;
        }
        if (bytesToRead == 0)
        {
            errorTextOut = QStringLiteral("读取长度为 0。");
            return false;
        }

        QString openErrorText;
        HANDLE handleValue = openDiskHandle(devicePath, false, openErrorText);
        if (handleValue == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        LARGE_INTEGER targetOffset{};
        targetOffset.QuadPart = static_cast<LONGLONG>(offsetBytes);
        if (::SetFilePointerEx(handleValue, targetOffset, nullptr, FILE_BEGIN) == FALSE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("设置读取偏移"));
            ::CloseHandle(handleValue);
            return false;
        }

        QByteArray buffer;
        buffer.resize(static_cast<int>(bytesToRead));
        DWORD readBytesValue = 0;
        if (::ReadFile(
            handleValue,
            buffer.data(),
            bytesToRead,
            &readBytesValue,
            nullptr) == FALSE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("读取磁盘字节"));
            ::CloseHandle(handleValue);
            return false;
        }
        ::CloseHandle(handleValue);

        buffer.resize(static_cast<int>(readBytesValue));
        bytesOut = buffer;
        return true;
    }

    bool DiskEditorBackend::writeBytes(
        const QString& devicePath,
        const std::uint64_t offsetBytes,
        const QByteArray& bytes,
        const std::uint32_t bytesPerSector,
        const bool requireSectorAligned,
        QString& errorTextOut)
    {
        errorTextOut.clear();
        if (devicePath.trimmed().isEmpty())
        {
            errorTextOut = QStringLiteral("磁盘设备路径为空。");
            return false;
        }
        if (bytes.isEmpty())
        {
            errorTextOut = QStringLiteral("写入缓冲区为空。");
            return false;
        }

        const std::uint32_t sectorSize = bytesPerSector == 0 ? 512U : bytesPerSector;
        if (requireSectorAligned)
        {
            if ((offsetBytes % sectorSize) != 0
                || (static_cast<std::uint64_t>(bytes.size()) % sectorSize) != 0)
            {
                errorTextOut = QStringLiteral("写入被拒绝：偏移和长度必须按 %1 字节扇区对齐。").arg(sectorSize);
                return false;
            }
        }

        QString openErrorText;
        HANDLE handleValue = openDiskHandle(devicePath, true, openErrorText);
        if (handleValue == INVALID_HANDLE_VALUE)
        {
            errorTextOut = openErrorText;
            return false;
        }

        LARGE_INTEGER targetOffset{};
        targetOffset.QuadPart = static_cast<LONGLONG>(offsetBytes);
        if (::SetFilePointerEx(handleValue, targetOffset, nullptr, FILE_BEGIN) == FALSE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("设置写入偏移"));
            ::CloseHandle(handleValue);
            return false;
        }

        DWORD writtenBytes = 0;
        const DWORD requestedBytes = static_cast<DWORD>(bytes.size());
        if (::WriteFile(
            handleValue,
            bytes.constData(),
            requestedBytes,
            &writtenBytes,
            nullptr) == FALSE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("写入磁盘字节"));
            ::CloseHandle(handleValue);
            return false;
        }

        ::FlushFileBuffers(handleValue);
        ::CloseHandle(handleValue);

        if (writtenBytes != requestedBytes)
        {
            errorTextOut = QStringLiteral("写入长度不足：期望 %1 字节，实际 %2 字节。")
                .arg(requestedBytes)
                .arg(writtenBytes);
            return false;
        }
        return true;
    }

    std::vector<QVariantMap> DiskEditorBackend::queryVolumeMappings(
        const int diskIndex,
        QString& errorTextOut)
    {
        std::vector<QVariantMap> volumes;
        errorTextOut.clear();

        std::array<wchar_t, MAX_PATH> volumeNameBuffer{};
        HANDLE findHandle = ::FindFirstVolumeW(volumeNameBuffer.data(), static_cast<DWORD>(volumeNameBuffer.size()));
        if (findHandle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("枚举卷"));
            return volumes;
        }

        for (;;)
        {
            const QString volumeName = QString::fromWCharArray(volumeNameBuffer.data());
            QString openError;
            HANDLE volumeHandle = openVolumeHandle(volumeName, openError);
            if (volumeHandle != INVALID_HANDLE_VALUE)
            {
                DWORD returnedBytes = 0;
                constexpr DWORD kExtentBufferBytes = sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 31U;
                std::array<unsigned char, kExtentBufferBytes> extentBuffer{};
                if (::DeviceIoControl(
                    volumeHandle,
                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    nullptr,
                    0,
                    extentBuffer.data(),
                    static_cast<DWORD>(extentBuffer.size()),
                    &returnedBytes,
                    nullptr) != FALSE
                    && returnedBytes >= sizeof(VOLUME_DISK_EXTENTS))
                {
                    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extentBuffer.data());
                    QString label;
                    QString fileSystem;
                    collectVolumeInformation(volumeName, label, fileSystem);
                    const QString mountPoints = collectMountPoints(volumeName);
                    const QString devicePath = collectVolumeDevicePath(volumeName);
                    for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index)
                    {
                        const DISK_EXTENT& extent = extents->Extents[index];
                        if (static_cast<int>(extent.DiskNumber) != diskIndex)
                        {
                            continue;
                        }

                        QVariantMap volume;
                        volume.insert(QStringLiteral("volumeName"), volumeName);
                        volume.insert(QStringLiteral("mountPoints"), mountPoints);
                        volume.insert(QStringLiteral("devicePath"), devicePath);
                        volume.insert(QStringLiteral("fileSystem"), fileSystem);
                        volume.insert(QStringLiteral("label"), label);
                        volume.insert(QStringLiteral("diskNumber"), static_cast<int>(extent.DiskNumber));
                        volume.insert(QStringLiteral("offsetBytes"), static_cast<qulonglong>(extent.StartingOffset.QuadPart));
                        volume.insert(QStringLiteral("lengthBytes"), static_cast<qulonglong>(extent.ExtentLength.QuadPart));
                        volumes.push_back(std::move(volume));
                    }
                }
                ::CloseHandle(volumeHandle);
            }

            volumeNameBuffer.fill(L'\0');
            if (::FindNextVolumeW(findHandle, volumeNameBuffer.data(), static_cast<DWORD>(volumeNameBuffer.size())) == FALSE)
            {
                const DWORD errorCode = ::GetLastError();
                if (errorCode != ERROR_NO_MORE_FILES)
                {
                    errorTextOut = QStringLiteral("继续枚举卷失败，Win32错误码=%1").arg(errorCode);
                }
                break;
            }
        }

        ::FindVolumeClose(findHandle);
        return volumes;
    }

    std::vector<QVariantMap> DiskEditorBackend::queryHealthItems(
        const QString& devicePath,
        QString& errorTextOut)
    {
        std::vector<QVariantMap> items;
        errorTextOut.clear();

        HANDLE handleValue = ::CreateFileW(
            toWide(devicePath).c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handleValue == INVALID_HANDLE_VALUE)
        {
            errorTextOut = lastWin32ErrorText(QStringLiteral("打开磁盘做健康探测"));
            addHealthMap(items, QStringLiteral("探测"), QStringLiteral("只读句柄"), QStringLiteral("失败"), errorTextOut, 1);
            return items;
        }

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceTrimProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_TRIM_DESCRIPTOR trimDescriptor{};
        DWORD returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            &trimDescriptor,
            static_cast<DWORD>(sizeof(trimDescriptor)),
            &returnedBytes,
            nullptr) != FALSE)
        {
            addHealthMap(items, QStringLiteral("能力"), QStringLiteral("TRIM/UNMAP"), boolText(trimDescriptor.TrimEnabled != FALSE), QStringLiteral("StorageDeviceTrimProperty"));
        }
        else
        {
            addHealthMap(items, QStringLiteral("能力"), QStringLiteral("TRIM/UNMAP"), QStringLiteral("未知"), lastWin32ErrorText(QStringLiteral("查询 TRIM")), 1);
        }

        query = STORAGE_PROPERTY_QUERY{};
        query.PropertyId = StorageDeviceWriteCacheProperty;
        query.QueryType = PropertyStandardQuery;
        STORAGE_WRITE_CACHE_PROPERTY writeCache{};
        returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            &writeCache,
            static_cast<DWORD>(sizeof(writeCache)),
            &returnedBytes,
            nullptr) != FALSE)
        {
            addHealthMap(items, QStringLiteral("缓存"), QStringLiteral("写缓存启用"), boolText(writeCache.WriteCacheEnabled != FALSE), QStringLiteral("WriteCacheType=%1").arg(static_cast<int>(writeCache.WriteCacheType)));
            addHealthMap(items, QStringLiteral("缓存"), QStringLiteral("Flush 支持"), boolText(writeCache.FlushCacheSupported != FALSE), QStringLiteral("WriteCacheChangeable=%1").arg(boolText(writeCache.WriteCacheChangeable != FALSE)));
        }
        else
        {
            addHealthMap(items, QStringLiteral("缓存"), QStringLiteral("写缓存状态"), QStringLiteral("未知"), lastWin32ErrorText(QStringLiteral("查询写缓存")), 1);
        }

        STORAGE_HOTPLUG_INFO hotplugInfo{};
        returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            IOCTL_STORAGE_GET_HOTPLUG_INFO,
            nullptr,
            0,
            &hotplugInfo,
            static_cast<DWORD>(sizeof(hotplugInfo)),
            &returnedBytes,
            nullptr) != FALSE)
        {
            addHealthMap(items, QStringLiteral("热插拔"), QStringLiteral("介质可移除"), boolText(hotplugInfo.MediaRemovable != FALSE), QStringLiteral("STORAGE_HOTPLUG_INFO"));
            addHealthMap(items, QStringLiteral("热插拔"), QStringLiteral("设备热插拔"), boolText(hotplugInfo.DeviceHotplug != FALSE), QStringLiteral("WriteCacheEnableOverride=%1").arg(boolText(hotplugInfo.WriteCacheEnableOverride != FALSE)));
        }
        else
        {
            addHealthMap(items, QStringLiteral("热插拔"), QStringLiteral("热插拔信息"), QStringLiteral("未知"), lastWin32ErrorText(QStringLiteral("查询热插拔")), 1);
        }

        GETVERSIONINPARAMS smartVersion{};
        returnedBytes = 0;
        if (::DeviceIoControl(
            handleValue,
            SMART_GET_VERSION,
            nullptr,
            0,
            &smartVersion,
            static_cast<DWORD>(sizeof(smartVersion)),
            &returnedBytes,
            nullptr) != FALSE)
        {
            addHealthMap(
                items,
                QStringLiteral("SMART"),
                QStringLiteral("SMART 接口"),
                (smartVersion.fCapabilities & CAP_SMART_CMD) ? QStringLiteral("可用") : QStringLiteral("驱动不声明 SMART"),
                QStringLiteral("capabilities=0x%1").arg(static_cast<unsigned int>(smartVersion.fCapabilities), 8, 16, QChar('0')).toUpper());
        }
        else
        {
            addHealthMap(items, QStringLiteral("SMART"), QStringLiteral("SMART 接口"), QStringLiteral("不可用/需权限"), lastWin32ErrorText(QStringLiteral("查询 SMART 版本")), 1);
        }

        ::CloseHandle(handleValue);
        return items;
    }

    QString DiskEditorBackend::formatBytes(const std::uint64_t bytes)
    {
        const double value = static_cast<double>(bytes);
        if (bytes >= 1024ULL * 1024ULL * 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 TB").arg(value / 1099511627776.0, 0, 'f', 2);
        }
        if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 GB").arg(value / 1073741824.0, 0, 'f', 2);
        }
        if (bytes >= 1024ULL * 1024ULL)
        {
            return QStringLiteral("%1 MB").arg(value / 1048576.0, 0, 'f', 2);
        }
        if (bytes >= 1024ULL)
        {
            return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 2);
        }
        return QStringLiteral("%1 B").arg(static_cast<qulonglong>(bytes));
    }

    QString DiskEditorBackend::partitionStyleText(const DiskPartitionStyle style)
    {
        switch (style)
        {
        case DiskPartitionStyle::Raw: return QStringLiteral("RAW");
        case DiskPartitionStyle::Mbr: return QStringLiteral("MBR");
        case DiskPartitionStyle::Gpt: return QStringLiteral("GPT");
        default: break;
        }
        return QStringLiteral("未知");
    }
}
