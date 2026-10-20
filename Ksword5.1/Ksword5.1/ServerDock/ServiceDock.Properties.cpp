#include "../UI/StructuredFieldView.h"
#include "ServiceDock.Internal.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/CodeTextEdit.h"
#include "../UI/DetailPresentation.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/FlowLayout.h"
#include "../UI/AsyncOperation.h"
#include "../UI/AsyncCleanupRegistry.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QFileDialog>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVariant>

using namespace service_dock_detail;

namespace
{
    // kDetailRenderDebounceMs 作用：
    // - 主列表连续换选中行时的详情渲染去抖窗口；
    // - 方向键连滚只在停下来之后触发一次重量级页签重建。
    constexpr int kDetailRenderDebounceMs = 150;

    // kDetailRenderTimerProperty 作用：
    // - 存放“详情页延迟渲染”单发定时器指针；
    // - 本次改动不扩展共享头 ServiceDock.h，故用动态属性承载这个状态。
    constexpr const char* kDetailRenderTimerProperty = "service_detail_render_timer";

    // kRenderedServiceNameProperty 作用：
    // - 记录某个重量级页签当前展示的是哪个服务；
    // - 切换页签时据此判断内容是否过期，避免无谓重查与冲掉未保存编辑。
    constexpr const char* kRenderedServiceNameProperty = "service_detail_rendered_service";

    // detailRenderTimerOf 作用：取出挂在 ServiceDock 上的延迟渲染定时器。
    // 入参：ownerWidget 为 ServiceDock 实例。
    // 返回：定时器指针；尚未初始化时返回 nullptr。
    QTimer* detailRenderTimerOf(const QWidget* ownerWidget)
    {
        if (ownerWidget == nullptr)
        {
            return nullptr;
        }
        return qobject_cast<QTimer*>(ownerWidget->property(kDetailRenderTimerProperty).value<QObject*>());
    }

    // markDetailPageRendered 作用：记录某个重量级页签当前渲染的服务短名。
    // 入参：detailPage 为页签根控件；serviceNameText 为已渲染的服务短名，空串表示作废。
    // 返回：无。
    void markDetailPageRendered(QWidget* detailPage, const QString& serviceNameText)
    {
        if (detailPage != nullptr)
        {
            detailPage->setProperty(kRenderedServiceNameProperty, serviceNameText);
        }
    }

    // isDetailPageRenderedFor 作用：判断某个重量级页签的内容是否已对应指定服务。
    // 入参：detailPage 为页签根控件；serviceNameText 为当前选中服务短名。
    // 返回：内容已是该服务的最新渲染结果时返回 true。
    bool isDetailPageRenderedFor(const QWidget* detailPage, const QString& serviceNameText)
    {
        if (detailPage == nullptr || serviceNameText.trimmed().isEmpty())
        {
            return false;
        }

        const QString renderedServiceNameText = detailPage->property(kRenderedServiceNameProperty).toString();
        return !renderedServiceNameText.isEmpty()
            && QString::compare(renderedServiceNameText, serviceNameText, Qt::CaseInsensitive) == 0;
    }

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
        button->setIcon(createBlueIcon(iconPath, KswordTheme::CompactIconSize()));
        KswordTheme::ApplyCompactIconButtonMetrics(button);
        button->setToolTip(toolTipText);
        ks::ui::ApplyFlatButtonTheme(button, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
        ks::ui::NormalizeToolbarControl(button);
        return button;
    }

    // 服务编辑页只让卡片正文滚动，顶部重载和保存始终可达；表格页仍自行滚动。
    // page/root：原业务页与顶层布局；返回正文的纵向布局，所有字段对象保持不变。
    QVBoxLayout* createServiceCardBody(QWidget* page, QVBoxLayout* root)
    {
        auto* scroll = new QScrollArea(page);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setMinimumSize(0, 0);
        scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        auto* content = new QWidget(scroll);
        content->setMinimumWidth(0);
        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* body = new QVBoxLayout(content);
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(12);
        body->setAlignment(Qt::AlignTop);
        scroll->setWidget(content);
        root->addWidget(scroll, 1);
        return body;
    }

