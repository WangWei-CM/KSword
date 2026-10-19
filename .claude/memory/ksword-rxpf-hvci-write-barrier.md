---
name: ksword-rxpf-hvci-write-barrier
description: HVCI 下 RXPF 只能修改 .text 页，.data/.rdata 全部被 VTL1 钳在 RO；MmChangeImageProtection 只对原本 RX 的页释放写保护
metadata:
  type: project
---

## HVCI 下 RXPF 的真实可写范围（实测 2026-10-18，Build 22621）

**规则：`MmChangeImageProtection(section, PAGE_READONLY)` 只对原本 `PAGE_EXECUTE_READ` 的页释放 VTL1 SLAT 写保护。** 对原本不可执行的页（PAGE_READONLY / PAGE_READWRITE 等），CHANGE 名义成功但别名拿不到 PAGE_READWRITE，`MmProtectMdlSystemAddress` 把别名钳在 PAGE_READONLY。

CI.dll 实测矩阵（EXTERNAL_IMAGE + CHANGE）：

| 偏移 | 节 | origP | curP | alias | 结果 |
|------|-----|-------|------|-------|------|
| +0x01000 | .text 早 | 0x20 (RX) | 0x02 | 0x04 (RW) | **可写** |
| +0x10000 | .text 中 | 0x20 (RX) | 0x02 | 0x04 (RW) | **可写** |
| +0x30000 | .rdata | 0x02 (R) | 0x02 | 0x02 (R) | **不可写** |
| +0x38000 | .rdata | 0x04 (?) | 0x02 | 0x02 (R) | **不可写** |
| +0x3C000 | .data | 0x02 (R) | 0x02 | 0x02 (R) | **不可写** |

同族 .text 已验证可写模块：CI.dll / FLTMGR.SYS / ndis.sys / tcpip.sys / NETIO.SYS。

## 对 g_CiOptions 的具体后果

`g_CiOptions` 在 CI.dll 的 .data 节 → 走 EXTERNAL_IMAGE 第一步 CHANGE 就拿不到 RW 别名 → 无法通过 RXPF 修改。

同样过不去的还有：**CI.dll 的所有数据状态、CiCallbacks 表、g_CiNtosDebugMode、任何内核模块的 .data/.rdata**。

早期还踩过另一组坑（已固定修法，别回退）：
1. **MmProbeAndLockPages(IoWriteAccess) → 不可捕获的 0x1A** `MEMORY_MANAGEMENT subcode 0x5150b`。`__try/__except` 挡不住。EXTERNAL_IMAGE 必须用 `IoReadAccess`。
2. **直接写入原始 VA（跳过 MDL）→ 同样 0x1A**。image section 页的写故障走 MM 特殊路径 → BugCheck，`__try` 同样挡不住。

**Why:** VTL1 把 .text 页的 SLAT 从 RX 翻到 RW 是 Windows 自己为了 patchguard/热补丁需要而留下的通路；.data/.rdata 页从来不需要变可执行所以 VTL1 不开这个口子。`MmChangeImageProtection` 字面意义就是"改 Image 保护"，Image 指映像的**可执行**部分。

**How to apply:**
- 任何"修改 g_CiOptions 来关 DSE"的方案在 HVCI 下结构性不可行，别再提
- 任何涉及"修改 CI 数据状态"的 bypass 方案在 HVCI 下结构性不可行
- RXPF 的真实能力面是：**改 .text 字节本身**，而且修改后的字节**不可执行**（PTE 停在 RO/NX），要让字节生效必须接上影子 IDT 做 #PF 模拟 — 见 [[ksword-rxpf-is-half-useless-without-shadow-idt]]
- UI 文案要说"HVCI 阻止"而不是静默失败；对 .data/.rdata 的写请求驱动端应该在 CHANGE 阶段就拒绝（alias != 0x04）

与 [[ksword-vbs-no-coexistence]] 一致，三条官方封死的路加上这里的数据页封死，HVCI 下的 CI 旁路空间非常小。
