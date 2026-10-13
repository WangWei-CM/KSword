#include "../Framework.h"
#include "SettingsDock.h"
#include "SettingsDock.Layout.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ThemeAccentIcon.h"
#include "../UI/ThemeStatusRole.h"
#include "../theme.h"
#include "../../../shared/usermode/KswordMcpProtocol.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>

namespace
{
    // 返回当前安装位置对应的 sidecar；只检查约定落点，不递归扫描或运行脚本。
    QString DefaultMcpSidecarPath()
    {
        const QDir executableDir(QCoreApplication::applicationDirPath()); // 当前 exe 所在目录。
        const QStringList candidates{ // 发布目录与仓库标准 Release 目录的相对位置。
            executableDir.filePath(QStringLiteral("tools/ksword_mcp/server.py")),
            executableDir.filePath(QStringLiteral("../../../tools/ksword_mcp/server.py"))};
        for (const QString& candidate : candidates)
        {
            const QFileInfo script(candidate); // 单个约定路径的存在性与文件身份。
            if (script.isFile())
            {
                return QDir::cleanPath(script.absoluteFilePath());
            }
        }
        return QString();
    }

    // 创建可换行说明；输入稳定语言键与中文回退，返回由父控件管理的标签。
    QLabel* McpHint(QWidget* parent, const QString& key, const QString& text)
    {
        auto* label = ks::settings::ui::Label(parent, key, text); // 自适应说明正文。
        label->setProperty("ksword_settings_hint", true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        return label;
    }

    // 创建标准 SVG 图标动作；明确实底与语义角色，提示绑定同时用于可访问名称。
    QPushButton* McpIconButton(QWidget* parent, const QString& name, const QString& icon,
        const QString& key, const QString& hint, ks::ui::FlatButtonTone tone)
    {
        auto* button = new QPushButton(parent); // 保留 Qt 原生按钮动作与焦点语义。
        button->setObjectName(name);
        KswordTheme::ApplyStandardIconButtonMetrics(button);
        ks::ui::ApplyFlatButtonTheme(button, tone, ks::ui::FlatButtonAppearance::Solid);
        button->setIcon(ks::ui::MakeThemeButtonAccentIcon(QIcon(icon), QColor(), button));
        ks::i18n::LanguageManager::instance().bindToolTip(button, key, hint);
        button->setAccessibleName(ks::i18n::sourceText(hint));
        return button;
    }
}

// 从顶部“设置 → MCP”进入；设置正文独立滚动，配置预览保留自己的文本滚动。
void SettingsDock::initializeMcpTab()
{
    m_mcpTab = new QWidget(m_tabWidget);
    m_mcpTab->setObjectName(QStringLiteral("settings_mcp_page"));
    auto* root = new QVBoxLayout(m_mcpTab); // 设置、预览和说明各自拥有明确高度。
    ks::ui::StyleSecondaryContentLayout(root);
    root->setContentsMargins(6, 6, 6, 6); // 页根等边留白只由这一层负责。
    auto& language = ks::i18n::LanguageManager::instance(); // 共享热切换语言绑定。

    auto* service = new QGroupBox(m_mcpTab); // 当前服务与下次启动偏好。
    language.bindText(service, QStringLiteral("settings.mcp.service"), QStringLiteral("本地 MCP 服务"));
    auto* serviceLayout = new QVBoxLayout(service);
    serviceLayout->addWidget(McpHint(service, QStringLiteral("settings.mcp.service_hint"),
        QStringLiteral("允许 MCP 客户端读取界面并触发操作。启停立即影响本次会话；自动启动选项点击“应用”后保存。")));
    auto* serviceForm = ks::settings::ui::Form(serviceLayout, 128); // 自然高度状态行。
    m_mcpStatusLabel = new QLabel(service);
    m_mcpStatusLabel->setObjectName(QStringLiteral("mcp_service_status"));
    serviceForm->addRow(ks::settings::ui::Label(service, QStringLiteral("settings.mcp.status"),
        QStringLiteral("当前状态")), m_mcpStatusLabel);
    auto* pid = new QLabel(QString::number(QCoreApplication::applicationPid()), service); // 本次进程 PID。
    pid->setTextInteractionFlags(Qt::TextSelectableByMouse);
    serviceForm->addRow(ks::settings::ui::Label(service, QStringLiteral("settings.mcp.pid"),
        QStringLiteral("进程 PID")), pid);
    m_mcpEndpointLabel = new QLabel(service);
    m_mcpEndpointLabel->setWordWrap(true);
    m_mcpEndpointLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mcpEndpointLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    serviceForm->addRow(ks::settings::ui::Label(service, QStringLiteral("settings.mcp.endpoint"),
        QStringLiteral("本地管道")), m_mcpEndpointLabel);

    // 本次动作不会改写待保存偏好；停止会断开连接并取消尚未派发的界面动作。
    auto* sessionRow = new QHBoxLayout(); // 同排等高的启停动作。
    m_mcpStartButton = McpIconButton(service, QStringLiteral("mcp_service_start"),
        QStringLiteral(":/Icon/process_start.svg"), QStringLiteral("settings.mcp.start"),
        QStringLiteral("本次启用 MCP 服务"), ks::ui::FlatButtonTone::Accent);
    m_mcpStopButton = McpIconButton(service, QStringLiteral("mcp_service_stop"),
        QStringLiteral(":/Icon/process_stop.svg"), QStringLiteral("settings.mcp.stop"),
        QStringLiteral("停止 MCP 服务并断开当前连接"), ks::ui::FlatButtonTone::Neutral);
    sessionRow->addWidget(m_mcpStartButton);
    sessionRow->addWidget(m_mcpStopButton);
    sessionRow->addStretch(1);
    serviceForm->addRow(ks::settings::ui::Label(service, QStringLiteral("settings.mcp.session"),
        QStringLiteral("本次会话")), ks::settings::ui::ControlRow(sessionRow, service));
    m_mcpAutoStartCheckBox = new QCheckBox(service);
    m_mcpAutoStartCheckBox->setObjectName(QStringLiteral("mcp_auto_start"));
    ks::settings::ui::ToggleRow(serviceForm, m_mcpAutoStartCheckBox,
        QStringLiteral("settings.mcp.auto_start"), QStringLiteral("随程序启动"));
    language.bindToolTip(m_mcpAutoStartCheckBox, QStringLiteral("settings.mcp.auto_start_hint"),
        QStringLiteral("点击“应用”后，下次启动自动启用本地 MCP 服务；默认关闭。"));
    m_mcpErrorLabel = McpHint(service, QStringLiteral("settings.mcp.error"), QString());
    m_mcpErrorLabel->setTextFormat(Qt::PlainText);
    m_mcpErrorLabel->setProperty("ksword_settings_hint", false); // 错误色由状态角色唯一拥有。
    ks::ui::ApplyStatusRole(m_mcpErrorLabel, ks::ui::StatusRole::Error);
    serviceLayout->addWidget(m_mcpErrorLabel);
    ks::settings::ui::Section(service);

    auto* client = new QGroupBox(m_mcpTab); // 路径只用于客户端配置预览。
    language.bindText(client, QStringLiteral("settings.mcp.client"), QStringLiteral("客户端配置"));
    auto* clientLayout = new QVBoxLayout(client);
    clientLayout->addWidget(McpHint(client, QStringLiteral("settings.mcp.client_hint"),
        QStringLiteral("使用已安装 MCP 依赖的 Python。配置包含当前 PID，程序重启后需重新复制。")));
    auto* clientForm = ks::settings::ui::Form(clientLayout, 112); // 路径输入统一标签与行高。
    m_mcpPythonCommandEdit = new QLineEdit(client);
    m_mcpPythonCommandEdit->setObjectName(QStringLiteral("mcp_python_command"));
    clientForm->addRow(ks::settings::ui::Label(client, QStringLiteral("settings.mcp.python"),
        QStringLiteral("Python 命令")), m_mcpPythonCommandEdit);
    language.bindToolTip(m_mcpPythonCommandEdit, QStringLiteral("settings.mcp.python_hint"),
        QStringLiteral("填写 python 或 Python 可执行文件完整路径，不包含额外参数。"));
    m_mcpSidecarPathEdit = new QLineEdit(client);
    m_mcpSidecarPathEdit->setObjectName(QStringLiteral("mcp_sidecar_script_path"));
    language.bindPlaceholder(m_mcpSidecarPathEdit, QStringLiteral("settings.mcp.script_placeholder"),
        QStringLiteral("选择 tools/ksword_mcp/server.py"));
    auto* pathRow = new QHBoxLayout(); // 路径伸缩，浏览动作固定为标准图标按钮。
    auto* browse = McpIconButton(client, QStringLiteral("mcp_script_browse"),
        QStringLiteral(":/Icon/settings_background_browse.svg"), QStringLiteral("settings.mcp.browse"),
        QStringLiteral("选择 MCP sidecar 脚本"), ks::ui::FlatButtonTone::Neutral);
    pathRow->addWidget(m_mcpSidecarPathEdit, 1);
    pathRow->addWidget(browse);
    clientForm->addRow(ks::settings::ui::Label(client, QStringLiteral("settings.mcp.script"),
        QStringLiteral("Sidecar 脚本")), ks::settings::ui::ControlRow(pathRow, client));
    m_mcpConfigFormatCombo = new QComboBox(client);
    m_mcpConfigFormatCombo->setObjectName(QStringLiteral("mcp_config_format"));
    m_mcpConfigFormatCombo->addItem(QStringLiteral("JSON"));
    m_mcpConfigFormatCombo->addItem(QStringLiteral("TOML (Codex)"));
    clientForm->addRow(ks::settings::ui::Label(client, QStringLiteral("settings.mcp.format"),
        QStringLiteral("配置格式")), m_mcpConfigFormatCombo);
    ks::settings::ui::Section(client);

    // 共用自适应双栏；窄窗按服务、客户端顺序纵排，参数不会靠水平滚动隐藏。
    auto* columns = ks::ui::CreateSecondaryColumns(service, client, m_mcpTab, 900, 1, 1);
    auto* settingsScroll = ks::ui::CreateSecondaryFormScroll(columns, m_mcpTab); // 自然高参数滚动区。
    root->addWidget(settingsScroll, 0);
    // 配置与日志共用剩余高度，参数/会话状态始终可见，文本各自承担内部滚动。
    auto* outputTabs = new QTabWidget(m_mcpTab); // 本页内部输出导航，不另造详情外框。
    outputTabs->setObjectName(QStringLiteral("mcp_output_tabs"));
    ks::ui::StyleSecondaryTabs(outputTabs);
    root->addWidget(outputTabs, 1);
    auto* configPage = new QWidget(outputTabs); // 原配置预览保留自身控件与文档身份。
    auto* configLayout = new QVBoxLayout(configPage);
    configLayout->setContentsMargins(0, 0, 0, 0);
    configLayout->setSpacing(8);
    auto* previewRow = new QHBoxLayout(); // 配置标题与复制动作固定在预览上方。
    previewRow->addWidget(ks::settings::ui::Label(m_mcpTab, QStringLiteral("settings.mcp.preview"),
        QStringLiteral("配置预览")), 1);
    m_mcpCopyButton = McpIconButton(m_mcpTab, QStringLiteral("mcp_config_copy"),
        QStringLiteral(":/Icon/codeeditor_copy.svg"), QStringLiteral("settings.mcp.copy"),
        QStringLiteral("复制完整 MCP 客户端配置"), ks::ui::FlatButtonTone::Neutral);
    previewRow->addWidget(m_mcpCopyButton);
    ks::ui::NormalizeToolbarRow(previewRow);
    configLayout->addLayout(previewRow);
    m_mcpConfigPreview = new CodeEditorWidget(m_mcpTab);
    m_mcpConfigPreview->setObjectName(QStringLiteral("mcp_config_preview"));
    m_mcpConfigPreview->setReadOnly(true);
    m_mcpConfigPreview->setWordWrapEnabled(true);
    m_mcpConfigPreview->setMinimumHeight(180);
    configLayout->addWidget(m_mcpConfigPreview, 1);
    m_mcpConfigHintLabel = McpHint(m_mcpTab, QStringLiteral("settings.mcp.connection_hint"), QString());
    configLayout->addWidget(m_mcpConfigHintLabel);
    outputTabs->addTab(configPage, QStringLiteral("配置预览"));
    language.bindTab(outputTabs, configPage, QStringLiteral("settings.mcp.preview"), QStringLiteral("配置预览"));

    auto* logPage = new QWidget(outputTabs); // 操作日志使用同一个规范文档，复制与导出保持完整。
    auto* logLayout = new QVBoxLayout(logPage);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->setSpacing(8);
    auto* logToolbar = new QHBoxLayout(); // 说明与清空动作固定，正文独立滚动。
    logToolbar->addWidget(McpHint(logPage, QStringLiteral("settings.mcp.log_hint"),
        QStringLiteral("保留最近1000条。记录调用派发与错误，不代表业务完成；不记录输入值或读取结果。")), 1);
    m_mcpClearLogButton = McpIconButton(logPage, QStringLiteral("mcp_operation_log_clear"),
        QStringLiteral(":/Icon/log_clear.svg"), QStringLiteral("settings.mcp.log_clear"),
        QStringLiteral("清空本次 MCP 操作日志"), ks::ui::FlatButtonTone::Neutral);
    m_mcpClearLogButton->setEnabled(false);
    logToolbar->addWidget(m_mcpClearLogButton);
    ks::ui::NormalizeToolbarRow(logToolbar);
    logLayout->addLayout(logToolbar);
    m_mcpLogViewer = new CodeEditorWidget(logPage);
    m_mcpLogViewer->setObjectName(QStringLiteral("mcp_operation_log"));
    m_mcpLogViewer->setReadOnly(true);
    m_mcpLogViewer->setWordWrapEnabled(true);
    m_mcpLogViewer->setMaximumBlockCount(ks::mcp::MaxActivityLogLines);
    m_mcpLogViewer->setMinimumHeight(180);
    logLayout->addWidget(m_mcpLogViewer, 1);
    outputTabs->addTab(logPage, QStringLiteral("操作日志"));
    language.bindTab(outputTabs, logPage, QStringLiteral("settings.mcp.log"), QStringLiteral("操作日志"));
    connect(m_mcpClearLogButton, &QPushButton::clicked, this, &SettingsDock::mcpOperationLogClearRequested);
    // 安装引导作为独立文本输出页，继续复用共享导航、编辑器与完整复制语义。
    auto* installPage = new QWidget(outputTabs); // 不执行安装，只提供用户交给 agent 的提示词。
    auto* installLayout = new QVBoxLayout(installPage);
    installLayout->setContentsMargins(0, 0, 0, 0);
    installLayout->setSpacing(8);
    auto* installToolbar = new QHBoxLayout(); // 说明与复制动作固定在正文上方。
    installToolbar->addWidget(McpHint(installPage, QStringLiteral("settings.mcp.install_hint"),
        QStringLiteral("复制后发给 agent：安装前会询问是否下载 KSword、未运行时的策略，以及自动启动时的权限。")), 1);
    m_mcpInstallPromptCopyButton = McpIconButton(installPage, QStringLiteral("mcp_install_prompt_copy"),
        QStringLiteral(":/Icon/codeeditor_copy.svg"), QStringLiteral("settings.mcp.install_copy"),
        QStringLiteral("复制 MCP 一键安装提示词"), ks::ui::FlatButtonTone::Neutral);
    installToolbar->addWidget(m_mcpInstallPromptCopyButton);
    ks::ui::NormalizeToolbarRow(installToolbar);
    installLayout->addLayout(installToolbar);
    m_mcpInstallPromptViewer = new CodeEditorWidget(installPage);
    m_mcpInstallPromptViewer->setObjectName(QStringLiteral("mcp_install_prompt"));
    m_mcpInstallPromptViewer->setReadOnly(true);
    m_mcpInstallPromptViewer->setWordWrapEnabled(true);
    m_mcpInstallPromptViewer->setMinimumHeight(180);
    installLayout->addWidget(m_mcpInstallPromptViewer, 1);
    outputTabs->addTab(installPage, QStringLiteral("安装提示词"));
    language.bindTab(outputTabs, installPage, QStringLiteral("settings.mcp.install_tab"), QStringLiteral("安装提示词"));
    connect(m_mcpInstallPromptCopyButton, &QPushButton::clicked, this, [this]()
    {
        QApplication::clipboard()->setText(m_mcpInstallPromptViewer->text()); // 只复制规范正文。
    });
    refreshMcpInstallPrompt();
    m_tabWidget->addTab(m_mcpTab, QStringLiteral("MCP"));
    language.bindTab(m_tabWidget, m_mcpTab, QStringLiteral("settings.tab.mcp"), QStringLiteral("MCP"));

    // 所有持久化编辑复用“应用/取消”；预览格式与本次启停不产生待保存改动。
    connect(m_mcpAutoStartCheckBox, &QCheckBox::toggled, this, [this]()
    {
        markPendingChanges(QStringLiteral("MCP 启动偏好变化"));
    });
    const auto pathChanged = [this]() // 两个路径字段共用预览与待保存更新。
    {
        refreshMcpClientConfiguration();
        markPendingChanges(QStringLiteral("MCP 客户端路径变化"));
    };
    connect(m_mcpPythonCommandEdit, &QLineEdit::textChanged, this, pathChanged);
    connect(m_mcpSidecarPathEdit, &QLineEdit::textChanged, this, pathChanged);
    connect(m_mcpConfigFormatCombo, &QComboBox::currentIndexChanged,
        this, &SettingsDock::refreshMcpClientConfiguration);
    connect(m_mcpStartButton, &QPushButton::clicked, this, [this]()
    {
        emit mcpServiceEnabledRequested(true);
    });
    connect(m_mcpStopButton, &QPushButton::clicked, this, [this]()
    {
        emit mcpServiceEnabledRequested(false);
    });
    connect(browse, &QPushButton::clicked, this, [this]()
    {
        const QString path = QFileDialog::getOpenFileName(this, // 原生选择器，不套详情框架。
            ks::i18n::sourceText(QStringLiteral("选择 MCP sidecar 脚本")),
            m_mcpSidecarPathEdit->text(), QStringLiteral("Python (*.py)"));
        if (!path.isEmpty())
        {
            m_mcpSidecarPathEdit->setText(QDir::cleanPath(path));
        }
    });
    connect(m_mcpCopyButton, &QPushButton::clicked, this, [this]()
    {
        QApplication::clipboard()->setText(m_mcpConfigPreview->text()); // 复制唯一完整文档。
    });
    refreshMcpServiceState();
}

// 主窗口为唯一日志源；设置窗口重开时回填最近记录，不清除服务的历史。
void SettingsDock::setMcpOperationLog(const QString& text)
{
    m_mcpLogViewer->setRawText(text);
    m_mcpClearLogButton->setEnabled(!text.isEmpty());
}

// 原文增量追加沿用共享编辑器的尾部跟随规则，用户阅读旧记录时不跳动。
void SettingsDock::appendMcpOperationLog(const QString& line)
{
    m_mcpLogViewer->appendRawText(line, true);
    m_mcpClearLogButton->setEnabled(true);
}

// 收到统一清空通知后删除当前文档，搜索、复制与导出不保留旧镜像。
void SettingsDock::clearMcpOperationLog()
{
    m_mcpLogViewer->clear();
    m_mcpClearLogButton->setEnabled(false);
}

// 资源与发布 README 使用同一文件；完整原文交给语言包翻译，不从页面另造安装步骤。
void SettingsDock::refreshMcpInstallPrompt()
{
    if (!m_mcpInstallPromptViewer)
    {
        return;
    }
    static const QString source = []() // 资源只读一次，不在主题或字段更新时反复查文件。
    {
        QFile prompt(QStringLiteral(":/Data/mcp_agent_install_prompt.txt")); // 随 exe 发布的提示词。
        return prompt.open(QIODevice::ReadOnly)
            ? QString::fromUtf8(prompt.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).trimmed()
            : QString(); // 资源换行统一后与语言包的规范短提示词键一致。
    }();
    const QString text = source.isEmpty()
        ? ks::i18n::sourceText(QStringLiteral("安装提示词暂不可用。")) : ks::i18n::sourceText(source);
    if (m_mcpInstallPromptViewer->text() != text)
    {
        m_mcpInstallPromptViewer->setRawText(text);
    }
    m_mcpInstallPromptCopyButton->setEnabled(!source.isEmpty());
}

// 输入主窗口回填的真实状态；错误和值始终按普通文本显示，不解释为富文本。
void SettingsDock::setMcpServiceState(bool running, const QString& endpoint, const QString& error)
{
    m_mcpServiceRunning = running;
    m_mcpServiceEndpoint = endpoint;
    m_mcpServiceError = error;
    refreshMcpServiceState();
}

// 当前状态由监听器决定，自动启动复选框只描述下次启动偏好。
void SettingsDock::refreshMcpServiceState()
{
    if (!m_mcpStatusLabel)
    {
        return;
    }
    if (m_mcpLogViewer)
    {
        // 状态与语言刷新共用入口，空日志提示不混入原始记录正文。
        m_mcpLogViewer->setPlaceholderText(ks::i18n::sourceText(QStringLiteral("本次会话尚无 MCP 操作记录。")));
    }
    m_mcpStatusLabel->setText(ks::i18n::sourceText(m_mcpServiceRunning
        ? QStringLiteral("运行中") : (m_mcpServiceError.isEmpty()
            ? QStringLiteral("已停止") : QStringLiteral("启动失败"))));
    ks::ui::ApplyStatusRole(m_mcpStatusLabel, m_mcpServiceRunning
        ? ks::ui::StatusRole::Success : (m_mcpServiceError.isEmpty()
            ? ks::ui::StatusRole::Idle : ks::ui::StatusRole::Error));
    const QString endpoint = m_mcpServiceEndpoint.isEmpty() // 尚未启用时也显示本次固定端点。
        ? QStringLiteral("ksword-mcp-") + QString::number(QCoreApplication::applicationPid())
        : m_mcpServiceEndpoint;
    m_mcpEndpointLabel->setText(QStringLiteral("\\\\.\\pipe\\") + endpoint);
    m_mcpErrorLabel->setText(m_mcpServiceError);
    m_mcpErrorLabel->setVisible(!m_mcpServiceError.isEmpty());
    m_mcpStartButton->setEnabled(!m_mcpServiceRunning);
    m_mcpStopButton->setEnabled(m_mcpServiceRunning);
}

// 回填配置时自动发现脚本位置，不执行 Python；已有显式路径优先。
void SettingsDock::applyMcpSettingsToUi(const ks::settings::AppearanceSettings& settings)
{
    m_mcpAutoStartCheckBox->setChecked(settings.mcpAutoStart);
    m_mcpPythonCommandEdit->setText(settings.mcpPythonCommand);
    m_mcpSidecarPathEdit->setText(settings.mcpSidecarScriptPath.isEmpty()
        ? DefaultMcpSidecarPath() : settings.mcpSidecarScriptPath);
    refreshMcpClientConfiguration();
}

// 只收集待应用字段；自动发现路径仍保存为空，以保留安装目录迁移能力。
void SettingsDock::collectMcpSettingsFromUi(ks::settings::AppearanceSettings& settings) const
{
    settings.mcpAutoStart = m_mcpAutoStartCheckBox->isChecked();
    settings.mcpPythonCommand = m_mcpPythonCommandEdit->text().trimmed();
    const QString path = QDir::fromNativeSeparators(m_mcpSidecarPathEdit->text().trimmed()); // 规范路径。
    settings.mcpSidecarScriptPath = path == DefaultMcpSidecarPath() ? QString() : path;
}

// 配置唯一文档来自当前输入和当前 PID；JSON 转义同时用于 TOML 基础字符串。
void SettingsDock::refreshMcpClientConfiguration()
{
    if (!m_mcpConfigPreview)
    {
        return;
    }
    const QString command = m_mcpPythonCommandEdit->text().trimmed(); // 一个命令，无 shell 拼接。
    const QString path = m_mcpSidecarPathEdit->text().trimmed(); // 用户明确输入的脚本位置。
    const QString script = path.isEmpty() ? QString() // 相对路径也按当前位置生成绝对路径。
        : QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    const QJsonArray args{script, QStringLiteral("--pid"),
        QString::number(QCoreApplication::applicationPid())}; // 本次 GUI 的真实进程身份。
    QString configuration; // 预览、复制与编辑器导出的同一规范正文。
    if (m_mcpConfigFormatCombo->currentIndex() == 0)
    {
        const QJsonObject server{{QStringLiteral("command"), command}, {QStringLiteral("args"), args}};
        const QJsonObject document{{QStringLiteral("mcpServers"),
            QJsonObject{{QStringLiteral("ksword"), server}}}};
        configuration = QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Indented));
    }
    else
    {
        const QByteArray quotedCommand = QJsonDocument(QJsonArray{command}) // 保留引号与反斜线转义。
            .toJson(QJsonDocument::Compact).mid(1).chopped(1);
        configuration = QStringLiteral("[mcp_servers.ksword]\ncommand = %1\nargs = %2\n")
            .arg(QString::fromUtf8(quotedCommand),
                QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
    }
    if (m_mcpConfigPreview->text() != configuration)
    {
        m_mcpConfigPreview->setRawText(configuration);
    }
    const bool usable = !command.isEmpty() && QFileInfo(script).isFile(); // 不声称已安装 Python 依赖。
    m_mcpCopyButton->setEnabled(usable);
    m_mcpConfigHintLabel->setText(ks::i18n::sourceText(usable
        ? QStringLiteral("仅限本机、同用户、同会话与匹配权限的客户端。路径点击“应用”后保存；主程序不会自动安装依赖。")
        : QStringLiteral("请填写 Python 命令并选择存在的 sidecar 脚本，再复制配置。")));
    ks::ui::ApplyStatusRole(m_mcpConfigHintLabel,
        usable ? ks::ui::StatusRole::Idle : ks::ui::StatusRole::Warning);
}
