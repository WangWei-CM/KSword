#include "FilePropertyPeAnalyzer.h"
#include "FilePropertyPeAnalyzer.Internal.h"

// ============================================================
// FilePropertyPeAnalyzer.cpp
// 作用：
// 1) 解析 PE 头基础字段；
// 2) 解析导入表与导出表；
// 3) 生成属性窗口可直接显示的纯文本报告。
// ============================================================

#include <QDateTime>
#include <QFile>
#include <QTextStream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>     // std::array：数据目录名称表。
#include <algorithm> // std::max：RVA 映射时选择更大区段跨度。
#include <cstdint>   // std::uint*_t：PE 头固定宽度字段。
#include <cstring>   // std::memcpy：安全读取结构体。
#include <vector>    // std::vector：区段表缓存。

namespace file_dock_detail
{
    using namespace pe_tables_detail;

    namespace
    {
        // kMaxImportPerModule 作用：
        // - 限制单个模块最多展示的导入函数数量；
        // - 避免异常文件造成无限遍历或输出爆炸。
        constexpr int kMaxImportPerModule = 2048;

        // kMaxExportEntries 作用：
        // - 限制导出表最大展示条目数；
        // - 防止畸形样本把属性窗口撑爆。
        constexpr int kMaxExportEntries = 4096;

        // kMaxSectionCount 作用：
        // - 对区段数量做保守上限保护；
        // - 正常 PE 不会接近该值。
        constexpr int kMaxSectionCount = 128;
        constexpr int kMaxTlsCallbackCount = 64;

        // DelayImportDescriptor：
        // - 作用：描述延迟导入表一条记录；
        // - 按 PE 规范使用 32 位 RVA 字段。
        struct DelayImportDescriptor
        {
            std::uint32_t attributes = 0;
            std::uint32_t nameRva = 0;
            std::uint32_t moduleHandleRva = 0;
            std::uint32_t iatRva = 0;
            std::uint32_t intRva = 0;
            std::uint32_t boundIatRva = 0;
            std::uint32_t unloadIatRva = 0;
            std::uint32_t timeStamp = 0;
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

        // machineToText 作用：
        // - 把 Machine 字段转换为常见体系结构文本。
        QString machineToText(const std::uint16_t machineValue)
        {
            switch (machineValue)
            {
            case IMAGE_FILE_MACHINE_I386:
                return QStringLiteral("x86");
            case IMAGE_FILE_MACHINE_AMD64:
                return QStringLiteral("x64");
            case IMAGE_FILE_MACHINE_ARM64:
                return QStringLiteral("ARM64");
            case IMAGE_FILE_MACHINE_ARM:
                return QStringLiteral("ARM");
            default:
                return QStringLiteral("Unknown");
            }
        }

        // subsystemToText 作用：
        // - 把 Subsystem 字段转换为可读文本。
        QString subsystemToText(const std::uint16_t subsystemValue)
        {
            switch (subsystemValue)
            {
            case IMAGE_SUBSYSTEM_NATIVE:
                return QStringLiteral("Native");
            case IMAGE_SUBSYSTEM_WINDOWS_GUI:
                return QStringLiteral("Windows GUI");
            case IMAGE_SUBSYSTEM_WINDOWS_CUI:
                return QStringLiteral("Windows CUI");
            case IMAGE_SUBSYSTEM_POSIX_CUI:
                return QStringLiteral("POSIX CUI");
            case IMAGE_SUBSYSTEM_WINDOWS_CE_GUI:
                return QStringLiteral("Windows CE GUI");
            case IMAGE_SUBSYSTEM_EFI_APPLICATION:
                return QStringLiteral("EFI Application");
            case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
                return QStringLiteral("EFI Boot Service Driver");
            case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
                return QStringLiteral("EFI Runtime Driver");
            case IMAGE_SUBSYSTEM_XBOX:
                return QStringLiteral("XBOX");
            case IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION:
                return QStringLiteral("Windows Boot Application");
            default:
                return QStringLiteral("Unknown");
            }
        }