    // 将字段值约束在当前列内，长账户名或类型说明按窗口宽度换行，不撑宽页面。
    void configureServiceValue(QLabel* value)
    {
        value->setWordWrap(true);
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
        ks::ui::StylePrimaryCombo(comboBox);
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

    // 恢复/依存关系/审计三页每次回填都要重新走 SCM（合计十余次到 services.exe 的 RPC），
    // 早先在 itemSelectionChanged 里同步全渲染，方向键连滚时每行都要付这笔开销。
    // 这里改成单发定时器统一调度：
    // 1) 换选中行只重启定时器，连滚过程中不产生任何 SCM 往返；
    // 2) 定时器只渲染当前可见的那一页，看不见的页留到切过去时再补。
    QTimer* const detailRenderTimer = new QTimer(this);
    detailRenderTimer->setSingleShot(true);
    setProperty(kDetailRenderTimerProperty, QVariant::fromValue<QObject*>(detailRenderTimer));

    connect(detailRenderTimer, &QTimer::timeout, this, [this]() { requestCurrentDetail(); });

    connect(m_detailTabWidget, &QTabWidget::currentChanged, this, [this](const int)
        {
            // 切换页签时立即补渲染：这里不作废任何标记，
            // 因此已经对应当前服务的页会被跳过，用户未保存的编辑不会被冲掉。
            QTimer* const renderTimer = detailRenderTimerOf(this);
            if (renderTimer != nullptr)
            {
                renderTimer->start(0);
            }
        });
}

void ServiceDock::initializeGeneralTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_generalTabPage);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(12);

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
    ks::ui::NormalizeToolbarRow(actionRowLayout);
    rootLayout->addWidget(actionRowWidget, 0);

    // 常规配置与运行状态分成独立卡片；正文仅有这一层滚动。
    QVBoxLayout* body = createServiceCardBody(m_generalTabPage, rootLayout);
    QWidget* generalCard = ks::ui::CreateDetailCard(QStringLiteral("常规"), m_generalTabPage);
    auto* formLayout = new QFormLayout();
    ks::ui::ConfigureDetailForm(formLayout);
    qobject_cast<QVBoxLayout*>(generalCard->layout())->addLayout(formLayout);
    body->addWidget(generalCard);

    m_generalServiceNameEdit = new QLineEdit(m_generalTabPage);
    m_generalServiceNameEdit->setReadOnly(true);
    m_generalDisplayNameEdit = new QLineEdit(m_generalTabPage);
    m_generalBinaryPathEdit = new QLineEdit(m_generalTabPage);
    m_generalBinaryPathEdit->setReadOnly(true);
    auto* descriptionEditor = new CodeTextEdit(m_generalTabPage);
    descriptionEditor->setSyntaxLanguage(CodeTextEdit::SyntaxLanguage::PlainText);
    descriptionEditor->setLineNumbersVisible(false);
    descriptionEditor->setCompactMode(true);
    m_generalDescriptionEdit = descriptionEditor;
    m_generalDescriptionEdit->setMinimumHeight(72);
    m_generalDescriptionEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_generalStartTypeCombo = new QComboBox(m_generalTabPage);
    m_generalStartTypeCombo->addItem(QStringLiteral("自动"), static_cast<qulonglong>(SERVICE_AUTO_START));
    m_generalStartTypeCombo->addItem(QStringLiteral("手动"), static_cast<qulonglong>(SERVICE_DEMAND_START));
    m_generalStartTypeCombo->addItem(QStringLiteral("禁用"), static_cast<qulonglong>(SERVICE_DISABLED));
    ks::ui::StylePrimaryCombo(m_generalStartTypeCombo);
    for (QLineEdit* input : {m_generalServiceNameEdit, m_generalDisplayNameEdit, m_generalBinaryPathEdit})
    {
        ks::ui::StyleStandaloneInput(input);
    }
    m_generalDelayedAutoCheck = new QCheckBox(QStringLiteral("延迟自动启动"), m_generalTabPage);
    m_generalStartTypeCombo->setToolTip(
        QStringLiteral("设置该服务的启动方式：自动=开机自启，手动=按需启动，禁用=禁止启动"));
    m_generalDelayedAutoCheck->setToolTip(
        QStringLiteral("开机后延迟一段时间再启动该服务，可减轻开机瞬间的负载"));

    QWidget* startTypeRowWidget = new QWidget(m_generalTabPage);
    auto* startTypeRowLayout = new ks::ui::FlowLayout(startTypeRowWidget, 0, 8, 6);
    startTypeRowLayout->setContentsMargins(0, 0, 0, 0);
    startTypeRowLayout->setSpacing(8);
    startTypeRowLayout->addWidget(m_generalStartTypeCombo);
    startTypeRowLayout->addWidget(m_generalDelayedAutoCheck);

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
    // 宽窗将状态分成两列；收窄后恢复完整顺序，标签和值共同换行。
    QWidget* stateCard = ks::ui::CreateDetailCard(QStringLiteral("状态"), m_generalTabPage);
    QFormLayout* first = nullptr;
    QFormLayout* second = nullptr;
    ks::ui::AddDetailFieldColumns(stateCard, first, second);
    for (QLabel* value : {m_generalStateValueLabel, m_generalPidValueLabel,
        m_generalAccountValueLabel, m_generalTypeValueLabel, m_generalErrorControlValueLabel})
    {
        configureServiceValue(value);
    }
    first->addRow(QStringLiteral("状态"), m_generalStateValueLabel);
    first->addRow(QStringLiteral("PID"), m_generalPidValueLabel);
    first->addRow(QStringLiteral("账户"), m_generalAccountValueLabel);
    second->addRow(QStringLiteral("类型"), m_generalTypeValueLabel);
    second->addRow(QStringLiteral("错误控制"), m_generalErrorControlValueLabel);
    body->addWidget(stateCard);

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
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(12);

    QWidget* actionRowWidget = new QWidget(m_logonTabPage);
    QHBoxLayout* actionRowLayout = new QHBoxLayout(actionRowWidget);
    actionRowLayout->setContentsMargins(0, 0, 0, 0);
    actionRowLayout->setSpacing(6);
    m_logonReloadButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_refresh.svg", QStringLiteral("重载登录页属性"));
    m_logonApplyButton = buildPropertyActionButton(actionRowWidget, ":/Icon/codeeditor_save.svg", QStringLiteral("应用登录页修改"));
    actionRowLayout->addStretch(1);
    actionRowLayout->addWidget(m_logonReloadButton);
    actionRowLayout->addWidget(m_logonApplyButton);
    ks::ui::NormalizeToolbarRow(actionRowLayout);
    rootLayout->addWidget(actionRowWidget, 0);

    m_logonLocalSystemRadio = new QRadioButton(QStringLiteral("本地系统帐户"), m_logonTabPage);
    m_logonDesktopInteractCheck = new QCheckBox(QStringLiteral("允许服务与桌面交互"), m_logonTabPage);
    m_logonAccountRadio = new QRadioButton(QStringLiteral("此帐户"), m_logonTabPage);
    m_logonLocalSystemRadio->setToolTip(
        QStringLiteral("以系统内置的高权限帐户运行该服务"));
    m_logonDesktopInteractCheck->setToolTip(
        QStringLiteral("允许该服务在当前登录用户的桌面上显示窗口（较老的机制，现代系统通常无效）"));
    m_logonAccountRadio->setToolTip(
        QStringLiteral("改用下方指定的用户帐户运行该服务"));
    m_logonAccountEdit = new QLineEdit(m_logonTabPage);
    m_logonPasswordEdit = new QLineEdit(m_logonTabPage);
    m_logonPasswordEdit->setEchoMode(QLineEdit::Password);
    m_logonConfirmPasswordEdit = new QLineEdit(m_logonTabPage);
    m_logonConfirmPasswordEdit->setEchoMode(QLineEdit::Password);
    m_logonBrowseButton = buildPropertyActionButton(m_logonTabPage, ":/Icon/process_open_folder.svg", QStringLiteral("输入或浏览服务登录帐户"));

    // 身份选项与账户凭据分别成卡片，避免单列表单把空白高度分配给每一行。
    QVBoxLayout* body = createServiceCardBody(m_logonTabPage, rootLayout);
    QWidget* identityCard = ks::ui::CreateDetailCard(QStringLiteral("登录"), m_logonTabPage);
    auto* identityLayout = qobject_cast<QVBoxLayout*>(identityCard->layout());
    identityLayout->addWidget(m_logonLocalSystemRadio);
    identityLayout->addWidget(m_logonDesktopInteractCheck);
    identityLayout->addWidget(m_logonAccountRadio);
    body->addWidget(identityCard);
    for (QLineEdit* input : {m_logonAccountEdit, m_logonPasswordEdit, m_logonConfirmPasswordEdit})
    {
        ks::ui::StyleStandaloneInput(input);
    }

    QFormLayout* formLayout = new QFormLayout();
    ks::ui::ConfigureDetailForm(formLayout);

    QWidget* accountRowWidget = new QWidget(m_logonTabPage);
    QHBoxLayout* accountRowLayout = new QHBoxLayout(accountRowWidget);
    accountRowLayout->setContentsMargins(0, 0, 0, 0);
    accountRowLayout->setSpacing(6);
    accountRowLayout->addWidget(m_logonAccountEdit, 1);
    accountRowLayout->addWidget(m_logonBrowseButton, 0);

    formLayout->addRow(QStringLiteral("帐户"), accountRowWidget);
    formLayout->addRow(QStringLiteral("密码"), m_logonPasswordEdit);
    formLayout->addRow(QStringLiteral("确认密码"), m_logonConfirmPasswordEdit);
    QWidget* credentialsCard = ks::ui::CreateDetailCard(QStringLiteral("帐户"), m_logonTabPage);
    qobject_cast<QVBoxLayout*>(credentialsCard->layout())->addLayout(formLayout);
    body->addWidget(credentialsCard);

    connect(m_logonLocalSystemRadio, &QRadioButton::toggled, this, [this](bool) { refreshLogonTabUiState(); });
    connect(m_logonAccountRadio, &QRadioButton::toggled, this, [this](bool) { refreshLogonTabUiState(); });
    connect(m_logonBrowseButton, &QToolButton::clicked, this, [this]() { browseLogonAccount(); });
    connect(m_logonReloadButton, &QToolButton::clicked, this, [this]() { refreshSelectedService(); });
    connect(m_logonApplyButton, &QToolButton::clicked, this, [this]() { applyLogonTabChanges(); });
}

