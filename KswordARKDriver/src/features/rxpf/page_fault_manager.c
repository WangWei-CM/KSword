/*++

Module Name:

    page_fault_manager.c

Abstract:

    Per-processor shadow-IDT lifecycle and nonpaged vector-14 dispatcher.

Environment:

    Installation runs at PASSIVE_LEVEL.  The dispatcher runs on the exception
    path without allocation, pageable access, blocking locks, or formatted I/O.

--*/

#include "page_fault_manager.h"
#include "rxpf_diagnostics.h"
#include "src/platform/pool_compat.h"

#define KSW_RXPF_IDT_TAG 'iPxR'
#define KSW_RXPF_IDT_VECTOR_COUNT 256UL
#define KSW_RXPF_IDT_ENTRY_BYTES 16UL
#define KSW_RXPF_PAGE_FAULT_VECTOR 14UL

#pragma pack(push, 1)
typedef struct _KSW_RXPF_IDTR
{
    USHORT Limit;
    ULONG64 Base;
} KSW_RXPF_IDTR, *PKSW_RXPF_IDTR;

typedef struct _KSW_RXPF_IDT_ENTRY
{
    USHORT OffsetLow;
    USHORT Selector;
    UCHAR Ist;
    UCHAR TypeAttributes;
    USHORT OffsetMiddle;
    ULONG OffsetHigh;
    ULONG Reserved;
} KSW_RXPF_IDT_ENTRY, *PKSW_RXPF_IDT_ENTRY;
#pragma pack(pop)

typedef struct _KSW_RXPF_CPU_IDT_STATE
{
    KSW_RXPF_IDTR OriginalIdtr;
    KSW_RXPF_IDTR ShadowIdtr;
    KSW_RXPF_IDT_ENTRY OriginalPageFaultEntry;
    PVOID OriginalPageFaultHandler;
    PVOID ShadowTable;
    PROCESSOR_NUMBER ProcessorNumber;
    ULONG ProcessorIndex;
    volatile LONG Active;
    volatile LONG Installed;
    volatile LONG HandlerDepth;
    volatile LONG LastStatus;
} KSW_RXPF_CPU_IDT_STATE, *PKSW_RXPF_CPU_IDT_STATE;

typedef struct _KSW_RXPF_PAGE_FAULT_STATE
{
    PKSW_RXPF_PAGE_TABLE PageTable;
    PKSW_RXPF_CPU_IDT_STATE Cpus;
    ULONG CpuCapacity;
    volatile LONG CpuCount;
    PVOID ProcessorChangeHandle;
    PVOID EmergencyOriginalHandler;
    EX_PUSH_LOCK ControlLock;
    volatile LONG Initialized;
    volatile LONG Installing;
    volatile LONG Installed;
    volatile LONG LastStatus;
} KSW_RXPF_PAGE_FAULT_STATE;

static KSW_RXPF_PAGE_FAULT_STATE g_KswRxpfPageFault;

C_ASSERT(sizeof(KSW_RXPF_IDTR) == 10U);
C_ASSERT(sizeof(KSW_RXPF_IDT_ENTRY) == 16U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, R15) == 0U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, R11) == 32U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, Rax) == 112U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, ErrorCode) == 120U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, Rip) == 128U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, Cs) == 136U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, Rflags) == 144U);
C_ASSERT(FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, HardwareRsp) == 152U);
C_ASSERT(sizeof(KSW_RXPF_TRAP_FRAME) == 168U);

static ULONGLONG
KswRxpfIdtHandlerAddress(
    _In_ const KSW_RXPF_IDT_ENTRY* Entry
    )
{
    ULONGLONG address = 0ULL;

    /* Combine the architectural offset fragments without unaligned loads. */
    address = (ULONGLONG)Entry->OffsetLow;
    address |= ((ULONGLONG)Entry->OffsetMiddle) << 16;
    address |= ((ULONGLONG)Entry->OffsetHigh) << 32;
    return address;
}

static VOID
KswRxpfIdtSetHandlerAddress(
    _Inout_ KSW_RXPF_IDT_ENTRY* Entry,
    _In_ ULONGLONG HandlerAddress
    )
{
    ULONGLONG address = HandlerAddress;

    /* Change only the three handler-offset fields in the copied descriptor. */
    Entry->OffsetLow = (USHORT)(address & 0xFFFFULL);
    Entry->OffsetMiddle = (USHORT)((address >> 16) & 0xFFFFULL);
    Entry->OffsetHigh = (ULONG)(address >> 32);
}

static BOOLEAN
KswRxpfCanonicalKernelAddress(
    _In_ ULONGLONG Address
    )
{
    /* All installed handler targets must be canonical kernel addresses. */
    return (Address >> 48) == 0xFFFFULL &&
        Address >= (ULONGLONG)(ULONG_PTR)MmSystemRangeStart;
}

