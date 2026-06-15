# DynData / PDB profile v3 manifest

本文件记录下一阶段 cross-view、驱动完整性和内核内存归因所需的 DynData
协议扩展。本文只描述 schema 和协议面，不要求重新生成
`profiles/ark_dyndata_pack_v1.json`、`profiles/ark_dyndata_pack_v2.json` 或写入
`Release`。

## 新增 capability 位

| 名称 | 值 | 说明 |
| --- | ---: | --- |
| `KSW_CAP_PROCESS_LIST_FIELDS` | `0x0000000000008000` | `_EPROCESS` 进程链表、PID、镜像名和 Token 字段齐备。 |
| `KSW_CAP_THREAD_LIST_FIELDS` | `0x0000000000010000` | `_KTHREAD` / `_ETHREAD` 线程归属、线程链和入口地址字段齐备。 |
| `KSW_CAP_CID_TABLE_WALK` | `0x0000000000020000` | `PspCidTable` RVA 与句柄表解码关键字段齐备。 |
| `KSW_CAP_KERNEL_MODULE_LIST_FIELDS` | `0x0000000000040000` | `_KLDR_DATA_TABLE_ENTRY` 模块链表字段齐备。 |
| `KSW_CAP_DRIVER_OBJECT_FIELDS` | `0x0000000000080000` | `_DRIVER_OBJECT` 起止范围、节对象和分发表字段齐备。 |
| `KSW_CAP_KERNEL_GLOBALS` | `0x0000000000100000` | 至少一个 v3 optional kernel `GlobalRva` 已由 PDB profile 提供。 |

## 新增字段 ID

| ID | 名称 | 类型 | 必需性 | 目标 |
| ---: | --- | --- | --- | --- |
| 57 | `KSW_DYN_FIELD_ID_EP_UNIQUE_PROCESS_ID` | `StructOffset` | required | `_EPROCESS.UniqueProcessId` |
| 58 | `KSW_DYN_FIELD_ID_EP_ACTIVE_PROCESS_LINKS` | `StructOffset` | required | `_EPROCESS.ActiveProcessLinks` |
| 59 | `KSW_DYN_FIELD_ID_EP_THREAD_LIST_HEAD` | `StructOffset` | required | `_EPROCESS.ThreadListHead` |
| 60 | `KSW_DYN_FIELD_ID_EP_IMAGE_FILE_NAME` | `StructOffset` | required | `_EPROCESS.ImageFileName` |
| 61 | `KSW_DYN_FIELD_ID_EP_TOKEN` | `StructOffset` | required | `_EPROCESS.Token` |
| 62 | `KSW_DYN_FIELD_ID_ET_CID` | `StructOffset` | required | `_ETHREAD.Cid` |
| 63 | `KSW_DYN_FIELD_ID_ET_THREAD_LIST_ENTRY` | `StructOffset` | required | `_ETHREAD.ThreadListEntry` |
| 64 | `KSW_DYN_FIELD_ID_ET_START_ADDRESS` | `StructOffset` | required | `_ETHREAD.StartAddress` |
| 65 | `KSW_DYN_FIELD_ID_ET_WIN32_START_ADDRESS` | `StructOffset` | optional | `_ETHREAD.Win32StartAddress` |
| 66 | `KSW_DYN_FIELD_ID_KT_PROCESS` | `StructOffset` | required | `_KTHREAD.Process` |
| 67 | `KSW_DYN_FIELD_ID_HT_TABLE_CODE` | `StructOffset` | required | `_HANDLE_TABLE.TableCode` |
| 68 | `KSW_DYN_FIELD_ID_HT_HANDLE_COUNT` | `StructOffset` | required | `_HANDLE_TABLE.HandleCount` |
| 69 | `KSW_DYN_FIELD_ID_HTE_LOW_VALUE` | `StructOffset` | required | `_HANDLE_TABLE_ENTRY.LowValue` |
| 70 | `KSW_DYN_FIELD_ID_KLDR_IN_LOAD_ORDER_LINKS` | `StructOffset` | required | `_KLDR_DATA_TABLE_ENTRY.InLoadOrderLinks` |
| 71 | `KSW_DYN_FIELD_ID_KLDR_DLL_BASE` | `StructOffset` | required | `_KLDR_DATA_TABLE_ENTRY.DllBase` |
| 72 | `KSW_DYN_FIELD_ID_KLDR_SIZE_OF_IMAGE` | `StructOffset` | required | `_KLDR_DATA_TABLE_ENTRY.SizeOfImage` |
| 73 | `KSW_DYN_FIELD_ID_KLDR_FULL_DLL_NAME` | `StructOffset` | required | `_KLDR_DATA_TABLE_ENTRY.FullDllName` |
| 74 | `KSW_DYN_FIELD_ID_KLDR_BASE_DLL_NAME` | `StructOffset` | required | `_KLDR_DATA_TABLE_ENTRY.BaseDllName` |
| 75 | `KSW_DYN_FIELD_ID_KLDR_FLAGS` | `StructOffset` | optional | `_KLDR_DATA_TABLE_ENTRY.Flags` |
| 76 | `KSW_DYN_FIELD_ID_DO_DRIVER_START` | `StructOffset` | required | `_DRIVER_OBJECT.DriverStart` |
| 77 | `KSW_DYN_FIELD_ID_DO_DRIVER_SIZE` | `StructOffset` | required | `_DRIVER_OBJECT.DriverSize` |
| 78 | `KSW_DYN_FIELD_ID_DO_DRIVER_SECTION` | `StructOffset` | required | `_DRIVER_OBJECT.DriverSection` |
| 79 | `KSW_DYN_FIELD_ID_DO_MAJOR_FUNCTION` | `StructOffset` | required | `_DRIVER_OBJECT.MajorFunction` |
| 80 | `KSW_DYN_FIELD_ID_DO_FAST_IO_DISPATCH` | `StructOffset` | optional | `_DRIVER_OBJECT.FastIoDispatch` |
| 81 | `KSW_DYN_FIELD_ID_DO_DRIVER_UNLOAD` | `StructOffset` | required | `_DRIVER_OBJECT.DriverUnload` |
| 82 | `KSW_DYN_FIELD_ID_KG_PSP_CID_TABLE` | `GlobalRva` | optional | `PspCidTable` |
| 83 | `KSW_DYN_FIELD_ID_KG_PS_LOADED_MODULE_LIST` | `GlobalRva` | optional | `PsLoadedModuleList` |
| 84 | `KSW_DYN_FIELD_ID_KG_MM_UNLOADED_DRIVERS` | `GlobalRva` | optional | `MmUnloadedDrivers` |
| 85 | `KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE` | `GlobalRva` | optional | `PiDDBCacheTable` |

