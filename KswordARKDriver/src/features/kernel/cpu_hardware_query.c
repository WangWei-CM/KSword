/*++

Module Name:

    cpu_hardware_query.c

Abstract:

    Read-only CPU hardware snapshot collection for the HardwareDock utilization
    page. The implementation intentionally limits itself to CPUID and processor
    count APIs so the request is safe for periodic UI sampling.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <intrin.h>
#include <ntstrsafe.h>

#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xFFFFU
#endif

#define KSW_CPUID_EAX 0
#define KSW_CPUID_EBX 1
#define KSW_CPUID_ECX 2
#define KSW_CPUID_EDX 3

static VOID
KswordARKCpuHardwareCpuid(
    _Out_writes_(4) int Registers[4],
    _In_ int Leaf
    )
/*++

Routine Description:

    Execute CPUID for a basic leaf.

Arguments:

    Registers - Four-integer output array receiving EAX/EBX/ECX/EDX.
    Leaf - CPUID leaf value.

Return Value:

    None. Output is zero-filled before CPUID executes.

--*/
{
    RtlZeroMemory(Registers, sizeof(int) * 4);
    __cpuid(Registers, Leaf);
}

static VOID
KswordARKCpuHardwareCpuidEx(
    _Out_writes_(4) int Registers[4],
    _In_ int Leaf,
    _In_ int SubLeaf
    )
/*++

Routine Description:

    Execute CPUID for a leaf/subleaf pair.

Arguments:

    Registers - Four-integer output array receiving EAX/EBX/ECX/EDX.
    Leaf - CPUID leaf value.
    SubLeaf - CPUID subleaf value.

Return Value:

    None. Output is zero-filled before CPUID executes.

--*/
{
    RtlZeroMemory(Registers, sizeof(int) * 4);
    __cpuidex(Registers, Leaf, SubLeaf);
}

static VOID
KswordARKCpuHardwareCopyAscii(
    _Out_writes_(DestinationChars) char* Destination,
    _In_ ULONG DestinationChars,
    _In_reads_bytes_opt_(SourceBytes) const char* Source,
    _In_ ULONG SourceBytes
    )
/*++

Routine Description:

    Copy a bounded ASCII CPUID string into a protocol buffer.

Arguments:

    Destination - Fixed output buffer.
    DestinationChars - Output capacity including NUL.
    Source - Optional source bytes.
    SourceBytes - Number of source bytes to copy at most.

Return Value:

    None. Destination is always NUL-terminated when capacity is non-zero.

--*/
{
    ULONG copyBytes = 0;

    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    RtlZeroMemory(Destination, DestinationChars);
    if (Source == NULL || SourceBytes == 0UL) {
        return;
    }

    copyBytes = SourceBytes;
    if (copyBytes >= DestinationChars) {
        copyBytes = DestinationChars - 1UL;
    }

    RtlCopyMemory(Destination, Source, copyBytes);
    Destination[DestinationChars - 1UL] = '\0';
}

static VOID
KswordARKCpuHardwareTrimAscii(
    _Inout_updates_(BufferChars) char* Buffer,
    _In_ ULONG BufferChars
    )
/*++

Routine Description:

    Trim leading and trailing spaces from a fixed CPUID ASCII string in-place.

Arguments:

    Buffer - NUL-terminated buffer to normalize.
    BufferChars - Buffer capacity including NUL.

Return Value:

    None.

--*/
{
    ULONG start = 0;
    ULONG end = 0;
    ULONG length = 0;

    if (Buffer == NULL || BufferChars == 0UL) {
        return;
    }

    while (length < BufferChars && Buffer[length] != '\0') {
        ++length;
    }
    while (start < length && Buffer[start] == ' ') {
        ++start;
    }
    end = length;
    while (end > start && Buffer[end - 1UL] == ' ') {
        --end;
    }

    if (start > 0UL && end > start) {
        RtlMoveMemory(Buffer, Buffer + start, end - start);
    }
    Buffer[end - start] = '\0';
}

static ULONGLONG
KswordARKCpuHardwareBuildFeatureMask(
    _In_ ULONG Leaf1Ecx,
    _In_ ULONG Leaf1Edx,
    _In_ ULONG Leaf7Ebx,
    _In_ ULONG Leaf7Ecx,
    _In_ ULONG Leaf80000001Ecx,
    _In_ ULONG Leaf80000001Edx,
    _In_ ULONG Leaf80000007Edx
    )
