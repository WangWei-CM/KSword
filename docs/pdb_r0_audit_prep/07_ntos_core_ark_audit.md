# ntoskrnl / ntkrla57 PDB Core ARK Audit Prep

本文仅做 PDB R0 核心 ARK 审计增强的前期设计和盘点，不修改主项目源码、`tools` 或协议。目标模块为 `ntoskrnl` / `ntkrla57` 动态偏移与只读审计能力。

## 读取范围

| 类别 | 文件/目录 | 用途 |
|---|---|---|
| R0/R3 协议 | `shared/driver/KswordArkProcessIoctl.h`, `KswordArkThreadIoctl.h`, `KswordArkHandleIoctl.h`, `KswordArkKernelIoctl.h`, `KswordArkCallbackIoctl.h` | 确认现有 IOCTL、row 字段、状态位、source/anomaly/risk 位 |
| R0 实现 | `KswordARKDriver/src/features/process`, `thread`, `handle`, `kernel`, `callback` | 判断是否已有实现、已注册、仍需 PDB 字段补强 |
| PDB/DynData | `shared/driver/KswordArkDynDataIoctl.h`, `KswordARKDriver/src/features/dyndata`, `tools/pdb_offset_generator/` | 盘点字段 ID、字段来源和 generator 覆盖 |
| 既有规划 | `docs/next_phase_manifests/process_thread_crossview.md`, `driver_kernel_integrity.md` | 对齐 cross-view / driver integrity 设计意图 |

## 当前 DynData 覆盖

| 分组 | 已覆盖字段/全局 | 当前用途 | 备注 |
|---|---|---|---|
| EPROCESS | `EpObjectTable`, `EpSectionObject`, `EpUniqueProcessId`, `EpActiveProcessLinks`, `EpThreadListHead`, `EpImageFileName`, `EpToken`, `EpProtection`, `EpSignatureLevel`, `EpSectionSignatureLevel` | 进程枚举、隐藏/恢复、process cross-view、句柄表、Section、PPL/签名展示 | `UniqueProcessId` / `ActiveProcessLinks` / `ThreadListHead` / `ImageFileName` / `Token` 已切到 PDB profile 来源 |
| ETHREAD / KTHREAD | `EtCid`, `EtThreadListEntry`, `EtStartAddress`, `EtWin32StartAddress`, `KtProcess`, `KtInitialStack`, `KtStackLimit`, `KtStackBase`, `KtKernelStack`, `KtReadOperationCount`, `KtWriteOperationCount`, `KtOtherOperationCount`, `KtReadTransferCount`, `KtWriteTransferCount`, `KtOtherTransferCount` | 线程扩展枚举、thread cross-view、start address 校验、栈/I/O counter | `EtWin32StartAddress` 为可选字段 |
| Handle table / Object type | `HtHandleContentionEvent`, `HtTableCode`, `HtHandleCount`, `HteLowValue`, `OtName`, `OtIndex`, `ObDecodeShift`, `ObAttributesShift` | `PspCidTable` walk、进程句柄表 decode、对象类型名/索引查询 | 缺少通用 `_OBJECT_HEADER` schema |
| KLDR / module list | `KldrInLoadOrderLinks`, `KldrDllBase`, `KldrSizeOfImage`, `KldrFullDllName`, `KldrBaseDllName`, `KldrFlags` | `PsLoadedModuleList` 对齐、DriverSection 校验、driver integrity | 入口级枚举已具备基础字段 |
| DriverObject | `DoDriverStart`, `DoDriverSize`, `DoDriverSection`, `DoMajorFunction`, `DoFastIoDispatch`, `DoDriverUnload` | DriverObject 查询、DriverObject/IRP hook 风险评估、driver integrity | 还缺 DeviceObject 字段 PDB 化 |
| Kernel globals | `PspCidTable`, `PsLoadedModuleList`, `MmUnloadedDrivers`, `PiDDBCacheTable` | CID walk、模块 cross-view、卸载驱动痕迹、PiDDB 可用性 | `MmUnloadedDrivers` / `PiDDBCacheTable` 目前偏全局地址可用性，entry schema 待补 |
| Callback globals/fields | `PspCreateProcessNotifyRoutine`, `PspCreateThreadNotifyRoutine`, `PspLoadImageNotifyRoutine`, `PspNotifyEnableMask`, `CmCallbackListHead`, `_OBJECT_TYPE.CallbackList`, `_CALLBACK_ENTRY_ITEM.*`, `_CALLBACK_ENTRY.Altitude`, `_CALLBACK_ENTRY.RegistrationContext` | process/thread/image notify、registry callback、Ob callback 枚举与移除候选 | callback profile schema 已在 generator 中独立处理 |

## 增强清单

