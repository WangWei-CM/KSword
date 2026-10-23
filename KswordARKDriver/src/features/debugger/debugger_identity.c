/* 这里只固定对象身份，不创建目标句柄，不读写目标，也不修改调试或授权状态。 */
#include <ntifs.h>
/* 导入独立租约入口及既有共享协议。 */
#include "debugger_identity.h"
/* 使用项目统一的非分页分配兼容入口。 */
#include "../../platform/pool_compat.h"

/* 记录在发布前取得全部引用，发布后仅由文件销毁回调释放。 */
typedef struct _KSW_DEBUGGER_IDENTITY_RECORD {
    /* 固定真实 IRP 请求方对象，避免请求方 PID 复用产生身份混淆。 */
    PEPROCESS OwnerProcess;
    /* 固定目标对象，阻止当前 PID 在租约期间被回收后指向另一对象。 */
    PEPROCESS TargetProcess;
    /* 线程来源额外持有真实线程，阻止同一进程内的 TID 回收复用。 */
    PETHREAD TargetThread;
    /* 保存明确的来源类型，不将进程租约悄悄切换成线程租约。 */
    ULONG SourceKind;
    /* 数字来源保留初次选择的 PID，后续调用不重新查找该数字。 */
    ULONG SourceProcessId;
    /* 实际 PID 始终从已引用对象取得，不向用户暴露对象指针。 */
    ULONG ProcessId;
    /* 线程来源保存实际 TID，进程来源保持零。 */
    ULONG ThreadId;
} KSW_DEBUGGER_IDENTITY_RECORD, *PKSW_DEBUGGER_IDENTITY_RECORD;

/* 为已有 WDF 文件追加不同类型的上下文，不替换 HVM 的文件上下文或回调。 */
typedef struct _KSW_DEBUGGER_IDENTITY_CONTEXT {
    /* 非分页不可变记录仅发布一次；并发初始化通过 CAS 交接所有权。 */
    PVOID volatile Record;
} KSW_DEBUGGER_IDENTITY_CONTEXT, *PKSW_DEBUGGER_IDENTITY_CONTEXT;

/* 只通过该独立类型取得自己的上下文。 */
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KSW_DEBUGGER_IDENTITY_CONTEXT, KswDebuggerGetIdentityContext);

/* 声明框架销毁回调，使 WDF 验证器保留准确的回调角色。 */
static EVT_WDF_OBJECT_CONTEXT_DESTROY KswDebuggerIdentityDestroy;

/* 失败候选及文件最终销毁共用同一引用释放规则，允许在 DISPATCH_LEVEL 调用。 */
static VOID KswDebuggerIdentityReleaseRecord(PKSW_DEBUGGER_IDENTITY_RECORD Record)
{
    /* 空上下文没有持有任何对象或分配。 */
    if (Record == NULL) { return; }
    /* 线程引用独立计数，失败候选及最终销毁都只释放一次。 */
    if (Record->TargetThread != NULL) { ObDereferenceObjectDeferDelete(Record->TargetThread); }
    /* 延迟最后一次目标对象删除，避免文件销毁回调的 IRQL 或锁环境触发同步删除。 */
    if (Record->TargetProcess != NULL) { ObDereferenceObjectDeferDelete(Record->TargetProcess); }
    /* 真实请求方的独立引用也必须精确释放一次。 */
    if (Record->OwnerProcess != NULL) { ObDereferenceObjectDeferDelete(Record->OwnerProcess); }
    /* 记录来自非分页池，销毁回调无需切换线程或等待工作项。 */
    ExFreePoolWithTag(Record, 'iDdK');
}

/* 文件引用归零后只访问自身上下文，不调用文件方法，也不持有文件自引用。 */
static VOID KswDebuggerIdentityDestroy(WDFOBJECT Object)
{
    /* WDF 明确允许 Destroy 回调读取对象上下文。 */
    PKSW_DEBUGGER_IDENTITY_CONTEXT context = KswDebuggerGetIdentityContext(Object);
    /* 原子摘除已发布记录，使最终释放只发生一次。 */
    PKSW_DEBUGGER_IDENTITY_RECORD record = (PKSW_DEBUGGER_IDENTITY_RECORD)InterlockedExchangePointer(&context->Record, NULL);
    /* 该回调可能在 DISPATCH_LEVEL，释放入口只使用相容的非分页与延迟删除操作。 */
    KswDebuggerIdentityReleaseRecord(record);
}

