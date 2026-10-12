/*++

Module Name:

    kernel_cache_fallback.c

Abstract:

    Runtime fallback for MmUnloadedDrivers and PiDDBCacheTable.  Candidate
    globals are recovered from bounded call graphs rooted at stable ntoskrnl
    exports, with routine ranges limited by the PE exception directory. No
    address is published until PE-section checks, uniqueness, and
    live semantic validation agree.  Ambiguous or empty structures fail closed.

    Field-shape heuristics alone never authorize a kernel API call here.  A
    candidate PiDDB lock must be proven to sit on the global resource list
    before it reaches ExAcquireResourceSharedLite, and the cache table is
    walked through bounded fault-tolerant reads instead of the Rtl generic
    table API, which dereferences and mutates its argument unconditionally.

Environment:

    Kernel mode, PASSIVE_LEVEL during DynData initialization.

--*/

#include <ntifs.h>
#include "kernel_cache_fallback.h"
#include "kernel_object_probe.h"
#include "runtime_signature_scan.h"
#include "pool_compat.h"
#include "ark/ark_startup.h" // 精确地址补全路径也必须遵守 OS build 上限。

#define KSW_KERNEL_CACHE_TAG 'cCsK'
#define KSW_KERNEL_CACHE_MAX_REFERENCES 512UL
#define KSW_KERNEL_CACHE_ROUTINE_SCAN_BYTES 0x0800UL
#define KSW_KERNEL_CACHE_MAX_UNLOADED_RECORDS 50UL
#define KSW_KERNEL_CACHE_MIN_UNLOADED_STRIDE 0x20UL
#define KSW_KERNEL_CACHE_MAX_UNLOADED_STRIDE 0x80UL
#define KSW_KERNEL_CACHE_MAX_PRIVATE_ENTRY 0x100UL
#define KSW_KERNEL_CACHE_MAX_SAMPLE_ENTRIES 16UL
#define KSW_KERNEL_CACHE_MAX_NAME_BYTES 520U
#define KSW_KERNEL_CACHE_MAX_AVL_ELEMENTS 0x10000UL
#define KSW_KERNEL_CACHE_MAX_AVL_DEPTH 64UL

typedef struct _KSW_UNLOADED_LAYOUT_CANDIDATE
{
    ULONG_PTR PointerGlobal;
    ULONG_PTR Records;
    ULONG NameOffset;
    ULONG StartAddressOffset;
    ULONG EndAddressOffset;
    ULONG CurrentTimeOffset;
    ULONG RecordSize;
    ULONG ValidRecordCount;
    ULONG_PTR ReferenceRoutine;
} KSW_UNLOADED_LAYOUT_CANDIDATE, *PKSW_UNLOADED_LAYOUT_CANDIDATE;

typedef struct _KSW_PIDDB_LAYOUT_CANDIDATE
{
    PRTL_AVL_TABLE Table;
    PERESOURCE Lock;
    ULONG DriverNameOffset;
    ULONG TimeDateStampOffset;
    ULONG LoadStatusOffset;
    ULONG EntrySize;
} KSW_PIDDB_LAYOUT_CANDIDATE, *PKSW_PIDDB_LAYOUT_CANDIDATE;

static BOOLEAN
KswordARKKernelCacheIsKernelPointer(
    _In_ ULONG_PTR Address
    )
{
#if defined(_M_AMD64) || defined(_M_X64)
    return Address >= (ULONG_PTR)MmSystemRangeStart &&
        (Address >> 48U) == 0xFFFFU;
#else
    return Address >= (ULONG_PTR)MmSystemRangeStart;
#endif
}

static BOOLEAN
KswordARKKernelCacheUnicodeSuffixMatches(
    _In_ const UNICODE_STRING* String,
    _In_z_ PCWSTR Suffix
    )
/*++

Routine Description:

    Compare a bounded private UNICODE_STRING suffix without trusting its buffer.

Return Value:

    TRUE only for a readable, even-sized, bounded kernel string with the suffix.

--*/
{
    WCHAR tail[5];
    SIZE_T suffixBytes = 0U;
    UNICODE_STRING tailString;
    UNICODE_STRING expectedString;

    if (String == NULL || Suffix == NULL || String->Buffer == NULL ||
        !KswordARKKernelCacheIsKernelPointer((ULONG_PTR)String->Buffer) ||
        String->Length == 0U || String->Length > KSW_KERNEL_CACHE_MAX_NAME_BYTES ||
        String->Length > String->MaximumLength ||
        (String->Length & (sizeof(WCHAR) - 1U)) != 0U) {
        return FALSE;
    }
    RtlInitUnicodeString(&expectedString, Suffix);
    suffixBytes = expectedString.Length;
    if (suffixBytes == 0U ||
        suffixBytes >= sizeof(tail) ||
        String->Length < suffixBytes) {
        return FALSE;
    }
    RtlZeroMemory(tail, sizeof(tail));
    if (!KswordARKRuntimeReadMemory(
            (const UCHAR*)String->Buffer + String->Length - suffixBytes,
            tail,
            suffixBytes)) {
        return FALSE;
    }
    tailString.Buffer = tail;
    tailString.Length = (USHORT)suffixBytes;
    tailString.MaximumLength = (USHORT)suffixBytes;
    return RtlEqualUnicodeString(&tailString, &expectedString, TRUE);
}

