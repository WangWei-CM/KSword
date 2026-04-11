#include "ServiceDock.Internal.h"

#include <QFileDialog>
#include <QSignalBlocker>

using namespace service_dock_detail;

namespace
{
    // kToolbarIconSize 作用：
    // - 统一右侧属性页工具按钮图标尺寸；
    // - 与顶部工具栏保持一致，降低视觉割裂感。
    constexpr QSize kToolbarIconSize(16, 16);

    // isLocalSystemAccountText 作用：
    // - 判断账户文本是否代表 LocalSystem；
    // - 登录页回填单选框时复用。
    bool isLocalSystemAccountText(const QString& accountText)
    {
        const QString normalizedText = accountText.trimmed().toLower();
        return normalizedText == QStringLiteral("localsystem")
            || normalizedText == QStringLiteral("nt authority\\localsystem");
    }

    // buildPropertyActionButton 作用：
    // - 构建属性页顶部动作按钮；
    // - 统一图标、尺寸与 tooltip 规则。
    QToolButton* buildPropertyActionButton(
        QWidget* parentWidget,
        const char* iconPath,
        const QString& toolTipText)
    {
        QToolButton* button = new QToolButton(parentWidget);
        button->setIcon(createBlueIcon(iconPath, kToolbarIconSize));
        button->setIconSize(kToolbarIconSize);
        button->setFixedSize(28, 28);
        button->setToolTip(toolTipText);
        return button;
    }

    // buildRecoveryActionCombo 作用：
    // - 构建恢复动作下拉框并写入动作枚举数据；
    // - 恢复页三组动作共用一套选项。
    QComboBox* buildRecoveryActionCombo(QWidget* parentWidget)
    {
        QComboBox* comboBox = new QComboBox(parentWidget);
        comboBox->addItem(QStringLiteral("无操作"), static_cast<int>(SC_ACTION_NONE));
        comboBox->addItem(QStringLiteral("重新启动服务"), static_cast<int>(SC_ACTION_RESTART));
        comboBox->addItem(QStringLiteral("运行程序"), static_cast<int>(SC_ACTION_RUN_COMMAND));
        comboBox->addItem(QStringLiteral("重新启动计算机"), static_cast<int>(SC_ACTION_REBOOT));
        return comboBox;
    }

    // splitRecoveryCommandText 作用：
    // - 把 FailureActions 的 command 文本拆成“程序 + 参数”；
    // - 同时识别是否附带 /fail=%1% 结尾参数。
    void splitRecoveryCommandText(
        const QString& commandText,
        QString* programPathOut,
        QString* argumentsOut,
        bool* appendFailureCountOut)
    {
        if (programPathOut != nullptr)
        {
            *programPathOut = QString();
        }
        if (argumentsOut != nullptr)
        {
            *argumentsOut = QString();
        }
        if (appendFailureCountOut != nullptr)
        {
            *appendFailureCountOut = false;
        }

        QString workingText = commandText.trimmed();
        if (workingText.isEmpty())
        {
            return;
        }

        const QString failTokenText = QStringLiteral("/fail=%1%");
        if (workingText.contains(failTokenText, Qt::CaseInsensitive))
        {
            workingText.replace(failTokenText, QString(), Qt::CaseInsensitive);
            workingText = workingText.trimmed();
            if (appendFailureCountOut != nullptr)
            {
                *appendFailureCountOut = true;
            }
        }

        if (workingText.startsWith('\"'))
        {
            const int endQuoteIndex = workingText.indexOf('\"', 1);
            if (endQuoteIndex > 1)
            {
                if (programPathOut != nullptr)
                {
                    *programPathOut = workingText.mid(1, endQuoteIndex - 1).trimmed();
                }
                if (argumentsOut != nullptr)
                {
                    *argumentsOut = workingText.mid(endQuoteIndex + 1).trimmed();
                }
                return;
            }
        }

        const int firstSpaceIndex = workingText.indexOf(' ');
        if (firstSpaceIndex > 0)
        {
            if (programPathOut != nullptr)
            {
                *programPathOut = workingText.left(firstSpaceIndex).trimmed();
            }
            if (argumentsOut != nullptr)
            {
                *argumentsOut = workingText.mid(firstSpaceIndex + 1).trimmed();
            }
            return;
        }

        if (programPathOut != nullptr)
        {
            *programPathOut = workingText;
        }
    }

