#include "KernelDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QComboBox>
#include <QColor>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QVariant>
#include <QStringList>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    QString kernelHookButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString kernelHookInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString kernelHookHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString kernelHookSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString kernelHookStatusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString kernelHookSafeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString kernelHookFormatAddress(const std::uint64_t addressValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 16, 16, QChar('0'))
            .toUpper();
    }

    QString kernelHookFormatNtStatus(const long statusValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(statusValue)), 8, 16, QChar('0'))
            .toUpper();
    }

    QString kernelHookBytesToText(const std::vector<std::uint8_t>& bytes, const std::uint32_t byteCount)
    {
        // 作用：把 R0 返回的函数头字节转换成可复制、可比对的十六进制文本。
        // 返回：形如 "48 8B ..." 的文本；没有字节时返回占位符。
        QStringList byteTextList;
        const std::size_t count = std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(byteCount));
        byteTextList.reserve(static_cast<int>(count));
        for (std::size_t index = 0U; index < count; ++index)
        {
            byteTextList.push_back(QStringLiteral("%1")
                .arg(static_cast<unsigned int>(bytes[index]), 2, 16, QChar('0'))
                .toUpper());
        }
        return byteTextList.isEmpty() ? QStringLiteral("<无字节>") : byteTextList.join(' ');
    }

    struct KernelHookLoadedModuleInfo
    {
        // 输入：来自 NtQuerySystemInformation(SystemModuleInformation) 的单个内核模块条目。
        // 处理：缓存加载基址、映像大小、NT 路径和文件名，供 Inline Hook 结果按基址反查磁盘文件。
        // 返回：结构体只保存数据，不提供成员函数返回值。
        std::uint64_t imageBase = 0;  // imageBase：内核模块加载基址。
        std::uint32_t imageSize = 0;  // imageSize：内核模块映像大小。
        QString ntPathText;           // ntPathText：R0/SystemModuleInformation 返回的 NT 风格路径。
        QString fileNameText;         // fileNameText：模块文件名，作为路径回退匹配依据。
    };

    struct KernelHookDiskBaselineResult
    {
        // 输入：由 Inline Hook 行的 moduleBase/functionAddress/currentByteCount 派生。
        // 处理：记录 R3 从磁盘 PE 同 RVA 读取基线字节的结果以及和内存字节的对比状态。
        // 返回：结构体只保存数据，不提供成员函数返回值。
        bool available = false;              // available：是否成功读取磁盘基线。
        bool differsFromMemory = false;      // differsFromMemory：磁盘基线是否与内存 currentBytes 不同。
        std::uint32_t byteCount = 0;         // byteCount：实际参与比较的字节数。
        std::uint64_t rva = 0;               // rva：函数入口相对模块基址的 RVA。
        QString filePathText;                // filePathText：R3 实际打开的磁盘文件路径。
        QString statusText;                  // statusText：中文差异状态或失败原因。
        std::vector<std::uint8_t> bytes;     // bytes：磁盘文件同 RVA 读出的字节。
    };

    using KernelHookModulePathMap = QHash<qulonglong, KernelHookLoadedModuleInfo>;
    using KernelHookDiskFileCache = QHash<QString, QByteArray>;

    struct KernelHookSystemModuleEntry
    {
        // 输入：布局对齐 Windows SystemModuleInformation 的 RTL_PROCESS_MODULE_INFORMATION。
        // 处理：本结构仅用于 R3 解析内核模块快照，不写入系统状态。
        // 返回：结构体无成员函数返回值。
        void* section;
        void* mappedBase;
        void* imageBase;
        unsigned long imageSize;
        unsigned long flags;
        unsigned short loadOrderIndex;
        unsigned short initOrderIndex;
        unsigned short loadCount;
        unsigned short offsetToFileName;
        unsigned char fullPathName[256];
    };

    struct KernelHookSystemModuleInformation
    {
        // 输入：NtQuerySystemInformation(SystemModuleInformation) 输出缓冲头。
        // 处理：numberOfModules 后跟随变长模块数组，调用方负责边界检查。
        // 返回：结构体无成员函数返回值。
        unsigned long numberOfModules;
        KernelHookSystemModuleEntry modules[1];
    };

    QString kernelHookBoundedAnsiPathToText(const unsigned char* textBuffer, const std::size_t maxBytes)
    {
        // 输入：SystemModuleInformation 中固定长度 ANSI 路径缓冲。
        // 处理：在 maxBytes 内查找 NUL，避免对未终止内核路径执行越界 strlen。
        // 返回：本地 8-bit 解码后的 QString；输入为空或长度为 0 时返回空字符串。
        if (textBuffer == nullptr || maxBytes == 0U)
        {
            return QString();
        }

        std::size_t textBytes = 0U;
        while (textBytes < maxBytes && textBuffer[textBytes] != '\0')
        {
            ++textBytes;
        }
        return QString::fromLocal8Bit(reinterpret_cast<const char*>(textBuffer), static_cast<int>(textBytes));
    }

    QString kernelHookWindowsDirectoryPath()
    {
        // 输入：无，读取当前系统 Windows 目录。
        // 处理：优先调用 GetWindowsDirectoryW，失败时回退 SystemRoot 环境变量。
        // 返回：规范化后的 Windows 目录；仍失败时返回 C:\Windows。
        wchar_t windowsPathBuffer[MAX_PATH] = {};
        const UINT copiedChars = ::GetWindowsDirectoryW(windowsPathBuffer, MAX_PATH);
        if (copiedChars > 0U && copiedChars < MAX_PATH)
        {
            return QDir::toNativeSeparators(QString::fromWCharArray(windowsPathBuffer));
        }

        const QString envPath = qEnvironmentVariable("SystemRoot");
        return envPath.isEmpty()
            ? QStringLiteral("C:\\Windows")
            : QDir::toNativeSeparators(envPath);
    }

    QString kernelHookSystemDrivePrefix()
    {
        // 输入：无，依赖 Windows 目录所在盘符。
        // 处理：从 C:\Windows 这类路径抽取盘符，用于解析 \Windows\... 内核路径。
        // 返回：形如 C: 的盘符；无法判断时保守返回 C:。
        const QString windowsPath = kernelHookWindowsDirectoryPath();
        if (windowsPath.size() >= 2 && windowsPath.at(1) == QLatin1Char(':'))
        {
            return windowsPath.left(2);
        }
        return QStringLiteral("C:");
    }

    QString kernelHookMapNtDevicePathToDosPath(const QString& ntPathText)
    {
        // 输入：\Device\HarddiskVolumeX\... 形式的 NT 路径。
        // 处理：枚举 A: 到 Z: 的 QueryDosDeviceW 映射，找到匹配前缀后替换为盘符路径。
        // 返回：可供 QFile 打开的 Win32 路径；无法映射时返回空字符串。
        const QString normalizedNtPath = QDir::toNativeSeparators(ntPathText.trimmed());
        if (!normalizedNtPath.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            return QString();
        }

        for (wchar_t driveLetter = L'A'; driveLetter <= L'Z'; ++driveLetter)
        {
            const QString driveName = QStringLiteral("%1:").arg(QChar(driveLetter));
            wchar_t deviceNameBuffer[1024] = {};
            const DWORD copiedChars = ::QueryDosDeviceW(
                reinterpret_cast<LPCWSTR>(driveName.utf16()),
                deviceNameBuffer,
                static_cast<DWORD>(sizeof(deviceNameBuffer) / sizeof(deviceNameBuffer[0])));
            if (copiedChars == 0U)
            {
                continue;
            }

            const QString deviceName = QDir::toNativeSeparators(QString::fromWCharArray(deviceNameBuffer));
            if (deviceName.isEmpty() || !normalizedNtPath.startsWith(deviceName, Qt::CaseInsensitive))
            {
                continue;
            }

            const QString suffixText = normalizedNtPath.mid(deviceName.size());
            return QDir::toNativeSeparators(driveName + suffixText);
        }

        return QString();
    }

    QString kernelHookNormalizeKernelModulePath(const QString& rawPathText)
    {
        // 输入：R0/SystemModuleInformation 可能返回的 NT、SystemRoot 或 Win32 模块路径。
        // 处理：把常见内核路径转换成 R3 可访问的 Win32 文件路径，并保留已有盘符路径。
        // 返回：可尝试打开的本地路径；无法转换时返回空字符串。
        QString pathText = QDir::toNativeSeparators(rawPathText.trimmed());
        if (pathText.isEmpty() || pathText == QStringLiteral("<未解析>"))
        {
            return QString();
        }

        if (pathText.startsWith(QStringLiteral("\\??\\"), Qt::CaseInsensitive))
        {
            pathText = pathText.mid(4);
        }
        if (pathText.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive))
        {
            pathText = kernelHookWindowsDirectoryPath() + pathText.mid(QStringLiteral("\\SystemRoot").size());
        }
        else if (pathText.startsWith(QStringLiteral("SystemRoot\\"), Qt::CaseInsensitive))
        {
            pathText = kernelHookWindowsDirectoryPath() + QStringLiteral("\\") + pathText.mid(QStringLiteral("SystemRoot\\").size());
        }
        else if (pathText.startsWith(QStringLiteral("\\Windows\\"), Qt::CaseInsensitive))
        {
            pathText = kernelHookSystemDrivePrefix() + pathText;
        }
        else if (pathText.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            pathText = kernelHookMapNtDevicePathToDosPath(pathText);
        }

        if (pathText.size() >= 2 && pathText.at(1) == QLatin1Char(':'))
        {
            return QDir::toNativeSeparators(QFileInfo(pathText).absoluteFilePath());
        }
        return QString();
    }

    KernelHookModulePathMap kernelHookQueryLoadedModulePathMap()
    {
        // 输入：无，调用当前系统 ntdll!NtQuerySystemInformation(SystemModuleInformation)。
        // 处理：读取已加载内核模块快照，把模块加载基址映射到 NT 路径、文件名和映像大小。
        // 返回：以 imageBase 为 key 的模块路径表；查询失败时返回空表，调用方继续使用 R0 观察基线。
        using NtQuerySystemInformationFn = long (NTAPI*)(unsigned long, void*, unsigned long, unsigned long*);
        constexpr unsigned long kSystemModuleInformationClass = 11UL;
        constexpr long kStatusInfoLengthMismatch = static_cast<long>(0xC0000004L);

        KernelHookModulePathMap modulePathMap;
        HMODULE ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdllModule == nullptr)
        {
            return modulePathMap;
        }

        const auto querySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
            ::GetProcAddress(ntdllModule, "NtQuerySystemInformation"));
        if (querySystemInformation == nullptr)
        {
            return modulePathMap;
        }

        unsigned long requiredBytes = 0UL;
        long status = querySystemInformation(kSystemModuleInformationClass, nullptr, 0UL, &requiredBytes);
        if (requiredBytes == 0UL)
        {
            requiredBytes = 1024UL * 1024UL;
        }

        std::vector<std::uint8_t> snapshotBuffer(static_cast<std::size_t>(requiredBytes) + (64U * 1024U));
        for (int attemptIndex = 0; attemptIndex < 4; ++attemptIndex)
        {
            status = querySystemInformation(
                kSystemModuleInformationClass,
                snapshotBuffer.data(),
                static_cast<unsigned long>(snapshotBuffer.size()),
                &requiredBytes);
            if (status == 0)
            {
                break;
            }

            if (requiredBytes > snapshotBuffer.size())
            {
                snapshotBuffer.resize(static_cast<std::size_t>(requiredBytes) + (64U * 1024U));
                continue;
            }
            if (status == kStatusInfoLengthMismatch)
            {
                snapshotBuffer.resize(snapshotBuffer.size() * 2U);
                continue;
            }
            return modulePathMap;
        }
        if (status != 0 || snapshotBuffer.size() < offsetof(KernelHookSystemModuleInformation, modules))
        {
            return modulePathMap;
        }

        const auto* moduleInfo = reinterpret_cast<const KernelHookSystemModuleInformation*>(snapshotBuffer.data());
        const std::size_t moduleArrayOffset = offsetof(KernelHookSystemModuleInformation, modules);
        const std::size_t availableModuleBytes = snapshotBuffer.size() - moduleArrayOffset;
        const std::size_t maxModuleCount = availableModuleBytes / sizeof(KernelHookSystemModuleEntry);
        const std::size_t moduleCount = std::min<std::size_t>(
            static_cast<std::size_t>(moduleInfo->numberOfModules),
            maxModuleCount);

        modulePathMap.reserve(static_cast<int>(moduleCount));
        for (std::size_t moduleIndex = 0U; moduleIndex < moduleCount; ++moduleIndex)
        {
            const KernelHookSystemModuleEntry& moduleEntry = moduleInfo->modules[moduleIndex];
            const auto imageBase = static_cast<qulonglong>(
                reinterpret_cast<std::uintptr_t>(moduleEntry.imageBase));
            if (imageBase == 0ULL)
            {
                continue;
            }

            const QString ntPathText = QDir::toNativeSeparators(
                kernelHookBoundedAnsiPathToText(moduleEntry.fullPathName, sizeof(moduleEntry.fullPathName)));
            const std::size_t fileNameOffset = moduleEntry.offsetToFileName < sizeof(moduleEntry.fullPathName)
                ? static_cast<std::size_t>(moduleEntry.offsetToFileName)
                : 0U;
            const QString fileNameText = kernelHookBoundedAnsiPathToText(
                moduleEntry.fullPathName + fileNameOffset,
                sizeof(moduleEntry.fullPathName) - fileNameOffset);

            KernelHookLoadedModuleInfo loadedModule{};
            loadedModule.imageBase = static_cast<std::uint64_t>(imageBase);
            loadedModule.imageSize = static_cast<std::uint32_t>(moduleEntry.imageSize);
            loadedModule.ntPathText = ntPathText;
            loadedModule.fileNameText = fileNameText;
            modulePathMap.insert(imageBase, loadedModule);
        }

        return modulePathMap;
    }

    template <typename PodType>
    bool kernelHookReadPodAtOffset(const QByteArray& fileBytes, const std::uint64_t fileOffset, PodType* valueOut)
    {
        // 输入：文件字节、文件偏移和 POD 输出对象。
        // 处理：检查偏移范围后用 memcpy 复制，避免对未对齐 PE 结构直接解引用。
        // 返回：读取成功返回 true；参数无效或越界返回 false。
        if (valueOut == nullptr)
        {
            return false;
        }
        if (fileOffset > static_cast<std::uint64_t>(fileBytes.size()) ||
            sizeof(PodType) > static_cast<std::uint64_t>(fileBytes.size()) - fileOffset)
        {
            return false;
        }

        std::memcpy(
            valueOut,
            fileBytes.constData() + static_cast<qsizetype>(fileOffset),
            sizeof(PodType));
        return true;
    }

    bool kernelHookRvaToFileOffset(
        const QByteArray& fileBytes,
        const std::uint32_t rva,
        const std::uint32_t bytesToRead,
        std::uint64_t* fileOffsetOut,
        QString* errorTextOut)
    {
        // 输入：磁盘 PE 文件字节、目标 RVA 和期望读取长度。
        // 处理：解析 DOS/NT/Optional Header 与节表，把内存 RVA 映射到文件 raw offset，并校验读取区间。
        // 返回：映射成功返回 true，同时写出文件偏移；失败返回 false 并填充中文原因。
        auto fail = [errorTextOut](const QString& messageText) -> bool
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = messageText;
                }
                return false;
            };

        if (fileOffsetOut == nullptr)
        {
            return fail(QStringLiteral("内部错误：文件偏移输出为空。"));
        }
        *fileOffsetOut = 0ULL;
        if (bytesToRead == 0U)
        {
            return fail(QStringLiteral("读取长度为 0。"));
        }
        if (fileBytes.size() < static_cast<qsizetype>(sizeof(IMAGE_DOS_HEADER)))
        {
            return fail(QStringLiteral("磁盘文件过小，无法读取 DOS 头。"));
        }

        IMAGE_DOS_HEADER dosHeader{};
        if (!kernelHookReadPodAtOffset(fileBytes, 0ULL, &dosHeader) ||
            dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
            dosHeader.e_lfanew < 0)
        {
            return fail(QStringLiteral("磁盘文件不是有效 MZ/PE 文件。"));
        }

        const std::uint64_t ntHeaderOffset = static_cast<std::uint64_t>(dosHeader.e_lfanew);
        std::uint32_t peSignature = 0U;
        if (!kernelHookReadPodAtOffset(fileBytes, ntHeaderOffset, &peSignature) ||
            peSignature != IMAGE_NT_SIGNATURE)
        {
            return fail(QStringLiteral("磁盘文件 PE 签名无效。"));
        }

        IMAGE_FILE_HEADER fileHeader{};
        const std::uint64_t fileHeaderOffset = ntHeaderOffset + sizeof(std::uint32_t);
        if (!kernelHookReadPodAtOffset(fileBytes, fileHeaderOffset, &fileHeader))
        {
            return fail(QStringLiteral("读取 COFF 文件头失败。"));
        }
        if (fileHeader.NumberOfSections == 0U || fileHeader.NumberOfSections > 96U)
        {
            return fail(QStringLiteral("PE 区段数量异常：%1。").arg(fileHeader.NumberOfSections));
        }

        const std::uint64_t optionalHeaderOffset = fileHeaderOffset + sizeof(IMAGE_FILE_HEADER);
        std::uint16_t optionalMagic = 0U;
        if (!kernelHookReadPodAtOffset(fileBytes, optionalHeaderOffset, &optionalMagic))
        {
            return fail(QStringLiteral("读取 Optional Header 魔数失败。"));
        }

        std::uint32_t sizeOfHeaders = 0U;
        if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER64 optionalHeader{};
            if (!kernelHookReadPodAtOffset(fileBytes, optionalHeaderOffset, &optionalHeader))
            {
                return fail(QStringLiteral("读取 PE32+ Optional Header 失败。"));
            }
            sizeOfHeaders = optionalHeader.SizeOfHeaders;
        }
        else if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 optionalHeader{};
            if (!kernelHookReadPodAtOffset(fileBytes, optionalHeaderOffset, &optionalHeader))
            {
                return fail(QStringLiteral("读取 PE32 Optional Header 失败。"));
            }
            sizeOfHeaders = optionalHeader.SizeOfHeaders;
        }
        else
        {
            return fail(QStringLiteral("未知 Optional Header 魔数：0x%1。")
                .arg(optionalMagic, 4, 16, QChar('0')).toUpper());
        }

        if (rva < sizeOfHeaders)
        {
            const std::uint64_t headerOffset = static_cast<std::uint64_t>(rva);
            if (headerOffset <= static_cast<std::uint64_t>(fileBytes.size()) &&
                bytesToRead <= static_cast<std::uint64_t>(fileBytes.size()) - headerOffset)
            {
                *fileOffsetOut = headerOffset;
                return true;
            }
            return fail(QStringLiteral("RVA 位于 PE 头部，但读取范围超出磁盘文件。"));
        }

        const std::uint64_t sectionTableOffset = optionalHeaderOffset + fileHeader.SizeOfOptionalHeader;
        for (std::uint16_t sectionIndex = 0U; sectionIndex < fileHeader.NumberOfSections; ++sectionIndex)
        {
            IMAGE_SECTION_HEADER sectionHeader{};
            const std::uint64_t currentSectionOffset =
                sectionTableOffset + (static_cast<std::uint64_t>(sectionIndex) * sizeof(IMAGE_SECTION_HEADER));
            if (!kernelHookReadPodAtOffset(fileBytes, currentSectionOffset, &sectionHeader))
            {
                return fail(QStringLiteral("读取 PE 区段表失败，索引=%1。").arg(sectionIndex));
            }

            const std::uint32_t virtualAddress = sectionHeader.VirtualAddress;
            const std::uint32_t virtualSize = sectionHeader.Misc.VirtualSize;
            const std::uint32_t rawSize = sectionHeader.SizeOfRawData;
            const std::uint32_t mappedSize = std::max(virtualSize, rawSize);
            const std::uint64_t sectionStart = static_cast<std::uint64_t>(virtualAddress);
            const std::uint64_t sectionEnd = sectionStart + static_cast<std::uint64_t>(mappedSize);
            const std::uint64_t targetRva = static_cast<std::uint64_t>(rva);
            if (mappedSize == 0U || targetRva < sectionStart || targetRva >= sectionEnd)
            {
                continue;
            }

            const std::uint32_t delta = rva - virtualAddress;
            if (delta >= rawSize || bytesToRead > rawSize - delta)
            {
                return fail(QStringLiteral("RVA 落在区段虚拟范围内，但磁盘 raw 数据不足。"));
            }

            const std::uint64_t rawOffset =
                static_cast<std::uint64_t>(sectionHeader.PointerToRawData) + static_cast<std::uint64_t>(delta);
            if (rawOffset > static_cast<std::uint64_t>(fileBytes.size()) ||
                bytesToRead > static_cast<std::uint64_t>(fileBytes.size()) - rawOffset)
            {
                return fail(QStringLiteral("映射到的磁盘偏移超出文件大小。"));
            }

            *fileOffsetOut = rawOffset;
            return true;
        }

        return fail(QStringLiteral("未找到覆盖目标 RVA 的 PE 区段。"));
    }

    bool kernelHookReadPeBytesAtRva(
        const QString& filePath,
        const std::uint32_t rva,
        const std::uint32_t bytesToRead,
        KernelHookDiskFileCache* fileCache,
        std::vector<std::uint8_t>* bytesOut,
        QString* errorTextOut)
    {
        // 输入：磁盘 PE 路径、目标 RVA 和读取长度。
        // 处理：读取或复用线程内缓存的文件字节，把 RVA 转换为 raw offset，再复制指定长度的磁盘字节。
        // 返回：成功返回 true 并填充 bytesOut；失败返回 false 并填充中文错误。
        auto fail = [errorTextOut](const QString& messageText) -> bool
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = messageText;
                }
                return false;
            };

        if (bytesOut == nullptr)
        {
            return fail(QStringLiteral("内部错误：磁盘字节输出为空。"));
        }
        bytesOut->clear();

        QByteArray localFileBytes;
        const QByteArray* fileBytes = nullptr;
        if (fileCache != nullptr)
        {
            auto cacheIterator = fileCache->constFind(filePath);
            if (cacheIterator == fileCache->constEnd())
            {
                QFile fileObject(filePath);
                if (!fileObject.open(QIODevice::ReadOnly))
                {
                    return fail(QStringLiteral("打开磁盘模块文件失败：%1。").arg(fileObject.errorString()));
                }

                localFileBytes = fileObject.readAll();
                if (localFileBytes.isEmpty())
                {
                    return fail(QStringLiteral("磁盘模块文件为空或读取失败。"));
                }

                fileCache->insert(filePath, localFileBytes);
                cacheIterator = fileCache->constFind(filePath);
            }

            if (cacheIterator != fileCache->constEnd())
            {
                fileBytes = &cacheIterator.value();
            }
        }
        else
        {
            QFile fileObject(filePath);
            if (!fileObject.open(QIODevice::ReadOnly))
            {
                return fail(QStringLiteral("打开磁盘模块文件失败：%1。").arg(fileObject.errorString()));
            }

            localFileBytes = fileObject.readAll();
            fileBytes = &localFileBytes;
        }

        if (fileBytes == nullptr || fileBytes->isEmpty())
        {
            return fail(QStringLiteral("磁盘模块文件为空或读取失败。"));
        }

        std::uint64_t fileOffset = 0ULL;
        QString mapErrorText;
        if (!kernelHookRvaToFileOffset(*fileBytes, rva, bytesToRead, &fileOffset, &mapErrorText))
        {
            return fail(mapErrorText);
        }

        bytesOut->resize(bytesToRead);
        std::memcpy(
            bytesOut->data(),
            fileBytes->constData() + static_cast<qsizetype>(fileOffset),
            bytesToRead);
        return true;
    }

    KernelHookDiskBaselineResult kernelHookReadDiskBaselineForInlineHook(
        const KernelInlineHookEntry& row,
        const KernelHookModulePathMap& modulePathMap,
        KernelHookDiskFileCache* fileCache)
    {
        // 输入：Inline Hook UI 行与当前系统模块路径映射。
        // 处理：用 functionAddress-moduleBase 计算 RVA，从磁盘模块文件读取同 RVA 字节并与内存字节比较。
        // 返回：包含可用性、磁盘字节、差异状态和中文诊断原因的磁盘基线结果。
        KernelHookDiskBaselineResult baselineResult{};
        const std::size_t availableCurrentBytes = std::min<std::size_t>(
            row.currentBytes.size(),
            static_cast<std::size_t>(row.currentByteCount));
        baselineResult.byteCount = static_cast<std::uint32_t>(std::min<std::size_t>(
            availableCurrentBytes,
            static_cast<std::size_t>(KSWORD_ARK_KERNEL_HOOK_BYTES)));

        if (baselineResult.byteCount == 0U)
        {
            baselineResult.statusText = QStringLiteral("不可用：R0 未返回内存字节。");
            return baselineResult;
        }
        if (row.moduleBase == 0U || row.functionAddress < row.moduleBase)
        {
            baselineResult.statusText = QStringLiteral("不可用：函数地址或模块基址无效。");
            return baselineResult;
        }

        const std::uint64_t rva64 = row.functionAddress - row.moduleBase;
        baselineResult.rva = rva64;
        if (rva64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            baselineResult.statusText = QStringLiteral("不可用：函数 RVA 超出 32 位 PE 范围。");
            return baselineResult;
        }

        const auto moduleIterator = modulePathMap.constFind(static_cast<qulonglong>(row.moduleBase));
        if (moduleIterator == modulePathMap.constEnd())
        {
            baselineResult.statusText = QStringLiteral("不可用：R3 未能反查模块磁盘路径。");
            return baselineResult;
        }

        baselineResult.filePathText = kernelHookNormalizeKernelModulePath(moduleIterator->ntPathText);
        if (baselineResult.filePathText.isEmpty())
        {
            baselineResult.statusText = QStringLiteral("不可用：模块路径无法转换为 Win32 路径（%1）。")
                .arg(kernelHookSafeText(moduleIterator->ntPathText, QStringLiteral("<空路径>")));
            return baselineResult;
        }
        if (!QFileInfo::exists(baselineResult.filePathText))
        {
            baselineResult.statusText = QStringLiteral("不可用：磁盘模块文件不存在（%1）。")
                .arg(QDir::toNativeSeparators(baselineResult.filePathText));
            return baselineResult;
        }

        QString readErrorText;
        if (!kernelHookReadPeBytesAtRva(
            baselineResult.filePathText,
            static_cast<std::uint32_t>(rva64),
            baselineResult.byteCount,
            fileCache,
            &baselineResult.bytes,
            &readErrorText))
        {
            baselineResult.statusText = QStringLiteral("不可用：%1").arg(readErrorText);
            return baselineResult;
        }

        baselineResult.available = true;
        baselineResult.differsFromMemory = !std::equal(
            baselineResult.bytes.begin(),
            baselineResult.bytes.end(),
            row.currentBytes.begin());
        baselineResult.statusText = baselineResult.differsFromMemory
            ? QStringLiteral("不同：内存字节与磁盘基线不一致")
            : QStringLiteral("一致：内存字节与磁盘基线相同");
        return baselineResult;
    }

    void kernelHookCopyTextToClipboard(const QString& text)
    {
        if (QApplication::clipboard() != nullptr)
        {
            QApplication::clipboard()->setText(text);
        }
    }

    enum class ShadowSsdtColumn : int
    {
        Index = 0,
        ServiceName,
        StubAddress,
        ServiceAddress,
        Module,
        Status,
        Count
    };

    enum class InlineHookColumn : int
    {
        Module = 0,
        Function,
        FunctionAddress,
        HookType,
        TargetAddress,
        TargetModule,
        Status,
        CurrentBytes,
        DiskBytes,
        DiskDiff,
        Count
    };

    enum class IatEatHookColumn : int
    {
        Class = 0,
        Module,
        ImportModule,
        Function,
        ThunkAddress,
        CurrentTarget,
        ExpectedTarget,
        TargetModule,
        Status,
        Count
    };

    QString kernelHookStatusText(const std::uint32_t status)
    {
        switch (status)
        {
        case KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN:
            return QStringLiteral("干净");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS:
            return QStringLiteral("可疑外跳");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH:
            return QStringLiteral("模块内跳转");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED:
            return QStringLiteral("读取失败");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED:
            return QStringLiteral("解析失败");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED:
            return QStringLiteral("需要强制确认");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED:
            return QStringLiteral("已修复/摘除");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED:
            return QStringLiteral("修复失败");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN:
        default:
            return QStringLiteral("未知(%1)").arg(status);
        }
    }

    QString inlineHookTypeText(const std::uint32_t hookType)
    {
        switch (hookType)
        {
        case KSWORD_ARK_INLINE_HOOK_TYPE_NONE:
            return QStringLiteral("无明显补丁");
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL32:
            return QStringLiteral("JMP rel32");
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL8:
            return QStringLiteral("JMP rel8");
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_RIP_INDIRECT:
            return QStringLiteral("JMP [RIP+rel32]");
        case KSWORD_ARK_INLINE_HOOK_TYPE_MOV_RAX_JMP_RAX:
            return QStringLiteral("MOV RAX; JMP RAX");
        case KSWORD_ARK_INLINE_HOOK_TYPE_MOV_R11_JMP_R11:
            return QStringLiteral("MOV R11; JMP R11");
        case KSWORD_ARK_INLINE_HOOK_TYPE_RET_PATCH:
            return QStringLiteral("RET 补丁");
        case KSWORD_ARK_INLINE_HOOK_TYPE_INT3_PATCH:
            return QStringLiteral("INT3 补丁");
        case KSWORD_ARK_INLINE_HOOK_TYPE_UNKNOWN_PATCH:
            return QStringLiteral("未知补丁");
        default:
            return QStringLiteral("未知(%1)").arg(hookType);
        }
    }

    std::uint32_t inlineHookPatchLength(const std::uint32_t hookType, const std::uint32_t availableBytes)
    {
        // 作用：为 NOP 摘除计算保守补丁长度，避免覆盖跳转后面的非指令立即数。
        // 返回：要写入 NOP 的字节数；0 表示当前类型不适合自动处理。
        std::uint32_t desiredBytes = 0U;
        switch (hookType)
        {
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL32:
            desiredBytes = 5U;
            break;
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL8:
            desiredBytes = 2U;
            break;
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_RIP_INDIRECT:
            desiredBytes = 6U;
            break;
        case KSWORD_ARK_INLINE_HOOK_TYPE_MOV_RAX_JMP_RAX:
            desiredBytes = 12U;
            break;
        case KSWORD_ARK_INLINE_HOOK_TYPE_MOV_R11_JMP_R11:
            desiredBytes = 13U;
            break;
        case KSWORD_ARK_INLINE_HOOK_TYPE_RET_PATCH:
        case KSWORD_ARK_INLINE_HOOK_TYPE_INT3_PATCH:
            desiredBytes = 1U;
            break;
        default:
            desiredBytes = 0U;
            break;
        }
        return std::min<std::uint32_t>(desiredBytes, availableBytes);
    }

    QString iatEatClassText(const std::uint32_t hookClass)
    {
        switch (hookClass)
        {
        case KSWORD_ARK_IAT_EAT_HOOK_CLASS_IAT:
            return QStringLiteral("IAT");
        case KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT:
            return QStringLiteral("EAT");
        default:
            return QStringLiteral("未知(%1)").arg(hookClass);
        }
    }

    QColor statusColor(const std::uint32_t status)
    {
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED)
        {
            return QColor(QStringLiteral("#B23A3A"));
        }
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED)
        {
            return QColor(QStringLiteral("#D77A00"));
        }
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN)
        {
            return QColor(QStringLiteral("#3A8F3A"));
        }
        return KswordTheme::TextSecondaryColor();
    }

    void prepareTable(QTableWidget* tableWidget)
    {
        // 作用：统一三张 Hook 表格的基础交互行为。
        // 返回：无；Qt 控件由调用方持有。
        if (tableWidget == nullptr)
        {
            return;
        }
        tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tableWidget->setAlternatingRowColors(true);
        tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        tableWidget->setStyleSheet(kernelHookSelectionStyle());
        tableWidget->setCornerButtonEnabled(false);
        tableWidget->verticalHeader()->setVisible(false);
        tableWidget->horizontalHeader()->setStyleSheet(kernelHookHeaderStyle());
        tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }

    QString shadowSsdtColumnHeader(const ShadowSsdtColumn column)
    {
        switch (column)
        {
        case ShadowSsdtColumn::Index:
            return QStringLiteral("索引");
        case ShadowSsdtColumn::ServiceName:
            return QStringLiteral("服务名");
        case ShadowSsdtColumn::StubAddress:
            return QStringLiteral("Stub地址");
        case ShadowSsdtColumn::ServiceAddress:
            return QStringLiteral("服务例程");
        case ShadowSsdtColumn::Module:
            return QStringLiteral("模块");
        case ShadowSsdtColumn::Status:
            return QStringLiteral("状态");
        default:
            return QStringLiteral("未知列");
        }
    }

    QString inlineHookColumnHeader(const InlineHookColumn column)
    {
        switch (column)
        {
        case InlineHookColumn::Module:
            return QStringLiteral("模块");
        case InlineHookColumn::Function:
            return QStringLiteral("函数");
        case InlineHookColumn::FunctionAddress:
            return QStringLiteral("函数地址");
        case InlineHookColumn::HookType:
            return QStringLiteral("类型");
        case InlineHookColumn::TargetAddress:
            return QStringLiteral("目标地址");
        case InlineHookColumn::TargetModule:
            return QStringLiteral("目标模块");
        case InlineHookColumn::Status:
            return QStringLiteral("状态");
        case InlineHookColumn::CurrentBytes:
            return QStringLiteral("内存字节");
        case InlineHookColumn::DiskBytes:
            return QStringLiteral("磁盘字节");
        case InlineHookColumn::DiskDiff:
            return QStringLiteral("差异状态");
        default:
            return QStringLiteral("未知列");
        }
    }

    QString iatEatColumnHeader(const IatEatHookColumn column)
    {
        switch (column)
        {
        case IatEatHookColumn::Class:
            return QStringLiteral("类别");
        case IatEatHookColumn::Module:
            return QStringLiteral("模块");
        case IatEatHookColumn::ImportModule:
            return QStringLiteral("导入模块");
        case IatEatHookColumn::Function:
            return QStringLiteral("函数/序号");
        case IatEatHookColumn::ThunkAddress:
            return QStringLiteral("Thunk/EAT项");
        case IatEatHookColumn::CurrentTarget:
            return QStringLiteral("当前目标");
        case IatEatHookColumn::ExpectedTarget:
            return QStringLiteral("期望目标");
        case IatEatHookColumn::TargetModule:
            return QStringLiteral("目标模块");
        case IatEatHookColumn::Status:
            return QStringLiteral("状态");
        default:
            return QStringLiteral("未知列");
        }
    }

    QString shadowSsdtColumnText(const KernelSsdtEntry& entry, const ShadowSsdtColumn column)
    {
        switch (column)
        {
        case ShadowSsdtColumn::Index:
            return entry.indexResolved ? QString::number(entry.serviceIndex) : QStringLiteral("<未知>");
        case ShadowSsdtColumn::ServiceName:
            return kernelHookSafeText(entry.serviceNameText);
        case ShadowSsdtColumn::StubAddress:
            return kernelHookFormatAddress(entry.zwRoutineAddress);
        case ShadowSsdtColumn::ServiceAddress:
            return kernelHookFormatAddress(entry.serviceRoutineAddress);
        case ShadowSsdtColumn::Module:
            return kernelHookSafeText(entry.moduleNameText);
        case ShadowSsdtColumn::Status:
            return kernelHookSafeText(entry.statusText);
        default:
            return QString();
        }
    }

    QString inlineHookColumnText(const KernelInlineHookEntry& entry, const InlineHookColumn column)
    {
        switch (column)
        {
        case InlineHookColumn::Module:
            return kernelHookSafeText(entry.moduleNameText);
        case InlineHookColumn::Function:
            return kernelHookSafeText(entry.functionNameText);
        case InlineHookColumn::FunctionAddress:
            return kernelHookFormatAddress(entry.functionAddress);
        case InlineHookColumn::HookType:
            return entry.hookTypeText;
        case InlineHookColumn::TargetAddress:
            return kernelHookFormatAddress(entry.targetAddress);
        case InlineHookColumn::TargetModule:
            return kernelHookSafeText(entry.targetModuleNameText, QStringLiteral("<未解析>"));
        case InlineHookColumn::Status:
            return entry.statusText;
        case InlineHookColumn::CurrentBytes:
            return entry.currentBytesText;
        case InlineHookColumn::DiskBytes:
            return entry.diskBytesText;
        case InlineHookColumn::DiskDiff:
            return entry.diskBaselineStatusText;
        default:
            return QString();
        }
    }

    QString iatEatColumnText(const KernelIatEatHookEntry& entry, const IatEatHookColumn column)
    {
        switch (column)
        {
        case IatEatHookColumn::Class:
            return entry.classText;
        case IatEatHookColumn::Module:
            return kernelHookSafeText(entry.moduleNameText);
        case IatEatHookColumn::ImportModule:
            return kernelHookSafeText(entry.importModuleNameText, QStringLiteral("<不适用>"));
        case IatEatHookColumn::Function:
            return kernelHookSafeText(entry.functionNameText, QStringLiteral("#%1").arg(entry.ordinal));
        case IatEatHookColumn::ThunkAddress:
            return kernelHookFormatAddress(entry.thunkAddress);
        case IatEatHookColumn::CurrentTarget:
            return kernelHookFormatAddress(entry.currentTarget);
        case IatEatHookColumn::ExpectedTarget:
            return kernelHookFormatAddress(entry.expectedTarget);
        case IatEatHookColumn::TargetModule:
            return kernelHookSafeText(entry.targetModuleNameText, QStringLiteral("<未解析>"));
        case IatEatHookColumn::Status:
            return entry.statusText;
        default:
            return QString();
        }
    }

    QString shadowSsdtRowAsTsv(const KernelSsdtEntry& entry)
    {
        QStringList fields;
        fields.reserve(static_cast<int>(ShadowSsdtColumn::Count));
        for (int index = 0; index < static_cast<int>(ShadowSsdtColumn::Count); ++index)
        {
            fields.push_back(shadowSsdtColumnText(entry, static_cast<ShadowSsdtColumn>(index)));
        }
        return fields.join('\t');
    }

    QString inlineHookRowAsTsv(const KernelInlineHookEntry& entry)
    {
        QStringList fields;
        fields.reserve(static_cast<int>(InlineHookColumn::Count));
        for (int index = 0; index < static_cast<int>(InlineHookColumn::Count); ++index)
        {
            fields.push_back(inlineHookColumnText(entry, static_cast<InlineHookColumn>(index)));
        }
        return fields.join('\t');
    }

    QString iatEatRowAsTsv(const KernelIatEatHookEntry& entry)
    {
        QStringList fields;
        fields.reserve(static_cast<int>(IatEatHookColumn::Count));
        for (int index = 0; index < static_cast<int>(IatEatHookColumn::Count); ++index)
        {
            fields.push_back(iatEatColumnText(entry, static_cast<IatEatHookColumn>(index)));
        }
        return fields.join('\t');
    }

    QString headerAsTsv(const int columnCount, const std::function<QString(int)>& headerResolver)
    {
        // 作用：构造复制表头 TSV；不同表格通过 headerResolver 提供中文列名。
        // 返回：单行 TSV 表头。
        QStringList headers;
        headers.reserve(columnCount);
        for (int index = 0; index < columnCount; ++index)
        {
            headers.push_back(headerResolver(index));
        }
        return headers.join('\t');
    }

    template <typename RowType>
    std::vector<std::size_t> selectedSourceIndices(
        const QTableWidget* tableWidget,
        const std::vector<RowType>& rows,
        const int fallbackRow)
    {
        // 作用：读取表格选中行映射的源缓存索引，支持 Ctrl 多选。
        // 返回：去重后的源索引；没有显式选择时使用 fallbackRow。
        std::vector<std::size_t> result;
        if (tableWidget == nullptr)
        {
            return result;
        }

        const QModelIndexList selectedRows = tableWidget->selectionModel() != nullptr
            ? tableWidget->selectionModel()->selectedRows()
            : QModelIndexList();
        for (const QModelIndex& modelIndex : selectedRows)
        {
            const QTableWidgetItem* item = tableWidget->item(modelIndex.row(), 0);
            if (item == nullptr)
            {
                continue;
            }
            const std::size_t sourceIndex = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
            if (sourceIndex < rows.size())
            {
                result.push_back(sourceIndex);
            }
        }

        if (result.empty() && fallbackRow >= 0)
        {
            const QTableWidgetItem* item = tableWidget->item(fallbackRow, 0);
            if (item != nullptr)
            {
                const std::size_t sourceIndex = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
                if (sourceIndex < rows.size())
                {
                    result.push_back(sourceIndex);
                }
            }
        }

        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    QString buildInlineHookDetailText(const KernelInlineHookEntry& row)
    {
        // 输入：已转换并可能已补充磁盘基线的 Inline Hook 行。
        // 处理：统一生成 CodeEditorWidget 详情文本，明确区分内存字节、R0 观察基线和 R3 磁盘基线。
        // 返回：中文多行详情文本；不执行任何内核写入或自动修复。
        const QString diskBytesText = row.diskBaselineAvailable
            ? row.diskBytesText
            : QStringLiteral("<不可用>");
        const QString diskByteCountText = row.diskBaselineAvailable
            ? QString::number(std::min<std::size_t>(row.diskBytes.size(), static_cast<std::size_t>(row.currentByteCount)))
            : QStringLiteral("0");
        const QString diskPathText = row.diskBaselinePathText.trimmed().isEmpty()
            ? QStringLiteral("<不可用>")
            : QDir::toNativeSeparators(row.diskBaselinePathText);
        const QString rvaText = (row.moduleBase != 0U && row.functionAddress >= row.moduleBase)
            ? kernelHookFormatAddress(row.functionAddress - row.moduleBase)
            : QStringLiteral("<未解析>");

        return QStringLiteral(
            "Inline Hook 检测详情\n"
            "模块: %1\n"
            "函数: %2\n"
            "函数地址: %3\n"
            "Hook类型: %4\n"
            "目标地址: %5\n"
            "目标模块: %6\n"
            "状态: %7\n"
            "模块基址: %8\n"
            "目标模块基址: %9\n"
            "当前内存字节(%10): %11\n"
            "R0 观察基线(%12): %13\n"
            "磁盘基线字节(%14): %15\n"
            "差异状态: %16\n"
            "磁盘路径: %17\n"
            "RVA: %18\n"
            "标志: 0x%19\n\n"
            "说明: 当前协议字段 expectedBytes 在 R0 中来自内存观察，通常是 currentBytes 的同源快照，不代表磁盘原始字节。"
            "本页额外由 R3 按模块基址和 RVA 从磁盘模块文件读取基线字节并与当前内存字节比较；"
            "如果磁盘基线不可用，请只把 R0 观察基线当作诊断快照，不要把它理解为干净基线。"
            "磁盘基线是文件同 RVA 的 raw 字节，未应用重定位、热补丁或厂商运行时改写校正，差异仍需结合 Hook 类型和目标地址判断。"
            "摘除操作保持原有 NOP 流程，不新增自动修复能力。")
            .arg(kernelHookSafeText(row.moduleNameText))
            .arg(kernelHookSafeText(row.functionNameText))
            .arg(kernelHookFormatAddress(row.functionAddress))
            .arg(row.hookTypeText)
            .arg(kernelHookFormatAddress(row.targetAddress))
            .arg(kernelHookSafeText(row.targetModuleNameText, QStringLiteral("<未解析>")))
            .arg(row.statusText)
            .arg(kernelHookFormatAddress(row.moduleBase))
            .arg(kernelHookFormatAddress(row.targetModuleBase))
            .arg(row.currentByteCount)
            .arg(row.currentBytesText)
            .arg(row.originalByteCount)
            .arg(row.observedBytesText)
            .arg(diskByteCountText)
            .arg(diskBytesText)
            .arg(row.diskBaselineStatusText)
            .arg(diskPathText)
            .arg(rvaText)
            .arg(static_cast<qulonglong>(row.flags), 8, 16, QChar('0'));
    }

    void applyDiskBaselineToInlineHookEntry(
        KernelInlineHookEntry* row,
        const KernelHookModulePathMap& modulePathMap,
        KernelHookDiskFileCache* fileCache)
    {
        // 输入：待补充的 Inline Hook 行和 R3 查询到的模块基址/路径映射。
        // 处理：读取磁盘同 RVA 基线，写入磁盘字节、差异状态、路径和详情文本。
        // 返回：无返回值；读取失败时仅记录中文原因，不影响扫描结果展示。
        if (row == nullptr)
        {
            return;
        }

        const KernelHookDiskBaselineResult baselineResult =
            kernelHookReadDiskBaselineForInlineHook(*row, modulePathMap, fileCache);
        row->diskBaselineAvailable = baselineResult.available;
        row->diskBaselineDiffers = baselineResult.differsFromMemory;
        row->diskBaselineRva = baselineResult.rva;
        row->diskBaselineStatusText = baselineResult.statusText.trimmed().isEmpty()
            ? QStringLiteral("磁盘基线：未校验")
            : baselineResult.statusText;
        row->diskBaselinePathText = baselineResult.filePathText.trimmed().isEmpty()
            ? QStringLiteral("<不可用>")
            : QDir::toNativeSeparators(baselineResult.filePathText);
        row->diskBytes = baselineResult.bytes;
        row->diskBytesText = row->diskBaselineAvailable
            ? kernelHookBytesToText(row->diskBytes, baselineResult.byteCount)
            : QStringLiteral("<不可用>");
        row->detailText = buildInlineHookDetailText(*row);
    }

    KernelSsdtEntry convertShadowSsdtEntry(
        const ksword::ark::SsdtEntry& source,
        const ksword::ark::SsdtEnumResult& enumResult)
    {
        KernelSsdtEntry row{};
        row.serviceIndex = source.serviceIndex;
        row.flags = source.flags;
        row.zwRoutineAddress = source.zwRoutineAddress;
        row.serviceRoutineAddress = source.serviceRoutineAddress;
        row.serviceTableBase = enumResult.serviceTableBase;
        row.serviceNameText = QString::fromLocal8Bit(source.serviceName.data(), static_cast<int>(source.serviceName.size()));
        row.moduleNameText = QString::fromLocal8Bit(source.moduleName.data(), static_cast<int>(source.moduleName.size()));
        row.indexResolved = (row.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_INDEX_RESOLVED) != 0U;
        row.querySucceeded = true;

        QStringList statusParts;
        statusParts.push_back(QStringLiteral("Shadow/GUI表"));
        statusParts.push_back(row.indexResolved ? QStringLiteral("索引已解析") : QStringLiteral("索引未解析"));
        statusParts.push_back((row.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_STUB_EXPORT) != 0U
            ? QStringLiteral("Stub导出")
            : QStringLiteral("非Stub导出"));
        statusParts.push_back(row.serviceRoutineAddress != 0U
            ? QStringLiteral("表项已解析")
            : QStringLiteral("表项地址暂不可用"));
        row.statusText = statusParts.join(QStringLiteral(" | "));
        row.detailText = QStringLiteral(
            "SSSDT/Shadow SSDT 解析\n"
            "协议版本: %1\n"
            "总条目: %2\n"
            "返回条目: %3\n"
            "服务名: %4\n"
            "模块: %5\n"
            "服务索引: %6\n"
            "Stub地址: %7\n"
            "Shadow服务表基址: %8\n"
            "服务例程地址: %9\n"
            "驱动标志: 0x%10\n\n"
            "说明: 当前 R0 参考 System Informer 的 ksyscall 思路，从 win32k.sys 的 __win32kstub_* 和 win32u.dll 的 Nt* stub 中解析 syscall index。"
            "若服务例程地址为 0，表示本轮只完成 stub/index 解析，未解析 shadow service table 实际表项。")
            .arg(enumResult.version)
            .arg(enumResult.totalCount)
            .arg(enumResult.returnedCount)
            .arg(kernelHookSafeText(row.serviceNameText))
            .arg(kernelHookSafeText(row.moduleNameText))
            .arg(row.indexResolved ? QString::number(row.serviceIndex) : QStringLiteral("<未知>"))
            .arg(kernelHookFormatAddress(row.zwRoutineAddress))
            .arg(kernelHookFormatAddress(row.serviceTableBase))
            .arg(kernelHookFormatAddress(row.serviceRoutineAddress))
            .arg(static_cast<qulonglong>(row.flags), 8, 16, QChar('0'));
        return row;
    }

    KernelInlineHookEntry convertInlineHookEntry(const ksword::ark::KernelInlineHookEntry& source)
    {
        KernelInlineHookEntry row{};
        row.status = source.status;
        row.hookType = source.hookType;
        row.flags = source.flags;
        row.originalByteCount = source.originalByteCount;
        row.currentByteCount = source.currentByteCount;
        row.functionAddress = source.functionAddress;
        row.targetAddress = source.targetAddress;
        row.moduleBase = source.moduleBase;
        row.targetModuleBase = source.targetModuleBase;
        row.moduleNameText = QString::fromStdWString(source.moduleName);
        row.functionNameText = QString::fromLocal8Bit(source.functionName.data(), static_cast<int>(source.functionName.size()));
        row.targetModuleNameText = QString::fromStdWString(source.targetModuleName);
        row.hookTypeText = inlineHookTypeText(row.hookType);
        row.statusText = kernelHookStatusText(row.status);
        row.currentBytes = source.currentBytes;
        row.currentBytesText = kernelHookBytesToText(row.currentBytes, row.currentByteCount);
        row.observedBytes = source.expectedBytes;
        row.observedBytesText = kernelHookBytesToText(row.observedBytes, row.originalByteCount);
        row.diskBaselineAvailable = false;
        row.diskBaselineDiffers = false;
        row.diskBaselineRva = 0U;
        row.diskBaselineStatusText = QStringLiteral("磁盘基线：未校验");
        row.diskBaselinePathText = QStringLiteral("<未解析>");
        row.diskBytesText = QStringLiteral("<未获取>");
        row.detailText = buildInlineHookDetailText(row);
        return row;
    }

    KernelIatEatHookEntry convertIatEatHookEntry(const ksword::ark::KernelIatEatHookEntry& source)
    {
        KernelIatEatHookEntry row{};
        row.hookClass = source.hookClass;
        row.status = source.status;
        row.flags = source.flags;
        row.ordinal = source.ordinal;
        row.moduleBase = source.moduleBase;
        row.thunkAddress = source.thunkAddress;
        row.currentTarget = source.currentTarget;
        row.expectedTarget = source.expectedTarget;
        row.targetModuleBase = source.targetModuleBase;
        row.classText = iatEatClassText(row.hookClass);
        row.statusText = kernelHookStatusText(row.status);
        row.moduleNameText = QString::fromStdWString(source.moduleName);
        row.importModuleNameText = QString::fromStdWString(source.importModuleName);
        row.functionNameText = QString::fromLocal8Bit(source.functionName.data(), static_cast<int>(source.functionName.size()));
        row.targetModuleNameText = QString::fromStdWString(source.targetModuleName);
        row.detailText = QStringLiteral(
            "IAT/EAT Hook 检测详情\n"
            "类别: %1\n"
            "模块: %2\n"
            "导入模块: %3\n"
            "函数/序号: %4 / #%5\n"
            "Thunk/EAT项: %6\n"
            "当前目标: %7\n"
            "期望目标: %8\n"
            "目标模块: %9\n"
            "所属模块基址: %10\n"
            "目标模块基址: %11\n"
            "状态: %12\n"
            "标志: 0x%13\n\n"
            "说明: IAT 检测比较 thunk 当前目标是否仍落在声明导入模块内；EAT 检测导出 RVA 是否落在自身映像或转发导出区域内。")
            .arg(row.classText)
            .arg(kernelHookSafeText(row.moduleNameText))
            .arg(kernelHookSafeText(row.importModuleNameText, QStringLiteral("<不适用>")))
            .arg(kernelHookSafeText(row.functionNameText))
            .arg(row.ordinal)
            .arg(kernelHookFormatAddress(row.thunkAddress))
            .arg(kernelHookFormatAddress(row.currentTarget))
            .arg(kernelHookFormatAddress(row.expectedTarget))
            .arg(kernelHookSafeText(row.targetModuleNameText, QStringLiteral("<未解析>")))
            .arg(kernelHookFormatAddress(row.moduleBase))
            .arg(kernelHookFormatAddress(row.targetModuleBase))
            .arg(row.statusText)
            .arg(static_cast<qulonglong>(row.flags), 8, 16, QChar('0'));
        return row;
    }

    void setTableItem(QTableWidget* tableWidget, const int row, const int column, QTableWidgetItem* item)
    {
        // 作用：统一设置不可编辑 item，降低表格构建处重复代码。
        // 返回：无；空 table/item 时直接忽略。
        if (tableWidget == nullptr || item == nullptr)
        {
            delete item;
            return;
        }
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        tableWidget->setItem(row, column, item);
    }
}

