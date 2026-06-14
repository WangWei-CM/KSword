# KswordARKDriver IOCTL Audit Report

- Generated at: `2026-06-13T18:57:22.844608+00:00`
- Repository root: `D:\Projects\Ksword5.1`
- Headers scanned: `20`
- Registry scanned: `KswordARKDriver/src/dispatch/ioctl_registry.c`

## Summary

| Metric | Value |
| --- | --- |
| Shared IOCTL definitions | 63 |
| Registry entries | 63 |
| Registered definitions | 63 |
| Unregistered definitions | 0 |
| HIGH findings | 8 |
| MEDIUM findings | 0 |
| LOW findings | 0 |

## High-risk findings

| Category | IOCTL | Message | Details |
| --- | --- | --- | --- |
| mutating_any_access | IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["ANSWER"], "source": "shared/driver/KswordArkCallbackIoctl.h", "line": 45} |
| mutating_any_access | IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["CANCEL"], "source": "shared/driver/KswordArkCallbackIoctl.h", "line": 52} |
| mutating_any_access | IOCTL_KSWORD_ARK_DELETE_PATH | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["DELETE"], "source": "shared/driver/KswordArkFileIoctl.h", "line": 18} |
| mutating_any_access | IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["CONTROL"], "source": "shared/driver/KswordArkFileMonitorIoctl.h", "line": 19} |
| mutating_any_access | IOCTL_KSWORD_ARK_SET_CALLBACK_RULES | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["SET"], "source": "shared/driver/KswordArkCallbackIoctl.h", "line": 24} |
| mutating_any_access | IOCTL_KSWORD_ARK_SET_PPL_LEVEL | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["SET"], "source": "shared/driver/KswordArkProcessIoctl.h", "line": 65} |
| mutating_any_access | IOCTL_KSWORD_ARK_SUSPEND_PROCESS | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["SUSPEND"], "source": "shared/driver/KswordArkProcessIoctl.h", "line": 53} |
| mutating_any_access | IOCTL_KSWORD_ARK_TERMINATE_PROCESS | Mutating keyword matched but access is FILE_ANY_ACCESS. | {"keywords": ["TERMINATE"], "source": "shared/driver/KswordArkProcessIoctl.h", "line": 40} |

## All findings

| Severity | Category | Name | Message |
| --- | --- | --- | --- |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_DELETE_PATH | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_SET_CALLBACK_RULES | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_SET_PPL_LEVEL | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_SUSPEND_PROCESS | Mutating keyword matched but access is FILE_ANY_ACCESS. |
| HIGH | mutating_any_access | IOCTL_KSWORD_ARK_TERMINATE_PROCESS | Mutating keyword matched but access is FILE_ANY_ACCESS. |

## IOCTL inventory

