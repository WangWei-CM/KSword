#include "BootEditorTab.h"
#include "../../UI/AsyncOperation.h"
#include "../../Internationalization/LanguageManager.h"
#include "../../UI/TableContextMenu.h"
#include "../../UI/PageControlStyle.h"
#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../UI/CodeTextEdit.h"

#include "../../Framework/PrivilegeElevationPrompt.h"
#include "../../UI/VisibleTableWidget.h"

#include "../../theme.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>

namespace
{

    enum class BootSearchText
    {
        Filter, Admin, NonAdmin, Basic, Identifier, Type, PolicyUnchanged, PolicyStandard, PolicyLegacy, IdentifierCaption, TypeCaption, DescriptionCaption, DeviceCaption, OsDeviceCaption, PathCaption, SystemRootCaption, LocaleCaption, PolicyCaption, TimeoutCaption, ApplyTimeout, LegacySection, FlagsSection, TestSigning, NoIntegrity, Debug, BootLog, BaseVideo, Recovery, SafeOff, SafeMinimal, SafeNetwork, SafeShell, SafeCaption, CustomSection, ApplyEntry, Reload, HeaderIdentifier, HeaderDescription, HeaderType, HeaderDevice, HeaderPath, HeaderState, Output, Status
    };
    struct BootSearchEntry { const char* target; const char* source; bool mnemonic; };
    constexpr BootSearchEntry kBootSearchTexts[] = {
        {"ks_boot_filter", "筛选：标识符/描述/路径/类型", false},
        {"ks_boot_admin", "权限：管理员（可编辑）", false},
        {"ks_boot_admin", "权限：非管理员（多数写操作会失败）", false},
        {"ks_boot_basic", "基础字段", false},
        {"ks_boot_identifier", "-", false},
        {"ks_boot_type", "-", false},
        {"ks_boot_policy", "不修改", false},
        {"ks_boot_policy", "Standard", false},
        {"ks_boot_policy", "Legacy", false},
        {"ks_boot_identifier_caption", "标识符", false},
        {"ks_boot_type_caption", "对象类型", false},
        {"ks_boot_description_caption", "描述 description", false},
        {"ks_boot_device_caption", "设备 device", false},
        {"ks_boot_osdevice_caption", "OS 设备 osdevice", false},
        {"ks_boot_path_caption", "加载路径 path", false},
        {"ks_boot_systemroot_caption", "系统根 systemroot", false},
        {"ks_boot_locale_caption", "区域 locale", false},
        {"ks_boot_policy_caption", "启动菜单策略", false},
        {"ks_boot_timeout_caption", "菜单等待超时", false},
        {"ks_boot_apply_timeout", "应用 bootmgr 超时", true},
        {"ks_boot_legacy_section", "传统引导（Legacy/F8）", true},
        {"ks_boot_flags_section", "高级开关", true},
        {"ks_boot_testsigning", "开启测试签名 (testsigning)", true},
        {"ks_boot_nointegrity", "关闭完整性检查 (nointegritychecks)", true},
        {"ks_boot_debug", "开启内核调试 (debug)", true},
        {"ks_boot_bootlog", "开启启动日志 (bootlog)", true},
        {"ks_boot_basevideo", "使用基础视频驱动 (basevideo)", true},
        {"ks_boot_recovery", "启用恢复环境 (recoveryenabled)", true},
        {"ks_boot_safeboot", "关闭安全模式", false},
        {"ks_boot_safeboot", "最小安全模式", false},
        {"ks_boot_safeboot", "网络安全模式", false},
        {"ks_boot_safeboot", "命令行安全模式", false},
        {"ks_boot_safeboot_caption", "safeboot 模式", false},
        {"ks_boot_custom_section", "自定义命令", true},
        {"ks_boot_apply_entry", "应用当前条目", true},
        {"ks_boot_reload", "重读当前条目", true},
        {"ks_boot_entries", "标识符", false},
        {"ks_boot_entries", "描述", false},
        {"ks_boot_entries", "类型", false},
        {"ks_boot_entries", "设备", false},
        {"ks_boot_entries", "路径", false},
        {"ks_boot_entries", "状态", false},
        {"ks_boot_output", "原始输出", false},
        {"ks_boot_status", "状态：尚未加载 BCD 数据", false},
    };
    const BootSearchEntry& bootSearchEntry(BootSearchText id)
    {
        return kBootSearchTexts[static_cast<int>(id)];
    }
    QString bootSearchSource(BootSearchText id)
    {
        return QString::fromUtf8(bootSearchEntry(id).source);
    }
    QString bootSearchTarget(BootSearchText id)
    {
        return QString::fromLatin1(bootSearchEntry(id).target);
    }
    QStringList bootSearchHeaders()
    {
        return {bootSearchSource(BootSearchText::HeaderIdentifier), bootSearchSource(BootSearchText::HeaderDescription),
            bootSearchSource(BootSearchText::HeaderType), bootSearchSource(BootSearchText::HeaderDevice),
            bootSearchSource(BootSearchText::HeaderPath), bootSearchSource(BootSearchText::HeaderState)};
    }

    // 列索引常量：
    // - 统一定义条目表格列顺序；
    // - 避免代码中散落魔法数字。
    constexpr int kColumnIdentifier = 0;
    constexpr int kColumnDescription = 1;
    constexpr int kColumnType = 2;
    constexpr int kColumnDevice = 3;
    constexpr int kColumnPath = 4;
    constexpr int kColumnFlags = 5;

    // kDefaultCommandTimeoutMs：
    // - bcdedit 执行超时时间（毫秒）；
    // - 避免命令异常挂起导致 UI 长时间阻塞。
    constexpr int kDefaultCommandTimeoutMs = 30000;

    // buildBlueToolButtonStyle：
    // - 统一工具栏图标按钮风格；
    // - 与项目蓝色主题保持一致。
    QString buildBlueToolButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    // isCurrentProcessElevated：
    // - 检查当前进程是否为管理员权限；
    // - 用于顶部提示“是否可能执行成功”。
    bool isCurrentProcessElevated()
    {
        HANDLE tokenHandle = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
        {
            return false;
        }

        TOKEN_ELEVATION tokenElevation{};
        DWORD returnLength = 0;
        const BOOL queryOk = ::GetTokenInformation(
            tokenHandle,
            TokenElevation,
            &tokenElevation,
            sizeof(tokenElevation),
            &returnLength);
        ::CloseHandle(tokenHandle);
        return queryOk != FALSE && tokenElevation.TokenIsElevated != 0;
    }