void KernelDock::initializeShadowSsdtTab()
{
    if (m_shadowSsdtPage == nullptr || m_shadowSsdtLayout != nullptr)
    {
        return;
    }

    m_shadowSsdtLayout = new QVBoxLayout(m_shadowSsdtPage);
    m_shadowSsdtLayout->setContentsMargins(4, 4, 4, 4);
    m_shadowSsdtLayout->setSpacing(6);

    m_shadowSsdtToolLayout = new QHBoxLayout();
    m_shadowSsdtToolLayout->setContentsMargins(0, 0, 0, 0);
    m_shadowSsdtToolLayout->setSpacing(6);

    m_refreshShadowSsdtButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_shadowSsdtPage);
    m_refreshShadowSsdtButton->setToolTip(QStringLiteral("刷新 SSSDT/Shadow SSDT 解析结果"));
    m_refreshShadowSsdtButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshShadowSsdtButton->setFixedWidth(34);

    m_shadowSsdtFilterEdit = new QLineEdit(m_shadowSsdtPage);
    m_shadowSsdtFilterEdit->setPlaceholderText(QStringLiteral("按索引/服务名/模块/地址/状态筛选"));
    m_shadowSsdtFilterEdit->setClearButtonEnabled(true);
    m_shadowSsdtFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_shadowSsdtStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_shadowSsdtPage);
    m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_shadowSsdtToolLayout->addWidget(m_refreshShadowSsdtButton, 0);
    m_shadowSsdtToolLayout->addWidget(m_shadowSsdtFilterEdit, 1);
    m_shadowSsdtToolLayout->addWidget(m_shadowSsdtStatusLabel, 0);
    m_shadowSsdtLayout->addLayout(m_shadowSsdtToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_shadowSsdtPage);
    m_shadowSsdtLayout->addWidget(splitter, 1);

    m_shadowSsdtTable = new QTableWidget(splitter);
    m_shadowSsdtTable->setColumnCount(static_cast<int>(ShadowSsdtColumn::Count));
    m_shadowSsdtTable->setHorizontalHeaderLabels(QStringList{
        shadowSsdtColumnHeader(ShadowSsdtColumn::Index),
        shadowSsdtColumnHeader(ShadowSsdtColumn::ServiceName),
        shadowSsdtColumnHeader(ShadowSsdtColumn::StubAddress),
        shadowSsdtColumnHeader(ShadowSsdtColumn::ServiceAddress),
        shadowSsdtColumnHeader(ShadowSsdtColumn::Module),
        shadowSsdtColumnHeader(ShadowSsdtColumn::Status)
        });
    prepareTable(m_shadowSsdtTable);
    m_shadowSsdtTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(ShadowSsdtColumn::ServiceName), QHeaderView::Stretch);

    m_shadowSsdtDetailEditor = new CodeEditorWidget(splitter);
    m_shadowSsdtDetailEditor->setReadOnly(true);
    m_shadowSsdtDetailEditor->setText(QStringLiteral("请选择一条 SSSDT 记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshShadowSsdtButton, &QPushButton::clicked, this, [this]() {
        refreshShadowSsdtAsync();
    });
    connect(m_shadowSsdtFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildShadowSsdtTable(filterText.trimmed());
    });
    connect(m_shadowSsdtTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showShadowSsdtDetailByCurrentRow();
    });
    connect(m_shadowSsdtTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showShadowSsdtContextMenu(position);
    });
}

