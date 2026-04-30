/*++

Module Name:

    kernel_module_identity.c

Abstract:

    Loaded-kernel-module identity resolver used by DynData exact matching.

Environment:

    Kernel-mode Driver Framework

--*/

#include "kernel_module_identity.h"

#include <ntimage.h>
#include <ntstrsafe.h>

#define KSW_MODULE_IDENTITY_TAG 'iDsK'
#define KSW_SYSTEM_MODULE_INFORMATION_CLASS 11UL

typedef PVOID
(NTAPI* KSW_EX_ALLOCATE_POOL2_ROUTINE)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

typedef struct _KSW_SYSTEM_MODULE_ENTRY
{
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} KSW_SYSTEM_MODULE_ENTRY, *PKSW_SYSTEM_MODULE_ENTRY;

typedef struct _KSW_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSW_SYSTEM_MODULE_ENTRY Modules[1];
} KSW_SYSTEM_MODULE_INFORMATION, *PKSW_SYSTEM_MODULE_INFORMATION;

static CHAR
KswordARKAsciiLower(
    _In_ CHAR Character
    )
/*++

Routine Description:

    Convert one ASCII character to lowercase for bounded module-name matching.

Arguments:

    Character - Input ANSI character.

Return Value:

    Lowercase ASCII character when applicable; otherwise the original byte.

--*/
{
    if (Character >= 'A' && Character <= 'Z') {
        return (CHAR)(Character + ('a' - 'A'));
    }

    return Character;
}

