# PDB/R0 审计能力接入与验收矩阵

## 1. 文档定位

本文记录本轮 PDB/R0 审计能力的接入面和最终总验收矩阵。范围只覆盖文档归纳，不代表本次修改源码、驱动、shared 协议或项目文件。

本矩阵的证据来源为当前仓库静态检查：

- 总控与专题设计：`docs/pdb_r0_audit_prep/01..10_*.md`。
- 阶段 manifest：`docs/next_phase_manifests/*.md`。
- shared 协议头：`shared/driver/*Ioctl.h`。
- R0 注册表：`KswordARKDriver/src/dispatch/ioctl_registry.c`。
- R3 统一入口：`Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h`。
- 主 GUI 与 ARKLight 页面落点：`Ksword5.1/Ksword5.1/*Dock/`、`KswordARKLight/Features/`。
- CLI 分发：`KswordCLI/KswordCLI.cpp`。

约束沿用根目录 `AGENTS.md`：R0/R3 协议只在 `shared/driver/`，R0 IOCTL 只经 `ioctl_registry.c` 注册，Dock/UI 不直接 `DeviceIoControl`，用户态统一走 `ArkDriverClient`。

## 2. 状态图例

| 状态 | 含义 |
| --- | --- |
| 已注册 | shared IOCTL、R0 handler 和 `ioctl_registry.c` 注册项均在当前静态树中可见。 |
| 已有 wrapper | `ArkDriverClient` 暴露 typed wrapper，UI 可通过 wrapper 调用。 |
| 已有 GUI | 主 Qt GUI 已有明确 Dock/Page 落点。 |
| 已有 Light | `KswordARKLight/Features` 已有明确落点。 |
| 已有 CLI | `KswordCLI` 已有对应 family/subcommand。 |
| 部分接入 | 协议/后端/UI 中有缺口，或只覆盖 P0 子集。 |
| 设计/待接入 | 目前主要是文档设计或 manifest，最终验收前仍需实现/集成。 |
| 不适用 | 该功能区不需要对应落点，或当前轮明确不做。 |

## 3. 总览矩阵

