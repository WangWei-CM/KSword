#pragma once

// ============================================================
// SystemTimePage.h
// 作用：
// 1) 在“杂项”中提供系统全局加速、减速与恢复入口；
// 2) 通过 ArkDriverClient 查询和控制 R0 计时重映射；
// 3) 持续展示高风险警告、接管状态和冲突诊断。
// ============================================================

#include "../../Framework.h"
#include "../../UI/PageNavigationCatalog.h"
#include "../../ArkDriverClient/ArkDriverTypes.h"
#include <functional>

#include <QElapsedTimer>
#include <QWidget>

class QCheckBox;
class QHideEvent;
class QLabel;
class QProcess;
class QPushButton;
class QRadioButton;
class QShowEvent;
class QSpinBox;
class QTimer;
namespace ks::ui { class AsyncOperation; }

namespace ks::misc
{
    class SystemTimePage final : public QWidget
    {
    public:
        // 构造函数：创建页面但不主动访问 R0，避免打开“杂项”时弹出驱动提示。
        explicit SystemTimePage(QWidget* parent = nullptr);
        ~SystemTimePage() override;
        // Initial searchable values; creates no page or native backend.
        static QVector<ks::ui::PageSearchBodyText> coldSearchBody();

    protected:
        // 页面真正可见时才查询并轮询状态，隐藏后停止周期 IOCTL。
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;
        // 语义色（Warning 及其背景）在 theme.h 里没有 palette 等价物，写进 QSS
        // 就被定死在下发那一刻的主题上。主题切换时必须重下发，否则这条永久警告
        // 会变成暗底黑字或浅底白字，正好读不出来。
        void changeEvent(QEvent* event) override;

    private:
        // initializeUi：创建永久警告、倍率控件、确认开关和状态证据区。
        void initializeUi();
        // applyWarningBannerStyle：下发永久警告横幅样式，构造期与主题切换共用。
        void applyWarningBannerStyle();
        // initializeConnections：连接刷新、应用、恢复和定时状态查询。
        void initializeConnections();
        // refreshStatus：通过 ArkDriverClient 获取最新倍率、代次与接管状态。
        void refreshStatus();
        struct ControlReceipt
        {
            ksword::ark::SystemTimeQueryResult before;
            ksword::ark::SystemTimeControlResult result;
            ksword::ark::SystemTimeQueryResult after;
            bool hasAfter = false;
        };
        void applyQueryResult(const ksword::ark::SystemTimeQueryResult& result);
        void showAsyncFailure();
        void runControl(std::function<ControlReceipt()> operation,
            std::function<void(const ControlReceipt&)> applyReceipt);
        // applyRequestedMode：执行双重确认后应用当前加速或减速倍率。
        void applyRequestedMode();
        // resetSystemTime：无额外门槛地停止变速，并以连续计数保持 1x。
        void resetSystemTime();
        // synchronizeFromTimeServer：通过 Windows 时间服务异步请求立即校时。
        void synchronizeFromTimeServer();
        // completeTimeSynchronization：收拢 w32tm 成功、失败和超时结果。
        void completeTimeSynchronization(
            QProcess* process,
            bool success,
            const QString& detailText);
        // resetCalibratedClock：以当前系统墙钟重建倍率校准锚点。
        void resetCalibratedClock();
        // updateCalibratedTimeDisplay：按当前倍率反向修正 QPC 经过时间。
        void updateCalibratedTimeDisplay();
        // confirmHighRisk：显示所选方案和风险清单，并要求输入固定确认短语。
        bool confirmHighRisk(
            const QString& modeText,
            const QString& backendText,
            const QString& schemeText,
            unsigned long factor);
        // updateStatusDisplay：把协议字段转换为用户可读状态和诊断证据。
        void updateStatusDisplay(
            unsigned long status,
            unsigned long stateFlags,
            unsigned long generation,
            unsigned long command,
            unsigned long factor,
            unsigned long osBuildNumber,
            long lastStatus,
            unsigned long resolutionMode,
            unsigned long backend,
            unsigned long long counterSourceAddress,
            unsigned long long primarySlotAddress,
            unsigned long long secondarySlotAddress,
            unsigned long long hypervisorSharedPageAddress,
            unsigned long long hypervisorTimeUpdateLock,
            unsigned long long hypervisorOriginalMultiplier,
            unsigned long long hypervisorOriginalBias,
            unsigned long long hypervisorCurrentMultiplier,
            unsigned long long hypervisorCurrentBias);
        // updateButtons：根据忙碌、协议支持和风险确认状态更新按钮。
        void updateButtons();
        // setBusy：防止一次同步 IOCTL 尚未完成时重复点击。
        void setBusy(bool busy);

