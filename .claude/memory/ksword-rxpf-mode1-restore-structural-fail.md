---
name: ksword-rxpf-mode1-restore-structural-fail
description: mode=1 对 catalog 内签名模块可返 SUCCESS 且 PTE 真回 RX（CI.dll+0x1000 实测）；但"SHA-512 digest attestation 放行修改内容"叙事已被 workflow 审计推翻——EXTERNAL_IMAGE 根本不走 PrepareReferenceDigest（硬编码 STATUS_NOT_SUPPORTED 短路），Phase F SUCCESS 的真因是 auto-restore 在 mode=1 前把物理页回写为 catalog 原字节，与 digest 无关；VTL1 实际验证算法未经证实
metadata:
  type: project
---

## 关键事实（2026-10-18 workflow 审计后修正）

### 稳的（high confidence）

1. **MmChangeImageProtection 4 参数签名** 和 MDL 约束 `(MdlFlags & 7) == 2`、`mode ∈ {1,2}` 已由 22621 ntkrnlmp 静态反汇编（`output/mmchangeimageprotection-20261018/22621-report.md`）证实
2. **mode=1 对 CI.dll+0x1000 实测可返 STATUS_SUCCESS**（**唯一已验样本**；NTOS/NDIS/FLTMGR 都没验，不能外推）
3. **本机 MiFlags & 0x4000 = 1**——证据链：`.rxpftst` 实验返 `0xC0000005`，而 `nt!MmChangeImageProtection` 的直接返回码里不包含 `0xC0000005`（见 22621-report Branches 表），故该值必然来自 `VslValidateDynamicCodePages` 内部 SEH #PF 转换；所以 VSL/VTL1 分支并没有被 `MiFlags & 0x4000 = 0` 跳过
4. **auto-restore 时序**（`image_protection_manager.c:1406-1454`）：Backup → WritableAlias → CurrentContent 全部回写为 catalog 原字节，**然后才**走 Protect(RO)/Unmap/mode=1
5. **EXTERNAL_IMAGE 永远走 fallback**：`KswRxpfPrepareReferenceDigest`（`image_protection_manager.c:1320-1322`）对 TARGET_EXTERNAL_IMAGE 硬编码 `return STATUS_NOT_SUPPORTED`，CI.dll 调 mode=1 时实际传入 `(Record->PageBase, PAGE_SIZE)` 而不是 digest 指针
6. **mode=1 真的把 PTE 翻回 RX 且物理页保持 payload**（2026-10-18 next-exp #3 PASS）——证据链见下节

### next-exp #3 决定性对照组（2026-10-18）

| 实验 | Unregister flag | mode=1 返回 | VM 存活 |
|------|---|---|---|
| #6（Phase F，auto-restore ON） | `UI_CONFIRMED` | `0x00000000` | **活** |
| #7（next-exp #3，skip-restore） | `UI_CONFIRMED \| SKIP_BACKUP_RESTORE` | `0x00000000` | **秒级 BSOD `SYSTEM_SERVICE_EXCEPTION, What failed: CI.dll`** |

两组**唯一差别**是 flag，流程完全一样（Register(CB) + Change(mode=2) + Write(0x90@0) + Unregister）。所以差异必然源自 flag 控制的那一步——auto-restore 决定物理页是原字节还是 payload。

**bugcheck 名字排除了几种可能**：
- `SYSTEM_SERVICE_EXCEPTION`（0x3B）不是 `ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY`（0xFC），所以 **PTE NX=0**（可执行）
- 不是 `PAGE_FAULT_IN_NONPAGED_AREA`（0x50），所以 PTE Present=1
- 秒级发生，排除 PatchGuard（典型几分钟到几小时）
- 两组对照，排除 CFG / 下游 NULL deref（这些不随 payload 内容改变）

