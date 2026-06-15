/*++
Module Name:
    kernel_cpu_integrity.c
Abstract:
    Read-only per-CPU descriptor/MSR/control-register evidence collection for
    DriverDock integrity diagnostics.
Environment:
    Kernel-mode Driver Framework
--*/
#include "kernel_cpu_integrity.h"
#include <intrin.h>
#include <ntstrsafe.h>
#include <stdarg.h>
#define KSW_CPU_INTEGRITY_MSR_EFER 0xC0000080UL
#define KSW_CPU_INTEGRITY_MSR_LSTAR 0xC0000082UL
#define KSW_CPU_INTEGRITY_MSR_SYSENTER_EIP 0x00000176UL
#define KSW_CPU_INTEGRITY_CR0_WP 0x0000000000010000ULL
#define KSW_CPU_INTEGRITY_CR4_SMEP 0x0000000000100000ULL
#define KSW_CPU_INTEGRITY_CR4_SMAP 0x0000000000200000ULL
#define KSW_CPU_INTEGRITY_EFER_NXE 0x0000000000000800ULL
#define KSW_CPU_INTEGRITY_IDT_ENTRY_BYTES 16UL
#define KSW_DRIVER_INTEGRITY_LOADED_MODULE_LIMIT 512UL
#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xFFFFU
#endif
#if defined(_M_AMD64) || defined(_M_X64)
extern void _sgdt(void*);
#pragma intrinsic(_sgdt)
#define KswordARKCpuStoreGdtr _sgdt
#else
#define KswordARKCpuStoreGdtr(_Destination) UNREFERENCED_PARAMETER(_Destination)
#endif
#pragma pack(push, 1)
typedef struct _KSW_CPU_INTEGRITY_DESCRIPTOR_REGISTER
{
    USHORT Limit;
    ULONG_PTR Base;
} KSW_CPU_INTEGRITY_DESCRIPTOR_REGISTER, *PKSW_CPU_INTEGRITY_DESCRIPTOR_REGISTER;
typedef struct _KSW_CPU_INTEGRITY_IDT_ENTRY64
{
    USHORT OffsetLow;
    USHORT Selector;
    USHORT IstAndType;
    USHORT OffsetMiddle;
    ULONG OffsetHigh;
    ULONG Reserved;
} KSW_CPU_INTEGRITY_IDT_ENTRY64, *PKSW_CPU_INTEGRITY_IDT_ENTRY64;
#pragma pack(pop)
typedef struct _KSW_CPU_INTEGRITY_SAMPLE
{
    ULONG Group;
    ULONG Number;
    ULONG Captured;
    ULONGLONG Cr0;
    ULONGLONG Cr4;
    ULONGLONG Efer;
    ULONGLONG Lstar;
    ULONGLONG SysenterEip;
    KSW_CPU_INTEGRITY_DESCRIPTOR_REGISTER Idtr;
    KSW_CPU_INTEGRITY_DESCRIPTOR_REGISTER Gdtr;
} KSW_CPU_INTEGRITY_SAMPLE, *PKSW_CPU_INTEGRITY_SAMPLE;
static VOID
KswordARKCpuIntegrityFormatDetail(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_z_ PCWSTR FormatText,
    ...
    )
/*++
Routine Description:
    Format a bounded wide detail string for CPU evidence rows.
Arguments:
    Destination - Fixed output buffer.
    DestinationChars - Output capacity in WCHARs.
    FormatText - printf-style wide format.
    ... - Formatting arguments.
Return Value:
    None. Invalid output is ignored.
--*/
{
    va_list arguments;
    if (Destination == NULL || DestinationChars == 0UL || FormatText == NULL) {
        return;
    }
    Destination[0] = L'\0';
    va_start(arguments, FormatText);
    (VOID)RtlStringCbVPrintfW(Destination, (SIZE_T)DestinationChars * sizeof(WCHAR), FormatText, arguments);
    va_end(arguments);
    Destination[DestinationChars - 1UL] = L'\0';
}
VOID
KswordARKDriverIntegrityCopyWide(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_opt_z_ PCWSTR Source
    )
