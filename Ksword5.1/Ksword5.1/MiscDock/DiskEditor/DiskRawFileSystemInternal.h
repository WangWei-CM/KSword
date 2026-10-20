#pragma once

#include "DiskRawFileSystemBrowser.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <any>
#include <memory>

namespace ks::misc::rawfs
{
    constexpr std::uint32_t kMaximumSingleReadBytes = 16U * 1024U * 1024U;
    constexpr std::uint32_t kExportChunkBytes = 4U * 1024U * 1024U;
    constexpr std::uint32_t kMaximumExtentCount = 4096U;
    constexpr std::uint32_t kMaximumTreeDepth = 32U;

    class VolumeReader final
    {
    public:
        VolumeReader(
            int diskIndex,
            unsigned long backend,
            std::uint64_t partitionOffset,
            std::uint64_t partitionLength,
            std::uint32_t logicalSectorSize);

        // A read session is confined to one filesystem/path and one serial worker.
        // Mounted volumes borrow this stable reader; their typed state dies before the reader.
        void enableFileSession() { m_fileSessionEnabled = true; }
        template<class State> std::shared_ptr<State> fileSessionState() const
        {
            if (!m_fileSessionEnabled) return {};
            if (const auto* state = std::any_cast<std::shared_ptr<State>>(&m_fileSessionState)) return *state;
            return {};
        }
        template<class State> void setFileSessionState(std::shared_ptr<State> state) const
        {
            if (m_fileSessionEnabled) m_fileSessionState = std::move(state);
        }

        bool read(
            std::uint64_t relativeOffset,
            std::uint32_t length,
            QByteArray& bytesOut,
            QString& errorTextOut) const;

        bool rangeIsValid(
            std::uint64_t relativeOffset,
            std::uint64_t length) const;

        std::uint64_t partitionOffset() const;
        std::uint64_t partitionLength() const;

    private:
        int m_diskIndex = -1;
        unsigned long m_backend = 1UL;
        std::uint64_t m_partitionOffset = 0;
        std::uint64_t m_partitionLength = 0;
        std::uint32_t m_logicalSectorSize = 512U;
        bool m_fileSessionEnabled = false;
        mutable std::any m_fileSessionState;
    };

    QString normalizePath(const QString& path);
    QString childPath(const QString& parent, const QString& name);

    RawDirectoryResult listNtfs(
        const VolumeReader& reader,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readNtfs(
        const VolumeReader& reader,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listFat(
        const VolumeReader& reader,
        ForensicFileSystemKind expectedKind,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readFat(
        const VolumeReader& reader,
        ForensicFileSystemKind expectedKind,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listExFat(
        const VolumeReader& reader,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readExFat(
        const VolumeReader& reader,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listExt(
        const VolumeReader& reader,
        ForensicFileSystemKind expectedKind,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readExt(
        const VolumeReader& reader,
        ForensicFileSystemKind expectedKind,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listBtrfs(
        const VolumeReader& reader,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readBtrfs(
        const VolumeReader& reader,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listApfs(
        const VolumeReader& reader,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readApfs(
        const VolumeReader& reader,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listHfs(
        const VolumeReader& reader,
        ForensicFileSystemKind expectedKind,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readHfs(
        const VolumeReader& reader,
        ForensicFileSystemKind expectedKind,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);

    RawDirectoryResult listRefs(
        const VolumeReader& reader,
        const QString& path,
        std::uint32_t maximumEntries);
    RawFileReadResult readRefs(
        const VolumeReader& reader,
        const QString& path,
        std::uint64_t offset,
        std::uint32_t length);
}
