#include "DmaProcessOpPage.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/SecondaryPageLayout.h"
#include "../UI/CodeTextEdit.h"
#include "../UI/ParameterWorkspace.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../theme.h"
#include "../Internationalization/LanguageManager.h"
#include "../../../shared/evidence/NumericTextParse.h"

#include <QCheckBox>
#include <QFutureWatcher>
#include <QPromise>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

using namespace Ksword::Evidence;

namespace ksword::memory_dock
{
    namespace
    {
        constexpr std::uint64_t kPageBytes = kDmaOpPageBytes;

        QString hex64(const std::uint64_t value)
        {
            return QStringLiteral("0x%1").arg(value, 16, 16, QChar('0'))
                .toUpper().replace(QStringLiteral("0X"), QStringLiteral("0x"));
        }

        QString hexDump(const std::vector<std::uint8_t>& bytes, const std::size_t limit)
        {
            QStringList parts;
            const std::size_t shown = (std::min)(bytes.size(), limit);
            for (std::size_t i = 0U; i < shown; ++i)
            {
                parts << QStringLiteral("%1").arg(bytes[i], 2, 16, QChar('0')).toUpper();
            }
            QString text = parts.join(QLatin1Char(' '));
            if (bytes.size() > shown)
            {
                text += QStringLiteral(" …（共 %1 字节）").arg(bytes.size());
            }
            return text;
        }

        // parseHexPayload：把 "90 48 31 C0" 这类文本解析成字节。
        //
        // 只接受成对的十六进制数位，分隔符随意。刻意不接受"0x" 前缀混排或十进制：
        // 载荷是机器码，一个被宽容解析成别的值的字节就是一条别的指令，而错在哪
        // 从结果上看不出来。
        bool parseHexPayload(const QString& text, std::vector<std::uint8_t>& out, QString& errorOut)
        {
            out.clear();
            QString compact;
            for (const QChar ch : text)
            {
                if (ch.isSpace() || ch == QLatin1Char(',') || ch == QLatin1Char('-'))
                {
                    continue;
                }
                if (!isxdigit(static_cast<unsigned char>(ch.toLatin1())))
                {
                    errorOut = QStringLiteral("载荷里出现了非十六进制字符：%1").arg(ch);
                    return false;
                }
                compact.append(ch);
            }
            if (compact.isEmpty())
            {
                errorOut = QStringLiteral("载荷为空。");
                return false;
            }
            if ((compact.size() % 2) != 0)
            {
                // 奇数位必然是少写了一位。补零会静默改变最后一个字节的值。
                errorOut = QStringLiteral("载荷的十六进制位数是奇数，缺了一位。");
                return false;
            }
            out.reserve(static_cast<std::size_t>(compact.size() / 2));
            for (int i = 0; i < compact.size(); i += 2)
            {
                bool ok = false;
                out.push_back(static_cast<std::uint8_t>(compact.mid(i, 2).toUInt(&ok, 16)));
                if (!ok)
                {
                    errorOut = QStringLiteral("载荷解析失败。");
                    return false;
                }
            }
            return true;
        }
    }

    enum class DmaOperationTone { Normal, Success, Warning, Error };

    // Recovery belongs to the process, not the requesting QWidget.
    struct DmaOperationLedger
    {
        std::mutex mutex;
        std::uint32_t processId = 0;
        std::uint64_t creationTime100ns = 0;
        std::uint64_t sequence = 0;
        bool pending = false;
        QFuture<void> future;
        std::vector<DmaOpRecord> records;
        QStringList log;
        QString status;
        DmaOperationTone tone = DmaOperationTone::Normal;
    };

    namespace
    {
        struct OperationRequest
        {
            std::uint32_t processId = 0;
            std::uint64_t creationTime100ns = 0;
            std::uint64_t virtualAddress = 0;
            ksword::memory_backend::DdmaSessionSnapshot session;
            std::vector<std::uint8_t> payload;
            bool injectPayload = false;
            bool forceApproved = false;
            bool unknownSharingApproved = false;
            bool restore = false;
            DmaOpRecord restoreRecord;
        };

        struct LedgerProjection
        {
            QStringList log;
            QString status;
            DmaOperationTone tone = DmaOperationTone::Normal;
            bool pending = false;
            bool hasRecords = false;
        };

        std::shared_ptr<DmaOperationLedger> ledgerFor(
            const std::uint32_t processId, const std::uint64_t creationTime100ns)
        {
            static std::mutex registryMutex;
            static std::vector<std::shared_ptr<DmaOperationLedger>> ledgers;
            const std::lock_guard<std::mutex> registryLock(registryMutex);
            for (auto it = ledgers.rbegin(); it != ledgers.rend(); ++it)
            {
                const std::lock_guard<std::mutex> ledgerLock((*it)->mutex);
                if ((*it)->processId == processId
                    && (*it)->creationTime100ns == creationTime100ns) return *it;
            }
            auto ledger = std::make_shared<DmaOperationLedger>();
            ledger->processId = processId;
            ledger->creationTime100ns = creationTime100ns;
            ledgers.push_back(ledger);
            return ledger;
        }

        LedgerProjection projectLedger(const std::shared_ptr<DmaOperationLedger>& ledger)
        {
            LedgerProjection projection;
            if (!ledger) return projection;
            const std::lock_guard<std::mutex> lock(ledger->mutex);
            projection.log = ledger->log;
            projection.status = ledger->status;
            projection.tone = ledger->tone;
            projection.pending = ledger->pending;
            projection.hasRecords = !ledger->records.empty();
            return projection;
        }

