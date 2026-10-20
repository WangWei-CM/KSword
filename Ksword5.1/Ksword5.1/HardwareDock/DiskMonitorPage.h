#pragma once

// ============================================================
// DiskMonitorPage.h
// 作用：
// 1) 在“硬件”Dock 下提供独立的“硬盘监控”页；
// 2) 用进程 IO 计数器实现资源监视器风格的进程勾选与磁盘活动聚合；
// 3) 启用 Microsoft-Windows-Kernel-File ETW 文件级聚合，失败时明确提示文件级采集不可用。
// ============================================================

#include "../Framework.h"
#include "DiskMonitorStoragePanel.h"

#include <QHash>
#include <QIcon>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>         // std::atomic_bool：控制后台 ETW 线程退出。
#include <cstdint>        // std::uint32_t/std::uint64_t：PID、字节数与时间戳。
#include <memory>         // std::unique_ptr：托管后台 ETW 线程。
#include <mutex>          // std::mutex：保护 ETW 聚合表。
#include <thread>         // std::thread：后台 ETW 实时会话。
#include <unordered_map>  // std::unordered_map：保存跨采样周期的进程计数器基线。
#include <unordered_set>  // std::unordered_set：保存用户勾选 PID 集合。
#include <vector>         // std::vector：承载采样结果并排序展示。

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QToolButton;
class QVBoxLayout;
struct _EVENT_RECORD;
namespace ks::ui { class AsyncUiDispatcher; }

// DiskMonitorPage 说明：
// - 输入：Qt 父控件；
// - 处理：周期枚举进程 IO_COUNTERS，计算读/写/总速率；空选择显示全部文件活动，勾选后按 PID 过滤；
// - 返回：该控件无业务返回值，结果直接显示在表格中。
class DiskMonitorPage final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - parent：Qt 父对象；
    // - 初始化 UI、连接信号并启动 1 秒刷新定时器。
    explicit DiskMonitorPage(QWidget* parent = nullptr);

    // 析构函数：
    // - 停止刷新定时器；
    // - Qt 子对象由父子树释放。
    ~DiskMonitorPage() override;

protected:
    // 重新显示时提交隐藏期间的最新缓存；不重启采集，不重置速率基线。
    void showEvent(QShowEvent* event) override;

