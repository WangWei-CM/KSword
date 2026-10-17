#include "HvciBypassPage.h"

// ============================================================
// HvciBypassPage.cpp
// 作用：HVCI W^X 绕过实验页面的 UI 实现。
// 页面使用 ParameterWorkspace 两栏结构：
//   左/上：参数区（目标模块、偏移、标志位）
//   右/下：结果区（状态码、读回字节、判定文本、操作轨迹日志）
// 所有 R0 调用委托给 HvciBypassBackend，页面只组合共享组件。
// ============================================================

#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../UI/FlatButtonTheme.h"
#include "../../UI/CodeEditorWidget.h"

#include "../../ArkDriverClient/ArkDriverClient.h"
#include "../../Internationalization/LanguageManager.h"
#include "../../theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include <QDateTime>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace
{
    // ntstatusHex：将 NTSTATUS 值转换为可读的十六进制字符串。
    QString ntstatusHex(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned long>(status), 8, 16, QLatin1Char('0'))
            .toUpper();
    }

    // byteHex：将单字节值转换为十六进制字符串（如 "0x41"）。
    QString byteHex(const std::uint8_t value)
    {
        return QStringLiteral("0x%1")
            .arg(value, 2, 16, QLatin1Char('0'))
            .toUpper();
    }

    // addressHex：将 64 位地址转换为可读的十六进制字符串。
    QString addressHex(const std::uint64_t address)
    {
        if (address == 0ULL)
        {
            return QStringLiteral("不可用");
        }
        return QStringLiteral("0x%1")
            .arg(address, 16, 16, QLatin1Char('0'))
            .toUpper();
    }
}

namespace ks::misc
{
    HvciBypassPage::HvciBypassPage(QWidget* parent)
        : QWidget(parent)
    {
        initializeUi();
        initializeConnections();
    }

