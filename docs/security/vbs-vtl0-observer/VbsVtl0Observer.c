/*
 * VBS architecture research tool —— 测量 VTL0 下的 IDTR 写入与 #PF 交付路径。
 *
 * 本驱动分四个阶段，每个阶段均单处理器、单次、即测即还：
 *   Phase A：现场快照（OS 版本 / CPUID / CodeIntegrityOptions / SIDT 两次）。
 *   Phase B：在当前 CPU 上尝试一次 LIDT：把 IDT 拷贝到 NonPagedPoolNx，患换
 *            第 14 项（#PF）为计数桩，LIDT 后立即 SIDT 回读；只问 IDTR 是否生效。
 *   Phase C：仅当 Phase B 报告 LIDT 生效时触发一次已知的用户态非法读，看
 *            计数桩是否被交付（计数器是否被 lock inc 修改）；桩内链回原始处理器。
 *   Phase D：确认同一 CPU 的 IDTR 回到原值后才释放拷贝；未确认时保留资源，
 *            禁止正常卸载并成功返回以保持驱动驻留，随后恢复线程亲和性。
 *
 * 本驱动不修改页保护、不写入内核映像；正常恢复时 IDT 拷贝只短暂保留。
 * 恢复未确认时该 CPU 的副本、计数桩链回状态及驱动会继续驻留，不能正常卸载。
 * SIDT 变化可能来自 Hyper-V 的指令模拟，计数器增量可能来自异常重注入；不自称
 * 已经证伪或证实其中任何一条。
 *
 * 本文件是独立的 x64 WDM 示例，不加入 KSword 产品驱动，不需要用户态组件。
 */

#include <ntddk.h> /* 使用公开 WDK 内核类型、IRQL、版本查询和调试输出接口。 */
#include <ntstrsafe.h> /* 为报告拼接提供带长度的 printf 变体。 */
#include <intrin.h> /* 使用 MSVC 的 SIDT/LIDT 与 CPUID 编译器内建函数。 */

#define VBS_OBSERVER_SYSTEM_CODEINTEGRITY_INFORMATION 103UL /* WDK 未声明此查询类，使用微软公开的编号。 */
#define VBS_OBSERVER_HVCI_KMCI_ENABLED 0x400UL /* 用公开 CI 位解释回执，而不推断 VBS 运行状态。 */
#define VBS_OBSERVER_POOL_TAG 'OsbV' /* "VbsO" 小端 ASCII，便于 !poolused 排查残留。 */
#define VBS_OBSERVER_PF_VECTOR 14UL /* x64 的 #PF 向量号，固定为 14。 */
#define VBS_OBSERVER_IDT_ENTRY_SIZE 16UL /* x64 IDT 门描述符固定 16 字节。 */
#define VBS_OBSERVER_REPORT_CHARS 4096UL /* Registry 中保留 4K 字符存放完整报告。 */
#define VBS_OBSERVER_LINE_CHARS 256UL /* 单行上限；DbgPrintEx 和报告行共用。 */

/* 全局报告缓冲：Phase 中逐行累积，DriverEntry 末尾一次性写入 Registry。 */
static WCHAR g_VbsObserverReport[VBS_OBSERVER_REPORT_CHARS];
static ULONG g_VbsObserverReportLength = 0; /* 下一次写入的起始偏移（字符数）。 */

