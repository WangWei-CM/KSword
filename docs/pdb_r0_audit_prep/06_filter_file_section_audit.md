# PDB R0 审计准备：Filter / FileObject / Section / ControlArea

## 0. 范围与原则

本文只设计后续基于 PDB/DynData 的 R0 只读审计能力，覆盖 fltMgr、fileinfo、filecrypt、bindflt、cldflt、luafv、prjflt、wcifs，以及 FileObject、Section、ControlArea 关联关系。本文不是实现文档，不改变当前 IOCTL 协议，不引入写内核链表、卸载、绕过或修复动作。

### 0.1 优先级定义

| 优先级 | 含义 | 默认动作边界 |
| --- | --- | --- |
| P0 | MVP 必须具备，优先保证稳定、只读、可验收 | 只读采集与 cross-view 对照，不做内核状态修改 |
| P1 | 审计增强，依赖更多 PDB 结构确认或特殊模块适配 | 只读为主，可输出风险判断；不做自动动作 |
| P2 | 深度检测、启发式分析、后续运维动作入口 | 默认只读；若未来需要动作，必须单独设计权限、确认和回滚 |

### 0.2 只读与后续动作边界

- 只读范围：枚举 Filter、Instance、Volume 绑定、altitude、operation callbacks、FileObject 字段、SectionObjectPointers、ControlArea、映射进程与地址区间、特定 filter 模块的注册/回调/状态信息。
- 后续才可能做动作：卸载 filter、断开 Instance、修改 bypass PID、清理链表、替换 callback、解除文件占用、关闭句柄、解除映射。此类动作不属于本文 MVP，必须另开设计并要求显式确认。
- R3 不应把任意内核地址作为可信输入。内核对象地址可作为诊断输出字段；需要反查时应优先由 R0 从路径、PID、句柄、枚举结果等可信上下文重新定位。

## 1. 现有能力基线

| 领域 | 已有基础 | 本文增强方向 |
| --- | --- | --- |
| Minifilter 枚举 | 已有 fltMgr 公共 API 枚举路径：`FltEnumerateFilters` / `FltGetFilterInformation`；已有 `KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER` 和 PDB profile 来源标识 | 增加 fltMgr API 与 PDB 私有链表 cross-view，补齐 Instance、Volume、callback 归属 |
| 文件信息 | 现有查询可输出 `fileObjectAddress`、`sectionObjectPointersAddress`、data/image section 地址、对象名 | 补齐 `DeviceObject`、`Vpb`、`FsContext`、`FsContext2`、共享访问与删除状态 |
| 文件监控 | 现有 minifilter 实时事件包含 major/minor、PID/TID、访问参数、`fileObjectAddress`、路径和状态 | 作为审计结果的事件侧证，不替代静态对象审计 |
| Section | 已有按 PID 查询进程 Section、按路径查询文件映射；ControlArea 地址仅诊断输出 | 增强 FileObject -> SectionObjectPointers -> ControlArea -> VAD/进程 cross-view |
| OpenArk 对照 | TODO 中存在 ObjectSections、过滤链枚举、文件占用、DriverObject/IRP Hook 检查缺口 | 本文覆盖过滤链与 FileObject/Section/ControlArea 的准备设计 |

## 2. PDB 模块矩阵

