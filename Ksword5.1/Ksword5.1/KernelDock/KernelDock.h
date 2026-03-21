#pragma once

// ============================================================
// KernelDock.h
// 作用说明：
// 1) 提供“内核对象类型”标签页，展示对象类型编号/名称/句柄统计；
// 2) 提供“NtQuery 信息”标签页，批量调用 NtQuery*Information 系列接口；
// 3) 所有重查询流程走后台线程，避免阻塞主界面。
// ============================================================

#include "../Framework.h"

#include <QWidget>
#include <QString>

#include <atomic>  // std::atomic_bool：控制异步刷新互斥状态。
#include <vector>  // std::vector：保存表格快照数据。

// Qt 前置声明：减少头文件耦合。
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;

// ============================================================
// KernelObjectTypeEntry
// 作用：
// - 表示一个内核对象类型行；
// - 供“对象类型表格 + 详情面板”复用。
// ============================================================
struct KernelObjectTypeEntry
{
    std::uint32_t typeIndex = 0;              // 对象类型编号（例如 File/Process 对应编号）。
    QString typeNameText;                     // 对象类型名称（例如 File、Thread、Mutant）。
    std::uint64_t totalObjectCount = 0;       // 当前系统中该类型对象总数。
    std::uint64_t totalHandleCount = 0;       // 当前系统中该类型句柄总数。
    std::uint32_t validAccessMask = 0;        // 访问掩码（十六进制展示）。
    bool securityRequired = false;            // 是否需要安全检查。
    bool maintainHandleCount = false;         // 是否维护句柄计数。
    std::uint32_t poolType = 0;               // 内核池类型值。
    std::uint32_t defaultPagedPoolCharge = 0; // 默认分页池占用。
    std::uint32_t defaultNonPagedPoolCharge = 0; // 默认非分页池占用。
};

// ============================================================
// KernelNtQueryResultEntry
// 作用：
// - 表示一次 NtQuery 查询结果；
// - 保存表格列信息与完整详情文本。
// ============================================================
struct KernelNtQueryResultEntry
{
    QString categoryText;                     // 类别（系统/进程/线程/对象/令牌/导出）。
    QString functionNameText;                 // 调用函数名（NtQuerySystemInformation 等）。
    QString queryItemText;                    // 查询项名称（例如 ProcessBasicInformation）。
    long statusCode = 0;                      // NTSTATUS 原始码。
    QString statusText;                       // NTSTATUS 可读文本（含十六进制）。
    QString summaryText;                      // 表格摘要文本（简短结果）。
    QString detailText;                       // 详情面板完整文本。
};

// ============================================================
// KernelDock
// 作用：
// - 内核分析主控件；
// - 管理两个标签页与异步刷新流程。
// ============================================================
class KernelDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 UI 与信号槽，并触发首轮异步刷新。
    // - 参数 parent：Qt 父控件。
    explicit KernelDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：默认析构即可，后台线程通过 QPointer 防悬空访问。
    ~KernelDock() override = default;

private:
    // ==================== UI 初始化 ====================
    // initializeUi：
    // - 作用：构建根布局与顶层标签页。
    void initializeUi();

    // initializeKernelTypeTab：
    // - 作用：构建“内核对象类型”页的工具栏/表格/详情区。
    void initializeKernelTypeTab();

    // initializeNtQueryTab：
    // - 作用：构建“NtQuery 信息”页的工具栏/表格/详情区。
    void initializeNtQueryTab();

    // initializeConnections：
    // - 作用：绑定按钮、过滤框、表格选择变化等交互。
    void initializeConnections();

    // ==================== 异步刷新 ====================
    // refreshKernelTypeAsync：
    // - 作用：异步刷新内核对象类型快照并更新 UI。
    void refreshKernelTypeAsync();

    // refreshNtQueryAsync：
    // - 作用：异步执行 NtQuery*Information 调用并更新结果表。
    void refreshNtQueryAsync();

    // ==================== 表格渲染 ====================
    // rebuildKernelTypeTable：
    // - 作用：按关键词重绘对象类型表格。
    // - 参数 filterKeyword：过滤关键词（空串表示不过滤）。
    void rebuildKernelTypeTable(const QString& filterKeyword);

    // rebuildNtQueryTable：
    // - 作用：按 m_ntQueryResults 重绘 NtQuery 结果表。
    void rebuildNtQueryTable();

    // ==================== 详情更新 ====================
    // showKernelTypeDetailByCurrentRow：
    // - 作用：根据对象类型表当前选中行刷新详情文本。
    void showKernelTypeDetailByCurrentRow();

    // showNtQueryDetailByCurrentRow：
    // - 作用：根据 NtQuery 结果表当前选中行刷新详情文本。
    void showNtQueryDetailByCurrentRow();

private:
    // ==================== 根级控件 ====================
    QVBoxLayout* m_rootLayout = nullptr;      // 根布局。
    QTabWidget* m_tabWidget = nullptr;        // 顶层标签页容器。

    // ==================== 内核对象类型页 ====================
    QWidget* m_kernelTypePage = nullptr;      // 对象类型页容器。
    QVBoxLayout* m_kernelTypeLayout = nullptr; // 对象类型页布局。
    QHBoxLayout* m_kernelTypeToolLayout = nullptr; // 对象类型页工具栏布局。
    QPushButton* m_refreshKernelTypeButton = nullptr; // 刷新对象类型按钮。
    QLineEdit* m_kernelTypeFilterEdit = nullptr; // 对象类型关键词过滤输入框。
    QLabel* m_kernelTypeStatusLabel = nullptr; // 对象类型状态文本。
    QTableWidget* m_kernelTypeTable = nullptr; // 对象类型列表表格。
    QPlainTextEdit* m_kernelTypeDetailEdit = nullptr; // 对象类型详情文本。

    // ==================== NtQuery 信息页 ====================
    QWidget* m_ntQueryPage = nullptr;         // NtQuery 页容器。
    QVBoxLayout* m_ntQueryLayout = nullptr;   // NtQuery 页布局。
    QHBoxLayout* m_ntQueryToolLayout = nullptr; // NtQuery 页工具栏布局。
    QPushButton* m_refreshNtQueryButton = nullptr; // 刷新 NtQuery 结果按钮。
    QLabel* m_ntQueryStatusLabel = nullptr;   // NtQuery 状态文本。
    QTableWidget* m_ntQueryTable = nullptr;   // NtQuery 结果表格。
    QPlainTextEdit* m_ntQueryDetailEdit = nullptr; // NtQuery 详情文本。

    // ==================== 数据缓存 ====================
    std::vector<KernelObjectTypeEntry> m_kernelTypeRows; // 对象类型快照行集合。
    std::vector<KernelNtQueryResultEntry> m_ntQueryResults; // NtQuery 结果快照行集合。

    // ==================== 刷新状态 ====================
    std::atomic_bool m_kernelTypeRefreshRunning{ false }; // 对象类型刷新是否进行中。
    std::atomic_bool m_ntQueryRefreshRunning{ false }; // NtQuery 刷新是否进行中。
};