static BOOLEAN
KswordARKKernelCacheReadDriverName(
    _In_ const UCHAR* Object,
    _In_ ULONG Offset,
    _Out_opt_ UNICODE_STRING* NameOut
    )
{
    UNICODE_STRING name;

    if (Object == NULL) {
        return FALSE;
    }
    RtlZeroMemory(&name, sizeof(name));
    if (!KswordARKRuntimeReadMemory(
            Object + Offset,
            &name,
            sizeof(name)) ||
        (!KswordARKKernelCacheUnicodeSuffixMatches(&name, L".sys") &&
         !KswordARKKernelCacheUnicodeSuffixMatches(&name, L".dll"))) {
        return FALSE;
    }
    if (NameOut != NULL) {
        *NameOut = name;
    }
    return TRUE;
}

static BOOLEAN
KswordARKKernelCacheTimeIsPlausible(
    _In_ LONGLONG Value,
    _In_ LONGLONG CurrentSystemTime
    )
{
    const LONGLONG earliestSupportedTime = 129067776000000000LL;
    const LONGLONG oneDay = 24LL * 60LL * 60LL * 10000000LL;

    return Value >= earliestSupportedTime &&
        CurrentSystemTime > 0LL && Value <= CurrentSystemTime + oneDay;
}

static ULONG
KswordARKKernelCacheCountUnloadedLayoutMatches(
    _In_ ULONG_PTR Records,
    _In_ ULONG RecordSize,
    _In_ ULONG NameOffset,
    _In_ ULONG StartOffset,
    _In_ ULONG EndOffset,
    _In_ ULONG TimeOffset,
    _In_ LONGLONG CurrentSystemTime
    )
{
    ULONG index = 0UL;
    ULONG matches = 0UL;

    for (index = 0UL; index < KSW_KERNEL_CACHE_MAX_UNLOADED_RECORDS; ++index) {
        const UCHAR* record = (const UCHAR*)Records +
            ((SIZE_T)index * RecordSize);
        ULONG_PTR startAddress = 0U;
        ULONG_PTR endAddress = 0U;
        LARGE_INTEGER currentTime;

        RtlZeroMemory(&currentTime, sizeof(currentTime));
        if (!KswordARKKernelCacheReadDriverName(record, NameOffset, NULL) ||
            !KswordARKRuntimeReadMemory(
                record + StartOffset,
                &startAddress,
                sizeof(startAddress)) ||
            !KswordARKRuntimeReadMemory(
                record + EndOffset,
                &endAddress,
                sizeof(endAddress)) ||
            !KswordARKRuntimeReadMemory(
                record + TimeOffset,
                &currentTime,
                sizeof(currentTime)) ||
            !KswordARKKernelCacheIsKernelPointer(startAddress) ||
            !KswordARKKernelCacheIsKernelPointer(endAddress) ||
            endAddress <= startAddress ||
            endAddress - startAddress > 0x40000000U ||
            !KswordARKKernelCacheTimeIsPlausible(
                currentTime.QuadPart,
                CurrentSystemTime)) {
            continue;
        }
        matches += 1UL;
    }
    return matches;
}

static BOOLEAN
KswordARKKernelCacheInferUnloadedLayout(
    _In_ ULONG_PTR PointerGlobal,
    _In_ ULONG_PTR ReferenceRoutine,
    _Out_ KSW_UNLOADED_LAYOUT_CANDIDATE* CandidateOut
    )
/*++

Routine Description:

    Infer the fixed 50-entry unloaded-driver record layout by requiring at
    least two rows with a driver suffix, ordered kernel range, and plausible
    system unload time.  A tied best layout is rejected.

Return Value:

    TRUE only for one strongest live layout.

--*/
{
    ULONG_PTR records = 0U;
    LARGE_INTEGER now;
    ULONG stride = 0UL;
    ULONG nameOffset = 0UL;
    ULONG bestMatches = 0UL;
    BOOLEAN tied = FALSE;
    KSW_UNLOADED_LAYOUT_CANDIDATE best;

    if (CandidateOut == NULL ||
        !KswordARKRuntimeReadMemory(
            (const VOID*)PointerGlobal,
            &records,
            sizeof(records)) ||
        !KswordARKKernelCacheIsKernelPointer(records) ||
        (records & (sizeof(PVOID) - 1U)) != 0U) {
        return FALSE;
    }
    RtlZeroMemory(&best, sizeof(best));
    KeQuerySystemTime(&now);

    for (stride = KSW_KERNEL_CACHE_MIN_UNLOADED_STRIDE;
         stride <= KSW_KERNEL_CACHE_MAX_UNLOADED_STRIDE;
         stride += sizeof(PVOID)) {
        for (nameOffset = 0UL;
             nameOffset + sizeof(UNICODE_STRING) +
                 (3UL * sizeof(ULONG_PTR)) <= stride;
             nameOffset += sizeof(PVOID)) {
            ULONG startOffset = nameOffset + sizeof(UNICODE_STRING);
            ULONG endOffset = startOffset + sizeof(PVOID);
            ULONG timeOffset = endOffset + sizeof(PVOID);
            ULONG matches = KswordARKKernelCacheCountUnloadedLayoutMatches(
                records,
                stride,
                nameOffset,
                startOffset,
                endOffset,
                timeOffset,
                now.QuadPart);

            if (matches < 2UL || matches < bestMatches) {
                continue;
            }
            if (matches == bestMatches && bestMatches != 0UL) {
                tied = TRUE;
                continue;
            }
            bestMatches = matches;
            tied = FALSE;
            best.PointerGlobal = PointerGlobal;
            best.Records = records;
            best.NameOffset = nameOffset;
            best.StartAddressOffset = startOffset;
            best.EndAddressOffset = endOffset;
            best.CurrentTimeOffset = timeOffset;
            best.RecordSize = stride;
            best.ValidRecordCount = matches;
            best.ReferenceRoutine = ReferenceRoutine;
        }
    }
    if (bestMatches < 2UL || tied) {
        return FALSE;
    }
    *CandidateOut = best;
    return TRUE;
}

