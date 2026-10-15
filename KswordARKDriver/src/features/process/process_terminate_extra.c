#include "process_terminate_extra.h"
#include "../dyndata/dyndata_v4_internal.h"
#include "ark/ark_push_lock.h"
#include "../../platform/runtime_signature_scan.h"
#include "../../platform/pool_compat.h"

/*
 * The private PspTerminateProcess ABI is not publicly versioned.  The old
 * four-argument guess was never sufficient proof of parameter roles and is
 * therefore compile-time disabled until a per-build ABI profile exists.
 */
#define KSWORD_ARK_PSP_TERMINATE_ABI_VERIFIED 0

// ntddk.h 不公开这两个 CID/归属查询声明，按既有模块声明可选查询入口。
NTSYSAPI NTSTATUS NTAPI PsLookupThreadByThreadId(_In_ HANDLE ThreadId, _Outptr_ PETHREAD* Thread);
NTSYSAPI PEPROCESS NTAPI PsGetThreadProcess(_In_ PETHREAD Thread);

// 已核对的 x64 四参数 ABI：进程、排除线程、退出状态、终止标志。
typedef NTSTATUS (NTAPI* KSW_PSP_TERMINATE_PROCESS_FN)(PEPROCESS Process, PETHREAD ExcludedThread, NTSTATUS ExitStatus, ULONG Flags);
// 枚举入口可选解析，避免在未导出该 API 的系统上产生加载依赖。
typedef PETHREAD (NTAPI* KSW_NEXT_PROCESS_THREAD_FN)(PEPROCESS Process, PETHREAD Thread);

NTSTATUS
KswordARKDriverTerminateProcessPsp(_In_ PEPROCESS Process, _In_ NTSTATUS ExitStatus)
{
#if !KSWORD_ARK_PSP_TERMINATE_ABI_VERIFIED
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(ExitStatus);
    return STATUS_NOT_SUPPORTED;
#else
    // 当前 v4 状态已经过内核 PE 和 RSDS 身份校验，只读取可选函数 RVA。
    KSW_DYN_V4_MODULE_STATE* state = NULL;
    KSW_RUNTIME_IMAGE_VIEW* view = NULL;
    ULONG_PTR base = 0;
    ULONG size = 0;
    ULONG rva = 0;
    ULONG index = 0;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    // 该完整入口序列依次保存 RCX/RDX/R8D/R9D，确认四参数角色而非旧两参数 ABI。
    static const UCHAR abi4Prologue[] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xec, 0x20, 0x41, 0x8b, 0xf9, 0x41, 0x8b, 0xe8,
        0x4c, 0x8b, 0xfa, 0x48, 0x8b, 0xd9
    };
    // 安全读取当前入口；更新编译器或内核改变序列时返回不支持，不能猜 ABI。
    UCHAR observedPrologue[sizeof(abi4Prologue)] = { 0 };
    // 入口要求非空对象、PASSIVE_LEVEL 和非当前请求进程，防止 IOCTL 自终止。
    if (Process == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        Process == PsGetCurrentProcess() ||
        Process == PsInitialSystemProcess ||
        HandleToULong(PsGetProcessId(Process)) <= 4UL) {
        return STATUS_INVALID_PARAMETER;
    }
    // v4 更新可并发执行；在共享锁下复制事实，不缓存跨代次函数指针。
    KswordARKAcquirePushLockShared(&g_KswordDynDataV4Lock);
    // NTOSKRNL 与 NTKRLA57 分别占据前两个槽位，按实际可用的匹配模块查找。
    for (index = 0; index < 2UL && rva == 0; ++index) {
        ULONG itemIndex = 0;
        // 只接受经过完整身份匹配的已占用内核模块状态。
        state = &g_KswordDynDataV4State.Modules[index];
        if (!state->Occupied || state->StoredItemCount > KSW_DYN_V4_MAX_ITEMS_PER_MODULE ||
            (state->PublicEntry.statusFlags & KSW_DYN_V4_STATUS_FLAG_IDENTITY_MATCHED) == 0 ||
            (state->PublicEntry.module.image.classId != KSW_DYN_PROFILE_CLASS_NTOSKRNL &&
             state->PublicEntry.module.image.classId != KSW_DYN_PROFILE_CLASS_NTKRLA57)) {
            continue;
        }
        // 在有界 item 数组中查找唯一的新函数 ID。
        for (itemIndex = 0; itemIndex < state->StoredItemCount; ++itemIndex) {
            const KSW_DYN_V4_ITEM_PACKET* item = &state->Items[itemIndex];
            // 只接受 core 组的 GlobalRva，拒绝高位和空 RVA。
            if (item->itemId == KSW_DYN_V4_ITEM_ID_PSP_TERMINATE_PROCESS &&
                item->capabilityGroupId == KSW_DYN_V4_CAPABILITY_GROUP_NTOS_CORE &&
                item->itemKind == KSW_DYN_V4_ITEM_KIND_GLOBAL_RVA &&
                item->valueHigh == 0 && item->valueLow != 0 &&
                item->valueLow < state->PublicEntry.module.image.sizeOfImage) {
                base = (ULONG_PTR)state->PublicEntry.module.image.imageBase;
                size = state->PublicEntry.module.image.sizeOfImage;
                rva = item->valueLow;
                break;
            }
        }
    }
    // 私有函数调用不持有 DynData 锁，避免回调终止通知时重入。
    KswordARKReleasePushLockShared(&g_KswordDynDataV4Lock);
    // 缺失符号或 RVA 溢出时明确返回不可用，让后续方法继续。
    if (rva == 0 || base == 0 || base > MAXULONG_PTR - rva) {
        return STATUS_NOT_SUPPORTED;
    }
    // PE 视图较大，放在非分页池避免耗尽内核栈。
    view = (KSW_RUNTIME_IMAGE_VIEW*)KswordARKAllocateNonPagedPool(sizeof(*view), 'tPsK');
    // 分配失败保持确定的错误返回。
    if (view == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // 由安全读取工具解析当前 PE，只接受映像可执行节内的目标。
    if (KswordARKRuntimeInitializeImageView((PVOID)base, size, view) && view->Size == size &&
        KswordARKRuntimeAddressIsExecutable(view, base + rva, sizeof(abi4Prologue)) &&
        KswordARKRuntimeReadMemory((const VOID*)(base + rva), observedPrologue, sizeof(observedPrologue)) &&
        RtlCompareMemory(observedPrologue, abi4Prologue, sizeof(abi4Prologue)) == sizeof(abi4Prologue)) {
        KSW_PSP_TERMINATE_PROCESS_FN terminate = (KSW_PSP_TERMINATE_PROCESS_FN)(base + rva);
        // NTSTATUS 返回值只表示请求结果，调用方仍等待目标对象真正退出。
        // 与 NtTerminateProcess 的外部目标调用一致：排除当前 IOCTL 线程，flags 为零。
        // 与原生包装入口相同，在临界区内执行私有终止，防止普通 Kernel APC 重入。
        KeEnterCriticalRegion();
        status = terminate(Process, PsGetCurrentThread(), ExitStatus, 0UL);
        // 外部目标调用应返回；对称恢复当前线程的 Kernel APC 计数。
        KeLeaveCriticalRegion();
    }
    // 成功和失败都释放临时 PE 视图。
    ExFreePoolWithTag(view, 'tPsK');
    return status;
#endif
}

