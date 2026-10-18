# `MmChangeImageProtection`：22621.4317 与 26100.9022 指令分析

本报告只以本机 PE、匹配的 PDB 和直接反汇编为行为证据。所有地址均为模块 RVA。两版的逐条指令、完整调用表和重点子函数指令见文末证据索引。

## 样本与方法

| 样本 | SHA-256 | PDB RSDS GUID / Age | 导出 RVA |
| --- | --- | --- | --- |
| 22621.4317 `ntkrnlmp.exe` | `8214B4F98D1E18DF604BF2A9ED0CB3C6C4DF062A95181964E3DA5E359CCA4008` | `9074FC2B-82ED-2B7E-1CB3-366B64BE62F9 / 1` | `0x736CF0` |
| 26100.9022 `ntoskrnl.exe` | `CB0603CD0BC46BFB2D94963E8A525C3C78BA90E3739E09DEEB3185FC71613C7B` | `9DAAB201-5E4C-ABF6-9800-9DBD0548B669 / 1` | `0xA62300` |

22621 的 PDB 是用户指定的本地文件；26100 的 PDB 由该 PE 的 RSDS 身份在微软符号服务器获取并校验。使用 CDB 的 `uf`、DbgHelp 符号解析、PE 节解析和 Capstone 独立复核指令边界。IDA 9.3 的本机批处理接口报告无有效许可证，因此下文的 C 伪代码均为**按指令手工重构**，并非 Hex-Rays 输出。没有加载驱动、修改内核或做运行时保护测试。

22621 的主热区是 `0x736CF0..0x736F9A`，还有 CDB `uf` 发现的冷区 `0x8C908E..0x8C912B`；逐字节列表共 206 条，包含 `0x736F47` 的对齐 `CC`。26100 的函数 `.pdata` 为 `0xA62300..0xA62585`，CDB 与 Capstone 都解出 158 条。

## 签名、入口检查与分流

匹配 PDB 的 `_MDL` 定义把 `MdlFlags`、`StartVa`、`ByteCount`、`ByteOffset` 分别放在 `+0x0A`、`+0x20`、`+0x28`、`+0x2C`；两版的 `RCX` 访问这些位置，并从 `+0x30` 读取 PFN 数组。因此可以把第一参数确定为 `PMDL`。`RDX` 是被检查的范围起点，`R8` 是长度且高 32 位必须为零，`R9D` 只接受内部模式 `1` 或 `2`；返回值在 `EAX` 中，按 NTSTATUS 负值判断。可按如下近似原型阅读：

```c
NTSTATUS MmChangeImageProtection(PMDL mdl, void *start,
                                 size_t length, unsigned mode);
```

这不是从调用者恢复的精确私有声明：所检 nt 镜像均无该导出的静态直接调用，且本机 26100 CI.dll 未导入它。22621 入口 `0x736CF5` 和 26100 入口 `0xA6230F` 把第四参数 `R9D` 存入 home slot；后续栈读取不是第五参数。`mode=1/2` 也不是 Win32 `PAGE_*` 常量。

| 检查或动作 | 22621 RVA | 26100 RVA | 指令可见结果 |
| --- | --- | --- | --- |
| 只接受模式 1/2 | `0x736D0E..0x736D22` | `0xA62327..0xA62334` | 其余返回 `0xC000000D` |
| 长度高 32 位、MDL flags、偏移、页对齐 | `0x736D28..0x736D5A` | `0xA6233A..0xA62366` | 不符返回 `0xC000000D` |
| 镜像范围与溢出检查 | `0x736DA2..0x736DD9` | `0xA6241F..0xA62456` | 不符返回 `0xC0000018` |
| 已有镜像操作标志 `0x80000` | `0x736DDF..0x736DE9` | `0xA6245C..0xA62466` | 返回 `0xC0000043` |
| 物理地址 / 原型 PFN / 页特权检查 | `0x736E31..0x736E90` | `0xA62470..0xA624C8` | 拒绝相应 PFN 或特权状态 |
| 首次设置内部保护值 1 | `0x736EB1..0x736EC2` | `0xA62390..0xA623A1` | 两种模式均执行 |
| 模式 1 的 VSL 验证 | `0x736F48..0x736F68` | `0xA623B1..0xA623BC`, `0xA62522..0xA62534` | 仅在 `MiFlags & 0x4000` 时调用；负值直接走清理 |
| 模式 1 第二次设置内部保护值 3 或 `0x13` | `0x736F6E..0x736F8F` | `0xA623C2..0xA623E4`, `0xA62549..0xA62558` | `MiFlags & 0x8000` 且 VSL 返回 `0x12C` 时选 `0x13` |
| 模式 2 清除每页的 image-verified 状态 | `0x8C90E2..0x8C911B` | `0xA624D3..0xA6251B` | 仅在 `MiFlags & 0x8000` 时执行 |

