/*++

Module Name:

    ioctl_registry.c

Abstract:

    Static IOCTL registry for the KswordARK dispatch path.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ioctl_registry.h"
#include "ark/ark_ioctl.h"

// Feature handler declarations live here instead of in the central dispatch file.
NTSTATUS KswordARKProcessIoctlTerminate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlSuspend(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlSetPplLevel(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlEnumProcess(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlSetVisibility(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlSetSpecialFlags(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKProcessIoctlDkomProcess(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlQueryVirtualMemory(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlReadVirtualMemory(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlWriteVirtualMemory(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlReadPhysicalMemory(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlWritePhysicalMemory(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlTranslateVirtualAddress(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKMemoryIoctlQueryPageTableEntry(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKFileIoctlDeletePath(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKFileIoctlQueryFileInfo(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKFileMonitorIoctlControl(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKFileMonitorIoctlDrain(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKFileMonitorIoctlQueryStatus(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlEnumSsdt(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlQueryDriverObject(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlEnumShadowSsdt(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlScanInlineHooks(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlPatchInlineHook(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlEnumIatEatHooks(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKKernelIoctlForceUnloadDriver(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlSetRulesHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlGetRuntimeStateHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlWaitEventHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlAnswerEventHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlCancelAllPendingHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlRemoveExternalCallbackHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCallbackIoctlEnumCallbacksHandler(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKDynDataIoctlQueryStatus(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKDynDataIoctlQueryFields(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKDynDataIoctlQueryCapabilities(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKCapabilityIoctlQueryDriverCapabilities(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKThreadIoctlEnumThread(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKHandleIoctlEnumProcessHandles(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKHandleIoctlQueryHandleObject(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKAlpcIoctlQueryAlpcPort(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKSectionIoctlQueryProcessSection(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKSectionIoctlQueryFileSectionMappings(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKWslSiloIoctlQuery(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKTrustIoctlQueryImageTrust(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKSafetyIoctlQueryPolicy(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKSafetyIoctlSetPolicy(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKPreflightIoctlQuery(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKRegistryIoctlReadValue(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKRedirectIoctlSetRules(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKRedirectIoctlQueryStatus(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKNetworkIoctlSetRules(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);
NTSTATUS KswordARKNetworkIoctlQueryStatus(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ size_t InputBufferLength, _In_ size_t OutputBufferLength, _Out_ size_t* BytesReturned);

static const KSWORD_ARK_IOCTL_ENTRY g_KswordArkIoctlTable[] = {
    { IOCTL_KSWORD_ARK_TERMINATE_PROCESS, KswordARKProcessIoctlTerminate, "IOCTL_KSWORD_ARK_TERMINATE_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SUSPEND_PROCESS, KswordARKProcessIoctlSuspend, "IOCTL_KSWORD_ARK_SUSPEND_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_PPL_LEVEL, KswordARKProcessIoctlSetPplLevel, "IOCTL_KSWORD_ARK_SET_PPL_LEVEL", KSW_CAP_PROCESS_PROTECTION_PATCH, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_PROCESS, KswordARKProcessIoctlEnumProcess, "IOCTL_KSWORD_ARK_ENUM_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY, KswordARKProcessIoctlSetVisibility, "IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS, KswordARKProcessIoctlSetSpecialFlags, "IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_DKOM_PROCESS, KswordARKProcessIoctlDkomProcess, "IOCTL_KSWORD_ARK_DKOM_PROCESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY, KswordARKMemoryIoctlQueryVirtualMemory, "IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY, KswordARKMemoryIoctlReadVirtualMemory, "IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY, KswordARKMemoryIoctlWriteVirtualMemory, "IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY, KswordARKMemoryIoctlReadPhysicalMemory, "IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY, KswordARKMemoryIoctlWritePhysicalMemory, "IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS, KswordARKMemoryIoctlTranslateVirtualAddress, "IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY, KswordARKMemoryIoctlQueryPageTableEntry, "IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_DELETE_PATH, KswordARKFileIoctlDeletePath, "IOCTL_KSWORD_ARK_DELETE_PATH", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_FILE_INFO, KswordARKFileIoctlQueryFileInfo, "IOCTL_KSWORD_ARK_QUERY_FILE_INFO", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL, KswordARKFileMonitorIoctlControl, "IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_FILE_MONITOR_DRAIN, KswordARKFileMonitorIoctlDrain, "IOCTL_KSWORD_ARK_FILE_MONITOR_DRAIN", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS, KswordARKFileMonitorIoctlQueryStatus, "IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_SSDT, KswordARKKernelIoctlEnumSsdt, "IOCTL_KSWORD_ARK_ENUM_SSDT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT, KswordARKKernelIoctlQueryDriverObject, "IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT, KswordARKKernelIoctlEnumShadowSsdt, "IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS, KswordARKKernelIoctlScanInlineHooks, "IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK, KswordARKKernelIoctlPatchInlineHook, "IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS, KswordARKKernelIoctlEnumIatEatHooks, "IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER, KswordARKKernelIoctlForceUnloadDriver, "IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_CALLBACK_RULES, KswordARKCallbackIoctlSetRulesHandler, "IOCTL_KSWORD_ARK_SET_CALLBACK_RULES", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE, KswordARKCallbackIoctlGetRuntimeStateHandler, "IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT, KswordARKCallbackIoctlWaitEventHandler, "IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT, KswordARKCallbackIoctlAnswerEventHandler, "IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS, KswordARKCallbackIoctlCancelAllPendingHandler, "IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK, KswordARKCallbackIoctlRemoveExternalCallbackHandler, "IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_CALLBACKS, KswordARKCallbackIoctlEnumCallbacksHandler, "IOCTL_KSWORD_ARK_ENUM_CALLBACKS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_DYN_STATUS, KswordARKDynDataIoctlQueryStatus, "IOCTL_KSWORD_ARK_QUERY_DYN_STATUS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS, KswordARKDynDataIoctlQueryFields, "IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_CAPABILITIES, KswordARKDynDataIoctlQueryCapabilities, "IOCTL_KSWORD_ARK_QUERY_CAPABILITIES", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES, KswordARKCapabilityIoctlQueryDriverCapabilities, "IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_THREAD, KswordARKThreadIoctlEnumThread, "IOCTL_KSWORD_ARK_ENUM_THREAD", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES, KswordARKHandleIoctlEnumProcessHandles, "IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES", KSW_CAP_PROCESS_OBJECT_TABLE | KSW_CAP_HANDLE_TABLE_DECODE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT, KswordARKHandleIoctlQueryHandleObject, "IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT", KSW_CAP_OBJECT_TYPE_FIELDS, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_ALPC_PORT, KswordARKAlpcIoctlQueryAlpcPort, "IOCTL_KSWORD_ARK_QUERY_ALPC_PORT", KSW_CAP_ALPC_FIELDS | KSW_CAP_OBJECT_TYPE_FIELDS, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION, KswordARKSectionIoctlQueryProcessSection, "IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION", KSW_CAP_SECTION_CONTROL_AREA, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS, KswordARKSectionIoctlQueryFileSectionMappings, "IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS", KSW_CAP_SECTION_CONTROL_AREA, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_WSL_SILO, KswordARKWslSiloIoctlQuery, "IOCTL_KSWORD_ARK_QUERY_WSL_SILO", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST, KswordARKTrustIoctlQueryImageTrust, "IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_SAFETY_POLICY, KswordARKSafetyIoctlQueryPolicy, "IOCTL_KSWORD_ARK_QUERY_SAFETY_POLICY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_SET_SAFETY_POLICY, KswordARKSafetyIoctlSetPolicy, "IOCTL_KSWORD_ARK_SET_SAFETY_POLICY", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_QUERY_PREFLIGHT, KswordARKPreflightIoctlQuery, "IOCTL_KSWORD_ARK_QUERY_PREFLIGHT", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE, KswordARKRegistryIoctlReadValue, "IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_REDIRECT_SET_RULES, KswordARKRedirectIoctlSetRules, "IOCTL_KSWORD_ARK_REDIRECT_SET_RULES", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS, KswordARKRedirectIoctlQueryStatus, "IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_NETWORK_SET_RULES, KswordARKNetworkIoctlSetRules, "IOCTL_KSWORD_ARK_NETWORK_SET_RULES", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE },
    { IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS, KswordARKNetworkIoctlQueryStatus, "IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS", KSWORD_ARK_IOCTL_CAPABILITY_NONE, KSWORD_ARK_IOCTL_FLAG_NONE }
};

_Must_inspect_result_
const KSWORD_ARK_IOCTL_ENTRY*
KswordARKLookupIoctlEntry(
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    Locate the registered IOCTL entry for the supplied control code. The table is
    exact-match only so unsupported IOCTLs fail closed.

Arguments:

    IoControlCode - Control code received by the queue dispatch callback.

Return Value:

    Pointer to a registry entry when found; NULL when unsupported.

--*/
{
    ULONG entryIndex = 0;

    for (entryIndex = 0; entryIndex < (sizeof(g_KswordArkIoctlTable) / sizeof(g_KswordArkIoctlTable[0])); ++entryIndex) {
        if (g_KswordArkIoctlTable[entryIndex].IoControlCode == IoControlCode) {
            return &g_KswordArkIoctlTable[entryIndex];
        }
    }

    return NULL;
}

