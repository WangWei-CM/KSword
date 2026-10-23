#include "KernelCleanImageBaseline.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../Internationalization/LanguageManager.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <mscat.h>
#include <Softpub.h>
#include <WinTrust.h>
#include <winternl.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#pragma comment(lib, "Wintrust.lib")

namespace
{
    constexpr unsigned long kSystemModuleInformationClass = 11UL;
    thread_local bool g_deferBaselineTranslation = false;

    QString baselineText(const QString& sourceText)
    {
        // 扫描只生成源文案；结果回到 UI 后再翻译，避免并发读取语言包。
        if (g_deferBaselineTranslation)
        {
            return sourceText;
        }
        return ks::i18n::sourceText(sourceText);
    }

    struct KernelModuleRow
    {
        HANDLE section;
        PVOID mappedBase;
        PVOID imageBase;
        ULONG imageSize;
        ULONG flags;
        USHORT loadOrderIndex;
        USHORT initOrderIndex;
        USHORT loadCount;
        USHORT fileNameOffset;
        UCHAR fullPathName[256];
    };

    struct KernelModuleList
    {
        ULONG count;
        KernelModuleRow rows[1];
    };

    struct LoadedModule
    {
        std::uint64_t base = 0;
        std::uint32_t size = 0;
        QString ntPath;
        QString filePath;
        QString name;
    };

    using NtQuerySystemInformationFunction =
        NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

    class ReadOnlyTrustFile final
    {
    public:
        explicit ReadOnlyTrustFile(const QString& path)
            : handle(::CreateFileW(
                  reinterpret_cast<LPCWSTR>(path.utf16()),
                  GENERIC_READ,
                  FILE_SHARE_READ,
                  nullptr,
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                  nullptr))
        {
        }