    // composeRecoveryCommandText 作用：
    // - 按“程序 + 参数 + 可选 fail token”拼装恢复命令；
    // - 保存恢复配置时复用，避免字符串规则分散。
    QString composeRecoveryCommandText(
        const QString& programPathText,
        const QString& argumentsText,
        const bool appendFailureCount)
    {
        QStringList segmentList;
        const QString normalizedProgramPath = programPathText.trimmed();
        if (!normalizedProgramPath.isEmpty())
        {
            segmentList.push_back(normalizedProgramPath.contains(' ')
                ? QStringLiteral("\"%1\"").arg(normalizedProgramPath)
                : normalizedProgramPath);
        }

        const QString normalizedArguments = argumentsText.trimmed();
        if (!normalizedArguments.isEmpty())
        {
            segmentList.push_back(normalizedArguments);
        }
        if (appendFailureCount)
        {
            segmentList.push_back(QStringLiteral("/fail=%1%"));
        }

        return segmentList.join(QStringLiteral(" ")).trimmed();
    }
}

void ServiceDock::initializeDetailTabs()
{
    m_generalTabPage = new QWidget(m_detailTabWidget);
    m_logonTabPage = new QWidget(m_detailTabWidget);
    m_recoveryTabPage = new QWidget(m_detailTabWidget);
    m_dependencyTabPage = new QWidget(m_detailTabWidget);
    m_auditTabPage = new QWidget(m_detailTabWidget);

    initializeGeneralTab();
    initializeLogonTab();
    initializeRecoveryTab();
    initializeDependencyTab();
    initializeAuditTab();

    m_detailTabWidget->addTab(m_generalTabPage, createBlueIcon(":/Icon/process_details.svg"), QStringLiteral("常规"));
    m_detailTabWidget->addTab(m_logonTabPage, createBlueIcon(":/Icon/process_main.svg"), QStringLiteral("登录"));
    m_detailTabWidget->addTab(m_recoveryTabPage, createBlueIcon(":/Icon/process_refresh.svg"), QStringLiteral("恢复"));
    m_detailTabWidget->addTab(m_dependencyTabPage, createBlueIcon(":/Icon/process_list.svg"), QStringLiteral("依存关系"));
    m_detailTabWidget->addTab(m_auditTabPage, createBlueIcon(":/Icon/process_critical.svg"), QStringLiteral("审计"));

    m_detailTabWidget->setTabToolTip(0, QStringLiteral("常规属性与启动类型配置"));
    m_detailTabWidget->setTabToolTip(1, QStringLiteral("服务登录身份与交互桌面配置"));
    m_detailTabWidget->setTabToolTip(2, QStringLiteral("服务失败恢复动作配置"));
    m_detailTabWidget->setTabToolTip(3, QStringLiteral("服务依赖与反向依赖信息"));
    m_detailTabWidget->setTabToolTip(4, QStringLiteral("触发器、安全、风险与导出信息"));
}

