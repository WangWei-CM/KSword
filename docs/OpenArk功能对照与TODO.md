# OpenArk 功能覆盖对照与 TODO

> 对照基准：用户提供的 OpenArk 功能清单。  
> 检查范围：`README.md`、`docs/功能技术文档.md`、主程序源码目录、共享 R3/R0 协议和 `KswordARKDriver`。  
> 判定说明：`已覆盖` 表示 KSword 已有直接功能入口；`部分覆盖` 表示已有相邻能力但缺少 OpenArk 同名入口、字段或完整工作流；`未覆盖` 表示当前源码/文档中未发现对应实现。

---

## 1. 总体结论

- KSword 已覆盖 OpenArk 的大部分核心 ARK 基础面：进程、线程、模块、句柄、文件占用、网络连接、Hosts、驱动列表、服务式驱动操作、SSDT、回调拦截/移除、注册表、启动项、PE 基础分析。
- KSword 并没有完整满足 OpenArk 的全部功能；主要缺口集中在 OpenArk 的工具箱类功能和部分内核枚举细项。
- KSword 已经明显超越 OpenArk 的方向包括：启动项总览、服务管理、ETW/WMI/WinAPI 监控、HTTPS 解析、网络抓包/限速/请求构造、硬件性能页、本地账号管理、BCD 启动编辑、NTFS 删除项恢复、HUD/Taskbar 辅助组件。
- 建议路线：先补齐 OpenArk 的 ARK 核心缺口，再考虑 CoderKit、Bundler、ToolRepo、Cleaner 等“工具合集”类能力。

---

## 2. OpenArk 功能覆盖矩阵

