#pragma once

// ============================================================
// ThreatBookScan.h
// 作用：
// - 声明 ThreatBook（微步在线）文件扫描类；
// - 调用方传入本地文件路径后，类负责读取设置中的 API Key、上传样本、按 SHA-256 查询报告并弹窗展示；
// - 当前只准备能力，不主动接入主窗口菜单或文件列表入口。
// ============================================================

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QWidget;

// ThreatBookScan：
// - 输入：调用 scanFile 时传入文件路径和可选父窗口；
// - 处理：读取设置中的 threatbook_api_key，调用 ThreatBook file/upload 与 file/report；
// - 输出：完成或失败均通过弹窗反馈，无同步返回扫描结果。
class ThreatBookScan final : public QObject
{
public:
    // 构造函数作用：
    // - 创建网络访问管理器；
    // - 不读取 API Key，不发起请求。
    // 调用方式：可由未来文件右键菜单或按钮创建实例。
    // 入参 parent：Qt 父对象，可为空。
    explicit ThreatBookScan(QObject* parent = nullptr);

    // 析构函数作用：
    // - QObject 父子机制释放网络对象；
    // - 若仍有请求，Qt 会随对象销毁断开回调。
    ~ThreatBookScan() override;

    // scanFile 作用：
    // - 读取设置中的 ThreatBook API Key 并上传指定文件；
    // - 上传后按 SHA-256 轮询报告，完成时弹出结果窗口。
    // 调用方式：scanner->scanFile(path, parentWidget)。
    // 入参 filePath：本地文件路径。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无；异步流程通过弹窗和 kPro 反馈。
    void scanFile(const QString& filePath, QWidget* dialogParent = nullptr);

    // scanFileAndAutoDelete 作用：
    // - 便捷创建堆对象，扫描结束后自动 deleteLater；
    // - 未来 UI 入口若不想保存成员指针，可直接调用。
    // 调用方式：ThreatBookScan::scanFileAndAutoDelete(path, parent)。
    // 入参 filePath：本地文件路径。
    // 入参 dialogParent：弹窗父控件，可为空。
    // 返回：无。
    static void scanFileAndAutoDelete(const QString& filePath, QWidget* dialogParent = nullptr);

private:
    // uploadFile 作用：
    // - 使用 multipart/form-data 调用 ThreatBook file/upload；
    // - 上传成功后继续按 resource 查询报告。
    // 返回：无，异步回调 handleUploadReply。
    void uploadFile();

    // handleUploadReply 作用：
    // - 解析 file/upload 响应；
    // - 成功或样本已存在时进入 report 轮询。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleUploadReply(QNetworkReply* reply);

    // scheduleReportPoll 作用：
    // - 安排下一次 file/report 查询；
    // - 首次查询也通过该函数统一调度。
    // 入参 delayMs：延迟毫秒数。
    // 返回：无。
    void scheduleReportPoll(int delayMs);

    // requestReport 作用：
    // - 调用 ThreatBook file/report 查询指定 SHA-256 报告；
    // - 若服务端仍在分析，会继续 scheduleReportPoll。
    // 返回：无。
    void requestReport();

    // handleReportReply 作用：
    // - 解析报告查询响应；
    // - 已完成时弹出结果，未完成时继续轮询或超时失败。
    // 入参 reply：Qt 网络响应对象。
    // 返回：无。
    void handleReportReply(QNetworkReply* reply);

    // finishWithError 作用：
    // - 统一结束失败流程，更新 kPro、弹出错误并按需自删除。
    // 入参 titleText：弹窗标题。
    // 入参 detailText：错误详情。
    // 返回：无。
    void finishWithError(const QString& titleText, const QString& detailText);

    // finishWithResult 作用：
    // - 统一结束成功流程，更新 kPro 并弹出扫描结果；
    // - 结果弹窗使用 CodeEditorWidget 展示完整 JSON。
    // 入参 reportObject：ThreatBook report JSON 根对象。
    // 返回：无。
    void finishWithResult(const QJsonObject& reportObject);

    // resetRuntimeState 作用：
    // - 清理上一次扫描遗留的文件路径、SHA-256 和轮询计数；
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
    // - 从 ThreatBook report JSON 中提取 summary 与 multiengines 摘要；
    // - 用于结果弹窗顶部的人类可读说明。
    // 入参 reportObject：ThreatBook report JSON 根对象。
    // 返回：摘要文本。
    QString buildResultSummary(const QJsonObject& reportObject) const;

    // reportDataObject 作用：
    // - 兼容 ThreatBook report 响应中 data 直接是报告对象或按 hash 分组的两种形式；
    // - 后续摘要提取统一访问该对象。
    // 入参 reportObject：ThreatBook report JSON 根对象。
    // 返回：提取出的报告 data 对象，缺失时为空对象。
    QJsonObject reportDataObject(const QJsonObject& reportObject) const;

private:
    QNetworkAccessManager* m_networkManager = nullptr; // 网络请求管理器，负责所有 ThreatBook HTTP 调用。
    QPointer<QWidget> m_dialogParent;                  // 弹窗父控件，避免父控件提前销毁后悬空。
    QString m_filePath;                                // 当前待上传的本地文件路径。
    QString m_apiKey;                                  // 当前扫描读取到的 ThreatBook API Key。
    QString m_sha256Text;                              // 当前样本 SHA-256，用作 file/report resource。
    int m_progressPid = 0;                             // kPro 任务 PID，0 表示当前无进度任务。
    int m_pollAttempt = 0;                             // 当前轮询次数，用于超时控制。
    bool m_scanInProgress = false;                     // 当前对象是否已有扫描流程在运行。
    bool m_autoDeleteWhenFinished = false;             // 扫描结束后是否自动 deleteLater。
};
