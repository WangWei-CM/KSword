#include "ApplicationControlPage.h"

#include "../ksword/startup/startup.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QXmlStreamReader>

#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>

namespace
{
    // makeReadOnlyItem：
    // - 创建只读表格项；
    // - text 为单元格文本；
    // - 返回一个可直接塞进 QTableWidget 的 item。
    QTableWidgetItem* makeReadOnlyItem(const QString& text)
    {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        return item;
    }

    // sizeTextFromBytes：
    // - 把字节数格式化为人类可读文本；
    // - bytes 小于 0 时返回破折号。
    QString sizeTextFromBytes(const qint64 bytes)
    {
        if (bytes < 0)
        {
            return QStringLiteral("—");
        }

        const double value = static_cast<double>(bytes);
        if (value < 1024.0)
        {
            return QStringLiteral("%1 B").arg(bytes);
        }
        if (value < 1024.0 * 1024.0)
        {
            return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 2);
        }
        if (value < 1024.0 * 1024.0 * 1024.0)
        {
            return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 2);
        }
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }

    // dateTimeText：
    // - 把本地时间转成展示字符串；
    // - 无效时间返回破折号。
    QString dateTimeText(const QDateTime& dateTime)
    {
        return dateTime.isValid()
            ? dateTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("—");
    }

    // sidToFriendlyText：
    // - 把常见 SID 映射为更容易读的文本；
    // - 未知 SID 原样回退。
    QString sidToFriendlyText(const QString& sidText)
    {
        const QString trimmedSid = sidText.trimmed();
        if (trimmedSid == QStringLiteral("S-1-1-0")) return QStringLiteral("Everyone");
        if (trimmedSid == QStringLiteral("S-1-5-32-545")) return QStringLiteral("Users");
        if (trimmedSid == QStringLiteral("S-1-5-32-544")) return QStringLiteral("Administrators");
        if (trimmedSid == QStringLiteral("S-1-5-18")) return QStringLiteral("SYSTEM");
        if (trimmedSid == QStringLiteral("S-1-5-19")) return QStringLiteral("LOCAL SERVICE");
        if (trimmedSid == QStringLiteral("S-1-5-20")) return QStringLiteral("NETWORK SERVICE");
        return trimmedSid;
    }

    // pathLikeTextToRegex：
    // - 把带 * 和 ? 的路径通配转换为正则表达式；
    // - 用于 AppLocker 路径规则的可能命中判断。
    QRegularExpression pathLikeTextToRegex(QString text)
    {
        text = text.trimmed();
        text.replace(QStringLiteral("/"), QStringLiteral("\\"));
        text.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        text.replace(QStringLiteral("."), QStringLiteral("\\."));
        text.replace(QStringLiteral("+"), QStringLiteral("\\+"));
        text.replace(QStringLiteral("("), QStringLiteral("\\("));
        text.replace(QStringLiteral(")"), QStringLiteral("\\)"));
        text.replace(QStringLiteral("$"), QStringLiteral("\\$"));
        text.replace(QStringLiteral("^"), QStringLiteral("\\^"));
        text.replace(QStringLiteral("{"), QStringLiteral("\\{"));
        text.replace(QStringLiteral("}"), QStringLiteral("\\}"));
        text.replace(QStringLiteral("|"), QStringLiteral("\\|"));
        text.replace(QStringLiteral("*"), QStringLiteral(".*"));
        text.replace(QStringLiteral("?"), QStringLiteral("."));
        return QRegularExpression(QStringLiteral("^%1$").arg(text), QRegularExpression::CaseInsensitiveOption);
    }

    // expandCommonEnvironmentTokens：
    // - 展开 AppLocker 路径规则里常见的环境变量；
    // - 先做一轮常见变量替换，再留给正则通配匹配。
    QString expandCommonEnvironmentTokens(QString text)
    {
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const auto replaceToken = [&text, &env](const QString& token, const QString& envName) {
            const QString value = env.value(envName);
            if (!value.isEmpty())
            {
                text.replace(token, QDir::toNativeSeparators(value), Qt::CaseInsensitive);
            }
        };

        replaceToken(QStringLiteral("%WINDIR%"), QStringLiteral("WINDIR"));
        replaceToken(QStringLiteral("%SYSTEMROOT%"), QStringLiteral("SystemRoot"));
        replaceToken(QStringLiteral("%OSDRIVE%"), QStringLiteral("SystemDrive"));
        replaceToken(QStringLiteral("%PROGRAMFILES%"), QStringLiteral("ProgramFiles"));
        replaceToken(QStringLiteral("%PROGRAMFILES(X86)%"), QStringLiteral("ProgramFiles(x86)"));
        replaceToken(QStringLiteral("%USERPROFILE%"), QStringLiteral("USERPROFILE"));
        replaceToken(QStringLiteral("%LOCALAPPDATA%"), QStringLiteral("LOCALAPPDATA"));
        replaceToken(QStringLiteral("%APPDATA%"), QStringLiteral("APPDATA"));
        replaceToken(QStringLiteral("%TEMP%"), QStringLiteral("TEMP"));
        replaceToken(QStringLiteral("%TMP%"), QStringLiteral("TMP"));
        return text;
    }

    // isBroadPathRuleText：
    // - 判断路径规则是否过宽；
    // - 用于在 AppLocker 表格中标记风险。
    bool isBroadPathRuleText(const QString& conditionText)
    {
        const QString lower = conditionText.toLower();
        return conditionText.trimmed() == QStringLiteral("*")
            || lower.contains(QStringLiteral("\\users\\"))
            || lower.contains(QStringLiteral("\\downloads\\"))
            || lower.contains(QStringLiteral("\\desktop\\"))
            || lower.contains(QStringLiteral("\\temp\\"))
            || lower.contains(QStringLiteral("\\appdata\\local\\temp"))
            || lower.contains(QStringLiteral("\\programdata\\"))
            || lower.contains(QStringLiteral("%temp%"))
            || lower.contains(QStringLiteral("%userprofile%"))
            || lower.contains(QStringLiteral("%localappdata%"))
            || lower.contains(QStringLiteral("%appdata%"));
    }

    // collapseSpaces：
    // - 压缩文本中的多余空白；
    // - 便于表格展示。
    QString collapseSpaces(QString text)
    {
        text = text.simplified();
        return text;
    }

    // jsonValueToText：
    // - 将 JSON 值转换为展示文本；
    // - 对数组和对象做紧凑化回退，避免丢信息。
    QString jsonValueToText(const QJsonValue& value)
    {
        switch (value.type())
        {
        case QJsonValue::String:
            return value.toString();
        case QJsonValue::Double:
            return QString::number(value.toDouble());
        case QJsonValue::Bool:
            return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
        case QJsonValue::Array:
            return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
        case QJsonValue::Object:
            return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
        case QJsonValue::Null:
        case QJsonValue::Undefined:
        default:
            return QString();
        }
    }

    // classifyCodeIntegrityVerdict：
    // - 根据事件消息和级别做允许/阻止/审计粗分类；
    // - 仅做展示用途，不作为系统判定。
    QString classifyCodeIntegrityVerdict(const QString& messageText, const QString& levelText)
    {
        const QString lower = messageText.toLower();
        if (lower.contains(QStringLiteral("audit")) || lower.contains(QStringLiteral("审计")) || lower.contains(QStringLiteral("would have been blocked")))
        {
            return QStringLiteral("审计");
        }
        if (lower.contains(QStringLiteral("block")) || lower.contains(QStringLiteral("deny")) || lower.contains(QStringLiteral("阻止")) || lower.contains(QStringLiteral("not allowed")))
        {
            return QStringLiteral("阻止");
        }
        if (lower.contains(QStringLiteral("allow")) || lower.contains(QStringLiteral("loaded")) || lower.contains(QStringLiteral("允许")))
        {
            return QStringLiteral("允许");
        }
        if (levelText.contains(QStringLiteral("warning"), Qt::CaseInsensitive))
        {
            return QStringLiteral("审计");
        }
        return QStringLiteral("事件");
    }

    // collectElementSummary：
    // - 递归收集 XML 元素名称与属性摘要；
    // - reader 必须位于 StartElement 上；
    // - 返回格式类似 "FilePathCondition Path=C:\\*"。
    QString collectElementSummary(QXmlStreamReader& reader)
    {
        const QString elementName = reader.name().toString();
        QStringList fragments;
        const auto attributes = reader.attributes();
        for (const QXmlStreamAttribute& attribute : attributes)
        {
            fragments.push_back(QStringLiteral("%1=%2")
                .arg(attribute.name().toString(), attribute.value().toString()));
        }

        while (reader.readNextStartElement())
        {
            fragments.push_back(collectElementSummary(reader));
        }

        if (fragments.isEmpty())
        {
            return elementName;
        }
        return QStringLiteral("%1 %2").arg(elementName, fragments.join(QStringLiteral(" | ")));
    }

    // fillTable：
    // - 以纯文本二维数组重建 QTableWidget；
    // - headers 为列标题。
    void fillTable(QTableWidget* table, const QStringList& headers, const QVector<QStringList>& rows)
    {
        if (table == nullptr)
        {
            return;
        }

        const bool sortingEnabled = table->isSortingEnabled();
        table->setSortingEnabled(false);
        table->clear();
        table->setColumnCount(headers.size());
        table->setRowCount(rows.size());
        table->setHorizontalHeaderLabels(headers);

        for (int row = 0; row < rows.size(); ++row)
        {
            const QStringList& values = rows.at(row);
            for (int column = 0; column < headers.size(); ++column)
            {
                const QString cellText = column < values.size() ? values.at(column) : QString();
                table->setItem(row, column, makeReadOnlyItem(cellText));
            }
        }

        table->setSortingEnabled(sortingEnabled);
        if (table->horizontalHeader() != nullptr)
        {
            table->horizontalHeader()->setStretchLastSection(true);
        }
    }
}

