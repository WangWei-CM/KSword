# Security CI/VBS/Hyper-V PDB Read-Only Audit Prep

## Scope

本页设计 `ci/skci/securekernel/appid/applockerfltr/mssecflt/ahcache/bam/vmbus/vmswitch/vpci/winhv/winhvr/hvloader/HvService/HvSocket` 基于 PDB 的只读安全态势审计。目标是为后续 R0/R3 页面、DynData/PDB profile、验收脚本准备字段和边界，不在本阶段修改主项目源码、tools 或 IOCTL 注册。

现有可复用基础：

- `shared/driver/KswordArkTrustIoctl.h` 已定义 `SystemCodeIntegrityInformation`、Secure Boot、`SeGetCachedSigningLevel` 只读 trust 查询。
- `shared/driver/KswordArkCapabilityIoctl.h` 已有 read-only capability 表达方式，可新增安全态势能力项。
- `shared/driver/KswordArkPreflightIoctl.h` 已把无法内核自证的外部门槛显式标为 `NotRun`，本页沿用该语义。
- `docs/next_phase_manifests/driver_kernel_integrity.md` 已定义 driver integrity cross-view 思路，可与 driver trust 做只读交叉验证。

PDB 缓存只读根目录：`E:\KswordPDB\PDB\pdb-cache\amd64`。本次已确认存在目标 PDB 名称：`ci.pdb`、`skci.pdb`、`securekernel.pdb`、`appid.pdb`、`applockerfltr.pdb`、`mssecflt.pdb`、`ahcache.pdb`、`bam.pdb`、`vmbus.pdb`、`vmswitch.pdb`、`vpci.pdb`、`winhv.pdb`、`winhvr.pdb`、`hvloader.pdb`、`HvService.pdb`、`HvSocket.pdb`。

## Safety Boundaries

所有条目默认只读审计：

- 不读取凭据、token secret、LSA secret、DPAPI material、Kerberos/NTLM material。
- 不提取密钥、不导出证书私钥、不读取受保护服务的敏感内存。
- 不绕过 Code Integrity、HVCI、SKCI、AppLocker、AppID、mssecflt 或 Hyper-V/VBS 机制。
- 不关闭安全机制，不修改 BCD、注册表策略、CI policy、AppLocker policy、minifilter 状态、Hyper-V switch/port/device 状态。
- 不扫描用户隐私活动作为默认行为；`ahcache/bam` 仅用于状态/记录存在性和审计可用性提示，默认不枚举用户历史明细。
- R0 只返回状态、计数、模块归属、地址可用性、版本/符号置信度和 bounded evidence rows；R3 负责解释、过滤和展示。

## Protocol Shape

建议新增一组只读协议，或先作为下一阶段 manifest 记录：

- Request: `KSWORD_ARK_QUERY_SECURITY_POSTURE_REQUEST`
  - `size`, `version`, `flags`
  - `maxEntries`
  - `targetProcessId`，仅在请求 process protection/signature cross-view 时使用。
  - `privacyMode`，默认 `SummaryOnly`。
- Response: `KSWORD_ARK_QUERY_SECURITY_POSTURE_RESPONSE`
  - `overallStatus`
  - `fieldFlags`
  - `codeIntegrityOptions`
  - `secureBootEnabled`, `secureBootCapable`
  - `vbsFlags`, `hvciFlags`, `skciFlags`
  - `testSigningDebugFlags`
  - `driverTrustSummary`
  - `processTrustSummary`
  - `appidSummary`, `mssecfltSummary`
  - `hypervSummary`
  - `ahcacheBamSummary`
  - variable `KSWORD_ARK_SECURITY_POSTURE_EVIDENCE entries[]`
- Evidence row fields:
  - `priority` (`P0/P1/P2`)
  - `category`
  - `sourceModule`
  - `symbolNameHash` or stable symbolic name string when safe
  - `objectAddress`
  - `targetAddress`
  - `state`
  - `riskFlags`
  - `confidence`
  - `ntstatus`
  - `detail`

所有私有结构偏移应由 PDB/DynData profile 提供；缺失时返回 `Unavailable` 或 `Degraded`，不得猜测偏移。

## P0 MVP

### P0.1 Code Integrity Options

- PDB modules: `ntoskrnl.pdb`/`ntkrnlmp.pdb`, `ci.pdb`
- Candidate structures/globals:
  - public query path: `ZwQuerySystemInformation(SystemCodeIntegrityInformation)`
  - existing flags: `CODEINTEGRITY_OPTION_ENABLED`, `TESTSIGN`, `UMCI`, `HVCI_KMCI`, audit/strict flags
  - `ci.pdb` candidate globals/functions for corroboration only: CI policy/options globals, CI validation entrypoints, policy cache state
