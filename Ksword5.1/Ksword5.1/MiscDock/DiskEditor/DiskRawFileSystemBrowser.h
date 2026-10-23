#pragma once

#include "DiskFileSystemForensics.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace ks::misc
{
    enum class RawFileObjectType : int
    {
        Unknown = 0,
        RegularFile,
        Directory,
        SymbolicLink,
        Special,
        NamedStream
    };

    struct RawFileExtent
    {
        std::uint64_t logicalOffset = 0;
        std::uint64_t absoluteOffset = 0;
        std::uint64_t lengthBytes = 0;
        bool sparse = false;
        bool unwritten = false;
        bool compressed = false;
        bool physicalMappingExact = false;
    };

    struct RawFileEntry
    {
        QString name;
        QString fullPath;
        QString streamName;
        RawFileObjectType type = RawFileObjectType::Unknown;
        std::uint64_t objectId = 0;
        std::uint64_t parentObjectId = 0;
        std::uint64_t fileSizeBytes = 0;
        std::uint64_t allocatedSizeBytes = 0;
        std::uint64_t metadataOffset = 0;
        std::uint32_t modeOrFlags = 0;
        std::vector<RawFileExtent> extents;
        bool extentsTruncated = false;
    };

    struct RawDirectoryResult
    {
        bool success = false;
        ForensicFileSystemKind fileSystem =
            ForensicFileSystemKind::Unknown;
        QString fileSystemName;
        QString requestedPath;
        QString canonicalPath;
        QString errorText;
        std::uint64_t directoryObjectId = 0;
        std::uint64_t scannedRecords = 0;
        bool truncated = false;
        std::vector<RawFileEntry> entries;
    };

    struct RawFileReadResult
    {
        bool success = false;
        ForensicFileSystemKind fileSystem =
            ForensicFileSystemKind::Unknown;
        QString fileSystemName;
        QString filePath;
        QString errorText;
        std::uint64_t fileSizeBytes = 0;
        std::uint64_t requestedOffset = 0;
        bool endOfFile = false;
        QByteArray bytes;
        std::vector<RawFileExtent> extents;
        bool extentsTruncated = false;
    };

    struct RawFileExportResult
    {
        bool success = false;
        QString filePath;
        QString destinationPath;
        QString errorText;
        std::uint64_t fileSizeBytes = 0;
        std::uint64_t bytesWritten = 0;
    };

    class DiskRawFileSystemBrowser final
    {
    public:
        static RawDirectoryResult listDirectory(
            int diskIndex,
            unsigned long backend,
            std::uint64_t partitionOffset,
            std::uint64_t partitionLength,
            std::uint32_t logicalSectorSize,
            ForensicFileSystemKind fileSystem,
            const QString& path,
            std::uint32_t maximumEntries = 4096U);

        // The returned value owns one mounted file state. Call serially for this read only;
        // a new source/partition/filesystem/path must create a new session.
        using FileReadSession = std::function<RawFileReadResult(std::uint64_t, std::uint32_t)>;
        static FileReadSession createReadSession(
            int diskIndex, unsigned long backend, std::uint64_t partitionOffset,
            std::uint64_t partitionLength, std::uint32_t logicalSectorSize,
            ForensicFileSystemKind fileSystem, const QString& path);

        static RawFileReadResult readFile(
            int diskIndex,
            unsigned long backend,
            std::uint64_t partitionOffset,
            std::uint64_t partitionLength,
            std::uint32_t logicalSectorSize,
            ForensicFileSystemKind fileSystem,
            const QString& path,
            std::uint64_t offset,
            std::uint32_t length);

        static RawFileExportResult exportFile(
            int diskIndex,
            unsigned long backend,
            std::uint64_t partitionOffset,
            std::uint64_t partitionLength,
            std::uint32_t logicalSectorSize,
            ForensicFileSystemKind fileSystem,
            const QString& sourcePath,
            const QString& destinationPath);

        static QString objectTypeText(RawFileObjectType type);
    };
}
