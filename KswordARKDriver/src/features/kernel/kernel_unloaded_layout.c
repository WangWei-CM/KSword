/*++
    Per-source unloaded-driver layouts. Exact PDB globals and live-validated
    runtime fields remain usable without a complete System Informer profile.
--*/
#include "kernel_unloaded_layout.h"
#include "ark/ark_dyndata.h"
#include "../../platform/kernel_cache_fallback.h"
#include "../../platform/runtime_signature_scan.h"

#define KSW_UNLOADED_DRIVER_MAX_ENTRY_BYTES 4096UL

static BOOLEAN
KswordARKUnloadedDynDataSourceIsTrusted(
    _In_ ULONG Source
    )
{
    return Source == KSW_DYN_FIELD_SOURCE_PDB_PROFILE ||
        Source == KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
}

static BOOLEAN
KswordARKUnloadedRvaToAddress(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* Identity,
    _In_ ULONG Rva,
    _In_ SIZE_T RequiredBytes,
    _Out_ ULONGLONG* AddressOut
    )
/*++

Routine Description:

    Convert one identity-matched module RVA into a bounded live kernel address.

Return Value:

    TRUE when the complete requested range lies inside the current image.

--*/
{
    if (Identity == NULL || AddressOut == NULL ||
        Identity->present == 0UL || Identity->imageBase == 0ULL ||
        Identity->sizeOfImage == 0UL || Rva == 0UL ||
        Rva == KSW_DYN_OFFSET_UNAVAILABLE || Rva >= Identity->sizeOfImage ||
        RequiredBytes > (SIZE_T)(Identity->sizeOfImage - Rva) ||
        Identity->imageBase > (~0ULL - Rva)) {
        return FALSE;
    }

    *AddressOut = Identity->imageBase + Rva;
    return TRUE;
}

NTSTATUS
KswordARKUnloadedResolveMmLayout(
    _Out_ KSW_MM_UNLOADED_LAYOUT* Layout
    )
