#pragma once

// ============================================================
// DirectKernelCallMonitorWidget.h
// 作用：
// 1) 为“监控”模块提供“直接内核调用”标签页；
// 2) 基于 Windows ETW System Syscall Provider 采集系统调用事件；
// 3) 通过 ntdll/win32u 导出桩解析 syscall 编号到 Nt*/Zw*/NtUser*/NtGdi* 名称；
// 4) 提供 PID 限定、实时筛选、暂停、导出和详情查看能力。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

class QPoint;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QVBoxLayout;
struct _EVENT_RECORD;

class DirectKernelCallMonitorWidget final : public QWidget
{
public:
    explicit DirectKernelCallMonitorWidget(QWidget* parent = nullptr);
    ~DirectKernelCallMonitorWidget() override;

public:
    enum EventColumn
    {
        EventColumnTime100ns = 0,
        EventColumnPidTid,
        EventColumnProcess,
        EventColumnSyscallNumber,
        EventColumnServiceName,
        EventColumnVerdict,
        EventColumnCallAddress,
        EventColumnEventName,
        EventColumnDetail,
        EventColumnCount
    };

    struct SyscallMapEntry
    {
        std::uint32_t syscallNumber = 0;
        QString serviceName;
        QString sourceModule;
    };

    struct DecodedProperty
    {
        QString name;
        QString valueText;
        std::uint64_t numericValue = 0;
        bool hasNumericValue = false;
    };

    struct ModuleRange
    {
        std::uint64_t startAddress = 0;
        std::uint64_t endAddress = 0;
        QString moduleName;
        QString imagePath;
    };

    struct CapturedEventRow
    {
        QString time100nsText;
        std::uint32_t pid = 0;
        std::uint32_t tid = 0;
        QString pidTidText;
        QString processText;
        std::uint32_t syscallNumber = 0;
        bool hasSyscallNumber = false;
        QString syscallNumberText;
        QString serviceName;
        QString verdictText;
        std::uint64_t callAddress = 0;
        QString callAddressText;
        QString eventName;
        QString detailText;
        QString detailAllText;
        QString globalSearchText;
    };

private:
    void initializeUi();
    void initializeConnections();
    void reloadSyscallMap();
    void startCapture();
    void stopCapture();
    void stopCaptureInternal(bool waitForThread);
    void setCapturePaused(bool paused);
    void updateActionState();
    void updateStatusLabel();
    void flushPendingRows();
    void appendEventRow(const CapturedEventRow& rowValue);
    void scheduleFilterApply();
    void applyFilter();
    void clearFilter();
    void exportVisibleRowsToTsv();
    void showEventContextMenu(const QPoint& position);
    void openEventDetailViewerForRow(int rowIndex);

    static void WINAPI eventRecordCallback(struct _EVENT_RECORD* eventRecordPtr);
    void enqueueEventFromRecord(const struct _EVENT_RECORD* eventRecordPtr);
    CapturedEventRow buildRowFromRecord(const struct _EVENT_RECORD* eventRecordPtr);
    std::vector<DecodedProperty> decodeEventProperties(
        const struct _EVENT_RECORD* eventRecordPtr,
        QString* eventNameOut) const;
    QString serviceNameForNumber(std::uint32_t syscallNumber) const;
    QString processNameForPid(std::uint32_t pid);
    QString moduleNameForAddress(std::uint32_t pid, std::uint64_t addressValue);
    void refreshModuleRangesForPid(std::uint32_t pid);
    std::set<std::uint32_t> parsePidSet(const QString& text) const;
    bool shouldCapturePid(std::uint32_t pid) const;

private:
    QVBoxLayout* m_rootLayout = nullptr;
    QWidget* m_controlPanel = nullptr;
    QLineEdit* m_targetPidEdit = nullptr;
    QCheckBox* m_globalCaptureCheck = nullptr;
    QCheckBox* m_resolveAddressCheck = nullptr;
    QSpinBox* m_maxRowsSpin = nullptr;
    QSpinBox* m_bufferSizeSpin = nullptr;
    QPushButton* m_reloadMapButton = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_pauseButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QPushButton* m_exportButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_mapStatusLabel = nullptr;

    QWidget* m_filterPanel = nullptr;
    QLineEdit* m_processFilterEdit = nullptr;
    QLineEdit* m_serviceFilterEdit = nullptr;
    QLineEdit* m_detailFilterEdit = nullptr;
    QLineEdit* m_globalFilterEdit = nullptr;
    QCheckBox* m_regexCheck = nullptr;
    QCheckBox* m_caseCheck = nullptr;
    QCheckBox* m_invertCheck = nullptr;
    QCheckBox* m_keepBottomCheck = nullptr;
    QPushButton* m_clearFilterButton = nullptr;
    QLabel* m_filterStatusLabel = nullptr;
    QTableWidget* m_eventTable = nullptr;
    QTimer* m_uiUpdateTimer = nullptr;
    QTimer* m_filterDebounceTimer = nullptr;

    std::unordered_map<std::uint32_t, SyscallMapEntry> m_syscallMap;
    mutable std::mutex m_syscallMapMutex;
    std::unordered_map<std::uint32_t, QString> m_processNameCache;
    std::unordered_map<std::uint32_t, std::vector<ModuleRange>> m_moduleRangeCache;
    std::mutex m_cacheMutex;
    std::vector<CapturedEventRow> m_pendingRows;
    std::mutex m_pendingMutex;
    std::set<std::uint32_t> m_capturePidSet;
    mutable std::mutex m_captureConfigMutex;

    std::atomic_bool m_captureRunning{ false };
    std::atomic_bool m_capturePaused{ false };
    std::atomic_bool m_captureStopFlag{ false };
    std::atomic_bool m_captureAllProcesses{ false };
    std::atomic_bool m_resolveCallAddress{ true };
    std::unique_ptr<std::thread> m_captureThread;
    std::atomic<std::uint64_t> m_sessionHandle{ 0 };
    std::atomic<std::uint64_t> m_traceHandle{ 0 };
    QString m_sessionName;
    int m_captureProgressPid = 0;
};

