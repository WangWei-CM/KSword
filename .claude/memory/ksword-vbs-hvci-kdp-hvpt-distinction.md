---
name: ksword-vbs-hvci-kdp-hvpt-distinction
description: VBS 下的 HVCI/KDP/HVPT 是三套机制分别管不同的东西，不能统称为"HVCI 页表保护"；remap 攻击的真正封堵者是 HVPT
metadata:
  type: project
---

## 三层分开看

| 机制 | 主要保护对象 | 对我们的意义 |
|------|------------|-----------|
| **HVCI** | 内核**执行权限**、CI 验证、W^X | 决定 .text 能否重新变可执行；控制 `MmChangeImageProtection` 的许可面 |
| **KDP** | 受保护数据当前对应的 **GPA/PFN** | 把特定物理帧标脏，但历史上只**周期性**校验 GVA→GPA 是否仍指向该 GPA |
| **HVPT** | **GVA→GPA 翻译本身** | Secure Kernel 管理分页结构，专门封堵 alias/remap 攻击；是**近年才补上**的层 |

关键历史：经典 KDP 的 remap 缺口（把受保护 VA 的 PTE 改指向新 GPA）在 HVPT 之前是真实存在的；HVPT 的存在正是因为这个缺口。参考：
- [KDP 设计](https://www.microsoft.com/en-us/security/blog/2020/07/08/introducing-kernel-data-protection-a-new-platform-security-technology-for-preventing-data-corruption/)
- [HVPT 设计](https://techcommunity.microsoft.com/blog/windowsosplatform/protecting-linear-address-translations-with-hypervisor-enforced-paging-translati/4399739)

## SecurityServicesRunning 的值不是 HVCI 的全部

- `2` = HVCI running
- `7` = HVPT running

`Win32_DeviceGuard.SecurityServicesRunning` 查到 `2` **不代表** HVPT 也在跑。要看列表里是否也包含 `7`。[状态值文档](https://learn.microsoft.com/en-us/windows/security/hardware-security/enable-virtualization-based-protection-of-code-integrity)

## 对"改 PFN 指向新页"这种 remap 思路的影响

| 场景 | 判定 |
|------|------|
| HVPT 在跑且覆盖目标 GVA | **结构性死** —— 页表翻译由 Secure Kernel 管，VTL0 改 PTE 没有合法通路 |
| 只有 HVCI/KDP、没有 HVPT | **技术上有缺口** —— 这是历史攻击面；但 remap 会破坏 Windows MM 内部状态，且 KDP 的周期校验可能发现 |
| HVCI 全关 | 不在本讨论范围 |

**所以不能一句"HVCI 页表只读"结论 remap 结构死**；要区分"HVCI/KDP only"与"HVPT 也在跑"两种状态。

## 实际项目路线上的后果

即便目标机只有 HVCI 没有 HVPT，remap 仍然不是稳定路线：
- 周期校验不是即时发现，但有发现窗口
- MM 内部状态（PFN DB、working set、ControlArea、引用计数、回写状态）需要和我们的 remap 保持一致，否则后续 trim/paging 操作会解引用错误的 PFN 并 BugCheck
- 这是未受支持的攻击面，不是产品级做法

**Why:** 之前（2026-10-18）讨论"让数据段 PAGED 然后接管 #PF"的通杀方案时，我（Claude）用"HVCI 页表只读"一句把 ③ 判死，是把三套机制混成一堵墙。见 [[claude-conflated-vbs-mechanisms]]。

**How to apply:**
- 讨论 VBS 内任何改 PTE / PFN / translation 的思路时，必须分开 HVCI/KDP/HVPT 三层
- `SecurityServicesRunning` 查到含 `2` 不等于 remap 不可行；要看是否含 `7`
- 查 HVPT 现场状态是 `Get-CimInstance Win32_DeviceGuard | select SecurityServicesRunning`；返回数组看 `7` 是否在列
- 即便 HVPT 不在跑，不要把 remap 当"可行方案"，当成"未受支持的历史攻击面"，并说明破坏 MM 一致性的具体代价
- 相关：[[ksword-vbs-no-coexistence]]（宏观 VBS 封死路径）、[[ksword-rxpf-hvci-write-barrier]]（image 节 CHANGE 行为）