void ServiceDock::initializeRecoveryTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_recoveryTabPage);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(12);

    QWidget* actionRowWidget = new QWidget(m_recoveryTabPage);
    QHBoxLayout* actionRowLayout = new QHBoxLayout(actionRowWidget);
    actionRowLayout->setContentsMargins(0, 0, 0, 0);
    actionRowLayout->setSpacing(6);
    m_recoveryReloadButton = buildPropertyActionButton(actionRowWidget, ":/Icon/process_refresh.svg", QStringLiteral("重载恢复页属性"));
    m_recoveryApplyButton = buildPropertyActionButton(actionRowWidget, ":/Icon/codeeditor_save.svg", QStringLiteral("应用恢复页修改"));
    actionRowLayout->addStretch(1);
    actionRowLayout->addWidget(m_recoveryReloadButton);
    actionRowLayout->addWidget(m_recoveryApplyButton);
    ks::ui::NormalizeToolbarRow(actionRowLayout);
    rootLayout->addWidget(actionRowWidget, 0);
    m_recoveryStatusLabel = new QLabel(m_recoveryTabPage);
    m_recoveryStatusLabel->setWordWrap(true);
    rootLayout->addWidget(m_recoveryStatusLabel, 0);

    // 失败策略与恢复程序分组；双列阈值由共享详情布局按当前字体和宽度决定。
    QVBoxLayout* body = createServiceCardBody(m_recoveryTabPage, rootLayout);
    m_recoveryFields = body->parentWidget();
    QWidget* failureCard = ks::ui::CreateDetailCard(QStringLiteral("恢复"), m_recoveryTabPage);
    QFormLayout* formLayout = nullptr;
    QFormLayout* timingForm = nullptr;
    ks::ui::AddDetailFieldColumns(failureCard, formLayout, timingForm);
    body->addWidget(failureCard);

    m_recoveryFirstActionCombo = buildRecoveryActionCombo(m_recoveryTabPage);
    m_recoverySecondActionCombo = buildRecoveryActionCombo(m_recoveryTabPage);
    m_recoverySubsequentActionCombo = buildRecoveryActionCombo(m_recoveryTabPage);
    m_recoveryResetDaysSpin = new QSpinBox(m_recoveryTabPage);
    m_recoveryResetDaysSpin->setRange(0, 3650);
    m_recoveryResetDaysSpin->setSuffix(QStringLiteral(" 天"));
    m_recoveryRestartMinutesSpin = new QSpinBox(m_recoveryTabPage);
    m_recoveryRestartMinutesSpin->setRange(0, 1440);
    m_recoveryRestartMinutesSpin->setSuffix(QStringLiteral(" 分钟"));
    m_recoveryFirstActionCombo->setToolTip(
        QStringLiteral("服务第一次失败时执行的操作"));
    m_recoverySecondActionCombo->setToolTip(
        QStringLiteral("服务第二次失败时执行的操作"));
    m_recoverySubsequentActionCombo->setToolTip(
        QStringLiteral("服务第三次及以后失败时执行的操作"));
    m_recoveryResetDaysSpin->setToolTip(
        QStringLiteral("经过这些天没有再失败后，把失败计数清零重新计算"));
    m_recoveryRestartMinutesSpin->setToolTip(
        QStringLiteral("选择“重新启动服务”时，等待多久再重启"));
    m_recoveryFailureActionsFlagCheck = new QCheckBox(QStringLiteral("启用发生错误便停止时的操作"), m_recoveryTabPage);
    m_recoveryFailureActionsFlagCheck->setToolTip(
        QStringLiteral("服务以错误码异常结束时也触发上面的恢复操作；不勾选则只在崩溃时触发"));
    m_recoveryRebootMessageEdit = new QLineEdit(m_recoveryTabPage);
    m_recoveryProgramEdit = new QLineEdit(m_recoveryTabPage);
    m_recoveryArgumentsEdit = new QLineEdit(m_recoveryTabPage);
    for (QLineEdit* input : {m_recoveryRebootMessageEdit, m_recoveryProgramEdit, m_recoveryArgumentsEdit})
    {
        ks::ui::StyleStandaloneInput(input);
    }
    ks::ui::StyleStandaloneInput(m_recoveryResetDaysSpin);
    ks::ui::StyleStandaloneInput(m_recoveryRestartMinutesSpin);
    m_recoveryAppendFailCountCheck = new QCheckBox(QStringLiteral("将失败计数附加到命令行结尾 (/fail=%1%)"), m_recoveryTabPage);
    m_recoveryAppendFailCountCheck->setToolTip(
        QStringLiteral("运行恢复程序时，在命令行末尾附加当前是第几次失败"));
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
    timingForm->addRow(QStringLiteral("重置失败计数"), m_recoveryResetDaysSpin);
    timingForm->addRow(QStringLiteral("重新启动服务"), m_recoveryRestartMinutesSpin);
    timingForm->addRow(QStringLiteral("错误停止"), m_recoveryFailureActionsFlagCheck);
    QWidget* programCard = ks::ui::CreateDetailCard(QStringLiteral("程序"), m_recoveryTabPage);
    auto* programForm = new QFormLayout();
    ks::ui::ConfigureDetailForm(programForm);
    programForm->addRow(QStringLiteral("重启消息"), m_recoveryRebootMessageEdit);
    programForm->addRow(QStringLiteral("程序"), programRowWidget);
    programForm->addRow(QStringLiteral("命令行参数"), m_recoveryArgumentsEdit);
    programForm->addRow(QStringLiteral("失败计数"), m_recoveryAppendFailCountCheck);
    qobject_cast<QVBoxLayout*>(programCard->layout())->addLayout(programForm);
    body->addWidget(programCard);

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
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(12);

    m_dependencyEditor = new ks::ui::StructuredFieldView(m_dependencyTabPage);

    m_dependencyEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("未选择服务")));
    rootLayout->addWidget(ks::ui::CreateDetailFieldProjection(m_dependencyEditor, m_dependencyTabPage), 1);
}

