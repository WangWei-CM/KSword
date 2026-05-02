/*++

Module Name:

    callback_enum.c

Abstract:

    Implements the read-only callback traversal IOCTL for KswordARK.

Environment:

    Kernel-mode Driver Framework

--*/

#include <fltKernel.h>
#include "callback_internal.h"
#define KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL 1
#include "callback_external_core.h"
#include "ark/ark_dyndata.h"

#define KSWORD_ARK_CALLBACK_ENUM_TAG 'eCbK'
#define KSWORD_ARK_CALLBACK_ENUM_MAX_ENTRIES 4096UL
#define KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES 0x300UL
#define KSWORD_ARK_CALLBACK_ENUM_NOTIFY_SLOT_COUNT 64UL
#define KSWORD_ARK_CALLBACK_ENUM_LIST_WALK_LIMIT 512UL
#define KSWORD_ARK_CALLBACK_ENUM_OBJECT_TYPE_SCAN_BYTES 0x300UL
#define KSWORD_ARK_CALLBACK_ENUM_POINTER_SCAN_BACK_BYTES 0x80L
#define KSWORD_ARK_CALLBACK_ENUM_POINTER_SCAN_FORWARD_BYTES 0x180L
#define KSWORD_ARK_CALLBACK_ENUM_FAST_REF_MASK (~(ULONG_PTR)0x0FULL)
#define SystemModuleInformation 11UL

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

typedef struct _KSWORD_ARK_CALLBACK_ENUM_CODE_CANDIDATE
{
    ULONG64 Address;
    LONG RelativeOffset;
} KSWORD_ARK_CALLBACK_ENUM_CODE_CANDIDATE;

typedef struct _KSWORD_ARK_CALLBACK_ENUM_OBJECT_SCAN_RESULT
{
    ULONG64 PreOperation;
    ULONG64 PostOperation;
    ULONG OperationMask;
    ULONG64 RegistrationBlock;
} KSWORD_ARK_CALLBACK_ENUM_OBJECT_SCAN_RESULT;

static const ULONG g_KswordArkCallbackEnumHeaderBytes =
    (ULONG)(sizeof(KSWORD_ARK_ENUM_CALLBACKS_RESPONSE) - sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY));

_Must_inspect_result_
static NTSTATUS
KswordArkCallbackEnumResolveModuleByAddress(
    _In_ ULONG64 CallbackAddress,
    _Out_writes_(ModulePathChars) PWCHAR ModulePath,
    _In_ ULONG ModulePathChars,
    _Out_opt_ ULONG64* ModuleBaseOut,
    _Out_opt_ ULONG* ModuleSizeOut
    );

extern NTSTATUS
KswordArkRegistryCallback(
    _In_opt_ PVOID callbackContext,
    _In_opt_ PVOID argument1,
    _In_opt_ PVOID argument2
    );

extern VOID
KswordArkProcessCreateNotifyEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    );

extern VOID
KswordArkThreadCreateNotify(
    _In_ HANDLE ProcessId,
    _In_ HANDLE ThreadId,
    _In_ BOOLEAN Create
    );

extern VOID
KswordArkLoadImageNotify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo
    );

extern OB_PREOP_CALLBACK_STATUS
KswordArkObjectPreOperation(
    _In_ PVOID RegistrationContext,
    _Inout_ POB_PRE_OPERATION_INFORMATION OperationInformation
    );

extern FLT_PREOP_CALLBACK_STATUS
FLTAPI
KswordArkMinifilterPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
    );

VOID
KswordArkCallbackEnumCopyWide(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_opt_z_ PCWSTR Source
    )
/*++

Routine Description:

    把 NUL 结尾的宽字符串复制到固定响应字段。中文说明：函数总是截断并
    终止字符串，因此 R3 可以按固定字段安全转换。

Arguments:

    Destination - 输出宽字符缓冲区。
    DestinationChars - 输出缓冲区容量，单位为 WCHAR。
    Source - 可选输入宽字符串。

Return Value:

    无返回值。

--*/
{
    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL) {
        return;
    }

    (VOID)RtlStringCchCopyNW(Destination, (size_t)DestinationChars, Source, (size_t)(DestinationChars - 1UL));
    Destination[DestinationChars - 1UL] = L'\0';
}

VOID
KswordArkCallbackEnumCopyUnicode(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_opt_ PCUNICODE_STRING Source
    )
{
    size_t copyChars = 0U;

    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0U) {
        return;
    }

    copyChars = (size_t)(Source->Length / sizeof(WCHAR));
    if (copyChars >= (size_t)DestinationChars) {
        copyChars = (size_t)DestinationChars - 1U;
    }

    RtlCopyMemory(Destination, Source->Buffer, copyChars * sizeof(WCHAR));
    Destination[copyChars] = L'\0';
}

static VOID
KswordArkCallbackEnumCopyAnsiPathToWide(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_reads_bytes_(SourceBytes) const UCHAR* Source,
    _In_ ULONG SourceBytes
    )
{
    ULONG index = 0UL;

    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL || SourceBytes == 0UL) {
        return;
    }

    for (index = 0UL; index + 1UL < DestinationChars && index < SourceBytes; ++index) {
        if (Source[index] == '\0') {
            break;
        }
        Destination[index] = (WCHAR)Source[index];
    }

    Destination[index] = L'\0';
}

VOID
KswordArkCallbackEnumInitModuleCache(
    _Out_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache
    )
/*++

Routine Description:

    初始化模块缓存结构。中文说明：缓存只在一次 IOCTL 枚举周期内使用，避免
    每解析一个回调地址都重复查询 SystemModuleInformation。

Arguments:

    ModuleCache - 输出模块缓存。

Return Value:

    无返回值。

--*/
{
    if (ModuleCache == NULL) {
        return;
    }

    ModuleCache->ModuleInfo = NULL;
    ModuleCache->ModuleInfoBytes = 0UL;
}

VOID
KswordArkCallbackEnumFreeModuleCache(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache
    )
/*++

Routine Description:

    释放模块缓存。中文说明：函数仅释放本枚举路径分配的非分页池，并把指针清零
    防止后续误用。

Arguments:

    ModuleCache - 输入输出模块缓存。

Return Value:

    无返回值。

--*/
{
    if (ModuleCache == NULL) {
        return;
    }

    if (ModuleCache->ModuleInfo != NULL) {
        ExFreePool(ModuleCache->ModuleInfo);
        ModuleCache->ModuleInfo = NULL;
    }
    ModuleCache->ModuleInfoBytes = 0UL;
}

_Must_inspect_result_
NTSTATUS
KswordArkCallbackEnumEnsureModuleCache(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache
    )
/*++

Routine Description:

    按需填充系统模块缓存。中文说明：私有回调扫描需要频繁判断候选函数地址是否
    落在已加载内核模块内，缓存后可减少 ZwQuerySystemInformation 开销。

Arguments:

    ModuleCache - 输入输出模块缓存。

Return Value:

    成功返回 STATUS_SUCCESS；分配或查询失败返回对应 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;

    if (ModuleCache == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ModuleCache->ModuleInfo != NULL) {
        return STATUS_SUCCESS;
    }

    status = ZwQuerySystemInformation(SystemModuleInformation, NULL, 0UL, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH || requiredBytes == 0UL) {
        return STATUS_UNSUCCESSFUL;
    }

    ModuleCache->ModuleInfo = (KSWORD_ARK_CALLBACK_MODULE_INFORMATION*)KswordArkAllocateNonPaged(
        requiredBytes,
        KSWORD_ARK_CALLBACK_ENUM_TAG);
    if (ModuleCache->ModuleInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ModuleCache->ModuleInfoBytes = requiredBytes;

    status = ZwQuerySystemInformation(
        SystemModuleInformation,
        ModuleCache->ModuleInfo,
        requiredBytes,
        &requiredBytes);
    if (!NT_SUCCESS(status)) {
        KswordArkCallbackEnumFreeModuleCache(ModuleCache);
        return status;
    }

    return STATUS_SUCCESS;
}

_Must_inspect_result_
NTSTATUS
KswordArkCallbackEnumResolveModuleByAddressCached(
    _Inout_opt_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG64 CallbackAddress,
    _Out_writes_(ModulePathChars) PWCHAR ModulePath,
    _In_ ULONG ModulePathChars,
    _Out_opt_ ULONG64* ModuleBaseOut,
    _Out_opt_ ULONG* ModuleSizeOut
    )
/*++

Routine Description:

    使用缓存的系统模块表解析地址所属模块。中文说明：该函数不解引用回调函数
    地址，仅做数值范围比较，适合私有结构扫描后的候选地址过滤。

Arguments:

    ModuleCache - 可选模块缓存；为 NULL 时走原有一次性查询函数。
    CallbackAddress - 输入回调函数地址。
    ModulePath - 输出模块路径。
    ModulePathChars - 模块路径缓冲区容量。
    ModuleBaseOut - 可选输出模块基址。
    ModuleSizeOut - 可选输出模块大小。

Return Value:

    命中返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND；缓存初始化失败返回
    对应 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG moduleIndex = 0UL;

    if (ModulePath == NULL || ModulePathChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    ModulePath[0] = L'\0';
    if (ModuleBaseOut != NULL) {
        *ModuleBaseOut = 0ULL;
    }
    if (ModuleSizeOut != NULL) {
        *ModuleSizeOut = 0UL;
    }
    if (CallbackAddress == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (ModuleCache == NULL) {
        return KswordArkCallbackEnumResolveModuleByAddress(
            CallbackAddress,
            ModulePath,
            ModulePathChars,
            ModuleBaseOut,
            ModuleSizeOut);
    }

    status = KswordArkCallbackEnumEnsureModuleCache(ModuleCache);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    for (moduleIndex = 0UL; moduleIndex < ModuleCache->ModuleInfo->NumberOfModules; ++moduleIndex) {
        const KSWORD_ARK_CALLBACK_MODULE_ENTRY* moduleEntry =
            &ModuleCache->ModuleInfo->Modules[moduleIndex];
        const ULONG64 moduleBase = (ULONG64)(ULONG_PTR)moduleEntry->ImageBase;
        const ULONG64 moduleEnd = moduleBase + (ULONG64)moduleEntry->ImageSize;
        if (CallbackAddress < moduleBase || CallbackAddress >= moduleEnd) {
            continue;
        }

        if (ModuleBaseOut != NULL) {
            *ModuleBaseOut = moduleBase;
        }
        if (ModuleSizeOut != NULL) {
            *ModuleSizeOut = moduleEntry->ImageSize;
        }
        KswordArkCallbackEnumCopyAnsiPathToWide(
            ModulePath,
            ModulePathChars,
            moduleEntry->FullPathName,
            RTL_NUMBER_OF(moduleEntry->FullPathName));
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

BOOLEAN
KswordArkCallbackEnumIsKernelModuleAddress(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG64 CandidateAddress
    )
/*++

Routine Description:

    判断候选地址是否位于任一已加载内核模块。中文说明：私有结构字段扫描会产生
    多个指针候选，本过滤用于优先保留真正代码地址。

Arguments:

    ModuleCache - 模块缓存。
    CandidateAddress - 候选地址。

Return Value:

    位于模块范围返回 TRUE；否则返回 FALSE。

--*/
{
    WCHAR modulePath[4];

    RtlZeroMemory(modulePath, sizeof(modulePath));
    return NT_SUCCESS(KswordArkCallbackEnumResolveModuleByAddressCached(
        ModuleCache,
        CandidateAddress,
        modulePath,
        RTL_NUMBER_OF(modulePath),
        NULL,
        NULL));
}

static BOOLEAN
KswordArkCallbackEnumReadMemory(
    _In_ const VOID* SourceAddress,
    _Out_writes_bytes_(BytesToRead) VOID* DestinationBuffer,
    _In_ SIZE_T BytesToRead
    )
