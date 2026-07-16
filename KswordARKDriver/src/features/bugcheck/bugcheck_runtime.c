/*++

Module Name:

    bugcheck_runtime.c

Abstract:

    VMware-gated bugcheck callback registration and nonpaged diagnostic state.

--*/

#include "bugcheck_internal.h"

#include <aux_klib.h>
#include <ntstrsafe.h>

#define KSWORD_ARK_BUGCHECK_POOL_TAG 'cbSK'
#define KSWORD_ARK_BUGCHECK_SECONDARY_SIGNATURE 0x4442534BUL /* 'KSBD' */

typedef struct _KSWORD_ARK_BUGCHECK_SECONDARY_DATA
{
    ULONG Signature;
    ULONG Version;
    ULONG Size;
    ULONG Reserved;
    KSWORD_ARK_BUGCHECK_DIAGNOSTICS Diagnostics;
} KSWORD_ARK_BUGCHECK_SECONDARY_DATA;

KSWORD_ARK_BUGCHECK_STATE g_KswordArkBugcheckState;
UCHAR g_KswordArkBugcheckBitmapPixels[KSWORD_ARK_BUGCHECK_BITMAP_MAX_BYTES];

static UCHAR g_KswordArkBugcheckComponent[] = "KswordARK";
static KSWORD_ARK_BUGCHECK_SECONDARY_DATA g_KswordArkBugcheckSecondaryData;
static const GUID g_KswordArkBugcheckSecondaryGuid =
{ 0x956d0947, 0x326a, 0x4ba7, { 0x92, 0xf1, 0x4c, 0x8b, 0x5a, 0x5c, 0x71, 0x2d } };

static CHAR
KswordARKBugcheckLowerA(
    _In_ CHAR Value
    )
{
    return (Value >= 'A' && Value <= 'Z') ? (CHAR)(Value - 'A' + 'a') : Value;
}

static BOOLEAN
KswordARKBugcheckEqualsNoCaseA(
    _In_z_ PCSTR Left,
    _In_z_ PCSTR Right
    )
{
    if (Left == NULL || Right == NULL) {
        return FALSE;
    }

    while (*Left != '\0' && *Right != '\0') {
        if (KswordARKBugcheckLowerA(*Left) != KswordARKBugcheckLowerA(*Right)) {
            return FALSE;
        }
        ++Left;
        ++Right;
    }

    return (*Left == '\0' && *Right == '\0') ? TRUE : FALSE;
}

static BOOLEAN
KswordARKBugcheckStartsWithNoCaseA(
    _In_z_ PCSTR Text,
    _In_z_ PCSTR Prefix
    )
{
    if (Text == NULL || Prefix == NULL) {
        return FALSE;
    }

    while (*Prefix != '\0') {
        if (*Text == '\0' ||
            KswordARKBugcheckLowerA(*Text) != KswordARKBugcheckLowerA(*Prefix)) {
            return FALSE;
        }
        ++Text;
        ++Prefix;
    }
    return TRUE;
}

static BOOLEAN
KswordARKBugcheckContainsNoCaseA(
    _In_z_ PCSTR Text,
    _In_z_ PCSTR Needle
    )
{
    PCSTR cursor;

    if (Text == NULL || Needle == NULL || Needle[0] == '\0') {
        return FALSE;
    }

    for (cursor = Text; *cursor != '\0'; ++cursor) {
        if (KswordARKBugcheckStartsWithNoCaseA(cursor, Needle)) {
            return TRUE;
        }
    }
    return FALSE;
}

