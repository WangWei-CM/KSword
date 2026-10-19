// ============================================================================
// KernelHookWizard.cpp
// 实现 KernelHookWizard.h 声明的 5 步线性 wizard。
// 流程 overview：
//   Step 0 选目标     选内核模块 + 偏移 → 算绝对 VA
//   Step 1 准备      Register(CAPTURE_BACKUP) + Change(mode=2) + QueryBackup
//   Step 2 写入      选 payload 预设 / 自定义 hex + Write
//   Step 3 保留&触发  Unregister(SKIP_RESTORE) → 等用户在外部触发目标代码
//   Step 4 恢复&验证  Register+Change+Write(backup)+Unregister 恢复，Re-Register 验证
//
// 所有 R0 调用通过 rxpf:: 命名空间（RxpfBackend.h），不直接 CreateFile/DeviceIoControl。
// 异步任务用 QtConcurrent::run + QFutureWatcher 模式（参照 RxpfPage.cpp:977-1005）。
// ============================================================================

#include "KernelHookWizard.h"

#include "../Framework.h"
#include "../MiscDock/HvciBypass/RxpfBackend.h"
#include "../theme.h"
#include "FlatButtonTheme.h"
#include "CodeEditorWidget.h"
#include "SecondaryPageLayout.h"
#include "../Internationalization/LanguageManager.h"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <algorithm>
#include <memory>
#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QtConcurrent/QtConcurrent>
#include <QTextStream>
#include <QVBoxLayout>

#include <psapi.h>

namespace
{
    // timestamp：格式化当前时间为 "HH:mm:ss.zzz"，附在轨迹每一行前。
    QString timestamp()
    {
        return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    }

    // formatHex8：把 8 字节无符号整型格式化为 "0x0000000000000000"。
    QString formatHex8(const quint64 v)
    {
        return QStringLiteral("0x%1").arg(v, 16, 16, QChar('0'));
    }

    // formatHex4：把 4 字节无符号整型格式化为 "0x00000000"。
    QString formatHex4(const quint32 v)
    {
        return QStringLiteral("0x%1").arg(v, 8, 16, QChar('0'));
    }

    // bytesToHex：把 QByteArray 格式化为 "AA BB CC ..." 的 hex 字符串。
    QString bytesToHex(const QByteArray& bytes)
    {
        QString out;
        out.reserve(bytes.size() * 3);
        for (int i = 0; i < bytes.size(); ++i)
        {
            if (i > 0) { out.append(QLatin1Char(' ')); }
            out.append(QStringLiteral("%1").arg(
                static_cast<unsigned char>(bytes[i]), 2, 16, QChar('0')).toUpper());
        }
        return out;
    }

    // parseHexBytes：把用户输入的 "90", "33 C0 C3", "33c0c3" 之类字符串解析为字节数组。
    // 忽略空白；非法字符返回空数组。
    QByteArray parseHexBytes(const QString& text)
    {
        QByteArray out;
        QString cleaned;
        cleaned.reserve(text.size());
        for (const QChar c : text)
        {
            if (!c.isSpace()) { cleaned.append(c); }
        }
        if (cleaned.size() % 2 != 0) { return QByteArray(); }
        for (int i = 0; i < cleaned.size(); i += 2)
        {
            bool ok = false;
            const int b = cleaned.mid(i, 2).toInt(&ok, 16);
            if (!ok || b < 0 || b > 0xFF) { return QByteArray(); }
            out.append(static_cast<char>(b));
        }
        return out;
    }

    // enumerateKernelModule：从 R3 用 psapi 枚举已加载内核驱动的 base。
    // 返回 (name → base) 字典；只保留 name 匹配 wantedNames 的条目。
    QMap<QString, quint64> enumerateKernelModule(const QStringList& wantedNames)
    {
        QMap<QString, quint64> result;
        DWORD needed = 0;
        if (!EnumDeviceDrivers(nullptr, 0, &needed) || needed == 0)
        {
            return result;
        }
        const int count = static_cast<int>(needed / sizeof(LPVOID));
        QVector<LPVOID> bases(count);
        if (!EnumDeviceDrivers(bases.data(), needed, &needed))
        {
            return result;
        }
        for (LPVOID base : bases)
        {
            wchar_t nameBuf[256] = { 0 };
            const DWORD len = GetDeviceDriverBaseNameW(base, nameBuf,
                sizeof(nameBuf) / sizeof(wchar_t));
            if (len == 0) { continue; }
            const QString name = QString::fromWCharArray(nameBuf, static_cast<int>(len));
            // 内核模块名大小写不敏感匹配
            for (const QString& wanted : wantedNames)
            {
                if (name.compare(wanted, Qt::CaseInsensitive) == 0)
                {
                    result.insert(name.toLower(),
                        static_cast<quint64>(reinterpret_cast<quintptr>(base)));
                    break;
                }
            }
        }
        return result;
    }
}

// 命名空间别名：rxpf:: 等效于完全限定的 ks::misc::rxpf::，
// 让 wizard 在 ks::ui 命名空间内也能直接写 rxpf::registerPage 等。
namespace rxpf = ::ks::misc::rxpf;

namespace ks::ui
{
    // ========================================================================
    // openWizard：MainWindow 菜单项的入口。
    // 构造并 show() wizard；WA_DeleteOnClose 保证关闭时自动销毁。
    // 非模态；用户可以在 wizard 开着时继续看主界面。
    // ========================================================================
    void KernelHookWizard::openWizard(QWidget* parent)
    {
        auto* w = new KernelHookWizard(parent);
        w->setAttribute(Qt::WA_DeleteOnClose);
        // SecondaryWindowChrome 统一主题；避免父容器透明样式穿透导致黑底
        ks::ui::StyleSecondaryWindow(w);
        w->show();
        w->raise();
        w->activateWindow();
    }

