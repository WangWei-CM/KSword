---
name: ksword-rxpf-is-half-useless-without-shadow-idt
description: RXPF write-only mode 在 HVCI 下的实际价值几乎为零；修改的 .text 字节不可执行、数据页改不动，整个项目依赖物理机 LIDT 能跑才活
metadata:
  type: project
---

## 事实链

HVCI 下 RXPF 实际能做什么、不能做什么（2026-10-18 实测收敛）：

```
能做：
  ✓ 把 CI.dll / 其他内核模块的 .text 页通过别名改字节
  ✓ WRITE IOCTL（NOP / JMP / 64B 任意）全部返回成功
  ✓ UNREGISTER 还原字节（带 CAPTURE_BACKUP 时）

不能做：
  ✗ 执行修改后的字节（原页 PTE 停在 RO/NX，CPU 执行到就 #PF-NX）
  ✗ 改任何 .data / .rdata 页（CHANGE 后别名钳在 PAGE_READONLY，见 [[ksword-rxpf-hvci-write-barrier]]）
  ✗ 让页回到 PAGE_EXECUTE_READ（VTL1 校验内容已改 → 拒绝）
```

## 影响：常见"HVCI 下还能用 RXPF 做什么"的方案全部结构性不可行

- ❌ **修改 `g_CiOptions` 关 DSE** — 在 .data 节，CHANGE 过不去
- ❌ **Patch `CiValidateImageHeader` 等 CI 函数返回 0** — 字节能写但不执行，等于没改
- ❌ **Inline hook 内核 API（JMP stub）** — 同上
- ❌ **热补丁函数修复漏洞** — 同上
- ❌ **临时改代码路径让某个检查放行** — 同上

**整条 RXPF 的实用价值 = 修改字节 × 能让字节执行**。前半在 HVCI 下只对 .text 成立，后半完全依赖影子 IDT 做 #PF 模拟。影子 IDT 在当前代码里被 CPUID "Microsoft Hv" 检测挡住（见 `page_fault_manager.c:494-580`），而那条检测在物理 HVCI 机上是否过于保守，是整个项目的生死线。

## 生死线：物理 HVCI 机 LIDT 行为

| LIDT 结果 | 对 RXPF 的意义 |
|-----------|---------------|
| `LIDT_SUCCEEDED` + 影子 IDT 收到 #PF | 影子 IDT 可装 → RXPF 可从 write-only 升级到 emulation → **项目活** |
| `LIDT_EXCEPTION`（嵌套 VM 已实测 `STATUS_PRIVILEGED_INSTRUCTION`） | Hyper-V 拒绝 LIDT → 影子 IDT 装不上 → **RXPF 死在 write-only 这步** |
| `LIDT_SILENTLY_DROPPED` | Hyper-V 模拟但没真切 IDTR → 等于拒绝 → 同上 |

验证驱动：`docs/security/vbs-vtl0-observer/`（已签名的 .sys 产物位于 `output/vbs-vtl0-observer/x64/Release/`），四阶段 Phase A/B/C/D，Registry 写报告到 `HKLM\Software\VbsVtl0Observer\LastRun`。

**双场景实测（2026-10-18）均返回 `LIDT_EXCEPTION status=0xC0000096` (STATUS_PRIVILEGED_INSTRUCTION)**：

| 场景 | OS Build | CodeIntegrityOptions | LIDT 处置 |
|------|---------|---------------------|---------|
| 嵌套 Hyper-V VM | 22621 | 0x00083607 | `0xC0000096` |
| 物理 Win11 ProForWS | 26300 | 0x0000F701 (含 STRICTMODE+IUM+WHQL) | `0xC0000096` |

两场景一致 → Hyper-V 对所有 VTL0 内核的 LIDT 都是"拒绝+注入 STATUS_PRIVILEGED_INSTRUCTION"，与 host/guest、build 版本、HVCI 严格度无关。

**最终定案：RXPF 影子 IDT 路线在当前 Windows 架构下无出路。** `page_fault_manager.c:531-546` 的 CPUID "Microsoft Hv" 阻断不是过度保守而是正确判断，无需放宽。

**Why:** 这条项目格局之前多次被我（Claude）错误地描述为"HVCI 下 RXPF write-only mode 可用于 DSE 绕过"等，参见 [[claude-rxpf-capability-confusion]]。实际 write-only mode 单独是半成品，需要影子 IDT 做搭档才有实用价值。

**How to apply:**
- 任何在 HVCI 下讨论 RXPF 用途的问题，必须先问"要让字节执行吗"
- 要执行 → 回答"需要影子 IDT，当前被 Hyper-V 挡住，物理机结果未知"
- 不执行只是改字节让别人读 → 没有可利用的"别人"（内核不会读 .text 做决策）
- 新 feature 建议如果依赖 RXPF 修改任何 .data 字段 → 结构性不可行，直接否
- 新 feature 建议如果依赖"修改 .text 让新字节生效" → 依赖影子 IDT，要么等物理机实测要么换思路
