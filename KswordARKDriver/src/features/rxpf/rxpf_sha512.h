/*++

Module Name:

    rxpf_sha512.h

Abstract:

    FIPS 180-4 SHA-512 hash for RXPF reference digest computation.
    VTL1 使用 SHA-512 的 [0x20,0x40) 字节段验证动态代码页。

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

#pragma once

#include <ntddk.h>

EXTERN_C_START

/* SHA-512 块尺寸 128 字节，状态 8 个 64 位字。 */
typedef struct _KSW_RXPF_SHA512_CONTEXT
{
    ULONGLONG State[8];        /* 中间哈希值 H0..H7。 */
    ULONGLONG TotalBytes;      /* 已处理的消息字节总数。 */
    UCHAR Block[128];          /* 未满块的缓冲区。 */
    ULONG BlockBytes;          /* Block 中已填充的字节数。 */
} KSW_RXPF_SHA512_CONTEXT;

/* 初始化上下文——设置 FIPS 180-4 初始哈希值。 */
VOID
KswRxpfSha512Init(
    _Out_ KSW_RXPF_SHA512_CONTEXT* Context
    );

/* 追加 Length 字节到哈希计算。可多次调用。 */
VOID
KswRxpfSha512Update(
    _Inout_ KSW_RXPF_SHA512_CONTEXT* Context,
    _In_reads_bytes_(Length) const VOID* Data,
    _In_ ULONG Length
    );

/* 写出 64 字节最终摘要并清除上下文。 */
VOID
KswRxpfSha512Final(
    _Inout_ KSW_RXPF_SHA512_CONTEXT* Context,
    _Out_writes_bytes_(64) UCHAR Digest[64]
    );

/* 一次性计算——适用于单缓冲区场景。 */
VOID
KswRxpfSha512(
    _In_reads_bytes_(Length) const VOID* Data,
    _In_ ULONG Length,
    _Out_writes_bytes_(64) UCHAR Digest[64]
    );

EXTERN_C_END
