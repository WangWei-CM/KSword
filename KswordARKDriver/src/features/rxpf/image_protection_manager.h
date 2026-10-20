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

/*
 * mode=1 restore：对 Record 调用 MmChangeImageProtection(op=1)，
 * 内部计算当前页内容的 SHA-512 作为 reference 让 VTL1 验证通过，PTE 回 RX。
 * 实测在 HVCI guest 下 VTL1 会返回 STATUS_ACCESS_VIOLATION（见注释），
 * 在实体机 root partition 下保留修改字节且 PTE 回 RX。调用后看 Record->LastStatus。
 */
VOID
KswRxpfRestoreExecutableProtection(
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
