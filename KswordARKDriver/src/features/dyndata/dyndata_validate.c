/*++

Module Name:

    dyndata_validate.c

Abstract:

    Field descriptor and capability-gating helpers for Ksword DynData.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_dyndata_fields.h"

#include <ntstrsafe.h>

typedef struct _KSW_DYN_FIELD_BINDING
{
    ULONG FieldId;
    PCSTR FieldName;
    PCSTR FeatureName;
    ULONG64 CapabilityMask;
    BOOLEAN Required;
    ULONG Source;
    SIZE_T OffsetInState;
} KSW_DYN_FIELD_BINDING, *PKSW_DYN_FIELD_BINDING;

#define KSW_FIELD_BINDING(FieldIdValue, FieldNameText, FeatureText, CapabilityValue, RequiredValue, SourceValue, MemberPath) \
    { FieldIdValue, FieldNameText, FeatureText, CapabilityValue, RequiredValue, SourceValue, FIELD_OFFSET(KSW_DYN_STATE, MemberPath) }

static const KSW_DYN_FIELD_BINDING g_KswordDynFieldBindings[] = {
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_OBJECT_TABLE, "EpObjectTable", "Process HandleTable", KSW_CAP_PROCESS_OBJECT_TABLE | KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EpObjectTable),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_SECTION_OBJECT, "EpSectionObject", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EpSectionObject),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID, "_EPROCESS.UniqueProcessId", "Process List Fields", KSW_CAP_PROCESS_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EpUniqueProcessId),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS, "_EPROCESS.ActiveProcessLinks", "Process List Fields", KSW_CAP_PROCESS_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EpActiveProcessLinks),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD, "_EPROCESS.ThreadListHead", "Process List Fields", KSW_CAP_PROCESS_LIST_FIELDS | KSW_CAP_THREAD_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EpThreadListHead),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_IMAGE_FILE_NAME, "_EPROCESS.ImageFileName", "Process List Fields", KSW_CAP_PROCESS_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EpImageFileName),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_TOKEN, "_EPROCESS.Token", "Process List Fields", KSW_CAP_PROCESS_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EpToken),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_HT_HANDLE_CONTENTION_EVENT, "HtHandleContentionEvent", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, FALSE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.HtHandleContentionEvent),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_HT_TABLE_CODE, "_HANDLE_TABLE.TableCode", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE | KSW_CAP_CID_TABLE_WALK, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.HtTableCode),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_HT_HANDLE_COUNT, "_HANDLE_TABLE.HandleCount", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.HtHandleCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_HTE_LOW_VALUE, "_HANDLE_TABLE_ENTRY.LowValue", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE | KSW_CAP_CID_TABLE_WALK, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.HteLowValue),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OT_NAME, "OtName", "Object Type", KSW_CAP_OBJECT_TYPE_FIELDS | KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.OtName),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OT_INDEX, "OtIndex", "Object Type", KSW_CAP_OBJECT_TYPE_FIELDS | KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.OtIndex),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OB_DECODE_SHIFT, "ObDecodeShift", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.ObDecodeShift),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_OB_ATTRIBUTES_SHIFT, "ObAttributesShift", "HandleTable Decode", KSW_CAP_HANDLE_TABLE_DECODE, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.ObAttributesShift),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EGE_GUID, "EgeGuid", "ETW GUID Entry", KSW_CAP_ETW_GUID_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EgeGuid),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ERE_GUID_ENTRY, "EreGuidEntry", "ETW Registration Entry", KSW_CAP_ETW_GUID_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.EreGuidEntry),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_INITIAL_STACK, "KtInitialStack", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtInitialStack),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_STACK_LIMIT, "KtStackLimit", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtStackLimit),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_STACK_BASE, "KtStackBase", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtStackBase),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_KERNEL_STACK, "KtKernelStack", "Thread Stack", KSW_CAP_THREAD_STACK_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtKernelStack),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_PROCESS, "_KTHREAD.Process", "Thread List Fields", KSW_CAP_THREAD_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KtProcess),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ET_CID, "_ETHREAD.Cid", "Thread List Fields", KSW_CAP_THREAD_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EtCid),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY, "_ETHREAD.ThreadListEntry", "Thread List Fields", KSW_CAP_THREAD_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EtThreadListEntry),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ET_START_ADDRESS, "_ETHREAD.StartAddress", "Thread List Fields", KSW_CAP_THREAD_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EtStartAddress),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS, "_ETHREAD.Win32StartAddress", "Thread List Fields", KSW_CAP_THREAD_LIST_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.EtWin32StartAddress),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_READ_OPERATION_COUNT, "KtReadOperationCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtReadOperationCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_WRITE_OPERATION_COUNT, "KtWriteOperationCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtWriteOperationCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_OTHER_OPERATION_COUNT, "KtOtherOperationCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtOtherOperationCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_READ_TRANSFER_COUNT, "KtReadTransferCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtReadTransferCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_WRITE_TRANSFER_COUNT, "KtWriteTransferCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtWriteTransferCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KT_OTHER_TRANSFER_COUNT, "KtOtherTransferCount", "Thread I/O Counters", KSW_CAP_THREAD_IO_COUNTERS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.KtOtherTransferCount),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_MM_SECTION_CONTROL_AREA, "MmSectionControlArea", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.MmSectionControlArea),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LIST_HEAD, "MmControlAreaListHead", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.MmControlAreaListHead),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LOCK, "MmControlAreaLock", "Section/ControlArea", KSW_CAP_SECTION_CONTROL_AREA, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.MmControlAreaLock),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_COMMUNICATION_INFO, "AlpcCommunicationInfo", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcCommunicationInfo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_OWNER_PROCESS, "AlpcOwnerProcess", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcOwnerProcess),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_CONNECTION_PORT, "AlpcConnectionPort", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcConnectionPort),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_SERVER_COMMUNICATION_PORT, "AlpcServerCommunicationPort", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcServerCommunicationPort),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_CLIENT_COMMUNICATION_PORT, "AlpcClientCommunicationPort", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcClientCommunicationPort),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE, "AlpcHandleTable", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcHandleTable),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE_LOCK, "AlpcHandleTableLock", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcHandleTableLock),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES, "AlpcAttributes", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcAttributes),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES_FLAGS, "AlpcAttributesFlags", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcAttributesFlags),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_PORT_CONTEXT, "AlpcPortContext", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcPortContext),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_PORT_OBJECT_LOCK, "AlpcPortObjectLock", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcPortObjectLock),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_SEQUENCE_NO, "AlpcSequenceNo", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcSequenceNo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_ALPC_STATE, "AlpcState", "ALPC", KSW_CAP_ALPC_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, Kernel.AlpcState),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_PROC, "LxPicoProc", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoProc),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO, "LxPicoProcInfo", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoProcInfo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO_PID, "LxPicoProcInfoPID", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoProcInfoPID),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO, "LxPicoThrdInfo", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoThrdInfo),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO_TID, "LxPicoThrdInfoTID", "WSL/Lxcore", KSW_CAP_WSL_LXCORE_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER, LxcoreOffsets.LxPicoThrdInfoTID),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_PROTECTION, "EpProtection", "Process Protection Patch", KSW_CAP_PROCESS_PROTECTION_PATCH, TRUE, KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN, Kernel.EpProtection),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_SIGNATURE_LEVEL, "EpSignatureLevel", "Process Protection Patch", KSW_CAP_PROCESS_PROTECTION_PATCH, TRUE, KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN, Kernel.EpSignatureLevel),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_EP_SECTION_SIGNATURE_LEVEL, "EpSectionSignatureLevel", "Process Protection Patch", KSW_CAP_PROCESS_PROTECTION_PATCH, TRUE, KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN, Kernel.EpSectionSignatureLevel),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KLDR_IN_LOAD_ORDER_LINKS, "_KLDR_DATA_TABLE_ENTRY.InLoadOrderLinks", "Kernel Module List Fields", KSW_CAP_KERNEL_MODULE_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KldrInLoadOrderLinks),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KLDR_DLL_BASE, "_KLDR_DATA_TABLE_ENTRY.DllBase", "Kernel Module List Fields", KSW_CAP_KERNEL_MODULE_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KldrDllBase),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KLDR_SIZE_OF_IMAGE, "_KLDR_DATA_TABLE_ENTRY.SizeOfImage", "Kernel Module List Fields", KSW_CAP_KERNEL_MODULE_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KldrSizeOfImage),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KLDR_FULL_DLL_NAME, "_KLDR_DATA_TABLE_ENTRY.FullDllName", "Kernel Module List Fields", KSW_CAP_KERNEL_MODULE_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KldrFullDllName),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KLDR_BASE_DLL_NAME, "_KLDR_DATA_TABLE_ENTRY.BaseDllName", "Kernel Module List Fields", KSW_CAP_KERNEL_MODULE_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KldrBaseDllName),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KLDR_FLAGS, "_KLDR_DATA_TABLE_ENTRY.Flags", "Kernel Module List Fields", KSW_CAP_KERNEL_MODULE_LIST_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.KldrFlags),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_DO_DRIVER_START, "_DRIVER_OBJECT.DriverStart", "Driver Object Fields", KSW_CAP_DRIVER_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.DoDriverStart),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_DO_DRIVER_SIZE, "_DRIVER_OBJECT.DriverSize", "Driver Object Fields", KSW_CAP_DRIVER_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.DoDriverSize),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_DO_DRIVER_SECTION, "_DRIVER_OBJECT.DriverSection", "Driver Object Fields", KSW_CAP_DRIVER_OBJECT_FIELDS | KSW_CAP_KERNEL_MODULE_LIST_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.DoDriverSection),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_DO_MAJOR_FUNCTION, "_DRIVER_OBJECT.MajorFunction", "Driver Object Fields", KSW_CAP_DRIVER_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.DoMajorFunction),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_DO_FAST_IO_DISPATCH, "_DRIVER_OBJECT.FastIoDispatch", "Driver Object Fields", KSW_CAP_DRIVER_OBJECT_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.DoFastIoDispatch),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_DO_DRIVER_UNLOAD, "_DRIVER_OBJECT.DriverUnload", "Driver Object Fields", KSW_CAP_DRIVER_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, Kernel.DoDriverUnload),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KG_PSP_CID_TABLE, "PspCidTable", "Kernel Globals", KSW_CAP_KERNEL_GLOBALS | KSW_CAP_CID_TABLE_WALK, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, KernelGlobals.PspCidTable),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KG_PS_LOADED_MODULE_LIST, "PsLoadedModuleList", "Kernel Globals", KSW_CAP_KERNEL_GLOBALS | KSW_CAP_KERNEL_MODULE_LIST_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, KernelGlobals.PsLoadedModuleList),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KG_MM_UNLOADED_DRIVERS, "MmUnloadedDrivers", "Kernel Globals", KSW_CAP_KERNEL_GLOBALS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, KernelGlobals.MmUnloadedDrivers),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE, "PiDDBCacheTable", "Kernel Globals", KSW_CAP_KERNEL_GLOBALS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, KernelGlobals.PiDDBCacheTable),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_PSP_CREATE_PROCESS_NOTIFY_ROUTINE, "PspCreateProcessNotifyRoutine", "Callback Notify Globals", KSW_CAP_CALLBACK_NOTIFY_GLOBALS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackGlobals.PspCreateProcessNotifyRoutine),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_PSP_CREATE_THREAD_NOTIFY_ROUTINE, "PspCreateThreadNotifyRoutine", "Callback Notify Globals", KSW_CAP_CALLBACK_NOTIFY_GLOBALS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackGlobals.PspCreateThreadNotifyRoutine),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_PSP_LOAD_IMAGE_NOTIFY_ROUTINE, "PspLoadImageNotifyRoutine", "Callback Notify Globals", KSW_CAP_CALLBACK_NOTIFY_GLOBALS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackGlobals.PspLoadImageNotifyRoutine),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_PSP_NOTIFY_ENABLE_MASK, "PspNotifyEnableMask", "Callback Notify Globals", KSW_CAP_CALLBACK_NOTIFY_GLOBALS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackGlobals.PspNotifyEnableMask),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CM_CALLBACK_LIST_HEAD, "CmCallbackListHead", "Callback Registry Globals", KSW_CAP_CALLBACK_REGISTRY_GLOBALS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackGlobals.CmCallbackListHead),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_OBJECT_TYPE_CALLBACK_LIST, "_OBJECT_TYPE.CallbackList", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.ObjectTypeCallbackList),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_ENTRY_LIST, "_CALLBACK_ENTRY_ITEM.EntryList", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryItemEntryList),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_PRE_OPERATION, "_CALLBACK_ENTRY_ITEM.PreOperation", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryItemPreOperation),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_POST_OPERATION, "_CALLBACK_ENTRY_ITEM.PostOperation", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryItemPostOperation),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_OPERATIONS, "_CALLBACK_ENTRY_ITEM.Operations", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryItemOperations),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_CALLBACK_ENTRY, "_CALLBACK_ENTRY_ITEM.CallbackEntry", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, TRUE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryItemCallbackEntry),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ALTITUDE, "_CALLBACK_ENTRY.Altitude", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryAltitude),
    KSW_FIELD_BINDING(KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_REGISTRATION_CONTEXT, "_CALLBACK_ENTRY.RegistrationContext", "Callback Object Fields", KSW_CAP_CALLBACK_OBJECT_FIELDS, FALSE, KSW_DYN_FIELD_SOURCE_PDB_PROFILE, CallbackOffsets.CallbackEntryRegistrationContext)
};

static ULONG
KswordARKDynDataReadBoundOffset(
    _In_ const KSW_DYN_STATE* State,
    _In_ const KSW_DYN_FIELD_BINDING* Binding
    )
/*++

Routine Description:

    Read a bound offset from a DynData state snapshot. Bindings use FIELD_OFFSET
    so query and capability code can share one field list.

Arguments:

    State - State snapshot that owns all offsets.
    Binding - Field binding row to read.

Return Value:

    Offset value, or KSW_DYN_OFFSET_UNAVAILABLE on invalid input.

--*/
{
    const UCHAR* stateBytes = (const UCHAR*)State;

    if (State == NULL || Binding == NULL || Binding->OffsetInState > sizeof(KSW_DYN_STATE) - sizeof(ULONG)) {
        return KSW_DYN_OFFSET_UNAVAILABLE;
    }

    return *(const ULONG*)(stateBytes + Binding->OffsetInState);
}

