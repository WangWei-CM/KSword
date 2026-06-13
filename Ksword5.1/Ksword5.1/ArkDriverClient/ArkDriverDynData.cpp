#include "ArkDriverClient.h"

#include <algorithm>
#include <cstring>
#include <limits>
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

        // copyDynAnsi 作用：
        // - 将 std::string 安全复制进共享协议固定 char 数组；
        // - 入参 destination/destinationBytes/source：目标缓冲、容量、来源文本；
        // - 返回：无；始终保证目标 NUL 结尾。
        void copyDynAnsi(char* const destination, const std::size_t destinationBytes, const std::string& source)
        {
            if (destination == nullptr || destinationBytes == 0U)
            {
                return;
            }

            std::memset(destination, 0, destinationBytes);
            const std::size_t bytesToCopy = std::min<std::size_t>(source.size(), destinationBytes - 1U);
            if (bytesToCopy > 0U)
            {
                std::memcpy(destination, source.data(), bytesToCopy);
            }
        }

        // buildIdentityPacket 作用：
        // - 把 R3 模块身份转换成共享协议 packet；
        // - 入参 source：ArkDynModuleIdentity；
        // - 返回：KSW_DYN_MODULE_IDENTITY_PACKET 值对象。
        KSW_DYN_MODULE_IDENTITY_PACKET buildIdentityPacket(const ArkDynModuleIdentity& source)
        {
            KSW_DYN_MODULE_IDENTITY_PACKET packet{};
            packet.present = source.present ? 1UL : 0UL;
            packet.classId = static_cast<unsigned long>(source.classId);
            packet.machine = static_cast<unsigned long>(source.machine);
            packet.timeDateStamp = static_cast<unsigned long>(source.timeDateStamp);
            packet.sizeOfImage = static_cast<unsigned long>(source.sizeOfImage);
            packet.imageBase = static_cast<unsigned long long>(source.imageBase);
            const std::size_t charsToCopy = std::min<std::size_t>(
                source.moduleName.size(),
                static_cast<std::size_t>(KSW_DYN_MODULE_NAME_CHARS - 1U));
            for (std::size_t index = 0U; index < charsToCopy; ++index)
            {
                packet.moduleName[index] = source.moduleName[index];
            }
            packet.moduleName[KSW_DYN_MODULE_NAME_CHARS - 1U] = L'\0';
            return packet;
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

    // applyDynDataProfile 作用：
    // - 将 R3 已解析 JSON profile 打包成 IOCTL 请求并发送给 R0；
    // - 入参 profile：profile 元数据、ntoskrnl identity 和字段数组；
    // - 返回：R0 apply 响应；io.ok 表示传输/协议可用，status 表示语义应用结果。
    DynDataProfileApplyResult DriverClient::applyDynDataProfile(const DynDataProfileApplyInput& profile) const
    {
        DynDataProfileApplyResult result{};
        if (profile.fields.empty() || profile.fields.size() > KSW_DYN_PROFILE_MAX_FIELDS)
        {
            result.io.ok = false;
            result.io.win32Error = ERROR_INVALID_PARAMETER;
            result.io.message = "DynData profile field count invalid.";
            return result;
        }

        const std::size_t requestBytes =
            KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE +
            (profile.fields.size() * sizeof(KSW_DYN_PROFILE_FIELD_PACKET));
        if (requestBytes > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max()))
        {
            result.io.ok = false;
            result.io.win32Error = ERROR_INVALID_PARAMETER;
            result.io.message = "DynData profile request too large.";
            return result;
        }

        std::vector<std::uint8_t> requestBuffer(requestBytes, 0U);
        auto* request = reinterpret_cast<KSW_APPLY_DYN_PROFILE_REQUEST*>(requestBuffer.data());
        request->size = static_cast<unsigned long>(requestBytes);
        request->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
        request->flags = KSW_DYN_PROFILE_FLAG_TRANSPORT_IOCTL;
        request->fieldCount = static_cast<unsigned long>(profile.fields.size());
        request->ntoskrnl = buildIdentityPacket(profile.ntoskrnl);
        copyDynAnsi(request->profileName, sizeof(request->profileName), profile.profileName);
        copyDynAnsi(request->pdbName, sizeof(request->pdbName), profile.pdbName);
        copyDynAnsi(request->pdbGuid, sizeof(request->pdbGuid), profile.pdbGuid);
        request->pdbAge = static_cast<unsigned long>(profile.pdbAge);

        for (std::size_t index = 0U; index < profile.fields.size(); ++index)
        {
            request->fields[index].fieldId = static_cast<unsigned long>(profile.fields[index].fieldId);
            request->fields[index].offset = static_cast<unsigned long>(profile.fields[index].offset);
        }

        KSW_APPLY_DYN_PROFILE_RESPONSE response{};
        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE,
            requestBuffer.data(),
            static_cast<unsigned long>(requestBuffer.size()),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!result.io.ok)
        {
            result.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE) failed, error=" + std::to_string(result.io.win32Error);
            return result;
        }
        if (result.io.bytesReturned < sizeof(response) || response.version != KSWORD_ARK_DYNDATA_PROTOCOL_VERSION)
        {
            result.io.ok = false;
            result.io.message = "DynData profile apply response invalid, bytesReturned=" + std::to_string(result.io.bytesReturned);
            return result;
        }

        result.status = static_cast<long>(response.status);
        result.appliedFieldCount = static_cast<std::uint32_t>(response.appliedFieldCount);
        result.rejectedFieldCount = static_cast<std::uint32_t>(response.rejectedFieldCount);
        result.unknownFieldCount = static_cast<std::uint32_t>(response.unknownFieldCount);
        result.statusFlags = static_cast<std::uint32_t>(response.statusFlags);
        result.capabilityMask = static_cast<std::uint64_t>(response.capabilityMask);
        result.message = fixedDynWideToString(response.message, KSW_DYN_REASON_CHARS);
        result.io.ntStatus = result.status;

        std::ostringstream stream;
        stream << "DynData PDB profile apply status=0x" << std::hex << static_cast<unsigned long>(result.status)
            << std::dec << ", applied=" << result.appliedFieldCount
            << ", rejected=" << result.rejectedFieldCount
            << ", unknown=" << result.unknownFieldCount;
        result.io.message = stream.str();
        return result;
    }
}