private:
    struct SamplingState;
    // ProcessDiskSample：
    // - 作用：保存单进程本轮磁盘 IO 采样与相邻两轮差值；
    // - 处理：raw* 字段来自 GetProcessIoCounters，rate* 字段由历史样本换算；
    // - 注意：这些计数器覆盖进程文件/设备 IO 传输量，不等同于物理磁盘落盘量；
    // - 返回：结构体本身无返回值。
    struct ProcessDiskSample
    {
        std::uint32_t pid = 0;                 // pid：进程 ID。
        QString processName;                   // processName：进程名。
        QString processImagePath;              // processImagePath：进程路径，权限不足时可为空。
        std::uint32_t threadCount = 0;          // threadCount：进程线程数。
        std::uint64_t rawReadBytes = 0;         // rawReadBytes：累计读取传输字节。
        std::uint64_t rawWriteBytes = 0;        // rawWriteBytes：累计写入传输字节。
        std::uint64_t rawOtherBytes = 0;        // rawOtherBytes：累计其它 IO 传输字节。
        std::uint64_t rawReadOps = 0;           // rawReadOps：累计读取次数。
        std::uint64_t rawWriteOps = 0;          // rawWriteOps：累计写入次数。
        std::uint64_t rawOtherOps = 0;          // rawOtherOps：累计其它 IO 次数。
        QString ioPriorityText;                 // ioPriorityText：进程当前 I/O 优先级，权限不足时为空。
        double readBytesPerSec = 0.0;           // readBytesPerSec：读取 B/s。
        double writeBytesPerSec = 0.0;          // writeBytesPerSec：写入 B/s。
        double totalBytesPerSec = 0.0;          // totalBytesPerSec：读写合计 B/s。
        double readOpsPerSec = 0.0;             // readOpsPerSec：读次数/s。
        double writeOpsPerSec = 0.0;            // writeOpsPerSec：写次数/s。
        double otherBytesPerSec = 0.0;          // otherBytesPerSec：其它 IO B/s。
        double responseTimeMs = 0.0;            // responseTimeMs：基于吞吐/次数估算的平均响应时间。
        bool countersReady = false;             // countersReady：是否成功读取动态计数器。
        bool rateReady = false;                 // rateReady：是否已有历史样本可计算速率。
    };

    // ProcessDiskBaseline：
    // - 作用：保存上次采样的原始累计值；
    // - 处理：用 PID + 创建时间作为 identity，降低 PID 复用误差；
    // - 返回：结构体无返回值。
    struct ProcessDiskBaseline
    {
        std::uint64_t identityCreateTime100ns = 0; // identityCreateTime100ns：进程创建时间。
        std::uint64_t sampleTickMs = 0;            // sampleTickMs：采样时刻毫秒。
        std::uint64_t rawReadBytes = 0;            // rawReadBytes：上次累计读字节。
        std::uint64_t rawWriteBytes = 0;           // rawWriteBytes：上次累计写字节。
        std::uint64_t rawOtherBytes = 0;           // rawOtherBytes：上次累计其它字节。
        std::uint64_t rawReadOps = 0;              // rawReadOps：上次累计读次数。
        std::uint64_t rawWriteOps = 0;             // rawWriteOps：上次累计写次数。
        std::uint64_t rawOtherOps = 0;             // rawOtherOps：上次累计其它次数。
    };

    // FileActivitySample：
    // - 作用：保存 Kernel-File ETW 事件按 PID + 文件路径聚合后的 1 秒活动；
    // - 处理：read/write 字段来自实时 ETW 字节累加，进程名和 I/O 优先级由采集链路补齐；
    // - 返回：结构体仅用于 UI 展示，不持有系统资源。
    struct FileActivitySample
    {
        std::uint32_t pid = 0;             // pid：触发文件 IO 的进程 ID。
        QString processName;               // processName：进程名。
        QString processImagePath;          // processImagePath：用于活动表进程图标与筛选。
        QString filePath;                  // filePath：ETW 解析到的文件路径。
        double readBytesPerSec = 0.0;      // readBytesPerSec：文件读取速率。
        double writeBytesPerSec = 0.0;     // writeBytesPerSec：文件写入速率。
        QString ioPriorityText;            // ioPriorityText：I/O 优先级文本，未知时显示“未知”。
        double responseTimeMs = 0.0;       // responseTimeMs：ETW 可得时的平均响应时间。
        bool responseAvailable = false;    // responseAvailable：是否有真实/事件字段响应时间。
        std::uint32_t eventCount = 0;       // eventCount：本窗口内聚合的 ETW 事件数。
    };

    // FileActivityAccumulator：
    // - 作用：在 ETW 回调线程内累计当前刷新窗口的文件活动；
    // - 处理：采样 worker 每秒交换并清空，换算成 FileActivitySample；
    // - 返回：结构体不返回数据。
    struct FileActivityAccumulator
    {
        std::uint32_t pid = 0;                  // pid：进程 ID。
        QString filePath;                       // filePath：文件路径。
        std::uint64_t readBytes = 0;            // readBytes：窗口内读取字节。
        std::uint64_t writeBytes = 0;           // writeBytes：窗口内写入字节。
        QString ioPriorityText;                 // ioPriorityText：窗口内最近一次可用 I/O 优先级。
        double responseMsTotal = 0.0;           // responseMsTotal：响应时间累计。
        std::uint32_t responseCount = 0;        // responseCount：响应时间样本数。
        std::uint32_t eventCount = 0;           // eventCount：事件数。
    };

    // FileActivityStateEntry：
    // - 作用：为同一 PID + 文件路径保存唯一且持续更新的活动行；
    // - 处理：每轮用最新窗口速率覆盖，空闲超过保留窗口或进程退出时删除；
    // - 返回：结构体只参与采样 worker 活动状态维护，不直接返回系统资源。
    struct FileActivityStateEntry
    {
        std::uint64_t lastActivityMs = 0;        // lastActivityMs：最近一次读写开始或完成的单调时间。
        FileActivitySample sample;               // sample：该逻辑活动当前刷新窗口的展示值。
    };

    // PendingFileIoOperation：
    // - 作用：保存 Read/Write 发起事件到 OperationEnd 完成事件之间的临时状态；
    // - 处理：用 IRP 指针关联开始/结束，完成时把真实耗时写回文件活动聚合；
    // - 返回：结构体只作为回调线程缓存，不向外返回。
    struct PendingFileIoOperation
    {
        std::uint32_t pid = 0;                  // pid：发起 I/O 的进程 ID。
        QString filePath;                       // filePath：发起 I/O 时解析到的文件路径。
        QString ioPriorityText;                 // ioPriorityText：发起 I/O 时解析到的优先级文本。
        std::uint64_t startTime100ns = 0;       // startTime100ns：ETW 时间戳，ClientContext=2 时为 100ns。
    };

    // ===================== UI 初始化 =====================
    void initializeUi();
    void initializeConnections();
    void configureTableWidget(QTableWidget* tableWidget, int processIdColumn = -1) const;

    // startInitialSampling 作用：
    // - 输入：无，构造后由 QTimer 延迟调用；
    // - 处理：启动文件 ETW、执行首轮进程 IO 采样并启动周期定时器；
    // - 返回：无返回值，重复调用会被 m_initialSamplingStarted 拦截。
    void startInitialSampling();

    // ===================== 采样与刷新 =====================
    void refreshNow();
    void applyProcessDiskSamples(
        std::vector<ProcessDiskSample> sampleList,
        std::vector<FileActivitySample> fileSamples,
        DiskMonitorStorageBatch storageSampleBatch);
    static std::vector<ProcessDiskSample> collectProcessDiskSamples(SamplingState& state);
    static std::vector<FileActivitySample> consumeFileActivitySamples(
        SamplingState& state, const std::vector<ProcessDiskSample>& sampleList);
    void pruneStaleSelection(const std::vector<ProcessDiskSample>& sampleList);
    void updateProcessTable(const std::vector<ProcessDiskSample>& sampleList);
    void updateActivityTable();
    void updateSummaryLabels(const std::vector<ProcessDiskSample>& sampleList);
    // 仅在可见且允许提交时更新两张活动表，合并隐藏期间多轮 UI 重建。
    void refreshProcessPresentation();
    void syncSelectionFromTable();

    // installProcessColumnMenu：
    // - 输入：无；
    // - 处理：为进程表表头增加逐列显隐菜单，默认显示全部字段；
    // - 返回：无。
    void installProcessColumnMenu();

    // ===================== ETW 文件活动采集 =====================
    void startFileActivityEtw();
    void stopFileActivityEtw();
    static void WINAPI fileActivityEtwCallback(struct _EVENT_RECORD* eventRecordPointer);
    static void handleFileActivityEtwEvent(
        SamplingState& state, const struct _EVENT_RECORD* eventRecordPointer);

    // ===================== 表格工具 =====================
    QTableWidgetItem* createReadOnlyItem(const QString& text) const;
    QTableWidgetItem* createNumericItem(const QString& text, double numericValue) const;
    void setTableItemText(
        QTableWidget* tableWidget,
        int rowIndex,
        int columnIndex,
        QTableWidgetItem* itemPointer) const;
    void applyProcessRowCheckState(QTableWidgetItem* checkItem, std::uint32_t pid) const;
    QString processSearchText(const ProcessDiskSample& sample) const;
    bool sampleMatchesFilter(const ProcessDiskSample& sample) const;
    bool activityMatchesFilter(const FileActivitySample& sample) const;
    QIcon processIconForPath(const QString& imagePath);

    // ===================== 格式化工具 =====================
    QString formatBytesPerSecond(double bytesPerSecond) const;
    QString formatBytes(double bytesValue) const;
    QString formatOpsPerSecond(double opsPerSecond) const;
    QString formatMilliseconds(double milliseconds) const;

