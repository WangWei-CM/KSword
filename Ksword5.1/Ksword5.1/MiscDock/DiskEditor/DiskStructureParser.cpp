#include "DiskStructureParser.h"

// ============================================================
// DiskStructureParser.cpp
// 作用：
// 1) 用纯只读方式解析 MBR、GPT 和启动扇区关键字段；
// 2) 将基础后端返回的卷映射与健康信息转换为高级 UI 模型；
// 3) 对结构字段做可跳转、可告警的表格化表达。
// ============================================================

#include "DiskEditorBackend.h"

#include <QVariantMap>
#include <QStringList>

#include <algorithm>
#include <array>
#include <limits>

namespace
{
    using ks::misc::DiskDeviceInfo;
    using ks::misc::DiskHealthItem;
    using ks::misc::DiskPartitionInfo;
    using ks::misc::DiskStructureField;
    using ks::misc::DiskStructureReport;
    using ks::misc::DiskStructureSeverity;
    using ks::misc::DiskVolumeInfo;
    using ks::misc::DiskEditorBackend;

    // kMbrSignatureOffset：MBR 结束签名 0x55AA 的固定偏移。
    constexpr std::uint32_t kMbrSignatureOffset = 510;

    // kMbrPartitionTableOffset：MBR 四个主分区表项的固定偏移。
    constexpr std::uint32_t kMbrPartitionTableOffset = 446;

    // kMbrPartitionEntryBytes：每个 MBR 分区表项长度。
    constexpr std::uint32_t kMbrPartitionEntryBytes = 16;

    // kGptHeaderLba：主 GPT Header 所在 LBA。
    constexpr std::uint64_t kGptHeaderLba = 1;