| PDB 模块 | 目标 | 候选结构/符号 | 优先级 | 备注 |
| --- | --- | --- | --- | --- |
| `fltMgr.pdb` | Minifilter 核心枚举、Filter/Instance/Volume/callback cross-view | `_FLT_FILTER`、`_FLT_INSTANCE`、`_FLT_VOLUME`、operation registration/callback node 相关私有结构 | P0/P1 | P0 先与公共 API 对照；P1 再稳定解析私有链表 |
| `ntkrnlmp.pdb` | FileObject、DeviceObject、VPB、SectionObjectPointers、ControlArea、VAD | `_FILE_OBJECT`、`_DEVICE_OBJECT`、`_VPB`、`_SECTION_OBJECT_POINTERS`、`_SECTION`、`_CONTROL_AREA`、VAD 相关结构 | P0/P1 | 结构随版本变化，必须由 DynData/PDB gating |
| `fileinfo.pdb` | FileInfo minifilter 注册、callback 归属、altitude 和实例状态 | 模块导出、注册上下文、callback 函数符号 | P1 | 作为标准系统 filter 基线 |
| `filecrypt.pdb` | EFS/加密相关过滤行为与 callback 归属 | 模块私有上下文、callback 函数符号 | P1 | 只做识别和链路解释，不解密、不修改策略 |
| `cldflt.pdb` | Cloud Files filter 状态、占位文件相关路径 | callback 函数符号、实例上下文、stream/file context 候选结构 | P1/P2 | P2 可做云占位状态解释 |
| `bindflt.pdb` | 绑定/重定向 filter 状态与 callback 归属 | callback 函数符号、实例/卷上下文候选结构 | P1 | 关注路径重定向、容器/包相关场景 |
| `luafv.pdb` | UAC virtualization filter 状态 | callback 函数符号、实例上下文候选结构 | P1 | 关注虚拟化路径和权限侧证 |
| `prjflt.pdb` | Projected File System filter 状态 | callback 函数符号、provider/instance context 候选结构 | P1/P2 | P2 可关联 provider 视图 |
| `wcifs.pdb` | Windows Container Isolation FS filter 状态 | callback 函数符号、隔离上下文候选结构 | P1/P2 | 关注容器隔离路径 cross-view |

## 3. Minifilter 审计设计

### 3.1 P0：Minifilter 表

| 项目 | PDB 模块 | 候选结构 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| Filter 列表 | `fltMgr.pdb` | `_FLT_FILTER`，公共 API filter information | `filterAddress`、`filterName`、`driverObject`、`driverImageBase`、`driverImageSize`、`moduleName`、`sourceFlags` | Filter 名称、驱动模块、地址、来源 | 公共 API 枚举与 PDB cross-view 至少能互相标记同名 filter；缺失项显示来源差异 |
| altitude | `fltMgr.pdb` | `_FLT_FILTER` / Instance registration 中 altitude 字符串或公共信息 | `altitude`、`altitudeSource`、`parseStatus` | Altitude、排序 | UI 按 altitude 稳定排序；解析失败不导致整表失败 |
| operation callbacks | `fltMgr.pdb` + filter 模块 PDB | operation registration/callback node | `majorFunction`、`operationName`、`preCallback`、`postCallback`、`callbackFlags` | 操作类型、Pre、Post、标志 | CREATE/READ/WRITE/SET_INFORMATION/FSCTL/CLEANUP/CLOSE 至少能显示已有注册项 |
| callback 地址归属 | `ntkrnlmp.pdb` + 各 filter PDB | 模块地址范围、符号表 | `preOwnerModule`、`preSymbol`、`postOwnerModule`、`postSymbol`、`ownerTrust` | 回调所属模块、符号、异常标记 | callback 地址落在注册 filter 驱动模块内为正常；落在未知模块或非映像内标红 |

### 3.2 P0：Instance 与 Volume 绑定

| 项目 | PDB 模块 | 候选结构 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| Instance 列表 | `fltMgr.pdb` | `_FLT_INSTANCE` | `instanceAddress`、`filterAddress`、`instanceName`、`altitude`、`flags`、`state` | Instance、Filter、Altitude、状态 | 每个 Filter 可展开看到 Instance；空 Instance 不崩溃 |
| Volume 绑定 | `fltMgr.pdb` | `_FLT_VOLUME`、Instance->Volume 引用 | `volumeAddress`、`volumeName`、`deviceObject`、`diskDeviceObject`、`fileSystemType` | 卷名、设备对象、文件系统 | 同一卷上多个 Instance 能按 altitude 显示；离线卷/失败卷显示状态 |
| fltMgr API vs 内核链表 cross-view | `fltMgr.pdb` | fltMgr 全局 filter/volume/instance 链 | `apiPresent`、`pdbPresent`、`crossViewState`、`reason` | 来源对照、异常原因 | API 有而链表无、链表有而 API 无均显示差异，不自动修复 |