    // buildSafeBootModeText：
    // - 把 safeboot 文本转为可展示中文；
    // - 提升列表阅读效率。
    QString buildSafeBootModeText(const QString& safeBootValueText)
    {
        const QString normalizedText = safeBootValueText.trimmed().toLower();
        if (normalizedText == QStringLiteral("minimal"))
        {
            return QStringLiteral("安全模式-最小");
        }
        if (normalizedText == QStringLiteral("network"))
        {
            return QStringLiteral("安全模式-网络");
        }
        if (normalizedText.isEmpty())
        {
            return QStringLiteral("正常启动");
        }
        return QStringLiteral("安全模式-%1").arg(safeBootValueText);
    }
}

QVector<ks::ui::PageSearchBodyText> BootEditorTab::coldSearchBody()
{
    QVector<ks::ui::PageSearchBodyText> body;
    body.reserve(43);
    const auto text = [](BootSearchText id) { return ks::i18n::sourceText(bootSearchSource(id)); };
    const auto append = [&body, &text](BootSearchText id, const QStringList& groups = {}) {
        body.append({text(id), bootSearchTarget(id), groups, bootSearchEntry(id).mnemonic});
    };
    append(BootSearchText::Filter);
    append(isCurrentProcessElevated() ? BootSearchText::Admin : BootSearchText::NonAdmin);
    // Follow the original QObject order after ParameterWorkspace reparents the
    // two parameter columns, fixed actions and result splitter. Collapsed
    // Legacy/custom contents are not revealable under the old search rules.
    append(BootSearchText::Basic);
    const QStringList basic{text(BootSearchText::Basic)};
    for (const auto id : {BootSearchText::Identifier, BootSearchText::Type,
        BootSearchText::PolicyUnchanged, BootSearchText::PolicyStandard, BootSearchText::PolicyLegacy,
        BootSearchText::IdentifierCaption, BootSearchText::TypeCaption, BootSearchText::DescriptionCaption,
        BootSearchText::DeviceCaption, BootSearchText::OsDeviceCaption, BootSearchText::PathCaption,
        BootSearchText::SystemRootCaption, BootSearchText::LocaleCaption, BootSearchText::PolicyCaption,
        BootSearchText::TimeoutCaption, BootSearchText::ApplyTimeout}) append(id, basic);
    append(BootSearchText::LegacySection);
    append(BootSearchText::FlagsSection);
    for (const auto id : {BootSearchText::TestSigning, BootSearchText::NoIntegrity, BootSearchText::Debug,
        BootSearchText::BootLog, BootSearchText::BaseVideo, BootSearchText::Recovery,
        BootSearchText::SafeOff, BootSearchText::SafeMinimal, BootSearchText::SafeNetwork,
        BootSearchText::SafeShell, BootSearchText::SafeCaption, BootSearchText::CustomSection,
        BootSearchText::ApplyEntry, BootSearchText::Reload, BootSearchText::HeaderIdentifier,
        BootSearchText::HeaderDescription, BootSearchText::HeaderType, BootSearchText::HeaderDevice,
        BootSearchText::HeaderPath, BootSearchText::HeaderState, BootSearchText::Output,
        BootSearchText::Status}) append(id);
    return body;
}

BootEditorTab::BootEditorTab(QWidget* parent)
    : QWidget(parent)
{
    // 构造期只搭 UI：
    // - 首轮 BCD 枚举挪到 showEvent，避免“杂项”页一被创建就去拉起 bcdedit；
    // - 状态栏保持 initializeUi 写入的“尚未加载”，语义与真实状态一致。
    initializeUi();
    initializeConnections();
    m_commandOperation = new ks::ui::AsyncOperation(this);
    m_commandOperation->setStateChangedCallback([this]() {
        if (m_commandPending && !m_commandOperation->isInFlight())
        {
            setBcdCommandBusy(false);
            m_statusLabel->setText(ks::i18n::text(QStringLiteral("misc.async.receipt_missing"), QStringLiteral("后台任务未返回有效回执，请刷新状态。")));
        }
    });
}

// showEvent：
// - 作用：页面首次真正可见时排队一次 BCD 枚举，之后的显示/隐藏不再重复触发。
// - 入参 event：Qt 显示事件，透传给基类。
// - 返回：无。
void BootEditorTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (m_firstShowHandled)
    {
        return;
    }

    m_firstShowHandled = true;

    // 延后到下一轮事件循环：保证页面先完成首帧绘制，再排队后台枚举。
    QTimer::singleShot(0, this, [this]()
        {
            refreshBcdEntries();
        });
}

void BootEditorTab::initializeUi()
{
    // 根布局：
    // - 顶部工具栏；
    // - 中部上方条目表与下方三栏编辑区；
    // - 底部状态摘要。
    m_rootLayout = new QVBoxLayout(this);
    // ParameterWorkspace 已自带 16px 留白，外层只提供阅读基线。
    ks::ui::StyleWorkspacePage(this, 0);
    m_rootLayout->setSpacing(6);

    initializeToolbar();
    initializeCenterPane();

    m_statusLabel = new QLabel(bootSearchSource(BootSearchText::Status), this);
    m_statusLabel->setObjectName(bootSearchTarget(BootSearchText::Status));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setContentsMargins(16, 0, 16, 0);
    m_rootLayout->addWidget(m_statusLabel);
}

