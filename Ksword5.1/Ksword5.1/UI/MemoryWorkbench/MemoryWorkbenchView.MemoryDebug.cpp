// 独立进程内存调试页的模式配置：复用工作台服务，不建立 Windows 调试会话。
#include "MemoryWorkbenchView.h"
#include "WorkbenchTarget.h"
#include "WorkbenchSessionBar.h"
#include "Int3PatchPanel.h"

#include <QPointer>
#include <QStackedWidget>

namespace ks::ui
{
    LeaveEntryStatus MemoryWorkbenchView::setMemoryDebugMode(const bool enabled, LeaveCompletion completion)
    {
        if (memoryDebugMode_ == enabled)
        {
            return LeaveEntryStatus::Completed;
        }
        if (!target_ || (enabled && embedded_))
        {
            return LeaveEntryStatus::Rejected;
        }
        const QPointer<MemoryWorkbenchView> self(this);
        if (enabled)
        {
            // 先用正常身份守卫切换，再生效显示模式；拒绝时原工作台完全不变。
            IdentityRequest request;
            request.scope = ksword::memwb::Scope::ProcessVirtual;
            request.channel = ksword::memwb::Channel::UserMode;
            const auto current = target_->session();
            if (current.pid != 0U)
            {
                request.pinPid = current.pid;
                request.expectCreateTime = current.processCreateTime100ns;
            }
            const auto status = target_->requestIdentity(request, LeaveReason::ScopeChange,
                [self, enabled, completion = std::move(completion)](bool allowed) mutable {
                    if (!self) return;
                    if (allowed) self->finishMemoryDebugMode(enabled);
                    if (self && completion) completion(allowed);
                });
            if (!self) return LeaveEntryStatus::Rejected;
            if (status != LeaveEntryStatus::Completed) return status;
        }
        finishMemoryDebugMode(enabled);
        return LeaveEntryStatus::Completed;
    }

    void MemoryWorkbenchView::finishMemoryDebugMode(const bool enabled)
    {
        memoryDebugMode_ = enabled;
        target_->setPolicy(WorkbenchTarget::Policy{
            /*lockToDock=*/embedded_,
            /*allowKernelPhysical=*/!embedded_ && !enabled,
            /*allowFollowDock=*/!enabled});
        sessionBar_->setMemoryDebugMode(enabled);
        // int3 在本页不是调试断点；隐藏入口且禁用，退出也不处理其它视图的补丁。
        int3Panel_->setEnabled(!enabled);
        int3Panel_->setVisible(!enabled);
        if (enabled)
        {
            subTabStack_->setCurrentIndex(1);
        }
        refreshTargetDisplays();
        refreshChannelGateDisplay();
    }

    bool MemoryWorkbenchView::isMemoryDebugMode() const noexcept
    {
        return memoryDebugMode_;
    }

    LeaveEntryStatus MemoryWorkbenchView::clearMemoryDebugTarget(LeaveCompletion completion)
    {
        return memoryDebugMode_ && target_ ? target_->clearMemoryDebugTarget(std::move(completion)) : LeaveEntryStatus::Rejected;
    }

}