        ~ReadOnlyTrustFile()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(handle);
            }
        }

        ReadOnlyTrustFile(const ReadOnlyTrustFile&) = delete;
        ReadOnlyTrustFile& operator=(const ReadOnlyTrustFile&) = delete;

        HANDLE handle = INVALID_HANDLE_VALUE;
    };

    class CatalogAdminContext final
    {
    public:
        ~CatalogAdminContext()
        {
            if (handle != nullptr)
            {
                ::CryptCATAdminReleaseContext(handle, 0);
            }
        }

        CatalogAdminContext(const CatalogAdminContext&) = delete;
        CatalogAdminContext& operator=(const CatalogAdminContext&) = delete;
        CatalogAdminContext() = default;

        HCATADMIN handle = nullptr;
    };

    void initializeStrictTrustData(WINTRUST_DATA& trustData)
    {
        trustData = WINTRUST_DATA{};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        trustData.dwProvFlags =
            WTD_SAFER_FLAG |
            WTD_CACHE_ONLY_URL_RETRIEVAL |
            WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
            WTD_DISABLE_MD2_MD4;
    }

    LONG runWinTrustAndClose(WINTRUST_DATA& trustData)
    {
        GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        const LONG status =
            ::WinVerifyTrust(nullptr, &policyGuid, &trustData);
        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        ::WinVerifyTrust(nullptr, &policyGuid, &trustData);
        trustData.hWVTStateData = nullptr;
        return status;
    }

    bool calculateCatalogHash(
        const HCATADMIN catalogAdmin,
        const HANDLE fileHandle,
        std::vector<BYTE>& hashOut)
    {
        LARGE_INTEGER fileStart{};
        hashOut.clear();
        ::SetFilePointerEx(fileHandle, fileStart, nullptr, FILE_BEGIN);
        DWORD hashBytes = 0U;
        if (!::CryptCATAdminCalcHashFromFileHandle2(
                catalogAdmin,
                fileHandle,
                &hashBytes,
                nullptr,
                0) ||
            hashBytes == 0U)
        {
            return false;
        }
        hashOut.resize(hashBytes);
        ::SetFilePointerEx(fileHandle, fileStart, nullptr, FILE_BEGIN);
        if (!::CryptCATAdminCalcHashFromFileHandle2(
                catalogAdmin,
                fileHandle,
                &hashBytes,
                hashOut.data(),
                0))
        {
            hashOut.clear();
            return false;
        }
        hashOut.resize(hashBytes);
        return true;
    }

    bool verifyCatalogTrust(
        const QString& path,
        const HANDLE fileHandle)
    {
        CatalogAdminContext catalogAdmin;
        GUID driverActionVerify = DRIVER_ACTION_VERIFY;
        if (!::CryptCATAdminAcquireContext2(
                &catalogAdmin.handle,
                &driverActionVerify,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0))
        {
            return false;
        }

        std::vector<BYTE> hash;
        if (!calculateCatalogHash(
                catalogAdmin.handle,
                fileHandle,
                hash))
        {
            return false;
        }
        const QByteArray fileHashBytes(
            reinterpret_cast<const char*>(hash.data()),
            static_cast<qsizetype>(hash.size()));
        const QByteArray memberTagBytes =
            fileHashBytes.toHex().toUpper();
        const QString memberTag = QString::fromLatin1(
            memberTagBytes.constData(),
            memberTagBytes.size());

        HCATINFO previousCatalog = nullptr;
        for (;;)
        {
            HCATINFO currentCatalog =
                ::CryptCATAdminEnumCatalogFromHash(
                    catalogAdmin.handle,
                    hash.data(),
                    static_cast<DWORD>(hash.size()),
                    0,
                    &previousCatalog);
            if (currentCatalog == nullptr)
            {
                // Enum consumes/releases the previous context when advancing.
                // Natural exhaustion therefore leaves no caller-owned HCATINFO.
                return false;
            }
            previousCatalog = currentCatalog;

            CATALOG_INFO catalogInfo{};
            catalogInfo.cbStruct = sizeof(catalogInfo);
            if (!::CryptCATCatalogInfoFromContext(
                    currentCatalog,
                    &catalogInfo,
                    0))
            {
                continue;
            }

            WINTRUST_CATALOG_INFO trustCatalogInfo{};
            trustCatalogInfo.cbStruct = sizeof(trustCatalogInfo);
            trustCatalogInfo.pcwszCatalogFilePath =
                catalogInfo.wszCatalogFile;
            trustCatalogInfo.pcwszMemberTag =
                reinterpret_cast<LPCWSTR>(memberTag.utf16());
            trustCatalogInfo.pcwszMemberFilePath =
                reinterpret_cast<LPCWSTR>(path.utf16());
            trustCatalogInfo.hMemberFile = fileHandle;
            trustCatalogInfo.pbCalculatedFileHash = hash.data();
            trustCatalogInfo.cbCalculatedFileHash =
                static_cast<DWORD>(hash.size());
            trustCatalogInfo.hCatAdmin = catalogAdmin.handle;

            LARGE_INTEGER fileStart{};
            ::SetFilePointerEx(
                fileHandle,
                fileStart,
                nullptr,
                FILE_BEGIN);
            WINTRUST_DATA trustData{};
            initializeStrictTrustData(trustData);
            trustData.dwUnionChoice = WTD_CHOICE_CATALOG;
            trustData.pCatalog = &trustCatalogInfo;
            if (runWinTrustAndClose(trustData) == ERROR_SUCCESS)
            {
                ::CryptCATAdminReleaseCatalogContext(
                    catalogAdmin.handle,
                    currentCatalog,
                    0);
                previousCatalog = nullptr;
                return true;
            }
        }
    }

    bool readDiskImage(
        const HANDLE fileHandle,
        QByteArray& bytesOut)
    {
        bytesOut.clear();
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        LARGE_INTEGER fileSize{};
        constexpr LONGLONG maximumImageBytes =
            512LL * 1024LL * 1024LL;
        if (!::GetFileSizeEx(fileHandle, &fileSize) ||
            fileSize.QuadPart < 0 ||
            fileSize.QuadPart > maximumImageBytes ||
            fileSize.QuadPart >
                static_cast<LONGLONG>(
                    std::numeric_limits<qsizetype>::max()))
        {
            return false;
        }
        LARGE_INTEGER fileStart{};
        if (!::SetFilePointerEx(
                fileHandle,
                fileStart,
                nullptr,
                FILE_BEGIN))
        {
            return false;
        }

        bytesOut.resize(static_cast<qsizetype>(fileSize.QuadPart));
        qsizetype offset = 0;
        while (offset < bytesOut.size())
        {
            const qsizetype remaining = bytesOut.size() - offset;
            const DWORD requestBytes = static_cast<DWORD>(
                std::min<qsizetype>(remaining, MAXDWORD));
            DWORD bytesRead = 0U;
            if (!::ReadFile(
                    fileHandle,
                    bytesOut.data() + offset,
                    requestBytes,
                    &bytesRead,
                    nullptr) ||
                bytesRead == 0U)
            {
                bytesOut.clear();
                return false;
            }
            offset += static_cast<qsizetype>(bytesRead);
        }
        return true;
    }

    bool readTrustedDiskImage(
        const QString& path,
        const HANDLE fileHandle,
        QByteArray& bytesOut)
    {
        bytesOut.clear();
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath =
            reinterpret_cast<LPCWSTR>(path.utf16());
        fileInfo.hFile = fileHandle;

        WINTRUST_DATA trustData{};
        initializeStrictTrustData(trustData);
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        const bool trusted =
            runWinTrustAndClose(trustData) == ERROR_SUCCESS ||
            verifyCatalogTrust(path, fileHandle);
        return trusted &&
            readDiskImage(fileHandle, bytesOut);
    }

    QString normalizeKernelPath(const QString& input)
    {
        QString path = input.trimmed();
        if (path.isEmpty())
        {
            return {};
        }
        path.replace(QLatin1Char('/'), QLatin1Char('\\'));
        if (path.startsWith(QStringLiteral("\\SystemRoot\\"),
                Qt::CaseInsensitive))
        {
            wchar_t windowsDirectory[MAX_PATH] = {};
            const UINT length = ::GetWindowsDirectoryW(
                windowsDirectory,
                static_cast<UINT>(std::size(windowsDirectory)));
            if (length != 0U
                && length < static_cast<UINT>(std::size(windowsDirectory)))
            {
                return QDir::cleanPath(
                    QString::fromWCharArray(windowsDirectory)
                    + path.mid(11));
            }
        }
        if (path.startsWith(QStringLiteral("\\??\\")))
        {
            return QDir::cleanPath(path.mid(4));
        }
        if (path.size() >= 3
            && path.at(1) == QLatin1Char(':')
            && path.at(2) == QLatin1Char('\\'))
        {
            return QDir::cleanPath(path);
        }
        if (path.startsWith(QStringLiteral("\\Device\\"),
                Qt::CaseInsensitive))
        {
            for (wchar_t drive = L'A'; drive <= L'Z'; ++drive)
            {
                wchar_t driveName[3] = { drive, L':', L'\0' };
                std::vector<wchar_t> target(32768U, L'\0');
                const DWORD chars = ::QueryDosDeviceW(
                    driveName,
                    target.data(),
                    static_cast<DWORD>(target.size()));
                if (chars == 0U)
                {
                    continue;
                }
                const QString devicePrefix =
                    QString::fromWCharArray(target.data());
                if (path.startsWith(devicePrefix, Qt::CaseInsensitive))
                {
                    return QDir::cleanPath(
                        QString::fromWCharArray(driveName)
                        + path.mid(devicePrefix.size()));
                }
            }
        }
        return QDir::cleanPath(path);
    }

    bool enumerateLoadedModules(
        std::vector<LoadedModule>& modulesOut,
        QString& errorTextOut)
    {
        modulesOut.clear();
        errorTextOut.clear();
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            errorTextOut = QStringLiteral("无法获取 ntdll 模块句柄。");
            return false;
        }
        const auto query = reinterpret_cast<NtQuerySystemInformationFunction>(
            ::GetProcAddress(ntdll, "NtQuerySystemInformation"));
        if (query == nullptr)
        {
            errorTextOut =
                QStringLiteral("无法解析系统模块枚举入口。");
            return false;
        }

        ULONG required = 0U;
        NTSTATUS status = query(
            kSystemModuleInformationClass,
            nullptr,
            0U,
            &required);
        if (required < sizeof(KernelModuleList))
        {
            required = 1024U * 1024U;
        }
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            std::vector<std::uint8_t> buffer(
                static_cast<std::size_t>(required) + 64U * 1024U,
                0U);
            ULONG returned = 0U;
            status = query(
                kSystemModuleInformationClass,
                buffer.data(),
                static_cast<ULONG>(buffer.size()),
                &returned);
            if (status == static_cast<NTSTATUS>(0xC0000004L))
            {
                required = std::max<ULONG>(
                    returned,
                    static_cast<ULONG>(buffer.size() * 2U));
                continue;
            }
            if (status < 0)
            {
                errorTextOut = QStringLiteral(
                    "系统模块枚举失败，NTSTATUS=0x%1。")
                    .arg(static_cast<unsigned long>(status),
                        8,
                        16,
                        QChar('0'));
                return false;
            }

            const auto* list =
                reinterpret_cast<const KernelModuleList*>(buffer.data());
            const std::size_t maximumRows =
                (buffer.size() - offsetof(KernelModuleList, rows))
                / sizeof(KernelModuleRow);
            const std::size_t rows = std::min<std::size_t>(
                list->count,
                maximumRows);
            modulesOut.reserve(rows);
            for (std::size_t index = 0; index < rows; ++index)
            {
                const KernelModuleRow& source = list->rows[index];
                LoadedModule module;
                module.base = reinterpret_cast<std::uintptr_t>(
                    source.imageBase);
                module.size = source.imageSize;
                module.ntPath = QString::fromLocal8Bit(
                    reinterpret_cast<const char*>(source.fullPathName),
                    static_cast<int>(strnlen_s(
                        reinterpret_cast<const char*>(
                            source.fullPathName),
                        sizeof(source.fullPathName))));
                module.filePath = normalizeKernelPath(module.ntPath);
                const int nameOffset = std::min<int>(
                    source.fileNameOffset,
                    module.ntPath.size());
                module.name = module.ntPath.mid(nameOffset);
                modulesOut.push_back(std::move(module));
            }
            return true;
        }
        errorTextOut = QStringLiteral("系统模块列表在重试后仍持续变化。");
        return false;
    }

    const LoadedModule* moduleForAddress(
        const std::vector<LoadedModule>& modules,
        const std::uint64_t address)
    {
        for (const LoadedModule& module : modules)
        {
            if (address >= module.base
                && address - module.base < module.size)
            {
                return &module;
            }
        }
        return nullptr;
    }

    template <typename T>
    bool copyStructure(
        const QByteArray& bytes,
        const std::uint64_t offset,
        T& valueOut)
    {
        if (offset > static_cast<std::uint64_t>(bytes.size())
            || sizeof(T)
                > static_cast<std::uint64_t>(bytes.size()) - offset)
        {
            return false;
        }
        std::memcpy(
            &valueOut,
            bytes.constData() + static_cast<qsizetype>(offset),
            sizeof(T));
        return true;
    }

    struct PeIdentity
    {
        bool valid = false;
        std::uint16_t machine = 0;
        std::uint32_t timestamp = 0;
        std::uint32_t sizeOfImage = 0;
        std::uint32_t checkSum = 0;
        std::uint32_t sizeOfHeaders = 0;
        std::uint32_t relocationRva = 0;
        std::uint32_t relocationSize = 0;
        std::uint64_t preferredImageBase = 0;
        std::uint64_t sectionTableOffset = 0;
        std::uint16_t sectionCount = 0;
    };

    bool parsePeIdentity(
        const QByteArray& bytes,
        PeIdentity& identityOut,
        QString& errorTextOut)
    {
        identityOut = {};
        IMAGE_DOS_HEADER dos = {};
        if (!copyStructure(bytes, 0U, dos)
            || dos.e_magic != IMAGE_DOS_SIGNATURE
            || dos.e_lfanew <= 0)
        {
            errorTextOut = QStringLiteral("映像 DOS 头无效。");
            return false;
        }

        const std::uint64_t ntOffset =
            static_cast<std::uint32_t>(dos.e_lfanew);
        DWORD signature = 0U;
        IMAGE_FILE_HEADER fileHeader = {};
        if (!copyStructure(bytes, ntOffset, signature)
            || signature != IMAGE_NT_SIGNATURE
            || !copyStructure(
                bytes,
                ntOffset + sizeof(signature),
                fileHeader))
        {
            errorTextOut = QStringLiteral("映像 PE 头无效。");
            return false;
        }

        const std::uint64_t optionalOffset =
            ntOffset + sizeof(signature) + sizeof(fileHeader);
        WORD magic = 0U;
        if (!copyStructure(bytes, optionalOffset, magic))
        {
            errorTextOut = QStringLiteral("映像可选头缺失。");
            return false;
        }
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER64 optional = {};
            if (!copyStructure(bytes, optionalOffset, optional))
            {
                errorTextOut = QStringLiteral("PE32+ 可选头不完整。");
                return false;
            }
            identityOut.sizeOfImage = optional.SizeOfImage;
            identityOut.checkSum = optional.CheckSum;
            identityOut.sizeOfHeaders = optional.SizeOfHeaders;
            identityOut.preferredImageBase = optional.ImageBase;
            if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
            {
                identityOut.relocationRva =
                    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]
                        .VirtualAddress;
                identityOut.relocationSize =
                    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]
                        .Size;
            }
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 optional = {};
            if (!copyStructure(bytes, optionalOffset, optional))
            {
                errorTextOut = QStringLiteral("PE32 可选头不完整。");
                return false;
            }
            identityOut.sizeOfImage = optional.SizeOfImage;
            identityOut.checkSum = optional.CheckSum;
            identityOut.sizeOfHeaders = optional.SizeOfHeaders;
            identityOut.preferredImageBase = optional.ImageBase;
            if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
            {
                identityOut.relocationRva =
                    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]
                        .VirtualAddress;
                identityOut.relocationSize =
                    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]
                        .Size;
            }
        }
        else
        {
            errorTextOut = QStringLiteral("映像可选头类型未知。");
            return false;
        }

        if (fileHeader.NumberOfSections == 0U
            || fileHeader.NumberOfSections > 128U)
        {
            errorTextOut = QStringLiteral("映像区段数量异常。");
            return false;
        }
        identityOut.timestamp = fileHeader.TimeDateStamp;
        identityOut.machine = fileHeader.Machine;
        identityOut.sectionCount = fileHeader.NumberOfSections;
        identityOut.sectionTableOffset =
            optionalOffset + fileHeader.SizeOfOptionalHeader;
        identityOut.valid = true;
        return true;
    }

    std::vector<std::uint8_t> loadedHeaderBytes(
        std::uint64_t moduleBase,
        QString& errorTextOut,
        ksword::memory_backend::MemoryAccessBackend backend);

    bool mapPeImage(
        const QByteArray& diskImage,
        const PeIdentity& identity,
        const std::uint64_t loadedBase,
        std::vector<std::uint8_t>& mappedImageOut,
        bool& relocationAppliedOut,
        QString& errorTextOut)
    {
        constexpr std::uint32_t maximumMappedImageBytes =
            512U * 1024U * 1024U;
        mappedImageOut.clear();
        relocationAppliedOut = false;
        if (!identity.valid
            || identity.sizeOfImage == 0U
            || identity.sizeOfImage > maximumMappedImageBytes)
        {
            errorTextOut =
                baselineText(QStringLiteral("映像 SizeOfImage 无效或超过取证上限。"));
            return false;
        }

        mappedImageOut.assign(identity.sizeOfImage, 0U);
        const std::size_t headerBytes = std::min<std::size_t>(
            {
                mappedImageOut.size(),
                static_cast<std::size_t>(diskImage.size()),
                static_cast<std::size_t>(identity.sizeOfHeaders)
            });
        if (headerBytes == 0U)
        {
            errorTextOut = baselineText(QStringLiteral("映像头范围为空。"));
            return false;
        }
        std::memcpy(
            mappedImageOut.data(),
            diskImage.constData(),
            headerBytes);

        for (std::uint16_t index = 0;
             index < identity.sectionCount;
             ++index)
        {
            IMAGE_SECTION_HEADER section = {};
            const std::uint64_t sectionOffset =
                identity.sectionTableOffset
                + static_cast<std::uint64_t>(index)
                    * sizeof(IMAGE_SECTION_HEADER);
            if (!copyStructure(diskImage, sectionOffset, section))
            {
                errorTextOut =
                    baselineText(QStringLiteral("映像区段表读取失败。"));
                return false;
            }
            if (section.SizeOfRawData == 0U)
            {
                continue;
            }
            if (section.VirtualAddress >= mappedImageOut.size()
                || section.PointerToRawData
                    >= static_cast<std::uint32_t>(diskImage.size()))
            {
                errorTextOut =
                    baselineText(QStringLiteral("映像区段范围越界。"));
                return false;
            }
            const std::size_t mappedRemaining =
                mappedImageOut.size() - section.VirtualAddress;
            const std::size_t fileRemaining =
                static_cast<std::size_t>(diskImage.size())
                - section.PointerToRawData;
            const std::size_t copyBytes = std::min<std::size_t>(
                {
                    static_cast<std::size_t>(section.SizeOfRawData),
                    mappedRemaining,
                    fileRemaining
                });
            if (copyBytes != section.SizeOfRawData)
            {
                errorTextOut =
                    baselineText(QStringLiteral("映像区段原始数据被截断。"));
                return false;
            }
            std::memcpy(
                mappedImageOut.data() + section.VirtualAddress,
                diskImage.constData() + section.PointerToRawData,
                copyBytes);
        }

        const std::uint64_t relocationDelta =
            loadedBase - identity.preferredImageBase;
        if (relocationDelta == 0U)
        {
            return true;
        }
        if (identity.relocationRva == 0U
            || identity.relocationSize < sizeof(IMAGE_BASE_RELOCATION)
            || identity.relocationRva >= mappedImageOut.size()
            || identity.relocationSize
                > mappedImageOut.size() - identity.relocationRva)
        {
            errorTextOut = baselineText(QStringLiteral("映像发生基址变化，但重定位目录不可用。"));
            return false;
        }

        std::uint32_t consumed = 0U;
        while (consumed < identity.relocationSize)
        {
            if (identity.relocationSize - consumed
                    < sizeof(IMAGE_BASE_RELOCATION))
            {
                errorTextOut =
                    baselineText(QStringLiteral("PE 重定位块头被截断。"));
                return false;
            }
            const std::size_t blockOffset =
                static_cast<std::size_t>(identity.relocationRva)
                + consumed;
            IMAGE_BASE_RELOCATION block = {};
            std::memcpy(
                &block,
                mappedImageOut.data() + blockOffset,
                sizeof(block));
            if (block.SizeOfBlock < sizeof(block)
                || block.SizeOfBlock
                    > identity.relocationSize - consumed)
            {
                errorTextOut =
                    baselineText(QStringLiteral("PE 重定位块长度无效。"));
                return false;
            }
            const std::uint32_t entryBytes =
                block.SizeOfBlock - sizeof(block);
            if ((entryBytes % sizeof(WORD)) != 0U)
            {
                errorTextOut =
                    baselineText(QStringLiteral("PE 重定位项未按 WORD 对齐。"));
                return false;
            }
            const std::uint32_t entryCount =
                entryBytes / sizeof(WORD);
            for (std::uint32_t entryIndex = 0U;
                 entryIndex < entryCount;
                 ++entryIndex)
            {
                WORD entry = 0U;
                std::memcpy(
                    &entry,
                    mappedImageOut.data()
                        + blockOffset + sizeof(block)
                        + entryIndex * sizeof(WORD),
                    sizeof(entry));
                const WORD type = static_cast<WORD>(entry >> 12);
                const std::uint64_t targetRva =
                    static_cast<std::uint64_t>(block.VirtualAddress)
                    + (entry & 0x0FFFU);
                if (type == IMAGE_REL_BASED_ABSOLUTE)
                {
                    continue;
                }
                if (type == IMAGE_REL_BASED_DIR64)
                {
                    if (targetRva > mappedImageOut.size()
                        || sizeof(std::uint64_t)
                            > mappedImageOut.size()
                                - static_cast<std::size_t>(targetRva))
                    {
                        errorTextOut =
                            baselineText(QStringLiteral("DIR64 重定位目标越界。"));
                        return false;
                    }
                    std::uint64_t value = 0U;
                    std::memcpy(
                        &value,
                        mappedImageOut.data()
                            + static_cast<std::size_t>(targetRva),
                        sizeof(value));
                    value += relocationDelta;
                    std::memcpy(
                        mappedImageOut.data()
                            + static_cast<std::size_t>(targetRva),
                        &value,
                        sizeof(value));
                    relocationAppliedOut = true;
                    continue;
                }
                if (type == IMAGE_REL_BASED_HIGHLOW)
                {
                    if (targetRva > mappedImageOut.size()
                        || sizeof(std::uint32_t)
                            > mappedImageOut.size()
                                - static_cast<std::size_t>(targetRva))
                    {
                        errorTextOut =
                            baselineText(QStringLiteral("HIGHLOW 重定位目标越界。"));
                        return false;
                    }
                    std::uint32_t value = 0U;
                    std::memcpy(
                        &value,
                        mappedImageOut.data()
                            + static_cast<std::size_t>(targetRva),
                        sizeof(value));
                    value += static_cast<std::uint32_t>(
                        relocationDelta);
                    std::memcpy(
                        mappedImageOut.data()
                            + static_cast<std::size_t>(targetRva),
                        &value,
                        sizeof(value));
                    relocationAppliedOut = true;
                    continue;
                }
                errorTextOut = baselineText(QStringLiteral("PE 含有当前取证器不支持的重定位类型 %1。"))
                    .arg(type);
                return false;
            }
            consumed += block.SizeOfBlock;
        }
        return true;
    }

    QString thumbprintText(
        const KSWORD_ARK_QUERY_IMAGE_SIGNATURE_RESPONSE& response)
    {
        const std::uint32_t byteCount = std::min<std::uint32_t>(
            response.thumbprintSize,
            KSWORD_ARK_TRUST_THUMBPRINT_MAX_BYTES);
        return QString::fromLatin1(
            QByteArray(
                reinterpret_cast<const char*>(response.thumbprint),
                static_cast<qsizetype>(byteCount))
                .toHex());
    }

    struct PreparedTrustedImage
    {
        ksword::memory_backend::MemoryAccessBackend backend =
            ksword::memory_backend::MemoryAccessBackend::StandardDriver;
        LoadedModule module;
        PeIdentity identity;
        QByteArray diskImage;
        std::vector<std::uint8_t> mappedImage;
        QString sha256;
        QString signingThumbprint;
        std::uint32_t signingLevel = 0;
        bool diskTrustVerified = false;
        bool relocationApplied = false;
    };

    bool prepareTrustedImage(
        const LoadedModule& module,
        const bool requireTrustedDiskImage,
        PreparedTrustedImage& preparedOut,
        QString& errorTextOut,
        const ksword::memory_backend::MemoryAccessBackend backend =
            ksword::memory_backend::MemoryAccessBackend::StandardDriver)
    {
        preparedOut = {};
        preparedOut.backend = backend;
        preparedOut.module = module;
        if (module.filePath.isEmpty()
            || !QFileInfo::exists(module.filePath))
        {
            errorTextOut = baselineText(QStringLiteral("模块磁盘路径不可用：%1")).arg(module.ntPath);
            return false;
        }

        ReadOnlyTrustFile imageFile(module.filePath);
        if (imageFile.handle == INVALID_HANDLE_VALUE)
        {
            errorTextOut = baselineText(QStringLiteral("无法读取模块磁盘映像：%1"))
                .arg(QDir::toNativeSeparators(module.filePath));
            return false;
        }
        if (requireTrustedDiskImage)
        {
            if (!readTrustedDiskImage(
                    module.filePath,
                    imageFile.handle,
                    preparedOut.diskImage))
            {
                errorTextOut = baselineText(QStringLiteral(
                    "磁盘映像未通过 embedded/catalog 完整链信任验证，拒绝建立安全基线。"));
                return false;
            }
            preparedOut.diskTrustVerified = true;
        }
        else
        {
            if (!readDiskImage(
                    imageFile.handle,
                    preparedOut.diskImage))
            {
                errorTextOut = baselineText(QStringLiteral("无法读取模块磁盘映像：%1"))
                    .arg(QDir::toNativeSeparators(module.filePath));
                return false;
            }
        }
        if (preparedOut.diskImage.isEmpty())
        {
            errorTextOut = baselineText(QStringLiteral("模块磁盘映像为空。"));
            return false;
        }
        preparedOut.sha256 = QString::fromLatin1(
            QCryptographicHash::hash(
                preparedOut.diskImage,
                QCryptographicHash::Sha256)
                .toHex());
        if (!parsePeIdentity(
            preparedOut.diskImage,
            preparedOut.identity,
            errorTextOut))
        {
            return false;
        }

        const std::vector<std::uint8_t> memoryHeader =
            loadedHeaderBytes(module.base, errorTextOut, backend);
        if (memoryHeader.empty())
        {
            return false;
        }
        const QByteArray memoryHeaderArray(
            reinterpret_cast<const char*>(memoryHeader.data()),
            static_cast<qsizetype>(memoryHeader.size()));
        PeIdentity memoryIdentity;
        if (!parsePeIdentity(
            memoryHeaderArray,
            memoryIdentity,
            errorTextOut))
        {
            errorTextOut = baselineText(QStringLiteral("已加载映像身份读取失败：%1")).arg(errorTextOut);
            return false;
        }
        if (preparedOut.identity.machine != memoryIdentity.machine
            || preparedOut.identity.timestamp != memoryIdentity.timestamp
            || preparedOut.identity.sizeOfImage != memoryIdentity.sizeOfImage
            || preparedOut.identity.checkSum != memoryIdentity.checkSum
            || preparedOut.identity.sizeOfImage != module.size)
        {
            errorTextOut = baselineText(QStringLiteral("磁盘映像与已加载模块的机器类型、时间戳、映像大小或校验和不一致。"));
            return false;
        }

        const ksword::ark::DriverClient client;
        const ksword::ark::ImageSignatureQueryResult signature =
            client.queryImageSignature(
                module.ntPath.toStdWString(),
                module.base,
                KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_DEFAULT
                    | KSWORD_ARK_IMAGE_SIGNATURE_QUERY_FLAG_MATCH_LOADED_MODULE);
        const std::uint32_t requiredFields =
            KSWORD_ARK_IMAGE_SIGNATURE_FIELD_SIGNING_LEVEL
            | KSWORD_ARK_IMAGE_SIGNATURE_FIELD_LOADED_MODULE
            | KSWORD_ARK_IMAGE_SIGNATURE_FIELD_LOADED_MODULE_NAME_MATCH;
        if (!signature.io.ok
            || (signature.response.fieldFlags & requiredFields)
                != requiredFields
            || signature.response.signingLevelStatus < 0
            || signature.response.loadedModuleStatus < 0
            || signature.response.matchedModuleBase != module.base
            || signature.response.signingLevel
                < KSWORD_ARK_SIGNING_LEVEL_AUTHENTICODE
            || (signature.response.structuralFlags
                & KSWORD_ARK_IMAGE_SIGNATURE_STRUCT_LOADED_NAME_MISMATCH)
                != 0U)
        {
            errorTextOut = baselineText(QStringLiteral("内核 Code Integrity 签名级别或已加载模块身份证据不足，拒绝建立可信基线。"));
            return false;
        }
        preparedOut.signingLevel = signature.response.signingLevel;
        preparedOut.signingThumbprint =
            thumbprintText(signature.response);

        return mapPeImage(
            preparedOut.diskImage,
            preparedOut.identity,
            module.base,
            preparedOut.mappedImage,
            preparedOut.relocationApplied,
            errorTextOut);
    }

    const PreparedTrustedImage* cachedTrustedImage(
        const LoadedModule& module,
        QString& errorTextOut,
        const ksword::memory_backend::MemoryAccessBackend backend =
            ksword::memory_backend::MemoryAccessBackend::StandardDriver)
    {
        thread_local std::vector<PreparedTrustedImage> cache;
        const auto existing = std::find_if(
            cache.cbegin(),
            cache.cend(),
            [&module, backend](const PreparedTrustedImage& candidate)
            {
                return candidate.backend == backend
                    && candidate.module.base == module.base
                    && candidate.module.size == module.size
                    && candidate.module.filePath.compare(
                        module.filePath,
                        Qt::CaseInsensitive) == 0;
            });
        if (existing != cache.cend())
        {
            return &(*existing);
        }

        PreparedTrustedImage prepared;
        if (!prepareTrustedImage(
                module,
                false,
                prepared,
                errorTextOut,
                backend))
        {
            return nullptr;
        }
        cache.push_back(std::move(prepared));
        return &cache.back();
    }

    struct IdtBaselineProfile
    {
        std::uint32_t tableRva = 0;
        std::array<std::vector<std::uint64_t>, 256>
            handlerAddresses;
        QString sourceSymbol;
        QString profilePath;
    };

    std::optional<std::uint32_t> jsonUInt32(
        const QJsonValue& value)
    {
        if (value.isDouble())
        {
            const double number = value.toDouble(-1.0);
            if (number < 0.0
                || number
                    > static_cast<double>(
                        std::numeric_limits<std::uint32_t>::max())
                || number != static_cast<double>(
                    static_cast<std::uint32_t>(number)))
            {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(number);
        }
        if (!value.isString())
        {
            return std::nullopt;
        }
        QString text = value.toString().trimmed();
        int base = 10;
        if (text.startsWith(QStringLiteral("0x"),
                Qt::CaseInsensitive))
        {
            text = text.mid(2);
            base = 16;
        }
        bool converted = false;
        const qulonglong parsed = text.toULongLong(
            &converted,
            base);
        if (!converted
            || parsed
                > std::numeric_limits<std::uint32_t>::max())
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(parsed);
    }

    QStringList idtProfileCandidatePaths()
    {
        QStringList paths;
        const QStringList roots = {
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("profiles")),
            QDir::current().filePath(QStringLiteral("profiles"))
        };
        QSet<QString> seen;
        for (const QString& root : roots)
        {
            const QDir directory(root);
            const QString path =
                QFileInfo(directory.filePath(QStringLiteral("ark_dyndata_pack_v4.json")))
                    .absoluteFilePath();
            const QString key =
                QDir::cleanPath(path).toLower();
            if (!seen.contains(key) && QFileInfo::exists(path))
            {
                seen.insert(key);
                paths.push_back(path);
            }
        }
        return paths;
    }

    bool idtProfileEntryMatches(
        const QJsonObject& profile,
        const PreparedTrustedImage& image,
        const bool compactPack)
    {
        const QJsonObject identity = compactPack
            ? profile
            : profile.value(QStringLiteral("module")).toObject();
        const std::optional<std::uint32_t> machine =
            jsonUInt32(identity.value(QStringLiteral("machine")));
        const std::optional<std::uint32_t> timestamp =
            jsonUInt32(identity.value(QStringLiteral("timeDateStamp")));
        const std::optional<std::uint32_t> size =
            jsonUInt32(identity.value(QStringLiteral("sizeOfImage")));
        return machine.has_value()
            && timestamp.has_value()
            && size.has_value()
            && machine.value() == image.identity.machine
            && timestamp.value() == image.identity.timestamp
            && size.value() == image.identity.sizeOfImage;
    }

    bool loadIdtBaselineProfile(
        const PreparedTrustedImage& image,
        IdtBaselineProfile& profileOut,
        QString& errorTextOut)
    {
        profileOut = {};
        bool identitySeen = false;
        bool hashSeen = false;
        bool invalidMetadataSeen = false;
        for (const QString& path : idtProfileCandidatePaths())
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                continue;
            }
            QJsonParseError parseError = {};
            const QJsonDocument document =
                QJsonDocument::fromJson(file.readAll(), &parseError);
            file.close();
            if (parseError.error != QJsonParseError::NoError
                || !document.isObject())
            {
                continue;
            }

            const QJsonObject root = document.object();
            const bool compactPack =
                root.value(QStringLiteral("profiles")).isArray();
            QJsonArray entries;
            if (compactPack)
            {
                entries =
                    root.value(QStringLiteral("profiles")).toArray();
            }
            else
            {
                entries.append(root);
            }
            for (const QJsonValue& entryValue : entries)
            {
                if (!entryValue.isObject())
                {
                    continue;
                }
                const QJsonObject entry = entryValue.toObject();
                if (!idtProfileEntryMatches(
                    entry,
                    image,
                    compactPack))
                {
                    continue;
                }
                identitySeen = true;
                const QJsonObject identity = compactPack
                    ? entry
                    : entry.value(QStringLiteral("module")).toObject();
                const QString profileHash =
                    identity.value(QStringLiteral("sha256"))
                        .toString()
                        .trimmed()
                        .toLower();
                if (profileHash.size() != 64
                    || profileHash != image.sha256.toLower())
                {
                    continue;
                }
                hashSeen = true;
                const QJsonObject baseline =
                    entry.value(QStringLiteral("idtBaseline"))
                        .toObject();
                const std::optional<std::uint32_t> schemaVersion =
                    jsonUInt32(
                        baseline.value(QStringLiteral("schemaVersion")));
                const std::optional<std::uint32_t> tableRva =
                    jsonUInt32(
                        baseline.value(QStringLiteral("tableRva")));
                const QString sourceSymbol =
                    baseline.value(QStringLiteral("sourceSymbol"))
                        .toString()
                        .trimmed();
                const QJsonArray handlers =
                    baseline.value(QStringLiteral("handlers")).toArray();
                if (!schemaVersion.has_value()
                    || schemaVersion.value() != 2U
                    || !tableRva.has_value()
                    || tableRva.value() == 0U
                    || tableRva.value() >= image.mappedImage.size()
                    || handlers.isEmpty()
                    || sourceSymbol
                        != QStringLiteral("KiInterruptInitTable"))
                {
                    invalidMetadataSeen = true;
                    continue;
                }
                IdtBaselineProfile parsedProfile;
                parsedProfile.tableRva = tableRva.value();
                parsedProfile.sourceSymbol = sourceSymbol;
                parsedProfile.profilePath = path;
                std::size_t acceptedHandlers = 0U;
                bool invalidHandlerSeen = false;
                for (const QJsonValue& handlerValue : handlers)
                {
                    const QJsonObject handler =
                        handlerValue.toObject();
                    const std::optional<std::uint32_t> vector =
                        jsonUInt32(
                            handler.value(QStringLiteral("vector")));
                    const std::optional<std::uint32_t> handlerRva =
                        jsonUInt32(
                            handler.value(QStringLiteral("rva")));
                    const QString handlerSymbol =
                        handler.value(QStringLiteral("symbol"))
                            .toString()
                            .trimmed();
                    if (!vector.has_value()
                        || vector.value() > 0xFFU
                        || !handlerRva.has_value()
                        || handlerRva.value() == 0U
                        || handlerRva.value() >= image.module.size
                        || !handlerSymbol.startsWith(
                            QStringLiteral("Ki")))
                    {
                        invalidHandlerSeen = true;
                        break;
                    }
                    const std::uint64_t handlerAddress =
                        image.module.base + handlerRva.value();
                    std::vector<std::uint64_t>& candidates =
                        parsedProfile.handlerAddresses[vector.value()];
                    if (std::find(
                        candidates.cbegin(),
                        candidates.cend(),
                        handlerAddress) == candidates.cend())
                    {
                        candidates.push_back(handlerAddress);
                        ++acceptedHandlers;
                    }
                }
                if (invalidHandlerSeen || acceptedHandlers == 0U)
                {
                    invalidMetadataSeen = true;
                    continue;
                }
                profileOut = std::move(parsedProfile);
                return true;
            }
        }

        if (hashSeen)
        {
            errorTextOut = invalidMetadataSeen
                ? QStringLiteral(
                    "精确 SHA256 profile 的 IDT 基线元数据缺失或无效，IDT 预期处理程序 unsupported。")
                : QStringLiteral(
                    "精确 SHA256 profile 未提供 KiInterruptInitTable，IDT 预期处理程序 unsupported。");
        }
        else if (identitySeen)
        {
            errorTextOut = QStringLiteral(
                "PDB profile 的 PE 身份匹配但 SHA256 不匹配，拒绝使用。");
        }
        else
        {
            errorTextOut = QStringLiteral(
                "没有与当前 ntoskrnl 机器类型、时间戳、SizeOfImage 和 SHA256 精确匹配的 IDT profile。");
        }
        return false;
    }

    std::vector<std::uint8_t> loadedHeaderBytes(
        const std::uint64_t moduleBase,
        QString& errorTextOut,
        const ksword::memory_backend::MemoryAccessBackend backend)
    {
        std::vector<std::uint8_t> bytes;
        if (!ks::kernel::KernelCleanImageBaseline::readKernelBytes(
            moduleBase,
            64U * 1024U,
            bytes,
            errorTextOut,
            backend))
        {
            return {};
        }
        return bytes;
    }

    // ---- 可执行节全量比对所需的 PE 结构辅助 ----

