/*++

Module Name:

    hook_scan_support.c

Abstract:

    Safe kernel image read helpers for hook_scan.c.

Environment:

    Kernel-mode Driver Framework

--*/

#include "hook_scan_support.h"

#define KSW_HOOK_SCAN_SYSTEM_MODULE_INFORMATION_CLASS 11UL

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

CHAR
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

BOOLEAN
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

BOOLEAN
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

VOID
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

VOID
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

BOOLEAN
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

NTSTATUS
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

const KSW_HOOK_SYSTEM_MODULE_ENTRY*
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

VOID
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

BOOLEAN
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
    MM_COPY_ADDRESS copyAddress;
    SIZE_T copiedBytes = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (Source == NULL || Destination == NULL || BytesToRead == 0U) {
        return FALSE;
    }
    if (KeGetCurrentIrql() > APC_LEVEL) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }

    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.VirtualAddress = (PVOID)Source;
    __try {
        status = MmCopyMemory(
            Destination,
            copyAddress,
            BytesToRead,
            MM_COPY_MEMORY_VIRTUAL,
            &copiedBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }

    if (!NT_SUCCESS(status) || copiedBytes != BytesToRead) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
KswordARKHookMultiplyUlong(
    _In_ ULONG LeftValue,
    _In_ ULONG RightValue,
    _Out_ ULONG* ProductOut
    )
/*++

Routine Description:

    安全计算 ULONG 乘积。中文说明：PE 表项数量来自目标映像，先检查乘法溢出
    才能继续做 RVA 范围校验。

Arguments:

    LeftValue - 左操作数。
    RightValue - 右操作数。
    ProductOut - 返回乘积。

Return Value:

    TRUE 表示乘积可用；FALSE 表示参数无效或溢出。

--*/
{
    ULONGLONG product = 0ULL;

    if (ProductOut == NULL) {
        return FALSE;
    }
    *ProductOut = 0UL;

    product = (ULONGLONG)LeftValue * (ULONGLONG)RightValue;
    if (product > MAXULONG) {
        return FALSE;
    }

    *ProductOut = (ULONG)product;
    return TRUE;
}

BOOLEAN
KswordARKHookAddRvaOffset(
    _In_ ULONG BaseRva,
    _In_ ULONG Index,
    _In_ ULONG ElementBytes,
    _Out_ ULONG* RvaOut
    )
/*++

Routine Description:

    安全计算数组元素 RVA。中文说明：IAT/EAT 数组下标可能来自异常 PE，
    不能让 ULONG 加法回绕后落回映像范围。

Arguments:

    BaseRva - 数组起始 RVA。
    Index - 元素下标。
    ElementBytes - 单个元素大小。
    RvaOut - 返回元素 RVA。

Return Value:

    TRUE 表示结果未溢出。

--*/
{
    ULONGLONG offset = 0ULL;
    ULONGLONG result = 0ULL;

    if (RvaOut == NULL || ElementBytes == 0UL) {
        return FALSE;
    }
    *RvaOut = 0UL;

    offset = (ULONGLONG)Index * (ULONGLONG)ElementBytes;
    result = (ULONGLONG)BaseRva + offset;
    if (result > MAXULONG) {
        return FALSE;
    }

    *RvaOut = (ULONG)result;
    return TRUE;
}

BOOLEAN
KswordARKHookImageAddressFromRva(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_ ULONG_PTR* AddressOut
    )
/*++

Routine Description:

    将有效 RVA 转为内核虚拟地址。中文说明：转换前复用映像边界校验，
    并额外防止基址加 RVA 发生指针回绕。

Arguments:

    ModuleEntry - 当前模块快照条目。
    Rva - 待转换 RVA。
    AddressOut - 返回虚拟地址数值。

Return Value:

    TRUE 表示地址可用于只读探测。

--*/
{
    ULONG_PTR imageBase = 0U;

    if (ModuleEntry == NULL || ModuleEntry->ImageBase == NULL || AddressOut == NULL) {
        return FALSE;
    }
    *AddressOut = 0U;

    if (!KswordARKHookValidateRvaRange(Rva, 1UL, ModuleEntry->ImageSize)) {
        return FALSE;
    }

    imageBase = (ULONG_PTR)ModuleEntry->ImageBase;
    if ((ULONG_PTR)Rva > (((ULONG_PTR)(~(ULONG_PTR)0)) - imageBase)) {
        return FALSE;
    }

    *AddressOut = imageBase + (ULONG_PTR)Rva;
    return TRUE;
}

BOOLEAN
KswordARKHookReadImageBytes(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_writes_bytes_(BytesToRead) VOID* Destination,
    _In_ SIZE_T BytesToRead
    )
/*++

Routine Description:

    按 RVA 安全读取已加载映像字节。中文说明：所有 PE 目录访问都应走该函数，
    避免对可能卸载或分页异常的模块地址直接解引用。

Arguments:

    ModuleEntry - 当前模块快照条目。
    Rva - 起始 RVA。
    Destination - 输出缓冲。
    BytesToRead - 读取字节数。

Return Value:

    TRUE 表示完整读取成功。

--*/
{
    ULONG_PTR sourceAddress = 0U;

    if (ModuleEntry == NULL || Destination == NULL || BytesToRead == 0U || BytesToRead > MAXULONG) {
        return FALSE;
    }
    if (!KswordARKHookValidateRvaRange(Rva, (ULONG)BytesToRead, ModuleEntry->ImageSize)) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }
    if (!KswordARKHookImageAddressFromRva(ModuleEntry, Rva, &sourceAddress)) {
        RtlZeroMemory(Destination, BytesToRead);
        return FALSE;
    }

    return KswordARKHookReadMemorySafe((const VOID*)sourceAddress, Destination, BytesToRead);
}

