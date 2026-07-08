#pragma once

// ============================================================
// VirusTotalOnlineScan.h
// 作用：
// - 声明 VirusTotal 在线文件扫描类；
// - 调用方传入本地文件路径后，类负责读取设置中的 API Key、上传样本、轮询分析结果并弹窗展示；
// - 当前只准备能力，不主动接入主窗口菜单或文件列表入口。
// ============================================================

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QDialog;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QWidget;
class CodeEditorWidget;

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
    // - 上传响应返回后立即打开实时结果窗口，并持续追加分析轮询原始 JSON；
    // - sourceText 用于标识调用来源，例如“进程列表 PID=1234”。
    // 调用方式：scanner->scanFile(path, sourceText, parentWidget)。
    // 入参 filePath：本地文件路径。
    // 入参 sourceText：上传来源说明；为空时自动使用通用来源文本。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无；异步流程通过实时结果窗口和 kPro 反馈。
    void scanFile(const QString& filePath, const QString& sourceText, QWidget* dialogParent = nullptr);

    // scanFile 作用：
    // - 兼容旧调用方，只传文件路径时使用默认来源文本。
    // 入参 filePath：本地文件路径。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无；内部转发到带 sourceText 的重载。
    void scanFile(const QString& filePath, QWidget* dialogParent = nullptr);

    // scanFileAndAutoDelete 作用：
    // - 便捷创建堆对象，扫描结束后自动 deleteLater；
    // - 未来 UI 入口若不想保存成员指针，可直接调用。
    // 调用方式：VirusTotalOnlineScan::scanFileAndAutoDelete(path, source, parent)。
    // 入参 filePath：本地文件路径。
    // 入参 sourceText：上传来源说明。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无。
    static void scanFileAndAutoDelete(
        const QString& filePath,
        const QString& sourceText,
        QWidget* dialogParent = nullptr);

    // scanFileAndAutoDelete 作用：
    // - 兼容旧调用方，只传文件路径时使用默认来源文本。
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
    // - 统一结束失败流程，更新 kPro、写入实时结果窗口并按需自删除。
    // 入参 titleText：弹窗标题。
    // 入参 detailText：错误详情。
    // 返回：无。
    void finishWithError(const QString& titleText, const QString& detailText);

    // finishWithResult 作用：
    // - 统一结束成功流程，更新 kPro 并刷新实时结果窗口摘要；
    // - 原始 JSON 已在每次响应到达时追加到窗口。
    // 入参 analysisObject：VirusTotal analysis JSON 根对象。
    // 返回：无。
    void finishWithResult(const QJsonObject& analysisObject);

    // ensureResultDialog 作用：
    // - 确保实时结果窗口已创建并显示；
    // - 上传响应/分析响应/错误响应都会先调用该函数再追加内容。
    // 返回：无。
    void ensureResultDialog();

    // appendRawJsonSection 作用：
    // - 把一个 VirusTotal 原始 JSON 响应追加到实时窗口；
    // - titleText 用于标注 upload_url/upload/analysis/error 等阶段。
    // 入参 titleText：章节标题。
    // 入参 jsonObject：原始 JSON 对象。
    // 返回：无。
    void appendRawJsonSection(const QString& titleText, const QJsonObject& jsonObject);

    // appendRawTextSection 作用：
    // - 把非 JSON 错误详情或本地诊断追加到实时窗口；
    // - 用于网络错误、解析错误和超时详情。
    // 入参 titleText：章节标题。
    // 入参 detailText：原始/诊断文本。
    // 返回：无。
    void appendRawTextSection(const QString& titleText, const QString& detailText);

    // appendRawReplyBodySection 作用：
    // - 把 HTTP 响应体完整写入实时窗口和导出 JSON；
    // - 若响应体是 JSON 对象则按 JSON 结构保存，否则按 UTF-8 文本保存；
    // - 用于网络错误和解析失败路径，避免只展示截断后的错误摘要。
    // 入参 titleText：章节标题，通常包含接口路径和“原始响应体”语义。
    // 入参 bodyBytes：QNetworkReply::readAll() 得到的完整响应体。
    // 返回：无；空响应体会记录明确占位文本。
    void appendRawReplyBodySection(const QString& titleText, const QByteArray& bodyBytes);

    // updateResultSummary 作用：
    // - 更新实时窗口顶部摘要；
    // - 结果正文不丢失，只改变窗口顶部状态文本。
    // 入参 summaryText：新的摘要文本。
    // 返回：无。
    void updateResultSummary(const QString& summaryText);

    // buildRawExportJson 作用：
    // - 把实时窗口累计的原始响应、错误文本和上传上下文打包为 UTF-8 JSON；
    // - responses 字段保留每次 upload_url/upload/analysis/error 的原始内容；
    // - raw_text 字段保留窗口中显示的原始文本，便于人工排查时逐字对照。
    // 返回：可直接写入 .json 文件的 UTF-8 文档字节。
    QByteArray buildRawExportJson() const;

    // finalizeAutoDeleteIfNeeded 作用：
    // - 扫描成功或失败后统一处理 scanFileAndAutoDelete 创建的临时对象生命周期；
    // - 若实时结果窗口仍在显示，则等待窗口关闭后再 deleteLater，避免按钮回调访问已释放对象。
    // 返回：无；只修改对象生命周期标记。
    void finalizeAutoDeleteIfNeeded();

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

    // refreshReadableResult 作用：
    // - 把 VirusTotal 固定结构的 analysis 响应转换为可读页面；
    // - 刷新概要、文件哈希、统计和多引擎检测表，不影响原始 JSON 页。
    // 入参 analysisObject：/api/v3/analyses/{id} 返回的根对象。
    // 返回：无。
    void refreshReadableResult(const QJsonObject& analysisObject);

    // appendResponseTreeJsonSection 作用：
    // - 把一个 JSON 响应追加到“响应详情”树；
    // - 树节点可展开，便于逐层查看 VT 返回字段。
    // 入参 titleText：响应阶段标题。
    // 入参 timestampText：UTC 时间戳。
    // 入参 jsonObject：原始 JSON 对象。
    // 返回：无。
    void appendResponseTreeJsonSection(
        const QString& titleText,
        const QString& timestampText,
        const QJsonObject& jsonObject);

    // appendResponseTreeTextSection 作用：
    // - 把错误文本或非 JSON 响应追加到“响应详情”树；
    // - 用于网络错误、解析失败和本地校验错误。
    // 入参 titleText：响应阶段标题。
    // 入参 timestampText：UTC 时间戳。
    // 入参 detailText：错误或响应文本。
    // 返回：无。
    void appendResponseTreeTextSection(
        const QString& titleText,
        const QString& timestampText,
        const QString& detailText);