| 优先级 | 项目 | 当前仓库状态 | 需要补的 PDB 字段 | R0 输出字段 | UI 展示字段 | 验收方式 |
|---|---|---|---|---|---|---|
| P0 | process cross-view | 已有协议、R0 `process_crossview.c`、IOCTL 注册；source 包括 public walk、ActiveProcessLinks、PspCidTable | 已有：`EpUniqueProcessId`, `EpActiveProcessLinks`, `EpThreadListHead`, `EpImageFileName`, `PspCidTable`, `HtTableCode`, `HteLowValue`；候选：`_EPROCESS.Flags`, `ExitStatus`, `RundownProtect` 用于退出态降噪 | `objectAddress`, PID/PPID, `sourceMask`, `anomalyFlags`, `confidence`, `fieldOffsets`, `detail` | PID、进程名、来源矩阵、异常标签、EPROCESS 地址、置信度、DynData 缺口 | 构造只摘 `ActiveProcessLinks`、只改 `UniqueProcessId`、移除 CID 的测试进程，分别确认 anomaly |
| P0 | thread cross-view | 已有协议、R0 `thread_crossview.c`、IOCTL 注册；source 包括 public walk、`ThreadListHead`、CID | 已有：`EpThreadListHead`, `EtThreadListEntry`, `EtCid`, `EtStartAddress`, `EtWin32StartAddress`, `KtProcess`, `PspCidTable`, `HtTableCode`, `HteLowValue`；候选：`_ETHREAD.CrossThreadFlags`, termination/APC 状态字段 | TID/PID, ETHREAD, owning EPROCESS, start address, `sourceMask`, `anomalyFlags`, `confidence`, `detail` | TID、PID、进程名、线程来源矩阵、start module、异常标签 | 人工从 `ThreadListHead` 摘链、CID 缺失、orphan owner 三类样本，确认 UI 可区分 |
| P0 | PspCidTable walk | 已被 process/thread cross-view 和 enum 补扫使用；handle decode 也复用表解码 | 已有：`PspCidTable`, `HtTableCode`, `HteLowValue`, `EtCid`, `EpUniqueProcessId`；候选：HANDLE_TABLE 层级/entry flags 更完整 schema | CID entry index/handle value、object address、object type match、reference status、dangling status | CID-only、dangling、type mismatch、lookup status | 与 `PsLookupProcessByProcessId` / `PsLookupThreadByThreadId` 交叉比对；无效 entry 不蓝屏、不越界 |
| P0 | PsLoadedModuleList | 已有 KLDR 字段和 `driver_integrity.c` 只读 walk；IOCTL 已注册 | 已有：`PsLoadedModuleList`, `Kldr*`；候选：`_KLDR_DATA_TABLE_ENTRY.LoadCount`, `SectionPointer`, `CheckSum`, `TimeDateStamp` | module base/size/name/full path/flags/source/risk | SystemModule vs AuxKlib vs PsLoadedModuleList 对照、缺失/孤儿模块标签 | 隐藏驱动样本：从 KLDR 摘链、仅 SystemModule 可见、DriverObject 孤立，分别出风险 |
| P0 | DriverObject / DeviceObject | 已有 DriverObject 查询、driver integrity、MajorFunction、FastIo、DeviceObject/attached chain 基础审计 | 已有：`DoDriverStart`, `DoDriverSize`, `DoDriverSection`, `DoMajorFunction`, `DoFastIoDispatch`, `DoDriverUnload`；候选：`_DRIVER_OBJECT.DeviceObject`, `Flags`, `DriverExtension`, `DriverName`, `HardwareDatabase`; `_DEVICE_OBJECT.NextDevice`, `AttachedDevice`, `DriverObject`, `DeviceType`, `Characteristics`, `StackSize` | DriverObject address/start/size/section/unload、major/fastio target、device chain、risk flags | Driver card、IRP 表、FastIo 表、设备链、owner module、风险分 | DriverObject 指向合法镜像得低分；MajorFunction/FastIo 指到外部模块或无模块得高分 |
| P0 | callback notify / registry / Ob | 已有 callback enum、runtime、remove、规则链路；PDB callback fields 已覆盖 | 已有：`PspCreateProcessNotifyRoutine`, `PspCreateThreadNotifyRoutine`, `PspLoadImageNotifyRoutine`, `CmCallbackListHead`, `_OBJECT_TYPE.CallbackList`, `_CALLBACK_ENTRY_ITEM.*`, `_CALLBACK_ENTRY.*`；候选：notify array entry schema、callback block flags | callback class/source/status/address/context/registration/module/altitude/operationMask/trust/removeBehavior | 回调类型、模块、altitude、可信来源、可移除性、raw storage | 注册自测回调、第三方回调、失效模块地址回调，确认枚举和模块归属 |
| P1 | handle table decode | 已有 `handle_query.c`, `handle_object_query.c`、IOCTL 注册、object type 查询 | 已有：`EpObjectTable`, `HtTableCode`, `HtHandleCount`, `HteLowValue`, `ObDecodeShift`, `ObAttributesShift`, `OtName`, `OtIndex`；候选：`_OBJECT_HEADER.TypeIndex`, `HandleCount`, `PointerCount`, `NameInfoOffset`, `InfoMask` | handle value, object, type index/name, granted access, attributes, decode status | 进程句柄列表、对象名、类型名、权限、decode 错误 | 与 `NtQuerySystemInformation(SystemExtendedHandleInformation)` 交叉验证 |
| P1 | object type / object header | 已有 type name/index 级能力；通用 object header 未完整 PDB 化 | 候选：`_OBJECT_HEADER.TypeIndex`, `PointerCount`, `HandleCount`, `InfoMask`, `NameInfoOffset`, `_OBJECT_HEADER_NAME_INFO.Name`, `_OBJECT_TYPE.TypeList`, `Name`, `Index`, `CallbackList` | object header counters、type pointer/index、name info、per-type list source | 对象地址、类型、引用计数、名字、header 异常 | 对 Process/Thread/File/Driver/Device 对象做 header 解码，与 Ob API 查询结果比较 |
| P1 | MmUnloadedDrivers | 目前 driver integrity 仅报告全局地址可用性 | 已有：`MmUnloadedDrivers` global；候选：`MmLastUnloadedDriver`, `_UNLOADED_DRIVERS.Name`, `StartAddress`, `EndAddress`, `CurrentTime` | unloaded driver rows: name/base/end/time/index/source | 卸载驱动时间线、与当前模块冲突、可疑清理 | 加载卸载测试驱动后确认 entry 出现；清空痕迹样本确认 unavailable/zeroed 风险 |
| P1 | PiDDBCacheTable | 目前 driver integrity 仅报告全局地址可用性 | 已有：`PiDDBCacheTable` global；候选：`_RTL_AVL_TABLE`/`_RTL_BALANCED_LINKS` 适配、PiDDB entry key/name/time/status | cache rows: name/timestamp/load status/node address/source | PiDDB 条目、与 Services/文件/模块对照、缺失风险 | 测试驱动加载后条目可枚举；手动清理后与 Service/文件残留交叉提示 |
| P1 | SSDT / ShadowSSDT / Inline / IAT / EAT | 已有 SSDT/ShadowSSDT、inline scan/patch、IAT/EAT scan 协议与实现 | 候选：`KeServiceDescriptorTable`, shadow table 符号、win32k/ntos 导出基线、module section header PDB/PE baseline | service index/name/current target/owner module、inline bytes、IAT/EAT current/expected | Hook 类型、目标模块、当前/期望地址、字节 diff、置信度 | 干净系统基线无高危；测试跳板/导入改写能标记 owner mismatch |
| P2 | DriverObject 强化审计 | 基础 driver integrity 已有；评分规则需 UI/协议固化 | 候选：`_DRIVER_EXTENSION.ServiceKeyName`, `_DRIVER_OBJECT.Flags`, `DeviceObject`, `_FAST_IO_DISPATCH` 全成员偏移 | normalized service metadata、FastIo operation index、dispatch target class | 风险分、证据列表、服务项对照 | 同一驱动正常/IRP hook/FastIo hook/设备链异常四组样本评分单调上升 |
| P2 | module / driver cross-view 聚合 | SystemModule、AuxKlib、PsLoadedModuleList、DriverObject、Service 各有部分证据 | 候选：Service name linkage、`DriverSection`->KLDR reverse index、unloaded/PiDDB entry schema | module identity row, source mask, owner mismatch, stale/unloaded flags | 模块总表、隐藏驱动标签、服务状态、历史痕迹 | 从 KLDR 摘链、删服务、清 PiDDB、残留 DriverObject 各场景出独立标签 |