    // le16：
    // - 从 QByteArray 读取 little-endian 16 位数；
    // - bytes 为数据缓冲，offset 为缓冲内偏移；
    // - 越界时返回 0。
    std::uint16_t le16(const QByteArray& bytes, const std::uint64_t offset)
    {
        if (offset + 2 > static_cast<std::uint64_t>(bytes.size()))
        {
            return 0;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
        return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }

    // le32：
    // - 从 QByteArray 读取 little-endian 32 位数；
    // - bytes 为数据缓冲，offset 为缓冲内偏移；
    // - 越界时返回 0。
    std::uint32_t le32(const QByteArray& bytes, const std::uint64_t offset)
    {
        if (offset + 4 > static_cast<std::uint64_t>(bytes.size()))
        {
            return 0;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
        return static_cast<std::uint32_t>(p[0])
            | (static_cast<std::uint32_t>(p[1]) << 8)
            | (static_cast<std::uint32_t>(p[2]) << 16)
            | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    // le64：
    // - 从 QByteArray 读取 little-endian 64 位数；
    // - bytes 为数据缓冲，offset 为缓冲内偏移；
    // - 越界时返回 0。
    std::uint64_t le64(const QByteArray& bytes, const std::uint64_t offset)
    {
        const std::uint64_t low = le32(bytes, offset);
        const std::uint64_t high = le32(bytes, offset + 4);
        return low | (high << 32);
    }

    // latinText：
    // - 从字节缓冲读取 ASCII/OEM 文本；
    // - offset/length 指定范围；
    // - 返回清理后的展示字符串。
    QString latinText(const QByteArray& bytes, const std::uint64_t offset, const std::uint32_t length)
    {
        if (offset + length > static_cast<std::uint64_t>(bytes.size()))
        {
            return QString();
        }
        QString text = QString::fromLatin1(bytes.constData() + offset, static_cast<int>(length));
        text.replace(QChar('\0'), QChar(' '));
        return text.simplified();
    }

    // utf16Text：
    // - 从固定长度 UTF-16LE 字段读取文本；
    // - offset/byteLength 指定范围；
    // - 返回去掉 NUL 和空白后的字符串。
    QString utf16Text(const QByteArray& bytes, const std::uint64_t offset, const std::uint32_t byteLength)
    {
        if (offset + byteLength > static_cast<std::uint64_t>(bytes.size()))
        {
            return QString();
        }
        QString text = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(bytes.constData() + offset),
            static_cast<int>(byteLength / 2));
        const int nulIndex = text.indexOf(QChar('\0'));
        if (nulIndex >= 0)
        {
            text.truncate(nulIndex);
        }
        return text.trimmed();
    }

    // hexValue：
    // - 统一格式化整数为 0x 前缀大写十六进制；
    // - value 为输入数值；
    // - width 为最小位宽。
    QString hexValue(const std::uint64_t value, const int width = 0)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), width, 16, QChar('0'))
            .toUpper();
    }

    // bytesToHexText：
    // - 把小段字节转成空格分隔 HEX；
    // - bytes 为缓冲，offset/length 指定范围；
    // - 越界时自动截断。
    QString bytesToHexText(const QByteArray& bytes, const std::uint64_t offset, const std::uint32_t length)
    {
        if (offset >= static_cast<std::uint64_t>(bytes.size()))
        {
            return QString();
        }
        const std::uint64_t available = std::min<std::uint64_t>(
            length,
            static_cast<std::uint64_t>(bytes.size()) - offset);
        QStringList parts;
        for (std::uint64_t index = 0; index < available; ++index)
        {
            const auto value = static_cast<unsigned char>(bytes.at(static_cast<int>(offset + index)));
            parts << QStringLiteral("%1").arg(static_cast<unsigned int>(value), 2, 16, QChar('0')).toUpper();
        }
        return parts.join(QChar(' '));
    }

    // guidFromBytes：
    // - 从 GPT 小端 GUID 字段生成文本；
    // - bytes 为缓冲，offset 指向 16 字节 GUID；
    // - 返回标准 GUID 文本，不带大括号。
    QString guidFromBytes(const QByteArray& bytes, const std::uint64_t offset)
    {
        if (offset + 16 > static_cast<std::uint64_t>(bytes.size()))
        {
            return QString();
        }
        return QStringLiteral("%1-%2-%3-%4%5-%6%7%8%9%10%11")
            .arg(le32(bytes, offset + 0), 8, 16, QChar('0'))
            .arg(le16(bytes, offset + 4), 4, 16, QChar('0'))
            .arg(le16(bytes, offset + 6), 4, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 8)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 9)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 10)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 11)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 12)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 13)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 14)))), 2, 16, QChar('0'))
            .arg(static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(static_cast<int>(offset + 15)))), 2, 16, QChar('0'))
            .toUpper();
    }

    // addField：
    // - 向字段列表追加一行结构解析结果；
    // - group/name/value/detail 定义展示内容；
    // - offsetBytes/sizeBytes 让 UI 可跳转；
    // - severity 控制告警等级。
    void addField(
        std::vector<DiskStructureField>& fields,
        const QString& group,
        const QString& name,
        const QString& value,
        const QString& detail,
        const std::uint64_t offsetBytes,
        const std::uint32_t sizeBytes,
        const DiskStructureSeverity severity = DiskStructureSeverity::Info)
    {
        DiskStructureField field;
        field.group = group;
        field.name = name;
        field.value = value;
        field.detail = detail;
        field.offsetBytes = offsetBytes;
        field.sizeBytes = sizeBytes;
        field.severity = severity;
        fields.push_back(std::move(field));
    }

    // crc32Table：
    // - 生成 GPT 使用的 IEEE CRC32 表；
    // - 返回静态数组引用。
    const std::array<std::uint32_t, 256>& crc32Table()
    {
        static const std::array<std::uint32_t, 256> table = []()
        {
            std::array<std::uint32_t, 256> values{};
            for (std::uint32_t index = 0; index < 256; ++index)
            {
                std::uint32_t crc = index;
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc & 1U) ? (0xEDB88320U ^ (crc >> 1)) : (crc >> 1);
                }
                values[index] = crc;
            }
            return values;
        }();
        return table;
    }

    // computeCrc32：
    // - 计算 GPT Header/Entry Array 使用的 CRC32；
    // - data 指向输入字节，size 为长度；
    // - 返回 CRC32 值。
    std::uint32_t computeCrc32(const unsigned char* data, const std::size_t size)
    {
        std::uint32_t crc = 0xFFFFFFFFU;
        const auto& table = crc32Table();
        for (std::size_t index = 0; index < size; ++index)
        {
            crc = table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFU;
    }

    // readBoundedSlice：
    // - 从 leadingBytes 读取一段安全切片；
    // - offset/length 使用缓冲内偏移；
    // - 成功返回对应 QByteArray，失败返回空。
    QByteArray readBoundedSlice(const QByteArray& leadingBytes, const std::uint64_t offset, const std::uint64_t length)
    {
        if (length == 0 || offset + length > static_cast<std::uint64_t>(leadingBytes.size()))
        {
            return QByteArray();
        }
        if (length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        {
            return QByteArray();
        }
        return leadingBytes.mid(static_cast<int>(offset), static_cast<int>(length));
    }

    // mbrTypeName：
    // - 把 MBR 类型字节转为简短描述；
    // - typeByte 为分区类型；
    // - 返回展示文本。
    QString mbrTypeName(const std::uint8_t typeByte)
    {
        switch (typeByte)
        {
        case 0x00: return QStringLiteral("空槽");
        case 0x05: return QStringLiteral("扩展分区 CHS");
        case 0x07: return QStringLiteral("NTFS/exFAT/HPFS");
        case 0x0B: return QStringLiteral("FAT32 CHS");
        case 0x0C: return QStringLiteral("FAT32 LBA");
        case 0x0F: return QStringLiteral("扩展分区 LBA");
        case 0x27: return QStringLiteral("Windows Recovery/OEM");
        case 0x82: return QStringLiteral("Linux Swap");
        case 0x83: return QStringLiteral("Linux 文件系统");
        case 0x8E: return QStringLiteral("Linux LVM");
        case 0xEE: return QStringLiteral("GPT Protective MBR");
        case 0xEF: return QStringLiteral("EFI System");
        default: break;
        }
        return QStringLiteral("类型 %1").arg(hexValue(typeByte, 2));
    }

    // gptTypeName：
    // - 把常见 GPT 类型 GUID 转为中文说明；
    // - guidText 为标准 GUID 文本；
    // - 返回展示文本。
    QString gptTypeName(const QString& guidText)
    {
        const QString normalized = guidText.toUpper();
        if (normalized == QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"))
        {
            return QStringLiteral("Microsoft 基本数据");
        }
        if (normalized == QStringLiteral("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"))
        {
            return QStringLiteral("EFI 系统分区");
        }
        if (normalized == QStringLiteral("E3C9E316-0B5C-4DB8-817D-F92DF00215AE"))
        {
            return QStringLiteral("Microsoft 保留分区");
        }
        if (normalized == QStringLiteral("DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"))
        {
            return QStringLiteral("Windows 恢复分区");
        }
        if (normalized == QStringLiteral("0FC63DAF-8483-4772-8E79-3D69D8477DE4"))
        {
            return QStringLiteral("Linux 文件系统");
        }
        if (normalized == QStringLiteral("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"))
        {
            return QStringLiteral("Linux Swap");
        }
        return QStringLiteral("GPT %1").arg(guidText);
    }

    // parseMbr：
    // - 解析 LBA0 的 MBR 签名和四个主分区表项；
    // - report 接收字段与告警；
    // - 无返回值。
    void parseMbr(const DiskDeviceInfo& disk, const QByteArray& leadingBytes, DiskStructureReport& report)
    {
        if (leadingBytes.size() < 512)
        {
            report.warnings << QStringLiteral("前部读取不足 512 字节，无法解析 MBR。");
            return;
        }

        const std::uint16_t signature = le16(leadingBytes, kMbrSignatureOffset);
        addField(
            report.fields,
            QStringLiteral("MBR"),
            QStringLiteral("结束签名"),
            hexValue(signature, 4),
            signature == 0xAA55U ? QStringLiteral("有效 0x55AA") : QStringLiteral("签名异常，磁盘可能为 RAW 或前部损坏"),
            kMbrSignatureOffset,
            2,
            signature == 0xAA55U ? DiskStructureSeverity::Info : DiskStructureSeverity::Error);

        const std::uint32_t diskSignature = le32(leadingBytes, 440);
        addField(
            report.fields,
            QStringLiteral("MBR"),
            QStringLiteral("磁盘签名"),
            hexValue(diskSignature, 8),
            QStringLiteral("Windows MBR 磁盘签名字段"),
            440,
            4);

        for (int slot = 0; slot < 4; ++slot)
        {
            const std::uint64_t entryOffset = kMbrPartitionTableOffset + slot * kMbrPartitionEntryBytes;
            const auto bootFlag = static_cast<std::uint8_t>(leadingBytes.at(static_cast<int>(entryOffset)));
            const auto typeByte = static_cast<std::uint8_t>(leadingBytes.at(static_cast<int>(entryOffset + 4)));
            const std::uint32_t firstLba = le32(leadingBytes, entryOffset + 8);
            const std::uint32_t sectorCount = le32(leadingBytes, entryOffset + 12);
            const QString group = QStringLiteral("MBR 分区槽 %1").arg(slot + 1);
            const std::uint64_t absoluteOffset = static_cast<std::uint64_t>(firstLba) * disk.bytesPerSector;
            const std::uint64_t lengthBytes = static_cast<std::uint64_t>(sectorCount) * disk.bytesPerSector;

            addField(
                report.fields,
                group,
                QStringLiteral("启动标记"),
                hexValue(bootFlag, 2),
                bootFlag == 0x80U ? QStringLiteral("活动分区") : (bootFlag == 0x00U ? QStringLiteral("非活动") : QStringLiteral("非标准启动标记")),
                entryOffset,
                1,
                (bootFlag == 0x00U || bootFlag == 0x80U) ? DiskStructureSeverity::Info : DiskStructureSeverity::Warning);
            addField(
                report.fields,
                group,
                QStringLiteral("分区类型"),
                QStringLiteral("%1 (%2)").arg(hexValue(typeByte, 2), mbrTypeName(typeByte)),
                typeByte == 0xEEU ? QStringLiteral("GPT Protective MBR 保护项") : QStringLiteral("传统 MBR 类型字节"),
                entryOffset + 4,
                1);
            addField(
                report.fields,
                group,
                QStringLiteral("起始 LBA"),
                QString::number(firstLba),
                QStringLiteral("绝对偏移 %1").arg(hexValue(absoluteOffset, 16)),
                entryOffset + 8,
                4,
                (sectorCount > 0 && absoluteOffset >= disk.sizeBytes) ? DiskStructureSeverity::Warning : DiskStructureSeverity::Info);
            addField(
                report.fields,
                group,
                QStringLiteral("扇区数量"),
                QString::number(sectorCount),
                QStringLiteral("长度 %1").arg(DiskEditorBackend::formatBytes(lengthBytes)),
                entryOffset + 12,
                4);
        }
    }

    // parseGpt：
    // - 解析主 GPT Header 和可读取范围内的 GPT Entry；
    // - 计算 Header CRC 和 Entry Array CRC；
    // - 无返回值。
    void parseGpt(const DiskDeviceInfo& disk, const QByteArray& leadingBytes, DiskStructureReport& report)
    {
        const std::uint32_t sectorSize = disk.bytesPerSector == 0 ? 512U : disk.bytesPerSector;
        const std::uint64_t headerOffset = kGptHeaderLba * sectorSize;
        if (headerOffset + 92 > static_cast<std::uint64_t>(leadingBytes.size()))
        {
            report.warnings << QStringLiteral("前部读取不足，无法完整解析主 GPT Header。");
            return;
        }

        const QString signature = latinText(leadingBytes, headerOffset, 8);
        addField(
            report.fields,
            QStringLiteral("GPT Header"),
            QStringLiteral("签名"),
            signature,
            signature == QStringLiteral("EFI PART") ? QStringLiteral("有效 GPT Header 签名") : QStringLiteral("GPT 签名异常"),
            headerOffset,
            8,
            signature == QStringLiteral("EFI PART") ? DiskStructureSeverity::Info : DiskStructureSeverity::Error);

        if (signature != QStringLiteral("EFI PART"))
        {
            return;
        }

        const std::uint32_t revision = le32(leadingBytes, headerOffset + 8);
        const std::uint32_t headerSize = le32(leadingBytes, headerOffset + 12);
        const std::uint32_t storedHeaderCrc = le32(leadingBytes, headerOffset + 16);
        const std::uint64_t currentLba = le64(leadingBytes, headerOffset + 24);
        const std::uint64_t backupLba = le64(leadingBytes, headerOffset + 32);
        const std::uint64_t firstUsableLba = le64(leadingBytes, headerOffset + 40);
        const std::uint64_t lastUsableLba = le64(leadingBytes, headerOffset + 48);
        const QString diskGuid = guidFromBytes(leadingBytes, headerOffset + 56);
        const std::uint64_t entryArrayLba = le64(leadingBytes, headerOffset + 72);
        const std::uint32_t entryCount = le32(leadingBytes, headerOffset + 80);
        const std::uint32_t entrySize = le32(leadingBytes, headerOffset + 84);
        const std::uint32_t storedEntryCrc = le32(leadingBytes, headerOffset + 88);

        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("版本"), hexValue(revision, 8), QStringLiteral("通常为 0x00010000"), headerOffset + 8, 4);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("Header 大小"), QString::number(headerSize), QStringLiteral("用于 CRC 计算的头部长度"), headerOffset + 12, 4);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("当前 LBA"), QString::number(currentLba), QStringLiteral("主 Header 通常为 LBA 1"), headerOffset + 24, 8);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("备份 LBA"), QString::number(backupLba), QStringLiteral("应位于磁盘末尾 LBA"), headerOffset + 32, 8);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("可用 LBA 范围"), QStringLiteral("%1 - %2").arg(firstUsableLba).arg(lastUsableLba), QStringLiteral("分区不应越界到该范围外"), headerOffset + 40, 16);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("磁盘 GUID"), diskGuid, QStringLiteral("GPT Disk GUID"), headerOffset + 56, 16);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("Entry 起始 LBA"), QString::number(entryArrayLba), QStringLiteral("分区项数组起始位置"), headerOffset + 72, 8);
        addField(report.fields, QStringLiteral("GPT Header"), QStringLiteral("Entry 数量/大小"), QStringLiteral("%1 x %2").arg(entryCount).arg(entrySize), QStringLiteral("Windows 常见为 128 x 128"), headerOffset + 80, 8);

        if (headerSize >= 92 && headerOffset + headerSize <= static_cast<std::uint64_t>(leadingBytes.size()))
        {
            QByteArray headerBytes = readBoundedSlice(leadingBytes, headerOffset, headerSize);
            if (!headerBytes.isEmpty())
            {
                for (int index = 16; index < 20; ++index)
                {
                    headerBytes[index] = '\0';
                }
                const std::uint32_t computedCrc = computeCrc32(
                    reinterpret_cast<const unsigned char*>(headerBytes.constData()),
                    static_cast<std::size_t>(headerBytes.size()));
                addField(
                    report.fields,
                    QStringLiteral("GPT Header"),
                    QStringLiteral("Header CRC32"),
                    QStringLiteral("存储 %1 / 计算 %2").arg(hexValue(storedHeaderCrc, 8), hexValue(computedCrc, 8)),
                    computedCrc == storedHeaderCrc ? QStringLiteral("Header CRC 匹配") : QStringLiteral("Header CRC 不匹配"),
                    headerOffset + 16,
                    4,
                    computedCrc == storedHeaderCrc ? DiskStructureSeverity::Info : DiskStructureSeverity::Error);
            }
        }
        else
        {
            addField(
                report.fields,
                QStringLiteral("GPT Header"),
                QStringLiteral("Header CRC32"),
                hexValue(storedHeaderCrc, 8),
                QStringLiteral("Header 大小异常或读取不足，未计算 CRC"),
                headerOffset + 16,
                4,
                DiskStructureSeverity::Warning);
        }

        const std::uint64_t entryArrayOffset = entryArrayLba * sectorSize;
        const std::uint64_t entryArrayBytes = static_cast<std::uint64_t>(entryCount) * entrySize;
        const QByteArray entryBytes = readBoundedSlice(leadingBytes, entryArrayOffset, entryArrayBytes);
        if (!entryBytes.isEmpty())
        {
            const std::uint32_t computedEntryCrc = computeCrc32(
                reinterpret_cast<const unsigned char*>(entryBytes.constData()),
                static_cast<std::size_t>(entryBytes.size()));
            addField(
                report.fields,
                QStringLiteral("GPT Entry Array"),
                QStringLiteral("Entry Array CRC32"),
                QStringLiteral("存储 %1 / 计算 %2").arg(hexValue(storedEntryCrc, 8), hexValue(computedEntryCrc, 8)),
                computedEntryCrc == storedEntryCrc ? QStringLiteral("Entry Array CRC 匹配") : QStringLiteral("Entry Array CRC 不匹配"),
                headerOffset + 88,
                4,
                computedEntryCrc == storedEntryCrc ? DiskStructureSeverity::Info : DiskStructureSeverity::Error);
        }
        else
        {
            addField(
                report.fields,
                QStringLiteral("GPT Entry Array"),
                QStringLiteral("Entry Array CRC32"),
                hexValue(storedEntryCrc, 8),
                QStringLiteral("Entry Array 超出当前前部读取范围，未计算 CRC"),
                headerOffset + 88,
                4,
                DiskStructureSeverity::Warning);
        }

        const std::uint32_t safeEntrySize = entrySize == 0 ? 128U : entrySize;
        const std::uint32_t entriesToShow = std::min<std::uint32_t>(entryCount, 64U);
        for (std::uint32_t index = 0; index < entriesToShow; ++index)
        {
            const std::uint64_t entryOffset = entryArrayOffset + static_cast<std::uint64_t>(index) * safeEntrySize;
            if (entryOffset + std::min<std::uint32_t>(safeEntrySize, 128U) > static_cast<std::uint64_t>(leadingBytes.size()))
            {
                break;
            }

            const QString typeGuid = guidFromBytes(leadingBytes, entryOffset);
            const QString uniqueGuid = guidFromBytes(leadingBytes, entryOffset + 16);
            const std::uint64_t firstLba = le64(leadingBytes, entryOffset + 32);
            const std::uint64_t lastLba = le64(leadingBytes, entryOffset + 40);
            const std::uint64_t attributes = le64(leadingBytes, entryOffset + 48);
            const QString name = utf16Text(leadingBytes, entryOffset + 56, std::min<std::uint32_t>(72U, safeEntrySize > 56 ? safeEntrySize - 56 : 0));
            const bool emptyEntry = typeGuid == QStringLiteral("00000000-0000-0000-0000-000000000000");
            if (emptyEntry)
            {
                continue;
            }

            const QString group = QStringLiteral("GPT Entry %1").arg(index + 1);
            const std::uint64_t partitionOffset = firstLba * sectorSize;
            const std::uint64_t partitionLength = lastLba >= firstLba
                ? (lastLba - firstLba + 1ULL) * sectorSize
                : 0ULL;
            addField(report.fields, group, QStringLiteral("类型 GUID"), typeGuid, gptTypeName(typeGuid), entryOffset, 16);
            addField(report.fields, group, QStringLiteral("唯一 GUID"), uniqueGuid, QStringLiteral("Partition GUID"), entryOffset + 16, 16);
            addField(report.fields, group, QStringLiteral("LBA 范围"), QStringLiteral("%1 - %2").arg(firstLba).arg(lastLba), QStringLiteral("偏移 %1，长度 %2").arg(hexValue(partitionOffset, 16), DiskEditorBackend::formatBytes(partitionLength)), entryOffset + 32, 16);
            addField(report.fields, group, QStringLiteral("属性"), hexValue(attributes, 16), attributes == 0 ? QStringLiteral("无特殊属性") : QStringLiteral("GPT Attributes 位图"), entryOffset + 48, 8);
            addField(report.fields, group, QStringLiteral("名称"), name.isEmpty() ? QStringLiteral("<未命名>") : name, QStringLiteral("UTF-16LE 分区名称"), entryOffset + 56, std::min<std::uint32_t>(72U, safeEntrySize > 56 ? safeEntrySize - 56 : 0));
        }
    }

    // BootSectorAnalysis 作用：
    // - 缓存启动扇区的文件系统类型和 BPB 字段；
    // - 避免 NTFS/exFAT 字段偏移差异导致错误读取。
    struct BootSectorAnalysis
    {
        QString kind;                    // kind：NTFS/FAT32/exFAT/未知。
        std::uint16_t bytesPerSector = 0; // bytesPerSector：每扇区字节数。
        std::uint32_t sectorsPerCluster = 0; // sectorsPerCluster：每簇扇区数。
        std::uint64_t totalSectors = 0;  // totalSectors：卷内总扇区数。
        bool hasFatBpb = false;          // hasFatBpb：是否可使用传统 BPB 字段。
    };

    // analyzeBootSector：
    // - 依据 OEM 名称和字段特征识别启动扇区文件系统；
    // - bytes 为起始扇区数据；
    // - 返回带关键 BPB 字段的分析结果。
    BootSectorAnalysis analyzeBootSector(const QByteArray& bytes)
    {
        BootSectorAnalysis analysis;
        const QString oem = latinText(bytes, 3, 8).toUpper();
        if (oem.contains(QStringLiteral("NTFS")))
        {
            const std::uint16_t bps = le16(bytes, 11);
            const std::uint32_t spc = static_cast<std::uint8_t>(bytes.at(13));
            analysis.kind = QStringLiteral("NTFS");
            analysis.bytesPerSector = bps;
            analysis.sectorsPerCluster = spc;
            analysis.totalSectors = le64(bytes, 40);
            analysis.hasFatBpb = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc != 0;
            return analysis;
        }
        if (oem.contains(QStringLiteral("EXFAT")))
        {
            const auto bytesPerSectorShift = static_cast<std::uint8_t>(bytes.at(108));
            const auto sectorsPerClusterShift = static_cast<std::uint8_t>(bytes.at(109));
            analysis.kind = QStringLiteral("exFAT");
            analysis.bytesPerSector = bytesPerSectorShift < 16
                ? static_cast<std::uint16_t>(1U << bytesPerSectorShift)
                : 0;
            analysis.sectorsPerCluster = sectorsPerClusterShift < 31
                ? static_cast<std::uint32_t>(1U << sectorsPerClusterShift)
                : 0;
            analysis.totalSectors = le64(bytes, 72);
            analysis.hasFatBpb = false;
            return analysis;
        }
        const QString fat16 = latinText(bytes, 54, 8).toUpper();
        const QString fat32 = latinText(bytes, 82, 8).toUpper();
        if (fat32.contains(QStringLiteral("FAT32")))
        {
            const std::uint16_t bps = le16(bytes, 11);
            const std::uint32_t spc = static_cast<std::uint8_t>(bytes.at(13));
            analysis.kind = QStringLiteral("FAT32");
            analysis.bytesPerSector = bps;
            analysis.sectorsPerCluster = spc;
            analysis.totalSectors = le16(bytes, 19) != 0 ? le16(bytes, 19) : le32(bytes, 32);
            analysis.hasFatBpb = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc != 0;
            return analysis;
        }
        if (fat16.contains(QStringLiteral("FAT")))
        {
            const std::uint16_t bps = le16(bytes, 11);
            const std::uint32_t spc = static_cast<std::uint8_t>(bytes.at(13));
            analysis.kind = QStringLiteral("FAT12/16");
            analysis.bytesPerSector = bps;
            analysis.sectorsPerCluster = spc;
            analysis.totalSectors = le16(bytes, 19) != 0 ? le16(bytes, 19) : le32(bytes, 32);
            analysis.hasFatBpb = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc != 0;
            return analysis;
        }
        analysis.kind = QStringLiteral("未知/非标准");
        analysis.bytesPerSector = le16(bytes, 11);
        analysis.sectorsPerCluster = static_cast<std::uint8_t>(bytes.at(13));
        analysis.totalSectors = le16(bytes, 19) != 0 ? le16(bytes, 19) : le32(bytes, 32);
        analysis.hasFatBpb = false;
        return analysis;
    }

    // parseBootSectorAt：
    // - 解析指定绝对偏移处的启动扇区常见字段；
    // - groupPrefix 用于区分整盘 LBA0 或具体分区；
    // - 无返回值。
    void parseBootSectorAt(
        const DiskDeviceInfo& disk,
        const QByteArray& leadingBytes,
        const std::uint64_t baseOffset,
        const QString& groupPrefix,
        DiskStructureReport& report)
    {
        if (baseOffset + 512 > static_cast<std::uint64_t>(leadingBytes.size()))
        {
            return;
        }

        const QByteArray sector = leadingBytes.mid(static_cast<int>(baseOffset), 512);
        const std::uint16_t signature = le16(sector, 510);
        if (signature != 0xAA55U)
        {
            return;
        }

        const BootSectorAnalysis analysis = analyzeBootSector(sector);
        const QString kind = analysis.kind;
        const QString group = QStringLiteral("%1 启动扇区").arg(groupPrefix);
        const std::uint16_t bytesPerSector = analysis.bytesPerSector;
        const std::uint32_t sectorsPerCluster = analysis.sectorsPerCluster;
        const std::uint16_t reservedSectors = le16(sector, 14);
        const auto fatCount = static_cast<std::uint8_t>(sector.at(16));
        const std::uint64_t totalSectors = analysis.totalSectors;

        addField(report.fields, group, QStringLiteral("文件系统识别"), kind, QStringLiteral("依据 OEM 名称和 BPB 字段推断"), baseOffset + 3, 8);
        addField(report.fields, group, QStringLiteral("OEM 名称"), latinText(sector, 3, 8), QStringLiteral("启动扇区 OEM 字符串"), baseOffset + 3, 8);
        addField(report.fields, group, QStringLiteral("每扇区字节"), QString::number(bytesPerSector), bytesPerSector == disk.bytesPerSector ? QStringLiteral("与磁盘逻辑扇区一致") : QStringLiteral("与磁盘逻辑扇区不同或为 0"), analysis.hasFatBpb ? baseOffset + 11 : baseOffset + 108, analysis.hasFatBpb ? 2 : 1, (bytesPerSector == 0 || (disk.bytesPerSector != 0 && bytesPerSector != disk.bytesPerSector)) ? DiskStructureSeverity::Warning : DiskStructureSeverity::Info);
        addField(report.fields, group, QStringLiteral("每簇扇区"), QString::number(sectorsPerCluster), QStringLiteral("簇大小约 %1").arg(DiskEditorBackend::formatBytes(static_cast<std::uint64_t>(bytesPerSector) * sectorsPerCluster)), analysis.hasFatBpb ? baseOffset + 13 : baseOffset + 109, 1);
        if (analysis.hasFatBpb)
        {
            addField(report.fields, group, QStringLiteral("保留扇区/FAT 数"), QStringLiteral("%1 / %2").arg(reservedSectors).arg(fatCount), QStringLiteral("FAT/NTFS BPB 传统字段"), baseOffset + 14, 3);
        }
        addField(report.fields, group, QStringLiteral("总扇区"), QString::number(totalSectors), totalSectors == 0 ? QStringLiteral("总扇区字段为空") : QStringLiteral("约 %1").arg(DiskEditorBackend::formatBytes(totalSectors * bytesPerSector)), analysis.hasFatBpb ? baseOffset + 19 : baseOffset + 72, analysis.hasFatBpb ? 4 : 8);

        if (kind == QStringLiteral("NTFS"))
        {
            const std::uint64_t totalSectors64 = le64(sector, 40);
            const std::uint64_t mftCluster = le64(sector, 48);
            const std::uint64_t mirrorCluster = le64(sector, 56);
            const auto clustersPerFileRecord = static_cast<signed char>(sector.at(64));
            addField(report.fields, group, QStringLiteral("NTFS 总扇区"), QString::number(totalSectors64), QStringLiteral("NTFS BPB 64 位总扇区"), baseOffset + 40, 8);
            addField(report.fields, group, QStringLiteral("$MFT 簇号"), QString::number(mftCluster), QStringLiteral("$MFT 物理偏移约 %1").arg(hexValue(baseOffset + mftCluster * sectorsPerCluster * bytesPerSector, 16)), baseOffset + 48, 8);
            addField(report.fields, group, QStringLiteral("$MFTMirr 簇号"), QString::number(mirrorCluster), QStringLiteral("MFT 镜像簇号"), baseOffset + 56, 8);
            addField(report.fields, group, QStringLiteral("文件记录大小编码"), QString::number(clustersPerFileRecord), clustersPerFileRecord < 0 ? QStringLiteral("记录大小为 2^%1 字节").arg(-clustersPerFileRecord) : QStringLiteral("记录大小为簇数倍数"), baseOffset + 64, 1);
        }
        else if (kind == QStringLiteral("FAT32"))
        {
            const std::uint32_t sectorsPerFat = le32(sector, 36);
            const std::uint32_t rootCluster = le32(sector, 44);
            const std::uint16_t fsInfoSector = le16(sector, 48);
            addField(report.fields, group, QStringLiteral("FAT32 每 FAT 扇区"), QString::number(sectorsPerFat), QStringLiteral("FAT 表长度"), baseOffset + 36, 4);
            addField(report.fields, group, QStringLiteral("FAT32 根目录簇"), QString::number(rootCluster), QStringLiteral("根目录起始簇"), baseOffset + 44, 4);
            addField(report.fields, group, QStringLiteral("FAT32 FSInfo 扇区"), QString::number(fsInfoSector), QStringLiteral("通常为 1"), baseOffset + 48, 2);
        }
        else if (kind == QStringLiteral("exFAT"))
        {
            const std::uint64_t volumeOffset = le64(sector, 64);
            const std::uint64_t volumeLength = le64(sector, 72);
            const std::uint32_t fatOffset = le32(sector, 80);
            const std::uint32_t clusterHeapOffset = le32(sector, 88);
            const auto bytesPerSectorShift = static_cast<std::uint8_t>(sector.at(108));
            const auto sectorsPerClusterShift = static_cast<std::uint8_t>(sector.at(109));
            addField(report.fields, group, QStringLiteral("exFAT 卷偏移/长度"), QStringLiteral("%1 / %2").arg(volumeOffset).arg(volumeLength), QStringLiteral("以扇区为单位"), baseOffset + 64, 16);
            addField(report.fields, group, QStringLiteral("exFAT FAT/簇堆偏移"), QStringLiteral("%1 / %2").arg(fatOffset).arg(clusterHeapOffset), QStringLiteral("以扇区为单位"), baseOffset + 80, 12);
            addField(report.fields, group, QStringLiteral("exFAT 扇区/簇位移"), QStringLiteral("%1 / %2").arg(bytesPerSectorShift).arg(sectorsPerClusterShift), QStringLiteral("扇区大小=2^shift，簇扇区数=2^shift"), baseOffset + 108, 2);
        }
    }

    // severityFromMap：
    // - 把基础后端 QVariantMap 中的 severity 整数转为高级模型枚举；
    // - severityValue 约定 0=信息、1=警告、2=错误；
    // - 返回 DiskStructureSeverity。
    DiskStructureSeverity severityFromMap(const int severityValue)
    {
        if (severityValue >= 2)
        {
            return DiskStructureSeverity::Error;
        }
        if (severityValue == 1)
        {
            return DiskStructureSeverity::Warning;
        }
        return DiskStructureSeverity::Info;
    }

    // boolText：
    // - 把布尔值转是/否；
    // - value 为输入布尔；
    // - 返回中文文本。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    // addHealth：
    // - 向高级健康条目列表追加一行基础信息；
    // - category/name/value/detail 为展示内容；
    // - severity 为严重度。
    void addHealth(
        std::vector<DiskHealthItem>& items,
        const QString& category,
        const QString& name,
        const QString& value,
        const QString& detail,
        const DiskStructureSeverity severity = DiskStructureSeverity::Info)
    {
        DiskHealthItem item;
        item.category = category;
        item.name = name;
        item.value = value;
        item.detail = detail;
        item.severity = severity;
        items.push_back(std::move(item));
    }
}