#pragma pack(push, 1)
    // IMAGE_DYNAMIC_RELOCATION_TABLE：动态重定位表头。
    struct DynamicRelocationTableHeader
    {
        std::uint32_t version;
        std::uint32_t size;
    };

    // IMAGE_DYNAMIC_RELOCATION64：每个符号一段，后面跟 IMAGE_BASE_RELOCATION 块。
    struct DynamicRelocation64Header
    {
        std::uint64_t symbol;
        std::uint32_t baseRelocSize;
    };
#pragma pack(pop)

    // 本工具能够解码位点偏移的动态重定位符号。它们都是 x64 上真正会改写 .text
    // 的那几类（import optimization 与两种间接控制转移改写）。
    constexpr std::uint64_t kDynamicRelocImportControlTransfer = 3ULL;
    constexpr std::uint64_t kDynamicRelocIndirControlTransfer = 4ULL;
    constexpr std::uint64_t kDynamicRelocSwitchtableBranch = 5ULL;

    // 一个动态重定位位点最多覆盖的字节数。x64 上被改写的是一条 call/jmp，
    // 取 8 字节留出前缀与 ModRM 的余量。
    constexpr std::uint32_t kDynamicRelocSiteSpan = 8U;

    // 描述一个可执行节在映像中的范围。
    struct ExecutableSection
    {
        std::uint32_t rva = 0;
        std::uint32_t size = 0;
        QString name;
    };

    QString sectionNameText(const IMAGE_SECTION_HEADER& section)
    {
        char buffer[IMAGE_SIZEOF_SHORT_NAME + 1] = { 0 };
        std::memcpy(buffer, section.Name, IMAGE_SIZEOF_SHORT_NAME);
        return QString::fromLatin1(buffer).trimmed();
    }

    // 收集映像里所有带 IMAGE_SCN_MEM_EXECUTE 的节。内存中的节长度取 VirtualSize
    // 与 SizeOfRawData 的较大值，并被截断到 SizeOfImage 之内。
    std::vector<ExecutableSection> collectExecutableSections(
        const PreparedTrustedImage& prepared)
    {
        std::vector<ExecutableSection> sections;
        for (std::uint16_t index = 0;
             index < prepared.identity.sectionCount;
             ++index)
        {
            IMAGE_SECTION_HEADER header = {};
            const std::uint64_t offset =
                prepared.identity.sectionTableOffset
                + static_cast<std::uint64_t>(index)
                    * sizeof(IMAGE_SECTION_HEADER);
            if (!copyStructure(prepared.diskImage, offset, header))
            {
                break;
            }
            if ((header.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0U)
            {
                continue;
            }
            std::uint32_t length = std::max<std::uint32_t>(
                header.Misc.VirtualSize,
                header.SizeOfRawData);
            if (length == 0U
                || header.VirtualAddress >= prepared.mappedImage.size())
            {
                continue;
            }
            const std::uint32_t available =
                static_cast<std::uint32_t>(prepared.mappedImage.size())
                - header.VirtualAddress;
            length = std::min(length, available);
            sections.push_back(
                ExecutableSection{
                    header.VirtualAddress,
                    length,
                    sectionNameText(header) });
        }
        return sections;
    }

    // 从 LoadConfig 目录定位动态重定位表，把可解码的位点 RVA 收进 sitesOut。
    // 返回 false 表示遇到了本工具不解析的符号，调用方据此降低结论强度。
    bool collectDynamicRelocationSites(
        const PreparedTrustedImage& prepared,
        std::vector<std::uint32_t>& sitesOut)
    {
        sitesOut.clear();

        IMAGE_DOS_HEADER dos = {};
        if (!copyStructure(prepared.diskImage, 0U, dos)
            || dos.e_lfanew <= 0)
        {
            return true;
        }
        IMAGE_NT_HEADERS64 ntHeaders = {};
        if (!copyStructure(
                prepared.diskImage,
                static_cast<std::uint64_t>(dos.e_lfanew),
                ntHeaders)
            || ntHeaders.OptionalHeader.Magic
                != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || ntHeaders.OptionalHeader.NumberOfRvaAndSizes
                <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
        {
            return true;
        }

        const IMAGE_DATA_DIRECTORY loadConfigDirectory =
            ntHeaders.OptionalHeader
                .DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
        if (loadConfigDirectory.VirtualAddress == 0U
            || loadConfigDirectory.Size < sizeof(std::uint32_t)
            || loadConfigDirectory.VirtualAddress
                >= prepared.mappedImage.size())
        {
            return true;
        }

        // LoadConfig 结构随版本增长，只有 Size 字段声明的部分才可以使用。
        IMAGE_LOAD_CONFIG_DIRECTORY64 loadConfig = {};
        const std::size_t copyBytes = std::min<std::size_t>(
            {
                sizeof(loadConfig),
                static_cast<std::size_t>(loadConfigDirectory.Size),
                prepared.mappedImage.size()
                    - loadConfigDirectory.VirtualAddress
            });
        std::memcpy(
            &loadConfig,
            prepared.mappedImage.data() + loadConfigDirectory.VirtualAddress,
            copyBytes);

        constexpr std::size_t requiredSize =
            offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,
                DynamicValueRelocTableSection)
            + sizeof(loadConfig.DynamicValueRelocTableSection);
        if (loadConfig.Size < requiredSize
            || copyBytes < requiredSize
            || loadConfig.DynamicValueRelocTableOffset == 0U
            || loadConfig.DynamicValueRelocTableSection == 0U)
        {
            return true;
        }

        // DynamicValueRelocTableSection 是 1 基的节序号，偏移相对该节起始。
        IMAGE_SECTION_HEADER hostSection = {};
        const std::uint16_t sectionIndex =
            static_cast<std::uint16_t>(
                loadConfig.DynamicValueRelocTableSection - 1U);
        if (sectionIndex >= prepared.identity.sectionCount
            || !copyStructure(
                prepared.diskImage,
                prepared.identity.sectionTableOffset
                    + static_cast<std::uint64_t>(sectionIndex)
                        * sizeof(IMAGE_SECTION_HEADER),
                hostSection))
        {
            return true;
        }

        const std::uint64_t tableRva =
            static_cast<std::uint64_t>(hostSection.VirtualAddress)
            + loadConfig.DynamicValueRelocTableOffset;
        if (tableRva + sizeof(DynamicRelocationTableHeader)
            > prepared.mappedImage.size())
        {
            return true;
        }

        DynamicRelocationTableHeader tableHeader = {};
        std::memcpy(
            &tableHeader,
            prepared.mappedImage.data() + tableRva,
            sizeof(tableHeader));
        if (tableHeader.version != 1U || tableHeader.size == 0U)
        {
            // 版本不认识时不做任何解码，并让调用方保守处理。
            return tableHeader.size == 0U;
        }

        const std::uint64_t entriesRva =
            tableRva + sizeof(DynamicRelocationTableHeader);
        if (entriesRva > prepared.mappedImage.size()
            || tableHeader.size
                > prepared.mappedImage.size() - entriesRva)
        {
            return true;
        }

        bool fullyParsed = true;
        std::uint32_t consumed = 0U;
        while (consumed + sizeof(DynamicRelocation64Header)
               <= tableHeader.size)
        {
            DynamicRelocation64Header entry = {};
            std::memcpy(
                &entry,
                prepared.mappedImage.data() + entriesRva + consumed,
                sizeof(entry));
            consumed += static_cast<std::uint32_t>(sizeof(entry));
            if (entry.baseRelocSize == 0U
                || entry.baseRelocSize > tableHeader.size - consumed)
            {
                fullyParsed = false;
                break;
            }

            const std::uint32_t entrySize =
                (entry.symbol == kDynamicRelocImportControlTransfer
                 || entry.symbol == kDynamicRelocSwitchtableBranch)
                    ? 4U
                    : (entry.symbol == kDynamicRelocIndirControlTransfer
                        ? 2U
                        : 0U);
            if (entrySize == 0U)
            {
                // 未知或未实现的符号：跳过它的数据，但记下解析不完整。
                fullyParsed = false;
                consumed += entry.baseRelocSize;
                continue;
            }

            std::uint32_t blockConsumed = 0U;
            while (blockConsumed + sizeof(IMAGE_BASE_RELOCATION)
                   <= entry.baseRelocSize)
            {
                IMAGE_BASE_RELOCATION block = {};
                std::memcpy(
                    &block,
                    prepared.mappedImage.data()
                        + entriesRva + consumed + blockConsumed,
                    sizeof(block));
                if (block.SizeOfBlock < sizeof(block)
                    || block.SizeOfBlock
                        > entry.baseRelocSize - blockConsumed)
                {
                    fullyParsed = false;
                    break;
                }
                const std::uint32_t payloadBytes =
                    block.SizeOfBlock
                    - static_cast<std::uint32_t>(sizeof(block));
                for (std::uint32_t offsetInPayload = 0U;
                     offsetInPayload + entrySize <= payloadBytes;
                     offsetInPayload += entrySize)
                {
                    std::uint32_t raw = 0U;
                    std::memcpy(
                        &raw,
                        prepared.mappedImage.data()
                            + entriesRva + consumed + blockConsumed
                            + sizeof(block) + offsetInPayload,
                        entrySize);
                    // 三种符号的低 12 位都是页内偏移。
                    const std::uint32_t pageOffset = raw & 0x0FFFU;
                    sitesOut.push_back(
                        block.VirtualAddress + pageOffset);
                }
                blockConsumed += block.SizeOfBlock;
            }
            consumed += entry.baseRelocSize;
        }

        std::sort(sitesOut.begin(), sitesOut.end());
        sitesOut.erase(
            std::unique(sitesOut.begin(), sitesOut.end()),
            sitesOut.end());
        return fullyParsed;
    }

    // 判断 [rva, rva+length) 是否完全落在已知动态重定位位点覆盖的范围内。
    bool rangeCoveredByDynamicRelocation(
        const std::vector<std::uint32_t>& sites,
        const std::uint32_t rva,
        const std::uint32_t length)
    {
        if (sites.empty() || length == 0U)
        {
            return false;
        }
        for (std::uint32_t offset = 0U; offset < length; ++offset)
        {
            const std::uint32_t probe = rva + offset;
            // 找到第一个起点大于 probe 的位点，它的前一个才可能覆盖 probe。
            const auto upper = std::upper_bound(
                sites.cbegin(),
                sites.cend(),
                probe);
            if (upper == sites.cbegin())
            {
                return false;
            }
            const std::uint32_t site = *std::prev(upper);
            if (probe - site >= kDynamicRelocSiteSpan)
            {
                return false;
            }
        }
        return true;
    }
}

