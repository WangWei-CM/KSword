#include "ProcessDetailWindow.InternalCommon.h"

using namespace process_detail_window_internal;

// ============================================================
// ProcessDetailWindow.BaseAndUi.cpp
// 作用：
// - 负责构造/基础数据合并、多个 Tab 的 UI 初始化以及基础信号连接。
// - 聚焦“窗口骨架与静态展示数据”逻辑。
// ============================================================

namespace
{
    QString detailProcessFieldSourceText(const std::uint32_t sourceValue)
    {
        // sourceValue 用途：共享协议中的 Phase-2 字段来源枚举。
        // 返回值：直接给详情页展示的稳定文本。
        switch (sourceValue)
        {
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_PUBLIC_API:
            return QStringLiteral("Public API");
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_SYSTEM_INFORMER_DYNDATA:
            return QStringLiteral("System Informer DynData");
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_RUNTIME_PATTERN:
            return QStringLiteral("Runtime pattern");
        case KSWORD_ARK_PROCESS_FIELD_SOURCE_PDB_PROFILE:
            return QStringLiteral("PDB profile");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    QString detailProcessR0StatusText(const std::uint32_t statusValue)
    {
        // statusValue 用途：R0 枚举行的扩展读取总体状态。
        // 返回值：详情页状态文本，便于和 ProcessDock 列保持一致。
        switch (statusValue)
        {
        case KSWORD_ARK_PROCESS_R0_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_PROCESS_R0_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_PROCESS_R0_STATUS_DYNDATA_MISSING:
            return QStringLiteral("DynData missing");
        case KSWORD_ARK_PROCESS_R0_STATUS_READ_FAILED:
            return QStringLiteral("Read failed");
        default:
            return QStringLiteral("Unavailable");
        }
    }

    QString detailProcessByteHexText(const std::uint8_t byteValue)
    {
        // byteValue 用途：Protection/SignatureLevel 等单字节内核字段。
        // 返回值：0xNN 大写十六进制文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(byteValue), 2, 16, QChar('0'))
            .toUpper();
    }

    QString detailProcessOffsetText(const std::uint32_t offsetValue)
    {
        // offsetValue 用途：EPROCESS 字段偏移。
        // 返回值：不可用时明确显示 Unavailable，可用时显示 0xNN。
        if (offsetValue == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE || offsetValue == 0x0000FFFFUL)
        {
            return QStringLiteral("Unavailable");
        }
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(offsetValue), 0, 16)
            .toUpper();
    }

    QString detailProcessPointerText(
        const QString& availableLabel,
        const bool available,
        const std::uint64_t addressValue,
        const std::uint32_t sourceValue)
    {
        // availableLabel 用途：传入 HandleTable/SectionObject 等领域语义。
        // 返回值：先展示“available”结论，再附带地址和来源。
        if (!available)
        {
            return QStringLiteral("Unavailable (%1)").arg(detailProcessFieldSourceText(sourceValue));
        }
        if (addressValue == 0U)
        {
            return QStringLiteral("%1: null (%2)")
                .arg(availableLabel)
                .arg(detailProcessFieldSourceText(sourceValue));
        }
        const QString addressText = QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 0, 16)
            .toUpper();
        return QStringLiteral("%1: 0x%2 (%3)")
            .arg(availableLabel)
            .arg(addressText.mid(2))
            .arg(detailProcessFieldSourceText(sourceValue));
    }

    QString detailProcessCapabilityText(const std::uint64_t capabilityMask)
    {
        // capabilityMask 用途：R0 枚举时附带的 DynData capability 快照。
        // 返回值：十六进制位图 + Phase-2 关心的能力名称。
        QStringList capabilityNames;
        if ((capabilityMask & KSW_CAP_PROCESS_OBJECT_TABLE) != 0U)
        {
            capabilityNames << QStringLiteral("ProcessObjectTable");
        }
        if ((capabilityMask & KSW_CAP_SECTION_CONTROL_AREA) != 0U)
        {
            capabilityNames << QStringLiteral("SectionControlArea");
        }
        if ((capabilityMask & KSW_CAP_PROCESS_PROTECTION_PATCH) != 0U)
        {
            capabilityNames << QStringLiteral("ProcessProtectionPatch");
        }
        if (capabilityNames.isEmpty())
        {
            capabilityNames << QStringLiteral("None/Unavailable");
        }
        const QString maskText = QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(capabilityMask), 0, 16)
            .toUpper();
        return QStringLiteral("%1 (%2)")
            .arg(maskText)
            .arg(capabilityNames.join(QStringLiteral(", ")));
    }
}

ProcessDetailWindow::ProcessDetailWindow(const ks::process::ProcessRecord& baseRecord, QWidget* parent)
    : QWidget(parent)
    , m_baseRecord(baseRecord)
{
    // 构造入口日志：记录目标 PID 与 identity 关键字段。
    kLogEvent ctorStartEvent;
    info << ctorStartEvent
        << "[ProcessDetailWindow] 构造开始, pid="
        << m_baseRecord.pid
        << ", createTime100ns="
        << m_baseRecord.creationTime100ns
        << eol;

    // 详情窗口是独立顶层窗口：非 Dock、非模态，不阻塞主界面。
    setWindowFlag(Qt::Window, true);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1160, 760);

    // identity 用于日志和窗口复用定位。
    m_identityKey = ks::process::BuildProcessIdentityKey(
        m_baseRecord.pid,
        m_baseRecord.creationTime100ns);

    // 构造阶段不做同步静态详情查询：
    // - QueryProcessStaticDetailByPid 会读取命令行、令牌、签名等慢字段；
    // - WinVerifyTrust 在证书链/网络策略异常时可能明显阻塞 UI；
    // - 打开窗口必须先返回事件循环，缺失字段交给后台任务补齐。
    const bool needStaticQuery =
        m_baseRecord.imagePath.empty() ||
        m_baseRecord.commandLine.empty() ||
        m_baseRecord.userName.empty() ||
        m_baseRecord.signatureState.empty() ||
        m_baseRecord.signatureState == "Pending";
    if (needStaticQuery && m_baseRecord.pid != 0)
    {
        kLogEvent ctorStaticQueryDeferredEvent;
        info << ctorStaticQueryDeferredEvent
            << "[ProcessDetailWindow] 构造阶段跳过同步静态查询，改为后台补齐, pid="
            << m_baseRecord.pid
            << eol;
    }

    // 按“建 UI -> 连信号 -> 填初值 -> 首次异步刷新”顺序初始化。
    initializeUi();
    initializeConnections();
    refreshDetailTabTexts();
    requestAsyncStaticDetailRefresh(true);
    requestInitialRefreshForCurrentTab();

    // 构造结束日志：标记窗口初始化链路完成。
    kLogEvent ctorFinishEvent;
    info << ctorFinishEvent
        << "[ProcessDetailWindow] 构造完成, pid="
        << m_baseRecord.pid
        << ", identity="
        << m_identityKey
        << eol;
}

void ProcessDetailWindow::updateBaseRecord(const ks::process::ProcessRecord& baseRecord)
{
    // 更新入口日志：记录新快照 PID 与旧 identity。
    kLogEvent updateRecordStartEvent;
    info << updateRecordStartEvent
        << "[ProcessDetailWindow] updateBaseRecord: 开始更新, incomingPid="
        << baseRecord.pid
        << ", oldIdentity="
        << m_identityKey
        << eol;

    const std::string oldIdentityKey = m_identityKey;

    // 外部推送新快照时：
    // 1) 先保留已有的“已补齐字段”；
    // 2) 再合并新快照；
    // 3) 必要时补查静态详情，避免字段被空值覆盖。
    ks::process::ProcessRecord mergedRecord = baseRecord;
    if (mergedRecord.imagePath.empty()) mergedRecord.imagePath = m_baseRecord.imagePath;
    if (mergedRecord.commandLine.empty()) mergedRecord.commandLine = m_baseRecord.commandLine;
    if (mergedRecord.userName.empty()) mergedRecord.userName = m_baseRecord.userName;
    if (mergedRecord.startTimeText.empty()) mergedRecord.startTimeText = m_baseRecord.startTimeText;
    if (mergedRecord.signatureState.empty()) mergedRecord.signatureState = m_baseRecord.signatureState;
    if (mergedRecord.signaturePublisher.empty()) mergedRecord.signaturePublisher = m_baseRecord.signaturePublisher;
    if (mergedRecord.r0FieldFlags == 0U) mergedRecord.r0FieldFlags = m_baseRecord.r0FieldFlags;
    if (mergedRecord.r0ImagePath.empty()) mergedRecord.r0ImagePath = m_baseRecord.r0ImagePath;
    if (mergedRecord.r0Status == KSWORD_ARK_PROCESS_R0_STATUS_UNAVAILABLE) mergedRecord.r0Status = m_baseRecord.r0Status;
    if (mergedRecord.r0DynDataCapabilityMask == 0U) mergedRecord.r0DynDataCapabilityMask = m_baseRecord.r0DynDataCapabilityMask;
    if (mergedRecord.r0ProtectionSource == KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE) mergedRecord.r0ProtectionSource = m_baseRecord.r0ProtectionSource;
    if (mergedRecord.r0SignatureLevelSource == KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE) mergedRecord.r0SignatureLevelSource = m_baseRecord.r0SignatureLevelSource;
    if (mergedRecord.r0SectionSignatureLevelSource == KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE) mergedRecord.r0SectionSignatureLevelSource = m_baseRecord.r0SectionSignatureLevelSource;
    if (mergedRecord.r0ObjectTableSource == KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE) mergedRecord.r0ObjectTableSource = m_baseRecord.r0ObjectTableSource;
    if (mergedRecord.r0SectionObjectSource == KSWORD_ARK_PROCESS_FIELD_SOURCE_UNAVAILABLE) mergedRecord.r0SectionObjectSource = m_baseRecord.r0SectionObjectSource;
    if (mergedRecord.r0ProtectionOffset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE) mergedRecord.r0ProtectionOffset = m_baseRecord.r0ProtectionOffset;
    if (mergedRecord.r0SignatureLevelOffset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE) mergedRecord.r0SignatureLevelOffset = m_baseRecord.r0SignatureLevelOffset;
    if (mergedRecord.r0SectionSignatureLevelOffset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE) mergedRecord.r0SectionSignatureLevelOffset = m_baseRecord.r0SectionSignatureLevelOffset;
    if (mergedRecord.r0ObjectTableOffset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE) mergedRecord.r0ObjectTableOffset = m_baseRecord.r0ObjectTableOffset;
    if (mergedRecord.r0SectionObjectOffset == KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE) mergedRecord.r0SectionObjectOffset = m_baseRecord.r0SectionObjectOffset;
    if (mergedRecord.r0ObjectTableAddress == 0U) mergedRecord.r0ObjectTableAddress = m_baseRecord.r0ObjectTableAddress;
    if (mergedRecord.r0SectionObjectAddress == 0U) mergedRecord.r0SectionObjectAddress = m_baseRecord.r0SectionObjectAddress;
    mergedRecord.r0Protection = (mergedRecord.r0FieldFlags != 0U) ? mergedRecord.r0Protection : m_baseRecord.r0Protection;
    mergedRecord.r0SignatureLevel = (mergedRecord.r0FieldFlags != 0U) ? mergedRecord.r0SignatureLevel : m_baseRecord.r0SignatureLevel;
    mergedRecord.r0SectionSignatureLevel = (mergedRecord.r0FieldFlags != 0U) ? mergedRecord.r0SectionSignatureLevel : m_baseRecord.r0SectionSignatureLevel;
    mergedRecord.signatureTrusted = mergedRecord.signatureTrusted || m_baseRecord.signatureTrusted;

    const bool needStaticQuery =
        mergedRecord.imagePath.empty() ||
        mergedRecord.commandLine.empty() ||
        mergedRecord.userName.empty() ||
        mergedRecord.signatureState.empty() ||
        mergedRecord.signatureState == "Pending";
    const bool shouldTryStaticBackgroundRefresh =
        needStaticQuery &&
        mergedRecord.pid != 0 &&
        !m_staticDetailRefreshing &&
        !m_staticDetailRefreshAttempted;
    if (needStaticQuery && mergedRecord.pid != 0)
    {
        // updateBaseRecord 可能由 ProcessDock 周期刷新触发。
        // 这里不能同步调用 QueryProcessStaticDetailByPid，否则打开详情窗口后每轮刷新都可能卡 UI。
        kLogEvent updateRecordStaticDeferredEvent;
        dbg << updateRecordStaticDeferredEvent
            << "[ProcessDetailWindow] updateBaseRecord: 静态信息缺失，保留现有值并等待后台补齐, pid="
            << mergedRecord.pid
            << eol;
    }

    m_baseRecord = mergedRecord;
    m_identityKey = ks::process::BuildProcessIdentityKey(
        m_baseRecord.pid,
        m_baseRecord.creationTime100ns);
    const bool identityChanged = m_identityKey != oldIdentityKey;
    if (identityChanged)
    {
        // 同一窗口如果被复用于新 identity，需要重置一次性后台刷新状态。
        // 否则旧进程的首刷标记会阻止新进程数据按需加载。
        m_staticDetailRefreshing = false;
        m_staticDetailRefreshAttempted = false;
        ++m_staticDetailRefreshTicket;
        m_threadInspectInitialRefreshStarted = false;
        m_moduleInitialRefreshStarted = false;
        m_tokenInitialRefreshStarted = false;
        m_tokenSwitchInitialRefreshStarted = false;
        m_sectionInfoInitialRefreshStarted = false;
        m_hotkeyInitialRefreshStarted = false;
        m_keyboardInitialRefreshStarted = false;
        m_pebInitialRefreshStarted = false;
        ++m_hotkeyRefreshTicket;
        ++m_keyboardRefreshTicket;
    }
    refreshDetailTabTexts();
    if (shouldTryStaticBackgroundRefresh || identityChanged)
    {
        requestAsyncStaticDetailRefresh(true);
    }
    requestInitialRefreshForCurrentTab();

    // 更新结束日志：输出新 identity 与关键字段状态。
    kLogEvent updateRecordFinishEvent;
    info << updateRecordFinishEvent
        << "[ProcessDetailWindow] updateBaseRecord: 完成, pid="
        << m_baseRecord.pid
        << ", newIdentity="
        << m_identityKey
        << ", signatureState="
        << m_baseRecord.signatureState
        << eol;
}

std::uint32_t ProcessDetailWindow::pid() const
{
    return m_baseRecord.pid;
}

void ProcessDetailWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);

    // 主题切换时立即重建内部样式，避免旧主题颜色残留。
    if (event == nullptr)
    {
        return;
    }

    const bool isThemeEvent =
        (event->type() == QEvent::PaletteChange) ||
        (event->type() == QEvent::ApplicationPaletteChange) ||
        (event->type() == QEvent::StyleChange);
    if (!isThemeEvent)
    {
        return;
    }

    applyThemeStyle();
    refreshDetailTabTexts();

    if (m_threadInspectStatusLabel != nullptr)
    {
        updateThreadInspectStatusLabel(m_threadInspectStatusLabel->text(), m_threadInspectRefreshing);
    }

    if (m_moduleStatusLabel != nullptr)
    {
        updateModuleStatusLabel(m_moduleStatusLabel->text(), m_moduleRefreshing);
        if (!m_moduleRefreshing && m_moduleRecords.empty())
        {
            m_moduleStatusLabel->setStyleSheet(buildStateLabelStyle(statusErrorColor(), 700));
        }
    }

    if (m_tokenStatusLabel != nullptr)
    {
        m_tokenStatusLabel->setStyleSheet(
            m_tokenRefreshing
            ? buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700)
            : buildStateLabelStyle(statusIdleColor(), 600));
    }

    if (m_tokenSwitchStatusLabel != nullptr)
    {
        const QString statusText = m_tokenSwitchStatusLabel->text();
        if (statusText.contains(QStringLiteral("失败")) || statusText.contains(QStringLiteral("失败项")))
        {
            m_tokenSwitchStatusLabel->setStyleSheet(buildStateLabelStyle(statusWarningColor(), 700));
        }
        else if (statusText.contains(QStringLiteral("完成")) || statusText.contains(QStringLiteral("成功")))
        {
            m_tokenSwitchStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
        }
        else
        {
            m_tokenSwitchStatusLabel->setStyleSheet(buildStateLabelStyle(statusSecondaryColor(), 600));
        }
    }

    if (m_pebStatusLabel != nullptr)
    {
        const bool hasDiagnostic = m_pebStatusLabel->text().contains(QStringLiteral(" | "));
        if (m_pebRefreshing)
        {
            m_pebStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
        }
        else if (hasDiagnostic)
        {
            m_pebStatusLabel->setStyleSheet(buildStateLabelStyle(statusWarningColor(), 700));
        }
        else
        {
            m_pebStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
        }
    }

    if (m_hotkeyStatusLabel != nullptr)
    {
        const QString statusText = m_hotkeyStatusLabel->text();
        if (m_hotkeyRefreshing)
        {
            m_hotkeyStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
        }
        else if (statusText.contains(QStringLiteral("无公开API")) || statusText.contains(QStringLiteral("失败")))
        {
            m_hotkeyStatusLabel->setStyleSheet(buildStateLabelStyle(statusWarningColor(), 700));
        }
        else
        {
            m_hotkeyStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
        }
    }

    if (m_keyboardStatusLabel != nullptr)
    {
        const QString statusText = m_keyboardStatusLabel->text();
        if (m_keyboardRefreshing)
        {
            m_keyboardStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
        }
        else if (statusText.contains(QStringLiteral("失败")) || statusText.contains(QStringLiteral("不可用")))
        {
            m_keyboardStatusLabel->setStyleSheet(buildStateLabelStyle(statusWarningColor(), 700));
        }
        else
        {
            m_keyboardStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
        }
    }

    refreshKernelObjectTabTexts();
}

void ProcessDetailWindow::applyThemeStyle()
{
    if (m_themeStyleApplying)
    {
        return;
    }
    m_themeStyleApplying = true;

    // 显式设置窗口调色板：
    // - Win11 下必须手动强制窗口背景，避免被系统自动接管为亮色。
    const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
    QPalette themedPalette = (qApp != nullptr) ? qApp->palette() : palette();
    themedPalette.setColor(QPalette::Window, darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255));
    themedPalette.setColor(QPalette::WindowText, darkModeEnabled ? QColor(255, 255, 255) : QColor(0, 0, 0));
    themedPalette.setColor(QPalette::Base, darkModeEnabled ? QColor(22, 22, 22) : QColor(255, 255, 255));
    themedPalette.setColor(QPalette::AlternateBase, darkModeEnabled ? QColor(30, 30, 30) : QColor(247, 249, 252));
    themedPalette.setColor(QPalette::Text, darkModeEnabled ? QColor(255, 255, 255) : QColor(0, 0, 0));
    themedPalette.setColor(QPalette::Mid, darkModeEnabled ? QColor(86, 86, 86) : QColor(180, 180, 180));
    themedPalette.setColor(QPalette::Highlight, KswordTheme::PrimaryBlueColor);
    themedPalette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));

    setPalette(themedPalette);
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(buildProcessDetailRootStyle());

    // 子页面也强制设置背景，避免 tab 内容区域出现白底。
    const std::vector<QWidget*> tabPageList{
        m_detailTab,
        m_threadTab,
        m_actionTab,
        m_moduleTab,
        m_tokenTab,
        m_tokenSwitchTab,
        m_kernelObjectTab,
        m_hotkeyTab,
        m_keyboardTab,
        m_pebTab
    };
    for (QWidget* tabPage : tabPageList)
    {
        if (tabPage == nullptr)
        {
            continue;
        }
        tabPage->setPalette(themedPalette);
        tabPage->setAutoFillBackground(true);
        tabPage->setAttribute(Qt::WA_StyledBackground, true);
    }

    // 表头统一用主题文本色，杜绝深色模式下黑字问题。
    const QString headerStyle = QStringLiteral(
        "QHeaderView::section {"
        "  color:%1;"
        "  background:%2;"
        "  border:1px solid %3;"
        "  padding:4px;"
        "  font-weight:600;"
        "}")
        .arg(KswordTheme::TextPrimaryHex())
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex());

    if (m_threadInspectTable != nullptr && m_threadInspectTable->horizontalHeader() != nullptr)
    {
        m_threadInspectTable->horizontalHeader()->setStyleSheet(headerStyle);
    }
    if (m_moduleTable != nullptr && m_moduleTable->header() != nullptr)
    {
        m_moduleTable->header()->setStyleSheet(headerStyle);
    }
    if (m_hotkeyTable != nullptr && m_hotkeyTable->horizontalHeader() != nullptr)
    {
        m_hotkeyTable->horizontalHeader()->setStyleSheet(headerStyle);
    }
    if (m_keyboardHotkeyTable != nullptr && m_keyboardHotkeyTable->horizontalHeader() != nullptr)
    {
        m_keyboardHotkeyTable->horizontalHeader()->setStyleSheet(headerStyle);
    }
    if (m_keyboardHookTable != nullptr && m_keyboardHookTable->horizontalHeader() != nullptr)
    {
        m_keyboardHookTable->horizontalHeader()->setStyleSheet(headerStyle);
    }

    if (m_signatureCheckBox != nullptr)
    {
        m_signatureCheckBox->setStyleSheet(QStringLiteral(
            "QCheckBox { color:%1; font-weight:600; }"
            "QCheckBox::indicator { border:1px solid %2; background:%3; width:13px; height:13px; }"
            "QCheckBox::indicator:checked { background:%4; border:1px solid %4; }")
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::PrimaryBlueHex));
    }

    if (m_tokenRawInfoClassCombo != nullptr || m_tokenRawInputModeCombo != nullptr)
    {
        const QString comboStyle = QStringLiteral(
            "QComboBox {"
            "  border: 1px solid %1;"
            "  border-radius: 4px;"
            "  padding: 3px 8px;"
            "  color: %2;"
            "  background: %3;"
            "}"
            "QComboBox:hover {"
            "  border-color: %4;"
            "}"
            "QComboBox::drop-down {"
            "  border:none;"
            "  width:20px;"
            "}"
            "QComboBox QAbstractItemView {"
            "  background:%3;"
            "  color:%2;"
            "  border:1px solid %1;"
            "  selection-background-color:%4;"
            "  selection-color:#FFFFFF;"
            "}")
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::PrimaryBlueHex);
        if (m_tokenRawInfoClassCombo != nullptr)
        {
            m_tokenRawInfoClassCombo->setStyleSheet(comboStyle);
        }
        if (m_tokenRawInputModeCombo != nullptr)
        {
            m_tokenRawInputModeCombo->setStyleSheet(comboStyle);
        }
    }

    m_themeStyleApplying = false;
}

void ProcessDetailWindow::initializeUi()
{
    // UI 初始化入口日志：用于排查窗口初始化顺序。
    kLogEvent initUiEvent;
    info << initUiEvent
        << "[ProcessDetailWindow] initializeUi: 创建根布局和Tab容器。"
        << eol;

    // 根窗口对象名用于样式选择器精确命中。
    setObjectName(QStringLiteral("ProcessDetailWindowRoot"));

    // 根布局只放一个 TabWidget，满足 4.2 的三分栏结构。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_tabWidget, 1);

    // 创建各详情页面并分别初始化。
    m_detailTab = new QWidget(m_tabWidget);
    m_threadTab = new QWidget(m_tabWidget);
    m_actionTab = new QWidget(m_tabWidget);
    m_moduleTab = new QWidget(m_tabWidget);
    m_tokenTab = new QWidget(m_tabWidget);
    m_tokenSwitchTab = new QWidget(m_tabWidget);
    m_kernelObjectTab = new QWidget(m_tabWidget);
    m_hotkeyTab = new QWidget(m_tabWidget);
    m_keyboardTab = new QWidget(m_tabWidget);
    m_pebTab = new QWidget(m_tabWidget);

    m_detailTab->setObjectName(QStringLiteral("ProcessDetailTab_Detail"));
    m_threadTab->setObjectName(QStringLiteral("ProcessDetailTab_Thread"));
    m_actionTab->setObjectName(QStringLiteral("ProcessDetailTab_Action"));
    m_moduleTab->setObjectName(QStringLiteral("ProcessDetailTab_Module"));
    m_tokenTab->setObjectName(QStringLiteral("ProcessDetailTab_Token"));
    m_tokenSwitchTab->setObjectName(QStringLiteral("ProcessDetailTab_TokenSwitch"));
    m_kernelObjectTab->setObjectName(QStringLiteral("ProcessDetailTab_KernelObject"));
    m_hotkeyTab->setObjectName(QStringLiteral("ProcessDetailTab_Hotkey"));
    m_keyboardTab->setObjectName(QStringLiteral("ProcessDetailTab_Keyboard"));
    m_pebTab->setObjectName(QStringLiteral("ProcessDetailTab_Peb"));

    initializeDetailTab();
    initializeThreadTab();
    initializeActionTab();
    initializeModuleTab();
    initializeTokenTab();
    initializeTokenSwitchTab();
    initializeKernelObjectTab();
    initializeHotkeyTab();
    initializeKeyboardTab();
    initializePebTab();

    // 为 Tab 指定图标与标题文本。
    m_tabWidget->addTab(m_detailTab, QIcon(":/Icon/process_details.svg"), "详细信息");
    m_tabWidget->addTab(m_threadTab, QIcon(":/Icon/process_tree.svg"), "线程");
    m_tabWidget->addTab(m_actionTab, QIcon(":/Icon/process_priority.svg"), "操作");
    m_tabWidget->addTab(m_moduleTab, QIcon(":/Icon/process_list.svg"), "模块");
    m_tabWidget->addTab(m_tokenTab, QIcon(":/Icon/process_critical.svg"), "令牌");
    m_tabWidget->addTab(m_tokenSwitchTab, QIcon(":/Icon/process_start.svg"), "令牌开关");
    m_tabWidget->addTab(m_kernelObjectTab, QIcon(":/Icon/process_critical.svg"), "内核对象");
    m_tabWidget->addTab(m_hotkeyTab, QIcon(":/Icon/process_hotkey.svg"), "进程热键");
    m_tabWidget->addTab(m_keyboardTab, QIcon(":/Icon/process_hotkey.svg"), "键盘");
    m_tabWidget->addTab(m_pebTab, QIcon(":/Icon/process_tree.svg"), "PEB");
    m_tabWidget->setCurrentWidget(m_detailTab);

    // 所有控件创建完毕后统一套用主题样式。
    applyThemeStyle();

    updateWindowTitle();
}

void ProcessDetailWindow::requestInitialRefreshForCurrentTab()
{
    // 懒加载策略：
    // - 详情页只显示构造时已有的轻量字段；
    // - 线程/模块/令牌/PEB/Section 等重型查询等用户切到对应页后再启动；
    // - 每页自动首刷只执行一次，用户点击刷新按钮仍可手动刷新。
    if (m_tabWidget == nullptr)
    {
        return;
    }

    QWidget* const currentTab = m_tabWidget->currentWidget();
    if (currentTab == nullptr)
    {
        return;
    }

    if (currentTab == m_threadTab)
    {
        if (!m_threadInspectInitialRefreshStarted)
        {
            requestAsyncThreadInspectRefresh();
        }
        return;
    }

    if (currentTab == m_moduleTab)
    {
        if (!m_moduleInitialRefreshStarted)
        {
            requestAsyncModuleRefresh(true);
        }
        return;
    }

    if (currentTab == m_tokenTab)
    {
        if (!m_tokenInitialRefreshStarted)
        {
            requestAsyncTokenRefresh();
        }
        return;
    }

    if (currentTab == m_tokenSwitchTab)
    {
        if (!m_tokenSwitchInitialRefreshStarted)
        {
            refreshTokenSwitchStates();
        }
        return;
    }

    if (currentTab == m_kernelObjectTab)
    {
        if (!m_sectionInfoInitialRefreshStarted)
        {
            requestAsyncSectionRefresh();
        }
        return;
    }

    if (currentTab == m_hotkeyTab)
    {
        if (!m_hotkeyInitialRefreshStarted)
        {
            requestAsyncHotkeyRefresh();
        }
        return;
    }

    if (currentTab == m_keyboardTab)
    {
        if (!m_keyboardInitialRefreshStarted)
        {
            requestAsyncKeyboardRefresh();
        }
        return;
    }

    if (currentTab == m_pebTab && !m_pebInitialRefreshStarted)
    {
        requestAsyncPebRefresh();
    }
}

