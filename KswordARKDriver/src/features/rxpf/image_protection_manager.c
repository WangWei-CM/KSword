/*++

Module Name:

    image_protection_manager.c

Abstract:

    Exact-build MmChangeImageProtection resolver and managed test-page MDLs.

Environment:

    Kernel mode, PASSIVE_LEVEL control path only.

--*/

#include "image_protection_manager.h"
#include <ntimage.h>
#include "rxpf_sha512.h"
#include "src/platform/kernel_module_identity.h"
#include "src/platform/pool_compat.h"

NTSYSAPI
PVOID
NTAPI
RtlPcToFileHeader(
    _In_ PVOID PcValue,
    _Outptr_ PVOID* BaseOfImage
    );

#define KSW_RXPF_BACKUP_TAG 'bPxR'
#define KSW_RXPF_CONTENT_TAG 'cPxR'
#define KSW_RXPF_EXPECTED_NT_BUILD 26220UL
#define KSW_RXPF_EXPECTED_TIMESTAMP 0xB292FCA9UL
#define KSW_RXPF_EXPECTED_IMAGE_SIZE 0x01450000UL
#define KSW_RXPF_EXPECTED_CHECKSUM 0x00C7D4D4UL
#define KSW_RXPF_MM_CHANGE_RVA 0x00A3B300UL
#define KSW_RXPF_MM_CHANGE_OPERATION_RX 1UL
#define KSW_RXPF_MM_CHANGE_OPERATION_READONLY_NX 2UL

/* Build 22621 (Windows 11 22H2) ntoskrnl identity. */
#define KSW_RXPF_22621_NT_BUILD      22621UL
#define KSW_RXPF_22621_TIMESTAMP     0x2C33C508UL
#define KSW_RXPF_22621_IMAGE_SIZE    0x01047000UL
#define KSW_RXPF_22621_CHECKSUM      0x00B8C0BFUL
#define KSW_RXPF_22621_MM_CHANGE_RVA 0x00736CF0UL

/* Build 26300.9711 (Windows 11 24H2 + cumulative) profile — 本机 VM 实测。 */
/* PE 元数据由 scratchpad/extract_nt_meta.py 读取 C:\WINDOWS\System32\ntoskrnl.exe 得到。 */
/* MmChangeImageProtection RVA 由 memory_ioctl.c:change-image-protection */
/*   的 resolvedFunctionVa 字段减去 ntoskrnl base 实测确认，与 PE 符号一致。 */
/* 注：9457 → 9711 是 Windows Update 自动升级的结果；MmChangeImageProtection */
/*   的 64 字节 prologue 在两个 UBR 完全相同，只是 PDB GUID / TimeDateStamp */
/*   / CheckSum / RVA 变了。宏保留 KSW_RXPF_26300_* 命名表示同一 26300 family。 */
#define KSW_RXPF_26300_NT_BUILD      26300UL
#define KSW_RXPF_26300_TIMESTAMP     0xAFF6531FUL
#define KSW_RXPF_26300_IMAGE_SIZE    0x01450000UL
#define KSW_RXPF_26300_CHECKSUM      0x00C8AD9CUL
#define KSW_RXPF_26300_MM_CHANGE_RVA 0x00A565F0UL

#define KSW_RXPF_RSDS_SIGNATURE 0x53445352UL

/*
 * MmChangeImageProtection 真实 ABI（逆向自 26100/22621 ntoskrnl）:
 *   参数 2,3 是参考摘要缓冲区——**不是**代码页地址/长度。
 *   mode=2（移除 X）不经 VTL1，参数 2,3 仅用于 PFN 范围标记。
 *   mode=1（恢复 RX）经 VTL1，VTL1 将缓冲区内容作为 SHA-512
 *   尾段（[0x20,0x40) 字节）与目标页哈希做 memcmp。
 *   缓冲区必须位于 MDL->StartVa 所属模块的 [DllBase, DllBase+SizeOfImage) 内。
 */
typedef NTSTATUS
(NTAPI* KSW_RXPF_MM_CHANGE_IMAGE_PROTECTION)(
    _In_ PMDL Mdl,
    _In_ PVOID ReferenceBufferOrRange,
    _In_ ULONG ReferenceBufferLengthOrSize,
    _In_ ULONG Operation
    );

#pragma pack(push, 1)
typedef struct _KSW_RXPF_RSDS_HEADER
{
    ULONG Signature;
    GUID Guid;
    ULONG Age;
} KSW_RXPF_RSDS_HEADER, *PKSW_RXPF_RSDS_HEADER;
#pragma pack(pop)

typedef struct _KSW_RXPF_BUILD_PROFILE
{
    ULONG NtBuildNumber;
    ULONG ImageTimeDateStamp;
    ULONG ImageSize;
    ULONG ImageCheckSum;
    ULONG FunctionRva;
    ULONG FunctionParameterCount;
    ULONG OperationRx;
    ULONG OperationReadOnlyNx;
    ULONG PdbAge;
    const GUID* PdbGuid;
    const UCHAR* Signature;
    const UCHAR* SignatureMask;
    ULONG SignatureLength;
} KSW_RXPF_BUILD_PROFILE, *PKSW_RXPF_BUILD_PROFILE;

typedef struct _KSW_RXPF_IMAGE_PROTECTION_STATE
{
    PDRIVER_OBJECT DriverObject;
    PVOID KernelBase;
    ULONG NtBuildNumber;
    ULONG ImageTimeDateStamp;
    ULONG ImageSize;
    ULONG ImageCheckSum;
    ULONG BuildStatus;
    GUID PdbGuid;
    ULONG PdbAge;
    KSW_RXPF_MM_CHANGE_IMAGE_PROTECTION MmChangeImageProtection;
    const KSW_RXPF_BUILD_PROFILE* ActiveProfile;
    volatile LONG Initialized;
    volatile LONG BuildSupported;
    NTSTATUS LastStatus;
} KSW_RXPF_IMAGE_PROTECTION_STATE;

static KSW_RXPF_IMAGE_PROTECTION_STATE g_KswRxpfImageProtection;

/*
 * mode=1 参考摘要缓冲区——位于驱动 .data 节，地址在本映像的
 * [DllBase, DllBase+SizeOfImage) 内，因此通过 MmChangeImageProtection
 * 对 MDL->StartVa 所属模块的范围校验（自映像测试场景）。
 * VTL1 从此处读取 0x20 字节作为 SHA-512 尾段预期值。
 */
static DECLSPEC_ALIGN(16) UCHAR
    g_KswRxpfReferenceDigest[0x20];

static const GUID g_KswRxpfExpectedPdbGuid = {
    0x3E5A9A8BUL,
    0x6B78U,
    0x281FU,
    { 0x3BU, 0xE2U, 0x15U, 0x01U, 0x10U, 0x32U, 0x52U, 0x15U }
};

/* Build 22621 PDB: ntkrnlmp.pdb {9074FC2B-82ED-2B7E-1CB3-366B64BE62F9} age 1. */
static const GUID g_KswRxpfPdbGuid22621 = {
    0x9074FC2BUL,
    0x82EDU,
    0x2B7EU,
    { 0x1CU, 0xB3U, 0x36U, 0x6BU, 0x64U, 0xBEU, 0x62U, 0xF9U }
};

/* Build 26300.9711 PDB: ntkrnlmp.pdb {B661C6FE-7299-1D16-0FE3-7C48B8BC5470} age 1. */
/* 该 GUID 从 VM 本机 ntoskrnl.exe (10.0.26300.9711) 的 CodeView RSDS 直接读得。 */
static const GUID g_KswRxpfPdbGuid26300 = {
    0xB661C6FEUL,
    0x7299U,
    0x1D16U,
    { 0x0FU, 0xE3U, 0x7CU, 0x48U, 0xB8U, 0xBCU, 0x54U, 0x70U }
};