static ULONG
KswordARKBugcheckClassifyModuleName(
    _In_z_ PCSTR Name
    )
{
    static const PCSTR knownMicrosoftModules[] = {
        "ntoskrnl.exe", "hal.dll", "kdcom.dll", "bootvid.dll", "ci.dll",
        "clfs.sys", "cng.sys", "acpi.sys", "pci.sys", "partmgr.sys",
        "volmgr.sys", "volsnap.sys", "disk.sys", "classpnp.sys",
        "storport.sys", "stornvme.sys", "ntfs.sys", "fltmgr.sys",
        "ndis.sys", "tcpip.sys", "afd.sys", "wdf01000.sys",
        "watchdog.sys", "dxgkrnl.sys", "basicdisplay.sys",
        "basicrender.sys", "win32k.sys"
    };
    ULONG index;

    if (Name == NULL || Name[0] == '\0') {
        return KSWORD_ARK_BUGCHECK_MODULE_UNKNOWN;
    }

    if (KswordARKBugcheckEqualsNoCaseA(Name, "KswordARK.sys") ||
        KswordARKBugcheckContainsNoCaseA(Name, "kswordark")) {
        return KSWORD_ARK_BUGCHECK_MODULE_OURS;
    }

    for (index = 0; index < RTL_NUMBER_OF(knownMicrosoftModules); ++index) {
        if (KswordARKBugcheckEqualsNoCaseA(Name, knownMicrosoftModules[index])) {
            return KSWORD_ARK_BUGCHECK_MODULE_MICROSOFT;
        }
    }

    if (KswordARKBugcheckStartsWithNoCaseA(Name, "win32k") ||
        KswordARKBugcheckStartsWithNoCaseA(Name, "dxgmms") ||
        KswordARKBugcheckStartsWithNoCaseA(Name, "ksec") ||
        KswordARKBugcheckStartsWithNoCaseA(Name, "msrpc") ||
        KswordARKBugcheckStartsWithNoCaseA(Name, "netio") ||
        KswordARKBugcheckStartsWithNoCaseA(Name, "spaceport") ||
        KswordARKBugcheckStartsWithNoCaseA(Name, "iorate")) {
        return KSWORD_ARK_BUGCHECK_MODULE_MICROSOFT;
    }

    return KSWORD_ARK_BUGCHECK_MODULE_THIRD_PARTY;
}

PCSTR
KswordARKBugcheckName(
    _In_ ULONG BugCheckCode
    )
{
    switch (BugCheckCode) {
    case 0x0000000A: return "IRQL_NOT_LESS_OR_EQUAL";
    case 0x0000001A: return "MEMORY_MANAGEMENT";
    case 0x0000001E: return "KMODE_EXCEPTION_NOT_HANDLED";
    case 0x00000024: return "NTFS_FILE_SYSTEM";
    case 0x0000002E: return "DATA_BUS_ERROR";
    case 0x0000003B: return "SYSTEM_SERVICE_EXCEPTION";
    case 0x00000050: return "PAGE_FAULT_IN_NONPAGED_AREA";
    case 0x0000007E: return "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED";
    case 0x0000007F: return "UNEXPECTED_KERNEL_MODE_TRAP";
    case 0x0000009F: return "DRIVER_POWER_STATE_FAILURE";
    case 0x000000A0: return "INTERNAL_POWER_ERROR";
    case 0x000000BE: return "ATTEMPTED_WRITE_TO_READONLY_MEMORY";
    case 0x000000C2: return "BAD_POOL_CALLER";
    case 0x000000C4: return "DRIVER_VERIFIER_DETECTED_VIOLATION";
    case 0x000000C5: return "DRIVER_CORRUPTED_EXPOOL";
    case 0x000000C9: return "DRIVER_VERIFIER_IOMANAGER_VIOLATION";
    case 0x000000D1: return "DRIVER_IRQL_NOT_LESS_OR_EQUAL";
    case 0x000000D5: return "DRIVER_PAGE_FAULT_IN_FREED_SPECIAL_POOL";
    case 0x000000EA: return "THREAD_STUCK_IN_DEVICE_DRIVER";
    case 0x000000F7: return "DRIVER_OVERRAN_STACK_BUFFER";
    case 0x00000109: return "CRITICAL_STRUCTURE_CORRUPTION";
    case 0x00000116: return "VIDEO_TDR_FAILURE";
    case 0x00000117: return "VIDEO_TDR_TIMEOUT_DETECTED";
    case 0x00000119: return "VIDEO_SCHEDULER_INTERNAL_ERROR";
    case 0x00000124: return "WHEA_UNCORRECTABLE_ERROR";
    case 0x00000133: return "DPC_WATCHDOG_VIOLATION";
    case 0x00000139: return "KERNEL_SECURITY_CHECK_FAILURE";
    case 0x0000013A: return "KERNEL_MODE_HEAP_CORRUPTION";
    default: return "UNKNOWN_BUGCHECK_CODE";
    }
}