void KernelDock::initializeInlineHookTab()
{
    if (m_inlineHookPage == nullptr || m_inlineHookLayout != nullptr)
    {
        return;
    }

    m_inlineHookLayout = new QVBoxLayout(m_inlineHookPage);
    m_inlineHookLayout->setContentsMargins(4, 4, 4, 4);
    m_inlineHookLayout->setSpacing(6);

    m_inlineHookToolLayout = new QHBoxLayout();
    m_inlineHookToolLayout->setContentsMargins(0, 0, 0, 0);
    m_inlineHookToolLayout->setSpacing(6);

    m_refreshInlineHookButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_inlineHookPage);
    m_refreshInlineHookButton->setToolTip(QStringLiteral("扫描内核模块导出函数 Inline Hook"));
    m_refreshInlineHookButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshInlineHookButton->setFixedWidth(34);

    m_patchInlineHookButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("NOP 摘除选中"), m_inlineHookPage);
    m_patchInlineHookButton->setToolTip(QStringLiteral("对当前选中 Hook 先普通请求，再经强制确认后写入 NOP"));
    m_patchInlineHookButton->setStyleSheet(kernelHookButtonStyle());

    m_inlineHookModuleEdit = new QLineEdit(m_inlineHookPage);
    m_inlineHookModuleEdit->setPlaceholderText(QStringLiteral("模块过滤，如 ntoskrnl.exe / win32k.sys（留空扫描全部）"));
    m_inlineHookModuleEdit->setClearButtonEnabled(true);
    m_inlineHookModuleEdit->setStyleSheet(kernelHookInputStyle());

    m_inlineHookFilterEdit = new QLineEdit(m_inlineHookPage);
    m_inlineHookFilterEdit->setPlaceholderText(QStringLiteral("本地筛选：模块/函数/地址/类型/状态/字节"));
    m_inlineHookFilterEdit->setClearButtonEnabled(true);
    m_inlineHookFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_inlineHookIncludeCombo = new QComboBox(m_inlineHookPage);
    m_inlineHookIncludeCombo->addItem(QStringLiteral("仅可疑外跳"), QVariant::fromValue<qulonglong>(0ULL));
    m_inlineHookIncludeCombo->addItem(QStringLiteral("可疑 + 模块内跳转"), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL));
    m_inlineHookIncludeCombo->addItem(QStringLiteral("包含干净项"), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN));
    m_inlineHookIncludeCombo->setToolTip(QStringLiteral("控制 R0 扫描结果返回范围，包含干净项会明显增多"));

    m_inlineHookStatusLabel = new QLabel(QStringLiteral("状态：等待扫描"), m_inlineHookPage);
    m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_inlineHookToolLayout->addWidget(m_refreshInlineHookButton, 0);
    m_inlineHookToolLayout->addWidget(m_patchInlineHookButton, 0);
    m_inlineHookToolLayout->addWidget(m_inlineHookModuleEdit, 2);
    m_inlineHookToolLayout->addWidget(m_inlineHookFilterEdit, 2);
    m_inlineHookToolLayout->addWidget(m_inlineHookIncludeCombo, 0);
    m_inlineHookToolLayout->addWidget(m_inlineHookStatusLabel, 0);
    m_inlineHookLayout->addLayout(m_inlineHookToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_inlineHookPage);
    m_inlineHookLayout->addWidget(splitter, 1);

    m_inlineHookTable = new QTableWidget(splitter);
    m_inlineHookTable->setColumnCount(static_cast<int>(InlineHookColumn::Count));
    m_inlineHookTable->setHorizontalHeaderLabels(QStringList{
        inlineHookColumnHeader(InlineHookColumn::Module),
        inlineHookColumnHeader(InlineHookColumn::Function),
        inlineHookColumnHeader(InlineHookColumn::FunctionAddress),
        inlineHookColumnHeader(InlineHookColumn::HookType),
        inlineHookColumnHeader(InlineHookColumn::TargetAddress),
        inlineHookColumnHeader(InlineHookColumn::TargetModule),
        inlineHookColumnHeader(InlineHookColumn::Status),
        inlineHookColumnHeader(InlineHookColumn::CurrentBytes),
        inlineHookColumnHeader(InlineHookColumn::DiskBytes),
        inlineHookColumnHeader(InlineHookColumn::DiskDiff)
        });
    prepareTable(m_inlineHookTable);
    m_inlineHookTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(InlineHookColumn::Function), QHeaderView::Stretch);

    m_inlineHookDetailEditor = new CodeEditorWidget(splitter);
    m_inlineHookDetailEditor->setReadOnly(true);
    m_inlineHookDetailEditor->setText(QStringLiteral("请选择一条 Inline Hook 记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshInlineHookButton, &QPushButton::clicked, this, [this]() {
        refreshInlineHooksAsync();
    });
    connect(m_patchInlineHookButton, &QPushButton::clicked, this, [this]() {
        patchSelectedInlineHookWithNop();
    });
    connect(m_inlineHookFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildInlineHookTable(filterText.trimmed());
    });
    connect(m_inlineHookTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showInlineHookDetailByCurrentRow();
    });
    connect(m_inlineHookTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showInlineHookContextMenu(position);
    });
}

