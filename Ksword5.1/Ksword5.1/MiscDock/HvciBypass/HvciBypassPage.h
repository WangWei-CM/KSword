#pragma once

// ============================================================
// HvciBypassPage.h
// 作用：
// - HVCI W^X 绕过实验页面，接入 MiscDock "杂项" Tab；
// - 参数区：目标模块选择、页内偏移、实验标志位；
// - 动作区：刷新模块、执行实验；
// - 结果区：各阶段状态码、读回字节证据与判定文本；
// - 后端逻辑由 HvciBypassBackend 提供，本页只组合共享组件。
// ============================================================

#include "HvciBypassBackend.h"

#include "../../Framework.h"

#include <QWidget>

#include <cstdint>
#include <vector>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class CodeEditorWidget;

namespace ks::misc
{
    // HvciBypassPage：
    // - 作用：在杂项 Tab 中提供 HVCI 映像保护绕过实验界面；
    // - 布局：使用 ParameterWorkspace 两栏结构，参数独立滚动；
    // - 页面不直接接触驱动设备，所有 R0 调用委托给后端。
    class HvciBypassPage final : public QWidget
    {
    public:
        // 构造函数：建出界面控件与连接，不执行任何 R0 查询。
        explicit HvciBypassPage(QWidget* parent = nullptr);
        ~HvciBypassPage() override = default;

    protected:
        // showEvent：首次可见时查询系统姿态并枚举内核模块。
        void showEvent(QShowEvent* event) override;
        // changeEvent：跟随主题切换刷新风险横幅配色。
        void changeEvent(QEvent* event) override;

    private:
        // initializeUi：构建根布局、参数区、动作区和结果区。
        void initializeUi();
        // initializeConnections：把按钮信号连接到处理函数。
        void initializeConnections();
        // applyBannerStyle：下发风险横幅样式，构造期与主题切换共用。
        void applyBannerStyle();

        // refreshPosture：查询 CI/HVCI 姿态并更新显示。
        void refreshPosture();
        // refreshModules：枚举内核模块列表并填充下拉框。
        void refreshModules();
        // runExperiment：收集参数、调用后端并显示结果。
        void runExperiment();

        // displayResult：把一次实验结果刷新到结果区控件。
        void displayResult(const hvci_bypass::ExperimentResult& result);
        // appendTrace：追加一行操作轨迹到日志区。
        void appendTrace(const QString& line);
        // setBusy：操作进行中禁用按钮。
        void setBusy(bool busy);

    private:
        // ===================== 姿态区 =====================
        QLabel* m_warningLabel = nullptr;       // m_warningLabel：PatchGuard 风险横幅。
        QLabel* m_postureLabel = nullptr;       // m_postureLabel：CI / HVCI 姿态摘要。

        // ===================== 参数区 =====================
        QComboBox* m_moduleCombo = nullptr;     // m_moduleCombo：目标内核模块下拉框。
        QLineEdit* m_offsetEdit = nullptr;      // m_offsetEdit：页内偏移输入（十六进制）。
        QCheckBox* m_writeTestCheck = nullptr;  // m_writeTestCheck：WRITE_TEST 标志。
        QCheckBox* m_restoreCheck = nullptr;    // m_restoreCheck：RESTORE 标志。
        QCheckBox* m_unmapCheck = nullptr;      // m_unmapCheck：UNMAP_BEFORE_RESTORE 标志。
        QLineEdit* m_funcOverrideEdit = nullptr;// m_funcOverrideEdit：函数地址手动覆盖。

        // ===================== 动作区 =====================
        QPushButton* m_refreshButton = nullptr; // m_refreshButton：刷新姿态与模块列表。
        QPushButton* m_executeButton = nullptr; // m_executeButton：执行实验。

        // ===================== 结果区 =====================
        QLabel* m_statusLabel = nullptr;        // m_statusLabel：各阶段 NTSTATUS 码。
        QLabel* m_evidenceLabel = nullptr;      // m_evidenceLabel：读回字节证据。
        QLabel* m_verdictLabel = nullptr;       // m_verdictLabel：判定结论。
        CodeEditorWidget* m_traceEdit = nullptr;// m_traceEdit：详细操作轨迹日志。

        // ===================== 状态 =====================
        std::vector<hvci_bypass::KernelModuleInfo> m_modules; // m_modules：缓存的模块列表。
        bool m_busy = false;                    // m_busy：后台操作进行中。
        bool m_firstShow = true;                // m_firstShow：首次可见标记。
        bool m_firstModuleEnum = true;          // m_firstModuleEnum：首次枚举模块时自动选中 CI.dll，后续刷新保留用户选择。
    };
}