PCSTR
KswordARKBugcheckModuleClassText(
    _In_ ULONG Classification
    )
{
    switch (Classification) {
    case KSWORD_ARK_BUGCHECK_MODULE_OURS: return "OUR_DRIVER";
    case KSWORD_ARK_BUGCHECK_MODULE_MICROSOFT: return "MICROSOFT_KNOWN";
    case KSWORD_ARK_BUGCHECK_MODULE_THIRD_PARTY: return "THIRD_PARTY";
    default: return "UNKNOWN";
    }
}

PCSTR
KswordARKBugcheckConfidenceText(
    _In_ ULONG Confidence
    )
{
    switch (Confidence) {
    case KSWORD_ARK_BUGCHECK_CONFIDENCE_HIGH: return "HIGH";
    case KSWORD_ARK_BUGCHECK_CONFIDENCE_MEDIUM: return "MEDIUM";
    case KSWORD_ARK_BUGCHECK_CONFIDENCE_LOW: return "LOW";
    default: return "NONE";
    }
}

PCSTR
KswordARKBugcheckVerdictText(
    _In_ ULONG Classification
    )
{
    switch (Classification) {
    case KSWORD_ARK_BUGCHECK_MODULE_OURS:
        return "KswordARK may be involved. Capture this page and attach the crash dump when reporting the issue.";
    case KSWORD_ARK_BUGCHECK_MODULE_MICROSOFT:
        return "The available crash parameters point to a known Microsoft kernel component.";
    case KSWORD_ARK_BUGCHECK_MODULE_THIRD_PARTY:
        return "The available crash parameters point to another third-party kernel component.";
    default:
        return "The faulting component is unknown. Capture this page and preserve the crash dump.";
    }
}

PCSTR
KswordARKBugcheckDumpTypeText(
    _In_ ULONG DumpType
    )
{
    switch (DumpType) {
    case KbDumpIoHeader: return "Header";
    case KbDumpIoBody: return "Body";
    case KbDumpIoSecondaryData: return "SecondaryData";
    case KbDumpIoComplete: return "Complete";
    default: return "Unknown";
    }
}

PCSTR
KswordARKBugcheckReasonText(
    _In_ ULONG Reason
    )
{
    switch (Reason) {
    case KbCallbackInvalid: return "Invalid";
    case KbCallbackSecondaryDumpData: return "SecondaryDumpData";
    case KbCallbackDumpIo: return "DumpIo";
    case KbCallbackAddPages: return "AddPages";
    case KbCallbackSecondaryMultiPartDumpData: return "SecondaryMultiPartDumpData";
    case KbCallbackRemovePages: return "RemovePages";
    case KbCallbackTriageDumpData: return "TriageDumpData";
    default: return "Unknown";
    }
}