/*++

Routine Description:

    带异常保护地读取内核内存。中文说明：私有回调数组和链表没有公开同步契约，
    因此所有字段读取都必须短路径、边界化并能承受无效地址。

Arguments:

    SourceAddress - 输入源地址。
    DestinationBuffer - 输出缓冲区。
    BytesToRead - 读取字节数。

Return Value:

    读取成功返回 TRUE；地址无效、参数错误或异常返回 FALSE。

--*/
{
    if (SourceAddress == NULL || DestinationBuffer == NULL || BytesToRead == 0U) {
        return FALSE;
    }
    if (!MmIsAddressValid((PVOID)SourceAddress)) {
        return FALSE;
    }
    if (BytesToRead > sizeof(ULONG_PTR) && !MmIsAddressValid((PUCHAR)SourceAddress + BytesToRead - 1U)) {
        return FALSE;
    }

    __try {
        RtlCopyMemory(DestinationBuffer, SourceAddress, BytesToRead);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(DestinationBuffer, BytesToRead);
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
KswordArkCallbackEnumReadUchar(
    _In_ ULONG64 Address,
    _Out_ UCHAR* ValueOut
    )
/*++

Routine Description:

    读取一个 UCHAR。中文说明：用于机器码模式匹配，所有访问均经过异常保护。

Arguments:

    Address - 输入地址。
    ValueOut - 输出字节。

Return Value:

    成功返回 TRUE；失败返回 FALSE。

--*/
{
    UCHAR value = 0U;

    if (ValueOut == NULL) {
        return FALSE;
    }
    if (!KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)Address, &value, sizeof(value))) {
        *ValueOut = 0U;
        return FALSE;
    }
    *ValueOut = value;
    return TRUE;
}

static BOOLEAN
KswordArkCallbackEnumReadPointer(
    _In_ ULONG64 Address,
    _Out_ ULONG64* ValueOut
    )
/*++

Routine Description:

    读取一个指针宽度的值。中文说明：回调数组槽、链表字段和对象类型字段均通过
    该函数读取，避免裸解引用私有地址。

Arguments:

    Address - 输入指针值所在地址。
    ValueOut - 输出读取到的指针值。

Return Value:

    成功返回 TRUE；失败返回 FALSE。

--*/
{
    ULONG_PTR value = 0U;

    if (ValueOut == NULL) {
        return FALSE;
    }
    if (!KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)Address, &value, sizeof(value))) {
        *ValueOut = 0ULL;
        return FALSE;
    }
    *ValueOut = (ULONG64)value;
    return TRUE;
}

static BOOLEAN
KswordArkCallbackEnumReadUlong(
    _In_ ULONG64 Address,
    _Out_ ULONG* ValueOut
    )
/*++

Routine Description:

    读取 ULONG 值。中文说明：用于读取对象回调 operation 掩码、notify enable mask
    等诊断字段。

Arguments:

    Address - 输入字段地址。
    ValueOut - 输出 ULONG。

Return Value:

    成功返回 TRUE；失败返回 FALSE。

--*/
{
    ULONG value = 0UL;

    if (ValueOut == NULL) {
        return FALSE;
    }
    if (!KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)Address, &value, sizeof(value))) {
        *ValueOut = 0UL;
        return FALSE;
    }
    *ValueOut = value;
    return TRUE;
}

static BOOLEAN
KswordArkCallbackEnumReadListEntry(
    _In_ ULONG64 Address,
    _Out_ LIST_ENTRY* ListEntryOut
    )
/*++

Routine Description:

    读取 LIST_ENTRY。中文说明：注册表和对象回调链表遍历只读取 Flink/Blink，
    不修改链表内容。

Arguments:

    Address - 输入 LIST_ENTRY 地址。
    ListEntryOut - 输出链表项。

Return Value:

    成功返回 TRUE；失败返回 FALSE。

--*/
{
    if (ListEntryOut == NULL) {
        return FALSE;
    }
    return KswordArkCallbackEnumReadMemory(
        (PVOID)(ULONG_PTR)Address,
        ListEntryOut,
        sizeof(*ListEntryOut));
}

static BOOLEAN
KswordArkCallbackEnumReadUnicodeString(
    _In_ ULONG64 Address,
    _Out_ UNICODE_STRING* UnicodeStringOut
    )
/*++

Routine Description:

    读取 UNICODE_STRING 描述符。中文说明：仅复制描述符本身，实际字符串缓冲会在
    复制函数中再次做边界检查。

Arguments:

    Address - 输入 UNICODE_STRING 地址。
    UnicodeStringOut - 输出描述符。

Return Value:

    成功返回 TRUE；失败返回 FALSE。

--*/
{
    if (UnicodeStringOut == NULL) {
        return FALSE;
    }
    return KswordArkCallbackEnumReadMemory(
        (PVOID)(ULONG_PTR)Address,
        UnicodeStringOut,
        sizeof(*UnicodeStringOut));
}

static BOOLEAN
KswordArkCallbackEnumLooksLikeKernelPointer(
    _In_ ULONG64 CandidateAddress
    )
/*++

Routine Description:

    对候选内核指针做快速形态检查。中文说明：该函数只做地址范围和对齐检查，
    真正代码指针还需要模块表命中过滤。

Arguments:

    CandidateAddress - 候选地址。

Return Value:

    看起来像内核指针返回 TRUE；否则返回 FALSE。

--*/
{
    if (CandidateAddress == 0ULL) {
        return FALSE;
    }
    if (CandidateAddress < (ULONG64)(ULONG_PTR)MmUserProbeAddress) {
        return FALSE;
    }
    return TRUE;
}

static ULONG64
KswordArkCallbackEnumResolveRelativeAddress(
    _In_ ULONG64 InstructionAddress,
    _In_ ULONG DisplacementOffset
    )
/*++

Routine Description:

    解析 x64 RIP-relative/call 相对地址。中文说明：兼容 SKT64 的实现语义，
    结果为 InstructionAddress + Offset + sizeof(INT32) + disp32。

Arguments:

    InstructionAddress - 指令起始地址。
    DisplacementOffset - disp32 在指令中的偏移。

Return Value:

    成功返回解析出的绝对地址；读取失败返回 0。

--*/
{
    LONG displacement = 0L;

    if (InstructionAddress == 0ULL) {
        return 0ULL;
    }
    if (!KswordArkCallbackEnumReadMemory(
        (PVOID)(ULONG_PTR)(InstructionAddress + DisplacementOffset),
        &displacement,
        sizeof(displacement))) {
        return 0ULL;
    }

    return InstructionAddress + (ULONG64)DisplacementOffset + sizeof(LONG) + (LONG64)displacement;
}

static PVOID
KswordArkCallbackEnumGetSystemRoutine(
    _In_z_ PCWSTR RoutineName
    )
/*++

Routine Description:

    解析 ntoskrnl 导出例程。中文说明：用 MmGetSystemRoutineAddress 获取公开导出，
    再从导出函数代码定位私有全局变量。

Arguments:

    RoutineName - 输入导出例程名。

Return Value:

    成功返回例程地址；失败返回 NULL。

--*/
{
    UNICODE_STRING routineNameString;

    RtlInitUnicodeString(&routineNameString, RoutineName);
    return MmGetSystemRoutineAddress(&routineNameString);
}

static BOOLEAN
KswordArkCallbackEnumFindCodePattern(
    _In_ ULONG64 StartAddress,
    _In_ ULONG ScanBytes,
    _In_reads_bytes_(PatternBytes) const UCHAR* Pattern,
    _In_reads_bytes_(PatternBytes) const UCHAR* Mask,
    _In_ ULONG PatternBytes,
    _Out_ ULONG64* MatchAddressOut
    )
/*++

Routine Description:

    在指定代码窗口内查找字节模式。中文说明：Mask 中非零字节表示必须精确匹配，
    零字节表示通配；读取失败时跳过当前候选。

Arguments:

    StartAddress - 扫描起始地址。
    ScanBytes - 最大扫描长度。
    Pattern - 模式字节数组。
    Mask - 掩码字节数组。
    PatternBytes - 模式长度。
    MatchAddressOut - 输出命中地址。

Return Value:

    命中返回 TRUE；未命中返回 FALSE。

--*/
{
    ULONG offset = 0UL;
    ULONG patternIndex = 0UL;

    if (MatchAddressOut == NULL) {
        return FALSE;
    }
    *MatchAddressOut = 0ULL;
    if (StartAddress == 0ULL || Pattern == NULL || Mask == NULL || PatternBytes == 0UL || ScanBytes < PatternBytes) {
        return FALSE;
    }

    for (offset = 0UL; offset <= ScanBytes - PatternBytes; ++offset) {
        BOOLEAN matched = TRUE;
        for (patternIndex = 0UL; patternIndex < PatternBytes; ++patternIndex) {
            UCHAR value = 0U;
            if (!KswordArkCallbackEnumReadUchar(StartAddress + offset + patternIndex, &value)) {
                matched = FALSE;
                break;
            }
            if (Mask[patternIndex] != 0U && value != Pattern[patternIndex]) {
                matched = FALSE;
                break;
            }
        }
        if (matched) {
            *MatchAddressOut = StartAddress + offset;
            return TRUE;
        }
    }

    return FALSE;
}

_Must_inspect_result_
static NTSTATUS
KswordArkCallbackEnumResolveModuleByAddress(
    _In_ ULONG64 CallbackAddress,
    _Out_writes_(ModulePathChars) PWCHAR ModulePath,
    _In_ ULONG ModulePathChars,
    _Out_opt_ ULONG64* ModuleBaseOut,
    _Out_opt_ ULONG* ModuleSizeOut
    )