        // fileCharacteristicsToText 作用：
        // - 把 IMAGE_FILE_HEADER.Characteristics 常见位转换为说明文本。
        QString fileCharacteristicsToText(const std::uint16_t characteristicsValue)
        {
            QStringList itemList;
            if ((characteristicsValue & IMAGE_FILE_EXECUTABLE_IMAGE) != 0)
            {
                itemList.push_back(QStringLiteral("Executable"));
            }
            if ((characteristicsValue & IMAGE_FILE_DLL) != 0)
            {
                itemList.push_back(QStringLiteral("DLL"));
            }
            if ((characteristicsValue & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0)
            {
                itemList.push_back(QStringLiteral("LargeAddressAware"));
            }
            if ((characteristicsValue & IMAGE_FILE_32BIT_MACHINE) != 0)
            {
                itemList.push_back(QStringLiteral("Machine32Bit"));
            }
            if ((characteristicsValue & IMAGE_FILE_SYSTEM) != 0)
            {
                itemList.push_back(QStringLiteral("System"));
            }
            if ((characteristicsValue & IMAGE_FILE_RELOCS_STRIPPED) != 0)
            {
                itemList.push_back(QStringLiteral("RelocsStripped"));
            }
            return itemList.isEmpty() ? QStringLiteral("<无常见标记>") : itemList.join(QStringLiteral(" | "));
        }

        // sectionCharacteristicsToText 作用：
        // - 输出区段常见属性位说明。
        QString sectionCharacteristicsToText(const std::uint32_t characteristicsValue)
        {
            QStringList itemList;
            if ((characteristicsValue & IMAGE_SCN_CNT_CODE) != 0)
            {
                itemList.push_back(QStringLiteral("CODE"));
            }
            if ((characteristicsValue & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0)
            {
                itemList.push_back(QStringLiteral("INIT_DATA"));
            }
            if ((characteristicsValue & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0)
            {
                itemList.push_back(QStringLiteral("BSS"));
            }
            if ((characteristicsValue & IMAGE_SCN_MEM_EXECUTE) != 0)
            {
                itemList.push_back(QStringLiteral("EXECUTE"));
            }
            if ((characteristicsValue & IMAGE_SCN_MEM_READ) != 0)
            {
                itemList.push_back(QStringLiteral("READ"));
            }
            if ((characteristicsValue & IMAGE_SCN_MEM_WRITE) != 0)
            {
                itemList.push_back(QStringLiteral("WRITE"));
            }
            return itemList.isEmpty() ? QStringLiteral("<无常见属性>") : itemList.join(QStringLiteral(" | "));
        }


        // safeAsciiText 作用：
        // - 从固定长度字节数组中提取 0 结尾 ASCII 文本。
        QString safeAsciiText(const char* textPointer, const int maxLength)
        {
            if (textPointer == nullptr || maxLength <= 0)
            {
                return QString();
            }

            int actualLength = 0;
            while (actualLength < maxLength && textPointer[actualLength] != '\0')
            {
                ++actualLength;
            }
            return QString::fromLatin1(textPointer, actualLength);
        }

        // readNullTerminatedAsciiAtOffset 作用：
        // - 从文件原始字节按偏移读取 ANSI 字符串；
        // - 最长读取到缓冲区末尾或遇到 NUL。
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

        // dumpImportTable 作用：
        // - 解析并输出导入表；
        // - 兼容 PE32 / PE32+ 的 thunk 宽度。
        void dumpImportTable(
            QTextStream& outputStream,
            const QByteArray& fileBytes,
            const bool isPe64,
            const std::uint32_t sizeOfHeadersValue,
            const std::vector<IMAGE_SECTION_HEADER>& sectionList,
            const IMAGE_DATA_DIRECTORY& importDirectory)
        {
            outputStream << "[导入表]\n";
            if (importDirectory.VirtualAddress == 0 || importDirectory.Size == 0)
            {
                outputStream << "无导入表。\n";
                return;
            }

            std::uint32_t importDescriptorOffset = 0;
            if (!rvaToFileOffset(
                importDirectory.VirtualAddress,
                sizeOfHeadersValue,
                sectionList,
                &importDescriptorOffset))
            {
                outputStream << "导入表 RVA 无法映射到文件偏移。\n";
                return;
            }

            int moduleIndex = 0;
            while (true)
            {
                IMAGE_IMPORT_DESCRIPTOR importDescriptor{};
                if (!readPodAtOffset(fileBytes, importDescriptorOffset, &importDescriptor))
                {
                    outputStream << "导入描述符读取失败。\n";
                    return;
                }

                if (importDescriptor.OriginalFirstThunk == 0
                    && importDescriptor.FirstThunk == 0
                    && importDescriptor.Name == 0)
                {
                    if (moduleIndex == 0)
                    {
                        outputStream << "导入表为空。\n";
                    }
                    break;
                }

                std::uint32_t moduleNameOffset = 0;
                const bool moduleNameOk = rvaToFileOffset(
                    importDescriptor.Name,
                    sizeOfHeadersValue,
                    sectionList,
                    &moduleNameOffset);
                const QString moduleNameText = moduleNameOk
                    ? readNullTerminatedAsciiAtOffset(fileBytes, moduleNameOffset)
                    : QStringLiteral("<模块名解析失败>");

                outputStream
                    << QStringLiteral("模块[%1]: %2\n")
                        .arg(moduleIndex)
                        .arg(moduleNameText);

                const std::uint32_t thunkRva = (importDescriptor.OriginalFirstThunk != 0)
                    ? importDescriptor.OriginalFirstThunk
                    : importDescriptor.FirstThunk;
                std::uint32_t thunkOffset = 0;
                if (!rvaToFileOffset(thunkRva, sizeOfHeadersValue, sectionList, &thunkOffset))
                {
                    outputStream << "  Thunk RVA 无法映射。\n";
                    importDescriptorOffset += sizeof(IMAGE_IMPORT_DESCRIPTOR);
                    ++moduleIndex;
                    continue;
                }

                int importIndex = 0;
                while (importIndex < kMaxImportPerModule)
                {
                    std::uint64_t thunkData = 0;
                    if (isPe64)
                    {
                        std::uint64_t thunk64 = 0;
                        if (!readPodAtOffset(fileBytes, thunkOffset, &thunk64))
                        {
                            outputStream << "  读取 64 位 thunk 失败。\n";
                            break;
                        }
                        thunkData = thunk64;
                        thunkOffset += sizeof(std::uint64_t);
                        if (thunk64 == 0)
                        {
                            break;
                        }

                        if ((thunk64 & IMAGE_ORDINAL_FLAG64) != 0)
                        {
                            outputStream
                                << QStringLiteral("  [%1] Ordinal #%2\n")
                                    .arg(importIndex)
                                    .arg(static_cast<qulonglong>(thunk64 & 0xFFFFULL));
                            ++importIndex;
                            continue;
                        }
                    }
                    else
                    {
                        std::uint32_t thunk32 = 0;
                        if (!readPodAtOffset(fileBytes, thunkOffset, &thunk32))
                        {
                            outputStream << "  读取 32 位 thunk 失败。\n";
                            break;
                        }
                        thunkData = thunk32;
                        thunkOffset += sizeof(std::uint32_t);
                        if (thunk32 == 0)
                        {
                            break;
                        }

                        if ((thunk32 & IMAGE_ORDINAL_FLAG32) != 0)
                        {
                            outputStream
                                << QStringLiteral("  [%1] Ordinal #%2\n")
                                    .arg(importIndex)
                                    .arg(static_cast<qulonglong>(thunk32 & 0xFFFFU));
                            ++importIndex;
                            continue;
                        }
                    }

                    std::uint32_t importByNameOffset = 0;
                    if (!rvaToFileOffset(
                        static_cast<std::uint32_t>(thunkData),
                        sizeOfHeadersValue,
                        sectionList,
                        &importByNameOffset))
                    {
                        outputStream
                            << QStringLiteral("  [%1] 名称 RVA 无法映射。\n")
                                .arg(importIndex);
                        ++importIndex;
                        continue;
                    }

                    std::uint16_t hintValue = 0;
                    readPodAtOffset(fileBytes, importByNameOffset, &hintValue);
                    const QString functionNameText = readNullTerminatedAsciiAtOffset(
                        fileBytes,
                        static_cast<qsizetype>(importByNameOffset) + 2);
                    outputStream
                        << QStringLiteral("  [%1] %2 (Hint=%3)\n")
                            .arg(importIndex)
                            .arg(functionNameText.isEmpty() ? QStringLiteral("<空名称>") : functionNameText)
                            .arg(hintValue);
                    ++importIndex;
                }

                if (importIndex >= kMaxImportPerModule)
                {
                    outputStream
                        << QStringLiteral("  ... 单模块导入项超过 %1，后续已省略\n")
                            .arg(kMaxImportPerModule);
                }

                importDescriptorOffset += sizeof(IMAGE_IMPORT_DESCRIPTOR);
                ++moduleIndex;
            }
        }

        // dumpExportTable 作用：
        // - 解析并输出导出表。
        void dumpExportTable(
            QTextStream& outputStream,
            const QByteArray& fileBytes,
            const std::uint32_t sizeOfHeadersValue,
            const std::vector<IMAGE_SECTION_HEADER>& sectionList,
            const IMAGE_DATA_DIRECTORY& exportDirectory)
        {
            outputStream << "\n[导出表]\n";
            if (exportDirectory.VirtualAddress == 0 || exportDirectory.Size == 0)
            {
                outputStream << "无导出表。\n";
                return;
            }

            std::uint32_t exportDirectoryOffset = 0;
            if (!rvaToFileOffset(
                exportDirectory.VirtualAddress,
                sizeOfHeadersValue,
                sectionList,
                &exportDirectoryOffset))
            {
                outputStream << "导出表 RVA 无法映射到文件偏移。\n";
                return;
            }

            IMAGE_EXPORT_DIRECTORY exportDirectoryHeader{};
            if (!readPodAtOffset(fileBytes, exportDirectoryOffset, &exportDirectoryHeader))
            {
                outputStream << "导出表头读取失败。\n";
                return;
            }

            std::uint32_t dllNameOffset = 0;
            QString dllNameText = QStringLiteral("<DLL名称解析失败>");
            if (rvaToFileOffset(
                exportDirectoryHeader.Name,
                sizeOfHeadersValue,
                sectionList,
                &dllNameOffset))
            {
                dllNameText = readNullTerminatedAsciiAtOffset(fileBytes, dllNameOffset);
            }

            outputStream
                << QStringLiteral("DLL名称: %1\n").arg(dllNameText)
                << QStringLiteral("OrdinalBase: %1\n").arg(exportDirectoryHeader.Base)
                << QStringLiteral("函数数量: %1\n").arg(exportDirectoryHeader.NumberOfFunctions)
                << QStringLiteral("命名导出数量: %1\n").arg(exportDirectoryHeader.NumberOfNames);

            std::uint32_t addressArrayOffset = 0;
            std::uint32_t nameArrayOffset = 0;
            std::uint32_t ordinalArrayOffset = 0;
            if (!rvaToFileOffset(exportDirectoryHeader.AddressOfFunctions, sizeOfHeadersValue, sectionList, &addressArrayOffset)
                || !rvaToFileOffset(exportDirectoryHeader.AddressOfNames, sizeOfHeadersValue, sectionList, &nameArrayOffset)
                || !rvaToFileOffset(exportDirectoryHeader.AddressOfNameOrdinals, sizeOfHeadersValue, sectionList, &ordinalArrayOffset))
            {
                outputStream << "导出表数组 RVA 映射失败。\n";
                return;
            }

            const std::uint32_t exportDirectoryStartRva = exportDirectory.VirtualAddress;
            const std::uint32_t exportDirectoryEndRva = exportDirectory.VirtualAddress + exportDirectory.Size;
            const std::uint32_t exportCount = std::min<std::uint32_t>(
                exportDirectoryHeader.NumberOfNames,
                static_cast<std::uint32_t>(kMaxExportEntries));

            for (std::uint32_t exportIndex = 0; exportIndex < exportCount; ++exportIndex)
            {
                std::uint32_t nameRva = 0;
                std::uint16_t ordinalIndex = 0;
                if (!readPodAtOffset(fileBytes, nameArrayOffset + exportIndex * sizeof(std::uint32_t), &nameRva)
                    || !readPodAtOffset(fileBytes, ordinalArrayOffset + exportIndex * sizeof(std::uint16_t), &ordinalIndex))
                {
                    outputStream << "导出项读取失败。\n";
                    break;
                }

                std::uint32_t nameOffset = 0;
                QString exportNameText = QStringLiteral("<名称解析失败>");
                if (rvaToFileOffset(nameRva, sizeOfHeadersValue, sectionList, &nameOffset))
                {
                    exportNameText = readNullTerminatedAsciiAtOffset(fileBytes, nameOffset);
                }

                std::uint32_t functionRva = 0;
                if (!readPodAtOffset(fileBytes, addressArrayOffset + ordinalIndex * sizeof(std::uint32_t), &functionRva))
                {
                    outputStream << "导出函数 RVA 读取失败。\n";
                    break;
                }

                const std::uint32_t exportOrdinal = exportDirectoryHeader.Base + ordinalIndex;
                outputStream
                    << QStringLiteral("  [%1] Ordinal=%2 RVA=0x%3 Name=%4")
                        .arg(exportIndex)
                        .arg(exportOrdinal)
                        .arg(QString::number(functionRva, 16).toUpper())
                        .arg(exportNameText);

                if (functionRva >= exportDirectoryStartRva && functionRva < exportDirectoryEndRva)
                {
                    std::uint32_t forwarderOffset = 0;
                    if (rvaToFileOffset(functionRva, sizeOfHeadersValue, sectionList, &forwarderOffset))
                    {
                        const QString forwarderText = readNullTerminatedAsciiAtOffset(fileBytes, forwarderOffset);
                        outputStream << QStringLiteral(" -> Forwarder=%1").arg(forwarderText);
                    }
                }
                outputStream << '\n';
            }

            if (exportDirectoryHeader.NumberOfNames > exportCount)
            {
                outputStream
                    << QStringLiteral("  ... 导出项超过 %1，后续已省略\n")
                        .arg(kMaxExportEntries);
            }
        }

    }

    QString buildPeAnalysisText(const QString& filePath)
    {
        QFile fileObject(filePath);
        if (!fileObject.open(QIODevice::ReadOnly))
        {
            return QStringLiteral("无法读取文件，无法解析PE信息。");
        }

        const QByteArray fileBytes = fileObject.readAll();
        fileObject.close();
        if (fileBytes.size() < static_cast<qsizetype>(sizeof(IMAGE_DOS_HEADER)))
        {
            return QStringLiteral("文件过小，无法识别PE。");
        }

        IMAGE_DOS_HEADER dosHeader{};
        if (!readPodAtOffset(fileBytes, 0, &dosHeader) || dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        {
            return QStringLiteral("非PE文件（缺少 MZ 标记）。");
        }

        if (dosHeader.e_lfanew <= 0)
        {
            return QStringLiteral("PE 偏移无效。");
        }

        std::uint32_t peSignature = 0;
        if (!readPodAtOffset(fileBytes, dosHeader.e_lfanew, &peSignature)
            || peSignature != IMAGE_NT_SIGNATURE)
        {
            return QStringLiteral("PE 签名无效（缺少 PE\\0\\0 标记）。");
        }

        IMAGE_FILE_HEADER fileHeader{};
        if (!readPodAtOffset(fileBytes, dosHeader.e_lfanew + 4, &fileHeader))
        {
            return QStringLiteral("PE 文件头读取失败。");
        }

        const qsizetype optionalHeaderOffset =
            dosHeader.e_lfanew + 4 + static_cast<qsizetype>(sizeof(IMAGE_FILE_HEADER));
        std::uint16_t optionalMagic = 0;
        if (!readPodAtOffset(fileBytes, optionalHeaderOffset, &optionalMagic))
        {
            return QStringLiteral("PE OptionalHeader 读取失败。");
        }

        const bool isPe32Plus = (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
        const bool isPe32 = (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC);
        if (!isPe32 && !isPe32Plus)
        {
            return QStringLiteral("未知 PE OptionalHeader Magic: 0x%1")
                .arg(QString::number(optionalMagic, 16).toUpper());
        }

        IMAGE_OPTIONAL_HEADER32 optionalHeader32{};
        IMAGE_OPTIONAL_HEADER64 optionalHeader64{};
        std::uint32_t sizeOfHeadersValue = 0;
        std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> dataDirectoryList{};
        IMAGE_DATA_DIRECTORY exportDirectory{};
        IMAGE_DATA_DIRECTORY importDirectory{};
        IMAGE_DATA_DIRECTORY resourceDirectory{};
        IMAGE_DATA_DIRECTORY securityDirectory{};
        IMAGE_DATA_DIRECTORY relocDirectory{};
        IMAGE_DATA_DIRECTORY debugDirectory{};
        IMAGE_DATA_DIRECTORY tlsDirectory{};
        IMAGE_DATA_DIRECTORY boundImportDirectory{};
        IMAGE_DATA_DIRECTORY delayImportDirectory{};
        IMAGE_DATA_DIRECTORY loadConfigDirectory{};
        IMAGE_DATA_DIRECTORY clrDirectory{};
        std::uint64_t imageBaseValue = 0;
        std::uint32_t addressOfEntryPoint = 0;
        std::uint16_t subsystemValue = 0;
        std::uint32_t sizeOfImageValue = 0;
        std::uint32_t sizeOfHeadersOutValue = 0;
        std::uint32_t sectionAlignmentValue = 0;
        std::uint32_t fileAlignmentValue = 0;
        std::uint32_t checksumValue = 0;

        if (isPe32Plus)
        {
            if (!readPodAtOffset(fileBytes, optionalHeaderOffset, &optionalHeader64))
            {
                return QStringLiteral("PE32+ OptionalHeader 读取失败。");
            }
            sizeOfHeadersValue = optionalHeader64.SizeOfHeaders;
            for (int directoryIndex = 0; directoryIndex < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++directoryIndex)
            {
                dataDirectoryList[static_cast<std::size_t>(directoryIndex)] = optionalHeader64.DataDirectory[directoryIndex];
            }
            exportDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            importDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            resourceDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
            securityDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
            relocDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            debugDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            tlsDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            boundImportDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT];
            delayImportDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            loadConfigDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
            clrDirectory = optionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
            imageBaseValue = optionalHeader64.ImageBase;
            addressOfEntryPoint = optionalHeader64.AddressOfEntryPoint;
            subsystemValue = optionalHeader64.Subsystem;
            sizeOfImageValue = optionalHeader64.SizeOfImage;
            sizeOfHeadersOutValue = optionalHeader64.SizeOfHeaders;
            sectionAlignmentValue = optionalHeader64.SectionAlignment;
            fileAlignmentValue = optionalHeader64.FileAlignment;
            checksumValue = optionalHeader64.CheckSum;
        }
        else
        {
            if (!readPodAtOffset(fileBytes, optionalHeaderOffset, &optionalHeader32))
            {
                return QStringLiteral("PE32 OptionalHeader 读取失败。");
            }
            sizeOfHeadersValue = optionalHeader32.SizeOfHeaders;
            for (int directoryIndex = 0; directoryIndex < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++directoryIndex)
            {
                dataDirectoryList[static_cast<std::size_t>(directoryIndex)] = optionalHeader32.DataDirectory[directoryIndex];
            }
            exportDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            importDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            resourceDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
            securityDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
            relocDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            debugDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            tlsDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            boundImportDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT];
            delayImportDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            loadConfigDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
            clrDirectory = optionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
            imageBaseValue = optionalHeader32.ImageBase;
            addressOfEntryPoint = optionalHeader32.AddressOfEntryPoint;
            subsystemValue = optionalHeader32.Subsystem;
            sizeOfImageValue = optionalHeader32.SizeOfImage;
            sizeOfHeadersOutValue = optionalHeader32.SizeOfHeaders;
            sectionAlignmentValue = optionalHeader32.SectionAlignment;
            fileAlignmentValue = optionalHeader32.FileAlignment;
            checksumValue = optionalHeader32.CheckSum;
        }

        const qsizetype sectionTableOffset =
            optionalHeaderOffset + static_cast<qsizetype>(fileHeader.SizeOfOptionalHeader);
        if (fileHeader.NumberOfSections > kMaxSectionCount)
        {
            return QStringLiteral("区段数量异常：%1").arg(fileHeader.NumberOfSections);
        }

        std::vector<IMAGE_SECTION_HEADER> sectionList;
        sectionList.reserve(fileHeader.NumberOfSections);
        for (std::uint16_t sectionIndex = 0; sectionIndex < fileHeader.NumberOfSections; ++sectionIndex)
        {
            IMAGE_SECTION_HEADER sectionHeader{};
            const qsizetype currentOffset =
                sectionTableOffset + static_cast<qsizetype>(sectionIndex) * static_cast<qsizetype>(sizeof(IMAGE_SECTION_HEADER));
            if (!readPodAtOffset(fileBytes, currentOffset, &sectionHeader))
            {
                return QStringLiteral("区段表读取失败，索引=%1").arg(sectionIndex);
            }
            sectionList.push_back(sectionHeader);
        }

        QString outputText;
        QTextStream outputStream(&outputText);
        outputStream.setIntegerBase(10);
        outputStream.setNumberFlags(QTextStream::ShowBase);

        outputStream << "[PE头]\n";
        outputStream << QStringLiteral("文件格式: PE%1\n").arg(isPe32Plus ? QStringLiteral("32+") : QStringLiteral("32"));
        outputStream << QStringLiteral("e_lfanew: 0x%1\n").arg(QString::number(dosHeader.e_lfanew, 16).toUpper());
        outputStream << QStringLiteral("Machine: 0x%1 (%2)\n")
            .arg(QString::number(fileHeader.Machine, 16).toUpper())
            .arg(machineToText(fileHeader.Machine));
        outputStream << QStringLiteral("Section数量: %1\n").arg(fileHeader.NumberOfSections);
        outputStream << QStringLiteral("TimeDateStamp: 0x%1 (%2)\n")
            .arg(QString::number(fileHeader.TimeDateStamp, 16).toUpper())
            .arg(QDateTime::fromSecsSinceEpoch(fileHeader.TimeDateStamp).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        outputStream << QStringLiteral("Characteristics: 0x%1 (%2)\n")
            .arg(QString::number(fileHeader.Characteristics, 16).toUpper())
            .arg(fileCharacteristicsToText(fileHeader.Characteristics));
        outputStream << QStringLiteral("EntryPoint RVA: 0x%1\n")
            .arg(QString::number(addressOfEntryPoint, 16).toUpper());
        outputStream << QStringLiteral("ImageBase: 0x%1\n")
            .arg(QString::number(static_cast<qulonglong>(imageBaseValue), 16).toUpper());
        outputStream << QStringLiteral("Subsystem: 0x%1 (%2)\n")
            .arg(QString::number(subsystemValue, 16).toUpper())
            .arg(subsystemToText(subsystemValue));
        outputStream << QStringLiteral("SectionAlignment: 0x%1\n")
            .arg(QString::number(sectionAlignmentValue, 16).toUpper());
        outputStream << QStringLiteral("FileAlignment: 0x%1\n")
            .arg(QString::number(fileAlignmentValue, 16).toUpper());
        outputStream << QStringLiteral("SizeOfImage: 0x%1\n")
            .arg(QString::number(sizeOfImageValue, 16).toUpper());
        outputStream << QStringLiteral("SizeOfHeaders: 0x%1\n")
            .arg(QString::number(sizeOfHeadersOutValue, 16).toUpper());
        outputStream << QStringLiteral("CheckSum: 0x%1\n")
            .arg(QString::number(checksumValue, 16).toUpper());

        outputStream << "\n[区段表]\n";
        for (int sectionIndex = 0; sectionIndex < static_cast<int>(sectionList.size()); ++sectionIndex)
        {
            const IMAGE_SECTION_HEADER& sectionHeader = sectionList[static_cast<std::size_t>(sectionIndex)];
            outputStream
                << QStringLiteral("[%1] %2\n")
                    .arg(sectionIndex)
                    .arg(safeAsciiText(reinterpret_cast<const char*>(sectionHeader.Name), IMAGE_SIZEOF_SHORT_NAME))
                << QStringLiteral("  VirtualAddress: 0x%1\n")
                    .arg(QString::number(sectionHeader.VirtualAddress, 16).toUpper())
                << QStringLiteral("  VirtualSize: 0x%1\n")
                    .arg(QString::number(sectionHeader.Misc.VirtualSize, 16).toUpper())
                << QStringLiteral("  PointerToRawData: 0x%1\n")
                    .arg(QString::number(sectionHeader.PointerToRawData, 16).toUpper())
                << QStringLiteral("  SizeOfRawData: 0x%1\n")
                    .arg(QString::number(sectionHeader.SizeOfRawData, 16).toUpper())
                << QStringLiteral("  Entropy: %1\n")
                    .arg(calculateSectionEntropy(
                        fileBytes,
                        sectionHeader.PointerToRawData,
                        sectionHeader.SizeOfRawData), 0, 'f', 4)
                << QStringLiteral("  Characteristics: 0x%1 (%2)\n")
                    .arg(QString::number(sectionHeader.Characteristics, 16).toUpper())
                    .arg(sectionCharacteristicsToText(sectionHeader.Characteristics));
        }

        dumpDataDirectories(outputStream, dataDirectoryList);
        dumpImportTable(
            outputStream,
            fileBytes,
            isPe32Plus,
            sizeOfHeadersValue,
            sectionList,
            importDirectory);
        dumpDelayImportTable(
            outputStream,
            fileBytes,
            isPe32Plus,
            sizeOfHeadersValue,
            sectionList,
            delayImportDirectory);
        dumpExportTable(
            outputStream,
            fileBytes,
            sizeOfHeadersValue,
            sectionList,
            exportDirectory);
        dumpTlsDirectory(
            outputStream,
            fileBytes,
            isPe32Plus,
            imageBaseValue,
            sizeOfHeadersValue,
            sectionList,
            tlsDirectory);
        dumpResourceDirectory(
            outputStream,
            fileBytes,
            sizeOfHeadersValue,
            sectionList,
            resourceDirectory);
        dumpBaseRelocDirectory(
            outputStream,
            fileBytes,
            sizeOfHeadersValue,
            sectionList,
            relocDirectory);
        dumpDebugDirectory(
            outputStream,
            fileBytes,
            sizeOfHeadersValue,
            sectionList,
            debugDirectory);
        dumpBoundImportDirectory(
            outputStream,
            fileBytes,
            sizeOfHeadersValue,
            sectionList,
            boundImportDirectory);
        dumpLoadConfigDirectory(
            outputStream,
            fileBytes,
            isPe32Plus,
            sizeOfHeadersValue,
            sectionList,
            loadConfigDirectory);
        dumpClrDirectory(
            outputStream,
            fileBytes,
            sizeOfHeadersValue,
            sectionList,
            clrDirectory);
        dumpSecurityDirectory(
            outputStream,
            securityDirectory);

        return outputText;
    }
}