static ULONG
KswordARKDynDataReadBoundSource(
    _In_ const KSW_DYN_STATE* State,
    _In_ const KSW_DYN_FIELD_BINDING* Binding
    )
/*++

Routine Description:

    Read the runtime provenance for one bound offset. Offset descriptors keep
    static metadata, while the source lives in KSW_DYN_STATE so PDB profile
    merges can override System Informer or runtime-pattern provenance precisely.

Arguments:

    State - State snapshot that owns all source fields.
    Binding - Field binding row whose FieldId selects a source field.

Return Value:

    KSW_DYN_FIELD_SOURCE_* value. Unknown bindings return UNAVAILABLE.

--*/
{
    if (State == NULL || Binding == NULL) {
        return KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
    }

    switch (Binding->FieldId) {
    case KSW_DYN_FIELD_ID_EP_OBJECT_TABLE:
        return State->KernelSources.EpObjectTable;
    case KSW_DYN_FIELD_ID_EP_SECTION_OBJECT:
        return State->KernelSources.EpSectionObject;
    case KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID:
        return State->KernelSources.EpUniqueProcessId;
    case KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS:
        return State->KernelSources.EpActiveProcessLinks;
    case KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD:
        return State->KernelSources.EpThreadListHead;
    case KSW_DYN_FIELD_ID_EP_IMAGE_FILE_NAME:
        return State->KernelSources.EpImageFileName;
    case KSW_DYN_FIELD_ID_EP_TOKEN:
        return State->KernelSources.EpToken;
    case KSW_DYN_FIELD_ID_HT_HANDLE_CONTENTION_EVENT:
        return State->KernelSources.HtHandleContentionEvent;
    case KSW_DYN_FIELD_ID_HT_TABLE_CODE:
        return State->KernelSources.HtTableCode;
    case KSW_DYN_FIELD_ID_HT_HANDLE_COUNT:
        return State->KernelSources.HtHandleCount;
    case KSW_DYN_FIELD_ID_HTE_LOW_VALUE:
        return State->KernelSources.HteLowValue;
    case KSW_DYN_FIELD_ID_OT_NAME:
        return State->KernelSources.OtName;
    case KSW_DYN_FIELD_ID_OT_INDEX:
        return State->KernelSources.OtIndex;
    case KSW_DYN_FIELD_ID_OB_DECODE_SHIFT:
        return State->KernelSources.ObDecodeShift;
    case KSW_DYN_FIELD_ID_OB_ATTRIBUTES_SHIFT:
        return State->KernelSources.ObAttributesShift;
    case KSW_DYN_FIELD_ID_EGE_GUID:
        return State->KernelSources.EgeGuid;
    case KSW_DYN_FIELD_ID_ERE_GUID_ENTRY:
        return State->KernelSources.EreGuidEntry;
    case KSW_DYN_FIELD_ID_KT_INITIAL_STACK:
        return State->KernelSources.KtInitialStack;
    case KSW_DYN_FIELD_ID_KT_STACK_LIMIT:
        return State->KernelSources.KtStackLimit;
    case KSW_DYN_FIELD_ID_KT_STACK_BASE:
        return State->KernelSources.KtStackBase;
    case KSW_DYN_FIELD_ID_KT_KERNEL_STACK:
        return State->KernelSources.KtKernelStack;
    case KSW_DYN_FIELD_ID_KT_PROCESS:
        return State->KernelSources.KtProcess;
    case KSW_DYN_FIELD_ID_ET_CID:
        return State->KernelSources.EtCid;
    case KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY:
        return State->KernelSources.EtThreadListEntry;
    case KSW_DYN_FIELD_ID_ET_START_ADDRESS:
        return State->KernelSources.EtStartAddress;
    case KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS:
        return State->KernelSources.EtWin32StartAddress;
    case KSW_DYN_FIELD_ID_KT_READ_OPERATION_COUNT:
        return State->KernelSources.KtReadOperationCount;
    case KSW_DYN_FIELD_ID_KT_WRITE_OPERATION_COUNT:
        return State->KernelSources.KtWriteOperationCount;
    case KSW_DYN_FIELD_ID_KT_OTHER_OPERATION_COUNT:
        return State->KernelSources.KtOtherOperationCount;
    case KSW_DYN_FIELD_ID_KT_READ_TRANSFER_COUNT:
        return State->KernelSources.KtReadTransferCount;
    case KSW_DYN_FIELD_ID_KT_WRITE_TRANSFER_COUNT:
        return State->KernelSources.KtWriteTransferCount;
    case KSW_DYN_FIELD_ID_KT_OTHER_TRANSFER_COUNT:
        return State->KernelSources.KtOtherTransferCount;
    case KSW_DYN_FIELD_ID_MM_SECTION_CONTROL_AREA:
        return State->KernelSources.MmSectionControlArea;
    case KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LIST_HEAD:
        return State->KernelSources.MmControlAreaListHead;
    case KSW_DYN_FIELD_ID_MM_CONTROL_AREA_LOCK:
        return State->KernelSources.MmControlAreaLock;
    case KSW_DYN_FIELD_ID_ALPC_COMMUNICATION_INFO:
        return State->KernelSources.AlpcCommunicationInfo;
    case KSW_DYN_FIELD_ID_ALPC_OWNER_PROCESS:
        return State->KernelSources.AlpcOwnerProcess;
    case KSW_DYN_FIELD_ID_ALPC_CONNECTION_PORT:
        return State->KernelSources.AlpcConnectionPort;
    case KSW_DYN_FIELD_ID_ALPC_SERVER_COMMUNICATION_PORT:
        return State->KernelSources.AlpcServerCommunicationPort;
    case KSW_DYN_FIELD_ID_ALPC_CLIENT_COMMUNICATION_PORT:
        return State->KernelSources.AlpcClientCommunicationPort;
    case KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE:
        return State->KernelSources.AlpcHandleTable;
    case KSW_DYN_FIELD_ID_ALPC_HANDLE_TABLE_LOCK:
        return State->KernelSources.AlpcHandleTableLock;
    case KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES:
        return State->KernelSources.AlpcAttributes;
    case KSW_DYN_FIELD_ID_ALPC_ATTRIBUTES_FLAGS:
        return State->KernelSources.AlpcAttributesFlags;
    case KSW_DYN_FIELD_ID_ALPC_PORT_CONTEXT:
        return State->KernelSources.AlpcPortContext;
    case KSW_DYN_FIELD_ID_ALPC_PORT_OBJECT_LOCK:
        return State->KernelSources.AlpcPortObjectLock;
    case KSW_DYN_FIELD_ID_ALPC_SEQUENCE_NO:
        return State->KernelSources.AlpcSequenceNo;
    case KSW_DYN_FIELD_ID_ALPC_STATE:
        return State->KernelSources.AlpcState;
    case KSW_DYN_FIELD_ID_LX_PICO_PROC:
        return State->LxcoreSources.LxPicoProc;
    case KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO:
        return State->LxcoreSources.LxPicoProcInfo;
    case KSW_DYN_FIELD_ID_LX_PICO_PROC_INFO_PID:
        return State->LxcoreSources.LxPicoProcInfoPID;
    case KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO:
        return State->LxcoreSources.LxPicoThrdInfo;
    case KSW_DYN_FIELD_ID_LX_PICO_THRD_INFO_TID:
        return State->LxcoreSources.LxPicoThrdInfoTID;
    case KSW_DYN_FIELD_ID_EP_PROTECTION:
        return State->KernelSources.EpProtection;
    case KSW_DYN_FIELD_ID_EP_SIGNATURE_LEVEL:
        return State->KernelSources.EpSignatureLevel;
    case KSW_DYN_FIELD_ID_EP_SECTION_SIGNATURE_LEVEL:
        return State->KernelSources.EpSectionSignatureLevel;
    case KSW_DYN_FIELD_ID_KLDR_IN_LOAD_ORDER_LINKS:
        return State->KernelSources.KldrInLoadOrderLinks;
    case KSW_DYN_FIELD_ID_KLDR_DLL_BASE:
        return State->KernelSources.KldrDllBase;
    case KSW_DYN_FIELD_ID_KLDR_SIZE_OF_IMAGE:
        return State->KernelSources.KldrSizeOfImage;
    case KSW_DYN_FIELD_ID_KLDR_FULL_DLL_NAME:
        return State->KernelSources.KldrFullDllName;
    case KSW_DYN_FIELD_ID_KLDR_BASE_DLL_NAME:
        return State->KernelSources.KldrBaseDllName;
    case KSW_DYN_FIELD_ID_KLDR_FLAGS:
        return State->KernelSources.KldrFlags;
    case KSW_DYN_FIELD_ID_DO_DRIVER_START:
        return State->KernelSources.DoDriverStart;
    case KSW_DYN_FIELD_ID_DO_DRIVER_SIZE:
        return State->KernelSources.DoDriverSize;
    case KSW_DYN_FIELD_ID_DO_DRIVER_SECTION:
        return State->KernelSources.DoDriverSection;
    case KSW_DYN_FIELD_ID_DO_MAJOR_FUNCTION:
        return State->KernelSources.DoMajorFunction;
    case KSW_DYN_FIELD_ID_DO_FAST_IO_DISPATCH:
        return State->KernelSources.DoFastIoDispatch;
    case KSW_DYN_FIELD_ID_DO_DRIVER_UNLOAD:
        return State->KernelSources.DoDriverUnload;
    case KSW_DYN_FIELD_ID_KG_PSP_CID_TABLE:
        return State->KernelGlobalSources.PspCidTable;
    case KSW_DYN_FIELD_ID_KG_PS_LOADED_MODULE_LIST:
        return State->KernelGlobalSources.PsLoadedModuleList;
    case KSW_DYN_FIELD_ID_KG_MM_UNLOADED_DRIVERS:
        return State->KernelGlobalSources.MmUnloadedDrivers;
    case KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE:
        return State->KernelGlobalSources.PiDDBCacheTable;
    case KSW_DYN_FIELD_ID_CB_PSP_CREATE_PROCESS_NOTIFY_ROUTINE:
        return State->CallbackGlobalSources.PspCreateProcessNotifyRoutine;
    case KSW_DYN_FIELD_ID_CB_PSP_CREATE_THREAD_NOTIFY_ROUTINE:
        return State->CallbackGlobalSources.PspCreateThreadNotifyRoutine;
    case KSW_DYN_FIELD_ID_CB_PSP_LOAD_IMAGE_NOTIFY_ROUTINE:
        return State->CallbackGlobalSources.PspLoadImageNotifyRoutine;
    case KSW_DYN_FIELD_ID_CB_PSP_NOTIFY_ENABLE_MASK:
        return State->CallbackGlobalSources.PspNotifyEnableMask;
    case KSW_DYN_FIELD_ID_CB_CM_CALLBACK_LIST_HEAD:
        return State->CallbackGlobalSources.CmCallbackListHead;
    case KSW_DYN_FIELD_ID_CB_OBJECT_TYPE_CALLBACK_LIST:
        return State->CallbackOffsetSources.ObjectTypeCallbackList;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_ENTRY_LIST:
        return State->CallbackOffsetSources.CallbackEntryItemEntryList;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_PRE_OPERATION:
        return State->CallbackOffsetSources.CallbackEntryItemPreOperation;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_POST_OPERATION:
        return State->CallbackOffsetSources.CallbackEntryItemPostOperation;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_OPERATIONS:
        return State->CallbackOffsetSources.CallbackEntryItemOperations;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ITEM_CALLBACK_ENTRY:
        return State->CallbackOffsetSources.CallbackEntryItemCallbackEntry;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_ALTITUDE:
        return State->CallbackOffsetSources.CallbackEntryAltitude;
    case KSW_DYN_FIELD_ID_CB_CALLBACK_ENTRY_REGISTRATION_CONTEXT:
        return State->CallbackOffsetSources.CallbackEntryRegistrationContext;
    default:
        return KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
    }
}

