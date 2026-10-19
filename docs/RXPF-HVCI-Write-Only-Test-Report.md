# RXPF Write-Only Mode — HVCI 实机验证报告

| 项目 | 值 |
|------|-----|
| 日期 | 2026-10-17 |
| 靶机 | KSword-HVM-Target (Hyper-V Gen2) |
| OS | Windows 11 22H2 (Build 22621) |
| HVCI | 已启用 |
| 驱动 | KswordARK.sys (testsigning) |
| Support flags | `0xFF` (INIT\|BUILD\|ABI\|ALLOC\|SELF\|IDT_SHADOW\|EMU\|EXT_IMG) |

---

## 1. 摘要

RXPF write-only mode 在 HVCI 环境下完成**写入路径**端到端验证。该模式通过
`MmChangeImageProtection(PAGE_READONLY)` 令 VTL1 释放物理页上的 SLAT 写保护，
再通过 `MmProtectMdlSystemAddress(mdl, PAGE_READWRITE)` 将 VTL0 别名 PTE 升级为可写，
实现对内核映像 .text 节字节的写入，全程不依赖影子 IDT 或指令模拟。

**重要边界：**本节验证的是"把字节写入物理页"的管道；写入后的字节**不可执行**
（原页 PTE 停在 PAGE_READONLY，VTL1 SLAT 保持 NX），因此单独使用 write-only mode
**无法达成 DSE 绕过、inline hook、热补丁等常见目标**——那些目标需要字节能被 CPU 执行
或被安全数据能被读取，两者都需要进一步条件（影子 IDT 或可写数据页），
而这两个条件都不被 HVCI 允许。详见 §8。

### 关键结论

- **5 个内核模块的 .text 节确认可写**（CI.dll / FLTMGR.SYS / ndis.sys / tcpip.sys / NETIO.SYS）
- **WRITE IOCTL 三种负载全部写入成功**（4B NOP、5B JMP stub、64B 任意块）；
  "写入成功"仅指字节出现在物理页上，不代表字节会被执行或代码行为改变
- **`.data`/`.rdata` 页第一步 CHANGE 就拿不到 RW 别名**（实测 CI.dll +0x30000/+0x38000/+0x3C000）；
  这意味着 `g_CiOptions` 等 CI 数据状态结构性不可写
- **HOOK IOCTL 配置路径正常**（LOG / REDIRECT / CLEAR）；实际触发需要影子 IDT，当前不可用
- **UNREGISTER 正确恢复 PTE 和别名**；但如果字节已改且未还原，VTL1 拒绝把页回到 PAGE_EXECUTE_READ
- ntoskrnl.exe 与 hal.dll 的 +0x1000 页实际落入只读数据节（origP=0x02），非 .text
- 影子 IDT 在 Hyper-V 下被 CPUID "Microsoft Hv" 检测阻止；物理机 VBS 下行为未验证

---

## 2. 测试方法

所有测试通过 PowerShell Direct（`New-PSSession -VMName`）从宿主向靶机发送 IOCTL。
使用 P/Invoke 直接调用 `DeviceIoControl`，绕过 GUI，确保结果可复现。

IOCTL 管道：

```
REGISTER(EXTERNAL_IMAGE, flags=UI_CONFIRMED|CAPTURE_BACKUP)
  → CHANGE(recordId)
    → WRITE(recordId, offset, bytes)
    → HOOK(recordId, address, action)
  → UNREGISTER(recordId)
```

协议约束：header version=1, size=实际结构大小, confirmationToken=0x46505852。

---

## 3. 多模块 .text 节覆盖测试

每个模块在 base+0x1000 处注册 EXTERNAL_IMAGE 页面，执行 CHANGE 后立即 UNREGISTER。

| 模块 | 基址 | origP | curP | aliasP | 结果 |
|------|------|-------|------|--------|------|
| CI.dll | 0xFFFFF8035D200000 | 0x20 (EXECUTE_READ) | 0x02 (READONLY) | 0x04 (READWRITE) | **WRITABLE** |
| ntoskrnl.exe | 0xFFFFF80144A00000 | 0x02 (READONLY) | 0x02 | 0x02 | RO-ONLY¹ |
| hal.dll | 0xFFFFF801401C0000 | 0x02 (READONLY) | 0x02 | 0x02 | RO-ONLY¹ |
| FLTMGR.SYS | 0xFFFFF801403D0000 | 0x20 (EXECUTE_READ) | 0x02 (READONLY) | 0x04 (READWRITE) | **WRITABLE** |
| ndis.sys | 0xFFFFF801466F0000 | 0x20 (EXECUTE_READ) | 0x02 (READONLY) | 0x04 (READWRITE) | **WRITABLE** |
| tcpip.sys | 0xFFFFF80146FD0000 | 0x20 (EXECUTE_READ) | 0x02 (READONLY) | 0x04 (READWRITE) | **WRITABLE** |
| NETIO.SYS | 0xFFFFF80146640000 | 0x20 (EXECUTE_READ) | 0x02 (READONLY) | 0x04 (READWRITE) | **WRITABLE** |

