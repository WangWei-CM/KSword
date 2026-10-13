#include "MemoryDock.Internal.h"
#include "../UI/AsyncOperation.h"
#include "../Internationalization/LanguageManager.h"

#include <QSaveFile>

#include <algorithm>
#include <limits>

using namespace ksword::memory_dock_internal;

// 后台仅返回内部原因码和接口详情；用户提示模板统一在 UI 线程翻译。
bool MemoryDock::dumpMemoryRegionToFile(
    const std::uint32_t pid,
    const std::shared_ptr<void>& targetProcess,
    const std::uint64_t creationTime,
    const RegionEntry& region,
    const ksword::memory_backend::MemoryAccessBackend backend,
    const QString& dumpFilePath,
    const int progressPid,
    const std::function<bool()>& canceled,
    QString& errorTextOut,
    const std::function<bool()>& beginCommit,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress)
{
    errorTextOut.clear();
    using namespace ksword::memory_backend;
    if (backend != MemoryAccessBackend::UserMode && backend != MemoryAccessBackend::StandardDriver)
    {
        errorTextOut = QStringLiteral("backend_unsupported");
        return false;
    }

    // 范围来自菜单前快照；拒绝空区域、未提交区域、地址溢出及文件长度溢出。
    if (pid == 0 || creationTime == 0 || !targetProcess || region.regionSize == 0
        || region.state != MEM_COMMIT
        || region.regionSize > std::numeric_limits<std::uint64_t>::max() - region.baseAddress
        || region.regionSize > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))
    {
        errorTextOut = QStringLiteral("invalid_range");
        return false;
    }
    const HANDLE processHandle = targetProcess.get(); // 独立租约不随 UI 分离操作关闭/复用。
    const auto checkTarget = [&]()
    {
        if (canceled && canceled())
        {
            errorTextOut = QStringLiteral("canceled");
            return false;
        }
        FILETIME created{}, exited{}, kernel{}, user{}; // 每个边界复核创建时间与存活状态。
        if (::GetProcessId(processHandle) != toDwordPid(pid)
            || ::WaitForSingleObject(processHandle, 0) != WAIT_TIMEOUT
            || !::GetProcessTimes(processHandle, &created, &exited, &kernel, &user)
            || ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) != creationTime)
        {
            errorTextOut = QStringLiteral("identity_changed");
            return false;
        }
        return true;
    };
    const auto checkRegion = [&]()
    {
        MEMORY_BASIC_INFORMATION current{}; // 重新查询该地址，拒绝已释放/拆分/合并/改保护的区域。
        if (::VirtualQueryEx(processHandle, reinterpret_cast<LPCVOID>(region.baseAddress),
                &current, sizeof(current)) != sizeof(current)
            || reinterpret_cast<std::uintptr_t>(current.BaseAddress) != region.baseAddress
            || current.RegionSize != region.regionSize || current.Protect != region.protect
            || current.State != region.state || current.Type != region.type)
        {
            errorTextOut = QStringLiteral("region_changed");
            return false;
        }
        return true;
    };
    if (!checkTarget() || !checkRegion())
    {
        return false;
    }

    // 禁止直接写回退；任何失败或取消都只丢弃临时数据，不留下半个目标文件。
    QSaveFile outputFile(dumpFilePath);
    outputFile.setDirectWriteFallback(false);
    if (!outputFile.open(QIODevice::WriteOnly))
    {
        errorTextOut = QStringLiteral("open_failed\n") + outputFile.errorString();
        return false;
    }
    const DdmaSession unusedSession{}; // 本任务只允许 R3/R0，不读取或切换 DDMA/HVM 配置。
    std::uint64_t offset = 0; // 仅保留一个分块的字节缓冲，不分配整个区域大小。
    while (offset < region.regionSize)
    {
        if (!checkTarget() || !checkRegion())
        {
            return false;
        }
        const std::uint64_t length = std::min<std::uint64_t>( // 沿用共享协议的单次读取上限。
            region.regionSize - offset, KSWORD_ARK_MEMORY_READ_MAX_BYTES);
        const AccessOutcome outcome = readVirtual(backend, unusedSession, pid,
            region.baseAddress + offset, length);
        if (!checkTarget() || !checkRegion())
        {
            return false;
        }
        // PARTIAL_COPY、补零状态、短响应或长度不一致均不能当作有效 Dump 保存。
        if (!outcome.ok || outcome.partial || outcome.bytesDone != length
            || static_cast<std::uint64_t>(outcome.data.size()) != length)
        {
            errorTextOut = QStringLiteral("read_failed\n0x%1: %2/%3\n%4")
                .arg(region.baseAddress + offset, 0, 16).arg(outcome.bytesDone).arg(length).arg(outcome.failureText);
            return false;
        }
        if (outputFile.write(outcome.data.constData(), outcome.data.size()) != outcome.data.size())
        {
            errorTextOut = QStringLiteral("write_failed\n") + outputFile.errorString();
            return false;
        }
        offset += length;
        // 生产回调携带进度轮次；比例始终低于 1，完成必须由文件提交后的真实终态决定。
        if (progress)
        {
            progress(offset, region.regionSize);
        }
        else
        {
            kPro.set(progressPid, "正在导出内存区域", 0,
                static_cast<float>(static_cast<double>(offset) / static_cast<double>(region.regionSize) * 0.99));
        }
    }
    if (!checkTarget() || !checkRegion())
    {
        return false;
    }
    // 取消与进入提交由同一原子状态仲裁；进入提交后不能再承诺“取消保留旧文件”。
    if (beginCommit && !beginCommit())
    {
        errorTextOut = QStringLiteral("canceled");
        return false;
    }
    if (!outputFile.commit())
    {
        errorTextOut = QStringLiteral("commit_failed\n") + outputFile.errorString();
        return false;
    }
    return true;
}

