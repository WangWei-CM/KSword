#include "../UI/StructuredFieldView.h"
// ============================================================
// MinidumpDock.cpp
// 作用：
// - 实现转储分析页的界面骨架与交互：
//   工具栏（路径 + 浏览 + 系统目录 + 解析 + 导出）、状态栏与结果页签；
// - 解析任务通过全局线程池执行，用 generation + 共享 owner 状态
//   防止过期结果回写与退出阶段竞态（与 ScannerDock 相同的模式）；
// - 表格渲染与报告生成的实现位于 MinidumpDock.Tables.cpp。
// ============================================================

#include "MinidumpDock.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PerfTrace.h"
#include "../UI/AsyncOperation.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/VisibleTableWidget.h"

#include "../Framework.h"
#include "CrashHistory.h"
#include "DumpAnalyzer.h"
#include "DumpAutoCheck.h"
#include "DumpMemoryView.h"
#include "DumpPoolTag.h"
#include "DumpSymbolResolver.h"
#include "Internationalization/LanguageManager.h"
#include "MinidumpParser.h"
#include "UI/CodeEditorWidget.h"
#include "theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QSaveFile>
#include <QPointer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include "../UI/PrimaryPageStyle.h"
#include "../UI/FlatButtonTheme.h"
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QShowEvent>
#include <QTableWidget>
#include <QTextBrowser>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>
#include <mutex>
#include <utility>

// MinidumpAsyncState 作用：把 worker 的回投目标放在共享互斥状态中。
// 析构前先清空 owner；worker 只有在同一把锁保护下才能提交 queued 调用，
// 从而消除应用退出阶段的接收者竞态。
struct MinidumpExportRequest
{
    std::shared_ptr<const ks::minidump::DumpParseResult> result;
    QString savePath;
    std::shared_ptr<const ks::ui::FieldDocument> document;
    std::function<QString(const QString&)> translate;
};

struct MinidumpExportResult
{
    QString savePath;
    QString errorText;
    bool ok = false;
};

struct MinidumpAsyncState
{
    std::mutex mutex;               // mutex：保护 owner 的读写。
    MinidumpDock* owner = nullptr;  // owner：结果回投的目标控件；析构后为空。
    ks::minidump::DumpCancellationToken cancellation; // One explicit parse request.
};

namespace
{
    enum class InitialBodyText
    {
        PathLabel, PathPlaceholder, Browse, SystemDirectory, Parse, Export,
        SymbolLabel, SymbolPlaceholder, Idle
    };
    struct InitialBodyEntry
    {
        const char* targetObjectName;
        const char* key;
        const char* fallback;
        bool mnemonic;
    };
    // The UI and cold search consume the same text/target definitions. No
    // result-page content is searchable before a real dump has been parsed.
    constexpr InitialBodyEntry kInitialBody[] = {
        {"ksword_minidump_path_label", "minidump.path.label", "转储文件", false},
        {"ksword_minidump_path_input", "minidump.path.placeholder",
            "选择要解析的转储文件（应用崩溃 .dmp / 蓝屏 Minidump / MEMORY.DMP）", false},
        {"ksword_minidump_browse", "minidump.action.browse", "浏览…", true},
        {"ksword_minidump_system_directory", "minidump.action.system_dir", "系统转储", true},
        {"ksword_minidump_parse", "minidump.action.parse", "解析", true},
        {"ksword_minidump_export", "minidump.action.export", "导出报告", true},
        {"ksword_minidump_symbol_label", "minidump.symbol.label", "符号路径", false},
        {"ksword_minidump_symbol_input", "minidump.symbol.placeholder",
            "留空则搜索转储所在目录与本机符号缓存；分号分隔可加自己的构建输出目录", false},
        {"ksword_minidump_status", "minidump.status.idle",
            "选择一个转储文件开始解析：支持应用崩溃 MDMP 与系统蓝屏 DMP。", false}
    };
    const InitialBodyEntry& initialBodyEntry(const InitialBodyText id)
    {
        return kInitialBody[static_cast<int>(id)];
    }
    QString initialBodyText(const InitialBodyText id)
    {
        const auto& entry = initialBodyEntry(id);
        return ks::i18n::text(QString::fromLatin1(entry.key), QString::fromUtf8(entry.fallback));
    }
    QString initialBodyObjectName(const InitialBodyText id)
    {
        return QString::fromLatin1(initialBodyEntry(id).targetObjectName);
    }
    // All encoding and file work is worker-owned. The writer adapter also permits
    // pure-value short-write/failure tests without touching a real filesystem.
    template<class Writer>
    bool writeExportBytes(Writer& file, const QByteArray& bytes, QString& errorText)
    {
        if (!file.open(QIODevice::WriteOnly))
        {
            errorText = file.errorString();
            return false;
        }
        qint64 offset = 0;
        while (offset < bytes.size())
        {
            const qint64 written = file.write(bytes.constData() + offset, bytes.size() - offset);
            if (written <= 0 || written > bytes.size() - offset)
            {
                errorText = file.errorString();
                file.cancelWriting();
                return false;
            }
            offset += written;
        }
        if (!file.commit())
        {
            errorText = file.errorString();
            return false;
        }
        return true;
    }


