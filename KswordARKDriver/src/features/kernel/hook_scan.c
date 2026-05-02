/*++

Module Name:

    hook_scan.c

Abstract:

    Kernel inline hook and IAT/EAT diagnostic helpers for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"

#include <ntimage.h>
#include <ntstrsafe.h>

#define KSW_HOOK_SCAN_TAG 'hHsK'
#define KSW_HOOK_SCAN_SYSTEM_MODULE_INFORMATION_CLASS 11UL

#ifndef STATUS_REQUEST_NOT_ACCEPTED
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

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

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

static const ULONG g_KswordArkInlineHookResponseHeaderSize =
    (ULONG)(sizeof(KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY));

static const ULONG g_KswordArkIatEatHookResponseHeaderSize =
    (ULONG)(sizeof(KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY));

static CHAR
KswordARKHookAsciiLower(
    _In_ CHAR Character
    )
/*++

Routine Description:

    将一个 ASCII 字符转成小写。中文说明：只用于模块名比较，不处理区域化字符。

Arguments:

    Character - 输入字符。

Return Value:

    小写字符或原字符。

--*/
{
    if (Character >= 'A' && Character <= 'Z') {
        return (CHAR)(Character + ('a' - 'A'));
    }
    return Character;
}

static BOOLEAN
KswordARKHookBoundedAnsiEqualsInsensitive(
    _In_reads_bytes_(LeftBytes) const UCHAR* LeftText,
    _In_ ULONG LeftBytes,
    _In_z_ PCSTR RightText
    )
/*++

Routine Description:

    比较有限长 ANSI 字符串和常量字符串。中文说明：SystemModuleInformation 的
    FullPathName 是定长数组，不能假定文件名区域一定 NUL 结束。

Arguments:

    LeftText - 有限长文本。
    LeftBytes - 有限长文本最大字节数。
    RightText - NUL 结束常量文本。

Return Value:

    TRUE 表示大小写不敏感相等。

--*/
{
    ULONG index = 0UL;

    if (LeftText == NULL || LeftBytes == 0UL || RightText == NULL) {
        return FALSE;
    }

    for (index = 0UL; index < LeftBytes; ++index) {
        CHAR leftChar = (CHAR)LeftText[index];
        CHAR rightChar = RightText[index];

        if (rightChar == '\0') {
            return leftChar == '\0';
        }
        if (leftChar == '\0') {
            return FALSE;
        }
        if (KswordARKHookAsciiLower(leftChar) != KswordARKHookAsciiLower(rightChar)) {
            return FALSE;
        }
    }

    return RightText[index] == '\0';
}

static BOOLEAN
KswordARKHookWideModuleFilterMatches(
    _In_reads_bytes_(FileNameBytes) const UCHAR* FileNameText,
    _In_ ULONG FileNameBytes,
    _In_reads_(FilterChars) const WCHAR* FilterText,
    _In_ ULONG FilterChars
    )
/*++

Routine Description:

    检查模块过滤器是否匹配当前模块名。中文说明：R3 传入 WCHAR，内核模块列表
    是 ANSI，这里只做 ASCII 文件名匹配。

Arguments:

    FileNameText - 模块文件名 ANSI。
    FileNameBytes - 模块文件名最大字节数。
    FilterText - UI 过滤文本。
    FilterChars - 过滤文本字符数。

Return Value:

    TRUE 表示匹配或过滤为空。

--*/
{
    ULONG index = 0UL;
    ULONG filterLength = 0UL;
    ULONG startIndex = 0UL;

    if (FilterText == NULL || FilterChars == 0UL || FilterText[0] == L'\0') {
        return TRUE;
    }
    if (FileNameText == NULL || FileNameBytes == 0UL) {
        return FALSE;
    }

    // 中文说明：R3 过滤框经常只输入 ntoskrnl 或 win32k，不强制完整文件名相等。
    while (filterLength < FilterChars && FilterText[filterLength] != L'\0') {
        ++filterLength;
    }
    if (filterLength == 0UL) {
        return TRUE;
    }
    if (filterLength > FileNameBytes) {
        return FALSE;
    }

    // 中文说明：在有限长 ANSI 文件名里做大小写不敏感子串匹配，避免依赖 NUL 结束。
    for (startIndex = 0UL; startIndex + filterLength <= FileNameBytes; ++startIndex) {
        BOOLEAN matched = TRUE;

        for (index = 0UL; index < filterLength; ++index) {
            WCHAR filterChar = FilterText[index];
            CHAR fileChar = (CHAR)FileNameText[startIndex + index];

            if (fileChar == '\0') {
                matched = FALSE;
                break;
            }
            if (filterChar >= L'A' && filterChar <= L'Z') {
                filterChar = (WCHAR)(filterChar + (L'a' - L'A'));
            }
            fileChar = KswordARKHookAsciiLower(fileChar);
            if ((WCHAR)fileChar != filterChar) {
                matched = FALSE;
                break;
            }
        }

        if (matched) {
            return TRUE;
        }
    }

    return FALSE;
}

static VOID
KswordARKHookCopyAnsi(
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes,
    _In_opt_z_ const CHAR* Source
    )
