#pragma once

// KernelShadowComparisonTab.h
// 作用：内核 Shadow 比较页声明——逐条对比 SSDT / ShadowSSDT 的
// 当前值与基线，高亮被修改或重定向的服务项。

#include <QWidget>
#include "KernelServiceAuditUi.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;

// KernelShadowComparisonTab — 内核 Shadow 比较页。
//
// 用途：选择比较对象（SSDT 或 ShadowSSDT），扫描当前服务表，
// 在单张表中逐条列出序号、函数名、当前地址、所属模块和偏移状态，
// 并提供筛选器以快速定位被修改的条目。
//
// 嵌入方式：放入 KernelDock 的"内核审计与回调"内部 Tab Widget。
class KernelShadowComparisonTab final : public QWidget
{
public:
    // 构造：初始化 UI 组件。
    explicit KernelShadowComparisonTab(QWidget* parent = nullptr);

    // 析构：通知后台线程尽快退出。
    ~KernelShadowComparisonTab() override;

protected:
    // showEvent：首次显示时初始化提示文本。
    void showEvent(QShowEvent* event) override;

private:
    // CompareEntry — 单条比较结果。
    struct CompareEntry
    {
        std::uint32_t serviceIndex = 0;         // serviceIndex：服务表序号。
        QString serviceName;                     // serviceName：函数名。
        std::uint64_t serviceRoutineAddress = 0; // serviceRoutineAddress：当前解析地址。
        std::uint64_t currentTableValue = 0;     // currentTableValue：服务表原始值。
        QString moduleName;                      // moduleName：当前落点所属模块。
        ks::kernel::audit::Ownership ownership = ks::kernel::audit::Ownership::Unknown; // 可核验归属。
    };

    // initializeUi：构建工具栏和比较结果表。
    void initializeUi();

    // startComparison：按选定的目标在后台线程扫描。
    void startComparison();

    // cancelComparison：请求取消后台扫描。
    void cancelComparison();

    // populateTable：按当前过滤条件将 m_entries 填入表格。
    void populateTable();

    // hex64：格式化 64 位值为 "0x" 前缀十六进制。
    static QString hex64(std::uint64_t value);

    // --- UI 控件 ---
    QComboBox* m_targetCombo = nullptr;     // m_targetCombo：选择比较对象（SSDT/ShadowSSDT）。
    QPushButton* m_scanButton = nullptr;    // m_scanButton：开始比较。
    QPushButton* m_cancelButton = nullptr;  // m_cancelButton：取消比较。
    QCheckBox* m_suspiciousOnlyCheck = nullptr; // m_suspiciousOnlyCheck：只看可疑条目。
    QLineEdit* m_filterEdit = nullptr;      // m_filterEdit：文本过滤。
    QLabel* m_statusLabel = nullptr;        // m_statusLabel：扫描状态。
    QLabel* m_summaryLabel = nullptr;       // m_summaryLabel：汇总统计。
    QTableWidget* m_resultTable = nullptr;  // m_resultTable：比较结果表。

    // --- 数据 ---
    std::vector<CompareEntry> m_entries;    // m_entries：全量比较结果。
    std::shared_ptr<std::atomic_bool> m_cancelFlag; // m_cancelFlag：后台取消标志。
    bool m_firstShown = false;              // m_firstShown：首次显示标记。
    bool m_scanning = false;                // m_scanning：扫描进行中标记。
};