## Cross-View 检测规则

| 对象 | 规则 | 判定 | 风险 |
|---|---|---|---|
| 隐藏进程 | `PspCidTable` 或 `PsLookupProcessByProcessId` 可见，但 `PsGetNextProcess` / `ActiveProcessLinks` 不可见 | `CID_ONLY` + `MISSING_FROM_ACTIVE_LIST` | P0 高 |
| 隐藏进程 | `ActiveProcessLinks` 可见，但 CID lookup 失败或 `PspCidTable` 缺失 | `ACTIVE_ONLY` + `MISSING_FROM_CID_TABLE` | P0 高；可能是 DKOM/退出态，需 termination 字段降噪 |
| 隐藏进程 | EPROCESS 的 `UniqueProcessId` 与 CID key / public PID 不一致 | PID field mismatch | P0 高；典型 fake PID 隐藏 |
| 隐藏线程 | ETHREAD 在 `PspCidTable` 可见，但不在 owner `EPROCESS.ThreadListHead` | `CID_ONLY` + `THREAD_NOT_IN_PROCESS_LIST` | P0 高 |
| 孤儿线程 | ETHREAD 的 `KTHREAD.Process` 指向进程不在 process cross-view 任一来源 | `THREAD_ORPHAN` | P0 高 |
| 可疑线程入口 | `EtStartAddress` / `EtWin32StartAddress` 不落入任何已加载模块 | `START_ADDRESS_OUTSIDE_MODULE` | P1 中高；JIT/特殊系统线程需白名单 |
| 隐藏驱动 | SystemModule/AuxKlib 可见，但 `PsLoadedModuleList` 缺失 | module list unlink | P0 高 |
| 隐藏驱动 | DriverObject 可引用且 `DriverStart` 在内核地址范围，但无 SystemModule/KLDR 所属模块 | orphan DriverObject | P0 高 |
| 隐藏驱动 | `PsLoadedModuleList` 有 KLDR entry，但 Service missing、DriverObject missing、文件缺失 | stale loader/service mismatch | P1 中 |
| 卸载痕迹 | 当前无模块，但 `MmUnloadedDrivers` / PiDDB 有近期记录，且文件/服务残留异常 | suspicious unload trace | P1 中 |