void BootEditorTab::initializeToolbar()
{
    // 顶部工具栏主要承载“高频全局操作”。
    m_toolbarWidget = new QWidget(this);
    m_toolbarLayout = new QHBoxLayout(m_toolbarWidget);
    m_toolbarLayout->setContentsMargins(16, 12, 16, 12);
    m_toolbarLayout->setSpacing(6);
    ks::ui::StylePrimaryToolbar(m_toolbarWidget);
    m_rootLayout->addWidget(m_toolbarWidget);

    // btnFactory：
    // - 统一构建带图标、tooltip 与蓝色主题的图标按钮；
    // - 避免按钮初始化逻辑重复。
    const auto btnFactory = [this](const QString& iconPath, const QString& tooltipText) -> QToolButton*
        {
            QToolButton* button = new QToolButton(m_toolbarWidget);
            button->setIcon(QIcon(iconPath));
            button->setToolTip(tooltipText);
            button->setStyleSheet(buildBlueToolButtonStyle());
            button->setAutoRaise(false);
            button->setIconSize(QSize(16, 16));
            return button;
        };

    m_refreshButton = btnFactory(QStringLiteral(":/Icon/process_refresh.svg"), QStringLiteral("刷新 BCD 枚举"));
    m_exportButton = btnFactory(QStringLiteral(":/Icon/log_export.svg"), QStringLiteral("导出当前 BCD 存储"));
    m_importButton = btnFactory(QStringLiteral(":/Icon/codeeditor_open.svg"), QStringLiteral("导入 BCD 存储"));
    m_copyEntryButton = btnFactory(QStringLiteral(":/Icon/process_copy_row.svg"), QStringLiteral("复制当前引导项"));
    m_deleteEntryButton = btnFactory(QStringLiteral(":/Icon/process_terminate.svg"), QStringLiteral("删除当前引导项"));
    m_setDefaultButton = btnFactory(QStringLiteral(":/Icon/process_priority.svg"), QStringLiteral("设为默认启动项"));
    m_bootOnceButton = btnFactory(QStringLiteral(":/Icon/process_start.svg"), QStringLiteral("下一次启动使用该项"));
    m_copyRowButton = btnFactory(QStringLiteral(":/Icon/log_copy.svg"), QStringLiteral("复制当前行概要"));

    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_exportButton);
    m_toolbarLayout->addWidget(m_importButton);
    m_toolbarLayout->addWidget(m_copyEntryButton);
    m_toolbarLayout->addWidget(m_deleteEntryButton);
    m_toolbarLayout->addWidget(m_setDefaultButton);
    m_toolbarLayout->addWidget(m_bootOnceButton);
    m_toolbarLayout->addWidget(m_copyRowButton);

    m_filterEdit = new QLineEdit(m_toolbarWidget);
    m_filterEdit->setObjectName(bootSearchTarget(BootSearchText::Filter));
    m_filterEdit->setPlaceholderText(bootSearchSource(BootSearchText::Filter));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setToolTip(QStringLiteral("输入关键字实时过滤引导条目"));
    ks::ui::StyleSearchField(m_filterEdit);
    m_toolbarLayout->addWidget(m_filterEdit, 1);

    // m_adminHintLabel：
    // - 明确提示 bcdedit 是否具备执行前提；
    // - 降低“点击后命令失败”的困惑。
    m_adminHintLabel = new QLabel(m_toolbarWidget);
    m_adminHintLabel->setObjectName(bootSearchTarget(BootSearchText::Admin));
    const bool elevated = isCurrentProcessElevated();
    m_adminHintLabel->setText(
        elevated
        ? bootSearchSource(BootSearchText::Admin)
        : bootSearchSource(BootSearchText::NonAdmin));
    // 管理员态改用 palette(highlight) 动态取色，主题切换时自动跟随；
    // 非管理员态保留语义警告橙，它本来就该在深浅色下都是同一个橙。
    m_adminHintLabel->setStyleSheet(
        elevated
        ? QStringLiteral("color:%1;font-weight:600;")
            .arg(KswordTheme::PrimaryBlueHex)
        : QStringLiteral("color:%1;font-weight:600;")
            .arg(KswordTheme::WarningColor().name(QColor::HexRgb)));
    m_toolbarLayout->addWidget(m_adminHintLabel);
    ks::ui::NormalizeToolbarRow(m_toolbarLayout);
}