static VOID
KswordARKBugcheckRefreshModuleCache(
    VOID
    )
{
    NTSTATUS status;
    ULONG bytes = 0;
    ULONG count;
    ULONG index;
    ULONG stored = 0;
    PAUX_MODULE_EXTENDED_INFO modules;
    PCSTR name;

    g_KswordArkBugcheckState.ModuleCount = 0;
    RtlZeroMemory(g_KswordArkBugcheckState.Modules,
                  sizeof(g_KswordArkBugcheckState.Modules));

    status = AuxKlibInitialize();
    if (!NT_SUCCESS(status)) {
        return;
    }

    status = AuxKlibQueryModuleInformation(
        &bytes,
        sizeof(AUX_MODULE_EXTENDED_INFO),
        NULL);
    if (!NT_SUCCESS(status) || bytes == 0) {
        return;
    }

    modules = (PAUX_MODULE_EXTENDED_INFO)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        bytes,
        KSWORD_ARK_BUGCHECK_POOL_TAG);
    if (modules == NULL) {
        return;
    }

    RtlZeroMemory(modules, bytes);
    status = AuxKlibQueryModuleInformation(
        &bytes,
        sizeof(AUX_MODULE_EXTENDED_INFO),
        modules);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(modules, KSWORD_ARK_BUGCHECK_POOL_TAG);
        return;
    }

    count = bytes / sizeof(AUX_MODULE_EXTENDED_INFO);
    for (index = 0;
         index < count && stored < KSWORD_ARK_BUGCHECK_MODULE_CACHE_COUNT;
         ++index) {
        name = (PCSTR)modules[index].FullPathName;
        if (modules[index].FileNameOffset < AUX_KLIB_MODULE_PATH_LEN) {
            name = (PCSTR)&modules[index].FullPathName[modules[index].FileNameOffset];
        }

        g_KswordArkBugcheckState.Modules[stored].Base =
            (ULONG_PTR)modules[index].BasicInfo.ImageBase;
        g_KswordArkBugcheckState.Modules[stored].Size = modules[index].ImageSize;
        (VOID)RtlStringCbCopyA(
            g_KswordArkBugcheckState.Modules[stored].Name,
            sizeof(g_KswordArkBugcheckState.Modules[stored].Name),
            name);
        g_KswordArkBugcheckState.Modules[stored]
            .Name[KSWORD_ARK_BUGCHECK_MODULE_NAME_CHARS - 1] = '\0';
        g_KswordArkBugcheckState.Modules[stored].Classification =
            KswordARKBugcheckClassifyModuleName(
                g_KswordArkBugcheckState.Modules[stored].Name);
        ++stored;
    }

    g_KswordArkBugcheckState.ModuleCount = stored;
    ExFreePoolWithTag(modules, KSWORD_ARK_BUGCHECK_POOL_TAG);
}

static PKSWORD_ARK_BUGCHECK_MODULE_ENTRY
KswordARKBugcheckFindModuleForAddress(
    _In_ ULONG_PTR Address
    )
{
    ULONG index;

    if (Address < 0x10000ULL) {
        return NULL;
    }

    for (index = 0; index < g_KswordArkBugcheckState.ModuleCount; ++index) {
        const ULONG_PTR base = g_KswordArkBugcheckState.Modules[index].Base;
        const ULONG_PTR end = base + g_KswordArkBugcheckState.Modules[index].Size;
        if (Address >= base && Address < end) {
            return &g_KswordArkBugcheckState.Modules[index];
        }
    }
    return NULL;
}

static VOID
KswordARKBugcheckSetCandidate(
    _Inout_ PKSWORD_ARK_BUGCHECK_DIAGNOSTICS Diagnostics,
    _In_ PKSWORD_ARK_BUGCHECK_MODULE_ENTRY Module,
    _In_ ULONG_PTR Address,
    _In_ ULONG Confidence,
    _In_ ULONG ParameterIndex,
    _In_z_ PCSTR Source
    )
{
    Diagnostics->CandidateAddress = Address;
    Diagnostics->CandidateModuleBase = Module->Base;
    Diagnostics->CandidateModuleSize = Module->Size;
    Diagnostics->CandidateModuleOffset = Address >= Module->Base
        ? Address - Module->Base
        : 0;
    Diagnostics->CandidateParameter = ParameterIndex;
    Diagnostics->CandidateClass = Module->Classification;
    Diagnostics->CandidateConfidence = Confidence;
    (VOID)RtlStringCbCopyA(
        Diagnostics->CandidateModule,
        sizeof(Diagnostics->CandidateModule),
        Module->Name);
    (VOID)RtlStringCbCopyA(
        Diagnostics->CandidateSource,
        sizeof(Diagnostics->CandidateSource),
        Source);
}

