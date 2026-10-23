#include "ProcessDetailWindow.InternalCommon.h"
#include "../UI/DetailDialogChrome.h"
#include "../UI/StructuredFieldView.h"
#include "../UI/ImmutableValueTable.h"

#include "../ksword/process/dll_hijack_detector.h"

#include <QBrush>

using namespace process_detail_window_internal;

namespace
{
    thread_local const ks::i18n::SourceTextSnapshot* reportTranslator = nullptr;
    struct ReportLanguageScope
    {
        const ks::i18n::SourceTextSnapshot* previous;
        explicit ReportLanguageScope(const ks::i18n::SourceTextSnapshot& translator)
            : previous(reportTranslator) { reportTranslator = &translator; }
        ~ReportLanguageScope() { reportTranslator = previous; }
    };
    QString dllHijackText(const char* const key, const QString& sourceText)
    {
        static_cast<void>(key);
        return reportTranslator ? reportTranslator->sourceText(sourceText) : ks::i18n::sourceText(sourceText);
    }

    int riskRank(const ks::process::DllHijackRisk risk)
    {
        switch (risk)
        {
        case ks::process::DllHijackRisk::High:
            return 3;
        case ks::process::DllHijackRisk::Suspicious:
            return 2;
        case ks::process::DllHijackRisk::Informational:
            return 1;
        case ks::process::DllHijackRisk::Safe:
        default:
            return 0;
        }
    }

    QString riskText(const ks::process::DllHijackRisk risk)
    {
        switch (risk)
        {
        case ks::process::DllHijackRisk::High:
            return dllHijackText(
                "process.detail.dll_hijack.risk.high",
                QStringLiteral("高风险"));
        case ks::process::DllHijackRisk::Suspicious:
            return dllHijackText(
                "process.detail.dll_hijack.risk.suspicious",
                QStringLiteral("可疑"));
        case ks::process::DllHijackRisk::Informational:
            return dllHijackText(
                "process.detail.dll_hijack.risk.informational",
                QStringLiteral("提示"));
        case ks::process::DllHijackRisk::Safe:
        default:
            return dllHijackText(
                "process.detail.dll_hijack.risk.safe",
                QStringLiteral("一致"));
        }
    }

    QColor riskColor(const ks::process::DllHijackRisk risk)
    {
        switch (risk)
        {
        case ks::process::DllHijackRisk::High:
            return KswordTheme::ErrorColor();
        case ks::process::DllHijackRisk::Suspicious:
            return KswordTheme::WarningColor();
        case ks::process::DllHijackRisk::Safe:
            return KswordTheme::SuccessColor();
        case ks::process::DllHijackRisk::Informational:
        default:
            return KswordTheme::TextSecondaryColor();
        }
    }

    QString presenceText(const ks::process::DllHijackPresence presence)
    {
        return presence == ks::process::DllHijackPresence::Loaded
            ? dllHijackText(
                "process.detail.dll_hijack.presence.loaded",
                QStringLiteral("已实际加载"))
            : dllHijackText(
                "process.detail.dll_hijack.presence.present_only",
                QStringLiteral("仅程序目录存在"));
    }

    QString trustSourceText(const ks::process::DllHijackTrustSource source)
    {
        switch (source)
        {
        case ks::process::DllHijackTrustSource::Embedded:
            return dllHijackText(
                "process.detail.dll_hijack.trust.embedded",
                QStringLiteral("嵌入签名"));
        case ks::process::DllHijackTrustSource::Catalog:
            return dllHijackText(
                "process.detail.dll_hijack.trust.catalog",
                QStringLiteral("目录签名"));
        case ks::process::DllHijackTrustSource::None:
        default:
            return dllHijackText(
                "process.detail.dll_hijack.trust.none",
                QStringLiteral("未建立信任"));
        }
    }

    QString trustText(const ks::process::DllFileEvidence& evidence)
    {
        if (!evidence.trusted)
        {
            return dllHijackText(
                "process.detail.dll_hijack.trust.untrusted",
                QStringLiteral("不可信或未签名"));
        }
        const QString signer = evidence.signer.isEmpty()
            ? dllHijackText(
                "process.detail.dll_hijack.trust.signer_unknown",
                QStringLiteral("签名者未知"))
            : evidence.signer;
        return dllHijackText(
            "process.detail.dll_hijack.trust.valid",
            QStringLiteral("可信（%1，%2）"))
            .arg(trustSourceText(evidence.trustSource), signer);
    }

