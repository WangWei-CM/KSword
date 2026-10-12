# CKS 页面 UI 修正记录

日期：2026-10-12。入口：杂项 → CKS 管理。页面类型：参数执行与系统诊断。

- 同类页对照：系统变速与虚拟定位；检查了 ParameterWorkspace、PageControlStyle、ToolbarMetrics 及结果表宿主实现。
- 两栏复用 ParameterWorkspace：流程与文件选择独立滚动，执行区固定；SSDE 动作使用 FlowLayout 按实际宽度换行。
- 策略清单使用 VisibleTableWidget、ResultTableHost 和 TablePresentation，操作条明确为 Compact；四列保持概览，无需扩展列组。状态只保留简短摘要，未新增日志或详情窗口。
- 用户截图中的参数区深色矩形来自子页误用 StyleWorkspacePage；普通子页改用共享 StyleParameterCanvas，嵌套 pane 沿用参数内容色。
- 用户截图中的 SSDE 标签矩形是鼠标触发的原生焦点提示。共享 PageTabFocusStyle 根据鼠标、FocusIn 和键盘事件区分来源，鼠标隐藏焦点 primitive，键盘导航保留，原焦点策略不变。影响范围为接入 StylePageTabBar 的普通页签。
- 普通确认框交给全局 ThemedMessageBox；页根、Tab、工具行与按钮沿用共享入口。业务查询、启用条件和确认动作保持原语义。
- 源码差异核对通过；HostX64 Release 构建成功，BUILD_RESULT=SUCCESS、EXIT_CODE=0，i18n 与主题门禁通过。日志：`.codex-build-logs/ksword-build-check-20261012-122243.raw.log`。
- 真实 GUI 证据仅有修复前用户截图。此前运行进程未开放 MCP UI 桥；修复后的鼠标/键盘焦点、深浅主题、背景模式、窄窗与 DPI 尚未完成实机视觉验收，不声明全页面视觉合规。
