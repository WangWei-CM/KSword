#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <vector>

namespace ksword::ark
{
    ThreadEnumResult DriverClient::enumerateThreads(
        const unsigned long flags,
        const std::uint32_t processId) const
    {
        ThreadEnumResult enumResult{};
        KSWORD_ARK_ENUM_THREAD_REQUEST request{};
        request.flags = flags;
        request.processId = processId;

        std::vector<std::uint8_t> responseBuffer(1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_THREAD,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_THREAD) failed, error=" +
                std::to_string(enumResult.io.win32Error);
            return enumResult;
        }

        constexpr std::size_t headerSize =
            sizeof(KSWORD_ARK_ENUM_THREAD_RESPONSE) - sizeof(KSWORD_ARK_THREAD_ENTRY);
        if (enumResult.io.bytesReturned < headerSize)
        {
            enumResult.io.ok = false;
            enumResult.io.message =
                "enum-thread response too small, bytesReturned=" +
                std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_ENUM_THREAD_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_THREAD_ENTRY))
        {
            enumResult.io.ok = false;
            enumResult.io.message =
                "enum-thread entry size invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(responseHeader->version);
        enumResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        enumResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);

        const std::size_t availableCount =
            (enumResult.io.bytesReturned - headerSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        enumResult.entries.reserve(parsedCount);

        for (std::size_t index = 0; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            const auto* entry =
                reinterpret_cast<const KSWORD_ARK_THREAD_ENTRY*>(responseBuffer.data() + entryOffset);
            ThreadEntry parsedEntry{};

            parsedEntry.threadId = static_cast<std::uint32_t>(entry->threadId);
            parsedEntry.processId = static_cast<std::uint32_t>(entry->processId);
            parsedEntry.flags = static_cast<std::uint32_t>(entry->flags);
            parsedEntry.fieldFlags = static_cast<std::uint32_t>(entry->fieldFlags);
            parsedEntry.r0Status = static_cast<std::uint32_t>(entry->r0Status);
            parsedEntry.stackFieldSource = static_cast<std::uint32_t>(entry->stackFieldSource);
            parsedEntry.ioFieldSource = static_cast<std::uint32_t>(entry->ioFieldSource);
            parsedEntry.initialStack = static_cast<std::uint64_t>(entry->initialStack);
            parsedEntry.stackLimit = static_cast<std::uint64_t>(entry->stackLimit);
            parsedEntry.stackBase = static_cast<std::uint64_t>(entry->stackBase);
            parsedEntry.kernelStack = static_cast<std::uint64_t>(entry->kernelStack);
            parsedEntry.readOperationCount = static_cast<std::uint64_t>(entry->readOperationCount);
            parsedEntry.writeOperationCount = static_cast<std::uint64_t>(entry->writeOperationCount);
            parsedEntry.otherOperationCount = static_cast<std::uint64_t>(entry->otherOperationCount);
            parsedEntry.readTransferCount = static_cast<std::uint64_t>(entry->readTransferCount);
            parsedEntry.writeTransferCount = static_cast<std::uint64_t>(entry->writeTransferCount);
            parsedEntry.otherTransferCount = static_cast<std::uint64_t>(entry->otherTransferCount);
            parsedEntry.ktInitialStackOffset = static_cast<std::uint32_t>(entry->ktInitialStackOffset);
            parsedEntry.ktStackLimitOffset = static_cast<std::uint32_t>(entry->ktStackLimitOffset);
            parsedEntry.ktStackBaseOffset = static_cast<std::uint32_t>(entry->ktStackBaseOffset);
            parsedEntry.ktKernelStackOffset = static_cast<std::uint32_t>(entry->ktKernelStackOffset);
            parsedEntry.ktReadOperationCountOffset = static_cast<std::uint32_t>(entry->ktReadOperationCountOffset);
            parsedEntry.ktWriteOperationCountOffset = static_cast<std::uint32_t>(entry->ktWriteOperationCountOffset);
            parsedEntry.ktOtherOperationCountOffset = static_cast<std::uint32_t>(entry->ktOtherOperationCountOffset);
            parsedEntry.ktReadTransferCountOffset = static_cast<std::uint32_t>(entry->ktReadTransferCountOffset);
            parsedEntry.ktWriteTransferCountOffset = static_cast<std::uint32_t>(entry->ktWriteTransferCountOffset);
            parsedEntry.ktOtherTransferCountOffset = static_cast<std::uint32_t>(entry->ktOtherTransferCountOffset);
            parsedEntry.dynDataCapabilityMask = static_cast<std::uint64_t>(entry->dynDataCapabilityMask);
            enumResult.entries.push_back(parsedEntry);
        }

        std::ostringstream stream;
        stream << "version=" << enumResult.version
            << ", total=" << enumResult.totalCount
            << ", returned=" << enumResult.returnedCount
            << ", parsed=" << enumResult.entries.size()
            << ", bytesReturned=" << enumResult.io.bytesReturned;
        enumResult.io.message = stream.str();
        return enumResult;
    }
}
