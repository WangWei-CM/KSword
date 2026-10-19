// ============================================================
// RxpfPage.cpp
// 作用：RXPF 模拟执行实验页面的 UI 实现。
// 页面使用 ParameterWorkspace 两栏结构：
//   参数区：目标选择、写入数据、页面状态
//   动作区：两排操作按钮（观察类 + 生命周期类）
//   结果区：CodeEditorWidget 操作轨迹与事件日志
// 所有 R0 调用委托给 RxpfBackend，页面只组合共享组件。
// ============================================================

#include "RxpfPage.h"

#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../UI/FlatButtonTheme.h"
#include "../../UI/CodeEditorWidget.h"

#include "../../ArkDriverClient/ArkDriverClient.h"
#include "../../Internationalization/LanguageManager.h"
#include "../../theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace
{
    // hexAddress：将 64 位地址格式化为十六进制。
    QString hexAddress(const std::uint64_t addr)
    {
        if (addr == 0ULL)
        {
            return QStringLiteral("N/A");
        }
        return QStringLiteral("0x%1")
            .arg(addr, 16, 16, QLatin1Char('0')).toUpper();
    }

    // hexU32：将 32 位值格式化为十六进制。
    QString hexU32(const unsigned long val)
    {
        return QStringLiteral("0x%1")
            .arg(val, 8, 16, QLatin1Char('0')).toUpper();
    }

    // timestamp：返回当前时间戳字符串。
    QString timestamp()
    {
        return QDateTime::currentDateTime().toString(
            QStringLiteral("HH:mm:ss.zzz"));
    }

    // makeIconButton：
    // - 作用：创建一个纯图标按钮，含释义 tooltip；
    // - iconPath：项目 Resource/Icon/ 下的相对路径；
    // - tooltip：悬停释义文本。
    QPushButton* makeIconButton(
        const QString& iconPath,
        const QString& tooltip,
        QWidget* parent)
    {
        auto* btn = new QPushButton(parent);
        btn->setIcon(QIcon(QStringLiteral(":/Icon/%1").arg(iconPath)));
        btn->setIconSize(KswordTheme::StandardIconSize());
        btn->setFixedSize(KswordTheme::StandardIconButtonSize());
        btn->setToolTip(tooltip);
        btn->setFocusPolicy(Qt::TabFocus);
        return btn;
    }
}

namespace ks::misc
{
    // ================================================================
    // 构造与事件
    // ================================================================

    RxpfPage::RxpfPage(QWidget* parent)
        : QWidget(parent)
    {
        initializeUi();
        initializeConnections();
    }

