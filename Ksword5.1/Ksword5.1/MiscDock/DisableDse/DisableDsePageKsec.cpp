// DisableDsePageKsec.cpp
// 作用：DisableDsePage 的 KSecDD 执行路径：写入桩定位与 KSecDD IOCTL 执行。
// 按 AGENTS.md 文件长度要求从 DisableDsePage.cpp 分出。

#include "DisableDsePage.h"
#include "KsecDdExecutor.h"

#include "../../Internationalization/LanguageManager.h"

#include <QApplication>
#include <QPointer>

#include <thread>

namespace ks::misc
{
    // ----------------------------------------------------------------
    // runKsecLocateGadget：
    // - 作用：在后台线程定位 ntoskrnl 中的 mov [rcx], edx; ret 字节序列；
    //   结果用于 KSecDD IOCTL 的 Function 参数。
    // ----------------------------------------------------------------
    void DisableDsePage::runKsecLocateGadget()
    {
        if (m_busy)
        {
            return;
        }
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("dse.trace.ksec.locating"),
            QStringLiteral("开始定位 ntoskrnl 写入桩……")));

        const QPointer<DisableDsePage> guard(this);

        std::thread([guard]() {
            // locateWriteGadget 全过程只读：SEC_IMAGE 映射 ntoskrnl 并扫描字节。
            const disable_dse::GadgetLocation gadget =
                disable_dse::locateWriteGadget();

            if (guard == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(qApp, [guard, gadget]() {
                if (guard == nullptr)
                {
                    return;
                }
                guard->setBusy(false);

                // 追加诊断轨迹。
                for (const QString& line : gadget.traceLines)
                {
                    guard->appendTrace(line);
                }

                // 保存结果并刷新 UI。
                guard->m_gadgetLocation = gadget;
                guard->updateStateDisplay();
                guard->updateButtons();

                if (gadget.ok)
                {
                    guard->setResultText(
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.ksec.gadget.ok"),
                            QStringLiteral("写入桩定位成功。")),
                        false);
                }
                else
                {
                    guard->setResultText(
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.ksec.gadget.failed"),
                            QStringLiteral("写入桩定位失败："))
                            + gadget.failureText,
                        true);
                }
            });
        }).detach();
    }

    // ----------------------------------------------------------------
    // runKsecApply：
    // - 输入 desiredValue：要写入 g_CiOptions 的值；
    //        isRestore：是恢复操作还是关闭操作；
    // - 作用：根据当前选定的 KSecDD 方案（LSASS 或 Server Silo），
    //   在后台线程完成一次 IOCTL 调用，然后回到 UI 线程更新状态。
    // ----------------------------------------------------------------
    void DisableDsePage::runKsecApply(
        const std::uint32_t desiredValue,
        const bool isRestore)
    {
        // 前置检查：桩和目标地址都已定位。
        if (m_busy || !m_gadgetLocation.ok || !m_location.ok)
        {
            return;
        }
        setBusy(true);
        appendTrace(isRestore
            ? ks::i18n::text(
                  QStringLiteral("dse.trace.ksec.restoring"),
                  QStringLiteral("KSecDD 恢复 g_CiOptions……"))
            : ks::i18n::text(
                  QStringLiteral("dse.trace.ksec.disabling"),
                  QStringLiteral("KSecDD 关闭签名强制……")));

        const QPointer<DisableDsePage> guard(this);
        const std::uint64_t gadgetAddr = m_gadgetLocation.kernelAddress;
        const std::uint64_t targetAddr = m_location.kernelAddress;
        const disable_dse::KsecDdStrategy strategy = m_ksecStrategy;

        // 记录恢复用的原值：如果之前没通过 R0 读回过，就从姿态推断。
        // 正常系统 ciEnabled 为 true 时 g_CiOptions = 0x6，
        // 附带 testSigning 时为 0x6|0x8=0xE。
        if (!isRestore && !m_hasSavedOriginal)
        {
            if (m_hasCurrentValue)
            {
                // 已从驱动直接读取到当前值，作为可靠原值。
                m_savedOriginalValue = m_currentValue;
                m_originalIsInferred = false;
            }
            else if (m_posture.ciEnabled)
            {
                // 警告：此值是从布尔姿态推断而非直接读取 g_CiOptions，
                // 可能丢失其它内部标志位。恢复时标记为推断值。
                m_savedOriginalValue = disable_dse::kCiOptionEnforceMask
                    | (m_posture.testSigningEnabled
                        ? disable_dse::kCiOptionTestSign : 0U);
                m_originalIsInferred = true;
            }
            else
            {
                // 签名强制已经关闭，没有有意义的原值可供恢复。
                m_savedOriginalValue = disable_dse::kCiOptionEnforceMask;
                m_originalIsInferred = true;
            }
            m_hasSavedOriginal = true;
        }

        std::thread([guard, gadgetAddr, targetAddr, desiredValue,
                     strategy, isRestore]()
        {
            disable_dse::ExecuteResult result;

            // 按选定方案执行。
            if (strategy == disable_dse::KsecDdStrategy::LsassInjection)
            {
                result = disable_dse::executeViaLsass(
                    gadgetAddr, targetAddr, desiredValue);
            }
            else
            {
                result = disable_dse::executeViaSilo(
                    gadgetAddr, targetAddr, desiredValue);
            }

            if (guard == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(qApp, [guard, result, desiredValue,
                                              strategy, isRestore]()
            {
                if (guard == nullptr)
                {
                    return;
                }
                guard->setBusy(false);

                for (const QString& line : result.traceLines)
                {
                    guard->appendTrace(line);
                }

                if (!result.ok)
                {
                    guard->appendTrace(ks::i18n::text(
                        QStringLiteral("dse.trace.ksec.apply_failed"),
                        QStringLiteral("KSecDD 操作失败：%1"))
                        .arg(result.failureText));
                    guard->updateStateDisplay();
                    guard->updateButtons();
                    guard->setResultText(
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.ksec.apply_failed"),
                            QStringLiteral("KSecDD 操作失败："))
                            + result.failureText,
                        true);
                    return;
                }

                // IOCTL 报告成功——刷新姿态验证实际效果。
                const auto postureAfter = disable_dse::queryPosture();
                guard->m_posture = postureAfter;

                // 查询姿态失败（source==None）时不可靠——默认值会
                // 误判关闭成功（ciEnabled 默认 false）。此时标记未验证。
                const bool postureReliable =
                    postureAfter.source != disable_dse::PostureSource::None;

                // 关闭时预期 ciEnabled=false，恢复时预期 ciEnabled=true。
                const bool effectVerified = postureReliable
                    && (isRestore
                        ? postureAfter.ciEnabled
                        : !postureAfter.ciEnabled);

                if (effectVerified)
                {
                    guard->m_currentValue = desiredValue;
                    guard->m_hasCurrentValue = true;
                }

                if (isRestore)
                {
                    const bool wasInferred = guard->m_originalIsInferred;
                    if (effectVerified)
                    {
                        // 只在验证通过后才清除恢复记录——
                        // 失败时保留原值以允许用户重试。
                        guard->m_hasSavedOriginal = false;
                        guard->m_originalIsInferred = false;
                        guard->m_usedKsecPath = false;
                        guard->appendTrace(ks::i18n::text(
                            QStringLiteral("dse.trace.ksec.restored"),
                            QStringLiteral("KSecDD 已恢复 g_CiOptions，签名强制已重新启用。")));
                        if (wasInferred)
                        {
                            guard->appendTrace(ks::i18n::text(
                                QStringLiteral("dse.trace.ksec.restored_inferred"),
                                QStringLiteral("注意：恢复值是从布尔姿态推断而非直接读取，可能丢失其它内部标志位。")));
                        }
                    }
                    else
                    {
                        // 恢复未验证通过——保留 m_hasSavedOriginal，
                        // 允许用户再次尝试恢复。
                        guard->appendTrace(ks::i18n::text(
                            QStringLiteral("dse.trace.ksec.restore_unverified"),
                            QStringLiteral("KSecDD IOCTL 返回成功，但签名强制状态未恢复（可能需要重启）。恢复记录已保留，可再次尝试。")));
                    }
                }
                else
                {
                    if (effectVerified)
                    {
                        // 记录本次是通过 KSecDD 路径关闭的，
                        // 析构时需通过同路径恢复。
                        guard->m_usedKsecPath = true;
                        guard->m_usedKsecStrategy = strategy;
                        guard->appendTrace(ks::i18n::text(
                            QStringLiteral("dse.trace.ksec.disabled"),
                            QStringLiteral("KSecDD 已关闭签名强制，原值已记录，请尽快恢复。")));
                    }
                    else if (!postureReliable)
                    {
                        // 姿态查询本身失败——无法验证效果。
                        guard->appendTrace(ks::i18n::text(
                            QStringLiteral("dse.trace.ksec.posture_query_failed"),
                            QStringLiteral("KSecDD IOCTL 返回成功，但姿态查询失败，无法验证签名强制是否已关闭。")));
                    }
                    else
                    {
                        guard->appendTrace(ks::i18n::text(
                            QStringLiteral("dse.trace.ksec.no_effect"),
                            QStringLiteral("KSecDD IOCTL 返回成功，但签名强制状态未改变——写入可能未生效。")));
                    }
                }

                guard->updateStateDisplay();
                guard->updateButtons();

                if (!effectVerified)
                {
                    guard->setResultText(
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.ksec.verify_failed"),
                            QStringLiteral("IOCTL 成功但验证未通过，签名强制状态未按预期改变。")),
                        true);
                }
                else
                {
                    guard->setResultText(
                        isRestore
                            ? ks::i18n::text(
                                  QStringLiteral("misc.disable_dse.ksec.restore_ok"),
                                  QStringLiteral("KSecDD 已恢复原值。"))
                            : ks::i18n::text(
                                  QStringLiteral("misc.disable_dse.ksec.disable_ok"),
                                  QStringLiteral("KSecDD 已关闭驱动签名强制。请立即加载目标驱动，然后马上恢复。")),
                        false);
                }
            });
        }).detach();
    }
}
