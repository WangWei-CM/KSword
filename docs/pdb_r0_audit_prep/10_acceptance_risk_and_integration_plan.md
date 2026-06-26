# 基于 PDB 的稳定 R0 审计功能：验收、风险边界与集成总控文档

## 1. 文档定位

本文是 PDB 驱动的稳定 R0 审计能力前期准备的总控文档，用于统一验收门槛、R0 安全边界、测试矩阵、UI 决策输入、后续集成阶段和多会话文档合并流程。

本文不替代各专题会话的字段级设计，不在本文中展开 `_EPROCESS`、`_ETHREAD`、`_DRIVER_OBJECT`、WFP、NDIS、Minifilter 等具体字段清单。字段细节应继续由各专题文档维护，本文只规定这些字段设计进入主线前必须满足的全局质量和安全条件。

### 1.1 已对齐的仓库约束

- R0/R3 协议只能在 `shared/driver/` 定义。
- 驱动 IOCTL handler 只能通过 `KswordARKDriver/src/dispatch/ioctl_registry.c` 注册。
- 用户态设备访问只能通过 `Ksword5.1/Ksword5.1/ArkDriverClient/`，Dock UI 不直接调用 KswordARK `DeviceIoControl`。
- 已有 IOCTL 审计显示 shared 定义与 registry 注册结构一致，但 mutating IOCTL 仍存在访问控制风险，需要在后续安全门禁中复查。
- 已有 DynData v3、cross-view、kernel memory evidence、driver integrity、mutation/audit、safety/preflight 等文档和协议可作为后续 PDB R0 审计能力的集成基础。

## 2. 全局验收标准

所有基于 PDB profile 的稳定 R0 审计功能，必须同时满足本节验收项才允许进入发布候选。单个功能页可以追加更严格标准，但不得降低本文门槛。

| 验收项 | 最低通过标准 | 阻断条件 | 证据形式 |
| --- | --- | --- | --- |
| PDB profile 命中率 | 支持的目标系统 build 必须能按 ntoskrnl identity、PDB GUID/Age、TimeDateStamp/SizeOfImage 命中正确 profile；命中失败必须可解释。 | 错配 profile 后仍继续读取私有字段；profile 来源无法追溯；无法区分 exact match 与 fallback。 | profile 匹配日志、pack 报告、R3 loader 诊断、R0 DynData status。 |
| 字段覆盖率 | 每个功能声明 required/optional 字段；required 字段缺失时功能降级或禁用；optional 字段缺失时 UI 显示缺失原因。 | required 字段缺失仍执行私有结构遍历；用猜测偏移替代 profile。 | schema coverage 报告、missingFields/missingGlobals、capability mask。 |
| 版本覆盖率 | Win10/Win11 代表性 build 有明确支持矩阵；未覆盖 build 只能走无 profile 降级路径。 | 将未验证 build 标记为稳定支持；发布包缺少覆盖范围说明。 | build 矩阵、profile pack 索引、测试记录。 |
| 无 profile 降级 | 无 profile、profile 不匹配、capability 缺失时，功能必须失败关闭或降级到公开 API/只读粗粒度证据。 | 继续执行私有字段读取、链表遍历或写操作。 | UI 状态、R0 status code、ArkDriverClient unsupported/graceful message。 |
| 不崩溃 | 任何受支持和不受支持环境下，R0 查询不得造成 BSOD、hang、长时间不可取消阻塞。 | 崩溃、死循环、DPC/IRQL 误用、不可控大范围扫描。 | VM/KD 测试日志、崩溃转储结论、超时记录。 |
| 输出稳定性 | 同一环境重复查询结果的排序、字段含义、风险标记和 partial 状态稳定；不可依赖随机指针顺序作为唯一 UI 语义。 | 重复刷新导致相同对象风险等级反复跳变且无原因；partial 未标记。 | 重复运行 diff、JSON/CSV 导出对比。 |
| UI 字段一致性 | UI 表格、详情面板、导出字段、risk center 聚合字段必须使用同一语义和单位。 | 同一字段在不同视图名称/单位/颜色不一致；unknown 与 clean 混淆。 | UI 字段映射表、导出样例、截图验收。 |

