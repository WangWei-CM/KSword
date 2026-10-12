/*++

Module Name:

    ci_hash_fallback.c

Abstract:

    Runtime-signature fallback for the CI kernel hash cache.  Stable exported
    CI entry points seed a bounded direct-call scan.  Candidate list globals,
    entry layouts, and ERESOURCE objects are accepted only after unique live
    chain/name validation and a non-blocking locked revalidation.

    A candidate lock must be proven to sit on the global resource list before
    it reaches ExAcquireResourceSharedLite.  Field-shape checks cannot settle
    that question, and this module's candidate pool is wider than the anchored
    scan alone: it falls back to a whole-image data-reference sweep.

Environment:

    Kernel mode, PASSIVE_LEVEL read-only query path.

--*/

#include "ci_hash_fallback.h"
#include "hook_scan_support.h"
#include "../../platform/kernel_object_probe.h"
#include "../../platform/runtime_signature_scan.h"
#include "../../platform/pool_compat.h"

#define KSW_CI_HASH_FALLBACK_TAG 'hCsK'
#define KSW_CI_HASH_MAX_REFERENCES 768UL
#define KSW_CI_HASH_ROUTINE_SCAN_BYTES 0x0800UL
#define KSW_CI_HASH_FULL_SCAN_BUDGET 0x00400000UL
#define KSW_CI_HASH_MAX_ENTRY_BYTES 0x0100UL
#define KSW_CI_HASH_MAX_CHAIN 64UL
#define KSW_CI_HASH_MAX_SAMPLES 16UL
#define KSW_CI_HASH_MAX_NAME_BYTES 520U

typedef struct _KSW_CI_HASH_CANDIDATE
{
    ULONG_PTR ListGlobal;
    ULONG_PTR ReferenceRoutine;
    ULONG_PTR ReferenceInstruction;
    ULONG NextOffset;
    ULONG NameOffset;
    ULONG ChainLength;
    PVOID Samples[KSW_CI_HASH_MAX_SAMPLES];
    ULONG SampleCount;
} KSW_CI_HASH_CANDIDATE, *PKSW_CI_HASH_CANDIDATE;

static VOID
KswordARKCiHashSelectListCandidate(
    _In_ const KSW_CI_HASH_CANDIDATE* Candidate,
    _Inout_ KSW_CI_HASH_CANDIDATE* Best,
    _Inout_ ULONG* BestScore,
    _Inout_ BOOLEAN* Ambiguous
    )
/*++

Routine Description:

    Preserve tied conflicts across duplicate references to the same list and
    layout. Only a strictly longer validated chain can remove the conflict.

--*/
{
    if (Candidate->ChainLength < *BestScore) { // 较短链不改变当前最强候选或歧义状态。
        return; // 保留先前已经成立的证据。
    } // 结束较弱候选过滤。
    if (Candidate->ChainLength == *BestScore && *BestScore != 0UL &&
        (Candidate->ListGlobal != Best->ListGlobal ||
         Candidate->NextOffset != Best->NextOffset ||
         Candidate->NameOffset != Best->NameOffset)) { // 等长但不同链或布局形成真实歧义。
        *Ambiguous = TRUE; // 歧义保持到出现严格更强的证据。
        return; // 不用矛盾候选覆盖当前记录。
    } // 结束同分矛盾候选处理。
    if (Candidate->ChainLength > *BestScore) { // 只有更强的链证据可以消除既有等分歧义。
        *Ambiguous = FALSE; // 同一全局的重复引用不能抹掉另一条矛盾链。
    } // 结束严格更强候选的歧义重置。
    *Best = *Candidate; // 保留最新的同一布局引用以供锁配对。
    *BestScore = Candidate->ChainLength; // 更新当前最强链长度。
}

static VOID
KswordARKCiHashSelectLockReference(
    _In_ const KSW_RUNTIME_DATA_REFERENCE* Candidate,
    _In_ ULONG PairScore,
    _Inout_ const KSW_RUNTIME_DATA_REFERENCE** Best,
    _Inout_ ULONG* BestScore,
    _Inout_ BOOLEAN* Ambiguous
    )
