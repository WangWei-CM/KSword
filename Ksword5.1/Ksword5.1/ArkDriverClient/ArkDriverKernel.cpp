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

        std::wstring fixedKernelWideToString(const wchar_t* textBuffer, const std::size_t maxChars)
        {
            if (textBuffer == nullptr || maxChars == 0U)
            {
                return {};
            }

            std::size_t length = 0U;
            while (length < maxChars && textBuffer[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(textBuffer, textBuffer + length);
        }

        void copyWideToFixed(wchar_t* destination, const std::size_t destinationChars, const std::wstring& source)
        {
            if (destination == nullptr || destinationChars == 0U)
            {
                return;
            }

            const std::size_t copyChars = std::min<std::size_t>(source.size(), destinationChars - 1U);
            std::fill(destination, destination + destinationChars, L'\0');
            if (copyChars != 0U)
            {
                std::copy(source.data(), source.data() + copyChars, destination);
            }
            destination[copyChars] = L'\0';
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

    DriverObjectQueryResult DriverClient::queryDriverObject(
        const std::wstring& driverName,
        const unsigned long flags,
        const unsigned long maxDevices,
        const unsigned long maxAttachedDevices) const
    {
        DriverObjectQueryResult queryResult{};
        KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST request{};
        request.flags = flags;
        request.maxDevices = maxDevices;
        request.maxAttachedDevices = maxAttachedDevices;
        copyWideToFixed(
            request.driverName,
            KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS,
            driverName);

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        queryResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!queryResult.io.ok)
        {
            queryResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT) failed, error=" +
                std::to_string(queryResult.io.win32Error);
            return queryResult;
        }

        constexpr std::size_t headerSize =
            sizeof(KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE) -
            sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY);
        if (queryResult.io.bytesReturned < headerSize)
        {
            queryResult.io.ok = false;
            queryResult.io.message =
                "driver-object response too small, bytesReturned=" +
                std::to_string(queryResult.io.bytesReturned);
            return queryResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE*>(responseBuffer.data());
        if (responseHeader->deviceEntrySize < sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY))
        {
            queryResult.io.ok = false;
            queryResult.io.message =
                "driver-object device entry size invalid, entrySize=" +
                std::to_string(responseHeader->deviceEntrySize);
            return queryResult;
        }

        queryResult.version = static_cast<std::uint32_t>(responseHeader->version);
        queryResult.queryStatus = static_cast<std::uint32_t>(responseHeader->queryStatus);
        queryResult.fieldFlags = static_cast<std::uint32_t>(responseHeader->fieldFlags);
        queryResult.majorFunctionCount = static_cast<std::uint32_t>(responseHeader->majorFunctionCount);
        queryResult.totalDeviceCount = static_cast<std::uint32_t>(responseHeader->totalDeviceCount);
        queryResult.returnedDeviceCount = static_cast<std::uint32_t>(responseHeader->returnedDeviceCount);
        queryResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        queryResult.driverFlags = static_cast<std::uint32_t>(responseHeader->driverFlags);
        queryResult.driverSize = static_cast<std::uint32_t>(responseHeader->driverSize);
        queryResult.driverObjectAddress = static_cast<std::uint64_t>(responseHeader->driverObjectAddress);
        queryResult.driverStart = static_cast<std::uint64_t>(responseHeader->driverStart);
        queryResult.driverSection = static_cast<std::uint64_t>(responseHeader->driverSection);
        queryResult.driverUnload = static_cast<std::uint64_t>(responseHeader->driverUnload);
        queryResult.driverName = fixedKernelWideToString(responseHeader->driverName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
        queryResult.serviceKeyName = fixedKernelWideToString(responseHeader->serviceKeyName, KSWORD_ARK_DRIVER_SERVICE_KEY_CHARS);
        queryResult.imagePath = fixedKernelWideToString(responseHeader->imagePath, KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS);

        const std::size_t majorCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->majorFunctionCount),
            static_cast<std::size_t>(KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT));
        queryResult.majorFunctions.reserve(majorCount);
        for (std::size_t index = 0; index < majorCount; ++index)
        {
            const KSWORD_ARK_DRIVER_MAJOR_FUNCTION_ENTRY& sourceEntry =
                responseHeader->majorFunctions[index];
            DriverMajorFunctionEntry row{};
            row.majorFunction = static_cast<std::uint32_t>(sourceEntry.majorFunction);
            row.flags = static_cast<std::uint32_t>(sourceEntry.flags);
            row.dispatchAddress = static_cast<std::uint64_t>(sourceEntry.dispatchAddress);
            row.moduleBase = static_cast<std::uint64_t>(sourceEntry.moduleBase);
            row.moduleName = fixedKernelWideToString(sourceEntry.moduleName, KSWORD_ARK_DRIVER_MODULE_NAME_CHARS);
            queryResult.majorFunctions.push_back(std::move(row));
        }

        const std::size_t availableDeviceCount =
            (queryResult.io.bytesReturned - headerSize) /
            static_cast<std::size_t>(responseHeader->deviceEntrySize);
        const std::size_t parsedDeviceCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedDeviceCount),
            availableDeviceCount);
        queryResult.devices.reserve(parsedDeviceCount);
        for (std::size_t index = 0; index < parsedDeviceCount; ++index)
        {
            const std::size_t entryOffset =
                headerSize + (index * static_cast<std::size_t>(responseHeader->deviceEntrySize));
            const auto* sourceEntry =
                reinterpret_cast<const KSWORD_ARK_DRIVER_DEVICE_ENTRY*>(responseBuffer.data() + entryOffset);
            DriverDeviceEntry row{};
            row.relationDepth = static_cast<std::uint32_t>(sourceEntry->relationDepth);
            row.deviceType = static_cast<std::uint32_t>(sourceEntry->deviceType);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.characteristics = static_cast<std::uint32_t>(sourceEntry->characteristics);
            row.stackSize = static_cast<std::uint32_t>(sourceEntry->stackSize);
            row.alignmentRequirement = static_cast<std::uint32_t>(sourceEntry->alignmentRequirement);
            row.nameStatus = static_cast<long>(sourceEntry->nameStatus);
            row.rootDeviceObjectAddress = static_cast<std::uint64_t>(sourceEntry->rootDeviceObjectAddress);
            row.deviceObjectAddress = static_cast<std::uint64_t>(sourceEntry->deviceObjectAddress);
            row.nextDeviceObjectAddress = static_cast<std::uint64_t>(sourceEntry->nextDeviceObjectAddress);
            row.attachedDeviceObjectAddress = static_cast<std::uint64_t>(sourceEntry->attachedDeviceObjectAddress);
            row.driverObjectAddress = static_cast<std::uint64_t>(sourceEntry->driverObjectAddress);
            row.deviceName = fixedKernelWideToString(sourceEntry->deviceName, KSWORD_ARK_DRIVER_DEVICE_NAME_CHARS);
            queryResult.devices.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << queryResult.version
            << ", status=" << queryResult.queryStatus
            << ", major=" << queryResult.majorFunctions.size()
            << ", devices=" << queryResult.devices.size()
            << "/" << queryResult.totalDeviceCount
            << ", bytesReturned=" << queryResult.io.bytesReturned;
        queryResult.io.message = stream.str();
        return queryResult;
    }
}
