/*++

Module Name:

    memory_pagetable.c

Abstract:

    x64 virtual-to-physical translation and page-table query helpers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#if defined(_M_AMD64) || defined(_M_X64)
#include <intrin.h>
#endif

#ifndef STATUS_PARTIAL_COPY
#define STATUS_PARTIAL_COPY ((NTSTATUS)0x8000000DL)
#endif

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTKERNELAPI
VOID
KeStackAttachProcess(
    _Inout_ PVOID Process,
    _Out_ PVOID ApcState
    );

NTKERNELAPI
VOID
KeUnstackDetachProcess(
    _In_ PVOID ApcState
    );

// x64 每级页表项大小固定为 8 字节。
#define KSWORD_ARK_PAGE_TABLE_ENTRY_BYTES 8ULL

// x64 四级分页使用 9 位索引。
#define KSWORD_ARK_PAGE_TABLE_INDEX_MASK 0x1FFULL

// CR3 的低 12 位为控制位，物理基址需要按页对齐。
#define KSWORD_ARK_PAGE_TABLE_CR3_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

// 普通 4KB PTE 的 PFN 位范围为 12..51。
#define KSWORD_ARK_PAGE_TABLE_4KB_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

// 2MB 大页 PDE 的物理基址位范围为 21..51。
#define KSWORD_ARK_PAGE_TABLE_2MB_ADDRESS_MASK 0x000FFFFFFFE00000ULL

// 1GB 大页 PDPTE 的物理基址位范围为 30..51。
#define KSWORD_ARK_PAGE_TABLE_1GB_ADDRESS_MASK 0x000FFFFFC0000000ULL

// IA32_EFER.NXE 开启时 bit63 表示 NX；未读取 EFER 时仍按原始位展示。
#define KSWORD_ARK_PAGE_TABLE_NX_BIT 0x8000000000000000ULL

// CR4.LA57 表示五级分页；当前协议只定义四级 PML4/PDPTE/PDE/PTE 展示。
#define KSWORD_ARK_X64_CR4_LA57_BIT 0x0000000000001000ULL

// 页表项 Present 位。
#define KSWORD_ARK_PAGE_TABLE_PRESENT_BIT 0x0000000000000001ULL

// PDE/PDPTE 的 PS 位；PTE 中同一位按 PAT 解释，本模块只在上级项使用它判断大页。
#define KSWORD_ARK_PAGE_TABLE_LARGE_BIT 0x0000000000000080ULL

typedef struct _KSWORD_ARK_PAGE_TABLE_WALK_CONTEXT
{
    ULONG ProcessId;
    ULONG64 VirtualAddress;
    ULONG64 Cr3PhysicalAddress;
    PEPROCESS ProcessObject;
} KSWORD_ARK_PAGE_TABLE_WALK_CONTEXT;

static BOOLEAN
KswordARKPageTableIsCanonicalAddress(
    _In_ ULONG64 VirtualAddress
    )
/*++

Routine Description:

    判断 x64 虚拟地址是否为 48 位 canonical 地址。中文说明：当前实现不假定
    LA57 五级分页，未知平台或五级分页环境由上层保守返回 NOT_SUPPORTED。

Arguments:

    VirtualAddress - 待检查虚拟地址。

Return Value:

    TRUE 表示地址满足 48 位 canonical 规则；FALSE 表示地址无效。

--*/
{
    ULONG64 signExtension = VirtualAddress & 0xFFFF000000000000ULL;
    BOOLEAN signBitSet = ((VirtualAddress & 0x0000800000000000ULL) != 0ULL) ? TRUE : FALSE;

    if (signBitSet) {
        return signExtension == 0xFFFF000000000000ULL;
    }
    return signExtension == 0ULL;
}

static ULONG
KswordARKPageTableExtractIndex(
    _In_ ULONG64 VirtualAddress,
    _In_ ULONG Shift
    )
/*++

Routine Description:

    提取指定层级页表索引。中文说明：x64 四级分页每层 9 位，调用方传入
    39/30/21/12 分别得到 PML4/PDPT/PD/PT 索引。

Arguments:

    VirtualAddress - 待解析虚拟地址。
    Shift - 右移位数。

Return Value:

    0..511 范围内的页表索引。

--*/
{
    return (ULONG)((VirtualAddress >> Shift) & KSWORD_ARK_PAGE_TABLE_INDEX_MASK);
}

