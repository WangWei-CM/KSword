# Issue #220 表格复制菜单核对

需求来源：[所有表格增加复制单元格并消除重复复制菜单](https://github.com/KSwordDEV/KSword/issues/220)。已通过浏览器查看 issue 中的四张截图。

## 截图与接入入口

| 实际导航入口 | 页面类型 | 共享入口与本次处理 |
| --- | --- | --- |
| 内核 → 对象命名空间 → 右侧属性表 | 对象字段表、现场列表 | `KernelDock.cpp::showCopyRowContextMenu` 标记业务行复制；共享层补单元格复制及导出。 |
| 内核 → I/O 管理 → SSDT / ShadowSSDT | 系统入口现场列表 | `KernelDock.Ssdt.cpp` 标记业务行复制，保留 HVM 等原业务动作。 |
| 内核 → 内核回调 → 回调枚举 / Hook | 现场列表与审计结果 | `KernelDock.CallbackEnum.cpp` 与 `KernelDock.KernelHooks.cpp` 的“复制”子菜单登记行复制角色，菜单根部不再追加同类动作。 |
| 内核 → CID 表 | 系统对象现场列表 | `KernelDockCidTab.cpp` 登记业务行复制，保留进程详情跳转。 |
| 各 Dock 的默认表格、惰性页和快照比对表 | 主列表 / 审计结果 | `TableInteractionSupport` 的应用级事件过滤器在真实右键事件时补齐共享菜单，`TableContextMenu` 按角色去重。 |
| 内存工作台 → 地址簿 | 专业表格编辑器 | 保留专业内部模型、行复制格式与 Ctrl+C；仅在既有菜单中补缺失的共享能力。 |

本次没有重排页面。原 `TablePresentation`、`ResultTableHost`、表格操作条模式和专业控件结构继续由其既有入口负责；字段树 `StructuredFieldView` 自有复制值/整行/选中项入口，不接入扁平表格菜单。新单元格复制覆盖 `QTableView`、`QTableWidget` 及其子类；树的原复制语义保留。

## 共享行为核对

- 业务动作通过 `SetTableContextActionRole` 明确声明 `CopyCell / CopyRows / ExportRows`，判断不依赖中文或英文标题。
- 检查当前菜单及其已经创建的子菜单。可见禁用动作仍占有其业务角色，保留依赖语义；隐藏项不阻止补缺失入口。
- 默认菜单与业务菜单复用相同的单元格、行复制和导出实现，专业的复制列、复制详情等动作保留。菜单只在真正缺少能力时追加分隔线。
- 单元格动作捕获右键命中的 `QPersistentModelIndex`；鼠标点空白处禁用，键盘上下文菜单使用当前格。复制原始 `DisplayRole` 文本，保留换行和制表符，不拼接整行。
- 来源表销毁、模型重置或更换后不再访问旧格。复用同一菜单时删除仅由共享层生成的旧动作，重绑当前来源，避免旧选区或重复回调。
- 表头的列显隐/排序菜单只保留提交屏障，不追加正文复制项。菜单打开期间仍使用 `UiCommitCoordinator` 阻止异步刷新提交。
- 原选中行复制与 TSV 导出实现保持选区、筛选及冻结语义；新增菜单使用现有 `ContextMenuStyle`，包含不透明底、正常/选中/禁用文字状态和项目 SVG 图标。
- “复制单元格”“复制选中行（TSV）”“导出选中行（TSV）”复用现有 zh-CN/en-US 源文本条目，没有整体重写语言包。新源码已登记主工程与 filters，也补进引用共享表格实现的既有组件构建清单。

## 业务角色登记清单

源码核对 73 个菜单实现文件，共 23 个单元格复制、100 个行复制、1 个导出角色。部分文件的同一 helper 服务多个动态表格；数字表示动作声明数，不表示已运行页面数。下表定位登记范围，UI运行与构建结论另行记录。

| 菜单实现（相对主程序根目录） | 单元格复制 | 行复制 | 导出 |
| --- | ---: | ---: | ---: |
| [RegistryDock_Themed.cpp](../Ksword5.1/Ksword5.1/RegistryDock_Themed.cpp) | 0 | 1 | 0 |
| [DriverDock/DriverDock.Integrity.cpp](../Ksword5.1/Ksword5.1/DriverDock/DriverDock.Integrity.cpp) | 0 | 1 | 0 |
| [DriverDock/DriverDock.Operation.cpp](../Ksword5.1/Ksword5.1/DriverDock/DriverDock.Operation.cpp) | 0 | 2 | 0 |
| [DriverDock/DriverDock.Ui.cpp](../Ksword5.1/Ksword5.1/DriverDock/DriverDock.Ui.cpp) | 0 | 1 | 0 |
| [DriverDock/DriverDock.Unloaded.cpp](../Ksword5.1/Ksword5.1/DriverDock/DriverDock.Unloaded.cpp) | 0 | 1 | 0 |
| [FileDock/FileDock.cpp](../Ksword5.1/Ksword5.1/FileDock/FileDock.cpp) | 0 | 4 | 0 |
| [FileDock/FileMappedProcessWindow.cpp](../Ksword5.1/Ksword5.1/FileDock/FileMappedProcessWindow.cpp) | 0 | 1 | 0 |
| [Framework/LogDockWidget.cpp](../Ksword5.1/Ksword5.1/Framework/LogDockWidget.cpp) | 1 | 2 | 0 |
| [HardwareDock/DiskMonitorPage.cpp](../Ksword5.1/Ksword5.1/HardwareDock/DiskMonitorPage.cpp) | 0 | 1 | 0 |
| [HardwareDock/HardwareDock.cpp](../Ksword5.1/Ksword5.1/HardwareDock/HardwareDock.cpp) | 0 | 1 | 0 |
| [HardwareDock/HardwareR0EvidencePage.cpp](../Ksword5.1/Ksword5.1/HardwareDock/HardwareR0EvidencePage.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelBaseNamedObjectsTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelBaseNamedObjectsTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelCommunicationEndpointTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelCommunicationEndpointTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDescriptorTableTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDescriptorTableTab.cpp) | 1 | 1 | 0 |
| [KernelDock/KernelDeviceDriverObjectsTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDeviceDriverObjectsTab.cpp) | 1 | 1 | 1 |
| [KernelDock/KernelDock.CallbackEnum.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.CallbackEnum.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDock.CallbackIntercept.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.CallbackIntercept.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDock.ContextMenu.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.ContextMenu.cpp) | 2 | 2 | 0 |
| [KernelDock/KernelDock.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDock.DriverStatus.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.DriverStatus.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDock.DynData.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.DynData.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDock.KernelHooks.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.KernelHooks.cpp) | 0 | 4 | 0 |
| [KernelDock/KernelDock.Ssdt.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDock.Ssdt.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDockCidTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDockCidTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelDockIpcTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDockIpcTab.cpp) | 0 | 2 | 0 |
| [KernelDock/KernelDriverStartIoTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelDriverStartIoTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelIoctlAuditTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelIoctlAuditTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelIoTimerTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelIoTimerTab.cpp) | 1 | 1 | 0 |
| [KernelDock/KernelNamedPipeTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelNamedPipeTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelObjectDirectoryDeepTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelObjectDirectoryDeepTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelObjectTypeMatrixTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelObjectTypeMatrixTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelObjectTypeProcedureTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelObjectTypeProcedureTab.cpp) | 0 | 1 | 0 |
| [KernelDock/KernelSymbolicLinkTab.cpp](../Ksword5.1/Ksword5.1/KernelDock/KernelSymbolicLinkTab.cpp) | 1 | 1 | 0 |
| [MemoryDock/MemoryDock.KernelExecutableMemory.cpp](../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.KernelExecutableMemory.cpp) | 0 | 1 | 0 |
| [MemoryDock/MemoryDock.KernelMemoryEvidence.cpp](../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.KernelMemoryEvidence.cpp) | 0 | 1 | 0 |
| [MemoryDock/MemoryDock.ProcessMemoryEvidence.cpp](../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.ProcessMemoryEvidence.cpp) | 0 | 1 | 0 |
| [MemoryDock/MemoryDock.ProcessPteTranslate.cpp](../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.ProcessPteTranslate.cpp) | 0 | 1 | 0 |
| [MemoryDock/MemoryDock.ProcessRegion.cpp](../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.ProcessRegion.cpp) | 0 | 1 | 0 |
| [MemoryDock/MemoryDock.UiWireAndStatus.cpp](../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.UiWireAndStatus.cpp) | 0 | 3 | 0 |
| [MonitorDock/DirectKernelCallMonitorWidget.cpp](../Ksword5.1/Ksword5.1/MonitorDock/DirectKernelCallMonitorWidget.cpp) | 0 | 1 | 0 |
| [MonitorDock/MonitorDock.ArkRiskCenter.cpp](../Ksword5.1/Ksword5.1/MonitorDock/MonitorDock.ArkRiskCenter.cpp) | 0 | 1 | 0 |
| [MonitorDock/MonitorDock.cpp](../Ksword5.1/Ksword5.1/MonitorDock/MonitorDock.cpp) | 2 | 5 | 0 |
| [MonitorDock/ProcessTraceMonitorWidget.Actions.cpp](../Ksword5.1/Ksword5.1/MonitorDock/ProcessTraceMonitorWidget.Actions.cpp) | 0 | 4 | 0 |
| [MonitorDock/ProcessTraceMonitorWidget.Export.cpp](../Ksword5.1/Ksword5.1/MonitorDock/ProcessTraceMonitorWidget.Export.cpp) | 1 | 1 | 0 |
| [MonitorDock/WinAPIDock.Actions.cpp](../Ksword5.1/Ksword5.1/MonitorDock/WinAPIDock.Actions.cpp) | 1 | 1 | 0 |
| [NetworkDock/NetworkAuditPage.cpp](../Ksword5.1/Ksword5.1/NetworkDock/NetworkAuditPage.cpp) | 0 | 2 | 0 |
| [NetworkDock/NetworkDock.HttpsAnalyze.cpp](../Ksword5.1/Ksword5.1/NetworkDock/NetworkDock.HttpsAnalyze.cpp) | 0 | 1 | 0 |
| [NetworkDock/NetworkDock.InternalHelpers.cpp](../Ksword5.1/Ksword5.1/NetworkDock/NetworkDock.InternalHelpers.cpp) | 0 | 1 | 0 |
| [NetworkDock/NetworkDock.Nids.cpp](../Ksword5.1/Ksword5.1/NetworkDock/NetworkDock.Nids.cpp) | 1 | 2 | 0 |
| [NetworkDock/NetworkDock.UiConnections.cpp](../Ksword5.1/Ksword5.1/NetworkDock/NetworkDock.UiConnections.cpp) | 0 | 3 | 0 |
| [NetworkDock/NetworkFirewallPage.cpp](../Ksword5.1/Ksword5.1/NetworkDock/NetworkFirewallPage.cpp) | 0 | 2 | 0 |
| [OnlineScan/VirusTotalOnlineScan.cpp](../Ksword5.1/Ksword5.1/OnlineScan/VirusTotalOnlineScan.cpp) | 1 | 2 | 0 |
| [OtherDock/OtherDock.cpp](../Ksword5.1/Ksword5.1/OtherDock/OtherDock.cpp) | 0 | 1 | 0 |
| [PrivilegeDock/PrivilegeAccountPages.cpp](../Ksword5.1/Ksword5.1/PrivilegeDock/PrivilegeAccountPages.cpp) | 0 | 1 | 0 |
| [PrivilegeDock/PrivilegeDock.cpp](../Ksword5.1/Ksword5.1/PrivilegeDock/PrivilegeDock.cpp) | 0 | 1 | 0 |
| [ProcessDock/ProcessDetailWindow.BaseAndUi.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessDetailWindow.BaseAndUi.cpp) | 1 | 2 | 0 |
| [ProcessDock/ProcessDetailWindow.Hotkey.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessDetailWindow.Hotkey.cpp) | 0 | 1 | 0 |
| [ProcessDock/ProcessDetailWindow.Module.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessDetailWindow.Module.cpp) | 1 | 1 | 0 |
| [ProcessDock/ProcessDock.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp) | 1 | 2 | 0 |
| [ProcessDock/ProcessDock.CrossView.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.CrossView.cpp) | 0 | 1 | 0 |
| [ProcessDock/ProcessDock.ThreadTab.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.ThreadTab.cpp) | 1 | 1 | 0 |
| [ProcessDock/ProcessMessageHookWindow.cpp](../Ksword5.1/Ksword5.1/ProcessDock/ProcessMessageHookWindow.cpp) | 1 | 1 | 0 |
| [StartupDock/StartupDock.Actions.cpp](../Ksword5.1/Ksword5.1/StartupDock/StartupDock.Actions.cpp) | 0 | 1 | 0 |
| [WindowDock/WindowDock.cpp](../Ksword5.1/Ksword5.1/WindowDock/WindowDock.cpp) | 0 | 1 | 0 |
| [WindowDock/WindowEventHookTab.cpp](../Ksword5.1/Ksword5.1/WindowDock/WindowEventHookTab.cpp) | 1 | 1 | 0 |
| [WindowDock/WindowGlobalHotkeyTab.cpp](../Ksword5.1/Ksword5.1/WindowDock/WindowGlobalHotkeyTab.cpp) | 1 | 1 | 0 |
| [WindowDock/WindowGuiHandleTab.cpp](../Ksword5.1/Ksword5.1/WindowDock/WindowGuiHandleTab.cpp) | 1 | 1 | 0 |
| [WindowDock/WindowTimerTab.cpp](../Ksword5.1/Ksword5.1/WindowDock/WindowTimerTab.cpp) | 1 | 1 | 0 |
| [句柄/HandleDock.cpp](../Ksword5.1/Ksword5.1/句柄/HandleDock.cpp) | 0 | 1 | 0 |
| [句柄/HandleDock.Filter.cpp](../Ksword5.1/Ksword5.1/句柄/HandleDock.Filter.cpp) | 1 | 1 | 0 |
| [UI/MemoryWorkbench/AddressBookPanel.Menu.cpp](../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookPanel.Menu.cpp) | 0 | 1 | 0 |
| [MiscDock/BootEditor/BootEditorTab.cpp](../Ksword5.1/Ksword5.1/MiscDock/BootEditor/BootEditorTab.cpp) | 0 | 1 | 0 |
| [MiscDock/DiskEditor/DiskEditorTab.cpp](../Ksword5.1/Ksword5.1/MiscDock/DiskEditor/DiskEditorTab.cpp) | 0 | 1 | 0 |

## 验证记录

- 源码扫描：未标记的既有单元格/当前行/选中行复制动作 0 项；字段树为上述明确例外。工程登记通过。
- Release/x64 构建通过：`BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`、`ARTIFACT_UPDATED=True`，主程序 24,974,848 字节。日志 `.codex-build-logs/ksword-build-check-20261012-231310.raw.log`；SHA256 `161CC2F2CE27040301CBA6832772471F6A0854E06BC838B202A4C6BA654E9DF5`。语言包与主题 token 构建门禁通过。
- 首轮构建在语言包门禁识别到头文件名 `TableContextMenu.h`；按项目已有约定把本目录引用写为 `./TableContextMenu.h` 后重新构建通过，没有添加无用途的头文件翻译。
- 真实主程序已启动到对象命名空间页，R0 未启用。用户按物理 Escape 停止 Computer Use，已停止窗口操作；本次没有取得修复后菜单截图，也未验证剪贴板输出。不能把启动成功或编译成功说成菜单 GUI 验收通过。
- 后续人工查看项：对象命名空间右侧属性表、SSDT/ShadowSSDT、内核审计与回调的“复制”子菜单、CID 表；另检查默认表格、空白处、键盘菜单、重复打开菜单、排序/代理表、快照比对与深浅主题。
- 自动验证没有向系统剪贴板写入测试数据，没有使用伪窗口或离屏窗口代替主程序。四份既有组件构建清单仅补充新模块的链接依赖，本次没有运行这些独立 UI 夹具。
- 全部旧页面的布局、窄窗、DPI、背景模式和深浅主题视觉合规不由本次菜单源码核对推定。

