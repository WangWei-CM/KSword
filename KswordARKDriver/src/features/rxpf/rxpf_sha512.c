/*++

Module Name:

    rxpf_sha512.c

Abstract:

    FIPS 180-4 SHA-512 implementation for RXPF reference digest computation.
    VTL1 的 SkciValidateDynamicCodePages 使用 SymCrypt SHA-512（算法选择器
    0x800E）计算每个目标页的哈希，然后将 [0x20,0x40) 字节段与调用方提供的
    参考记录做 memcmp。本模块提供兼容的 SHA-512 计算能力。

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

#include "rxpf_sha512.h"

/* ─── FIPS 180-4 SHA-512 round constants（前 80 个素数立方根的小数部分）─── */
static const ULONGLONG g_KswRxpfSha512K[80] = {
    0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL,
    0xB5C0FBCFEC4D3B2FULL, 0xE9B5DBA58189DBBCULL,
    0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL,
    0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL,
    0xD807AA98A3030242ULL, 0x12835B0145706FBEULL,
    0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
    0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL,
    0x9BDC06A725C71235ULL, 0xC19BF174CF692694ULL,
    0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL,
    0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL,
    0x2DE92C6F592B0275ULL, 0x4A7484AA6EA6E483ULL,
    0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
    0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL,
    0xB00327C898FB213FULL, 0xBF597FC7BEEF0EE4ULL,
    0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL,
    0x06CA6351E003826FULL, 0x142929670A0E6E70ULL,
    0x27B70A8546D22FFCULL, 0x2E1B21385C26C926ULL,
    0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
    0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL,
    0x81C2C92E47EDAEE6ULL, 0x92722C851482353BULL,
    0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL,
    0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL,
    0xD192E819D6EF5218ULL, 0xD69906245565A910ULL,
    0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
    0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL,
    0x2748774CDF8EEB99ULL, 0x34B0BCB5E19B48A8ULL,
    0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL,
    0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL,
    0x748F82EE5DEFB2FCULL, 0x78A5636F43172F60ULL,
    0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
    0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL,
    0xBEF9A3F7B2C67915ULL, 0xC67178F2E372532BULL,
    0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL,
    0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL,
    0x06F067AA72176FBAULL, 0x0A637DC5A2C898A6ULL,
    0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
    0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL,
    0x3C9EBE0A15C9BEBCULL, 0x431D67C49C100D4CULL,
    0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL,
    0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL
};

/* ─── 辅助操作 ─── */

/* 从大端字节序缓冲区载入 64 位字。 */
static __forceinline ULONGLONG
KswRxpfSha512Load64BE(
    _In_reads_bytes_(8) const UCHAR* P
    )
{
    return ((ULONGLONG)P[0] << 56) | ((ULONGLONG)P[1] << 48) |
           ((ULONGLONG)P[2] << 40) | ((ULONGLONG)P[3] << 32) |
           ((ULONGLONG)P[4] << 24) | ((ULONGLONG)P[5] << 16) |
           ((ULONGLONG)P[6] << 8)  | ((ULONGLONG)P[7]);
}

/* 将 64 位字写为大端字节序。 */
static __forceinline VOID
KswRxpfSha512Store64BE(
    _Out_writes_bytes_(8) UCHAR* P,
    _In_ ULONGLONG V
    )
{
    P[0] = (UCHAR)(V >> 56);
    P[1] = (UCHAR)(V >> 48);
    P[2] = (UCHAR)(V >> 40);
    P[3] = (UCHAR)(V >> 32);
    P[4] = (UCHAR)(V >> 24);
    P[5] = (UCHAR)(V >> 16);
    P[6] = (UCHAR)(V >> 8);
    P[7] = (UCHAR)(V);
}

/* 64 位循环右移。 */
static __forceinline ULONGLONG
KswRxpfRotr64(
    _In_ ULONGLONG Value,
    _In_ ULONG Count
    )
{
    return (Value >> Count) | (Value << (64U - Count));
}

/* ─── SHA-512 块压缩 ─── */

