#include "ThreatBookScan.h"

#include "OnlineScanSupport.h"
#include "../Framework.h"
#include "../SettingsDock/AppearanceSettings.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include <algorithm>

namespace
{
    // ThreatBook API 常量：集中维护官方 v3 端点，避免散落在业务逻辑中。
    constexpr const char* kThreatBookUploadEndpoint = "https://api.threatbook.cn/v3/file/upload";
    constexpr const char* kThreatBookReportEndpoint = "https://api.threatbook.cn/v3/file/report";
    constexpr int kInitialPollDelayMs = 15000;
    constexpr int kPollIntervalMs = 15000;
    constexpr int kMaxPollAttempts = 40;

    // addFormField 作用：
    // - 给 multipart 请求追加普通文本字段；
    // - ThreatBook file/upload 的 apikey 字段会调用该函数。
    // 入参 multiPart：multipart 请求体。
    // 入参 fieldName：字段名。
    // 入参 fieldValue：字段值。
    // 返回：无。
    void addFormField(QHttpMultiPart* multiPart, const QString& fieldName, const QString& fieldValue)
    {
        if (multiPart == nullptr)
        {
            return;
        }
        QHttpPart formPart;
        formPart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant(QStringLiteral("form-data; name=\"%1\"").arg(fieldName)));
        formPart.setBody(fieldValue.toUtf8());
        multiPart->append(formPart);
    }

    // threatBookResponseCode 作用：
    // - 读取 ThreatBook 响应中的 response_code；
    // - 字段缺失时返回 -999999，便于和正常 0 区分。
    // 入参 rootObject：响应 JSON 根对象。
    // 返回：response_code 数值。
    int threatBookResponseCode(const QJsonObject& rootObject)
    {
        return rootObject.value(QStringLiteral("response_code")).toInt(-999999);
    }

    // threatBookVerboseMessage 作用：
    // - 读取 ThreatBook 响应中的 verbose_msg 或 message；
    // - 用于错误弹窗和“仍在分析”状态识别。
    // 入参 rootObject：响应 JSON 根对象。
    // 返回：服务端消息文本。
    QString threatBookVerboseMessage(const QJsonObject& rootObject)
    {
        const QString verboseText = rootObject.value(QStringLiteral("verbose_msg")).toString().trimmed();
        if (!verboseText.isEmpty())
        {
            return verboseText;
        }
        return rootObject.value(QStringLiteral("message")).toString().trimmed();
    }

    // jsonContainsPendingHint 作用：
    // - 从响应 JSON 文本中识别 ThreatBook 正在排队/分析的状态；
    // - 官方文档示例会出现 In Progress，中文环境也可能返回正在分析。
    // 入参 rootObject：响应 JSON 根对象。
    // 返回：true=仍需继续轮询；false=不是等待状态。
    bool jsonContainsPendingHint(const QJsonObject& rootObject)
    {
        const QString compactText = QString::fromUtf8(
            QJsonDocument(rootObject).toJson(QJsonDocument::Compact)).toLower();
        return compactText.contains(QStringLiteral("in progress"))
            || compactText.contains(QStringLiteral("processing"))
            || compactText.contains(QStringLiteral("running"))
            || compactText.contains(QStringLiteral("queue"))
            || compactText.contains(QStringLiteral("analy"))
            || compactText.contains(QStringLiteral("正在"))
            || compactText.contains(QStringLiteral("排队"))
            || compactText.contains(QStringLiteral("分析中"));
    }

    // valueToDisplayText 作用：
    // - 将 ThreatBook summary 中可能为字符串、数字、数组的字段转成展示文本；
    // - 摘要弹窗避免直接显示 QVariant 类型噪声。
    // 入参 value：JSON 值。
    // 返回：用户可读文本。
    QString valueToDisplayText(const QJsonValue& value)
    {
        if (value.isString())
        {
            return value.toString();
        }
        if (value.isDouble())
        {
            return QString::number(value.toDouble(), 'f', 2);
        }
        if (value.isBool())
        {
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        }
        if (value.isArray())
        {
            QStringList itemTexts;
            const QJsonArray arrayValue = value.toArray();
            for (const QJsonValue& itemValue : arrayValue)
            {
                itemTexts << valueToDisplayText(itemValue);
            }
            return itemTexts.join(QStringLiteral(", "));
        }
        if (value.isObject())
        {
            return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
        }
        return QStringLiteral("-");
    }
}