static BOOLEAN
KswordARKBugcheckSelectPrimaryAddress(
    _Inout_ PKSWORD_ARK_BUGCHECK_DIAGNOSTICS Diagnostics,
    _Out_ PULONG_PTR Address,
    _Out_ PULONG ParameterIndex,
    _Out_ PULONG Confidence
    )
{
    PCSTR meaning = "no known address parameter";

    *Address = 0;
    *ParameterIndex = 0;
    *Confidence = KSWORD_ARK_BUGCHECK_CONFIDENCE_NONE;

    switch (Diagnostics->BugCheckCode) {
    case 0x0000000A:
    case 0x000000D1:
        *Address = Diagnostics->Parameter4;
        *ParameterIndex = 4;
        *Confidence = KSWORD_ARK_BUGCHECK_CONFIDENCE_HIGH;
        meaning = "instruction pointer that referenced memory";
        break;
    case 0x0000001E:
    case 0x0000003B:
    case 0x0000007E:
        *Address = Diagnostics->Parameter2;
        *ParameterIndex = 2;
        *Confidence = KSWORD_ARK_BUGCHECK_CONFIDENCE_HIGH;
        meaning = "exception or instruction address";
        break;
    case 0x00000050:
    case 0x000000BE:
    case 0x00000116:
    case 0x00000117:
        *Address = Diagnostics->Parameter3;
        *ParameterIndex = 3;
        *Confidence = KSWORD_ARK_BUGCHECK_CONFIDENCE_MEDIUM;
        meaning = "probable instruction or fault address";
        break;
    default:
        break;
    }

    Diagnostics->FaultAddress = *Address;
    Diagnostics->FaultParameter = *ParameterIndex;
    (VOID)RtlStringCbCopyA(
        Diagnostics->FaultMeaning,
        sizeof(Diagnostics->FaultMeaning),
        meaning);
    return (*Address != 0 && *Confidence != KSWORD_ARK_BUGCHECK_CONFIDENCE_NONE)
        ? TRUE
        : FALSE;
}

static VOID
KswordARKBugcheckResolveCandidate(
    _Inout_ PKSWORD_ARK_BUGCHECK_DIAGNOSTICS Diagnostics
    )
{
    ULONG_PTR candidates[4];
    ULONG_PTR primaryAddress;
    ULONG primaryParameter;
    ULONG primaryConfidence;
    ULONG index;
    PKSWORD_ARK_BUGCHECK_MODULE_ENTRY module;

    Diagnostics->CandidateAddress = 0;
    Diagnostics->CandidateModuleBase = 0;
    Diagnostics->CandidateModuleOffset = 0;
    Diagnostics->CandidateModuleSize = 0;
    Diagnostics->CandidateParameter = 0;
    Diagnostics->CandidateClass = KSWORD_ARK_BUGCHECK_MODULE_UNKNOWN;
    Diagnostics->CandidateConfidence = KSWORD_ARK_BUGCHECK_CONFIDENCE_NONE;
    (VOID)RtlStringCbCopyA(
        Diagnostics->CandidateModule,
        sizeof(Diagnostics->CandidateModule),
        "(none)");
    (VOID)RtlStringCbCopyA(
        Diagnostics->CandidateSource,
        sizeof(Diagnostics->CandidateSource),
        "none");
    Diagnostics->FaultAddress = 0;
    Diagnostics->FaultParameter = 0;
    (VOID)RtlStringCbCopyA(
        Diagnostics->FaultMeaning,
        sizeof(Diagnostics->FaultMeaning),
        "not classified");

    if (KswordARKBugcheckSelectPrimaryAddress(
            Diagnostics,
            &primaryAddress,
            &primaryParameter,
            &primaryConfidence)) {
        module = KswordARKBugcheckFindModuleForAddress(primaryAddress);
        if (module != NULL) {
            KswordARKBugcheckSetCandidate(
                Diagnostics,
                module,
                primaryAddress,
                primaryConfidence,
                primaryParameter,
                "bugcheck-specific address parameter");
            return;
        }
    }

    candidates[0] = Diagnostics->Parameter1;
    candidates[1] = Diagnostics->Parameter2;
    candidates[2] = Diagnostics->Parameter3;
    candidates[3] = Diagnostics->Parameter4;
    for (index = 0; index < RTL_NUMBER_OF(candidates); ++index) {
        module = KswordARKBugcheckFindModuleForAddress(candidates[index]);
        if (module != NULL) {
            KswordARKBugcheckSetCandidate(
                Diagnostics,
                module,
                candidates[index],
                index == 0
                    ? KSWORD_ARK_BUGCHECK_CONFIDENCE_MEDIUM
                    : KSWORD_ARK_BUGCHECK_CONFIDENCE_LOW,
                index + 1,
                "fallback scan of bugcheck parameters");
            return;
        }
    }
}