- R0 fields:
  - `codeIntegrityOptions`
  - `ciEnabled`, `umciEnabled`, `hvciKmciEnabled`
  - `ciAuditMode`, `hvciAuditMode`, `hvciStrictMode`
  - `ciSymbolStatus`, `ciPdbConfidence`
- R3 fields:
  - decoded option chips
  - source label: `SystemCodeIntegrityInformation`, `ci.pdb corroborated`, or `R0 unavailable`
- UI:
  - Security Posture page top status strip: `CI`, `UMCI`, `HVCI`, `Audit`, `Strict`
  - show raw flags in expandable details.
- Acceptance:
  - matches existing `QUERY_IMAGE_TRUST` `codeIntegrityOptions`.
  - missing CI PDB does not fail the page; it only reduces confidence.

### P0.2 HVCI/VBS/Secure Kernel/SKCI Status

- PDB modules: `securekernel.pdb`, `skci.pdb`, `ci.pdb`, `winhv.pdb`, `winhvr.pdb`, `hvloader.pdb`
- Candidate structures/globals:
  - Secure Kernel runtime state globals in `securekernel.pdb`
  - SKCI policy/options globals in `skci.pdb`
  - hypervisor interface/state globals in `winhv.pdb`/`winhvr.pdb`
  - boot handoff/hypervisor loader metadata in `hvloader.pdb`
- R0 fields:
  - `vbsPresent`, `secureKernelPresent`, `skciPresent`
  - `hvciKmciEnabled`, `hvciAuditMode`, `hvciStrictMode`, `hvciIumEnabled`
  - `secureKernelModuleLoaded`, `skciModuleLoaded`
  - `secureKernelSymbolStatus`, `skciSymbolStatus`
- R3 fields:
  - VBS/HVCI summary
  - source list with `SystemCodeIntegrityInformation`, loaded-module cross-view, PDB evidence
- UI:
  - one row per layer: Boot/Hypervisor, Secure Kernel, SKCI, HVCI.
  - states: `Enabled`, `Audit`, `Present`, `Unavailable`, `Unknown`.
- Acceptance:
  - on non-VBS systems, returns clean `Present=false` rows rather than errors.
  - on VBS systems, CI flags and loaded module evidence agree or produce an explicit `Conflict` evidence row.

### P0.3 Test Signing and Debug Flags

- PDB modules: `ntoskrnl.pdb`/`ntkrnlmp.pdb`, `ci.pdb`, `hvloader.pdb`
- Candidate structures/globals:
  - `SystemCodeIntegrityInformation` test/debug bits
  - kernel debugger state from supported kernel APIs/globals
  - boot loader/hypervisor loader debug/test-signing handoff candidates
- R0 fields:
  - `testSigningEnabled`
  - `ciDebugModeEnabled`
  - `kernelDebuggerEnabled`
  - `kernelDebuggerNotPresent`
  - `testBuild`, `flightBuild`, `flightingEnabled`
- R3 fields:
  - warnings separated from failure; test mode is posture evidence, not an action request.
- UI:
  - compact warning banner only when test/debug mode is active.
- Acceptance:
  - no BCD reads/writes in R0.
  - flags match documented system query outputs where available.

### P0.4 Driver Trust Cross-View

- PDB modules: `ntoskrnl.pdb`/`ntkrnlmp.pdb`, `ci.pdb`
- Candidate structures/globals:
  - loaded module list evidence from existing driver integrity design
  - `SeGetCachedSigningLevel`
  - CI validation export presence; do not call private CI policy mutation paths
- R0 fields:
  - per-driver `base`, `size`, `pathHash`, `signingLevel`, `signingLevelFlags`
  - `ownerModule`, `driverObject`, `driverSection`
  - `moduleListPresent`, `driverObjectPresent`, `trustPresent`
  - `crossViewRiskFlags`
- R3 fields:
  - driver table: name, path, signing level, source count, conflicts
- UI:
  - merge with existing Driver Integrity page or add a Security tab in DriverDock.
  - highlight only conflicts: unsigned loaded module, missing cached level, object/list mismatch.
- Acceptance:
  - no file content hashing by default.
  - existing `QUERY_IMAGE_TRUST` single-file result agrees with per-driver cached signing level when path is resolvable.

## P1 Security Stack Audit

### P1.1 Driver Signature Level

- PDB modules: `ntoskrnl.pdb`/`ntkrnlmp.pdb`, `ci.pdb`
- Candidate structures/globals:
  - driver object/section/image metadata
  - cached signing level fields and CI policy cache candidates
- R0 fields:
  - `driverSignatureLevel`
  - `sectionSignatureLevel`
  - `imageSigningLevel`
  - `signatureSource`
  - `confidence`
- R3 fields:
  - normalized labels from existing `KSWORD_ARK_SIGNING_LEVEL_*`.
