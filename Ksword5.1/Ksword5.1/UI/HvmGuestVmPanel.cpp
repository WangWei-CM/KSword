#include "HvmGuestVmPanel.h"
#include "./AdaptivePageScroll.h"
#include "./PrimaryPageStyle.h"
#include "./ToolbarMetrics.h"
#include "./FlatButtonTheme.h"

#include "../Internationalization/LanguageManager.h"
#include "../ksword/service/service.h"
#include "../theme.h"
#include "HvmControl.h"
#include "ThemeStatusRole.h"
#include "../Framework/DestructiveActionConfirmation.h"
#include <windows.h>
#include <tlhelp32.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

#include <thread>

// 本文件里每一条面向用户的长句都写在**一行**里，不拆成相邻字面量。
// 拆行的话，词条抽取工具会把每个片段都当成一条独立词条收进语言包，而运行时
// `sourceText` 拿到的是拼接后的整串 —— 于是包里多出一堆永远匹配不上的碎片，
// 真正那一条却仍然缺失。仓库里现有的长 tooltip 也都是单行。

namespace
{
    // VMware 的 VMX 驱动服务名。它只在自己启动时问一次 CPU 能力并记住，
    // 所以前面几步改完之后必须让它重新问一遍。
    const wchar_t* const kVmwareDriverService = L"vmx86";

    // Win32 SERVICE_STOPPED / SERVICE_RUNNING，避免为两个常量拉进 winsvc.h。
    constexpr std::uint32_t kServiceStopped = 1U;
    constexpr std::uint32_t kServiceRunning = 4U;

    QString doneMark()
    {
        return ks::i18n::sourceText(QStringLiteral("已完成"));
    }

    QString todoMark()
    {
        return ks::i18n::sourceText(QStringLiteral("还没做"));
    }

    void paintStatus(QLabel* label, const QString& text, bool done)
    {
        if (label == nullptr) { return; }
        label->setText(text);
        ks::ui::ApplyStatusRole(label, done ? ks::ui::StatusRole::Success : ks::ui::StatusRole::Warning);
    }
}