static VOID
KswRxpfProcessorChangeCallback(
    _In_ PVOID CallbackContext,
    _In_ PKE_PROCESSOR_CHANGE_NOTIFY_CONTEXT ChangeContext,
    _Inout_ PNTSTATUS OperationStatus
    )
{
    UNREFERENCED_PARAMETER(CallbackContext);

    /* Deny dynamic processor addition while capture/install/restore is active. */
    if (ChangeContext != NULL && OperationStatus != NULL &&
        ChangeContext->State == KeProcessorAddStartNotify &&
        (InterlockedCompareExchange(
            &g_KswRxpfPageFault.Installing,
            1,
            1) != 0 ||
         InterlockedCompareExchange(
            &g_KswRxpfPageFault.Installed,
            1,
            1) != 0)) {
        *OperationStatus = STATUS_DEVICE_BUSY;
    }
}

static VOID
KswRxpfFreeShadowTables(
    VOID
    )
{
    ULONG index = 0UL;

    /* Rows are unreachable from vector 14 before this control-path cleanup. */
    for (index = 0UL; index < g_KswRxpfPageFault.CpuCapacity; ++index) {
        PKSW_RXPF_CPU_IDT_STATE cpu = &g_KswRxpfPageFault.Cpus[index];
        PVOID shadow = cpu->ShadowTable;

        RtlZeroMemory(cpu, sizeof(*cpu));
        if (shadow != NULL) {
            ExFreePoolWithTag(shadow, KSW_RXPF_IDT_TAG);
        }
    }
    InterlockedExchange(&g_KswRxpfPageFault.CpuCount, 0);
    g_KswRxpfPageFault.EmergencyOriginalHandler = NULL;
}

static NTSTATUS
KswRxpfCaptureShadowTables(
    VOID
    )
{
    ULONG activeCount =
        KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    ULONG globalIndex = 0UL;

    /* The preallocated row array must cover the complete stable topology. */
    if (activeCount == 0UL ||
        activeCount > g_KswRxpfPageFault.CpuCapacity) {
        return STATUS_NOT_SUPPORTED;
    }
    KswRxpfFreeShadowTables();

    /* Capture and copy each processor's own IDT while pinned to that CPU. */
    for (globalIndex = 0UL; globalIndex < activeCount; ++globalIndex) {
        PROCESSOR_NUMBER processor;
        GROUP_AFFINITY targetAffinity;
        GROUP_AFFINITY previousAffinity;
        KSW_RXPF_IDTR idtr;
        PKSW_RXPF_CPU_IDT_STATE cpu =
            &g_KswRxpfPageFault.Cpus[globalIndex];
        SIZE_T tableBytes = 0U;
        PKSW_RXPF_IDT_ENTRY shadowEntries = NULL;
        KSW_RXPF_IDT_ENTRY pageFaultEntry;
        ULONGLONG originalHandler = 0ULL;
        NTSTATUS status = STATUS_SUCCESS;

        RtlZeroMemory(&processor, sizeof(processor));
        status = KeGetProcessorNumberFromIndex(globalIndex, &processor);
        if (!NT_SUCCESS(status) ||
            processor.Number >= sizeof(KAFFINITY) * 8UL) {
            KswRxpfFreeShadowTables();
            return STATUS_NOT_SUPPORTED;
        }
        cpu->ShadowTable = KswordARKAllocateNonPagedPool(
            KSW_RXPF_IDT_VECTOR_COUNT * KSW_RXPF_IDT_ENTRY_BYTES,
            KSW_RXPF_IDT_TAG);
        if (cpu->ShadowTable == NULL) {
            KswRxpfFreeShadowTables();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(
            cpu->ShadowTable,
            KSW_RXPF_IDT_VECTOR_COUNT * KSW_RXPF_IDT_ENTRY_BYTES);

        RtlZeroMemory(&targetAffinity, sizeof(targetAffinity));
        RtlZeroMemory(&previousAffinity, sizeof(previousAffinity));
        targetAffinity.Group = processor.Group;
        targetAffinity.Mask = ((KAFFINITY)1) << processor.Number;
        KeSetSystemGroupAffinityThread(
            &targetAffinity,
            &previousAffinity);
        RtlZeroMemory(&idtr, sizeof(idtr));
        __sidt(&idtr);
        tableBytes = (SIZE_T)idtr.Limit + 1U;
        if (idtr.Base == 0ULL ||
            tableBytes < (KSW_RXPF_PAGE_FAULT_VECTOR + 1UL) *
                KSW_RXPF_IDT_ENTRY_BYTES ||
            tableBytes > KSW_RXPF_IDT_VECTOR_COUNT *
                KSW_RXPF_IDT_ENTRY_BYTES) {
            KeRevertToUserGroupAffinityThread(&previousAffinity);
            KswRxpfFreeShadowTables();
            return STATUS_NOT_SUPPORTED;
        }
        __try {
            RtlCopyMemory(
                cpu->ShadowTable,
                (const VOID*)(ULONG_PTR)idtr.Base,
                tableBytes);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
        }
        KeRevertToUserGroupAffinityThread(&previousAffinity);
        if (!NT_SUCCESS(status)) {
            KswRxpfFreeShadowTables();
            return status;
        }

        shadowEntries =
            (PKSW_RXPF_IDT_ENTRY)cpu->ShadowTable;
        RtlCopyMemory(
            &pageFaultEntry,
            &shadowEntries[KSW_RXPF_PAGE_FAULT_VECTOR],
            sizeof(pageFaultEntry));
        originalHandler = KswRxpfIdtHandlerAddress(&pageFaultEntry);

        /* IF must stay masked until the handler joins its lifecycle read side. */
        if ((pageFaultEntry.TypeAttributes & 0x80U) == 0U ||
            (pageFaultEntry.TypeAttributes & 0x0FU) != 0x0EU ||
            (pageFaultEntry.Ist & 0x07U) != 0U ||
            pageFaultEntry.Selector == 0U ||
            pageFaultEntry.Reserved != 0UL ||
            !KswRxpfCanonicalKernelAddress(originalHandler)) {
            KswRxpfFreeShadowTables();
            return STATUS_NOT_SUPPORTED;
        }

        /* Preserve every gate attribute and replace only the handler offset. */
        KswRxpfIdtSetHandlerAddress(
            &shadowEntries[KSW_RXPF_PAGE_FAULT_VECTOR],
            (ULONGLONG)(ULONG_PTR)KswRxpfPageFaultStub);
        cpu->OriginalIdtr = idtr;
        cpu->ShadowIdtr.Limit = idtr.Limit;
        cpu->ShadowIdtr.Base =
            (ULONG64)(ULONG_PTR)cpu->ShadowTable;
        cpu->OriginalPageFaultEntry = pageFaultEntry;
        cpu->OriginalPageFaultHandler =
            (PVOID)(ULONG_PTR)originalHandler;
        cpu->ProcessorNumber = processor;
        cpu->ProcessorIndex = globalIndex;
        cpu->LastStatus = STATUS_SUCCESS;
        KeMemoryBarrier();
        InterlockedExchange(&cpu->Active, 1);
        if (g_KswRxpfPageFault.EmergencyOriginalHandler == NULL) {
            g_KswRxpfPageFault.EmergencyOriginalHandler =
                cpu->OriginalPageFaultHandler;
        }
    }

    /* Reject a topology change that raced the capture window. */
    if (KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) != activeCount) {
        KswRxpfFreeShadowTables();
        return STATUS_RETRY;
    }
    InterlockedExchange(
        &g_KswRxpfPageFault.CpuCount,
        (LONG)activeCount);
    return STATUS_SUCCESS;
}

