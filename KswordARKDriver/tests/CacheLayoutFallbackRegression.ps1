[CmdletBinding()]
param(
    [string]$KernelImage = 'C:\Windows\System32\ntoskrnl.exe',
    [string]$CiImage = 'C:\Windows\System32\ci.dll',
    [uint32]$PiDdbTableRva = 0xD2F000,
    [uint32]$PiDdbLockRva = 0xC44C40,
    [uint32]$CiListRva = 0xC3090,
    [uint32]$CiLockRva = 0x39DC0,
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)
# RVA arguments are fixture/oracle inputs from independently inspected exact PEs.
# The CI scanner never receives those oracles: they seed synthetic live memory
# and assertions only. PiDDB's exact-global path intentionally takes trusted RVAs.

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$taskImage = (Resolve-Path -LiteralPath $KernelImage).Path
$taskOutput = Join-Path $taskRoot '.codex-build-logs\cache-layout-fallback-regression'
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) { $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat' }
$taskKit = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
foreach ($taskRequired in @($taskVcvars, $taskKit)) {
    if (!(Test-Path -LiteralPath $taskRequired)) { throw "Required input missing: $taskRequired" }
}
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskMapped = Join-Path $taskOutput 'kernel-mapped.bin'
# The expected RVA is an independent test oracle. No PDB or profile is opened by the replay.
python -c 'import pefile,sys; p=pefile.PE(sys.argv[1]); b=p.get_memory_mapped_image(); open(sys.argv[2],"wb").write(b+bytes(p.OPTIONAL_HEADER.SizeOfImage-len(b)))' $taskImage $taskMapped
if ($LASTEXITCODE -ne 0) { throw 'Cannot map the supplied PE image.' }
$taskScanner = Join-Path $taskOutput 'runtime_signature_scan_replay.c'
$taskProduction = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\runtime_signature_scan.c'))
$taskStart = $taskProduction.IndexOf('typedef struct _KSW_RUNTIME_ROUTINE_WORK')
if ($taskStart -lt 0) { throw 'Cannot locate production scanner definitions.' }
# Preserve all production function bodies, removing only the imported export resolver declaration.
[IO.File]::WriteAllText($taskScanner, $taskProduction.Substring($taskStart), [Text.UTF8Encoding]::new($false))
$taskResolver = Join-Path $taskOutput 'kernel_cache_unloaded_replay.c'
$taskProduction = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\kernel_cache_fallback.c'))
[IO.File]::WriteAllText($taskResolver, $taskProduction, [Text.UTF8Encoding]::new($false))
$taskCiMapped = Join-Path $taskOutput 'ci-mapped.bin'
python -c 'import pefile,sys; p=pefile.PE(sys.argv[1]); b=p.get_memory_mapped_image(); open(sys.argv[2],"wb").write(b+bytes(p.OPTIONAL_HEADER.SizeOfImage-len(b)))' $CiImage $taskCiMapped
if ($LASTEXITCODE -ne 0) { throw 'Cannot map the supplied CI PE image.' }
$taskCiProduction = (Join-Path $taskRoot 'KswordARKDriver\src\features\kernel\ci_hash_fallback.c').Replace('\','/')
$taskSource = Join-Path $taskOutput 'runtime_unloaded_scan_regression.c'
$taskExecutable = Join-Path $taskOutput 'runtime_unloaded_scan_regression.exe'
$taskCode = @'
#include <ntifs.h>
#include <ntimage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma warning(push)
#pragma warning(disable: 4324) // WDK/WDF framework header alignment.
#include "ark/ark_startup.h"
#include "kernel_cache_fallback.h"
#include "kernel_object_probe.h"
#include "pool_compat.h"
#include "runtime_signature_scan.h"
#include "ci_hash_fallback.h"
#include "hook_scan_support.h"
#pragma warning(pop)

#define RECORD_BASE 0xFFFF900000200000ULL
#define NAME_BASE 0xFFFF900000300000ULL
static UCHAR* image;
static ULONG imageSize;
static ULONG_PTR imageBase = 0xFFFF800000100000ULL;
static UCHAR fixtureRecords[6400];
static WCHAR driverName[] = L"fixture.sys";
static PVOID testSystemRangeStart = (PVOID)(ULONG_PTR)0xFFFF800000000000ULL;
static ULONG failures;
static ULONG lockCalls;
static BOOLEAN resourceAllowed = TRUE;
static BOOLEAN lockAvailable = TRUE;
static KIRQL testIrql = PASSIVE_LEVEL;
static ULONG resourceRva;

static BOOLEAN TestRead(const VOID* Address, VOID* Buffer, SIZE_T Size)
{
    ULONG_PTR address = (ULONG_PTR)Address;
    ULONG_PTR bases[3] = { imageBase, RECORD_BASE, NAME_BASE };
    const UCHAR* buffers[3] = { image, fixtureRecords, (const UCHAR*)driverName };
    SIZE_T sizes[3] = { imageSize, sizeof(fixtureRecords), sizeof(driverName) };
    ULONG index;
    for (index = 0UL; index < 3UL; ++index) {
        if (address >= bases[index] && address - bases[index] < sizes[index] &&
            Size <= sizes[index] - (SIZE_T)(address - bases[index])) {
            memcpy(Buffer, buffers[index] + address - bases[index], Size);
            return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS TestCopy(VOID* Buffer, MM_COPY_ADDRESS Source, SIZE_T Size, ULONG Flags, SIZE_T* Copied)
{
    UNREFERENCED_PARAMETER(Flags);
    *Copied = 0U;
    if (!TestRead(Source.VirtualAddress, Buffer, Size)) { return STATUS_PARTIAL_COPY; }
    *Copied = Size;
    return STATUS_SUCCESS;
}

static PVOID TestExport(PVOID Base, PCCH Name)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(image + dos->e_lfanew);
    IMAGE_EXPORT_DIRECTORY* exports = (IMAGE_EXPORT_DIRECTORY*)(image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    ULONG index;
    UNREFERENCED_PARAMETER(Base);
    for (index = 0UL; index < exports->NumberOfNames; ++index) {
        ULONG nameRva = ((ULONG*)(image + exports->AddressOfNames))[index];
        USHORT ordinal = ((USHORT*)(image + exports->AddressOfNameOrdinals))[index];
        if (strcmp((const char*)image + nameRva, Name) == 0) {
            return (PVOID)(imageBase + ((ULONG*)(image + exports->AddressOfFunctions))[ordinal]);
        }
    }
    return NULL;
}

static KIRQL TestIrql(VOID) { return testIrql; }
static VOID TestTime(PLARGE_INTEGER Time) { Time->QuadPart = 134000000000000000LL; }
static VOID TestInitString(PUNICODE_STRING String, PCWSTR Text)
{
    SIZE_T length = 0U;
    while (Text[length] != L'\0') { ++length; }
    String->Buffer = (PWSTR)Text;
    String->Length = (USHORT)(length * sizeof(WCHAR));
    String->MaximumLength = (USHORT)(String->Length + sizeof(WCHAR));
}
static BOOLEAN TestEqualString(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN IgnoreCase)
{
    UNREFERENCED_PARAMETER(IgnoreCase);
    return A->Length == B->Length && memcmp(A->Buffer, B->Buffer, A->Length) == 0;
}
static BOOLEAN TestProbeResource(ULONG_PTR Address) { return resourceAllowed && Address == imageBase + resourceRva; }
static BOOLEAN TestAcquire(PERESOURCE Resource, BOOLEAN Wait) {
    ++lockCalls;
    if (Wait || !TestProbeResource((ULONG_PTR)Resource)) { ++failures; return FALSE; }
    return lockAvailable;
}
static VOID TestRelease(PERESOURCE Resource) { UNREFERENCED_PARAMETER(Resource); }
static VOID TestCritical(VOID) {}
static PVOID TestAllocate(SIZE_T Bytes, ULONG Tag) { UNREFERENCED_PARAMETER(Tag); return calloc(1U, Bytes); }
static VOID TestFree(PVOID Buffer, ULONG Tag) { UNREFERENCED_PARAMETER(Tag); free(Buffer); }
static NTSTATUS TestModules(KSW_HOOK_SYSTEM_MODULE_INFORMATION** Modules, ULONG* Bytes) {
    *Bytes = sizeof(**Modules); *Modules = (KSW_HOOK_SYSTEM_MODULE_INFORMATION*)calloc(1U, *Bytes);
    if (*Modules == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    (*Modules)->NumberOfModules = 1UL; (*Modules)->Modules[0].ImageBase = (PVOID)imageBase;
    (*Modules)->Modules[0].ImageSize = imageSize;
    memcpy((*Modules)->Modules[0].FullPathName, "ci.dll", 7U);
    return STATUS_SUCCESS;
}
static VOID TestFileName(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Module, const UCHAR** Name, ULONG* Bytes) {
    *Name = Module->FullPathName; *Bytes = 6UL;
}
static BOOLEAN TestAnsiEqual(const UCHAR* Left, ULONG Bytes, PCSTR Right) {
    return Bytes == strlen(Right) && memcmp(Left, Right, Bytes) == 0;
}
#define KswordARKKernelProbeResourceIsSystemResource TestProbeResource
#define ExAcquireResourceSharedLite TestAcquire
#define ExReleaseResourceLite TestRelease
#define KeEnterCriticalRegion TestCritical
#define KeLeaveCriticalRegion TestCritical
#define KswordARKAllocateNonPagedPool TestAllocate
#define ExFreePoolWithTag TestFree
#define KswordARKHookBuildModuleSnapshot TestModules
#define KswordARKHookGetModuleFileName TestFileName
#define KswordARKHookBoundedAnsiEqualsInsensitive TestAnsiEqual
#undef KeGetCurrentIrql
#undef KeQuerySystemTime
#define KeGetCurrentIrql TestIrql
#define KeQuerySystemTime TestTime
#define MmCopyMemory TestCopy
#define MmSystemRangeStart testSystemRangeStart
#define RtlFindExportedRoutineByName TestExport
#define RtlInitUnicodeString TestInitString
#define RtlEqualUnicodeString TestEqualString
#include "@SCANNER@"
#include "@RESOLVER@"
#include "@CI_PRODUCTION@"

static VOID Expect(const char* Name, BOOLEAN Passed) {
    printf("%s %s\n", Passed ? "PASS" : "FAIL", Name); if (!Passed) { ++failures; }
}
static BOOLEAN LoadImage(const char* Path) {
    FILE* file = NULL; long length;
    if (fopen_s(&file, Path, "rb") != 0) { return FALSE; }
    fseek(file, 0L, SEEK_END); length = ftell(file); fseek(file, 0L, SEEK_SET);
    if (length <= 0L || length > 0x4000000L) { fclose(file); return FALSE; }
    imageSize = (ULONG)length; image = (UCHAR*)malloc(imageSize);
    if (image == NULL || fread(image, 1U, imageSize, file) != imageSize) { fclose(file); return FALSE; }
    fclose(file); return TRUE;
}
static VOID WriteName(UCHAR* Destination) {
    UNICODE_STRING name; TestInitString(&name, driverName); name.Buffer = (PWSTR)(ULONG_PTR)NAME_BASE;
    memcpy(Destination, &name, sizeof(name));
}
static VOID WriteResource(ULONG Rva) {
    ERESOURCE resource; RtlZeroMemory(&resource, sizeof(resource));
    resource.SystemResourcesList.Flink = (PLIST_ENTRY)(imageBase + Rva);
    resource.SystemResourcesList.Blink = (PLIST_ENTRY)(imageBase + Rva);
    memcpy(image + Rva, &resource, sizeof(resource)); resourceRva = Rva;
}
static VOID WritePiDdb(ULONG TableRva, ULONG LockRva) {
    RTL_AVL_TABLE table; RTL_BALANCED_LINKS first; RTL_BALANCED_LINKS second;
    ULONG timestamp = 0x65012345UL; NTSTATUS status = STATUS_SUCCESS;
    RtlZeroMemory(&table, sizeof(table)); RtlZeroMemory(&first, sizeof(first)); RtlZeroMemory(&second, sizeof(second));
    RtlZeroMemory(fixtureRecords, sizeof(fixtureRecords));
    table.BalancedRoot.Parent = (PRTL_BALANCED_LINKS)(imageBase + TableRva);
    table.BalancedRoot.RightChild = (PRTL_BALANCED_LINKS)(ULONG_PTR)RECORD_BASE;
    table.NumberGenericTableElements = 2UL; table.DepthOfTree = 2UL;
    table.CompareRoutine = (PRTL_AVL_COMPARE_ROUTINE)TestExport((PVOID)imageBase, "MmUnloadSystemImage");
    table.AllocateRoutine = (PRTL_AVL_ALLOCATE_ROUTINE)TestExport((PVOID)imageBase, "MmUnloadSystemImage");
    table.FreeRoutine = (PRTL_AVL_FREE_ROUTINE)TestExport((PVOID)imageBase, "MmUnloadSystemImage");
    first.Parent = &table.BalancedRoot; first.Parent = (PRTL_BALANCED_LINKS)(imageBase + TableRva);
    first.RightChild = (PRTL_BALANCED_LINKS)(ULONG_PTR)(RECORD_BASE + 512UL);
    second.Parent = (PRTL_BALANCED_LINKS)(ULONG_PTR)RECORD_BASE;
    memcpy(fixtureRecords, &first, sizeof(first)); memcpy(fixtureRecords + 512UL, &second, sizeof(second));
    WriteName(fixtureRecords + sizeof(first) + 16UL); WriteName(fixtureRecords + 512UL + sizeof(second) + 16UL);
    memcpy(fixtureRecords + sizeof(first) + 32UL, &timestamp, sizeof(timestamp));
    memcpy(fixtureRecords + 512UL + sizeof(second) + 32UL, &timestamp, sizeof(timestamp));
    memcpy(fixtureRecords + sizeof(first) + 36UL, &status, sizeof(status));
    memcpy(fixtureRecords + 512UL + sizeof(second) + 36UL, &status, sizeof(status));
    memcpy(image + TableRva, &table, sizeof(table)); WriteResource(LockRva);
}
int main(int Argc, char** Argv) {
    ULONG tableRva; ULONG lockRva; ULONG listRva; ULONG ciLockRva; ULONG before;
    KSW_DYN_MODULE_IDENTITY_PACKET identity; KSW_RUNTIME_KERNEL_LAYOUT layout;
    KSW_DYN_V4_CI_KERNEL_HASH_LAYOUT ciLayout; NTSTATUS status; ULONG pass;
    if (Argc != 7) { return 2; }
    tableRva = strtoul(Argv[3], NULL, 0); lockRva = strtoul(Argv[4], NULL, 0);
    listRva = strtoul(Argv[5], NULL, 0); ciLockRva = strtoul(Argv[6], NULL, 0);
    if (!LoadImage(Argv[1])) { return 2; }
    for (pass = 0UL; pass < 2UL; ++pass) {
        WritePiDdb(tableRva, lockRva); RtlZeroMemory(&identity, sizeof(identity));
        identity.present = 1UL; identity.imageBase = imageBase; identity.sizeOfImage = imageSize;
        RtlFillMemory(&layout, sizeof(layout), 0xFF);
        status = KswordARKDriverResolvePiDdbLayout(&identity, tableRva, lockRva, &layout);
        Expect("exact PiDDB globals resolve private members after PDB application", status == STATUS_SUCCESS &&
            layout.PiDdbDriverName == 16L && layout.PiDdbTimeDateStamp == 32L &&
            layout.PiDdbLoadStatus == 36L && layout.PiDdbTypeSize == 40L);
        before = lockCalls; resourceAllowed = FALSE;
        status = KswordARKDriverResolvePiDdbLayout(&identity, tableRva, lockRva, &layout);
        Expect("unproven PiDDB resource never reaches acquire", status == STATUS_OBJECT_TYPE_MISMATCH && lockCalls == before);
        resourceAllowed = TRUE; lockAvailable = FALSE;
        status = KswordARKDriverResolvePiDdbLayout(&identity, tableRva, lockRva, &layout);
        Expect("busy PiDDB lock reports busy", status == STATUS_LOCK_NOT_GRANTED);
        lockAvailable = TRUE;
        ((RTL_BALANCED_LINKS*)fixtureRecords)->RightChild = (PRTL_BALANCED_LINKS)(ULONG_PTR)RECORD_BASE;
        status = KswordARKDriverResolvePiDdbLayout(&identity, tableRva, lockRva, &layout);
        Expect("PiDDB AVL cycle rejected", status == STATUS_DATA_ERROR);
        WritePiDdb(tableRva, lockRva); WriteName(fixtureRecords + sizeof(RTL_BALANCED_LINKS) + 64UL);
        WriteName(fixtureRecords + 512UL + sizeof(RTL_BALANCED_LINKS) + 64UL);
        status = KswordARKDriverResolvePiDdbLayout(&identity, tableRva, lockRva, &layout);
        Expect("ambiguous private name layout rejected", status == STATUS_NOT_SUPPORTED);
        testIrql = APC_LEVEL; before = lockCalls;
        status = KswordARKDriverResolvePiDdbLayout(&identity, tableRva, lockRva, &layout);
        Expect("non-PASSIVE PiDDB resolution rejected before acquire", status == STATUS_INVALID_DEVICE_STATE && lockCalls == before);
        testIrql = PASSIVE_LEVEL; imageBase += 0x10000000ULL;
    }
    free(image); if (!LoadImage(Argv[2])) { return 2; }
    for (pass = 0UL; pass < 2UL; ++pass) {
        ULONG_PTR pointer = RECORD_BASE; ULONG_PTR next = RECORD_BASE + 512UL;
        RtlZeroMemory(fixtureRecords, sizeof(fixtureRecords));
        memcpy(fixtureRecords, &next, sizeof(next)); WriteName(fixtureRecords + 8UL); WriteName(fixtureRecords + 512UL + 8UL);
        // Hash payload that resembles a timestamp/status pair must stay unclassified.
        *(ULONG*)(fixtureRecords + 24UL) = 0x65012345UL; *(ULONG*)(fixtureRecords + 28UL) = 0xC0000001UL;
        *(ULONG*)(fixtureRecords + 512UL + 24UL) = 0x65012345UL; *(ULONG*)(fixtureRecords + 512UL + 28UL) = 0xC0000001UL;
        memcpy(image + listRva, &pointer, sizeof(pointer)); WriteResource(ciLockRva);
        status = KswordARKCiHashResolveRuntimeLayout(&ciLayout);
        Expect("CI full production resolver reaches late sections after nonempty anchors", status == STATUS_SUCCESS &&
            ciLayout.KernelHashBucketListRva == listRva && ciLayout.HashCacheLockRva == ciLockRva &&
            ciLayout.EntryNext == 0UL && ciLayout.EntryDriverName == 8UL && ciLayout.EntryTypeSize == 24UL);
        Expect("CI opaque payload is not published as timestamp/status/image", ciLayout.EntryTimeDateStamp == KSW_DYN_OFFSET_UNAVAILABLE &&
            ciLayout.EntryLoadStatus == KSW_DYN_OFFSET_UNAVAILABLE && ciLayout.EntryImageBase == KSW_DYN_OFFSET_UNAVAILABLE);
        resourceAllowed = FALSE; before = lockCalls;
        status = KswordARKCiHashResolveRuntimeLayout(&ciLayout);
        Expect("unproven CI resource never reaches acquire", !NT_SUCCESS(status) && lockCalls == before);
        resourceAllowed = TRUE; lockAvailable = FALSE;
        status = KswordARKCiHashResolveRuntimeLayout(&ciLayout);
        Expect("busy CI lock reports busy", status == STATUS_LOCK_NOT_GRANTED);
        lockAvailable = TRUE;
        memcpy(fixtureRecords + 512UL, &pointer, sizeof(pointer)); before = lockCalls;
        status = KswordARKCiHashResolveRuntimeLayout(&ciLayout);
        Expect("CI chain cycle rejected before acquire", !NT_SUCCESS(status) && lockCalls == before);
        imageBase += 0x10000000ULL;
    }
    free(image); printf("CACHE_LAYOUT_FALLBACK failures=%lu\n", failures); return failures != 0UL;
}

'@
$taskCode = $taskCode.Replace('@CI_PRODUCTION@', $taskCiProduction).Replace('@SCANNER@', $taskScanner.Replace('\', '/')).Replace('@RESOLVER@', $taskResolver.Replace('\', '/'))
[IO.File]::WriteAllText($taskSource, $taskCode, [Text.UTF8Encoding]::new($false))
$taskIncludes = @(
    (Join-Path $taskKit 'km'), (Join-Path $taskKit 'shared'), (Join-Path $taskKit 'ucrt'),
    'C:\Program Files (x86)\Windows Kits\10\Include\wdf\kmdf\1.15',
    (Join-Path $taskRoot 'KswordARKDriver\include'), (Join-Path $taskRoot 'KswordARKDriver'),
    (Join-Path $taskRoot 'KswordARKDriver\src\platform'), (Join-Path $taskRoot 'KswordARKDriver\src\features\kernel'), (Join-Path $taskRoot 'shared')
)
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/Gy', '/utf-8', '/TC', '/D_AMD64_', '/DAMD64')
$taskCompile += $taskIncludes | ForEach-Object { '/I"{0}"' -f $_ }
$taskCompile += @(
    ('/Fo"{0}"' -f (Join-Path $taskOutput 'runtime_unloaded_scan_regression.obj')),
    ('/Fe"{0}"' -f $taskExecutable), ('"{0}"' -f $taskSource), '/link', '/OPT:REF'
)
& cmd.exe /d /s /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExecutable $taskMapped $taskCiMapped $PiDdbTableRva $PiDdbLockRva $CiListRva $CiLockRva
exit $LASTEXITCODE