/*++

Routine Description:

    复制 ANSI 文本到固定缓冲。中文说明：所有协议字符串都强制 NUL 结尾。

Arguments:

    Destination - 目标缓冲。
    DestinationBytes - 目标字节数。
    Source - 来源文本。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (Destination == NULL || DestinationBytes == 0U) {
        return;
    }

    Destination[0] = '\0';
    if (Source == NULL) {
        return;
    }

    (VOID)RtlStringCbCopyNA(Destination, DestinationBytes, Source, DestinationBytes - 1U);
    Destination[DestinationBytes - 1U] = '\0';
}

static VOID
KswordARKHookCopyBoundedAnsiToWide(
    _In_reads_bytes_(SourceBytes) const UCHAR* SourceText,
    _In_ ULONG SourceBytes,
    _Out_writes_(DestinationChars) PWCHAR DestinationText,
    _In_ ULONG DestinationChars
    )
/*++

Routine Description:

    复制有限长 ANSI 模块名到 WCHAR 协议字段。中文说明：只用于模块文件名展示。

Arguments:

    SourceText - 来源 ANSI 文本。
    SourceBytes - 来源最大字节数。
    DestinationText - 目标 WCHAR 缓冲。
    DestinationChars - 目标字符容量。

Return Value:

    None. 本函数没有返回值。

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

static BOOLEAN
KswordARKHookValidateRvaRange(
    _In_ ULONG Rva,
    _In_ ULONG Bytes,
    _In_ ULONG ImageSize
    )
/*++

Routine Description:

    校验 PE RVA 是否位于已加载映像范围内。中文说明：解析导入/导出表前必须防止
    恶意或异常驱动头导致越界。

Arguments:

    Rva - 起始 RVA。
    Bytes - 需要读取的字节数。
    ImageSize - 映像大小。

Return Value:

    TRUE 表示范围有效。

--*/
{
    if (Rva >= ImageSize || Bytes > ImageSize) {
        return FALSE;
    }
    return Rva <= (ImageSize - Bytes);
}

static NTSTATUS
KswordARKHookBuildModuleSnapshot(
    _Outptr_result_bytebuffer_(*BufferBytesOut) KSW_HOOK_SYSTEM_MODULE_INFORMATION** ModuleInfoOut,
    _Out_ ULONG* BufferBytesOut
    )
/*++

Routine Description:

    查询已加载内核模块快照。中文说明：Inline、IAT、EAT 检测都依赖该快照解析
    地址归属和模块边界。

Arguments:

    ModuleInfoOut - 返回分配的模块快照。
    BufferBytesOut - 返回快照字节数。

Return Value:

    STATUS_SUCCESS 或查询/分配错误。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;

    if (ModuleInfoOut == NULL || BufferBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ModuleInfoOut = NULL;
    *BufferBytesOut = 0UL;

    status = ZwQuerySystemInformation(
        KSW_HOOK_SCAN_SYSTEM_MODULE_INFORMATION_CLASS,
        NULL,
        0UL,
        &requiredBytes);
    if (requiredBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    moduleInfo = (KSW_HOOK_SYSTEM_MODULE_INFORMATION*)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        requiredBytes,
        KSW_HOOK_SCAN_TAG);
#pragma warning(pop)
    if (moduleInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySystemInformation(
        KSW_HOOK_SCAN_SYSTEM_MODULE_INFORMATION_CLASS,
        moduleInfo,
        requiredBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
        return status;
    }

    *ModuleInfoOut = moduleInfo;
    *BufferBytesOut = requiredBytes;
    return STATUS_SUCCESS;
}

static const KSW_HOOK_SYSTEM_MODULE_ENTRY*
KswordARKHookFindModuleForAddress(
    _In_opt_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ ULONG_PTR Address
    )
/*++

Routine Description:

    根据地址查找所属内核模块。中文说明：用于判断跳转目标是否跳出当前模块。

Arguments:

    ModuleInfo - 模块快照。
    Address - 待分类地址。

Return Value:

    命中返回模块条目；否则返回 NULL。

--*/
{
    ULONG moduleIndex = 0UL;

    if (ModuleInfo == NULL || Address == 0U) {
        return NULL;
    }

    for (moduleIndex = 0UL; moduleIndex < ModuleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_HOOK_SYSTEM_MODULE_ENTRY* moduleEntry = &ModuleInfo->Modules[moduleIndex];
        const ULONG_PTR imageBase = (ULONG_PTR)moduleEntry->ImageBase;
        const ULONG_PTR imageEnd = imageBase + (ULONG_PTR)moduleEntry->ImageSize;

        if (Address >= imageBase && Address < imageEnd) {
            return moduleEntry;
        }
    }

    return NULL;
}

static VOID
KswordARKHookGetModuleFileName(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Outptr_result_buffer_(*FileNameBytesOut) const UCHAR** FileNameOut,
    _Out_ ULONG* FileNameBytesOut
    )
