#pragma once

// ============================================================
// VirusTotalOnlineScan.h
// 作用：
// - 声明 VirusTotal 多 API 在线分析控制台；
// - 调用方传入本地文件路径、来源文本和初始 API 后，类负责读取设置中的 API Key；
// - 普通分析负责上传样本并轮询 /analyses/{id}，其它 API 基于本地 SHA256 查询文件画像、IOC 和沙箱行为；
// - 所有结果集中展示在多 Tab 弹窗，并支持导出全部 API 原始响应。
// ============================================================

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <array>
#include <functional>

class QDialog;
class QGroupBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QTreeWidget;
class QWidget;
class CodeEditorWidget;

// VirusTotalOnlineScan：
// - 输入：调用 scanFile 时传入文件路径、来源文本、初始 API 和可选父窗口；
// - 处理：读取设置中的 virustotal_api_key，按 VirusTotal v3 API 串行执行上传、画像、IOC、沙箱和 HTML 报告请求；
// - 输出：完成、无数据或失败均在多 API 结果窗口内反馈，无同步返回扫描结果。
class VirusTotalOnlineScan final : public QObject
{
public:
    // VtApiKind 作用：
    // - 描述 VirusTotal 多 API 控制台的一级 Tab；
    // - 调用方通过该枚举决定窗口打开后先切到哪个分析页。
    enum class VtApiKind
    {
        ShallowAnalysis = 0, // 普通分析：上传样本并轮询 /analyses/{id}。
        FileProfile = 1,    // 文件画像：GET /files/{sha256}。
        Ioc = 2,            // IOC：GET /files/{sha256}/{relationship} 常用关系。
        Sandbox = 3,        // 沙箱：behaviour_summary、behaviours 和 HTML 报告。
        AllApis = 4,        // 特殊入口：串行触发全部 API，不对应固定一级 Tab。
    };

