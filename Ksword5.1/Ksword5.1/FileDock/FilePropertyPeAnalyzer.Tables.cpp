#include "FilePropertyPeAnalyzer.h"
#include "FilePropertyPeAnalyzer.Internal.h"

// ============================================================
// FilePropertyPeAnalyzer.Tables.cpp
// 作用：
// 1) 承载 PE 扩展表项解析辅助函数；
// 2) 预留给延迟导入/TLS/资源/重定位等高级结构解析；
// 3) 把 FilePropertyPeAnalyzer.cpp 的超长实现拆开，避免单文件继续膨胀。
// ============================================================

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextStream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace file_dock_detail
{
    namespace pe_tables_detail
    {
        // kMaxImportPerModule 作用：
        // - 限制单个模块最多展示的导入函数数量；
        // - 避免异常文件导致输出爆炸。
        constexpr int kMaxImportPerModule = 2048;

        // kMaxTlsCallbackCount 作用：
        // - 限制 TLS 回调展示数量；
        // - 防止畸形样本导致无限遍历。
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

        // dataDirectoryName 作用：
        // - 返回指定数据目录索引对应的标准名称。
        QString dataDirectoryName(const int directoryIndex)
        {
            static const std::array<const char*, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> directoryNameList{ {
                "EXPORT", "IMPORT", "RESOURCE", "EXCEPTION",
                "SECURITY", "BASERELOC", "DEBUG", "ARCHITECTURE",
                "GLOBALPTR", "TLS", "LOAD_CONFIG", "BOUND_IMPORT",
                "IAT", "DELAY_IMPORT", "COM_DESCRIPTOR", "RESERVED"
            } };
            if (directoryIndex < 0 || directoryIndex >= static_cast<int>(directoryNameList.size()))
            {
                return QStringLiteral("UNKNOWN");
            }
            return QString::fromLatin1(directoryNameList[static_cast<std::size_t>(directoryIndex)]);
        }

        // calculateSectionEntropy 作用：
        // - 计算区段原始数据熵值；
        // - 用于判断压缩/加壳/高随机度区段。
        double calculateSectionEntropy(
            const QByteArray& fileBytes,
            const std::uint32_t rawOffsetValue,
            const std::uint32_t rawSizeValue)
        {
            if (rawSizeValue == 0 || rawOffsetValue >= static_cast<std::uint32_t>(fileBytes.size()))
            {
                return 0.0;
            }

            const std::uint32_t readableSize = std::min<std::uint32_t>(
                rawSizeValue,
                static_cast<std::uint32_t>(fileBytes.size()) - rawOffsetValue);
            if (readableSize == 0)
            {
                return 0.0;
            }

            std::array<std::uint32_t, 256> countList{};
            for (std::uint32_t index = 0; index < readableSize; ++index)
            {
                const unsigned char byteValue =
                    static_cast<unsigned char>(fileBytes.at(rawOffsetValue + static_cast<qsizetype>(index)));
                ++countList[byteValue];
            }

            double entropyValue = 0.0;
            for (const std::uint32_t countValue : countList)
            {
                if (countValue == 0)
                {
                    continue;
                }
                const double probability = static_cast<double>(countValue) / static_cast<double>(readableSize);
                entropyValue -= probability * std::log2(probability);
            }
            return entropyValue;
        }

        // dumpDataDirectories 作用：
        // - 输出全部数据目录的 RVA/Size 总览；
        // - 供主解析器后续接入。
        void dumpDataDirectories(
            QTextStream& outputStream,
            const std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES>& directoryList)
        {
            outputStream << "\n[数据目录]\n";
            for (int directoryIndex = 0; directoryIndex < static_cast<int>(directoryList.size()); ++directoryIndex)
            {
                const IMAGE_DATA_DIRECTORY& directoryItem = directoryList[static_cast<std::size_t>(directoryIndex)];
                outputStream << QStringLiteral("[%1] %2 RVA=0x%3 Size=0x%4\n")
                    .arg(directoryIndex, 2)
                    .arg(dataDirectoryName(directoryIndex))
                    .arg(QString::number(directoryItem.VirtualAddress, 16).toUpper())
                    .arg(QString::number(directoryItem.Size, 16).toUpper());
            }
        }

        // dumpDelayImportTable 作用：
        // - 解析并输出延迟导入表；
        // - 当前作为拆分后的扩展解析实现保留。
        void dumpDelayImportTable(
            QTextStream& outputStream,
            const QByteArray& fileBytes,
            const bool isPe64,
            const std::uint32_t sizeOfHeadersValue,
            const std::vector<IMAGE_SECTION_HEADER>& sectionList,
            const IMAGE_DATA_DIRECTORY& delayImportDirectory)
        {
            outputStream << "\n[延迟导入表]\n";
            if (delayImportDirectory.VirtualAddress == 0 || delayImportDirectory.Size == 0)
            {
                outputStream << "无延迟导入表。\n";
                return;
            }

            std::uint32_t descriptorOffset = 0;
            if (!rvaToFileOffset(delayImportDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &descriptorOffset))
            {
                outputStream << "延迟导入表 RVA 无法映射到文件偏移。\n";
                return;
            }

            int moduleIndex = 0;
            while (true)
            {
                DelayImportDescriptor descriptor{};
                if (!readPodAtOffset(fileBytes, descriptorOffset, &descriptor))
                {
                    outputStream << "延迟导入描述符读取失败。\n";
                    return;
                }

                if (descriptor.nameRva == 0 && descriptor.iatRva == 0 && descriptor.intRva == 0)
                {
                    if (moduleIndex == 0)
                    {
                        outputStream << "延迟导入表为空。\n";
                    }
                    break;
                }

                std::uint32_t moduleNameOffset = 0;
                const QString moduleNameText =
                    rvaToFileOffset(descriptor.nameRva, sizeOfHeadersValue, sectionList, &moduleNameOffset)
                    ? readNullTerminatedAsciiAtOffset(fileBytes, moduleNameOffset)
                    : QStringLiteral("<模块名解析失败>");
                outputStream << QStringLiteral("模块[%1]: %2\n").arg(moduleIndex).arg(moduleNameText);

                std::uint32_t thunkOffset = 0;
                if (!rvaToFileOffset(descriptor.intRva, sizeOfHeadersValue, sectionList, &thunkOffset))
                {
                    outputStream << "  INT RVA 无法映射。\n";
                    descriptorOffset += sizeof(DelayImportDescriptor);
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
                            outputStream << QStringLiteral("  [%1] Ordinal #%2\n")
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
                            outputStream << QStringLiteral("  [%1] Ordinal #%2\n")
                                .arg(importIndex)
                                .arg(static_cast<qulonglong>(thunk32 & 0xFFFFU));
                            ++importIndex;
                            continue;
                        }
                    }

                    std::uint32_t importByNameOffset = 0;
                    if (!rvaToFileOffset(static_cast<std::uint32_t>(thunkData), sizeOfHeadersValue, sectionList, &importByNameOffset))
                    {
                        break;
                    }

                    const QString functionNameText = readNullTerminatedAsciiAtOffset(
                        fileBytes,
                        static_cast<qsizetype>(importByNameOffset) + 2);
                    outputStream << QStringLiteral("  [%1] %2\n")
                        .arg(importIndex)
                        .arg(functionNameText.isEmpty() ? QStringLiteral("<空名称>") : functionNameText);
                    ++importIndex;
                }

                descriptorOffset += sizeof(DelayImportDescriptor);
                ++moduleIndex;
            }
        }

        // dumpTlsDirectory 作用：
        // - 解析并输出 TLS 表摘要与回调列表；
        // - 当前作为拆分后的扩展解析实现保留。
        void dumpTlsDirectory(
            QTextStream& outputStream,
            const QByteArray& fileBytes,
            const bool isPe64,
            const std::uint64_t imageBaseValue,
            const std::uint32_t sizeOfHeadersValue,
            const std::vector<IMAGE_SECTION_HEADER>& sectionList,
            const IMAGE_DATA_DIRECTORY& tlsDirectory)
        {
            outputStream << "\n[TLS表]\n";
            if (tlsDirectory.VirtualAddress == 0 || tlsDirectory.Size == 0)
            {
                outputStream << "无 TLS 表。\n";
                return;
            }

            std::uint32_t tlsOffset = 0;
            if (!rvaToFileOffset(tlsDirectory.VirtualAddress, sizeOfHeadersValue, sectionList, &tlsOffset))
            {
                outputStream << "TLS RVA 无法映射到文件偏移。\n";
                return;
            }

            std::uint64_t callbacksVa = 0;
            if (isPe64)
            {
                IMAGE_TLS_DIRECTORY64 tlsHeader{};
                if (!readPodAtOffset(fileBytes, tlsOffset, &tlsHeader))
                {
                    outputStream << "TLS64 头读取失败。\n";
                    return;
                }
                callbacksVa = tlsHeader.AddressOfCallBacks;
                outputStream << QStringLiteral("AddressOfCallbacks: 0x%1\n")
                    .arg(QString::number(static_cast<qulonglong>(tlsHeader.AddressOfCallBacks), 16).toUpper());
            }
            else
            {
                IMAGE_TLS_DIRECTORY32 tlsHeader{};
                if (!readPodAtOffset(fileBytes, tlsOffset, &tlsHeader))
                {
                    outputStream << "TLS32 头读取失败。\n";
                    return;
                }
                callbacksVa = tlsHeader.AddressOfCallBacks;
                outputStream << QStringLiteral("AddressOfCallbacks: 0x%1\n")
                    .arg(QString::number(tlsHeader.AddressOfCallBacks, 16).toUpper());
            }

            if (callbacksVa <= imageBaseValue)
            {
                return;
            }

            const std::uint64_t callbacksRva64 = callbacksVa - imageBaseValue;
            if (callbacksRva64 > 0xFFFFFFFFULL)
            {
                return;
            }

            std::uint32_t callbacksOffset = 0;
            if (!rvaToFileOffset(static_cast<std::uint32_t>(callbacksRva64), sizeOfHeadersValue, sectionList, &callbacksOffset))
            {
                outputStream << "TLS Callback RVA 无法映射。\n";
                return;
            }

            outputStream << "Callbacks:\n";
            for (int callbackIndex = 0; callbackIndex < kMaxTlsCallbackCount; ++callbackIndex)
            {
                std::uint64_t callbackVa = 0;
                if (isPe64)
                {
                    std::uint64_t callbackValue = 0;
                    if (!readPodAtOffset(fileBytes, callbacksOffset + callbackIndex * sizeof(std::uint64_t), &callbackValue))
                    {
                        break;
                    }
                    callbackVa = callbackValue;
                }
                else
                {
                    std::uint32_t callbackValue = 0;
                    if (!readPodAtOffset(fileBytes, callbacksOffset + callbackIndex * sizeof(std::uint32_t), &callbackValue))
                    {
                        break;
                    }
                    callbackVa = callbackValue;
                }

                if (callbackVa == 0)
                {
                    break;
                }

                outputStream << QStringLiteral("  [%1] VA=0x%2\n")
                    .arg(callbackIndex)
                    .arg(QString::number(static_cast<qulonglong>(callbackVa), 16).toUpper());
            }
        }
    }
}
