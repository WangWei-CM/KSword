#include "ArkDriverClient.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        // kDefaultAuditBufferBytes 用途：为新增只读审计 IOCTL 提供统一输出缓冲。
        // 处理逻辑：多数协议都是 count-first + bounded rows，4MB 足够首版 UI 展示。
        // 返回行为：常量无返回值，调用方仍按 bytesReturned 和 entrySize 二次限界。
        constexpr std::size_t kDefaultAuditBufferBytes = 4U * 1024U * 1024U;

        // fixedAuditWideToString 作用：
        // - 输入：共享协议定长 wchar_t 数组和最大字符数；
        // - 处理：扫描到 NUL 或边界，避免旧驱动缺 NUL 时越界；
        // - 返回：安全 std::wstring。
        std::wstring fixedAuditWideToString(const wchar_t* const textBuffer, const std::size_t maxChars)
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

        // copyAuditWideToFixed 作用：
        // - 输入：目标定长宽字符数组、容量和 R3 字符串；
        // - 处理：清零、截断复制并保证 NUL 结尾；
        // - 返回：无返回值。
        void copyAuditWideToFixed(wchar_t* const destination, const std::size_t destinationChars, const std::wstring& source)
        {
            if (destination == nullptr || destinationChars == 0U)
            {
                return;
            }

            std::fill(destination, destination + destinationChars, L'\0');
            const std::size_t copyChars = std::min<std::size_t>(source.size(), destinationChars - 1U);
            if (copyChars != 0U)
            {
                std::copy(source.data(), source.data() + static_cast<std::ptrdiff_t>(copyChars), destination);
            }
        }

        // isAuditUnsupportedError 作用：
        // - 输入：DeviceIoControl 失败后的 Win32 错误；
        // - 处理：识别旧驱动未注册新 IOCTL 的常见错误；
        // - 返回：true 表示 UI 应显示 unsupported 而不是协议损坏。
        bool isAuditUnsupportedError(const unsigned long win32Error)
        {
            return win32Error == ERROR_INVALID_FUNCTION ||
                win32Error == ERROR_NOT_SUPPORTED ||
                win32Error == ERROR_INVALID_PARAMETER;
        }

        // markUnsupportedIfNeeded 作用：
        // - 输入：任意包含 io/unsupported 字段的结果和操作名；
        // - 处理：当 IOCTL 失败时补齐统一 message 和 unsupported 标志；
        // - 返回：无返回值，Result 原地更新。
        template <typename TResult>
        void markUnsupportedIfNeeded(TResult& result, const char* const operationName)
        {
            if (result.io.ok)
            {
                return;
            }

            result.unsupported = isAuditUnsupportedError(result.io.win32Error);
            std::ostringstream stream;
            stream << "DeviceIoControl(" << (operationName != nullptr ? operationName : "audit")
                << ") failed, error=" << result.io.win32Error;
            if (result.unsupported)
            {
                stream << ", unsupported=true";
            }
            result.io.message = stream.str();
        }

        // validateAuditRows 作用：
        // - 输入：返回字节数、头大小、entrySize、最小行结构大小和 returnedCount；
        // - 处理：验证变长响应边界并计算可安全解析的行数；
        // - 返回：可解析行数，失败时设置 io 并返回 0。
        std::size_t validateAuditRows(
            IoResult& io,
            const std::size_t headerSize,
            const std::uint32_t entrySize,
            const std::size_t minimumEntrySize,
            const std::uint32_t returnedCount,
            const char* const operationName)
        {
            if (io.bytesReturned < headerSize)
            {
                io.ok = false;
                io.win32Error = ERROR_INSUFFICIENT_BUFFER;
                io.message = std::string(operationName) + " response too small, bytesReturned=" + std::to_string(io.bytesReturned);
                return 0U;
            }
            if (entrySize < minimumEntrySize)
            {
                io.ok = false;
                io.win32Error = ERROR_INVALID_DATA;
                io.message = std::string(operationName) + " entrySize invalid, entrySize=" + std::to_string(entrySize);
                return 0U;
            }

            const std::size_t availableRows = (io.bytesReturned - headerSize) / static_cast<std::size_t>(entrySize);
            return std::min<std::size_t>(static_cast<std::size_t>(returnedCount), availableRows);
        }

        // appendAuditSummary 作用：
        // - 输入：操作名、总数、返回数、解析数和字节数；
        // - 处理：生成统一成功诊断字符串；
        // - 返回：std::string，可直接写入 IoResult::message。
        std::string appendAuditSummary(
            const char* const operationName,
            const std::uint32_t totalCount,
            const std::uint32_t returnedCount,
            const std::size_t parsedCount,
            const unsigned long bytesReturned)
        {
            std::ostringstream stream;
            stream << operationName
                << " total=" << totalCount
                << ", returned=" << returnedCount
                << ", parsed=" << parsedCount
                << ", bytesReturned=" << bytesReturned;
            return stream.str();
        }

        // parseVariableRows 作用：
        // - 输入：响应缓冲、头部大小、行大小和解析行数；
        // - 处理：逐行 memcpy 到 std::vector，避免直接保存悬空指针；
        // - 返回：行 vector，行类型必须是 trivially copyable 协议结构。
        template <typename TEntry>
        std::vector<TEntry> parseVariableRows(
            const std::vector<std::uint8_t>& responseBuffer,
            const std::size_t headerSize,
            const std::uint32_t entrySize,
            const std::size_t parsedCount)
        {
            static_assert(std::is_trivially_copyable_v<TEntry>, "audit protocol rows must be trivially copyable");
            std::vector<TEntry> rows;
            rows.reserve(parsedCount);
            for (std::size_t index = 0U; index < parsedCount; ++index)
            {
                const std::size_t offset = headerSize + (index * static_cast<std::size_t>(entrySize));
                if (offset + sizeof(TEntry) > responseBuffer.size())
                {
                    break;
                }

                TEntry row{};
                std::memcpy(&row, responseBuffer.data() + offset, sizeof(TEntry));
                rows.push_back(row);
            }
            return rows;
        }

        // queryFixedAudit 作用：
        // - 输入：DriverClient、IOCTL、可选输入、固定响应和操作名；
        // - 处理：调用统一 deviceIoControl 并验证固定响应字节数；
        // - 返回：IoResult，固定响应由调用方传入的 responseOut 承载。
        template <typename TRequest, typename TResponse>
        IoResult queryFixedAudit(
            const DriverClient& client,
            const unsigned long ioctlCode,
            TRequest* const request,
            TResponse& responseOut,
            const char* const operationName)
        {
            IoResult io = client.deviceIoControl(
                ioctlCode,
                request,
                request != nullptr ? static_cast<unsigned long>(sizeof(TRequest)) : 0UL,
                &responseOut,
                static_cast<unsigned long>(sizeof(TResponse)));
            if (!io.ok)
            {
                io.message = std::string("DeviceIoControl(") + operationName + ") failed, error=" + std::to_string(io.win32Error);
                return io;
            }
            if (io.bytesReturned < sizeof(TResponse))
            {
                io.ok = false;
                io.win32Error = ERROR_INSUFFICIENT_BUFFER;
                io.message = std::string(operationName) + " response too small, bytesReturned=" + std::to_string(io.bytesReturned);
            }
            return io;
        }

        // queryNoInputFixedAudit 作用：
        // - 输入：DriverClient、IOCTL、固定响应和操作名；
        // - 处理：无输入缓冲调用固定响应 IOCTL；
        // - 返回：IoResult，供 Hyper-V/AppControl 等无输入查询复用。
        template <typename TResponse>
        IoResult queryNoInputFixedAudit(
            const DriverClient& client,
            const unsigned long ioctlCode,
            TResponse& responseOut,
            const char* const operationName)
        {
            IoResult io = client.deviceIoControl(
                ioctlCode,
                nullptr,
                0UL,
                &responseOut,
                static_cast<unsigned long>(sizeof(TResponse)));
            if (!io.ok)
            {
                io.message = std::string("DeviceIoControl(") + operationName + ") failed, error=" + std::to_string(io.win32Error);
                return io;
            }
            if (io.bytesReturned < sizeof(TResponse))
            {
                io.ok = false;
                io.win32Error = ERROR_INSUFFICIENT_BUFFER;
                io.message = std::string(operationName) + " response too small, bytesReturned=" + std::to_string(io.bytesReturned);
            }
            return io;
        }

        // buildNetworkRequest 作用：
        // - 输入：网络审计 flags 和行预算；
        // - 处理：填充共享协议版本、结构大小和保守预算；
        // - 返回：KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST。
        KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST buildNetworkRequest(const unsigned long flags, const unsigned long maxRows)
        {
            KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST request{};
            request.version = KSWORD_ARK_NETWORK_PROTOCOL_VERSION;
            request.size = sizeof(request);
            request.flags = flags;
            request.maxRows = maxRows;
            return request;
        }

        // buildStorageRequest 作用：
        // - 输入：卷路径、flags、行预算和栈深度预算；
        // - 处理：填充共享 Storage 请求并安全复制可选卷路径；
        // - 返回：KSWORD_ARK_STORAGE_AUDIT_REQUEST。
        KSWORD_ARK_STORAGE_AUDIT_REQUEST buildStorageRequest(
            const std::wstring& volumePath,
            const unsigned long flags,
            const unsigned long maxRows,
            const unsigned long maxDepth)
        {
            KSWORD_ARK_STORAGE_AUDIT_REQUEST request{};
            request.version = KSWORD_ARK_STORAGE_PROTOCOL_VERSION;
            request.size = sizeof(request);
            request.flags = flags;
            request.maxRows = maxRows;
            request.maxDepth = maxDepth;
            if (!volumePath.empty())
            {
                copyAuditWideToFixed(request.volumePath, KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS, volumePath);
                request.volumePathLengthChars = static_cast<unsigned short>(std::min<std::size_t>(volumePath.size(), KSWORD_ARK_STORAGE_VOLUME_PATH_CHARS - 1U));
            }
            return request;
        }

        // buildWin32kRequest 作用：
        // - 输入：flags/session/pid/tid/maxEntries；
        // - 处理：归一化 Win32K 共享查询请求；
        // - 返回：KSWORD_ARK_WIN32K_QUERY_REQUEST。
        KSWORD_ARK_WIN32K_QUERY_REQUEST buildWin32kRequest(
            const unsigned long flags,
            const unsigned long sessionId,
            const unsigned long processId,
            const unsigned long threadId,
            const unsigned long maxEntries)
        {
            KSWORD_ARK_WIN32K_QUERY_REQUEST request{};
            request.version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
            request.flags = flags;
            request.sessionId = sessionId;
            request.processId = processId;
            request.threadId = threadId;
            request.maxEntries = maxEntries;
            return request;
        }


        // queryNetworkEndpointAudit 作用：
        // - 输入：TCP/UDP IOCTL、flags、行预算和操作名；
        // - 处理：发送网络 endpoint 只读查询并解析变长响应；
        // - 返回：NetworkEndpointAuditResult。
        NetworkEndpointAuditResult queryNetworkEndpointAudit(
            const DriverClient& client,
            const unsigned long ioctlCode,
            const unsigned long flags,
            const unsigned long maxRows,
            const char* const operationName)
        {
            NetworkEndpointAuditResult result{};
            KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST request = buildNetworkRequest(flags, maxRows);
            std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
            result.io = client.deviceIoControl(ioctlCode, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
            if (!result.io.ok)
            {
                markUnsupportedIfNeeded(result, operationName);
                return result;
            }

            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW);
            const auto* response = reinterpret_cast<const KSWORD_ARK_NETWORK_ENDPOINT_RESPONSE*>(responseBuffer.data());
            const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_NETWORK_ENDPOINT_ROW), response->returnedRowCount, operationName);
            if (!result.io.ok)
            {
                return result;
            }

            result.version = response->version;
            result.status = response->status;
            result.flags = response->flags;
            result.totalCount = response->totalRowCount;
            result.returnedCount = response->returnedRowCount;
            result.entrySize = response->entrySize;
            result.sourceFlags = response->sourceFlags;
            result.budgetRows = response->budgetRows;
            result.generation = response->generation;
            result.lastStatus = response->lastStatus;
            result.io.ntStatus = response->lastStatus;
            result.entries = parseVariableRows<KSWORD_ARK_NETWORK_ENDPOINT_ROW>(responseBuffer, headerSize, response->entrySize, parsedCount);
            result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
            return result;
        }

        // queryNetworkWfpAudit 作用：发送 WFP inventory IOCTL 并解析 owner/module 行。
        NetworkWfpInventoryResult queryNetworkWfpAudit(
            const DriverClient& client,
            const unsigned long flags,
            const unsigned long maxRows)
        {
            constexpr const char* operationName = "IOCTL_KSWORD_ARK_NETWORK_QUERY_WFP_INVENTORY";
            NetworkWfpInventoryResult result{};
            KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST request = buildNetworkRequest(flags, maxRows);
            std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
            result.io = client.deviceIoControl(IOCTL_KSWORD_ARK_NETWORK_QUERY_WFP_INVENTORY, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
            if (!result.io.ok)
            {
                markUnsupportedIfNeeded(result, operationName);
                return result;
            }

            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW);
            const auto* response = reinterpret_cast<const KSWORD_ARK_NETWORK_WFP_INVENTORY_RESPONSE*>(responseBuffer.data());
            const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW), response->returnedRowCount, operationName);
            if (!result.io.ok)
            {
                return result;
            }

            result.version = response->version;
            result.status = response->status;
            result.flags = response->flags;
            result.totalCount = response->totalRowCount;
            result.returnedCount = response->returnedRowCount;
            result.entrySize = response->entrySize;
            result.sourceFlags = response->sourceFlags;
            result.budgetRows = response->budgetRows;
            result.generation = response->generation;
            result.lastStatus = response->lastStatus;
            result.io.ntStatus = response->lastStatus;
            result.entries = parseVariableRows<KSWORD_ARK_NETWORK_WFP_INVENTORY_ROW>(responseBuffer, headerSize, response->entrySize, parsedCount);
            result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
            return result;
        }

        // queryNetworkNdisAudit 作用：发送 NDIS chain IOCTL 并解析链路行。
        NetworkNdisChainResult queryNetworkNdisAudit(
            const DriverClient& client,
            const unsigned long flags,
            const unsigned long maxRows)
        {
            constexpr const char* operationName = "IOCTL_KSWORD_ARK_NETWORK_QUERY_NDIS_CHAIN";
            NetworkNdisChainResult result{};
            KSWORD_ARK_NETWORK_AUDIT_QUERY_REQUEST request = buildNetworkRequest(flags, maxRows);
            std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
            result.io = client.deviceIoControl(IOCTL_KSWORD_ARK_NETWORK_QUERY_NDIS_CHAIN, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
            if (!result.io.ok)
            {
                markUnsupportedIfNeeded(result, operationName);
                return result;
            }

            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE) - sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW);
            const auto* response = reinterpret_cast<const KSWORD_ARK_NETWORK_NDIS_CHAIN_RESPONSE*>(responseBuffer.data());
            const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW), response->returnedRowCount, operationName);
            if (!result.io.ok)
            {
                return result;
            }

            result.version = response->version;
            result.status = response->status;
            result.flags = response->flags;
            result.totalCount = response->totalRowCount;
            result.returnedCount = response->returnedRowCount;
            result.entrySize = response->entrySize;
            result.sourceFlags = response->sourceFlags;
            result.budgetRows = response->budgetRows;
            result.generation = response->generation;
            result.lastStatus = response->lastStatus;
            result.io.ntStatus = response->lastStatus;
            result.entries = parseVariableRows<KSWORD_ARK_NETWORK_NDIS_CHAIN_ROW>(responseBuffer, headerSize, response->entrySize, parsedCount);
            result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
            return result;
        }

        // queryStorageRows 作用：
        // - 输入：任意 Storage 变长响应/行类型与 IOCTL；
        // - 处理：发送请求、验证 rowSize、解析 rows；
        // - 返回：具体 Storage 结果类型。
        template <typename TResult, typename TResponse, typename TRow>
        TResult queryStorageRows(
            const DriverClient& client,
            const unsigned long ioctlCode,
            const KSWORD_ARK_STORAGE_AUDIT_REQUEST& request,
            const char* const operationName)
        {
            TResult result{};
            std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
            result.io = client.deviceIoControl(ioctlCode, const_cast<KSWORD_ARK_STORAGE_AUDIT_REQUEST*>(&request), sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
            if (!result.io.ok)
            {
                markUnsupportedIfNeeded(result, operationName);
                return result;
            }

            constexpr std::size_t headerSize = sizeof(TResponse) - sizeof(TRow);
            const auto* response = reinterpret_cast<const TResponse*>(responseBuffer.data());
            const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->rowSize, sizeof(TRow), response->returnedRows, operationName);
            if (!result.io.ok)
            {
                return result;
            }

            result.version = response->version;
            result.status = response->queryStatus;
            result.entrySize = response->rowSize;
            result.responseFlags = response->responseFlags;
            result.fieldFlags = response->fieldFlags;
            result.totalCount = response->totalRows;
            result.returnedCount = response->returnedRows;
            result.maxRows = response->maxRows;
            result.lastStatus = response->lastStatus;
            result.io.ntStatus = response->lastStatus;
            if constexpr (std::is_same_v<TResult, StorageVolumeStackAuditResult>)
            {
                result.fvevolPresent = response->fvevolPresent;
                result.fvevolPosition = response->fvevolPosition;
            }
            result.rows = parseVariableRows<TRow>(responseBuffer, headerSize, response->rowSize, parsedCount);
            result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.rows.size(), result.io.bytesReturned);
            return result;
        }
    }

    NetworkEndpointAuditResult DriverClient::queryNetworkTcpEndpoints(const unsigned long flags, const unsigned long maxRows) const
    {
        return queryNetworkEndpointAudit(*this, IOCTL_KSWORD_ARK_NETWORK_QUERY_TCP_ENDPOINTS, flags, maxRows, "IOCTL_KSWORD_ARK_NETWORK_QUERY_TCP_ENDPOINTS");
    }

    NetworkEndpointAuditResult DriverClient::queryNetworkUdpEndpoints(const unsigned long flags, const unsigned long maxRows) const
    {
        return queryNetworkEndpointAudit(*this, IOCTL_KSWORD_ARK_NETWORK_QUERY_UDP_ENDPOINTS, flags, maxRows, "IOCTL_KSWORD_ARK_NETWORK_QUERY_UDP_ENDPOINTS");
    }

    NetworkWfpInventoryResult DriverClient::queryNetworkWfpInventory(const unsigned long flags, const unsigned long maxRows) const
    {
        return queryNetworkWfpAudit(*this, flags, maxRows);
    }

    NetworkNdisChainResult DriverClient::queryNetworkNdisChain(const unsigned long flags, const unsigned long maxRows) const
    {
        return queryNetworkNdisAudit(*this, flags, maxRows);
    }

    MinifilterInventoryResult DriverClient::queryMinifilterInventory(const unsigned long flags, const unsigned long maxRows) const
    {
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_MINIFILTER_INVENTORY";
        MinifilterInventoryResult result{};
        KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_FILTER_PROTOCOL_VERSION;
        request.flags = flags;
        request.maxRows = maxRows;
        std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
        result.io = deviceIoControl(IOCTL_KSWORD_ARK_QUERY_MINIFILTER_INVENTORY, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_RESPONSE) - sizeof(KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_MINIFILTER_INVENTORY_RESPONSE*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY), response->returnedCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.status = response->queryStatus;
        result.totalCount = response->totalCount;
        result.returnedCount = response->returnedCount;
        result.entrySize = response->entrySize;
        result.responseFlags = response->flags;
        result.lastStatus = response->lastStatus;
        result.io.ntStatus = response->lastStatus;
        result.entries = parseVariableRows<KSWORD_ARK_MINIFILTER_INVENTORY_ENTRY>(responseBuffer, headerSize, response->entrySize, parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    StorageVolumeStackAuditResult DriverClient::queryVolumeStackAudit(const std::wstring& volumePath, const unsigned long flags, const unsigned long maxRows, const unsigned long maxDepth) const
    {
        const KSWORD_ARK_STORAGE_AUDIT_REQUEST request = buildStorageRequest(volumePath, flags, maxRows, maxDepth);
        return queryStorageRows<StorageVolumeStackAuditResult, KSWORD_ARK_QUERY_VOLUME_STACK_RESPONSE, KSWORD_ARK_VOLUME_STACK_ROW>(*this, IOCTL_KSWORD_ARK_QUERY_VOLUME_STACK_AUDIT, request, "IOCTL_KSWORD_ARK_QUERY_VOLUME_STACK_AUDIT");
    }

    StorageBitlockerFveAuditResult DriverClient::queryBitlockerFveAudit(const std::wstring& volumePath, const unsigned long flags, const unsigned long maxRows, const unsigned long maxDepth) const
    {
        const KSWORD_ARK_STORAGE_AUDIT_REQUEST request = buildStorageRequest(volumePath, flags, maxRows, maxDepth);
        return queryStorageRows<StorageBitlockerFveAuditResult, KSWORD_ARK_QUERY_BITLOCKER_FVE_RESPONSE, KSWORD_ARK_BITLOCKER_FVE_ROW>(*this, IOCTL_KSWORD_ARK_QUERY_BITLOCKER_FVE_AUDIT, request, "IOCTL_KSWORD_ARK_QUERY_BITLOCKER_FVE_AUDIT");
    }

    StorageMountMgrMappingAuditResult DriverClient::queryMountMgrMappingAudit(const std::wstring& volumePath, const unsigned long flags, const unsigned long maxRows, const unsigned long maxDepth) const
    {
        const KSWORD_ARK_STORAGE_AUDIT_REQUEST request = buildStorageRequest(volumePath, flags, maxRows, maxDepth);
        return queryStorageRows<StorageMountMgrMappingAuditResult, KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_RESPONSE, KSWORD_ARK_MOUNTMGR_MAPPING_ROW>(*this, IOCTL_KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_AUDIT, request, "IOCTL_KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_AUDIT");
    }

    StorageFilesystemIntegrityAuditResult DriverClient::queryFilesystemIntegrityAudit(const std::wstring& volumePath, const unsigned long flags, const unsigned long maxRows, const unsigned long maxDepth) const
    {
        const KSWORD_ARK_STORAGE_AUDIT_REQUEST request = buildStorageRequest(volumePath, flags, maxRows, maxDepth);
        return queryStorageRows<StorageFilesystemIntegrityAuditResult, KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_RESPONSE, KSWORD_ARK_FILESYSTEM_INTEGRITY_ROW>(*this, IOCTL_KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_AUDIT, request, "IOCTL_KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_AUDIT");
    }

    SecurityStatusAuditResult DriverClient::querySecurityStatus(const unsigned long flags) const
    {
        SecurityStatusAuditResult result{};
        KSWORD_ARK_QUERY_SECURITY_STATUS_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION;
        request.flags = flags;
        result.io = queryFixedAudit(*this, IOCTL_KSWORD_ARK_QUERY_SECURITY_STATUS, &request, result.response, "IOCTL_KSWORD_ARK_QUERY_SECURITY_STATUS");
        markUnsupportedIfNeeded(result, "IOCTL_KSWORD_ARK_QUERY_SECURITY_STATUS");
        result.io.ntStatus = result.response.queryStatus;
        return result;
    }


    DriverTrustViewAuditResult DriverClient::queryDriverTrustView(const unsigned long flags, const unsigned long maxEntries) const
    {
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW";
        DriverTrustViewAuditResult result{};
        KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION;
        request.flags = flags;
        request.maxEntries = maxEntries;
        std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
        result.io = deviceIoControl(IOCTL_KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE) - sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY), sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY), response->entryCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.status = static_cast<std::uint32_t>(response->queryStatus);
        result.fieldFlags = response->fieldFlags;
        result.sourceMask = response->sourceMask;
        result.totalCount = response->totalModuleCount;
        result.returnedCount = response->entryCount;
        result.entrySize = sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY);
        result.maxEntriesAccepted = response->maxEntriesAccepted;
        result.truncated = response->truncated;
        result.moduleQueryStatus = response->moduleQueryStatus;
        result.signingResolverStatus = response->signingResolverStatus;
        result.lastStatus = response->queryStatus;
        result.io.ntStatus = response->queryStatus;
        result.entries = parseVariableRows<KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY>(responseBuffer, headerSize, sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY), parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    HyperVSummaryAuditResult DriverClient::queryHyperVSummary() const
    {
        HyperVSummaryAuditResult result{};
        result.io = queryNoInputFixedAudit(*this, IOCTL_KSWORD_ARK_QUERY_HYPERV_SUMMARY, result.response, "IOCTL_KSWORD_ARK_QUERY_HYPERV_SUMMARY");
        markUnsupportedIfNeeded(result, "IOCTL_KSWORD_ARK_QUERY_HYPERV_SUMMARY");
        result.io.ntStatus = result.response.queryStatus;
        return result;
    }

    AppControlStatusAuditResult DriverClient::queryAppControlStatus() const
    {
        AppControlStatusAuditResult result{};
        result.io = queryNoInputFixedAudit(*this, IOCTL_KSWORD_ARK_QUERY_APP_CONTROL_STATUS, result.response, "IOCTL_KSWORD_ARK_QUERY_APP_CONTROL_STATUS");
        markUnsupportedIfNeeded(result, "IOCTL_KSWORD_ARK_QUERY_APP_CONTROL_STATUS");
        result.io.ntStatus = result.response.queryStatus;
        return result;
    }

    Win32kProfileStatusResult DriverClient::queryWin32kProfileStatus(const unsigned long flags, const unsigned long sessionId, const unsigned long maxEntries) const
    {
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_WIN32K_PROFILE_STATUS";
        Win32kProfileStatusResult result{};
        KSWORD_ARK_WIN32K_QUERY_REQUEST request = buildWin32kRequest(flags, sessionId, 0UL, 0UL, maxEntries);
        std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
        result.io = deviceIoControl(IOCTL_KSWORD_ARK_QUERY_WIN32K_PROFILE_STATUS, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY), response->returnedCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.status = response->status;
        result.totalCount = response->totalCount;
        result.returnedCount = response->returnedCount;
        result.entrySize = response->entrySize;
        result.flags = response->flags;
        result.lastStatus = response->lastStatus;
        result.capabilityMask = response->capabilityMask;
        result.missingCapabilityMask = response->missingCapabilityMask;
        result.userGetSiloGlobals = response->userGetSiloGlobals;
        result.win32k = response->win32k;
        result.win32kbase = response->win32kbase;
        result.win32kfull = response->win32kfull;
        result.fieldOffsets = response->fieldOffsets;
        result.io.ntStatus = response->lastStatus;
        result.entries = parseVariableRows<KSWORD_ARK_WIN32K_SESSION_ENTRY>(responseBuffer, headerSize, response->entrySize, parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    // queryWin32kRows 作用：
    // - 输入：Win32K 变长响应/行类型、IOCTL 和过滤参数；
    // - 处理：发送请求并解析 capability/offset/entries；
    // - 返回：具体 Win32K 结果类型。
    template <typename TResult, typename TResponse, typename TEntry>
    TResult queryWin32kRows(
        const DriverClient& client,
        const unsigned long ioctlCode,
        const KSWORD_ARK_WIN32K_QUERY_REQUEST& request,
        const char* const operationName)
    {
        TResult result{};
        std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
        result.io = client.deviceIoControl(ioctlCode, const_cast<KSWORD_ARK_WIN32K_QUERY_REQUEST*>(&request), sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(TResponse) - sizeof(TEntry);
        const auto* response = reinterpret_cast<const TResponse*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(TEntry), response->returnedCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.status = response->status;
        result.totalCount = response->totalCount;
        result.returnedCount = response->returnedCount;
        result.entrySize = response->entrySize;
        result.flags = response->flags;
        result.lastStatus = response->lastStatus;
        result.capabilityMask = response->capabilityMask;
        result.missingCapabilityMask = response->missingCapabilityMask;
        result.fieldOffsets = response->fieldOffsets;
        result.io.ntStatus = response->lastStatus;
        result.entries = parseVariableRows<TEntry>(responseBuffer, headerSize, response->entrySize, parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    Win32kWindowsResult DriverClient::queryWin32kWindows(const unsigned long flags, const unsigned long sessionId, const unsigned long processId, const unsigned long threadId, const unsigned long maxEntries) const
    {
        const KSWORD_ARK_WIN32K_QUERY_REQUEST request = buildWin32kRequest(flags, sessionId, processId, threadId, maxEntries);
        return queryWin32kRows<Win32kWindowsResult, KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE, KSWORD_ARK_WIN32K_WINDOW_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOWS, request, "IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOWS");
    }

    Win32kGuiThreadsResult DriverClient::queryWin32kGuiThreads(const unsigned long flags, const unsigned long sessionId, const unsigned long processId, const unsigned long threadId, const unsigned long maxEntries) const
    {
        const KSWORD_ARK_WIN32K_QUERY_REQUEST request = buildWin32kRequest(flags, sessionId, processId, threadId, maxEntries);
        return queryWin32kRows<Win32kGuiThreadsResult, KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE, KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_WIN32K_GUI_THREADS, request, "IOCTL_KSWORD_ARK_QUERY_WIN32K_GUI_THREADS");
    }

    Win32kHotkeysPdbResult DriverClient::queryWin32kHotkeysPdb(const unsigned long flags, const unsigned long sessionId, const unsigned long processId, const unsigned long threadId, const unsigned long maxEntries) const
    {
        const KSWORD_ARK_WIN32K_QUERY_REQUEST request = buildWin32kRequest(flags, sessionId, processId, threadId, maxEntries);
        return queryWin32kRows<Win32kHotkeysPdbResult, KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE, KSWORD_ARK_WIN32K_HOTKEY_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_WIN32K_HOTKEYS_PDB, request, "IOCTL_KSWORD_ARK_QUERY_WIN32K_HOTKEYS_PDB");
    }

    Win32kHooksPdbResult DriverClient::queryWin32kHooksPdb(const unsigned long flags, const unsigned long sessionId, const unsigned long processId, const unsigned long threadId, const unsigned long maxEntries) const
    {
        const KSWORD_ARK_WIN32K_QUERY_REQUEST request = buildWin32kRequest(flags, sessionId, processId, threadId, maxEntries);
        return queryWin32kRows<Win32kHooksPdbResult, KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE, KSWORD_ARK_WIN32K_HOOK_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_WIN32K_HOOKS_PDB, request, "IOCTL_KSWORD_ARK_QUERY_WIN32K_HOOKS_PDB");
    }

    Win32kWindowRuntimeDetailResult DriverClient::queryWin32kWindowDetail(
        const std::uint64_t hwnd,
        const unsigned long processId,
        const unsigned long threadId,
        const unsigned long flags) const
    {
        // 输入：HWND 以及可选 PID/TID 约束，flags 控制是否返回诊断文本。
        // 处理：调用 win32k 单窗口详情 IOCTL，R0 当前只做 profile/capability readiness。
        // 返回：固定响应；unsupported 表示旧驱动缺入口或 R0 明确未实现 tagWND 读取。
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOW_DETAIL";
        Win32kWindowRuntimeDetailResult result{};
        KSWORD_ARK_WIN32K_WINDOW_DETAIL_REQUEST request{};
        request.version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
        request.flags = flags;
        request.processId = processId;
        request.threadId = threadId;
        request.hwnd = hwnd;

        result.io = queryFixedAudit(
            *this,
            IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOW_DETAIL,
            &request,
            result.response,
            operationName);
        markUnsupportedIfNeeded(result, operationName);
        result.io.ntStatus = result.response.lastStatus;
        if (result.io.ok)
        {
            result.unsupported = result.response.status == KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED ||
                static_cast<unsigned long>(result.response.lastStatus) == 0xC00000BBUL ||
                static_cast<unsigned long>(result.response.lastStatus) == 0xC0000010UL;
            std::ostringstream stream;
            stream << operationName
                << " status=" << result.response.status
                << ", fields=0x" << std::hex << std::uppercase << result.response.fieldFlags
                << ", missingCaps=0x" << result.response.missingCapabilityMask
                << ", lastStatus=0x" << static_cast<unsigned long>(result.response.lastStatus)
                << std::dec << ", bytesReturned=" << result.io.bytesReturned;
            result.io.message = stream.str();
        }
        return result;
    }


    // queryDeviceAuditRows 作用：封装 Device/Input/USB/GPU 四类统一设备审计 IOCTL。
    DeviceAuditResult queryDeviceAuditRows(
        const DriverClient& client,
        const unsigned long ioctlCode,
        const unsigned long profileFlags,
        const std::wstring& targetName,
        const unsigned long maxRows,
        const unsigned long maxAttachedDepth,
        const char* const operationName)
    {
        DeviceAuditResult result{};
        KSWORD_ARK_QUERY_DEVICE_AUDIT_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_DEVICE_AUDIT_PROTOCOL_VERSION;
        request.profileFlags = profileFlags;
        request.maxRows = maxRows;
        request.maxAttachedDepth = maxAttachedDepth;
        copyAuditWideToFixed(request.targetName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS, targetName);
        std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
        result.io = client.deviceIoControl(ioctlCode, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE) - sizeof(KSWORD_ARK_DEVICE_AUDIT_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_DEVICE_AUDIT_RESPONSE*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_DEVICE_AUDIT_ENTRY), response->returnedCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.status = response->queryStatus;
        result.profileFlags = response->profileFlags;
        result.responseFlags = response->responseFlags;
        result.totalCount = response->totalCount;
        result.returnedCount = response->returnedCount;
        result.entrySize = response->entrySize;
        result.targetCount = response->targetCount;
        result.driverCount = response->driverCount;
        result.deviceCount = response->deviceCount;
        result.lastStatus = response->lastStatus;
        result.io.ntStatus = response->lastStatus;
        result.entries = parseVariableRows<KSWORD_ARK_DEVICE_AUDIT_ENTRY>(responseBuffer, headerSize, response->entrySize, parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    DeviceAuditResult DriverClient::queryDeviceStackAudit(const std::wstring& targetName, const unsigned long maxRows, const unsigned long maxAttachedDepth) const
    {
        return queryDeviceAuditRows(*this, IOCTL_KSWORD_ARK_QUERY_DEVICE_STACK_AUDIT, KSWORD_ARK_DEVICE_AUDIT_PROFILE_DEVICE_STACK, targetName, maxRows, maxAttachedDepth, "IOCTL_KSWORD_ARK_QUERY_DEVICE_STACK_AUDIT");
    }

    DeviceAuditResult DriverClient::queryInputStackAudit(const std::wstring& targetName, const unsigned long maxRows, const unsigned long maxAttachedDepth) const
    {
        return queryDeviceAuditRows(*this, IOCTL_KSWORD_ARK_QUERY_INPUT_STACK_AUDIT, KSWORD_ARK_DEVICE_AUDIT_PROFILE_INPUT_STACK, targetName, maxRows, maxAttachedDepth, "IOCTL_KSWORD_ARK_QUERY_INPUT_STACK_AUDIT");
    }

    DeviceAuditResult DriverClient::queryUsbTopologyAudit(const std::wstring& targetName, const unsigned long maxRows, const unsigned long maxAttachedDepth) const
    {
        return queryDeviceAuditRows(*this, IOCTL_KSWORD_ARK_QUERY_USB_TOPOLOGY_AUDIT, KSWORD_ARK_DEVICE_AUDIT_PROFILE_USB_TOPOLOGY, targetName, maxRows, maxAttachedDepth, "IOCTL_KSWORD_ARK_QUERY_USB_TOPOLOGY_AUDIT");
    }

    DeviceAuditResult DriverClient::queryGpuDisplayWatchdogAudit(const std::wstring& targetName, const unsigned long maxRows, const unsigned long maxAttachedDepth) const
    {
        return queryDeviceAuditRows(*this, IOCTL_KSWORD_ARK_QUERY_GPU_DISPLAY_WATCHDOG_AUDIT, KSWORD_ARK_DEVICE_AUDIT_PROFILE_GPU_DISPLAY_WATCHDOG, targetName, maxRows, maxAttachedDepth, "IOCTL_KSWORD_ARK_QUERY_GPU_DISPLAY_WATCHDOG_AUDIT");
    }

    CidTableAuditResult DriverClient::enumCidTable(const unsigned long flags, const unsigned long maxEntries, const unsigned long maxVisitCount, const unsigned long startCid, const unsigned long endCid) const
    {
        constexpr const char* operationName = "IOCTL_KSWORD_ARK_ENUM_CID_TABLE";
        CidTableAuditResult result{};
        KSWORD_ARK_ENUM_CID_TABLE_REQUEST request{};
        request.version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
        request.flags = flags;
        request.maxEntries = maxEntries;
        request.maxVisitCount = maxVisitCount;
        request.startCid = startCid;
        request.endCid = endCid;
        std::vector<std::uint8_t> responseBuffer(kDefaultAuditBufferBytes, 0U);
        result.io = deviceIoControl(IOCTL_KSWORD_ARK_ENUM_CID_TABLE, &request, sizeof(request), responseBuffer.data(), static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_CID_TABLE_RESPONSE) - sizeof(KSWORD_ARK_CID_TABLE_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_CID_TABLE_RESPONSE*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(KSWORD_ARK_CID_TABLE_ENTRY), response->returnedCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.status = response->status;
        result.totalCount = response->totalCount;
        result.returnedCount = response->returnedCount;
        result.entrySize = response->entrySize;
        result.flags = response->flags;
        result.visitedCount = response->visitedCount;
        result.maxVisitCount = response->maxVisitCount;
        result.lastStatus = response->lastStatus;
        result.pspCidTableAddress = response->pspCidTableAddress;
        result.dynDataCapabilityMask = response->dynDataCapabilityMask;
        result.htTableCodeOffset = response->htTableCodeOffset;
        result.hteLowValueOffset = response->hteLowValueOffset;
        result.io.ntStatus = response->lastStatus;
        result.entries = parseVariableRows<KSWORD_ARK_CID_TABLE_ENTRY>(responseBuffer, headerSize, response->entrySize, parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    KernelObjectSummaryAuditResult DriverClient::queryKernelObjectSummary(const unsigned long targetKind, const unsigned long cidValue, const std::uint64_t expectedObjectAddress, const unsigned long flags) const
    {
        KernelObjectSummaryAuditResult result{};
        KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY_REQUEST request{};
        request.version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
        request.flags = flags;
        request.targetKind = targetKind;
        request.cidValue = cidValue;
        request.expectedObjectAddress = expectedObjectAddress;
        result.io = queryFixedAudit(*this, IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY, &request, result.response, "IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY");
        markUnsupportedIfNeeded(result, "IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY");
        result.io.ntStatus = result.response.lookupStatus;
        return result;
    }

    IpcSummaryAuditResult DriverClient::queryIpcSummary(const unsigned long processId, const std::uint64_t handleValue, const unsigned long flags, const unsigned long maxEntries) const
    {
        IpcSummaryAuditResult result{};
        KSWORD_ARK_QUERY_IPC_SUMMARY_REQUEST request{};
        request.version = KSWORD_ARK_KERNEL_OBJECT_PROTOCOL_VERSION;
        request.flags = flags;
        request.processId = processId;
        request.handleValue = handleValue;
        request.maxEntries = maxEntries;
        result.io = queryFixedAudit(*this, IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY, &request, result.response, "IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY");
        markUnsupportedIfNeeded(result, "IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY");
        result.io.ntStatus = result.response.lastStatus;
        return result;
    }

    DynDataV4ApplyResult DriverClient::applyDynDataProfileV4(const DynDataV4ApplyInput& profile) const
    {
        DynDataV4ApplyResult result{};
        if (profile.items.empty() || profile.items.size() > KSW_DYN_V4_MAX_ITEMS_PER_MODULE || profile.capabilityGroups.size() > KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE)
        {
            result.io.ok = false;
            result.io.win32Error = ERROR_INVALID_PARAMETER;
            result.io.message = "DynData v4 profile item/group count invalid.";
            return result;
        }

        const std::size_t requestBytes = KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE + (profile.items.size() * sizeof(KSW_DYN_V4_ITEM_PACKET));
        if (requestBytes > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max()))
        {
            result.io.ok = false;
            result.io.win32Error = ERROR_INVALID_PARAMETER;
            result.io.message = "DynData v4 profile request too large.";
            return result;
        }

        std::vector<std::uint8_t> requestBuffer(requestBytes, 0U);
        auto* request = reinterpret_cast<KSW_APPLY_DYN_PROFILE_V4_REQUEST*>(requestBuffer.data());
        request->size = static_cast<unsigned long>(requestBytes);
        request->version = KSW_DYN_V4_PROTOCOL_VERSION;
        request->flags = profile.flags;
        request->itemCount = static_cast<unsigned long>(profile.items.size());
        request->capabilityGroupCount = static_cast<unsigned long>(profile.capabilityGroups.size());
        request->module = profile.module;
        for (std::size_t index = 0U; index < profile.capabilityGroups.size(); ++index)
        {
            request->capabilityGroups[index] = profile.capabilityGroups[index];
        }
        for (std::size_t index = 0U; index < profile.items.size(); ++index)
        {
            request->items[index] = profile.items[index];
        }

        result.io = deviceIoControl(IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_V4, requestBuffer.data(), static_cast<unsigned long>(requestBuffer.size()), &result.response, sizeof(result.response));
        markUnsupportedIfNeeded(result, "IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_V4");
        if (result.io.ok && result.io.bytesReturned < sizeof(KSW_APPLY_DYN_PROFILE_V4_RESPONSE))
        {
            result.io.ok = false;
            result.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            result.io.message = "DynData v4 apply response too small, bytesReturned=" + std::to_string(result.io.bytesReturned);
        }
        result.io.ntStatus = result.response.status;
        return result;
    }

    template <typename TResult, typename TResponse, typename TEntry>
    TResult queryDynDataV4Rows(const DriverClient& client, const unsigned long ioctlCode, const unsigned long maxRows, const char* const operationName)
    {
        TResult result{};
        std::vector<std::uint8_t> responseBuffer(std::max<std::size_t>(64U * 1024U, sizeof(TResponse) + (static_cast<std::size_t>(maxRows) * sizeof(TEntry))), 0U);
        result.io = client.deviceIoControl(ioctlCode, nullptr, 0UL, responseBuffer.data(), static_cast<unsigned long>(std::min<std::size_t>(responseBuffer.size(), kDefaultAuditBufferBytes)));
        if (!result.io.ok)
        {
            markUnsupportedIfNeeded(result, operationName);
            return result;
        }

        constexpr std::size_t headerSize = sizeof(TResponse) - sizeof(TEntry);
        const auto* response = reinterpret_cast<const TResponse*>(responseBuffer.data());
        const std::size_t parsedCount = validateAuditRows(result.io, headerSize, response->entrySize, sizeof(TEntry), response->returnedCount, operationName);
        if (!result.io.ok)
        {
            return result;
        }

        result.version = response->version;
        result.totalCount = response->totalCount;
        result.returnedCount = response->returnedCount;
        result.entrySize = response->entrySize;
        result.entries = parseVariableRows<TEntry>(responseBuffer, headerSize, response->entrySize, parsedCount);
        result.io.message = appendAuditSummary(operationName, result.totalCount, result.returnedCount, result.entries.size(), result.io.bytesReturned);
        return result;
    }

    DynDataV4ModulesResult DriverClient::queryDynDataV4Modules(const unsigned long maxRows) const
    {
        return queryDynDataV4Rows<DynDataV4ModulesResult, KSW_QUERY_DYN_V4_MODULES_RESPONSE, KSW_DYN_V4_MODULE_STATUS_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_DYN_V4_MODULES, maxRows, "IOCTL_KSWORD_ARK_QUERY_DYN_V4_MODULES");
    }

    DynDataV4CapabilityGroupsResult DriverClient::queryDynDataV4CapabilityGroups(const unsigned long maxRows) const
    {
        return queryDynDataV4Rows<DynDataV4CapabilityGroupsResult, KSW_QUERY_DYN_V4_CAPABILITY_GROUPS_RESPONSE, KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_DYN_V4_CAPABILITY_GROUPS, maxRows, "IOCTL_KSWORD_ARK_QUERY_DYN_V4_CAPABILITY_GROUPS");
    }

    DynDataV4MissingItemsResult DriverClient::queryDynDataV4MissingItems(const unsigned long maxRows) const
    {
        return queryDynDataV4Rows<DynDataV4MissingItemsResult, KSW_QUERY_DYN_V4_MISSING_ITEMS_RESPONSE, KSW_DYN_V4_MISSING_ITEM_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_DYN_V4_MISSING_ITEMS, maxRows, "IOCTL_KSWORD_ARK_QUERY_DYN_V4_MISSING_ITEMS");
    }

    DynDataV4ItemsResult DriverClient::queryDynDataV4Items(const unsigned long maxRows) const
    {
        return queryDynDataV4Rows<DynDataV4ItemsResult, KSW_QUERY_DYN_V4_ITEMS_RESPONSE, KSW_DYN_V4_ITEM_STATUS_ENTRY>(*this, IOCTL_KSWORD_ARK_QUERY_DYN_V4_ITEMS, maxRows, "IOCTL_KSWORD_ARK_QUERY_DYN_V4_ITEMS");
    }
}
