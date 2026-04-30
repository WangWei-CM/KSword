#include "ArkDriverClient.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace ksword::ark
{
    namespace
    {
        std::string fixedKernelAnsiToString(const char* textBuffer, const std::size_t maxBytes)
        {
            if (textBuffer == nullptr || maxBytes == 0U)
            {
                return {};
            }

            std::size_t length = 0U;
            while (length < maxBytes && textBuffer[length] != '\0')
            {
                ++length;
            }
            return std::string(textBuffer, textBuffer + length);
        }
    }

    SsdtEnumResult DriverClient::enumerateSsdt(const unsigned long flags) const
    {
        SsdtEnumResult enumResult{};
        KSWORD_ARK_ENUM_SSDT_REQUEST request{};
        request.flags = flags;

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_SSDT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_SSDT) failed, error=" + std::to_string(enumResult.io.win32Error);
            return enumResult;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY);
        if (enumResult.io.bytesReturned < headerSize)
        {
            enumResult.io.ok = false;
            enumResult.io.message = "SSDT response too small, bytesReturned=" + std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader = reinterpret_cast<const KSWORD_ARK_ENUM_SSDT_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_SSDT_ENTRY))
        {
            enumResult.io.ok = false;
            enumResult.io.message = "SSDT entrySize invalid, entrySize=" + std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = responseHeader->version;
        enumResult.totalCount = responseHeader->totalCount;
        enumResult.returnedCount = responseHeader->returnedCount;
        enumResult.serviceTableBase = responseHeader->serviceTableBase;
        enumResult.serviceCountFromTable = responseHeader->serviceCountFromTable;

        const std::size_t availableCount = (enumResult.io.bytesReturned - headerSize) / static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(static_cast<std::size_t>(responseHeader->returnedCount), availableCount);
        enumResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset = headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_SSDT_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry = reinterpret_cast<const KSWORD_ARK_SSDT_ENTRY*>(responseBuffer.data() + entryOffset);
            SsdtEntry row{};
            row.serviceIndex = static_cast<std::uint32_t>(sourceEntry->serviceIndex);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.zwRoutineAddress = static_cast<std::uint64_t>(sourceEntry->zwRoutineAddress);
            row.serviceRoutineAddress = static_cast<std::uint64_t>(sourceEntry->serviceRoutineAddress);
            row.serviceName = fixedKernelAnsiToString(sourceEntry->serviceName, sizeof(sourceEntry->serviceName));
            row.moduleName = fixedKernelAnsiToString(sourceEntry->moduleName, sizeof(sourceEntry->moduleName));
            enumResult.entries.push_back(std::move(row));
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