    // dumpInputStyle / dumpButtonStyle 作用：
    // - 给本页的输入框与按钮套上与其它 Dock 一致的主题外观；
    // - 颜色一律取自 KswordTheme 的动态 token（展开为 palette(...) 字面量），
    //   由 Qt 在每次绘制时按控件当前调色板求值，主题切换能同步生效。
    QString dumpInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{"
            "  border:1px solid %2;"
            "  border-radius:4px;"
            "  background:%3;"
            "  color:%4;"
            "  padding:2px 6px;"
            "}"
            "QLineEdit:focus{ border:1px solid %1; }")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString dumpButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    // defaultDumpDirectory 作用：给文件选择器一个有意义的起始目录。
    // 优先系统蓝屏小型转储目录，其次 Windows 目录（MEMORY.DMP 所在），
    // 都不存在时回退用户主目录；返回本地风格路径。
    QString defaultDumpDirectory()
    {
        // systemRoot：SystemRoot 环境变量；服务化环境可能为空。
        const QString systemRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
        const QString minidumpDir = systemRoot + QStringLiteral("\\Minidump");
        if (QFileInfo::exists(minidumpDir))
        {
            return QDir::toNativeSeparators(minidumpDir);
        }
        if (QFileInfo::exists(systemRoot))
        {
            return QDir::toNativeSeparators(systemRoot);
        }
        return QDir::toNativeSeparators(QDir::homePath());
    }
}

QVector<ks::ui::PageSearchBodyText> MinidumpDock::coldSearchBody()
{
    QVector<ks::ui::PageSearchBodyText> body;
    body.reserve(static_cast<qsizetype>(sizeof(kInitialBody) / sizeof(kInitialBody[0])));
    for (const auto& entry : kInitialBody)
    {
        body.append({ks::i18n::text(QString::fromLatin1(entry.key), QString::fromUtf8(entry.fallback)),
            QString::fromLatin1(entry.targetObjectName), {}, entry.mnemonic});
    }
    return body;
}

MinidumpDock::MinidumpDock(QWidget* parent)
    : QWidget(parent)
{
    // m_asyncState：与 worker 共享的生命周期状态，必须先于任何解析建立。
    m_asyncState = std::make_shared<MinidumpAsyncState>();
    m_asyncState->owner = this;
    buildUi();
    retranslateUi();
    const auto& idle = initialBodyEntry(InitialBodyText::Idle);
    setStatus(idle.key, idle.fallback);
    // 转储页头与结果共用整页外框，结果Tab本身不制造外圆，原8px留白保持一致。
    ks::ui::StyleWorkspaceContentFrame(this, 8);
}

MinidumpDock::~MinidumpDock()
{
    if (m_reportBuildOperation != nullptr) m_reportBuildOperation->close();
    if (m_exportOperation != nullptr) m_exportOperation->close();
    // 加锁清空 owner：此后 worker 即使完成也不会再向本控件投递结果。
    std::lock_guard<std::mutex> lock(m_asyncState->mutex);
    m_asyncState->owner = nullptr;
    if (m_asyncState->cancellation)
    {
        m_asyncState->cancellation->store(true, std::memory_order_relaxed);
    }
}

void MinidumpDock::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::LanguageChange)
    {
        retranslateUi();
    }
}