BOOLEAN
KswordARKDriverResolveMmUnloadedLayout(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* NtoskrnlIdentity,
    _In_ ULONG PointerRva,
    _Out_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    )
{
    KSW_RUNTIME_IMAGE_VIEW view; // 本次查询的映像边界与节属性快照。
    KSW_UNLOADED_LAYOUT_CANDIDATE candidate; // 只发布唯一且多记录一致的布局。

    // OS build 防护已停用；继续校验 IRQL、模块身份与 RVA 边界。
    if (Layout == NULL || NtoskrnlIdentity == NULL ||
        // !KswordArkStartupIsOsBuildSupported() ||
        KeGetCurrentIrql() > APC_LEVEL ||
        NtoskrnlIdentity->present == 0UL || NtoskrnlIdentity->imageBase == 0ULL ||
        PointerRva == 0UL || PointerRva >= NtoskrnlIdentity->sizeOfImage ||
        NtoskrnlIdentity->imageBase > (~0ULL - PointerRva)) {
        return FALSE; // 不写任何不完整的布局结果。
    }
    RtlFillMemory(Layout, sizeof(*Layout), 0xFF); // 所有未解析成员保持 -1。
    RtlZeroMemory(&view, sizeof(view)); // 映像视图必须重新读取。
    RtlZeroMemory(&candidate, sizeof(candidate)); // 候选不继承旧查询结果。

    // 已知 RVA 仍必须位于当前映像可写数据节，且满足现有布局验证规则。
    if (!KswordARKRuntimeInitializeImageView(
            (PVOID)(ULONG_PTR)NtoskrnlIdentity->imageBase,
            NtoskrnlIdentity->sizeOfImage, &view) ||
        !KswordARKRuntimeAddressIsWritableData(
            &view, view.Base + PointerRva, sizeof(PVOID)) ||
        !KswordARKKernelCacheInferUnloadedLayout(
            view.Base + PointerRva, 0U, &candidate)) {
        return FALSE; // 空表、少于两条有效记录或歧义布局都保持不可用。
    }
    Layout->UldName = (LONG)candidate.NameOffset; // 运行时验证的名称偏移。
    Layout->UldStartAddress = (LONG)candidate.StartAddressOffset; // 已验证起址。
    Layout->UldEndAddress = (LONG)candidate.EndAddressOffset; // 已验证止址。
    Layout->UldCurrentTime = (LONG)candidate.CurrentTimeOffset; // 已验证时间。
    Layout->UldTypeSize = (LONG)candidate.RecordSize; // 已验证步长。
    return TRUE; // 不把推断得到的布局冒充 PDB 类型信息。
}

static VOID
KswordARKKernelCacheResolveUnloadedDrivers(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_reads_(ReferenceCount) const KSW_RUNTIME_DATA_REFERENCE* References,
    _In_ ULONG ReferenceCount,
    _Inout_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    )
{
    KSW_UNLOADED_LAYOUT_CANDIDATE best;
    ULONG bestScore = 0UL;
    BOOLEAN ambiguous = FALSE;
    ULONG index = 0UL;
    LONG lastIndexRva = -1;

    if (View == NULL || References == NULL || Layout == NULL) {
        return;
    }
    RtlZeroMemory(&best, sizeof(best));
    for (index = 0UL; index < ReferenceCount; ++index) {
        KSW_UNLOADED_LAYOUT_CANDIDATE candidate;

        RtlZeroMemory(&candidate, sizeof(candidate));
        if (!KswordARKRuntimeAddressIsWritableData(
                View,
                References[index].Address,
                sizeof(PVOID)) ||
            !KswordARKKernelCacheInferUnloadedLayout(
                References[index].Address,
                References[index].RoutineAddress,
                &candidate) ||
            candidate.ValidRecordCount < bestScore) {
            continue;
        }
        if (candidate.ValidRecordCount == bestScore && bestScore != 0UL) {
            if (candidate.PointerGlobal == best.PointerGlobal) { // 同一全局被不同函数引用不是新候选。
                continue; // 保留原有歧义状态，不能清除其它地址造成的歧义。
            }
            ambiguous = TRUE;
            continue;
        }
        best = candidate;
        bestScore = candidate.ValidRecordCount;
        ambiguous = FALSE;
    }
    if (bestScore < 2UL || ambiguous || best.PointerGlobal < View->Base ||
        best.PointerGlobal - View->Base > MAXLONG) {
        return;
    }

    for (index = 0UL; index < ReferenceCount; ++index) {
        ULONG value = 0UL;
        ULONG_PTR address = References[index].Address;

        if (References[index].RoutineAddress != best.ReferenceRoutine ||
            address == best.PointerGlobal ||
            !KswordARKRuntimeAddressIsWritableData(View, address, sizeof(value)) ||
            !KswordARKRuntimeReadMemory((const VOID*)address, &value, sizeof(value)) ||
            value > KSW_KERNEL_CACHE_MAX_UNLOADED_RECORDS ||
            address < View->Base || address - View->Base > MAXLONG) {
            continue;
        }
        if (lastIndexRva >= 0) {
            lastIndexRva = -1;
            break;
        }
        lastIndexRva = (LONG)(address - View->Base);
    }

    Layout->MmUnloadedDriversRva = (LONG)(best.PointerGlobal - View->Base);
    Layout->MmLastUnloadedDriverRva = lastIndexRva;
    Layout->UldName = (LONG)best.NameOffset;
    Layout->UldStartAddress = (LONG)best.StartAddressOffset;
    Layout->UldEndAddress = (LONG)best.EndAddressOffset;
    Layout->UldCurrentTime = (LONG)best.CurrentTimeOffset;
    Layout->UldTypeSize = (LONG)best.RecordSize;
}