void KernelDock::initializeIatEatHookTab()
{
    if (m_iatEatHookPage == nullptr || m_iatEatHookLayout != nullptr)
    {
        return;
    }

    m_iatEatHookLayout = new QVBoxLayout(m_iatEatHookPage);
    m_iatEatHookLayout->setContentsMargins(4, 4, 4, 4);
    m_iatEatHookLayout->setSpacing(6);

    m_iatEatHookToolLayout = new QHBoxLayout();
    m_iatEatHookToolLayout->setContentsMargins(0, 0, 0, 0);
    m_iatEatHookToolLayout->setSpacing(6);

    m_refreshIatEatHookButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_iatEatHookPage);
    m_refreshIatEatHookButton->setToolTip(QStringLiteral("扫描内核模块 IAT/EAT Hook"));
    m_refreshIatEatHookButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshIatEatHookButton->setFixedWidth(34);

    m_iatEatHookModuleEdit = new QLineEdit(m_iatEatHookPage);
    m_iatEatHookModuleEdit->setPlaceholderText(QStringLiteral("模块过滤，如 ntoskrnl.exe / fltmgr.sys（留空扫描全部）"));
    m_iatEatHookModuleEdit->setClearButtonEnabled(true);
    m_iatEatHookModuleEdit->setStyleSheet(kernelHookInputStyle());

    m_iatEatHookFilterEdit = new QLineEdit(m_iatEatHookPage);
    m_iatEatHookFilterEdit->setPlaceholderText(QStringLiteral("本地筛选：类别/模块/导入模块/函数/地址/状态"));
    m_iatEatHookFilterEdit->setClearButtonEnabled(true);
    m_iatEatHookFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_iatEatHookIncludeCombo = new QComboBox(m_iatEatHookPage);
    m_iatEatHookIncludeCombo->addItem(QStringLiteral("IAT + EAT 可疑项"), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS));
    m_iatEatHookIncludeCombo->addItem(QStringLiteral("仅 IAT 可疑项"), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS));
    m_iatEatHookIncludeCombo->addItem(QStringLiteral("仅 EAT 可疑项"), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS));
    m_iatEatHookIncludeCombo->addItem(QStringLiteral("IAT + EAT + 干净项"), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN));
    m_iatEatHookIncludeCombo->setToolTip(QStringLiteral("控制 R0 扫描 IAT/EAT 范围，包含干净项会明显增多"));

    m_iatEatHookStatusLabel = new QLabel(QStringLiteral("状态：等待扫描"), m_iatEatHookPage);
    m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_iatEatHookToolLayout->addWidget(m_refreshIatEatHookButton, 0);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookModuleEdit, 2);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookFilterEdit, 2);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookIncludeCombo, 0);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookStatusLabel, 0);
    m_iatEatHookLayout->addLayout(m_iatEatHookToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_iatEatHookPage);
    m_iatEatHookLayout->addWidget(splitter, 1);

    m_iatEatHookTable = new QTableWidget(splitter);
    m_iatEatHookTable->setColumnCount(static_cast<int>(IatEatHookColumn::Count));
    m_iatEatHookTable->setHorizontalHeaderLabels(QStringList{
        iatEatColumnHeader(IatEatHookColumn::Class),
        iatEatColumnHeader(IatEatHookColumn::Module),
        iatEatColumnHeader(IatEatHookColumn::ImportModule),
        iatEatColumnHeader(IatEatHookColumn::Function),
        iatEatColumnHeader(IatEatHookColumn::ThunkAddress),
        iatEatColumnHeader(IatEatHookColumn::CurrentTarget),
        iatEatColumnHeader(IatEatHookColumn::ExpectedTarget),
        iatEatColumnHeader(IatEatHookColumn::TargetModule),
        iatEatColumnHeader(IatEatHookColumn::Status)
        });
    prepareTable(m_iatEatHookTable);
    m_iatEatHookTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(IatEatHookColumn::Function), QHeaderView::Stretch);

    m_iatEatHookDetailEditor = new CodeEditorWidget(splitter);
    m_iatEatHookDetailEditor->setReadOnly(true);
    m_iatEatHookDetailEditor->setText(QStringLiteral("请选择一条 IAT/EAT 记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshIatEatHookButton, &QPushButton::clicked, this, [this]() {
        refreshIatEatHooksAsync();
    });
    connect(m_iatEatHookFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildIatEatHookTable(filterText.trimmed());
    });
    connect(m_iatEatHookTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showIatEatHookDetailByCurrentRow();
    });
    connect(m_iatEatHookTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showIatEatHookContextMenu(position);
    });
}