void MinidumpDock::buildUi()
{
    // rootLayout：承载路径工具条、状态提示与结果页签。
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(0); // 来源、诊断状态与结果Tab连成同宽头部。

    // pathLayout：路径输入与全部动作按钮排在同一行。
    auto* sourceToolbar = new QWidget(this); // 转储与符号来源集中在结果上方，不挤占各结果页工具栏。
    auto* sourceLayout = new QVBoxLayout(sourceToolbar);
    sourceLayout->setContentsMargins(6, 4, 6, 6);
    sourceLayout->setSpacing(8);
    auto* pathLayout = new QHBoxLayout();
    pathLayout->setSpacing(6);
    m_pathLabel = new QLabel(this);
    m_pathLabel->setObjectName(initialBodyObjectName(InitialBodyText::PathLabel));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setObjectName(initialBodyObjectName(InitialBodyText::PathPlaceholder));
    m_pathEdit->setClearButtonEnabled(true);
    m_browseButton = new QPushButton(this);
    m_browseButton->setObjectName(initialBodyObjectName(InitialBodyText::Browse));
    m_systemDirButton = new QPushButton(this);
    m_systemDirButton->setObjectName(initialBodyObjectName(InitialBodyText::SystemDirectory));
    m_parseButton = new QPushButton(this);
    m_parseButton->setObjectName(initialBodyObjectName(InitialBodyText::Parse));
    m_exportButton = new QPushButton(this);
    m_exportButton->setObjectName(initialBodyObjectName(InitialBodyText::Export));
    // 图标：含义简单的按钮用标准图标表达，文字配合悬停释义。
    m_browseButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_systemDirButton->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
    m_parseButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_exportButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_exportButton->setEnabled(false);

    // 主题化：此前这些控件用的是 Qt 默认外观，和其它 Dock 的蓝色主题对不上。
    const QString inputStyle = dumpInputStyle();
    const QString buttonStyle = dumpButtonStyle();
    m_pathEdit->setStyleSheet(inputStyle);
    for (QPushButton* const actionButton :
        { m_browseButton, m_systemDirButton, m_parseButton, m_exportButton })
    {
        actionButton->setStyleSheet(buttonStyle);
    }

    pathLayout->addWidget(m_pathLabel);
    pathLayout->addWidget(m_pathEdit, 1);
    pathLayout->addWidget(m_browseButton);
    pathLayout->addWidget(m_systemDirButton);
    pathLayout->addWidget(m_parseButton);
    pathLayout->addWidget(m_exportButton);
    ks::ui::NormalizeToolbarRow(pathLayout);
    sourceLayout->addLayout(pathLayout);

    // 符号路径行：默认路径覆盖不到"自己编译出来的驱动"这种最常见的自查场景——
    // 它的 .pdb 通常躺在构建输出目录里，既不在转储旁边也不在系统符号缓存里。
    // 没有这个入口，本项目自己的驱动崩溃就永远只能看到 模块+偏移。
    QHBoxLayout* const symbolLayout = new QHBoxLayout();
    symbolLayout->setContentsMargins(0, 0, 0, 0);
    symbolLayout->setSpacing(6);
    m_symbolPathLabel = new QLabel(this);
    m_symbolPathLabel->setObjectName(initialBodyObjectName(InitialBodyText::SymbolLabel));
    m_symbolPathEdit = new QLineEdit(this);
    m_symbolPathEdit->setObjectName(initialBodyObjectName(InitialBodyText::SymbolPlaceholder));
    m_symbolPathEdit->setClearButtonEnabled(true);
    m_symbolPathEdit->setStyleSheet(inputStyle);
    // 文件路径和符号路径采用普通参数输入，不套搜索语义或改变加载时机。
    ks::ui::StyleStandaloneInput(m_pathEdit);
    ks::ui::StyleStandaloneInput(m_symbolPathEdit);
    symbolLayout->addWidget(m_symbolPathLabel);
    symbolLayout->addWidget(m_symbolPathEdit, 1);
    ks::ui::NormalizeToolbarRow(symbolLayout);
    sourceLayout->addLayout(symbolLayout);
    ks::ui::StylePrimaryToolbar(sourceToolbar);
    rootLayout->addWidget(sourceToolbar);
    // 解析是当前页面的主动作，来源选择和导出使用同一中性操作底面。
    ks::ui::ApplyFlatButtonTheme(m_parseButton, ks::ui::FlatButtonTone::Accent);
    for (QPushButton* action : {m_browseButton, m_systemDirButton, m_exportButton})
    {
        ks::ui::ApplyFlatButtonTheme(action, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
    }

    // m_statusLabel：允许复制诊断状态，长路径自动换行。
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(initialBodyObjectName(InitialBodyText::Idle));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceLayout->addWidget(m_statusLabel);

    // m_resultTabs：解析结果页签；具体页在 renderResult 里按数据动态挂载。
    m_resultTabs = new QTabWidget(this);
    m_resultTabs->setDocumentMode(true);
    ks::ui::StyleJoinedResultTabs(m_resultTabs, sourceToolbar);
    rootLayout->addWidget(m_resultTabs, 1);

    // 预创建全部表格与报告编辑器：语言切换/重复解析时复用，不反复销毁。
    m_analysisView = new QTextBrowser(m_resultTabs);
    m_analysisView->setOpenExternalLinks(false);
    m_analysisView->setFrameShape(QFrame::NoFrame);
    m_blameTable = createReadOnlyTable(m_resultTabs);
    m_stackTable = createReadOnlyTable(m_resultTabs);
    m_registerTable = createReadOnlyTable(m_resultTabs);
    // 寄存器和摘要属于单份转储的属性投影，不展示快照/导出条占据阅读空间。
    ks::ui::SetTableActionBarMode(m_registerTable, ks::ui::TableActionBarMode::None);
    m_overviewTable = createReadOnlyTable(m_resultTabs);
    ks::ui::SetTableActionBarMode(m_overviewTable, ks::ui::TableActionBarMode::None);
    m_exceptionTable = createReadOnlyTable(m_resultTabs);
    ks::ui::SetTableActionBarMode(m_exceptionTable, ks::ui::TableActionBarMode::None);
    m_executionContextTable = createReadOnlyTable(m_resultTabs);
    ks::ui::SetTableActionBarMode(m_executionContextTable, ks::ui::TableActionBarMode::None);
    m_streamTable = createReadOnlyTable(m_resultTabs);
    m_moduleTable = createReadOnlyTable(m_resultTabs);
    m_threadTable = createReadOnlyTable(m_resultTabs);
    m_memoryTable = createReadOnlyTable(m_resultTabs);
    m_handleTable = createReadOnlyTable(m_resultTabs);
    m_unloadedTable = createReadOnlyTable(m_resultTabs);
    m_symbolTable = createReadOnlyTable(m_resultTabs);
    m_poolTagTable = createReadOnlyTable(m_resultTabs);
    m_crashHistoryTable = createReadOnlyTable(m_resultTabs);
    // 历史跨多次转储更新，保留完整快照与比对；其它解析证据表只需复制/导出。
    ks::ui::SetTableActionBarMode(m_crashHistoryTable, ks::ui::TableActionBarMode::Full);
    m_crashHistoryTable->setWordWrap(true);
    // 肇事模块表的证据列是长文本，允许换行以免被省略号截断。
    m_blameTable->setWordWrap(true);
    // 符号状态表的"说明"列同理：不匹配的具体差异必须完整读到，不能被截断。
    m_symbolTable->setWordWrap(true);
    m_poolTagTable->setWordWrap(true);
    // 调用栈表不拆 A/B/C：它的“来源”列标着「栈扫描（可能误报）」，
    // 一旦被列组预设隐藏，整页就再没有任何地方提示这些帧是猜的。
    // 六列在正常窗口宽度下放得下，没必要为它牺牲这条提示。
    m_stackPage = m_stackTable;
    // 宽表包装成 A/B/C 列组页；列数与 Tables.cpp 的填充逻辑保持一致。
    m_modulePage = createStructuredTablePage(m_moduleTable, 8);
    m_threadPage = createStructuredTablePage(m_threadTable, 13);
    m_memoryPage = createStructuredTablePage(m_memoryTable, 6);
    m_handlePage = createStructuredTablePage(m_handleTable, 7);
    m_rawMemoryTabs = new QTabWidget(m_resultTabs);
    m_rawMemoryTabs->setDocumentMode(true);
    ks::ui::StylePageTabs(m_rawMemoryTabs);
    m_memoryView = new DumpMemoryView(m_resultTabs);
    m_reportEditor = new ks::ui::StructuredFieldView(m_resultTabs);
    m_reportEditor->setValueModelProjection();
    m_reportEditor->setHiddenProjectionDeferred();


    // 预创建的页全部以 m_resultTabs 为父，但此刻一个都还没 addTab。
    // 有父而未进 tab 栈的控件会作为 QTabWidget 的普通子控件浮在客户区上，
    // 表现就是"几个表格和列组按钮叠在一起"。必须显式隐藏，
    // 由 addTab 负责在需要时显示；clearResultTabs 移除后也要重新隐藏。
    for (QWidget* const pendingPage : {
            static_cast<QWidget*>(m_analysisView),
            static_cast<QWidget*>(m_blameTable),
            static_cast<QWidget*>(m_overviewTable),
            static_cast<QWidget*>(m_exceptionTable),
            static_cast<QWidget*>(m_executionContextTable),
            static_cast<QWidget*>(m_streamTable),
            static_cast<QWidget*>(m_unloadedTable),
            static_cast<QWidget*>(m_registerTable),
            static_cast<QWidget*>(m_symbolTable),
            static_cast<QWidget*>(m_poolTagTable),
            static_cast<QWidget*>(m_crashHistoryTable),
            m_stackPage, m_modulePage, m_threadPage, m_memoryPage, m_handlePage,
            static_cast<QWidget*>(m_rawMemoryTabs),
            static_cast<QWidget*>(m_memoryView),
            static_cast<QWidget*>(m_reportEditor) })
    {
        if (pendingPage != nullptr)
        {
            pendingPage->hide();
        }
    }

    connect(m_resultTabs, &QTabWidget::currentChanged, this,
        [this](int) { renderCurrentResultPage(); });
    connect(m_rawMemoryTabs, &QTabWidget::currentChanged, this,
        [this](int) { renderCurrentRawMemoryPage(); });

    // 所有动作统一进入成员函数的校验流程。
    connect(m_browseButton, &QPushButton::clicked, this, [this]() { chooseFile(); });
    connect(m_systemDirButton, &QPushButton::clicked, this, [this]()
        {
            // 系统目录按钮：直接把选择器定位到蓝屏转储目录。
            m_pathEdit->setText(defaultDumpDirectory());
            chooseFile();
        });
    connect(m_parseButton, &QPushButton::clicked, this, [this]() { beginParse(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this]() { exportReport(); });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() { beginParse(); });
}

QString MinidumpDock::translated(const char* key, const char* fallback) const
{
    return ks::i18n::text(
        QString::fromLatin1(key),
        QString::fromUtf8(fallback));
}

void MinidumpDock::retranslateUi()
{
    m_pathLabel->setText(initialBodyText(InitialBodyText::PathLabel));
    m_pathEdit->setPlaceholderText(initialBodyText(InitialBodyText::PathPlaceholder));
    m_symbolPathLabel->setText(initialBodyText(InitialBodyText::SymbolLabel));
    m_symbolPathEdit->setPlaceholderText(initialBodyText(InitialBodyText::SymbolPlaceholder));
    m_symbolPathEdit->setToolTip(translated(
        "minidump.symbol.tooltip",
        "只搜索本地目录，不联网下载符号。"
        "映像与转储记录不一致时会明确判为不匹配，并拒绝给出会误导人的行号。"));
    m_browseButton->setText(initialBodyText(InitialBodyText::Browse));
    m_browseButton->setToolTip(translated(
        "minidump.action.browse.tooltip",
        "选择一个转储文件进行只读解析"));
    m_systemDirButton->setText(initialBodyText(InitialBodyText::SystemDirectory));
    m_systemDirButton->setToolTip(translated(
        "minidump.action.system_dir.tooltip",
        "定位到系统蓝屏转储目录（C:\\Windows\\Minidump）"));
    m_parseButton->setText(initialBodyText(InitialBodyText::Parse));
    m_parseButton->setToolTip(translated(
        "minidump.action.parse.tooltip",
        "在后台解析转储结构，不会修改目标文件"));
    m_exportButton->setText(initialBodyText(InitialBodyText::Export));
    m_exportButton->setToolTip(translated(
        "minidump.action.export.tooltip",
        "把当前解析结果的全文报告保存为文本文件"));
    refreshStatus();

    // 表格按语言失效后仅投影当前页；规范字段视图自行重译，不重读内存。
    retranslateResultPages();
}

void MinidumpDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    renderCurrentResultPage();
}