| 功能区 | shared IOCTL | ArkDriverClient wrapper | 主 GUI 落点 | KswordARKLight 落点 | KswordCLI 命令 | 当前验收状态 |
| --- | --- | --- | --- | --- | --- | --- |
| PDB Profile / DynData V4 | `IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_V4`; `IOCTL_KSWORD_ARK_QUERY_DYN_V4_MODULES`; `IOCTL_KSWORD_ARK_QUERY_DYN_V4_CAPABILITY_GROUPS`; `IOCTL_KSWORD_ARK_QUERY_DYN_V4_MISSING_ITEMS`; 兼容 v1/v2/v3 status/fields/capabilities/apply/ex | `applyDynDataProfileV4`; `queryDynDataV4Modules`; `queryDynDataV4CapabilityGroups`; `queryDynDataV4MissingItems`; v1/v2/v3 wrappers | `KernelDock` DynData / DriverStatus；R3 loader 仍负责 profile pack 解析 | `DriverFeature` 的“DynData能力”“动态偏移 / DynData”；`KernelPage` DynData/DynDataCapabilities | `dyn status`; `dyn fields`; `dyn capabilities`; `dyn profile`/`v4-modules`; `dyn v4-capabilities`; `dyn v4-missing`; `dyn apply-profile-v4`; 兼容旧 `dyn apply-profile`, `dyn apply-profile-ex` | V4 查询面与 CLI blob 下发入口已接入；文档仍要求最终验收 profile identity、per-module capability、缺字段降级与运行态响应。 |
| 进程/线程/CID | `IOCTL_KSWORD_ARK_ENUM_PROCESS`; `IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW`; `IOCTL_KSWORD_ARK_ENUM_THREAD`; `IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW`; `IOCTL_KSWORD_ARK_ENUM_CID_TABLE`; `IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY` | `enumerateProcesses`; `queryProcessCrossView`; `enumerateThreads`; `queryThreadCrossView`; `enumCidTable`; `queryKernelObjectSummary` | `ProcessDock` Cross-View；`KernelDock` CID/Object summary | `KernelPage` ProcessCrossView / ThreadCrossView / CID；ProcessDetail 仍为 R3/R0 混合详情 | `process enum`; `process crossview`; `thread enum`; `thread crossview`; `kernel cid` | 已注册、已有 wrapper、已有 GUI/Light/CLI。验收重点是 DynData 缺失降级、budget/partial、CID-only/active-only/thread orphan 分类。 |
| 句柄/对象 | `IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES`; `IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT`; `IOCTL_KSWORD_ARK_QUERY_ALPC_PORT`; `IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY`; `IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY` | `enumerateProcessHandles`; `queryHandleObject`; `queryAlpcPort`; `queryKernelObjectSummary`; `queryIpcSummary` | 主 GUI `HandleDock`；`KernelDock` Object Namespace / Object Type / ALPC/IP 桥接页 | `Features/Handle`; `KernelPage` ObjectTypeMatrix / ObjectNamespace / CommunicationEndpoint / NamedPipe | `handle enum`; `handle query-object`; `handle object-header`; `handle type-matrix`; `alpc ...`; `kernel ipc` | P0 句柄枚举和对象摘要已接入；通用 `_OBJECT_HEADER` 深字段仍按 `07_ntos_core_ark_audit.md` 标为 P1 增强验收。 |
| 驱动与模块完整性 | `IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT`; `IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY`; `IOCTL_KSWORD_ARK_QUERY_CPU_HARDWARE`; `IOCTL_KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT`; `IOCTL_KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW` | `queryDriverObject`; `queryDriverIntegrity`; `queryKernelCpuIntegrity`; `queryCpuHardwareSnapshot`; `queryPhysicalMemoryLayout`; `queryDriverTrustView` | `DriverDock` Integrity/Evidence；`KernelDock` DriverObject、CPU integrity、module/driver evidence | `Features/Driver`; `KernelPage` DriverIntegrity / KernelCpuIntegrity / DeviceDriverObjects | `driver integrity`; `driver detail`; `driver unloaded`; `driver piddb`; `kernel query-driver-integrity`; `kernel query-driver-object`; `misc driver-trust` | 已注册且已多端接入。`driver unloaded`/`driver piddb` 已复用 Driver Integrity optional-globals 投影 `MmUnloadedDrivers`/`PiDDBCacheTable` 地址证据；entry-level 枚举仍是 P1/P2 待 schema。 |
| 回调与 Hook | `IOCTL_KSWORD_ARK_ENUM_CALLBACKS`; `IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE`; `IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT`; `IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS`; `IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS`; `IOCTL_KSWORD_ARK_ENUM_SSDT`; `IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT`; win32k hotkey/hook 见 GUI 行 | `enumerateCallbacks`; `queryCallbackRuntimeState`; `waitCallbackEventAsync`; `scanInlineHooks`; `enumerateIatEatHooks`; `enumerateSsdt`; `enumerateShadowSsdt`; keyboard/win32k wrappers | `KernelDock` Callback/SSDT/ShadowSSDT/Inline/IAT/EAT；Risk Center 聚合 | `KernelPage` CallbackEnumeration、KeyboardHotkeys、KeyboardHooks、Ssdt/ShadowSsdt/InlineHook/IatEatHook | `callback enum`; `callback runtime-state`; `kernel callbacks`; `kernel hooks`; `kernel ssdt`; `kernel shadow-ssdt`; `kernel scan-inline-hooks`; `kernel enum-iat-eat-hooks`; `keyboard enum-hotkeys`; `keyboard enum-hooks` | 只读枚举面已接入；callback 修改、remove、bypass、patch/unload 类命令存在于仓库但不属于本轮只读审计验收，应按 mutation/safety 单独门禁。 |
| Win32K GUI | `IOCTL_KSWORD_ARK_QUERY_WIN32K_PROFILE_STATUS`; `IOCTL_KSWORD_ARK_QUERY_WIN32K_WINDOWS`; `IOCTL_KSWORD_ARK_QUERY_WIN32K_GUI_THREADS`; `IOCTL_KSWORD_ARK_QUERY_WIN32K_HOTKEYS_PDB`; `IOCTL_KSWORD_ARK_QUERY_WIN32K_HOOKS_PDB` | `queryWin32kProfileStatus`; `queryWin32kWindows`; `queryWin32kGuiThreads`; `queryWin32kHotkeysPdb`; `queryWin32kHooksPdb` | 主 GUI 现有 `WindowDock`/`OtherDock` 窗口管理；Win32k R0 证据可由 Window/Kernel 方向承接 | `Features/Window` 中 Win32kGuiAudit 模式调用 ArkDriverClient；R3 `EnumWindows` baseline 保留 | `window win32k`; `window gui`; `window gui-threads` | 已注册、Light 已接入 GUI 审计入口。最终验收要覆盖 session/desktop 上下文、R3 `EnumWindows` cross-view、profile 缺失 unsupported。 |
| Network Stack | `IOCTL_KSWORD_ARK_NETWORK_QUERY_TCP_ENDPOINTS`; `IOCTL_KSWORD_ARK_NETWORK_QUERY_UDP_ENDPOINTS`; `IOCTL_KSWORD_ARK_NETWORK_QUERY_WFP_INVENTORY`; `IOCTL_KSWORD_ARK_NETWORK_QUERY_NDIS_CHAIN`; 旧策略 `NETWORK_SET_RULES/QUERY_STATUS` 非本轮只读验收重点 | `queryNetworkTcpEndpoints`; `queryNetworkUdpEndpoints`; `queryNetworkWfpInventory`; `queryNetworkNdisChain` | `NetworkDock` TCP/UDP 与 `NetworkAuditPage`；Firewall/WFP R3 页面可作 cross-view | `Features/Network` R0 TCP/UDP/WFP/NDIS 只读页 | `network tcp`; `network udp`; `network audit`; `network wfp`; `network ndis`; `network afd`; `network nsi` | TCP/UDP/WFP/NDIS 已接入；`network afd`/`network nsi` 已有明确 degraded R3 fallback 输出，不再是硬 unsupported；AFD/NSI/HTTP 的独立 shared/R0 IOCTL 仍按 `04_network_stack_audit.md` 为 P1/P2 待接入。 |
| Minifilter/FileObject/Section/Storage/BitLocker | `IOCTL_KSWORD_ARK_QUERY_MINIFILTER_INVENTORY`; `IOCTL_KSWORD_ARK_QUERY_FILE_INFO`; `IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION`; `IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS`; `IOCTL_KSWORD_ARK_QUERY_VOLUME_STACK_AUDIT`; `IOCTL_KSWORD_ARK_QUERY_BITLOCKER_FVE_AUDIT`; `IOCTL_KSWORD_ARK_QUERY_MOUNTMGR_MAPPING_AUDIT`; `IOCTL_KSWORD_ARK_QUERY_FILESYSTEM_INTEGRITY_AUDIT` | `queryMinifilterInventory`; `queryFileInfo`; `queryProcessSection`; `queryFileSectionMappings`; `queryVolumeStackAudit`; `queryBitlockerFveAudit`; `queryMountMgrMappingAudit`; `queryFilesystemIntegrityAudit` | `FileDock` file info/section/minifilter/storage audit；`HardwareDock` 可 cross-link storage device | `Features/File` Minifilter / Section / Storage / BitLocker / FS Integrity tabs | `file minifilter`; `file query-info`; `file fileobject`; `file section`; `section query-process`; `section query-file-mappings`; `file bitlocker`; `file storage`; `file mountmgr`; `file filesystem` | P0 多数已接入。BitLocker 验收必须确认不返回 key material；ControlArea/deleted-file 深证据仍是 P1/P2。 |
| Security/CI/VBS/Hyper-V/AppControl | `IOCTL_KSWORD_ARK_QUERY_SECURITY_STATUS`; `IOCTL_KSWORD_ARK_QUERY_DRIVER_TRUST_VIEW`; `IOCTL_KSWORD_ARK_QUERY_HYPERV_SUMMARY`; `IOCTL_KSWORD_ARK_QUERY_APP_CONTROL_STATUS`; 另有 `IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST`、`IOCTL_KSWORD_ARK_QUERY_PREFLIGHT` | `querySecurityStatus`; `queryDriverTrustView`; `queryHyperVSummary`; `queryAppControlStatus`; `queryImageTrust`; `queryPreflight` | `MiscDock/ApplicationControlPage`; `DriverDock` trust cross-view；Preflight/Trust 相关页面 | `Features/Misc` Security / CI / VBS / Hyper-V / AppControl read-only facade | `misc security`; `misc ci`; `misc vbs`; `misc hyperv`; `misc applocker`; `misc bam`; `misc driver-trust`; `trust ...`; `preflight ...` | P0 posture 已接入；AppLocker policy details、mssecflt 深实例、VMBus/vSwitch/vPCI/HvSocket、ahcache/BAM 明细仍按设计延后，默认 summary/privacy。 |
| 设备与输入链 | `IOCTL_KSWORD_ARK_QUERY_DEVICE_STACK_AUDIT`; `IOCTL_KSWORD_ARK_QUERY_INPUT_STACK_AUDIT`; `IOCTL_KSWORD_ARK_QUERY_USB_TOPOLOGY_AUDIT`; `IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS`; `IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS` | `queryDeviceStackAudit`; `queryInputStackAudit`; `queryUsbTopologyAudit`; `enumerateKeyboardHotkeys`; `enumerateKeyboardHooks` | `HardwareDock` DeviceManager/R0Evidence；`KernelDock` keyboard hotkey/hook | `Features/Hardware` SetupAPI/CM tree with R0 protocol summaries；`KernelPage` KeyboardHotkeys/KeyboardHooks | `hardware audit`; `hardware pnp`; `hardware input`; `hardware usb`; `keyboard enum-hotkeys`; `keyboard enum-hooks` | P0 device/input/USB stack audit已接入。验收必须确认无键盘/鼠标内容采集，只有设备栈、filter、hook/hotkey 元数据。 |
| GPU/Display/Watchdog | `IOCTL_KSWORD_ARK_QUERY_GPU_DISPLAY_WATCHDOG_AUDIT` | `queryGpuDisplayWatchdogAudit` | 主 GUI 可落 `WindowDock`/`HardwareDock`/Driver integrity；当前 R0 evidence 也可聚合到 Risk Center | `Features/Window` GpuDisplayAudit；`Features/Hardware` 可显示 R0 device stack protocol | `window gpu`; `window display`; `window watchdog` | P1/P2 方向已有协议和 Light 入口；深度 dxgkrnl/dxgmms2 进程 GPU 对象仍为 P2 research，TDR/watchdog 私有状态只做有限只读。 |
| IPC/SMB/Pipe | `IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY`; `IOCTL_KSWORD_ARK_QUERY_ALPC_PORT`; `IOCTL_KSWORD_ARK_QUERY_KERNEL_OBJECT_SUMMARY`; named pipe 目前主要走对象命名空间/句柄/KernelDock worker | `queryIpcSummary`; `queryAlpcPort`; `queryKernelObjectSummary` | `KernelDock` NamedPipe / CommunicationEndpoint / ALPC / Object namespace | `KernelPage` NamedPipe / CommunicationEndpoint / ObjectNamespace；`Features/Handle` 可查句柄对象 | `kernel ipc`; `alpc ...`; `handle query-object` | IPC summary/ALPC/Pipe 对象侧已接入。SMB 栈专用 R0/PDB IOCTL 未见独立协议，当前应标为通过文件/网络/IPC 交叉证据覆盖，SMB 专项待设计。 |
| Memory Evidence / Kernel Memory | `IOCTL_KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY`; `IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE`; `IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS`; `IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY`; 另有 VA/physical read/write 但本轮验收只看只读证据 | `scanKernelExecutableMemory`; `queryKernelMemoryEvidence`; PTE/translate 通过 memory wrappers/types；主 GUI 内存页调用 ArkDriverClient | `MemoryDock` Kernel Executable Memory / Kernel Memory Evidence / Process PTE Translate；Risk Center 聚合 | `Features/Memory` 嵌入 `KernelPage` KernelExecutableMemory / KernelMemoryEvidence，另有 R3 Process VA Evidence fallback | `memory scan-kexec`; `memory scan-evidence`; `memory translate-va`; `memory query-pte` | 本轮重点已接入：kernel executable scan v2 有 address range、protection、module/section owner、unknown executable、first bytes hash/status、budget/partial；memory evidence 有 RWX/RX private、non-module executable、image-like、Section/ControlArea/deleted-file hint；PTE/VA 输出 flags、large/NX/write/user/global、confidence。未编译、未运行。 |

