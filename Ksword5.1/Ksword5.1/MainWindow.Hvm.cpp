// MainWindow.Hvm.cpp
//
// 标题栏权限按钮排里 HVM（KSwordVM，R-1 层）按钮的全部行为。
//
// 拆分理由与 KernelHvmTab 分片一致：MainWindow.cpp 已经承担了窗口、Dock、
// 权限、驱动服务等多条主线，HVM 的状态机、菜单与后台查询自成一块，混进去只会
// 让两边都更难读。
//
// 三条硬性约束：
// - 状态查询与所有控制命令都是阻塞 IOCTL，一律走后台线程，UI 线程只做展示；
// - 进入 VMX non-root 会改变全机 CPU 状态，属于高风险操作，必须走统一确认；
// - 写权限默认关闭。关闭时 HVM 只做观测，任何 R-1 改写入口都不出现在菜单里。

#include "MainWindow.h"

#include "Framework/DestructiveActionConfirmation.h"
#include "Internationalization/LanguageManager.h"
#include "UI/HvmControl.h"
#include "UI/HvmCrPolicyDialog.h"
#include "UI/HvmEventDialog.h"
#include "UI/HvmMemoryDialog.h"
#include "UI/HvmMsrPolicyDialog.h"
#include "UI/HvmDomainDialog.h"
#include "UI/HvmHookWizard.h"
#include "UI/HvmProcessDialog.h"
#include "UI/HvmViewDialog.h"
#include "UI/HvmWriteAccessGate.h"
#include "theme.h"

#include <QAction>
#include <QDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

#include <thread>
#include <utility>

namespace
{
    /*
     * HVM 按钮的显示状态：**一个**有序状态，不是两个独立布尔量。
     *
     * 原先背景色编码"常驻是否激活"、边框与文字色编码"能力是否可用"，两个通道
     * 各自独立取值，于是能画出自相矛盾的组合——出故障时背景说"正在跑"、边框
     * 说"不可用"；更糟的是 NotPrepared 被算成"可用"，一个还没准备的驱动被画成
     * 有能力的样子，而提示文字同时在说它没准备。看的人只能二选一地相信。
     *
     * 改成单一状态之后，"按钮长什么样"与"现在能不能点、点了会发生什么"是同一
     * 件事的两种说法，不可能互相打架。
     */
    enum class HvmButtonState
    {
        Unavailable,   // 硬件门没过、驱动没起、或外层 hypervisor 占着。
        Faulted,       // 有故障或待回滚，点之前必须先重置。
        NotPrepared,   // 能力齐备但资源没准备，点一下会先准备再常驻。
        Ready,         // 已准备、未常驻，点一下启动常驻。
        Resident       // 正在常驻，点一下停止。
    };

    /*
     * 已常驻时优先表达停止入口；故障/准入拒绝不能遮住部分常驻的退出路径。
     * 未常驻时才按故障、硬件和准备模式判定能否进入。
     */
    HvmButtonState resolveHvmButtonState(
        const ksword::hvm::HvmAvailability availability,
        const bool residentActive,
        const bool faulted,
        const bool admissionReady)
    {
        // Retained residency must remain stoppable even after a capability or power refusal.
        if (residentActive) { return HvmButtonState::Resident; }
        if (availability != ksword::hvm::HvmAvailability::Available &&
            availability != ksword::hvm::HvmAvailability::NotPrepared &&
            availability != ksword::hvm::HvmAvailability::Faulted)
        {
            return HvmButtonState::Unavailable;
        }
        if (faulted || availability == ksword::hvm::HvmAvailability::Faulted)
        {
            return HvmButtonState::Faulted;
        }
        if (!admissionReady) { return HvmButtonState::Unavailable; }
        if (availability == ksword::hvm::HvmAvailability::NotPrepared)
        {
            return HvmButtonState::NotPrepared;
        }
        return HvmButtonState::Ready;
    }

    QString buildHvmButtonStyle(const HvmButtonState state)
    {
        const bool residentActive = state == HvmButtonState::Resident;
        // 只有 Ready 与 Resident 是"这一刻真的可以按预期工作"。
        // NotPrepared 画成弱可用：能点，但点了要先走一步准备。
        const bool available = state == HvmButtonState::Ready ||
            state == HvmButtonState::Resident ||
            state == HvmButtonState::NotPrepared;
        const QString backgroundColor = residentActive
            ? KswordTheme::PrimaryBlueHex
            : KswordTheme::SurfaceHex();
        /*
         * 故障单独一种颜色，不与"不可用"合流。
         *
         * 两者要人做的事完全不同：故障是点一下重置就能继续，不可用是这台机器
         * 或这套配置根本走不通。画成同一个灰色，一次可恢复的故障会被读成"换台
         * 机器吧"，而那正好是最贵的误读。
         */
        const bool faulted = state == HvmButtonState::Faulted;
        // 非激活态的强调色文字要先对 Surface 校准，否则高亮度强调色会糊在底上。
        const QString textColor = residentActive
            ? KswordTheme::OnAccentHex()
            : (faulted
                ? KswordTheme::WarningHex()
                : (available
                    ? KswordTheme::AccentButtonTextHex()
                    : KswordTheme::TextSecondaryHex()));
        const QString borderColor = faulted
            ? KswordTheme::WarningHex()
            : (available
                ? KswordTheme::PrimaryBlueBorderHex
                : KswordTheme::BorderHex());
        const QString hoverColor = residentActive
            ? KswordTheme::PrimaryBlueSolidHoverHex()
            : KswordTheme::PrimaryBlueSubtleHex();
        const QString hoverTextColor = residentActive
            ? KswordTheme::OnAccentHex()
            : KswordTheme::TextPrimaryColorHex();
        return QStringLiteral(
            "QPushButton {"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:2px 8px;"
            "  font-weight:600;"
            "}"
            "QPushButton:hover {"
            "  background:%4;"
            "  color:%5;"
            "  border:1px solid %4;"
            "}"
            "QPushButton:pressed {"
            "  background:%6;"
            "  color:%5;"
            "}"
            "QPushButton:disabled {"
            "  color:%7;"
            "}")
            .arg(backgroundColor)
            .arg(textColor)
            .arg(borderColor)
            .arg(hoverColor)
            .arg(hoverTextColor)
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::TextSecondaryHex());
    }
}