void MinidumpDock::chooseFile()
{
    // startDir：优先当前输入的路径（或其所在目录），否则用系统转储目录。
    QString startDir = m_pathEdit->text().trimmed();
    if (!startDir.isEmpty())
    {
        const QFileInfo startInfo(startDir);
        startDir = startInfo.isDir() ? startInfo.absoluteFilePath() : startInfo.absolutePath();
    }
    if (startDir.isEmpty() || !QFileInfo::exists(startDir))
    {
        startDir = defaultDumpDirectory();
    }
    const QString selectedPath = QFileDialog::getOpenFileName(
        this,
        translated("minidump.dialog.choose_file", "选择转储文件"),
        startDir,
        translated(
            "minidump.dialog.file_filter",
            "转储文件 (*.dmp *.mdmp *.hdmp *.kdmp);;所有文件 (*.*)"));
    if (selectedPath.isEmpty())
    {
        return;
    }
    m_pathEdit->setText(QDir::toNativeSeparators(selectedPath));
    beginParse();
}

void MinidumpDock::beginParse()
{
    if (m_parseBusy)
    {
        return;
    }

    // fileInfo：启动线程前先排除空路径、目录和不存在的目标。
    const QString path = QDir::toNativeSeparators(m_pathEdit->text().trimmed());
    const QFileInfo fileInfo(path);
    if (path.isEmpty() || !fileInfo.exists() || !fileInfo.isFile())
    {
        QMessageBox::warning(
            this,
            translated("minidump.dialog.invalid_file.title", "无法解析"),
            translated("minidump.dialog.invalid_file.body", "请选择一个存在的转储文件。"));
        return;
    }

    m_pathEdit->setText(path);
    setBusy(true);
    setStatus(
        "minidump.status.parsing",
        "正在解析：%1",
        QStringList{ path });

    {
        // 解析开始日志：整个动作链共用一个 kLogEvent 便于追踪。
        kLogEvent parseEvent;
        info << parseEvent << "MinidumpDock 开始解析转储文件: "
             << path.toStdString() << eol;
    }

    // generation：只有最新一代结果可以回写，避免旧解析覆盖新目标。
    const std::uint64_t generation = ++m_parseGeneration;
    const std::shared_ptr<MinidumpAsyncState> asyncState = m_asyncState;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(asyncState->mutex);
        if (asyncState->cancellation)
        {
            asyncState->cancellation->store(true, std::memory_order_relaxed);
        }
        asyncState->cancellation = cancellation;
    }
    // symbolPath：在 UI 线程取值后按值带进 worker，worker 内不得触碰控件。
    const QString symbolPath = m_symbolPathEdit->text().trimmed();
    QThreadPool::globalInstance()->start(
        [asyncState, generation, path, symbolPath, cancellation]()
        {
            // result：worker 中完成的解析产物；自包含，与文件映射无关联。
            // 解析的输入是不可信文件，畸形样本可能让某个列表申请超大内存。
            // 线程池 worker 里逃逸的异常会直接 std::terminate 掉整个进程，
            // 因此这里必须兜住，把它降级成一次“解析失败”。
            std::shared_ptr<ks::minidump::DumpParseResult> result;
            try
            {
                const ks::minidump::ScopedDumpCancellation cancellationScope(cancellation);
                ks::minidump::CheckDumpCancellation();
                result = std::make_shared<ks::minidump::DumpParseResult>(
                    ks::minidump::ParseDumpFile(path, cancellation));
                ks::minidump::CheckDumpCancellation();
                // 符号化放在解析之后、回主线程之前：DbgHelp 加载 PDB 可能耗时到
                // 秒级，绝不能放在 UI 线程上。失败不影响已有结论，因此不改 success。
                if (result->success)
                {
                    ks::minidump::ApplySymbols(symbolPath, *result, cancellation);
                    ks::minidump::CheckDumpCancellation();
                    // 池标记归属同样可能读大量磁盘（要在模块映像里搜标记字节），
                    // 一并留在 worker 里做。
                    ks::minidump::ApplyPoolTagAttribution(*result, cancellation);
                    ks::minidump::CheckDumpCancellation();
                    // 崩溃时间线：单看一个转储答不出"这次是蓝屏还是没转储的硬挂死"，
                    // 也答不出"崩溃当时跑的是哪一次构建的驱动"。筛选器名过滤成
                    // Ksword，是因为本项目最需要对账的就是自己那个驱动的映像时间戳。
                    QString historyError;
                    result->crashHistory = ks::minidump::CollectCrashHistory(
                        30, QStringLiteral("Ksword"), &historyError, cancellation);
                    ks::minidump::CheckDumpCancellation();
                    if (!historyError.isEmpty())
                    {
                        result->diagnostics.append(historyError);
                    }
                }
            }
            catch (const ks::minidump::DumpWorkCancelled&)
            {
                return; // Unwind the mapping/session/handles; no partial UI result.
            }
            catch (const std::exception& error)
            {
                result = std::make_shared<ks::minidump::DumpParseResult>();
                result->filePath = path;
                result->errorText =
                    QStringLiteral("解析过程中发生异常，文件可能已损坏或结构异常：%1")
                        .arg(QString::fromUtf8(error.what()));
            }
            catch (...)
            {
                result = std::make_shared<ks::minidump::DumpParseResult>();
                result->filePath = path;
                result->errorText =
                    QStringLiteral("解析过程中发生未知异常，文件可能已损坏或结构异常。");
            }
            std::lock_guard<std::mutex> lock(asyncState->mutex);
            MinidumpDock* receiver = asyncState->owner;
            if (receiver == nullptr || cancellation->load(std::memory_order_relaxed))
            {
                return;
            }
            QMetaObject::invokeMethod(
                receiver,
                [asyncState, generation, cancellation, result = std::move(result)]()
                {
                    // owner 复核：queued 回调真正执行时控件可能已析构。
                    MinidumpDock* owner = nullptr;
                    {
                        std::lock_guard<std::mutex> stateLock(asyncState->mutex);
                        owner = asyncState->owner;
                    }
                    if (owner != nullptr &&
                        !cancellation->load(std::memory_order_relaxed))
                    {
                        owner->finishParse(generation, result);
                    }
                },
                Qt::QueuedConnection);
        });
}

