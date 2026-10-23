#include "MemoryAccessBackend.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../ksword/log/log.h"
#include "../UI/AsyncCleanupRegistry.h"
// 切片长度与门禁顺序都取共用纯函数，和 R0 用的是同一份实现，也是单元测试
// 覆盖的那一份。
#include "../../../shared/driver/KswordArkDdmaPlan.h"

#include "../../../shared/evidence/SerialTransactionQueue.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <limits>
#include <vector>

// ============================================================
// MemoryAccessBackend.cpp
// 作用：
// - 实现标准驱动通道与 DDMA 通道的统一读写门面；
// - 所有"DDMA 能不能用""怎么切片""失败怎么说"的判据只在本文件里有一份。
// ============================================================

namespace ksword::memory_backend
{
    namespace
    {
        // DDMA 的一次 DMA 传输长度，与 R0 协议常量一致。
        constexpr std::uint64_t kDdmaPageBytes =
            static_cast<std::uint64_t>(KSWORD_ARK_DDMA_TRANSFER_BYTES);

        // 标准通道的单次上限，直接取协议常量，避免在 UI 里再写一遍魔数。
        constexpr std::uint64_t kStandardPhysicalReadMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES);
        constexpr std::uint64_t kStandardPhysicalWriteMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES);
        constexpr std::uint64_t kStandardVirtualReadMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_READ_MAX_BYTES);
        constexpr std::uint64_t kStandardVirtualWriteMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_WRITE_MAX_BYTES);

        // x64 内核高半区起点。规范形式的地址要么低于 0x0000800000000000，
        // 要么不低于 0xFFFF800000000000，中间是不可用的空洞。
        constexpr std::uint64_t kKernelSpaceStart = 0xFFFF800000000000ULL;

        bool isValidMemoryRange(const std::uint64_t address, const std::uint64_t length)
        {
            return length != 0ULL &&
                length <= static_cast<std::uint64_t>((std::numeric_limits<qsizetype>::max)()) &&
                address <= (std::numeric_limits<std::uint64_t>::max)() - (length - 1ULL);
        }

        // ddmaFlagsForRead / ddmaFlagsForWrite：
        // 把"会话已确认"翻译成协议 flags。LBA_VALID 与 ACKNOWLEDGED 必须同时
        // 带上，缺任何一个 R0 都会返回各自独立的状态码而不是笼统拒绝。
        unsigned long ddmaFlagsForRead()
        {
            return KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED |
                KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID |
                KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED;
        }

        unsigned long ddmaFlagsForWrite(const bool forceApproved)
        {
            unsigned long flags = ddmaFlagsForRead();
            if (forceApproved)
            {
                flags |= KSWORD_ARK_DDMA_FLAG_FORCE;
            }
            return flags;
        }

        // describeDdmaReadFailure：把 R0 读状态翻译成用户能据此行动的文案。
        QString describeDdmaReadFailure(const ksword::ark::DdmaReadResult& result)
        {
            switch (result.readStatus)
            {
            case KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED:
                return QStringLiteral("驱动拒绝：必须显式指定暂存扇区 LBA。");
            case KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED:
                return QStringLiteral("驱动拒绝：尚未确认暂存扇区可被临时覆盖。");
            case KSWORD_ARK_DDMA_READ_STATUS_DISK_NOT_FOUND:
                return QStringLiteral("目标磁盘已不在设备列表中，请重新探测 DDMA 通道。");
            case KSWORD_ARK_DDMA_READ_STATUS_RANGE_REJECTED:
                return QStringLiteral("物理区间被驱动拒绝：长度为 0、超过一页或跨页。");
            case KSWORD_ARK_DDMA_READ_STATUS_MAP_FAILED:
                return QStringLiteral("映射目标物理页失败，该物理地址可能不存在。");
            case KSWORD_ARK_DDMA_READ_STATUS_BACKUP_FAILED:
                return QStringLiteral(
                    "备份暂存扇区失败，本次未对磁盘做任何写入。请确认该 LBA 在磁盘容量范围内。");
            case KSWORD_ARK_DDMA_READ_STATUS_STAGE_OUT_FAILED:
                return QStringLiteral("把目标物理页 DMA 写到暂存扇区失败。");
            case KSWORD_ARK_DDMA_READ_STATUS_STAGE_IN_FAILED:
                return QStringLiteral("从暂存扇区 DMA 读回失败。");
            case KSWORD_ARK_DDMA_READ_STATUS_IRQL_REJECTED:
                return QStringLiteral("驱动在非 PASSIVE_LEVEL 下拒绝了本次请求。");
            case KSWORD_ARK_DDMA_READ_STATUS_BUFFER_TOO_SMALL:
                return QStringLiteral("响应缓冲不足，这是客户端缺陷，请上报。");
            default:
                break;
            }
            return QStringLiteral("DDMA 读取失败，readStatus=%1。").arg(result.readStatus);
        }

        // describeDdmaWriteFailure：写路径的状态文案，与读路径分开是因为
        // FORCE_REQUIRED 只在写路径出现，且需要引导用户去点确认而不是改配置。
        QString describeDdmaWriteFailure(const ksword::ark::DdmaWriteResult& result)
        {
            switch (result.writeStatus)
            {
            case KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED:
                return QStringLiteral("驱动拒绝：必须显式指定暂存扇区 LBA。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED:
                return QStringLiteral("驱动拒绝：尚未确认暂存扇区可被临时覆盖。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED:
                return QStringLiteral("驱动要求对 DDMA 写入附加强制标志。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_DISK_NOT_FOUND:
                return QStringLiteral("目标磁盘已不在设备列表中，请重新探测 DDMA 通道。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_RANGE_REJECTED:
                return QStringLiteral("物理区间被驱动拒绝：长度为 0、超过一页或跨页。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_MAP_FAILED:
                return QStringLiteral("映射目标物理页失败，该物理地址可能不存在。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_BACKUP_FAILED:
                return QStringLiteral(
                    "备份暂存扇区失败，本次未对磁盘或物理内存做任何写入。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_READBACK_FAILED:
                return QStringLiteral(
                    "读-改-写的读回阶段失败，目标物理页未被修改。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_OUT_FAILED:
                return QStringLiteral("把数据 DMA 写到暂存扇区失败，目标物理页未被修改。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_IN_FAILED:
                return QStringLiteral(
                    "从暂存扇区 DMA 写入目标物理页失败，该页可能处于半写状态。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_ACCESS_DENIED:
                return QStringLiteral("驱动安全策略拒绝了本次 DDMA 写入。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_IRQL_REJECTED:
                return QStringLiteral("驱动在非 PASSIVE_LEVEL 下拒绝了本次请求。");
            default:
                break;
            }
            return QStringLiteral("DDMA 写入失败，writeStatus=%1。").arg(result.writeStatus);
        }

        // formatHex：统一的 0x 十六进制文案，避免各处大小写与位宽不一致。
        QString formatHex(const std::uint64_t value)
        {
            return QStringLiteral("0x%1").arg(value, 0, 16).toUpper().replace(
                QStringLiteral("0X"), QStringLiteral("0x"));
        }

        std::mutex& scratchReceiptMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::vector<DdmaScratchFailure>& mutableDdmaScratchFailures()
        {
            static std::vector<DdmaScratchFailure> failures;
            return failures;
        }

        AccessOutcome recordScratchFailure(const DdmaSession& session,
            const std::uint64_t physicalAddress, AccessOutcome outcome, const bool write)
        {
            if (outcome.scratchDirty)
            {
                {
                    std::lock_guard<std::mutex> receiptLock(scratchReceiptMutex());
                    mutableDdmaScratchFailures().push_back({session, physicalAddress,
                        outcome.bytesDone, write, outcome.failureText});
                }
                // The existing thread-safe archive exposes this receipt even
                // when the page was closed or its visual result became stale.
                warn << "DDMA scratch restore failed: disk=" << session.diskIndex
                     << " device=" << QString::fromStdWString(session.deviceName)
                     << " lba=" << session.scratchLba << " physical=" << physicalAddress
                     << " write=" << write << " bytesDone=" << outcome.bytesDone
                     << " detail=" << outcome.failureText << eol;
            }
            return outcome;
        }

        // ddmaReadOnePage：
        // - 读取一个物理页内的一段（调用方保证不跨页）；
        // - 把 R0 的多段状态压成 AccessOutcome 的少数几个布尔。
        AccessOutcome ddmaReadOnePage(
            const ksword::ark::DriverClient& client,
            const DdmaSession& session,
            const std::uint64_t physicalAddress,
            const std::uint32_t lengthBytes)
        {
            AccessOutcome outcome;
            const ksword::ark::DdmaReadResult result = client.ddmaReadPhysicalMemory(
                session.diskIndex,
                physicalAddress,
                lengthBytes,
                session.scratchLba,
                ddmaFlagsForRead());

            // 暂存扇区是否还原要先取：即使读取本身失败也必须把脏扇区报出去。
            outcome.scratchDirty = !result.scratchRestored() &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED) &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED) &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_BACKUP_FAILED) &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_UNAVAILABLE);

            if (!result.io.ok)
            {
                outcome.failureText = result.unsupported
                    ? QStringLiteral("当前驱动不支持 DDMA，请更新 KswordARK 驱动。")
                    : QStringLiteral("DDMA 读取通信失败：%1")
                          .arg(QString::fromStdString(result.io.message));
                return recordScratchFailure(session, physicalAddress, std::move(outcome), false);
            }
            if (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_OK ||
                result.data.size() != static_cast<std::size_t>(lengthBytes))
            {
                outcome.failureText = describeDdmaReadFailure(result);
                return recordScratchFailure(session, physicalAddress, std::move(outcome), false);
            }

            outcome.ok = true;
            outcome.bytesDone = lengthBytes;
            outcome.data = QByteArray(
                reinterpret_cast<const char*>(result.data.data()),
                static_cast<qsizetype>(result.data.size()));
            return recordScratchFailure(session, physicalAddress, std::move(outcome), false);
        }

        // ddmaWriteOnePage：写入一个物理页内的一段（调用方保证不跨页）。
        AccessOutcome ddmaWriteOnePage(
            const ksword::ark::DriverClient& client,
            const DdmaSession& session,
            const std::uint64_t physicalAddress,
            const QByteArray& chunk,
            const bool forceApproved)
        {
            AccessOutcome outcome;
            const std::vector<std::uint8_t> payload(
                reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                reinterpret_cast<const std::uint8_t*>(chunk.constData()) + chunk.size());

            const ksword::ark::DdmaWriteResult result = client.ddmaWritePhysicalMemory(
                session.diskIndex,
                physicalAddress,
                payload,
                session.scratchLba,
                ddmaFlagsForWrite(forceApproved));

            outcome.scratchDirty = !result.scratchRestored() &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_BACKUP_FAILED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_UNAVAILABLE);
            outcome.lostUpdateWindow = result.readModifyWriteUsed();

            if (!result.io.ok)
            {
                outcome.failureText = result.unsupported
                    ? QStringLiteral("当前驱动不支持 DDMA，请更新 KswordARK 驱动。")
                    : QStringLiteral("DDMA 写入通信失败：%1")
                          .arg(QString::fromStdString(result.io.message));
                return recordScratchFailure(session, physicalAddress, std::move(outcome), true);
            }
            if (result.writeStatus == KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED)
            {
                // 这一条要与其它失败区分开：调用方应当弹确认后带 force 重试，
                // 而不是让用户回去改 DDMA 配置。
                outcome.forceRequired = true;
                outcome.failureText = describeDdmaWriteFailure(result);
                return recordScratchFailure(session, physicalAddress, std::move(outcome), true);
            }
            if (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_OK ||
                result.bytesWritten != static_cast<std::uint32_t>(chunk.size()))
            {
                outcome.failureText = describeDdmaWriteFailure(result);
                return recordScratchFailure(session, physicalAddress, std::move(outcome), true);
            }

            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(chunk.size());
            return recordScratchFailure(session, physicalAddress, std::move(outcome), true);
        }

        // mergeChunkOutcome：把逐页结果里的告警位合并进总结果。
        // 告警位是"只要出现过一次就要保留"的语义，不能被后续成功页覆盖掉。
        void mergeChunkOutcome(AccessOutcome& total, const AccessOutcome& chunk)
        {
            total.scratchDirty = total.scratchDirty || chunk.scratchDirty;
            total.lostUpdateWindow = total.lostUpdateWindow || chunk.lostUpdateWindow;
            total.partial = total.partial || chunk.partial;
        }
    }

    namespace
    {
        // The UI owns the legacy reference. Background callers capture a value
        // while holding sessionStateMutex, never the UI-owned live object.
        DdmaSession& mutableCurrentDdmaSession()
        {
            static DdmaSession session;
            return session;
        }

        std::mutex& sessionStateMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::atomic<std::uint64_t>& mutableDdmaSessionGeneration()
        {
            static std::atomic<std::uint64_t> generation{ 0ULL };
            return generation;
        }
    }

    const DdmaSession& currentDdmaSession()
    {
        return mutableCurrentDdmaSession();
    }

    void setCurrentDdmaSession(const DdmaSession& session)
    {
        std::lock_guard<std::mutex> stateLock(sessionStateMutex());
        mutableCurrentDdmaSession() = session;
        ++mutableDdmaSessionGeneration();
    }

    std::uint64_t ddmaSessionGeneration()
    {
        return mutableDdmaSessionGeneration().load();
    }

    DdmaSessionSnapshot captureDdmaSession()
    {
        std::lock_guard<std::mutex> stateLock(sessionStateMutex());
        return { mutableCurrentDdmaSession(), mutableDdmaSessionGeneration().load() };
    }

    std::vector<DdmaScratchFailure> captureDdmaScratchFailures()
    {
        std::lock_guard<std::mutex> receiptLock(scratchReceiptMutex());
        return mutableDdmaScratchFailures();
    }

    std::recursive_mutex& ddmaTransactionMutex()
    {
        static std::recursive_mutex mutex;
        return mutex;
    }

    bool submitDdmaTransaction(std::function<void()> transaction)
    {
        // Register before queue admission: coordinated exit freezes new work
        // and waits for every accepted transaction before stopping R0.
        auto cleanupLease = ks::ui::AsyncCleanupRegistry::global().acquire();
        if (!cleanupLease) return false;
        // Construct the gate first so it outlives queue shutdown and draining.
        auto* const gate = &ddmaTransactionMutex();
        (void)captureDdmaSession(); // Session state must also outlive queue draining.
        (void)mutableDdmaScratchFailures();
        (void)scratchReceiptMutex();
        static ksword::evidence::SerialTransactionQueue queue(32);
        return queue.trySubmit([gate, cleanupLease = std::move(cleanupLease),
            transaction = std::move(transaction)]() mutable {
            // Move ownership into this invocation. Unlock the gate and destroy
            // the task's captured resources before returning the cleanup lease,
            // including when the transaction throws. Retaining a completed
            // callable cannot keep exit waiting or release its lease twice.
            auto acceptedCleanup = std::move(cleanupLease);
            (void)acceptedCleanup;
            auto work = std::move(transaction);
            transaction = nullptr;
            std::lock_guard<std::recursive_mutex> transactionLock(*gate);
            work();
        });
    }

    std::uint32_t ddmaTransferBytes()
    {
        return static_cast<std::uint32_t>(KSWORD_ARK_DDMA_TRANSFER_BYTES);
    }

    QString backendDisplayName(const MemoryAccessBackend backend)
    {
        if (backend == MemoryAccessBackend::Ddma)
        {
            return QStringLiteral("DDMA（磁盘 DMA）");
        }
        if (backend == MemoryAccessBackend::UserMode)
        {
            return QStringLiteral("R3（ReadProcessMemory）");
        }
        if (backend == MemoryAccessBackend::Hvm)
        {
            return QStringLiteral("HVM（私有页表窗口）");
        }
        return QStringLiteral("R0（驱动通道）");
    }

    namespace
    {
        // userModeReadVirtual：R3 通道。自己开句柄而不是借用调用方的，理由是
        // 这样四个页面对同一个 PID 得到的权限完全一致——借句柄的话，能不能读
        // 取决于调用方当初 OpenProcess 要了什么，同一条通道在不同页面上会有
        // 不同的失败面，而失败面正是用来区分三条通道的判据。
        AccessOutcome userModeReadVirtual(
            const std::uint32_t processId,
            const std::uint64_t virtualAddress,
            const std::uint64_t lengthBytes)
        {
            AccessOutcome outcome;
            if (virtualAddress >= kKernelSpaceStart)
            {
                outcome.failureText = QStringLiteral("R3 通道读不了内核地址：ReadProcessMemory 只能访问目标进程的用户态地址空间，请改用 R0 驱动通道。");
                return outcome;
            }
            const HANDLE processHandle = ::OpenProcess(
                PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                FALSE,
                static_cast<DWORD>(processId));
            if (processHandle == nullptr)
            {
                outcome.failureText = QStringLiteral("R3 通道打开进程失败，win32=%1。")
                    .arg(::GetLastError());
                return outcome;
            }

            QByteArray buffer(static_cast<qsizetype>(lengthBytes), '\0');
            SIZE_T bytesRead = 0;
            const BOOL readOk = ::ReadProcessMemory(
                processHandle,
                reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(virtualAddress)),
                buffer.data(),
                static_cast<SIZE_T>(lengthBytes),
                &bytesRead);
            // 错误码必须紧挨着调用取一次：后面的 CloseHandle 与 QString 拼接
            // 都可能改写线程的 last error。
            const DWORD readError = (readOk == FALSE) ? ::GetLastError() : ERROR_SUCCESS;
            ::CloseHandle(processHandle);

            if (bytesRead == 0)
            {
                outcome.failureText = QStringLiteral("R3 通道读取失败，win32=%1。")
                    .arg(readError);
                return outcome;
            }
            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(bytesRead);
            outcome.partial = (bytesRead < lengthBytes);
            outcome.data = buffer.left(static_cast<qsizetype>(bytesRead));
            return outcome;
        }

        // userModeWriteVirtual：R3 写。
        AccessOutcome userModeWriteVirtual(
            const std::uint32_t processId,
            const std::uint64_t virtualAddress,
            const QByteArray& payload)
        {
            AccessOutcome outcome;
            if (virtualAddress >= kKernelSpaceStart)
            {
                outcome.failureText = QStringLiteral(
                    "R3 通道写不了内核地址，请改用 R0 驱动通道。");
                return outcome;
            }
            const HANDLE processHandle = ::OpenProcess(
                PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                FALSE,
                static_cast<DWORD>(processId));
            if (processHandle == nullptr)
            {
                outcome.failureText = QStringLiteral("R3 通道打开进程失败，win32=%1。")
                    .arg(::GetLastError());
                return outcome;
            }
            SIZE_T bytesWritten = 0;
            const BOOL writeOk = ::WriteProcessMemory(
                processHandle,
                reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(virtualAddress)),
                payload.constData(),
                static_cast<SIZE_T>(payload.size()),
                &bytesWritten);
            const DWORD writeError = (writeOk == FALSE) ? ::GetLastError() : ERROR_SUCCESS;
            const BOOL flushed = bytesWritten == 0 ? TRUE : ::FlushInstructionCache(
                processHandle,
                reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(virtualAddress)),
                bytesWritten);
            const DWORD flushError = flushed == FALSE ? ::GetLastError() : ERROR_SUCCESS;
            ::CloseHandle(processHandle);

            if (bytesWritten == 0)
            {
                outcome.failureText = QStringLiteral("R3 通道写入失败，win32=%1。")
                    .arg(writeError);
                return outcome;
            }
            outcome.bytesDone = static_cast<std::uint64_t>(bytesWritten);
            if (flushed == FALSE)
            {
                outcome.failureText = QStringLiteral(
                    "R3 通道写入已完成，但刷新指令缓存失败，win32=%1。")
                    .arg(flushError);
                return outcome;
            }
            outcome.ok = true;
            outcome.partial = (static_cast<qsizetype>(bytesWritten) < payload.size());
            return outcome;
        }

        // userModeRejectPhysical：R3 没有任何访问物理地址的手段。这里干净地
        // 拒绝，而不是悄悄退回别的通道——退回去的话用户以为自己在用 R3，
        // 读到的却是 R0 的结果，两条通道的差异就再也看不出来了。
        AccessOutcome userModeRejectPhysical()
        {
            AccessOutcome outcome;
            outcome.failureText = QStringLiteral("R3 通道没有访问物理地址的手段，物理内存读写请选 R0 驱动通道或 DDMA。");
            return outcome;
        }

        // describeHvmMemoryStatus：把 R-1 内存状态翻成可据以行动的文案。
        QString describeHvmMemoryStatus(const unsigned long status)
        {
            switch (status)
            {
            case KSWORD_ARK_HVM_MEMORY_STATUS_INVALID_REQUEST:
                return QStringLiteral("驱动拒绝：请求字段无效。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_CONFIRMATION_REQUIRED:
                return QStringLiteral("驱动拒绝：缺少界面确认令牌。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_WINDOW_UNAVAILABLE:
                return QStringLiteral(
                    "私有页表窗口不可用：自映射基址没有标定出来（窗口可能落在大页映射里）。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_ADDRESS_INVALID:
                return QStringLiteral("地址无效。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_TRANSLATION_FAILED:
                return QStringLiteral("虚拟地址翻译失败，该页可能未驻留。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_ACCESS_FAILED:
                return QStringLiteral("访问目标页失败。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_BUSY:
                return QStringLiteral("私有窗口正被另一次访问占用，请重试。");
            case KSWORD_ARK_HVM_MEMORY_STATUS_PROCESS_LOOKUP_FAILED:
                return QStringLiteral("目标进程查找失败或已退出。");
            default:
                break;
            }
            return QStringLiteral("R-1 内存访问失败，status=%1。").arg(status);
        }

        // hvmTransfer：按 1024 字节上限切片跑一次 R-1 读或写。
        //
        // 切片是协议要求（KSWORD_ARK_HVM_MEMORY_MAX_BYTES），不是调优：请求走
        // METHOD_BUFFERED，整个请求结构要被快照进系统缓冲，所以单次必须小。
        //
        // usedDirectWindow 按**所有分片的与**汇总，不是取最后一片。任意一片退回
        // 了 MmCopyMemory，这一次访问作为整体就不再是"没走内存管理器"的——
        // 取最后一片会让一次半数退回的访问看上去完全没退回。
        // 严格读取要求驱动逐片使用私有窗口，仍核验返回标记以防旧驱动忽略要求；
        // 回退分片的字节与完成量均不计入结果，不自动改走标准 R0 通道。
        AccessOutcome hvmTransfer(
            const ksword::ark::DriverClient& client,
            const unsigned long readOperation,
            const unsigned long writeOperation,
            const std::uint32_t processId,
            const std::uint64_t baseAddress,
            const QByteArray* payload,
            const std::uint64_t lengthBytes,
            const bool requireHvmDirectWindow = false)
        {
            AccessOutcome outcome;
            const bool isWrite = (payload != nullptr);
            const std::uint64_t totalBytes =
                isWrite ? static_cast<std::uint64_t>(payload->size()) : lengthBytes;

            QByteArray collected;
            bool allDirectWindow = true;
            bool anyTransferred = false;
            std::uint64_t offset = 0ULL;

            for (; offset < totalBytes;)
            {
                const std::uint64_t remaining = totalBytes - offset;
                const unsigned long chunk = static_cast<unsigned long>(
                    (std::min)(remaining,
                        static_cast<std::uint64_t>(KSWORD_ARK_HVM_MEMORY_MAX_BYTES)));

                const ksword::ark::HvmMemoryResult result = client.hvmMemory(
                    isWrite ? writeOperation : readOperation,
                    baseAddress + offset,
                    0ULL,
                    chunk,
                    isWrite
                        ? reinterpret_cast<const unsigned char*>(payload->constData() + offset)
                        : nullptr,
                    /*requireWindow*/ isWrite || requireHvmDirectWindow,
                    /*uiConfirmed*/ true,
                    processId);

                if (!result.io.ok)
                {
                    outcome.failureText = result.unsupported
                        ? QStringLiteral("当前驱动不支持 R-1 内存访问，请更新 KswordARK 驱动。")
                        : QStringLiteral("R-1 内存访问通信失败：%1")
                              .arg(QString::fromStdString(result.io.message));
                    outcome.bytesDone = offset;
                    outcome.partial = offset != 0ULL;
                    outcome.data = collected;
                    return outcome;
                }
                const unsigned long status = result.response.status;
                if (status != KSWORD_ARK_HVM_MEMORY_STATUS_OK
                    && status != KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL)
                {
                    outcome.failureText = describeHvmMemoryStatus(status);
                    outcome.bytesDone = offset;
                    outcome.partial = offset != 0ULL;
                    outcome.data = collected;
                    return outcome;
                }

                if (!isWrite && requireHvmDirectWindow && result.response.usedDirectWindow == 0U)
                {
                    outcome.failureText = QStringLiteral(
                        "R-1 严格读取未使用私有页表窗口，已拒绝回退分片并停止访问；已读取 %1/%2 字节。")
                        .arg(offset).arg(totalBytes);
                    outcome.bytesDone = offset;
                    outcome.partial = offset != 0ULL;
                    outcome.data = collected;
                    return outcome;
                }
                const unsigned long done = result.response.bytesTransferred;
                if (done > chunk)
                {
                    outcome.failureText = QStringLiteral("R-1 返回的完成长度超出请求分片，已停止访问。");
                    outcome.bytesDone = offset;
                    outcome.partial = offset != 0ULL;
                    outcome.data = collected;
                    return outcome;
                }
                anyTransferred = anyTransferred || done != 0UL;
                if (result.response.usedDirectWindow == 0U)
                {
                    allDirectWindow = false;
                }
                if (!isWrite && done != 0UL)
                {
                    collected.append(
                        reinterpret_cast<const char*>(result.response.data),
                        static_cast<qsizetype>((std::min)(done,
                            static_cast<unsigned long>(KSWORD_ARK_HVM_MEMORY_MAX_BYTES))));
                }
                offset += done;
                // 驱动报完成 0 字节却又不报失败时必须停下，否则这里会空转。
                if (done == 0UL)
                {
                    outcome.partial = true;
                    break;
                }
                if (done != chunk || status == KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL)
                {
                    outcome.partial = true;
                    break;
                }
            }

            if (!anyTransferred)
            {
                outcome.failureText = QStringLiteral("R-1 内存访问未完成任何分片。");
                return outcome;
            }
            outcome.partial = outcome.partial || offset < totalBytes;
            outcome.ok = !isWrite || !outcome.partial;
            outcome.bytesDone = offset;
            outcome.data = collected;
            if (outcome.partial)
            {
                outcome.failureText = QStringLiteral("R-1 内存访问仅完成 %1/%2 字节，已停止访问。")
                    .arg(offset).arg(totalBytes);
            }
            // 没走成私有窗口时**不能静默成功**：那一次读走的正是我们想避开的
            // MmCopyMemory，拿它去跟 R0 比对什么都证明不了，而界面上两者看起来
            // 完全一样。借 lostUpdateWindow 之外没有合适字段，所以写进 failureText
            // 的同时保持 ok=true——数据是真的，只是它的独立性没有成立。
            if (!allDirectWindow)
            {
                outcome.failureText += QStringLiteral("注意：本次访问回退到了 MmCopyMemory（私有页表窗口未标定），因此它与 R0 通道不再是两条独立的路径，两者一致不能用来排除内存管理器被挂钩。");
            }
            return outcome;
        }
    }

    bool isHvmMemoryUsable(QString* const reasonOut)
    {
        const auto setReason = [reasonOut](const QString& text) {
            if (reasonOut != nullptr)
            {
                *reasonOut = text;
            }
        };

        const ksword::ark::DriverClient client;
        const ksword::ark::HvmMemoryResult result = client.hvmMemory(
            KSWORD_ARK_HVM_MEMORY_OP_QUERY_WINDOW,
            0ULL, 0ULL, 0UL, nullptr,
            /*requireWindow*/ false,
            /*uiConfirmed*/ true);
        if (!result.io.ok)
        {
            setReason(result.unsupported
                ? QStringLiteral("当前驱动不支持 R-1 内存访问。")
                : QStringLiteral("查询 R-1 内存窗口失败：%1")
                      .arg(QString::fromStdString(result.io.message)));
            return false;
        }
        if (result.response.windowReady == 0U)
        {
            // 窗口没标定出来时这条通道**仍然能返回数据**（回退 MmCopyMemory），
            // 但那样它就不再独立于 R0。这里判为不可用，免得用户在界面上看到
            // 两条通道一致就以为排除了挂钩。
            setReason(QStringLiteral("私有页表窗口未标定：自映射基址没有找到（窗口可能落在大页映射里）。此时访问会回退到 MmCopyMemory，与 R0 不再是独立的两条路径。"));
            return false;
        }
        setReason(QString());
        return true;
    }

    bool isKernelVirtualAddress(const std::uint64_t virtualAddress)
    {
        return virtualAddress >= kKernelSpaceStart;
    }

    bool isDdmaUsable(const DdmaSession& session, QString* const reasonOut)
    {
        // 判定顺序本身是判据的一部分（内核调试必须排在"还没配好"前面），
        // 因此顺序由共用纯函数决定，本函数只负责把结论翻成可展示的文案。
        const int gate = KswordArkDdmaEvaluateGate(
            session.configured ? 1 : 0,
            session.kernelDebuggerEnabled ? 1 : 0,
            session.scratchLbaValid ? 1 : 0,
            session.scratchAcknowledged ? 1 : 0);

        QString reason;
        switch (gate)
        {
        case KSWORD_ARK_DDMA_GATE_ALLOWED:
            if (reasonOut != nullptr)
            {
                reasonOut->clear();
            }
            return true;
        case KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED:
            reason = QStringLiteral(
                "尚未配置 DDMA 通道。请先到“内存 → DDMA”页探测磁盘并指定暂存扇区。");
            break;
        case KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER:
            // 这一条不是"用不了"，是"用了会蓝屏"：DDMA 用 MmMapIoSpace 映射
            // 普通 RAM，开着内核调试时会命中 MiShowBadMapper。
            reason = QStringLiteral(
                "本机启用了内核调试。DDMA 需要映射普通物理页，这种机器上会命中 "
                "MiShowBadMapper 直接蓝屏，因此禁止使用。请关闭内核调试后重试。");
            break;
        case KSWORD_ARK_DDMA_GATE_SCRATCH_LBA_MISSING:
            reason = QStringLiteral(
                "尚未指定暂存扇区 LBA。DDMA 必须借用磁盘扇区中转，本工具不提供默认值。");
            break;
        case KSWORD_ARK_DDMA_GATE_SCRATCH_NOT_ACKNOWLEDGED:
        default:
            reason = QStringLiteral(
                "尚未确认暂存扇区可被临时覆盖。请在 DDMA 页勾选确认后再使用。");
            break;
        }

        if (reasonOut != nullptr)
        {
            *reasonOut = reason;
        }
        return false;
    }

    AccessOutcome readPhysical(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint64_t physicalAddress,
        const std::uint64_t lengthBytes,
        const bool requireHvmDirectWindow)
    {
        std::unique_lock<std::recursive_mutex> transactionLock(ddmaTransactionMutex(), std::defer_lock);
        if (backend == MemoryAccessBackend::Ddma)
        {
            transactionLock.lock();
        }
        AccessOutcome outcome;
        if (lengthBytes == 0ULL)
        {
            outcome.failureText = QStringLiteral("读取长度为 0。");
            return outcome;
        }
        if (!isValidMemoryRange(physicalAddress, lengthBytes))
        {
            outcome.failureText = QStringLiteral("内存访问范围超出地址空间或缓冲区容量。");
            return outcome;
        }

        if (backend == MemoryAccessBackend::UserMode)
        {
            return userModeRejectPhysical();
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::Hvm)
        {
            return hvmTransfer(
                client,
                KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL,
                KSWORD_ARK_HVM_MEMORY_OP_WRITE_PHYSICAL,
                0U, physicalAddress, nullptr, lengthBytes, requireHvmDirectWindow);
        }

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            if (lengthBytes > kStandardPhysicalReadMax)
            {
                outcome.failureText = QStringLiteral(
                    "标准通道单次物理读上限 %1 字节，请缩小范围。")
                    .arg(kStandardPhysicalReadMax);
                return outcome;
            }
            const ksword::ark::PhysicalMemoryReadResult result = client.readPhysicalMemory(
                physicalAddress,
                static_cast<std::uint32_t>(lengthBytes),
                0UL);
            if (!result.io.ok)
            {
                outcome.failureText = QStringLiteral("物理内存读取失败：%1")
                    .arg(QString::fromStdString(result.io.message));
                return outcome;
            }
            const bool usable =
                (result.readStatus == KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_OK ||
                 result.readStatus == KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_PARTIAL);
            if (!usable || result.data.empty())
            {
                outcome.failureText = QStringLiteral(
                    "物理内存读取未返回可用数据，readStatus=%1。").arg(result.readStatus);
                return outcome;
            }
            outcome.ok = true;
            outcome.partial =
                (result.readStatus == KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_PARTIAL);
            outcome.bytesDone = static_cast<std::uint64_t>(result.data.size());
            outcome.data = QByteArray(
                reinterpret_cast<const char*>(result.data.data()),
                static_cast<qsizetype>(result.data.size()));
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        // DDMA 的传输粒度是一页，按页边界切片逐页读取。
        outcome.data.reserve(static_cast<qsizetype>(lengthBytes));
        std::uint64_t cursor = physicalAddress;
        std::uint64_t remaining = lengthBytes;
        while (remaining > 0ULL)
        {
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const std::uint64_t chunkLength =
                static_cast<std::uint64_t>(KswordArkDdmaChunkLength(cursor, remaining));

            const AccessOutcome chunk = ddmaReadOnePage(
                client, session, cursor, static_cast<std::uint32_t>(chunkLength));
            mergeChunkOutcome(outcome, chunk);
            if (!chunk.ok)
            {
                outcome.failureText = QStringLiteral("物理地址 %1：%2")
                    .arg(formatHex(cursor))
                    .arg(chunk.failureText);
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                return outcome;
            }
            outcome.data.append(chunk.data);
            if (chunk.scratchDirty)
            {
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                outcome.partial = outcome.bytesDone < lengthBytes;
                outcome.ok = !outcome.partial;
                outcome.failureText = QStringLiteral("DDMA 暂存扇区未能还原，已停止后续读取；已读取 %1/%2 字节。")
                    .arg(outcome.bytesDone).arg(lengthBytes);
                return outcome;
            }
            cursor += chunkLength;
            remaining -= chunkLength;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
        return outcome;
    }

    AccessOutcome writePhysical(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint64_t physicalAddress,
        const QByteArray& bytes,
        const bool forceApproved)
    {
        std::unique_lock<std::recursive_mutex> transactionLock(ddmaTransactionMutex(), std::defer_lock);
        if (backend == MemoryAccessBackend::Ddma)
        {
            transactionLock.lock();
        }
        AccessOutcome outcome;
        if (bytes.isEmpty())
        {
            outcome.failureText = QStringLiteral("写入长度为 0。");
            return outcome;
        }
        if (!isValidMemoryRange(physicalAddress, static_cast<std::uint64_t>(bytes.size())))
        {
            outcome.failureText = QStringLiteral("内存访问范围超出地址空间或缓冲区容量。");
            return outcome;
        }

        if (backend == MemoryAccessBackend::UserMode)
        {
            return userModeRejectPhysical();
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::Hvm)
        {
            return hvmTransfer(
                client,
                KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL,
                KSWORD_ARK_HVM_MEMORY_OP_WRITE_PHYSICAL,
                0U, physicalAddress, &bytes, 0ULL);
        }

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            // 标准物理写单次上限 4KB，超过按上限切片逐块提交。
            qsizetype offset = 0;
            while (offset < bytes.size())
            {
                const qsizetype chunkSize = std::min<qsizetype>(
                    static_cast<qsizetype>(kStandardPhysicalWriteMax),
                    bytes.size() - offset);
                const QByteArray chunk = bytes.mid(offset, chunkSize);
                const std::vector<std::uint8_t> payload(
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()) + chunk.size());

                unsigned long writeFlags = KSWORD_ARK_PHYSICAL_WRITE_FLAG_UI_CONFIRMED;
                if (forceApproved)
                {
                    writeFlags |= KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE;
                }
                const ksword::ark::PhysicalMemoryWriteResult result =
                    client.writePhysicalMemory(
                        physicalAddress + static_cast<std::uint64_t>(offset),
                        payload,
                        writeFlags);

                if (result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_FORCE_REQUIRED)
                {
                    outcome.forceRequired = true;
                    outcome.failureText = QStringLiteral("驱动要求对物理内存写入附加强制标志。");
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                const bool chunkOk = result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_OK &&
                    result.bytesWritten == static_cast<std::uint32_t>(chunk.size());
                if (!chunkOk)
                {
                    outcome.failureText = QStringLiteral(
                        "物理内存写入失败。地址 %1，writeStatus=%2；本轮已写入 %3 字节，"
                        "失败前的改动不会自动回滚。")
                        .arg(formatHex(physicalAddress + static_cast<std::uint64_t>(offset)))
                        .arg(result.writeStatus)
                        .arg(offset);
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                offset += chunkSize;
            }
            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        qsizetype offset = 0;
        while (offset < bytes.size())
        {
            const std::uint64_t cursor = physicalAddress + static_cast<std::uint64_t>(offset);
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const qsizetype chunkSize = static_cast<qsizetype>(KswordArkDdmaChunkLength(
                cursor,
                static_cast<std::uint64_t>(bytes.size() - offset)));
            const QByteArray chunk = bytes.mid(offset, chunkSize);

            const AccessOutcome chunkOutcome =
                ddmaWriteOnePage(client, session, cursor, chunk, forceApproved);
            mergeChunkOutcome(outcome, chunkOutcome);
            if (!chunkOutcome.ok)
            {
                outcome.forceRequired = chunkOutcome.forceRequired;
                outcome.failureText = QStringLiteral("物理地址 %1：%2 本轮已写入 %3 字节。")
                    .arg(formatHex(cursor))
                    .arg(chunkOutcome.failureText)
                    .arg(offset);
                outcome.bytesDone = static_cast<std::uint64_t>(offset);
                return outcome;
            }
            offset += chunkSize;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
        return outcome;
    }

    AccessOutcome readVirtual(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint32_t processId,
        const std::uint64_t virtualAddress,
        const std::uint64_t lengthBytes,
        const bool requireHvmDirectWindow)
    {
        std::unique_lock<std::recursive_mutex> transactionLock(ddmaTransactionMutex(), std::defer_lock);
        if (backend == MemoryAccessBackend::Ddma)
        {
            transactionLock.lock();
        }
        AccessOutcome outcome;
        if (lengthBytes == 0ULL)
        {
            outcome.failureText = QStringLiteral("读取长度为 0。");
            return outcome;
        }
        if (!isValidMemoryRange(virtualAddress, lengthBytes))
        {
            outcome.failureText = QStringLiteral("内存访问范围超出地址空间或缓冲区容量。");
            return outcome;
        }

        if (backend == MemoryAccessBackend::UserMode)
        {
            return userModeReadVirtual(processId, virtualAddress, lengthBytes);
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::Hvm)
        {
            return hvmTransfer(
                client,
                KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL,
                KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL,
                processId, virtualAddress, nullptr, lengthBytes, requireHvmDirectWindow);
        }

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            if (lengthBytes > kStandardVirtualReadMax)
            {
                outcome.failureText = QStringLiteral(
                    "标准通道单次虚拟读上限 %1 字节，请缩小范围。")
                    .arg(kStandardVirtualReadMax);
                return outcome;
            }
            unsigned long readFlags = KSWORD_ARK_MEMORY_READ_FLAG_ZERO_FILL_UNREADABLE;
            if (isKernelVirtualAddress(virtualAddress))
            {
                readFlags |= KSWORD_ARK_MEMORY_READ_FLAG_KERNEL_ADDRESS;
            }
            const ksword::ark::VirtualMemoryReadResult result = client.readVirtualMemory(
                processId,
                virtualAddress,
                static_cast<std::uint32_t>(lengthBytes),
                readFlags);
            if (!result.io.ok)
            {
                outcome.failureText = QStringLiteral("虚拟内存读取失败：%1")
                    .arg(QString::fromStdString(result.io.message));
                return outcome;
            }
            if (result.data.empty())
            {
                outcome.failureText = QStringLiteral(
                    "虚拟内存读取未返回数据，readStatus=%1。").arg(result.readStatus);
                return outcome;
            }
            // 状态必须按白名单认，与上面物理读那条保持同一种写法。原先这里只看
            // "data 非空"，于是 ZERO_FILLED 会被当成干净成功——而它的字面含义是
            // **整段都不可读、返回的全是补上去的零**。调用方拿到一片零字节，界面
            // 上与真读到一页零毫无区别。这与"两个后端都读到全零就报一致"属于同
            // 一类假读数：读取失败被装扮成了数据。
            if (result.readStatus != KSWORD_ARK_MEMORY_READ_STATUS_OK &&
                result.readStatus != KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY)
            {
                outcome.failureText =
                    (result.readStatus == KSWORD_ARK_MEMORY_READ_STATUS_ZERO_FILLED)
                    ? QStringLiteral("该范围整段都不可读，驱动返回的全部是补零数据。")
                    : QStringLiteral("虚拟内存读取未返回可用数据，readStatus=%1。")
                        .arg(result.readStatus);
                return outcome;
            }
            outcome.ok = true;
            outcome.partial =
                (result.readStatus == KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY);
            outcome.bytesDone = static_cast<std::uint64_t>(result.data.size());
            outcome.data = QByteArray(
                reinterpret_cast<const char*>(result.data.data()),
                static_cast<qsizetype>(result.data.size()));
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        // DDMA 只认物理地址，虚拟地址必须逐页翻译。翻译走的是 R0 既有的
        // 只读页表游走后端，这里不重新实现任何页表解析。
        outcome.data.reserve(static_cast<qsizetype>(lengthBytes));
        std::uint64_t cursor = virtualAddress;
        std::uint64_t remaining = lengthBytes;
        while (remaining > 0ULL)
        {
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const std::uint64_t chunkLength =
                static_cast<std::uint64_t>(KswordArkDdmaChunkLength(cursor, remaining));

            const ksword::ark::VirtualAddressTranslateResult translation =
                client.translateVirtualAddress(processId, cursor);
            if (!translation.io.ok || !translation.resolved)
            {
                // 未映射字节不是零值：不能把伪造的零交给搜索、比较或编辑器。
                outcome.failureText = QStringLiteral("虚拟地址 %1 无法翻译，DDMA 读取已停止；已读取 %2 字节。")
                    .arg(formatHex(cursor)).arg(outcome.data.size());
                outcome.partial = !outcome.data.isEmpty();
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                return outcome;
            }

            // translateVirtualAddress 返回的是整个映射的物理基址加页内偏移，
            // 大页情形下 pageSize 会大于 4KB，但物理地址本身已经算好了偏移，
            // 直接用即可；DDMA 的一次传输仍然只覆盖 4KB。
            const AccessOutcome chunk = ddmaReadOnePage(
                client,
                session,
                translation.physicalAddress,
                static_cast<std::uint32_t>(chunkLength));
            mergeChunkOutcome(outcome, chunk);
            if (!chunk.ok)
            {
                outcome.failureText = QStringLiteral("虚拟地址 %1（物理 %2）：%3")
                    .arg(formatHex(cursor))
                    .arg(formatHex(translation.physicalAddress))
                    .arg(chunk.failureText);
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                return outcome;
            }
            outcome.data.append(chunk.data);
            if (chunk.scratchDirty)
            {
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                outcome.partial = outcome.bytesDone < lengthBytes;
                outcome.ok = !outcome.partial;
                outcome.failureText = QStringLiteral("DDMA 暂存扇区未能还原，已停止后续读取；已读取 %1/%2 字节。")
                    .arg(outcome.bytesDone).arg(lengthBytes);
                return outcome;
            }
            cursor += chunkLength;
            remaining -= chunkLength;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
        return outcome;
    }

    AccessOutcome writeVirtual(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint32_t processId,
        const std::uint64_t virtualAddress,
        const QByteArray& bytes,
        const bool forceApproved)
    {
        std::unique_lock<std::recursive_mutex> transactionLock(ddmaTransactionMutex(), std::defer_lock);
        if (backend == MemoryAccessBackend::Ddma)
        {
            transactionLock.lock();
        }
        AccessOutcome outcome;
        if (bytes.isEmpty())
        {
            outcome.failureText = QStringLiteral("写入长度为 0。");
            return outcome;
        }
        if (!isValidMemoryRange(virtualAddress, static_cast<std::uint64_t>(bytes.size())))
        {
            outcome.failureText = QStringLiteral("内存访问范围超出地址空间或缓冲区容量。");
            return outcome;
        }

        if (backend == MemoryAccessBackend::UserMode)
        {
            return userModeWriteVirtual(processId, virtualAddress, bytes);
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::Hvm)
        {
            return hvmTransfer(
                client,
                KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL,
                KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL,
                processId, virtualAddress, &bytes, 0ULL);
        }

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            qsizetype offset = 0;
            while (offset < bytes.size())
            {
                const qsizetype chunkSize = std::min<qsizetype>(
                    static_cast<qsizetype>(kStandardVirtualWriteMax),
                    bytes.size() - offset);
                const QByteArray chunk = bytes.mid(offset, chunkSize);
                const std::vector<std::uint8_t> payload(
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()) + chunk.size());
                const std::uint64_t target =
                    virtualAddress + static_cast<std::uint64_t>(offset);

                unsigned long writeFlags = KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED;
                if (isKernelVirtualAddress(target))
                {
                    writeFlags |= KSWORD_ARK_MEMORY_WRITE_FLAG_KERNEL_ADDRESS;
                }
                if (forceApproved)
                {
                    writeFlags |= KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE;
                }
                const ksword::ark::VirtualMemoryWriteResult result =
                    client.writeVirtualMemory(processId, target, payload, writeFlags);

                if (result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_FORCE_REQUIRED)
                {
                    outcome.forceRequired = true;
                    outcome.failureText = QStringLiteral("驱动要求对本次写入附加强制标志。");
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                const bool chunkOk = result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_OK &&
                    result.bytesWritten == static_cast<std::uint32_t>(chunk.size());
                if (!chunkOk)
                {
                    outcome.failureText = QStringLiteral(
                        "虚拟内存写入失败。地址 %1，writeStatus=%2；本轮已写入 %3 字节。")
                        .arg(formatHex(target))
                        .arg(result.writeStatus)
                        .arg(offset);
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                offset += chunkSize;
            }
            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        qsizetype offset = 0;
        while (offset < bytes.size())
        {
            const std::uint64_t cursor = virtualAddress + static_cast<std::uint64_t>(offset);
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const qsizetype chunkSize = static_cast<qsizetype>(KswordArkDdmaChunkLength(
                cursor,
                static_cast<std::uint64_t>(bytes.size() - offset)));
            const QByteArray chunk = bytes.mid(offset, chunkSize);

            const ksword::ark::VirtualAddressTranslateResult translation =
                client.translateVirtualAddress(processId, cursor);
            if (!translation.io.ok || !translation.resolved)
            {
                // 写入路径上翻译失败必须整体停下。跳过一页继续写会造成
                // "看起来写成功了、其实中间有一段没写进去"的静默损坏。
                outcome.failureText = QStringLiteral(
                    "虚拟地址 %1 无法翻译成物理地址，DDMA 写入已停止；本轮已写入 %2 字节。")
                    .arg(formatHex(cursor))
                    .arg(offset);
                outcome.bytesDone = static_cast<std::uint64_t>(offset);
                return outcome;
            }

            const AccessOutcome chunkOutcome = ddmaWriteOnePage(
                client, session, translation.physicalAddress, chunk, forceApproved);
            mergeChunkOutcome(outcome, chunkOutcome);
            if (!chunkOutcome.ok)
            {
                outcome.forceRequired = chunkOutcome.forceRequired;
                outcome.failureText = QStringLiteral("虚拟地址 %1（物理 %2）：%3 本轮已写入 %4 字节。")
                    .arg(formatHex(cursor))
                    .arg(formatHex(translation.physicalAddress))
                    .arg(chunkOutcome.failureText)
                    .arg(offset);
                outcome.bytesDone = static_cast<std::uint64_t>(offset);
                return outcome;
            }
            offset += chunkSize;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
        return outcome;
    }
}
