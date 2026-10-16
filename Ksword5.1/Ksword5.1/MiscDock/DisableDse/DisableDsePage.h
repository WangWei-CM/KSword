#pragma once

// ============================================================
// DisableDsePage.h
// 作用：
// 1) 在"杂项"里提供驱动签名强制（DSE）开关，用于临时加载未签名驱动；
// 2) 展示 CI / HVCI / 安全启动姿态，并把不满足条件的情况直接挡在按钮之前；
// 3) 定位 CI.dll!g_CiOptions 并展示完整定位轨迹，供用户核对地址是否可信；
// 4) 关闭前记录原值，提供一键恢复；页面析构时若仍处于关闭状态会自动写回。
//
// 风险说明：g_CiOptions 属于 PatchGuard 的巡检范围，长时间保持修改会触发
// CRITICAL_STRUCTURE_CORRUPTION 蓝屏。本页的定位是运行时反汇编得出的，
// 并在写入前校验读回值的强制签名位与系统自报状态自洽，
// 但仍要求用户加载完驱动后立刻恢复。
// ============================================================

#include "DisableDseBackend.h"
#include "KsecDdExecutor.h"

#include "../../Framework.h"

#include <QWidget>

#include <cstdint>

class QLabel;
class QPushButton;
class QRadioButton;
class QShowEvent;
class CodeEditorWidget;

namespace ks::misc
{
    // DisableDsePage：
    // - 作用：DSE 开关页；唯一的系统修改是 CI.dll!g_CiOptions 的 4 个字节；
    // - 说明：页面不加载任何第三方漏洞驱动，全部内核访问都走 KswordARK 自有 R0 事务通道。
    class DisableDsePage final : public QWidget
    {
    public:
        // 构造函数：只建界面，不访问驱动，避免打开"杂项"就触发一次内核查询。
        explicit DisableDsePage(QWidget* parent = nullptr);
        // 析构函数：若本页关闭过 DSE 且尚未恢复，在这里做最后一次写回，
        // 避免用户忘记恢复后被 PatchGuard 判定为内核数据被篡改。
        ~DisableDsePage() override;

    protected:
        // 页面第一次真正可见时才读一次系统姿态。
        void showEvent(QShowEvent* event) override;
        // 语义色（Error/Warning 及其背景）在 theme.h 里没有 palette 等价物，
        // 写进 QSS 就被定死在下发那一刻的主题上，主题切换时必须重下发。
        void changeEvent(QEvent* event) override;

    private:
        // initializeUi：建出风险横幅、姿态区、定位区与操作区。
        void initializeUi();
        // applyBannerStyle：下发风险横幅样式，构造期与主题切换共用。
        void applyBannerStyle();
        // initializeConnections：把四个按钮接到各自的处理函数。
        void initializeConnections();

        // refreshPosture：同步查询 CI / HVCI 姿态并刷新到界面。
        void refreshPosture();
        // refreshKsecStrategyAssessment：只读核对当前 KSecDD PoC 方案的前置条件。
        void refreshKsecStrategyAssessment();
        // updateKsecStrategyDisplay：切换方案或语言后重绘摘要与边界说明。
        void updateKsecStrategyDisplay();
        // updateR0StrategyDisplay：R0 路径选中时刷新摘要显示（驱动是否在线等）。
        void updateR0StrategyDisplay();
        // runLocate：在后台线程定位 g_CiOptions，并在定位成功后立即读回校验。
        void runLocate();
        // runApply：在后台线程把 g_CiOptions 写成 desiredValue；
        //   isRestore 只影响文案与状态记账，写入路径完全相同。
        void runApply(std::uint32_t desiredValue, bool isRestore);
        // runKsecLocateGadget：在后台线程定位 ntoskrnl 中的写入桩。
        void runKsecLocateGadget();
        // runKsecApply：通过当前选定的 KSecDD 方案写入 g_CiOptions。
        void runKsecApply(std::uint32_t desiredValue, bool isRestore);

        // LocateOutcome：一次定位 + 读回校验的综合结果，只在线程间传值。
        struct LocateOutcome
        {
            disable_dse::TargetLocation location;      // location：定位结果。
            disable_dse::ReadbackResult readback;      // readback：定位成功后的读回结果。
            disable_dse::CodeIntegrityPosture posture; // posture：做一致性判定时依据的系统姿态。
            bool valueMatched = false;                 // valueMatched：读回值是否与系统姿态自洽。
        };

        // applyLocateOutcome：在 UI 线程收拢定位结果。
        void applyLocateOutcome(const LocateOutcome& outcome);
        // applyApplyOutcome：在 UI 线程收拢写入结果。
        void applyApplyOutcome(const disable_dse::ApplyResult& result, bool isRestore);