void ServiceDock::initializeAuditTab()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(m_auditTabPage);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(12);

    m_auditEditor = new ks::ui::StructuredFieldView(m_auditTabPage);

    m_auditEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("未选择服务")));
    rootLayout->addWidget(ks::ui::CreateDetailFieldProjection(m_auditEditor, m_auditTabPage), 1);
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
    const auto& snapshot = m_detailResults[static_cast<int>(DetailKind::Recovery)];
    if (!snapshot || snapshot->revision != m_detailRevision || snapshot->serviceName.compare(entry.serviceNameText, Qt::CaseInsensitive) != 0) return;
    const ServiceRecoverySettings& settings = snapshot->recovery;
    const QString& errorText = snapshot->error;
    const bool queryOk = snapshot->queryOk;

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
    m_recoveryStatusLabel->setText(queryOk ? QString() : errorText);
    m_recoveryFields->setEnabled(true);
    m_recoveryApplyButton->setEnabled(entry.scmRecordPresent);
    markDetailPageRendered(m_recoveryTabPage, entry.serviceNameText);
}

void ServiceDock::populateDependencyTab(const ServiceEntry& entry)
{
    if (m_dependencyEditor != nullptr)
    {
        m_dependencyEditor->setDocument(m_detailResults[static_cast<int>(DetailKind::Dependency)]->document);
    }
    markDetailPageRendered(m_dependencyTabPage, entry.serviceNameText);
}