/*++
Routine Description:
    Copy a NUL-terminated wide string into a fixed protocol buffer.
Arguments:
    Destination - Fixed output buffer.
    DestinationChars - Output capacity in WCHARs.
    Source - Optional source string.
Return Value:
    None. Output is terminated when a buffer is supplied.
--*/
{
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL) {
        return;
    }
    (VOID)RtlStringCchCopyNW(Destination, DestinationChars, Source, DestinationChars - 1UL);
    Destination[DestinationChars - 1UL] = L'\0';
}
const KSW_HOOK_SYSTEM_MODULE_ENTRY*
KswordARKDriverIntegrityFindModuleForAddress(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONGLONG Address
    )
/*++
Routine Description:
    Resolve a kernel virtual address to a loaded module snapshot row.
Arguments:
    ModuleInfo - Optional SystemModuleInformation snapshot.
    Address - Address to classify.
Return Value:
    Owning module row or NULL.
--*/
{
    if (Address == 0ULL || Address > (ULONGLONG)((ULONG_PTR)~((ULONG_PTR)0U))) {
        return NULL;
    }
    return KswordARKHookFindModuleForAddress(ModuleInfo, (ULONG_PTR)Address);
}
BOOLEAN
KswordARKDriverIntegrityIsCoreKernelModule(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry
    )
/*++
Routine Description:
    Decide whether a module is one of the normal ntoskrnl/HAL entry owners.
Arguments:
    ModuleEntry - Optional loaded-module snapshot row.
Return Value:
    TRUE for nt kernel or HAL style filenames; otherwise FALSE.
--*/
{
    const UCHAR* fileName = NULL;
    ULONG fileNameBytes = 0UL;
    if (ModuleEntry == NULL) {
        return FALSE;
    }
    KswordARKHookGetModuleFileName(ModuleEntry, &fileName, &fileNameBytes);
    return KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, "ntoskrnl.exe") ||
        KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, "ntkrnlmp.exe") ||
        KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, "ntkrnlpa.exe") ||
        KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, "ntkrpamp.exe") ||
        KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, "hal.dll");
}
VOID
KswordARKDriverIntegrityAddEvidence(
    _Inout_ KSW_DRIVER_INTEGRITY_BUILDER* Builder,
    _In_ ULONG EvidenceClass,
    _In_ ULONGLONG ObjectAddress,
    _In_ ULONGLONG TargetAddress,
    _In_ ULONG RiskFlags,
    _In_ ULONG SourceMask,
    _In_ ULONG Confidence,
    _In_ ULONG ProcessorGroup,
    _In_ ULONG ProcessorNumber,
    _In_ ULONG Vector,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* OwnerModule,
    _In_opt_z_ PCWSTR DetailText
    )
/*++
Routine Description:
    Append one variable evidence row and preserve total count on truncation.
Arguments:
    Builder - Response builder state.
    EvidenceClass - Evidence class identifier.
    ObjectAddress - Object or field address under inspection.
    TargetAddress - Observed target address.
    RiskFlags - Risk bits for the row.
    SourceMask - Evidence source mask for the row.
    Confidence - 0..100 row confidence.
    ProcessorGroup - CPU group or ULONG_MAX for non-CPU rows.
    ProcessorNumber - CPU number or ULONG_MAX for non-CPU rows.
    Vector - IDT vector or ULONG_MAX for non-vector rows.
    OwnerModule - Optional owner module for TargetAddress.
    DetailText - Optional detail string.
Return Value:
    None. Response flags and counters expose truncation.
--*/
{
    KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE* response = NULL;
    KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE* row = NULL;
    if (Builder == NULL || Builder->Response == NULL) {
        return;
    }
    response = Builder->Response;
    response->totalCount += 1UL;
    response->sourceMask |= SourceMask;
    response->flags |= RiskFlags;
    if ((Builder->RowLimit != 0UL && response->returnedCount >= Builder->RowLimit) || response->returnedCount >= Builder->Capacity) {
        response->flags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_TRUNCATED;
        return;
    }
    row = &response->entries[response->returnedCount];
    RtlZeroMemory(row, sizeof(*row));
    row->evidenceClass = EvidenceClass;
    row->riskFlags = RiskFlags;
    row->sourceMask = SourceMask;
    row->confidence = Confidence;
    row->processorGroup = ProcessorGroup;
    row->processorNumber = ProcessorNumber;
    row->vector = Vector;
    row->objectAddress = ObjectAddress;
    row->targetAddress = TargetAddress;
    if (OwnerModule != NULL) {
        const UCHAR* fileName = NULL;
        ULONG fileNameBytes = 0UL;
        row->ownerModuleBase = (ULONGLONG)(ULONG_PTR)OwnerModule->ImageBase;
        row->ownerModuleSize = OwnerModule->ImageSize;
        KswordARKHookGetModuleFileName(OwnerModule, &fileName, &fileNameBytes);
        KswordARKHookCopyBoundedAnsiToWide(fileName, fileNameBytes, row->ownerModule, KSWORD_ARK_DRIVER_INTEGRITY_OWNER_CHARS);
    }
    KswordARKDriverIntegrityCopyWide(row->detail, KSWORD_ARK_DRIVER_INTEGRITY_DETAIL_CHARS, DetailText);
    response->returnedCount += 1UL;
}
BOOLEAN
KswordARKDriverIntegrityOffsetPresent(
    _In_ ULONG Offset
    )