HvmGuestVmPanel::HvmGuestVmPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* const outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* const scroll = new QScrollArea(this);
    // 明确登记唯一的页面滚动壳，外框留白由共享框架计算，步骤卡片保留内部留白。
    scroll->setObjectName(QString::fromLatin1(ks::ui::kAdaptivePageScrollObjectName));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* const host = new QWidget(scroll);
    auto* const layout = new QVBoxLayout(host);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    m_intro = new QLabel(host);
    m_intro->setWordWrap(true);
    m_intro->setText(ks::i18n::sourceText(QStringLiteral("Intel 嵌套 VMX 允许来宾使用虚拟化指令。下面按顺序设置外层许可、来宾嵌套和身份选项，再准备、自检与启动，最后按需重新识别 VMware 能力。驱动确认启用后，仍需单独验证第三方虚拟机；这些步骤不保证所有软件兼容。")));
    layout->addWidget(m_intro);

    auto* const buttonRow = new QHBoxLayout();
    m_doAll = new QPushButton(
        ks::i18n::sourceText(QStringLiteral("一键完成全部五步")), host);
    m_doAll->setToolTip(ks::i18n::sourceText(QStringLiteral("按顺序执行：打开三个设置，准备并启动 KSwordVM，最后重启 VMware 的驱动服务。每一步的结果都会显示在下面的清单里。")));
    connect(m_doAll, &QPushButton::clicked, this, [this]() {
        if (!confirmActivation()) { return; }
        runInBackground([completion = m_vmwareDriverRestarted]() -> QString {
            const auto before = ksword::hvm::queryState();
            if (!before.residentActive) { enableAllSwitches(); }
            const QString startError = startMonitor();
            if (!startError.isEmpty()) { return startError; }
            const QString restartError = restartVmwareDriver(completion);
            if (!restartError.isEmpty()) { return restartError; }
            return ks::i18n::sourceText(QStringLiteral("流程已完成；请查看驱动回读状态。内层系统运行仍需单独验证。"));
        });
    });
    buttonRow->addWidget(m_doAll);

    m_refresh = new QPushButton(
        ks::i18n::sourceText(QStringLiteral("刷新状态")), host);
    connect(m_refresh, &QPushButton::clicked, this, [this]() { refreshAsync(); });
    buttonRow->addWidget(m_refresh);
    buttonRow->addStretch(1);
    ks::ui::NormalizeToolbarRow(buttonRow);
    layout->addLayout(buttonRow);

    m_stepAllowNested = addStep(layout, 1,
        ks::i18n::sourceText(QStringLiteral("允许 KSwordVM 运行在虚拟机里")),
        ks::i18n::sourceText(QStringLiteral("仅在当前外层明确提供嵌套虚拟化时允许进入。此许可不能绕过 Hyper-V/VBS 的硬件准入拒绝；裸机无需开启。")),
        ks::i18n::sourceText(QStringLiteral("打开")),
        [this]() {
            runInBackground([completion = m_vmwareDriverRestarted]() -> QString {
                ksword::hvm::setNestedAllowed(true);
                return ks::i18n::sourceText(QStringLiteral("第 1 步已打开。"));
            });
        });

    m_stepHostGuests = addStep(layout, 2,
        ks::i18n::sourceText(QStringLiteral("允许别的虚拟机运行在 KSwordVM 下面")),
        ks::i18n::sourceText(QStringLiteral("这一项和上一项方向相反：上一项是让 KSwordVM 跑在别人下面，这一项是让别人跑在 KSwordVM 下面。不打开的话，VMware 一按「开启此虚拟机」就会失败。它有两个前提：需要先打开「允许 R-1 写操作」，并且关掉「每处理器私有 EPT」，后者和这一项不能同时开，同时开会被整条拒绝。")),
        ks::i18n::sourceText(QStringLiteral("打开")),
        [this]() {
            if (!confirmActivation()) { return; }
            runInBackground([completion = m_vmwareDriverRestarted]() -> QString {
                if (ksword::hvm::queryState().backend == KSWORD_ARK_HVM_BACKEND_VMX && ksword::hvm::isLocalEptEnabled())
                {
                    return ks::i18n::sourceText(QStringLiteral("打不开：「每处理器私有 EPT」正开着，它和这一项不能同时开，请先关掉它。"));
                }
                if (!ksword::hvm::isWriteAccessEnabled())
                {
                    return ks::i18n::sourceText(QStringLiteral("打不开：需要先打开「允许 R-1 写操作」。"));
                }
                ksword::hvm::setNestedDispatchEnabled(true);
                return ks::i18n::sourceText(QStringLiteral("第 2 步已打开。"));
            });
        });

    m_stepHideIdentity = addStep(layout, 3,
        ks::i18n::sourceText(QStringLiteral("对虚拟机软件隐藏 KSwordVM 的身份")),
        ks::i18n::sourceText(QStringLiteral("这一步不能省。VMware 启动时会先检查 CPU 上有没有别的虚拟化软件，一旦发现就直接弹「与 Hyper-V 不兼容」并退出，它连能力都不会去问，所以前两步做得再对也救不回来。打开之后虚拟机软件就看不到 KSwordVM 了。")),
        ks::i18n::sourceText(QStringLiteral("打开")),
        [this]() {
            runInBackground([completion = m_vmwareDriverRestarted]() -> QString {
                ksword::hvm::setHypervisorHidden(true);
                return ks::i18n::sourceText(QStringLiteral("第 3 步已打开。"));
            });
        });

    m_stepStartMonitor = addStep(layout, 4,
        ks::i18n::sourceText(QStringLiteral("启动 KSwordVM")),
        ks::i18n::sourceText(QStringLiteral("分配资源、做一次自检，然后正式接管 CPU 的虚拟化功能。前三步是设置，只有走完这一步它们才真正生效：设置是在启动的那一刻被读取的，启动之后再改开关不会影响已经跑起来的这一份。")),
        ks::i18n::sourceText(QStringLiteral("启动")),
        [this]() {
            if (!confirmActivation()) { return; }
            runInBackground([completion = m_vmwareDriverRestarted]() -> QString {
                return startMonitor();
            });
        });

    m_stepRestartVmware = addStep(layout, 5,
        ks::i18n::sourceText(QStringLiteral("让 VMware 重新识别 CPU 能力")),
        ks::i18n::sourceText(QStringLiteral("VMware 的驱动只在它自己启动的时候问一次 CPU 支持哪些虚拟化能力，问完就记住了。前面几步改完之后它手里还是旧答案，所以必须让它重启一次重新问。这一步动的是 VMware 自己的服务，不是 KSwordVM；重启前请先关掉所有正在运行的虚拟机。")),
        ks::i18n::sourceText(QStringLiteral("重启 VMware 驱动服务")),
        [this]() {
            runInBackground([completion = m_vmwareDriverRestarted]() -> QString {
                return restartVmwareDriver(completion);
            });
        });

    auto* const line = new QFrame(host);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    m_verdict = new QLabel(host);
    m_verdict->setWordWrap(true);
    layout->addWidget(m_verdict);

    m_lastMessage = new QLabel(host);
    m_lastMessage->setWordWrap(true);
    m_lastMessage->setStyleSheet(
        QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    layout->addWidget(m_lastMessage);

    layout->addStretch(1);
    scroll->setWidget(host);
    outer->addWidget(scroll);
}

