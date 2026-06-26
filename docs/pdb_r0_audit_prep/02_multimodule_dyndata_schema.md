# DynData v4 多模块 PDB profile schema 预研

## 结论先行

当前 DynData v1/v2/v3 已经能支撑 `ntoskrnl.exe` / `ntkrnlmp.exe` / `ntkrla57.exe` 的结构偏移、部分全局 RVA、callback 相关 RVA/结构偏移下发，并且 R0 侧具备基本的 capability 派生、字段查询、profile 精确匹配和 copy-on-success 应用机制。但是现有设计仍然是“单主模块 ntoskrnl 视角”，不能稳定覆盖 GUI、网络、文件过滤、BitLocker 等跨模块 R0 审计场景。

建议新增并行存在的 profile v4，而不是扩展 v3 原语义：v4 应以“pack -> moduleProfiles[] -> items[]”为核心，按模块身份精确匹配已加载模块，并以 capability group 为单位向 R0 暴露可用性。v4 必须继续由 R3 解析 JSON/PDB 产物，R0 只接收已验证的紧凑二进制请求；R0 不解析 JSON、不读取 PDB、不猜测跨版本字段。

v4 的关键变化：

1. 从单 `ntoskrnl` profile 扩展为多模块 profile，覆盖 `ntoskrnl`、`win32k`、`win32kbase`、`win32kfull`、`tcpip`、`ndis`、`netio`、`fltMgr`、`fvevol` 等。
2. 每个模块 profile 独立携带完整 identity：module name、image timestamp、image size、machine、pdb name、pdb guid、pdb age、module class id。
3. item 类型从 v3 的 `StructOffset` / `GlobalRva` 扩展到：`StructOffset`、`GlobalRva`、`FunctionRva`、`EnumValue`、`TypeSize`、`BitField`、`ListHeadGlobal`。
4. pack 文件不应再依赖单个 profile 内 `KSW_DYN_PROFILE_EX_MAX_ITEMS=256` 的硬上限。v4 要么分模块分块下发，要么引入 section/stream 传输；R0 应按模块独立应用，避免一个巨型请求失败导致所有模块不可用。
5. 兼容路线应保持 v3 完全不破坏：v3 继续服务现有功能，v4 并存用于稳定 R0 审计；UI 要能展示每个模块 profile 的命中/缺失/降级状态。

## 当前 DynData v1/v2/v3 支持范围审计

### 协议和 R0 状态面

只读审计到的当前事实：

- 共享协议位于 `shared/driver/KswordArkDynDataIoctl.h`。
- 当前 `KSWORD_ARK_DYNDATA_PROTOCOL_VERSION` 仍为 `1`。
- 已有 IOCTL：
  - `IOCTL_KSWORD_ARK_QUERY_DYN_STATUS`
  - `IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS`
  - `IOCTL_KSWORD_ARK_QUERY_CAPABILITIES`
  - `IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE`
  - `IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_EX`
- 当前公开状态只固定包含 `ntoskrnl` 与 `lxcore` identity；profile apply / profile EX apply 实际按 `candidateState.Ntoskrnl` 精确匹配。
- 当前模块 class 只有：
  - `KSW_DYN_PROFILE_CLASS_NTOSKRNL = 0`
  - `KSW_DYN_PROFILE_CLASS_NTKRLA57 = 1`
  - `KSW_DYN_PROFILE_CLASS_LXCORE = 2`
- 当前 `KSW_DYN_PROFILE_EX_MAX_ITEMS = 256`。
- 当前 `KSW_DYN_PROFILE_MAX_FIELDS = 128`。
- 当前 `KSW_DYN_PROFILE_OFFSET_MAX = 0x0000FFFF`，结构偏移仍偏向 16-bit safe range。
- 当前 `KSW_DYN_PROFILE_GLOBAL_RVA_MAX = 0x7FFFFFFF`。

### v1：fields-only profile

v1 的有效形态是：

- pack 顶层：`schemaVersion`、`packVersion: 1`、`fieldDictionary`、`profiles`。
- 每个 profile 以 `moduleClassId`、`machine`、`timeDateStamp`、`sizeOfImage` 精确匹配模块。
- `fields` 是 `[fieldDictionaryIndex, offset]` 数组。
- R3 将 JSON 转成 `KSW_APPLY_DYN_PROFILE_REQUEST`。
- R0 只接受 `fieldId + offset`，只应用已知 `KSW_DYN_FIELD_ID_*`。
- 适合现有 `ntoskrnl` 结构偏移补充，但不表达 item kind、全局 RVA、函数 RVA、枚举、类型大小或 bitfield。