/*++
Routine Description:
    Test whether a DynData offset is usable for read-only field access.
Arguments:
    Offset - Offset value from the active DynData snapshot.
Return Value:
    TRUE when the offset is not a sentinel.
--*/
{
    return Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL;
}
static BOOLEAN
KswordARKDriverIntegrityReadLoadedModuleRecord(
    _In_ ULONGLONG EntryAddress,
    _In_ const KSW_DYN_KERNEL_OFFSETS* Offsets,
    _Out_ KSW_DRIVER_INTEGRITY_LDR_TARGET* Record
    )
/*++
Routine Description:
    Read one KLDR entry using only DynData offsets and guarded memory copies.
Arguments:
    EntryAddress - Address of the KLDR_DATA_TABLE_ENTRY candidate.
    Offsets - Active DynData kernel offsets.
    Record - Receives the selected KLDR fields and a bounded basename.
Return Value:
    TRUE when the required fields were copied.
--*/
{
    UNICODE_STRING baseName;
    WCHAR baseNameBuffer[KSWORD_ARK_DRIVER_INTEGRITY_OWNER_CHARS] = { 0 };
    ULONG baseNameChars = 0UL;
    if (Record == NULL || Offsets == NULL || EntryAddress == 0ULL) {
        return FALSE;
    }
    if (!KswordARKDriverIntegrityOffsetPresent(Offsets->KldrInLoadOrderLinks) ||
        !KswordARKDriverIntegrityOffsetPresent(Offsets->KldrDllBase) ||
        !KswordARKDriverIntegrityOffsetPresent(Offsets->KldrSizeOfImage)) {
        return FALSE;
    }
    RtlZeroMemory(Record, sizeof(*Record));
    Record->Available = TRUE;
    Record->EntryAddress = EntryAddress;
    Record->LinkAddress = EntryAddress + (ULONGLONG)Offsets->KldrInLoadOrderLinks;
    if (!KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)(EntryAddress + (ULONGLONG)Offsets->KldrDllBase), &Record->DllBase, sizeof(Record->DllBase)) ||
        !KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)(EntryAddress + (ULONGLONG)Offsets->KldrSizeOfImage), &Record->SizeOfImage, sizeof(Record->SizeOfImage))) {
        return FALSE;
    }
    if (KswordARKDriverIntegrityOffsetPresent(Offsets->KldrBaseDllName) &&
        KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)(EntryAddress + (ULONGLONG)Offsets->KldrBaseDllName), &baseName, sizeof(baseName)) &&
        baseName.Buffer != NULL &&
        baseName.Length != 0U) {
        baseNameChars = (ULONG)(baseName.Length / sizeof(WCHAR));
        if (baseNameChars >= RTL_NUMBER_OF(baseNameBuffer)) {
            baseNameChars = RTL_NUMBER_OF(baseNameBuffer) - 1UL;
        }
        if (baseNameChars != 0UL &&
            KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)baseName.Buffer, baseNameBuffer, (SIZE_T)baseNameChars * sizeof(WCHAR))) {
            baseNameBuffer[baseNameChars] = L'\0';
            baseName.Buffer = baseNameBuffer;
            baseName.Length = (USHORT)(baseNameChars * sizeof(WCHAR));
            baseName.MaximumLength = baseName.Length;
            KswordARKDriverIntegrityCopyWide(Record->BaseDllName, RTL_NUMBER_OF(Record->BaseDllName), baseNameBuffer);
        }
    }
    return TRUE;
}
ULONGLONG
KswordARKDriverIntegrityNtosAddressFromRva(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONG Rva,
    _In_ SIZE_T ProbeBytes
    )