/* 身份租约没有附带执行载荷，严格保留现有 v1 包的保留字段语义。 */
static BOOLEAN KswDebuggerIdentityRequestValid(const KSWORD_ARK_DEBUGGER_REQUEST* Input)
{
    /* 逐字节检查现有上下文数组，避免另在内核栈分配一个大零数组。 */
    ULONG index;
    /* 公共标量不承载执行载荷，来源枚举以外的任何值都明确拒绝。 */
    if (Input->version != KSWORD_ARK_DEBUGGER_VERSION || Input->size != sizeof(*Input) ||
        Input->operation != KSWORD_ARK_DEBUGGER_IDENTITY_RETAIN || Input->threadId != 0UL ||
        Input->contextFlags > KSWORD_ARK_DEBUGGER_IDENTITY_SOURCE_THREAD_HANDLE || Input->flags != 0UL ||
        Input->reserved != 0UL || Input->bytes != 0ULL ||
        Input->reservedArgument != 0ULL || Input->protection != 0UL || Input->allocationType != 0UL) {
        /* 无效包不触及文件上下文或进程对象。 */
        return FALSE;
    }
    /* 数字 PID 引用不是目标执行操作，因此仅排除无效的零编号，不屏蔽 PID 4。 */
    if (Input->contextFlags == KSWORD_ARK_DEBUGGER_IDENTITY_SOURCE_PID) {
        /* 数字来源不能同时携带另一个句柄来源。 */
        if (Input->processId == 0UL || Input->address != 0ULL) { return FALSE; }
    } else {
        /* 句柄来源只从当前请求方自己的表解析，不接受用户声明的数字身份。 */
        if (Input->processId != 0UL || Input->address == 0ULL) { return FALSE; }
    }
    /* 不允许借用引用操作传递任何上下文数据。 */
    for (index = 0UL; index < KSWORD_ARK_DEBUGGER_CONTEXT_BYTES; ++index) {
        /* 遇到非零载荷立即返回明确的包验证失败。 */
        if (Input->context[index] != 0U) { return FALSE; }
    }
    /* 完整 v1 输入已通过纯身份请求验证。 */
    return TRUE;
}

/* 从已引用对象发布数字身份，失败响应始终保持初始化后的零身份。 */
static VOID KswDebuggerIdentityPublish(const KSW_DEBUGGER_IDENTITY_RECORD* Record,
    KSWORD_ARK_DEBUGGER_RESPONSE* Output)
{
    /* PID 来源和 HANDLE 来源都只返回真实对象的 PID。 */
    Output->processId = Record->ProcessId;
    /* 线程来源才返回已引用线程的 TID。 */
    Output->threadId = Record->ThreadId;
    /* 完整记录的引用和身份都已发布，当前调用幂等成功。 */
    Output->status = STATUS_SUCCESS;
}

/* 同一文件只绑定一个真实请求方、来源类型及实际对象，不比较易复用的句柄值。 */
static NTSTATUS KswDebuggerIdentityMatch(const KSW_DEBUGGER_IDENTITY_RECORD* Record,
    const KSW_DEBUGGER_IDENTITY_RECORD* Candidate)
{
    /* 已发布记录不可变，且当前 WDF 请求仍持有其关联的文件生命周期。 */
    return Record->OwnerProcess == Candidate->OwnerProcess && Record->SourceKind == Candidate->SourceKind &&
        Record->SourceProcessId == Candidate->SourceProcessId && Record->TargetProcess == Candidate->TargetProcess &&
        Record->TargetThread == Candidate->TargetThread ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* 仅解析请求方私有副本的身份，不创建句柄，不申请任何额外目标访问权限。 */
static NTSTATUS KswDebuggerIdentityReferenceSource(PKSW_DEBUGGER_IDENTITY_RECORD Record,
    const KSWORD_ARK_DEBUGGER_REQUEST* Input)
{
    /* 只在请求队列线程不属于 owner 时短暂切到 owner 的句柄表。 */
    KAPC_STATE attachState;
    /* 记录短暂 owner 上下文切换，保证每次切入都在返回前切出。 */
    BOOLEAN attached = FALSE;
    /* 保存对象查找的原始状态。 */
    NTSTATUS status;
    /* 数字来源只固定首次查找到的进程对象，不执行任何目标操作。 */
    if (Record->SourceKind == KSWORD_ARK_DEBUGGER_IDENTITY_SOURCE_PID) {
        /* 唯一数字查找只产生对象引用。 */
        status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)Input->processId, &Record->TargetProcess);
    } else {
        /* 只允许在真实 IRP owner 自己的句柄表解释用户态句柄。 */
        if (PsGetCurrentProcess() != Record->OwnerProcess) {
            /* 这里切入的是请求方，绝不切入被观察目标。 */
            KeStackAttachProcess((PRKPROCESS)Record->OwnerProcess, &attachState);
            /* 标记必须配对的 owner 上下文切换。 */
            attached = TRUE;
        }
        /* 进程来源必须通过准确的对象类型和 UserMode 校验。 */
        if (Record->SourceKind == KSWORD_ARK_DEBUGGER_IDENTITY_SOURCE_PROCESS_HANDLE) {
            /* DesiredAccess=0 只保留已有对象身份，不绕过或扩展其实际授权。 */
            status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Input->address, 0UL,
                *PsProcessType, UserMode, (PVOID*)&Record->TargetProcess, NULL);
        } else {
            /* 线程来源也只从 owner 已有的线程句柄取得引用。 */
            status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Input->address, 0UL,
                *PsThreadType, UserMode, (PVOID*)&Record->TargetThread, NULL);
        }
        /* owner 上下文内没有 I/O、等待、目标访问或对象分配。 */
        if (attached) { KeUnstackDetachProcess(&attachState); }
    }
    /* 无效句柄、错误类型或不存在 PID 都保留真实失败状态。 */
    if (!NT_SUCCESS(status)) { return status; }
    /* 已引用的线程固定其所属进程，后者再取得一份独立生命周期引用。 */
    if (Record->TargetThread != NULL) {
        /* 所属进程由真实 ETHREAD 决定，不信任用户声明的 PID。 */
        Record->TargetProcess = IoThreadToProcess(Record->TargetThread);
        /* 与线程引用独立计数，最终清理保持精确平衡。 */
        ObReferenceObject(Record->TargetProcess);
        /* TID 也从已引用线程读取，不能重新按可复用原始 HANDLE 查询。 */
        Record->ThreadId = HandleToULong(PsGetThreadId(Record->TargetThread));
    }
    /* 进程身份始终来自此时已持有引用的实际对象。 */
    Record->ProcessId = HandleToULong(PsGetProcessId(Record->TargetProcess));
    /* 空进程编号不能提供可用身份；所有候选引用由调用者统一释放。 */
    if (Record->ProcessId == 0UL || (Record->TargetThread != NULL && Record->ThreadId == 0UL)) { return STATUS_INVALID_CID; }
    /* 对象已完整引用，后续发布不会触发额外权限路径。 */
    return STATUS_SUCCESS;
}