| IOCTL | Function | Method | Access | Registered | Handler | Source |
| --- | --- | --- | --- | --- | --- | --- |
| IOCTL_KSWORD_ARK_QUERY_ALPC_PORT | 0x80E | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKAlpcIoctlQueryAlpcPort | shared/driver/KswordArkAlpcIoctl.h:17 |
| IOCTL_KSWORD_ARK_SET_CALLBACK_RULES | 0x880 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCallbackIoctlSetRulesHandler | shared/driver/KswordArkCallbackIoctl.h:24 |
| IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE | 0x881 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCallbackIoctlGetRuntimeStateHandler | shared/driver/KswordArkCallbackIoctl.h:31 |
| IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT | 0x882 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCallbackIoctlWaitEventHandler | shared/driver/KswordArkCallbackIoctl.h:38 |
| IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT | 0x883 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCallbackIoctlAnswerEventHandler | shared/driver/KswordArkCallbackIoctl.h:45 |
| IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS | 0x884 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCallbackIoctlCancelAllPendingHandler | shared/driver/KswordArkCallbackIoctl.h:52 |
| IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK | 0x885 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKCallbackIoctlRemoveExternalCallbackHandler | shared/driver/KswordArkCallbackIoctl.h:59 |
| IOCTL_KSWORD_ARK_ENUM_CALLBACKS | 0x886 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCallbackIoctlEnumCallbacksHandler | shared/driver/KswordArkCallbackIoctl.h:66 |
| IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES | 0x80A | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKCapabilityIoctlQueryDriverCapabilities | shared/driver/KswordArkCapabilityIoctl.h:19 |
| IOCTL_KSWORD_ARK_QUERY_DYN_STATUS | 0x807 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKDynDataIoctlQueryStatus | shared/driver/KswordArkDynDataIoctl.h:20 |
| IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS | 0x808 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKDynDataIoctlQueryFields | shared/driver/KswordArkDynDataIoctl.h:27 |
| IOCTL_KSWORD_ARK_QUERY_CAPABILITIES | 0x809 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKDynDataIoctlQueryCapabilities | shared/driver/KswordArkDynDataIoctl.h:34 |
| IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE | 0x82F | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKDynDataIoctlApplyProfile | shared/driver/KswordArkDynDataIoctl.h:41 |
| IOCTL_KSWORD_ARK_DELETE_PATH | 0x804 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKFileIoctlDeletePath | shared/driver/KswordArkFileIoctl.h:18 |
| IOCTL_KSWORD_ARK_QUERY_FILE_INFO | 0x812 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKFileIoctlQueryFileInfo | shared/driver/KswordArkFileIoctl.h:25 |
| IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL | 0x815 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKFileMonitorIoctlControl | shared/driver/KswordArkFileMonitorIoctl.h:19 |
| IOCTL_KSWORD_ARK_FILE_MONITOR_DRAIN | 0x816 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKFileMonitorIoctlDrain | shared/driver/KswordArkFileMonitorIoctl.h:26 |
| IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS | 0x817 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKFileMonitorIoctlQueryStatus | shared/driver/KswordArkFileMonitorIoctl.h:33 |
| IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES | 0x80C | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKHandleIoctlEnumProcessHandles | shared/driver/KswordArkHandleIoctl.h:18 |
| IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT | 0x80D | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKHandleIoctlQueryHandleObject | shared/driver/KswordArkHandleIoctl.h:25 |
| IOCTL_KSWORD_ARK_ENUM_SSDT | 0x806 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKernelIoctlEnumSsdt | shared/driver/KswordArkKernelIoctl.h:21 |
| IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT | 0x811 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKernelIoctlQueryDriverObject | shared/driver/KswordArkKernelIoctl.h:28 |
| IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT | 0x81E | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKernelIoctlEnumShadowSsdt | shared/driver/KswordArkKernelIoctl.h:35 |
| IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS | 0x81F | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKernelIoctlScanInlineHooks | shared/driver/KswordArkKernelIoctl.h:42 |
| IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK | 0x820 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKKernelIoctlPatchInlineHook | shared/driver/KswordArkKernelIoctl.h:49 |
| IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS | 0x821 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKernelIoctlEnumIatEatHooks | shared/driver/KswordArkKernelIoctl.h:56 |
| IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER | 0x826 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKKernelIoctlForceUnloadDriver | shared/driver/KswordArkKernelIoctl.h:63 |
| IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS | 0x847 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKeyboardIoctlEnumHotkeys | shared/driver/KswordArkKeyboardIoctl.h:18 |
| IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS | 0x848 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKKeyboardIoctlEnumHooks | shared/driver/KswordArkKeyboardIoctl.h:25 |
| IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY | 0x813 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKMemoryIoctlQueryVirtualMemory | shared/driver/KswordArkMemoryIoctl.h:25 |
| IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY | 0x814 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKMemoryIoctlReadVirtualMemory | shared/driver/KswordArkMemoryIoctl.h:32 |
| IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY | 0x81D | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKMemoryIoctlWriteVirtualMemory | shared/driver/KswordArkMemoryIoctl.h:39 |
| IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY | 0x82B | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKMemoryIoctlReadPhysicalMemory | shared/driver/KswordArkMemoryIoctl.h:46 |
| IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY | 0x82C | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKMemoryIoctlWritePhysicalMemory | shared/driver/KswordArkMemoryIoctl.h:53 |
| IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS | 0x82D | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKMemoryIoctlTranslateVirtualAddress | shared/driver/KswordArkMemoryIoctl.h:60 |
| IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY | 0x82E | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKMemoryIoctlQueryPageTableEntry | shared/driver/KswordArkMemoryIoctl.h:67 |
| IOCTL_KSWORD_ARK_NETWORK_SET_RULES | 0x829 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKNetworkIoctlSetRules | shared/driver/KswordArkNetworkIoctl.h:18 |
| IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS | 0x82A | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKNetworkIoctlQueryStatus | shared/driver/KswordArkNetworkIoctl.h:25 |
| IOCTL_KSWORD_ARK_QUERY_PREFLIGHT | 0x81C | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKPreflightIoctlQuery | shared/driver/KswordArkPreflightIoctl.h:19 |
| IOCTL_KSWORD_ARK_TERMINATE_PROCESS | 0x801 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKProcessIoctlTerminate | shared/driver/KswordArkProcessIoctl.h:40 |
| IOCTL_KSWORD_ARK_SUSPEND_PROCESS | 0x802 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKProcessIoctlSuspend | shared/driver/KswordArkProcessIoctl.h:53 |
| IOCTL_KSWORD_ARK_SET_PPL_LEVEL | 0x803 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKProcessIoctlSetPplLevel | shared/driver/KswordArkProcessIoctl.h:65 |
| IOCTL_KSWORD_ARK_ENUM_PROCESS | 0x805 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKProcessIoctlEnumProcess | shared/driver/KswordArkProcessIoctl.h:79 |
| IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY | 0x822 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKProcessIoctlSetVisibility | shared/driver/KswordArkProcessIoctl.h:196 |
| IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS | 0x824 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKProcessIoctlSetSpecialFlags | shared/driver/KswordArkProcessIoctl.h:221 |
| IOCTL_KSWORD_ARK_DKOM_PROCESS | 0x825 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKProcessIoctlDkomProcess | shared/driver/KswordArkProcessIoctl.h:262 |
| IOCTL_KSWORD_ARK_REDIRECT_SET_RULES | 0x827 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRedirectIoctlSetRules | shared/driver/KswordArkRedirectIoctl.h:18 |
| IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS | 0x828 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKRedirectIoctlQueryStatus | shared/driver/KswordArkRedirectIoctl.h:25 |
| IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE | 0x823 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKRegistryIoctlReadValue | shared/driver/KswordArkRegistryIoctl.h:23 |
| IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY | 0x840 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKRegistryIoctlEnumKey | shared/driver/KswordArkRegistryIoctl.h:30 |
| IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE | 0x841 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRegistryIoctlSetValue | shared/driver/KswordArkRegistryIoctl.h:37 |
| IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE | 0x842 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRegistryIoctlDeleteValue | shared/driver/KswordArkRegistryIoctl.h:44 |
| IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY | 0x843 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRegistryIoctlCreateKey | shared/driver/KswordArkRegistryIoctl.h:51 |
| IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY | 0x844 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRegistryIoctlDeleteKey | shared/driver/KswordArkRegistryIoctl.h:58 |
| IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE | 0x845 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRegistryIoctlRenameValue | shared/driver/KswordArkRegistryIoctl.h:65 |
| IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY | 0x846 | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKRegistryIoctlRenameKey | shared/driver/KswordArkRegistryIoctl.h:72 |
| IOCTL_KSWORD_ARK_QUERY_SAFETY_POLICY | 0x81A | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKSafetyIoctlQueryPolicy | shared/driver/KswordArkSafetyIoctl.h:18 |
| IOCTL_KSWORD_ARK_SET_SAFETY_POLICY | 0x81B | METHOD_BUFFERED | FILE_WRITE_ACCESS | yes | KswordARKSafetyIoctlSetPolicy | shared/driver/KswordArkSafetyIoctl.h:25 |
| IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION | 0x80F | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKSectionIoctlQueryProcessSection | shared/driver/KswordArkSectionIoctl.h:18 |
| IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS | 0x810 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKSectionIoctlQueryFileSectionMappings | shared/driver/KswordArkSectionIoctl.h:25 |
| IOCTL_KSWORD_ARK_ENUM_THREAD | 0x80B | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKThreadIoctlEnumThread | shared/driver/KswordArkThreadIoctl.h:17 |
| IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST | 0x819 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKTrustIoctlQueryImageTrust | shared/driver/KswordArkTrustIoctl.h:17 |
| IOCTL_KSWORD_ARK_QUERY_WSL_SILO | 0x818 | METHOD_BUFFERED | FILE_ANY_ACCESS | yes | KswordARKWslSiloIoctlQuery | shared/driver/KswordArkWslSiloIoctl.h:17 |