HvmGuestVmPanel::StepRow HvmGuestVmPanel::addStep(
    QVBoxLayout* parentLayout,
    const int number,
    const QString& title,
    const QString& explanation,
    const QString& actionText,
    const std::function<void()>& onClicked)
{
    StepRow row;
    row.originalTitle = QStringLiteral("%1. %2").arg(number).arg(title);
    row.originalExplanation = explanation;

    auto* const box = new QFrame(parentLayout->parentWidget());
    row.container = box;
    // 五步（AMD 为四步）均由此工厂创建，使用共享内容面代替原生方形面板边框。
    box->setFrameShape(QFrame::NoFrame);
    ks::ui::StylePrimaryContentPanel(box);
    auto* const boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(12, 10, 12, 10);
    boxLayout->setSpacing(6);

    auto* const headerRow = new QHBoxLayout();
    auto* const titleLabel = new QLabel(
        QStringLiteral("%1. %2").arg(number).arg(title), box);
    row.title = titleLabel;
    titleLabel->setStyleSheet(QStringLiteral("font-weight:600;"));
    titleLabel->setWordWrap(true);
    headerRow->addWidget(titleLabel, 1);

    row.status = new QLabel(box);
    headerRow->addWidget(row.status);

    row.action = new QPushButton(actionText, box);
    ks::ui::NormalizeToolbarControl(row.action);
    ks::ui::ApplyFlatButtonTheme(row.action,
        ks::ui::FlatButtonTone::Neutral, ks::ui::FlatButtonAppearance::Solid);
    connect(row.action, &QPushButton::clicked, this, onClicked);
    headerRow->addWidget(row.action);
    boxLayout->addLayout(headerRow);

    auto* const why = new QLabel(explanation, box);
    row.explanation = why;
    why->setWordWrap(true);
    why->setStyleSheet(
        QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    boxLayout->addWidget(why);

    parentLayout->addWidget(box);
    return row;
}

void HvmGuestVmPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshAsync();
}

void HvmGuestVmPanel::setBusy(const bool busy)
{
    const bool changed = m_busy != busy;
    m_busy = busy;
    // 「刷新」不看后端：读一次状态在哪台机器上都成立，而它正是用户在 AMD 上
    // 唯一还能按的东西——把它一起灰掉，这一页就没有任何出口了。
    if (m_refresh != nullptr) { m_refresh->setEnabled(!busy); }
    const bool actionsEnabled = !busy && m_backendSupported;
    if (m_doAll != nullptr) { m_doAll->setEnabled(actionsEnabled); }
    for (StepRow* const row : { &m_stepAllowNested, &m_stepHostGuests,
                                &m_stepHideIdentity, &m_stepStartMonitor,
                                &m_stepRestartVmware })
    {
        if (row->action != nullptr) { row->action->setEnabled(actionsEnabled); }
    }
    if (changed && onBusyChanged) { onBusyChanged(busy); }
}