/*++

Routine Description:

    Resolve the MmUnloadedDrivers pointer and exact _UNLOADED_DRIVERS member
    layout from trusted PDB/runtime fields, completing a missing private layout
    with multi-record live validation when its exact global RVA is available.

Return Value:

    STATUS_SUCCESS, STATUS_DEVICE_NOT_READY for missing initialization/identity, or
    STATUS_NOT_SUPPORTED for an incomplete/invalid layout.

--*/
{
    KSW_DYN_STATE state;
    ULONGLONG recordsPointerAddress = 0ULL;
    PVOID records = NULL;

    if (Layout == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Layout, sizeof(*Layout));
    RtlZeroMemory(&state, sizeof(state));
    KswordARKDynDataSnapshot(&state);

    // 整体 profile 未命中不等于本来源不可用；仍需初始化完成且内核身份有效。
    if (!state.Initialized || state.Ntoskrnl.present == 0UL ||
        state.Ntoskrnl.imageBase == 0ULL || state.Ntoskrnl.sizeOfImage == 0UL) {
        return STATUS_DEVICE_NOT_READY; // 保留真正未初始化/无身份的诊断。
    }
    // 公共 PDB 可能只有精确全局 RVA，没有卸载记录私有类型；只补全本次快照。
    if (KswordARKUnloadedDynDataSourceIsTrusted(state.KernelGlobalSources.MmUnloadedDrivers) &&
        (state.Kernel.UldName == KSW_DYN_OFFSET_UNAVAILABLE ||
        !KswordARKUnloadedDynDataSourceIsTrusted(state.KernelSources.UldName) ||
        state.Kernel.UldStartAddress == KSW_DYN_OFFSET_UNAVAILABLE ||
        !KswordARKUnloadedDynDataSourceIsTrusted(state.KernelSources.UldStartAddress) ||
        state.Kernel.UldEndAddress == KSW_DYN_OFFSET_UNAVAILABLE ||
        !KswordARKUnloadedDynDataSourceIsTrusted(state.KernelSources.UldEndAddress) ||
        state.Kernel.UldCurrentTime == KSW_DYN_OFFSET_UNAVAILABLE ||
        !KswordARKUnloadedDynDataSourceIsTrusted(state.KernelSources.UldCurrentTime) ||
        state.Kernel.UldTypeSize == KSW_DYN_OFFSET_UNAVAILABLE ||
        !KswordARKUnloadedDynDataSourceIsTrusted(state.KernelSources.UldTypeSize))) {
        KSW_RUNTIME_KERNEL_LAYOUT runtimeLayout; // 不发布或修改整体 DynData 激活状态。
        if (KswordARKDriverResolveMmUnloadedLayout(
                &state.Ntoskrnl, state.KernelGlobals.MmUnloadedDrivers, &runtimeLayout)) {
            state.Kernel.UldName = (ULONG)runtimeLayout.UldName; // 局部使用已验证的偏移。
            state.KernelSources.UldName = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN; // 保留真实来源。
            state.Kernel.UldStartAddress = (ULONG)runtimeLayout.UldStartAddress; // 局部使用已验证的偏移。
            state.KernelSources.UldStartAddress = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN; // 保留真实来源。
            state.Kernel.UldEndAddress = (ULONG)runtimeLayout.UldEndAddress; // 局部使用已验证的偏移。
            state.KernelSources.UldEndAddress = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN; // 保留真实来源。
            state.Kernel.UldCurrentTime = (ULONG)runtimeLayout.UldCurrentTime; // 局部使用已验证的偏移。
            state.KernelSources.UldCurrentTime = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN; // 保留真实来源。
            state.Kernel.UldTypeSize = (ULONG)runtimeLayout.UldTypeSize; // 局部使用已验证的偏移。
            state.KernelSources.UldTypeSize = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN; // 保留真实来源。
        }
    }
    if (!KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.UldName) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.UldStartAddress) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.UldEndAddress) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.UldCurrentTime) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.UldTypeSize) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelGlobalSources.MmUnloadedDrivers) ||
        state.Kernel.UldName == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.UldStartAddress == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.UldEndAddress == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.UldCurrentTime == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.UldTypeSize == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.UldTypeSize < sizeof(UNICODE_STRING) ||
        state.Kernel.UldTypeSize > KSW_UNLOADED_DRIVER_MAX_ENTRY_BYTES ||
        state.Kernel.UldName > state.Kernel.UldTypeSize - sizeof(UNICODE_STRING) ||
        state.Kernel.UldStartAddress > state.Kernel.UldTypeSize - sizeof(PVOID) ||
        state.Kernel.UldEndAddress > state.Kernel.UldTypeSize - sizeof(PVOID) ||
        state.Kernel.UldCurrentTime > state.Kernel.UldTypeSize - sizeof(LARGE_INTEGER)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKUnloadedRvaToAddress(
            &state.Ntoskrnl,
            state.KernelGlobals.MmUnloadedDrivers,
            sizeof(PVOID),
            &recordsPointerAddress)) {
        return STATUS_NOT_SUPPORTED;
    }

    // 全局地址合法也不代表记录数组当前仍可读；禁止直接解引用候选地址。
    if (!KswordARKRuntimeReadMemory(
            (const VOID*)(ULONG_PTR)recordsPointerAddress, &records, sizeof(records))) {
        return STATUS_PARTIAL_COPY; // 读取失败不能伪装为正常空表。
    }
    if (records == NULL) {
        return STATUS_NOT_FOUND;
    }
    Layout->Records = records;
    Layout->RecordSize = state.Kernel.UldTypeSize;
    Layout->NameOffset = state.Kernel.UldName;
    Layout->StartAddressOffset = state.Kernel.UldStartAddress;
    Layout->EndAddressOffset = state.Kernel.UldEndAddress;
    Layout->CurrentTimeOffset = state.Kernel.UldCurrentTime;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKUnloadedResolvePiDdbLayout(
    _Out_ KSW_PIDDB_QUERY_LAYOUT* Layout
    )
