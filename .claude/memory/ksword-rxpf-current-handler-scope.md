---
name: ksword-rxpf-current-handler-scope
description: 现有 RXPF 代码的具体受理范围——只处理内核取指 NX fault，锁 PFN 做身份复核；任何"接管数据缺页"或"换 PFN"方案都不是当前代码能承载的
metadata:
  type: project
---

## 实际三道门（都在当前代码里）

### 门 1：fault class 过滤（只接内核取指 NX）

`KswRxpfExecuteFaultClass` 在 `KswordARKDriver/src/features/rxpf/page_fault_manager.c:662`：

```c
return (Frame->Cs & 3ULL) == 0ULL &&      /* CPL=0 */
    Cr2 == Frame->Rip &&                  /* 取指 fault（CR2 等于 RIP） */
    (error & 0x01ULL) != 0ULL &&          /* P=1 */
    (error & 0x02ULL) == 0ULL &&          /* W=0 */
    (error & 0x04ULL) == 0ULL &&          /* U=0 */
    (error & 0x08ULL) == 0ULL &&          /* RSVD=0 */
    (error & 0x10ULL) != 0ULL &&          /* I/D=1 */
    (error & ~0x1FULL) == 0ULL;           /* 无后续扩展位 */
```

**排除的 fault 类型全部走回 Windows：**
- P=0（not present / paged out / demand zero）
- W=1（数据写）
- U=1（用户态）
- CR2 ≠ RIP（数据读/写 fault）
- I/D=0（非取指，而是数据）

### 门 2：PFN 身份复核（锁死 remap）

`page_fault_manager.c:786`：

```c
physicalAddress = MmGetPhysicalAddress((PVOID)PageBase);
if ((physicalAddress.QuadPart >> PAGE_SHIFT) != record->Pfn || ...) {
    status = STATUS_REVISION_MISMATCH;
    LastFailureReason = KSWORD_ARK_RXPF_EMULATION_INVALID_ADDRESS;
    goto ManagedFailure;
}
```

Handler 在模拟前复核：**当前 VA 翻译到的物理帧必须等于注册时锁定的 PFN**。VA 被 remap 到新 PFN → 拒绝。

### 门 3：注册时 MmProbeAndLockPages

`image_protection_manager.c:892`：IoReadAccess 锁住当前 PFN，并把 PFN 存进 record。此后任何"让 PFN 变化"的操作都会被门 2 识别为非法。

## 对各种新方案的即时判定

| 方案 | 现在能跑吗 | 为什么 |
|------|---------|------|
| 自然 trim + 接管 #PF 填新页 | ❌ | 门 1 拒绝 P=0，fault 不会到我们这 |
| 自己清 P 位然后接管 | ❌ | 同上 + 门 2 发现 PFN 变化 |
| 修改数据段（数据读/写 fault） | ❌ | 门 1 要求 I/D=1 和 CR2==RIP，数据 fault 两条都不满足 |
| 接管数据写做 copy-on-write | ❌ | 需要重写门 1 的 fault class，还要加完整的数据访问虚拟化器（原子/LOCK/字符串/SIMD/跨页/并发语义） |
| 普通 .text inline hook（当前 write-only mode 已验） | 单独无用 | 字节能写但执行时触发的 NX fault 走不到我们这（页是 RO 而非 X）；要走到我们必须先重新把页标 X，但 VTL1 不允许 |

## 现在的 .text 执行模拟还要什么条件

现有 handler 设计意图是：目标 .text 页保留 X 权限、我们把 CPU 导到 NX fault（通过某种手段制造 I/D=1 的 fault），然后在 handler 里从 writable alias 读修改后字节解码模拟。

- Hyper-V 下影子 IDT 装不上，fault 根本不走我们 → 当前 write-only mode 的 WRITE 字节没有执行通路
- 物理机下 LIDT 行为未测
- 即使能装影子 IDT，还要让目标页进入可触发 I/D=1 的状态（NX 位要生效），这又是另一个条件

## CI.dll 本地 PE 节属性（本机 10.0.26100.8941）

- `.rdata = 0x48000040`：INITIALIZED_DATA + MEM_READ + **MEM_NOT_PAGED**
- `.data/CiPolicy = 0xC8000040`：INITIALIZED_DATA + MEM_READ + MEM_WRITE + **MEM_NOT_PAGED**
- Pageable 内容在 `PAGE/PAGEDATA/PAGECONS` 独立节

**结论**：g_CiOptions 所在的节天然 non-pageable，`MmPageEntireDriver` 对整幅驱动的编译属性生效但不能只选 CI.dll 的某一节，而且 Microsoft 文档明说不适用于**持续活动**的核心镜像生命周期。[分页规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/making-drivers-pageable)、[MmPageEntireDriver](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmpageentiredriver)

**Why:** 这些是用户 2026-10-18 分析"让数据段 PAGED + 接管 #PF"方案时提供的具体代码/文档读数，用来证明方案在当前 CI.dll 和当前 RXPF 代码双重约束下不成立。

**How to apply:**
- 任何"用 RXPF 接管 X 类型 fault"的提议，先回这三道门：fault class 是否放行、PFN 身份是否稳定、锁定前提是否成立
- 要扩展 fault class 必须明说"重写门 1 + 建数据访问模拟器"，这不是小改
- "让某页 pageable" 对 CI.dll 这类安全核心映像结构性不成立，`MmPageEntireDriver` 不是选项
- 相关：[[ksword-rxpf-hvci-write-barrier]]（image 节 CHANGE 行为）、[[ksword-rxpf-is-half-useless-without-shadow-idt]]（项目格局）、[[ksword-vbs-hvci-kdp-hvpt-distinction]]（remap 为什么不是简单结构死）