HvmDock* MainWindow::createHvmDockContent()
{
    auto* const dockContent = new HvmDock(this);
    dockContent->setActionHandler([this](const HvmDock::Action action) {
        handleHvmDockAction(action);
    });
    dockContent->setCommandOperationHandler([this](bool running) {
        m_hvmOperationRunning = running;
        applyHvmButtonState();
        if (!running) { refreshHvmStatusAsync(); }
    });
    return dockContent;
}

void MainWindow::handleHvmDockAction(const HvmDock::Action action)
{
    // 这一层只做分派。每一条都落到右键菜单用的同一个实现上，包括那几处
    // confirmDestructiveAction 高危确认——两个入口因此不可能走出两套口径。
    const auto showHvmDialog = [](QDialog* const dialog) {
        // 无父窗口模态：R-1 面板要能和主界面并排使用，与右键菜单一致。
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    };

    switch (action)
    {
    case HvmDock::Action::ToggleResident:
        handleHvmStatusButtonClicked();
        return;
    case HvmDock::Action::SelfTest:
        if (m_hvmWidget != nullptr) { m_hvmWidget->testPreparedBackend(); }
        return;
    case HvmDock::Action::Soak:
        runHvmSoak(5000);
        return;
    case HvmDock::Action::PrepareResources:
        runHvmPrepare();
        return;
    case HvmDock::Action::ReleaseResources:
        runHvmRelease();
        return;
    case HvmDock::Action::ResetFault:
        runHvmFaultReset();
        return;
    case HvmDock::Action::OpenHookWizard:
        // 向导自己管非模态生命周期与 WA_DeleteOnClose（openWizard 的契约），
        // 所以这一条不走 showHvmDialog —— 走了会重复设置属性并多一层 show。
        ks::ui::HvmHookWizard::openWizard(this);
        return;
    case HvmDock::Action::OpenViewDialog:
        showHvmDialog(new HvmViewDialog(this));
        return;
    case HvmDock::Action::OpenDomainDialog:
        showHvmDialog(new HvmDomainDialog(this));
        return;
    case HvmDock::Action::OpenMsrPolicyDialog:
        showHvmDialog(new HvmMsrPolicyDialog(this));
        return;
    case HvmDock::Action::OpenCrPolicyDialog:
        showHvmDialog(new HvmCrPolicyDialog(this));
        return;
    case HvmDock::Action::OpenMemoryDialog:
        showHvmDialog(new HvmMemoryDialog(this));
        return;
    case HvmDock::Action::OpenEventDialog:
        showHvmDialog(new HvmEventDialog(this));
        return;
    case HvmDock::Action::OpenProcessDialog:
        showHvmDialog(new HvmProcessDialog(this));
        return;
    }
}

void MainWindow::applyHvmButtonState()
{
    // HVM 页的状态轮询看不到"命令正在跑"这件事：命令期间驱动侧状态锁被独占，
    // 查询只会排在它后面。只能由发起命令的这一侧推过去。
    if (m_hvmWidget != nullptr)
    {
        m_hvmWidget->setOperationRunning(m_hvmOperationRunning);
    }
    if (m_hvmStatusButton == nullptr)
    {
        return;
    }
    // 周期状态仍查询，实际状态/主题不变时不重复制造 StyleChange 和图标重建。
    const QString hvmStyle = buildHvmButtonStyle(resolveHvmButtonState(
        m_hvmAvailability,
        m_hvmResidentActive,
        m_hvmFaulted,
        m_hvmAvailable));
    if (m_hvmStatusButton->styleSheet() != hvmStyle)
    {
        m_hvmStatusButton->setStyleSheet(hvmStyle);
    }
    // 操作进行中禁用按钮：常驻切换与保持自检都会独占驱动侧状态锁。
    if (m_hvmStatusButton->isEnabled() != !m_hvmOperationRunning)
    {
        m_hvmStatusButton->setEnabled(!m_hvmOperationRunning);
    }
    /*
     * 提示文字跟随显示名。
     *
     * 这里原先写死 "HVM"，于是设置里把显示名换成 HVM 或 R-1 之后，按钮文字改了
     * 而提示还在自称 HVM——同一个控件的两处文本各叫各的名字。
     */
    const QString hvmName = ks::settings::hvmDisplayNameLabel(
        m_currentAppearanceSettings.hvmDisplayName);
    if (m_hvmOperationRunning)
    {
        m_hvmStatusButton->setToolTip(
            ks::i18n::sourceText(QStringLiteral("%1 操作进行中...")).arg(hvmName));
        return;
    }
    m_hvmStatusButton->setToolTip(m_hvmTooltip.isEmpty()
        ? ks::i18n::sourceText(QStringLiteral("%1：KSwordVM 硬件虚拟化（R-1）常驻状态。左键启动或停止常驻，右键打开 R-1 能力菜单。")).arg(hvmName)
        : m_hvmTooltip);
}

