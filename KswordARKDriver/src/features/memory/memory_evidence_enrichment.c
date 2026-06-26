/*++

Module Name:

    memory_evidence_enrichment.c

Abstract:

    Small read-only enrichment helpers for kernel memory evidence rows.

Environment:

    Kernel-mode Driver Framework

--*/

#include "memory_evidence_enrichment.h"

static BOOLEAN
KswordARKMemoryEvidenceBytesLookImageLike(
    _In_reads_bytes_(SampleSize) const UCHAR* Sample,
    _In_ ULONG SampleSize
    )
/*++

Routine Description:

    判断当前有限字节是否像 PE 映像开头。中文说明：该判断只使用调用方已经
    复制到本地的少量字节，识别 MZ 以及样本内可验证的 PE\0\0 签名。

Arguments:

    Sample - 本地样本字节。
    SampleSize - 本地样本长度。

Return Value:

    TRUE 表示样本具备 MZ 或 PE-like 头部特征；FALSE 表示未观察到。

--*/
{
    ULONG peOffset = 0UL;

    if (Sample == NULL || SampleSize < 2UL) {
        return FALSE;
    }
    if (Sample[0] != 'M' || Sample[1] != 'Z') {
        return FALSE;
    }
    if (SampleSize < 0x40UL) {
        return TRUE;
    }

    peOffset =
        (ULONG)Sample[0x3CUL] |
        ((ULONG)Sample[0x3DUL] << 8) |
        ((ULONG)Sample[0x3EUL] << 16) |
        ((ULONG)Sample[0x3FUL] << 24);
    if (peOffset <= SampleSize - 4UL &&
        Sample[peOffset] == 'P' &&
        Sample[peOffset + 1UL] == 'E' &&
        Sample[peOffset + 2UL] == 0U &&
        Sample[peOffset + 3UL] == 0U) {
        return TRUE;
    }
    return TRUE;
}

static BOOLEAN
KswordARKMemoryEvidenceAddressLooksImageLike(
    _In_ ULONG64 VirtualAddress
    )
/*++

Routine Description:

    从目标地址只读复制一个固定短前缀并判断是否 image-like。中文说明：本函数
    不写入协议 sample 字段，读取失败只返回 FALSE，不影响扫描主结果。

Arguments:

    VirtualAddress - 待读取的内核虚拟地址。

Return Value:

    TRUE 表示短前缀像 PE 映像；FALSE 表示不可读或未观察到该特征。

--*/
{
    UCHAR localSample[KSWORD_ARK_MEMORY_EVIDENCE_SECTION_SAMPLE_BYTES];
    MM_COPY_ADDRESS copyAddress;
    SIZE_T copiedBytes = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (VirtualAddress == 0ULL) {
        return FALSE;
    }

    RtlZeroMemory(localSample, sizeof(localSample));
    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.VirtualAddress = (PVOID)(ULONG_PTR)VirtualAddress;
    __try {
        status = MmCopyMemory(
            localSample,
            copyAddress,
            sizeof(localSample),
            MM_COPY_MEMORY_VIRTUAL,
            &copiedBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        copiedBytes = 0U;
    }

    if (!NT_SUCCESS(status) || copiedBytes < 2U) {
        RtlZeroMemory(localSample, sizeof(localSample));
        return FALSE;
    }
    if (copiedBytes > sizeof(localSample)) {
        copiedBytes = sizeof(localSample);
    }

    status = KswordARKMemoryEvidenceBytesLookImageLike(localSample, (ULONG)copiedBytes) ?
        STATUS_SUCCESS :
        STATUS_NOT_FOUND;
    RtlZeroMemory(localSample, sizeof(localSample));
    return NT_SUCCESS(status);
}

VOID
KswordARKMemoryEvidenceApplyImageLikeHint(
    _Inout_ KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW* Row
    )
/*++

Routine Description:

    根据已读取样本标记 image-like memory。中文说明：这是只读启发式，默认只
    提升 private/non-module/BigPool 行的 risk，不尝试解析或转储完整 PE，也
    不访问磁盘文件。

Arguments:

    Row - 待更新的 evidence row。

Return Value:

    None. 函数只写入 Row。

--*/
{
    if (Row == NULL) {
        return;
    }
    if ((Row->sampleSize != 0UL &&
            KswordARKMemoryEvidenceBytesLookImageLike(Row->sample, Row->sampleSize)) ||
        (Row->sampleSize == 0UL &&
            KswordARKMemoryEvidenceAddressLooksImageLike(Row->virtualAddress))) {
        Row->rowFlags |= KSWORD_ARK_MEMORY_EVIDENCE_ROW_FLAG_IMAGE_LIKE_MEMORY;
        if (Row->backingKind != KSWORD_ARK_MEMORY_EVIDENCE_BACKING_LOADED_MODULE) {
            Row->riskFlags |= KSWORD_ARK_MEMORY_EVIDENCE_RISK_IMAGE_LIKE_MEMORY;
        }
        if (Row->confidence < KSWORD_ARK_MEMORY_EVIDENCE_CONFIDENCE_MEDIUM) {
            Row->confidence = KSWORD_ARK_MEMORY_EVIDENCE_CONFIDENCE_MEDIUM;
        }
    }
}