## Rule notes

```json
{
  "allowedAnyAccess": [],
  "mutatingKeywords": [
    "SET",
    "WRITE",
    "PATCH",
    "TERMINATE",
    "KILL",
    "UNLOAD",
    "DELETE",
    "CREATE",
    "RENAME",
    "HIDE",
    "PROTECT",
    "APPLY",
    "SUSPEND",
    "CONTROL",
    "CANCEL",
    "REMOVE",
    "ANSWER",
    "DKOM"
  ],
  "queryKeywords": [
    "QUERY",
    "READ",
    "ENUM",
    "GET",
    "SCAN",
    "WAIT",
    "DRAIN",
    "TRANSLATE",
    "STATUS"
  ],
  "ignoredHeaders": [],
  "notes": {
    "purpose": "Policy file for the KswordARKDriver IOCTL static auditor.",
    "allowedAnyAccess": "Add a full IOCTL_KSWORD_ARK_* name here only when FILE_ANY_ACCESS is intentional and documented.",
    "mutatingKeywords": "Tokens that usually indicate state-changing or destructive operations and should not stay FILE_ANY_ACCESS by default.",
    "queryKeywords": "Tokens that usually indicate read-only/query operations and should not require FILE_WRITE_ACCESS unless there is a documented reason.",
    "ignoredHeaders": "Use repository-relative paths or basenames for generated/legacy headers that should be skipped."
  }
}
```