void MainWindow::refreshHvmStatusAsync()
{
    // 合并并发查询：权限按钮刷新是周期性的，堆积请求只会拖慢驱动。
    if (m_hvmQueryInFlight || m_hvmOperationRunning)
    {
        return;
    }
    m_hvmQueryInFlight = true;
    QPointer<MainWindow> safeThis(this);
    std::thread([safeThis]() {
        const ksword::hvm::HvmState state = ksword::hvm::queryState();
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, state]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_hvmQueryInFlight = false;
                safeThis->m_hvmResidentActive = state.residentActive;
                /*
                 * 保留完整的可用性取值，不再压成一个布尔量。
                 *
                 * 压扁是矛盾的来源：NotPrepared 被并进"可用"，于是没准备过的
                 * 驱动画成有能力的样子；Faulted 被并进"不可用"，于是一次可重置
                 * 的故障和"这机器不支持"长得一样。按钮状态现在从这个原值算。
                 */
                safeThis->m_hvmAvailability = state.availability;
                safeThis->m_hvmAvailable = state.residentAdmission && state.configurationReason.isEmpty();
                safeThis->m_hvmFaulted = state.faulted;
                safeThis->m_hvmGeneration = state.generation;
                safeThis->m_hvmBackend = state.backend;
                safeThis->m_hvmNestedSupported = state.nestedSupported;
                safeThis->m_hvmTooltip = state.detail;
                safeThis->applyHvmButtonState();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::handleHvmStatusButtonClicked()
{
    if (m_hvmOperationRunning)
    {
        return;
    }
    if (!m_r0DriverServiceRunning)
    {
        QMessageBox::information(
            this,
            QStringLiteral("HVM"),
            ks::i18n::sourceText(QStringLiteral(
                "KswordARK 驱动未运行。请先点击 R0 启动驱动服务。")));
        return;
    }
    if (m_hvmFaulted && !m_hvmResidentActive)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("HVM"),
            ks::i18n::sourceText(QStringLiteral(
                "HVM 处于故障或待回滚状态。请先在右键菜单中执行“重置故障状态”。")));
        return;
    }
    if (!m_hvmResidentActive && !m_hvmAvailable)
    {
        // 不可用的具体原因由后台快照写进 tooltip，这里原样呈现而不是给通用文案。
        QMessageBox::information(
            this,
            QStringLiteral("HVM"),
            m_hvmTooltip.isEmpty()
                ? ks::i18n::sourceText(QStringLiteral("当前硬件或系统状态不支持 HVM 常驻。"))
                : m_hvmTooltip);
        return;
    }

    const bool stopping = m_hvmResidentActive;
    if (!stopping)
    {
        // 进入 VMX non-root 会改变全部逻辑处理器的运行模式，走统一高风险确认。
        const bool confirmed = ks::ui::confirmDestructiveAction(
            this,
            QStringLiteral("HvmStartResident"),
            ks::i18n::sourceText(QStringLiteral("启动 KSwordVM 常驻")),
            ks::i18n::sourceText(QStringLiteral("本机全部逻辑处理器")),
            ks::i18n::sourceText(QStringLiteral("全部逻辑处理器将使用 Intel VMX/EPT 或 AMD SVM/NPT 进入来宾态。必须通过全核自检与生命周期保护；AMD 嵌套仍为实验性。硬件、驱动或电源转换异常可能导致系统不稳定或蓝屏。")));
        if (!confirmed)
        {
            return;
        }
    }

    m_hvmOperationRunning = true;
    applyHvmButtonState();
    QPointer<MainWindow> safeThis(this);
    const unsigned long generation = m_hvmGeneration;
    std::thread([safeThis, stopping, generation]() {
        const ksword::hvm::HvmCommandResult result = stopping
            ? ksword::hvm::stopResident(generation)
            : ksword::hvm::startResident(generation);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_hvmOperationRunning = false;
                safeThis->applyHvmButtonState();
                if (!result.ok)
                {
                    QMessageBox::warning(
                        safeThis,
                        QStringLiteral("HVM"),
                        result.message);
                }
                safeThis->refreshHvmStatusAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::runHvmSoak(const unsigned long milliseconds)
{
    if (m_hvmOperationRunning)
    {
        return;
    }
    // 自检期间同样会让全部处理器进入 non-root，风险与直接启动常驻一致。
    const bool confirmed = ks::ui::confirmDestructiveAction(
        this,
        QStringLiteral("HvmSoak"),
        ks::i18n::sourceText(QStringLiteral("KSwordVM 常驻保持自检")),
        ks::i18n::sourceText(QStringLiteral("本机全部逻辑处理器")),
        ks::i18n::sourceText(QStringLiteral("全部逻辑处理器会进入 VMX non-root 并保持数秒后自动退出。期间任何未被处理的 VM-exit 都会被记录为掉核，与 Hyper-V/VBS 冲突时可能导致系统不稳定。")));
    if (!confirmed)
    {
        return;
    }

    m_hvmOperationRunning = true;
    applyHvmButtonState();
    QPointer<MainWindow> safeThis(this);
    const unsigned long generation = m_hvmGeneration;
    std::thread([safeThis, generation, milliseconds]() {
        const ksword::hvm::HvmCommandResult result =
            ksword::hvm::runSoak(generation, milliseconds);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_hvmOperationRunning = false;
                safeThis->applyHvmButtonState();
                // 自检结论无论成败都要呈现：它是常驻可用性的唯一直接证据。
                if (result.ok)
                {
                    QMessageBox::information(
                        safeThis,
                        QStringLiteral("HVM"),
                        result.message);
                }
                else
                {
                    QMessageBox::warning(
                        safeThis,
                        QStringLiteral("HVM"),
                        result.message);
                }
                safeThis->refreshHvmStatusAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::runHvmPrepare()
{
    if (m_hvmOperationRunning)
    {
        return;
    }
    m_hvmOperationRunning = true;
    applyHvmButtonState();
    QPointer<MainWindow> safeThis(this);
    std::thread([safeThis]() {
        // ensurePrepared 会按需 PREPARE + SELF_TEST，已就绪时直接返回成功。
        // 它**不**进入常驻 —— 那正是这个入口存在的理由。
        const ksword::hvm::HvmCommandResult result =
            ksword::hvm::ensurePrepared();
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_hvmOperationRunning = false;
                safeThis->applyHvmButtonState();
                if (!result.ok)
                {
                    QMessageBox::warning(
                        safeThis,
                        QStringLiteral("HVM"),
                        result.message);
                }
                else
                {
                    QMessageBox::information(
                        safeThis,
                        QStringLiteral("HVM"),
                        ks::i18n::sourceText(QStringLiteral("资源与全核自检已就绪，尚未常驻。Intel 可配置视图、策略与域；AMD 按准备时选择的 SVM 模式启动常驻。")));
                }
                safeThis->refreshHvmStatusAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::runHvmRelease()
{
    if (m_hvmOperationRunning)
    {
        return;
    }
    // 释放资源会丢掉已装的视图/策略/域，所以走高危确认。
    const bool confirmed = ks::ui::confirmDestructiveAction(
        this,
        QStringLiteral("HvmReleaseResources"),
        ks::i18n::sourceText(QStringLiteral("释放 HVM 资源")),
        ks::i18n::sourceText(QStringLiteral("本机全部逻辑处理器")),
        ks::i18n::sourceText(QStringLiteral("将释放全部每处理器资源与 EPT/NPT 层次，并清除准备和自检证据。Intel 视图、策略与域会一并消失；修改 AMD 嵌套模式后也需要释放并重新准备。")));
    if (!confirmed)
    {
        return;
    }
    m_hvmOperationRunning = true;
    applyHvmButtonState();
    QPointer<MainWindow> safeThis(this);
    const unsigned long generation = m_hvmGeneration;
    std::thread([safeThis, generation]() {
        const ksword::hvm::HvmCommandResult result =
            ksword::hvm::releaseResources(generation);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_hvmOperationRunning = false;
                safeThis->applyHvmButtonState();
                if (!result.ok)
                {
                    QMessageBox::warning(
                        safeThis,
                        QStringLiteral("HVM"),
                        result.message);
                }
                safeThis->refreshHvmStatusAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::runHvmFaultReset()
{
    if (m_hvmOperationRunning)
    {
        return;
    }
    m_hvmOperationRunning = true;
    applyHvmButtonState();
    QPointer<MainWindow> safeThis(this);
    const unsigned long generation = m_hvmGeneration;
    std::thread([safeThis, generation]() {
        const ksword::hvm::HvmCommandResult result =
            ksword::hvm::resetFault(generation);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_hvmOperationRunning = false;
                safeThis->applyHvmButtonState();
                if (!result.ok)
                {
                    QMessageBox::warning(
                        safeThis,
                        QStringLiteral("HVM"),
                        result.message);
                }
                safeThis->refreshHvmStatusAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::showHvmMenu(const QPoint& globalPosition)
{
    QMenu menu(this);
    const bool backendKnown = m_hvmBackend == KSWORD_ARK_HVM_BACKEND_VMX || m_hvmBackend == KSWORD_ARK_HVM_BACKEND_SVM;

    // 常驻开关与左键一致，放进菜单只是为了让能力集中可见。
    QAction* const toggleAction = menu.addAction(m_hvmResidentActive
        ? ks::i18n::sourceText(QStringLiteral("停止常驻"))
        : ks::i18n::sourceText(QStringLiteral("启动常驻")));
    toggleAction->setEnabled(!m_hvmOperationRunning &&
        (m_hvmResidentActive || m_hvmAvailable));
    connect(toggleAction, &QAction::triggered, this, [this]() {
        handleHvmStatusButtonClicked();
    });

    // 保持自检是唯一能证明"常驻活得下来"的手段：进出一次只证明转换本身没问题。
    QAction* const soakAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("常驻保持自检（5 秒）")));
    soakAction->setEnabled(!m_hvmOperationRunning &&
        !m_hvmResidentActive &&
        m_hvmAvailable);
    connect(soakAction, &QAction::triggered, this, [this]() {
        runHvmSoak(5000);
    });

    menu.addSeparator();

    // 「准备资源」与「释放资源」必须在这里，否则这个菜单有一条必然踩中的死路。
    //
    // 视图 / MSR / CR / 域四个面板都要求资源已准备。而这个菜单原本能做的只有
    // 「启动常驻」—— 它会在同一次调用里准备完资源**紧接着进入常驻**，于是四个
    // 面板立刻从「尚未准备」变成「常驻期间不能改」。中间那个唯一可用的窗口期
    // 在这里按不出来，用户只能在另一个 Dock 的另一个页里找到它，而两边此前
    // 没有任何交叉引用。
    //
    // 「释放资源」同时是让后端开关生效的唯一途径：后端在 PREPARE 时选定，
    // 而 ensurePrepared 在资源已就绪时不会重发 PREPARE。
    QAction* const prepareAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("准备资源（不进入常驻）")));
    prepareAction->setEnabled(!m_hvmOperationRunning &&
        !m_hvmResidentActive &&
        m_hvmAvailable);
    // 一条 sourceText 必须是**一个不拆行的字面量**：相邻字符串拼接会被
    // i18n 提取器当成多个独立词条，于是语言包里多出几条永远匹配不上的碎片。
    prepareAction->setToolTip(m_hvmBackend == KSWORD_ARK_HVM_BACKEND_SVM
        ? ks::i18n::sourceText(QStringLiteral("准备 AMD SVM/VMCB/NPT 资源，不进入常驻；嵌套模式在此确定，随后执行全核自检。"))
        : ks::i18n::sourceText(QStringLiteral("分配每处理器资源并建立 EPT，但不进入常驻。分离视图、MSR 策略、CR 策略与执行域都必须在这一步之后、启动常驻之前安装 —— 常驻期间这几张表都是不可变的。")));
    connect(prepareAction, &QAction::triggered, this, [this]() {
        runHvmPrepare();
    });

    QAction* const releaseAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("释放资源")));
    releaseAction->setEnabled(!m_hvmOperationRunning && !m_hvmResidentActive &&
        m_r0DriverServiceRunning && backendKnown);
    releaseAction->setToolTip(m_hvmBackend == KSWORD_ARK_HVM_BACKEND_SVM
        ? ks::i18n::sourceText(QStringLiteral("释放 SVM/VMCB/NPT 资源；更改嵌套模式后必须释放并重新准备。"))
        : ks::i18n::sourceText(QStringLiteral("释放全部可逆资源，回到未准备状态。改过分离视图后端或每处理器私有 EPT 之后必须走这一步 —— 那两个选择只在准备资源时被消费，已准备的运行时改开关不会生效。")));
    connect(releaseAction, &QAction::triggered, this, [this]() {
        runHvmRelease();
    });

    menu.addSeparator();

    // 嵌套模式：外层有 hypervisor 时（虚拟机内、或裸机开着 VBS/HVCI）唯一能跑起来的
    // 方式。不改写任何系统状态，所以不走高风险确认，但要说清代价。
    QAction* const nestedAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("允许嵌套运行（作为 L1）")));
    nestedAction->setCheckable(true);
    nestedAction->setChecked(ksword::hvm::isNestedAllowed());
    nestedAction->setEnabled(backendKnown && !m_hvmOperationRunning && !m_hvmResidentActive);
    nestedAction->setToolTip(m_hvmBackend == KSWORD_ARK_HVM_BACKEND_SVM
        ? ks::i18n::sourceText(QStringLiteral("裸机无需外层嵌套许可。作为来宾运行时仅支持显式允许的 VMware 外层；Hyper-V/VBS 和未知外层不会因打开此选项而获准。"))
        : ks::i18n::sourceText(QStringLiteral("在虚拟机内或开着 VBS/HVCI 的机器上，KSwordVM 只能作为 L1 运行：每条 VMX 操作都由外层 hypervisor 模拟，性能明显下降，可用能力也只剩外层愿意暴露的那部分。")));
    connect(nestedAction, &QAction::triggered, this, [this](const bool checked) {
        ksword::hvm::setNestedAllowed(checked);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    /*
     * 嵌套派发：与上面那一项**方向相反**，所以紧挨着放。
     *
     * 上面说的是「允许我们跑在别人底下」，我们是来宾；这一项说的是「允许别人
     * 跑在我们底下」，我们是宿主。两句话都叫"嵌套"，但打开的是完全不同的东西。
     *
     * 在这一项存在之前，整个嵌套派发只有 KernelDock 的嵌套页能打开，而那一页
     * 把它绑死在"当前在哪个功能页"上——也就是说没有开关，去到那一页就必然开，
     * 不去那一页就必然不开。一项已经端到端验证过的能力，在主界面上不存在。
     */
    QAction* const nestedDispatchAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("允许来宾嵌套（我们作为宿主）")));
    nestedDispatchAction->setEnabled(!m_hvmOperationRunning && !m_hvmResidentActive && m_hvmNestedSupported);
    nestedDispatchAction->setCheckable(true);
    nestedDispatchAction->setChecked(ksword::hvm::isNestedDispatchEnabled());
    if (m_hvmBackend == KSWORD_ARK_HVM_BACKEND_SVM)
    {
        nestedDispatchAction->setText(ks::i18n::sourceText(QStringLiteral("允许来宾嵌套 SVM（实验性）")));
        nestedDispatchAction->setToolTip(ks::i18n::sourceText(QStringLiteral("在 AMD 上启用实验性 VMCB 分派、退出反射与 NPT 合成。必须在准备资源前选择；改变后先停止、释放并重新准备。本开关不持久化，内层系统启动尚未验收。")));
    }
    else
    {
    nestedDispatchAction->setToolTip(ks::i18n::sourceText(QStringLiteral("与上一项方向相反：上一项是让我们跑在别人底下，这一项是让别人跑在我们底下。打开后，来宾里的 ring 0 代码可以真的 VMXON、维护自己的 vmcs12、把 L2 跑起来；退出先落到我们手上，L1 要 EPT 时由影子层次按需合成。关着时 VMX 指令被注 #UD——对已经在跑的 VMware / VirtualBox / WSL2 来说就是「虚拟机打不开了」。与每处理器私有 EPT 互斥。本开关不持久化。")));
    }
    connect(nestedDispatchAction, &QAction::triggered, this,
            [this, nestedDispatchAction](const bool checked) {
        if (!checked)
        {
            ksword::hvm::setNestedDispatchEnabled(false);
            applyHvmButtonState();
            refreshHvmStatusAsync();
            return;
        }
        // 互斥是驱动的硬拒绝：嵌套要把来宾的 EPT 层次和我们的合成成一个指针，
        // 而私有根会让这个合成变成处理器相关的。同时请求会被判
        // STATUS_INVALID_PARAMETER，而那条回答只说"请求不合法"，不指哪一位。
        if (m_hvmBackend == KSWORD_ARK_HVM_BACKEND_VMX && ksword::hvm::isLocalEptEnabled())
        {
            nestedDispatchAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("嵌套派发与私有 EPT 互斥")),
                ks::i18n::sourceText(QStringLiteral("嵌套要把来宾的 EPT 层次和我们的合成成一个 EPT 指针，而私有 EPT 要给每个处理器各自一份层次，那会让这个合成变成处理器相关的。驱动会拒绝同时请求。请先关掉「每处理器私有 EPT」。")));
            return;
        }
        // 写权限前置：放开一个来宾可见的、能改变它自己执行环境的能力，
        // 与 VMFUNC 同类。
        if (!ksword::hvm::isWriteAccessEnabled())
        {
            nestedDispatchAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("嵌套派发需要先开启写权限")),
                ks::i18n::sourceText(QStringLiteral("嵌套派发会向来宾开放 VMX 或 SVM 能力，属于写权限门管辖的范围。请先打开「允许 R-1 写操作」。")));
            return;
        }
        const bool confirmed = ks::ui::confirmDestructiveAction(
            this,
            QStringLiteral("HvmEnableNestedDispatch"),
            ks::i18n::sourceText(QStringLiteral("允许来宾嵌套")),
            ks::i18n::sourceText(QStringLiteral("本机全部 ring 0 代码")),
            m_hvmBackend == KSWORD_ARK_HVM_BACKEND_SVM
                ? ks::i18n::sourceText(QStringLiteral("AMD 来宾将获得实验性嵌套 SVM 能力，通过 VMCB 反射与 NPT 合成运行内层代码。每核需要额外资源；完整内层系统兼容性及性能尚未验收。"))
                : ks::i18n::sourceText(QStringLiteral("打开后，这台机器上任何 ring 0 代码都能在我们底下起一台虚拟机，而我们只看得到它产生的退出，看不到它在里面跑什么。影子 EPT 层次按需合成，MSR 与 I/O 位图按 L1 自己的那份合并，每核要额外占用若干页。位图在每次进入 L2 时重算一遍、不缓存，所以来宾越频繁地进出 L2 越贵。")));
        if (!confirmed)
        {
            nestedDispatchAction->setChecked(false);
            return;
        }
        ksword::hvm::setNestedDispatchEnabled(true);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    menu.addSeparator();

    auto* hideHypervisorAction = menu.addAction(ks::i18n::sourceText(QStringLiteral("对来宾用户态隐藏 Hypervisor 身份")));
    hideHypervisorAction->setCheckable(true);
    hideHypervisorAction->setChecked(ksword::hvm::isHypervisorHidden());
    hideHypervisorAction->setEnabled(!m_hvmResidentActive && !m_hvmOperationRunning && ksword::hvm::isNestedDispatchEnabled());
    connect(hideHypervisorAction, &QAction::toggled, this, [](bool enabled) {
        ksword::hvm::setHypervisorHidden(enabled);
    });

    // 私有 EPT：不放开能力，只是让已有的视图/授权在多核上安全，所以不走
    // 高风险确认——真正危险的是视图本身，那由写权限门管。
    QAction* const localEptAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("每处理器私有 EPT（多核可用视图）")));
    localEptAction->setCheckable(true);
    localEptAction->setChecked(ksword::hvm::isLocalEptEnabled());
    localEptAction->setToolTip(ks::i18n::sourceText(QStringLiteral("给每个处理器一份私有 EPT 层次，翻转只落在取到 exit 的那个处理器上。打开后才能在多核机器上安装 EPT 视图；关着时视图仍然只能在单核拓扑安装。与 VMFUNC、嵌套 VMX 互斥，且每核要多花若干页。")));
    connect(localEptAction, &QAction::triggered, this, [this, localEptAction](const bool checked) {
        // 互斥不是偏好而是驱动的硬拒绝：与 EPTP 切换后端同时请求会在任何
        // 分配之前被判 INVALID_REQUEST。这里拦下来并说明原因，比让用户攒出
        // 一个必然失败的组合再去猜协议状态码强。
        if (checked && ksword::hvm::isEptpSwitchEnabled())
        {
            localEptAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("私有 EPT 与 EPTP 切换后端互斥")),
                ks::i18n::sourceText(QStringLiteral("EPTP 切换后端靠在多份 EPT 层次之间换 EPTP 来做视图，私有 EPT 则要给每个处理器各自一份层次，两者对层次的用法冲突，驱动会在分配任何资源之前拒绝同时请求。请先关掉「EPT 分离视图用 EPTP 切换后端」。")));
            return;
        }
        /*
         * 与 VMFUNC 也互斥，这一条原先两边都漏了。
         *
         * 漏掉的后果比"少一次提示"重：驱动确实会拒绝这个组合，但它返回的
         * STATUS_INVALID_PARAMETER 在 START_RESIDENT 上没有对应的状态映射，
         * 落到兜底分支报成 RENDEZVOUS_FAILED —— 一个与真因毫无关系的名字，
         * 会把人引到多核同步那边去查。这一项自己的说明文字里就写着"与 VMFUNC
         * 互斥"，只是代码没照着做。
         */
        if (checked && ksword::hvm::isVmFuncEnabled())
        {
            localEptAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("私有 EPT 与 VMFUNC 互斥")),
                ks::i18n::sourceText(QStringLiteral("VMFUNC 要求所有处理器共享同一份 EPTP list，而私有 EPT 要给每个处理器各自一份层次，驱动会拒绝同时请求。请先关掉「武装 VMFUNC / EPTP 切换」。")));
            return;
        }
        /*
         * 与嵌套派发也互斥，方向必须补齐。
         *
         * 这一项自己的说明文字里早就写着"与 VMFUNC、嵌套 VMX 互斥"，但嵌套那
         * 一半从来没有对应的代码——因为在嵌套派发有开关之前，这个组合攒不出来。
         * 现在攒得出来了，缺的这一半就得补上，否则从这一侧进来的人照样会撞上
         * 那句不指出哪一位的"请求不合法"。
         */
        if (checked && ksword::hvm::isNestedDispatchEnabled())
        {
            localEptAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("私有 EPT 与嵌套派发互斥")),
                ks::i18n::sourceText(QStringLiteral("嵌套要把来宾的 EPT 层次和我们的合成成一个 EPT 指针，而私有 EPT 要给每个处理器各自一份层次，那会让这个合成变成处理器相关的。驱动会拒绝同时请求。请先关掉「允许来宾嵌套（我们作为宿主）」。")));
            return;
        }
        ksword::hvm::setLocalEptEnabled(checked);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    // EPTP 切换后端：这一项选的不是"要不要一个能力"，而是"用哪套机器装视图"。
    // 关掉时行为与默认后端逐字节相同，所以不走高风险确认。选它的唯一理由是
    // 默认后端要 Monitor Trap Flag，而嵌套 Hyper-V 客户机拿不到 MTF。
    QAction* const eptpSwitchAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("EPT 分离视图用 EPTP 切换后端")));
    eptpSwitchAction->setCheckable(true);
    eptpSwitchAction->setChecked(ksword::hvm::isEptpSwitchEnabled());
    eptpSwitchAction->setToolTip(ks::i18n::sourceText(QStringLiteral("默认后端是「写 EPT 叶 + 用 Monitor Trap Flag 单步一条指令 + 写回去」，它要求处理器提供 Monitor Trap Flag。EPTP 切换后端换成在两份层次之间切 EPTP，只要 execute-only EPT 叶，既不要 MTF 也不要 VMFUNC。差别不是性能而是能力：嵌套 Hyper-V 客户机拿不到 MTF，那种机器上只有这套后端能装上视图。与私有 EPT、VMFUNC 互斥。这一位只随准备资源发出，改完要重新准备才生效。")));
    connect(eptpSwitchAction, &QAction::triggered, this, [this, eptpSwitchAction](const bool checked) {
        if (!checked)
        {
            ksword::hvm::setEptpSwitchEnabled(false);
            applyHvmButtonState();
            refreshHvmStatusAsync();
            return;
        }
        if (ksword::hvm::isLocalEptEnabled() || ksword::hvm::isVmFuncEnabled())
        {
            eptpSwitchAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("EPTP 切换后端与私有 EPT、VMFUNC 互斥")),
                ks::i18n::sourceText(QStringLiteral("EPTP 切换后端要在多份 EPT 层次之间换 EPTP：私有 EPT 要给每个处理器各自一份层次，VMFUNC 又要所有处理器共享同一份 EPTP list，两者都与它冲突。驱动会在分配任何资源之前拒绝同时请求。请先关掉「每处理器私有 EPT」与「武装 VMFUNC 视图切换」。")));
            return;
        }
        ksword::hvm::setEptpSwitchEnabled(true);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    // 写权限门：关闭时 HVM 只做观测，所有 R-1 改写能力都不可用。
    QAction* const writeAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("允许 R-1 写操作")));
    writeAction->setCheckable(true);
    writeAction->setChecked(ksword::hvm::isWriteAccessEnabled());
    connect(writeAction, &QAction::triggered, this, [this](const bool checked) {
        if (!checked)
        {
            ksword::hvm::setWriteAccessEnabled(false);
            applyHvmButtonState();
            refreshHvmStatusAsync();
            return;
        }
        // 打开写权限等于解锁一整类可改写系统状态的能力，必须显式确认一次。
        //
        // 确认文案与 suppressionKey 只存在于 HvmWriteAccessGate 一处：向导的预检
        // 也要能就地开启，两个入口各拼一遍的话，勾过「不再提示」的用户会在另一个
        // 入口被重新弹一次，而两处文案只要差一个字就再也说不清用户同意过什么。
        (void)ks::ui::requestHvmWriteAccess(this);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    // #VE 反射：本模块唯一一个「配错就是当场蓝屏」的开关，门禁因此比写权限更多。
    // guest 就是正在跑的这台 Windows，它的 IDT[20] 没有 #VE 处理程序。
    QAction* const veAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("武装 #VE 反射（危险）")));
    veAction->setCheckable(true);
    veAction->setChecked(ksword::hvm::isVeEnabled());
    veAction->setToolTip(ks::i18n::sourceText(QStringLiteral("把 EPT violation 反射成 guest 的 #VE（向量 20）。驱动侧有两道独立保险：所有 EPT 叶项都带 suppress-#VE，每 CPU 的信息区分配时即锁 busy。两道都在时，控制位开着也投递不出 #VE。本开关不持久化，重启客户端即回到关闭。")));
    connect(veAction, &QAction::triggered, this, [this, veAction](const bool checked) {
        if (!checked)
        {
            ksword::hvm::setVeEnabled(false);
            applyHvmButtonState();
            refreshHvmStatusAsync();
            return;
        }
        // 门禁一：裸机宿主机上不给开。#VE 配错的代价是整台机器立刻重启，
        // 唯一可以承受这个代价的地方是虚拟机里。
        if (!ksword::hvm::isNestedAllowed())
        {
            veAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("#VE 需要先进入嵌套模式")),
                ks::i18n::sourceText(QStringLiteral("#VE 只能在嵌套模式下武装，也就是只能在虚拟机里。配错的代价是整台机器立刻 triple fault 重启，这个代价只有虚拟机承受得起。请先打开「允许嵌套运行（作为 L1）」。")));
            return;
        }
        // 门禁二：写权限是前置，#VE 属于会改变 guest 可见行为的一类能力。
        if (!ksword::hvm::isWriteAccessEnabled())
        {
            veAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("#VE 需要先开启写权限")),
                ks::i18n::sourceText(QStringLiteral("武装 #VE 会改变 guest 可见的异常行为，属于写权限门管辖的范围。请先打开「允许 R-1 写操作」。")));
            return;
        }
        // 门禁三：标准高风险确认。
        const bool confirmed = ks::ui::confirmDestructiveAction(
            this,
            QStringLiteral("HvmEnableVe"),
            ks::i18n::sourceText(QStringLiteral("武装 #VE 反射")),
            ks::i18n::sourceText(QStringLiteral("当前虚拟机的运行中内核")),
            ks::i18n::sourceText(QStringLiteral("#VE 把 EPT violation 变成 guest 内部的 20 号异常。这里的 guest 就是正在跑的这个 Windows，它没有 #VE 处理程序，真投递一次就是 #GP 转 #DF 转 triple fault，虚拟机当场重启且不会留下崩溃转储。")));
        if (!confirmed)
        {
            veAction->setChecked(false);
            return;
        }
        // 门禁四：最后一次，默认落在取消上。
        const auto answer = QMessageBox::warning(
            this,
            ks::i18n::sourceText(QStringLiteral("确认武装 #VE")),
            ks::i18n::sourceText(QStringLiteral("武装后驱动仍有两道保险挡着实际投递：所有 EPT 叶项都带 suppress-#VE，每 CPU 的信息区出厂即锁 busy。因此这一步得到的是「控制位已武装」，不是「#VE 已生效」。要真的收到 #VE，还需要在 guest 里装好处理程序、清掉 busy、再把目标页显式设为可转换——那三步本客户端不提供。确定要武装吗？")),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        const bool armed = answer == QMessageBox::Yes;
        veAction->setChecked(armed);
        ksword::hvm::setVeEnabled(armed);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    // VMFUNC：不会蓝屏，但会把 EPTP list 里的每个域发布给任意 ring 3 线程。
    QAction* const vmFuncAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("武装 VMFUNC 视图切换")));
    vmFuncAction->setCheckable(true);
    vmFuncAction->setChecked(ksword::hvm::isVmFuncEnabled());
    vmFuncAction->setToolTip(ks::i18n::sourceText(QStringLiteral("武装后 guest 用一条 VMFUNC 就能在 EPTP list 的域之间切换，不产生 VM exit，驱动也收不到通知。VMFUNC 不做 CPL 检查，所以任意进程的任意 ring 3 线程都能切。这不是提权路径（域只能被拿掉权限），但确实是驱动观测不到的状态切换。本开关不持久化。")));
    connect(vmFuncAction, &QAction::triggered, this, [this, vmFuncAction](const bool checked) {
        if (!checked)
        {
            ksword::hvm::setVmFuncEnabled(false);
            applyHvmButtonState();
            refreshHvmStatusAsync();
            return;
        }
        // 与私有 EPT 同样的互斥：VMFUNC 要所有处理器共享同一份 EPTP list，
        // 而 EPTP 切换后端要在多份层次之间换 EPTP，驱动会直接拒绝这个组合。
        if (ksword::hvm::isEptpSwitchEnabled())
        {
            vmFuncAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("VMFUNC 与 EPTP 切换后端互斥")),
                ks::i18n::sourceText(QStringLiteral("VMFUNC 要求所有处理器共享同一份 EPTP list，而 EPTP 切换后端要在多份 EPT 层次之间换 EPTP，驱动会在分配任何资源之前拒绝同时请求。请先关掉「EPT 分离视图用 EPTP 切换后端」。")));
            return;
        }
        // 与私有 EPT 也互斥。理由同私有 EPT 那一项：驱动的拒绝会以
        // RENDEZVOUS_FAILED 这个与真因无关的名字出现，在这里拦下更省事。
        if (ksword::hvm::isLocalEptEnabled())
        {
            vmFuncAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("VMFUNC 与私有 EPT 互斥")),
                ks::i18n::sourceText(QStringLiteral("VMFUNC 要求所有处理器共享同一份 EPTP list，而私有 EPT 要给每个处理器各自一份层次，驱动会拒绝同时请求。请先关掉「每处理器私有 EPT」。")));
            return;
        }
        // 写权限前置：武装一个 guest 可见的切换接口属于改变系统行为。
        if (!ksword::hvm::isWriteAccessEnabled())
        {
            vmFuncAction->setChecked(false);
            QMessageBox::warning(
                this,
                ks::i18n::sourceText(QStringLiteral("VMFUNC 需要先开启写权限")),
                ks::i18n::sourceText(QStringLiteral("武装 VMFUNC 会让 guest 获得一个驱动观测不到的视图切换能力，属于写权限门管辖的范围。请先打开「允许 R-1 写操作」。")));
            return;
        }
        const bool confirmed = ks::ui::confirmDestructiveAction(
            this,
            QStringLiteral("HvmEnableVmFunc"),
            ks::i18n::sourceText(QStringLiteral("武装 VMFUNC 视图切换")),
            ks::i18n::sourceText(QStringLiteral("EPTP list 中的全部执行域")),
            ks::i18n::sourceText(QStringLiteral("VMFUNC 不做 CPL 检查：武装之后，任意进程里的任意用户态线程都能用一条指令切换当前使用的 EPT 视图，不产生 VM exit，驱动也不会被通知。域只能被拿掉权限，所以切过去拿不到新的访问权，但这仍然是一个你观测不到的状态变化。")));
        vmFuncAction->setChecked(confirmed);
        ksword::hvm::setVmFuncEnabled(confirmed);
        applyHvmButtonState();
        refreshHvmStatusAsync();
    });

    menu.addSeparator();

    // R-1 内存面板不要求常驻：私有页表窗口在驱动加载时就已建立。
    QAction* const memoryAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("R-1 内存操作...")));
    memoryAction->setEnabled(m_r0DriverServiceRunning && backendKnown);
    connect(memoryAction, &QAction::triggered, this, [this]() {
        // 无父窗口模态：内存面板要能和主界面并排使用。
        HvmMemoryDialog* const dialog = new HvmMemoryDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    // EPT 视图同样不要求常驻：它是安装在 EPT 上的，常驻期间反而不能改。
    QAction* const viewAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("EPT 分离视图（隐蔽 Hook / 内存隐藏）...")));
    viewAction->setEnabled(m_r0DriverServiceRunning);
    connect(viewAction, &QAction::triggered, this, [this]() {
        HvmViewDialog* const dialog = new HvmViewDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    // 执行域面板：域是默认视图的分叉，只能被拿掉权限，不能被加权限。
    QAction* const domainAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("EPT 执行域（VMFUNC 可切换）...")));
    domainAction->setEnabled(m_r0DriverServiceRunning);
    connect(domainAction, &QAction::triggered, this, [this]() {
        HvmDomainDialog* const dialog = new HvmDomainDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    // MSR 策略同样在未常驻时配置：位图是活的硬件状态，常驻期间不能改。
    QAction* const msrAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("MSR 策略...")));
    msrAction->setEnabled(m_r0DriverServiceRunning);
    connect(msrAction, &QAction::triggered, this, [this]() {
        HvmMsrPolicyDialog* const dialog = new HvmMsrPolicyDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    // 控制寄存器策略同样在建 VMCS 时消费，必须在常驻启动前配置。
    QAction* const crAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("控制寄存器策略...")));
    crAction->setEnabled(m_r0DriverServiceRunning);
    connect(crAction, &QAction::triggered, this, [this]() {
        HvmCrPolicyDialog* const dialog = new HvmCrPolicyDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    // 事件流是只读的，任何时候都能看——它是上面几项能力唯一的实时证据。
    QAction* const eventAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("事件流...")));
    eventAction->setEnabled(m_r0DriverServiceRunning && backendKnown);
    connect(eventAction, &QAction::triggered, this, [this]() {
        HvmEventDialog* const dialog = new HvmEventDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    // R-1 进程处置与注入。这一项此前只能经「完整命令面板」下达，而那条路是把
    // 主程序当 hvm_ctl 子进程拉起来的探针通路，已整条摘除；能力本身有专属
    // IOCTL，所以在这里补一个与其它面板同形状的入口。
    QAction* const processAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("R-1 进程处置与注入...")));
    processAction->setEnabled(m_r0DriverServiceRunning);
    connect(processAction, &QAction::triggered, this, [this]() {
        HvmProcessDialog* const dialog = new HvmProcessDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    menu.addSeparator();

    // 故障重置只清可恢复标记，常驻中会被驱动拒绝。
    QAction* const resetAction = menu.addAction(
        ks::i18n::sourceText(QStringLiteral("重置故障状态")));
    resetAction->setEnabled(!m_hvmOperationRunning && m_hvmFaulted);
    connect(resetAction, &QAction::triggered, this, [this]() {
        runHvmFaultReset();
    });

    // AMD 后端：把建立在 EPT 分离视图或 VMCS 字段上的入口灰掉。
    //
    // 与 HvmDock 上那组门同一个判据、同一句说明——这两处各判各的，用户会在一个
    // 入口里按不动、在另一个入口里按了没反应。
    //
    // Intel 专属偏好保留，并仅在 Intel 后端显示和应用。
    const bool intelBackend = m_hvmBackend == KSWORD_ARK_HVM_BACKEND_VMX;
    for (QAction* const action : { viewAction, domainAction, msrAction, crAction, processAction,
                                  soakAction, localEptAction, eptpSwitchAction, veAction, vmFuncAction, hideHypervisorAction })
    {
        action->setVisible(intelBackend);
    }
    if (m_hvmBackend == KSWORD_ARK_HVM_BACKEND_NONE)
    {
        nestedDispatchAction->setEnabled(false);
    }

    menu.exec(globalPosition);
}