局限：

- 单 profile 绑定 `ntoskrnl` identity。
- 无模块数组。
- 无 capability 分组级诊断。
- 缺字段只能体现为字段不存在或 capability 缺失，无法表达精细降级原因。

### v2：callbackItems 扩展

v2 在 v1 基础上新增 `callbackItems`：

- item kind 仅接受 `GlobalRva` / `StructOffset`。
- callback global 支持：
  - `PspCreateProcessNotifyRoutine`
  - `PspCreateThreadNotifyRoutine`
  - `PspLoadImageNotifyRoutine`
  - `PspNotifyEnableMask`
  - `CmCallbackListHead`
- callback struct offset 支持：
  - `_OBJECT_TYPE.CallbackList`
  - `_CALLBACK_ENTRY_ITEM.EntryList`
  - `_CALLBACK_ENTRY_ITEM.PreOperation`
  - `_CALLBACK_ENTRY_ITEM.PostOperation`
  - `_CALLBACK_ENTRY_ITEM.Operations`
  - `_CALLBACK_ENTRY_ITEM.CallbackEntry`
  - `_CALLBACK_ENTRY.Altitude`
  - `_CALLBACK_ENTRY.RegistrationContext`
- R0 EX apply 使用 `KSW_DYN_PROFILE_EX_ITEM_FLAG_CALLBACK` 标识 callback item，并在成功时设置 callback profile active 状态。

局限：

- 仍然是 ntoskrnl 精确匹配。
- 不能描述 win32k/network/filter/crypto 等其它模块。
- 不能描述函数 RVA、枚举值、类型大小、bitfield 等 PDB 审计常用原语。

### v3：typed items 与 kernel globals

v3 在 v1/v2 兼容布局上新增 `items`：

- `items[]` 可承载 `StructOffset` 和 `GlobalRva`。
- 结构偏移覆盖当前 generator 的 `FIELD_MAP`：EPROCESS、ETHREAD、KTHREAD、HANDLE_TABLE、KLDR、DRIVER_OBJECT 等。
- kernel globals 当前支持：
  - `PspCidTable`
  - `PsLoadedModuleList`
  - `MmUnloadedDrivers`
  - `PiDDBCacheTable`
- pack v3 还保留 `coveragePercent`、`missingFields`、`missingGlobals` 供发布侧审计。
- R0 EX apply 对 `GlobalRva` 做非零、`< SizeOfImage`、`<= KSW_DYN_PROFILE_GLOBAL_RVA_MAX` 校验。
- R0 EX apply 对 `StructOffset` 做 `!= KSW_DYN_OFFSET_UNAVAILABLE` 且 `<= KSW_DYN_PROFILE_OFFSET_MAX` 校验。
- R0 apply 是 copy-on-success：任何 rejected/unknown item 都会导致 active state 不变。

对当前 `Ksword5.1\Ksword5.1\profiles\ark_dyndata_pack_v3.json` 的摘要审计：

- `schemaVersion = 1`
- `packVersion = 3`
- `fieldDictionary` 数量：38
- profile 数量：2065
- `moduleClassId` 分布：`0` 共 1533，`1` 共 532
- PDB 名称：`ntkrnlmp.pdb` 与 `ntkrla57.pdb`
- 所有 profile 均有 `items`
- `callbackItems` 在该 pack 中为空
- item kind 仅出现 `StructOffset` 和 `GlobalRva`
- 单 profile 最大 `fields` 数量：38
- 单 profile 最大 `items` 数量：47

v3 局限：

- 仍是 ntoskrnl/ntkrla57-only pack。
- R0 状态结构不是按模块存储；新增模块会迫使继续扩充单体 state，扩展成本高。
- `KSW_DYN_PROFILE_EX_MAX_ITEMS=256` 当前够 v3，但多模块审计会很快不够。例如 GUI + network + filter + storage 每模块几十至上百 item，单请求 256 明显偏紧。
- `StructOffset` 仍以 16-bit offset 上限为主，对大型类型/复杂内核私有类型不一定长期安全。
- 没有 `FunctionRva`、`EnumValue`、`TypeSize`、`BitField`、`ListHeadGlobal`，因此无法完整表达稳定审计所需的函数边界、枚举语义、对象大小和位域位置。

## profile v4 设计目标