**所以 mode=1 返 SUCCESS 后 PTE 真的回到 RX，物理页保持 payload**。CPU 执行到 CI.dll+0x1000 处的 0x90 后，指令解码错位进入某个异常，走到 nt 的 service handler 里再异常，变成 `SYSTEM_SERVICE_EXCEPTION`。

**仍差的直接证据**（但不影响能力结论）：
- R0 探针读 PTE qword 看 NX/Present 真实位（审计 next-exp #8）
- 保 BugCheck Arg1-4 看具体异常类型（审计 next-exp #2）

### 必须撤回（REFUTED, 旧 memory 版本写错了）

- ❌ "VTL1 用 SHA-512(目标页)[0x20,0x40) 和 caller-supplied digest 做 memcmp 放行"——VTL0 边界里只有 `VslpEnterIumSecureMode` 发个 IUM 请求再读回 ebx，SHA-512 比对的证据链在本任务里**从未闭环**；且 EXTERNAL_IMAGE 根本没传 digest
- ❌ "mode=1 对任意 catalog 内 Microsoft 签名模块的 .text 都放行"——只验了 CI.dll+0x1000 一个页，一个 payload（1 字节 0x90/0xCC @ offset=0），其他全是推测
- ❌ "Phase F 证明可以干净往返任意修改内容"——SUCCESS 真因是 auto-restore 在 mode=1 之前已把物理页重写回 catalog 原字节，mode=1 看到的是原内容；**不独立证明 VTL1 放行了修改字节**
- ❌ "外层 lastStatus=0x00000000 等于 VTL1 内部验证通过"——外层 0 兼容 VSL 返回 0 和 0x12C 两种非负值，VTL0 边界无法区分
- ❌ "originalProtection=0x20 证明 PTE 真回 RX"——该字段来自 `KswRxpfQuerySectionProtection` 的 PE 节头静态解析（`image_protection_manager.c:285-339`），不采样 live PTE 或 SLAT，是循环证据
- ❌ "`.rxpftst` 的 `0xC0000005` 真因是不在 VTL1 catalog 里"——未隔离四个混淆变量：section 名、PE characteristics、测试证书签名位阶、以及两组实验 refVA/refLen 本身不同（`.rxpftst` 传 digest 缓冲，CI.dll 走 fallback 传 PageBase）
- ❌ "SYSTEM_SERVICE_EXCEPTION What failed: CI.dll 证明修改后指令被 CPU 执行"——只保留了 Windows bucket 字符串，缺 BugCheck Code/Arg1-4/trap frame；可能是 (a) 执行被改字节 (b) CFG 拦截 (c) PatchGuard (d) NX 阻拦 (e) 下游 NULL 解引用

### UNDETERMINED（需要下一轮实验才能收口）

- VTL1 侧真正的验证算法（catalog 查找 vs 内容哈希 vs 其他 attestation）——缺错误 digest 对照
- VSL 返回 `0x12C` 的具体语义——22621-report 原文 "Meaning of 0x12C itself is not established"
- mode=1 覆盖是否超出 CI.dll+0x1000 单样本——其他模块未验
- 多字节 / 结构化指令 / 跨 cacheline payload 的 mode=1 是否仍 SUCCESS——未验
- 实验 #3/#4 BSOD 真实根因——无 BugCheck Code 证据
- HashType=0x800E 在当前 Windows 到底是 SHA-256 还是 SHA-512——KSword 的取法与 warbird-research 公开 SHA-256 推断冲突

## 工程修复（与安全结论独立，仍保留）

- MDL 别名未解映射就调 mode=1 → `0xC000000D` 修复：`image_protection_manager.c:KswRxpfImageProtectionReleaseRecord` 新顺序 `Protect(RO) → Unmap(alias) → mode=1 → Unlock/Free`
- Hyper-V 来宾下 SELF_IMAGE_TEST 门禁死锁修复：`rxpf_runtime.c:KswRxpfRuntimeRegisterPage` 把 SELF_IMAGE_TEST 加入 `AllocatedSelfTestPassed` 门禁豁免列表
- Phase F Backup auto-restore：`ReleaseRecord` 自动在 mode=1 前把 Backup 回写物理页；默认开，可用 `FLAG_SKIP_BACKUP_RESTORE (0x4)` 关掉
- GUI 接入 `FLAG_SKIP_BACKUP_RESTORE`：`RxpfPage` 加 `m_keepModifiedToggle`，`unregisterPage(id, skipBackupRestore=false)` 可选参