private:
    QNetworkAccessManager* m_networkManager = nullptr; // 网络请求管理器，负责所有 VirusTotal HTTP 调用。
    QPointer<QWidget> m_dialogParent;                  // 弹窗父控件，避免父控件提前销毁后悬空。
    QPointer<QDialog> m_resultDialog;                  // 实时结果窗口，上传响应到达后立即显示。
    QPointer<QLabel> m_resultSummaryLabel;             // 实时结果摘要标签，展示来源、文件、状态和 analysis id。
    QPointer<QTabWidget> m_resultTabWidget;            // 结果页签：可读结果、响应详情、原始数据。
    QPointer<QLabel> m_readableOverviewLabel;          // 可读页概要，展示状态、来源、analysis id 和判断结论。
    QPointer<QTableWidget> m_fileInfoTable;            // 可读页文件信息表，展示 hash、大小和 VT item 链接。
    QPointer<QTableWidget> m_engineTable;              // 可读页引擎明细表，展示每个引擎的分类和命中名称。
    QPointer<QTreeWidget> m_staticAnalysisTree;        // 可读页静态分析树，展示基础信息、分析任务和统计字段。
    QPointer<QTreeWidget> m_responseTree;              // 响应详情页，按树形结构展开每次 VT 返回。
    QPointer<CodeEditorWidget> m_resultEditor;         // 实时结果原始数据编辑器，展示全部响应和错误文本。
    QString m_filePath;                                // 当前待上传的本地文件路径。
    QString m_sourceText;                              // 当前上传来源说明，用于结果窗口摘要和导出上下文。
    QString m_resultRawText;                           // 当前已累计的原始响应/错误文本，供展示和导出使用。
    QJsonArray m_resultRawSections;                    // 当前已累计的结构化原始响应/错误章节，供 JSON 导出使用。
    QString m_apiKey;                                  // 当前扫描读取到的 VirusTotal API Key。
    QString m_analysisId;                              // 上传后返回的 analysis id，用于轮询结果。
    int m_progressPid = 0;                             // kPro 任务 PID，0 表示当前无进度任务。
    int m_pollAttempt = 0;                             // 当前轮询次数，用于超时控制。
    bool m_scanInProgress = false;                     // 当前对象是否已有扫描流程在运行。
    bool m_autoDeleteWhenFinished = false;             // 扫描结束后是否自动 deleteLater。
    bool m_deleteAfterResultDialogClosed = false;      // 扫描结束后等待实时结果窗口关闭再自删除。
};