BOOLEAN
KswordARKHookReadImageNtHeaders(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _Out_ IMAGE_NT_HEADERS* NtHeadersOut
    )
/*++

Routine Description:

    安全读取映像 NT 头。中文说明：参考 System Informer 的做法先验证头部边界，
    但实际访问通过 MmCopyMemory 完成，避免坏映像头触发 PAGE_FAULT。

Arguments:

    ModuleEntry - 当前模块快照条目。
    NtHeadersOut - 返回 NT 头副本。

Return Value:

    TRUE 表示 DOS/NT 签名和头部范围均有效。

--*/
{
    IMAGE_DOS_HEADER dosHeader;
    ULONG peOffset = 0UL;

    if (ModuleEntry == NULL || NtHeadersOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(&dosHeader, sizeof(dosHeader));
    RtlZeroMemory(NtHeadersOut, sizeof(*NtHeadersOut));

    if (!KswordARKHookReadImageBytes(ModuleEntry, 0UL, &dosHeader, sizeof(dosHeader))) {
        return FALSE;
    }
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0) {
        return FALSE;
    }

    peOffset = (ULONG)dosHeader.e_lfanew;
    if (!KswordARKHookValidateRvaRange(peOffset, sizeof(*NtHeadersOut), ModuleEntry->ImageSize)) {
        return FALSE;
    }
    if (!KswordARKHookReadImageBytes(ModuleEntry, peOffset, NtHeadersOut, sizeof(*NtHeadersOut))) {
        return FALSE;
    }

    if (NtHeadersOut->Signature != IMAGE_NT_SIGNATURE) {
        return FALSE;
    }
#if defined(_M_AMD64)
    if (NtHeadersOut->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return FALSE;
    }
#endif

    return TRUE;
}

BOOLEAN
KswordARKHookGetDataDirectory(
    _In_ const IMAGE_NT_HEADERS* NtHeaders,
    _In_ ULONG DirectoryIndex,
    _Out_ IMAGE_DATA_DIRECTORY* DirectoryOut
    )