void ServiceDock::initializeGeneralTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_generalTabPage);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    QWidget* actionRowWidget = new QWidget(m_generalTabPage);
    QHBoxLayout* actionRowLayout = new QHBoxLayout(actionRowWidget);
    actionRowLayout->setContentsMargins(0, 0, 0, 0);
    actionRowLayout->setSpacing(6);

    m_generalStartButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_start.svg", QStringLiteral("启动当前服务"));
    m_generalStopButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_terminate.svg", QStringLiteral("停止当前服务"));
    m_generalPauseButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_pause.svg", QStringLiteral("暂停当前服务"));
    m_generalContinueButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_resume.svg", QStringLiteral("继续当前服务"));
    m_generalReloadButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_refresh.svg", QStringLiteral("重载当前服务属性"));
    m_generalApplyButton = buildPropertyActionButton(actionRowWidget, ":/Icon/codeeditor_save.svg", QStringLiteral("应用常规页修改"));

    actionRowLayout->addWidget(m_generalStartButton);
    actionRowLayout->addWidget(m_generalStopButton);
    actionRowLayout->addWidget(m_generalPauseButton);
    actionRowLayout->addWidget(m_generalContinueButton);
    actionRowLayout->addStretch(1);
    actionRowLayout->addWidget(m_generalReloadButton);
    actionRowLayout->addWidget(m_generalApplyButton);
    rootLayout->addWidget(actionRowWidget, 0);

    QFormLayout* formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    formLayout->setFormAlignment(Qt::AlignTop);
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(8);

    m_generalServiceNameEdit = new QLineEdit(m_generalTabPage);
    m_generalServiceNameEdit->setReadOnly(true);
    m_generalDisplayNameEdit = new QLineEdit(m_generalTabPage);
    m_generalBinaryPathEdit = new QLineEdit(m_generalTabPage);
    m_generalBinaryPathEdit->setReadOnly(true);
    m_generalDescriptionEdit = new QPlainTextEdit(m_generalTabPage);
    m_generalDescriptionEdit->setFixedHeight(96);
    m_generalStartTypeCombo = new QComboBox(m_generalTabPage);
    m_generalStartTypeCombo->addItem(QStringLiteral("自动"), static_cast<qulonglong>(SERVICE_AUTO_START));
    m_generalStartTypeCombo->addItem(QStringLiteral("手动"), static_cast<qulonglong>(SERVICE_DEMAND_START));
    m_generalStartTypeCombo->addItem(QStringLiteral("禁用"), static_cast<qulonglong>(SERVICE_DISABLED));
    m_generalDelayedAutoCheck = new QCheckBox(QStringLiteral("延迟自动启动"), m_generalTabPage);

    QWidget* startTypeRowWidget = new QWidget(m_generalTabPage);
    QHBoxLayout* startTypeRowLayout = new QHBoxLayout(startTypeRowWidget);
    startTypeRowLayout->setContentsMargins(0, 0, 0, 0);
    startTypeRowLayout->setSpacing(8);
    startTypeRowLayout->addWidget(m_generalStartTypeCombo, 1);
    startTypeRowLayout->addWidget(m_generalDelayedAutoCheck, 0);

    m_generalStateValueLabel = new QLabel(QStringLiteral("-"), m_generalTabPage);
    m_generalPidValueLabel = new QLabel(QStringLiteral("-"), m_generalTabPage);
    m_generalAccountValueLabel = new QLabel(QStringLiteral("-"), m_generalTabPage);
    m_generalTypeValueLabel = new QLabel(QStringLiteral("-"), m_generalTabPage);
    m_generalErrorControlValueLabel = new QLabel(QStringLiteral("-"), m_generalTabPage);

    formLayout->addRow(QStringLiteral("服务名"), m_generalServiceNameEdit);
    formLayout->addRow(QStringLiteral("显示名"), m_generalDisplayNameEdit);
    formLayout->addRow(QStringLiteral("路径"), m_generalBinaryPathEdit);
    formLayout->addRow(QStringLiteral("描述"), m_generalDescriptionEdit);
    formLayout->addRow(QStringLiteral("启动类型"), startTypeRowWidget);
    formLayout->addRow(QStringLiteral("状态"), m_generalStateValueLabel);
    formLayout->addRow(QStringLiteral("PID"), m_generalPidValueLabel);
    formLayout->addRow(QStringLiteral("账户"), m_generalAccountValueLabel);
    formLayout->addRow(QStringLiteral("类型"), m_generalTypeValueLabel);
    formLayout->addRow(QStringLiteral("错误控制"), m_generalErrorControlValueLabel);
    rootLayout->addLayout(formLayout, 1);

    connect(m_generalStartButton, &QToolButton::clicked, this, [this]() { startSelectedService(); });
    connect(m_generalStopButton, &QToolButton::clicked, this, [this]() { stopSelectedService(); });
    connect(m_generalPauseButton, &QToolButton::clicked, this, [this]() { pauseSelectedService(); });
    connect(m_generalContinueButton, &QToolButton::clicked, this, [this]() { continueSelectedService(); });
    connect(m_generalReloadButton, &QToolButton::clicked, this, [this]() { refreshSelectedService(); });
    connect(m_generalApplyButton, &QToolButton::clicked, this, [this]() { applyGeneralTabChanges(); });
    connect(m_generalStartTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshGeneralTabUiState(); });
}

