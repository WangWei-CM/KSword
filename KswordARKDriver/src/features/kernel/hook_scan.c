/*++

Module Name:

    hook_scan.c

Abstract:

    Kernel inline hook and IAT/EAT diagnostic helpers for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "hook_scan_support.h"

#include <ntimage.h>

#define KSW_HOOK_SCAN_IMPORT_DESCRIPTOR_LIMIT 1024UL
#define KSW_HOOK_SCAN_IMPORT_THUNK_LIMIT 16384UL

#ifndef STATUS_REQUEST_NOT_ACCEPTED
#define STATUS_REQUEST_NOT_ACCEPTED ((NTSTATUS)0xC00000D0L)
#endif

static const ULONG g_KswordArkInlineHookResponseHeaderSize =
    (ULONG)(sizeof(KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY));

static const ULONG g_KswordArkIatEatHookResponseHeaderSize =
    (ULONG)(sizeof(KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY));

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
        IMAGE_NT_HEADERS ntHeaders;
        IMAGE_DATA_DIRECTORY exportDirectory;
        IMAGE_EXPORT_DIRECTORY exportHeader;
        ULONG nameArrayBytes = 0UL;
        ULONG ordinalArrayBytes = 0UL;
        ULONG functionArrayBytes = 0UL;
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

        RtlZeroMemory(&ntHeaders, sizeof(ntHeaders));
        RtlZeroMemory(&exportDirectory, sizeof(exportDirectory));
        RtlZeroMemory(&exportHeader, sizeof(exportHeader));
        if (!KswordARKHookReadImageNtHeaders(moduleEntry, &ntHeaders) ||
            !KswordARKHookGetDataDirectory(&ntHeaders, IMAGE_DIRECTORY_ENTRY_EXPORT, &exportDirectory) ||
            exportDirectory.VirtualAddress == 0UL ||
            !KswordARKHookReadImageBytes(moduleEntry, exportDirectory.VirtualAddress, &exportHeader, sizeof(exportHeader))) {
            continue;
        }

        if (exportHeader.AddressOfNames == 0UL ||
            exportHeader.AddressOfNameOrdinals == 0UL ||
            exportHeader.AddressOfFunctions == 0UL ||
            !KswordARKHookMultiplyUlong(exportHeader.NumberOfNames, sizeof(ULONG), &nameArrayBytes) ||
            !KswordARKHookMultiplyUlong(exportHeader.NumberOfNames, sizeof(USHORT), &ordinalArrayBytes) ||
            !KswordARKHookMultiplyUlong(exportHeader.NumberOfFunctions, sizeof(ULONG), &functionArrayBytes) ||
            !KswordARKHookValidateRvaRange(exportHeader.AddressOfNames, nameArrayBytes, moduleEntry->ImageSize) ||
            !KswordARKHookValidateRvaRange(exportHeader.AddressOfNameOrdinals, ordinalArrayBytes, moduleEntry->ImageSize) ||
            !KswordARKHookValidateRvaRange(exportHeader.AddressOfFunctions, functionArrayBytes, moduleEntry->ImageSize)) {
            continue;
        }

        for (exportNameIndex = 0UL; exportNameIndex < exportHeader.NumberOfNames; ++exportNameIndex) {
            ULONG nameEntryRva = 0UL;
            ULONG ordinalEntryRva = 0UL;
            ULONG functionEntryRva = 0UL;
            ULONG nameRva = 0UL;
            USHORT ordinalIndex = 0U;
            CHAR exportNameBuffer[KSWORD_ARK_KERNEL_HOOK_NAME_CHARS] = { 0 };
            ULONG functionRva = 0UL;
            ULONG_PTR functionAddressValue = 0U;
            UCHAR currentBytes[KSWORD_ARK_KERNEL_HOOK_BYTES] = { 0 };
            UCHAR expectedBytes[KSWORD_ARK_KERNEL_HOOK_BYTES] = { 0 };
            ULONG_PTR targetAddress = 0U;
            ULONG hookType = KSWORD_ARK_INLINE_HOOK_TYPE_NONE;
            KSWORD_ARK_INLINE_HOOK_ENTRY tempEntry;

            if (!KswordARKHookAddRvaOffset(exportHeader.AddressOfNames, exportNameIndex, sizeof(ULONG), &nameEntryRva) ||
                !KswordARKHookAddRvaOffset(exportHeader.AddressOfNameOrdinals, exportNameIndex, sizeof(USHORT), &ordinalEntryRva) ||
                !KswordARKHookReadImageUlong(moduleEntry, nameEntryRva, &nameRva) ||
                !KswordARKHookReadImageUshort(moduleEntry, ordinalEntryRva, &ordinalIndex) ||
                ordinalIndex >= exportHeader.NumberOfFunctions) {
                continue;
            }

            if (!KswordARKHookAddRvaOffset(exportHeader.AddressOfFunctions, ordinalIndex, sizeof(ULONG), &functionEntryRva) ||
                !KswordARKHookReadImageUlong(moduleEntry, functionEntryRva, &functionRva) ||
                KswordARKHookIsRvaInsideDirectory(functionRva, &exportDirectory) ||
                !KswordARKHookImageAddressFromRva(moduleEntry, functionRva, &functionAddressValue) ||
                !KswordARKHookReadImageBytes(moduleEntry, functionRva, currentBytes, sizeof(currentBytes)) ||
                !KswordARKHookCopyImageAnsi(moduleEntry, nameRva, exportNameBuffer, sizeof(exportNameBuffer))) {
                continue;
            }

            RtlCopyMemory(expectedBytes, currentBytes, sizeof(expectedBytes));
            hookType = KswordARKHookClassifyInlineBytes(
                functionAddressValue,
                currentBytes,
                KSWORD_ARK_KERNEL_HOOK_BYTES,
                &targetAddress);

            RtlZeroMemory(&tempEntry, sizeof(tempEntry));
            KswordARKHookFillInlineEntry(
                moduleInfo,
                moduleEntry,
                exportNameBuffer,
                (PVOID)functionAddressValue,
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
        IMAGE_NT_HEADERS ntHeaders;
        IMAGE_DATA_DIRECTORY exportDirectory;
        IMAGE_DATA_DIRECTORY importDirectory;

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

        RtlZeroMemory(&ntHeaders, sizeof(ntHeaders));
        RtlZeroMemory(&exportDirectory, sizeof(exportDirectory));
        RtlZeroMemory(&importDirectory, sizeof(importDirectory));
        if (!KswordARKHookReadImageNtHeaders(moduleEntry, &ntHeaders)) {
            continue;
        }
        (VOID)KswordARKHookGetDataDirectory(&ntHeaders, IMAGE_DIRECTORY_ENTRY_EXPORT, &exportDirectory);
        (VOID)KswordARKHookGetDataDirectory(&ntHeaders, IMAGE_DIRECTORY_ENTRY_IMPORT, &importDirectory);

        if ((requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS) != 0UL) {
            IMAGE_EXPORT_DIRECTORY exportHeader;
            ULONG functionArrayBytes = 0UL;
            ULONG nameArrayBytes = 0UL;
            ULONG ordinalArrayBytes = 0UL;
            ULONG exportNameIndex = 0UL;

            RtlZeroMemory(&exportHeader, sizeof(exportHeader));
            if (exportDirectory.VirtualAddress != 0UL &&
                KswordARKHookReadImageBytes(moduleEntry, exportDirectory.VirtualAddress, &exportHeader, sizeof(exportHeader)) &&
                exportHeader.AddressOfFunctions != 0UL &&
                exportHeader.AddressOfNames != 0UL &&
                exportHeader.AddressOfNameOrdinals != 0UL &&
                KswordARKHookMultiplyUlong(exportHeader.NumberOfFunctions, sizeof(ULONG), &functionArrayBytes) &&
                KswordARKHookMultiplyUlong(exportHeader.NumberOfNames, sizeof(ULONG), &nameArrayBytes) &&
                KswordARKHookMultiplyUlong(exportHeader.NumberOfNames, sizeof(USHORT), &ordinalArrayBytes) &&
                KswordARKHookValidateRvaRange(exportHeader.AddressOfFunctions, functionArrayBytes, moduleEntry->ImageSize) &&
                KswordARKHookValidateRvaRange(exportHeader.AddressOfNames, nameArrayBytes, moduleEntry->ImageSize) &&
                KswordARKHookValidateRvaRange(exportHeader.AddressOfNameOrdinals, ordinalArrayBytes, moduleEntry->ImageSize)) {
                for (exportNameIndex = 0UL; exportNameIndex < exportHeader.NumberOfNames; ++exportNameIndex) {
                    ULONG nameEntryRva = 0UL;
                    ULONG ordinalEntryRva = 0UL;
                    ULONG functionEntryRva = 0UL;
                    ULONG nameRva = 0UL;
                    USHORT ordinalIndex = 0U;
                    ULONG functionRva = 0UL;
                    BOOLEAN suspicious = FALSE;
                    KSWORD_ARK_IAT_EAT_HOOK_ENTRY row;

                    if (!KswordARKHookAddRvaOffset(exportHeader.AddressOfNames, exportNameIndex, sizeof(ULONG), &nameEntryRva) ||
                        !KswordARKHookAddRvaOffset(exportHeader.AddressOfNameOrdinals, exportNameIndex, sizeof(USHORT), &ordinalEntryRva) ||
                        !KswordARKHookReadImageUlong(moduleEntry, nameEntryRva, &nameRva) ||
                        !KswordARKHookReadImageUshort(moduleEntry, ordinalEntryRva, &ordinalIndex) ||
                        ordinalIndex >= exportHeader.NumberOfFunctions ||
                        !KswordARKHookAddRvaOffset(exportHeader.AddressOfFunctions, ordinalIndex, sizeof(ULONG), &functionEntryRva) ||
                        !KswordARKHookReadImageUlong(moduleEntry, functionEntryRva, &functionRva)) {
                        continue;
                    }

                    if (KswordARKHookIsRvaInsideDirectory(functionRva, &exportDirectory)) {
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
                    row.ordinal = (ULONG)ordinalIndex + exportHeader.Base;
                    (VOID)KswordARKHookCopyImageAnsi(moduleEntry, nameRva, row.functionName, sizeof(row.functionName));
                    KswordARKHookCopyBoundedAnsiToWide(moduleFileName, moduleFileNameBytes, row.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                    RtlCopyMemory(&response->entries[response->returnedCount], &row, sizeof(row));
                    response->returnedCount += 1UL;
                }
            }
        }

        if ((requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS) != 0UL) {
            ULONG descriptorIndex = 0UL;
            ULONG importDirectoryEnd = 0UL;

            if (importDirectory.VirtualAddress == 0UL ||
                importDirectory.Size == 0UL ||
                importDirectory.Size > MAXULONG - importDirectory.VirtualAddress) {
                continue;
            }
            importDirectoryEnd = importDirectory.VirtualAddress + importDirectory.Size;

            for (descriptorIndex = 0UL;
                descriptorIndex < KSW_HOOK_SCAN_IMPORT_DESCRIPTOR_LIMIT;
                ++descriptorIndex) {
                ULONG descriptorRva = 0UL;
                IMAGE_IMPORT_DESCRIPTOR importDescriptor;
                CHAR importNameBuffer[KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS] = { 0 };
                const KSW_HOOK_SYSTEM_MODULE_ENTRY* importModule = NULL;
                ULONG findIndex = 0UL;
                ULONG thunkIndex = 0UL;
                ULONG thunkRva = 0UL;

                if (!KswordARKHookAddRvaOffset(importDirectory.VirtualAddress, descriptorIndex, sizeof(IMAGE_IMPORT_DESCRIPTOR), &descriptorRva) ||
                    descriptorRva >= importDirectoryEnd ||
                    (importDirectoryEnd - descriptorRva) < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
                    !KswordARKHookReadImageBytes(moduleEntry, descriptorRva, &importDescriptor, sizeof(importDescriptor))) {
                    break;
                }
                if (importDescriptor.Name == 0UL) {
                    break;
                }
                if (importDescriptor.FirstThunk == 0UL ||
                    importDescriptor.FirstThunk > MAXULONG ||
                    !KswordARKHookCopyImageAnsi(moduleEntry, importDescriptor.Name, importNameBuffer, sizeof(importNameBuffer))) {
                    continue;
                }

                for (findIndex = 0UL; findIndex < moduleInfo->NumberOfModules; ++findIndex) {
                    const UCHAR* candidateName = NULL;
                    ULONG candidateBytes = 0UL;

                    KswordARKHookGetModuleFileName(&moduleInfo->Modules[findIndex], &candidateName, &candidateBytes);
                    if (KswordARKHookBoundedAnsiEqualsInsensitive(candidateName, candidateBytes, importNameBuffer)) {
                        importModule = &moduleInfo->Modules[findIndex];
                        break;
                    }
                }

                thunkRva = importDescriptor.FirstThunk;
                if (!KswordARKHookValidateRvaRange(thunkRva, sizeof(ULONG_PTR), moduleEntry->ImageSize)) {
                    continue;
                }

                for (thunkIndex = 0UL;
                    thunkIndex < KSW_HOOK_SCAN_IMPORT_THUNK_LIMIT;
                    ++thunkIndex) {
                    ULONG thunkEntryRva = 0UL;
                    ULONG_PTR thunkAddressValue = 0U;
                    ULONG_PTR target = 0U;
                    const KSW_HOOK_SYSTEM_MODULE_ENTRY* targetModule = NULL;
                    BOOLEAN suspicious = FALSE;
                    KSWORD_ARK_IAT_EAT_HOOK_ENTRY row;

                    if (!KswordARKHookAddRvaOffset(thunkRva, thunkIndex, sizeof(ULONG_PTR), &thunkEntryRva) ||
                        !KswordARKHookImageAddressFromRva(moduleEntry, thunkEntryRva, &thunkAddressValue) ||
                        !KswordARKHookReadImageBytes(moduleEntry, thunkEntryRva, &target, sizeof(target))) {
                        break;
                    }
                    if (target == 0U) {
                        break;
                    }

                    targetModule = KswordARKHookFindModuleForAddress(moduleInfo, target);
                    if (importModule != NULL && targetModule != importModule) {
                        suspicious = TRUE;
                    }
                    if (!suspicious && (requestFlags & KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN) == 0UL) {
                        continue;
                    }

                    response->totalCount += 1UL;
                    if (response->returnedCount < entryCapacity) {
                        const UCHAR* targetName = NULL;
                        ULONG targetNameBytes = 0UL;

                        RtlZeroMemory(&row, sizeof(row));
                        row.hookClass = KSWORD_ARK_IAT_EAT_HOOK_CLASS_IAT;
                        row.status = suspicious ? KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS : KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
                        row.moduleBase = (ULONGLONG)(ULONG_PTR)moduleEntry->ImageBase;
                        row.thunkAddress = (ULONGLONG)thunkAddressValue;
                        row.currentTarget = (ULONGLONG)target;
                        row.targetModuleBase = targetModule ? (ULONGLONG)(ULONG_PTR)targetModule->ImageBase : 0ULL;
                        row.ordinal = thunkIndex;
                        KswordARKHookCopyAnsi(row.functionName, sizeof(row.functionName), "<import-thunk>");
                        KswordARKHookCopyBoundedAnsiToWide(moduleFileName, moduleFileNameBytes, row.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                        KswordARKHookCopyBoundedAnsiToWide((const UCHAR*)importNameBuffer, (ULONG)sizeof(importNameBuffer), row.importModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                        if (targetModule != NULL) {
                            KswordARKHookGetModuleFileName(targetModule, &targetName, &targetNameBytes);
                            KswordARKHookCopyBoundedAnsiToWide(targetName, targetNameBytes, row.targetModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
                        }
                        RtlCopyMemory(&response->entries[response->returnedCount], &row, sizeof(row));
                        response->returnedCount += 1UL;
                    }
                }
            }
        }
    }

    response->status = KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN;
    *BytesWrittenOut = g_KswordArkIatEatHookResponseHeaderSize +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY));
    ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    return STATUS_SUCCESS;
}