两版的完整条件分支、状态码和手工伪代码分别见 [22621 报告](../../output/mmchangeimageprotection-20261018/22621-report.md)与[26100 报告](../../output/mmchangeimageprotection-20261018/26100-report.md)。

## PTE、VTL 与方向

`MiSetImageProtection` 进入 `MiSetSystemCodeProtection`，普通有效 PTE 路径构造新 PTE、写入并交给 TLB flush 列表。22621 的 `MmProtectToPteMask` 在 RVA `0x163D0`，26100 在 RVA `0x10A00`；实际表项索引 `1` 为 `0x8000000000000000`（NX bit 63），索引 `3` 为零，索引 `0x13` 为 `0x18`（bit 3/4，无 NX）。22621 的读取在 `MiMakeValidPte` RVA `0x24196E..0x2419B7`，26100 在 `0x36755B..0x3675A9`。这支持模式 2 将普通目标页撤销执行许可，以及模式 1 验证后**尝试恢复**执行许可。构造函数另有特定条件下追加 NX 的分支；最终页状态必须运行时读回才能确认。

所检 PTE 写入指令是 22621 RVA `0x25A752`、26100 RVA `0x29568A` 的 `mov [rdi],rbx`。在该写入函数已覆盖的路径中未见对 PTE 的 `lock cmpxchg`。22621 的 `lock bts` / `lock and` 位于 `MiSetSystemCodeProtection` RVA `0x258A33` / `0x258A74`，目标为 PFN 元数据 `+0x18`。写入助手保留旧 PTE 的 bits 56..62，NX/RW/Present 等其余位取新构造值；硬件 RW bit 1 不是由主函数的 `mode=1/2` 直接写入的。22621 的 `MI_PFN_IS_PROTO` 在 RVA `0x2E9660..0x2E9669` 读取 `[PFN+0x28]` 的符号位；26100 在主函数 `0xA624AF..0xA624B6` 内联同一检查。其为原型 PFN 分流，不应与“直接对原型 PTE 执行原子写”混同。

22621 在 `VslValidateDynamicCodePages` RVA `0x8794EA` 用 `CL=2`、`EDX=0x1F` 和 `R9=&request` 调 `VslpEnterIumSecureMode`，后者在 RVA `0x338763` 调 `HvlSwitchToVsmVtl1`。该 wrapper 在 `0x410FDF` 从 `HvlpVsmVtlCallVa` 读取运行时函数指针，`0x410FE6` 经仅含 `jmp rax` 的 helper 调用。这里的 `0x1F` 是可见的内部 IUM 请求值，**不能作为 TLFS hypercall 编号**；已检静态链中没有可确认的 `vmcall`、`vmmcall` 或 TLFS 编号。VTL1 实际哈希、签名或调用者检查不在这些 nt 样本内，不能据函数名补写算法。26100 对应指令链和请求布局见其分版报告。

TLB 路径同样经子调用而非主函数内的 `invlpg`：22621 `MiSetSystemCodeProtection` RVA `0x258AD0` 插入 flush 条目、`0x258B24/0x258B69` 调 `MiFlushTbList`；向下可见 `KiFlushCurrentTbWorker` 的 CR3 reload（`0x33154D/0x331550`）和 `KeFlushTb` 的跨核 IPI 调用（`0x25E8D2/0x25E989`）。26100 的 `MiFlushTbList` 则在 RVA `0x22CD6F/0x22CDFD` 有 `invpcid`、`0x22CD79/0x22CE07` 有 `invlpg`，在 `0x22D20B/0x22D3E8` 可调 `KiIpiSendRequest`，在 `0x22D0A0/0x22D38A` 可调 `HvlFlushRangeListTb`。这些是不同条件分支，不能宣称每次调用都会执行。