/*++

Routine Description:

    Score lock identities without allowing duplicate references to clear a
    tied conflict. A stronger list/lock association can establish a new best.

--*/
{
    if (PairScore == 0UL || PairScore < *BestScore) { // 未关联或更弱的锁不影响选择结果。
        return; // 保留原来的锁候选与歧义状态。
    } // 结束无效或较弱配对过滤。
    if (PairScore == *BestScore && *BestScore != 0UL && *Best != NULL &&
        (*Best)->Address != Candidate->Address) { // 只比较真实锁地址，不把代码引用地址当锁身份。
        *Ambiguous = TRUE; // 不同锁等分时必须拒绝后续 acquire。
        return; // 不用矛盾锁替换已记录锁。
    } // 结束同分锁身份冲突。
    if (PairScore > *BestScore) { // 只有更高配对分数可以重新证明锁候选唯一。
        *Ambiguous = FALSE; // 重复引用同一锁时保留此前的等分锁冲突。
    } // 结束严格更强锁配对的歧义重置。
    *Best = Candidate; // 保存当前候选引用，地址仍需通过系统 resource 身份验证。
    *BestScore = PairScore; // 更新最强关联分数。
}

static BOOLEAN
KswordARKCiHashIsKernelAddress(
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
KswordARKCiHashNameIsDriver(
    _In_ const UCHAR* Entry,
    _In_ ULONG Offset
    )
{
    UNICODE_STRING name;
    UNICODE_STRING expected;
    UNICODE_STRING tailString;
    WCHAR tail[5];
    SIZE_T suffixBytes = 0U;
    BOOLEAN matched = FALSE;

    if (Entry == NULL) {
        return FALSE;
    }
    RtlZeroMemory(&name, sizeof(name));
    RtlZeroMemory(tail, sizeof(tail));
    if (!KswordARKRuntimeReadMemory(
            Entry + Offset,
            &name,
            sizeof(name)) ||
        name.Buffer == NULL ||
        !KswordARKCiHashIsKernelAddress((ULONG_PTR)name.Buffer) ||
        name.Length == 0U || name.Length > KSW_CI_HASH_MAX_NAME_BYTES ||
        name.Length > name.MaximumLength ||
        (name.Length & (sizeof(WCHAR) - 1U)) != 0U) {
        return FALSE;
    }

    RtlInitUnicodeString(&expected, L".sys");
    suffixBytes = expected.Length;
    if (name.Length >= suffixBytes &&
        KswordARKRuntimeReadMemory(
            (const UCHAR*)name.Buffer + name.Length - suffixBytes,
            tail,
            suffixBytes)) {
        tailString.Buffer = tail;
        tailString.Length = (USHORT)suffixBytes;
        tailString.MaximumLength = (USHORT)suffixBytes;
        matched = RtlEqualUnicodeString(&tailString, &expected, TRUE);
    }
    if (matched) {
        return TRUE;
    }

    RtlZeroMemory(tail, sizeof(tail));
    RtlInitUnicodeString(&expected, L".dll");
    suffixBytes = expected.Length;
    if (name.Length < suffixBytes ||
        !KswordARKRuntimeReadMemory(
            (const UCHAR*)name.Buffer + name.Length - suffixBytes,
            tail,
            suffixBytes)) {
        return FALSE;
    }
    tailString.Buffer = tail;
    tailString.Length = (USHORT)suffixBytes;
    tailString.MaximumLength = (USHORT)suffixBytes;
    return RtlEqualUnicodeString(&tailString, &expected, TRUE);
}

static BOOLEAN
KswordARKCiHashWalkCandidate(
    _In_ ULONG_PTR ListGlobal,
    _In_ ULONG NextOffset,
    _In_ ULONG NameOffset,
    _Out_opt_ KSW_CI_HASH_CANDIDATE* CandidateOut
    )
{
    ULONG_PTR current = 0U;
    ULONG_PTR seen[KSW_CI_HASH_MAX_CHAIN];
    ULONG count = 0UL;
    KSW_CI_HASH_CANDIDATE candidate;

    RtlZeroMemory(seen, sizeof(seen));
    RtlZeroMemory(&candidate, sizeof(candidate));
    if (!KswordARKRuntimeReadMemory(
            (const VOID*)ListGlobal,
            &current,
            sizeof(current)) ||
        !KswordARKCiHashIsKernelAddress(current)) {
        return FALSE;
    }
    while (current != 0U && count < KSW_CI_HASH_MAX_CHAIN) {
        ULONG_PTR next = 0U;
        ULONG seenIndex = 0UL;

        for (seenIndex = 0UL; seenIndex < count; ++seenIndex) {
            if (seen[seenIndex] == current) {
                return FALSE;
            }
        }
        if (!KswordARKCiHashNameIsDriver(
                (const UCHAR*)current,
                NameOffset) ||
            !KswordARKRuntimeReadMemory(
                (const UCHAR*)current + NextOffset,
                &next,
                sizeof(next)) ||
            (next != 0U && !KswordARKCiHashIsKernelAddress(next))) {
            return FALSE;
        }
        seen[count] = current;
        if (count < KSW_CI_HASH_MAX_SAMPLES) {
            candidate.Samples[count] = (PVOID)current;
            candidate.SampleCount += 1UL;
        }
        count += 1UL;
        current = next;
    }
    if (count < 2UL || current != 0U) {
        return FALSE;
    }
    candidate.ListGlobal = ListGlobal;
    candidate.NextOffset = NextOffset;
    candidate.NameOffset = NameOffset;
    candidate.ChainLength = count;
    if (CandidateOut != NULL) {
        *CandidateOut = candidate;
    }
    return TRUE;
}

static BOOLEAN
KswordARKCiHashInferListCandidate(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ const KSW_RUNTIME_DATA_REFERENCE* Reference,
    _Out_ KSW_CI_HASH_CANDIDATE* CandidateOut
    )
{
    KSW_CI_HASH_CANDIDATE best;
    ULONG bestLength = 0UL;
    BOOLEAN ambiguous = FALSE;
    ULONG nextOffset = 0UL;

    if (View == NULL || Reference == NULL || CandidateOut == NULL ||
        !KswordARKRuntimeAddressIsWritableData(
            View,
            Reference->Address,
            sizeof(PVOID))) {
        return FALSE;
    }
    RtlZeroMemory(&best, sizeof(best));
    for (nextOffset = 0UL;
         nextOffset + sizeof(PVOID) <= KSW_CI_HASH_MAX_ENTRY_BYTES;
         nextOffset += sizeof(PVOID)) {
        ULONG nameOffset = 0UL;

        for (nameOffset = 0UL;
             nameOffset + sizeof(UNICODE_STRING) <= KSW_CI_HASH_MAX_ENTRY_BYTES;
             nameOffset += sizeof(PVOID)) {
            KSW_CI_HASH_CANDIDATE candidate;

            if (nameOffset == nextOffset) {
                continue;
            }
            RtlZeroMemory(&candidate, sizeof(candidate));
            if (!KswordARKCiHashWalkCandidate(
                    Reference->Address,
                    nextOffset,
                    nameOffset,
                    &candidate) ||
                candidate.ChainLength < bestLength) {
                continue;
            }
            if (candidate.ChainLength == bestLength && bestLength != 0UL) {
                ambiguous = TRUE;
                continue;
            }
            best = candidate;
            bestLength = candidate.ChainLength;
            ambiguous = FALSE;
        }
    }
    if (bestLength < 2UL || ambiguous) {
        return FALSE;
    }
    best.ReferenceRoutine = Reference->RoutineAddress;
    best.ReferenceInstruction = Reference->InstructionAddress;
    *CandidateOut = best;
    return TRUE;
}

static BOOLEAN
KswordARKCiHashOptionalPointerIsSane(
    _In_opt_ const VOID* Pointer
    )
{
    return Pointer == NULL ||
        KswordARKCiHashIsKernelAddress((ULONG_PTR)Pointer);
}

static BOOLEAN
KswordARKCiHashResourceIsPlausible(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG_PTR Address
    )
/*++

Routine Description:

    Cheap shape filter used while pairing candidates.  It narrows the search
    space only; it can never establish that the address is a real ERESOURCE,
    because every self-consistent LIST_ENTRY in a writable image section
    satisfies these conditions.  Identity is settled by
    KswordARKKernelProbeResourceIsSystemResource before the lock is acquired.

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
    if ((Address & (sizeof(PVOID) - 1U)) != 0U ||
        !KswordARKRuntimeAddressIsWritableData(View, Address, sizeof(resource)) ||
        !KswordARKRuntimeReadMemory((const VOID*)Address, &resource, sizeof(resource)) ||
        !KswordARKCiHashIsKernelAddress(
            (ULONG_PTR)resource.SystemResourcesList.Flink) ||
        !KswordARKCiHashIsKernelAddress(
            (ULONG_PTR)resource.SystemResourcesList.Blink) ||
        (((ULONG_PTR)resource.SystemResourcesList.Flink |
          (ULONG_PTR)resource.SystemResourcesList.Blink) &
         (sizeof(PVOID) - 1U)) != 0U ||
        resource.ActiveCount < 0 ||
        !KswordARKCiHashOptionalPointerIsSane(resource.OwnerTable) ||
        !KswordARKCiHashOptionalPointerIsSane(resource.SharedWaiters) ||
        !KswordARKCiHashOptionalPointerIsSane(resource.ExclusiveWaiters) ||
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

static ULONG
KswordARKCiHashPairScore(
    _In_ const KSW_CI_HASH_CANDIDATE* ListCandidate,
    _In_ const KSW_RUNTIME_DATA_REFERENCE* LockReference
    )
{
    ULONG_PTR distance = ListCandidate->ReferenceInstruction >
            LockReference->InstructionAddress
        ? ListCandidate->ReferenceInstruction - LockReference->InstructionAddress
        : LockReference->InstructionAddress - ListCandidate->ReferenceInstruction;
    ULONG score = 0UL;

    if (ListCandidate->ReferenceRoutine == LockReference->RoutineAddress) {
        score += 8UL;
    }
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
KswordARKCiHashFindModule(
    _In_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* Modules,
    _Out_ const KSW_HOOK_SYSTEM_MODULE_ENTRY** ModuleOut
    )
{
    ULONG index = 0UL;

    if (Modules == NULL || ModuleOut == NULL) {
        return FALSE;
    }
    *ModuleOut = NULL;
    for (index = 0UL; index < Modules->NumberOfModules; ++index) {
        const UCHAR* fileName = NULL;
        ULONG fileNameBytes = 0UL;

        KswordARKHookGetModuleFileName(
            &Modules->Modules[index],
            &fileName,
            &fileNameBytes);
        if (KswordARKHookBoundedAnsiEqualsInsensitive(
                fileName,
                fileNameBytes,
                "ci.dll") ||
            KswordARKHookBoundedAnsiEqualsInsensitive(
                fileName,
                fileNameBytes,
                "ci.sys")) {
            *ModuleOut = &Modules->Modules[index];
            return TRUE;
        }
    }
    return FALSE;
}

NTSTATUS
KswordARKCiHashResolveRuntimeLayout(
    _Out_ KSW_DYN_V4_CI_KERNEL_HASH_LAYOUT* LayoutOut
    )
{
    static PCSTR const anchors[] = {
        "CiInitialize",
        "CiValidateFileObject",
        "CiCheckSignedFile",
        "CiVerifyHashInCatalog"
    };
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* modules = NULL;
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* ciModule = NULL;
    ULONG moduleBytes = 0UL;
    KSW_RUNTIME_IMAGE_VIEW view;
    KSW_RUNTIME_DATA_REFERENCE* references = NULL;
    ULONG referenceCount = 0UL;
    KSW_CI_HASH_CANDIDATE bestList;
    const KSW_RUNTIME_DATA_REFERENCE* bestLock = NULL;
    KSW_RUNTIME_DATA_REFERENCE bestLockStorage;
    BOOLEAN fullScanNeeded = FALSE;
    ULONG bestChainLength = 0UL;
    ULONG bestPairScore = 0UL;
    BOOLEAN listAmbiguous = FALSE;
    BOOLEAN pairAmbiguous = FALSE;
    ULONG referenceIndex = 0UL;
    BOOLEAN lockAcquired = FALSE;
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    if (LayoutOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(LayoutOut, sizeof(*LayoutOut));
    LayoutOut->EntryTimeDateStamp = KSW_DYN_OFFSET_UNAVAILABLE;
    LayoutOut->EntryLoadStatus = KSW_DYN_OFFSET_UNAVAILABLE;
    LayoutOut->EntryImageBase = KSW_DYN_OFFSET_UNAVAILABLE;
    LayoutOut->EntryImageSize = KSW_DYN_OFFSET_UNAVAILABLE;
    RtlZeroMemory(&view, sizeof(view));
    RtlZeroMemory(&bestList, sizeof(bestList));
    RtlZeroMemory(&bestLockStorage, sizeof(bestLockStorage));
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = KswordARKHookBuildModuleSnapshot(&modules, &moduleBytes);
    if (!NT_SUCCESS(status) || !KswordARKCiHashFindModule(modules, &ciModule) ||
        !KswordARKRuntimeInitializeImageView(
            ciModule->ImageBase,
            ciModule->ImageSize,
            &view)) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }

    references = (KSW_RUNTIME_DATA_REFERENCE*)KswordARKAllocateNonPagedPool(
        KSW_CI_HASH_MAX_REFERENCES * sizeof(*references),
        KSW_CI_HASH_FALLBACK_TAG);
    if (references == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    referenceCount = KswordARKRuntimeCollectFunctionDataReferences( // PE 函数边界防止相邻例程引用污染 CI 候选集。
        &view,
        anchors,
        RTL_NUMBER_OF(anchors),
        4UL,
        KSW_CI_HASH_ROUTINE_SCAN_BYTES,
        references,
        KSW_CI_HASH_MAX_REFERENCES);
    for (referenceIndex = 0UL; referenceIndex < referenceCount; ++referenceIndex) {
        KSW_CI_HASH_CANDIDATE candidate;
        RtlZeroMemory(&candidate, sizeof(candidate));
        if (KswordARKCiHashInferListCandidate(&view, &references[referenceIndex], &candidate)) {
            KswordARKCiHashSelectListCandidate(&candidate, &bestList, &bestChainLength, &listAmbiguous);
        }
    }
    // Nonempty anchor results can still omit the cache. Resume through later
    // executable sections rather than truncating everything at the first 768 refs.
    fullScanNeeded = bestChainLength < 2UL || listAmbiguous;
    if (fullScanNeeded) {
        ULONG cursorRva = 0UL;
        ULONG scannedTotal = 0UL;
        while (cursorRva < view.Size && scannedTotal < KSW_CI_HASH_FULL_SCAN_BUDGET) {
            ULONG scanned = 0UL;
            referenceCount = KswordARKRuntimeCollectExecutableDataReferenceBatch(&view,
                min(0x10000UL, KSW_CI_HASH_FULL_SCAN_BUDGET - scannedTotal),
                &cursorRva, references, KSW_CI_HASH_MAX_REFERENCES, &scanned);
            scannedTotal += scanned;
            for (referenceIndex = 0UL; referenceIndex < referenceCount; ++referenceIndex) {
                KSW_CI_HASH_CANDIDATE candidate;
                RtlZeroMemory(&candidate, sizeof(candidate));
                if (KswordARKCiHashInferListCandidate(&view, &references[referenceIndex], &candidate)) {
                    KswordARKCiHashSelectListCandidate(&candidate, &bestList, &bestChainLength, &listAmbiguous);
                }
            }
            if (scanned == 0UL) { break; }
        }
    }
    if (bestChainLength < 2UL || listAmbiguous) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }

    referenceCount = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors,
        RTL_NUMBER_OF(anchors), 4UL, KSW_CI_HASH_ROUTINE_SCAN_BYTES,
        references, KSW_CI_HASH_MAX_REFERENCES);
    // Revisit references after list selection, retaining the selected lock by
    // value because the reference buffer is reused for each scan batch.
    {
        ULONG cursorRva = 0UL;
        ULONG scannedTotal = 0UL;
        BOOLEAN anchorsPending = TRUE;
        for (;;) {
            for (referenceIndex = 0UL; referenceIndex < referenceCount; ++referenceIndex) {
                ULONG pairScore;
                if (references[referenceIndex].Address == bestList.ListGlobal ||
                    !KswordARKCiHashResourceIsPlausible(&view, references[referenceIndex].Address)) {
                    continue;
                }
                pairScore = KswordARKCiHashPairScore(&bestList, &references[referenceIndex]);
                KswordARKCiHashSelectLockReference(&references[referenceIndex], pairScore,
                    &bestLock, &bestPairScore, &pairAmbiguous);
                if (bestLock != NULL && bestLock != &bestLockStorage) {
                    bestLockStorage = *bestLock;
                    bestLock = &bestLockStorage;
                }
            }
            if (anchorsPending) {
                anchorsPending = FALSE;
                if (!fullScanNeeded && bestLock != NULL && !pairAmbiguous && bestPairScore >= 2UL) { break; }
            }
            if (cursorRva >= view.Size || scannedTotal >= KSW_CI_HASH_FULL_SCAN_BUDGET) { break; }
            {
                ULONG scanned = 0UL;
                referenceCount = KswordARKRuntimeCollectExecutableDataReferenceBatch(&view,
                    min(0x10000UL, KSW_CI_HASH_FULL_SCAN_BUDGET - scannedTotal),
                    &cursorRva, references, KSW_CI_HASH_MAX_REFERENCES, &scanned);
                scannedTotal += scanned;
                if (scanned == 0UL) { break; }
            }
        }
    }
    if (bestLock == NULL || bestPairScore < 2UL || pairAmbiguous) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }

    /*
     * Shape checks got us a candidate; only global-resource-list membership
     * proves it is a resource.  Handing a non-resource to the Ex API faults
     * inside the kernel's lock path, so this gate precedes the acquire.
     */
    if (!KswordARKKernelProbeResourceIsSystemResource(bestLock->Address)) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }

    KeEnterCriticalRegion();
    status = STATUS_LOCK_NOT_GRANTED;
    lockAcquired = ExAcquireResourceSharedLite(
        (PERESOURCE)bestLock->Address,
        FALSE);
    if (lockAcquired) {
        KSW_CI_HASH_CANDIDATE lockedCandidate;
        status = STATUS_DATA_ERROR;

        RtlZeroMemory(&lockedCandidate, sizeof(lockedCandidate));
        if (KswordARKCiHashWalkCandidate(
                bestList.ListGlobal,
                bestList.NextOffset,
                bestList.NameOffset,
                &lockedCandidate) &&
            lockedCandidate.ChainLength == bestList.ChainLength &&
            bestList.ListGlobal >= view.Base &&
            bestLock->Address >= view.Base &&
            bestList.ListGlobal - view.Base <= MAXULONG &&
            bestLock->Address - view.Base <= MAXULONG) {
            LayoutOut->ModuleBase = view.Base;
            LayoutOut->ModuleSize = view.Size;
            LayoutOut->KernelHashBucketListRva =
                (ULONG)(bestList.ListGlobal - view.Base);
            LayoutOut->HashCacheLockRva =
                (ULONG)(bestLock->Address - view.Base);
            LayoutOut->EntryNext = bestList.NextOffset;
            LayoutOut->EntryDriverName = bestList.NameOffset;
            LayoutOut->EntryTypeSize = max(
                LayoutOut->EntryNext + (ULONG)sizeof(PVOID),
                LayoutOut->EntryDriverName + (ULONG)sizeof(UNICODE_STRING));
            LayoutOut->EntryTypeSize =
                (LayoutOut->EntryTypeSize + (sizeof(PVOID) - 1UL)) &
                ~(sizeof(PVOID) - 1UL);
            status = STATUS_SUCCESS;
        }
        ExReleaseResourceLite((PERESOURCE)bestLock->Address);
    }
    KeLeaveCriticalRegion();

Exit:
    if (references != NULL) {
        ExFreePoolWithTag(references, KSW_CI_HASH_FALLBACK_TAG);
    }
    if (modules != NULL) {
        ExFreePoolWithTag(modules, KSW_HOOK_SCAN_TAG);
    }
    UNREFERENCED_PARAMETER(moduleBytes);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(LayoutOut, sizeof(*LayoutOut));
    }
    return status;
}
