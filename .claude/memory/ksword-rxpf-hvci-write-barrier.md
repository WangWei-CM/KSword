---
name: ksword-rxpf-hvci-write-barrier
description: HVCI 下 RXPF 写入 CI.dll 数据页的三道不可越障碍，以及 MmProbeAndLockPages(IoWriteAccess) 的不可捕获 0x1A 崩溃
metadata:
  type: project
---

## HVCI 下 RXPF 写入 CI.dll g_CiOptions 的实测结果

三道障碍全部实测确认（2026-10-17，Build 22621，CodeIntegrityOptions=0x83607）：

1. **MmProbeAndLockPages(IoWriteAccess) → 不可捕获的 0x1A (MEMORY_MANAGEMENT, subcode 0x5150b)**
   - `__try/__except` 挡不住，不是 STATUS_ACCESS_VIOLATION 而是 BugCheck
   - 原因：VTL1 的 SLAT 保护在 MM 内部触发页管理损坏
   - 修法：EXTERNAL_IMAGE 始终用 IoReadAccess

2. **IoReadAccess MDL → MmMapLockedPagesSpecifyCache 创建的别名只读**
   - 写入别名得到可捕获的 STATUS_ACCESS_VIOLATION (0xC0000005)
   - PTE 层面只读，__try 正常捕获，不崩

3. **直接写入原始 VA → 同样的 0x1A 崩溃**
   - 对 image section 页的写故障走 MM 特殊处理路径 → BugCheck
   - __try 同样挡不住

**结论**：VTL0 无法写入 CI.dll g_CiOptions 页面，与 [[ksword-vbs-no-coexistence]] 一致。
写入路径在非 HVCI 机器上应当可用（IoWriteAccess 成功 → 别名可写），但 HVCI 下结构性不可行。

**Why:** 安全内核（VTL1）通过 SLAT 控制物理帧读写权限，g_CiOptions 所在帧被标记为只读。
所有 VTL0 机制（MDL、别名映射、直接 VA）最终都指向同一物理帧，全部被拦截。

**How to apply:** DisableDseBackend 在 HVCI 机器上应该：
- 仍可 REGISTER + CHANGE（不崩），但 WRITE 会返回 0xC0000005
- GUI 报告"HVCI 阻止了 DSE 写入"而不是静默失败
- 不要尝试 IoWriteAccess（会不可恢复地崩溃）