static VOID
KswordARKDynDataCopyAnsi(
    _Out_writes_bytes_(DestinationBytes) CHAR* Destination,
    _In_ size_t DestinationBytes,
    _In_opt_z_ PCSTR Source
    )
/*++

Routine Description:

    Copy a small descriptor string into an IOCTL response row while guaranteeing
    NUL termination.

Arguments:

    Destination - Output char buffer.
    DestinationBytes - Size of Destination in bytes.
    Source - Optional source string.

Return Value:

    None.

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

static PCSTR
KswordARKDynDataSourceName(
    _In_ ULONG Source
    )
/*++

Routine Description:

    Convert a field source code into a stable short diagnostic label.

Arguments:

    Source - KSW_DYN_FIELD_SOURCE_* value.

Return Value:

    Static string label.

--*/
{
    switch (Source) {
    case KSW_DYN_FIELD_SOURCE_SYSTEM_INFORMER:
        return "System Informer";
    case KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN:
        return "Runtime pattern";
    case KSW_DYN_FIELD_SOURCE_KSWORD_EXTRA_TABLE:
        return "Ksword extra";
    case KSW_DYN_FIELD_SOURCE_PDB_PROFILE:
        return "PDB profile";
    default:
        return "Unavailable";
    }
}

static BOOLEAN
KswordARKDynDataIsOffsetPresent(
    _In_ ULONG Offset
    )
