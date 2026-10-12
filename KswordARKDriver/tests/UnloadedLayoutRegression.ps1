[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$taskOutput = Join-Path $taskRoot '.codex-build-logs\unloaded-layout-regression'
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) {
    $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat'
}
$taskKit = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
foreach ($taskRequired in @($taskVcvars, $taskKit)) {
    if (!(Test-Path -LiteralPath $taskRequired)) { throw "Required input missing: $taskRequired" }
}
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskSource = Join-Path $taskOutput 'unloaded_layout_regression.c'
$taskExecutable = Join-Path $taskOutput 'unloaded_layout_regression.exe'
$taskResolver = Join-Path $taskOutput 'kernel_cache_unloaded_prefix.c'
$taskProduction = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\kernel_cache_fallback.c'))
$taskCut = [regex]::Match($taskProduction, 'static BOOLEAN\s+KswordARKKernelCacheAvlTableIsPlausible\(')
if (!$taskCut.Success) { throw 'Cannot locate the end of the production MmUnloadedDrivers inference routines.' }
# Keep the exact production inference code; unrelated AVL and export scanning
# entry points require kernel APIs and are deliberately outside this replay.
[IO.File]::WriteAllText($taskResolver, $taskProduction.Substring(0, $taskCut.Index), [Text.UTF8Encoding]::new($false))

# Compile the real resolver and inference code with a bounded synthetic memory
# reader. This replay never opens a driver device or accesses kernel memory.
$taskCode = @'
#include <ntifs.h>
#include <stdio.h>
#include <string.h>
#pragma warning(push)
#pragma warning(disable: 4324) // WDK/WDF headers intentionally align framework request structures.
#include "ark/ark_dyndata.h"
#include "ark/ark_startup.h"
#include "kernel_cache_fallback.h"
#include "runtime_signature_scan.h"
#include "kernel_object_probe.h"
#include "pool_compat.h"
#pragma warning(pop)

#define IMAGE_BASE 0xFFFF800000100000ULL
#define RECORD_BASE 0xFFFF800000200000ULL
#define NAME_BASE 0xFFFF800000300000ULL
#define POINTER_RVA 0x100UL

static UCHAR imageBytes[4096];
static UCHAR recordBytes[6400];
static WCHAR driverName[] = L"fixture.sys";
static KSW_DYN_STATE dynState;
static KIRQL testIrql;
static BOOLEAN readableGlobal;
static BOOLEAN writableGlobal;
static ULONG readCalls;
static ULONG failures;
static ULONG piResolveCalls;
static NTSTATUS piResolveStatus;
static PVOID testSystemRangeStart = (PVOID)(ULONG_PTR)0xFFFF800000000000ULL;

static KIRQL TestIrql(VOID) { return testIrql; }
static VOID TestTime(PLARGE_INTEGER Time) { Time->QuadPart = 134000000000000000LL; }
static VOID TestSnapshot(KSW_DYN_STATE* State) { *State = dynState; }

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

