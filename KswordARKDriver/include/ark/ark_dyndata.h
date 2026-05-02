#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkDynDataIoctl.h"

EXTERN_C_START

#define KSW_DYN_OFFSET_UNAVAILABLE 0xFFFFFFFFUL
#define KSW_DYN_CAPABILITY_NONE 0ULL

typedef struct _KSW_DYN_KERNEL_OFFSETS
{
    ULONG EpObjectTable;
    ULONG EpSectionObject;
    ULONG HtHandleContentionEvent;
    ULONG OtName;
    ULONG OtIndex;
    ULONG ObDecodeShift;
    ULONG ObAttributesShift;
    ULONG EgeGuid;
    ULONG EreGuidEntry;
    ULONG KtInitialStack;
    ULONG KtStackLimit;
    ULONG KtStackBase;
    ULONG KtKernelStack;
    ULONG KtReadOperationCount;
    ULONG KtWriteOperationCount;
    ULONG KtOtherOperationCount;
    ULONG KtReadTransferCount;
    ULONG KtWriteTransferCount;
    ULONG KtOtherTransferCount;
    ULONG MmSectionControlArea;
    ULONG MmControlAreaListHead;
    ULONG MmControlAreaLock;
    ULONG AlpcCommunicationInfo;
    ULONG AlpcOwnerProcess;
    ULONG AlpcConnectionPort;
    ULONG AlpcServerCommunicationPort;
    ULONG AlpcClientCommunicationPort;
    ULONG AlpcHandleTable;
    ULONG AlpcHandleTableLock;
    ULONG AlpcAttributes;
    ULONG AlpcAttributesFlags;
    ULONG AlpcPortContext;
    ULONG AlpcPortObjectLock;
    ULONG AlpcSequenceNo;
    ULONG AlpcState;
    ULONG EpProtection;
    ULONG EpSignatureLevel;
    ULONG EpSectionSignatureLevel;
} KSW_DYN_KERNEL_OFFSETS, *PKSW_DYN_KERNEL_OFFSETS;

typedef struct _KSW_DYN_LXCORE_OFFSETS
{
    ULONG LxPicoProc;
    ULONG LxPicoProcInfo;
    ULONG LxPicoProcInfoPID;
    ULONG LxPicoThrdInfo;
    ULONG LxPicoThrdInfoTID;
} KSW_DYN_LXCORE_OFFSETS, *PKSW_DYN_LXCORE_OFFSETS;

typedef struct _KSW_DYN_FIELD_DESCRIPTOR
{
    ULONG FieldId;
    PCSTR FieldName;
    PCSTR FeatureName;
    ULONG64 CapabilityMask;
    BOOLEAN Required;
    ULONG Source;
    ULONG Offset;
} KSW_DYN_FIELD_DESCRIPTOR, *PKSW_DYN_FIELD_DESCRIPTOR;

typedef struct _KSW_DYN_STATE
{
    BOOLEAN Initialized;
    BOOLEAN NtosActive;
    BOOLEAN LxcoreActive;
    BOOLEAN ExtraActive;
    NTSTATUS LastStatus;
    ULONG64 CapabilityMask;
    ULONG SystemInformerDataVersion;
    ULONG SystemInformerDataLength;
    ULONG MatchedProfileClass;
    ULONG MatchedProfileOffset;
    ULONG MatchedFieldsId;
    KSW_DYN_MODULE_IDENTITY_PACKET Ntoskrnl;
    KSW_DYN_MODULE_IDENTITY_PACKET Lxcore;
    KSW_DYN_KERNEL_OFFSETS Kernel;
    KSW_DYN_LXCORE_OFFSETS LxcoreOffsets;
    WCHAR UnavailableReason[KSW_DYN_REASON_CHARS];
} KSW_DYN_STATE, *PKSW_DYN_STATE;

NTSTATUS
KswordARKDynDataInitialize(
    _In_opt_ WDFDEVICE Device
    );

VOID
KswordARKDynDataUninitialize(
    VOID
    );

VOID
KswordARKDynDataSnapshot(
    _Out_ KSW_DYN_STATE* StateOut
    );

ULONG
KswordARKDynDataBuildFieldEntries(
    _Out_writes_opt_(EntryCapacity) KSW_DYN_FIELD_ENTRY* Entries,
    _In_ ULONG EntryCapacity
    );

NTSTATUS
KswordARKDynDataQueryStatus(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataQueryFields(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataQueryCapabilities(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