### 2.1 Profile 命中率分级

- `Exact`：PDB GUID/Age 与目标模块 identity 完整匹配，允许启用对应 capability。
- `Compatible`：同 build、同 TimeDateStamp/SizeOfImage 但缺少完整 PDB identity，仅允许经专题文档批准的只读能力。
- `Fallback`：profile 不存在或 required 字段不足，只允许公开 API、SystemInformation、模块范围归因、基础状态提示。
- `Blocked`：identity 冲突、字段越界、RVA 超出 image、结构大小异常，必须禁用相关 R0 私有字段路径。

### 2.2 字段覆盖率口径

- 字段覆盖率按功能页和 evidence source 单独计算，不用全局平均数掩盖关键字段缺失。
- Required 字段覆盖率必须达到 100% 才能启用依赖该字段的私有遍历。
- Optional 字段不得影响核心只读查询的稳定性，但必须进入 diagnostics，便于 UI 说明“未检测”与“未发现”的区别。
- 全局 RVA 类字段只能作为 optional 或 gated capability 使用；找不到全局符号不得导致 profile 整体失败。

## 3. R0 安全边界

### 3.1 默认只读原则

- PDB R0 审计能力默认只读，只收集 evidence、source mask、confidence、risk flags、status 和 detail。
- read-only 查询可以使用 profile 中的结构偏移和全局 RVA，但不得修改目标对象、链表、表项、回调、hook、PTE、MSR 或 CR0 WP。
- Kernel memory evidence、cross-view、driver integrity、callback/filter/network chain 枚举都应先以只读 evidence 形态进入主线。

### 3.2 禁止默认 patch/delete/bypass

默认 UI 和默认 R0 policy 禁止以下行为：

- patch inline hook、IAT/EAT、SSDT、Shadow SSDT、IDT/GDT/MSR 或任意内核字节；
- delete/unlink callback、DriverObject、DeviceObject、Minifilter、WFP callout、NDIS filter、进程或线程链表节点；
- bypass PPL、callback policy、Minifilter、WFP、NDIS 或安全产品过滤链；
- force unload driver 或清理卸载失败后的残留对象；
- 任意物理内存写、PTE 写、CR0 WP 修改。

如确需提供修复/变更能力，必须作为单独高级功能进入 mutation/audit，而不是审计查询的附带按钮。

### 3.3 所有写操作进入 mutation/audit

任何写操作必须满足：

1. `PREPARE` 阶段只读验证目标、采集 before snapshot、生成 transaction id。
2. `COMMIT` 阶段重新验证目标身份、before bytes/hash、capability、safety policy、用户确认和 FORCE 标记。
3. `ROLLBACK` 阶段只能恢复已记录且仍匹配的目标，失败时保留证据，不做二次破坏性清理。
4. `QUERY_AUDIT` 必须能导出最近事务记录，默认隐藏敏感字节，必要时经显式选项包含。
5. UI 只显示 dry-run、audit、rollback 状态；危险提交入口必须独立评审。

### 3.4 指针校验

R0 私有结构读取必须执行以下校验：

- canonical kernel address、对齐、范围、模块归属和 image bounds 校验；
- profile offset/global RVA 上限校验，禁止使用 `0xFFFFFFFF`、零 RVA 或超出 image 的 RVA；
- 对链表指针、对象指针和函数指针使用 guarded read，并在失败时返回 partial/unavailable evidence；
- R3 提供的对象地址不能作为可信对象身份，必须通过公开引用、CID table、模块快照或其他 R0 可验证来源确认；
- 指针落入用户态、分页不可读、非预期模块或非执行映像范围时，只能标记风险，不得尝试修复。

### 3.5 列表遍历上限

所有链表、表、树、全局数组和扫描路径都必须有默认上限和硬上限：

- 默认上限面向常规环境，避免 UI 卡顿和长时间 R0 占用。
- 硬上限不可由 UI 无限放大，超限必须返回 `partial/budget_exhausted`。
- 遍历必须具备 loop detection、重复对象去重和 progress 统计。
- 对 BigPool、CID table、loaded module list、callback list、filter chain、network/filter chain、handle table 等高风险结构分别设置独立 budget。

