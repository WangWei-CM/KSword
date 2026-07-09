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
    ULONG EpUniqueProcessId;
    ULONG EpActiveProcessLinks;
    ULONG EpThreadListHead;
    ULONG EpImageFileName;
    ULONG EpToken;
    ULONG EpFlags;
    ULONG EpFlags2;
    ULONG EpRundownProtect;
    ULONG EpProcessLock;
    ULONG EpCreateTime;
    ULONG EpExitTime;
    ULONG EpExitStatus;
    ULONG EpPeb;
    ULONG EpSession;
    ULONG EpWin32Process;
    ULONG EpWow64Process;
    ULONG EpInheritedFromUniqueProcessId;
    ULONG EpSeAuditProcessCreationInfo;
    ULONG EpJob;
    ULONG EpDeviceMap;
    ULONG EpDebugPort;
    ULONG EpExceptionPortData;
    ULONG EpSectionBaseAddress;
    ULONG EpImageFilePointer;
    ULONG EpPriorityClass;
    ULONG EpActiveThreads;
    ULONG EpVadRoot;
    ULONG EpVadHint;
    ULONG EpCloneRoot;
    ULONG EpNumberOfPrivatePages;
    ULONG EpNumberOfLockedPages;
    ULONG EpCommitCharge;
    ULONG EpCommitChargePeak;
    ULONG EpPeakVirtualSize;
    ULONG EpVirtualSize;
    ULONG EpSessionProcessLinks;
    ULONG EpMitigationFlags;
    ULONG EpMitigationFlags2;
    ULONG EpProcessQuotaUsage;
    ULONG EpProcessQuotaPeak;
    ULONG EpAddressCreationLock;
    ULONG EpPageTableCommitmentLock;
    ULONG EpRotateInProgress;
    ULONG EpForkInProgress;
    ULONG EpCommitChargeJob;
    ULONG EpCookie;
    ULONG EpWorkingSetWatch;
    ULONG EpWin32WindowStation;
    ULONG EpOwnerProcessId;
    ULONG EpQuotaBlock;
    ULONG EpEtwDataSource;
    ULONG EpPageDirectoryPte;
    ULONG EpSecurityPort;
    ULONG EpJobLinks;
    ULONG EpHighestUserAddress;
    ULONG EpImagePathHash;
    ULONG EpDefaultHardErrorProcessing;
    ULONG EpLastThreadExitStatus;
    ULONG EpPrefetchTrace;
    ULONG EpLockedPagesList;
    ULONG EpReadOperationCount;
    ULONG EpWriteOperationCount;
    ULONG EpOtherOperationCount;
    ULONG EpReadTransferCount;
    ULONG EpWriteTransferCount;
    ULONG EpOtherTransferCount;
    ULONG EpCommitChargeLimit;
    ULONG EpVm;
    ULONG EpMmProcessLinks;
    ULONG EpModifiedPageCount;
    ULONG EpVadCount;
    ULONG EpVadPhysicalPages;
    ULONG EpVadPhysicalPagesLimit;
    ULONG EpAlpcContext;
    ULONG EpTimerResolutionLink;
    ULONG EpTimerResolutionStackRecord;
    ULONG EpRequestedTimerResolution;
    ULONG EpSmallestTimerResolution;
    ULONG EpInvertedFunctionTable;
    ULONG EpInvertedFunctionTableLock;
    ULONG EpActiveThreadsHighWatermark;
    ULONG EpLargePrivateVadCount;
    ULONG EpThreadListLock;
    ULONG EpWnfContext;
    ULONG EpFlags3;
    ULONG EpDiskCounters;
    ULONG TokTokenSource;
    ULONG TokTokenId;
    ULONG TokAuthenticationId;
    ULONG TokParentTokenId;
    ULONG TokExpirationTime;
    ULONG TokTokenLock;
    ULONG TokModifiedId;
    ULONG TokPrivileges;
    ULONG TokAuditPolicy;
    ULONG TokSessionId;
    ULONG TokUserAndGroupCount;
    ULONG TokRestrictedSidCount;
    ULONG TokVariableLength;
    ULONG TokDynamicCharged;
    ULONG TokDynamicAvailable;
    ULONG TokDefaultOwnerIndex;
    ULONG TokUserAndGroups;
    ULONG TokRestrictedSids;
    ULONG TokPrimaryGroup;
    ULONG TokDynamicPart;
    ULONG TokDefaultDacl;
    ULONG TokTokenType;
    ULONG TokImpersonationLevel;
    ULONG TokTokenFlags;
    ULONG TokTokenInUse;
    ULONG TokIntegrityLevelIndex;
    ULONG TokMandatoryPolicy;
    ULONG TokLogonSession;
    ULONG TokOriginatingLogonSession;
    ULONG TokSidHash;
    ULONG TokRestrictedSidHash;
    ULONG TokPSecurityAttributes;
    ULONG TokPackage;
    ULONG TokCapabilities;
    ULONG TokCapabilityCount;
    ULONG TokCapabilitiesHash;
    ULONG TokLowboxNumberEntry;
    ULONG TokLowboxHandlesEntry;
    ULONG TokPClaimAttributes;
    ULONG TokTrustLevelSid;
    ULONG TokTrustLinkedToken;
    ULONG TokIntegrityLevelSidValue;
    ULONG TokTokenSidValues;
    ULONG TokSessionObject;
    ULONG TokVariablePart;
    ULONG EtCid;
    ULONG EtThreadListEntry;
    ULONG EtStartAddress;
    ULONG EtWin32StartAddress;
    ULONG KtProcess;
    ULONG HtHandleContentionEvent;
    ULONG HtTableCode;
    ULONG HtHandleCount;
    ULONG HteLowValue;
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
    ULONG KldrInLoadOrderLinks;
    ULONG KldrDllBase;
    ULONG KldrSizeOfImage;
    ULONG KldrFullDllName;
    ULONG KldrBaseDllName;
    ULONG KldrFlags;
    ULONG DoDriverStart;
    ULONG DoDriverSize;
    ULONG DoDriverSection;
    ULONG DoMajorFunction;
    ULONG DoFastIoDispatch;
    ULONG DoDriverUnload;
    ULONG UldName;
    ULONG UldStartAddress;
    ULONG UldEndAddress;
    ULONG UldCurrentTime;
    ULONG UldTypeSize;
    ULONG RtlAvlBalancedRoot;
    ULONG RtlAvlOrderedPointer;
    ULONG RtlAvlWhichOrderedElement;
    ULONG RtlAvlNumberGenericTableElements;
    ULONG RtlAvlDepthOfTree;
    ULONG RtlAvlRestartKey;
    ULONG RtlAvlDeleteCount;
    ULONG RtlAvlTypeSize;
} KSW_DYN_KERNEL_OFFSETS, *PKSW_DYN_KERNEL_OFFSETS;

