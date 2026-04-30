#include "ArkDriverClient.h"

#include "ArkDriverError.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

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
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_PROCESS_ENTRY))
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
}