`KSW_DYN_FIELD_ID_MAX` 更新为 `KSW_DYN_FIELD_ID_KG_PIDDB_CACHE_TABLE`。

## pack v3 schema

pack v3 保留 v1/v2 顶层 identity 和 `fields` 布局，新增推荐字段
`profiles[].items`。R3 loader 支持：

- `packVersion: 1`：仅 `fieldDictionary` + `fields`。
- `packVersion: 2`：v1 + `callbackItems`。
- `packVersion: 3`：v1/v2 兼容字段 + `items` typed payload。

示例：

```json
{
  "schemaVersion": 1,
  "packVersion": 3,
  "fieldDictionary": ["EpUniqueProcessId", "EpActiveProcessLinks"],
  "profiles": [
    {
      "moduleClassId": 0,
      "machine": 34404,
      "timeDateStamp": 305419896,
      "sizeOfImage": 21299200,
      "profileName": "ntoskrnl_amd64_example",
      "pdbName": "ntkrnlmp.pdb",
      "pdbGuid": "00000000-0000-0000-0000-000000000000",
      "pdbAge": 1,
      "fields": [[0, 1088], [1, 1096]],
      "items": [
        { "name": "EpUniqueProcessId", "kind": "StructOffset", "value": 1088 },
        { "name": "PspCidTable", "kind": "GlobalRva", "value": 1193046 }
      ],
      "missingFields": ["_ETHREAD->Win32StartAddress"],
      "missingGlobals": ["PiDDBCacheTable"],
      "coveragePercent": 96.3
    }
  ]
}
```

`items[].kind` 只允许：

- `StructOffset`：结构成员偏移，值必须不是 `0xFFFFFFFF` 且不超过
  `KSW_DYN_PROFILE_OFFSET_MAX`。
- `GlobalRva`：ntoskrnl 内全局符号 RVA，值必须非零、小于当前
  `SizeOfImage` 且不超过 `KSW_DYN_PROFILE_GLOBAL_RVA_MAX`。

全局 RVA 是 optional：生成器找不到 `PspCidTable`、`PsLoadedModuleList`、
`MmUnloadedDrivers` 或 `PiDDBCacheTable` 时不得导致 profile 失败，只写入
`missingGlobals` / diagnostics。

## 已接入面

- R0/R3 共享协议新增字段 ID 和 capability 位。
- R0 DynData state 可保存新增结构偏移和 optional kernel global RVA。
- R0 EX apply 支持 v3 通用 `StructOffset` / `GlobalRva` item，并继续兼容
  v2 callback items。
- R3 KernelDock profile loader 支持 packVersion 1/2/3。
- PDB generator 扩展 `FIELD_MAP` 并解析 optional kernel global RVAs。
- release sync 支持 v3 pack 输出，并在报告中输出每个 profile 的
  `missingFields`、`missingGlobals`、`coveragePercent`。

## 未完成项

- 未重新生成任何 `profiles/ark_dyndata_pack_v1.json` /
  `profiles/ark_dyndata_pack_v2.json` / v3 pack。
- 未构建驱动、用户态或 Qt 项目。
- 新 capability 当前只暴露 DynData/profile 面；cross-view 枚举、驱动完整性
  校验和内核内存归因的业务 IOCTL/worker 尚未接入。
- v3 coverage 依赖生成器和 release sync 的静态报告，尚未在 R0 协议中新增
  独立 coverage 查询字段。