namespace ks::kernel
{
    bool KernelCleanImageBaseline::readKernelBytes(
        const std::uint64_t kernelAddress,
        const std::uint32_t byteCount,
        std::vector<std::uint8_t>& bytesOut,
        QString& errorTextOut,
        const ksword::memory_backend::MemoryAccessBackend backend)
    {
        bytesOut.clear();
        errorTextOut.clear();
        if (kernelAddress == 0U
            || byteCount == 0U
            || byteCount > KSWORD_ARK_MEMORY_READ_MAX_BYTES)
        {
            errorTextOut = QStringLiteral("内核读取参数无效。");
            return false;
        }

        if (backend != ksword::memory_backend::MemoryAccessBackend::StandardDriver
            && backend != ksword::memory_backend::MemoryAccessBackend::Hvm)
        {
            errorTextOut = QStringLiteral("内核完整性扫描仅支持 R0 或 HVM 后端。");
            return false;
        }

        const auto read = ksword::memory_backend::readVirtual(
            backend,
            {},
            0U,
            kernelAddress,
            byteCount,
            backend == ksword::memory_backend::MemoryAccessBackend::Hvm);
        if (!read.ok || read.partial
            || read.bytesDone != byteCount
            || read.data.size() != static_cast<qsizetype>(byteCount))
        {
            errorTextOut = QStringLiteral(
                "%1 内核读取不完整：读取=%2/%3。%4")
                .arg(ksword::memory_backend::backendDisplayName(backend))
                .arg(read.bytesDone)
                .arg(byteCount)
                .arg(read.failureText);
            return false;
        }
        const auto* begin = reinterpret_cast<const std::uint8_t*>(read.data.constData());
        bytesOut.assign(begin, begin + byteCount);
        return true;
    }