/*++

Routine Description:

    Convert raw CPUID registers into Ksword's stable CPU feature bitmask.

Arguments:

    Leaf1Ecx/Leaf1Edx - CPUID leaf 1 feature registers.
    Leaf7Ebx/Leaf7Ecx - CPUID leaf 7 subleaf 0 feature registers.
    Leaf80000001Ecx/Leaf80000001Edx - Extended feature registers.
    Leaf80000007Edx - Extended power-management feature register.

Return Value:

    KSWORD_ARK_CPU_FEATURE_* mask.

--*/
{
    ULONGLONG mask = 0ULL;

#define KSW_SET_FEATURE_IF(_condition, _feature) \
    do { if ((_condition)) { mask |= (_feature); } } while (0)

    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 0)) != 0UL, KSWORD_ARK_CPU_FEATURE_SSE3);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 1)) != 0UL, KSWORD_ARK_CPU_FEATURE_PCLMULQDQ);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 5)) != 0UL, KSWORD_ARK_CPU_FEATURE_VMX);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 9)) != 0UL, KSWORD_ARK_CPU_FEATURE_SSSE3);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 12)) != 0UL, KSWORD_ARK_CPU_FEATURE_FMA);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 19)) != 0UL, KSWORD_ARK_CPU_FEATURE_SSE41);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 20)) != 0UL, KSWORD_ARK_CPU_FEATURE_SSE42);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 25)) != 0UL, KSWORD_ARK_CPU_FEATURE_AES);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 26)) != 0UL, KSWORD_ARK_CPU_FEATURE_XSAVE);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 27)) != 0UL, KSWORD_ARK_CPU_FEATURE_OSXSAVE);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 28)) != 0UL, KSWORD_ARK_CPU_FEATURE_AVX);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 30)) != 0UL, KSWORD_ARK_CPU_FEATURE_RDRAND);
    KSW_SET_FEATURE_IF((Leaf1Ecx & (1UL << 31)) != 0UL, KSWORD_ARK_CPU_FEATURE_HYPERVISOR);

    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 5)) != 0UL, KSWORD_ARK_CPU_FEATURE_MSR);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 6)) != 0UL, KSWORD_ARK_CPU_FEATURE_PAE);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 7)) != 0UL, KSWORD_ARK_CPU_FEATURE_MCE);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 8)) != 0UL, KSWORD_ARK_CPU_FEATURE_CX8);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 9)) != 0UL, KSWORD_ARK_CPU_FEATURE_APIC);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 11)) != 0UL, KSWORD_ARK_CPU_FEATURE_SEP);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 12)) != 0UL, KSWORD_ARK_CPU_FEATURE_MTRR);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 13)) != 0UL, KSWORD_ARK_CPU_FEATURE_PGE);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 14)) != 0UL, KSWORD_ARK_CPU_FEATURE_MCA);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 15)) != 0UL, KSWORD_ARK_CPU_FEATURE_CMOV);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 16)) != 0UL, KSWORD_ARK_CPU_FEATURE_PAT);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 17)) != 0UL, KSWORD_ARK_CPU_FEATURE_PSE36);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 19)) != 0UL, KSWORD_ARK_CPU_FEATURE_CLFSH);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 23)) != 0UL, KSWORD_ARK_CPU_FEATURE_MMX);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 24)) != 0UL, KSWORD_ARK_CPU_FEATURE_FXSR);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 25)) != 0UL, KSWORD_ARK_CPU_FEATURE_SSE);
    KSW_SET_FEATURE_IF((Leaf1Edx & (1UL << 26)) != 0UL, KSWORD_ARK_CPU_FEATURE_SSE2);

    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 0)) != 0UL, KSWORD_ARK_CPU_FEATURE_FSGSBASE);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 3)) != 0UL, KSWORD_ARK_CPU_FEATURE_BMI1);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 4)) != 0UL, KSWORD_ARK_CPU_FEATURE_HLE);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 5)) != 0UL, KSWORD_ARK_CPU_FEATURE_AVX2);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 7)) != 0UL, KSWORD_ARK_CPU_FEATURE_SMEP);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 8)) != 0UL, KSWORD_ARK_CPU_FEATURE_BMI2);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 9)) != 0UL, KSWORD_ARK_CPU_FEATURE_ERMS);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 10)) != 0UL, KSWORD_ARK_CPU_FEATURE_INVPCID);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 11)) != 0UL, KSWORD_ARK_CPU_FEATURE_RTM);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 16)) != 0UL, KSWORD_ARK_CPU_FEATURE_AVX512F);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 18)) != 0UL, KSWORD_ARK_CPU_FEATURE_RDSEED);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 19)) != 0UL, KSWORD_ARK_CPU_FEATURE_ADX);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 20)) != 0UL, KSWORD_ARK_CPU_FEATURE_SMAP);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 23)) != 0UL, KSWORD_ARK_CPU_FEATURE_CLFLUSHOPT);
    KSW_SET_FEATURE_IF((Leaf7Ebx & (1UL << 24)) != 0UL, KSWORD_ARK_CPU_FEATURE_CLWB);
    KSW_SET_FEATURE_IF((Leaf7Ecx & (1UL << 2)) != 0UL, KSWORD_ARK_CPU_FEATURE_UMIP);
    KSW_SET_FEATURE_IF((Leaf7Ecx & (1UL << 3)) != 0UL, KSWORD_ARK_CPU_FEATURE_PKU);
    KSW_SET_FEATURE_IF((Leaf7Ecx & (1UL << 4)) != 0UL, KSWORD_ARK_CPU_FEATURE_OSPKE);

    KSW_SET_FEATURE_IF((Leaf80000001Ecx & (1UL << 0)) != 0UL, KSWORD_ARK_CPU_FEATURE_LAHF_LM);
    KSW_SET_FEATURE_IF((Leaf80000001Edx & (1UL << 20)) != 0UL, KSWORD_ARK_CPU_FEATURE_NX);
    KSW_SET_FEATURE_IF((Leaf80000001Edx & (1UL << 27)) != 0UL, KSWORD_ARK_CPU_FEATURE_RDTSCP);
    KSW_SET_FEATURE_IF((Leaf80000001Edx & (1UL << 29)) != 0UL, KSWORD_ARK_CPU_FEATURE_LM);
    KSW_SET_FEATURE_IF((Leaf80000007Edx & (1UL << 8)) != 0UL, KSWORD_ARK_CPU_FEATURE_INVARIANT_TSC);

