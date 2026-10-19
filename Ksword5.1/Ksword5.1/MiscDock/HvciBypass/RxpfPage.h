#pragma once

// ============================================================
// RxpfPage.h
// 作用：
// - RXPF（RX Page Fault）模拟执行实验页面；
// - 接管 IDT #PF → 对 NX 内核映像页进行 x64 指令模拟，
//   无需恢复 RX 权限即可在 HVCI 下执行被修改的代码页；
// - 提供完整的操作流程：查询支持→注册→变更保护→写入→
//   启用模拟→统计→事件→自检→注销；
// - 后端逻辑由 RxpfBackend 提供，本页只组合共享组件。
// ============================================================

#include "RxpfBackend.h"

#include "../../Framework.h"

#include <QWidget>

#include <cstdint>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class CodeEditorWidget;

namespace ks::misc
{
    // RxpfPage：
    // - 作用：在杂项 Tab 中提供 #PF 模拟执行实验界面；
    // - 布局：使用 ParameterWorkspace 两栏结构，参数独立滚动；
    // - 页面不直接接触驱动设备，所有 R0 调用委托给 RxpfBackend。
    class RxpfPage final : public QWidget
    {
    public:
        // 构造函数：建出界面控件与连接，不执行任何 R0 查询。
        explicit RxpfPage(QWidget* parent = nullptr);
        ~RxpfPage() override = default;

    protected:
        // showEvent：首次可见时查询 RXPF 支持状态。
        void showEvent(QShowEvent* event) override;
        // changeEvent：跟随主题切换刷新风险横幅配色。
        void changeEvent(QEvent* event) override;

    private:
        // initializeUi：构建根布局、参数区、动作区和结果区。
        void initializeUi();
        // initializeConnections：把按钮信号连接到处理函数。
        void initializeConnections();
        // applyBannerStyle：下发风险横幅样式。
        void applyBannerStyle();

        // ===================== 操作处理函数 =====================
        void doQuerySupport();      // 查询 RXPF 子系统支持状态。
        void doRegister();          // 注册一个内核页面用于模拟。
        void doChangeProtection();  // 将页面保护从 RX 改为 NX/RW。
        void doWritePage();         // 向页面可写别名写入数据。
        void doSetEmulation();      // 启用或禁用指令模拟。
        void doQueryPage();         // 查询页面当前状态。
        void doQueryStats();        // 查询全局统计。
        void doDrainEvents();       // 拉取模拟事件。
        void doSelfTest();          // 运行端到端自检。
        void doUnregister();        // 注销页面并恢复保护。

        // ===================== UI 辅助 =====================
        // updateStateDisplay：根据当前页面状态刷新状态区和按钮可用性。
        void updateStateDisplay(const rxpf::PageInfo& info);
        // appendTrace：追加一行操作轨迹到日志区。
        void appendTrace(const QString& line);
        // setBusy：操作进行中禁用按钮。
        void setBusy(bool busy);

    private:
        // ===================== 风险/状态区 =====================
        QLabel* m_warningLabel = nullptr;       // m_warningLabel：风险横幅。
        QLabel* m_supportLabel = nullptr;       // m_supportLabel：RXPF 支持状态摘要。

        // ===================== 参数区 =====================
        QComboBox* m_targetKindCombo = nullptr; // m_targetKindCombo：目标类型下拉框。
        QLineEdit* m_targetAddressEdit = nullptr; // m_targetAddressEdit：目标地址输入。
        QLineEdit* m_writeOffsetEdit = nullptr; // m_writeOffsetEdit：写入偏移。
        QLineEdit* m_writeBytesEdit = nullptr;  // m_writeBytesEdit：写入数据（十六进制）。

        // ===================== 状态显示区 =====================
        QLabel* m_recordIdLabel = nullptr;      // m_recordIdLabel：当前记录 ID。
        QLabel* m_pageStateLabel = nullptr;     // m_pageStateLabel：页面状态。
        QLabel* m_countersLabel = nullptr;      // m_countersLabel：计数器。

        // ===================== 动作区按钮 =====================
        QPushButton* m_querySupportBtn = nullptr;  // 查询支持。
        QPushButton* m_selfTestBtn = nullptr;      // 运行自检。
        QPushButton* m_queryStatsBtn = nullptr;    // 查询统计。
        QPushButton* m_drainEventsBtn = nullptr;   // 拉取事件。
        QPushButton* m_registerBtn = nullptr;      // 注册页面。
        QPushButton* m_changeBtn = nullptr;        // 变更保护。
        QPushButton* m_writeBtn = nullptr;         // 写入数据。
        QPushButton* m_emulateBtn = nullptr;       // 启用/禁用模拟。
        QPushButton* m_queryPageBtn = nullptr;     // 查询页面。
        QPushButton* m_keepModifiedToggle = nullptr; // Phase G：注销时跳过 Backup 自动恢复。
        QPushButton* m_unregisterBtn = nullptr;    // 注销页面。

        // ===================== 结果区 =====================
        CodeEditorWidget* m_traceEdit = nullptr; // m_traceEdit：操作轨迹与事件日志。

        // ===================== 状态 =====================
        std::uint64_t m_currentRecordId = 0;    // m_currentRecordId：当前活跃的记录 ID。
        std::uint64_t m_lastEventSequence = 0;  // m_lastEventSequence：上次拉取事件的序列号。
        bool m_emulationEnabled = false;        // m_emulationEnabled：模拟是否已启用。
        bool m_busy = false;                    // m_busy：后台操作进行中。
        bool m_firstShow = true;                // m_firstShow：首次可见标记。
    };
}
