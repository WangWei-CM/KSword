---
name: claude-conflated-vbs-mechanisms
description: 我把 HVCI/KDP/HVPT 当成一堵"HVCI 页表只读"的墙，用这个判死 remap 思路；三套机制分别有不同的封堵面
metadata:
  type: feedback
---

## 规则

讨论 VBS 内任何涉及 **PTE/PFN/translation** 的方案时，不能用"HVCI 保护页表"或"HVCI 页表只读"这种统称判死。要明确说当前假设哪一层在跑：

- **HVCI**：W^X、CI 验证、执行权限（**不**直接把所有 PT 页标 RO）
- **KDP**：标记特定 GPA/PFN 不可写，但**周期**校验 GVA→GPA 关系
- **HVPT**：GVA→GPA 翻译由 Secure Kernel 管，封住 alias/remap 攻击面

**Why:** 2026-10-18 讨论"让数据段 PAGED + 接管 #PF + 换 PFN"方案时，我用"HVCI 页表保护 → PageTable 页只读 → remap 结构性不行"一句判死 ③。用户纠正：HVPT 才是封 remap 的那一层；经典 KDP 的 remap 是**历史缺口**，HVPT 不运行时技术上有空间，只是会破坏 MM 一致性且可能被周期校验发现。

**How to apply:**
- 遇到 "VBS/HVCI 下 PT 不能改" 的直觉结论时，先问自己"是哪一层封的"
- 回答方案可行性前，列清楚假设：HVCI only / HVCI+KDP / HVCI+KDP+HVPT 三种
- 查目标机 HVPT 现状的命令：`Get-CimInstance Win32_DeviceGuard | select SecurityServicesRunning`，列表含 `7` = HVPT running
- 即便 HVPT 不在跑，不要把 remap 当"可用方案"，要说明破坏 MM 一致性的具体代价（PFN DB、working set、ControlArea、引用计数、回写状态）
- 相关：[[ksword-vbs-hvci-kdp-hvpt-distinction]]（三机制详细分工）、[[claude-rxpf-capability-confusion]]（我把 WRITE ok 当项目可用的另一个错误模式）
