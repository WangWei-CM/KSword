#include "ArkDriverClient.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        // fixedCapabilityAnsiToString 作用：
        // - 把驱动返回的固定长度 char 数组转成 std::string；
        // - 入参 textBuffer/maxBytes：驱动响应字段和最大字节数；
        // - 返回：截至 NUL 或最大长度的安全字符串。
        std::string fixedCapabilityAnsiToString(const char* const textBuffer, const std::size_t maxBytes)
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

    // queryDriverCapabilities 作用：
    // - 请求 R0 Phase 1 统一能力、协议、安全策略和错误摘要；
    // - 无入参；
    // - 返回：解析后的状态字段和能力矩阵。
    DriverCapabilitiesQueryResult DriverClient::queryDriverCapabilities() const
    {
        DriverCapabilitiesQueryResult result{};
        std::vector<std::uint8_t> responseBuffer(64U * 1024U, 0U);

        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES,
            nullptr,
            0UL,
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES) failed, error=" + std::to_string(result.io.win32Error);
            return result;
        }

        constexpr std::size_t headerSize =
            sizeof(KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE) -
            sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY);
        if (result.io.bytesReturned < headerSize)
        {
            result.io.ok = false;
            result.io.message = "driver capability response too small, bytesReturned=" + std::to_string(result.io.bytesReturned);
            return result;
        }

        const auto* responseHeader = reinterpret_cast<const KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE*>(responseBuffer.data());
        if (responseHeader->version != KSWORD_ARK_DRIVER_CAPABILITY_PROTOCOL_VERSION ||
            responseHeader->entrySize < sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY))
        {
            result.io.ok = false;
            result.io.message = "driver capability response header invalid.";
            return result;
        }

        result.version = static_cast<std::uint32_t>(responseHeader->version);
        result.driverProtocolVersion = static_cast<std::uint32_t>(responseHeader->driverProtocolVersion);
        result.statusFlags = static_cast<std::uint32_t>(responseHeader->statusFlags);
        result.securityPolicyFlags = static_cast<std::uint32_t>(responseHeader->securityPolicyFlags);
        result.dynDataStatusFlags = static_cast<std::uint32_t>(responseHeader->dynDataStatusFlags);
        result.lastErrorStatus = static_cast<long>(responseHeader->lastErrorStatus);
        result.totalFeatureCount = static_cast<std::uint32_t>(responseHeader->totalFeatureCount);
        result.returnedFeatureCount = static_cast<std::uint32_t>(responseHeader->returnedFeatureCount);
        result.dynDataCapabilityMask = static_cast<std::uint64_t>(responseHeader->dynDataCapabilityMask);
        result.lastErrorSource = fixedCapabilityAnsiToString(responseHeader->lastErrorSource, sizeof(responseHeader->lastErrorSource));
        result.lastErrorSummary = fixedCapabilityAnsiToString(responseHeader->lastErrorSummary, sizeof(responseHeader->lastErrorSummary));

        const std::size_t availableCount =
            (result.io.bytesReturned - headerSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedFeatureCount),
            availableCount);
        result.entries.reserve(parsedCount);

        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset = headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY) > result.io.bytesReturned)
            {
                break;
            }

            const auto* sourceEntry = reinterpret_cast<const KSWORD_ARK_FEATURE_CAPABILITY_ENTRY*>(responseBuffer.data() + entryOffset);
            DriverFeatureCapabilityEntry row{};
            row.featureId = static_cast<std::uint32_t>(sourceEntry->featureId);
            row.state = static_cast<std::uint32_t>(sourceEntry->state);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.requiredPolicyFlags = static_cast<std::uint32_t>(sourceEntry->requiredPolicyFlags);
            row.deniedPolicyFlags = static_cast<std::uint32_t>(sourceEntry->deniedPolicyFlags);
            row.requiredDynDataMask = static_cast<std::uint64_t>(sourceEntry->requiredDynDataMask);
            row.presentDynDataMask = static_cast<std::uint64_t>(sourceEntry->presentDynDataMask);
            row.featureName = fixedCapabilityAnsiToString(sourceEntry->featureName, sizeof(sourceEntry->featureName));
            row.stateName = fixedCapabilityAnsiToString(sourceEntry->stateName, sizeof(sourceEntry->stateName));
            row.dependencyText = fixedCapabilityAnsiToString(sourceEntry->dependencyText, sizeof(sourceEntry->dependencyText));
            row.reasonText = fixedCapabilityAnsiToString(sourceEntry->reasonText, sizeof(sourceEntry->reasonText));
            result.entries.push_back(std::move(row));
        }

        result.io.message = "Driver capabilities parsed=" + std::to_string(result.entries.size()) +
            ", total=" + std::to_string(result.totalFeatureCount);
        return result;
    }
}