void BootEditorTab::initializeCenterPane()
{
    // 参数表单占左侧，条目选择器与命令回执在右侧共享高度。
    auto* workspace = new ks::ui::ParameterWorkspace(this, 500);
    workspace->useWideParameterLayout(60);
    m_rootLayout->addWidget(workspace, 1);
    m_mainSplitter = new QSplitter(Qt::Vertical, workspace);
    m_mainSplitter->setChildrenCollapsible(false);
    workspace->results()->addWidget(m_mainSplitter, 1);

    // 上方表格：展示当前 BCD 条目列表。
    m_entryTable = new ks::ui::VisibleTableWidget(m_mainSplitter);
    m_entryTable->setObjectName(bootSearchTarget(BootSearchText::HeaderIdentifier));
    // BCD 条目是编辑选择器，页面已有存储导入导出与行复制，不增快照栏。
    ks::ui::SetTableActionBarMode(m_entryTable, ks::ui::TableActionBarMode::None);
    m_entryTable->setColumnCount(6);
    m_entryTable->setHorizontalHeaderLabels(bootSearchHeaders());
    m_entryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_entryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entryTable->setAlternatingRowColors(true);
    m_entryTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_entryTable->verticalHeader()->setVisible(false);
    m_entryTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    // 表头宽度策略：
    // - 描述与路径列可伸展；
    // - 其余列按内容自适应。
    QHeaderView* entryHeader = m_entryTable->horizontalHeader();
    entryHeader->setStretchLastSection(false);
    entryHeader->setSectionResizeMode(kColumnIdentifier, QHeaderView::ResizeToContents);
    entryHeader->setSectionResizeMode(kColumnDescription, QHeaderView::Stretch);
    entryHeader->setSectionResizeMode(kColumnType, QHeaderView::ResizeToContents);
    entryHeader->setSectionResizeMode(kColumnDevice, QHeaderView::ResizeToContents);
    entryHeader->setSectionResizeMode(kColumnPath, QHeaderView::Stretch);
    entryHeader->setSectionResizeMode(kColumnFlags, QHeaderView::ResizeToContents);

    // 合并原来两列输入；各设置分区按自然高度排列，低频开关可展开。
    m_editorPane = workspace;
    m_editorPaneLayout = workspace->parameters();
    QWidget* leftEditorColumn = workspace;
    QWidget* middleEditorColumn = workspace;
    QVBoxLayout* leftEditorLayout = workspace->parameters();
    QVBoxLayout* middleEditorLayout = workspace->parameters();
    QWidget* outputEditorColumn = new QWidget(m_mainSplitter);
    QVBoxLayout* outputEditorLayout = new QVBoxLayout(outputEditorColumn);
    outputEditorLayout->setContentsMargins(0, 0, 0, 0);
    outputEditorLayout->setSpacing(6);

    // 基础字段编辑组。
    QGroupBox* basicGroup = new QGroupBox(bootSearchSource(BootSearchText::Basic), leftEditorColumn);
    basicGroup->setObjectName(bootSearchTarget(BootSearchText::Basic));
    ks::ui::StylePrimaryGroup(basicGroup);
    QFormLayout* basicLayout = new QFormLayout(basicGroup);
    basicLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    basicLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);

    m_identifierValueLabel = new QLabel(bootSearchSource(BootSearchText::Identifier), basicGroup);
    m_identifierValueLabel->setObjectName(bootSearchTarget(BootSearchText::Identifier));
    m_identifierValueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_identifierValueLabel->setWordWrap(true);
    m_typeValueLabel = new QLabel(bootSearchSource(BootSearchText::Type), basicGroup);
    m_typeValueLabel->setObjectName(bootSearchTarget(BootSearchText::Type));
    m_typeValueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_typeValueLabel->setWordWrap(true);
    m_descriptionEdit = new QLineEdit(basicGroup);
    m_deviceEdit = new QLineEdit(basicGroup);
    m_osDeviceEdit = new QLineEdit(basicGroup);
    m_pathEdit = new QLineEdit(basicGroup);
    m_systemRootEdit = new QLineEdit(basicGroup);
    m_localeEdit = new QLineEdit(basicGroup);
    m_bootMenuPolicyCombo = new QComboBox(basicGroup);
    m_bootMenuPolicyCombo->setObjectName(bootSearchTarget(BootSearchText::PolicyUnchanged));
    ks::ui::StylePrimaryCombo(m_bootMenuPolicyCombo);
    m_bootMenuPolicyCombo->addItem(bootSearchSource(BootSearchText::PolicyUnchanged), QString());
    m_bootMenuPolicyCombo->addItem(bootSearchSource(BootSearchText::PolicyStandard), QStringLiteral("Standard"));
    m_bootMenuPolicyCombo->addItem(bootSearchSource(BootSearchText::PolicyLegacy), QStringLiteral("Legacy"));
    m_timeoutSpin = new QSpinBox(basicGroup);
    m_timeoutSpin->setRange(0, 999);
    m_timeoutSpin->setSuffix(QStringLiteral(" 秒"));

    // BCD 编辑字段沿用共享输入层；不能在组合框已登记主题后再覆盖整份旧 QSS。
    for (QLineEdit* field : {m_descriptionEdit, m_deviceEdit, m_osDeviceEdit,
        m_pathEdit, m_systemRootEdit, m_localeEdit})
    {
        ks::ui::StyleStandaloneInput(field);
    }
    ks::ui::StyleStandaloneInput(m_timeoutSpin);

    basicLayout->addRow(bootSearchSource(BootSearchText::IdentifierCaption), m_identifierValueLabel);
    basicLayout->labelForField(m_identifierValueLabel)->setObjectName(bootSearchTarget(BootSearchText::IdentifierCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::TypeCaption), m_typeValueLabel);
    basicLayout->labelForField(m_typeValueLabel)->setObjectName(bootSearchTarget(BootSearchText::TypeCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::DescriptionCaption), m_descriptionEdit);
    basicLayout->labelForField(m_descriptionEdit)->setObjectName(bootSearchTarget(BootSearchText::DescriptionCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::DeviceCaption), m_deviceEdit);
    basicLayout->labelForField(m_deviceEdit)->setObjectName(bootSearchTarget(BootSearchText::DeviceCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::OsDeviceCaption), m_osDeviceEdit);
    basicLayout->labelForField(m_osDeviceEdit)->setObjectName(bootSearchTarget(BootSearchText::OsDeviceCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::PathCaption), m_pathEdit);
    basicLayout->labelForField(m_pathEdit)->setObjectName(bootSearchTarget(BootSearchText::PathCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::SystemRootCaption), m_systemRootEdit);
    basicLayout->labelForField(m_systemRootEdit)->setObjectName(bootSearchTarget(BootSearchText::SystemRootCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::LocaleCaption), m_localeEdit);
    basicLayout->labelForField(m_localeEdit)->setObjectName(bootSearchTarget(BootSearchText::LocaleCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::PolicyCaption), m_bootMenuPolicyCombo);
    basicLayout->labelForField(m_bootMenuPolicyCombo)->setObjectName(bootSearchTarget(BootSearchText::PolicyCaption));
    basicLayout->addRow(bootSearchSource(BootSearchText::TimeoutCaption), m_timeoutSpin);
    basicLayout->labelForField(m_timeoutSpin)->setObjectName(bootSearchTarget(BootSearchText::TimeoutCaption));

    ks::ui::StyleParameterSection(basicGroup);
    leftEditorLayout->addWidget(basicGroup);

    // 传统引导快捷组：
    // - 传统引导 = bootmenupolicy Legacy（可用 F8 菜单）；
    // - 不等同于 BIOS/UEFI 固件模式切换（后者需在主板固件设置中操作）。
    QGroupBox* legacyGroup = new QGroupBox(bootSearchSource(BootSearchText::LegacySection), leftEditorColumn);
    ks::ui::StylePrimaryGroup(legacyGroup);
    QVBoxLayout* legacyLayout = new QVBoxLayout(legacyGroup);
    legacyLayout->setContentsMargins(8, 8, 8, 8);
    legacyLayout->setSpacing(6);

    m_legacyModeHintLabel = new QLabel(
        QStringLiteral("说明：这里的“传统引导”是 Windows 启动菜单策略（bootmenupolicy=Legacy），"
            "不是 BIOS/UEFI 模式切换。"),
        legacyGroup);
    m_legacyModeHintLabel->setWordWrap(true);
    legacyLayout->addWidget(m_legacyModeHintLabel);

    QWidget* legacyActionWidget = new QWidget(legacyGroup);
    QVBoxLayout* legacyActionLayout = new QVBoxLayout(legacyActionWidget);
    legacyActionLayout->setContentsMargins(0, 0, 0, 0);
    legacyActionLayout->setSpacing(6);

    m_setLegacyForSelectedButton = new QPushButton(QStringLiteral("当前项启用 Legacy"), legacyActionWidget);
    m_setLegacyForSelectedButton->setToolTip(
        QStringLiteral("对当前选中引导项执行：bcdedit /set <id> bootmenupolicy Legacy"));
    m_setLegacyForSelectedButton->setIcon(QIcon(QStringLiteral(":/Icon/process_start.svg")));
    legacyActionLayout->addWidget(m_setLegacyForSelectedButton);

    m_setLegacyForDefaultButton = new QPushButton(QStringLiteral("默认项启用 Legacy"), legacyActionWidget);
    m_setLegacyForDefaultButton->setToolTip(
        QStringLiteral("对当前默认引导项执行：bcdedit /set <default> bootmenupolicy Legacy"));
    m_setLegacyForDefaultButton->setIcon(QIcon(QStringLiteral(":/Icon/process_priority.svg")));
    legacyActionLayout->addWidget(m_setLegacyForDefaultButton);

    m_setStandardForSelectedButton = new QPushButton(QStringLiteral("当前项恢复 Standard"), legacyActionWidget);
    m_setStandardForSelectedButton->setToolTip(
        QStringLiteral("对当前选中引导项执行：bcdedit /set <id> bootmenupolicy Standard"));
    m_setStandardForSelectedButton->setIcon(QIcon(QStringLiteral(":/Icon/process_refresh.svg")));
    legacyActionLayout->addWidget(m_setStandardForSelectedButton);
    for (QPushButton* button : { m_setLegacyForSelectedButton, m_setLegacyForDefaultButton, m_setStandardForSelectedButton })
        ks::ui::NormalizeToolbarControl(button);
    legacyLayout->addWidget(legacyActionWidget);
    QWidget* legacySection = ks::ui::FoldParameterSection(legacyGroup); // 低频 Legacy 操作仍可折叠。
    legacySection->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)->setObjectName(bootSearchTarget(BootSearchText::LegacySection));
    leftEditorLayout->addWidget(legacySection);

    // 高级开关组。
    QGroupBox* flagGroup = new QGroupBox(bootSearchSource(BootSearchText::FlagsSection), middleEditorColumn);
    ks::ui::StylePrimaryGroup(flagGroup);
    QVBoxLayout* flagLayout = new QVBoxLayout(flagGroup);
    // 标题与内容之间保留额外顶部空间，避免全局 QGroupBox 标题压住第一个复选框。
    flagLayout->setContentsMargins(8, 14, 8, 8);
    flagLayout->setSpacing(4);

    m_testSigningCheck = new QCheckBox(bootSearchSource(BootSearchText::TestSigning), flagGroup);
    m_testSigningCheck->setObjectName(bootSearchTarget(BootSearchText::TestSigning));
    m_noIntegrityCheck = new QCheckBox(bootSearchSource(BootSearchText::NoIntegrity), flagGroup);
    m_noIntegrityCheck->setObjectName(bootSearchTarget(BootSearchText::NoIntegrity));
    m_debugCheck = new QCheckBox(bootSearchSource(BootSearchText::Debug), flagGroup);
    m_debugCheck->setObjectName(bootSearchTarget(BootSearchText::Debug));
    m_bootLogCheck = new QCheckBox(bootSearchSource(BootSearchText::BootLog), flagGroup);
    m_bootLogCheck->setObjectName(bootSearchTarget(BootSearchText::BootLog));
    m_baseVideoCheck = new QCheckBox(bootSearchSource(BootSearchText::BaseVideo), flagGroup);
    m_baseVideoCheck->setObjectName(bootSearchTarget(BootSearchText::BaseVideo));
    m_recoveryEnabledCheck = new QCheckBox(bootSearchSource(BootSearchText::Recovery), flagGroup);
    m_recoveryEnabledCheck->setObjectName(bootSearchTarget(BootSearchText::Recovery));
    m_safeBootCombo = new QComboBox(flagGroup);
    m_safeBootCombo->setObjectName(bootSearchTarget(BootSearchText::SafeOff));
    ks::ui::StylePrimaryCombo(m_safeBootCombo);
    m_safeBootCombo->setToolTip(QStringLiteral("设置 safeboot 模式。关闭会删除 safeboot 字段。"));
    m_safeBootCombo->addItem(bootSearchSource(BootSearchText::SafeOff), QStringLiteral("off"));
    m_safeBootCombo->addItem(bootSearchSource(BootSearchText::SafeMinimal), QStringLiteral("minimal"));
    m_safeBootCombo->addItem(bootSearchSource(BootSearchText::SafeNetwork), QStringLiteral("network"));
    m_safeBootCombo->addItem(bootSearchSource(BootSearchText::SafeShell), QStringLiteral("alternateshell"));

    flagLayout->addWidget(m_testSigningCheck);
    flagLayout->addWidget(m_noIntegrityCheck);
    flagLayout->addWidget(m_debugCheck);
    flagLayout->addWidget(m_bootLogCheck);
    flagLayout->addWidget(m_baseVideoCheck);
    flagLayout->addWidget(m_recoveryEnabledCheck);
    auto* safeCaption = new QLabel(bootSearchSource(BootSearchText::SafeCaption), flagGroup);
    safeCaption->setObjectName(bootSearchTarget(BootSearchText::SafeCaption));
    flagLayout->addWidget(safeCaption);
    flagLayout->addWidget(m_safeBootCombo);
    QWidget* flagSection = ks::ui::FoldParameterSection(flagGroup, true); // 常用引导开关默认可见。
    flagSection->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)->setObjectName(bootSearchTarget(BootSearchText::FlagsSection));
    middleEditorLayout->addWidget(flagSection);

    // 应用动作区：分离“当前条目修改”和“bootmgr 全局参数”。
    QWidget* actionWidget = workspace;
    QHBoxLayout* actionLayout = workspace->actions();

    m_applyEntryButton = new QPushButton(bootSearchSource(BootSearchText::ApplyEntry), actionWidget);
    m_applyEntryButton->setObjectName(bootSearchTarget(BootSearchText::ApplyEntry));
    m_applyEntryButton->setToolTip(QStringLiteral("按基础字段与高级开关写回当前选中的 BCD 条目"));
    m_applyEntryButton->setIcon(QIcon(QStringLiteral(":/Icon/process_start.svg")));
    m_applyBootMgrButton = new QPushButton(bootSearchSource(BootSearchText::ApplyTimeout), actionWidget);
    m_applyBootMgrButton->setObjectName(bootSearchTarget(BootSearchText::ApplyTimeout));
    m_applyBootMgrButton->setToolTip(QStringLiteral("仅写入 bcdedit /timeout 参数"));
    m_applyBootMgrButton->setIcon(QIcon(QStringLiteral(":/Icon/process_priority.svg")));
    m_reloadOneButton = new QPushButton(bootSearchSource(BootSearchText::Reload), actionWidget);
    m_reloadOneButton->setObjectName(bootSearchTarget(BootSearchText::Reload));
    m_reloadOneButton->setToolTip(QStringLiteral("按当前选中行重新加载编辑字段（不写入）"));
    m_reloadOneButton->setIcon(QIcon(QStringLiteral(":/Icon/process_refresh.svg")));

    actionLayout->addWidget(m_applyEntryButton);
    basicLayout->addRow(m_applyBootMgrButton);
    actionLayout->addWidget(m_reloadOneButton);
    actionLayout->addStretch(1);
    ks::ui::NormalizeToolbarRow(actionLayout);

    // 自定义命令组：支持输入任意 bcdedit 参数。
    QGroupBox* customGroup = new QGroupBox(bootSearchSource(BootSearchText::CustomSection), middleEditorColumn);
    ks::ui::StylePrimaryGroup(customGroup);
    QHBoxLayout* customLayout = new QHBoxLayout(customGroup);
    customLayout->setContentsMargins(8, 8, 8, 8);
    customLayout->setSpacing(6);

    m_customCommandEdit = new QLineEdit(customGroup);
    m_customCommandEdit->setPlaceholderText(QStringLiteral("示例：/set {current} bootmenupolicy Legacy"));
    m_customCommandEdit->setToolTip(QStringLiteral("只填写 bcdedit 后面的参数；也支持完整输入 bcdedit ..."));
    ks::ui::StyleStandaloneInput(m_customCommandEdit);
    m_runCustomCommandButton = new QPushButton(QStringLiteral("执行"), customGroup);
    m_runCustomCommandButton->setToolTip(QStringLiteral("执行自定义 bcdedit 命令并输出原始结果"));
    m_runCustomCommandButton->setIcon(QIcon(QStringLiteral(":/Icon/process_start.svg")));

    customLayout->addWidget(m_customCommandEdit, 1);
    customLayout->addWidget(m_runCustomCommandButton, 0);
    ks::ui::NormalizeToolbarRow(customLayout);
    QWidget* customSection = ks::ui::FoldParameterSection(customGroup); // 任意命令保持低频入口。
    customSection->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)->setObjectName(bootSearchTarget(BootSearchText::CustomSection));
    middleEditorLayout->addWidget(customSection);

    // 原始输出位于右侧条目表下方，不再占用参数栏或产生第三个窄列。
    QGroupBox* outputGroup = new QGroupBox(bootSearchSource(BootSearchText::Output), outputEditorColumn);
    outputGroup->setObjectName(bootSearchTarget(BootSearchText::Output));
    ks::ui::StylePrimaryGroup(outputGroup);
    QVBoxLayout* outputLayout = new QVBoxLayout(outputGroup);
    outputLayout->setContentsMargins(8, 8, 8, 8);
    outputLayout->setSpacing(4);

    m_rawOutputEdit = new CodeTextEdit(outputGroup);
    static_cast<CodeTextEdit*>(m_rawOutputEdit)->setSyntaxLanguage(CodeTextEdit::SyntaxLanguage::PlainText);
    m_rawOutputEdit->setReadOnly(true);
    m_rawOutputEdit->setPlaceholderText(QStringLiteral("这里显示 bcdedit 原始输出与命令执行日志。"));
    m_rawOutputEdit->setMinimumHeight(180);
    outputLayout->addWidget(m_rawOutputEdit);
    ks::ui::StyleParameterSection(outputGroup);
    outputEditorLayout->addWidget(outputGroup, 1);

    // 右侧表格优先保证条目可读，下方保留命令输出的连续观察区域。
    m_mainSplitter->setStretchFactor(0, 6);
    m_mainSplitter->setStretchFactor(1, 5);
    m_mainSplitter->setSizes(QList<int>{ 380, 320 });

    // 基础字段与开关宽屏互补两列，保持原控件、选择条目与写回流程。
    ks::ui::ArrangeParameterColumns(workspace->parameters(),
        { basicGroup, legacySection }, { flagSection, customSection }, 300);
    workspace->finish();
}