### 3.3 P1/P2：异常判断与后续动作边界

| 项目 | 优先级 | R0 输出字段 | UI 字段 | 只读/动作边界 |
| --- | --- | --- | --- | --- |
| 隐藏 Filter/Instance | P1 | `crossViewState=ApiOnly/PdbOnly/Mismatch`、`missingReason` | Cross-view 异常 | 只读告警；不 unlink、不 relink |
| Callback 越界/跳板 | P1 | `callbackRangeState`、`firstBytesHash`、`moduleRange` | 回调异常 | 只读告警；不 patch |
| Public unload 入口 | P2 | `canUnloadByPublicApi`、`filterName` | 可卸载提示 | 后续动作；必须使用公共 `FltUnloadFilter`，不属于 MVP |
| 私有链表摘除/修复 | P2 | 不在 MVP 输出 | 不提供默认按钮 | 高风险后续项；本文明确不实现 |

## 4. FileObject 增强设计

### 4.1 P0：FileObject 详情

| 项目 | PDB 模块 | 候选结构 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| FileObject 地址 | `ntkrnlmp.pdb` | `_FILE_OBJECT` | `fileObjectAddress`、`objectName`、`typeCheckState` | FileObject、对象名 | 与现有文件查询输出兼容；地址仅诊断显示 |
| DeviceObject / Vpb | `ntkrnlmp.pdb` | `_FILE_OBJECT.DeviceObject`、`_DEVICE_OBJECT`、`_VPB` | `deviceObjectAddress`、`deviceName`、`vpbAddress`、`volumeLabel`、`serialNumber` | 设备对象、卷、卷标 | 普通 NTFS 文件能显示 DeviceObject；无 VPB 时显示空状态 |
| FsContext / FsContext2 | `ntkrnlmp.pdb` + 文件系统上下文仅做地址 | `_FILE_OBJECT.FsContext`、`FsContext2` | `fsContext`、`fsContext2`、`fsContextOwnerHint` | FsContext、FsContext2 | 不强行解析 NTFS/其他 FS 私有结构；仅输出地址和所属模块提示 |
| SectionObjectPointers | `ntkrnlmp.pdb` | `_SECTION_OBJECT_POINTERS` | `sectionObjectPointersAddress`、`dataSectionObject`、`sharedCacheMap`、`imageSectionObject` | SectionObjectPointers、Data/Image/Cache | 与现有 file section 查询结果一致；空指针正常显示 |
| DeletePending / ShareAccess | `ntkrnlmp.pdb` | `_FILE_OBJECT` 标志位、share access 字段 | `deletePending`、`sharedRead`、`sharedWrite`、`sharedDelete`、`readAccess`、`writeAccess`、`deleteAccess` | 删除挂起、共享读/写/删、访问状态 | 正常打开文件可显示共享状态；字段缺失时输出 `offsetUnavailable` |

### 4.2 P1：FileObject cross-view

| 项目 | PDB 模块 | 候选结构 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| FileObject -> Handle cross-view | `ntkrnlmp.pdb` | 进程句柄表、Object header | `pid`、`handle`、`grantedAccess`、`fileObjectAddress` | 持有进程/句柄 | 同一 FileObject 可列出至少当前可枚举的持有句柄 |
| FileObject -> Minifilter 事件侧证 | `fltMgr.pdb` + 现有事件 | 实时事件中的 `fileObjectAddress` | `lastEvents[]`、`eventTime`、`majorFunction`、`status` | 最近事件 | 只读关联，不依赖实时监控开启作为静态审计前提 |
| FileObject -> Section 映射 | `ntkrnlmp.pdb` | SectionObjectPointers / ControlArea | `mappingSummary`、`mappedProcessCount` | 映射摘要 | 能从 FileObject 详情跳转 Section 映射视图 |

