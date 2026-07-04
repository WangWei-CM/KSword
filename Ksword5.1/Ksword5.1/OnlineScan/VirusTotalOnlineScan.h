#pragma once

// ============================================================
// VirusTotalOnlineScan.h
// 作用：
// - 声明 VirusTotal 在线文件扫描类；
// - 调用方传入本地文件路径后，类负责读取设置中的 API Key、上传样本、轮询分析结果并弹窗展示；
// - 当前只准备能力，不主动接入主窗口菜单或文件列表入口。
// ============================================================

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QWidget;

// VirusTotalOnlineScan：
// - 输入：调用 scanFile 时传入文件路径和可选父窗口；
// - 处理：读取设置中的 virustotal_api_key，按 VirusTotal v3 API 上传并轮询 /analyses/{id}；
// - 输出：完成或失败均通过弹窗反馈，无同步返回扫描结果。
class VirusTotalOnlineScan final : public QObject
{
public:
    // 构造函数作用：
    // - 创建网络访问管理器；
    // - 不读取 API Key，不发起请求。
    // 调用方式：可由未来文件右键菜单或按钮创建实例。
    // 入参 parent：Qt 父对象，可为空。
    explicit VirusTotalOnlineScan(QObject* parent = nullptr);

    // 析构函数作用：
    // - QObject 父子机制释放网络对象；
    // - 若仍有请求，Qt 会随对象销毁断开回调。
    ~VirusTotalOnlineScan() override;

    // scanFile 作用：
    // - 读取设置中的 VirusTotal API Key 并上传指定文件；
    // - 上传后轮询分析状态，完成时弹出结果窗口。
    // 调用方式：scanner->scanFile(path, parentWidget)。
    // 入参 filePath：本地文件路径。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无；异步流程通过弹窗和 kPro 反馈。
    void scanFile(const QString& filePath, QWidget* dialogParent = nullptr);

    // scanFileAndAutoDelete 作用：
    // - 便捷创建堆对象，扫描结束后自动 deleteLater；
    // - 未来 UI 入口若不想保存成员指针，可直接调用。
    // 调用方式：VirusTotalOnlineScan::scanFileAndAutoDelete(path, parent)。
    // 入参 filePath：本地文件路径。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无。
    static void scanFileAndAutoDelete(const QString& filePath, QWidget* dialogParent = nullptr);

private:
    // requestLargeUploadUrl 作用：
    // - 当样本超过 32MB 时请求 VirusTotal 一次性大文件上传 URL；
    // - 成功后继续调用 uploadFileToUrl。
    // 返回：无，异步回调 handleUploadUrlReply。
    void requestLargeUploadUrl();

    // uploadFileToUrl 作用：
    // - 使用 multipart/form-data 把当前文件上传到指定 URL；
    // - 小文件直接使用 /api/v3/files，大文件使用 upload_url 返回值。
    // 入参 uploadUrl：目标上传 URL。
    // 返回：无，异步回调 handleUploadReply。
    void uploadFileToUrl(const QUrl& uploadUrl);

    // handleUploadUrlReply 作用：
    // - 解析 /api/v3/files/upload_url 的响应；
    // - 提取 data 字段作为真实上传地址。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleUploadUrlReply(QNetworkReply* reply);

    // handleUploadReply 作用：
    // - 解析文件上传响应；
    // - 提取 analysis id 并进入轮询阶段。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleUploadReply(QNetworkReply* reply);

    // scheduleAnalysisPoll 作用：
    // - 安排下一次分析状态轮询；
    // - 首次轮询也通过该函数统一调度，避免重复逻辑。
    // 入参 delayMs：延迟毫秒数。
    // 返回：无。
    void scheduleAnalysisPoll(int delayMs);

    // requestAnalysisStatus 作用：
    // - 调用 VirusTotal /api/v3/analyses/{id} 查询分析状态；
    // - 若任务未完成，会继续 scheduleAnalysisPoll。
    // 返回：无。
    void requestAnalysisStatus();

    // handleAnalysisReply 作用：
    // - 解析分析状态响应；
    // - completed 时弹出结果，非 completed 时继续轮询或超时失败。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleAnalysisReply(QNetworkReply* reply);

    // finishWithError 作用：
    // - 统一结束失败流程，更新 kPro、弹出错误并按需自删除。
    // 入参 titleText：弹窗标题。
    // 入参 detailText：错误详情。
    // 返回：无。
    void finishWithError(const QString& titleText, const QString& detailText);

    // finishWithResult 作用：
    // - 统一结束成功流程，更新 kPro 并弹出扫描结果；
    // - 结果弹窗使用 CodeEditorWidget 展示完整 JSON。
    // 入参 analysisObject：VirusTotal analysis JSON 根对象。
    // 返回：无。
    void finishWithResult(const QJsonObject& analysisObject);

    // resetRuntimeState 作用：
    // - 清理上一次扫描遗留的文件路径、分析 ID 和轮询计数；
    // - 不清除网络管理器对象。
    // 返回：无。
    void resetRuntimeState();

    // completeProgress 作用：
    // - 将当前 kPro 任务推进到 100% 并清除 PID；
    // - 避免成功/失败路径重复写进度条收尾逻辑。
    // 入参 messageText：最终显示的任务状态。
    // 返回：无。
    void completeProgress(const QString& messageText);

    // buildResultSummary 作用：
    // - 从 VirusTotal analysis JSON 中提取 stats 摘要；
    // - 用于结果弹窗顶部的人类可读说明。
    // 入参 analysisObject：VirusTotal analysis JSON 根对象。
    // 返回：摘要文本。
    QString buildResultSummary(const QJsonObject& analysisObject) const;

private:
    QNetworkAccessManager* m_networkManager = nullptr; // 网络请求管理器，负责所有 VirusTotal HTTP 调用。
    QPointer<QWidget> m_dialogParent;                  // 弹窗父控件，避免父控件提前销毁后悬空。
    QString m_filePath;                                // 当前待上传的本地文件路径。
    QString m_apiKey;                                  // 当前扫描读取到的 VirusTotal API Key。
    QString m_analysisId;                              // 上传后返回的 analysis id，用于轮询结果。
    int m_progressPid = 0;                             // kPro 任务 PID，0 表示当前无进度任务。
    int m_pollAttempt = 0;                             // 当前轮询次数，用于超时控制。
    bool m_scanInProgress = false;                     // 当前对象是否已有扫描流程在运行。
    bool m_autoDeleteWhenFinished = false;             // 扫描结束后是否自动 deleteLater。
};