    void RxpfPage::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        // 首次可见时查询 RXPF 支持状态。
        if (m_firstShow)
        {
            m_firstShow = false;
            doQuerySupport();
        }
    }

    void RxpfPage::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event == nullptr)
        {
            return;
        }
        // 主题切换时刷新风险横幅配色。
        if (event->type() == QEvent::ApplicationPaletteChange
            || event->type() == QEvent::PaletteChange)
        {
            applyBannerStyle();
        }
    }

    // applyBannerStyle：使用当前主题的语义色下发风险横幅样式。
    void RxpfPage::applyBannerStyle()
    {
        if (m_warningLabel == nullptr)
        {
            return;
        }
        m_warningLabel->setStyleSheet(
            QStringLiteral(
                "QLabel{padding:10px;border:1px solid %1;border-radius:5px;"
                "background:%2;color:%3;font-weight:600;}")
                .arg(KswordTheme::WarningHex())
                .arg(KswordTheme::ThemeColorName(
                    KswordTheme::WarningBackgroundColor()))
                .arg(KswordTheme::TextPrimaryHex()));
    }

    // ================================================================
    // initializeUi：构建完整的页面布局。
    // ================================================================
    void RxpfPage::initializeUi()
    {
        // rootLayout：页面根布局。
        auto* rootLayout = new QVBoxLayout(this);
        ks::ui::StyleWorkspacePage(this, 0);
        rootLayout->setSpacing(10);

        // ===================== 风险说明区（固定顶部） =====================
        auto* introduction = new QWidget(this);
        auto* introLayout = new QVBoxLayout(introduction);
        introLayout->setContentsMargins(16, 16, 16, 0);
        introLayout->setSpacing(8);
        rootLayout->addWidget(introduction);

        // 风险横幅：影子 IDT + #PF 接管的后果说明。
        m_warningLabel = new QLabel(
            QStringLiteral(
                "⚠ RXPF 模拟执行会替换系统 IDT 的 #PF 向量，"
                "在影子中断描述符表中运行自定义 Page Fault 处理器。"
                "异常处理器内的任何错误都可能导致双重错误蓝屏。"
                "请先保存工作，仅在测试环境中使用。"),
            introduction);
        m_warningLabel->setWordWrap(true);
        applyBannerStyle();
        introLayout->addWidget(m_warningLabel);

        // RXPF 支持状态摘要。
        m_supportLabel = new QLabel(introduction);
        m_supportLabel->setWordWrap(true);
        m_supportLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        introLayout->addWidget(m_supportLabel);

        // ===================== ParameterWorkspace =====================
        auto* workspace = new ks::ui::ParameterWorkspace(this, 480);
        rootLayout->addWidget(workspace, 1);

        // ===================== 参数：目标选择 =====================
        auto* targetGroup = new QGroupBox(this);
        targetGroup->setTitle(QStringLiteral("目标选择"));
        auto* targetLayout = new QVBoxLayout(targetGroup);
        targetLayout->setSpacing(6);
        ks::ui::StylePrimaryGroup(targetGroup);

        // 目标类型下拉框。
        auto* kindLabel = new QLabel(
            QStringLiteral("目标类型："), targetGroup);
        targetLayout->addWidget(kindLabel);

        m_targetKindCombo = new QComboBox(targetGroup);
        // 三种目标类型对应 R0 协议。
        m_targetKindCombo->addItem(
            QStringLiteral("驱动分配测试页（最安全）"),
            static_cast<uint>(rxpf::kTargetAllocatedTest));
        m_targetKindCombo->addItem(
            QStringLiteral("驱动自身映像页"),
            static_cast<uint>(rxpf::kTargetSelfImageTest));
        m_targetKindCombo->addItem(
            QStringLiteral("外部内核映像页"),
            static_cast<uint>(rxpf::kTargetExternalImage));
        m_targetKindCombo->setToolTip(
            QStringLiteral("选择实验目标类型：测试页最安全；外部映像需要手动输入地址"));
        targetLayout->addWidget(m_targetKindCombo);

        // 目标地址输入（仅外部映像需要）。
        auto* addrLabel = new QLabel(
            QStringLiteral("目标地址（十六进制，外部映像时必填）："),
            targetGroup);
        targetLayout->addWidget(addrLabel);

        m_targetAddressEdit = new QLineEdit(targetGroup);
        m_targetAddressEdit->setPlaceholderText(
            QStringLiteral("例如：FFFFF80012345000"));
        m_targetAddressEdit->setToolTip(
            QStringLiteral("内核虚拟地址，指向目标映像 .text 节中某一页的起始"));
        targetLayout->addWidget(m_targetAddressEdit);

        ks::ui::StyleParameterSection(targetGroup);
        workspace->parameters()->addWidget(targetGroup);

        // ===================== 参数：写入数据 =====================
        auto* writeGroup = new QGroupBox(this);
        writeGroup->setTitle(QStringLiteral("写入数据"));
        auto* writeLayout = new QVBoxLayout(writeGroup);
        writeLayout->setSpacing(6);
        ks::ui::StylePrimaryGroup(writeGroup);

        // 页内偏移。
        auto* offLabel = new QLabel(
            QStringLiteral("页内偏移（十六进制）："), writeGroup);
        writeLayout->addWidget(offLabel);

        m_writeOffsetEdit = new QLineEdit(writeGroup);
        m_writeOffsetEdit->setText(QStringLiteral("0"));
        m_writeOffsetEdit->setToolTip(
            QStringLiteral("写入位置相对页面起始的偏移（0~FFF）"));
        writeLayout->addWidget(m_writeOffsetEdit);

        // 写入字节。
        auto* bytesLabel = new QLabel(
            QStringLiteral("写入数据（十六进制字节串）："), writeGroup);
        writeLayout->addWidget(bytesLabel);

        m_writeBytesEdit = new QLineEdit(writeGroup);
        m_writeBytesEdit->setText(QStringLiteral("90"));
        m_writeBytesEdit->setPlaceholderText(
            QStringLiteral("例如：90 = NOP，CC = INT3"));
        m_writeBytesEdit->setToolTip(
            QStringLiteral("要写入的字节（十六进制），最多 64 字节"));
        writeLayout->addWidget(m_writeBytesEdit);

        ks::ui::StyleParameterSection(writeGroup);
        workspace->parameters()->addWidget(writeGroup);

        // ===================== 参数：页面状态显示 =====================
        auto* stateGroup = new QGroupBox(this);
        stateGroup->setTitle(QStringLiteral("页面状态"));
        auto* stateLayout = new QVBoxLayout(stateGroup);
        stateLayout->setSpacing(4);
        ks::ui::StylePrimaryGroup(stateGroup);

        m_recordIdLabel = new QLabel(
            QStringLiteral("记录 ID：未注册"), stateGroup);
        m_recordIdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        stateLayout->addWidget(m_recordIdLabel);

        m_pageStateLabel = new QLabel(
            QStringLiteral("页面状态：无"), stateGroup);
        m_pageStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        stateLayout->addWidget(m_pageStateLabel);

        m_countersLabel = new QLabel(
            QStringLiteral("计数器：—"), stateGroup);
        m_countersLabel->setWordWrap(true);
        m_countersLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        stateLayout->addWidget(m_countersLabel);

        ks::ui::StyleParameterSection(stateGroup);
        workspace->parameters()->addWidget(stateGroup);

        // ===================== 动作区：第一排（观察类） =====================
        auto* actionRow1 = new QWidget(this);
        auto* actionLayout1 = new QHBoxLayout(actionRow1);
        actionLayout1->setContentsMargins(0, 0, 0, 0);
        actionLayout1->setSpacing(8);

        // 查询支持按钮。
        m_querySupportBtn = makeIconButton(
            QStringLiteral("rxpf_query_support.svg"),
            QStringLiteral("查询 RXPF 子系统支持状态"),
            actionRow1);
        actionLayout1->addWidget(m_querySupportBtn);

        // 自检按钮。
        m_selfTestBtn = makeIconButton(
            QStringLiteral("rxpf_self_test.svg"),
            QStringLiteral("运行端到端自检（驱动自动注册→变更→模拟→验证）"),
            actionRow1);
        actionLayout1->addWidget(m_selfTestBtn);

        // 查询统计按钮。
        m_queryStatsBtn = makeIconButton(
            QStringLiteral("rxpf_query_stats.svg"),
            QStringLiteral("查询全局模拟统计信息"),
            actionRow1);
        actionLayout1->addWidget(m_queryStatsBtn);

        // 拉取事件按钮。
        m_drainEventsBtn = makeIconButton(
            QStringLiteral("rxpf_drain_events.svg"),
            QStringLiteral("拉取最新模拟事件（#PF 处理记录）"),
            actionRow1);
        actionLayout1->addWidget(m_drainEventsBtn);

        actionLayout1->addStretch();
        workspace->actions()->addWidget(actionRow1);

        // ===================== 动作区：第二排（生命周期操作） =====================
        auto* actionRow2 = new QWidget(this);
        auto* actionLayout2 = new QHBoxLayout(actionRow2);
        actionLayout2->setContentsMargins(0, 0, 0, 0);
        actionLayout2->setSpacing(8);

        // 注册页面按钮。
        m_registerBtn = makeIconButton(
            QStringLiteral("rxpf_register.svg"),
            QStringLiteral("注册目标页面用于 #PF 模拟"),
            actionRow2);
        ks::ui::ApplyFlatButtonTheme(m_registerBtn,
            ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
        actionLayout2->addWidget(m_registerBtn);

        // 变更保护按钮。
        m_changeBtn = makeIconButton(
            QStringLiteral("rxpf_change.svg"),
            QStringLiteral("将页面保护从 RX 改为 RW/NX"),
            actionRow2);
        m_changeBtn->setEnabled(false);
        actionLayout2->addWidget(m_changeBtn);

        // 写入数据按钮。
        m_writeBtn = makeIconButton(
            QStringLiteral("rxpf_write.svg"),
            QStringLiteral("向页面可写别名写入数据"),
            actionRow2);
        m_writeBtn->setEnabled(false);
        actionLayout2->addWidget(m_writeBtn);

        // 启用模拟按钮。
        m_emulateBtn = makeIconButton(
            QStringLiteral("rxpf_emulate.svg"),
            QStringLiteral("启用/禁用此页面的指令模拟"),
            actionRow2);
        m_emulateBtn->setEnabled(false);
        actionLayout2->addWidget(m_emulateBtn);

        // 查询页面按钮。
        m_queryPageBtn = makeIconButton(
            QStringLiteral("rxpf_query_page.svg"),
            QStringLiteral("查询已注册页面的当前状态与计数器"),
            actionRow2);
        m_queryPageBtn->setEnabled(false);
        actionLayout2->addWidget(m_queryPageBtn);

        // Phase G 开关：注销时跳过驱动的 Backup→page 自动恢复。
        // checked 时 Unregister 不还原原字节 → R3 写入保留到 PTE 回 RX，
        // 调用方必须在 PatchGuard 扫到之前自己改回去，否则必蓝屏。
        // 默认 off（走 Phase F 安全往返路径），tooltip 明确警告。
        m_keepModifiedToggle = makeIconButton(
            QStringLiteral("rxpf_keep_modified.svg"),
            QStringLiteral("保留 R3 写入字节；注销后 Backup 不自动恢复（有蓝屏风险）"),
            actionRow2);
        m_keepModifiedToggle->setCheckable(true);
        m_keepModifiedToggle->setChecked(false);
        // Accent + Flat：未勾时透明工具按钮；勾中后 FlatButtonState::Checked
        // 会由共享主题给到强调底色，视觉提示"保留修改"这档危险模式已开。
        ks::ui::ApplyFlatButtonTheme(m_keepModifiedToggle,
            ks::ui::FlatButtonTone::Accent,
            ks::ui::FlatButtonAppearance::Flat);
        actionLayout2->addWidget(m_keepModifiedToggle);

        // 注销按钮（保留原整串键 tooltip，兼容既有 i18n 词条）。
        m_unregisterBtn = makeIconButton(
            QStringLiteral("rxpf_unregister.svg"),
            QStringLiteral("注销页面并恢复原始保护"),
            actionRow2);
        ks::ui::ApplyFlatButtonTheme(m_unregisterBtn,
            ks::ui::FlatButtonTone::Danger,
            ks::ui::FlatButtonAppearance::Solid);
        m_unregisterBtn->setEnabled(false);
        actionLayout2->addWidget(m_unregisterBtn);

        actionLayout2->addStretch();
        workspace->actions()->addWidget(actionRow2);

        // ===================== 结果区 =====================
        m_traceEdit = new CodeEditorWidget(this);
        m_traceEdit->setReadOnly(true);
        m_traceEdit->setMinimumHeight(200);
        workspace->results()->addWidget(m_traceEdit, 1);
    }

    // ================================================================
    // initializeConnections：按钮信号连接。
    // ================================================================
    void RxpfPage::initializeConnections()
    {
        connect(m_querySupportBtn, &QPushButton::clicked,
            this, &RxpfPage::doQuerySupport);
        connect(m_selfTestBtn, &QPushButton::clicked,
            this, &RxpfPage::doSelfTest);
        connect(m_queryStatsBtn, &QPushButton::clicked,
            this, &RxpfPage::doQueryStats);
        connect(m_drainEventsBtn, &QPushButton::clicked,
            this, &RxpfPage::doDrainEvents);
        connect(m_registerBtn, &QPushButton::clicked,
            this, &RxpfPage::doRegister);
        connect(m_changeBtn, &QPushButton::clicked,
            this, &RxpfPage::doChangeProtection);
        connect(m_writeBtn, &QPushButton::clicked,
            this, &RxpfPage::doWritePage);
        connect(m_emulateBtn, &QPushButton::clicked,
            this, &RxpfPage::doSetEmulation);
        connect(m_queryPageBtn, &QPushButton::clicked,
            this, &RxpfPage::doQueryPage);
        connect(m_unregisterBtn, &QPushButton::clicked,
            this, &RxpfPage::doUnregister);
    }

    // ================================================================
    // 操作处理函数
    // ================================================================

    void RxpfPage::doQuerySupport()
    {
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.querying_support"),
            QStringLiteral("[%1] 正在查询 RXPF 支持状态..."))
            .arg(timestamp()));

        // 异步查询支持状态。
        auto* watcher = new QFutureWatcher<rxpf::SupportInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::SupportInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    // IOCTL 通信失败。
                    m_supportLabel->setText(
                        QStringLiteral("RXPF 支持查询失败：%1")
                            .arg(info.ioErrorText));
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.query_failed"),
                        QStringLiteral("[%1] 查询失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                // 更新支持状态摘要。
                m_supportLabel->setText(
                    QStringLiteral(
                        "构建状态：%1 | NT 构建号：%2 | 处理器数：%3\n"
                        "支持标志：%4\n"
                        "函数 RVA：%5 | 参数数：%6 | 最近状态：%7")
                        .arg(rxpf::buildStatusText(info.buildStatus))
                        .arg(info.ntBuildNumber)
                        .arg(info.processorCount)
                        .arg(rxpf::supportFlagsText(info.supportFlags))
                        .arg(hexU32(info.functionRva))
                        .arg(info.functionParameterCount)
                        .arg(hexU32(static_cast<unsigned long>(info.lastStatus))));

                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.query_support_done"),
                    QStringLiteral("[%1] 支持查询完成：构建=%2 标志=0x%3"))
                    .arg(timestamp())
                    .arg(rxpf::buildStatusText(info.buildStatus))
                    .arg(info.supportFlags, 8, 16, QLatin1Char('0')));
            });
        watcher->setFuture(QtConcurrent::run(rxpf::querySupport));
    }

    void RxpfPage::doRegister()
    {
        // 读取目标类型和地址。
        const unsigned long targetKind =
            m_targetKindCombo->currentData().toUInt();
        std::uint64_t targetAddr = 0ULL;

        // 外部映像需要手动输入地址。
        if (targetKind == rxpf::kTargetExternalImage)
        {
            bool ok = false;
            targetAddr = m_targetAddressEdit->text()
                .trimmed().toULongLong(&ok, 16);
            if (!ok || targetAddr == 0ULL)
            {
                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.external_image_needs_address"),
                    QStringLiteral("[%1] 外部映像目标需要有效的十六进制地址"))
                    .arg(timestamp()));
                return;
            }
        }

        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.registering_page"),
            QStringLiteral("[%1] 正在注册页面 (类型=%2 地址=%3)..."))
            .arg(timestamp())
            .arg(targetKind)
            .arg(hexAddress(targetAddr)));

        // 异步注册页面。
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.register_failed"),
                        QStringLiteral("[%1] 注册失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                // 保存记录 ID。
                m_currentRecordId = info.recordId;
                updateStateDisplay(info);

                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.register_success"),
                    QStringLiteral("[%1] 注册成功：记录ID=%2 页基址=%3 状态=%4"))
                    .arg(timestamp())
                    .arg(info.recordId)
                    .arg(hexAddress(info.pageBase))
                    .arg(rxpf::pageStateText(info.state)));
            });
        watcher->setFuture(QtConcurrent::run(
            [targetKind, targetAddr]()
            {
                return rxpf::registerPage(targetKind, targetAddr);
            }));
    }

    void RxpfPage::doChangeProtection()
    {
        if (m_currentRecordId == 0ULL)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.please_register_first"),
                QStringLiteral("[%1] 请先注册页面")).arg(timestamp()));
            return;
        }

        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.changing_protection"),
            QStringLiteral("[%1] 正在变更页面保护 (RX→NX/RW)..."))
            .arg(timestamp()));

        const auto recordId = m_currentRecordId;
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.change_protection_failed"),
                        QStringLiteral("[%1] 变更保护失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                updateStateDisplay(info);
                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.change_protection_done"),
                    QStringLiteral("[%1] 保护变更完成：状态=%2 原保护=%3 现保护=%4"))
                    .arg(timestamp())
                    .arg(rxpf::pageStateText(info.state))
                    .arg(hexU32(info.originalProtection))
                    .arg(hexU32(info.currentProtection)));
            });
        watcher->setFuture(QtConcurrent::run(
            [recordId]() { return rxpf::changePage(recordId); }));
    }

    void RxpfPage::doWritePage()
    {
        if (m_currentRecordId == 0ULL)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.please_register_and_change"),
                QStringLiteral("[%1] 请先注册并变更保护")).arg(timestamp()));
            return;
        }

        // 解析偏移。
        bool offsetOk = false;
        const unsigned long offset =
            m_writeOffsetEdit->text().trimmed().toULong(&offsetOk, 16);
        if (!offsetOk)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.offset_format_error"),
                QStringLiteral("[%1] 偏移格式错误")).arg(timestamp()));
            return;
        }

        // 解析十六进制字节串。
        const QString hexStr = m_writeBytesEdit->text()
            .trimmed().remove(QChar(' '));
        if (hexStr.isEmpty() || (hexStr.size() % 2) != 0)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.data_format_error"),
                QStringLiteral("[%1] 数据格式错误（需偶数位十六进制）"))
                .arg(timestamp()));
            return;
        }

        QByteArray bytes;
        for (int i = 0; i < hexStr.size(); i += 2)
        {
            bool byteOk = false;
            const auto byte = static_cast<char>(
                hexStr.mid(i, 2).toUShort(&byteOk, 16));
            if (!byteOk)
            {
                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.invalid_hex_byte"),
                    QStringLiteral("[%1] 无效的十六进制字节：%2"))
                    .arg(timestamp(), hexStr.mid(i, 2)));
                return;
            }
            bytes.append(byte);
        }

        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.writing_bytes"),
            QStringLiteral("[%1] 正在写入 %2 字节到偏移 0x%3..."))
            .arg(timestamp())
            .arg(bytes.size())
            .arg(offset, 0, 16));

        const auto recordId = m_currentRecordId;
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.write_failed"),
                        QStringLiteral("[%1] 写入失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                updateStateDisplay(info);
                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.write_done"),
                    QStringLiteral("[%1] 写入完成：状态=%2 NTSTATUS=%3"))
                    .arg(timestamp())
                    .arg(rxpf::pageStateText(info.state))
                    .arg(hexU32(static_cast<unsigned long>(info.lastStatus))));
            });
        watcher->setFuture(QtConcurrent::run(
            [recordId, offset, bytes]()
            {
                return rxpf::writePage(recordId, offset, bytes);
            }));
    }

    void RxpfPage::doSetEmulation()
    {
        if (m_currentRecordId == 0ULL)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.please_register_first"),
                QStringLiteral("[%1] 请先注册页面")).arg(timestamp()));
            return;
        }

        // 切换启用/禁用。
        const bool newEnable = !m_emulationEnabled;
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.toggling_emulation"),
            QStringLiteral("[%1] 正在%2模拟..."))
            .arg(timestamp())
            .arg(newEnable
                ? ks::i18n::text(QStringLiteral("rxpf.trace.enable_verb"),
                    QStringLiteral("启用"))
                : ks::i18n::text(QStringLiteral("rxpf.trace.disable_verb"),
                    QStringLiteral("禁用"))));

        const auto recordId = m_currentRecordId;
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished,
            this, [this, watcher, newEnable]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.emulation_control_failed"),
                        QStringLiteral("[%1] 模拟控制失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                // 更新本地状态。
                m_emulationEnabled = (info.emulationEnabled != 0UL);
                updateStateDisplay(info);

                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.emulation_toggled"),
                    QStringLiteral("[%1] 模拟%2：状态=%3 模拟启用=%4"))
                    .arg(timestamp())
                    .arg(newEnable
                        ? ks::i18n::text(QStringLiteral("rxpf.trace.enabled_state"),
                            QStringLiteral("已启用"))
                        : ks::i18n::text(QStringLiteral("rxpf.trace.disabled_state"),
                            QStringLiteral("已禁用")))
                    .arg(rxpf::pageStateText(info.state))
                    .arg(info.emulationEnabled));
            });
        watcher->setFuture(QtConcurrent::run(
            [recordId, newEnable]()
            {
                return rxpf::setEmulation(recordId, newEnable);
            }));
    }

    void RxpfPage::doQueryPage()
    {
        if (m_currentRecordId == 0ULL)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.please_register_first"),
                QStringLiteral("[%1] 请先注册页面")).arg(timestamp()));
            return;
        }

        setBusy(true);
        const auto recordId = m_currentRecordId;
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.query_page_failed"),
                        QStringLiteral("[%1] 查询页面失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                updateStateDisplay(info);
                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.query_page_done"),
                    QStringLiteral("[%1] 页面查询：状态=%2 错误=%3 模拟=%4 PFN=%5"))
                    .arg(timestamp())
                    .arg(rxpf::pageStateText(info.state))
                    .arg(info.faultCount)
                    .arg(info.emulatedCount)
                    .arg(hexAddress(info.pfn)));
            });
        watcher->setFuture(QtConcurrent::run(
            [recordId]() { return rxpf::queryPage(recordId); }));
    }

    void RxpfPage::doQueryStats()
    {
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.querying_stats"),
            QStringLiteral("[%1] 正在查询全局统计..."))
            .arg(timestamp()));

        auto* watcher = new QFutureWatcher<rxpf::StatsInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::StatsInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.stats_query_failed"),
                        QStringLiteral("[%1] 统计查询失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                // 输出详细统计。
                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.stats_result"),
                    QStringLiteral(
                    "[%1] 全局统计：\n"
                    "  注册页面=%2  启用模拟=%3  IDT已安装=%4\n"
                    "  处理器数=%5  活跃处理器=%6\n"
                    "  总错误=%7  托管=%8  已模拟=%9  转发=%10\n"
                    "  递归=%11  不支持=%12  丢弃事件=%13"))
                    .arg(timestamp())
                    .arg(info.registeredPages)
                    .arg(info.enabledPages)
                    .arg(info.idtInstalled)
                    .arg(info.processorCount)
                    .arg(info.activeHandlers)
                    .arg(info.totalFaults)
                    .arg(info.managedFaults)
                    .arg(info.emulatedInstructions)
                    .arg(info.chainedFaults)
                    .arg(info.recursiveFaults)
                    .arg(info.unsupportedInstructions)
                    .arg(info.droppedEvents));
            });
        watcher->setFuture(QtConcurrent::run(rxpf::queryStats));
    }

    void RxpfPage::doDrainEvents()
    {
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.draining_events"),
            QStringLiteral("[%1] 正在拉取事件 (序列号>%2)..."))
            .arg(timestamp())
            .arg(m_lastEventSequence));

        const auto afterSeq = m_lastEventSequence;
        auto* watcher = new QFutureWatcher<rxpf::EventsInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::EventsInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.drain_events_failed"),
                        QStringLiteral("[%1] 事件拉取失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.drain_events_summary"),
                    QStringLiteral("[%1] 返回 %2 条事件（可用 %3，丢弃 %4）"))
                    .arg(timestamp())
                    .arg(info.returnedRows)
                    .arg(info.availableRows)
                    .arg(info.droppedRows));

                // 逐行输出事件。
                for (const auto& row : info.rows)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.event_row"),
                        QStringLiteral("  #%1 CPU%2 CR2=%3 RIP=%4 → %5 指令=%6 结果=%7"))
                        .arg(row.sequence)
                        .arg(row.processorIndex)
                        .arg(hexAddress(row.cr2))
                        .arg(hexAddress(row.rip))
                        .arg(hexAddress(row.newRip))
                        .arg(rxpf::decodedInstructionText(row.decodedInstruction))
                        .arg(rxpf::emulationResultText(row.emulationResult)));
                }

                // 更新序列号：用本批最后一条实际返回行的序号，
                // 不用 newestSequence（可能远超本批），避免跳过中间记录。
                if (!info.rows.empty())
                {
                    const auto lastReturned = info.rows.back().sequence;
                    if (lastReturned > m_lastEventSequence)
                    {
                        m_lastEventSequence = lastReturned;
                    }
                }
            });
        watcher->setFuture(QtConcurrent::run(
            [afterSeq]()
            {
                return rxpf::drainEvents(afterSeq, KSWORD_ARK_RXPF_MAX_EVENT_ROWS);
            }));
    }

    void RxpfPage::doSelfTest()
    {
        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("rxpf.trace.running_self_test"),
            QStringLiteral("[%1] 正在运行端到端自检..."))
            .arg(timestamp()));

        auto* watcher = new QFutureWatcher<rxpf::SelfTestInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::SelfTestInfo>::finished,
            this, [this, watcher]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.self_test_io_failed"),
                        QStringLiteral("[%1] 自检失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                // resultText：自检通过/失败的可读文本。
                const QString resultText = (info.result != 0UL)
                    ? ks::i18n::text(QStringLiteral("rxpf.trace.self_test_passed"),
                        QStringLiteral("通过"))
                    : ks::i18n::text(QStringLiteral("rxpf.trace.self_test_failed_verdict"),
                        QStringLiteral("失败"));

                appendTrace(ks::i18n::text(
                    QStringLiteral("rxpf.trace.self_test_result"),
                    QStringLiteral(
                    "[%1] 自检结果：%2\n"
                    "  模拟指令数=%3  #PF 次数=%4\n"
                    "  返回值=%5  期望值=%6\n"
                    "  记录ID=%7  NTSTATUS=%8"))
                    .arg(timestamp())
                    .arg(resultText)
                    .arg(info.instructionCount)
                    .arg(info.faultsObserved)
                    .arg(hexAddress(info.returnedValue))
                    .arg(hexAddress(info.expectedValue))
                    .arg(info.recordId)
                    .arg(hexU32(static_cast<unsigned long>(info.lastStatus))));
            });
        // recordId：当前已注册页面的 ID，自检需要已处于 RW_NX 状态的记录。
        const auto recordId = m_currentRecordId;
        watcher->setFuture(QtConcurrent::run([recordId]() {
            return rxpf::runSelfTest(recordId);
        }));
    }

    void RxpfPage::doUnregister()
    {
        if (m_currentRecordId == 0ULL)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("rxpf.trace.no_registered_page"),
                QStringLiteral("[%1] 没有已注册的页面")).arg(timestamp()));
            return;
        }

        // 读取 Phase G 开关状态：checked = 不自动恢复 Backup。
        // 读在 setBusy 前，以便 appendTrace 的措辞与真实 flag 一致。
        const bool keepModified = m_keepModifiedToggle != nullptr
            && m_keepModifiedToggle->isChecked();

        setBusy(true);
        appendTrace(ks::i18n::text(
            keepModified
                ? QStringLiteral("rxpf.trace.unregistering_page_keep")
                : QStringLiteral("rxpf.trace.unregistering_page"),
            keepModified
                ? QStringLiteral(
                    "[%1] 正在注销页面 (ID=%2) [保留修改：Backup 不恢复]...")
                : QStringLiteral("[%1] 正在注销页面 (ID=%2)..."))
            .arg(timestamp())
            .arg(m_currentRecordId));

        const auto recordId = m_currentRecordId;
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished,
            this, [this, watcher, keepModified]()
            {
                const auto info = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                if (!info.ioSuccess)
                {
                    appendTrace(ks::i18n::text(
                        QStringLiteral("rxpf.trace.unregister_failed"),
                        QStringLiteral("[%1] 注销失败：%2"))
                        .arg(timestamp(), info.ioErrorText));
                    return;
                }

                // 清除本地状态。
                m_currentRecordId = 0ULL;
                m_emulationEnabled = false;
                updateStateDisplay(info);

                appendTrace(ks::i18n::text(
                    keepModified
                        ? QStringLiteral("rxpf.trace.page_unregistered_keep")
                        : QStringLiteral("rxpf.trace.page_unregistered"),
                    keepModified
                        ? QStringLiteral(
                            "[%1] 页面已注销；R3 写入字节保留 - 若未恢复则 PatchGuard 必蓝屏")
                        : QStringLiteral("[%1] 页面已注销"))
                    .arg(timestamp()));
            });
        // keepModified 透传给 Backend，决定是否带 FLAG_SKIP_BACKUP_RESTORE。
        watcher->setFuture(QtConcurrent::run(
            [recordId, keepModified]()
            {
                return rxpf::unregisterPage(recordId, keepModified);
            }));
    }

    // ================================================================
    // UI 辅助函数
    // ================================================================

    // updateStateDisplay：根据页面状态刷新标签和按钮可用性。
    void RxpfPage::updateStateDisplay(const rxpf::PageInfo& info)
    {
        // hasRecord：是否有活跃的记录。
        const bool hasRecord = (m_currentRecordId != 0ULL);

        // isRwNx：页面是否已变更为 RW/NX 状态。
        const bool isRwNx =
            (info.state == KSWORD_ARK_RXPF_PAGE_STATE_RW_NX);

        // 更新状态标签。
        m_recordIdLabel->setText(
            hasRecord
                ? QStringLiteral("记录 ID：%1").arg(m_currentRecordId)
                : QStringLiteral("记录 ID：未注册"));

        m_pageStateLabel->setText(
            QStringLiteral("页面状态：%1")
                .arg(rxpf::pageStateText(info.state)));

        m_countersLabel->setText(
            QStringLiteral(
                "#PF 次数：%1  已模拟：%2  不支持：%3  模拟启用：%4")
                .arg(info.faultCount)
                .arg(info.emulatedCount)
                .arg(info.unsupportedCount)
                .arg(info.emulationEnabled != 0UL
                    ? QStringLiteral("是") : QStringLiteral("否")));

        // 更新按钮可用性。
        m_registerBtn->setEnabled(!hasRecord && !m_busy);
        m_changeBtn->setEnabled(hasRecord && !m_busy);
        m_writeBtn->setEnabled(hasRecord && isRwNx && !m_busy);
        m_emulateBtn->setEnabled(hasRecord && !m_busy);
        m_queryPageBtn->setEnabled(hasRecord && !m_busy);
        m_unregisterBtn->setEnabled(hasRecord && !m_busy);
    }

    // appendTrace：追加一行到操作日志。
    void RxpfPage::appendTrace(const QString& line)
    {
        if (m_traceEdit != nullptr)
        {
            m_traceEdit->appendRawText(line, true);
        }
    }

    // setBusy：操作进行中禁用/启用所有按钮。
    void RxpfPage::setBusy(const bool busy)
    {
        m_busy = busy;
        m_querySupportBtn->setEnabled(!busy);
        m_selfTestBtn->setEnabled(!busy);
        m_queryStatsBtn->setEnabled(!busy);
        m_drainEventsBtn->setEnabled(!busy);

        // 生命周期按钮的可用性还依赖记录状态。
        if (busy)
        {
            m_registerBtn->setEnabled(false);
            m_changeBtn->setEnabled(false);
            m_writeBtn->setEnabled(false);
            m_emulateBtn->setEnabled(false);
            m_queryPageBtn->setEnabled(false);
            m_unregisterBtn->setEnabled(false);
        }
        else
        {
            // 恢复时根据当前记录状态决定。
            const bool hasRecord = (m_currentRecordId != 0ULL);
            m_registerBtn->setEnabled(!hasRecord);
            m_changeBtn->setEnabled(hasRecord);
            m_writeBtn->setEnabled(hasRecord);
            m_emulateBtn->setEnabled(hasRecord);
            m_queryPageBtn->setEnabled(hasRecord);
            m_unregisterBtn->setEnabled(hasRecord);
        }
    }

} // namespace ks::misc