## 5. Section / ControlArea 审计设计

### 5.1 P0：Section 映射 MVP

| 项目 | PDB 模块 | 候选结构 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| 文件映射入口 | `ntkrnlmp.pdb` | `_SECTION_OBJECT_POINTERS` | `filePath`、`fileObjectAddress`、`sectionObjectPointersAddress` | 文件路径、FileObject、SOP | 输入路径后能返回 data/image section 基础信息 |
| image/data section | `ntkrnlmp.pdb` | `_SECTION`、`_CONTROL_AREA` | `dataSectionObject`、`imageSectionObject`、`dataControlArea`、`imageControlArea`、`sectionKind` | Data/Image Section、ControlArea | DLL/EXE 映射能显示 image section；普通映射文件能显示 data section |
| 控制区引用 | `ntkrnlmp.pdb` | `_CONTROL_AREA` | `numberOfSectionReferences`、`numberOfMappedViews`、`numberOfUserReferences`、`flags` | Section 引用、MappedViews、UserRefs、Flags | 引用字段不可用时降级显示地址和 offset 状态 |
| 映射进程 cross-view | `ntkrnlmp.pdb` | ControlArea VAD list、进程 VAD | `pid`、`processName`、`startVa`、`endVa`、`protection`、`sectionKind` | PID、进程名、VA 范围、保护 | 给定已映射文件能列出至少一个映射进程；无映射时返回空列表而非失败 |

### 5.2 P1/P2：深度 Section 审计

| 项目 | 优先级 | PDB 模块 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| 文件路径到映射进程 cross-view | P1 | `ntkrnlmp.pdb` | `pathOpenState`、`fileObjectAddress`、`controlAreas[]`、`mappedProcesses[]` | 路径、映射进程 | 路径查询与进程反查结果可互相跳转 |
| 进程模块/VAD 到 ControlArea 反查 | P1 | `ntkrnlmp.pdb` | `pid`、`vadRange`、`controlArea`、`fileObject`、`filePath` | VAD、ControlArea、文件 | 给定 PID 能关联到文件映射列表 |
| 异常映射检测 | P2 | `ntkrnlmp.pdb` | `imageWithoutFile`、`deletedFileMapped`、`writeableImageMapping` | 异常标签 | 只读告警；不解除映射、不关闭句柄 |
| ControlArea 生命周期解释 | P2 | `ntkrnlmp.pdb` | `flagsDecoded`、`referenceSummary`、`flushStateHint` | 控制区状态说明 | 输出解释必须带 offset/profile 版本，避免跨版本误判 |

## 6. 特定 Filter 模块审计设计

### 6.1 模块通用输出

所有特定 filter 均先复用 Minifilter 表、Instance、Volume、callback 归属结果，再增加模块专属解释字段。专属解释字段缺失时不影响通用审计表。

| 项目 | PDB 模块 | 候选结构 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| 模块存在性 | 对应模块 PDB + loaded module list | 模块基址/大小/符号 | `moduleLoaded`、`imageBase`、`imageSize`、`pdbMatched` | 已加载、PDB 状态 | 未加载模块显示 NotLoaded，不报错 |
| Filter 注册归属 | `fltMgr.pdb` + 对应模块 PDB | `_FLT_FILTER`、callback 函数符号 | `filterName`、`altitude`、`callbacks[]` | Filter、Altitude、Callbacks | callback owner 应落在对应模块范围内 |
| Instance/Volume 状态 | `fltMgr.pdb` | `_FLT_INSTANCE`、`_FLT_VOLUME` | `instances[]`、`volumes[]`、`state` | 实例、卷绑定 | 模块无 Instance 时显示空状态 |

### 6.2 单模块重点