### 3.6 try/except、probe、timeout 策略

- 只在 PASSIVE_LEVEL 或专题设计明确允许的 IRQL 执行可阻塞或 pageable 查询。
- R0 guarded read 使用结构化异常保护，失败时记录 `lastStatus`，不得吞掉错误后继续使用未初始化数据。
- 用户输入 buffer 只通过 WDF/协议层获取和长度校验，不手写信任长度。
- 大范围扫描必须支持 timeout/budget，R3 刷新周期不得无限并发堆积请求。
- 发生异常、timeout、capability 缺失时，R0 返回部分结果和原因；UI 不把 partial 渲染成 clean。

## 4. 测试矩阵

### 4.1 OS 与构建覆盖

| 维度 | 必测组合 | 验收关注点 |
| --- | --- | --- |
| Win10 build | 至少覆盖一个 LTSC/稳定企业常见 build、一个 22H2 build。 | profile 命中、字段覆盖、fallback、cross-view 结果稳定。 |
| Win11 build | 至少覆盖 23H2/24H2 或当前主线 build。 | VBS/HVCI、Kernel isolation、PDB 字段变化、UI 降级提示。 |
| 未覆盖 build | 至少一台无 profile 或 profile 故意移除环境。 | 无 profile 降级、不崩溃、不猜偏移。 |
| 符号错配 | 使用错误 GUID/Age 或错误 SizeOfImage 的 profile。 | 必须 blocked，不得继续启用 capability。 |

### 4.2 安全与平台特性

| 维度 | 组合 | 验收关注点 |
| --- | --- | --- |
| VBS/HVCI | on/off | R0 查询可用性、禁止非法写、风险提示、性能退化。 |
| Secure Boot | on/off | 驱动加载前置提示、profile 与安全状态展示。 |
| BitLocker | on/off | 发布包和日志路径、崩溃恢复、磁盘证据读取的 R3 边界。 |
| Hyper-V | on/off | 虚拟化环境字段差异、CPU/MSR 只读证据、timeout。 |
| KD/VM Snapshot | enabled/disabled | 高风险功能只在 VM/KD 验证；生产环境保持只读。 |

### 4.3 交互和桌面环境

| 维度 | 组合 | 验收关注点 |
| --- | --- | --- |
| 多显示器 | 单屏/双屏/不同 DPI | UI 表格列、详情面板、风险颜色和导出入口不丢失。 |
| 多桌面 | 默认桌面/安全桌面/虚拟桌面 | 窗口/热键/桌面相关证据不误判，不阻塞刷新。 |
| 高 DPI/缩放 | 100%/150%/混合缩放 | cross-view 差异、tooltip、详情文本可读。 |

### 4.4 网络、过滤链与文件系统环境

| 维度 | 组合 | 验收关注点 |
| --- | --- | --- |
| VPN | 无 VPN/主流 VPN 已连接 | WFP/NDIS/LSP 相关 evidence 不崩溃，归属和 unknown 区分清楚。 |
| WFP | 默认防火墙/第三方 callout | callout/filter/provider 只读枚举、风险颜色、签名归因。 |
| NDIS filter | 无第三方/第三方 filter | filter stack 顺序、模块归属、partial 状态。 |
| Minifilter | 单实例/多实例/第三方安全软件 | altitude、实例、回调归属和绕过状态只读展示。 |
| 文件系统 | NTFS 普通卷/BitLocker 卷/网络路径 | 文件与 section evidence 不触发破坏性操作。 |

## 5. UI 决策输入模板

每个功能页进入 UI 设计前，专题会话必须提交以下模板。总控只规定模板，不填具体字段。

### 5.1 功能页元信息

| 项 | 内容 |
| --- | --- |
| 功能页名称 | 例如 Process Cross-View、Driver Integrity、Kernel Memory Evidence、Filter Chain Audit。 |
| 数据来源 | R3 public API、R0 public walk、PDB/DynData private fields、SystemInformation、disk image、signature/trust。 |
| 依赖 capability | 列出 required capability mask 和 optional capability mask。 |
| 无 profile 行为 | disabled、fallback、partial 或 public-only。 |
| 刷新模型 | 手动刷新、定时刷新、长任务、可取消任务。 |
| 导出格式 | JSON/CSV/TSV/screenshot，字段语义必须与表格一致。 |