void ProcessDetailWindow::requestAsyncStaticDetailRefresh(const bool includeSignatureCheck)
{
    // 静态详情补齐防重入：
    // - 周期刷新可能频繁调用 updateBaseRecord；
    // - 同一个窗口只允许一个后台静态详情任务，避免签名校验堆积。
    const std::uint32_t currentPid = m_baseRecord.pid;
    const std::uint64_t currentCreationTime = m_baseRecord.creationTime100ns;
    if (currentPid == 0 || m_staticDetailRefreshing || m_staticDetailRefreshAttempted)
    {
        return;
    }

    const bool needStaticQuery =
        m_baseRecord.imagePath.empty() ||
        m_baseRecord.commandLine.empty() ||
        m_baseRecord.userName.empty() ||
        m_baseRecord.startTimeText.empty() ||
        m_baseRecord.architectureText.empty() ||
        m_baseRecord.priorityText.empty() ||
        m_baseRecord.signatureState.empty() ||
        m_baseRecord.signatureState == "Pending";
    if (!needStaticQuery)
    {
        return;
    }

    m_staticDetailRefreshing = true;
    m_staticDetailRefreshAttempted = true;
    const std::uint64_t ticketValue = ++m_staticDetailRefreshTicket;
    const std::uint32_t pidValue = currentPid;
    const std::uint64_t creationTimeValue = currentCreationTime;
    const std::string identityKeyValue = ks::process::BuildProcessIdentityKey(
        pidValue,
        creationTimeValue);
    QPointer<ProcessDetailWindow> guardThis(this);

    kLogEvent requestStaticDetailEvent;
    info << requestStaticDetailEvent
        << "[ProcessDetailWindow] requestAsyncStaticDetailRefresh: 后台补齐静态详情, pid="
        << pidValue
        << ", includeSignature="
        << (includeSignatureCheck ? "true" : "false")
        << eol;

    QRunnable* refreshTask = QRunnable::create(
        [guardThis, ticketValue, pidValue, creationTimeValue, identityKeyValue, includeSignatureCheck]()
        {
            StaticDetailRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();
            refreshResult.processRecord.pid = pidValue;
            refreshResult.processRecord.creationTime100ns = creationTimeValue;
            refreshResult.processRecord.processName = ks::process::GetProcessNameByPID(pidValue);

            // 动态计数器只补轻量数值；签名校验由 FillProcessStaticDetails 的参数控制。
            ks::process::RefreshProcessDynamicCounters(refreshResult.processRecord);
            if (creationTimeValue != 0 &&
                refreshResult.processRecord.creationTime100ns != 0 &&
                refreshResult.processRecord.creationTime100ns != creationTimeValue)
            {
                // PID 已经复用：丢弃本轮结果，避免把新进程信息写回旧窗口。
                QMetaObject::invokeMethod(
                    guardThis,
                    [guardThis, ticketValue, identityKeyValue]()
                    {
                        if (guardThis == nullptr || guardThis->m_staticDetailRefreshTicket != ticketValue)
                        {
                            return;
                        }
                        const std::string currentIdentityKey = ks::process::BuildProcessIdentityKey(
                            guardThis->m_baseRecord.pid,
                            guardThis->m_baseRecord.creationTime100ns);
                        if (currentIdentityKey == identityKeyValue)
                        {
                            guardThis->m_staticDetailRefreshing = false;
                            guardThis->m_staticDetailRefreshAttempted = true;
                        }
                    },
                    Qt::QueuedConnection);
                return;
            }
            refreshResult.queryOk = ks::process::FillProcessStaticDetails(
                refreshResult.processRecord,
                includeSignatureCheck);
            if (creationTimeValue == 0 && refreshResult.processRecord.creationTime100ns != 0)
            {
                // 轻量开窗记录没有创建时间时，保持 identity 为 PID#0。
                // 这样 ProcessDock 的窗口缓存键不会在后台补齐后漂移。
                refreshResult.processRecord.creationTime100ns = creationTimeValue;
            }
            if (!refreshResult.queryOk)
            {
                refreshResult.diagnosticText = QStringLiteral("静态详情读取失败或权限不足");
            }
            if (refreshResult.processRecord.processName.empty())
            {
                refreshResult.processRecord.processName = "PID_" + std::to_string(pidValue);
            }

            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, ticketValue, identityKeyValue, includeSignatureCheck, refreshResult]()
                {
                    if (guardThis == nullptr || guardThis->m_staticDetailRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    const std::string currentIdentityKey = ks::process::BuildProcessIdentityKey(
                        guardThis->m_baseRecord.pid,
                        guardThis->m_baseRecord.creationTime100ns);
                    if (currentIdentityKey != identityKeyValue)
                    {
                        // 目标进程 identity 已变化时丢弃旧结果，并允许新 identity 重新排队补齐。
                        guardThis->m_staticDetailRefreshing = false;
                        guardThis->m_staticDetailRefreshAttempted = false;
                        guardThis->requestAsyncStaticDetailRefresh(includeSignatureCheck);
                        return;
                    }
                    guardThis->applyStaticDetailRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });
    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyStaticDetailRefreshResult(const StaticDetailRefreshResult& refreshResult)
{
    // 后台静态详情回填：
    // - 只合并有效字段，避免权限不足结果清空用户当前看到的缓存字段；
    // - 保留 R0 扩展字段，因为它们可能来自进程列表或驱动枚举。
    m_staticDetailRefreshing = false;
    if (refreshResult.processRecord.pid != m_baseRecord.pid)
    {
        return;
    }

    const ks::process::ProcessRecord& queriedRecord = refreshResult.processRecord;
    if (!queriedRecord.processName.empty()) m_baseRecord.processName = queriedRecord.processName;
    if (!queriedRecord.imagePath.empty()) m_baseRecord.imagePath = queriedRecord.imagePath;
    if (!queriedRecord.commandLine.empty()) m_baseRecord.commandLine = queriedRecord.commandLine;
    if (!queriedRecord.userName.empty()) m_baseRecord.userName = queriedRecord.userName;
    if (!queriedRecord.startTimeText.empty()) m_baseRecord.startTimeText = queriedRecord.startTimeText;
    if (!queriedRecord.architectureText.empty()) m_baseRecord.architectureText = queriedRecord.architectureText;
    if (!queriedRecord.priorityText.empty()) m_baseRecord.priorityText = queriedRecord.priorityText;
    if (!queriedRecord.signatureState.empty()) m_baseRecord.signatureState = queriedRecord.signatureState;
    if (!queriedRecord.signaturePublisher.empty()) m_baseRecord.signaturePublisher = queriedRecord.signaturePublisher;
    m_baseRecord.signatureTrusted = queriedRecord.signatureTrusted;
    m_baseRecord.isAdmin = queriedRecord.isAdmin;
    if (queriedRecord.parentPid != 0) m_baseRecord.parentPid = queriedRecord.parentPid;
    if (queriedRecord.sessionId != 0) m_baseRecord.sessionId = queriedRecord.sessionId;
    if (queriedRecord.threadCount != 0) m_baseRecord.threadCount = queriedRecord.threadCount;
    if (queriedRecord.handleCount != 0) m_baseRecord.handleCount = queriedRecord.handleCount;
    if (queriedRecord.creationTime100ns != 0) m_baseRecord.creationTime100ns = queriedRecord.creationTime100ns;
    if (queriedRecord.staticDetailsReady) m_baseRecord.staticDetailsReady = true;

    m_identityKey = ks::process::BuildProcessIdentityKey(
        m_baseRecord.pid,
        m_baseRecord.creationTime100ns);
    refreshDetailTabTexts();

    kLogEvent applyStaticDetailEvent;
    (refreshResult.queryOk ? info : warn) << applyStaticDetailEvent
        << "[ProcessDetailWindow] applyStaticDetailRefreshResult: 完成, pid="
        << m_baseRecord.pid
        << ", queryOk="
        << (refreshResult.queryOk ? "true" : "false")
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << ", diagnostic="
        << refreshResult.diagnosticText.toStdString()
        << eol;
}

void ProcessDetailWindow::initializeDetailTab()
{
    // 详情页初始化日志：确认详细信息面板构建开始。
    kLogEvent initDetailTabEvent;
    info << initDetailTabEvent
        << "[ProcessDetailWindow] initializeDetailTab: 构建详细信息页面。"
        << eol;

    m_detailLayout = new QVBoxLayout(m_detailTab);
    m_detailLayout->setContentsMargins(6, 6, 6, 6);
    m_detailLayout->setSpacing(8);

    // 顶部：40px 图标 + 进程名与 PID。
    QHBoxLayout* titleLayout = new QHBoxLayout();
    m_processIconLabel = new QLabel(m_detailTab);
    m_processIconLabel->setFixedSize(40, 40);
    m_processTitleLabel = new QLabel(m_detailTab);
    m_processTitleLabel->setStyleSheet(
        QStringLiteral("font-size:18px; font-weight:700; color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    titleLayout->addWidget(m_processIconLabel, 0, Qt::AlignTop);
    titleLayout->addWidget(m_processTitleLabel, 1);
    titleLayout->addStretch(1);
    m_detailLayout->addLayout(titleLayout);

    // 路径行：只读输入框 + 复制 + 打开文件夹。
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel("程序路径:", m_detailTab));
    m_pathLineEdit = new QLineEdit(m_detailTab);
    m_pathLineEdit->setReadOnly(true);
    m_copyPathButton = new QPushButton(QIcon(":/Icon/process_copy_cell.svg"), "复制", m_detailTab);
    m_openPathFolderButton = new QPushButton(QIcon(":/Icon/process_open_folder.svg"), "打开文件夹", m_detailTab);
    pathLayout->addWidget(m_pathLineEdit, 1);
    pathLayout->addWidget(m_copyPathButton);
    pathLayout->addWidget(m_openPathFolderButton);
    m_detailLayout->addLayout(pathLayout);

    // 命令行行：只读输入框 + 复制。
    QHBoxLayout* commandLayout = new QHBoxLayout();
    commandLayout->addWidget(new QLabel("启动命令行:", m_detailTab));
    m_commandLineEdit = new QLineEdit(m_detailTab);
    m_commandLineEdit->setReadOnly(true);
    m_copyCommandButton = new QPushButton(QIcon(":/Icon/process_copy_cell.svg"), "复制", m_detailTab);
    commandLayout->addWidget(m_commandLineEdit, 1);
    commandLayout->addWidget(m_copyCommandButton);
    m_detailLayout->addLayout(commandLayout);

    // 父进程行：20px 图标 + 名称 PID + 转到父进程按钮（存在时显示）。
    QHBoxLayout* parentLayout = new QHBoxLayout();
    m_parentIconLabel = new QLabel(m_detailTab);
    m_parentIconLabel->setFixedSize(20, 20);
    m_parentInfoLabel = new QLabel(m_detailTab);
    m_parentInfoLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    m_openHandleDockButton = new QPushButton(QIcon(":/Icon/process_list.svg"), "", m_detailTab);
    m_openHandleDockButton->setToolTip(QStringLiteral("跳转到句柄 Dock，并按当前 PID 过滤"));
    m_openHandleDockButton->setFixedSize(28, 28);
    m_gotoParentButton = new QPushButton(QIcon(":/Icon/process_details.svg"), "转到父进程", m_detailTab);
    m_gotoParentButton->setVisible(false);
    parentLayout->addWidget(new QLabel("父进程:", m_detailTab));
    parentLayout->addWidget(m_parentIconLabel);
    parentLayout->addWidget(m_parentInfoLabel, 1);
    parentLayout->addWidget(m_openHandleDockButton);
    parentLayout->addWidget(m_gotoParentButton);
    m_detailLayout->addLayout(parentLayout);

    // 详细字段区域：尽可能展示更多可得信息。
    QGroupBox* detailGroup = new QGroupBox("更多进程详细数据", m_detailTab);
    QFormLayout* detailFormLayout = new QFormLayout(detailGroup);
    detailFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    detailFormLayout->setHorizontalSpacing(18);
    detailFormLayout->setVerticalSpacing(6);

    m_detailStartTimeValue = new QLabel(detailGroup);
    m_detailUserValue = new QLabel(detailGroup);
    m_detailAdminValue = new QLabel(detailGroup);
    m_detailArchitectureValue = new QLabel(detailGroup);
    m_detailPriorityValue = new QLabel(detailGroup);
    m_detailSessionValue = new QLabel(detailGroup);
    m_detailThreadCountValue = new QLabel(detailGroup);
    m_detailHandleCountValue = new QLabel(detailGroup);
    m_detailCpuValue = new QLabel(detailGroup);
    m_detailRamValue = new QLabel(detailGroup);
    m_detailDiskValue = new QLabel(detailGroup);
    m_detailSignatureValue = new QLabel(detailGroup);

    detailFormLayout->addRow("启动时间", m_detailStartTimeValue);
    detailFormLayout->addRow("用户", m_detailUserValue);
    detailFormLayout->addRow("管理员", m_detailAdminValue);
    detailFormLayout->addRow("架构", m_detailArchitectureValue);
    detailFormLayout->addRow("优先级", m_detailPriorityValue);
    detailFormLayout->addRow("Session ID", m_detailSessionValue);
    detailFormLayout->addRow("线程数量", m_detailThreadCountValue);
    detailFormLayout->addRow("句柄数量", m_detailHandleCountValue);
    detailFormLayout->addRow("CPU 占用", m_detailCpuValue);
    detailFormLayout->addRow("RAM 占用", m_detailRamValue);
    detailFormLayout->addRow("DISK 吞吐", m_detailDiskValue);
    detailFormLayout->addRow("数字签名", m_detailSignatureValue);

    m_detailLayout->addWidget(detailGroup);

    m_detailLayout->addStretch(1);

    const QString buttonStyle = buildBlueButtonStyle();
    m_copyPathButton->setStyleSheet(buttonStyle);
    m_openPathFolderButton->setStyleSheet(buttonStyle);
    m_copyCommandButton->setStyleSheet(buttonStyle);
    m_openHandleDockButton->setStyleSheet(buildBlueButtonStyle());
    m_gotoParentButton->setStyleSheet(buttonStyle);
}

void ProcessDetailWindow::initializeThreadTab()
{
    // 线程页初始化日志：把线程枚举与寄存器摘要单独放到独立标签。
    kLogEvent initThreadTabEvent;
    info << initThreadTabEvent
        << "[ProcessDetailWindow] initializeThreadTab: 构建线程信息页面。"
        << eol;

    m_threadLayout = new QVBoxLayout(m_threadTab);
    m_threadLayout->setContentsMargins(6, 6, 6, 6);
    m_threadLayout->setSpacing(8);

    QGroupBox* threadGroup = new QGroupBox("线程枚举与上下文摘要", m_threadTab);
    QVBoxLayout* threadGroupLayout = new QVBoxLayout(threadGroup);
    threadGroupLayout->setContentsMargins(8, 8, 8, 8);
    threadGroupLayout->setSpacing(6);

    // 顶部工具栏：
    // - 刷新按钮保留明确文字，避免仅靠图标无法分辨“线程刷新”语义；
    // - 状态标签放在右侧，持续反馈本轮刷新耗时与诊断信息。
    QHBoxLayout* threadTopBarLayout = new QHBoxLayout();
    m_refreshThreadInspectButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), "刷新线程", threadGroup);
    m_refreshThreadInspectButton->setToolTip("异步刷新线程枚举、TEB、起始地址与寄存器摘要");
    auto* openThreadStackButton = new QPushButton(QIcon(":/Icon/process_threads.svg"), "查看调用栈", threadGroup);
    openThreadStackButton->setToolTip("打开当前选中线程的 Phase-8 调用栈窗口");
    m_threadInspectStatusLabel = new QLabel("● 尚未刷新", threadGroup);
    m_threadInspectStatusLabel->setStyleSheet(
        QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    threadTopBarLayout->addWidget(m_refreshThreadInspectButton);
    threadTopBarLayout->addWidget(openThreadStackButton);
    threadTopBarLayout->addWidget(m_threadInspectStatusLabel, 1);
    threadGroupLayout->addLayout(threadTopBarLayout);

    // 线程表格：
    // - 继续沿用原有列定义和刷新逻辑；
    // - 仅把显示位置从“详细信息页底部”迁移到独立标签。
    m_threadInspectTable = new QTableWidget(threadGroup);
    m_threadInspectTable->setColumnCount(toThreadColumnIndex(ThreadRowColumn::Count));
    m_threadInspectTable->setHorizontalHeaderLabels(ThreadInspectHeaders);
    m_threadInspectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_threadInspectTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_threadInspectTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_threadInspectTable->setAlternatingRowColors(true);
    m_threadInspectTable->verticalHeader()->setVisible(false);
    m_threadInspectTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_threadInspectTable->horizontalHeader()->setStretchLastSection(true);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::ThreadId), 96);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::State), 82);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::Priority), 72);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::SwitchCount), 96);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::StartAddress), 130);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::TebAddress), 130);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::Affinity), 108);
    m_threadInspectTable->setColumnWidth(toThreadColumnIndex(ThreadRowColumn::StackBoundary), 260);
    threadGroupLayout->addWidget(m_threadInspectTable, 1);

    m_threadLayout->addWidget(threadGroup, 1);

    const QString buttonStyle = buildBlueButtonStyle();
    m_refreshThreadInspectButton->setStyleSheet(buttonStyle);
    openThreadStackButton->setStyleSheet(buttonStyle);
    connect(openThreadStackButton, &QPushButton::clicked, this, [this]() {
        openSelectedThreadStackWindow();
        });
    connect(m_threadInspectTable, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
        openSelectedThreadStackWindow();
        });
}