v4 不替代 v3，而是新增“多模块 PDB 事实包”。目标是把 PDB 解析出的稳定审计事实，按模块精确绑定到当前已加载内核模块：

- kernel core：`ntoskrnl.exe` / `ntkrnlmp.exe` / `ntkrla57.exe`
- GUI：`win32k.sys`、`win32kbase.sys`、`win32kfull.sys`
- network：`tcpip.sys`、`ndis.sys`、`netio.sys`
- filter / file：`fltMgr.sys`
- storage / encryption：`fvevol.sys`
- 后续可扩展：`afd.sys`、`storport.sys`、`volmgr.sys`、`ci.dll` 等，但不在本期强行纳入。

设计约束：

1. R3 负责 JSON、PDB、pack 解析与名字到 item id 的映射。
2. R0 只接收紧凑结构，按模块 identity 精确匹配。
3. R0 应按 module profile 粒度独立应用，不因某个可选模块缺失影响其它模块。
4. capability 必须从“已匹配模块 + 已应用 item + 必需字段齐备”派生，不直接信任 R3 标志。
5. 缺字段应降级，不应让稳定审计功能误用猜测 offset/RVA。
6. v3 IOCTL、pack、UI 行为保持不破坏。

## module class id 建议

module class id 应成为跨 R3/R0 的稳定枚举，不要直接依赖字符串比较。建议预留范围：

| Class ID | 模块 | 类别 | 说明 |
| ---: | --- | --- | --- |
| 0 | `ntoskrnl.exe` / `ntkrnlmp.exe` | KernelCore | 保持 v3 兼容语义。 |
| 1 | `ntkrla57.exe` | KernelCore | 保持 v3 兼容语义。 |
| 2 | `lxcore.sys` | WSL | 保持现有语义。 |
| 16 | `win32k.sys` | Win32k | GUI subsystem legacy/core split 入口。 |
| 17 | `win32kbase.sys` | Win32k | USER/GDI 基础对象与 shared state。 |
| 18 | `win32kfull.sys` | Win32k | USER/GDI heavy implementation。 |
| 32 | `tcpip.sys` | Network | TCP/IP endpoint、WFP 关联审计。 |
| 33 | `ndis.sys` | Network | miniport/protocol/filter 绑定审计。 |
| 34 | `netio.sys` | Network | NETIO/WFP shared internals。 |
| 48 | `fltMgr.sys` | Filter | minifilter frame/instance/callback 审计。 |
| 64 | `fvevol.sys` | StorageCrypto | BitLocker volume/encryption path 审计。 |

命名建议：R3 pack 使用 lower-case canonical module name；R0 identity resolver 做 case-insensitive basename 匹配，并记录实际加载名。

## profile identity 设计

每个 `moduleProfiles[]` 必须携带完整 identity：

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `moduleName` | string | yes | canonical basename，例如 `tcpip.sys`。 |
| `moduleClassId` | uint32 | yes | 稳定模块类别 ID。 |
| `machine` | uint32 | yes | COFF Machine，例如 x64 为 34404。 |
| `timeDateStamp` | uint32 | yes | PE FileHeader TimeDateStamp。 |
| `sizeOfImage` | uint32 | yes | PE OptionalHeader SizeOfImage；R0 使用加载镜像 PE 头优先。 |
| `pdbName` | string | yes | RSDS PDB 文件名。 |
| `pdbGuid` | string | yes | RSDS GUID，标准带连字符大写/小写均可，R3 规范化。 |
| `pdbAge` | uint32 | yes | RSDS Age。 |
| `profileName` | string | yes | 人类可读 profile key，建议包含模块、版本、guidage。 |
| `imageVersion` | string | optional | 文件版本，仅诊断，不参与 R0 强匹配。 |
| `source` | object | optional | 生成器、symbol server、pdbutil 版本等审计信息。 |

匹配原则：

- R0 必须至少匹配 `moduleClassId + machine + timeDateStamp + sizeOfImage + module basename`。
- `pdbName + pdbGuid + pdbAge` 在 R3 选择 profile 时必须匹配 PE RSDS；R0 可保存/回显，但不能依赖它从内存中重新解析 PDB。
- 若同一模块 class 存在多个候选 profile，R3 必须先按完整 identity 去重；R0 发现重复/多请求冲突时拒绝后来的不一致 profile。
- 对 pageable 或延迟加载模块，R0 应返回 module absent，而不是把 profile 标成失败。

## item 类型设计

v4 item 必须同时支持“紧凑传输”和“可审计 JSON”。建议 JSON 层使用字符串 kind，R3 下发时映射为 numeric kind + numeric item id。