/*++

Routine Description:

    从本地 NT 头副本取得数据目录。中文说明：先检查 NumberOfRvaAndSizes，
    防止旧映像或损坏映像缺少目标目录。

Arguments:

    NtHeaders - 本地 NT 头副本。
    DirectoryIndex - IMAGE_DIRECTORY_ENTRY_* 下标。
    DirectoryOut - 返回目录副本。

Return Value:

    TRUE 表示目录字段存在。

--*/
{
    if (NtHeaders == NULL || DirectoryOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(DirectoryOut, sizeof(*DirectoryOut));

    if (DirectoryIndex >= NtHeaders->OptionalHeader.NumberOfRvaAndSizes ||
        DirectoryIndex >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES) {
        return FALSE;
    }

    *DirectoryOut = NtHeaders->OptionalHeader.DataDirectory[DirectoryIndex];
    return TRUE;
}

BOOLEAN
KswordARKHookReadImageUlong(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_ ULONG* ValueOut
    )
/*++

Routine Description:

    从映像 RVA 读取 ULONG。中文说明：用于导入导出表数组，调用者无需直接
    解引用目标映像地址。

Arguments:

    ModuleEntry - 当前模块快照条目。
    Rva - ULONG 所在 RVA。
    ValueOut - 返回读取值。

Return Value:

    TRUE 表示完整读取成功。

--*/
{
    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0UL;
    return KswordARKHookReadImageBytes(ModuleEntry, Rva, ValueOut, sizeof(*ValueOut));
}

BOOLEAN
KswordARKHookReadImageUshort(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_ USHORT* ValueOut
    )
/*++

Routine Description:

    从映像 RVA 读取 USHORT。中文说明：用于读取导出 ordinal 数组，失败时
    返回 FALSE 而不是让扫描线程触发异常。

Arguments:

    ModuleEntry - 当前模块快照条目。
    Rva - USHORT 所在 RVA。
    ValueOut - 返回读取值。

Return Value:

    TRUE 表示完整读取成功。

--*/
{
    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0U;
    return KswordARKHookReadImageBytes(ModuleEntry, Rva, ValueOut, sizeof(*ValueOut));
}

BOOLEAN
KswordARKHookCopyImageAnsi(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntry,
    _In_ ULONG Rva,
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes
    )
/*++

Routine Description:

    从映像 RVA 复制 NUL 结束 ANSI 字符串。中文说明：导出名和导入模块名
    都来自目标映像，逐字节安全读取可避免跨页坏地址导致蓝屏。

Arguments:

    ModuleEntry - 当前模块快照条目。
    Rva - 字符串起始 RVA。
    Destination - 目标 ANSI 缓冲。
    DestinationBytes - 目标字节容量。

Return Value:

    TRUE 表示读取到 NUL；FALSE 表示参数无效、越界或字符串被截断。

--*/
{
    size_t index = 0U;
    CHAR oneChar = '\0';

    if (Destination == NULL || DestinationBytes == 0U) {
        return FALSE;
    }
    Destination[0] = '\0';

    if (ModuleEntry == NULL || !KswordARKHookValidateRvaRange(Rva, 1UL, ModuleEntry->ImageSize)) {
        return FALSE;
    }

    for (index = 0U; index + 1U < DestinationBytes; ++index) {
        ULONG charRva = 0UL;

        if ((ULONGLONG)Rva + (ULONGLONG)index > MAXULONG) {
            Destination[index] = '\0';
            return FALSE;
        }

        charRva = Rva + (ULONG)index;
        if (!KswordARKHookReadImageBytes(ModuleEntry, charRva, &oneChar, sizeof(oneChar))) {
            Destination[index] = '\0';
            return FALSE;
        }

        Destination[index] = oneChar;
        if (oneChar == '\0') {
            return TRUE;
        }
    }

    Destination[DestinationBytes - 1U] = '\0';
    return FALSE;
}

BOOLEAN
KswordARKHookIsRvaInsideDirectory(
    _In_ ULONG Rva,
    _In_ const IMAGE_DATA_DIRECTORY* Directory
    )
/*++

Routine Description:

    判断 RVA 是否位于指定 PE 数据目录内。中文说明：导出转发字符串位于
    export directory，本函数用无溢出的方式处理目录尾地址。

Arguments:

    Rva - 待判断 RVA。
    Directory - 数据目录副本。

Return Value:

    TRUE 表示 Rva 落在目录范围内。

--*/
{
    ULONG directoryEnd = 0UL;

    if (Directory == NULL || Directory->VirtualAddress == 0UL || Directory->Size == 0UL) {
        return FALSE;
    }
    if (Directory->Size > MAXULONG - Directory->VirtualAddress) {
        return FALSE;
    }

    directoryEnd = Directory->VirtualAddress + Directory->Size;
    return Rva >= Directory->VirtualAddress && Rva < directoryEnd;
}