void ProcessDetailWindow::initializeActionTab()
{
    // 操作页初始化日志：确认动作按钮区域构建。
    kLogEvent initActionTabEvent;
    info << initActionTabEvent
        << "[ProcessDetailWindow] initializeActionTab: 构建进程操作页面。"
        << eol;

    m_actionLayout = new QVBoxLayout(m_actionTab);
    m_actionLayout->setContentsMargins(6, 6, 6, 6);
    m_actionLayout->setSpacing(10);

    // buildTextActionButton 作用：
    // - 为操作页生成统一文字按钮；
    // - 输入 buttonText 作为用户直接可见的按钮含义，toolTipText 作为补充解释；
    // - 返回 QPushButton，不使用图标-only 形态，避免操作面板含义不直观。
    const auto buildTextActionButton =
        [](const QString& buttonText, const QString& toolTipText, QWidget* parentWidget) -> QPushButton*
    {
        QPushButton* actionButton = new QPushButton(buttonText, parentWidget);
        actionButton->setMinimumHeight(32);
        actionButton->setMinimumWidth(72);
        actionButton->setToolTip(toolTipText);
        return actionButton;
    };

    // 结束与控制组：
    // - “结束方案”改为下拉选择，避免四个宽按钮铺满整行；
    // - 运行控制与关键进程操作改为紧凑图标按钮，保留 tooltip 解释语义。
    QGroupBox* controlGroup = new QGroupBox("结束与控制", m_actionTab);
    QGridLayout* controlLayout = new QGridLayout(controlGroup);
    controlLayout->setHorizontalSpacing(8);
    controlLayout->setVerticalSpacing(8);

    m_terminateActionCombo = new QComboBox(controlGroup);
    m_terminateActionCombo->addItem(QIcon(":/Icon/process_terminate.svg"), "结束进程(组合方法链)", 2);
    m_terminateActionCombo->addItem(QIcon(":/Icon/process_terminate.svg"), "TerminateProcess", 0);
    m_terminateActionCombo->addItem(QIcon(":/Icon/process_terminate.svg"), "TerminateThread(全部线程)", 1);
    m_terminateActionCombo->setToolTip("选择结束当前进程的执行方案");
    m_executeTerminateActionButton = buildTextActionButton(
        QStringLiteral("执行"),
        QStringLiteral("执行当前选中的结束方案"),
        controlGroup);

    m_suspendProcessButton = buildTextActionButton(
        QStringLiteral("挂起"),
        QStringLiteral("挂起当前进程"),
        controlGroup);
    m_resumeProcessButton = buildTextActionButton(
        QStringLiteral("恢复"),
        QStringLiteral("恢复当前进程"),
        controlGroup);
    m_setCriticalButton = buildTextActionButton(
        QStringLiteral("设为关键"),
        QStringLiteral("把当前进程设为关键进程"),
        controlGroup);
    m_clearCriticalButton = buildTextActionButton(
        QStringLiteral("取消关键"),
        QStringLiteral("取消当前进程的关键进程标记"),
        controlGroup);
    m_priorityCombo = new QComboBox(controlGroup);
    m_priorityCombo->addItem("Idle", 0);
    m_priorityCombo->addItem("Below Normal", 1);
    m_priorityCombo->addItem("Normal", 2);
    m_priorityCombo->addItem("Above Normal", 3);
    m_priorityCombo->addItem("High", 4);
    m_priorityCombo->addItem("Realtime", 5);
    m_priorityCombo->setCurrentIndex(2);
    m_priorityCombo->setToolTip("选择当前进程的新优先级");
    m_applyPriorityButton = buildTextActionButton(
        QStringLiteral("应用"),
        QStringLiteral("应用当前选中的进程优先级"),
        controlGroup);

    controlLayout->addWidget(new QLabel("结束方案", controlGroup), 0, 0);
    controlLayout->addWidget(m_terminateActionCombo, 0, 1, 1, 3);
    controlLayout->addWidget(m_executeTerminateActionButton, 0, 4);
    controlLayout->addWidget(new QLabel("运行控制", controlGroup), 1, 0);
    controlLayout->addWidget(m_suspendProcessButton, 1, 1);
    controlLayout->addWidget(m_resumeProcessButton, 1, 2);
    controlLayout->addWidget(new QLabel("关键进程", controlGroup), 2, 0);
    controlLayout->addWidget(m_setCriticalButton, 2, 1);
    controlLayout->addWidget(m_clearCriticalButton, 2, 2);
    controlLayout->addWidget(new QLabel("优先级", controlGroup), 3, 0);
    controlLayout->addWidget(m_priorityCombo, 3, 1, 1, 3);
    controlLayout->addWidget(m_applyPriorityButton, 3, 4);
    m_actionLayout->addWidget(controlGroup);

    // 补充操作组：
    // - 与进程列表右键菜单对齐，把详情页原先遗漏的效率模式、PPL 刷新和 R0 能力放进来；
    // - R0 按钮统一叠加 Kernel.png 角标，菜单弹出项在点击时动态生成。
    QGroupBox* extendedActionGroup = new QGroupBox(QStringLiteral("右键菜单同步能力"), m_actionTab);
    QGridLayout* extendedActionLayout = new QGridLayout(extendedActionGroup);
    extendedActionLayout->setHorizontalSpacing(8);
    extendedActionLayout->setVerticalSpacing(8);

    m_openProcessFolderButton = buildTextActionButton(
        QStringLiteral("打开目录"),
        QStringLiteral("打开当前进程所在目录"),
        extendedActionGroup);
    m_refreshPplProtectionButton = buildTextActionButton(
        QStringLiteral("刷新PPL"),
        QStringLiteral("手动刷新当前进程 PPL 保护级别"),
        extendedActionGroup);
    m_enableEfficiencyModeButton = buildTextActionButton(
        QStringLiteral("开效率"),
        QStringLiteral("开启当前进程效率模式（绿叶）"),
        extendedActionGroup);
    m_disableEfficiencyModeButton = buildTextActionButton(
        QStringLiteral("关效率"),
        QStringLiteral("关闭当前进程效率模式"),
        extendedActionGroup);

    // buildR0MenuButton 作用：
    // - 为 R0 功能创建“文字 + Kernel.png 角标”的按钮；
    // - 输入 buttonText 为可见文字，iconPath 用于生成 R0 角标图标，toolTipText 为补充说明；
    // - 返回按钮对象，调用方负责接入布局和 clicked 处理。
    const auto buildR0MenuButton =
        [](const QString& buttonText, const QString& iconPath, const QString& toolTipText, QWidget* parentWidget) -> QPushButton*
    {
        QPushButton* actionButton = new QPushButton(
            buildProcessDetailR0ActionIcon(iconPath),
            buttonText,
            parentWidget);
        actionButton->setMinimumHeight(32);
        actionButton->setMinimumWidth(92);
        actionButton->setIconSize(QSize(16, 16));
        actionButton->setToolTip(toolTipText);
        return actionButton;
    };

    m_r0TerminateProcessButton = buildR0MenuButton(
        QStringLiteral("R0结束"),
        QStringLiteral(":/Icon/process_terminate.svg"),
        QStringLiteral("通过 R0 驱动结束当前进程"),
        extendedActionGroup);
    m_r0SuspendProcessButton = buildR0MenuButton(
        QStringLiteral("R0挂起"),
        QStringLiteral(":/Icon/process_suspend.svg"),
        QStringLiteral("通过 R0 驱动挂起当前进程"),
        extendedActionGroup);
    m_r0SetPplButton = buildR0MenuButton(
        QStringLiteral("R0 PPL"),
        QStringLiteral(":/Icon/process_critical.svg"),
        QStringLiteral("通过 R0 驱动设置当前进程 PPL 层级"),
        extendedActionGroup);
    m_r0VisibilityButton = buildR0MenuButton(
        QStringLiteral("R0隐藏"),
        QStringLiteral(":/Icon/process_details.svg"),
        QStringLiteral("通过 R0 驱动隐藏/恢复当前进程"),
        extendedActionGroup);
    m_r0DangerFlagsButton = buildR0MenuButton(
        QStringLiteral("R0危险"),
        QStringLiteral(":/Icon/process_uncritical.svg"),
        QStringLiteral("R0 BreakOnTermination / APC / DKOM 高风险操作"),
        extendedActionGroup);

    extendedActionLayout->addWidget(new QLabel(QStringLiteral("辅助"), extendedActionGroup), 0, 0);
    extendedActionLayout->addWidget(m_openProcessFolderButton, 0, 1);
    extendedActionLayout->addWidget(m_refreshPplProtectionButton, 0, 2);
    extendedActionLayout->addWidget(new QLabel(QStringLiteral("效率模式"), extendedActionGroup), 1, 0);
    extendedActionLayout->addWidget(m_enableEfficiencyModeButton, 1, 1);
    extendedActionLayout->addWidget(m_disableEfficiencyModeButton, 1, 2);
    extendedActionLayout->addWidget(new QLabel(QStringLiteral("R0"), extendedActionGroup), 2, 0);
    extendedActionLayout->addWidget(m_r0TerminateProcessButton, 2, 1);
    extendedActionLayout->addWidget(m_r0SuspendProcessButton, 2, 2);
    extendedActionLayout->addWidget(m_r0SetPplButton, 2, 3);
    extendedActionLayout->addWidget(m_r0VisibilityButton, 2, 4);
    extendedActionLayout->addWidget(m_r0DangerFlagsButton, 2, 5);
    extendedActionLayout->setColumnStretch(6, 1);
    m_actionLayout->addWidget(extendedActionGroup);

    // 注入与载入组：
    // - 把 DLL / Shellcode 两套操作收成统一两行；
    // - 浏览与执行按钮使用文字按钮，保证操作面板不再依赖图标表达含义。
    QGroupBox* injectGroup = new QGroupBox("注入与载入", m_actionTab);
    QGridLayout* injectLayout = new QGridLayout(injectGroup);
    injectLayout->setHorizontalSpacing(8);
    injectLayout->setVerticalSpacing(8);

    m_dllPathLineEdit = new QLineEdit(injectGroup);
    m_dllPathLineEdit->setPlaceholderText("请选择要注入的 DLL 路径");
    m_browseDllButton = buildTextActionButton(
        QStringLiteral("浏览"),
        QStringLiteral("浏览并选择 DLL 文件"),
        injectGroup);
    m_injectDllButton = buildTextActionButton(
        QStringLiteral("注入"),
        QStringLiteral("执行 DLL 注入"),
        injectGroup);

    m_shellcodePathLineEdit = new QLineEdit(injectGroup);
    m_shellcodePathLineEdit->setPlaceholderText("请选择原始 shellcode 二进制文件");
    m_browseShellcodeButton = buildTextActionButton(
        QStringLiteral("浏览"),
        QStringLiteral("浏览并选择 shellcode 文件"),
        injectGroup);
    m_injectShellcodeButton = buildTextActionButton(
        QStringLiteral("执行"),
        QStringLiteral("执行 shellcode 注入"),
        injectGroup);

    injectLayout->addWidget(new QLabel("DLL", injectGroup), 0, 0);
    injectLayout->addWidget(m_dllPathLineEdit, 0, 1);
    injectLayout->addWidget(m_browseDllButton, 0, 2);
    injectLayout->addWidget(m_injectDllButton, 0, 3);
    injectLayout->addWidget(new QLabel("Shellcode", injectGroup), 1, 0);
    injectLayout->addWidget(m_shellcodePathLineEdit, 1, 1);
    injectLayout->addWidget(m_browseShellcodeButton, 1, 2);
    injectLayout->addWidget(m_injectShellcodeButton, 1, 3);
    m_actionLayout->addWidget(injectGroup);

    m_actionLayout->addStretch(1);

    // 统一按钮主题样式：
    // - 组合框继续使用项目蓝色描边；
    // - 紧凑按钮沿用统一蓝色按钮皮肤，避免局部控件风格割裂。
    const QString buttonStyle = buildBlueButtonStyle();
    const QString comboStyle = QStringLiteral(
        "QComboBox {"
        "  border: 1px solid %1;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "  color: %2;"
        "  background: %3;"
        "}"
        "QComboBox:hover {"
        "  border-color: %4;"
        "}"
        "QComboBox::drop-down {"
        "  border:none;"
        "  width:20px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background:%3;"
        "  color:%2;"
        "  border:1px solid %1;"
        "  selection-background-color:%4;"
        "  selection-color:#FFFFFF;"
        "}")
        .arg(KswordTheme::BorderHex())
        .arg(KswordTheme::TextPrimaryHex())
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::PrimaryBlueHex);
    m_terminateActionCombo->setStyleSheet(comboStyle);
    m_priorityCombo->setStyleSheet(comboStyle);

    const std::vector<QPushButton*> actionButtons{
        m_executeTerminateActionButton,
        m_suspendProcessButton,
        m_resumeProcessButton,
        m_setCriticalButton,
        m_clearCriticalButton,
        m_applyPriorityButton,
        m_openProcessFolderButton,
        m_refreshPplProtectionButton,
        m_enableEfficiencyModeButton,
        m_disableEfficiencyModeButton,
        m_r0TerminateProcessButton,
        m_r0SuspendProcessButton,
        m_r0SetPplButton,
        m_r0VisibilityButton,
        m_r0DangerFlagsButton,
        m_browseDllButton,
        m_injectDllButton,
        m_browseShellcodeButton,
        m_injectShellcodeButton
    };
    for (QPushButton* buttonItem : actionButtons)
    {
        if (buttonItem != nullptr)
        {
            buttonItem->setStyleSheet(buttonStyle);
        }
    }
}

void ProcessDetailWindow::initializeModuleTab()
{
    // 模块页初始化日志：确认模块表与工具栏创建。
    kLogEvent initModuleTabEvent;
    info << initModuleTabEvent
        << "[ProcessDetailWindow] initializeModuleTab: 构建模块页面。"
        << eol;

    m_moduleLayout = new QVBoxLayout(m_moduleTab);
    m_moduleLayout->setContentsMargins(6, 6, 6, 6);
    m_moduleLayout->setSpacing(6);

    // 顶部工具栏：刷新按钮 + 签名校验选项 + 状态标签。
    m_moduleTopBarLayout = new QHBoxLayout();
    m_moduleTopBarLayout->setContentsMargins(0, 0, 0, 0);
    m_moduleTopBarLayout->setSpacing(8);
    m_refreshModuleButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), "刷新模块", m_moduleTab);
    m_signatureCheckBox = new QCheckBox("刷新时校验签名", m_moduleTab);
    m_signatureCheckBox->setChecked(true);
    m_signatureCheckBox->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-weight:600; }"
        "QCheckBox::indicator:checked { background:%1; border:1px solid %1; }")
        .arg(KswordTheme::PrimaryBlueHex));
    m_moduleStatusLabel = new QLabel("● 等待首次刷新", m_moduleTab);
    m_moduleStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    m_moduleTopBarLayout->addWidget(m_refreshModuleButton);
    m_moduleTopBarLayout->addWidget(m_signatureCheckBox);
    m_moduleTopBarLayout->addStretch(1);
    m_moduleTopBarLayout->addWidget(m_moduleStatusLabel);
    m_moduleLayout->addLayout(m_moduleTopBarLayout);

    // 模块列表表格。
    m_moduleTable = new QTreeWidget(m_moduleTab);
    m_moduleTable->setColumnCount(static_cast<int>(ModuleColumn::Count));
    m_moduleTable->setHeaderLabels(ModuleHeaders);
    m_moduleTable->setRootIsDecorated(false);
    m_moduleTable->setItemsExpandable(false);
    m_moduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_moduleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_moduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_moduleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_moduleTable->setSortingEnabled(true);
    m_moduleTable->setAlternatingRowColors(true);
    m_moduleLayout->addWidget(m_moduleTable, 1);

    // 列宽初始化。
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::Path), 560);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::Size), 110);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::Signature), 260);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::EntryOffset), 120);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::State), 90);
    m_moduleTable->setColumnWidth(toModuleColumnIndex(ModuleColumn::ThreadId), 180);

    // 表头蓝色主题。
    m_moduleTable->header()->setStyleSheet(QStringLiteral(
        "QHeaderView::section {"
        "  color:%1;"
        "  background:%2;"
        "  border:1px solid %3;"
        "  padding:4px;"
        "  font-weight:600;"
        "}")
        .arg(KswordTheme::PrimaryBlueHex)
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::BorderHex()));

    m_refreshModuleButton->setStyleSheet(buildBlueButtonStyle());
}

void ProcessDetailWindow::initializeTokenTab()
{
    // 令牌页初始化：专门展示 SID/特权/完整性级别等信息。
    kLogEvent initTokenTabEvent;
    info << initTokenTabEvent
        << "[ProcessDetailWindow] initializeTokenTab: 构建令牌信息页面。"
        << eol;

    m_tokenLayout = new QVBoxLayout(m_tokenTab);
    m_tokenLayout->setContentsMargins(6, 6, 6, 6);
    m_tokenLayout->setSpacing(6);

    QHBoxLayout* tokenTopBarLayout = new QHBoxLayout();
    m_refreshTokenButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), "刷新令牌", m_tokenTab);
    m_refreshTokenButton->setToolTip("异步刷新用户 SID、组、特权、完整性级别等令牌信息");
    m_tokenStatusLabel = new QLabel("● 尚未刷新", m_tokenTab);
    m_tokenStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    tokenTopBarLayout->addWidget(m_refreshTokenButton);
    tokenTopBarLayout->addWidget(m_tokenStatusLabel, 1);
    m_tokenLayout->addLayout(tokenTopBarLayout);

    m_tokenDetailOutput = new CodeEditorWidget(m_tokenTab);
    m_tokenDetailOutput->setReadOnly(true);
    m_tokenDetailOutput->setText(QStringLiteral("令牌详细信息将在此处显示。"));
    m_tokenLayout->addWidget(m_tokenDetailOutput, 1);

    const QString buttonStyle = buildBlueButtonStyle();
    m_refreshTokenButton->setStyleSheet(buttonStyle);
}

