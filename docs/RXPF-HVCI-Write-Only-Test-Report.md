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

RXPF write-only mode 在 HVCI 环境下完成端到端验证。该模式通过
`MmChangeImageProtection(PAGE_READONLY)` 令 VTL1 释放物理页上的 SLAT 写保护，
再通过 `MmProtectMdlSystemAddress(mdl, PAGE_READWRITE)` 将 VTL0 别名 PTE 升级为可写，
实现对内核映像 .text 节的写入，全程不依赖影子 IDT 或指令模拟。

### 关键结论

- **5 个内核模块的 .text 节确认可写**（CI.dll / FLTMGR.SYS / ndis.sys / tcpip.sys / NETIO.SYS）
- **WRITE IOCTL 三种负载全部成功**（4B NOP、5B JMP stub、64B 任意块）
- **HOOK IOCTL 配置路径正常**（LOG / REDIRECT / CLEAR）
- **UNREGISTER 正确恢复原始保护**
- ntoskrnl.exe 与 hal.dll 的 +0x1000 页实际落入只读数据节（origP=0x02），非 .text
- 影子 IDT 在 Hyper-V 下被正确阻止（CPUID "Microsoft Hv" 检测），write-only 模式不受影响

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

## 8. 应用场景

Write-only 模式在 HVCI 环境下可用于：

1. **DSE 绕过**：修改 CI.dll 中的 `g_CiOptions` 或代码完整性校验函数
2. **Inline Hook**：向 .text 节写入 JMP stub（5 字节 E9 相对跳转），重定向内核函数
3. **热补丁**：修改内核模块中的已知漏洞函数字节码
4. **安全研究**：在不关闭 HVCI 的前提下测试内核代码修改效果

已验证可修改的模块：CI.dll、FLTMGR.SYS、ndis.sys、tcpip.sys、NETIO.SYS。
ntoskrnl.exe 的 .text 需要精确的节偏移定位（非固定 +0x1000）。

---

## 9. 已知限制

1. **非原子写入**：WRITE IOCTL 最大 64 字节，大范围修改需多次调用
2. **无执行拦截**：影子 IDT 在 Hyper-V 下不可用，无法拦截/模拟被修改代码的执行
3. **单页粒度**：每次 REGISTER 针对一个 4KB 页面
4. **连续快速操作限制**：7+ 个模块的快速 CHANGE 循环可能导致 VTL1 SLAT 操作累积异常
5. **ntoskrnl/hal 需要精确偏移**：这两个模块的 .text 节起始偏移不在 +0x1000

---

*报告生成于 RXPF write-only mode HVCI 验证测试完成后。*
*所有测试均在 Hyper-V Gen2 VM（HVCI 已启用、testsigning=yes）上通过 PowerShell Direct IOCTL 完成。*