¹ 这两个模块的 +0x1000 页面原始保护为 PAGE_READONLY（0x02），说明该偏移落入 PE 头或只读数据节，
  而非 .text 节。ntoskrnl 的 .text 节起始偏移因构建版本而异，通常远高于 0x1000。
  MmChangeImageProtection 对已经是 READONLY 的页面不再改变 VTL1 SLAT，因此别名也只能获得 READONLY。

### 保护属性变换链

```
原始: PAGE_EXECUTE_READ (0x20) — VTL1 SLAT 标记为只读+可执行
  ↓ MmChangeImageProtection(PAGE_READONLY)
中间: PAGE_READONLY (0x02) — VTL1 释放该物理页的写保护
  ↓ MmProtectMdlSystemAddress(mdl, PAGE_READWRITE)
最终: PAGE_READWRITE (0x04) — 别名 PTE 可写，物理页可通过别名修改
```

### 稳定性说明

连续快速测试 7+ 个模块时，PowerShell Direct 会话会断开（"Hyper-V 套接字目标进程已结束"）。
该问题在两次独立测试中均在第 7 个模块后复现。原因可能是短时间内大量
MmChangeImageProtection 调用导致 VTL1 SLAT 操作累积。单模块操作完全稳定。

---

## 4. WRITE IOCTL 详细测试

在 CI.dll .text 页面（state=CHANGED, aliasP=PAGE_READWRITE）上执行三种写入。

### 4.1 NOP 写入（4 字节）

```
WRITE offset=0x200 length=4 payload=[90 90 90 90]
  ok=True  state=2  NT=0x00000000
  lastWriteOffset=512  lastWriteLength=4  lastWriteBytes=[90 90 90 90]
```

### 4.2 JMP Stub 写入（5 字节，模拟 inline hook）

```
WRITE offset=0x300 length=5 payload=[E9 00 10 00 00]  (jmp +0x1000)
  ok=True  state=2  NT=0x00000000
  lastWriteOffset=768  lastWriteLength=5  lastWriteBytes=[E9 00 10 00 00]
```

### 4.3 最大块写入（64 字节）

```
WRITE offset=0x400 length=64 payload=[XOR pattern: i ^ 0xAA]
  ok=True  state=2  NT=0x00000000
  lastWriteOffset=1024  lastWriteLength=64
```

**结论：** WRITE IOCTL 在 HVCI 下通过别名 PTE 成功写入内核 .text 页面。
写入的字节通过 PAGE_RESPONSE 的 lastWriteBytes 字段回显确认。
64 字节（MAX_WRITE_BYTES）的最大块写入正常。

---

## 5. HOOK IOCTL 测试

HOOK IOCTL 在已 CHANGE 的页面上设置钩点配置。钩点的实际触发需要影子 IDT（#PF 模拟），
在 Hyper-V 下该能力被阻止；本测试验证配置路径。

### 5.1 LOG 钩点

```
SET_HOOK_POINT action=LOG address=0xFFFFF8035D201200
  ok=True  action=2  state=2  hookAddress=0xFFFFF8035D201200  NT=0x00000000
```

### 5.2 REDIRECT 钩点

```
SET_HOOK_POINT action=REDIRECT address=0xFFFFF8035D201200 target=0xFFFFF8035D201500
  ok=True  action=1  target=0xFFFFF8035D201500  redirectCount=0  NT=0x00000000
```

### 5.3 清除钩点

```
SET_HOOK_POINT action=NONE
  ok=True  action=0  NT=0x00000000
```

**结论：** HOOK 三种动作（LOG / REDIRECT / NONE）的配置路径在 HVCI + write-only 模式下均正常。
redirectCount=0 符合预期（无影子 IDT 时钩点不会被触发）。

---

## 6. 完整管道验证

CI.dll .text 页面的完整生命周期：

```
REGISTER  → ok  rid=1  origP=0x20 (EXECUTE_READ)
CHANGE    → ok  curP=0x02  aliasP=0x04 (READWRITE)  alias=0xFFFFC981780CA000
WRITE×3   → ok  4B NOP + 5B JMP + 64B block 全部成功
HOOK×3    → ok  LOG + REDIRECT + CLEAR 全部成功
UNREGISTER→ ok  state=2  origP=0x20 (恢复)
```

