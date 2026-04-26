#include "SettingsDock.h"

#include "../Framework.h"
#include "../theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace
{
    // ToolTip 与图标常量：统一维护设置页按钮文案，避免硬编码分散。
    constexpr const char* IconThemeFollowSystem = ":/Icon/settings_theme_system.svg";
    constexpr const char* IconThemeLight = ":/Icon/settings_theme_light.svg";
    constexpr const char* IconThemeDark = ":/Icon/settings_theme_dark.svg";
    constexpr const char* IconBrowseBackground = ":/Icon/settings_background_browse.svg";
    constexpr const char* IconResetBackground = ":/Icon/settings_background_reset.svg";

    // formatScaleFactorText 作用：
    // - 把缩放因子格式化为两位小数字符串；
    // - 统一输入框显示格式，避免精度噪声。
    // 调用方式：配置回填与保存后 UI 刷新时调用。
    // 入参 scaleFactor：已校正的缩放因子。
    // 返回：例如 "1.00" 的文本。
    QString formatScaleFactorText(const double scaleFactor)
    {
        return QString::number(ks::settings::normalizeWindowScaleFactor(scaleFactor), 'f', 2);
    }
}

SettingsDock::SettingsDock(QWidget* parent)
    : QWidget(parent)
{
    // 构造日志事件：用于追踪“设置页初始化”整个调用链。
    kLogEvent settingsInitEvent;
    info << settingsInitEvent << "[SettingsDock] 开始初始化设置页 UI。" << eol;

    initializeUi();
    initializeAppearanceTab();
    loadSettingsFromJson();

    info << settingsInitEvent << "[SettingsDock] 设置页初始化完成，界面与启动配置已加载。" << eol;
}

ks::settings::AppearanceSettings SettingsDock::currentAppearanceSettings() const
{
    return m_currentAppearanceSettings;
}

void SettingsDock::initializeUi()
{
    // rootLayout 作用：SettingsDock 根布局，承载 Tab 控件。
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    // m_tabWidget 作用：设置页签容器，后续可扩展更多标签页。
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabPosition(QTabWidget::North);
    rootLayout->addWidget(m_tabWidget);

    setLayout(rootLayout);
}