| OpenArk 功能域 | KSword 状态 | KSword 已有能力 | 缺口 / 备注 |
|---|---|---|---|
| Process 进程列表 | 已覆盖 | 进程枚举、树/列表显示、搜索、进程路径、命令行、用户、启动时间、CPU/内存/磁盘/GPU/网络、R3/R0 枚举对比，内存模块已提供进程内存 Dump 到文件。 | 进程详情内的窗口/句柄/内存页可继续与独立模块做更强联动。 |
| Process 进程操作 | 部分覆盖 | 结束、挂起/恢复、优先级、关键进程、R0 结束/挂起、PPL 设置、打开目录、CreateProcess 参数化创建，进程内存 Dump 到文件。 | 缺少 OpenArk 风格 `Send to Scanner` 一键入口；如需完全对齐，可额外提供 MiniDump/FullDump 命名兼容入口。 |
| 线程列表 | 已覆盖 | 全局线程页、进程详情线程页、线程状态/优先级/起始地址/TEB/寄存器摘要，支持线程挂起/恢复/终止。 | OpenArk 的“按进程右键 Enum Thread”可通过详情页实现；可增加右键快捷入口。 |
| 模块列表 | 已覆盖 | 进程详情模块页、模块基址/大小/路径/签名、刷新、模块卸载、模块关联线程操作。 | 缺少一键“发送到 Scanner/文件详情”的统一动作。 |
| DLL / Shellcode 注入 | 已覆盖并超越 | 进程详情操作页有 DLL 注入和 Shellcode 注入。 | 高危功能应保持确认、日志和授权提示。 |
| Token / PPL | 已覆盖并超越 | Token 详情、Token 开关、Raw TokenInformation、R0 PPL 级别调整。 | OpenArk 主要查看 PPL；KSword 已含修改能力，风险更高。 |
| 句柄列表 | 已覆盖 | 系统句柄枚举、PID/关键字/类型过滤、命名对象解析、对象地址、GrantedAccess、关闭句柄、同类型批量关闭。 | OpenArk 的“隐藏无名句柄”可由“只看命名对象”替代；可补按钮名兼容。 |
| ObjectTypes | 已覆盖 | 句柄模块提供对象类型统计、类型索引、对象数、句柄数、访问掩码等。 | 字段比 OpenArk 更偏 Native；可在内核页增加跳转。 |
| ObjectSections | 未覆盖 | 未发现 SectionDirectory/SectionName/Session/Dump/Edit 专门页。 | 放入 TODO。 |
| 进程内存 | 部分覆盖 | 内存模块支持进程附加、内存区域、搜索、十六进制查看、断点、书签、进程内存 Dump。 | OpenArk 的“可疑内存扫描/RX 私有页聚焦”需要补专用规则；内核内存读写另见 Kernel Memory。 |
| Kernel 入口 | 部分覆盖 | KSword 通过 `KswordARKDriver` 和共享 IOCTL 执行 R0 功能，主程序有驱动日志读取。 | 缺少 OpenArk 式统一“进入/退出 KernelMode”状态页和所有依赖项的集中提示。 |
| DriverKit 驱动安装器 | 部分覆盖 | 驱动服务注册/更新、加载、卸载、删除、状态查询。 | 缺 NT/WDF Installer、无签/过期签名安装、签名、写/清理注册表向导。 |
| Driver Manager | 部分覆盖 | 驱动服务列表、已加载内核模块、路径、基址、DBWIN 调试输出。 | 缺签名/版本/公司/描述字段完整表、DriverObject/DeviceObject/Dispatch/FastIo/IRP Hook 检查。 |
| System Notify | 部分覆盖 | 回调拦截支持注册表、进程、线程、镜像、对象规则；外部回调移除支持进程/线程/镜像 notify。 | 缺全量回调枚举表、模块归属、入口反汇编；注册表/Ob/Minifilter/WFP/ETW 外部删除未完整支持。 |
| System Hotkey | 未覆盖 | 未发现系统热键枚举/删除模块。 | 放入 TODO。 |
| Kernel Memory | 未覆盖 | 进程内存模块为 R3 进程内存；未发现通用 R0 地址读写页。 | 放入 TODO，写内存需默认高级模式关闭。 |
| Storage / UnlockFile | 已覆盖并超越 | 文件占用扫描、解锁、按驱动删除、结束占用进程、文件详情、NTFS 删除项恢复。 | OpenArk 的 FileObject/DllBase/FileHandle 字段可进一步补齐。 |
| Network TCP/UDP | 已覆盖并超越 | TCP/UDP 连接管理、TCP 终止、进程关联、抓包、过滤、进程限速、请求构造、HTTPS 解析、ARP/DNS、存活主机、多线程下载。 | 缺 OpenArk/常见 ARK 的 WFP/NDIS/LSP 链枚举、路由表/防火墙规则专页。 |
| Hosts 管理 | 部分覆盖 | 网络模块含 Hosts 文件编辑页。 | OpenArk 的备份、主文件标记、删除非主文件等备份管理流程未确认。 |
| Scanner PE/ELF | 部分覆盖 | 文件详情支持哈希、签名、PE 信息、字符串、Hex，PE 分析器读取 Header/Section/DataDirectory/CLR 等。 | 缺独立 Scanner 工作台、ELF 解析、RVA 转换页、Debug/Reloc/Import/Export 专门表格化体验。 |
| CoderKit | 未覆盖 | 仅有 CodeEditorWidget/HexEditorWidget，不等价于 CoderKit。 | 放入 TODO：编码转换、错误码查询、Base64/URL/hash、汇编/反汇编。 |
| Bundler | 未覆盖 | 未发现目录/多程序打包成 exe 和脚本执行器。 | 放入 TODO。 |
| ToolRepo | 未覆盖 | 未发现外部逆向/系统工具库目录和下载/启动入口。 | 放入 TODO。 |
| Cleaner | 未覆盖 | 未发现垃圾扫描/清理/自定义清理页。 | 放入 TODO。 |
| Utilities 系统工具入口 | 部分覆盖 | 已有设置、服务、注册表、启动项、BCD、桌面管理等内置页。 | 缺 OpenArk 式系统工具启动面板：cmd、PowerShell、设备管理器、事件查看器、防火墙、任务管理器等快捷入口。 |
| Console | 部分覆盖 | 有日志输出和即时窗口。 | 未发现 OpenArk 式命令控制台、命令帮助、运行历史。 |
| Settings | 部分覆盖 | 主题、背景、透明度、默认启动页、启动最大化、自动提权、系统右键菜单、滚动条/滑块设置。 | 缺中英文 UI 切换、检查更新、手册入口、置顶/重置窗口等 OpenArk 设置项。 |

---

## 3. KSword 超越 OpenArk 的功能