static BOOLEAN
KswordARKKernelCacheAvlTableIsPlausible(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG_PTR Address,
    _Out_opt_ RTL_AVL_TABLE* SnapshotOut
    )
{
    RTL_AVL_TABLE table;
    ULONG_PTR childPointers[3];
    ULONG index = 0UL;

    RtlZeroMemory(&table, sizeof(table));
    if (View == NULL || (Address & (sizeof(PVOID) - 1U)) != 0U ||
        !KswordARKRuntimeAddressIsWritableData(View, Address, sizeof(table)) ||
        !KswordARKRuntimeReadMemory((const VOID*)Address, &table, sizeof(table)) ||
        table.NumberGenericTableElements == 0UL ||
        table.NumberGenericTableElements > 0x10000UL ||
        table.DepthOfTree == 0UL || table.DepthOfTree > 64UL ||
        table.WhichOrderedElement > table.NumberGenericTableElements ||
        !KswordARKRuntimeAddressIsExecutable(
            View,
            (ULONG_PTR)table.CompareRoutine,
            1U) ||
        !KswordARKRuntimeAddressIsExecutable(
            View,
            (ULONG_PTR)table.AllocateRoutine,
            1U) ||
        !KswordARKRuntimeAddressIsExecutable(
            View,
            (ULONG_PTR)table.FreeRoutine,
            1U)) {
        return FALSE;
    }
    childPointers[0] = (ULONG_PTR)table.BalancedRoot.Parent;
    childPointers[1] = (ULONG_PTR)table.BalancedRoot.LeftChild;
    childPointers[2] = (ULONG_PTR)table.BalancedRoot.RightChild;
    for (index = 0UL; index < RTL_NUMBER_OF(childPointers); ++index) {
        if (childPointers[index] != 0U &&
            !KswordARKKernelCacheIsKernelPointer(childPointers[index])) {
            return FALSE;
        }
    }
    if (table.OrderedPointer != NULL &&
        !KswordARKKernelCacheIsKernelPointer((ULONG_PTR)table.OrderedPointer)) {
        return FALSE;
    }
    if (table.RestartKey != NULL &&
        !KswordARKKernelCacheIsKernelPointer((ULONG_PTR)table.RestartKey)) {
        return FALSE;
    }
    if (SnapshotOut != NULL) {
        *SnapshotOut = table;
    }
    return TRUE;
}

static BOOLEAN
KswordARKKernelCacheOptionalPointerIsSane(
    _In_opt_ const VOID* Pointer
    )
{
    return Pointer == NULL ||
        KswordARKKernelCacheIsKernelPointer((ULONG_PTR)Pointer);
}

static BOOLEAN
KswordARKKernelCacheResourceIsPlausible(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG_PTR Address
    )