void ServiceDock::initializeLogonTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_logonTabPage);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    QWidget* actionRowWidget = new QWidget(m_logonTabPage);
    QHBoxLayout* actionRowLayout = new QHBoxLayout(actionRowWidget);
    actionRowLayout->setContentsMargins(0, 0, 0, 0);
    actionRowLayout->setSpacing(6);
    m_logonReloadButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_refresh.svg", QStringLiteral("重载登录页属性"));
    m_logonApplyButton = buildPropertyActionButton(actionRowWidget, ":/Icon/codeeditor_save.svg", QStringLiteral("应用登录页修改"));
    actionRowLayout->addStretch(1);
    actionRowLayout->addWidget(m_logonReloadButton);
    actionRowLayout->addWidget(m_logonApplyButton);
    rootLayout->addWidget(actionRowWidget, 0);

    m_logonLocalSystemRadio = new QRadioButton(QStringLiteral("本地系统帐户"), m_logonTabPage);
    m_logonDesktopInteractCheck = new QCheckBox(QStringLiteral("允许服务与桌面交互"), m_logonTabPage);
    m_logonAccountRadio = new QRadioButton(QStringLiteral("此帐户"), m_logonTabPage);
    m_logonAccountEdit = new QLineEdit(m_logonTabPage);
    m_logonPasswordEdit = new QLineEdit(m_logonTabPage);
    m_logonPasswordEdit->setEchoMode(QLineEdit::Password);
    m_logonConfirmPasswordEdit = new QLineEdit(m_logonTabPage);
    m_logonConfirmPasswordEdit->setEchoMode(QLineEdit::Password);
    m_logonBrowseButton = buildPropertyActionButton(m_logonTabPage, ":/Icon/process_open_folder.svg", QStringLiteral("输入或浏览服务登录帐户"));

    rootLayout->addWidget(m_logonLocalSystemRadio, 0);
    rootLayout->addWidget(m_logonDesktopInteractCheck, 0);
    rootLayout->addWidget(m_logonAccountRadio, 0);

    QFormLayout* formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(8);

    QWidget* accountRowWidget = new QWidget(m_logonTabPage);
    QHBoxLayout* accountRowLayout = new QHBoxLayout(accountRowWidget);
    accountRowLayout->setContentsMargins(0, 0, 0, 0);
    accountRowLayout->setSpacing(6);
    accountRowLayout->addWidget(m_logonAccountEdit, 1);
    accountRowLayout->addWidget(m_logonBrowseButton, 0);

    formLayout->addRow(QStringLiteral("帐户"), accountRowWidget);
    formLayout->addRow(QStringLiteral("密码"), m_logonPasswordEdit);
    formLayout->addRow(QStringLiteral("确认密码"), m_logonConfirmPasswordEdit);
    rootLayout->addLayout(formLayout, 1);

    connect(m_logonLocalSystemRadio, &QRadioButton::toggled, this, [this](bool) { refreshLogonTabUiState(); });
    connect(m_logonAccountRadio, &QRadioButton::toggled, this, [this](bool) { refreshLogonTabUiState(); });
    connect(m_logonBrowseButton, &QToolButton::clicked, this, [this]() { browseLogonAccount(); });
    connect(m_logonReloadButton, &QToolButton::clicked, this, [this]() { refreshSelectedService(); });
    connect(m_logonApplyButton, &QToolButton::clicked, this, [this]() { applyLogonTabChanges(); });
}

