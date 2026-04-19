#pragma once

// ============================================================
// KernelDock.h
// 作用说明：
// 1) 提供“对象命名空间遍历”标签页（默认页）；
// 2) 提供“原子表遍历”标签页；
// 3) 保留旧版“NtQuery 信息”标签页到历史页；
// 4) 全部耗时查询走后台线程，避免阻塞 UI 线程。
// ============================================================

#include "../Framework.h"

#include <QWidget>
#include <QString>

#include <atomic>   // std::atomic_bool：控制异步刷新互斥。
#include <cstdint>  // std::uintXX_t：固定宽度整数。
#include <vector>   // std::vector：缓存表格快照行。

// Qt 前置声明：减少头文件耦合。
class QPoint;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QVBoxLayout;
class CodeEditorWidget;

// ============================================================
// KernelObjectTypeEntry
// 作用：
// - 兼容旧 Worker 保留的数据结构；
// - 当前 UI 已不直接显示该结构，但保留以避免耦合回归。
// ============================================================
struct KernelObjectTypeEntry
{
    std::uint32_t typeIndex = 0;                 // typeIndex：对象类型编号。
    QString typeNameText;                        // typeNameText：对象类型名。
    std::uint64_t totalObjectCount = 0;          // totalObjectCount：对象数量。
    std::uint64_t totalHandleCount = 0;          // totalHandleCount：句柄数量。
    std::uint32_t validAccessMask = 0;           // validAccessMask：访问掩码。
    bool securityRequired = false;               // securityRequired：是否要求安全检查。
    bool maintainHandleCount = false;            // maintainHandleCount：是否维护句柄计数。
    std::uint32_t poolType = 0;                  // poolType：池类型。
    std::uint32_t defaultPagedPoolCharge = 0;    // defaultPagedPoolCharge：分页池默认配额。
    std::uint32_t defaultNonPagedPoolCharge = 0; // defaultNonPagedPoolCharge：非分页池默认配额。
};

// ============================================================
// KernelNtQueryResultEntry
// 作用：
// - 表示一次 NtQuery 查询结果；
// - 保存表格列信息与完整详情文本。
// ============================================================
struct KernelNtQueryResultEntry
{
    QString categoryText;     // categoryText：类别（系统/进程/线程/对象/令牌/导出）。
    QString functionNameText; // functionNameText：调用函数名。
    QString queryItemText;    // queryItemText：查询项名称。
    long statusCode = 0;      // statusCode：NTSTATUS 原始码。
    QString statusText;       // statusText：可读状态文本。
    QString summaryText;      // summaryText：表格摘要文本。
    QString detailText;       // detailText：详情面板完整文本。
};

// ============================================================
// KernelObjectNamespaceEntry
// 作用：
// - 表示对象管理器命名空间的一条枚举结果；
// - 同时承载“目录路径 + 枚举 API + 对象详情 + 操作信息”。
// ============================================================
struct KernelObjectNamespaceEntry
{
    QString rootPathText;            // rootPathText：根目录（如 \Device）。
    QString scopeDescriptionText;    // scopeDescriptionText：该根目录用途说明。
    QString directoryPathText;       // directoryPathText：当前被枚举目录路径。
    QString objectNameText;          // objectNameText：对象名。
    QString objectTypeText;          // objectTypeText：对象类型（Directory/SymbolicLink 等）。
    QString fullPathText;            // fullPathText：对象完整路径。
    QString enumApiText;             // enumApiText：本条记录使用的枚举 API。
    QString symbolicLinkTargetText;  // symbolicLinkTargetText：符号链接目标（非链接可为空）。
    QString statusText;              // statusText：状态文本（成功/失败 + 码）。
    QString detailText;              // detailText：详情面板展示文本。
    long statusCode = 0;             // statusCode：原始 NTSTATUS。
    bool querySucceeded = false;     // querySucceeded：枚举是否成功。
    bool isDirectory = false;        // isDirectory：该对象是否目录类型。
    bool isSymbolicLink = false;     // isSymbolicLink：该对象是否符号链接类型。
};

// ============================================================
// KernelAtomEntry
// 作用：
// - 表示原子表中的一条记录；
// - 统一承载 Atom 值、名称、来源与详情。
// ============================================================
struct KernelAtomEntry
{
    std::uint16_t atomValue = 0; // atomValue：Atom 十进制值。
    QString atomNameText;        // atomNameText：Atom 名称。
    QString sourceText;          // sourceText：来源（GlobalAtom/ClipboardFormat 等）。
    QString statusText;          // statusText：状态文本。
    QString detailText;          // detailText：详情文本。
    bool querySucceeded = false; // querySucceeded：该条目是否有效。
};

