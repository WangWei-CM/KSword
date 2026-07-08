#include "VirusTotalOnlineScan.h"

#include "OnlineScanSupport.h"
#include "../Framework.h"
#include "../SettingsDock/AppearanceSettings.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QTimeZone>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <vector>

namespace
{
    // VirusTotal API 常量：集中维护官方 v3 端点，避免散落在业务逻辑中。
    constexpr const char* kVirusTotalFilesEndpoint = "https://www.virustotal.com/api/v3/files";
    constexpr const char* kVirusTotalLargeUploadUrlEndpoint = "https://www.virustotal.com/api/v3/files/upload_url";
    constexpr const char* kVirusTotalAnalysisEndpointPrefix = "https://www.virustotal.com/api/v3/analyses/";
    constexpr int kInitialPollDelayMs = 15000;
    constexpr int kPollIntervalMs = 15000;
    constexpr int kMaxPollAttempts = 40;

    // setVirusTotalHeaders 作用：
    // - 给 VirusTotal 请求统一添加 API Key 和 User-Agent；
    // - Content-Type 由 QHttpMultiPart 或 Qt 自动处理，不在此处设置。
    // 入参 request：待修改的请求对象。
    // 入参 apiKey：用户配置的 API Key。
    // 返回：无。
    void setVirusTotalHeaders(QNetworkRequest* request, const QString& apiKey)
    {
        if (request == nullptr)
        {
            return;
        }
        request->setRawHeader("x-apikey", apiKey.toUtf8());
        request->setRawHeader("User-Agent", "Ksword5.1-OnlineScan/1.0");
    }

    // jsonValueToInt 作用：
    // - 安全读取 VirusTotal stats 中的整数值；
    // - 兼容 JSON 字段缺失或类型异常。
    // 入参 objectValue：JSON 对象。
    // 入参 keyText：字段名。
    // 返回：字段整数值，缺失时为 0。
    int jsonValueToInt(const QJsonObject& objectValue, const QString& keyText)
    {
        return objectValue.value(keyText).toInt(0);
    }

    // utcTimestampText 作用：
    // - 生成原始响应章节时间戳；
    // - 输出使用 ISO 格式，便于导出 JSON/TXT 后做时间线分析。
    // 返回：当前 UTC 时间文本。
    QString utcTimestampText()
    {
        return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    }

    // fileDisplayText 作用：
    // - 生成实时结果窗口顶部文件说明；
    // - 兼容路径为空或 QFileInfo 无法解析文件名的情况。
    // 入参 filePath：本地样本路径。
    // 返回：文件名 + 路径的用户可读文本。
    QString fileDisplayText(const QString& filePath)
    {
        const QFileInfo fileInfo(filePath);
        const QString fileName = fileInfo.fileName().isEmpty()
            ? QStringLiteral("<未知文件>")
            : fileInfo.fileName();
        return QStringLiteral("%1\n%2").arg(fileName, QDir::toNativeSeparators(filePath));
    }

    // formatByteCount 作用：
    // - Convert VT/local file byte counts into compact human-readable text.
    // - The UI uses this for the headline and basic information table.
    // Input byteCount: raw byte count; negative means unknown.
    // Return: formatted size text, or "-" when unavailable.
    QString formatByteCount(const qint64 byteCount)
    {
        if (byteCount < 0)
        {
            return QStringLiteral("-");
        }
        if (byteCount < 1024)
        {
            return QStringLiteral("%1 B").arg(byteCount);
        }

        const double kibValue = static_cast<double>(byteCount) / 1024.0;
        if (kibValue < 1024.0)
        {
            return QStringLiteral("%1 KB").arg(kibValue, 0, 'f', 2);
        }

        const double mibValue = kibValue / 1024.0;
        if (mibValue < 1024.0)
        {
            return QStringLiteral("%1 MB").arg(mibValue, 0, 'f', 2);
        }

        const double gibValue = mibValue / 1024.0;
        return QStringLiteral("%1 GB").arg(gibValue, 0, 'f', 2);
    }

    // unixDateText 作用：
    // - Format VT epoch seconds into local time text.
    // - Unknown or zero timestamps are rendered as "-".
    // Input secondsValue: seconds since Unix epoch.
    // Return: local date/time string.
    QString unixDateText(const qint64 secondsValue)
    {
        if (secondsValue <= 0)
        {
            return QStringLiteral("-");
        }
        return QDateTime::fromSecsSinceEpoch(secondsValue, QTimeZone::UTC)
            .toLocalTime()
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }

    // jsonPrimitiveText 作用：
    // - Convert a primitive JSON value into a short display string for tables/trees.
    // - Objects and arrays are represented by shape markers because children carry details.
    // Input jsonValue: VT JSON value.
    // Return: user-readable scalar value.
    QString jsonPrimitiveText(const QJsonValue& jsonValue)
    {
        if (jsonValue.isString())
        {
            return jsonValue.toString();
        }
        if (jsonValue.isBool())
        {
            return jsonValue.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        }
        if (jsonValue.isDouble())
        {
            const double numberValue = jsonValue.toDouble();
            const qint64 integerValue = static_cast<qint64>(numberValue);
            if (qFuzzyCompare(numberValue + 1.0, static_cast<double>(integerValue) + 1.0))
            {
                return QString::number(integerValue);
            }
            return QString::number(numberValue, 'f', 4);
        }
        if (jsonValue.isNull())
        {
            return QStringLiteral("null");
        }
        if (jsonValue.isUndefined())
        {
            return QStringLiteral("<undefined>");
        }
        if (jsonValue.isArray())
        {
            return QStringLiteral("[Array]");
        }
        if (jsonValue.isObject())
        {
            return QStringLiteral("{Object}");
        }
        return QString();
    }

    // categoryText 作用：
    // - Localize VirusTotal engine result category into concise Chinese text.
    // - Unknown categories are preserved so new VT values remain visible.
    // Input categoryTextValue: VT category string.
    // Return: localized category label.
    QString categoryText(const QString& categoryTextValue)
    {
        const QString normalizedText = categoryTextValue.trimmed().toLower();
        if (normalizedText == QStringLiteral("malicious"))
        {
            return QStringLiteral("恶意");
        }
        if (normalizedText == QStringLiteral("suspicious"))
        {
            return QStringLiteral("可疑");
        }
        if (normalizedText == QStringLiteral("harmless"))
        {
            return QStringLiteral("无害");
        }
        if (normalizedText == QStringLiteral("undetected"))
        {
            return QStringLiteral("无检出");
        }
        if (normalizedText == QStringLiteral("timeout"))
        {
            return QStringLiteral("超时");
        }
        if (normalizedText == QStringLiteral("confirmed-timeout"))
        {
            return QStringLiteral("确认超时");
        }
        if (normalizedText == QStringLiteral("failure"))
        {
            return QStringLiteral("失败");
        }
        if (normalizedText == QStringLiteral("type-unsupported"))
        {
            return QStringLiteral("类型不支持");
        }
        return categoryTextValue.trimmed().isEmpty() ? QStringLiteral("-") : categoryTextValue.trimmed();
    }

    // categoryPriority 作用：
    // - Sort engine rows by analyst importance.
    // - Detections appear first, then weak/unknown states, then clean results.
    // Input categoryTextValue: VT category string.
    // Return: lower value sorts earlier.
    int categoryPriority(const QString& categoryTextValue)
    {
        const QString normalizedText = categoryTextValue.trimmed().toLower();
        if (normalizedText == QStringLiteral("malicious"))
        {
            return 0;
        }
        if (normalizedText == QStringLiteral("suspicious"))
        {
            return 1;
        }
        if (normalizedText == QStringLiteral("failure") ||
            normalizedText == QStringLiteral("timeout") ||
            normalizedText == QStringLiteral("confirmed-timeout"))
        {
            return 2;
        }
        if (normalizedText == QStringLiteral("undetected"))
        {
            return 3;
        }
        if (normalizedText == QStringLiteral("harmless"))
        {
            return 4;
        }
        return 5;
    }