/*++

Routine Description:

    Normalize DynData field availability. System Informer marks missing fields
    as 0xffff; Ksword stores unavailable fields as ULONG_MAX.

Arguments:

    Offset - Offset value to test.

Return Value:

    TRUE when usable, FALSE when unavailable.

--*/
{
    return (Offset != KSW_DYN_OFFSET_UNAVAILABLE && Offset != 0x0000FFFFUL) ? TRUE : FALSE;
}

ULONG
KswordARKDynDataCountFieldDescriptors(
    VOID
    )
/*++

Routine Description:

    Return the number of field descriptors exported to user mode.

Arguments:

    None.

Return Value:

    Descriptor count.

--*/
{
    return (ULONG)(sizeof(g_KswordDynFieldBindings) / sizeof(g_KswordDynFieldBindings[0]));
}

ULONG
KswordARKDynDataCopyFieldDescriptors(
    _In_ const KSW_DYN_STATE* State,
    _Out_writes_opt_(EntryCapacity) KSW_DYN_FIELD_ENTRY* Entries,
    _In_ ULONG EntryCapacity
    )
/*++

Routine Description:

    Build the public field table from the active state and static descriptor
    metadata. The caller may pass NULL Entries to query only total count.

Arguments:

    State - DynData state snapshot.
    Entries - Optional output entry array.
    EntryCapacity - Number of writable output entries.

Return Value:

    Number of entries copied to Entries.

--*/
{
    ULONG index = 0;
    ULONG copied = 0;
    ULONG totalCount = KswordARKDynDataCountFieldDescriptors();

    if (State == NULL || Entries == NULL || EntryCapacity == 0U) {
        return 0U;
    }

    for (index = 0; index < totalCount && copied < EntryCapacity; ++index) {
        const KSW_DYN_FIELD_BINDING* binding = &g_KswordDynFieldBindings[index];
        const ULONG offset = KswordARKDynDataReadBoundOffset(State, binding);
        const ULONG source = KswordARKDynDataReadBoundSource(State, binding);
        KSW_DYN_FIELD_ENTRY* entry = &Entries[copied];

        RtlZeroMemory(entry, sizeof(*entry));
        entry->fieldId = binding->FieldId;
        entry->offset = offset;
        entry->source = KswordARKDynDataIsOffsetPresent(offset) ? source : KSW_DYN_FIELD_SOURCE_UNAVAILABLE;
        entry->capabilityMask = binding->CapabilityMask;
        entry->flags = binding->Required ? KSW_DYN_FIELD_FLAG_REQUIRED : KSW_DYN_FIELD_FLAG_OPTIONAL;
        if (KswordARKDynDataIsOffsetPresent(offset)) {
            entry->flags |= KSW_DYN_FIELD_FLAG_PRESENT;
        }

        KswordARKDynDataCopyAnsi(entry->fieldName, sizeof(entry->fieldName), binding->FieldName);
        KswordARKDynDataCopyAnsi(entry->featureName, sizeof(entry->featureName), binding->FeatureName);
        KswordARKDynDataCopyAnsi(entry->sourceName, sizeof(entry->sourceName), KswordARKDynDataSourceName(entry->source));
        copied += 1U;
    }

    return copied;
}

