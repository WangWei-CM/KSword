#include "ArkDriverClient.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace ksword::ark
{
    namespace
    {
        std::wstring fixedWideToWString(const wchar_t* textBuffer, const std::size_t maxChars)
        {
            // textBuffer 用途：解析驱动固定宽字符响应缓冲。
            // maxChars 用途：限制扫描边界，防止旧驱动响应缺少 NUL 时越界。
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

    FileInfoQueryResult DriverClient::queryFileInfo(const std::wstring& ntPath, const unsigned long flags) const
    {
        // 作用：用独立控制句柄查询 R0 文件基础信息。
        // 返回：FileInfoQueryResult，失败时 io.message 包含 Win32/协议诊断。
        DriverHandle handle = open();
        return queryFileInfo(handle, ntPath, flags);
    }

    FileInfoQueryResult DriverClient::queryFileInfo(
        DriverHandle& handle,
        const std::wstring& ntPath,
        const unsigned long flags) const
    {
        // 作用：调用 IOCTL_KSWORD_ARK_QUERY_FILE_INFO。
        // 处理：只传 NT 路径和 flags；驱动返回 FileBasic/FileStandard 与对象诊断字段。
        // 返回：解析后的 FileInfoQueryResult。
        FileInfoQueryResult queryResult{};
        if (ntPath.empty() || ntPath.size() >= KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS)
        {
            queryResult.io.ok = false;
            queryResult.io.win32Error = ERROR_INVALID_PARAMETER;
            queryResult.io.message = "file-info path invalid, chars=" + std::to_string(ntPath.size());
            return queryResult;
        }

        KSWORD_ARK_QUERY_FILE_INFO_REQUEST request{};
        KSWORD_ARK_QUERY_FILE_INFO_RESPONSE response{};
        request.flags = flags;
        request.pathLengthChars = static_cast<unsigned short>(ntPath.size());
        std::copy(ntPath.begin(), ntPath.end(), request.path);
        request.path[request.pathLengthChars] = L'\0';

        queryResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_QUERY_FILE_INFO,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)),
            &handle);
        if (!queryResult.io.ok)
        {
            queryResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_QUERY_FILE_INFO) failed, error=" +
                std::to_string(queryResult.io.win32Error);
            return queryResult;
        }
        if (queryResult.io.bytesReturned < sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE))
        {
            queryResult.io.ok = false;
            queryResult.io.message =
                "query-file-info response too small, bytesReturned=" +
                std::to_string(queryResult.io.bytesReturned);
            return queryResult;
        }

        queryResult.version = static_cast<std::uint32_t>(response.version);
        queryResult.fieldFlags = static_cast<std::uint32_t>(response.fieldFlags);
        queryResult.queryStatus = static_cast<std::uint32_t>(response.queryStatus);
        queryResult.openStatus = static_cast<long>(response.openStatus);
        queryResult.basicStatus = static_cast<long>(response.basicStatus);
        queryResult.standardStatus = static_cast<long>(response.standardStatus);
        queryResult.objectStatus = static_cast<long>(response.objectStatus);
        queryResult.nameStatus = static_cast<long>(response.nameStatus);
        queryResult.fileAttributes = static_cast<std::uint32_t>(response.fileAttributes);
        queryResult.allocationSize = static_cast<std::int64_t>(response.allocationSize);
        queryResult.endOfFile = static_cast<std::int64_t>(response.endOfFile);
        queryResult.creationTime = static_cast<std::int64_t>(response.creationTime);
        queryResult.lastAccessTime = static_cast<std::int64_t>(response.lastAccessTime);
        queryResult.lastWriteTime = static_cast<std::int64_t>(response.lastWriteTime);
        queryResult.changeTime = static_cast<std::int64_t>(response.changeTime);
        queryResult.fileObjectAddress = static_cast<std::uint64_t>(response.fileObjectAddress);
        queryResult.sectionObjectPointersAddress = static_cast<std::uint64_t>(response.sectionObjectPointersAddress);
        queryResult.dataSectionObjectAddress = static_cast<std::uint64_t>(response.dataSectionObjectAddress);
        queryResult.imageSectionObjectAddress = static_cast<std::uint64_t>(response.imageSectionObjectAddress);
        queryResult.ntPath = fixedWideToWString(response.ntPath, KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS);
        queryResult.objectName = fixedWideToWString(response.objectName, KSWORD_ARK_FILE_INFO_OBJECT_NAME_MAX_CHARS);

        std::ostringstream stream;
        stream << "version=" << queryResult.version
            << ", status=" << queryResult.queryStatus
            << ", fields=0x" << std::hex << std::uppercase << queryResult.fieldFlags
            << ", openStatus=0x" << static_cast<unsigned long>(static_cast<std::uint32_t>(queryResult.openStatus))
            << ", basicStatus=0x" << static_cast<unsigned long>(static_cast<std::uint32_t>(queryResult.basicStatus))
            << ", standardStatus=0x" << static_cast<unsigned long>(static_cast<std::uint32_t>(queryResult.standardStatus))
            << std::dec << ", bytesReturned=" << queryResult.io.bytesReturned;
        queryResult.io.message = stream.str();
        return queryResult;
    }

    IoResult DriverClient::deletePath(const std::wstring& ntPath, const bool isDirectory) const
    {
        DriverHandle handle = open();
        return deletePath(handle, ntPath, isDirectory);
    }

    IoResult DriverClient::deletePath(DriverHandle& handle, const std::wstring& ntPath, const bool isDirectory) const
    {
        if (ntPath.empty() || ntPath.size() >= KSWORD_ARK_DELETE_PATH_MAX_CHARS)
        {
            IoResult result{};
            result.ok = false;
            result.win32Error = ERROR_INVALID_PARAMETER;
            result.message = "path too long for ioctl, chars=" + std::to_string(ntPath.size());
            return result;
        }

        KSWORD_ARK_DELETE_PATH_REQUEST request{};
        request.flags = isDirectory ? KSWORD_ARK_DELETE_PATH_FLAG_DIRECTORY : 0UL;
        request.pathLengthChars = static_cast<unsigned short>(ntPath.size());
        std::copy(ntPath.begin(), ntPath.end(), request.path);
        request.path[request.pathLengthChars] = L'\0';

        IoResult result = deviceIoControl(
            IOCTL_KSWORD_ARK_DELETE_PATH,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            nullptr,
            0,
            &handle);

        std::ostringstream stream;
        stream << "pathChars=" << ntPath.size()
            << ", directory=" << (isDirectory ? 1 : 0)
            << ", bytesReturned=" << result.bytesReturned;
        stream << (result.ok ? ", ioctl=ok" : ", ioctl=fail, error=" + std::to_string(result.win32Error));
        result.message = stream.str();
        return result;
    }

    IoResult DriverClient::controlFileMonitor(
        const unsigned long action,
        const unsigned long operationMask,
        const unsigned long processId,
        const unsigned long flags) const
    {
        // 作用：控制 R0 文件系统 minifilter 的 Start/Stop/Clear 状态。
        // 处理：构造共享协议控制包，只经 ArkDriverClient 统一下发 IOCTL。
        // 返回：IoResult，message 中补充 action/mask/pid 便于应用日志定位。
        KSWORD_ARK_FILE_MONITOR_CONTROL_REQUEST request{};
        request.action = action;
        request.operationMask = operationMask;
        request.processId = processId;
        request.flags = flags;

        IoResult result = deviceIoControl(
            IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            nullptr,
            0);

        std::ostringstream stream;
        stream << "file-monitor control action=" << action
            << ", mask=0x" << std::hex << std::uppercase << operationMask
            << std::dec << ", pid=" << processId
            << ", flags=0x" << std::hex << std::uppercase << flags
            << std::dec << ", bytesReturned=" << result.bytesReturned;
        stream << (result.ok ? ", ioctl=ok" : ", ioctl=fail, error=" + std::to_string(result.win32Error));
        result.message = stream.str();
        return result;
    }

    FileMonitorStatusResult DriverClient::queryFileMonitorStatus() const
    {
        // 作用：查询 R0 文件系统 minifilter 注册、启动和事件队列状态。
        // 处理：解析 KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE 为 UI 友好的模型。
        // 返回：FileMonitorStatusResult，失败时 io.ok=false 且保留错误信息。
        FileMonitorStatusResult statusResult{};
        KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE response{};

        statusResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS,
            nullptr,
            0,
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!statusResult.io.ok)
        {
            statusResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS) failed, error=" +
                std::to_string(statusResult.io.win32Error);
            return statusResult;
        }
        if (statusResult.io.bytesReturned < sizeof(response))
        {
            statusResult.io.ok = false;
            statusResult.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            statusResult.io.message =
                "file-monitor status response too small, bytesReturned=" +
                std::to_string(statusResult.io.bytesReturned);
            return statusResult;
        }

        statusResult.version = static_cast<std::uint32_t>(response.version);
        statusResult.size = static_cast<std::uint32_t>(response.size);
        statusResult.runtimeFlags = static_cast<std::uint32_t>(response.runtimeFlags);
        statusResult.operationMask = static_cast<std::uint32_t>(response.operationMask);
        statusResult.processIdFilter = static_cast<std::uint32_t>(response.processIdFilter);
        statusResult.ringCapacity = static_cast<std::uint32_t>(response.ringCapacity);
        statusResult.queuedCount = static_cast<std::uint32_t>(response.queuedCount);
        statusResult.droppedCount = static_cast<std::uint32_t>(response.droppedCount);
        statusResult.sequence = static_cast<std::uint64_t>(response.sequence);
        statusResult.registerStatus = static_cast<long>(response.registerStatus);
        statusResult.startStatus = static_cast<long>(response.startStatus);
        statusResult.lastErrorStatus = static_cast<long>(response.lastErrorStatus);
        statusResult.io.ntStatus = statusResult.lastErrorStatus;

        std::ostringstream stream;
        stream << "file-monitor status flags=0x" << std::hex << std::uppercase << statusResult.runtimeFlags
            << ", mask=0x" << statusResult.operationMask
            << ", register=0x" << static_cast<unsigned long>(static_cast<std::uint32_t>(statusResult.registerStatus))
            << ", start=0x" << static_cast<unsigned long>(static_cast<std::uint32_t>(statusResult.startStatus))
            << ", last=0x" << static_cast<unsigned long>(static_cast<std::uint32_t>(statusResult.lastErrorStatus))
            << std::dec << ", queued=" << statusResult.queuedCount
            << ", dropped=" << statusResult.droppedCount
            << ", bytesReturned=" << statusResult.io.bytesReturned;
        statusResult.io.message = stream.str();
        return statusResult;
    }
}