static const UCHAR g_KswRxpfMmChangeSignature[64] = {
    0x48U, 0x8BU, 0xC4U, 0x48U, 0x89U, 0x58U, 0x08U, 0x48U,
    0x89U, 0x68U, 0x10U, 0x48U, 0x89U, 0x70U, 0x18U, 0x44U,
    0x89U, 0x48U, 0x20U, 0x57U, 0x41U, 0x54U, 0x41U, 0x55U,
    0x41U, 0x56U, 0x41U, 0x57U, 0x48U, 0x83U, 0xECU, 0x30U,
    0x83U, 0x60U, 0xC8U, 0x00U, 0x49U, 0x8BU, 0xD8U, 0x41U,
    0x8DU, 0x41U, 0xFFU, 0x4CU, 0x8BU, 0xE2U, 0x48U, 0x8BU,
    0xF9U, 0x83U, 0xF8U, 0x01U, 0x0FU, 0x87U, 0x41U, 0x02U,
    0x00U, 0x00U, 0x44U, 0x8BU, 0xEBU, 0x49U, 0x3BU, 0xDDU
};

static const UCHAR g_KswRxpfMmChangeSignatureMask[64] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
};

/* Build 22621 MmChangeImageProtection prologue (64 bytes at RVA 0x00736CF0). */
static const UCHAR g_KswRxpfMmChangeSignature22621[64] = {
    0x48U, 0x89U, 0x5CU, 0x24U, 0x08U, 0x44U, 0x89U, 0x4CU,
    0x24U, 0x20U, 0x48U, 0x89U, 0x54U, 0x24U, 0x10U, 0x55U,
    0x56U, 0x57U, 0x41U, 0x54U, 0x41U, 0x55U, 0x41U, 0x56U,
    0x41U, 0x57U, 0x48U, 0x83U, 0xECU, 0x30U, 0x41U, 0x8DU,
    0x41U, 0xFFU, 0xBEU, 0x01U, 0x00U, 0x00U, 0x00U, 0x49U,
    0x8BU, 0xD8U, 0x4CU, 0x8BU, 0xEAU, 0x48U, 0x8BU, 0xF9U,
    0x3BU, 0xC6U, 0x0FU, 0x87U, 0xFAU, 0x23U, 0x19U, 0x00U,
    0x8BU, 0xC3U, 0x48U, 0x89U, 0x44U, 0x24U, 0x28U, 0x48U
};

/* Build 26300.9711 MmChangeImageProtection prologue (64 bytes at RVA 0x00A565F0). */
/* 和 26220 几乎逐字节一致，仅 offset 0x36-0x37 处的 JA (0F 87) 相对偏移不同 */
/* (26220: 0x41 0x02；26300: 0x2D 0x02)——函数内部条件跳转目标随 build 偏移。 */
/* 9457 → 9711 升级后，这 64 字节 prologue 完全未变（函数内部布局没动）， */
/*   只是 RVA 从 0xA5DDB0 偏移到 0xA565F0 (函数在 .text 段里被挪了 0x7C0 字节)。 */
/* 直接读 VM 的 ntoskrnl.exe 文件内容抓到；mask 复用全 0xFF 策略。 */
static const UCHAR g_KswRxpfMmChangeSignature26300[64] = {
    0x48U, 0x8BU, 0xC4U, 0x48U, 0x89U, 0x58U, 0x08U, 0x48U,
    0x89U, 0x68U, 0x10U, 0x48U, 0x89U, 0x70U, 0x18U, 0x44U,
    0x89U, 0x48U, 0x20U, 0x57U, 0x41U, 0x54U, 0x41U, 0x55U,
    0x41U, 0x56U, 0x41U, 0x57U, 0x48U, 0x83U, 0xECU, 0x30U,
    0x83U, 0x60U, 0xC8U, 0x00U, 0x49U, 0x8BU, 0xD8U, 0x41U,
    0x8DU, 0x41U, 0xFFU, 0x4CU, 0x8BU, 0xE2U, 0x48U, 0x8BU,
    0xF9U, 0x83U, 0xF8U, 0x01U, 0x0FU, 0x87U, 0x2DU, 0x02U,
    0x00U, 0x00U, 0x44U, 0x8BU, 0xEBU, 0x49U, 0x3BU, 0xDDU
};

static const KSW_RXPF_BUILD_PROFILE g_KswRxpfBuildProfiles[] = {
    {
        KSW_RXPF_EXPECTED_NT_BUILD,
        KSW_RXPF_EXPECTED_TIMESTAMP,
        KSW_RXPF_EXPECTED_IMAGE_SIZE,
        KSW_RXPF_EXPECTED_CHECKSUM,
        KSW_RXPF_MM_CHANGE_RVA,
        4UL,
        KSW_RXPF_MM_CHANGE_OPERATION_RX,
        KSW_RXPF_MM_CHANGE_OPERATION_READONLY_NX,
        1UL,
        &g_KswRxpfExpectedPdbGuid,
        g_KswRxpfMmChangeSignature,
        g_KswRxpfMmChangeSignatureMask,
        RTL_NUMBER_OF(g_KswRxpfMmChangeSignature)
    },
    {
        KSW_RXPF_22621_NT_BUILD,
        KSW_RXPF_22621_TIMESTAMP,
        KSW_RXPF_22621_IMAGE_SIZE,
        KSW_RXPF_22621_CHECKSUM,
        KSW_RXPF_22621_MM_CHANGE_RVA,
        4UL,
        KSW_RXPF_MM_CHANGE_OPERATION_RX,
        KSW_RXPF_MM_CHANGE_OPERATION_READONLY_NX,
        1UL,
        &g_KswRxpfPdbGuid22621,
        g_KswRxpfMmChangeSignature22621,
        g_KswRxpfMmChangeSignatureMask,
        RTL_NUMBER_OF(g_KswRxpfMmChangeSignature22621)
    },
    {
        /* Build 26300.9457 profile — 本机 VM 实测 build，补上让 Phase G 完整链能跑。 */
        KSW_RXPF_26300_NT_BUILD,
        KSW_RXPF_26300_TIMESTAMP,
        KSW_RXPF_26300_IMAGE_SIZE,
        KSW_RXPF_26300_CHECKSUM,
        KSW_RXPF_26300_MM_CHANGE_RVA,
        4UL,
        KSW_RXPF_MM_CHANGE_OPERATION_RX,
        KSW_RXPF_MM_CHANGE_OPERATION_READONLY_NX,
        1UL,
        &g_KswRxpfPdbGuid26300,
        g_KswRxpfMmChangeSignature26300,
        g_KswRxpfMmChangeSignatureMask,
        RTL_NUMBER_OF(g_KswRxpfMmChangeSignature26300)
    }
};

C_ASSERT(RTL_NUMBER_OF(g_KswRxpfBuildProfiles) == 3U);
C_ASSERT(sizeof(g_KswRxpfMmChangeSignature) == 64U);

/*
 * The linker gives this unique executable section its own page.  No handler or
 * control-path code resides here, so leaving it NX until image unload cannot
 * strand an unload callback.  Bytes encode MOV RAX,12345678h; ADD RAX,1; RET.
 */
#pragma section(".rxpftst", read, execute)
__declspec(allocate(".rxpftst"))
__declspec(align(PAGE_SIZE))
const UCHAR g_KswRxpfSelfImageTestPage[PAGE_SIZE] = {
    0x48U, 0xB8U, 0x78U, 0x56U, 0x34U, 0x12U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x48U, 0x83U, 0xC0U, 0x01U, 0xC3U
};

static BOOLEAN
KswRxpfCanonicalKernelPage(
    _In_ ULONGLONG Address
    )
{
    ULONGLONG high = Address >> 48;

    /* Require canonical sign extension, kernel range, and page alignment. */
    return high == 0xFFFFULL &&
        Address >= (ULONGLONG)(ULONG_PTR)MmSystemRangeStart &&
        (Address & (PAGE_SIZE - 1ULL)) == 0ULL;
}

