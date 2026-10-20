#include "UiCommitCoordinator.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // 保留已有属性名，使菜单状态与存量表格代码的观察结果一致。
    constexpr char kMenuDepthProperty[] = "KSWORD_TABLE_CONTEXT_MENU_DEPTH";
    constexpr char kCoordinatorName[] = "ksword_ui_commit_coordinator";

    bool realLeftCtrlHeld()
    {
        return (::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    }

    // Qt 下拉列表位于独立 Popup，父控件用于区分下拉列表与业务 QMenu。
    bool realComboPopupOpen()
    {
        QWidget* const popup = QApplication::activePopupWidget();
        return popup != nullptr && qobject_cast<QComboBox*>(popup->parentWidget()) != nullptr;
    }
}

namespace ks::ui
{
    UiCommitCoordinator::UiCommitCoordinator(
        QApplication* application,
        UiCommitInteractionProbes probes,
        UiCommitDrainOptions drainOptions)
        : QObject(application)
        , m_application(application)
        , m_probes(std::move(probes))
        , m_drainOptions(std::move(drainOptions))
    {
        m_drainOptions.softBudgetNanoseconds = std::max<qint64>(1, m_drainOptions.softBudgetNanoseconds);
        // 默认探针保持只检测左 Ctrl 的已有产品行为，右 Ctrl 不阻塞刷新。
        if (!m_probes.leftCtrlHeld)
        {
            m_probes.leftCtrlHeld = realLeftCtrlHeld;
        }
        if (!m_probes.comboPopupOpen)
        {
            m_probes.comboPopupOpen = realComboPopupOpen;
        }
        if (application != nullptr)
        {
            application->installEventFilter(this);
        }
    }

    UiCommitCoordinator::~UiCommitCoordinator()
    {
        // Qt 清理事件过滤器和所有 queued 回调；释放快照捕获，不再执行提交。
        if (!m_application.isNull())
        {
            m_application->removeEventFilter(this);
        }
    }

    UiCommitCoordinator* UiCommitCoordinator::forApplication(QApplication* application)
    {
        if (application == nullptr)
        {
            application = qobject_cast<QApplication*>(QCoreApplication::instance());
        }
        if (application == nullptr)
        {
            return nullptr;
        }
        // 不使用进程静态裸指针；QApplication 重建后得到新的生命周期对象。
        for (QObject* const child : application->children())
        {
            if (child->objectName() == QLatin1String(kCoordinatorName))
            {
                return static_cast<UiCommitCoordinator*>(child);
            }
        }
        auto* const coordinator = new UiCommitCoordinator(application);
        coordinator->setObjectName(QLatin1String(kCoordinatorName));
        return coordinator;
    }

    bool UiCommitCoordinator::inputBlocked() const
    {
        return m_probes.leftCtrlHeld() || m_probes.comboPopupOpen();
    }

    bool UiCommitCoordinator::isBlocked(const QList<QAbstractItemView*>& views) const
    {
        if (inputBlocked())
        {
            return true;
        }
        return std::any_of(views.cbegin(), views.cend(), [](const QAbstractItemView* view)
        {
            return view != nullptr && view->property(kMenuDepthProperty).toInt() > 0;
        });
    }

    // 屏障解除时仍以完整目标集合复查，任一目标销毁意味着原子提交不可执行。
    bool UiCommitCoordinator::targetsAlive(const PendingCommit& pending) const
    {
        return !pending.owner.isNull() && std::all_of(
            pending.views.cbegin(), pending.views.cend(), [](const auto& view)
            {
                return !view.isNull();
            });
    }

    bool UiCommitCoordinator::pendingBlocked(const PendingCommit& pending) const
    {
        QList<QAbstractItemView*> views;
        views.reserve(pending.views.size());
        for (const auto& view : pending.views)
        {
            views.append(view.data());
        }
        return isBlocked(views);
    }

    void UiCommitCoordinator::removeIndex(const PendingCommit& pending)
    {
        auto owner = m_pendingIndex.find(pending.ownerIdentity);
        if (owner == m_pendingIndex.end()) return;
        owner->remove(pending.key);
        if (owner->isEmpty()) m_pendingIndex.erase(owner);
    }