void BootEditorTab::initializeConnections()
{
    connect(m_refreshButton, &QToolButton::clicked, this, [this]()
        {
            refreshBcdEntries();
        });
    connect(m_exportButton, &QToolButton::clicked, this, [this]()
        {
            exportBcdStore();
        });
    connect(m_importButton, &QToolButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("导入 BCD 存储"));
                return;
            }
            importBcdStore();
        });
    connect(m_copyEntryButton, &QToolButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("复制 BCD 启动项"));
                return;
            }
            createCopyFromSelectedEntry();
        });
    connect(m_deleteEntryButton, &QToolButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("删除 BCD 启动项"));
                return;
            }
            deleteSelectedEntry();
        });
    connect(m_setDefaultButton, &QToolButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("设置默认 BCD 启动项"));
                return;
            }
            setSelectedAsDefaultEntry();
        });
    connect(m_bootOnceButton, &QToolButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("设置一次性 BCD 启动项"));
                return;
            }
            addSelectedToBootSequence();
        });
    connect(m_copyRowButton, &QToolButton::clicked, this, [this]()
        {
            copySelectedRowToClipboard();
        });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString&)
        {
            rebuildEntryTable();
        });
    connect(m_applyEntryButton, &QPushButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("应用 BCD 启动项修改"));
                return;
            }
            applySelectedEntryChanges();
        });
    connect(m_applyBootMgrButton, &QPushButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("应用 BCD 启动管理器修改"));
                return;
            }
            applyBootManagerChanges();
        });
    connect(m_setLegacyForSelectedButton, &QPushButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("设置 Legacy 启动菜单策略"));
                return;
            }
            setLegacyBootForSelectedEntry();
        });
    connect(m_setLegacyForDefaultButton, &QPushButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("设置默认项 Legacy 启动菜单策略"));
                return;
            }
            setLegacyBootForDefaultEntry();
        });
    connect(m_setStandardForSelectedButton, &QPushButton::clicked, this, [this]()
        {
            if (!ks::ui::isCurrentProcessElevated())
            {
                (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("设置 Standard 启动菜单策略"));
                return;
            }
            setStandardBootForSelectedEntry();
        });
    connect(m_reloadOneButton, &QPushButton::clicked, this, [this]()
        {
            syncEditorFromSelection();
        });
    connect(m_runCustomCommandButton, &QPushButton::clicked, this, [this]()
        {
            executeCustomCommand();
        });
    connect(m_entryTable, &QTableWidget::itemSelectionChanged, this, [this]()
        {
            syncEditorFromSelection();
        });
    connect(m_entryTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPos)
        {
            QMenu menu(m_entryTable);
            // 显式填充菜单背景，避免浅色模式下继承透明样式出现黑底。
            menu.setStyleSheet(KswordTheme::ContextMenuStyle());
            QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制当前行"));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            QAction* selectedAction = menu.exec(m_entryTable->viewport()->mapToGlobal(localPos));
            if (selectedAction == copyRowAction)
            {
                copySelectedRowToClipboard();
            }
        });
}