// ============================================================
// KernelDock
// 作用：
// - 内核分析主控件；
// - 管理对象命名空间/原子表/历史 NtQuery 三个页面；
// - 管理异步刷新、筛选、详情联动与右键菜单操作。
// ============================================================
class KernelDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 UI、连接信号槽并触发首轮异步刷新。
    // - 参数 parent：Qt 父控件。
    explicit KernelDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 资源由 Qt 父子机制与容器自动释放。
    ~KernelDock() override = default;

private:
    // ==================== UI 初始化 ====================
    // initializeUi：
    // - 作用：创建根布局与顶层 Tab 容器。
    void initializeUi();

    // initializeObjectNamespaceTab：
    // - 作用：创建“对象命名空间遍历”页（默认页）。
    void initializeObjectNamespaceTab();

    // initializeAtomTableTab：
    // - 作用：创建“原子表遍历”页。
    void initializeAtomTableTab();

    // initializeNtQueryTab：
    // - 作用：创建“历史 NtQuery 信息”页。
    void initializeNtQueryTab();

    // initializeConnections：
    // - 作用：绑定按钮、筛选框、表格联动与右键菜单。
    void initializeConnections();

    // ==================== 异步刷新 ====================
    // refreshObjectNamespaceAsync：
    // - 作用：后台刷新对象命名空间枚举结果。
    void refreshObjectNamespaceAsync();

    // refreshAtomTableAsync：
    // - 作用：后台刷新原子表枚举结果。
    void refreshAtomTableAsync();

    // refreshNtQueryAsync：
    // - 作用：后台刷新历史 NtQuery 结果。
    void refreshNtQueryAsync();

    // ==================== 表格渲染 ====================
    // rebuildObjectNamespaceTable：
    // - 作用：根据筛选关键词重建对象命名空间树。
    // - 参数 filterKeyword：筛选关键词（空表示不过滤）。
    void rebuildObjectNamespaceTable(const QString& filterKeyword);

    // rebuildObjectNamespacePropertyTable：
    // - 作用：重建“对象属性项/值”表，展示当前节点或对象记录的详细字段。
    // - 参数 entry：当前对象记录；为 nullptr 时展示树节点摘要。
    // - 参数 nodeNameText：树节点名称（entry 为 nullptr 时使用）。
    // - 参数 nodeTypeText：树节点类型（entry 为 nullptr 时使用）。
    // - 参数 nodePathText：树节点路径（entry 为 nullptr 时使用）。
    // - 参数 nodeDescriptionText：树节点说明（entry 为 nullptr 时使用）。
    void rebuildObjectNamespacePropertyTable(
        const KernelObjectNamespaceEntry* entry,
        const QString& nodeNameText,
        const QString& nodeTypeText,
        const QString& nodePathText,
        const QString& nodeDescriptionText);

    // selectFirstObjectNamespaceEntryItem：
    // - 作用：在树中定位并选中第一条对象记录节点（非根/目录摘要节点）。
    void selectFirstObjectNamespaceEntryItem();

    // rebuildAtomTable：
    // - 作用：根据筛选关键词重建原子表格。
    // - 参数 filterKeyword：筛选关键词（空表示不过滤）。
    void rebuildAtomTable(const QString& filterKeyword);

    // rebuildNtQueryTable：
    // - 作用：重建历史 NtQuery 结果表格。
    void rebuildNtQueryTable();

    // ==================== 详情联动 ====================
    // showObjectNamespaceDetailByCurrentRow：
    // - 作用：根据当前选中行显示对象命名空间详情。
    void showObjectNamespaceDetailByCurrentRow();

    // showAtomDetailByCurrentRow：
    // - 作用：根据当前选中行显示原子详情。
    void showAtomDetailByCurrentRow();

    // showNtQueryDetailByCurrentRow：
    // - 作用：根据当前选中行显示 NtQuery 详情。
    void showNtQueryDetailByCurrentRow();

    // ==================== 右键菜单 ====================
    // showObjectNamespaceContextMenu：
    // - 作用：弹出对象命名空间表格右键菜单（复制 + 对象操作）。
    // - 参数 localPosition：表格视口坐标。
    void showObjectNamespaceContextMenu(const QPoint& localPosition);

    // showAtomContextMenu：
    // - 作用：弹出原子表格右键菜单（复制 + 原子操作）。
    // - 参数 localPosition：表格视口坐标。
    void showAtomContextMenu(const QPoint& localPosition);

    // ==================== 当前行索引辅助 ====================
    // currentObjectNamespaceSourceIndex：
    // - 作用：读取对象命名空间当前行映射到缓存向量的索引。
    // - 传出 sourceIndexOut：缓存索引。
    // - 返回：true=读取成功；false=无选中或越界。
    bool currentObjectNamespaceSourceIndex(std::size_t& sourceIndexOut) const;

    // currentAtomSourceIndex：
    // - 作用：读取原子表当前行映射到缓存向量的索引。
    // - 传出 sourceIndexOut：缓存索引。
    // - 返回：true=读取成功；false=无选中或越界。
    bool currentAtomSourceIndex(std::size_t& sourceIndexOut) const;

    // currentObjectNamespaceEntry：
    // - 作用：返回对象命名空间当前选中项。
    // - 返回：命中返回指针；否则 nullptr。
    const KernelObjectNamespaceEntry* currentObjectNamespaceEntry() const;

    // currentAtomEntry：
    // - 作用：返回原子表当前选中项。
    // - 返回：命中返回指针；否则 nullptr。
    const KernelAtomEntry* currentAtomEntry() const;