- UI:
  - same vocabulary as Image Trust: `Unsigned`, `Authenticode`, `Microsoft`, `Windows`, `Windows TCB`.
- Acceptance:
  - never attempts to raise/lower signing level.
  - unknown private fields return `Unavailable`.

### P1.2 Process Protection and Signature Level

- PDB modules: `ntoskrnl.pdb`/`ntkrnlmp.pdb`, `ci.pdb`
- Candidate structures/globals:
  - `_EPROCESS.Protection`
  - `_EPROCESS.SignatureLevel`
  - `_EPROCESS.SectionSignatureLevel`
  - `_PS_PROTECTION`
- R0 fields:
  - `processId`
  - `protectionType`, `protectionSigner`
  - `signatureLevel`, `sectionSignatureLevel`
  - `imageFileName`, `pathHash`
  - `dynDataFieldMask`
- R3 fields:
  - process trust row, protection label, signature labels
- UI:
  - Process Detail `Trust` section.
  - warnings for contradictory evidence only; protected process status itself is normal posture data.
- Acceptance:
  - read-only guarded copy from EPROCESS.
  - no process protection patch path, no writes to EPROCESS.

### P1.3 AppLocker/AppID/applockerfltr

- PDB modules: `appid.pdb`, `applockerfltr.pdb`, `ci.pdb`
- Candidate structures/globals:
  - AppID service/policy runtime globals
  - applockerfltr minifilter registration/filter state
  - policy cache/list heads and counters, if stable per build
- R0 fields:
  - `appidServicePresent`
  - `applockerFilterLoaded`
  - `applockerFilterRegistered`
  - `policyCachePresent`
  - `policyRuleCountApprox`
  - `lastPolicyStatus`
- R3 fields:
  - policy availability summary, filter state, rule-count confidence
- UI:
  - `Application Control` panel with AppID service, filter, policy cache.
- Acceptance:
  - no policy modification.
  - no rule export by default; only counts/status unless user explicitly opens details later.

### P1.4 mssecflt

- PDB modules: `mssecflt.pdb`
- Candidate structures/globals:
  - minifilter registration state
  - monitored volume/instance lists
  - policy/status counters, if PDB exposes stable symbols
- R0 fields:
  - `mssecfltLoaded`
  - `filterRegistered`
  - `instanceCount`
  - `lastStatus`
  - `symbolConfidence`
- R3 fields:
  - Windows security minifilter status and instance count.
- UI:
  - under `Application Control / File Security Filters`.
- Acceptance:
  - no detach/unload/control requests.
  - if symbol layout differs, show loaded-module-only evidence.

## P1 Hyper-V/VBS Audit

### P1.5 Hypervisor Present

- PDB modules: `winhv.pdb`, `winhvr.pdb`, `hvloader.pdb`, `ntoskrnl.pdb`/`ntkrnlmp.pdb`
- Candidate structures/globals:
  - hypervisor present/enlightenment state
  - root partition state
  - hvloader boot metadata candidates
- R0 fields:
  - `hypervisorPresent`
  - `rootPartition`
  - `enlightenmentFlags`
  - `hypervisorVendor`
  - `sourceMask`
- R3 fields:
  - Hyper-V state summary and source confidence.
- UI:
  - `Hypervisor` row in VBS panel.
- Acceptance:
  - agrees with supported OS query/CPUID evidence where available.
  - no hypercall control operations.

### P1.6 VMBus Channels

- PDB modules: `vmbus.pdb`
- Candidate structures/globals:
  - channel list/table globals
  - channel object type, state, offer/channel identifiers
  - device relation lists
- R0 fields:
  - `channelCount`
  - per-channel `channelId`, `state`, `classGuid`, `deviceObject`, `targetVtl` when observable
  - `enumerationTruncated`
- R3 fields:
  - channel table with count-first summary.
- UI:
  - Hyper-V detail page: channels grouped by state/class.
- Acceptance:
  - bounded enumeration with max rows.
  - no packet reads, no ring-buffer content dump by default.

### P1.7 vSwitch Extensions

- PDB modules: `vmswitch.pdb`
- Candidate structures/globals:
  - switch/port/extension list heads
  - extension state/counters
  - NDIS filter binding metadata candidates
- R0 fields:
  - `switchCount`
  - `portCount`
  - `extensionCount`
  - per-extension `nameHash`, `state`, `providerGuid`
- R3 fields:
  - extension inventory with status.
- UI:
  - `vSwitch` tab: switches, ports, extensions.
- Acceptance:
  - no packet inspection.
  - no extension enable/disable/reorder operation.

### P1.8 vPCI Devices

- PDB modules: `vpci.pdb`
- Candidate structures/globals:
  - vPCI root/device lists
  - device state, instance identifiers, resource assignment state