        // appendTrace：把一行诊断信息追加到轨迹框。
        void appendTrace(const QString& line);
        // setResultText：把一次操作的结论写到底部，isError 决定配色。
        void setResultText(const QString& text, bool isError);
        // setBusy：一次操作没结束时禁用按钮，防止重复点击。
        void setBusy(bool busy);
        // updateButtons：按忙碌状态、姿态与定位结果刷新按钮可用性。
        void updateButtons();
        // updateStateDisplay：刷新姿态摘要、阻断原因与当前值显示。
        void updateStateDisplay();

    private:
        QLabel* m_warningLabel = nullptr;       // m_warningLabel：PatchGuard 风险横幅。
        QLabel* m_postureLabel = nullptr;       // m_postureLabel：CI / HVCI / 安全启动摘要。
        QLabel* m_optionsLabel = nullptr;       // m_optionsLabel：CodeIntegrityOptions 位分解。
        QLabel* m_blockLabel = nullptr;         // m_blockLabel：当前为什么不允许操作。
        QLabel* m_locationLabel = nullptr;      // m_locationLabel：g_CiOptions 定位结果摘要。
        QLabel* m_ksecStrategySummaryLabel = nullptr; // 当前 PoC 方案的准入摘要。
        QLabel* m_ksecStrategyDetailLabel = nullptr;  // 当前 PoC 方案的实现链与风险边界。
        QLabel* m_pendingLabel = nullptr;       // m_pendingLabel：尚未恢复时的醒目提示。
        QLabel* m_resultLabel = nullptr;        // m_resultLabel：最近一次操作结论。
        CodeEditorWidget* m_traceEdit = nullptr; // m_traceEdit：定位轨迹与事务日志。
        QStringList m_traceLines;               // m_traceLines：保留中文规范源文本，供运行时切换语言后重绘。
        QPushButton* m_refreshButton = nullptr; // m_refreshButton：重新查询系统姿态。
        QPushButton* m_locateButton = nullptr;  // m_locateButton：定位 g_CiOptions 并读回校验。
        QPushButton* m_disableButton = nullptr; // m_disableButton：关闭驱动签名强制。
        QPushButton* m_restoreButton = nullptr; // m_restoreButton：把 g_CiOptions 写回原值。
        QPushButton* m_ksecRefreshButton = nullptr; // 重新执行只读前置条件核对。
        QRadioButton* m_r0DriverButton = nullptr;   // R0 驱动直写方案。
        QRadioButton* m_kexecDdButton = nullptr;    // KSecDD LSASS 注入方案。
        QRadioButton* m_kexecDdPlusButton = nullptr;// KSecDD Server Silo 方案。
        QWidget* m_r0Container = nullptr;            // R0 路径专属控件容器，按方案切换可见性。
        QWidget* m_ksecContainer = nullptr;          // KSecDD 路径专属控件容器，按方案切换可见性。

        disable_dse::CodeIntegrityPosture m_posture; // m_posture：最近一次查询到的姿态。
        disable_dse::TargetLocation m_location;      // m_location：最近一次定位结果。

        std::uint32_t m_currentValue = 0;      // m_currentValue：最近一次读回的 g_CiOptions。
        bool m_hasCurrentValue = false;        // m_hasCurrentValue：m_currentValue 是否有效。
        std::uint32_t m_savedOriginalValue = 0;// m_savedOriginalValue：本页关闭 DSE 前的原值。
        bool m_hasSavedOriginal = false;       // m_hasSavedOriginal：是否记录了待恢复的原值。
        bool m_originalIsInferred = false;     // m_originalIsInferred：原值是推断的，非直接读取。
        bool m_usedKsecPath = false;           // m_usedKsecPath：最后一次关闭 DSE 是否经由 KSecDD 路径。
        bool m_valueMatched = false;           // m_valueMatched：读回值是否与系统自报值一致，false 时禁止写入。
        bool m_busy = false;                   // m_busy：后台操作进行中。
        disable_dse::KsecDdStrategy m_ksecStrategy =
            disable_dse::KsecDdStrategy::LsassInjection;
        disable_dse::KsecDdAssessment m_ksecAssessment;
        disable_dse::GadgetLocation m_gadgetLocation;  // m_gadgetLocation：写入桩定位结果。
        QLabel* m_ksecGadgetLabel = nullptr;            // m_ksecGadgetLabel：桩地址摘要。
        QPushButton* m_ksecLocateGadgetButton = nullptr;// 定位 ntoskrnl 写入桩。
        QPushButton* m_ksecDisableButton = nullptr;     // 通过 KSecDD 关闭签名强制。
        QPushButton* m_ksecRestoreButton = nullptr;     // 通过 KSecDD 恢复原值。

        // m_blockReason：
        // - 最近一次算出的准入判定，updateStateDisplay 里刷新、updateButtons 里消费；
        // - 缓存下来是为了避免每次刷新按钮都去开一次驱动设备句柄。
        disable_dse::BlockReason m_blockReason = disable_dse::BlockReason::DriverUnavailable;
    };
}
