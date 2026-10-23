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
#include "../../../shared/driver/KswordArkRxPfIoctl.h"
#include "../theme.h"
#include "FlatButtonTheme.h"
#include "CodeEditorWidget.h"
#include "SecondaryPageLayout.h"
#include "../Internationalization/LanguageManager.h"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QCloseEvent>
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

namespace
{
    // 操作成功必须同时有完整响应及成功NTSTATUS，不能仅依赖DeviceIoControl。
    bool pageSucceeded(const rxpf::PageInfo& page)
    {
        return page.ioSuccess && page.lastStatus >= 0;
    }

    QString pageFailure(const rxpf::PageInfo& page)
    {
        return page.ioSuccess ? formatHex4(static_cast<quint32>(page.lastStatus))
                              : page.ioErrorText;
    }

    // 后台恢复携带最新所有权快照；任何中间失败也把记录交还UI，便于重试。
    struct RecoveryResult
    {
        ks::ui::KernelHookPlan plan;
        QString failure;
        bool ok = false;
    };

    RecoveryResult recoverOwnedPage(ks::ui::KernelHookPlan plan)
    {
        RecoveryResult result;
        result.plan = plan;
        auto& owned = result.plan;

        // 原子提交没有Backup；这里只重试已有记录的注销，不能把资源清理说成原字节恢复。
        // 每成功一个就移除一个，后续失败时把剩余记录完整交还UI继续持有退出责任。
        while (!owned.atomicCleanupRecordIds.isEmpty())
        {
            const auto released = rxpf::unregisterPage(owned.atomicCleanupRecordIds.first(), true);
            if (!pageSucceeded(released))
            {
                result.failure = pageFailure(released);
                return result;
            }
            owned.atomicCleanupRecordIds.removeFirst();
        }

        // 原准备记录持有整页原始Backup，默认注销即可恢复；不重注册捕获补丁。
        if (owned.recordId != 0ULL && !owned.recoveryRecord)
        {
            const auto released = rxpf::unregisterPage(owned.recordId);
            if (!pageSucceeded(released))
            {
                result.failure = pageFailure(released);
                return result;
            }
            owned.recordId = 0ULL;
            owned.recordPrepared = false;
            // R0已经恢复整页Backup，不再依赖之前缺失的局部QueryBackup证据。
            owned.hasUncachedWrites = false;
        }
        else if (owned.payloadPending)
        {
            if (owned.hasUncachedWrites || owned.restoreRanges.isEmpty())
            {
                result.failure = QStringLiteral("缺少完整原字节证据，无法证实恢复。请保留当前窗口并处理目标页面。");
                return result;
            }

            // 已保留补丁后只能用缓存的原范围恢复；新记录的Backup包含补丁，绝不能自动回放。
            if (owned.recordId == 0ULL)
            {
                const auto registered = rxpf::registerPage(rxpf::kTargetExternalImage,
                    owned.targetVa);
                if (!pageSucceeded(registered))
                {
                    result.failure = pageFailure(registered);
                    return result;
                }
                owned.recordId = registered.recordId;
                owned.recoveryRecord = true;
                owned.originalBytesWritten = false;
            }
            if (!owned.originalBytesWritten)
            {
                const auto current = rxpf::queryPage(owned.recordId);
                if (!pageSucceeded(current))
                {
                    result.failure = pageFailure(current);
                    return result;
                }
                if (current.state == KSWORD_ARK_RXPF_PAGE_STATE_ERROR)
                {
                    // 失败释放留下的记录先按原收尾路径重试；成功后才创建下一次恢复记录。
                    const auto released = rxpf::unregisterPage(owned.recordId, true);
                    if (!pageSucceeded(released))
                    {
                        result.failure = pageFailure(released);
                        return result;
                    }
                    owned.recordId = 0ULL;
                    owned.recoveryRecord = false;
                    return recoverOwnedPage(owned);
                }
                if (current.state != KSWORD_ARK_RXPF_PAGE_STATE_RW_NX)
                {
                    const auto changed = rxpf::changePage(owned.recordId);
                    if (!pageSucceeded(changed))
                    {
                        result.failure = pageFailure(changed);
                        return result;
                    }
                }
                // 这里只回放已缓存的原字节，不扩展用户payload的单次写入能力。
                for (const auto& range : owned.restoreRanges)
                {
                    const auto written = rxpf::writePage(owned.recordId,
                        range.offset, range.bytes);
                    if (!pageSucceeded(written)
                        || written.writtenLength != static_cast<unsigned long>(range.bytes.size()))
                    {
                        result.failure = pageSucceeded(written)
                            ? QStringLiteral("原字节写回长度不完整，尚未证实恢复。")
                            : pageFailure(written);
                        return result;
                    }
                }
                owned.originalBytesWritten = true;
            }
            const auto released = rxpf::unregisterPage(owned.recordId, true);
            owned.restoreStatus = released.lastStatus;
            if (!pageSucceeded(released))
            {
                result.failure = pageFailure(released);
                return result;
            }
            owned.recordId = 0ULL;
            owned.recordPrepared = false;
        }

        // 完整只读回读才允许把收尾标为完成；回读失败仍保留原范围供重试。
        for (const auto& range : owned.restoreRanges)
        {
            const auto read = rxpf::readPageBytes(owned.targetVa,
                range.offset, static_cast<unsigned long>(range.bytes.size()));
            if (!read.ok || read.bytes != range.bytes)
            {
                result.failure = read.ok ? QStringLiteral("恢复后字节与原始备份不一致。") : read.errorText;
                return result;
            }
        }
        owned.payloadPending = false;
        owned.recoveryRecord = false;
        owned.originalBytesWritten = false;
        owned.restored = !owned.atomicOriginalBytesUnverified;
        result.ok = true;
        return result;
    }
}

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
        qApp->installEventFilter(this);
        if (parent && parent->window()) parent->window()->installEventFilter(this);
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
        connect(m_moduleCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateTargetFromInputs(); });
        connect(m_offsetEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { updateTargetFromInputs(); });

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
            "可输入任意 hex 字符串；现有单次写入最多 64 字节，超出时只写入前 64 字节并报告部分完成。"
            "预设只是一键填充，不代表只能用这些。原字节已在上一步存入 Backup 可随后恢复。<br>"
            "<i>页内范围仍由驱动校验；实际写入长度会明确显示。</i>"),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        // Payload 预设下拉：仅做"一键填充 hex 输入框"的便捷功能，不限制用户输入
        auto* presetRow = new QHBoxLayout();
        presetRow->addWidget(new QLabel(QStringLiteral("Payload 预设："), page));
        m_payloadPresetCombo = new QComboBox(page);
        // 预设按钮：常用的"真·控制流篡改" payload 一键填入
        // 分三组：短 payload（最小破坏）/ 返回常量（短路函数）/ 跳转骨架（用户填 displacement）
        m_payloadPresetCombo->addItem(QStringLiteral("自定义（不自动覆盖）"), QString());
        // --- 单字节/最小破坏 ---
        m_payloadPresetCombo->addItem(QStringLiteral("ret (C3) — 立即返回"),
            QStringLiteral("C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("NOP (90) — 继续执行后一字节"),
            QStringLiteral("90"));
        m_payloadPresetCombo->addItem(QStringLiteral("int 3 (CC) — 立即蓝屏/断点"),
            QStringLiteral("CC"));
        // --- 短路函数：返回 0（约定 NTSTATUS SUCCESS / BOOL FALSE / 整数 0） ---
        m_payloadPresetCombo->addItem(QStringLiteral("xor eax,eax; ret (33 C0 C3) — 32bit 返回 0"),
            QStringLiteral("33 C0 C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("xor rax,rax; ret (48 31 C0 C3) — 64bit 返回 0"),
            QStringLiteral("48 31 C0 C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("mov eax,0; ret (B8 00 00 00 00 C3) — 32bit 返回 0 常量版"),
            QStringLiteral("B8 00 00 00 00 C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("mov rax,0; ret (48 C7 C0 00 00 00 00 C3) — 64bit 返回 0 常量版"),
            QStringLiteral("48 C7 C0 00 00 00 00 C3"));
        // --- 短路函数：返回 1（约定 NTSTATUS 非零错误 / BOOL TRUE） ---
        m_payloadPresetCombo->addItem(QStringLiteral("mov eax,1; ret (B8 01 00 00 00 C3) — 32bit 返回 1"),
            QStringLiteral("B8 01 00 00 00 C3"));
        m_payloadPresetCombo->addItem(QStringLiteral("mov rax,1; ret (48 C7 C0 01 00 00 00 C3) — 64bit 返回 1"),
            QStringLiteral("48 C7 C0 01 00 00 00 C3"));
        // --- 短路函数：返回 -1（约定 BOOL FALSE 的另一种 / 常见错误码 0xFFFFFFFF） ---
        m_payloadPresetCombo->addItem(QStringLiteral("or eax,-1; ret (83 C8 FF C3) — 32bit 返回 0xFFFFFFFF"),
            QStringLiteral("83 C8 FF C3"));
        // --- 跳转骨架：填 00 的 displacement 用户自改 ---
        m_payloadPresetCombo->addItem(QStringLiteral("jmp rel32 骨架 (E9 00 00 00 00) — 用户自填 displacement"),
            QStringLiteral("E9 00 00 00 00"));
        m_payloadPresetCombo->addItem(QStringLiteral("call rel32 骨架 (E8 00 00 00 00) — 用户自填 displacement"),
            QStringLiteral("E8 00 00 00 00"));
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
            "任意 hex，空格可选；单次实际写入最多 64 字节"));
        m_payloadHexEdit->setToolTip(QStringLiteral(
            "保留任意 hex 输入；单次最多写入前 64 字节，其余不写入并报告部分完成"));
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

        // 两个写入按钮并列：
        // - "写入 payload"：分步模式，依赖第 2 步 Register 产生的 recordId，仅做 Write
        // - "原子提交 (单次 IOCTL)"：Register+Change+Write+Unregister(SKIP) 全部在一次 R0 call
        //   内完成；减少用户态往返仍不提供全核隔离或无执行故障保证。
        auto* writeRow = new QHBoxLayout();
        m_writeBtn = new QPushButton(QStringLiteral("写入 payload"), page);
        ks::ui::ApplyFlatButtonTheme(m_writeBtn, ks::ui::FlatButtonTone::Accent);
        m_writeBtn->setMinimumHeight(36);
        m_writeBtn->setToolTip(QStringLiteral("分步模式：仅发 Write IOCTL，需要先在第 2 步完成 Register+Change；目标页在分步操作期间仍有执行故障风险"));
        writeRow->addWidget(m_writeBtn);

        m_atomicCommitBtn = new QPushButton(QStringLiteral("原子提交 (单次 IOCTL)"), page);
        ks::ui::ApplyFlatButtonTheme(m_atomicCommitBtn, ks::ui::FlatButtonTone::Accent);
        m_atomicCommitBtn->setMinimumHeight(36);
        m_atomicCommitBtn->setToolTip(QStringLiteral("原子模式：在单次 IOCTL 中请求既有操作；减少用户态往返不等于全核隔离，仍可能发生执行故障；无 Backup 可自动恢复"));
        writeRow->addWidget(m_atomicCommitBtn);
        writeRow->addStretch();
        layout->addLayout(writeRow);

        m_writeStatusLabel = new QLabel(QStringLiteral("状态：未执行"), page);
        layout->addWidget(m_writeStatusLabel);

        connect(m_writeBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onWritePayloadClicked);
        connect(m_atomicCommitBtn, &QPushButton::clicked,
            this, &KernelHookWizard::onAtomicCommitClicked);

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
            "&nbsp;&nbsp;①<b>恢复</b>：仍持有准备记录时使用其原始 Backup 收尾；已保留补丁时回写缓存的原范围，跳过新快照的自动回放。<br>"
            "&nbsp;&nbsp;②<b>验证</b>：只读回读缓存的原字节范围并比较，不改变页面保护。<br>"
            "<i>字节回读不等价于执行权限验证；本向导未采样 live PTE。</i>"),
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
    bool KernelHookWizard::beginOperation()
    {
        if (m_busy) return false;
        // 与其它事务页面共用退出协调：在派发前登记，已有记录的恢复重用原 lease。
        // 主窗口已经开始收尾时不接受新工作；取消退出后无需重建向导即可再次操作。
        if (!m_shutdownLease)
        {
            m_shutdownLease = AsyncCleanupRegistry::global().acquire();
            if (!m_shutdownLease)
            {
                appendTrace(ks::i18n::displayText(QStringLiteral("主程序正在退出，请取消退出后再开始新的操作。")));
                return false;
            }
        }
        setBusy(true);
        return true;
    }

    void KernelHookWizard::setBusy(bool busy)
    {
        m_busy = busy;
        if (m_moduleCombo) m_moduleCombo->setEnabled(!busy);
        if (m_offsetEdit) m_offsetEdit->setEnabled(!busy);
        if (m_payloadHexEdit) m_payloadHexEdit->setEnabled(!busy);
        if (m_payloadOffsetEdit) m_payloadOffsetEdit->setEnabled(!busy);
        if (m_payloadPresetCombo) m_payloadPresetCombo->setEnabled(!busy);
        if (m_prepareBtn) { m_prepareBtn->setEnabled(!busy); }
        if (m_writeBtn)   { m_writeBtn->setEnabled(!busy); }
        if (m_atomicCommitBtn) { m_atomicCommitBtn->setEnabled(!busy); }
        if (m_keepBtn)    { m_keepBtn->setEnabled(!busy); }
        if (m_restoreBtn) { m_restoreBtn->setEnabled(!busy); }
        if (m_verifyBtn)  { m_verifyBtn->setEnabled(!busy); }
        updateNavButtons();
        // 后台结束仍可能留下记录或待恢复字节；只有回执已归集且资源已清空才允许退出。
        if (!m_busy && !m_plan.hasPendingCleanup())
            m_shutdownLease.reset();
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

    // 关闭按钮、窗口X与Esc共用异步收尾，不能销毁仍持有记录的窗口。
    void KernelHookWizard::closeEvent(QCloseEvent* event)
    {
        if (m_busy)
        {
            event->ignore();
            return;
        }
        if (m_plan.hasPendingCleanup())
        {
            event->ignore();
            m_closeAfterCleanup = true;
            onRestoreClicked();
            return;
        }
        QDialog::closeEvent(event);
    }

    void KernelHookWizard::reject()
    {
        close();
    }

    bool KernelHookWizard::eventFilter(QObject* watched, QEvent* event)
    {
        const bool quitting = watched == qApp && event->type() == QEvent::Quit;
        const bool hostClosing = parentWidget() && watched == parentWidget()->window()
            && event->type() == QEvent::Close;
        if ((quitting || hostClosing)
            && (m_busy || m_plan.hasPendingCleanup()))
        {
            event->ignore();
            appendTrace(QStringLiteral("请先完成当前向导的恢复与资源清理，再关闭主程序。"));
            return true;
        }
        return QDialog::eventFilter(watched, event);
    }

    // 新选择与旧记录分开保存；收尾成功前不覆盖用于恢复的目标身份。
    void KernelHookWizard::updateTargetFromInputs()
    {
        if (m_busy) return;
        bool baseOk = false;
        bool offsetOk = false;
        KernelHookPlan requested;
        requested.moduleName = m_moduleCombo->currentText();
        requested.moduleBase = m_moduleCombo->currentData().toULongLong(&baseOk);
        requested.targetOffset = m_offsetEdit->text().toULongLong(&offsetOk, 16);
        if (!baseOk || requested.moduleBase == 0ULL || !offsetOk
            || requested.targetOffset > UINT64_MAX - requested.moduleBase)
        {
            if (!m_plan.hasPendingCleanup()) m_plan.targetVa = 0ULL;
            m_vaLabel->setText(QStringLiteral("绝对 VA：模块未加载或偏移格式错误"));
            return;
        }
        requested.targetVa = requested.moduleBase + requested.targetOffset;
        m_requestedTarget = requested;
        if (m_plan.hasPendingCleanup()
            && requested.targetVa != m_plan.targetVa)
        {
            m_targetChangePending = true;
            onRestoreClicked();
            return;
        }
        m_plan.moduleName = requested.moduleName;
        m_plan.moduleBase = requested.moduleBase;
        m_plan.targetOffset = requested.targetOffset;
        m_plan.targetVa = requested.targetVa;
        m_vaLabel->setText(QStringLiteral("绝对 VA：%1 (= %2 + %3)")
            .arg(formatHex8(requested.targetVa), formatHex8(requested.moduleBase),
                formatHex8(requested.targetOffset)));
    }

    void KernelHookWizard::onPrepareClicked()
    {
        if (m_busy) return;
        if (m_plan.hasPendingCleanup())
        {
            m_prepareAfterCleanup = true;
            onRestoreClicked();
            return;
        }
        if (m_plan.targetVa == 0ULL)
        {
            appendTrace(QStringLiteral("请先在第 1 步选目标模块与偏移"));
            return;
        }
        // 新一轮准备只沿用目标参数，不沿用上一轮的局部备份或完成标记。
        updateTargetFromInputs();
        if (m_plan.targetVa == 0ULL) return;
        if (!beginOperation()) return;
        m_plan = m_requestedTarget;
        struct PrepareResult
        {
            rxpf::PageInfo page;
            rxpf::BackupInfo preview;
            quint64 recordId = 0ULL;
        };
        const quint64 va = m_plan.targetVa;
        appendTrace(QStringLiteral("Register(EXTERNAL_IMAGE, CAPTURE_BACKUP) target=%1")
            .arg(formatHex8(va)));
        auto* watcher = new QFutureWatcher<PrepareResult>(this);
        connect(watcher, &QFutureWatcher<PrepareResult>::finished, this, [this, watcher]() {
            const auto result = watcher->result();
            watcher->deleteLater();
            m_plan.recordId = result.recordId;
            m_plan.recordPrepared = pageSucceeded(result.page)
                && result.page.state == KSWORD_ARK_RXPF_PAGE_STATE_RW_NX;
            m_plan.originalProtection = result.page.originalProtection;
            m_plan.aliasProtection = result.page.writableAliasProtection;
            appendTrace(QStringLiteral("Register OK recordId=%1 origProt=%2")
                .arg(result.recordId).arg(formatHex4(result.page.originalProtection)));
            m_prepareStatusLabel->setText(m_plan.recordPrepared
                ? QStringLiteral("状态：准备完成，可进入第 3 步")
                : QStringLiteral("状态：准备失败，记录保留供清理重试"));
            if (!m_plan.recordPrepared) appendTrace(pageFailure(result.page));
            if (result.preview.ioSuccess && result.preview.hasBackup)
                m_backupHexLabel->setText(bytesToHex(result.preview.bytes));
            setBusy(false);
        });
        watcher->setFuture(QtConcurrent::run([va]() {
            PrepareResult result;
            result.page = rxpf::registerPage(rxpf::kTargetExternalImage, va);
            if (!pageSucceeded(result.page)) return result;
            result.recordId = result.page.recordId;
            result.page = rxpf::changePage(result.recordId);
            if (pageSucceeded(result.page)) result.preview = rxpf::queryBackup(result.recordId, 0UL, 16UL);
            return result;
        }));
    }

    void KernelHookWizard::onWritePayloadClicked()
    {
        if (m_busy) return;
        if (m_plan.recordId == 0ULL)
        {
            appendTrace(QStringLiteral("请先在第 2 步完成准备"));
            return;
        }
        const QByteArray payload = parseHexBytes(m_payloadHexEdit->text());
        if (payload.isEmpty())
        {
            appendTrace(QStringLiteral("Payload hex 解析失败或为空"));
            return;
        }
        bool offsetOk = false;
        const QString text = m_payloadOffsetEdit->text().trimmed();
        const bool hexadecimal = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
        const auto parsedOffset = (hexadecimal ? text.mid(2) : text).toULongLong(&offsetOk,
            hexadecimal ? 16 : 10);
        if (!offsetOk)
        {
            appendTrace(QStringLiteral("Offset 解析失败：%1").arg(text));
            return;
        }
        struct WriteResult
        {
            rxpf::PageInfo page;
            rxpf::BackupInfo backup;
        };
        const auto offset = static_cast<unsigned long>(parsedOffset & 0xFFFFFFFFULL);
        const quint64 rid = m_plan.recordId;
        if (!beginOperation()) return;
        appendTrace(QStringLiteral("Write(offset=0x%1, len=%2) bytes=%3")
            .arg(static_cast<quint64>(offset), 0, 16)
            .arg(payload.size()).arg(bytesToHex(payload)));
        auto* watcher = new QFutureWatcher<WriteResult>(this);
        connect(watcher, &QFutureWatcher<WriteResult>::finished, this,
            [this, watcher, payload, offset]() {
                const auto result = watcher->result();
                watcher->deleteLater();
                // 包括失败调用在内保守记账；原准备记录仍可通过整页Backup安全清理。
                m_plan.payloadPending = true;
                m_plan.restored = false;
                const auto written = result.page.writtenLength;
                if (result.backup.ioSuccess && result.backup.hasBackup
                    && result.backup.bytes.size() == static_cast<int>(result.backup.length))
                {
                    const int cached = pageSucceeded(result.page)
                        ? static_cast<int>(written) : result.backup.bytes.size();
                    if (cached > 0)
                        m_plan.restoreRanges.append({offset, result.backup.bytes.left(cached)});
                }
                else
                {
                    m_plan.hasUncachedWrites = true;
                    appendTrace(QStringLiteral("WARN: QueryBackup 失败，payload 已写入但 backup 为空，第 5 步无法原字节恢复"));
                }
                m_plan.payloadOffset = offset;
                m_plan.payload = payload.left(static_cast<int>(written));
                m_plan.backupBytes = result.backup.bytes.left(static_cast<int>(written));
                if (!pageSucceeded(result.page))
                {
                    m_writeStatusLabel->setText(QStringLiteral("状态：Write 失败"));
                    appendTrace(pageFailure(result.page));
                }
                else
                {
                    // 保持原64字节写入能力，准确说明请求长于实际写入的部分完成状态。
                    m_writeStatusLabel->setText(QStringLiteral("写入结果：请求 %1 字节，实际写入 %2 字节。")
                        .arg(payload.size()).arg(written));
                    if (static_cast<std::uint64_t>(written) != static_cast<std::uint64_t>(payload.size()))
                        appendTrace(QStringLiteral("仅部分写入，剩余 payload 未写入；恢复范围按实际写入字节记录。"));
                }
                setBusy(false);
            });
        watcher->setFuture(QtConcurrent::run([rid, offset, payload]() {
            WriteResult result;
            const unsigned long effectiveLength = static_cast<unsigned long>(
                std::min<qsizetype>(payload.size(), KSWORD_ARK_RXPF_MAX_WRITE_BYTES));
            result.backup = rxpf::queryBackup(rid, offset, effectiveLength);
            // 保留原warning语义；没有备份仍可请求既有写入，但不能虚构恢复证据。
            result.page = rxpf::writePage(rid, offset, payload);
            return result;
        }));
    }

    void KernelHookWizard::onAtomicCommitClicked()
    {
        if (m_busy) return;
        // 前置：必须完成第 1 步得到 targetVa（页 0 的 updateVa 会填 moduleBase+offset）
        if (m_plan.targetVa == 0ULL) {
            appendTrace(QStringLiteral("请先在第 1 步选目标模块与偏移"));
            return;
        }

        // 1) 解析 payload hex：空输入直接拒
        const QByteArray payload = parseHexBytes(m_payloadHexEdit->text());
        if (payload.isEmpty()) {
            appendTrace(QStringLiteral("Payload hex 解析失败或为空"));
            return;
        }
        // 原子协议 R0 侧上限 64 字节；GUI 显式拒绝 >64 避免 R3 backend 隐式截断
        if (payload.size() > 64) {
            appendTrace(QStringLiteral(
                "原子提交 payload 最多 64 字节（协议上限），当前 %1 字节")
                .arg(payload.size()));
            return;
        }

        // 2) 解析用户指定的页内 offset：支持 0x 前缀 hex 与十进制
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
        // offset+length 越页 R0 会拒；GUI 侧提前拒以给用户立即反馈
        if (parsedOffset + static_cast<qulonglong>(payload.size()) > 4096ULL) {
            appendTrace(QStringLiteral(
                "原子提交 offset(0x%1) + length(%2) 越页（>4096）")
                .arg(static_cast<quint64>(parsedOffset), 0, 16)
                .arg(payload.size()));
            return;
        }

        // 3) 把本次选择塞进 plan（原子路径不走 Backup，recordId 保持 0）
        if (!beginOperation()) return;
        // 原子请求不产生原字节备份，不能沿用此前分步记录的验证结论。
        m_plan.atomicOriginalBytesUnverified = true;
        m_plan.restored = false;
        m_plan.payload = payload;
        m_plan.payloadOffset = static_cast<unsigned long>(parsedOffset & 0xFFFFFFFFULL);

        m_writeStatusLabel->setText(QStringLiteral("状态：原子提交中..."));
        appendTrace(QStringLiteral(
            "AtomicCommit(va=0x%1, offset=0x%2, len=%3) bytes=%4")
            .arg(m_plan.targetVa, 0, 16)
            .arg(static_cast<quint64>(m_plan.payloadOffset), 0, 16)
            .arg(payload.size())
            .arg(bytesToHex(payload)));

        // 4) Worker thread：一次 IOCTL 走完 Register+Change+Write+Unregister(SKIP)。
        //    R3 backend 的 atomicCommit 签名 (targetVa, offset, bytes)，无 recordId。
        const quint64 va = m_plan.targetVa;
        const unsigned long offsetArg = m_plan.payloadOffset;
        auto* w = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(w, &QFutureWatcher<rxpf::PageInfo>::finished, this,
            [this, w]() {
                const auto info = w->result();
                w->deleteLater();
                // 只有完整回执中的ERROR记录仍由驱动保留；归集必须早于setBusy(false)归还lease。
                // 独立保存身份，保留用户已有分步记录；重复回执不会重复注销。
                if (info.ioSuccess && info.state == KSWORD_ARK_RXPF_PAGE_STATE_ERROR
                    && info.recordId != 0ULL)
                {
                    if (!m_plan.atomicCleanupRecordIds.contains(info.recordId))
                        m_plan.atomicCleanupRecordIds.append(info.recordId);
                    m_plan.atomicOriginalBytesUnverified = true;
                    m_plan.restored = false;
                    appendTrace(QStringLiteral("原子提交遗留记录 %1，需重试资源清理。").arg(info.recordId));
                }
                if (!info.ioSuccess) {
                    appendTrace(QStringLiteral("AtomicCommit 失败：%1").arg(info.ioErrorText));
                    m_writeStatusLabel->setText(QStringLiteral("状态：原子提交失败"));
                } else if (info.lastStatus < 0) {
                    // IOCTL 通了但 R0 返回失败 NTSTATUS（例如 CreateRecord 拒绝）
                    appendTrace(QStringLiteral(
                        "AtomicCommit NTSTATUS 失败 status=%1 reason=%2")
                        .arg(formatHex4(static_cast<quint32>(info.lastStatus)))
                        .arg(info.lastFailureReason));
                    m_writeStatusLabel->setText(QStringLiteral(
                        "状态：原子提交失败（R0 NTSTATUS）"));
                } else {
                    appendTrace(QStringLiteral(
                        "AtomicCommit OK status=%1 pageBase=0x%2 origProt=0x%3 curProt=0x%4")
                        .arg(formatHex4(static_cast<quint32>(info.lastStatus)))
                        .arg(info.pageBase, 0, 16)
                        .arg(info.originalProtection, 0, 16)
                        .arg(info.currentProtection, 0, 16));
                    m_writeStatusLabel->setText(QStringLiteral(
                        "状态：原子提交返回成功；字节与执行权限仍需独立核验"));
                }
                setBusy(false);
            });
        w->setFuture(QtConcurrent::run([va, offsetArg, payload]() -> rxpf::PageInfo {
            return rxpf::atomicCommit(va, offsetArg, payload);
        }));
    }


    void KernelHookWizard::onKeepModifiedClicked()
    {
        if (m_busy) return;
        if (m_plan.recordId == 0ULL)
        {
            appendTrace(QStringLiteral("请先完成第 3 步 Write"));
            return;
        }
        if (!beginOperation()) return;
        const quint64 rid = m_plan.recordId;
        appendTrace(QStringLiteral("Unregister(FLAG_SKIP_BACKUP_RESTORE) recordId=%1")
            .arg(rid));
        auto* watcher = new QFutureWatcher<rxpf::PageInfo>(this);
        connect(watcher, &QFutureWatcher<rxpf::PageInfo>::finished, this, [this, watcher]() {
            const auto result = watcher->result();
            watcher->deleteLater();
            m_plan.mode1Status = result.lastStatus;
            if (pageSucceeded(result))
            {
                m_plan.recordId = 0ULL;
                m_plan.recordPrepared = false;
                m_keepStatusLabel->setText(QStringLiteral("状态：payload 已就位；立刻进入第 5 步恢复以避免蓝屏"));
            }
            else
            {
                m_keepStatusLabel->setText(QStringLiteral("状态：注销或权限恢复失败，记录保留供重试"));
                appendTrace(pageFailure(result));
            }
            setBusy(false);
        });
        watcher->setFuture(QtConcurrent::run([rid]() { return rxpf::unregisterPage(rid, true); }));
    }

    void KernelHookWizard::onRestoreClicked()
    {
        if (m_busy) return;
        if (!beginOperation()) return;
        const KernelHookPlan snapshot = m_plan;
        m_finalStatusLabel->setText(QStringLiteral("状态：恢复中..."));
        auto* watcher = new QFutureWatcher<RecoveryResult>(this);
        connect(watcher, &QFutureWatcher<RecoveryResult>::finished, this, [this, watcher]() {
            const auto result = watcher->result();
            watcher->deleteLater();
            m_plan = result.plan;
            setBusy(false);
            if (!result.ok)
            {
                m_closeAfterCleanup = false;
                m_prepareAfterCleanup = false;
                m_finalStatusLabel->setText(QStringLiteral("恢复尚未完成：%1").arg(result.failure));
                appendTrace(result.failure);
                return;
            }
            m_finalStatusLabel->setText(m_plan.atomicOriginalBytesUnverified
                ? QStringLiteral("状态：资源清理已完成；原子提交没有原字节备份，尚未证实原字节恢复。")
                : QStringLiteral("状态：原字节及资源清理已完成；执行权限以驱动恢复回执为准。"));
            if (m_targetChangePending)
            {
                m_targetChangePending = false;
                m_plan = m_requestedTarget;
                updateTargetFromInputs();
            }
            if (m_closeAfterCleanup)
            {
                m_closeAfterCleanup = false;
                close();
            }
            else if (m_prepareAfterCleanup)
            {
                m_prepareAfterCleanup = false;
                m_plan.restoreRanges.clear();
                onPrepareClicked();
            }
        });
        watcher->setFuture(QtConcurrent::run([snapshot]() { return recoverOwnedPage(snapshot); }));
    }

    // 验证只读回放原范围，不再注册新记录，也不把PE节头当成live PTE证据。
    void KernelHookWizard::onFinalVerifyClicked()
    {
        if (m_busy) return;
        const KernelHookPlan snapshot = m_plan;
        if (!beginOperation()) return;
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
            const QString failure = watcher->result();
            watcher->deleteLater();
            m_finalStatusLabel->setText(failure.isEmpty()
                ? QStringLiteral("验证：缓存原字节回读一致；未采样 live PTE。") : failure);
            setBusy(false);
        });
        watcher->setFuture(QtConcurrent::run([snapshot]() -> QString {
            if (snapshot.restoreRanges.isEmpty() || snapshot.hasUncachedWrites
                || snapshot.atomicOriginalBytesUnverified)
                return QStringLiteral("缺少完整原字节证据，无法证实恢复。请保留当前窗口并处理目标页面。");
            for (const auto& range : snapshot.restoreRanges)
            {
                const auto read = rxpf::readPageBytes(snapshot.targetVa, range.offset,
                    static_cast<unsigned long>(range.bytes.size()));
                if (!read.ok) return read.errorText;
                if (read.bytes != range.bytes) return QStringLiteral("恢复后字节与原始备份不一致。");
            }
            return QString();
        }));
    }
}