namespace ks::misc
{
    DiskStructureReport DiskStructureParser::buildReport(
        const DiskDeviceInfo& disk,
        const QByteArray& leadingBytes,
        QString& errorTextOut)
    {
        DiskStructureReport report;
        errorTextOut.clear();

        if (leadingBytes.isEmpty())
        {
            errorTextOut = QStringLiteral("前部扇区缓冲为空。");
            return report;
        }

        parseMbr(disk, leadingBytes, report);
        parseGpt(disk, leadingBytes, report);
        parseBootSectorAt(disk, leadingBytes, 0, QStringLiteral("LBA0"), report);

        int parsedBootSectors = 0;
        for (const DiskPartitionInfo& partition : disk.partitions)
        {
            if (partition.partitionNumber == 0 || partition.lengthBytes == 0)
            {
                continue;
            }
            parseBootSectorAt(
                disk,
                leadingBytes,
                partition.offsetBytes,
                partition.name.isEmpty() ? QStringLiteral("分区 %1").arg(partition.partitionNumber) : partition.name,
                report);
            ++parsedBootSectors;
            if (parsedBootSectors >= 16)
            {
                break;
            }
        }

        QString volumeError;
        report.volumes = collectVolumeMapping(disk.diskIndex, volumeError);
        if (!volumeError.isEmpty())
        {
            report.warnings << volumeError;
        }

        QString healthError;
        report.healthItems = collectHealthItems(disk, healthError);
        if (!healthError.isEmpty())
        {
            report.warnings << healthError;
        }

        if (report.fields.empty())
        {
            report.warnings << QStringLiteral("未解析到可识别结构字段，可能需要管理员权限或更大的前部读取范围。");
        }
        return report;
    }

