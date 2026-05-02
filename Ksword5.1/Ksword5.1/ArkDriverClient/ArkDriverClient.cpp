#include "ArkDriverClient.h"

#include "ArkDriverError.h"

#include <algorithm>
#include <cwchar>
#include <sstream>
#include <string>
#include <utility>

#include "../ksword/string/string.h"

namespace ksword::ark
{
    namespace
    {
        constexpr unsigned long kDefaultShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;

        std::string fixedAnsiToString(const char* textBuffer, const std::size_t maxBytes)
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

        std::string fixedUtf16ToUtf8String(const unsigned short* textBuffer, const std::size_t maxChars)
        {
            // textBuffer 用途：接收共享协议中的 UTF-16 code unit 数组。
            // maxChars 用途：限定最大扫描长度，避免缺少结尾 NUL 时越界。
            if (textBuffer == nullptr || maxChars == 0U)
            {
                return {};
            }

            std::size_t length = 0U;
            while (length < maxChars && textBuffer[length] != 0U)
            {
                ++length;
            }
            if (length == 0U)
            {
                return {};
            }

            std::wstring wideText;
            wideText.reserve(length);
            for (std::size_t index = 0; index < length; ++index)
            {
                wideText.push_back(static_cast<wchar_t>(textBuffer[index]));
            }
            return ks::str::Utf16ToUtf8(wideText);
        }
    }

    DriverHandle::DriverHandle(const HANDLE handleValue) noexcept
        : m_handle(handleValue)
    {
    }

    DriverHandle::~DriverHandle()
    {
        reset();
    }

    DriverHandle::DriverHandle(DriverHandle&& other) noexcept
        : m_handle(other.release())
    {
    }

    DriverHandle& DriverHandle::operator=(DriverHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }

    bool DriverHandle::isValid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    HANDLE DriverHandle::native() const noexcept
    {
        return m_handle;
    }

    HANDLE DriverHandle::release() noexcept
    {
        HANDLE detachedHandle = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return detachedHandle;
    }