26100 的 TLB flush 支路还能进入 hypercall wrapper：`HvlFlushRangeListTb` 在 RVA `0x22D7B9` 置控制字低位 `3`，`0x22D7D0` 写入高 32 位的计数，随后 `0x22D840..0x22D843` 调 `HvcallInitiateHypercall`；另一支在 `0x22D8AC..0x22D925` 构造低位 `0x10003` 并在 `0x22D8B9` 写入高 32 位的计数，然后调 `HvcallFastExtended`。完整 `RCX` 因此含计数，不能写成字面量 `3` 或 `0x10003`。前者在 RVA `0x22E08D` 从 PDB 符号 `HvcallCodeVa`（全局 RVA `0x1201858`）读指针，`0x22E0A6` 执行 `call rax`。该指针的磁盘初值为 `0x1404BBDE0`，即 RVA `0x4BBDE0` 的 `mov eax,0x1000; ret` stub；运行时是否重新绑定未由磁盘样本证明。`0x22D843` 与 `0x22D925` 的紧邻调用者均未直接对返回 `RAX` 分支。这些控制字属于**TLB flush 支路**，不能写成上面的动态代码验证请求值或证明其每次触发；所检磁盘入口没有 `vmcall`/`vmmcall`。寄存器输入、缓冲区和返回值检查逐项见 26100 分版报告。

## 调用者与版本差异

[调用点扫描报告](../../output/mmchangeimageprotection-20261018/xrefs-report.md)逐节检查了 nt 镜像的 `E8 rel32`、`E9 rel32`、RIP 相对地址引用及绝对指针，并以 `dumpbin` 独立交叉检查：两个 nt 镜像均未发现指向此导出入口的静态直接调用。26100.9022 的 CI.dll 有 262 个具名 nt 导入，其中没有 `MmChangeImageProtection`；所有这些导入也没有按序号导入。22621 的 CI.dll 样本未提供、未在限定目录取得，因此其调用者无法列全。当前样本也就没有可恢复的调用者 `PAGE_READWRITE → PAGE_EXECUTE_READ` 常量对、HotPatch/Patch/Fix 或 Retpoline/Spectre 调用点。此结果不排除其它模块或运行时函数指针调用导出。

主函数的直接 `call` 数量由 22621 的 22 处变为 26100 的 8 处，主要可见变化是加载项锁处理集中到 `MiLockLoadedDataTableEntryIfNecessary` / `MmUnlockLoadedDataTableEntry`，22621 的 `MI_PFN_IS_PROTO` 符号调用在 26100 内联，模式 2 调 `MiClearPfnImageVerified` 的第二参数由 `0x18` 改为 `9`。更深一层的 `VslValidateDynamicCodePages` 传入的内部 IUM 请求值由 22621 RVA `0x8794D8` 的 `0x1F` 改为 26100 RVA `0xA626DC` 的 `0x20`；两版的同一助手都有限制临时 MDL 最多一页的判断（`0x879420..0x879424` / `0xA62624..0xA62628`）。没有对应 secure-kernel 样本，不能从请求数值变化推断验证算法。两版都保留条件式 VSL 验证和页特权检查；主函数层没有发现 26100 新增的哈希、签名或调用者身份验证。进一步的子函数差异见[逐项对比](../../output/mmchangeimageprotection-20261018/diff-report.md)。

## 证据索引

- [22621 完整符号反汇编](../../output/mmchangeimageprotection-20261018/22621-complete-disassembly.txt)、[全部 22 个调用点](../../output/mmchangeimageprotection-20261018/22621-calls.tsv)、[函数及重点子调用报告](../../output/mmchangeimageprotection-20261018/22621-report.md)。
- [26100 完整符号反汇编](../../output/mmchangeimageprotection-20261018/26100-mmchange.asm)、[全部 8 个调用点](../../output/mmchangeimageprotection-20261018/26100-mmchange-calls.json)、[函数及重点子调用报告](../../output/mmchangeimageprotection-20261018/26100-report.md)。
- [跨版本差异](../../output/mmchangeimageprotection-20261018/diff-report.md)、[nt 与 CI 静态交叉引用](../../output/mmchangeimageprotection-20261018/xrefs-report.md)。

重点子函数的前 200 条（或函数结尾）指令文件和重复调用图均由两份分版报告逐一链接。全部分析是静态证据；没有把编译、驱动加载、HVCI 实机状态或最终页面权限当作已验证结果。