/*++
Routine Description:
    Convert a DynData ntoskrnl RVA to a readable kernel VA.
Arguments:
    DynState - Active DynData snapshot.
    Rva - Global RVA from the PDB profile.
    ProbeBytes - Optional bytes that must fit inside the image.
Return Value:
    Kernel VA, or zero when DynData is inactive/untrusted/unreadable.
--*/
{
    UCHAR probe[sizeof(LIST_ENTRY)] = { 0 };
    ULONGLONG address = 0ULL;
    if (DynState == NULL || !DynState->Initialized || !DynState->NtosActive ||
        DynState->Ntoskrnl.imageBase == 0ULL || DynState->Ntoskrnl.sizeOfImage == 0UL ||
        !KswordARKDriverIntegrityOffsetPresent(Rva)) {
        return 0ULL;
    }
    if (Rva >= DynState->Ntoskrnl.sizeOfImage ||
        ProbeBytes > sizeof(probe) ||
        (ProbeBytes != 0U && ((ULONGLONG)Rva + (ULONGLONG)ProbeBytes) > (ULONGLONG)DynState->Ntoskrnl.sizeOfImage) ||
        DynState->Ntoskrnl.imageBase > (((ULONGLONG)~0ULL) - (ULONGLONG)Rva)) {
        return 0ULL;
    }
    address = DynState->Ntoskrnl.imageBase + (ULONGLONG)Rva;
    if (ProbeBytes != 0U &&
        !KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)address, probe, ProbeBytes)) {
        return 0ULL;
    }
    return address;
}
NTSTATUS
KswordARKDriverIntegrityFindLoadedModule(
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONGLONG DriverStart,
    _Out_ KSW_DRIVER_INTEGRITY_LDR_TARGET* TargetOut
    )