static BOOLEAN
KswRxpfValidateExecutableSection(
    _In_ PVOID ImageBase,
    _In_ ULONG Rva
    )
{
    BOOLEAN valid = FALSE;

    /* Parse the loaded PE under exception protection before trusting sections. */
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)ImageBase;
        PIMAGE_NT_HEADERS64 nt = NULL;
        PIMAGE_SECTION_HEADER section = NULL;
        USHORT index = 0U;

        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0 ||
            (ULONG)dos->e_lfanew > 0x1000UL) {
            return FALSE;
        }
        nt = (PIMAGE_NT_HEADERS64)((PUCHAR)ImageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return FALSE;
        }
        section = IMAGE_FIRST_SECTION(nt);
        for (index = 0U; index < nt->FileHeader.NumberOfSections; ++index) {
            ULONG sectionSize = max(
                section[index].Misc.VirtualSize,
                section[index].SizeOfRawData);
            ULONG sectionEnd = section[index].VirtualAddress + sectionSize;

            if (sectionEnd < section[index].VirtualAddress) {
                continue;
            }
            if (Rva >= section[index].VirtualAddress && Rva < sectionEnd) {
                valid =
                    (section[index].Characteristics &
                        IMAGE_SCN_MEM_EXECUTE) != 0UL &&
                    (section[index].Characteristics &
                        IMAGE_SCN_MEM_DISCARDABLE) == 0UL;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        valid = FALSE;
    }
    return valid;
}

/* 根据 PE 节特征推算页面在 HVCI 下的有效保护属性。 */
static ULONG
KswRxpfQuerySectionProtection(
    _In_ PVOID ImageBase,
    _In_ ULONG Rva
    )
{
    ULONG protection = PAGE_EXECUTE_READ;

    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)ImageBase;
        PIMAGE_NT_HEADERS64 nt = NULL;
        PIMAGE_SECTION_HEADER section = NULL;
        USHORT index = 0U;

        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0 ||
            (ULONG)dos->e_lfanew > 0x1000UL) {
            return protection;
        }
        nt = (PIMAGE_NT_HEADERS64)((PUCHAR)ImageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return protection;
        }
        section = IMAGE_FIRST_SECTION(nt);
        for (index = 0U; index < nt->FileHeader.NumberOfSections; ++index) {
            ULONG sectionSize = max(
                section[index].Misc.VirtualSize,
                section[index].SizeOfRawData);
            ULONG sectionEnd = section[index].VirtualAddress + sectionSize;
            ULONG chars = 0UL;

            if (sectionEnd < section[index].VirtualAddress) {
                continue;
            }
            if (Rva < section[index].VirtualAddress || Rva >= sectionEnd) {
                continue;
            }
            chars = section[index].Characteristics;
            if ((chars & IMAGE_SCN_MEM_EXECUTE) != 0UL) {
                protection = ((chars & IMAGE_SCN_MEM_WRITE) != 0UL)
                    ? PAGE_EXECUTE_READWRITE
                    : PAGE_EXECUTE_READ;
            } else {
                protection = ((chars & IMAGE_SCN_MEM_WRITE) != 0UL)
                    ? PAGE_READWRITE
                    : PAGE_READONLY;
            }
            break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        /* 解析失败按可执行保守处理——CHANGE 路径会走 MmChangeImageProtection。 */
    }
    return protection;
}

static BOOLEAN
KswRxpfReadRsds(
    _In_ PVOID ImageBase,
    _Out_ GUID* GuidOut,
    _Out_ ULONG* AgeOut
    )
{
    ULONG matchingEntries = 0UL;

    /* Accept exactly one well-formed RSDS record from the loaded image. */
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)ImageBase;
        PIMAGE_NT_HEADERS64 nt = NULL;
        IMAGE_DATA_DIRECTORY directory;
        PIMAGE_DEBUG_DIRECTORY entries = NULL;
        ULONG entryCount = 0UL;
        ULONG index = 0UL;

        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0 ||
            (ULONG)dos->e_lfanew > 0x1000UL) {
            return FALSE;
        }
        nt = (PIMAGE_NT_HEADERS64)((PUCHAR)ImageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_DEBUG) {
            return FALSE;
        }
        directory = nt->OptionalHeader.DataDirectory[
            IMAGE_DIRECTORY_ENTRY_DEBUG];
        if (directory.VirtualAddress == 0UL ||
            directory.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
            directory.VirtualAddress + directory.Size <
                directory.VirtualAddress ||
            directory.VirtualAddress + directory.Size >
                nt->OptionalHeader.SizeOfImage) {
            return FALSE;
        }
        entries = (PIMAGE_DEBUG_DIRECTORY)(
            (PUCHAR)ImageBase + directory.VirtualAddress);
        entryCount = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
        for (index = 0UL; index < entryCount; ++index) {
            KSW_RXPF_RSDS_HEADER rsds;

            if (entries[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
                entries[index].AddressOfRawData == 0UL ||
                entries[index].SizeOfData < sizeof(rsds) ||
                entries[index].AddressOfRawData + sizeof(rsds) <
                    entries[index].AddressOfRawData ||
                entries[index].AddressOfRawData + sizeof(rsds) >
                    nt->OptionalHeader.SizeOfImage) {
                continue;
            }
            RtlCopyMemory(
                &rsds,
                (PUCHAR)ImageBase + entries[index].AddressOfRawData,
                sizeof(rsds));
            if (rsds.Signature != KSW_RXPF_RSDS_SIGNATURE) {
                continue;
            }
            *GuidOut = rsds.Guid;
            *AgeOut = rsds.Age;
            matchingEntries += 1UL;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return matchingEntries == 1UL;
}

static BOOLEAN
KswRxpfSignatureMatches(
    _In_ const KSW_RXPF_BUILD_PROFILE* Profile,
    _In_reads_(Profile->SignatureLength) const UCHAR* Address
    )
{
    ULONG index = 0UL;

    /* Compare every masked byte from the exact PDB/disassembly profile. */
    if (Profile == NULL || Address == NULL ||
        Profile->SignatureLength == 0UL ||
        Profile->SignatureLength > 64UL) {
        return FALSE;
    }
    __try {
        for (index = 0UL; index < Profile->SignatureLength; ++index) {
            if ((Address[index] & Profile->SignatureMask[index]) !=
                (Profile->Signature[index] &
                    Profile->SignatureMask[index])) {
                return FALSE;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return TRUE;
}

static VOID
KswRxpfResolveExactBuild(
    VOID
    )
{
    static const KSW_KERNEL_MODULE_NAME_MATCH matches[] = {
        { "ntoskrnl.exe", 0UL },
        { "ntkrnlmp.exe", 0UL },
        { "ntkrnlpa.exe", 0UL },
        { "ntkrpamp.exe", 0UL }
    };
    KSW_DYN_MODULE_IDENTITY_PACKET identity;
    RTL_OSVERSIONINFOW version;
    GUID pdbGuid;
    ULONG pdbAge = 0UL;
    PVOID functionAddress = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    const KSW_RXPF_BUILD_PROFILE* profile = NULL;
    ULONG profileIndex = 0UL;

    /* Start from an unsupported state and promote only after every check. */
    g_KswRxpfImageProtection.BuildStatus =
        KSWORD_ARK_RXPF_BUILD_STATUS_UNINITIALIZED;
    g_KswRxpfImageProtection.LastStatus = STATUS_NOT_SUPPORTED;
    RtlZeroMemory(&identity, sizeof(identity));
    RtlZeroMemory(&version, sizeof(version));
    RtlZeroMemory(&pdbGuid, sizeof(pdbGuid));
    version.dwOSVersionInfoSize = sizeof(version);
    status = RtlGetVersion(&version);
    if (!NT_SUCCESS(status)) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_OS_BUILD_MISMATCH;
        g_KswRxpfImageProtection.LastStatus = status;
        return;
    }
    g_KswRxpfImageProtection.NtBuildNumber = version.dwBuildNumber;
    for (profileIndex = 0UL;
         profileIndex < RTL_NUMBER_OF(g_KswRxpfBuildProfiles);
         ++profileIndex) {
        if (g_KswRxpfBuildProfiles[profileIndex].NtBuildNumber ==
            version.dwBuildNumber) {
            profile = &g_KswRxpfBuildProfiles[profileIndex];
            break;
        }
    }
    if (profile == NULL) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_OS_BUILD_MISMATCH;
        return;
    }

    /* Resolve the loaded nt image instead of trusting an on-disk path. */
    status = KswordARKQueryKernelModuleIdentity(
        matches,
        RTL_NUMBER_OF(matches),
        &identity);
    if (!NT_SUCCESS(status) || identity.present == 0UL) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH;
        g_KswRxpfImageProtection.LastStatus = status;
        return;
    }
    g_KswRxpfImageProtection.KernelBase =
        (PVOID)(ULONG_PTR)identity.imageBase;
    g_KswRxpfImageProtection.ImageTimeDateStamp = identity.timeDateStamp;
    g_KswRxpfImageProtection.ImageSize = identity.sizeOfImage;
    if (identity.machine != IMAGE_FILE_MACHINE_AMD64 ||
        identity.timeDateStamp != profile->ImageTimeDateStamp ||
        identity.sizeOfImage != profile->ImageSize) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH;
        return;
    }

    /* Read the checksum from the same loaded PE headers. */
    __try {
        PIMAGE_DOS_HEADER dos =
            (PIMAGE_DOS_HEADER)g_KswRxpfImageProtection.KernelBase;
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(
            (PUCHAR)g_KswRxpfImageProtection.KernelBase + dos->e_lfanew);

        g_KswRxpfImageProtection.ImageCheckSum =
            nt->OptionalHeader.CheckSum;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH;
        g_KswRxpfImageProtection.LastStatus = GetExceptionCode();
        return;
    }
    if (g_KswRxpfImageProtection.ImageCheckSum !=
        profile->ImageCheckSum) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_IMAGE_MISMATCH;
        return;
    }

    /* Bind the profile to the exact Microsoft RSDS GUID and age. */
    if (!KswRxpfReadRsds(
            g_KswRxpfImageProtection.KernelBase,
            &pdbGuid,
            &pdbAge) ||
        !RtlEqualMemory(
            &pdbGuid,
            profile->PdbGuid,
            sizeof(pdbGuid)) ||
        pdbAge != profile->PdbAge) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_RSDS_MISMATCH;
        return;
    }
    g_KswRxpfImageProtection.PdbGuid = pdbGuid;
    g_KswRxpfImageProtection.PdbAge = pdbAge;

    /* Require the PDB RVA to land in a live executable PE section. */
    if (!KswRxpfValidateExecutableSection(
            g_KswRxpfImageProtection.KernelBase,
            profile->FunctionRva)) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_SECTION_INVALID;
        return;
    }
    functionAddress =
        (PUCHAR)g_KswRxpfImageProtection.KernelBase +
        profile->FunctionRva;
    if (!KswRxpfSignatureMatches(
            profile,
            (const UCHAR*)functionAddress)) {
        g_KswRxpfImageProtection.BuildStatus =
            KSWORD_ARK_RXPF_BUILD_STATUS_SIGNATURE_MISMATCH;
        return;
    }

    /* Publish the internal call target only after every independent check. */
    g_KswRxpfImageProtection.ActiveProfile = profile;
    g_KswRxpfImageProtection.MmChangeImageProtection =
        (KSW_RXPF_MM_CHANGE_IMAGE_PROTECTION)functionAddress;
    g_KswRxpfImageProtection.BuildStatus =
        KSWORD_ARK_RXPF_BUILD_STATUS_SUPPORTED;
    g_KswRxpfImageProtection.LastStatus = STATUS_SUCCESS;
    InterlockedExchange(&g_KswRxpfImageProtection.BuildSupported, 1);
}

