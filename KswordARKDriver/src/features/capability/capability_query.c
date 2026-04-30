/*++

Module Name:

    capability_query.c

Abstract:

    Unified Phase 1 driver capability and status query implementation.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_capability.h"
#include "ark/ark_dyndata.h"

#include <ntstrsafe.h>

#define KSW_CAPABILITY_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE) - sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY))

typedef struct _KSW_CAP_LAST_ERROR
{
    NTSTATUS Status;
    CHAR Source[KSWORD_ARK_CAPABILITY_ERROR_SOURCE_CHARS];
    CHAR Summary[KSWORD_ARK_CAPABILITY_ERROR_SUMMARY_CHARS];
} KSW_CAP_LAST_ERROR, *PKSW_CAP_LAST_ERROR;

typedef struct _KSW_CAP_FEATURE_TEMPLATE
{
    ULONG FeatureId;
    PCSTR FeatureName;
    ULONG Flags;
    ULONG RequiredPolicyFlags;
    ULONG64 RequiredDynDataMask;
    PCSTR DependencyText;
} KSW_CAP_FEATURE_TEMPLATE, *PKSW_CAP_FEATURE_TEMPLATE;

static EX_PUSH_LOCK g_KswordArkCapabilityErrorLock;
static KSW_CAP_LAST_ERROR g_KswordArkLastCapabilityError;

static const KSW_CAP_FEATURE_TEMPLATE g_KswordArkFeatureTemplates[] = {
    { KSWORD_ARK_FEATURE_ID_DRIVER_HEALTH, "Driver health", KSWORD_ARK_FEATURE_FLAG_READ_ONLY, 0UL, 0ULL, "Driver loaded + protocol response" },
    { KSWORD_ARK_FEATURE_ID_PROCESS_BASIC_ACTIONS, "Process basic actions", KSWORD_ARK_FEATURE_FLAG_MUTATING | KSWORD_ARK_FEATURE_FLAG_POLICY_GATED, KSWORD_ARK_SECURITY_POLICY_ALLOW_MUTATING_ACTIONS, 0ULL, "Policy allows mutating process actions" },
    { KSWORD_ARK_FEATURE_ID_FILE_DELETE, "File delete", KSWORD_ARK_FEATURE_FLAG_MUTATING | KSWORD_ARK_FEATURE_FLAG_POLICY_GATED, KSWORD_ARK_SECURITY_POLICY_ALLOW_FILE_DELETE, 0ULL, "Policy allows file delete" },
    { KSWORD_ARK_FEATURE_ID_SSDT_SNAPSHOT, "SSDT snapshot", KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY | KSWORD_ARK_FEATURE_FLAG_READ_ONLY | KSWORD_ARK_FEATURE_FLAG_POLICY_GATED, KSWORD_ARK_SECURITY_POLICY_ALLOW_KERNEL_SNAPSHOTS, 0ULL, "Policy allows kernel snapshots" },
    { KSWORD_ARK_FEATURE_ID_CALLBACK_CONTROL, "Callback control", KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY | KSWORD_ARK_FEATURE_FLAG_MUTATING | KSWORD_ARK_FEATURE_FLAG_POLICY_GATED, KSWORD_ARK_SECURITY_POLICY_ALLOW_CALLBACK_CONTROL, 0ULL, "Policy allows callback control" },
    { KSWORD_ARK_FEATURE_ID_DYNDATA_STATUS, "DynData status", KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY | KSWORD_ARK_FEATURE_FLAG_READ_ONLY, 0UL, 0ULL, "DynData query IOCTLs available" },
    { KSWORD_ARK_FEATURE_ID_PROCESS_PROTECTION_PATCH, "Process protection patch", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_MUTATING | KSWORD_ARK_FEATURE_FLAG_POLICY_GATED, KSWORD_ARK_SECURITY_POLICY_ALLOW_PROCESS_PROTECTION, KSW_CAP_PROCESS_PROTECTION_PATCH, "EpProtection + EpSignatureLevel + EpSectionSignatureLevel" },
    { KSWORD_ARK_FEATURE_ID_PROCESS_HANDLE_TABLE, "Process handle table", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_PROCESS_OBJECT_TABLE | KSW_CAP_HANDLE_TABLE_DECODE, "EpObjectTable + ObDecodeShift + ObAttributesShift + OtName + OtIndex" },
    { KSWORD_ARK_FEATURE_ID_OBJECT_TYPE_FIELDS, "Object type fields", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_OBJECT_TYPE_FIELDS, "OtName + OtIndex" },
    { KSWORD_ARK_FEATURE_ID_THREAD_STACK_FIELDS, "Thread stack fields", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_THREAD_STACK_FIELDS, "KtInitialStack + KtStackLimit + KtStackBase + KtKernelStack" },
    { KSWORD_ARK_FEATURE_ID_THREAD_IO_COUNTERS, "Thread I/O counters", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_THREAD_IO_COUNTERS, "KTHREAD read/write/other operation and transfer counters" },
    { KSWORD_ARK_FEATURE_ID_ALPC_FIELDS, "ALPC fields", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_ALPC_FIELDS, "ALPC port and communication-info fields" },
    { KSWORD_ARK_FEATURE_ID_SECTION_CONTROL_AREA, "Section ControlArea", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_SECTION_CONTROL_AREA, "EpSectionObject + MmSectionControlArea + MmControlAreaListHead + MmControlAreaLock" },
    { KSWORD_ARK_FEATURE_ID_WSL_LXCORE_FIELDS, "WSL lxcore fields", KSWORD_ARK_FEATURE_FLAG_REQUIRES_DYNDATA | KSWORD_ARK_FEATURE_FLAG_KERNEL_ONLY, 0UL, KSW_CAP_WSL_LXCORE_FIELDS, "LxPicoProc + LxPicoProcInfo + PID/TID fields" }
};

VOID
KswordARKCapabilityInitialize(
    VOID
    )
/*++

Routine Description:

    Initialize global Phase 1 capability diagnostics state before the control
    device accepts IOCTL traffic.

Arguments:

    None.

Return Value:

    None.

--*/
{
    ExInitializePushLock(&g_KswordArkCapabilityErrorLock);
    RtlZeroMemory(&g_KswordArkLastCapabilityError, sizeof(g_KswordArkLastCapabilityError));
}

