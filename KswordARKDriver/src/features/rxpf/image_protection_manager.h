#pragma once

#include <ntddk.h>

#include "page_state_table.h"

EXTERN_C_START

#ifndef KSW_RXPF_ENABLE_EXTERNAL_IMAGE_TARGETS
#define KSW_RXPF_ENABLE_EXTERNAL_IMAGE_TARGETS 1
#endif

#define KSW_RXPF_SELF_TEST_EXPECTED_VALUE 0x0000000012345679ULL

NTSTATUS
KswRxpfImageProtectionInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    );

VOID
KswRxpfImageProtectionUninitialize(
    VOID
    );

VOID
KswRxpfImageProtectionQuerySupport(
    _Out_ KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE* Response
    );

BOOLEAN
KswRxpfImageProtectionBuildSupported(
    VOID
    );

PVOID
KswRxpfImageProtectionSelfTestPage(
    VOID
    );

NTSTATUS
KswRxpfImageProtectionCreateRecord(
    _In_ ULONG TargetKind,
    _In_ ULONGLONG RequestedAddress,
    _In_ ULONG Flags,
    _Out_ KSW_RXPF_PAGE_RECORD* RecordSource
    );

NTSTATUS
KswRxpfImageProtectionChangeToRwNx(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    );

NTSTATUS
KswRxpfImageProtectionWrite(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) const UCHAR* Bytes,
    _In_ ULONG Length
    );

VOID
KswRxpfImageProtectionReleaseRecord(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    );

EXTERN_C_END
