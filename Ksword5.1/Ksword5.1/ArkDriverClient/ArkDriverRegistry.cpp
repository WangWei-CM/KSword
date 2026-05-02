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
}