void MinidumpDock::finishParse(
    const std::uint64_t generation,
    std::shared_ptr<ks::minidump::DumpParseResult> result)
{
    if (generation != m_parseGeneration.load() || !result)
    {
        return;
    }

    // 采集：解析完成回到 UI 线程后的整体收尾（含结果渲染）耗时。
    ks::ui::PerfScope finishParsePerf("perf_dump_finish_parse", QString());
    setBusy(false);
    m_lastResult = std::move(result);
    renderResult(*m_lastResult);
    m_exportButton->setEnabled(m_lastResult->success &&
        (m_exportOperation == nullptr || !m_exportOperation->isBusy()));

    {
        // 解析结束日志：记录结果类别与成败，方便回溯。
        kLogEvent parseEvent;
        if (m_lastResult->success)
        {
            info << parseEvent << "MinidumpDock 解析完成: "
                 << m_lastResult->filePath.toStdString()
                 << " 模块 " << m_lastResult->modules.size()
                 << " 线程 " << m_lastResult->threads.size() << eol;
        }
        else
        {
            warn << parseEvent << "MinidumpDock 解析失败: "
                 << m_lastResult->filePath.toStdString()
                 << " 原因: " << m_lastResult->errorText.toStdString() << eol;
        }
    }

    if (m_lastResult->success)
    {
        // kindText：结果类别的状态词，走词条翻译后再代入状态文本。
        QString kindText;
        switch (m_lastResult->kind)
        {
        case ks::minidump::DumpKind::UserMinidump:
            kindText = translated("minidump.kind.user", "用户态 MDMP");
            break;
        case ks::minidump::DumpKind::KernelDump64:
            kindText = translated("minidump.kind.kernel64", "64 位内核转储");
            break;
        case ks::minidump::DumpKind::KernelDump32:
            kindText = translated("minidump.kind.kernel32", "32 位内核转储");
            break;
        default:
            kindText = translated("minidump.kind.unknown", "未知");
            break;
        }
        // 状态行放结论而不是路径：路径就在上方的输入框里，重复一遍没有信息量，
        // 而"结论 + 可信度"是用户解析完最想立刻看到的一句话。
        if (!m_lastResult->analysis.headline.isEmpty())
        {
            setStatus(
                "minidump.status.success_analysis",
                "%1（%2 · 可信度 %3）",
                QStringList{
                    ks::i18n::sourceText(m_lastResult->analysis.headline),
                    kindText,
                    ks::i18n::sourceText(ks::minidump::AnalysisConfidenceText(
                        m_lastResult->analysis.confidence)) });
        }
        else
        {
            setStatus(
                "minidump.status.success",
                "解析完成：%1（%2）。",
                QStringList{ m_lastResult->filePath, kindText });
        }
    }
    else if (m_lastResult->recognized)
    {
        // errorText 为解析层产出的中文规范文本，整串词条翻译后代入。
        setStatus(
            "minidump.status.malformed",
            "已识别转储格式，但内容损坏或截断：%1",
            QStringList{ ks::i18n::sourceText(m_lastResult->errorText) });
    }
    else
    {
        setStatus(
            "minidump.status.unrecognized",
            "不是受支持的转储文件：%1",
            QStringList{ ks::i18n::sourceText(m_lastResult->errorText) });
    }

    if (m_lastResult->success)
    {
        promptKswordRelatedCrash(*m_lastResult);
    }
}