        void logLine(const std::shared_ptr<DmaOperationLedger>& ledger, const QString& line)
        {
            const std::lock_guard<std::mutex> lock(ledger->mutex);
            ledger->log.append(line);
        }

        void setStatus(const std::shared_ptr<DmaOperationLedger>& ledger,
            const QString& status, const DmaOperationTone tone)
        {
            const std::lock_guard<std::mutex> lock(ledger->mutex);
            ledger->status = status;
            ledger->tone = tone;
        }

        // This lease stays on the queue thread through final readback. R0-only
        // targets require an actual selection-time R0 creation time at every check.
        struct TargetLease
        {
            std::uint32_t processId = 0;
            std::uint64_t creationTime100ns = 0;
            HANDLE handle = nullptr;
            ~TargetLease() { if (handle != nullptr) CloseHandle(handle); }
            TargetLease() = default;
            TargetLease(const TargetLease&) = delete;
            TargetLease& operator=(const TargetLease&) = delete;

            static std::uint64_t nativeCreationTime(HANDLE process)
            {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
                return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32)
                    | static_cast<std::uint64_t>(created.dwLowDateTime);
            }

            bool matches() const
            {
                if (creationTime100ns == 0) return false;
                if (handle != nullptr)
                    return WaitForSingleObject(handle, 0) == WAIT_TIMEOUT
                        && nativeCreationTime(handle) == creationTime100ns;
                const ksword::ark::DriverClient client;
                const auto result = client.enumerateProcesses(0UL);
                if (!result.io.ok) return false;
                for (const auto& entry : result.entries)
                    if (entry.processId == processId)
                        return !(entry.flags & KSWORD_ARK_PROCESS_FLAG_TERMINATING_OR_EXITED)
                            && entry.creationTime100ns != 0
                            && entry.creationTime100ns == creationTime100ns;
                return false;
            }

