#include "ArkDriverClient.h"

#include <string>
namespace ksword::ark
{
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
}
