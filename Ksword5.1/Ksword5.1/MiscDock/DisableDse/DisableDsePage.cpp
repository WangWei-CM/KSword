// DisableDsePage.cpp
// 说明见 DisableDsePage.h。所有内核访问都在 DisableDseBackend 里，本文件只做界面与调度。

#include "DisableDsePage.h"
#include "KsecDdExecutor.h"
#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../UI/FlatButtonTheme.h"
#include "../../UI/SegmentedControl.h"

#include "../../Internationalization/LanguageManager.h"
#include "../../UI/CodeEditorWidget.h"
#include "../../theme.h"

#include <QApplication>
#include <QAbstractButton>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

#include <thread>

namespace
{
    using ks::misc::disable_dse::ApplyResult;
    using ks::misc::disable_dse::BlockReason;
    using ks::misc::disable_dse::CodeIntegrityPosture;
    using ks::misc::disable_dse::PostureSource;

    // formatHex32：
    // - 输入 value：32 位值；
    // - 作用：统一成 0xXXXXXXXX 的写法，便于和其他工具的输出对照；
    // - 返回：定宽十六进制文本。
    QString formatHex32(const std::uint32_t value)
    {
        return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0'));
    }

    // formatHex64：
    // - 输入 value：64 位地址；
    // - 作用：统一成 0x 前缀的定宽地址写法；
    // - 返回：定宽十六进制文本。
    QString formatHex64(const std::uint64_t value)
    {
        return QStringLiteral("0x%1").arg(value, 16, 16, QChar('0'));
    }

    // postureSourceText：
    // - 输入 source：姿态数据来源；
    // - 作用：告诉用户这份状态是谁给出的；
    // - 返回：来源说明文本。
    QString postureSourceText(const PostureSource source)
    {
        switch (source)
        {
        case PostureSource::Driver:
            return QStringLiteral("R0 驱动查询");
        case PostureSource::Win32:
            return QStringLiteral("R3 系统查询");
        case PostureSource::None:
        default:
            break;
        }
        return QStringLiteral("无可用通道");
    }

    // onOffText：
    // - 输入 enabled：开关状态；
    // - 作用：统一"开启/关闭"文案；
    // - 返回：状态文本。
    // 这里刻意不写成单行三元：两个 QStringLiteral 挤在一行会让 i18n 提取器
    // 把中间的 `") : QStringLiteral("` 误当成一条待翻译字面量。
    QString onOffText(const bool enabled)
    {
        if (enabled)
        {
            return QStringLiteral("开启");
        }
        return QStringLiteral("关闭");
    }

    // showOpaqueMessage：
    // - 作用：弹一个不透明的主题化提示框，避免继承父链的半透明背景；
    // - 返回：无。
    void showOpaqueMessage(
        QWidget* const parent,
        const QMessageBox::Icon icon,
        const QString& title,
        const QString& message)
    {
        QMessageBox dialog(parent);
        dialog.setObjectName(QStringLiteral("ksDisableDseMessageBox"));
        dialog.setStyleSheet(KswordTheme::OpaqueDialogStyle(dialog.objectName()));
        dialog.setIcon(icon);
        dialog.setWindowTitle(title);
        dialog.setText(message);
        dialog.setStandardButtons(QMessageBox::Ok);
        dialog.exec();
    }

    // askConfirmation：
    // - 作用：对改内核数据这类不可撤销的操作做一次显式确认；
    // - 返回：true 表示用户选择继续。
    bool askConfirmation(
        QWidget* const parent,
        const QString& title,
        const QString& message)
    {
        QMessageBox dialog(parent);
        dialog.setObjectName(QStringLiteral("ksDisableDseConfirmBox"));
        dialog.setStyleSheet(KswordTheme::OpaqueDialogStyle(dialog.objectName()));
        dialog.setIcon(QMessageBox::Warning);
        dialog.setWindowTitle(title);
        dialog.setText(message);
        dialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        dialog.setDefaultButton(QMessageBox::No);
        return dialog.exec() == QMessageBox::Yes;
    }
}

namespace ks::misc
{
    DisableDsePage::DisableDsePage(QWidget* parent, const std::optional<BlockReason> initialReason)
        : QWidget(parent)
    {
        initializeUi();
        initializeConnections();
        if (initialReason) applyStateDisplay(*initialReason);
        else updateStateDisplay();
        updateButtons();
        // 沿本页明确的父链固定主窗口，不枚举其它页面或顶层窗口。
        for (QWidget* host = parentWidget(); host != nullptr; host = host->parentWidget())
        {
            if (host->inherits("MainWindow"))
            {
                m_ksecPrimaryCloseHost = host;
                break;
            }
        }
        if (qApp != nullptr)
        {
            // 应用过滤器先于 MainWindow::closeEvent 执行，不能让停驱抢在 worker 前面。
            qApp->installEventFilter(this);
            // exit() 可绕过 Quit 事件；事件循环结束前同步归集，worker 从不等待 UI。
            connect(qApp, &QCoreApplication::aboutToQuit, this,
                [this]() { drainPendingKsecApply(true); }, Qt::DirectConnection);
        }
    }

