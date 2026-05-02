#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        constexpr std::size_t kReadResponseHeaderSize =
            sizeof(KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE) -
            sizeof(((KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE*)nullptr)->data);

        constexpr std::size_t kWriteRequestHeaderSize =
            sizeof(KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST) -
            sizeof(((KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST*)nullptr)->data);
    }

    VirtualMemoryReadResult DriverClient::readVirtualMemory(
        const std::uint32_t processId,
        const std::uint64_t baseAddress,
        const std::uint32_t bytesToRead,
        const unsigned long flags) const
    {
        // request 用途：承载 R3 对 R0 的读取参数，数据缓冲单独从响应中解析。
        VirtualMemoryReadResult readResult{};
        KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST request{};

        // bytesToRead 在 R3 先做上限保护，避免构造过大的响应缓冲。
        if (bytesToRead > KSWORD_ARK_MEMORY_READ_MAX_BYTES)
        {
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_PARAMETER;
            readResult.io.message = "readVirtualMemory size exceeds driver limit";
            return readResult;
        }

        // request 字段逐项填充，保持共享协议字段含义清晰。
        request.flags = flags;
        request.processId = processId;
        request.baseAddress = baseAddress;
        request.bytesToRead = bytesToRead;

        // responseBuffer 包含固定头和 data[]，长度为请求大小加头部。
        std::vector<std::uint8_t> responseBuffer(
            kReadResponseHeaderSize + static_cast<std::size_t>(bytesToRead),
            0U);
        readResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!readResult.io.ok)
        {
            readResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY) failed, error=" +
                std::to_string(readResult.io.win32Error);
            return readResult;
        }

        // 固定头不足说明驱动或协议不匹配，直接标记失败。
        if (readResult.io.bytesReturned < kReadResponseHeaderSize)
        {
            readResult.io.ok = false;
            readResult.io.message =
                "read-vm response too small, bytesReturned=" +
                std::to_string(readResult.io.bytesReturned);
            return readResult;
        }

        // responseHeader 指向 METHOD_BUFFERED 响应头，只读解析。
        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE*>(responseBuffer.data());
        readResult.version = static_cast<std::uint32_t>(responseHeader->version);
        readResult.processId = static_cast<std::uint32_t>(responseHeader->processId);
        readResult.fieldFlags = static_cast<std::uint32_t>(responseHeader->fieldFlags);
        readResult.readStatus = static_cast<std::uint32_t>(responseHeader->readStatus);
        readResult.lookupStatus = static_cast<long>(responseHeader->lookupStatus);
        readResult.copyStatus = static_cast<long>(responseHeader->copyStatus);
        readResult.source = static_cast<std::uint32_t>(responseHeader->source);
        readResult.requestedBaseAddress = static_cast<std::uint64_t>(responseHeader->requestedBaseAddress);
        readResult.requestedBytes = static_cast<std::uint32_t>(responseHeader->requestedBytes);
        readResult.bytesRead = static_cast<std::uint32_t>(responseHeader->bytesRead);
        readResult.maxBytesPerRequest = static_cast<std::uint32_t>(responseHeader->maxBytesPerRequest);

        // dataBytes 受 bytesReturned、bytesRead 和缓冲长度三者共同约束。
        const std::size_t bytesAvailable =
            static_cast<std::size_t>(readResult.io.bytesReturned) - kReadResponseHeaderSize;
        const std::size_t dataBytes = std::min<std::size_t>(
            bytesAvailable,
            static_cast<std::size_t>(readResult.bytesRead));
        if (dataBytes > 0U)
        {
            readResult.data.assign(
                responseBuffer.begin() + static_cast<std::ptrdiff_t>(kReadResponseHeaderSize),
                responseBuffer.begin() + static_cast<std::ptrdiff_t>(kReadResponseHeaderSize + dataBytes));
        }

        // message 汇总关键诊断字段，供 UI 状态栏直接展示。
        std::ostringstream stream;
        stream << "pid=" << readResult.processId
            << ", address=0x" << std::hex << std::uppercase << readResult.requestedBaseAddress
            << std::dec << ", requested=" << readResult.requestedBytes
            << ", read=" << readResult.bytesRead
            << ", status=" << readResult.readStatus
            << ", nt=0x" << std::hex << static_cast<unsigned long>(readResult.copyStatus);
        readResult.io.message = stream.str();
        return readResult;
    }

    VirtualMemoryWriteResult DriverClient::writeVirtualMemory(
        const std::uint32_t processId,
        const std::uint64_t baseAddress,
        const std::vector<std::uint8_t>& bytes,
        const unsigned long flags) const
    {
        // writeResult 用途：承载 R0 固定响应和 DeviceIoControl 状态。
        VirtualMemoryWriteResult writeResult{};

        // 空差异不应传给驱动，调用方应直接跳过。
        if (bytes.empty() || bytes.size() > KSWORD_ARK_MEMORY_WRITE_MAX_BYTES)
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "writeVirtualMemory invalid diff size";
            return writeResult;
        }

        // inputBuffer 按共享协议头 + data[] 构造，避免 UI 直接拼 IOCTL。
        std::vector<std::uint8_t> inputBuffer(kWriteRequestHeaderSize + bytes.size(), 0U);
        auto* request =
            reinterpret_cast<KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST*>(inputBuffer.data());
        request->flags = flags;
        request->processId = processId;
        request->baseAddress = baseAddress;
        request->bytesToWrite = static_cast<unsigned long>(bytes.size());
        std::copy(bytes.begin(), bytes.end(), request->data);

        // response 是固定大小结构，所有写入细节由 R0 统一填写。
        KSWORD_ARK_WRITE_VIRTUAL_MEMORY_RESPONSE response{};
        writeResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY,
            inputBuffer.data(),
            static_cast<unsigned long>(inputBuffer.size()),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!writeResult.io.ok)
        {
            writeResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY) failed, error=" +
                std::to_string(writeResult.io.win32Error);
            return writeResult;
        }
        if (writeResult.io.bytesReturned < sizeof(response))
        {
            writeResult.io.ok = false;
            writeResult.io.message =
                "write-vm response too small, bytesReturned=" +
                std::to_string(writeResult.io.bytesReturned);
            return writeResult;
        }

        // 解析响应字段，供 UI 判断成功、部分成功或失败。
        writeResult.version = static_cast<std::uint32_t>(response.version);
        writeResult.processId = static_cast<std::uint32_t>(response.processId);
        writeResult.fieldFlags = static_cast<std::uint32_t>(response.fieldFlags);
        writeResult.writeStatus = static_cast<std::uint32_t>(response.writeStatus);
        writeResult.lookupStatus = static_cast<long>(response.lookupStatus);
        writeResult.copyStatus = static_cast<long>(response.copyStatus);
        writeResult.source = static_cast<std::uint32_t>(response.source);
        writeResult.requestedBaseAddress = static_cast<std::uint64_t>(response.requestedBaseAddress);
        writeResult.requestedBytes = static_cast<std::uint32_t>(response.requestedBytes);
        writeResult.bytesWritten = static_cast<std::uint32_t>(response.bytesWritten);
        writeResult.maxBytesPerRequest = static_cast<std::uint32_t>(response.maxBytesPerRequest);

        // message 汇总本次差异块写入结果。
        std::ostringstream stream;
        stream << "pid=" << writeResult.processId
            << ", address=0x" << std::hex << std::uppercase << writeResult.requestedBaseAddress
            << std::dec << ", requested=" << writeResult.requestedBytes
            << ", written=" << writeResult.bytesWritten
            << ", status=" << writeResult.writeStatus
            << ", fields=0x" << std::hex << std::uppercase << writeResult.fieldFlags
            << ", nt=0x" << std::hex << static_cast<unsigned long>(writeResult.copyStatus);
        if (writeResult.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_FORCE_REQUIRED)
        {
            stream << ", force-required";
        }
        writeResult.io.message = stream.str();
        return writeResult;
    }
}