BootEditorTab::~BootEditorTab()
{
    m_commandOperation->close();
}

void BootEditorTab::refreshBcdEntries()
{
    if (m_commandPending) return;
    const BcdCommandResult enumResult = runBcdEdit(
        QStringList{ QStringLiteral("/enum"), QStringLiteral("all"), QStringLiteral("/v") },
        kDefaultCommandTimeoutMs,
        QStringLiteral("枚举全部 BCD 条目"));
    if (enumResult.pending)
    {
        return;
    }
    appendCommandLog(QStringLiteral("bcdedit /enum all /v"), enumResult);

    applyBcdEnumResult(enumResult);
}

void BootEditorTab::applyBcdEnumResult(const BcdCommandResult& enumResult)
{
    if (!enumResult.startSucceeded || enumResult.timeout || enumResult.exitCode != 0)
    {
        // 刷新失败时必须清空缓存：
        // - 防止默认项缓存残留导致后续“默认项操作”误写到过期目标；
        // - 同时清空原始枚举文本，避免界面继续展示旧数据。
        m_entryList.clear();
        m_defaultIdentifierText.clear();
        m_lastEnumRawText.clear();
        rebuildEntryTable();
        clearEditorForNoSelection();

        const QString trimmedErrorText = enumResult.mergedOutputText.trimmed();
        if (trimmedErrorText.contains(QStringLiteral("Access is denied"), Qt::CaseInsensitive)
            || trimmedErrorText.contains(QStringLiteral("拒绝访问"), Qt::CaseInsensitive))
        {
            m_statusLabel->setText(QStringLiteral("状态：读取 BCD 失败（访问被拒绝，请以管理员运行）。"));
        }
        else
        {
            m_statusLabel->setText(QStringLiteral("状态：读取 BCD 失败，请查看原始输出。"));
        }
        return;
    }

    m_lastEnumRawText = enumResult.mergedOutputText;
    m_entryList = parseBcdEnumOutput(enumResult.mergedOutputText);

    // 读取默认启动项：优先从 {bootmgr} 的 default 字段获取。
    m_defaultIdentifierText.clear();
    for (const BcdEntry& entry : m_entryList)
    {
        if (!entry.isBootManager)
        {
            continue;
        }
        m_defaultIdentifierText = readElementValue(entry, QStringList{
            QStringLiteral("default"),
            QStringLiteral("默认")
            });
        break;
    }

    const QString defaultIdLowerText = m_defaultIdentifierText.trimmed().toLower();
    for (BcdEntry& entry : m_entryList)
    {
        if (!defaultIdLowerText.isEmpty()
            && entry.identifierText.trimmed().compare(defaultIdLowerText, Qt::CaseInsensitive) == 0)
        {
            entry.isDefault = true;
        }
    }

    rebuildEntryTable();

    if (m_entryTable->rowCount() > 0)
    {
        m_entryTable->selectRow(0);
    }
    else
    {
        clearEditorForNoSelection();
    }
    updateStatusSummary();
}