void ProcessDetailWindow::initializeKernelObjectTab()
{
    // 内核对象页初始化：
    // - 展示 Phase-2 进程扩展信息；
    // - 不执行句柄表/Section 枚举，避免 DynData 缺失时误触后续高风险路径。
    kLogEvent initKernelObjectTabEvent;
    info << initKernelObjectTabEvent
        << "[ProcessDetailWindow] initializeKernelObjectTab: 构建内核对象页面。"
        << eol;

    m_kernelObjectLayout = new QVBoxLayout(m_kernelObjectTab);
    m_kernelObjectLayout->setContentsMargins(6, 6, 6, 6);
    m_kernelObjectLayout->setSpacing(8);

    QLabel* hintLabel = new QLabel(
        QStringLiteral("本页展示 R0 读取到的 EPROCESS 字段来源、偏移和可用性；Section/ControlArea 查询只通过 PID 调用驱动，不把诊断地址作为凭据。"),
        m_kernelObjectTab);
    hintLabel->setWordWrap(true);
    hintLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    m_kernelObjectLayout->addWidget(hintLabel);

    QGroupBox* summaryGroup = new QGroupBox(QStringLiteral("R0 扩展摘要"), m_kernelObjectTab);
    QFormLayout* summaryFormLayout = new QFormLayout(summaryGroup);
    summaryFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    summaryFormLayout->setHorizontalSpacing(18);
    summaryFormLayout->setVerticalSpacing(6);

    m_kernelObjectR0StatusValue = new QLabel(summaryGroup);
    m_kernelObjectCapabilityValue = new QLabel(summaryGroup);
    m_kernelObjectImagePathValue = new QLabel(summaryGroup);
    m_kernelObjectR0StatusValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelObjectCapabilityValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelObjectImagePathValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelObjectImagePathValue->setWordWrap(true);

    summaryFormLayout->addRow(QStringLiteral("R0 状态"), m_kernelObjectR0StatusValue);
    summaryFormLayout->addRow(QStringLiteral("DynData Capability"), m_kernelObjectCapabilityValue);
    summaryFormLayout->addRow(QStringLiteral("R0 镜像路径"), m_kernelObjectImagePathValue);
    m_kernelObjectLayout->addWidget(summaryGroup);

    QGroupBox* objectGroup = new QGroupBox(QStringLiteral("对象字段可用性"), m_kernelObjectTab);
    QFormLayout* objectFormLayout = new QFormLayout(objectGroup);
    objectFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    objectFormLayout->setHorizontalSpacing(18);
    objectFormLayout->setVerticalSpacing(6);

    m_kernelObjectHandleTableValue = new QLabel(objectGroup);
    m_kernelObjectSectionObjectValue = new QLabel(objectGroup);
    m_kernelObjectHandleTableValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelObjectSectionObjectValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    objectFormLayout->addRow(QStringLiteral("HandleTable"), m_kernelObjectHandleTableValue);
    objectFormLayout->addRow(QStringLiteral("SectionObject"), m_kernelObjectSectionObjectValue);
    m_kernelObjectLayout->addWidget(objectGroup);

    QGroupBox* protectionGroup = new QGroupBox(QStringLiteral("保护与签名字段"), m_kernelObjectTab);
    QFormLayout* protectionFormLayout = new QFormLayout(protectionGroup);
    protectionFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    protectionFormLayout->setHorizontalSpacing(18);
    protectionFormLayout->setVerticalSpacing(6);

    m_kernelObjectProtectionValue = new QLabel(protectionGroup);
    m_kernelObjectSignatureValue = new QLabel(protectionGroup);
    m_kernelObjectSectionSignatureValue = new QLabel(protectionGroup);
    m_kernelObjectProtectionValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelObjectSignatureValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kernelObjectSectionSignatureValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    protectionFormLayout->addRow(QStringLiteral("EPROCESS.Protection"), m_kernelObjectProtectionValue);
    protectionFormLayout->addRow(QStringLiteral("SignatureLevel"), m_kernelObjectSignatureValue);
    protectionFormLayout->addRow(QStringLiteral("SectionSignatureLevel"), m_kernelObjectSectionSignatureValue);
    m_kernelObjectLayout->addWidget(protectionGroup);

    QGroupBox* sourceGroup = new QGroupBox(QStringLiteral("字段来源"), m_kernelObjectTab);
    QFormLayout* sourceFormLayout = new QFormLayout(sourceGroup);
    sourceFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sourceFormLayout->setHorizontalSpacing(18);
    sourceFormLayout->setVerticalSpacing(6);

    m_kernelObjectSessionSourceValue = new QLabel(sourceGroup);
    m_kernelObjectImagePathSourceValue = new QLabel(sourceGroup);
    m_kernelObjectProtectionSourceValue = new QLabel(sourceGroup);
    m_kernelObjectSignatureSourceValue = new QLabel(sourceGroup);
    m_kernelObjectSectionSignatureSourceValue = new QLabel(sourceGroup);
    m_kernelObjectObjectTableSourceValue = new QLabel(sourceGroup);
    m_kernelObjectSectionObjectSourceValue = new QLabel(sourceGroup);
    sourceFormLayout->addRow(QStringLiteral("Session"), m_kernelObjectSessionSourceValue);
    sourceFormLayout->addRow(QStringLiteral("ImagePath"), m_kernelObjectImagePathSourceValue);
    sourceFormLayout->addRow(QStringLiteral("Protection"), m_kernelObjectProtectionSourceValue);
    sourceFormLayout->addRow(QStringLiteral("SignatureLevel"), m_kernelObjectSignatureSourceValue);
    sourceFormLayout->addRow(QStringLiteral("SectionSignatureLevel"), m_kernelObjectSectionSignatureSourceValue);
    sourceFormLayout->addRow(QStringLiteral("ObjectTable"), m_kernelObjectObjectTableSourceValue);
    sourceFormLayout->addRow(QStringLiteral("SectionObject"), m_kernelObjectSectionObjectSourceValue);
    m_kernelObjectLayout->addWidget(sourceGroup);

    QGroupBox* offsetGroup = new QGroupBox(QStringLiteral("EPROCESS 偏移"), m_kernelObjectTab);
    QFormLayout* offsetFormLayout = new QFormLayout(offsetGroup);
    offsetFormLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    offsetFormLayout->setHorizontalSpacing(18);
    offsetFormLayout->setVerticalSpacing(6);

    m_kernelObjectProtectionOffsetValue = new QLabel(offsetGroup);
    m_kernelObjectSignatureOffsetValue = new QLabel(offsetGroup);
    m_kernelObjectSectionSignatureOffsetValue = new QLabel(offsetGroup);
    m_kernelObjectObjectTableOffsetValue = new QLabel(offsetGroup);
    m_kernelObjectSectionObjectOffsetValue = new QLabel(offsetGroup);
    offsetFormLayout->addRow(QStringLiteral("Protection"), m_kernelObjectProtectionOffsetValue);
    offsetFormLayout->addRow(QStringLiteral("SignatureLevel"), m_kernelObjectSignatureOffsetValue);
    offsetFormLayout->addRow(QStringLiteral("SectionSignatureLevel"), m_kernelObjectSectionSignatureOffsetValue);
    offsetFormLayout->addRow(QStringLiteral("ObjectTable"), m_kernelObjectObjectTableOffsetValue);
    offsetFormLayout->addRow(QStringLiteral("SectionObject"), m_kernelObjectSectionObjectOffsetValue);
    m_kernelObjectLayout->addWidget(offsetGroup);

    QGroupBox* sectionGroup = new QGroupBox(QStringLiteral("Section / ControlArea 映射关系"), m_kernelObjectTab);
    QVBoxLayout* sectionGroupLayout = new QVBoxLayout(sectionGroup);
    sectionGroupLayout->setContentsMargins(8, 8, 8, 8);
    sectionGroupLayout->setSpacing(6);

    QHBoxLayout* sectionTopBarLayout = new QHBoxLayout();
    m_refreshSectionInfoButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新 Section"), sectionGroup);
    m_refreshSectionInfoButton->setToolTip(QStringLiteral("通过 R0 查询当前进程 SectionObject、ControlArea 和映射摘要"));
    m_sectionInfoStatusLabel = new QLabel(QStringLiteral("● 尚未刷新"), sectionGroup);
    m_sectionInfoStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    sectionTopBarLayout->addWidget(m_refreshSectionInfoButton);
    sectionTopBarLayout->addWidget(m_sectionInfoStatusLabel, 1);
    sectionGroupLayout->addLayout(sectionTopBarLayout);

    m_sectionInfoOutput = new CodeEditorWidget(sectionGroup);
    m_sectionInfoOutput->setReadOnly(true);
    m_sectionInfoOutput->setText(QStringLiteral("Section/ControlArea 查询结果将在此处显示。"));
    sectionGroupLayout->addWidget(m_sectionInfoOutput, 1);
    m_kernelObjectLayout->addWidget(sectionGroup, 1);

    m_kernelObjectLayout->addStretch(1);
}