## 4. 分功能验收要点

### 4.1 PDB Profile / DynData V4

- shared IOCTL：`KswordArkDynDataIoctl.h` 中 v1/v2/v3 状态、字段、capability、apply 与 v4 apply/query 系列。
- ArkDriverClient wrapper：`applyDynDataProfileV4`、`queryDynDataV4Modules`、`queryDynDataV4CapabilityGroups`、`queryDynDataV4MissingItems`。
- 主 GUI 落点：`KernelDock` DynData 状态、profile 下发、driver capability 诊断。
- KswordARKLight 落点：`Features/Driver` 的 DynData tab 与 `KernelPage` DynData/DynDataCapabilities。
- KswordCLI 命令：`dyn status`、`dyn fields`、`dyn capabilities`、`dyn profile`、`dyn v4-capabilities`、`dyn v4-missing`、`dyn apply-profile-v4`；旧格式 apply 为 `dyn apply-profile`/`apply-profile-ex`。
- 验收状态：v4 查询面与 CLI blob 下发入口已存在；最终验收必须证明模块 identity 精确匹配、required/optional item 降级、single-module failure 不影响其它模块，并在真实驱动环境中验证 `apply-profile-v4` 的 unsupported/ok 输出。

### 4.2 进程/线程/CID

- shared IOCTL：process/thread cross-view、thread enum、CID table、kernel object summary。
- ArkDriverClient wrapper：`queryProcessCrossView`、`queryThreadCrossView`、`enumCidTable`、`queryKernelObjectSummary`。
- 主 GUI 落点：`ProcessDock.CrossView`、`KernelDock` CID/Object。
- KswordARKLight 落点：`KernelPage` ProcessCrossView、ThreadCrossView、CID/Object pages。
- KswordCLI 命令：`process crossview`、`thread crossview`、`kernel cid`。
- 验收状态：已接入。需要用摘链/CID 缺失/退出态/孤儿线程样本验证 anomaly bits、confidence、partial/budget。

### 4.3 句柄/对象

- shared IOCTL：handle enum、handle object、ALPC port、object summary、IPC summary。
- ArkDriverClient wrapper：`enumerateProcessHandles`、`queryHandleObject`、`queryAlpcPort`、`queryKernelObjectSummary`、`queryIpcSummary`。
- 主 GUI 落点：`HandleDock`、`KernelDock` object namespace/type/communication pages。
- KswordARKLight 落点：`Features/Handle` 和 `KernelPage` object pages。
- KswordCLI 命令：`handle enum`、`handle query-object`、`handle object-header`、`handle type-matrix`、`alpc`、`kernel ipc`。
- 验收状态：基础已接入；`_OBJECT_HEADER` 通用深解析、per-type list、NameInfo 等仍是 P1。