/*++

Routine Description:

    Cheap shape filter used while pairing candidates.  It narrows the search
    space only; it can never establish that the address is a real ERESOURCE,
    because every self-consistent LIST_ENTRY in a writable ntoskrnl section
    satisfies these conditions.  Identity is settled later by
    KswordARKKernelProbeResourceIsSystemResource.

Return Value:

    TRUE when the candidate is shaped like a resource.

--*/
{
    ERESOURCE resource;
    LIST_ENTRY forward;
    LIST_ENTRY backward;

    RtlZeroMemory(&resource, sizeof(resource));
    RtlZeroMemory(&forward, sizeof(forward));
    RtlZeroMemory(&backward, sizeof(backward));
    if (View == NULL || (Address & (sizeof(PVOID) - 1U)) != 0U ||
        !KswordARKRuntimeAddressIsWritableData(View, Address, sizeof(resource)) ||
        !KswordARKRuntimeReadMemory((const VOID*)Address, &resource, sizeof(resource)) ||
        !KswordARKKernelCacheIsKernelPointer(
            (ULONG_PTR)resource.SystemResourcesList.Flink) ||
        !KswordARKKernelCacheIsKernelPointer(
            (ULONG_PTR)resource.SystemResourcesList.Blink) ||
        (((ULONG_PTR)resource.SystemResourcesList.Flink |
          (ULONG_PTR)resource.SystemResourcesList.Blink) &
         (sizeof(PVOID) - 1U)) != 0U ||
        resource.ActiveCount < 0 ||
        !KswordARKKernelCacheOptionalPointerIsSane(resource.OwnerTable) ||
        !KswordARKKernelCacheOptionalPointerIsSane(resource.SharedWaiters) ||
        !KswordARKKernelCacheOptionalPointerIsSane(resource.ExclusiveWaiters) ||
        !KswordARKRuntimeReadMemory(
            resource.SystemResourcesList.Flink,
            &forward,
            sizeof(forward)) ||
        !KswordARKRuntimeReadMemory(
            resource.SystemResourcesList.Blink,
            &backward,
            sizeof(backward)) ||
        forward.Blink != (PLIST_ENTRY)Address ||
        backward.Flink != (PLIST_ENTRY)Address) {
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN
KswordARKKernelCacheCollectAvlEntries(
    _In_ const RTL_AVL_TABLE* Snapshot,
    _In_ ULONG_PTR TableAddress,
    _Out_writes_to_(Capacity, *CountOut) PVOID* Entries,
    _In_ ULONG Capacity,
    _Out_ ULONG* CountOut
    )
/*++

Routine Description:

    Walk the balanced tree with bounded fault-tolerant reads and collect user
    data pointers.  This replaces RtlGetElementGenericTableAvl, which follows
    every link without validation and additionally mutates OrderedPointer and
    WhichOrderedElement inside a table that is only held shared.  Parent back
    pointers, balance factors, and the element count must all agree, so a
    structure that merely resembles a table fails here instead of inside the
    Rtl walker.

Return Value:

    TRUE when the whole tree is intact and at least one element was collected.

--*/
{
    ULONG_PTR nodeStack[KSW_KERNEL_CACHE_MAX_AVL_DEPTH];
    ULONG_PTR parentStack[KSW_KERNEL_CACHE_MAX_AVL_DEPTH];
    ULONG depth = 0UL;
    ULONG visited = 0UL;
    ULONG collected = 0UL;
    ULONG_PTR sentinel = 0U;

    if (Snapshot == NULL || TableAddress == 0U || Entries == NULL ||
        Capacity == 0UL || CountOut == NULL) {
        return FALSE;
    }
    *CountOut = 0UL;
    if (Snapshot->NumberGenericTableElements == 0UL ||
        Snapshot->NumberGenericTableElements > KSW_KERNEL_CACHE_MAX_AVL_ELEMENTS ||
        Snapshot->BalancedRoot.LeftChild != NULL ||
        Snapshot->BalancedRoot.RightChild == NULL) {
        return FALSE;
    }
    RtlZeroMemory(nodeStack, sizeof(nodeStack));
    RtlZeroMemory(parentStack, sizeof(parentStack));
    sentinel = TableAddress + FIELD_OFFSET(RTL_AVL_TABLE, BalancedRoot);
    nodeStack[0] = (ULONG_PTR)Snapshot->BalancedRoot.RightChild;
    parentStack[0] = sentinel;
    depth = 1UL;

    while (depth != 0UL) {
        RTL_BALANCED_LINKS links;
        ULONG_PTR node = 0U;
        ULONG_PTR expectedParent = 0U;
        ULONG child = 0UL;
        ULONG_PTR children[2];

        depth -= 1UL;
        node = nodeStack[depth];
        expectedParent = parentStack[depth];
        if ((node & (sizeof(PVOID) - 1U)) != 0U ||
            !KswordARKKernelCacheIsKernelPointer(node)) {
            return FALSE;
        }
        RtlZeroMemory(&links, sizeof(links));
        if (!KswordARKRuntimeReadMemory((const VOID*)node, &links, sizeof(links)) ||
            (ULONG_PTR)links.Parent != expectedParent ||
            links.Balance < -1 || links.Balance > 1) {
            return FALSE;
        }
        visited += 1UL;
        if (visited > Snapshot->NumberGenericTableElements) {
            return FALSE;
        }
        if (collected < Capacity) {
            Entries[collected] = (PVOID)(node + sizeof(RTL_BALANCED_LINKS));
            collected += 1UL;
        }
        children[0] = (ULONG_PTR)links.LeftChild;
        children[1] = (ULONG_PTR)links.RightChild;
        for (child = 0UL; child < RTL_NUMBER_OF(children); ++child) {
            if (children[child] == 0U) {
                continue;
            }
            if (depth >= KSW_KERNEL_CACHE_MAX_AVL_DEPTH) {
                return FALSE;
            }
            nodeStack[depth] = children[child];
            parentStack[depth] = node;
            depth += 1UL;
        }
    }
    if (visited != Snapshot->NumberGenericTableElements) {
        return FALSE;
    }
    *CountOut = collected;
    return collected != 0UL;
}

static ULONG
KswordARKKernelCachePairScore(
    _In_ const KSW_RUNTIME_DATA_REFERENCE* TableReference,
    _In_ const KSW_RUNTIME_DATA_REFERENCE* LockReference
    )
{
    ULONG_PTR distance = 0U;
    ULONG score = 0UL;

    if (TableReference->RoutineAddress == LockReference->RoutineAddress) {
        score += 8UL;
    }
    distance = TableReference->InstructionAddress > LockReference->InstructionAddress
        ? TableReference->InstructionAddress - LockReference->InstructionAddress
        : LockReference->InstructionAddress - TableReference->InstructionAddress;
    if (distance <= 0x100UL) {
        score += 4UL;
    }
    else if (distance <= 0x800UL) {
        score += 2UL;
    }
    else if (distance <= 0x2000UL) {
        score += 1UL;
    }
    return score;
}

static BOOLEAN
KswordARKKernelCacheInferPiDdbEntryLayout(
    _In_reads_(SampleCount) PVOID const* Entries,
    _In_ ULONG SampleCount,
    _Out_ KSW_PIDDB_LAYOUT_CANDIDATE* Layout
    )
/*++

Routine Description:

    Infer PiDDB element fields while its validated resource is held.  Driver
    names must agree across multiple AVL elements; timestamp and status fields
    must form one unique adjacent semantic pair after the name descriptor.

Return Value:

    TRUE only for a unique bounded layout.

--*/
{
    ULONG sampleCount = 0UL;
    ULONG index = 0UL;
    ULONG nameOffset = 0UL;
    ULONG bestNameMatches = 0UL;
    LONG bestNameOffset = -1;
    BOOLEAN nameTied = FALSE;
    ULONG pairOffset = 0UL;
    ULONG bestPairMatches = 0UL;
    LONG bestPairOffset = -1;
    BOOLEAN pairTied = FALSE;

    if (Entries == NULL || Layout == NULL) {
        return FALSE;
    }
    sampleCount = min(SampleCount, KSW_KERNEL_CACHE_MAX_SAMPLE_ENTRIES);
    if (sampleCount < 2UL) {
        return FALSE;
    }
    for (index = 0UL; index < sampleCount; ++index) {
        if (Entries[index] == NULL ||
            !KswordARKKernelCacheIsKernelPointer((ULONG_PTR)Entries[index])) {
            return FALSE;
        }
    }

    for (nameOffset = 0UL;
         nameOffset + sizeof(UNICODE_STRING) <= KSW_KERNEL_CACHE_MAX_PRIVATE_ENTRY;
         nameOffset += sizeof(PVOID)) {
        ULONG matches = 0UL;

        for (index = 0UL; index < sampleCount; ++index) {
            if (KswordARKKernelCacheReadDriverName(
                    (const UCHAR*)Entries[index],
                    nameOffset,
                    NULL)) {
                matches += 1UL;
            }
        }
        if (matches < 2UL || matches < bestNameMatches) {
            continue;
        }
        if (matches == bestNameMatches && bestNameMatches != 0UL) {
            nameTied = TRUE;
            continue;
        }
        bestNameMatches = matches;
        bestNameOffset = (LONG)nameOffset;
        nameTied = FALSE;
    }
    if (bestNameOffset < 0 || nameTied) {
        return FALSE;
    }

    for (pairOffset = (ULONG)bestNameOffset + sizeof(UNICODE_STRING);
         pairOffset + (2UL * sizeof(ULONG)) <=
             min(KSW_KERNEL_CACHE_MAX_PRIVATE_ENTRY,
                 (ULONG)bestNameOffset + sizeof(UNICODE_STRING) + 0x20UL);
         pairOffset += sizeof(ULONG)) {
        ULONG matches = 0UL;

        for (index = 0UL; index < sampleCount; ++index) {
            ULONG timeDateStamp = 0UL;
            NTSTATUS loadStatus = STATUS_SUCCESS;

            if (!KswordARKRuntimeReadMemory(
                    (const UCHAR*)Entries[index] + pairOffset,
                    &timeDateStamp,
                    sizeof(timeDateStamp)) ||
                !KswordARKRuntimeReadMemory(
                    (const UCHAR*)Entries[index] + pairOffset + sizeof(ULONG),
                    &loadStatus,
                    sizeof(loadStatus)) ||
                timeDateStamp < 0x20000000UL ||
                !((loadStatus == STATUS_SUCCESS) ||
                  (((ULONG)loadStatus & 0xC0000000UL) == 0xC0000000UL) ||
                  (((ULONG)loadStatus & 0xC0000000UL) == 0x80000000UL))) {
                continue;
            }
            matches += 1UL;
        }
        if (matches < 2UL || matches < bestPairMatches) {
            continue;
        }
        if (matches == bestPairMatches && bestPairMatches != 0UL) {
            pairTied = TRUE;
            continue;
        }
        bestPairMatches = matches;
        bestPairOffset = (LONG)pairOffset;
        pairTied = FALSE;
    }
    if (bestPairOffset < 0 || pairTied) {
        return FALSE;
    }

    Layout->DriverNameOffset = (ULONG)bestNameOffset;
    Layout->TimeDateStampOffset = (ULONG)bestPairOffset;
    Layout->LoadStatusOffset = (ULONG)bestPairOffset + sizeof(ULONG);
    Layout->EntrySize = (Layout->LoadStatusOffset + sizeof(NTSTATUS) +
        (sizeof(PVOID) - 1UL)) & ~(sizeof(PVOID) - 1UL);
    return TRUE;
}

static VOID
KswordARKKernelCacheResolvePiDdb(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_reads_(ReferenceCount) const KSW_RUNTIME_DATA_REFERENCE* References,
    _In_ ULONG ReferenceCount,
    _Inout_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    )
{
    ULONG tableIndex = 0UL;
    ULONG bestScore = 0UL;
    BOOLEAN ambiguous = FALSE;
    const KSW_RUNTIME_DATA_REFERENCE* bestTableReference = NULL;
    const KSW_RUNTIME_DATA_REFERENCE* bestLockReference = NULL;
    KSW_PIDDB_LAYOUT_CANDIDATE candidate;
    BOOLEAN acquired = FALSE;

    if (View == NULL || References == NULL || Layout == NULL ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlZeroMemory(&candidate, sizeof(candidate));
    for (tableIndex = 0UL; tableIndex < ReferenceCount; ++tableIndex) {
        ULONG lockIndex = 0UL;

        if (!KswordARKKernelCacheAvlTableIsPlausible(
                View,
                References[tableIndex].Address,
                NULL)) {
            continue;
        }
        for (lockIndex = 0UL; lockIndex < ReferenceCount; ++lockIndex) {
            ULONG score = 0UL;

            if (lockIndex == tableIndex ||
                !KswordARKKernelCacheResourceIsPlausible(
                    View,
                    References[lockIndex].Address)) {
                continue;
            }
            score = KswordARKKernelCachePairScore(
                &References[tableIndex],
                &References[lockIndex]);
            if (score == 0UL || score < bestScore) {
                continue;
            }
            if (score == bestScore && bestScore != 0UL &&
                (bestTableReference->Address != References[tableIndex].Address ||
                 bestLockReference->Address != References[lockIndex].Address)) {
                ambiguous = TRUE;
                continue;
            }
            if (score > bestScore) {
                ambiguous = FALSE; // Duplicate references cannot clear a tied conflict.
            }
            bestScore = score;
            bestTableReference = &References[tableIndex];
            bestLockReference = &References[lockIndex];
        }
    }
    if (bestScore < 2UL || ambiguous || bestTableReference == NULL ||
        bestLockReference == NULL) {
        return;
    }

    candidate.Table = (PRTL_AVL_TABLE)bestTableReference->Address;
    candidate.Lock = (PERESOURCE)bestLockReference->Address;
    if (!KswordARKKernelProbeResourceIsSystemResource(
            (ULONG_PTR)candidate.Lock)) {
        return;
    }

    KeEnterCriticalRegion();
    acquired = ExAcquireResourceSharedLite(candidate.Lock, FALSE);
    if (acquired) {
        RTL_AVL_TABLE snapshot;
        PVOID entries[KSW_KERNEL_CACHE_MAX_SAMPLE_ENTRIES];
        ULONG entryCount = 0UL;

        RtlZeroMemory(&snapshot, sizeof(snapshot));
        RtlZeroMemory(entries, sizeof(entries));
        if (KswordARKKernelCacheAvlTableIsPlausible(
                View,
                (ULONG_PTR)candidate.Table,
                &snapshot) &&
            KswordARKKernelCacheCollectAvlEntries(
                &snapshot,
                (ULONG_PTR)candidate.Table,
                entries,
                RTL_NUMBER_OF(entries),
                &entryCount) &&
            KswordARKKernelCacheInferPiDdbEntryLayout(
                entries,
                entryCount,
                &candidate)) {
            if ((ULONG_PTR)candidate.Table >= View->Base &&
                (ULONG_PTR)candidate.Lock >= View->Base &&
                (ULONG_PTR)candidate.Table - View->Base <= MAXLONG &&
                (ULONG_PTR)candidate.Lock - View->Base <= MAXLONG) {
                Layout->PiDDBCacheTableRva =
                    (LONG)((ULONG_PTR)candidate.Table - View->Base);
                Layout->PiDDBLockRva =
                    (LONG)((ULONG_PTR)candidate.Lock - View->Base);
                Layout->PiDdbDriverName = (LONG)candidate.DriverNameOffset;
                Layout->PiDdbTimeDateStamp =
                    (LONG)candidate.TimeDateStampOffset;
                Layout->PiDdbLoadStatus = (LONG)candidate.LoadStatusOffset;
                Layout->PiDdbTypeSize = (LONG)candidate.EntrySize;
            }
        }
        ExReleaseResourceLite(candidate.Lock);
    }
    KeLeaveCriticalRegion();
}

NTSTATUS
KswordARKDriverResolvePiDdbLayout(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* NtoskrnlIdentity,
    _In_ ULONG TableRva,
    _In_ ULONG LockRva,
    _Out_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    )
{
    KSW_RUNTIME_IMAGE_VIEW view;
    KSW_PIDDB_LAYOUT_CANDIDATE candidate;
    RTL_AVL_TABLE snapshot;
    PVOID entries[KSW_KERNEL_CACHE_MAX_SAMPLE_ENTRIES];
    ULONG entryCount = 0UL;
    BOOLEAN acquired;
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    if (Layout == NULL || NtoskrnlIdentity == NULL ||
        NtoskrnlIdentity->present == 0UL || NtoskrnlIdentity->imageBase == 0ULL ||
        NtoskrnlIdentity->sizeOfImage == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return STATUS_INVALID_DEVICE_STATE; }
    RtlZeroMemory(&view, sizeof(view));
    RtlZeroMemory(&candidate, sizeof(candidate));
    RtlZeroMemory(&snapshot, sizeof(snapshot));
    RtlZeroMemory(entries, sizeof(entries));
    if (!KswordARKRuntimeInitializeImageView((PVOID)(ULONG_PTR)NtoskrnlIdentity->imageBase,
            NtoskrnlIdentity->sizeOfImage, &view) || TableRva >= view.Size || LockRva >= view.Size) {
        return STATUS_NOT_SUPPORTED;
    }
    candidate.Table = (PRTL_AVL_TABLE)(view.Base + TableRva);
    candidate.Lock = (PERESOURCE)(view.Base + LockRva);
    if (!KswordARKKernelCacheAvlTableIsPlausible(&view, (ULONG_PTR)candidate.Table, NULL) ||
        !KswordARKKernelCacheResourceIsPlausible(&view, (ULONG_PTR)candidate.Lock)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKKernelProbeResourceIsSystemResource((ULONG_PTR)candidate.Lock)) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    KeEnterCriticalRegion();
    acquired = ExAcquireResourceSharedLite(candidate.Lock, FALSE);
    if (acquired) {
        if (!KswordARKKernelCacheAvlTableIsPlausible(&view, (ULONG_PTR)candidate.Table, &snapshot) ||
            !KswordARKKernelCacheCollectAvlEntries(&snapshot, (ULONG_PTR)candidate.Table,
                entries, RTL_NUMBER_OF(entries), &entryCount)) {
            status = STATUS_DATA_ERROR;
        }
        else if (KswordARKKernelCacheInferPiDdbEntryLayout(entries, entryCount, &candidate)) {
            Layout->PiDDBCacheTableRva = (LONG)TableRva;
            Layout->PiDDBLockRva = (LONG)LockRva;
            Layout->PiDdbDriverName = (LONG)candidate.DriverNameOffset;
            Layout->PiDdbTimeDateStamp = (LONG)candidate.TimeDateStampOffset;
            Layout->PiDdbLoadStatus = (LONG)candidate.LoadStatusOffset;
            Layout->PiDdbTypeSize = (LONG)candidate.EntrySize;
            status = STATUS_SUCCESS;
        }
        ExReleaseResourceLite(candidate.Lock);
    }
    else { status = STATUS_LOCK_NOT_GRANTED; }
    KeLeaveCriticalRegion();
    return status;
}

VOID
KswordARKDriverResolveKernelCacheFallback(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* NtoskrnlIdentity,
    _Inout_ PKSW_RUNTIME_KERNEL_LAYOUT Layout
    )
/*++

Routine Description:

    Resolve both ntoskrnl caches from stable exported call graphs.  The two
    features are independent: one may remain unavailable without weakening the
    other feature's validation.

Return Value:

    None.  Unresolved signed members retain the caller's -1 sentinel.

--*/
{
    static PCSTR const anchors[] = {
        "MmUnloadSystemImage",
        "MmLoadSystemImage",
        "NtLoadDriver",
        "NtUnloadDriver"
    };
    KSW_RUNTIME_IMAGE_VIEW view;
    KSW_RUNTIME_DATA_REFERENCE* references = NULL;
    ULONG referenceCount = 0UL;

    if (NtoskrnlIdentity == NULL || Layout == NULL ||
        NtoskrnlIdentity->present == 0UL ||
        NtoskrnlIdentity->imageBase == 0ULL ||
        NtoskrnlIdentity->sizeOfImage == 0UL ||
        KeGetCurrentIrql() > APC_LEVEL) {
        return;
    }
    RtlZeroMemory(&view, sizeof(view));
    if (!KswordARKRuntimeInitializeImageView(
            (PVOID)(ULONG_PTR)NtoskrnlIdentity->imageBase,
            NtoskrnlIdentity->sizeOfImage,
            &view)) {
        return;
    }
    references = (KSW_RUNTIME_DATA_REFERENCE*)KswordARKAllocateNonPagedPool(
        KSW_KERNEL_CACHE_MAX_REFERENCES * sizeof(*references),
        KSW_KERNEL_CACHE_TAG);
    if (references == NULL) {
        return;
    }
    referenceCount = KswordARKRuntimeCollectFunctionDataReferences( // 使用 PE 函数边界避免相邻代码耗尽遍历预算。
        &view,
        anchors,
        RTL_NUMBER_OF(anchors),
        4UL,
        KSW_KERNEL_CACHE_ROUTINE_SCAN_BYTES,
        references,
        KSW_KERNEL_CACHE_MAX_REFERENCES);
    if (referenceCount != 0UL) {
        KswordARKKernelCacheResolveUnloadedDrivers(
            &view,
            references,
            referenceCount,
            Layout);
        KswordARKKernelCacheResolvePiDdb(
            &view,
            references,
            referenceCount,
            Layout);
    }
    ExFreePoolWithTag(references, KSW_KERNEL_CACHE_TAG);
}