/*++
Routine Description:
    Walk PsLoadedModuleList read-only with DynData KLDR offsets and find the target image.
Arguments:
    DynState - Query-time DynData snapshot.
    DriverStart - DriverObject->DriverStart or requested module base.
    TargetOut - Receives the matched target or list-head evidence.
Return Value:
    STATUS_SUCCESS when a target row is found, STATUS_NOT_FOUND for a valid
    walk without a target match, or an explanatory status for unavailable data.
--*/
{
    ULONGLONG listHead = 0ULL;
    LIST_ENTRY headLinks;
    ULONGLONG currentLink = 0ULL;
    ULONG visited = 0UL;
    if (TargetOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(TargetOut, sizeof(*TargetOut));
    if (DynState == NULL ||
        !DynState->Initialized ||
        !DynState->NtosActive ||
        (DynState->CapabilityMask & KSW_CAP_KERNEL_MODULE_LIST_FIELDS) == 0ULL ||
        !KswordARKDriverIntegrityOffsetPresent(DynState->KernelGlobals.PsLoadedModuleList)) {
        return STATUS_NOT_SUPPORTED;
    }
    listHead = KswordARKDriverIntegrityNtosAddressFromRva(DynState, DynState->KernelGlobals.PsLoadedModuleList, sizeof(LIST_ENTRY));
    if (listHead == 0ULL) {
        return STATUS_NOT_FOUND;
    }
    if (!KswordARKDriverIntegrityOffsetPresent(DynState->Kernel.KldrInLoadOrderLinks) ||
        !KswordARKDriverIntegrityOffsetPresent(DynState->Kernel.KldrDllBase) ||
        !KswordARKDriverIntegrityOffsetPresent(DynState->Kernel.KldrSizeOfImage)) {
        return STATUS_NOT_SUPPORTED;
    }
    RtlZeroMemory(&headLinks, sizeof(headLinks));
    if (!KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)listHead, &headLinks, sizeof(headLinks))) {
        return STATUS_ACCESS_VIOLATION;
    }
    TargetOut->Available = TRUE;
    TargetOut->ListHeadAddress = listHead;
    currentLink = (ULONGLONG)(ULONG_PTR)headLinks.Flink;
    while (currentLink != 0ULL && currentLink != listHead && visited < KSW_DRIVER_INTEGRITY_LOADED_MODULE_LIMIT) {
        KSW_DRIVER_INTEGRITY_LDR_TARGET record;
        ULONGLONG entryAddress = 0ULL;
        LIST_ENTRY links;
        if (currentLink < (ULONGLONG)DynState->Kernel.KldrInLoadOrderLinks) {
            return STATUS_ACCESS_VIOLATION;
        }
        entryAddress = currentLink - (ULONGLONG)DynState->Kernel.KldrInLoadOrderLinks;
        RtlZeroMemory(&record, sizeof(record));
        RtlZeroMemory(&links, sizeof(links));
        if (!KswordARKDriverIntegrityReadLoadedModuleRecord(entryAddress, &DynState->Kernel, &record)) {
            return STATUS_PARTIAL_COPY;
        }
        record.ListHeadAddress = listHead;
        if (DriverStart != 0ULL &&
            record.SizeOfImage != 0UL &&
            record.DllBase <= (((ULONGLONG)~0ULL) - (ULONGLONG)record.SizeOfImage) &&
            DriverStart >= record.DllBase &&
            DriverStart < (record.DllBase + (ULONGLONG)record.SizeOfImage)) {
            record.Found = TRUE;
            RtlCopyMemory(TargetOut, &record, sizeof(*TargetOut));
            return STATUS_SUCCESS;
        }
        if (!KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)currentLink, &links, sizeof(links))) {
            return STATUS_PARTIAL_COPY;
        }
        currentLink = (ULONGLONG)(ULONG_PTR)links.Flink;
        ++visited;
    }
    return (visited >= KSW_DRIVER_INTEGRITY_LOADED_MODULE_LIMIT) ? STATUS_MORE_ENTRIES : STATUS_NOT_FOUND;
}
static ULONGLONG
KswordARKCpuIntegrityIdtHandler(
    _In_ const KSW_CPU_INTEGRITY_IDT_ENTRY64* Entry
    )
/*++
Routine Description:
    Decode one x64 IDT gate handler address from copied descriptor bytes.
Arguments:
    Entry - Copied IDT gate descriptor.
Return Value:
    Handler virtual address, or zero for invalid input.
--*/
{
#if defined(_M_AMD64) || defined(_M_X64)
    ULONGLONG high = 0ULL;
    ULONGLONG middle = 0ULL;
    ULONGLONG low = 0ULL;
    if (Entry == NULL) {
        return 0ULL;
    }
    low = (ULONGLONG)Entry->OffsetLow;
    middle = ((ULONGLONG)Entry->OffsetMiddle) << 16;
    high = ((ULONGLONG)Entry->OffsetHigh) << 32;
    return high | middle | low;
#else
    UNREFERENCED_PARAMETER(Entry);
    return 0ULL;
#endif
}
static VOID
KswordARKCpuIntegrityCaptureCurrent(
    _Inout_ KSW_CPU_INTEGRITY_SAMPLE* Sample
    )
/*++
Routine Description:
    Capture only read-only CPU state on the current processor.
Arguments:
    Sample - Per-CPU sample to populate.
Return Value:
    None. Unsupported architectures mark Captured as zero.
--*/
{
#if defined(_M_AMD64) || defined(_M_X64)
    if (Sample == NULL) {
        return;
    }
    Sample->Cr0 = __readcr0();
    Sample->Cr4 = __readcr4();
    Sample->Efer = __readmsr(KSW_CPU_INTEGRITY_MSR_EFER);
    Sample->Lstar = __readmsr(KSW_CPU_INTEGRITY_MSR_LSTAR);
    Sample->SysenterEip = __readmsr(KSW_CPU_INTEGRITY_MSR_SYSENTER_EIP);
    __sidt(&Sample->Idtr);
    KswordARKCpuStoreGdtr(&Sample->Gdtr);
    Sample->Captured = 1UL;
#else
    UNREFERENCED_PARAMETER(Sample);
#endif
}
static VOID
KswordARKCpuIntegrityEmitControlRows(
    _Inout_ KSW_DRIVER_INTEGRITY_BUILDER* Builder,
    _In_ const KSW_CPU_INTEGRITY_SAMPLE* Sample,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo
    )