/*++

Routine Description:

    根据回调地址解析所属内核模块。中文说明：实现只读取系统模块列表，不
    解引用回调地址本身，因此适合诊断类只读枚举路径。

Arguments:

    CallbackAddress - 输入回调函数地址。
    ModulePath - 输出模块路径。
    ModulePathChars - 模块路径缓冲区容量。
    ModuleBaseOut - 可选输出模块基址。
    ModuleSizeOut - 可选输出模块大小。

Return Value:

    成功解析返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND；查询失败返回
    对应 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBytes = 0UL;
    ULONG moduleIndex = 0UL;
    KSWORD_ARK_CALLBACK_MODULE_INFORMATION* moduleInfo = NULL;

    if (ModulePath == NULL || ModulePathChars == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    ModulePath[0] = L'\0';
    if (ModuleBaseOut != NULL) {
        *ModuleBaseOut = 0ULL;
    }
    if (ModuleSizeOut != NULL) {
        *ModuleSizeOut = 0UL;
    }
    if (CallbackAddress == 0ULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ZwQuerySystemInformation(SystemModuleInformation, NULL, 0UL, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH || requiredBytes == 0UL) {
        return STATUS_UNSUCCESSFUL;
    }

    moduleInfo = (KSWORD_ARK_CALLBACK_MODULE_INFORMATION*)KswordArkAllocateNonPaged(
        requiredBytes,
        KSWORD_ARK_CALLBACK_ENUM_TAG);
    if (moduleInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySystemInformation(SystemModuleInformation, moduleInfo, requiredBytes, &requiredBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePool(moduleInfo);
        return status;
    }

    for (moduleIndex = 0UL; moduleIndex < moduleInfo->NumberOfModules; ++moduleIndex) {
        const KSWORD_ARK_CALLBACK_MODULE_ENTRY* moduleEntry = &moduleInfo->Modules[moduleIndex];
        const ULONG64 moduleBase = (ULONG64)(ULONG_PTR)moduleEntry->ImageBase;
        const ULONG64 moduleEnd = moduleBase + (ULONG64)moduleEntry->ImageSize;
        if (CallbackAddress < moduleBase || CallbackAddress >= moduleEnd) {
            continue;
        }

        if (ModuleBaseOut != NULL) {
            *ModuleBaseOut = moduleBase;
        }
        if (ModuleSizeOut != NULL) {
            *ModuleSizeOut = moduleEntry->ImageSize;
        }
        KswordArkCallbackEnumCopyAnsiPathToWide(
            ModulePath,
            ModulePathChars,
            moduleEntry->FullPathName,
            RTL_NUMBER_OF(moduleEntry->FullPathName));
        ExFreePool(moduleInfo);
        return STATUS_SUCCESS;
    }

    ExFreePool(moduleInfo);
    return STATUS_NOT_FOUND;
}

KSWORD_ARK_CALLBACK_ENUM_ENTRY*
KswordArkCallbackEnumReserveEntry(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    if (Builder == NULL || Builder->Response == NULL) {
        return NULL;
    }

    Builder->TotalCount += 1UL;
    if (Builder->ReturnedCount >= Builder->EntryCapacity) {
        Builder->Flags |= KSWORD_ARK_ENUM_CALLBACK_RESPONSE_FLAG_TRUNCATED;
        return NULL;
    }

    entry = &Builder->Response->entries[Builder->ReturnedCount];
    Builder->ReturnedCount += 1UL;
    RtlZeroMemory(entry, sizeof(*entry));
    entry->size = sizeof(*entry);
    return entry;
}

static VOID
KswordArkCallbackEnumFinalizeModule(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_ENTRY* Entry
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Entry == NULL || Entry->callbackAddress == 0ULL) {
        return;
    }

    status = KswordArkCallbackEnumResolveModuleByAddress(
        Entry->callbackAddress,
        Entry->modulePath,
        RTL_NUMBER_OF(Entry->modulePath),
        &Entry->moduleBase,
        &Entry->moduleSize);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_MODULE;
    }
}

VOID
KswordArkCallbackEnumFinalizeModuleCached(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_ENTRY* Entry
    )
/*++

Routine Description:

    使用模块缓存补全回调记录的模块字段。中文说明：私有扫描会产生大量记录，
    缓存版可减少系统模块查询次数。

Arguments:

    ModuleCache - 模块缓存。
    Entry - 输入输出回调枚举行。

Return Value:

    无返回值。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Entry == NULL || Entry->callbackAddress == 0ULL) {
        return;
    }

    status = KswordArkCallbackEnumResolveModuleByAddressCached(
        ModuleCache,
        Entry->callbackAddress,
        Entry->modulePath,
        RTL_NUMBER_OF(Entry->modulePath),
        &Entry->moduleBase,
        &Entry->moduleSize);
    if (NT_SUCCESS(status)) {
        Entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_MODULE;
    }
}

static VOID
KswordArkCallbackEnumCopyUnicodeSafe(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_ const UNICODE_STRING* Source
    )
/*++

Routine Description:

    带异常保护地复制私有结构中的 UNICODE_STRING 字符串。中文说明：注册表和对象
    回调 altitude 来自私有链表，复制时限制最大长度并验证缓冲地址。

Arguments:

    Destination - 输出固定宽字符缓冲。
    DestinationChars - 输出缓冲容量。
    Source - 输入 UNICODE_STRING 描述符。

Return Value:

    无返回值。

--*/
{
    USHORT copyBytes = 0U;

    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }
    Destination[0] = L'\0';
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0U) {
        return;
    }
    if (Source->Length > Source->MaximumLength && Source->MaximumLength != 0U) {
        return;
    }
    if (Source->Length > (USHORT)((DestinationChars - 1UL) * sizeof(WCHAR))) {
        copyBytes = (USHORT)((DestinationChars - 1UL) * sizeof(WCHAR));
    }
    else {
        copyBytes = Source->Length;
    }
    if (copyBytes == 0U) {
        return;
    }
    if (!MmIsAddressValid(Source->Buffer) ||
        !MmIsAddressValid((PUCHAR)Source->Buffer + copyBytes - sizeof(WCHAR))) {
        return;
    }

    __try {
        RtlCopyMemory(Destination, Source->Buffer, copyBytes);
        Destination[copyBytes / sizeof(WCHAR)] = L'\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Destination[0] = L'\0';
    }
}

static VOID
KswordArkCallbackEnumAddSelfRow(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _In_ ULONG RegisteredMask,
    _In_ ULONG RequiredMask,
    _In_ ULONG CallbackClass,
    _In_ ULONG OperationMask,
    _In_ ULONG ObjectTypeMask,
    _In_ ULONG64 CallbackAddress,
    _In_ ULONG64 ContextAddress,
    _In_ ULONG64 RegistrationAddress,
    _In_opt_z_ PCWSTR NameText,
    _In_opt_z_ PCWSTR AltitudeText,
    _In_opt_z_ PCWSTR DetailText
    )