    QString machineText(const std::uint16_t machine)
    {
        if (machine == 0U)
        {
            return dllHijackText(
                "process.detail.dll_hijack.machine.unknown",
                QStringLiteral("未知"));
        }
        return QStringLiteral("0x%1")
            .arg(machine, 4, 16, QChar('0'))
            .toUpper();
    }

    QString differenceText(const ks::process::DllHijackFinding& finding)
    {
        QStringList evidence;
        if (!finding.hashComparable)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.hash_unavailable",
                QStringLiteral("SHA-256 无法比较")));
        }
        else if (finding.hashesMatch)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.identical",
                QStringLiteral("字节完全一致")));
        }
        else
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.hash_differs",
                QStringLiteral("SHA-256 不同")));
        }
        if (!finding.localFile.trusted)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.local_untrusted",
                QStringLiteral("本地签名不可信")));
        }
        if (finding.signerComparable && !finding.signersMatch)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.signer_differs",
                QStringLiteral("签名者不同")));
        }
        if (finding.companyComparable && !finding.companiesMatch)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.company_differs",
                QStringLiteral("公司信息不同")));
        }
        if (finding.originalFilenameComparable &&
            !finding.originalFilenamesMatch)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.original_name_differs",
                QStringLiteral("原始文件名不同")));
        }
        if (finding.versionComparable && !finding.versionsMatch)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.version_differs",
                QStringLiteral("文件版本不同")));
        }
        if (!finding.machineCompatible)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.machine_mismatch",
                QStringLiteral("体系结构不兼容")));
        }
        if (finding.localFile.reparsePoint)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.reparse_point",
                QStringLiteral("本地路径是重解析点")));
        }
        if (finding.knownDll)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.known_dll",
                QStringLiteral("命中 KnownDLL")));
        }
        if (finding.dllRedirectionPresent)
        {
            evidence.push_back(dllHijackText(
                "process.detail.dll_hijack.evidence.dot_local",
                QStringLiteral("存在 .local 重定向")));
        }
        return evidence.join(dllHijackText(
            "process.detail.dll_hijack.evidence.separator",
            QStringLiteral("；")));
    }

    ks::ui::FieldDocument findingDocument(const ks::process::DllHijackFinding& finding)
    {
        ks::ui::FieldDocument document;
        document.section(QStringLiteral("DLL 劫持检测"));
        document.field(QStringLiteral("风险"), riskText(finding.risk), true);
        document.field(QStringLiteral("加载状态"), presenceText(finding.presence), true);
        document.field(QStringLiteral("差异证据"), differenceText(finding), true);
        const auto appendFile = [&](const QString& title, const ks::process::DllFileEvidence& file) {
            document.section(title);
            document.field(QStringLiteral("路径"), file.path);
            document.field(QStringLiteral("签名"), trustText(file), true);
            document.field(QStringLiteral("SHA-256"), file.sha256);
            document.field(QStringLiteral("版本"), file.fileVersion);
            document.field(QStringLiteral("公司"), file.companyName);
            document.field(QStringLiteral("原始文件名"), file.originalFilename);
            document.field(QStringLiteral("机器"), machineText(file.machine), true);
        };
        appendFile(QStringLiteral("程序目录 DLL"), finding.localFile);
        appendFile(QStringLiteral("系统签名基线"), finding.systemFile);
        return document;
    }

    QString scanFailureText(const ks::process::DllHijackScanResult& result)
    {
        switch (result.status)
        {
        case ks::process::DllHijackScanStatus::ProcessIdentityUnavailable:
            return dllHijackText(
                "process.detail.dll_hijack.failure.identity_unavailable",
                QStringLiteral("无法验证当前进程实例，检测已安全停止。"));
        case ks::process::DllHijackScanStatus::ProcessIdentityMismatch:
            return dllHijackText(
                "process.detail.dll_hijack.failure.identity_mismatch",
                QStringLiteral("PID 已被复用或进程实例已变化，检测结果已丢弃。"));
        case ks::process::DllHijackScanStatus::ImagePathUnavailable:
            return dllHijackText(
                "process.detail.dll_hijack.failure.image_path",
                QStringLiteral("无法读取目标进程映像路径。"));
        case ks::process::DllHijackScanStatus::ApplicationDirectoryUnavailable:
            return dllHijackText(
                "process.detail.dll_hijack.failure.application_directory",
                QStringLiteral("目标程序目录不可访问。"));
        case ks::process::DllHijackScanStatus::SystemDirectoryUnavailable:
            return dllHijackText(
                "process.detail.dll_hijack.failure.system_directory",
                QStringLiteral("无法定位架构匹配的 Windows 系统目录。"));
        case ks::process::DllHijackScanStatus::Complete:
        default:
            return QString();
        }
    }

    ks::ui::FieldDocument buildScanDocument(
        const ks::process::DllHijackScanResult& result, const QString& processName, const std::uint32_t pid)
    {
        ks::ui::FieldDocument document;
        document.section(QStringLiteral("KSword DLL 劫持检测报告"));
        document.field(QStringLiteral("进程"), processName);
        document.field(QStringLiteral("PID"), QString::number(pid));
        document.field(QStringLiteral("映像"), result.processImagePath);
        document.field(QStringLiteral("程序目录"), result.applicationDirectory);
        document.field(QStringLiteral("系统基线目录"), result.systemDirectory);
        document.note(dllHijackText("process.detail.dll_hijack.report.boundary", QStringLiteral("结论边界：同名本身不是恶意结论；风险由实际加载状态、Windows 信任链、SHA-256、版本身份和路径证据共同决定。检测全程不会加载待检 DLL。")));
        const QString failure = scanFailureText(result);
        if (!failure.isEmpty())
        { document.note(failure); if (!result.diagnosticText.isEmpty()) document.field(QStringLiteral("技术信息"), result.diagnosticText); return document; }
        document.field(QStringLiteral("程序目录 DLL"), QString::number(result.scannedApplicationDllCount));
        document.field(QStringLiteral("系统同名"), QString::number(result.systemNameCollisionCount));
        document.field(QStringLiteral("签名基线"), QString::number(result.signedSystemBaselineCount));
        document.field(QStringLiteral("结果"), QString::number(result.findings.size()));
        for (const auto& finding : result.findings) appendFields(document, findingDocument(finding));
        return document;
    }

    struct DllReportValues
    {
        ks::process::DllHijackScanResult result;
        QVector<ks::ui::FieldDocument> details;
        QString reportText;
        int high = 0, suspicious = 0, informational = 0, safe = 0;
    };
    std::shared_ptr<DllReportValues> makeDllReportValues()
    {
        return {new DllReportValues, [](DllReportValues* value) noexcept
        {
            try
            {
                auto retired = std::unique_ptr<DllReportValues>(value);
                auto task = std::unique_ptr<QRunnable>(QRunnable::create([retired = std::move(retired)] {}));
                QThreadPool::globalInstance()->start(task.get()); (void)task.release();
            }
            catch (...) { /* RAII owns enqueue failure. */ }
        }};
    }

    void showDllHijackResultDialog(
        QWidget* const parent,
        const std::shared_ptr<const DllReportValues>& prepared,
        const QString& processName,
        const std::uint32_t pid)
    {
        const auto& result = prepared->result;
        QDialog dialog(parent);
        dialog.setWindowTitle(dllHijackText(
            "process.detail.dll_hijack.dialog.title",
            QStringLiteral("DLL 劫持检测 - %1"))
            .arg(processName));
        dialog.resize(1180, 720);

        QVBoxLayout* const layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        const int highCount = prepared->high;
        const int suspiciousCount = prepared->suspicious;
        const int informationalCount = prepared->informational;
        const int safeCount = prepared->safe;
        QString summaryText;
        const QString failure = scanFailureText(result);
        if (!failure.isEmpty())
        {
            summaryText = failure;
            if (!result.diagnosticText.isEmpty())
            {
                summaryText += QChar('\n') + dllHijackText(
                    "process.detail.dll_hijack.report.diagnostic",
                    QStringLiteral("技术信息：%1"))
                    .arg(result.diagnosticText);
            }
        }
        else
        {
            summaryText = dllHijackText(
                "process.detail.dll_hijack.dialog.summary",
                QStringLiteral("扫描 %1 个程序目录 DLL，命中 %2 个系统同名文件；高风险 %3，可疑 %4，提示 %5，一致 %6。"))
                .arg(result.scannedApplicationDllCount)
                .arg(result.systemNameCollisionCount)
                .arg(highCount)
                .arg(suspiciousCount)
                .arg(informationalCount)
                .arg(safeCount);
            if (!result.loadedModuleEvidenceAvailable)
            {
                summaryText += QChar('\n') + dllHijackText(
                    "process.detail.dll_hijack.dialog.loaded_unavailable",
                    QStringLiteral("当前无法取得模块快照；结果只包含落盘候选，未声称 DLL 已被加载。"));
            }
            if (result.directoryEnumerationTruncated)
            {
                summaryText += QChar('\n') + dllHijackText(
                    "process.detail.dll_hijack.dialog.truncated",
                    QStringLiteral("程序目录 DLL 数量超过安全上限，本次结果已截断。"));
            }
            if (result.dllRedirectionPresent)
            {
                summaryText += QChar('\n') + dllHijackText(
                    "process.detail.dll_hijack.dialog.dot_local",
                    QStringLiteral("检测到与进程映像同名的 .local 重定向标记，程序目录优先级证据已计入风险。"));
            }
        }

        QLabel* const summaryLabel = new QLabel(summaryText, &dialog);
        summaryLabel->setWordWrap(true);
        layout->addWidget(summaryLabel);

        QLabel* const boundaryLabel = new QLabel(
            dllHijackText(
                "process.detail.dll_hijack.dialog.boundary",
                QStringLiteral("同名 DLL 仅是候选：字节一致副本不会告警；合法私有版本会保留为提示。只有加载状态、签名/签名者、哈希、版本身份和路径证据组合后才提高风险。")),
            &dialog);
        boundaryLabel->setWordWrap(true);
        boundaryLabel->setStyleSheet(QStringLiteral("color:%1;")
            .arg(KswordTheme::TextSecondaryHex()));
        layout->addWidget(boundaryLabel);


        // 一次扫描报告仅需复制与导出，无实时冻结/快照链路。
        const QVector<ks::ui::ImmutableTableHeader> headers{
            dllHijackText("process.detail.dll_hijack.header.risk", QStringLiteral("风险")),
            dllHijackText("process.detail.dll_hijack.header.presence", QStringLiteral("加载状态")),
            dllHijackText("process.detail.dll_hijack.header.dll", QStringLiteral("DLL")),
            dllHijackText("process.detail.dll_hijack.header.local_trust", QStringLiteral("本地签名")),
            dllHijackText("process.detail.dll_hijack.header.system_trust", QStringLiteral("系统签名")),
            dllHijackText("process.detail.dll_hijack.header.hash", QStringLiteral("SHA-256")),
            dllHijackText("process.detail.dll_hijack.header.evidence", QStringLiteral("差异证据")),
            dllHijackText("process.detail.dll_hijack.header.local_path", QStringLiteral("程序目录路径")),
            dllHijackText("process.detail.dll_hijack.header.system_path", QStringLiteral("系统基线路径"))
        };
        auto* const table = new ks::ui::ImmutableValueTable(&dialog, headers);
        ks::ui::SetTableActionBarMode(table, ks::ui::TableActionBarMode::Compact);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setSortingEnabled(false);

        const QColor safeColor = riskColor(ks::process::DllHijackRisk::Safe);
        const QColor infoColor = riskColor(ks::process::DllHijackRisk::Informational);
        const QColor suspiciousColor = riskColor(ks::process::DllHijackRisk::Suspicious);
        const QColor highColor = riskColor(ks::process::DllHijackRisk::High);
        table->setSourceFactory([prepared, safeColor, infoColor, suspiciousColor, highColor](const ks::i18n::SourceTextSnapshot& translator)
        {
            const ReportLanguageScope language(translator);
            const auto& result = prepared->result;
            QVector<ks::ui::ImmutableTableRow> rows;
            rows.reserve(result.findings.size());
            for (int row = 0; row < result.findings.size(); ++row)
            {
                const auto& finding = result.findings.at(row);
            const QString hashState = !finding.hashComparable
                ? dllHijackText(
                    "process.detail.dll_hijack.hash.unavailable",
                    QStringLiteral("无法比较"))
                : finding.hashesMatch
                ? dllHijackText(
                    "process.detail.dll_hijack.hash.identical",
                    QStringLiteral("相同"))
                : dllHijackText(
                    "process.detail.dll_hijack.hash.different",
                    QStringLiteral("不同"));
            const QStringList cells{
                riskText(finding.risk),
                presenceText(finding.presence),
                QFileInfo(finding.localFile.path).fileName(),
                trustText(finding.localFile),
                trustText(finding.systemFile),
                hashState,
                differenceText(finding),
                finding.localFile.path,
                finding.systemFile.path
            };

                ks::ui::ImmutableTableRow value;
                value.key = finding.localFile.path + QLatin1Char(':') + QString::number(row);
                value.cells = cells;
                value.rolesInAllColumns = true;
                const QColor color = finding.risk == ks::process::DllHijackRisk::High ? highColor :
                    finding.risk == ks::process::DllHijackRisk::Suspicious ? suspiciousColor :
                    finding.risk == ks::process::DllHijackRisk::Informational ? infoColor : safeColor;
                value.roles.insert(Qt::ForegroundRole, QBrush(color));
                value.roles.insert(Qt::UserRole + 1, row);
                value.columnRoles[0].insert(Qt::UserRole, riskRank(finding.risk));
                const QString detail = prepared->details.at(row).toPlainText([&translator](const QString& text) { return translator.sourceText(text); });
                value.roles.insert(Qt::ToolTipRole, detail.left(8000));
                rows.push_back(std::move(value));
            }
            return ks::ui::MakeImmutableTableSource(std::move(rows));
        });
        table->horizontalHeader()->setStretchLastSection(false);
        table->setColumnWidth(0, 80);
        table->setColumnWidth(1, 115);
        table->setColumnWidth(2, 150);
        table->setColumnWidth(3, 210);
        table->setColumnWidth(4, 210);
        table->setColumnWidth(5, 85);
        table->setColumnWidth(6, 300);
        table->setColumnWidth(7, 360);
        table->setColumnWidth(8, 360);
        layout->addWidget(table, 1);

        auto* const detailPane = new ks::ui::StructuredFieldView(&dialog);
        detailPane->setMaximumHeight(180);
        if (!result.findings.isEmpty())
        {
            detailPane->setDocument(prepared->details.first());
        }
        else
        {
            detailPane->setDocument(fieldNotice(failure.isEmpty()
                ? dllHijackText(
                    "process.detail.dll_hijack.dialog.no_candidates",
                    QStringLiteral("未发现可与签名系统 DLL 建立基线的程序目录同名候选。"))
                : failure));
        }
        layout->addWidget(detailPane);

        table->currentRowChanged = [detailPane, table, prepared]
        {
            bool valid = false;
            const int sourceRow = table->currentCell(0, Qt::UserRole + 1).toInt(&valid);
            if (valid && sourceRow >= 0 && sourceRow < prepared->details.size())
                detailPane->setDocument(prepared->details.at(sourceRow));
        };
        table->sourceReady = [table] { if (table->rowCount() > 0) table->selectRow(0); };

        QHBoxLayout* const buttonLayout = new QHBoxLayout();
        buttonLayout->addStretch(1);
        QPushButton* const copyButton = new QPushButton(
            QIcon(":/Icon/process_copy_row.svg"),
            dllHijackText(
                "process.detail.dll_hijack.action.copy_report",
                QStringLiteral("复制报告")),
            &dialog);
        QPushButton* const openDirectoryButton = new QPushButton(
            QIcon(":/Icon/process_open_folder.svg"),
            dllHijackText(
                "process.detail.dll_hijack.action.open_directory",
                QStringLiteral("打开程序目录")),
            &dialog);
        QPushButton* const closeButton = new QPushButton(
            dllHijackText(
                "process.detail.dll_hijack.action.close",
                QStringLiteral("关闭")),
            &dialog);
        copyButton->setStyleSheet(buildBlueButtonStyle());
        openDirectoryButton->setStyleSheet(buildBlueButtonStyle());
        openDirectoryButton->setEnabled(
            QFileInfo(result.applicationDirectory).isDir());
        closeButton->setStyleSheet(buildBlueButtonStyle());
        buttonLayout->addWidget(copyButton);
        buttonLayout->addWidget(openDirectoryButton);
        buttonLayout->addWidget(closeButton);
        layout->addLayout(buttonLayout);

        QObject::connect(copyButton, &QPushButton::clicked, &dialog, [prepared]()
        {
            QApplication::clipboard()->setText(prepared->reportText);
        });
        QObject::connect(
            openDirectoryButton,
            &QPushButton::clicked,
            &dialog,
            [applicationDirectory = result.applicationDirectory]()
            {
                std::string detailText;
                ks::process::OpenFolderByPath(
                    applicationDirectory.toStdString(),
                    &detailText);
            });
        QObject::connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

        if (table->rowCount() > 0)
        {
            table->selectRow(0);
        }
        ks::ui::ApplyDetailDialogChrome(&dialog);
        dialog.exec();
    }
}