    DisableDsePage::~DisableDsePage()
    {
        // 先撤销排队回调，再等待；避免归集或既有恢复期间的嵌套事件重入半析构页面。
        m_ksecCallbackLifetime->store(false, std::memory_order_release);
        if (qApp != nullptr)
        {
            qApp->removeEventFilter(this);
        }
        // join 不泵消息且 worker 不等待 UI，完成后才进入既有恢复路径。
        drainPendingKsecApply(true);
        // 用户可能加载完驱动就直接关了程序。g_CiOptions 停在被改过的状态会被
        // PatchGuard 巡检判定为内核数据被篡改，因此这里做最后一次同步写回。
        if (!m_hasSavedOriginal || !m_location.ok)
        {
            return;
        }

        kLogEvent closeEvent;

        // ——尝试通过与关闭时相同的路径恢复——

        if (m_usedKsecPath && m_gadgetLocation.ok)
        {
            // 使用执行时锁定的方案恢复，而非 UI 当前选项——
            // 防止用户仅浏览另一方案就意外切换恢复路径。
            const disable_dse::ExecuteResult ksecResult =
                (m_usedKsecStrategy == disable_dse::KsecDdStrategy::LsassInjection)
                    ? disable_dse::executeViaLsass(
                          m_gadgetLocation.kernelAddress,
                          m_location.kernelAddress,
                          m_savedOriginalValue)
                    : disable_dse::executeViaSilo(
                          m_gadgetLocation.kernelAddress,
                          m_location.kernelAddress,
                          m_savedOriginalValue);

            if (ksecResult.ok)
            {
                const QString message =
                    QStringLiteral("[DisableDSE] 页面关闭时已通过 KSecDD 把 g_CiOptions 写回原值 %1。")
                        .arg(formatHex32(m_savedOriginalValue));
                info << closeEvent << message.toStdString() << eol;
                return;
            }

            // KSecDD 恢复失败——回退到 R0 驱动路径。
            const QString fallbackMessage =
                QStringLiteral("[DisableDSE] KSecDD 恢复失败（%1），尝试 R0 驱动路径……")
                    .arg(ksecResult.failureText);
            warn << closeEvent << fallbackMessage.toStdString() << eol;
        }

        // R0 驱动路径恢复。
        const disable_dse::ReadbackResult readback =
            disable_dse::readCiOptions(m_location);
        if (!readback.ok || readback.value == m_savedOriginalValue)
        {
            return;
        }

        const disable_dse::ApplyResult result = disable_dse::writeCiOptions(
            m_location, readback.value, m_savedOriginalValue,
            m_posture.hvciEnabled);

        if (result.ok)
        {
            const QString message =
                QStringLiteral("[DisableDSE] 页面关闭时已把 g_CiOptions 写回原值 %1。")
                    .arg(formatHex32(m_savedOriginalValue));
            info << closeEvent << message.toStdString() << eol;
        }
        else
        {
            const QString message =
                QStringLiteral("[DisableDSE] 页面关闭时恢复 g_CiOptions 失败：%1 请立即手动恢复或重启系统。")
                    .arg(result.detailText);
            err << closeEvent << message.toStdString() << eol;
        }
    }