## DriverObject / IRP Hook 风险评分

| 分数 | 触发条件 | 说明 |
|---|---|---|
| +40 | `MajorFunction[i]` 指针不在 `DriverStart..DriverStart+DriverSize` 且 owner module 非目标驱动 | 典型 IRP dispatch hook |
| +35 | `FastIoDispatch` 表地址或任一 FastIo handler 指向目标驱动外部模块 | FastIo hook 或转发 |
| +30 | dispatch/FastIo 指向未知内核地址，无法归属任何已加载模块 | shellcode/已卸载模块残留风险 |
| +25 | `DriverSection` 与 `PsLoadedModuleList` KLDR entry 不一致 | DriverObject 与 loader 脱节 |
| +25 | `DriverStart` / `DriverSize` 与 SystemModule/AuxKlib/KLDR 范围不一致 | 模块身份异常 |
| +20 | DeviceObject 链存在 loop、跨 DriverObject attached chain、无效 `DriverObject` 回指 | 设备栈 DKOM 或对象损坏 |
| +15 | `DriverUnload` 缺失、指向外部模块或未知地址 | 不单独判恶，但提高卸载/清理风险 |
| +15 | Service registry 缺失或 image path 与模块路径冲突 | 隐藏/残留驱动信号 |
| +10 | `MmUnloadedDrivers` / PiDDB 与当前模块状态冲突 | 辅助痕迹 |

| 总分 | 等级 | UI 建议 |
|---|---|---|
| 0-19 | Low | 展示为正常或信息项 |
| 20-49 | Medium | 黄色风险，要求展开证据 |
| 50-79 | High | 红色风险，默认置顶 |
| >=80 | Critical | 多源一致异常，进入隐藏/Hook 专项视图 |

## MVP 优先级

| 优先级 | MVP 项 | 范围 | 依赖 | 验收 |
|---|---|---|---|---|
| P0 | process cross-view | public walk + ActiveProcessLinks + PspCidTable，输出 anomaly/confidence/detail | 已有 process cross-view 协议和 PDB fields | 只摘链进程可标记 `MISSING_FROM_ACTIVE_LIST` |
| P0 | thread cross-view | public walk + ThreadListHead + PspCidTable + start address module check | 已有 thread cross-view 协议和 PDB fields | 摘线程链/孤儿线程可区分 |
| P0 | module / driver cross-view | SystemModule + AuxKlib + PsLoadedModuleList + DriverObject + Service | 已有 driver integrity、KLDR、DriverObject 字段 | KLDR 摘链或 orphan DriverObject 出高风险 |
| P0 | callback cross-view | process/thread/image notify + registry + Ob callback，按 module 归属 | 已有 callback enum 协议和 PDB callback fields | 第三方回调能展示地址、模块、trust flags |
| P1 | handle/object header | 进程 handle table decode + object header/type/name 增强 | 已有 handle decode，需补 `_OBJECT_HEADER` fields | 与系统 handle info 对齐，header 计数可展示 |
| P1 | unloaded/PiDDB entries | 从仅地址可用升级到 entry 枚举 | 需补 entry schema | 加载卸载测试驱动后可展示历史条目 |

## 不改代码声明

本文件是准备文档。当前任务不修改主项目源码、不修改 `tools`、不修改协议头、不新增 PDB 字段 ID；后续实现应另起任务，按 P0/P1/P2 分批评审、补协议、补 generator、补 R0 只读 collector、补 UI。