void ProcessDetailWindow::requestAsyncDllHijackScan()
{
    if (m_dllHijackScanRunning)
    {
        return;
    }

    m_dllHijackScanRunning = true;
    const std::uint64_t localTicket = ++m_dllHijackScanTicket;
    if (m_dllHijackScanButton != nullptr)
    {
        m_dllHijackScanButton->setEnabled(false);
    }
    updateModuleStatusLabel(
        dllHijackText(
            "process.detail.dll_hijack.status.scanning",
            QStringLiteral("● 正在只读检测 DLL 劫持候选...")),
        true);

    const std::uint32_t pid = m_baseRecord.pid;
    const std::uint64_t creationTime100ns = m_baseRecord.creationTime100ns;
    const QString fallbackImagePath = QString::fromStdString(m_baseRecord.imagePath);
    const QString processName = QString::fromStdString(m_baseRecord.processName);

    kLogEvent scanStartEvent;
    info << scanStartEvent
        << "[ProcessDetailWindow] DLL hijack scan start, pid="
        << pid
        << ", creationTime100ns="
        << creationTime100ns
        << eol;

    const auto translator = ks::i18n::LanguageManager::instance().sourceTextSnapshot();
    QPointer<ProcessDetailWindow> guard(this);
    QRunnable* const task = QRunnable::create([
        guard,
        localTicket,
        pid,
        creationTime100ns,
        fallbackImagePath,
        processName, translator]()
    {
        auto prepared = makeDllReportValues();
        prepared->result = ks::process::ScanProcessDllHijacking(
                pid,
                creationTime100ns,
                fallbackImagePath);
        const auto& result = prepared->result;
        const ReportLanguageScope language(translator);
        prepared->details.reserve(result.findings.size());
        for (const auto& finding : result.findings)
        {
            prepared->details.push_back(findingDocument(finding));
            switch (finding.risk)
            {
            case ks::process::DllHijackRisk::High: ++prepared->high; break;
            case ks::process::DllHijackRisk::Suspicious: ++prepared->suspicious; break;
            case ks::process::DllHijackRisk::Informational: ++prepared->informational; break;
            case ks::process::DllHijackRisk::Safe: ++prepared->safe; break;
            }
        }
        prepared->reportText = buildScanDocument(result, processName, pid).toPlainText(
            [&translator](const QString& text) { return translator.sourceText(text); });
        if (guard == nullptr)
        {
            return;
        }

        QMetaObject::invokeMethod(
            qApp,
            [guard, localTicket, pid, creationTime100ns, processName, prepared]()
            {
                const auto& result = prepared->result;
                if (guard == nullptr ||
                    localTicket != guard->m_dllHijackScanTicket || pid != guard->m_baseRecord.pid ||
                    creationTime100ns != guard->m_baseRecord.creationTime100ns)
                {
                    return;
                }

                guard->m_dllHijackScanRunning = false;
                if (guard->m_dllHijackScanButton != nullptr)
                {
                    guard->m_dllHijackScanButton->setEnabled(true);
                }

                const int highCount = prepared->high;
                const int suspiciousCount = prepared->suspicious;
                if (result.status == ks::process::DllHijackScanStatus::Complete)
                {
                    guard->updateModuleStatusLabel(
                        dllHijackText(
                            "process.detail.dll_hijack.status.completed",
                            QStringLiteral("● DLL 劫持检测完成：高风险 %1，可疑 %2，候选 %3"))
                            .arg(highCount)
                            .arg(suspiciousCount)
                            .arg(result.findings.size()),
                        false);
                }
                else
                {
                    guard->updateModuleStatusLabel(
                        dllHijackText(
                            "process.detail.dll_hijack.status.failed",
                            QStringLiteral("● DLL 劫持检测未完成")),
                        false);
                    if (guard->m_moduleStatusLabel != nullptr)
                    {
                        guard->m_moduleStatusLabel->setStyleSheet(
                            buildStateLabelStyle(statusErrorColor(), 700));
                    }
                }

                kLogEvent scanFinishEvent;
                info << scanFinishEvent
                    << "[ProcessDetailWindow] DLL hijack scan finish, pid="
                    << pid
                    << ", status="
                    << static_cast<int>(result.status)
                    << ", findings="
                    << result.findings.size()
                    << ", high="
                    << highCount
                    << ", suspicious="
                    << suspiciousCount
                    << eol;

                showDllHijackResultDialog(
                    guard,
                    prepared,
                    processName,
                    pid);
            },
            Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}
