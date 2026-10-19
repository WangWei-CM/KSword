---
name: ksword-rxpf-mode1-realhardware-26h2-confirmed
description: 26H2 实体机 + 全部 VBS 防护开启下，RXPF mode=1 restore 保留了修改后的 RX 字节；是本项目目前最强的"HVCI 被绕过"实证
metadata:
  type: project
---

26H2 实体机（非 VM、非嵌套、非调试模式）跑 RXPF Phase F mode=1 restore 的七步实验，结论为 PASS。

**环境**：CI=启用、HVCI=启用、HVCI Strict=启用、Secure Boot=启用、TestSigning=关闭、来源=R0。
277 个内核模块已枚举。

**七步全部 success (0x00000000)**：函数解析 → MDL 分配 → 页面锁定 → 映像保护变更 → 别名映射 → 写入测试 → RX 恢复。

**决定性读回**：
- 修改前字节 = `0x48`
- 写入后字节 = `0xB7`
- **恢复后字节 = `0xB7`**（没有恢复到 0x48，修改穿透并留存）

目标 `MmChangeImageProtection@0xfffff80498662300`；MDL 别名在 `0xffffa882a687d000`；原页面在 `0xfffff80429fe0000`，flags=0x7。

**Why**：之前所有 "mode=1 restore 把修改后的内容让回 RX" 的证据都是 VM 内（嵌套 Hyper-V 下的 KSword-HVM-Target，CI 版本可能和实体机不一致，且 TestSigning 需要开）。实体机关 TestSigning + Secure Boot 开 + HVCI Strict 开，整条链路再无"因为 VM 配置差异而放行"的借口；这是正面的生产环境证据。

**How to apply**：
- 这条写进报告 §3 的稳固事实表（high-conf，附实验时间戳 07:22:24）
- §6 实验矩阵的 #7 对照条应该直接引用这条（而不是继续引 VM 的 next-exp #3）
- 任何讨论"VBS 下 RXPF 是否构成实用绕过"的新文档，都应该从这条开始，而不是从假 digest attestation 叙事开始
- 不要再把它说成"VM 现象、实体机有待确认"——已确认
- 这是对 [[ksword-rxpf-mode1-restore-structural-fail.md]] 和 [[ksword-rxpf-hvci-write-barrier.md]] 的生产环境升级版证据