/* 用途：把一行结果同时写到 DbgPrintEx 和报告缓冲，便于内核调试器与 Registry 对账。 */
static VOID VbsObserverRecord(PCSTR Format, ...)
{
    va_list args;
    CHAR narrow[VBS_OBSERVER_LINE_CHARS];
    WCHAR wide[VBS_OBSERVER_LINE_CHARS];
    NTSTATUS cvt;
    ANSI_STRING asrc;
    UNICODE_STRING udst;
    size_t narrowLen = 0;
    size_t copy;

    va_start(args, Format);
    (VOID)RtlStringCbVPrintfA(narrow, sizeof(narrow), Format, args);
    va_end(args);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "%s", narrow);

    /* 把窄字符行转成宽字符，追加到报告缓冲；缓冲满后丢弃新内容但不越界。 */
    (VOID)RtlStringCbLengthA(narrow, sizeof(narrow), &narrowLen);
    RtlInitAnsiString(&asrc, narrow);
    udst.Buffer = wide;
    udst.MaximumLength = sizeof(wide);
    udst.Length = 0;
    cvt = RtlAnsiStringToUnicodeString(&udst, &asrc, FALSE);
    if (!NT_SUCCESS(cvt)) { return; }
    copy = udst.Length / sizeof(WCHAR);
    if (g_VbsObserverReportLength + copy + 1 >= VBS_OBSERVER_REPORT_CHARS) { return; }
    RtlCopyMemory(&g_VbsObserverReport[g_VbsObserverReportLength], wide, copy * sizeof(WCHAR));
    g_VbsObserverReportLength += (ULONG)copy;
    g_VbsObserverReport[g_VbsObserverReportLength] = L'\0';
    UNREFERENCED_PARAMETER(narrowLen);
}

/* 用途：把报告缓冲作为 REG_SZ 写到 HKLM\Software\VbsVtl0Observer\LastRun。 */
static VOID VbsObserverFlushToRegistry(VOID)
{
    UNICODE_STRING keyPath;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS st;
    UNICODE_STRING valueName;
    ULONG byteCount;

    RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\Software\\VbsVtl0Observer");
    InitializeObjectAttributes(&oa, &keyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateKey(&hKey, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(st)) { return; }

    RtlInitUnicodeString(&valueName, L"LastRun");
    byteCount = (g_VbsObserverReportLength + 1) * sizeof(WCHAR); /* 含结尾 NUL。 */
    (VOID)ZwSetValueKey(hKey, &valueName, 0, REG_SZ, g_VbsObserverReport, byteCount);
    (VOID)ZwClose(hKey);
}

typedef struct _VBS_OBSERVER_CODEINTEGRITY_INFORMATION
{
    ULONG Length; /* 内核按此字段验证查询缓冲区的完整长度。 */
    ULONG CodeIntegrityOptions; /* 仅在查询成功时解释返回的选项。 */
} VBS_OBSERVER_CODEINTEGRITY_INFORMATION;

NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    ); /* WDK 此处缺少公开原型，保持与仓库既有声明的 ABI 一致。 */

#if !defined(_M_AMD64) /* 本示例只描述 x64 的 10 字节 IDTR 表示。 */
#error This observer requires the x64 WDK target.
#endif

#pragma pack(push, 1) /* 按 SIDT 的硬件输出格式布局，禁止字段间填充。 */
typedef struct _VBS_OBSERVER_IDTR /* 保存 IDTR 的可见限长和基址，不解引用该基址。 */
{
    USHORT Limit; /* 保存 IDT 的字节限长。 */
    ULONG64 Base; /* 保存 IDT 的 64 位线性基址。 */
} VBS_OBSERVER_IDTR; /* 定义仅用于本次栈上观测的类型。 */

typedef struct _VBS_OBSERVER_IDT_ENTRY /* x64 门描述符按硬件布局解包。 */
{
    USHORT OffsetLow;   /* 处理器地址的低 16 位，CPU 用它拼回完整 RIP。 */
    USHORT Selector;    /* 处理器用来加载 CS，不在本阶段改动。 */
    UCHAR  IstReserved; /* 低 3 位为 IST 栈索引，高 5 位保留必须为零。 */
    UCHAR  TypeAttr;    /* 低 4 位 Type、位 4 零、位 5-6 DPL、位 7 Present。 */
    USHORT OffsetMid;   /* 处理器地址的 16-31 位。 */
    ULONG  OffsetHigh;  /* 处理器地址的 32-63 位。 */
    ULONG  Reserved;    /* 保留位，拷贝时逐字节保持原值。 */
} VBS_OBSERVER_IDT_ENTRY;
#pragma pack(pop) /* 恢复其余类型的默认对齐。 */

C_ASSERT(sizeof(VBS_OBSERVER_IDTR) == 10); /* 编译期检查 SIDT 输出缓冲区尺寸。 */
C_ASSERT(sizeof(VBS_OBSERVER_IDT_ENTRY) == VBS_OBSERVER_IDT_ENTRY_SIZE); /* 门描述符尺寸由硬件决定。 */