### 4.4 驱动与模块完整性

- shared IOCTL：driver object、driver integrity、CPU hardware、physical layout、driver trust view。
- ArkDriverClient wrapper：`queryDriverObject`、`queryDriverIntegrity`、`queryKernelCpuIntegrity`、`queryCpuHardwareSnapshot`、`queryPhysicalMemoryLayout`、`queryDriverTrustView`。
- 主 GUI 落点：`DriverDock.Integrity` / `DriverDock.Evidence`、`KernelDock` driver/cpu evidence。
- KswordARKLight 落点：`Features/Driver` 与 `KernelPage` DriverIntegrity/KernelCpuIntegrity。
- KswordCLI 命令：`driver integrity`、`driver detail`、`driver unloaded`、`driver piddb`、`kernel query-driver-integrity`、`misc driver-trust`。
- 验收状态：P0 已接入；`driver unloaded`/`driver piddb` 当前返回 Driver Integrity optional-global 地址证据并把地址存在但 entry schema 未走通标为 partial/degraded，完整 entry 枚举仍需后续 PDB schema。

### 4.5 回调与 Hook

- shared IOCTL：callback enum/runtime/wait/answer/cancel/remove、SSDT/Shadow SSDT、inline scan、IAT/EAT、keyboard hotkey/hook、win32k PDB hotkey/hook。
- ArkDriverClient wrapper：callback、kernel hook、keyboard、win32k wrappers。
- 主 GUI 落点：`KernelDock` callback/hook 系列。
- KswordARKLight 落点：`KernelPage` CallbackEnumeration、KeyboardHotkeys、KeyboardHooks、SSDT/Hook pages。
- KswordCLI 命令：`callback enum`、`kernel callbacks`、`kernel hooks`、`keyboard enum-hotkeys`、`keyboard enum-hooks`。
- 验收状态：只读枚举面已接入。所有 remove/patch/bypass 类能力不纳入默认只读审计验收，必须由 mutation/safety gate 单独验收。

### 4.6 Win32K GUI

- shared IOCTL：profile status、windows、GUI threads、hotkeys PDB、hooks PDB。
- ArkDriverClient wrapper：对应 `queryWin32k*` 五个 wrapper。
- 主 GUI 落点：Window/Kernel GUI 审计落点；R3 `WindowDock`/旧 `OtherDock` 仍提供公开 API baseline。
- KswordARKLight 落点：`Features/Window` 的 Win32kGuiAudit 与 R3 `EnumWindows` baseline。
- KswordCLI 命令：`window win32k`、`window gui`、`window gui-threads`。
- 验收状态：P0 接入。需要验证 session/desktop 绑定、profile 缺失 unsupported、R3/R0 差异分类。

### 4.7 Network Stack

- shared IOCTL：TCP/UDP endpoint audit、WFP inventory、NDIS chain；现有 network rules/status 属于策略/运行态，不是本轮只读重点。
- ArkDriverClient wrapper：`queryNetworkTcpEndpoints`、`queryNetworkUdpEndpoints`、`queryNetworkWfpInventory`、`queryNetworkNdisChain`。
- 主 GUI 落点：`NetworkDock` 与 `NetworkAuditPage`。
- KswordARKLight 落点：`Features/Network`。
- KswordCLI 命令：`network audit`、`network tcp`、`network udp`、`network wfp`、`network ndis`、`network afd`、`network nsi`。
- 验收状态：P0/P1 部分已接入。`network afd` 当前打印 degraded R3 IP Helper owner PID 表，`network nsi` 当前打印 degraded `GetAdaptersAddresses` 接口/地址投影；两者明确声明缺少专用 R0 IOCTL，不能误标为 AFD/NSI R0/PDB 深审计完成。HTTP 仍未有 shared IOCTL。

### 4.8 Minifilter/FileObject/Section/Storage/BitLocker

- shared IOCTL：minifilter inventory、file info、process/file section mappings、volume stack、BitLocker FVE、MountMgr、filesystem integrity。
- ArkDriverClient wrapper：对应 `queryMinifilterInventory`、`queryFileInfo`、`queryProcessSection`、`queryFileSectionMappings`、`queryVolumeStackAudit`、`queryBitlockerFveAudit`、`queryMountMgrMappingAudit`、`queryFilesystemIntegrityAudit`。
- 主 GUI 落点：`FileDock` 与 storage/section/minifilter audit pages。
- KswordARKLight 落点：`Features/File`。
- KswordCLI 命令：`file minifilter`、`file fileobject`、`file section`、`file bitlocker`、`file storage`、`file mountmgr`、`file filesystem`、`section query-*`。
- 验收状态：P0 已接入；BitLocker 必须验证敏感材料永不序列化；NTFS deleted-entry、ControlArea 深状态仍是 P2。

### 4.9 Security/CI/VBS/Hyper-V/AppControl

- shared IOCTL：security status、driver trust view、Hyper-V summary、AppControl status、image trust、preflight。
- ArkDriverClient wrapper：`querySecurityStatus`、`queryDriverTrustView`、`queryHyperVSummary`、`queryAppControlStatus`、trust/preflight wrappers。
- 主 GUI 落点：`MiscDock`、`ApplicationControlPage`、Driver trust evidence。
- KswordARKLight 落点：`Features/Misc`。
- KswordCLI 命令：`misc security`、`misc ci`、`misc vbs`、`misc hyperv`、`misc applocker`、`misc bam`、`misc driver-trust`。
- 验收状态：P0 posture 已接入；VMBus/vSwitch/vPCI/HvSocket、ahcache/BAM 明细必须保持 summary/privacy 默认。

### 4.10 设备与输入链

- shared IOCTL：device stack、input stack、USB topology、keyboard hotkeys/hooks。
- ArkDriverClient wrapper：`queryDeviceStackAudit`、`queryInputStackAudit`、`queryUsbTopologyAudit`、`enumerateKeyboardHotkeys`、`enumerateKeyboardHooks`。
- 主 GUI 落点：`HardwareDock` R0 Evidence 与 Kernel keyboard pages。
- KswordARKLight 落点：`Features/Hardware` SetupAPI/CM tree 加 R0 protocol summary；`KernelPage` keyboard pages。
- KswordCLI 命令：`hardware audit`、`hardware pnp`、`hardware input`、`hardware usb`、`keyboard enum-hotkeys`、`keyboard enum-hooks`。
- 验收状态：P0 接入。必须确认只采设备栈、filter 和 hook/hotkey 元数据，不采集键鼠输入内容。