void ServiceDock::initializeRecoveryTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_recoveryTabPage);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    QWidget* actionRowWidget = new QWidget(m_recoveryTabPage);
    QHBoxLayout* actionRowLayout = new QHBoxLayout(actionRowWidget);
    actionRowLayout->setContentsMargins(0, 0, 0, 0);
    actionRowLayout->setSpacing(6);
    m_recoveryReloadButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_refresh.svg", QStringLiteral("重载恢复页属性"));
    m_recoveryApplyButton = buildPropertyActionButton(actionRowWidget, ":/Icon/codeeditor_save.svg", QStringLiteral("应用恢复页修改"));
    actionRowLayout->addStretch(1);
    actionRowLayout->addWidget(m_recoveryReloadButton);
    actionRowLayout->addWidget(m_recoveryApplyButton);
    rootLayout->addWidget(actionRowWidget, 0);

    QFormLayout* formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(8);

    m_recoveryFirstActionCombo = buildRecoveryActionCombo(m_recoveryTabPage);
    m_recoverySecondActionCombo = buildRecoveryActionCombo(m_recoveryTabPage);
    m_recoverySubsequentActionCombo = buildRecoveryActionCombo(m_recoveryTabPage);
    m_recoveryResetDaysSpin = new QSpinBox(m_recoveryTabPage);
    m_recoveryResetDaysSpin->setRange(0, 3650);
    m_recoveryResetDaysSpin->setSuffix(QStringLiteral(" 天"));
    m_recoveryRestartMinutesSpin = new QSpinBox(m_recoveryTabPage);
    m_recoveryRestartMinutesSpin->setRange(0, 1440);
    m_recoveryRestartMinutesSpin->setSuffix(QStringLiteral(" 分钟"));
    m_recoveryFailureActionsFlagCheck = new QCheckBox(QStringLiteral("启用发生错误便停止时的操作"), m_recoveryTabPage);
    m_recoveryRebootMessageEdit = new QLineEdit(m_recoveryTabPage);
    m_recoveryProgramEdit = new QLineEdit(m_recoveryTabPage);
    m_recoveryArgumentsEdit = new QLineEdit(m_recoveryTabPage);
    m_recoveryAppendFailCountCheck = new QCheckBox(QStringLiteral("将失败计数附加到命令行结尾 (/fail=%1%)"), m_recoveryTabPage);
    m_recoveryBrowseProgramButton = buildPropertyActionButton(m_recoveryTabPage, ":/Icon/process_open_folder.svg", QStringLiteral("浏览恢复动作程序路径"));

    QWidget* programRowWidget = new QWidget(m_recoveryTabPage);
    QHBoxLayout* programRowLayout = new QHBoxLayout(programRowWidget);
    programRowLayout->setContentsMargins(0, 0, 0, 0);
    programRowLayout->setSpacing(6);
    programRowLayout->addWidget(m_recoveryProgramEdit, 1);
    programRowLayout->addWidget(m_recoveryBrowseProgramButton, 0);

    formLayout->addRow(QStringLiteral("第一次失败"), m_recoveryFirstActionCombo);
    formLayout->addRow(QStringLiteral("第二次失败"), m_recoverySecondActionCombo);
    formLayout->addRow(QStringLiteral("后续失败"), m_recoverySubsequentActionCombo);
    formLayout->addRow(QStringLiteral("重置失败计数"), m_recoveryResetDaysSpin);
    formLayout->addRow(QStringLiteral("重新启动服务"), m_recoveryRestartMinutesSpin);
    formLayout->addRow(QStringLiteral("错误停止"), m_recoveryFailureActionsFlagCheck);
    formLayout->addRow(QStringLiteral("重启消息"), m_recoveryRebootMessageEdit);
    formLayout->addRow(QStringLiteral("程序"), programRowWidget);
    formLayout->addRow(QStringLiteral("命令行参数"), m_recoveryArgumentsEdit);
    formLayout->addRow(QStringLiteral("失败计数"), m_recoveryAppendFailCountCheck);
    rootLayout->addLayout(formLayout, 1);

    connect(m_recoveryFirstActionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshRecoveryTabUiState(); });
    connect(m_recoverySecondActionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshRecoveryTabUiState(); });
    connect(m_recoverySubsequentActionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshRecoveryTabUiState(); });
    connect(m_recoveryBrowseProgramButton, &QToolButton::clicked, this, [this]() { browseRecoveryProgramPath(); });
    connect(m_recoveryReloadButton, &QToolButton::clicked, this, [this]() { refreshSelectedService(); });
    connect(m_recoveryApplyButton, &QToolButton::clicked, this, [this]() { applyRecoveryTabChanges(); });
}

void ServiceDock::initializeDependencyTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_dependencyTabPage);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    m_dependencyEditor = new CodeEditorWidget(m_dependencyTabPage);
    m_dependencyEditor->setReadOnly(true);
    m_dependencyEditor->setText(QStringLiteral("未选择服务"));
    rootLayout->addWidget(m_dependencyEditor, 1);
}

