#pragma once

// UI 提交协调器只管理交互屏障和刷新生命周期，不持有业务数据或模型。
#include <QHash>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include "PerfTrace.h"

#include <functional>

class QApplication;
class QAbstractItemView;
class QEvent;

namespace ks::ui
{
    // 可替换的物理输入探针供离屏回归使用；未提供时读取真实左 Ctrl/下拉弹层。
    struct UiCommitInteractionProbes
    {
        std::function<bool()> leftCtrlHeld;   // 左 Ctrl 是否仍按住。
        std::function<bool()> comboPopupOpen; // 下拉弹层是否仍占用输入。
    };

    // 预算只在两个原子回调之间检查；单个 void 回调不能被抢占。
    struct UiCommitDrainOptions
    {
        qint64 softBudgetNanoseconds = 4'000'000; // 全局一轮排空合计约 4 ms。
        // 留空使用 QElapsedTimer；注入单调时钟可做不创建 QWidget 的确定性回归。
        std::function<qint64()> monotonicNanoseconds;
    };

    // 提交结果区分无效调用、当前完成和已缓存，便于业务显式安排快照提交。
    enum class UiCommitSubmission
    {
        Rejected,
        Executed,
        Deferred
    };

    class UiCommitCoordinator final : public QObject
    {
    public:
        // 以 application 管理生命周期并观察释放事件；探针仅影响物理输入判据。
        explicit UiCommitCoordinator(
            QApplication* application,
            UiCommitInteractionProbes probes = {},
            UiCommitDrainOptions drainOptions = {});
        ~UiCommitCoordinator() override;

        // 返回应用唯一协调器；必须在 GUI 线程调用，空应用返回 nullptr。
        static UiCommitCoordinator* forApplication(QApplication* application = nullptr);

        // 任一目标菜单打开或物理输入屏障存在时返回 true。
        bool isBlocked(const QList<QAbstractItemView*>& views) const;

        // 兼容旧接口：输入阻塞或排空续轮期间接管 action；false 时调用者立即提交。
        // 同 owner/key 的新提交取代旧提交，包括解除屏障后的立即提交。
        bool deferIfBlocked(
            QObject* owner,
            const QString& key,
            const QList<QAbstractItemView*>& views,
            std::function<void()> action);

        // Only pure presentation commits opt into capture/hidden-view deferral.
        // Known QWidget owners with nonempty descendant targets keep only the
        // latest hidden projection until an actual target/owner Show.
        // Business cache, sampling and archive commits keep deferIfBlocked/submit.
        bool deferPresentationIfBlocked(
            QObject* owner,
            const QString& key,
            const QList<QAbstractItemView*>& views,
            std::function<void()> action);

        // 新接口在无屏障/续轮时直接执行；排队时只保留同键最后一份原子提交。
        UiCommitSubmission submit(
            QObject* owner,
            const QString& key,
            const QList<QAbstractItemView*>& views,
            std::function<void()> action);

        // 菜单开始增加深度，结束延至外层事件循环，避免业务 action 仍使用旧行。
        void beginContextMenu(QAbstractItemView* view);
        void endContextMenu(QAbstractItemView* view);

        // Bounded read-only value/scope preparation defers presentation commits
        // only. Sampling and canonical archives continue; every lease is released
        // on completion/cancel/close. Independent from real menu nesting.
        void beginValueCapture(QAbstractItemView* view);
        void endValueCapture(QAbstractItemView* view);

        // 释放事件合并调度复查；全 owner 共用软预算，续轮前重新核对完整目标组。
        void notifyInteractionEnded();
        void flush();
        qsizetype pendingCount() const;

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        // 一条待提交记录是一组视图的原子更新，不拆分 TCP/UDP 等关联结果。
        struct PendingCommit
        {
            QObject* ownerIdentity = nullptr;          // 仅作索引身份，绝不解引用。
            QPointer<QObject> owner;                    // 回调生命周期所有者。
            QString key;                              // 所有者内部去重键。
            QList<QPointer<QAbstractItemView>> views;  // 本次原子更新的目标集合。
            std::function<void()> action;             // 最新快照对应的提交动作。
            bool waitsForValueCapture = false;        // Pure presentation only.
            bool waitsForExposure = false;            // Explicit known owner/targets at admission.
            QList<QObject*> presentationHosts;        // Index identities only; never dereference.
#if KSWORD_PERF_TRACE_ENABLED
            PerfTraceContext traceContext;
            qint64 deferredAtNs = -1;
#endif
        };

        using PendingMap = QMap<quint64, PendingCommit>;

        bool inputBlocked() const;
        bool deferChecked(
            QObject* owner,
            const QString& key,
            const QList<QAbstractItemView*>& views,
            std::function<void()>& action,
            bool executingLegacyGuard = false,
            bool waitsForValueCapture = false);
        bool isPresentationBlocked(const QList<QAbstractItemView*>& views) const;
        bool pendingBlocked(const PendingCommit& pending) const;
        bool targetsAlive(const PendingCommit& pending) const;
        void removeMatching(QObject* owner, const QString& key);
        void watchOwner(QObject* owner);
        void removeIndex(const PendingCommit& pending);
        void scheduleContinuation();
        void scheduleFlush();

        QPointer<QApplication> m_application;      // 安装事件过滤器的应用。
        UiCommitInteractionProbes m_probes;        // 输入判据，默认读取真实状态。
        UiCommitDrainOptions m_drainOptions;        // 全应用共享排空预算与测试时钟。
        PendingMap m_pending;                     // 按到达代次排序，回调重入不会使游标悬空。
        QHash<QObject*, QHash<QString, quint64>> m_pendingIndex; // owner/key 到最新代次。
        QHash<QObject*, qsizetype> m_pendingShowHosts; // Only current explicit presentation hosts; shared app filter.
        quint64 m_nextGeneration = 0;              // 每个缓存提交的唯一到达代次。
        quint64 m_roundCutoff = 0;                 // 本轮只处理开始时已到达的快照。
        quint64 m_roundCursor = 0;                 // 跨软预算续轮恢复扫描位置。
        bool m_roundActive = false;               // 本轮尚有未扫描项。
        bool m_roundVisiblePass = true;            // Same cutoff: visible first, then other targets.
        bool m_continuationPending = false;       // 新提交不得绕过已经让出的排空轮。
        bool m_flushRequested = false;            // 嵌套事件循环请求留到外层复查。
        QPointer<QObject> m_executingOwner;        // 当前原子回调的旧式 defer guard 身份。
        QString m_executingKey;
        bool m_legacyGuardAvailable = false;      // 允许旧 refresh()->Defer... 自回调穿透一次。
        QSet<QObject*> m_watchedOwners;            // 已建立销毁回调的所有者。
        bool m_flushScheduled = false;            // 防止重复安排空闲回投。
        bool m_flushing = false;                   // 防止嵌套事件循环重复遍历队列。
    };
}