static BOOLEAN
KswordARKDynDataHasAll(
    _In_ const ULONG* Offsets,
    _In_ ULONG Count
    )
/*++

Routine Description:

    Test a small dependency list for all-required field availability.

Arguments:

    Offsets - Dependency offset array.
    Count - Number of offsets in the array.

Return Value:

    TRUE when all fields are present; otherwise FALSE.

--*/
{
    ULONG index = 0;

    if (Offsets == NULL || Count == 0U) {
        return FALSE;
    }

    for (index = 0; index < Count; ++index) {
        if (!KswordARKDynDataIsOffsetPresent(Offsets[index])) {
            return FALSE;
        }
    }

    return TRUE;
}

static BOOLEAN
KswordARKDynDataHasAny(
    _In_ const ULONG* Offsets,
    _In_ ULONG Count
    )
/*++

Routine Description:

    Test whether at least one offset in a small optional dependency list is
    present.

Arguments:

    Offsets - Optional dependency offset array.
    Count - Number of offsets in the array.

Return Value:

    TRUE when one or more offsets are present; otherwise FALSE.

--*/
{
    ULONG index = 0;

    if (Offsets == NULL || Count == 0U) {
        return FALSE;
    }

    for (index = 0; index < Count; ++index) {
        if (KswordARKDynDataIsOffsetPresent(Offsets[index])) {
            return TRUE;
        }
    }

    return FALSE;
}