    // ========================================================================
    // 构造：装 UI + 初始化到第 0 步 + 枚举模块填下拉
    // ========================================================================
    KernelHookWizard::KernelHookWizard(QWidget* parent)
        : QDialog(parent)
    {
        setObjectName(QStringLiteral("ksKernelHookWizard"));
        setWindowTitle(QStringLiteral("内核 Hook 向导"));
        setMinimumSize(820, 640);
        setModal(false);
        initializeUi();
        refreshModules();
        goTo(0);
    }

    KernelHookWizard::~KernelHookWizard() = default;

    // ========================================================================
    // initializeUi：顶层布局 = StackedWidget + 底部导航按钮 + 底部轨迹编辑器
    // ========================================================================
    void KernelHookWizard::initializeUi()
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(10);

        // 顶部：步骤指示器
        m_stepLabel = new QLabel(QStringLiteral("第 1 / 5 步"), this);
        m_stepLabel->setStyleSheet(QStringLiteral(
            "QLabel{font-weight:bold; font-size:14px;}"));
        root->addWidget(m_stepLabel);

        // 中部：5 页堆叠
        m_stack = new QStackedWidget(this);
        m_stack->addWidget(buildTargetPage());
        m_stack->addWidget(buildPreparePage());
        m_stack->addWidget(buildWritePage());
        m_stack->addWidget(buildKeepPage());
        m_stack->addWidget(buildVerifyPage());
        root->addWidget(m_stack, 1);

        // 轨迹编辑器：所有步骤共享，位于底部
        auto* traceGroup = new QGroupBox(QStringLiteral("操作轨迹"), this);
        auto* traceLayout = new QVBoxLayout(traceGroup);
        traceLayout->setContentsMargins(8, 8, 8, 8);
        m_traceEdit = new CodeEditorWidget(this);
        m_traceEdit->setReadOnly(true);
        m_traceEdit->setMinimumHeight(140);
        traceLayout->addWidget(m_traceEdit);
        root->addWidget(traceGroup);

        // 底部：导航按钮
        auto* navRow = new QHBoxLayout();
        navRow->setSpacing(8);
        m_prevBtn = new QPushButton(QStringLiteral("上一步"), this);
        m_nextBtn = new QPushButton(QStringLiteral("下一步"), this);
        m_cancelBtn = new QPushButton(QStringLiteral("关闭"), this);
        ks::ui::ApplyFlatButtonTheme(m_prevBtn,
            ks::ui::FlatButtonTone::Neutral);
        ks::ui::ApplyFlatButtonTheme(m_nextBtn,
            ks::ui::FlatButtonTone::Accent);
        ks::ui::ApplyFlatButtonTheme(m_cancelBtn,
            ks::ui::FlatButtonTone::Neutral);
        m_prevBtn->setMinimumWidth(100);
        m_nextBtn->setMinimumWidth(100);
        m_cancelBtn->setMinimumWidth(100);
        navRow->addStretch();
        navRow->addWidget(m_prevBtn);
        navRow->addWidget(m_nextBtn);
        navRow->addWidget(m_cancelBtn);
        root->addLayout(navRow);