    void UiCommitCoordinator::removeMatching(QObject* owner, const QString& key)
    {
        const auto ownerEntry = m_pendingIndex.constFind(owner);
        if (ownerEntry == m_pendingIndex.cend()) return;
        const auto keyEntry = ownerEntry->constFind(key);
        if (keyEntry == ownerEntry->cend()) return;
        const auto pending = m_pending.find(keyEntry.value());
        if (pending == m_pending.end()) return;
        removeIndex(pending.value());
        m_pending.erase(pending);
    }

    // 立即释放被销毁页面的捕获快照，避免下拉菜单长期开启时保留大块数据。
    void UiCommitCoordinator::watchOwner(QObject* owner)
    {
        if (m_watchedOwners.contains(owner)) return;
        m_watchedOwners.insert(owner);
        connect(owner, &QObject::destroyed, this, [this, owner]()
        {
            m_watchedOwners.remove(owner);
            const auto generations = m_pendingIndex.take(owner);
            for (const quint64 generation : generations) m_pending.remove(generation);
        });
    }

    bool UiCommitCoordinator::deferIfBlocked(
        QObject* owner,
        const QString& key,
        const QList<QAbstractItemView*>& views,
        std::function<void()> action)
    {
        // 旧入口常缓存 [this]{ refresh(); }，refresh 顶部会再次进入同一个 defer guard。
        // 只放行这一次 guard，避免永久自排队；submit 的新快照不享有这个许可。
        const bool executingLegacyGuard = m_flushing && m_legacyGuardAvailable
            && m_executingOwner.data() == owner && m_executingKey == key;
        if (executingLegacyGuard) m_legacyGuardAvailable = false;
        return deferChecked(owner, key, views, action, executingLegacyGuard);
    }

    // 同一次输入状态只查询一次；未阻塞时保留 action，避免两次探针间松键丢提交。
    bool UiCommitCoordinator::deferChecked(
        QObject* owner,
        const QString& key,
        const QList<QAbstractItemView*>& views,
        std::function<void()>& action,
        const bool executingLegacyGuard)
    {
        if (owner == nullptr || key.isEmpty() || !action)
        {
            return false;
        }
        // 先淘汰同键旧提交。松开 Ctrl 后先收到新快照时，旧快照不可随后覆盖它。
        removeMatching(owner, key);
        if (!isBlocked(views) && (executingLegacyGuard || (!m_flushing && !m_roundActive && !m_continuationPending)))
        {
            if (!m_pending.isEmpty() && !m_flushing)
            {
                notifyInteractionEnded();
            }
            return false;
        }
        PendingCommit pending;
        pending.ownerIdentity = owner;
        pending.owner = owner;
        pending.key = key;
        pending.action = std::move(action);
        for (QAbstractItemView* const view : views)
        {
            if (view != nullptr)
            {
                pending.views.append(view);
            }
        }
        watchOwner(owner);
        const quint64 generation = ++m_nextGeneration;
        m_pendingIndex[owner].insert(key, generation);
        m_pending.insert(generation, std::move(pending));
        if (m_flushing || m_roundActive || m_continuationPending) scheduleContinuation();
        return true;
    }

    UiCommitSubmission UiCommitCoordinator::submit(
        QObject* owner,
        const QString& key,
        const QList<QAbstractItemView*>& views,
        std::function<void()> action)
    {
        if (owner == nullptr || key.isEmpty() || !action)
        {
            return UiCommitSubmission::Rejected;
        }
        // 屏障检测与动作接管共用一次判断，只有缓存路径移动捕获快照。
        if (deferChecked(owner, key, views, action))
        {
            return UiCommitSubmission::Deferred;
        }
        action();
        return UiCommitSubmission::Executed;
    }

    void UiCommitCoordinator::beginContextMenu(QAbstractItemView* view)
    {
        if (view != nullptr)
        {
            view->setProperty(kMenuDepthProperty, view->property(kMenuDepthProperty).toInt() + 1);
        }
    }