### 1. StructOffset

用途：结构成员字节偏移。

字段：

- `kind: "StructOffset"`
- `name`: 稳定 item 名，例如 `EPROCESS.ActiveProcessLinks` 或 `FLT_INSTANCE.CallbackNodes`
- `typeName`: PDB 类型名，例如 `_EPROCESS`
- `memberPath`: 成员路径，支持匿名 union 展平后的 dotted path
- `value`: byte offset
- `size`: optional，成员大小
- `required`: bool

校验：

- `value != 0xFFFFFFFF`
- `value < typeSize`，如果同 profile 中存在 `TypeSize(typeName)`
- 对指针/list entry/alignment-sensitive 字段，按 machine 做自然对齐检查

### 2. GlobalRva

用途：模块内全局数据符号 RVA。

字段：

- `kind: "GlobalRva"`
- `name`: 符号名或稳定别名
- `symbolName`: PDB/global/public symbol 原名
- `value`: RVA
- `required`: bool

校验：

- `value != 0`
- `value < sizeOfImage`
- RVA 落在允许 section：`.data`、`.rdata`、`.mrdata`、`PAGE` 等按 item policy 配置
- 默认不接受落在 discardable section 的全局 RVA

### 3. FunctionRva

用途：模块内函数入口或内部 helper 的 RVA，用于边界检查、hook 审计、call target 归因。

字段：

- `kind: "FunctionRva"`
- `name`: 稳定函数别名
- `symbolName`: PDB function symbol
- `value`: RVA
- `size`: optional function length
- `required`: bool

校验：

- `value != 0`
- `value < sizeOfImage`
- RVA 必须落在 executable section
- 如提供 `size`，`value + size <= sizeOfImage` 且范围不跨非执行 section
- 对 hotpatch/thunk 函数允许 `flags: ["thunkAllowed"]`，否则应拒绝过短函数

### 4. EnumValue

用途：PDB enum 常量值，例如状态枚举、对象类型枚举、WFP layer/condition 等。

字段：

- `kind: "EnumValue"`
- `name`: 稳定别名
- `enumName`: PDB enum 类型名
- `enumerator`: 枚举项名
- `value`: signed/unsigned 64-bit 建议 JSON 字符串或 number
- `width`: 1/2/4/8
- `required`: bool

校验：

- `width` 必须为 1、2、4、8
- R3 负责符号解析；R0 仅按 item id 存储数值
- 如果 enum 缺失，依赖该 enum 的 parser/audit capability 不置位

### 5. TypeSize

用途：结构/union/class 的 sizeof，用于安全遍历和 offset 边界校验。

字段：

- `kind: "TypeSize"`
- `name`: 稳定别名，例如 `sizeof(_EPROCESS)`
- `typeName`: PDB 类型名
- `value`: size in bytes
- `required`: bool

校验：

- `value > 0`
- `value <= reasonableMax`，按模块/类型配置，例如核心 kernel object 可设 1MB 上限
- 若同模块存在 `StructOffset` 引用该 type，R3 应先验证 offset < TypeSize；R0 可重复轻量验证

### 6. BitField

用途：结构内 bitfield 的 byte offset、bit offset、bit width，例如 flags、state、protection 等。

字段：

- `kind: "BitField"`
- `name`: 稳定别名
- `typeName`: PDB 类型名
- `memberPath`: bitfield 成员路径
- `byteOffset`: byte offset
- `bitOffset`: 0-63
- `bitWidth`: 1-64
- `storageSize`: 1/2/4/8
- `required`: bool

校验：

- `storageSize` 为 1、2、4、8
- `bitOffset + bitWidth <= storageSize * 8`
- `byteOffset + storageSize <= TypeSize(typeName)`，如果 TypeSize 可用
- R0 读取时必须使用掩码，不允许把 bitfield 当完整字段覆盖

### 7. ListHeadGlobal

用途：表达“全局 LIST_ENTRY 头”而不是普通 `GlobalRva`。它语义上要求双向链表安全遍历、entry type 和 link offset。

字段：

- `kind: "ListHeadGlobal"`
- `name`: 稳定别名，例如 `PsLoadedModuleList`、`FltGlobals.FrameList`
- `symbolName`: 全局 LIST_ENTRY 符号或包含 LIST_ENTRY 的全局结构
- `value`: list head RVA
- `entryTypeName`: 链表节点类型
- `linkMemberPath`: 节点中的 LIST_ENTRY 成员路径
- `required`: bool