static ULONG
KswordARKPageTableDecodeEntryFlags(
    _In_ ULONG64 EntryValue,
    _In_ BOOLEAN LargePageEntry
    )
/*++

Routine Description:

    将 x64 页表项原始 bit 转成共享协议 flags。中文说明：函数只解析通用位，
    不解释软件保留位；LargePageEntry 由调用方按层级传入，避免把 PTE 的 PAT
    bit 误报为大页。

Arguments:

    EntryValue - 原始页表项值。
    LargePageEntry - TRUE 表示该项来自 PDPTE/PDE 且 PS 位可表示大页。

Return Value:

    KSWORD_ARK_PAGE_TABLE_FLAG_* 组合。

--*/
{
    ULONG flags = 0UL;

    if ((EntryValue & 0x1ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_PRESENT;
    }
    if ((EntryValue & 0x2ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_WRITABLE;
    }
    if ((EntryValue & 0x4ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_USER;
    }
    if ((EntryValue & 0x8ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_WRITE_THROUGH;
    }
    if ((EntryValue & 0x10ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_CACHE_DISABLE;
    }
    if ((EntryValue & 0x20ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_ACCESSED;
    }
    if ((EntryValue & 0x40ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_DIRTY;
    }
    if (LargePageEntry && (EntryValue & KSWORD_ARK_PAGE_TABLE_LARGE_BIT) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_LARGE_PAGE;
    }
    if ((EntryValue & 0x100ULL) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_GLOBAL;
    }
    if ((EntryValue & KSWORD_ARK_PAGE_TABLE_NX_BIT) != 0ULL) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_NX;
    }

    return flags;
}

static ULONG
KswordARKPageTableMergeEffectiveFlags(
    _In_ ULONG Pml4eFlags,
    _In_ ULONG PdpteFlags,
    _In_ ULONG PdeFlags,
    _In_ ULONG PteFlags,
    _In_ ULONG LevelCount
    )
/*++

Routine Description:

    合并页表各层权限为最终展示 flags。中文说明：Present/Writable/User 需要所有
    已参与层级同时允许才算有效，NX 只要任一层置位就生效，其它诊断位保留按 OR
    展示，方便 R3 同时看到 accessed/dirty/global 等信息。

Arguments:

    Pml4eFlags - PML4E flags。
    PdpteFlags - PDPTE flags。
    PdeFlags - PDE flags。
    PteFlags - PTE flags；大页时可为 0。
    LevelCount - 参与合并的层级数量，1GB 为 2，2MB 为 3，4KB 为 4。

Return Value:

    合并后的 KSWORD_ARK_PAGE_TABLE_FLAG_* 位图。

--*/
{
    ULONG flags = Pml4eFlags | PdpteFlags | PdeFlags | PteFlags;
    ULONG requiredMask = Pml4eFlags;

    if (LevelCount >= 2UL) {
        requiredMask &= PdpteFlags;
    }
    if (LevelCount >= 3UL) {
        requiredMask &= PdeFlags;
    }
    if (LevelCount >= 4UL) {
        requiredMask &= PteFlags;
    }

    if ((requiredMask & KSWORD_ARK_PAGE_TABLE_FLAG_PRESENT) == 0UL) {
        flags &= ~KSWORD_ARK_PAGE_TABLE_FLAG_PRESENT;
    }
    if ((requiredMask & KSWORD_ARK_PAGE_TABLE_FLAG_WRITABLE) == 0UL) {
        flags &= ~KSWORD_ARK_PAGE_TABLE_FLAG_WRITABLE;
    }
    if ((requiredMask & KSWORD_ARK_PAGE_TABLE_FLAG_USER) == 0UL) {
        flags &= ~KSWORD_ARK_PAGE_TABLE_FLAG_USER;
    }
    if ((Pml4eFlags | PdpteFlags | PdeFlags | PteFlags) & KSWORD_ARK_PAGE_TABLE_FLAG_NX) {
        flags |= KSWORD_ARK_PAGE_TABLE_FLAG_NX;
    }

    return flags;
}

static NTSTATUS
KswordARKPageTableReadPhysicalU64(
    _In_ ULONG64 PhysicalAddress,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    从物理地址读取一个 64 位页表项。中文说明：页表 walker 只读 8 字节项，
    使用 MmCopyMemory physical 路径并包裹异常，避免直接映射或解引用不可信地址。

Arguments:

    PhysicalAddress - 页表项物理地址。
    ValueOut - 接收读取到的原始 64 位值。

Return Value:

    STATUS_SUCCESS 表示完整读取 8 字节；否则返回 MmCopyMemory 或异常状态。

--*/
{
    MM_COPY_ADDRESS copyAddress;
    SIZE_T bytesCopied = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (ValueOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ValueOut = 0ULL;

    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.PhysicalAddress.QuadPart = (LONGLONG)PhysicalAddress;

    __try {
        status = MmCopyMemory(
            ValueOut,
            copyAddress,
            sizeof(*ValueOut),
            MM_COPY_MEMORY_PHYSICAL,
            &bytesCopied);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        bytesCopied = 0U;
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (bytesCopied != sizeof(*ValueOut)) {
        return STATUS_PARTIAL_COPY;
    }
    return STATUS_SUCCESS;
}

static VOID
KswordARKPageTableInitInfo(
    _Out_ KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* Info,
    _In_ ULONG ProcessId,
    _In_ ULONG64 VirtualAddress
    )
/*++

Routine Description:

    初始化页表查询响应结构。中文说明：所有层级状态先置为未解析，调用方后续
    逐层填充原始项、物理地址、索引和最终 VA->PA 结果。

Arguments:

    Info - 输出页表信息结构。
    ProcessId - 目标 PID；0 表示当前进程上下文。
    VirtualAddress - 待解析虚拟地址。

Return Value:

    None. 函数只写入 Info。

--*/
{
    RtlZeroMemory(Info, sizeof(*Info));
    Info->version = KSWORD_ARK_MEMORY_PROTOCOL_VERSION;
    Info->size = sizeof(*Info);
    Info->processId = ProcessId;
    Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_UNAVAILABLE;
    Info->lookupStatus = STATUS_SUCCESS;
    Info->walkStatus = STATUS_NOT_SUPPORTED;
    Info->source = KSWORD_ARK_MEMORY_SOURCE_R0_PAGE_TABLE_WALK;
    Info->largePageType = KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_NONE;
    Info->pageSize = 0UL;
    Info->resolved = 0UL;
    Info->virtualAddress = VirtualAddress;
    Info->pml4Index = KswordARKPageTableExtractIndex(VirtualAddress, 39U);
    Info->pdptIndex = KswordARKPageTableExtractIndex(VirtualAddress, 30U);
    Info->pdIndex = KswordARKPageTableExtractIndex(VirtualAddress, 21U);
    Info->ptIndex = KswordARKPageTableExtractIndex(VirtualAddress, 12U);
}

static NTSTATUS
KswordARKPageTableAttachAndReadCr3(
    _Inout_ KSWORD_ARK_PAGE_TABLE_WALK_CONTEXT* Context
    )
/*++

Routine Description:

    在目标进程上下文读取 CR3。中文说明：Windows 不公开稳定 EPROCESS DirectoryTableBase
    偏移，本模块不硬编码版本相关偏移，而是通过 KeStackAttachProcess 后读取 CR3。

Arguments:

    Context - walker 上下文，ProcessObject 必须已经引用。

Return Value:

    STATUS_SUCCESS 表示 Cr3PhysicalAddress 已填充；未知架构返回 STATUS_NOT_SUPPORTED。

--*/
{
#if defined(_M_AMD64) || defined(_M_X64)
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    BOOLEAN attached = FALSE;
    unsigned __int64 cr3Value = 0ULL;
    unsigned __int64 cr4Value = 0ULL;

    if (Context == NULL || Context->ProcessObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    cr4Value = __readcr4();
    if ((cr4Value & KSWORD_ARK_X64_CR4_LA57_BIT) != 0ULL) {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(attachState, sizeof(attachState));
    __try {
        KeStackAttachProcess((PVOID)Context->ProcessObject, attachState);
        attached = TRUE;
        cr3Value = __readcr3();
        KeUnstackDetachProcess(attachState);
        attached = FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (attached) {
            KeUnstackDetachProcess(attachState);
            attached = FALSE;
        }
        return GetExceptionCode();
    }

    Context->Cr3PhysicalAddress =
        ((ULONG64)cr3Value) & KSWORD_ARK_PAGE_TABLE_CR3_ADDRESS_MASK;
    return STATUS_SUCCESS;
#else
    UNREFERENCED_PARAMETER(Context);
    return STATUS_NOT_SUPPORTED;
#endif
}

static NTSTATUS
KswordARKPageTableResolveProcess(
    _In_ ULONG ProcessId,
    _Outptr_ PEPROCESS* ProcessObjectOut
    )
/*++

Routine Description:

    获取页表解析目标进程对象。中文说明：ProcessId 为 0 时使用当前进程并主动
    ObReferenceObject，非 0 时通过 PsLookupProcessByProcessId 获取引用。

Arguments:

    ProcessId - 目标 PID，0 表示当前进程。
    ProcessObjectOut - 接收需要调用方释放的 EPROCESS 引用。

Return Value:

    STATUS_SUCCESS 或进程查找失败状态。

--*/
{
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessObjectOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ProcessObjectOut = NULL;

    if (ProcessId == 0UL) {
        processObject = PsGetCurrentProcess();
        ObReferenceObject(processObject);
        *ProcessObjectOut = processObject;
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &processObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *ProcessObjectOut = processObject;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKPageTableWalk(
    _Inout_ KSWORD_ARK_PAGE_TABLE_WALK_CONTEXT* Context,
    _Out_ KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* Info
    )
/*++

Routine Description:

    执行四级页表只读遍历。中文说明：函数逐层读取 PML4E/PDPTE/PDE/PTE，
    遇到 not-present 立即停止并返回可展示的部分信息；遇到 1GB/2MB 大页时按
    大页偏移计算最终物理地址。

Arguments:

    Context - 解析上下文，包含目标进程和虚拟地址。
    Info - 输出页表信息结构。

Return Value:

    STATUS_SUCCESS 表示遍历逻辑完成；not-present 也通过 Info->queryStatus 表示。

--*/
{
    ULONG64 nextTablePhysical = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Context == NULL || Info == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_IRQL_REJECTED;
        Info->walkStatus = STATUS_INVALID_DEVICE_STATE;
        return STATUS_SUCCESS;
    }
    if (!KswordARKPageTableIsCanonicalAddress(Context->VirtualAddress)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_INVALID_ADDRESS;
        Info->walkStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    status = KswordARKPageTableAttachAndReadCr3(Context);
    if (!NT_SUCCESS(status)) {
        Info->queryStatus =
            (status == STATUS_NOT_SUPPORTED) ?
            KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_SUPPORTED :
            KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED;
        Info->walkStatus = status;
        return STATUS_SUCCESS;
    }

    Info->cr3PhysicalAddress = Context->Cr3PhysicalAddress;
    Info->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_PRESENT;

    Info->pml4ePhysicalAddress =
        Context->Cr3PhysicalAddress +
        ((ULONG64)Info->pml4Index * KSWORD_ARK_PAGE_TABLE_ENTRY_BYTES);
    status = KswordARKPageTableReadPhysicalU64(
        Info->pml4ePhysicalAddress,
        &Info->pml4eValue);
    Info->pml4eFlags = KswordARKPageTableDecodeEntryFlags(Info->pml4eValue, FALSE);
    Info->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PML4E_PRESENT;
    if (!NT_SUCCESS(status)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED;
        Info->walkStatus = status;
        return STATUS_SUCCESS;
    }
    if ((Info->pml4eValue & KSWORD_ARK_PAGE_TABLE_PRESENT_BIT) == 0ULL) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_PRESENT;
        Info->walkStatus = STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }

    nextTablePhysical = Info->pml4eValue & KSWORD_ARK_PAGE_TABLE_4KB_ADDRESS_MASK;
    Info->pdptePhysicalAddress =
        nextTablePhysical +
        ((ULONG64)Info->pdptIndex * KSWORD_ARK_PAGE_TABLE_ENTRY_BYTES);
    status = KswordARKPageTableReadPhysicalU64(
        Info->pdptePhysicalAddress,
        &Info->pdpteValue);
    Info->pdpteFlags = KswordARKPageTableDecodeEntryFlags(Info->pdpteValue, TRUE);
    Info->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PDPTE_PRESENT;
    if (!NT_SUCCESS(status)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED;
        Info->walkStatus = status;
        return STATUS_SUCCESS;
    }
    if ((Info->pdpteValue & KSWORD_ARK_PAGE_TABLE_PRESENT_BIT) == 0ULL) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_PRESENT;
        Info->walkStatus = STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }
    if ((Info->pdpteValue & KSWORD_ARK_PAGE_TABLE_LARGE_BIT) != 0ULL) {
        Info->largePageType = KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_1GB;
        Info->pageSize = KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_1GB;
        Info->physicalAddress =
            (Info->pdpteValue & KSWORD_ARK_PAGE_TABLE_1GB_ADDRESS_MASK) |
            (Context->VirtualAddress & (KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_1GB - 1ULL));
        Info->effectiveFlags = KswordARKPageTableMergeEffectiveFlags(
            Info->pml4eFlags,
            Info->pdpteFlags,
            0UL,
            0UL,
            2UL);
        Info->resolved = 1UL;
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_OK;
        Info->walkStatus = STATUS_SUCCESS;
        Info->fieldFlags |=
            KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT |
            KSWORD_ARK_MEMORY_FIELD_LARGE_PAGE_1GB |
            KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_WALK_COMPLETE;
        return STATUS_SUCCESS;
    }

    nextTablePhysical = Info->pdpteValue & KSWORD_ARK_PAGE_TABLE_4KB_ADDRESS_MASK;
    Info->pdePhysicalAddress =
        nextTablePhysical +
        ((ULONG64)Info->pdIndex * KSWORD_ARK_PAGE_TABLE_ENTRY_BYTES);
    status = KswordARKPageTableReadPhysicalU64(
        Info->pdePhysicalAddress,
        &Info->pdeValue);
    Info->pdeFlags = KswordARKPageTableDecodeEntryFlags(Info->pdeValue, TRUE);
    Info->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PDE_PRESENT;
    if (!NT_SUCCESS(status)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED;
        Info->walkStatus = status;
        return STATUS_SUCCESS;
    }
    if ((Info->pdeValue & KSWORD_ARK_PAGE_TABLE_PRESENT_BIT) == 0ULL) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_PRESENT;
        Info->walkStatus = STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }
    if ((Info->pdeValue & KSWORD_ARK_PAGE_TABLE_LARGE_BIT) != 0ULL) {
        Info->largePageType = KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_2MB;
        Info->pageSize = KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_2MB;
        Info->physicalAddress =
            (Info->pdeValue & KSWORD_ARK_PAGE_TABLE_2MB_ADDRESS_MASK) |
            (Context->VirtualAddress & (KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_2MB - 1ULL));
        Info->effectiveFlags = KswordARKPageTableMergeEffectiveFlags(
            Info->pml4eFlags,
            Info->pdpteFlags,
            Info->pdeFlags,
            0UL,
            3UL);
        Info->resolved = 1UL;
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_OK;
        Info->walkStatus = STATUS_SUCCESS;
        Info->fieldFlags |=
            KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT |
            KSWORD_ARK_MEMORY_FIELD_LARGE_PAGE_2MB |
            KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_WALK_COMPLETE;
        return STATUS_SUCCESS;
    }

    nextTablePhysical = Info->pdeValue & KSWORD_ARK_PAGE_TABLE_4KB_ADDRESS_MASK;
    Info->ptePhysicalAddress =
        nextTablePhysical +
        ((ULONG64)Info->ptIndex * KSWORD_ARK_PAGE_TABLE_ENTRY_BYTES);
    status = KswordARKPageTableReadPhysicalU64(
        Info->ptePhysicalAddress,
        &Info->pteValue);
    Info->pteFlags = KswordARKPageTableDecodeEntryFlags(Info->pteValue, FALSE);
    Info->fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PTE_PRESENT;
    if (!NT_SUCCESS(status)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED;
        Info->walkStatus = status;
        return STATUS_SUCCESS;
    }
    if ((Info->pteValue & KSWORD_ARK_PAGE_TABLE_PRESENT_BIT) == 0ULL) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_PRESENT;
        Info->walkStatus = STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }

    Info->largePageType = KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_NONE;
    Info->pageSize = KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_4KB;
    Info->physicalAddress =
        (Info->pteValue & KSWORD_ARK_PAGE_TABLE_4KB_ADDRESS_MASK) |
        (Context->VirtualAddress & (KSWORD_ARK_PAGE_TABLE_PAGE_SIZE_4KB - 1ULL));
    Info->effectiveFlags = KswordARKPageTableMergeEffectiveFlags(
        Info->pml4eFlags,
        Info->pdpteFlags,
        Info->pdeFlags,
        Info->pteFlags,
        4UL);
    Info->resolved = 1UL;
    Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_OK;
    Info->walkStatus = STATUS_SUCCESS;
    Info->fieldFlags |=
        KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT |
        KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_WALK_COMPLETE;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKPageTableQueryCommon(
    _Out_ KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* Info,
    _In_ ULONG ProcessId,
    _In_ ULONG64 VirtualAddress
    )
/*++

Routine Description:

    页表查询公共入口。中文说明：函数负责初始化响应、引用目标进程、执行只读
    walker 并释放对象；外层 IOCTL 或导出函数只需要处理缓冲区。

Arguments:

    Info - 输出页表信息结构。
    ProcessId - 目标 PID，0 表示当前进程。
    VirtualAddress - 待解析虚拟地址。

Return Value:

    STATUS_SUCCESS 表示 Info 有效；失败通常只发生在参数无效。

--*/
{
    KSWORD_ARK_PAGE_TABLE_WALK_CONTEXT context;
    NTSTATUS status = STATUS_SUCCESS;

    if (Info == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KswordARKPageTableInitInfo(Info, ProcessId, VirtualAddress);
    RtlZeroMemory(&context, sizeof(context));
    context.ProcessId = ProcessId;
    context.VirtualAddress = VirtualAddress;

    status = KswordARKPageTableResolveProcess(ProcessId, &context.ProcessObject);
    Info->lookupStatus = status;
    if (!NT_SUCCESS(status)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_PROCESS_LOOKUP_FAILED;
        Info->walkStatus = STATUS_NOT_FOUND;
        return STATUS_SUCCESS;
    }

    status = KswordARKPageTableWalk(&context, Info);
    ObDereferenceObject(context.ProcessObject);
    context.ProcessObject = NULL;

    if (!NT_SUCCESS(status)) {
        Info->queryStatus = KSWORD_ARK_MEMORY_TRANSLATE_STATUS_READ_FAILED;
        Info->walkStatus = status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverTranslateVirtualAddress(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    将目标进程虚拟地址解析为物理地址。中文说明：该接口返回完整页表信息结构，
    但 R3 可只使用 physicalAddress/pageSize/resolved 字段作为 VA->PA 结果。

Arguments:

    OutputBuffer - 固定响应缓冲区。
    OutputBufferLength - 响应缓冲区长度。
    Request - 查询请求，包含 PID 和 VA。
    BytesWrittenOut - 接收响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；解析细节见 response->info。

--*/
{
    KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE* response = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < sizeof(KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->flags != 0UL || Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE*)OutputBuffer;

    status = KswordARKPageTableQueryCommon(
        &response->info,
        Request->processId,
        Request->virtualAddress);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverQueryPageTableEntry(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询目标虚拟地址对应的 PML4E/PDPTE/PDE/PTE 信息。中文说明：接口只读页表，
    不修改任何 PTE/PDE；大页时不会伪造不存在的下级 PTE，而是通过 largePageType
    和 pageSize 告知 R3 展示层。

Arguments:

    OutputBuffer - 固定响应缓冲区。
    OutputBufferLength - 响应缓冲区长度。
    Request - 查询请求，包含 PID 和 VA。
    BytesWrittenOut - 接收响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；解析细节见 response->info。

--*/
{
    KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE* response = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->flags != 0UL || Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE*)OutputBuffer;

    status = KswordARKPageTableQueryCommon(
        &response->info,
        Request->processId,
        Request->virtualAddress);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