void KernelDock::refreshShadowSsdtAsync()
{
    if (m_shadowSsdtRefreshRunning.exchange(true))
    {
        return;
    }

    if (m_refreshShadowSsdtButton != nullptr)
    {
        m_refreshShadowSsdtButton->setEnabled(false);
    }
    if (m_shadowSsdtStatusLabel != nullptr)
    {
        m_shadowSsdtStatusLabel->setText(QStringLiteral("状态：SSSDT 解析中..."));
        m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::PrimaryBlueHex));
    }

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelSsdtEntry> resultRows;
        QString errorText;
        std::uint32_t totalCount = 0U;
        std::uint32_t returnedCount = 0U;
        bool success = false;

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::SsdtEnumResult enumResult = driverClient.enumerateShadowSsdt(
            KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED);
        success = enumResult.io.ok;
        if (success)
        {
            totalCount = enumResult.totalCount;
            returnedCount = enumResult.returnedCount;
            resultRows.reserve(enumResult.entries.size());
            for (const ksword::ark::SsdtEntry& sourceEntry : enumResult.entries)
            {
                resultRows.push_back(convertShadowSsdtEntry(sourceEntry, enumResult));
            }
            std::sort(resultRows.begin(), resultRows.end(), [](const KernelSsdtEntry& left, const KernelSsdtEntry& right) {
                if (left.indexResolved != right.indexResolved)
                {
                    return left.indexResolved && !right.indexResolved;
                }
                if (left.serviceIndex != right.serviceIndex)
                {
                    return left.serviceIndex < right.serviceIndex;
                }
                return QString::compare(left.serviceNameText, right.serviceNameText, Qt::CaseInsensitive) < 0;
            });
        }
        else
        {
            errorText = QStringLiteral("SSSDT 解析 IOCTL 调用失败。\nWin32=%1\n详情=%2")
                .arg(enumResult.io.win32Error)
                .arg(QString::fromStdString(enumResult.io.message));
        }

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, totalCount, returnedCount, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_shadowSsdtRefreshRunning.store(false);
            if (guardThis->m_refreshShadowSsdtButton != nullptr)
            {
                guardThis->m_refreshShadowSsdtButton->setEnabled(true);
            }

            if (!success)
            {
                guardThis->m_shadowSsdtStatusLabel->setText(QStringLiteral("状态：解析失败"));
                guardThis->m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_shadowSsdtDetailEditor->setText(errorText);
                return;
            }

            guardThis->m_shadowSsdtRows = std::move(resultRows);
            guardThis->rebuildShadowSsdtTable(guardThis->m_shadowSsdtFilterEdit->text().trimmed());
            guardThis->m_shadowSsdtStatusLabel->setText(
                QStringLiteral("状态：已解析 %1/%2 项，显示 %3 项")
                .arg(returnedCount)
                .arg(totalCount)
                .arg(guardThis->m_shadowSsdtRows.size()));
            guardThis->m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(QStringLiteral("#3A8F3A")));

            if (guardThis->m_shadowSsdtTable->rowCount() > 0)
            {
                guardThis->m_shadowSsdtTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_shadowSsdtDetailEditor->setText(QStringLiteral("当前环境未返回 SSSDT stub 解析结果。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshInlineHooksAsync()
{
    if (m_inlineHookRefreshRunning.exchange(true))
    {
        return;
    }

    if (m_refreshInlineHookButton != nullptr)
    {
        m_refreshInlineHookButton->setEnabled(false);
    }
    if (m_inlineHookStatusLabel != nullptr)
    {
        m_inlineHookStatusLabel->setText(QStringLiteral("状态：Inline Hook 扫描中..."));
        m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::PrimaryBlueHex));
    }

    const unsigned long flags = m_inlineHookIncludeCombo != nullptr
        ? static_cast<unsigned long>(m_inlineHookIncludeCombo->currentData().toULongLong())
        : 0UL;
    const QString moduleFilterText = m_inlineHookModuleEdit != nullptr
        ? m_inlineHookModuleEdit->text().trimmed()
        : QString();

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis, flags, moduleFilterText]() {
        std::vector<KernelInlineHookEntry> resultRows;
        QString errorText;
        std::uint32_t totalCount = 0U;
        std::uint32_t moduleCount = 0U;
        long lastStatus = 0;
        bool success = false;

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::KernelInlineHookScanResult scanResult = driverClient.scanInlineHooks(
            flags,
            KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES,
            moduleFilterText.toStdWString());
        success = scanResult.io.ok;
        if (success)
        {
            const KernelHookModulePathMap modulePathMap = kernelHookQueryLoadedModulePathMap();
            KernelHookDiskFileCache diskFileCache;
            totalCount = scanResult.totalCount;
            moduleCount = scanResult.moduleCount;
            lastStatus = scanResult.lastStatus;
            resultRows.reserve(scanResult.entries.size());
            for (const ksword::ark::KernelInlineHookEntry& sourceEntry : scanResult.entries)
            {
                KernelInlineHookEntry row = convertInlineHookEntry(sourceEntry);
                applyDiskBaselineToInlineHookEntry(&row, modulePathMap, &diskFileCache);
                resultRows.push_back(std::move(row));
            }
        }
        else
        {
            errorText = QStringLiteral("Inline Hook 扫描 IOCTL 调用失败。\nWin32=%1\n详情=%2")
                .arg(scanResult.io.win32Error)
                .arg(QString::fromStdString(scanResult.io.message));
        }

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, totalCount, moduleCount, lastStatus, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_inlineHookRefreshRunning.store(false);
            if (guardThis->m_refreshInlineHookButton != nullptr)
            {
                guardThis->m_refreshInlineHookButton->setEnabled(true);
            }

            if (!success)
            {
                guardThis->m_inlineHookStatusLabel->setText(QStringLiteral("状态：扫描失败"));
                guardThis->m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_inlineHookDetailEditor->setText(errorText);
                return;
            }

            std::size_t suspiciousCount = 0U;
            std::size_t internalCount = 0U;
            for (const KernelInlineHookEntry& entry : resultRows)
            {
                if (entry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS)
                {
                    ++suspiciousCount;
                }
                else if (entry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH)
                {
                    ++internalCount;
                }
            }

            guardThis->m_inlineHookRows = std::move(resultRows);
            guardThis->rebuildInlineHookTable(guardThis->m_inlineHookFilterEdit->text().trimmed());
            guardThis->m_inlineHookStatusLabel->setText(
                QStringLiteral("状态：模块=%1，总命中=%2，返回=%3，可疑=%4，内部跳转=%5，Last=%6")
                .arg(moduleCount)
                .arg(totalCount)
                .arg(guardThis->m_inlineHookRows.size())
                .arg(suspiciousCount)
                .arg(internalCount)
                .arg(kernelHookFormatNtStatus(lastStatus)));
            guardThis->m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
                suspiciousCount == 0U ? QStringLiteral("#3A8F3A") : QStringLiteral("#B23A3A")));

            if (guardThis->m_inlineHookTable->rowCount() > 0)
            {
                guardThis->m_inlineHookTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_inlineHookDetailEditor->setText(QStringLiteral("当前过滤条件下未返回 Inline Hook 记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshIatEatHooksAsync()
{
    if (m_iatEatHookRefreshRunning.exchange(true))
    {
        return;
    }

    if (m_refreshIatEatHookButton != nullptr)
    {
        m_refreshIatEatHookButton->setEnabled(false);
    }
    if (m_iatEatHookStatusLabel != nullptr)
    {
        m_iatEatHookStatusLabel->setText(QStringLiteral("状态：IAT/EAT 扫描中..."));
        m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::PrimaryBlueHex));
    }

    const unsigned long flags = m_iatEatHookIncludeCombo != nullptr
        ? static_cast<unsigned long>(m_iatEatHookIncludeCombo->currentData().toULongLong())
        : (KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS);
    const QString moduleFilterText = m_iatEatHookModuleEdit != nullptr
        ? m_iatEatHookModuleEdit->text().trimmed()
        : QString();

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis, flags, moduleFilterText]() {
        std::vector<KernelIatEatHookEntry> resultRows;
        QString errorText;
        std::uint32_t totalCount = 0U;
        std::uint32_t moduleCount = 0U;
        long lastStatus = 0;
        bool success = false;

        const ksword::ark::DriverClient driverClient;
        const ksword::ark::KernelIatEatHookScanResult scanResult = driverClient.enumerateIatEatHooks(
            flags,
            KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES,
            moduleFilterText.toStdWString());
        success = scanResult.io.ok;
        if (success)
        {
            totalCount = scanResult.totalCount;
            moduleCount = scanResult.moduleCount;
            lastStatus = scanResult.lastStatus;
            resultRows.reserve(scanResult.entries.size());
            for (const ksword::ark::KernelIatEatHookEntry& sourceEntry : scanResult.entries)
            {
                resultRows.push_back(convertIatEatHookEntry(sourceEntry));
            }
        }
        else
        {
            errorText = QStringLiteral("IAT/EAT 扫描 IOCTL 调用失败。\nWin32=%1\n详情=%2")
                .arg(scanResult.io.win32Error)
                .arg(QString::fromStdString(scanResult.io.message));
        }

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, totalCount, moduleCount, lastStatus, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_iatEatHookRefreshRunning.store(false);
            if (guardThis->m_refreshIatEatHookButton != nullptr)
            {
                guardThis->m_refreshIatEatHookButton->setEnabled(true);
            }

            if (!success)
            {
                guardThis->m_iatEatHookStatusLabel->setText(QStringLiteral("状态：扫描失败"));
                guardThis->m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_iatEatHookDetailEditor->setText(errorText);
                return;
            }

            std::size_t suspiciousCount = 0U;
            for (const KernelIatEatHookEntry& entry : resultRows)
            {
                if (entry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS)
                {
                    ++suspiciousCount;
                }
            }

            guardThis->m_iatEatHookRows = std::move(resultRows);
            guardThis->rebuildIatEatHookTable(guardThis->m_iatEatHookFilterEdit->text().trimmed());
            guardThis->m_iatEatHookStatusLabel->setText(
                QStringLiteral("状态：模块=%1，总命中=%2，返回=%3，可疑=%4，Last=%5")
                .arg(moduleCount)
                .arg(totalCount)
                .arg(guardThis->m_iatEatHookRows.size())
                .arg(suspiciousCount)
                .arg(kernelHookFormatNtStatus(lastStatus)));
            guardThis->m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
                suspiciousCount == 0U ? QStringLiteral("#3A8F3A") : QStringLiteral("#B23A3A")));

            if (guardThis->m_iatEatHookTable->rowCount() > 0)
            {
                guardThis->m_iatEatHookTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_iatEatHookDetailEditor->setText(QStringLiteral("当前过滤条件下未返回 IAT/EAT 记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildShadowSsdtTable(const QString& filterKeyword)
{
    if (m_shadowSsdtTable == nullptr)
    {
        return;
    }

    m_shadowSsdtTable->setSortingEnabled(false);
    m_shadowSsdtTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_shadowSsdtRows.size(); ++sourceIndex)
    {
        const KernelSsdtEntry& entry = m_shadowSsdtRows[sourceIndex];
        QStringList matchFields;
        for (int column = 0; column < static_cast<int>(ShadowSsdtColumn::Count); ++column)
        {
            matchFields.push_back(shadowSsdtColumnText(entry, static_cast<ShadowSsdtColumn>(column)));
        }
        const bool matched = filterKeyword.isEmpty() || matchFields.join(' ').contains(filterKeyword, Qt::CaseInsensitive) || entry.detailText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_shadowSsdtTable->rowCount();
        m_shadowSsdtTable->insertRow(rowIndex);
        for (int column = 0; column < static_cast<int>(ShadowSsdtColumn::Count); ++column)
        {
            auto* item = new QTableWidgetItem(shadowSsdtColumnText(entry, static_cast<ShadowSsdtColumn>(column)));
            if (column == 0)
            {
                item->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
            }
            if (column == static_cast<int>(ShadowSsdtColumn::Status) && !entry.indexResolved)
            {
                item->setForeground(QBrush(QColor(QStringLiteral("#D77A00"))));
            }
            setTableItem(m_shadowSsdtTable, rowIndex, column, item);
        }
    }

    m_shadowSsdtTable->setSortingEnabled(true);
}

void KernelDock::rebuildInlineHookTable(const QString& filterKeyword)
{
    if (m_inlineHookTable == nullptr)
    {
        return;
    }

    m_inlineHookTable->setSortingEnabled(false);
    m_inlineHookTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_inlineHookRows.size(); ++sourceIndex)
    {
        const KernelInlineHookEntry& entry = m_inlineHookRows[sourceIndex];
        QStringList matchFields;
        for (int column = 0; column < static_cast<int>(InlineHookColumn::Count); ++column)
        {
            matchFields.push_back(inlineHookColumnText(entry, static_cast<InlineHookColumn>(column)));
        }
        const bool matched = filterKeyword.isEmpty() || matchFields.join(' ').contains(filterKeyword, Qt::CaseInsensitive) || entry.detailText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_inlineHookTable->rowCount();
        m_inlineHookTable->insertRow(rowIndex);
        for (int column = 0; column < static_cast<int>(InlineHookColumn::Count); ++column)
        {
            auto* item = new QTableWidgetItem(inlineHookColumnText(entry, static_cast<InlineHookColumn>(column)));
            if (column == 0)
            {
                item->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
            }
            if (column == static_cast<int>(InlineHookColumn::Status))
            {
                item->setForeground(QBrush(statusColor(entry.status)));
            }
            if (column == static_cast<int>(InlineHookColumn::DiskDiff))
            {
                if (!entry.diskBaselineAvailable)
                {
                    item->setForeground(QBrush(QColor(QStringLiteral("#D77A00"))));
                }
                else
                {
                    item->setForeground(QBrush(entry.diskBaselineDiffers
                        ? QColor(QStringLiteral("#B23A3A"))
                        : QColor(QStringLiteral("#3A8F3A"))));
                }
            }
            setTableItem(m_inlineHookTable, rowIndex, column, item);
        }
    }

    m_inlineHookTable->setSortingEnabled(true);
}

void KernelDock::rebuildIatEatHookTable(const QString& filterKeyword)
{
    if (m_iatEatHookTable == nullptr)
    {
        return;
    }

    m_iatEatHookTable->setSortingEnabled(false);
    m_iatEatHookTable->setRowCount(0);

    for (std::size_t sourceIndex = 0U; sourceIndex < m_iatEatHookRows.size(); ++sourceIndex)
    {
        const KernelIatEatHookEntry& entry = m_iatEatHookRows[sourceIndex];
        QStringList matchFields;
        for (int column = 0; column < static_cast<int>(IatEatHookColumn::Count); ++column)
        {
            matchFields.push_back(iatEatColumnText(entry, static_cast<IatEatHookColumn>(column)));
        }
        const bool matched = filterKeyword.isEmpty() || matchFields.join(' ').contains(filterKeyword, Qt::CaseInsensitive) || entry.detailText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_iatEatHookTable->rowCount();
        m_iatEatHookTable->insertRow(rowIndex);
        for (int column = 0; column < static_cast<int>(IatEatHookColumn::Count); ++column)
        {
            auto* item = new QTableWidgetItem(iatEatColumnText(entry, static_cast<IatEatHookColumn>(column)));
            if (column == 0)
            {
                item->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
            }
            if (column == static_cast<int>(IatEatHookColumn::Status))
            {
                item->setForeground(QBrush(statusColor(entry.status)));
            }
            setTableItem(m_iatEatHookTable, rowIndex, column, item);
        }
    }

    m_iatEatHookTable->setSortingEnabled(true);
}

bool KernelDock::currentShadowSsdtSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_shadowSsdtTable == nullptr || m_shadowSsdtTable->currentRow() < 0)
    {
        return false;
    }
    const QTableWidgetItem* item = m_shadowSsdtTable->item(m_shadowSsdtTable->currentRow(), 0);
    if (item == nullptr)
    {
        return false;
    }
    sourceIndexOut = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_shadowSsdtRows.size();
}

