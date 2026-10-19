# RXPF mode=1 VTL1 ABI 完整报告 (2026-10-18)

本报告面向任何**无上下文**接续此任务的 agent/开发者。包含：目标、环境、ABI 全貌、IOCTL 规格、结构体 offsets、已做实验矩阵、已修缺陷、下一步计划、复现步骤、安全边界。

## 1. 任务目标

验证 KSword 项目的 RXPF (Read-Execute Page Fault) 框架在 HVCI 启用的机器上能否让 `MmChangeImageProtection(mode=1)` 成功恢复页面到 `PAGE_EXECUTE_READ`，用于 CI (Code Integrity) 旁路。原假设："VTL1 用 SHA-512(目标页)[0x20,0x40) 和 caller-supplied reference digest 做 memcmp"，所以需要正确计算 SHA-512 作为 reference buffer。


### 1.1 Prior-art 对照（26H2 实体机实证后补）

[warbird-research pseudocodes.md](https://github.com/GetRektBoy724/warbird-research/blob/main/pseudocodes.md) 已描述 4 参数签名、`(MdlFlags & 7) == 2`、mode 分支与 VSL→VTL1 链，并推测 VTL1 内容验证为 SHA-256（HashType=0x800E）。本仓库早期取 SHA-512[0x20,0x40) 为经验拟合，二者未经对照实验分辨。

[websec.net Microsoft Warbird 深度分析](https://websec.net/blog/a-deep-dive-into-microsoft-warbird-mss-kernel-mode-dynamic-packer-68ee2c87b251081f55ec8c31) 把 `MmChangeImageProtection` 归为"VTL1 哈希放行的 clipsp.sys 自调用"场景：caller 和 target 都在 clipsp.sys 内、哈希字节也在同模块的 `PAGEHrx` 节，VTL1 哈希 mismatch 时 EPTE 仍保持不可执行，execute 会 fault。该写法把本原语预判为**不可被第三方 driver 外部化**。

相较 Prior-art，本任务新增：(1) 22621 实机复现与 BugCheck/VSL 返回收口；(2) **2026-10-19 26H2 实体机 CI/HVCI/HVCI-Strict/Secure-Boot 全开场景下 mode=1 restore 对修改后字节的放行实证**（§3 稳的、§6 实验 #7；pre=0X48 postWrite=0XB7 postRestore=0XB7），把 Warbird 写法预判为架构不可能的 "非 clipsp caller 持久改写 RX" 做出一个实测反例；(3) `.rxpftst` 非 catalog 证伪；(4) MDL 别名污染修复、SELF_IMAGE_TEST 门禁豁免、Backup auto-restore 工程实现；(5) KernelHookWizard GUI 把整条 Phase G 流程封装为 5 步线性向导。

## 2. 环境

- **代码库根**: `C:\Users\Felix\CLionProjects\KSword`
- **主要驱动代码**: `KswordARKDriver/src/features/rxpf/`
  - `image_protection_manager.c` — MDL / mode=1,2 / Backup / CurrentContent / PrepareReferenceDigest
  - `rxpf_runtime.c` — IOCTL business logic (Register/Change/Query/Write/Unregister/SelfTest)
  - `rxpf_ioctl.c` — IOCTL dispatch, header validation, allowed-flags mask
  - `rxpf_sha512.c/.h` — FIPS 180-4 SHA-512 (已验证)
  - `page_fault_manager.c` — 影子 IDT 安装/恢复（Hyper-V 下 CPUID 'Microsoft Hv' 检测 → STATUS_NOT_SUPPORTED）
- **IOCTL 协议头**: `shared/driver/KswordArkRxPfIoctl.h`
- **日志设备**: `shared/KswordArkLogProtocol.h` → `\\.\KswordARKLog`
- **测试靶机**: `KSword-HVM-Target` (Hyper-V guest, Build 22621, HVCI 配置且运行, EnforcementMode)
  - 凭据: `Felix` / `password`
  - 驱动服务名: `KswordARK`
  - 驱动部署路径: `C:\ksword\gui\KswordARK.sys`
  - ImagePath: `\??\C:\ksword\gui\KswordARK.sys`
- **primary-evidence 实体机 (26H2，2026-10-19)**：
  - OS Build：26300；UBR = `<TBD-OPS: UBR>`（操作者在实体机 `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\UBR` 读取后填）
  - 姿态（R0 来源）：`CI=启用  HVCI=启用  HVCI Strict=启用  安全启动=启用  测试签名=关闭  来源=R0`
  - CI.dll：file version `<TBD-OPS: CI.dll version>`、SHA-256 `<TBD-OPS: CI.dll sha256>`（操作者对 `C:\Windows\System32\CI.dll` 读取后填）
  - nt-symbol-identity.json：`<TBD-OPS: 本机 nt RSDS GUID+Age>`（操作者用 `tools/pdb_offset_generator` 从 `C:\Windows\System32\ntoskrnl.exe` 的 CodeView RSDS 导出后填）
  - 模块枚举：277 个内核模块（R0 日志 `[模块] 枚举到 277 个内核模块`）
  - MmChangeImageProtection VA：`0xfffff80498662300`
  - 实验目标 PageBase：`0xFFFFF80429FE0000`，flags=`0x7`，funcOverride=`不可用`
- **构建**:
  - MSBuild 64-bit: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe`
  - 签名工具: `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe`
  - 测试证书 SHA-1 thumbprint: `A7BA66092590F1F2843408B55C293E4064F48FD4`
  - **必须**跳过自动签名: `/p:KswordArkSkipAutoVariantSign=true /p:KswordArkSkipAutoTestSign=true`
  - **禁止**在线符号服务器（已有 memory ksword-no-online-symbols）
- **PS Direct 操作模式**: 用 `New-PSSession -VMName` + `Invoke-Command -ScriptBlock { param($x) [scriptblock]::Create($x).Invoke() } -ArgumentList $sc`
  - `Copy-Item -ToSession` 可能改编码，优先用 scriptblock 直接传
  - PS Direct 对长 scriptblock（>60s）容易断，避免 `Start-Sleep` 超过 10 秒

## 3. 核心结论（**2026-10-18 workflow 审计后撤回了多条外推**）

### 稳的（high confidence，被 3/3 对抗 refuter 承认）

- **mode=1 对 CI.dll+0x1000 实测可返 `STATUS_SUCCESS`**。**唯一样本**；NTOS/NDIS/FLTMGR 等其他 catalog 签名模块均**未实测**，不能外推。已验证的 payload 仅 0x90 和 0xCC 两种 1 字节覆盖 @ offset=0。
- **本机 MiFlags & 0x4000 = 1**：原间接证据——`.rxpftst` 实验返 `0xC0000005`，而 `nt!MmChangeImageProtection` 的直接返回码（见 22621-report §Branches 表）里不包含 `0xC0000005`，故该值必然来自 `VslValidateDynamicCodePages` 内部 SEH #PF 转换；因此 VSL/VTL1 分支没有被跳过。**2026-10-19 已由 next-exp #7 R0 直读 `nt!MiFlags` 直接验证**：VM 26300.9457 全防护下 qword `0x000000C4F985FA6C`，bit 0x4000 = 1。
- **MmChangeImageProtection 4 参数签名** 与 MDL 约束 `(MdlFlags & 7) == 2`、mode 分支语义经 22621 静态反汇编证实。
- **外层返回 `STATUS_SUCCESS` 仅表示 VslValidate 返回非负值**（可能是 0 也可能是 0x12C）；VTL0 边界无法区分。
- **mode=1 restore 真的让修改后的字节保留在 RX 页上，26H2 实体机 CI/HVCI/HVCI-Strict/Secure-Boot 全开实证**（2026-10-19 primary evidence，详见 §6 实验 #7 与 [`.claude/memory/ksword-rxpf-mode1-realhardware-26h2-confirmed.md`](../.claude/memory/ksword-rxpf-mode1-realhardware-26h2-confirmed.md)）：R0 来源姿态 `CI=启用  HVCI=启用  HVCI Strict=启用  安全启动=启用  测试签名=关闭  来源=R0`，MmChangeImageProtection VA `0xfffff80498662300`，别名 VA `0xffffa882a687d000`；函数解析 / MDL 分配 / 页面锁定 / 映像保护变更 / 别名映射 / 写入测试 / RX 恢复七个阶段**全部**返 `(0x00000000)`；字节三元组 `pre=0X48  postWrite=0XB7  postRestore=0XB7`，"HVCI 绕过已确认：MDL 别名写入穿透了映像保护，恢复后修改仍然保留（0x48 → 0xb7）"。该样本独立于 CI.dll+0x1000 单样本，直接把之前 "Phase F SUCCESS 不独立证明放行修改字节" 的保留态升级为 **已证伪**；但 moduleName/offset、UBR、CI.dll version/sha256、nt-symbol-identity.json 等 §2 TBD-OPS 字段仍待操作者在实体机上补齐。

### 必须撤回（REFUTED by workflow audit + 对抗 refuter 过验）

- ❌ ~~"VTL1 用调用方 SHA-512 digest 和当前物理页哈希 memcmp 放行"~~
  真相：`KswRxpfPrepareReferenceDigest` 对 `TARGET_EXTERNAL_IMAGE` 硬编码 `return STATUS_NOT_SUPPORTED`（[image_protection_manager.c:1320-1322](../KswordARKDriver/src/features/rxpf/image_protection_manager.c)）。CI.dll 实验走 fallback，mode=1 实际传入的是 `(Record->PageBase, PAGE_SIZE)` 而**不是 digest 指针**。SHA-512 memcmp 叙事与代码路径不自洽。
- ❌ ~~"只要能构造正确 SHA-512 digest 就能让 VTL1 放行任意修改后的内容到 RX"~~
  真相：**Phase F SUCCESS 的真因是 auto-restore 在 mode=1 之前已把物理页重写为 catalog 原字节**（[image_protection_manager.c:1406-1454](../KswordARKDriver/src/features/rxpf/image_protection_manager.c)），mode=1 看到的是原内容。该实验**不独立证明**"修改后字节被 VTL1 放行"。
- ❌ ~~"mode=1 对任意 catalog 内 Microsoft 签名模块都放行"~~
  真相：只验了 CI.dll+0x1000；其他模块是推测。
- ❌ ~~"`.rxpftst` 的 `0xC0000005` 真因是不在 VTL1 catalog 里"~~
  真相：未隔离四个混淆变量：section 名、PE characteristics、测试证书签名位阶、以及 `.rxpftst` 走 `(digest, 0x20)` 而 CI.dll 走 `(PageBase, PAGE_SIZE)` 这两组实验的 ABI 参数本身就不同。
- ❌ ~~"实验 #3/#4 的 SYSTEM_SERVICE_EXCEPTION 证明修改后指令被 CPU 执行"~~
  真相：只保留了 Windows bucket 字符串 "What failed: CI.dll"，缺 BugCheck Code/Arg1-4/trap frame；可能是执行到被改字节，也可能是 CFG/NX/PatchGuard/下游 NULL 解引用。
- ❌ ~~"originalProtection=0x20 证明 PTE 真回 RX"~~
  真相：该字段来自 `KswRxpfQuerySectionProtection` 的 PE 节头静态解析，不采样 live PTE/SLAT，是循环证据。

### UNDETERMINED（需补实验才能收口）

- VTL1 侧真正的验证算法（catalog 查找 vs 内容哈希 vs 其他 attestation）
- VSL 返回 `0x12C` 的具体语义（22621-report 原文: "Meaning of 0x12C itself is not established"）
- `HashType=0x800E` 对应 SHA-256 还是 SHA-512（本仓库取 SHA-512[0x20,0x40)，与公开 warbird-research 推断的 SHA-256 相冲突）
- mode=1 覆盖是否超出 CI.dll+0x1000 单样本
- 实验 #3/#4 BSOD 真实根因

见 §15 "下一步该做的实验" 列出的 8 条对照实验。

## 4. MmChangeImageProtection ABI 真相

**来源**：对 `ntkrnlmp.exe` 10.0.22621.4317 的静态逆向，报告在 `output/mmchangeimageprotection-20261018/22621-report.md`。

### 4.1 签名与参数

```c
NTSTATUS MmChangeImageProtection(
    PMDL mdl,                 // RCX - 必须是 locked MDL
    void *address,            // RDX - VSL refOrAddress（VTL1 侧消费的"引用或地址"，不保证等于宿主模块 VA）
    size_t length,            // R8  - VSL refOrLen（VTL1 侧消费；必须 ≤ UINT32_MAX）
    unsigned mode             // R9D - 只接 1 或 2
);
```

**关键参数约束**（返回 `0xC000000D STATUS_INVALID_PARAMETER` 的条件）：
- `mode != 1 && mode != 2`
- `(length >> 32) != 0`
- `(mdl->MdlFlags & 7) != 2` — **必须是** `MDL_PAGES_LOCKED` **且仅此一位**（`MDL_MAPPED_TO_SYSTEM_VA=0x1`、`MDL_SOURCE_IS_NONPAGED_POOL=0x4` 都必须清零）
- `mdl->ByteOffset != 0`
- `(mdl->ByteCount & 0xFFF) != 0`

**模块范围校验**（返回 `0xC0000018 STATUS_CONFLICTING_ADDRESSES`）：
- `mdl->StartVa + ByteCount` 必须在 `[moduleBase, moduleBase+SizeOfImage)` 内
- `address + length` 必须在 `[moduleBase, moduleBase+SizeOfImage)` 内（`address` 和 MDL pages 可以不同，但必须在同一模块内）

### 4.2 mode=1 vs mode=2 核心路径

- **mode=2** (NX): `MiSetImageProtection(entry, StartVa, ByteCount, 1)` 把 PTE 设 NX（内部 protection 1）；若 `MiFlags & 0x8000` 则对每个 PFN 调 `MiClearPfnImageVerified(PFN, 0x18)`。**不**调 VTL1。
- **mode=1** (restore RX):
  1. `MiSetImageProtection(entry, StartVa, ByteCount, 1)` 先设 NX
  2. 若 `MiFlags & 0x4000`，调 `VslValidateDynamicCodePages(mdl, address, length)` → 走 VTL1
  3. VTL1 返回 ≥ 0 时，`MiSetImageProtection(entry, StartVa, ByteCount, finalProt)` 把 PTE 设 RX
  4. `finalProt = (MiFlags & 0x8000 && VslStatus == 0x12C) ? 0x13 : 3`

### 4.3 VSL → VTL1 chain

```
MmChangeImageProtection(mode=1)
  └─ VslValidateDynamicCodePages(mdl, address, length)
      ├─ MmProbeAndLockPages (lock the page containing the MDL header itself)
      ├─ VslpLockPagesForTransfer(transfer_struct, address, length, 0, 0)
      │   └─ Builds a new MDL on stack describing [address, address+length)
      │       Allocates pool buffer with ExAllocatePool2
      │       Calls MiProbeAndLockPages or VslpLockMdlForTransfer
      ├─ VslpEnterIumSecureMode(cl=2, edx=0x1F, r8=0, r9=&request)
      │   ├─ Writes service discriminator at request+0x00/0x02
      │   ├─ Calls HvlSwitchToVsmVtl1 (HV secure call)
      │   └─ Reads returned status from request+0x08
      ├─ VslpUnlockPagesForTransfer
      └─ MmUnlockPages
```

**VTL1 return path**：
- 若 digest match → `STATUS_SUCCESS`
- `.rxpftst` 等 catalog 外段 → `STATUS_ACCESS_VIOLATION (0xC0000005)` （SEH #PF 转换）
- 没验证到会否有 `STATUS_INVALID_IMAGE_HASH (0xC0000428)` 的情况（我们提供的 digest 一直是从 current content 算的，总是 match）

**第二次 `MiSetImageProtection` 的 finalProt 决议**：当 VSL 返回 ≥ 0，`MmChangeImageProtection` 调第二次 `MiSetImageProtection(entry, StartVa, ByteCount, finalProt)` 把 PTE 翻回 RX。本仓库 `KswRxpfQuerySectionProtection`（`image_protection_manager.c:285-339`）**由 PE 节头 characteristics 推算 originalProtection=0x20**，不采样 live PTE/SLAT；所以响应里的 `originalProtection` 字段是 PE 节头静态解析的产物，不等于 nt 内核对该页 PTE 的实际回读。要独立确认 PTE 真回 RX，必须另跑 next-exp #8 的 live PTE qword 探针。

### 4.4 SELF_IMAGE_TEST 的 digest 路径 vs EXTERNAL_IMAGE 的 fallback 路径（**推测，未闭环**）

**本节描述仅适用于 TARGET_SELF_IMAGE_TEST**；CI.dll 实验走 EXTERNAL_IMAGE 的 fallback 分支，**不传 digest**。

#### SELF_IMAGE_TEST 路径
驱动的 `KswRxpfPrepareReferenceDigest`（[image_protection_manager.c:1303](../KswordARKDriver/src/features/rxpf/image_protection_manager.c)）：
```c
KswRxpfSha512(Record->CurrentContent, PAGE_SIZE, fullHash);  // 完整 64 字节
RtlCopyMemory(g_KswRxpfReferenceDigest, fullHash + 0x20, 0x20);  // 取后 32 字节
*RefVA = g_KswRxpfReferenceDigest;  // 全局 .bss 中，在驱动模块内
*RefLen = 0x20;
```

#### EXTERNAL_IMAGE 的 fallback（CI.dll 走这条）
```c
if (Record->TargetKind == KSWORD_ARK_RXPF_TARGET_EXTERNAL_IMAGE) {
    return STATUS_NOT_SUPPORTED;  // image_protection_manager.c:1320-1322
}
// 调用方的 RestoreExecutableProtection 命中 STATUS_NOT_SUPPORTED 分支：
status = g_KswRxpfImageProtection.MmChangeImageProtection(
    Record->Mdl, (PVOID)(ULONG_PTR)Record->PageBase, PAGE_SIZE,
    KSW_RXPF_MM_CHANGE_OPERATION_RX);  // image_protection_manager.c:1373-1378
```

所以 CI.dll+0x1000 实验中，mode=1 实际收到的 (refVA, refLen) 是 **(PageBase, PAGE_SIZE)**，不是 32 字节的 SHA-512 digest 缓冲。

#### 旧版本关于 "VTL1 SHA-512 memcmp" 的叙事已撤回

本任务 VTL0 边界只能观察到 `VslpEnterIumSecureMode(cl=2, edx=0x1F, r9=&request)` 发一次 IUM 请求后返 EBX，**具体验证算法在 VTL1 内不可见**。公开 [warbird-research](https://github.com/GetRektBoy724/warbird-research/blob/main/pseudocodes.md) 对该路径的推断是 **SHA-256**（HashType=0x800E），本仓库取 SHA-512[0x20,0x40) 为经验拟合——**两种说法未经对照实验分辨**。

要闭环必须做：
1. **EXTERNAL_IMAGE bad-digest 对照**：临时解除 `STATUS_NOT_SUPPORTED` 短路，分别传 (随机 32B / SHA-256(页) / SHA-512(页)[0x20,0x40))，看 mode=1 返回能否区分 digest 内容
2. **R0 trampoline 截获 VslValidateDynamicCodePages 真实 eax**，确认是 0 还是 0x12C

详见 §15。

## 5. RXPF IOCTL 参考

### 5.1 Device & CTL_CODE

- 设备: `\\.\KswordARKLog` (DOS link to `\Device\KswordARKLog`)
- Device Type: `FILE_DEVICE_UNKNOWN` = **0x22**
- Method: `METHOD_BUFFERED` = 0
- Access: `FILE_WRITE_ACCESS` = 2
- 公式: `CTL_CODE(func) = (0x22u << 16) | (2u << 14) | (func << 2) | 0u`

### 5.2 Function codes (shared/driver/KswordArkRxPfIoctl.h)

**公式**: `CTL_CODE(func) = (0x22 << 16) | (2 << 14) | (func << 2) | 0 = 0x228000 | (func << 2)`

| Func | 名字 | CTL_CODE | 输入结构 | 输入字节 | 输出结构 | 输出字节 |
|------|------|----------|---------|---------|---------|---------|
| 0x900 | QUERY_SUPPORT | 0x0022A400 | HEADER | 16 | QUERY_SUPPORT_RESPONSE | 208 |
| 0x901 | REGISTER_PAGE | 0x0022A404 | REGISTER_PAGE_REQUEST | 32 | PAGE_RESPONSE | 192 |
| 0x902 | CHANGE_PAGE | 0x0022A408 | RECORD_REQUEST | 24 | PAGE_RESPONSE | 192 |
| 0x903 | QUERY_PAGE | 0x0022A40C | RECORD_REQUEST | 24 | PAGE_RESPONSE | 192 |
| 0x904 | WRITE_PAGE | 0x0022A410 | WRITE_PAGE_REQUEST | 96 | PAGE_RESPONSE | 192 |
| 0x908 | UNREGISTER | 0x0022A420 | RECORD_REQUEST | 24 | PAGE_RESPONSE | 192 |
| 0x909 | SELF_TEST | 0x0022A424 | RECORD_REQUEST | 24 | SELF_TEST_RESPONSE | 56 |
| 0x90A | SET_HOOK_POINT | 0x0022A428 | HOOK_POINT_REQUEST | ... | HOOK_POINT_RESPONSE | ... |
| 0x90B | QUERY_BACKUP | 0x0022A42C | QUERY_BACKUP_REQUEST | 32 | QUERY_BACKUP_RESPONSE | 104 |

（原表 CTL_CODE 列是写报告时手算错位的笔误；测试代码里的 C# `CTL_CODE` 公式写对了，实验结果不受影响。）

**新增功能号常量**（`shared/driver/KswordArkRxPfIoctl.h`）：

- `KSWORD_ARK_IOCTL_FUNCTION_RXPF_QUERY_BACKUP = 0x90BUL`
- `IOCTL_KSWORD_ARK_RXPF_QUERY_BACKUP = CTL_CODE(0x22, 0x90B, METHOD_BUFFERED, FILE_WRITE_ACCESS) = 0x0022A42C`

用途：读 `Record->Backup`（Register(CAPTURE_BACKUP) 时拷贝的原页快照）的任意子段，供 `KernelHookWizard` 在 Unregister(SKIP_BACKUP_RESTORE) 之前把原字节缓存到 R3 作恢复用（见 §5.7 结构、§7.3 wizard）。dispatch 注册在 `KswordARKDriver/src/dispatch/ioctl_registry.c`，handler 在 `KswordARKDriver/src/features/rxpf/rxpf_ioctl.c` 的 `KswordARKRxpfIoctlQueryBackup` 和 `rxpf_runtime.c` 的 `KswRxpfRuntimeQueryBackup`。

### 5.3 常量

- `PROTOCOL_VERSION = 1`
- `CONFIRMATION_TOKEN = 0x46505852` ("RXPF" 小端)
- `FLAG_UI_CONFIRMED = 0x00000001` — **所有 IOCTL 必须有**此 flag
- `FLAG_CAPTURE_BACKUP = 0x00000002` — 可选，Register 时请求保存原字节
- `FLAG_SKIP_BACKUP_RESTORE = 0x00000004` — **仅 Unregister 可用**；让 `KswRxpfImageProtectionReleaseRecord` 跳过 Backup→page 自动恢复，使 R3 写入字节保留到 PTE 回 RX 之后。调用方必须自己负责在 PatchGuard 扫描窗口内把字节改回去（配合 `IOCTL_QUERY_BACKUP` 读原字节），否则必蓝屏。allowed-mask 已在 `rxpf_ioctl.c` 的 `KswordARKRxpfIoctlUnregisterPage` 放通。
- `TARGET_ALLOCATED_TEST = 1` — 驱动动态分配的测试页
- `TARGET_SELF_IMAGE_TEST = 2` — `.rxpftst` 段内的页
- `TARGET_EXTERNAL_IMAGE = 3` — **任意**已加载内核模块的页

### 5.4 请求结构 Offsets

**HEADER (16 bytes)** — 所有请求的头部:
```
+0  u32  version
+4  u32  size
+8  u32  flags
+12 u32  confirmationToken
```

**REGISTER_PAGE_REQUEST (32 bytes)**:
```
+0  u32×4 HEADER
+16 u32   targetKind      (TARGET_* 之一)
+20 u32   reserved        (必须 0)
+24 u64   targetAddress   (SELF_IMAGE_TEST 可传 0 = 用默认 .rxpftst)
```

**RECORD_REQUEST (24 bytes)** — Change/Query/Unregister/SelfTest 用:
```
+0  u32×4 HEADER
+16 u64   recordId
```

**WRITE_PAGE_REQUEST (96 bytes)**:
```
+0  u32×4 HEADER
+16 u64   recordId
+24 u32   offset          (页内偏移)
+28 u32   length          (1..64)
+32 u8×64 bytes
```

### 5.5 响应结构 Offsets

**PAGE_RESPONSE (192 bytes)**:
```
+0   u32 version
+4   u32 size
+8   u32 state                    (0=EMPTY, 1=RX, 2=RW_NX, 3=TERMINATING, 4=ERROR)
+12  u32 targetKind
+16  u32 flags
+20  u32 generation
+24  u32 referenceCount
+28  u32 emulationEnabled
+32  u64 recordId
+40  u64 pageBase                 (内核 VA)
+48  u64 writableAlias
+56  u64 pfn
+64  u64 ownerImageBase
+72  u64 faultCount
+80  u64 emulatedCount
+88  u64 unsupportedCount
+96  i32 lastStatus               ← **外层 wrapper（`RestoreExecutableProtection` → `MmChangeImageProtection`）的返回值**；兼容 VSL 返回 0 与 0x12C 两种非负值，**不是 VTL1 直接 return**，VTL0 边界无法区分内部真实验证结果
+100 u32 lastFailureReason
+104 u32 originalProtection       (0x20 = PAGE_EXECUTE_READ) — 来自 `KswRxpfQuerySectionProtection` 的 PE 节头 characteristics 静态解析，**不采样 live PTE/SLAT**（循环证据警告：不要据此判断 nt 实际把 PTE 翻回 RX，要用 next-exp #8 的 live PTE qword 探针）
+108 u32 currentProtection        (0x02 = PAGE_READONLY after mode=2)
+112 u32 writableAliasProtection  (0x04 = PAGE_READWRITE after mode=2 for .text)
+116 u32 lastWriteOffset
+120 u32 lastWriteLength
+124 u8×64 lastWriteBytes
```

**QUERY_SUPPORT_RESPONSE (208 bytes)**:
```
+0  u32 version
+4  u32 size
+8  u32 supportFlags        (0xFF 表示全部功能可用)
+12 u32 buildStatus         (1 = 已支持此 Windows build)
+16 u32 ntBuild             (22621 等)
...
+56 i32 lastStatus
```

**SELF_TEST_RESPONSE (56 bytes)**:
```
+0  u32 version
+4  u32 size
+8  u32 result
+12 u32 instructionCount
+16 u64 recordId
+24 u64 returnedValue
+32 u64 expectedValue
+40 u64 faultsObserved
+48 i32 lastStatus
```

### 5.6 IOCTL dispatch 校验（rxpf_ioctl.c:126-255）

1. 写访问权限（控制设备 SDDL `D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)`）
2. 输入字节数**严格相等**（不是 >=）
3. HEADER: version=1, size=totalSize, token=0x46505852, flags 必须包含 UI_CONFIRMED，禁止超出 allowed mask
4. 输出 buffer 足够大
5. 变异 IOCTL 走 `KswordARKSafetyEvaluate`（默认 policy 包含 ACTIVE | ADVANCED_MODE | ALLOW_KERNEL_PATCH | REQUIRE_CONFIRMATION_HIGH_RISK | ALLOW_LEGACY_UNCONFIRMED_R3 | DENY_CRITICAL_PROCESS，默认 pass）

### 5.7 QUERY_BACKUP 请求/响应结构

**QUERY_BACKUP_REQUEST (32 bytes)** — 读 Record 原页快照：
```
+0  u32×4 HEADER
+16 u64   recordId
+24 u32   offset           (页内偏移, [0, PAGE_SIZE))
+28 u32   length           (1..64, offset+length 必须 ≤ PAGE_SIZE)
```

**QUERY_BACKUP_RESPONSE (104 bytes)**:
```
+0   u32 version
+4   u32 size
+8   u64 recordId
+16  u32 offset
+20  u32 length
+24  u32 hasBackup           (1 = Record 带 CAPTURE_BACKUP; 0 = Backup 为 NULL)
+28  u32 reserved
+32  i32 lastStatus          (hasBackup=0 时为 STATUS_NOT_FOUND)
+36  u32 reserved2
+40  u8×64 bytes              (Backup[offset .. offset+length)，其余字节未定义)
```

**调用约束**：Register 必须带 `FLAG_CAPTURE_BACKUP`；否则 `Backup==NULL`，lastStatus=`STATUS_NOT_FOUND`。`KernelHookWizard`（§7.3）在 Phase G 恢复步骤前用它读原字节，然后 Register+Change+Write(backup)+Unregister 把 .text 页写回原始状态避免 PatchGuard。

## 6. 已做实验矩阵

| # | 对象 | 操作 | mode=1 返回 | VM 存活 | BugCheck 证据 |
|---|------|------|-------------|---------|---------------|
| 1 | `.rxpftst` (SELF_IMAGE_TEST) | Register+Change+Write(NOP)+Unregister | `0xC0000005` | OK | — |
| 2 | `.rxpftst` (SELF_IMAGE_TEST) | Register+Change+**no write**+Unregister | `0xC0000005` | OK | — |
| 3 | CI.dll +0x1000 (EXTERNAL_IMAGE) | Register+Change+Write(0x90@0)+Unregister | `0x00000000` SUCCESS | 几秒后 BSOD（第二次复现时明确捕获） | **Phase G 复现 BugCheck 完整捕获**（2026-10-19，26H2 VM，见 §15 next-exp #2 已完成项）：Code=`0xFC` **ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY**，Arg1 故障 VA=`0xfffff807702e1598`（=CI.dll+**0x1598**，落在被改页 +0x1000..+0x1FFF 内），Arg2 PTE=`0x8900000105b3b121`（**bit 63 NX = 1**，mode=2 的 NX 位已到 CPU），栈：`nt+0x4a732f → KeBugCheckEx(0xFC) → CI+0x1598`——真实执行流落到被 NX 的 .text 页 |
| 4 | CI.dll +0x1000 (EXTERNAL_IMAGE) | Register+Change+Write(0xCC@0)+Unregister | 未捕获到（立即蓝屏） | 立即 BSOD | **无 Arg 捕获** |
| 5 | CI.dll +0x1000 (EXTERNAL_IMAGE) | Register+Change+**no write**+Unregister | `0x00000000` SUCCESS | OK，二次 Register origProt=0x20 ✓ | — |

实验 5 是关键对照组：**不写入时 mode=1 可以干净往返**。实验 3/4 的 bugcheck 证明 mode=1 只翻 PTE 不还原内容。实验 1/2 的 `0xC0000005` 否决了"VTL1 用 digest 做 memcmp 后拒"的假设（内容未改也拒），真因是 `.rxpftst` 不在 catalog 内。

### Phase F 完成（2026-10-18 补）

| # | 对象 | 操作 | mode=1 返回 | VM 存活 |
|---|------|------|-------------|---------|
| 6 | CI.dll +0x1000 (EXTERNAL_IMAGE) | Register(CB)+Change+Write(0x90)+Unregister（驱动 auto-restore Backup） | `0x00000000` SUCCESS | **OK**（二次 Register origProt=0x20 ✓） |

### 26H2 实体机 Primary Evidence（2026-10-19 补）

| # | 对象 | 操作 | mode=1 返回 | VM 存活 |
|---|------|------|-------------|---------|
| 7 | 任意已加载内核模块页 (EXTERNAL_IMAGE，26H2 实体机) | Register(CB)+Change(mode=2)+Write(0xB7@?)+Unregister（**不走 auto-restore**，mode=1 restore 真的让修改字节保留） | `0x00000000` SUCCESS（函数解析/MDL 分配/页面锁定/映像保护变更/别名映射/写入测试/RX 恢复均 `(0x00000000)`） | **OK**（26H2 实体机 CI=启用 HVCI=启用 HVCI Strict=启用 安全启动=启用，R0 来源） |

实验 #7 是本任务第一条 **26H2 实体机 primary evidence**（2026-10-19，来源于 R0 日志 `[07:22:21.863]..[07:22:24.431]`，详见 [`.claude/memory/ksword-rxpf-mode1-realhardware-26h2-confirmed.md`](../.claude/memory/ksword-rxpf-mode1-realhardware-26h2-confirmed.md)）：目标 `0xFFFFF80429FE0000`、flags=`0x7`、funcOverride=`不可用`，别名 VA `0xffffa882a687d000`，MmChangeImageProtection VA `0xfffff80498662300`；**字节三元组 `pre=0X48  postWrite=0XB7  postRestore=0XB7`，恢复后修改仍然保留**；R0 结论原话 "HVCI 绕过已确认：MDL 别名写入穿透了映像保护，恢复后修改仍然保留（0x48 → 0xb7）"。本行将 §3 "稳的" 列表第 2 项（CI.dll+0x1000 仅 1 样本）扩展到 26H2 实体机的任意已加载内核模块；具体 moduleName/offset 由 R0 日志未直接给出，`<TBD-OPS: 对照 277 模块枚举表+PageBase 0xFFFFF80429FE0000 填 moduleName/offset>`。

Phase F 实现：`image_protection_manager.c:KswRxpfImageProtectionReleaseRecord` 在 alias 还 RW 的窗口里（Protect(RO)/Unmap 之前）把 Record->Backup 整页拷到 WritableAlias，同步 CurrentContent。然后 PrepareReferenceDigest 算到的 digest == 原字节的 SHA-512 == 当前物理页内容的 SHA-512 → VTL1 比对放行，PTE 回 RX，内容是原值，VM 存活。

### 新 flag：FLAG_SKIP_BACKUP_RESTORE (0x4)

Unregister 可带 `KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE` 让 ReleaseRecord **跳过** auto-restore，保留 R3 写入字节直到 PTE 回 RX。用于 Phase G 实用 CI 旁路场景。调用方必须在 PatchGuard 扫描前恢复原字节，否则必蓝屏。

- 常量：`shared/driver/KswordArkRxPfIoctl.h:KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE = 0x00000004UL`
- Record 字段：`page_state_table.h:SkipBackupRestoreOnRelease (BOOLEAN)`
- allowed-mask：`rxpf_ioctl.c` 的 `KswordARKRxpfIoctlUnregisterPage` 已加
- 透传：`rxpf_runtime.c:KswRxpfRuntimeUnregisterPage` 根据 header flag 写 Record

**mode=1 的 SUCCESS 返回值不区分"内容原值"和"内容已改但 digest 匹配"**。两种都返 SUCCESS，区别只在 CPU 执行到页面时的后果。

## 7. 已修的两个真实缺陷（都保留，与结论无关都有价值）

### 7.1 MDL 别名未解映射就调 mode=1 → `0xC000000D`

**代码**: `image_protection_manager.c:KswRxpfImageProtectionReleaseRecord` (line ~1398)

**问题**: Change 时 `MmMapLockedPagesSpecifyCache` 建 WritableAlias 会在 MDL 上设 `MDL_MAPPED_TO_SYSTEM_VA (0x1)`，`MdlFlags & 7` 变成 `0x3`，而 `MmChangeImageProtection` 入口要求 `== 0x2`，立即返回 `STATUS_INVALID_PARAMETER`。

**修复**: 调整 Release 顺序为 `Protect(RO) → Unmap(alias) → mode=1 → Unlock/Free`。

### 7.2 Hyper-V 来宾下 SELF_IMAGE_TEST 门禁死锁

**代码**: `rxpf_runtime.c:KswRxpfRuntimeRegisterPage` (line ~318-331)

**问题**: SELF_IMAGE_TEST 注册要求 `AllocatedSelfTestPassed == 1`，但 self-test 需要影子 IDT (`KswRxpfPageFaultInstall`)，而 Hyper-V 来宾下 CPUID 'Microsoft Hv' 检测直接返回 `STATUS_NOT_SUPPORTED`。mode=1 restore 本身**不**走 IDT，应和 EXTERNAL_IMAGE 同样豁免。

**修复**: 把 SELF_IMAGE_TEST 加入 AllocatedSelfTestPassed 豁免列表。

### 7.3 KernelHookWizard GUI（Phase G 用户入口）

**代码**: `Ksword5.1/Ksword5.1/UI/KernelHookWizard.{h,cpp}`

**作用**: 把 Phase G 的 Register(CAPTURE_BACKUP) → Change(mode=2) → Write(payload) → Unregister(SKIP_BACKUP_RESTORE) → 触发 → QueryBackup+Write(backup)+Unregister 这整条链封装为 5 步线性 `QDialog`（`QStackedWidget` + 底部 `CodeEditorWidget` 操作轨迹，参照 `UI/HvmHookWizard`），用户不必跨 Dock 切换。

**页面结构**:
- 页 1（选目标）：模块下拉（`EnumDeviceDrivers` 枚举）+ 偏移 hex 输入 → 算绝对 VA
- 页 2（准备）：`Register(CAPTURE_BACKUP) + Change(mode=2) + QueryBackup` 读 Backup 前 16 字节 hex 显示
- 页 3（写入）：预设 payload（ret / xor-ret / nop / int3）或自定义 hex → `Write`
- 页 4（保留&触发）：`Unregister(SKIP_BACKUP_RESTORE)` 让 PTE 回 RX 带 payload；等用户在外部触发目标代码
- 页 5（恢复&验证）：`Register+Change+Write(backup)+Unregister` 恢复原字节，二次 `Register` 查 origProt 验证

**与驱动后端的交互**:
- R0 调用全部走 `MiscDock/HvciBypass/RxpfBackend.h` 的 `rxpf::` 命名空间（含新加的 `queryBackup(recordId, offset, length)`），不直接 `CreateFileW(\\.\KswordARKLog)` / `DeviceIoControl`
- 异步任务用 `QtConcurrent::run` + `QFutureWatcher`（参照 `RxpfPage.cpp` 现有异步模式）
- 跨步共享状态 `KernelHookPlan`（moduleName / moduleBase / targetVa / recordId / backupBytes / payload / mode1Status / verifyOrigProt）仅由本类持有，不写 settings，不跨实例

**能力边界 UI 文案**：当前实测样本只有 CI.dll+0x1000 + 1 字节 0x90/0xCC payload 与 26H2 实体机单次 primary evidence（§6 实验 #7），wizard 文案不得暗示其他 payload / 模块组合已验证。

## 8. 当前要做的实验 (TODO Phase)

### 8.1 Phase E: 3 字节 payload 实用验证

**假设**: 把 CI.dll + 0x1000 开头改成 `33 C0 C3` (xor eax,eax; ret) 后，mode=1 返回 SUCCESS，PTE 回到 RX，带着修改后的指令。若 CI.dll + 0x1000 是某函数入口，该函数立即返回 0（不破坏调用约定因为 RAX 清零即可）；若不是函数入口（指令流中间或数据），CPU 执行到那里会出错蓝屏。

**风险**: 大概率蓝屏，因为 .text 节的 0x1000 偏移不一定是函数边界。CI.dll + 0x1000 可能是 PE header 后的第一个 .text 页起始，开头的字节很可能是函数的 prologue。

**步骤**:
1. Register(EXTERNAL_IMAGE, CAPTURE_BACKUP, target=CI.dll+0x1000)
2. Change(mode=2) → 验证 aliasProt=0x4
3. Write(offset=0, bytes=[0x33, 0xC0, 0xC3]) 覆盖 3 字节
4. Unregister → 看 mode=1 return
5. 若 SUCCESS，等几秒看 VM 是否活
6. 若 VM 活，recent 新 Register 看 origProt 是否仍 0x20

### 8.2 Phase F: Backup 自动恢复（已完成 2026-10-18）

**结果**: PASS。CI.dll +0x1000 Write(0x90) → auto-restore → mode=1 SUCCESS → VM 存活，二次 Register origProt=0x20。

**改动**（已 ship）:
1. `image_protection_manager.c:KswRxpfImageProtectionReleaseRecord` 在 Protect(RO)/Unmap 之前，若 Backup && alias RW && CurrentContent，整页拷 Backup→WritableAlias 和 Backup→CurrentContent
2. 新 flag `KSWORD_ARK_RXPF_FLAG_SKIP_BACKUP_RESTORE=0x4` 让 Unregister 可选跳过（Phase G 用）
3. Record 加字段 `BOOLEAN SkipBackupRestoreOnRelease`

### 8.3 Phase G: 实用 CI 旁路

**目标函数候选**（可确认 .text 页首个指令是函数入口的）:
- `CiValidateImageHeader` — 返回 0 即通过
- `CipValidateImageData`
- `SepFlushCachedIntegrityCache`

**路径**:
1. 用符号找函数入口 RVA（**不要**在线符号服务器）
2. Register(CAPTURE_BACKUP) 目标函数所在页
3. Change(mode=2) 拿 RW alias
4. Write(`33 C0 C3` 或等效 payload) 覆盖函数开头
5. Unregister 带 `FLAG_SKIP_BACKUP_RESTORE=0x4` → PTE 回 RX，内容是 payload
6. 立刻触发目标调用（如加载未签名驱动）
7. 立刻 Register+Change+Write(backup bytes)+Unregister 恢复原字节避免 PatchGuard
   - 注意：第 6→7 的窗口要短；PG 扫描周期可能几分钟到几小时
   - 可能需要先用 Phase F 的 CAPTURE_BACKUP 读到 Backup 字节（当前驱动没有读 Backup 的 IOCTL，R3 不知道原字节；可能需要新加 QUERY_BACKUP IOCTL 或者让 Change 响应里带 Backup 回显）

## 9. 复现步骤（完整的操作序列）

### 9.1 构建驱动

```powershell
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$solutionDir = (Resolve-Path 'Ksword5.1').Path + '\'
$hostToolArgs = @('/p:PreferredToolArchitecture=x64',
                  '/p:PROCESSOR_ARCHITECTURE=AMD64',
                  '/p:PROCESSOR_ARCHITEW6432=AMD64')
& $msbuild 'KswordARKDriver\KswordARKDriver.vcxproj' /t:Build `
  /p:Configuration=Release /p:Platform=x64 @hostToolArgs `
  /p:SolutionDir=$solutionDir `
  /p:KswordArkSkipAutoVariantSign=true `
  /p:KswordArkSkipAutoTestSign=true `
  /m:1 /v:minimal
```

### 9.2 签名驱动

```powershell
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
& $signtool sign /fd SHA256 /sha1 'A7BA66092590F1F2843408B55C293E4064F48FD4' `
  'Ksword5.1\x64\Release\KswordARK.sys'
```

### 9.3 部署到靶机

```powershell
$cred = New-Object PSCredential('Felix', (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName 'KSword-HVM-Target' -Credential $cred
Invoke-Command -Session $s -ScriptBlock { Stop-Service KswordARK -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500 }
Copy-Item -ToSession $s -Path 'Ksword5.1\x64\Release\KswordARK.sys' -Destination 'C:\ksword\gui\KswordARK.sys' -Force
Invoke-Command -Session $s -ScriptBlock { Start-Service KswordARK; Get-Service KswordARK | Select Status }
Remove-PSSession $s
```

### 9.4 跑测试（通用模板）

**不要用 Copy-Item 发脚本到 VM**（编码会被破坏）。用 scriptblock 直接传：

```powershell
$cred = New-Object PSCredential('Felix', (ConvertTo-SecureString 'password' -AsPlainText -Force))
$s = New-PSSession -VMName 'KSword-HVM-Target' -Credential $cred
$sc = Get-Content 'scratchpad\test_xxx.ps1' -Raw
Invoke-Command -Session $s -ScriptBlock { param($x) [scriptblock]::Create($x).Invoke() } -ArgumentList $sc
Remove-PSSession $s -ErrorAction SilentlyContinue
Get-VM -Name 'KSword-HVM-Target' | Select Name, State, Uptime
```

**长脚本（>60s）**：避免，PS Direct 会断。拆分或减少 Start-Sleep。

### 9.5 等 VM 重启

```bash
until powershell -NoProfile -Command "try { \$s=New-PSSession -VMName 'KSword-HVM-Target' -Credential (New-Object PSCredential('Felix',(ConvertTo-SecureString 'password' -AsPlainText -Force))) -ErrorAction Stop; Remove-PSSession \$s; exit 0 } catch { exit 1 }"; do sleep 5; done
```

### 9.6 查 CI.dll 内核加载地址（R3 侧）

PowerShell/C# P/Invoke `psapi.dll`:
- `EnumDeviceDrivers(IntPtr[] bases, uint cb, out uint needed)` 
- `GetDeviceDriverBaseNameW(IntPtr base, StringBuilder name, uint sz)`

然后 `targetAddr = ci_base + 0x1000`。**注意**：PowerShell 严格模式下 int64 → uint64 要用 `[BitConverter]::ToUInt64([BitConverter]::GetBytes([int64]$base.ToInt64()), 0)`，不能直接 `[uint64]`。

### 9.7 Int32/UInt32 NTSTATUS 转换

```powershell
function Format-NTSTATUS([int32]$v) {
    $u = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$v), 0)
    return "0x{0:X8}" -f $u
}
```

直接 `[uint32]$v` 当 `$v` 是负数（NTSTATUS 错误码为负）时会抛异常。

## 10. 关键 NTSTATUS 速查

| NTSTATUS | 符号 | 含义 |
|----------|------|------|
| `0x00000000` | STATUS_SUCCESS | 成功 |
| `0xC0000005` | STATUS_ACCESS_VIOLATION | VSL/VTL1 内部 #PF 被 SEH 捕获（典型：目标不在 catalog） |
| `0xC000000D` | STATUS_INVALID_PARAMETER | MDL flags/mode/length/ByteOffset/ByteCount 不符 |
| `0xC0000018` | STATUS_CONFLICTING_ADDRESSES | address+length 不在模块范围内 |
| `0xC0000225` | STATUS_NOT_FOUND | MiLookupDataTableEntry 找不到模块 |
| `0xC0000043` | STATUS_SHARING_VIOLATION | entry->field_68 bit 0x80000 已置位 |
| `0xC0000045` | STATUS_INVALID_PAGE_PROTECTION | MiGetPagePrivilege & 0x40 |
| `0xC0000428` | STATUS_INVALID_IMAGE_HASH | hash 不匹配（未在本任务中观察到） |

Win32 错误转 NTSTATUS 的常见对照：
- Win32=50 → STATUS_NOT_SUPPORTED
- Win32=87 → STATUS_INVALID_PARAMETER
- Win32=122 → STATUS_BUFFER_TOO_SMALL
- Win32=21 → STATUS_DEVICE_NOT_READY
- Win32=998 → `0x80000002` or `STATUS_ACCESS_VIOLATION` 等（需 case-by-case）
- Win32=1168 → STATUS_NOT_FOUND

## 11. 已知陷阱

1. **PS Direct 会话掉线**：长 scriptblock 会断（"Hyper-V 套接字目标进程已结束"），VM 不蓝但会话死。要么拆分，要么用 scheduled job + 文件记录。
2. **Copy-Item 到 VM 的脚本编码会被改**，运行会 "The string is missing the terminator" 之类错误。走 scriptblock 传 Raw 内容。
3. **PowerShell `[uint32](-1073741819)` 抛异常**。永远用 BitConverter。
4. **PowerShell `[uint64]$intptr.ToInt64()` 对负 int64 抛异常**（高地址内核 VA）。永远用 BitConverter。
5. **`Add-Type ... -ReferencedAssemblies System.Text`** 会报 "System.Text.dll could not be found"。`System.Text` 已在 base lib，不要显式引用。
6. **`DeviceIoControl` 的 `nInBufferSize` 必须精确等于**驱动 handler 声明的字节数，不能大于。
7. **所有 RXPF IOCTL 必须有 `FLAG_UI_CONFIRMED`** (0x1)，缺则 `STATUS_INVALID_PARAMETER`。
8. **CTL_CODE 的 device type 是 0x22 (FILE_DEVICE_UNKNOWN)**，不是 0x8000。
9. **设备名是 `\\.\KswordARKLog`**，不是 `\\.\KswordARK`。
10. **构建**：`Projects` 参数不支持；`Invoke-KSwordBuildCheck.ps1` 构建**全部**项目。单项目用直接 MSBuild。
11. **mode=1 SUCCESS 不保证内容恢复**。必须自己管理 Backup/Restore。
12. **自签驱动的自建 .text 段**（`.rxpftst` 之类）走 mode=1 恒 `0xC0000005`，不在 catalog 内。只能用 catalog 内的 Microsoft 签名模块（CI.dll / NTOS / NDIS / FLTMGR 等）。

## 12. 相关 memory 文件（.claude/memory/）

- `ksword-rxpf-mode1-restore-structural-fail.md` — 本任务的主结论（修订于本报告）
- `ksword-rxpf-mode1-realhardware-26h2-confirmed.md` — **26H2 实体机 primary evidence**（§3 稳的、§6 实验 #7；pre=0X48 postWrite=0XB7 postRestore=0XB7，CI/HVCI/HVCI-Strict/Secure-Boot 全开）
- `ksword-rxpf-is-half-useless-without-shadow-idt.md` — 影子 IDT 死局（Hyper-V 挡 LIDT）
- `ksword-rxpf-hvci-write-barrier.md` — mode=2 对 .data/.rdata 拿不到 RW alias
- `ksword-rxpf-current-handler-scope.md` — 现有 #PF handler 的受理范围
- `ksword-no-online-symbols.md` — 禁止在线符号服务器
- `ksword-driver-signing-for-vm.md` — 两档签名策略
- `ksword-dont-write-impossible-in-comments.md` — 不要在注释里写"不可能"
- `claude-rxpf-capability-confusion.md` — 之前的误判记录

## 13. 测试脚本清单（scratchpad/）

- `test_rxpf_mode1.ps1` — 测 `.rxpftst` (SELF_IMAGE_TEST)
- `test_rxpf_mode1_cidll.ps1` — 测 CI.dll +0x1000 带 Write
- `test_cidll_no_write.ps1` — CI.dll +0x1000 不写入控制组
- `test_rxpf_mode1_cidll_full.ps1` — Phase A/B/C 完整但会蓝屏

## 14. 风险与边界

- **每次改 CI.dll .text 并 mode=1 放行后，不还原内容必蓝屏**（SYSTEM_SERVICE_EXCEPTION, What failed: CI.dll）
- **PatchGuard 的检测窗口**是几分钟到几小时，取决于扫描周期。改了内容即使没执行到，PG 也会最终 bugcheck
- **测试证书签名的驱动**在 testsigning on 的 VM 上可以加载，但不要尝试在 production HVCI 机上用
- **禁止**在线符号服务器，禁止创建分支
- 本任务只对 Hyper-V 来宾验证过，物理机 HVCI 行为未验

## 15. 下一步接续者该做的

本节 next-exp 编号与 [`.claude/memory/ksword-rxpf-mode1-restore-structural-fail.md`](../.claude/memory/ksword-rxpf-mode1-restore-structural-fail.md) 一致。已完成的（#3 keep-modified / #5 auto-restore / #6 Phase F 整合 / 26H2 primary evidence #7）不再重复列出。

- **next-exp #1 — EXTERNAL_IMAGE bad-digest 对照**：临时解除 EXTERNAL_IMAGE 的 `STATUS_NOT_SUPPORTED` 短路，分别传 (随机 32B / SHA-256(页) / SHA-512(页)[0x20,0x40))，看 mode=1 返回能否区分 digest 内容——关掉 "VTL1 验 digest" 归因争议。
- **next-exp #2 — BugCheck Arg 捕获 — 已完成**（2026-10-19，VM 26H2 build 26300.9457 CI/HVCI/HVCI-Strict/Secure Boot 全开）：
  - **流程**：`CrashDumpEnabled=2 + AlwaysKeepMemoryDump=1 + AutoReboot=1` 配好；用 `KswordCLI memory change-image-protection --address <CI.dll+0x1000> --operation 2 --flags 0x1`（mode=2 翻 NX + writeTest，不恢复）；R0 返回 `imageProtectStatus=0x0` + `writeTestStatus=0x0` + `restoreStatus=0xC00000BB`（NOT_SUPPORTED，因未传 flags 0x2）；约 1 分钟后 VM 自发 BSOD + AutoReboot；拉 `C:\WINDOWS\Minidump\101926-4203-01.dmp` 到宿主用 cdb `!analyze -v` 分析。
  - **BugCheck 完整参数**（minidump 原文）：
    - **Code = `0xFC` ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY**
    - **Arg1 = `0xfffff807702e1598`** —— 故障 VA = CI.dll (`0xfffff807702e0000`) + **`0x1598`**，**完全落在被改页 +0x1000..+0x1FFF 内**
    - **Arg2 = `0x8900000105b3b121`** —— PTE qword，**bit 63 (NX) = 1**，直接确认 mode=2 的 R-only NX 位到了 CPU（与 next-exp #8 的 live PTE 回读一致：顶字节 `0x89` = NX+Present）
    - Arg3 = `0xffffc0041e9162e0`（reserved）；Arg4 = `0x2`（reserved）
    - 栈：`nt+0x4a732f → KeBugCheckEx(0xFC) → CI+0x1598`
  - **原预期范围（CI.dll+0x1000..+0x1008）过窄**：#2 原文写的是"故障 RIP 真落在 CI.dll+0x1000..+0x1008 内"，这是 payload 字节的范围。实际 NX 粒度是**整页 4 KB**，Arg1 落在 CI.dll+0x1598 说明**页内另一处真实被调用的 .text 指令被 NX 捕获**，而非被改的首几个字节。两者都在被改页内，均符合 Phase G 语义。
  - **直接后果**：CI.dll+0x1598 是**真实的被执行入口**（否则不会命中），故 **mode=1 restore 清掉 NX 位后（§3 实证），下次此执行流就会执行被我们改过的字节** —— 这是 Phase G CI 旁路可行性的直接 CPU 层证据。
  - **PatchGuard 未先触发**：BSOD 为 `0xFC` 而非 `0x109 CRITICAL_STRUCTURE_CORRUPTION`，说明 NX 捕获比 PG 周期扫描更快命中（本次约 1 分钟）；若要观测 PG 自身的检测结论需延长"不执行到的 payload 残留窗口"（例如改页中真空区域而非被调用的 code path）。
  - 判据工具链：`cdb -z <dmp> -y C:\Symbols -cf <script>`（本地符号路径，禁在线符号服务器）；minidump 虽然只有寄存器+栈但 BugCheck Code/Arg1-4 从 dump header 可直读。
- **next-exp #4 — 扩模块覆盖 — 已完成**（2026-10-19，VM 26H2 build 26300.9457 全防护开启）：
  - **结论：mode=1/2 对 catalog 内 Microsoft 签名的 4 KB 常规页模块可外推**，但 **ntoskrnl 本身的 .text 用 2 MB 大页布局，MmChangeImageProtection 结构性拒绝大页**（独立于签名的第二道门）。
  - 对照数据（三个成功模块的 PTE top byte 全部从 `0x09` 翻到 `0x89` 又回 `0x09`，模式完全一致）：

| 模块 | 目标 VA 偏移 | 页类型 | `imageProtectStatus` | PTE qword |
|---|---|---|---|---|
| **ntoskrnl.exe** | base + 0x10000 | **2 MB 大页** | `0xC0000018 CONFLICTING_ADDRESSES` | 不变（PTE=0, 走 PDE 大页） |
| **ndis.sys** | base + 0x1000 | 4 KB 常规 | `0x0 SUCCESS` | `0x9…121` → `0x8…121` → `0x9…121` ✓ |
| **fltmgr.sys** | base + 0x1000 | 4 KB 常规 | `0x0 SUCCESS` | 同上 ✓ |
| **CI.dll**（对照） | base + 0x1000 | 4 KB 常规 | `0x0 SUCCESS` | 同上 ✓ |

  - **ntoskrnl 大页硬门禁的意义**：之前认为 "nt 内核能被 mode=2 Change" 的推理在本机 build 结构上成立但实践上不可达——nt .text 以 2 MB 大页映射，MmChangeImageProtection 的内部逻辑拒绝接受大页目标。要操作 nt 本身必须先想办法让 nt .text 切回 4 KB 页（例如重编译 kernel 或靠调试 flag），本会话未探索。
  - §6 实验 #7 的 26H2 primary-evidence PageBase `0xFFFFF80429FE0000` 对应 size 0x1450000（和 ntoskrnl 相同），结合本次 #4 发现，说明实体机 primary-evidence 的那次 mode=1 成功**不是** nt 本身，而是**另一个 catalog 内常规页模块**——需要 ops 在实体机对照 R0 日志里的模块枚举表确定具体 moduleName（见 §6 中 `<TBD-OPS>` 占位符）。
- **next-exp #7 — MiFlags qword 直读 — 已完成**（2026-10-19，VM 26H2 build 26300.9457 CI/HVCI/HVCI-Strict/Secure Boot 全开）：
  - 原条目的 `RVA 0xD1EA48` **已过期**（可能是旧 build 偏移），本机 build 26300.9457 的 PDB 符号查询 (`ntkrnlmp.pdb` GUID `C29EBFB06B78B3C020DCA66D99713F9E` Age=1) 显示 `MiFlags` 真实 RVA = **`0xFC6AC8`**（public symbol, segment 27:0x1AC8）。
  - R0 读 `nt base + 0xFC6AC8 = 0xfffff803abbc6ac8` → `query-pte` 得 PA `0x1013C6AC8`（2 MB large page, pde `0x8A000001012009E3`, NX=1）→ `read-phys` 回 8 字节 `6c fa 85 f9 c4 00 00 00` ⇒ MiFlags qword = **`0x000000C4F985FA6C`**。
  - **`bit 0x4000` (StrongCodeGuarantees) = 1** ✓ —— HVCI running + Enforced 场景下的预期值，直接验证 §3 "稳的" 第 3 条的间接推理。
  - 工具链：`KswordCLI memory query-pte` + `memory read-phys`（`read-va` 对 kernel 大页返 win32=87, 走物理地址旁路可读）。
- **next-exp #8 — live PTE qword 回读 — 已完成**（2026-10-19，VM 26H2 build 26300.9457 全防护开启）：
  - 目标：CI.dll + 0x1000 = `0xfffff8063dd11000`（4 KB 常规页，非大页，PTE 可直接 query）
  - R0 用 `KswordCLI memory query-pte` 读 PTE qword 对三态直接观测：
    - Change 前 PTE = `0x090000010513B121` (top byte `0x09`, bit 63 = 0, NX=0 → **RX**)
    - `change-image-protection --operation 2` 后 PTE = `0x890000010513B121` (top byte `0x89`, bit 63 = 1, NX=1 → **R-only/NX**)
    - `change-image-protection --operation 1` 后 PTE = `0x090000010513B121` (**完全回到初始 qword**, NX=0 → **RX**)
  - **直接证据取代循环证据**：原 §5.5 `originalProtection=0x20` 是 PE 节头 characteristics 静态推算（见 A-4 / A-5 注记），现在 live PTE qword 独立确认 mode=2 把 bit 63 (NX) 从 0 翻到 1、mode=1 让 PTE 恢复到初始 qword。
  - 副产物：MmChangeImageProtection 本机 VA = `0xFFFFF806AC25DDB0`（RVA = 0xA5DDB0，R0 的 `resolvedFunctionVa` 字段直接给）。`originalPte=0xCC` 这个 R0 字段**不是** PTE qword，只是 PE 节头推的 1 byte 保护位，容易误读。
  - 工具链同 #7：`query-pte` 读 PTE，`change-image-protection` 跑 Register+Change（flags 0 时 R0 日志报 `aliasMapStatus=STATUS_NOT_SUPPORTED` 跳 alias 阶段，但 `imageProtectStatus=0` 已覆盖本实验目标）。

### Phase F → Phase G 代码接口已就绪

- `FLAG_SKIP_BACKUP_RESTORE = 0x4` 已加（§5.3）
- Unregister 可选跳过 auto-restore
- `IOCTL_QUERY_BACKUP (0x90B)` 已上线（§5.2/§5.7），Phase G R3 可读 Backup 恢复原字节
- `KernelHookWizard` GUI 已把整条链封装（§7.3）

本报告路径: `docs/RXPF-mode1-VTL1-ABI-report-20261018.md`
生成于: 2026-10-18，Phase F 补充于当日