| 超越功能 | 说明 |
|---|---|
| 启动项总览 | OpenArk 不突出 Autoruns 全景；KSword 已有登录项、服务、驱动、计划任务、高级注册表、Winsock、WMI 分类和导出。 |
| 服务管理 | 独立服务模块支持筛选、启动/停止/暂停/继续、启动类型调整、属性编辑、依赖关系、恢复策略、审计和 TSV/JSON 导出。 |
| ETW 监控 | 可枚举 Provider/Session、启动采集、暂停/停止、前置/后置过滤、解析进程/文件/注册表/网络/安全/WMI/脚本等字段。 |
| WMI 监控 | 支持 WMI Provider/事件类查看、事件订阅、过滤和导出。 |
| WinAPI 监控 | `APIMonitor_x64` + `WinAPIDock` 提供目标进程 API 调用监控能力。 |
| 进程定向追踪 | `ProcessTraceMonitorWidget` 面向目标进程树做行为追踪。 |
| 网络抓包与过滤 | 不止 TCP/UDP 表，还包括多条件抓包过滤、进程关联、包详情和重放到请求构造。 |
| 进程限速 | 提供按 PID 的网络限速/挂起策略和动作日志。 |
| HTTPS 解析 | 本地代理、根证书信任、系统代理应用/清除、HTTPS 请求解析表。 |
| 手工请求构造 | 支持 Socket 参数、bind/connect/send/recv/shutdown 等低层请求构造。 |
| 多线程下载 | 支持分片下载、剪贴板捕获、分段进度条和任务状态管理。 |
| 硬件利用率页 | 类任务管理器的 CPU/内存/磁盘/网络/GPU 利用率与硬件详情。 |
| 权限/账号管理 | 本地用户、组、当前进程 Token/特权快照，并支持创建用户/重置密码。 |
| 注册表工作台 | 树浏览、键值增删改、导入/导出、异步搜索、内核路径复制。 |
| 文件恢复 | NTFS 删除项扫描和恢复导出，属于 OpenArk 未突出的取证/恢复能力。 |
| BCD 启动编辑 | 杂项模块含 BootEditor，可管理启动项、默认项、BootOnce、测试签名、安全模式等。 |
| 桌面管理 | 窗口模块复用 `OtherDock`，支持桌面枚举和切换。 |
| HUD / Taskbar 辅助组件 | 独立辅助工程提供 HUD 和任务栏类展示能力。 |
| GUID 调用链日志 | 日志系统通过 `kLogEvent` 串联一次操作的调用链，便于排障留证。 |
| 任务进度面板 | `kProgress` 为后台任务提供统一步骤、百分比和交互选择。 |

---

## 4. TODO：OpenArk 有但 KSword 未覆盖或未完全覆盖

### P0：优先补齐 ARK 核心体验

- [ ] Scanner 联动：进程、模块、驱动、网络连接、文件占用等右键增加 `发送到文件详情/Scanner`，统一打开可疑文件分析页。
- [ ] 进程详情联动页：在进程详情页补“句柄/窗口/内存区域”快捷 Tab 或跳转按钮，对齐 OpenArk 的进程聚合视图。
- [ ] System Notify 枚举：增加进程/线程/镜像/注册表/Ob 回调枚举表，展示回调地址、所属模块、服务名、签名状态和风险说明。
- [ ] 回调入口反汇编：对回调地址提供只读字节 Dump 和反汇编预览；删除回调前展示模块归属和风险确认。
- [ ] Kernel Memory：新增内核内存页，先做系统信息、R0/R3 地址范围、页大小、物理内存、只读 `ReadMemory` 和 `DumpToFile`。
- [ ] System Hotkey：新增系统热键枚举/删除页，展示 HotkeyObject、HotkeyID、HWND、标题、类名和输入法提示。
- [ ] ObjectSections：新增 Section 对象页，展示目录、名称、大小、Session、DumpToFile；Memory Edit 放入高级模式。

### P1：补齐 OpenArk 内核/驱动细项