void ServiceDock::initializeAuditTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_auditTabPage);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    m_auditEditor = new CodeEditorWidget(m_auditTabPage);
    m_auditEditor->setReadOnly(true);
    m_auditEditor->setText(QStringLiteral("未选择服务"));
    rootLayout->addWidget(m_auditEditor, 1);
}

void ServiceDock::refreshGeneralTabUiState()
{
    if (m_generalStartTypeCombo == nullptr || m_generalDelayedAutoCheck == nullptr)
    {
        return;
    }

    const DWORD startTypeValue = static_cast<DWORD>(m_generalStartTypeCombo->currentData().toULongLong());
    const bool autoStartType = startTypeValue == SERVICE_AUTO_START;
    if (!autoStartType)
    {
        m_generalDelayedAutoCheck->setChecked(false);
    }
    m_generalDelayedAutoCheck->setEnabled(autoStartType);
}

void ServiceDock::refreshLogonTabUiState()
{
    if (m_logonLocalSystemRadio == nullptr || m_logonAccountRadio == nullptr)
    {
        return;
    }

    const bool useLocalSystem = m_logonLocalSystemRadio->isChecked();
    const bool useCustomAccount = m_logonAccountRadio->isChecked();
    m_logonDesktopInteractCheck->setEnabled(useLocalSystem);
    if (!useLocalSystem)
    {
        m_logonDesktopInteractCheck->setChecked(false);
    }

    m_logonAccountEdit->setEnabled(useCustomAccount);
    m_logonPasswordEdit->setEnabled(useCustomAccount);
    m_logonConfirmPasswordEdit->setEnabled(useCustomAccount);
    m_logonBrowseButton->setEnabled(useCustomAccount);
}

void ServiceDock::refreshRecoveryTabUiState()
{
    const auto actionTypeFromCombo = [](QComboBox* comboBox) -> SC_ACTION_TYPE
        {
            if (comboBox == nullptr)
            {
                return SC_ACTION_NONE;
            }
            return static_cast<SC_ACTION_TYPE>(comboBox->currentData().toInt());
        };

    const SC_ACTION_TYPE firstActionType = actionTypeFromCombo(m_recoveryFirstActionCombo);
    const SC_ACTION_TYPE secondActionType = actionTypeFromCombo(m_recoverySecondActionCombo);
    const SC_ACTION_TYPE subsequentActionType = actionTypeFromCombo(m_recoverySubsequentActionCombo);

    const bool hasRestartAction =
        firstActionType == SC_ACTION_RESTART
        || secondActionType == SC_ACTION_RESTART
        || subsequentActionType == SC_ACTION_RESTART;
    const bool hasRunProgramAction =
        firstActionType == SC_ACTION_RUN_COMMAND
        || secondActionType == SC_ACTION_RUN_COMMAND
        || subsequentActionType == SC_ACTION_RUN_COMMAND;
    const bool hasRebootAction =
        firstActionType == SC_ACTION_REBOOT
        || secondActionType == SC_ACTION_REBOOT
        || subsequentActionType == SC_ACTION_REBOOT;

    m_recoveryRestartMinutesSpin->setEnabled(hasRestartAction);
    m_recoveryProgramEdit->setEnabled(hasRunProgramAction);
    m_recoveryArgumentsEdit->setEnabled(hasRunProgramAction);
    m_recoveryAppendFailCountCheck->setEnabled(hasRunProgramAction);
    m_recoveryBrowseProgramButton->setEnabled(hasRunProgramAction);
    m_recoveryRebootMessageEdit->setEnabled(hasRebootAction);
}