    void DisableDsePage::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        refreshPosture();
        // 按当前选中的方案刷新对应的状态显示。
        if (m_r0DriverButton != nullptr && m_r0DriverButton->isChecked())
        {
            updateR0StrategyDisplay();
        }
        else
        {
            refreshKsecStrategyAssessment();
        }
    }

    void DisableDsePage::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event == nullptr)
        {
            return;
        }
        if (event->type() == QEvent::LanguageChange)
        {
            // Translate each owned log message directly; never interpret it as fields.
            QStringList translatedLines;
            for (const QString& line : m_traceLines)
                translatedLines.append(ks::i18n::displayText(line));
            if (m_traceEdit) m_traceEdit->replaceRawText(translatedLines.join(QLatin1Char('\n')));
            updateStateDisplay();
            if (m_r0DriverButton != nullptr && m_r0DriverButton->isChecked())
                updateR0StrategyDisplay();
            else
                updateKsecStrategyDisplay();
        }
        if (event->type() == QEvent::ApplicationPaletteChange
            || event->type() == QEvent::PaletteChange)
        {
            applyBannerStyle();
            updateStateDisplay();
        }
    }

    // applyBannerStyle 作用：
    // - 输入：无，读取当前主题的语义色；
    // - 处理：下发风险横幅与待恢复提示的样式，构造期与主题切换走同一条路径；
    // - 返回：无，控件尚未创建时静默跳过。
    void DisableDsePage::applyBannerStyle()
    {
        if (m_warningLabel != nullptr)
        {
            m_warningLabel->setStyleSheet(
                QStringLiteral(
                    "QLabel{padding:16px;border:0;border-left:3px solid %1;border-radius:5px;"
                    "background:%2;color:%3;}")
                    .arg(KswordTheme::ErrorHex())
                    .arg(KswordTheme::ThemeColorName(KswordTheme::ErrorBackgroundColor()))
                    .arg(KswordTheme::TextPrimaryHex()));
        }
        if (m_pendingLabel != nullptr)
        {
            m_pendingLabel->setStyleSheet(
                QStringLiteral(
                    "QLabel{padding:12px;border:0;border-left:3px solid %1;border-radius:5px;"
                    "background:%2;color:%3;}")
                    .arg(KswordTheme::WarningHex())
                    .arg(KswordTheme::ThemeColorName(KswordTheme::WarningBackgroundColor()))
                    .arg(KswordTheme::TextPrimaryHex()));
        }
    }

    void DisableDsePage::initializeUi()
    {
        auto& language = ks::i18n::LanguageManager::instance();

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);
        auto* workspace = new ks::ui::ParameterWorkspace(this, 520);
        rootLayout->addWidget(workspace);

        // ===================== 风险横幅 =====================
        m_warningLabel = new QLabel(this);
        m_warningLabel->setWordWrap(true);
        m_warningLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        language.bindText(
            m_warningLabel,
            QStringLiteral("misc.disable_dse.warning"),
            QStringLiteral("高风险功能：本页直接修改内核里 CI.dll!g_CiOptions 的 4 个字节，关闭后系统将接受未签名驱动。该变量在 PatchGuard 的巡检范围内，长时间保持关闭会触发 CRITICAL_STRUCTURE_CORRUPTION 蓝屏。请在加载完目标驱动后立刻点“恢复”，不要让系统长期停在关闭状态。"));
        workspace->parameters()->addWidget(m_warningLabel);

        // ===================== DSE 关闭方案（三路径选择） =====================
        // R0 驱动直写 / KSecDD LSASS 注入 / KSecDD Server Silo。
        auto* strategyGroup = new QGroupBox(this);
        language.bindText(
            strategyGroup,
            QStringLiteral("misc.disable_dse.strategy.title"),
            QStringLiteral("DSE 关闭方案"));
        auto* strategyLayout = new QVBoxLayout(strategyGroup);
        strategyLayout->setSpacing(8);

        // 三个互斥方案的分段按钮。
        auto* strategyButtonLayout = new QHBoxLayout();
        strategyButtonLayout->setSpacing(0);
        m_r0DriverButton = new QRadioButton(
            QStringLiteral("KswordARK · R0"), strategyGroup);
        m_kexecDdButton = new QRadioButton(
            QStringLiteral("KSecDD · LSASS"), strategyGroup);
        m_kexecDdPlusButton = new QRadioButton(
            QStringLiteral("KSecDD · Silo"), strategyGroup);
        m_r0DriverButton->setChecked(true);
        language.bindToolTip(
            m_r0DriverButton,
            QStringLiteral("misc.disable_dse.strategy.r0.tooltip"),
            QStringLiteral("通过已加载的 KswordARK R0 驱动直接写入 g_CiOptions。需要 R0 驱动在线。"));
        language.bindToolTip(
            m_kexecDdButton,
            QStringLiteral("misc.disable_dse.ksec.original.tooltip"),
            QStringLiteral("复用 LSASS 已建立的 KSecDD 连接；要求 LSA Protection/PPL 关闭。"));
        language.bindToolTip(
            m_kexecDdPlusButton,
            QStringLiteral("misc.disable_dse.ksec.plus.tooltip"),
            QStringLiteral("在 Server Silo 中建立独立 KSecDD 连接；不受 LSA Protection 前置阻断，但有每次启动最多 4 次的硬限制。"));
        strategyButtonLayout->addWidget(m_r0DriverButton);
        strategyButtonLayout->addWidget(m_kexecDdButton);
        strategyButtonLayout->addWidget(m_kexecDdPlusButton);
        auto* strategySurface = ks::ui::InstallSegmentedButtons(
            strategyButtonLayout,
            QList<QAbstractButton*>{
                m_r0DriverButton, m_kexecDdButton, m_kexecDdPlusButton });
        strategyLayout->addWidget(strategySurface);

        // 方案摘要与详情标签（三条路径共用，内容随选中路径切换）。
        m_ksecStrategySummaryLabel = new QLabel(strategyGroup);
        m_ksecStrategySummaryLabel->setWordWrap(true);
        m_ksecStrategySummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        strategyLayout->addWidget(m_ksecStrategySummaryLabel);

        m_ksecStrategyDetailLabel = new QLabel(strategyGroup);
        m_ksecStrategyDetailLabel->setWordWrap(true);
        m_ksecStrategyDetailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        strategyLayout->addWidget(m_ksecStrategyDetailLabel);

        // g_CiOptions 定位按钮：R0 和 KSecDD 路径都需要目标地址，放在公共区域。
        auto* locateLayout = new QHBoxLayout();
        locateLayout->setSpacing(8);
        m_locateButton = new QPushButton(strategyGroup);
        ks::ui::ApplyFlatButtonTheme(m_locateButton, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_locateButton,
            QStringLiteral("misc.disable_dse.action.locate"),
            QStringLiteral("定位并校验"));
        language.bindToolTip(
            m_locateButton,
            QStringLiteral("misc.disable_dse.action.locate.tooltip"),
            QStringLiteral("通过 R0 驱动反汇编定位 g_CiOptions 并读回校验。"));
        locateLayout->addWidget(m_locateButton);
        locateLayout->addStretch(1);
        ks::ui::NormalizeToolbarRow(locateLayout);
        strategyLayout->addLayout(locateLayout);

        // ---------- R0 路径专属容器 ----------
        m_r0Container = new QWidget(strategyGroup);
        auto* r0Layout = new QVBoxLayout(m_r0Container);
        r0Layout->setContentsMargins(0, 0, 0, 0);
        r0Layout->setSpacing(8);

        // R0 执行动作行：关闭签名强制 / 恢复原值。
        auto* r0ExecLayout = new QHBoxLayout();
        r0ExecLayout->setSpacing(8);

        m_disableButton = new QPushButton(m_r0Container);
        ks::ui::ApplyFlatButtonTheme(m_disableButton, ks::ui::FlatButtonTone::Danger,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_disableButton,
            QStringLiteral("misc.disable_dse.action.disable"),
            QStringLiteral("关闭签名强制"));
        language.bindToolTip(
            m_disableButton,
            QStringLiteral("misc.disable_dse.action.disable.tooltip"),
            QStringLiteral("通过 R0 驱动将 g_CiOptions 写为 0，关闭驱动签名强制。"));
        r0ExecLayout->addWidget(m_disableButton);

        m_restoreButton = new QPushButton(m_r0Container);
        ks::ui::ApplyFlatButtonTheme(m_restoreButton, ks::ui::FlatButtonTone::Accent,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_restoreButton,
            QStringLiteral("misc.disable_dse.action.restore"),
            QStringLiteral("恢复原值"));
        language.bindToolTip(
            m_restoreButton,
            QStringLiteral("misc.disable_dse.action.restore.tooltip"),
            QStringLiteral("通过 R0 驱动将 g_CiOptions 写回原值。"));
        r0ExecLayout->addWidget(m_restoreButton);
        r0ExecLayout->addStretch(1);
        ks::ui::NormalizeToolbarRow(r0ExecLayout);
        r0Layout->addLayout(r0ExecLayout);
        strategyLayout->addWidget(m_r0Container);

        // ---------- KSecDD 路径专属容器 ----------
        m_ksecContainer = new QWidget(strategyGroup);
        auto* ksecLayout = new QVBoxLayout(m_ksecContainer);
        ksecLayout->setContentsMargins(0, 0, 0, 0);
        ksecLayout->setSpacing(8);

        // KSecDD 辅助动作行：检查前置条件。
        auto* strategyActionLayout = new QHBoxLayout();
        strategyActionLayout->setSpacing(8);
        m_ksecRefreshButton = new QPushButton(m_ksecContainer);
        ks::ui::ApplyFlatButtonTheme(m_ksecRefreshButton, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_ksecRefreshButton,
            QStringLiteral("misc.disable_dse.ksec.refresh"),
            QStringLiteral("检查前置条件"));
        strategyActionLayout->addWidget(m_ksecRefreshButton);
        strategyActionLayout->addStretch(1);
        ks::ui::NormalizeToolbarRow(strategyActionLayout);
        ksecLayout->addLayout(strategyActionLayout);

        // 写入桩定位结果。
        m_ksecGadgetLabel = new QLabel(m_ksecContainer);
        m_ksecGadgetLabel->setWordWrap(true);
        m_ksecGadgetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        ksecLayout->addWidget(m_ksecGadgetLabel);

        // KSecDD 执行动作行：定位桩 / 关闭 / 恢复。
        auto* ksecExecLayout = new QHBoxLayout();
        ksecExecLayout->setSpacing(8);
        m_ksecLocateGadgetButton = new QPushButton(m_ksecContainer);
        ks::ui::ApplyFlatButtonTheme(m_ksecLocateGadgetButton,
            ks::ui::FlatButtonTone::Neutral, ks::ui::FlatButtonAppearance::Solid);
        language.bindText(m_ksecLocateGadgetButton,
            QStringLiteral("misc.disable_dse.ksec.locate_gadget"),
            QStringLiteral("定位写入桩"));
        language.bindToolTip(m_ksecLocateGadgetButton,
            QStringLiteral("misc.disable_dse.ksec.locate_gadget.tooltip"),
            QStringLiteral("在 ntoskrnl 中定位 mov [rcx], edx; ret 字节序列，KSecDD 通过它向内核地址写值。"));
        ksecExecLayout->addWidget(m_ksecLocateGadgetButton);

        m_ksecDisableButton = new QPushButton(m_ksecContainer);
        ks::ui::ApplyFlatButtonTheme(m_ksecDisableButton,
            ks::ui::FlatButtonTone::Danger, ks::ui::FlatButtonAppearance::Solid);
        language.bindText(m_ksecDisableButton,
            QStringLiteral("misc.disable_dse.ksec.disable"),
            QStringLiteral("KSecDD 关闭"));
        language.bindToolTip(m_ksecDisableButton,
            QStringLiteral("misc.disable_dse.ksec.disable.tooltip"),
            QStringLiteral("通过 KSecDD IOCTL 将 g_CiOptions 写为 0，关闭驱动签名强制。不需要 KswordARK R0 驱动。"));
        ksecExecLayout->addWidget(m_ksecDisableButton);

        m_ksecRestoreButton = new QPushButton(m_ksecContainer);
        ks::ui::ApplyFlatButtonTheme(m_ksecRestoreButton,
            ks::ui::FlatButtonTone::Accent, ks::ui::FlatButtonAppearance::Solid);
        language.bindText(m_ksecRestoreButton,
            QStringLiteral("misc.disable_dse.ksec.restore"),
            QStringLiteral("KSecDD 恢复"));
        language.bindToolTip(m_ksecRestoreButton,
            QStringLiteral("misc.disable_dse.ksec.restore.tooltip"),
            QStringLiteral("通过 KSecDD IOCTL 将 g_CiOptions 写回原值。"));
        ksecExecLayout->addWidget(m_ksecRestoreButton);
        ksecExecLayout->addStretch(1);
        ks::ui::NormalizeToolbarRow(ksecExecLayout);
        ksecLayout->addLayout(ksecExecLayout);

        // 初始状态：R0 选中时隐藏 KSecDD 容器。
        m_ksecContainer->setVisible(false);
        strategyLayout->addWidget(m_ksecContainer);

        ks::ui::StyleParameterSection(strategyGroup);
        workspace->parameters()->addWidget(strategyGroup);

        // ===================== 当前状态 =====================
        auto* postureGroup = new QGroupBox(this);
        language.bindText(
            postureGroup,
            QStringLiteral("misc.disable_dse.posture.title"),
            QStringLiteral("代码完整性状态"));
        auto* postureLayout = new QVBoxLayout(postureGroup);
        postureLayout->setSpacing(6);

        m_postureLabel = new QLabel(postureGroup);
        m_postureLabel->setWordWrap(true);
        m_postureLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        postureLayout->addWidget(m_postureLabel);

        m_optionsLabel = new QLabel(postureGroup);
        m_optionsLabel->setWordWrap(true);
        m_optionsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        postureLayout->addWidget(m_optionsLabel);

        m_blockLabel = new QLabel(postureGroup);
        m_blockLabel->setWordWrap(true);
        m_blockLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        postureLayout->addWidget(m_blockLabel);

        ks::ui::StyleParameterSection(postureGroup);
        workspace->parameters()->addWidget(postureGroup);

        // ===================== 目标定位 =====================
        auto* locationGroup = new QGroupBox(this);
        language.bindText(
            locationGroup,
            QStringLiteral("misc.disable_dse.location.title"),
            QStringLiteral("g_CiOptions 定位"));
        auto* locationLayout = new QVBoxLayout(locationGroup);
        locationLayout->setSpacing(6);

        m_locationLabel = new QLabel(locationGroup);
        m_locationLabel->setWordWrap(true);
        m_locationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        locationLayout->addWidget(m_locationLabel);

        ks::ui::StyleParameterSection(locationGroup);
        workspace->parameters()->addWidget(locationGroup);

        auto* traceTitle = new QLabel(workspace);
        language.bindText(
            traceTitle,
            QStringLiteral("misc.disable_dse.trace.title"),
            QStringLiteral("定位轨迹与事务日志"));
        traceTitle->setProperty("ksword_detail_section_title", true);
        workspace->results()->addWidget(traceTitle);

        // 定位与事务保留实际时序消息；追加时翻译，语言切换由页面按消息重绘。
        m_traceEdit = new CodeEditorWidget(workspace);
        m_traceEdit->setReadOnly(true);
        m_traceEdit->setMinimumHeight(96);
        language.bindToolTip(
            m_traceEdit,
            QStringLiteral("misc.disable_dse.trace.tooltip"),
            QStringLiteral("定位轨迹与事务日志。定位不使用任何硬编码偏移：从导出的 CiInitialize 反汇编找到 CipInitialize，再在其中找写 g_CiOptions 的指令。"));
        workspace->results()->addWidget(m_traceEdit, 1);

        // ===================== 操作 =====================
        m_pendingLabel = new QLabel(this);
        m_pendingLabel->setWordWrap(true);
        m_pendingLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_pendingLabel->setVisible(false);
        workspace->parameters()->addWidget(m_pendingLabel);

        // 底部操作栏：只保留与路径无关的刷新状态按钮。
        m_refreshButton = new QPushButton(workspace);
        ks::ui::ApplyFlatButtonTheme(m_refreshButton, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_refreshButton,
            QStringLiteral("misc.disable_dse.action.refresh"),
            QStringLiteral("刷新状态"));
        workspace->actions()->addWidget(m_refreshButton);

        workspace->actions()->addStretch(1);
        ks::ui::NormalizeToolbarRow(workspace->actions());

        m_resultLabel = new QLabel(workspace);
        m_resultLabel->setWordWrap(true);
        m_resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->addWidget(m_resultLabel);

        workspace->finish();

        applyBannerStyle();
    }

    void DisableDsePage::initializeConnections()
    {
        // R0 驱动直写方案选中时：显示 R0 容器，隐藏 KSecDD 容器，更新状态。
        connect(m_r0DriverButton, &QRadioButton::toggled, this, [this](const bool checked) {
            if (!checked)
            {
                return;
            }
            if (m_r0Container != nullptr)
                m_r0Container->setVisible(true);
            if (m_ksecContainer != nullptr)
                m_ksecContainer->setVisible(false);
            updateR0StrategyDisplay();
            updateButtons();
        });
        // KSecDD LSASS 方案选中时：隐藏 R0 容器，显示 KSecDD 容器，执行前置核对。
        connect(m_kexecDdButton, &QRadioButton::toggled, this, [this](const bool checked) {
            if (!checked)
            {
                return;
            }
            if (m_r0Container != nullptr)
                m_r0Container->setVisible(false);
            if (m_ksecContainer != nullptr)
                m_ksecContainer->setVisible(true);
            m_ksecStrategy = disable_dse::KsecDdStrategy::LsassInjection;
            refreshKsecStrategyAssessment();
        });
        // KSecDD Server Silo 方案选中时：同 LSASS 方案的容器切换。
        connect(m_kexecDdPlusButton, &QRadioButton::toggled, this, [this](const bool checked) {
            if (!checked)
            {
                return;
            }
            if (m_r0Container != nullptr)
                m_r0Container->setVisible(false);
            if (m_ksecContainer != nullptr)
                m_ksecContainer->setVisible(true);
            m_ksecStrategy = disable_dse::KsecDdStrategy::ServerSilo;
            refreshKsecStrategyAssessment();
        });
        connect(m_ksecRefreshButton, &QPushButton::clicked, this, [this]() {
            refreshKsecStrategyAssessment();
        });
        connect(m_ksecLocateGadgetButton, &QPushButton::clicked, this, [this]() {
            runKsecLocateGadget();
        });
        connect(m_ksecDisableButton, &QPushButton::clicked, this, [this]() {
            // HVCI 开启时沿用二次确认，明确该路径的安全失败尚未验证。
            if (m_posture.hvciEnabled)
            {
                if (!askConfirmation(
                        this,
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.hvci_confirm.title"),
                            QStringLiteral("HVCI 写入限制")),
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.hvci_confirm.message"),
                            QStringLiteral("当前系统已开启内存完整性（HVCI），内核写入可能被保护机制阻止。\n\n不能保证该路径会安全失败或避免蓝屏；尝试可能失败或导致系统崩溃。\n\n确定要尝试吗？"))))
                {
                    return;
                }
            }
            const QString message = ks::i18n::text(
                QStringLiteral("misc.disable_dse.ksec.confirm.disable"),
                QStringLiteral("即将通过 KSecDD 把 g_CiOptions 改成 0。\n\n这条路径不经过 KswordARK R0 驱动，直接利用 ksecdd.sys 在内核模式写入。\n\nPatchGuard 风险与 R0 写入相同：请在加载完驱动后立即恢复。\n\n确定继续吗？"));
            if (!askConfirmation(
                    this,
                    ks::i18n::text(
                        QStringLiteral("misc.disable_dse.ksec.confirm.title"),
                        QStringLiteral("KSecDD 关闭签名强制")),
                    message))
            {
                return;
            }
            runKsecApply(disable_dse::kDisabledValue, false);
        });
        connect(m_ksecRestoreButton, &QPushButton::clicked, this, [this]() {
            if (m_posture.hvciEnabled)
            {
                if (!askConfirmation(
                        this,
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.hvci_confirm.title"),
                            QStringLiteral("HVCI 写入限制")),
                        ks::i18n::text(
                            QStringLiteral("misc.disable_dse.hvci_confirm.message"),
                            QStringLiteral("当前系统已开启内存完整性（HVCI），内核写入可能被保护机制阻止。\n\n不能保证该路径会安全失败或避免蓝屏；尝试可能失败或导致系统崩溃。\n\n确定要尝试吗？"))))
                {
                    return;
                }
            }
            runKsecApply(m_savedOriginalValue, true);
        });
        connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
            refreshPosture();
        });
        connect(m_locateButton, &QPushButton::clicked, this, [this]() {
            runLocate();
        });
        connect(m_disableButton, &QPushButton::clicked, this, [this]() {
            // HVCI 环境下尝试 RXPF 写入，VTL1 SLAT 会安全拦截。
            const QString message = ks::i18n::text(
                QStringLiteral("misc.disable_dse.confirm.disable"),
                QStringLiteral("即将把 g_CiOptions 改成 0，系统将不再校验驱动签名。\n\n该变量在 PatchGuard 的巡检范围内，保持关闭状态会导致蓝屏，请在加载完驱动后立即恢复。\n\n确定继续吗？"));
            if (!askConfirmation(
                    this,
                    ks::i18n::text(
                        QStringLiteral("misc.disable_dse.confirm.title"),
                        QStringLiteral("关闭驱动签名强制")),
                    message))
            {
                return;
            }
            runApply(disable_dse::kDisabledValue, false);
        });
        connect(m_restoreButton, &QPushButton::clicked, this, [this]() {
            // HVCI 环境下尝试 RXPF 写入，VTL1 SLAT 会安全拦截。
            runApply(m_savedOriginalValue, true);
        });
    }

    void DisableDsePage::refreshPosture()
    {
        m_posture = disable_dse::queryPosture();
        updateStateDisplay();
        updateButtons();

        if (!m_posture.queried)
        {
            setResultText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.result.posture_failed"),
                    QStringLiteral("状态查询失败："))
                    + m_posture.failureText,
                true);
        }
    }

    void DisableDsePage::refreshKsecStrategyAssessment()
    {
        m_ksecAssessment = disable_dse::assessKsecDdStrategy(m_ksecStrategy);
        updateKsecStrategyDisplay();
    }

    void DisableDsePage::updateKsecStrategyDisplay()
    {
        if (m_ksecStrategySummaryLabel == nullptr || m_ksecStrategyDetailLabel == nullptr)
        {
            return;
        }

        if (m_ksecAssessment.summaryText.isEmpty())
        {
            m_ksecStrategySummaryLabel->setText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.ksec.unchecked"),
                    QStringLiteral("尚未检查当前方案的前置条件。")));
            m_ksecStrategyDetailLabel->clear();
            return;
        }

        m_ksecStrategySummaryLabel->setText(
            ks::i18n::displayText(m_ksecAssessment.summaryText));
        m_ksecStrategySummaryLabel->setStyleSheet(
            QStringLiteral("color:%1;font-weight:650;")
                .arg(m_ksecAssessment.prerequisitesMet
                    ? KswordTheme::SuccessHex() : KswordTheme::WarningHex()));

        QStringList translatedDetails;
        for (const QString& detail : m_ksecAssessment.detailLines)
        {
            translatedDetails.append(ks::i18n::displayText(detail));
        }
        m_ksecStrategyDetailLabel->setText(translatedDetails.join(QLatin1Char('\n')));
        m_ksecStrategyDetailLabel->setStyleSheet(
            QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    }

    void DisableDsePage::updateR0StrategyDisplay()
    {
        if (m_ksecStrategySummaryLabel == nullptr || m_ksecStrategyDetailLabel == nullptr)
        {
            return;
        }

        // R0 路径的准入取决于驱动是否在线，由 m_blockReason 反映。
        const bool driverOnline =
            m_blockReason != disable_dse::BlockReason::DriverUnavailable;
        if (driverOnline)
        {
            m_ksecStrategySummaryLabel->setText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.strategy.r0.ready"),
                    QStringLiteral("KswordARK R0 驱动在线，可直接写入内核。")));
            m_ksecStrategySummaryLabel->setStyleSheet(
                QStringLiteral("color:%1;font-weight:650;")
                    .arg(KswordTheme::SuccessHex()));
        }
        else
        {
            m_ksecStrategySummaryLabel->setText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.strategy.r0.unavailable"),
                    QStringLiteral("KswordARK R0 驱动不在线。请先在驱动页面加载驱动。")));
            m_ksecStrategySummaryLabel->setStyleSheet(
                QStringLiteral("color:%1;font-weight:650;")
                    .arg(KswordTheme::WarningHex()));
        }

        // 对 R0 路径简要说明：直接通过 R0 驱动读写内核地址，不经 KSecDD。
        m_ksecStrategyDetailLabel->setText(
            ks::i18n::text(
                QStringLiteral("misc.disable_dse.strategy.r0.detail"),
                QStringLiteral("通过 KswordARK 自有内核驱动的事务通道读写 g_CiOptions。\n需要 R0 驱动在线且定位校验通过。\nHVCI 环境下使用 RXPF 框架尝试写入，但 VTL1 SLAT 保护会阻止修改。")));
        m_ksecStrategyDetailLabel->setStyleSheet(
            QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    }

    void DisableDsePage::runLocate()
    {
        if (m_busy)
        {
            return;
        }
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("dse.trace.locating"),
            QStringLiteral("开始定位 g_CiOptions……")));

        const QPointer<DisableDsePage> guardThis(this);
        const disable_dse::CodeIntegrityPosture posture = m_posture;

        std::thread([guardThis, posture]() {
            LocateOutcome outcome;
            outcome.posture = posture;
            outcome.location = disable_dse::locateCiOptions();
            if (outcome.location.ok)
            {
                outcome.readback = disable_dse::readCiOptions(outcome.location);
                // 写入前的护栏：g_CiOptions 用的是 CI.dll 内部编码，与系统自报的
                // CODEINTEGRITY_OPTION_* 不是一套，两个数值本来就不相等，
                // 因此只比对"强制签名此刻是否生效"这一个语义。地址若定位错了，
                // 落在无关内核数据上几乎不可能恰好满足这个关系。
                outcome.valueMatched = outcome.readback.ok
                    && disable_dse::ciOptionsAgreesWithPosture(
                        outcome.readback.value, outcome.posture);
            }

            if (guardThis == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(qApp, [guardThis, outcome]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->applyLocateOutcome(outcome);
            });
        }).detach();
    }

    void DisableDsePage::runApply(const std::uint32_t desiredValue, const bool isRestore)
    {
        if (m_busy || !m_location.ok)
        {
            return;
        }
        setBusy(true);

        const QPointer<DisableDsePage> guardThis(this);
        const disable_dse::TargetLocation location = m_location;
        // hvciEnabled：捕获当前 HVCI 状态，线程内决定走 RXPF 还是常规事务。
        const bool hvciEnabled = m_posture.hvciEnabled;

        std::thread([guardThis, location, desiredValue, isRestore, hvciEnabled]() {
            ApplyResult result;
            // 写之前再读一次，把 expected-before 对齐到此刻的真实值：
            // 期间可能有别的工具动过 g_CiOptions。
            const disable_dse::ReadbackResult readback =
                disable_dse::readCiOptions(location);
            if (!readback.ok)
            {
                result.detailText = readback.failureText;
            }
            else
            {
                result = disable_dse::writeCiOptions(
                    location, readback.value, desiredValue, hvciEnabled);
            }

            if (guardThis == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(qApp, [guardThis, result, isRestore]() {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->applyApplyOutcome(result, isRestore);
            });
        }).detach();
    }

    void DisableDsePage::applyLocateOutcome(const LocateOutcome& outcome)
    {
        setBusy(false);
        m_location = outcome.location;
        m_valueMatched = outcome.valueMatched;

        for (const QString& line : outcome.location.traceLines)
        {
            appendTrace(line);
        }

        if (!outcome.location.ok)
        {
            m_hasCurrentValue = false;
            appendTrace(ks::i18n::text(
                QStringLiteral("dse.trace.locate_failed"),
                QStringLiteral("定位失败：%1"))
                .arg(outcome.location.failureText));
            updateStateDisplay();
            updateButtons();
            setResultText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.result.locate_failed"),
                    QStringLiteral("定位失败："))
                    + outcome.location.failureText,
                true);
            return;
        }

        if (!outcome.readback.ok)
        {
            m_hasCurrentValue = false;
            appendTrace(ks::i18n::text(
                QStringLiteral("dse.trace.readback_failed"),
                QStringLiteral("读回失败：%1"))
                .arg(outcome.readback.failureText));
            updateStateDisplay();
            updateButtons();
            setResultText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.result.read_failed"),
                    QStringLiteral("读回失败："))
                    + outcome.readback.failureText,
                true);
            return;
        }

        m_currentValue = outcome.readback.value;
        m_hasCurrentValue = true;
        appendTrace(ks::i18n::text(
            QStringLiteral("dse.trace.readback_value"),
            QStringLiteral("读回 g_CiOptions = %1（%2）"))
            .arg(formatHex32(outcome.readback.value))
            .arg(disable_dse::describeCiOptions(outcome.readback.value)));

        if (!outcome.valueMatched)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("dse.trace.mismatch"),
                QStringLiteral("校验不通过：系统自报驱动签名强制为“%1”，而读回值 %2 的强制位与之矛盾。已拒绝后续写入。"))
                .arg(onOffText(outcome.posture.ciEnabled))
                .arg(formatHex32(outcome.readback.value)));
            updateStateDisplay();
            updateButtons();
            setResultText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.result.mismatch"),
                    QStringLiteral("读回值的强制签名位与系统自报状态矛盾，地址存疑，已禁止写入。")),
                true);
            return;
        }

        appendTrace(ks::i18n::text(
            QStringLiteral("dse.trace.verify_passed"),
            QStringLiteral("校验通过：读回值的强制签名位与系统自报状态一致。")));
        updateStateDisplay();
        updateButtons();
        setResultText(
            ks::i18n::text(
                QStringLiteral("misc.disable_dse.result.locate_ok"),
                QStringLiteral("定位并校验通过，可以执行操作。")),
            false);
    }

    void DisableDsePage::applyApplyOutcome(const ApplyResult& result, const bool isRestore)
    {
        setBusy(false);

        for (const QString& line : result.traceLines)
        {
            appendTrace(line);
        }

        if (!result.ok)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("dse.trace.apply_failed"),
                QStringLiteral("操作失败：%1"))
                .arg(result.detailText));
            updateStateDisplay();
            updateButtons();
            setResultText(
                ks::i18n::text(
                    QStringLiteral("misc.disable_dse.result.apply_failed"),
                    QStringLiteral("操作失败："))
                    + result.detailText,
                true);
            return;
        }

        m_currentValue = result.writtenValue;
        m_hasCurrentValue = true;

        if (isRestore)
        {
            // 恢复成功后清掉待恢复记账，页面析构时就不会再写一次。
            m_hasSavedOriginal = false;
            appendTrace(ks::i18n::text(
                QStringLiteral("dse.trace.restored"),
                QStringLiteral("已恢复 g_CiOptions = %1"))
                .arg(formatHex32(result.writtenValue)));
        }
        else
        {
            // 只在第一次关闭时记录原值：连续两次关闭不能把原值覆盖成 0。
            if (!m_hasSavedOriginal)
            {
                m_savedOriginalValue = result.previousValue;
                m_hasSavedOriginal = true;
            }
            appendTrace(ks::i18n::text(
                QStringLiteral("dse.trace.disabled"),
                QStringLiteral("已关闭签名强制，原值 %1 已记录，请尽快恢复。"))
                .arg(formatHex32(m_savedOriginalValue)));
        }

        // 写入会改变 CodeIntegrityOptions，重新查一次姿态让展示保持同步。
        m_posture = disable_dse::queryPosture();
        updateStateDisplay();
        updateButtons();

        setResultText(
            isRestore
                ? ks::i18n::text(
                      QStringLiteral("misc.disable_dse.result.restore_ok"),
                      QStringLiteral("已恢复原值，驱动签名强制回到原始状态。"))
                : ks::i18n::text(
                      QStringLiteral("misc.disable_dse.result.disable_ok"),
                      QStringLiteral("已关闭驱动签名强制。请立即加载目标驱动，然后马上点“恢复原值”。")),
            false);
    }

    void DisableDsePage::appendTrace(const QString& line)
    {
        if (m_traceEdit == nullptr)
        {
            return;
        }
        m_traceLines.append(line);
        m_traceEdit->appendRawText(ks::i18n::displayText(line));
    }

    void DisableDsePage::setResultText(const QString& text, const bool isError)
    {
        if (m_resultLabel == nullptr)
        {
            return;
        }
        m_resultLabel->setText(text);
        m_resultLabel->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg(isError ? KswordTheme::ErrorHex() : KswordTheme::SuccessHex()));
    }

    bool DisableDsePage::eventFilter(QObject* watched, QEvent* event)
    {
        // 普通空闲页面不承担全局事件处理成本，也不改变其它窗口行为。
        if (!m_pendingKsecApply || event == nullptr)
        {
            return QWidget::eventFilter(watched, event);
        }
        const bool applicationQuit = watched == qApp && event->type() == QEvent::Quit;
        // 同时覆盖本页、当前浮动宿主和构造时固定的主窗口，停驱前直接拒绝关闭。
        const bool closingHost = (watched == this || watched == window()
            || watched == m_ksecPrimaryCloseHost.data())
            && (event->type() == QEvent::Close || event->type() == QEvent::DeferredDelete);
        if (applicationQuit || closingHost)
        {
            event->ignore();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void DisableDsePage::setBusy(const bool busy)
    {
        m_busy = busy;
        updateButtons();
    }

    void DisableDsePage::updateButtons()
    {
        // 保留 HVCI 环境下的既有按钮可用性，不据此承诺写入能安全失败。
        const bool allowed = !m_busy
            && (m_blockReason == disable_dse::BlockReason::None
                || m_blockReason == disable_dse::BlockReason::HvciEnabled);

        if (m_refreshButton != nullptr)
        {
            m_refreshButton->setEnabled(!m_busy);
        }
        if (m_locateButton != nullptr)
        {
            // 定位本身只读，只要不忙就允许；失败时的原因比灰按钮更有用。
            m_locateButton->setEnabled(!m_busy);
        }
        if (m_disableButton != nullptr)
        {
            m_disableButton->setEnabled(
                allowed && m_valueMatched && m_hasCurrentValue
                && m_currentValue != disable_dse::kDisabledValue);
        }
        if (m_restoreButton != nullptr)
        {
            m_restoreButton->setEnabled(
                allowed && m_valueMatched && m_hasSavedOriginal
                && m_hasCurrentValue && m_currentValue != m_savedOriginalValue);
        }
        // KSecDD 执行按钮：不需要 R0 驱动在线，但仍受 HVCI / 版本阻断。
        {
            const bool ksecPathOk = !m_busy
                && m_blockReason != disable_dse::BlockReason::UnsupportedBuild;
            if (m_ksecLocateGadgetButton != nullptr)
                m_ksecLocateGadgetButton->setEnabled(!m_busy);
            if (m_ksecDisableButton != nullptr)
                m_ksecDisableButton->setEnabled(
                    ksecPathOk && m_gadgetLocation.ok && m_location.ok
                    && m_ksecAssessment.prerequisitesMet
                    && (!m_hasCurrentValue || m_currentValue != disable_dse::kDisabledValue));
            if (m_ksecRestoreButton != nullptr)
                m_ksecRestoreButton->setEnabled(
                    ksecPathOk && m_gadgetLocation.ok && m_location.ok
                    && m_ksecAssessment.prerequisitesMet && m_hasSavedOriginal
                    && (!m_hasCurrentValue || m_currentValue != m_savedOriginalValue));
        }
        if (m_ksecRefreshButton != nullptr)
        {
            m_ksecRefreshButton->setEnabled(!m_busy);
        }
        // 三个方案切换按钮：只要不忙就允许切换。
        if (m_r0DriverButton != nullptr)
        {
            m_r0DriverButton->setEnabled(!m_busy);
        }
        if (m_kexecDdButton != nullptr)
        {
            m_kexecDdButton->setEnabled(!m_busy);
        }
        if (m_kexecDdPlusButton != nullptr)
        {
            m_kexecDdPlusButton->setEnabled(!m_busy);
        }
    }

    void DisableDsePage::updateStateDisplay()
    {
        applyStateDisplay(disable_dse::evaluateBlockReason(m_posture, m_location));
    }

    void DisableDsePage::applyStateDisplay(const BlockReason reason)
    {
        m_blockReason = reason;
        if (m_blockReason == disable_dse::BlockReason::None
            && m_location.ok && !m_valueMatched && m_hasCurrentValue)
        {
            m_blockReason = disable_dse::BlockReason::ValueMismatch;
        }

        if (m_postureLabel != nullptr)
        {
            if (!m_posture.queried)
            {
                m_postureLabel->setText(
                    ks::i18n::text(
                        QStringLiteral("misc.disable_dse.posture.unknown"),
                        QStringLiteral("尚未取得代码完整性状态。")));
            }
            else
            {
                QStringList parts;
                parts << QStringLiteral("驱动签名强制：%1").arg(onOffText(m_posture.ciEnabled));
                parts << QStringLiteral("测试签名：%1").arg(onOffText(m_posture.testSigningEnabled));
                parts << QStringLiteral("内存完整性(HVCI)：%1").arg(onOffText(m_posture.hvciEnabled));
                if (m_posture.source == PostureSource::Driver)
                {
                    parts << QStringLiteral("安全启动：%1").arg(onOffText(m_posture.secureBootEnabled));
                }
                m_postureLabel->setText(
                    QStringLiteral("%1\n数据来源：%2　系统内部版本：%3")
                        .arg(parts.join(QStringLiteral("　")))
                        .arg(postureSourceText(m_posture.source))
                        .arg(m_posture.buildNumber));
            }
        }

        if (m_optionsLabel != nullptr)
        {
            if (!m_posture.queried)
            {
                m_optionsLabel->clear();
            }
            else
            {
                QString text = QStringLiteral("系统自报 CodeIntegrityOptions = %1（%2）")
                                   .arg(formatHex32(m_posture.options))
                                   .arg(disable_dse::describeOptions(m_posture.options));
                if (m_hasCurrentValue)
                {
                    // 两个值用的不是同一套位编码，不该相互对照数值，因此分行标注各自含义。
                    text += QStringLiteral("\n内核读回 g_CiOptions = %1（%2）")
                                .arg(formatHex32(m_currentValue))
                                .arg(disable_dse::describeCiOptions(m_currentValue));
                }
                m_optionsLabel->setText(text);
            }
            m_optionsLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
        }

        if (m_blockLabel != nullptr)
        {
            const QString reasonText = disable_dse::blockReasonText(m_blockReason);
            m_blockLabel->setText(reasonText);
            m_blockLabel->setVisible(!reasonText.isEmpty());
            // HvciEnabled 沿用提示信息的次要文字色；其它原因用警告色。
            m_blockLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(
                    m_blockReason == disable_dse::BlockReason::HvciEnabled
                        ? KswordTheme::TextSecondaryHex()
                        : KswordTheme::WarningHex()));
        }

        if (m_locationLabel != nullptr)
        {
            if (!m_location.ok)
            {
                m_locationLabel->setText(
                    ks::i18n::text(
                        QStringLiteral("misc.disable_dse.location.none"),
                        QStringLiteral("尚未定位。点“定位并校验”开始。")));
            }
            else
            {
                m_locationLabel->setText(
                    QStringLiteral("%1 基址 %2　RVA 0x%3（节 %4）\ng_CiOptions 内核地址 %5")
                        .arg(m_location.moduleName)
                        .arg(formatHex64(m_location.moduleBase))
                        .arg(m_location.rva, 0, 16)
                        .arg(m_location.sectionName)
                        .arg(formatHex64(m_location.kernelAddress)));
            }
        }

        // 写入桩定位结果显示。
        if (m_ksecGadgetLabel != nullptr)
        {
            if (!m_gadgetLocation.ok)
            {
                m_ksecGadgetLabel->setText(
                    ks::i18n::text(
                        QStringLiteral("misc.disable_dse.ksec.gadget.none"),
                        QStringLiteral("写入桩尚未定位。点“定位写入桩”搜索 ntoskrnl 中的 mov [rcx], edx; ret。")));
                m_ksecGadgetLabel->setStyleSheet(
                    QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
            }
            else
            {
                m_ksecGadgetLabel->setText(
                    QStringLiteral("ntoskrnl 基址 %1　RVA 0x%2　写入桩 %3")
                        .arg(formatHex64(m_gadgetLocation.ntoskrnlBase))
                        .arg(m_gadgetLocation.rva, 0, 16)
                        .arg(formatHex64(m_gadgetLocation.kernelAddress)));
                m_ksecGadgetLabel->setStyleSheet(
                    QStringLiteral("color:%1;").arg(KswordTheme::SuccessHex()));
            }
        }

        if (m_pendingLabel != nullptr)
        {
            m_pendingLabel->setVisible(m_hasSavedOriginal);
            if (m_hasSavedOriginal)
            {
                m_pendingLabel->setText(
                    QStringLiteral("驱动签名强制当前处于被本页修改的状态，原值 %1 已记录。请在加载完驱动后立即点“恢复原值”；若直接关闭程序，本页会在退出时尝试自动写回。")
                        .arg(formatHex32(m_savedOriginalValue)));
            }
        }

        // R0 选中时同步刷新 R0 方案摘要（驱动在线状态可能随时变化）。
        if (m_r0DriverButton != nullptr && m_r0DriverButton->isChecked())
        {
            updateR0StrategyDisplay();
        }
    }
}
