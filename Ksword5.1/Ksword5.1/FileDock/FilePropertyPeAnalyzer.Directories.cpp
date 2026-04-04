#include "FilePropertyPeAnalyzer.h"
#include "FilePropertyPeAnalyzer.Internal.h"

// ============================================================
// FilePropertyPeAnalyzer.Directories.cpp
// 作用：
// 1) 承载 PE 数据目录级扩展解析；
// 2) 预留给资源目录、重定位、调试目录、CLR、证书摘要；
// 3) 继续拆分 PE 解析逻辑，避免单文件再次超长。
// ============================================================

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextStream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <corhdr.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace file_dock_detail::pe_tables_detail
{
    namespace
    {
        // CvInfoPdb70：
        // - 作用：描述 RSDS CodeView 记录头；
        // - 用于提取 PDB 路径。
        struct CvInfoPdb70
        {
            std::uint32_t signature = 0;
            std::uint8_t guid[16] = {};
            std::uint32_t age = 0;
        };

        // readPodAtOffset 作用：
        // - 从原始字节缓冲安全读取一个 POD 结构；
        // - 读取前做边界校验，避免越界解释。
        template <typename T>
        bool readPodAtOffset(const QByteArray& fileBytes, const qsizetype offsetValue, T* valueOut)
        {
            if (valueOut == nullptr || offsetValue < 0)
            {
                return false;
            }

            const qsizetype endOffset = offsetValue + static_cast<qsizetype>(sizeof(T));
            if (endOffset > fileBytes.size())
            {
                return false;
            }

            std::memcpy(valueOut, fileBytes.constData() + offsetValue, sizeof(T));
            return true;
        }

        // readNullTerminatedAsciiAtOffset 作用：
        // - 从文件原始字节按偏移读取 ANSI 字符串。
        QString readNullTerminatedAsciiAtOffset(const QByteArray& fileBytes, const qsizetype offsetValue)
        {
            if (offsetValue < 0 || offsetValue >= fileBytes.size())
            {
                return QString();
            }

            qsizetype currentOffset = offsetValue;
            while (currentOffset < fileBytes.size() && fileBytes.at(currentOffset) != '\0')
            {
                ++currentOffset;
            }
            return QString::fromLatin1(fileBytes.constData() + offsetValue, currentOffset - offsetValue);
        }

        // readResourceNameAtOffset 作用：
        // - 读取 IMAGE_RESOURCE_DIR_STRING_U 资源名称。
        QString readResourceNameAtOffset(const QByteArray& fileBytes, const qsizetype offsetValue)
        {
            std::uint16_t nameLength = 0;
            if (!readPodAtOffset(fileBytes, offsetValue, &nameLength))
            {
                return QStringLiteral("<名称读取失败>");
            }

            const qsizetype charOffset = offsetValue + static_cast<qsizetype>(sizeof(std::uint16_t));
            const qsizetype byteLength = static_cast<qsizetype>(nameLength) * static_cast<qsizetype>(sizeof(wchar_t));
            if (charOffset + byteLength > fileBytes.size())
            {
                return QStringLiteral("<名称越界>");
            }

            return QString::fromUtf16(
                reinterpret_cast<const char16_t*>(fileBytes.constData() + charOffset),
                static_cast<qsizetype>(nameLength));
        }

        // rvaToFileOffset 作用：
        // - 把 RVA 映射到文件偏移；
        // - 先尝试区段映射，命中失败时再对头部区域做兜底。
        bool rvaToFileOffset(
            const std::uint32_t rvaValue,
            const std::uint32_t sizeOfHeadersValue,
            const std::vector<IMAGE_SECTION_HEADER>& sectionList,
            std::uint32_t* fileOffsetOut)
        {
            if (fileOffsetOut == nullptr)
            {
                return false;
            }

            for (const IMAGE_SECTION_HEADER& sectionHeader : sectionList)
            {
                const std::uint32_t sectionRva = sectionHeader.VirtualAddress;
                const std::uint32_t sectionSpan = std::max(
                    sectionHeader.Misc.VirtualSize,
                    sectionHeader.SizeOfRawData);
                if (sectionSpan == 0)
                {
                    continue;
                }

                if (rvaValue >= sectionRva && rvaValue < sectionRva + sectionSpan)
                {
                    *fileOffsetOut = sectionHeader.PointerToRawData + (rvaValue - sectionRva);
                    return true;
                }
            }

            if (rvaValue < sizeOfHeadersValue)
            {
                *fileOffsetOut = rvaValue;
                return true;
            }
            return false;
        }

        // resourceTypeIdToText 作用：
        // - 把资源类型 ID 转换为可读文本。
        QString resourceTypeIdToText(const std::uint32_t typeIdValue)
        {
            switch (typeIdValue)
            {
            case 1: return QStringLiteral("CURSOR");
            case 2: return QStringLiteral("BITMAP");
            case 3: return QStringLiteral("ICON");
            case 4: return QStringLiteral("MENU");
            case 5: return QStringLiteral("DIALOG");
            case 6: return QStringLiteral("STRING");
            case 9: return QStringLiteral("ACCELERATOR");
            case 10: return QStringLiteral("RCDATA");
            case 14: return QStringLiteral("GROUP_ICON");
            case 16: return QStringLiteral("VERSION");
            case 24: return QStringLiteral("MANIFEST");
            default: return QStringLiteral("TYPE_%1").arg(typeIdValue);
            }
        }
    }

    // dumpResourceDirectory：
    // - 输出资源目录一级概览；
    // - 仅统计资源类型，不做完整递归树展开。
    void dumpResourceDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        const std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& resourceDirectory)
    {
        outputStream << "\n[资源目录]\n";
        if (resourceDirectory.VirtualAddress == 0 || resourceDirectory.Size == 0)
        {
            outputStream << "无资源目录。\n";
            return;
        }

        std::uint32_t resourceBaseOffset = 0;
        if (!rvaToFileOffset(resourceDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &resourceBaseOffset))
        {
            outputStream << "资源目录 RVA 无法映射到文件偏移。\n";
            return;
        }

        IMAGE_RESOURCE_DIRECTORY rootDirectory{};
        if (!readPodAtOffset(fileBytes, resourceBaseOffset, &rootDirectory))
        {
            outputStream << "资源目录头读取失败。\n";
            return;
        }

        const int entryCount = rootDirectory.NumberOfNamedEntries + rootDirectory.NumberOfIdEntries;
        outputStream << QStringLiteral("一级资源节点数: %1\n").arg(entryCount);

        qsizetype entryOffset = static_cast<qsizetype>(resourceBaseOffset) + static_cast<qsizetype>(sizeof(IMAGE_RESOURCE_DIRECTORY));
        for (int entryIndex = 0; entryIndex < entryCount; ++entryIndex)
        {
            IMAGE_RESOURCE_DIRECTORY_ENTRY directoryEntry{};
            if (!readPodAtOffset(fileBytes, entryOffset, &directoryEntry))
            {
                outputStream << "资源目录项读取失败。\n";
                break;
            }

            QString typeText;
            if ((directoryEntry.Name & IMAGE_RESOURCE_NAME_IS_STRING) != 0)
            {
                const qsizetype nameOffset =
                    static_cast<qsizetype>(resourceBaseOffset) + static_cast<qsizetype>(directoryEntry.Name & 0x7FFFFFFF);
                typeText = readResourceNameAtOffset(fileBytes, nameOffset);
            }
            else
            {
                typeText = resourceTypeIdToText(directoryEntry.Name);
            }

            outputStream << QStringLiteral("  [%1] %2\n").arg(entryIndex).arg(typeText);

            if ((directoryEntry.OffsetToData & IMAGE_RESOURCE_DATA_IS_DIRECTORY) != 0)
            {
                const qsizetype level2DirOffset =
                    static_cast<qsizetype>(resourceBaseOffset) + static_cast<qsizetype>(directoryEntry.OffsetToData & 0x7FFFFFFF);
                IMAGE_RESOURCE_DIRECTORY level2Directory{};
                if (readPodAtOffset(fileBytes, level2DirOffset, &level2Directory))
                {
                    const int level2EntryCount =
                        level2Directory.NumberOfNamedEntries + level2Directory.NumberOfIdEntries;
                    qsizetype level2EntryOffset =
                        level2DirOffset + static_cast<qsizetype>(sizeof(IMAGE_RESOURCE_DIRECTORY));
                    for (int level2Index = 0; level2Index < level2EntryCount; ++level2Index)
                    {
                        IMAGE_RESOURCE_DIRECTORY_ENTRY level2Entry{};
                        if (!readPodAtOffset(fileBytes, level2EntryOffset, &level2Entry))
                        {
                            break;
                        }

                        QString nameText;
                        if ((level2Entry.Name & IMAGE_RESOURCE_NAME_IS_STRING) != 0)
                        {
                            const qsizetype nameOffset =
                                static_cast<qsizetype>(resourceBaseOffset) + static_cast<qsizetype>(level2Entry.Name & 0x7FFFFFFF);
                            nameText = readResourceNameAtOffset(fileBytes, nameOffset);
                        }
                        else
                        {
                            nameText = QStringLiteral("ID_%1").arg(level2Entry.Name);
                        }

                        outputStream << QStringLiteral("      名称[%1]: %2\n")
                            .arg(level2Index)
                            .arg(nameText);

                        if ((level2Entry.OffsetToData & IMAGE_RESOURCE_DATA_IS_DIRECTORY) != 0)
                        {
                            const qsizetype level3DirOffset =
                                static_cast<qsizetype>(resourceBaseOffset) + static_cast<qsizetype>(level2Entry.OffsetToData & 0x7FFFFFFF);
                            IMAGE_RESOURCE_DIRECTORY level3Directory{};
                            if (readPodAtOffset(fileBytes, level3DirOffset, &level3Directory))
                            {
                                const int level3EntryCount =
                                    level3Directory.NumberOfNamedEntries + level3Directory.NumberOfIdEntries;
                                qsizetype level3EntryOffset =
                                    level3DirOffset + static_cast<qsizetype>(sizeof(IMAGE_RESOURCE_DIRECTORY));
                                for (int level3Index = 0; level3Index < level3EntryCount; ++level3Index)
                                {
                                    IMAGE_RESOURCE_DIRECTORY_ENTRY level3Entry{};
                                    if (!readPodAtOffset(fileBytes, level3EntryOffset, &level3Entry))
                                    {
                                        break;
                                    }
                                    outputStream << QStringLiteral("          语言[%1]: ID_%2\n")
                                        .arg(level3Index)
                                        .arg(level3Entry.Name & 0xFFFF);
                                    level3EntryOffset += sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY);
                                }
                            }
                        }

                        level2EntryOffset += sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY);
                    }
                }
            }
            entryOffset += sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY);
        }
    }

    // dumpBaseRelocDirectory：
    // - 输出重定位表块摘要。
    void dumpBaseRelocDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        const std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& relocDirectory)
    {
        outputStream << "\n[重定位表]\n";
        if (relocDirectory.VirtualAddress == 0 || relocDirectory.Size == 0)
        {
            outputStream << "无重定位表。\n";
            return;
        }

        std::uint32_t relocOffset = 0;
        if (!rvaToFileOffset(relocDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &relocOffset))
        {
            outputStream << "重定位表 RVA 无法映射到文件偏移。\n";
            return;
        }

        std::uint32_t remainingBytes = relocDirectory.Size;
        int blockIndex = 0;
        while (remainingBytes >= sizeof(IMAGE_BASE_RELOCATION))
        {
            IMAGE_BASE_RELOCATION relocBlock{};
            if (!readPodAtOffset(fileBytes, relocOffset, &relocBlock))
            {
                outputStream << "重定位块读取失败。\n";
                return;
            }

            if (relocBlock.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
            {
                break;
            }

            const std::uint32_t entryCount =
                (relocBlock.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(std::uint16_t);
            outputStream << QStringLiteral("块[%1] PageRVA=0x%2 EntryCount=%3\n")
                .arg(blockIndex)
                .arg(QString::number(relocBlock.VirtualAddress, 16).toUpper())
                .arg(entryCount);

            relocOffset += relocBlock.SizeOfBlock;
            if (remainingBytes < relocBlock.SizeOfBlock)
            {
                break;
            }
            remainingBytes -= relocBlock.SizeOfBlock;
            ++blockIndex;
        }
    }

    // dumpDebugDirectory：
    // - 输出调试目录摘要。
    void dumpDebugDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        const std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& debugDirectory)
    {
        outputStream << "\n[调试目录]\n";
        if (debugDirectory.VirtualAddress == 0 || debugDirectory.Size == 0)
        {
            outputStream << "无调试目录。\n";
            return;
        }

        std::uint32_t debugOffset = 0;
        if (!rvaToFileOffset(debugDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &debugOffset))
        {
            outputStream << "调试目录 RVA 无法映射到文件偏移。\n";
            return;
        }

        const std::uint32_t debugEntryCount = debugDirectory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
        outputStream << QStringLiteral("调试目录项数量: %1\n").arg(debugEntryCount);
        for (std::uint32_t debugIndex = 0; debugIndex < debugEntryCount; ++debugIndex)
        {
            IMAGE_DEBUG_DIRECTORY debugEntry{};
            if (!readPodAtOffset(fileBytes, debugOffset, &debugEntry))
            {
                outputStream << "调试目录读取失败。\n";
                break;
            }

            outputStream << QStringLiteral("  [%1] Type=%2 RawDataSize=0x%3 RawOffset=0x%4\n")
                .arg(debugIndex)
                .arg(debugEntry.Type)
                .arg(QString::number(debugEntry.SizeOfData, 16).toUpper())
                .arg(QString::number(debugEntry.PointerToRawData, 16).toUpper());

            if (debugEntry.Type == IMAGE_DEBUG_TYPE_CODEVIEW
                && debugEntry.PointerToRawData > 0
                && debugEntry.SizeOfData >= sizeof(CvInfoPdb70))
            {
                CvInfoPdb70 codeViewHeader{};
                if (readPodAtOffset(fileBytes, debugEntry.PointerToRawData, &codeViewHeader)
                    && codeViewHeader.signature == 0x53445352U)
                {
                    const QString pdbPathText = readNullTerminatedAsciiAtOffset(
                        fileBytes,
                        static_cast<qsizetype>(debugEntry.PointerToRawData) + static_cast<qsizetype>(sizeof(CvInfoPdb70)));
                    outputStream << QStringLiteral("      CodeView=RSDS Age=%1 PDB=%2\n")
                        .arg(codeViewHeader.age)
                        .arg(pdbPathText);
                }
            }
            debugOffset += sizeof(IMAGE_DEBUG_DIRECTORY);
        }
    }

    // dumpBoundImportDirectory：
    // - 输出绑定导入表摘要。
    void dumpBoundImportDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        const std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& boundImportDirectory)
    {
        outputStream << "\n[绑定导入表]\n";
        if (boundImportDirectory.VirtualAddress == 0 || boundImportDirectory.Size == 0)
        {
            outputStream << "无绑定导入表。\n";
            return;
        }

        std::uint32_t boundOffset = 0;
        if (!rvaToFileOffset(boundImportDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &boundOffset))
        {
            outputStream << "绑定导入表 RVA 无法映射到文件偏移。\n";
            return;
        }

        const std::uint32_t baseOffset = boundOffset;
        int moduleIndex = 0;
        while (true)
        {
            IMAGE_BOUND_IMPORT_DESCRIPTOR descriptor{};
            if (!readPodAtOffset(fileBytes, boundOffset, &descriptor))
            {
                outputStream << "绑定导入描述符读取失败。\n";
                return;
            }

            if (descriptor.TimeDateStamp == 0 && descriptor.OffsetModuleName == 0 && descriptor.NumberOfModuleForwarderRefs == 0)
            {
                if (moduleIndex == 0)
                {
                    outputStream << "绑定导入表为空。\n";
                }
                break;
            }

            const qsizetype moduleNameOffset =
                static_cast<qsizetype>(baseOffset) + static_cast<qsizetype>(descriptor.OffsetModuleName);
            const QString moduleNameText = readNullTerminatedAsciiAtOffset(fileBytes, moduleNameOffset);
            outputStream << QStringLiteral("模块[%1]: %2 TimeDateStamp=0x%3 ForwarderRefs=%4\n")
                .arg(moduleIndex)
                .arg(moduleNameText.isEmpty() ? QStringLiteral("<模块名解析失败>") : moduleNameText)
                .arg(QString::number(descriptor.TimeDateStamp, 16).toUpper())
                .arg(descriptor.NumberOfModuleForwarderRefs);

            boundOffset += sizeof(IMAGE_BOUND_IMPORT_DESCRIPTOR);
            boundOffset += static_cast<std::uint32_t>(descriptor.NumberOfModuleForwarderRefs)
                * static_cast<std::uint32_t>(sizeof(IMAGE_BOUND_FORWARDER_REF));
            ++moduleIndex;
        }
    }

    // dumpLoadConfigDirectory：
    // - 输出 Load Config 摘要。
    void dumpLoadConfigDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        const bool isPe64,
        const std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& loadConfigDirectory)
    {
        outputStream << "\n[Load Config]\n";
        if (loadConfigDirectory.VirtualAddress == 0 || loadConfigDirectory.Size == 0)
        {
            outputStream << "无 Load Config。\n";
            return;
        }

        std::uint32_t loadConfigOffset = 0;
        if (!rvaToFileOffset(loadConfigDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &loadConfigOffset))
        {
            outputStream << "Load Config RVA 无法映射到文件偏移。\n";
            return;
        }

        if (isPe64)
        {
            IMAGE_LOAD_CONFIG_DIRECTORY64 loadConfig{};
            if (!readPodAtOffset(fileBytes, loadConfigOffset, &loadConfig))
            {
                outputStream << "Load Config64 读取失败。\n";
                return;
            }

            outputStream << QStringLiteral("Size: 0x%1\n").arg(QString::number(loadConfig.Size, 16).toUpper());
            outputStream << QStringLiteral("TimeDateStamp: 0x%1\n").arg(QString::number(loadConfig.TimeDateStamp, 16).toUpper());
            outputStream << QStringLiteral("GuardFlags: 0x%1\n").arg(QString::number(loadConfig.GuardFlags, 16).toUpper());
            outputStream << QStringLiteral("GuardCFCheckFunctionPointer: 0x%1\n")
                .arg(QString::number(static_cast<qulonglong>(loadConfig.GuardCFCheckFunctionPointer), 16).toUpper());
            outputStream << QStringLiteral("GuardCFFunctionCount: %1\n")
                .arg(static_cast<qulonglong>(loadConfig.GuardCFFunctionCount));
        }
        else
        {
            IMAGE_LOAD_CONFIG_DIRECTORY32 loadConfig{};
            if (!readPodAtOffset(fileBytes, loadConfigOffset, &loadConfig))
            {
                outputStream << "Load Config32 读取失败。\n";
                return;
            }

            outputStream << QStringLiteral("Size: 0x%1\n").arg(QString::number(loadConfig.Size, 16).toUpper());
            outputStream << QStringLiteral("TimeDateStamp: 0x%1\n").arg(QString::number(loadConfig.TimeDateStamp, 16).toUpper());
            outputStream << QStringLiteral("GuardFlags: 0x%1\n").arg(QString::number(loadConfig.GuardFlags, 16).toUpper());
            outputStream << QStringLiteral("GuardCFCheckFunctionPointer: 0x%1\n")
                .arg(QString::number(loadConfig.GuardCFCheckFunctionPointer, 16).toUpper());
            outputStream << QStringLiteral("GuardCFFunctionCount: %1\n")
                .arg(loadConfig.GuardCFFunctionCount);
        }
    }

    // dumpClrDirectory：
    // - 输出 CLR/.NET 头摘要。
    void dumpClrDirectory(
        QTextStream& outputStream,
        const QByteArray& fileBytes,
        const std::uint32_t sizeOfHeadersValue,
        const std::vector<IMAGE_SECTION_HEADER>& sectionList,
        const IMAGE_DATA_DIRECTORY& clrDirectory)
    {
        outputStream << "\n[CLR/.NET头]\n";
        if (clrDirectory.VirtualAddress == 0 || clrDirectory.Size == 0)
        {
            outputStream << "无 CLR 头。\n";
            return;
        }

        std::uint32_t clrOffset = 0;
        if (!rvaToFileOffset(clrDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &clrOffset))
        {
            outputStream << "CLR 头 RVA 无法映射到文件偏移。\n";
            return;
        }

        IMAGE_COR20_HEADER clrHeader{};
        if (!readPodAtOffset(fileBytes, clrOffset, &clrHeader))
        {
            outputStream << "CLR 头读取失败。\n";
            return;
        }

        outputStream << QStringLiteral("Cb: 0x%1\n").arg(QString::number(clrHeader.cb, 16).toUpper());
        outputStream << QStringLiteral("RuntimeVersion: %1.%2\n")
            .arg(clrHeader.MajorRuntimeVersion)
            .arg(clrHeader.MinorRuntimeVersion);
        outputStream << QStringLiteral("Flags: 0x%1\n")
            .arg(QString::number(clrHeader.Flags, 16).toUpper());
        outputStream << QStringLiteral("EntryPointToken/RVA: 0x%1\n")
            .arg(QString::number(clrHeader.EntryPointToken, 16).toUpper());
        outputStream << QStringLiteral("MetaData RVA=0x%1 Size=0x%2\n")
            .arg(QString::number(clrHeader.MetaData.VirtualAddress, 16).toUpper())
            .arg(QString::number(clrHeader.MetaData.Size, 16).toUpper());
    }

    // dumpSecurityDirectory：
    // - 输出证书安全目录摘要。
    void dumpSecurityDirectory(
        QTextStream& outputStream,
        const IMAGE_DATA_DIRECTORY& securityDirectory)
    {
        outputStream << "\n[安全目录/证书]\n";
        if (securityDirectory.VirtualAddress == 0 || securityDirectory.Size == 0)
        {
            outputStream << "无安全目录。\n";
            return;
        }

        outputStream << QStringLiteral("SecurityOffset(FileOffset)=0x%1\n")
            .arg(QString::number(securityDirectory.VirtualAddress, 16).toUpper());
        outputStream << QStringLiteral("SecuritySize=0x%1\n")
            .arg(QString::number(securityDirectory.Size, 16).toUpper());
        outputStream << QStringLiteral("提示：该目录使用文件偏移，不参与内存映像 RVA 映射。\n");
    }
}