void ServiceDock::populateGeneralTab(const ServiceEntry& entry)
{
    const QSignalBlocker startTypeBlocker(m_generalStartTypeCombo);
    const QSignalBlocker delayedBlocker(m_generalDelayedAutoCheck);

    m_generalServiceNameEdit->setText(entry.serviceNameText);
    m_generalDisplayNameEdit->setText(entry.displayNameText);
    m_generalBinaryPathEdit->setText(entry.commandLineText);
    m_generalDescriptionEdit->setPlainText(entry.descriptionText);
    m_generalStateValueLabel->setText(entry.stateText);
    m_generalPidValueLabel->setText(entry.processId == 0 ? QStringLiteral("-") : QString::number(entry.processId));
    m_generalAccountValueLabel->setText(entry.accountText);
    m_generalTypeValueLabel->setText(entry.serviceTypeText);
    m_generalErrorControlValueLabel->setText(entry.errorControlText);

    const int startTypeIndex = m_generalStartTypeCombo->findData(static_cast<qulonglong>(entry.startTypeValue));
    m_generalStartTypeCombo->setCurrentIndex(startTypeIndex >= 0 ? startTypeIndex : 0);
    m_generalDelayedAutoCheck->setChecked(entry.delayedAutoStart);
    refreshGeneralTabUiState();
}

void ServiceDock::populateLogonTab(const ServiceEntry& entry)
{
    const QSignalBlocker localSystemBlocker(m_logonLocalSystemRadio);
    const QSignalBlocker accountBlocker(m_logonAccountRadio);
    const QSignalBlocker desktopBlocker(m_logonDesktopInteractCheck);

    const bool useLocalSystem = isLocalSystemAccountText(entry.accountText);
    m_logonLocalSystemRadio->setChecked(useLocalSystem);
    m_logonAccountRadio->setChecked(!useLocalSystem);
    m_logonDesktopInteractCheck->setChecked((entry.serviceTypeValue & SERVICE_INTERACTIVE_PROCESS) != 0);
    m_logonAccountEdit->setText(useLocalSystem ? QString() : entry.accountText);
    m_logonPasswordEdit->clear();
    m_logonConfirmPasswordEdit->clear();
    refreshLogonTabUiState();
}

void ServiceDock::populateRecoveryTab(const ServiceEntry& entry)
{
    ServiceRecoverySettings settings;
    QString errorText;
    const bool queryOk = queryServiceFailureSettings(entry.serviceNameText, &settings, &errorText);

    const QSignalBlocker firstBlocker(m_recoveryFirstActionCombo);
    const QSignalBlocker secondBlocker(m_recoverySecondActionCombo);
    const QSignalBlocker subsequentBlocker(m_recoverySubsequentActionCombo);
    const QSignalBlocker resetBlocker(m_recoveryResetDaysSpin);
    const QSignalBlocker restartBlocker(m_recoveryRestartMinutesSpin);
    const QSignalBlocker flagBlocker(m_recoveryFailureActionsFlagCheck);
    const QSignalBlocker appendBlocker(m_recoveryAppendFailCountCheck);

    const auto setComboByActionType = [](QComboBox* comboBox, SC_ACTION_TYPE actionType)
        {
            if (comboBox == nullptr)
            {
                return;
            }
            const int targetIndex = comboBox->findData(static_cast<int>(actionType));
            comboBox->setCurrentIndex(targetIndex >= 0 ? targetIndex : 0);
        };

    setComboByActionType(m_recoveryFirstActionCombo, settings.firstActionType);
    setComboByActionType(m_recoverySecondActionCombo, settings.secondActionType);
    setComboByActionType(m_recoverySubsequentActionCombo, settings.subsequentActionType);
    m_recoveryResetDaysSpin->setValue(settings.resetPeriodDays);
    m_recoveryRestartMinutesSpin->setValue(settings.restartDelayMinutes);
    m_recoveryFailureActionsFlagCheck->setChecked(settings.failureActionsFlag);
    m_recoveryRebootMessageEdit->setText(settings.rebootMessageText);
    m_recoveryProgramEdit->setText(settings.programPathText);
    m_recoveryArgumentsEdit->setText(settings.programArgumentsText);
    m_recoveryAppendFailCountCheck->setChecked(settings.appendFailureCount);
    if (!queryOk)
    {
        m_recoveryProgramEdit->setPlaceholderText(errorText);
    }
    else
    {
        m_recoveryProgramEdit->setPlaceholderText(QString());
    }
    refreshRecoveryTabUiState();
}

void ServiceDock::populateDependencyTab(const ServiceEntry& entry)
{
    if (m_dependencyEditor != nullptr)
    {
        m_dependencyEditor->setText(buildDependencyDetailText(entry));
    }
}