校验：

- `value != 0`
- `value < sizeOfImage`
- list head RVA 不能落在 executable section
- `linkMemberPath` 必须有对应 `StructOffset`
- R0 遍历必须有最大节点数、地址范围、Flink/Blink 双向一致性和异常保护

## pack v4 文件布局建议

### 顶层布局

```json
{
  "schemaVersion": 2,
  "packVersion": 4,
  "target": {
    "osFamily": "Windows",
    "arch": "amd64"
  },
  "generatedAtUtc": "2026-06-26T00:00:00Z",
  "generator": {
    "name": "ksword_pdb_profile_generator",
    "version": "v4-draft",
    "llvmPdbutilVersion": "optional"
  },
  "moduleClassDictionary": [
    { "id": 0, "name": "ntoskrnl", "canonicalNames": ["ntoskrnl.exe", "ntkrnlmp.exe"] },
    { "id": 16, "name": "win32k", "canonicalNames": ["win32k.sys"] },
    { "id": 32, "name": "tcpip", "canonicalNames": ["tcpip.sys"] }
  ],
  "itemDictionary": [
    {
      "id": 100001,
      "name": "EPROCESS.ActiveProcessLinks",
      "kind": "StructOffset",
      "capabilityGroup": "Kernel.ProcessCrossView",
      "required": true
    }
  ],
  "profileSets": [
    {
      "profileSetId": "win11_26100_amd64_example",
      "modules": [
        {
          "moduleName": "ntoskrnl.exe",
          "moduleClassId": 0,
          "machine": 34404,
          "timeDateStamp": 305419896,
          "sizeOfImage": 31264768,
          "pdbName": "ntkrnlmp.pdb",
          "pdbGuid": "00000000-0000-0000-0000-000000000000",
          "pdbAge": 1,
          "profileName": "ntoskrnl_amd64_26100_example",
          "items": [
            { "id": 100001, "kind": "StructOffset", "value": 1096 },
            { "id": 100101, "kind": "ListHeadGlobal", "value": 1193046, "linkItemId": 100201 }
          ],
          "missingItems": [],
          "coverage": {
            "required": 42,
            "presentRequired": 42,
            "optional": 18,
            "presentOptional": 11
          }
        },
        {
          "moduleName": "tcpip.sys",
          "moduleClassId": 32,
          "machine": 34404,
          "timeDateStamp": 2271560481,
          "sizeOfImage": 3276800,
          "pdbName": "tcpip.pdb",
          "pdbGuid": "11111111-2222-3333-4444-555555555555",
          "pdbAge": 1,
          "profileName": "tcpip_amd64_26100_example",
          "items": [
            { "id": 300001, "kind": "GlobalRva", "value": 745472 },
            { "id": 300101, "kind": "FunctionRva", "value": 131072, "size": 384 }
          ],
          "missingItems": ["TcpEndpoint.PartitionLinks"],
          "coverage": {
            "required": 12,
            "presentRequired": 11,
            "optional": 20,
            "presentOptional": 8
          }
        }
      ]
    }
  ]
}
```

### 为什么不把所有 items 塞进一个 EX 请求

当前 `KSW_DYN_PROFILE_EX_MAX_ITEMS=256` 对 v3 够用，是因为当前 pack 最大单 profile `items` 约 47。v4 多模块后，如果保守估算：

- ntoskrnl：80-150 items
- win32k/win32kbase/win32kfull：每个 40-120 items
- tcpip/ndis/netio：每个 40-120 items
- fltMgr/fvevol：每个 30-80 items

单 OS build 的总 item 很容易超过 500-900。继续用单个 `APPLY_DYN_PROFILE_EX` 请求会遇到：

- 请求过大，buffer 分配和 METHOD_BUFFERED copy 风险上升。
- 单个可选模块缺失导致整包失败的诱惑变大。
- R0 copy-on-success 粒度过粗，诊断困难。

建议二选一：

1. **分模块 APPLY**：新增 v4 module apply IOCTL，每次只下发一个 module profile。每模块 item 上限可暂定 256 或 512。
2. **分块 APPLY + COMMIT**：R3 对同一 module 分 chunk 下发，R0 暂存 staging buffer，最后 commit 时统一校验并应用。

首选方案是分模块 APPLY，简单、稳定、诊断直接。

## R3 下发策略

R3 loader 建议流程：

