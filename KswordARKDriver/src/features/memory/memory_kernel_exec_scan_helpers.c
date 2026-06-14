/*++

Module Name:

    memory_kernel_exec_scan_helpers.c

Abstract:

    Helper routines for conservative kernel executable page scanning.

Environment:

    Kernel-mode Driver Framework

--*/

#include "memory_kernel_exec_scan_internal.h"

ULONG64
KswordARKKernelExecAlignDown(
    _In_ ULONG64 Value,
    _In_ ULONG64 Alignment
    )
/*++

Routine Description:

    向下对齐地址。中文说明：扫描页表时以实际 pageSize 为粒度，函数只做
    2 的幂对齐计算，不访问目标地址。

Arguments:

    Value - 输入地址。
    Alignment - 对齐粒度，必须是 2 的幂；异常输入会退化为原值。

Return Value:

    返回对齐后的地址；函数不返回错误状态。

--*/
{
    if (Alignment == 0ULL || (Alignment & (Alignment - 1ULL)) != 0ULL) {
        return Value;
    }
    return Value & ~(Alignment - 1ULL);
}

BOOLEAN
KswordARKKernelExecRangeIntersectsRequest(
    _In_ ULONG64 RangeStart,
    _In_ ULONG64 RangeEnd,
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request
    )
