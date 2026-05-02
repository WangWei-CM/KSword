#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        void copyRegistryWideToFixed(
            wchar_t* destination,
            const std::size_t destinationChars,
            const std::wstring& source)
        {
            // 作用：把 R3 路径/值名复制到共享协议定长 WCHAR 数组。
            // 返回：无；超长时截断并保留 NUL 结尾。
            if (destination == nullptr || destinationChars == 0U)
            {
                return;
            }

            std::fill(destination, destination + destinationChars, L'\0');
            const std::size_t copyChars = std::min<std::size_t>(source.size(), destinationChars - 1U);
            if (copyChars != 0U)
            {
                std::copy(source.data(), source.data() + copyChars, destination);
            }
            destination[copyChars] = L'\0';
        }

        std::wstring registryFixedWideToString(
            const wchar_t* source,
            const std::size_t sourceChars)
        {
            // 作用：把 R0 固定 WCHAR 数组转换为 std::wstring。
            // 返回：遇到 NUL 或达到上限后得到的字符串。
            if (source == nullptr || sourceChars == 0U)
            {
                return {};
            }

            std::size_t length = 0U;
            while (length < sourceChars && source[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(source, source + length);
        }

        RegistryOperationResult parseRegistryOperationResponse(
            IoResult ioResult,
            const KSWORD_ARK_REGISTRY_OPERATION_RESPONSE& response,
            const char* operationName)
        {
            // 作用：把通用 R0 注册表写操作响应转换为 R3 模型。
            // 返回：RegistryOperationResult，失败时保留 DeviceIoControl 详情。
            RegistryOperationResult result{};
            result.io = std::move(ioResult);
            if (!result.io.ok)
            {
                result.io.message =
                    std::string("DeviceIoControl(") + operationName + ") failed, error=" +
                    std::to_string(result.io.win32Error);
                return result;
            }
            if (result.io.bytesReturned < sizeof(response))
            {
                result.io.ok = false;
                result.io.message =
                    std::string("registry operation response too small, bytesReturned=") +
                    std::to_string(result.io.bytesReturned);
                return result;
            }

            result.version = static_cast<std::uint32_t>(response.version);
            result.status = static_cast<std::uint32_t>(response.status);
            result.lastStatus = static_cast<long>(response.lastStatus);
            result.io.ntStatus = result.lastStatus;

            std::ostringstream stream;
            stream << "status=" << result.status
                << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus);
            result.io.message = stream.str();
            return result;
        }
    }

    RegistryReadResult DriverClient::readRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& valueName,
        const unsigned long maxDataBytes) const
    {
        // 作用：读取 R0 注册表值，路径必须是 \REGISTRY\... 内核路径。
        // 返回：RegistryReadResult；结构化失败也会携带 R0 status/lastStatus。
        RegistryReadResult readResult{};
        KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST request{};
        KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        request.maxDataBytes = maxDataBytes;
        copyRegistryWideToFixed(
            request.keyPath,
            KSWORD_ARK_REGISTRY_PATH_CHARS,
            kernelKeyPath);
        if (!valueName.empty())
        {
            request.flags |= KSWORD_ARK_REGISTRY_READ_FLAG_VALUE_NAME_PRESENT;
            copyRegistryWideToFixed(
                request.valueName,
                KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS,
                valueName);
        }

        readResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!readResult.io.ok)
        {
            readResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE) failed, error=" +
                std::to_string(readResult.io.win32Error);
            return readResult;
        }
        if (readResult.io.bytesReturned < sizeof(response))
        {
            readResult.io.ok = false;
            readResult.io.message =
                "registry read response too small, bytesReturned=" +
                std::to_string(readResult.io.bytesReturned);
            return readResult;
        }

        readResult.version = static_cast<std::uint32_t>(response.version);
        readResult.status = static_cast<std::uint32_t>(response.status);
        readResult.valueType = static_cast<std::uint32_t>(response.valueType);
        readResult.dataBytes = static_cast<std::uint32_t>(response.dataBytes);
        readResult.requiredBytes = static_cast<std::uint32_t>(response.requiredBytes);
        readResult.lastStatus = static_cast<long>(response.lastStatus);
        readResult.io.ntStatus = readResult.lastStatus;

        const std::size_t copyBytes = std::min<std::size_t>(
            static_cast<std::size_t>(readResult.dataBytes),
            KSWORD_ARK_REGISTRY_DATA_MAX_BYTES);
        readResult.data.assign(response.data, response.data + copyBytes);

        std::ostringstream stream;
        stream << "status=" << readResult.status
            << ", type=" << readResult.valueType
            << ", data=" << readResult.dataBytes
            << "/" << readResult.requiredBytes
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(readResult.lastStatus);
        readResult.io.message = stream.str();
        return readResult;
    }

    RegistryEnumResult DriverClient::enumerateRegistryKey(
        const std::wstring& kernelKeyPath,
        const unsigned long flags) const
    {
        // 作用：通过 R0 枚举注册表键下的子键和值。
        // 返回：RegistryEnumResult；部分返回时 status 为 PARTIAL。
        RegistryEnumResult enumResult{};
        KSWORD_ARK_ENUM_REGISTRY_KEY_REQUEST request{};
        KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        request.flags = flags;
        request.maxSubKeys = KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS;
        request.maxValues = KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES;
        request.maxValueDataBytes = KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES;
        copyRegistryWideToFixed(
            request.keyPath,
            KSWORD_ARK_REGISTRY_PATH_CHARS,
            kernelKeyPath);

        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!enumResult.io.ok)
        {
            enumResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY) failed, error=" +
                std::to_string(enumResult.io.win32Error);
            return enumResult;
        }
        if (enumResult.io.bytesReturned < sizeof(response))
        {
            enumResult.io.ok = false;
            enumResult.io.message =
                "registry enum response too small, bytesReturned=" +
                std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(response.version);
        enumResult.status = static_cast<std::uint32_t>(response.status);
        enumResult.subKeyCount = static_cast<std::uint32_t>(response.subKeyCount);
        enumResult.returnedSubKeyCount = static_cast<std::uint32_t>(response.returnedSubKeyCount);
        enumResult.valueCount = static_cast<std::uint32_t>(response.valueCount);
        enumResult.returnedValueCount = static_cast<std::uint32_t>(response.returnedValueCount);
        enumResult.lastStatus = static_cast<long>(response.lastStatus);
        enumResult.io.ntStatus = enumResult.lastStatus;

        const std::size_t subKeyCount = std::min<std::size_t>(
            enumResult.returnedSubKeyCount,
            KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS);
        enumResult.subKeys.reserve(subKeyCount);
        for (std::size_t index = 0U; index < subKeyCount; ++index)
        {
            RegistrySubKeyEntry entry{};
            entry.name = registryFixedWideToString(
                response.subKeys[index].name,
                KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS);
            enumResult.subKeys.push_back(std::move(entry));
        }

        const std::size_t valueCount = std::min<std::size_t>(
            enumResult.returnedValueCount,
            KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES);
        enumResult.values.reserve(valueCount);
        for (std::size_t index = 0U; index < valueCount; ++index)
        {
            const auto& source = response.values[index];
            RegistryValueEntry entry{};
            entry.name = registryFixedWideToString(
                source.name,
                KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS);
            if ((source.flags & KSWORD_ARK_REGISTRY_ENUM_VALUE_FLAG_NAME_PRESENT) == 0UL)
            {
                entry.name.clear();
            }
            entry.valueType = static_cast<std::uint32_t>(source.valueType);
            entry.dataBytes = static_cast<std::uint32_t>(source.dataBytes);
            entry.requiredBytes = static_cast<std::uint32_t>(source.requiredBytes);
            const std::size_t dataBytes = std::min<std::size_t>(
                static_cast<std::size_t>(entry.dataBytes),
                KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES);
            entry.data.assign(source.data, source.data + dataBytes);
            enumResult.values.push_back(std::move(entry));
        }

        std::ostringstream stream;
        stream << "status=" << enumResult.status
            << ", subkeys=" << enumResult.returnedSubKeyCount << "/" << enumResult.subKeyCount
            << ", values=" << enumResult.returnedValueCount << "/" << enumResult.valueCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(enumResult.lastStatus);
        enumResult.io.message = stream.str();
        return enumResult;
    }

    RegistryOperationResult DriverClient::setRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& valueName,
        const std::uint32_t valueType,
        const std::vector<std::uint8_t>& data) const
    {
        // 作用：通过 R0 写入或创建注册表值。
        // 返回：RegistryOperationResult，status 表示 R0 聚合状态。
        KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        request.valueType = valueType;
        request.dataBytes = static_cast<unsigned long>(std::min<std::size_t>(data.size(), KSWORD_ARK_REGISTRY_DATA_MAX_BYTES));
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        if (!valueName.empty())
        {
            request.flags |= KSWORD_ARK_REGISTRY_SET_FLAG_VALUE_NAME_PRESENT;
            copyRegistryWideToFixed(request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, valueName);
        }
        if (request.dataBytes != 0UL)
        {
            std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(request.dataBytes), request.data);
        }

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE");
    }

    RegistryOperationResult DriverClient::deleteRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& valueName) const
    {
        KSWORD_ARK_REGISTRY_VALUE_NAME_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        if (!valueName.empty())
        {
            request.flags |= KSWORD_ARK_REGISTRY_DELETE_VALUE_FLAG_NAME_PRESENT;
            copyRegistryWideToFixed(request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, valueName);
        }

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE");
    }

    RegistryOperationResult DriverClient::createRegistryKey(const std::wstring& kernelKeyPath) const
    {
        KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY");
    }

    RegistryOperationResult DriverClient::deleteRegistryKey(const std::wstring& kernelKeyPath) const
    {
        KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY");
    }

    RegistryOperationResult DriverClient::renameRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& oldValueName,
        const std::wstring& newValueName) const
    {
        KSWORD_ARK_RENAME_REGISTRY_VALUE_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        copyRegistryWideToFixed(request.oldValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, oldValueName);
        copyRegistryWideToFixed(request.newValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, newValueName);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE");
    }

    RegistryOperationResult DriverClient::renameRegistryKey(
        const std::wstring& kernelKeyPath,
        const std::wstring& newKeyName) const
    {
        KSWORD_ARK_RENAME_REGISTRY_KEY_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        copyRegistryWideToFixed(request.newKeyName, KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS, newKeyName);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY");
    }
}
