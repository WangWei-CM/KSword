#pragma once

#include "hvm_exit.h"
#include "hvm_resident.h"

#include "driver/KswordArkHvmNoAttachIoctl.h"

/* 内部发布状态绝不对外表示为已完成停止。 */
#define KSW_NOATTACH_PUBLISHING 11L
/* 命令先占用停止，再检查 stopId，防止 STOPPED 状态 ABA。 */
#define KSW_NOATTACH_COMMAND_RESERVING 12L
/* Intel 将 MBEC 定义为次级处理器控制的第 22 位。 */
#define KSW_NOATTACH_MBEC_CONTROL (1UL << 22)
/* 启用 MBEC 后，此 EPT 位允许用户态指令取指。 */
#define KSW_NOATTACH_USER_EXECUTE (1ULL << 10)

/* 保留一份不可变目标绑定，直到全部常驻读者排空。 */
typedef struct _KSW_HVM_NOATTACH
{
    /* 仅串行化 PASSIVE_LEVEL 控制与延后退休。 */
    EX_PUSH_LOCK Lock;
    /* 本生命周期被预约期间，禁止所有外部 HVM 修改。 */
    volatile LONG Owned;
    /* 取得预约不代表获准停止其它客户端的资源。 */
    volatile LONG ResourcesOwned;
    /* 退出回调必须在 TEB 可被复用前撤销发布。 */
    volatile LONG Enabled;
    /* 当前状态独立于不可变身份字段发布。 */
    volatile LONG State;
    /* 保护有界快照复制，避免与根模式写者并发撕裂。 */
    volatile LONG SnapshotSequence;
    /* 标识唯一获准完成在途 MTF 请求的处理器。 */
    volatile LONG StepProcessor;
    /* 防止重复排入清理工作项。 */
    volatile LONG WorkQueued;
    /* 发布回调初始化成功状态。 */
    BOOLEAN Initialized;
    /* 驱动卸载排空回调前，停止排入新工作。 */
    BOOLEAN ShuttingDown;
    /* 在驱动生命周期内保留异步关闭工作项。 */
    WDFWORKITEM Work;
    /* 绑定真实请求方对象，不信任调用方提供的 PID。 */
    PEPROCESS Owner;
    /* 目标进程身份不依赖可能复用的 PID。 */
    PEPROCESS Process;
    /* 直接比较当前 ETHREAD 指针；单独的 TEB 不是身份。 */
    PETHREAD Thread;
    /* 所属控制句柄关闭时撤销该会话。 */
    WDFFILEOBJECT File;
    /* 保留已核对的所有者创建时间，用于 ACK 恢复。 */
    ULONGLONG OwnerCreationTime;
    /* 目标 TEB 仅保留为数值交叉判据，绝不解引用。 */
    ULONGLONG Teb;
    /* 使用预先构建的不可变根表；VM-exit 路径不修改 EPT 表。 */
    ULONGLONG NormalEptp;
    /* 禁止用户取指，同时允许监督态执行。 */
    ULONGLONG UserGateEptp;
    /* 禁止监督态取指，以截获每次返回用户态。 */
    ULONGLONG KernelGateEptp;
    /* 当前停止证据与历史 HVM 事件环分开保存。 */
    KSWORD_ARK_HVM_NOATTACH_RESPONSE Snapshot;
} KSW_HVM_NOATTACH;

/* 即使句柄清理与 OPEN 请求并发，也保留正在关闭的状态。 */
typedef struct _KSW_HVM_NOATTACH_FILE_CONTEXT
{
    /* KMDF 在接收请求前将上下文初始化为零。 */
    volatile LONG Closing;
} KSW_HVM_NOATTACH_FILE_CONTEXT;
/* 只有主控制设备安装此私有文件上下文。 */
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KSW_HVM_NOATTACH_FILE_CONTEXT, KswordARKHvmNoAttachFileContext);

/* 单会话存储位于非分页内存，地址在驱动生命周期内固定。 */
extern KSW_HVM_NOATTACH g_KswordNoAttach;
/* 初始化生命周期回调，不进入 VMX。 */
NTSTATUS KswordARKHvmNoAttachInitialize(VOID);
/* 既有运行时销毁前，先撤销、排空并退休本会话。 */
VOID KswordARKHvmNoAttachShutdown(VOID);
/* 查询资源修改是否由本子系统独占。 */
BOOLEAN KswordARKHvmNoAttachOwned(VOID);
/* 撤销发布，并排入有界的 PASSIVE_LEVEL 清理。 */
VOID KswordARKHvmNoAttachRevoke(VOID);
/* 即使调用方仍存活，KMDF 也会关闭此确切会话。 */
VOID KswordARKHvmNoAttachFileCleanup(WDFFILEOBJECT File);
/* 在 PASSIVE_LEVEL 验证并执行一次固定协议操作。 */
NTSTATUS KswordARKHvmNoAttachControl(const KSWORD_ARK_HVM_NOATTACH_REQUEST *Request,
                                     KSWORD_ARK_HVM_NOATTACH_RESPONSE *Response, ULONG OwnerPid,
                                     WDFFILEOBJECT File);
/* 持运行时锁的封装复用既有全处理器生命周期。 */
NTSTATUS KswordARKHvmNoAttachLifecycle(BOOLEAN Start, ULONG Flags);
/* 常驻插入前构建 MBEC 根表；所有页面纳入运行时台账。 */
NTSTATUS KswordARKHvmNoAttachBuildRootsLocked(KSW_HVM_RUNTIME *Runtime);
/* 常驻处理器数量归零后才撤销根表指针。 */
VOID KswordARKHvmNoAttachResetRootsLocked(KSW_HVM_RUNTIME *Runtime);
/* 仅处理本方会话的模式门禁和 MTF 退出。 */
BOOLEAN KswordARKHvmNoAttachHandleExit(KSW_HVM_RESIDENT_VCPU *Context, KSW_HVM_GPR_FRAME *Frame, ULONG Reason,
                                       ULONGLONG Qualification, ULONG *Action);
/* 在每个 CPU 请求一次无副作用的超调用，使 PAUSE 和 CONTINUE 生效。 */
NTSTATUS KswordARKHvmNoAttachRendezvous(KSW_HVM_RUNTIME *Runtime);