    CleanImageBaselineResult KernelCleanImageBaseline::compareAddress(
        const std::uint64_t kernelAddress,
        const std::uint32_t byteCount,
        const std::vector<std::uint8_t>& observedBytes,
        const bool requireTrustedDiskImage)
    {
        CleanImageBaselineResult result;
        if (kernelAddress == 0U
            || byteCount == 0U
            || byteCount > 64U * 1024U)
        {
            result.statusText = QStringLiteral("基线请求范围无效。");
            return result;
        }

        std::vector<LoadedModule> modules;
        QString errorText;
        if (!enumerateLoadedModules(modules, errorText))
        {
            result.statusText = errorText;
            return result;
        }
        const LoadedModule* module =
            moduleForAddress(modules, kernelAddress);
        if (module == nullptr)
        {
            result.statusText =
                QStringLiteral("目标地址不属于已加载内核映像。");
            return result;
        }

        result.moduleBase = module->base;
        result.moduleSize = module->size;
        result.moduleName = module->name;
        result.imagePath = module->filePath;
        const std::uint64_t rva64 = kernelAddress - module->base;
        if (rva64 > std::numeric_limits<std::uint32_t>::max())
        {
            result.statusText = QStringLiteral("目标 RVA 超出 32 位范围。");
            return result;
        }
        result.relativeVirtualAddress =
            static_cast<std::uint32_t>(rva64);
        PreparedTrustedImage trustedDiskImage;
        const PreparedTrustedImage* prepared = nullptr;
        if (requireTrustedDiskImage)
        {
            if (!prepareTrustedImage(
                    *module,
                    true,
                    trustedDiskImage,
                    errorText))
            {
                result.statusText = errorText;
                return result;
            }
            prepared = &trustedDiskImage;
        }
        else
        {
            prepared = cachedTrustedImage(
                *module,
                errorText);
        }
        if (prepared == nullptr)
        {
            result.statusText = errorText;
            return result;
        }
        result.identityMatched = true;
        result.diskTrustVerified = prepared->diskTrustVerified;
        result.codeIntegrityTrusted = true;
        result.relocationApplied = prepared->relocationApplied;
        result.preferredImageBase =
            prepared->identity.preferredImageBase;
        result.signingLevel = prepared->signingLevel;
        result.imageSha256 = prepared->sha256;
        result.signingThumbprint = prepared->signingThumbprint;
        if (result.relativeVirtualAddress > prepared->mappedImage.size()
            || byteCount > prepared->mappedImage.size()
                - result.relativeVirtualAddress)
        {
            result.statusText =
                baselineText(QStringLiteral("目标 RVA 在已重定位映像中越界。"));
            return result;
        }
        result.cleanBytes.assign(
            prepared->mappedImage.cbegin()
                + result.relativeVirtualAddress,
            prepared->mappedImage.cbegin()
                + result.relativeVirtualAddress + byteCount);
        if (observedBytes.empty())
        {
            if (!readKernelBytes(
                kernelAddress,
                byteCount,
                result.observedBytes,
                errorText))
            {
                result.statusText = errorText;
                return result;
            }
        }
        else if (observedBytes.size() == byteCount)
        {
            result.observedBytes = observedBytes;
        }
        else
        {
            result.statusText =
                QStringLiteral("调用方提供的观察字节长度与目标范围不一致。");
            return result;
        }

        result.available = true;
        result.differs = result.cleanBytes != result.observedBytes;
        result.statusText = result.differs
            ? baselineText(QStringLiteral("当前内存与身份、SHA256、Code Integrity 签名级别绑定的重定位映像基线不一致"))
            : baselineText(QStringLiteral("当前内存与身份、SHA256、Code Integrity 签名级别绑定的重定位映像基线一致"));
        return result;
    }