void HvmGuestVmPanel::runInBackground(const std::function<QString()>& work)
{
    if (m_busy) { return; }
    markConfigurationChanged();
    setBusy(true);
    QPointer<HvmGuestVmPanel> safeThis(this);
    std::thread([safeThis, work]() {
        QString message;
        if (work) { message = work(); }
        const ksword::hvm::HvmState state = ksword::hvm::queryState();
        ks::service::ServiceStatus vmwareStatus{};
        std::uint32_t serviceError = 0;
        const bool vmwareInstalled = ks::service::QueryServiceStatus(kVmwareDriverService, &vmwareStatus, nullptr, &serviceError);
        const bool vmwareQueryValid = vmwareInstalled || serviceError == ERROR_SERVICE_DOES_NOT_EXIST;
        if (safeThis == nullptr) { return; }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, state, message, vmwareInstalled, vmwareQueryValid]() {
                if (safeThis == nullptr) { return; }
                safeThis->setBusy(false);
                if (!message.isEmpty() && safeThis->m_lastMessage != nullptr)
                {
                    safeThis->m_lastMessage->setText(message);
                }
                safeThis->m_vmwareInstalled = vmwareInstalled;
                safeThis->m_vmwareQueryValid = vmwareQueryValid;
                safeThis->applyState(state);
            },
            Qt::QueuedConnection);
    }).detach();
}

void HvmGuestVmPanel::refreshAsync()
{
    if (m_busy || m_queryInFlight) { return; }
    m_queryInFlight = true;
    QPointer<HvmGuestVmPanel> safeThis(this);
    std::thread([safeThis]() {
        const ksword::hvm::HvmState state = ksword::hvm::queryState();
        ks::service::ServiceStatus vmwareStatus{};
        std::uint32_t serviceError = 0;
        const bool vmwareInstalled = ks::service::QueryServiceStatus(kVmwareDriverService, &vmwareStatus, nullptr, &serviceError);
        const bool vmwareQueryValid = vmwareInstalled || serviceError == ERROR_SERVICE_DOES_NOT_EXIST;
        if (safeThis == nullptr) { return; }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, state, vmwareInstalled, vmwareQueryValid]() {
                if (safeThis == nullptr) { return; }
                safeThis->m_queryInFlight = false;
                safeThis->m_vmwareInstalled = vmwareInstalled;
                safeThis->m_vmwareQueryValid = vmwareQueryValid;
                safeThis->applyState(state);
            },
            Qt::QueuedConnection);
    }).detach();
}

void HvmGuestVmPanel::markConfigurationChanged()
{
    m_vmwareDriverRestarted->store(0ULL);
}

bool HvmGuestVmPanel::confirmActivation()
{
    if (!ksword::hvm::isWriteAccessEnabled())
    {
        m_lastMessage->setText(ks::i18n::sourceText(QStringLiteral("请先在标题栏虚拟化菜单开启允许 R-1 写操作。")));
        return false;
    }
    return ks::ui::confirmDestructiveAction(this, QStringLiteral("HvmGuestActivate"),
        ks::i18n::sourceText(QStringLiteral("启用来宾嵌套与常驻")),
        ks::i18n::sourceText(QStringLiteral("本机全部逻辑处理器")),
        ks::i18n::sourceText(QStringLiteral("将允许 ring 0 代码运行内层虚拟机，并尝试全核常驻。AMD 嵌套仍为实验性，内层系统尚未完成验收。已运行虚拟机必须先关闭；任何一步失败会停止后续流程。")));
}

void HvmGuestVmPanel::enableAllSwitches()
{
    const auto state = ksword::hvm::queryState();
    if (state.hypervisorPresent) { ksword::hvm::setNestedAllowed(true); }
    if (ksword::hvm::isWriteAccessEnabled() &&
        (state.backend == KSWORD_ARK_HVM_BACKEND_SVM || !ksword::hvm::isLocalEptEnabled()))
    {
        ksword::hvm::setNestedDispatchEnabled(true);
    }
    if (state.backend == KSWORD_ARK_HVM_BACKEND_VMX) { ksword::hvm::setHypervisorHidden(true); }
}