        // 导航信号接线
        connect(m_prevBtn, &QPushButton::clicked, this, [this]() {
            if (m_currentStep > 0) { goTo(m_currentStep - 1); }
        });
        connect(m_nextBtn, &QPushButton::clicked, this, [this]() {
            if (m_currentStep < 4) { goTo(m_currentStep + 1); }
            else { close(); }
        });
        connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::close);
    }

    // ========================================================================
    // buildTargetPage：第 1 步，选模块 + 输入偏移 → 算绝对 VA
    // ========================================================================
    QWidget* KernelHookWizard::buildTargetPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto* hint = new QLabel(QStringLiteral(
            "<b>第 1 步：选择目标</b><br>"
            "选一个内核模块和模块内偏移。Wizard 将对该页面做 Register(CAPTURE_BACKUP)+Change+Write+Unregister 完整往返。<br>"
            "<i>已实测 catalog 内模块 (CI.dll +0x1000) 可被 Phase G 放行；其他模块+偏移组合未实测，可能行为不同。</i>"),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        // 模块下拉
        auto* moduleRow = new QHBoxLayout();
        moduleRow->addWidget(new QLabel(QStringLiteral("目标模块："), page));
        m_moduleCombo = new QComboBox(page);
        m_moduleCombo->setEditable(false);
        m_moduleCombo->setMinimumWidth(200);
        moduleRow->addWidget(m_moduleCombo);
        moduleRow->addStretch();
        layout->addLayout(moduleRow);

        // 偏移输入
        auto* offsetRow = new QHBoxLayout();
        offsetRow->addWidget(new QLabel(QStringLiteral("模块内偏移 (hex)："), page));
        m_offsetEdit = new QLineEdit(QStringLiteral("1000"), page);
        m_offsetEdit->setMinimumWidth(180);
        m_offsetEdit->setPlaceholderText(QStringLiteral("1000"));
        offsetRow->addWidget(m_offsetEdit);
        offsetRow->addStretch();
        layout->addLayout(offsetRow);

        // 绝对 VA 预览
        m_vaLabel = new QLabel(QStringLiteral("绝对 VA：—"), page);
        m_vaLabel->setStyleSheet(QStringLiteral(
            "QLabel{font-family: Consolas, 'Courier New', monospace; padding:4px;}"));
        layout->addWidget(m_vaLabel);

        // 根据模块/偏移变化更新 VA 显示
        auto updateVa = [this]() {
            bool ok = false;
            const QString name = m_moduleCombo->currentText();
            const quint64 base = m_moduleCombo->currentData().toULongLong(&ok);
            if (!ok || base == 0ULL) {
                m_vaLabel->setText(QStringLiteral("绝对 VA：模块未加载"));
                return;
            }
            bool offOk = false;
            const quint64 off = m_offsetEdit->text().toULongLong(&offOk, 16);
            if (!offOk) {
                m_vaLabel->setText(QStringLiteral("绝对 VA：偏移格式错误"));
                return;
            }
            m_plan.moduleName = name;
            m_plan.moduleBase = base;
            m_plan.targetOffset = off;
            m_plan.targetVa = base + off;
            m_vaLabel->setText(QStringLiteral("绝对 VA：%1 (= %2 + %3)")
                .arg(formatHex8(m_plan.targetVa), formatHex8(base), formatHex8(off)));
        };
        connect(m_moduleCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [updateVa](int){ updateVa(); });
        connect(m_offsetEdit, &QLineEdit::textChanged,
            this, [updateVa](const QString&){ updateVa(); });

        layout->addStretch();
        return page;
    }

    // ========================================================================
    // buildPreparePage：第 2 步，Register(CB) + Change(mode=2) + QueryBackup
    // ========================================================================
    QWidget* KernelHookWizard::buildPreparePage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto* hint = new QLabel(QStringLiteral(
            "<b>第 2 步：准备页面</b><br>"
            "对目标页做 Register(CAPTURE_BACKUP) 让驱动保存原字节快照，然后 Change(mode=2) 翻 PTE 到 RW/NX 并拿到可写别名，最后 QueryBackup 把原字节前 16 字节读回来显示。<br>"
            "<i>Backup 的完整 4KB 存在驱动里；R3 只读前 16 字节用于 UI 展示和恢复 payload 覆盖范围。</i>"),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        m_prepareBtn = new QPushButton(QStringLiteral("执行准备步骤"), page);
        ks::ui::ApplyFlatButtonTheme(m_prepareBtn,
            ks::ui::FlatButtonTone::Accent);
        m_prepareBtn->setMinimumHeight(36);
        layout->addWidget(m_prepareBtn);

        m_prepareStatusLabel = new QLabel(QStringLiteral("状态：未执行"), page);
        layout->addWidget(m_prepareStatusLabel);

        m_backupHexLabel = new QLabel(QStringLiteral("原字节 (前 16 字节)：—"), page);
        m_backupHexLabel->setStyleSheet(QStringLiteral(
            "QLabel{font-family: Consolas, 'Courier New', monospace; padding:4px;}"));
        m_backupHexLabel->setWordWrap(true);
        layout->addWidget(m_backupHexLabel);

        connect(m_prepareBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onPrepareClicked);

        layout->addStretch();
        return page;
    }

    // ========================================================================
    // buildWritePage：第 3 步，选 payload + Write
    // ========================================================================
    QWidget* KernelHookWizard::buildWritePage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto* hint = new QLabel(QStringLiteral(
            "<b>第 3 步：写入 payload</b><br>"
            "任意 hex 字符串作为 payload，长度与页内偏移由你自己定。"
            "预设只是一键填充，不代表只能用这些。原字节已在上一步存入 Backup 可随后恢复。<br>"
            "<i>payload+offset 的合法范围由驱动按单页约束校验；GUI 不替你拒绝。</i>"),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        // Payload 预设下拉：仅做"一键填充 hex 输入框"的便捷功能，不限制用户输入
        auto* presetRow = new QHBoxLayout();
        presetRow->addWidget(new QLabel(QStringLiteral("Payload 预设："), page));
        m_payloadPresetCombo = new QComboBox(page);
        m_payloadPresetCombo->addItem(QStringLiteral("自定义（不自动覆盖）"), QString());
        m_payloadPresetCombo->addItem(QStringLiteral("ret (C3)"), QStringLiteral("C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("xor eax,eax; ret (33 C0 C3)"),
            QStringLiteral("33 C0 C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("NOP (90)"), QStringLiteral("90"));
        m_payloadPresetCombo->addItem(QStringLiteral("int 3 (CC)"), QStringLiteral("CC"));
        m_payloadPresetCombo->setToolTip(QStringLiteral(
            "快捷填充 payload hex 输入框；选任何一项都只是便捷填充，payload 本身可任意编辑"));
        m_payloadPresetCombo->setMinimumWidth(260);
        presetRow->addWidget(m_payloadPresetCombo);
        presetRow->addStretch();
        layout->addLayout(presetRow);

        // Payload hex 输入：用户任意字节序列（长度不设 GUI 侧上限，由 driver 校验）
        auto* hexRow = new QHBoxLayout();
        hexRow->addWidget(new QLabel(QStringLiteral("Payload (hex)："), page));
        m_payloadHexEdit = new QLineEdit(QStringLiteral("C3"), page);
        m_payloadHexEdit->setMinimumWidth(360);
        m_payloadHexEdit->setPlaceholderText(QStringLiteral(
            "任意 hex，空格可选，例如 48 83 EC 20 ...；长度不设 GUI 上限"));
        m_payloadHexEdit->setToolTip(QStringLiteral(
            "用户任意 hex 字符串；GUI 不设 payload 长度上限，由驱动按单页约束校验"));
        hexRow->addWidget(m_payloadHexEdit);
        hexRow->addStretch();
        layout->addLayout(hexRow);

        // Payload offset 输入：用户指定页内写入偏移（支持 0x 前缀 hex / 十进制）
        auto* offsetRow = new QHBoxLayout();
        offsetRow->addWidget(new QLabel(QStringLiteral("Offset (页内)："), page));
        m_payloadOffsetEdit = new QLineEdit(QStringLiteral("0"), page);
        m_payloadOffsetEdit->setMinimumWidth(140);
        m_payloadOffsetEdit->setPlaceholderText(QStringLiteral("0x0 ~ 0xFFF 或十进制"));
        m_payloadOffsetEdit->setToolTip(QStringLiteral(
            "页内写入偏移；用户任意指定；offset+payload 长度是否越页由驱动校验"));
        offsetRow->addWidget(m_payloadOffsetEdit);
        offsetRow->addStretch();
        layout->addLayout(offsetRow);

        // 预设选中：仅当 data 非空（非"自定义"）才覆盖 hex 输入框，避免覆盖用户正在编辑的内容
        connect(m_payloadPresetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                const QString v = m_payloadPresetCombo->currentData().toString();
                if (!v.isEmpty()) { m_payloadHexEdit->setText(v); }
            });

        m_writeBtn = new QPushButton(QStringLiteral("写入 payload"), page);
        ks::ui::ApplyFlatButtonTheme(m_writeBtn, ks::ui::FlatButtonTone::Accent);
        m_writeBtn->setMinimumHeight(36);
        layout->addWidget(m_writeBtn);

        m_writeStatusLabel = new QLabel(QStringLiteral("状态：未执行"), page);
        layout->addWidget(m_writeStatusLabel);

        connect(m_writeBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onWritePayloadClicked);

        layout->addStretch();
        return page;
    }

    // ========================================================================
    // buildKeepPage：第 4 步，Unregister(SKIP_RESTORE) 保留 payload，等外部触发
    // ========================================================================
    QWidget* KernelHookWizard::buildKeepPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto* hint = new QLabel(QStringLiteral(
            "<b>第 4 步：保留 payload 并触发</b><br>"
            "调用 Unregister(FLAG_SKIP_BACKUP_RESTORE)，驱动跳过 auto-restore，物理页保留你写入的 payload；mode=1 把 PTE 翻回 PAGE_EXECUTE_READ。<br>"
            "<b style='color:#d9534f;'>警告：此时内核任何代码路径执行到目标 VA 都会按 payload 执行。常见后果：</b><br>"
            "&nbsp;&nbsp;• <code>C3</code>/<code>33 C0 C3</code>：函数立即返回，视调用者行为可能继续运行或 BSOD<br>"
            "&nbsp;&nbsp;• <code>CC</code>：触发 int 3 → 立即 BSOD<br>"
            "&nbsp;&nbsp;• <code>90</code>：NOP 后继续执行原字节（已对 CI.dll+0x1000 实测秒级 BSOD）<br>"
            "<b>必须</b>在 PatchGuard 扫描前（分钟级）走到第 5 步恢复，否则必蓝屏。"),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        m_keepBtn = new QPushButton(QStringLiteral("执行 Unregister(SKIP_RESTORE) 保留 payload"),
            page);
        ks::ui::ApplyFlatButtonTheme(m_keepBtn, ks::ui::FlatButtonTone::Danger);
        m_keepBtn->setMinimumHeight(36);
        layout->addWidget(m_keepBtn);

        m_keepStatusLabel = new QLabel(QStringLiteral("状态：未执行"), page);
        layout->addWidget(m_keepStatusLabel);

        connect(m_keepBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onKeepModifiedClicked);

        layout->addStretch();
        return page;
    }

    // ========================================================================
    // buildVerifyPage：第 5 步，恢复原字节 + 验证
    // ========================================================================
    QWidget* KernelHookWizard::buildVerifyPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto* hint = new QLabel(QStringLiteral(
            "<b>第 5 步：恢复 payload + 验证</b><br>"
            "分两个动作：<br>"
            "&nbsp;&nbsp;①<b>恢复</b>：Register(CB)+Change+Write(backup)+Unregister，用第 2 步缓存的原字节覆盖回去。走正常 auto-restore 流程。<br>"
            "&nbsp;&nbsp;②<b>验证</b>：再 Register 一次读 origProt，期望 0x20 (PAGE_EXECUTE_READ)。<br>"
            "<i>注意 originalProtection 来自 PE 节头解析，不采样 live PTE——只能作为\"页仍在模块范围内\"的参考，不等价于 PTE 真回 RX。</i>"),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        m_restoreBtn = new QPushButton(QStringLiteral("① 恢复原字节"), page);
        ks::ui::ApplyFlatButtonTheme(m_restoreBtn, ks::ui::FlatButtonTone::Accent);
        m_restoreBtn->setMinimumHeight(36);
        layout->addWidget(m_restoreBtn);

        m_verifyBtn = new QPushButton(QStringLiteral("② 验证页面保护"), page);
        ks::ui::ApplyFlatButtonTheme(m_verifyBtn, ks::ui::FlatButtonTone::Neutral);
        m_verifyBtn->setMinimumHeight(36);
        layout->addWidget(m_verifyBtn);

        m_finalStatusLabel = new QLabel(QStringLiteral("状态：未执行"), page);
        m_finalStatusLabel->setWordWrap(true);
        layout->addWidget(m_finalStatusLabel);

        connect(m_restoreBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onRestoreClicked);
        connect(m_verifyBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onFinalVerifyClicked);

        layout->addStretch();
        return page;
    }

    // ========================================================================
    // goTo：切到指定步骤，更新 stackedWidget 和导航按钮
    // ========================================================================
    void KernelHookWizard::goTo(int step)
    {
        if (step < 0 || step > 4) { return; }
        m_currentStep = step;
        m_stack->setCurrentIndex(step);
        m_stepLabel->setText(QStringLiteral("第 %1 / 5 步").arg(step + 1));
        updateNavButtons();
    }

    void KernelHookWizard::updateNavButtons()
    {
        m_prevBtn->setEnabled(!m_busy && m_currentStep > 0);
        m_nextBtn->setEnabled(!m_busy);
        m_nextBtn->setText(m_currentStep == 4
            ? QStringLiteral("关闭向导") : QStringLiteral("下一步"));
    }

    // ========================================================================
    // setBusy / appendTrace：工具函数
    // ========================================================================
    void KernelHookWizard::setBusy(bool busy)
    {
        m_busy = busy;
        if (m_prepareBtn) { m_prepareBtn->setEnabled(!busy); }
        if (m_writeBtn)   { m_writeBtn->setEnabled(!busy); }
        if (m_keepBtn)    { m_keepBtn->setEnabled(!busy); }
        if (m_restoreBtn) { m_restoreBtn->setEnabled(!busy); }
        if (m_verifyBtn)  { m_verifyBtn->setEnabled(!busy); }
        updateNavButtons();
    }

    void KernelHookWizard::appendTrace(const QString& text)
    {
        if (!m_traceEdit) { return; }
        const QString line = QStringLiteral("[%1] %2\n").arg(timestamp(), text);
        m_traceEdit->appendRawText(line);
    }

    // ========================================================================
    // refreshModules：枚举已加载的内核模块，填入下拉
    // ========================================================================
    void KernelHookWizard::refreshModules()
    {
        if (!m_moduleCombo) { return; }
        m_moduleCombo->clear();
        const QStringList wanted = { QStringLiteral("ci.dll"),
            QStringLiteral("ntoskrnl.exe"), QStringLiteral("ndis.sys"),
            QStringLiteral("fltmgr.sys"), QStringLiteral("tcpip.sys"),
            QStringLiteral("NETIO.SYS") };
        const auto found = enumerateKernelModule(wanted);
        for (auto it = found.constBegin(); it != found.constEnd(); ++it)
        {
            m_moduleCombo->addItem(QStringLiteral("%1 @ %2")
                .arg(it.key(), formatHex8(it.value())), it.value());
            m_moduleCombo->setItemData(m_moduleCombo->count() - 1, it.key(),
                Qt::UserRole + 1);
        }
        if (m_moduleCombo->count() == 0)
        {
            m_moduleCombo->addItem(
                QStringLiteral("<未枚举到任何内核模块>"), QVariant(quint64(0)));
            appendTrace(QStringLiteral(
                "警告：EnumDeviceDrivers 返回 0 个候选模块；"
                "请以管理员身份运行主程序"));
        }
    }

    // ========================================================================
    // onPrepareClicked：Register(CB) + Change(mode=2) + QueryBackup，异步跑
    // ========================================================================
    void KernelHookWizard::onPrepareClicked()
    {
        if (m_plan.targetVa == 0ULL)
        {
            appendTrace(QStringLiteral("请先在第 1 步选目标模块与偏移"));
            return;
        }
        setBusy(true);
        m_prepareStatusLabel->setText(QStringLiteral("状态：Register(CB) 中..."));
        appendTrace(QStringLiteral("Register(EXTERNAL_IMAGE, CAPTURE_BACKUP) target=%1")
            .arg(formatHex8(m_plan.targetVa)));

        const quint64 va = m_plan.targetVa;
        auto* w = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(w, &QFutureWatcher<rxpf::PageInfo>::finished, this,
            [this, w]() {
                const auto reg = w->result();
                w->deleteLater();
                if (!reg.ioSuccess) {
                    appendTrace(QStringLiteral("Register 失败：%1").arg(reg.ioErrorText));
                    m_prepareStatusLabel->setText(QStringLiteral("状态：Register 失败"));
                    setBusy(false);
                    return;
                }
                m_plan.recordId = reg.recordId;
                m_plan.originalProtection = reg.originalProtection;
                appendTrace(QStringLiteral("Register OK recordId=%1 origProt=%2")
                    .arg(reg.recordId).arg(formatHex4(reg.originalProtection)));

                // 继续 Change(mode=2)
                appendTrace(QStringLiteral("Change(mode=2) recordId=%1").arg(reg.recordId));
                const quint64 rid = reg.recordId;
                auto* w2 = new QFutureWatcher<rxpf::PageInfo>(this);
                connect(w2, &QFutureWatcher<rxpf::PageInfo>::finished, this,
                    [this, w2]() {
                        const auto chg = w2->result();
                        w2->deleteLater();
                        if (!chg.ioSuccess) {
                            appendTrace(QStringLiteral("Change 失败：%1").arg(chg.ioErrorText));
                            m_prepareStatusLabel->setText(QStringLiteral("状态：Change 失败"));
                            setBusy(false);
                            return;
                        }
                        m_plan.aliasProtection = chg.writableAliasProtection;
                        appendTrace(QStringLiteral("Change OK curP=%1 aliasP=%2")
                            .arg(formatHex4(chg.currentProtection))
                            .arg(formatHex4(chg.writableAliasProtection)));

                        // 继续 QueryBackup（读前 16 字节）
                        const quint64 rid2 = m_plan.recordId;
                        auto* w3 = new QFutureWatcher<rxpf::BackupInfo>(this);
                        connect(w3, &QFutureWatcher<rxpf::BackupInfo>::finished, this,
                            [this, w3]() {
                                const auto bk = w3->result();
                                w3->deleteLater();
                                if (!bk.ioSuccess || !bk.hasBackup) {
                                    appendTrace(QStringLiteral(
                                        "QueryBackup 失败：%1 (hasBackup=%2)")
                                        .arg(bk.ioErrorText)
                                        .arg(bk.hasBackup ? "true" : "false"));
                                    m_prepareStatusLabel->setText(QStringLiteral(
                                        "状态：QueryBackup 失败"));
                                    setBusy(false);
                                    return;
                                }
                                // 这里的 16 字节只是页头预览给用户看，不作为恢复数据：
                                // 用户的 payloadOffset / payload.size() 可能完全不同，
                                // 真正用于恢复的 backup 字节在第 3 步 Write 时按用户选的
                                // (offset, size) 分段 QueryBackup 读入 m_plan.backupBytes。
                                m_backupHexLabel->setText(
                                    QStringLiteral("原字节 (页头前 %1 字节预览，仅显示)：%2")
                                        .arg(bk.length).arg(bytesToHex(bk.bytes)));
                                m_prepareStatusLabel->setText(QStringLiteral(
                                    "状态：准备完成，可进入第 3 步"));
                                appendTrace(QStringLiteral(
                                    "QueryBackup preview bytes=%1").arg(bytesToHex(bk.bytes)));
                                m_plan.backupBytes.clear();
                                setBusy(false);
                            });
                        w3->setFuture(QtConcurrent::run([rid2]() -> rxpf::BackupInfo {
                            return rxpf::queryBackup(rid2, 0UL, 16UL);
                        }));
                    });
                w2->setFuture(QtConcurrent::run([rid]() -> rxpf::PageInfo {
                    return rxpf::changePage(rid);
                }));
            });
        w->setFuture(QtConcurrent::run([va]() -> rxpf::PageInfo {
            return rxpf::registerPage(rxpf::kTargetExternalImage, va);
        }));
    }

    // ========================================================================
    // onWritePayloadClicked：Write(payload) 异步
    // ========================================================================
    void KernelHookWizard::onWritePayloadClicked()
    {
        // 前置：必须已完成第 2 步 Register+Change 拿到 recordId
        if (m_plan.recordId == 0ULL) {
            appendTrace(QStringLiteral("请先在第 2 步完成准备"));
            return;
        }

        // 1) 解析 payload hex：空输入直接拒；长度不设 GUI 上限，交给驱动按单页约束
        const QByteArray payload = parseHexBytes(m_payloadHexEdit->text());
        if (payload.isEmpty()) {
            appendTrace(QStringLiteral("Payload hex 解析失败或为空"));
            return;
        }

        // 2) 解析用户指定的页内 offset：支持 0x 前缀 hex 与十进制；只拒"解析失败"或负数
        bool offsetOk = false;
        const QString offsetText = m_payloadOffsetEdit->text().trimmed();
        const QString normalizedOffsetText = offsetText.startsWith(QStringLiteral("0x"),
            Qt::CaseInsensitive)
            ? offsetText.mid(2)
            : offsetText;
        const int offsetBase = offsetText.startsWith(QStringLiteral("0x"),
            Qt::CaseInsensitive)
            ? 16
            : 10;
        const qulonglong parsedOffset = normalizedOffsetText.toULongLong(&offsetOk, offsetBase);
        if (!offsetOk) {
            appendTrace(QStringLiteral("Offset 解析失败：%1").arg(offsetText));
            return;
        }

        // 3) 把用户选择全部塞进 plan，供后续恢复步骤使用；不在 GUI 侧 clamp，由驱动校验
        m_plan.payload = payload;
        m_plan.payloadOffset = static_cast<unsigned long>(parsedOffset & 0xFFFFFFFFULL);

        setBusy(true);
        m_writeStatusLabel->setText(QStringLiteral("状态：QueryBackup + Write 中..."));
        appendTrace(QStringLiteral("Write(offset=0x%1, len=%2) bytes=%3")
            .arg(static_cast<quint64>(m_plan.payloadOffset), 0, 16)
            .arg(payload.size())
            .arg(bytesToHex(payload)));

        // 4) Worker thread: 先分段 QueryBackup 读准确 (offset, size) 的原字节用于恢复，
        //    再 Write(offset, payload) 下用户 payload。QueryBackup 单段 ≤ 64 字节是协议上限，
        //    用户 payload 长度无 GUI 上限 → 需要分段循环；任意一段失败整体失败。
        //    backup 字节通过 shared_ptr 跨线程回传给 UI finished 回调。
        const quint64 rid = m_plan.recordId;
        const unsigned long offsetArg = m_plan.payloadOffset;
        auto backupHolder = std::make_shared<QByteArray>();
        auto* w = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(w, &QFutureWatcher<rxpf::PageInfo>::finished, this,
            [this, w, backupHolder]() {
                const auto info = w->result();
                w->deleteLater();
                if (!info.ioSuccess) {
                    appendTrace(QStringLiteral("Write 失败：%1").arg(info.ioErrorText));
                    m_writeStatusLabel->setText(QStringLiteral("状态：Write 失败"));
                    // Write 失败时清空 cached backup，避免第 5 步用错数据写回
                    m_plan.backupBytes.clear();
                } else {
                    // Write OK：把 Write 之前读到的准确 backup 字节 cache 进 plan 供第 5 步恢复
                    m_plan.backupBytes = *backupHolder;
                    appendTrace(QStringLiteral("Backup cached len=%1 bytes=%2")
                        .arg(backupHolder->size()).arg(bytesToHex(*backupHolder)));
                    appendTrace(QStringLiteral("Write OK status=%1")
                        .arg(formatHex4(static_cast<quint32>(info.lastStatus))));
                    m_writeStatusLabel->setText(QStringLiteral(
                        "状态：payload 已写入可写别名，可进入第 4 步"));
                }
                setBusy(false);
            });
        w->setFuture(QtConcurrent::run([rid, offsetArg, payload, backupHolder]() -> rxpf::PageInfo {
            // 分段 QueryBackup：协议上限 64 字节/段，用户 payload 任意长 → 按 chunk 循环
            constexpr int kQueryBackupChunk = 64;
            for (int readIndex = 0; readIndex < payload.size(); readIndex += kQueryBackupChunk) {
                const int remaining = payload.size() - readIndex;
                const int thisChunk = std::min(kQueryBackupChunk, remaining);
                const auto qb = rxpf::queryBackup(rid,
                    offsetArg + static_cast<unsigned long>(readIndex),
                    static_cast<unsigned long>(thisChunk));
                if (!qb.ioSuccess || !qb.hasBackup) {
                    // 构造失败态 PageInfo 让 UI 回调走失败分支
                    rxpf::PageInfo fail;
                    fail.ioSuccess = false;
                    fail.ioErrorText = qb.ioErrorText.isEmpty()
                        ? QStringLiteral("QueryBackup 分段失败 offset=%1 len=%2")
                            .arg(static_cast<quint64>(offsetArg + readIndex), 0, 16)
                            .arg(thisChunk)
                        : qb.ioErrorText;
                    return fail;
                }
                backupHolder->append(qb.bytes);
            }
            // Backup 全部读完才 Write；Write 本身可以写任意长度 payload（driver 校验越页）
            return rxpf::writePage(rid, offsetArg, payload);
        }));
    }

    // ========================================================================
    // onKeepModifiedClicked：Unregister(SKIP_RESTORE)
    // ========================================================================
    void KernelHookWizard::onKeepModifiedClicked()
    {
        if (m_plan.recordId == 0ULL) {
            appendTrace(QStringLiteral("请先完成第 3 步 Write"));
            return;
        }
        setBusy(true);
        m_keepStatusLabel->setText(QStringLiteral(
            "状态：Unregister(SKIP) 中——完成后 payload 已就位，等待外部触发"));
        appendTrace(QStringLiteral("Unregister(FLAG_SKIP_BACKUP_RESTORE) recordId=%1")
            .arg(m_plan.recordId));

        const quint64 rid = m_plan.recordId;
        auto* w = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(w, &QFutureWatcher<rxpf::PageInfo>::finished, this,
            [this, w]() {
                const auto info = w->result();
                w->deleteLater();
                if (!info.ioSuccess) {
                    appendTrace(QStringLiteral("Unregister 失败：%1").arg(info.ioErrorText));
                    m_keepStatusLabel->setText(QStringLiteral("状态：Unregister 失败"));
                } else {
                    m_plan.mode1Status = info.lastStatus;
                    appendTrace(QStringLiteral("Unregister OK mode=1 lastStatus=%1")
                        .arg(formatHex4(static_cast<quint32>(info.lastStatus))));
                    m_keepStatusLabel->setText(QStringLiteral(
                        "状态：payload 已就位；立刻进入第 5 步恢复以避免蓝屏"));
                    // record 已销毁，recordId 作废
                    m_plan.recordId = 0ULL;
                }
                setBusy(false);
            });
        w->setFuture(QtConcurrent::run([rid]() -> rxpf::PageInfo {
            return rxpf::unregisterPage(rid, /*skipBackupRestore=*/true);
        }));
    }

    // ========================================================================
    // onRestoreClicked：Register+Change+Write(backup)+Unregister 恢复原字节
    // ========================================================================
    void KernelHookWizard::onRestoreClicked()
    {
        if (m_plan.backupBytes.isEmpty()) {
            appendTrace(QStringLiteral("无 Backup，无法恢复；回到第 2 步重新准备"));
            return;
        }
        setBusy(true);
        m_finalStatusLabel->setText(QStringLiteral("状态：恢复中..."));
        appendTrace(QStringLiteral(
            "恢复：Register+Change+Write(backup len=%1)+Unregister target=%2")
            .arg(m_plan.backupBytes.size()).arg(formatHex8(m_plan.targetVa)));

        const quint64 va = m_plan.targetVa;
        const QByteArray backup = m_plan.backupBytes;
        const unsigned long restoreOffset = m_plan.payloadOffset;
        auto* w = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(w, &QFutureWatcher<rxpf::PageInfo>::finished, this,
            [this, w, backup, restoreOffset]() {
                const auto reg = w->result();
                w->deleteLater();
                if (!reg.ioSuccess) {
                    appendTrace(QStringLiteral("恢复 Register 失败：%1").arg(reg.ioErrorText));
                    m_finalStatusLabel->setText(QStringLiteral("状态：Register 失败"));
                    setBusy(false);
                    return;
                }
                const quint64 rid = reg.recordId;
                auto* w2 = new QFutureWatcher<rxpf::PageInfo>(this);
                connect(w2, &QFutureWatcher<rxpf::PageInfo>::finished, this,
                    [this, w2, rid, backup, restoreOffset]() {
                        const auto chg = w2->result();
                        w2->deleteLater();
                        if (!chg.ioSuccess) {
                            appendTrace(QStringLiteral("恢复 Change 失败：%1")
                                .arg(chg.ioErrorText));
                            m_finalStatusLabel->setText(QStringLiteral("状态：Change 失败"));
                            setBusy(false);
                            return;
                        }
                        auto* w3 = new QFutureWatcher<rxpf::PageInfo>(this);
                        connect(w3, &QFutureWatcher<rxpf::PageInfo>::finished, this,
                            [this, w3, rid]() {
                                const auto wr = w3->result();
                                w3->deleteLater();
                                if (!wr.ioSuccess) {
                                    appendTrace(QStringLiteral("恢复 Write 失败：%1")
                                        .arg(wr.ioErrorText));
                                    m_finalStatusLabel->setText(QStringLiteral(
                                        "状态：Write 失败"));
                                    setBusy(false);
                                    return;
                                }
                                auto* w4 = new QFutureWatcher<rxpf::PageInfo>(this);
                                connect(w4, &QFutureWatcher<rxpf::PageInfo>::finished, this,
                                    [this, w4]() {
                                        const auto un = w4->result();
                                        w4->deleteLater();
                                        if (!un.ioSuccess) {
                                            appendTrace(QStringLiteral(
                                                "恢复 Unregister 失败：%1").arg(un.ioErrorText));
                                            m_finalStatusLabel->setText(QStringLiteral(
                                                "状态：Unregister 失败"));
                                        } else {
                                            m_plan.restored = true;
                                            m_plan.restoreStatus = un.lastStatus;
                                            appendTrace(QStringLiteral(
                                                "恢复 Unregister OK mode=1 status=%1")
                                                .arg(formatHex4(static_cast<quint32>(
                                                    un.lastStatus))));
                                            m_finalStatusLabel->setText(QStringLiteral(
                                                "状态：恢复完成，可点\"验证\"检查页面保护"));
                                        }
                                        setBusy(false);
                                    });
                                w4->setFuture(QtConcurrent::run([rid]() -> rxpf::PageInfo {
                                    return rxpf::unregisterPage(rid, /*skip=*/false);
                                }));
                            });
                        // 用用户在第 3 步指定的 payloadOffset 把 backup 写回同一位置
                        w3->setFuture(QtConcurrent::run([rid, backup, restoreOffset]() -> rxpf::PageInfo {
                            return rxpf::writePage(rid, restoreOffset, backup);
                        }));
                    });
                w2->setFuture(QtConcurrent::run([rid]() -> rxpf::PageInfo {
                    return rxpf::changePage(rid);
                }));
            });
        w->setFuture(QtConcurrent::run([va]() -> rxpf::PageInfo {
            return rxpf::registerPage(rxpf::kTargetExternalImage, va);
        }));
    }

    // ========================================================================
    // onFinalVerifyClicked：Re-Register 查 origProt，立即 Unregister
    // ========================================================================
    void KernelHookWizard::onFinalVerifyClicked()
    {
        setBusy(true);
        const quint64 va = m_plan.targetVa;
        appendTrace(QStringLiteral("验证：Register target=%1").arg(formatHex8(va)));
        auto* w = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(w, &QFutureWatcher<rxpf::PageInfo>::finished, this,
            [this, w]() {
                const auto reg = w->result();
                w->deleteLater();
                if (!reg.ioSuccess) {
                    appendTrace(QStringLiteral("验证 Register 失败：%1").arg(reg.ioErrorText));
                    m_finalStatusLabel->setText(QStringLiteral("状态：Register 失败"));
                    setBusy(false);
                    return;
                }
                m_plan.verifyOrigProt = reg.originalProtection;
                const QString ok = (reg.originalProtection == 0x20)
                    ? QStringLiteral("<b style='color:#5cb85c;'>origProt=0x20 (PAGE_EXECUTE_READ) OK</b>")
                    : QStringLiteral("<b style='color:#d9534f;'>origProt=%1 (非 0x20)</b>")
                        .arg(formatHex4(reg.originalProtection));
                appendTrace(QStringLiteral("验证 origProt=%1 (期望 0x20)")
                    .arg(formatHex4(reg.originalProtection)));
                // 立即 Unregister 清理 record
                const quint64 rid = reg.recordId;
                auto* w2 = new QFutureWatcher<rxpf::PageInfo>(this);
                connect(w2, &QFutureWatcher<rxpf::PageInfo>::finished, this,
                    [this, w2, ok]() {
                        const auto un = w2->result();
                        w2->deleteLater();
                        appendTrace(QStringLiteral("清理 Unregister OK"));
                        m_finalStatusLabel->setText(QStringLiteral("验证结果：%1").arg(ok));
                        setBusy(false);
                    });
                w2->setFuture(QtConcurrent::run([rid]() -> rxpf::PageInfo {
                    return rxpf::unregisterPage(rid, /*skip=*/false);
                }));
            });
        w->setFuture(QtConcurrent::run([va]() -> rxpf::PageInfo {
            return rxpf::registerPage(rxpf::kTargetExternalImage, va);
        }));
    }
}