bool MemoryDock::tryCancelMemoryRegionDump()
{
    if (!m_regionDumpPhase || m_regionDumpOperation == nullptr)
    {
        return false;
    }
    RegionDumpPhase expected = RegionDumpPhase::Reading; // 仅读取阶段能接受取消。
    if (m_regionDumpPhase->compare_exchange_strong(expected, RegionDumpPhase::CancelRequested))
    {
        m_regionDumpOperation->cancel();
        return true;
    }
    return expected == RegionDumpPhase::CancelRequested;
}

// 菜单入口只负责固定范围、选路径和提交共享异步任务，真正读取在上述静态函数中执行。
void MemoryDock::requestDumpMemoryRegion(
    const std::uint32_t pid,
    const QString& processName,
    const std::shared_ptr<void>& targetProcess,
    const std::uint64_t creationTime,
    const RegionEntry& region,
    const ksword::memory_backend::MemoryAccessBackend backend)
{
    const QString title = ks::i18n::sourceText(QStringLiteral("导出内存区域")); // 本次语言快照。
    if (m_regionDumpOperation != nullptr
        && (m_regionDumpOperation->isInFlight() || m_regionDumpOperation->hasPending()))
    {
        QMessageBox::information(this, title,
            ks::i18n::sourceText(QStringLiteral("已有区域导出正在进行，请等待完成或取消。")));
        return;
    }
    const auto targetStillSelected = [&]()
    {
        FILETIME created{}, exited{}, kernel{}, user{}; // 模态保存框前后都核对当前附加对象。
        return targetProcess && m_attachedPid == pid && m_attachedProcessHandle != nullptr
            && ::GetProcessId(m_attachedProcessHandle) == pid
            && ::WaitForSingleObject(targetProcess.get(), 0) == WAIT_TIMEOUT
            && ::GetProcessTimes(m_attachedProcessHandle, &created, &exited, &kernel, &user)
            && ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) == creationTime;
    };
    if (!targetStillSelected())
    {
        QMessageBox::warning(this, title,
            ks::i18n::sourceText(QStringLiteral("目标进程已改变，请重新选择区域。")));
        return;
    }

    // 后端由点击的菜单动作决定，不再次让用户选择、不回退到其它通道。
    const QString backendName = ks::i18n::sourceText(ksword::memory_backend::backendDisplayName(backend));
    const QString defaultName = QStringLiteral("%1_pid%2_0x%3_%4_%5.bin")
        .arg(processName.isEmpty() ? QStringLiteral("process") : processName).arg(pid)
        .arg(region.baseAddress, 0, 16).arg(backend == ksword::memory_backend::MemoryAccessBackend::UserMode
            ? QStringLiteral("R3") : QStringLiteral("R0"))
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    QPointer<MemoryDock> guardedOwner(this); // 保存框可运行嵌套事件循环并销毁宿主。
    const QString outputPath = QFileDialog::getSaveFileName(this, title, defaultName,
        ks::i18n::sourceText(QStringLiteral("原始内存转储 (*.bin);;所有文件 (*.*)")));
    if (guardedOwner.isNull() || outputPath.trimmed().isEmpty())
    {
        return;
    }
    if (!targetStillSelected())
    {
        QMessageBox::warning(this, title,
            ks::i18n::sourceText(QStringLiteral("目标进程已改变，请重新选择区域。")));
        return;
    }
    if (m_regionDumpOperation == nullptr)
    {
        m_regionDumpOperation = new ks::ui::AsyncOperation(this);
    }
    if (m_regionDumpOperation->isInFlight() || m_regionDumpOperation->hasPending())
    {
        return; // 防止保存框期间重入，两个导出不能共用进度或替换同一目标文件。
    }
    if (m_regionDumpProgressPid == 0)
    {
        m_regionDumpProgressPid = kPro.addReusable(this, "内存区域导出", "Dump内存区域");
    }
    const int progressPid = m_regionDumpProgressPid; // 后台与收尾只持值编号。
    const std::uint64_t progressGeneration = kPro.beginCycle(progressPid, "准备读取内存区域");
    m_regionDumpPhase = std::make_shared<std::atomic<RegionDumpPhase>>(RegionDumpPhase::Reading);
    const auto phase = m_regionDumpPhase; // UI 和 worker 仅共享原子值状态。

    // 错误标题和模板在 UI 线程预先翻译，后台只产生稳定原因码和外部接口详情。
    const QHash<QString, QString> errorTexts{
        {QStringLiteral("backend_unsupported"), ks::i18n::sourceText(QStringLiteral("区域导出仅支持 R3 或 R0。"))},
        {QStringLiteral("invalid_range"), ks::i18n::sourceText(QStringLiteral("区域范围无效或尚未提交，无法导出。"))},
        {QStringLiteral("identity_changed"), ks::i18n::sourceText(QStringLiteral("目标进程已退出或身份已改变，导出已取消。"))},
        {QStringLiteral("region_changed"), ks::i18n::sourceText(QStringLiteral("内存区域已变化或无法查询，导出已取消。"))},
        {QStringLiteral("canceled"), ks::i18n::sourceText(QStringLiteral("区域导出已取消。"))},
        {QStringLiteral("open_failed"), ks::i18n::sourceText(QStringLiteral("无法创建区域转储文件。"))},
        {QStringLiteral("read_failed"), ks::i18n::sourceText(QStringLiteral("内存读取未完成，区域转储未保存。"))},
        {QStringLiteral("write_failed"), ks::i18n::sourceText(QStringLiteral("区域转储文件写入失败。"))},
        {QStringLiteral("commit_failed"), ks::i18n::sourceText(QStringLiteral("区域转储文件提交失败。"))}
    };
    const QString successTitle = ks::i18n::sourceText(QStringLiteral("区域导出完成"));
    const QString failureTitle = ks::i18n::sourceText(QStringLiteral("区域导出失败"));
    const QString successText = ks::i18n::sourceText(QStringLiteral("PID %1，区域 0x%2（%3 字节）已通过 %4 保存到：\n%5"));
    const QString failureText = ks::i18n::sourceText(QStringLiteral("PID %1，区域 0x%2（%3 字节），后端 %4。\n%5"));
    const QString exceptionText = ks::i18n::sourceText(QStringLiteral("区域导出发生异常，未完成保存。"));

    struct Request final // 后台请求不持 UI 指针，不借用可被分离流程关闭的原句柄。
    {
        std::uint32_t pid; // 请求所属 PID。
        std::shared_ptr<void> target; // 独立进程对象租约。
        std::uint64_t created; // 防 PID 复用的创建时间。
        RegionEntry region; // 菜单前冻结的区域元数据。
        ksword::memory_backend::MemoryAccessBackend backend; // 用户明确选定的 R3/R0。
        QString path; // 保存框确认的目标文件。
        int progress; // 本次有限任务的进度编号。
        std::uint64_t progressGeneration; // 拒绝过期任务更新当前进度。
        std::shared_ptr<std::atomic<RegionDumpPhase>> phase; // 提交/取消仲裁。
        std::string readStep; // UI 线程预译的步骤文本。
    };
    struct Result final // 后台终态数据由共享控制器投递到所属 UI 线程。
    {
        bool ok = false; // 只有完成原子提交后才为真。
        QString error; // 内部原因码与外部接口详情。
    };
    const Request request{pid, targetProcess, creationTime, region, backend, outputPath, progressPid,
        progressGeneration, phase, ks::i18n::sourceText(QStringLiteral("正在导出内存区域")).toStdString()};
    m_regionDumpOperation->submit<Request, Result>(request,
        [](const Request& job, const ks::ui::AsyncOperationToken& token)
        {
            Result result;
            try
            {
                result.ok = MemoryDock::dumpMemoryRegionToFile(job.pid, job.target, job.created,
                    job.region, job.backend, job.path, job.progress,
                    [token]() { return token.isCanceled(); }, result.error,
                    [phase = job.phase]()
                    {
                        RegionDumpPhase expected = RegionDumpPhase::Reading;
                        return phase->compare_exchange_strong(expected, RegionDumpPhase::Committing);
                    },
                    [progressPid = job.progress, generation = job.progressGeneration, step = job.readStep]
                    (const std::uint64_t done, const std::uint64_t total)
                    {
                        kPro.set(progressPid, generation, step, 0,
                            static_cast<float>(static_cast<double>(done) / static_cast<double>(total) * 0.99));
                    });
            }
            catch (...)
            {
                job.phase->store(RegionDumpPhase::Failed);
                kPro.finish(job.progress, job.progressGeneration, kProgressState::Failure, "区域导出失败");
                throw;
            }
            // 即使宿主已关闭，提交后的真实终态仍由持值 worker 收尾，不把已保存结果记为取消。
            const bool wasCanceled = result.error == QStringLiteral("canceled");
            job.phase->store(result.ok ? RegionDumpPhase::Committed
                : (wasCanceled ? RegionDumpPhase::CancelRequested : RegionDumpPhase::Failed));
            kPro.finish(job.progress, job.progressGeneration,
                result.ok ? kProgressState::Success : (wasCanceled ? kProgressState::Canceled : kProgressState::Failure),
                result.ok ? "区域导出完成" : (wasCanceled ? "区域导出已取消" : "区域导出失败"));
            return result;
        },
        [this, request, backendName, errorTexts, successTitle, failureTitle, successText, failureText]
        (const Result& result, std::uint64_t)
        {
            if (result.ok)
            {
                QMessageBox::information(this, successTitle, successText.arg(request.pid)
                    .arg(request.region.baseAddress, 0, 16).arg(request.region.regionSize)
                    .arg(backendName, request.path));
            }
            else
            {
                const QString code = result.error.section(QLatin1Char('\n'), 0, 0); // 内部原因码。
                const QString detail = result.error.section(QLatin1Char('\n'), 1); // 接口原始错误详情。
                QString explanation = errorTexts.value(code, failureTitle);
                if (!detail.isEmpty())
                {
                    explanation += QLatin1Char('\n') + detail;
                }
                QMessageBox::warning(this, failureTitle, failureText.arg(request.pid)
                    .arg(request.region.baseAddress, 0, 16).arg(request.region.regionSize)
                    .arg(backendName, explanation));
            }
            return true;
        },
        [this, progressPid, progressGeneration, failureTitle, exceptionText](std::exception_ptr)
        {
            kPro.finish(progressPid, progressGeneration, kProgressState::Failure, "区域导出失败");
            QMessageBox::warning(this, failureTitle, exceptionText);
        },
        [progressPid, progressGeneration, phase](const ks::ui::AsyncOperationOutcome outcome)
        {
            // 被取消/关窗/丢队列的请求也结束进度，收尾不访问任何页面 QObject。
            if (outcome == ks::ui::AsyncOperationOutcome::Canceled
                || outcome == ks::ui::AsyncOperationOutcome::OwnerClosed
                || outcome == ks::ui::AsyncOperationOutcome::DeliveryDropped
                || outcome == ks::ui::AsyncOperationOutcome::Superseded)
            {
                RegionDumpPhase expected = RegionDumpPhase::Reading;
                phase->compare_exchange_strong(expected, RegionDumpPhase::CancelRequested);
                if (phase->load() == RegionDumpPhase::CancelRequested)
                {
                    kPro.finish(progressPid, progressGeneration, kProgressState::Canceled, "区域导出已取消");
                }
            }
        });
}