/*++

Routine Description:

    写入 Ksword 自身注册的回调记录。中文说明：这些地址来自本驱动编译单元，
    不需要扫描系统私有链表即可准确展示当前 Ksword runtime 是否在线。

Arguments:

    Builder - 枚举响应构建器。
    RegisteredMask - runtime 中的已注册回调位图。
    RequiredMask - 当前记录对应的必需位。
    CallbackClass - 回调类别。
    OperationMask - 回调操作掩码。
    ObjectTypeMask - 对象类型掩码。
    CallbackAddress - 回调函数地址。
    ContextAddress - 回调上下文地址。
    RegistrationAddress - cookie 或 registration handle。
    NameText - 展示名称。
    AltitudeText - 可选 altitude 文本。
    DetailText - 详情文本。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = CallbackClass;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_KSWORD_SELF;
    entry->status = ((RegisteredMask & RequiredMask) != 0UL)
        ? KSWORD_ARK_CALLBACK_ENUM_STATUS_OK
        : KSWORD_ARK_CALLBACK_ENUM_STATUS_NOT_REGISTERED;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_OWNED_BY_KSWORD;
    entry->operationMask = OperationMask;
    entry->objectTypeMask = ObjectTypeMask;
    entry->callbackAddress = CallbackAddress;
    entry->contextAddress = ContextAddress;
    entry->registrationAddress = RegistrationAddress;

    if (CallbackAddress != 0ULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS;
    }
    if (ContextAddress != 0ULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS;
    }
    if (RegistrationAddress != 0ULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS;
    }
    if (OperationMask != 0UL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_OPERATION_MASK;
    }
    if (ObjectTypeMask != 0UL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_OBJECT_TYPE_MASK;
    }
    if (NameText != NULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME;
        KswordArkCallbackEnumCopyWide(entry->name, RTL_NUMBER_OF(entry->name), NameText);
    }
    if (AltitudeText != NULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_ALTITUDE;
        KswordArkCallbackEnumCopyWide(entry->altitude, RTL_NUMBER_OF(entry->altitude), AltitudeText);
    }

    KswordArkCallbackEnumCopyWide(entry->detail, RTL_NUMBER_OF(entry->detail), DetailText);
    KswordArkCallbackEnumFinalizeModule(entry);
}

VOID
KswordArkCallbackEnumAddUnsupportedRow(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _In_ ULONG CallbackClass,
    _In_opt_z_ PCWSTR NameText,
    _In_opt_z_ PCWSTR DetailText
    )
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = CallbackClass;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED;
    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED;
    entry->lastStatus = STATUS_NOT_SUPPORTED;
    if (NameText != NULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME;
        KswordArkCallbackEnumCopyWide(entry->name, RTL_NUMBER_OF(entry->name), NameText);
    }
    KswordArkCallbackEnumCopyWide(entry->detail, RTL_NUMBER_OF(entry->detail), DetailText);
}

static VOID
KswordArkCallbackEnumAddSelfCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    枚举 KswordARK 自身注册到系统中的回调。中文说明：runtime 持有注册位图、
    registry cookie 和 Ob registration handle，因此这些行能准确反映驱动状态。

Arguments:

    Builder - 枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    ULONG registeredMask = 0UL;
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime();
    const ULONG64 contextAddress = (ULONG64)(ULONG_PTR)runtime;

    if (runtime != NULL) {
        registeredMask = runtime->RegisteredCallbacksMask;
    }

    KswordArkCallbackEnumAddSelfRow(
        Builder,
        registeredMask,
        KSWORD_ARK_CALLBACK_REGISTERED_REGISTRY,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY,
        KSWORD_ARK_REG_OP_ALL,
        0UL,
        (ULONG64)(ULONG_PTR)KswordArkRegistryCallback,
        contextAddress,
        (runtime != NULL) ? (ULONG64)runtime->RegistryCookie.QuadPart : 0ULL,
        L"KswordArkRegistryCallback",
        L"385201.5141",
        L"CmRegisterCallbackEx 注册表回调；外部 CmCallbackListHead 私有链表由“私有结构枚举”阶段另行展示。");

    KswordArkCallbackEnumAddSelfRow(
        Builder,
        registeredMask,
        KSWORD_ARK_CALLBACK_REGISTERED_PROCESS,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS,
        KSWORD_ARK_PROCESS_OP_CREATE,
        0UL,
        (ULONG64)(ULONG_PTR)KswordArkProcessCreateNotifyEx,
        contextAddress,
        0ULL,
        L"KswordArkProcessCreateNotifyEx",
        NULL,
        L"PsSetCreateProcessNotifyRoutineEx 进程创建回调；外部 Psp notify 数组由“私有结构枚举”阶段另行展示。");

    KswordArkCallbackEnumAddSelfRow(
        Builder,
        registeredMask,
        KSWORD_ARK_CALLBACK_REGISTERED_THREAD,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD,
        KSWORD_ARK_THREAD_OP_CREATE | KSWORD_ARK_THREAD_OP_EXIT,
        0UL,
        (ULONG64)(ULONG_PTR)KswordArkThreadCreateNotify,
        contextAddress,
        0ULL,
        L"KswordArkThreadCreateNotify",
        NULL,
        L"PsSetCreateThreadNotifyRoutine 线程创建/退出回调；外部 Psp notify 数组由“私有结构枚举”阶段另行展示。");

    KswordArkCallbackEnumAddSelfRow(
        Builder,
        registeredMask,
        KSWORD_ARK_CALLBACK_REGISTERED_IMAGE,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE,
        KSWORD_ARK_IMAGE_OP_LOAD,
        0UL,
        (ULONG64)(ULONG_PTR)KswordArkLoadImageNotify,
        contextAddress,
        0ULL,
        L"KswordArkLoadImageNotify",
        NULL,
        L"PsSetLoadImageNotifyRoutine 镜像加载回调；外部 Psp notify 数组由“私有结构枚举”阶段另行展示。");

    KswordArkCallbackEnumAddSelfRow(
        Builder,
        registeredMask,
        KSWORD_ARK_CALLBACK_REGISTERED_OBJECT,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT,
        KSWORD_ARK_OBJECT_OP_HANDLE_CREATE | KSWORD_ARK_OBJECT_OP_HANDLE_DUPLICATE,
        KSWORD_ARK_OBJECT_OP_TYPE_PROCESS | KSWORD_ARK_OBJECT_OP_TYPE_THREAD,
        (ULONG64)(ULONG_PTR)KswordArkObjectPreOperation,
        contextAddress,
        (runtime != NULL) ? (ULONG64)(ULONG_PTR)runtime->ObRegistrationHandle : 0ULL,
        L"KswordArkObjectPreOperation",
        L"385201.5142",
        L"ObRegisterCallbacks 对象句柄回调；仅覆盖 Process/Thread Handle Create/Duplicate。");

    KswordArkCallbackEnumAddSelfRow(
        Builder,
        registeredMask,
        KSWORD_ARK_CALLBACK_REGISTERED_MINIFILTER,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER,
        KSWORD_ARK_MINIFILTER_OP_ALL,
        0UL,
        (ULONG64)(ULONG_PTR)KswordArkMinifilterPreOperation,
        contextAddress,
        (runtime != NULL) ? (ULONG64)(ULONG_PTR)runtime->MiniFilterHandle : 0ULL,
        L"KswordArkMinifilterPreOperation",
        L"385210",
        L"FltRegisterFilter 文件系统微过滤器回调；同时服务文件监控和自定义回调规则。");
}

static VOID
KswordArkCallbackEnumAddMinifilterRow(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _In_ PFLT_FILTER FilterObject
    )
/*++

Routine Description:

    查询并写入一个 Filter Manager filter。中文说明：Filter Manager 提供公开枚举
    API，这部分能稳定遍历系统中已注册的 minifilter 对象。

Arguments:

    Builder - 枚举响应构建器。
    FilterObject - FltEnumerateFilters 返回并持有引用的 filter 对象。

Return Value:

    无返回值。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesReturned = 0UL;
    FILTER_AGGREGATE_STANDARD_INFORMATION* filterInfo = NULL;
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;
    UNICODE_STRING nameString;
    UNICODE_STRING altitudeString;

    RtlZeroMemory(&nameString, sizeof(nameString));
    RtlZeroMemory(&altitudeString, sizeof(altitudeString));

    status = FltGetFilterInformation(
        FilterObject,
        FilterAggregateStandardInformation,
        NULL,
        0UL,
        &bytesReturned);
    if (status == STATUS_BUFFER_TOO_SMALL && bytesReturned >= sizeof(FILTER_AGGREGATE_STANDARD_INFORMATION)) {
        filterInfo = (FILTER_AGGREGATE_STANDARD_INFORMATION*)KswordArkAllocateNonPaged(
            bytesReturned,
            KSWORD_ARK_CALLBACK_ENUM_TAG);
        if (filterInfo != NULL) {
            RtlZeroMemory(filterInfo, bytesReturned);
            status = FltGetFilterInformation(
                FilterObject,
                FilterAggregateStandardInformation,
                filterInfo,
                bytesReturned,
                &bytesReturned);
        }
        else {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        if (filterInfo != NULL) {
            ExFreePool(filterInfo);
        }
        return;
    }

    entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION;
    entry->callbackAddress = (ULONG64)(ULONG_PTR)FilterObject;
    entry->registrationAddress = (ULONG64)(ULONG_PTR)FilterObject;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE;
    entry->lastStatus = status;

    if (NT_SUCCESS(status) && filterInfo != NULL && ((filterInfo->Flags & FLTFL_ASI_IS_MINIFILTER) != 0UL)) {
        entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
        entry->contextAddress = ((ULONG64)filterInfo->Type.MiniFilter.FrameID << 32) |
            (ULONG64)filterInfo->Type.MiniFilter.NumberOfInstances;
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS;

        if (filterInfo->Type.MiniFilter.FilterNameBufferOffset != 0U &&
            filterInfo->Type.MiniFilter.FilterNameLength != 0U) {
            nameString.Buffer = (PWCHAR)((PUCHAR)filterInfo + filterInfo->Type.MiniFilter.FilterNameBufferOffset);
            nameString.Length = filterInfo->Type.MiniFilter.FilterNameLength;
            nameString.MaximumLength = filterInfo->Type.MiniFilter.FilterNameLength;
            KswordArkCallbackEnumCopyUnicode(entry->name, RTL_NUMBER_OF(entry->name), &nameString);
        }

        if (filterInfo->Type.MiniFilter.FilterAltitudeBufferOffset != 0U &&
            filterInfo->Type.MiniFilter.FilterAltitudeLength != 0U) {
            altitudeString.Buffer = (PWCHAR)((PUCHAR)filterInfo + filterInfo->Type.MiniFilter.FilterAltitudeBufferOffset);
            altitudeString.Length = filterInfo->Type.MiniFilter.FilterAltitudeLength;
            altitudeString.MaximumLength = filterInfo->Type.MiniFilter.FilterAltitudeLength;
            entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_ALTITUDE;
            KswordArkCallbackEnumCopyUnicode(entry->altitude, RTL_NUMBER_OF(entry->altitude), &altitudeString);
        }

        (VOID)RtlStringCbPrintfW(
            entry->detail,
            sizeof(entry->detail),
            L"Filter Manager minifilter；FrameID=%lu，实例数=%lu，FilterObject=0x%p。",
            (unsigned long)filterInfo->Type.MiniFilter.FrameID,
            (unsigned long)filterInfo->Type.MiniFilter.NumberOfInstances,
            FilterObject);
        ExFreePool(filterInfo);
        return;
    }

    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED;
    KswordArkCallbackEnumCopyWide(entry->name, RTL_NUMBER_OF(entry->name), L"<FltGetFilterInformation failed>");
    (VOID)RtlStringCbPrintfW(
        entry->detail,
        sizeof(entry->detail),
        L"FltGetFilterInformation(FilterAggregateStandardInformation) 失败，NTSTATUS=0x%08lX。",
        (unsigned long)status);
    if (filterInfo != NULL) {
        ExFreePool(filterInfo);
    }
}

VOID
KswordArkCallbackEnumAddMinifilters(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    通过 Filter Manager 公共 API 枚举当前所有 minifilter。中文说明：函数使用
    两次 FltEnumerateFilters 获取数量和对象数组，并在处理后释放每个引用。

Arguments:

    Builder - 枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG filterCount = 0UL;
    ULONG filterIndex = 0UL;
    PFLT_FILTER* filterList = NULL;
    SIZE_T allocationBytes = 0U;

    status = FltEnumerateFilters(NULL, 0UL, &filterCount);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_SUCCESS) {
        Builder->LastStatus = status;
        KswordArkCallbackEnumAddUnsupportedRow(
            Builder,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER,
            L"Minifilter enumeration failed",
            L"FltEnumerateFilters 长度探测失败，无法枚举 minifilter。");
        return;
    }

    if (filterCount == 0UL) {
        KswordArkCallbackEnumAddUnsupportedRow(
            Builder,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER,
            L"Minifilter",
            L"FltEnumerateFilters 返回 0 个 minifilter。");
        return;
    }

    allocationBytes = (SIZE_T)filterCount * sizeof(PFLT_FILTER);
    filterList = (PFLT_FILTER*)KswordArkAllocateNonPaged(allocationBytes, KSWORD_ARK_CALLBACK_ENUM_TAG);
    if (filterList == NULL) {
        Builder->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
        return;
    }
    RtlZeroMemory(filterList, allocationBytes);

    status = FltEnumerateFilters(filterList, filterCount, &filterCount);
    if (!NT_SUCCESS(status)) {
        Builder->LastStatus = status;
        ExFreePool(filterList);
        KswordArkCallbackEnumAddUnsupportedRow(
            Builder,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER,
            L"Minifilter enumeration failed",
            L"FltEnumerateFilters 返回错误，当前无法展示 minifilter 列表。");
        return;
    }

    for (filterIndex = 0UL; filterIndex < filterCount; ++filterIndex) {
        if (filterList[filterIndex] != NULL) {
            KswordArkCallbackEnumAddMinifilterRow(Builder, filterList[filterIndex]);
            FltObjectDereference(filterList[filterIndex]);
        }
    }

    ExFreePool(filterList);
}

static VOID
KswordArkCallbackEnumAddLocateRow(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _In_ ULONG CallbackClass,
    _In_opt_z_ PCWSTR NameText,
    _In_ ULONG64 LocatedAddress,
    _In_ NTSTATUS LocateStatus,
    _In_opt_z_ PCWSTR DetailText
    )
/*++

Routine Description:

    写入一个私有全局定位诊断行。中文说明：定位行帮助 R3 判断本机内核版本上
    SKT64 风格特征是否命中，并显示全局数组或链表头地址。

Arguments:

    Builder - 枚举响应构建器。
    CallbackClass - 回调类别。
    NameText - 展示名称。
    LocatedAddress - 定位到的全局地址。
    LocateStatus - 定位状态。
    DetailText - 详情文本。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = CallbackClass;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN;
    entry->status = NT_SUCCESS(LocateStatus)
        ? KSWORD_ARK_CALLBACK_ENUM_STATUS_OK
        : KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED;
    entry->lastStatus = LocateStatus;
    entry->registrationAddress = LocatedAddress;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME;
    if (LocatedAddress != 0ULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS;
    }
    KswordArkCallbackEnumCopyWide(entry->name, RTL_NUMBER_OF(entry->name), NameText);
    KswordArkCallbackEnumCopyWide(entry->detail, RTL_NUMBER_OF(entry->detail), DetailText);
}

static NTSTATUS
KswordArkCallbackEnumLocatePspCreateProcessNotifyRoutine(
    _Out_ ULONG64* ArrayAddressOut
    )
/*++

Routine Description:

    定位 PspCreateProcessNotifyRoutine 私有数组。中文说明：实现复用 SKT64 思路，
    先从 PsSetCreateProcessNotifyRoutine 找到内部 PspSet* 调用，再在内部函数中
    查找 4C 8D rip-relative 数组地址。

Arguments:

    ArrayAddressOut - 输出 notify 数组地址。

Return Value:

    成功返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND。

--*/
{
    ULONG64 exportAddress = (ULONG64)(ULONG_PTR)KswordArkCallbackEnumGetSystemRoutine(L"PsSetCreateProcessNotifyRoutine");
    ULONG64 innerRoutine = 0ULL;
    ULONG64 matchAddress = 0ULL;
    ULONG64 arrayAddress = 0ULL;
    static const UCHAR callPattern[] = { 0xE8U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U };
    static const UCHAR callMask[] = { 1U, 0U, 0U, 0U, 0U, 1U };
    static const UCHAR leaPattern[] = { 0x4CU, 0x8DU };
    static const UCHAR leaMask[] = { 1U, 1U };

    if (ArrayAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ArrayAddressOut = 0ULL;
    if (exportAddress == 0ULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(
        exportAddress,
        KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES,
        callPattern,
        callMask,
        sizeof(callPattern),
        &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    innerRoutine = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 1UL);
    if (innerRoutine == 0ULL || !MmIsAddressValid((PVOID)(ULONG_PTR)innerRoutine)) {
        return STATUS_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(
        innerRoutine,
        KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES,
        leaPattern,
        leaMask,
        sizeof(leaPattern),
        &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    arrayAddress = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 3UL);
    if (arrayAddress == 0ULL || !MmIsAddressValid((PVOID)(ULONG_PTR)arrayAddress)) {
        return STATUS_NOT_FOUND;
    }

    *ArrayAddressOut = arrayAddress;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordArkCallbackEnumLocatePspCreateThreadNotifyRoutine(
    _Out_ ULONG64* ArrayAddressOut
    )
/*++

Routine Description:

    定位 PspCreateThreadNotifyRoutine 私有数组。中文说明：SKT64 使用
    PsRemoveCreateThreadNotifyRoutine 中的 48 8D 0D rip-relative 引用。

Arguments:

    ArrayAddressOut - 输出 notify 数组地址。

Return Value:

    成功返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND。

--*/
{
    ULONG64 exportAddress = (ULONG64)(ULONG_PTR)KswordArkCallbackEnumGetSystemRoutine(L"PsRemoveCreateThreadNotifyRoutine");
    ULONG64 matchAddress = 0ULL;
    ULONG64 arrayAddress = 0ULL;
    static const UCHAR leaPattern[] = { 0x48U, 0x8DU, 0x0DU };
    static const UCHAR leaMask[] = { 1U, 1U, 1U };

    if (ArrayAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ArrayAddressOut = 0ULL;
    if (exportAddress == 0ULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(
        exportAddress,
        KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES,
        leaPattern,
        leaMask,
        sizeof(leaPattern),
        &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    arrayAddress = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 3UL);
    if (arrayAddress == 0ULL || !MmIsAddressValid((PVOID)(ULONG_PTR)arrayAddress)) {
        return STATUS_NOT_FOUND;
    }

    *ArrayAddressOut = arrayAddress;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordArkCallbackEnumLocatePspLoadImageNotifyRoutine(
    _Out_ ULONG64* ArrayAddressOut
    )
/*++

Routine Description:

    定位 PspLoadImageNotifyRoutine 私有数组。中文说明：优先使用
    PsSetLoadImageNotifyRoutineEx；如果导出缺失，再回退 PsSetLoadImageNotifyRoutine。

Arguments:

    ArrayAddressOut - 输出 notify 数组地址。

Return Value:

    成功返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND。

--*/
{
    ULONG64 exportAddress = (ULONG64)(ULONG_PTR)KswordArkCallbackEnumGetSystemRoutine(L"PsSetLoadImageNotifyRoutineEx");
    ULONG64 matchAddress = 0ULL;
    ULONG64 arrayAddress = 0ULL;
    static const UCHAR leaPattern[] = { 0x48U, 0x8DU, 0x0DU };
    static const UCHAR leaMask[] = { 1U, 1U, 1U };

    if (ArrayAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ArrayAddressOut = 0ULL;
    if (exportAddress == 0ULL) {
        exportAddress = (ULONG64)(ULONG_PTR)KswordArkCallbackEnumGetSystemRoutine(L"PsSetLoadImageNotifyRoutine");
    }
    if (exportAddress == 0ULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(
        exportAddress,
        KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES,
        leaPattern,
        leaMask,
        sizeof(leaPattern),
        &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    arrayAddress = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 3UL);
    if (arrayAddress == 0ULL || !MmIsAddressValid((PVOID)(ULONG_PTR)arrayAddress)) {
        return STATUS_NOT_FOUND;
    }

    *ArrayAddressOut = arrayAddress;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordArkCallbackEnumLocatePspNotifyEnableMask(
    _Out_ ULONG64* MaskAddressOut
    )
/*++

Routine Description:

    定位 PspNotifyEnableMask 私有全局。中文说明：该值不是回调项本身，但能辅助
    判断 Psp notify 路径是否处于启用状态。

Arguments:

    MaskAddressOut - 输出 mask 地址。

Return Value:

    成功返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND。

--*/
{
    ULONG64 exportAddress = (ULONG64)(ULONG_PTR)KswordArkCallbackEnumGetSystemRoutine(L"PsSetLoadImageNotifyRoutineEx");
    ULONG64 matchAddress = 0ULL;
    ULONG64 maskAddress = 0ULL;
    static const UCHAR maskPattern[] = { 0x8BU, 0x05U, 0x00U, 0x00U, 0x00U, 0x00U, 0xA8U };
    static const UCHAR maskMask[] = { 1U, 1U, 0U, 0U, 0U, 0U, 1U };

    if (MaskAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *MaskAddressOut = 0ULL;
    if (exportAddress == 0ULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(
        exportAddress,
        KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES,
        maskPattern,
        maskMask,
        sizeof(maskPattern),
        &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    maskAddress = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 2UL);
    if (maskAddress == 0ULL || !MmIsAddressValid((PVOID)(ULONG_PTR)maskAddress)) {
        return STATUS_NOT_FOUND;
    }

    *MaskAddressOut = maskAddress;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordArkCallbackEnumLocateCmCallbackListHead(
    _Out_ ULONG64* ListHeadOut
    )
/*++

Routine Description:

    定位 CmCallbackListHead 私有链表头。中文说明：SKT64 从 CmUnRegisterCallback
    中查找 48 8D 0D rip-relative 链表头引用。

Arguments:

    ListHeadOut - 输出链表头地址。

Return Value:

    成功返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND。

--*/
{
    ULONG64 exportAddress = (ULONG64)(ULONG_PTR)KswordArkCallbackEnumGetSystemRoutine(L"CmUnRegisterCallback");
    ULONG64 matchAddress = 0ULL;
    ULONG64 listHead = 0ULL;
    static const UCHAR leaPattern[] = { 0x48U, 0x8DU, 0x0DU };
    static const UCHAR leaMask[] = { 1U, 1U, 1U };

    if (ListHeadOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ListHeadOut = 0ULL;
    if (exportAddress == 0ULL) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(
        exportAddress,
        KSWORD_ARK_CALLBACK_ENUM_PRIVATE_SCAN_BYTES,
        leaPattern,
        leaMask,
        sizeof(leaPattern),
        &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    listHead = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 3UL);
    if (listHead == 0ULL || !MmIsAddressValid((PVOID)(ULONG_PTR)listHead)) {
        return STATUS_NOT_FOUND;
    }

    *ListHeadOut = listHead;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordArkCallbackEnumLocateObpCallPreOperationCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _Out_ ULONG64* RoutineAddressOut
    )
/*++

Routine Description:

    定位 ObpCallPreOperationCallbacks 内部例程。中文说明：SKT64 从 ntoskrnl 代码
    中匹配调用点；Ksword 复用该模式并限制在内核模块镜像范围内扫描。

Arguments:

    ModuleCache - 模块缓存。
    RoutineAddressOut - 输出内部例程地址。

Return Value:

    成功返回 STATUS_SUCCESS；未命中返回 STATUS_NOT_FOUND。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG64 ntBase = 0ULL;
    ULONG ntSize = 0UL;
    ULONG64 matchAddress = 0ULL;
    ULONG64 routineAddress = 0ULL;
    static const UCHAR pattern[] = {
        0xE8U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x85U, 0xC0U, 0x78U, 0x00U,
        0x45U, 0x84U, 0x00U, 0x75U, 0x00U, 0x8BU
    };
    static const UCHAR mask[] = {
        1U, 0U, 0U, 0U, 0U,
        1U, 1U, 1U, 0U,
        1U, 1U, 0U, 1U, 0U, 1U
    };

    if (RoutineAddressOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *RoutineAddressOut = 0ULL;

    status = KswordArkCallbackEnumEnsureModuleCache(ModuleCache);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (ModuleCache->ModuleInfo == NULL || ModuleCache->ModuleInfo->NumberOfModules == 0UL) {
        return STATUS_NOT_FOUND;
    }

    ntBase = (ULONG64)(ULONG_PTR)ModuleCache->ModuleInfo->Modules[0].ImageBase;
    ntSize = ModuleCache->ModuleInfo->Modules[0].ImageSize;
    if (ntBase == 0ULL || ntSize < sizeof(pattern)) {
        return STATUS_NOT_FOUND;
    }

    if (!KswordArkCallbackEnumFindCodePattern(ntBase, ntSize, pattern, mask, sizeof(pattern), &matchAddress)) {
        return STATUS_NOT_FOUND;
    }

    routineAddress = KswordArkCallbackEnumResolveRelativeAddress(matchAddress, 1UL);
    if (routineAddress == 0ULL || !KswordArkCallbackEnumIsKernelModuleAddress(ModuleCache, routineAddress)) {
        return STATUS_NOT_FOUND;
    }

    *RoutineAddressOut = routineAddress;
    return STATUS_SUCCESS;
}

static VOID
KswordArkCallbackEnumAddNotifyArrayEntry(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG CallbackClass,
    _In_ ULONG OperationMask,
    _In_ ULONG SlotIndex,
    _In_ ULONG64 SlotAddress,
    _In_ ULONG64 FastRefValue,
    _In_ ULONG64 RoutineBlock,
    _In_ ULONG64 FunctionAddress,
    _In_ ULONG64 ContextAddress,
    _In_opt_z_ PCWSTR NamePrefix
    )
/*++

Routine Description:

    写入一个 Psp notify 数组项。中文说明：数组槽保存 EX_FAST_REF，低 4 位是
    引用计数，清除低位后得到 EX_CALLBACK_ROUTINE_BLOCK，再读取 Function/Context。

Arguments:

    Builder - 枚举响应构建器。
    ModuleCache - 模块缓存。
    CallbackClass - 回调类别。
    OperationMask - 操作掩码。
    SlotIndex - 数组槽索引。
    SlotAddress - 数组槽地址。
    FastRefValue - 原始 EX_FAST_REF 值。
    RoutineBlock - 解码后的 routine block 地址。
    FunctionAddress - 回调函数地址。
    ContextAddress - 回调上下文地址。
    NamePrefix - 行名称前缀。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = CallbackClass;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY;
    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_OPERATION_MASK;
    entry->operationMask = OperationMask;
    entry->callbackAddress = FunctionAddress;
    entry->contextAddress = ContextAddress;
    entry->registrationAddress = SlotAddress;

    (VOID)RtlStringCbPrintfW(
        entry->name,
        sizeof(entry->name),
        L"%ws[%lu]",
        (NamePrefix != NULL) ? NamePrefix : L"PspNotify",
        (unsigned long)SlotIndex);
    (VOID)RtlStringCbPrintfW(
        entry->detail,
        sizeof(entry->detail),
        L"Psp notify 私有数组项；slot=0x%p，EX_FAST_REF=0x%llX，RoutineBlock=0x%p，Function=0x%p，Context=0x%p。",
        (PVOID)(ULONG_PTR)SlotAddress,
        FastRefValue,
        (PVOID)(ULONG_PTR)RoutineBlock,
        (PVOID)(ULONG_PTR)FunctionAddress,
        (PVOID)(ULONG_PTR)ContextAddress);
    KswordArkCallbackEnumFinalizeModuleCached(ModuleCache, entry);
}

static ULONG
KswordArkCallbackEnumAddNotifyArray(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG CallbackClass,
    _In_ ULONG OperationMask,
    _In_ ULONG64 ArrayAddress,
    _In_opt_z_ PCWSTR NamePrefix
    )
/*++

Routine Description:

    遍历 Psp notify 私有数组。中文说明：该函数只读 EX_FAST_REF 数组，不调用
    移除 API，不改写任何槽位；每个候选函数必须命中内核模块表才展示。

Arguments:

    Builder - 枚举响应构建器。
    ModuleCache - 模块缓存。
    CallbackClass - 回调类别。
    OperationMask - 操作掩码。
    ArrayAddress - 私有数组地址。
    NamePrefix - 行名称前缀。

Return Value:

    返回枚举到的有效回调数量。

--*/
{
    ULONG slotIndex = 0UL;
    ULONG addedCount = 0UL;

    if (ArrayAddress == 0ULL) {
        return 0UL;
    }

    for (slotIndex = 0UL; slotIndex < KSWORD_ARK_CALLBACK_ENUM_NOTIFY_SLOT_COUNT; ++slotIndex) {
        ULONG64 slotAddress = ArrayAddress + ((ULONG64)slotIndex * sizeof(ULONG_PTR));
        ULONG64 fastRefValue = 0ULL;
        ULONG64 routineBlock = 0ULL;
        ULONG64 functionAddress = 0ULL;
        ULONG64 contextAddress = 0ULL;

        if (!KswordArkCallbackEnumReadPointer(slotAddress, &fastRefValue)) {
            continue;
        }
        if (fastRefValue == 0ULL) {
            continue;
        }

        routineBlock = fastRefValue & (ULONG64)KSWORD_ARK_CALLBACK_ENUM_FAST_REF_MASK;
        if (!KswordArkCallbackEnumLooksLikeKernelPointer(routineBlock)) {
            continue;
        }
        if (!KswordArkCallbackEnumReadPointer(routineBlock + sizeof(ULONG_PTR), &functionAddress)) {
            continue;
        }
        if (!KswordArkCallbackEnumReadPointer(routineBlock + (2ULL * sizeof(ULONG_PTR)), &contextAddress)) {
            contextAddress = 0ULL;
        }
        if (!KswordArkCallbackEnumLooksLikeKernelPointer(functionAddress)) {
            continue;
        }
        if (!KswordArkCallbackEnumIsKernelModuleAddress(ModuleCache, functionAddress)) {
            continue;
        }

        KswordArkCallbackEnumAddNotifyArrayEntry(
            Builder,
            ModuleCache,
            CallbackClass,
            OperationMask,
            slotIndex,
            slotAddress,
            fastRefValue,
            routineBlock,
            functionAddress,
            contextAddress,
            NamePrefix);
        addedCount += 1UL;
    }

    return addedCount;
}

static VOID
KswordArkCallbackEnumAddRegistryEntry(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG EntryIndex,
    _In_ ULONG64 EntryAddress,
    _In_ ULONG64 FunctionAddress,
    _In_ ULONG64 ContextAddress,
    _In_ const UNICODE_STRING* AltitudeString
    )
/*++

Routine Description:

    写入一个 Cm registry callback 链表项。中文说明：不同 Windows 版本的私有
    结构存在漂移，因此本函数只写入已经被模块表验证过的 Function 候选。

Arguments:

    Builder - 枚举响应构建器。
    ModuleCache - 模块缓存。
    EntryIndex - 链表序号。
    EntryAddress - 链表节点地址。
    FunctionAddress - 回调函数地址。
    ContextAddress - 回调上下文地址。
    AltitudeString - 可选 altitude 描述符。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST;
    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_OPERATION_MASK;
    entry->operationMask = KSWORD_ARK_REG_OP_ALL;
    entry->callbackAddress = FunctionAddress;
    entry->contextAddress = ContextAddress;
    entry->registrationAddress = EntryAddress;

    (VOID)RtlStringCbPrintfW(
        entry->name,
        sizeof(entry->name),
        L"CmCallback[%lu]",
        (unsigned long)EntryIndex);
    if (AltitudeString != NULL) {
        KswordArkCallbackEnumCopyUnicodeSafe(
            entry->altitude,
            RTL_NUMBER_OF(entry->altitude),
            AltitudeString);
        if (entry->altitude[0] != L'\0') {
            entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_ALTITUDE;
        }
    }
    (VOID)RtlStringCbPrintfW(
        entry->detail,
        sizeof(entry->detail),
        L"CmCallbackListHead 私有链表项；Entry=0x%p，Function=0x%p，Context=0x%p。",
        (PVOID)(ULONG_PTR)EntryAddress,
        (PVOID)(ULONG_PTR)FunctionAddress,
        (PVOID)(ULONG_PTR)ContextAddress);
    KswordArkCallbackEnumFinalizeModuleCached(ModuleCache, entry);
}

static BOOLEAN
KswordArkCallbackEnumFindRegistryFields(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG64 EntryAddress,
    _Out_ ULONG64* FunctionAddressOut,
    _Out_ ULONG64* ContextAddressOut,
    _Out_ UNICODE_STRING* AltitudeStringOut
    )
/*++

Routine Description:

    从 Cm callback 私有节点中启发式识别 Function/Context/Altitude 字段。中文说明：
    优先选择能解析到内核模块的指针作为函数地址，再在其邻近字段寻找上下文和
    altitude 描述符。

Arguments:

    ModuleCache - 模块缓存。
    EntryAddress - 链表节点地址。
    FunctionAddressOut - 输出函数地址。
    ContextAddressOut - 输出上下文地址。
    AltitudeStringOut - 输出 altitude 描述符。

Return Value:

    成功识别函数地址返回 TRUE；否则返回 FALSE。

--*/
{
    LONG offset = 0L;
    ULONG64 functionAddress = 0ULL;
    ULONG64 contextAddress = 0ULL;
    UNICODE_STRING altitudeString;

    RtlZeroMemory(&altitudeString, sizeof(altitudeString));
    if (FunctionAddressOut == NULL || ContextAddressOut == NULL || AltitudeStringOut == NULL) {
        return FALSE;
    }
    *FunctionAddressOut = 0ULL;
    *ContextAddressOut = 0ULL;
    RtlZeroMemory(AltitudeStringOut, sizeof(*AltitudeStringOut));

    for (offset = 0x10L; offset <= 0x100L; offset += (LONG)sizeof(ULONG_PTR)) {
        ULONG64 candidate = 0ULL;
        if (!KswordArkCallbackEnumReadPointer(EntryAddress + (ULONG64)offset, &candidate)) {
            continue;
        }
        if (!KswordArkCallbackEnumLooksLikeKernelPointer(candidate)) {
            continue;
        }
        if (!KswordArkCallbackEnumIsKernelModuleAddress(ModuleCache, candidate)) {
            continue;
        }

        functionAddress = candidate;
        if (offset >= (LONG)sizeof(ULONG_PTR)) {
            (VOID)KswordArkCallbackEnumReadPointer(
                EntryAddress + (ULONG64)(offset - (LONG)sizeof(ULONG_PTR)),
                &contextAddress);
        }
        if (contextAddress == 0ULL) {
            (VOID)KswordArkCallbackEnumReadPointer(
                EntryAddress + (ULONG64)(offset + (LONG)sizeof(ULONG_PTR)),
                &contextAddress);
        }
        break;
    }

    if (functionAddress == 0ULL) {
        return FALSE;
    }

    for (offset = 0x10L; offset <= 0x120L; offset += (LONG)sizeof(USHORT)) {
        UNICODE_STRING candidateString;
        RtlZeroMemory(&candidateString, sizeof(candidateString));
        if (!KswordArkCallbackEnumReadUnicodeString(EntryAddress + (ULONG64)offset, &candidateString)) {
            continue;
        }
        if (candidateString.Buffer == NULL ||
            candidateString.Length == 0U ||
            candidateString.Length > 128U ||
            candidateString.MaximumLength < candidateString.Length ||
            (candidateString.Length % sizeof(WCHAR)) != 0U) {
            continue;
        }
        if (!MmIsAddressValid(candidateString.Buffer)) {
            continue;
        }

        altitudeString = candidateString;
        break;
    }

    *FunctionAddressOut = functionAddress;
    *ContextAddressOut = contextAddress;
    *AltitudeStringOut = altitudeString;
    return TRUE;
}

static ULONG
KswordArkCallbackEnumAddRegistryList(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG64 ListHeadAddress
    )
/*++

Routine Description:

    遍历 CmCallbackListHead 私有链表。中文说明：链表遍历设定最大节点数并校验
    LIST_ENTRY 指针，避免私有结构异常导致长循环。

Arguments:

    Builder - 枚举响应构建器。
    ModuleCache - 模块缓存。
    ListHeadAddress - 链表头地址。

Return Value:

    返回枚举到的有效注册表回调数量。

--*/
{
    LIST_ENTRY listHead;
    ULONG index = 0UL;
    ULONG addedCount = 0UL;
    ULONG64 currentAddress = 0ULL;

    RtlZeroMemory(&listHead, sizeof(listHead));
    if (ListHeadAddress == 0ULL ||
        !KswordArkCallbackEnumReadListEntry(ListHeadAddress, &listHead)) {
        return 0UL;
    }

    currentAddress = (ULONG64)(ULONG_PTR)listHead.Flink;
    while (currentAddress != 0ULL &&
        currentAddress != ListHeadAddress &&
        index < KSWORD_ARK_CALLBACK_ENUM_LIST_WALK_LIMIT) {
        LIST_ENTRY currentEntry;
        ULONG64 functionAddress = 0ULL;
        ULONG64 contextAddress = 0ULL;
        UNICODE_STRING altitudeString;

        RtlZeroMemory(&currentEntry, sizeof(currentEntry));
        RtlZeroMemory(&altitudeString, sizeof(altitudeString));
        if (!KswordArkCallbackEnumReadListEntry(currentAddress, &currentEntry)) {
            break;
        }
        if (currentEntry.Flink == NULL || currentEntry.Blink == NULL) {
            break;
        }

        if (KswordArkCallbackEnumFindRegistryFields(
            ModuleCache,
            currentAddress,
            &functionAddress,
            &contextAddress,
            &altitudeString)) {
            KswordArkCallbackEnumAddRegistryEntry(
                Builder,
                ModuleCache,
                index,
                currentAddress,
                functionAddress,
                contextAddress,
                &altitudeString);
            addedCount += 1UL;
        }

        currentAddress = (ULONG64)(ULONG_PTR)currentEntry.Flink;
        index += 1UL;
    }

    return addedCount;
}

static BOOLEAN
KswordArkCallbackEnumObjectNodeLooksValid(
    _In_ ULONG64 NodeAddress
    )
/*++

Routine Description:

    对对象回调链表节点做基础校验。中文说明：对象类型私有链表没有公开结构，
    这里只校验 LIST_ENTRY 形态，真正回调函数再由模块表确认。

Arguments:

    NodeAddress - 候选 LIST_ENTRY 地址。

Return Value:

    看起来可遍历返回 TRUE；否则返回 FALSE。

--*/
{
    LIST_ENTRY entry;

    RtlZeroMemory(&entry, sizeof(entry));
    if (NodeAddress == 0ULL || !KswordArkCallbackEnumReadListEntry(NodeAddress, &entry)) {
        return FALSE;
    }
    if (entry.Flink == NULL || entry.Blink == NULL) {
        return FALSE;
    }
    if ((ULONG64)(ULONG_PTR)entry.Flink < (ULONG64)(ULONG_PTR)MmUserProbeAddress ||
        (ULONG64)(ULONG_PTR)entry.Blink < (ULONG64)(ULONG_PTR)MmUserProbeAddress) {
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN
KswordArkCallbackEnumFindObjectTypeCallbackListHead(
    _In_ POBJECT_TYPE ObjectType,
    _Out_ ULONG64* ListHeadAddressOut
    )
/*++

Routine Description:

    在 OBJECT_TYPE 私有结构中寻找 CallbackList 链表头。中文说明：该结构版本相关，
    因此函数只在对象类型前若干字节中寻找满足“双向链表且非空”的候选头。

Arguments:

    ObjectType - 输入对象类型指针。
    ListHeadAddressOut - 输出链表头地址。

Return Value:

    找到非空链表头返回 TRUE；否则返回 FALSE。

--*/
{
    ULONG offset = 0UL;
    ULONG64 objectTypeAddress = (ULONG64)(ULONG_PTR)ObjectType;

    if (ListHeadAddressOut == NULL) {
        return FALSE;
    }
    *ListHeadAddressOut = 0ULL;
    if (ObjectType == NULL || objectTypeAddress == 0ULL) {
        return FALSE;
    }

    for (offset = 0x40UL; offset < KSWORD_ARK_CALLBACK_ENUM_OBJECT_TYPE_SCAN_BYTES; offset += (ULONG)sizeof(ULONG_PTR)) {
        ULONG64 headAddress = objectTypeAddress + offset;
        LIST_ENTRY headEntry;
        ULONG64 flinkAddress = 0ULL;
        ULONG64 blinkAddress = 0ULL;
        LIST_ENTRY firstEntry;

        RtlZeroMemory(&headEntry, sizeof(headEntry));
        RtlZeroMemory(&firstEntry, sizeof(firstEntry));
        if (!KswordArkCallbackEnumReadListEntry(headAddress, &headEntry)) {
            continue;
        }

        flinkAddress = (ULONG64)(ULONG_PTR)headEntry.Flink;
        blinkAddress = (ULONG64)(ULONG_PTR)headEntry.Blink;
        if (flinkAddress == 0ULL || blinkAddress == 0ULL || flinkAddress == headAddress) {
            continue;
        }
        if (!KswordArkCallbackEnumObjectNodeLooksValid(flinkAddress)) {
            continue;
        }
        if (!KswordArkCallbackEnumReadListEntry(flinkAddress, &firstEntry)) {
            continue;
        }
        if ((ULONG64)(ULONG_PTR)firstEntry.Blink != headAddress &&
            (ULONG64)(ULONG_PTR)headEntry.Blink != flinkAddress) {
            continue;
        }

        *ListHeadAddressOut = headAddress;
        return TRUE;
    }

    return FALSE;
}

static BOOLEAN
KswordArkCallbackEnumFindObjectCallbackFields(
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG64 NodeAddress,
    _Out_ KSWORD_ARK_CALLBACK_ENUM_OBJECT_SCAN_RESULT* ResultOut
    )
/*++

Routine Description:

    从对象回调私有节点中识别 Pre/PostOperation、Operations 和 Registration 字段。
    中文说明：Pre/PostOperation 指针必须落在已加载内核模块；Operations 掩码在
    邻近 ULONG 字段中启发式读取。

Arguments:

    ModuleCache - 模块缓存。
    NodeAddress - 链表节点地址。
    ResultOut - 输出识别结果。

Return Value:

    找到至少一个回调函数返回 TRUE；否则返回 FALSE。

--*/
{
    LONG offset = 0L;
    KSWORD_ARK_CALLBACK_ENUM_OBJECT_SCAN_RESULT result;

    RtlZeroMemory(&result, sizeof(result));
    if (ResultOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(ResultOut, sizeof(*ResultOut));

    for (offset = KSWORD_ARK_CALLBACK_ENUM_POINTER_SCAN_BACK_BYTES * -1L;
        offset <= KSWORD_ARK_CALLBACK_ENUM_POINTER_SCAN_FORWARD_BYTES;
        offset += (LONG)sizeof(ULONG_PTR)) {
        ULONG64 fieldAddress = 0ULL;
        ULONG64 candidate = 0ULL;

        if (offset < 0L && NodeAddress < (ULONG64)(-offset)) {
            continue;
        }
        fieldAddress = (offset < 0L)
            ? NodeAddress - (ULONG64)(-offset)
            : NodeAddress + (ULONG64)offset;
        if (!KswordArkCallbackEnumReadPointer(fieldAddress, &candidate)) {
            continue;
        }
        if (!KswordArkCallbackEnumLooksLikeKernelPointer(candidate)) {
            continue;
        }
        if (!KswordArkCallbackEnumIsKernelModuleAddress(ModuleCache, candidate)) {
            continue;
        }

        if (result.PreOperation == 0ULL) {
            result.PreOperation = candidate;
        }
        else if (result.PostOperation == 0ULL && candidate != result.PreOperation) {
            result.PostOperation = candidate;
            break;
        }
    }

    if (result.PreOperation == 0ULL && result.PostOperation == 0ULL) {
        return FALSE;
    }

    for (offset = -0x40L; offset <= 0x80L; offset += (LONG)sizeof(ULONG)) {
        ULONG candidateMask = 0UL;
        ULONG64 fieldAddress = 0ULL;
        if (offset < 0L && NodeAddress < (ULONG64)(-offset)) {
            continue;
        }
        fieldAddress = (offset < 0L)
            ? NodeAddress - (ULONG64)(-offset)
            : NodeAddress + (ULONG64)offset;
        if (!KswordArkCallbackEnumReadUlong(fieldAddress, &candidateMask)) {
            continue;
        }
        if ((candidateMask & (OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE)) != 0UL &&
            (candidateMask & ~(OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE | 0xFFFF0000UL)) == 0UL) {
            result.OperationMask = candidateMask & (OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE);
            break;
        }
    }
    if (result.OperationMask == 0UL) {
        result.OperationMask = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    }

    for (offset = KSWORD_ARK_CALLBACK_ENUM_POINTER_SCAN_BACK_BYTES * -1L;
        offset <= KSWORD_ARK_CALLBACK_ENUM_POINTER_SCAN_FORWARD_BYTES;
        offset += (LONG)sizeof(ULONG_PTR)) {
        ULONG64 fieldAddress = 0ULL;
        ULONG64 candidate = 0ULL;
        if (offset < 0L && NodeAddress < (ULONG64)(-offset)) {
            continue;
        }
        fieldAddress = (offset < 0L)
            ? NodeAddress - (ULONG64)(-offset)
            : NodeAddress + (ULONG64)offset;
        if (!KswordArkCallbackEnumReadPointer(fieldAddress, &candidate)) {
            continue;
        }
        if (candidate == 0ULL ||
            candidate == result.PreOperation ||
            candidate == result.PostOperation) {
            continue;
        }
        if (KswordArkCallbackEnumLooksLikeKernelPointer(candidate) && !KswordArkCallbackEnumIsKernelModuleAddress(ModuleCache, candidate)) {
            result.RegistrationBlock = candidate;
            break;
        }
    }

    *ResultOut = result;
    return TRUE;
}

static VOID
KswordArkCallbackEnumAddObjectCallbackEntry(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG EntryIndex,
    _In_ ULONG ObjectTypeMask,
    _In_z_ PCWSTR ObjectTypeName,
    _In_ ULONG64 NodeAddress,
    _In_ ULONG64 CallbackAddress,
    _In_ ULONG OperationMask,
    _In_ ULONG64 RegistrationBlock,
    _In_ BOOLEAN IsPostOperation
    )
/*++

Routine Description:

    写入一个 ObRegisterCallbacks 私有链表候选。中文说明：同一个节点可能有 Pre
    和 Post 两个函数，函数地址命中模块表后分别展示。

Arguments:

    Builder - 枚举响应构建器。
    ModuleCache - 模块缓存。
    EntryIndex - 链表序号。
    ObjectTypeMask - 对象类型掩码。
    ObjectTypeName - 对象类型展示名。
    NodeAddress - 链表节点地址。
    CallbackAddress - 回调函数地址。
    OperationMask - Ob operation 掩码。
    RegistrationBlock - 注册块候选地址。
    IsPostOperation - TRUE 表示 PostOperation；FALSE 表示 PreOperation。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST;
    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_OPERATION_MASK |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_OBJECT_TYPE_MASK;
    entry->operationMask = OperationMask;
    entry->objectTypeMask = ObjectTypeMask;
    entry->callbackAddress = CallbackAddress;
    entry->registrationAddress = NodeAddress;
    entry->contextAddress = RegistrationBlock;
    if (RegistrationBlock != 0ULL) {
        entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS;
    }

    (VOID)RtlStringCbPrintfW(
        entry->name,
        sizeof(entry->name),
        L"Ob%wsCallback[%ws:%lu]",
        IsPostOperation ? L"Post" : L"Pre",
        ObjectTypeName,
        (unsigned long)EntryIndex);
    (VOID)RtlStringCbPrintfW(
        entry->detail,
        sizeof(entry->detail),
        L"OBJECT_TYPE CallbackList 私有链表候选；ObjectType=%ws，Node=0x%p，%wsOperation=0x%p，Operations=0x%lX，RegistrationBlock=0x%p。",
        ObjectTypeName,
        (PVOID)(ULONG_PTR)NodeAddress,
        IsPostOperation ? L"Post" : L"Pre",
        (PVOID)(ULONG_PTR)CallbackAddress,
        (unsigned long)OperationMask,
        (PVOID)(ULONG_PTR)RegistrationBlock);
    KswordArkCallbackEnumFinalizeModuleCached(ModuleCache, entry);
}

static ULONG
KswordArkCallbackEnumAddObjectTypeCallbackList(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ POBJECT_TYPE ObjectType,
    _In_ ULONG ObjectTypeMask,
    _In_z_ PCWSTR ObjectTypeName
    )
/*++

Routine Description:

    枚举一个对象类型的 Ob callback 私有链表。中文说明：优先从 OBJECT_TYPE 中
    启发式找到 CallbackList 头，再遍历有限节点并解析 Pre/Post 回调函数。

Arguments:

    Builder - 枚举响应构建器。
    ModuleCache - 模块缓存。
    ObjectType - 目标对象类型。
    ObjectTypeMask - 对象类型掩码。
    ObjectTypeName - 对象类型文本。

Return Value:

    返回枚举到的有效对象回调函数数量。

--*/
{
    ULONG64 listHeadAddress = 0ULL;
    LIST_ENTRY listHead;
    ULONG index = 0UL;
    ULONG addedCount = 0UL;
    ULONG64 currentAddress = 0ULL;

    RtlZeroMemory(&listHead, sizeof(listHead));
    if (!KswordArkCallbackEnumFindObjectTypeCallbackListHead(ObjectType, &listHeadAddress)) {
        return 0UL;
    }
    if (!KswordArkCallbackEnumReadListEntry(listHeadAddress, &listHead)) {
        return 0UL;
    }

    currentAddress = (ULONG64)(ULONG_PTR)listHead.Flink;
    while (currentAddress != 0ULL &&
        currentAddress != listHeadAddress &&
        index < KSWORD_ARK_CALLBACK_ENUM_LIST_WALK_LIMIT) {
        LIST_ENTRY currentEntry;
        KSWORD_ARK_CALLBACK_ENUM_OBJECT_SCAN_RESULT scanResult;

        RtlZeroMemory(&currentEntry, sizeof(currentEntry));
        RtlZeroMemory(&scanResult, sizeof(scanResult));
        if (!KswordArkCallbackEnumReadListEntry(currentAddress, &currentEntry)) {
            break;
        }
        if (!KswordArkCallbackEnumFindObjectCallbackFields(ModuleCache, currentAddress, &scanResult)) {
            currentAddress = (ULONG64)(ULONG_PTR)currentEntry.Flink;
            index += 1UL;
            continue;
        }

        if (scanResult.PreOperation != 0ULL) {
            KswordArkCallbackEnumAddObjectCallbackEntry(
                Builder,
                ModuleCache,
                index,
                ObjectTypeMask,
                ObjectTypeName,
                currentAddress,
                scanResult.PreOperation,
                scanResult.OperationMask,
                scanResult.RegistrationBlock,
                FALSE);
            addedCount += 1UL;
        }
        if (scanResult.PostOperation != 0ULL) {
            KswordArkCallbackEnumAddObjectCallbackEntry(
                Builder,
                ModuleCache,
                index,
                ObjectTypeMask,
                ObjectTypeName,
                currentAddress,
                scanResult.PostOperation,
                scanResult.OperationMask,
                scanResult.RegistrationBlock,
                TRUE);
            addedCount += 1UL;
        }

        currentAddress = (ULONG64)(ULONG_PTR)currentEntry.Flink;
        index += 1UL;
    }

    return addedCount;
}

VOID
KswordArkCallbackEnumAddPrivateCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    枚举系统私有回调结构。中文说明：实现参考 SKT64 的特征定位方式，只读遍历
    Psp notify 数组、Cm 回调链表和 Process/Thread OBJECT_TYPE 回调链表。

Arguments:

    Builder - 枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_MODULE_CACHE moduleCache;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG64 processArray = 0ULL;
    ULONG64 threadArray = 0ULL;
    ULONG64 imageArray = 0ULL;
    ULONG64 notifyEnableMask = 0ULL;
    ULONG64 cmListHead = 0ULL;
    ULONG64 obpCallPreOperationCallbacks = 0ULL;
    ULONG addedCount = 0UL;
    ULONG notifyMaskValue = 0UL;

    KswordArkCallbackEnumInitModuleCache(&moduleCache);

    status = KswordArkCallbackEnumLocatePspNotifyEnableMask(&notifyEnableMask);
    if (NT_SUCCESS(status)) {
        (VOID)KswordArkCallbackEnumReadUlong(notifyEnableMask, &notifyMaskValue);
    }
    KswordArkCallbackEnumAddLocateRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS,
        L"PspNotifyEnableMask",
        notifyEnableMask,
        status,
        NT_SUCCESS(status)
            ? L"已定位 PspNotifyEnableMask；用于诊断进程/线程/镜像 notify 全局启用状态。"
            : L"未能定位 PspNotifyEnableMask；不影响后续数组特征扫描。");

    status = KswordArkCallbackEnumLocatePspCreateProcessNotifyRoutine(&processArray);
    KswordArkCallbackEnumAddLocateRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS,
        L"PspCreateProcessNotifyRoutine",
        processArray,
        status,
        NT_SUCCESS(status)
            ? L"已定位 PspCreateProcessNotifyRoutine 私有数组，开始遍历 EX_FAST_REF 槽。"
            : L"未能通过 PsSetCreateProcessNotifyRoutine 特征定位进程 notify 数组。");
    if (NT_SUCCESS(status)) {
        addedCount = KswordArkCallbackEnumAddNotifyArray(
            Builder,
            &moduleCache,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS,
            KSWORD_ARK_PROCESS_OP_CREATE,
            processArray,
            L"PspCreateProcessNotifyRoutine");
        if (addedCount == 0UL) {
            KswordArkCallbackEnumAddUnsupportedRow(
                Builder,
                KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS,
                L"PspCreateProcessNotifyRoutine empty",
                L"已定位进程 notify 数组，但未找到能解析到模块的有效 EX_CALLBACK_ROUTINE_BLOCK。");
        }
    }

    status = KswordArkCallbackEnumLocatePspCreateThreadNotifyRoutine(&threadArray);
    KswordArkCallbackEnumAddLocateRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD,
        L"PspCreateThreadNotifyRoutine",
        threadArray,
        status,
        NT_SUCCESS(status)
            ? L"已定位 PspCreateThreadNotifyRoutine 私有数组，开始遍历 EX_FAST_REF 槽。"
            : L"未能通过 PsRemoveCreateThreadNotifyRoutine 特征定位线程 notify 数组。");
    if (NT_SUCCESS(status)) {
        addedCount = KswordArkCallbackEnumAddNotifyArray(
            Builder,
            &moduleCache,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD,
            KSWORD_ARK_THREAD_OP_CREATE | KSWORD_ARK_THREAD_OP_EXIT,
            threadArray,
            L"PspCreateThreadNotifyRoutine");
        if (addedCount == 0UL) {
            KswordArkCallbackEnumAddUnsupportedRow(
                Builder,
                KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD,
                L"PspCreateThreadNotifyRoutine empty",
                L"已定位线程 notify 数组，但未找到能解析到模块的有效 EX_CALLBACK_ROUTINE_BLOCK。");
        }
    }

    status = KswordArkCallbackEnumLocatePspLoadImageNotifyRoutine(&imageArray);
    KswordArkCallbackEnumAddLocateRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE,
        L"PspLoadImageNotifyRoutine",
        imageArray,
        status,
        NT_SUCCESS(status)
            ? L"已定位 PspLoadImageNotifyRoutine 私有数组，开始遍历 EX_FAST_REF 槽。"
            : L"未能通过 PsSetLoadImageNotifyRoutineEx 特征定位镜像 notify 数组。");
    if (NT_SUCCESS(status)) {
        addedCount = KswordArkCallbackEnumAddNotifyArray(
            Builder,
            &moduleCache,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE,
            KSWORD_ARK_IMAGE_OP_LOAD,
            imageArray,
            L"PspLoadImageNotifyRoutine");
        if (addedCount == 0UL) {
            KswordArkCallbackEnumAddUnsupportedRow(
                Builder,
                KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE,
                L"PspLoadImageNotifyRoutine empty",
                L"已定位镜像 notify 数组，但未找到能解析到模块的有效 EX_CALLBACK_ROUTINE_BLOCK。");
        }
    }

    status = KswordArkCallbackEnumLocateCmCallbackListHead(&cmListHead);
    KswordArkCallbackEnumAddLocateRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY,
        L"CmCallbackListHead",
        cmListHead,
        status,
        NT_SUCCESS(status)
            ? L"已定位 CmCallbackListHead 私有链表，开始保守遍历并识别 Function/Altitude。"
            : L"未能通过 CmUnRegisterCallback 特征定位注册表回调链表头。");
    if (NT_SUCCESS(status)) {
        addedCount = KswordArkCallbackEnumAddRegistryList(Builder, &moduleCache, cmListHead);
        if (addedCount == 0UL) {
            KswordArkCallbackEnumAddUnsupportedRow(
                Builder,
                KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY,
                L"CmCallbackListHead empty",
                L"已定位注册表回调链表头，但未找到能解析到模块的 Function 字段。");
        }
    }

    status = KswordArkCallbackEnumLocateObpCallPreOperationCallbacks(&moduleCache, &obpCallPreOperationCallbacks);
    KswordArkCallbackEnumAddLocateRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT,
        L"ObpCallPreOperationCallbacks",
        obpCallPreOperationCallbacks,
        status,
        NT_SUCCESS(status)
            ? L"已定位 ObpCallPreOperationCallbacks 内部例程；对象回调项继续从 OBJECT_TYPE CallbackList 启发式枚举。"
            : L"未能通过 ntoskrnl 特征定位 ObpCallPreOperationCallbacks；仍会尝试 OBJECT_TYPE 链表启发式枚举。");

    addedCount = 0UL;
    if (PsProcessType != NULL && *PsProcessType != NULL) {
        addedCount += KswordArkCallbackEnumAddObjectTypeCallbackList(
            Builder,
            &moduleCache,
            *PsProcessType,
            KSWORD_ARK_OBJECT_OP_TYPE_PROCESS,
            L"Process");
    }
    if (PsThreadType != NULL && *PsThreadType != NULL) {
        addedCount += KswordArkCallbackEnumAddObjectTypeCallbackList(
            Builder,
            &moduleCache,
            *PsThreadType,
            KSWORD_ARK_OBJECT_OP_TYPE_THREAD,
            L"Thread");
    }
    if (addedCount == 0UL) {
        KswordArkCallbackEnumAddUnsupportedRow(
            Builder,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT,
            L"OBJECT_TYPE CallbackList empty",
            L"未能在 Process/Thread OBJECT_TYPE 私有区域识别非空 CallbackList；对象回调结构随版本变化较大。");
    }

    if (notifyMaskValue != 0UL) {
        KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = KswordArkCallbackEnumReserveEntry(Builder);
        if (entry != NULL) {
            entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS;
            entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN;
            entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
            entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
                KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS |
                KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS;
            entry->contextAddress = notifyMaskValue;
            entry->registrationAddress = notifyEnableMask;
            KswordArkCallbackEnumCopyWide(entry->name, RTL_NUMBER_OF(entry->name), L"PspNotifyEnableMask value");
            (VOID)RtlStringCbPrintfW(
                entry->detail,
                sizeof(entry->detail),
                L"PspNotifyEnableMask=0x%08lX，Address=0x%p；该值仅用于诊断 notify 路径启用状态。",
                (unsigned long)notifyMaskValue,
                (PVOID)(ULONG_PTR)notifyEnableMask);
        }
    }

    KswordArkCallbackEnumFreeModuleCache(&moduleCache);
}

static VOID
KswordArkCallbackEnumAddSystemInformerDynDataRow(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    把 System Informer DynData 的回调相关字段写入诊断行。中文说明：当前
    Ksword 已经 vendored kphdyn 数据，本行明确展示 ETW 私有结构偏移是否命中。

Arguments:

    Builder - 枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    KSW_DYN_STATE dynState;
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED;
    entry->status = ((dynState.CapabilityMask & KSW_CAP_ETW_GUID_FIELDS) != 0ULL)
        ? KSWORD_ARK_CALLBACK_ENUM_STATUS_OK
        : KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED;
    entry->lastStatus = dynState.LastStatus;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS;
    entry->contextAddress = ((ULONG64)dynState.Kernel.EgeGuid << 32) |
        (ULONG64)dynState.Kernel.EreGuidEntry;
    KswordArkCallbackEnumCopyWide(
        entry->name,
        RTL_NUMBER_OF(entry->name),
        L"System Informer DynData: ETW offsets");
    (VOID)RtlStringCbPrintfW(
        entry->detail,
        sizeof(entry->detail),
        L"已复用 third_party/systeminformer_dyn/kphdyn 数据；NtosActive=%lu，EgeGuid=0x%08lX，EreGuidEntry=0x%08lX，cap=0x%llX。",
        (unsigned long)(dynState.NtosActive ? 1UL : 0UL),
        (unsigned long)dynState.Kernel.EgeGuid,
        (unsigned long)dynState.Kernel.EreGuidEntry,
        dynState.CapabilityMask);
}

static VOID
KswordArkCallbackEnumAddUnsupportedKinds(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    添加仍未覆盖类别的说明行。中文说明：进程、线程、镜像、注册表、对象和
    WFP 已由其它阶段处理；此处只保留 ETW 仍缺少安全全局入口的说明。

Arguments:

    Builder - 枚举响应构建器。

Return Value:

    无返回值。

--*/
{
    KswordArkCallbackEnumAddSystemInformerDynDataRow(Builder);
    KswordArkCallbackEnumAddUnsupportedRow(
        Builder,
        KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER,
        L"ETW providers/consumers",
        L"System Informer DynData 暴露 EgeGuid/EreGuidEntry 偏移，但仍需安全定位 ETW 全局表入口；当前仅标记未支持。");
}

NTSTATUS
KswordARKCallbackIoctlEnumCallbacks(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_ENUM_CALLBACKS。中文说明：该 IOCTL 只读遍历当前可稳定
    获取的回调信息，优先返回 Ksword 自身注册项和 Filter Manager minifilter 列表。

Arguments:

    Request - 当前 WDF 请求。
    InputBufferLength - 输入缓冲区长度。
    OutputBufferLength - 输出缓冲区长度。
    CompleteBytesOut - 输出实际完成字节数。

Return Value:

    成功返回 STATUS_SUCCESS；参数或缓冲区不合法返回对应 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer = NULL;
    size_t inputLength = 0U;
    size_t outputLength = 0U;
    ULONG requestFlags = KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL;
    ULONG requestMaxEntries = KSWORD_ARK_CALLBACK_ENUM_MAX_ENTRIES;
    ULONG outputCapacity = 0UL;
    KSWORD_ARK_ENUM_CALLBACKS_REQUEST* requestPacket = NULL;
    KSWORD_ARK_ENUM_CALLBACKS_RESPONSE* responsePacket = NULL;
    KSWORD_ARK_CALLBACK_ENUM_BUILDER builder;

    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    if (InputBufferLength < sizeof(KSWORD_ARK_ENUM_CALLBACKS_REQUEST) ||
        OutputBufferLength < g_KswordArkCallbackEnumHeaderBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_ENUM_CALLBACKS_REQUEST),
        &inputBuffer,
        &inputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (inputLength < sizeof(KSWORD_ARK_ENUM_CALLBACKS_REQUEST)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        g_KswordArkCallbackEnumHeaderBytes,
        (PVOID*)&responsePacket,
        &outputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (outputLength < g_KswordArkCallbackEnumHeaderBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    requestPacket = (KSWORD_ARK_ENUM_CALLBACKS_REQUEST*)inputBuffer;
    if (requestPacket->size < sizeof(KSWORD_ARK_ENUM_CALLBACKS_REQUEST) ||
        requestPacket->version != KSWORD_ARK_CALLBACK_ENUM_PROTOCOL_VERSION) {
        return STATUS_INVALID_PARAMETER;
    }

    requestFlags = requestPacket->flags;
    if (requestFlags == 0UL) {
        requestFlags = KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL;
    }
    if ((requestFlags & (~KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL)) != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (requestPacket->maxEntries != 0UL) {
        requestMaxEntries = requestPacket->maxEntries;
    }
    if (requestMaxEntries > KSWORD_ARK_CALLBACK_ENUM_MAX_ENTRIES) {
        requestMaxEntries = KSWORD_ARK_CALLBACK_ENUM_MAX_ENTRIES;
    }

    RtlZeroMemory(responsePacket, outputLength);
    responsePacket->size = sizeof(KSWORD_ARK_ENUM_CALLBACKS_RESPONSE);
    responsePacket->version = KSWORD_ARK_CALLBACK_ENUM_PROTOCOL_VERSION;
    responsePacket->entrySize = sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY);
    responsePacket->lastStatus = STATUS_SUCCESS;

    if (outputLength > g_KswordArkCallbackEnumHeaderBytes) {
        outputCapacity = (ULONG)((outputLength - g_KswordArkCallbackEnumHeaderBytes) / sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY));
    }
    if (outputCapacity > requestMaxEntries) {
        outputCapacity = requestMaxEntries;
    }

    RtlZeroMemory(&builder, sizeof(builder));
    builder.Response = responsePacket;
    builder.EntryCapacity = outputCapacity;
    builder.LastStatus = STATUS_SUCCESS;

    if ((requestFlags & KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_KSWORD_SELF) != 0UL) {
        KswordArkCallbackEnumAddSelfCallbacks(&builder);
    }
    if ((requestFlags & KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_MINIFILTERS) != 0UL) {
        KswordArkCallbackEnumAddMinifilters(&builder);
    }
    if ((requestFlags & KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_PRIVATE) != 0UL) {
        KswordArkCallbackEnumAddPrivateCallbacks(&builder);
        KswordArkCallbackExternalAddCallbacks(&builder);
    }
    if ((requestFlags & KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_UNSUPPORTED) != 0UL) {
        KswordArkCallbackEnumAddUnsupportedKinds(&builder);
    }

    responsePacket->totalCount = builder.TotalCount;
    responsePacket->returnedCount = builder.ReturnedCount;
    responsePacket->flags = builder.Flags;
    responsePacket->lastStatus = builder.LastStatus;
    *CompleteBytesOut = (size_t)g_KswordArkCallbackEnumHeaderBytes +
        ((size_t)builder.ReturnedCount * sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY));

    if ((builder.Flags & KSWORD_ARK_ENUM_CALLBACK_RESPONSE_FLAG_TRUNCATED) != 0UL) {
        return STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}