NTSTATUS
KswRxpfImageProtectionInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    /* Record driver identity and resolve but never call the internal routine. */
    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (InterlockedCompareExchange(
            &g_KswRxpfImageProtection.Initialized,
            1,
            1) != 0) {
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(
        &g_KswRxpfImageProtection,
        sizeof(g_KswRxpfImageProtection));
    g_KswRxpfImageProtection.DriverObject = DriverObject;
    KswRxpfResolveExactBuild();
    InterlockedExchange(&g_KswRxpfImageProtection.Initialized, 1);
    return STATUS_SUCCESS;
}

VOID
KswRxpfImageProtectionUninitialize(
    VOID
    )
{
    /* Remove the private function pointer before driver-owned state disappears. */
    InterlockedExchange(&g_KswRxpfImageProtection.BuildSupported, 0);
    InterlockedExchange(&g_KswRxpfImageProtection.Initialized, 0);
    KeMemoryBarrier();
    RtlZeroMemory(
        &g_KswRxpfImageProtection,
        sizeof(g_KswRxpfImageProtection));
}

VOID
KswRxpfImageProtectionQuerySupport(
    _Out_ KSWORD_ARK_RXPF_QUERY_SUPPORT_RESPONSE* Response
    )
{
    ULONG supportFlags = 0UL;
    const KSW_RXPF_BUILD_PROFILE* profile =
        g_KswRxpfImageProtection.ActiveProfile;

    if (profile == NULL) {
        profile = &g_KswRxpfBuildProfiles[0];
    }

    /* Return the exact build profile and signature used by runtime gating. */
    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_RXPF_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    if (InterlockedCompareExchange(
            &g_KswRxpfImageProtection.Initialized,
            1,
            1) != 0) {
        supportFlags |= KSWORD_ARK_RXPF_SUPPORT_INITIALIZED |
            KSWORD_ARK_RXPF_SUPPORT_ALLOCATED_TEST_PAGE |
            KSWORD_ARK_RXPF_SUPPORT_IDT_SHADOW |
            KSWORD_ARK_RXPF_SUPPORT_EMULATOR;
    }
    if (KswRxpfImageProtectionBuildSupported()) {
        supportFlags |= KSWORD_ARK_RXPF_SUPPORT_BUILD_MATCH |
            KSWORD_ARK_RXPF_SUPPORT_ABI_VERIFIED |
            KSWORD_ARK_RXPF_SUPPORT_SELF_IMAGE_TEST_PAGE;
    }
#if KSW_RXPF_ENABLE_EXTERNAL_IMAGE_TARGETS
    supportFlags |= KSWORD_ARK_RXPF_SUPPORT_EXTERNAL_IMAGE_COMPILED;
#endif
    Response->supportFlags = supportFlags;
    Response->buildStatus = g_KswRxpfImageProtection.BuildStatus;
    Response->ntBuildNumber = g_KswRxpfImageProtection.NtBuildNumber;
    Response->imageTimeDateStamp =
        g_KswRxpfImageProtection.ImageTimeDateStamp;
    Response->imageSize = g_KswRxpfImageProtection.ImageSize;
    Response->imageCheckSum = g_KswRxpfImageProtection.ImageCheckSum;
    Response->functionRva = profile->FunctionRva;
    Response->functionOperationRx = profile->OperationRx;
    Response->functionOperationReadOnlyNx =
        profile->OperationReadOnlyNx;
    Response->functionParameterCount = profile->FunctionParameterCount;
    Response->pdbAge = g_KswRxpfImageProtection.PdbAge;
    Response->lastStatus = g_KswRxpfImageProtection.LastStatus;
    RtlCopyMemory(
        Response->pdbGuid,
        &g_KswRxpfImageProtection.PdbGuid,
        sizeof(Response->pdbGuid));
    RtlCopyMemory(
        Response->signature,
        profile->Signature,
        profile->SignatureLength);
    RtlCopyMemory(
        Response->signatureMask,
        profile->SignatureMask,
        profile->SignatureLength);
}

BOOLEAN
KswRxpfImageProtectionBuildSupported(
    VOID
    )
{
    /* The function pointer and exact identity are published as one gate. */
    return InterlockedCompareExchange(
        &g_KswRxpfImageProtection.BuildSupported,
        1,
        1) != 0 &&
        g_KswRxpfImageProtection.MmChangeImageProtection != NULL;
}

PVOID
KswRxpfImageProtectionSelfTestPage(
    VOID
    )
{
    /* The dedicated image section is page-aligned by declaration and linker. */
    return (PVOID)(ULONG_PTR)g_KswRxpfSelfImageTestPage;
}