void ProcessDetailWindow::initializeTokenSwitchTab()
{
    // 令牌设置页初始化：
    // - 第一部分提供常用开关复选框（快速设置）；
    // - 第二部分提供原始 NtSetInformationToken 入口（覆盖全部信息类）。
    kLogEvent initTokenSwitchTabEvent;
    info << initTokenSwitchTabEvent
        << "[ProcessDetailWindow] initializeTokenSwitchTab: 构建完整令牌设置页面。"
        << eol;

    m_tokenSwitchLayout = new QVBoxLayout(m_tokenSwitchTab);
    m_tokenSwitchLayout->setContentsMargins(6, 6, 6, 6);
    m_tokenSwitchLayout->setSpacing(8);

    // 顶部工具栏按钮：
    // - 刷新开关：只刷新快捷开关复选框；
    // - 应用开关：提交快捷开关；
    // - 刷新全部：触发令牌详情页“全信息类枚举”刷新。
    QHBoxLayout* tokenSwitchTopBarLayout = new QHBoxLayout();
    m_refreshTokenSwitchButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_tokenSwitchTab);
    m_refreshTokenSwitchButton->setToolTip(QStringLiteral("刷新当前进程令牌的各项开关状态"));
    m_refreshTokenSwitchButton->setFixedSize(34, 34);
    m_refreshTokenSwitchButton->setIconSize(QSize(16, 16));
    m_applyTokenSwitchButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_tokenSwitchTab);
    m_applyTokenSwitchButton->setToolTip(QStringLiteral("把下方复选框状态写回目标进程令牌"));
    m_applyTokenSwitchButton->setFixedSize(34, 34);
    m_applyTokenSwitchButton->setIconSize(QSize(16, 16));
    m_refreshTokenAllInfoButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_tokenSwitchTab);
    m_refreshTokenAllInfoButton->setToolTip(QStringLiteral("刷新完整令牌信息（包含全部 TokenInformationClass 枚举）"));
    m_refreshTokenAllInfoButton->setFixedSize(34, 34);
    m_refreshTokenAllInfoButton->setIconSize(QSize(16, 16));
    m_tokenSwitchStatusLabel = new QLabel(QStringLiteral("● 尚未刷新令牌开关"), m_tokenSwitchTab);
    m_tokenSwitchStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    tokenSwitchTopBarLayout->addWidget(m_refreshTokenSwitchButton);
    tokenSwitchTopBarLayout->addWidget(m_applyTokenSwitchButton);
    tokenSwitchTopBarLayout->addWidget(m_refreshTokenAllInfoButton);
    tokenSwitchTopBarLayout->addWidget(m_tokenSwitchStatusLabel, 1);
    m_tokenSwitchLayout->addLayout(tokenSwitchTopBarLayout);

    // 快捷开关组：
    // - 对应常见 Token 布尔位与 MandatoryPolicy 位；
    // - 适合“一眼可见 + 一键应用”的高频修改场景。
    QGroupBox* tokenSwitchGroup = new QGroupBox(QStringLiteral("Token 快捷开关"), m_tokenSwitchTab);
    QGridLayout* tokenSwitchGridLayout = new QGridLayout(tokenSwitchGroup);
    tokenSwitchGridLayout->setHorizontalSpacing(12);
    tokenSwitchGridLayout->setVerticalSpacing(8);

    m_tokenSandboxInertCheck = new QCheckBox(QStringLiteral("SandboxInert"), tokenSwitchGroup);
    m_tokenSandboxInertCheck->setToolTip(QStringLiteral("TokenSandBoxInert：沙箱惰性开关，常用于兼容旧进程策略"));
    m_tokenVirtualizationAllowedCheck = new QCheckBox(QStringLiteral("VirtualizationAllowed"), tokenSwitchGroup);
    m_tokenVirtualizationAllowedCheck->setToolTip(QStringLiteral("TokenVirtualizationAllowed：是否允许 UAC 虚拟化"));
    m_tokenVirtualizationEnabledCheck = new QCheckBox(QStringLiteral("VirtualizationEnabled"), tokenSwitchGroup);
    m_tokenVirtualizationEnabledCheck->setToolTip(QStringLiteral("TokenVirtualizationEnabled：是否启用 UAC 虚拟化"));
    m_tokenUiAccessCheck = new QCheckBox(QStringLiteral("UIAccess"), tokenSwitchGroup);
    m_tokenUiAccessCheck->setToolTip(QStringLiteral("TokenUIAccess：是否允许跨完整性级别访问部分 UI"));
    m_tokenMandatoryNoWriteUpCheck = new QCheckBox(QStringLiteral("MandatoryPolicy.NoWriteUp"), tokenSwitchGroup);
    m_tokenMandatoryNoWriteUpCheck->setToolTip(QStringLiteral("TokenMandatoryPolicy 位 0：禁止低完整性向高完整性写入"));
    m_tokenMandatoryNewProcessMinCheck = new QCheckBox(QStringLiteral("MandatoryPolicy.NewProcessMin"), tokenSwitchGroup);
    m_tokenMandatoryNewProcessMinCheck->setToolTip(QStringLiteral("TokenMandatoryPolicy 位 1：新进程最小化完整性策略"));

    tokenSwitchGridLayout->addWidget(m_tokenSandboxInertCheck, 0, 0);
    tokenSwitchGridLayout->addWidget(m_tokenVirtualizationAllowedCheck, 0, 1);
    tokenSwitchGridLayout->addWidget(m_tokenVirtualizationEnabledCheck, 1, 0);
    tokenSwitchGridLayout->addWidget(m_tokenUiAccessCheck, 1, 1);
    tokenSwitchGridLayout->addWidget(m_tokenMandatoryNoWriteUpCheck, 2, 0);
    tokenSwitchGridLayout->addWidget(m_tokenMandatoryNewProcessMinCheck, 2, 1);
    m_tokenSwitchLayout->addWidget(tokenSwitchGroup);

    // 常用信息类（布尔语义）组：
    // - 这些项都来自 TokenInformationClass 下拉中的高频类；
    // - 通过复选框直接读写，减少“选类 + 填值”的重复操作。
    QGroupBox* tokenCommonClassGroup =
        new QGroupBox(QStringLiteral("Token 常用信息类（布尔语义）"), m_tokenSwitchTab);
    QGridLayout* tokenCommonClassGridLayout = new QGridLayout(tokenCommonClassGroup);
    tokenCommonClassGridLayout->setHorizontalSpacing(12);
    tokenCommonClassGridLayout->setVerticalSpacing(8);

    m_tokenHasRestrictionsCheck =
        new QCheckBox(QStringLiteral("HasRestrictions"), tokenCommonClassGroup);
    m_tokenHasRestrictionsCheck->setToolTip(
        QStringLiteral("TokenHasRestrictions（class=21）：是否存在限制 SID / 限制策略"));
    m_tokenIsAppContainerCheck =
        new QCheckBox(QStringLiteral("IsAppContainer"), tokenCommonClassGroup);
    m_tokenIsAppContainerCheck->setToolTip(
        QStringLiteral("TokenIsAppContainer（class=29）：当前令牌是否为 AppContainer"));
    m_tokenIsRestrictedCheck =
        new QCheckBox(QStringLiteral("IsRestricted"), tokenCommonClassGroup);
    m_tokenIsRestrictedCheck->setToolTip(
        QStringLiteral("TokenIsRestricted（class=40）：当前令牌是否受限制"));
    m_tokenIsLessPrivilegedAppContainerCheck =
        new QCheckBox(QStringLiteral("IsLessPrivilegedAppContainer"), tokenCommonClassGroup);
    m_tokenIsLessPrivilegedAppContainerCheck->setToolTip(
        QStringLiteral("TokenIsLessPrivilegedAppContainer（class=46）：是否为低权限 AppContainer"));
    m_tokenIsSandboxedCheck =
        new QCheckBox(QStringLiteral("IsSandboxed"), tokenCommonClassGroup);
    m_tokenIsSandboxedCheck->setToolTip(
        QStringLiteral("TokenIsSandboxed（class=47）：当前令牌是否被沙箱化"));
    m_tokenIsAppSiloCheck =
        new QCheckBox(QStringLiteral("IsAppSilo"), tokenCommonClassGroup);
    m_tokenIsAppSiloCheck->setToolTip(
        QStringLiteral("TokenIsAppSilo（class=51）：当前令牌是否属于 AppSilo"));

    tokenCommonClassGridLayout->addWidget(m_tokenHasRestrictionsCheck, 0, 0);
    tokenCommonClassGridLayout->addWidget(m_tokenIsAppContainerCheck, 0, 1);
    tokenCommonClassGridLayout->addWidget(m_tokenIsRestrictedCheck, 1, 0);
    tokenCommonClassGridLayout->addWidget(m_tokenIsLessPrivilegedAppContainerCheck, 1, 1);
    tokenCommonClassGridLayout->addWidget(m_tokenIsSandboxedCheck, 2, 0);
    tokenCommonClassGridLayout->addWidget(m_tokenIsAppSiloCheck, 2, 1);
    m_tokenSwitchLayout->addWidget(tokenCommonClassGroup);

    // 原始设置组：
    // - 允许用户选择任意 TokenInformationClass；
    // - 负载支持 UInt32/UInt64/HexBytes，直接进入 NtSetInformationToken。
    QGroupBox* rawSetGroup = new QGroupBox(QStringLiteral("原始 NtSetInformationToken（全部信息类）"), m_tokenSwitchTab);
    QGridLayout* rawSetLayout = new QGridLayout(rawSetGroup);
    rawSetLayout->setHorizontalSpacing(10);
    rawSetLayout->setVerticalSpacing(8);

    m_tokenRawInfoClassCombo = new QComboBox(rawSetGroup);
    m_tokenRawInfoClassCombo->setToolTip(QStringLiteral("选择要传给 NtSetInformationToken 的 TokenInformationClass"));
    const auto tokenInfoClassNameById = [](const int classId) -> QString
    {
        switch (classId)
        {
        case 1: return QStringLiteral("TokenUser");
        case 2: return QStringLiteral("TokenGroups");
        case 3: return QStringLiteral("TokenPrivileges");
        case 4: return QStringLiteral("TokenOwner");
        case 5: return QStringLiteral("TokenPrimaryGroup");
        case 6: return QStringLiteral("TokenDefaultDacl");
        case 7: return QStringLiteral("TokenSource");
        case 8: return QStringLiteral("TokenType");
        case 9: return QStringLiteral("TokenImpersonationLevel");
        case 10: return QStringLiteral("TokenStatistics");
        case 11: return QStringLiteral("TokenRestrictedSids");
        case 12: return QStringLiteral("TokenSessionId");
        case 13: return QStringLiteral("TokenGroupsAndPrivileges");
        case 14: return QStringLiteral("TokenSessionReference");
        case 15: return QStringLiteral("TokenSandBoxInert");
        case 16: return QStringLiteral("TokenAuditPolicy");
        case 17: return QStringLiteral("TokenOrigin");
        case 18: return QStringLiteral("TokenElevationType");
        case 19: return QStringLiteral("TokenLinkedToken");
        case 20: return QStringLiteral("TokenElevation");
        case 21: return QStringLiteral("TokenHasRestrictions");
        case 22: return QStringLiteral("TokenAccessInformation");
        case 23: return QStringLiteral("TokenVirtualizationAllowed");
        case 24: return QStringLiteral("TokenVirtualizationEnabled");
        case 25: return QStringLiteral("TokenIntegrityLevel");
        case 26: return QStringLiteral("TokenUIAccess");
        case 27: return QStringLiteral("TokenMandatoryPolicy");
        case 28: return QStringLiteral("TokenLogonSid");
        case 29: return QStringLiteral("TokenIsAppContainer");
        case 30: return QStringLiteral("TokenCapabilities");
        case 31: return QStringLiteral("TokenAppContainerSid");
        case 32: return QStringLiteral("TokenAppContainerNumber");
        case 33: return QStringLiteral("TokenUserClaimAttributes");
        case 34: return QStringLiteral("TokenDeviceClaimAttributes");
        case 35: return QStringLiteral("TokenRestrictedUserClaimAttributes");
        case 36: return QStringLiteral("TokenRestrictedDeviceClaimAttributes");
        case 37: return QStringLiteral("TokenDeviceGroups");
        case 38: return QStringLiteral("TokenRestrictedDeviceGroups");
        case 39: return QStringLiteral("TokenSecurityAttributes");
        case 40: return QStringLiteral("TokenIsRestricted");
        case 41: return QStringLiteral("TokenProcessTrustLevel");
        case 42: return QStringLiteral("TokenPrivateNameSpace");
        case 43: return QStringLiteral("TokenSingletonAttributes");
        case 44: return QStringLiteral("TokenBnoIsolation");
        case 45: return QStringLiteral("TokenChildProcessFlags");
        case 46: return QStringLiteral("TokenIsLessPrivilegedAppContainer");
        case 47: return QStringLiteral("TokenIsSandboxed");
        case 48: return QStringLiteral("TokenOriginatingProcessTrustLevel");
        case 49: return QStringLiteral("TokenLoggingInformation");
        case 50: return QStringLiteral("TokenLearningMode");
        case 51: return QStringLiteral("TokenIsAppSilo");
        default: return QStringLiteral("TokenClass%1").arg(classId);
        }
    };
    for (int classId = 1; classId <= 80; ++classId)
    {
        const QString itemText = QStringLiteral("[%1] %2")
            .arg(classId)
            .arg(tokenInfoClassNameById(classId));
        m_tokenRawInfoClassCombo->addItem(itemText, classId);
    }
    m_tokenRawInfoClassCombo->setCurrentIndex(14);

    m_tokenRawInputModeCombo = new QComboBox(rawSetGroup);
    m_tokenRawInputModeCombo->setToolTip(QStringLiteral("选择原始负载解释方式"));
    m_tokenRawInputModeCombo->addItem(QStringLiteral("UInt32"), QStringLiteral("u32"));
    m_tokenRawInputModeCombo->addItem(QStringLiteral("UInt64"), QStringLiteral("u64"));
    m_tokenRawInputModeCombo->addItem(QStringLiteral("HexBytes"), QStringLiteral("hex"));

    m_tokenRawPayloadEdit = new QLineEdit(rawSetGroup);
    m_tokenRawPayloadEdit->setPlaceholderText(QStringLiteral("示例：UInt32=1；UInt64=0x10；HexBytes=01 00 00 00"));
    m_tokenRawPayloadEdit->setToolTip(QStringLiteral("原始输入内容，按当前输入模式解析后直接传给 NtSetInformationToken"));

    m_tokenRawApplyButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), rawSetGroup);
    m_tokenRawApplyButton->setToolTip(QStringLiteral("应用原始 NtSetInformationToken 设置"));
    m_tokenRawApplyButton->setFixedSize(34, 34);
    m_tokenRawApplyButton->setIconSize(QSize(16, 16));

    rawSetLayout->addWidget(new QLabel(QStringLiteral("信息类"), rawSetGroup), 0, 0);
    rawSetLayout->addWidget(m_tokenRawInfoClassCombo, 0, 1, 1, 2);
    rawSetLayout->addWidget(new QLabel(QStringLiteral("输入模式"), rawSetGroup), 1, 0);
    rawSetLayout->addWidget(m_tokenRawInputModeCombo, 1, 1, 1, 2);
    rawSetLayout->addWidget(new QLabel(QStringLiteral("原始负载"), rawSetGroup), 2, 0);
    rawSetLayout->addWidget(m_tokenRawPayloadEdit, 2, 1);
    rawSetLayout->addWidget(m_tokenRawApplyButton, 2, 2);
    m_tokenSwitchLayout->addWidget(rawSetGroup);

    QLabel* tokenSwitchHintLabel = new QLabel(
        QStringLiteral("提示：可先点“刷新全部令牌信息”查看所有 TokenInformationClass 的当前状态，再按快捷或原始模式应用。"),
        m_tokenSwitchTab);
    tokenSwitchHintLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    m_tokenSwitchLayout->addWidget(tokenSwitchHintLabel);
    m_tokenSwitchLayout->addStretch(1);

    // 页面样式：
    // - 图标按钮统一蓝色皮肤；
    // - 原始设置组合框使用同一套描边/高亮风格。
    const QString buttonStyle = buildBlueButtonStyle();
    m_refreshTokenSwitchButton->setStyleSheet(buttonStyle);
    m_applyTokenSwitchButton->setStyleSheet(buttonStyle);
    m_refreshTokenAllInfoButton->setStyleSheet(buttonStyle);
    m_tokenRawApplyButton->setStyleSheet(buttonStyle);

    const QString comboStyle = QStringLiteral(
        "QComboBox {"
        "  border: 1px solid %1;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "  color: %2;"
        "  background: %3;"
        "}"
        "QComboBox:hover {"
        "  border-color: %4;"
        "}"
        "QComboBox::drop-down {"
        "  border:none;"
        "  width:20px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background:%3;"
        "  color:%2;"
        "  border:1px solid %1;"
        "  selection-background-color:%4;"
        "  selection-color:#FFFFFF;"
        "}")
        .arg(KswordTheme::BorderHex())
        .arg(KswordTheme::TextPrimaryHex())
        .arg(KswordTheme::SurfaceHex())
        .arg(KswordTheme::PrimaryBlueHex);
    m_tokenRawInfoClassCombo->setStyleSheet(comboStyle);
    m_tokenRawInputModeCombo->setStyleSheet(comboStyle);
}

void ProcessDetailWindow::initializePebTab()
{
    // PEB 页初始化：展示 PEB 地址、参数块、环境变量等。
    kLogEvent initPebTabEvent;
    info << initPebTabEvent
        << "[ProcessDetailWindow] initializePebTab: 构建 PEB 信息页面。"
        << eol;

    m_pebLayout = new QVBoxLayout(m_pebTab);
    m_pebLayout->setContentsMargins(6, 6, 6, 6);
    m_pebLayout->setSpacing(6);

    QHBoxLayout* pebTopBarLayout = new QHBoxLayout();
    m_refreshPebButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), "刷新PEB", m_pebTab);
    m_refreshPebButton->setToolTip("异步刷新 PEB、命令行、当前目录、环境块与安全标志");
    m_applyPebEditButton = new QPushButton(QIcon(":/Icon/process_settings.svg"), QStringLiteral("应用修改"), m_pebTab);
    m_applyPebEditButton->setToolTip(QStringLiteral("把下方可编辑字段写回目标进程。字符串字段优先写入现有缓冲区；空间不足时会尝试远程分配新缓冲区。"));
    m_pebStatusLabel = new QLabel("● 尚未刷新", m_pebTab);
    m_pebStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    pebTopBarLayout->addWidget(m_refreshPebButton);
    pebTopBarLayout->addWidget(m_applyPebEditButton);
    pebTopBarLayout->addWidget(m_pebStatusLabel, 1);
    m_pebLayout->addLayout(pebTopBarLayout);

    QGroupBox* editableGroup = new QGroupBox(QStringLiteral("PEB 可编辑字段（R3 写入目标进程内存）"), m_pebTab);
    QGridLayout* editableGrid = new QGridLayout(editableGroup);
    editableGrid->setContentsMargins(8, 8, 8, 8);
    editableGrid->setSpacing(6);

    m_pebTargetCombo = new QComboBox(editableGroup);
    m_pebTargetCombo->addItem(QStringLiteral("NativePEB"), QStringLiteral("NativePEB"));
    m_pebTargetCombo->addItem(QStringLiteral("Wow64PEB"), QStringLiteral("Wow64PEB"));
    m_pebTargetCombo->setToolTip(QStringLiteral("选择写入 Native PEB 还是 Wow64 PEB。32 位目标通常需要同步修改 Wow64PEB。"));

    m_pebCommandLineEdit = new QLineEdit(editableGroup);
    m_pebImagePathEdit = new QLineEdit(editableGroup);
    m_pebCurrentDirectoryEdit = new QLineEdit(editableGroup);
    m_pebEnvironmentNameEdit = new QLineEdit(editableGroup);
    m_pebEnvironmentValueEdit = new QLineEdit(editableGroup);
    m_pebImageBaseEdit = new QLineEdit(editableGroup);
    m_pebAffinityMaskEdit = new QLineEdit(editableGroup);
    m_pebPriorityClassCombo = new QComboBox(editableGroup);

    m_pebCommandLineEdit->setPlaceholderText(QStringLiteral("RTL_USER_PROCESS_PARAMETERS.CommandLine"));
    m_pebImagePathEdit->setPlaceholderText(QStringLiteral("RTL_USER_PROCESS_PARAMETERS.ImagePathName"));
    m_pebCurrentDirectoryEdit->setPlaceholderText(QStringLiteral("RTL_USER_PROCESS_PARAMETERS.CurrentDirectory.DosPath"));
    m_pebEnvironmentNameEdit->setPlaceholderText(QStringLiteral("例如 PATH / TEMP / 自定义变量名"));
    m_pebEnvironmentValueEdit->setPlaceholderText(QStringLiteral("变量值；为空表示写成 NAME=，不会删除旧环境块条目"));
    m_pebImageBaseEdit->setPlaceholderText(QStringLiteral("高级：PEB.ImageBaseAddress，例如 0x7C0000"));
    m_pebAffinityMaskEdit->setPlaceholderText(QStringLiteral("进程亲和性掩码，例如 0xFFFFFFFF"));

    m_pebImageBaseEdit->setToolTip(QStringLiteral("危险字段：只修改 PEB.ImageBaseAddress 指针，不会重映射模块。错误值可能误导目标进程或工具。"));
    m_pebAffinityMaskEdit->setToolTip(QStringLiteral("调用 SetProcessAffinityMask，属于真实进程属性，不是 PEB 字段。"));

    m_pebPriorityClassCombo->addItem(QStringLiteral("不修改"), 0u);
    m_pebPriorityClassCombo->addItem(QStringLiteral("IDLE"), static_cast<unsigned int>(IDLE_PRIORITY_CLASS));
    m_pebPriorityClassCombo->addItem(QStringLiteral("BELOW_NORMAL"), static_cast<unsigned int>(BELOW_NORMAL_PRIORITY_CLASS));
    m_pebPriorityClassCombo->addItem(QStringLiteral("NORMAL"), static_cast<unsigned int>(NORMAL_PRIORITY_CLASS));
    m_pebPriorityClassCombo->addItem(QStringLiteral("ABOVE_NORMAL"), static_cast<unsigned int>(ABOVE_NORMAL_PRIORITY_CLASS));
    m_pebPriorityClassCombo->addItem(QStringLiteral("HIGH"), static_cast<unsigned int>(HIGH_PRIORITY_CLASS));
    m_pebPriorityClassCombo->addItem(QStringLiteral("REALTIME"), static_cast<unsigned int>(REALTIME_PRIORITY_CLASS));

    editableGrid->addWidget(new QLabel(QStringLiteral("目标PEB"), editableGroup), 0, 0);
    editableGrid->addWidget(m_pebTargetCombo, 0, 1);
    editableGrid->addWidget(new QLabel(QStringLiteral("CommandLine"), editableGroup), 1, 0);
    editableGrid->addWidget(m_pebCommandLineEdit, 1, 1, 1, 3);
    editableGrid->addWidget(new QLabel(QStringLiteral("ImagePathName"), editableGroup), 2, 0);
    editableGrid->addWidget(m_pebImagePathEdit, 2, 1, 1, 3);
    editableGrid->addWidget(new QLabel(QStringLiteral("CurrentDirectory"), editableGroup), 3, 0);
    editableGrid->addWidget(m_pebCurrentDirectoryEdit, 3, 1, 1, 3);
    editableGrid->addWidget(new QLabel(QStringLiteral("环境变量名"), editableGroup), 4, 0);
    editableGrid->addWidget(m_pebEnvironmentNameEdit, 4, 1);
    editableGrid->addWidget(new QLabel(QStringLiteral("环境变量值"), editableGroup), 4, 2);
    editableGrid->addWidget(m_pebEnvironmentValueEdit, 4, 3);
    editableGrid->addWidget(new QLabel(QStringLiteral("ImageBaseAddress"), editableGroup), 5, 0);
    editableGrid->addWidget(m_pebImageBaseEdit, 5, 1);
    editableGrid->addWidget(new QLabel(QStringLiteral("AffinityMask"), editableGroup), 5, 2);
    editableGrid->addWidget(m_pebAffinityMaskEdit, 5, 3);
    editableGrid->addWidget(new QLabel(QStringLiteral("PriorityClass"), editableGroup), 6, 0);
    editableGrid->addWidget(m_pebPriorityClassCombo, 6, 1);
    m_pebLayout->addWidget(editableGroup, 0);

    m_pebDetailOutput = new CodeEditorWidget(m_pebTab);
    m_pebDetailOutput->setReadOnly(true);
    m_pebDetailOutput->setText(QStringLiteral("PEB 与地址空间摘要将在此处显示。"));
    m_pebLayout->addWidget(m_pebDetailOutput, 1);

    m_pebReadonlyReasonOutput = new QPlainTextEdit(m_pebTab);
    m_pebReadonlyReasonOutput->setReadOnly(true);
    m_pebReadonlyReasonOutput->setMaximumHeight(120);
    m_pebReadonlyReasonOutput->setPlainText(QStringLiteral(
        "不可直接修改/不建议直接修改：\n"
        "- KernelCpuMs/UserCpuMs/WorkingSet/PrivateUsage/IO计数/PageFaultCount：系统统计计数，只能由内核/调度器/内存管理器更新。\n"
        "- VirtualAddressRegionPreview：地址空间枚举结果；应通过 VirtualAllocEx/VirtualProtectEx/Unmap/Map 等专门操作改变。\n"
        "- RegionCount/CommitBytes/MappedBytes/ImageBytes/PrivateBytes：统计结果，不是单一字段。\n"
        "- HeapCount/HeapBlock：需要堆管理器一致性，不在 PEB 页直接写。\n"
        "- ProcessParameters 指针/Environment 指针：本页会按需更新字符串字段/环境项，不建议手工乱改指针。"));
    m_pebLayout->addWidget(m_pebReadonlyReasonOutput, 0);

    const QString buttonStyle = buildBlueButtonStyle();
    m_refreshPebButton->setStyleSheet(buttonStyle);
    m_applyPebEditButton->setStyleSheet(buttonStyle);
}

