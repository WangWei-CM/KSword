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
        // RXPF 协议上限：MAX_WRITE_BYTES=64。
        constexpr std::uint32_t kExecProofMaxBytes = KSWORD_ARK_RXPF_MAX_WRITE_BYTES;

        // callRxpfExecProof：单次 IOCTL 走完 driver 侧的 Register(ALLOCATED_TEST)+
        // Change(mode=2)+Write(custom payload)+PageFaultInstall+ConcurrentExecute+Reset+
        // Unregister，payload 字节 fetch 都经由 shadow IDT emulator 服务，
        // worker 的 RAX 回传到 executionReturnValue。
        //
        // 这是 ATOMIC_COMMIT 的"合法"兄弟——ATOMIC_COMMIT 走 EXTERNAL_IMAGE，mode=1 restore
        // 被 VTL1 hash 拒绝 → 页永远 NX → CPU fetch 必 bugcheck 0xFC；EXEC_PROOF 走
        // ALLOCATED_TEST（driver 自有 PFN，不受 VTL1 digest 约束），IDT emulator 真正
        // 服务 fetch，payload 行为可观测，不会蓝屏。
        Result callRxpfExecProof(const Args& arguments)
        {
            // 变更路径强制 --confirm。
            if (!arguments.has(L"--confirm"))
            {
                throw std::invalid_argument("rxpf-exec-proof requires --confirm");
            }

            // --hex：payload 字节的十六进制表示（emulator 自行 decode）。
            const auto hexText = arguments.require(L"--hex");
            auto payload = parseHexPayload(hexText);
            if (payload.empty())
            {
                throw std::invalid_argument("rxpf-exec-proof requires a non-empty --hex payload");
            }
            if (payload.size() > kExecProofMaxBytes)
            {
                throw std::invalid_argument("rxpf-exec-proof payload exceeds 64 bytes");
            }

            // --expected：payload 执行完后期望 RAX 的值；driver 会把真实 RAX 与此比对。
            const auto expectedRax = arguments.integer(L"--expected");
            (void)arguments.require(L"--expected");

            // 构造请求：targetKind 固定 ALLOCATED_TEST（driver-owned MDL）。
            KSWORD_ARK_RXPF_EXEC_PROOF_REQUEST request{};
            request.header.version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
            request.header.size = sizeof(request);
            request.header.flags = KSWORD_ARK_RXPF_FLAG_UI_CONFIRMED;
            request.header.confirmationToken = KSWORD_ARK_RXPF_CONFIRMATION_TOKEN;
            request.targetKind = KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST;
            request.length = static_cast<unsigned long>(payload.size());
            request.expectedReturnValue = expectedRax;
            std::memcpy(request.bytes, payload.data(), payload.size());

            // 响应：workerCount + returnedValue + faultsObserved + verdict + 诊断字段。
            // 显式 16-byte 对齐规避 I/O manager ProbeForWrite 的对齐检查。
            alignas(16) KSWORD_ARK_RXPF_EXEC_PROOF_RESPONSE response{};
            // 诊断：打印 request/response 指针与 size + 实际 CTL_CODE。
            std::fwprintf(stderr, L"[diag] CTL_CODE=0x%08X &request=%p sizeof(request)=%zu &response=%p sizeof(response)=%zu\n",
                static_cast<unsigned int>(IOCTL_KSWORD_ARK_RXPF_EXEC_PROOF),
                static_cast<void*>(&request), sizeof(request),
                static_cast<void*>(&response), sizeof(response));

            const ksword::ark::DriverClient client;
            const auto io = client.deviceIoControl(
                IOCTL_KSWORD_ARK_RXPF_EXEC_PROOF,
                &request, static_cast<unsigned long>(sizeof(request)),
                &response, static_cast<unsigned long>(sizeof(response)));

            // transport 失败且设备打不开 → 真的 transport failure，不读 response。
            if (!io.ok && io.deviceOpenFailed)
            {
                return {3, Json::object({
                    {L"source", Json::string(L"ksword::ark::DriverClient synchronous IOCTL_KSWORD_ARK_RXPF_EXEC_PROOF")},
                    {L"transportSuccess", Json::boolean(false)},
                    {L"deviceOpenFailed", Json::boolean(true)},
                    {L"win32Error", Json::number(io.win32Error)},
                    {L"length", Json::count(payload.size())}
                }), {L"IOCTL transport failed before any R0 side effect. Driver must be loaded, signed and the control device must be reachable."}};
            }
            // 注意：handler 返回 non-success NTSTATUS 时 DeviceIoControl 也返回 FALSE，
            // 但 output buffer 仍被 WDF 填充。继续读 response 以获取 verdict/setupStatus。

            // 诊断：dump raw response bytes 便于定位 handler 是否被调用。
            const unsigned char* rawResp = reinterpret_cast<const unsigned char*>(&response);
            std::vector<std::uint8_t> rawRespBytes(rawResp, rawResp + sizeof(response));

            // 不再对 bytesReturned != sizeof(response) 直接 refuse：
            // handler 内部早返回时 dispatchFixed 早 return 不设 BytesReturned，CLI 要能看到真因。
            // Response 已被 RtlZeroMemory 清零，字段默认 0，不会读到野值。

            // Verdict 映射到文字；0=SUCCESS 为唯一通过状态。
            const unsigned long verdictCode = response.verdict;
            std::wstring verdictText = L"unknown";
            switch (verdictCode)
            {
                case KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SUCCESS:
                    verdictText = L"success"; break;
                case KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_RETURN_MISMATCH:
                    verdictText = L"return-mismatch"; break;
                case KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_NO_FAULTS:
                    verdictText = L"no-faults"; break;
                case KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_EXEC_FAILED:
                    verdictText = L"exec-failed"; break;
                case KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SETUP_FAILED:
                    verdictText = L"setup-failed"; break;
            }

            // Exit code：verdict==SUCCESS 且 lastStatus>=0 → 0，否则 1。
            const int exitCode = (verdictCode == KSWORD_ARK_RXPF_EXEC_PROOF_VERDICT_SUCCESS && response.lastStatus >= 0) ? 0 : 1;

            std::vector<std::uint8_t> payloadBytes(payload.begin(), payload.end());

            return {exitCode, Json::object({
                {L"source", Json::string(L"IOCTL_KSWORD_ARK_RXPF_EXEC_PROOF (ALLOCATED_TEST + shadow IDT emulator)")},
                {L"transportSuccess", Json::boolean(true)},
                {L"win32Error", Json::number(io.win32Error)},
                {L"bytesReturned", Json::number(io.bytesReturned)},
                {L"targetKind", Json::number(response.targetKind)},
                {L"length", Json::count(response.length)},
                {L"payloadHex", Json::bytes(payloadBytes, payloadBytes.size())},
                {L"recordId", Json::hex(response.recordId)},
                {L"pageBase", Json::hex(response.pageBase)},
                {L"workerCount", Json::number(response.workerCount)},
                {L"returnedValue", Json::hex(response.returnedValue)},
                {L"expectedReturnValue", Json::hex(response.expectedReturnValue)},
                {L"faultsObserved", Json::hex(response.faultsObserved)},
                {L"verdict", Json::string(verdictText)},
                {L"verdictCode", Json::number(verdictCode)},
                {L"lastStatus", Json::hex(static_cast<std::uint32_t>(response.lastStatus))},
                {L"setupStatus", Json::hex(static_cast<std::uint32_t>(response.setupStatus))},
                {L"rawResponseHex", Json::bytes(rawRespBytes, rawRespBytes.size())}
            }), {L"ALLOCATED_TEST page allocated by driver, bytes written to writable alias, shadow IDT installed, call rcx issued on every active CPU; each fetched byte is #PF→emulator. Return RAX is reported from worker 0 (all workers should agree). verdict=success means CPU-executed emulated bytes produced RAX==expectedReturnValue AND fault count non-zero (emulator did service the fetches)."}};
        }
    }

    void registerRxpfExecProof()
    {
        addCommand({L"kernel rxpf-exec-proof",
            L"KswordCLI.exe kernel rxpf-exec-proof --hex HEX --expected 0xN --confirm [--json]",
            L"Allocate a driver-owned test page, write custom payload, drive emulated execution via shadow IDT, verify RAX.",
            L"Required: --hex payload bytes (1..64), --expected 0xN RAX value payload is expected to produce, --confirm. Optional: --json.",
            L"Writes caller bytes to a freshly-allocated driver-owned (ALLOCATED_TEST) page and drives emulated execution of them via the shadow IDT #PF handler on every active CPU. The page is NX and MmChangeImageProtection(mode=1) is never attempted — this bypasses the VTL1 SHA-512 restore-rejection that prevents direct CPU execution of modified EXTERNAL_IMAGE pages. Compare returnedValue to expectedReturnValue to verify payload executed as expected. Exit 0 on verdict=success (RAX matches AND fault count non-zero), 1 otherwise.",
            callRxpfExecProof});
    }
}