- R0 fields:
  - `deviceCount`
  - per-device `state`, `vendorId`, `deviceId`, `instanceHash`, `pdoAddress`
  - `resourceState`
- R3 fields:
  - virtual PCI inventory.
- UI:
  - `vPCI` table under Hyper-V details.
- Acceptance:
  - no resource reassignment.
  - no config-space writes.

### P1.9 HvSocket Endpoints Feasibility

- PDB modules: `HvSocket.pdb`, `HvService.pdb`, `vmbus.pdb`
- Candidate structures/globals:
  - service endpoint/list globals
  - socket provider state
  - VMBus service channel linkage candidates
- R0 fields:
  - `hvSocketProviderPresent`
  - `endpointCountApprox`
  - per-endpoint optional `serviceGuid`, `state`, `ownerProcessId` only if already exposed by kernel object state
  - `privacyMode`
- R3 fields:
  - feasibility state: `Unavailable`, `SummaryOnly`, `EndpointInventory`.
- UI:
  - hidden behind Hyper-V advanced details; default summary only.
- Acceptance:
  - no socket payload reads.
  - no endpoint connection attempts.
  - owner/process data omitted unless needed for administrative posture and clearly labeled.

## P2 Auxiliary State

### P2.1 ahcache Read-Only Feasibility

- PDB modules: `ahcache.pdb`
- Candidate structures/globals:
  - cache service/driver state globals
  - record/table counters
  - version and backing-store state
- R0 fields:
  - `ahcachePresent`
  - `recordCountApprox`
  - `cacheVersion`
  - `lastStatus`
  - `privacyMode`
- R3 fields:
  - availability and count summary.
- UI:
  - `Auxiliary Records` section, collapsed by default.
- Acceptance:
  - default does not list application execution records.
  - no user SID/path timeline export.

### P2.2 BAM Read-Only Feasibility

- PDB modules: `bam.pdb`
- Candidate structures/globals:
  - BAM runtime state globals
  - record/list counters
  - service/driver initialized flags
- R0 fields:
  - `bamPresent`
  - `recordCountApprox`
  - `initialized`
  - `lastStatus`
  - `privacyMode`
- R3 fields:
  - state/count only.
- UI:
  - same `Auxiliary Records` section as ahcache.
- Acceptance:
  - no default per-user activity listing.
  - no reconstruction of execution timeline.

## Priority Summary

- P0:
  - CI options
  - VBS/HVCI/Secure Kernel/SKCI state
  - test signing/debug flags
  - driver trust cross-view
- P1:
  - driver signature level details
  - process protection/signature level
  - AppLocker/AppID/applockerfltr
  - mssecflt
  - Hypervisor present
  - VMBus channels
  - vSwitch extensions
  - vPCI devices
  - HvSocket feasibility
- P2:
  - ahcache summary-only feasibility
  - BAM summary-only feasibility

## MVP Delivery Plan

MVP should be deliberately small:

1. Extend trust/preflight/capability planning with `Security Posture` read-only capability labels.
2. Implement or stage one read-only query for:
   - CI options from `SystemCodeIntegrityInformation`
   - Secure Boot from existing trust query
   - HVCI/VBS/SKCI presence from CI flags + loaded module/PDB evidence
   - test signing/debug flags
   - driver trust cross-view using loaded module evidence + cached signing level
3. R3 page shows:
   - top posture strip: `CI`, `Secure Boot`, `HVCI`, `VBS`, `SKCI`, `TestSign`, `Debug`
   - driver trust table with source/confidence/conflict columns
   - a details drawer containing raw flags and evidence rows
4. Explicitly defer:
   - AppLocker policy details
   - mssecflt instances beyond loaded/registered status
   - Hyper-V inventories
   - ahcache/BAM record details

MVP acceptance:

- Works on systems with and without VBS.
- Missing PDB profiles degrade to existing documented OS queries.
- No write IOCTLs, no policy changes, no unload/detach/control operations.
- Driver trust cross-view identifies at least: signed level available, unavailable, and conflicting owner/list evidence.

## Validation Checklist

- PDB availability:
  - confirm target PDB names exist in `E:\KswordPDB\PDB\pdb-cache\amd64`.
  - record build/version/hash in generated PDB profile metadata.
- R0 safety:
  - all collection paths run at safe IRQL.
  - all private pointer reads use guarded bounded copy helpers.
  - all lists have maximum entry count and loop detection.
  - all missing symbols return `Unavailable` or `Degraded`.
- R3 behavior:
  - default UI is summary-first.
  - privacy-sensitive auxiliary data stays collapsed and summary-only.
  - raw addresses are hidden unless advanced/debug display is enabled.
- Regression:
  - existing `QUERY_IMAGE_TRUST` and `QUERY_PREFLIGHT` results remain unchanged unless a future implementation explicitly versions the protocol.
  - no new mutating safety policy flags are required for this page.