bool KernelDock::currentInlineHookSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_inlineHookTable == nullptr || m_inlineHookTable->currentRow() < 0)
    {
        return false;
    }
    const QTableWidgetItem* item = m_inlineHookTable->item(m_inlineHookTable->currentRow(), 0);
    if (item == nullptr)
    {
        return false;
    }
    sourceIndexOut = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_inlineHookRows.size();
}

bool KernelDock::currentIatEatHookSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_iatEatHookTable == nullptr || m_iatEatHookTable->currentRow() < 0)
    {
        return false;
    }
    const QTableWidgetItem* item = m_iatEatHookTable->item(m_iatEatHookTable->currentRow(), 0);
    if (item == nullptr)
    {
        return false;
    }
    sourceIndexOut = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_iatEatHookRows.size();
}

const KernelSsdtEntry* KernelDock::currentShadowSsdtEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentShadowSsdtSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_shadowSsdtRows[sourceIndex];
}

const KernelInlineHookEntry* KernelDock::currentInlineHookEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentInlineHookSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_inlineHookRows[sourceIndex];
}

const KernelIatEatHookEntry* KernelDock::currentIatEatHookEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentIatEatHookSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_iatEatHookRows[sourceIndex];
}

void KernelDock::showShadowSsdtDetailByCurrentRow()
{
    if (m_shadowSsdtDetailEditor == nullptr)
    {
        return;
    }
    const KernelSsdtEntry* entry = currentShadowSsdtEntry();
    m_shadowSsdtDetailEditor->setText(entry != nullptr ? entry->detailText : QStringLiteral("请选择一条 SSSDT 记录查看详情。"));
}

