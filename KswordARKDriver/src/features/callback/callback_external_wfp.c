/*++

Module Name:

    callback_external_wfp.c

Abstract:

    WFP callout enumeration and safe removal through public WFP management APIs.

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_external_wfp.h"
#include <ntimage.h>

#define KSWORD_ARK_WFP_ENUM_PAGE_SIZE 64U
#define KSWORD_ARK_WFP_MAX_CALLOUT_ID 0xFFFFFFFFULL

#ifndef RPC_C_AUTHN_WINNT
#define RPC_C_AUTHN_WINNT 10U
#endif

typedef struct _SEC_WINNT_AUTH_IDENTITY_W SEC_WINNT_AUTH_IDENTITY_W;

typedef struct _KSWORD_ARK_FWPM_DISPLAY_DATA0
{
    WCHAR* name;
    WCHAR* description;
} KSWORD_ARK_FWPM_DISPLAY_DATA0;

typedef struct _KSWORD_ARK_FWPM_SESSION0
{
    GUID sessionKey;
    KSWORD_ARK_FWPM_DISPLAY_DATA0 displayData;
    UINT32 flags;
    UINT32 txnWaitTimeoutInMSec;
    ULONG processId;
    SID* sid;
    WCHAR* username;
    BOOLEAN kernelMode;
} KSWORD_ARK_FWPM_SESSION0;

typedef struct _KSWORD_ARK_FWPM_CALLOUT_ENUM_TEMPLATE0
{
    GUID* providerKey;
    GUID layerKey;
} KSWORD_ARK_FWPM_CALLOUT_ENUM_TEMPLATE0;

typedef struct _KSWORD_ARK_FWPM_CALLOUT0
{
    GUID calloutKey;
    KSWORD_ARK_FWPM_DISPLAY_DATA0 displayData;
    UINT32 flags;
    GUID* providerKey;
    struct
    {
        UINT32 size;
        UINT8* data;
    } providerData;
    GUID applicableLayer;
    UINT32 calloutId;
} KSWORD_ARK_FWPM_CALLOUT0;

typedef NTSTATUS (*KSWORD_ARK_WFP_ENGINE_OPEN)(
    _In_opt_ const WCHAR* ServerName,
    _In_ UINT32 AuthnService,
    _In_opt_ SEC_WINNT_AUTH_IDENTITY_W* AuthIdentity,
    _In_opt_ const KSWORD_ARK_FWPM_SESSION0* Session,
    _Out_ HANDLE* EngineHandle);
typedef NTSTATUS (*KSWORD_ARK_WFP_ENGINE_CLOSE)(_In_ HANDLE EngineHandle);
typedef NTSTATUS (*KSWORD_ARK_WFP_CALLOUT_CREATE_ENUM_HANDLE)(
    _In_ HANDLE EngineHandle,
    _In_opt_ const KSWORD_ARK_FWPM_CALLOUT_ENUM_TEMPLATE0* EnumTemplate,
    _Out_ HANDLE* EnumHandle);
typedef NTSTATUS (*KSWORD_ARK_WFP_CALLOUT_DESTROY_ENUM_HANDLE)(
    _In_ HANDLE EngineHandle,
    _In_ HANDLE EnumHandle);
typedef NTSTATUS (*KSWORD_ARK_WFP_CALLOUT_ENUM)(
    _In_ HANDLE EngineHandle,
    _In_ HANDLE EnumHandle,
    _In_ UINT32 NumEntriesRequested,
    _Outptr_result_buffer_(*NumEntriesReturned) KSWORD_ARK_FWPM_CALLOUT0*** Entries,
    _Out_ UINT32* NumEntriesReturned);
typedef NTSTATUS (*KSWORD_ARK_WFP_CALLOUT_GET_BY_ID)(
    _In_ HANDLE EngineHandle,
    _In_ UINT32 Id,
    _Outptr_ KSWORD_ARK_FWPM_CALLOUT0** Callout);
typedef NTSTATUS (*KSWORD_ARK_WFP_CALLOUT_DELETE_BY_ID)(
    _In_ HANDLE EngineHandle,
    _In_ UINT32 Id);
typedef VOID (*KSWORD_ARK_WFP_FREE_MEMORY)(_Inout_ VOID** Pointer);

typedef struct _KSWORD_ARK_WFP_API
{
    KSWORD_ARK_WFP_ENGINE_OPEN EngineOpen;
    KSWORD_ARK_WFP_ENGINE_CLOSE EngineClose;
    KSWORD_ARK_WFP_CALLOUT_CREATE_ENUM_HANDLE CalloutCreateEnumHandle;
    KSWORD_ARK_WFP_CALLOUT_DESTROY_ENUM_HANDLE CalloutDestroyEnumHandle;
    KSWORD_ARK_WFP_CALLOUT_ENUM CalloutEnum;
    KSWORD_ARK_WFP_CALLOUT_GET_BY_ID CalloutGetById;
    KSWORD_ARK_WFP_CALLOUT_DELETE_BY_ID CalloutDeleteById;
    KSWORD_ARK_WFP_FREE_MEMORY FreeMemory;
} KSWORD_ARK_WFP_API;

static BOOLEAN
KswordArkWfpAsciiEquals(
    _In_z_ const CHAR* Left,
    _In_z_ const CHAR* Right
    )
/*++

Routine Description:

    比较两个 PE export ASCII 名称。中文说明：比较长度限制为 128 字节，
    防止异常导出表导致长循环。

Arguments:

    Left - 输入导出表中的名称。
    Right - 输入期望名称。

Return Value:

    相等返回 TRUE；否则返回 FALSE。

--*/
{
    ULONG index = 0UL;

    if (Left == NULL || Right == NULL) {
        return FALSE;
    }

    __try {
        for (index = 0UL; index < 128UL; ++index) {
            if (Left[index] != Right[index]) {
                return FALSE;
            }
            if (Left[index] == '\0') {
                return TRUE;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    return FALSE;
}

static BOOLEAN
KswordArkWfpAnsiPathContains(
    _In_reads_bytes_(TextBytes) const UCHAR* Text,
    _In_ ULONG TextBytes,
    _In_z_ const CHAR* Needle
    )
/*++

Routine Description:

    在系统模块 ANSI 路径中搜索小写子串。中文说明：只读固定模块路径字段，
    用于识别已加载的 fwpkclnt.sys。

Arguments:

    Text - 输入模块路径。
    TextBytes - 输入路径字段长度。
    Needle - 输入小写 ASCII 子串。

Return Value:

    命中返回 TRUE；否则返回 FALSE。

--*/
{
    ULONG textIndex = 0UL;

    if (Text == NULL || TextBytes == 0UL || Needle == NULL) {
        return FALSE;
    }

    for (textIndex = 0UL; textIndex < TextBytes && Text[textIndex] != '\0'; ++textIndex) {
        ULONG needleIndex = 0UL;

        for (needleIndex = 0UL; Needle[needleIndex] != '\0'; ++needleIndex) {
            UCHAR current = 0U;

            if (textIndex + needleIndex >= TextBytes) {
                return FALSE;
            }
            current = Text[textIndex + needleIndex];
            if (current == '\0') {
                return FALSE;
            }
            if (current >= 'A' && current <= 'Z') {
                current = (UCHAR)(current - 'A' + 'a');
            }
            if ((CHAR)current != Needle[needleIndex]) {
                break;
            }
        }
        if (Needle[needleIndex] == '\0') {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
KswordArkWfpRvaValid(
    _In_ ULONG Rva,
    _In_ ULONG Size,
    _In_ ULONG ImageSize
    )
/*++

Routine Description:

    校验 PE RVA 范围。中文说明：所有导出目录字段必须完全落在镜像范围内，
    溢出或空范围都视为不可用。

Arguments:

    Rva - 输入 RVA。
    Size - 输入区域大小。
    ImageSize - 输入镜像大小。

Return Value:

    范围可信返回 TRUE；否则返回 FALSE。

--*/
{
    if (Rva == 0UL || Size == 0UL || ImageSize == 0UL) {
        return FALSE;
    }
    if (Size > ImageSize || Rva >= ImageSize || Size > ImageSize - Rva) {
        return FALSE;
    }
    return TRUE;
}

static PVOID
KswordArkWfpResolveExport(
    _In_ ULONG64 ImageBase,
    _In_ ULONG ImageSize,
    _In_z_ const CHAR* RoutineName
    )
/*++

Routine Description:

    从已加载 fwpkclnt.sys 映像中解析导出。中文说明：函数只读取 PE header 和
    export directory，不跟随 forwarder，不写模块内存。

Arguments:

    ImageBase - 输入模块基址。
    ImageSize - 输入模块大小。
    RoutineName - 输入导出名称。

Return Value:

    成功返回导出地址；失败返回 NULL。

--*/
{
    PIMAGE_DOS_HEADER dosHeader = NULL;
    PIMAGE_NT_HEADERS ntHeaders = NULL;
    PIMAGE_EXPORT_DIRECTORY exportDirectory = NULL;
    ULONG exportRva = 0UL;
    ULONG exportSize = 0UL;
    ULONG nameIndex = 0UL;
    ULONG* nameArray = NULL;
    ULONG* functionArray = NULL;
    USHORT* ordinalArray = NULL;

    if (ImageBase == 0ULL || ImageSize < sizeof(IMAGE_DOS_HEADER) || RoutineName == NULL) {
        return NULL;
    }

    __try {
        dosHeader = (PIMAGE_DOS_HEADER)(ULONG_PTR)ImageBase;
        if (!MmIsAddressValid(dosHeader) || dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            return NULL;
        }
        if (dosHeader->e_lfanew <= 0 || (ULONG)dosHeader->e_lfanew > ImageSize - sizeof(IMAGE_NT_HEADERS)) {
            return NULL;
        }

        ntHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)(ULONG_PTR)ImageBase + dosHeader->e_lfanew);
        if (!MmIsAddressValid(ntHeaders) || ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
            return NULL;
        }
        if (ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
            return NULL;
        }

        exportRva = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        exportSize = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (!KswordArkWfpRvaValid(exportRva, exportSize, ImageSize)) {
            return NULL;
        }

        exportDirectory = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)(ULONG_PTR)ImageBase + exportRva);
        if (!MmIsAddressValid(exportDirectory) || exportDirectory->NumberOfNames == 0UL) {
            return NULL;
        }
        if (!KswordArkWfpRvaValid(exportDirectory->AddressOfNames, exportDirectory->NumberOfNames * sizeof(ULONG), ImageSize) ||
            !KswordArkWfpRvaValid(exportDirectory->AddressOfNameOrdinals, exportDirectory->NumberOfNames * sizeof(USHORT), ImageSize) ||
            !KswordArkWfpRvaValid(exportDirectory->AddressOfFunctions, exportDirectory->NumberOfFunctions * sizeof(ULONG), ImageSize)) {
            return NULL;
        }

        nameArray = (ULONG*)((PUCHAR)(ULONG_PTR)ImageBase + exportDirectory->AddressOfNames);
        ordinalArray = (USHORT*)((PUCHAR)(ULONG_PTR)ImageBase + exportDirectory->AddressOfNameOrdinals);
        functionArray = (ULONG*)((PUCHAR)(ULONG_PTR)ImageBase + exportDirectory->AddressOfFunctions);

        for (nameIndex = 0UL; nameIndex < exportDirectory->NumberOfNames; ++nameIndex) {
            ULONG nameRva = nameArray[nameIndex];
            USHORT ordinalIndex = ordinalArray[nameIndex];
            ULONG functionRva = 0UL;
            const CHAR* exportedName = NULL;

            if (!KswordArkWfpRvaValid(nameRva, 1UL, ImageSize) || ordinalIndex >= exportDirectory->NumberOfFunctions) {
                continue;
            }
            exportedName = (const CHAR*)((PUCHAR)(ULONG_PTR)ImageBase + nameRva);
            if (!MmIsAddressValid((PVOID)exportedName) || !KswordArkWfpAsciiEquals(exportedName, RoutineName)) {
                continue;
            }
            functionRva = functionArray[ordinalIndex];
            if (functionRva >= exportRva && functionRva < exportRva + exportSize) {
                return NULL;
            }
            if (!KswordArkWfpRvaValid(functionRva, 1UL, ImageSize)) {
                return NULL;
            }
            return (PVOID)((PUCHAR)(ULONG_PTR)ImageBase + functionRva);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }

    return NULL;
}

static PVOID
KswordArkWfpGetRoutine(
    _In_z_ const CHAR* RoutineName
    )
/*++

Routine Description:

    解析 WFP 管理 API 地址。中文说明：为了不修改 vcxproj 链接库，本函数只读
    系统模块表并解析已加载 fwpkclnt.sys 的导出；缺失时返回 NULL。

Arguments:

    RoutineName - 输入 ASCII 导出名。

Return Value:

    成功返回例程地址；失败返回 NULL。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG moduleIndex = 0UL;
    PVOID routineAddress = NULL;
    KSWORD_ARK_CALLBACK_MODULE_CACHE moduleCache;

    if (RoutineName == NULL) {
        return NULL;
    }

    KswordArkCallbackEnumInitModuleCache(&moduleCache);
    status = KswordArkCallbackEnumEnsureModuleCache(&moduleCache);
    if (!NT_SUCCESS(status) || moduleCache.ModuleInfo == NULL) {
        KswordArkCallbackEnumFreeModuleCache(&moduleCache);
        return NULL;
    }

    for (moduleIndex = 0UL; moduleIndex < moduleCache.ModuleInfo->NumberOfModules; ++moduleIndex) {
        KSWORD_ARK_CALLBACK_MODULE_ENTRY* moduleEntry = &moduleCache.ModuleInfo->Modules[moduleIndex];
        if (!KswordArkWfpAnsiPathContains(moduleEntry->FullPathName, RTL_NUMBER_OF(moduleEntry->FullPathName), "fwpkclnt.sys")) {
            continue;
        }
        routineAddress = KswordArkWfpResolveExport((ULONG64)(ULONG_PTR)moduleEntry->ImageBase, moduleEntry->ImageSize, RoutineName);
        break;
    }

    KswordArkCallbackEnumFreeModuleCache(&moduleCache);
    return routineAddress;
}

static NTSTATUS
KswordArkWfpResolveApi(
    _Out_ KSWORD_ARK_WFP_API* ApiOut
    )
/*++

Routine Description:

    解析 WFP API 表。中文说明：所有必需导出存在才启用 WFP 枚举/移除能力，
    否则返回 STATUS_NOT_SUPPORTED。

Arguments:

    ApiOut - 输出 API 表。

Return Value:

    成功返回 STATUS_SUCCESS；导出缺失返回 STATUS_NOT_SUPPORTED。

--*/
{
    if (ApiOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(ApiOut, sizeof(*ApiOut));
    ApiOut->EngineOpen = (KSWORD_ARK_WFP_ENGINE_OPEN)KswordArkWfpGetRoutine("FwpmEngineOpen0");
    ApiOut->EngineClose = (KSWORD_ARK_WFP_ENGINE_CLOSE)KswordArkWfpGetRoutine("FwpmEngineClose0");
    ApiOut->CalloutCreateEnumHandle = (KSWORD_ARK_WFP_CALLOUT_CREATE_ENUM_HANDLE)KswordArkWfpGetRoutine("FwpmCalloutCreateEnumHandle0");
    ApiOut->CalloutDestroyEnumHandle = (KSWORD_ARK_WFP_CALLOUT_DESTROY_ENUM_HANDLE)KswordArkWfpGetRoutine("FwpmCalloutDestroyEnumHandle0");
    ApiOut->CalloutEnum = (KSWORD_ARK_WFP_CALLOUT_ENUM)KswordArkWfpGetRoutine("FwpmCalloutEnum0");
    ApiOut->CalloutGetById = (KSWORD_ARK_WFP_CALLOUT_GET_BY_ID)KswordArkWfpGetRoutine("FwpmCalloutGetById0");
    ApiOut->CalloutDeleteById = (KSWORD_ARK_WFP_CALLOUT_DELETE_BY_ID)KswordArkWfpGetRoutine("FwpmCalloutDeleteById0");
    ApiOut->FreeMemory = (KSWORD_ARK_WFP_FREE_MEMORY)KswordArkWfpGetRoutine("FwpmFreeMemory0");

    if (ApiOut->EngineOpen == NULL || ApiOut->EngineClose == NULL || ApiOut->CalloutCreateEnumHandle == NULL ||
        ApiOut->CalloutDestroyEnumHandle == NULL || ApiOut->CalloutEnum == NULL || ApiOut->CalloutGetById == NULL ||
        ApiOut->CalloutDeleteById == NULL || ApiOut->FreeMemory == NULL) {
        RtlZeroMemory(ApiOut, sizeof(*ApiOut));
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordArkWfpOpenEngine(
    _In_ const KSWORD_ARK_WFP_API* Api,
    _Out_ HANDLE* EngineHandleOut
    )
/*++

Routine Description:

    打开 WFP engine 会话。中文说明：WFP 管理 API 要求 PASSIVE_LEVEL，本函数
    先做 IRQL 门控。

Arguments:

    Api - 输入 API 表。
    EngineHandleOut - 输出 engine handle。

Return Value:

    成功返回 STATUS_SUCCESS；上下文不安全返回 STATUS_NOT_SUPPORTED。

--*/
{
    if (Api == NULL || Api->EngineOpen == NULL || EngineHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *EngineHandleOut = NULL;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_NOT_SUPPORTED;
    }
    return Api->EngineOpen(NULL, RPC_C_AUTHN_WINNT, NULL, NULL, EngineHandleOut);
}

static VOID
KswordArkWfpAddRow(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _In_ const KSWORD_ARK_FWPM_CALLOUT0* Callout
    )
/*++

Routine Description:

    写入 WFP callout 枚举行。中文说明：公开 FWPM_CALLOUT0 不暴露内核函数
    地址，因此 callbackAddress 存放 calloutId，并使用 IDENTIFIER 字段标志。

Arguments:

    Builder - 输入输出枚举构建器。
    Callout - 输入 WFP callout。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_ENUM_ENTRY* entry = NULL;

    if (Builder == NULL || Callout == NULL) {
        return;
    }
    entry = KswordArkCallbackEnumReserveEntry(Builder);
    if (entry == NULL) {
        return;
    }

    entry->callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT;
    entry->source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_WFP_MGMT_API;
    entry->status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    entry->fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_NAME |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE;
    entry->callbackAddress = (ULONG64)Callout->calloutId;
    entry->registrationAddress = (ULONG64)Callout->calloutId;
    entry->lastStatus = STATUS_SUCCESS;

    if (Callout->displayData.name != NULL) {
        KswordArkCallbackEnumCopyWide(entry->name, RTL_NUMBER_OF(entry->name), Callout->displayData.name);
    }
    else {
        (VOID)RtlStringCbPrintfW(entry->name, sizeof(entry->name), L"WFP Callout #%lu", (unsigned long)Callout->calloutId);
    }
    if (Callout->displayData.description != NULL) {
        KswordArkCallbackEnumCopyWide(entry->altitude, RTL_NUMBER_OF(entry->altitude), Callout->displayData.description);
        if (entry->altitude[0] != L'\0') {
            entry->fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_ALTITUDE;
        }
    }
    (VOID)RtlStringCbPrintfW(
        entry->detail,
        sizeof(entry->detail),
        L"WFP FWPM_CALLOUT0；CalloutId=%lu，Flags=0x%08lX；公开 API 不返回 classify/notify 函数地址，移除使用 FwpmCalloutDeleteById0。",
        (unsigned long)Callout->calloutId,
        (unsigned long)Callout->flags);
}

VOID
KswordArkCallbackExternalWfpAddCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    枚举 WFP callout。中文说明：只使用公开 Fwpm* 管理 API，不扫描或修改 BFE
    内部链表；API 不可用时写入不支持说明行。

Arguments:

    Builder - 输入输出枚举构建器。

Return Value:

    无返回值。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE engineHandle = NULL;
    HANDLE enumHandle = NULL;
    UINT32 returnedCount = 0U;
    KSWORD_ARK_WFP_API api;

    if (Builder == NULL) {
        return;
    }
    RtlZeroMemory(&api, sizeof(api));

    status = KswordArkWfpResolveApi(&api);
    if (!NT_SUCCESS(status)) {
        Builder->LastStatus = status;
        KswordArkCallbackEnumAddUnsupportedRow(Builder, KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT, L"WFP callout enumeration", L"未解析到完整 fwpkclnt.sys Fwpm* 管理 API；当前返回 STATUS_NOT_SUPPORTED。");
        return;
    }

    status = KswordArkWfpOpenEngine(&api, &engineHandle);
    if (!NT_SUCCESS(status)) {
        Builder->LastStatus = status;
        KswordArkCallbackEnumAddUnsupportedRow(Builder, KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT, L"WFP callout enumeration", L"FwpmEngineOpen0 失败或当前 IRQL 不满足 PASSIVE_LEVEL。");
        return;
    }

    status = api.CalloutCreateEnumHandle(engineHandle, NULL, &enumHandle);
    if (!NT_SUCCESS(status)) {
        Builder->LastStatus = status;
        (VOID)api.EngineClose(engineHandle);
        KswordArkCallbackEnumAddUnsupportedRow(Builder, KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT, L"WFP callout enumeration", L"FwpmCalloutCreateEnumHandle0 失败，无法获取枚举快照。");
        return;
    }

    do {
        KSWORD_ARK_FWPM_CALLOUT0** entries = NULL;
        UINT32 entryIndex = 0U;
        returnedCount = 0U;

        status = api.CalloutEnum(engineHandle, enumHandle, KSWORD_ARK_WFP_ENUM_PAGE_SIZE, &entries, &returnedCount);
        if (!NT_SUCCESS(status)) {
            Builder->LastStatus = status;
            if (entries != NULL) {
                VOID* freePointer = entries;
                api.FreeMemory(&freePointer);
            }
            break;
        }
        for (entryIndex = 0U; entryIndex < returnedCount; ++entryIndex) {
            if (entries != NULL && entries[entryIndex] != NULL) {
                KswordArkWfpAddRow(Builder, entries[entryIndex]);
            }
        }
        if (entries != NULL) {
            VOID* freePointer = entries;
            api.FreeMemory(&freePointer);
        }
    } while (returnedCount == KSWORD_ARK_WFP_ENUM_PAGE_SIZE);

    (VOID)api.CalloutDestroyEnumHandle(engineHandle, enumHandle);
    (VOID)api.EngineClose(engineHandle);
}

NTSTATUS
KswordArkCallbackExternalWfpRemove(
    _In_ const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST* RequestPacket,
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE* ResponsePacket
    )
/*++

Routine Description:

    通过 calloutId 移除 WFP callout。中文说明：先用 FwpmCalloutGetById0 验证
    该 ID 来自当前可枚举对象，再调用 FwpmCalloutDeleteById0。

Arguments:

    RequestPacket - 输入请求，callbackAddress 承载 calloutId。
    ResponsePacket - 输入输出响应。

Return Value:

    成功返回 STATUS_SUCCESS；不可验证或 API 不可用返回对应 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE engineHandle = NULL;
    KSWORD_ARK_FWPM_CALLOUT0* callout = NULL;
    UINT32 calloutId = 0U;
    KSWORD_ARK_WFP_API api;

    if (RequestPacket == NULL || ResponsePacket == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (RequestPacket->callbackClass != KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT ||
        RequestPacket->callbackAddress == 0ULL || RequestPacket->callbackAddress > KSWORD_ARK_WFP_MAX_CALLOUT_ID) {
        return STATUS_INVALID_PARAMETER;
    }

    calloutId = (UINT32)RequestPacket->callbackAddress;
    RtlZeroMemory(&api, sizeof(api));
    status = KswordArkWfpResolveApi(&api);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = KswordArkWfpOpenEngine(&api, &engineHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = api.CalloutGetById(engineHandle, calloutId, &callout);
    if (!NT_SUCCESS(status) || callout == NULL) {
        (VOID)api.EngineClose(engineHandle);
        return status;
    }

    ResponsePacket->mappingFlags |= KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_ENUMERATED |
        KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_PUBLIC_API;
    if (callout->displayData.name != NULL) {
        KswordArkCallbackEnumCopyWide(ResponsePacket->serviceName, RTL_NUMBER_OF(ResponsePacket->serviceName), callout->displayData.name);
    }

    {
        VOID* freePointer = callout;
        api.FreeMemory(&freePointer);
        callout = NULL;
    }

    status = api.CalloutDeleteById(engineHandle, calloutId);
    (VOID)api.EngineClose(engineHandle);
    return status;
}