/*++

Routine Description:

    取得模块文件名部分。中文说明：SystemModuleInformation 保存完整路径和文件名
    偏移，本函数统一处理越界偏移。

Arguments:

    ModuleEntry - 模块条目。
    FileNameOut - 返回文件名指针。
    FileNameBytesOut - 返回剩余字节数。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (FileNameOut == NULL || FileNameBytesOut == NULL) {
        return;
    }
    *FileNameOut = NULL;
    *FileNameBytesOut = 0UL;
    if (ModuleEntry == NULL) {
        return;
    }

    if (ModuleEntry->OffsetToFileName < sizeof(ModuleEntry->FullPathName)) {
        *FileNameOut = ModuleEntry->FullPathName + ModuleEntry->OffsetToFileName;
        *FileNameBytesOut = (ULONG)(sizeof(ModuleEntry->FullPathName) - ModuleEntry->OffsetToFileName);
    }
    else {
        *FileNameOut = ModuleEntry->FullPathName;
        *FileNameBytesOut = (ULONG)sizeof(ModuleEntry->FullPathName);
    }
}

static BOOLEAN
KswordARKHookReadMemorySafe(
    _In_ const VOID* Source,
    _Out_writes_bytes_(BytesToRead) VOID* Destination,
    _In_ SIZE_T BytesToRead
    )
/*++

Routine Description:

    安全读取内核内存。中文说明：扫描内核任意模块时不能假设每个 PE 目录都可信，
    因此读取失败只让当前行失败，不影响整个枚举。

Arguments:

    Source - 来源地址。
    Destination - 目标缓冲。
    BytesToRead - 读取长度。

Return Value:

    TRUE 表示读取成功。

--*/
{
    if (Source == NULL || Destination == NULL || BytesToRead == 0U) {
        return FALSE;
    }

    __try {
        RtlCopyMemory(Destination, Source, BytesToRead);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }

    return TRUE;
}

static ULONG
KswordARKHookClassifyInlineBytes(
    _In_ ULONG_PTR FunctionAddress,
    _In_reads_(ByteCount) const UCHAR* Bytes,
    _In_ ULONG ByteCount,
    _Out_ ULONG_PTR* TargetAddressOut
    )
/*++

Routine Description:

    识别常见 x64/x86 Inline Hook 指令形态。中文说明：这里只做保守解析，不做
    完整反汇编；命中 JMP/MOV+JMP/RET/INT3 等明显补丁时返回类型和目标地址。

Arguments:

    FunctionAddress - 当前函数地址。
    Bytes - 函数开头字节。
    ByteCount - 可用字节数。
    TargetAddressOut - 返回跳转目标，无法解析时为 0。

Return Value:

    KSWORD_ARK_INLINE_HOOK_TYPE_*。

--*/
{
    ULONG_PTR targetAddress = 0U;
    ULONG hookType = KSWORD_ARK_INLINE_HOOK_TYPE_NONE;

    if (TargetAddressOut != NULL) {
        *TargetAddressOut = 0U;
    }
    if (Bytes == NULL || ByteCount == 0UL) {
        return KSWORD_ARK_INLINE_HOOK_TYPE_NONE;
    }

    if (ByteCount >= 5UL && Bytes[0] == 0xE9U) {
        LONG rel32 = 0;
        RtlCopyMemory(&rel32, Bytes + 1, sizeof(rel32));
        targetAddress = FunctionAddress + 5U + (LONG_PTR)rel32;
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL32;
    }
    else if (ByteCount >= 2UL && Bytes[0] == 0xEBU) {
        CHAR rel8 = (CHAR)Bytes[1];
        targetAddress = FunctionAddress + 2U + (LONG_PTR)rel8;
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL8;
    }
#if defined(_M_AMD64)
    else if (ByteCount >= 14UL &&
        Bytes[0] == 0xFFU &&
        Bytes[1] == 0x25U) {
        LONG rel32 = 0;
        ULONG_PTR pointerAddress = 0U;
        RtlCopyMemory(&rel32, Bytes + 2, sizeof(rel32));
        pointerAddress = FunctionAddress + 6U + (LONG_PTR)rel32;
        (VOID)KswordARKHookReadMemorySafe((const VOID*)pointerAddress, &targetAddress, sizeof(targetAddress));
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_JMP_RIP_INDIRECT;
    }
    else if (ByteCount >= 12UL &&
        Bytes[0] == 0x48U &&
        Bytes[1] == 0xB8U &&
        Bytes[10] == 0xFFU &&
        Bytes[11] == 0xE0U) {
        RtlCopyMemory(&targetAddress, Bytes + 2, sizeof(targetAddress));
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_MOV_RAX_JMP_RAX;
    }
    else if (ByteCount >= 13UL &&
        Bytes[0] == 0x49U &&
        Bytes[1] == 0xBBU &&
        Bytes[10] == 0x41U &&
        Bytes[11] == 0xFFU &&
        Bytes[12] == 0xE3U) {
        RtlCopyMemory(&targetAddress, Bytes + 2, sizeof(targetAddress));
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_MOV_R11_JMP_R11;
    }
#endif
    else if (Bytes[0] == 0xC3U || Bytes[0] == 0xC2U) {
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_RET_PATCH;
    }
    else if (Bytes[0] == 0xCCU) {
        hookType = KSWORD_ARK_INLINE_HOOK_TYPE_INT3_PATCH;
    }

    if (TargetAddressOut != NULL) {
        *TargetAddressOut = targetAddress;
    }
    return hookType;
}

static NTSTATUS
KswordARKHookWriteKernelMemoryUnsafe(
    _In_ PVOID Destination,
    _In_reads_bytes_(BytesToWrite) const VOID* Source,
    _In_ SIZE_T BytesToWrite
    )
