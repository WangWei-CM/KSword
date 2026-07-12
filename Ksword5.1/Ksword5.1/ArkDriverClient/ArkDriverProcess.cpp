#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <utility>
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
            offsets.epUniqueProcessIdSource = static_cast<std::uint32_t>(source.epUniqueProcessIdSource);
            offsets.epActiveProcessLinksSource = static_cast<std::uint32_t>(source.epActiveProcessLinksSource);
            offsets.epThreadListHeadSource = static_cast<std::uint32_t>(source.epThreadListHeadSource);
            offsets.epImageFileNameSource = static_cast<std::uint32_t>(source.epImageFileNameSource);
            offsets.etCidSource = static_cast<std::uint32_t>(source.etCidSource);
            offsets.etThreadListEntrySource = static_cast<std::uint32_t>(source.etThreadListEntrySource);
            offsets.etStartAddressSource = static_cast<std::uint32_t>(source.etStartAddressSource);
            offsets.etWin32StartAddressSource = static_cast<std::uint32_t>(source.etWin32StartAddressSource);
            offsets.ktProcessSource = static_cast<std::uint32_t>(source.ktProcessSource);
            offsets.htTableCodeSource = static_cast<std::uint32_t>(source.htTableCodeSource);
            offsets.hteLowValueSource = static_cast<std::uint32_t>(source.hteLowValueSource);
            offsets.pspCidTableSource = static_cast<std::uint32_t>(source.pspCidTableSource);
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
            row.publicProcessId = static_cast<std::uint32_t>(sourceRow->publicProcessId);
            row.activeListProcessId = static_cast<std::uint32_t>(sourceRow->activeListProcessId);
            row.cidTableProcessId = static_cast<std::uint32_t>(sourceRow->cidTableProcessId);
            row.publicWalkStatus = static_cast<long>(sourceRow->publicWalkStatus);
            row.activeListStatus = static_cast<long>(sourceRow->activeListStatus);
            row.cidTableStatus = static_cast<long>(sourceRow->cidTableStatus);
            row.detailStatus = static_cast<std::uint32_t>(sourceRow->detailStatus);
            row.denoiseFlags = static_cast<std::uint32_t>(sourceRow->denoiseFlags);
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
            row.publicThreadId = static_cast<std::uint32_t>(sourceRow->publicThreadId);
            row.threadListThreadId = static_cast<std::uint32_t>(sourceRow->threadListThreadId);
            row.cidTableThreadId = static_cast<std::uint32_t>(sourceRow->cidTableThreadId);
            row.publicProcessId = static_cast<std::uint32_t>(sourceRow->publicProcessId);
            row.threadListProcessId = static_cast<std::uint32_t>(sourceRow->threadListProcessId);
            row.cidTableProcessId = static_cast<std::uint32_t>(sourceRow->cidTableProcessId);
            row.publicWalkStatus = static_cast<long>(sourceRow->publicWalkStatus);
            row.threadListStatus = static_cast<long>(sourceRow->threadListStatus);
            row.cidTableStatus = static_cast<long>(sourceRow->cidTableStatus);
            row.startAddressStatus = static_cast<long>(sourceRow->startAddressStatus);
            row.detailStatus = static_cast<std::uint32_t>(sourceRow->detailStatus);
            row.denoiseFlags = static_cast<std::uint32_t>(sourceRow->denoiseFlags);
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

    namespace
    {
        bool isUnsupportedRuntimeDetailResponse(
            const unsigned long detailStatus,
            const long lastStatus)
        {
            // 输入：R0 固定详情响应中的语义 status 与 NTSTATUS。
            // 处理：识别旧驱动/协议版本/未实现路径，不把它当成 UI 崩溃或解析失败。
            // 返回：true 表示调用方应展示 unsupported/unavailable。
            return detailStatus == KSWORD_ARK_DETAIL_STATUS_UNSUPPORTED ||
                static_cast<unsigned long>(lastStatus) == 0xC00000BBUL ||
                static_cast<unsigned long>(lastStatus) == 0xC0000010UL;
        }

        std::string buildRuntimeDetailMessage(
            const char* const operationName,
            const unsigned long detailStatus,
            const unsigned long fieldFlags,
            const unsigned long long missingCapabilityMask,
            const long lastStatus,
            const unsigned long bytesReturned)
        {
            // 输入：固定详情响应的核心诊断字段。
            // 处理：生成一行稳定摘要，供 UI 最后一列/详情区展示。
            // 返回：包含 status、字段位、缺失 capability 和字节数的 std::string。
            std::ostringstream stream;
            stream << (operationName != nullptr ? operationName : "runtime detail")
                << " status=" << detailStatus
                << ", fields=0x" << std::hex << std::uppercase << fieldFlags
                << ", missingCaps=0x" << missingCapabilityMask
                << ", lastStatus=0x" << static_cast<unsigned long>(lastStatus)
                << std::dec << ", bytesReturned=" << bytesReturned;
            return stream.str();
        }
    }

    ProcessRuntimeDetailResult DriverClient::queryProcessRuntimeDetail(
        const std::uint32_t processId,
        const unsigned long flags) const
    {
        // 输入：目标 PID 和字段组 flags。
        // 处理：封装 IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL，固定响应不足时给出明确错误。
        // 返回：ProcessRuntimeDetailResult；unsupported=true 表示旧驱动缺入口或 R0 明确未实现。
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL";
        ProcessRuntimeDetailResult detailResult{};
        KSWORD_ARK_PROCESS_DETAIL_REQUEST request{};
        request.version = KSWORD_ARK_RUNTIME_DETAIL_PROTOCOL_VERSION;
        request.flags = flags;
        request.processId = processId;

        detailResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &detailResult.response,
            static_cast<unsigned long>(sizeof(detailResult.response)));
        if (!detailResult.io.ok)
        {
            detailResult.unsupported = isUnsupportedIoctlError(detailResult.io.win32Error);
            detailResult.io.message = detailResult.unsupported
                ? "IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL unsupported or driver version is too old"
                : "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_PROCESS_DETAIL) failed, error=" +
                    std::to_string(detailResult.io.win32Error);
            return detailResult;
        }

        if (detailResult.io.bytesReturned < sizeof(detailResult.response))
        {
            detailResult.io.ok = false;
            detailResult.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            detailResult.io.message =
                "process runtime detail response too small, bytesReturned=" +
                std::to_string(detailResult.io.bytesReturned);
            return detailResult;
        }

        detailResult.io.ntStatus = detailResult.response.lastStatus;
        detailResult.unsupported = isUnsupportedRuntimeDetailResponse(
            detailResult.response.status,
            detailResult.response.lastStatus);
        detailResult.io.message = buildRuntimeDetailMessage(
            operationName,
            detailResult.response.status,
            detailResult.response.fieldFlags,
            detailResult.response.missingCapabilityMask,
            detailResult.response.lastStatus,
            detailResult.io.bytesReturned);
        return detailResult;
    }

    ThreadRuntimeDetailResult DriverClient::queryThreadRuntimeDetail(
        const std::uint32_t threadId,
        const std::uint32_t processId,
        const unsigned long flags) const
    {
        // 输入：目标 TID、可选 PID 约束和字段组 flags。
        // 处理：封装 IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL；PID 不匹配由 R0 写入语义状态。
        // 返回：ThreadRuntimeDetailResult；调用方只展示证据，不执行线程动作。
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL";
        ThreadRuntimeDetailResult detailResult{};
        KSWORD_ARK_THREAD_DETAIL_REQUEST request{};
        request.version = KSWORD_ARK_RUNTIME_DETAIL_PROTOCOL_VERSION;
        request.flags = flags;
        request.threadId = threadId;
        request.processId = processId;

        detailResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &detailResult.response,
            static_cast<unsigned long>(sizeof(detailResult.response)));
        if (!detailResult.io.ok)
        {
            detailResult.unsupported = isUnsupportedIoctlError(detailResult.io.win32Error);
            detailResult.io.message = detailResult.unsupported
                ? "IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL unsupported or driver version is too old"
                : "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_THREAD_DETAIL) failed, error=" +
                    std::to_string(detailResult.io.win32Error);
            return detailResult;
        }

        if (detailResult.io.bytesReturned < sizeof(detailResult.response))
        {
            detailResult.io.ok = false;
            detailResult.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            detailResult.io.message =
                "thread runtime detail response too small, bytesReturned=" +
                std::to_string(detailResult.io.bytesReturned);
            return detailResult;
        }

        detailResult.io.ntStatus = detailResult.response.lastStatus;
        detailResult.unsupported = isUnsupportedRuntimeDetailResponse(
            detailResult.response.status,
            detailResult.response.lastStatus);
        detailResult.io.message = buildRuntimeDetailMessage(
            operationName,
            detailResult.response.status,
            detailResult.response.fieldFlags,
            detailResult.response.missingCapabilityMask,
            detailResult.response.lastStatus,
            detailResult.io.bytesReturned);
        return detailResult;
    }

    namespace
    {
        std::size_t runtimeFieldSampleResponseHeaderSize()
        {
            // 输入：无。
            // 处理：扣除 entries[1] 占位，得到变长响应头长度。
            // 返回：用于解析 R0 runtime field sample 响应的 header size。
            return sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE) -
                sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW);
        }

        std::size_t processRuntimeFieldSampleRequestHeaderSize()
        {
            // 输入：无。
            // 处理：扣除 items[1] 占位，得到进程 sample 请求头长度。
            // 返回：用于构造 IOCTL 输入缓冲区的 header size。
            return sizeof(KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST) -
                sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST);
        }

        std::size_t threadRuntimeFieldSampleRequestHeaderSize()
        {
            // 输入：无。
            // 处理：扣除 items[1] 占位，得到线程 sample 请求头长度。
            // 返回：用于构造 IOCTL 输入缓冲区的 header size。
            return sizeof(KSWORD_ARK_THREAD_RUNTIME_FIELD_SAMPLE_REQUEST) -
                sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST);
        }

        std::string buildRuntimeFieldSampleMessage(
            const char* const operationName,
            const RuntimeFieldSampleResult& result,
            const unsigned long bytesReturned)
        {
            // 输入：sampler 操作名、解析后的响应和 DeviceIoControl 返回字节数。
            // 处理：生成 UI 可读的一行稳定诊断。
            // 返回：std::string，供详情页转换为中文说明或直接调试。
            std::ostringstream stream;
            stream << (operationName != nullptr ? operationName : "runtime field sample")
                << " status=" << result.status
                << ", returned=" << result.returnedCount
                << "/" << result.totalCount
                << ", object=0x" << std::hex << std::uppercase << result.objectAddress
                << ", dynCaps=0x" << result.dynDataCapabilityMask
                << ", lastStatus=0x" << static_cast<unsigned long>(result.lastStatus)
                << std::dec << ", bytesReturned=" << bytesReturned;
            return stream.str();
        }

        RuntimeFieldSampleResult parseRuntimeFieldSampleResponse(
            IoResult ioResult,
            const std::vector<std::uint8_t>& responseBuffer,
            const std::vector<RuntimeFieldSampleRequestItem>& requestedItems,
            const char* const operationName)
        {
            // 输入：DeviceIoControl 结果、原始响应缓冲区和请求元数据。
            // 处理：校验变长响应头/entrySize，并把 R0 row 转成 R3 模型。
            // 返回：RuntimeFieldSampleResult；失败时 io.ok=false 且保留明确 message。
            RuntimeFieldSampleResult result{};
            const std::size_t headerSize = runtimeFieldSampleResponseHeaderSize();
            result.io = ioResult;

            if (!result.io.ok)
            {
                result.unsupported = isUnsupportedIoctlError(result.io.win32Error);
                result.io.message = result.unsupported
                    ? std::string(operationName) + " unsupported or driver version is too old"
                    : std::string("DeviceIoControl(") + operationName + ") failed, error=" +
                        std::to_string(result.io.win32Error);
                return result;
            }
            if (result.io.bytesReturned < headerSize || responseBuffer.size() < headerSize)
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
                result.io.message = std::string(operationName) +
                    " response too small, bytesReturned=" +
                    std::to_string(result.io.bytesReturned);
                return result;
            }

            const auto* response = reinterpret_cast<const KSWORD_ARK_RUNTIME_FIELD_SAMPLE_RESPONSE*>(responseBuffer.data());
            result.version = static_cast<std::uint32_t>(response->version);
            result.status = static_cast<std::uint32_t>(response->status);
            result.totalCount = static_cast<std::uint32_t>(response->totalCount);
            result.returnedCount = static_cast<std::uint32_t>(response->returnedCount);
            result.entrySize = static_cast<std::uint32_t>(response->entrySize);
            result.flags = static_cast<std::uint32_t>(response->flags);
            result.lastStatus = response->lastStatus;
            result.objectAddress = static_cast<std::uint64_t>(response->objectAddress);
            result.dynDataCapabilityMask = static_cast<std::uint64_t>(response->dynDataCapabilityMask);
            result.io.ntStatus = response->lastStatus;

            if (result.entrySize < sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW))
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INVALID_DATA;
                result.io.message = std::string(operationName) + " invalid entrySize=" +
                    std::to_string(result.entrySize);
                return result;
            }

            const std::size_t availableRows =
                (result.io.bytesReturned - headerSize) / result.entrySize;
            const std::size_t parsedRows = std::min<std::size_t>(
                availableRows,
                static_cast<std::size_t>(result.returnedCount));
            result.entries.reserve(parsedRows);
            for (std::size_t rowIndex = 0; rowIndex < parsedRows; ++rowIndex)
            {
                const auto* row = reinterpret_cast<const KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW*>(
                    responseBuffer.data() + headerSize + (rowIndex * result.entrySize));
                RuntimeFieldSampleEntry entry{};
                entry.runtimeItemId = static_cast<std::uint32_t>(row->runtimeItemId);
                entry.offset = static_cast<std::uint32_t>(row->offset);
                entry.size = static_cast<std::uint32_t>(row->size);
                entry.status = static_cast<std::uint32_t>(row->status);
                entry.bytesRead = static_cast<std::uint32_t>(row->bytesRead);
                entry.flags = static_cast<std::uint32_t>(row->flags);
                entry.lastStatus = row->lastStatus;
                entry.valueU64 = static_cast<std::uint64_t>(row->valueU64);
                const std::size_t byteCount = std::min<std::size_t>(
                    row->bytesRead,
                    KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_VALUE_BYTES);
                entry.sampleBytes.assign(row->sampleBytes, row->sampleBytes + byteCount);
                if (rowIndex < requestedItems.size())
                {
                    entry.name = requestedItems[rowIndex].name;
                    entry.type = requestedItems[rowIndex].type;
                }
                result.entries.push_back(std::move(entry));
            }

            result.io.message = buildRuntimeFieldSampleMessage(
                operationName,
                result,
                result.io.bytesReturned);
            return result;
        }
    }

    RuntimeFieldSampleResult DriverClient::queryProcessRuntimeFieldSamples(
        const std::uint32_t processId,
        const std::vector<RuntimeFieldSampleRequestItem>& items,
        const unsigned long flags) const
    {
        // 输入：目标 PID 与 deep PDB 字段列表。
        // 处理：构造变长请求，调用只读 0x83E sampler。
        // 返回：RuntimeFieldSampleResult；旧驱动或协议缺失时 unsupported=true。
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_PROCESS_RUNTIME_FIELDS";
        const std::size_t itemCount = std::min<std::size_t>(items.size(), KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS);
        const std::size_t headerSize = processRuntimeFieldSampleRequestHeaderSize();
        const std::size_t inputSize = headerSize +
            (itemCount * sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST));
        const std::size_t outputSize = runtimeFieldSampleResponseHeaderSize() +
            (itemCount * sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW));
        std::vector<std::uint8_t> inputBuffer(inputSize, 0U);
        std::vector<std::uint8_t> outputBuffer(outputSize, 0U);
        auto* request = reinterpret_cast<KSWORD_ARK_PROCESS_RUNTIME_FIELD_SAMPLE_REQUEST*>(inputBuffer.data());
        request->version = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_PROTOCOL_VERSION;
        request->flags = flags;
        request->processId = processId;
        request->itemCount = static_cast<unsigned long>(itemCount);
        for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex)
        {
            request->items[itemIndex].runtimeItemId = items[itemIndex].runtimeItemId;
            request->items[itemIndex].offset = items[itemIndex].offset;
            request->items[itemIndex].size = items[itemIndex].size;
            request->items[itemIndex].flags = items[itemIndex].flags;
        }

        IoResult ioResult = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_PROCESS_RUNTIME_FIELDS,
            inputBuffer.data(),
            static_cast<unsigned long>(inputBuffer.size()),
            outputBuffer.data(),
            static_cast<unsigned long>(outputBuffer.size()));
        return parseRuntimeFieldSampleResponse(
            ioResult,
            outputBuffer,
            items,
            operationName);
    }

    RuntimeFieldSampleResult DriverClient::queryThreadRuntimeFieldSamples(
        const std::uint32_t threadId,
        const std::uint32_t processId,
        const std::vector<RuntimeFieldSampleRequestItem>& items,
        const unsigned long flags) const
    {
        // 输入：目标 TID、可选 PID 和 deep PDB 字段列表。
        // 处理：构造变长请求，调用只读 0x83F sampler。
        // 返回：RuntimeFieldSampleResult；旧驱动或协议缺失时 unsupported=true。
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_THREAD_RUNTIME_FIELDS";
        const std::size_t itemCount = std::min<std::size_t>(items.size(), KSWORD_ARK_RUNTIME_FIELD_SAMPLE_MAX_ITEMS);
        const std::size_t headerSize = threadRuntimeFieldSampleRequestHeaderSize();
        const std::size_t inputSize = headerSize +
            (itemCount * sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ITEM_REQUEST));
        const std::size_t outputSize = runtimeFieldSampleResponseHeaderSize() +
            (itemCount * sizeof(KSWORD_ARK_RUNTIME_FIELD_SAMPLE_ROW));
        std::vector<std::uint8_t> inputBuffer(inputSize, 0U);
        std::vector<std::uint8_t> outputBuffer(outputSize, 0U);
        auto* request = reinterpret_cast<KSWORD_ARK_THREAD_RUNTIME_FIELD_SAMPLE_REQUEST*>(inputBuffer.data());
        request->version = KSWORD_ARK_RUNTIME_FIELD_SAMPLE_PROTOCOL_VERSION;
        request->flags = flags;
        request->threadId = threadId;
        request->processId = processId;
        request->itemCount = static_cast<unsigned long>(itemCount);
        for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex)
        {
            request->items[itemIndex].runtimeItemId = items[itemIndex].runtimeItemId;
            request->items[itemIndex].offset = items[itemIndex].offset;
            request->items[itemIndex].size = items[itemIndex].size;
            request->items[itemIndex].flags = items[itemIndex].flags;
        }

        IoResult ioResult = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_THREAD_RUNTIME_FIELDS,
            inputBuffer.data(),
            static_cast<unsigned long>(inputBuffer.size()),
            outputBuffer.data(),
            static_cast<unsigned long>(outputBuffer.size()));
        return parseRuntimeFieldSampleResponse(
            ioResult,
            outputBuffer,
            items,
            operationName);
    }

}