/*++
Routine Description:
    Emit CR0/CR4/EFER/LSTAR/SYSENTER and descriptor-table summary rows.
Arguments:
    Builder - Response builder.
    Sample - Captured CPU state.
    ModuleInfo - Optional module snapshot for MSR owner attribution.
Return Value:
    None. Rows are appended to Builder.
--*/
{
    WCHAR detail[KSWORD_ARK_DRIVER_INTEGRITY_DETAIL_CHARS] = { 0 };
    ULONG riskFlags = KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE;
    ULONG lstarRisk = KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE;
    ULONG sysenterRisk = KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE;
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* lstarOwner = NULL;
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* sysenterOwner = NULL;
    if (Builder == NULL || Sample == NULL || Sample->Captured == 0UL) {
        return;
    }
    if ((Sample->Cr0 & KSW_CPU_INTEGRITY_CR0_WP) == 0ULL) {
        riskFlags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED;
    }
    if ((Sample->Cr4 & KSW_CPU_INTEGRITY_CR4_SMEP) == 0ULL) {
        riskFlags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED;
    }
    if ((Sample->Cr4 & KSW_CPU_INTEGRITY_CR4_SMAP) == 0ULL) {
        riskFlags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED;
    }
    if ((Sample->Efer & KSW_CPU_INTEGRITY_EFER_NXE) == 0ULL) {
        riskFlags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED;
    }
    KswordARKCpuIntegrityFormatDetail(detail, RTL_NUMBER_OF(detail), L"CR0=0x%llX CR4=0x%llX EFER=0x%llX; WP/SMEP/SMAP/NXE read-only state.", Sample->Cr0, Sample->Cr4, Sample->Efer);
    KswordARKDriverIntegrityAddEvidence(Builder, KSWORD_ARK_DRIVER_INTEGRITY_CLASS_CPU_CONTROL, 0ULL, 0ULL, riskFlags,
        KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_CPU_REGISTER | KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_MSR, 90UL, Sample->Group, Sample->Number, ~0UL, NULL, detail);
    lstarOwner = KswordARKDriverIntegrityFindModuleForAddress(ModuleInfo, Sample->Lstar);
    sysenterOwner = KswordARKDriverIntegrityFindModuleForAddress(ModuleInfo, Sample->SysenterEip);
    if (lstarOwner == NULL) {
        lstarRisk |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED;
    }
    else if (!KswordARKDriverIntegrityIsCoreKernelModule(lstarOwner)) {
        lstarRisk |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER;
    }
    if (Sample->SysenterEip != 0ULL) {
        if (sysenterOwner == NULL) {
            sysenterRisk |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED;
        }
        else if (!KswordARKDriverIntegrityIsCoreKernelModule(sysenterOwner)) {
            sysenterRisk |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER;
        }
    }
    KswordARKCpuIntegrityFormatDetail(detail, RTL_NUMBER_OF(detail), L"MSR_LSTAR=0x%llX; SYSENTER_EIP=0x%llX.", Sample->Lstar, Sample->SysenterEip);
    KswordARKDriverIntegrityAddEvidence(Builder, KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY, 0ULL, Sample->Lstar,
        lstarRisk,
        KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_MSR | KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SYSTEM_MODULE, 90UL, Sample->Group, Sample->Number, ~0UL, lstarOwner, detail);
    KswordARKDriverIntegrityAddEvidence(Builder, KSWORD_ARK_DRIVER_INTEGRITY_CLASS_MSR_ENTRY, 0ULL, Sample->SysenterEip,
        sysenterRisk,
        KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_MSR | KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SYSTEM_MODULE, 70UL, Sample->Group, Sample->Number, ~0UL, sysenterOwner, L"SYSENTER_EIP captured read-only; x64 systems may leave this path unused.");
    KswordARKCpuIntegrityFormatDetail(detail, RTL_NUMBER_OF(detail), L"IDTR base=0x%llX limit=0x%04X; GDTR base=0x%llX limit=0x%04X.", (ULONGLONG)Sample->Idtr.Base, Sample->Idtr.Limit, (ULONGLONG)Sample->Gdtr.Base, Sample->Gdtr.Limit);
    KswordARKDriverIntegrityAddEvidence(Builder, KSWORD_ARK_DRIVER_INTEGRITY_CLASS_DESCRIPTOR_TABLE, (ULONGLONG)Sample->Idtr.Base,
        (ULONGLONG)Sample->Gdtr.Base, KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE,
        KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_IDT | KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_GDT, 90UL, Sample->Group, Sample->Number, ~0UL, NULL, detail);
}
static VOID
KswordARKCpuIntegrityEmitIdtRows(
    _Inout_ KSW_DRIVER_INTEGRITY_BUILDER* Builder,
    _In_ const KSW_CPU_INTEGRITY_SAMPLE* Sample,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONG MaxVectors
    )