1. 查询 R0 已加载模块 identity 列表。
2. 对每个已加载模块，根据 `moduleClassId + moduleName + machine + timeDateStamp + sizeOfImage` 查找 v4 profile。
3. R3 本地验证 PDB identity：`pdbName + pdbGuid + pdbAge` 必须来自同一 PE RSDS，避免错误 pack 混入。
4. 生成紧凑二进制 module apply request：
   - module identity header
   - item count
   - item records
   - optional profile hash / pack hash
5. 按模块逐个下发：核心模块先下发，依赖模块后下发。
6. 下发后查询 R0 module profile status，更新 UI 命中状态。

R3 不应做的事：

- 不应因 v4 缺失而禁用 v3 当前路径。
- 不应把未匹配模块 profile 强行下发。
- 不应把缺失 required item 的 capability 标记为可用。
- 不应在 R3 自行“修正” R0 identity，只能选择匹配 profile。

## R0 应用策略

### 按已加载模块匹配

R0 应提供或扩展 loaded module identity 查询能力：

- 输入：module class dictionary 或内置 class -> basename 表。
- 输出：每个目标模块的 present、moduleName、classId、machine、timeDateStamp、sizeOfImage、imageBase。
- 匹配：module profile 的 `moduleClassId + moduleName + machine + timeDateStamp + sizeOfImage` 全部一致才允许 apply。
- absent：模块未加载时返回 absent，不算失败。

### capability bit 生成

R0 不信任 R3 传来的 capability。R0 应根据 module item 状态生成 capability：

- module present bit：模块已加载且 identity 已读取。
- profile matched bit：模块 profile identity 匹配并成功应用。
- group capability bit：某 capability group 的 required items 全部 present 且通过类型/范围校验。
- optional enhancement bit：可选 item 满足时额外置位，不影响基础 capability。

### 缺字段降级

缺字段分四类：

1. module absent：模块没加载。相关 capability 不置位，UI 显示 `Module absent`。
2. profile missing：pack 没有匹配 profile。相关 capability 不置位，UI 显示 `No profile`。
3. required item missing：profile 有但关键 item 缺失。group capability 不置位，UI 显示缺失 item。
4. optional item missing：基础 capability 可用，但增强检查降级，UI 显示 degraded。

### 字段校验

R0 应至少校验：

- request size / item count / integer overflow。
- item kind 是否受当前 R0 支持。
- item id 是否属于对应 module class。
- `StructOffset` 是否非 sentinel、在上限内，若有 TypeSize 则 offset < typeSize。
- `GlobalRva` / `ListHeadGlobal` 是否非零且 `< sizeOfImage`。
- `FunctionRva` 是否非零、`< sizeOfImage`，并位于可执行 section。
- `EnumValue` width 是否合法。
- `BitField` 的 bitOffset/bitWidth/storageSize 是否自洽。
- 同一 module profile 内重复 item id 拒绝。
- unknown required item 拒绝；unknown optional item 可计入 unknown 并拒绝该模块 profile，或者在 v4.1 明确允许 ignoreOptionalUnknown。建议 v4 初版全部拒绝 unknown，简洁可靠。

### 版本不匹配拒绝

以下情况必须拒绝该 module profile，且不影响其它模块已应用 profile：

- protocol version 不匹配。
- pack v4 module request header version 不匹配。
- module identity 不匹配。
- machine 不匹配。
- timeDateStamp / sizeOfImage 任一不匹配。
- item count 超限。
- required item 被 reject。
- 重复 item id。
- item kind 与 item id 元数据不匹配。

## capability 分组建议

建议 capability 从“功能审计域”而非“模块名”出发，方便 UI 和 R0 consumer 判断是否可用。

