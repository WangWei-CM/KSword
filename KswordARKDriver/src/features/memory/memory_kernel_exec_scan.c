/*++

Module Name:

    memory_kernel_exec_scan.c

Abstract:

    Conservative read-only kernel executable page scanner for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "memory_kernel_exec_scan_internal.h"

static NTSTATUS
KswordARKKernelExecScanOnePage(
    _Inout_ KSW_KERNEL_EXEC_SCAN_STATE* State,
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request,
    _In_ ULONG64 PageAddress,
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_reads_(SectionCount) const IMAGE_SECTION_HEADER* SectionHeaders,
    _In_ ULONG SectionCount,
    _In_reads_bytes_(ModulePathBytes) const UCHAR* ModulePath,
    _In_ ULONG ModulePathBytes,
    _Out_ ULONG64* NextAddressOut
    )
/*++

Routine Description:

    扫描单个页或大页地址。中文说明：先查询页表，Present 且非 NX 才视为
    executable；根据实际 pageSize 推进扫描，避免大页内重复生成大量相同行。

Arguments:

    State - 扫描状态。
    Request - 请求快照。
    PageAddress - 当前页对齐地址。
    ModuleEntry - 所属模块。
    SectionHeaders - 本地复制的 section header 数组。
    SectionCount - section header 数量。
    ModulePath - 模块路径。
    ModulePathBytes - 模块路径长度上限。
    NextAddressOut - 返回下一次扫描地址。

Return Value:

    STATUS_SUCCESS 表示本页处理完成；非成功表示页表后端异常。

--*/
{
    KSWORD_ARK_PAGE_TABLE_ENTRY_INFO pageInfo;
    KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY candidate;
    ULONG pageSize = PAGE_SIZE;
    ULONG64 pageBase = PageAddress;
    BOOLEAN isTextLikePage = FALSE;
    BOOLEAN isWritablePage = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (State == NULL || Request == NULL || ModuleEntry == NULL || NextAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (PageAddress > (MAXULONGLONG - (ULONG64)PAGE_SIZE)) {
        *NextAddressOut = MAXULONGLONG;
    }
    else {
        *NextAddressOut = PageAddress + (ULONG64)PAGE_SIZE;
    }

    status = KswordARKKernelExecQueryPage(PageAddress, &pageInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (pageInfo.pageSize != 0UL) {
        pageSize = pageInfo.pageSize;
        pageBase = KswordARKKernelExecAlignDown(PageAddress, (ULONG64)pageSize);
        if (pageBase <= PageAddress &&
            (ULONG64)pageSize <= (MAXULONGLONG - pageBase)) {
            *NextAddressOut = pageBase + (ULONG64)pageSize;
        }
    }

    if (pageInfo.queryStatus != KSWORD_ARK_MEMORY_TRANSLATE_STATUS_OK) {
        if (pageInfo.queryStatus == KSWORD_ARK_MEMORY_TRANSLATE_STATUS_NOT_PRESENT) {
            return STATUS_SUCCESS;
        }
        return KswordARKKernelExecPageQueryFailureStatus(&pageInfo);
    }
    if (pageInfo.resolved == 0UL ||
        (pageInfo.effectiveFlags & KSWORD_ARK_PAGE_TABLE_FLAG_PRESENT) == 0UL) {
        return STATUS_UNSUCCESSFUL;
    }
    if ((pageInfo.effectiveFlags & KSWORD_ARK_PAGE_TABLE_FLAG_NX) != 0UL) {
        return STATUS_SUCCESS;
    }

    KswordARKKernelExecClassifyPageBySections(
        SectionHeaders,
        SectionCount,
        (ULONG64)(ULONG_PTR)ModuleEntry->ImageBase,
        ModuleEntry->ImageSize,
        pageBase,
        pageInfo.pageSize,
        &isTextLikePage,
        &isWritablePage);

    KswordARKKernelExecFillCandidate(
        pageBase,
        &pageInfo,
        ModuleEntry,
        ModulePath,
        ModulePathBytes,
        isTextLikePage,
        isWritablePage,
        &candidate);

    if (!KswordARKKernelExecShouldReturnCandidate(
        Request->flags,
        candidate.ownerKind,
        candidate.riskFlags)) {
        return STATUS_SUCCESS;
    }

    KswordARKKernelExecAddEntry(State, &candidate);
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKKernelExecScanModulePages(
    _Inout_ KSW_KERNEL_EXEC_SCAN_STATE* State,
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request,
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_reads_(SectionCount) const IMAGE_SECTION_HEADER* SectionHeaders,
    _In_ ULONG SectionCount,
    _In_reads_bytes_(ModulePathBytes) const UCHAR* ModulePath,
    _In_ ULONG ModulePathBytes
    )
/*++

Routine Description:

    扫描一个模块映像范围内的页。中文说明：这是 v1 的保守边界，不扫描全内核
    地址空间，只遍历 SystemModuleInformation 已加载模块的 imageBase..imageEnd；
    section headers 只用于 text/non-text/writable 分类。

Arguments:

    State - 扫描状态。
    Request - 请求快照。
    ModuleEntry - 当前已加载模块。
    SectionHeaders - 本地复制的 section header 数组。
    SectionCount - section header 数量。
    ModulePath - 模块路径。
    ModulePathBytes - 模块路径最大字节数。

Return Value:

    STATUS_SUCCESS 或页表查询异常状态。

--*/
{
    ULONG64 moduleBase = 0ULL;
    ULONG64 moduleEnd = 0ULL;
    ULONG64 scanAddress = 0ULL;
    NTSTATUS firstFailure = STATUS_SUCCESS;

    if (State == NULL || Request == NULL || ModuleEntry == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKKernelExecSafeModuleRange(ModuleEntry, &moduleBase, &moduleEnd)) {
        return STATUS_SUCCESS;
    }

    scanAddress = KswordARKKernelExecAlignDown(moduleBase, (ULONG64)PAGE_SIZE);
    if (scanAddress < moduleBase) {
        scanAddress = moduleBase;
    }
    if (Request->startAddress != 0ULL && scanAddress < Request->startAddress) {
        scanAddress = KswordARKKernelExecAlignDown(Request->startAddress, (ULONG64)PAGE_SIZE);
        if (scanAddress < moduleBase) {
            scanAddress = moduleBase;
        }
    }

    while (scanAddress < moduleEnd) {
        ULONG64 nextAddress = 0ULL;
        ULONG64 probeEnd = 0ULL;
        NTSTATUS status = STATUS_SUCCESS;

        if (Request->endAddress != 0ULL && scanAddress >= Request->endAddress) {
            break;
        }
        if (scanAddress > (MAXULONGLONG - (ULONG64)PAGE_SIZE)) {
            probeEnd = MAXULONGLONG;
        }
        else {
            /*
             * 中文说明：过滤检查以 4KB 探测窗口为最小单位。若请求 startAddress
             * 落在当前页中间，scanAddress..scanAddress+1 不会相交，会错误跳过
             * 该页；真正的大页边界会在页表查询后通过 NextAddressOut 推进。
             */
            probeEnd = scanAddress + (ULONG64)PAGE_SIZE;
        }
        if (!KswordARKKernelExecRangeIntersectsRequest(scanAddress, probeEnd, Request)) {
            break;
        }

        status = KswordARKKernelExecScanOnePage(
            State,
            Request,
            scanAddress,
            ModuleEntry,
            SectionHeaders,
            SectionCount,
            ModulePath,
            ModulePathBytes,
            &nextAddress);
        if (!NT_SUCCESS(status) && NT_SUCCESS(firstFailure)) {
            firstFailure = status;
        }
        if (nextAddress <= scanAddress) {
            if (scanAddress > (MAXULONGLONG - (ULONG64)PAGE_SIZE)) {
                break;
            }
            nextAddress = scanAddress + (ULONG64)PAGE_SIZE;
        }
        scanAddress = nextAddress;
    }

    return firstFailure;
}

static NTSTATUS
KswordARKKernelExecScanModule(
    _Inout_ KSW_KERNEL_EXEC_SCAN_STATE* State,
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request,
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry
    )
/*++

Routine Description:

    扫描一个已加载内核模块的映像页。中文说明：PE 头和 section header 均通过
    hook_scan_support 的安全读取 helper 复制到本地后解析；本函数不依赖 PE
    section execute 位作为最终依据，最终 executable 判定来自只读页表解析中的
    Present/NX。

Arguments:

    State - 扫描状态。
    Request - 请求快照。
    ModuleEntry - SystemModuleInformation 模块条目。

Return Value:

    STATUS_SUCCESS 表示模块处理完成；解析失败时返回对应状态供 partial 标记。

--*/
{
    IMAGE_SECTION_HEADER sectionHeaders[KSWORD_ARK_KERNEL_EXEC_MAX_SECTION_HEADERS];
    IMAGE_NT_HEADERS ntHeaders;
    const UCHAR* modulePathText = NULL;
    ULONG modulePathBytes = 0UL;
    ULONG sectionTableRva = 0UL;
    ULONG sectionCount = 0UL;
    ULONG sectionIndex = 0UL;
    ULONG64 moduleBase = 0ULL;
    ULONG64 moduleEnd = 0ULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS firstFailure = STATUS_SUCCESS;

    RtlZeroMemory(sectionHeaders, sizeof(sectionHeaders));
    if (State == NULL || Request == NULL || ModuleEntry == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKKernelExecSafeModuleRange(ModuleEntry, &moduleBase, &moduleEnd)) {
        return STATUS_SUCCESS;
    }
    if (!KswordARKKernelExecRangeIntersectsRequest(
        moduleBase,
        moduleEnd,
        Request)) {
        return STATUS_SUCCESS;
    }

    modulePathText = ModuleEntry->FullPathName;
    modulePathBytes = (ULONG)sizeof(ModuleEntry->FullPathName);
    RtlZeroMemory(&ntHeaders, sizeof(ntHeaders));
    if (!KswordARKHookReadImageNtHeaders(ModuleEntry, &ntHeaders)) {
        firstFailure = STATUS_INVALID_IMAGE_FORMAT;
    }
    else if (ntHeaders.FileHeader.NumberOfSections == 0U ||
        ntHeaders.FileHeader.NumberOfSections > KSWORD_ARK_KERNEL_EXEC_MAX_SECTION_HEADERS ||
        (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) >
            (MAXULONG - (ULONG)ntHeaders.FileHeader.SizeOfOptionalHeader)) {
        firstFailure = STATUS_INVALID_IMAGE_FORMAT;
    }
    else {
        /*
         * 中文说明：section table 位于 PE 头起始 RVA + OptionalHeader 字段偏移
         * + SizeOfOptionalHeader。不要从本地 ntHeaders 副本地址推导 RVA。
         */
        IMAGE_DOS_HEADER dosHeader;
        ULONG peHeaderRva = 0UL;

        RtlZeroMemory(&dosHeader, sizeof(dosHeader));
        if (!KswordARKHookReadImageBytes(ModuleEntry, 0UL, &dosHeader, sizeof(dosHeader)) ||
            dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
            dosHeader.e_lfanew <= 0) {
            firstFailure = STATUS_INVALID_IMAGE_FORMAT;
        }
        else {
            peHeaderRva = (ULONG)dosHeader.e_lfanew;
            if (peHeaderRva > (MAXULONG - (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader)) ||
                peHeaderRva + (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) >
                    (MAXULONG - (ULONG)ntHeaders.FileHeader.SizeOfOptionalHeader)) {
                firstFailure = STATUS_INVALID_IMAGE_FORMAT;
            }
            else {
                sectionTableRva =
                    peHeaderRva +
                    (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
                    (ULONG)ntHeaders.FileHeader.SizeOfOptionalHeader;
                sectionCount = (ULONG)ntHeaders.FileHeader.NumberOfSections;
            }
        }
    }

    for (sectionIndex = 0UL; sectionIndex < sectionCount; ++sectionIndex) {
        IMAGE_SECTION_HEADER sectionHeader;
        ULONG sectionRva = 0UL;

        if (!KswordARKHookAddRvaOffset(
            sectionTableRva,
            sectionIndex,
            (ULONG)sizeof(IMAGE_SECTION_HEADER),
            &sectionRva)) {
            if (NT_SUCCESS(firstFailure)) {
                firstFailure = STATUS_INTEGER_OVERFLOW;
            }
            break;
        }
        RtlZeroMemory(&sectionHeader, sizeof(sectionHeader));
        if (!KswordARKHookReadImageBytes(
            ModuleEntry,
            sectionRva,
            &sectionHeader,
            sizeof(sectionHeader))) {
            if (NT_SUCCESS(firstFailure)) {
                firstFailure = STATUS_ACCESS_VIOLATION;
            }
            continue;
        }

        RtlCopyMemory(&sectionHeaders[sectionIndex], &sectionHeader, sizeof(sectionHeaders[sectionIndex]));
    }

    status = KswordARKKernelExecScanModulePages(
        State,
        Request,
        ModuleEntry,
        sectionHeaders,
        sectionCount,
        modulePathText,
        modulePathBytes);
    if (!NT_SUCCESS(status) && NT_SUCCESS(firstFailure)) {
        firstFailure = status;
    }

    return firstFailure;
}

static BOOLEAN
KswordARKKernelExecValidateRequest(
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request
    )
/*++

Routine Description:

    校验扫描请求。中文说明：未知 flags、反向地址区间和明显无意义 maxEntries
    会被 handler/backend 拒绝；maxEntries 为 0 表示只按输出缓冲容量限制。

Arguments:

    Request - 请求结构。

Return Value:

    TRUE 表示请求可接受；FALSE 表示应返回 STATUS_INVALID_PARAMETER。

--*/
{
    if (Request == NULL) {
        return FALSE;
    }
    if ((Request->flags & ~KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_ALL) != 0UL) {
        return FALSE;
    }
    if (Request->startAddress != 0ULL &&
        Request->endAddress != 0ULL &&
        Request->endAddress <= Request->startAddress) {
        return FALSE;
    }
    return TRUE;
}

NTSTATUS
KswordARKDriverScanKernelExecutableMemory(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    保守扫描内核模块映像内的 executable 页。中文说明：v1 只遍历
    SystemModuleInformation 返回的已加载内核模块映像范围，复用页表只读查询判断
    Present/NX/Writable；PE section 只参与 text/non-text/writable 分类，不扫描
    全内核地址空间，不写 PTE，不改 CR0。

Arguments:

    OutputBuffer - 响应缓冲，头部后紧跟 entries。
    OutputBufferLength - 响应缓冲总长度。
    Request - 扫描请求，包含 flags/maxEntries/start/end。
    BytesWrittenOut - 返回实际响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；完整性由 response->status/lastStatus 表示。

--*/
{
    KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    KSW_KERNEL_EXEC_SCAN_STATE state;
    ULONG moduleInfoBytes = 0UL;
    ULONG moduleIndex = 0UL;
    ULONG entryCapacity = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS firstPartialStatus = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_KERNEL_EXEC_SCAN_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!KswordARKKernelExecValidateRequest(Request)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_MEMORY_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_KERNEL_EXEC_SCAN_STATUS_UNAVAILABLE;
    response->entrySize = sizeof(KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY);
    response->lastStatus = STATUS_SUCCESS;

    entryCapacity = (ULONG)((OutputBufferLength - KSWORD_ARK_KERNEL_EXEC_SCAN_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY));
    if (Request->maxEntries != 0UL && entryCapacity > Request->maxEntries) {
        entryCapacity = Request->maxEntries;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->status = KSWORD_ARK_KERNEL_EXEC_SCAN_STATUS_IRQL_REJECTED;
        response->lastStatus = STATUS_INVALID_DEVICE_STATE;
        *BytesWrittenOut = KSWORD_ARK_KERNEL_EXEC_SCAN_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    // 中文说明：moduleInfoBytes 仅满足 snapshot helper 的输出契约，扫描逻辑使用条目数。
    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_KERNEL_EXEC_SCAN_STATUS_QUERY_FAILED;
        *BytesWrittenOut = KSWORD_ARK_KERNEL_EXEC_SCAN_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    response->moduleCount = moduleInfo->NumberOfModules;

    RtlZeroMemory(&state, sizeof(state));
    state.Response = response;
    state.EntryCapacity = entryCapacity;

    for (moduleIndex = 0UL; moduleIndex < moduleInfo->NumberOfModules; ++moduleIndex) {
        NTSTATUS moduleStatus = STATUS_SUCCESS;

        moduleStatus = KswordARKKernelExecScanModule(
            &state,
            Request,
            &moduleInfo->Modules[moduleIndex]);
        if (!NT_SUCCESS(moduleStatus) && NT_SUCCESS(firstPartialStatus)) {
            firstPartialStatus = moduleStatus;
        }
    }

    if (state.Truncated || !NT_SUCCESS(firstPartialStatus)) {
        response->status = KSWORD_ARK_KERNEL_EXEC_SCAN_STATUS_PARTIAL_CONSERVATIVE;
        response->lastStatus = state.Truncated ?
            STATUS_BUFFER_TOO_SMALL :
            firstPartialStatus;
    }
    else {
        response->status = KSWORD_ARK_KERNEL_EXEC_SCAN_STATUS_CONSERVATIVE;
        response->lastStatus = STATUS_SUCCESS;
    }

    *BytesWrittenOut = KSWORD_ARK_KERNEL_EXEC_SCAN_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY));

    ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    return STATUS_SUCCESS;
}