    std::vector<KernelTextIntegrityResult>
    KernelCleanImageBaseline::scanExecutableSections(
        const KernelTextScanOptions& options)
    {
        // 只改变扫描调用树的翻译时机，保留其他基线调用方的原有行为。
        struct SourceTextScope final
        {
            const bool previous = g_deferBaselineTranslation;
            SourceTextScope() { g_deferBaselineTranslation = true; }
            ~SourceTextScope() { g_deferBaselineTranslation = previous; }
        } sourceTextScope;
        std::vector<KernelTextIntegrityResult> results;

        std::vector<LoadedModule> modules;
        QString errorText;
        if (!enumerateLoadedModules(modules, errorText))
        {
            KernelTextIntegrityResult failure;
            failure.backend = options.backend;
            failure.statusText = errorText;
            if (options.onModuleComplete)
            {
                options.onModuleComplete(failure);
            }
            results.push_back(std::move(failure));
            return results;
        }

        // 读块大小要同时受协议上限与调用方设置约束。
        std::uint32_t chunkBytes = options.chunkBytes;
        if (chunkBytes == 0U
            || chunkBytes > KSWORD_ARK_MEMORY_READ_MAX_BYTES)
        {
            chunkBytes = KSWORD_ARK_MEMORY_READ_MAX_BYTES;
        }

        for (const LoadedModule& module : modules)
        {
            if (options.cancelFlag != nullptr
                && options.cancelFlag->load())
            {
                break;
            }
            if (!options.moduleFilter.isEmpty()
                && !module.name.contains(
                    options.moduleFilter,
                    Qt::CaseInsensitive))
            {
                continue;
            }

            KernelTextIntegrityResult result;
            result.backend = options.backend;
            result.moduleBase = module.base;
            result.moduleSize = module.size;
            result.moduleName = module.name;
            result.imagePath = module.filePath;

            QString prepareError;
            // A whole-module scan visits each module once. Keep only this
            // module's verified disk/mapped images until its result is complete.
            // Address comparisons retain their existing per-thread reuse cache.
            PreparedTrustedImage preparedImage;
            const PreparedTrustedImage* const prepared = &preparedImage;
            if (!prepareTrustedImage(
                    module, false, preparedImage, prepareError, options.backend))
            {
                result.statusText = prepareError;
                if (options.onModuleComplete)
                {
                    options.onModuleComplete(result);
                }
                results.push_back(std::move(result));
                continue;
            }

            result.identityMatched = true;
            result.diskTrustVerified = prepared->diskTrustVerified;
            result.relocationApplied = prepared->relocationApplied;
            result.imageSha256 = prepared->sha256;

            std::vector<std::uint32_t> dynamicSites;
            result.unparsedDynamicRelocations =
                !collectDynamicRelocationSites(*prepared, dynamicSites);

            const std::vector<ExecutableSection> sections =
                collectExecutableSections(*prepared);
            result.executableSectionCount =
                static_cast<std::uint32_t>(sections.size());
            if (sections.empty())
            {
                result.statusText = baselineText(
                    QStringLiteral("该映像没有可执行节，未做比对。"));
                if (options.onModuleComplete)
                {
                    options.onModuleComplete(result);
                }
                results.push_back(std::move(result));
                continue;
            }

            bool cancelled = false;
            QString firstReadError;
            for (const ExecutableSection& section : sections)
            {
                std::uint32_t sectionOffset = 0U;
                while (sectionOffset < section.size)
                {
                    if (options.cancelFlag != nullptr
                        && options.cancelFlag->load())
                    {
                        cancelled = true;
                        break;
                    }

                    const std::uint32_t readBytes = std::min(
                        chunkBytes,
                        section.size - sectionOffset);
                    const std::uint32_t chunkRva =
                        section.rva + sectionOffset;
                    sectionOffset += readBytes;

                    std::vector<std::uint8_t> observed;
                    QString readError;
                    if (!readKernelBytes(
                            module.base + chunkRva,
                            readBytes,
                            observed,
                            readError,
                            options.backend))
                    {
                        // 读不到的块只计量，不当作差异，避免把分页失败说成篡改。
                        result.unreadableBytes += readBytes;
                        if (firstReadError.isEmpty())
                        {
                            firstReadError = readError;
                        }
                        continue;
                    }
                    result.scannedBytes += readBytes;

                    const std::uint8_t* clean =
                        prepared->mappedImage.data() + chunkRva;
                    std::uint32_t index = 0U;
                    while (index < readBytes)
                    {
                        if (clean[index] == observed[index])
                        {
                            ++index;
                            continue;
                        }
                        // 把连续不同的字节合并成一个区间再分类。
                        const std::uint32_t start = index;
                        while (index < readBytes
                               && clean[index] != observed[index])
                        {
                            ++index;
                        }
                        const std::uint32_t length = index - start;
                        result.differingBytes += length;

                        const std::uint32_t rangeRva = chunkRva + start;
                        const bool known = rangeCoveredByDynamicRelocation(
                            dynamicSites,
                            rangeRva,
                            length);
                        if (known)
                        {
                            result.knownRangeCount += 1U;
                        }
                        else
                        {
                            result.unexplainedRangeCount += 1U;
                        }

                        if (result.ranges.size()
                            >= options.maxRangesPerModule)
                        {
                            result.truncatedRangeCount += 1U;
                            continue;
                        }

                        KernelTextDiffRange range;
                        range.rva = rangeRva;
                        range.length = length;
                        range.kernelAddress = module.base + rangeRva;
                        range.origin = known
                            ? KernelTextDiffRange::Origin::KnownDynamicRelocation
                            : KernelTextDiffRange::Origin::Unexplained;
                        range.sectionName = section.name;
                        const std::uint32_t previewBytes = std::min(
                            length,
                            options.maxRangeBytes);
                        range.cleanBytes.assign(
                            clean + start,
                            clean + start + previewBytes);
                        range.observedBytes.assign(
                            observed.cbegin() + start,
                            observed.cbegin() + start + previewBytes);
                        result.ranges.push_back(std::move(range));
                    }
                }
                if (cancelled)
                {
                    break;
                }
            }

            result.available = result.scannedBytes != 0U;
            result.complete = !cancelled
                && result.unreadableBytes == 0U
                && result.available;
            if (cancelled)
            {
                result.statusText = baselineText(
                    QStringLiteral("扫描被取消，结果不完整。"));
            }
            else if (result.unreadableBytes != 0U)
            {
                result.statusText = baselineText(
                    QStringLiteral("扫描覆盖不完整：%1 字节不可读，无法确认整体代码完整性。%2"))
                    .arg(result.unreadableBytes)
                    .arg(firstReadError);
            }
            else if (result.unexplainedRangeCount != 0U)
            {
                result.statusText = baselineText(
                    QStringLiteral("发现无法用动态重定位解释的代码改写。"));
            }
            else if (result.knownRangeCount != 0U)
            {
                result.statusText = baselineText(
                    QStringLiteral("仅存在动态重定位位点差异，未见异常改写。"));
            }
            else
            {
                result.statusText = baselineText(
                    QStringLiteral("可执行节与重定位后的磁盘净映像一致。"));
            }
            if (options.onModuleComplete)
            {
                options.onModuleComplete(result);
            }
            results.push_back(std::move(result));

            if (cancelled)
            {
                break;
            }
        }

        return results;
    }