/*
 * 共享状态：计数桩原子增量 g_VbsPfCounter，并把控制转发给 g_VbsOriginalPfHandler。
 * 两者都在驱动的非分页 .data 节中；lock inc 要求地址非分页且可写，jmp [mem]
 * 要求内存可读。__declspec(align(16)) 让读/写不跨越缓存行，减少同一 Load/Store
 * 被拆成两次总线事务的可能性。
 */
__declspec(align(16)) volatile LONG64 g_VbsPfCounter = 0;
__declspec(align(16)) volatile PVOID g_VbsOriginalPfHandler = NULL;

/* 计数桩定义于 VbsVtl0Stub.asm；此处仅取其地址填入 IDT 项，不直接从 C 调用。 */
extern void VbsPfCountingStub(void);

DRIVER_INITIALIZE DriverEntry; /* 声明系统加载入口并保留 WDK 的调用约定。 */
DRIVER_UNLOAD VbsObserverUnload; /* 仅在 IDTR 已确认恢复、无保留资源时允许卸载。 */

/* 用途：正常恢复路径允许卸载；恢复未确认时 DriverUnload 会置空，不能进入此回调。 */
_Use_decl_annotations_
VOID VbsObserverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject); /* 明确该观察器没有设备或资源要撤销。 */
}

/* 用途：拼接门描述符的三段偏移，还原 64 位处理器地址，不解引用该地址。 */
static ULONG64 VbsReadIdtEntryHandler(const VBS_OBSERVER_IDT_ENTRY* Entry)
{
    return ((ULONG64)Entry->OffsetLow) |
           (((ULONG64)Entry->OffsetMid) << 16) |
           (((ULONG64)Entry->OffsetHigh) << 32);
}

/* 用途：把 64 位处理器地址分拆写回门描述符的三段偏移，保留其它字段原值。 */
static VOID VbsWriteIdtEntryHandler(VBS_OBSERVER_IDT_ENTRY* Entry, ULONG64 Address)
{
    Entry->OffsetLow  = (USHORT)(Address & 0xFFFFULL);
    Entry->OffsetMid  = (USHORT)((Address >> 16) & 0xFFFFULL);
    Entry->OffsetHigh = (ULONG)((Address >> 32) & 0xFFFFFFFFULL);
}