### 4.11 GPU/Display/Watchdog

- shared IOCTL：GPU/display/watchdog audit。
- ArkDriverClient wrapper：`queryGpuDisplayWatchdogAudit`。
- 主 GUI 落点：Window/Hardware/Driver evidence 可聚合。
- KswordARKLight 落点：`Features/Window` GpuDisplayAudit。
- KswordCLI 命令：`window gpu`、`window display`、`window watchdog`。
- 验收状态：协议和 Light 入口已存在；私有 dxgkrnl/dxgmms2 process GPU object 是 P2，不应阻塞 P0/P1。

### 4.12 IPC/SMB/Pipe

- shared IOCTL：IPC summary、ALPC port、kernel object summary；Pipe 主要由对象命名空间和句柄视图覆盖。
- ArkDriverClient wrapper：`queryIpcSummary`、`queryAlpcPort`、`queryKernelObjectSummary`。
- 主 GUI 落点：`KernelDock` NamedPipe、CommunicationEndpoint、Object namespace；`HandleDock` 可反查对象。
- KswordARKLight 落点：`KernelPage` NamedPipe / CommunicationEndpoint / ObjectNamespace。
- KswordCLI 命令：`kernel ipc`、`alpc ...`、`handle query-object`。
- 验收状态：IPC/Pipe 对象侧已接入；SMB 专用栈未见独立协议，标为待设计/由 Network+File+IPC 交叉证据覆盖。

### 4.13 Memory Evidence / Kernel Memory

- shared IOCTL：kernel executable memory scan、kernel memory evidence scan、VA translate、PTE query。
- ArkDriverClient wrapper：`scanKernelExecutableMemory`、`queryKernelMemoryEvidence`；PTE/VA types 在 ArkDriverClient memory/kernel 模块中解析。
- 主 GUI 落点：`MemoryDock.KernelExecutableMemory`、`MemoryDock.KernelMemoryEvidence`、`MemoryDock.ProcessPteTranslate`。
- KswordARKLight 落点：`Features/Memory` 嵌入 `KernelPage` KernelExecutableMemory / KernelMemoryEvidence；R3 Process VA Evidence fallback。
- KswordCLI 命令：`memory scan-kexec`、`memory scan-evidence`、`memory translate-va`、`memory query-pte`。
- 验收状态：本轮新增证据字段已在协议中体现：
  - kernel executable scan：地址范围、保护位、模块归属、section owner、unknown executable、first bytes hash/status、`maxBytes`、`bytesScanned`、truncated/budget flags。
  - memory evidence：RWX/RX private、non-module executable、image-like memory、Section/ControlArea hint、deleted-file mapped hint、BigPool/MDL/SystemPTE owner hints、row confidence。
  - VA/PTE：PTE flags、large page、NX/write/user/global、effective protection、confidence。
  - 所有 scan 均应返回 budget、range limit、partial status；默认不 dump 敏感内容、不写内存、不 patch PTE。

## 5. 发布候选前统一验收清单

1. 静态协议一致性：shared IOCTL、function id、METHOD、access、registry row、handler 名称一致。
2. R3 访问路径：主 GUI 与 ARKLight 只通过 `ArkDriverClient` typed wrapper，不直接 `DeviceIoControl`。
3. Profile 降级：无 profile、错配 profile、capability missing 均返回 unsupported/partial/degraded，不猜偏移。
4. Budget/partial：所有扫描、链表、表、树、BigPool、CID、device stack、network stack 都有 maxRows/maxBytes/maxDepth 和 partial/budget exhausted 状态。
5. 只读边界：审计页不得默认提供 patch/delete/bypass/unlink/remove/disable；已有危险能力必须走独立 safety/mutation 门禁，不能混入本矩阵的“通过”。
6. 隐私/敏感内容：BitLocker key material、键鼠输入内容、网络 payload、HvSocket payload、ahcache/BAM 用户活动明细、内核原始内容 dump 均不作为默认输出。
7. Cross-view 解释：R0-only/R3-only/API-only/PDB-only/partial/unsupported 不渲染为 clean；单源差异不直接定性恶意。
8. CLI 验收：每个已标“已有 CLI”的命令至少可打印 unsupported/graceful 或正常响应；unsupported 子命令不得标为完成。

## 6. 当前待收尾/待确认项

- DynData v4 apply 的 CLI 入口已补为 `dyn apply-profile-v4 --blob ...`；仍需真实驱动环境验证 identity mismatch、required/optional 降级和 unsupported 输出。
- Network AFD/NSI、HTTP.sys、SMB 专项协议未见独立 shared IOCTL；CLI AFD/NSI 已提供 degraded R3 fallback，但 R0/PDB 深审计仍是后续项。
- `MmUnloadedDrivers` / `PiDDBCacheTable` entry-level 枚举仍依赖稳定 DynData schema。
- Win32k GUI 需用真实 session/desktop 测试验证 R3/R0 差异分类。
- Storage/BitLocker 验收需做敏感字段 grep 和运行样例确认无 key/password/recovery payload。
- Memory Evidence 本轮协议增强已记录；仍需后续实际驱动环境测试确认 partial/budget 与 hash/status 行为。

## 7. 线程13静态验收补记（2026-06-27）

本节记录线程13在当前 worktree 上做的集成验收与轻量修正。验收方式仅限静态检查；遵守“不编译、不运行主程序、不运行 MSBuild”的约束，因此运行态行为仍需后续验收会话在真实驱动环境中确认。

### 7.1 已核对通过的集成项

- 新增源码工程注册：
  - 主 GUI 新增/未跟踪源码均已出现在 `Ksword5.1/Ksword5.1/Ksword5.1.vcxproj` 与 `.vcxproj.filters`：
    - `ArkDriverClient/ArkDriverAudit.cpp`
    - `KernelDock/KernelDockCidTab.cpp/.h`
    - `KernelDock/KernelDockIpcTab.cpp/.h`
    - `MemoryDock/MemoryDock.ProcessMemoryEvidence.cpp`
    - `MemoryDock/MemoryDock.ProcessPteTranslate.cpp`
    - `NetworkDock/NetworkAuditPage.cpp/.h`
  - `KswordARKLight/Features/AuditCommon`、`Handle`、`Misc`、`Network` 下新增 `.cpp/.h` 均已出现在 `KswordARKLight/KswordARKLight.vcxproj` 与 `.filters`。