            bool open(const OperationRequest& request)
            {
                processId = request.processId;
                creationTime100ns = request.creationTime100ns;
                if (creationTime100ns == 0) return false;
                handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                    FALSE, processId);
                if (handle != nullptr)
                {
                    const auto actual = nativeCreationTime(handle);
                    return actual != 0 && actual == creationTime100ns && matches();
                }
                return matches();
            }
        };

        QString identityFailure()
        {
            return QStringLiteral("Target process identity is unavailable or changed; no further write was issued. Backups are retained.");
        }

        bool sessionCurrent(const OperationRequest& request, QString& error)
        {
            if (request.session.generation != ksword::memory_backend::ddmaSessionGeneration())
            {
                error = QStringLiteral("DDMA session changed; read a new snapshot before retrying.");
                return false;
            }
            return ksword::memory_backend::isDdmaUsable(request.session.session, &error);
        }

        bool sameMapping(const OperationRequest& request, const TargetLease& target,
            const std::uint64_t pagePhysical, QString& error)
        {
            if (!sessionCurrent(request, error)) return false;
            if (!target.matches()) { error = identityFailure(); return false; }
            const ksword::ark::DriverClient client;
            const auto translated = client.translateVirtualAddress(request.processId,
                request.virtualAddress & ~(kPageBytes - 1ULL), 0UL);
            if (!translated.io.ok || !translated.resolved
                || translated.physicalAddress != pagePhysical)
            {
                error = QStringLiteral("Target page mapping changed or is unavailable; no further write was issued. Backups are retained.");
                return false;
            }
            if (!target.matches()) { error = identityFailure(); return false; }
            return sessionCurrent(request, error);
        }

        bool resolveTargetPage(const OperationRequest& request, TargetLease& target,
            std::uint64_t& pagePhysical, std::vector<std::uint8_t>& pageBytes, QString& error)
        {
            if (!sessionCurrent(request, error)) return false;
            if (!target.open(request)) { error = identityFailure(); return false; }
            const ksword::ark::DriverClient client;
            const auto translated = client.translateVirtualAddress(request.processId,
                request.virtualAddress & ~(kPageBytes - 1ULL), 0UL);
            if (!translated.io.ok || !translated.resolved)
            {
                error = translated.io.ok ? QStringLiteral("该页翻译不出物理地址，可能未驻留。")
                    : QStringLiteral("地址翻译失败：%1").arg(QString::fromStdString(translated.io.message));
                return false;
            }
            pagePhysical = translated.physicalAddress;
            if (!sessionCurrent(request, error)) return false;
            const auto read = ksword::memory_backend::readPhysical(
                ksword::memory_backend::MemoryAccessBackend::Ddma,
                request.session.session, pagePhysical, kPageBytes);
            if (read.scratchDirty)
            {
                error = QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe.");
                return false;
            }
            if (!read.ok || read.partial)
            {
                error = QStringLiteral("读取目标页失败：%1").arg(read.failureText);
                return false;
            }
            if (static_cast<std::uint64_t>(read.data.size()) != kPageBytes)
            {
                error = QStringLiteral("只读到 %1 / %2 字节，无法据此计划写入。")
                    .arg(read.data.size()).arg(kPageBytes);
                return false;
            }
            pageBytes.assign(reinterpret_cast<const std::uint8_t*>(read.data.constData()),
                reinterpret_cast<const std::uint8_t*>(read.data.constData()) + read.data.size());
            return sameMapping(request, target, pagePhysical, error);
        }

        DmaTargetSharing evaluateSharing(const OperationRequest& request,
            const std::uint64_t pagePhysical, QString& evidence)
        {
            using Clock = std::chrono::steady_clock;
            const auto started = Clock::now();
            const auto deadline = started + std::chrono::milliseconds(750);
            constexpr std::size_t maxDriverQueries = 64;
            std::size_t driverQueries = 0;
            const auto budgetAvailable = [&]() {
                return driverQueries < maxDriverQueries && Clock::now() < deadline;
            };
            const ksword::ark::DriverClient client;
            const auto pageVirtualAddress = request.virtualAddress & ~(kPageBytes - 1ULL);
            ++driverQueries;
            const auto self = client.queryVirtualMemory(request.processId, pageVirtualAddress,
                KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME);
            if (!self.io.ok || !(self.fieldFlags & KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT)
                || self.state != MEM_COMMIT)
            {
                evidence = QStringLiteral("查询目标区域失败：%1")
                    .arg(QString::fromStdString(self.io.message));
                return DmaTargetSharing::SharingUnknown;
            }
            if (self.type == MEM_PRIVATE)
            {
                evidence = QStringLiteral("区域类型 MEM_PRIVATE，不由节对象支撑。");
                return DmaTargetSharing::PrivateConfirmed;
            }
            const QString backingFile = QString::fromStdWString(self.mappedFileName);
            if (budgetAvailable())
            {
                ++driverQueries;
                const auto processes = client.enumerateProcesses(0UL);
                if (processes.io.ok)
                {
                    for (const auto& entry : processes.entries)
                    {
                        const auto otherPid = static_cast<std::uint32_t>(entry.processId);
                        if (otherPid == request.processId || otherPid == 0 || otherPid == 4) continue;
                        if (!budgetAvailable()) break;
                        ++driverQueries; // Includes failed queries and different backing files.
                        const auto other = client.queryVirtualMemory(otherPid, pageVirtualAddress,
                            KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_MAPPED_FILE_NAME);
                        if (!other.io.ok || !(other.fieldFlags & KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT)
                            || other.state != MEM_COMMIT || backingFile.isEmpty()
                            || QString::fromStdWString(other.mappedFileName).compare(
                                backingFile, Qt::CaseInsensitive) != 0) continue;
                        if (!budgetAvailable()) break;
                        ++driverQueries;
                        const auto otherPa = client.translateVirtualAddress(otherPid, pageVirtualAddress, 0UL);
                        if (!otherPa.io.ok || !otherPa.resolved) continue;
                        if (otherPa.physicalAddress == pagePhysical)
                        {
                            evidence = QStringLiteral("PID %1 的同一虚拟地址落在同一张物理页 %2 上，支撑文件 %3。")
                                .arg(otherPid).arg(hex64(pagePhysical)).arg(backingFile);
                            return DmaTargetSharing::SharedConfirmed;
                        }
                        // A differing COW page proves nothing about later candidates.
                    }
                }
            }
            evidence = QStringLiteral("Sharing check incomplete (%1 driver queries, %2 ms); sharing remains unknown for %3.")
                .arg(driverQueries)
                .arg(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count())
                .arg(backingFile.isEmpty() ? QStringLiteral("未知") : backingFile);
            return DmaTargetSharing::SharingUnknown;
        }

        std::vector<std::uint8_t> outcomeBytes(const ksword::memory_backend::AccessOutcome& outcome)
        {
            if (!outcome.ok || outcome.partial) return {};
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(outcome.data.constData()),
                reinterpret_cast<const std::uint8_t*>(outcome.data.constData()) + outcome.data.size());
        }

        ksword::memory_backend::AccessOutcome finalReadback(const OperationRequest& request,
            const std::uint64_t address, const std::size_t size)
        {
            // Completion is unconditional after mutation may have begun. Closing
            // the page or changing its selection never cancels this step.
            try
            {
                return ksword::memory_backend::readPhysical(
                    ksword::memory_backend::MemoryAccessBackend::Ddma,
                    request.session.session, address, static_cast<std::uint64_t>(size));
            }
            catch (...)
            {
                ksword::memory_backend::AccessOutcome failed;
                failed.failureText = QStringLiteral("DDMA transaction failed unexpectedly.");
                return failed;
            }
        }

        ksword::memory_backend::AccessOutcome writeOnce(const OperationRequest& request,
            const std::uint64_t address, const std::vector<std::uint8_t>& bytes)
        {
            try
            {
                return ksword::memory_backend::writePhysical(
                    ksword::memory_backend::MemoryAccessBackend::Ddma,
                    request.session.session, address,
                    QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())),
                    request.forceApproved);
            }
            catch (...)
            {
                ksword::memory_backend::AccessOutcome failed;
                failed.failureText = QStringLiteral("DDMA transaction failed unexpectedly.");
                return failed;
            }
        }

        void logReceipt(const std::shared_ptr<DmaOperationLedger>& ledger,
            const ksword::memory_backend::AccessOutcome& outcome)
        {
            logLine(ledger, QStringLiteral("DDMA write receipt: %1 bytes, partial=%2, scratchDirty=%3, lostUpdateWindow=%4.")
                .arg(outcome.bytesDone).arg(outcome.partial).arg(outcome.scratchDirty).arg(outcome.lostUpdateWindow));
            if (outcome.scratchDirty)
                logLine(ledger, QStringLiteral("   严重告警：暂存扇区未能还原，磁盘上留下了脏扇区。"));
        }

        QString verificationDetail(const DmaWriteVerification& verification)
        {
            return verification.readbackTooShort
                ? QStringLiteral("读回只有 %1 字节").arg(verification.comparedBytes)
                : QStringLiteral("第 %1 字节期望 %2、实际 %3").arg(verification.firstMismatchOffset)
                    .arg(QStringLiteral("%1").arg(verification.expectedByte, 2, 16, QChar('0')).toUpper())
                    .arg(QStringLiteral("%1").arg(verification.actualByte, 2, 16, QChar('0')).toUpper());
        }

        void logVerification(const std::shared_ptr<DmaOperationLedger>& ledger,
            const DmaWriteVerification& verification)
        {
            logLine(ledger, verification.matched
                ? QStringLiteral("   读回校验通过（%1 字节一致）。").arg(verification.comparedBytes)
                : QStringLiteral("   读回校验失败：%1").arg(verificationDetail(verification)));
        }

        void runWrite(const OperationRequest& request, const std::shared_ptr<DmaOperationLedger>& ledger)
        {
            TargetLease target;
            std::uint64_t pagePhysical = 0;
            std::vector<std::uint8_t> pageBytes;
            QString error;
            if (!resolveTargetPage(request, target, pagePhysical, pageBytes, error))
            {
                setStatus(ledger, error, DmaOperationTone::Warning);
                return;
            }
            QString evidence;
            const auto sharing = evaluateSharing(request, pagePhysical, evidence);
            logLine(ledger, QStringLiteral("── 共享性检查：%1").arg(QString::fromUtf8(DmaTargetSharingName(sharing))));
            logLine(ledger, QStringLiteral("   依据：%1").arg(evidence));
            if (sharing == DmaTargetSharing::SharedConfirmed)
            {
                setStatus(ledger, QStringLiteral("已拒绝：目标页确认被其它进程共享。%1 DMA 不触发写时复制，写入会波及每一个映射这张页的进程。请改用 R-1 注入（它把载荷放在影子页里，作用域限定在单个进程），或选一张进程私有的页。")
                    .arg(evidence), DmaOperationTone::Error);
                return;
            }
            if (sharing == DmaTargetSharing::SharingUnknown && !request.unknownSharingApproved)
            {
                setStatus(ledger, QStringLiteral("已拒绝：无法确认目标页是否被其它进程共享。%1 没找到第二个映射它的进程不等于没有。要继续，请勾选下面那一项并自行承担波及其它进程的可能。")
                    .arg(evidence), DmaOperationTone::Warning);
                return;
            }
            const auto plan = request.injectPayload
                ? PlanPayloadIntoCave(pageBytes, request.payload, kDmaOpMinCaveBytes)
                : PlanBytesAtOffset(pageBytes, static_cast<std::size_t>(request.virtualAddress & (kPageBytes - 1ULL)), UndefinedInstructionBytes());
            const QString description = request.injectPayload
                ? QStringLiteral("注入载荷 %1 字节").arg(request.payload.size()) : QStringLiteral("写 UD2");
            if (plan.status != DmaOpPlanStatus::Ok)
            {
                setStatus(ledger, QStringLiteral("计划被拒绝：%1").arg(QString::fromUtf8(DmaOpPlanStatusName(plan.status))),
                    DmaOperationTone::Warning);
                return;
            }
            const auto writeAddress = pagePhysical + static_cast<std::uint64_t>(plan.offsetInPage);
            if (!sameMapping(request, target, pagePhysical, error))
            {
                setStatus(ledger, error, DmaOperationTone::Warning);
                return;
            }
            const auto before = ksword::memory_backend::readPhysical(
                ksword::memory_backend::MemoryAccessBackend::Ddma, request.session.session,
                writeAddress, static_cast<std::uint64_t>(plan.originalBytes.size()));
            if (before.scratchDirty)
            {
                setStatus(ledger, QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe."), DmaOperationTone::Error);
                return;
            }
            if (!before.ok || !VerifyWriteReadback(plan.originalBytes, outcomeBytes(before)).matched)
            {
                setStatus(ledger, QStringLiteral("Target bytes changed or could not be read completely; no write was issued. Backups are retained."), DmaOperationTone::Warning);
                return;
            }
            if (!sameMapping(request, target, pagePhysical, error))
            {
                setStatus(ledger, error, DmaOperationTone::Warning);
                return;
            }
            logLine(ledger, QStringLiteral("── %1").arg(description));
            logLine(ledger, QStringLiteral("   目标 VA %1，页物理 %2，页内偏移 0x%3")
                .arg(hex64(request.virtualAddress)).arg(hex64(pagePhysical))
                .arg(QString::number(plan.offsetInPage, 16).toUpper()));
            logLine(ledger, QStringLiteral("   备份（原始字节）：%1").arg(hexDump(plan.originalBytes, 32U)));
            logLine(ledger, QStringLiteral("   将写入：%1").arg(hexDump(plan.bytesToWrite, 32U)));
            DmaOpRecord record;
            record.processId = request.processId;
            record.creationTime100ns = target.creationTime100ns;
            record.session = request.session;
            record.virtualAddress = (request.virtualAddress & ~(kPageBytes - 1ULL)) + plan.offsetInPage;
            record.physicalAddress = writeAddress;
            record.offsetInPage = plan.offsetInPage;
            record.writtenBytes = plan.bytesToWrite;
            record.originalBytes = plan.originalBytes;
            record.description = description;
            {
                const std::lock_guard<std::mutex> lock(ledger->mutex);
                // Allocate/save before mutation. Failed/partial transport receipts
                // must never erase the only recovery bytes.
                record.recordId = ledger->sequence;
                ledger->records.push_back(record);
            }
            const auto written = writeOnce(request, writeAddress, plan.bytesToWrite);
            const bool completeWrite = written.ok && !written.partial
                && written.bytesDone == static_cast<std::uint64_t>(plan.bytesToWrite.size());
            const QString writeFailure = written.failureText.isEmpty()
                ? QStringLiteral("DDMA transaction returned incomplete data.") : written.failureText;
            // Read back before formatting/logging after a possible physical write.
            const auto readback = finalReadback(request, writeAddress, plan.bytesToWrite.size());
            const auto verification = VerifyWriteReadback(plan.bytesToWrite, outcomeBytes(readback));
            const bool contextValid = sameMapping(request, target, pagePhysical, error);
            logReceipt(ledger, written);
            logVerification(ledger, verification);
            if (readback.scratchDirty)
                logLine(ledger, QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe."));
            if (!completeWrite) logLine(ledger, QStringLiteral("   写入失败：%1").arg(writeFailure));
            {
                const std::lock_guard<std::mutex> lock(ledger->mutex);
                for (auto& saved : ledger->records)
                    if (saved.recordId == record.recordId)
                        saved.verified = completeWrite && verification.matched && contextValid && !written.scratchDirty && !readback.scratchDirty;
            }
            if (written.scratchDirty || readback.scratchDirty)
            {
                setStatus(ledger, QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe."), DmaOperationTone::Error);
            }
            else if (!contextValid)
            {
                logLine(ledger, error);
                setStatus(ledger, error, DmaOperationTone::Error);
            }
            else if (!completeWrite)
                setStatus(ledger, QStringLiteral("写入失败：%1").arg(writeFailure), DmaOperationTone::Error);
            else if (verification.matched)
            {
                setStatus(ledger, QStringLiteral("%1 完成并通过读回校验。注意：DMA 没有触发点，载荷要等目标自己执行到；真页已被修改，记得用“还原上一次写入”。")
                    .arg(description), written.scratchDirty ? DmaOperationTone::Error : DmaOperationTone::Success);
            }
            else
            {
                const QString detail = verificationDetail(verification);
                setStatus(ledger, QStringLiteral("%1 的写入命令被接受，但读回校验失败（%2）。目标页当前状态不确定，备份已记录，建议立刻还原。")
                    .arg(description).arg(detail), DmaOperationTone::Error);
            }
        }

        void runRestore(const OperationRequest& request, const std::shared_ptr<DmaOperationLedger>& ledger)
        {
            const auto& record = request.restoreRecord;
            TargetLease target;
            QString error;
            const auto pagePhysical = record.physicalAddress - record.offsetInPage;
            if (!target.open(request))
            {
                setStatus(ledger, identityFailure(), DmaOperationTone::Error);
                return;
            }
            if (record.session.session.diskIndex != request.session.session.diskIndex
                || record.session.session.deviceName != request.session.session.deviceName)
            {
                setStatus(ledger, QStringLiteral("DDMA session changed; read a new snapshot before retrying."), DmaOperationTone::Warning);
                return;
            }
            if (!sameMapping(request, target, pagePhysical, error))
            {
                setStatus(ledger, error, DmaOperationTone::Error);
                return;
            }
            const auto current = ksword::memory_backend::readPhysical(
                ksword::memory_backend::MemoryAccessBackend::Ddma, request.session.session,
                record.physicalAddress, static_cast<std::uint64_t>(record.originalBytes.size()));
            const auto bytes = outcomeBytes(current);
            bool expected = bytes.size() == record.originalBytes.size();
            for (std::size_t i = 0; expected && i < bytes.size(); ++i)
                expected = bytes[i] == record.originalBytes[i] || bytes[i] == record.writtenBytes[i];
            if (current.scratchDirty)
            {
                setStatus(ledger, QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe."), DmaOperationTone::Error);
                return;
            }
            if (!current.ok || !expected)
            {
                setStatus(ledger, QStringLiteral("Target bytes changed or could not be read completely; no write was issued. Backups are retained."), DmaOperationTone::Warning);
                return;
            }
            if (!sameMapping(request, target, pagePhysical, error))
            {
                setStatus(ledger, error, DmaOperationTone::Error);
                return;
            }
            logLine(ledger, QStringLiteral("── 还原 %1").arg(record.description));
            const auto written = writeOnce(request, record.physicalAddress, record.originalBytes);
            const bool completeWrite = written.ok && !written.partial
                && written.bytesDone == static_cast<std::uint64_t>(record.originalBytes.size());
            const QString writeFailure = written.failureText.isEmpty()
                ? QStringLiteral("DDMA transaction returned incomplete data.") : written.failureText;
            const auto readback = finalReadback(request, record.physicalAddress, record.originalBytes.size());
            const auto verification = VerifyWriteReadback(record.originalBytes, outcomeBytes(readback));
            const bool contextValid = sameMapping(request, target, pagePhysical, error);
            logReceipt(ledger, written);
            logVerification(ledger, verification);
            if (readback.scratchDirty)
                logLine(ledger, QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe."));
            if (written.scratchDirty || readback.scratchDirty)
            {
                setStatus(ledger, QStringLiteral("DDMA scratch sectors could not be restored; further DDMA operations are unsafe."), DmaOperationTone::Error);
                return;
            }
            if (!contextValid)
            {
                logLine(ledger, error);
                setStatus(ledger, error, DmaOperationTone::Error);
                return;
            }
            if (!completeWrite)
            {
                logLine(ledger, QStringLiteral("   还原失败：%1").arg(writeFailure));
                setStatus(ledger, QStringLiteral("还原失败：%1。备份仍然保留在日志里。")
                    .arg(writeFailure), DmaOperationTone::Error);
                return;
            }
            if (!verification.matched)
            {
                logLine(ledger, QStringLiteral("   还原后的读回校验失败，目标页仍处于未知状态。"));
                setStatus(ledger, QStringLiteral("还原命令被接受，但读回校验失败。目标页仍处于未知状态，备份保留在日志里。"), DmaOperationTone::Error);
                return;
            }
            {
                const std::lock_guard<std::mutex> lock(ledger->mutex);
                // Remove only the exact authorized top record.
                if (!ledger->records.empty() && ledger->records.back().recordId == record.recordId)
                    ledger->records.pop_back();
            }
            logLine(ledger, QStringLiteral("   还原完成并通过读回校验。"));
            setStatus(ledger, QStringLiteral("已还原并通过读回校验。"),
                written.scratchDirty ? DmaOperationTone::Error : DmaOperationTone::Success);
        }

        bool queueOperation(OperationRequest request, const std::shared_ptr<DmaOperationLedger>& ledger)
        {
            auto promise = std::make_shared<QPromise<void>>();
            promise->start();
            {
                const std::lock_guard<std::mutex> lock(ledger->mutex);
                if (ledger->pending) return false;
                if (request.restore)
                {
                    if (ledger->records.empty()) return false;
                    request.restoreRecord = ledger->records.back();
                    request.processId = request.restoreRecord.processId;
                    request.creationTime100ns = request.restoreRecord.creationTime100ns;
                    request.virtualAddress = request.restoreRecord.virtualAddress;
                }
                ledger->pending = true;
                ++ledger->sequence;
                ledger->future = promise->future();
                ledger->status = QStringLiteral("DDMA operation is running; accepted transactions finish and retain recovery records after this window closes.");
                ledger->tone = DmaOperationTone::Normal;
            }
            bool accepted = false;
            try
            {
                accepted = ksword::memory_backend::submitDdmaTransaction(
                [request = std::move(request), ledger, promise]() {
                    struct Completion
                    {
                        std::shared_ptr<DmaOperationLedger> ledger;
                        std::shared_ptr<QPromise<void>> promise;
                        ~Completion()
                        {
                            { const std::lock_guard<std::mutex> lock(ledger->mutex); ledger->pending = false; }
                            promise->finish();
                        }
                    } completion{ledger, promise};
                    try
                    {
                        if (request.restore) runRestore(request, ledger);
                        else runWrite(request, ledger);
                    }
                    catch (...)
                    {
                        setStatus(ledger, QStringLiteral("DDMA transaction failed unexpectedly."), DmaOperationTone::Error);
                    }
                    QString finalStatus;
                    { const std::lock_guard<std::mutex> lock(ledger->mutex); finalStatus = ledger->status; }
                    logLine(ledger, finalStatus);
                });
            }
            catch (...)
            {
                setStatus(ledger, QStringLiteral("DDMA transaction failed unexpectedly."), DmaOperationTone::Error);
                { const std::lock_guard<std::mutex> lock(ledger->mutex); ledger->pending = false; }
                promise->finish();
                return false;
            }
            if (!accepted)
            {
                setStatus(ledger, QStringLiteral("DDMA transaction queue is full; wait for the current operation and retry."), DmaOperationTone::Warning);
                {
                    const std::lock_guard<std::mutex> lock(ledger->mutex);
                    ledger->pending = false;
                }
                promise->finish();
            }
            return accepted;
        }
    }

    DmaProcessOpPage::DmaProcessOpPage(QWidget* const parent)
        : QWidget(parent)
    {
        buildUi();
        wireSignals();
        refreshChannelAvailability();
        updateActionState();
    }

    DmaProcessOpPage::~DmaProcessOpPage() = default;

    void DmaProcessOpPage::buildUi()
    {
        QVBoxLayout* root = new QVBoxLayout(this);
        ks::ui::StyleSecondaryContentLayout(root);
        // 参数和风险确认独立滚动，执行/还原固定，完整校验日志留在结果区。
        auto* workspace = new ks::ui::ParameterWorkspace(this, 480);
        root->addWidget(workspace);

        QLabel* intro = new QLabel(this);
        intro->setWordWrap(true);
        intro->setTextInteractionFlags(Qt::TextSelectableByMouse);
        intro->setText(QStringLiteral("通过 DDMA 直接往目标进程的物理页里写字节。与“R-1 注入”不是同一件事：R-1 把载荷放在影子页里、真页从头到尾没被改过，还有一个武装好的触发点；DMA 一样都没有。这里改的是**真页**，任何读取路径都看得见（包括本工具自己的 R3/R0/HVM）；而且**没有触发**，载荷写完就躺在那里等目标自己执行到，写进一个永远不会被执行的位置等于什么都没做。每一次写入都强制先备份、写完立刻读回校验——DDMA 写入没有任何自证，驱动报成功只说明命令被接受，不说明物理页真的变了。"));
        workspace->parameters()->addWidget(intro);

        m_targetLabel = new QLabel(QStringLiteral("未附加进程。"), this);
        m_targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_targetLabel->setWordWrap(true);
        workspace->parameters()->addWidget(m_targetLabel);

        // 参数和确认条件共用平面分区，日志保留独立伸缩空间。
        auto* inputSection = new QGroupBox(QStringLiteral("操作参数"), this);
        ks::ui::StyleParameterSection(inputSection);
        auto* inputLayout = new QVBoxLayout(inputSection);
        inputLayout->setSpacing(8);
        auto* form = new QFormLayout();
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_addressEdit = new QLineEdit(this);
        m_addressEdit->setPlaceholderText(QStringLiteral("目标进程里的地址，默认十六进制"));
        m_addressEdit->setClearButtonEnabled(true);
        m_addressEdit->setToolTip(QStringLiteral("注入时这个地址只用来定位所在的页，载荷会被排进页内的空隙里；写 UD2 时写的就是这个地址本身。"));
        form->addRow(QStringLiteral("目标虚拟地址"), m_addressEdit);

        m_payloadEdit = new QLineEdit(this);
        m_payloadEdit->setPlaceholderText(QStringLiteral("例如 90 48 31 C0 C3"));
        m_payloadEdit->setClearButtonEnabled(true);
        m_payloadEdit->setToolTip(QStringLiteral("位置无关的机器码。只接受成对的十六进制数位，分隔符随意；位数为奇数会被拒绝而不是补零——补零会静默改变最后一个字节，也就是改变最后一条指令。"));
        form->addRow(QStringLiteral("载荷（十六进制）"), m_payloadEdit);
        inputLayout->addLayout(form);

        m_forceCheck = new QCheckBox(QStringLiteral("附加 FORCE 标志（DDMA 写入要求）"), this);
        m_forceCheck->setToolTip(QStringLiteral("驱动对 DDMA 写入要求显式的强制标志，缺了会被拒绝。"));
        m_acknowledgeCheck = new QCheckBox(
            QStringLiteral("我确认这会修改目标进程的真实内存页，并且由我负责还原"), this);
        inputLayout->addWidget(m_forceCheck);
        m_unknownSharingCheck = new QCheckBox(
            QStringLiteral("目标页的共享性无法确认时仍然写入（后果可能波及其它进程）"), this);
        m_unknownSharingCheck->setToolTip(QStringLiteral("只在“无法确认”时起作用。已经确认被其它进程共享的页没有任何开关能解锁——往一张共享的映像页写字节会打到每一个映射它的进程。"));
        inputLayout->addWidget(m_acknowledgeCheck);
        inputLayout->addWidget(m_unknownSharingCheck);
        workspace->parameters()->addWidget(inputSection);

        // 三个长动作纵向排列，避免在窄参数栏中挤掉“还原”入口。
        auto* actions = new QVBoxLayout();
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(8);
        m_injectButton = new QPushButton(QStringLiteral("注入载荷到页内空隙"), this);
        m_injectButton->setToolTip(QStringLiteral("在目标地址所在页里找一段至少 64 字节的填充（只认 0x00 与 0xCC），把载荷排进去。找不到足够长的空隙会拒绝，而不是凑合写在一段短的上——那与直接覆盖真代码没有区别。"));
        m_ud2Button = new QPushButton(QStringLiteral("写 UD2（让目标崩掉）"), this);
        m_ud2Button->setToolTip(QStringLiteral("往目标地址写两字节 0F 0B。它不是一个可靠的结束：目标可能带异常处理器把 #UD 吞掉，生效与否还取决于那段代码会不会被执行到，而且会留下崩溃转储。"));
        m_restoreButton = new QPushButton(QStringLiteral("还原上一次写入"), this);
        actions->addWidget(m_injectButton);
        actions->addWidget(m_ud2Button);
        actions->addWidget(m_restoreButton);
        workspace->actions()->addLayout(actions, 1);
        ks::ui::NormalizeToolbarControl(m_injectButton);
        ks::ui::NormalizeToolbarControl(m_ud2Button);
        ks::ui::NormalizeToolbarControl(m_restoreButton);

        m_channelHintLabel = new QLabel(this);
        m_channelHintLabel->setWordWrap(true);
        workspace->parameters()->addWidget(m_channelHintLabel);

        m_statusLabel = new QLabel(QStringLiteral("尚未执行。"), this);
        m_statusLabel->setWordWrap(true);
        m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->addWidget(m_statusLabel);

        m_logText = new CodeTextEdit(this);
        static_cast<CodeTextEdit*>(m_logText)->setSyntaxLanguage(CodeTextEdit::SyntaxLanguage::PlainText);
        m_logText->setReadOnly(true);
        m_logText->setPlaceholderText(QStringLiteral("每一次写入的计划、备份与读回校验结果都会记在这里。备份是还原的唯一依据，别清空它。"));
        workspace->results()->addWidget(m_logText, 1);
        workspace->finish();
    }

    void DmaProcessOpPage::wireSignals()
    {
        connect(m_injectButton, &QPushButton::clicked, this, [this]() { performWrite(true); });
        connect(m_ud2Button, &QPushButton::clicked, this, [this]() { performWrite(false); });
        connect(m_restoreButton, &QPushButton::clicked, this, [this]() { restoreLastWrite(); });
        const auto gate = [this](bool) { updateActionState(); };
        connect(m_forceCheck, &QCheckBox::toggled, this, gate);
        connect(m_acknowledgeCheck, &QCheckBox::toggled, this, gate);
        connect(m_unknownSharingCheck, &QCheckBox::toggled, this, gate);
        connect(m_addressEdit, &QLineEdit::textChanged, this,
            [this](const QString&) { updateActionState(); });
    }

    void DmaProcessOpPage::setAttachedProcess(const std::uint32_t processId,
        const QString& processName, const std::uint64_t expectedCreationTime100ns)
    {
        ++m_targetGeneration;
        m_attachedPid = processId;
        m_attachedProcessName = processName;
        m_expectedCreationTime100ns = expectedCreationTime100ns;
        m_ledger = processId == 0 ? nullptr : ledgerFor(processId, expectedCreationTime100ns);
        m_watchedOperation = 0;
        m_projectedLogLines = 0;
        m_logText->clear();
        m_targetLabel->setText(processId == 0U ? QStringLiteral("未附加进程。")
            : QStringLiteral("目标：%1 [PID:%2]").arg(processName).arg(processId));
        synchronizeLedger();
        followPendingOperation();
    }

    void DmaProcessOpPage::refreshChannelAvailability()
    {
        QString reason;
        const auto session = ksword::memory_backend::captureDdmaSession();
        const bool usable = ksword::memory_backend::isDdmaUsable(session.session, &reason);
        m_channelHintLabel->setText(usable ? QStringLiteral("DDMA 通道就绪。")
            : QStringLiteral("DDMA 暂不可用：%1。本页的每一步都要经这条通道。").arg(reason));
        m_channelHintLabel->setStyleSheet(QStringLiteral("color:%1;")
            .arg(usable ? KswordTheme::SuccessHex() : KswordTheme::WarningHex()));
        synchronizeLedger();
        followPendingOperation();
    }

    void DmaProcessOpPage::updateActionState()
    {
        QString reason;
        const auto session = ksword::memory_backend::captureDdmaSession();
        const bool channelReady = ksword::memory_backend::isDdmaUsable(session.session, &reason);
        const auto projection = projectLedger(m_ledger);
        const bool ready = channelReady && !projection.pending && m_attachedPid != 0
            && !m_addressEdit->text().trimmed().isEmpty()
            && m_forceCheck->isChecked() && m_acknowledgeCheck->isChecked();
        m_injectButton->setEnabled(ready);
        m_ud2Button->setEnabled(ready);
        m_restoreButton->setEnabled(channelReady && !projection.pending && projection.hasRecords);
        m_addressEdit->setEnabled(!projection.pending);
        m_payloadEdit->setEnabled(!projection.pending);
        m_forceCheck->setEnabled(!projection.pending);
        m_acknowledgeCheck->setEnabled(!projection.pending);
        m_unknownSharingCheck->setEnabled(!projection.pending);
    }

    void DmaProcessOpPage::synchronizeLedger()
    {
        const auto projection = projectLedger(m_ledger);
        for (; m_projectedLogLines < projection.log.size(); ++m_projectedLogLines)
            m_logText->appendPlainText(ks::i18n::sourceText(projection.log.at(m_projectedLogLines)));
        m_statusLabel->setText(projection.status.isEmpty() ? QStringLiteral("尚未执行。")
            : ks::i18n::sourceText(projection.status));
        switch (projection.tone)
        {
        case DmaOperationTone::Success:
            m_statusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::SuccessHex())); break;
        case DmaOperationTone::Warning:
            m_statusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::WarningHex())); break;
        case DmaOperationTone::Error:
            m_statusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::ErrorHex())); break;
        case DmaOperationTone::Normal: m_statusLabel->setStyleSheet(QString()); break;
        }
        updateActionState();
    }

    void DmaProcessOpPage::followPendingOperation()
    {
        if (!m_ledger) return;
        QFuture<void> future;
        {
            const std::lock_guard<std::mutex> lock(m_ledger->mutex);
            if (m_ledger->sequence == 0 || m_watchedOperation == m_ledger->sequence) return;
            m_watchedOperation = m_ledger->sequence;
            future = m_ledger->future;
        }
        const auto ledger = m_ledger;
        const auto generation = m_targetGeneration;
        auto* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, ledger, generation]() {
            watcher->deleteLater();
            if (generation == m_targetGeneration && ledger == m_ledger) synchronizeLedger();
        });
        watcher->setFuture(future);
    }

    void DmaProcessOpPage::performWrite(const bool injectPayload)
    {
        if (!m_ledger || projectLedger(m_ledger).pending) return;
        OperationRequest request;
        request.processId = m_attachedPid;
        request.creationTime100ns = m_expectedCreationTime100ns;
        request.session = ksword::memory_backend::captureDdmaSession();
        request.injectPayload = injectPayload;
        request.forceApproved = m_forceCheck->isChecked();
        request.unknownSharingApproved = m_unknownSharingCheck->isChecked();
        if (!request.forceApproved || !m_acknowledgeCheck->isChecked() || request.processId == 0) return;
        if (request.creationTime100ns == 0)
        {
            setStatus(m_ledger, identityFailure(), DmaOperationTone::Warning);
            synchronizeLedger();
            return;
        }
        const auto parsed = ksword::evidence::ParseNumericText(m_addressEdit->text().trimmed().toStdString(),
            ksword::evidence::NumericTextDefaultRadix::Hexadecimal);
        QString error;
        if (!parsed.ok) error = QStringLiteral("目标地址解析失败。无前缀按十六进制解释。");
        else if (injectPayload) parseHexPayload(m_payloadEdit->text(), request.payload, error);
        if (!error.isEmpty())
        {
            m_statusLabel->setText(ks::i18n::sourceText(error));
            m_statusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
            return;
        }
        request.virtualAddress = parsed.value;
        queueOperation(std::move(request), m_ledger);
        synchronizeLedger();
        followPendingOperation();
    }

    void DmaProcessOpPage::restoreLastWrite()
    {
        if (!m_ledger || projectLedger(m_ledger).pending) return;
        OperationRequest request;
        request.session = ksword::memory_backend::captureDdmaSession();
        request.restore = true;
        request.forceApproved = true;
        queueOperation(std::move(request), m_ledger);
        synchronizeLedger();
        followPendingOperation();
    }
}