ULONG64
KswordARKDynDataComputeCapabilities(
    _In_ const KSW_DYN_STATE* State
    )
/*++

Routine Description:

    Compute feature capability flags from field availability. Capabilities are
    strict: a missing required field disables the dependent feature.

Arguments:

    State - DynData state snapshot with converted offsets.

Return Value:

    KSW_CAP_* bit mask.

--*/
{
    ULONG64 capabilities = KSW_DYN_CAPABILITY_NONE;

    if (State == NULL) {
        return KSW_DYN_CAPABILITY_NONE;
    }

    const ULONG objectTypeFields[] = { State->Kernel.OtName, State->Kernel.OtIndex };
    const ULONG processListFields[] = { State->Kernel.EpUniqueProcessId, State->Kernel.EpActiveProcessLinks, State->Kernel.EpThreadListHead, State->Kernel.EpImageFileName, State->Kernel.EpToken };
    const ULONG handleTableFields[] = { State->Kernel.EpObjectTable, State->Kernel.ObDecodeShift, State->Kernel.ObAttributesShift, State->Kernel.OtName, State->Kernel.OtIndex };
    const ULONG cidTableFields[] = { State->KernelGlobals.PspCidTable, State->Kernel.HtTableCode, State->Kernel.HteLowValue };
    const ULONG threadStackFields[] = { State->Kernel.KtInitialStack, State->Kernel.KtStackLimit, State->Kernel.KtStackBase, State->Kernel.KtKernelStack };
    const ULONG threadListFields[] = { State->Kernel.EpThreadListHead, State->Kernel.KtProcess, State->Kernel.EtCid, State->Kernel.EtThreadListEntry, State->Kernel.EtStartAddress };
    const ULONG threadIoFields[] = { State->Kernel.KtReadOperationCount, State->Kernel.KtWriteOperationCount, State->Kernel.KtOtherOperationCount, State->Kernel.KtReadTransferCount, State->Kernel.KtWriteTransferCount, State->Kernel.KtOtherTransferCount };
    const ULONG alpcFields[] = { State->Kernel.AlpcCommunicationInfo, State->Kernel.AlpcOwnerProcess, State->Kernel.AlpcConnectionPort, State->Kernel.AlpcServerCommunicationPort, State->Kernel.AlpcClientCommunicationPort, State->Kernel.AlpcHandleTable, State->Kernel.AlpcHandleTableLock, State->Kernel.AlpcAttributes, State->Kernel.AlpcAttributesFlags, State->Kernel.AlpcPortContext, State->Kernel.AlpcPortObjectLock, State->Kernel.AlpcSequenceNo, State->Kernel.AlpcState };
    const ULONG sectionFields[] = { State->Kernel.EpSectionObject, State->Kernel.MmSectionControlArea, State->Kernel.MmControlAreaListHead, State->Kernel.MmControlAreaLock };
    const ULONG kernelModuleListFields[] = { State->Kernel.KldrInLoadOrderLinks, State->Kernel.KldrDllBase, State->Kernel.KldrSizeOfImage, State->Kernel.KldrFullDllName, State->Kernel.KldrBaseDllName };
    const ULONG driverObjectFields[] = { State->Kernel.DoDriverStart, State->Kernel.DoDriverSize, State->Kernel.DoDriverSection, State->Kernel.DoMajorFunction, State->Kernel.DoDriverUnload };
    const ULONG protectionFields[] = { State->Kernel.EpProtection, State->Kernel.EpSignatureLevel, State->Kernel.EpSectionSignatureLevel };
    const ULONG etwFields[] = { State->Kernel.EgeGuid, State->Kernel.EreGuidEntry };
    const ULONG lxcoreFields[] = { State->LxcoreOffsets.LxPicoProc, State->LxcoreOffsets.LxPicoProcInfo, State->LxcoreOffsets.LxPicoProcInfoPID, State->LxcoreOffsets.LxPicoThrdInfo, State->LxcoreOffsets.LxPicoThrdInfoTID };
    const ULONG kernelGlobalFields[] = { State->KernelGlobals.PspCidTable, State->KernelGlobals.PsLoadedModuleList, State->KernelGlobals.MmUnloadedDrivers, State->KernelGlobals.PiDDBCacheTable };
    const ULONG callbackNotifyGlobals[] = { State->CallbackGlobals.PspCreateProcessNotifyRoutine, State->CallbackGlobals.PspCreateThreadNotifyRoutine, State->CallbackGlobals.PspLoadImageNotifyRoutine };
    const ULONG callbackRegistryGlobals[] = { State->CallbackGlobals.CmCallbackListHead };
    const ULONG callbackObjectFields[] = { State->CallbackOffsets.ObjectTypeCallbackList, State->CallbackOffsets.CallbackEntryItemPreOperation, State->CallbackOffsets.CallbackEntryItemPostOperation, State->CallbackOffsets.CallbackEntryItemOperations, State->CallbackOffsets.CallbackEntryItemCallbackEntry };

    if (State->NtosActive) {
        capabilities |= KSW_CAP_DYN_NTOS_ACTIVE;
    }
    if (State->LxcoreActive) {
        capabilities |= KSW_CAP_DYN_LXCORE_ACTIVE;
    }
    if (KswordARKDynDataHasAll(objectTypeFields, RTL_NUMBER_OF(objectTypeFields))) {
        capabilities |= KSW_CAP_OBJECT_TYPE_FIELDS;
    }
    if (KswordARKDynDataHasAll(processListFields, RTL_NUMBER_OF(processListFields))) {
        capabilities |= KSW_CAP_PROCESS_LIST_FIELDS;
    }
    if (KswordARKDynDataHasAll(handleTableFields, RTL_NUMBER_OF(handleTableFields))) {
        capabilities |= KSW_CAP_HANDLE_TABLE_DECODE | KSW_CAP_PROCESS_OBJECT_TABLE;
    }
    if (KswordARKDynDataHasAll(cidTableFields, RTL_NUMBER_OF(cidTableFields))) {
        capabilities |= KSW_CAP_CID_TABLE_WALK;
    }
    if (KswordARKDynDataHasAll(threadStackFields, RTL_NUMBER_OF(threadStackFields))) {
        capabilities |= KSW_CAP_THREAD_STACK_FIELDS;
    }
    if (KswordARKDynDataHasAll(threadListFields, RTL_NUMBER_OF(threadListFields))) {
        capabilities |= KSW_CAP_THREAD_LIST_FIELDS;
    }
    if (KswordARKDynDataHasAll(threadIoFields, RTL_NUMBER_OF(threadIoFields))) {
        capabilities |= KSW_CAP_THREAD_IO_COUNTERS;
    }
    if (KswordARKDynDataHasAll(alpcFields, RTL_NUMBER_OF(alpcFields))) {
        capabilities |= KSW_CAP_ALPC_FIELDS;
    }
    if (KswordARKDynDataHasAll(sectionFields, RTL_NUMBER_OF(sectionFields))) {
        capabilities |= KSW_CAP_SECTION_CONTROL_AREA;
    }
    if (KswordARKDynDataHasAll(kernelModuleListFields, RTL_NUMBER_OF(kernelModuleListFields))) {
        capabilities |= KSW_CAP_KERNEL_MODULE_LIST_FIELDS;
    }
    if (KswordARKDynDataHasAll(driverObjectFields, RTL_NUMBER_OF(driverObjectFields))) {
        capabilities |= KSW_CAP_DRIVER_OBJECT_FIELDS;
    }
    if (KswordARKDynDataHasAll(protectionFields, RTL_NUMBER_OF(protectionFields))) {
        capabilities |= KSW_CAP_PROCESS_PROTECTION_PATCH;
    }
    if (KswordARKDynDataHasAll(etwFields, RTL_NUMBER_OF(etwFields))) {
        capabilities |= KSW_CAP_ETW_GUID_FIELDS;
    }
    if (State->LxcoreActive && KswordARKDynDataHasAll(lxcoreFields, RTL_NUMBER_OF(lxcoreFields))) {
        capabilities |= KSW_CAP_WSL_LXCORE_FIELDS;
    }
    if (State->CallbackProfileActive && KswordARKDynDataHasAll(callbackNotifyGlobals, RTL_NUMBER_OF(callbackNotifyGlobals))) {
        capabilities |= KSW_CAP_CALLBACK_NOTIFY_GLOBALS;
    }
    if (State->CallbackProfileActive && KswordARKDynDataHasAll(callbackRegistryGlobals, RTL_NUMBER_OF(callbackRegistryGlobals))) {
        capabilities |= KSW_CAP_CALLBACK_REGISTRY_GLOBALS;
    }
    if (State->CallbackProfileActive && KswordARKDynDataHasAll(callbackObjectFields, RTL_NUMBER_OF(callbackObjectFields))) {
        capabilities |= KSW_CAP_CALLBACK_OBJECT_FIELDS;
    }
    if (KswordARKDynDataHasAny(kernelGlobalFields, RTL_NUMBER_OF(kernelGlobalFields))) {
        capabilities |= KSW_CAP_KERNEL_GLOBALS;
    }

    return capabilities;
}
