# 已卸载驱动来源布局与配置缺口

- `dyn status` 的 `status=9` 是 `INITIALIZED | EXTRA_ACTIVE`，不是 NTOS profile 已激活。
  `No exact System Informer profile matched this kernel image.` 不影响驱动服务进入 Running。
- 单项枚举不能仅凭 `NtosActive=false` 拒绝：应检查初始化、当前 NTOS 身份，以及该来源每项地址/字段的来源和边界。
  `kernel_unloaded_layout.c/.h` 集中实现 MmUnloadedDrivers 和 PiDDB 的查询布局；不修改整体 DynData 激活状态。
- 公共 PDB 可以提供精确 `MmUnloadedDrivers` RVA，但不提供 `_UNLOADED_DRIVERS` 私有类型。
  已知可信 RVA 的补全使用 `KswordARKDriverResolveMmUnloadedLayout`，复用多记录运行时验证，
  仍要求至少两条有效记录、唯一布局、当前映像可写数据节和支持的 OS build；不把推断布局标作 PDB 字段。
  补全仅写入一次查询的局部快照，因此 `dyn fields` 仍可显示全局布局为 Unavailable。
- Mm 记录和三来源共用的名称缓冲区通过 `KswordARKRuntimeReadMemory` 读取；
  不使用 SEH 包裹 memcpy/直接解引用来保护可能失效的记录地址。
- 2026-10-02 本机证据：内核 `10.0.19041.7725`，PE timestamp `CD28AC68`、image size `01045000`，
  RSDS `63950D24-7362-0017-E1FC-214544590A77/1`。旧包 2405 项中无匹配；本地精确 PDB 生成了 224 项配置，
  其中 MmUnloadedDrivers RVA `00C2A498`，私有 Mm/PiDDB 类型仍缺失，不能伪造类型字段。
- 原有 2405 项保留，源码/Release pack 新增唯一当前 NTOS 项。候选产物与配置备份位于
  `.codex-build-logs/unloaded-dyndata-20261002/`，不作为正式矩阵语料库完整导入或签名/实机通过证据。
  候选 x64 MSVC/WDK Build、ApiValidator Universal、INF/CAT 通过，零警告；候选未签名、未装载。
- `KswordARKDriver/tests/UnloadedLayoutRegression.ps1` 使用生产布局代码和有界合成内存做 23 项离线回归；
  另跑 SafeReadRegression 与 IOCTL plan gate。实机是否获得记录仍需要正常换版后验证。
- 运行时全局扫描不必依赖 PDB。原 `0x800` 固定长度扫描会包含相邻函数，提前耗尽 64 函数调用链预算。
  本机生产代码回放旧路径得到 482 个引用但没有 `MmUnloadedDrivers`；
  `KswordARKRuntimeCollectFunctionDataReferences` 通过 PE x64 异常目录二分定位函数边界，
  再扫描受限直接调用链，可在当前内核发现精确 RVA `00C2A498`。函数表缺失/损坏时拒绝扫描，
  普通 `CollectAnchoredDataReferences` 其它使用者保持原行为。
- Mm 候选唯一性按全局地址判定：同一全局在不同函数出现不算歧义；其它全局的相同得分仍然拒绝，
  重复引用不能清除此前其它全局引起的歧义。
- `RuntimeUnloadedScanRegression.ps1` 的扫描与布局函数均取自生产源码；传入期望 RVA 只用作测试断言，
  不作为扫描输入且不读取 PDB/配置。当前内核在两个合成装载基址下发现地址并通过合成多记录布局验证，
  以及缺失/越界函数表拒绝，共 8 项断言通过。不能据此宣称其它 Windows 版本或实机查询已通过。

- 2026-10-12 离线 PiDDB/CI 研究：当前 `10.0.19041.7725` 的 ntoskrnl PDB 只有
  `PiDDBCacheTable=0xD2F000`、`PiDDBLock=0xC44C40`，没有 PiDDB 私有条目类型；CI PDB
  只有 `g_KernelHashBucketList=0xC3090`、`g_HashCacheLock=0x39DC0`，没有条目结构类型。
  这确认缺口是 PDB 本身不含私有类型，不是提取器漏读。
- PiDDB 查询现在在精确可信全局通过后，以只读共享锁、AVL 完整性检查、系统资源身份探针和多条目一致性
  补全本次查询所需的私有字段；不会写回全局 DynData，也不会把推断字段冒充 PDB 来源。CI 查询现在会在
  非空锚点仍不足以覆盖缓存时，按可执行节分批继续扫描，并删除了把不透明哈希负载猜成时间戳/状态/映像
  的逻辑；可选字段继续保持不可用。
- `CacheLayoutFallbackRegression.ps1` 使用生产 C 代码和合成内存覆盖 PiDDB/CI 的有效、忙锁、资源身份、
  AVL/链表环、歧义和 IRQL 拒绝路径，22 项断言通过；另有 SafeRead、Mm 扫描和既有布局回归通过。候选
  x64 驱动通过 MSVC/WDK、ApiValidator Universal、INF/CAT，产物在 `.codex-build-logs/piddb-ci-layout-investigation/driver/`，
  未签名且未加载。硬盘移除期间只能继续做离线验证，无法代替实机驱动查询。
- 同轮审查发现 CI 消费端仍有旧的裸指针读取；现已改为 `KswordARKRuntimeReadMemory`，包括链表头、Next、名称和所有可选字段。
  当前可访问系统的 `ci.dll` 是 `10.0.19041.5737`，而离线研究的匹配 CI PDB 是 `10.0.19041.7725`，因此不能把本机 CI 文件当作同一身份的实机证据。