/*++
Routine Description:
    Read copied IDT entries and emit handler owner evidence rows.
Arguments:
    Builder - Response builder.
    Sample - Captured descriptor state for one CPU.
    ModuleInfo - Optional module snapshot for owner attribution.
    MaxVectors - Maximum vectors to emit for this CPU.
Return Value:
    None. IDT rows are appended to Builder.
--*/
{
    ULONG vector = 0UL;
    ULONG vectorCount = 0UL;
    if (Builder == NULL || Sample == NULL || Sample->Captured == 0UL || Sample->Idtr.Base == 0U) {
        return;
    }
    vectorCount = ((ULONG)Sample->Idtr.Limit + 1UL) / KSW_CPU_INTEGRITY_IDT_ENTRY_BYTES;
    if (vectorCount > 256UL) {
        vectorCount = 256UL;
    }
    if (MaxVectors != 0UL && vectorCount > MaxVectors) {
        vectorCount = MaxVectors;
    }
    for (vector = 0UL; vector < vectorCount; ++vector) {
        KSW_CPU_INTEGRITY_IDT_ENTRY64 entry;
        ULONGLONG entryAddress = (ULONGLONG)Sample->Idtr.Base + ((ULONGLONG)vector * KSW_CPU_INTEGRITY_IDT_ENTRY_BYTES);
        ULONGLONG handler = 0ULL;
        ULONG riskFlags = KSWORD_ARK_DRIVER_INTEGRITY_RISK_NONE;
        const KSW_HOOK_SYSTEM_MODULE_ENTRY* owner = NULL;
        WCHAR detail[KSWORD_ARK_DRIVER_INTEGRITY_DETAIL_CHARS] = { 0 };
        RtlZeroMemory(&entry, sizeof(entry));
        if (!KswordARKHookReadMemorySafe((const VOID*)(ULONG_PTR)entryAddress, &entry, sizeof(entry))) {
            riskFlags = KSWORD_ARK_DRIVER_INTEGRITY_RISK_QUERY_FAILED;
            KswordARKDriverIntegrityAddEvidence(Builder, KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER, entryAddress, 0ULL, riskFlags,
                KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_IDT, 35UL, Sample->Group, Sample->Number, vector, NULL, L"IDT entry read failed.");
            continue;
        }
        handler = KswordARKCpuIntegrityIdtHandler(&entry);
        owner = KswordARKDriverIntegrityFindModuleForAddress(ModuleInfo, handler);
        if (owner == NULL) {
            riskFlags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED;
        }
        else if (!KswordARKDriverIntegrityIsCoreKernelModule(owner)) {
            riskFlags |= KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER;
        }
        KswordARKCpuIntegrityFormatDetail(detail, RTL_NUMBER_OF(detail), L"IDT[%lu] gate=0x%llX handler=0x%llX selector=0x%04X attr=0x%04X.", vector, entryAddress, handler, entry.Selector, entry.IstAndType);
        KswordARKDriverIntegrityAddEvidence(Builder, KSWORD_ARK_DRIVER_INTEGRITY_CLASS_IDT_HANDLER, entryAddress, handler, riskFlags,
            KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_IDT | KSWORD_ARK_DRIVER_INTEGRITY_SOURCE_SYSTEM_MODULE, 85UL, Sample->Group, Sample->Number, vector, owner, detail);
    }
}
NTSTATUS
KswordARKCpuIntegrityCollect(
    _Inout_ KSW_DRIVER_INTEGRITY_BUILDER* Builder,
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONG Flags,
    _In_ ULONG MaxIdtVectorsPerCpu,
    _Out_ ULONG* CpuCountOut
    )
