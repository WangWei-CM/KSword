#include "ArkDriverClient.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace ksword::ark
{
    namespace
    {
        // fixedDynAnsiToString 作用：
        // - 把驱动返回的固定长度 char 数组转成 std::string；
        // - 入参 textBuffer/maxBytes：驱动响应字段和最大字节数；
        // - 返回：截至 NUL 或最大长度的安全字符串。
        std::string fixedDynAnsiToString(const char* const textBuffer, const std::size_t maxBytes)
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

        // fixedDynWideToString 作用：
        // - 把驱动返回的固定长度 wchar_t 数组转成 std::wstring；
        // - 入参 textBuffer/maxChars：驱动响应字段和最大字符数；
        // - 返回：截至 NUL 或最大长度的安全宽字符串。
        std::wstring fixedDynWideToString(const wchar_t* const textBuffer, const std::size_t maxChars)
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

        // convertModuleIdentity 作用：
        // - 把共享协议中的模块身份包转换成 R3 友好结构；
        // - 入参 source：驱动返回的模块身份结构；
        // - 返回：ArkDynModuleIdentity 值对象。
        ArkDynModuleIdentity convertModuleIdentity(const KSW_DYN_MODULE_IDENTITY_PACKET& source)
        {
            ArkDynModuleIdentity result{};
            result.present = source.present != 0UL;
            result.classId = static_cast<std::uint32_t>(source.classId);
            result.machine = static_cast<std::uint32_t>(source.machine);
            result.timeDateStamp = static_cast<std::uint32_t>(source.timeDateStamp);
            result.sizeOfImage = static_cast<std::uint32_t>(source.sizeOfImage);
            result.imageBase = static_cast<std::uint64_t>(source.imageBase);
            result.moduleName = fixedDynWideToString(source.moduleName, KSW_DYN_MODULE_NAME_CHARS);
            return result;
        }
    }

    // queryDynDataStatus 作用：
    // - 请求 R0 当前 DynData 匹配状态；
    // - 无入参；
    // - 返回：包含 IO 结果、模块身份、capability 与不可用原因。
    DynDataStatusResult DriverClient::queryDynDataStatus() const
    {
        DynDataStatusResult result{};
        KSW_QUERY_DYN_STATUS_RESPONSE response{};

        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_DYN_STATUS,
            nullptr,
            0UL,
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!result.io.ok)
        {
            result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_DYN_STATUS) failed, error=" + std::to_string(result.io.win32Error);
            return result;
        }
        if (result.io.bytesReturned < sizeof(response) || response.version != KSWORD_ARK_DYNDATA_PROTOCOL_VERSION)
        {
            result.io.ok = false;
            result.io.message = "DynData status response invalid, bytesReturned=" + std::to_string(result.io.bytesReturned);
            return result;
        }

        result.statusFlags = static_cast<std::uint32_t>(response.statusFlags);
        result.systemInformerDataVersion = static_cast<std::uint32_t>(response.systemInformerDataVersion);
        result.systemInformerDataLength = static_cast<std::uint32_t>(response.systemInformerDataLength);
        result.lastStatus = static_cast<long>(response.lastStatus);
        result.matchedProfileClass = static_cast<std::uint32_t>(response.matchedProfileClass);
        result.matchedProfileOffset = static_cast<std::uint32_t>(response.matchedProfileOffset);
        result.matchedFieldsId = static_cast<std::uint32_t>(response.matchedFieldsId);
        result.fieldCount = static_cast<std::uint32_t>(response.fieldCount);
        result.capabilityMask = static_cast<std::uint64_t>(response.capabilityMask);
        result.ntoskrnl = convertModuleIdentity(response.ntoskrnl);
        result.lxcore = convertModuleIdentity(response.lxcore);
        result.unavailableReason = fixedDynWideToString(response.unavailableReason, KSW_DYN_REASON_CHARS);

        std::ostringstream stream;
        stream << "DynData status flags=0x" << std::hex << result.statusFlags
            << ", caps=0x" << result.capabilityMask
            << std::dec << ", fields=" << result.fieldCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus);
        result.io.message = stream.str();
        return result;
    }

    // queryDynDataCapabilities 作用：
    // - 请求 R0 当前 DynData capability 位图；
    // - 无入参；
    // - 返回：轻量 capability 查询结果。
    DynDataCapabilitiesResult DriverClient::queryDynDataCapabilities() const
    {
        DynDataCapabilitiesResult result{};
        KSW_QUERY_CAPABILITIES_RESPONSE response{};

        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_CAPABILITIES,
            nullptr,
            0UL,
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!result.io.ok)
        {
            result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_CAPABILITIES) failed, error=" + std::to_string(result.io.win32Error);
            return result;
        }
        if (result.io.bytesReturned < sizeof(response) || response.version != KSWORD_ARK_DYNDATA_PROTOCOL_VERSION)
        {
            result.io.ok = false;
            result.io.message = "DynData capabilities response invalid, bytesReturned=" + std::to_string(result.io.bytesReturned);
            return result;
        }

        result.statusFlags = static_cast<std::uint32_t>(response.statusFlags);
        result.capabilityMask = static_cast<std::uint64_t>(response.capabilityMask);
        result.io.message = "DynData capability query ok, mask=" + std::to_string(result.capabilityMask);
        return result;
    }

    // queryDynDataFields 作用：
    // - 请求 R0 当前 DynData 字段列表；
    // - 无入参；
    // - 返回：解析后的字段行以及响应总数。
    DynDataFieldsResult DriverClient::queryDynDataFields() const
    {
        DynDataFieldsResult result{};
        std::vector<std::uint8_t> responseBuffer(64U * 1024U, 0U);

        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS,
            nullptr,
            0UL,
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS) failed, error=" + std::to_string(result.io.win32Error);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(KSW_QUERY_DYN_FIELDS_RESPONSE) - sizeof(KSW_DYN_FIELD_ENTRY);
        if (result.io.bytesReturned < headerSize)
        {
            result.io.ok = false;
            result.io.message = "DynData fields response too small, bytesReturned=" + std::to_string(result.io.bytesReturned);
            return result;
        }

        const auto* responseHeader = reinterpret_cast<const KSW_QUERY_DYN_FIELDS_RESPONSE*>(responseBuffer.data());
        if (responseHeader->version != KSWORD_ARK_DYNDATA_PROTOCOL_VERSION || responseHeader->entrySize < sizeof(KSW_DYN_FIELD_ENTRY))
        {
            result.io.ok = false;
            result.io.message = "DynData fields response header invalid.";
            return result;
        }

        result.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        result.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        const std::size_t availableCount = (result.io.bytesReturned - headerSize) / static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(static_cast<std::size_t>(responseHeader->returnedCount), availableCount);
        result.entries.reserve(parsedCount);

        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset = headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSW_DYN_FIELD_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry = reinterpret_cast<const KSW_DYN_FIELD_ENTRY*>(responseBuffer.data() + entryOffset);
            DynDataFieldEntry row{};
            row.fieldId = static_cast<std::uint32_t>(sourceEntry->fieldId);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.source = static_cast<std::uint32_t>(sourceEntry->source);
            row.offset = static_cast<std::uint32_t>(sourceEntry->offset);
            row.capabilityMask = static_cast<std::uint64_t>(sourceEntry->capabilityMask);
            row.fieldName = fixedDynAnsiToString(sourceEntry->fieldName, sizeof(sourceEntry->fieldName));
            row.sourceName = fixedDynAnsiToString(sourceEntry->sourceName, sizeof(sourceEntry->sourceName));
            row.featureName = fixedDynAnsiToString(sourceEntry->featureName, sizeof(sourceEntry->featureName));
            result.entries.push_back(std::move(row));
        }

        result.io.message = "DynData fields parsed=" + std::to_string(result.entries.size())
            + ", total=" + std::to_string(result.totalCount);
        return result;
    }
}