    // categoryColor 作用：
    // - Map VT categories to visible semantic colors.
    // - Colors are used only as foreground hints and keep table text readable.
    // Input categoryTextValue: VT category string.
    // Return: QColor; invalid color means default palette foreground.
    QColor categoryColor(const QString& categoryTextValue)
    {
        const QString normalizedText = categoryTextValue.trimmed().toLower();
        if (normalizedText == QStringLiteral("malicious"))
        {
            return QColor(255, 76, 76);
        }
        if (normalizedText == QStringLiteral("suspicious"))
        {
            return QColor(255, 160, 64);
        }
        if (normalizedText == QStringLiteral("failure") ||
            normalizedText == QStringLiteral("timeout") ||
            normalizedText == QStringLiteral("confirmed-timeout"))
        {
            return QColor(210, 170, 80);
        }
        if (normalizedText == QStringLiteral("undetected") ||
            normalizedText == QStringLiteral("harmless"))
        {
            return QColor(140, 210, 150);
        }
        return QColor();
    }

    // engineDetectionText 作用：
    // - 把 VirusTotal 单引擎 category/result 压缩成报告页一行双引擎布局需要的短文本；
    // - 恶意/可疑统一显示“检出：XXX”，未检出/无害显示短状态，异常状态保留诊断语义。
    // 入参 categoryTextValue：VT 返回的 category 字段。
    // 入参 resultTextValue：VT 返回的 result 字段。
    // 返回：用于“多引擎检测”结果列的短文本。
    QString engineDetectionText(const QString& categoryTextValue, const QString& resultTextValue)
    {
        const QString normalizedText = categoryTextValue.trimmed().toLower();
        const QString trimmedResultText = resultTextValue.trimmed();
        if (normalizedText == QStringLiteral("malicious") ||
            normalizedText == QStringLiteral("suspicious"))
        {
            return QStringLiteral("检出：%1").arg(trimmedResultText.isEmpty()
                ? categoryText(categoryTextValue)
                : trimmedResultText);
        }
        if (normalizedText == QStringLiteral("undetected"))
        {
            return QStringLiteral("未检出");
        }
        if (normalizedText == QStringLiteral("harmless"))
        {
            return QStringLiteral("安全");
        }
        if (normalizedText == QStringLiteral("timeout") ||
            normalizedText == QStringLiteral("confirmed-timeout"))
        {
            return QStringLiteral("超时");
        }
        if (normalizedText == QStringLiteral("failure"))
        {
            return QStringLiteral("失败");
        }
        if (normalizedText == QStringLiteral("type-unsupported"))
        {
            return QStringLiteral("不支持");
        }
        return trimmedResultText.isEmpty() ? categoryText(categoryTextValue) : trimmedResultText;
    }

    struct ReportVerdict
    {
        QString labelText;     // 报告顶部显示的四类结论之一。
        QString iconPath;      // qrc 中的 SVG 图标路径。
        QColor accentColor;    // 文本强调色。
    };

    // reportVerdictFromStats 作用：
    // - 把 VirusTotal stats 压缩为用户要求的四类结论：威胁、可疑、安全、未检出；
    // - 恶意优先级最高，其次可疑；无恶意/可疑且有 harmless 时视为安全；其余为未检出。
    // 入参 maliciousCount/suspiciousCount/harmlessCount：VT stats 中对应计数。
    // 返回：报告顶部使用的文案、图标和强调色。
    ReportVerdict reportVerdictFromStats(
        const int maliciousCount,
        const int suspiciousCount,
        const int harmlessCount)
    {
        if (maliciousCount > 0)
        {
            return {
                QStringLiteral("威胁"),
                QStringLiteral(":/Icon/vt_status_threat.svg"),
                QColor(239, 68, 68),
            };
        }
        if (suspiciousCount > 0)
        {
            return {
                QStringLiteral("可疑"),
                QStringLiteral(":/Icon/vt_status_suspicious.svg"),
                QColor(245, 158, 11),
            };
        }
        if (harmlessCount > 0)
        {
            return {
                QStringLiteral("安全"),
                QStringLiteral(":/Icon/vt_status_safe.svg"),
                QColor(34, 197, 94),
            };
        }
        return {
            QStringLiteral("未检出"),
            QStringLiteral(":/Icon/vt_status_undetected.svg"),
            QColor(59, 130, 246),
        };
    }

    // createReadOnlyTableItem 作用：
    // - Create a non-editable table item with optional semantic color.
    // - All result tables are display-only, so this helper centralizes flags/tooltips.
    // Input textValue: cell text.
    // Input foregroundColor: optional foreground color.
    // Return: newly allocated QTableWidgetItem owned by the table after setItem().
    QTableWidgetItem* createReadOnlyTableItem(const QString& textValue, const QColor& foregroundColor = QColor())
    {
        QTableWidgetItem* item = new QTableWidgetItem(textValue);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        item->setToolTip(textValue);
        if (foregroundColor.isValid())
        {
            item->setForeground(QBrush(foregroundColor));
        }
        return item;
    }

    // configureReadOnlyTable 作用：
    // - Apply common read-only behavior to VT result tables.
    // - Keeps interaction predictable across overview, engine and detail tables.
    // Input table: table widget to configure.
    // Return: no value.
    void configureReadOnlyTable(QTableWidget* table)
    {
        if (table == nullptr)
        {
            return;
        }
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setHighlightSections(false);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setWordWrap(false);
    }

    // configureBorderlessInfoTable 作用：
    // - 把 QTableWidget 调整为报告页的“文本信息块”效果；
    // - 隐藏表头、网格和外框，只保留两列字段和值。
    // 入参 table：目标表格。
    // 返回：无。
    void configureBorderlessInfoTable(QTableWidget* table)
    {
        if (table == nullptr)
        {
            return;
        }
        configureReadOnlyTable(table);
        table->setFrameShape(QFrame::NoFrame);
        table->setShowGrid(false);
        table->setFocusPolicy(Qt::NoFocus);
        table->horizontalHeader()->setVisible(false);
        table->verticalHeader()->setVisible(false);
        table->verticalHeader()->setDefaultSectionSize(18);
        table->verticalHeader()->setMinimumSectionSize(16);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }

    // fitTableHeightToRows 作用：
    // - 按当前行高计算 QTableWidget 的最小/最大高度；
    // - 用于基础信息这种紧凑文本块，避免空白区域让行距显得松散。
    // 入参 table：需要压缩高度的表格。
    // 返回：无。
    void fitTableHeightToRows(QTableWidget* table)
    {
        if (table == nullptr)
        {
            return;
        }
        int totalHeight = table->frameWidth() * 2 + 2;
        if (table->horizontalHeader() != nullptr && table->horizontalHeader()->isVisible())
        {
            totalHeight += table->horizontalHeader()->height();
        }
        for (int rowIndex = 0; rowIndex < table->rowCount(); ++rowIndex)
        {
            totalHeight += table->rowHeight(rowIndex);
        }
        table->setMinimumHeight(totalHeight);
        table->setMaximumHeight(totalHeight);
    }

    // setTableRow 作用：
    // - Write a two-column key/value row into a QTableWidget.
    // - It grows the table when needed and keeps cells read-only.
    // Input table: target table.
    // Input rowIndex: target row.
    // Input keyText: left column label.
    // Input valueText: right column value.
    // Return: no value.
    void setTableRow(QTableWidget* table, const int rowIndex, const QString& keyText, const QString& valueText)
    {
        if (table == nullptr)
        {
            return;
        }
        if (table->rowCount() <= rowIndex)
        {
            table->setRowCount(rowIndex + 1);
        }
        table->setItem(rowIndex, 0, createReadOnlyTableItem(keyText));
        table->setItem(rowIndex, 1, createReadOnlyTableItem(valueText));
        table->setRowHeight(rowIndex, 18);
    }

    // addTreeLeaf 作用：
    // - Add a simple key/value child item to a QTreeWidgetItem.
    // - Used by both readable static analysis tree and raw response tree.
    // Input parentItem: parent tree item.
    // Input keyText: field name.
    // Input valueText: field value.
    // Return: created child item, or nullptr if parent is missing.
    QTreeWidgetItem* addTreeLeaf(QTreeWidgetItem* parentItem, const QString& keyText, const QString& valueText)
    {
        if (parentItem == nullptr)
        {
            return nullptr;
        }
        QTreeWidgetItem* childItem = new QTreeWidgetItem(parentItem);
        childItem->setText(0, keyText);
        childItem->setText(1, valueText);
        childItem->setToolTip(0, keyText);
        childItem->setToolTip(1, valueText);
        return childItem;
    }