void BootEditorTab::rebuildEntryTable()
{
    m_entryTable->setRowCount(0);

    int visibleCount = 0;
    for (int index = 0; index < static_cast<int>(m_entryList.size()); ++index)
    {
        const BcdEntry& entry = m_entryList[static_cast<std::size_t>(index)];
        if (!entryMatchesFilter(entry))
        {
            continue;
        }

        const int rowIndex = visibleCount++;
        m_entryTable->insertRow(rowIndex);

        const QString descriptionText = readElementValue(entry, QStringList{
            QStringLiteral("description"),
            QStringLiteral("描述")
            });
        const QString deviceText = readElementValue(entry, QStringList{
            QStringLiteral("device"),
            QStringLiteral("设备")
            });
        const QString pathText = readElementValue(entry, QStringList{
            QStringLiteral("path"),
            QStringLiteral("路径")
            });
        const QString safeBootText = readElementValue(entry, QStringList{
            QStringLiteral("safeboot")
            });

        QStringList flagTextList;
        if (entry.isBootManager) { flagTextList.push_back(QStringLiteral("BOOTMGR")); }
        if (entry.isCurrent) { flagTextList.push_back(QStringLiteral("当前")); }
        if (entry.isDefault) { flagTextList.push_back(QStringLiteral("默认")); }
        if (!safeBootText.trimmed().isEmpty())
        {
            flagTextList.push_back(buildSafeBootModeText(safeBootText));
        }

        auto makeItem = [index](const QString& text) -> QTableWidgetItem*
            {
                QTableWidgetItem* item = new QTableWidgetItem(text);
                item->setData(Qt::UserRole, index);
                return item;
            };

        m_entryTable->setItem(rowIndex, kColumnIdentifier, makeItem(entry.identifierText));
        m_entryTable->setItem(rowIndex, kColumnDescription, makeItem(descriptionText));
        m_entryTable->setItem(rowIndex, kColumnType, makeItem(entry.objectTypeText));
        m_entryTable->setItem(rowIndex, kColumnDevice, makeItem(deviceText));
        m_entryTable->setItem(rowIndex, kColumnPath, makeItem(pathText));
        m_entryTable->setItem(rowIndex, kColumnFlags, makeItem(flagTextList.join(QStringLiteral(" | "))));
    }

    updateStatusSummary();
}

