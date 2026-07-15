#include "ArkDriverClient.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <utility>

namespace ksword::ark
{
    namespace
    {
        // 识别旧版驱动没有注册新增 IOCTL 时常见的 Win32 映射。
        bool isDebugOutputUnsupportedError(const unsigned long win32Error)
        {
            return win32Error == ERROR_INVALID_FUNCTION ||
                win32Error == ERROR_NOT_SUPPORTED ||
                win32Error == ERROR_INVALID_PARAMETER;
        }

        // 解析固定控制响应；即使控制动作失败，也尽量保留驱动返回的诊断字段。
        DebugOutputControlResult controlDebugOutputImpl(
            const DriverClient& client,
            DriverHandle* handle,
            const unsigned long action)
        {
            DebugOutputControlResult result{};
            KSWORD_ARK_DEBUG_OUTPUT_CONTROL_REQUEST request{};
            KSWORD_ARK_DEBUG_OUTPUT_CONTROL_RESPONSE response{};

            request.version = KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION;
            request.size = sizeof(request);
            request.action = action;
            result.io = client.deviceIoControl(
                IOCTL_KSWORD_ARK_DEBUG_OUTPUT_CONTROL,
                &request,
                static_cast<unsigned long>(sizeof(request)),
                &response,
                static_cast<unsigned long>(sizeof(response)),
                handle);
            result.unsupported = !result.io.ok &&
                isDebugOutputUnsupportedError(result.io.win32Error);

            // 固定响应完整时逐字段复制，避免 UI 依赖协议结构的对齐方式。
            if (result.io.bytesReturned >= sizeof(response))
            {
                result.version = static_cast<std::uint32_t>(response.version);
                result.runtimeFlags = static_cast<std::uint32_t>(response.runtimeFlags);
                result.ringCapacity = static_cast<std::uint32_t>(response.ringCapacity);
                result.queuedCount = static_cast<std::uint32_t>(response.queuedCount);
                result.latestSequence = static_cast<std::uint64_t>(response.latestSequence);
                result.droppedCount = static_cast<std::uint64_t>(response.droppedCount);
                result.registrationStatus = static_cast<long>(response.registrationStatus);
                result.lastStatus = static_cast<long>(response.lastStatus);
                result.io.ntStatus = result.lastStatus;
            }
            if (result.io.ok &&
                (result.io.bytesReturned < sizeof(response) ||
                 response.version != KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION ||
                 response.size < sizeof(response)))
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INVALID_DATA;
            }

            std::ostringstream stream;
            stream << "debug-output control action=" << action
                << ", ioctl=" << (result.io.ok ? "ok" : "fail")
                << ", win32=" << result.io.win32Error
                << ", flags=0x" << std::hex << result.runtimeFlags
                << ", register=0x" << static_cast<unsigned long>(result.registrationStatus)
                << ", last=0x" << static_cast<unsigned long>(result.lastStatus)
                << std::dec << ", queued=" << result.queuedCount
                << ", dropped=" << result.droppedCount;
            result.io.message = stream.str();
            return result;
        }

        // 解析变长 drain 响应，并严格受 bytesReturned 与 entrySize 双重边界约束。
        DebugOutputDrainResult drainDebugOutputImpl(
            const DriverClient& client,
            DriverHandle* handle,
            const std::uint64_t afterSequence,
            const unsigned long maxRecords)
        {
            DebugOutputDrainResult result{};
            const unsigned long requestedRecords = std::min<unsigned long>(
                maxRecords == 0UL ? KSWORD_ARK_DEBUG_OUTPUT_DEFAULT_DRAIN_RECORDS : maxRecords,
                KSWORD_ARK_DEBUG_OUTPUT_MAX_DRAIN_RECORDS);
            constexpr std::size_t responseHeaderBytes =
                offsetof(KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE, records);
            const std::size_t outputBytes = responseHeaderBytes +
                (static_cast<std::size_t>(requestedRecords) * sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD));
            KSWORD_ARK_DEBUG_OUTPUT_DRAIN_REQUEST request{};
            std::vector<unsigned char> outputBuffer(outputBytes, 0U);