QString HvmGuestVmPanel::startMonitor()
{
    const auto before = ksword::hvm::queryState();
    if (!before.configurationReason.isEmpty()) { return before.configurationReason; }
    if (before.residentActive)
    {
        if (!before.residentComplete || !before.nestedArmed ||
            (before.backend == KSWORD_ARK_HVM_BACKEND_VMX && !before.identityHidden))
        {
            return ks::i18n::sourceText(QStringLiteral("当前常驻未全核启用来宾嵌套；请停止常驻、释放资源后重新准备。"));
        }
        return QString();
    }
    if (!ksword::hvm::isNestedDispatchEnabled())
    {
        return ks::i18n::sourceText(QStringLiteral("请先开启允许来宾嵌套，再准备资源与启动常驻。"));
    }
    const auto started = ksword::hvm::startResident(before.generation);
    if (!started.ok) { return started.message; }
    const auto after = ksword::hvm::queryState();
    if (!after.residentComplete || !after.nestedArmed ||
        (after.backend == KSWORD_ARK_HVM_BACKEND_VMX && !after.identityHidden))
    {
        return ks::i18n::sourceText(QStringLiteral("驱动尚未确认全核常驻与嵌套启用，未继续重启 VMware 服务。"));
    }
    return QString();
}

QString HvmGuestVmPanel::restartVmwareDriver(const std::shared_ptr<std::atomic<unsigned long long>>& completion)
{
    const auto state = ksword::hvm::queryState();
    if (!state.residentComplete || !state.nestedArmed || state.faulted || !state.configurationReason.isEmpty() ||
        (state.backend == KSWORD_ARK_HVM_BACKEND_VMX && !state.identityHidden))
    {
        return ks::i18n::sourceText(QStringLiteral("驱动尚未确认全核常驻与嵌套启用，未继续重启 VMware 服务。"));
    }
    ks::service::ServiceStatus status{};
    std::uint32_t serviceError = 0;
    if (!ks::service::QueryServiceStatus(kVmwareDriverService, &status, nullptr, &serviceError))
    {
        if (serviceError == ERROR_SERVICE_DOES_NOT_EXIST) { return QString(); }
        return ks::i18n::sourceText(QStringLiteral("无法查询 VMware 驱动服务，未继续重启。"));
    }
    // Never stop a VMware driver while its VM process is alive.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return ks::i18n::sourceText(QStringLiteral("无法检查运行中的虚拟机，未重启 VMware 服务。"));
    }
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    bool activeVm = false;
    if (Process32FirstW(snapshot, &process))
    {
        do { if (_wcsicmp(process.szExeFile, L"vmware-vmx.exe") == 0) { activeVm = true; break; } }
        while (Process32NextW(snapshot, &process));
    }
    else { activeVm = true; }
    CloseHandle(snapshot);
    if (activeVm) { return ks::i18n::sourceText(QStringLiteral("请先关闭所有 VMware 虚拟机，再重新识别 CPU 能力。")); }
    if (!ks::service::StopServiceByName(kVmwareDriverService, 15000U, kServiceStopped) ||
        !ks::service::StartServiceByName(kVmwareDriverService, 15000U, kServiceRunning))
    {
        return ks::i18n::sourceText(QStringLiteral("重启 VMware 驱动服务失败，请检查服务状态。"));
    }
    const auto after = ksword::hvm::queryState();
    if (after.generation != state.generation || !after.residentComplete || !after.nestedArmed || after.faulted)
    {
        return ks::i18n::sourceText(QStringLiteral("VMware 服务已完成重启，但驱动代次或常驻状态已变化；请刷新后重新检查。"));
    }
    completion->store(static_cast<unsigned long long>(after.generation) + 1ULL);
    return QString();
}