static VOID
KswordARKBugcheckCaptureData(
    _In_opt_ PKBUGCHECK_TRIAGE_DUMP_DATA TriageData,
    _In_ ULONG Reason,
    _In_ ULONG DumpType,
    _In_ ULONG64 DumpOffset,
    _In_ ULONG DumpBufferLength
    )
{
    KBUGCHECK_DATA bugData;
    PKSWORD_ARK_BUGCHECK_DIAGNOSTICS diagnostics =
        &g_KswordArkBugcheckState.Diagnostics;

    RtlZeroMemory(&bugData, sizeof(bugData));
    bugData.BugCheckDataSize = sizeof(bugData);

    if (TriageData != NULL) {
        diagnostics->BugCheckCode = TriageData->BugCheckCode;
        diagnostics->Parameter1 = TriageData->BugCheckParameter1;
        diagnostics->Parameter2 = TriageData->BugCheckParameter2;
        diagnostics->Parameter3 = TriageData->BugCheckParameter3;
        diagnostics->Parameter4 = TriageData->BugCheckParameter4;
    } else if (NT_SUCCESS(AuxKlibGetBugCheckData(&bugData)) &&
               bugData.BugCheckDataSize >= sizeof(bugData)) {
        diagnostics->BugCheckCode = bugData.BugCheckCode;
        diagnostics->Parameter1 = bugData.Parameter1;
        diagnostics->Parameter2 = bugData.Parameter2;
        diagnostics->Parameter3 = bugData.Parameter3;
        diagnostics->Parameter4 = bugData.Parameter4;
    }

    diagnostics->LastReason = Reason;
    diagnostics->LastDumpType = DumpType;
    diagnostics->DumpOffset = DumpOffset;
    diagnostics->DumpBufferLength = DumpBufferLength;
    diagnostics->Irql = (ULONG)KeGetCurrentIrql();
    diagnostics->Cpu = KeGetCurrentProcessorNumber();
    diagnostics->PerfCounter = KeQueryPerformanceCounter(NULL);
    KswordARKBugcheckResolveCandidate(diagnostics);
    InterlockedExchange(&diagnostics->Captured, 1);

    g_KswordArkBugcheckSecondaryData.Signature =
        KSWORD_ARK_BUGCHECK_SECONDARY_SIGNATURE;
    g_KswordArkBugcheckSecondaryData.Version = 1;
    g_KswordArkBugcheckSecondaryData.Size =
        sizeof(g_KswordArkBugcheckSecondaryData);
    RtlCopyMemory(
        &g_KswordArkBugcheckSecondaryData.Diagnostics,
        diagnostics,
        sizeof(*diagnostics));
}

static VOID
KswordARKBugcheckDrawLoop(
    VOID
    )
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER startCounter;
    LARGE_INTEGER currentCounter;
    ULONGLONG elapsedMilliseconds;
    ULONG fallbackPass;

    if (InterlockedCompareExchange(&g_KswordArkBugcheckState.Active, 1, 1) == 0) {
        return;
    }

    if (InterlockedCompareExchange(&g_KswordArkBugcheckState.ModeSetDone, 1, 0) == 0) {
        KswordARKBugcheckSvgaModeSetNoLog(&g_KswordArkBugcheckState.Svga);
        KeStallExecutionProcessor(2000);
    }

    frequency.QuadPart = 0;
    startCounter = KeQueryPerformanceCounter(&frequency);
    if (frequency.QuadPart <= 0) {
        for (fallbackPass = 0; fallbackPass < 10000UL; ++fallbackPass) {
            KswordARKBugcheckSvgaDrawPanelNoLog(&g_KswordArkBugcheckState);
            KeStallExecutionProcessor(1000);
        }
        return;
    }

    for (;;) {
        KswordARKBugcheckSvgaDrawPanelNoLog(&g_KswordArkBugcheckState);
        KeStallExecutionProcessor(1000);
        currentCounter = KeQueryPerformanceCounter(NULL);
        elapsedMilliseconds =
            ((ULONGLONG)(currentCounter.QuadPart - startCounter.QuadPart) * 1000ULL) /
            (ULONGLONG)frequency.QuadPart;
        if (elapsedMilliseconds >= KSWORD_ARK_BUGCHECK_DRAW_MILLISECONDS) {
            break;
        }
    }
}

