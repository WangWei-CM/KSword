#pragma once

#include <fltKernel.h>
#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdf.h>

#include "ark/ark_callback.h"
#include "ark/ark_log.h"

#define KSWORD_ARK_CALLBACK_TAG_RUNTIME 'rCbK'
#define KSWORD_ARK_CALLBACK_TAG_SNAPSHOT 'sCbK'
#define KSWORD_ARK_CALLBACK_TAG_PENDING 'pCbK'

#define KSWORD_ARK_CALLBACK_REGISTERED_REGISTRY 0x00000001UL
#define KSWORD_ARK_CALLBACK_REGISTERED_PROCESS 0x00000002UL
#define KSWORD_ARK_CALLBACK_REGISTERED_THREAD 0x00000004UL
#define KSWORD_ARK_CALLBACK_REGISTERED_IMAGE 0x00000008UL
#define KSWORD_ARK_CALLBACK_REGISTERED_OBJECT 0x00000010UL
#define KSWORD_ARK_CALLBACK_REGISTERED_MINIFILTER 0x00000020UL

typedef struct _KSWORD_ARK_CALLBACK_RULE_SNAPSHOT KSWORD_ARK_CALLBACK_RULE_SNAPSHOT;
typedef struct _KSWORD_ARK_PENDING_DECISION KSWORD_ARK_PENDING_DECISION;

typedef struct _KSWORD_ARK_CALLBACK_MATCH_RESULT
{
    BOOLEAN Matched;
    ULONG CallbackType;
    ULONG RuleOperationMask;
    ULONG Action;
    ULONG MatchMode;
    ULONG AskTimeoutMs;
    ULONG AskDefaultDecision;
    ULONG GroupId;
    ULONG RuleId;
    ULONG GroupPriority;
    ULONG RulePriority;
    WCHAR GroupName[KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS];
    WCHAR RuleName[KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS];
    WCHAR RuleInitiatorPattern[KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS];
    WCHAR RuleTargetPattern[KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS];
} KSWORD_ARK_CALLBACK_MATCH_RESULT;

typedef struct _KSWORD_ARK_CALLBACK_EVENT_INPUT
{
    ULONG CallbackType;
    ULONG OperationType;
    ULONG OriginatingPid;
    ULONG OriginatingTid;
    ULONG SessionId;
    ULONG PathUnavailable;
    UNICODE_STRING InitiatorPath;
    UNICODE_STRING TargetPath;
    KSWORD_ARK_CALLBACK_MATCH_RESULT Match;
} KSWORD_ARK_CALLBACK_EVENT_INPUT;

typedef struct _KSWORD_ARK_RUNTIME_RULE
{
    ULONG GroupId;
    ULONG RuleId;
    ULONG GroupPriority;
    ULONG RulePriority;
    ULONG CallbackType;
    ULONG OperationMask;
    ULONG Action;
    ULONG MatchMode;
    ULONG AskTimeoutMs;
    ULONG AskDefaultDecision;
    UNICODE_STRING InitiatorPattern;
    UNICODE_STRING TargetPattern;
    UNICODE_STRING GroupName;
    UNICODE_STRING RuleName;
} KSWORD_ARK_RUNTIME_RULE;

typedef struct _KSWORD_ARK_CALLBACK_RULE_SNAPSHOT
{
    EX_RUNDOWN_REF RundownRef;
    ULONG GlobalFlags;
    ULONG GroupCount;
    ULONG RuleCount;
    ULONG ActiveRuleCount;
    ULONG StringPoolBytes;
    ULONG BucketStart[KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED + 1];
    ULONG BucketCount[KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED + 1];
    ULONG64 RuleVersion;
    LARGE_INTEGER AppliedAtUtc100ns;
    KSWORD_ARK_RUNTIME_RULE Rules[1];
} KSWORD_ARK_CALLBACK_RULE_SNAPSHOT;

typedef struct _KSWORD_ARK_CALLBACK_RUNTIME
{
    WDFDEVICE Device;
    WDFQUEUE WaitQueue;

    EX_PUSH_LOCK SnapshotLock;
    KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* ActiveSnapshot;

    EX_PUSH_LOCK PendingLock;
    LIST_ENTRY PendingDecisionList;
    volatile LONG PendingDecisionCount;
    volatile LONG64 EventSequence;

    LARGE_INTEGER RegistryCookie;
    PVOID ObRegistrationHandle;
    PFLT_FILTER MiniFilterHandle;
    BOOLEAN MiniFilterStarted;
    NTSTATUS MiniFilterRegisterStatus;
    NTSTATUS MiniFilterStartStatus;
    ULONG RegisteredCallbacksMask;
    BOOLEAN Initialized;
} KSWORD_ARK_CALLBACK_RUNTIME;

typedef struct _KSWORD_ARK_PENDING_DECISION
{
    LIST_ENTRY Link;
    LONG RefCount;
    volatile LONG Answered;
    KSWORD_ARK_GUID128 EventGuid;
    KEVENT DecisionEvent;
    ULONG FinalDecision;
    ULONG DefaultDecision;
    ULONG CallbackType;
    ULONG OperationType;
    ULONG OriginatingPid;
    ULONG OriginatingTid;
    ULONG SessionId;
    ULONG PathUnavailable;
    ULONG TimeoutMs;
    LARGE_INTEGER CreatedAtUtc100ns;
    LARGE_INTEGER DeadlineUtc100ns;
    KSWORD_ARK_CALLBACK_MATCH_RESULT Match;
    WCHAR InitiatorPath[KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS];
    WCHAR TargetPath[KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS];
} KSWORD_ARK_PENDING_DECISION;

