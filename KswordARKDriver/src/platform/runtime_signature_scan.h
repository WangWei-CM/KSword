#pragma once

#include <ntddk.h>

EXTERN_C_START

#define KSW_RUNTIME_IMAGE_MAX_SECTIONS 64UL
#define KSW_RUNTIME_SIGNATURE_MAX_ROUTINES 64UL

typedef struct _KSW_RUNTIME_IMAGE_SECTION
{
    ULONG_PTR Start;
    ULONG_PTR End;
    ULONG Characteristics;
} KSW_RUNTIME_IMAGE_SECTION, *PKSW_RUNTIME_IMAGE_SECTION;

typedef struct _KSW_RUNTIME_IMAGE_VIEW
{
    ULONG_PTR Base;
    ULONG Size;
    ULONG SectionCount;
    KSW_RUNTIME_IMAGE_SECTION Sections[KSW_RUNTIME_IMAGE_MAX_SECTIONS];
} KSW_RUNTIME_IMAGE_VIEW, *PKSW_RUNTIME_IMAGE_VIEW;

typedef struct _KSW_RUNTIME_DATA_REFERENCE
{
    ULONG_PTR Address;
    ULONG_PTR RoutineAddress;
    ULONG_PTR InstructionAddress;
} KSW_RUNTIME_DATA_REFERENCE, *PKSW_RUNTIME_DATA_REFERENCE;

BOOLEAN
KswordARKRuntimeReadMemory(
    _In_ const VOID* Address,
    _Out_writes_bytes_(Size) VOID* Buffer,
    _In_ SIZE_T Size
    );

BOOLEAN
KswordARKRuntimeInitializeImageView(
    _In_ PVOID ImageBase,
    _In_ ULONG ImageSize,
    _Out_ PKSW_RUNTIME_IMAGE_VIEW ViewOut
    );

BOOLEAN
KswordARKRuntimeAddressInImage(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG_PTR Address,
    _In_ SIZE_T RequiredBytes
    );

BOOLEAN
KswordARKRuntimeAddressIsExecutable(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG_PTR Address,
    _In_ SIZE_T RequiredBytes
    );

BOOLEAN
KswordARKRuntimeAddressIsWritableData(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG_PTR Address,
    _In_ SIZE_T RequiredBytes
    );

PVOID
KswordARKRuntimeFindExport(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_z_ PCSTR ExportName
    );

ULONG
KswordARKRuntimeCollectAnchoredDataReferences(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_reads_(AnchorCount) PCSTR const* AnchorNames,
    _In_ ULONG AnchorCount,
    _In_ ULONG MaxCallDepth,
    _In_ ULONG RoutineScanBytes,
    _Out_writes_(ReferenceCapacity) KSW_RUNTIME_DATA_REFERENCE* References,
    _In_ ULONG ReferenceCapacity
    );

ULONG
KswordARKRuntimeCollectFunctionDataReferences(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_reads_(AnchorCount) PCSTR const* AnchorNames,
    _In_ ULONG AnchorCount,
    _In_ ULONG MaxCallDepth,
    _In_ ULONG RoutineScanBytes,
    _Out_writes_(ReferenceCapacity) KSW_RUNTIME_DATA_REFERENCE* References,
    _In_ ULONG ReferenceCapacity
    ); // 只扫描 PE 异常目录确认的函数范围；不依赖 PDB。

ULONG
KswordARKRuntimeCollectExecutableDataReferences(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG ScanByteBudget,
    _Out_writes_(ReferenceCapacity) KSW_RUNTIME_DATA_REFERENCE* References,
    _In_ ULONG ReferenceCapacity
    );

// Resume a bounded executable-section scan after a full reference batch.
// CursorRva and ScannedBytesOut prevent the first section from exhausting the
// caller's reference capacity and hiding later executable sections.
ULONG
KswordARKRuntimeCollectExecutableDataReferenceBatch(
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View,
    _In_ ULONG ScanByteBudget,
    _Inout_ ULONG* CursorRva,
    _Out_writes_(ReferenceCapacity) KSW_RUNTIME_DATA_REFERENCE* References,
    _In_ ULONG ReferenceCapacity,
    _Out_ ULONG* ScannedBytesOut
    );

EXTERN_C_END
