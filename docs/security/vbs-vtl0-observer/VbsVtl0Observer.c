/*
 * VBS architecture research tool -- read-only architectural observation.
 * 仅观察：不执行 LIDT，不复制或修改 IDT，不安装异常钩子，不触发页故障，
 * 不修改页保护、不注入代码，也不读取或修改 VMCS。
 * 两次 SIDT 只能报告调用方可见的 IDTR；相同读数不能证明不存在 VM-exit。
 * CPUID 的 hypervisor-present 位不能证明 VBS/HVCI 已启用或正在运行。
 * 本文件是独立的 x64 WDM 示例，不加入 KSword 产品驱动，不需要用户态组件。
 */

#include <ntddk.h> /* 使用公开 WDK 内核类型、IRQL、版本查询和调试输出接口。 */
#include <intrin.h> /* 使用 MSVC 的只读 SIDT 与 CPUID 编译器内建函数。 */

#if !defined(_M_AMD64) /* 本示例只描述 x64 的 10 字节 IDTR 表示。 */
#error This observer requires the x64 WDK target.
#endif

#pragma pack(push, 1) /* 按 SIDT 的硬件输出格式布局，禁止字段间填充。 */
typedef struct _VBS_OBSERVER_IDTR /* 保存 IDTR 的可见限长和基址，不解引用该基址。 */
{
    USHORT Limit; /* 保存 IDT 的字节限长。 */
    ULONG64 Base; /* 保存 IDT 的 64 位线性基址。 */
} VBS_OBSERVER_IDTR; /* 定义仅用于本次栈上观测的类型。 */
#pragma pack(pop) /* 恢复其余类型的默认对齐。 */

C_ASSERT(sizeof(VBS_OBSERVER_IDTR) == 10); /* 编译期检查 SIDT 输出缓冲区尺寸。 */

DRIVER_INITIALIZE DriverEntry; /* 声明系统加载入口并保留 WDK 的调用约定。 */
DRIVER_UNLOAD VbsObserverUnload; /* 声明无资源需要清理的卸载入口。 */

/* 用途：允许系统正常卸载；输入为驱动对象，无输出、无持久资源。 */
_Use_decl_annotations_
VOID VbsObserverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject); /* 明确该观察器没有设备或资源要撤销。 */
}