    // appendJsonValueToTree 作用：
    // - Recursively project JSON objects/arrays into an expandable tree.
    // - Object and array nodes keep the structural marker in column 1; scalar leaves show values.
    // Input parentItem: parent tree item.
    // Input keyText: current field/index label.
    // Input jsonValue: current JSON value.
    // Return: no value.
    void appendJsonValueToTree(QTreeWidgetItem* parentItem, const QString& keyText, const QJsonValue& jsonValue)
    {
        if (parentItem == nullptr)
        {
            return;
        }

        QTreeWidgetItem* currentItem = new QTreeWidgetItem(parentItem);
        currentItem->setText(0, keyText);
        if (jsonValue.isObject())
        {
            const QJsonObject objectValue = jsonValue.toObject();
            currentItem->setText(1, QStringLiteral("{ %1 fields }").arg(objectValue.size()));
            for (auto iterator = objectValue.constBegin(); iterator != objectValue.constEnd(); ++iterator)
            {
                appendJsonValueToTree(currentItem, iterator.key(), iterator.value());
            }
            return;
        }
        if (jsonValue.isArray())
        {
            const QJsonArray arrayValue = jsonValue.toArray();
            currentItem->setText(1, QStringLiteral("[ %1 items ]").arg(arrayValue.size()));
            for (int index = 0; index < arrayValue.size(); ++index)
            {
                appendJsonValueToTree(currentItem, QStringLiteral("[%1]").arg(index), arrayValue.at(index));
            }
            return;
        }

        const QString valueText = jsonPrimitiveText(jsonValue);
        currentItem->setText(1, valueText);
        currentItem->setToolTip(0, keyText);
        currentItem->setToolTip(1, valueText);
    }
}

VirusTotalOnlineScan::VirusTotalOnlineScan(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
}

VirusTotalOnlineScan::~VirusTotalOnlineScan() = default;

void VirusTotalOnlineScan::scanFile(
    const QString& filePath,
    const QString& sourceText,
    QWidget* dialogParent)
{
    if (m_scanInProgress)
    {
        ks::online_scan::showErrorDialog(
            dialogParent,
            QStringLiteral("VirusTotal 在线扫描"),
            QStringLiteral("当前 VirusTotal 扫描仍在进行，请等待完成后再上传新文件。"));
        return;
    }

    resetRuntimeState();
    m_dialogParent = dialogParent;
    m_filePath = filePath.trimmed();
    m_sourceText = sourceText.trimmed().isEmpty()
        ? QStringLiteral("手动上传")
        : sourceText.trimmed();

    // settings 作用：从统一设置 JSON 读取 API Key，未配置时直接提示用户补充。
    const ks::settings::AppearanceSettings settings = ks::settings::loadAppearanceSettings();
    m_apiKey = settings.virusTotalApiKey.trimmed();
    if (m_apiKey.isEmpty())
    {
        ks::online_scan::showMissingApiKeyDialog(dialogParent, QStringLiteral("VirusTotal"));
        return;
    }

    QString fileErrorText;
    if (!ks::online_scan::validateReadableFile(
        m_filePath,
        ks::online_scan::kVirusTotalLargeUploadMaxBytes,
        &fileErrorText))
    {
        ks::online_scan::showErrorDialog(dialogParent, QStringLiteral("VirusTotal 在线扫描"), fileErrorText);
        return;
    }

    m_scanInProgress = true;
    m_progressPid = kPro.add("在线扫描", "VirusTotal 上传准备");
    kPro.set(m_progressPid, "VirusTotal：准备上传样本", 0, 5.0f);

    const QFileInfo fileInfo(m_filePath);
    if (fileInfo.size() > ks::online_scan::kVirusTotalDirectUploadMaxBytes)
    {
        requestLargeUploadUrl();
        return;
    }
    uploadFileToUrl(QUrl(QString::fromLatin1(kVirusTotalFilesEndpoint)));
}

void VirusTotalOnlineScan::scanFile(const QString& filePath, QWidget* dialogParent)
{
    scanFile(filePath, QStringLiteral("手动上传"), dialogParent);
}

void VirusTotalOnlineScan::scanFileAndAutoDelete(
    const QString& filePath,
    const QString& sourceText,
    QWidget* dialogParent)
{
    VirusTotalOnlineScan* scanner = new VirusTotalOnlineScan(dialogParent);
    scanner->m_autoDeleteWhenFinished = true;
    scanner->scanFile(filePath, sourceText, dialogParent);
    if (!scanner->m_scanInProgress)
    {
        scanner->deleteLater();
    }
}

void VirusTotalOnlineScan::scanFileAndAutoDelete(const QString& filePath, QWidget* dialogParent)
{
    scanFileAndAutoDelete(filePath, QStringLiteral("手动上传"), dialogParent);
}

void VirusTotalOnlineScan::requestLargeUploadUrl()
{
    QNetworkRequest request(QUrl(QString::fromLatin1(kVirusTotalLargeUploadUrlEndpoint)));
    setVirusTotalHeaders(&request, m_apiKey);

    kPro.set(m_progressPid, "VirusTotal：获取大文件上传地址", 0, 8.0f);
    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            handleUploadUrlReply(reply);
        });
}

void VirusTotalOnlineScan::uploadFileToUrl(const QUrl& uploadUrl)
{
    QFile* uploadFile = new QFile(m_filePath);
    if (!uploadFile->open(QIODevice::ReadOnly))
    {
        const QString errorText = QStringLiteral("打开上传文件失败：%1").arg(uploadFile->errorString());
        uploadFile->deleteLater();
        finishWithError(QStringLiteral("VirusTotal 上传失败"), errorText);
        return;
    }

    // multiPart 作用：构造 VirusTotal v3 files 上传所需的 multipart/form-data 请求体。
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    const QString safeFileName = ks::online_scan::sanitizeFileNameForContentDisposition(QFileInfo(m_filePath).fileName());
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(safeFileName)));
    filePart.setBodyDevice(uploadFile);
    uploadFile->setParent(multiPart);
    multiPart->append(filePart);

    QNetworkRequest request(uploadUrl);
    setVirusTotalHeaders(&request, m_apiKey);

    kPro.set(m_progressPid, "VirusTotal：上传样本", 0, 12.0f);
    QNetworkReply* reply = m_networkManager->post(request, multiPart);
    multiPart->setParent(reply);

    connect(reply, &QNetworkReply::uploadProgress, this,
        [this](const qint64 sentBytes, const qint64 totalBytes)
        {
            if (m_progressPid == 0 || totalBytes <= 0)
            {
                return;
            }
            const float uploadPercent = static_cast<float>(sentBytes) * 40.0f / static_cast<float>(totalBytes);
            kPro.set(m_progressPid, "VirusTotal：上传样本", 0, 12.0f + std::min(uploadPercent, 40.0f));
        });
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            handleUploadReply(reply);
        });
}

