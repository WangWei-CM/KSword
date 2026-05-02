#pragma once

#include "ark/ark_driver.h"

#include <ntimage.h>
#include <ntstrsafe.h>

#define KSW_HOOK_SCAN_TAG 'hHsK'

typedef struct _KSW_HOOK_SYSTEM_MODULE_ENTRY
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
} KSW_HOOK_SYSTEM_MODULE_ENTRY, *PKSW_HOOK_SYSTEM_MODULE_ENTRY;

typedef struct _KSW_HOOK_SYSTEM_MODULE_INFORMATION
{
    ULONG NumberOfModules;
    KSW_HOOK_SYSTEM_MODULE_ENTRY Modules[1];
} KSW_HOOK_SYSTEM_MODULE_INFORMATION, *PKSW_HOOK_SYSTEM_MODULE_INFORMATION;

EXTERN_C_START

CHAR
KswordARKHookAsciiLower(
    _In_ CHAR Character
    );

BOOLEAN
KswordARKHookBoundedAnsiEqualsInsensitive(
    _In_reads_bytes_(LeftBytes) const UCHAR* LeftText,
    _In_ ULONG LeftBytes,
    _In_z_ PCSTR RightText
    );

BOOLEAN
KswordARKHookWideModuleFilterMatches(
    _In_reads_bytes_(FileNameBytes) const UCHAR* FileNameText,
    _In_ ULONG FileNameBytes,
    _In_reads_(FilterChars) const WCHAR* FilterText,
    _In_ ULONG FilterChars
    );

VOID
KswordARKHookCopyAnsi(
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes,
    _In_opt_z_ const CHAR* Source
    );

VOID
KswordARKHookCopyBoundedAnsiToWide(
    _In_reads_bytes_(SourceBytes) const UCHAR* SourceText,
    _In_ ULONG SourceBytes,
    _Out_writes_(DestinationChars) PWCHAR DestinationText,
    _In_ ULONG DestinationChars
    );

BOOLEAN
KswordARKHookValidateRvaRange(
    _In_ ULONG Rva,
    _In_ ULONG Bytes,
    _In_ ULONG ImageSize
    );

NTSTATUS
KswordARKHookBuildModuleSnapshot(
    _Outptr_result_bytebuffer_(*BufferBytesOut) KSW_HOOK_SYSTEM_MODULE_INFORMATION** ModuleInfoOut,
    _Out_ ULONG* BufferBytesOut
    );

const KSW_HOOK_SYSTEM_MODULE_ENTRY*
KswordARKHookFindModuleForAddress(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONG_PTR Address
    );

VOID
KswordARKHookGetModuleFileName(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Outptr_result_buffer_(*FileNameBytesOut) const UCHAR** FileNameOut,
    _Out_ ULONG* FileNameBytesOut
    );

BOOLEAN
KswordARKHookReadMemorySafe(
    _In_ const VOID* Source,
    _Out_writes_bytes_(BytesToRead) VOID* Destination,
    _In_ SIZE_T BytesToRead
    );

BOOLEAN
KswordARKHookMultiplyUlong(
    _In_ ULONG LeftValue,
    _In_ ULONG RightValue,
    _Out_ ULONG* ProductOut
    );

BOOLEAN
KswordARKHookAddRvaOffset(
    _In_ ULONG BaseRva,
    _In_ ULONG Index,
    _In_ ULONG ElementBytes,
    _Out_ ULONG* RvaOut
    );

BOOLEAN
KswordARKHookImageAddressFromRva(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_ ULONG_PTR* AddressOut
    );

BOOLEAN
KswordARKHookReadImageBytes(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_writes_bytes_(BytesToRead) VOID* Destination,
    _In_ SIZE_T BytesToRead
    );

BOOLEAN
KswordARKHookReadImageNtHeaders(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Out_ IMAGE_NT_HEADERS* NtHeadersOut
    );

BOOLEAN
KswordARKHookGetDataDirectory(
    _In_ const IMAGE_NT_HEADERS* NtHeaders,
    _In_ ULONG DirectoryIndex,
    _Out_ IMAGE_DATA_DIRECTORY* DirectoryOut
    );

BOOLEAN
KswordARKHookReadImageUlong(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_ ULONG* ValueOut
    );

BOOLEAN
KswordARKHookReadImageUshort(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_ USHORT* ValueOut
    );

BOOLEAN
KswordARKHookCopyImageAnsi(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes
    );

BOOLEAN
KswordARKHookIsRvaInsideDirectory(
    _In_ ULONG Rva,
    _In_ const IMAGE_DATA_DIRECTORY* Directory
    );

EXTERN_C_END