NTSTATUS
KswordARKDriverTerminateProcessViaApc(_In_ PEPROCESS Process, _In_ NTSTATUS ExitStatus)
{
    // 所有排队都针对 caller 已引用并校验创建时间的精确对象。
    UNICODE_STRING name;
    KSW_NEXT_PROCESS_THREAD_FN next = NULL;
    PETHREAD cursor = NULL;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    ULONG visited = 0;
    // 系统线程仍使用原独立入口；不能把系统线程终止误当作普通进程 APC。
    if (Process == NULL || Process == PsInitialSystemProcess ||
        HandleToULong(PsGetProcessId(Process)) <= 4UL ||
        Process == PsGetCurrentProcess() || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    // 只解析枚举入口，不增加静态导入或借用用户态线程列表。
    RtlInitUnicodeString(&name, L"PsGetNextProcessThread");
    next = (KSW_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&name);
    // 旧系统未导出枚举入口时，通过有界 CID 查询获得真实引用，不猜 ETHREAD 地址。
    if (next == NULL) {
        ULONG threadId = 4UL;
        // 最多扫描 0x800000 个 CID 范围，时间预算为 500ms。
        ULONGLONG started = KeQueryInterruptTime();
        // 没有匹配线程与缺少 API 是不同状态。
        status = STATUS_NOT_FOUND;
        for (threadId = 4UL; threadId < 0x00800000UL; threadId += 4UL) {
            PETHREAD thread = NULL;
            // 每 256 项检查一次预算，避免缺少目标时阻塞请求线程。
            if ((threadId & 0x3ffUL) == 0 && KeQueryInterruptTime() - started >= 5000000ULL) {
                break;
            }
            // CID 查询返回已引用对象；失败项不影响后续候选。
            if (NT_SUCCESS(PsLookupThreadByThreadId(ULongToHandle(threadId), &thread))) {
                // 归属必须是上层已验证创建时间的精确 EPROCESS。
                if (PsGetThreadProcess(thread) == Process) {
                    status = KswordARKDriverQueueTerminateProcessApc(thread, Process, ExitStatus);
                }
                // 无论排队是否成功都归还 CID 查询的临时引用。
                ObDereferenceObject(thread);
                // 只排一个 APC；排队结果仍由上层等待实际退出。
                if (NT_SUCCESS(status)) {
                    return status;
                }
            }
        }
        return status;
    }
    // 一个目标最多排一个进程终止 APC；先尝试最多 4096 条可用线程。
    cursor = next(Process, NULL);
    while (cursor != NULL && visited < 4096UL) {
        // 排队函数在持有 ETHREAD 时再次核对 EPROCESS 归属。
        status = KswordARKDriverQueueTerminateProcessApc(cursor, Process, ExitStatus);
        // 任一线程成功排队后停止，不以排队成功直接宣布进程已退出。
        if (NT_SUCCESS(status)) {
            // 提前结束枚举时手动归还引用；APC 另持有自己的引用。
            ObDereferenceObject(cursor);
            return status;
        }
        // 继续枚举会消耗传入 cursor 的引用，不能再手动解引用旧游标。
        cursor = next(Process, cursor);
        // 记录本次已尝试线程，超过预算后释放尚未处理的返回游标。
        ++visited;
    }
    // 达到有界预算时归还尚未处理的下一条引用。
    if (cursor != NULL) {
        ObDereferenceObject(cursor);
    }
    return status;
}