    void DriverHandle::reset(const HANDLE newHandle) noexcept
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_handle);
        }
        m_handle = newHandle;
    }

    DriverHandle DriverClient::open(const unsigned long desiredAccess) const
    {
        return DriverHandle(::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            desiredAccess,
            kDefaultShareMode,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
    }

    DriverHandle DriverClient::openOverlapped(const unsigned long desiredAccess) const
    {
        return DriverHandle(::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            desiredAccess,
            kDefaultShareMode,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr));
    }

    IoResult DriverClient::deviceIoControl(
        const unsigned long ioControlCode,
        void* const inputBuffer,
        const unsigned long inputBytes,
        void* const outputBuffer,
        const unsigned long outputBytes,
        DriverHandle* const existingHandle) const
    {
        DriverHandle localHandle;
        DriverHandle* activeHandle = existingHandle;
        if (activeHandle == nullptr)
        {
            localHandle = open();
            activeHandle = &localHandle;
        }

        if (activeHandle == nullptr || !activeHandle->isValid())
        {
            const unsigned long openError = ::GetLastError();
            IoResult result = makeWin32IoResult(false, openError, 0, "CreateFileW(KswordARK)");
            return result;
        }

        DWORD bytesReturned = 0;
        const BOOL ioctlOk = ::DeviceIoControl(
            activeHandle->native(),
            ioControlCode,
            inputBuffer,
            inputBytes,
            outputBuffer,
            outputBytes,
            &bytesReturned,
            nullptr);
        const unsigned long ioctlError = ioctlOk ? ERROR_SUCCESS : ::GetLastError();
        return makeWin32IoResult(ioctlOk != FALSE, ioctlError, bytesReturned, "KswordARK DeviceIoControl");
    }

    AsyncIoResult DriverClient::deviceIoControlAsync(
        DriverHandle& handle,
        const unsigned long ioControlCode,
        void* const inputBuffer,
        const unsigned long inputBytes,
        void* const outputBuffer,
        const unsigned long outputBytes,
        OVERLAPPED* const overlapped) const
    {
        AsyncIoResult result{};
        if (!handle.isValid())
        {
            result.issued = false;
            result.win32Error = ERROR_INVALID_HANDLE;
            return result;
        }

        DWORD bytesReturned = 0;
        const BOOL ioctlOk = ::DeviceIoControl(
            handle.native(),
            ioControlCode,
            inputBuffer,
            inputBytes,
            outputBuffer,
            outputBytes,
            &bytesReturned,
            overlapped);
        result.issued = (ioctlOk != FALSE);
        result.win32Error = result.issued ? ERROR_SUCCESS : ::GetLastError();
        result.bytesReturned = bytesReturned;
        return result;
    }

    IoResult DriverClient::terminateProcess(const std::uint32_t processId, const long exitStatus) const
    {
        DriverHandle handle = open();
        return terminateProcess(handle, processId, exitStatus);
    }

    IoResult DriverClient::terminateProcess(DriverHandle& handle, const std::uint32_t processId, const long exitStatus) const
    {
        KSWORD_ARK_TERMINATE_PROCESS_REQUEST request{};
        request.processId = processId;
        request.exitStatus = exitStatus;
        IoResult result = deviceIoControl(
            IOCTL_KSWORD_ARK_TERMINATE_PROCESS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            nullptr,
            0,
            &handle);

        std::ostringstream stream;
        stream << "pid=" << processId << ", bytesReturned=" << result.bytesReturned;
        if (result.ok)
        {
            stream << ", ioctl=ok";
        }
        else
        {
            stream << ", ioctl=fail, error=" << result.win32Error;
            if (result.win32Error == ERROR_ACCESS_DENIED)
            {
                stream << " (driver returned failing NTSTATUS, check R0 log for status)";
            }
        }
        result.message = stream.str();
        return result;
    }

    IoResult DriverClient::suspendProcess(const std::uint32_t processId) const
    {
        KSWORD_ARK_SUSPEND_PROCESS_REQUEST request{};
        request.processId = processId;
        IoResult result = deviceIoControl(
            IOCTL_KSWORD_ARK_SUSPEND_PROCESS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            nullptr,
            0);

        std::ostringstream stream;
        stream << "pid=" << processId << ", bytesReturned=" << result.bytesReturned;
        stream << (result.ok ? ", ioctl=ok" : ", ioctl=fail, error=" + std::to_string(result.win32Error));
        result.message = stream.str();
        return result;
    }

    IoResult DriverClient::setProcessProtection(const std::uint32_t processId, const std::uint8_t protectionLevel) const
    {
        KSWORD_ARK_SET_PPL_LEVEL_REQUEST request{};
        request.processId = processId;
        request.protectionLevel = protectionLevel;
        IoResult result = deviceIoControl(
            IOCTL_KSWORD_ARK_SET_PPL_LEVEL,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            nullptr,
            0);

        std::ostringstream stream;
        stream << "pid=" << processId
            << ", protectionLevel=0x" << std::hex << std::uppercase << static_cast<unsigned int>(protectionLevel)
            << std::dec << ", bytesReturned=" << result.bytesReturned;
        stream << (result.ok ? ", ioctl=ok" : ", ioctl=fail, error=" + std::to_string(result.win32Error));
        result.message = stream.str();
        return result;
    }

    ProcessVisibilityResult DriverClient::setProcessVisibility(
        const std::uint32_t processId,
        const unsigned long action,
        const unsigned long flags) const
    {
        // 作用：请求 R0 更新可恢复进程隐藏标记。
        // 返回：解析后的响应；IOCTL 失败时 io.ok=false 且 message 带 Win32 错误。
        ProcessVisibilityResult visibilityResult{};
        KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST request{};
        KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE response{};
        request.processId = processId;
        request.action = action;
        request.flags = flags;

        visibilityResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!visibilityResult.io.ok)
        {
            visibilityResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY) failed, error=" +
                std::to_string(visibilityResult.io.win32Error);
            return visibilityResult;
        }
        if (visibilityResult.io.bytesReturned < sizeof(response))
        {
            visibilityResult.io.ok = false;
            visibilityResult.io.message =
                "process visibility response too small, bytesReturned=" +
                std::to_string(visibilityResult.io.bytesReturned);
            return visibilityResult;
        }

        visibilityResult.version = static_cast<std::uint32_t>(response.version);
        visibilityResult.processId = static_cast<std::uint32_t>(response.processId);
        visibilityResult.status = static_cast<std::uint32_t>(response.status);
        visibilityResult.hiddenCount = static_cast<std::uint32_t>(response.hiddenCount);
        visibilityResult.lastStatus = static_cast<long>(response.lastStatus);
        visibilityResult.io.ntStatus = visibilityResult.lastStatus;

        std::ostringstream stream;
        stream << "pid=" << visibilityResult.processId
            << ", action=" << action
            << ", status=" << visibilityResult.status
            << ", hiddenCount=" << visibilityResult.hiddenCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(visibilityResult.lastStatus);
        visibilityResult.io.message = stream.str();
        return visibilityResult;
    }

    ProcessEnumResult DriverClient::enumerateProcesses(const unsigned long flags) const
    {
        ProcessEnumResult enumResult{};
        KSWORD_ARK_ENUM_PROCESS_REQUEST request{};
        request.flags = flags;

        std::vector<std::uint8_t> responseBuffer(1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_PROCESS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message = "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_PROCESS) failed, error=" + std::to_string(enumResult.io.win32Error);
            return enumResult;
        }

        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_PROCESS_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_ENTRY);
        if (enumResult.io.bytesReturned < headerSize)
        {
            enumResult.io.ok = false;
            enumResult.io.message = "enum-process response too small, bytesReturned=" + std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader = reinterpret_cast<const KSWORD_ARK_ENUM_PROCESS_RESPONSE*>(responseBuffer.data());
        // v1MinimumEntrySize 用途：
        // - 只要求老协议固定头字段完整，确保 protocol v1 驱动仍可被解析；
        // - imageName 长度来自协议常量，避免对空指针成员表达式产生编译器差异。
        constexpr std::size_t v1MinimumEntrySize =
            sizeof(unsigned long) * 4U + 16U;
        if (responseHeader->entrySize < v1MinimumEntrySize)
        {
            enumResult.io.ok = false;
            enumResult.io.message = "enum-process entry size invalid, entrySize=" + std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = responseHeader->version;
        enumResult.totalCount = responseHeader->totalCount;
        enumResult.returnedCount = responseHeader->returnedCount;
        const std::size_t availableCount = (enumResult.io.bytesReturned - headerSize) / static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(static_cast<std::size_t>(responseHeader->returnedCount), availableCount);
        enumResult.entries.reserve(parsedCount);
        for (std::size_t index = 0; index < parsedCount; ++index)
        {
            const std::size_t entryOffset = headerSize + (index * static_cast<std::size_t>(responseHeader->entrySize));
            const auto* entry = reinterpret_cast<const KSWORD_ARK_PROCESS_ENTRY*>(responseBuffer.data() + entryOffset);
            ProcessEntry parsedEntry{};
            parsedEntry.processId = static_cast<std::uint32_t>(entry->processId);
            parsedEntry.parentProcessId = static_cast<std::uint32_t>(entry->parentProcessId);
            parsedEntry.flags = static_cast<std::uint32_t>(entry->flags);
            parsedEntry.imageName = fixedAnsiToString(entry->imageName, sizeof(entry->imageName));
            if (responseHeader->entrySize >= sizeof(KSWORD_ARK_PROCESS_ENTRY))
            {
                parsedEntry.sessionId = static_cast<std::uint32_t>(entry->sessionId);
                parsedEntry.fieldFlags = static_cast<std::uint32_t>(entry->fieldFlags);
                parsedEntry.r0Status = static_cast<std::uint32_t>(entry->r0Status);
                parsedEntry.sessionSource = static_cast<std::uint32_t>(entry->sessionSource);
                parsedEntry.protection = static_cast<std::uint8_t>(entry->protection);
                parsedEntry.signatureLevel = static_cast<std::uint8_t>(entry->signatureLevel);
                parsedEntry.sectionSignatureLevel = static_cast<std::uint8_t>(entry->sectionSignatureLevel);
                parsedEntry.protectionSource = static_cast<std::uint32_t>(entry->protectionSource);
                parsedEntry.signatureLevelSource = static_cast<std::uint32_t>(entry->signatureLevelSource);
                parsedEntry.sectionSignatureLevelSource = static_cast<std::uint32_t>(entry->sectionSignatureLevelSource);
                parsedEntry.objectTableSource = static_cast<std::uint32_t>(entry->objectTableSource);
                parsedEntry.sectionObjectSource = static_cast<std::uint32_t>(entry->sectionObjectSource);
                parsedEntry.imagePathSource = static_cast<std::uint32_t>(entry->imagePathSource);
                parsedEntry.protectionOffset = static_cast<std::uint32_t>(entry->protectionOffset);
                parsedEntry.signatureLevelOffset = static_cast<std::uint32_t>(entry->signatureLevelOffset);
                parsedEntry.sectionSignatureLevelOffset = static_cast<std::uint32_t>(entry->sectionSignatureLevelOffset);
                parsedEntry.objectTableOffset = static_cast<std::uint32_t>(entry->objectTableOffset);
                parsedEntry.sectionObjectOffset = static_cast<std::uint32_t>(entry->sectionObjectOffset);
                parsedEntry.objectTableAddress = static_cast<std::uint64_t>(entry->objectTableAddress);
                parsedEntry.sectionObjectAddress = static_cast<std::uint64_t>(entry->sectionObjectAddress);
                parsedEntry.dynDataCapabilityMask = static_cast<std::uint64_t>(entry->dynDataCapabilityMask);
                parsedEntry.imagePath = fixedUtf16ToUtf8String(
                    entry->imagePath,
                    KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS);
            }
            enumResult.entries.push_back(std::move(parsedEntry));
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

    ProcessSpecialFlagsResult DriverClient::setProcessSpecialFlags(
        const std::uint32_t processId,
        const unsigned long action,
        const unsigned long flags) const
    {
        // 作用：请求 R0 设置 BreakOnTermination 或禁用目标进程线程 APC 插入。
        // 返回：解析后的固定响应；IOCTL 失败时 io.ok=false。
        ProcessSpecialFlagsResult specialResult{};
        KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST request{};
        KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE response{};
        request.version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
        request.processId = processId;
        request.action = action;
        request.flags = flags;

        specialResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!specialResult.io.ok)
        {
            specialResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS) failed, error=" +
                std::to_string(specialResult.io.win32Error);
            return specialResult;
        }
        if (specialResult.io.bytesReturned < sizeof(response))
        {
            specialResult.io.ok = false;
            specialResult.io.message =
                "process-special response too small, bytesReturned=" +
                std::to_string(specialResult.io.bytesReturned);
            return specialResult;
        }

        specialResult.version = static_cast<std::uint32_t>(response.version);
        specialResult.processId = static_cast<std::uint32_t>(response.processId);
        specialResult.action = static_cast<std::uint32_t>(response.action);
        specialResult.status = static_cast<std::uint32_t>(response.status);
        specialResult.appliedFlags = static_cast<std::uint32_t>(response.appliedFlags);
        specialResult.touchedThreadCount = static_cast<std::uint32_t>(response.touchedThreadCount);
        specialResult.lastStatus = static_cast<long>(response.lastStatus);
        specialResult.io.ntStatus = specialResult.lastStatus;

        std::ostringstream stream;
        stream << "pid=" << specialResult.processId
            << ", action=" << specialResult.action
            << ", status=" << specialResult.status
            << ", applied=0x" << std::hex << specialResult.appliedFlags
            << ", touchedThreads=" << std::dec << specialResult.touchedThreadCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(specialResult.lastStatus);
        specialResult.io.message = stream.str();
        return specialResult;
    }

    ProcessDkomResult DriverClient::dkomProcess(
        const std::uint32_t processId,
        const unsigned long action,
        const unsigned long flags) const
    {
        // 作用：请求 R0 执行进程 DKOM 操作，当前用于从 PspCidTable 删除 PID。
        // 返回：解析后的固定响应；诊断地址只用于展示，不作为后续凭据。
        ProcessDkomResult dkomResult{};
        KSWORD_ARK_DKOM_PROCESS_REQUEST request{};
        KSWORD_ARK_DKOM_PROCESS_RESPONSE response{};
        request.version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
        request.processId = processId;
        request.action = action;
        request.flags = flags;

        dkomResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_DKOM_PROCESS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!dkomResult.io.ok)
        {
            dkomResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_DKOM_PROCESS) failed, error=" +
                std::to_string(dkomResult.io.win32Error);
            return dkomResult;
        }
        if (dkomResult.io.bytesReturned < sizeof(response))
        {
            dkomResult.io.ok = false;
            dkomResult.io.message =
                "process-dkom response too small, bytesReturned=" +
                std::to_string(dkomResult.io.bytesReturned);
            return dkomResult;
        }

        dkomResult.version = static_cast<std::uint32_t>(response.version);
        dkomResult.processId = static_cast<std::uint32_t>(response.processId);
        dkomResult.action = static_cast<std::uint32_t>(response.action);
        dkomResult.status = static_cast<std::uint32_t>(response.status);
        dkomResult.removedEntries = static_cast<std::uint32_t>(response.removedEntries);
        dkomResult.lastStatus = static_cast<long>(response.lastStatus);
        dkomResult.pspCidTableAddress = static_cast<std::uint64_t>(response.pspCidTableAddress);
        dkomResult.processObjectAddress = static_cast<std::uint64_t>(response.processObjectAddress);
        dkomResult.io.ntStatus = dkomResult.lastStatus;

        std::ostringstream stream;
        stream << "pid=" << dkomResult.processId
            << ", action=" << dkomResult.action
            << ", status=" << dkomResult.status
            << ", removed=" << dkomResult.removedEntries
            << ", pspCidTable=0x" << std::hex << dkomResult.pspCidTableAddress
            << ", eprocess=0x" << dkomResult.processObjectAddress
            << ", lastStatus=0x" << static_cast<unsigned long>(dkomResult.lastStatus);
        dkomResult.io.message = stream.str();
        return dkomResult;
    }
}