void MinidumpDock::openDumpFile(const QString& filePath)
{
    const QString normalizedPath = QDir::toNativeSeparators(filePath.trimmed());
    if (normalizedPath.isEmpty() || m_pathEdit == nullptr)
    {
        return;
    }
    m_pathEdit->setText(normalizedPath);
    beginParse();
}

void MinidumpDock::promptKswordRelatedCrash(const ks::minidump::DumpParseResult& result)
{
    const ks::minidump::KswordRelevance relevance =
        ks::minidump::EvaluateKswordRelevance(result);
    if (!relevance.related)
    {
        return;
    }

    {
        kLogEvent relatedEvent;
        warn << relatedEvent << "MinidumpDock 解析结果指向 KSword 自身组件: "
             << result.filePath.toStdString()
             << " 命中 " << relevance.matchedModules.join(QStringLiteral(",")).toStdString()
             << eol;
    }

    QMessageBox messageBox(this);
    messageBox.setIcon(QMessageBox::Warning);
    messageBox.setWindowTitle(
        translated("minidump.dialog.ksword_related.title", "这次崩溃与 KSword 有关"));
    messageBox.setText(
        translated("minidump.dialog.ksword_related.text", "这次崩溃与 KSword 有关"));
    messageBox.setInformativeText(
        ks::minidump::BuildKswordReportGuidance(relevance, result.filePath));

    QPushButton* const qqButton = messageBox.addButton(
        translated("minidump.dialog.ksword_related.qq", "加入 QQ 群反馈"),
        QMessageBox::ActionRole);
    QPushButton* const issueButton = messageBox.addButton(
        translated("minidump.dialog.ksword_related.issue", "打开 GitHub Issues"),
        QMessageBox::ActionRole);
    QPushButton* const exportButton = messageBox.addButton(
        translated("minidump.dialog.ksword_related.export", "先导出报告"),
        QMessageBox::ActionRole);
    messageBox.addButton(
        translated("minidump.dialog.ksword_related.close", "知道了"),
        QMessageBox::RejectRole);
    messageBox.exec();

    // 三个动作按钮都不关闭"下次还提示"的开关：与 KSword 有关的崩溃每次都值得提醒。
    if (messageBox.clickedButton() == qqButton)
    {
        QDesktopServices::openUrl(QUrl(ks::minidump::KswordQqGroupUrl()));
    }
    else if (messageBox.clickedButton() == issueButton)
    {
        QDesktopServices::openUrl(QUrl(ks::minidump::KswordIssuesUrl()));
    }
    else if (messageBox.clickedButton() == exportButton)
    {
        exportReport();
    }
}