/*++

Routine Description:

    判断半开区间是否与请求地址过滤条件相交。中文说明：startAddress/endAddress
    均为 0 表示不过滤；endAddress 为 0 表示从 startAddress 到最高地址。

Arguments:

    RangeStart - 候选半开区间起始地址。
    RangeEnd - 候选半开区间结束地址。
    Request - 本次扫描请求快照。

Return Value:

    TRUE 表示候选区间需要扫描或返回；FALSE 表示可跳过。

--*/
{
    ULONG64 filterEnd = 0ULL;

    if (Request == NULL) {
        return FALSE;
    }
    if (RangeEnd <= RangeStart) {
        return FALSE;
    }
    if (Request->startAddress == 0ULL && Request->endAddress == 0ULL) {
        return TRUE;
    }

    filterEnd = (Request->endAddress == 0ULL) ? MAXULONGLONG : Request->endAddress;
    if (RangeEnd <= Request->startAddress) {
        return FALSE;
    }
    if (RangeStart >= filterEnd) {
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN
KswordARKKernelExecSectionIsTextLike(
    _In_ const IMAGE_SECTION_HEADER* SectionHeader
    )
/*++

Routine Description:

    判断 PE section 是否应归类为模块代码/text 区域。中文说明：优先使用
    IMAGE_SCN_CNT_CODE，同时兼容常见的 .text 名称；其它 executable section
    会被归为 module non-text executable。

Arguments:

    SectionHeader - 已安全复制到本地栈上的 section header。

Return Value:

    TRUE 表示 text/code 类 section；FALSE 表示非 text 类 section。

--*/
{
    if (SectionHeader == NULL) {
        return FALSE;
    }
    if ((SectionHeader->Characteristics & IMAGE_SCN_CNT_CODE) != 0UL) {
        return TRUE;
    }
    if (SectionHeader->Name[0] == '.' &&
        (SectionHeader->Name[1] == 't' || SectionHeader->Name[1] == 'T') &&
        (SectionHeader->Name[2] == 'e' || SectionHeader->Name[2] == 'E') &&
        (SectionHeader->Name[3] == 'x' || SectionHeader->Name[3] == 'X') &&
        (SectionHeader->Name[4] == 't' || SectionHeader->Name[4] == 'T')) {
        return TRUE;
    }
    return FALSE;
}

BOOLEAN
KswordARKKernelExecSafeModuleRange(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Out_ ULONG64* ModuleBaseOut,
    _Out_ ULONG64* ModuleEndOut
    )
/*++

Routine Description:

    安全计算模块映像半开地址区间。中文说明：SystemModuleInformation 来自内核，
    但仍要防止 base + size 回绕后影响地址过滤和 section 裁剪。

Arguments:

    ModuleEntry - 已加载模块条目。
    ModuleBaseOut - 返回模块基址。
    ModuleEndOut - 返回模块结束地址。

Return Value:

    TRUE 表示区间有效；FALSE 表示条目缺少基址/大小或发生整数溢出。

--*/
{
    ULONG64 moduleBase = 0ULL;

    if (ModuleEntry == NULL || ModuleBaseOut == NULL || ModuleEndOut == NULL) {
        return FALSE;
    }
    *ModuleBaseOut = 0ULL;
    *ModuleEndOut = 0ULL;
    if (ModuleEntry->ImageBase == NULL || ModuleEntry->ImageSize == 0UL) {
        return FALSE;
    }

    moduleBase = (ULONG64)(ULONG_PTR)ModuleEntry->ImageBase;
    if ((ULONG64)ModuleEntry->ImageSize > (MAXULONGLONG - moduleBase)) {
        return FALSE;
    }

    *ModuleBaseOut = moduleBase;
    *ModuleEndOut = moduleBase + (ULONG64)ModuleEntry->ImageSize;
    return TRUE;
}

static BOOLEAN
KswordARKKernelExecCanMergeEntry(
    _In_ const KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Left,
    _In_ const KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Right,
    _Out_opt_ BOOLEAN* OverlapOut
    )
/*++

Routine Description:

    判断两个扫描结果是否属于同一连续区间。中文说明：只合并同一模块、同一
    页大小、同一权限和同一风险分类的相邻页；大页重复命中时返回 overlap。

Arguments:

    Left - 已聚合的上一条结果。
    Right - 新候选结果。
    OverlapOut - 可选输出，TRUE 表示 Right 落在 Left 覆盖范围内。

Return Value:

    TRUE 表示可合并为一个 entry；FALSE 表示应新增 entry 或跳过重复。

--*/
{
    ULONG64 leftBytes = 0ULL;
    ULONG64 leftEnd = 0ULL;

    if (OverlapOut != NULL) {
        *OverlapOut = FALSE;
    }
    if (Left == NULL || Right == NULL || Left->pageSize == 0UL) {
        return FALSE;
    }
    if (Left->moduleBase != Right->moduleBase ||
        Left->moduleSize != Right->moduleSize ||
        Left->pageSize != Right->pageSize) {
        return FALSE;
    }

    leftBytes = (ULONG64)Left->pageCount * (ULONG64)Left->pageSize;
    if (leftBytes > (MAXULONGLONG - Left->virtualAddress)) {
        return FALSE;
    }
    leftEnd = Left->virtualAddress + leftBytes;

    if (Right->virtualAddress < leftEnd) {
        if (OverlapOut != NULL) {
            *OverlapOut = TRUE;
        }
        return FALSE;
    }
    if (Left->effectiveFlags != Right->effectiveFlags ||
        Left->riskFlags != Right->riskFlags ||
        Left->ownerKind != Right->ownerKind) {
        return FALSE;
    }
    return Right->virtualAddress == leftEnd;
}

static VOID
KswordARKKernelExecMakeEntryMoreConservative(
    _Inout_ KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Target,
    _In_ const KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Source
    )
/*++

Routine Description:

    让现有扫描行向更保守的分类收敛。中文说明：当同一页在多个 section 中
    被重复观测时，后到的更高风险分类应覆盖前面的较低风险标签。

Arguments:

    Target - 当前已写入的扫描行。
    Source - 新观测到的更高风险候选。

Return Value:

    None. 函数只更新 Target。

--*/
{
    if (Target == NULL || Source == NULL) {
        return;
    }

    Target->effectiveFlags |= Source->effectiveFlags;
    Target->riskFlags |= Source->riskFlags;
    if (Source->ownerKind > Target->ownerKind) {
        Target->ownerKind = Source->ownerKind;
    }
}

VOID
KswordARKKernelExecAddEntry(
    _Inout_ KSW_KERNEL_EXEC_SCAN_STATE* State,
    _In_ const KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Candidate
    )
/*++

Routine Description:

    将一个候选 executable page 聚合进响应。中文说明：totalCount 统计聚合后的
    行数；输出缓冲不足时继续维护 totalCount，但不写越界。

Arguments:

    State - 扫描状态，包含响应头、容量和上一条聚合结果。
    Candidate - 新的页或大页候选。

Return Value:

    None. 函数只更新 State/Response。

--*/
{
    BOOLEAN overlap = FALSE;

    if (State == NULL || State->Response == NULL || Candidate == NULL || Candidate->pageCount == 0UL) {
        return;
    }

    if (State->HaveLastAggregate) {
        if (KswordARKKernelExecCanMergeEntry(&State->LastAggregate, Candidate, &overlap)) {
            State->LastAggregate.pageCount += Candidate->pageCount;
            KswordARKKernelExecMakeEntryMoreConservative(&State->LastAggregate, Candidate);
            if (!State->Truncated && State->Response->returnedCount > 0UL) {
                State->Response->entries[State->Response->returnedCount - 1UL].pageCount =
                    State->LastAggregate.pageCount;
                KswordARKKernelExecMakeEntryMoreConservative(
                    &State->Response->entries[State->Response->returnedCount - 1UL],
                    Candidate);
            }
            return;
        }
        if (overlap) {
            KswordARKKernelExecMakeEntryMoreConservative(&State->LastAggregate, Candidate);
            if (!State->Truncated && State->Response->returnedCount > 0UL) {
                KswordARKKernelExecMakeEntryMoreConservative(
                    &State->Response->entries[State->Response->returnedCount - 1UL],
                    Candidate);
            }
            return;
        }
    }

    RtlCopyMemory(&State->LastAggregate, Candidate, sizeof(State->LastAggregate));
    State->HaveLastAggregate = TRUE;
    State->Response->totalCount += 1UL;
    if (State->Response->returnedCount < State->EntryCapacity) {
        RtlCopyMemory(
            &State->Response->entries[State->Response->returnedCount],
            Candidate,
            sizeof(*Candidate));
        State->Response->returnedCount += 1UL;
    }
    else {
        State->Truncated = TRUE;
    }
}

NTSTATUS
KswordARKKernelExecQueryPage(
    _In_ ULONG64 VirtualAddress,
    _Out_ KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* InfoOut
    )
/*++

Routine Description:

    查询一个内核虚拟地址的页表信息。中文说明：复用现有
    KswordARKDriverQueryPageTableEntry 后端，保持 Present/NX/Writable 解析口径
    与单地址页表 IOCTL 一致；该函数只读页表，不修改 PTE/PDE。

Arguments:

    VirtualAddress - 待查询的内核虚拟地址。
    InfoOut - 接收页表解析结果。

Return Value:

    STATUS_SUCCESS 表示 InfoOut 已初始化；解析失败细节见 InfoOut->queryStatus。

--*/
{
    KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST request;
    KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE response;
    size_t bytesWritten = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (InfoOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(InfoOut, sizeof(*InfoOut));
    RtlZeroMemory(&request, sizeof(request));
    RtlZeroMemory(&response, sizeof(response));

    request.processId = 0UL;
    request.virtualAddress = VirtualAddress;
    status = KswordARKDriverQueryPageTableEntry(
        &response,
        sizeof(response),
        &request,
        &bytesWritten);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (bytesWritten < sizeof(response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(InfoOut, &response.info, sizeof(*InfoOut));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKKernelExecPageQueryFailureStatus(
    _In_ const KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* PageInfo
    )
/*++

Routine Description:

    将页表查询协议状态转换成可记录的 NTSTATUS。中文说明：页表后端通常返回
    STATUS_SUCCESS 并把细节放入 queryStatus/walkStatus；扫描层需要把不支持、
    读失败、非法地址等严重状态传递给响应 lastStatus，以免误报为完整保守扫描。

Arguments:

    PageInfo - 单页页表查询结果。

Return Value:

    返回可用于 response->lastStatus 的 NTSTATUS；无法细分时返回 STATUS_UNSUCCESSFUL。

--*/
{
    if (PageInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(PageInfo->walkStatus) &&
        PageInfo->walkStatus != STATUS_NO_MEMORY) {
        return PageInfo->walkStatus;
    }
    if (PageInfo->queryStatus == KSWORD_ARK_MEMORY_TRANSLATE_STATUS_PROCESS_LOOKUP_FAILED) {
        return NT_SUCCESS(PageInfo->lookupStatus) ? STATUS_NOT_FOUND : PageInfo->lookupStatus;
    }
    if (PageInfo->queryStatus == KSWORD_ARK_MEMORY_TRANSLATE_STATUS_INVALID_ADDRESS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (PageInfo->queryStatus == KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_SUPPORTED) {
        return STATUS_NOT_SUPPORTED;
    }
    if (PageInfo->queryStatus == KSWORD_ARK_MEMORY_TRANSLATE_STATUS_IRQL_REJECTED) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    return STATUS_UNSUCCESSFUL;
}

BOOLEAN
KswordARKKernelExecShouldReturnCandidate(
    _In_ ULONG RequestFlags,
    _In_ ULONG OwnerKind,
    _In_ ULONG RiskFlags
    )
/*++

Routine Description:

    根据请求 flags 判断候选行是否需要返回。中文说明：flags 为 0 时等价于
    INCLUDE_ALL；writable executable 即使来自 text section，也可被对应风险过滤命中。

Arguments:

    RequestFlags - 请求 flags。
    OwnerKind - 候选 ownerKind。
    RiskFlags - 候选 riskFlags。

Return Value:

    TRUE 表示该候选应计入响应；FALSE 表示跳过。

--*/
{
    ULONG effectiveFlags = RequestFlags;

    if (effectiveFlags == 0UL) {
        effectiveFlags = KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_ALL;
    }
    if ((effectiveFlags & KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_WRITABLE_EXECUTABLE) != 0UL &&
        (RiskFlags & KSWORD_ARK_KERNEL_EXEC_RISK_WRITABLE_EXECUTABLE) != 0UL) {
        return TRUE;
    }
    if ((effectiveFlags & KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_MODULE_TEXT) != 0UL &&
        OwnerKind == KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_TEXT) {
        return TRUE;
    }
    if ((effectiveFlags & KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_MODULE_NON_TEXT) != 0UL &&
        OwnerKind == KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_NON_TEXT) {
        return TRUE;
    }
    return FALSE;
}

VOID
KswordARKKernelExecFillCandidate(
    _In_ ULONG64 PageAddress,
    _In_ const KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* PageInfo,
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_reads_bytes_(ModulePathBytes) const UCHAR* ModulePath,
    _In_ ULONG ModulePathBytes,
    _In_ BOOLEAN IsTextLikeSection,
    _In_ BOOLEAN IsWritableSection,
    _Out_ KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Candidate
    )
/*++

Routine Description:

    构造一个内核 executable page 响应行。中文说明：ownerKind/riskFlags 根据 PE
    section 属性和页表 effectiveFlags 共同确定，模块路径来自 SystemModuleInformation。

Arguments:

    PageAddress - 页或大页起始虚拟地址。
    PageInfo - 页表查询结果。
    ModuleEntry - 所属已加载模块。
    ModulePath - 模块路径 ANSI 区间。
    ModulePathBytes - 模块路径最大字节数。
    IsTextLikeSection - 当前 section 是否为 text/code。
    IsWritableSection - 当前 section PE 属性是否可写。
    Candidate - 输出响应行。

Return Value:

    None. 函数只写入 Candidate。

--*/
{
    ULONG ownerKind = KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_TEXT;
    ULONG riskFlags = KSWORD_ARK_KERNEL_EXEC_RISK_NONE;

    if (Candidate == NULL || PageInfo == NULL || ModuleEntry == NULL) {
        return;
    }

    RtlZeroMemory(Candidate, sizeof(*Candidate));
    Candidate->virtualAddress = PageAddress;
    Candidate->pageCount = 1UL;
    Candidate->pageSize = PageInfo->pageSize;
    Candidate->effectiveFlags = PageInfo->effectiveFlags;
    Candidate->moduleBase = (ULONG64)(ULONG_PTR)ModuleEntry->ImageBase;
    Candidate->moduleSize = ModuleEntry->ImageSize;

    if (!IsTextLikeSection) {
        ownerKind = KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_NON_TEXT;
        riskFlags |= KSWORD_ARK_KERNEL_EXEC_RISK_MODULE_NON_TEXT_EXECUTABLE;
    }
    if (IsWritableSection) {
        riskFlags |= KSWORD_ARK_KERNEL_EXEC_RISK_SECTION_WRITABLE;
    }
    if ((PageInfo->effectiveFlags & KSWORD_ARK_PAGE_TABLE_FLAG_WRITABLE) != 0UL) {
        ownerKind = KSWORD_ARK_KERNEL_EXEC_OWNER_MODULE_WRITABLE_EXECUTABLE;
        riskFlags |= KSWORD_ARK_KERNEL_EXEC_RISK_WRITABLE_EXECUTABLE;
    }
    if (PageInfo->largePageType != KSWORD_ARK_PAGE_TABLE_LARGE_PAGE_NONE) {
        riskFlags |= KSWORD_ARK_KERNEL_EXEC_RISK_LARGE_PAGE;
    }

    Candidate->ownerKind = ownerKind;
    Candidate->riskFlags = riskFlags;
    KswordARKHookCopyBoundedAnsiToWide(
        ModulePath,
        ModulePathBytes,
        Candidate->modulePath,
        KSWORD_ARK_KERNEL_EXEC_MODULE_PATH_CHARS);
}

VOID
KswordARKKernelExecClassifyPageBySections(
    _In_reads_(SectionCount) const IMAGE_SECTION_HEADER* SectionHeaders,
    _In_ ULONG SectionCount,
    _In_ ULONG64 ModuleBase,
    _In_ ULONG ModuleSize,
    _In_ ULONG64 PageAddress,
    _In_ ULONG PageSize,
    _Out_ BOOLEAN* IsTextLikeOut,
    _Out_ BOOLEAN* IsWritableOut
    )
/*++

Routine Description:

    按 PE section 元数据归类一个模块页。中文说明：扫描范围是完整模块映像，
    section 只用于把 executable 页标记成 text / non-text / writable；没有命中
    任何 section 的 header/gap 页保守归为 non-text。

Arguments:

    SectionHeaders - 本地复制的 section header 数组。
    SectionCount - section header 数量。
    ModuleBase - 模块加载基址。
    ModuleSize - 模块映像大小。
    PageAddress - 当前页起始地址。
    PageSize - 当前页大小，来自页表解析或默认 4KB。
    IsTextLikeOut - 返回 TRUE 表示该页只命中 text/code section。
    IsWritableOut - 返回 TRUE 表示该页命中任一 writable section。

Return Value:

    None. 函数只写入输出布尔值。

--*/
{
    ULONG sectionIndex = 0UL;
    ULONG64 moduleEnd = ModuleBase + (ULONG64)ModuleSize;
    ULONG64 pageEnd = 0ULL;
    BOOLEAN hasTextSection = FALSE;
    BOOLEAN hasNonTextSection = FALSE;
    BOOLEAN hasWritableSection = FALSE;

    if (IsTextLikeOut != NULL) {
        *IsTextLikeOut = FALSE;
    }
    if (IsWritableOut != NULL) {
        *IsWritableOut = FALSE;
    }
    if (SectionHeaders == NULL || SectionCount == 0UL || ModuleSize == 0UL || PageSize == 0UL) {
        return;
    }
    if ((ULONG64)PageSize > (MAXULONGLONG - PageAddress)) {
        pageEnd = MAXULONGLONG;
    }
    else {
        pageEnd = PageAddress + (ULONG64)PageSize;
    }

    for (sectionIndex = 0UL; sectionIndex < SectionCount; ++sectionIndex) {
        const IMAGE_SECTION_HEADER* sectionHeader = &SectionHeaders[sectionIndex];
        ULONG sectionSize = sectionHeader->Misc.VirtualSize;
        ULONG64 sectionStart = 0ULL;
        ULONG64 sectionEnd = 0ULL;

        if (sectionSize == 0UL) {
            sectionSize = sectionHeader->SizeOfRawData;
        }
        if (sectionSize == 0UL || sectionHeader->VirtualAddress >= ModuleSize) {
            continue;
        }
        if ((ULONG64)sectionHeader->VirtualAddress > (MAXULONGLONG - ModuleBase)) {
            continue;
        }

        sectionStart = ModuleBase + (ULONG64)sectionHeader->VirtualAddress;
        if ((ULONG64)sectionSize > (MAXULONGLONG - sectionStart)) {
            sectionEnd = moduleEnd;
        }
        else {
            sectionEnd = sectionStart + (ULONG64)sectionSize;
        }
        if (sectionEnd > moduleEnd || sectionEnd < sectionStart) {
            sectionEnd = moduleEnd;
        }
        if (pageEnd <= sectionStart || PageAddress >= sectionEnd) {
            continue;
        }

        if (KswordARKKernelExecSectionIsTextLike(sectionHeader)) {
            hasTextSection = TRUE;
        }
        else {
            hasNonTextSection = TRUE;
        }
        if ((sectionHeader->Characteristics & IMAGE_SCN_MEM_WRITE) != 0UL) {
            hasWritableSection = TRUE;
        }
    }

    if (IsTextLikeOut != NULL) {
        *IsTextLikeOut = (hasTextSection && !hasNonTextSection) ? TRUE : FALSE;
    }
    if (IsWritableOut != NULL) {
        *IsWritableOut = hasWritableSection;
    }
}
