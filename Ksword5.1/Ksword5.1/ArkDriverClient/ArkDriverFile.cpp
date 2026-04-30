#include "ArkDriverClient.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace ksword::ark
{
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
}