#undef KSW_SET_FEATURE_IF

    return mask;
}

NTSTATUS
KswordARKDriverQueryCpuHardware(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Build a fixed-size read-only CPU hardware snapshot from CPUID and kernel
    processor-count APIs.

Arguments:

    OutputBuffer - Caller-supplied response buffer.
    OutputBufferLength - Size of OutputBuffer in bytes.
    BytesWrittenOut - Receives sizeof(KSWORD_ARK_QUERY_CPU_HARDWARE_RESPONSE).

Return Value:

    STATUS_SUCCESS on a full response; STATUS_BUFFER_TOO_SMALL or
    STATUS_INVALID_PARAMETER for malformed caller buffers.

--*/
{
    KSWORD_ARK_QUERY_CPU_HARDWARE_RESPONSE* response = NULL;
    int leaf0[4] = { 0 };
    int leaf1[4] = { 0 };
    int leaf7[4] = { 0 };
    int leaf80000000[4] = { 0 };
    int leaf80000001[4] = { 0 };
    int leaf80000007[4] = { 0 };
    char vendorBytes[12] = { 0 };
    int brandLeaf[12] = { 0 };
    char brandBytes[48] = { 0 };
    ULONG eax = 0;
    ULONG ebx = 0;
    ULONG ecx = 0;
    ULONG edx = 0;
    ULONG extendedFamily = 0;
    ULONG extendedModel = 0;
    ULONG familyId = 0;
    ULONG modelId = 0;
    ULONG activeProcessorCount = 0;

    if (BytesWrittenOut == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0;

    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_CPU_HARDWARE_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    response = (KSWORD_ARK_QUERY_CPU_HARDWARE_RESPONSE*)OutputBuffer;
    RtlZeroMemory(response, sizeof(*response));
    response->version = KSWORD_ARK_CPU_HARDWARE_PROTOCOL_VERSION;
    response->lastStatus = STATUS_SUCCESS;

    KswordARKCpuHardwareCpuid(leaf0, 0);
    response->maxBasicLeaf = (ULONG)leaf0[KSW_CPUID_EAX];
    RtlCopyMemory(vendorBytes + 0, &leaf0[KSW_CPUID_EBX], sizeof(int));
    RtlCopyMemory(vendorBytes + 4, &leaf0[KSW_CPUID_EDX], sizeof(int));
    RtlCopyMemory(vendorBytes + 8, &leaf0[KSW_CPUID_ECX], sizeof(int));
    KswordARKCpuHardwareCopyAscii(response->vendor, KSWORD_ARK_CPU_HARDWARE_VENDOR_CHARS, vendorBytes, sizeof(vendorBytes));

    if (response->maxBasicLeaf >= 1UL) {
        KswordARKCpuHardwareCpuid(leaf1, 1);
        eax = (ULONG)leaf1[KSW_CPUID_EAX];
        ebx = (ULONG)leaf1[KSW_CPUID_EBX];
        ecx = (ULONG)leaf1[KSW_CPUID_ECX];
        edx = (ULONG)leaf1[KSW_CPUID_EDX];

        response->stepping = eax & 0xFUL;
        modelId = (eax >> 4) & 0xFUL;
        familyId = (eax >> 8) & 0xFUL;
        response->processorType = (eax >> 12) & 0x3UL;
        extendedModel = (eax >> 16) & 0xFUL;
        extendedFamily = (eax >> 20) & 0xFFUL;
        response->family = familyId == 0xFUL ? familyId + extendedFamily : familyId;
        response->model = (familyId == 0x6UL || familyId == 0xFUL)
            ? modelId + (extendedModel << 4)
            : modelId;
        response->brandIndex = ebx & 0xFFUL;
        response->clflushLineSize = ((ebx >> 8) & 0xFFUL) * 8UL;
        response->logicalProcessorCount = (ebx >> 16) & 0xFFUL;
        response->initialApicId = (ebx >> 24) & 0xFFUL;
        response->leaf1Ecx = (ULONGLONG)ecx;
        response->leaf1Edx = (ULONGLONG)edx;
    }

    if (response->maxBasicLeaf >= 7UL) {
        KswordARKCpuHardwareCpuidEx(leaf7, 7, 0);
        response->leaf7Ebx = (ULONGLONG)(ULONG)leaf7[KSW_CPUID_EBX];
        response->leaf7Ecx = (ULONGLONG)(ULONG)leaf7[KSW_CPUID_ECX];
        response->leaf7Edx = (ULONGLONG)(ULONG)leaf7[KSW_CPUID_EDX];
    }

    KswordARKCpuHardwareCpuid(leaf80000000, (int)0x80000000UL);
    response->maxExtendedLeaf = (ULONG)leaf80000000[KSW_CPUID_EAX];
    if (response->maxExtendedLeaf >= 0x80000001UL) {
        KswordARKCpuHardwareCpuid(leaf80000001, (int)0x80000001UL);
        response->leaf80000001Ecx = (ULONGLONG)(ULONG)leaf80000001[KSW_CPUID_ECX];
        response->leaf80000001Edx = (ULONGLONG)(ULONG)leaf80000001[KSW_CPUID_EDX];
    }
    if (response->maxExtendedLeaf >= 0x80000004UL) {
        KswordARKCpuHardwareCpuid(&brandLeaf[0], (int)0x80000002UL);
        KswordARKCpuHardwareCpuid(&brandLeaf[4], (int)0x80000003UL);
        KswordARKCpuHardwareCpuid(&brandLeaf[8], (int)0x80000004UL);
        RtlCopyMemory(&brandBytes[0], &brandLeaf[0], sizeof(brandBytes));
        KswordARKCpuHardwareCopyAscii(response->brand, KSWORD_ARK_CPU_HARDWARE_BRAND_CHARS, brandBytes, sizeof(brandBytes));
        KswordARKCpuHardwareTrimAscii(response->brand, KSWORD_ARK_CPU_HARDWARE_BRAND_CHARS);
    }
    if (response->maxExtendedLeaf >= 0x80000007UL) {
        KswordARKCpuHardwareCpuid(leaf80000007, (int)0x80000007UL);
    }

    activeProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    response->activeProcessorCount = activeProcessorCount;
    if (response->logicalProcessorCount == 0UL) {
        response->logicalProcessorCount = activeProcessorCount;
    }
    response->packageCount = 0UL;
    response->featureMask = KswordARKCpuHardwareBuildFeatureMask(
        (ULONG)response->leaf1Ecx,
        (ULONG)response->leaf1Edx,
        (ULONG)response->leaf7Ebx,
        (ULONG)response->leaf7Ecx,
        (ULONG)response->leaf80000001Ecx,
        (ULONG)response->leaf80000001Edx,
        (ULONG)leaf80000007[KSW_CPUID_EDX]);

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
