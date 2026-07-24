#include "KernelDock.h"
#include "../UI/VisibleTableWidget.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../OnlineScan/SandboxUploadActions.h"
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

using ksword::kernel_dock_internal::kernelText;

namespace
{
    QString kernelHookButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString kernelHookInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:transparent;/* %3 */color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // friendlyKernelHookIoMessage 作用：
    // - 把 Hook 审计/扫描/补丁 wrapper 的底层 io.message 转成人读说明；
    // - 输入 messageText：ArkDriverClient::IoResult::message；
    // - 返回：适合状态栏、详情编辑器和 QMessageBox 的短文本。
    QString friendlyKernelHookIoMessage(const std::string& messageText)
    {
        const QString rawText = QString::fromUtf8(messageText.data(), static_cast<int>(messageText.size())).trimmed();
        if (rawText.isEmpty())
        {
            return kernelText("kernel.hooks.message.no_driver_message", QStringLiteral("驱动未返回额外说明。"));
        }
        if (rawText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.hooks.message.communication_failure", QStringLiteral("驱动接口调用失败或当前驱动版本不支持该 Hook 审计入口。"));
        }
        if (rawText.contains(QStringLiteral("unsupported"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("not supported"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.hooks.message.unsupported", QStringLiteral("当前驱动不支持该 Hook 审计/操作入口。"));
        }
        if (rawText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.hooks.message.capability", QStringLiteral("动态偏移能力不足，Hook 详情暂不可用。"));
        }
        if (rawText.contains(QStringLiteral("access"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("denied"), Qt::CaseInsensitive))
        {
            return kernelText("kernel.hooks.message.access_denied", QStringLiteral("请求被权限或安全策略拒绝，未修改目标。"));
        }
        return rawText;
    }

    QString kernelHookHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:transparent;/* %2 */border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString kernelHookSelectionStyle()
    {
        return QString();
    }

    QString kernelHookStatusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString kernelHookSafeText(const QString& valueText, const QString& fallbackText)
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString kernelHookSafeText(const QString& valueText)
    {
        return kernelHookSafeText(valueText, kernelText("kernel.hooks.placeholder.empty", QStringLiteral("<空>")));
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
        return byteTextList.isEmpty()
            ? kernelText("kernel.hooks.placeholder.no_bytes", QStringLiteral("<无字节>"))
            : byteTextList.join(' ');
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

    QString kernelHookResolveModuleForAddress(
        const KernelHookModulePathMap& modulePathMap,
        const std::uint64_t address)
    {
        if (address == 0U)
        {
            return QString();
        }
        for (auto iterator = modulePathMap.constBegin(); iterator != modulePathMap.constEnd(); ++iterator)
        {
            const KernelHookLoadedModuleInfo& module = iterator.value();
            const std::uint64_t base = module.imageBase;
            const std::uint64_t size = module.imageSize;
            if (base != 0U && size != 0U && address >= base && address - base < size)
            {
                return module.fileNameText.trimmed().isEmpty()
                    ? module.ntPathText
                    : module.fileNameText;
            }
        }
        return QString();
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
            return fail(kernelText("kernel.hooks.pe.error.file_offset_output", QStringLiteral("内部错误：文件偏移输出为空。")));
        }
        *fileOffsetOut = 0ULL;
        if (bytesToRead == 0U)
        {
            return fail(kernelText("kernel.hooks.pe.error.zero_read_length", QStringLiteral("读取长度为 0。")));
        }
        if (fileBytes.size() < static_cast<qsizetype>(sizeof(IMAGE_DOS_HEADER)))
        {
            return fail(kernelText("kernel.hooks.pe.error.file_too_small", QStringLiteral("磁盘文件过小，无法读取 DOS 头。")));
        }

        IMAGE_DOS_HEADER dosHeader{};
        if (!kernelHookReadPodAtOffset(fileBytes, 0ULL, &dosHeader) ||
            dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
            dosHeader.e_lfanew < 0)
        {
            return fail(kernelText("kernel.hooks.pe.error.invalid_mz_pe", QStringLiteral("磁盘文件不是有效 MZ/PE 文件。")));
        }

        const std::uint64_t ntHeaderOffset = static_cast<std::uint64_t>(dosHeader.e_lfanew);
        std::uint32_t peSignature = 0U;
        if (!kernelHookReadPodAtOffset(fileBytes, ntHeaderOffset, &peSignature) ||
            peSignature != IMAGE_NT_SIGNATURE)
        {
            return fail(kernelText("kernel.hooks.pe.error.invalid_pe_signature", QStringLiteral("磁盘文件 PE 签名无效。")));
        }

        IMAGE_FILE_HEADER fileHeader{};
        const std::uint64_t fileHeaderOffset = ntHeaderOffset + sizeof(std::uint32_t);
        if (!kernelHookReadPodAtOffset(fileBytes, fileHeaderOffset, &fileHeader))
        {
            return fail(kernelText("kernel.hooks.pe.error.coff_header", QStringLiteral("读取 COFF 文件头失败。")));
        }
        if (fileHeader.NumberOfSections == 0U || fileHeader.NumberOfSections > 96U)
        {
            return fail(kernelText("kernel.hooks.pe.error.section_count", QStringLiteral("PE 区段数量异常：%1。")).arg(fileHeader.NumberOfSections));
        }

        const std::uint64_t optionalHeaderOffset = fileHeaderOffset + sizeof(IMAGE_FILE_HEADER);
        std::uint16_t optionalMagic = 0U;
        if (!kernelHookReadPodAtOffset(fileBytes, optionalHeaderOffset, &optionalMagic))
        {
            return fail(kernelText("kernel.hooks.pe.error.optional_magic_read", QStringLiteral("读取 Optional Header 魔数失败。")));
        }

        std::uint32_t sizeOfHeaders = 0U;
        if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER64 optionalHeader{};
            if (!kernelHookReadPodAtOffset(fileBytes, optionalHeaderOffset, &optionalHeader))
            {
                return fail(kernelText("kernel.hooks.pe.error.optional_header_64", QStringLiteral("读取 PE32+ Optional Header 失败。")));
            }
            sizeOfHeaders = optionalHeader.SizeOfHeaders;
        }
        else if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 optionalHeader{};
            if (!kernelHookReadPodAtOffset(fileBytes, optionalHeaderOffset, &optionalHeader))
            {
                return fail(kernelText("kernel.hooks.pe.error.optional_header_32", QStringLiteral("读取 PE32 Optional Header 失败。")));
            }
            sizeOfHeaders = optionalHeader.SizeOfHeaders;
        }
        else
        {
            return fail(kernelText("kernel.hooks.pe.error.unknown_optional_magic", QStringLiteral("未知 Optional Header 魔数：0x%1。"))
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
            return fail(kernelText("kernel.hooks.pe.error.header_range", QStringLiteral("RVA 位于 PE 头部，但读取范围超出磁盘文件。")));
        }

        const std::uint64_t sectionTableOffset = optionalHeaderOffset + fileHeader.SizeOfOptionalHeader;
        for (std::uint16_t sectionIndex = 0U; sectionIndex < fileHeader.NumberOfSections; ++sectionIndex)
        {
            IMAGE_SECTION_HEADER sectionHeader{};
            const std::uint64_t currentSectionOffset =
                sectionTableOffset + (static_cast<std::uint64_t>(sectionIndex) * sizeof(IMAGE_SECTION_HEADER));
            if (!kernelHookReadPodAtOffset(fileBytes, currentSectionOffset, &sectionHeader))
            {
                return fail(kernelText("kernel.hooks.pe.error.section_table", QStringLiteral("读取 PE 区段表失败，索引=%1。")).arg(sectionIndex));
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
                return fail(kernelText("kernel.hooks.pe.error.raw_data_short", QStringLiteral("RVA 落在区段虚拟范围内，但磁盘 raw 数据不足。")));
            }

            const std::uint64_t rawOffset =
                static_cast<std::uint64_t>(sectionHeader.PointerToRawData) + static_cast<std::uint64_t>(delta);
            if (rawOffset > static_cast<std::uint64_t>(fileBytes.size()) ||
                bytesToRead > static_cast<std::uint64_t>(fileBytes.size()) - rawOffset)
            {
                return fail(kernelText("kernel.hooks.pe.error.file_offset_range", QStringLiteral("映射到的磁盘偏移超出文件大小。")));
            }

            *fileOffsetOut = rawOffset;
            return true;
        }

        return fail(kernelText("kernel.hooks.pe.error.section_not_found", QStringLiteral("未找到覆盖目标 RVA 的 PE 区段。")));
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
            return fail(kernelText("kernel.hooks.pe.error.disk_bytes_output", QStringLiteral("内部错误：磁盘字节输出为空。")));
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
                    return fail(kernelText("kernel.hooks.pe.error.open_module", QStringLiteral("打开磁盘模块文件失败：%1。")).arg(fileObject.errorString()));
                }

                localFileBytes = fileObject.readAll();
                if (localFileBytes.isEmpty())
                {
                    return fail(kernelText("kernel.hooks.pe.error.module_empty", QStringLiteral("磁盘模块文件为空或读取失败。")));
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
                return fail(kernelText("kernel.hooks.pe.error.open_module", QStringLiteral("打开磁盘模块文件失败：%1。")).arg(fileObject.errorString()));
            }

            localFileBytes = fileObject.readAll();
            fileBytes = &localFileBytes;
        }

        if (fileBytes == nullptr || fileBytes->isEmpty())
        {
            return fail(kernelText("kernel.hooks.pe.error.module_empty", QStringLiteral("磁盘模块文件为空或读取失败。")));
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
            baselineResult.statusText = kernelText("kernel.hooks.baseline.memory_bytes_missing", QStringLiteral("不可用：R0 未返回内存字节。"));
            return baselineResult;
        }
        if (row.moduleBase == 0U || row.functionAddress < row.moduleBase)
        {
            baselineResult.statusText = kernelText("kernel.hooks.baseline.address_invalid", QStringLiteral("不可用：函数地址或模块基址无效。"));
            return baselineResult;
        }

        const std::uint64_t rva64 = row.functionAddress - row.moduleBase;
        baselineResult.rva = rva64;
        if (rva64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            baselineResult.statusText = kernelText("kernel.hooks.baseline.rva_out_of_range", QStringLiteral("不可用：函数 RVA 超出 32 位 PE 范围。"));
            return baselineResult;
        }

        const auto moduleIterator = modulePathMap.constFind(static_cast<qulonglong>(row.moduleBase));
        if (moduleIterator == modulePathMap.constEnd())
        {
            baselineResult.statusText = kernelText("kernel.hooks.baseline.module_path_missing", QStringLiteral("不可用：R3 未能反查模块磁盘路径。"));
            return baselineResult;
        }

        baselineResult.filePathText = kernelHookNormalizeKernelModulePath(moduleIterator->ntPathText);
        if (baselineResult.filePathText.isEmpty())
        {
            baselineResult.statusText = kernelText("kernel.hooks.baseline.path_conversion_failed", QStringLiteral("不可用：模块路径无法转换为 Win32 路径（%1）。"))
                .arg(kernelHookSafeText(
                    moduleIterator->ntPathText,
                    kernelText("kernel.hooks.placeholder.empty_path", QStringLiteral("<空路径>"))));
            return baselineResult;
        }
        if (!QFileInfo::exists(baselineResult.filePathText))
        {
            baselineResult.statusText = kernelText("kernel.hooks.baseline.module_not_found", QStringLiteral("不可用：磁盘模块文件不存在（%1）。"))
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
            baselineResult.statusText = kernelText("kernel.hooks.baseline.read_failed", QStringLiteral("不可用：%1")).arg(readErrorText);
            return baselineResult;
        }

        baselineResult.available = true;
        baselineResult.differsFromMemory = !std::equal(
            baselineResult.bytes.begin(),
            baselineResult.bytes.end(),
            row.currentBytes.begin());
        baselineResult.statusText = baselineResult.differsFromMemory
            ? kernelText("kernel.hooks.baseline.different", QStringLiteral("不同：内存字节与磁盘基线不一致"))
            : kernelText("kernel.hooks.baseline.same", QStringLiteral("一致：内存字节与磁盘基线相同"));
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

    enum class TimerDpcColumn : int
    {
        Cpu = 0,
        Bucket,
        Timer,
        DueTime,
        Period,
        Type,
        Dpc,
        Routine,
        Context,
        Module,
        Status,
        Count
    };

    QString kernelHookStatusText(const std::uint32_t status)
    {
        switch (status)
        {
        case KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN:
            return kernelText("kernel.hooks.status.clean", QStringLiteral("干净"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS:
            return kernelText("kernel.hooks.status.suspicious", QStringLiteral("可疑外跳"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH:
            return kernelText("kernel.hooks.status.internal_branch", QStringLiteral("模块内跳转"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED:
            return kernelText("kernel.hooks.status.read_failed", QStringLiteral("读取失败"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED:
            return kernelText("kernel.hooks.status.parse_failed", QStringLiteral("解析失败"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED:
            return kernelText("kernel.hooks.status.force_required", QStringLiteral("需要强制确认"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED:
            return kernelText("kernel.hooks.status.patched", QStringLiteral("已修复/摘除"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED:
            return kernelText("kernel.hooks.status.patch_failed", QStringLiteral("修复失败"));
        case KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN:
        default:
            return kernelText("kernel.hooks.status.unknown", QStringLiteral("未知(%1)")).arg(status);
        }
    }

    QString inlineHookTypeText(const std::uint32_t hookType)
    {
        switch (hookType)
        {
        case KSWORD_ARK_INLINE_HOOK_TYPE_NONE:
            return kernelText("kernel.hooks.type.none", QStringLiteral("无明显补丁"));
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
            return kernelText("kernel.hooks.type.ret_patch", QStringLiteral("RET 补丁"));
        case KSWORD_ARK_INLINE_HOOK_TYPE_INT3_PATCH:
            return kernelText("kernel.hooks.type.int3_patch", QStringLiteral("INT3 补丁"));
        case KSWORD_ARK_INLINE_HOOK_TYPE_UNKNOWN_PATCH:
            return kernelText("kernel.hooks.type.unknown_patch", QStringLiteral("未知补丁"));
        default:
            return kernelText("kernel.hooks.type.unknown", QStringLiteral("未知(%1)")).arg(hookType);
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
            return kernelText("kernel.hooks.iat.class.unknown", QStringLiteral("未知(%1)")).arg(hookClass);
        }
    }

    QColor statusColor(const std::uint32_t status)
    {
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED)
        {
            return KswordTheme::ErrorColor();
        }
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED)
        {
            return KswordTheme::WarningColor();
        }
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED ||
            status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN)
        {
            return KswordTheme::SuccessColor();
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
            return kernelText("kernel.ssdt.header.index", QStringLiteral("索引"));
        case ShadowSsdtColumn::ServiceName:
            return kernelText("kernel.ssdt.header.service_name", QStringLiteral("服务名"));
        case ShadowSsdtColumn::StubAddress:
            return kernelText("kernel.hooks.shadow.header.stub_address", QStringLiteral("Stub地址"));
        case ShadowSsdtColumn::ServiceAddress:
            return kernelText("kernel.hooks.shadow.header.service_routine", QStringLiteral("服务例程"));
        case ShadowSsdtColumn::Module:
            return kernelText("kernel.ssdt.header.module", QStringLiteral("模块"));
        case ShadowSsdtColumn::Status:
            return kernelText("kernel.ssdt.header.status", QStringLiteral("状态"));
        default:
            return kernelText("kernel.hooks.header.unknown", QStringLiteral("未知列"));
        }
    }

    QString inlineHookColumnHeader(const InlineHookColumn column)
    {
        switch (column)
        {
        case InlineHookColumn::Module:
            return kernelText("kernel.hooks.inline.header.module", QStringLiteral("模块"));
        case InlineHookColumn::Function:
            return kernelText("kernel.hooks.inline.header.function", QStringLiteral("函数"));
        case InlineHookColumn::FunctionAddress:
            return kernelText("kernel.hooks.inline.header.function_address", QStringLiteral("函数地址"));
        case InlineHookColumn::HookType:
            return kernelText("kernel.hooks.inline.header.type", QStringLiteral("类型"));
        case InlineHookColumn::TargetAddress:
            return kernelText("kernel.hooks.inline.header.target_address", QStringLiteral("目标地址"));
        case InlineHookColumn::TargetModule:
            return kernelText("kernel.hooks.inline.header.target_module", QStringLiteral("目标模块"));
        case InlineHookColumn::Status:
            return kernelText("kernel.hooks.inline.header.status", QStringLiteral("状态"));
        case InlineHookColumn::CurrentBytes:
            return kernelText("kernel.hooks.inline.header.memory_bytes", QStringLiteral("内存字节"));
        case InlineHookColumn::DiskBytes:
            return kernelText("kernel.hooks.inline.header.disk_bytes", QStringLiteral("磁盘字节"));
        case InlineHookColumn::DiskDiff:
            return kernelText("kernel.hooks.inline.header.disk_diff", QStringLiteral("差异状态"));
        default:
            return kernelText("kernel.hooks.header.unknown", QStringLiteral("未知列"));
        }
    }

    QString iatEatColumnHeader(const IatEatHookColumn column)
    {
        switch (column)
        {
        case IatEatHookColumn::Class:
            return kernelText("kernel.hooks.iat.header.class", QStringLiteral("类别"));
        case IatEatHookColumn::Module:
            return kernelText("kernel.hooks.iat.header.module", QStringLiteral("模块"));
        case IatEatHookColumn::ImportModule:
            return kernelText("kernel.hooks.iat.header.import_module", QStringLiteral("导入模块"));
        case IatEatHookColumn::Function:
            return kernelText("kernel.hooks.iat.header.function_or_ordinal", QStringLiteral("函数/序号"));
        case IatEatHookColumn::ThunkAddress:
            return kernelText("kernel.hooks.iat.header.thunk_eat_item", QStringLiteral("Thunk/EAT项"));
        case IatEatHookColumn::CurrentTarget:
            return kernelText("kernel.hooks.iat.header.current_target", QStringLiteral("当前目标"));
        case IatEatHookColumn::ExpectedTarget:
            return kernelText("kernel.hooks.iat.header.expected_target", QStringLiteral("期望目标"));
        case IatEatHookColumn::TargetModule:
            return kernelText("kernel.hooks.iat.header.target_module", QStringLiteral("目标模块"));
        case IatEatHookColumn::Status:
            return kernelText("kernel.hooks.iat.header.status", QStringLiteral("状态"));
        default:
            return kernelText("kernel.hooks.header.unknown", QStringLiteral("未知列"));
        }
    }

    QString timerDpcColumnHeader(const TimerDpcColumn column)
    {
        switch (column)
        {
        case TimerDpcColumn::Cpu: return kernelText("kernel.timer_dpc.header.cpu", QStringLiteral("CPU"));
        case TimerDpcColumn::Bucket: return kernelText("kernel.timer_dpc.header.bucket", QStringLiteral("Bucket"));
        case TimerDpcColumn::Timer: return kernelText("kernel.timer_dpc.header.timer", QStringLiteral("Timer"));
        case TimerDpcColumn::DueTime: return kernelText("kernel.timer_dpc.header.due_time", QStringLiteral("DueTime"));
        case TimerDpcColumn::Period: return kernelText("kernel.timer_dpc.header.period", QStringLiteral("Period"));
        case TimerDpcColumn::Type: return kernelText("kernel.timer_dpc.header.type", QStringLiteral("类型"));
        case TimerDpcColumn::Dpc: return kernelText("kernel.timer_dpc.header.dpc", QStringLiteral("DPC"));
        case TimerDpcColumn::Routine: return kernelText("kernel.timer_dpc.header.routine", QStringLiteral("例程"));
        case TimerDpcColumn::Context: return kernelText("kernel.timer_dpc.header.context", QStringLiteral("上下文"));
        case TimerDpcColumn::Module: return kernelText("kernel.timer_dpc.header.module", QStringLiteral("模块"));
        case TimerDpcColumn::Status: return kernelText("kernel.timer_dpc.header.status", QStringLiteral("状态"));
        default: return kernelText("kernel.hooks.header.unknown", QStringLiteral("未知列"));
        }
    }

    QString timerDpcTypeText(const std::uint32_t timerType)
    {
        if (timerType == 8U)
        {
            return kernelText("kernel.timer_dpc.type.notification", QStringLiteral("通知定时器(8)"));
        }
        if (timerType == 9U)
        {
            return kernelText("kernel.timer_dpc.type.synchronization", QStringLiteral("同步定时器(9)"));
        }
        return kernelText("kernel.timer_dpc.type.unknown", QStringLiteral("未知(%1)")).arg(timerType);
    }

    QString timerDpcEntryStatusText(const std::uint32_t flags, const QString& moduleName)
    {
        QStringList parts;
        if ((flags & KSWORD_ARK_TIMER_DPC_ENTRY_DPC_PRESENT) == 0U)
        {
            parts.push_back(kernelText("kernel.timer_dpc.status.no_dpc", QStringLiteral("无DPC")));
        }
        else if ((flags & KSWORD_ARK_TIMER_DPC_ENTRY_DPC_FIELDS_PRESENT) == 0U)
        {
            parts.push_back(kernelText("kernel.timer_dpc.status.dpc_unreadable", QStringLiteral("DPC字段不可读")));
        }
        else
        {
            parts.push_back(kernelText("kernel.timer_dpc.status.dpc", QStringLiteral("DPC已解析")));
        }
        if ((flags & KSWORD_ARK_TIMER_DPC_ENTRY_PERIODIC) != 0U)
        {
            parts.push_back(kernelText("kernel.timer_dpc.status.periodic", QStringLiteral("周期")));
        }
        if ((flags & KSWORD_ARK_TIMER_DPC_ENTRY_READ_PARTIAL) != 0U)
        {
            parts.push_back(kernelText("kernel.timer_dpc.status.partial", QStringLiteral("字段部分读取")));
        }
        if (moduleName.trimmed().isEmpty() && (flags & KSWORD_ARK_TIMER_DPC_ENTRY_DPC_FIELDS_PRESENT) != 0U)
        {
            parts.push_back(kernelText("kernel.timer_dpc.status.module_unresolved", QStringLiteral("模块未解析")));
        }
        return parts.join(QStringLiteral(" / "));
    }

    QString timerDpcColumnText(const KernelTimerDpcEntry& entry, const TimerDpcColumn column)
    {
        switch (column)
        {
        case TimerDpcColumn::Cpu:
            return QStringLiteral("%1:%2").arg(entry.processorGroup).arg(entry.processorNumber);
        case TimerDpcColumn::Bucket:
            return QString::number(entry.bucketIndex);
        case TimerDpcColumn::Timer:
            return kernelHookFormatAddress(entry.timerAddress);
        case TimerDpcColumn::DueTime:
            return QString::number(entry.dueTime);
        case TimerDpcColumn::Period:
            return QString::number(entry.period);
        case TimerDpcColumn::Type:
            return timerDpcTypeText(entry.timerType);
        case TimerDpcColumn::Dpc:
            return entry.dpcAddress == 0U ? kernelText("kernel.timer_dpc.placeholder.none", QStringLiteral("<无>")) : kernelHookFormatAddress(entry.dpcAddress);
        case TimerDpcColumn::Routine:
            return entry.deferredRoutine == 0U ? kernelText("kernel.timer_dpc.placeholder.none", QStringLiteral("<无>")) : kernelHookFormatAddress(entry.deferredRoutine);
        case TimerDpcColumn::Context:
            return entry.deferredContext == 0U ? kernelText("kernel.timer_dpc.placeholder.none", QStringLiteral("<无>")) : kernelHookFormatAddress(entry.deferredContext);
        case TimerDpcColumn::Module:
            return kernelHookSafeText(entry.moduleNameText, kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>")));
        case TimerDpcColumn::Status:
            return kernelHookSafeText(entry.statusText);
        default:
            return QString();
        }
    }

    QString timerDpcRowAsTsv(const KernelTimerDpcEntry& entry)
    {
        QStringList fields;
        fields.reserve(static_cast<int>(TimerDpcColumn::Count));
        for (int index = 0; index < static_cast<int>(TimerDpcColumn::Count); ++index)
        {
            fields.push_back(timerDpcColumnText(entry, static_cast<TimerDpcColumn>(index)));
        }
        return fields.join('\t');
    }

    QString shadowSsdtColumnText(const KernelSsdtEntry& entry, const ShadowSsdtColumn column)
    {
        switch (column)
        {
        case ShadowSsdtColumn::Index:
            return entry.indexResolved ? QString::number(entry.serviceIndex) : kernelText("kernel.hooks.placeholder.unknown", QStringLiteral("<未知>"));
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
            return kernelHookSafeText(entry.targetModuleNameText, kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>")));
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
            return kernelHookSafeText(entry.importModuleNameText, kernelText("kernel.hooks.placeholder.not_applicable", QStringLiteral("<不适用>")));
        case IatEatHookColumn::Function:
            return kernelHookSafeText(entry.functionNameText, QStringLiteral("#%1").arg(entry.ordinal));
        case IatEatHookColumn::ThunkAddress:
            return kernelHookFormatAddress(entry.thunkAddress);
        case IatEatHookColumn::CurrentTarget:
            return kernelHookFormatAddress(entry.currentTarget);
        case IatEatHookColumn::ExpectedTarget:
            return kernelHookFormatAddress(entry.expectedTarget);
        case IatEatHookColumn::TargetModule:
            return kernelHookSafeText(entry.targetModuleNameText, kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>")));
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
            : kernelText("kernel.hooks.placeholder.unavailable", QStringLiteral("<不可用>"));
        const QString diskByteCountText = row.diskBaselineAvailable
            ? QString::number(std::min<std::size_t>(row.diskBytes.size(), static_cast<std::size_t>(row.currentByteCount)))
            : QStringLiteral("0");
        const QString diskPathText = row.diskBaselinePathText.trimmed().isEmpty()
            ? kernelText("kernel.hooks.placeholder.unavailable", QStringLiteral("<不可用>"))
            : QDir::toNativeSeparators(row.diskBaselinePathText);
        const QString rvaText = (row.moduleBase != 0U && row.functionAddress >= row.moduleBase)
            ? kernelHookFormatAddress(row.functionAddress - row.moduleBase)
            : kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>"));

        return kernelText("kernel.hooks.inline.detail", QStringLiteral(
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
            "摘除操作保持原有 NOP 流程，不新增自动修复能力。"))
            .arg(kernelHookSafeText(row.moduleNameText))
            .arg(kernelHookSafeText(row.functionNameText))
            .arg(kernelHookFormatAddress(row.functionAddress))
            .arg(row.hookTypeText)
            .arg(kernelHookFormatAddress(row.targetAddress))
            .arg(kernelHookSafeText(row.targetModuleNameText, kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>"))))
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
            ? kernelText("kernel.hooks.disk_baseline.unchecked", QStringLiteral("磁盘基线：未校验"))
            : baselineResult.statusText;
        row->diskBaselinePathText = baselineResult.filePathText.trimmed().isEmpty()
            ? kernelText("kernel.hooks.placeholder.unavailable", QStringLiteral("<不可用>"))
            : QDir::toNativeSeparators(baselineResult.filePathText);
        row->diskBytes = baselineResult.bytes;
        row->diskBytesText = row->diskBaselineAvailable
            ? kernelHookBytesToText(row->diskBytes, baselineResult.byteCount)
            : kernelText("kernel.hooks.placeholder.unavailable", QStringLiteral("<不可用>"));
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
        statusParts.push_back(kernelText("kernel.hooks.shadow.status.table", QStringLiteral("Shadow/GUI表")));
        statusParts.push_back(row.indexResolved
            ? kernelText("kernel.hooks.shadow.status.index_resolved", QStringLiteral("索引已解析"))
            : kernelText("kernel.hooks.shadow.status.index_unresolved", QStringLiteral("索引未解析")));
        statusParts.push_back((row.flags & KSWORD_ARK_SSDT_ENTRY_FLAG_STUB_EXPORT) != 0U
            ? kernelText("kernel.hooks.shadow.status.stub_export", QStringLiteral("Stub导出"))
            : kernelText("kernel.hooks.shadow.status.not_stub_export", QStringLiteral("非Stub导出")));
        statusParts.push_back(row.serviceRoutineAddress != 0U
            ? kernelText("kernel.hooks.shadow.status.entry_resolved", QStringLiteral("表项已解析"))
            : kernelText("kernel.hooks.shadow.status.entry_unavailable", QStringLiteral("表项地址暂不可用")));
        row.statusText = statusParts.join(QStringLiteral(" | "));
        row.detailText = kernelText("kernel.hooks.shadow.detail", QStringLiteral(
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
            "说明: 服务例程地址为 0 表示当前资料不足或该表项暂不可读。"))
            .arg(enumResult.version)
            .arg(enumResult.totalCount)
            .arg(enumResult.returnedCount)
            .arg(kernelHookSafeText(row.serviceNameText))
            .arg(kernelHookSafeText(row.moduleNameText))
            .arg(row.indexResolved ? QString::number(row.serviceIndex) : kernelText("kernel.hooks.placeholder.unknown", QStringLiteral("<未知>")))
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
        row.diskBaselineStatusText = kernelText("kernel.hooks.disk_baseline.unchecked", QStringLiteral("磁盘基线：未校验"));
        row.diskBaselinePathText = kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>"));
        row.diskBytesText = kernelText("kernel.hooks.placeholder.not_fetched", QStringLiteral("<未获取>"));
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
        row.detailText = kernelText("kernel.hooks.iat.detail", QStringLiteral(
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
            "说明: IAT 检测比较 thunk 当前目标是否仍落在声明导入模块内；EAT 检测导出 RVA 是否落在自身映像或转发导出区域内。"))
            .arg(row.classText)
            .arg(kernelHookSafeText(row.moduleNameText))
            .arg(kernelHookSafeText(row.importModuleNameText, kernelText("kernel.hooks.placeholder.not_applicable", QStringLiteral("<不适用>"))))
            .arg(kernelHookSafeText(row.functionNameText))
            .arg(row.ordinal)
            .arg(kernelHookFormatAddress(row.thunkAddress))
            .arg(kernelHookFormatAddress(row.currentTarget))
            .arg(kernelHookFormatAddress(row.expectedTarget))
            .arg(kernelHookSafeText(row.targetModuleNameText, kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>"))))
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
    m_refreshShadowSsdtButton->setToolTip(kernelText("kernel.hooks.shadow.toolbar.refresh.tooltip", QStringLiteral("刷新 SSSDT/Shadow SSDT 解析结果")));
    m_refreshShadowSsdtButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshShadowSsdtButton->setFixedWidth(34);

    m_shadowSsdtFilterEdit = new QLineEdit(m_shadowSsdtPage);
    m_shadowSsdtFilterEdit->setPlaceholderText(kernelText("kernel.hooks.shadow.toolbar.filter.placeholder", QStringLiteral("按索引/服务名/模块/地址/状态筛选")));
    m_shadowSsdtFilterEdit->setClearButtonEnabled(true);
    m_shadowSsdtFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_shadowSsdtStatusLabel = new QLabel(kernelText("kernel.hooks.shadow.status.waiting", QStringLiteral("状态：等待刷新")), m_shadowSsdtPage);
    m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_shadowSsdtToolLayout->addWidget(m_refreshShadowSsdtButton, 0);
    m_shadowSsdtToolLayout->addWidget(m_shadowSsdtFilterEdit, 1);
    m_shadowSsdtToolLayout->addWidget(m_shadowSsdtStatusLabel, 0);
    m_shadowSsdtLayout->addLayout(m_shadowSsdtToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_shadowSsdtPage);
    m_shadowSsdtLayout->addWidget(splitter, 1);

    m_shadowSsdtTable = new ks::ui::VisibleTableWidget(splitter);
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
    m_shadowSsdtDetailEditor->setText(kernelText("kernel.hooks.shadow.detail.initial", QStringLiteral("请选择一条 SSSDT 记录查看详情。")));

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
    m_refreshInlineHookButton->setToolTip(kernelText("kernel.hooks.inline.toolbar.scan.tooltip", QStringLiteral("扫描内核模块导出函数 Inline Hook")));
    m_refreshInlineHookButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshInlineHookButton->setFixedWidth(34);

    m_patchInlineHookButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), kernelText("kernel.hooks.inline.toolbar.patch", QStringLiteral("NOP 摘除选中")), m_inlineHookPage);
    m_patchInlineHookButton->setToolTip(kernelText("kernel.hooks.inline.toolbar.patch.tooltip", QStringLiteral("对当前选中 Hook 先普通请求，再经强制确认后写入 NOP")));
    m_patchInlineHookButton->setStyleSheet(kernelHookButtonStyle());

    m_inlineHookModuleEdit = new QLineEdit(m_inlineHookPage);
    m_inlineHookModuleEdit->setPlaceholderText(kernelText("kernel.hooks.inline.toolbar.module.placeholder", QStringLiteral("模块过滤，如 ntoskrnl.exe / win32k.sys（留空扫描全部）")));
    m_inlineHookModuleEdit->setClearButtonEnabled(true);
    m_inlineHookModuleEdit->setStyleSheet(kernelHookInputStyle());

    m_inlineHookFilterEdit = new QLineEdit(m_inlineHookPage);
    m_inlineHookFilterEdit->setPlaceholderText(kernelText("kernel.hooks.inline.toolbar.filter.placeholder", QStringLiteral("本地筛选：模块/函数/地址/类型/状态/字节")));
    m_inlineHookFilterEdit->setClearButtonEnabled(true);
    m_inlineHookFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_inlineHookIncludeCombo = new QComboBox(m_inlineHookPage);
    m_inlineHookIncludeCombo->addItem(kernelText("kernel.hooks.inline.combo.suspicious_only", QStringLiteral("仅可疑外跳")), QVariant::fromValue<qulonglong>(0ULL));
    m_inlineHookIncludeCombo->addItem(kernelText("kernel.hooks.inline.combo.suspicious_internal", QStringLiteral("可疑 + 模块内跳转")), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL));
    m_inlineHookIncludeCombo->addItem(kernelText("kernel.hooks.inline.combo.include_clean", QStringLiteral("包含干净项")), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN));
    m_inlineHookIncludeCombo->setToolTip(kernelText("kernel.hooks.inline.combo.tooltip", QStringLiteral("控制 R0 扫描结果返回范围，包含干净项会明显增多")));

    m_inlineHookStatusLabel = new QLabel(kernelText("kernel.hooks.inline.status.waiting", QStringLiteral("状态：等待扫描")), m_inlineHookPage);
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

    m_inlineHookTable = new ks::ui::VisibleTableWidget(splitter);
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
    m_inlineHookDetailEditor->setText(kernelText("kernel.hooks.inline.detail.initial", QStringLiteral("请选择一条 Inline Hook 记录查看详情。")));

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
    m_refreshIatEatHookButton->setToolTip(kernelText("kernel.hooks.iat.toolbar.scan.tooltip", QStringLiteral("扫描内核模块 IAT/EAT Hook")));
    m_refreshIatEatHookButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshIatEatHookButton->setFixedWidth(34);

    m_iatEatHookModuleEdit = new QLineEdit(m_iatEatHookPage);
    m_iatEatHookModuleEdit->setPlaceholderText(kernelText("kernel.hooks.iat.toolbar.module.placeholder", QStringLiteral("模块过滤，如 ntoskrnl.exe / fltmgr.sys（留空扫描全部）")));
    m_iatEatHookModuleEdit->setClearButtonEnabled(true);
    m_iatEatHookModuleEdit->setStyleSheet(kernelHookInputStyle());

    m_iatEatHookFilterEdit = new QLineEdit(m_iatEatHookPage);
    m_iatEatHookFilterEdit->setPlaceholderText(kernelText("kernel.hooks.iat.toolbar.filter.placeholder", QStringLiteral("本地筛选：类别/模块/导入模块/函数/地址/状态")));
    m_iatEatHookFilterEdit->setClearButtonEnabled(true);
    m_iatEatHookFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_iatEatHookIncludeCombo = new QComboBox(m_iatEatHookPage);
    m_iatEatHookIncludeCombo->addItem(kernelText("kernel.hooks.iat.combo.suspicious_both", QStringLiteral("IAT + EAT 可疑项")), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS));
    m_iatEatHookIncludeCombo->addItem(kernelText("kernel.hooks.iat.combo.suspicious_iat", QStringLiteral("仅 IAT 可疑项")), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS));
    m_iatEatHookIncludeCombo->addItem(kernelText("kernel.hooks.iat.combo.suspicious_eat", QStringLiteral("仅 EAT 可疑项")), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS));
    m_iatEatHookIncludeCombo->addItem(kernelText("kernel.hooks.iat.combo.include_clean", QStringLiteral("IAT + EAT + 干净项")), QVariant::fromValue<qulonglong>(KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN));
    m_iatEatHookIncludeCombo->setToolTip(kernelText("kernel.hooks.iat.combo.tooltip", QStringLiteral("控制 R0 扫描 IAT/EAT 范围，包含干净项会明显增多")));

    m_iatEatHookStatusLabel = new QLabel(kernelText("kernel.hooks.iat.status.waiting", QStringLiteral("状态：等待扫描")), m_iatEatHookPage);
    m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_iatEatHookToolLayout->addWidget(m_refreshIatEatHookButton, 0);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookModuleEdit, 2);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookFilterEdit, 2);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookIncludeCombo, 0);
    m_iatEatHookToolLayout->addWidget(m_iatEatHookStatusLabel, 0);
    m_iatEatHookLayout->addLayout(m_iatEatHookToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_iatEatHookPage);
    m_iatEatHookLayout->addWidget(splitter, 1);

    m_iatEatHookTable = new ks::ui::VisibleTableWidget(splitter);
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
    m_iatEatHookDetailEditor->setText(kernelText("kernel.hooks.iat.detail.initial", QStringLiteral("请选择一条 IAT/EAT 记录查看详情。")));

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

void KernelDock::initializeTimerDpcTab()
{
    if (m_timerDpcPage == nullptr || m_timerDpcLayout != nullptr)
    {
        return;
    }

    m_timerDpcLayout = new QVBoxLayout(m_timerDpcPage);
    m_timerDpcLayout->setContentsMargins(4, 4, 4, 4);
    m_timerDpcLayout->setSpacing(6);
    m_timerDpcToolLayout = new QHBoxLayout();
    m_timerDpcToolLayout->setContentsMargins(0, 0, 0, 0);
    m_timerDpcToolLayout->setSpacing(6);

    m_refreshTimerDpcButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_timerDpcPage);
    m_refreshTimerDpcButton->setToolTip(kernelText("kernel.timer_dpc.refresh.tooltip", QStringLiteral("刷新每 CPU KTIMER/KDPC 快照")));
    m_refreshTimerDpcButton->setStyleSheet(kernelHookButtonStyle());
    m_refreshTimerDpcButton->setFixedWidth(34);

    m_timerDpcFilterEdit = new QLineEdit(m_timerDpcPage);
    m_timerDpcFilterEdit->setPlaceholderText(kernelText("kernel.timer_dpc.filter.placeholder", QStringLiteral("筛选 CPU/Bucket/Timer/DPC/例程/模块/状态")));
    m_timerDpcFilterEdit->setClearButtonEnabled(true);
    m_timerDpcFilterEdit->setStyleSheet(kernelHookInputStyle());

    m_timerDpcStatusLabel = new QLabel(kernelText("kernel.timer_dpc.status.waiting", QStringLiteral("状态：等待刷新")), m_timerDpcPage);
    m_timerDpcStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_timerDpcToolLayout->addWidget(m_refreshTimerDpcButton, 0);
    m_timerDpcToolLayout->addWidget(m_timerDpcFilterEdit, 1);
    m_timerDpcToolLayout->addWidget(m_timerDpcStatusLabel, 0);
    m_timerDpcLayout->addLayout(m_timerDpcToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_timerDpcPage);
    m_timerDpcLayout->addWidget(splitter, 1);
    m_timerDpcTable = new ks::ui::VisibleTableWidget(splitter);
    m_timerDpcTable->setColumnCount(static_cast<int>(TimerDpcColumn::Count));
    QStringList headers;
    for (int column = 0; column < static_cast<int>(TimerDpcColumn::Count); ++column)
    {
        headers.push_back(timerDpcColumnHeader(static_cast<TimerDpcColumn>(column)));
    }
    m_timerDpcTable->setHorizontalHeaderLabels(headers);
    prepareTable(m_timerDpcTable);
    m_timerDpcTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(TimerDpcColumn::Module), QHeaderView::Stretch);

    m_timerDpcDetailEditor = new CodeEditorWidget(splitter);
    m_timerDpcDetailEditor->setReadOnly(true);
    m_timerDpcDetailEditor->setText(kernelText("kernel.timer_dpc.detail.initial", QStringLiteral("请选择一条 KTIMER/DPC 记录查看详情。")));
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshTimerDpcButton, &QPushButton::clicked, this, [this]() { refreshTimerDpcAfterDynDataAsync(); });
    connect(m_timerDpcFilterEdit, &QLineEdit::textChanged, this, [this](const QString& text) { rebuildTimerDpcTable(text.trimmed()); });
    connect(m_timerDpcTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) { showTimerDpcDetailByCurrentRow(); });
    connect(m_timerDpcTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) { showTimerDpcContextMenu(position); });
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
        m_shadowSsdtStatusLabel->setText(kernelText("kernel.hooks.shadow.status.parsing", QStringLiteral("状态：SSSDT 解析中...")));
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
            errorText = kernelText("kernel.hooks.shadow.error.io", QStringLiteral("SSSDT 解析 IOCTL 调用失败。\nWin32=%1\n详情=%2"))
                .arg(enumResult.io.win32Error)
                .arg(friendlyKernelHookIoMessage(enumResult.io.message));
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
                guardThis->m_shadowSsdtStatusLabel->setText(kernelText("kernel.hooks.shadow.status.failed", QStringLiteral("状态：解析失败")));
                guardThis->m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::ErrorHex()));
                guardThis->m_shadowSsdtDetailEditor->setText(errorText);
                return;
            }

            guardThis->m_shadowSsdtRows = std::move(resultRows);
            guardThis->rebuildShadowSsdtTable(guardThis->m_shadowSsdtFilterEdit->text().trimmed());
            guardThis->m_shadowSsdtStatusLabel->setText(
                kernelText("kernel.hooks.shadow.status.summary", QStringLiteral("状态：已解析 %1/%2 项，显示 %3 项"))
                .arg(returnedCount)
                .arg(totalCount)
                .arg(guardThis->m_shadowSsdtRows.size()));
            guardThis->m_shadowSsdtStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::SuccessHex()));

            if (guardThis->m_shadowSsdtTable->rowCount() > 0)
            {
                guardThis->m_shadowSsdtTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_shadowSsdtDetailEditor->setText(kernelText("kernel.hooks.shadow.empty", QStringLiteral("当前环境未返回 SSSDT stub 解析结果。")));
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
        m_inlineHookStatusLabel->setText(kernelText("kernel.hooks.inline.status.scanning", QStringLiteral("状态：Inline Hook 扫描中...")));
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
            errorText = kernelText("kernel.hooks.inline.error.io", QStringLiteral("Inline Hook 扫描 IOCTL 调用失败。\nWin32=%1\n详情=%2"))
                .arg(scanResult.io.win32Error)
                .arg(friendlyKernelHookIoMessage(scanResult.io.message));
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
                guardThis->m_inlineHookStatusLabel->setText(kernelText("kernel.hooks.inline.status.failed", QStringLiteral("状态：扫描失败")));
                guardThis->m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::ErrorHex()));
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
                kernelText("kernel.hooks.inline.status.summary", QStringLiteral("状态：模块=%1，总命中=%2，返回=%3，可疑=%4，内部跳转=%5，Last=%6"))
                .arg(moduleCount)
                .arg(totalCount)
                .arg(guardThis->m_inlineHookRows.size())
                .arg(suspiciousCount)
                .arg(internalCount)
                .arg(kernelHookFormatNtStatus(lastStatus)));
            guardThis->m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
                suspiciousCount == 0U ? KswordTheme::SuccessHex() : KswordTheme::ErrorHex()));

            if (guardThis->m_inlineHookTable->rowCount() > 0)
            {
                guardThis->m_inlineHookTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_inlineHookDetailEditor->setText(kernelText("kernel.hooks.inline.empty", QStringLiteral("当前过滤条件下未返回 Inline Hook 记录。")));
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
        m_iatEatHookStatusLabel->setText(kernelText("kernel.hooks.iat.status.scanning", QStringLiteral("状态：IAT/EAT 扫描中...")));
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
            errorText = kernelText("kernel.hooks.iat.error.io", QStringLiteral("IAT/EAT 扫描 IOCTL 调用失败。\nWin32=%1\n详情=%2"))
                .arg(scanResult.io.win32Error)
                .arg(friendlyKernelHookIoMessage(scanResult.io.message));
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
                guardThis->m_iatEatHookStatusLabel->setText(kernelText("kernel.hooks.iat.status.failed", QStringLiteral("状态：扫描失败")));
                guardThis->m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::ErrorHex()));
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
                kernelText("kernel.hooks.iat.status.summary", QStringLiteral("状态：模块=%1，总命中=%2，返回=%3，可疑=%4，Last=%5"))
                .arg(moduleCount)
                .arg(totalCount)
                .arg(guardThis->m_iatEatHookRows.size())
                .arg(suspiciousCount)
                .arg(kernelHookFormatNtStatus(lastStatus)));
            guardThis->m_iatEatHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
                suspiciousCount == 0U ? KswordTheme::SuccessHex() : KswordTheme::ErrorHex()));

            if (guardThis->m_iatEatHookTable->rowCount() > 0)
            {
                guardThis->m_iatEatHookTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_iatEatHookDetailEditor->setText(kernelText("kernel.hooks.iat.empty", QStringLiteral("当前过滤条件下未返回 IAT/EAT 记录。")));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshTimerDpcAsync()
{
    if (m_timerDpcRefreshRunning.exchange(true))
    {
        return;
    }
    if (m_refreshTimerDpcButton != nullptr)
    {
        m_refreshTimerDpcButton->setEnabled(false);
    }
    if (m_timerDpcStatusLabel != nullptr)
    {
        m_timerDpcStatusLabel->setText(kernelText("kernel.timer_dpc.status.enumerating", QStringLiteral("状态：正在遍历 KTIMER/DPC...")));
        m_timerDpcStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::PrimaryBlueHex));
    }

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        const ksword::ark::DriverClient driverClient;
        const ksword::ark::KernelTimerDpcEnumResult enumResult = driverClient.enumerateKernelTimerDpc();
        std::vector<KernelTimerDpcEntry> resultRows;
        if (enumResult.io.ok)
        {
            const KernelHookModulePathMap modulePathMap = kernelHookQueryLoadedModulePathMap();
            resultRows.reserve(enumResult.entries.size());
            for (const ksword::ark::KernelTimerDpcEntry& source : enumResult.entries)
            {
                KernelTimerDpcEntry row{};
                row.processorGroup = source.processorGroup;
                row.processorNumber = source.processorNumber;
                row.bucketIndex = source.bucketIndex;
                row.flags = source.flags;
                row.timerType = source.timerType;
                row.period = source.period;
                row.dueTime = source.dueTime;
                row.timerAddress = source.timerAddress;
                row.dpcAddress = source.dpcAddress;
                row.deferredRoutine = source.deferredRoutine;
                row.deferredContext = source.deferredContext;
                row.moduleNameText = kernelHookResolveModuleForAddress(modulePathMap, row.deferredRoutine);
                row.statusText = timerDpcEntryStatusText(row.flags, row.moduleNameText);
                row.detailText = kernelText("kernel.timer_dpc.detail", QStringLiteral(
                    "KTIMER / KDPC 详情\n"
                    "CPU: %1:%2\n"
                    "Bucket: %3\n"
                    "Timer: %4\n"
                    "DueTime: %5\n"
                    "Period: %6\n"
                    "类型: %7\n"
                    "DPC: %8\n"
                    "DeferredRoutine: %9\n"
                    "DeferredContext: %10\n"
                    "模块: %11\n"
                    "状态: %12\n"
                    "标志: 0x%13\n\n"
                    "说明: 数据由 R0 使用精确 DynData v4 布局只读遍历当前活动 TimerTable 获得；"
                    "未获取私有 bucket lock，刷新期间并发增删可能导致 partial/corrupt 诊断。"))
                    .arg(row.processorGroup)
                    .arg(row.processorNumber)
                    .arg(row.bucketIndex)
                    .arg(kernelHookFormatAddress(row.timerAddress))
                    .arg(row.dueTime)
                    .arg(row.period)
                    .arg(timerDpcTypeText(row.timerType))
                    .arg(row.dpcAddress == 0U ? kernelText("kernel.timer_dpc.placeholder.none", QStringLiteral("<无>")) : kernelHookFormatAddress(row.dpcAddress))
                    .arg(row.deferredRoutine == 0U ? kernelText("kernel.timer_dpc.placeholder.none", QStringLiteral("<无>")) : kernelHookFormatAddress(row.deferredRoutine))
                    .arg(row.deferredContext == 0U ? kernelText("kernel.timer_dpc.placeholder.none", QStringLiteral("<无>")) : kernelHookFormatAddress(row.deferredContext))
                    .arg(kernelHookSafeText(row.moduleNameText, kernelText("kernel.hooks.placeholder.not_resolved", QStringLiteral("<未解析>"))))
                    .arg(row.statusText)
                    .arg(static_cast<qulonglong>(row.flags), 8, 16, QChar('0'));
                resultRows.push_back(std::move(row));
            }
        }

        QMetaObject::invokeMethod(guardThis, [guardThis, enumResult, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }
            guardThis->m_timerDpcRefreshRunning.store(false);
            if (guardThis->m_refreshTimerDpcButton != nullptr)
            {
                guardThis->m_refreshTimerDpcButton->setEnabled(true);
            }
            if (!enumResult.io.ok)
            {
                guardThis->m_timerDpcStatusLabel->setText(kernelText("kernel.timer_dpc.status.failed", QStringLiteral("状态：枚举失败")));
                guardThis->m_timerDpcStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(KswordTheme::ErrorHex()));
                guardThis->m_timerDpcDetailEditor->setText(kernelText("kernel.timer_dpc.error.io", QStringLiteral("KTIMER/DPC 驱动接口调用失败。\nWin32=%1\n详情=%2"))
                    .arg(enumResult.io.win32Error)
                    .arg(friendlyKernelHookIoMessage(enumResult.io.message)));
                return;
            }

            guardThis->m_timerDpcRows = std::move(resultRows);
            guardThis->rebuildTimerDpcTable(guardThis->m_timerDpcFilterEdit->text().trimmed());
            const bool complete = enumResult.queryStatus == KSWORD_ARK_TIMER_DPC_QUERY_STATUS_OK && enumResult.statusFlags == 0U;
            guardThis->m_timerDpcStatusLabel->setText(kernelText("kernel.timer_dpc.status.summary", QStringLiteral(
                "状态：CPU=%1，Bucket=%2/%3，Timer=%4/%5，损坏=%6，读取失败=%7，重复=%8，完整性=%9"))
                .arg(enumResult.processorCount)
                .arg(enumResult.bucketsVisited)
                .arg(enumResult.processorCount * enumResult.bucketCount)
                .arg(enumResult.entries.size())
                .arg(enumResult.totalCount)
                .arg(enumResult.corruptBucketCount)
                .arg(enumResult.readFailureCount)
                .arg(enumResult.duplicateCount)
                .arg(complete
                    ? kernelText("kernel.timer_dpc.integrity.complete", QStringLiteral("完整"))
                    : kernelText("kernel.timer_dpc.integrity.partial", QStringLiteral("部分"))));
            guardThis->m_timerDpcStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
                complete ? KswordTheme::SuccessHex() : KswordTheme::WarningHex()));

            if (enumResult.queryStatus == KSWORD_ARK_TIMER_DPC_QUERY_STATUS_DYNDATA_MISSING)
            {
                guardThis->m_timerDpcDetailEditor->setText(kernelText("kernel.timer_dpc.error.dyndata", QStringLiteral("当前 ntoskrnl 的 DynData v4 Timer/DPC 布局不可用。请确认偏移包已匹配并下发到驱动。")));
            }
            else if (enumResult.queryStatus == KSWORD_ARK_TIMER_DPC_QUERY_STATUS_INVALID_LAYOUT)
            {
                guardThis->m_timerDpcDetailEditor->setText(kernelText("kernel.timer_dpc.error.layout", QStringLiteral("驱动拒绝了当前 Timer/DPC 布局，未读取 TimerTable。")));
            }
            else if (guardThis->m_timerDpcTable->rowCount() > 0)
            {
                guardThis->m_timerDpcTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_timerDpcDetailEditor->setText(kernelText("kernel.timer_dpc.empty", QStringLiteral("当前快照未返回活动 KTIMER 记录。")));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::refreshTimerDpcAfterDynDataAsync()
{
    // TimerTable 的所有私有字段都来自 DynData v4。动态偏移页是惰性 UI，
    // Timer/DPC 不能依赖用户先手动打开该页，因此在业务查询前显式完成
    // profile 匹配和下发。已有 DynData 任务运行时只登记一次后续刷新。
    if (!m_dynDataTabInitialized)
    {
        initializeDynDataTab();
        m_dynDataTabInitialized = true;
    }

    m_timerDpcRefreshAfterDynData.store(true);
    refreshDynDataAsync();
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
                item->setForeground(QBrush(KswordTheme::WarningColor()));
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
                    item->setForeground(QBrush(KswordTheme::WarningColor()));
                }
                else
                {
                    item->setForeground(QBrush(entry.diskBaselineDiffers
                        ? KswordTheme::ErrorColor()
                        : KswordTheme::SuccessColor()));
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

void KernelDock::rebuildTimerDpcTable(const QString& filterKeyword)
{
    if (m_timerDpcTable == nullptr)
    {
        return;
    }
    m_timerDpcTable->setSortingEnabled(false);
    m_timerDpcTable->setRowCount(0);
    for (std::size_t sourceIndex = 0U; sourceIndex < m_timerDpcRows.size(); ++sourceIndex)
    {
        const KernelTimerDpcEntry& entry = m_timerDpcRows[sourceIndex];
        QStringList fields;
        for (int column = 0; column < static_cast<int>(TimerDpcColumn::Count); ++column)
        {
            fields.push_back(timerDpcColumnText(entry, static_cast<TimerDpcColumn>(column)));
        }
        if (!filterKeyword.isEmpty() &&
            !fields.join(' ').contains(filterKeyword, Qt::CaseInsensitive) &&
            !entry.detailText.contains(filterKeyword, Qt::CaseInsensitive))
        {
            continue;
        }
        const int rowIndex = m_timerDpcTable->rowCount();
        m_timerDpcTable->insertRow(rowIndex);
        for (int column = 0; column < static_cast<int>(TimerDpcColumn::Count); ++column)
        {
            auto* item = new QTableWidgetItem(timerDpcColumnText(entry, static_cast<TimerDpcColumn>(column)));
            if (column == 0)
            {
                item->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
            }
            if (column == static_cast<int>(TimerDpcColumn::Status))
            {
                const bool partial = (entry.flags & KSWORD_ARK_TIMER_DPC_ENTRY_READ_PARTIAL) != 0U;
                item->setForeground(QBrush(partial ? KswordTheme::WarningColor() : KswordTheme::SuccessColor()));
            }
            setTableItem(m_timerDpcTable, rowIndex, column, item);
        }
    }
    m_timerDpcTable->setSortingEnabled(true);
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
    m_shadowSsdtDetailEditor->setText(entry != nullptr ? entry->detailText : kernelText("kernel.hooks.shadow.detail.initial", QStringLiteral("请选择一条 SSSDT 记录查看详情。")));
}

void KernelDock::showInlineHookDetailByCurrentRow()
{
    if (m_inlineHookDetailEditor == nullptr)
    {
        return;
    }
    const KernelInlineHookEntry* entry = currentInlineHookEntry();
    m_inlineHookDetailEditor->setText(entry != nullptr ? entry->detailText : kernelText("kernel.hooks.inline.detail.initial", QStringLiteral("请选择一条 Inline Hook 记录查看详情。")));
}

void KernelDock::showIatEatHookDetailByCurrentRow()
{
    if (m_iatEatHookDetailEditor == nullptr)
    {
        return;
    }
    const KernelIatEatHookEntry* entry = currentIatEatHookEntry();
    m_iatEatHookDetailEditor->setText(entry != nullptr ? entry->detailText : kernelText("kernel.hooks.iat.detail.initial", QStringLiteral("请选择一条 IAT/EAT 记录查看详情。")));
}

void KernelDock::showTimerDpcDetailByCurrentRow()
{
    if (m_timerDpcDetailEditor == nullptr || m_timerDpcTable == nullptr || m_timerDpcTable->currentRow() < 0)
    {
        return;
    }
    const QTableWidgetItem* item = m_timerDpcTable->item(m_timerDpcTable->currentRow(), 0);
    if (item == nullptr)
    {
        return;
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong());
    if (sourceIndex < m_timerDpcRows.size())
    {
        m_timerDpcDetailEditor->setText(m_timerDpcRows[sourceIndex].detailText);
    }
}

void KernelDock::showTimerDpcContextMenu(const QPoint& localPosition)
{
    if (m_timerDpcTable == nullptr)
    {
        return;
    }
    QTableWidgetItem* clickedItem = m_timerDpcTable->itemAt(localPosition);
    const int clickedRow = clickedItem != nullptr ? clickedItem->row() : m_timerDpcTable->currentRow();
    if (clickedItem != nullptr && !clickedItem->isSelected())
    {
        m_timerDpcTable->clearSelection();
        m_timerDpcTable->setCurrentItem(clickedItem);
        m_timerDpcTable->selectRow(clickedRow);
    }
    const std::vector<std::size_t> selectedIndices = selectedSourceIndices(
        m_timerDpcTable,
        m_timerDpcRows,
        clickedRow);

    QMenu menu(this);
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), kernelText("kernel.timer_dpc.menu.refresh", QStringLiteral("刷新 KTIMER/DPC")));
    QAction* copyRowsAction = menu.addAction(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy_row", QStringLiteral("复制选中行（TSV）")));
    QAction* copyDetailAction = menu.addAction(kernelText("kernel.hooks.menu.copy_detail", QStringLiteral("复制详情（选中行）")));
    copyRowsAction->setEnabled(!selectedIndices.empty());
    copyDetailAction->setEnabled(!selectedIndices.empty());

    const QAction* selectedAction = menu.exec(m_timerDpcTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == refreshAction)
    {
        refreshTimerDpcAfterDynDataAsync();
    }
    else if (selectedAction == copyRowsAction)
    {
        QStringList lines;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            lines.push_back(timerDpcRowAsTsv(m_timerDpcRows[sourceIndex]));
        }
        kernelHookCopyTextToClipboard(lines.join('\n'));
    }
    else if (selectedAction == copyDetailAction)
    {
        QStringList details;
        for (const std::size_t sourceIndex : selectedIndices)
        {
            details.push_back(m_timerDpcRows[sourceIndex].detailText);
        }
        kernelHookCopyTextToClipboard(details.join(QStringLiteral("\n\n---\n\n")));
    }
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
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), kernelText("kernel.hooks.shadow.menu.refresh", QStringLiteral("刷新 SSSDT")));
    menu.addSeparator();
    QMenu* copyMenu = menu.addMenu(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy", QStringLiteral("复制")));
    QAction* copyCurrentColumnAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), kernelText("kernel.hooks.menu.copy_current_column", QStringLiteral("复制当前列（选中行）")));
    QAction* copyRowsAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy_row", QStringLiteral("复制选中行（TSV）")));
    QAction* copyRowsWithHeaderAction = copyMenu->addAction(kernelText("kernel.hooks.menu.copy_header_rows", QStringLiteral("复制表头+选中行（TSV）")));
    QAction* copyDetailAction = copyMenu->addAction(kernelText("kernel.hooks.menu.copy_detail", QStringLiteral("复制详情（选中行）")));
    copyMenu->addSeparator();
    QMenu* columnMenu = copyMenu->addMenu(kernelText("kernel.hooks.menu.copy_columns", QStringLiteral("复制指定栏目（选中行）")));
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
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), kernelText("kernel.hooks.inline.menu.rescan", QStringLiteral("重新扫描 Inline Hook")));
    QAction* patchAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), kernelText("kernel.hooks.inline.menu.patch_current", QStringLiteral("NOP 摘除当前 Hook")));
    patchAction->setEnabled(hasSelection);
    const bool hasSingleInlineHook = selectedIndices.size() == 1U && selectedIndices.front() < m_inlineHookRows.size();
    const QString inlineHookDiskPath = hasSingleInlineHook
        ? m_inlineHookRows[selectedIndices.front()].diskBaselinePathText.trimmed()
        : QString();
    QAction* uploadVirusTotalAction = ks::online_scan::addVirusTotalSandboxMenu(
        &menu,
        this,
        [this, hasSingleInlineHook, inlineHookDiskPath, selectedIndices]() -> ks::online_scan::SandboxUploadTarget
        {
            // 输入：Inline Hook 当前单选行。
            // 处理：使用磁盘基线路径作为所属模块文件路径，不从模块名猜测路径。
            // 返回：待上传路径和来源说明。
            ks::online_scan::SandboxUploadTarget uploadTarget;
            if (!hasSingleInlineHook || selectedIndices.front() >= m_inlineHookRows.size())
            {
                uploadTarget.errorText = kernelText("kernel.hooks.inline.upload.single_row", QStringLiteral("请只选择一条 Inline Hook 记录。"));
                return uploadTarget;
            }
            const KernelInlineHookEntry& entry = m_inlineHookRows[selectedIndices.front()];
            uploadTarget.filePath = inlineHookDiskPath;
            uploadTarget.sourceText = kernelText("kernel.hooks.inline.upload.source", QStringLiteral("Inline Hook 模块 %1!%2"))
                .arg(entry.moduleNameText, entry.functionNameText);
            if (uploadTarget.filePath.trimmed().isEmpty())
            {
                uploadTarget.errorText = kernelText("kernel.hooks.inline.upload.no_disk_path", QStringLiteral("当前 Inline Hook 行没有可用磁盘基线路径。"));
            }
            return uploadTarget;
        });
    if (uploadVirusTotalAction != nullptr)
    {
        uploadVirusTotalAction->setEnabled(hasSingleInlineHook && QFileInfo(inlineHookDiskPath).isFile());
    }
    menu.addSeparator();
    QMenu* copyMenu = menu.addMenu(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy", QStringLiteral("复制")));
    QAction* copyCurrentColumnAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), kernelText("kernel.hooks.menu.copy_current_column", QStringLiteral("复制当前列（选中行）")));
    QAction* copyRowsAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy_row", QStringLiteral("复制选中行（TSV）")));
    QAction* copyRowsWithHeaderAction = copyMenu->addAction(kernelText("kernel.hooks.menu.copy_header_rows", QStringLiteral("复制表头+选中行（TSV）")));
    QAction* copyDetailAction = copyMenu->addAction(kernelText("kernel.hooks.menu.copy_detail", QStringLiteral("复制详情（选中行）")));
    copyMenu->addSeparator();
    QMenu* columnMenu = copyMenu->addMenu(kernelText("kernel.hooks.menu.copy_columns", QStringLiteral("复制指定栏目（选中行）")));
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
    if (selectedAction == uploadVirusTotalAction)
    {
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
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), kernelText("kernel.hooks.iat.menu.rescan", QStringLiteral("重新扫描 IAT/EAT")));
    menu.addSeparator();
    QMenu* copyMenu = menu.addMenu(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy", QStringLiteral("复制")));
    QAction* copyCurrentColumnAction = copyMenu->addAction(QIcon(":/Icon/process_copy_cell.svg"), kernelText("kernel.hooks.menu.copy_current_column", QStringLiteral("复制当前列（选中行）")));
    QAction* copyRowsAction = copyMenu->addAction(QIcon(":/Icon/process_copy_row.svg"), kernelText("kernel.context.menu.copy_row", QStringLiteral("复制选中行（TSV）")));
    QAction* copyRowsWithHeaderAction = copyMenu->addAction(kernelText("kernel.hooks.menu.copy_header_rows", QStringLiteral("复制表头+选中行（TSV）")));
    QAction* copyDetailAction = copyMenu->addAction(kernelText("kernel.hooks.menu.copy_detail", QStringLiteral("复制详情（选中行）")));
    copyMenu->addSeparator();
    QMenu* columnMenu = copyMenu->addMenu(kernelText("kernel.hooks.menu.copy_columns", QStringLiteral("复制指定栏目（选中行）")));
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
        QMessageBox::warning(this, kernelText("kernel.hooks.inline.patch.title", QStringLiteral("Inline Hook 摘除")), kernelText("kernel.hooks.inline.patch.no_selection", QStringLiteral("请先选择一条 Inline Hook 记录。")));
        return;
    }

    const std::uint32_t patchBytes = inlineHookPatchLength(entry->hookType, entry->currentByteCount);
    if (patchBytes == 0U)
    {
        QMessageBox::warning(
            this,
            kernelText("kernel.hooks.inline.patch.title", QStringLiteral("Inline Hook 摘除")),
            kernelText("kernel.hooks.inline.patch.unsuitable", QStringLiteral("当前 Hook 类型不适合自动 NOP 摘除：%1")).arg(entry->hookTypeText));
        return;
    }

    const QMessageBox::StandardButton firstConfirm = QMessageBox::warning(
        this,
        kernelText("kernel.hooks.inline.patch.confirm.title", QStringLiteral("Inline Hook 摘除确认")),
        kernelText("kernel.hooks.inline.patch.confirm.message", QStringLiteral("将对内核函数写入 NOP 补丁。\n\n模块: %1\n函数: %2\n地址: %3\n类型: %4\n字节数: %5\n\n普通请求会先提交给 R0，驱动预计会返回需要强制确认。是否继续？"))
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
            kernelText("kernel.hooks.inline.patch.title", QStringLiteral("Inline Hook 摘除")),
            kernelText("kernel.hooks.inline.patch.request_failed", QStringLiteral("普通摘除请求失败：\n%1")).arg(friendlyKernelHookIoMessage(patchResult.io.message)));
        return;
    }

    if (patchResult.status == KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED)
    {
        QMessageBox warningBox(this);
        warningBox.setIcon(QMessageBox::Warning);
        warningBox.setWindowTitle(kernelText("kernel.hooks.inline.patch.force.title", QStringLiteral("强制 Inline Hook 摘除")));
        warningBox.setText(kernelText("kernel.hooks.inline.patch.force.rejected", QStringLiteral("R0 已拒绝普通内核补丁请求。")));
        warningBox.setInformativeText(
            kernelText("kernel.hooks.inline.patch.force.message", QStringLiteral("目标函数: %1!%2\n地址: %3\n补丁长度: %4 字节\nR0状态: %5\nLastStatus: %6\n\n强制继续会修改内核代码页，只应在确认目标和字节快照无误时使用。"))
            .arg(kernelHookSafeText(entry->moduleNameText))
            .arg(kernelHookSafeText(entry->functionNameText))
            .arg(kernelHookFormatAddress(entry->functionAddress))
            .arg(patchBytes)
            .arg(kernelHookStatusText(patchResult.status))
            .arg(kernelHookFormatNtStatus(patchResult.lastStatus)));
        warningBox.setStandardButtons(QMessageBox::Cancel);
        warningBox.setDefaultButton(QMessageBox::Cancel);
        QPushButton* forceButton = warningBox.addButton(kernelText("kernel.hooks.inline.patch.force.continue", QStringLiteral("强制继续")), QMessageBox::DestructiveRole);
        warningBox.exec();
        if (warningBox.clickedButton() != forceButton)
        {
            if (m_inlineHookStatusLabel != nullptr)
            {
                m_inlineHookStatusLabel->setText(kernelText("kernel.hooks.inline.patch.force.cancelled", QStringLiteral("状态：用户取消强制 Inline Hook 摘除")));
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

    const QString resultText = kernelText("kernel.hooks.inline.patch.result", QStringLiteral(
        "Inline Hook 摘除结果\n"
        "函数: %1!%2\n"
        "地址: %3\n"
        "状态: %4\n"
        "写入字节: %5\n"
        "LastStatus: %6\n"
        "R3信息: %7"))
        .arg(kernelHookSafeText(entry->moduleNameText))
        .arg(kernelHookSafeText(entry->functionNameText))
        .arg(kernelHookFormatAddress(entry->functionAddress))
        .arg(kernelHookStatusText(patchResult.status))
        .arg(patchResult.bytesPatched)
        .arg(kernelHookFormatNtStatus(patchResult.lastStatus))
        .arg(friendlyKernelHookIoMessage(patchResult.io.message));

    if (m_inlineHookDetailEditor != nullptr)
    {
        m_inlineHookDetailEditor->setText(resultText);
    }
    if (m_inlineHookStatusLabel != nullptr)
    {
        m_inlineHookStatusLabel->setText(kernelText("kernel.hooks.inline.patch.status", QStringLiteral("状态：%1，写入 %2 字节"))
            .arg(kernelHookStatusText(patchResult.status))
            .arg(patchResult.bytesPatched));
        m_inlineHookStatusLabel->setStyleSheet(kernelHookStatusLabelStyle(
            patchResult.status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED ? KswordTheme::SuccessHex() : KswordTheme::ErrorHex()));
    }

    if (patchResult.status == KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED)
    {
        refreshInlineHooksAsync();
    }
    else
    {
        QMessageBox::warning(this, kernelText("kernel.hooks.inline.patch.title", QStringLiteral("Inline Hook 摘除")), resultText);
    }
}