| 模块 | 优先级 | 重点审计 | R0 输出字段 | UI 字段 | 验收方式 |
| --- | --- | --- | --- | --- | --- |
| `fileinfo` | P1 | 标准文件信息 filter 基线、callback 是否被篡改 | `fileinfoFilterPresent`、`callbackOwnerState`、`altitude` | FileInfo 状态、回调归属 | 正常系统上能识别 fileinfo filter 和 callback 模块归属 |
| `filecrypt` | P1 | EFS/加密路径相关 callback、实例绑定 | `filecryptPresent`、`operationMask`、`volumeBindings` | FileCrypt 状态、操作掩码 | 只显示加密 filter 状态，不读取/修改加密内容 |
| `cldflt` | P1/P2 | Cloud Files 占位文件 filter、云文件相关操作 | `cldfltPresent`、`placeholderHint`、`callbacks[]` | Cloud Filter、占位提示 | P1 识别回调；P2 才解释占位状态 |
| `bindflt` | P1 | 路径绑定/重定向相关 filter | `bindfltPresent`、`bindingsHint`、`callbacks[]` | Bind Filter、绑定提示 | 能区分 filter 未加载、已加载无实例、已绑定卷 |
| `luafv` | P1 | UAC 虚拟化 filter、虚拟化路径侧证 | `luafvPresent`、`virtualizationHint`、`callbacks[]` | LUAFV 状态、虚拟化提示 | 只读解释，不启用/禁用虚拟化 |
| `prjflt` | P1/P2 | Projected File System provider/instance 关联 | `prjfltPresent`、`providerHint`、`callbacks[]` | PrjFlt 状态、Provider 提示 | P1 显示 filter；P2 关联 provider 上下文 |
| `wcifs` | P1/P2 | 容器隔离文件系统 filter、隔离路径侧证 | `wcifsPresent`、`isolationHint`、`callbacks[]` | WCIFS 状态、隔离提示 | P1 显示绑定；P2 解释容器隔离上下文 |

## 7. R0 输出结构建议

### 7.1 Minifilter 表输出

- `profileVersion`：PDB/DynData profile 版本。
- `enumerationTime`：采集时间。
- `filters[]`：每个 filter 包含 `filterAddress`、`name`、`altitude`、`driverObject`、`module`、`apiPresent`、`pdbPresent`、`instances[]`、`operations[]`、`crossViewState`。
- `instances[]`：每个 instance 包含 `instanceAddress`、`filterAddress`、`name`、`altitude`、`volumeAddress`、`volumeName`、`state`、`flags`。
- `operations[]`：每项包含 `majorFunction`、`preCallback`、`postCallback`、`preOwnerModule`、`postOwnerModule`、`symbolName`、`rangeState`。
- `errors[]`：offset 缺失、PDB 不匹配、结构校验失败等非致命错误。

### 7.2 FileObject 详情输出

- `fileObjectAddress`、`objectName`、`deviceObjectAddress`、`deviceName`、`vpbAddress`、`volumeLabel`。
- `fsContext`、`fsContext2`、`sectionObjectPointersAddress`、`dataSectionObject`、`imageSectionObject`、`sharedCacheMap`。
- `deletePending`、`sharedRead`、`sharedWrite`、`sharedDelete`、`readAccess`、`writeAccess`、`deleteAccess`。
- `offsetStatus[]`：每个字段的 profile 命中状态，避免版本漂移导致误读。

### 7.3 Section 映射输出

- `filePath`、`fileObjectAddress`、`sectionObjectPointersAddress`。
- `sections[]`：`sectionKind`、`sectionObject`、`controlArea`、`segment`、`flags`、引用计数摘要。
- `mappedProcesses[]`：`pid`、`processName`、`startVa`、`endVa`、`protection`、`sectionKind`、`vadAddress`。
- `crossView[]`：路径查询、FileObject 查询、PID/VAD 反查之间的一致性状态。

## 8. UI 字段与交互建议

### 8.1 P0 页面