/*++

Routine Description:

    写入内核代码页。中文说明：该函数只在 UI 明确 force 后调用；使用 MDL 建立
    可写系统映射，避免直接改 CR0.WP，写完立即释放映射。

Arguments:

    Destination - 目标内核地址。
    Source - 来源字节。
    BytesToWrite - 写入长度。

Return Value:

    STATUS_SUCCESS 或 MDL/异常状态。

--*/
{
    PMDL mdl = NULL;
    PVOID mappedAddress = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN pagesLocked = FALSE;

    if (Destination == NULL || Source == NULL || BytesToWrite == 0U || BytesToWrite > KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        mdl = IoAllocateMdl(Destination, (ULONG)BytesToWrite, FALSE, FALSE, NULL);
        if (mdl == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        // 中文说明：这里是写代码页路径，必须用 IoModifyAccess 并记录锁页状态，
        // 避免 Probe 抛异常后在 Exit 分支错误解锁未锁定的 MDL。
        MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);
        pagesLocked = TRUE;
        mappedAddress = MmMapLockedPagesSpecifyCache(
            mdl,
            KernelMode,
            MmNonCached,
            NULL,
            FALSE,
            NormalPagePriority);
        if (mappedAddress == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        status = MmProtectMdlSystemAddress(mdl, PAGE_READWRITE);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
        RtlCopyMemory(mappedAddress, Source, BytesToWrite);
        // 中文说明：写入后放置内存屏障，保证本 CPU 上的补丁字节提交顺序明确；
        // 指令缓存一致性由 x64 内核代码页映射和后续执行路径承担，这里不调用未声明例程。
        KeMemoryBarrier();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

Exit:
    if (mappedAddress != NULL) {
        MmUnmapLockedPages(mappedAddress, mdl);
        mappedAddress = NULL;
    }
    if (mdl != NULL && pagesLocked) {
        MmUnlockPages(mdl);
        pagesLocked = FALSE;
    }
    if (mdl != NULL) {
        IoFreeMdl(mdl);
        mdl = NULL;
    }
    return status;
}

static VOID
KswordARKHookFillInlineEntry(
    _In_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_z_ const CHAR* FunctionName,
    _In_ PVOID FunctionAddress,
    _In_reads_(KSWORD_ARK_KERNEL_HOOK_BYTES) const UCHAR* ExpectedBytes,
    _In_reads_(KSWORD_ARK_KERNEL_HOOK_BYTES) const UCHAR* CurrentBytes,
    _In_ ULONG HookType,
    _In_ ULONG_PTR TargetAddress,
    _Inout_ KSWORD_ARK_INLINE_HOOK_ENTRY* Entry
    )
/*++

Routine Description:

    填充 Inline Hook 响应行。中文说明：该函数只整理诊断字段，不做写入。

Arguments:

    ModuleInfo - 模块快照。
    ModuleEntry - 当前模块。
    FunctionName - 函数名。
    FunctionAddress - 函数地址。
    ExpectedBytes - 当前导出地址处的基准字节。
    CurrentBytes - 当前读取字节。
    HookType - 识别到的 Hook 类型。
    TargetAddress - 跳转目标。
    Entry - 输出行。

Return Value:

    None. 本函数没有返回值。

--*/
{
    const UCHAR* moduleFileName = NULL;
    ULONG moduleFileNameBytes = 0UL;
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* targetModule = NULL;

    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->hookType = HookType;
    Entry->functionAddress = (ULONGLONG)(ULONG_PTR)FunctionAddress;
    Entry->targetAddress = (ULONGLONG)TargetAddress;
    Entry->moduleBase = (ULONGLONG)(ULONG_PTR)ModuleEntry->ImageBase;
    Entry->originalByteCount = KSWORD_ARK_KERNEL_HOOK_BYTES;
    Entry->currentByteCount = KSWORD_ARK_KERNEL_HOOK_BYTES;
    Entry->flags = 0UL;
    RtlCopyMemory(Entry->expectedBytes, ExpectedBytes, KSWORD_ARK_KERNEL_HOOK_BYTES);
    RtlCopyMemory(Entry->currentBytes, CurrentBytes, KSWORD_ARK_KERNEL_HOOK_BYTES);
    KswordARKHookCopyAnsi(Entry->functionName, sizeof(Entry->functionName), FunctionName);

    KswordARKHookGetModuleFileName(ModuleEntry, &moduleFileName, &moduleFileNameBytes);
    KswordARKHookCopyBoundedAnsiToWide(
        moduleFileName,
        moduleFileNameBytes,
        Entry->moduleName,
        KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);

    targetModule = KswordARKHookFindModuleForAddress(ModuleInfo, TargetAddress);
    if (targetModule != NULL) {
        const UCHAR* targetFileName = NULL;
        ULONG targetFileNameBytes = 0UL;

        Entry->targetModuleBase = (ULONGLONG)(ULONG_PTR)targetModule->ImageBase;
        KswordARKHookGetModuleFileName(targetModule, &targetFileName, &targetFileNameBytes);
        KswordARKHookCopyBoundedAnsiToWide(
            targetFileName,
            targetFileNameBytes,
            Entry->targetModuleName,
            KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
    }

    if (HookType == KSWORD_ARK_INLINE_HOOK_TYPE_NONE) {
        Entry->status = KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
    }
    else if (targetModule == ModuleEntry || targetModule == NULL) {
        Entry->status = KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH;
    }
    else {
        Entry->status = KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS;
    }
}

NTSTATUS
KswordARKDriverScanInlineHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    扫描内核模块导出函数开头的常见 Inline Hook。中文说明：以加载映像自身导出
    地址为扫描源，识别明显跳转/补丁并分类为 clean/internal/suspicious。

Arguments:

    OutputBuffer - 响应缓冲。
    OutputBufferLength - 响应缓冲长度。
    Request - 可选扫描请求。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 或查询/解析失败。

--*/
{
    KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG entryCapacity = 0UL;
    ULONG moduleIndex = 0UL;
    ULONG requestFlags = 0UL;
    ULONG maxEntries = KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < g_KswordArkInlineHookResponseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Request != NULL) {
        requestFlags = Request->flags;
        if (Request->maxEntries != 0UL) {
            maxEntries = Request->maxEntries;
        }
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KERNEL_HOOK_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY);
    response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN;
    entryCapacity = (ULONG)((OutputBufferLength - g_KswordArkInlineHookResponseHeaderSize) / sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY));
    if (entryCapacity > maxEntries) {
        entryCapacity = maxEntries;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED;
        *BytesWrittenOut = g_KswordArkInlineHookResponseHeaderSize;
        return STATUS_SUCCESS;
    }
    response->moduleCount = moduleInfo->NumberOfModules;

    for (moduleIndex = 0UL; moduleIndex < moduleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_HOOK_SYSTEM_MODULE_ENTRY* moduleEntry = &moduleInfo->Modules[moduleIndex];
        const UCHAR* moduleFileName = NULL;
        ULONG moduleFileNameBytes = 0UL;
        PIMAGE_DOS_HEADER dosHeader = NULL;
        PIMAGE_NT_HEADERS ntHeaders = NULL;
        const IMAGE_DATA_DIRECTORY* exportDirectory = NULL;
        PIMAGE_EXPORT_DIRECTORY exportHeader = NULL;
        PULONG nameRvaArray = NULL;
        PUSHORT nameOrdinalArray = NULL;
        PULONG functionRvaArray = NULL;
        ULONG exportNameIndex = 0UL;

        KswordARKHookGetModuleFileName(moduleEntry, &moduleFileName, &moduleFileNameBytes);
        if ((requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_MODULE_FILTER) != 0UL &&
            Request != NULL &&
            !KswordARKHookWideModuleFilterMatches(
                moduleFileName,
                moduleFileNameBytes,
                Request->moduleName,
                KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS)) {
            continue;
        }

        __try {
            dosHeader = (PIMAGE_DOS_HEADER)moduleEntry->ImageBase;
            if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
                continue;
            }
            ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)moduleEntry->ImageBase + (ULONG)dosHeader->e_lfanew);
            if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
                continue;
            }
            exportDirectory = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (exportDirectory->VirtualAddress == 0UL ||
                !KswordARKHookValidateRvaRange(exportDirectory->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), moduleEntry->ImageSize)) {
                continue;
            }
            exportHeader = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)moduleEntry->ImageBase + exportDirectory->VirtualAddress);
            if (exportHeader->AddressOfNames == 0UL ||
                exportHeader->AddressOfNameOrdinals == 0UL ||
                exportHeader->AddressOfFunctions == 0UL ||
                !KswordARKHookValidateRvaRange(exportHeader->AddressOfNames, exportHeader->NumberOfNames * sizeof(ULONG), moduleEntry->ImageSize) ||
                !KswordARKHookValidateRvaRange(exportHeader->AddressOfNameOrdinals, exportHeader->NumberOfNames * sizeof(USHORT), moduleEntry->ImageSize) ||
                !KswordARKHookValidateRvaRange(exportHeader->AddressOfFunctions, exportHeader->NumberOfFunctions * sizeof(ULONG), moduleEntry->ImageSize)) {
                continue;
            }

            nameRvaArray = (PULONG)((PUCHAR)moduleEntry->ImageBase + exportHeader->AddressOfNames);
            nameOrdinalArray = (PUSHORT)((PUCHAR)moduleEntry->ImageBase + exportHeader->AddressOfNameOrdinals);
            functionRvaArray = (PULONG)((PUCHAR)moduleEntry->ImageBase + exportHeader->AddressOfFunctions);

            for (exportNameIndex = 0UL; exportNameIndex < exportHeader->NumberOfNames; ++exportNameIndex) {
                const ULONG nameRva = nameRvaArray[exportNameIndex];
                const USHORT ordinalIndex = nameOrdinalArray[exportNameIndex];
                const CHAR* exportName = NULL;
                ULONG functionRva = 0UL;
                PVOID functionAddress = NULL;
                UCHAR currentBytes[KSWORD_ARK_KERNEL_HOOK_BYTES] = { 0 };
                UCHAR expectedBytes[KSWORD_ARK_KERNEL_HOOK_BYTES] = { 0 };
                ULONG_PTR targetAddress = 0U;
                ULONG hookType = KSWORD_ARK_INLINE_HOOK_TYPE_NONE;
                BOOLEAN readOk = FALSE;
                KSWORD_ARK_INLINE_HOOK_ENTRY tempEntry;

                if (ordinalIndex >= exportHeader->NumberOfFunctions ||
                    !KswordARKHookValidateRvaRange(nameRva, 2UL, moduleEntry->ImageSize)) {
                    continue;
                }
                exportName = (const CHAR*)((PUCHAR)moduleEntry->ImageBase + nameRva);
                functionRva = functionRvaArray[ordinalIndex];
                if (functionRva >= exportDirectory->VirtualAddress &&
                    functionRva < exportDirectory->VirtualAddress + exportDirectory->Size) {
                    continue;
                }
                if (!KswordARKHookValidateRvaRange(functionRva, KSWORD_ARK_KERNEL_HOOK_BYTES, moduleEntry->ImageSize)) {
                    continue;
                }

                functionAddress = (PUCHAR)moduleEntry->ImageBase + functionRva;
                readOk = KswordARKHookReadMemorySafe(functionAddress, currentBytes, sizeof(currentBytes));
                if (!readOk) {
                    continue;
                }
                RtlCopyMemory(expectedBytes, functionAddress, sizeof(expectedBytes));
                hookType = KswordARKHookClassifyInlineBytes(
                    (ULONG_PTR)functionAddress,
                    currentBytes,
                    KSWORD_ARK_KERNEL_HOOK_BYTES,
                    &targetAddress);

                RtlZeroMemory(&tempEntry, sizeof(tempEntry));
                KswordARKHookFillInlineEntry(
                    moduleInfo,
                    moduleEntry,
                    exportName,
                    functionAddress,
                    expectedBytes,
                    currentBytes,
                    hookType,
                    targetAddress,
                    &tempEntry);

                if (tempEntry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN &&
                    (requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN) == 0UL) {
                    continue;
                }
                if (tempEntry.status == KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH &&
                    (requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL) == 0UL) {
                    continue;
                }

                response->totalCount += 1UL;
                if (response->returnedCount >= entryCapacity) {
                    continue;
                }
                RtlCopyMemory(
                    &response->entries[response->returnedCount],
                    &tempEntry,
                    sizeof(tempEntry));
                response->returnedCount += 1UL;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            response->lastStatus = GetExceptionCode();
        }
    }

    response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
    *BytesWrittenOut = g_KswordArkInlineHookResponseHeaderSize +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY));
    ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverPatchInlineHook(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    修复 Inline Hook 指令补丁。中文说明：普通请求只返回 force-required；force 后
    才会比较 expectedCurrentBytes 并写入 restoreBytes 或 NOP 补丁。

Arguments:

    OutputBuffer - 固定响应缓冲。
    OutputBufferLength - 响应长度。
    Request - 修复请求。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 表示响应有效。

--*/
{
    KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE* response = NULL;
    UCHAR currentBytes[KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES] = { 0 };
    UCHAR patchBytes[KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES] = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    ULONG patchBytesCount = 0UL;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->functionAddress == 0ULL ||
        Request->patchBytes == 0UL ||
        Request->patchBytes > KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KERNEL_HOOK_PROTOCOL_VERSION;
    response->functionAddress = Request->functionAddress;
    response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN;
    response->lastStatus = STATUS_SUCCESS;
    patchBytesCount = Request->patchBytes;

    if ((Request->flags & KSWORD_ARK_KERNEL_PATCH_FLAG_FORCE) == 0UL) {
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED;
        response->lastStatus = STATUS_REQUEST_NOT_ACCEPTED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    if (!KswordARKHookReadMemorySafe(
        (const VOID*)(ULONG_PTR)Request->functionAddress,
        currentBytes,
        patchBytesCount)) {
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED;
        response->lastStatus = STATUS_ACCESS_VIOLATION;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    RtlCopyMemory(response->beforeBytes, currentBytes, patchBytesCount);

    if (RtlCompareMemory(currentBytes, Request->expectedCurrentBytes, patchBytesCount) != patchBytesCount) {
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED;
        response->lastStatus = STATUS_REVISION_MISMATCH;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    if (Request->mode == KSWORD_ARK_INLINE_PATCH_MODE_RESTORE_BYTES) {
        RtlCopyMemory(patchBytes, Request->restoreBytes, patchBytesCount);
    }
    else if (Request->mode == KSWORD_ARK_INLINE_PATCH_MODE_NOP_BRANCH) {
        RtlFillMemory(patchBytes, patchBytesCount, 0x90U);
    }
    else {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKHookWriteKernelMemoryUnsafe(
        (PVOID)(ULONG_PTR)Request->functionAddress,
        patchBytes,
        patchBytesCount);
    response->lastStatus = status;
    if (NT_SUCCESS(status)) {
        response->bytesPatched = patchBytesCount;
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED;
        (VOID)KswordARKHookReadMemorySafe(
            (const VOID*)(ULONG_PTR)Request->functionAddress,
            response->afterBytes,
            patchBytesCount);
    }
    else {
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED;
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnumerateIatEatHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    枚举内核模块 IAT/EAT 可疑指针。中文说明：IAT 检测导入 thunk 当前目标是否
    落在声明导入模块内；EAT 检测导出 RVA 是否落在自身映像内或是否为转发导出。

Arguments:

    OutputBuffer - 响应缓冲。
    OutputBufferLength - 响应缓冲长度。
    Request - 扫描请求。
    BytesWrittenOut - 返回写入字节数。

Return Value:

    STATUS_SUCCESS 或查询状态。

--*/
{
    KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    ULONG entryCapacity = 0UL;
    ULONG moduleIndex = 0UL;
    ULONG requestFlags = KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < g_KswordArkIatEatHookResponseHeaderSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request != NULL && Request->flags != 0UL) {
        requestFlags = Request->flags;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KERNEL_HOOK_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY);
    response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_UNKNOWN;
    entryCapacity = (ULONG)((OutputBufferLength - g_KswordArkIatEatHookResponseHeaderSize) / sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY));

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED;
        *BytesWrittenOut = g_KswordArkIatEatHookResponseHeaderSize;
        return STATUS_SUCCESS;
    }
    response->moduleCount = moduleInfo->NumberOfModules;

    for (moduleIndex = 0UL; moduleIndex < moduleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_HOOK_SYSTEM_MODULE_ENTRY* moduleEntry = &moduleInfo->Modules[moduleIndex];
        const UCHAR* moduleFileName = NULL;
        ULONG moduleFileNameBytes = 0UL;
        PIMAGE_DOS_HEADER dosHeader = NULL;
        PIMAGE_NT_HEADERS ntHeaders = NULL;

        KswordARKHookGetModuleFileName(moduleEntry, &moduleFileName, &moduleFileNameBytes);
        if ((requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_MODULE_FILTER) != 0UL &&
            Request != NULL &&
            !KswordARKHookWideModuleFilterMatches(
                moduleFileName,
                moduleFileNameBytes,
                Request->moduleName,
                KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS)) {
            continue;
        }

        __try {
            dosHeader = (PIMAGE_DOS_HEADER)moduleEntry->ImageBase;
            if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
                continue;
            }
            ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)moduleEntry->ImageBase + (ULONG)dosHeader->e_lfanew);
            if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
                continue;
            }

            if ((requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS) != 0UL) {
                const IMAGE_DATA_DIRECTORY* exportDirectory =
                    &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                PIMAGE_EXPORT_DIRECTORY exportHeader = NULL;
                PULONG functionRvaArray = NULL;
                PULONG nameRvaArray = NULL;
                PUSHORT nameOrdinalArray = NULL;
                ULONG exportNameIndex = 0UL;

                if (exportDirectory->VirtualAddress != 0UL &&
                    KswordARKHookValidateRvaRange(exportDirectory->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), moduleEntry->ImageSize)) {
                    exportHeader = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)moduleEntry->ImageBase + exportDirectory->VirtualAddress);
                    if (exportHeader->AddressOfFunctions != 0UL &&
                        exportHeader->AddressOfNames != 0UL &&
                        exportHeader->AddressOfNameOrdinals != 0UL &&
                        KswordARKHookValidateRvaRange(exportHeader->AddressOfFunctions, exportHeader->NumberOfFunctions * sizeof(ULONG), moduleEntry->ImageSize) &&
                        KswordARKHookValidateRvaRange(exportHeader->AddressOfNames, exportHeader->NumberOfNames * sizeof(ULONG), moduleEntry->ImageSize) &&
                        KswordARKHookValidateRvaRange(exportHeader->AddressOfNameOrdinals, exportHeader->NumberOfNames * sizeof(USHORT), moduleEntry->ImageSize)) {
                        functionRvaArray = (PULONG)((PUCHAR)moduleEntry->ImageBase + exportHeader->AddressOfFunctions);
                        nameRvaArray = (PULONG)((PUCHAR)moduleEntry->ImageBase + exportHeader->AddressOfNames);
                        nameOrdinalArray = (PUSHORT)((PUCHAR)moduleEntry->ImageBase + exportHeader->AddressOfNameOrdinals);

                        for (exportNameIndex = 0UL; exportNameIndex < exportHeader->NumberOfNames; ++exportNameIndex) {
                            const ULONG nameRva = nameRvaArray[exportNameIndex];
                            const USHORT ordinalIndex = nameOrdinalArray[exportNameIndex];
                            ULONG functionRva = 0UL;
                            BOOLEAN suspicious = FALSE;
                            KSWORD_ARK_IAT_EAT_HOOK_ENTRY row;

                            if (ordinalIndex >= exportHeader->NumberOfFunctions ||
                                !KswordARKHookValidateRvaRange(nameRva, 2UL, moduleEntry->ImageSize)) {
                                continue;
                            }
                            functionRva = functionRvaArray[ordinalIndex];
                            if (functionRva >= exportDirectory->VirtualAddress &&
                                functionRva < exportDirectory->VirtualAddress + exportDirectory->Size) {
                                suspicious = FALSE;
                            }
                            else if (!KswordARKHookValidateRvaRange(functionRva, 1UL, moduleEntry->ImageSize)) {
                                suspicious = TRUE;
                            }
                            if (!suspicious && (requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN) == 0UL) {
                                continue;
                            }

                            response->totalCount += 1UL;
                            if (response->returnedCount >= entryCapacity) {
                                continue;
                            }
                            RtlZeroMemory(&row, sizeof(row));
                            row.hookClass = KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT;
                            row.status = suspicious ? KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS : KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
                            row.moduleBase = (ULONGLONG)(ULONG_PTR)moduleEntry->ImageBase;
                            row.currentTarget = (ULONGLONG)((ULONG_PTR)moduleEntry->ImageBase + (ULONG_PTR)functionRva);
                            row.expectedTarget = row.currentTarget;
                            row.ordinal = (ULONG)ordinalIndex + exportHeader->Base;
                            KswordARKHookCopyAnsi(row.functionName, sizeof(row.functionName), (const CHAR*)((PUCHAR)moduleEntry->ImageBase + nameRva));
                            KswordARKHookCopyBoundedAnsiToWide(moduleFileName, moduleFileNameBytes, row.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                            RtlCopyMemory(&response->entries[response->returnedCount], &row, sizeof(row));
                            response->returnedCount += 1UL;
                        }
                    }
                }
            }

            if ((requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS) != 0UL) {
                const IMAGE_DATA_DIRECTORY* importDirectory =
                    &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                PIMAGE_IMPORT_DESCRIPTOR importDescriptor = NULL;

                if (importDirectory->VirtualAddress != 0UL &&
                    KswordARKHookValidateRvaRange(importDirectory->VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), moduleEntry->ImageSize)) {
                    importDescriptor = (PIMAGE_IMPORT_DESCRIPTOR)((PUCHAR)moduleEntry->ImageBase + importDirectory->VirtualAddress);
                    while (importDescriptor->Name != 0UL) {
                        const CHAR* importName = NULL;
                        const KSW_HOOK_SYSTEM_MODULE_ENTRY* importModule = NULL;
                        ULONG findIndex = 0UL;
                        ULONG thunkIndex = 0UL;
                        ULONG_PTR thunkRva = importDescriptor->FirstThunk;
                        ULONG_PTR originalThunkRva = importDescriptor->OriginalFirstThunk;

                        if (!KswordARKHookValidateRvaRange(importDescriptor->Name, 2UL, moduleEntry->ImageSize)) {
                            break;
                        }
                        importName = (const CHAR*)((PUCHAR)moduleEntry->ImageBase + importDescriptor->Name);
                        for (findIndex = 0UL; findIndex < moduleInfo->NumberOfModules; ++findIndex) {
                            const UCHAR* candidateName = NULL;
                            ULONG candidateBytes = 0UL;
                            KswordARKHookGetModuleFileName(&moduleInfo->Modules[findIndex], &candidateName, &candidateBytes);
                            if (KswordARKHookBoundedAnsiEqualsInsensitive(candidateName, candidateBytes, importName)) {
                                importModule = &moduleInfo->Modules[findIndex];
                                break;
                            }
                        }

                        if (thunkRva == 0U || !KswordARKHookValidateRvaRange((ULONG)thunkRva, sizeof(ULONG_PTR), moduleEntry->ImageSize)) {
                            ++importDescriptor;
                            continue;
                        }
                        if (originalThunkRva == 0U) {
                            originalThunkRva = thunkRva;
                        }

                        while (KswordARKHookValidateRvaRange((ULONG)(thunkRva + thunkIndex * sizeof(ULONG_PTR)), sizeof(ULONG_PTR), moduleEntry->ImageSize)) {
                            ULONG_PTR* thunk = (ULONG_PTR*)((PUCHAR)moduleEntry->ImageBase + thunkRva + thunkIndex * sizeof(ULONG_PTR));
                            ULONG_PTR target = *thunk;
                            const KSW_HOOK_SYSTEM_MODULE_ENTRY* targetModule = NULL;
                            BOOLEAN suspicious = FALSE;
                            KSWORD_ARK_IAT_EAT_HOOK_ENTRY row;

                            if (target == 0U) {
                                break;
                            }
                            targetModule = KswordARKHookFindModuleForAddress(moduleInfo, target);
                            if (importModule != NULL && targetModule != importModule) {
                                suspicious = TRUE;
                            }
                            if (!suspicious && (requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN) == 0UL) {
                                ++thunkIndex;
                                continue;
                            }

                            response->totalCount += 1UL;
                            if (response->returnedCount < entryCapacity) {
                                const UCHAR* targetName = NULL;
                                ULONG targetNameBytes = 0UL;
                                ULONG importChar = 0UL;

                                RtlZeroMemory(&row, sizeof(row));
                                row.hookClass = KSWORD_ARK_IAT_EAT_HOOK_CLASS_IAT;
                                row.status = suspicious ? KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS : KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
                                row.moduleBase = (ULONGLONG)(ULONG_PTR)moduleEntry->ImageBase;
                                row.thunkAddress = (ULONGLONG)(ULONG_PTR)thunk;
                                row.currentTarget = (ULONGLONG)target;
                                row.targetModuleBase = targetModule ? (ULONGLONG)(ULONG_PTR)targetModule->ImageBase : 0ULL;
                                row.ordinal = thunkIndex;
                                KswordARKHookCopyAnsi(row.functionName, sizeof(row.functionName), "<import-thunk>");
                                KswordARKHookCopyBoundedAnsiToWide(moduleFileName, moduleFileNameBytes, row.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                                {
                                    WCHAR importWide[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS] = { 0 };
                                    for (importChar = 0UL; importChar + 1UL < KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS && importName[importChar] != '\0'; ++importChar) {
                                        importWide[importChar] = (WCHAR)importName[importChar];
                                    }
                                    RtlCopyMemory(row.importModuleName, importWide, sizeof(importWide));
                                }
                                if (targetModule != NULL) {
                                    KswordARKHookGetModuleFileName(targetModule, &targetName, &targetNameBytes);
                                    KswordARKHookCopyBoundedAnsiToWide(targetName, targetNameBytes, row.targetModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                                }
                                RtlCopyMemory(&response->entries[response->returnedCount], &row, sizeof(row));
                                response->returnedCount += 1UL;
                            }
                            ++thunkIndex;
                        }
                        ++importDescriptor;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            response->lastStatus = GetExceptionCode();
        }
    }

    response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
    *BytesWrittenOut = g_KswordArkIatEatHookResponseHeaderSize +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY));
    ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    return STATUS_SUCCESS;
}
