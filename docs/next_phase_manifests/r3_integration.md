# R3 Integration Manifest

## 范围

本轮只做下一阶段统一集成：中央 IOCTL 注册、R0/R3 项目文件清单、ArkDriverClient 统一入口和 Dock UI 聚合页。未构建、未运行真实驱动测试、未写 Release。

已检查并基于以下 manifest 集成：

- `dyndata_v3.md`
- `memory_evidence.md`
- `process_thread_crossview.md`
- `driver_kernel_integrity.md`
- `mutation_transaction.md`

## IOCTL registry

`KswordARKDriver/src/dispatch/ioctl_registry.c` 已接入：

- `IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE`
- `IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW`
- `IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW`
- `IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY`
- `IOCTL_KSWORD_ARK_MUTATION_PREPARE`
- `IOCTL_KSWORD_ARK_MUTATION_COMMIT`
- `IOCTL_KSWORD_ARK_MUTATION_ROLLBACK`
- `IOCTL_KSWORD_ARK_MUTATION_QUERY_AUDIT`

所有业务 handler 仍只通过 `ioctl_registry.c` 注册，未改 `ioctl_dispatch.c`。

## Project files

R0 项目文件已加入本阶段明确存在的 memory evidence、process/thread cross-view、driver/kernel integrity、mutation transaction 源文件和头文件。

R3 项目文件已加入：

- `ArkDriverClient/ArkDriverMutation.cpp`
- `MemoryDock/MemoryDock.KernelMemoryEvidence.cpp`
- `ProcessDock/ProcessDock.CrossView.cpp`
- `DriverDock/DriverDock.Integrity.cpp`
- `MonitorDock/MonitorDock.ArkRiskCenter.cpp`

`MiscDock/ContextMenuCleaner` 属于其他施工范围，本文件不把它计入本轮集成结果。

## ArkDriverClient

新增查询入口统一走 `ArkDriverClient`，Dock 不直接调用 KswordARK `DeviceIoControl`：

- `queryKernelMemoryEvidence`
- `queryProcessCrossView`
- `queryThreadCrossView`
- `queryDriverIntegrity`
- `queryKernelCpuIntegrity`
- `prepareMutation`
- `commitMutation`
- `rollbackMutation`
- `queryMutationAudit`

旧驱动、未注册 IOCTL 或能力缺失通过 `unsupported`/`IoResult` 传回 UI，由 UI 显示 graceful message。

## UI pages

已放置到对应 Dock：

- `MemoryDock`：内核内存证据页。
- `ProcessDock`：Cross-View 页。
- `DriverDock`：驱动完整性页。
- `MonitorDock`：ARK 风险中心。

未新增 `KernelDock` 新功能页面。

ARK 风险中心只读聚合 Memory / Process / Thread / Driver / CPU / Hook / Callback / Mutation Audit 结果，按 `riskScore` 排序，支持过滤、高风险开关、JSON/CSV 导出。Mutation 在 UI 中只展示 dry-run/audit/rollback 状态，不提供任意写或危险提交按钮。

## Protocol notes

`IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE` 与 `IOCTL_KSWORD_ARK_MUTATION_PREPARE` 当前共享 function id `0x832`，但 Access 位不同，完整 CTL_CODE 不同；registry 按完整 IOCTL code 精确匹配，因此不会在当前分发表中互相覆盖。后续若要降低维护歧义，建议在协议层分配唯一 function id。

## Blockers / deferred

- 未构建、未运行 MSBuild/devenv/WDK/Qt build。
- 未运行真实驱动测试。
- 未写 Release。
- Memory text diff 仍按 manifest 保持 R0 只返回内存 hash/sample，磁盘比对应由 R3 后续完善。
- Driver integrity 对 `MmUnloadedDrivers` / `PiDDBCacheTable` 的 entry-level 枚举仍等待稳定 DynData schema。
- Mutation 危险写入口未暴露到 UI；未来修复动作必须继续走 transaction + safety policy。