static VOID
KswordARKBugcheckClassicCallback(
    _In_ PVOID Buffer,
    _In_ ULONG Length
    )
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    // Windows can paint over the classic callback. The late dump-I/O callback
    // owns the actual panel draw, so this callback intentionally performs no I/O.
    (VOID)InterlockedCompareExchange(
        &g_KswordArkBugcheckState.ClassicDisplayStarted,
        1,
        0);
}

static VOID
KswordARKBugcheckReasonCallback(
    _In_ KBUGCHECK_CALLBACK_REASON Reason,
    _In_ PKBUGCHECK_REASON_CALLBACK_RECORD Record,
    _Inout_ PVOID ReasonSpecificData,
    _In_ ULONG ReasonSpecificDataLength
    )
{
    PKBUGCHECK_SECONDARY_DUMP_DATA secondaryData;
    PKBUGCHECK_DUMP_IO dumpIoData;
    PKBUGCHECK_TRIAGE_DUMP_DATA triageData;
    ULONG dumpLength;

    UNREFERENCED_PARAMETER(Record);

    if (ReasonSpecificData == NULL ||
        InterlockedCompareExchange(&g_KswordArkBugcheckState.Active, 1, 1) == 0) {
        return;
    }

    if (Reason == KbCallbackSecondaryDumpData) {
        if (ReasonSpecificDataLength < sizeof(KBUGCHECK_SECONDARY_DUMP_DATA)) {
            return;
        }
        secondaryData = (PKBUGCHECK_SECONDARY_DUMP_DATA)ReasonSpecificData;
        dumpLength = sizeof(g_KswordArkBugcheckSecondaryData);
        if (dumpLength > secondaryData->MaximumAllowed) {
            dumpLength = secondaryData->MaximumAllowed;
        }
        secondaryData->Guid = g_KswordArkBugcheckSecondaryGuid;
        secondaryData->OutBuffer = &g_KswordArkBugcheckSecondaryData;
        secondaryData->OutBufferLength = dumpLength;
        return;
    }

    if (Reason == KbCallbackTriageDumpData) {
        if (ReasonSpecificDataLength < sizeof(KBUGCHECK_TRIAGE_DUMP_DATA)) {
            return;
        }
        triageData = (PKBUGCHECK_TRIAGE_DUMP_DATA)ReasonSpecificData;
        KswordARKBugcheckCaptureData(
            triageData,
            Reason,
            KbDumpIoInvalid,
            0,
            0);
        return;
    }

    if (Reason == KbCallbackDumpIo) {
        if (ReasonSpecificDataLength < sizeof(KBUGCHECK_DUMP_IO)) {
            return;
        }
        dumpIoData = (PKBUGCHECK_DUMP_IO)ReasonSpecificData;
        KswordARKBugcheckCaptureData(
            NULL,
            Reason,
            dumpIoData->Type,
            dumpIoData->Offset,
            dumpIoData->BufferLength);
        if ((dumpIoData->Type == KbDumpIoHeader ||
             dumpIoData->Type == KbDumpIoBody ||
             dumpIoData->Type == KbDumpIoSecondaryData) &&
            InterlockedCompareExchange(
                &g_KswordArkBugcheckState.DumpDisplayStarted,
                1,
                0) == 0) {
            KswordARKBugcheckDrawLoop();
        }
    }
}