ULONG
KswordARKGetRegisteredIoctlCount(
    VOID
    )
/*++

Routine Description:

    返回当前 IOCTL 注册表项数量。中文说明：Phase-16 preflight 使用该值检查
    注册表是否可枚举，避免 R3 自己猜测驱动支持面。

Arguments:

    None.

Return Value:

    g_KswordArkIoctlTable 的元素数量。

--*/
{
    return (ULONG)(sizeof(g_KswordArkIoctlTable) / sizeof(g_KswordArkIoctlTable[0]));
}

ULONG
KswordARKGetDuplicateIoctlCount(
    VOID
    )
/*++

Routine Description:

    检查 IOCTL 注册表中重复 control code 的数量。中文说明：重复注册会导致
    后面的 handler 永远不可达，是发布前必须暴露的结构性问题。

Arguments:

    None.

Return Value:

    重复项数量；0 表示没有重复。

--*/
{
    ULONG duplicateCount = 0UL;
    ULONG outerIndex = 0UL;
    ULONG innerIndex = 0UL;
    const ULONG totalCount = KswordARKGetRegisteredIoctlCount();

    for (outerIndex = 0UL; outerIndex < totalCount; ++outerIndex) {
        for (innerIndex = outerIndex + 1UL; innerIndex < totalCount; ++innerIndex) {
            if (g_KswordArkIoctlTable[outerIndex].IoControlCode ==
                g_KswordArkIoctlTable[innerIndex].IoControlCode) {
                duplicateCount += 1UL;
            }
        }
    }

    return duplicateCount;
}