    void UiCommitCoordinator::endContextMenu(QAbstractItemView* view)
    {
        const QPointer<QAbstractItemView> guardedView(view);
        // Hide 位于 QMenu::exec 返回之前，延后才能保护原 action 捕获的行身份。
        QTimer::singleShot(0, this, [this, guardedView]()
        {
            if (!guardedView.isNull())
            {
                guardedView->setProperty(kMenuDepthProperty,
                    std::max(0, guardedView->property(kMenuDepthProperty).toInt() - 1));
            }
            notifyInteractionEnded();
        });
    }

    void UiCommitCoordinator::notifyInteractionEnded()
    {
        // 已扫描但受屏障阻塞的项也必须在实际交互结束后重新检查。
        m_flushRequested = true;
        scheduleFlush();
    }

    void UiCommitCoordinator::scheduleFlush()
    {
        if (m_flushScheduled || m_pending.isEmpty())
        {
            return;
        }
        m_flushScheduled = true;
        QTimer::singleShot(0, this, [this]()
        {
            m_flushScheduled = false;
            flush();
        });
    }

    void UiCommitCoordinator::scheduleContinuation()
    {
        m_continuationPending = true;
        scheduleFlush();
    }

    void UiCommitCoordinator::flush()
    {
        if (m_flushing)
        {
            m_flushRequested = true;
            return;
        }
        m_flushing = true;
        m_continuationPending = false;
        if (m_flushRequested)
        {
            m_roundActive = false;
            m_flushRequested = false;
        }
        if (!m_roundActive)
        {
            m_roundActive = true;
            m_roundCutoff = m_nextGeneration;
            m_roundCursor = 0;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        const auto clock = m_drainOptions.monotonicNanoseconds;
        const qint64 started = clock ? clock() : 0;
        const auto budgetExpired = [&]()
        {
            return (clock ? clock() - started : elapsed.nsecsElapsed())
                >= m_drainOptions.softBudgetNanoseconds;
        };
        while (true)
        {
            // 每次按代次重新查找：用户回调可覆盖任何 key 或销毁任意 owner。
            const auto next = m_pending.upperBound(m_roundCursor);
            if (next == m_pending.end() || next.key() > m_roundCutoff)
            {
                m_roundActive = false;
                break;
            }
            m_roundCursor = next.key();
            if (!targetsAlive(next.value()))
            {
                removeIndex(next.value());
                m_pending.erase(next);
            }
            else if (!pendingBlocked(next.value()))
            {
                // 原子目标组不拆开；预算只能约束多个 action 的串联，不能抢占单个 action。
                PendingCommit pending = std::move(next.value());
                removeIndex(pending);
                m_pending.erase(next);
                const QPointer<UiCommitCoordinator> self(this);
                m_executingOwner = pending.owner;
                m_executingKey = pending.key;
                m_legacyGuardAvailable = true;
                pending.action();
                if (self.isNull()) return;
                m_legacyGuardAvailable = false;
                m_executingOwner.clear();
                m_executingKey.clear();
            }
            // 连屏障检查也计入同一份预算；游标跨续轮保存，避免反复从头扫描饥饿。
            if (budgetExpired()) break;
        }
        m_flushing = false;
        if (m_pending.isEmpty())
        {
            m_roundActive = false;
            m_continuationPending = false;
            return;
        }
        if (m_roundActive || m_nextGeneration > m_roundCutoff || m_flushRequested)
        {
            scheduleContinuation();
        }
    }

    qsizetype UiCommitCoordinator::pendingCount() const
    {
        return m_pending.size();
    }

    bool UiCommitCoordinator::eventFilter(QObject* watched, QEvent* event)
    {
        if (event != nullptr)
        {
            // 只把释放事件作为调度提示，flush 始终重查真实左 Ctrl 状态。
            if (event->type() == QEvent::KeyRelease)
            {
                const auto* const key = static_cast<QKeyEvent*>(event);
                if (key->key() == Qt::Key_Control && !key->isAutoRepeat())
                {
                    notifyInteractionEnded();
                }
            }
            else if (event->type() == QEvent::Hide)
            {
                QWidget* const hidden = qobject_cast<QWidget*>(watched);
                if (hidden != nullptr && qobject_cast<QComboBox*>(hidden->parentWidget()) != nullptr)
                {
                    notifyInteractionEnded();
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
}