static PVOID
KswordARKAllocateModuleIdentityBuffer(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    Allocate the transient SystemModuleInformation buffer. Newer kernels expose
    ExAllocatePool2, while older targets need the deprecated fallback.

Arguments:

    BufferBytes - Number of nonpaged bytes to allocate.

Return Value:

    Nonpaged allocation pointer, or NULL on failure.

--*/
{
    UNICODE_STRING routineName;
    KSW_EX_ALLOCATE_POOL2_ROUTINE allocatePool2 = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
    allocatePool2 = (KSW_EX_ALLOCATE_POOL2_ROUTINE)MmGetSystemRoutineAddress(&routineName);
    if (allocatePool2 != NULL) {
        return allocatePool2(POOL_FLAG_NON_PAGED, BufferBytes, KSW_MODULE_IDENTITY_TAG);
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, KSW_MODULE_IDENTITY_TAG);
#pragma warning(pop)
}

static BOOLEAN
KswordARKBoundedAnsiEqualsInsensitive(
    _In_reads_bytes_(LeftBytes) const UCHAR* LeftText,
    _In_ ULONG LeftBytes,
    _In_z_ PCSTR RightText
    )
/*++

Routine Description:

    Compare a bounded ANSI filename from SystemModuleInformation with a constant
    NUL-terminated filename without assuming the bounded string is terminated.

Arguments:

    LeftText - Bounded ANSI filename buffer.
    LeftBytes - Maximum readable bytes in LeftText.
    RightText - Constant filename to match.

Return Value:

    TRUE when both strings match case-insensitively; otherwise FALSE.

--*/
{
    ULONG index = 0UL;

    if (LeftText == NULL || LeftBytes == 0UL || RightText == NULL) {
        return FALSE;
    }

    for (index = 0UL; index < LeftBytes; ++index) {
        const CHAR leftCharacter = (CHAR)LeftText[index];
        const CHAR rightCharacter = RightText[index];

        if (rightCharacter == '\0') {
            return (leftCharacter == '\0') ? TRUE : FALSE;
        }
        if (leftCharacter == '\0') {
            return FALSE;
        }
        if (KswordARKAsciiLower(leftCharacter) != KswordARKAsciiLower(rightCharacter)) {
            return FALSE;
        }
    }

    return (RightText[index] == '\0') ? TRUE : FALSE;
}

static VOID
KswordARKCopyBoundedAnsiToWide(
    _In_reads_bytes_(SourceBytes) const UCHAR* SourceText,
    _In_ ULONG SourceBytes,
    _Out_writes_(DestinationChars) PWCHAR DestinationText,
    _In_ ULONG DestinationChars
    )
/*++

Routine Description:

    Copy a bounded ANSI module name into the shared wide-character identity
    packet while guaranteeing termination.

Arguments:

    SourceText - Bounded ANSI source string.
    SourceBytes - Maximum readable source bytes.
    DestinationText - Wide-character output buffer.
    DestinationChars - Output buffer capacity in WCHARs.

Return Value:

    None.

--*/
{
    ULONG index = 0UL;

    if (DestinationText == NULL || DestinationChars == 0UL) {
        return;
    }

    DestinationText[0] = L'\0';
    if (SourceText == NULL || SourceBytes == 0UL) {
        return;
    }

    for (index = 0UL; index + 1UL < DestinationChars && index < SourceBytes; ++index) {
        if (SourceText[index] == '\0') {
            break;
        }
        DestinationText[index] = (WCHAR)SourceText[index];
    }
    DestinationText[index] = L'\0';
}

static NTSTATUS
KswordARKReadLoadedImagePeIdentity(
    _In_ PVOID ImageBase,
    _Out_ ULONG* MachineOut,
    _Out_ ULONG* TimeDateStampOut,
    _Out_ ULONG* SizeOfImageOut
    )
/*++

Routine Description:

    Read PE header identity values from a loaded kernel image. The caller already
    received the base address from the trusted kernel module list; guarded reads
    still prevent a malformed image from breaking driver initialization.

Arguments:

    ImageBase - Loaded image base address.
    MachineOut - Receives IMAGE_FILE_HEADER.Machine.
    TimeDateStampOut - Receives IMAGE_FILE_HEADER.TimeDateStamp.
    SizeOfImageOut - Receives IMAGE_OPTIONAL_HEADER.SizeOfImage.

Return Value:

    STATUS_SUCCESS on a valid PE header, or an image/parameter failure status.

--*/
{
    PIMAGE_DOS_HEADER dosHeader = NULL;
    PIMAGE_NT_HEADERS ntHeaders = NULL;
    ULONG peOffset = 0UL;

    if (ImageBase == NULL || MachineOut == NULL || TimeDateStampOut == NULL || SizeOfImageOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *MachineOut = 0UL;
    *TimeDateStampOut = 0UL;
    *SizeOfImageOut = 0UL;

    __try {
        dosHeader = (PIMAGE_DOS_HEADER)ImageBase;
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        peOffset = (ULONG)dosHeader->e_lfanew;
        if (peOffset > 0x100000UL) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)ImageBase + peOffset);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        *MachineOut = (ULONG)ntHeaders->FileHeader.Machine;
        *TimeDateStampOut = ntHeaders->FileHeader.TimeDateStamp;
        *SizeOfImageOut = ntHeaders->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKFillModuleIdentityFromEntry(
    _In_ const KSW_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG ClassId,
    _Out_ KSW_DYN_MODULE_IDENTITY_PACKET* IdentityOut
    )
/*++

Routine Description:

    Convert one SystemModuleInformation row into the shared DynData identity
    packet used by exact profile matching and UI diagnostics.

Arguments:

    ModuleEntry - Loaded module row from ZwQuerySystemInformation.
    ClassId - KSW_DYN_PROFILE_CLASS_* value selected by filename matching.
    IdentityOut - Output identity packet.

Return Value:

    STATUS_SUCCESS when PE identity was parsed; otherwise an image failure.

--*/
{
    const UCHAR* fileNameText = NULL;
    ULONG fileNameBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG imageSizeFromPe = 0UL;

    if (ModuleEntry == NULL || IdentityOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(IdentityOut, sizeof(*IdentityOut));
    if (ModuleEntry->OffsetToFileName < sizeof(ModuleEntry->FullPathName)) {
        fileNameText = ModuleEntry->FullPathName + ModuleEntry->OffsetToFileName;
        fileNameBytes = (ULONG)(sizeof(ModuleEntry->FullPathName) - ModuleEntry->OffsetToFileName);
    }
    else {
        fileNameText = ModuleEntry->FullPathName;
        fileNameBytes = (ULONG)sizeof(ModuleEntry->FullPathName);
    }

    status = KswordARKReadLoadedImagePeIdentity(
        ModuleEntry->ImageBase,
        &IdentityOut->machine,
        &IdentityOut->timeDateStamp,
        &imageSizeFromPe);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    IdentityOut->present = 1UL;
    IdentityOut->classId = ClassId;
    IdentityOut->sizeOfImage = (imageSizeFromPe != 0UL) ? imageSizeFromPe : ModuleEntry->ImageSize;
    IdentityOut->imageBase = (ULONGLONG)(ULONG_PTR)ModuleEntry->ImageBase;
    KswordARKCopyBoundedAnsiToWide(
        fileNameText,
        fileNameBytes,
        IdentityOut->moduleName,
        KSW_DYN_MODULE_NAME_CHARS);

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKQueryKernelModuleIdentity(
    _In_reads_(NameMatchCount) const KSW_KERNEL_MODULE_NAME_MATCH* NameMatches,
    _In_ ULONG NameMatchCount,
    _Out_ KSW_DYN_MODULE_IDENTITY_PACKET* IdentityOut
    )
/*++

Routine Description:

    Query the loaded kernel module list and return the first module whose file
    name matches one of the supplied DynData module names.

Arguments:

    NameMatches - Accepted filenames plus their DynData class ids.
    NameMatchCount - Number of rows in NameMatches.
    IdentityOut - Receives module identity for exact DynData matching.

Return Value:

    STATUS_SUCCESS on match, STATUS_NOT_FOUND when absent, or a query/allocation
    status when the module list cannot be read.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    KSW_SYSTEM_MODULE_INFORMATION* moduleInformation = NULL;
    ULONG moduleIndex = 0UL;

    if (NameMatches == NULL || NameMatchCount == 0UL || IdentityOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(IdentityOut, sizeof(*IdentityOut));
    status = ZwQuerySystemInformation(
        KSW_SYSTEM_MODULE_INFORMATION_CLASS,
        NULL,
        0UL,
        &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

    moduleInformation = (KSW_SYSTEM_MODULE_INFORMATION*)KswordARKAllocateModuleIdentityBuffer(requiredBytes);
    if (moduleInformation == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySystemInformation(
        KSW_SYSTEM_MODULE_INFORMATION_CLASS,
        moduleInformation,
        requiredBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(moduleInformation, KSW_MODULE_IDENTITY_TAG);
        return status;
    }

    for (moduleIndex = 0UL; moduleIndex < moduleInformation->NumberOfModules; ++moduleIndex) {
        const KSW_SYSTEM_MODULE_ENTRY* moduleEntry = &moduleInformation->Modules[moduleIndex];
        const UCHAR* fileNameText = moduleEntry->FullPathName;
        ULONG fileNameBytes = (ULONG)sizeof(moduleEntry->FullPathName);
        ULONG matchIndex = 0UL;

        if (moduleEntry->OffsetToFileName < sizeof(moduleEntry->FullPathName)) {
            fileNameText = moduleEntry->FullPathName + moduleEntry->OffsetToFileName;
            fileNameBytes = (ULONG)(sizeof(moduleEntry->FullPathName) - moduleEntry->OffsetToFileName);
        }

        for (matchIndex = 0UL; matchIndex < NameMatchCount; ++matchIndex) {
            if (!KswordARKBoundedAnsiEqualsInsensitive(fileNameText, fileNameBytes, NameMatches[matchIndex].FileName)) {
                continue;
            }

            status = KswordARKFillModuleIdentityFromEntry(
                moduleEntry,
                NameMatches[matchIndex].ClassId,
                IdentityOut);
            ExFreePoolWithTag(moduleInformation, KSW_MODULE_IDENTITY_TAG);
            return status;
        }
    }

    ExFreePoolWithTag(moduleInformation, KSW_MODULE_IDENTITY_TAG);
    return STATUS_NOT_FOUND;
}