- `KswordARKLight/Features/FeatureRegistry.cpp` 已注册新增轻量模块：
  - `40011 网络` -> `Network::CreateNetworkFeaturePage`
  - `40012 句柄` -> `Handle::CreateHandleFeaturePage`
  - `40013 杂项安全` -> `Misc::CreateMiscFeaturePage`
- 主 GUI 网络审计页已接入 ArkDriverClient 网络 wrapper，不直接访问 KswordARK 设备：
  - `queryNetworkTcpEndpoints`
  - `queryNetworkUdpEndpoints`
  - `queryNetworkWfpInventory`
  - `queryNetworkNdisChain`
  - UI 摘要显示 `ok / unsupported / unavailable`、`returned / total / parsed`、`truncated`、`io.message`。
- Kernel 视觉标识资源路径已统一到 qrc 真实路径 `:/Image/kernel_badge.png`：
  - 已修正 `NetworkDock/NetworkAuditPage.cpp`
  - 已修正 `WindowDock/WindowDock.cpp`

## 8. 连续验收补记（2026-06-27）

本节记录后续验收会话在当前 worktree 上继续清理的真实占位与弱实现。这里的“完成”仅覆盖列出的具体项，不代表 PDB/R0 审计总目标已经全部结束。

### 8.1 CLI Network AFD/NSI 硬 unsupported 清理

- `KswordCLI network afd` 已从硬 `commandUnsupported` 改为 degraded R3 fallback：
  - 使用 `GetExtendedTcpTable` / `GetExtendedUdpTable` 输出 IPv4 TCP/UDP owner PID 投影。
  - 明确打印 `status=degraded unsupportedR0=1`，不伪装为 AFD R0/PDB 深审计。
  - IPv6 owner PID 在当前 SDK/CLI 编译上下文中未稳定暴露，输出中明确提示由 `network nsi` fallback 覆盖接口/地址侧证据。
- `KswordCLI network nsi` 已从硬 `commandUnsupported` 改为 degraded R3 fallback：
  - 使用 `GetAdaptersAddresses` 输出 adapter、ifIndex、IPv6 ifIndex、operStatus、ifType、MTU 与 unicast address 摘要。
  - `--limit` 同时限制 adapter 与 address 输出量，避免验收命令输出过长。
- `KswordCLI.vcxproj` 已显式链接 `Iphlpapi.lib` 和 `Ws2_32.lib`。
- 验证命令：
  - `MSBuild KswordCLI.vcxproj /t:Build /p:Configuration=Debug;Platform=x64` 通过。
  - `KswordCLI.exe network afd --limit 3` 返回 degraded fallback rows。
  - `KswordCLI.exe network nsi --limit 3` 返回 degraded fallback rows。

### 8.2 主 GUI 进程详情模块页占位清理

- `ProcessDetailWindow.Module.cpp` 右键菜单中的“转到模块（预留）/暂未实现”已替换为“查看模块详情”。
- 新增 `showCurrentModuleDetailDialog()`，在当前模块行上打开只读详情弹窗：
  - 使用项目内置 `CodeEditorWidget` 展示复杂文本属性。
  - 显示 PID、进程名、模块路径、模块基址、模块大小、入口 RVA、签名状态、运行状态、代表线程。
  - 提供“复制全部”“打开目录”“关闭”按钮。
  - 弹窗显式设置不透明背景样式，避免浅色/深色主题下出现黑底不可读。
- 验证命令：
  - `MSBuild Ksword5.1.vcxproj /t:Build /p:Configuration=Debug;Platform=x64` 通过。

### 8.3 当前编译快照

- `KswordCLI` Debug x64：通过。
- 主 GUI `Ksword5.1` Debug x64：通过。
- `KswordARKLight` Debug x64：通过。
- `KswordARKDriver` Debug x64：通过；构建和签名写入成功，`signtool verify /pa` 因当前非管理员/LocalMachine 信任区未导入仍报告信任链警告，不是编译错误。
- 未跟踪构建产物清理：
  - 已删除 `Ksword5.1/Ksword5.1/release/Ksword5.tlog/`。
  - `release/moc_predefs.h.cbt` 是 git 已跟踪文件，未删除。
- 冲突标记检查：
  - 使用行首精确模式检查 `<<<<<<<`、`=======`、`>>>>>>>`，当前目标源码/文档范围未发现合并冲突标记。
- 行尾空白检查：
  - 自定义脚本对当前修改/新增的 `.cpp/.h/.c/.md/.json/.vcxproj/.filters/.qrc` 做真实行尾空格检查，并排除 CRLF 的 `\r` 误报；未发现行尾空格或 tab。

### 7.2 R3/R0 访问边界核对

### 7.2.1 Manifest 覆盖与危险命令隔离

- `D:\Temp\ksword_r3_integration_manifests` 当前存在 `01_cli`、`02_arklight_kernel`、线程13重建的 `03_win32k_gui_reconstructed`、`04_arklight_file` 到 `12_arklight_audit_common` 以及 `13_acceptance`；原始 `03_*.json` 未找到。
- Window/Win32k 方向当前证据来自线程13重建 manifest、`10_arklight_window.json`、`docs/pdb_r0_audit_prep/03_win32k_gui_audit.md`、`KswordARKLight/Features/Window`、主 GUI `WindowDock`、CLI、shared 协议头和驱动 IOCTL registry 静态检查；该 manifest 仅证明静态接入链路，session/desktop 运行态与 unsupported/degraded 路径仍需实机验证。
- `KswordCLI` 历史上仍暴露 `callback set-rules/remove/remove-ex`、`kernel patch-inline-hook/force-unload-driver`、`redirect set-rules`、`network set-rules`、`safety set-policy`、`mutation prepare/commit/rollback` 等 mutation 命令。
- 这些命令不计入本轮 PDB/R0 只读审计通过项；验收矩阵只把 query/audit/cross-view/status 类命令算作只读审计能力，mutation 类命令必须由 safety/mutation gate 单独验收。

