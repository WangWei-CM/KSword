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

        // extractExpectedRaxFromPayload：mov rax, imm64; ret → 从 payload[2..9] 读小端 imm64。
        // 调用方要求 payload 以 48 B8 开头、第 10 字节为 C3（driver 侧亦校验），
        // 这里只有在布局匹配时返回 true，否则返回 false，让调用方要么回退到
        // --expected-rax，要么拒绝请求。
        bool extractExpectedRaxFromPayload(
            const std::vector<std::uint8_t>& payload,
            std::uint64_t& outExpected)
        {
            if (payload.size() < 12U) return false;
            if (payload[0] != 0x48U || payload[1] != 0xB8U || payload[10] != 0xC3U) return false;
            std::uint64_t v = 0ULL;
            for (int i = 0; i < 8; ++i) {
                v |= static_cast<std::uint64_t>(payload[2U + static_cast<std::size_t>(i)])
                        << (static_cast<std::uint64_t>(i) * 8ULL);
            }
            outExpected = v;
            return true;
        }

        // callRxpfAtomicCommit：一次 IOCTL 走完 Register+Change(mode=2)+Write+
        // Unregister(SKIP_BACKUP_RESTORE) 的原子路径；返回 R0 响应与完整 NTSTATUS。
        // 若携带 --execute-proof，R0 还会在 mode=1 恢复 RX 之后、ClearRemovedLocked
        // 之前直接 CALL payload 一次，把 RAX 回带到 executionReturnValue 字段，
        // 这是最直接的 "CPU 真的执行了这些字节" 证据。
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

            // --execute-proof：请求 R0 在 mode=1 后 CALL rcx 进入 payload，返回 RAX。
            // 开启时 payload 必须是 48 B8 <imm64> C3（12 字节 mov rax, imm64; ret），
            // 这个契约在 driver 侧亦被强制校验，R3 侧预先校验是为了避免往返浪费。
            const bool executeProof = arguments.has(L"--execute-proof");
            std::uint64_t expectedRax = 0ULL;
            bool haveExpectedRax = false;
            if (executeProof)
            {
                if (!extractExpectedRaxFromPayload(payload, expectedRax))
                {
                    throw std::invalid_argument(
                        "--execute-proof requires payload to be exactly "
                        "48 B8 <imm64> C3 (12 bytes: mov rax, imm64; ret). "
                        "Pad to >12 bytes if desired, but first 12 must match this layout.");
                }
                haveExpectedRax = true;
                // --expected-rax 允许覆盖从 payload 推出的 imm64，用于负例测试
                // （故意写错 payload、看返回值是否也错）。
                if (arguments.has(L"--expected-rax"))
                {
                    expectedRax = arguments.integer(L"--expected-rax");
                }
            }

            // 构造 R0 请求：定长，直接 memcpy payload，不隐式扩展或零填充语义。
            KSWORD_ARK_RXPF_ATOMIC_COMMIT_REQUEST request{};
            request.header.version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
            request.header.size = sizeof(request);
            request.header.flags = KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED |
                (executeProof ? KSWORD_ARK_RXPF_FLAG_EXECUTE_PROOF : 0UL);
            request.header.confirmationToken = KSWORD_ARK_RXPF_CONFIRMATION_TOKEN;
            request.targetKind = KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE;
            request.reserved = 0UL;
            request.targetAddress = targetAddress;
            request.offset = offset32;
            request.length = static_cast<unsigned long>(payload.size());
            std::memcpy(request.bytes, payload.data(), payload.size());

            // 响应：ATOMIC_COMMIT_RESPONSE（PAGE_RESPONSE 子段 + execution-proof 字段）。
            // R0 handler 在 ReleaseRecord 后、ClearRemovedLocked 前捕获所有状态；
            // execute-proof 时还会在同一窗口里 CALL rcx 收集 RAX。
            KSWORD_ARK_RXPF_ATOMIC_COMMIT_RESPONSE response{};

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
                }), {L"IOCTL transport failed before any R0 side effect. Driver must be loaded, signed and the control device must be reachable."}};
            }

            // 响应长度检查：R0 handler 以 METHOD_BUFFERED 返回定长 ATOMIC_COMMIT_RESPONSE。
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
            const auto lastStatus = response.page.lastStatus;
            int exitCode = (lastStatus >= 0) ? 0 : 1;

            // 若请求 execute-proof 且 RAX 不等于预期，报告为失败（exit 1），
            // 但保留 ATOMIC_COMMIT 本身的 lastStatus 不变——两者是独立的判据：
            //   - lastStatus != 0 → 写入/恢复链路失败
            //   - executionAttempted == 1 且 return != expected → VTL1 revert
            //   - executionFaulted == 1 → SEH 捕获 #UD/#PF/CET
            bool execProofPass = true;
            std::wstring execVerdict = L"not-requested";
            if (executeProof)
            {
                if (response.executionAttempted == 0UL)
                {
                    execProofPass = false;
                    execVerdict = L"driver-skipped";
                    if (exitCode == 0) exitCode = 1;
                }
                else if (response.executionFaulted != 0UL)
                {
                    execProofPass = false;
                    execVerdict = L"seh-caught";
                    if (exitCode == 0) exitCode = 1;
                }
                else if (haveExpectedRax && response.executionReturnValue != expectedRax)
                {
                    execProofPass = false;
                    execVerdict = L"return-mismatch";
                    if (exitCode == 0) exitCode = 1;
                }
                else
                {
                    execVerdict = L"executed-and-returned-expected";
                }
            }

            // 构造输出字段：既暴露交付载荷又回放 R0 实际看到的状态；
            // execute-proof 时额外输出 executionReturnValue/Status/Fault/CallSite。
            std::vector<std::uint8_t> payloadBytes(payload.begin(), payload.end());

            std::vector<std::pair<std::wstring, Json>> fields = {
                {L"source", Json::string(L"IOCTL_KSWORD_ARK_RXPF_ATOMIC_COMMIT (Register→Change→Write→Unregister in one R0 handler)")},
                {L"transportSuccess", Json::boolean(true)},
                {L"win32Error", Json::number(io.win32Error)},
                {L"bytesReturned", Json::number(io.bytesReturned)},
                {L"targetAddress", Json::hex(targetAddress)},
                {L"offset", Json::hex(offset32)},
                {L"length", Json::count(payload.size())},
                {L"payloadHex", Json::bytes(payloadBytes, payloadBytes.size())},
                {L"lastStatus", Json::hex(static_cast<std::uint32_t>(lastStatus))},
                {L"state", Json::number(response.page.state)},
                {L"flags", Json::hex(response.page.flags)},
                {L"generation", Json::number(response.page.generation)},
                {L"recordId", Json::hex(response.page.recordId)},
                {L"pageBase", Json::hex(response.page.pageBase)},
                {L"originalProtection", Json::hex(response.page.originalProtection)},
                {L"currentProtection", Json::hex(response.page.currentProtection)},
                {L"writableAliasProtection", Json::hex(response.page.writableAliasProtection)},
                {L"lastWriteOffset", Json::hex(response.page.lastWriteOffset)},
                {L"lastWriteLength", Json::count(response.page.lastWriteLength)},
                {L"lastFailureReason", Json::number(response.page.lastFailureReason)},
                {L"executionAttempted", Json::boolean(response.executionAttempted != 0UL)},
                {L"executionFaulted", Json::boolean(response.executionFaulted != 0UL)},
                {L"executionStatus", Json::hex(static_cast<std::uint32_t>(response.executionStatus))},
                {L"executionReturnValue", Json::hex(response.executionReturnValue)},
                {L"executionCallSiteAddress", Json::hex(response.executionCallSiteAddress)},
                {L"executionCallSiteOffset", Json::hex(response.executionCallSiteOffset)},
                {L"executionVerdict", Json::string(execVerdict)},
                {L"executionProofPassed", Json::boolean(execProofPass)}
            };
            if (haveExpectedRax)
            {
                fields.push_back({L"executionExpectedReturnValue", Json::hex(expectedRax)});
            }

            return {exitCode, Json::object(fields), {L"NX window is confined to the driver function body. On success the page returns to RX with payload bytes persisted (SKIP_BACKUP_RESTORE). With --execute-proof, R0 calls into the written payload once and reports RAX in executionReturnValue (direct CPU-execution evidence). The payload must restore itself quickly or PatchGuard/exceptions will bugcheck."}};
        }
    }

    // registerRxpfAtomicCommit：登记 kernel rxpf-atomic-commit 命令。
    void registerRxpfAtomicCommit()
    {
        addCommand({L"kernel rxpf-atomic-commit",
            L"KswordCLI.exe kernel rxpf-atomic-commit --address VA --offset N --hex HEX --confirm [--execute-proof] [--expected-rax 0xN] [--json]",
            L"Atomic RXPF Register+Change(mode=2)+Write+Unregister(SKIP_BACKUP_RESTORE) in one IOCTL; optional R0 self-call for direct CPU-execution proof.",
            L"Required: --address kernel VA inside a loaded image .text page, --offset page-offset (0..0xFFF), --hex payload bytes (1..64 bytes), --confirm. Optional: --execute-proof (requires payload to be exactly 48 B8 <imm64> C3), --expected-rax 0xN (default: imm64 extracted from payload), --json.",
            L"Fuses the four-step Phase G path into a single R0 handler so the NX window is confined to the kernel function body (microseconds). With --execute-proof the driver calls into the just-written payload once (SEH-protected) and reports RAX in executionReturnValue — if that equals the imm64 in the mov rax instruction, the CPU provably decoded and executed the modified bytes. Exit codes: 0=atomic-commit success and (if proof) RAX matched; 1=NTSTATUS failure OR execution proof failed; 3=IOCTL transport failure; 4=incompatible response.",
            callRxpfAtomicCommit});
    }
}
