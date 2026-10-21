#pragma once

#include "../ArkDriverClient/ArkDriverTypes.h"

#include <QByteArray>
#include <QWidget>

namespace ks::ui { class AsyncOperation; }
struct HardwarePowerSchemeSnapshot;
struct HardwarePowerTransactionRequest;
struct HardwarePowerTransactionResult;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTableWidget;

// HardwarePowerPage：Windows 电源方案与受控 R0 CPU 电源调节独立页。
class HardwarePowerPage final : public QWidget
{
public:
    explicit HardwarePowerPage(QWidget* parent = nullptr);
    ~HardwarePowerPage() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void initializeUi();
    void initializeConnections();
    void refreshAll();
    void applyPowerSchemesSnapshot(const HardwarePowerSchemeSnapshot& snapshot);
    void applySelectedPowerScheme();
    void restoreInitialState();
    void applySnapshotToUi(const ksword::ark::CpuPowerResult& result);
    void updateControlAvailability();
    void setStatus(const QString& text, bool isError = false);
    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST buildControlRequest(
        unsigned long applyFlags) const;
    KSWORD_ARK_CPU_POWER_CONTROL_REQUEST buildRestoreControlRequest(
        unsigned long applyFlags) const;
    unsigned long restorableCpuApplyFlags() const;
    void confirmRestoreInitialState();
    void submitPowerTransaction(const HardwarePowerTransactionRequest& request);
    void consumePowerTransaction(const HardwarePowerTransactionResult& result);
    void reportPowerTransactionUnavailable(bool accepted);
    bool presentControlResult(const KSWORD_ARK_CPU_POWER_CONTROL_REQUEST& request,
        const ksword::ark::CpuPowerResult& result);
    void sendControlRequest(
        unsigned long applyFlags,
        const QString& confirmationText);
    void raisePowerLimitsToPlatformMaximum();

    ks::ui::AsyncOperation* m_queryOperation = nullptr;
    bool m_queryPending = false;
    bool m_transactionPending = false;
    bool m_confirming = false;
    bool m_restorePreparing = false;
    QString m_powerSchemeEnumerationError;
    QString m_statusText;
    bool m_statusIsError = false;

    bool m_loadedOnce = false;                        // m_loadedOnce：首次显示时自动刷新一次。
    bool m_hasSnapshot = false;                       // m_hasSnapshot：expected raw MSR 是否有效。
    KSWORD_ARK_CPU_POWER_RESPONSE m_snapshot{};       // m_snapshot：最近一次锁内 R0 快照。
    bool m_hasRestoreCpuSnapshot = false;             // m_hasRestoreCpuSnapshot：首次有效 CPU 快照是否已捕获。
    KSWORD_ARK_CPU_POWER_RESPONSE m_restoreCpuSnapshot{}; // m_restoreCpuSnapshot：一键还原目标。
    QByteArray m_restorePowerSchemeGuid;               // m_restorePowerSchemeGuid：首次活动 Windows 方案。

    QLabel* m_statusLabel = nullptr;                  // m_statusLabel：页面操作与错误状态。
    QComboBox* m_powerSchemeCombo = nullptr;          // m_powerSchemeCombo：Windows 可用电源方案。
    QPushButton* m_refreshAllButton = nullptr;        // m_refreshAllButton：同时刷新方案与 R0。
    QPushButton* m_applyPowerSchemeButton = nullptr;  // m_applyPowerSchemeButton：激活所选方案。
    QPushButton* m_restoreInitialStateButton = nullptr; // m_restoreInitialStateButton：恢复首次捕获状态。
    QTableWidget* m_snapshotTable = nullptr;          // m_snapshotTable：CPU 能力和当前值摘要。

    QDoubleSpinBox* m_pl1Spin = nullptr;              // m_pl1Spin：PL1 长时功耗 W。
    QDoubleSpinBox* m_pl2Spin = nullptr;              // m_pl2Spin：PL2 短时功耗 W。
    QCheckBox* m_pl1EnableCheck = nullptr;            // m_pl1EnableCheck：启用 PL1。
    QCheckBox* m_pl1ClampCheck = nullptr;             // m_pl1ClampCheck：允许 PL1 clamp。
    QCheckBox* m_pl2EnableCheck = nullptr;            // m_pl2EnableCheck：启用 PL2。
    QCheckBox* m_pl2ClampCheck = nullptr;             // m_pl2ClampCheck：允许 PL2 clamp。
    QPushButton* m_applyPowerLimitsButton = nullptr;  // m_applyPowerLimitsButton：提交 PL1/PL2。
    QPushButton* m_raisePowerLimitsButton = nullptr;  // m_raisePowerLimitsButton：提升到 SKU 最大值。

    QCheckBox* m_turboEnableCheck = nullptr;           // m_turboEnableCheck：Turbo 开关目标。
    QPushButton* m_applyTurboButton = nullptr;         // m_applyTurboButton：提交 Turbo 开关。
    QSpinBox* m_turboRatioSpin = nullptr;              // m_turboRatioSpin：所有已实现档位的目标 ratio。
    QPushButton* m_applyTurboRatioButton = nullptr;    // m_applyTurboRatioButton：提交 Turbo Ratio。
    QSpinBox* m_requestedMultiplierSpin = nullptr;     // m_requestedMultiplierSpin：IA32_PERF_CTL 请求倍频。
    QPushButton* m_applyRequestedMultiplierButton = nullptr; // m_applyRequestedMultiplierButton：提交请求倍频。

    QSpinBox* m_hwpMinimumSpin = nullptr;              // m_hwpMinimumSpin：HWP minimum performance。
    QSpinBox* m_hwpMaximumSpin = nullptr;              // m_hwpMaximumSpin：HWP maximum performance。
    QSpinBox* m_hwpDesiredSpin = nullptr;              // m_hwpDesiredSpin：HWP desired，0=自动。
    QSpinBox* m_hwpEppSpin = nullptr;                  // m_hwpEppSpin：HWP EPP，0 性能/255 节能。
    QPushButton* m_applyHwpButton = nullptr;           // m_applyHwpButton：提交全逻辑处理器 HWP。
};