void MinidumpDock::requestReportDocument()
{
    if (!m_lastResult) return;
    const std::shared_ptr<const ks::minidump::DumpParseResult> snapshot = m_lastResult;
    if (m_cachedReportSource == snapshot && m_cachedReportDocument)
    {
        m_reportEditor->setDocument(*m_cachedReportDocument);
        m_reportEditor->setEnabled(true);
        return;
    }
    if (m_reportBuildOperation == nullptr)
    {
        m_reportBuildOperation = std::make_unique<ks::ui::AsyncOperation>(this);
        m_reportBuildOperation->setStateChangedCallback([this]()
            {
                if (m_reportBuildOperation->isBusy() ||
                    (m_cachedReportSource == m_lastResult && m_cachedReportDocument)) return;
                // A dropped GUI delivery must not leave the report disabled indefinitely.
                m_projectedResultPages.remove(m_reportEditor);
                m_reportEditor->setEnabled(true);
                if (m_resultTabs->currentWidget() == m_reportEditor &&
                    m_statusKey == QStringLiteral("minidump.status.projecting"))
                    setStatus("minidump.status.report_failed", "报告视图生成失败：%1；重新打开报告页可重试。",
                        {QString::number(ERROR_OPERATION_ABORTED)});
            });
    }
    m_reportEditor->setDocument({});
    m_reportEditor->setEnabled(false);
    if (m_statusKey != QStringLiteral("minidump.status.projecting") &&
        m_statusKey != QStringLiteral("minidump.status.report_failed"))
    {
        m_projectionPreviousStatusKey = m_statusKey;
        m_projectionPreviousStatusFallback = m_statusFallback;
        m_projectionPreviousStatusArguments = m_statusArguments;
    }
    setStatus("minidump.status.projecting", "正在更新结果视图：%1 / %2", {QStringLiteral("0"), QStringLiteral("1")});
    using Source = std::shared_ptr<const ks::minidump::DumpParseResult>;
    using Document = std::shared_ptr<const ks::ui::FieldDocument>;
    m_reportBuildOperation->submit<Source, Document>(snapshot,
        [](const Source& source, const ks::ui::AsyncOperationToken& token)
        {
            if (token.isCanceled()) return Document{};
            return Document(new ks::ui::FieldDocument(buildReportText(*source)),
                [](const ks::ui::FieldDocument* document)
                {
                    // A superseded large value document is never recursively freed on GUI.
                    auto* release = QRunnable::create([document]() { delete document; });
                    release->setAutoDelete(true);
                    QThreadPool::globalInstance()->start(release, -1);
                });
        },
        [this, snapshot](const Document& document, std::uint64_t)
        {
            if (m_lastResult != snapshot || !document) return true;
            m_cachedReportSource = snapshot;
            m_cachedReportDocument = document;
            const QPointer<MinidumpDock> guardThis(this);
            m_reportEditor->setDocument(*document);
            if (guardThis == nullptr) return true;
            m_reportEditor->setEnabled(true);
            if (m_resultTabs->currentWidget() == m_reportEditor) restoreResultProjectionStatus();
            return true;
        },
        [this, snapshot](std::exception_ptr failure)
        {
            if (m_lastResult != snapshot) return;
            m_projectedResultPages.remove(m_reportEditor);
            m_reportEditor->setEnabled(true);
            if (m_resultTabs->currentWidget() != m_reportEditor) return;
            QString detail = QString::number(ERROR_UNHANDLED_EXCEPTION);
            try { if (failure) std::rethrow_exception(failure); }
            catch (const std::exception& error) { detail = QString::fromUtf8(error.what()); }
            catch (...) {}
            setStatus("minidump.status.report_failed", "报告视图生成失败：%1；重新打开报告页可重试。", {detail});
        });
}