void VirusTotalOnlineScan::handleUploadUrlReply(QNetworkReply* reply)
{
    const QByteArray bodyBytes = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;

    if (!networkOk)
    {
        const QString errorText = ks::online_scan::networkReplyErrorText(reply, bodyBytes);
        appendRawTextSection(QStringLiteral("获取大文件上传 URL 失败"), errorText);
        appendRawReplyBodySection(QStringLiteral("GET /api/v3/files/upload_url 错误响应体"), bodyBytes);
        reply->deleteLater();
        finishWithError(
            QStringLiteral("VirusTotal 获取上传地址失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
    if (parseErrorText.isEmpty())
    {
        appendRawJsonSection(QStringLiteral("GET /api/v3/files/upload_url"), rootObject);
    }
    else
    {
        appendRawTextSection(QStringLiteral("GET /api/v3/files/upload_url 解析失败"), parseErrorText);
        appendRawReplyBodySection(QStringLiteral("GET /api/v3/files/upload_url 原始响应体"), bodyBytes);
    }
    const QString uploadUrlText = rootObject.value(QStringLiteral("data")).toString().trimmed();
    if (!parseErrorText.isEmpty() || uploadUrlText.isEmpty())
    {
        finishWithError(
            QStringLiteral("VirusTotal 获取上传地址失败"),
            parseErrorText.isEmpty() ? QStringLiteral("响应中缺少 data 上传 URL。") : parseErrorText);
        return;
    }

    updateResultSummary(QStringLiteral(
        "来源：%1\n文件：%2\n状态：已取得大文件上传 URL，正在上传样本。")
        .arg(m_sourceText, fileDisplayText(m_filePath)));
    uploadFileToUrl(QUrl(uploadUrlText));
}

void VirusTotalOnlineScan::handleUploadReply(QNetworkReply* reply)
{
    const QByteArray bodyBytes = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;

    if (!networkOk)
    {
        const QString errorText = ks::online_scan::networkReplyErrorText(reply, bodyBytes);
        appendRawTextSection(QStringLiteral("上传样本失败"), errorText);
        appendRawReplyBodySection(QStringLiteral("POST /api/v3/files 错误响应体"), bodyBytes);
        reply->deleteLater();
        finishWithError(
            QStringLiteral("VirusTotal 上传失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
    if (parseErrorText.isEmpty())
    {
        appendRawJsonSection(QStringLiteral("POST /api/v3/files 上传响应"), rootObject);
    }
    else
    {
        appendRawTextSection(QStringLiteral("POST /api/v3/files 上传响应解析失败"), parseErrorText);
        appendRawReplyBodySection(QStringLiteral("POST /api/v3/files 原始响应体"), bodyBytes);
    }
    if (!parseErrorText.isEmpty())
    {
        finishWithError(QStringLiteral("VirusTotal 上传失败"), parseErrorText);
        return;
    }

    const QJsonObject dataObject = rootObject.value(QStringLiteral("data")).toObject();
    m_analysisId = dataObject.value(QStringLiteral("id")).toString().trimmed();
    if (m_analysisId.isEmpty())
    {
        finishWithError(
            QStringLiteral("VirusTotal 上传失败"),
            QStringLiteral("上传响应中缺少 analysis id。\n\n%1").arg(ks::online_scan::formatJsonObject(rootObject)));
        return;
    }

    kPro.set(m_progressPid, "VirusTotal：等待分析结果", 0, 55.0f);
    updateResultSummary(QStringLiteral(
        "来源：%1\n文件：%2\nAnalysisId：%3\n状态：上传响应已返回，正在等待 VirusTotal 分析结果。")
        .arg(m_sourceText, fileDisplayText(m_filePath), m_analysisId));
    scheduleAnalysisPoll(kInitialPollDelayMs);
}

void VirusTotalOnlineScan::scheduleAnalysisPoll(const int delayMs)
{
    QTimer::singleShot(delayMs, this, [this]()
        {
            requestAnalysisStatus();
        });
}

void VirusTotalOnlineScan::requestAnalysisStatus()
{
    if (!m_scanInProgress || m_analysisId.isEmpty())
    {
        return;
    }

    ++m_pollAttempt;
    const float progressValue = std::min(95.0f, 55.0f + static_cast<float>(m_pollAttempt) * 1.0f);
    kPro.set(
        m_progressPid,
        QStringLiteral("VirusTotal：轮询分析结果(%1/%2)").arg(m_pollAttempt).arg(kMaxPollAttempts).toStdString(),
        0,
        progressValue);

    QNetworkRequest request(QUrl(QString::fromLatin1(kVirusTotalAnalysisEndpointPrefix) + m_analysisId));
    setVirusTotalHeaders(&request, m_apiKey);
    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            handleAnalysisReply(reply);
        });
}

void VirusTotalOnlineScan::handleAnalysisReply(QNetworkReply* reply)
{
    const QByteArray bodyBytes = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;

    if (!networkOk)
    {
        const QString errorText = ks::online_scan::networkReplyErrorText(reply, bodyBytes);
        appendRawTextSection(
            QStringLiteral("GET /api/v3/analyses/%1 查询失败").arg(m_analysisId),
            errorText);
        appendRawReplyBodySection(
            QStringLiteral("GET /api/v3/analyses/%1 错误响应体").arg(m_analysisId),
            bodyBytes);
        reply->deleteLater();
        finishWithError(
            QStringLiteral("VirusTotal 查询失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
    if (parseErrorText.isEmpty())
    {
        appendRawJsonSection(
            QStringLiteral("GET /api/v3/analyses/%1 第 %2 次响应")
                .arg(m_analysisId)
                .arg(m_pollAttempt),
            rootObject);
    }
    else
    {
        appendRawTextSection(
            QStringLiteral("GET /api/v3/analyses/%1 解析失败").arg(m_analysisId),
            parseErrorText);
        appendRawReplyBodySection(
            QStringLiteral("GET /api/v3/analyses/%1 原始响应体").arg(m_analysisId),
            bodyBytes);
    }
    if (!parseErrorText.isEmpty())
    {
        finishWithError(QStringLiteral("VirusTotal 查询失败"), parseErrorText);
        return;
    }

    const QJsonObject dataObject = rootObject.value(QStringLiteral("data")).toObject();
    const QJsonObject attributesObject = dataObject.value(QStringLiteral("attributes")).toObject();
    const QString statusText = attributesObject.value(QStringLiteral("status")).toString().trimmed().toLower();
    if (statusText == QStringLiteral("completed"))
    {
        finishWithResult(rootObject);
        return;
    }

    if (m_pollAttempt >= kMaxPollAttempts)
    {
        finishWithError(
            QStringLiteral("VirusTotal 查询超时"),
            QStringLiteral("分析任务尚未完成，analysis id：%1\n最后状态：%2")
                .arg(m_analysisId, statusText.isEmpty() ? QStringLiteral("未知") : statusText));
        return;
    }
    updateResultSummary(QStringLiteral(
        "来源：%1\n文件：%2\nAnalysisId：%3\n状态：%4；已轮询 %5/%6 次。")
        .arg(m_sourceText)
        .arg(fileDisplayText(m_filePath))
        .arg(m_analysisId)
        .arg(statusText.isEmpty() ? QStringLiteral("未知") : statusText)
        .arg(m_pollAttempt)
        .arg(kMaxPollAttempts));
    scheduleAnalysisPoll(kPollIntervalMs);
}

void VirusTotalOnlineScan::finishWithError(const QString& titleText, const QString& detailText)
{
    completeProgress(QStringLiteral("VirusTotal：扫描失败"));
    m_scanInProgress = false;
    ensureResultDialog();
    updateResultSummary(QStringLiteral(
        "来源：%1\n文件：%2\n状态：%3\n错误：%4")
        .arg(m_sourceText, fileDisplayText(m_filePath), titleText, detailText));
    if (!m_readableOverviewLabel.isNull())
    {
        m_readableOverviewLabel->setText(QStringLiteral(
            "<table cellspacing='0' cellpadding='0'>"
            "<tr>"
            "<td width='124'><img src=':/Icon/vt_status_threat.svg' width='104' height='104'/></td>"
            "<td>"
            "<div style='font-size:22px;font-weight:700;'>%1</div>"
            "<div style='margin-top:6px;color:#EF4444;font-size:18px;font-weight:700;'>威胁</div>"
            "<div style='margin-top:8px;'>状态：%2</div>"
            "<div style='margin-top:6px;'>错误详情：%3</div>"
            "</td>"
            "</tr>"
            "</table>")
            .arg(QFileInfo(m_filePath).fileName().toHtmlEscaped().isEmpty()
                ? QStringLiteral("<未知文件>")
                : QFileInfo(m_filePath).fileName().toHtmlEscaped())
            .arg(titleText.toHtmlEscaped())
            .arg(detailText.toHtmlEscaped()));
    }
    if (!m_resultTabWidget.isNull())
    {
        m_resultTabWidget->setCurrentIndex(0);
    }
    appendRawTextSection(titleText, detailText);
    finalizeAutoDeleteIfNeeded();
}

void VirusTotalOnlineScan::finishWithResult(const QJsonObject& analysisObject)
{
    completeProgress(QStringLiteral("VirusTotal：扫描完成"));
    m_scanInProgress = false;

    const QString summaryText = buildResultSummary(analysisObject);
    ensureResultDialog();
    refreshReadableResult(analysisObject);
    updateResultSummary(QStringLiteral("来源：%1\n%2").arg(m_sourceText, summaryText));

    finalizeAutoDeleteIfNeeded();
}

void VirusTotalOnlineScan::resetRuntimeState()
{
    m_dialogParent.clear();
    m_resultDialog.clear();
    m_resultSummaryLabel.clear();
    m_resultTabWidget.clear();
    m_readableOverviewLabel.clear();
    m_fileInfoTable.clear();
    m_engineTable.clear();
    m_staticAnalysisTree.clear();
    m_responseTree.clear();
    m_resultEditor.clear();
    m_filePath.clear();
    m_sourceText.clear();
    m_resultRawText.clear();
    m_resultRawSections = QJsonArray();
    m_apiKey.clear();
    m_analysisId.clear();
    m_pollAttempt = 0;
    m_scanInProgress = false;
    m_progressPid = 0;
    m_deleteAfterResultDialogClosed = false;
}

void VirusTotalOnlineScan::completeProgress(const QString& messageText)
{
    if (m_progressPid == 0)
    {
        return;
    }
    kPro.set(m_progressPid, messageText.toStdString(), 0, 100.0f);
    m_progressPid = 0;
}

void VirusTotalOnlineScan::ensureResultDialog()
{
    if (!m_resultDialog.isNull())
    {
        if (!m_resultDialog->isVisible())
        {
            m_resultDialog->show();
        }
        m_resultDialog->raise();
        m_resultDialog->activateWindow();
        return;
    }

    QDialog* resultDialog = new QDialog(m_dialogParent.data());
    resultDialog->setAttribute(Qt::WA_DeleteOnClose, true);
    resultDialog->setObjectName(QStringLiteral("virusTotalLiveResultDialog"));
    resultDialog->setWindowTitle(QStringLiteral("VirusTotal 云沙箱上传结果"));
    resultDialog->resize(1180, 780);
    resultDialog->setStyleSheet(
        KswordTheme::OpaqueDialogStyle(resultDialog->objectName()) +
        QStringLiteral(
            "QDialog#virusTotalLiveResultDialog QLabel#vtOverviewCard{"
            "  border:0;"
            "  padding:4px 0 12px 0;"
            "}"
            "QDialog#virusTotalLiveResultDialog QGroupBox{"
            "  border:0;"
            "  margin-top:12px;"
            "  padding-top:12px;"
            "}"
            "QDialog#virusTotalLiveResultDialog QGroupBox::title{"
            "  subcontrol-origin:margin;"
            "  left:0px;"
            "  padding:0 4px;"
            "  font-weight:600;"
            "}"
            "QDialog#virusTotalLiveResultDialog QTabWidget::pane{"
            "  border:1px solid palette(mid);"
            "}"
            "QDialog#virusTotalLiveResultDialog QTableWidget#vtFileInfoTable{"
            "  border:0;"
            "  background:transparent;"
            "}"
            "QDialog#virusTotalLiveResultDialog QTableWidget#vtFileInfoTable::item{"
            "  border:0;"
            "  padding:1px 8px 1px 0;"
            "}"));

    // dialogLayout 作用：
    // - 顶部不再放 Tab 外状态栏，避免用户先看到临时调试信息；
    // - 中部默认展示报告式可读页面，同时保留响应树和原始数据页；
    // - 底部提供复制、导出和关闭按钮。
    QVBoxLayout* dialogLayout = new QVBoxLayout(resultDialog);
    dialogLayout->setContentsMargins(0, 0, 0, 10);
    dialogLayout->setSpacing(0);

    QTabWidget* resultTabWidget = new QTabWidget(resultDialog);
    resultTabWidget->setDocumentMode(false);
    dialogLayout->addWidget(resultTabWidget, 1);

    QWidget* readablePage = new QWidget(resultTabWidget);
    QVBoxLayout* readablePageLayout = new QVBoxLayout(readablePage);
    readablePageLayout->setContentsMargins(0, 0, 0, 0);
    readablePageLayout->setSpacing(0);

    QScrollArea* readableScrollArea = new QScrollArea(readablePage);
    readableScrollArea->setWidgetResizable(true);
    readableScrollArea->setFrameShape(QFrame::NoFrame);
    readablePageLayout->addWidget(readableScrollArea, 1);

    QWidget* readableContent = new QWidget(readableScrollArea);
    QVBoxLayout* readableLayout = new QVBoxLayout(readableContent);
    readableLayout->setContentsMargins(12, 12, 12, 12);
    readableLayout->setSpacing(10);

    QLabel* readableOverviewLabel = new QLabel(readableContent);
    readableOverviewLabel->setObjectName(QStringLiteral("vtOverviewCard"));
    readableOverviewLabel->setTextFormat(Qt::RichText);
    readableOverviewLabel->setWordWrap(true);
    readableOverviewLabel->setText(QStringLiteral(
        "<table cellspacing='0' cellpadding='0'>"
        "<tr>"
        "<td width='124'><img src=':/Icon/vt_status_undetected.svg' width='104' height='104'/></td>"
        "<td>"
        "<div style='font-size:22px;font-weight:700;'>%1</div>"
        "<div style='margin-top:6px;color:#3B82F6;font-size:18px;font-weight:700;'>未检出</div>"
        "<div style='margin-top:6px;'>等待 VirusTotal 返回分析结果</div>"
        "</td>"
        "</tr>"
        "</table>")
        .arg(QFileInfo(m_filePath).fileName().toHtmlEscaped()));
    readableLayout->addWidget(readableOverviewLabel, 0);

    QGroupBox* fileInfoGroup = new QGroupBox(QStringLiteral("基础信息 / HASH"), readableContent);
    QVBoxLayout* fileInfoLayout = new QVBoxLayout(fileInfoGroup);
    fileInfoLayout->setContentsMargins(0, 2, 0, 2);
    fileInfoLayout->setSpacing(0);
    QTableWidget* fileInfoTable = new QTableWidget(fileInfoGroup);
    fileInfoTable->setObjectName(QStringLiteral("vtFileInfoTable"));
    fileInfoTable->setColumnCount(2);
    configureBorderlessInfoTable(fileInfoTable);
    fileInfoTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    fileInfoTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    setTableRow(fileInfoTable, 0, QStringLiteral("文件名"), QFileInfo(m_filePath).fileName());
    setTableRow(fileInfoTable, 1, QStringLiteral("文件大小"), formatByteCount(QFileInfo(m_filePath).size()));
    fitTableHeightToRows(fileInfoTable);
    fileInfoLayout->addWidget(fileInfoTable, 1);
    readableLayout->addWidget(fileInfoGroup, 0);

    QGroupBox* engineGroup = new QGroupBox(QStringLiteral("多引擎检测"), readableContent);
    QVBoxLayout* engineLayout = new QVBoxLayout(engineGroup);
    QTableWidget* engineTable = new QTableWidget(engineGroup);
    engineTable->setColumnCount(4);
    engineTable->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("引擎")
        << QStringLiteral("结果")
        << QStringLiteral("引擎")
        << QStringLiteral("结果"));
    configureReadOnlyTable(engineTable);
    engineTable->horizontalHeader()->setVisible(false);
    engineTable->setSortingEnabled(false);
    engineTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    engineTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    engineTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    engineTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    engineLayout->addWidget(engineTable, 1);
    readableLayout->addWidget(engineGroup, 3);

    QGroupBox* staticGroup = new QGroupBox(QStringLiteral("静态分析 / 可展开详情"), readableContent);
    QVBoxLayout* staticLayout = new QVBoxLayout(staticGroup);
    QTreeWidget* staticAnalysisTree = new QTreeWidget(staticGroup);
    staticAnalysisTree->setColumnCount(2);
    staticAnalysisTree->setHeaderLabels(QStringList() << QStringLiteral("字段") << QStringLiteral("值"));
    staticAnalysisTree->setAlternatingRowColors(true);
    staticAnalysisTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    staticAnalysisTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    staticAnalysisTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    staticLayout->addWidget(staticAnalysisTree, 1);
    readableLayout->addWidget(staticGroup, 2);
    readableScrollArea->setWidget(readableContent);

    QWidget* responsePage = new QWidget(resultTabWidget);
    QVBoxLayout* responseLayout = new QVBoxLayout(responsePage);
    responseLayout->setContentsMargins(8, 8, 8, 8);
    QTreeWidget* responseTree = new QTreeWidget(responsePage);
    responseTree->setColumnCount(3);
    responseTree->setHeaderLabels(QStringList()
        << QStringLiteral("字段 / 响应阶段")
        << QStringLiteral("值")
        << QStringLiteral("时间(UTC)"));
    responseTree->setAlternatingRowColors(true);
    responseTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    responseTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    responseTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    responseTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    responseLayout->addWidget(responseTree, 1);

    QWidget* rawPage = new QWidget(resultTabWidget);
    QVBoxLayout* rawLayout = new QVBoxLayout(rawPage);
    rawLayout->setContentsMargins(8, 8, 8, 8);
    CodeEditorWidget* resultEditor = new CodeEditorWidget(resultDialog);
    resultEditor->setReadOnly(true);
    resultEditor->setText(m_resultRawText);
    rawLayout->addWidget(resultEditor, 1);

    resultTabWidget->addTab(readablePage, QStringLiteral("报告视图"));
    resultTabWidget->addTab(responsePage, QStringLiteral("响应详情"));
    resultTabWidget->addTab(rawPage, QStringLiteral("原始数据"));

    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, resultDialog);
    QPushButton* copyButton = buttonBox->addButton(QStringLiteral("复制原始数据"), QDialogButtonBox::ActionRole);
    QPushButton* exportButton = buttonBox->addButton(QStringLiteral("导出原始数据"), QDialogButtonBox::ActionRole);

    QObject::connect(copyButton, &QPushButton::clicked, this, [this]()
        {
            // 输入：当前累计的原始响应文本。
            // 处理：写入系统剪贴板，便于分析人员粘贴到外部工具。
            // 返回：无。
            if (QApplication::clipboard() != nullptr)
            {
                QApplication::clipboard()->setText(m_resultRawText);
            }
        });
    QObject::connect(exportButton, &QPushButton::clicked, this, [this, resultDialog]()
        {
            // 输入：当前累计原始响应文本、结构化响应数组和样本文件名。
            // 处理：弹出保存对话框并写入 UTF-8 JSON 文件，raw_text 字段保留窗口原文。
            // 返回：无；失败时通过弹窗提示。
            const QString defaultName = QStringLiteral("virustotal_%1_%2.json")
                .arg(QFileInfo(m_filePath).completeBaseName().isEmpty()
                    ? QStringLiteral("sample")
                    : QFileInfo(m_filePath).completeBaseName())
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
            const QString savePath = QFileDialog::getSaveFileName(
                resultDialog,
                QStringLiteral("导出 VirusTotal 原始数据"),
                QDir(QDir::homePath()).absoluteFilePath(defaultName),
                QStringLiteral("JSON Files (*.json);;Text Files (*.txt);;All Files (*)"));
            if (savePath.isEmpty())
            {
                return;
            }

            QFile outputFile(savePath);
            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            {
                QMessageBox::warning(
                    resultDialog,
                    QStringLiteral("导出 VirusTotal 原始数据"),
                    QStringLiteral("无法写入文件：%1\n%2").arg(savePath, outputFile.errorString()));
                return;
            }
            outputFile.write(buildRawExportJson());
            outputFile.close();
        });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, resultDialog, &QDialog::close);
    dialogLayout->addWidget(buttonBox, 0);

    m_resultDialog = resultDialog;
    m_resultSummaryLabel.clear();
    m_resultTabWidget = resultTabWidget;
    m_readableOverviewLabel = readableOverviewLabel;
    m_fileInfoTable = fileInfoTable;
    m_engineTable = engineTable;
    m_staticAnalysisTree = staticAnalysisTree;
    m_responseTree = responseTree;
    m_resultEditor = resultEditor;
    QObject::connect(resultDialog, &QObject::destroyed, this, [this]()
        {
            // 输入：实时结果窗口 destroyed 信号。
            // 处理：清空弱指针，并在 scanFileAndAutoDelete 场景中释放扫描对象。
            // 返回：无；deleteLater 交给 Qt 事件循环执行。
            m_resultDialog.clear();
            m_resultSummaryLabel.clear();
            m_resultTabWidget.clear();
            m_readableOverviewLabel.clear();
            m_fileInfoTable.clear();
            m_engineTable.clear();
            m_staticAnalysisTree.clear();
            m_responseTree.clear();
            m_resultEditor.clear();
            if (m_deleteAfterResultDialogClosed)
            {
                m_deleteAfterResultDialogClosed = false;
                deleteLater();
            }
        });
    resultDialog->show();
    resultDialog->raise();
    resultDialog->activateWindow();
}

void VirusTotalOnlineScan::appendRawJsonSection(const QString& titleText, const QJsonObject& jsonObject)
{
    ensureResultDialog();
    const QString timestampText = utcTimestampText();
    QJsonObject sectionObject;
    sectionObject.insert(QStringLiteral("timestamp_utc"), timestampText);
    sectionObject.insert(QStringLiteral("title"), titleText);
    sectionObject.insert(QStringLiteral("kind"), QStringLiteral("json"));
    sectionObject.insert(QStringLiteral("body"), jsonObject);
    m_resultRawSections.append(sectionObject);

    m_resultRawText += QStringLiteral("\n===== %1 | %2 =====\n")
        .arg(timestampText, titleText);
    m_resultRawText += ks::online_scan::formatJsonObject(jsonObject);
    m_resultRawText += QLatin1Char('\n');
    if (!m_resultEditor.isNull())
    {
        m_resultEditor->setText(m_resultRawText);
    }
    appendResponseTreeJsonSection(titleText, timestampText, jsonObject);
}

void VirusTotalOnlineScan::appendRawTextSection(const QString& titleText, const QString& detailText)
{
    ensureResultDialog();
    const QString timestampText = utcTimestampText();
    QJsonObject sectionObject;
    sectionObject.insert(QStringLiteral("timestamp_utc"), timestampText);
    sectionObject.insert(QStringLiteral("title"), titleText);
    sectionObject.insert(QStringLiteral("kind"), QStringLiteral("text"));
    sectionObject.insert(QStringLiteral("body"), detailText);
    m_resultRawSections.append(sectionObject);

    m_resultRawText += QStringLiteral("\n===== %1 | %2 =====\n")
        .arg(timestampText, titleText);
    m_resultRawText += detailText;
    m_resultRawText += QLatin1Char('\n');
    if (!m_resultEditor.isNull())
    {
        m_resultEditor->setText(m_resultRawText);
    }
    appendResponseTreeTextSection(titleText, timestampText, detailText);
}

void VirusTotalOnlineScan::appendRawReplyBodySection(const QString& titleText, const QByteArray& bodyBytes)
{
    // 输入：HTTP 完整响应体，可能是 VirusTotal JSON 错误，也可能是网关/代理返回的纯文本。
    // 处理：优先按 JSON 对象保存；无法解析时按 UTF-8 文本保存，空响应体也显式记录。
    // 返回：无；所有数据追加到实时窗口和 m_resultRawSections，供导出 JSON 使用。
    if (bodyBytes.isEmpty())
    {
        appendRawTextSection(titleText, QStringLiteral("<空响应体>"));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(bodyBytes, &parseError);
    if (parseError.error == QJsonParseError::NoError && jsonDocument.isObject())
    {
        appendRawJsonSection(titleText, jsonDocument.object());
        return;
    }

    appendRawTextSection(titleText, QString::fromUtf8(bodyBytes));
}

QByteArray VirusTotalOnlineScan::buildRawExportJson() const
{
    // rootObject 结构：
    // - metadata：上传来源、样本文件、analysis id、导出时间等上下文；
    // - responses：每次返回/错误的原始 JSON 或文本；
    // - raw_text：实时窗口正文原文，方便人工核对导出没有遗漏。
    QJsonObject metadataObject;
    metadataObject.insert(QStringLiteral("service"), QStringLiteral("VirusTotal"));
    metadataObject.insert(QStringLiteral("source"), m_sourceText);
    metadataObject.insert(QStringLiteral("file_path"), QDir::toNativeSeparators(m_filePath));
    metadataObject.insert(QStringLiteral("file_name"), QFileInfo(m_filePath).fileName());
    metadataObject.insert(QStringLiteral("analysis_id"), m_analysisId);
    metadataObject.insert(QStringLiteral("poll_attempts"), m_pollAttempt);
    metadataObject.insert(QStringLiteral("scan_in_progress"), m_scanInProgress);
    metadataObject.insert(QStringLiteral("exported_at_utc"), utcTimestampText());

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("metadata"), metadataObject);
    rootObject.insert(QStringLiteral("responses"), m_resultRawSections);
    rootObject.insert(QStringLiteral("raw_text"), m_resultRawText);
    return QJsonDocument(rootObject).toJson(QJsonDocument::Indented);
}

void VirusTotalOnlineScan::updateResultSummary(const QString& summaryText)
{
    ensureResultDialog();
    if (!m_resultSummaryLabel.isNull())
    {
        m_resultSummaryLabel->setText(summaryText);
    }
}

void VirusTotalOnlineScan::finalizeAutoDeleteIfNeeded()
{
    if (!m_autoDeleteWhenFinished)
    {
        return;
    }

    // 自动删除场景中，结果窗口按钮仍然需要读取 m_resultRawText 和 m_filePath。
    // 因此只要窗口存在，就把对象生命周期延长到窗口关闭；如果窗口尚未创建或已关闭，则直接释放对象。
    if (!m_resultDialog.isNull())
    {
        m_deleteAfterResultDialogClosed = true;
        return;
    }

    deleteLater();
}

QString VirusTotalOnlineScan::buildResultSummary(const QJsonObject& analysisObject) const
{
    const QJsonObject dataObject = analysisObject.value(QStringLiteral("data")).toObject();
    const QJsonObject attributesObject = dataObject.value(QStringLiteral("attributes")).toObject();
    const QJsonObject statsObject = attributesObject.value(QStringLiteral("stats")).toObject();

    // statsObject 作用：承载 VirusTotal 多引擎统计，缺失时仍返回基础 analysis id。
    const int maliciousCount = jsonValueToInt(statsObject, QStringLiteral("malicious"));
    const int suspiciousCount = jsonValueToInt(statsObject, QStringLiteral("suspicious"));
    const int harmlessCount = jsonValueToInt(statsObject, QStringLiteral("harmless"));
    const int undetectedCount = jsonValueToInt(statsObject, QStringLiteral("undetected"));
    const int timeoutCount = jsonValueToInt(statsObject, QStringLiteral("timeout"));
    const int failureCount = jsonValueToInt(statsObject, QStringLiteral("failure"));
    const int unsupportedCount = jsonValueToInt(statsObject, QStringLiteral("type-unsupported"));

    QStringList summaryLines;
    summaryLines << QStringLiteral("文件：%1").arg(QFileInfo(m_filePath).fileName());
    summaryLines << QStringLiteral("AnalysisId：%1").arg(m_analysisId);
    summaryLines << QStringLiteral("状态：%1").arg(attributesObject.value(QStringLiteral("status")).toString(QStringLiteral("未知")));
    summaryLines << QStringLiteral("恶意：%1，可疑：%2，无害：%3，未检出：%4")
        .arg(maliciousCount)
        .arg(suspiciousCount)
        .arg(harmlessCount)
        .arg(undetectedCount);
    summaryLines << QStringLiteral("超时：%1，失败：%2，不支持类型：%3")
        .arg(timeoutCount)
        .arg(failureCount)
        .arg(unsupportedCount);
    return summaryLines.join(QChar('\n'));
}

void VirusTotalOnlineScan::refreshReadableResult(const QJsonObject& analysisObject)
{
    // 输入：VirusTotal /analyses/{id} 的最终 JSON。
    // 处理：从固定字段 data.attributes.stats/results 与 meta.file_info 提取报告视图数据；
    //      检出项按恶意/可疑优先排序，静态分析树保留可展开的关键原始字段。
    // 返回：无；只刷新已创建的报告页控件。
    ensureResultDialog();

    const QJsonObject dataObject = analysisObject.value(QStringLiteral("data")).toObject();
    const QJsonObject attributesObject = dataObject.value(QStringLiteral("attributes")).toObject();
    const QJsonObject statsObject = attributesObject.value(QStringLiteral("stats")).toObject();
    const QJsonObject resultsObject = attributesObject.value(QStringLiteral("results")).toObject();
    const QJsonObject metaObject = analysisObject.value(QStringLiteral("meta")).toObject();
    const QJsonObject fileInfoObject = metaObject.value(QStringLiteral("file_info")).toObject();
    const QJsonObject linksObject = dataObject.value(QStringLiteral("links")).toObject();

    const int maliciousCount = jsonValueToInt(statsObject, QStringLiteral("malicious"));
    const int suspiciousCount = jsonValueToInt(statsObject, QStringLiteral("suspicious"));
    const int harmlessCount = jsonValueToInt(statsObject, QStringLiteral("harmless"));
    const int undetectedCount = jsonValueToInt(statsObject, QStringLiteral("undetected"));
    const int timeoutCount = jsonValueToInt(statsObject, QStringLiteral("timeout"));
    const int confirmedTimeoutCount = jsonValueToInt(statsObject, QStringLiteral("confirmed-timeout"));
    const int failureCount = jsonValueToInt(statsObject, QStringLiteral("failure"));
    const int unsupportedCount = jsonValueToInt(statsObject, QStringLiteral("type-unsupported"));
    const int detectedCount = maliciousCount + suspiciousCount;
    const int engineTotal = maliciousCount + suspiciousCount + harmlessCount + undetectedCount +
        timeoutCount + confirmedTimeoutCount + failureCount + unsupportedCount;

    const QString statusText = attributesObject.value(QStringLiteral("status")).toString(QStringLiteral("未知"));
    const QString sha256Text = fileInfoObject.value(QStringLiteral("sha256")).toString();
    const QString sha1Text = fileInfoObject.value(QStringLiteral("sha1")).toString();
    const QString md5Text = fileInfoObject.value(QStringLiteral("md5")).toString();
    const qint64 vtSizeBytes = static_cast<qint64>(fileInfoObject.value(QStringLiteral("size")).toDouble(-1.0));
    const QFileInfo localFileInfo(m_filePath);
    const qint64 displaySizeBytes = vtSizeBytes >= 0 ? vtSizeBytes : localFileInfo.size();

    const ReportVerdict verdict = reportVerdictFromStats(maliciousCount, suspiciousCount, harmlessCount);

    if (!m_readableOverviewLabel.isNull())
    {
        const QString verdictColorText = verdict.accentColor.isValid()
            ? verdict.accentColor.name(QColor::HexRgb)
            : KswordTheme::TextPrimaryColorHex();
        m_readableOverviewLabel->setText(QStringLiteral(
            "<table cellspacing='0' cellpadding='0'>"
            "<tr>"
            "<td width='124'><img src='%1' width='104' height='104'/></td>"
            "<td>"
            "<div style='font-size:22px;font-weight:700;'>%2</div>"
            "<div style='margin-top:6px;'>"
            "<span style='color:%3;font-size:18px;font-weight:700;'>%4</span>"
            "&nbsp;&nbsp; 检出率：<span style='color:%3;font-weight:700;'>%5 / %6</span>"
            "&nbsp;&nbsp; 状态：%7"
            "</div>"
            "<div style='margin-top:6px;'>AnalysisId：%8</div>"
            "</td>"
            "</tr>"
            "</table>")
            .arg(verdict.iconPath)
            .arg(localFileInfo.fileName().toHtmlEscaped().isEmpty()
                ? QStringLiteral("<未知文件>")
                : localFileInfo.fileName().toHtmlEscaped())
            .arg(verdictColorText)
            .arg(verdict.labelText.toHtmlEscaped())
            .arg(detectedCount)
            .arg(engineTotal)
            .arg(statusText.toHtmlEscaped())
            .arg(m_analysisId.toHtmlEscaped()));
    }

    if (!m_fileInfoTable.isNull())
    {
        m_fileInfoTable->setRowCount(0);
        setTableRow(m_fileInfoTable, 0, QStringLiteral("文件名"), localFileInfo.fileName());
        setTableRow(m_fileInfoTable, 1, QStringLiteral("文件大小"), formatByteCount(displaySizeBytes));
        setTableRow(m_fileInfoTable, 2, QStringLiteral("SHA256"), sha256Text.isEmpty() ? QStringLiteral("-") : sha256Text);
        setTableRow(m_fileInfoTable, 3, QStringLiteral("SHA1"), sha1Text.isEmpty() ? QStringLiteral("-") : sha1Text);
        setTableRow(m_fileInfoTable, 4, QStringLiteral("MD5"), md5Text.isEmpty() ? QStringLiteral("-") : md5Text);
        setTableRow(m_fileInfoTable, 5, QStringLiteral("AnalysisId"), dataObject.value(QStringLiteral("id")).toString(m_analysisId));
        setTableRow(m_fileInfoTable, 6, QStringLiteral("分析时间"), unixDateText(static_cast<qint64>(attributesObject.value(QStringLiteral("date")).toDouble(0.0))));
        setTableRow(m_fileInfoTable, 7, QStringLiteral("VT 文件链接"), linksObject.value(QStringLiteral("item")).toString(QStringLiteral("-")));
        fitTableHeightToRows(m_fileInfoTable);
    }

    if (!m_engineTable.isNull())
    {
        struct EngineRow
        {
            QString engineText;
            QString categoryRawText;
            QString resultText;
            QString versionText;
            QString updateText;
        };
        std::vector<EngineRow> engineRows;
        engineRows.reserve(static_cast<std::size_t>(resultsObject.size()));
        for (auto iterator = resultsObject.constBegin(); iterator != resultsObject.constEnd(); ++iterator)
        {
            const QJsonObject engineObject = iterator.value().toObject();
            EngineRow row;
            row.engineText = engineObject.value(QStringLiteral("engine_name")).toString(iterator.key());
            row.categoryRawText = engineObject.value(QStringLiteral("category")).toString();
            row.resultText = engineObject.value(QStringLiteral("result")).toString();
            row.versionText = engineObject.value(QStringLiteral("engine_version")).toString();
            row.updateText = engineObject.value(QStringLiteral("engine_update")).toString();
            engineRows.push_back(row);
        }
        std::sort(engineRows.begin(), engineRows.end(), [](const EngineRow& left, const EngineRow& right)
            {
                const int leftPriority = categoryPriority(left.categoryRawText);
                const int rightPriority = categoryPriority(right.categoryRawText);
                if (leftPriority != rightPriority)
                {
                    return leftPriority < rightPriority;
                }
                return QString::localeAwareCompare(left.engineText, right.engineText) < 0;
            });

        const int tableRowCount = static_cast<int>((engineRows.size() + 1) / 2);
        m_engineTable->clearContents();
        m_engineTable->setRowCount(tableRowCount);
        for (int visibleIndex = 0; visibleIndex < static_cast<int>(engineRows.size()); ++visibleIndex)
        {
            const EngineRow& row = engineRows[static_cast<std::size_t>(visibleIndex)];
            const QColor rowColor = categoryColor(row.categoryRawText);
            const QString versionText = row.versionText.trimmed().isEmpty()
                ? QStringLiteral("-")
                : row.versionText.trimmed();
            const QString updateText = row.updateText.trimmed().isEmpty()
                ? QStringLiteral("-")
                : row.updateText.trimmed();
            const QString tooltipText = QStringLiteral("版本：%1\n更新时间：%2").arg(versionText, updateText);
            const int tableRow = visibleIndex / 2;
            const int tableColumn = (visibleIndex % 2) * 2;

            QTableWidgetItem* engineItem = createReadOnlyTableItem(row.engineText);
            QTableWidgetItem* resultItem = createReadOnlyTableItem(
                engineDetectionText(row.categoryRawText, row.resultText),
                rowColor);
            engineItem->setToolTip(tooltipText);
            resultItem->setToolTip(tooltipText);
            m_engineTable->setItem(tableRow, tableColumn, engineItem);
            m_engineTable->setItem(tableRow, tableColumn + 1, resultItem);
            m_engineTable->setRowHeight(tableRow, 22);
        }
    }

    if (!m_staticAnalysisTree.isNull())
    {
        m_staticAnalysisTree->clear();
        QTreeWidgetItem* basicItem = new QTreeWidgetItem(m_staticAnalysisTree);
        basicItem->setText(0, QStringLiteral("基础信息"));
        basicItem->setText(1, QStringLiteral("文件与 Analysis 上下文"));
        addTreeLeaf(basicItem, QStringLiteral("文件名"), localFileInfo.fileName());
        addTreeLeaf(basicItem, QStringLiteral("大小"), formatByteCount(displaySizeBytes));
        addTreeLeaf(basicItem, QStringLiteral("SHA256"), sha256Text.isEmpty() ? QStringLiteral("-") : sha256Text);
        addTreeLeaf(basicItem, QStringLiteral("SHA1"), sha1Text.isEmpty() ? QStringLiteral("-") : sha1Text);
        addTreeLeaf(basicItem, QStringLiteral("MD5"), md5Text.isEmpty() ? QStringLiteral("-") : md5Text);

        QTreeWidgetItem* analysisItem = new QTreeWidgetItem(m_staticAnalysisTree);
        analysisItem->setText(0, QStringLiteral("分析任务"));
        analysisItem->setText(1, QStringLiteral("VirusTotal analysis"));
        addTreeLeaf(analysisItem, QStringLiteral("AnalysisId"), dataObject.value(QStringLiteral("id")).toString(m_analysisId));
        addTreeLeaf(analysisItem, QStringLiteral("状态"), statusText);
        addTreeLeaf(analysisItem, QStringLiteral("提交/分析时间"), unixDateText(static_cast<qint64>(attributesObject.value(QStringLiteral("date")).toDouble(0.0))));
        addTreeLeaf(analysisItem, QStringLiteral("Self 链接"), linksObject.value(QStringLiteral("self")).toString(QStringLiteral("-")));
        addTreeLeaf(analysisItem, QStringLiteral("File 链接"), linksObject.value(QStringLiteral("item")).toString(QStringLiteral("-")));

        QTreeWidgetItem* statsItem = new QTreeWidgetItem(m_staticAnalysisTree);
        statsItem->setText(0, QStringLiteral("统计字段"));
        statsItem->setText(1, QStringLiteral("data.attributes.stats"));
        appendJsonValueToTree(statsItem, QStringLiteral("stats"), statsObject);

        QTreeWidgetItem* rawAttributesItem = new QTreeWidgetItem(m_staticAnalysisTree);
        rawAttributesItem->setText(0, QStringLiteral("原始 attributes"));
        rawAttributesItem->setText(1, QStringLiteral("可展开查看 VT 固定字段"));
        appendJsonValueToTree(rawAttributesItem, QStringLiteral("attributes"), attributesObject);

        basicItem->setExpanded(true);
        analysisItem->setExpanded(true);
        statsItem->setExpanded(true);
        m_staticAnalysisTree->resizeColumnToContents(0);
    }

    if (!m_resultTabWidget.isNull())
    {
        m_resultTabWidget->setCurrentIndex(0);
    }
}

void VirusTotalOnlineScan::appendResponseTreeJsonSection(
    const QString& titleText,
    const QString& timestampText,
    const QJsonObject& jsonObject)
{
    // 输入：一个已解析的 VT JSON 响应。
    // 处理：追加为响应详情页顶层节点，下级递归展开 JSON 字段。
    // 返回：无。
    if (m_responseTree.isNull())
    {
        return;
    }

    QTreeWidgetItem* sectionItem = new QTreeWidgetItem(m_responseTree);
    sectionItem->setText(0, titleText);
    sectionItem->setText(1, QStringLiteral("{ %1 fields }").arg(jsonObject.size()));
    sectionItem->setText(2, timestampText);
    sectionItem->setExpanded(false);
    for (auto iterator = jsonObject.constBegin(); iterator != jsonObject.constEnd(); ++iterator)
    {
        appendJsonValueToTree(sectionItem, iterator.key(), iterator.value());
    }
    m_responseTree->scrollToItem(sectionItem);
}

void VirusTotalOnlineScan::appendResponseTreeTextSection(
    const QString& titleText,
    const QString& timestampText,
    const QString& detailText)
{
    // 输入：一个非 JSON 响应或本地错误详情。
    // 处理：追加为响应详情页顶层节点，正文放在子节点中，避免撑开表头。
    // 返回：无。
    if (m_responseTree.isNull())
    {
        return;
    }

    QTreeWidgetItem* sectionItem = new QTreeWidgetItem(m_responseTree);
    sectionItem->setText(0, titleText);
    sectionItem->setText(1, QStringLiteral("text"));
    sectionItem->setText(2, timestampText);
    addTreeLeaf(sectionItem, QStringLiteral("详情"), detailText);
    sectionItem->setExpanded(true);
    m_responseTree->scrollToItem(sectionItem);
}