static VOID
KswordARKCapabilityCopyAnsi(
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes,
    _In_opt_z_ PCSTR Source
    )
/*++

Routine Description:

    Copy a bounded ANSI diagnostic string into a shared response field.

Arguments:

    Destination - Output character buffer.
    DestinationBytes - Output buffer capacity in bytes.
    Source - Optional NUL-terminated source string.

Return Value:

    None.

--*/
{
    if (Destination == NULL || DestinationBytes == 0U) {
        return;
    }

    Destination[0] = '\0';
    if (Source == NULL) {
        return;
    }

    (VOID)RtlStringCbCopyNA(Destination, DestinationBytes, Source, DestinationBytes - 1U);
    Destination[DestinationBytes - 1U] = '\0';
}

static ULONG
KswordARKCapabilityDynStatusFlags(
    _In_ const KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Convert a DynData state snapshot into public status flags.

Arguments:

    State - DynData snapshot.

Return Value:

    KSW_DYN_STATUS_FLAG_* bit mask.

--*/
{
    ULONG flags = 0UL;

    if (State == NULL) {
        return 0UL;
    }

    if (State->Initialized) {
        flags |= KSW_DYN_STATUS_FLAG_INITIALIZED;
    }
    if (State->NtosActive) {
        flags |= KSW_DYN_STATUS_FLAG_NTOS_ACTIVE;
    }
    if (State->LxcoreActive) {
        flags |= KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE;
    }
    if (State->ExtraActive) {
        flags |= KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE;
    }

    return flags;
}

static ULONG
KswordARKCapabilityCurrentSecurityPolicy(
    VOID
    )
/*++

Routine Description:

    Return the current conservative security policy. Phase 1 exposes the policy
    explicitly so R3 can display policy-denied features without guessing.

Arguments:

    None.

Return Value:

    KSWORD_ARK_SECURITY_POLICY_* bit mask.

--*/
{
    return KSWORD_ARK_SECURITY_POLICY_FLAG_ACTIVE |
        KSWORD_ARK_SECURITY_POLICY_ALLOW_ALL;
}

static PCSTR
KswordARKCapabilityStateName(
    _In_ ULONG State
    )
/*++

Routine Description:

    Convert a feature state enum into a stable short label.

Arguments:

    State - KSWORD_ARK_FEATURE_STATE_* value.

Return Value:

    Static ANSI state label.

--*/
{
    switch (State) {
    case KSWORD_ARK_FEATURE_STATE_AVAILABLE:
        return "Available";
    case KSWORD_ARK_FEATURE_STATE_UNAVAILABLE:
        return "Unavailable";
    case KSWORD_ARK_FEATURE_STATE_DEGRADED:
        return "Degraded";
    case KSWORD_ARK_FEATURE_STATE_DENIED_BY_POLICY:
        return "Denied by policy";
    default:
        return "Unknown";
    }
}

static ULONG
KswordARKCapabilityEvaluateFeatureState(
    _In_ const KSW_CAP_FEATURE_TEMPLATE* Feature,
    _In_ ULONG SecurityPolicyFlags,
    _In_ ULONG64 DynDataCapabilityMask,
    _Out_ ULONG* DeniedPolicyFlagsOut,
    _Out_ ULONG64* PresentDynDataMaskOut,
    _Out_ PCSTR* ReasonTextOut
    )
/*++

Routine Description:

    Evaluate one feature against security policy and DynData capability bits.

Arguments:

    Feature - Static feature template.
    SecurityPolicyFlags - Current policy bit mask.
    DynDataCapabilityMask - Current DynData capability bit mask.
    DeniedPolicyFlagsOut - Receives missing policy bits.
    PresentDynDataMaskOut - Receives present DynData dependency bits.
    ReasonTextOut - Receives a static diagnostic reason string.

Return Value:

    KSWORD_ARK_FEATURE_STATE_* value.

--*/
{
    ULONG deniedPolicyFlags = 0UL;
    ULONG64 presentDynDataMask = 0ULL;
    PCSTR reasonText = "Feature is available.";
    ULONG state = KSWORD_ARK_FEATURE_STATE_AVAILABLE;

    if (Feature == NULL || DeniedPolicyFlagsOut == NULL || PresentDynDataMaskOut == NULL || ReasonTextOut == NULL) {
        return KSWORD_ARK_FEATURE_STATE_UNKNOWN;
    }

    if (Feature->RequiredPolicyFlags != 0UL) {
        deniedPolicyFlags = Feature->RequiredPolicyFlags & (~SecurityPolicyFlags);
    }
    if (Feature->RequiredDynDataMask != 0ULL) {
        presentDynDataMask = DynDataCapabilityMask & Feature->RequiredDynDataMask;
    }

    if (deniedPolicyFlags != 0UL) {
        state = KSWORD_ARK_FEATURE_STATE_DENIED_BY_POLICY;
        reasonText = "Security policy denied one or more required operations.";
    }
    else if (Feature->RequiredDynDataMask != 0ULL && presentDynDataMask == 0ULL) {
        state = KSWORD_ARK_FEATURE_STATE_UNAVAILABLE;
        reasonText = "Required DynData capability bits are absent.";
    }
    else if (Feature->RequiredDynDataMask != 0ULL && presentDynDataMask != Feature->RequiredDynDataMask) {
        state = KSWORD_ARK_FEATURE_STATE_DEGRADED;
        reasonText = "Only part of the required DynData capability bits are present.";
    }

    *DeniedPolicyFlagsOut = deniedPolicyFlags;
    *PresentDynDataMaskOut = presentDynDataMask;
    *ReasonTextOut = reasonText;
    return state;
}

static ULONG
KswordARKCapabilityBuildFeatureEntries(
    _Out_writes_opt_(EntryCapacity) KSWORD_ARK_FEATURE_CAPABILITY_ENTRY* Entries,
    _In_ ULONG EntryCapacity,
    _In_ ULONG SecurityPolicyFlags,
    _In_ ULONG64 DynDataCapabilityMask
    )
/*++

Routine Description:

    Build public feature capability entries from the static matrix and current
    runtime state.

Arguments:

    Entries - Optional output array.
    EntryCapacity - Output array capacity in entries.
    SecurityPolicyFlags - Current security policy bits.
    DynDataCapabilityMask - Current DynData capability bits.

Return Value:

    Number of entries copied when Entries is present; otherwise total entries.

--*/
{
    ULONG index = 0UL;
    ULONG copied = 0UL;
    const ULONG totalCount = (ULONG)(sizeof(g_KswordArkFeatureTemplates) / sizeof(g_KswordArkFeatureTemplates[0]));

    if (Entries == NULL || EntryCapacity == 0UL) {
        return totalCount;
    }

    for (index = 0UL; index < totalCount && copied < EntryCapacity; ++index) {
        const KSW_CAP_FEATURE_TEMPLATE* feature = &g_KswordArkFeatureTemplates[index];
        KSWORD_ARK_FEATURE_CAPABILITY_ENTRY* entry = &Entries[copied];
        ULONG deniedPolicyFlags = 0UL;
        ULONG64 presentDynDataMask = 0ULL;
        PCSTR reasonText = NULL;
        ULONG state = KSWORD_ARK_FEATURE_STATE_UNKNOWN;

        state = KswordARKCapabilityEvaluateFeatureState(
            feature,
            SecurityPolicyFlags,
            DynDataCapabilityMask,
            &deniedPolicyFlags,
            &presentDynDataMask,
            &reasonText);

        RtlZeroMemory(entry, sizeof(*entry));
        entry->featureId = feature->FeatureId;
        entry->state = state;
        entry->flags = feature->Flags;
        entry->requiredPolicyFlags = feature->RequiredPolicyFlags;
        entry->deniedPolicyFlags = deniedPolicyFlags;
        entry->requiredDynDataMask = feature->RequiredDynDataMask;
        entry->presentDynDataMask = presentDynDataMask;
        KswordARKCapabilityCopyAnsi(entry->featureName, sizeof(entry->featureName), feature->FeatureName);
        KswordARKCapabilityCopyAnsi(entry->stateName, sizeof(entry->stateName), KswordARKCapabilityStateName(state));
        KswordARKCapabilityCopyAnsi(entry->dependencyText, sizeof(entry->dependencyText), feature->DependencyText);
        KswordARKCapabilityCopyAnsi(entry->reasonText, sizeof(entry->reasonText), reasonText);
        copied += 1UL;
    }

    return copied;
}

VOID
KswordARKCapabilityRecordLastError(
    _In_ NTSTATUS Status,
    _In_z_ PCSTR SourceText,
    _In_z_ PCSTR SummaryText
    )
/*++

Routine Description:

    Record the latest driver error summary for Phase 1 diagnostics.

Arguments:

    Status - Failing NTSTATUS.
    SourceText - Short subsystem name.
    SummaryText - Human-readable summary.

Return Value:

    None.

--*/
{
    ExAcquirePushLockExclusive(&g_KswordArkCapabilityErrorLock);
    g_KswordArkLastCapabilityError.Status = Status;
    KswordARKCapabilityCopyAnsi(g_KswordArkLastCapabilityError.Source, sizeof(g_KswordArkLastCapabilityError.Source), SourceText);
    KswordARKCapabilityCopyAnsi(g_KswordArkLastCapabilityError.Summary, sizeof(g_KswordArkLastCapabilityError.Summary), SummaryText);
    ExReleasePushLockExclusive(&g_KswordArkCapabilityErrorLock);
}

BOOLEAN
KswordARKCapabilityIsIoctlAllowed(
    _In_ ULONG64 RequiredCapability,
    _Out_opt_ NTSTATUS* DeniedStatusOut
    )
/*++

Routine Description:

    Fail closed for IOCTLs that require private DynData capabilities.

Arguments:

    RequiredCapability - Required KSW_CAP_* dependency mask.
    DeniedStatusOut - Optional denied status output.

Return Value:

    TRUE when allowed; FALSE when the required capability is unavailable.

--*/
{
    KSW_DYN_STATE dynState;

    if (DeniedStatusOut != NULL) {
        *DeniedStatusOut = STATUS_SUCCESS;
    }
    if (RequiredCapability == 0ULL) {
        return TRUE;
    }

    KswordARKDynDataSnapshot(&dynState);
    if ((dynState.CapabilityMask & RequiredCapability) == RequiredCapability) {
        return TRUE;
    }

    if (DeniedStatusOut != NULL) {
        *DeniedStatusOut = STATUS_NOT_SUPPORTED;
    }
    return FALSE;
}

NTSTATUS
KswordARKCapabilityQuery(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Write the unified driver capability response for R3 state and matrix UI.

Arguments:

    OutputBuffer - METHOD_BUFFERED output buffer.
    OutputBufferLength - Writable output byte count.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS when the response header is written; otherwise validation
    status.

--*/
{
    KSW_DYN_STATE dynState;
    KSW_CAP_LAST_ERROR lastError;
    KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE* response = NULL;
    ULONG securityPolicyFlags = 0UL;
    ULONG totalCount = 0UL;
    ULONG entryCapacity = 0UL;
    ULONG returnedCount = 0UL;
    ULONG statusFlags = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSW_CAPABILITY_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KswordARKDynDataSnapshot(&dynState);
    securityPolicyFlags = KswordARKCapabilityCurrentSecurityPolicy();
    totalCount = KswordARKCapabilityBuildFeatureEntries(NULL, 0UL, securityPolicyFlags, dynState.CapabilityMask);

    RtlZeroMemory(&lastError, sizeof(lastError));
    ExAcquirePushLockShared(&g_KswordArkCapabilityErrorLock);
    RtlCopyMemory(&lastError, &g_KswordArkLastCapabilityError, sizeof(lastError));
    ExReleasePushLockShared(&g_KswordArkCapabilityErrorLock);

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE*)OutputBuffer;
    response->size = (ULONG)KSW_CAPABILITY_RESPONSE_HEADER_SIZE;
    response->version = KSWORD_ARK_DRIVER_CAPABILITY_PROTOCOL_VERSION;
    response->driverProtocolVersion = KSWORD_ARK_DRIVER_PROTOCOL_VERSION;
    response->securityPolicyFlags = securityPolicyFlags;
    response->dynDataStatusFlags = KswordARKCapabilityDynStatusFlags(&dynState);
    response->lastErrorStatus = (LONG)lastError.Status;
    response->totalFeatureCount = totalCount;
    response->entrySize = sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY);
    response->dynDataCapabilityMask = dynState.CapabilityMask;

    statusFlags = KSWORD_ARK_DRIVER_STATUS_FLAG_DRIVER_LOADED |
        KSWORD_ARK_DRIVER_STATUS_FLAG_PROTOCOL_OK |
        KSWORD_ARK_DRIVER_STATUS_FLAG_SECURITY_POLICY_ON;
    if (dynState.Initialized) {
        statusFlags |= KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_INITIALIZED;
    }
    if (dynState.NtosActive) {
        statusFlags |= KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_ACTIVE;
    }
    else {
        statusFlags |= KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_MISSING | KSWORD_ARK_DRIVER_STATUS_FLAG_LIMITED;
    }
    if (!NT_SUCCESS(lastError.Status) && lastError.Status != 0) {
        statusFlags |= KSWORD_ARK_DRIVER_STATUS_FLAG_LAST_ERROR_PRESENT;
    }
    response->statusFlags = statusFlags;

    KswordARKCapabilityCopyAnsi(response->lastErrorSource, sizeof(response->lastErrorSource), lastError.Source);
    KswordARKCapabilityCopyAnsi(response->lastErrorSummary, sizeof(response->lastErrorSummary), lastError.Summary);

    entryCapacity = (ULONG)((OutputBufferLength - KSW_CAPABILITY_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY));
    if (entryCapacity > 0UL) {
        returnedCount = KswordARKCapabilityBuildFeatureEntries(
            response->entries,
            entryCapacity,
            securityPolicyFlags,
            dynState.CapabilityMask);
    }

    response->returnedFeatureCount = returnedCount;
    response->size = (ULONG)(KSW_CAPABILITY_RESPONSE_HEADER_SIZE + ((size_t)returnedCount * sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY)));
    *BytesWrittenOut = response->size;
    return STATUS_SUCCESS;
}