typedef struct _KSW_DYN_LXCORE_OFFSETS
{
    ULONG LxPicoProc;
    ULONG LxPicoProcInfo;
    ULONG LxPicoProcInfoPID;
    ULONG LxPicoThrdInfo;
    ULONG LxPicoThrdInfoTID;
} KSW_DYN_LXCORE_OFFSETS, *PKSW_DYN_LXCORE_OFFSETS;

/*
 * KSW_DYN_KERNEL_GLOBALS
 * Inputs:
 * - Populated from R3 PDB profile EX GlobalRva items after ntoskrnl identity
 *   matching.
 * Processing:
 * - Stores RVAs, not kernel virtual addresses, so consumers can validate them
 *   against the active image and derive addresses from the current image base.
 * Return behavior:
 * - Plain state container; no function-like return value.
 */
typedef struct _KSW_DYN_KERNEL_GLOBALS
{
    ULONG PspCidTable;
    ULONG PsLoadedModuleList;
    ULONG MmUnloadedDrivers;
    ULONG PiDDBCacheTable;
    ULONG KeServiceDescriptorTableShadow;
    ULONG MmLastUnloadedDriver;
} KSW_DYN_KERNEL_GLOBALS, *PKSW_DYN_KERNEL_GLOBALS;

typedef struct _KSW_DYN_CALLBACK_GLOBALS
{
    ULONG PspCreateProcessNotifyRoutine;
    ULONG PspCreateThreadNotifyRoutine;
    ULONG PspLoadImageNotifyRoutine;
    ULONG PspNotifyEnableMask;
    ULONG CmCallbackListHead;
} KSW_DYN_CALLBACK_GLOBALS, *PKSW_DYN_CALLBACK_GLOBALS;

typedef struct _KSW_DYN_CALLBACK_OFFSETS
{
    ULONG ObjectTypeCallbackList;
    ULONG CallbackEntryItemEntryList;
    ULONG CallbackEntryItemPreOperation;
    ULONG CallbackEntryItemPostOperation;
    ULONG CallbackEntryItemOperations;
    ULONG CallbackEntryItemCallbackEntry;
    ULONG CallbackEntryAltitude;
    ULONG CallbackEntryRegistrationContext;
} KSW_DYN_CALLBACK_OFFSETS, *PKSW_DYN_CALLBACK_OFFSETS;

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
    BOOLEAN PdbProfileActive;
    BOOLEAN CallbackProfileActive;
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
    KSW_DYN_KERNEL_OFFSETS KernelSources;
    KSW_DYN_LXCORE_OFFSETS LxcoreOffsets;
    KSW_DYN_LXCORE_OFFSETS LxcoreSources;
    KSW_DYN_KERNEL_GLOBALS KernelGlobals;
    KSW_DYN_KERNEL_GLOBALS KernelGlobalSources;
    KSW_DYN_CALLBACK_GLOBALS CallbackGlobals;
    KSW_DYN_CALLBACK_GLOBALS CallbackGlobalSources;
    KSW_DYN_CALLBACK_OFFSETS CallbackOffsets;
    KSW_DYN_CALLBACK_OFFSETS CallbackOffsetSources;
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

NTSTATUS
KswordARKDynDataApplyProfile(
    _In_reads_bytes_(InputBufferLength) const KSW_APPLY_DYN_PROFILE_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSW_APPLY_DYN_PROFILE_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataApplyProfileEx(
    _In_reads_bytes_(InputBufferLength) const KSW_APPLY_DYN_PROFILE_EX_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSW_APPLY_DYN_PROFILE_EX_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

VOID
KswordARKDynDataV4Initialize(
    VOID
    );

VOID
KswordARKDynDataV4Uninitialize(
    VOID
    );

NTSTATUS
KswordARKDynDataV4ApplyProfile(
    _In_reads_bytes_(InputBufferLength) const KSW_APPLY_DYN_PROFILE_V4_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSW_APPLY_DYN_PROFILE_V4_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataV4QueryModules(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataV4QueryCapabilityGroups(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataV4QueryMissingItems(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataV4QueryItems(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDynDataIoctlApplyProfileV4(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKDynDataIoctlQueryV4Modules(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKDynDataIoctlQueryV4CapabilityGroups(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKDynDataIoctlQueryV4MissingItems(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

NTSTATUS
KswordARKDynDataIoctlQueryV4Items(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    );

EXTERN_C_END
