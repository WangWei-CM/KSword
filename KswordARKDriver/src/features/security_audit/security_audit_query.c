/*++
Module Name:
    security_audit_query.c
Abstract:
    Read-only Security/CI/VBS/SKCI/Hyper-V/AppControl posture queries.
Environment:
    Kernel-mode Driver Framework
--*/
#include "ark/ark_driver.h"
#include "security_audit_internal.h"
#include <ntstrsafe.h>
#if defined(_M_IX86) || defined(_M_X64)
#include <intrin.h>
#endif
#define KSWORD_ARK_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION 11UL
#define KSWORD_ARK_SECURITY_AUDIT_SYSTEM_CODEINTEGRITY_INFORMATION 103UL
#define KSWORD_ARK_SECURITY_AUDIT_SYSTEM_SECUREBOOT_INFORMATION 145UL
#define KSWORD_ARK_SECURITY_AUDIT_POOL_TAG 'aSsK'
typedef struct _KSWORD_SECURITY_AUDIT_SYSTEM_CODEINTEGRITY_INFORMATION
{
    ULONG Length;
    ULONG CodeIntegrityOptions;
} KSWORD_SECURITY_AUDIT_SYSTEM_CODEINTEGRITY_INFORMATION;
typedef struct _KSWORD_SECURITY_AUDIT_SYSTEM_SECUREBOOT_INFORMATION
{
    BOOLEAN SecureBootEnabled;
    BOOLEAN SecureBootCapable;
} KSWORD_SECURITY_AUDIT_SYSTEM_SECUREBOOT_INFORMATION;
typedef struct _KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY
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
} KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY, *PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY;
typedef struct _KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY Modules[1];
} KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION, *PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION;
typedef UCHAR KSWORD_SECURITY_AUDIT_SE_SIGNING_LEVEL;
typedef KSWORD_SECURITY_AUDIT_SE_SIGNING_LEVEL* PKSWORD_SECURITY_AUDIT_SE_SIGNING_LEVEL;
typedef NTSTATUS
(NTAPI* KSWORD_SECURITY_AUDIT_SE_GET_CACHED_SIGNING_LEVEL_FN)(
    _In_ PFILE_OBJECT FileObject,
    _Out_ PULONG Flags,
    _Out_ PKSWORD_SECURITY_AUDIT_SE_SIGNING_LEVEL SigningLevel,
    _Out_writes_bytes_to_opt_(*ThumbprintSize, *ThumbprintSize) PUCHAR Thumbprint,
    _Inout_opt_ PULONG ThumbprintSize,
    _Out_opt_ PULONG ThumbprintAlgorithm
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
static volatile LONG g_KswordSecurityAuditSigningResolverDone = 0;
static KSWORD_SECURITY_AUDIT_SE_GET_CACHED_SIGNING_LEVEL_FN g_KswordSecurityAuditSeGetCachedSigningLevel = NULL;
static PVOID
KswordSecurityAuditAllocate(
    _In_ SIZE_T Bytes
    )
/*++
Routine Description:
    Allocate nonpaged temporary memory for bounded security audit snapshots.
Arguments:
    Bytes - Requested byte count.
Return Value:
    Nonpaged pool pointer, or NULL on allocation failure.
--*/
{
    if (Bytes == 0U) {
        return NULL;
    }
#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, Bytes, KSWORD_ARK_SECURITY_AUDIT_POOL_TAG);
#pragma warning(pop)
}
static VOID
KswordSecurityAuditFree(
    _In_opt_ PVOID Buffer
    )
/*++
Routine Description:
    Free a buffer allocated by KswordSecurityAuditAllocate.
Arguments:
    Buffer - Optional pool pointer.
Return Value:
    None. The function only releases memory when Buffer is non-NULL.
--*/
{
    if (Buffer != NULL) {
        ExFreePoolWithTag(Buffer, KSWORD_ARK_SECURITY_AUDIT_POOL_TAG);
    }
}
static CHAR
KswordSecurityAuditAsciiLower(
    _In_ CHAR Character
    )
/*++
Routine Description:
    Convert one ASCII character to lowercase without locale dependencies.
Arguments:
    Character - Input character.
Return Value:
    Lowercase ASCII character, or the original value for non-uppercase bytes.
--*/
{
    if (Character >= 'A' && Character <= 'Z') {
        return (CHAR)(Character + ('a' - 'A'));
    }
    return Character;
}
static ULONG
KswordSecurityAuditBoundedAnsiLength(
    _In_reads_bytes_(MaximumBytes) const UCHAR* Text,
    _In_ ULONG MaximumBytes
    )
/*++
Routine Description:
    Measure a bounded ANSI string from SystemModuleInformation.
Arguments:
    Text - Bounded ANSI text buffer.
    MaximumBytes - Maximum readable byte count.
Return Value:
    Number of bytes before NUL or MaximumBytes.
--*/
{
    ULONG index = 0UL;
    if (Text == NULL || MaximumBytes == 0UL) {
        return 0UL;
    }
    for (index = 0UL; index < MaximumBytes; ++index) {
        if (Text[index] == 0U) {
            break;
        }
    }
    return index;
}
static BOOLEAN
KswordSecurityAuditBoundedAnsiEquals(
    _In_reads_bytes_(LeftBytes) const UCHAR* LeftText,
    _In_ ULONG LeftBytes,
    _In_z_ PCSTR RightText
    )
/*++
Routine Description:
    Compare a bounded module basename with a NUL-terminated ASCII name.
Arguments:
    LeftText - Bounded ANSI text from SystemModuleInformation.
    LeftBytes - Maximum readable bytes for LeftText.
    RightText - Expected module basename.
Return Value:
    TRUE when the bounded left string equals RightText case-insensitively.
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
        if (KswordSecurityAuditAsciiLower(leftCharacter) != KswordSecurityAuditAsciiLower(rightCharacter)) {
            return FALSE;
        }
    }
    return (RightText[index] == '\0') ? TRUE : FALSE;
}
static const UCHAR*
KswordSecurityAuditModuleFileName(
    _In_ const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Out_ ULONG* FileNameBytesOut
    )
/*++
Routine Description:
    Return the bounded filename portion of a SystemModuleInformation row.
Arguments:
    ModuleEntry - Module row supplied by the kernel.
    FileNameBytesOut - Receives the readable byte count for the returned pointer.
Return Value:
    Pointer into ModuleEntry->FullPathName; never points outside the fixed array.
--*/
{
    const UCHAR* fileName = NULL;
    ULONG fileNameBytes = 0UL;
    if (FileNameBytesOut != NULL) {
        *FileNameBytesOut = 0UL;
    }
    if (ModuleEntry == NULL || FileNameBytesOut == NULL) {
        return NULL;
    }
    fileName = ModuleEntry->FullPathName;
    fileNameBytes = sizeof(ModuleEntry->FullPathName);
    if (ModuleEntry->OffsetToFileName < sizeof(ModuleEntry->FullPathName)) {
        fileName = ModuleEntry->FullPathName + ModuleEntry->OffsetToFileName;
        fileNameBytes = (ULONG)(sizeof(ModuleEntry->FullPathName) - ModuleEntry->OffsetToFileName);
    }
    *FileNameBytesOut = fileNameBytes;
    return fileName;
}
static VOID
KswordSecurityAuditCopyAnsiNameToWide(
    _Out_writes_(DestinationChars) PWSTR Destination,
    _In_ USHORT DestinationChars,
    _In_reads_bytes_(SourceBytes) const UCHAR* Source,
    _In_ ULONG SourceBytes
    )
/*++
Routine Description:
    Copy a bounded ANSI module name to a fixed WCHAR protocol field.
Arguments:
    Destination - Fixed WCHAR output field.
    DestinationChars - Capacity of Destination in WCHARs.
    Source - Bounded ANSI source text.
    SourceBytes - Maximum readable bytes in Source.
Return Value:
    None. The destination is always NUL-terminated when capacity is nonzero.
--*/
{
    USHORT index = 0U;
    USHORT maxCopy = 0U;
    if (Destination == NULL || DestinationChars == 0U) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL || SourceBytes == 0UL) {
        return;
    }
    maxCopy = (USHORT)((SourceBytes < (ULONG)(DestinationChars - 1U)) ? SourceBytes : (ULONG)(DestinationChars - 1U));
    for (index = 0U; index < maxCopy; ++index) {
        const UCHAR sourceByte = Source[index];
        if (sourceByte == 0U) {
            break;
        }
        Destination[index] = (WCHAR)sourceByte;
    }
    Destination[index] = L'\0';
}
static ULONG
KswordSecurityAuditHashAnsiPath(
    _In_reads_bytes_(SourceBytes) const UCHAR* Source,
    _In_ ULONG SourceBytes
    )
