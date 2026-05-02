#include "ArkDriverClient.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
namespace ksword::ark
{
    namespace
    {
        std::wstring fixedCallbackWideToString(const wchar_t* const textBuffer, const std::size_t maxChars)
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
    }

    AsyncIoResult DriverClient::waitCallbackEventAsync(
        DriverHandle& handle,
        KSWORD_ARK_CALLBACK_WAIT_REQUEST& request,
        KSWORD_ARK_CALLBACK_EVENT_PACKET& eventPacket,
        OVERLAPPED* const overlapped) const
    {
        return deviceIoControlAsync(
            handle,
            IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &eventPacket,
            static_cast<unsigned long>(sizeof(eventPacket)),
            overlapped);
    }

    IoResult DriverClient::setCallbackRules(const void* const blobBytes, const unsigned long blobSize) const
    {
        return deviceIoControl(
            IOCTL_KSWORD_ARK_SET_CALLBACK_RULES,
            const_cast<void*>(blobBytes),
            blobSize,
            nullptr,
            0);
    }

    CallbackRuntimeResult DriverClient::queryCallbackRuntimeState() const
    {
        CallbackRuntimeResult result{};
        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE,
            nullptr,
            0,
            &result.state,
            static_cast<unsigned long>(sizeof(result.state)));
        if (result.io.ok && result.io.bytesReturned < sizeof(result.state))
        {
            result.io.ok = false;
            result.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            result.io.message = "callback runtime response too small, bytesReturned=" + std::to_string(result.io.bytesReturned);
        }
        return result;
    }

    IoResult DriverClient::answerCallbackEvent(const KSWORD_ARK_CALLBACK_ANSWER_REQUEST& request) const
    {
        KSWORD_ARK_CALLBACK_ANSWER_REQUEST mutableRequest = request;
        return deviceIoControl(
            IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT,
            &mutableRequest,
            static_cast<unsigned long>(sizeof(mutableRequest)),
            nullptr,
            0);
    }

    IoResult DriverClient::cancelAllPendingCallbackDecisions() const
    {
        return deviceIoControl(
            IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS,
            nullptr,
            0,
            nullptr,
            0);
    }

    CallbackRemoveResult DriverClient::removeExternalCallback(const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST& request) const
    {
        CallbackRemoveResult result{};
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST mutableRequest = request;
        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK,
            &mutableRequest,
            static_cast<unsigned long>(sizeof(mutableRequest)),
            &result.response,
            static_cast<unsigned long>(sizeof(result.response)));
        if (result.io.ok)
        {
            result.io.ntStatus = result.response.ntstatus;
        }
        return result;
    }

    CallbackEnumResult DriverClient::enumerateCallbacks(const unsigned long flags) const
    {
        CallbackEnumResult enumResult{};
        KSWORD_ARK_ENUM_CALLBACKS_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_CALLBACK_ENUM_PROTOCOL_VERSION;
        request.flags = flags;
        request.maxEntries = 2048UL;

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_CALLBACKS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_CALLBACKS) failed, error=" + std::to_string(enumResult.io.win32Error);
            return enumResult;
        }

        constexpr std::size_t headerSize =
            sizeof(KSWORD_ARK_ENUM_CALLBACKS_RESPONSE) -
            sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY);
        if (enumResult.io.bytesReturned < headerSize)
        {
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            enumResult.io.message = "callback enum response too small, bytesReturned=" + std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_ENUM_CALLBACKS_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY))
        {
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INVALID_DATA;
            enumResult.io.message = "callback enum entrySize invalid, entrySize=" + std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(responseHeader->version);
        enumResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        enumResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        enumResult.flags = static_cast<std::uint32_t>(responseHeader->flags);
        enumResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        enumResult.io.ntStatus = enumResult.lastStatus;

        const std::size_t availableCount =
            (enumResult.io.bytesReturned - headerSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        enumResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry =
                reinterpret_cast<const KSWORD_ARK_CALLBACK_ENUM_ENTRY*>(responseBuffer.data() + entryOffset);
            CallbackEnumEntry row{};
            row.callbackClass = static_cast<std::uint32_t>(sourceEntry->callbackClass);
            row.source = static_cast<std::uint32_t>(sourceEntry->source);
            row.status = static_cast<std::uint32_t>(sourceEntry->status);
            row.fieldFlags = static_cast<std::uint32_t>(sourceEntry->fieldFlags);
            row.operationMask = static_cast<std::uint32_t>(sourceEntry->operationMask);
            row.objectTypeMask = static_cast<std::uint32_t>(sourceEntry->objectTypeMask);
            row.lastStatus = static_cast<long>(sourceEntry->lastStatus);
            row.callbackAddress = static_cast<std::uint64_t>(sourceEntry->callbackAddress);
            row.contextAddress = static_cast<std::uint64_t>(sourceEntry->contextAddress);
            row.registrationAddress = static_cast<std::uint64_t>(sourceEntry->registrationAddress);
            row.moduleBase = static_cast<std::uint64_t>(sourceEntry->moduleBase);
            row.moduleSize = static_cast<std::uint32_t>(sourceEntry->moduleSize);
            row.name = fixedCallbackWideToString(sourceEntry->name, KSWORD_ARK_CALLBACK_ENUM_NAME_CHARS);
            row.altitude = fixedCallbackWideToString(sourceEntry->altitude, KSWORD_ARK_CALLBACK_ENUM_ALTITUDE_CHARS);
            row.modulePath = fixedCallbackWideToString(sourceEntry->modulePath, KSWORD_ARK_CALLBACK_ENUM_MODULE_PATH_CHARS);
            row.detail = fixedCallbackWideToString(sourceEntry->detail, KSWORD_ARK_CALLBACK_ENUM_DETAIL_CHARS);
            enumResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << enumResult.version
            << ", total=" << enumResult.totalCount
            << ", returned=" << enumResult.returnedCount
            << ", parsed=" << enumResult.entries.size()
            << ", flags=0x" << std::hex << enumResult.flags
            << ", bytesReturned=" << std::dec << enumResult.io.bytesReturned;
        enumResult.io.message = stream.str();
        return enumResult;
    }
}