void KernelDock::showInlineHookDetailByCurrentRow()
{
    if (m_inlineHookDetailEditor == nullptr)
    {
        return;
    }
    const KernelInlineHookEntry* entry = currentInlineHookEntry();
    m_inlineHookDetailEditor->setText(entry != nullptr ? entry->detailText : QStringLiteral("请选择一条 Inline Hook 记录查看详情。"));
}

void KernelDock::showIatEatHookDetailByCurrentRow()
{
    if (m_iatEatHookDetailEditor == nullptr)
    {
        return;
    }
    const KernelIatEatHookEntry* entry = currentIatEatHookEntry();
    m_iatEatHookDetailEditor->setText(entry != nullptr ? entry->detailText : QStringLiteral("请选择一条 IAT/EAT 记录查看详情。"));
}

void KernelDock::showShadowSsdtContextMenu(const QPoint& localPosition)
{
    if (m_shadowSsdtTable == nullptr)
    {
        return;
    }

    QTableWidgetItem* clickedItem = m_shadowSsdtTable->itemAt(localPosition);
    const int clickedRow = clickedItem != nullptr ? clickedItem->row() : -1;
    const int clickedColumn = m_shadowSsdtTable->columnAt(localPosition.x());
    if (clickedItem != nullptr && !clickedItem->isSelected())
    {
        m_shadowSsdtTable->clearSelection();
        m_shadowSsdtTable->setCurrentItem(clickedItem);
        m_shadowSsdtTable->selectRow(clickedRow);
    }

    const std::vector<std::size_t> selectedIndices = selectedSourceIndices(m_shadowSsdtTable, m_shadowSsdtRows, clickedRow >= 0 ? clickedRow : m_shadowSsdtTable->currentRow());
    const bool hasSelection = !selectedIndices.empty();

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新 SSSDT"));
    menu.addSeparator();
    QMenu* copyMenu = menu.addMenu(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制"));
    QAction* copyCurrentColumnAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制当前列（选中行）"));
    QAction* copyRowsAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制选中行（TSV）"));
    QAction* copyRowsWithHeaderAction = copyMenu->addAction(QStringLiteral("复制表头+选中行（TSV）"));
    QAction* copyDetailAction = copyMenu->addAction(QStringLiteral("复制详情（选中行）"));
    copyMenu->addSeparator();
    QMenu* columnMenu = copyMenu->addMenu(QStringLiteral("复制指定栏目（选中行）"));
    for (int column = 0; column < static_cast<int>(ShadowSsdtColumn::Count); ++column)
    {
        QAction* action = columnMenu->addAction(shadowSsdtColumnHeader(static_cast<ShadowSsdtColumn>(column)));
        action->setData(column);
    }
    copyCurrentColumnAction->setEnabled(hasSelection);
    copyRowsAction->setEnabled(hasSelection);
    copyRowsWithHeaderAction->setEnabled(hasSelection);
    copyDetailAction->setEnabled(hasSelection);
    columnMenu->setEnabled(hasSelection);

    QAction* selectedAction = menu.exec(m_shadowSsdtTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }
    if (selectedAction == refreshAction)
    {
        refreshShadowSsdtAsync();
        return;
    }
    if (!hasSelection)
    {
        return;
    }

    const auto copyColumn = [this, &selectedIndices](const ShadowSsdtColumn column) {
        QStringList values;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            values.push_back(shadowSsdtColumnText(m_shadowSsdtRows[sourceIndex], column));
        }
        kernelHookCopyTextToClipboard(values.join('\n'));
    };

    if (selectedAction == copyCurrentColumnAction)
    {
        int column = clickedColumn >= 0 ? clickedColumn : m_shadowSsdtTable->currentColumn();
        if (column < 0 || column >= static_cast<int>(ShadowSsdtColumn::Count))
        {
            column = 0;
        }
        copyColumn(static_cast<ShadowSsdtColumn>(column));
        return;
    }
    if (selectedAction == copyRowsAction || selectedAction == copyRowsWithHeaderAction)
    {
        QStringList lines;
        if (selectedAction == copyRowsWithHeaderAction)
        {
            lines.push_back(headerAsTsv(static_cast<int>(ShadowSsdtColumn::Count), [](const int column) {
                return shadowSsdtColumnHeader(static_cast<ShadowSsdtColumn>(column));
            }));
        }
        for (const std::size_t sourceIndex : selectedIndices)
        {
            lines.push_back(shadowSsdtRowAsTsv(m_shadowSsdtRows[sourceIndex]));
        }
        kernelHookCopyTextToClipboard(lines.join('\n'));
        return;
    }
    if (selectedAction == copyDetailAction)
    {
        QStringList details;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            details.push_back(m_shadowSsdtRows[sourceIndex].detailText);
        }
        kernelHookCopyTextToClipboard(details.join(QStringLiteral("\n\n---\n\n")));
        return;
    }

    if (columnMenu->actions().contains(selectedAction))
    {
        const int column = selectedAction->data().toInt();
        if (column >= 0 && column < static_cast<int>(ShadowSsdtColumn::Count))
        {
            copyColumn(static_cast<ShadowSsdtColumn>(column));
        }
    }
}