---

## 7. Hyper-V 环境约束

| 能力 | 状态 | 原因 |
|------|------|------|
| Write-Only 模式 | **可用** | 不依赖影子 IDT |
| 影子 IDT (#PF 模拟) | 阻止 | CPUID 检测到 "Microsoft Hv"，LIDT 会被截获 |
| 自检 (Self-Test) | 阻止 | 依赖影子 IDT |
| HOOK 触发 | 阻止 | 依赖 #PF 模拟 |
| HOOK 配置 | **可用** | 仅设置内存中的结构 |
| EXTERNAL_IMAGE 注册 | **可用** | 跳过 AllocatedSelfTestPassed 门禁 |

---

## 8. 实际可行性评估（修正）

**本节早期草稿中列出了"DSE 绕过 / Inline Hook / 热补丁"等用例，这些声明错误。**
Write-only mode 在 HVCI 环境下**不满足**这些用例的实际需求，具体原因如下：

### 8.1 为什么这些用例都走不通

| 候选用例 | 为什么走不通 |
|---------|------------|
| **DSE 绕过** | `g_CiOptions` 在 CI.dll 的 .data 节；`MmChangeImageProtection` 只对原本 `PAGE_EXECUTE_READ` 的页释放 SLAT 写保护，.data/.rdata 页第一步 CHANGE 就拿不到 PAGE_READWRITE 别名（实测 CI+0x30000/0x38000/0x3C000 别名均停在 0x02）。CI 的所有数据状态同理。 |
| **Inline Hook** | 可以把 5 字节 JMP 写进 .text 页的物理帧，但原页 PTE 停在 PAGE_READONLY 且 VTL1 的 SLAT 保持 NX，CPU 执行到该字节时触发 #PF-NX。没有影子 IDT 做模拟就是普通的 NX 违例，走系统的 KiPageFault，一般结局是进程崩溃或 BSOD。 |
| **热补丁** | 同上，字节能写但不执行。 |
| **安全研究 / 对比修改前后 CPU 行为** | 单独使用 write-only mode 无法观察到任何执行层面的差异（新字节不执行），只能观察到物理页的字节内容本身。 |

### 8.2 实际能做的事情

| 能力 | 现实价值 |
|------|---------|
| 读取 .text 页的字节（通过原 VA 或别名） | 调试辅助，但 ReadProcessMemory / nt!MmCopyVirtualMemory 已能做到，无需走 RXPF |
| 把 .text 字节暂时改掉再还原 | 单看字节的"存在性"没有可利用方；没有执行就没有行为改变 |
| 后续如果能装影子 IDT，接 #PF 模拟 | 这才是 RXPF 原始设计目标，但当前被 Hyper-V 挡住 |

**结论：Write-only mode 本身是半成品，必须与影子 IDT 搭档才构成完整 RXPF；
当前 Hyper-V 下不可装影子 IDT（CPUID "Microsoft Hv" 检测拦下），
物理机 VBS 下 LIDT 是否被接受为唯一未解问题。**

---

## 9. 已知限制

1. **只能动 .text**：`MmChangeImageProtection` 只对原本 `PAGE_EXECUTE_READ` 的页释放 VTL1 SLAT 写保护，.data/.rdata/.rsrc 全部结构性不可写
2. **写入的字节不可执行**：原页 PTE 停在 PAGE_READONLY，CPU 执行到即 #PF-NX
3. **恢复可执行受 VTL1 校验**：UNREGISTER 调用 `MmChangeImageProtection(PAGE_EXECUTE_READ)` 时 VTL1 校验页内容，字节改过且未还原时拒绝
4. **非原子写入**：WRITE IOCTL 最大 64 字节，大范围修改需多次调用
5. **无执行拦截**：影子 IDT 在 Hyper-V 下不可用（CPUID 检测阻止），物理机 VBS 下行为未知
6. **单页粒度**：每次 REGISTER 针对一个 4KB 页面
7. **连续快速操作限制**：7+ 个模块的快速 CHANGE 循环可能导致 VTL1 SLAT 操作累积异常（实测两次独立复现）
8. **ntoskrnl/hal 需要精确偏移**：这两个模块的 .text 节起始偏移不在 +0x1000

---

*报告生成于 RXPF write-only mode HVCI 验证测试完成后。*
*所有测试均在 Hyper-V Gen2 VM（HVCI 已启用、testsigning=yes）上通过 PowerShell Direct IOCTL 完成。*