| 页面 | 表格/面板 | 字段 |
| --- | --- | --- |
| Minifilter 表 | 主表 | Filter、Altitude、模块、Instance 数、Volume 数、Callback 数、Cross-view 状态 |
| Minifilter 表 | 详情 | Filter 地址、DriverObject、每个 Instance、每个 Volume、operation callback 与 owner |
| FileObject 详情 | 基础信息 | 路径、FileObject、DeviceObject、VPB、FsContext/FsContext2 |
| FileObject 详情 | SectionObjectPointers | Data Section、Image Section、SharedCacheMap、DeletePending、ShareAccess |
| Section 映射 | 映射表 | SectionKind、ControlArea、PID、进程名、VA 范围、保护属性 |

### 8.2 状态呈现

- `OK`：公共 API 与 PDB cross-view 一致，callback 地址归属正常。
- `MissingOffset`：当前系统 profile 不支持某字段，表格保留行但字段置空。
- `ApiOnly`：公共 API 可见但 PDB 私有链未确认。
- `PdbOnly`：PDB 私有链可见但公共 API 不可见，需标记为高风险只读告警。
- `OwnerMismatch`：callback 地址不在声明 filter 模块范围内。
- `Unreadable`：目标对象页不可安全读取，必须跳过并记录错误。

## 9. MVP 切分

### 9.1 MVP 必做 P0

1. Minifilter 表：
   - 公共 API 枚举结果。
   - PDB 私有链表只读 cross-view。
   - Filter/Instance/Volume/altitude/operation callback/pre-post owner。
2. FileObject 详情：
   - FileObject、DeviceObject、VPB、FsContext/FsContext2。
   - SectionObjectPointers、Data/Image Section、DeletePending、ShareAccess。
3. Section 映射：
   - 文件路径 -> FileObject -> SectionObjectPointers -> ControlArea。
   - Data/Image section 区分。
   - ControlArea -> 映射进程、VA 范围 cross-view。

### 9.2 MVP 明确不做

- 不卸载 filter。
- 不断开 Instance 或修改 Volume 绑定。
- 不修改 callback 指针。
- 不清理、摘除或修复 fltMgr 私有链表。
- 不关闭文件句柄、不解除 Section 映射、不修改 ControlArea。
- 不接受 R3 传入任意内核对象地址作为操作目标。

## 10. 验收清单

| 验收项 | 优先级 | 验收标准 |
| --- | --- | --- |
| Minifilter 表可见 | P0 | 能显示系统 filter 列表、altitude、模块、Instance 数和 Volume 绑定 |
| Callback 归属 | P0 | 每个 operation callback 显示 pre/post 地址及 owner module；未知归属不崩溃 |
| fltMgr cross-view | P0 | 公共 API 与 PDB 视图均可输出，并能显示一致/差异状态 |
| FileObject 详情 | P0 | 给定文件路径可显示 FileObject、DeviceObject、VPB、FsContext/FsContext2、SectionObjectPointers |
| Share/Delete 状态 | P0 | 显示 DeletePending、SharedRead、SharedWrite、SharedDelete，offset 缺失有明确状态 |
| Section 映射 | P0 | 给定已映射文件可显示 data/image section、ControlArea、映射进程和 VA 范围 |
| 特定 filter 识别 | P1 | 能识别 fileinfo/filecrypt/bindflt/cldflt/luafv/prjflt/wcifs 是否加载、是否有 filter/instance/callback |
| 特定 filter 解释 | P2 | 对云文件、投影文件系统、容器隔离等输出只读解释，不影响 P0 表格 |

## 11. 风险与防御性要求

- 所有 PDB offset 必须绑定模块 GUID/age 或等价 profile 标识；不匹配时禁止读取私有字段。
- 读取链表必须有最大节点数、环检测、地址范围检查和异常保护，避免坏链表导致卡死。
- 对 FileObject、Section、ControlArea 的引用只做短时安全引用或受保护读取；失败返回部分结果和错误码。
- UI 不能把诊断地址当作可执行动作入口；任何未来动作必须重新验证对象身份和生命周期。
- 特定 filter 模块的私有结构解释必须可选；PDB 缺失时仍保留通用 Minifilter 审计能力。
