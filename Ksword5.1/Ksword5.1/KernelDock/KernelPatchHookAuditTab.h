#pragma once

// KernelPatchHookAuditTab.h
// 作用：统一内核修改审计页声明——将 Inline Hook、IAT/EAT、SSDT、
// ObjectType 方法指针和驱动完整性审计合并在一张表中按风险等级展示。

#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;

// KernelPatchHookAuditTab — 统一内核修改审计页。
//
// 用途：一键扫描所有内核 hook/patch 类型（Inline Hook、IAT/EAT Hook、
// SSDT/ShadowSSDT 篡改、ObjectType 方法指针重定向、驱动完整性证据），
// 结果汇入同一张表格按风险分级展示。
//
// 嵌入方式：放入 KernelDock 的"内核审计与回调"内部 Tab Widget。
// 首次显示时不自动扫描，由用户点击"开始审计"触发。
class KernelPatchHookAuditTab final : public QWidget
{
public:
    // 构造：初始化 UI 组件。
    explicit KernelPatchHookAuditTab(QWidget* parent = nullptr);

    // 析构：通知后台线程尽快退出。
    ~KernelPatchHookAuditTab() override;

protected:
    // showEvent：首次显示时初始化状态提示文本。
    void showEvent(QShowEvent* event) override;

private:
    // AuditEntry — 统一审计结果条目。
    // 不同检测类型的发现被转化为统一格式在同一张表中展示。
    struct AuditEntry
    {
        QString category;               // category：检测类别（Inline/IAT/EAT/SSDT/…）。
        QString moduleName;             // moduleName：所属或宿主模块名。
        QString targetName;             // targetName：函数名、服务号、类型名等标识。
        std::uint64_t address = 0;      // address：当前地址或跳转目标。
        std::uint64_t expected = 0;     // expected：期望/基线地址（无基线时为 0）。
        QString riskLevel;              // riskLevel：风险等级文本（Critical/Warning/Info）。
        QString detail;                 // detail：补充说明。
        QString currentBytes;           // currentBytes：当前字节十六进制（如适用）。
        QString baselineBytes;          // baselineBytes：基线字节十六进制（如适用）。
    };

    // initializeUi：构建工具栏、过滤器和结果表格。
    void initializeUi();

    // startFullAudit：在后台线程启动全类型扫描。
    void startFullAudit();

    // cancelAudit：请求取消正在进行的后台扫描。
    void cancelAudit();

    // populateTable：按当前过滤条件将 m_allEntries 填入表格。
    void populateTable();

    // --- 各子扫描函数（static，不访问成员，可在后台线程安全调用）---

    // scanInlineHooks：扫描 Inline Hook，可疑项追加到 out。
    static void scanInlineHooks(
        std::vector<AuditEntry>& out,
        const std::shared_ptr<std::atomic_bool>& cancel);

    // scanIatEatHooks：扫描 IAT/EAT 重定向，可疑项追加到 out。
    static void scanIatEatHooks(
        std::vector<AuditEntry>& out,
        const std::shared_ptr<std::atomic_bool>& cancel);

    // scanSsdtEntries：扫描 SSDT 和 ShadowSSDT，非内核模块的服务追加。
    static void scanSsdtEntries(
        std::vector<AuditEntry>& out,
        const std::shared_ptr<std::atomic_bool>& cancel);

    // scanObjectTypeProcedures：扫描 ObjectType 方法指针，有风险的追加。
    static void scanObjectTypeProcedures(
        std::vector<AuditEntry>& out,
        const std::shared_ptr<std::atomic_bool>& cancel);

    // scanDriverIntegrity：查询驱动/IDT 完整性证据，有风险标志的追加。
    static void scanDriverIntegrity(
        std::vector<AuditEntry>& out,
        const std::shared_ptr<std::atomic_bool>& cancel);

    // hex64：格式化 64 位值为 "0x" 前缀十六进制字符串。
    static QString hex64(std::uint64_t value);

    // bytesToHex：将字节向量转为空格分隔的十六进制字符串。
    static QString bytesToHex(const std::vector<std::uint8_t>& bytes);

    // riskSortKey：返回风险等级的排序权重（Critical=0, Warning=1, Info=2）。
    static int riskSortKey(const QString& risk);

    // --- UI 控件 ---
    QLabel* m_statusLabel = nullptr;            // m_statusLabel：扫描状态文字。
    QLabel* m_summaryLabel = nullptr;           // m_summaryLabel：汇总计数。
    QLineEdit* m_filterEdit = nullptr;          // m_filterEdit：文本过滤输入。
    QComboBox* m_riskFilterCombo = nullptr;     // m_riskFilterCombo：风险等级过滤。
    QComboBox* m_categoryFilterCombo = nullptr; // m_categoryFilterCombo：类别过滤。
    QPushButton* m_scanButton = nullptr;        // m_scanButton：开始审计。
    QPushButton* m_cancelButton = nullptr;      // m_cancelButton：取消审计。
    QTableWidget* m_resultTable = nullptr;      // m_resultTable：统一结果表。

    // --- 数据 ---
    std::vector<AuditEntry> m_allEntries;       // m_allEntries：全量审计结果。
    std::shared_ptr<std::atomic_bool> m_cancelFlag; // m_cancelFlag：后台取消标志。
    bool m_firstShown = false;                  // m_firstShown：首次显示标记。
    bool m_scanning = false;                    // m_scanning：扫描进行中标记。
};