void ServiceDock::populateAuditTab(const ServiceEntry& entry)
{
    if (m_auditEditor != nullptr)
    {
        m_auditEditor->setDocument(m_detailResults[static_cast<int>(DetailKind::Audit)]->document);
    }
    markDetailPageRendered(m_auditTabPage, entry.serviceNameText);
}

void ServiceDock::updateDetailViewsFromSelection()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    const bool hasSelection = selectedIndex >= 0 && selectedIndex < static_cast<int>(m_serviceList.size());
    QTimer* const detailRenderTimer = detailRenderTimerOf(this);

    ++m_detailRevision;
    m_detailPendingReceipt = false;
    m_detailOperation->cancel();
    for (auto& snapshot : m_detailResults) snapshot.reset();
    m_recoveryFields->setEnabled(false);
    m_recoveryStatusLabel->setText(hasSelection ? ks::i18n::sourceText(QStringLiteral("服务详情尚未加载，请切换到对应页签。")) : QString());
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

        if (m_dependencyEditor != nullptr) { m_dependencyEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("未选择服务"))); }
        if (m_auditEditor != nullptr) { m_auditEditor->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("未选择服务"))); }

        // 没有选中项时清空渲染标记并撤销待执行的延迟渲染。
        markDetailPageRendered(m_recoveryTabPage, QString());
        markDetailPageRendered(m_dependencyTabPage, QString());
        markDetailPageRendered(m_auditTabPage, QString());
        if (detailRenderTimer != nullptr)
        {
            detailRenderTimer->stop();
        }
    }
    else
    {
        const ServiceEntry& entry = m_serviceList[static_cast<std::size_t>(selectedIndex)];
        // 常规页与登录页完全来自内存缓存，没有 SCM 往返，继续同步回填。
        populateGeneralTab(entry);
        populateLogonTab(entry);

        // 恢复/依存关系/审计三页每次都要重新查 SCM：
        // 先整体作废渲染标记（缓存数据可能已被刷新覆盖），
        // 再交给单发定时器去抖后只渲染当前可见的那一页。
        markDetailPageRendered(m_recoveryTabPage, QString());
        markDetailPageRendered(m_dependencyTabPage, QString());
        markDetailPageRendered(m_auditTabPage, QString());
        if (detailRenderTimer != nullptr)
        {
            detailRenderTimer->start(kDetailRenderDebounceMs);
        }
    }
    m_detailUiSyncInProgress = false;

    refreshGeneralTabUiState();
    refreshLogonTabUiState();
    refreshRecoveryTabUiState();

    const bool enableApplyButtons = hasSelection
        && m_serviceList[static_cast<std::size_t>(selectedIndex)].scmRecordPresent;
    if (m_generalApplyButton != nullptr) { m_generalApplyButton->setEnabled(enableApplyButtons); }
    if (m_generalReloadButton != nullptr) { m_generalReloadButton->setEnabled(enableApplyButtons); }
    if (m_logonApplyButton != nullptr) { m_logonApplyButton->setEnabled(enableApplyButtons); }
    if (m_logonReloadButton != nullptr) { m_logonReloadButton->setEnabled(enableApplyButtons); }
    if (m_recoveryApplyButton != nullptr) { m_recoveryApplyButton->setEnabled(false); }
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

