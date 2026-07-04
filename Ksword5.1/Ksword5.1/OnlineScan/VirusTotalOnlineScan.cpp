#include "VirusTotalOnlineScan.h"

#include "OnlineScanSupport.h"
#include "../Framework.h"
#include "../SettingsDock/AppearanceSettings.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QWidget>

#include <algorithm>

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
}

VirusTotalOnlineScan::VirusTotalOnlineScan(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
}

VirusTotalOnlineScan::~VirusTotalOnlineScan() = default;

void VirusTotalOnlineScan::scanFile(const QString& filePath, QWidget* dialogParent)
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

void VirusTotalOnlineScan::scanFileAndAutoDelete(const QString& filePath, QWidget* dialogParent)
{
    VirusTotalOnlineScan* scanner = new VirusTotalOnlineScan(dialogParent);
    scanner->m_autoDeleteWhenFinished = true;
    scanner->scanFile(filePath, dialogParent);
    if (!scanner->m_scanInProgress)
    {
        scanner->deleteLater();
    }
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
        reply->deleteLater();
        finishWithError(
            QStringLiteral("VirusTotal 获取上传地址失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
    const QString uploadUrlText = rootObject.value(QStringLiteral("data")).toString().trimmed();
    if (!parseErrorText.isEmpty() || uploadUrlText.isEmpty())
    {
        finishWithError(
            QStringLiteral("VirusTotal 获取上传地址失败"),
            parseErrorText.isEmpty() ? QStringLiteral("响应中缺少 data 上传 URL。") : parseErrorText);
        return;
    }

    uploadFileToUrl(QUrl(uploadUrlText));
}

void VirusTotalOnlineScan::handleUploadReply(QNetworkReply* reply)
{
    const QByteArray bodyBytes = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;

    if (!networkOk)
    {
        const QString errorText = ks::online_scan::networkReplyErrorText(reply, bodyBytes);
        reply->deleteLater();
        finishWithError(
            QStringLiteral("VirusTotal 上传失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
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
        reply->deleteLater();
        finishWithError(
            QStringLiteral("VirusTotal 查询失败"),
            errorText);
        return;
    }
    reply->deleteLater();

    QString parseErrorText;
    const QJsonObject rootObject = ks::online_scan::parseJsonObjectFromBytes(bodyBytes, &parseErrorText);
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
    scheduleAnalysisPoll(kPollIntervalMs);
}

void VirusTotalOnlineScan::finishWithError(const QString& titleText, const QString& detailText)
{
    completeProgress(QStringLiteral("VirusTotal：扫描失败"));
    m_scanInProgress = false;
    ks::online_scan::showErrorDialog(m_dialogParent.data(), titleText, detailText);
    if (m_autoDeleteWhenFinished)
    {
        deleteLater();
    }
}

void VirusTotalOnlineScan::finishWithResult(const QJsonObject& analysisObject)
{
    completeProgress(QStringLiteral("VirusTotal：扫描完成"));
    m_scanInProgress = false;

    const QString summaryText = buildResultSummary(analysisObject);
    const QString detailText = ks::online_scan::formatJsonObject(analysisObject);
    ks::online_scan::showResultDialog(
        m_dialogParent.data(),
        QStringLiteral("VirusTotal 扫描结果"),
        summaryText,
        detailText);

    if (m_autoDeleteWhenFinished)
    {
        deleteLater();
    }
}

void VirusTotalOnlineScan::resetRuntimeState()
{
    m_dialogParent.clear();
    m_filePath.clear();
    m_apiKey.clear();
    m_analysisId.clear();
    m_pollAttempt = 0;
    m_scanInProgress = false;
    m_progressPid = 0;
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