    private:
        QLabel* m_warningLabel = nullptr; // m_warningLabel：永久显示的系统失稳警告。
        QLabel* m_persistenceLabel = nullptr; // m_persistenceLabel：离开页面不自动恢复的说明。
        QLabel* m_currentModeLabel = nullptr; // m_currentModeLabel：当前 1x/加速/减速状态。
        QLabel* m_calibratedTimeLabel = nullptr; // m_calibratedTimeLabel：按倍率反向校准后的墙钟时间。
        QLabel* m_backendLabel = nullptr; // m_backendLabel：构建与解析策略摘要。
        QLabel* m_diagnosticLabel = nullptr; // m_diagnosticLabel：槽地址和 NTSTATUS 证据。
        QLabel* m_operationLabel = nullptr; // m_operationLabel：最近刷新或控制结果。
        QRadioButton* m_hypervBackendRadio = nullptr; // m_hypervBackendRadio：选择 Hyper-V 共享 QPC 后端。
        QRadioButton* m_halBackendRadio = nullptr; // m_halBackendRadio：选择原 HAL 兼容后端。
        QRadioButton* m_compatRadio = nullptr; // m_compatRadio：选择兼容定位模式。
        QRadioButton* m_guardedResolutionRadio = nullptr; // m_guardedResolutionRadio：选择增强校验定位。
        QRadioButton* m_speedUpRadio = nullptr; // m_speedUpRadio：选择 N 倍加速。
        QRadioButton* m_slowDownRadio = nullptr; // m_slowDownRadio：选择 1/N 减速。
        QSpinBox* m_factorSpin = nullptr; // m_factorSpin：2 到协议上限的倍率输入。
        QCheckBox* m_acknowledgeCheck = nullptr; // m_acknowledgeCheck：持久风险确认开关。
        QPushButton* m_refreshButton = nullptr; // m_refreshButton：图标化状态刷新按钮。
        QPushButton* m_timeSyncButton = nullptr; // m_timeSyncButton：从 Windows 时间服务器立即更新时间。
        QPushButton* m_applyButton = nullptr; // m_applyButton：应用选定模式和倍率。
        QPushButton* m_resetButton = nullptr; // m_resetButton：紧急恢复 1x。
        QTimer* m_refreshTimer = nullptr; // m_refreshTimer：页面可见时的状态轮询器。
        QTimer* m_clockTimer = nullptr; // m_clockTimer：页面可见时刷新校准时间。
        QProcess* m_timeSyncProcess = nullptr; // m_timeSyncProcess：当前异步 w32tm 进程。
        QElapsedTimer m_calibratedElapsedTimer; // m_calibratedElapsedTimer：会受全局倍速影响的 QPC 经过时间。
        qint64 m_calibratedAnchorEpochMs = 0; // m_calibratedAnchorEpochMs：校准起点的本地墙钟毫秒。
        unsigned long m_generation = 0UL; // m_generation：最近查询到的并发控制代次。
        unsigned long m_calibrationGeneration = ~0UL; // m_calibrationGeneration：校准锚点对应的状态代次。
        unsigned long m_currentCommand = 0UL; // m_currentCommand：校准使用的当前速度命令。
        unsigned long m_currentFactor = 1UL; // m_currentFactor：校准使用的当前倍率。
        unsigned long m_currentBackend = 0UL; // m_currentBackend：R0 当前或默认后端。
        bool m_active = false; // m_active：R0 当前是否已经接管计数器槽。
        bool m_supported = false; // m_supported：当前 R0 是否解析并支持本功能。
        bool m_hypervAvailable = false; // m_hypervAvailable：Microsoft Hv 与共享 QPC 页是否同时可用。
        ks::ui::AsyncOperation* m_statusOperation = nullptr;
        ks::ui::AsyncOperation* m_controlOperation = nullptr;
        bool m_statusPending = false;
        bool m_controlPending = false;
        bool m_busy = false; // m_busy：同步控制期间阻止重复操作。
    };
}
