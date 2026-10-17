#include "CommandRegistry.h"
#include "../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ks::cli
{
    namespace
    {
        // 原始请求采用有限缓冲；设备固定为 ArkDriverClient 的 KSword 控制设备。
        constexpr std::uint32_t kMaximumRawBytes = 1024U * 1024U;

        // readRawInput：从十六进制或单个文件建立值快照；不打开驱动、不借用调用方内存。
        std::vector<std::uint8_t> readRawInput(const Args& arguments)
        {
            if (arguments.has(L"--input-hex") && arguments.has(L"--input-file"))
            {
                throw std::invalid_argument("choose --input-hex or --input-file");
            }
            if (arguments.has(L"--input-hex"))
            {
                const auto text = arguments.get(L"--input-hex"); // 原始十六进制输入。
                if (text.size() > static_cast<std::size_t>(kMaximumRawBytes) * 3U)
                {
                    throw std::invalid_argument("raw IOCTL input exceeds 1 MiB");
                }
                auto bytes = parseHexPayload(text); // 共用严格的字节解析，不解释缓冲区 ABI。
                if (bytes.size() > kMaximumRawBytes)
                {
                    throw std::invalid_argument("raw IOCTL input exceeds 1 MiB");
                }
                return bytes;
            }
            if (!arguments.has(L"--input-file"))
            {
                return {};
            }

            // 先在同一个文件句柄上取得长度；有界读取，不按任意外部文件大小分配。
            const std::filesystem::path path(arguments.require(L"--input-file"));
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            const auto length = file ? file.tellg() : std::streampos(-1);
            if (length < 0 || length > static_cast<std::streamoff>(kMaximumRawBytes))
            {
                throw std::invalid_argument("raw IOCTL input file is unavailable or exceeds 1 MiB");
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length)); // 只持本次输入快照。
            file.seekg(0);
            if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())))
            {
                throw std::invalid_argument("raw IOCTL input file could not be read completely");
            }
            return bytes;
        }

        // callRawIoctl：确认后只发一次原始请求，输出实际传输结果及实际返回字节。
        Result callRawIoctl(const Args& arguments)
        {
            if (!arguments.has(L"--confirm"))
            {
                throw std::invalid_argument("raw IOCTL requires --confirm");
            }
            const auto code = arguments.u32(L"--code"); // CTL_CODE 的完整 uint32 值。
            (void)arguments.require(L"--code");
            const bool allowUnregistered = arguments.has(L"--allow-unregistered");
            const auto capacity = arguments.u32(L"--output-bytes", 4096U); // 调用方指定的输出上限。
            if (capacity > kMaximumRawBytes)
            {
                throw std::invalid_argument("raw IOCTL output exceeds 1 MiB");
            }
            const ksword::ark::DriverClient client; // 唯一控制设备访问入口，沿用现有 ACL/权限。
            if (!allowUnregistered)
            {
                const auto registry = client.queryIoctlRegistry();
                if (!registry.io.ok || registry.unsupported)
                {
                    throw std::invalid_argument(
                        "raw IOCTL requires a readable driver IOCTL registry; use --allow-unregistered only for an explicitly trusted compatibility probe");
                }
                const auto registered = std::find_if(
                    registry.entries.begin(), registry.entries.end(),
                    [code](const auto& entry) { return entry.ioControlCode == code; });
                if (registered == registry.entries.end())
                {
                    throw std::invalid_argument(
                        "raw IOCTL code is not registered by this driver; pass --allow-unregistered only for a trusted compatibility probe");
                }
            }
            auto input = readRawInput(arguments); // 驱动调用前完成输入验证。
            std::vector<std::uint8_t> output(capacity, std::uint8_t{0}); // 未返回的缓冲区不输出。
            const auto result = client.deviceIoControl(code,
                input.empty() ? nullptr : input.data(), static_cast<unsigned long>(input.size()),
                output.empty() ? nullptr : output.data(), capacity);
            if (result.bytesReturned > output.size())
            {
                return {4, Json::object({}), {L"Driver returned a byte count exceeding the output buffer."}};
            }
            output.resize(result.bytesReturned); // 只报告 Win32 回执声明的实际字节。
            return {result.ok ? 0 : 3, Json::object({
                {L"source", Json::string(L"ksword::ark::DriverClient synchronous raw IOCTL")},
                {L"code", Json::hex(code)},
                {L"transportSuccess", Json::boolean(result.ok)},
                {L"deviceOpenFailed", Json::boolean(result.deviceOpenFailed)},
                {L"win32Error", Json::number(result.win32Error)},
                {L"inputBytes", Json::count(input.size())},
                {L"outputCapacity", Json::number(capacity)},
                {L"bytesReturned", Json::number(result.bytesReturned)},
                {L"outputHex", Json::bytes(output, output.size())}
            }), {L"Transport success does not decode the response ABI or establish business completion. The driver is not loaded or elevated by this command; failed requests are not retried."}};
        }
    }

    // registerRawIoctl：登记原始调用及完整帮助，设备名称不向任意外部设备扩展。
    void registerRawIoctl()
    {
        addCommand({L"system ioctl call",
            L"KswordCLI.exe system ioctl call --code N [--input-hex HEX | --input-file PATH] [--output-bytes N] --confirm [--allow-unregistered] [--json]",
            L"Send one bounded raw IOCTL through the existing KSword ArkDriverClient.",
            L"Required: --code uint32, --confirm. Optional: --input-hex or --input-file (maximum 1 MiB); --output-bytes 0..1048576 (default 4096); --allow-unregistered; --json.",
            L"The target is the KSword control device only. The driver registry must prove the code is registered unless --allow-unregistered is explicitly supplied for a trusted compatibility probe. Calls may mutate state and require confirmation. No driver load, permission change, automatic retry, ABI inference or zero-filled tail output.",
            callRawIoctl});
    }
}
