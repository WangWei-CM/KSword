#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        constexpr std::size_t kProcessCrossViewHeaderSize =
            sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE) -
            sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_ROW);
        constexpr std::size_t kThreadCrossViewHeaderSize =
            sizeof(KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE) -
            sizeof(KSWORD_ARK_THREAD_CROSSVIEW_ROW);

        bool isUnsupportedIoctlError(const unsigned long win32Error)
        {
            // 输入：DeviceIoControl 失败后的 Win32 错误。
            // 处理：匹配旧驱动未注册新 IOCTL 的常见错误码。
            // 返回：true 表示 UI 应显示“不支持/驱动过旧”。
            return win32Error == ERROR_INVALID_FUNCTION ||
                win32Error == ERROR_NOT_SUPPORTED ||
                win32Error == ERROR_INVALID_PARAMETER;
        }

        std::string fixedAnsiToString(const char* const textBuffer, const std::size_t maxBytes)
        {
            // 输入：R0 固定长度 ANSI 字段。
            // 处理：在 maxBytes 边界内寻找 NUL，避免旧响应缺 NUL 时越界。
            // 返回：std::string，空指针或空长度返回空串。
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

        CrossViewFieldOffsets copyCrossViewOffsets(const KSWORD_ARK_CROSSVIEW_FIELD_OFFSETS& source)
        {
            // 输入：共享协议中的 offset 包。
            // 处理：逐字段复制，不解释偏移值是否可用。
            // 返回：R3 侧 CrossViewFieldOffsets。
            CrossViewFieldOffsets offsets{};
            offsets.epUniqueProcessId = static_cast<std::uint32_t>(source.epUniqueProcessId);
            offsets.epActiveProcessLinks = static_cast<std::uint32_t>(source.epActiveProcessLinks);
            offsets.epThreadListHead = static_cast<std::uint32_t>(source.epThreadListHead);
            offsets.epImageFileName = static_cast<std::uint32_t>(source.epImageFileName);
            offsets.etCid = static_cast<std::uint32_t>(source.etCid);
            offsets.etThreadListEntry = static_cast<std::uint32_t>(source.etThreadListEntry);
            offsets.etStartAddress = static_cast<std::uint32_t>(source.etStartAddress);
            offsets.etWin32StartAddress = static_cast<std::uint32_t>(source.etWin32StartAddress);
            offsets.ktProcess = static_cast<std::uint32_t>(source.ktProcess);
            offsets.htTableCode = static_cast<std::uint32_t>(source.htTableCode);
            offsets.hteLowValue = static_cast<std::uint32_t>(source.hteLowValue);
            offsets.pspCidTableRva = static_cast<std::uint32_t>(source.pspCidTableRva);
            offsets.pspCidTableAddress = static_cast<std::uint64_t>(source.pspCidTableAddress);
            return offsets;
        }
    }

    ThreadEnumResult DriverClient::enumerateThreads(
        const unsigned long flags,
        const std::uint32_t processId) const
    {
        ThreadEnumResult enumResult{};
        KSWORD_ARK_ENUM_THREAD_REQUEST request{};
        request.flags = flags;
        request.processId = processId;

        std::vector<std::uint8_t> responseBuffer(1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_THREAD,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_THREAD) failed, error=" +
                std::to_string(enumResult.io.win32Error);
            return enumResult;
        }

        constexpr std::size_t headerSize =
            sizeof(KSWORD_ARK_ENUM_THREAD_RESPONSE) - sizeof(KSWORD_ARK_THREAD_ENTRY);
        if (enumResult.io.bytesReturned < headerSize)
        {
            enumResult.io.ok = false;
            enumResult.io.message =
                "enum-thread response too small, bytesReturned=" +
                std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_ENUM_THREAD_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_THREAD_ENTRY))
        {
            enumResult.io.ok = false;
            enumResult.io.message =
                "enum-thread entry size invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(responseHeader->version);
        enumResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        enumResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);

        const std::size_t availableCount =
            (enumResult.io.bytesReturned - headerSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        enumResult.entries.reserve(parsedCount);

        for (std::size_t index = 0; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            const auto* entry =
                reinterpret_cast<const KSWORD_ARK_THREAD_ENTRY*>(responseBuffer.data() + entryOffset);
            ThreadEntry parsedEntry{};

            // 复制线程身份和 cross-view flags：
            // - threadId/processId 用于 R3 线程页按 PID/TID 合并；
            // - flags 保留 R0 active-thread walk 与 CID scan 的差异标记。
            parsedEntry.threadId = static_cast<std::uint32_t>(entry->threadId);
            parsedEntry.processId = static_cast<std::uint32_t>(entry->processId);
            parsedEntry.flags = static_cast<std::uint32_t>(entry->flags);
            parsedEntry.fieldFlags = static_cast<std::uint32_t>(entry->fieldFlags);
            parsedEntry.r0Status = static_cast<std::uint32_t>(entry->r0Status);
            parsedEntry.stackFieldSource = static_cast<std::uint32_t>(entry->stackFieldSource);
            parsedEntry.ioFieldSource = static_cast<std::uint32_t>(entry->ioFieldSource);
            parsedEntry.initialStack = static_cast<std::uint64_t>(entry->initialStack);
            parsedEntry.stackLimit = static_cast<std::uint64_t>(entry->stackLimit);
            parsedEntry.stackBase = static_cast<std::uint64_t>(entry->stackBase);
            parsedEntry.kernelStack = static_cast<std::uint64_t>(entry->kernelStack);
            parsedEntry.readOperationCount = static_cast<std::uint64_t>(entry->readOperationCount);
            parsedEntry.writeOperationCount = static_cast<std::uint64_t>(entry->writeOperationCount);
            parsedEntry.otherOperationCount = static_cast<std::uint64_t>(entry->otherOperationCount);
            parsedEntry.readTransferCount = static_cast<std::uint64_t>(entry->readTransferCount);
            parsedEntry.writeTransferCount = static_cast<std::uint64_t>(entry->writeTransferCount);
            parsedEntry.otherTransferCount = static_cast<std::uint64_t>(entry->otherTransferCount);
            parsedEntry.ktInitialStackOffset = static_cast<std::uint32_t>(entry->ktInitialStackOffset);
            parsedEntry.ktStackLimitOffset = static_cast<std::uint32_t>(entry->ktStackLimitOffset);
            parsedEntry.ktStackBaseOffset = static_cast<std::uint32_t>(entry->ktStackBaseOffset);
            parsedEntry.ktKernelStackOffset = static_cast<std::uint32_t>(entry->ktKernelStackOffset);
            parsedEntry.ktReadOperationCountOffset = static_cast<std::uint32_t>(entry->ktReadOperationCountOffset);
            parsedEntry.ktWriteOperationCountOffset = static_cast<std::uint32_t>(entry->ktWriteOperationCountOffset);
            parsedEntry.ktOtherOperationCountOffset = static_cast<std::uint32_t>(entry->ktOtherOperationCountOffset);
            parsedEntry.ktReadTransferCountOffset = static_cast<std::uint32_t>(entry->ktReadTransferCountOffset);
            parsedEntry.ktWriteTransferCountOffset = static_cast<std::uint32_t>(entry->ktWriteTransferCountOffset);
            parsedEntry.ktOtherTransferCountOffset = static_cast<std::uint32_t>(entry->ktOtherTransferCountOffset);
            parsedEntry.dynDataCapabilityMask = static_cast<std::uint64_t>(entry->dynDataCapabilityMask);
            enumResult.entries.push_back(parsedEntry);
        }

        std::ostringstream stream;
        stream << "version=" << enumResult.version
            << ", total=" << enumResult.totalCount
            << ", returned=" << enumResult.returnedCount
            << ", parsed=" << enumResult.entries.size()
            << ", bytesReturned=" << enumResult.io.bytesReturned;
        enumResult.io.message = stream.str();
        return enumResult;
    }

    ProcessCrossViewResult DriverClient::queryProcessCrossView(
        const unsigned long flags,
        const std::uint32_t startPid,
        const std::uint32_t endPid,
        const unsigned long maxNodes) const
    {
        // 输入：进程 cross-view 查询 flags、PID 范围和最大节点预算。
        // 处理：调用 R0 只读 cross-view IOCTL，按 entrySize 解码 EPROCESS 来源矩阵。
        // 返回：ProcessCrossViewResult，unsupported=true 表示驱动过旧或 IOCTL 未集成。
        ProcessCrossViewResult crossViewResult{};
        KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST request{};
        request.version = KSWORD_ARK_CROSSVIEW_PROTOCOL_VERSION;
        request.flags = flags;
        request.startPid = startPid;
        request.endPid = endPid;
        request.maxNodes = maxNodes;

        std::vector<std::uint8_t> responseBuffer(4U * 1024U * 1024U, 0U);
        crossViewResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!crossViewResult.io.ok)
        {
            crossViewResult.unsupported = isUnsupportedIoctlError(crossViewResult.io.win32Error);
            crossViewResult.io.message = crossViewResult.unsupported
                ? "IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW unsupported or driver version is too old"
                : "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW) failed, error=" +
                    std::to_string(crossViewResult.io.win32Error);
            return crossViewResult;
        }
        if (crossViewResult.io.bytesReturned < kProcessCrossViewHeaderSize)
        {
            crossViewResult.io.ok = false;
            crossViewResult.io.message =
                "process cross-view response too small, bytesReturned=" +
                std::to_string(crossViewResult.io.bytesReturned);
            return crossViewResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_ROW))
        {
            crossViewResult.io.ok = false;
            crossViewResult.io.message =
                "process cross-view entrySize invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return crossViewResult;
        }

        crossViewResult.version = static_cast<std::uint32_t>(responseHeader->version);
        crossViewResult.status = static_cast<std::uint32_t>(responseHeader->status);
        crossViewResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        crossViewResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        crossViewResult.dynDataCapabilityMask = static_cast<std::uint64_t>(responseHeader->dynDataCapabilityMask);
        crossViewResult.missingCapabilityMask = static_cast<std::uint64_t>(responseHeader->missingCapabilityMask);
        crossViewResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        crossViewResult.fieldOffsets = copyCrossViewOffsets(responseHeader->fieldOffsets);
        crossViewResult.io.ntStatus = crossViewResult.lastStatus;
        if (static_cast<unsigned long>(crossViewResult.lastStatus) == 0xC00000BBUL ||
            static_cast<unsigned long>(crossViewResult.lastStatus) == 0xC0000010UL)
        {
            crossViewResult.unsupported = true;
            crossViewResult.io.ok = false;
            crossViewResult.io.message =
                "IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW unsupported by current driver response";
            return crossViewResult;
        }

        const std::size_t availableCount =
            (static_cast<std::size_t>(crossViewResult.io.bytesReturned) - kProcessCrossViewHeaderSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        crossViewResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                kProcessCrossViewHeaderSize +
                (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_ROW) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceRow =
                reinterpret_cast<const KSWORD_ARK_PROCESS_CROSSVIEW_ROW*>(responseBuffer.data() + entryOffset);
            ProcessCrossViewEntry row{};
            row.objectAddress = static_cast<std::uint64_t>(sourceRow->objectAddress);
            row.startAddress = static_cast<std::uint64_t>(sourceRow->startAddress);
            row.processId = static_cast<std::uint32_t>(sourceRow->processId);
            row.parentProcessId = static_cast<std::uint32_t>(sourceRow->parentProcessId);
            row.sourceMask = static_cast<std::uint32_t>(sourceRow->sourceMask);
            row.anomalyFlags = static_cast<std::uint32_t>(sourceRow->anomalyFlags);
            row.dynDataCapabilityMask = static_cast<std::uint64_t>(sourceRow->dynDataCapabilityMask);
            row.fieldOffsets = copyCrossViewOffsets(sourceRow->fieldOffsets);
            row.lastStatus = static_cast<long>(sourceRow->lastStatus);
            row.confidence = static_cast<std::uint32_t>(sourceRow->confidence);
            row.imageName = fixedAnsiToString(sourceRow->imageName, sizeof(sourceRow->imageName));
            row.detail = fixedAnsiToString(sourceRow->detail, sizeof(sourceRow->detail));
            crossViewResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << crossViewResult.version
            << ", status=" << crossViewResult.status
            << ", total=" << crossViewResult.totalCount
            << ", returned=" << crossViewResult.returnedCount
            << ", parsed=" << crossViewResult.entries.size()
            << ", missingCaps=0x" << std::hex << std::uppercase << crossViewResult.missingCapabilityMask
            << ", lastStatus=0x" << static_cast<unsigned long>(crossViewResult.lastStatus);
        crossViewResult.io.message = stream.str();
        return crossViewResult;
    }

    ThreadCrossViewResult DriverClient::queryThreadCrossView(
        const unsigned long flags,
        const std::uint32_t processId,
        const std::uint32_t startTid,
        const std::uint32_t endTid,
        const unsigned long maxNodes) const
    {
        // 输入：线程 cross-view 查询 flags、可选 PID/TID 范围和节点预算。
        // 处理：调用 R0 只读线程证据 IOCTL，按 entrySize 解码 ETHREAD/KTHREAD 行。
        // 返回：ThreadCrossViewResult，包含 orphan/CID-only/start-address 异常证据。
        ThreadCrossViewResult crossViewResult{};
        KSWORD_ARK_THREAD_CROSSVIEW_REQUEST request{};
        request.version = KSWORD_ARK_CROSSVIEW_PROTOCOL_VERSION;
        request.flags = flags;
        request.processId = processId;
        request.startTid = startTid;
        request.endTid = endTid;
        request.maxNodes = maxNodes;

        std::vector<std::uint8_t> responseBuffer(4U * 1024U * 1024U, 0U);
        crossViewResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!crossViewResult.io.ok)
        {
            crossViewResult.unsupported = isUnsupportedIoctlError(crossViewResult.io.win32Error);
            crossViewResult.io.message = crossViewResult.unsupported
                ? "IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW unsupported or driver version is too old"
                : "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW) failed, error=" +
                    std::to_string(crossViewResult.io.win32Error);
            return crossViewResult;
        }
        if (crossViewResult.io.bytesReturned < kThreadCrossViewHeaderSize)
        {
            crossViewResult.io.ok = false;
            crossViewResult.io.message =
                "thread cross-view response too small, bytesReturned=" +
                std::to_string(crossViewResult.io.bytesReturned);
            return crossViewResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_THREAD_CROSSVIEW_ROW))
        {
            crossViewResult.io.ok = false;
            crossViewResult.io.message =
                "thread cross-view entrySize invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return crossViewResult;
        }

        crossViewResult.version = static_cast<std::uint32_t>(responseHeader->version);
        crossViewResult.status = static_cast<std::uint32_t>(responseHeader->status);
        crossViewResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        crossViewResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        crossViewResult.dynDataCapabilityMask = static_cast<std::uint64_t>(responseHeader->dynDataCapabilityMask);
        crossViewResult.missingCapabilityMask = static_cast<std::uint64_t>(responseHeader->missingCapabilityMask);
        crossViewResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        crossViewResult.fieldOffsets = copyCrossViewOffsets(responseHeader->fieldOffsets);
        crossViewResult.io.ntStatus = crossViewResult.lastStatus;
        if (static_cast<unsigned long>(crossViewResult.lastStatus) == 0xC00000BBUL ||
            static_cast<unsigned long>(crossViewResult.lastStatus) == 0xC0000010UL)
        {
            crossViewResult.unsupported = true;
            crossViewResult.io.ok = false;
            crossViewResult.io.message =
                "IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW unsupported by current driver response";
            return crossViewResult;
        }

        const std::size_t availableCount =
            (static_cast<std::size_t>(crossViewResult.io.bytesReturned) - kThreadCrossViewHeaderSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        crossViewResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                kThreadCrossViewHeaderSize +
                (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_THREAD_CROSSVIEW_ROW) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceRow =
                reinterpret_cast<const KSWORD_ARK_THREAD_CROSSVIEW_ROW*>(responseBuffer.data() + entryOffset);
            ThreadCrossViewEntry row{};
            row.objectAddress = static_cast<std::uint64_t>(sourceRow->objectAddress);
            row.processObjectAddress = static_cast<std::uint64_t>(sourceRow->processObjectAddress);
            row.startAddress = static_cast<std::uint64_t>(sourceRow->startAddress);
            row.processId = static_cast<std::uint32_t>(sourceRow->processId);
            row.threadId = static_cast<std::uint32_t>(sourceRow->threadId);
            row.sourceMask = static_cast<std::uint32_t>(sourceRow->sourceMask);
            row.anomalyFlags = static_cast<std::uint32_t>(sourceRow->anomalyFlags);
            row.dynDataCapabilityMask = static_cast<std::uint64_t>(sourceRow->dynDataCapabilityMask);
            row.fieldOffsets = copyCrossViewOffsets(sourceRow->fieldOffsets);
            row.lastStatus = static_cast<long>(sourceRow->lastStatus);
            row.confidence = static_cast<std::uint32_t>(sourceRow->confidence);
            row.imageName = fixedAnsiToString(sourceRow->imageName, sizeof(sourceRow->imageName));
            row.detail = fixedAnsiToString(sourceRow->detail, sizeof(sourceRow->detail));
            crossViewResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << crossViewResult.version
            << ", status=" << crossViewResult.status
            << ", total=" << crossViewResult.totalCount
            << ", returned=" << crossViewResult.returnedCount
            << ", parsed=" << crossViewResult.entries.size()
            << ", missingCaps=0x" << std::hex << std::uppercase << crossViewResult.missingCapabilityMask
            << ", lastStatus=0x" << static_cast<unsigned long>(crossViewResult.lastStatus);
        crossViewResult.io.message = stream.str();
        return crossViewResult;
    }
}
