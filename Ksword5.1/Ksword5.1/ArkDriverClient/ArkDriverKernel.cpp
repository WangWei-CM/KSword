#include "ArkDriverClient.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace ksword::ark
{
    namespace
    {
        constexpr std::size_t kSsdtResponseHeaderSize =
            sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY);

        constexpr std::size_t kInlineHookResponseHeaderSize =
            sizeof(KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY);

        constexpr std::size_t kIatEatHookResponseHeaderSize =
            sizeof(KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY);

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

        void copyVectorToFixedBytes(
            unsigned char* destination,
            const std::size_t destinationBytes,
            const std::vector<std::uint8_t>& sourceBytes)
        {
            // 作用：把 R3 vector 安全复制到共享协议定长字节数组。
            // 返回：无；超长部分被截断，剩余位置填 0。
            if (destination == nullptr || destinationBytes == 0U)
            {
                return;
            }

            std::fill(destination, destination + destinationBytes, 0U);
            const std::size_t copyBytes = std::min<std::size_t>(destinationBytes, sourceBytes.size());
            if (copyBytes != 0U)
            {
                std::copy(sourceBytes.begin(), sourceBytes.begin() + static_cast<std::ptrdiff_t>(copyBytes), destination);
            }
        }

        std::vector<std::uint8_t> fixedBytesToVector(const unsigned char* bytes, const std::size_t maxBytes)
        {
            // 作用：把共享协议定长字节数组转成 R3 vector，便于 UI 展示和重放比较。
            // 返回：长度固定为 maxBytes 的字节数组；空指针返回空 vector。
            if (bytes == nullptr || maxBytes == 0U)
            {
                return {};
            }

            return std::vector<std::uint8_t>(bytes, bytes + maxBytes);
        }

        SsdtEnumResult parseSsdtResponse(
            IoResult ioResult,
            const std::vector<std::uint8_t>& responseBuffer,
            const char* operationName)
        {
            // 作用：解析 SSDT/SSSDT 共用响应包。
            // 返回：填充后的 SsdtEnumResult，失败时 io.ok 被置 false。
            SsdtEnumResult enumResult{};
            enumResult.io = std::move(ioResult);
            if (!enumResult.io.ok)
            {
                enumResult.io.message = std::string("DeviceIoControl(") + operationName + ") failed, error=" + std::to_string(enumResult.io.win32Error);
                return enumResult;
            }

            if (enumResult.io.bytesReturned < kSsdtResponseHeaderSize)
            {
                enumResult.io.ok = false;
                enumResult.io.message = std::string(operationName) + " response too small, bytesReturned=" + std::to_string(enumResult.io.bytesReturned);
                return enumResult;
            }

            const auto* responseHeader = reinterpret_cast<const KSWORD_ARK_ENUM_SSDT_RESPONSE*>(responseBuffer.data());
            if (responseHeader->entrySize < sizeof(KSWORD_ARK_SSDT_ENTRY))
            {
                enumResult.io.ok = false;
                enumResult.io.message = std::string(operationName) + " entrySize invalid, entrySize=" + std::to_string(responseHeader->entrySize);
                return enumResult;
            }

            enumResult.version = responseHeader->version;
            enumResult.totalCount = responseHeader->totalCount;
            enumResult.returnedCount = responseHeader->returnedCount;
            enumResult.serviceTableBase = responseHeader->serviceTableBase;
            enumResult.serviceCountFromTable = responseHeader->serviceCountFromTable;

            const std::size_t availableCount =
                (enumResult.io.bytesReturned - kSsdtResponseHeaderSize) /
                static_cast<std::size_t>(responseHeader->entrySize);
            const std::size_t parsedCount = std::min<std::size_t>(
                static_cast<std::size_t>(responseHeader->returnedCount),
                availableCount);
            enumResult.entries.reserve(parsedCount);
            for (std::size_t index = 0U; index < parsedCount; ++index)
            {
                const std::size_t entryOffset =
                    kSsdtResponseHeaderSize +
                    (index * static_cast<std::size_t>(responseHeader->entrySize));
                if (entryOffset + sizeof(KSWORD_ARK_SSDT_ENTRY) > responseBuffer.size())
                {
                    break;
                }

                const auto* sourceEntry =
                    reinterpret_cast<const KSWORD_ARK_SSDT_ENTRY*>(responseBuffer.data() + entryOffset);
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

    SsdtEnumResult DriverClient::enumerateSsdt(const unsigned long flags) const
    {
        KSWORD_ARK_ENUM_SSDT_REQUEST request{};
        request.flags = flags;

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        IoResult ioResult = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_SSDT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        return parseSsdtResponse(std::move(ioResult), responseBuffer, "IOCTL_KSWORD_ARK_ENUM_SSDT");
    }

    SsdtEnumResult DriverClient::enumerateShadowSsdt(const unsigned long flags) const
    {
        KSWORD_ARK_ENUM_SSDT_REQUEST request{};
        request.flags = flags;

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        IoResult ioResult = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        return parseSsdtResponse(std::move(ioResult), responseBuffer, "IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT");
    }

    KernelInlineHookScanResult DriverClient::scanInlineHooks(
        const unsigned long flags,
        const unsigned long maxEntries,
        const std::wstring& moduleName) const
    {
        KernelInlineHookScanResult scanResult{};
        KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST request{};
        request.flags = flags;
        request.maxEntries = maxEntries;
        if (!moduleName.empty())
        {
            request.flags |= KSWORD_ARK_KERNEL_SCAN_FLAG_MODULE_FILTER;
            copyWideToFixed(request.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS, moduleName);
        }

        std::vector<std::uint8_t> responseBuffer(4U * 1024U * 1024U, 0U);
        scanResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!scanResult.io.ok)
        {
            scanResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS) failed, error=" +
                std::to_string(scanResult.io.win32Error);
            return scanResult;
        }
        if (scanResult.io.bytesReturned < kInlineHookResponseHeaderSize)
        {
            scanResult.io.ok = false;
            scanResult.io.message =
                "inline hook response too small, bytesReturned=" +
                std::to_string(scanResult.io.bytesReturned);
            return scanResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY))
        {
            scanResult.io.ok = false;
            scanResult.io.message =
                "inline hook entrySize invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return scanResult;
        }

        scanResult.version = static_cast<std::uint32_t>(responseHeader->version);
        scanResult.status = static_cast<std::uint32_t>(responseHeader->status);
        scanResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        scanResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        scanResult.moduleCount = static_cast<std::uint32_t>(responseHeader->moduleCount);
        scanResult.lastStatus = static_cast<long>(responseHeader->lastStatus);

        const std::size_t availableCount =
            (scanResult.io.bytesReturned - kInlineHookResponseHeaderSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        scanResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                kInlineHookResponseHeaderSize +
                (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry =
                reinterpret_cast<const KSWORD_ARK_INLINE_HOOK_ENTRY*>(responseBuffer.data() + entryOffset);
            KernelInlineHookEntry row{};
            row.status = static_cast<std::uint32_t>(sourceEntry->status);
            row.hookType = static_cast<std::uint32_t>(sourceEntry->hookType);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.originalByteCount = static_cast<std::uint32_t>(sourceEntry->originalByteCount);
            row.currentByteCount = static_cast<std::uint32_t>(sourceEntry->currentByteCount);
            row.functionAddress = static_cast<std::uint64_t>(sourceEntry->functionAddress);
            row.targetAddress = static_cast<std::uint64_t>(sourceEntry->targetAddress);
            row.moduleBase = static_cast<std::uint64_t>(sourceEntry->moduleBase);
            row.targetModuleBase = static_cast<std::uint64_t>(sourceEntry->targetModuleBase);
            row.functionName = fixedKernelAnsiToString(sourceEntry->functionName, sizeof(sourceEntry->functionName));
            row.moduleName = fixedKernelWideToString(sourceEntry->moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
            row.targetModuleName = fixedKernelWideToString(sourceEntry->targetModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
            row.currentBytes = fixedBytesToVector(sourceEntry->currentBytes, KSWORD_ARK_KERNEL_HOOK_BYTES);
            row.expectedBytes = fixedBytesToVector(sourceEntry->expectedBytes, KSWORD_ARK_KERNEL_HOOK_BYTES);
            scanResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << scanResult.version
            << ", total=" << scanResult.totalCount
            << ", returned=" << scanResult.returnedCount
            << ", parsed=" << scanResult.entries.size()
            << ", modules=" << scanResult.moduleCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(scanResult.lastStatus);
        scanResult.io.message = stream.str();
        return scanResult;
    }

    KernelInlinePatchResult DriverClient::patchInlineHook(
        const std::uint64_t functionAddress,
        const unsigned long mode,
        const unsigned long patchBytes,
        const std::vector<std::uint8_t>& expectedCurrentBytes,
        const std::vector<std::uint8_t>& restoreBytes,
        const unsigned long flags) const
    {
        KernelInlinePatchResult patchResult{};
        KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST request{};
        KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE response{};
        request.flags = flags;
        request.mode = mode;
        request.patchBytes = patchBytes;
        request.functionAddress = functionAddress;
        copyVectorToFixedBytes(
            request.expectedCurrentBytes,
            KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES,
            expectedCurrentBytes);
        copyVectorToFixedBytes(
            request.restoreBytes,
            KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES,
            restoreBytes);

        patchResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!patchResult.io.ok)
        {
            patchResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK) failed, error=" +
                std::to_string(patchResult.io.win32Error);
            return patchResult;
        }
        if (patchResult.io.bytesReturned < sizeof(response))
        {
            patchResult.io.ok = false;
            patchResult.io.message =
                "inline patch response too small, bytesReturned=" +
                std::to_string(patchResult.io.bytesReturned);
            return patchResult;
        }

        patchResult.version = static_cast<std::uint32_t>(response.version);
        patchResult.status = static_cast<std::uint32_t>(response.status);
        patchResult.bytesPatched = static_cast<std::uint32_t>(response.bytesPatched);
        patchResult.fieldFlags = static_cast<std::uint32_t>(response.fieldFlags);
        patchResult.lastStatus = static_cast<long>(response.lastStatus);
        patchResult.functionAddress = static_cast<std::uint64_t>(response.functionAddress);
        patchResult.beforeBytes = fixedBytesToVector(response.beforeBytes, KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES);
        patchResult.afterBytes = fixedBytesToVector(response.afterBytes, KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES);

        std::ostringstream stream;
        stream << "version=" << patchResult.version
            << ", status=" << patchResult.status
            << ", bytesPatched=" << patchResult.bytesPatched
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(patchResult.lastStatus);
        patchResult.io.message = stream.str();
        return patchResult;
    }

    KernelIatEatHookScanResult DriverClient::enumerateIatEatHooks(
        const unsigned long flags,
        const unsigned long maxEntries,
        const std::wstring& moduleName) const
    {
        KernelIatEatHookScanResult scanResult{};
        KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST request{};
        request.flags = flags;
        request.maxEntries = maxEntries;
        if (!moduleName.empty())
        {
            request.flags |= KSWORD_ARK_KERNEL_SCAN_FLAG_MODULE_FILTER;
            copyWideToFixed(request.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS, moduleName);
        }

        std::vector<std::uint8_t> responseBuffer(4U * 1024U * 1024U, 0U);
        scanResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!scanResult.io.ok)
        {
            scanResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS) failed, error=" +
                std::to_string(scanResult.io.win32Error);
            return scanResult;
        }
        if (scanResult.io.bytesReturned < kIatEatHookResponseHeaderSize)
        {
            scanResult.io.ok = false;
            scanResult.io.message =
                "IAT/EAT response too small, bytesReturned=" +
                std::to_string(scanResult.io.bytesReturned);
            return scanResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY))
        {
            scanResult.io.ok = false;
            scanResult.io.message =
                "IAT/EAT entrySize invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return scanResult;
        }

        scanResult.version = static_cast<std::uint32_t>(responseHeader->version);
        scanResult.status = static_cast<std::uint32_t>(responseHeader->status);
        scanResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        scanResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        scanResult.moduleCount = static_cast<std::uint32_t>(responseHeader->moduleCount);
        scanResult.lastStatus = static_cast<long>(responseHeader->lastStatus);

        const std::size_t availableCount =
            (scanResult.io.bytesReturned - kIatEatHookResponseHeaderSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        scanResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                kIatEatHookResponseHeaderSize +
                (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry =
                reinterpret_cast<const KSWORD_ARK_IAT_EAT_HOOK_ENTRY*>(responseBuffer.data() + entryOffset);
            KernelIatEatHookEntry row{};
            row.hookClass = static_cast<std::uint32_t>(sourceEntry->hookClass);
            row.status = static_cast<std::uint32_t>(sourceEntry->status);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.ordinal = static_cast<std::uint32_t>(sourceEntry->ordinal);
            row.moduleBase = static_cast<std::uint64_t>(sourceEntry->moduleBase);
            row.thunkAddress = static_cast<std::uint64_t>(sourceEntry->thunkAddress);
            row.currentTarget = static_cast<std::uint64_t>(sourceEntry->currentTarget);
            row.expectedTarget = static_cast<std::uint64_t>(sourceEntry->expectedTarget);
            row.targetModuleBase = static_cast<std::uint64_t>(sourceEntry->targetModuleBase);
            row.functionName = fixedKernelAnsiToString(sourceEntry->functionName, sizeof(sourceEntry->functionName));
            row.moduleName = fixedKernelWideToString(sourceEntry->moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
            row.importModuleName = fixedKernelWideToString(sourceEntry->importModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
            row.targetModuleName = fixedKernelWideToString(sourceEntry->targetModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
            scanResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << scanResult.version
            << ", total=" << scanResult.totalCount
            << ", returned=" << scanResult.returnedCount
            << ", parsed=" << scanResult.entries.size()
            << ", modules=" << scanResult.moduleCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(scanResult.lastStatus);
        scanResult.io.message = stream.str();
        return scanResult;
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

    DriverForceUnloadResult DriverClient::forceUnloadDriver(
        const std::wstring& driverName,
        const unsigned long flags,
        const unsigned long timeoutMilliseconds) const
    {
        // 作用：按 DriverObject 名称请求 R0 调用 DriverUnload。
        // 返回：固定响应；R0 地址仅作为诊断文本，不作为二次操作凭据。
        DriverForceUnloadResult unloadResult{};
        KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST request{};
        KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE response{};
        request.version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
        request.flags = flags;
        request.timeoutMilliseconds = timeoutMilliseconds;
        copyWideToFixed(
            request.driverName,
            KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS,
            driverName);

        unloadResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!unloadResult.io.ok)
        {
            unloadResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER) failed, error=" +
                std::to_string(unloadResult.io.win32Error);
            return unloadResult;
        }
        if (unloadResult.io.bytesReturned < sizeof(response))
        {
            unloadResult.io.ok = false;
            unloadResult.io.message =
                "driver-unload response too small, bytesReturned=" +
                std::to_string(unloadResult.io.bytesReturned);
            return unloadResult;
        }

        unloadResult.version = static_cast<std::uint32_t>(response.version);
        unloadResult.status = static_cast<std::uint32_t>(response.status);
        unloadResult.flags = static_cast<std::uint32_t>(response.flags);
        unloadResult.lastStatus = static_cast<long>(response.lastStatus);
        unloadResult.waitStatus = static_cast<long>(response.waitStatus);
        unloadResult.driverObjectAddress = static_cast<std::uint64_t>(response.driverObjectAddress);
        unloadResult.driverUnloadAddress = static_cast<std::uint64_t>(response.driverUnloadAddress);
        unloadResult.callbackCandidates = static_cast<std::uint32_t>(response.callbackCandidates);
        unloadResult.callbacksRemoved = static_cast<std::uint32_t>(response.callbacksRemoved);
        unloadResult.callbackFailures = static_cast<std::uint32_t>(response.callbackFailures);
        unloadResult.callbackLastStatus = static_cast<long>(response.callbackLastStatus);
        unloadResult.driverName = fixedKernelWideToString(
            response.driverName,
            KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
        unloadResult.io.ntStatus = unloadResult.lastStatus;

        std::ostringstream stream;
        stream << "status=" << unloadResult.status
            << ", object=0x" << std::hex << unloadResult.driverObjectAddress
            << ", unload=0x" << unloadResult.driverUnloadAddress
            << ", lastStatus=0x" << static_cast<unsigned long>(unloadResult.lastStatus)
            << ", waitStatus=0x" << static_cast<unsigned long>(unloadResult.waitStatus)
            << ", callbackCandidates=" << std::dec << unloadResult.callbackCandidates
            << ", callbacksRemoved=" << unloadResult.callbacksRemoved
            << ", callbackFailures=" << unloadResult.callbackFailures
            << ", callbackLast=0x" << std::hex << static_cast<unsigned long>(unloadResult.callbackLastStatus);
        unloadResult.io.message = stream.str();
        return unloadResult;
    }

    DriverForceUnloadResult DriverClient::forceUnloadDriverByModuleBase(
        const std::uint64_t moduleBase,
        const std::wstring& fallbackDriverName,
        const unsigned long flags,
        const unsigned long timeoutMilliseconds) const
    {
        // 作用：按内核模块基址请求 R0 反查 DriverObject 并执行强制清理。
        // 返回：固定响应；如果模块基址无法匹配 DriverObject，R0 会继续按 fallbackDriverName 兜底。
        DriverForceUnloadResult unloadResult{};
        KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST request{};
        KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE response{};
        request.version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
        request.flags = flags | KSWORD_ARK_DRIVER_UNLOAD_FLAG_TARGET_MODULE_BASE_PRESENT;
        request.timeoutMilliseconds = timeoutMilliseconds;
        request.targetModuleBase = static_cast<unsigned long long>(moduleBase);
        copyWideToFixed(
            request.driverName,
            KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS,
            fallbackDriverName);

        unloadResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!unloadResult.io.ok)
        {
            unloadResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER/module-base) failed, error=" +
                std::to_string(unloadResult.io.win32Error);
            return unloadResult;
        }
        if (unloadResult.io.bytesReturned < sizeof(response))
        {
            unloadResult.io.ok = false;
            unloadResult.io.message =
                "driver-unload/module-base response too small, bytesReturned=" +
                std::to_string(unloadResult.io.bytesReturned);
            return unloadResult;
        }

        unloadResult.version = static_cast<std::uint32_t>(response.version);
        unloadResult.status = static_cast<std::uint32_t>(response.status);
        unloadResult.flags = static_cast<std::uint32_t>(response.flags);
        unloadResult.lastStatus = static_cast<long>(response.lastStatus);
        unloadResult.waitStatus = static_cast<long>(response.waitStatus);
        unloadResult.driverObjectAddress = static_cast<std::uint64_t>(response.driverObjectAddress);
        unloadResult.driverUnloadAddress = static_cast<std::uint64_t>(response.driverUnloadAddress);
        unloadResult.callbackCandidates = static_cast<std::uint32_t>(response.callbackCandidates);
        unloadResult.callbacksRemoved = static_cast<std::uint32_t>(response.callbacksRemoved);
        unloadResult.callbackFailures = static_cast<std::uint32_t>(response.callbackFailures);
        unloadResult.callbackLastStatus = static_cast<long>(response.callbackLastStatus);
        unloadResult.driverName = fixedKernelWideToString(
            response.driverName,
            KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
        unloadResult.io.ntStatus = unloadResult.lastStatus;

        std::ostringstream stream;
        stream << "moduleBase=0x" << std::hex << moduleBase
            << ", status=" << unloadResult.status
            << ", object=0x" << unloadResult.driverObjectAddress
            << ", unload=0x" << unloadResult.driverUnloadAddress
            << ", lastStatus=0x" << static_cast<unsigned long>(unloadResult.lastStatus)
            << ", waitStatus=0x" << static_cast<unsigned long>(unloadResult.waitStatus)
            << ", callbackCandidates=" << std::dec << unloadResult.callbackCandidates
            << ", callbacksRemoved=" << unloadResult.callbacksRemoved
            << ", callbackFailures=" << unloadResult.callbackFailures
            << ", callbackLast=0x" << std::hex << static_cast<unsigned long>(unloadResult.callbackLastStatus);
        unloadResult.io.message = stream.str();
        return unloadResult;
    }
}