- 主 GUI Dock/UI 对 KswordARK 驱动访问应通过 `ArkDriverClient`。当前静态 grep 结果：
  - `NetworkAuditPage.cpp/.h` 未发现裸 `DeviceIoControl`、`CreateFileW(\\.\KswordARK*)` 或 `\\.\KswordARKLog` 访问。
  - `MainWindow.cpp` 中出现 `\\.\KswordARKLog` 仅为注释/日志文本；实际日志设备打开路径已通过 `ArkDriverClient::open(GENERIC_READ)` 和 `DriverHandle` 承载。
  - 主 GUI 中仍存在若干 `DeviceIoControl` 调用用于本地磁盘、卷、文件系统等 Win32/FSCTL 场景；这些不是 KswordARK R0 控制设备直连，不属于本轮 R0 wrapper 违规项。
- `KswordARKLight/Features/Handle/HandleClient.cpp` 中出现 `DeviceIoControl(...)` 的位置为错误消息字符串；实际调用路径是 `ksword::ark::DriverClient::deviceIoControl` 并显式传入 `DriverHandle`。
- 线程13最新静态 sweep 结果：
  - `D:\Temp\ksword_r3_integration_manifests` 下 `01` 到 `13` 数字前缀已连续存在，所有 manifest 均可通过 `python -m json.tool` 解析。
  - `git status` 当前显示的主 GUI 未跟踪 `.cpp/.h`：`ArkDriverAudit.cpp`、`KernelDockCidTab.*`、`KernelDockIpcTab.*`、`MemoryDock.ProcessMemoryEvidence.cpp`、`MemoryDock.ProcessPteTranslate.cpp`、`NetworkAuditPage.*` 均已登记到 `Ksword5.1.vcxproj` 和 `.filters`。
  - `KswordARKLight` 工程登记 sweep 未发现未登记 `.cpp/.h`（排除 `x64/debug/release/.vs` 构建目录）。
  - 聚焦主 GUI R0 访问边界后，未发现 Dock/UI 在 `ArkDriverClient` 之外直接打开或 `DeviceIoControl` 调用 KswordARK 控制设备；剩余 `DeviceIoControl/CreateFileW` 命中属于注释、ArkDriverClient 内部、或非 KswordARK 的磁盘/卷/文件/管道 Win32/FSCTL 路径。
- `KswordARKLight/Features/FeatureRegistry.cpp` 已包含 `40011 网络`、`40012 句柄`、`40013 杂项安全` 三个本轮新增入口。
  - `KswordCLI` 对用户要求的 PDB/R0 审计命令族已有静态 dispatch 证据：
    - `dyn`：`status/fields/capabilities/profile/v4-modules/v4-capabilities/v4-missing/apply-profile-v4`。
    - `process/thread`：`process crossview`、`thread crossview`。
    - `kernel`：`cid/ipc/callbacks/hooks`，其中 CID 使用 `IOCTL_KSWORD_ARK_ENUM_CID_TABLE`，IPC 使用 `IOCTL_KSWORD_ARK_QUERY_IPC_SUMMARY`。
    - `handle`：`object-table/object-header/type-matrix`。
    - `driver`：`integrity/detail/device/major/fastio`；`unloaded/piddb` 当前复用 Driver Integrity optional-globals 输出 `MmUnloadedDrivers`/`PiDDBCacheTable` 地址证据，entry 级枚举仍为 degraded。
    - `file`：`minifilter/fileobject/section/bitlocker/storage`。
    - `network`：`audit/tcp/udp/wfp/ndis`；`afd/nsi` 当前返回 degraded R3 fallback，并明确提示缺少专用 R0 IOCTL。
    - `hardware/window/memory/misc`：`audit/input/usb/pnp`、`win32k/gui/gui-threads/gpu/display/watchdog`、`scan-evidence/query-pte/scan-kexec`、`security/ci/vbs/hyperv/applocker/bam`。
  - R0/IOCTL 静态链路 sweep 结果：
    - `KswordARKDriver/src/dispatch/ioctl_registry.c` 当前解析出 107 个 registry entry，所有已注册 handler 均有声明/定义文本证据。
    - `shared/driver` 下 `KSWORD_ARK_IOCTL_FUNCTION_*` 数值 sweep 未发现重复；重点 id 包括 `0x860-0x863` DynData v4、`0x878` CID、`0x87A` IPC、`0x890-0x894` Win32k、`0x8E3` GPU/Display Watchdog。
    - R0 PDB/audit 重点链路已存在 registry entry：DynData v4、Network TCP/UDP/WFP/NDIS、CID/IPC、Win32k、Device audit、Handle、Driver integrity、Memory evidence/kernel-exec。
    - 驱动 `src/features` 下 `.c/.h` 工程登记 sweep 已清零；线程13补充了 `KswordARKDriver.vcxproj.filters` 中 `src\features\memory\memory_kernel_exec_scan_internal.h` 的 filter 条目，该头文件原本已存在于 `.vcxproj`。
  - 工程 XML / R3 wrapper 链路 sweep 结果：
    - 主 GUI、KswordARKLight、KswordARKDriver、KswordCLI 的 `.vcxproj` 与 `.vcxproj.filters` 均可被 XML parser 正常解析。
    - 按当前项目归属范围做登记检查后，主 GUI、KswordARKLight、KswordARKDriver 均未发现新增 `.cpp/.c/.h` 缺 `.vcxproj/.filters` 登记；主 GUI 检查排除了历史 `backup`、第三方 `include/ads`、旧 UI/helper 和临时句柄工具目录。
    - ArkDriverClient wrapper 链路有声明/定义/引用证据：DynData v4、Network TCP/UDP/WFP/NDIS、Win32k/GPU、CID、IPC 均在 `ArkDriverClient.h` 声明、`ArkDriverAudit.cpp` 定义，并由 CLI 或对应 Dock/UI 页面引用。
  - Manifest 内容一致性 sweep 结果：
    - `01` 到 `13` manifest 中 `modifiedFiles/createdFiles/addedFiles/sourceEvidence/modifiedFilesInferredFromCurrentState` 声明路径均存在。
    - 线程13修正了重建 `03_win32k_gui_reconstructed.json` 中一处证据路径：`KswordARKDriver/src/features/device` 改为当前真实存在的 `KswordARKDriver/src/features/device_audit`。
    - manifest 声明的 `.cpp/.c/.h` 文件均已登记到对应 `.vcxproj/.filters`，缺失数为 0。

### 7.3 当前不能标为完成的项目

