#pragma once

#include "ArkDriverTypes.h"

namespace ksword::ark
{
    // Convert a Win32 error code into short English diagnostic text. The caller
    // may still display the numeric error for localization-sensitive UI strings.
    std::string formatWin32Error(unsigned long errorCode);

    // Build the standard IoResult shape from a Win32 success bit and byte count.
    // The function does not throw and never calls DeviceIoControl itself.
    IoResult makeWin32IoResult(
        bool ok,
        unsigned long win32Error,
        unsigned long bytesReturned,
        const char* operationName);
}
