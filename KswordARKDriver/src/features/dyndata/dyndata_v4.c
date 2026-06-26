/*++

Module Name:

    dyndata_v4.c

Abstract:

    DynData v4 multi-module PDB profile storage and validation.

Environment:

    Kernel-mode Driver Framework

--*/

#include "dyndata_v4_internal.h"
#include "../../platform/kernel_module_identity.h"

#include <ntstrsafe.h>

#define KSW_DYN_V4_STATE_POOL_TAG 'sDvK'

typedef PVOID
(NTAPI* KSW_DYN_V4_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

typedef struct _KSW_DYN_V4_MODULE_MATCH
{
    ULONG ClassId;
    const KSW_KERNEL_MODULE_NAME_MATCH* Names;
    ULONG NameCount;
} KSW_DYN_V4_MODULE_MATCH;

EX_PUSH_LOCK g_KswordDynDataV4Lock;
KSW_DYN_V4_STATE g_KswordDynDataV4State;

static PVOID
KswordARKDynDataV4AllocateStateBuffer(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    Allocate nonpaged temporary storage for v4 apply state. This avoids placing
    the large per-module item cache on the small kernel stack.

Arguments:

    BufferBytes - Number of bytes required by the caller.

Return Value:

    Nonpaged allocation on success; NULL on zero length or allocation failure.

--*/
{
    static volatile LONG allocatorResolved = 0;
    static KSW_DYN_V4_EX_ALLOCATE_POOL2_FN exAllocatePool2Fn = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&allocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        exAllocatePool2Fn = (KSW_DYN_V4_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (exAllocatePool2Fn != NULL) {
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, KSW_DYN_V4_STATE_POOL_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSW_DYN_V4_STATE_POOL_TAG);
#pragma warning(pop)
}

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4NtosNames[] = {
    { "ntoskrnl.exe", KSW_DYN_PROFILE_CLASS_NTOSKRNL },
    { "ntkrnlmp.exe", KSW_DYN_PROFILE_CLASS_NTOSKRNL }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4Ntkrla57Names[] = {
    { "ntkrla57.exe", KSW_DYN_PROFILE_CLASS_NTKRLA57 }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4LxcoreNames[] = {
    { "lxcore.sys", KSW_DYN_PROFILE_CLASS_LXCORE }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4Win32kNames[] = {
    { "win32k.sys", KSW_DYN_PROFILE_CLASS_WIN32K }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4Win32kbaseNames[] = {
    { "win32kbase.sys", KSW_DYN_PROFILE_CLASS_WIN32KBASE }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4Win32kfullNames[] = {
    { "win32kfull.sys", KSW_DYN_PROFILE_CLASS_WIN32KFULL }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4TcpipNames[] = {
    { "tcpip.sys", KSW_DYN_PROFILE_CLASS_TCPIP }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4NdisNames[] = {
    { "ndis.sys", KSW_DYN_PROFILE_CLASS_NDIS }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4NetioNames[] = {
    { "netio.sys", KSW_DYN_PROFILE_CLASS_NETIO }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4FltMgrNames[] = {
    { "fltMgr.sys", KSW_DYN_PROFILE_CLASS_FLTMGR }
};

static const KSW_KERNEL_MODULE_NAME_MATCH g_KswordDynV4FvevolNames[] = {
    { "fvevol.sys", KSW_DYN_PROFILE_CLASS_FVEVOL }
};

static const KSW_DYN_V4_MODULE_MATCH g_KswordDynV4ModuleMatches[] = {
    { KSW_DYN_PROFILE_CLASS_NTOSKRNL, g_KswordDynV4NtosNames, RTL_NUMBER_OF(g_KswordDynV4NtosNames) },
    { KSW_DYN_PROFILE_CLASS_NTKRLA57, g_KswordDynV4Ntkrla57Names, RTL_NUMBER_OF(g_KswordDynV4Ntkrla57Names) },
    { KSW_DYN_PROFILE_CLASS_LXCORE, g_KswordDynV4LxcoreNames, RTL_NUMBER_OF(g_KswordDynV4LxcoreNames) },
    { KSW_DYN_PROFILE_CLASS_WIN32K, g_KswordDynV4Win32kNames, RTL_NUMBER_OF(g_KswordDynV4Win32kNames) },
    { KSW_DYN_PROFILE_CLASS_WIN32KBASE, g_KswordDynV4Win32kbaseNames, RTL_NUMBER_OF(g_KswordDynV4Win32kbaseNames) },
    { KSW_DYN_PROFILE_CLASS_WIN32KFULL, g_KswordDynV4Win32kfullNames, RTL_NUMBER_OF(g_KswordDynV4Win32kfullNames) },
    { KSW_DYN_PROFILE_CLASS_TCPIP, g_KswordDynV4TcpipNames, RTL_NUMBER_OF(g_KswordDynV4TcpipNames) },
    { KSW_DYN_PROFILE_CLASS_NDIS, g_KswordDynV4NdisNames, RTL_NUMBER_OF(g_KswordDynV4NdisNames) },
    { KSW_DYN_PROFILE_CLASS_NETIO, g_KswordDynV4NetioNames, RTL_NUMBER_OF(g_KswordDynV4NetioNames) },
    { KSW_DYN_PROFILE_CLASS_FLTMGR, g_KswordDynV4FltMgrNames, RTL_NUMBER_OF(g_KswordDynV4FltMgrNames) },
    { KSW_DYN_PROFILE_CLASS_FVEVOL, g_KswordDynV4FvevolNames, RTL_NUMBER_OF(g_KswordDynV4FvevolNames) }
};

static VOID
KswordARKDynDataV4SetMessage(
    _Out_writes_(KSW_DYN_REASON_CHARS) WCHAR* Destination,
    _In_z_ PCWSTR Message
    )
/*++

Routine Description:

    Store a bounded v4 response message for user-mode diagnostics.

Arguments:

    Destination - Fixed WCHAR message buffer in a response packet.
    Message - NUL-terminated diagnostic message.

Return Value:

    None.

--*/
{
    if (Destination == NULL) {
        return;
    }

    Destination[0] = L'\0';
    if (Message == NULL) {
        return;
    }

    (VOID)RtlStringCchCopyW(Destination, KSW_DYN_REASON_CHARS, Message);
    Destination[KSW_DYN_REASON_CHARS - 1U] = L'\0';
}

static CHAR
KswordARKDynDataV4LowerAnsi(
    _In_ CHAR Character
    )
/*++

Routine Description:

    Convert one ASCII character to lowercase for bounded module-name matching.

Arguments:

    Character - Input character.

Return Value:

    Lowercase ASCII character when applicable; otherwise the original byte.

--*/
{
    if (Character >= 'A' && Character <= 'Z') {
        return (CHAR)(Character + ('a' - 'A'));
    }

    return Character;
}

static BOOLEAN
KswordARKDynDataV4WideEqualsInsensitive(
    _In_reads_(LeftChars) const WCHAR* LeftText,
    _In_ ULONG LeftChars,
    _In_reads_(RightChars) const WCHAR* RightText,
    _In_ ULONG RightChars
    )
/*++

Routine Description:

    Compare two bounded shared WCHAR names without trusting trailing bytes.

Arguments:

    LeftText - First WCHAR buffer.
    LeftChars - Maximum readable WCHARs in LeftText.
    RightText - Second WCHAR buffer.
    RightChars - Maximum readable WCHARs in RightText.

Return Value:

    TRUE when both strings terminate at the same point and match ignoring ASCII
    case; FALSE otherwise.

--*/
{
    ULONG index = 0UL;
    ULONG limit = 0UL;

    if (LeftText == NULL || RightText == NULL || LeftChars == 0UL || RightChars == 0UL) {
        return FALSE;
    }

    limit = (LeftChars < RightChars) ? LeftChars : RightChars;
    for (index = 0UL; index < limit; ++index) {
        WCHAR leftCharacter = LeftText[index];
        WCHAR rightCharacter = RightText[index];

        if (leftCharacter == L'\0' || rightCharacter == L'\0') {
            return (leftCharacter == rightCharacter) ? TRUE : FALSE;
        }
        if (leftCharacter > 0x7f || rightCharacter > 0x7f) {
            return FALSE;
        }
        if (KswordARKDynDataV4LowerAnsi((CHAR)leftCharacter) != KswordARKDynDataV4LowerAnsi((CHAR)rightCharacter)) {
            return FALSE;
        }
    }

    return FALSE;
}

static BOOLEAN
KswordARKDynDataV4WideMatchesAnsi(
    _In_reads_(WideChars) const WCHAR* WideText,
    _In_ ULONG WideChars,
    _In_z_ PCSTR AnsiText
    )
/*++

Routine Description:

    Compare a fixed shared WCHAR module name with an ANSI basename constant.

Arguments:

    WideText - Shared WCHAR buffer to compare.
    WideChars - Maximum readable WCHARs in WideText.
    AnsiText - NUL-terminated ASCII module basename.

Return Value:

    TRUE when the names are equal ignoring ASCII case.

--*/
{
    ULONG index = 0UL;

    if (WideText == NULL || WideChars == 0UL || AnsiText == NULL) {
        return FALSE;
    }

    for (index = 0UL; index < WideChars; ++index) {
        const WCHAR wideCharacter = WideText[index];
        const CHAR ansiCharacter = AnsiText[index];

        if (ansiCharacter == '\0') {
            return (wideCharacter == L'\0') ? TRUE : FALSE;
        }
        if (wideCharacter == L'\0' || wideCharacter > 0x7f) {
            return FALSE;
        }
        if (KswordARKDynDataV4LowerAnsi((CHAR)wideCharacter) != KswordARKDynDataV4LowerAnsi(ansiCharacter)) {
            return FALSE;
        }
    }

    return (AnsiText[index] == '\0') ? TRUE : FALSE;
}

static const KSW_DYN_V4_MODULE_MATCH*
KswordARKDynDataV4FindModuleMatch(
    _In_ ULONG ClassId
    )
/*++

Routine Description:

    Resolve a stable v4 module class id into the accepted loaded-module names.

Arguments:

    ClassId - KSW_DYN_PROFILE_CLASS_* value from the v4 request.

Return Value:

    Pointer to a static match row when supported; NULL otherwise.

--*/
{
    ULONG index = 0UL;

    for (index = 0UL; index < RTL_NUMBER_OF(g_KswordDynV4ModuleMatches); ++index) {
        if (g_KswordDynV4ModuleMatches[index].ClassId == ClassId) {
            return &g_KswordDynV4ModuleMatches[index];
        }
    }

    return NULL;
}

static LONG
KswordARKDynDataV4FindModuleSlot(
    _In_ ULONG ClassId
    )
/*++

Routine Description:

    Resolve a module class id into a stable compact v4 state slot.

Arguments:

    ClassId - KSW_DYN_PROFILE_CLASS_* value.

Return Value:

    Zero-based slot index when supported; -1 when unsupported.

--*/
{
    ULONG index = 0UL;

    for (index = 0UL; index < RTL_NUMBER_OF(g_KswordDynV4ModuleMatches); ++index) {
        if (g_KswordDynV4ModuleMatches[index].ClassId == ClassId) {
            return (LONG)index;
        }
    }

    return -1L;
}

static BOOLEAN
KswordARKDynDataV4ModuleNameAllowed(
    _In_ const KSW_DYN_V4_MODULE_MATCH* Match,
    _In_reads_(KSW_DYN_MODULE_NAME_CHARS) const WCHAR* ModuleName
    )
/*++

Routine Description:

    Check that the request names one of the basenames owned by its class id.

Arguments:

    Match - Static class-to-name mapping.
    ModuleName - Request module basename.

Return Value:

    TRUE when the basename belongs to the class mapping.

--*/
{
    ULONG index = 0UL;

    if (Match == NULL || ModuleName == NULL) {
        return FALSE;
    }

    for (index = 0UL; index < Match->NameCount; ++index) {
        if (KswordARKDynDataV4WideMatchesAnsi(ModuleName, KSW_DYN_MODULE_NAME_CHARS, Match->Names[index].FileName)) {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
KswordARKDynDataV4ImageIdentityMatches(
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* CurrentIdentity,
    _In_ const KSW_DYN_MODULE_IDENTITY_PACKET* RequestedIdentity
    )
/*++

Routine Description:

    Compare the loaded-image tuple that makes a v4 module profile safe to store.

Arguments:

    CurrentIdentity - Identity read from the loaded kernel module list.
    RequestedIdentity - Identity supplied by the v4 PDB profile request.

Return Value:

    TRUE when class, machine, timestamp, size, and basename all match.

--*/
{
    if (CurrentIdentity == NULL || RequestedIdentity == NULL) {
        return FALSE;
    }
    if (CurrentIdentity->present == 0UL || RequestedIdentity->present == 0UL) {
        return FALSE;
    }
    if (CurrentIdentity->classId != RequestedIdentity->classId ||
        CurrentIdentity->machine != RequestedIdentity->machine ||
        CurrentIdentity->timeDateStamp != RequestedIdentity->timeDateStamp ||
        CurrentIdentity->sizeOfImage != RequestedIdentity->sizeOfImage) {
        return FALSE;
    }

    return KswordARKDynDataV4WideEqualsInsensitive(
        CurrentIdentity->moduleName,
        KSW_DYN_MODULE_NAME_CHARS,
        RequestedIdentity->moduleName,
        KSW_DYN_MODULE_NAME_CHARS);
}

static BOOLEAN
KswordARKDynDataV4ItemKindSupported(
    _In_ ULONG ItemKind
    )
/*++

Routine Description:

    Check whether one v4 item kind is understood by this R0 storage layer.

Arguments:

    ItemKind - KSW_DYN_V4_ITEM_KIND_* value.

Return Value:

    TRUE for supported v4 item kinds; FALSE for unknown kinds.

--*/
{
    return ItemKind == KSW_DYN_V4_ITEM_KIND_STRUCT_OFFSET ||
        ItemKind == KSW_DYN_V4_ITEM_KIND_GLOBAL_RVA ||
        ItemKind == KSW_DYN_V4_ITEM_KIND_FUNCTION_RVA ||
        ItemKind == KSW_DYN_V4_ITEM_KIND_ENUM_VALUE ||
        ItemKind == KSW_DYN_V4_ITEM_KIND_TYPE_SIZE ||
        ItemKind == KSW_DYN_V4_ITEM_KIND_BIT_FIELD ||
        ItemKind == KSW_DYN_V4_ITEM_KIND_LIST_HEAD_GLOBAL;
}

static BOOLEAN
KswordARKDynDataV4ItemValueValid(
    _In_ const KSW_DYN_V4_ITEM_PACKET* Item,
    _In_ ULONG SizeOfImage
    )
/*++

Routine Description:

    Apply kind-specific range checks that do not require business consumers.

Arguments:

    Item - One compact v4 item.
    SizeOfImage - Loaded image size for RVA range checks.

Return Value:

    TRUE when the item is self-consistent and safe to store.

--*/
{
    if (Item == NULL) {
        return FALSE;
    }

    switch (Item->itemKind) {
    case KSW_DYN_V4_ITEM_KIND_STRUCT_OFFSET:
        return (Item->valueLow != KSW_DYN_OFFSET_UNAVAILABLE);
    case KSW_DYN_V4_ITEM_KIND_GLOBAL_RVA:
    case KSW_DYN_V4_ITEM_KIND_FUNCTION_RVA:
    case KSW_DYN_V4_ITEM_KIND_LIST_HEAD_GLOBAL:
        return (Item->valueLow != 0UL && Item->valueLow < SizeOfImage);
    case KSW_DYN_V4_ITEM_KIND_ENUM_VALUE:
        return (Item->aux0 == 1UL || Item->aux0 == 2UL || Item->aux0 == 4UL || Item->aux0 == 8UL);
    case KSW_DYN_V4_ITEM_KIND_TYPE_SIZE:
        return (Item->valueLow != 0UL);
    case KSW_DYN_V4_ITEM_KIND_BIT_FIELD:
        if (!(Item->aux2 == 1UL || Item->aux2 == 2UL || Item->aux2 == 4UL || Item->aux2 == 8UL)) {
            return FALSE;
        }
        if (Item->aux1 == 0UL || Item->aux1 > 64UL || Item->aux0 >= 64UL) {
            return FALSE;
        }
        return (Item->aux0 + Item->aux1) <= (Item->aux2 * 8UL);
    default:
        return FALSE;
    }
}

static LONG
KswordARKDynDataV4FindGroupIndex(
    _In_reads_(GroupCount) const KSW_DYN_V4_CAPABILITY_GROUP_PACKET* Groups,
    _In_ ULONG GroupCount,
    _In_ ULONG GroupId
    )
/*++

Routine Description:

    Locate one capability group id in a bounded request group array.

Arguments:

    Groups - Fixed request group array.
    GroupCount - Number of active rows in Groups.
    GroupId - Capability group id to locate.

Return Value:

    Zero-based group index when found; -1 when missing.

--*/
{
    ULONG index = 0UL;

    if (Groups == NULL) {
        return -1L;
    }

    for (index = 0UL; index < GroupCount; ++index) {
        if (Groups[index].groupId == GroupId) {
            return (LONG)index;
        }
    }

    return -1L;
}

static BOOLEAN
KswordARKDynDataV4ItemDuplicate(
    _In_reads_(ItemCount) const KSW_DYN_V4_ITEM_PACKET* Items,
    _In_ ULONG ItemCount,
    _In_ ULONG CurrentIndex
    )
/*++

Routine Description:

    Detect duplicate item ids inside one module profile request.

Arguments:

    Items - Variable request item array.
    ItemCount - Number of item rows.
    CurrentIndex - Index whose itemId is being checked.

Return Value:

    TRUE when an earlier item uses the same nonzero item id.

--*/
{
    ULONG index = 0UL;
    ULONG itemId = 0UL;

    if (Items == NULL || CurrentIndex >= ItemCount) {
        return TRUE;
    }

    itemId = Items[CurrentIndex].itemId;
    for (index = 0UL; index < CurrentIndex; ++index) {
        if (Items[index].itemId == itemId) {
            return TRUE;
        }
    }

    return FALSE;
}

static VOID
KswordARKDynDataV4AppendMissing(
    _Inout_ KSW_DYN_V4_STATE* State,
    _In_ ULONG ModuleClassId,
    _In_ ULONG GroupId,
    _In_ ULONG MissingKind,
    _In_ ULONG MissingCount,
    _In_z_ PCSTR Reason
    )
/*++

Routine Description:

    Append a bounded missing-summary row for R3 diagnostics.

Arguments:

    State - Mutable v4 state.
    ModuleClassId - Module class that owns the missing summary.
    GroupId - Capability group whose count is short.
    MissingKind - Required or optional missing category.
    MissingCount - Number of absent items summarized by this row.
    Reason - ASCII reason text.

Return Value:

    None.

--*/
{
    KSW_DYN_V4_MISSING_ITEM_ENTRY* entry = NULL;

    if (State == NULL || MissingCount == 0UL || State->MissingCount >= KSW_DYN_V4_MAX_MISSING_SUMMARY) {
        return;
    }

    entry = &State->Missing[State->MissingCount];
    RtlZeroMemory(entry, sizeof(*entry));
    entry->moduleClassId = ModuleClassId;
    entry->itemId = MissingCount;
    entry->capabilityGroupId = GroupId;
    entry->missingKind = MissingKind;
    (VOID)RtlStringCchCopyA(entry->itemName, KSW_DYN_V4_ITEM_NAME_CHARS, "summary-count");
    (VOID)RtlStringCchCopyA(entry->reason, KSW_DYN_V4_MISSING_REASON_CHARS, Reason);
    State->MissingCount += 1UL;
}

VOID
KswordARKDynDataV4Initialize(
    VOID
    )
/*++

Routine Description:

    Initialize the independent v4 profile state lock and clear cached modules.

Arguments:

    None.

Return Value:

    None.

--*/
{
    ExInitializePushLock(&g_KswordDynDataV4Lock);
    ExAcquirePushLockExclusive(&g_KswordDynDataV4Lock);
    RtlZeroMemory(&g_KswordDynDataV4State, sizeof(g_KswordDynDataV4State));
    ExReleasePushLockExclusive(&g_KswordDynDataV4Lock);
}

VOID
KswordARKDynDataV4Uninitialize(
    VOID
    )
/*++

Routine Description:

    Clear cached v4 module profile state during DynData shutdown.

Arguments:

    None.

Return Value:

    None.

--*/
{
    ExAcquirePushLockExclusive(&g_KswordDynDataV4Lock);
    RtlZeroMemory(&g_KswordDynDataV4State, sizeof(g_KswordDynDataV4State));
    ExReleasePushLockExclusive(&g_KswordDynDataV4Lock);
}

NTSTATUS
KswordARKDynDataV4ApplyProfile(
    _In_reads_bytes_(InputBufferLength) const KSW_APPLY_DYN_PROFILE_V4_REQUEST* Request,
    _In_ size_t InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) KSW_APPLY_DYN_PROFILE_V4_RESPONSE* Response,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Validate and store one v4 module profile without wiring items to consumers.

Arguments:

    Request - METHOD_BUFFERED v4 module profile request.
    InputBufferLength - Total input bytes supplied by WDF.
    Response - Fixed v4 apply response.
    OutputBufferLength - Writable response byte count.
    BytesWrittenOut - Receives response bytes written.

Return Value:

    STATUS_SUCCESS on accepted storage; validation status when rejected.

--*/
{
    const KSW_DYN_V4_MODULE_MATCH* moduleMatch = NULL;
    KSW_DYN_MODULE_IDENTITY_PACKET currentIdentity;
    KSW_DYN_V4_MODULE_STATE* moduleState = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    size_t requiredBytes = 0U;
    LONG moduleSlot = -1L;
    ULONG groupPresentRequired[KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE] = { 0 };
    ULONG groupPresentOptional[KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE] = { 0 };
    ULONG index = 0UL;
    ULONG rejectedCount = 0UL;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (Response == NULL || OutputBufferLength < sizeof(KSW_APPLY_DYN_PROFILE_V4_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Response, OutputBufferLength);
    Response->size = sizeof(*Response);
    Response->version = KSW_DYN_V4_PROTOCOL_VERSION;
    Response->status = STATUS_UNSUCCESSFUL;
    KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 apply did not run.");
    *BytesWrittenOut = sizeof(*Response);

    if (Request == NULL || InputBufferLength < KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE) {
        status = STATUS_BUFFER_TOO_SMALL;
        Response->status = status;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 request header is too small.");
        return status;
    }
    if (Request->version != KSW_DYN_V4_PROTOCOL_VERSION) {
        status = STATUS_REVISION_MISMATCH;
        Response->status = status;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 protocol version mismatch.");
        return status;
    }
    if (Request->itemCount == 0UL || Request->itemCount > KSW_DYN_V4_MAX_ITEMS_PER_MODULE ||
        Request->capabilityGroupCount == 0UL ||
        Request->capabilityGroupCount > KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE) {
        status = STATUS_INVALID_PARAMETER;
        Response->status = status;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 request counts are invalid.");
        return status;
    }
    if ((Request->itemCount - 1UL) >
        ((MAXSIZE_T - KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE) / sizeof(KSW_DYN_V4_ITEM_PACKET))) {
        status = STATUS_INTEGER_OVERFLOW;
        Response->status = status;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 request size overflow.");
        return status;
    }

    requiredBytes = KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE +
        ((size_t)Request->itemCount * sizeof(KSW_DYN_V4_ITEM_PACKET));
    if ((size_t)Request->size < requiredBytes || InputBufferLength < requiredBytes) {
        status = STATUS_BUFFER_TOO_SMALL;
        Response->status = status;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 request does not contain all items.");
        return status;
    }

    moduleMatch = KswordARKDynDataV4FindModuleMatch(Request->module.image.classId);
    if (moduleMatch == NULL ||
        !KswordARKDynDataV4ModuleNameAllowed(moduleMatch, Request->module.image.moduleName)) {
        status = STATUS_NOT_SUPPORTED;
        Response->status = status;
        Response->statusFlags = KSW_DYN_V4_STATUS_FLAG_IDENTITY_REJECTED;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 module class or name is unsupported.");
        return status;
    }
    moduleSlot = KswordARKDynDataV4FindModuleSlot(Request->module.image.classId);
    if (moduleSlot < 0L) {
        status = STATUS_NOT_SUPPORTED;
        Response->status = status;
        Response->statusFlags = KSW_DYN_V4_STATUS_FLAG_IDENTITY_REJECTED;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 module class has no storage slot.");
        return status;
    }

    status = KswordARKQueryKernelModuleIdentity(moduleMatch->Names, moduleMatch->NameCount, &currentIdentity);
    if (!NT_SUCCESS(status)) {
        Response->status = status;
        Response->statusFlags = KSW_DYN_V4_STATUS_FLAG_IDENTITY_REJECTED;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 target module is absent or unreadable.");
        return status;
    }
    if (!KswordARKDynDataV4ImageIdentityMatches(&currentIdentity, &Request->module.image)) {
        status = STATUS_NOT_SUPPORTED;
        Response->status = status;
        Response->statusFlags = KSW_DYN_V4_STATUS_FLAG_IDENTITY_REJECTED;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 module identity mismatch.");
        return status;
    }

    moduleState = (KSW_DYN_V4_MODULE_STATE*)KswordARKDynDataV4AllocateStateBuffer(sizeof(*moduleState));
    if (moduleState == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        Response->status = status;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 could not allocate module state storage.");
        return status;
    }

    RtlZeroMemory(moduleState, sizeof(*moduleState));
    moduleState->Occupied = TRUE;
    moduleState->PublicEntry.moduleIndex = (ULONG)moduleSlot;
    moduleState->PublicEntry.module = Request->module;
    moduleState->PublicEntry.module.image.imageBase = currentIdentity.imageBase;
    moduleState->PublicEntry.itemCount = Request->itemCount;
    moduleState->PublicEntry.capabilityGroupCount = Request->capabilityGroupCount;
    moduleState->PublicEntry.statusFlags = KSW_DYN_V4_STATUS_FLAG_IDENTITY_MATCHED;

    for (index = 0UL; index < Request->capabilityGroupCount; ++index) {
        moduleState->Groups[index].PublicEntry.moduleClassId = Request->module.image.classId;
        moduleState->Groups[index].PublicEntry.groupId = Request->capabilityGroups[index].groupId;
        moduleState->Groups[index].PublicEntry.requiredItemCount = Request->capabilityGroups[index].requiredItemCount;
        moduleState->Groups[index].PublicEntry.optionalItemCount = Request->capabilityGroups[index].optionalItemCount;
        RtlCopyMemory(
            moduleState->Groups[index].PublicEntry.groupName,
            Request->capabilityGroups[index].groupName,
            sizeof(moduleState->Groups[index].PublicEntry.groupName));
        moduleState->Groups[index].PublicEntry.groupName[KSW_DYN_V4_CAPABILITY_NAME_CHARS - 1U] = '\0';
        Response->requiredItemCount += Request->capabilityGroups[index].requiredItemCount;
        Response->optionalItemCount += Request->capabilityGroups[index].optionalItemCount;
    }

    for (index = 0UL; index < Request->itemCount; ++index) {
        const KSW_DYN_V4_ITEM_PACKET* item = &Request->items[index];
        LONG groupIndex = -1L;
        ULONG groupSlot = 0UL;

        if (item->itemId == 0UL ||
            !KswordARKDynDataV4ItemKindSupported(item->itemKind) ||
            !KswordARKDynDataV4ItemValueValid(item, currentIdentity.sizeOfImage) ||
            KswordARKDynDataV4ItemDuplicate(Request->items, Request->itemCount, index)) {
            rejectedCount += 1UL;
            continue;
        }

        groupIndex = KswordARKDynDataV4FindGroupIndex(
            Request->capabilityGroups,
            Request->capabilityGroupCount,
            item->capabilityGroupId);
        if (groupIndex < 0L) {
            rejectedCount += 1UL;
            continue;
        }

        groupSlot = (ULONG)groupIndex;
        Response->appliedItemCount += 1UL;
        moduleState->Items[moduleState->StoredItemCount] = *item;
        moduleState->StoredItemCount += 1UL;
        if ((item->flags & KSW_DYN_V4_ITEM_FLAG_REQUIRED) != 0UL) {
            groupPresentRequired[groupSlot] += 1UL;
            Response->presentRequiredItemCount += 1UL;
        }
        else {
            groupPresentOptional[groupSlot] += 1UL;
            Response->presentOptionalItemCount += 1UL;
        }
    }

    Response->rejectedItemCount = rejectedCount;
    if (rejectedCount != 0UL || Response->appliedItemCount == 0UL) {
        status = STATUS_INVALID_PARAMETER;
        Response->status = status;
        Response->statusFlags = KSW_DYN_V4_STATUS_FLAG_VALIDATION_FAILED;
        KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 profile contained invalid items; active v4 state was left unchanged.");
        ExFreePoolWithTag(moduleState, KSW_DYN_V4_STATE_POOL_TAG);
        return status;
    }

    for (index = 0UL; index < Request->capabilityGroupCount; ++index) {
        KSW_DYN_V4_CAPABILITY_GROUP_STATUS_ENTRY* group = &moduleState->Groups[index].PublicEntry;
        group->presentRequiredItemCount = groupPresentRequired[index];
        group->presentOptionalItemCount = groupPresentOptional[index];
        if (group->presentRequiredItemCount >= group->requiredItemCount) {
            group->statusFlags |= KSW_DYN_V4_STATUS_FLAG_REQUIRED_COMPLETE;
            group->statusFlags |= KSW_DYN_V4_STATUS_FLAG_PROFILE_APPLIED;
            moduleState->PublicEntry.activeCapabilityGroupCount += 1UL;
        }
        else {
            moduleState->PublicEntry.missingRequiredItemCount += group->requiredItemCount - group->presentRequiredItemCount;
        }
        if (group->presentOptionalItemCount < group->optionalItemCount) {
            group->statusFlags |= KSW_DYN_V4_STATUS_FLAG_OPTIONAL_DEGRADED;
            moduleState->PublicEntry.missingOptionalItemCount += group->optionalItemCount - group->presentOptionalItemCount;
        }
    }

    if (moduleState->PublicEntry.missingRequiredItemCount == 0UL) {
        moduleState->PublicEntry.statusFlags |= KSW_DYN_V4_STATUS_FLAG_REQUIRED_COMPLETE;
        moduleState->PublicEntry.statusFlags |= KSW_DYN_V4_STATUS_FLAG_PROFILE_APPLIED;
    }
    else {
        moduleState->PublicEntry.statusFlags |= KSW_DYN_V4_STATUS_FLAG_VALIDATION_FAILED;
    }
    if (moduleState->PublicEntry.missingOptionalItemCount != 0UL) {
        moduleState->PublicEntry.statusFlags |= KSW_DYN_V4_STATUS_FLAG_OPTIONAL_DEGRADED;
    }

    ExAcquirePushLockExclusive(&g_KswordDynDataV4Lock);
    RtlCopyMemory(&g_KswordDynDataV4State.Modules[(ULONG)moduleSlot], moduleState, sizeof(*moduleState));
    g_KswordDynDataV4State.MissingCount = 0UL;
    for (index = 0UL; index < KSW_DYN_V4_MAX_MODULES; ++index) {
        const KSW_DYN_V4_MODULE_STATE* storedModule = &g_KswordDynDataV4State.Modules[index];

        if (!storedModule->Occupied) {
            continue;
        }
        if (storedModule->PublicEntry.missingRequiredItemCount != 0UL) {
            KswordARKDynDataV4AppendMissing(
                &g_KswordDynDataV4State,
                storedModule->PublicEntry.module.image.classId,
                0UL,
                KSW_DYN_V4_MISSING_KIND_REQUIRED,
                storedModule->PublicEntry.missingRequiredItemCount,
                "required items absent from applied profile");
        }
        if (storedModule->PublicEntry.missingOptionalItemCount != 0UL) {
            KswordARKDynDataV4AppendMissing(
                &g_KswordDynDataV4State,
                storedModule->PublicEntry.module.image.classId,
                0UL,
                KSW_DYN_V4_MISSING_KIND_OPTIONAL,
                storedModule->PublicEntry.missingOptionalItemCount,
                "optional items absent from applied profile");
        }
    }
    ExReleasePushLockExclusive(&g_KswordDynDataV4Lock);

    Response->status = STATUS_SUCCESS;
    Response->statusFlags = moduleState->PublicEntry.statusFlags;
    Response->activeCapabilityGroupCount = moduleState->PublicEntry.activeCapabilityGroupCount;
    Response->missingRequiredItemCount = moduleState->PublicEntry.missingRequiredItemCount;
    Response->missingOptionalItemCount = moduleState->PublicEntry.missingOptionalItemCount;
    Response->module = moduleState->PublicEntry.module;
    ExFreePoolWithTag(moduleState, KSW_DYN_V4_STATE_POOL_TAG);
    KswordARKDynDataV4SetMessage(Response->message, L"DynData v4 module profile accepted for safe storage.");
    return STATUS_SUCCESS;
}