## 下一步该做的实验（按优先级）

1. **EXTERNAL_IMAGE bad-digest 对照**：临时解除 EXTERNAL_IMAGE 的 `STATUS_NOT_SUPPORTED` 短路，传三组 (随机 32B / SHA-256(页) / SHA-512(页)[0x20,0x40))，看 mode=1 返回能否区分 digest 内容——这是关掉 "VTL1 验 digest" 争议的决定性实验
2. **启用 kernel dump 捕获**（`CrashOnCtrlScroll` + `AlwaysKeepMemoryDump`），重跑实验 #3/#4，用 cdb `!analyze -v; .cxr; r; u @rip L8; kb` 定位 BugCheck Code 与故障 RIP 是否真落在 CI.dll+0x1000..+0x1008 内
3. **带 FLAG_SKIP_BACKUP_RESTORE 的 Phase F**：Register(CB)+Change+Write(0x90)+Unregister(skip)，立即二次 Register 查页内容——这是唯一能证明 "VTL1 对修改后 current-content 直接放行" 独立于 auto-restore 的实验
4. **扩模块覆盖**：ntoskrnl+0x1000 / ndis.sys+0x1000 / fltmgr.sys+0x1000 各跑一轮，验证外推
5. **R0 trampoline 截获 VslValidateDynamicCodePages 真实 eax**，写入 Record 新字段 `VslInternalStatus`——首次在 22621 上固定 0x12C 语义
6. **把 CreateSelfImageTestPage 的 mode=1 临时改为 fallback 形式** `(mdl, PageBase, PAGE_SIZE, 1)`，看 `.rxpftst` 是否仍返 `0xC0000005`——分离 "模块身份" vs "refVA/refLen 参数" 两条因素
7. **R0 探针直接读 nt!MiFlags(RVA 0xD1EA48)** 的 dword 值，直接确认 `0x4000` 位状态，替代间接推理
8. **Phase F SUCCESS 后 Unregister 前**，用 R0 定位 PTE qword 并回读 NX/Present bit，把 livePteSnapshot 写入 Response——替换掉 `originalProtection=0x20` 这个循环证据

## 公开 Prior Art

本工作验证的多数 ABI 事实（4 参数签名、`(MdlFlags & 7) == 2` 约束、mode 分支、VSL→VTL1 链）在 [GetRektBoy724/warbird-research pseudocodes.md](https://github.com/GetRektBoy724/warbird-research/blob/main/pseudocodes.md) 已有描述。本仓库增量集中在：Build 22621 复现、`.rxpftst` 非 catalog 的经验证伪、MDL 别名污染修复、SELF_IMAGE_TEST 门禁豁免、Backup auto-restore 工程实现。SHA-512[0x20,0x40) 取法为经验拟合、与公开 SHA-256 推断相冲突，待对照实验收口。

## How to apply

- 任何 "VTL1 用 digest 放行修改内容" 的方案在本任务证据下**不可信**——走到 Phase G 之前必须先用实验 1 关掉 digest attestation 的归因争议
- 任何依赖 "mode=1 对 Microsoft 签名模块都放行" 的方案要先问 "除了 CI.dll+0x1000 外有没有独立样本"——没有就加实验 4
- `.rxpftst` 恒拒 `0xC0000005` 的推论暂时站不住，不能据此断定 "catalog 内外是分界线"——需要实验 6
- 实验 #3/#4 的秒级蓝屏不等于 "修改后指令真被 CPU 执行"——加实验 2 收口