void ServiceDock::populateAuditTab(const ServiceEntry& entry)
{
    if (m_auditEditor != nullptr)
    {
        m_auditEditor->setText(buildAuditTabText(entry));
    }
}

void ServiceDock::updateDetailViewsFromSelection()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    const bool hasSelection = selectedIndex >= 0 && selectedIndex < static_cast<int>(m_serviceList.size());

    m_detailUiSyncInProgress = true;
    if (!hasSelection)
    {
        m_generalServiceNameEdit->clear();
        m_generalDisplayNameEdit->clear();
        m_generalBinaryPathEdit->clear();
        m_generalDescriptionEdit->clear();
        m_generalStateValueLabel->setText(QStringLiteral("-"));
        m_generalPidValueLabel->setText(QStringLiteral("-"));
        m_generalAccountValueLabel->setText(QStringLiteral("-"));
        m_generalTypeValueLabel->setText(QStringLiteral("-"));
        m_generalErrorControlValueLabel->setText(QStringLiteral("-"));

        m_logonLocalSystemRadio->setChecked(true);
        m_logonAccountEdit->clear();
        m_logonPasswordEdit->clear();
        m_logonConfirmPasswordEdit->clear();
        m_logonDesktopInteractCheck->setChecked(false);

        m_recoveryFirstActionCombo->setCurrentIndex(0);
        m_recoverySecondActionCombo->setCurrentIndex(0);
        m_recoverySubsequentActionCombo->setCurrentIndex(0);
        m_recoveryResetDaysSpin->setValue(0);
        m_recoveryRestartMinutesSpin->setValue(1);
        m_recoveryFailureActionsFlagCheck->setChecked(false);
        m_recoveryRebootMessageEdit->clear();
        m_recoveryProgramEdit->clear();
        m_recoveryArgumentsEdit->clear();
        m_recoveryAppendFailCountCheck->setChecked(false);

        if (m_dependencyEditor != nullptr) { m_dependencyEditor->setText(QStringLiteral("未选择服务")); }
        if (m_auditEditor != nullptr) { m_auditEditor->setText(QStringLiteral("未选择服务")); }
    }
    else
    {
        const ServiceEntry& entry = m_serviceList[static_cast<std::size_t>(selectedIndex)];
        populateGeneralTab(entry);
        populateLogonTab(entry);
        populateRecoveryTab(entry);
        populateDependencyTab(entry);
        populateAuditTab(entry);
    }
    m_detailUiSyncInProgress = false;

    refreshGeneralTabUiState();
    refreshLogonTabUiState();
    refreshRecoveryTabUiState();

    const bool enableApplyButtons = hasSelection;
    if (m_generalApplyButton != nullptr) { m_generalApplyButton->setEnabled(enableApplyButtons); }
    if (m_generalReloadButton != nullptr) { m_generalReloadButton->setEnabled(enableApplyButtons); }
    if (m_logonApplyButton != nullptr) { m_logonApplyButton->setEnabled(enableApplyButtons); }
    if (m_logonReloadButton != nullptr) { m_logonReloadButton->setEnabled(enableApplyButtons); }
    if (m_recoveryApplyButton != nullptr) { m_recoveryApplyButton->setEnabled(enableApplyButtons); }
    if (m_recoveryReloadButton != nullptr) { m_recoveryReloadButton->setEnabled(enableApplyButtons); }
}

void ServiceDock::browseLogonAccount()
{
    bool ok = false;
    const QString accountText = QInputDialog::getText(
        this,
        QStringLiteral("输入服务登录帐户"),
        QStringLiteral("帐户名"),
        QLineEdit::Normal,
        m_logonAccountEdit->text(),
        &ok);
    if (ok)
    {
        m_logonAccountEdit->setText(accountText.trimmed());
    }
}

void ServiceDock::browseRecoveryProgramPath()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择恢复程序"),
        m_recoveryProgramEdit->text(),
        QStringLiteral("可执行文件 (*.exe *.cmd *.bat *.com);;所有文件 (*.*)"));
    if (!filePath.trimmed().isEmpty())
    {
        m_recoveryProgramEdit->setText(QDir::toNativeSeparators(filePath));
    }
}