static ULONG_PTR
KswRxpfInstallIpi(
    _In_ ULONG_PTR Context
    )
{
    ULONG processorIndex = KeGetCurrentProcessorIndex();
    KSW_RXPF_IDTR currentIdtr;
    KSW_RXPF_IDTR verifyIdtr;
    PKSW_RXPF_CPU_IDT_STATE cpu = NULL;

    UNREFERENCED_PARAMETER(Context);
    if (processorIndex >= g_KswRxpfPageFault.CpuCapacity) {
        return 0ULL;
    }
    cpu = &g_KswRxpfPageFault.Cpus[processorIndex];
    if (InterlockedCompareExchange(&cpu->Active, 1, 1) == 0) {
        cpu->LastStatus = STATUS_NOT_FOUND;
        return 0ULL;
    }

    /* Refuse to replace an IDTR that changed after the per-CPU capture. */
    RtlZeroMemory(&currentIdtr, sizeof(currentIdtr));
    __sidt(&currentIdtr);
    if (currentIdtr.Base != cpu->OriginalIdtr.Base ||
        currentIdtr.Limit != cpu->OriginalIdtr.Limit) {
        cpu->LastStatus = STATUS_REVISION_MISMATCH;
        return 0ULL;
    }
    __lidt(&cpu->ShadowIdtr);
    KeMemoryBarrier();
    InterlockedExchange(&cpu->Installed, 1);
    RtlZeroMemory(&verifyIdtr, sizeof(verifyIdtr));
    __sidt(&verifyIdtr);
    if (verifyIdtr.Base != cpu->ShadowIdtr.Base ||
        verifyIdtr.Limit != cpu->ShadowIdtr.Limit) {
        /* Conservatively retain Installed so rollback restores this CPU. */
        cpu->LastStatus = STATUS_UNSUCCESSFUL;
        return 0ULL;
    }
    cpu->LastStatus = STATUS_SUCCESS;
    return 1ULL;
}