MinidumpExportResult MinidumpDock::exportReportSnapshot(const MinidumpExportRequest& request)
{
    MinidumpExportResult result;
    result.savePath = request.savePath;
    const QByteArray bytes = request.document
        ? request.document->toPlainText(request.translate).toUtf8()
        : buildReportText(*request.result).toPlainText(request.translate).toUtf8();
    QSaveFile file(request.savePath);
    result.ok = writeExportBytes(file, bytes, result.errorText);
    return result;
}

void MinidumpDock::exportReport()
{
    if (!m_lastResult || !m_lastResult->success ||
        (m_exportOperation != nullptr && m_exportOperation->isBusy())) return;
    // Bind the accepted export to the originally chosen result, even across a modal dialog.
    const auto snapshot = m_lastResult;
    const QFileInfo dumpInfo(snapshot->filePath);
    const QString suggestedName = dumpInfo.completeBaseName() + QStringLiteral("_report.txt");
    const QPointer<MinidumpDock> guardThis(this);
    const QString savePath = QFileDialog::getSaveFileName(this,
        translated("minidump.dialog.export_report", "导出解析报告"),
        QDir::toNativeSeparators(dumpInfo.absolutePath() + QStringLiteral("/") + suggestedName),
        translated("minidump.dialog.report_filter", "文本文件 (*.txt);;所有文件 (*.*)"));
    if (guardThis == nullptr || savePath.isEmpty()) return;
    if (m_exportOperation == nullptr)
    {
        m_exportOperation = std::make_unique<ks::ui::AsyncOperation>(this);
        m_exportOperation->setStateChangedCallback([this]()
            {
                m_exportButton->setEnabled(!m_parseBusy && m_lastResult && m_lastResult->success &&
                    !m_exportOperation->isBusy());
            });
    }
    const auto translator = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
    MinidumpExportRequest request;
    request.result = snapshot;
    if (m_cachedReportSource == snapshot) request.document = m_cachedReportDocument;
    request.savePath = savePath;
    request.translate = [translator](const QString& source) { return translator.sourceText(source); };
    const auto ticket = m_exportOperation->submitTransaction<MinidumpExportRequest, MinidumpExportResult>(
        std::move(request),
        [](const MinidumpExportRequest& value) { return exportReportSnapshot(value); },
        [this, snapshot](const MinidumpExportResult& result, std::uint64_t)
        {
            if (!result.ok)
            {
                QMessageBox::warning(this,
                    translated("minidump.dialog.export_failed.title", "导出失败"),
                    translated("minidump.dialog.export_failed.body", "无法写入目标文件：%1")
                        .arg(QDir::toNativeSeparators(result.savePath) + QLatin1Char('\n') + result.errorText));
            }
            else if (!m_parseBusy && m_lastResult == snapshot)
            {
                setStatus("minidump.status.exported", "报告已导出：%1",
                    {QDir::toNativeSeparators(result.savePath)});
            }
            else
            {
                // Do not replace the state of a newer parse with an older export completion.
                QMessageBox::information(this,
                    translated("minidump.action.export", "导出报告"),
                    translated("minidump.status.exported", "报告已导出：%1")
                        .arg(QDir::toNativeSeparators(result.savePath)));
            }
            return true;
        },
        [this, savePath](std::exception_ptr failure)
        {
            QString detail = QString::number(ERROR_UNHANDLED_EXCEPTION);
            try { if (failure) std::rethrow_exception(failure); }
            catch (const std::exception& error) { detail = QString::fromUtf8(error.what()); }
            catch (...) {}
            QMessageBox::warning(this,
                translated("minidump.dialog.export_failed.title", "导出失败"),
                translated("minidump.dialog.export_failed.body", "无法写入目标文件：%1")
                    .arg(QDir::toNativeSeparators(savePath) + QLatin1Char('\n') + detail));
        });
    if (ticket == 0)
    {
        QMessageBox::warning(this,
            translated("minidump.dialog.export_failed.title", "导出失败"),
            translated("minidump.dialog.export_failed.body", "无法写入目标文件：%1")
                .arg(QDir::toNativeSeparators(savePath) + QLatin1Char('\n') +
                    QString::number(ERROR_OPERATION_ABORTED)));
    }
}

void MinidumpDock::setStatus(
    const char* key,
    const char* fallback,
    const QStringList& arguments)
{
    // 记录键与参数：语言切换时 refreshStatus 按新语言重新渲染。
    m_statusKey = QString::fromLatin1(key);
    m_statusFallback = QString::fromUtf8(fallback);
    m_statusArguments = arguments;
    refreshStatus();
}

void MinidumpDock::refreshStatus()
{
    if (m_statusLabel == nullptr || m_statusKey.isEmpty())
    {
        return;
    }
    // text：先取词条再依次代入参数；参数本身保持原样（路径等动态内容）。
    QString text = ks::i18n::text(m_statusKey, m_statusFallback);
    for (const QString& argument : m_statusArguments)
    {
        text = text.arg(argument);
    }
    m_statusLabel->setText(text);
}

void MinidumpDock::setBusy(const bool busy)
{
    m_parseBusy = busy;
    // 解析期间冻结全部入口按钮，防止并发解析同一控件状态。
    m_parseButton->setEnabled(!busy);
    m_browseButton->setEnabled(!busy);
    m_systemDirButton->setEnabled(!busy);
    m_exportButton->setEnabled(!busy && m_lastResult && m_lastResult->success &&
        (m_exportOperation == nullptr || !m_exportOperation->isBusy()));
}