/*++

Routine Description:

    Resolve the PiDDB AVL table, its ERESOURCE, and entry field offsets
    from identity-matched PDB or live-validated runtime data.

Return Value:

    STATUS_SUCCESS or a readable profile/layout status.

--*/
{
    KSW_DYN_STATE state;
    ULONGLONG tableAddress = 0ULL;
    ULONGLONG lockAddress = 0ULL;

    if (Layout == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Layout, sizeof(*Layout));
    RtlZeroMemory(&state, sizeof(state));
    KswordARKDynDataSnapshot(&state);

    // 整体 profile 未命中不等于本来源不可用；仍需初始化完成且内核身份有效。
    if (!state.Initialized || state.Ntoskrnl.present == 0UL ||
        state.Ntoskrnl.imageBase == 0ULL || state.Ntoskrnl.sizeOfImage == 0UL) {
        return STATUS_DEVICE_NOT_READY; // 保留真正未初始化/无身份的诊断。
    }
    // Global RVAs accepted for this exact kernel can complete only this query's
    // missing private fields. Keep shared DynData and all provenance unchanged.
    if (KswordARKUnloadedDynDataSourceIsTrusted(state.KernelGlobalSources.PiDDBCacheTable) &&
        KswordARKUnloadedDynDataSourceIsTrusted(state.KernelGlobalSources.PiDDBLock) &&
        (state.Kernel.PiDdbDriverName == KSW_DYN_OFFSET_UNAVAILABLE ||
         state.Kernel.PiDdbTimeDateStamp == KSW_DYN_OFFSET_UNAVAILABLE ||
         state.Kernel.PiDdbLoadStatus == KSW_DYN_OFFSET_UNAVAILABLE ||
         state.Kernel.PiDdbTypeSize == KSW_DYN_OFFSET_UNAVAILABLE)) {
        KSW_RUNTIME_KERNEL_LAYOUT runtimeLayout;
        NTSTATUS resolveStatus;
        RtlFillMemory(&runtimeLayout, sizeof(runtimeLayout), 0xFF);
        resolveStatus = KswordARKDriverResolvePiDdbLayout(&state.Ntoskrnl,
            state.KernelGlobals.PiDDBCacheTable, state.KernelGlobals.PiDDBLock, &runtimeLayout);
        if (!NT_SUCCESS(resolveStatus)) { return resolveStatus; }
        state.Kernel.PiDdbDriverName = (ULONG)runtimeLayout.PiDdbDriverName;
        state.Kernel.PiDdbTimeDateStamp = (ULONG)runtimeLayout.PiDdbTimeDateStamp;
        state.Kernel.PiDdbLoadStatus = (ULONG)runtimeLayout.PiDdbLoadStatus;
        state.Kernel.PiDdbTypeSize = (ULONG)runtimeLayout.PiDdbTypeSize;
        state.KernelSources.PiDdbDriverName = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
        state.KernelSources.PiDdbTimeDateStamp = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
        state.KernelSources.PiDdbLoadStatus = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
        state.KernelSources.PiDdbTypeSize = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    }
    if (!KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.PiDdbDriverName) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.PiDdbTimeDateStamp) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.PiDdbLoadStatus) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelSources.PiDdbTypeSize) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelGlobalSources.PiDDBCacheTable) ||
        !KswordARKUnloadedDynDataSourceIsTrusted(
            state.KernelGlobalSources.PiDDBLock) ||
        state.Kernel.PiDdbDriverName == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.PiDdbTimeDateStamp == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.PiDdbLoadStatus == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.PiDdbTypeSize == KSW_DYN_OFFSET_UNAVAILABLE ||
        state.Kernel.PiDdbTypeSize < sizeof(UNICODE_STRING) ||
        state.Kernel.PiDdbTypeSize > KSW_UNLOADED_DRIVER_MAX_ENTRY_BYTES ||
        state.Kernel.PiDdbDriverName >
            state.Kernel.PiDdbTypeSize - sizeof(UNICODE_STRING) ||
        state.Kernel.PiDdbTimeDateStamp >
            state.Kernel.PiDdbTypeSize - sizeof(ULONG) ||
        state.Kernel.PiDdbLoadStatus >
            state.Kernel.PiDdbTypeSize - sizeof(NTSTATUS)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKUnloadedRvaToAddress(
            &state.Ntoskrnl,
            state.KernelGlobals.PiDDBCacheTable,
            sizeof(RTL_AVL_TABLE),
            &tableAddress) ||
        !KswordARKUnloadedRvaToAddress(
            &state.Ntoskrnl,
            state.KernelGlobals.PiDDBLock,
            sizeof(ERESOURCE),
            &lockAddress)) {
        return STATUS_NOT_SUPPORTED;
    }

    Layout->Table = (PRTL_AVL_TABLE)(ULONG_PTR)tableAddress;
    Layout->Lock = (PERESOURCE)(ULONG_PTR)lockAddress;
    Layout->DriverNameOffset = state.Kernel.PiDdbDriverName;
    Layout->TimeDateStampOffset = state.Kernel.PiDdbTimeDateStamp;
    Layout->LoadStatusOffset = state.Kernel.PiDdbLoadStatus;
    Layout->EntrySize = state.Kernel.PiDdbTypeSize;
    return STATUS_SUCCESS;
}
