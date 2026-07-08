#include "ArkDriverClient.h"

#include <sstream>

namespace ksword::ark
{
    namespace
    {
        // isHwidUnsupportedWin32Error：
        // - 输入：DeviceIoControl 失败时的 Win32 错误码；
        // - 处理：识别旧驱动未注册新 HWID IOCTL 的常见映射；
        // - 返回：true 表示 UI 应显示“驱动版本过旧/未集成”。
        bool isHwidUnsupportedWin32Error(const unsigned long win32Error)
        {
            return win32Error == ERROR_INVALID_FUNCTION ||
                win32Error == ERROR_NOT_SUPPORTED ||
                win32Error == ERROR_INVALID_PARAMETER;
        }

        // formatHwidIoMessage：
        // - 输入：操作名、IO 结果和可选响应；
        // - 处理：统一拼接日志/状态栏文本；
        // - 返回：面向 UI 的短消息。
        std::string formatHwidIoMessage(
            const char* operationName,
            const IoResult& io,
            const KSWORD_ARK_HWID_DISPATCH_RESPONSE& response)
        {
            std::ostringstream stream;
            stream << operationName
                << ", ioctl=" << (io.ok ? "ok" : "fail")
                << ", win32=" << io.win32Error
                << ", bytes=" << io.bytesReturned
                << ", status=" << response.overallStatus
                << ", active=0x" << std::hex << response.activeTargetFlags
                << ", failed=0x" << response.failedTargetFlags;
            return stream.str();
        }
    }

    HwidDispatchResult DriverClient::queryHwidDispatchState() const
    {
        // response 用途：接收 R0 固定状态包。
        HwidDispatchResult result{};

        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_HWID_DISPATCH_QUERY,
            nullptr,
            0UL,
            &result.response,
            static_cast<unsigned long>(sizeof(result.response)));
        result.unsupported = !result.io.ok && isHwidUnsupportedWin32Error(result.io.win32Error);
        if (result.io.bytesReturned >= sizeof(result.response))
        {
            result.io.ntStatus = result.response.lastStatus;
        }
        result.io.message = result.unsupported
            ? "IOCTL_KSWORD_ARK_HWID_DISPATCH_QUERY unsupported by current driver"
            : formatHwidIoMessage("IOCTL_KSWORD_ARK_HWID_DISPATCH_QUERY", result.io, result.response);
        return result;
    }

    HwidDispatchResult DriverClient::controlHwidDispatch(
        const KSWORD_ARK_HWID_DISPATCH_CONTROL_REQUEST& request) const
    {
        // mutableRequest 用途：DeviceIoControl 需要非常量输入指针，协议内容不在 R3 修改。
        KSWORD_ARK_HWID_DISPATCH_CONTROL_REQUEST mutableRequest = request;
        HwidDispatchResult result{};

        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_HWID_DISPATCH_CONTROL,
            &mutableRequest,
            static_cast<unsigned long>(sizeof(mutableRequest)),
            &result.response,
            static_cast<unsigned long>(sizeof(result.response)));
        result.unsupported = !result.io.ok && isHwidUnsupportedWin32Error(result.io.win32Error);
        if (result.io.bytesReturned >= sizeof(result.response))
        {
            result.io.ntStatus = result.response.lastStatus;
        }
        result.io.message = result.unsupported
            ? "IOCTL_KSWORD_ARK_HWID_DISPATCH_CONTROL unsupported by current driver"
            : formatHwidIoMessage("IOCTL_KSWORD_ARK_HWID_DISPATCH_CONTROL", result.io, result.response);
        return result;
    }
}