/*++
Routine Description:
    Compute a bounded FNV-1a hash for a module path without returning full paths.
Arguments:
    Source - Bounded ANSI source path.
    SourceBytes - Maximum readable bytes in Source.
Return Value:
    32-bit hash value; zero when Source is absent.
--*/
{
    ULONG hashValue = 2166136261UL;
    ULONG index = 0UL;
    if (Source == NULL || SourceBytes == 0UL) {
        return 0UL;
    }
    for (index = 0UL; index < SourceBytes; ++index) {
        UCHAR sourceByte = Source[index];
        if (sourceByte == 0U) {
            break;
        }
        if (sourceByte >= 'A' && sourceByte <= 'Z') {
            sourceByte = (UCHAR)(sourceByte + ('a' - 'A'));
        }
        hashValue ^= (ULONG)sourceByte;
        hashValue *= 16777619UL;
    }
    return hashValue;
}
static NTSTATUS
KswordSecurityAuditQueryModuleSnapshot(
    _Outptr_result_bytebuffer_(*SnapshotBytesOut) PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION* SnapshotOut,
    _Out_ ULONG* SnapshotBytesOut
    )
/*++
Routine Description:
    Query SystemModuleInformation into a bounded nonpaged snapshot.
Arguments:
    SnapshotOut - Receives the allocated module snapshot.
    SnapshotBytesOut - Receives the allocated byte count.
Return Value:
    STATUS_SUCCESS with a snapshot, or the query/allocation failure status.
--*/
{
    PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION snapshot = NULL;
    ULONG requiredBytes = 0UL;
    ULONG queryBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    if (SnapshotOut == NULL || SnapshotBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *SnapshotOut = NULL;
    *SnapshotBytesOut = 0UL;
    status = ZwQuerySystemInformation(
        KSWORD_ARK_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION,
        NULL,
        0UL,
        &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }
    queryBytes = requiredBytes + (64UL * 1024UL);
    snapshot = (PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION)KswordSecurityAuditAllocate(queryBytes);
    if (snapshot == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = ZwQuerySystemInformation(
        KSWORD_ARK_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION,
        snapshot,
        queryBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        KswordSecurityAuditFree(snapshot);
        return status;
    }
    *SnapshotOut = snapshot;
    *SnapshotBytesOut = queryBytes;
    return STATUS_SUCCESS;
}
static const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY*
KswordSecurityAuditFindModuleByName(
    _In_opt_ const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION* Snapshot,
    _In_z_ PCSTR ModuleName
    )
/*++
Routine Description:
    Find a loaded module by basename in a SystemModuleInformation snapshot.
Arguments:
    Snapshot - Optional module snapshot.
    ModuleName - Expected module basename, for example ci.dll or vmbus.sys.
Return Value:
    Matching module row, or NULL when absent.
--*/
{
    ULONG index = 0UL;
    if (Snapshot == NULL || ModuleName == NULL) {
        return NULL;
    }
    for (index = 0UL; index < Snapshot->NumberOfModules; ++index) {
        const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY* entry = &Snapshot->Modules[index];
        ULONG fileNameBytes = 0UL;
        const UCHAR* fileName = KswordSecurityAuditModuleFileName(entry, &fileNameBytes);
        if (KswordSecurityAuditBoundedAnsiEquals(fileName, fileNameBytes, ModuleName)) {
            return entry;
        }
    }
    return NULL;
}
static ULONG
KswordSecurityAuditModuleState(
    _In_opt_ const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION* Snapshot,
    _In_z_ PCSTR ModuleName
    )
/*++
Routine Description:
    Convert module-list presence into a protocol state value.
Arguments:
    Snapshot - Optional module snapshot.
    ModuleName - Module basename to search.
    PRESENT when loaded, ABSENT when the snapshot exists but no row matches.
--*/
{
    if (Snapshot == NULL) {
        return KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
    }
    return (KswordSecurityAuditFindModuleByName(Snapshot, ModuleName) != NULL) ?
        KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT :
        KSWORD_ARK_SECURITY_AUDIT_STATE_ABSENT;
}
static KSWORD_SECURITY_AUDIT_SE_GET_CACHED_SIGNING_LEVEL_FN
KswordSecurityAuditResolveSigningLevelRoutine(
    VOID
    )
/*++
Routine Description:
    Resolve SeGetCachedSigningLevel dynamically for optional read-only trust rows.
Arguments:
    None.
    Function pointer when exported; NULL when unavailable.
--*/
{
    if (InterlockedCompareExchange(&g_KswordSecurityAuditSigningResolverDone, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"SeGetCachedSigningLevel");
        g_KswordSecurityAuditSeGetCachedSigningLevel =
            (KSWORD_SECURITY_AUDIT_SE_GET_CACHED_SIGNING_LEVEL_FN)MmGetSystemRoutineAddress(&routineName);
    }
    return g_KswordSecurityAuditSeGetCachedSigningLevel;
}
static NTSTATUS
KswordSecurityAuditOpenModulePath(
    _In_reads_bytes_(PathBytes) const UCHAR* PathText,
    _In_ ULONG PathBytes,
    _Out_ HANDLE* FileHandleOut
    )
/*++
Routine Description:
    Open a loaded-module path for metadata-only cached signing level lookup.
Arguments:
    PathText - Bounded ANSI module path from SystemModuleInformation.
    PathBytes - Maximum readable bytes in PathText.
    FileHandleOut - Receives a kernel file handle on success.
    ZwCreateFile status or conversion failure status.
--*/
{
    ANSI_STRING ansiPath;
    UNICODE_STRING unicodePath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    ULONG pathLength = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    if (FileHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileHandleOut = NULL;
    if (PathText == NULL || PathBytes == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    pathLength = KswordSecurityAuditBoundedAnsiLength(PathText, PathBytes);
    if (pathLength == 0UL || pathLength > 255UL) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    RtlZeroMemory(&ansiPath, sizeof(ansiPath));
    ansiPath.Buffer = (PCHAR)PathText;
    ansiPath.Length = (USHORT)pathLength;
    ansiPath.MaximumLength = (USHORT)pathLength;
    RtlZeroMemory(&unicodePath, sizeof(unicodePath));
    status = RtlAnsiStringToUnicodeString(&unicodePath, &ansiPath, TRUE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    InitializeObjectAttributes(
        &objectAttributes,
        &unicodePath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        FileHandleOut,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_NON_DIRECTORY_FILE,
        NULL,
        0U);
    RtlFreeUnicodeString(&unicodePath);
    return status;
}
static NTSTATUS
KswordSecurityAuditQueryModuleSigningLevel(
    _In_ const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Out_ ULONG* SigningLevelOut,
    _Out_ ULONG* SigningFlagsOut
    )
/*++
Routine Description:
    Query cached signing level for one loaded module by opening its image path read-only.
Arguments:
    ModuleEntry - Loaded module row.
    SigningLevelOut - Receives the signing level value.
    SigningFlagsOut - Receives signing flags returned by the kernel cache.
    STATUS_SUCCESS when signing level was obtained; otherwise a non-mutating failure status.
--*/
{
    KSWORD_SECURITY_AUDIT_SE_GET_CACHED_SIGNING_LEVEL_FN signingRoutine = NULL;
    KSWORD_SECURITY_AUDIT_SE_SIGNING_LEVEL signingLevel = KSWORD_ARK_SIGNING_LEVEL_UNCHECKED;
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    UCHAR thumbprint[KSWORD_ARK_TRUST_THUMBPRINT_MAX_BYTES] = { 0 };
    ULONG thumbprintSize = sizeof(thumbprint);
    ULONG thumbprintAlgorithm = 0UL;
    ULONG signingFlags = 0UL;
    NTSTATUS status = STATUS_SUCCESS;
    if (SigningLevelOut == NULL || SigningFlagsOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *SigningLevelOut = KSWORD_ARK_SIGNING_LEVEL_UNCHECKED;
    *SigningFlagsOut = 0UL;
    if (ModuleEntry == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    signingRoutine = KswordSecurityAuditResolveSigningLevelRoutine();
    if (signingRoutine == NULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    status = KswordSecurityAuditOpenModulePath(
        ModuleEntry->FullPathName,
        sizeof(ModuleEntry->FullPathName),
        &fileHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(
        fileHandle,
        0,
        NULL,
        KernelMode,
        (PVOID*)&fileObject,
        NULL);
    if (NT_SUCCESS(status)) {
        status = signingRoutine(
            fileObject,
            &signingFlags,
            &signingLevel,
            thumbprint,
            &thumbprintSize,
            &thumbprintAlgorithm);
        ObDereferenceObject(fileObject);
    }
    ZwClose(fileHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *SigningLevelOut = (ULONG)signingLevel;
    *SigningFlagsOut = signingFlags;
    return STATUS_SUCCESS;
}
static BOOLEAN
KswordSecurityAuditReadBooleanRoutineAddress(
    _In_z_ PCWSTR NameText,
    _Out_ ULONG* ValueOut
    )
/*++
Routine Description:
    Resolve and read an exported BOOLEAN variable when the kernel exposes it.
Arguments:
    NameText - Exported symbol name.
    ValueOut - Receives 0 or 1 when the symbol is readable.
    TRUE when a value was read; FALSE when the export is unavailable.
--*/
{
    UNICODE_STRING routineName;
    volatile BOOLEAN* valuePointer = NULL;
    if (NameText == NULL || ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0UL;
    RtlInitUnicodeString(&routineName, NameText);
    valuePointer = (volatile BOOLEAN*)MmGetSystemRoutineAddress(&routineName);
    if (valuePointer == NULL) {
        return FALSE;
    }
    *ValueOut = (*valuePointer != FALSE) ? 1UL : 0UL;
    return TRUE;
}
static VOID
KswordSecurityAuditFillCpuidHypervisor(
    _Inout_ KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE* Response
    )
/*++
Routine Description:
    Use CPUID on x86/x64 to detect hypervisor presence and vendor string.
Arguments:
    Response - Hyper-V summary response to update.
    None. Non-x86 architectures are left unavailable by design.
--*/
{
#if defined(_M_IX86) || defined(_M_X64)
    int cpuInfo[4] = { 0, 0, 0, 0 };
    CHAR vendorText[13] = { 0 };
    ULONG index = 0UL;
    if (Response == NULL) {
        return;
    }
    __cpuid(cpuInfo, 1);
    Response->hypervisorPresent = ((cpuInfo[2] & 0x80000000) != 0) ?
        KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT :
        KSWORD_ARK_SECURITY_AUDIT_STATE_ABSENT;
    Response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_CPUID;
    if (Response->hypervisorPresent == KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT) {
        __cpuid(cpuInfo, 0x40000000);
        RtlCopyMemory(&vendorText[0], &cpuInfo[1], sizeof(int));
        RtlCopyMemory(&vendorText[4], &cpuInfo[2], sizeof(int));
        RtlCopyMemory(&vendorText[8], &cpuInfo[3], sizeof(int));
        vendorText[12] = '\0';
        for (index = 0UL; index < (KSWORD_ARK_SECURITY_AUDIT_VENDOR_CHARS - 1U) && vendorText[index] != '\0'; ++index) {
            Response->hypervisorVendor[index] = (WCHAR)vendorText[index];
        }
        Response->hypervisorVendor[index] = L'\0';
    }
#else
    if (Response != NULL) {
        Response->hypervisorPresent = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
    }
#endif
}
NTSTATUS
KswordARKSecurityAuditQuerySecurityStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Query CI/VBS/Secure Kernel/SKCI/test-signing/debug posture through read-only sources.
Arguments:
    OutputBuffer - METHOD_BUFFERED output memory.
    OutputBufferLength - Output buffer byte count.
    BytesWrittenOut - Receives the fixed response size.
    STATUS_SUCCESS when the response was populated; buffer/IRQL errors otherwise.
--*/
{
    KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE* response = NULL;
    KSWORD_SECURITY_AUDIT_SYSTEM_CODEINTEGRITY_INFORMATION ciInformation;
    KSWORD_SECURITY_AUDIT_SYSTEM_SECUREBOOT_INFORMATION secureBootInformation;
    PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION moduleSnapshot = NULL;
    ULONG moduleSnapshotBytes = 0UL;
    ULONG kdEnabled = 0UL;
    ULONG kdNotPresent = 0UL;
    NTSTATUS moduleStatus = STATUS_SUCCESS;
    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_SECURITY_STATUS_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION;
    response->queryStatus = STATUS_SUCCESS;
    response->ciModuleLoaded = KSWORD_ARK_SECURITY_AUDIT_STATE_UNKNOWN;
    response->secureKernelModuleLoaded = KSWORD_ARK_SECURITY_AUDIT_STATE_UNKNOWN;
    response->skciModuleLoaded = KSWORD_ARK_SECURITY_AUDIT_STATE_UNKNOWN;
    response->debuggerStatus = STATUS_NOT_SUPPORTED;
    RtlZeroMemory(&ciInformation, sizeof(ciInformation));
    ciInformation.Length = sizeof(ciInformation);
    response->codeIntegrityStatus = ZwQuerySystemInformation(
        KSWORD_ARK_SECURITY_AUDIT_SYSTEM_CODEINTEGRITY_INFORMATION,
        &ciInformation,
        sizeof(ciInformation),
        NULL);
    if (NT_SUCCESS(response->codeIntegrityStatus)) {
        response->codeIntegrityOptions = ciInformation.CodeIntegrityOptions;
        response->ciEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_ENABLED) != 0UL) ? 1UL : 0UL;
        response->umciEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_UMCI_ENABLED) != 0UL) ? 1UL : 0UL;
        response->hvciKmciEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED) != 0UL) ? 1UL : 0UL;
        response->hvciAuditMode = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_HVCI_KMCI_AUDITMODE) != 0UL) ? 1UL : 0UL;
        response->hvciStrictMode = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_HVCI_KMCI_STRICTMODE) != 0UL) ? 1UL : 0UL;
        response->hvciIumEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_HVCI_IUM_ENABLED) != 0UL) ? 1UL : 0UL;
        response->testSigningEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_TESTSIGN) != 0UL) ? 1UL : 0UL;
        response->ciDebugModeEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_DEBUGMODE_ENABLED) != 0UL) ? 1UL : 0UL;
        response->testBuild = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_TEST_BUILD) != 0UL) ? 1UL : 0UL;
        response->flightBuild = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_FLIGHT_BUILD) != 0UL) ? 1UL : 0UL;
        response->flightingEnabled = ((ciInformation.CodeIntegrityOptions & KSWORD_ARK_CODEINTEGRITY_OPTION_FLIGHTING_ENABLED) != 0UL) ? 1UL : 0UL;
        response->fieldFlags |= KSWORD_ARK_SECURITY_STATUS_FIELD_CI_OPTIONS | KSWORD_ARK_SECURITY_STATUS_FIELD_CI_FLAGS | KSWORD_ARK_SECURITY_STATUS_FIELD_VBS_HVCI;
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_SYSTEM_QUERY;
    }
    RtlZeroMemory(&secureBootInformation, sizeof(secureBootInformation));
    response->secureBootStatus = ZwQuerySystemInformation(
        KSWORD_ARK_SECURITY_AUDIT_SYSTEM_SECUREBOOT_INFORMATION,
        &secureBootInformation,
        sizeof(secureBootInformation),
        NULL);
    if (NT_SUCCESS(response->secureBootStatus)) {
        response->secureBootEnabled = (secureBootInformation.SecureBootEnabled != FALSE) ? 1UL : 0UL;
        response->secureBootCapable = (secureBootInformation.SecureBootCapable != FALSE) ? 1UL : 0UL;
        response->fieldFlags |= KSWORD_ARK_SECURITY_STATUS_FIELD_SECURE_BOOT;
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_SYSTEM_QUERY;
    }
    if (KswordSecurityAuditReadBooleanRoutineAddress(L"KdDebuggerEnabled", &kdEnabled) &&
        KswordSecurityAuditReadBooleanRoutineAddress(L"KdDebuggerNotPresent", &kdNotPresent)) {
        response->kernelDebuggerEnabled = kdEnabled;
        response->kernelDebuggerNotPresent = kdNotPresent;
        response->debuggerStatus = STATUS_SUCCESS;
        response->fieldFlags |= KSWORD_ARK_SECURITY_STATUS_FIELD_KD_FLAGS;
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_EXPORT;
    }
    moduleStatus = KswordSecurityAuditQueryModuleSnapshot(&moduleSnapshot, &moduleSnapshotBytes);
    response->moduleQueryStatus = moduleStatus;
    if (NT_SUCCESS(moduleStatus)) {
        response->ciModuleLoaded = KswordSecurityAuditModuleState(moduleSnapshot, "ci.dll");
        if (response->ciModuleLoaded == KSWORD_ARK_SECURITY_AUDIT_STATE_ABSENT) {
            response->ciModuleLoaded = KswordSecurityAuditModuleState(moduleSnapshot, "ci.sys");
        }
        response->secureKernelModuleLoaded = KswordSecurityAuditModuleState(moduleSnapshot, "securekernel.exe");
        response->skciModuleLoaded = KswordSecurityAuditModuleState(moduleSnapshot, "skci.dll");
        if (response->skciModuleLoaded == KSWORD_ARK_SECURITY_AUDIT_STATE_ABSENT) {
            response->skciModuleLoaded = KswordSecurityAuditModuleState(moduleSnapshot, "skci.sys");
        }
        response->vbsPresent = (response->secureKernelModuleLoaded == KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT ||
            response->skciModuleLoaded == KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT ||
            response->hvciKmciEnabled != 0UL ||
            response->hvciIumEnabled != 0UL) ? 1UL : 0UL;
        response->fieldFlags |= KSWORD_ARK_SECURITY_STATUS_FIELD_CI_MODULE | KSWORD_ARK_SECURITY_STATUS_FIELD_SECUREKERNEL | KSWORD_ARK_SECURITY_STATUS_FIELD_SKCI;
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_MODULE_LIST;
    }
    KswordSecurityAuditFree(moduleSnapshot);
    UNREFERENCED_PARAMETER(moduleSnapshotBytes);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