static ULONG_PTR
KswRxpfRestoreIpi(
    _In_ ULONG_PTR Context
    )
{
    ULONG processorIndex = KeGetCurrentProcessorIndex();
    KSW_RXPF_IDTR verifyIdtr;
    PKSW_RXPF_CPU_IDT_STATE cpu = NULL;

    UNREFERENCED_PARAMETER(Context);
    if (processorIndex >= g_KswRxpfPageFault.CpuCapacity) {
        return 0ULL;
    }
    cpu = &g_KswRxpfPageFault.Cpus[processorIndex];
    if (InterlockedCompareExchange(&cpu->Active, 1, 1) == 0) {
        return 1ULL;
    }
    if (InterlockedCompareExchange(&cpu->Installed, 1, 1) == 0) {
        cpu->LastStatus = STATUS_SUCCESS;
        return 1ULL;
    }

    __lidt(&cpu->OriginalIdtr);
    RtlZeroMemory(&verifyIdtr, sizeof(verifyIdtr));
    __sidt(&verifyIdtr);
    if (verifyIdtr.Base != cpu->OriginalIdtr.Base ||
        verifyIdtr.Limit != cpu->OriginalIdtr.Limit) {
        cpu->LastStatus = STATUS_UNSUCCESSFUL;
        return 0ULL;
    }
    KeMemoryBarrier();
    InterlockedExchange(&cpu->Installed, 0);
    cpu->LastStatus = STATUS_SUCCESS;
    return 1ULL;
}