ThreatBookScan::ThreatBookScan(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
}

ThreatBookScan::~ThreatBookScan() = default;

void ThreatBookScan::scanFile(const QString& filePath, QWidget* dialogParent)
{
    if (m_scanInProgress)
    {
        ks::online_scan::showErrorDialog(
            dialogParent,
            QStringLiteral("ThreatBook 在线扫描"),
            QStringLiteral("当前 ThreatBook 扫描仍在进行，请等待完成后再上传新文件。"));
        return;
    }

    resetRuntimeState();
    m_dialogParent = dialogParent;
    m_filePath = filePath.trimmed();

    // settings 作用：从统一设置 JSON 读取 API Key，未配置时直接提示用户补充。
    const ks::settings::AppearanceSettings settings = ks::settings::loadAppearanceSettings();
    m_apiKey = settings.threatBookApiKey.trimmed();
    if (m_apiKey.isEmpty())
    {
        ks::online_scan::showMissingApiKeyDialog(dialogParent, QStringLiteral("ThreatBook（微步在线）"));
        return;
    }

    QString fileErrorText;
    if (!ks::online_scan::validateReadableFile(
        m_filePath,
        ks::online_scan::kThreatBookUploadMaxBytes,
        &fileErrorText))
    {
        ks::online_scan::showErrorDialog(dialogParent, QStringLiteral("ThreatBook 在线扫描"), fileErrorText);
        return;
    }

    QString hashErrorText;
    m_sha256Text = ks::online_scan::calculateSha256Hex(m_filePath, &hashErrorText);
    if (m_sha256Text.isEmpty())
    {
        ks::online_scan::showErrorDialog(dialogParent, QStringLiteral("ThreatBook 在线扫描"), hashErrorText);
        return;
    }

    m_scanInProgress = true;
    m_progressPid = kPro.add("在线扫描", "ThreatBook 上传准备");
    kPro.set(m_progressPid, "ThreatBook：准备上传样本", 0, 5.0f);
    uploadFile();
}

void ThreatBookScan::scanFileAndAutoDelete(const QString& filePath, QWidget* dialogParent)
{
    ThreatBookScan* scanner = new ThreatBookScan(dialogParent);
    scanner->m_autoDeleteWhenFinished = true;
    scanner->scanFile(filePath, dialogParent);
    if (!scanner->m_scanInProgress)
    {
        scanner->deleteLater();
    }
}

void ThreatBookScan::uploadFile()
{
    QFile* uploadFileObject = new QFile(m_filePath);
    if (!uploadFileObject->open(QIODevice::ReadOnly))
    {
        const QString errorText = QStringLiteral("打开上传文件失败：%1").arg(uploadFileObject->errorString());
        uploadFileObject->deleteLater();
        finishWithError(QStringLiteral("ThreatBook 上传失败"), errorText);
        return;
    }

    // multiPart 作用：构造 ThreatBook file/upload 所需 multipart/form-data 请求体。
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    addFormField(multiPart, QStringLiteral("apikey"), m_apiKey);

    QHttpPart filePart;
    const QString safeFileName = ks::online_scan::sanitizeFileNameForContentDisposition(QFileInfo(m_filePath).fileName());
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(safeFileName)));
    filePart.setBodyDevice(uploadFileObject);
    uploadFileObject->setParent(multiPart);
    multiPart->append(filePart);

    QNetworkRequest request(QUrl(QString::fromLatin1(kThreatBookUploadEndpoint)));
    request.setRawHeader("User-Agent", "Ksword5.1-OnlineScan/1.0");

    kPro.set(m_progressPid, "ThreatBook：上传样本", 0, 12.0f);
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
            kPro.set(m_progressPid, "ThreatBook：上传样本", 0, 12.0f + std::min(uploadPercent, 40.0f));
        });
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            handleUploadReply(reply);
        });
}