static BOOLEAN TestRead(const VOID* Address, VOID* Buffer, SIZE_T Size)
{
    ULONGLONG address = (ULONGLONG)(ULONG_PTR)Address;
    ULONGLONG bases[3] = { IMAGE_BASE, RECORD_BASE, NAME_BASE };
    const UCHAR* buffers[3] = { imageBytes, recordBytes, (const UCHAR*)driverName };
    SIZE_T sizes[3] = { sizeof(imageBytes), sizeof(recordBytes), sizeof(driverName) };
    ULONG index;
    ++readCalls;
    if (testIrql > APC_LEVEL || Buffer == NULL || Size == 0U) { return FALSE; }
    if (!readableGlobal && address == IMAGE_BASE + POINTER_RVA) { return FALSE; }
    for (index = 0UL; index < 3UL; ++index) {
        if (address >= bases[index] && address - bases[index] < sizes[index] &&
            Size <= sizes[index] - (SIZE_T)(address - bases[index])) {
            memcpy(Buffer, buffers[index] + (SIZE_T)(address - bases[index]), Size);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN TestImage(PVOID Base, ULONG Size, PKSW_RUNTIME_IMAGE_VIEW View)
{
    if ((ULONGLONG)(ULONG_PTR)Base != IMAGE_BASE || Size != sizeof(imageBytes)) { return FALSE; }
    RtlZeroMemory(View, sizeof(*View));
    View->Base = (ULONG_PTR)Base;
    View->Size = Size;
    return TRUE;
}

static BOOLEAN TestWritable(const KSW_RUNTIME_IMAGE_VIEW* View, ULONG_PTR Address, SIZE_T Size)
{
    UNREFERENCED_PARAMETER(View);
    return writableGlobal && (Address == IMAGE_BASE + POINTER_RVA || Address == IMAGE_BASE + 0x200UL) && Size <= sizeof(PVOID);
}

/* Only mock the completion boundary here; CacheLayoutFallbackRegression runs
   its actual production inference against bounded memory. */
static NTSTATUS TestPiDdbLayout(const KSW_DYN_MODULE_IDENTITY_PACKET* Identity,
    ULONG TableRva, ULONG LockRva, PKSW_RUNTIME_KERNEL_LAYOUT Layout)
{
    ++piResolveCalls;
    if (Identity->imageBase != IMAGE_BASE || TableRva != 0x200UL || LockRva != 0x300UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (NT_SUCCESS(piResolveStatus)) {
        Layout->PiDdbDriverName = 16;
        Layout->PiDdbTimeDateStamp = 32;
        Layout->PiDdbLoadStatus = 36;
        Layout->PiDdbTypeSize = 40;
    }
    return piResolveStatus;
}

#undef KeGetCurrentIrql
#undef KeQuerySystemTime
#define KeGetCurrentIrql TestIrql
#define KeQuerySystemTime TestTime
#define MmSystemRangeStart testSystemRangeStart
#define RtlInitUnicodeString TestInitString
#define RtlEqualUnicodeString TestEqualString
#define KswordARKDynDataSnapshot TestSnapshot
#define KswordARKRuntimeReadMemory TestRead
#define KswordARKRuntimeInitializeImageView TestImage
#define KswordARKRuntimeAddressIsWritableData TestWritable
#define KswordARKDriverResolvePiDdbLayout TestPiDdbLayout
#include "@UNLOADED_RESOLVER@"
#include "@ROOT@/KswordARKDriver/src/features/kernel/kernel_unloaded_layout.c"

static VOID Expect(const char* Name, NTSTATUS Actual, NTSTATUS Expected)
{
    if (Actual != Expected) {
        printf("FAIL %s actual=0x%08lX expected=0x%08lX\n", Name, (ULONG)Actual, (ULONG)Expected);
        ++failures;
    } else { printf("PASS %s\n", Name); }
}

static VOID WriteRecord(SIZE_T Offset)
{
    UNICODE_STRING name;
    ULONGLONG start = 0xFFFFF80230000000ULL;
    ULONGLONG end = start + 0x4000ULL;
    LONGLONG time = 133999999900000000LL;
    TestInitString(&name, driverName);
    name.Buffer = (PWSTR)(ULONG_PTR)NAME_BASE;
    memcpy(recordBytes + Offset, &name, sizeof(name));
    memcpy(recordBytes + Offset + 16U, &start, sizeof(start));
    memcpy(recordBytes + Offset + 24U, &end, sizeof(end));
    memcpy(recordBytes + Offset + 32U, &time, sizeof(time));
}

static VOID Reset(VOID)
{
    ULONGLONG pointer = RECORD_BASE;
    RtlZeroMemory(imageBytes, sizeof(imageBytes));
    RtlZeroMemory(recordBytes, sizeof(recordBytes));
    RtlZeroMemory(&dynState, sizeof(dynState));
    RtlFillMemory(&dynState.Kernel, sizeof(dynState.Kernel), 0xFF);
    RtlFillMemory(&dynState.KernelGlobals, sizeof(dynState.KernelGlobals), 0xFF);
    dynState.Initialized = TRUE;
    dynState.Ntoskrnl.present = 1UL;
    dynState.Ntoskrnl.imageBase = IMAGE_BASE;
    dynState.Ntoskrnl.sizeOfImage = sizeof(imageBytes);
    dynState.KernelGlobals.MmUnloadedDrivers = POINTER_RVA;
    dynState.KernelGlobalSources.MmUnloadedDrivers = KSW_DYN_FIELD_SOURCE_PDB_PROFILE;
    memcpy(imageBytes + POINTER_RVA, &pointer, sizeof(pointer));
    WriteRecord(0U);
    WriteRecord(40U);
    testIrql = PASSIVE_LEVEL;
    readableGlobal = TRUE;
    writableGlobal = TRUE;
    readCalls = 0UL;
    piResolveCalls = 0UL;
    piResolveStatus = STATUS_NOT_SUPPORTED;
}

static VOID SetTrustedFields(ULONG Source)
{
    dynState.Kernel.UldName = 0UL;
    dynState.Kernel.UldStartAddress = 16UL;
    dynState.Kernel.UldEndAddress = 24UL;
    dynState.Kernel.UldCurrentTime = 32UL;
    dynState.Kernel.UldTypeSize = 40UL;
    dynState.KernelSources.UldName = Source;
    dynState.KernelSources.UldStartAddress = Source;
    dynState.KernelSources.UldEndAddress = Source;
    dynState.KernelSources.UldCurrentTime = Source;
    dynState.KernelSources.UldTypeSize = Source;
}

int main(void)
{
    KSW_MM_UNLOADED_LAYOUT layout;
    KSW_PIDDB_QUERY_LAYOUT piLayout;
    KSW_RUNTIME_IMAGE_VIEW view;
    KSW_RUNTIME_KERNEL_LAYOUT runtimeLayout;
    KSW_RUNTIME_DATA_REFERENCE references[3];
    ULONGLONG secondPointer = RECORD_BASE;
    Reset();
    Expect("exact global + live inference without NtosActive", KswordARKUnloadedResolveMmLayout(&layout), STATUS_SUCCESS);
    Expect("inferred field offsets", layout.RecordSize == 40UL && layout.NameOffset == 0UL &&
        layout.StartAddressOffset == 16UL && layout.EndAddressOffset == 24UL && layout.CurrentTimeOffset == 32UL ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    Expect("global DynData unchanged", !dynState.NtosActive && dynState.Kernel.UldName == 0xFFFFFFFFUL ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    Reset(); SetTrustedFields(KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN);
    dynState.KernelGlobalSources.MmUnloadedDrivers = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    Expect("complete runtime fields without NtosActive", KswordARKUnloadedResolveMmLayout(&layout), STATUS_SUCCESS);
    Reset(); SetTrustedFields(KSW_DYN_FIELD_SOURCE_PDB_PROFILE);
    Expect("complete PDB layout", KswordARKUnloadedResolveMmLayout(&layout), STATUS_SUCCESS);
    Reset(); dynState.Initialized = FALSE;
    Expect("uninitialized", KswordARKUnloadedResolveMmLayout(&layout), STATUS_DEVICE_NOT_READY);
    Reset(); dynState.Ntoskrnl.present = 0UL;
    Expect("missing identity", KswordARKUnloadedResolveMmLayout(&layout), STATUS_DEVICE_NOT_READY);
    Reset(); dynState.KernelGlobalSources.MmUnloadedDrivers = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
    Expect("untrusted global", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); dynState.KernelGlobals.MmUnloadedDrivers = 0xFFFFFFFFUL;
    Expect("missing global", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); RtlZeroMemory(recordBytes + 40U, 40U);
    Expect("one valid record cannot authorize layout", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); RtlZeroMemory(recordBytes, sizeof(recordBytes));
    Expect("empty records cannot authorize layout", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); RtlZeroMemory(recordBytes, sizeof(recordBytes)); WriteRecord(0U); WriteRecord(240U);
    Expect("ambiguous stride rejected", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); writableGlobal = FALSE;
    Expect("global outside writable data", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); testIrql = DISPATCH_LEVEL;
    Expect("high IRQL cannot infer layout", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); SetTrustedFields(KSW_DYN_FIELD_SOURCE_PDB_PROFILE); readableGlobal = FALSE;
    Expect("unreadable global", KswordARKUnloadedResolveMmLayout(&layout), STATUS_PARTIAL_COPY);
    Reset(); SetTrustedFields(KSW_DYN_FIELD_SOURCE_PDB_PROFILE); dynState.Kernel.UldName = 40UL;
    Expect("member outside record", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); SetTrustedFields(KSW_DYN_FIELD_SOURCE_PDB_PROFILE); dynState.Kernel.UldTypeSize = 4097UL;
    Expect("oversized record", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset(); SetTrustedFields(KSW_DYN_FIELD_SOURCE_PDB_PROFILE); dynState.KernelGlobals.MmUnloadedDrivers = sizeof(imageBytes) - 4UL;
    Expect("global extent outside image", KswordARKUnloadedResolveMmLayout(&layout), STATUS_NOT_SUPPORTED);
    Reset();
    dynState.Kernel.PiDdbDriverName = 0UL; dynState.Kernel.PiDdbTimeDateStamp = 16UL;
    dynState.Kernel.PiDdbLoadStatus = 20UL; dynState.Kernel.PiDdbTypeSize = 24UL;
    dynState.KernelSources.PiDdbDriverName = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    dynState.KernelSources.PiDdbTimeDateStamp = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    dynState.KernelSources.PiDdbLoadStatus = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    dynState.KernelSources.PiDdbTypeSize = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    dynState.KernelGlobals.PiDDBCacheTable = 0x200UL; dynState.KernelGlobals.PiDDBLock = 0x300UL;
    dynState.KernelGlobalSources.PiDDBCacheTable = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    dynState.KernelGlobalSources.PiDDBLock = KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN;
    Expect("PiDDB runtime layout without NtosActive", KswordARKUnloadedResolvePiDdbLayout(&piLayout), STATUS_SUCCESS);
    dynState.Kernel.PiDdbDriverName = 0xFFFFFFFFUL;
    Expect("PiDDB missing member rejected", KswordARKUnloadedResolvePiDdbLayout(&piLayout), STATUS_NOT_SUPPORTED);
    Expect("PiDDB trusted globals trigger completion", piResolveCalls == 1UL ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    piResolveStatus = STATUS_SUCCESS;
    Expect("PiDDB missing member completed locally", KswordARKUnloadedResolvePiDdbLayout(&piLayout), STATUS_SUCCESS);
    Expect("PiDDB completed layout matches resolver", piLayout.DriverNameOffset == 16UL &&
        piLayout.TimeDateStampOffset == 32UL && piLayout.LoadStatusOffset == 36UL &&
        piLayout.EntrySize == 40UL ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    Expect("PiDDB global snapshot unchanged", !dynState.NtosActive &&
        dynState.Kernel.PiDdbDriverName == KSW_DYN_OFFSET_UNAVAILABLE ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    piResolveStatus = STATUS_LOCK_NOT_GRANTED;
    Expect("PiDDB busy status preserved", KswordARKUnloadedResolvePiDdbLayout(&piLayout), STATUS_LOCK_NOT_GRANTED);
    dynState.KernelGlobalSources.PiDDBLock = KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
    piResolveCalls = 0UL;
    Expect("PiDDB untrusted global rejected", KswordARKUnloadedResolvePiDdbLayout(&piLayout), STATUS_NOT_SUPPORTED);
    Expect("PiDDB untrusted global cannot trigger completion", piResolveCalls == 0UL ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    Reset();
    TestImage((PVOID)(ULONG_PTR)IMAGE_BASE, sizeof(imageBytes), &view);
    RtlZeroMemory(references, sizeof(references));
    references[0].Address = IMAGE_BASE + POINTER_RVA;
    references[0].RoutineAddress = IMAGE_BASE + 0x400UL;
    references[1] = references[0]; references[1].RoutineAddress += 0x100UL;
    RtlFillMemory(&runtimeLayout, sizeof(runtimeLayout), 0xFF);
    KswordARKKernelCacheResolveUnloadedDrivers(&view, references, 2UL, &runtimeLayout);
    Expect("same global in different routines is not ambiguous",
        runtimeLayout.MmUnloadedDriversRva == POINTER_RVA ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    memcpy(imageBytes + 0x200UL, &secondPointer, sizeof(secondPointer));
    references[1].Address = IMAGE_BASE + 0x200UL;
    references[2] = references[0];
    RtlFillMemory(&runtimeLayout, sizeof(runtimeLayout), 0xFF);
    KswordARKKernelCacheResolveUnloadedDrivers(&view, references, 3UL, &runtimeLayout);
    Expect("distinct globals remain ambiguous despite duplicate reference",
        runtimeLayout.MmUnloadedDriversRva == -1 ? STATUS_SUCCESS : STATUS_DATA_ERROR, STATUS_SUCCESS);
    printf("failures=%lu\n", failures);
    return failures != 0UL;
}
'@
$taskCode = $taskCode.Replace('@ROOT@', $taskRoot.Replace('\', '/'))
$taskCode = $taskCode.Replace('@UNLOADED_RESOLVER@', $taskResolver.Replace('\', '/'))
[IO.File]::WriteAllText($taskSource, $taskCode, [Text.UTF8Encoding]::new($false))
$taskIncludes = @(
    (Join-Path $taskKit 'km'), (Join-Path $taskKit 'shared'), (Join-Path $taskKit 'ucrt'),
    'C:\Program Files (x86)\Windows Kits\10\Include\wdf\kmdf\1.15',
    (Join-Path $taskRoot 'KswordARKDriver\include'),
    (Join-Path $taskRoot 'KswordARKDriver'),
    (Join-Path $taskRoot 'KswordARKDriver\src\platform'), (Join-Path $taskRoot 'shared')
)
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/Gy', '/utf-8', '/TC', '/D_AMD64_', '/DAMD64')
$taskCompile += $taskIncludes | ForEach-Object { '/I"{0}"' -f $_ }
$taskCompile += @(
    ('/Fo"{0}"' -f (Join-Path $taskOutput 'unloaded_layout_regression.obj')),
    ('/Fe"{0}"' -f $taskExecutable), ('"{0}"' -f $taskSource), '/link', '/OPT:REF'
)
& cmd.exe /d /s /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExecutable
exit $LASTEXITCODE