static NTSTATUS
KswRxpfVerifyAllRowsRestored(
    VOID
    )
{
    LONG cpuCount = InterlockedCompareExchange(
        &g_KswRxpfPageFault.CpuCount,
        0,
        0);
    LONG index = 0;

    /* Every active row must report a verified restore before storage is freed. */
    for (index = 0; index < cpuCount; ++index) {
        PKSW_RXPF_CPU_IDT_STATE cpu = &g_KswRxpfPageFault.Cpus[index];

        if (InterlockedCompareExchange(&cpu->Active, 1, 1) != 0 &&
            (InterlockedCompareExchange(&cpu->Installed, 1, 1) != 0 ||
             cpu->LastStatus != STATUS_SUCCESS)) {
            return cpu->LastStatus != STATUS_SUCCESS
                ? cpu->LastStatus
                : STATUS_UNSUCCESSFUL;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
KswRxpfPageFaultManagerInitialize(
    _In_ PKSW_RXPF_PAGE_TABLE PageTable,
    _In_ ULONG MaximumProcessorCount
    )
{
    SIZE_T allocationBytes = 0U;
    PKSW_RXPF_CPU_IDT_STATE cpus = NULL;

    /* Allocate all topology rows before any IDT installation can begin. */
    if (PageTable == NULL || MaximumProcessorCount == 0UL ||
        MaximumProcessorCount > MAXULONG_PTR /
            sizeof(KSW_RXPF_CPU_IDT_STATE)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (InterlockedCompareExchange(
            &g_KswRxpfPageFault.Initialized,
            1,
            1) != 0) {
        return STATUS_SUCCESS;
    }
    allocationBytes =
        (SIZE_T)MaximumProcessorCount * sizeof(KSW_RXPF_CPU_IDT_STATE);
    cpus = (PKSW_RXPF_CPU_IDT_STATE)KswordARKAllocateNonPagedPool(
        allocationBytes,
        KSW_RXPF_IDT_TAG);
    if (cpus == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cpus, allocationBytes);
    RtlZeroMemory(&g_KswRxpfPageFault, sizeof(g_KswRxpfPageFault));
    g_KswRxpfPageFault.PageTable = PageTable;
    g_KswRxpfPageFault.Cpus = cpus;
    g_KswRxpfPageFault.CpuCapacity = MaximumProcessorCount;
    ExInitializePushLock(&g_KswRxpfPageFault.ControlLock);
    g_KswRxpfPageFault.ProcessorChangeHandle =
        KeRegisterProcessorChangeCallback(
            KswRxpfProcessorChangeCallback,
            NULL,
            0UL);
    if (g_KswRxpfPageFault.ProcessorChangeHandle == NULL) {
        ExFreePoolWithTag(cpus, KSW_RXPF_IDT_TAG);
        RtlZeroMemory(&g_KswRxpfPageFault, sizeof(g_KswRxpfPageFault));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeMemoryBarrier();
    InterlockedExchange(&g_KswRxpfPageFault.Initialized, 1);
    return STATUS_SUCCESS;
}

VOID
KswRxpfPageFaultManagerUninitialize(
    VOID
    )
{
    PKSW_RXPF_CPU_IDT_STATE cpus = g_KswRxpfPageFault.Cpus;
    PVOID callbackHandle =
        g_KswRxpfPageFault.ProcessorChangeHandle;
    LARGE_INTEGER retryDelay;

    /* Restore all CPUs before removing the topology callback and allocations. */
    retryDelay.QuadPart = -100000LL;
    while (!NT_SUCCESS(KswRxpfPageFaultRestore())) {
        /* A failed restore keeps every exception-visible allocation alive. */
        (void)KeDelayExecutionThread(KernelMode, FALSE, &retryDelay);
    }
    while (!NT_SUCCESS(KswRxpfDiagnosticsWaitForHandlers(5000UL))) {
        /* The manager owns exception-visible storage until every reader exits. */
    }
    if (callbackHandle != NULL) {
        KeDeregisterProcessorChangeCallback(callbackHandle);
    }
    KswRxpfFreeShadowTables();
    InterlockedExchange(&g_KswRxpfPageFault.Initialized, 0);
    KeMemoryBarrier();
    RtlZeroMemory(&g_KswRxpfPageFault, sizeof(g_KswRxpfPageFault));
    if (cpus != NULL) {
        ExFreePoolWithTag(cpus, KSW_RXPF_IDT_TAG);
    }
}

NTSTATUS
KswRxpfPageFaultInstall(
    VOID
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS rollbackStatus = STATUS_SUCCESS;
    LONG cpuCount = 0;
    LONG index = 0;

    /* Serialize capture/install/rollback against user control requests. */
    if (InterlockedCompareExchange(
            &g_KswRxpfPageFault.Initialized,
            1,
            1) == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_KswRxpfPageFault.ControlLock);
    if (InterlockedCompareExchange(
            &g_KswRxpfPageFault.Installed,
            1,
            1) != 0) {
        status = g_KswRxpfPageFault.LastStatus;
        ExReleasePushLockExclusive(&g_KswRxpfPageFault.ControlLock);
        KeLeaveCriticalRegion();
        return NT_SUCCESS(status)
            ? STATUS_SUCCESS
            : status;
    }
    InterlockedExchange(&g_KswRxpfPageFault.Installing, 1);
    status = KswRxpfCaptureShadowTables();
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    /* 预检：VBS/Hyper-V 会拦截 LIDT。原注释说会 "#GP + CFG 违规 (0x139)"，
       实测修正：**host (root partition) + guest 都会蓝屏 0x1E
       (STATUS_PRIVILEGED_INSTRUCTION 0xC0000096)，不是 0x139 CFG violation。**
       两处实证 fault RIP 都落在本文件的 LIDT 指令位置。
       CPL=0 可执行的 LIDT 被 hypervisor 识别为特权指令反弹 #GP 给 VTL0 kernel。
       SEH 可以 catch 这个 0x1E，但 shadow IDT 本身装不上，execute path 得走
       mode=1 restore + direct call (见 rxpf_runtime.c EXEC_PROOF 实现)。
       EXEC_PROOF 不依赖 shadow IDT，这个 gate 只是保护 self-test IOCTL 不被
       手动触发时意外蓝屏。 */
    {
        int cpuInfo[4] = {0};

        __cpuid(cpuInfo, 1);
        if (cpuInfo[2] & (1 << 31)) {
            /* hypervisor present — 检查是否 Microsoft Hyper-V */
            __cpuid(cpuInfo, 0x40000000);
            if (cpuInfo[1] == 0x7263694D &&   /* "Micr" */
                cpuInfo[2] == 0x666F736F &&   /* "osof" */
                cpuInfo[3] == 0x76482074) {   /* "t Hv" */
                status = STATUS_NOT_SUPPORTED;
                KswRxpfFreeShadowTables();
                goto Exit;
            }
        }
    }

    /* IPI 回调只执行 SIDT/LIDT 和定长行写入。 */
    (void)KeIpiGenericCall(KswRxpfInstallIpi, 0ULL);
    cpuCount = InterlockedCompareExchange(
        &g_KswRxpfPageFault.CpuCount,
        0,
        0);
    for (index = 0; index < cpuCount; ++index) {
        PKSW_RXPF_CPU_IDT_STATE cpu = &g_KswRxpfPageFault.Cpus[index];

        if (InterlockedCompareExchange(&cpu->Installed, 1, 1) == 0 ||
            cpu->LastStatus != STATUS_SUCCESS) {
            status = cpu->LastStatus != STATUS_SUCCESS
                ? cpu->LastStatus
                : STATUS_UNSUCCESSFUL;
            break;
        }
    }
    if (!NT_SUCCESS(status)) {
        /* Roll back every CPU that accepted the shadow before reporting failure. */
        (void)KeIpiGenericCall(KswRxpfRestoreIpi, 0ULL);
        rollbackStatus = KswRxpfVerifyAllRowsRestored();
        if (NT_SUCCESS(rollbackStatus)) {
            rollbackStatus =
                KswRxpfDiagnosticsWaitForHandlers(5000UL);
        }
        if (NT_SUCCESS(rollbackStatus)) {
            KswRxpfFreeShadowTables();
        } else {
            /* Keep restore callable and storage resident after partial rollback. */
            InterlockedExchange(&g_KswRxpfPageFault.Installed, 1);
            status = rollbackStatus;
        }
        goto Exit;
    }
    KeMemoryBarrier();
    InterlockedExchange(&g_KswRxpfPageFault.Installed, 1);

Exit:
    g_KswRxpfPageFault.LastStatus = status;
    InterlockedExchange(&g_KswRxpfPageFault.Installing, 0);
    ExReleasePushLockExclusive(&g_KswRxpfPageFault.ControlLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS
KswRxpfPageFaultRestore(
    VOID
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    /* Idempotent restore is safe during DriverEntry rollback and final unload. */
    if (InterlockedCompareExchange(
            &g_KswRxpfPageFault.Initialized,
            1,
            1) == 0) {
        return STATUS_SUCCESS;
    }
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_KswRxpfPageFault.ControlLock);
    if (InterlockedCompareExchange(
            &g_KswRxpfPageFault.Installed,
            1,
            1) == 0) {
        ExReleasePushLockExclusive(&g_KswRxpfPageFault.ControlLock);
        KeLeaveCriticalRegion();
        return STATUS_SUCCESS;
    }
    InterlockedExchange(&g_KswRxpfPageFault.Installing, 1);

    /* Every hooked CPU restores its original complete IDTR at IPI_LEVEL. */
    (void)KeIpiGenericCall(KswRxpfRestoreIpi, 0ULL);
    status = KswRxpfVerifyAllRowsRestored();
    if (NT_SUCCESS(status)) {
        status = KswRxpfDiagnosticsWaitForHandlers(5000UL);
        if (NT_SUCCESS(status)) {
            InterlockedExchange(&g_KswRxpfPageFault.Installed, 0);
            KswRxpfFreeShadowTables();
        }
    }
    /* Failure intentionally leaves Installed set and all storage resident. */
    g_KswRxpfPageFault.LastStatus = status;
    InterlockedExchange(&g_KswRxpfPageFault.Installing, 0);
    ExReleasePushLockExclusive(&g_KswRxpfPageFault.ControlLock);
    KeLeaveCriticalRegion();
    return status;
}

BOOLEAN
KswRxpfPageFaultIsInstalled(
    VOID
    )
{
    /* Return the atomic shadow-IDT publication state. */
    return InterlockedCompareExchange(
        &g_KswRxpfPageFault.Installed,
        1,
        1) != 0;
}

ULONG
KswRxpfPageFaultProcessorCount(
    VOID
    )
{
    /* Return the active topology captured for the current installation. */
    return (ULONG)InterlockedCompareExchange(
        &g_KswRxpfPageFault.CpuCount,
        0,
        0);
}

static BOOLEAN
KswRxpfExecuteFaultClass(
    _In_ const KSW_RXPF_TRAP_FRAME* Frame,
    _In_ ULONGLONG Cr2
    )
{
    ULONGLONG error = Frame->ErrorCode;

    /* Require P=1, W=0, U=0, RSVD=0, I/D=1 and no later extension bits. */
    return (Frame->Cs & 3ULL) == 0ULL &&
        Cr2 == Frame->Rip &&
        (error & 0x01ULL) != 0ULL &&
        (error & 0x02ULL) == 0ULL &&
        (error & 0x04ULL) == 0ULL &&
        (error & 0x08ULL) == 0ULL &&
        (error & 0x10ULL) != 0ULL &&
        (error & ~0x1FULL) == 0ULL;
}

static BOOLEAN
KswRxpfResumeStackValid(
    _In_ ULONGLONG ResumeRsp,
    _In_ ULONGLONG StackLow,
    _In_ ULONGLONG StackHigh
    )
{
    ULONGLONG frameBase = ResumeRsp - 24ULL;

    /* Assembly writes a three-qword same-CPL IRET frame below logical RSP. */
    if (frameBase > ResumeRsp || frameBase < StackLow ||
        ResumeRsp > StackHigh || ResumeRsp - frameBase != 24ULL) {
        return FALSE;
    }
    return MmIsAddressValid((PVOID)(ULONG_PTR)frameBase) &&
        MmIsAddressValid((PVOID)(ULONG_PTR)(ResumeRsp - 1ULL));
}

ULONGLONG
NTAPI
KswRxpfPageFaultDispatch(
    _Inout_ PKSW_RXPF_TRAP_FRAME Frame,
    _Out_ PVOID* TransferTargetOut,
    _Out_ ULONGLONG* ResumeRspOut,
    _Out_ PKSW_RXPF_TRAP_FRAME ResumeFrameOut
    )
{
    ULONG processorIndex = KeGetCurrentProcessorIndex();
    PKSW_RXPF_CPU_IDT_STATE cpu = NULL;
    PVOID originalHandler = g_KswRxpfPageFault.EmergencyOriginalHandler;
    ULONGLONG cr2 = __readcr2();
    ULONGLONG pageBase = cr2 & ~(PAGE_SIZE - 1ULL);
    PKSW_RXPF_PAGE_RECORD record = NULL;
    KSW_RXPF_EMULATION_CONTEXT emulation;
    KSW_RXPF_TRAP_FRAME originalFrame;
    ULONG frameBytes = FIELD_OFFSET(KSW_RXPF_TRAP_FRAME, HardwareRsp);
    ULONG availableBytes = 0UL;
    ULONGLONG stackLow = 0ULL;
    ULONGLONG stackHigh = 0ULL;
    ULONGLONG entryRsp = 0ULL;
    PHYSICAL_ADDRESS physicalAddress;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    ULONGLONG action = KSW_RXPF_DISPATCH_CHAIN;
    BOOLEAN enteredReadSide = FALSE;
    BOOLEAN enteredDepth = FALSE;
    BOOLEAN recordReferenced = FALSE;

    /*
     * The accepted interrupt gate keeps IPIs masked until this C entry. Join
     * the global read side before touching manager or page-table storage.
     */
    KswRxpfDiagnosticsEnterHandler();
    enteredReadSide = TRUE;

    /* Always return a chain target, including recursion and shutdown paths. */
    if (processorIndex < g_KswRxpfPageFault.CpuCapacity) {
        cpu = &g_KswRxpfPageFault.Cpus[processorIndex];
        if (cpu->OriginalPageFaultHandler != NULL) {
            originalHandler = cpu->OriginalPageFaultHandler;
        }
    }
    *TransferTargetOut = originalHandler;
    *ResumeRspOut = 0ULL;
    RtlZeroMemory(ResumeFrameOut, sizeof(*ResumeFrameOut));
    KswRxpfDiagnosticsCountTotalFault();
    if (Frame == NULL || originalHandler == NULL || cpu == NULL ||
        InterlockedCompareExchange(&cpu->Active, 1, 1) == 0) {
        status = STATUS_DEVICE_NOT_READY;
        goto Exit;
    }

    /* Per-CPU depth catches faults anywhere in the custom dispatch chain. */
    if (InterlockedIncrement(&cpu->HandlerDepth) != 1) {
        KswRxpfDiagnosticsCountRecursiveFault();
        InterlockedDecrement(&cpu->HandlerDepth);
        goto Exit;
    }
    enteredDepth = TRUE;

    /* Reject user, write, not-present, reserved-bit, and non-fetch faults. */
    if (!KswRxpfExecuteFaultClass(Frame, cr2) ||
        KeGetCurrentIrql() > APC_LEVEL) {
        status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }
    record = KswRxpfPageTableLookupFault(
        g_KswRxpfPageFault.PageTable,
        pageBase);
    if (record == NULL ||
        Frame->Rip < (ULONGLONG)record->PageBase ||
        Frame->Rip >= (ULONGLONG)record->PageBase + PAGE_SIZE) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }
    InterlockedIncrement(&record->ReferenceCount);
    recordReferenced = TRUE;
    KeMemoryBarrier();

    /* Snapshot state before any managed-failure branch needs diagnostics. */
    RtlZeroMemory(&originalFrame, sizeof(originalFrame));
    RtlCopyMemory(&originalFrame, Frame, frameBytes);
    RtlZeroMemory(ResumeFrameOut, sizeof(*ResumeFrameOut));
    RtlCopyMemory(ResumeFrameOut, &originalFrame, frameBytes);
    RtlZeroMemory(&emulation, sizeof(emulation));

    /* Revalidate the locked physical page identity before reading its alias. */
    physicalAddress = MmGetPhysicalAddress(
        (PVOID)(ULONG_PTR)record->PageBase);
    if (((ULONGLONG)physicalAddress.QuadPart >> PAGE_SHIFT) != record->Pfn ||
        record->WritableAlias == 0ULL) {
        status = STATUS_REVISION_MISMATCH;
        record->LastFailureReason =
            KSWORD_ARK_RXPF_EMULATION_INVALID_ADDRESS;
        goto ManagedFailure;
    }
    KswRxpfDiagnosticsCountManagedFault();
    InterlockedIncrement64(&record->FaultCount);

    emulation.Frame = ResumeFrameOut;
    IoGetStackLimits(
        (PULONG_PTR)&stackLow,
        (PULONG_PTR)&stackHigh);
    emulation.StackLow = stackLow;
    emulation.StackHigh = stackHigh;
    entryRsp = (ULONGLONG)(ULONG_PTR)&Frame->HardwareRsp;
    emulation.LogicalRsp = entryRsp;
    availableBytes = (ULONG)min(
        (ULONGLONG)KSW_RXPF_X64_MAX_INSTRUCTION_BYTES,
        ((ULONGLONG)record->PageBase + PAGE_SIZE) - Frame->Rip);
    emulation.AvailableBytes = availableBytes;

    /* Read only from the locked writable alias and never cross the page. */
    __try {
        RtlCopyMemory(
            emulation.Instruction,
            (const UCHAR*)(ULONG_PTR)record->WritableAlias +
                (Frame->Rip - (ULONGLONG)record->PageBase),
            availableBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        RtlCopyMemory(Frame, &originalFrame, frameBytes);
        record->LastFailureReason =
            KSWORD_ARK_RXPF_EMULATION_INVALID_ADDRESS;
        goto ManagedFailure;
    }

    /* Hook point: redirect execution when the faulting RIP matches the hook. */
    {
        ULONG hookAction = record->HookAction;
        ULONGLONG hookAddr = (ULONGLONG)record->HookAddress;

        if (hookAction == KSWORD_ARK_RXPF_HOOK_ACTION_REDIRECT &&
            hookAddr != 0ULL &&
            Frame->Rip == hookAddr &&
            record->RedirectTarget != 0ULL) {
            /* All GPRs are preserved in the resume frame from the snapshot. */
            ResumeFrameOut->Rip = record->RedirectTarget;
            *ResumeRspOut = entryRsp;
            InterlockedIncrement64(&record->RedirectCount);
            InterlockedIncrement64(&record->EmulatedCount);
            record->LastStatus = STATUS_SUCCESS;
            record->LastFailureReason =
                KSWORD_ARK_RXPF_EMULATION_REDIRECTED;
            KswRxpfDiagnosticsCountEmulatedInstruction();
            KswRxpfDiagnosticsRecord(
                processorIndex,
                cr2,
                originalFrame.Rip,
                originalFrame.ErrorCode,
                (ULONGLONG)record->RecordId,
                KSWORD_ARK_RXPF_DECODE_HOOK_REDIRECT,
                KSWORD_ARK_RXPF_EMULATION_REDIRECTED,
                record->RedirectTarget,
                STATUS_SUCCESS);
            action = KSW_RXPF_DISPATCH_HANDLED;
            goto Exit;
        }
        if (hookAction == KSWORD_ARK_RXPF_HOOK_ACTION_LOG &&
            hookAddr != 0ULL &&
            Frame->Rip == hookAddr) {
            /* Record the hook hit then fall through to normal emulation. */
            KswRxpfDiagnosticsRecord(
                processorIndex,
                cr2,
                originalFrame.Rip,
                originalFrame.ErrorCode,
                (ULONGLONG)record->RecordId,
                KSWORD_ARK_RXPF_DECODE_HOOK_REDIRECT,
                KSWORD_ARK_RXPF_EMULATION_SUCCESS,
                originalFrame.Rip,
                STATUS_SUCCESS);
        }
    }

    /* Emulate one complete whitelisted instruction or preserve the fault. */
    status = KswRxpfX64EmulateOne(&emulation);
    if (!NT_SUCCESS(status) ||
        entryRsp < 8ULL ||
        emulation.LogicalRsp < entryRsp - 8ULL ||
        !KswRxpfResumeStackValid(
            emulation.LogicalRsp,
            stackLow,
            stackHigh)) {
        if (NT_SUCCESS(status)) {
            status = STATUS_STACK_OVERFLOW;
            emulation.EmulationResult =
                KSWORD_ARK_RXPF_EMULATION_STACK_RANGE;
        }
        RtlCopyMemory(Frame, &originalFrame, frameBytes);
        record->LastFailureReason = emulation.EmulationResult;
        goto ManagedFailure;
    }

    /* Publish the modified context and same-CPL resume RSP to the assembly stub. */
    *ResumeRspOut = emulation.LogicalRsp;
    InterlockedIncrement64(&record->EmulatedCount);
    record->LastStatus = STATUS_SUCCESS;
    record->LastFailureReason = KSWORD_ARK_RXPF_EMULATION_SUCCESS;
    KswRxpfDiagnosticsCountEmulatedInstruction();
    KswRxpfDiagnosticsRecord(
        processorIndex,
        cr2,
        originalFrame.Rip,
        originalFrame.ErrorCode,
        (ULONGLONG)record->RecordId,
        emulation.DecodedInstruction,
        KSWORD_ARK_RXPF_EMULATION_SUCCESS,
        ResumeFrameOut->Rip,
        STATUS_SUCCESS);
    action = KSW_RXPF_DISPATCH_HANDLED;
    goto Exit;

ManagedFailure:
    /* Unsupported instructions remain visible to the original Windows #PF. */
    record->LastStatus = status;
    InterlockedIncrement64(&record->UnsupportedCount);
    KswRxpfDiagnosticsCountUnsupportedInstruction();
    KswRxpfDiagnosticsRecord(
        processorIndex,
        cr2,
        originalFrame.Rip,
        originalFrame.ErrorCode,
        (ULONGLONG)record->RecordId,
        emulation.DecodedInstruction,
        record->LastFailureReason,
        originalFrame.Rip,
        status);

Exit:
    if (action == KSW_RXPF_DISPATCH_CHAIN) {
        KswRxpfDiagnosticsCountChainedFault();
    }
    if (recordReferenced) {
        InterlockedDecrement(&record->ReferenceCount);
    }
    if (enteredDepth) {
        InterlockedDecrement(&cpu->HandlerDepth);
    }
    if (enteredReadSide) {
        KswRxpfDiagnosticsLeaveHandler();
    }
    return action;
}