NTSTATUS
KswordARKBugcheckInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ WDFDEVICE ControlDevice
    )
{
    NTSTATUS status;

    if (DriverObject == NULL || ControlDevice == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&g_KswordArkBugcheckState, sizeof(g_KswordArkBugcheckState));
    RtlZeroMemory(
        &g_KswordArkBugcheckSecondaryData,
        sizeof(g_KswordArkBugcheckSecondaryData));
    g_KswordArkBugcheckState.DriverObject = DriverObject;
    g_KswordArkBugcheckState.DeviceObject =
        WdfDeviceWdmGetDeviceObject(ControlDevice);
    g_KswordArkBugcheckState.Bitmap.BrandColorRgb = 0x0078D4UL;

    status = KswordARKBugcheckSvgaInitialize(&g_KswordArkBugcheckState.Svga);
    if (!NT_SUCCESS(status)) {
        KswordARKBugcheckSvgaShutdown(&g_KswordArkBugcheckState.Svga);
        return status;
    }

    KswordARKBugcheckRefreshModuleCache();
    InterlockedExchange(&g_KswordArkBugcheckState.Active, 1);

    KeInitializeCallbackRecord(&g_KswordArkBugcheckState.ClassicRecord);
    g_KswordArkBugcheckState.ClassicRegistered =
        KeRegisterBugCheckCallback(
            &g_KswordArkBugcheckState.ClassicRecord,
            KswordARKBugcheckClassicCallback,
            &g_KswordArkBugcheckSecondaryData,
            sizeof(g_KswordArkBugcheckSecondaryData),
            g_KswordArkBugcheckComponent);

    KeInitializeCallbackRecord(&g_KswordArkBugcheckState.SecondaryRecord);
    g_KswordArkBugcheckState.SecondaryRegistered =
        KeRegisterBugCheckReasonCallback(
            &g_KswordArkBugcheckState.SecondaryRecord,
            KswordARKBugcheckReasonCallback,
            KbCallbackSecondaryDumpData,
            g_KswordArkBugcheckComponent);

    KeInitializeCallbackRecord(&g_KswordArkBugcheckState.DumpIoRecord);
    g_KswordArkBugcheckState.DumpIoRegistered =
        KeRegisterBugCheckReasonCallback(
            &g_KswordArkBugcheckState.DumpIoRecord,
            KswordARKBugcheckReasonCallback,
            KbCallbackDumpIo,
            g_KswordArkBugcheckComponent);

    KeInitializeCallbackRecord(&g_KswordArkBugcheckState.TriageRecord);
    g_KswordArkBugcheckState.TriageRegistered =
        KeRegisterBugCheckReasonCallback(
            &g_KswordArkBugcheckState.TriageRecord,
            KswordARKBugcheckReasonCallback,
            KbCallbackTriageDumpData,
            g_KswordArkBugcheckComponent);

    if (!g_KswordArkBugcheckState.ClassicRegistered ||
        !g_KswordArkBugcheckState.SecondaryRegistered ||
        !g_KswordArkBugcheckState.DumpIoRegistered ||
        !g_KswordArkBugcheckState.TriageRegistered) {
        KswordARKBugcheckUninitialize();
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

VOID
KswordARKBugcheckUninitialize(
    VOID
    )
{
    InterlockedExchange(&g_KswordArkBugcheckState.Active, 0);
    InterlockedExchange(&g_KswordArkBugcheckState.Bitmap.Valid, 0);

    if (g_KswordArkBugcheckState.TriageRegistered) {
        (VOID)KeDeregisterBugCheckReasonCallback(
            &g_KswordArkBugcheckState.TriageRecord);
        g_KswordArkBugcheckState.TriageRegistered = FALSE;
    }
    if (g_KswordArkBugcheckState.DumpIoRegistered) {
        (VOID)KeDeregisterBugCheckReasonCallback(
            &g_KswordArkBugcheckState.DumpIoRecord);
        g_KswordArkBugcheckState.DumpIoRegistered = FALSE;
    }
    if (g_KswordArkBugcheckState.SecondaryRegistered) {
        (VOID)KeDeregisterBugCheckReasonCallback(
            &g_KswordArkBugcheckState.SecondaryRecord);
        g_KswordArkBugcheckState.SecondaryRegistered = FALSE;
    }
    if (g_KswordArkBugcheckState.ClassicRegistered) {
        (VOID)KeDeregisterBugCheckCallback(
            &g_KswordArkBugcheckState.ClassicRecord);
        g_KswordArkBugcheckState.ClassicRegistered = FALSE;
    }

    KswordARKBugcheckSvgaShutdown(&g_KswordArkBugcheckState.Svga);
}