private:
    std::shared_ptr<SamplingState> m_samplingState;
    std::shared_ptr<ks::ui::AsyncUiDispatcher> m_dispatcher;
    QVBoxLayout* m_rootLayout = nullptr;           // m_rootLayout：根布局。
    QLabel* m_titleLabel = nullptr;                // m_titleLabel：标题标签。
    QLabel* m_statusLabel = nullptr;               // m_statusLabel：刷新状态标签。
    QLabel* m_summaryLabel = nullptr;              // m_summaryLabel：总读写速率摘要。
    QLineEdit* m_filterEdit = nullptr;             // m_filterEdit：进程过滤输入框。
    QCheckBox* m_onlyActiveCheckBox = nullptr;     // m_onlyActiveCheckBox：仅显示活跃 IO 进程。
    QPushButton* m_refreshButton = nullptr;        // m_refreshButton：手动刷新按钮。
    QPushButton* m_selectActiveButton = nullptr;   // m_selectActiveButton：勾选当前活跃进程。
    QPushButton* m_clearSelectionButton = nullptr; // m_clearSelectionButton：清空勾选按钮。
    QSplitter* m_splitter = nullptr;               // m_splitter：上下表格分割器。
    QToolButton* m_processSectionButton = nullptr; // m_processSectionButton：进程活动折叠标题。
    QToolButton* m_activitySectionButton = nullptr;// m_activitySectionButton：文件活动折叠标题。
    QToolButton* m_storageSectionButton = nullptr; // m_storageSectionButton：存储折叠标题。
    QTableWidget* m_processTable = nullptr;        // m_processTable：进程级磁盘速率表。
    QTableWidget* m_activityTable = nullptr;       // m_activityTable：全部或按勾选 PID 过滤的磁盘活动表。
    DiskMonitorStoragePanel* m_storagePanel = nullptr; // m_storagePanel：固定卷容量与性能区。
    QTimer* m_refreshTimer = nullptr;              // m_refreshTimer：周期刷新定时器。
    bool m_initialSamplingStarted = false;         // m_initialSamplingStarted：是否已经启动 ETW 与首轮采样。
    bool m_processPresentationDirty = false;       // 最新采样已入缓存，但活动表尚未显示该轮内容。
    std::unique_ptr<std::thread> m_processSamplingThread; // m_processSamplingThread：后台进程 IO 采样线程。

    std::unordered_set<std::uint32_t> m_selectedPidSet; // m_selectedPidSet：用户勾选 PID 集。
    std::vector<ProcessDiskSample> m_lastSampleList;    // m_lastSampleList：最近一次采样结果。
    std::vector<FileActivitySample> m_lastFileActivityList; // m_lastFileActivityList：最近活动窗口内的唯一 PID+路径状态行。
    QHash<QString, QIcon> m_processIconByPath;      // 映像路径到 16px shell 图标的 UI 缓存。
    bool m_updatingProcessTable = false;                // m_updatingProcessTable：防止程序刷新触发递归勾选同步。

    std::unique_ptr<std::thread> m_fileActivityEtwThread; // m_fileActivityEtwThread：后台 ETW 会话线程。
};
