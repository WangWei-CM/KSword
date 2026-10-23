#include "CommandRegistry.h"
#include "../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../shared/driver/KswordArkRxPfIoctl.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace ks::cli
{
    namespace
    {
        // RXPF 协议上限：MAX_WRITE_BYTES=64，一轮原子提交写入不超过 64 字节。
        constexpr std::uint32_t kAtomicCommitMaxBytes = KSWORD_ARK_RXPF_MAX_WRITE_BYTES;

        // callRxpfAtomicCommit：一次 IOCTL 走完 Register+Change(mode=2)+Write+
        // Unregister(SKIP_BACKUP_RESTORE) 的原子路径；返回 R0 响应与完整 NTSTATUS。
        Result callRxpfAtomicCommit(const Args& arguments)
        {
            // 变更路径强制要求显式 --confirm，与现有 raw IOCTL / write-va 保持一致。
            if (!arguments.has(L"--confirm"))
            {
                throw std::invalid_argument("rxpf-atomic-commit requires --confirm");
            }

            // targetAddress：目标 .text 页内一个 VA；对齐到页由 R0 侧处理（CreateRecord 内）。
            const auto targetAddress = arguments.integer(L"--address");
            (void)arguments.require(L"--address");

            // offset：页内偏移；R0 侧校验 offset+length <= PAGE_SIZE。
            const auto offset32 = arguments.u32(L"--offset");
            (void)arguments.require(L"--offset");

            // --hex：payload 字节的十六进制表示，交给共享严格解析器。
            const auto hexText = arguments.require(L"--hex");
            auto payload = parseHexPayload(hexText);
            if (payload.empty())
            {
                throw std::invalid_argument("rxpf-atomic-commit requires a non-empty --hex payload");
            }
            if (payload.size() > kAtomicCommitMaxBytes)
            {
                throw std::invalid_argument("rxpf-atomic-commit payload exceeds 64 bytes");
            }

            // 构造 R0 请求：定长，直接 memcpy payload，不隐式扩展或零填充语义。
            KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST request{};
            request.header.version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
            request.header.size = sizeof(request);
            request.header.flags = KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED;
            request.header.confirmationToken = KSWORD_ARK_RXPF_CONFIRMATION_TOKEN;
            request.targetKind = KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE;
            request.reserved = 0UL;
            request.targetAddress = targetAddress;
            request.offset = offset32;
            request.length = static_cast<unsigned long>(payload.size());
            std::memcpy(request.bytes, payload.data(), payload.size());

            // 响应复用 PAGE_RESPONSE；R0 handler 在 ReleaseRecord 后、ClearRemovedLocked 前捕获状态。
            KSWORD_ARK_RXPF_PAGE_RESPONSE response{};

            const ksword::ark::DriverClient client;
            const auto io = client.deviceIoControl(
                IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT,
                &request, static_cast<unsigned long>(sizeof(request)),
                &response, static_cast<unsigned long>(sizeof(response)));

            // transport 失败：驱动句柄打不开或 DeviceIoControl 返回 FALSE。
            if (!io.ok)
            {
                return {3, Json::object({
                    {L"source", Json::string(L"ksword::ark::DriverClient synchronous IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT")},
                    {L"transportSuccess", Json::boolean(false)},
                    {L"deviceOpenFailed", Json::boolean(io.deviceOpenFailed)},
                    {L"win32Error", Json::number(io.win32Error)},
                    {L"targetAddress", Json::hex(targetAddress)},
                    {L"offset", Json::hex(offset32)},
                    {L"length", Json::count(payload.size())}
                }), {L"DeviceIoControl failed; inspect Win32 error before attributing a driver stage. If the device could not open, check that the driver and control device are available."}};
            }

            // 响应长度检查：R0 handler 以 METHOD_BUFFERED 返回定长 PAGE_RESPONSE。
            if (io.bytesReturned != sizeof(response))
            {
                return {4, Json::object({
                    {L"source", Json::string(L"ksword::ark::DriverClient synchronous IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT")},
                    {L"transportSuccess", Json::boolean(true)},
                    {L"win32Error", Json::number(io.win32Error)},
                    {L"bytesReturned", Json::number(io.bytesReturned)},
                    {L"expectedBytes", Json::number(static_cast<std::uint32_t>(sizeof(response)))}
                }), {L"Driver returned an unexpected response length; refusing to decode."}};
            }

            // NTSTATUS 由 R0 把该次原子序列的实际结果写回 lastStatus。
            const auto lastStatus = response.lastStatus;
            const int exitCode = (lastStatus >= 0) ? 0 : 1;

            // 构造输出字段：既暴露交付载荷又回放 R0 实际看到的状态。
            std::vector<std::uint8_t> payloadBytes(payload.begin(), payload.end());

            return {exitCode, Json::object({
                {L"source", Json::string(L"IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT (Register→Change→Write→Unregister in one R0 handler)")},
                {L"transportSuccess", Json::boolean(true)},
                {L"win32Error", Json::number(io.win32Error)},
                {L"bytesReturned", Json::number(io.bytesReturned)},
                {L"targetAddress", Json::hex(targetAddress)},
                {L"offset", Json::hex(offset32)},
                {L"length", Json::count(payload.size())},
                {L"payloadHex", Json::bytes(payloadBytes, payloadBytes.size())},
                {L"lastStatus", Json::hex(static_cast<std::uint32_t>(lastStatus))},
                {L"state", Json::number(response.state)},
                {L"flags", Json::hex(response.flags)},
                {L"generation", Json::number(response.generation)},
                {L"recordId", Json::hex(response.recordId)},
                {L"pageBase", Json::hex(response.pageBase)},
                {L"originalProtection", Json::hex(response.originalProtection)},
                {L"currentProtection", Json::hex(response.currentProtection)},
                {L"writableAliasProtection", Json::hex(response.writableAliasProtection)},
                {L"lastWriteOffset", Json::hex(response.lastWriteOffset)},
                {L"lastWriteLength", Json::count(response.lastWriteLength)},
                {L"lastFailureReason", Json::number(response.lastFailureReason)}
            }), {L"The single IOCTL reduces user-mode round trips but does not isolate other CPUs from the temporary NX state. Check lastStatus for the actual restore result; the command does not capture a rollback backup."}};
        }
    }

    // registerRxpfAtomicCommit：登记 kernel rxpf-atomic-commit 命令。
    void registerRxpfAtomicCommit()
    {
        addCommand({L"kernel rxpf-atomic-commit",
            L"KswordCLI.exe kernel rxpf-atomic-commit --address VA --offset N --hex HEX --confirm [--json]",
            L"Atomic RXPF Register+Change(mode=2)+Write+Unregister(SKIP_BACKUP_RESTORE) in one IOCTL.",
            L"Required: --address kernel VA inside a loaded image .text page, --offset page-offset (0..0xFFF), --hex payload bytes (1..64 bytes), --confirm. Optional: --json.",
            L"Fuses the four-step Phase G path into a single R0 handler and reduces user-mode round trips. It does not isolate other CPUs: an active code page can still fault while NX. EXTERNAL_IMAGE only. No backup is captured for later rollback; the caller remains responsible for restoration. Exit codes: 0=reported restore success, 1=reported failure, 3=DeviceIoControl failure, 4=incompatible response.",
            callRxpfAtomicCommit});
    }
}