void KernelDock::showInlineHookContextMenu(const QPoint& localPosition)
{
    if (m_inlineHookTable == nullptr)
    {
        return;
    }

    QTableWidgetItem* clickedItem = m_inlineHookTable->itemAt(localPosition);
    const int clickedRow = clickedItem != nullptr ? clickedItem->row() : -1;
    const int clickedColumn = m_inlineHookTable->columnAt(localPosition.x());
    if (clickedItem != nullptr && !clickedItem->isSelected())
    {
        m_inlineHookTable->clearSelection();
        m_inlineHookTable->setCurrentItem(clickedItem);
        m_inlineHookTable->selectRow(clickedRow);
    }

    const std::vector<std::size_t> selectedIndices = selectedSourceIndices(m_inlineHookTable, m_inlineHookRows, clickedRow >= 0 ? clickedRow : m_inlineHookTable->currentRow());
    const bool hasSelection = !selectedIndices.empty();

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("重新扫描 Inline Hook"));
    QAction* patchAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("NOP 摘除当前 Hook"));
    patchAction->setEnabled(hasSelection);
    menu.addSeparator();
    QMenu* copyMenu = menu.addMenu(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制"));
    QAction* copyCurrentColumnAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制当前列（选中行）"));
    QAction* copyRowsAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制选中行（TSV）"));
    QAction* copyRowsWithHeaderAction = copyMenu->addAction(QStringLiteral("复制表头+选中行（TSV）"));
    QAction* copyDetailAction = copyMenu->addAction(QStringLiteral("复制详情（选中行）"));
    copyMenu->addSeparator();
    QMenu* columnMenu = copyMenu->addMenu(QStringLiteral("复制指定栏目（选中行）"));
    for (int column = 0; column < static_cast<int>(InlineHookColumn::Count); ++column)
    {
        QAction* action = columnMenu->addAction(inlineHookColumnHeader(static_cast<InlineHookColumn>(column)));
        action->setData(column);
    }
    copyCurrentColumnAction->setEnabled(hasSelection);
    copyRowsAction->setEnabled(hasSelection);
    copyRowsWithHeaderAction->setEnabled(hasSelection);
    copyDetailAction->setEnabled(hasSelection);
    columnMenu->setEnabled(hasSelection);

    QAction* selectedAction = menu.exec(m_inlineHookTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }
    if (selectedAction == refreshAction)
    {
        refreshInlineHooksAsync();
        return;
    }
    if (selectedAction == patchAction)
    {
        patchSelectedInlineHookWithNop();
        return;
    }
    if (!hasSelection)
    {
        return;
    }

    const auto copyColumn = [this, &selectedIndices](const InlineHookColumn column) {
        QStringList values;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            values.push_back(inlineHookColumnText(m_inlineHookRows[sourceIndex], column));
        }
        kernelHookCopyTextToClipboard(values.join('\n'));
    };

    if (selectedAction == copyCurrentColumnAction)
    {
        int column = clickedColumn >= 0 ? clickedColumn : m_inlineHookTable->currentColumn();
        if (column < 0 || column >= static_cast<int>(InlineHookColumn::Count))
        {
            column = 0;
        }
        copyColumn(static_cast<InlineHookColumn>(column));
        return;
    }
    if (selectedAction == copyRowsAction || selectedAction == copyRowsWithHeaderAction)
    {
        QStringList lines;
        if (selectedAction == copyRowsWithHeaderAction)
        {
            lines.push_back(headerAsTsv(static_cast<int>(InlineHookColumn::Count), [](const int column) {
                return inlineHookColumnHeader(static_cast<InlineHookColumn>(column));
            }));
        }
        for (const std::size_t sourceIndex : selectedIndices)
        {
            lines.push_back(inlineHookRowAsTsv(m_inlineHookRows[sourceIndex]));
        }
        kernelHookCopyTextToClipboard(lines.join('\n'));
        return;
    }
    if (selectedAction == copyDetailAction)
    {
        QStringList details;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            details.push_back(m_inlineHookRows[sourceIndex].detailText);
        }
        kernelHookCopyTextToClipboard(details.join(QStringLiteral("\n\n---\n\n")));
        return;
    }
    if (columnMenu->actions().contains(selectedAction))
    {
        const int column = selectedAction->data().toInt();
        if (column >= 0 && column < static_cast<int>(InlineHookColumn::Count))
        {
            copyColumn(static_cast<InlineHookColumn>(column));
        }
    }
}

void KernelDock::showIatEatHookContextMenu(const QPoint& localPosition)
{
    if (m_iatEatHookTable == nullptr)
    {
        return;
    }

    QTableWidgetItem* clickedItem = m_iatEatHookTable->itemAt(localPosition);
    const int clickedRow = clickedItem != nullptr ? clickedItem->row() : -1;
    const int clickedColumn = m_iatEatHookTable->columnAt(localPosition.x());
    if (clickedItem != nullptr && !clickedItem->isSelected())
    {
        m_iatEatHookTable->clearSelection();
        m_iatEatHookTable->setCurrentItem(clickedItem);
        m_iatEatHookTable->selectRow(clickedRow);
    }

    const std::vector<std::size_t> selectedIndices = selectedSourceIndices(m_iatEatHookTable, m_iatEatHookRows, clickedRow >= 0 ? clickedRow : m_iatEatHookTable->currentRow());
    const bool hasSelection = !selectedIndices.empty();

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("重新扫描 IAT/EAT"));
    menu.addSeparator();
    QMenu* copyMenu = menu.addMenu(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制"));
    QAction* copyCurrentColumnAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制当前列（选中行）"));
    QAction* copyRowsAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), QStringLiteral("复制选中行（TSV）"));
    QAction* copyRowsWithHeaderAction = copyMenu->addAction(QStringLiteral("复制表头+选中行（TSV）"));
    QAction* copyDetailAction = copyMenu->addAction(QStringLiteral("复制详情（选中行）"));
    copyMenu->addSeparator();
    QMenu* columnMenu = copyMenu->addMenu(QStringLiteral("复制指定栏目（选中行）"));
    for (int column = 0; column < static_cast<int>(IatEatHookColumn::Count); ++column)
    {
        QAction* action = columnMenu->addAction(iatEatColumnHeader(static_cast<IatEatHookColumn>(column)));
        action->setData(column);
    }
    copyCurrentColumnAction->setEnabled(hasSelection);
    copyRowsAction->setEnabled(hasSelection);
    copyRowsWithHeaderAction->setEnabled(hasSelection);
    copyDetailAction->setEnabled(hasSelection);
    columnMenu->setEnabled(hasSelection);

    QAction* selectedAction = menu.exec(m_iatEatHookTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }
    if (selectedAction == refreshAction)
    {
        refreshIatEatHooksAsync();
        return;
    }
    if (!hasSelection)
    {
        return;
    }

    const auto copyColumn = [this, &selectedIndices](const IatEatHookColumn column) {
        QStringList values;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            values.push_back(iatEatColumnText(m_iatEatHookRows[sourceIndex], column));
        }
        kernelHookCopyTextToClipboard(values.join('\n'));
    };

    if (selectedAction == copyCurrentColumnAction)
    {
        int column = clickedColumn >= 0 ? clickedColumn : m_iatEatHookTable->currentColumn();
        if (column < 0 || column >= static_cast<int>(IatEatHookColumn::Count))
        {
            column = 0;
        }
        copyColumn(static_cast<IatEatHookColumn>(column));
        return;
    }
    if (selectedAction == copyRowsAction || selectedAction == copyRowsWithHeaderAction)
    {
        QStringList lines;
        if (selectedAction == copyRowsWithHeaderAction)
        {
            lines.push_back(headerAsTsv(static_cast<int>(IatEatHookColumn::Count), [](const int column) {
                return iatEatColumnHeader(static_cast<IatEatHookColumn>(column));
            }));
        }
        for (const std::size_t sourceIndex : selectedIndices)
        {
            lines.push_back(iatEatRowAsTsv(m_iatEatHookRows[sourceIndex]));
        }
        kernelHookCopyTextToClipboard(lines.join('\n'));
        return;
    }
    if (selectedAction == copyDetailAction)
    {
        QStringList details;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            details.push_back(m_iatEatHookRows[sourceIndex].detailText);
        }
        kernelHookCopyTextToClipboard(details.join(QStringLiteral("\n\n---\n\n")));
        return;
    }
    if (columnMenu->actions().contains(selectedAction))
    {
        const int column = selectedAction->data().toInt();
        if (column >= 0 && column < static_cast<int>(IatEatHookColumn::Count))
        {
            copyColumn(static_cast<IatEatHookColumn>(column));
        }
    }
}

void KernelDock::patchSelectedInlineHookWithNop()
{
    const KernelInlineHookEntry* entry = currentInlineHookEntry();
    if (entry == nullptr)
    {
        QMessageBox::warning(this, QStringLiteral("Inline Hook 摘除"), QStringLiteral("请先选择一条 Inline Hook 记录。"));
        return;
    }

    const std::uint32_t patchBytes = inlineHookPatchLength(entry->hookType, entry->currentByteCount);
    if (patchBytes == 0U)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("Inline Hook 摘除"),
            QStringLiteral("当前 Hook 类型不适合自动 NOP 摘除：%1").arg(entry->hookTypeText));
        return;
    }

    const QMessageBox::StandardButton firstConfirm = QMessageBox::warning(
        this,
        QStringLiteral("Inline Hook 摘除确认"),
        QStringLiteral("将对内核函数写入 NOP 补丁。\n\n模块: %1\n函数: %2\n地址: %3\n类型: %4\n字节数: %5\n\n普通请求会先提交给 R0，驱动预计会返回需要强制确认。是否继续？")
        .arg(kernelHookSafeText(entry->moduleNameText))
        .arg(kernelHookSafeText(entry->functionNameText))
        .arg(kernelHookFormatAddress(entry->functionAddress))
        .arg(entry->hookTypeText)
        .arg(patchBytes),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (firstConfirm != QMessageBox::Yes)
    {
        return;
    }

    ksword::ark::DriverClient driverClient;
    ksword::ark::KernelInlinePatchResult patchResult = driverClient.patchInlineHook(
        entry->functionAddress,
        KSWORD_ARK_INLINE_PATCH_MODE_NOP_BRANCH,
        patchBytes,
        entry->currentBytes,
        {},
        0UL);
    if (!patchResult.io.ok)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("Inline Hook 摘除"),
            QStringLiteral("普通摘除请求失败：\n%1").arg(QString::fromStdString(patchResult.io.message)));
        return;
    }

    if (patchResult.status == KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED)
    {
        QMessageBox warningBox(this);
        warningBox.setIcon(QMessageBox::Warning);
        warningBox.setWindowTitle(QStringLiteral("强制 Inline Hook 摘除"));
        warningBox.setText(QStringLiteral("R0 已拒绝普通内核补丁请求。"));
        warningBox.setInformativeText(
            QStringLiteral("目标函数: %1!%2\n地址: %3\n补丁长度: %4 字节\nR0状态: %5\nLastStatus: %6\n\n强制继续会修改内核代码页，只应在确认目标和字节快照无误时使用。")
            .arg(kernelHookSafeText(entry->moduleNameText))
            .arg(kernelHookSafeText(entry->functionNameText))
            .arg(kernelHookFormatAddress(entry->functionAddress))
            .arg(patchBytes)
            .arg(kernelHookStatusText(patchResult.status))
            .arg(kernelHookFormatNtStatus(patchResult.lastStatus)));
        warningBox.setStandardButtons(QMessageBox::Cancel);
        warningBox.setDefaultButton(QMessageBox::Cancel);
        QPushButton* forceButton = warningBox.addButton(QStringLiteral("强制继续"), QMessageBox::DestructiveRole);
        warningBox.exec();
        if (warningBox.clickedButton() != forceButton)
        {
            if (m_inlineHookStatusLabel != nullptr)
            {
                m_inlineHookStatusLabel->setText(QStringLiteral("状态：用户取消强制 Inline Hook 摘除"));
            }
            return;
        }

        patchResult = driverClient.patchInlineHook(
            entry->functionAddress,
            KSWORD_ARK_INLINE_PATCH_MODE_NOP_BRANCH,
            patchBytes,
            entry->currentBytes,
            {},
            KSWORD_ARK_KERNEL_PATCH_FLAG_FORCE);
    }

    const QString resultText = QStringLiteral(
        "Inline Hook 摘除结果\n"
        "函数: %1!%2\n"
        "地址: %3\n"
        "状态: %4\n"
        "写入字节: %5\n"
        "LastStatus: %6\n"
        "R3信息: %7")
        .arg(kernelHookSafeText(entry->moduleNameText))
        .arg(kernelHookSafeText(entry->functionNameText))
        .arg(kernelHookFormatAddress(entry->functionAddress))
        .arg(kernelHookStatusText(patchResult.status))
        .arg(patchResult.bytesPatched)
        .arg(kernelHookFormatNtStatus(patchResult.lastStatus))
        .arg(QString::fromStdString(patchResult.io.message));

    if (m_inlineHookDetailEditor != nullptr)
    {
        m_inlineHookDetailEditor->setText(resultText);
    }
    if (m_inlineHookStatusLabel != nullptr)
    {
        m_inlineHookStatusLabel->setText(QStringLiteral("状态：%1，写入 %2 字节")
            .arg(kernelHookStatusText(patchResult.status))
            .arg(patchResult.bytesPatched));
        m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
            patchResult.status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED ? QStringLiteral("#3A8F3A") : QStringLiteral("#B23A3A")));
    }

    if (patchResult.status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED)
    {
        refreshInlineHooksAsync();
    }
    else
    {
        QMessageBox::warning(this, QStringLiteral("Inline Hook 摘除"), resultText);
    }
}