### 5.2 表格设计输入

每个功能页至少定义：

- 主表：对象列表或 evidence rows。
- 来源表：source mask、采集路径、capability、profile status。
- 风险表：risk flags、confidence、lastStatus、detail。
- 差异表：R3/R0/public/private/cross-view 的差异，只显示事实差异，不直接等同恶意。
- 审计表：仅对 mutation/audit 或安全策略相关页面展示事务和确认记录。

### 5.3 详情面板字段模板

详情面板必须包含：

- 对象身份：名称、PID/TID/地址/模块/路径等稳定标识。
- 证据来源：哪些来源命中、哪些来源缺失、缺失原因。
- Profile 状态：exact/compatible/fallback/blocked、coverage、missing required/optional。
- 风险解释：risk flags 到自然语言说明的映射。
- 操作边界：只读、需要高级模式、需要 mutation/audit、当前禁用原因。
- 原始诊断：status、NTSTATUS、lastStatus、confidence、detail。

### 5.4 风险颜色

| 风险等级 | 颜色语义 | 使用条件 |
| --- | --- | --- |
| 绿色 | clean/normal | 多来源一致，profile exact，未发现异常。 |
| 蓝色 | informational | 仅信息提示，不代表风险。 |
| 黄色 | partial/unknown | 数据不完整、profile fallback、capability 缺失、timeout 或 budget exhausted。 |
| 橙色 | suspicious | cross-view 差异、模块归属异常、签名/路径/权限异常，但仍需人工判断。 |
| 红色 | high risk | 明确危险写能力、已确认异常 hook/callback/filter 或关键对象被隐藏/篡改。 |
| 灰色 | unsupported | 当前 build、profile、驱动能力或权限不支持。 |

UI 禁止把 `unknown/partial/unsupported` 渲染成绿色；也禁止把单一来源差异直接渲染成“已感染”。

### 5.5 Cross-view 差异展示

- 差异展示必须同时显示 source mask 和缺失来源，例如 public-only、private-only、CID-only、active-list-only。
- 差异结论使用“证据差异”措辞，不直接使用“隐藏木马”等定性。
- 同一对象跨页展示时，风险颜色和 anomaly flags 必须一致。
- 支持按差异类型过滤、按 confidence 排序，并能导出原始证据。

## 6. 后续集成分阶段

### Phase A：profile/schema

目标：稳定 PDB profile 生产、校验、pack、coverage 报告和 capability 映射。

准入：

- schema 区分 required/optional、StructOffset/GlobalRva、profile identity、missingFields/missingGlobals。
- profile 生成器输出 coveragePercent 和 diagnostics。
- R3 loader 能拒绝错配 profile，并把状态传递给 R0 DynData/status。
- 不引入任何 R0 行为变更。

退出：

- 代表性 Win10/Win11 build 有 profile pack 或明确 fallback 记录。
- 字段覆盖率报告可被 UI 和测试使用。

### Phase B：R0 read-only protocols

目标：将审计功能以只读协议进入 `shared/driver/` 与 R0 handler/backend。

准入：

- 每个 IOCTL 有协议版本、request/response、rowSize、status、lastStatus、source/risk/confidence 字段。
- 私有字段读取必须 capability gated。
- 所有遍历有默认上限、硬上限、loop detection 和 partial 状态。

退出：

- handler 通过 `ioctl_registry.c` 注册。
- 不修改 `ioctl_dispatch.c` 承载业务 switch。
- 无 profile 和错配 profile 测试不崩溃并正确降级。

### Phase C：R3 loader/UI

目标：通过 ArkDriverClient 接入查询，并在 UI 以一致模板展示。

准入：

- Dock 不直接调用 `DeviceIoControl`。
- ArkDriverClient 返回 unsupported、partial、capability missing、profile blocked 等状态。
- UI 表格、详情、导出字段使用统一字段字典。

退出：