    // VtApiState 作用：
    // - 记录每个一级 Tab 当前请求状态；
    // - UI 根据状态显示“开始分析”、运行中、完成、无数据或失败。
    enum class VtApiState
    {
        NotStarted,
        Hashing,
        Running,
        Completed,
        Empty,
        Failed,
    };

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
    // - 多 API 入口版本，额外接收 initialApi；
    // - initialApi 决定结果窗口初始切到哪个一级 Tab，以及首次自动启动哪个 API；
    // - AllApis 会串行启动普通分析、文件画像、IOC 和沙箱。
    // 入参 filePath/sourceText/dialogParent：同旧重载。
    // 入参 initialApi：初始分析类型。
    // 返回：无。
    void scanFile(
        const QString& filePath,
        const QString& sourceText,
        VtApiKind initialApi,
        QWidget* dialogParent = nullptr);

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
    // - 多 API 入口版本；调用完成后对象生命周期规则与旧版本一致。
    // 入参 initialApi：打开窗口后要启动/切换的 API 类型。
    // 返回：无。
    static void scanFileAndAutoDelete(
        const QString& filePath,
        const QString& sourceText,
        VtApiKind initialApi,
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

    // ensureLocalHashes 作用：
    // - 为非上传型 API 计算本地 MD5/SHA1/SHA256；
    // - 已有 hash 时直接进入目标 API，未计算时放入后台线程。
    // 入参 nextApi：hash 完成后继续启动的 API。
    // 返回：无。
    void ensureLocalHashes(VtApiKind nextApi);

    // handleLocalHashesReady 作用：
    // - 接收后台 hash 计算结果并唤醒等待中的 API；
    // - 失败时把等待中的 Tab 标记为失败。
    // 入参 md5Text/sha1Text/sha256Text：计算出的十六进制 hash。
    // 入参 errorText：失败原因；为空表示成功。
    // 返回：无。
    void handleLocalHashesReady(
        const QString& md5Text,
        const QString& sha1Text,
        const QString& sha256Text,
        const QString& errorText);

    // startApiAnalysis 作用：
    // - 根据一级 Tab 类型启动对应 VT API；
    // - 普通分析走上传/轮询，其余 API 先确保本地 SHA256。
    // 入参 apiKind：目标 API。
    // 返回：无。
    void startApiAnalysis(VtApiKind apiKind);

    // startAllApis 作用：
    // - 串行启动全部 API；
    // - 避免 Public API 场景下并发请求过多。
    // 返回：无。
    void startAllApis();

    // requestFileProfile 作用：
    // - 调用 GET /api/v3/files/{sha256} 获取文件画像。
    // 返回：无。
    void requestFileProfile();

    // handleFileProfileReply 作用：
    // - 解析文件画像响应并刷新“文件画像”Tab。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleFileProfileReply(QNetworkReply* reply);

    // requestNextIocRelationship 作用：
    // - 串行请求常用 IOC relationship；
    // - 每次响应后继续下一个 relationship。
    // 返回：无。
    void requestNextIocRelationship();

    // startSingleIocRelationship 作用：
    // - 从 IOC 报告视图的分项按钮启动单个 relationship 请求；
    // - 当前有其它 VT 请求运行时仅记录待执行 relationship，不破坏正在执行的全量 IOC 队列。
    // 入参 relationshipText：VT files relationship 名称。
    // 返回：无。
    void startSingleIocRelationship(const QString& relationshipText);

    // handleIocRelationshipReply 作用：
    // - 保存单个 IOC relationship 响应并决定是否继续。
    // 入参 relationshipText：当前 relationship 名称。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleIocRelationshipReply(const QString& relationshipText, QNetworkReply* reply);

    // requestSandboxSummary 作用：
    // - 启动沙箱行为分析：先请求 behaviour_summary，再请求 behaviours。
    // 返回：无。
    void requestSandboxSummary();

    // handleSandboxSummaryReply 作用：
    // - 保存 behaviour_summary 响应并继续请求 behaviours。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleSandboxSummaryReply(QNetworkReply* reply);

    // requestSandboxBehaviours 作用：
    // - 请求 GET /files/{sha256}/behaviours 获取单沙箱列表。
    // 返回：无。
    void requestSandboxBehaviours();

    // handleSandboxBehavioursReply 作用：
    // - 保存 behaviours 响应并刷新沙箱 Tab。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleSandboxBehavioursReply(QNetworkReply* reply);

    // requestSandboxHtmlReport 作用：
    // - 拉取单个 file_behaviour 的 HTML 报告；
    // - HTML 内容进入沙箱 Tab 原始数据和报告树摘要。
    // 入参 behaviourId：file_behaviour 对象 id。
    // 返回：无。
    void requestSandboxHtmlReport(const QString& behaviourId);

    // requestNextSandboxHtmlReport 作用：
    // - “全部 API”或按钮触发时串行拉取可用 HTML 报告；
    // - 队列为空时结束沙箱 API。
    // 返回：无。
    void requestNextSandboxHtmlReport();

    // handleSandboxHtmlReply 作用：
    // - 保存 HTML 报告响应。
    // 入参 behaviourId：file_behaviour 对象 id。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleSandboxHtmlReply(const QString& behaviourId, QNetworkReply* reply);

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

    // selectApiTab 作用：
    // - 把一级 Tab 切换到指定 API；
    // - AllApis 会切到普通分析 Tab。
    // 入参 apiKind：目标 API。
    // 返回：无。
    void selectApiTab(VtApiKind apiKind);

    // setApiState 作用：
    // - 更新一个一级 Tab 的状态并刷新开始按钮可见性；
    // - 所有 API 启动、完成、失败都通过该函数收敛 UI 状态。
    // 入参 apiKind/apiState/statusText：目标 API、状态和可选提示。
    // 返回：无。
    void setApiState(VtApiKind apiKind, VtApiState apiState, const QString& statusText = QString());

    // refreshApiPlaceholder 作用：
    // - 在未开始、运行中、无数据或失败时刷新报告视图占位内容；
    // - 已完成时由具体 refreshXxxResult 函数覆盖。
    // 入参 apiKind/statusText：目标 API 和提示文本。
    // 返回：无。
    void refreshApiPlaceholder(VtApiKind apiKind, const QString& statusText = QString());

    // hasActiveApiOperation 作用：
    // - 判断当前是否已有 VT API 请求或本地 hash 任务在运行；
    // - 用户手动点击其它 API 时据此进入串行等待队列，避免 Public API 并发打爆。
    // 返回：true=至少一个 API 正在运行或计算 hash；false=可立即启动下一项。
    bool hasActiveApiOperation() const;

    // startNextQueuedAllApi 作用：
    // - “传入所有API”和用户手动排队 API 的串行队列驱动函数；
    // - 当前 API 完成/失败后调用，启动下一个等待项。
    // 返回：无。
    void startNextQueuedAllApi();

    // scheduleRetryAfterRateLimit 作用：
    // - 对 VirusTotal 429/配额限流响应做统一串行重试；
    // - 读取 Retry-After 响应头，缺失时使用保守默认等待时间；
    // - 重试期间保持对应 API Pane 为 Running，避免其它 VT 请求并发插队。
    // 入参 apiKind：被限流的一级 API。
    // 入参 retryKey：用于计数的稳定端点 key，防止同一端点无限重试。
    // 入参 reply：携带 HTTP 状态和 Retry-After 的网络响应。
    // 入参 statusText：展示在报告视图中的限流等待说明。
    // 入参 retryAction：到时后重新发起同一请求的回调。
    // 返回：true=已安排重试；false=不是限流或已超过重试次数。
    bool scheduleRetryAfterRateLimit(
        VtApiKind apiKind,
        const QString& retryKey,
        QNetworkReply* reply,
        const QString& statusText,
        std::function<void()> retryAction);

    // appendRawJsonSection 作用：
    // - 把一个 VirusTotal 原始 JSON 响应追加到实时窗口；
    // - titleText 用于标注 upload_url/upload/analysis/error 等阶段。
    // 入参 titleText：章节标题。
    // 入参 jsonObject：原始 JSON 对象。
    // 返回：无。
    void appendRawJsonSection(const QString& titleText, const QJsonObject& jsonObject);

    // appendRawJsonSection 作用：
    // - 多 API 版本，把响应追加到指定一级 Tab 的“响应详情/原始数据”页；
    // - 旧版本默认追加到普通分析 Tab。
    // 入参 apiKind：目标一级 Tab。
    // 入参 titleText/jsonObject：响应标题和 JSON。
    // 返回：无。
    void appendRawJsonSection(VtApiKind apiKind, const QString& titleText, const QJsonObject& jsonObject);

    // appendRawJsonSection 作用：
    // - 带 HTTP 元数据版本；把 http_status/retry_after 等字段写入导出 JSON；
    // - JSON body 保持 VT 原始结构，不把 HTTP 元数据混入 body 本身。
    // 入参 responseMetadata：replyHttpMetadataObject 产生的 HTTP 元数据，可为空。
    // 返回：无。
    void appendRawJsonSection(
        VtApiKind apiKind,
        const QString& titleText,
        const QJsonObject& jsonObject,
        const QJsonObject& responseMetadata);

    // appendRawTextSection 作用：
    // - 把非 JSON 错误详情或本地诊断追加到实时窗口；
    // - 用于网络错误、解析错误和超时详情。
    // 入参 titleText：章节标题。
    // 入参 detailText：原始/诊断文本。
    // 返回：无。
    void appendRawTextSection(const QString& titleText, const QString& detailText);

    // appendRawTextSection 作用：
    // - 多 API 版本，把文本/错误追加到指定一级 Tab。
    // 入参 apiKind：目标一级 Tab。
    // 返回：无。
    void appendRawTextSection(VtApiKind apiKind, const QString& titleText, const QString& detailText);

    // appendRawTextSection 作用：
    // - 带 HTTP 元数据版本；用于错误文本、HTML 报告等非 JSON 响应；
    // - 导出时保留文本 body，同时单独保存 HTTP 状态。
    // 入参 responseMetadata：HTTP 元数据，可为空。
    // 返回：无。
    void appendRawTextSection(
        VtApiKind apiKind,
        const QString& titleText,
        const QString& detailText,
        const QJsonObject& responseMetadata);

    // appendRawReplyBodySection 作用：
    // - 把 HTTP 响应体完整写入实时窗口和导出 JSON；
    // - 若响应体是 JSON 对象则按 JSON 结构保存，否则按 UTF-8 文本保存；
    // - 用于网络错误和解析失败路径，避免只展示截断后的错误摘要。
    // 入参 titleText：章节标题，通常包含接口路径和“原始响应体”语义。
    // 入参 bodyBytes：QNetworkReply::readAll() 得到的完整响应体。
    // 返回：无；空响应体会记录明确占位文本。
    void appendRawReplyBodySection(const QString& titleText, const QByteArray& bodyBytes);

    // appendRawReplyBodySection 作用：
    // - 多 API 版本，按 JSON 或文本保存 HTTP 原始响应体。
    // 入参 apiKind：目标一级 Tab。
    // 返回：无。
    void appendRawReplyBodySection(VtApiKind apiKind, const QString& titleText, const QByteArray& bodyBytes);

    // appendRawReplyBodySection 作用：
    // - 带 HTTP 元数据版本；错误响应体被解析成 JSON 或文本时同步保存状态码；
    // - 便于导出的每个 API 响应都能追溯到 HTTP 层结果。
    // 入参 responseMetadata：HTTP 元数据，可为空。
    // 返回：无。
    void appendRawReplyBodySection(
        VtApiKind apiKind,
        const QString& titleText,
        const QByteArray& bodyBytes,
        const QJsonObject& responseMetadata);

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

    // refreshFileProfileResult 作用：
    // - 把 GET /files/{sha256} 文件画像转换为报告树。
    // 入参 fileObject：VT 文件对象 JSON。
    // 返回：无。
    void refreshFileProfileResult(const QJsonObject& fileObject);

    // refreshIocResult 作用：
    // - 把已请求的 IOC relationship 结果转换为报告树。
    // 返回：无。
    void refreshIocResult();

    // refreshSandboxResult 作用：
    // - 把沙箱汇总、沙箱列表和 HTML 报告状态转换为报告树。
    // 返回：无。
    void refreshSandboxResult();

    // showSandboxHtmlPreview 作用：
    // - 在“沙箱 -> 报告视图”内渲染指定 file_behaviour 的 HTML 报告；
    // - 错误或暂无 HTML 时显示可读文本，避免用户只能去原始数据页翻响应。
    // 入参 behaviourId：file_behaviour 对象 id。
    // 返回：无。
    void showSandboxHtmlPreview(const QString& behaviourId);

    // appendResponseTreeJsonSection 作用：
    // - 把一个 JSON 响应追加到“响应详情”树；
    // - 树节点可展开，便于逐层查看 VT 返回字段。
    // 入参 titleText：响应阶段标题。
    // 入参 timestampText：UTC 时间戳。
    // 入参 jsonObject：原始 JSON 对象。
    // 返回：无。
    void appendResponseTreeJsonSection(
        VtApiKind apiKind,
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
        VtApiKind apiKind,
        const QString& titleText,
        const QString& timestampText,
        const QString& detailText);

private:
    static constexpr int kApiPaneCount = 4;

    // LocalHashContext 作用：
    // - 保存本地样本 hash 和后台计算状态；
    // - 非上传 API 使用 sha256 作为 /files/{id} 参数。
    struct LocalHashContext
    {
        QString md5Text;
        QString sha1Text;
        QString sha256Text;
        bool ready = false;
        bool running = false;
    };

    // ApiPaneUi 作用：
    // - 保存一个一级 API Tab 内的二级 Tab 控件；
    // - 每个 Pane 固定包含报告视图、响应详情、原始数据。
    struct ApiPaneUi
    {
        QPointer<QTabWidget> detailTabWidget;
        QPointer<QLabel> overviewLabel;
        QPointer<QPushButton> startButton;
        QPointer<QPushButton> sandboxHtmlButton;
        QPointer<QLineEdit> fileProfileFilterEdit;
        QPointer<QTableWidget> fileInfoTable;
        QPointer<QTableWidget> engineTable;
        QPointer<QTreeWidget> reportTree;
        QPointer<QGroupBox> sandboxHtmlPreviewGroup;
        QPointer<QTextBrowser> sandboxHtmlPreview;
        QPointer<QTreeWidget> responseTree;
        QPointer<CodeEditorWidget> rawEditor;
    };

    QNetworkAccessManager* m_networkManager = nullptr; // 网络请求管理器，负责所有 VirusTotal HTTP 调用。
    QPointer<QWidget> m_dialogParent;                  // 弹窗父控件，避免父控件提前销毁后悬空。
    QPointer<QDialog> m_resultDialog;                  // 实时结果窗口，上传响应到达后立即显示。
    QPointer<QLabel> m_resultSummaryLabel;             // 实时结果摘要标签，展示来源、文件、状态和 analysis id。
    QPointer<QTabWidget> m_resultTabWidget;            // 一级结果页签：普通分析、文件画像、IOC、沙箱。
    QPointer<QLabel> m_readableOverviewLabel;          // 可读页概要，展示状态、来源、analysis id 和判断结论。
    QPointer<QTableWidget> m_fileInfoTable;            // 可读页文件信息表，展示 hash、大小和 VT item 链接。
    QPointer<QTableWidget> m_engineTable;              // 可读页引擎明细表，展示每个引擎的分类和命中名称。
    QPointer<QTreeWidget> m_staticAnalysisTree;        // 可读页静态分析树，展示基础信息、分析任务和统计字段。
    QPointer<QTreeWidget> m_responseTree;              // 响应详情页，按树形结构展开每次 VT 返回。
    QPointer<CodeEditorWidget> m_resultEditor;         // 实时结果原始数据编辑器，展示全部响应和错误文本。
    std::array<ApiPaneUi, kApiPaneCount> m_apiPanes;    // 多 API 一级 Tab 对应的 UI 控件。
    std::array<VtApiState, kApiPaneCount> m_apiStates;  // 多 API 状态。
    std::array<QString, kApiPaneCount> m_apiRawText;    // 每个 API Tab 的原始文本。
    std::array<QJsonArray, kApiPaneCount> m_apiRawSections; // 每个 API Tab 的结构化原始响应。
    QString m_filePath;                                // 当前待上传的本地文件路径。
    QString m_sourceText;                              // 当前上传来源说明，用于结果窗口摘要和导出上下文。
    QString m_resultRawText;                           // 当前已累计的原始响应/错误文本，供展示和导出使用。
    QJsonArray m_resultRawSections;                    // 当前已累计的结构化原始响应/错误章节，供 JSON 导出使用。
    LocalHashContext m_localHashes;                     // 本地样本 hash，上层 API 复用。
    QList<VtApiKind> m_pendingHashApis;                 // 等待 hash 完成后继续启动的 API。
    QList<VtApiKind> m_allApiQueue;                     // “传入所有API”串行队列。
    QList<VtApiKind> m_deferredApiQueue;                // 用户手动点击但因已有 VT 请求运行而延后启动的 API 队列。
    QStringList m_deferredSingleIocRelationships;       // 用户手动点击的 IOC 分项队列，独立于全量 IOC 队列。
    QJsonObject m_fileProfileObject;                    // GET /files/{sha256} 最新响应。
    QJsonObject m_iocRelationshipObjects;               // relationship -> 原始响应。
    QStringList m_iocRelationshipQueue;                 // IOC relationship 串行请求队列。
    QJsonObject m_sandboxSummaryObject;                 // behaviour_summary 最新响应。
    QJsonObject m_sandboxBehavioursObject;              // behaviours 最新响应。
    QJsonObject m_sandboxHtmlReports;                   // behaviour_id -> HTML 文本/错误元数据。
    QStringList m_sandboxHtmlQueue;                     // 待拉取的 file_behaviour HTML id 队列。
    bool m_sandboxHtmlFetchQueued = false;              // HTML 报告拉取是否因当前有 VT 请求而延后执行。
    QHash<QString, int> m_vtRateLimitRetryCounts;       // VT 429 重试计数，key 为 API/endpoint，防止无限重试。
    QString m_apiKey;                                  // 当前扫描读取到的 VirusTotal API Key。
    QString m_settingsJsonPath;                         // 当前运行时读取 API Key 的设置文件路径，只用于安全诊断。
    QString m_analysisId;                              // 上传后返回的 analysis id，用于轮询结果。
    int m_progressPid = 0;                             // kPro 任务 PID，0 表示当前无进度任务。
    int m_pollAttempt = 0;                             // 当前轮询次数，用于超时控制。
    bool m_scanInProgress = false;                     // 当前对象是否已有扫描流程在运行。
    bool m_allApisMode = false;                         // 当前是否由“传入所有API”驱动串行流程。
    bool m_dispatchingQueuedApi = false;                // 当前是否正在由内部串行队列启动 API，避免再次排队。
    bool m_autoDeleteWhenFinished = false;             // 扫描结束后是否自动 deleteLater。
    bool m_deleteAfterResultDialogClosed = false;      // 扫描结束后等待实时结果窗口关闭再自删除。
};