namespace ks::misc
{
    ApplicationControlPage::ApplicationControlPage(QWidget* parent)
        : QWidget(parent)
    {
        initializeUi();
        refreshAsync();
    }

    void ApplicationControlPage::initializeUi()
    {
        m_rootLayout = new QVBoxLayout(this);
        m_rootLayout->setContentsMargins(0, 0, 0, 0);
        m_rootLayout->setSpacing(6);

        m_toolbarWidget = new QWidget(this);
        auto* toolbarLayout = new QHBoxLayout(m_toolbarWidget);
        toolbarLayout->setContentsMargins(0, 0, 0, 0);
        toolbarLayout->setSpacing(8);

        m_refreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新"), m_toolbarWidget);
        m_refreshButton->setToolTip(QStringLiteral("重新采集 AppLocker / WDAC / Defender / 事件日志"));
        m_exportButton = new QPushButton(QIcon(QStringLiteral(":/Icon/log_export.svg")), QStringLiteral("导出 TSV"), m_toolbarWidget);
        m_exportButton->setToolTip(QStringLiteral("导出当前页主表格为 TSV"));
        m_statusLabel = new QLabel(QStringLiteral("状态: 正在加载…"), m_toolbarWidget);
        m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        toolbarLayout->addWidget(m_refreshButton);
        toolbarLayout->addWidget(m_exportButton);
        toolbarLayout->addStretch(1);
        toolbarLayout->addWidget(m_statusLabel);
        m_rootLayout->addWidget(m_toolbarWidget, 0);

        m_tabWidget = new QTabWidget(this);
        m_rootLayout->addWidget(m_tabWidget, 1);

        m_appLockerPage = buildAppLockerPage();
        m_wdacPage = buildWdacPage();
        m_defenderPage = buildDefenderPage();
        m_eventPage = buildEventLogPage();
        m_fileDiagnosisPage = buildFileDiagnosisPage();

        m_tabWidget->addTab(m_appLockerPage, QIcon(QStringLiteral(":/Icon/process_details.svg")), QStringLiteral("AppLocker"));
        m_tabWidget->addTab(m_wdacPage, QIcon(QStringLiteral(":/Icon/disk_storage.svg")), QStringLiteral("WDAC / Code Integrity"));
        m_tabWidget->addTab(m_defenderPage, QIcon(QStringLiteral(":/Icon/process_details.svg")), QStringLiteral("Defender / ASR"));
        m_tabWidget->addTab(m_eventPage, QIcon(QStringLiteral(":/Icon/process_details.svg")), QStringLiteral("事件日志"));
        m_tabWidget->addTab(m_fileDiagnosisPage, QIcon(QStringLiteral(":/Icon/process_details.svg")), QStringLiteral("文件诊断"));

        connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshAsync(); });
        connect(m_exportButton, &QPushButton::clicked, this, [this]() { exportCurrentTableTsv(); });
    }

    QWidget* ApplicationControlPage::buildAppLockerPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        m_appLockerSummary = new QPlainTextEdit(page);
        m_appLockerSummary->setReadOnly(true);
        m_appLockerSummary->setPlaceholderText(QStringLiteral("AppLocker 摘要会在后台刷新后显示。"));
        m_appLockerSummary->setMaximumHeight(120);

        m_appLockerTable = new QTableWidget(page);
        initializeTable(m_appLockerTable, true);

        layout->addWidget(m_appLockerSummary, 0);
        layout->addWidget(m_appLockerTable, 1);
        return page;
    }

    QWidget* ApplicationControlPage::buildWdacPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        m_wdacSummary = new QPlainTextEdit(page);
        m_wdacSummary->setReadOnly(true);
        m_wdacSummary->setPlaceholderText(QStringLiteral("WDAC / Code Integrity 摘要会在后台刷新后显示。"));
        m_wdacSummary->setMaximumHeight(120);

        m_policyFileTable = new QTableWidget(page);
        initializeTable(m_policyFileTable, true);

        m_codeIntegrityEventTable = new QTableWidget(page);
        initializeTable(m_codeIntegrityEventTable, true);

        layout->addWidget(m_wdacSummary, 0);
        layout->addWidget(m_policyFileTable, 1);
        layout->addWidget(m_codeIntegrityEventTable, 1);
        return page;
    }

    QWidget* ApplicationControlPage::buildDefenderPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        m_defenderSummary = new QPlainTextEdit(page);
        m_defenderSummary->setReadOnly(true);
        m_defenderSummary->setPlaceholderText(QStringLiteral("Defender 状态会在后台刷新后显示。"));
        m_defenderSummary->setMaximumHeight(120);

        m_defenderTable = new QTableWidget(page);
        initializeTable(m_defenderTable, true);

        layout->addWidget(m_defenderSummary, 0);
        layout->addWidget(m_defenderTable, 1);
        return page;
    }

    QWidget* ApplicationControlPage::buildEventLogPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto* filterRow = new QWidget(page);
        auto* filterLayout = new QHBoxLayout(filterRow);
        filterLayout->setContentsMargins(0, 0, 0, 0);
        filterLayout->setSpacing(8);

        m_eventVerdictFilterCombo = new QComboBox(filterRow);
        m_eventVerdictFilterCombo->addItems({
            QStringLiteral("全部分类"),
            QStringLiteral("阻止"),
            QStringLiteral("审计"),
            QStringLiteral("允许"),
            QStringLiteral("事件"),
            QStringLiteral("读取失败")
            });
        m_eventVerdictFilterCombo->setToolTip(QStringLiteral("按 Code Integrity 判定分类筛选事件。"));

        m_eventLimitCombo = new QComboBox(filterRow);
        m_eventLimitCombo->addItems({
            QStringLiteral("最近 100 条"),
            QStringLiteral("最近 200 条"),
            QStringLiteral("最近 500 条"),
            QStringLiteral("最近 1000 条")
            });
        m_eventLimitCombo->setCurrentIndex(1);
        m_eventLimitCombo->setToolTip(QStringLiteral("控制本页从 Code Integrity 事件日志读取的最大事件数。"));

        filterLayout->addWidget(new QLabel(QStringLiteral("分类"), filterRow), 0);
        filterLayout->addWidget(m_eventVerdictFilterCombo, 0);
        filterLayout->addWidget(new QLabel(QStringLiteral("数量"), filterRow), 0);
        filterLayout->addWidget(m_eventLimitCombo, 0);
        filterLayout->addStretch(1);

        m_eventSummary = new QPlainTextEdit(page);
        m_eventSummary->setReadOnly(true);
        m_eventSummary->setPlaceholderText(QStringLiteral("Code Integrity 事件摘要会在后台刷新后显示。"));
        m_eventSummary->setMaximumHeight(88);

        m_eventTable = new QTableWidget(page);
        initializeTable(m_eventTable, true);
        m_eventTable->setMinimumHeight(220);

        layout->addWidget(filterRow, 0);
        layout->addWidget(m_eventSummary, 0);
        layout->addWidget(m_eventTable, 1);

        connect(m_eventVerdictFilterCombo, &QComboBox::currentTextChanged, this, [this]() {
            rebuildEventTable();
        });
        connect(m_eventLimitCombo, &QComboBox::currentTextChanged, this, [this]() {
            refreshAsync();
        });
        return page;
    }

    QWidget* ApplicationControlPage::buildFileDiagnosisPage()
    {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto* inputRow = new QWidget(page);
        auto* inputLayout = new QHBoxLayout(inputRow);
        inputLayout->setContentsMargins(0, 0, 0, 0);
        inputLayout->setSpacing(8);

        m_filePathEdit = new QLineEdit(inputRow);
        m_filePathEdit->setPlaceholderText(QStringLiteral("输入 exe / dll / script 文件路径"));
        m_fileBrowseButton = new QPushButton(QStringLiteral("浏览…"), inputRow);
        m_fileDiagnoseButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_details.svg")), QStringLiteral("诊断"), inputRow);

        inputLayout->addWidget(m_filePathEdit, 1);
        inputLayout->addWidget(m_fileBrowseButton);
        inputLayout->addWidget(m_fileDiagnoseButton);

        m_fileDiagnosisSummary = new QPlainTextEdit(page);
        m_fileDiagnosisSummary->setReadOnly(true);
        m_fileDiagnosisSummary->setPlaceholderText(QStringLiteral("文件诊断结果会在运行后显示。"));
        m_fileDiagnosisSummary->setMaximumHeight(140);

        m_fileDiagnosisTable = new QTableWidget(page);
        initializeTable(m_fileDiagnosisTable, true);

        layout->addWidget(inputRow, 0);
        layout->addWidget(m_fileDiagnosisSummary, 0);
        layout->addWidget(m_fileDiagnosisTable, 1);

        connect(m_fileBrowseButton, &QPushButton::clicked, this, [this]() {
            const QString filePath = QFileDialog::getOpenFileName(
                this,
                QStringLiteral("选择待诊断文件"),
                QString(),
                QStringLiteral("可执行文件 (*.exe *.dll *.sys *.scr *.cpl *.ocx *.msi *.ps1 *.vbs *.js *.cmd *.bat);;所有文件 (*.*)"));
            if (!filePath.isEmpty())
            {
                m_filePathEdit->setText(filePath);
            }
        });

        connect(m_fileDiagnoseButton, &QPushButton::clicked, this, [this]() { runFileDiagnosisAsync(); });
        return page;
    }

    void ApplicationControlPage::initializeTable(QTableWidget* table, const bool stretchLastColumn)
    {
        if (table == nullptr)
        {
            return;
        }

        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        table->setAlternatingRowColors(true);
        table->setSortingEnabled(true);
        table->horizontalHeader()->setStretchLastSection(stretchLastColumn);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        connect(table, &QTableWidget::customContextMenuRequested, this, [this, table](const QPoint& localPosition) {
            showTableContextMenu(table, localPosition);
        });
    }

    QTableWidget* ApplicationControlPage::currentExportTable() const
    {
        if (m_tabWidget == nullptr)
        {
            return nullptr;
        }

        switch (m_tabWidget->currentIndex())
        {
        case 0: return m_appLockerTable;
        case 1:
            if (m_codeIntegrityEventTable != nullptr
                && (m_codeIntegrityEventTable->hasFocus()
                    || (m_codeIntegrityEventTable->selectionModel() != nullptr
                        && !m_codeIntegrityEventTable->selectionModel()->selectedRows().isEmpty())))
            {
                return m_codeIntegrityEventTable;
            }
            return m_policyFileTable;
        case 2: return m_defenderTable;
        case 3: return m_eventTable;
        case 4: return m_fileDiagnosisTable;
        default: return nullptr;
        }
    }

    void ApplicationControlPage::showTableContextMenu(QTableWidget* table, const QPoint& localPosition)
    {
        if (table == nullptr)
        {
            return;
        }

        const QModelIndex index = table->indexAt(localPosition);
        if (index.isValid())
        {
            table->setCurrentIndex(index);
            if (table->selectionModel() != nullptr && !table->selectionModel()->isRowSelected(index.row(), QModelIndex()))
            {
                table->selectRow(index.row());
            }
        }

        QMenu menu(this);
        QAction* copyCellAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_copy.svg")), QStringLiteral("复制单元格"));
        QAction* copyRowAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_clipboard.svg")), QStringLiteral("复制整行"));
        QAction* copySelectedAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_clipboard.svg")), QStringLiteral("复制选中行"));
        menu.addSeparator();
        QAction* exportAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_export.svg")), QStringLiteral("导出 TSV"));

        QAction* selectedAction = menu.exec(table->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        const int row = index.isValid() ? index.row() : table->currentRow();
        const int column = index.isValid() ? index.column() : table->currentColumn();

        if (selectedAction == copyCellAction)
        {
            copyTableCell(table, row, column);
            return;
        }
        if (selectedAction == copyRowAction)
        {
            copyTableRow(table, row);
            return;
        }
        if (selectedAction == copySelectedAction)
        {
            copySelectedRows(table);
            return;
        }
        if (selectedAction == exportAction)
        {
            const QString tsvText = tableToTsv(table, true);
            const QString outputPath = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("导出 TSV"),
                QStringLiteral("application_control.tsv"),
                QStringLiteral("TSV 文件 (*.tsv)"));
            if (outputPath.isEmpty())
            {
                return;
            }

            QFile outputFile(outputPath);
            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            {
                QMessageBox::warning(this, QStringLiteral("导出 TSV"), QStringLiteral("无法写入：%1").arg(outputPath));
                return;
            }
            outputFile.write(tsvText.toUtf8());
            outputFile.close();
            QMessageBox::information(this, QStringLiteral("导出 TSV"), QStringLiteral("已导出：%1").arg(outputPath));
        }
    }

    void ApplicationControlPage::copyTableCell(QTableWidget* table, const int row, const int column) const
    {
        if (table == nullptr || row < 0 || column < 0)
        {
            return;
        }

        QTableWidgetItem* item = table->item(row, column);
        QApplication::clipboard()->setText(item != nullptr ? item->text() : QString());
    }

    void ApplicationControlPage::copyTableRow(QTableWidget* table, const int row) const
    {
        if (table == nullptr || row < 0)
        {
            return;
        }

        QStringList values;
        for (int column = 0; column < table->columnCount(); ++column)
        {
            QTableWidgetItem* item = table->item(row, column);
            values.push_back(item != nullptr ? item->text() : QString());
        }
        QApplication::clipboard()->setText(values.join(QStringLiteral("\t")));
    }

    void ApplicationControlPage::copySelectedRows(QTableWidget* table) const
    {
        if (table == nullptr || table->selectionModel() == nullptr)
        {
            return;
        }

        const QModelIndexList selectedRows = table->selectionModel()->selectedRows();
        if (selectedRows.isEmpty())
        {
            const int currentRow = table->currentRow();
            if (currentRow >= 0)
            {
                copyTableRow(table, currentRow);
            }
            return;
        }

        QStringList lines;
        lines.reserve(selectedRows.size());
        for (const QModelIndex& rowIndex : selectedRows)
        {
            QStringList values;
            for (int column = 0; column < table->columnCount(); ++column)
            {
                QTableWidgetItem* item = table->item(rowIndex.row(), column);
                values.push_back(item != nullptr ? item->text() : QString());
            }
            lines.push_back(values.join(QStringLiteral("\t")));
        }
        QApplication::clipboard()->setText(lines.join(QStringLiteral("\n")));
    }

    QString ApplicationControlPage::tableToTsv(QTableWidget* table, const bool selectedOnly) const
    {
        if (table == nullptr)
        {
            return QString();
        }

        QStringList lines;
        QStringList headerValues;
        for (int column = 0; column < table->columnCount(); ++column)
        {
            QTableWidgetItem* headerItem = table->horizontalHeaderItem(column);
            headerValues.push_back(headerItem != nullptr ? headerItem->text() : QStringLiteral("Column %1").arg(column + 1));
        }
        lines.push_back(headerValues.join(QStringLiteral("\t")));

        QVector<int> rowIndexes;
        if (selectedOnly && table->selectionModel() != nullptr)
        {
            const QModelIndexList selectedRows = table->selectionModel()->selectedRows();
            rowIndexes.reserve(selectedRows.size());
            for (const QModelIndex& index : selectedRows)
            {
                rowIndexes.push_back(index.row());
            }
        }
        else
        {
            rowIndexes.reserve(table->rowCount());
            for (int row = 0; row < table->rowCount(); ++row)
            {
                rowIndexes.push_back(row);
            }
        }

        for (const int row : rowIndexes)
        {
            QStringList values;
            values.reserve(table->columnCount());
            for (int column = 0; column < table->columnCount(); ++column)
            {
                QTableWidgetItem* item = table->item(row, column);
                values.push_back(item != nullptr ? item->text() : QString());
            }
            lines.push_back(values.join(QStringLiteral("\t")));
        }

        return lines.join(QStringLiteral("\n"));
    }

    void ApplicationControlPage::exportCurrentTableTsv()
    {
        QTableWidget* const table = currentExportTable();
        if (table == nullptr)
        {
            QMessageBox::information(this, QStringLiteral("导出 TSV"), QStringLiteral("当前页没有可导出的表格。"));
            return;
        }

        const QString outputPath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出 TSV"),
            QStringLiteral("application_control.tsv"),
            QStringLiteral("TSV 文件 (*.tsv)"));
        if (outputPath.isEmpty())
        {
            return;
        }

        QFile outputFile(outputPath);
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        {
            QMessageBox::warning(this, QStringLiteral("导出 TSV"), QStringLiteral("无法写入：%1").arg(outputPath));
            return;
        }

        outputFile.write(tableToTsv(table, false).toUtf8());
        outputFile.close();
        QMessageBox::information(this, QStringLiteral("导出 TSV"), QStringLiteral("已导出：%1").arg(outputPath));
    }

    QString ApplicationControlPage::buildAppLockerRiskText(
        const QString& actionText,
        const QString& sidText,
        const QString& conditionTypeText,
        const QString& conditionText)
    {
        QStringList riskList;
        const QString loweredSid = sidText.trimmed();
        if (actionText.compare(QStringLiteral("Allow"), Qt::CaseInsensitive) == 0
            && (loweredSid == QStringLiteral("S-1-1-0") || sidToFriendlyText(loweredSid) == QStringLiteral("Everyone")))
        {
            riskList.push_back(QStringLiteral("Everyone Allow"));
        }

        if (conditionTypeText.compare(QStringLiteral("Path"), Qt::CaseInsensitive) == 0)
        {
            if (isBroadPathRuleText(conditionText))
            {
                riskList.push_back(QStringLiteral("Users writable path"));
            }
            const QString lower = conditionText.toLower();
            if (conditionText.trimmed() == QStringLiteral("*")
                || lower == QStringLiteral("*")
                || lower.contains(QStringLiteral("\\*"))
                || lower.contains(QStringLiteral("*\\"))
                || lower.contains(QStringLiteral("*.*")))
            {
                riskList.push_back(QStringLiteral("宽泛 * 规则"));
            }
        }
        else if (conditionText.trimmed().contains(QStringLiteral("*")))
        {
            riskList.push_back(QStringLiteral("宽泛 * 规则"));
        }

        return riskList.isEmpty() ? QString() : riskList.join(QStringLiteral("; "));
    }

    QString ApplicationControlPage::runPowerShellCaptureText(
        const QString& scriptText,
        const int timeoutMs,
        QString* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        QProcess process;
        process.setProgram(QStringLiteral("powershell.exe"));
        process.setArguments(QStringList{
            QStringLiteral("-NoLogo"),
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            scriptText
        });
        process.start();
        if (!process.waitForStarted(5000))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("无法启动 powershell.exe。");
            }
            return QString();
        }

        if (!process.waitForFinished(timeoutMs))
        {
            process.kill();
            process.waitForFinished(2000);
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("PowerShell 查询超时。");
            }
            return QString();
        }

        const QString stdOutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        const QString stdErrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (errorTextOut != nullptr)
        {
            *errorTextOut = stdErrText;
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            {
                const QString exitHint = QStringLiteral("PowerShell 退出码 %1。").arg(process.exitCode());
                if (errorTextOut->isEmpty())
                {
                    *errorTextOut = exitHint;
                }
                else
                {
                    *errorTextOut += QStringLiteral("\n");
                    *errorTextOut += exitHint;
                }
            }
        }

        return stdOutText;
    }

    std::pair<QVector<ApplicationControlPage::AppLockerRuleRecord>, QString> ApplicationControlPage::parseAppLockerPolicyXml(
        const QString& xmlText)
    {
        QVector<AppLockerRuleRecord> records;
        if (xmlText.trimmed().isEmpty())
        {
            return { records, QStringLiteral("AppLocker: 未配置") };
        }

        QXmlStreamReader reader(xmlText);
        QStringList summaryParts;
        int collectionCount = 0;

        const auto collectionToText = [](const QString& typeText) {
            if (typeText.compare(QStringLiteral("Exe"), Qt::CaseInsensitive) == 0) return QStringLiteral("EXE");
            if (typeText.compare(QStringLiteral("Dll"), Qt::CaseInsensitive) == 0) return QStringLiteral("DLL");
            if (typeText.compare(QStringLiteral("Msi"), Qt::CaseInsensitive) == 0) return QStringLiteral("MSI");
            if (typeText.compare(QStringLiteral("Script"), Qt::CaseInsensitive) == 0) return QStringLiteral("Script");
            if (typeText.compare(QStringLiteral("Appx"), Qt::CaseInsensitive) == 0 || typeText.contains(QStringLiteral("Packaged"), Qt::CaseInsensitive))
            {
                return QStringLiteral("Packaged app");
            }
            return typeText.isEmpty() ? QStringLiteral("Unknown") : typeText;
        };

        const auto conditionTypeFromSummary = [](const QString& summaryText) {
            const QString lower = summaryText.toLower();
            if (lower.contains(QStringLiteral("publisher"))) return QStringLiteral("Publisher");
            if (lower.contains(QStringLiteral("path"))) return QStringLiteral("Path");
            if (lower.contains(QStringLiteral("hash"))) return QStringLiteral("Hash");
            return QStringLiteral("Unknown");
        };

        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement())
            {
                continue;
            }

            if (reader.name().toString().compare(QStringLiteral("RuleCollection"), Qt::CaseInsensitive) != 0)
            {
                reader.skipCurrentElement();
                continue;
            }

            ++collectionCount;
            const QString collectionTypeText = collectionToText(reader.attributes().value(QStringLiteral("Type")).toString());
            int collectionRuleCount = 0;

            auto parseRuleElement = [&](const QString& ruleElementName) {
                AppLockerRuleRecord record;
                record.collectionText = collectionTypeText;
                const QXmlStreamAttributes attributes = reader.attributes();
                record.actionText = attributes.value(QStringLiteral("Action")).toString();
                record.sidText = attributes.value(QStringLiteral("UserOrGroupSid")).toString();
                record.userText = sidToFriendlyText(record.sidText);
                record.descriptionText = attributes.value(QStringLiteral("Description")).toString();
                if (record.descriptionText.trimmed().isEmpty())
                {
                    record.descriptionText = attributes.value(QStringLiteral("Name")).toString();
                }
                if (record.actionText.trimmed().isEmpty())
                {
                    record.actionText = QStringLiteral("—");
                }
                if (record.sidText.trimmed().isEmpty())
                {
                    record.sidText = QStringLiteral("—");
                }
                if (record.userText.trimmed().isEmpty())
                {
                    record.userText = QStringLiteral("—");
                }
                if (record.descriptionText.trimmed().isEmpty())
                {
                    record.descriptionText = QStringLiteral("—");
                }

                QStringList conditionSummaries;
                while (reader.readNextStartElement())
                {
                    const QString childName = reader.name().toString();
                    if (childName.compare(QStringLiteral("Conditions"), Qt::CaseInsensitive) == 0)
                    {
                        while (reader.readNextStartElement())
                        {
                            conditionSummaries.push_back(collectElementSummary(reader));
                        }
                    }
                    else
                    {
                        conditionSummaries.push_back(collectElementSummary(reader));
                    }
                }

                if (!conditionSummaries.isEmpty())
                {
                    record.conditionText = collapseSpaces(conditionSummaries.join(QStringLiteral(" ; ")));
                    record.conditionTypeText = conditionTypeFromSummary(conditionSummaries.front());
                }
                else
                {
                    record.conditionTypeText = QStringLiteral("Unknown");
                    record.conditionText = QStringLiteral("—");
                }

                record.riskText = buildAppLockerRiskText(
                    record.actionText,
                    record.sidText,
                    record.conditionTypeText,
                    record.conditionText);
                records.push_back(record);
                ++collectionRuleCount;
                Q_UNUSED(ruleElementName);
            };

            while (reader.readNextStartElement())
            {
                const QString childName = reader.name().toString();
                if (childName.endsWith(QStringLiteral("Rule"), Qt::CaseInsensitive))
                {
                    parseRuleElement(childName);
                }
                else
                {
                    reader.skipCurrentElement();
                }
            }

            summaryParts.push_back(QStringLiteral("%1: %2 条规则").arg(collectionTypeText).arg(collectionRuleCount));
        }

        if (reader.hasError())
        {
            return { records, QStringLiteral("AppLocker XML 解析失败：%1").arg(reader.errorString()) };
        }

        if (records.isEmpty())
        {
            return { records, QStringLiteral("AppLocker: 未配置") };
        }

        QString summaryText = QStringLiteral("AppLocker 规则集共 %1 个，规则共 %2 条。")
            .arg(collectionCount)
            .arg(records.size());
        if (!summaryParts.isEmpty())
        {
            summaryText += QStringLiteral("\n");
            summaryText += summaryParts.join(QStringLiteral("\n"));
        }
        return { records, summaryText };
    }

    std::pair<QVector<ApplicationControlPage::EventRecord>, QString> ApplicationControlPage::parseEventsJson(
        const QString& jsonText)
    {
        QVector<EventRecord> records;
        const QString trimmedText = jsonText.trimmed();
        if (trimmedText.isEmpty())
        {
            return { records, QStringLiteral("未获取到 Code Integrity 事件。") };
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(trimmedText.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            return { records, QStringLiteral("事件 JSON 解析失败：%1").arg(parseError.errorString()) };
        }

        QJsonArray array;
        if (document.isArray())
        {
            array = document.array();
        }
        else if (document.isObject())
        {
            array.push_back(document.object());
        }

        int allowCount = 0;
        int blockCount = 0;
        int auditCount = 0;

        for (const QJsonValue& value : array)
        {
            const QJsonObject object = value.toObject();
            EventRecord record;
            QString timeText = jsonValueToText(object.value(QStringLiteral("TimeText")));
            if (timeText.isEmpty())
            {
                timeText = jsonValueToText(object.value(QStringLiteral("TimeCreated")));
            }
            record.timeText = collapseSpaces(timeText);

            record.idText = jsonValueToText(object.value(QStringLiteral("IdText")));
            if (record.idText.isEmpty())
            {
                record.idText = jsonValueToText(object.value(QStringLiteral("Id")));
            }

            record.levelText = jsonValueToText(object.value(QStringLiteral("LevelText")));
            if (record.levelText.isEmpty())
            {
                record.levelText = jsonValueToText(object.value(QStringLiteral("LevelDisplayName")));
            }

            record.messageText = jsonValueToText(object.value(QStringLiteral("MessageText")));
            if (record.messageText.isEmpty())
            {
                record.messageText = jsonValueToText(object.value(QStringLiteral("Message")));
            }
            record.messageText = collapseSpaces(record.messageText);
            {
                QStringList errorParts;
                const QString errorText = jsonValueToText(object.value(QStringLiteral("ErrorText"))).trimmed();
                const QString errorCategory = jsonValueToText(object.value(QStringLiteral("ErrorCategory"))).trimmed();
                const QString errorType = jsonValueToText(object.value(QStringLiteral("ErrorType"))).trimmed();
                const QString hresultText = jsonValueToText(object.value(QStringLiteral("HResult"))).trimmed();
                if (!errorText.isEmpty()) errorParts << QStringLiteral("ErrorId=%1").arg(errorText);
                if (!errorCategory.isEmpty()) errorParts << QStringLiteral("Category=%1").arg(errorCategory);
                if (!errorType.isEmpty()) errorParts << QStringLiteral("Type=%1").arg(errorType);
                if (!hresultText.isEmpty()) errorParts << QStringLiteral("HResult=%1").arg(hresultText);
                if (!errorParts.isEmpty())
                {
                    record.messageText = QStringLiteral("%1 | %2")
                        .arg(record.messageText)
                        .arg(errorParts.join(QStringLiteral(" | "))).trimmed();
                }
            }

            record.verdictText = jsonValueToText(object.value(QStringLiteral("VerdictText")));
            if (record.verdictText.isEmpty())
            {
                record.verdictText = jsonValueToText(object.value(QStringLiteral("Verdict")));
            }
            if (record.verdictText.trimmed().isEmpty())
            {
                record.verdictText = classifyCodeIntegrityVerdict(record.messageText, record.levelText);
            }

            if (record.verdictText == QStringLiteral("允许")) ++allowCount;
            else if (record.verdictText == QStringLiteral("阻止")) ++blockCount;
            else if (record.verdictText == QStringLiteral("审计")) ++auditCount;

            if (record.messageText.isEmpty())
            {
                record.messageText = QStringLiteral("—");
            }
            if (record.idText.isEmpty())
            {
                record.idText = QStringLiteral("—");
            }
            if (record.levelText.isEmpty())
            {
                record.levelText = QStringLiteral("—");
            }
            if (record.timeText.isEmpty())
            {
                record.timeText = QStringLiteral("—");
            }
            records.push_back(record);
        }

        if (records.size() == 1)
        {
            const EventRecord& onlyRecord = records.front();
            if (onlyRecord.timeText == QStringLiteral("—")
                && onlyRecord.idText == QStringLiteral("—")
                && onlyRecord.messageText.contains(QStringLiteral("error"), Qt::CaseInsensitive))
            {
                return { records, QStringLiteral("Code Integrity 事件读取失败：%1").arg(onlyRecord.messageText) };
            }
        }

        QString summaryText = QStringLiteral("最近 %1 条事件：允许 %2，阻止 %3，审计 %4。")
            .arg(records.size())
            .arg(allowCount)
            .arg(blockCount)
            .arg(auditCount);
        return { records, summaryText };
    }

    std::pair<QVector<ApplicationControlPage::KeyValueRecord>, QString> ApplicationControlPage::parseDefenderJson(
        const QString& jsonText)
    {
        QVector<KeyValueRecord> records;
        const QString trimmedText = jsonText.trimmed();
        if (trimmedText.isEmpty())
        {
            return { records, QStringLiteral("未获取到 Defender 数据。") };
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(trimmedText.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            return { records, QStringLiteral("Defender JSON 解析失败：%1").arg(parseError.errorString()) };
        }

        QJsonArray array;
        if (document.isArray())
        {
            array = document.array();
        }
        else if (document.isObject())
        {
            const QJsonObject rootObject = document.object();
            if (rootObject.value(QStringLiteral("Rows")).isArray())
            {
                array = rootObject.value(QStringLiteral("Rows")).toArray();
            }
            else
            {
                array.push_back(rootObject);
            }
        }

        for (const QJsonValue& value : array)
        {
            const QJsonObject object = value.toObject();
            KeyValueRecord record;
            record.nameText = jsonValueToText(object.value(QStringLiteral("Name")));
            record.valueText = jsonValueToText(object.value(QStringLiteral("Value")));
            record.detailText = jsonValueToText(object.value(QStringLiteral("Detail")));
            if (record.nameText.isEmpty())
            {
                record.nameText = jsonValueToText(object.value(QStringLiteral("Key")));
            }
            if (record.valueText.isEmpty())
            {
                record.valueText = QStringLiteral("—");
            }
            if (record.detailText.isEmpty())
            {
                record.detailText = QStringLiteral("—");
            }
            records.push_back(record);
        }

        if (records.size() == 1)
        {
            const KeyValueRecord& onlyRecord = records.front();
            if (onlyRecord.nameText.contains(QStringLiteral("query"), Qt::CaseInsensitive)
                && onlyRecord.valueText.contains(QStringLiteral("Failed"), Qt::CaseInsensitive))
            {
                QStringList detailParts;
                detailParts << QStringLiteral("Defender 模块不可用或读取失败");
                if (!onlyRecord.detailText.trimmed().isEmpty())
                {
                    detailParts << QStringLiteral("Detail=%1").arg(onlyRecord.detailText.trimmed());
                }
                if (!onlyRecord.valueText.trimmed().isEmpty())
                {
                    detailParts << QStringLiteral("Value=%1").arg(onlyRecord.valueText.trimmed());
                }
                if (!onlyRecord.nameText.trimmed().isEmpty())
                {
                    detailParts << QStringLiteral("Name=%1").arg(onlyRecord.nameText.trimmed());
                }
                return { records, detailParts.join(QStringLiteral(" | ")) };
            }
        }

        const QString summaryText = QStringLiteral("Defender 状态共 %1 条。").arg(records.size());
        return { records, summaryText };
    }

    void ApplicationControlPage::refreshAsync()
    {
        if (m_refreshButton != nullptr)
        {
            m_refreshButton->setEnabled(false);
        }
        if (m_exportButton != nullptr)
        {
            m_exportButton->setEnabled(false);
        }
        if (m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("状态: 正在刷新…"));
        }
        if (m_appLockerSummary != nullptr) m_appLockerSummary->setPlainText(QStringLiteral("正在采集 AppLocker…"));
        if (m_wdacSummary != nullptr) m_wdacSummary->setPlainText(QStringLiteral("正在采集 WDAC / Code Integrity…"));
        if (m_defenderSummary != nullptr) m_defenderSummary->setPlainText(QStringLiteral("正在采集 Defender…"));
        if (m_eventSummary != nullptr) m_eventSummary->setPlainText(QStringLiteral("正在采集事件日志…"));

        const int requestedEventLimit = selectedEventLimit();
        const QPointer<ApplicationControlPage> guardThis(this);
        std::thread([guardThis, requestedEventLimit]() {
            QVector<AppLockerRuleRecord> appLockerRules;
            QVector<PolicyFileRecord> policyFiles;
            QVector<EventRecord> events;
            QVector<KeyValueRecord> defenderRows;
            QString appLockerSummary = QStringLiteral("AppLocker: 未配置");
            QString wdacSummary = QStringLiteral("WDAC / Code Integrity: 未发现常见策略文件。");
            QString defenderSummary = QStringLiteral("Defender: 未获取到状态。");
            QString eventSummary = QStringLiteral("未获取到 Code Integrity 事件。");
            QString statusText = QStringLiteral("刷新完成");

            // 1) WDAC / Code Integrity 文件扫描由 C++ 直接完成，避免额外脚本依赖。
            const QFileInfo sipolicyFile(QStringLiteral("C:/Windows/System32/CodeIntegrity/SIPolicy.p7b"));
            const QFileInfo activeDir(QStringLiteral("C:/Windows/System32/CodeIntegrity/CiPolicies/Active"));
            const auto appendPolicyFile = [&policyFiles](const QString& pathText, const QFileInfo& fileInfo, const QString& detailText, const QString& countText) {
                PolicyFileRecord record;
                record.pathText = pathText;
                record.existsText = fileInfo.exists() ? QStringLiteral("Yes") : QStringLiteral("No");
                record.sizeText = fileInfo.exists() ? sizeTextFromBytes(fileInfo.size()) : QStringLiteral("—");
                record.modifiedText = fileInfo.exists() ? dateTimeText(fileInfo.lastModified()) : QStringLiteral("—");
                record.countText = countText;
                record.detailText = detailText;
                policyFiles.push_back(record);
            };

            appendPolicyFile(
                QStringLiteral("C:\\Windows\\System32\\CodeIntegrity\\SIPolicy.p7b"),
                sipolicyFile,
                QStringLiteral("主 SIPolicy 文件"),
                sipolicyFile.exists() ? QStringLiteral("1") : QStringLiteral("0"));

            int activeCount = 0;
            if (activeDir.exists())
            {
                const QFileInfoList activeFiles = QDir(activeDir.absoluteFilePath()).entryInfoList(
                    QStringList{ QStringLiteral("*.cip") },
                    QDir::Files | QDir::Readable,
                    QDir::Name);
                activeCount = activeFiles.size();
                for (const QFileInfo& fileInfo : activeFiles)
                {
                    appendPolicyFile(
                        fileInfo.absoluteFilePath(),
                        fileInfo,
                        QStringLiteral("Active 目录下的 CIP 策略"),
                        QStringLiteral("1"));
                }
            }
            else
            {
                appendPolicyFile(
                    QStringLiteral("C:\\Windows\\System32\\CodeIntegrity\\CiPolicies\\Active\\*.cip"),
                    QFileInfo(),
                    QStringLiteral("Active 目录不存在"),
                    QStringLiteral("0"));
            }

            const int policyFileCount = (sipolicyFile.exists() ? 1 : 0) + activeCount;
            wdacSummary = QStringLiteral(
                "WDAC / Code Integrity 常见策略文件数: %1\n"
                "- SIPolicy.p7b: %2\n"
                "- Active/*.cip: %3")
                .arg(policyFileCount)
                .arg(sipolicyFile.exists() ? QStringLiteral("存在") : QStringLiteral("未找到"))
                .arg(activeCount);

            // 2) AppLocker 通过 PowerShell 拉取有效策略 XML。
            const QString appLockerScript = QStringLiteral(
                "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
                "try {"
                "  $xml=[string](Get-AppLockerPolicy -Effective -Xml -ErrorAction Stop);"
                "  if([string]::IsNullOrWhiteSpace($xml)){Write-Output '__NO_POLICY__'; exit 0};"
                "  Write-Output '__OK__';"
                "  Write-Output $xml;"
                "} catch {"
                "  $msg=$_.Exception.Message;"
                "  if($msg -match 'No AppLocker policy|not configured|未配置'){Write-Output '__NO_POLICY__'}"
                "  else { Write-Output '__ERROR__'; Write-Output $msg }"
                "}");
            QString appLockerErrorText;
            const QString appLockerOutput = runPowerShellCaptureText(appLockerScript, 15000, &appLockerErrorText);
            QString appLockerXmlText;
            if (appLockerOutput.startsWith(QStringLiteral("__OK__")))
            {
                appLockerXmlText = appLockerOutput.section(QChar::LineFeed, 1);
            }
            else if (appLockerOutput.contains(QStringLiteral("__NO_POLICY__")))
            {
                appLockerSummary = QStringLiteral("AppLocker: 未配置");
            }
            else
            {
                const QString parseHint = appLockerErrorText.isEmpty() ? appLockerOutput : appLockerErrorText;
                appLockerSummary = QStringLiteral("AppLocker 读取失败。\n建议：以管理员身份运行，并确认 Application Identity (AppIDSvc) 服务可用。\n%1")
                    .arg(parseHint.isEmpty() ? QStringLiteral("未返回额外错误信息。") : parseHint);
            }

            if (!appLockerXmlText.trimmed().isEmpty())
            {
                const auto parsedAppLocker = parseAppLockerPolicyXml(appLockerXmlText);
                appLockerRules = parsedAppLocker.first;
                if (!parsedAppLocker.second.trimmed().isEmpty())
                {
                    appLockerSummary = parsedAppLocker.second;
                }
                if (appLockerRules.isEmpty())
                {
                    appLockerSummary = QStringLiteral("AppLocker: 未配置");
                }
            }

            // 3) Defender / ASR 通过 PowerShell 输出为 JSON 数组，便于 UI 直读。
            const QString defenderScript = QStringLiteral(
                "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
                "try {"
                "  $rows=@();"
                "  $pref=Get-MpPreference -ErrorAction Stop;"
                "  $status=Get-MpComputerStatus -ErrorAction Stop;"
                "  $rows += [pscustomobject]@{Name='Controlled Folder Access'; Value=($pref.EnableControlledFolderAccess); Detail='0=Off,1=Block,2=Audit'};"
                "  $rows += [pscustomobject]@{Name='PUA Protection'; Value=($pref.PUAProtection); Detail='0=Disabled,1=Enabled,2=Audit'};"
                "  $rows += [pscustomobject]@{Name='Network Protection'; Value=($pref.EnableNetworkProtection); Detail='0=Disabled,1=Block,2=Audit'};"
                "  if($status.PSObject.Properties['SmartScreenEnabled']) { $rows += [pscustomobject]@{Name='SmartScreen'; Value=($status.SmartScreenEnabled); Detail='Available from Get-MpComputerStatus'} }"
                "  $asrIds=$pref.AttackSurfaceReductionRules_Ids;"
                "  $asrActions=$pref.AttackSurfaceReductionRules_Actions;"
                "  $count=[Math]::Min(@($asrIds).Count,@($asrActions).Count);"
                "  for($i=0; $i -lt $count; $i++){ $rows += [pscustomobject]@{Name=('ASR '+$asrIds[$i]); Value=($asrActions[$i]); Detail='AttackSurfaceReduction rule'} }"
                "  $rows += [pscustomobject]@{Name='Real Time Protection'; Value=($status.RealTimeProtectionEnabled); Detail='Get-MpComputerStatus'};"
                "  $rows += [pscustomobject]@{Name='Tamper Protection'; Value=($status.IsTamperProtected); Detail='Get-MpComputerStatus'};"
                "  $rows | ConvertTo-Json -Depth 4"
                "} catch {"
                "  [pscustomobject]@{Name='Defender query'; Value='Failed'; Detail=(($_.Exception.Message),'FullyQualifiedErrorId='+$_.FullyQualifiedErrorId,'Category='+$_.CategoryInfo.Category,'ErrorType='+$_.Exception.GetType().FullName,('HResult=0x{0:X8}' -f ($_.Exception.HResult -band 0xFFFFFFFF)) -join ' | ')} | ConvertTo-Json -Depth 3"
                "}");
            QString defenderErrorText;
            const QString defenderJsonText = runPowerShellCaptureText(defenderScript, 15000, &defenderErrorText);
            if (!defenderJsonText.trimmed().isEmpty())
            {
                const auto parsedDefender = parseDefenderJson(defenderJsonText);
                defenderRows = parsedDefender.first;
                defenderSummary = parsedDefender.second;
                if (!defenderErrorText.trimmed().isEmpty())
                {
                    defenderSummary += QStringLiteral("\n%1").arg(defenderErrorText);
                }
            }
            else if (!defenderErrorText.trimmed().isEmpty())
            {
                defenderSummary = QStringLiteral("Defender 模块不可用或读取失败：%1").arg(defenderErrorText);
            }

            // 4) Code Integrity 事件同样通过 PowerShell 输出为 JSON 数组。
            const QString eventScript = QStringLiteral(
                "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
                "try {"
                "  $events = Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-CodeIntegrity/Operational'} -MaxEvents %1 -ErrorAction Stop;"
                "  $events | ForEach-Object {"
                "    $message = $_.Message;"
                "    $level = $_.LevelDisplayName;"
                "    $lower = if($message){ $message.ToLower() } else { '' };"
                "    $verdict = if($lower -match 'audit|审计|would have been blocked'){ '审计' } elseif($lower -match 'block|deny|阻止|not allowed'){ '阻止' } elseif($lower -match 'allow|loaded|允许'){ '允许' } else { '事件' };"
                "    [pscustomobject]@{TimeText=$_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'); IdText=$_.Id; LevelText=$level; VerdictText=$verdict; MessageText=$message}"
                "  } | ConvertTo-Json -Depth 4"
                "} catch {"
                "  [pscustomobject]@{TimeText=''; IdText=''; LevelText=''; VerdictText='读取失败'; MessageText=$_.Exception.Message; ErrorText=$_.FullyQualifiedErrorId; ErrorCategory=$_.CategoryInfo.Category; ErrorType=$_.Exception.GetType().FullName; HResult=('0x{0:X8}' -f ($_.Exception.HResult -band 0xFFFFFFFF))} | ConvertTo-Json -Depth 3"
                "}").arg(requestedEventLimit);
            QString eventErrorText;
            const QString eventJsonText = runPowerShellCaptureText(eventScript, 15000, &eventErrorText);
            if (!eventJsonText.trimmed().isEmpty())
            {
                const auto parsedEvents = parseEventsJson(eventJsonText);
                events = parsedEvents.first;
                eventSummary = parsedEvents.second;
                if (!eventErrorText.trimmed().isEmpty())
                {
                    eventSummary += QStringLiteral("\n%1").arg(eventErrorText);
                }
            }
            else if (!eventErrorText.trimmed().isEmpty())
            {
                eventSummary = QStringLiteral("Code Integrity 事件读取失败：%1").arg(eventErrorText);
            }

            if (appLockerSummary.isEmpty())
            {
                appLockerSummary = QStringLiteral("AppLocker: 未配置");
            }

            if (guardThis == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(qApp, [guardThis,
                                             statusText,
                                             appLockerSummary,
                                             wdacSummary,
                                             defenderSummary,
                                             eventSummary,
                                             appLockerRules = std::move(appLockerRules),
                                             policyFiles = std::move(policyFiles),
                                             events = std::move(events),
                                             defenderRows = std::move(defenderRows)]() mutable {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->applyRefreshResult(
                    statusText,
                    appLockerSummary,
                    wdacSummary,
                    defenderSummary,
                    eventSummary,
                    std::move(appLockerRules),
                    std::move(policyFiles),
                    std::move(events),
                    std::move(defenderRows));
            }, Qt::QueuedConnection);
        }).detach();
    }

    void ApplicationControlPage::applyRefreshResult(
        QString statusText,
        QString appLockerSummary,
        QString wdacSummary,
        QString defenderSummary,
        QString eventSummary,
        QVector<AppLockerRuleRecord> appLockerRules,
        QVector<PolicyFileRecord> policyFiles,
        QVector<EventRecord> events,
        QVector<KeyValueRecord> defenderRows)
    {
        m_appLockerRules = std::move(appLockerRules);

        if (m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("状态: %1").arg(statusText));
        }

        if (m_appLockerSummary != nullptr)
        {
            m_appLockerSummary->setPlainText(appLockerSummary);
        }
        if (m_wdacSummary != nullptr)
        {
            m_wdacSummary->setPlainText(wdacSummary);
        }
        if (m_defenderSummary != nullptr)
        {
            m_defenderSummary->setPlainText(defenderSummary);
        }
        if (m_eventSummary != nullptr)
        {
            m_eventSummary->setProperty("ks_event_base_summary", eventSummary);
            m_eventSummary->setPlainText(eventSummary);
        }

        QVector<QStringList> appLockerRows;
        appLockerRows.reserve(appLockerRules.size());
        for (const AppLockerRuleRecord& record : appLockerRules)
        {
            appLockerRows.push_back(QStringList{
                record.collectionText,
                record.actionText,
                record.userText,
                record.sidText,
                record.conditionTypeText,
                record.conditionText,
                record.descriptionText,
                record.riskText
            });
        }
        fillTable(
            m_appLockerTable,
            QStringList{
                QStringLiteral("规则集合"),
                QStringLiteral("Action"),
                QStringLiteral("User"),
                QStringLiteral("SID"),
                QStringLiteral("条件类型"),
                QStringLiteral("路径 / 发布者 / Hash"),
                QStringLiteral("描述"),
                QStringLiteral("风险")
            },
            appLockerRows);

        QVector<QStringList> policyRows;
        policyRows.reserve(policyFiles.size());
        for (const PolicyFileRecord& record : policyFiles)
        {
            policyRows.push_back(QStringList{
                record.pathText,
                record.existsText,
                record.sizeText,
                record.modifiedText,
                record.countText,
                record.detailText
            });
        }
        fillTable(
            m_policyFileTable,
            QStringList{
                QStringLiteral("文件路径"),
                QStringLiteral("存在"),
                QStringLiteral("大小"),
                QStringLiteral("修改时间"),
                QStringLiteral("策略数量"),
                QStringLiteral("说明")
            },
            policyRows);

        m_eventRows = std::move(events);

        QVector<QStringList> eventRows;
        eventRows.reserve(m_eventRows.size());
        for (const EventRecord& record : m_eventRows)
        {
            eventRows.push_back(QStringList{
                record.timeText,
                record.idText,
                record.levelText,
                record.verdictText,
                record.messageText
            });
        }
        fillTable(
            m_codeIntegrityEventTable,
            QStringList{
                QStringLiteral("时间"),
                QStringLiteral("事件 ID"),
                QStringLiteral("级别"),
                QStringLiteral("判定"),
                QStringLiteral("消息")
            },
            eventRows);
        rebuildEventTable();

        QVector<QStringList> defenderRowsTable;
        defenderRowsTable.reserve(defenderRows.size());
        for (const KeyValueRecord& record : defenderRows)
        {
            defenderRowsTable.push_back(QStringList{
                record.nameText,
                record.valueText,
                record.detailText
            });
        }
        fillTable(
            m_defenderTable,
            QStringList{
                QStringLiteral("字段"),
                QStringLiteral("值"),
                QStringLiteral("说明")
            },
            defenderRowsTable);

        if (m_refreshButton != nullptr)
        {
            m_refreshButton->setEnabled(true);
        }
        if (m_exportButton != nullptr)
        {
            m_exportButton->setEnabled(true);
        }
    }

    int ApplicationControlPage::selectedEventLimit() const
    {
        const QString text = m_eventLimitCombo != nullptr
            ? m_eventLimitCombo->currentText()
            : QStringLiteral("最近 200 条");
        const QRegularExpression numberPattern(QStringLiteral("(\\d+)"));
        const QRegularExpressionMatch match = numberPattern.match(text);
        if (!match.hasMatch())
        {
            return 200;
        }
        return std::clamp(match.captured(1).toInt(), 50, 2000);
    }

    void ApplicationControlPage::rebuildEventTable()
    {
        const QString selectedVerdictText = m_eventVerdictFilterCombo != nullptr
            ? m_eventVerdictFilterCombo->currentText()
            : QStringLiteral("全部分类");

        QVector<QStringList> visibleRows;
        visibleRows.reserve(m_eventRows.size());
        for (const EventRecord& record : m_eventRows)
        {
            const bool matched =
                selectedVerdictText == QStringLiteral("全部分类") ||
                record.verdictText.compare(selectedVerdictText, Qt::CaseInsensitive) == 0;
            if (!matched)
            {
                continue;
            }

            visibleRows.push_back(QStringList{
                record.timeText,
                record.idText,
                record.levelText,
                record.verdictText,
                record.messageText
            });
        }

        fillTable(
            m_eventTable,
            QStringList{
                QStringLiteral("时间"),
                QStringLiteral("事件 ID"),
                QStringLiteral("级别"),
                QStringLiteral("判定"),
                QStringLiteral("消息")
            },
            visibleRows);

        if (m_eventSummary != nullptr)
        {
            const QString baseSummary = m_eventSummary->property("ks_event_base_summary").toString().trimmed().isEmpty()
                ? m_eventSummary->toPlainText().section(QStringLiteral("\n筛选："), 0, 0)
                : m_eventSummary->property("ks_event_base_summary").toString();
            if (selectedVerdictText == QStringLiteral("全部分类"))
            {
                m_eventSummary->setPlainText(baseSummary);
            }
            else
            {
                m_eventSummary->setPlainText(QStringLiteral("%1\n筛选：%2，显示 %3 / %4。")
                    .arg(baseSummary)
                    .arg(selectedVerdictText)
                    .arg(visibleRows.size())
                    .arg(m_eventRows.size()));
            }
        }
    }

    QString ApplicationControlPage::buildPathMatchHint(const QString& filePathText) const
    {
        if (m_appLockerRules.isEmpty())
        {
            return QStringLiteral("当前没有 AppLocker 规则缓存，无法判断路径命中。");
        }

        QString sanitizedPathText = filePathText.trimmed();
        sanitizedPathText.remove(QChar('"'));
        const QFileInfo fileInfo(sanitizedPathText);
        const QString normalizedPath = QDir::toNativeSeparators(fileInfo.exists() ? fileInfo.absoluteFilePath() : sanitizedPathText);
        QStringList matches;

        for (const AppLockerRuleRecord& record : m_appLockerRules)
        {
            if (!record.conditionTypeText.contains(QStringLiteral("Path"), Qt::CaseInsensitive))
            {
                continue;
            }

            QString conditionText = record.conditionText;
            const QRegularExpression pathExtractor(QStringLiteral("Path=([^;|]+)"), QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch match = pathExtractor.match(conditionText);
            if (match.hasMatch())
            {
                conditionText = match.captured(1).trimmed();
            }

            conditionText = expandCommonEnvironmentTokens(conditionText);
            const QRegularExpression regex = pathLikeTextToRegex(conditionText);
            const bool matched = regex.isValid()
                ? regex.match(normalizedPath).hasMatch()
                : normalizedPath.compare(conditionText, Qt::CaseInsensitive) == 0;
            if (!matched)
            {
                continue;
            }

            matches.push_back(QStringLiteral("%1 | %2 | %3 | %4")
                .arg(record.collectionText, record.actionText, record.userText, conditionText));
        }

        if (matches.isEmpty())
        {
            return QStringLiteral("未发现明显的 AppLocker 路径规则命中。");
        }

        return QStringLiteral("可能命中 %1 条 AppLocker 路径规则：\n%2")
            .arg(matches.size())
            .arg(matches.join(QStringLiteral("\n")));
    }

    void ApplicationControlPage::runFileDiagnosisAsync()
    {
        QString filePath = m_filePathEdit != nullptr ? m_filePathEdit->text().trimmed() : QString();
        filePath.remove(QChar('"'));
        if (filePath.isEmpty())
        {
            QMessageBox::information(this, QStringLiteral("文件诊断"), QStringLiteral("请输入文件路径。"));
            return;
        }

        if (m_fileDiagnoseButton != nullptr)
        {
            m_fileDiagnoseButton->setEnabled(false);
        }
        if (m_fileDiagnosisSummary != nullptr)
        {
            m_fileDiagnosisSummary->setPlainText(QStringLiteral("正在诊断：%1").arg(filePath));
        }

        const QPointer<ApplicationControlPage> guardThis(this);
        std::thread([guardThis, filePath]() {
            QVector<KeyValueRecord> rows;
            QString summaryText;

            const QFileInfo fileInfo(filePath);
            const bool exists = fileInfo.exists() && fileInfo.isFile();
            const QString normalizedPath = QDir::toNativeSeparators(fileInfo.exists() ? fileInfo.absoluteFilePath() : filePath);
            const QString suffixText = fileInfo.suffix().toLower();
            const QString publisherText = exists
                ? QString::fromStdString(ks::startup::QueryPublisherTextByPath(filePath.toStdString()))
                : QString();
            const QString pathMatchHint = guardThis != nullptr ? guardThis->buildPathMatchHint(filePath) : QStringLiteral("页面已关闭");

            const auto pushRow = [&rows](const QString& nameText, const QString& valueText, const QString& detailText) {
                KeyValueRecord record;
                record.nameText = nameText;
                record.valueText = valueText;
                record.detailText = detailText;
                rows.push_back(record);
            };

            pushRow(QStringLiteral("文件存在"), exists ? QStringLiteral("Yes") : QStringLiteral("No"), normalizedPath);
            pushRow(QStringLiteral("文件类型"), suffixText.isEmpty() ? QStringLiteral("—") : suffixText.toUpper(), QStringLiteral("输入文件扩展名"));

            if (exists)
            {
                QFile file(fileInfo.absoluteFilePath());
                if (file.open(QIODevice::ReadOnly))
                {
                    QCryptographicHash sha256(QCryptographicHash::Sha256);
                    while (!file.atEnd())
                    {
                        const QByteArray chunk = file.read(1024 * 1024);
                        if (!chunk.isEmpty())
                        {
                            sha256.addData(chunk);
                        }
                    }
                    pushRow(QStringLiteral("SHA256"), QString::fromLatin1(sha256.result().toHex()), QStringLiteral("QCryptographicHash"));
                }
                else
                {
                    pushRow(QStringLiteral("SHA256"), QStringLiteral("读取失败"), file.errorString());
                }
            }
            else
            {
                pushRow(QStringLiteral("SHA256"), QStringLiteral("—"), QStringLiteral("文件不存在"));
            }

            pushRow(
                QStringLiteral("签名/发布者"),
                publisherText.isEmpty() ? QStringLiteral("未获取到") : publisherText,
                QStringLiteral("复用 ks::startup::QueryPublisherTextByPath / WinVerifyTrust"));

            pushRow(
                QStringLiteral("AppLocker 路径命中"),
                pathMatchHint,
                QStringLiteral("只读推测，不修改策略"));

            pushRow(
                QStringLiteral("WDAC 提示"),
                QStringLiteral("若系统存在 WDAC / Code Integrity 策略，请结合事件日志判断最终结果。"),
                QStringLiteral("第一版仅做存在性和事件诊断"));

            QString testAppLockerText;
            if (exists)
            {
                QString escapedFilePath = filePath;
                escapedFilePath.replace(QStringLiteral("'"), QStringLiteral("''"));
                const QString testScript = QStringLiteral(
                    "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
                    "try {"
                    "  $path='%1';"
                    "  $result = Test-AppLockerPolicy -Path $path -ErrorAction Stop;"
                    "  if($null -eq $result){ Write-Output '__NO_RESULT__' } else { $result | Out-String -Width 65535 }"
                    "} catch {"
                    "  Write-Output '__ERROR__';"
                    "  Write-Output $_.Exception.Message"
                    "}")
                    .arg(escapedFilePath);
                QString testErrorText;
                testAppLockerText = runPowerShellCaptureText(testScript, 12000, &testErrorText);
                if (!testAppLockerText.trimmed().isEmpty())
                {
                    pushRow(QStringLiteral("Test-AppLockerPolicy"), collapseSpaces(testAppLockerText), QStringLiteral("PowerShell 结构化测试"));
                }
                else if (!testErrorText.trimmed().isEmpty())
                {
                    pushRow(QStringLiteral("Test-AppLockerPolicy"), QStringLiteral("不可用"), testErrorText);
                }
            }

            summaryText = QStringLiteral(
                "文件：%1\n存在：%2\n发布者：%3\n路径命中：%4")
                .arg(normalizedPath)
                .arg(exists ? QStringLiteral("Yes") : QStringLiteral("No"))
                .arg(publisherText.isEmpty() ? QStringLiteral("未获取到") : publisherText)
                .arg(pathMatchHint);

            if (guardThis == nullptr)
            {
                return;
            }

            QMetaObject::invokeMethod(qApp, [guardThis, summaryText, rows = std::move(rows)]() mutable {
                if (guardThis == nullptr)
                {
                    return;
                }
                guardThis->applyFileDiagnosisResult(summaryText, std::move(rows));
            }, Qt::QueuedConnection);
        }).detach();
    }

    void ApplicationControlPage::applyFileDiagnosisResult(QString summaryText, QVector<KeyValueRecord> rows)
    {
        if (m_fileDiagnosisSummary != nullptr)
        {
            m_fileDiagnosisSummary->setPlainText(summaryText);
        }

        QVector<QStringList> tableRows;
        tableRows.reserve(rows.size());
        for (const KeyValueRecord& record : rows)
        {
            tableRows.push_back(QStringList{ record.nameText, record.valueText, record.detailText });
        }
        fillTable(
            m_fileDiagnosisTable,
            QStringList{ QStringLiteral("检查项"), QStringLiteral("结果"), QStringLiteral("说明") },
            tableRows);

        if (m_fileDiagnoseButton != nullptr)
        {
            m_fileDiagnoseButton->setEnabled(true);
        }
    }
}