void ProcessDetailWindow::initializeConnections()
{
    // 连接初始化日志：用于确认所有按钮信号都已挂接。
    kLogEvent initConnectionsEvent;
    info << initConnectionsEvent
        << "[ProcessDetailWindow] initializeConnections: 开始连接信号槽。"
        << eol;

    // 复制路径按钮。
    connect(m_copyPathButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_pathLineEdit->text());
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDetailWindow] 复制程序路径, pid=" << m_baseRecord.pid << eol;
    });

    // 打开路径按钮。
    connect(m_openPathFolderButton, &QPushButton::clicked, this, [this]() {
        std::string detailText;
        const bool actionOk = ks::process::OpenFolderByPath(m_baseRecord.imagePath, &detailText);
        // 打开路径属于同一动作链，复用同一个 kLogEvent 传入结果函数。
        kLogEvent actionEvent;
        (actionOk ? info : err) << actionEvent
            << "[ProcessDetailWindow] 打开程序路径, pid="
            << m_baseRecord.pid
            << ", actionOk="
            << (actionOk ? "true" : "false")
            << ", detail="
            << detailText
            << eol;
        showActionResultMessage("打开程序路径", actionOk, detailText, actionEvent);
    });

    // 复制命令行按钮。
    connect(m_copyCommandButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_commandLineEdit->text());
        kLogEvent logEvent;
        dbg << logEvent << "[ProcessDetailWindow] 复制命令行, pid=" << m_baseRecord.pid << eol;
    });

    // 转到父进程按钮。
    connect(m_gotoParentButton, &QPushButton::clicked, this, [this]() {
        const QVariant parentPidVariant = m_gotoParentButton->property("parent_pid");
        if (!parentPidVariant.isValid())
        {
            return;
        }
        const std::uint32_t parentPid = parentPidVariant.toUInt();
        emit requestOpenProcessByPid(parentPid);
    });

    // 跳转句柄按钮：把当前 PID 转发给外部（MainWindow）打开句柄 Dock。
    connect(m_openHandleDockButton, &QPushButton::clicked, this, [this]() {
        if (m_baseRecord.pid == 0)
        {
            return;
        }
        emit requestOpenHandleDockByPid(m_baseRecord.pid);
    });

    // 线程细节刷新按钮。
    connect(m_refreshThreadInspectButton, &QPushButton::clicked, this, [this]() {
        requestAsyncThreadInspectRefresh();
    });

    // 令牌页刷新按钮。
    connect(m_refreshTokenButton, &QPushButton::clicked, this, [this]() {
        requestAsyncTokenRefresh();
    });

    // 令牌开关页刷新按钮：回读当前 token 开关值并同步复选框。
    connect(m_refreshTokenSwitchButton, &QPushButton::clicked, this, [this]() {
        refreshTokenSwitchStates();
    });

    // 令牌开关页应用按钮：把复选框状态写回目标 token。
    connect(m_applyTokenSwitchButton, &QPushButton::clicked, this, [this]() {
        applyTokenSwitchStates();
    });

    // 令牌设置页“刷新全部信息”按钮：触发令牌详情全量刷新（含全部信息类枚举）。
    connect(m_refreshTokenAllInfoButton, &QPushButton::clicked, this, [this]() {
        requestAsyncTokenRefresh();
    });

    // 令牌设置页“原始应用”按钮：按当前 class + payload 直接调用 NtSetInformationToken。
    connect(m_tokenRawApplyButton, &QPushButton::clicked, this, [this]() {
        applyRawTokenInformation();
    });

    // PEB 页刷新按钮。
    connect(m_refreshPebButton, &QPushButton::clicked, this, [this]() {
        requestAsyncPebRefresh();
    });
    connect(m_applyPebEditButton, &QPushButton::clicked, this, [this]() {
        applyPebEditableFields();
    });
    connect(m_pebTargetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_pebDetailOutput != nullptr)
        {
            populatePebEditableFieldsFromText(m_pebDetailOutput->text());
        }
    });

    // Section/ControlArea 刷新按钮：只传 PID 给 ArkDriverClient，避免 UI 回传内核地址。
    connect(m_refreshSectionInfoButton, &QPushButton::clicked, this, [this]() {
        requestAsyncSectionRefresh();
    });

    // 进程热键页刷新按钮。
    connect(m_refreshHotkeyButton, &QPushButton::clicked, this, [this]() {
        requestAsyncHotkeyRefresh();
    });

    // 键盘页刷新按钮：热键与键盘钩子一起刷新。
    connect(m_refreshKeyboardButton, &QPushButton::clicked, this, [this]() {
        requestAsyncKeyboardRefresh();
    });

    // Tab 首次切换时再启动对应重型刷新：
    // - 进程详情窗口打开路径只构建 UI 和轻量文本；
    // - 这样用户能立即看到窗口，后台扫描不会同时挤占线程池与 UI 回填。
    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](const int tabIndex) {
        Q_UNUSED(tabIndex);
        requestInitialRefreshForCurrentTab();
    });

    // 操作页按钮连接：
    // - 结束方案统一走下拉框调度，避免保留多个重复大按钮；
    // - 其余控制动作保持原有执行函数不变。
    connect(m_executeTerminateActionButton, &QPushButton::clicked, this, [this]() { executeSelectedTerminateAction(); });
    connect(m_suspendProcessButton, &QPushButton::clicked, this, [this]() { executeSuspendProcessAction(); });
    connect(m_resumeProcessButton, &QPushButton::clicked, this, [this]() { executeResumeProcessAction(); });
    connect(m_setCriticalButton, &QPushButton::clicked, this, [this]() { executeSetCriticalAction(true); });
    connect(m_clearCriticalButton, &QPushButton::clicked, this, [this]() { executeSetCriticalAction(false); });
    connect(m_applyPriorityButton, &QPushButton::clicked, this, [this]() { executeSetPriorityAction(); });
    connect(m_openProcessFolderButton, &QPushButton::clicked, this, [this]() { executeOpenProcessFolderAction(); });
    connect(m_refreshPplProtectionButton, &QPushButton::clicked, this, [this]() { executeRefreshPplProtectionLevelAction(); });
    connect(m_enableEfficiencyModeButton, &QPushButton::clicked, this, [this]() { executeSetEfficiencyModeAction(true); });
    connect(m_disableEfficiencyModeButton, &QPushButton::clicked, this, [this]() { executeSetEfficiencyModeAction(false); });
    connect(m_r0TerminateProcessButton, &QPushButton::clicked, this, [this]() { executeR0TerminateProcessAction(); });
    connect(m_r0SuspendProcessButton, &QPushButton::clicked, this, [this]() { executeR0SuspendProcessAction(); });

    // R0 PPL 菜单：
    // - 菜单内容与进程列表右键菜单保持一致；
    // - 每次点击按钮时动态创建局部 QMenu，避免窗口生命周期内持有陈旧 QAction。
    connect(m_r0SetPplButton, &QPushButton::clicked, this, [this]() {
        QMenu r0PplMenu(this);
        r0PplMenu.setStyleSheet(buildProcessDetailMenuStyle());
        QAction* noneAction = r0PplMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_critical.svg")),
            QStringLiteral("关闭PPL保护 (0x00)"));
        noneAction->setData(0x00U);
        r0PplMenu.addSeparator();

        struct PplSignerPreset
        {
            int signerValue = 0;             // signerValue：PPL Signer 数值。
            const char* signerName = "";    // signerName：菜单展示名称。
            const char* meaningText = "";   // meaningText：菜单展示释义。
            bool supportedByDriver = false; // supportedByDriver：当前驱动是否支持。
        };
        const PplSignerPreset presetList[] =
        {
            { 1, "Authenticode", "签名代码（Authenticode）", true },
            { 2, "CodeGen", "动态代码生成", true },
            { 3, "Antimalware", "反恶意软件", true },
            { 4, "Lsa", "本地安全机构", true },
            { 5, "Windows", "Windows 组件", true },
            { 6, "WinTcb", "可信计算基础（最高）", true },
            { 7, "WinSystem", "系统 signer（当前驱动未启用）", false }
        };
        for (const PplSignerPreset& presetEntry : presetList)
        {
            const unsigned int protectionLevel =
                (static_cast<unsigned int>(presetEntry.signerValue) << 4U) | 0x01U;
            const QString protectionLevelHexText = QStringLiteral("0x%1")
                .arg(protectionLevel, 2, 16, QChar('0'))
                .toUpper();
            QAction* presetAction = r0PplMenu.addAction(
                buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_critical.svg")),
                QStringLiteral("%1 (%2) → %3 [%4]")
                .arg(QString::fromLatin1(presetEntry.signerName))
                .arg(presetEntry.signerValue)
                .arg(QString::fromUtf8(presetEntry.meaningText))
                .arg(protectionLevelHexText));
            presetAction->setData(protectionLevel);
            if (!presetEntry.supportedByDriver)
            {
                presetAction->setEnabled(false);
                presetAction->setToolTip(QStringLiteral("该 Signer 在当前驱动下暂无签名级别联动映射。"));
            }
        }

        QAction* selectedAction = r0PplMenu.exec(m_r0SetPplButton->mapToGlobal(QPoint(0, m_r0SetPplButton->height())));
        if (selectedAction == nullptr)
        {
            return;
        }
        const unsigned int levelValue = selectedAction->data().toUInt();
        if (levelValue > 0xFFU)
        {
            kLogEvent actionEvent;
            warn << actionEvent
                << "[ProcessDetailWindow] R0 PPL 层级菜单值无效, levelValue="
                << levelValue
                << eol;
            showActionResultMessage(
                QStringLiteral("R0设置PPL层级"),
                false,
                std::string("invalid PPL level value"),
                actionEvent);
            return;
        }
        executeR0SetPplProtectionAction(
            static_cast<std::uint8_t>(levelValue),
            selectedAction->text());
    });

    // R0 可恢复隐藏菜单：
    // - 通过 ArkDriverClient 发送可恢复隐藏/恢复请求；
    // - 菜单项显式 tooltip 说明具体内核侧变更策略。
    connect(m_r0VisibilityButton, &QPushButton::clicked, this, [this]() {
        QMenu visibilityMenu(this);
        visibilityMenu.setStyleSheet(buildProcessDetailMenuStyle());
        QAction* hideUnlinkOnlyAction = visibilityMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_suspend.svg")),
            QStringLiteral("隐藏当前进程：只断链"));
        QAction* hidePatchPidOnlyAction = visibilityMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_uncritical.svg")),
            QStringLiteral("隐藏当前进程：只改PID"));
        QAction* hideLegacyBothAction = visibilityMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_critical.svg")),
            QStringLiteral("隐藏当前进程：改PID+断链(旧版高风险)"));
        visibilityMenu.addSeparator();
        QAction* unhideProcessAction = visibilityMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_resume.svg")),
            QStringLiteral("取消隐藏当前进程"));
        QAction* clearHiddenAction = visibilityMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_refresh.svg")),
            QStringLiteral("清空全部隐藏标记"));
        hideUnlinkOnlyAction->setToolTip(QStringLiteral("只摘除 ActiveProcessLinks，不修改 PID；更容易按原 PID 找回和恢复。"));
        hidePatchPidOnlyAction->setToolTip(QStringLiteral("只修改 UniqueProcessId，不摘链；高风险，可能影响按原 PID 查找目标。"));
        hideLegacyBothAction->setToolTip(QStringLiteral("兼容旧版：同时修改 UniqueProcessId 并摘除 ActiveProcessLinks；风险最高。"));
        unhideProcessAction->setToolTip(QStringLiteral("恢复由 Ksword 记录的 UniqueProcessId 和进程链表位置。"));
        clearHiddenAction->setToolTip(QStringLiteral("恢复所有由 Ksword 摘链的进程，并清空驱动内记录。"));

        QAction* selectedAction = visibilityMenu.exec(m_r0VisibilityButton->mapToGlobal(QPoint(0, m_r0VisibilityButton->height())));
        if (selectedAction == hideUnlinkOnlyAction)
        {
            executeR0SetProcessHiddenAction(true, KSWORD_ARK_PROCESS_VISIBILITY_FLAG_UNLINK_ACTIVE_LIST);
        }
        else if (selectedAction == hidePatchPidOnlyAction)
        {
            executeR0SetProcessHiddenAction(true, KSWORD_ARK_PROCESS_VISIBILITY_FLAG_PATCH_UNIQUE_PID);
        }
        else if (selectedAction == hideLegacyBothAction)
        {
            executeR0SetProcessHiddenAction(true, KSWORD_ARK_PROCESS_VISIBILITY_FLAG_LEGACY_BOTH);
        }
        else if (selectedAction == unhideProcessAction)
        {
            executeR0SetProcessHiddenAction(false);
        }
        else if (selectedAction == clearHiddenAction)
        {
            executeR0ClearProcessHiddenAction();
        }
    });

    // R0 危险标志/DKOM 菜单：
    // - BreakOnTermination/APC/DKOM 与列表右键菜单能力对齐；
    // - 高风险确认在动作函数内部完成，菜单本身只负责分发。
    connect(m_r0DangerFlagsButton, &QPushButton::clicked, this, [this]() {
        QMenu dangerMenu(this);
        dangerMenu.setStyleSheet(buildProcessDetailMenuStyle());
        QAction* enableBreakAction = dangerMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_critical.svg")),
            QStringLiteral("启用 BreakOnTermination"));
        QAction* disableBreakAction = dangerMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_uncritical.svg")),
            QStringLiteral("关闭 BreakOnTermination"));
        QAction* disableApcAction = dangerMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_suspend.svg")),
            QStringLiteral("禁止APC插入(现有线程)"));
        dangerMenu.addSeparator();
        QAction* dkomCidRemoveAction = dangerMenu.addAction(
            buildProcessDetailR0ActionIcon(QStringLiteral(":/Icon/process_uncritical.svg")),
            QStringLiteral("DKOM从PspCidTable删除"));
        enableBreakAction->setToolTip(QStringLiteral("调用 ZwSetInformationProcess(ProcessBreakOnTermination=1)。"));
        disableBreakAction->setToolTip(QStringLiteral("调用 ZwSetInformationProcess(ProcessBreakOnTermination=0)。"));
        disableApcAction->setToolTip(QStringLiteral("清除目标进程现有线程 ETHREAD ApcQueueable 位。"));
        dkomCidRemoveAction->setToolTip(QStringLiteral("从 PspCidTable 清零目标 EPROCESS 的 CID 表项；高风险且不可通过本菜单恢复。"));

        QAction* selectedAction = dangerMenu.exec(m_r0DangerFlagsButton->mapToGlobal(QPoint(0, m_r0DangerFlagsButton->height())));
        if (selectedAction == enableBreakAction)
        {
            executeR0SetBreakOnTerminationAction(true);
        }
        else if (selectedAction == disableBreakAction)
        {
            executeR0SetBreakOnTerminationAction(false);
        }
        else if (selectedAction == disableApcAction)
        {
            executeR0DisableApcInsertionAction();
        }
        else if (selectedAction == dkomCidRemoveAction)
        {
            executeR0DkomRemoveFromCidTableAction();
        }
    });
    connect(m_injectDllButton, &QPushButton::clicked, this, [this]() { executeInjectDllAction(); });
    connect(m_injectShellcodeButton, &QPushButton::clicked, this, [this]() { executeInjectShellcodeAction(); });

    // 浏览 DLL 路径。
    connect(m_browseDllButton, &QPushButton::clicked, this, [this]() {
        const QString filePath = QFileDialog::getOpenFileName(
            this,
            "选择要注入的 DLL",
            QString(),
            "DLL Files (*.dll);;All Files (*)");
        if (!filePath.isEmpty())
        {
            m_dllPathLineEdit->setText(filePath);
        }
    });

    // 浏览 shellcode 文件路径。
    connect(m_browseShellcodeButton, &QPushButton::clicked, this, [this]() {
        const QString filePath = QFileDialog::getOpenFileName(
            this,
            "选择 shellcode 文件",
            QString(),
            "Binary Files (*.bin *.dat);;All Files (*)");
        if (!filePath.isEmpty())
        {
            m_shellcodePathLineEdit->setText(filePath);
        }
    });

    // 模块刷新按钮。
    connect(m_refreshModuleButton, &QPushButton::clicked, this, [this]() {
        kLogEvent logEvent;
        info << logEvent
            << "[ProcessDetailWindow] 用户点击“刷新模块”, pid=" << m_baseRecord.pid
            << eol;
        requestAsyncModuleRefresh(true);
    });

    // 模块表右键菜单。
    connect(m_moduleTable, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showModuleContextMenu(localPosition);
    });
}