            request.version = KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION;
            request.size = sizeof(request);
            request.maxRecords = requestedRecords;
            request.afterSequence = afterSequence;
            result.io = client.deviceIoControl(
                IOCTL_KSWORD_ARK_DEBUG_OUTPUT_DRAIN,
                &request,
                static_cast<unsigned long>(sizeof(request)),
                outputBuffer.data(),
                static_cast<unsigned long>(outputBuffer.size()),
                handle);
            result.unsupported = !result.io.ok &&
                isDebugOutputUnsupportedError(result.io.win32Error);
            if (!result.io.ok)
            {
                result.io.message = result.unsupported
                    ? "IOCTL_KSWORD_ARK_DEBUG_OUTPUT_DRAIN unsupported by current driver"
                    : "DeviceIoControl(IOCTL_KSWORD_ARK_DEBUG_OUTPUT_DRAIN) failed, error=" +
                        std::to_string(result.io.win32Error);
                return result;
            }
            if (result.io.bytesReturned < responseHeaderBytes)
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
                result.io.message = "debug-output drain response header is incomplete";
                return result;
            }

            KSWORD_ARK_DEBUG_OUTPUT_DRAIN_RESPONSE header{};
            std::memcpy(&header, outputBuffer.data(), responseHeaderBytes);
            if (header.version != KSWORD_ARK_DEBUG_OUTPUT_PROTOCOL_VERSION ||
                header.entrySize < sizeof(KSWORD_ARK_DEBUG_OUTPUT_RECORD))
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INVALID_DATA;
                result.io.message = "debug-output drain protocol version or entry size is invalid";
                return result;
            }

            result.runtimeFlags = static_cast<std::uint32_t>(header.runtimeFlags);
            result.responseFlags = static_cast<std::uint32_t>(header.responseFlags);
            result.ringCapacity = static_cast<std::uint32_t>(header.ringCapacity);
            result.firstAvailableSequence = static_cast<std::uint64_t>(header.firstAvailableSequence);
            result.latestSequence = static_cast<std::uint64_t>(header.latestSequence);
            result.nextSequence = static_cast<std::uint64_t>(header.nextSequence);
            result.droppedCount = static_cast<std::uint64_t>(header.droppedCount);
            result.lostBeforeFirst = static_cast<std::uint64_t>(header.lostBeforeFirst);

            const std::size_t availableBytes = result.io.bytesReturned - responseHeaderBytes;
            const std::size_t availableRecords = availableBytes / header.entrySize;
            const std::size_t recordsToParse = std::min<std::size_t>(
                static_cast<std::size_t>(header.returnedCount),
                availableRecords);
            const unsigned char* firstRecord = outputBuffer.data() + responseHeaderBytes;
            result.records.reserve(recordsToParse);
            for (std::size_t recordIndex = 0; recordIndex < recordsToParse; ++recordIndex)
            {
                KSWORD_ARK_DEBUG_OUTPUT_RECORD packet{};
                std::memcpy(
                    &packet,
                    firstRecord + (recordIndex * header.entrySize),
                    sizeof(packet));
                DebugOutputRecord record{};
                record.sequence = static_cast<std::uint64_t>(packet.sequence);
                record.interruptTime100ns = static_cast<std::uint64_t>(packet.interruptTime100ns);
                record.componentId = static_cast<std::uint32_t>(packet.componentId);
                record.level = static_cast<std::uint32_t>(packet.level);
                record.flags = static_cast<std::uint32_t>(packet.flags);
                const std::size_t textLength = std::min<std::size_t>(
                    static_cast<std::size_t>(packet.textLengthBytes),
                    sizeof(packet.text));
                record.text.assign(packet.text, packet.text + textLength);
                result.records.push_back(std::move(record));
            }

            std::ostringstream stream;
            stream << "debug-output drain returned=" << header.returnedCount
                << ", parsed=" << result.records.size()
                << ", next=" << result.nextSequence
                << ", latest=" << result.latestSequence
                << ", lost=" << result.lostBeforeFirst
                << ", dropped=" << result.droppedCount;
            result.io.message = stream.str();
            return result;
        }
    }

    DebugOutputControlResult DriverClient::controlDebugOutput(const unsigned long action) const
    {
        return controlDebugOutputImpl(*this, nullptr, action);
    }

    DebugOutputControlResult DriverClient::controlDebugOutput(
        DriverHandle& handle,
        const unsigned long action) const
    {
        return controlDebugOutputImpl(*this, &handle, action);
    }

    DebugOutputDrainResult DriverClient::drainDebugOutput(
        const std::uint64_t afterSequence,
        const unsigned long maxRecords) const
    {
        return drainDebugOutputImpl(*this, nullptr, afterSequence, maxRecords);
    }

    DebugOutputDrainResult DriverClient::drainDebugOutput(
        DriverHandle& handle,
        const std::uint64_t afterSequence,
        const unsigned long maxRecords) const
    {
        return drainDebugOutputImpl(*this, &handle, afterSequence, maxRecords);
    }
}