| Capability Group | 依赖模块 | required 示例 | optional 示例 | 降级行为 |
| --- | --- | --- | --- | --- |
| `Kernel.ProcessCrossView` | ntoskrnl | EPROCESS list/PID/name/token，PspCidTable | protection/signature bitfield | 缺 required 禁用 cross-view；缺 optional 不显示保护态细节。 |
| `Kernel.ThreadCrossView` | ntoskrnl | ETHREAD/KTHREAD thread list/start/cid | Win32StartAddress | 缺 required 禁用线程 cross-view；缺 optional 隐藏 Win32 start。 |
| `Kernel.ModuleIntegrity` | ntoskrnl | PsLoadedModuleList/ListHeadGlobal，KLDR offsets | MmUnloadedDrivers, PiDDB | 缺 required 禁用模块链审计；缺 optional 降级隐藏卸载痕迹。 |
| `Kernel.CallbackAudit` | ntoskrnl | callback globals + callback struct offsets | altitude/context | 缺 required 禁用 callback 枚举/一致性校验。 |
| `Win32k.WindowObjectAudit` | win32kbase/win32kfull | tagWND/tagTHREADINFO/tagDESKTOP offsets | enum/type size | 缺 required 禁用 GUI 对象审计。 |
| `Win32k.HandleAudit` | win32k/win32kbase | handle table globals/offsets | type enum | 缺 required 禁用 USER/GDI handle audit。 |
| `Network.TcpEndpointAudit` | tcpip/netio | endpoint globals/list heads/function RVAs | enum state values | 缺 required 禁用 TCP endpoint cross-view。 |
| `Network.NdisBindingAudit` | ndis/netio | miniport/filter/protocol list heads | enum state values | 缺 required 禁用 NDIS binding audit。 |
| `Filter.MinifilterAudit` | fltMgr | frame/filter/instance list heads and offsets | callback function RVAs | 缺 required 禁用 minifilter topology audit。 |
| `Storage.BitLockerAudit` | fvevol | volume/context offsets and key state enum | helper function RVAs | 缺 required 禁用 fvevol audit；缺 optional 仅做基础状态显示。 |

capability bit 建议不要继续无限挤压当前 64-bit `capabilityMask`。v4 可设计：

- `globalCapabilityMaskLow64`：兼容当前查询。
- `moduleCapabilityWords[]`：每模块/每 domain 多 word bitset。
- `capabilityDictionary`：R3/UI 用于把 bit 映射到名字。

## 失败降级规则

R0/R3/UI 统一降级语义：

| 条件 | R0 行为 | R3 行为 | UI 展示 |
| --- | --- | --- | --- |
| v4 pack 不存在 | 不变 | 继续 v3 apply | `v4 unavailable, v3 active` |
| v4 module absent | 不 apply | 跳过该 module | `Absent` |
| v4 profile missing | 不 apply | 记录 miss | `No matching profile` |
| identity mismatch | 拒绝该 module | 不重试错误 profile | `Rejected: identity mismatch` |
| required item missing | 拒绝该 capability group 或拒绝该 module profile | 标记 group disabled | `Missing required: item` |
| optional item missing | 应用基础 item | 标记 degraded | `Degraded: optional missing` |
| unknown item kind | 拒绝该 module profile | 报告协议不兼容 | `Rejected: unsupported kind` |
| item out of range | 拒绝该 module profile | 报告 bad profile | `Rejected: invalid value` |
| one module apply failed | 不影响其它已成功模块 | 继续其它模块 | 单模块红/黄，整体部分可用 |
| R0 protocol old | 不下发 v4 | 回退 v3 | `Driver does not support v4` |

关键原则：稳定审计宁可少报能力，也不能用错 offset/RVA 造成误读或崩溃。

## 兼容迁移路线

### 阶段 0：文档与 schema 固化

- 保留 v3 pack、v3 R3 loader、v3 R0 apply。
- 新增 v4 schema 文档、item dictionary 规划、module class dictionary。
- generator 先 dry-run 生成 v4 草案，不进入 Release。

### 阶段 1：R3 只读识别 v4 pack

- R3 能读取 v4 pack 并展示可匹配模块数。
- 不下发 R0。
- UI 新增“每模块 profile 命中状态”只读页：Loaded / Profile Found / Missing Required / Coverage。

### 阶段 2：R0 module identity query

- 新增或扩展查询已加载目标模块 identity 的 IOCTL。
- UI 显示 R0 实际 identity 与 pack identity 是否匹配。
- v3 仍旧原样执行。

### 阶段 3：v4 module apply 并存

- 新增 v4 module apply IOCTL 或 EX2 IOCTL。
- R3 按模块下发。
- R0 按模块存储 state，按 capability group 生成状态。
- v3 capabilityMask 继续保留；v4 capability 通过新查询返回。

### 阶段 4：业务审计接入

- 各 R0 审计功能只看 capability，不直接判断字段是否存在。
- 先接入只读审计：cross-view、module integrity、callback consistency、network topology、minifilter topology。
- mutation/patch 类功能继续独立 gating，不因 v4 profile 存在自动开放。

### 阶段 5：发布并行

- Release 同时携带：
  - `ark_dyndata_pack_v3.json`
  - `ark_dyndata_pack_v4.json`