    std::vector<DiskVolumeInfo> DiskStructureParser::collectVolumeMapping(
        const int diskIndex,
        QString& errorTextOut)
    {
        std::vector<DiskVolumeInfo> volumes;
        errorTextOut.clear();

        const std::vector<QVariantMap> maps = DiskEditorBackend::queryVolumeMappings(diskIndex, errorTextOut);
        volumes.reserve(maps.size());
        for (const QVariantMap& map : maps)
        {
            DiskVolumeInfo volume;
            volume.volumeName = map.value(QStringLiteral("volumeName")).toString();
            volume.mountPoints = map.value(QStringLiteral("mountPoints")).toString();
            volume.devicePath = map.value(QStringLiteral("devicePath")).toString();
            volume.fileSystem = map.value(QStringLiteral("fileSystem")).toString();
            volume.label = map.value(QStringLiteral("label")).toString();
            volume.diskNumber = map.value(QStringLiteral("diskNumber")).toInt();
            volume.offsetBytes = map.value(QStringLiteral("offsetBytes")).toULongLong();
            volume.lengthBytes = map.value(QStringLiteral("lengthBytes")).toULongLong();
            volumes.push_back(std::move(volume));
        }
        return volumes;
    }

    std::vector<DiskHealthItem> DiskStructureParser::collectHealthItems(
        const DiskDeviceInfo& disk,
        QString& errorTextOut)
    {
        std::vector<DiskHealthItem> items;
        errorTextOut.clear();

        addHealth(items, QStringLiteral("基础"), QStringLiteral("设备路径"), disk.devicePath, QStringLiteral("物理磁盘路径"));
        addHealth(items, QStringLiteral("基础"), QStringLiteral("型号"), disk.model.isEmpty() ? QStringLiteral("<未知>") : disk.model, QStringLiteral("STORAGE_DEVICE_DESCRIPTOR"));
        addHealth(items, QStringLiteral("基础"), QStringLiteral("序列号"), disk.serial.isEmpty() ? QStringLiteral("<未知>") : disk.serial, QStringLiteral("部分桥接器可能隐藏序列号"));
        addHealth(items, QStringLiteral("基础"), QStringLiteral("总线"), disk.busType.isEmpty() ? QStringLiteral("<未知>") : disk.busType, QStringLiteral("SATA/NVMe/USB 等"));
        addHealth(items, QStringLiteral("基础"), QStringLiteral("可移动介质"), boolText(disk.removable), QStringLiteral("RemovableMedia 标志"));
        addHealth(items, QStringLiteral("容量"), QStringLiteral("容量"), DiskEditorBackend::formatBytes(disk.sizeBytes), QStringLiteral("%1 字节").arg(static_cast<qulonglong>(disk.sizeBytes)));
        addHealth(items, QStringLiteral("容量"), QStringLiteral("逻辑/物理扇区"), QStringLiteral("%1 / %2").arg(disk.bytesPerSector).arg(disk.physicalBytesPerSector), QStringLiteral("写入保护以逻辑扇区为基准"));

        const std::vector<QVariantMap> maps = DiskEditorBackend::queryHealthItems(disk.devicePath, errorTextOut);
        for (const QVariantMap& map : maps)
        {
            DiskHealthItem item;
            item.category = map.value(QStringLiteral("category")).toString();
            item.name = map.value(QStringLiteral("name")).toString();
            item.value = map.value(QStringLiteral("value")).toString();
            item.detail = map.value(QStringLiteral("detail")).toString();
            item.severity = severityFromMap(map.value(QStringLiteral("severity")).toInt());
            items.push_back(std::move(item));
        }
        return items;
    }
}