void ServiceDock::setDetailUnavailable(const DetailKind kind, const QString& text)
{
    if (kind == DetailKind::Recovery)
    {
        m_recoveryStatusLabel->setText(text);
        m_recoveryFields->setEnabled(false);
        m_recoveryApplyButton->setEnabled(false);
    }
    else if (kind == DetailKind::Dependency) m_dependencyEditor->setDocument(ks::ui::FieldDocument{}.note(text));
    else m_auditEditor->setDocument(ks::ui::FieldDocument{}.note(text));
}

void ServiceDock::reconcileDetailReceipt()
{
    if (!m_detailPendingReceipt || m_detailOperation->isInFlight()) return;
    m_detailPendingReceipt = false;
    if (isVisible() && m_detailPendingRevision == m_detailRevision)
        setDetailUnavailable(m_detailPendingKind, ks::i18n::sourceText(QStringLiteral("服务详情未返回有效结果，请重载后重试。")));
}

void ServiceDock::requestCurrentDetail()
{
    if (!isVisible()) return;
    const int index = findServiceIndexByName(selectedServiceName());
    if (index < 0 || index >= static_cast<int>(m_serviceList.size())) return;
    const ServiceEntry entry = m_serviceList[static_cast<std::size_t>(index)];
    const QWidget* page = m_detailTabWidget->currentWidget();
    DetailKind kind;
    if (page == m_recoveryTabPage) kind = DetailKind::Recovery;
    else if (page == m_dependencyTabPage) kind = DetailKind::Dependency;
    else if (page == m_auditTabPage) kind = DetailKind::Audit;
    else return;
    const auto& cached = m_detailResults[static_cast<int>(kind)];
    if (cached && cached->revision == m_detailRevision)
    {
        if (!isDetailPageRenderedFor(page, entry.serviceNameText)) applyCurrentDetail(*cached);
        return;
    }
    if (m_detailPendingReceipt && m_detailPendingRevision == m_detailRevision && m_detailPendingKind == kind && m_detailOperation->isInFlight()) return;
    reconcileDetailReceipt();
    struct Request {
        ServiceEntry entry;
        QHash<QString, DWORD> states;
        std::uint64_t revision;
        DetailKind kind;
        int visibleCount;
        std::shared_ptr<std::atomic_bool> stopping;
        std::shared_ptr<ks::ui::AsyncCleanupRegistry::Lease> lease;
    } request{entry, m_detailServiceStates, m_detailRevision, kind, m_visibleServiceCount};
    request.stopping = std::make_shared<std::atomic_bool>(false);
    request.lease = ks::ui::AsyncCleanupRegistry::global().acquire([flag = request.stopping]() { flag->store(true); });
    if (!request.lease)
    {
        setDetailUnavailable(kind, ks::i18n::sourceText(QStringLiteral("宿主正在关闭，服务详情未提交。")));
        return;
    }
    m_detailPendingRevision = m_detailRevision;
    m_detailPendingKind = kind;
    m_detailPendingReceipt = true;
    setDetailUnavailable(kind, ks::i18n::sourceText(QStringLiteral("正在读取服务详情...")));
    const auto ticket = m_detailOperation->submit<Request, ServiceDetailResult>(std::move(request),
        [](const Request& input, const ks::ui::AsyncOperationToken& token) {
            ServiceDetailResult result;
            result.serviceName = input.entry.serviceNameText;
            result.revision = input.revision;
            result.kind = input.kind;
            const auto shouldStop = [&]() { return token.isCanceled() || input.stopping->load(); };
            if (shouldStop()) { result.stopped = true; return result; }
            if (input.kind == DetailKind::Recovery)
                result.queryOk = queryServiceFailureSettings(result.serviceName, &result.recovery, &result.error);
            else if (input.kind == DetailKind::Dependency)
                result.document = buildDependencyDetailText(input.entry, input.states, shouldStop);
            else result.document = buildAuditTabText(input.entry, input.visibleCount, shouldStop);
            result.stopped = shouldStop();
            return result;
        },
        [this](const ServiceDetailResult& result, std::uint64_t) {
            m_detailPendingReceipt = false;
            if (result.revision != m_detailRevision || result.serviceName.compare(selectedServiceName(), Qt::CaseInsensitive) != 0) return true;
            if (result.stopped)
            {
                if (isVisible()) setDetailUnavailable(result.kind, ks::i18n::sourceText(QStringLiteral("服务详情读取已停止，请重载后重试。")));
                return true;
            }
            m_detailResults[static_cast<int>(result.kind)] = std::make_shared<ServiceDetailResult>(result);
            if (isVisible()) applyCurrentDetail(result);
            return true;
        },
        [this](std::exception_ptr) {
            m_detailPendingReceipt = false;
            if (isVisible()) setDetailUnavailable(m_detailPendingKind, ks::i18n::sourceText(QStringLiteral("服务详情未返回有效结果，请重载后重试。")));
        });
    if (!ticket) reconcileDetailReceipt();
}

void ServiceDock::applyCurrentDetail(const ServiceDetailResult& result)
{
    if (result.revision != m_detailRevision) return;
    const int index = findServiceIndexByName(selectedServiceName());
    if (index < 0 || index >= static_cast<int>(m_serviceList.size())) return;
    const ServiceEntry& entry = m_serviceList[static_cast<std::size_t>(index)];
    if (result.serviceName.compare(entry.serviceNameText, Qt::CaseInsensitive) != 0) return;
    const bool previous = m_detailUiSyncInProgress;
    m_detailUiSyncInProgress = true;
    if (result.kind == DetailKind::Recovery && m_detailTabWidget->currentWidget() == m_recoveryTabPage) populateRecoveryTab(entry);
    else if (result.kind == DetailKind::Dependency && m_detailTabWidget->currentWidget() == m_dependencyTabPage) populateDependencyTab(entry);
    else if (result.kind == DetailKind::Audit && m_detailTabWidget->currentWidget() == m_auditTabPage) populateAuditTab(entry);
    m_detailUiSyncInProgress = previous;
}