void ThreatBookScan::handleUploadReply(QNetworkReply* reply)
{
    const QByteArray bodyBytes = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;

    if (!networkOk)
    {
        const QString errorText = ks::online_scan::networkReplyErrorText(reply, bodyBytes);
        reply->deleteLater();
        finishWithError(
            QStringLiteral("ThreatBook 上传失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
    if (!parseErrorText.isEmpty())
    {
        finishWithError(QStringLiteral("ThreatBook 上传失败"), parseErrorText);
        return;
    }

    const int responseCode = threatBookResponseCode(rootObject);
    const QString verboseText = threatBookVerboseMessage(rootObject);
    if (responseCode != 0 && !jsonContainsPendingHint(rootObject))
    {
        finishWithError(
            QStringLiteral("ThreatBook 上传失败"),
            QStringLiteral("response_code=%1\n%2\n\n%3")
                .arg(responseCode)
                .arg(verboseText.isEmpty() ? QStringLiteral("服务端未返回详细消息。") : verboseText)
                .arg(ks::online_scan::formatJsonObject(rootObject)));
        return;
    }

    const QJsonObject dataObject = rootObject.value(QStringLiteral("data")).toObject();
    const QString remoteSha256Text = dataObject.value(QStringLiteral("sha256")).toString().trimmed();
    if (!remoteSha256Text.isEmpty())
    {
        m_sha256Text = remoteSha256Text;
    }

    kPro.set(m_progressPid, "ThreatBook：等待报告生成", 0, 55.0f);
    scheduleReportPoll(kInitialPollDelayMs);
}

void ThreatBookScan::scheduleReportPoll(const int delayMs)
{
    QTimer::singleShot(delayMs, this, [this]()
        {
            requestReport();
        });
}

void ThreatBookScan::requestReport()
{
    if (!m_scanInProgress || m_sha256Text.isEmpty())
    {
        return;
    }

    ++m_pollAttempt;
    const float progressValue = std::min(95.0f, 55.0f + static_cast<float>(m_pollAttempt) * 1.0f);
    kPro.set(
        m_progressPid,
        QStringLiteral("ThreatBook：轮询报告(%1/%2)").arg(m_pollAttempt).arg(kMaxPollAttempts).toStdString(),
        0,
        progressValue);

    QUrl reportUrl(QString::fromLatin1(kThreatBookReportEndpoint));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("apikey"), m_apiKey);
    query.addQueryItem(QStringLiteral("resource"), m_sha256Text);
    query.addQueryItem(QStringLiteral("query_fields"), QStringLiteral("summary"));
    query.addQueryItem(QStringLiteral("query_fields"), QStringLiteral("multiengines"));
    reportUrl.setQuery(query);

    QNetworkRequest request(reportUrl);
    request.setRawHeader("User-Agent", "Ksword5.1-OnlineScan/1.0");
    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            handleReportReply(reply);
        });
}