void SettingsDock::initializeAppearanceTab()
{
    // m_appearanceTab 作用：承载“外观与启动”相关控件。
    m_appearanceTab = new QWidget(m_tabWidget);
    QVBoxLayout* appearanceRootLayout = new QVBoxLayout(m_appearanceTab);
    appearanceRootLayout->setContentsMargins(8, 8, 8, 8);
    appearanceRootLayout->setSpacing(12);

    // ===== 主题模式分组 =====
    QGroupBox* themeGroupBox = new QGroupBox(QStringLiteral("主题模式"), m_appearanceTab);
    QVBoxLayout* themeLayout = new QVBoxLayout(themeGroupBox);
    themeLayout->setSpacing(8);

    QLabel* themeHintLabel = new QLabel(QStringLiteral("可选择跟随系统、浅色或深色主题。"), themeGroupBox);
    themeLayout->addWidget(themeHintLabel);

    QHBoxLayout* themeButtonLayout = new QHBoxLayout();
    themeButtonLayout->setSpacing(10);
    m_themeButtonGroup = new QButtonGroup(themeGroupBox);
    m_themeButtonGroup->setExclusive(true);

    // m_followSystemButton 作用：主题跟随系统按钮（图标 + 悬停说明）。
    m_followSystemButton = new QToolButton(themeGroupBox);
    m_followSystemButton->setIcon(QIcon(QString::fromUtf8(IconThemeFollowSystem)));
    m_followSystemButton->setCheckable(true);
    m_followSystemButton->setIconSize(QSize(20, 20));
    m_followSystemButton->setFixedSize(36, 36);
    m_followSystemButton->setToolTip(QStringLiteral("跟随系统主题（Windows 深浅切换时自动同步）"));

    // m_lightModeButton 作用：强制浅色主题按钮（图标 + 悬停说明）。
    m_lightModeButton = new QToolButton(themeGroupBox);
    m_lightModeButton->setIcon(QIcon(QString::fromUtf8(IconThemeLight)));
    m_lightModeButton->setCheckable(true);
    m_lightModeButton->setIconSize(QSize(20, 20));
    m_lightModeButton->setFixedSize(36, 36);
    m_lightModeButton->setToolTip(QStringLiteral("强制浅色模式（白底深色字）"));

    // m_darkModeButton 作用：强制深色主题按钮（图标 + 悬停说明）。
    m_darkModeButton = new QToolButton(themeGroupBox);
    m_darkModeButton->setIcon(QIcon(QString::fromUtf8(IconThemeDark)));
    m_darkModeButton->setCheckable(true);
    m_darkModeButton->setIconSize(QSize(20, 20));
    m_darkModeButton->setFixedSize(36, 36);
    m_darkModeButton->setToolTip(QStringLiteral("强制深色模式（黑底白字）"));

    m_themeButtonGroup->addButton(m_followSystemButton, static_cast<int>(ks::settings::ThemeMode::FollowSystem));
    m_themeButtonGroup->addButton(m_lightModeButton, static_cast<int>(ks::settings::ThemeMode::Light));
    m_themeButtonGroup->addButton(m_darkModeButton, static_cast<int>(ks::settings::ThemeMode::Dark));

    themeButtonLayout->addWidget(m_followSystemButton);
    themeButtonLayout->addWidget(m_lightModeButton);
    themeButtonLayout->addWidget(m_darkModeButton);
    themeButtonLayout->addStretch();
    themeLayout->addLayout(themeButtonLayout);
    appearanceRootLayout->addWidget(themeGroupBox);

    // ===== 背景图分组 =====
    QGroupBox* backgroundGroupBox = new QGroupBox(QStringLiteral("窗口背景图"), m_appearanceTab);
    QVBoxLayout* backgroundLayout = new QVBoxLayout(backgroundGroupBox);
    backgroundLayout->setSpacing(8);

    QLabel* pathHintLabel = new QLabel(
        QStringLiteral("默认路径：style/ksword_background.png，可手动选择 PNG/JPG/BMP。"),
        backgroundGroupBox);
    pathHintLabel->setWordWrap(true);
    backgroundLayout->addWidget(pathHintLabel);

    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->setSpacing(6);

    // m_backgroundPathEdit 作用：用户输入背景图路径文本。
    m_backgroundPathEdit = new QLineEdit(backgroundGroupBox);
    m_backgroundPathEdit->setPlaceholderText(QStringLiteral("style/ksword_background.png"));
    pathLayout->addWidget(m_backgroundPathEdit, 1);

    // m_browseBackgroundButton 作用：打开文件对话框选择背景图。
    m_browseBackgroundButton = new QToolButton(backgroundGroupBox);
    m_browseBackgroundButton->setIcon(QIcon(QString::fromUtf8(IconBrowseBackground)));
    m_browseBackgroundButton->setIconSize(QSize(18, 18));
    m_browseBackgroundButton->setFixedSize(34, 30);
    m_browseBackgroundButton->setToolTip(QStringLiteral("浏览背景图文件"));
    pathLayout->addWidget(m_browseBackgroundButton);

    // m_resetBackgroundButton 作用：恢复默认背景路径。
    m_resetBackgroundButton = new QToolButton(backgroundGroupBox);
    m_resetBackgroundButton->setIcon(QIcon(QString::fromUtf8(IconResetBackground)));
    m_resetBackgroundButton->setIconSize(QSize(18, 18));
    m_resetBackgroundButton->setFixedSize(34, 30);
    m_resetBackgroundButton->setToolTip(QStringLiteral("恢复默认背景路径"));
    pathLayout->addWidget(m_resetBackgroundButton);

    backgroundLayout->addLayout(pathLayout);

    QLabel* opacityHintLabel = new QLabel(QStringLiteral("背景图透明度（0% 仅纯色背景，100% 仅背景图）"), backgroundGroupBox);
    backgroundLayout->addWidget(opacityHintLabel);

    QHBoxLayout* opacityLayout = new QHBoxLayout();
    opacityLayout->setSpacing(6);

    // m_backgroundOpacitySlider 作用：控制背景图透明度数值。
    m_backgroundOpacitySlider = new QSlider(Qt::Horizontal, backgroundGroupBox);
    m_backgroundOpacitySlider->setRange(0, 100);
    m_backgroundOpacitySlider->setSingleStep(1);
    m_backgroundOpacitySlider->setPageStep(5);
    m_backgroundOpacitySlider->setToolTip(QStringLiteral("拖动调整背景图透明度"));
    opacityLayout->addWidget(m_backgroundOpacitySlider, 1);

    // m_backgroundOpacityValueLabel 作用：展示当前透明度百分比。
    m_backgroundOpacityValueLabel = new QLabel(QStringLiteral("35%"), backgroundGroupBox);
    m_backgroundOpacityValueLabel->setMinimumWidth(48);
    opacityLayout->addWidget(m_backgroundOpacityValueLabel);

    backgroundLayout->addLayout(opacityLayout);
    appearanceRootLayout->addWidget(backgroundGroupBox);

    // ===== 启动行为分组 =====
    QGroupBox* startupGroupBox = new QGroupBox(QStringLiteral("启动行为"), m_appearanceTab);
    QVBoxLayout* startupLayout = new QVBoxLayout(startupGroupBox);
    startupLayout->setSpacing(8);

    QLabel* startupHintLabel = new QLabel(
        QStringLiteral("设置应用下次启动时的默认页签、窗口显示方式与权限申请行为。"),
        startupGroupBox);
    startupHintLabel->setWordWrap(true);
    startupLayout->addWidget(startupHintLabel);

    QHBoxLayout* startupSelectorLayout = new QHBoxLayout();
    startupSelectorLayout->setSpacing(6);
    startupSelectorLayout->addWidget(new QLabel(QStringLiteral("默认页签"), startupGroupBox), 0);

    // m_startupDefaultTabCombo 作用：维护“启动默认页签 key”与“中文显示名”的映射。
    m_startupDefaultTabCombo = new QComboBox(startupGroupBox);
    m_startupDefaultTabCombo->setToolTip(QStringLiteral("选择应用下次启动时默认展示的主标签页"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("欢迎"), QStringLiteral("welcome"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("进程"), QStringLiteral("process"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("网络"), QStringLiteral("network"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("内存"), QStringLiteral("memory"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("文件"), QStringLiteral("file"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("驱动"), QStringLiteral("driver"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("内核"), QStringLiteral("kernel"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("监控"), QStringLiteral("monitor"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("硬件"), QStringLiteral("hardware"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("权限"), QStringLiteral("privilege"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("设置"), QStringLiteral("settings"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("窗口"), QStringLiteral("window"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("注册表"), QStringLiteral("registry"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("启动项"), QStringLiteral("startup"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("服务"), QStringLiteral("service"));
    m_startupDefaultTabCombo->addItem(QStringLiteral("杂项"), QStringLiteral("misc"));
    startupSelectorLayout->addWidget(m_startupDefaultTabCombo, 1);
    startupLayout->addLayout(startupSelectorLayout);

    // m_startupMaximizedCheckBox 作用：控制“下次启动时是否直接最大化显示”。
    m_startupMaximizedCheckBox = new QCheckBox(QStringLiteral("启动时最大化"), startupGroupBox);
    m_startupMaximizedCheckBox->setToolTip(QStringLiteral("下次启动主窗口时直接以最大化状态显示"));
    startupLayout->addWidget(m_startupMaximizedCheckBox);

    // m_startupAutoAdminCheckBox 作用：控制“启动图出现前是否先尝试 UAC 提权”。
    m_startupAutoAdminCheckBox = new QCheckBox(QStringLiteral("启动时自动请求管理员权限"), startupGroupBox);
    m_startupAutoAdminCheckBox->setToolTip(
        QStringLiteral("下次启动时会在启动画面出现前先尝试管理员提权，失败则继续普通权限启动"));
    startupLayout->addWidget(m_startupAutoAdminCheckBox);

    // m_unlockerShellContextMenuCheckBox 作用：控制是否启用系统右键“文件解锁器”菜单。
    m_unlockerShellContextMenuCheckBox = new QCheckBox(QStringLiteral("启用系统右键“文件解锁器”菜单"), startupGroupBox);
    m_unlockerShellContextMenuCheckBox->setToolTip(
        QStringLiteral("关闭后会在下次启动时自动移除系统右键菜单中的“Ksword 文件解锁器(R3/R0)”项"));
    startupLayout->addWidget(m_unlockerShellContextMenuCheckBox);

    // 启动缩放因子设置：重启后生效，用于统一控制主窗口 UI 缩放倍率。
    QHBoxLayout* startupScaleLayout = new QHBoxLayout();
    startupScaleLayout->setSpacing(6);
    startupScaleLayout->addWidget(new QLabel(QStringLiteral("窗口缩放因子"), startupGroupBox), 0);

    // m_startupWindowScaleFactorEdit 作用：输入下次启动主窗口缩放因子（1.00=100%）。
    m_startupWindowScaleFactorEdit = new QLineEdit(startupGroupBox);
    m_startupWindowScaleFactorEdit->setPlaceholderText(QStringLiteral("1.00"));
    m_startupWindowScaleFactorEdit->setFixedWidth(96);
    m_startupWindowScaleFactorEdit->setToolTip(
        QStringLiteral("主窗口缩放因子（重启生效）：1.00=100%，建议范围 0.50~2.00"));
    startupScaleLayout->addWidget(m_startupWindowScaleFactorEdit, 0);
    startupScaleLayout->addStretch(1);
    startupLayout->addLayout(startupScaleLayout);

    // m_startupWindowScaleHintLabel 作用：显示缩放因子对应百分比提示。
    m_startupWindowScaleHintLabel = new QLabel(
        QStringLiteral("当前：约 100%（重启后生效，系统缩放会叠加）"),
        startupGroupBox);
    m_startupWindowScaleHintLabel->setWordWrap(true);
    startupLayout->addWidget(m_startupWindowScaleHintLabel);

    appearanceRootLayout->addWidget(startupGroupBox);

    // ===== 应用按钮区域 =====
    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->addStretch(1);

    // m_applySettingsButton 作用：把当前待生效改动落盘并触发界面实际应用（文字按钮）。
    m_applySettingsButton = new QPushButton(QStringLiteral("应用"), m_appearanceTab);
    m_applySettingsButton->setMinimumWidth(72);
    m_applySettingsButton->setFixedHeight(30);
    m_applySettingsButton->setToolTip(QStringLiteral("应用当前设置改动"));
    m_applySettingsButton->setEnabled(false);
    actionLayout->addWidget(m_applySettingsButton, 0);
    appearanceRootLayout->addLayout(actionLayout);

    appearanceRootLayout->addStretch();
    m_tabWidget->addTab(m_appearanceTab, QStringLiteral("外观与启动"));

    bindAppearanceSignals();
    updateThemeButtonStyle();
    updateApplyButtonState();
}

void SettingsDock::bindAppearanceSignals()
{
    connect(m_themeButtonGroup, &QButtonGroup::idClicked, this, [this](int /*clickedId*/) {
        updateThemeButtonStyle();
        markPendingChanges(QStringLiteral("主题按钮切换"));
        });

    connect(m_backgroundPathEdit, &QLineEdit::editingFinished, this, [this]() {
        markPendingChanges(QStringLiteral("背景路径编辑完成"));
        });

    connect(m_backgroundOpacitySlider, &QSlider::valueChanged, this, [this](const int value) {
        updateOpacityValueLabel(value);
        if (!m_isApplyingUiState)
        {
            markPendingChanges(QStringLiteral("背景透明度变化"));
        }
        });

    connect(m_browseBackgroundButton, &QToolButton::clicked, this, [this]() {
        openBackgroundFileDialog();
        });

    connect(m_resetBackgroundButton, &QToolButton::clicked, this, [this]() {
        resetBackgroundPathToDefault();
        });

    connect(m_startupDefaultTabCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        markPendingChanges(QStringLiteral("启动默认页签切换"));
        });

    connect(m_startupMaximizedCheckBox, &QCheckBox::toggled, this, [this](const bool /*checkedState*/) {
        markPendingChanges(QStringLiteral("启动时最大化开关切换"));
        });

    connect(m_startupAutoAdminCheckBox, &QCheckBox::toggled, this, [this](const bool /*checkedState*/) {
        markPendingChanges(QStringLiteral("启动时自动请求管理员权限开关切换"));
        });

    connect(m_unlockerShellContextMenuCheckBox, &QCheckBox::toggled, this, [this](const bool /*checkedState*/) {
        markPendingChanges(QStringLiteral("系统右键文件解锁器开关切换"));
        });

    connect(m_startupWindowScaleFactorEdit, &QLineEdit::textChanged, this, [this](const QString& /*text*/) {
        const double normalizedScaleFactor = parseWindowScaleFactorFromUi();
        updateWindowScaleFactorHintLabel(normalizedScaleFactor);
        if (!m_isApplyingUiState)
        {
            markPendingChanges(QStringLiteral("启动窗口缩放因子变化"));
        }
        });

    connect(m_applySettingsButton, &QPushButton::clicked, this, [this]() {
        saveAndEmitFromUi(QStringLiteral("点击应用按钮"));
        });
}

void SettingsDock::loadSettingsFromJson()
{
    m_currentAppearanceSettings = ks::settings::loadAppearanceSettings();
    applySettingsToUi(m_currentAppearanceSettings);
    m_hasPendingChanges = false;
    updateApplyButtonState();
    emit appearanceSettingsChanged(m_currentAppearanceSettings);
}

void SettingsDock::applySettingsToUi(const ks::settings::AppearanceSettings& settings)
{
    m_isApplyingUiState = true;

    // selectedButton 作用：根据主题模式找到对应按钮并置为选中。
    QAbstractButton* selectedButton = m_themeButtonGroup->button(static_cast<int>(settings.themeMode));
    if (selectedButton != nullptr)
    {
        selectedButton->setChecked(true);
    }
    else if (m_followSystemButton != nullptr)
    {
        m_followSystemButton->setChecked(true);
    }

    m_backgroundPathEdit->setText(settings.backgroundImagePath);
    m_backgroundOpacitySlider->setValue(settings.backgroundOpacityPercent);

    // startupTabIndex 作用：把配置中的启动页 key 映射为下拉框索引；缺失时回退“欢迎”。
    int startupTabIndex = -1;
    if (m_startupDefaultTabCombo != nullptr)
    {
        startupTabIndex = m_startupDefaultTabCombo->findData(settings.startupDefaultTabKey.trimmed().toLower());
        if (startupTabIndex < 0)
        {
            startupTabIndex = m_startupDefaultTabCombo->findData(QStringLiteral("welcome"));
        }
        if (startupTabIndex >= 0)
        {
            m_startupDefaultTabCombo->setCurrentIndex(startupTabIndex);
        }
    }

    if (m_startupMaximizedCheckBox != nullptr)
    {
        m_startupMaximizedCheckBox->setChecked(settings.launchMaximizedOnStartup);
    }

    if (m_startupAutoAdminCheckBox != nullptr)
    {
        m_startupAutoAdminCheckBox->setChecked(settings.autoRequestAdminOnStartup);
    }
    if (m_unlockerShellContextMenuCheckBox != nullptr)
    {
        m_unlockerShellContextMenuCheckBox->setChecked(settings.unlockerShellContextMenuEnabled);
    }

    if (m_startupWindowScaleFactorEdit != nullptr)
    {
        const double normalizedScaleFactor =
            ks::settings::normalizeWindowScaleFactor(settings.startupWindowScaleFactor);
        m_startupWindowScaleFactorEdit->setText(formatScaleFactorText(normalizedScaleFactor));
    }

    updateOpacityValueLabel(settings.backgroundOpacityPercent);
    updateWindowScaleFactorHintLabel(
        ks::settings::normalizeWindowScaleFactor(settings.startupWindowScaleFactor));
    updateThemeButtonStyle();

    m_isApplyingUiState = false;
    m_hasPendingChanges = false;
    updateApplyButtonState();
}

ks::settings::AppearanceSettings SettingsDock::collectSettingsFromUi() const
{
    ks::settings::AppearanceSettings collectedSettings;

    // checkedThemeId 作用：读取当前选中的主题按钮 ID。
    const int checkedThemeId = m_themeButtonGroup->checkedId();
    if (checkedThemeId == static_cast<int>(ks::settings::ThemeMode::Light))
    {
        collectedSettings.themeMode = ks::settings::ThemeMode::Light;
    }
    else if (checkedThemeId == static_cast<int>(ks::settings::ThemeMode::Dark))
    {
        collectedSettings.themeMode = ks::settings::ThemeMode::Dark;
    }
    else
    {
        collectedSettings.themeMode = ks::settings::ThemeMode::FollowSystem;
    }

    const QString rawPathText = m_backgroundPathEdit->text().trimmed();
    collectedSettings.backgroundImagePath = rawPathText.isEmpty()
        ? QStringLiteral("style/ksword_background.png")
        : rawPathText;

    collectedSettings.backgroundOpacityPercent = m_backgroundOpacitySlider->value();
    if (m_startupDefaultTabCombo != nullptr)
    {
        const QString startupTabKey = m_startupDefaultTabCombo->currentData().toString().trimmed().toLower();
        collectedSettings.startupDefaultTabKey = startupTabKey.isEmpty()
            ? QStringLiteral("welcome")
            : startupTabKey;
    }
    else
    {
        collectedSettings.startupDefaultTabKey = QStringLiteral("welcome");
    }
    collectedSettings.launchMaximizedOnStartup =
        (m_startupMaximizedCheckBox != nullptr) && m_startupMaximizedCheckBox->isChecked();
    collectedSettings.autoRequestAdminOnStartup =
        (m_startupAutoAdminCheckBox != nullptr) && m_startupAutoAdminCheckBox->isChecked();
    collectedSettings.startupWindowScaleFactor = parseWindowScaleFactorFromUi();
    // 该开关来自启动前弹窗，不在设置页编辑；这里保留内存值，避免保存时被覆盖。
    collectedSettings.startupScaleRecommendPromptDisabled =
        m_currentAppearanceSettings.startupScaleRecommendPromptDisabled;
    collectedSettings.unlockerShellContextMenuEnabled =
        (m_unlockerShellContextMenuCheckBox != nullptr) && m_unlockerShellContextMenuCheckBox->isChecked();

    return collectedSettings;
}

void SettingsDock::markPendingChanges(const QString& triggerReason)
{
    Q_UNUSED(triggerReason);
    if (m_isApplyingUiState)
    {
        return;
    }

    m_hasPendingChanges = true;
    updateApplyButtonState();
}

void SettingsDock::updateApplyButtonState()
{
    if (m_applySettingsButton == nullptr)
    {
        return;
    }

    m_applySettingsButton->setEnabled(m_hasPendingChanges);
    m_applySettingsButton->setToolTip(
        m_hasPendingChanges
        ? QStringLiteral("应用当前设置改动（主题/背景立即生效，启动相关选项下次启动生效）")
        : QStringLiteral("当前设置已应用，无待提交改动"));
}

void SettingsDock::saveAndEmitFromUi(const QString& triggerReason)
{
    if (m_isApplyingUiState)
    {
        return;
    }

    // settingsEvent 作用：本次“设置变更”调用链统一日志事件对象。
    kLogEvent settingsEvent;
    const ks::settings::AppearanceSettings nextSettings = collectSettingsFromUi();
    const bool sameScaleFactor =
        std::fabs(nextSettings.startupWindowScaleFactor - m_currentAppearanceSettings.startupWindowScaleFactor) < 0.0001;

    if (nextSettings.themeMode == m_currentAppearanceSettings.themeMode
        && nextSettings.backgroundImagePath == m_currentAppearanceSettings.backgroundImagePath
        && nextSettings.backgroundOpacityPercent == m_currentAppearanceSettings.backgroundOpacityPercent
        && nextSettings.startupDefaultTabKey == m_currentAppearanceSettings.startupDefaultTabKey
        && nextSettings.launchMaximizedOnStartup == m_currentAppearanceSettings.launchMaximizedOnStartup
        && nextSettings.autoRequestAdminOnStartup == m_currentAppearanceSettings.autoRequestAdminOnStartup
        && sameScaleFactor
        && nextSettings.startupScaleRecommendPromptDisabled == m_currentAppearanceSettings.startupScaleRecommendPromptDisabled
        && nextSettings.unlockerShellContextMenuEnabled == m_currentAppearanceSettings.unlockerShellContextMenuEnabled)
    {
        m_hasPendingChanges = false;
        updateApplyButtonState();
        return;
    }

    QString saveErrorText;
    const bool saveOk = ks::settings::saveAppearanceSettings(nextSettings, &saveErrorText);
    if (!saveOk)
    {
        err << settingsEvent
            << "[SettingsDock] 保存外观设置失败，触发来源="
            << triggerReason.toStdString()
            << "，错误="
            << saveErrorText.toStdString()
            << eol;
        return;
    }

    m_currentAppearanceSettings = nextSettings;
    m_isApplyingUiState = true;
    if (m_startupWindowScaleFactorEdit != nullptr)
    {
        m_startupWindowScaleFactorEdit->setText(
            formatScaleFactorText(m_currentAppearanceSettings.startupWindowScaleFactor));
    }
    updateWindowScaleFactorHintLabel(m_currentAppearanceSettings.startupWindowScaleFactor);
    m_isApplyingUiState = false;
    m_hasPendingChanges = false;
    updateApplyButtonState();

    info << settingsEvent
        << "[SettingsDock] 外观设置已保存，触发来源="
        << triggerReason.toStdString()
        << "，主题模式="
        << ks::settings::themeModeToJsonText(m_currentAppearanceSettings.themeMode).toStdString()
        << "，背景路径="
        << m_currentAppearanceSettings.backgroundImagePath.toStdString()
        << "，透明度="
        << m_currentAppearanceSettings.backgroundOpacityPercent
        << "%，启动默认页签="
        << m_currentAppearanceSettings.startupDefaultTabKey.toStdString()
        << "，启动时最大化="
        << (m_currentAppearanceSettings.launchMaximizedOnStartup ? "true" : "false")
        << "，启动时自动请求管理员权限="
        << (m_currentAppearanceSettings.autoRequestAdminOnStartup ? "true" : "false")
        << "，启动窗口缩放因子="
        << m_currentAppearanceSettings.startupWindowScaleFactor
        << "，小屏缩放提示不再弹出="
        << (m_currentAppearanceSettings.startupScaleRecommendPromptDisabled ? "true" : "false")
        << "，系统右键文件解锁器菜单="
        << (m_currentAppearanceSettings.unlockerShellContextMenuEnabled ? "true" : "false")
        << eol;

    emit appearanceSettingsChanged(m_currentAppearanceSettings);
}

void SettingsDock::updateThemeButtonStyle()
{
    const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
    const QString normalStyle = darkModeEnabled
        ? QStringLiteral(
            "QToolButton{"
            "  border:1px solid #5A5A5A;"
            "  border-radius:6px;"
            "  background:#202020;"
            "}"
            "QToolButton:hover{"
            "  background:#2A2A2A;"
            "}")
        : QStringLiteral(
            "QToolButton{"
            "  border:1px solid #6A6A6A;"
            "  border-radius:6px;"
            "  background:#EDF5FF;"
            "}"
            "QToolButton:hover{"
            "  background:#DCEBFF;"
            "}");

    const QString checkedStyle = darkModeEnabled
        ? QStringLiteral(
            "QToolButton{"
            "  border:2px solid #43A0FF;"
            "  border-radius:6px;"
            "  background:#1B2A3C;"
            "}")
        : QStringLiteral(
            "QToolButton{"
            "  border:2px solid #43A0FF;"
            "  border-radius:6px;"
            "  background:#DDEEFF;"
            "}");

    const QList<QAbstractButton*> themeButtons = m_themeButtonGroup->buttons();
    for (QAbstractButton* themeButton : themeButtons)
    {
        QToolButton* themedToolButton = qobject_cast<QToolButton*>(themeButton);
        if (themedToolButton == nullptr)
        {
            continue;
        }
        themedToolButton->setStyleSheet(themedToolButton->isChecked() ? checkedStyle : normalStyle);
    }
}

void SettingsDock::updateOpacityValueLabel(const int opacityPercent)
{
    m_backgroundOpacityValueLabel->setText(QStringLiteral("%1%").arg(opacityPercent));
}

void SettingsDock::updateWindowScaleFactorHintLabel(const double normalizedScaleFactor)
{
    if (m_startupWindowScaleHintLabel == nullptr)
    {
        return;
    }

    const double safeScaleFactor = ks::settings::normalizeWindowScaleFactor(normalizedScaleFactor);
    const int scalePercent = static_cast<int>(std::lround(safeScaleFactor * 100.0));
    m_startupWindowScaleHintLabel->setText(
        QStringLiteral("当前：约 %1%%（重启后生效，系统缩放会叠加）").arg(scalePercent));
}

double SettingsDock::parseWindowScaleFactorFromUi() const
{
    const double currentScaleFactor =
        ks::settings::normalizeWindowScaleFactor(m_currentAppearanceSettings.startupWindowScaleFactor);

    if (m_startupWindowScaleFactorEdit == nullptr)
    {
        return currentScaleFactor;
    }

    QString rawScaleText = m_startupWindowScaleFactorEdit->text().trimmed();
    if (rawScaleText.isEmpty())
    {
        return currentScaleFactor;
    }

    // 兼容中文输入法场景下使用逗号作为小数点。
    rawScaleText.replace(',', '.');
    bool convertOk = false;
    const double parsedScaleFactor = rawScaleText.toDouble(&convertOk);
    if (!convertOk)
    {
        return currentScaleFactor;
    }
    return ks::settings::normalizeWindowScaleFactor(parsedScaleFactor);
}

void SettingsDock::openBackgroundFileDialog()
{
    const QString selectedFilePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择背景图片"),
        m_backgroundPathEdit->text(),
        QStringLiteral("图片文件 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*.*)"));

    if (selectedFilePath.isEmpty())
    {
        return;
    }

    m_backgroundPathEdit->setText(selectedFilePath);
    markPendingChanges(QStringLiteral("浏览按钮选择背景图"));
}

void SettingsDock::resetBackgroundPathToDefault()
{
    m_backgroundPathEdit->setText(QStringLiteral("style/ksword_background.png"));
    markPendingChanges(QStringLiteral("恢复默认背景路径"));
}