NTSTATUS
KswordARKSecurityAuditQueryDriverTrustView(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Build a bounded loaded-driver trust cross-view from module list and signing cache.
Arguments:
    OutputBuffer - METHOD_BUFFERED output memory.
    OutputBufferLength - Output buffer byte count.
    Request - Optional caller request; NULL selects default row count and flags.
    BytesWrittenOut - Receives the actual response byte count.
    STATUS_SUCCESS with a possibly degraded response, or a buffer/IRQL error.
--*/
{
    KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE* response = NULL;
    PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION moduleSnapshot = NULL;
    ULONG moduleSnapshotBytes = 0UL;
    ULONG requestFlags = KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_DEFAULT;
    ULONG maxEntries = KSWORD_ARK_SECURITY_AUDIT_DEFAULT_DRIVER_ROWS;
    ULONG writableEntries = 0UL;
    ULONG index = 0UL;
    size_t minimumBytes = FIELD_OFFSET(KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE, entries);
    NTSTATUS status = STATUS_SUCCESS;
    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < minimumBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Request != NULL) {
        requestFlags = (Request->flags == 0UL) ? KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_DEFAULT : Request->flags;
        maxEntries = (Request->maxEntries == 0UL) ? KSWORD_ARK_SECURITY_AUDIT_DEFAULT_DRIVER_ROWS : Request->maxEntries;
    }
    if ((requestFlags & ~KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_INCLUDE_SIGNING_LEVEL) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (maxEntries > KSWORD_ARK_SECURITY_AUDIT_MAX_DRIVER_ROWS) {
        maxEntries = KSWORD_ARK_SECURITY_AUDIT_MAX_DRIVER_ROWS;
    }
    writableEntries = (ULONG)((OutputBufferLength - minimumBytes) / sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY));
    if (writableEntries > maxEntries) {
        writableEntries = maxEntries;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW_RESPONSE*)OutputBuffer;
    response->size = (ULONG)minimumBytes;
    response->version = KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION;
    response->queryStatus = STATUS_SUCCESS;
    response->maxEntriesAccepted = maxEntries;
    response->signingResolverStatus = (KswordSecurityAuditResolveSigningLevelRoutine() != NULL) ? STATUS_SUCCESS : STATUS_PROCEDURE_NOT_FOUND;
    status = KswordSecurityAuditQueryModuleSnapshot(&moduleSnapshot, &moduleSnapshotBytes);
    response->moduleQueryStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus = status;
        *BytesWrittenOut = minimumBytes;
        return STATUS_SUCCESS;
    }
    response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_MODULE_LIST;
    response->totalModuleCount = moduleSnapshot->NumberOfModules;
    response->truncated = (moduleSnapshot->NumberOfModules > writableEntries) ? 1UL : 0UL;
    for (index = 0UL; index < moduleSnapshot->NumberOfModules && response->entryCount < writableEntries; ++index) {
        const KSWORD_SECURITY_AUDIT_SYSTEM_MODULE_ENTRY* moduleEntry = &moduleSnapshot->Modules[index];
        KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY* outputEntry = &response->entries[response->entryCount];
        ULONG fileNameBytes = 0UL;
        const UCHAR* fileName = KswordSecurityAuditModuleFileName(moduleEntry, &fileNameBytes);
        outputEntry->size = sizeof(*outputEntry);
        outputEntry->sourceMask = KSWORD_ARK_SECURITY_AUDIT_SOURCE_MODULE_LIST;
        outputEntry->sourceCount = 1UL;
        outputEntry->imageBase = (ULONG64)(ULONG_PTR)moduleEntry->ImageBase;
        outputEntry->imageSize = moduleEntry->ImageSize;
        outputEntry->signingLevel = KSWORD_ARK_SIGNING_LEVEL_UNCHECKED;
        outputEntry->signingStatus = STATUS_NOT_SUPPORTED;
        outputEntry->fieldFlags |= KSWORD_ARK_DRIVER_TRUST_FLAG_SYSTEM_MODULE_PRESENT;
        if (moduleEntry->ImageBase == NULL || moduleEntry->ImageSize == 0UL) {
            outputEntry->conflictFlags |= KSWORD_ARK_DRIVER_TRUST_CONFLICT_EMPTY_IMAGE_RANGE;
        }
        if (KswordSecurityAuditBoundedAnsiLength(moduleEntry->FullPathName, sizeof(moduleEntry->FullPathName)) == 0UL) {
            outputEntry->conflictFlags |= KSWORD_ARK_DRIVER_TRUST_CONFLICT_PATH_UNAVAILABLE;
        }
        else {
            outputEntry->pathHash = KswordSecurityAuditHashAnsiPath(moduleEntry->FullPathName, sizeof(moduleEntry->FullPathName));
            outputEntry->fieldFlags |= KSWORD_ARK_DRIVER_TRUST_FLAG_PATH_HASH_PRESENT;
        }
        KswordSecurityAuditCopyAnsiNameToWide(
            outputEntry->moduleName,
            KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS,
            fileName,
            fileNameBytes);
        if (outputEntry->moduleName[0] != L'\0') {
            outputEntry->fieldFlags |= KSWORD_ARK_DRIVER_TRUST_FLAG_NAME_PRESENT;
        }
        if ((requestFlags & KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_INCLUDE_SIGNING_LEVEL) != 0UL) {
            outputEntry->signingStatus = KswordSecurityAuditQueryModuleSigningLevel(
                moduleEntry,
                &outputEntry->signingLevel,
                &outputEntry->signingLevelFlags);
            if (NT_SUCCESS(outputEntry->signingStatus)) {
                outputEntry->fieldFlags |= KSWORD_ARK_DRIVER_TRUST_FLAG_SIGNING_PRESENT;
                outputEntry->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_SIGNING_CACHE;
                outputEntry->sourceCount += 1UL;
                if (outputEntry->signingLevel == KSWORD_ARK_SIGNING_LEVEL_UNCHECKED ||
                    outputEntry->signingLevel == KSWORD_ARK_SIGNING_LEVEL_UNSIGNED) {
                    outputEntry->conflictFlags |= KSWORD_ARK_DRIVER_TRUST_CONFLICT_UNSIGNED_OR_UNKNOWN;
                }
            }
            else {
                outputEntry->conflictFlags |= KSWORD_ARK_DRIVER_TRUST_CONFLICT_SIGNING_UNAVAILABLE;
            }
        }
        response->entryCount += 1UL;
    }
    response->fieldFlags = KSWORD_ARK_DRIVER_TRUST_FLAG_SYSTEM_MODULE_PRESENT;
    if ((requestFlags & KSWORD_ARK_DRIVER_TRUST_QUERY_FLAG_INCLUDE_SIGNING_LEVEL) != 0UL) {
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_SIGNING_CACHE;
    }
    response->size = (ULONG)(minimumBytes + ((size_t)response->entryCount * sizeof(KSWORD_ARK_DRIVER_TRUST_VIEW_ENTRY)));
    *BytesWrittenOut = response->size;
    KswordSecurityAuditFree(moduleSnapshot);
    UNREFERENCED_PARAMETER(moduleSnapshotBytes);
    return STATUS_SUCCESS;
}
NTSTATUS
KswordARKSecurityAuditQueryHyperVSummary(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Query read-only Hyper-V availability and module-backed status skeleton.
Arguments:
    OutputBuffer - METHOD_BUFFERED output memory.
    OutputBufferLength - Output buffer byte count.
    BytesWrittenOut - Receives the fixed response size.
    STATUS_SUCCESS when the response was populated; buffer/IRQL errors otherwise.
--*/
{
    KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE* response = NULL;
    PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION moduleSnapshot = NULL;
    ULONG moduleSnapshotBytes = 0UL;
    NTSTATUS moduleStatus = STATUS_SUCCESS;
    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_HYPERV_SUMMARY_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION;
    response->queryStatus = STATUS_SUCCESS;
    response->hypervisorPresent = KSWORD_ARK_SECURITY_AUDIT_STATE_UNKNOWN;
    response->rootPartitionStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
    KswordSecurityAuditFillCpuidHypervisor(response);
    moduleStatus = KswordSecurityAuditQueryModuleSnapshot(&moduleSnapshot, &moduleSnapshotBytes);
    response->moduleQueryStatus = moduleStatus;
    if (NT_SUCCESS(moduleStatus)) {
        response->vmbusStatus = KswordSecurityAuditModuleState(moduleSnapshot, "vmbus.sys");
        response->vSwitchStatus = KswordSecurityAuditModuleState(moduleSnapshot, "vmswitch.sys");
        response->vPciStatus = KswordSecurityAuditModuleState(moduleSnapshot, "vpci.sys");
        response->hvSocketStatus = KswordSecurityAuditModuleState(moduleSnapshot, "hvsocket.sys");
        response->winHvStatus = KswordSecurityAuditModuleState(moduleSnapshot, "winhv.sys");
        response->winHvRuntimeStatus = KswordSecurityAuditModuleState(moduleSnapshot, "winhvr.sys");
        response->hvLoaderStatus = KswordSecurityAuditModuleState(moduleSnapshot, "hvloader.sys");
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_MODULE_LIST;
    }
    else {
        response->vmbusStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->vSwitchStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->vPciStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->hvSocketStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->winHvStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->winHvRuntimeStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->hvLoaderStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
    }
    response->fieldFlags = 0xFFFFFFFFUL;
    *BytesWrittenOut = sizeof(*response);
    KswordSecurityAuditFree(moduleSnapshot);
    UNREFERENCED_PARAMETER(moduleSnapshotBytes);
    return STATUS_SUCCESS;
}
NTSTATUS
KswordARKSecurityAuditQueryAppControlStatus(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++
Routine Description:
    Query AppID/AppLocker/mssecflt presence and callback-owner skeleton read-only.
Arguments:
    OutputBuffer - METHOD_BUFFERED output memory.
    OutputBufferLength - Output buffer byte count.
    BytesWrittenOut - Receives the fixed response size.
    STATUS_SUCCESS when the response was populated; buffer/IRQL errors otherwise.
--*/
{
    KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE* response = NULL;
    PKSWORD_SECURITY_AUDIT_SYSTEM_MODULE_INFORMATION moduleSnapshot = NULL;
    ULONG moduleSnapshotBytes = 0UL;
    NTSTATUS moduleStatus = STATUS_SUCCESS;
    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_QUERY_APP_CONTROL_STATUS_RESPONSE*)OutputBuffer;
    response->size = sizeof(*response);
    response->version = KSWORD_ARK_SECURITY_AUDIT_PROTOCOL_VERSION;
    response->queryStatus = STATUS_SUCCESS;
    response->appidPolicyStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
    moduleStatus = KswordSecurityAuditQueryModuleSnapshot(&moduleSnapshot, &moduleSnapshotBytes);
    response->moduleQueryStatus = moduleStatus;
    if (NT_SUCCESS(moduleStatus)) {
        response->appidStatus = KswordSecurityAuditModuleState(moduleSnapshot, "appid.sys");
        response->appLockerFilterStatus = KswordSecurityAuditModuleState(moduleSnapshot, "applockerfltr.sys");
        response->mssecfltStatus = KswordSecurityAuditModuleState(moduleSnapshot, "mssecflt.sys");
        response->ahcacheStatus = KswordSecurityAuditModuleState(moduleSnapshot, "ahcache.sys");
        response->bamStatus = KswordSecurityAuditModuleState(moduleSnapshot, "bam.sys");
        response->appLockerCallbackOwnerStatus = response->appLockerFilterStatus;
        response->mssecfltCallbackOwnerStatus = response->mssecfltStatus;
        if (response->appLockerFilterStatus == KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT) {
            KswordSecurityAuditCopyAnsiNameToWide(
                response->appLockerOwnerModule,
                KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS,
                (const UCHAR*)"applockerfltr.sys",
                sizeof("applockerfltr.sys"));
        }
        if (response->mssecfltStatus == KSWORD_ARK_SECURITY_AUDIT_STATE_PRESENT) {
            KswordSecurityAuditCopyAnsiNameToWide(
                response->mssecfltOwnerModule,
                KSWORD_ARK_SECURITY_AUDIT_NAME_CHARS,
                (const UCHAR*)"mssecflt.sys",
                sizeof("mssecflt.sys"));
        }
        response->sourceMask |= KSWORD_ARK_SECURITY_AUDIT_SOURCE_MODULE_LIST;
    }
    else {
        response->appidStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->appLockerFilterStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->appLockerCallbackOwnerStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->mssecfltStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->mssecfltCallbackOwnerStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->ahcacheStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
        response->bamStatus = KSWORD_ARK_SECURITY_AUDIT_STATE_UNAVAILABLE;
    }
    response->fieldFlags = 0xFFFFFFFFUL;
    *BytesWrittenOut = sizeof(*response);
    KswordSecurityAuditFree(moduleSnapshot);
    UNREFERENCED_PARAMETER(moduleSnapshotBytes);
    return STATUS_SUCCESS;
}