void BootEditorTab::syncEditorFromSelection()
{
    const BcdEntry* selectedEntry = currentEntry();
    if (selectedEntry == nullptr)
    {
        clearEditorForNoSelection();
        return;
    }

    m_identifierValueLabel->setText(selectedEntry->identifierText);
    m_typeValueLabel->setText(selectedEntry->objectTypeText);
    m_descriptionEdit->setText(readElementValue(*selectedEntry, QStringList{
        QStringLiteral("description"),
        QStringLiteral("描述")
        }));
    m_deviceEdit->setText(readElementValue(*selectedEntry, QStringList{
        QStringLiteral("device"),
        QStringLiteral("设备")
        }));
    m_osDeviceEdit->setText(readElementValue(*selectedEntry, QStringList{
        QStringLiteral("osdevice")
        }));
    m_pathEdit->setText(readElementValue(*selectedEntry, QStringList{
        QStringLiteral("path"),
        QStringLiteral("路径")
        }));
    m_systemRootEdit->setText(readElementValue(*selectedEntry, QStringList{
        QStringLiteral("systemroot")
        }));
    m_localeEdit->setText(readElementValue(*selectedEntry, QStringList{
        QStringLiteral("locale")
        }));

    // bootmenupolicy：没有值时默认“不修改”。
    const QString bootMenuPolicyText = readElementValue(*selectedEntry, QStringList{
        QStringLiteral("bootmenupolicy")
        }).trimmed();
    int policyIndex = m_bootMenuPolicyCombo->findData(bootMenuPolicyText);
    if (policyIndex < 0)
    {
        policyIndex = 0;
    }
    m_bootMenuPolicyCombo->setCurrentIndex(policyIndex);

    // timeout：优先从 bootmgr 中读取，当前条目没有时保留原值。
    int timeoutValue = m_timeoutSpin->value();
    for (const BcdEntry& entry : m_entryList)
    {
        if (!entry.isBootManager)
        {
            continue;
        }
        const QString timeoutText = readElementValue(entry, QStringList{
            QStringLiteral("timeout"),
            QStringLiteral("超时")
            }).trimmed();
        bool convertOk = false;
        const int convertedValue = timeoutText.toInt(&convertOk);
        if (convertOk)
        {
            timeoutValue = convertedValue;
        }
        break;
    }
    m_timeoutSpin->setValue(timeoutValue);

    // 布尔字段：统一按 yes/no、on/off 兼容读取。
    m_testSigningCheck->setChecked(readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("testsigning") },
        false));
    m_noIntegrityCheck->setChecked(readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("nointegritychecks") },
        false));
    m_debugCheck->setChecked(readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("debug") },
        false));
    m_bootLogCheck->setChecked(readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("bootlog") },
        false));
    m_baseVideoCheck->setChecked(readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("basevideo") },
        false));
    m_recoveryEnabledCheck->setChecked(readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("recoveryenabled") },
        true));

    // safeboot 模式：
    // - alternateshell 使用 safeboot + safebootalternateshell 组合判定。
    const QString safeBootText = readElementValue(*selectedEntry, QStringList{
        QStringLiteral("safeboot")
        }).trimmed().toLower();
    const bool alternateShellEnabled = readElementBool(
        *selectedEntry,
        QStringList{ QStringLiteral("safebootalternateshell") },
        false);
    int safeBootIndex = 0;
    if (safeBootText == QStringLiteral("minimal") && alternateShellEnabled)
    {
        safeBootIndex = 3;
    }
    else if (safeBootText == QStringLiteral("minimal"))
    {
        safeBootIndex = 1;
    }
    else if (safeBootText == QStringLiteral("network"))
    {
        safeBootIndex = 2;
    }
    m_safeBootCombo->setCurrentIndex(safeBootIndex);

    m_rawOutputEdit->setPlainText(selectedEntry->rawBlockText);
}

void BootEditorTab::clearEditorForNoSelection()
{
    m_identifierValueLabel->setText(QStringLiteral("-"));
    m_typeValueLabel->setText(QStringLiteral("-"));
    m_descriptionEdit->clear();
    m_deviceEdit->clear();
    m_osDeviceEdit->clear();
    m_pathEdit->clear();
    m_systemRootEdit->clear();
    m_localeEdit->clear();
    m_bootMenuPolicyCombo->setCurrentIndex(0);
    m_safeBootCombo->setCurrentIndex(0);
    m_testSigningCheck->setChecked(false);
    m_noIntegrityCheck->setChecked(false);
    m_debugCheck->setChecked(false);
    m_bootLogCheck->setChecked(false);
    m_baseVideoCheck->setChecked(false);
    m_recoveryEnabledCheck->setChecked(true);
    m_rawOutputEdit->clear();
}

void BootEditorTab::updateStatusSummary()
{
    const int totalCount = static_cast<int>(m_entryList.size());
    const int visibleCount = m_entryTable->rowCount();
    QString statusText = QStringLiteral("状态：总条目 %1，筛选后 %2")
        .arg(totalCount)
        .arg(visibleCount);

    if (!m_defaultIdentifierText.trimmed().isEmpty())
    {
        statusText += QStringLiteral("，默认项 %1").arg(m_defaultIdentifierText.trimmed());
    }
    m_statusLabel->setText(statusText);
}