void HvmGuestVmPanel::applyState(const ksword::hvm::HvmState& state)
{
    m_backend = state.backend;
    const bool amd = state.backend == KSWORD_ARK_HVM_BACKEND_SVM;
    m_backendSupported = (state.backend == KSWORD_ARK_HVM_BACKEND_VMX || amd) && state.nestedSupported;
    const auto rows = { &m_stepAllowNested, &m_stepHostGuests, &m_stepHideIdentity, &m_stepStartMonitor, &m_stepRestartVmware };
    for (StepRow* row : rows)
    {
        row->title->setText(row->originalTitle);
        row->explanation->setText(row->originalExplanation);
    }
    m_stepHideIdentity.container->setVisible(!amd);
    m_stepAllowNested.action->setEnabled(state.hypervisorPresent);
    if (!m_intro->property("ks_intel_text").isValid()) { m_intro->setProperty("ks_intel_text", m_intro->text()); }
    if (amd)
    {
        m_intro->setText(ks::i18n::sourceText(QStringLiteral("AMD 使用实验性嵌套 SVM：虚拟 VMCB、权限图合并、退出反射和 NPT 合成。请在准备资源前开启来宾嵌套；AMD 不使用 Intel 身份隐藏选项。实际启用仍不代表内层操作系统或所有第三方软件已通过验收。")));
        m_doAll->setText(ks::i18n::sourceText(QStringLiteral("按顺序配置并启动 AMD 嵌套")));
        m_doAll->setToolTip(ks::i18n::sourceText(QStringLiteral("设置适用的嵌套选项，准备与自检，再启动；失败立即停止，成功后按需重新识别 VMware 能力。")));
        m_stepHostGuests.title->setText(ks::i18n::sourceText(QStringLiteral("2. 允许来宾嵌套 SVM（实验性）")));
        m_stepHostGuests.explanation->setText(ks::i18n::sourceText(QStringLiteral("需要允许 R-1 写操作；此配置在准备资源时确定。更改后必须停止常驻、释放资源并重新准备，不能仅在启动时切换。")));
        m_stepStartMonitor.title->setText(ks::i18n::sourceText(QStringLiteral("3. 准备、自检并启动全核 SVM 常驻")));
        m_stepStartMonitor.explanation->setText(ks::i18n::sourceText(QStringLiteral("先准备所选 SVM 模式，再逐核自检，最后启动全核常驻；任一阶段失败立即停止。菜单开关仅表示请求，以驱动回读的嵌套实际启用为准。")));
        m_stepRestartVmware.title->setText(ks::i18n::sourceText(QStringLiteral("4. 按需让 VMware 重新识别 CPU 能力")));
        m_stepAllowNested.explanation->setText(ks::i18n::sourceText(QStringLiteral("裸机无需外层嵌套许可。作为来宾运行时仅支持显式允许的 VMware 外层；Hyper-V/VBS 和未知外层不会因打开此选项而获准。")));
    }
    else
    {
        m_intro->setText(m_intro->property("ks_intel_text").toString());
        m_doAll->setText(ks::i18n::sourceText(QStringLiteral("一键完成全部五步")));
    }
    setBusy(m_busy);
    m_stepAllowNested.action->setEnabled(!m_busy && m_backendSupported && state.hypervisorPresent && !state.residentActive);
    m_stepHostGuests.action->setEnabled(!m_busy && m_backendSupported && !state.residentActive);
    m_stepHideIdentity.action->setEnabled(!m_busy && m_backendSupported && !state.residentActive);
    m_stepStartMonitor.action->setEnabled(!m_busy && m_backendSupported &&
        ((state.residentAdmission && state.configurationReason.isEmpty()) ||
         (state.residentComplete && state.nestedArmed && !state.faulted)));
    m_doAll->setEnabled(m_stepStartMonitor.action->isEnabled());
    m_stepRestartVmware.action->setEnabled(!m_busy && m_backendSupported && state.residentComplete && state.nestedArmed && !state.faulted && state.configurationReason.isEmpty());
    if (!m_backendSupported)
    {
        const QString reason = state.backend == KSWORD_ARK_HVM_BACKEND_NONE
            ? ks::i18n::sourceText(QStringLiteral("还读不到虚拟化后端，请先启动 R0 并刷新。"))
            : ks::i18n::sourceText(QStringLiteral("当前驱动没有发布来宾嵌套支持，请更新驱动或检查硬件准入状态。"));
        for (StepRow* row : rows) { paintStatus(row->status, reason, false); }
        m_verdict->setText(reason);
        return;
    }
    const bool allowNested = !state.hypervisorPresent || ksword::hvm::isNestedAllowed();
    const bool hostGuests = ksword::hvm::isNestedDispatchEnabled();
    const bool hideIdentity = amd || state.identityHidden;
    const bool running = state.residentComplete && state.nestedArmed;

    paintStatus(m_stepAllowNested.status,
                allowNested ? doneMark() : todoMark(), allowNested);
    paintStatus(m_stepHostGuests.status,
                state.nestedArmed ? doneMark() : hostGuests
                    ? ks::i18n::sourceText(QStringLiteral("已请求，尚未实际启用")) : todoMark(), state.nestedArmed);
    paintStatus(m_stepHideIdentity.status,
                hideIdentity ? doneMark() : ksword::hvm::isHypervisorHidden()
                    ? ks::i18n::sourceText(QStringLiteral("已请求，尚未实际启用")) : todoMark(), hideIdentity);
    paintStatus(m_stepStartMonitor.status,
                running
                    ? doneMark()
                    : ks::i18n::sourceText(QStringLiteral("没在运行")),
                running);

    const bool vmwareInstalled = m_vmwareInstalled;
    QString vmwareText;
    bool vmwareDone = false;
    if (!m_vmwareQueryValid)
    {
        vmwareText = ks::i18n::sourceText(QStringLiteral("查询失败"));
    }
    else if (!vmwareInstalled)
    {
        vmwareText = ks::i18n::sourceText(QStringLiteral("没装 VMware"));
        vmwareDone = true; // 没装就不需要这一步。
    }
    else if (m_vmwareDriverRestarted->load() == static_cast<unsigned long long>(state.generation) + 1ULL)
    {
        vmwareText = doneMark();
        vmwareDone = true;
    }
    else
    {
        vmwareText = ks::i18n::sourceText(QStringLiteral("还需重启一次"));
    }
    paintStatus(m_stepRestartVmware.status, vmwareText, vmwareDone);

    // 结论用一句人话，并且只在真正都满足时才说"可以了"。
    QString verdict;
    if (allowNested && hostGuests && hideIdentity && running && vmwareDone)
    {
        verdict = amd
            ? ks::i18n::sourceText(QStringLiteral("驱动已确认全核嵌套 SVM 启用。可继续单独验证内层系统；当前仍为实验性，不保证所有第三方软件兼容。"))
            : ks::i18n::sourceText(QStringLiteral("驱动已确认全核嵌套 VMX 启用；请继续验证第三方虚拟机运行。"));
    }
    else if (!running)
    {
        verdict = ks::i18n::sourceText(QStringLiteral("驱动尚未确认全核来宾嵌套启用，请按上面的顺序设置、准备、自检并启动。"));
    }
    else if (!hideIdentity)
    {
        verdict = ks::i18n::sourceText(QStringLiteral("还不行：没有隐藏身份，VMware 会直接报「与 Hyper-V 不兼容」并退出。这一项要在启动 KSwordVM 之前设好，改完需要重新启动 KSwordVM 才生效。"));
    }
    else if (!hostGuests)
    {
        verdict = ks::i18n::sourceText(QStringLiteral("还不行：没有允许别的虚拟机跑在 KSwordVM 下面，VMware 一开虚拟机就会失败。"));
    }
    else if (!m_vmwareQueryValid)
    {
        verdict = ks::i18n::sourceText(QStringLiteral("无法查询 VMware 驱动服务，未继续重启。"));
    }
    else if (!vmwareDone)
    {
        verdict = ks::i18n::sourceText(QStringLiteral("就差最后一步：VMware 的驱动手里还是旧的 CPU 能力答案，重启一次它的服务即可。"));
    }
    else
    {
        verdict = ks::i18n::sourceText(QStringLiteral("还有步骤没完成，请看上面的清单。"));
    }
    if (!state.configurationReason.isEmpty()) { verdict = state.configurationReason; }
    else if (!state.residentAdmission && !state.residentActive) { verdict = state.admissionReason; }
    if (m_verdict != nullptr)
    {
        m_verdict->setText(verdict);
        m_verdict->setStyleSheet(QStringLiteral("font-weight:600;"));
    }
}
