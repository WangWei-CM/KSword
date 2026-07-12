#include "ArkDriverError.h"

#include <sstream>

namespace ksword::ark
{
    std::string formatWin32Error(const unsigned long errorCode)
    {
        if (errorCode == ERROR_SUCCESS)
        {
            return "success";
        }

        // FormatMessageA is best-effort; the numeric code is always retained so
        // diagnostics remain useful even when no system message is available.
        char* messageBuffer = nullptr;
        const DWORD formatFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD copiedChars = ::FormatMessageA(
            formatFlags,
            nullptr,
            errorCode,
            0,
            reinterpret_cast<LPSTR>(&messageBuffer),
            0,
            nullptr);

        std::ostringstream stream;
        stream << "error=" << errorCode;
        if (copiedChars != 0 && messageBuffer != nullptr)
        {
            std::string messageText(messageBuffer, messageBuffer + copiedChars);
            while (!messageText.empty() &&
                (messageText.back() == '\r' || messageText.back() == '\n' || messageText.back() == ' '))
            {
                messageText.pop_back();
            }
            if (!messageText.empty())
            {
                stream << " (" << messageText << ")";
            }
        }

        if (messageBuffer != nullptr)
        {
            ::LocalFree(messageBuffer);
        }
        return stream.str();
    }

    IoResult makeWin32IoResult(
        const bool ok,
        const unsigned long win32Error,
        const unsigned long bytesReturned,
        const char* const operationName)
    {
        IoResult result{};
        result.ok = ok;
        result.win32Error = win32Error;
        result.bytesReturned = bytesReturned;

        std::ostringstream stream;
        stream << (operationName != nullptr ? operationName : "DeviceIoControl")
            << (ok ? " ok" : " failed")
            << ", bytesReturned=" << bytesReturned;
        if (!ok)
        {
            stream << ", " << formatWin32Error(win32Error);
        }
        result.message = stream.str();
        return result;
    }
}