- 老驱动/老 R3 忽略 v4。
- 新 R3 + 老 R0 自动回退 v3。
- 新 R3 + 新 R0 优先 v4，必要时 v3 补齐现有字段。

## UI 命中状态建议

UI 应至少展示：

| 列 | 含义 |
| --- | --- |
| Module | `ntoskrnl.exe` / `tcpip.sys` 等实际加载名。 |
| Class | module class name/id。 |
| Loaded Identity | timestamp/size/machine。 |
| PDB Identity | pdb name/guid/age。 |
| Profile | matched profile name。 |
| Status | `Applied` / `Absent` / `No profile` / `Rejected` / `Degraded`。 |
| Required | required present/total。 |
| Optional | optional present/total。 |
| Capability Groups | 已置位 group 名称。 |
| Missing | 缺失 required/optional item 摘要。 |

UI 不应把整体状态简化为一个绿灯。v4 的价值正是显示“哪个模块、哪个能力组、因哪个字段降级”。

## 后续编码任务清单

仅列任务，不在本阶段改代码：

1. shared protocol
   - 定义 v4 protocol version 或新增 v4 IOCTL function code。
   - 定义 module class id 枚举。
   - 定义 v4 item kind 枚举：StructOffset / GlobalRva / FunctionRva / EnumValue / TypeSize / BitField / ListHeadGlobal。
   - 定义 module apply request/response。
   - 定义 module status/capability query response。

2. R0 module identity
   - 扩展 loaded module identity resolver，支持多 basename 表。
   - 返回多模块 identity array。
   - 对 PE section 做只读解析，供 FunctionRva/GlobalRva section 校验。

3. R0 DynData v4 state
   - 新增按 module class 分桶的 state。
   - 新增 per-module item storage。
   - 新增 per-capability group dependency table。
   - 新增 copy-on-success module apply。

4. R0 validation
   - 实现 item kind/value 校验。
   - 实现 duplicate item id 检测。
   - 实现 FunctionRva executable section 校验。
   - 实现 BitField storage 校验。
   - 实现 ListHeadGlobal link dependency 校验。

5. R3 loader
   - 读取 `ark_dyndata_pack_v4.json`。
   - 构建 module identity index。
   - 按 R0 loaded modules 选择 profile。
   - 下发 per-module request。
   - 收集 per-module response。

6. PDB generator
   - 支持多模块 PE/PDB corpus。
   - 抽象 item resolvers：type/member/global/function/enum/bitfield/listhead。
   - 输出 v4 `moduleProfiles` 或 `profileSets`。
   - 输出 coverage 与 missing diagnostics。

7. release sync
   - 增加 `--pack-version 4`。
   - 验证 item dictionary、module identity、duplicate id、value range。
   - 生成 compact pack 和审计报告。

8. UI
   - 增加每模块 profile 命中页。
   - 增加 capability group 展示。
   - 增加缺字段/降级原因列表。
   - 保持 v3 页面不破坏。

9. 业务功能接入
   - Kernel process/thread/module/callback 审计先接 capability。
   - Win32k/network/fltMgr/fvevol 功能分阶段接入。
   - 所有 R0 worker 在 capability 缺失时必须返回明确降级状态。

## 验收标准

文档/方案验收：

- 明确审计了当前 v1/v2/v3 的支持范围和限制。
- 明确给出 v4 多模块 profile 的 identity 字段。
- 明确覆盖目标模块：ntoskrnl、win32k、win32kbase、win32kfull、tcpip、ndis、netio、fltMgr、fvevol。
- 明确设计 7 类 item：StructOffset、GlobalRva、FunctionRva、EnumValue、TypeSize、BitField、ListHeadGlobal。
- 包含 JSON 示例。
- 包含 capability 分组建议。
- 包含失败降级规则。
- 包含 v3 不破坏、v4 并存、UI 可见每模块状态的迁移路线。
- 包含后续编码任务清单。

后续代码实现验收：

- 老 v3 pack 和老 R0/R3 流程完全不受影响。
- 新 R3 在老 R0 上自动回退 v3，不误报 v4 失败为驱动异常。
- 新 R0 能返回多模块 identity。
- v4 能按模块独立 apply，单模块失败不影响其它模块。
- R0 capability 只由已验证 item 派生。
- 缺 required item 时相关 capability 不置位。
- 缺 optional item 时基础 capability 可用但 UI 显示 degraded。
- item 越界、identity mismatch、unknown kind、duplicate item 均被拒绝。
- UI 能看到每模块 profile 命中状态、缺失字段和 capability group 状态。