/*
 * 用途：在 DriverEntry 的 PASSIVE_LEVEL 上下文完成四阶段测量并输出结果。
 * 输入：系统提供的驱动对象和注册表路径；输出：Phase A 快照的 NTSTATUS。
 * Phase B/C/D 的失败不改变返回值（否则 sc start 会把观察数据当成错误丢弃）。
 * 所有涉及 LIDT/SIDT 的区间都在 DISPATCH_LEVEL 下执行，且已通过亲和性
 * 固定到同一 CPU；#PF 触发在 PASSIVE_LEVEL 下执行以便 __except 正常展开。
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
    int hypervisorInfo[4] = { 0 }; /* 仅在处理器报告存在 hypervisor 后查询 vendor leaf。 */
    CHAR hypervisorVendor[13] = { 0 }; /* 固定 12 字节签名后补 NUL，避免无界打印。 */
    VBS_OBSERVER_CODEINTEGRITY_INFORMATION ciInformation = { 0 }; /* 失败时不把未测值解释为零。 */
    NTSTATUS ciStatus; /* 单独保留查询失败，不影响 SIDT 观察。 */
    ULONG hypervisorPresent; /* 仅表示 CPUID 通告存在 hypervisor，不表示 VBS。 */
    BOOLEAN sameProcessor; /* 比较两个处理器身份字段，不使用含填充的结构比较。 */
    BOOLEAN sameIdtr; /* 比较两次可见 IDTR 的基址和限长。 */
    BOOLEAN phaseAComplete; /* Phase A 任一子查询或连续快照不一致都降级为 PARTIAL。 */
    PROCESSOR_NUMBER currentProc = { 0 }; /* 选定处理器后，整个采样与恢复过程保持该亲和性。 */
    GROUP_AFFINITY targetAffinity = { 0 }; /* 原始 IDTR 必须在固定到目标 CPU 之后读取。 */
    GROUP_AFFINITY savedAffinity = { 0 }; /* 每条结束路径都恢复调用线程原有亲和性。 */

    UNREFERENCED_PARAMETER(RegistryPath); /* 不读取或修改任何服务注册表配置。 */
    DriverObject->DriverUnload = VbsObserverUnload; /* 正常恢复后允许卸载；未确认恢复时撤销此入口。 */
    version.dwOSVersionInfoSize = sizeof(version); /* 按公开接口要求填写结构尺寸。 */
    versionStatus = RtlGetVersion(&version); /* 只读查询当前 Windows 的基础版本。 */
    __cpuid(processorInfo, 1); /* 只读查询处理器及 hypervisor 通告的特征位。 */
    hypervisorPresent = ((ULONG)processorInfo[2] >> 31) & 1UL; /* 提取 hypervisor-present 位。 */
    if (hypervisorPresent != 0UL) /* 无 hypervisor 时不把保留 leaf 的输出误标为供应商。 */
    {
        __cpuid(hypervisorInfo, 0x40000000); /* 仅将供应商签名用于现场诊断。 */
        RtlCopyMemory(&hypervisorVendor[0], &hypervisorInfo[1], sizeof(ULONG)); /* EBX 是签名前四字节。 */
        RtlCopyMemory(&hypervisorVendor[4], &hypervisorInfo[2], sizeof(ULONG)); /* ECX 紧接 EBX。 */
        RtlCopyMemory(&hypervisorVendor[8], &hypervisorInfo[3], sizeof(ULONG)); /* EDX 填充最后四字节。 */
    }
    ciInformation.Length = sizeof(ciInformation); /* 查询要求调用方提供结构大小。 */
    ciStatus = ZwQuerySystemInformation(
        VBS_OBSERVER_SYSTEM_CODEINTEGRITY_INFORMATION,
        &ciInformation,
        sizeof(ciInformation),
        NULL); /* 在 PASSIVE_LEVEL 查询，不把失败当作 HVCI 关闭。 */

    /* 先固定 CPU 再采集原始 IDTR；日志与 PASSIVE_LEVEL 缺页期间也不迁移。 */
    (VOID)KeGetCurrentProcessorNumberEx(&currentProc); /* 此时只选择目标，不采集恢复依据。 */
    targetAffinity.Group = currentProc.Group; /* 显式使用处理器组，避免跨组 CPU 编号混淆。 */
    targetAffinity.Mask = ((KAFFINITY)1) << currentProc.Number; /* 单 CPU 亲和性保持至清理结束。 */
    KeSetSystemGroupAffinityThread(&targetAffinity, &savedAffinity); /* 后续 SIDT 与 LIDT 都在此 CPU。 */

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
    phaseAComplete = NT_SUCCESS(versionStatus) && NT_SUCCESS(ciStatus) &&
        NT_SUCCESS(sampleStatus) && sameProcessor && sameIdtr; /* 快照一致性同样属于 Phase A 结果。 */

    VbsObserverRecord(
        "[VbsObserver] OS_STATUS=0x%08lX OS=%lu.%lu build=%lu HYPERVISOR_PRESENT=%lu\n",
        (ULONG)versionStatus, version.dwMajorVersion, version.dwMinorVersion,
        version.dwBuildNumber, hypervisorPresent); /* 在恢复 IRQL 后输出环境快照。 */
    if (hypervisorPresent != 0UL) /* 供应商字符串只是 CPUID 自报值。 */
    {
        VbsObserverRecord(
            "[VbsObserver] HYPERVISOR_VENDOR=%s MAX_LEAF=0x%08X\n",
            hypervisorVendor, (unsigned int)hypervisorInfo[0]); /* 明确来源于 leaf 0x40000000。 */
    }
    else /* 没有相应的自报字段时保留未知状态。 */
    {
        VbsObserverRecord(
            "[VbsObserver] HYPERVISOR_VENDOR=NOT_MEASURED\n"); /* 不编造供应商身份。 */
    }
    if (NT_SUCCESS(ciStatus)) /* 只在成功回执下解释 0x400。 */
    {
        VbsObserverRecord(
            "[VbsObserver] CI_QUERY_STATUS=0x%08lX CODE_INTEGRITY_OPTIONS=0x%08lX HVCI_KMCI_FLAG=%u\n",
            (ULONG)ciStatus, ciInformation.CodeIntegrityOptions,
            (unsigned int)((ciInformation.CodeIntegrityOptions & VBS_OBSERVER_HVCI_KMCI_ENABLED) != 0UL)); /* 保留原始掩码供复核。 */
    }
    else /* 查询失败仍继续剩余只读观察。 */
    {
        VbsObserverRecord(
            "[VbsObserver] CI_QUERY_STATUS=0x%08lX CODE_INTEGRITY_OPTIONS=NOT_MEASURED HVCI_KMCI_FLAG=NOT_MEASURED\n",
            (ULONG)ciStatus); /* 保留错误码，而不把失败折叠成假阴性。 */
    }
    VbsObserverRecord(
        "[VbsObserver] SAMPLE_STATUS=0x%08lX CPU_BEFORE=%u:%u CPU_AFTER=%u:%u\n",
        (ULONG)sampleStatus, (unsigned int)processorBefore.Group,
        (unsigned int)processorBefore.Number, (unsigned int)processorAfter.Group,
        (unsigned int)processorAfter.Number); /* 明确打印采集状态和处理器组信息。 */

    if (NT_SUCCESS(sampleStatus)) /* 仅在完整采集成功后判断重复读数，不把零值当证据。 */
    {
        VbsObserverRecord(
            "[VbsObserver] ORIGINAL_IDT=%p LIMIT=0x%04X REPEAT_IDT=%p LIMIT=0x%04X\n",
            (PVOID)(ULONG_PTR)original.Base, (unsigned int)original.Limit,
            (PVOID)(ULONG_PTR)repeated.Base, (unsigned int)repeated.Limit); /* 输出两次可见 IDTR。 */
        VbsObserverRecord(
            "[VbsObserver] EXPECT_SAME_CPU=1 MATCH=%u EXPECT_SAME_IDTR=1 MATCH=%u\n",
            (unsigned int)sameProcessor, (unsigned int)sameIdtr); /* 只检验重复快照的稳定性。 */
    }

    VbsObserverRecord(
        "[VbsObserver] PHASE_A=%s\n",
        phaseAComplete ? "SNAPSHOT_COMPLETE" : "PARTIAL"); /* 任一查询失败都不宣称完整。 */

    /* -------------------------------------------------------------------- *
     * Phase B：在固定的当前 CPU 上尝试 LIDT，立即 SIDT 回读判断是否生效。
     * 只有 Phase A 的 SIDT 本身成功才有参考基址可以回退；否则跳过 B/C/D。
     * -------------------------------------------------------------------- */
    if (!NT_SUCCESS(sampleStatus))
    {
        VbsObserverRecord(
            "[VbsObserver] PHASE_B=NOT_TESTED PHASE_C=NOT_TESTED PHASE_D=NOT_TESTED\n");
        VbsObserverRecord(
            "[VbsObserver] === INCOMPLETE ===\n");
        KeRevertToUserGroupAffinityThread(&savedAffinity); /* 只读采样失败时尚未改动 IDTR。 */
        VbsObserverFlushToRegistry();
        return sampleStatus;
    }

    /*
     * 本阶段的本地变量集中声明：MSVC 允许混合声明，但集中声明让 __try 的
     * 变量作用域更清晰，便于 Phase D 的清理用同一组句柄。
     */
    PVOID idtCopy = NULL; /* NonPagedPoolNx 分配的 IDT 副本首地址。 */
    SIZE_T idtBytes = (SIZE_T)original.Limit + 1; /* IDTR.Limit 的语义是"最后一个合法字节的偏移"。 */
    VBS_OBSERVER_IDTR postLidt = { 0 }; /* LIDT 之后的 SIDT 回读。 */
    VBS_OBSERVER_IDTR postRestore = { 0 }; /* 还原后的 SIDT 回读。 */
    NTSTATUS lidtStatus = STATUS_SUCCESS; /* LIDT 自身是否抛出可捕获异常。 */
    NTSTATUS restoreStatus = STATUS_SUCCESS; /* 还原时是否抛出可捕获异常。 */
    NTSTATUS pfStatus = STATUS_UNSUCCESSFUL; /* Phase C 读取非法地址的异常码。 */
    BOOLEAN lidtInstalled = FALSE; /* LIDT 后 SIDT 回读的新基址是否等于我们指定的基址。 */
    BOOLEAN lidtAttempted = FALSE; /* 在执行 LIDT 前设置，异常后仍必须确认当前 IDTR。 */
    BOOLEAN restoreConfirmed = FALSE; /* 唯有原 IDTR 的完整回读匹配才允许释放共享状态。 */
    LONG64 counterBeforeFault = 0; /* 只比较受控读取前后的计数，排除安装及日志期间的旧增量。 */
    LONG64 counterSnapshot = 0; /* Phase C 结束后原子读到的计数器值。 */
    ULONG64 originalPfHandlerAddress = 0; /* 从 IDT 第 14 项提取的原始处理器地址，仅用于日志。 */
    VBS_OBSERVER_IDT_ENTRY* pfEntry = NULL; /* IDT 副本中的第 14 项，便于两次写入。 */

    idtCopy = ExAllocatePool2(POOL_FLAG_NON_PAGED, idtBytes, VBS_OBSERVER_POOL_TAG);
    if (idtCopy == NULL)
    {
        VbsObserverRecord(
            "[VbsObserver] PHASE_B=ALLOC_FAILED idt_bytes=%Iu\n", idtBytes);
        VbsObserverRecord(
            "[VbsObserver] PHASE_C=NOT_TESTED PHASE_D=NOT_TESTED\n");
        KeRevertToUserGroupAffinityThread(&savedAffinity);
        VbsObserverRecord(
            "[VbsObserver] === INCOMPLETE ===\n");
        VbsObserverFlushToRegistry();
        return sampleStatus;
    }

    /*
     * 从原 IDT 拷贝 Limit+1 字节到 NonPagedPoolNx；IDT 由硬件以数据方式读取，
     * 不需要可执行；修改 pfEntry 后基本结构仍与原 IDT 等价，所有未修改向量
     * 继续走原 Windows 处理器。
     */
    RtlCopyMemory(idtCopy, (PVOID)(ULONG_PTR)original.Base, idtBytes);
    pfEntry = (VBS_OBSERVER_IDT_ENTRY*)((UCHAR*)idtCopy + VBS_OBSERVER_PF_VECTOR * VBS_OBSERVER_IDT_ENTRY_SIZE);
    originalPfHandlerAddress = VbsReadIdtEntryHandler(pfEntry);

    /*
     * 预先清零计数器并把原处理器地址发布到全局；这两处写入必须在 LIDT 之前
     * 完成且 lock 之前的 store 要对其它逻辑处理器可见，否则一旦 LIDT 立刻
     * 生效，桩的 jmp [g_VbsOriginalPfHandler] 可能读到 NULL 跳进 0 地址。
     */
    (VOID)InterlockedExchange64(&g_VbsPfCounter, 0);
    (VOID)InterlockedExchangePointer((PVOID volatile*)&g_VbsOriginalPfHandler,
        (PVOID)(ULONG_PTR)originalPfHandlerAddress);

    /*
     * 把 IDT 副本第 14 项的处理器地址改到计数桩，Selector/IST/TypeAttr/Reserved
     * 都从原项继承。只有 Offset 三段被改写。
     */
    VbsWriteIdtEntryHandler(pfEntry, (ULONG64)(ULONG_PTR)&VbsPfCountingStub);

    VBS_OBSERVER_IDTR newIdtr;
    newIdtr.Limit = original.Limit; /* 使用相同 Limit，意味着向量数量与原 IDT 一致。 */
    newIdtr.Base = (ULONG64)(ULONG_PTR)idtCopy;

    __try
    {
        KeRaiseIrql(DISPATCH_LEVEL, &previousIrql); /* 阻止调度和 DPC 排队中间插入。 */
        __try
        {
            lidtAttempted = TRUE; /* LIDT 或其后的 SIDT 抛异常时也不能跳过恢复确认。 */
            __lidt(&newIdtr); /* 期望改变 IDTR；若 Hyper-V 拒绝则此处抛异常或静默吞下。 */
            __sidt(&postLidt); /* 立即回读 IDTR；基址未变即视为"被静默吞下"。 */
        }
        __finally
        {
            KeLowerIrql(previousIrql);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        lidtStatus = GetExceptionCode();
    }

    lidtInstalled = NT_SUCCESS(lidtStatus) &&
                    lidtAttempted &&
                    postLidt.Base == newIdtr.Base &&
                    postLidt.Limit == newIdtr.Limit;

    if (!NT_SUCCESS(lidtStatus))
    {
        VbsObserverRecord(
            "[VbsObserver] PHASE_B=LIDT_EXCEPTION status=0x%08lX requested_base=%p\n",
            (ULONG)lidtStatus, (PVOID)(ULONG_PTR)newIdtr.Base);
    }
    else if (lidtInstalled)
    {
        VbsObserverRecord(
            "[VbsObserver] PHASE_B=LIDT_SUCCEEDED new_base=%p new_limit=0x%04X original_pf_handler=%p stub=%p\n",
            (PVOID)(ULONG_PTR)postLidt.Base, (unsigned int)postLidt.Limit,
            (PVOID)(ULONG_PTR)originalPfHandlerAddress,
            (PVOID)(ULONG_PTR)&VbsPfCountingStub);
        VbsObserverRecord(
            "[VbsObserver] PHASE_B_NOTE=SIDT_change_may_reflect_hypervisor_emulation\n");
    }
    else
    {
        VbsObserverRecord(
            "[VbsObserver] PHASE_B=LIDT_SILENTLY_DROPPED requested_base=%p observed_base=%p observed_limit=0x%04X\n",
            (PVOID)(ULONG_PTR)newIdtr.Base,
            (PVOID)(ULONG_PTR)postLidt.Base, (unsigned int)postLidt.Limit);
    }

    /* -------------------------------------------------------------------- *
     * Phase C：只有 LIDT 真正改变了 IDTR 基址才有必要触发 #PF；否则跳过并
     * 直接进入 Phase D 的还原。即便桩没装上，Phase D 的 LIDT 回退也要跑，
     * 以保证我们不留下任何被改过的 IDTR。
     * -------------------------------------------------------------------- */
    if (lidtInstalled)
    {
        counterBeforeFault = InterlockedCompareExchange64(&g_VbsPfCounter, 0, 0); /* 紧邻受控读取取得基线。 */
        /*
         * 读取用户态非法地址（null 页里的任意位置）从内核态会 #PF；KiPageFault
         * 会检查到用户地址被内核读、CPL=0、有 __try 帧，再派发为访问冲突。
         * 这是公开的 Windows 行为，不是未定义行为。
         */
        __try
        {
            volatile UCHAR dummy = *(volatile UCHAR*)(ULONG_PTR)1;
            UNREFERENCED_PARAMETER(dummy);
            pfStatus = STATUS_SUCCESS; /* 若没有抛异常，这一行会设置为"没抛"信号。 */
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            pfStatus = GetExceptionCode();
        }

        counterSnapshot = InterlockedCompareExchange64(&g_VbsPfCounter, 0, 0);

        if (counterSnapshot > counterBeforeFault)
        {
            VbsObserverRecord(
                "[VbsObserver] PHASE_C=PF_COUNTER_INCREMENTED before=%I64d after=%I64d pf_status=0x%08lX\n",
                counterBeforeFault, counterSnapshot, (ULONG)pfStatus);
            VbsObserverRecord(
                "[VbsObserver] PHASE_C_NOTE=increment_may_follow_hypervisor_reinjection\n");
        }
        else if (pfStatus == STATUS_ACCESS_VIOLATION)
        {
            VbsObserverRecord(
                "[VbsObserver] PHASE_C=PF_COUNTER_ZERO pf_status=0x%08lX\n", (ULONG)pfStatus);
            VbsObserverRecord(
                "[VbsObserver] PHASE_C_NOTE=exception_dispatched_without_stub_increment\n");
        }
        else
        {
            VbsObserverRecord(
                "[VbsObserver] PHASE_C=PF_NOT_CAUGHT pf_status=0x%08lX counter=%I64d\n",
                (ULONG)pfStatus, counterSnapshot);
        }
    }
    else
    {
        VbsObserverRecord(
            "[VbsObserver] PHASE_C=NOT_TESTED reason=lidt_not_installed\n");
    }

    /* -------------------------------------------------------------------- *
     * Phase D：任何 LIDT 尝试后都独立检查原 IDTR；即使安装后的 SIDT 失败
     * 也不能凭 lidtInstalled=false 释放仍可能在用的副本。只在确认恢复后释放。
     * -------------------------------------------------------------------- */
    if (lidtAttempted)
    {
        __try
        {
            KeRaiseIrql(DISPATCH_LEVEL, &previousIrql);
            __try
            {
                __sidt(&postRestore); /* 安装被拒绝时原 IDTR 可能已在位，不必再次写入。 */
                if (postRestore.Base != original.Base || postRestore.Limit != original.Limit) /* 仅实际偏离时回退。 */
                {
                    __lidt(&original); /* 同一固定 CPU 上恢复其原始 IDTR。 */
                    __sidt(&postRestore); /* 不能用 LIDT 未抛异常替代硬件可见回读。 */
                }
                restoreConfirmed = postRestore.Base == original.Base &&
                    postRestore.Limit == original.Limit; /* 基址和限长必须同时匹配。 */
            }
            __finally
            {
                KeLowerIrql(previousIrql);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            restoreStatus = GetExceptionCode();
        }

        if (!NT_SUCCESS(restoreStatus))
        {
            VbsObserverRecord(
                "[VbsObserver] PHASE_D=RESTORE_EXCEPTION status=0x%08lX\n", (ULONG)restoreStatus);
        }
        else if (restoreConfirmed)
        {
            VbsObserverRecord(
                "[VbsObserver] PHASE_D=RESTORED base=%p limit=0x%04X\n",
                (PVOID)(ULONG_PTR)postRestore.Base, (unsigned int)postRestore.Limit);
        }
        else
        {
            VbsObserverRecord(
                "[VbsObserver] PHASE_D=RESTORE_DIVERGED expected_base=%p observed_base=%p\n",
                (PVOID)(ULONG_PTR)original.Base, (PVOID)(ULONG_PTR)postRestore.Base);
        }
    }
    else
    {
        restoreConfirmed = TRUE; /* 没有尝试 LIDT，不会留下指向副本的 IDTR。 */
        VbsObserverRecord(
            "[VbsObserver] PHASE_D=SKIPPED reason=lidt_not_attempted\n");
    }

    if (restoreConfirmed) /* 同一 CPU 已不再引用副本，才可以撤销桩的共享状态。 */
    {
        (VOID)InterlockedExchangePointer((PVOID volatile*)&g_VbsOriginalPfHandler, NULL); /* 不再有新 #PF 进入此桩。 */
        ExFreePool(idtCopy); /* 所有可达 IDTR 均已回到系统原表。 */
    }
    else /* 无法证明副本已脱离 CPU，宁可保留有效表和链回指针。 */
    {
        DriverObject->DriverUnload = NULL; /* 禁止正常卸载，避免活跃 IDT 继续引用已卸载驱动代码。 */
        VbsObserverRecord(
            "[VbsObserver] RESOURCES_RETAINED=1 UNLOAD_DISABLED=1 reason=idtr_restore_unconfirmed\n");
    }
    KeRevertToUserGroupAffinityThread(&savedAffinity); /* 表/代码生命周期已有保证，再恢复线程亲和性。 */

    VbsObserverRecord(
        "[VbsObserver] === %s ===\n",
        phaseAComplete && NT_SUCCESS(lidtStatus) && lidtInstalled &&
        pfStatus == STATUS_ACCESS_VIOLATION && counterSnapshot > counterBeforeFault &&
        NT_SUCCESS(restoreStatus) && restoreConfirmed
        ? "COMPLETE" : "INCOMPLETE"); /* 未安装、无受控缺页或恢复未确认均不能判完成。 */

    VbsObserverFlushToRegistry(); /* 把完整报告落盘到 HKLM\\Software\\VbsVtl0Observer\\LastRun。 */
    return STATUS_SUCCESS; /* 主动路径失败只影响报告；保留资源时必须让映像继续驻留。 */
}