void ThreatBookScan::handleReportReply(QNetworkReply* reply)
{
    const QByteArray bodyBytes = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;

    if (!networkOk)
    {
        const QString errorText = ks::online_scan::networkReplyErrorText(reply, bodyBytes);
        reply->deleteLater();
        finishWithError(
            QStringLiteral("ThreatBook 查询失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
    if (!parseErrorText.isEmpty())
    {
        finishWithError(QStringLiteral("ThreatBook 查询失败"), parseErrorText);
        return;
    }

    const int responseCode = threatBookResponseCode(rootObject);
    const QJsonObject dataObject = reportDataObject(rootObject);
    if (responseCode == 0 && !dataObject.isEmpty() && !jsonContainsPendingHint(rootObject))
    {
        finishWithResult(rootObject);
        return;
    }

    if (m_pollAttempt >= kMaxPollAttempts)
    {
        finishWithError(
            QStringLiteral("ThreatBook 查询超时"),
            QStringLiteral("报告尚未完成，SHA-256：%1\n最后响应：\n%2")
                .arg(m_sha256Text, ks::online_scan::formatJsonObject(rootObject)));
        return;
    }

    // response_code 非 0 但包含分析中提示，或 data 暂为空，均继续轮询等待报告生成。
    scheduleReportPoll(kPollIntervalMs);
}

void ThreatBookScan::finishWithError(const QString& titleText, const QString& detailText)
{
    completeProgress(QStringLiteral("ThreatBook：扫描失败"));
    m_scanInProgress = false;
    ks::online_scan::showErrorDialog(m_dialogParent.data(), titleText, detailText);
    if (m_autoDeleteWhenFinished)
    {
        deleteLater();
    }
}

void ThreatBookScan::finishWithResult(const QJsonObject& reportObject)
{
    completeProgress(QStringLiteral("ThreatBook：扫描完成"));
    m_scanInProgress = false;

    const QString summaryText = buildResultSummary(reportObject);
    const QString detailText = ks::online_scan::formatJsonObject(reportObject);
    ks::online_scan::showResultDialog(
        m_dialogParent.data(),
        QStringLiteral("ThreatBook 扫描结果"),
        summaryText,
        detailText);

    if (m_autoDeleteWhenFinished)
    {
        deleteLater();
    }
}

void ThreatBookScan::resetRuntimeState()
{
    m_dialogParent.clear();
    m_filePath.clear();
    m_apiKey.clear();
    m_sha256Text.clear();
    m_pollAttempt = 0;
    m_scanInProgress = false;
    m_progressPid = 0;
}

void ThreatBookScan::completeProgress(const QString& messageText)
{
    if (m_progressPid == 0)
    {
        return;
    }
    kPro.set(m_progressPid, messageText.toStdString(), 0, 100.0f);
    m_progressPid = 0;
}

QString ThreatBookScan::buildResultSummary(const QJsonObject& reportObject) const
{
    const QJsonObject dataObject = reportDataObject(reportObject);
    const QJsonObject summaryObject = dataObject.value(QStringLiteral("summary")).toObject();
    const QJsonObject multienginesObject = dataObject.value(QStringLiteral("multiengines")).toObject();

    QStringList summaryLines;
    summaryLines << QStringLiteral("文件：%1").arg(QFileInfo(m_filePath).fileName());
    summaryLines << QStringLiteral("SHA-256：%1").arg(m_sha256Text);
    summaryLines << QStringLiteral("ThreatScore：%1")
        .arg(valueToDisplayText(summaryObject.value(QStringLiteral("threat_score"))));
    summaryLines << QStringLiteral("ThreatLevel：%1")
        .arg(valueToDisplayText(summaryObject.value(QStringLiteral("threat_level"))));
    summaryLines << QStringLiteral("MalwareType：%1")
        .arg(valueToDisplayText(summaryObject.value(QStringLiteral("malware_type"))));
    summaryLines << QStringLiteral("MalwareFamily：%1")
        .arg(valueToDisplayText(summaryObject.value(QStringLiteral("malware_family"))));
    if (!multienginesObject.isEmpty())
    {
        summaryLines << QStringLiteral("多引擎结果字段数：%1").arg(multienginesObject.size());
    }
    summaryLines << QStringLiteral("响应消息：%1")
        .arg(threatBookVerboseMessage(reportObject).isEmpty()
            ? QStringLiteral("-")
            : threatBookVerboseMessage(reportObject));
    return summaryLines.join(QChar('\n'));
}

QJsonObject ThreatBookScan::reportDataObject(const QJsonObject& reportObject) const
{
    const QJsonObject dataObject = reportObject.value(QStringLiteral("data")).toObject();
    if (dataObject.isEmpty())
    {
        return QJsonObject();
    }

    // ThreatBook report 常见响应会以 hash 作为 data 下一级 key；若存在则优先取该分组。
    const QJsonObject hashObject = dataObject.value(m_sha256Text).toObject();
    if (!hashObject.isEmpty())
    {
        return hashObject;
    }
    return dataObject;
}
