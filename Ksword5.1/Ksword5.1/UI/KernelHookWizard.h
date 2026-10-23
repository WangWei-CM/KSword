#pragma once

// ============================================================================
// KernelHookWizard：把跨 Dock 的"内核 Hook 实验"工作流聚合到一个独立对话框。
//
// 为什么要有它：
//   原流程要在 Misc/HvciBypass/Rxpf → Kernel/内核审计 → Memory/KernelExecutableMemory
//   三个一级 Dock 之间来回切，中间没有状态同步、没有引导，用户体验差。
//   本向导把 Register(CAPTURE_BACKUP) → Change(mode=2) → Write(payload) →
//   Unregister(SKIP_RESTORE) → 触发 → QueryBackup+Write(backup)+Unregister → 验证
//   这整条链封装成 5 步线性 wizard，中间状态由本类持有，用户不切 Dock。
//
// 【Phase F / Phase G 能力边界 —— 已定案事实，写入文案时参考】
//   - Phase F (auto-restore 默认流程) 只验证 mode=1 的 PTE 回 RX 本身正确，
//     不等价于"修改字节能持续存在于物理页"。
//   - Phase G (keep_modified / FLAG_SKIP_BACKUP_RESTORE) 已实测对 CI.dll+0x1000
//     放行修改后内容到 RX（next-exp #3 PASS，秒级 BSOD "What failed: CI.dll"
//     证据）；但唯一实测样本是 CI.dll+0x1000 一个页、payload 仅 0x90/0xCC 两种
//     1 字节覆盖 @ offset=0。本 wizard 的 UI 不得暗示其他组合已验证。
//   - 恢复步骤必须通过 IOCTL_QUERY_BACKUP 读 Record->Backup 得到原字节，否则
//     PatchGuard 几分钟到几小时内必蓝屏。
//
// 为什么是 QDialog + QStackedWidget 而不是 QWizard：
//   仓库零处使用 QWizard（grep "QWizard" 零命中）；参照 HvmHookWizard 的既定
//   模板（UI/HvmHookWizard.h）。入口静态函数 openWizard(QWidget*) 与它同形，
//   菜单项 lambda 直接调它。
// ============================================================================

#include <QDialog>
#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdint>
#include "AsyncCleanupRegistry.h"

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QCloseEvent;
QT_END_NAMESPACE

class CodeEditorWidget;

namespace ks::ui
{
    // 单次实际写入范围的原字节；多次写入仍属于同一个页备份身份。
    struct KernelHookBackupRange
    {
        unsigned long offset = 0;
        QByteArray bytes;
    };
    // ------------------------------------------------------------------------
    // KernelHookPlan：wizard 跨步共享的状态；每一步只读/写与自己相关的字段，
    // 下一步只读上一步的产物，不重新计算。
    // ------------------------------------------------------------------------
    struct KernelHookPlan
    {
        // 第 1 步选目标后锁定：
        QString moduleName;                 // 目标模块名（如 "ci.dll"）
        std::uint64_t moduleBase = 0ULL;    // 枚举得到的内核加载基址
        std::uint64_t targetOffset = 0ULL;  // 用户输入的模块内偏移
        std::uint64_t targetVa = 0ULL;      // moduleBase + targetOffset

        // 第 2 步 Register(CAPTURE_BACKUP) + Change(mode=2) 后锁定：
        std::uint64_t recordId = 0ULL;      // 驱动分配的记录 ID
        unsigned long originalProtection = 0; // Register 回显的保护属性（期望 0x20=RX）
        unsigned long aliasProtection = 0;    // Change 后 WritableAlias 保护（期望 0x4=RW）
        QByteArray backupBytes;             // QueryBackup 读到的原字节（用于恢复）
        bool recordPrepared = false;        // 当前记录已成功变更为 RW/NX。
        bool payloadPending = false;        // 已尝试写入，关闭前必须恢复并回读。
        bool recoveryRecord = false;        // 恢复阶段新注册的记录，其 Backup 含补丁。
        bool originalBytesWritten = false;  // 已向恢复记录写回原字节，清理失败可直接重试。
        QList<KernelHookBackupRange> restoreRanges; // 当前页所有已写范围的原字节。
        bool hasUncachedWrites = false;     // 存在没有完整原字节证据的写入。
        QList<std::uint64_t> atomicCleanupRecordIds; // 原子提交返回的待注销记录，不覆盖分步记录身份。
        bool atomicOriginalBytesUnverified = false; // 原子提交没有Backup，资源释放不能证明原字节恢复。

        // 返回仍需本向导归集的资源；仅缺少字节证据不永久阻止退出。
        bool hasPendingCleanup() const
        {
            return recordId != 0ULL || payloadPending || !atomicCleanupRecordIds.isEmpty();
        }

        // 第 3 步 Write payload 后锁定：
        QByteArray payload;                 // 要写入的 payload（1..PAGE_SIZE 字节，由用户决定）
        unsigned long payloadOffset = 0;    // 用户指定的页内偏移（0..PAGE_SIZE-1）

        // 第 4 步 Unregister(SKIP_RESTORE) 后锁定：
        long mode1Status = 0;               // Unregister 的 mode=1 返回状态

        // 第 5 步恢复 + 验证后锁定：
        bool restored = false;              // 是否执行过恢复步骤
        long restoreStatus = 0;             // 恢复 Unregister 的 mode=1 返回
        unsigned long verifyOrigProt = 0;   // 二次 Register 的 origProt（期望 0x20）
    };