/* 压缩一个 1024 位（128 字节）块到八字状态。 */
static VOID
KswRxpfSha512Transform(
    _Inout_updates_(8) ULONGLONG State[8],
    _In_reads_bytes_(128) const UCHAR* Block
    )
{
    ULONGLONG w[80];
    ULONGLONG a, b, c, d, e, f, g, h;
    ULONG i;

    /* 展开消息调度：前 16 字直接从块中载入大端字。 */
    for (i = 0; i < 16; i++) {
        w[i] = KswRxpfSha512Load64BE(Block + i * 8);
    }

    /* 扩展到 80 字：σ1(W[i-2]) + W[i-7] + σ0(W[i-15]) + W[i-16]。 */
    for (i = 16; i < 80; i++) {
        ULONGLONG s0 = KswRxpfRotr64(w[i - 15], 1) ^
                        KswRxpfRotr64(w[i - 15], 8) ^
                        (w[i - 15] >> 7);
        ULONGLONG s1 = KswRxpfRotr64(w[i - 2], 19) ^
                        KswRxpfRotr64(w[i - 2], 61) ^
                        (w[i - 2] >> 6);
        w[i] = s1 + w[i - 7] + s0 + w[i - 16];
    }

    /* 初始化工作变量。 */
    a = State[0]; b = State[1]; c = State[2]; d = State[3];
    e = State[4]; f = State[5]; g = State[6]; h = State[7];

    /* 80 轮压缩。 */
    for (i = 0; i < 80; i++) {
        ULONGLONG bigSigma1 = KswRxpfRotr64(e, 14) ^
                               KswRxpfRotr64(e, 18) ^
                               KswRxpfRotr64(e, 41);
        ULONGLONG ch = (e & f) ^ (~e & g);
        ULONGLONG t1 = h + bigSigma1 + ch + g_KswRxpfSha512K[i] + w[i];
        ULONGLONG bigSigma0 = KswRxpfRotr64(a, 28) ^
                               KswRxpfRotr64(a, 34) ^
                               KswRxpfRotr64(a, 39);
        ULONGLONG maj = (a & b) ^ (a & c) ^ (b & c);
        ULONGLONG t2 = bigSigma0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    /* 累加到状态。 */
    State[0] += a; State[1] += b; State[2] += c; State[3] += d;
    State[4] += e; State[5] += f; State[6] += g; State[7] += h;
}

/* ─── 公开接口 ─── */

VOID
KswRxpfSha512Init(
    _Out_ KSW_RXPF_SHA512_CONTEXT* Context
    )
{
    /* FIPS 180-4 §5.3.5 初始哈希值——前 8 个素数平方根的小数部分。 */
    Context->State[0] = 0x6A09E667F3BCC908ULL;
    Context->State[1] = 0xBB67AE8584CAA73BULL;
    Context->State[2] = 0x3C6EF372FE94F82BULL;
    Context->State[3] = 0xA54FF53A5F1D36F1ULL;
    Context->State[4] = 0x510E527FADE682D1ULL;
    Context->State[5] = 0x9B05688C2B3E6C1FULL;
    Context->State[6] = 0x1F83D9ABFB41BD6BULL;
    Context->State[7] = 0x5BE0CD19137E2179ULL;
    Context->TotalBytes = 0ULL;
    Context->BlockBytes = 0UL;
    RtlZeroMemory(Context->Block, sizeof(Context->Block));
}

VOID
KswRxpfSha512Update(
    _Inout_ KSW_RXPF_SHA512_CONTEXT* Context,
    _In_reads_bytes_(Length) const VOID* Data,
    _In_ ULONG Length
    )
{
    const UCHAR* src = (const UCHAR*)Data;
    ULONG remaining = Length;

    /* 填满当前不完整块后压缩。 */
    if (Context->BlockBytes > 0 && Context->BlockBytes + remaining >= 128) {
        ULONG fill = 128 - Context->BlockBytes;
        RtlCopyMemory(Context->Block + Context->BlockBytes, src, fill);
        KswRxpfSha512Transform(Context->State, Context->Block);
        Context->BlockBytes = 0;
        src += fill;
        remaining -= fill;
    }

    /* 对齐的完整块直接就地压缩。 */
    while (remaining >= 128) {
        KswRxpfSha512Transform(Context->State, src);
        src += 128;
        remaining -= 128;
    }

    /* 剩余尾部暂存到块缓冲区。 */
    if (remaining > 0) {
        RtlCopyMemory(Context->Block + Context->BlockBytes, src, remaining);
        Context->BlockBytes += remaining;
    }
    Context->TotalBytes += (ULONGLONG)Length;
}

VOID
KswRxpfSha512Final(
    _Inout_ KSW_RXPF_SHA512_CONTEXT* Context,
    _Out_writes_bytes_(64) UCHAR Digest[64]
    )
{
    ULONGLONG bitLen;
    ULONG i;

    /*
     * FIPS 180-4 §5.1.2 填充：
     *   消息 || 1 || 0...0 || 128 位大端长度
     * 块必须对齐到 128 字节，长度字段占最后 16 字节，
     * 因此 0x80 和填充零必须在倒数第 17 字节之前完成。
     */
    bitLen = Context->TotalBytes * 8ULL;

    /* 追加 0x80 终止位。 */
    Context->Block[Context->BlockBytes++] = 0x80U;

    /* 如果剩余空间不足 16 字节放长度字段，先压缩当前块再开新块。 */
    if (Context->BlockBytes > 112) {
        RtlZeroMemory(
            Context->Block + Context->BlockBytes,
            128 - Context->BlockBytes);
        KswRxpfSha512Transform(Context->State, Context->Block);
        Context->BlockBytes = 0;
    }

    /* 填零到偏移 112。 */
    RtlZeroMemory(
        Context->Block + Context->BlockBytes,
        112 - Context->BlockBytes);

    /* 写入 128 位消息长度（大端）。高 64 位为零——输入 ≤ 4 GiB。 */
    RtlZeroMemory(Context->Block + 112, 8);
    KswRxpfSha512Store64BE(Context->Block + 120, bitLen);

    /* 压缩最终填充块。 */
    KswRxpfSha512Transform(Context->State, Context->Block);

    /* 输出 64 字节摘要（大端）。 */
    for (i = 0; i < 8; i++) {
        KswRxpfSha512Store64BE(Digest + i * 8, Context->State[i]);
    }

    /* 安全清除上下文。 */
    RtlSecureZeroMemory(Context, sizeof(*Context));
}

VOID
KswRxpfSha512(
    _In_reads_bytes_(Length) const VOID* Data,
    _In_ ULONG Length,
    _Out_writes_bytes_(64) UCHAR Digest[64]
    )
{
    KSW_RXPF_SHA512_CONTEXT ctx;
    KswRxpfSha512Init(&ctx);
    KswRxpfSha512Update(&ctx, Data, Length);
    KswRxpfSha512Final(&ctx, Digest);
}
