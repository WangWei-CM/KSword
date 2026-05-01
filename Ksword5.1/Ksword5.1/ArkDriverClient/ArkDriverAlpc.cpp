#include "ArkDriverClient.h"

#include <cstdint>
#include <sstream>
#include <string>

namespace ksword::ark
{
    namespace
    {
        std::wstring fixedWideToWString(const wchar_t* textBuffer, const std::size_t maxChars)
        {
            // textBuffer：共享协议中的固定 UTF-16 缓冲。
            // maxChars：扫描上限，避免旧驱动响应缺少 NUL 时越界。
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

        AlpcPortInfo parseAlpcPortInfo(const KSWORD_ARK_ALPC_PORT_INFO& responsePort)
        {
            // 作用：把共享协议端口节点转换为 UI 侧模型。
            // 处理：逐字段复制，并把固定 WCHAR 名称转为 std::wstring。
            // 返回：AlpcPortInfo 值对象。
            AlpcPortInfo parsedPort{};
            parsedPort.relation = static_cast<std::uint32_t>(responsePort.relation);
            parsedPort.fieldFlags = static_cast<std::uint32_t>(responsePort.fieldFlags);
            parsedPort.ownerProcessId = static_cast<std::uint32_t>(responsePort.ownerProcessId);
            parsedPort.flags = static_cast<std::uint32_t>(responsePort.flags);
            parsedPort.state = static_cast<std::uint32_t>(responsePort.state);
            parsedPort.sequenceNo = static_cast<std::uint32_t>(responsePort.sequenceNo);
            parsedPort.basicStatus = static_cast<long>(responsePort.basicStatus);
            parsedPort.nameStatus = static_cast<long>(responsePort.nameStatus);
            parsedPort.objectAddress = static_cast<std::uint64_t>(responsePort.objectAddress);
            parsedPort.portContext = static_cast<std::uint64_t>(responsePort.portContext);
            parsedPort.portName = fixedWideToWString(responsePort.portName, KSWORD_ARK_ALPC_PORT_NAME_CHARS);
            return parsedPort;
        }
    }

    AlpcPortQueryResult DriverClient::queryAlpcPort(
        const std::uint32_t processId,
        const std::uint64_t handleValue,
        const unsigned long flags) const
    {
        // 作用：调用 R0 ALPC 查询 IOCTL。
        // 处理：只传 PID+HandleValue，不传 object address；解析固定响应包。
        // 返回：包含 IO 结果、状态和端口关系的 AlpcPortQueryResult。
        AlpcPortQueryResult queryResult{};
        KSWORD_ARK_QUERY_ALPC_PORT_REQUEST request{};
        KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE response{};
        request.flags = flags;
        request.processId = processId;
        request.handleValue = handleValue;

        queryResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_ALPC_PORT,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!queryResult.io.ok)
        {
            queryResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_ALPC_PORT) failed, error=" +
                std::to_string(queryResult.io.win32Error);
            return queryResult;
        }
        if (queryResult.io.bytesReturned < sizeof(KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE))
        {
            queryResult.io.ok = false;
            queryResult.io.message =
                "query-alpc-port response too small, bytesReturned=" +
                std::to_string(queryResult.io.bytesReturned);
            return queryResult;
        }

        queryResult.version = static_cast<std::uint32_t>(response.version);
        queryResult.processId = static_cast<std::uint32_t>(response.processId);
        queryResult.fieldFlags = static_cast<std::uint32_t>(response.fieldFlags);
        queryResult.handleValue = static_cast<std::uint64_t>(response.handleValue);
        queryResult.queryStatus = static_cast<std::uint32_t>(response.queryStatus);
        queryResult.objectReferenceStatus = static_cast<long>(response.objectReferenceStatus);
        queryResult.typeStatus = static_cast<long>(response.typeStatus);
        queryResult.basicStatus = static_cast<long>(response.basicStatus);
        queryResult.communicationStatus = static_cast<long>(response.communicationStatus);
        queryResult.nameStatus = static_cast<long>(response.nameStatus);
        queryResult.dynDataCapabilityMask = static_cast<std::uint64_t>(response.dynDataCapabilityMask);
        queryResult.alpcCommunicationInfoOffset = static_cast<std::uint32_t>(response.alpcCommunicationInfoOffset);
        queryResult.alpcOwnerProcessOffset = static_cast<std::uint32_t>(response.alpcOwnerProcessOffset);
        queryResult.alpcConnectionPortOffset = static_cast<std::uint32_t>(response.alpcConnectionPortOffset);
        queryResult.alpcServerCommunicationPortOffset = static_cast<std::uint32_t>(response.alpcServerCommunicationPortOffset);
        queryResult.alpcClientCommunicationPortOffset = static_cast<std::uint32_t>(response.alpcClientCommunicationPortOffset);
        queryResult.alpcHandleTableOffset = static_cast<std::uint32_t>(response.alpcHandleTableOffset);
        queryResult.alpcHandleTableLockOffset = static_cast<std::uint32_t>(response.alpcHandleTableLockOffset);
        queryResult.alpcAttributesOffset = static_cast<std::uint32_t>(response.alpcAttributesOffset);
        queryResult.alpcAttributesFlagsOffset = static_cast<std::uint32_t>(response.alpcAttributesFlagsOffset);
        queryResult.alpcPortContextOffset = static_cast<std::uint32_t>(response.alpcPortContextOffset);
        queryResult.alpcPortObjectLockOffset = static_cast<std::uint32_t>(response.alpcPortObjectLockOffset);
        queryResult.alpcSequenceNoOffset = static_cast<std::uint32_t>(response.alpcSequenceNoOffset);
        queryResult.alpcStateOffset = static_cast<std::uint32_t>(response.alpcStateOffset);
        queryResult.typeName = fixedWideToWString(response.typeName, KSWORD_ARK_ALPC_TYPE_NAME_CHARS);
        queryResult.queryPort = parseAlpcPortInfo(response.queryPort);
        queryResult.connectionPort = parseAlpcPortInfo(response.connectionPort);
        queryResult.serverPort = parseAlpcPortInfo(response.serverPort);
        queryResult.clientPort = parseAlpcPortInfo(response.clientPort);

        std::ostringstream stream;
        stream << "version=" << queryResult.version
            << ", pid=" << queryResult.processId
            << ", handle=0x" << std::hex << std::uppercase << queryResult.handleValue
            << ", queryStatus=" << std::dec << queryResult.queryStatus
            << ", fieldFlags=0x" << std::hex << std::uppercase << queryResult.fieldFlags
            << std::dec << ", bytesReturned=" << queryResult.io.bytesReturned;
        queryResult.io.message = stream.str();
        return queryResult;
    }
}