- [ ] DriverKit 安装器：增加 NT/WDF Driver Installer，支持服务名、拖拽 `.sys`、注册/启动/停止/卸载、写/清理注册表。
- [ ] 签名/测试安装流程：补 `Install Unsigned`、`Install Expired Sign`、签名状态检测和测试签名/完整性提示。
- [ ] 驱动表字段增强：已加载驱动表增加版本、公司、描述、签名状态、文件属性、复制信息、定位文件、发送 Scanner。
- [ ] DriverObject 详情：枚举 DriverObject、DeviceObject、DriverExtension、MajorFunction、FastIoDispatch、Unload。
- [ ] IRP Hook 检查：检查 MajorFunction/FastIo 指针是否落在异常模块或非驱动映像范围。
- [ ] Hook 检测：补 Shadow SSDT、IDT、GDT、MSR、Inline Hook、IAT/EAT Hook 的只读检测页。
- [ ] 过滤链枚举：补 Minifilter、WFP Callout、NDIS Filter、LSP/Winsock Provider 链枚举。
- [ ] 内存异常页扫描：在内存模块增加 RWX/RX private、无文件映射、内存 PE 头、Shellcode 字符串/熵规则筛选。
- [ ] 文件占用字段增强：补 FileObject、DllBase、FileHandle、Type 等 OpenArk 字段，并提供 `Unlock All`。
- [ ] Hosts 备份管理：补 Hosts 备份列表、重命名、删除、标记主文件、删除非主文件、恢复备份。

### P2：补齐 OpenArk 工具箱类功能

- [ ] Scanner 独立工作台：把文件详情中的 PE 能力抽成 Scanner 页，增加 Summary/Header/Section/Import/Export/Relocation/Debug/RVA 子页。
- [ ] ELF 解析：为 Scanner 增加 ELF Header、Program Header、Section Header、Symbol、Dynamic、Relocation 基础解析。
- [ ] CoderKit 编码工具：增加 ASCII/Unicode/UTF-7/UTF-8/UTF-16/UTF-32/GBK/BIG5 等转换。
- [ ] CoderKit 常量查询：增加 Windows Error、DOS Error、NTSTATUS、HRESULT 和 Message 查询。
- [ ] CoderKit 算法工具：增加 Base64、CRC32、MD5、SHA1、SHA256、URL Encode/Decode。
- [ ] AsmTools：集成 nasm/ndisasm 或等价库，支持 x86/x64/ARM/MIPS 汇编/反汇编和输出格式切换。
- [ ] Bundler：增加目录/多程序打包为 exe、图标选择、启动脚本、`start/call/cmd/clean` 脚本命令。
- [ ] ToolRepo：增加 Common/Reverse/WinDevKits 分类工具库，支持本地路径配置、下载链接、启动和版本备注。
- [ ] Cleaner：增加垃圾扫描、清理、自定义目录、自定义后缀、文件数量/大小统计和清理日志。
- [ ] Console：增加命令控制台、命令帮助、历史记录、清空、输出窗口；和即时窗口区分用途。
- [ ] Utilities 快捷入口：增加 cmd、PowerShell、环境变量、设备管理器、事件查看器、防火墙、代理、路由、共享文件夹、任务管理器等系统工具入口。
- [ ] Settings 增强：补中英文 UI 切换、Always On Top、Reset Window、Check for Updates、Manuals 入口。

---

## 5. 不建议默认开启的高风险 TODO

- [ ] 内核任意写内存：只允许高级模式，默认关闭，必须二次确认并记录目标地址/大小/调用链。
- [ ] 强制卸载驱动：只允许对明确可卸载且非系统关键驱动执行，先展示 DriverObject/DeviceObject/引用风险。
- [ ] 回调删除扩展到 Ob/Registry/MiniFilter/WFP：必须先完成枚举、归属校验、快照导出和撤销提示。
- [ ] Hook 修复/Patch：建议先只做检测和导出，不做自动修复；修复功能按 Windows 版本适配后再进入高级模式。
- [ ] PPL 修改增强：已有 R0 PPL 设置能力，应保留强风险提示，不建议作为普通用户常用入口突出展示。

---

## 6. 建议实施顺序

1. 补 `Scanner 联动`、`回调枚举`、`Kernel Memory 只读页`，这些最贴近 OpenArk 核心体验；进程内存 Dump 已由内存模块覆盖。
2. 补 `System Hotkey`、`ObjectSections`、`DriverObject/IRP Hook`、`过滤链枚举`，提高 ARK 技术辨识度。
3. 把已有文件详情 PE 能力抽象成独立 `Scanner`，再补 ELF/RVA/表格化子页。
4. 最后做 CoderKit、Bundler、ToolRepo、Cleaner、Console、Utilities 等工具箱类能力。