    std::vector<TrustedIdtBaselineResult>
    KernelCleanImageBaseline::compareIdtHandlers(
        const std::vector<IdtHandlerObservation>& observations)
    {
        std::vector<TrustedIdtBaselineResult> results;
        results.reserve(observations.size());
        for (const IdtHandlerObservation& observation : observations)
        {
            TrustedIdtBaselineResult row;
            row.vector = observation.vector;
            row.observedHandler = observation.handler;
            results.push_back(std::move(row));
        }
        if (observations.empty())
        {
            return results;
        }

        auto failAll = [&results](const QString& message)
        {
            for (TrustedIdtBaselineResult& row : results)
            {
                row.statusText = message;
            }
        };

        std::vector<LoadedModule> modules;
        QString errorText;
        if (!enumerateLoadedModules(modules, errorText))
        {
            failAll(errorText);
            return results;
        }
        const auto ntosIterator = std::find_if(
            modules.cbegin(),
            modules.cend(),
            [](const LoadedModule& module)
            {
                const QString fileName =
                    QFileInfo(module.filePath).fileName().toLower();
                return fileName == QStringLiteral("ntoskrnl.exe")
                    || fileName == QStringLiteral("ntkrnlmp.exe")
                    || fileName == QStringLiteral("ntkrla57.exe");
            });
        if (ntosIterator == modules.cend())
        {
            failAll(QStringLiteral(
                "无法在已加载模块列表中定位 ntoskrnl，IDT 可信映像基线 unsupported。"));
            return results;
        }

        PreparedTrustedImage image;
        if (!prepareTrustedImage(*ntosIterator, true, image, errorText))
        {
            failAll(errorText);
            return results;
        }
        IdtBaselineProfile profile;
        if (!loadIdtBaselineProfile(image, profile, errorText))
        {
            failAll(errorText);
            return results;
        }

        for (TrustedIdtBaselineResult& row : results)
        {
            row.identityMatched = true;
            row.diskTrustVerified = image.diskTrustVerified;
            row.codeIntegrityTrusted = true;
            row.profileHashMatched = true;
            row.imagePath = image.module.filePath;
            row.imageSha256 = image.sha256;
            row.profilePath = profile.profilePath;
            row.sourceSymbol = profile.sourceSymbol;
            if (row.vector > 0xFFU)
            {
                row.statusText = QStringLiteral(
                    "IDT 向量超出 x64 架构范围。");
                continue;
            }
            const std::vector<std::uint64_t>& candidates =
                profile.handlerAddresses[row.vector];
            row.expectedCandidateCount =
                static_cast<std::uint32_t>(candidates.size());
            if (candidates.empty())
            {
                row.statusText = QStringLiteral(
                    "精确 PDB 未公开该向量的静态 Handler；动态设备中断或默认 thunk 明确标记 unsupported。");
                continue;
            }
            const auto matched = std::find(
                candidates.cbegin(),
                candidates.cend(),
                row.observedHandler);
            row.handlerMatches = matched != candidates.cend();
            row.expectedHandler = row.handlerMatches
                ? row.observedHandler
                : candidates.front();
            row.available = true;
            row.statusText = row.handlerMatches
                ? QStringLiteral(
                    "当前 IDT Handler 与精确 PDB/SHA256 绑定的静态向量符号一致")
                : QStringLiteral(
                    "当前 IDT Handler 偏离精确 PDB/SHA256 绑定的静态向量符号");
        }
        return results;
    }

}