    void HvciBypassPage::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);

        // 首次可见时查询姿态并枚举模块。
        if (m_firstShow)
        {
            m_firstShow = false;
            refreshPosture();
            refreshModules();
        }
    }

    void HvciBypassPage::changeEvent(QEvent* event)
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

    // applyBannerStyle 作用：
    // - 使用当前主题的语义色下发风险横幅样式；
    // - 构造期与主题切换后走同一条路径，避免重复代码。
    void HvciBypassPage::applyBannerStyle()
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

    // initializeUi 作用：
    // - 按 ParameterWorkspace 模式构建根布局、参数区、动作区和结果区；
    // - 参数区：风险横幅、姿态摘要、目标模块下拉、偏移输入、标志位勾选；
    // - 动作区：刷新姿态/模块、执行实验；
    // - 结果区：各阶段状态码、读回字节证据、判定文本、操作轨迹日志。
    void HvciBypassPage::initializeUi()
    {
        auto& language = ks::i18n::LanguageManager::instance();

        // 根布局不叠加边距，交给 ParameterWorkspace 和说明区各自处理。
        auto* rootLayout = new QVBoxLayout(this);
        ks::ui::StyleWorkspacePage(this, 0);
        rootLayout->setSpacing(10);

        // ===================== 风险说明区 =====================
        // 风险说明固定在页面顶部，不随参数滚动。
        auto* introduction = new QWidget(this);
        auto* introductionLayout = new QVBoxLayout(introduction);
        introductionLayout->setContentsMargins(16, 16, 16, 0);
        introductionLayout->setSpacing(8);
        rootLayout->addWidget(introduction);

        // 永久风险横幅：HVCI 绕过的后果与 PatchGuard 警告。
        m_warningLabel = new QLabel(
            QStringLiteral(
                "⚠ HVCI 绕过实验会在内核映像页上执行保护变更与物理页修改。"
                "成功绕过后，PatchGuard 可能延迟检测并触发 0x109 蓝屏。"
                "修改后的页面在换出后会从 PE 文件重新加载干净内容。"
                "请先保存工作，仅在测试环境中使用。"),
            introduction);
        m_warningLabel->setWordWrap(true);
        applyBannerStyle();
        introductionLayout->addWidget(m_warningLabel);

        // CI / HVCI 姿态摘要。
        m_postureLabel = new QLabel(introduction);
        m_postureLabel->setWordWrap(true);
        m_postureLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        introductionLayout->addWidget(m_postureLabel);

        // ===================== ParameterWorkspace =====================
        // 参数区独立滚动、执行动作固定、结果区保留剩余高度。
        auto* workspace = new ks::ui::ParameterWorkspace(this, 480);
        rootLayout->addWidget(workspace, 1);

        // ===================== 参数：目标选择 =====================
        auto* targetGroup = new QGroupBox(this);
        language.bindText(
            targetGroup,
            QStringLiteral("misc.hvci_bypass.target.title"),
            QStringLiteral("实验目标"));
        auto* targetLayout = new QVBoxLayout(targetGroup);
        targetLayout->setSpacing(6);
        ks::ui::StylePrimaryGroup(targetGroup);

        // 模块下拉框：列出当前已加载的内核模块供选择。
        auto* moduleLabel = new QLabel(
            QStringLiteral("目标模块："),
            targetGroup);
        targetLayout->addWidget(moduleLabel);

        m_moduleCombo = new QComboBox(targetGroup);
        m_moduleCombo->setToolTip(
            QStringLiteral("选择要实验的内核模块，通常选 CI.dll 的 .text 节首页"));
        targetLayout->addWidget(m_moduleCombo);

        // 页内偏移输入：十六进制，默认 0x10000（CI.dll .text 节起始附近）。
        auto* offsetLabel = new QLabel(
            QStringLiteral("页内偏移（十六进制）："),
            targetGroup);
        targetLayout->addWidget(offsetLabel);

        m_offsetEdit = new QLineEdit(targetGroup);
        m_offsetEdit->setText(QStringLiteral("10000"));
        m_offsetEdit->setPlaceholderText(QStringLiteral("例如：10000"));
        m_offsetEdit->setToolTip(
            QStringLiteral("从模块基址开始的偏移量（十六进制），应指向 .text 节内的某一页"));
        targetLayout->addWidget(m_offsetEdit);

        // 函数地址覆盖：调试用途，跳过签名扫描。
        auto* funcOverrideLabel = new QLabel(
            QStringLiteral("MmChangeImageProtection 地址覆盖（可选）："),
            targetGroup);
        targetLayout->addWidget(funcOverrideLabel);

        m_funcOverrideEdit = new QLineEdit(targetGroup);
        m_funcOverrideEdit->setPlaceholderText(
            QStringLiteral("留空=R0 自动扫描定位；填入十六进制地址=跳过扫描"));
        m_funcOverrideEdit->setToolTip(
            QStringLiteral("调试用途：手动指定 MmChangeImageProtection 的内核地址，跳过 R0 签名扫描"));
        targetLayout->addWidget(m_funcOverrideEdit);

        ks::ui::StyleParameterSection(targetGroup);
        workspace->parameters()->addWidget(targetGroup);

        // ===================== 参数：实验标志位 =====================
        auto* flagsGroup = new QGroupBox(this);
        language.bindText(
            flagsGroup,
            QStringLiteral("misc.hvci_bypass.flags.title"),
            QStringLiteral("实验标志位"));
        auto* flagsLayout = new QVBoxLayout(flagsGroup);
        flagsLayout->setSpacing(6);
        ks::ui::StylePrimaryGroup(flagsGroup);

        // 三个标志位勾选框。
        m_writeTestCheck = new QCheckBox(
            QStringLiteral("WRITE_TEST — 通过别名映射写入测试"),
            flagsGroup);
        m_writeTestCheck->setChecked(true);
        m_writeTestCheck->setToolTip(
            QStringLiteral("请求 R0 在 MDL 别名映射上执行 XOR 写入测试"));
        flagsLayout->addWidget(m_writeTestCheck);

        m_restoreCheck = new QCheckBox(
            QStringLiteral("RESTORE — 写入后恢复 RX 权限"),
            flagsGroup);
        m_restoreCheck->setChecked(true);
        m_restoreCheck->setToolTip(
            QStringLiteral("请求 R0 在写入测试后将页面保护恢复为可读可执行（RX）"));
        flagsLayout->addWidget(m_restoreCheck);

        m_unmapCheck = new QCheckBox(
            QStringLiteral("UNMAP_BEFORE_RESTORE — 恢复前解除别名"),
            flagsGroup);
        m_unmapCheck->setChecked(true);
        m_unmapCheck->setToolTip(
            QStringLiteral(
                "恢复 RX 前先解除 MDL 别名映射，使 VTL1 看不到可写映射，"
                "从而允许恢复执行权限——这是绕过的关键步骤"));
        flagsLayout->addWidget(m_unmapCheck);

        ks::ui::StyleParameterSection(flagsGroup);
        workspace->parameters()->addWidget(flagsGroup);

        // ===================== 动作区 =====================
        m_refreshButton = new QPushButton(this);
        m_refreshButton->setIcon(QIcon(QStringLiteral(":/Icon/process_refresh.svg")));
        ks::ui::ApplyFlatButtonTheme(m_refreshButton,
            ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_refreshButton,
            QStringLiteral("misc.hvci_bypass.action.refresh"),
            QStringLiteral("刷新"));
        language.bindToolTip(
            m_refreshButton,
            QStringLiteral("misc.hvci_bypass.action.refresh.tooltip"),
            QStringLiteral("重新查询 CI/HVCI 姿态并枚举内核模块列表"));
        workspace->actions()->addWidget(m_refreshButton);

        m_executeButton = new QPushButton(this);
        m_executeButton->setIcon(QIcon(QStringLiteral(":/Icon/process_start.svg")));
        ks::ui::ApplyFlatButtonTheme(m_executeButton,
            ks::ui::FlatButtonTone::Danger,
            ks::ui::FlatButtonAppearance::Solid);
        language.bindText(
            m_executeButton,
            QStringLiteral("misc.hvci_bypass.action.execute"),
            QStringLiteral("执行实验"));
        language.bindToolTip(
            m_executeButton,
            QStringLiteral("misc.hvci_bypass.action.execute.tooltip"),
            QStringLiteral("向 R0 发送 HVCI 绕过实验 IOCTL，完整流程包含：解析→MDL→锁页→变更保护→别名写入→恢复"));
        workspace->actions()->addWidget(m_executeButton);
        workspace->actions()->addStretch(1);

        // ===================== 结果区 =====================
        // 各阶段状态码摘要。
        auto* statusTitle = new QLabel(workspace);
        language.bindText(
            statusTitle,
            QStringLiteral("misc.hvci_bypass.result.status_title"),
            QStringLiteral("各阶段状态"));
        statusTitle->setProperty("ksword_detail_section_title", true);
        workspace->results()->addWidget(statusTitle);

        m_statusLabel = new QLabel(workspace);
        m_statusLabel->setWordWrap(true);
        m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->addWidget(m_statusLabel);

        // 读回字节证据。
        auto* evidenceTitle = new QLabel(workspace);
        language.bindText(
            evidenceTitle,
            QStringLiteral("misc.hvci_bypass.result.evidence_title"),
            QStringLiteral("读回字节证据"));
        evidenceTitle->setProperty("ksword_detail_section_title", true);
        workspace->results()->addWidget(evidenceTitle);

        m_evidenceLabel = new QLabel(workspace);
        m_evidenceLabel->setWordWrap(true);
        m_evidenceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->addWidget(m_evidenceLabel);

        // 判定结论。
        auto* verdictTitle = new QLabel(workspace);
        language.bindText(
            verdictTitle,
            QStringLiteral("misc.hvci_bypass.result.verdict_title"),
            QStringLiteral("判定"));
        verdictTitle->setProperty("ksword_detail_section_title", true);
        workspace->results()->addWidget(verdictTitle);

        m_verdictLabel = new QLabel(workspace);
        m_verdictLabel->setWordWrap(true);
        m_verdictLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->addWidget(m_verdictLabel);

        // 操作轨迹日志。
        auto* traceTitle = new QLabel(workspace);
        language.bindText(
            traceTitle,
            QStringLiteral("misc.hvci_bypass.result.trace_title"),
            QStringLiteral("操作轨迹"));
        traceTitle->setProperty("ksword_detail_section_title", true);
        workspace->results()->addWidget(traceTitle);

        m_traceEdit = new CodeEditorWidget(workspace);
        m_traceEdit->setReadOnly(true);
        m_traceEdit->setMinimumHeight(96);
        language.bindToolTip(
            m_traceEdit,
            QStringLiteral("misc.hvci_bypass.result.trace.tooltip"),
            QStringLiteral("完整 IOCTL 请求与响应的时序日志"));
        workspace->results()->addWidget(m_traceEdit, 1);

        // 页面完成构造后为参数区补齐输入控件的表面和行高。
        workspace->finish();
    }

    // initializeConnections 作用：
    // - 把刷新按钮和执行按钮的点击信号连接到对应的处理函数。
    void HvciBypassPage::initializeConnections()
    {
        // 刷新按钮：重新查询姿态并枚举模块。
        connect(m_refreshButton, &QPushButton::clicked,
            this, [this]()
            {
                refreshPosture();
                refreshModules();
            });

        // 执行按钮：在后台线程运行实验。
        connect(m_executeButton, &QPushButton::clicked,
            this, &HvciBypassPage::runExperiment);
    }

    // refreshPosture 作用：
    // - 同步查询 CI/HVCI 姿态（委托后端的 queryCodeIntegrityPosture）；
    // - 将结果更新到姿态摘要标签。
    void HvciBypassPage::refreshPosture()
    {
        const auto posture = hvci_bypass::queryCodeIntegrityPosture();

        // 组装姿态摘要文本。
        QString text;
        if (!posture.queried)
        {
            text = QStringLiteral("姿态查询失败：%1")
                .arg(posture.failureText);
        }
        else
        {
            text = QStringLiteral(
                "CI=%1  HVCI=%2  HVCI Strict=%3  安全启动=%4  测试签名=%5  来源=%6")
                .arg(posture.ciEnabled ? QStringLiteral("启用") : QStringLiteral("关闭"))
                .arg(posture.hvciEnabled ? QStringLiteral("启用") : QStringLiteral("关闭"))
                .arg(posture.hvciStrictMode ? QStringLiteral("启用") : QStringLiteral("关闭"))
                .arg(posture.source == disable_dse::PostureSource::Driver
                    ? (posture.secureBootEnabled ? QStringLiteral("启用") : QStringLiteral("关闭"))
                    : QStringLiteral("未知"))
                .arg(posture.testSigningEnabled ? QStringLiteral("启用") : QStringLiteral("关闭"))
                .arg(posture.source == disable_dse::PostureSource::Driver
                    ? QStringLiteral("R0") : QStringLiteral("R3"));
        }
        m_postureLabel->setText(text);
        appendTrace(ks::i18n::text(
            QStringLiteral("hvci.trace.posture"),
            QStringLiteral("[姿态] %1")).arg(text));
    }

    // refreshModules 作用：
    // - 通过后端枚举内核模块列表；
    // - 清空并重新填充模块下拉框，默认选中 CI.dll。
    void HvciBypassPage::refreshModules()
    {
        // enumResult：模块枚举结果，区分查询失败和结果为空。
        const auto enumResult = hvci_bypass::enumerateKernelModules();
        m_moduleCombo->clear();

        if (!enumResult.ok)
        {
            // 枚举失败——清空缓存并报告错误。
            m_modules.clear();
            appendTrace(ks::i18n::text(
                QStringLiteral("hvci.trace.module_enum_failed"),
                QStringLiteral("[模块] 枚举失败：%1"))
                .arg(enumResult.errorText));
            return;
        }

        // 枚举成功——更新缓存。
        m_modules = enumResult.modules;

        int ciIndex = -1;
        for (int i = 0; i < static_cast<int>(m_modules.size()); ++i)
        {
            const auto& mod = m_modules[static_cast<size_t>(i)];
            // 下拉项格式："模块名 @ 0x基址 (大小)"。
            const QString itemText = QStringLiteral("%1 @ 0x%2 (%3 KB)")
                .arg(mod.name)
                .arg(mod.baseAddress, 16, 16, QLatin1Char('0'))
                .arg(mod.imageSize / 1024);
            m_moduleCombo->addItem(itemText);

            // 自动选中 CI.dll。
            if (mod.name.compare(QStringLiteral("CI.dll"), Qt::CaseInsensitive) == 0)
            {
                ciIndex = i;
            }
        }

        // 仅首次枚举时自动选中 CI.dll；后续刷新保留用户已选模块，
        // 避免用户手动选择其它模块并填入偏移后被重置。
        if (ciIndex >= 0 && m_firstModuleEnum)
        {
            m_moduleCombo->setCurrentIndex(ciIndex);
            m_firstModuleEnum = false;
        }
        appendTrace(ks::i18n::text(
            QStringLiteral("hvci.trace.module_enum_done"),
            QStringLiteral("[模块] 枚举到 %1 个内核模块"))
            .arg(m_modules.size()));
    }

    // runExperiment 作用：
    // - 收集参数（目标地址 = 模块基址 + 偏移、标志位、函数覆盖地址）；
    // - 在后台线程调用后端执行实验；
    // - 完成后在 UI 线程显示结果。
    void HvciBypassPage::runExperiment()
    {
        if (m_busy)
        {
            return;
        }

        // 读取选中模块的基址。
        const int moduleIndex = m_moduleCombo->currentIndex();
        if (moduleIndex < 0 || moduleIndex >= static_cast<int>(m_modules.size()))
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("hvci.trace.no_module_selected"),
                QStringLiteral("[错误] 未选择有效的目标模块")));
            return;
        }
        const std::uint64_t baseAddress =
            m_modules[static_cast<size_t>(moduleIndex)].baseAddress;

        // 解析页内偏移（十六进制）。
        bool offsetOk = false;
        const std::uint64_t offset =
            m_offsetEdit->text().trimmed().toULongLong(&offsetOk, 16);
        if (!offsetOk)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("hvci.trace.invalid_offset_format"),
                QStringLiteral("[错误] 偏移格式无效，请输入十六进制数")));
            return;
        }

        // 计算目标虚拟地址。
        const std::uint64_t targetVa = baseAddress + offset;

        // 组合标志位。
        unsigned long flags = 0UL;
        if (m_writeTestCheck->isChecked())
        {
            flags |= hvci_bypass::kFlagWriteTest;
        }
        if (m_restoreCheck->isChecked())
        {
            flags |= hvci_bypass::kFlagRestore;
        }
        if (m_unmapCheck->isChecked())
        {
            flags |= hvci_bypass::kFlagUnmapBeforeRestore;
        }

        // 解析函数地址覆盖（可选）。
        std::uint64_t funcOverride = 0ULL;
        const QString overrideText = m_funcOverrideEdit->text().trimmed();
        if (!overrideText.isEmpty())
        {
            bool overrideOk = false;
            funcOverride = overrideText.toULongLong(&overrideOk, 16);
            if (!overrideOk)
            {
                appendTrace(ks::i18n::text(
                    QStringLiteral("hvci.trace.invalid_func_override"),
                    QStringLiteral("[错误] 函数地址覆盖格式无效")));
                return;
            }
        }

        setBusy(true);
        appendTrace(ks::i18n::text(
            QStringLiteral("hvci.trace.experiment_start"),
            QStringLiteral("[实验] 目标=%1  flags=0x%2  funcOverride=%3"))
            .arg(addressHex(targetVa))
            .arg(flags, 1, 16)
            .arg(addressHex(funcOverride)));

        // 在后台线程执行 IOCTL，避免阻塞 UI。
        auto future = QtConcurrent::run(
            [targetVa, flags, funcOverride]() -> hvci_bypass::ExperimentResult
            {
                return hvci_bypass::runExperiment(targetVa, flags, funcOverride);
            });

        // 等待结果完成后在 UI 线程刷新显示。
        auto* watcher = new QFutureWatcher<hvci_bypass::ExperimentResult>(this);
        connect(watcher, &QFutureWatcher<hvci_bypass::ExperimentResult>::finished,
            this, [this, watcher]()
            {
                const auto result = watcher->result();
                displayResult(result);
                setBusy(false);
                watcher->deleteLater();
            });
        watcher->setFuture(future);
    }

    // displayResult 作用：
    // - 将 ExperimentResult 的各阶段状态码、读回字节和判定文本
    //   分别更新到结果区的三个标签和操作轨迹日志。
    void HvciBypassPage::displayResult(const hvci_bypass::ExperimentResult& result)
    {
        // 各阶段状态码。
        if (!result.ioSuccess)
        {
            m_statusLabel->setText(
                QStringLiteral("IOCTL 失败：%1").arg(result.ioErrorText));
        }
        else
        {
            m_statusLabel->setText(
                QStringLiteral(
                    "resolverStatus = %1\n"
                    "mdlAllocStatus = %2\n"
                    "lockStatus = %3\n"
                    "imageProtectStatus = %4\n"
                    "aliasMapStatus = %5\n"
                    "writeTestStatus = %6\n"
                    "restoreStatus = %7\n"
                    "resolvedFunctionVa = %8")
                .arg(ntstatusHex(result.resolverStatus))
                .arg(ntstatusHex(result.mdlAllocStatus))
                .arg(ntstatusHex(result.lockStatus))
                .arg(ntstatusHex(result.imageProtectStatus))
                .arg(ntstatusHex(result.aliasMapStatus))
                .arg(ntstatusHex(result.writeTestStatus))
                .arg(ntstatusHex(result.restoreStatus))
                .arg(addressHex(result.resolvedFunctionVa)));
        }

        // 与判定文本共用后端已经证明的读取标记，不再次按零值猜测证据有效性。
        const bool hasAnyByte = result.ioSuccess && (result.hasPreModifyByte
            || result.hasPostWriteByte || result.hasPostRestoreByte);
        const QString unavailable = QStringLiteral("（未读取或读取失败）");
        if (hasAnyByte)
        {
            const QString pre = result.hasPreModifyByte ? byteHex(result.preModifyByte) : unavailable;
            const QString postWrite = result.hasPostWriteByte ? byteHex(result.postWriteByte) : unavailable;
            const QString postRestore = result.hasPostRestoreByte ? byteHex(result.postRestoreByte) : unavailable;
            const std::uint8_t expectedXor =
                static_cast<std::uint8_t>(result.preModifyByte ^ 0xFFU);
            m_evidenceLabel->setText(
                QStringLiteral(
                    "preModifyByte = %1（修改前原始字节）\n"
                    "postWriteByte = %2（别名写入后从原始 VA 读回）\n"
                    "postRestoreByte = %3（恢复 RX 后从原始 VA 读回）\n"
                    "aliasVa = %4\n"
                    "期望 XOR 值 = %5")
                .arg(pre, postWrite, postRestore,
                     addressHex(result.aliasVa),
                     result.hasPreModifyByte ? byteHex(expectedXor) : unavailable));
        }
        else if (result.ioSuccess)
        {
            m_evidenceLabel->setText(
                QStringLiteral("字节证据不可用：未取得有效读回"));
        }
        else
        {
            m_evidenceLabel->setText(QStringLiteral("无读回数据"));
        }

        // 判定文本。
        const QString verdict = hvci_bypass::verdictText(result);
        m_verdictLabel->setText(verdict);

        // 追加到操作轨迹。
        appendTrace(ks::i18n::text(
            QStringLiteral("hvci.trace.result"),
            QStringLiteral("[结果] %1")).arg(verdict));
        if (hasAnyByte)
        {
            appendTrace(ks::i18n::text(
                QStringLiteral("hvci.trace.byte_evidence"),
                QStringLiteral("[字节] pre=%1  postWrite=%2  postRestore=%3"))
                .arg(result.hasPreModifyByte ? byteHex(result.preModifyByte) : unavailable)
                .arg(result.hasPostWriteByte ? byteHex(result.postWriteByte) : unavailable)
                .arg(result.hasPostRestoreByte ? byteHex(result.postRestoreByte) : unavailable));
        }
    }

    // appendTrace 作用：
    // - 追加一行带时间戳的操作轨迹到日志区。
    void HvciBypassPage::appendTrace(const QString& line)
    {
        if (m_traceEdit == nullptr)
        {
            return;
        }
        const QString timestamped = QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
            .arg(line);
        m_traceEdit->appendRawText(timestamped);
    }

    // setBusy 作用：
    // - 操作进行中禁用按钮，防止重复点击。
    void HvciBypassPage::setBusy(const bool busy)
    {
        m_busy = busy;
        if (m_refreshButton != nullptr)
        {
            m_refreshButton->setEnabled(!busy);
        }
        if (m_executeButton != nullptr)
        {
            m_executeButton->setEnabled(!busy);
        }
    }
}