/* 分派器在完成 WDF 请求之前同步调用此入口，目标引用一直随文件存续。 */
NTSTATUS KswordARKDebuggerRetainIdentity(WDFREQUEST Request,
    const KSWORD_ARK_DEBUGGER_REQUEST* Input, KSWORD_ARK_DEBUGGER_RESPONSE* Output)
{
    /* 保存现有文件对象，不创建新 WDF 对象或文件自引用。 */
    WDFFILEOBJECT fileObject;
    /* 新类型上下文与既有功能上下文并存。 */
    PKSW_DEBUGGER_IDENTITY_CONTEXT context = NULL;
    /* 候选记录在全部初始化完成后才允许发布。 */
    PKSW_DEBUGGER_IDENTITY_RECORD candidate = NULL;
    /* 并发胜者的记录保持不可变且随文件生存。 */
    PKSW_DEBUGGER_IDENTITY_RECORD existing;
    /* 为新上下文指定独立销毁回调，不更改文件原有 Cleanup/Close。 */
    WDF_OBJECT_ATTRIBUTES attributes;
    /* 从原始 IRP 获取真实请求方，不能借用队列工作线程的当前进程。 */
    PEPROCESS ownerProcess;
    /* 保留当前有效请求中的原始 IRP。 */
    PIRP irp;
    /* 记录上下文分配与目标查找的真实结果。 */
    NTSTATUS status;
    /* 初始化整个既有响应，避免任何内核指针或未初始化数据泄露。 */
    RtlZeroMemory(Output, sizeof(*Output));
    /* 始终返回明确的共享协议版本。 */
    Output->version = KSWORD_ARK_DEBUGGER_VERSION;
    /* 始终返回精确共享响应大小。 */
    Output->size = sizeof(*Output);
    /* 该独立入口只声明引用能力，不将其混同为任何目标访问权限。 */
    Output->capabilities = KSWORD_ARK_DEBUGGER_CAP_IDENTITY_RETAIN;
    /* 进程查找只在既有 PASSIVE_LEVEL 合同内执行。 */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { Output->status = STATUS_INVALID_DEVICE_STATE; return STATUS_SUCCESS; }
    /* 验证完整请求后才获取对象身份。 */
    if (!KswDebuggerIdentityRequestValid(Input)) { Output->status = STATUS_INVALID_PARAMETER; return STATUS_SUCCESS; }
    /* 当前请求关联文件负责租约释放，而不是全局注册表或进程 PID 表。 */
    fileObject = WdfRequestGetFileObject(Request);
    /* 无文件请求不能承载自动关闭的身份租约。 */
    if (fileObject == NULL) { Output->status = STATUS_INVALID_HANDLE; return STATUS_SUCCESS; }
    /* 仅查看当前仍未完成请求的原始 IRP。 */
    irp = WdfRequestWdmGetIrp(Request);
    /* 真实 owner 来自 IRP，用户载荷无法指定。 */
    ownerProcess = irp != NULL ? IoGetRequestorProcess(irp) : NULL;
    /* 无实际请求方的请求不创建未归属的长期引用。 */
    if (ownerProcess == NULL) { Output->status = STATUS_INVALID_PARAMETER; return STATUS_SUCCESS; }
    /* WDF 为这个新上下文类型初始化零内存，不覆盖已有类型。 */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, KSW_DEBUGGER_IDENTITY_CONTEXT);
    /* 在对象引用归零时才释放目标，涵盖仍在执行的关联 I/O。 */
    attributes.EvtDestroyCallback = KswDebuggerIdentityDestroy;
    /* 已分配同类型上下文时，WDF 返回已有上下文，不重复安装回调。 */
    status = WdfObjectAllocateContext(fileObject, &attributes, (PVOID*)&context);
    /* STATUS_OBJECT_NAME_EXISTS 仍带有可用的已有上下文。 */
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_EXISTS) { Output->status = status; return STATUS_SUCCESS; }
    /* 通过原子读取观察完整发布后的记录。 */
    existing = (PKSW_DEBUGGER_IDENTITY_RECORD)InterlockedCompareExchangePointer(&context->Record, NULL, NULL);
    /* 数字来源的已有绑定不再查找 PID，始终保持最初引用的目标对象。 */
    if (existing != NULL && Input->contextFlags == KSWORD_ARK_DEBUGGER_IDENTITY_SOURCE_PID) {
        /* 仅相同 owner、来源与原始选择数字可以复用已有记录。 */
        if (existing->OwnerProcess != ownerProcess || existing->SourceKind != KSWORD_ARK_DEBUGGER_IDENTITY_SOURCE_PID ||
            existing->SourceProcessId != Input->processId) { Output->status = STATUS_INVALID_PARAMETER; return STATUS_SUCCESS; }
        /* 不用输入回显或补查代替已引用对象的真实身份。 */
        KswDebuggerIdentityPublish(existing, Output);
        /* 原目标引用一直由此文件持有，本次无需额外分配。 */
        return STATUS_SUCCESS;
    }
    /* 候选记录本身也必须位于非分页池，以支持文件销毁的实际 IRQL。 */
    candidate = (PKSW_DEBUGGER_IDENTITY_RECORD)KswordARKAllocateNonPagedPool(sizeof(*candidate), 'iDdK');
    /* 不足时保留空上下文，后续调用仍可重试。 */
    if (candidate == NULL) { Output->status = STATUS_INSUFFICIENT_RESOURCES; return STATUS_SUCCESS; }
    /* 清零使任一中途失败都可使用统一引用清理路径。 */
    RtlZeroMemory(candidate, sizeof(*candidate));
    /* 在发布前先持有真实请求方对象，保持 owner 指针身份稳定。 */
    ObReferenceObject(ownerProcess);
    /* 将这一份新引用的所有权交给候选记录。 */
    candidate->OwnerProcess = ownerProcess;
    /* 固定输入来源类型，后续同文件调用不能悄悄更换。 */
    candidate->SourceKind = Input->contextFlags;
    /* 只有数字来源使用该原始选择值，句柄来源保持零。 */
    candidate->SourceProcessId = Input->processId;
    /* 所有来源都只增加对象引用，不创建或复制任何访问句柄。 */
    status = KswDebuggerIdentityReferenceSource(candidate, Input);
    /* 目标不存在时精确释放候选及 owner，不保留半初始化租约。 */
    if (!NT_SUCCESS(status)) { KswDebuggerIdentityReleaseRecord(candidate); Output->status = status; return STATUS_SUCCESS; }
    /* 只有一个初始化请求能取得文件租约的记录所有权。 */
    existing = (PKSW_DEBUGGER_IDENTITY_RECORD)InterlockedCompareExchangePointer(&context->Record, candidate, NULL);
    /* CAS 成功后记录与全部引用由文件销毁回调负责，无需保留请求本身。 */
    if (existing == NULL) { KswDebuggerIdentityPublish(candidate, Output); return STATUS_SUCCESS; }
    /* 先比较真实对象，不能把可复用的 HANDLE 数字相等当作同一身份。 */
    status = KswDebuggerIdentityMatch(existing, candidate);
    /* CAS 失败方从未发布，立即平衡其线程、目标、owner 与非分页池分配。 */
    KswDebuggerIdentityReleaseRecord(candidate);
    /* 与胜者对象绑定一致才幂等成功，另一对象不能替换已有租约。 */
    if (NT_SUCCESS(status)) { KswDebuggerIdentityPublish(existing, Output); } else { Output->status = status; }
    /* 业务结果留在 typed status，传输仍返回一个完整响应。 */
    return STATUS_SUCCESS;
}