- DynData v4 CLI 下发：
  - R0/shared/ArkDriverClient 已存在 `IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_V4` 与 `applyDynDataProfileV4`。
  - `KswordCLI.cpp` 已补 `dyn apply-profile-v4 --blob ...`，按原始 `KSW_APPLY_DYN_PROFILE_V4_REQUEST` 透传，使用 `GENERIC_READ` 打开设备以匹配 v4 `FILE_ANY_ACCESS` 协议意图，并打印 `statusFlags`、applied/rejected、required/optional 覆盖率、missing required/optional、module identity 和 message。
  - 脚本解析 `shared/driver/*.h` 后确认 `0x860`、`0x861`、`0x862`、`0x863` 仅由 DynData v4 四个 function id 使用，未发现同范围冲突。
  - 本项当前只能标为“静态入口已补”，仍需真实驱动环境验证。
- Network AFD/NSI/HTTP.sys/SMB：
  - 当前 CLI 对 `network afd`、`network nsi` 已从硬 unsupported 升级为 degraded R3 fallback；shared 协议中仍未见对应独立只读审计 IOCTL。
  - `network afd` 输出 `GetExtendedTcpTable` / `GetExtendedUdpTable` owner PID 投影；`network nsi` 输出 `GetAdaptersAddresses` adapter/unicast 投影。
  - 这些仍按设计文档标为 P1/P2 后续项，不阻塞 TCP/UDP/WFP/NDIS P0 接入验收。
- 运行态验证：
  - 本轮未编译、未启动程序、未加载驱动，因此所有 UI 渲染、真实 IOCTL 返回、profile identity mismatch、required/optional 降级、partial/budget 行为仍需运行态验收。

### 7.4 本轮修正清单

- `Ksword5.1/Ksword5.1/NetworkDock/NetworkAuditPage.cpp`
  - R0 网络摘要补充 `parsed` 计数。
  - Kernel 标识资源路径修正为 `:/Image/kernel_badge.png`。
- `Ksword5.1/Ksword5.1/WindowDock/WindowDock.cpp`
  - Kernel 标识资源路径修正为 `:/Image/kernel_badge.png`。
- `Ksword5.1/Ksword5.1/release/Ksword5.tlog/`
  - 删除未跟踪构建中间产物目录。

### 7.5 后续建议

1. 在允许编译的验收会话中编译主 GUI、KswordARKLight、KswordCLI，优先验证新增工程注册与 `dyn apply-profile-v4` 编译通过。
2. 在真实驱动环境中使用合法 v4 blob 与错配 identity blob 验证 `dyn apply-profile-v4 --blob ...` 的 ok / unsupported / rejected 输出。
3. 在真实驱动环境中分别验证：
   - DynData v4 identity mismatch 拒绝；
   - required 缺失时 capability 不置位；
   - optional 缺失时 degraded；
   - 网络 TCP/UDP/WFP/NDIS R0 wrapper 返回 unsupported/unavailable/ok 时 UI 表格不崩溃。
4. 保持 AFD/NSI/HTTP.sys/SMB 的 R0/PDB 深审计未完成标记；CLI AFD/NSI 的 degraded R3 fallback 不能替代 shared/R0/R3 全链路协议。

## 9. 2026-06-27 继续验收记录

本节记录继续验收时新增的当前状态证据。它补充 8.x 的矩阵，但不把仍缺
PDB 结构字段支持的项误判为已完成。

### 9.1 静态占位清理

- `Ksword5.1/Ksword5.1/KernelDock/KernelDeviceDriverObjectsTab.h`
  中原注释仍写着“TODO(集成)”。
- 当前静态证据显示该页已经实际挂载：
  - `KernelDock.cpp` include `KernelDeviceDriverObjectsTab.h`。
  - `KernelDock.cpp` 在对象命名空间页创建 `new KernelDeviceDriverObjectsTab(...)`。
- 已将头部说明改为“当前已由 KernelDock.cpp 挂载到对象命名空间页”，避免验收扫描把已集成页面误判为未完成 TODO。

### 9.2 本轮编译验证

在当前 worktree 下执行以下 Debug x64 构建，均通过：

- `Ksword5.1/Ksword5.1/Ksword5.1.vcxproj`
  - 输出：`Ksword5.1.exe`。
- `KswordCLI/KswordCLI.vcxproj`
  - 输出：`KswordCLI.exe`。
- `KswordARKLight/KswordARKLight.vcxproj`
  - 输出：`KswordARKLight.exe`。
- `KswordARKDriver/KswordARKDriver.vcxproj`
  - 输出：`KswordARK.sys`。
  - Signability test：Errors=None，Warnings=None。
  - 测试签名写入成功。
  - `signtool verify /pa` 仍因当前非管理员且 LocalMachine 信任区未导入测试证书返回 `0x80096019`；这不是编译失败。

### 9.3 PDB evidence 当前边界

- Driver Integrity optional globals 当前已能通过 DynData/PDB profile 获取：
  - `MmUnloadedDrivers` 全局 RVA。
  - `MmLastUnloadedDriver` 全局 RVA。
  - `_UNLOADED_DRIVERS` 的 `Name`、`StartAddress`、`EndAddress`、`CurrentTime` 字段偏移。
  - `_UNLOADED_DRIVERS` 的 `TypeSize`，用于 R0 按 PDB 类型大小计算 entry stride。
  - `PiDDBCacheTable` 全局 RVA。
- 驱动侧当前状态：
  - `MmUnloadedDrivers` 已从 global-only 升级为有界摘要：读取 `MmLastUnloadedDriver`，按 `_UNLOADED_DRIVERS.TypeSize` 扫描最多 50 项，并提取最新非空 entry 的 name/start/end/time。
  - 缺少任一 required PDB 字段或 TypeSize 异常时返回 partial/PDB-required，不猜测结构步长。
  - `PiDDBCacheTable` 仍只输出全局地址证据：`PiDDBCacheTable global address=...; table schema not walked.`
- 仍存在的真实能力边界：
  - `_RTL_AVL_TABLE` / PiDDB cache entry 的表结构和 entry 字段尚未进入 shared DynData 字段集。
- 因此本轮只实现 `MmUnloadedDrivers` 的稳定 PDB TypeSize 有界摘要；PiDDB 没有猜测 AVL 或 entry 布局，也没有伪造 entry 级枚举。后续若要完成 PiDDB entry walk，应先扩展 PDB extractor、shared DynData 协议、R0 校验与 R3 展示。