static NTSTATUS
KswRxpfCreateAllocatedTestPage(
    _In_ ULONG Flags,
    _Out_ KSW_RXPF_PAGE_RECORD* RecordSource
    )
{
    PHYSICAL_ADDRESS lowAddress;
    PHYSICAL_ADDRESS highAddress;
    PHYSICAL_ADDRESS skipBytes;
    PMDL mdl = NULL;
    PVOID writableMapping = NULL;
    PVOID executeMapping = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* Allocate one physical page described by an owned MDL. */
    lowAddress.QuadPart = 0LL;
    highAddress.QuadPart = MAXLONGLONG;
    skipBytes.QuadPart = 0LL;
    mdl = MmAllocatePagesForMdlEx(
        lowAddress,
        highAddress,
        skipBytes,
        PAGE_SIZE,
        MmCached,
        MM_ALLOCATE_FULLY_REQUIRED);
    if (mdl == NULL || MmGetMdlByteCount(mdl) != PAGE_SIZE) {
        if (mdl != NULL) {
            MmFreePagesFromMdl(mdl);
            ExFreePool(mdl);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Seed bytes through a writable/NX mapping before exposing an RX mapping. */
    writableMapping = MmMapLockedPagesSpecifyCache(
        mdl,
        KernelMode,
        MmCached,
        NULL,
        FALSE,
        (MM_PAGE_PRIORITY)(NormalPagePriority | MdlMappingNoExecute));
    if (writableMapping == NULL) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(
        writableMapping,
        g_KswRxpfSelfImageTestPage,
        PAGE_SIZE);
    MmUnmapLockedPages(writableMapping, mdl);

    /* Map the same locked page read/execute for the initial registered state. */
    executeMapping = MmMapLockedPagesSpecifyCache(
        mdl,
        KernelMode,
        MmCached,
        NULL,
        FALSE,
        (MM_PAGE_PRIORITY)(NormalPagePriority | MdlMappingNoWrite));
    if (executeMapping == NULL) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = MmProtectMdlSystemAddress(mdl, PAGE_EXECUTE_READ);
    if (!NT_SUCCESS(status)) {
        MmUnmapLockedPages(executeMapping, mdl);
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return status;
    }
    if (!KswRxpfCanonicalKernelPage(
            (ULONGLONG)(ULONG_PTR)executeMapping)) {
        MmUnmapLockedPages(executeMapping, mdl);
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_CONFLICTING_ADDRESSES;
    }

    /* Fill an unpublished source record for the fixed state table. */
    RtlZeroMemory(RecordSource, sizeof(*RecordSource));
    RecordSource->TargetKind = KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST;
    RecordSource->Flags = Flags;
    RecordSource->PageBase = (LONG64)(ULONG_PTR)executeMapping;
    RecordSource->OriginalMapping = executeMapping;
    RecordSource->OriginalProtection = PAGE_EXECUTE_READ;
    RecordSource->CurrentProtection = PAGE_EXECUTE_READ;
    RecordSource->WritableAliasProtection = 0UL;
    RecordSource->Pfn = (ULONGLONG)MmGetMdlPfnArray(mdl)[0];
    RecordSource->Mdl = mdl;
    RecordSource->OwnsMdlPages = TRUE;
    RecordSource->MappingIsAlias = FALSE;
    RecordSource->LastStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswRxpfCreateSelfImageTestPage(
    _In_ ULONGLONG RequestedAddress,
    _In_ ULONG Flags,
    _Out_ KSW_RXPF_PAGE_RECORD* RecordSource
    )
{
    PVOID page = KswRxpfImageProtectionSelfTestPage();
    PVOID ownerImageBase = NULL;
    PMDL mdl = NULL;
    BOOLEAN pagesLocked = FALSE;

    /* The internal ABI is unavailable on every non-exact Windows build. */
    if (!KswRxpfImageProtectionBuildSupported()) {
        return STATUS_NOT_SUPPORTED;
    }
    if (((ULONGLONG)(ULONG_PTR)page & (PAGE_SIZE - 1ULL)) != 0ULL ||
        (RequestedAddress != 0ULL &&
            RequestedAddress != (ULONGLONG)(ULONG_PTR)page)) {
        return STATUS_INVALID_ADDRESS;
    }
    if (RtlPcToFileHeader(page, &ownerImageBase) == NULL ||
        ownerImageBase == NULL) {
        return STATUS_NOT_FOUND;
    }

    /* Probe-and-lock creates the exact low-bit MDL flag state required by nt. */
    mdl = IoAllocateMdl(page, PAGE_SIZE, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try {
        MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
        pagesLocked = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS exceptionStatus = GetExceptionCode();

        IoFreeMdl(mdl);
        return exceptionStatus;
    }
    if (!pagesLocked ||
        (mdl->MdlFlags &
            (MDL_MAPPED_TO_SYSTEM_VA |
             MDL_PAGES_LOCKED |
             MDL_SOURCE_IS_NONPAGED_POOL)) != MDL_PAGES_LOCKED ||
        MmGetMdlByteOffset(mdl) != 0UL ||
        MmGetMdlByteCount(mdl) != PAGE_SIZE ||
        MmGetMdlVirtualAddress(mdl) != page) {
        if (pagesLocked) {
            MmUnlockPages(mdl);
        }
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }

    /* Fill the record while the original mapping is still RX and untouched. */
    RtlZeroMemory(RecordSource, sizeof(*RecordSource));
    RecordSource->TargetKind = KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST;
    RecordSource->Flags = Flags;
    RecordSource->PageBase = (LONG64)(ULONG_PTR)page;
    RecordSource->OriginalMapping = page;
    RecordSource->OriginalProtection = PAGE_EXECUTE_READ;
    RecordSource->CurrentProtection = PAGE_EXECUTE_READ;
    RecordSource->WritableAliasProtection = 0UL;
    RecordSource->Pfn = (ULONGLONG)MmGetMdlPfnArray(mdl)[0];
    RecordSource->OwnerImageBase =
        (ULONGLONG)(ULONG_PTR)ownerImageBase;
    RecordSource->Mdl = mdl;
    RecordSource->PagesLockedByProbe = TRUE;
    RecordSource->LastStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswRxpfCreateExternalImagePage(
    _In_ ULONGLONG RequestedAddress,
    _In_ ULONG Flags,
    _Out_ KSW_RXPF_PAGE_RECORD* RecordSource
    )
{
    /* 对齐到页边界——调用方传入的可能是页内地址。 */
    PVOID page = (PVOID)(ULONG_PTR)(RequestedAddress & ~(PAGE_SIZE - 1ULL));
    PVOID ownerImageBase = NULL;
    PMDL mdl = NULL;
    BOOLEAN pagesLocked = FALSE;
    BOOLEAN mdlLockedForWrite = FALSE;

    /* 外部映像页需要 MmChangeImageProtection 的精确构建匹配。 */
    if (!KswRxpfImageProtectionBuildSupported()) {
        return STATUS_NOT_SUPPORTED;
    }

    /* 验证地址在规范的内核空间且页对齐。 */
    if (!KswRxpfCanonicalKernelPage(
            RequestedAddress & ~(PAGE_SIZE - 1ULL))) {
        return STATUS_INVALID_ADDRESS;
    }

    /* 确认目标地址落在已加载的内核映像内。 */
    if (RtlPcToFileHeader(page, &ownerImageBase) == NULL ||
        ownerImageBase == NULL) {
        return STATUS_NOT_FOUND;
    }

    /* 创建 MDL 并锁定页面。
     * HVCI 下 IoWriteAccess 对安全映像页（如 CI.dll）会触发不可捕获的
     * 0x1A MEMORY_MANAGEMENT，因此外部映像页始终用 IoReadAccess。 */
    mdl = IoAllocateMdl(page, PAGE_SIZE, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try {
        MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
        pagesLocked = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS exceptionStatus = GetExceptionCode();

        IoFreeMdl(mdl);
        return exceptionStatus;
    }

    /* 验证 MDL 标志——只接受 MDL_PAGES_LOCKED 单一状态。 */
    if (!pagesLocked ||
        (mdl->MdlFlags &
            (MDL_MAPPED_TO_SYSTEM_VA |
             MDL_PAGES_LOCKED |
             MDL_SOURCE_IS_NONPAGED_POOL)) != MDL_PAGES_LOCKED ||
        MmGetMdlByteOffset(mdl) != 0UL ||
        MmGetMdlByteCount(mdl) != PAGE_SIZE ||
        MmGetMdlVirtualAddress(mdl) != page) {
        if (pagesLocked) {
            MmUnlockPages(mdl);
        }
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }

    /* 填充记录：TargetKind 为 EXTERNAL_IMAGE，保护属性按 PE 节特征设置。 */
    {
        ULONG sectionProt = KswRxpfQuerySectionProtection(
            ownerImageBase,
            (ULONG)((ULONG_PTR)page - (ULONG_PTR)ownerImageBase));
        RtlZeroMemory(RecordSource, sizeof(*RecordSource));
        RecordSource->TargetKind = KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE;
        RecordSource->Flags = Flags;
        RecordSource->PageBase = (LONG64)(ULONG_PTR)page;
        RecordSource->OriginalMapping = page;
        RecordSource->OriginalProtection = sectionProt;
        RecordSource->CurrentProtection = sectionProt;
        RecordSource->WritableAliasProtection = 0UL;
    }
    RecordSource->Pfn = (ULONGLONG)MmGetMdlPfnArray(mdl)[0];
    RecordSource->OwnerImageBase =
        (ULONGLONG)(ULONG_PTR)ownerImageBase;
    RecordSource->Mdl = mdl;
    RecordSource->PagesLockedByProbe = TRUE;
    RecordSource->MdlLockedForWrite = mdlLockedForWrite;
    RecordSource->LastStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS
KswRxpfImageProtectionCreateRecord(
    _In_ ULONG TargetKind,
    _In_ ULONGLONG RequestedAddress,
    _In_ ULONG Flags,
    _Out_ KSW_RXPF_PAGE_RECORD* RecordSource
    )
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    /* Only documented test kinds are accepted by the default build. */
    if (RecordSource == NULL ||
        (Flags & ~KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(RecordSource, sizeof(*RecordSource));
    if (TargetKind == KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST) {
        if (RequestedAddress != 0ULL) {
            return STATUS_INVALID_PARAMETER;
        }
        status = KswRxpfCreateAllocatedTestPage(Flags, RecordSource);
    } else if (TargetKind == KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST) {
        status = KswRxpfCreateSelfImageTestPage(
            RequestedAddress,
            Flags,
            RecordSource);
    } else if (TargetKind == KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE) {
#if KSW_RXPF_ENABLE_EXTERNAL_IMAGE_TARGETS
        status = KswRxpfCreateExternalImagePage(
            RequestedAddress,
            Flags,
            RecordSource);
#else
        UNREFERENCED_PARAMETER(RequestedAddress);
        status = STATUS_NOT_SUPPORTED;
#endif
    } else {
        status = STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Keep a complete nonpaged mirror of the page's current bytes. */
    RecordSource->CurrentContent = KswordARKAllocateNonPagedPool(
        PAGE_SIZE,
        KSW_RXPF_CONTENT_TAG);
    if (RecordSource->CurrentContent == NULL) {
        KswRxpfImageProtectionReleaseRecord(RecordSource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Optional backup is allocated on the control path, never by vector 14. */
    if ((Flags & KSWORD_ARK_RXPF_FLAG_CAPTURE_BACKUP) != 0UL) {
        RecordSource->Backup = KswordARKAllocateNonPagedPool(
            PAGE_SIZE,
            KSW_RXPF_BACKUP_TAG);
        if (RecordSource->Backup == NULL) {
            KswRxpfImageProtectionReleaseRecord(RecordSource);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        /*
         * Under HVCI the MDL executable mapping created for AllocatedTest
         * may be execute-only in the SLAT, making reads fault.  The page
         * was seeded from the static test pattern, so use that directly.
         */
        if (TargetKind == KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST) {
            RtlCopyMemory(
                RecordSource->Backup,
                g_KswRxpfSelfImageTestPage,
                PAGE_SIZE);
        }
    }
    __try {
        /* Same HVCI/SLAT concern: allocated test page may be execute-only. */
        if (TargetKind == KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST) {
            RtlCopyMemory(
                RecordSource->CurrentContent,
                g_KswRxpfSelfImageTestPage,
                PAGE_SIZE);
        } else if (TargetKind == KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE) {
            /* 临时只读别名绑定已锁定 PFN，不依赖可被卸载的原始映像 VA。 */
            PVOID readAlias = MmMapLockedPagesSpecifyCache(
                RecordSource->Mdl,
                KernelMode,
                MmCached,
                NULL,
                FALSE,
                (MM_PAGE_PRIORITY)(NormalPagePriority |
                    MdlMappingNoWrite | MdlMappingNoExecute));
            if (readAlias == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                __try {
                    RtlCopyMemory(RecordSource->CurrentContent, readAlias, PAGE_SIZE);
                }
                __finally {
                    /* 私有映像保护调用前恢复 MDL 的未映射状态。 */
                    MmUnmapLockedPages(readAlias, RecordSource->Mdl);
                }
            }
        } else {
            RtlCopyMemory(
                RecordSource->CurrentContent,
                RecordSource->OriginalMapping,
                PAGE_SIZE);
        }
        /* 备份与当前镜像来自同一次读取，且所有原始 VA 读取均受异常保护。 */
        if (NT_SUCCESS(status) && RecordSource->Backup != NULL) {
            RtlCopyMemory(RecordSource->Backup, RecordSource->CurrentContent, PAGE_SIZE);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        KswRxpfImageProtectionReleaseRecord(RecordSource);
        return status;
    }
    if (!NT_SUCCESS(status)) {
        KswRxpfImageProtectionReleaseRecord(RecordSource);
        return status;
    }
    RecordSource->LastWriteOffset = 0UL;
    RecordSource->LastWriteLength = 0UL;
    RtlZeroMemory(RecordSource->LastWriteBytes, sizeof(RecordSource->LastWriteBytes));
    return STATUS_SUCCESS;
}

/* MmChangeImageProtection is resolved by signature scan—not in the CFG
   bitmap—so the indirect call trips guard_icall_bugcheck under HVCI/VBS. */
__declspec(guard(nocf))
NTSTATUS
KswRxpfImageProtectionChangeToRwNx(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    )
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    /* A record transitions exactly once from RX to its persistent NX state. */
    if (Record == NULL || Record->Mdl == NULL ||
        Record->State != KSWORD_ARK_RXPF_PAGE_STATE_RX) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Record->TargetKind == KSWORD_ARK_RXPF_TARGET_ALLOCATED_TEST) {
        /* The public MDL API changes the existing mapping to PAGE_READWRITE/NX. */
        status = MmProtectMdlSystemAddress(Record->Mdl, PAGE_READWRITE);
        if (NT_SUCCESS(status)) {
            Record->WritableAlias =
                (ULONGLONG)(ULONG_PTR)Record->OriginalMapping;
            Record->MappingIsAlias = FALSE;
            Record->CurrentProtection = PAGE_READWRITE;
            Record->WritableAliasProtection = PAGE_READWRITE;
        }
    } else if (Record->TargetKind ==
        KSWORD_ARK_RXPF_TARGET_SELF_IMAGE_TEST ||
        Record->TargetKind ==
        KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE) {
        PVOID writableAlias = NULL;
        /* PE 节级别：PAGE_EXECUTE_* 表示可执行，需先移除 X 再建可写别名。 */
        BOOLEAN sectionIsExecutable =
            (Record->OriginalProtection & 0xF0UL) != 0UL;

        /* Revalidate the exact MDL low-bit contract immediately before the call. */
        if ((Record->Mdl->MdlFlags &
                (MDL_MAPPED_TO_SYSTEM_VA |
                 MDL_PAGES_LOCKED |
                 MDL_SOURCE_IS_NONPAGED_POOL)) != MDL_PAGES_LOCKED ||
            MmGetMdlByteOffset(Record->Mdl) != 0UL ||
            MmGetMdlByteCount(Record->Mdl) != PAGE_SIZE ||
            MmGetMdlVirtualAddress(Record->Mdl) !=
                (PVOID)(ULONG_PTR)Record->PageBase) {
            return STATUS_INVALID_PARAMETER;
        }
        if (sectionIsExecutable) {
            /*
             * 可执行节：必须先用 MmChangeImageProtection 移除执行位，
             * 否则可写别名会违反 HVCI 的 W^X 不变式。
             */
            if (!KswRxpfImageProtectionBuildSupported()) {
                return STATUS_NOT_SUPPORTED;
            }
            status = g_KswRxpfImageProtection.MmChangeImageProtection(
                Record->Mdl,
                (PVOID)(ULONG_PTR)Record->PageBase,
                PAGE_SIZE,
                KSW_RXPF_MM_CHANGE_OPERATION_READONLY_NX);
            if (!NT_SUCCESS(status)) {
                Record->LastStatus = status;
                return status;
            }
            Record->CurrentProtection = PAGE_READONLY;
            Record->ChangeProtectionApplied = TRUE;
        } else if (Record->MdlLockedForWrite) {
            /*
             * 可写数据节且 MDL 已用 IoWriteAccess 锁定：
             * 无需 MmChangeImageProtection——PFN 层已允许写入，
             * 直接建 MmMapLockedPagesSpecifyCache 别名即可写。
             */
            status = STATUS_SUCCESS;
        } else {
            /*
             * 非可执行节且 IoWriteAccess 被拒（HVCI 可能收紧了 SLAT）：
             * 尝试 MmChangeImageProtection 作为后备，记录返回值以便诊断。
             */
            if (KswRxpfImageProtectionBuildSupported()) {
                NTSTATUS slatStatus =
                    g_KswRxpfImageProtection.MmChangeImageProtection(
                        Record->Mdl,
                        (PVOID)(ULONG_PTR)Record->PageBase,
                        PAGE_SIZE,
                        KSW_RXPF_MM_CHANGE_OPERATION_READONLY_NX);
                if (NT_SUCCESS(slatStatus)) {
                    Record->CurrentProtection = PAGE_READONLY;
                } else {
                    Record->LastStatus = slatStatus;
                }
            }
            status = STATUS_SUCCESS;
        }

        /* Map a separate writable/NX alias. */
        writableAlias = MmMapLockedPagesSpecifyCache(
            Record->Mdl,
            KernelMode,
            MmCached,
            NULL,
            FALSE,
            (MM_PAGE_PRIORITY)(NormalPagePriority | MdlMappingNoExecute));
        if (writableAlias == NULL) {
            Record->State = KSWORD_ARK_RXPF_PAGE_STATE_ERROR;
            Record->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Record->WritableAlias = (ULONGLONG)(ULONG_PTR)writableAlias;
        Record->MappingIsAlias = TRUE;

        /* 可执行节：SLAT 写保护已释放，升级别名 PTE 为可写。
         * 非可执行节不升级——PTE 可写但 SLAT 只读会把可捕获的
         * 0xC0000005 升级为不可捕获的 0x1A BugCheck。 */
        if (Record->ChangeProtectionApplied) {
            NTSTATUS protStatus = MmProtectMdlSystemAddress(
                Record->Mdl,
                PAGE_READWRITE);
            Record->WritableAliasProtection =
                NT_SUCCESS(protStatus)
                ? PAGE_READWRITE
                : PAGE_READONLY;
            if (!NT_SUCCESS(protStatus)) {
                Record->LastStatus = protStatus;
                /* 不发布虚假的 RW_NX；原始失败交给调用方处理及清理。 */
                status = protStatus;
            }
        } else {
            Record->WritableAliasProtection =
                Record->MdlLockedForWrite
                ? PAGE_READWRITE
                : PAGE_READONLY;
        }
    } else {
        return STATUS_NOT_SUPPORTED;
    }

    /* Publish the completed transition only after a writable alias exists. */
    if (NT_SUCCESS(status)) {
        KeMemoryBarrier();
        InterlockedExchange(
            &Record->State,
            KSWORD_ARK_RXPF_PAGE_STATE_RW_NX);
        InterlockedIncrement((volatile LONG*)&Record->Generation);
    } else {
        Record->LastStatus = status;
        Record->LastFailureReason =
            KSWORD_ARK_RXPF_EMULATION_INTERNAL_ERROR;
    }
    return status;
}

NTSTATUS
KswRxpfImageProtectionWrite(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) const UCHAR* Bytes,
    _In_ ULONG Length
    )
{
    ULONGLONG end = (ULONGLONG)Offset + (ULONGLONG)Length;
    NTSTATUS status = STATUS_SUCCESS;
    /* bit24 = MdlLockedForWrite，bit25 = ChangeProtectionApplied（SLAT 已释放）。 */
    ULONG mdlBit = 0U;

    if (Record != NULL) {
        Record->LastFailureReason = 0x1000U;
    }

    if (Record == NULL || Bytes == NULL || Length == 0UL ||
        Record->CurrentContent == NULL ||
        Length > KSWORD_ARK_RXPF_MAX_WRITE_BYTES ||
        end > PAGE_SIZE || end < Offset ||
        Record->State != KSWORD_ARK_RXPF_PAGE_STATE_RW_NX ||
        Record->WritableAlias == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    mdlBit = (Record->MdlLockedForWrite ? 0x1000000U : 0U)
           | (Record->ChangeProtectionApplied ? 0x2000000U : 0U);

    /* 诊断模式：仅探测别名是否可读，不做任何写入。 */
    Record->LastFailureReason = 0x6000U | mdlBit;
    __try {
        RtlCopyMemory(
            (PUCHAR)(ULONG_PTR)Record->WritableAlias + Offset,
            Bytes,
            Length);
        Record->LastFailureReason = 0x4001U | mdlBit;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        Record->LastFailureReason = 0xA002U | mdlBit;
    }

    if (NT_SUCCESS(status)) {
        Record->LastFailureReason |= 0x10000U;
        __try {
            RtlCopyMemory(
                (PUCHAR)Record->CurrentContent + Offset,
                Bytes,
                Length);
            Record->LastWriteOffset = Offset;
            Record->LastWriteLength = Length;
            RtlZeroMemory(
                Record->LastWriteBytes,
                sizeof(Record->LastWriteBytes));
            RtlCopyMemory(Record->LastWriteBytes, Bytes, Length);
            KeMemoryBarrier();
            Record->LastFailureReason |= 0x20000U;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            Record->LastFailureReason = 0xA003U | mdlBit;
        }
    }
    Record->LastFailureReason |= 0x40000U;
    Record->LastStatus = status;
    if (NT_SUCCESS(status)) {
        InterlockedIncrement((volatile LONG*)&Record->Generation);
    }
    return status;
}

/*
 * 为 mode=1 (恢复 RX) 准备 VTL1 期望的参考摘要缓冲区。
 *
 * VTL1 的 SkciValidateDynamicCodePages 对每个目标页：
 *   1. SHA-512(整页 4096 字节) → 64 字节
 *   2. 取 [0x20,0x40) 的 32 字节作为"尾段"
 *   3. memcmp(尾段, 参考缓冲区 + pageIndex*0x20, 0x20)
 *
 * 自映像场景：将摘要写入驱动 .data 节的静态缓冲区，
 * 该地址在本映像范围内，通过 nt 的 DataTableEntry 范围校验。
 *
 * 返回 STATUS_SUCCESS 时 *RefVA/*RefLen 可直接传入 MmChangeImageProtection。
 */
static NTSTATUS
KswRxpfPrepareReferenceDigest(
    _In_ PKSW_RXPF_PAGE_RECORD Record,
    _Out_ PVOID* RefVA,
    _Out_ ULONG* RefLen
    )
{
    UCHAR fullHash[64];

    *RefVA = NULL;
    *RefLen = 0;

    if (Record->CurrentContent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* 外部映像暂不支持：参考缓冲区必须在目标模块范围内，
     * 而驱动 .data 节不在外部模块的 [DllBase, DllBase+SizeOfImage) 内。 */
    if (Record->TargetKind == KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE) {
        return STATUS_NOT_SUPPORTED;
    }

    /* 计算当前页内容的 SHA-512。 */
    KswRxpfSha512(Record->CurrentContent, PAGE_SIZE, fullHash);

    /* 提取 [0x20, 0x40)——VTL1 比对的尾段。 */
    RtlCopyMemory(g_KswRxpfReferenceDigest, fullHash + 0x20, 0x20);

    /* 清除栈上的完整哈希。 */
    RtlSecureZeroMemory(fullHash, sizeof(fullHash));

    *RefVA = (PVOID)g_KswRxpfReferenceDigest;
    *RefLen = 0x20;
    return STATUS_SUCCESS;
}

/* MmChangeImageProtection 通过签名扫描解析——不在 CFG 位图中。 */
__declspec(guard(nocf))
NTSTATUS
KswRxpfRestoreExecutableProtection(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    )
{
    PVOID refVA = NULL;
    ULONG refLen = 0;
    NTSTATUS status;

    /* ChangeProtectionApplied 为 TRUE 表示 SLAT 保护已释放，
     * 释放记录前必须恢复为 RX，否则代码页丧失执行权。 */
    if (Record == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Record->ChangeProtectionApplied) {
        return STATUS_SUCCESS;
    }
    if (Record->Mdl == NULL || !KswRxpfImageProtectionBuildSupported()) {
        Record->LastStatus = STATUS_INVALID_DEVICE_STATE;
        return Record->LastStatus;
    }

    /*
     * mode=1 优先沿用现有参考缓冲区；外部映像目前使用整页参考回退。
     * 该调用的成功或失败不单独证明 VTL1 内部采用何种摘要算法。
     *
     * 已实测：在 HVCI 来宾上 mode=1 对测试驱动的 .rxpftst 段统一返回
     *   STATUS_ACCESS_VIOLATION，即使页面内容未改动。
     *   记忆见 [[ksword-rxpf-is-half-useless-without-shadow-idt]]。
     */
    status = KswRxpfPrepareReferenceDigest(Record, &refVA, &refLen);
    if (NT_SUCCESS(status)) {
        status = g_KswRxpfImageProtection.MmChangeImageProtection(
            Record->Mdl,
            refVA,
            refLen,
            KSW_RXPF_MM_CHANGE_OPERATION_RX);
    } else {
        status = g_KswRxpfImageProtection.MmChangeImageProtection(
            Record->Mdl,
            (PVOID)(ULONG_PTR)Record->PageBase,
            PAGE_SIZE,
            KSW_RXPF_MM_CHANGE_OPERATION_RX);
    }
    Record->LastStatus = status;
    if (NT_SUCCESS(status)) {
        /* 仅在实际成功后撤销恢复责任，失败保留重试所需的 MDL 和状态。 */
        Record->ChangeProtectionApplied = FALSE;
        Record->CurrentProtection = Record->OriginalProtection;
    }
    return status;
}

NTSTATUS
KswRxpfImageProtectionReleaseRecord(
    _Inout_ PKSW_RXPF_PAGE_RECORD Record
    )
{
    /* status：任一恢复步骤失败时停止释放，保留资源所有权。 */
    NTSTATUS status = STATUS_SUCCESS;
    /* retryAlias：先前 mode=1 失败且本次请求改为恢复备份时的临时别名。 */
    PVOID retryAlias = NULL;
    /* Release only resources whose ownership flags were recorded at creation. */
    if (Record == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* mode=1 失败会要求先解映射别名。若重试时调用方要求恢复原字节，
     * 必须从仍锁定的 MDL 重建别名，不能把第二次默认注销误当作已恢复备份。 */
    if (!Record->SkipBackupRestoreOnRelease &&
        Record->ChangeProtectionApplied &&
        Record->Backup != NULL &&
        Record->Mdl != NULL &&
        !Record->MappingIsAlias) {
        retryAlias = MmMapLockedPagesSpecifyCache(
            Record->Mdl,
            KernelMode,
            MmCached,
            NULL,
            FALSE,
            (MM_PAGE_PRIORITY)(NormalPagePriority | MdlMappingNoExecute));
        if (retryAlias == NULL) {
            Record->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
            return Record->LastStatus;
        }
        status = MmProtectMdlSystemAddress(Record->Mdl, PAGE_READWRITE);
        if (!NT_SUCCESS(status)) {
            MmUnmapLockedPages(retryAlias, Record->Mdl);
            Record->LastStatus = status;
            return status;
        }
        Record->WritableAlias = (ULONGLONG)(ULONG_PTR)retryAlias;
        Record->MappingIsAlias = TRUE;
        Record->WritableAliasProtection = PAGE_READWRITE;
    }

    /*
     * Phase F：在释放前自动把 Backup 还原到物理页。
     * mode=1 不负责把物理页内容还原为原字节；若 Change 后曾写入过修改字节，
     * 不做 restore 就 Unregister 必然导致 PTE 回 RX 后内核执行到被改字节 → bugcheck
     * (SYSTEM_SERVICE_EXCEPTION, What failed: <module>)。
     *
     * 需要在 alias 还 RW 且 mapped 时执行，所以放在 Protect(RO)/Unmap 之前。
     * 同步刷新 CurrentContent，使控制路径的内容镜像与实际恢复字节保持一致。
     */
    if (!Record->SkipBackupRestoreOnRelease &&
        Record->MappingIsAlias && Record->WritableAlias != 0ULL &&
        Record->Mdl != NULL && Record->Backup != NULL &&
        Record->WritableAliasProtection == PAGE_READWRITE &&
        Record->CurrentContent != NULL) {
        __try {
            /* 整页拷回原字节：物理页被 writeback 到和 catalog 一致的状态。 */
            RtlCopyMemory(
                (PVOID)(ULONG_PTR)Record->WritableAlias,
                Record->Backup,
                PAGE_SIZE);
            /* 同步 CurrentContent，避免恢复后的参考内容仍指向修改字节。 */
            RtlCopyMemory(
                Record->CurrentContent,
                Record->Backup,
                PAGE_SIZE);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            /* restore 失败是致命：页内容仍是修改后的字节，接下去的 mode=1 若放行
             * 会让内核执行到坏指令。记录 LastFailureReason 以便 R3 诊断。 */
            Record->LastFailureReason = 0xBEEF0001U;
            Record->LastStatus = GetExceptionCode();
            return Record->LastStatus;
        }
    }

    /*
     * MmChangeImageProtection 入口校验要求 (MdlFlags & 7) == MDL_PAGES_LOCKED (0x2)，
     * 即 MDL_MAPPED_TO_SYSTEM_VA (0x1) 不得置位。MmMapLockedPagesSpecifyCache
     * 建的可写别名会把该位置为 1，若在 mode=1 之前不先解映射，
     * MmChangeImageProtection 立即返回 STATUS_INVALID_PARAMETER (0xC000000D)。
     * 顺序：BackupRestore → PROTECT(READONLY) 降权 → UNMAP 别名 → mode=1 恢复 RX → UNLOCK/FREE MDL。
     */
    if (Record->MappingIsAlias && Record->WritableAlias != 0ULL &&
        Record->Mdl != NULL) {
        /* Change 时若升过 RW，释放前先降回 RO 以匹配 PTE 清理约束。 */
        if (Record->WritableAliasProtection == PAGE_READWRITE) {
            status = MmProtectMdlSystemAddress(
                Record->Mdl,
                PAGE_READONLY);
            if (!NT_SUCCESS(status)) {
                Record->LastStatus = status;
                return status;
            }
            Record->WritableAliasProtection = PAGE_READONLY;
        }
        MmUnmapLockedPages(
            (PVOID)(ULONG_PTR)Record->WritableAlias,
            Record->Mdl);
        Record->WritableAlias = 0ULL;
        Record->MappingIsAlias = FALSE;
    }

    /* 恢复可执行节 SLAT 权限——必须在解锁之前，但现在在解映射之后。 */
    status = KswRxpfRestoreExecutableProtection(Record);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (Record->OwnsMdlPages &&
        Record->OriginalMapping != NULL && Record->Mdl != NULL) {
        MmUnmapLockedPages(Record->OriginalMapping, Record->Mdl);
        Record->OriginalMapping = NULL;
        Record->WritableAlias = 0ULL;
    }
    if (Record->PagesLockedByProbe && Record->Mdl != NULL) {
        MmUnlockPages(Record->Mdl);
        Record->PagesLockedByProbe = FALSE;
    }
    if (Record->OwnsMdlPages && Record->Mdl != NULL) {
        MmFreePagesFromMdl(Record->Mdl);
        ExFreePool(Record->Mdl);
        Record->Mdl = NULL;
    } else if (Record->Mdl != NULL) {
        IoFreeMdl(Record->Mdl);
        Record->Mdl = NULL;
    }
    if (Record->Backup != NULL) {
        ExFreePoolWithTag(Record->Backup, KSW_RXPF_BACKUP_TAG);
        Record->Backup = NULL;
    }
    if (Record->CurrentContent != NULL) {
        ExFreePoolWithTag(Record->CurrentContent, KSW_RXPF_CONTENT_TAG);
        Record->CurrentContent = NULL;
    }
    Record->LastStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}