private:
    // ==================== 根级控件 ====================
    QVBoxLayout* m_rootLayout = nullptr; // m_rootLayout：KernelDock 根布局。
    QTabWidget* m_tabWidget = nullptr;   // m_tabWidget：顶层 Tab 容器。

    // ==================== 对象命名空间页 ====================
    QWidget* m_objectNamespacePage = nullptr;                  // m_objectNamespacePage：对象命名空间页容器。
    QVBoxLayout* m_objectNamespaceLayout = nullptr;            // m_objectNamespaceLayout：对象命名空间页布局。
    QHBoxLayout* m_objectNamespaceToolLayout = nullptr;        // m_objectNamespaceToolLayout：对象命名空间工具栏布局。
    QPushButton* m_refreshObjectNamespaceButton = nullptr;     // m_refreshObjectNamespaceButton：对象命名空间刷新按钮。
    QLineEdit* m_objectNamespaceFilterEdit = nullptr;          // m_objectNamespaceFilterEdit：对象命名空间关键词过滤输入框。
    QLabel* m_objectNamespaceStatusLabel = nullptr;            // m_objectNamespaceStatusLabel：对象命名空间状态文本。
    QTreeWidget* m_objectNamespaceTree = nullptr;              // m_objectNamespaceTree：对象命名空间树（文件管理器式结构）。
    QTableWidget* m_objectNamespacePropertyTable = nullptr;    // m_objectNamespacePropertyTable：对象属性项/值表。
    CodeEditorWidget* m_objectNamespaceDetailEditor = nullptr; // m_objectNamespaceDetailEditor：对象命名空间详情编辑器（只读）。

    // ==================== 原子表页 ====================
    QWidget* m_atomPage = nullptr;                  // m_atomPage：原子表页容器。
    QVBoxLayout* m_atomLayout = nullptr;            // m_atomLayout：原子表页布局。
    QHBoxLayout* m_atomToolLayout = nullptr;        // m_atomToolLayout：原子表工具栏布局。
    QPushButton* m_refreshAtomButton = nullptr;     // m_refreshAtomButton：原子表刷新按钮。
    QLineEdit* m_atomFilterEdit = nullptr;          // m_atomFilterEdit：原子关键词过滤输入框。
    QLabel* m_atomStatusLabel = nullptr;            // m_atomStatusLabel：原子表状态文本。
    QTableWidget* m_atomTable = nullptr;            // m_atomTable：原子结果表。
    CodeEditorWidget* m_atomDetailEditor = nullptr; // m_atomDetailEditor：原子详情编辑器（只读）。

    // ==================== 历史 NtQuery 页 ====================
    QWidget* m_ntQueryPage = nullptr;                  // m_ntQueryPage：历史 NtQuery 页容器。
    QVBoxLayout* m_ntQueryLayout = nullptr;            // m_ntQueryLayout：历史 NtQuery 页布局。
    QHBoxLayout* m_ntQueryToolLayout = nullptr;        // m_ntQueryToolLayout：历史 NtQuery 工具栏布局。
    QPushButton* m_refreshNtQueryButton = nullptr;     // m_refreshNtQueryButton：历史 NtQuery 刷新按钮。
    QLabel* m_ntQueryStatusLabel = nullptr;            // m_ntQueryStatusLabel：历史 NtQuery 状态文本。
    QTableWidget* m_ntQueryTable = nullptr;            // m_ntQueryTable：历史 NtQuery 结果表。
    CodeEditorWidget* m_ntQueryDetailEditor = nullptr; // m_ntQueryDetailEditor：历史 NtQuery 详情编辑器（只读）。

    // ==================== 数据缓存 ====================
    std::vector<KernelObjectNamespaceEntry> m_objectNamespaceRows; // m_objectNamespaceRows：对象命名空间快照行。
    std::vector<KernelAtomEntry> m_atomRows;                       // m_atomRows：原子快照行。
    std::vector<KernelNtQueryResultEntry> m_ntQueryResults;        // m_ntQueryResults：历史 NtQuery 快照行。

    // ==================== 刷新状态 ====================
    std::atomic_bool m_objectNamespaceRefreshRunning{ false }; // m_objectNamespaceRefreshRunning：对象命名空间刷新状态。
    std::atomic_bool m_atomRefreshRunning{ false };            // m_atomRefreshRunning：原子表刷新状态。
    std::atomic_bool m_ntQueryRefreshRunning{ false };         // m_ntQueryRefreshRunning：历史 NtQuery 刷新状态。
};