    // ------------------------------------------------------------------------
    // KernelHookWizard：5 步线性向导。
    // 外部接口只有 openWizard() 一个静态函数，照抄 HvmHookWizard 的入口形式。
    // ------------------------------------------------------------------------
    class KernelHookWizard : public QDialog
    {
        Q_OBJECT

    public:
        // openWizard：MainWindow 菜单项的入口；非模态，带 WA_DeleteOnClose。
        // parent：顶层主窗口，用于锚定位置与主题继承。
        static void openWizard(QWidget* parent);

        explicit KernelHookWizard(QWidget* parent = nullptr);
        ~KernelHookWizard() override;

    protected:
        // 关闭与 Esc 共用异步恢复；在途请求及未完成恢复不能丢弃记录。
        void closeEvent(QCloseEvent* event) override;
        void reject() override;
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        // UI 构造：每个 build*Page() 返回一个 QWidget 填进 QStackedWidget。
        void initializeUi();                // 装 stackedWidget + 导航按钮
        QWidget* buildTargetPage();         // 第 1 步：选模块+偏移
        QWidget* buildPreparePage();        // 第 2 步：Register(CB) + Change
        QWidget* buildWritePage();          // 第 3 步：选 payload + Write
        QWidget* buildKeepPage();           // 第 4 步：Unregister(SKIP) 保留 payload 等触发
        QWidget* buildVerifyPage();         // 第 5 步：恢复 + 验证 + 最终报告

        // 步骤导航：
        void goTo(int step);                // 切到第 step 页，更新按钮启用态
        void updateNavButtons();            // 根据 m_currentStep 与 m_busy 更新按钮
        void updateTargetFromInputs();      // 持有旧目标时先异步收尾，再切换目标快照。

        // 异步任务（走 QtConcurrent + QFutureWatcher，参照 RxpfPage）：
        void refreshModules();              // 枚举已加载内核模块填 m_moduleCombo
        void onPrepareClicked();            // 执行 Register+Change+QueryBackup
        void onWritePayloadClicked();       // 执行 Write(payload)
        void onAtomicCommitClicked();       // 执行原子提交：R0 单次 call 做完 Register+Change+Write+Unregister(SKIP)
        void onKeepModifiedClicked();       // 执行 Unregister(SKIP_RESTORE)
        void onRestoreClicked();            // 执行 Register+Change+Write(backup)+Unregister
        void onFinalVerifyClicked();        // 执行 Register+Query 看 origProt + Unregister

        // UI 工具：
        void appendTrace(const QString& text); // 向 CodeEditorWidget 追加带时间戳的轨迹行
        void setBusy(bool busy);               // 禁用按钮避免并发触发
        bool beginOperation();                 // 接入既有退出协调，登记后再派发后台工作。

        // 共享状态与控件：
        KernelHookPlan m_plan;              // 跨步共享状态
        std::shared_ptr<AsyncCleanupRegistry::Lease> m_shutdownLease; // 工作和待恢复记录共同持有退出责任。
        bool m_busy = false;                // 是否有后台任务在跑
        int m_currentStep = 0;              // 0~4
        bool m_closeAfterCleanup = false;   // 恢复成功后重试关闭当前向导。
        bool m_targetChangePending = false; // 已选择新目标，等待旧目标收尾。
        bool m_prepareAfterCleanup = false; // 重复准备先清理当前记录，再继续准备。
        KernelHookPlan m_requestedTarget;   // 待切换的目标参数，与正在恢复的目标分开。

        QStackedWidget* m_stack = nullptr;  // 5 页容器
        QPushButton* m_prevBtn = nullptr;   // 上一步
        QPushButton* m_nextBtn = nullptr;   // 下一步 / 完成
        QPushButton* m_cancelBtn = nullptr; // 取消
        QLabel* m_stepLabel = nullptr;      // "第 N/5 步" 指示器
        CodeEditorWidget* m_traceEdit = nullptr; // 操作轨迹（底部，整个向导共享）

        // 第 1 页：选目标
        QComboBox* m_moduleCombo = nullptr; // 模块下拉
        QLineEdit* m_offsetEdit = nullptr;  // 偏移 hex 输入
        QLabel* m_vaLabel = nullptr;        // 显示绝对 VA

        // 第 2 页：Register+Change
        QPushButton* m_prepareBtn = nullptr;
        QLabel* m_prepareStatusLabel = nullptr;
        QLabel* m_backupHexLabel = nullptr; // 显示 Backup 前 16 字节 hex

        // 第 3 页：Write payload
        QComboBox* m_payloadPresetCombo = nullptr; // 预设 payload 快捷填充（选中会覆盖 hex 输入框）
        QLineEdit* m_payloadHexEdit = nullptr;     // 用户任意 payload hex；不设 GUI 侧长度上限，由 driver 校验
        QLineEdit* m_payloadOffsetEdit = nullptr;  // 用户指定的页内写入偏移（hex / dec 均可）
        QPushButton* m_writeBtn = nullptr;
        QPushButton* m_atomicCommitBtn = nullptr; // 原子提交（Register→Change→Write→Unregister(SKIP) 单次 IOCTL）
        QLabel* m_writeStatusLabel = nullptr;

        // 第 4 页：Keep + 等触发
        QPushButton* m_keepBtn = nullptr;
        QLabel* m_keepStatusLabel = nullptr;

        // 第 5 页：恢复 + 验证
        QPushButton* m_restoreBtn = nullptr;
        QPushButton* m_verifyBtn = nullptr;
        QLabel* m_finalStatusLabel = nullptr;
    };
}