/*++
Routine Description:
    Sequentially switch to each active processor and collect read-only CPU entry evidence.
Arguments:
    Builder - Response builder.
    ModuleInfo - Optional module snapshot for owner attribution.
    Flags - Query flags controlling IDT expansion.
    MaxIdtVectorsPerCpu - Per-CPU IDT vector cap; zero selects protocol default.
    CpuCountOut - Receives number of active processors visited or attempted.
Return Value:
    STATUS_SUCCESS when at least the current architecture supports sampling.
--*/
{
#if defined(_M_AMD64) || defined(_M_X64)
    ULONG groupCount = 0UL;
    ULONG groupIndex = 0UL;
    ULONG totalCpus = 0UL;
    ULONG allActiveCount = 0UL;
    if (Builder == NULL || CpuCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CpuCountOut = 0UL;
    if (MaxIdtVectorsPerCpu == 0UL || MaxIdtVectorsPerCpu > KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS) {
        MaxIdtVectorsPerCpu = KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS;
    }
    groupCount = KeQueryActiveGroupCount();
    allActiveCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    for (groupIndex = 0UL; groupIndex < groupCount; ++groupIndex) {
        ULONG activeInGroup = KeQueryActiveProcessorCountEx((USHORT)groupIndex);
        ULONG visitedInGroup = 0UL;
        ULONG globalIndex = 0UL;
        for (globalIndex = 0UL; globalIndex < allActiveCount && visitedInGroup < activeInGroup; ++globalIndex) {
            GROUP_AFFINITY targetAffinity;
            GROUP_AFFINITY previousAffinity;
            PROCESSOR_NUMBER processorNumber;
            KSW_CPU_INTEGRITY_SAMPLE sample;
            KAFFINITY bit = 0U;
            RtlZeroMemory(&processorNumber, sizeof(processorNumber));
            if (!NT_SUCCESS(KeGetProcessorNumberFromIndex(globalIndex, &processorNumber))) {
                break;
            }
            if (processorNumber.Group != (USHORT)groupIndex) {
                continue;
            }
            if (processorNumber.Number >= (sizeof(KAFFINITY) * 8UL)) {
                break;
            }
            bit = ((KAFFINITY)1) << processorNumber.Number;
            RtlZeroMemory(&targetAffinity, sizeof(targetAffinity));
            RtlZeroMemory(&previousAffinity, sizeof(previousAffinity));
            RtlZeroMemory(&sample, sizeof(sample));
            targetAffinity.Group = (USHORT)groupIndex;
            targetAffinity.Mask = bit;
            KeSetSystemGroupAffinityThread(&targetAffinity, &previousAffinity);
            sample.Group = groupIndex;
            sample.Number = processorNumber.Number;
            KswordARKCpuIntegrityCaptureCurrent(&sample);
            KeRevertToUserGroupAffinityThread(&previousAffinity);
            visitedInGroup += 1UL;
            totalCpus += 1UL;
            KswordARKCpuIntegrityEmitControlRows(Builder, &sample, ModuleInfo);
            if ((Flags & KSWORD_ARK_DRIVER_INTEGRITY_FLAG_IDT_ENTRIES) != 0UL) {
                KswordARKCpuIntegrityEmitIdtRows(Builder, &sample, ModuleInfo, MaxIdtVectorsPerCpu);
            }
        }
    }
    *CpuCountOut = totalCpus;
    return STATUS_SUCCESS;
#else
    UNREFERENCED_PARAMETER(Builder);
    UNREFERENCED_PARAMETER(ModuleInfo);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(MaxIdtVectorsPerCpu);
    if (CpuCountOut != NULL) {
        *CpuCountOut = 0UL;
    }
    return STATUS_NOT_SUPPORTED;
#endif
}