- 各功能页完成刷新、取消、导出、错误提示和无 profile 展示。
- UI 不提供默认 patch/delete/bypass 操作。

### Phase D：cross-view/risk scoring

目标：跨进程、线程、驱动、回调、过滤链、内存证据聚合风险。

准入：

- 风险规则只消费 evidence，不直接触发写操作。
- risk score 可解释，能展开到来源、字段、profile 状态和具体差异。
- 单一来源缺失只能标记 partial/unknown 或 suspicious，不能自动判定 critical。

退出：

- Risk Center 聚合视图和各功能页颜色一致。
- JSON/CSV 导出可复现同一 risk score。

### Phase E：release validation

目标：发布候选验收和回归。

准入：

- IOCTL 静态审计通过：shared 定义、registry 注册、function id、method/access、mutating access 风险复查。
- MSBuild/WDK/Qt 构建流程完成或记录无法构建驱动时沿用产物的理由。
- 测试矩阵关键组合完成。

退出：

- 7z 包结构正确，`Release\` 根目录完整。
- Profile pack、UI、driver、ArkDriverClient 版本匹配。
- 崩溃、hang、错配 profile 继续私有读取、默认危险写入口均为发布阻断项。

## 7. 多会话文档合并为总设计文档

### 7.1 文档角色划分

- 总控文档：本文，保留验收标准、风险边界、测试矩阵、UI 模板、阶段计划和合并流程。
- Profile/schema 专题：维护 PDB 解析、pack schema、coverage、identity、capability 映射。
- R0 evidence 专题：维护各功能只读协议、遍历策略、status/risk/source/confidence 语义。
- R3/UI 专题：维护 ArkDriverClient、页面布局、字段字典、导出格式、错误提示。
- Mutation/audit 专题：维护写操作事务、安全策略、审计记录和高级模式。
- Release/validation 专题：维护构建、打包、测试矩阵执行记录和发布阻断清单。

### 7.2 合并顺序

1. 先合并 profile/schema，锁定字段命名、required/optional、capability mask。
2. 再合并 R0 read-only protocol，确认所有私有读取都受 profile/capability gate 约束。
3. 再合并 R3 loader/UI，确认 UI 字段只消费协议字段，不反向定义 R0 语义。
4. 再合并 cross-view/risk scoring，统一风险颜色和 risk score 解释。
5. 最后合并 mutation/audit 和 release validation，确保危险操作与发布门禁独立审查。

### 7.3 合并检查清单

每个会话文档进入总设计前必须回答：

- 依赖哪些 profile fields 和 capability？缺失时如何降级？
- 是否有 R0 写操作？如果有，是否已经进入 mutation/audit，而不是审计 IOCTL？
- 是否有遍历/扫描上限、loop detection、timeout 和 partial status？
- UI 是否区分 clean、unknown、partial、unsupported、suspicious、high risk？
- 是否提供测试矩阵覆盖和未覆盖说明？
- 是否影响 `shared/driver/`、`ioctl_registry.c`、ArkDriverClient、Dock UI 或 Release pack？

### 7.4 最终总设计文档结构建议

最终总设计文档建议按以下目录组织：

1. 背景与目标。
2. 总体架构：PDB profile -> R3 loader -> DynData/capability -> R0 read-only evidence -> ArkDriverClient -> UI/Risk Center。
3. Profile/schema 规范。
4. R0 安全模型与只读协议规范。
5. 各功能域 evidence 设计索引。
6. UI 字段字典与风险颜色规范。
7. Mutation/audit 规范。
8. 测试矩阵与发布验收。
9. 已知风险、阻断项和延期项。
10. 附录：IOCTL 清单、capability 清单、profile coverage 报告、测试记录索引。

### 7.5 决策记录要求

- 任何从只读审计升级为写操作的决策，必须有单独 ADR，写明威胁、回滚、审计、默认禁用理由。
- 任何 profile 兼容匹配策略放宽，必须有 build 样本和字段 diff 证据。
- 任何 UI 风险颜色规则调整，必须同步更新字段字典、Risk Center 和导出说明。
- 任何发布阻断项豁免，必须写明影响范围、补偿控制和复查日期。