/*
 * 用途：在 DriverEntry 的 PASSIVE_LEVEL 上下文完成一次只读快照并输出结果。
 * 输入：系统提供的驱动对象和注册表路径；输出：快照的 NTSTATUS。
 * 仅在两次 SIDT 的短区间提升 IRQL，保证线程不迁移；所有输出在恢复后进行。
 * finally 只处理正常退出或可展开的异常，不保证从系统 bugcheck 中恢复。
 */
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    RTL_OSVERSIONINFOW version = { 0 }; /* 接收 Windows 主次版本及基础 build 号。 */
    NTSTATUS versionStatus; /* 保存版本查询状态，避免将失败解释为版本零。 */
    NTSTATUS sampleStatus = STATUS_SUCCESS; /* 保存 SIDT 快照或可捕获异常的状态。 */
    VBS_OBSERVER_IDTR original = { 0 }; /* 保存当前处理器的第一次 IDTR 读取。 */
    VBS_OBSERVER_IDTR repeated = { 0 }; /* 保存同一处理器的第二次 IDTR 读取。 */
    PROCESSOR_NUMBER processorBefore = { 0 }; /* 保存第一次 SIDT 前的处理器组和编号。 */
    PROCESSOR_NUMBER processorAfter = { 0 }; /* 保存第二次 SIDT 后的处理器组和编号。 */
    KIRQL previousIrql = PASSIVE_LEVEL; /* 保存短暂提升前的 IRQL，以便完整恢复。 */
    int processorInfo[4] = { 0 }; /* 接收 CPUID leaf 1 的 EAX、EBX、ECX、EDX。 */
    ULONG hypervisorPresent; /* 仅表示 CPUID 通告存在 hypervisor，不表示 VBS。 */
    BOOLEAN sameProcessor; /* 比较两个处理器身份字段，不使用含填充的结构比较。 */
    BOOLEAN sameIdtr; /* 比较两次可见 IDTR 的基址和限长。 */

    UNREFERENCED_PARAMETER(RegistryPath); /* 不读取或修改任何服务注册表配置。 */
    DriverObject->DriverUnload = VbsObserverUnload; /* 注册标准卸载回调，不安装异常钩子。 */
    version.dwOSVersionInfoSize = sizeof(version); /* 按公开接口要求填写结构尺寸。 */
    versionStatus = RtlGetVersion(&version); /* 只读查询当前 Windows 的基础版本。 */
    __cpuid(processorInfo, 1); /* 只读查询处理器及 hypervisor 通告的特征位。 */
    hypervisorPresent = ((ULONG)processorInfo[2] >> 31) & 1UL; /* 提取 hypervisor-present 位。 */

    __try /* 只记录 Windows 提供的可捕获异常，不宣称能够捕获 bugcheck。 */
    {
        KeRaiseIrql(DISPATCH_LEVEL, &previousIrql); /* 临时阻止线程调度和 CPU 迁移。 */
        __try /* 保证完成这段不分页、不分配、不打印的快照后恢复 IRQL。 */
        {
            (VOID)KeGetCurrentProcessorNumberEx(&processorBefore); /* 记录读取前的 CPU 身份。 */
            __sidt(&original); /* 将当前调用方可见的 IDTR 读到栈上。 */
            __sidt(&repeated); /* 不更改 IDTR，立即重复读取同一寄存器。 */
            (VOID)KeGetCurrentProcessorNumberEx(&processorAfter); /* 记录读取后的 CPU 身份。 */
        }
        __finally /* 正常执行和可展开异常均通过此处恢复进入前的优先级。 */
        {
            KeLowerIrql(previousIrql); /* 恢复 DriverEntry 原有 IRQL，再允许后续日志。 */
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) /* 仅在系统实际交付可捕获异常时记录状态。 */
    {
        sampleStatus = GetExceptionCode(); /* 保存异常码；不重试或更改控制寄存器。 */
    }

    sameProcessor = (processorBefore.Group == processorAfter.Group &&
        processorBefore.Number == processorAfter.Number); /* 检查重复读取的 CPU 身份。 */
    sameIdtr = (original.Base == repeated.Base &&
        original.Limit == repeated.Limit); /* 检查未更改 IDTR 时的同值预期。 */

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "[VbsObserver] OS_STATUS=0x%08lX OS=%lu.%lu build=%lu HYPERVISOR_PRESENT=%lu\n",
        (ULONG)versionStatus, version.dwMajorVersion, version.dwMinorVersion,
        version.dwBuildNumber, hypervisorPresent); /* 在恢复 IRQL 后输出环境快照。 */
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "[VbsObserver] SAMPLE_STATUS=0x%08lX CPU_BEFORE=%u:%u CPU_AFTER=%u:%u\n",
        (ULONG)sampleStatus, (unsigned int)processorBefore.Group,
        (unsigned int)processorBefore.Number, (unsigned int)processorAfter.Group,
        (unsigned int)processorAfter.Number); /* 明确打印采集状态和处理器组信息。 */

    if (NT_SUCCESS(sampleStatus)) /* 仅在完整采集成功后判断重复读数，不把零值当证据。 */
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[VbsObserver] ORIGINAL_IDT=%p LIMIT=0x%04X REPEAT_IDT=%p LIMIT=0x%04X\n",
            (PVOID)(ULONG_PTR)original.Base, (unsigned int)original.Limit,
            (PVOID)(ULONG_PTR)repeated.Base, (unsigned int)repeated.Limit); /* 输出两次可见 IDTR。 */
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[VbsObserver] EXPECT_SAME_CPU=1 MATCH=%u EXPECT_SAME_IDTR=1 MATCH=%u\n",
            (unsigned int)sameProcessor, (unsigned int)sameIdtr); /* 只检验重复快照的稳定性。 */
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "[VbsObserver] VBS=NOT_MEASURED HVCI=NOT_MEASURED LIDT=NOT_MEASURED "
        "PF_DELIVERY=NOT_MEASURED VMCS=NOT_MEASURED VM_EXITS=NOT_MEASURED\n"); /* 明确证据边界。 */
    return sampleStatus; /* 无资源需回滚；让系统获得真实的快照状态。 */
}