void ProcessDetailWindow::refreshDetailTabTexts()
{
    // 详情刷新入口日志：记录当前 PID 与进程名。
    kLogEvent refreshDetailEvent;
    dbg << refreshDetailEvent
        << "[ProcessDetailWindow] refreshDetailTabTexts: pid="
        << m_baseRecord.pid
        << ", processName="
        << m_baseRecord.processName
        << eol;

    // 顶部标题与图标。
    m_processTitleLabel->setText(
        QString("%1  (PID: %2)")
        .arg(QString::fromStdString(m_baseRecord.processName.empty() ? "Unknown" : m_baseRecord.processName))
        .arg(m_baseRecord.pid));
    m_processIconLabel->setPixmap(resolveProcessIcon(m_baseRecord.imagePath, 40).pixmap(40, 40));

    // 路径与命令行。
    QString processPathText = QString::fromStdString(m_baseRecord.imagePath);
    if (processPathText.trimmed().isEmpty() && m_baseRecord.pid != 0)
    {
        // 兜底再查一次路径，避免 UI 出现“路径始终为空”。
        processPathText = QString::fromStdString(ks::process::QueryProcessPathByPid(m_baseRecord.pid));
        if (!processPathText.trimmed().isEmpty())
        {
            m_baseRecord.imagePath = processPathText.toStdString();
        }
    }
    m_pathLineEdit->setText(processPathText.trimmed().isEmpty() ? "-" : processPathText);
    m_commandLineEdit->setText(QString::fromStdString(m_baseRecord.commandLine.empty() ? "-" : m_baseRecord.commandLine));
    if (m_openHandleDockButton != nullptr)
    {
        m_openHandleDockButton->setVisible(m_baseRecord.pid != 0);
    }

    // 详细字段赋值。
    m_detailStartTimeValue->setText(QString::fromStdString(m_baseRecord.startTimeText.empty() ? "-" : m_baseRecord.startTimeText));
    m_detailUserValue->setText(QString::fromStdString(m_baseRecord.userName.empty() ? "-" : m_baseRecord.userName));
    m_detailAdminValue->setText(m_baseRecord.isAdmin ? "■ 是" : "■ 否");
    m_detailAdminValue->setStyleSheet(
        m_baseRecord.isAdmin
        ? buildStateLabelStyle(signatureTrustedColor(), 700)
        : buildStateLabelStyle(signatureUntrustedColor(), 700));
    m_detailArchitectureValue->setText(QString::fromStdString(m_baseRecord.architectureText.empty() ? "Unknown" : m_baseRecord.architectureText));
    m_detailPriorityValue->setText(QString::fromStdString(m_baseRecord.priorityText.empty() ? "Unknown" : m_baseRecord.priorityText));
    m_detailSessionValue->setText(QString::number(m_baseRecord.sessionId));
    m_detailThreadCountValue->setText(QString::number(m_baseRecord.threadCount));
    m_detailHandleCountValue->setText(QString::number(m_baseRecord.handleCount));
    m_detailCpuValue->setText(formatDoubleText(m_baseRecord.cpuPercent, 2) + "%");
    m_detailRamValue->setText(formatDoubleText(m_baseRecord.ramMB, 1) + " MB");
    m_detailDiskValue->setText(formatDoubleText(m_baseRecord.diskMBps, 2) + " MB/s");
    m_detailSignatureValue->setText(QString::fromStdString(m_baseRecord.signatureState.empty() ? "Unknown" : m_baseRecord.signatureState));
    if (!m_baseRecord.signatureTrusted && m_baseRecord.signatureState != "Pending")
    {
        m_detailSignatureValue->setStyleSheet(
            buildStateLabelStyle(signatureUntrustedColor(), 700));
    }
    else if (m_baseRecord.signatureTrusted)
    {
        m_detailSignatureValue->setStyleSheet(
            buildStateLabelStyle(signatureTrustedColor(), 700));
    }
    else
    {
        m_detailSignatureValue->setStyleSheet(
            buildStateLabelStyle(statusSecondaryColor(), 600));
    }

    // 刷新父进程信息区。
    refreshParentProcessSection();
    refreshKernelObjectTabTexts();
    updateWindowTitle();

    // 详情刷新完成日志：确认核心字段已落到 UI。
    kLogEvent refreshDetailFinishEvent;
    dbg << refreshDetailFinishEvent
        << "[ProcessDetailWindow] refreshDetailTabTexts: 完成, signatureState="
        << m_baseRecord.signatureState
        << ", user="
        << m_baseRecord.userName
        << eol;
}

void ProcessDetailWindow::refreshKernelObjectTabTexts()
{
    // 内核对象页刷新：
    // - 所有字段都来自当前 ProcessRecord 缓存；
    // - 不在 UI 刷新时额外请求 R0，避免详情窗口无意触发内核枚举。
    if (m_kernelObjectTab == nullptr)
    {
        return;
    }

    const bool protectionPresent =
        (m_baseRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_PROTECTION_PRESENT) != 0U;
    const bool signaturePresent =
        (m_baseRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SIGNATURE_LEVEL_PRESENT) != 0U;
    const bool sectionSignaturePresent =
        (m_baseRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SECTION_SIGNATURE_LEVEL_PRESENT) != 0U;
    const bool objectTableAvailable =
        (m_baseRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_OBJECT_TABLE_AVAILABLE) != 0U;
    const bool sectionObjectAvailable =
        (m_baseRecord.r0FieldFlags & KSWORD_ARK_PROCESS_FIELD_SECTION_OBJECT_AVAILABLE) != 0U;

    if (m_kernelObjectR0StatusValue != nullptr)
    {
        m_kernelObjectR0StatusValue->setText(detailProcessR0StatusText(m_baseRecord.r0Status));
        m_kernelObjectR0StatusValue->setStyleSheet(
            buildStateLabelStyle(
                m_baseRecord.r0Status == KSWORD_ARK_PROCESS_R0_STATUS_OK
                ? statusIdleColor()
                : statusWarningColor(),
                700));
    }

    if (m_kernelObjectCapabilityValue != nullptr)
    {
        m_kernelObjectCapabilityValue->setText(detailProcessCapabilityText(m_baseRecord.r0DynDataCapabilityMask));
    }
    if (m_kernelObjectImagePathValue != nullptr)
    {
        m_kernelObjectImagePathValue->setText(
            QString::fromStdString(m_baseRecord.r0ImagePath.empty() ? std::string("-") : m_baseRecord.r0ImagePath));
    }

    if (m_kernelObjectHandleTableValue != nullptr)
    {
        m_kernelObjectHandleTableValue->setText(detailProcessPointerText(
            QStringLiteral("HandleTable available"),
            objectTableAvailable,
            m_baseRecord.r0ObjectTableAddress,
            m_baseRecord.r0ObjectTableSource));
        m_kernelObjectHandleTableValue->setStyleSheet(
            buildStateLabelStyle(objectTableAvailable ? statusIdleColor() : statusSecondaryColor(), 700));
    }
    if (m_kernelObjectSectionObjectValue != nullptr)
    {
        m_kernelObjectSectionObjectValue->setText(detailProcessPointerText(
            QStringLiteral("SectionObject available"),
            sectionObjectAvailable,
            m_baseRecord.r0SectionObjectAddress,
            m_baseRecord.r0SectionObjectSource));
        m_kernelObjectSectionObjectValue->setStyleSheet(
            buildStateLabelStyle(sectionObjectAvailable ? statusIdleColor() : statusSecondaryColor(), 700));
    }

    if (m_kernelObjectProtectionValue != nullptr)
    {
        m_kernelObjectProtectionValue->setText(protectionPresent
            ? detailProcessByteHexText(m_baseRecord.r0Protection)
            : QStringLiteral("Unavailable"));
    }
    if (m_kernelObjectSignatureValue != nullptr)
    {
        m_kernelObjectSignatureValue->setText(signaturePresent
            ? detailProcessByteHexText(m_baseRecord.r0SignatureLevel)
            : QStringLiteral("Unavailable"));
    }
    if (m_kernelObjectSectionSignatureValue != nullptr)
    {
        m_kernelObjectSectionSignatureValue->setText(sectionSignaturePresent
            ? detailProcessByteHexText(m_baseRecord.r0SectionSignatureLevel)
            : QStringLiteral("Unavailable"));
    }

    if (m_kernelObjectSessionSourceValue != nullptr) m_kernelObjectSessionSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0SessionSource));
    if (m_kernelObjectImagePathSourceValue != nullptr) m_kernelObjectImagePathSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0ImagePathSource));
    if (m_kernelObjectProtectionSourceValue != nullptr) m_kernelObjectProtectionSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0ProtectionSource));
    if (m_kernelObjectSignatureSourceValue != nullptr) m_kernelObjectSignatureSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0SignatureLevelSource));
    if (m_kernelObjectSectionSignatureSourceValue != nullptr) m_kernelObjectSectionSignatureSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0SectionSignatureLevelSource));
    if (m_kernelObjectObjectTableSourceValue != nullptr) m_kernelObjectObjectTableSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0ObjectTableSource));
    if (m_kernelObjectSectionObjectSourceValue != nullptr) m_kernelObjectSectionObjectSourceValue->setText(detailProcessFieldSourceText(m_baseRecord.r0SectionObjectSource));

    if (m_kernelObjectProtectionOffsetValue != nullptr) m_kernelObjectProtectionOffsetValue->setText(detailProcessOffsetText(m_baseRecord.r0ProtectionOffset));
    if (m_kernelObjectSignatureOffsetValue != nullptr) m_kernelObjectSignatureOffsetValue->setText(detailProcessOffsetText(m_baseRecord.r0SignatureLevelOffset));
    if (m_kernelObjectSectionSignatureOffsetValue != nullptr) m_kernelObjectSectionSignatureOffsetValue->setText(detailProcessOffsetText(m_baseRecord.r0SectionSignatureLevelOffset));
    if (m_kernelObjectObjectTableOffsetValue != nullptr) m_kernelObjectObjectTableOffsetValue->setText(detailProcessOffsetText(m_baseRecord.r0ObjectTableOffset));
    if (m_kernelObjectSectionObjectOffsetValue != nullptr) m_kernelObjectSectionObjectOffsetValue->setText(detailProcessOffsetText(m_baseRecord.r0SectionObjectOffset));
}

void ProcessDetailWindow::refreshParentProcessSection()
{
    // 父进程区域刷新日志：记录父 PID。
    kLogEvent refreshParentEvent;
    dbg << refreshParentEvent
        << "[ProcessDetailWindow] refreshParentProcessSection: parentPid="
        << m_baseRecord.parentPid
        << eol;

    // 默认先隐藏“转到父进程”，只有父进程仍存在才显示。
    m_gotoParentButton->setVisible(false);
    m_gotoParentButton->setProperty("parent_pid", QVariant());

    if (m_baseRecord.parentPid == 0)
    {
        m_parentInfoLabel->setText("无父进程信息");
        m_parentIconLabel->setPixmap(QIcon(":/Icon/process_main.svg").pixmap(20, 20));
        return;
    }

    const std::uint32_t parentPid = m_baseRecord.parentPid;
    const std::string parentName = ks::process::GetProcessNameByPID(parentPid);
    const bool parentAlive = !parentName.empty();

    if (parentAlive)
    {
        m_parentInfoLabel->setText(
            QString("%1 (PID: %2)")
            .arg(QString::fromStdString(parentName))
            .arg(parentPid));
        const std::string parentPath = ks::process::QueryProcessPathByPid(parentPid);
        m_parentIconLabel->setPixmap(resolveProcessIcon(parentPath, 20).pixmap(20, 20));
        m_gotoParentButton->setVisible(true);
        m_gotoParentButton->setProperty("parent_pid", QVariant::fromValue(parentPid));
        kLogEvent refreshParentAliveEvent;
        dbg << refreshParentAliveEvent
            << "[ProcessDetailWindow] refreshParentProcessSection: 父进程可访问, parentPid="
            << parentPid
            << eol;
    }
    else
    {
        m_parentInfoLabel->setText(QString("父进程已退出或不可访问 (PID: %1)").arg(parentPid));
        m_parentIconLabel->setPixmap(QIcon(":/Icon/process_main.svg").pixmap(20, 20));
        kLogEvent refreshParentDeadEvent;
        warn << refreshParentDeadEvent
            << "[ProcessDetailWindow] refreshParentProcessSection: 父进程不可访问, parentPid="
            << parentPid
            << eol;
    }
}

void ProcessDetailWindow::updateWindowTitle()
{
    // 标题更新日志：便于多窗口场景排查标题错乱。
    kLogEvent updateTitleEvent;
    dbg << updateTitleEvent
        << "[ProcessDetailWindow] updateWindowTitle: pid="
        << m_baseRecord.pid
        << eol;

    setWindowTitle(
        QString("进程详细信息 - %1 (PID %2)")
        .arg(QString::fromStdString(m_baseRecord.processName.empty() ? "Unknown" : m_baseRecord.processName))
        .arg(m_baseRecord.pid));
}

