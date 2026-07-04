#pragma once

// ============================================================
// OnlineScanSupport.h
// 作用：
// - 为 VirusTotalOnlineScan 与 ThreatBookScan 提供公共 UI、JSON、网络错误和文件工具；
// - 统一扫描结果弹窗的样式，避免普通 QDialog 在主题切换后出现透明或黑底问题；
// - 只提供无状态辅助函数，不直接发起任何联网请求。
// ============================================================

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class QNetworkReply;
class QWidget;

namespace ks::online_scan
{
    // kVirusTotalDirectUploadMaxBytes 作用：
    // - VirusTotal 官方 /api/v3/files 直接上传接口的 32MB 分界；
    // - 超过该值时调用 /api/v3/files/upload_url 获取一次性上传 URL。
    inline constexpr qint64 kVirusTotalDirectUploadMaxBytes = 32LL * 1024LL * 1024LL;

    // kVirusTotalLargeUploadMaxBytes 作用：
    // - VirusTotal 官方大文件上传 URL 的 650MB 上限；
    // - 超过该值时本地直接拒绝，避免发起必然失败的请求。
    inline constexpr qint64 kVirusTotalLargeUploadMaxBytes = 650LL * 1024LL * 1024LL;

    // kThreatBookUploadMaxBytes 作用：
    // - ThreatBook 官方 file/upload 接口要求单文件不超过 100MB；
    // - 上传前校验该值，避免无意义消耗配额和等待时间。
    inline constexpr qint64 kThreatBookUploadMaxBytes = 100LL * 1024LL * 1024LL;

    // validateReadableFile 作用：
    // - 校验输入文件路径是否存在、是否普通文件、是否可读，并按需检查大小上限；
    // - 扫描类在创建上传任务前调用。
    // 入参 filePath：用户传入的文件路径。
    // 入参 maxBytes：允许的最大字节数；小于等于 0 表示不检查上限。
    // 入参 errorTextOut：失败原因输出，可为空。
    // 返回：true=文件可用于上传；false=文件不可读或超过上限。
    bool validateReadableFile(const QString& filePath, qint64 maxBytes, QString* errorTextOut);

    // sanitizeFileNameForContentDisposition 作用：
    // - 清理 multipart Content-Disposition 中的文件名；
    // - 防止引号、斜杠等字符破坏 HTTP 表单头。
    // 入参 fileName：原始文件名。
    // 返回：可安全放入 filename="..." 的文件名。
    QString sanitizeFileNameForContentDisposition(const QString& fileName);

    // parseJsonObjectFromBytes 作用：
    // - 把 HTTP 响应体解析为 JSON 对象；
    // - 解析失败时返回空对象并写入错误文本。
    // 入参 bodyBytes：HTTP 响应体。
    // 入参 errorTextOut：失败原因输出，可为空。
    // 返回：解析后的 JSON 对象；失败时为空对象。
    QJsonObject parseJsonObjectFromBytes(const QByteArray& bodyBytes, QString* errorTextOut);

    // formatJsonObject 作用：
    // - 把 JSON 对象格式化为带缩进文本；
    // - 用于 CodeEditorWidget 展示扫描原始结果。
    // 入参 jsonObject：待格式化对象。
    // 返回：UTF-8 JSON 文本。
    QString formatJsonObject(const QJsonObject& jsonObject);

    // networkReplyErrorText 作用：
    // - 汇总 QNetworkReply 的 HTTP 状态、Qt 网络错误和响应体摘要；
    // - 扫描类在错误弹窗与日志中复用。
    // 入参 reply：已完成的网络响应对象。
    // 入参 bodyBytes：已经读取出的响应体。
    // 返回：适合用户阅读的错误说明。
    QString networkReplyErrorText(QNetworkReply* reply, const QByteArray& bodyBytes);

    // calculateSha256Hex 作用：
    // - 流式计算文件 SHA-256，供 ThreatBook 查询报告时使用；
    // - 文件较大时仍按块读取，避免一次性把样本读入内存。
    // 入参 filePath：待计算文件路径。
    // 入参 errorTextOut：失败原因输出，可为空。
    // 返回：小写十六进制 SHA-256；失败时返回空字符串。
    QString calculateSha256Hex(const QString& filePath, QString* errorTextOut);

    // showMissingApiKeyDialog 作用：
    // - 当设置中没有对应 API Key 时弹窗提示用户先进入设置页填写；
    // - 不发起任何网络请求。
    // 入参 parentWidget：弹窗父窗口，可为空。
    // 入参 serviceName：服务名称，如 VirusTotal 或 ThreatBook。
    // 返回：无。
    void showMissingApiKeyDialog(QWidget* parentWidget, const QString& serviceName);

    // showErrorDialog 作用：
    // - 用统一主题弹出在线扫描错误；
    // - 适用于本地校验、上传失败、轮询超时等场景。
    // 入参 parentWidget：弹窗父窗口，可为空。
    // 入参 titleText：弹窗标题。
    // 入参 detailText：详细错误文本。
    // 返回：无。
    void showErrorDialog(QWidget* parentWidget, const QString& titleText, const QString& detailText);

    // showResultDialog 作用：
    // - 使用项目内置 CodeEditorWidget 展示在线扫描结果 JSON；
    // - 上方展示摘要，下方展示完整原始响应，便于复制与搜索。
    // 入参 parentWidget：弹窗父窗口，可为空。
    // 入参 titleText：弹窗标题。
    // 入参 summaryText：摘要说明文本。
    // 入参 detailJsonText：格式化后的原始 JSON 文本。
    // 返回：无。
    void showResultDialog(
        QWidget* parentWidget,
        const QString& titleText,
        const QString& summaryText,
        const QString& detailJsonText);
}