EXTERN_C_START

KSWORD_ARK_CALLBACK_RUNTIME*
KswordArkCallbackGetRuntime(
    VOID
    );

VOID
KswordArkCallbackLogFrame(
    _In_z_ PCSTR levelText,
    _In_z_ PCSTR messageText
    );

VOID
KswordArkCallbackLogFormat(
    _In_z_ PCSTR levelText,
    _In_z_ _Printf_format_string_ PCSTR formatText,
    ...
    );

VOID
KswordArkGetSystemTimeUtc100ns(
    _Out_ LARGE_INTEGER* utcOut
    );

VOID
KswordArkCopyUnicodeToFixedBuffer(
    _In_opt_ PCUNICODE_STRING sourceText,
    _Out_writes_(destinationChars) PWCHAR destinationBuffer,
    _In_ USHORT destinationChars
    );

VOID
KswordArkCopyWideStringToFixedBuffer(
    _In_opt_z_ PCWSTR sourceText,
    _Out_writes_(destinationChars) PWCHAR destinationBuffer,
    _In_ USHORT destinationChars
    );

BOOLEAN
KswordArkResolveProcessImagePath(
    _In_opt_ PEPROCESS processObject,
    _Out_writes_(destinationChars) PWCHAR destinationBuffer,
    _In_ USHORT destinationChars,
    _Out_opt_ BOOLEAN* pathUnavailableOut
    );

ULONG
KswordArkGetProcessSessionIdSafe(
    _In_opt_ PEPROCESS processObject
    );

BOOLEAN
KswordArkGuidEquals(
    _In_ const KSWORD_ARK_GUID128* leftGuid,
    _In_ const KSWORD_ARK_GUID128* rightGuid
    );

PVOID
KswordArkAllocateNonPaged(
    _In_ SIZE_T bytes,
    _In_ ULONG poolTag
    );

VOID
KswordArkGuidGenerate(
    _Out_ KSWORD_ARK_GUID128* guidOut
    );

NTSTATUS
KswordArkCallbackBuildSnapshotFromBlob(
    _In_reads_bytes_(blobBytes) const VOID* blobData,
    _In_ size_t blobBytes,
    _Outptr_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT** snapshotOut
    );

VOID
KswordArkCallbackFreeSnapshot(
    _In_opt_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* snapshot
    );

NTSTATUS
KswordArkCallbackSwapSnapshot(
    _In_ KSWORD_ARK_CALLBACK_RULE_SNAPSHOT* newSnapshot
    );

VOID
KswordArkCallbackQueryRuntimeState(
    _Out_ KSWORD_ARK_CALLBACK_RUNTIME_STATE* runtimeStateOut
    );

NTSTATUS
KswordArkCallbackMatchRule(
    _In_ ULONG callbackType,
    _In_ ULONG operationType,
    _In_opt_ PCUNICODE_STRING initiatorPath,
    _In_opt_ PCUNICODE_STRING targetPath,
    _Out_ KSWORD_ARK_CALLBACK_MATCH_RESULT* matchResultOut
    );

NTSTATUS
KswordArkCallbackWaiterInitialize(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkCallbackWaiterUninitialize(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

NTSTATUS
KswordArkCallbackIoctlWaitEventInternal(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

NTSTATUS
KswordArkCallbackIoctlAnswerEventInternal(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

NTSTATUS
KswordArkCallbackCancelAllPendingInternal(
    VOID
    );

NTSTATUS
KswordArkCallbackAskUserDecision(
    _In_ const KSWORD_ARK_CALLBACK_EVENT_INPUT* eventInput,
    _Out_ ULONG* decisionOut
    );

ULONG
KswordArkCallbackGetWaitingRequestCount(
    VOID
    );

ULONG
KswordArkCallbackGetPendingDecisionCount(
    VOID
    );

NTSTATUS
KswordArkRegistryCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkRegistryCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

NTSTATUS
KswordArkProcessCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkProcessCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

NTSTATUS
KswordArkThreadCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkThreadCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

NTSTATUS
KswordArkImageCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkImageCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

NTSTATUS
KswordArkObjectCallbackRegister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkObjectCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkMinifilterCallbackUnregister(
    _In_ KSWORD_ARK_CALLBACK_RUNTIME* runtime
    );

VOID
KswordArkMinifilterCallbackUpdateState(
    _In_opt_ PFLT_FILTER FilterHandle,
    _In_ NTSTATUS RegisterStatus,
    _In_ NTSTATUS StartStatus,
    _In_ BOOLEAN Started
    );

FLT_PREOP_CALLBACK_STATUS
FLTAPI
KswordArkMinifilterPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
    );

FLT_POSTOP_CALLBACK_STATUS
FLTAPI
KswordArkMinifilterPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    );

FLT_PREOP_CALLBACK_STATUS
KswordArkMinifilterApplyRule(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ ULONG OperationType
    );

EXTERN_C_END
