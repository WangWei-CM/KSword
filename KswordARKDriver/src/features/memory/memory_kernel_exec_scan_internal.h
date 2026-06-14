#pragma once

#include "ark/ark_driver.h"
#include "../kernel/hook_scan_support.h"

#ifndef MAXULONGLONG
#define MAXULONGLONG ((ULONG64)~0ULL)
#endif

// 响应头不包含尾随 entries[1]，用于 METHOD_BUFFERED 输出长度校验。
#define KSWORD_ARK_KERNEL_EXEC_SCAN_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_RESPONSE) - sizeof(KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY))

// 中文说明：section header 只用于分类，不用于决定页面是否 executable；固定上限
// 防止异常 PE 头让栈缓冲失控，超出时继续按模块范围做无 section 的保守扫描。
#define KSWORD_ARK_KERNEL_EXEC_MAX_SECTION_HEADERS 96UL

typedef struct _KSW_KERNEL_EXEC_SCAN_STATE
{
    // Response：输出响应头，函数只在 entries 容量范围内写入结果行。
    KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_RESPONSE* Response;
    // EntryCapacity：OutputBufferLength 可容纳的 entry 数量，已受 maxEntries 裁剪。
    ULONG EntryCapacity;
    // Truncated：TRUE 表示 totalCount 大于 returnedCount，响应状态应为 partial。
    BOOLEAN Truncated;
    // HaveLastAggregate/LastAggregate：保存上一条聚合行，便于连续页合并。
    BOOLEAN HaveLastAggregate;
    KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY LastAggregate;
} KSW_KERNEL_EXEC_SCAN_STATE;

ULONG64
KswordARKKernelExecAlignDown(
    _In_ ULONG64 Value,
    _In_ ULONG64 Alignment
    );

BOOLEAN
KswordARKKernelExecRangeIntersectsRequest(
    _In_ ULONG64 RangeStart,
    _In_ ULONG64 RangeEnd,
    _In_ const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST* Request
    );

BOOLEAN
KswordARKKernelExecSafeModuleRange(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Out_ ULONG64* ModuleBaseOut,
    _Out_ ULONG64* ModuleEndOut
    );

VOID
KswordARKKernelExecAddEntry(
    _Inout_ KSW_KERNEL_EXEC_SCAN_STATE* State,
    _In_ const KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY* Candidate
    );

NTSTATUS
KswordARKKernelExecQueryPage(
    _In_ ULONG64 VirtualAddress,
    _Out_ KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* InfoOut
    );

NTSTATUS
KswordARKKernelExecPageQueryFailureStatus(
    _In_ const KSWORD_ARK_PAGE_TABLE_ENTRY_INFO* PageInfo
    );

BOOLEAN
KswordARKKernelExecShouldReturnCandidate(
    _In_ ULONG RequestFlags,
    _In_ ULONG OwnerKind,
    _In_ ULONG RiskFlags
    );

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
    );

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
    );
