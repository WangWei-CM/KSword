// =============================
// tabs-data.js
// 作用：维护 Ksword5.1 全部模块的中英文展示数据。
// =============================

// kswordTabs 数组用途：
// - 产品总览页生成模块卡片；
// - 详情页按 slug 渲染完整说明；
// - 截图命名规范与页面占位共用同一来源。
window.kswordTabs = [
    {
        slug: "welcome",
        page: "welcome.html",
        title: { zh: "欢迎", en: "Welcome" },
        summary: {
            zh: "展示产品定位、环境状态与新用户引导入口。",
            en: "Shows product positioning, environment status, and onboarding guidance."
        },
        tags: {
            zh: ["总览", "引导", "入口"],
            en: ["Overview", "Onboarding", "Entry"]
        },
        overview: {
            zh: "欢迎模块用于快速确认当前软件环境、用户身份与版本定位，是整个工具链的统一起点。",
            en: "The Welcome module verifies runtime environment, user identity, and version context as a unified starting point."
        },
        highlights: {
            zh: [
                "展示软件定位与核心能力概要。",
                "读取当前 Windows 用户信息并预留头像展示位。",
                "帮助新用户快速理解各功能模块入口。"
            ],
            en: [
                "Presents product positioning and core capabilities.",
                "Loads current Windows user information with avatar placeholder.",
                "Helps first-time users understand module navigation quickly."
            ]
        },
        workflow: {
            zh: [
                "启动后先查看欢迎页，确认版本与环境。",
                "检查权限状态后进入目标模块。",
                "排障时回到欢迎页核对基础信息。"
            ],
            en: [
                "Open Welcome first to verify version and environment.",
                "Check privilege state before entering target modules.",
                "Return here during troubleshooting to validate baseline info."
            ]
        },
        screenshotPlan: [
            {
                file: "tab_welcome_overview.png",
                goal: { zh: "欢迎页完整界面", en: "Full welcome page view" }
            },
            {
                file: "tab_welcome_user_profile.png",
                goal: { zh: "用户信息区域特写", en: "User profile section close-up" }
            }
        ]
    },
    {
        slug: "process",
        page: "process.html",
        title: { zh: "进程", en: "Process" },
        summary: {
            zh: "进程枚举、详情查看、常见控制动作与创建流程统一集中。",
            en: "Centralizes process enumeration, detail inspection, control actions, and creation flow."
        },
        tags: {
            zh: ["核心", "控制", "排障"],
            en: ["Core", "Control", "Troubleshooting"]
        },
        overview: {
            zh: "进程模块是 Ksword5.1 的主控台，支持实时刷新、树状展示、进程动作和详情联动分析。",
            en: "Process is the core console of Ksword5.1 with refresh, tree view, process actions, and deep inspection linkage."
        },
        highlights: {
            zh: [
                "支持列表/树状视图与多列进程指标展示。",
                "支持结束、挂起、恢复、优先级调整等动作。",
                "支持创建进程并配置命令行与权限相关参数。"
            ],
            en: [
                "Supports list/tree view with multi-column process metrics.",
                "Supports terminate, suspend, resume, and priority actions.",
                "Supports process creation with command and privilege parameters."
            ]
        },
        workflow: {
            zh: [
                "刷新列表并定位目标 PID。",
                "通过右键菜单执行进程控制动作。",
                "打开详情窗口核对模块与高级信息。"
            ],
            en: [
                "Refresh list and locate target PID.",
                "Use context menu to execute control actions.",
                "Open detail window for modules and advanced info."
            ]
        },
        screenshotPlan: [
            { file: "tab_process_main_table.png", goal: { zh: "进程主表格", en: "Main process table" } },
            { file: "tab_process_context_menu.png", goal: { zh: "进程右键菜单", en: "Process context menu" } },
            { file: "tab_process_create_form.png", goal: { zh: "创建进程表单", en: "Process creation form" } },
            { file: "tab_process_detail_window.png", goal: { zh: "进程详情窗口", en: "Process detail window" } }
        ]
    },
    {
        slug: "network",
        page: "network.html",
        title: { zh: "网络", en: "Network" },
        summary: {
            zh: "抓包监控、连接管理、限速规则和网络诊断功能集中在本模块。",
            en: "Packet monitoring, connection management, rate limiting, and diagnostics are centralized here."
        },
        tags: {
            zh: ["监控", "连接", "诊断"],
            en: ["Monitoring", "Connection", "Diagnostics"]
        },
        overview: {
            zh: "网络模块用于实时观察通信行为并执行连接管控，适用于定位可疑流量与通信异常。",
            en: "Network module monitors traffic in real time and applies connection controls for suspicious or abnormal communication."
        },
        highlights: {
            zh: [
                "支持抓包并按协议、进程、地址过滤。",
                "支持 TCP/UDP 连接列表刷新与终止。",
                "支持手工请求、ARP/DNS 缓存与主机扫描。"
            ],
            en: [
                "Supports packet capture with protocol/process/address filters.",
                "Supports TCP/UDP connection refresh and termination.",
                "Supports manual requests, ARP/DNS cache operations, and host scan."
            ]
        },
        workflow: {
            zh: [
                "先启动监控并设置过滤条件。",
                "在连接管理页处理异常连接。",
                "必要时使用手工请求复现实验流量。"
            ],
            en: [
                "Start monitoring and configure filters first.",
                "Handle suspicious sessions in connection management.",
                "Use manual request to reproduce traffic when needed."
            ]
        },
        screenshotPlan: [
            { file: "tab_network_monitor.png", goal: { zh: "流量监控页", en: "Traffic monitor page" } },
            { file: "tab_network_rate_limit.png", goal: { zh: "限速配置页", en: "Rate limit configuration" } },
            { file: "tab_network_connection_manage.png", goal: { zh: "连接管理页", en: "Connection management page" } },
            { file: "tab_network_manual_request.png", goal: { zh: "手工请求页", en: "Manual request page" } },
            { file: "tab_network_diagnostics.png", goal: { zh: "网络诊断页", en: "Network diagnostics page" } }
        ]
    },
    {
        slug: "memory",
        page: "memory.html",
        title: { zh: "内存", en: "Memory" },
        summary: {
            zh: "覆盖附加进程、区域浏览、搜索扫描、十六进制查看与断点书签。",
            en: "Covers process attach, region browsing, scan search, hex viewer, and breakpoint/bookmark tools."
        },
        tags: {
            zh: ["逆向", "调试", "分析"],
            en: ["Reverse", "Debug", "Analysis"]
        },
        overview: {
            zh: "内存模块提供从进程附加到地址定位的完整路径，是高级调试与逆向分析的核心能力。",
            en: "Memory module provides an end-to-end path from process attach to address localization for advanced debugging."
        },
        highlights: {
            zh: [
                "支持模块列表与内存区域属性展示。",
                "支持首次/再次扫描与结果过滤。",
                "支持十六进制查看器、断点和书签管理。"
            ],
            en: [
                "Supports module list and memory region attributes.",
                "Supports first/next scan with result filtering.",
                "Supports hex viewer, breakpoints, and bookmarks."
            ]
        },
        workflow: {
            zh: [
                "选择目标进程并附加。",
                "执行首次扫描后继续缩小结果。",
                "在查看器验证地址并添加书签。"
            ],
            en: [
                "Select and attach to target process.",
                "Run first scan, then narrow down with next scan.",
                "Verify addresses in viewer and add bookmarks."
            ]
        },
        screenshotPlan: [
            { file: "tab_memory_process_module.png", goal: { zh: "进程与模块页", en: "Process and module page" } },
            { file: "tab_memory_region_list.png", goal: { zh: "内存区域列表", en: "Memory region list" } },
            { file: "tab_memory_search.png", goal: { zh: "搜索结果页", en: "Search result page" } },
            { file: "tab_memory_viewer.png", goal: { zh: "十六进制查看器", en: "Hex viewer" } },
            { file: "tab_memory_breakpoint_bookmark.png", goal: { zh: "断点与书签页", en: "Breakpoint and bookmark page" } }
        ]
    },
    {
        slug: "file",
        page: "file.html",
        title: { zh: "文件", en: "File" },
        summary: {
            zh: "双面板文件浏览器，支持复制剪切粘贴与常见文件管理动作。",
            en: "Dual-panel file browser with copy/cut/paste and common file management operations."
        },
        tags: {
            zh: ["双栏", "管理", "效率"],
            en: ["Dual Panel", "Management", "Efficiency"]
        },
        overview: {
            zh: "文件模块采用双面板布局，适合在系统目录间进行高频批量操作和快速导航。",
            en: "File module uses a dual-panel layout for efficient bulk operations and rapid directory navigation."
        },
        highlights: {
            zh: [
                "左右面板支持独立路径和历史导航。",
                "支持复制、剪切、粘贴、重命名、删除。",
                "支持文件详情弹窗与过滤排序。"
            ],
            en: [
                "Left and right panels support independent paths and history.",
                "Supports copy, cut, paste, rename, and delete actions.",
                "Supports detail dialog and filter/sort controls."
            ]
        },
        workflow: {
            zh: [
                "左右面板分别定位源与目标目录。",
                "执行复制或剪切，再到目标目录粘贴。",
                "通过详情对话框核对关键属性。"
            ],
            en: [
                "Locate source and destination folders in each panel.",
                "Copy or cut, then paste into destination panel.",
                "Verify critical attributes in detail dialog."
            ]
        },
        screenshotPlan: [
            { file: "tab_file_dual_panel.png", goal: { zh: "双面板主界面", en: "Dual-panel main view" } },
            { file: "tab_file_context_menu.png", goal: { zh: "文件右键菜单", en: "File context menu" } },
            { file: "tab_file_detail_dialog.png", goal: { zh: "文件详情弹窗", en: "File detail dialog" } }
        ]
    },
    {
        slug: "driver",
        page: "driver.html",
        title: { zh: "驱动", en: "Driver" },
        summary: {
            zh: "支持驱动服务注册、加载卸载与调试输出捕获。",
            en: "Supports driver service registration, load/unload operations, and debug output capture."
        },
        tags: {
            zh: ["内核", "服务", "调试"],
            en: ["Kernel", "Service", "Debug"]
        },
        overview: {
            zh: "驱动模块面向内核运维场景，涵盖驱动生命周期管理与错误定位信息收集。",
            en: "Driver module targets kernel operations, covering lifecycle management and troubleshooting data collection."
        },
        highlights: {
            zh: [
                "展示驱动服务列表和状态信息。",
                "支持注册、加载、卸载、删除服务。",
                "支持调试输出实时捕获。"
            ],
            en: [
                "Displays driver service list and state information.",
                "Supports register, load, unload, and delete actions.",
                "Supports real-time debug output capture."
            ]
        },
        workflow: {
            zh: [
                "刷新服务列表并选中目标服务。",
                "配置驱动路径后执行加载或卸载。",
                "查看调试输出确认执行结果。"
            ],
            en: [
                "Refresh services and select target entry.",
                "Configure driver path then load or unload service.",
                "Review debug output for final validation."
            ]
        },
        screenshotPlan: [
            { file: "tab_driver_overview.png", goal: { zh: "驱动概览页", en: "Driver overview page" } },
            { file: "tab_driver_operate.png", goal: { zh: "驱动操作页", en: "Driver operation page" } },
            { file: "tab_driver_debug_output.png", goal: { zh: "调试输出页", en: "Debug output page" } }
        ]
    },
    {
        slug: "kernel",
        page: "kernel.html",
        title: { zh: "内核", en: "Kernel" },
        summary: {
            zh: "提供内核对象类型查询与 NtQuery 结果分析。",
            en: "Provides kernel object type queries and NtQuery result analysis."
        },
        tags: {
            zh: ["底层", "查询", "对象"],
            en: ["Low-level", "Query", "Object"]
        },
        overview: {
            zh: "内核模块用于查看系统底层对象结构与调用返回状态，适合内核级调试与学习。",
            en: "Kernel module reveals low-level object structures and call statuses for kernel debugging and exploration."
        },
        highlights: {
            zh: [
                "支持内核对象类型异步刷新与过滤。",
                "支持 NtQuery 查询项列表与详情联动。",
                "支持状态码、摘要、详情三层信息展示。"
            ],
            en: [
                "Supports async refresh and filtering for kernel object types.",
                "Supports NtQuery item list with linked details.",
                "Displays status code, summary, and full details."
            ]
        },
        workflow: {
            zh: [
                "刷新内核类型并筛选关键对象。",
                "切换到 NtQuery 页查看目标调用。",
                "通过详情区确认状态和返回内容。"
            ],
            en: [
                "Refresh kernel types and filter target objects.",
                "Switch to NtQuery tab for target calls.",
                "Validate status and return content in detail area."
            ]
        },
        screenshotPlan: [
            { file: "tab_kernel_type_table.png", goal: { zh: "内核类型表格", en: "Kernel type table" } },
            { file: "tab_kernel_ntquery.png", goal: { zh: "NtQuery 结果页", en: "NtQuery result page" } }
        ]
    },
    {
        slug: "monitor",
        page: "monitor.html",
        title: { zh: "监控", en: "Monitor" },
        summary: {
            zh: "WMI 与 ETW 双通道实时监控，支持过滤、暂停和导出。",
            en: "Dual-channel WMI and ETW monitoring with filtering, pause, and export support."
        },
        tags: {
            zh: ["WMI", "ETW", "事件"],
            en: ["WMI", "ETW", "Event"]
        },
        overview: {
            zh: "监控模块用于实时采集系统事件流，帮助快速定位行为轨迹与安全风险。",
            en: "Monitor module captures system event streams in real time to locate behavior trails and security risks."
        },
        highlights: {
            zh: [
                "支持 WMI Provider 与事件类刷新。",
                "支持订阅、暂停和筛选 WMI 事件。",
                "支持 ETW 捕获、上下文操作与导出。"
            ],
            en: [
                "Supports WMI provider and event class refresh.",
                "Supports WMI subscription, pause, and filtering.",
                "Supports ETW capture, context actions, and export."
            ]
        },
        workflow: {
            zh: [
                "刷新 Provider 并选择目标事件源。",
                "启动 WMI/ETW 采集并应用过滤条件。",
                "导出关键事件用于离线分析。"
            ],
            en: [
                "Refresh providers and choose target event source.",
                "Start WMI/ETW capture and apply filters.",
                "Export key events for offline analysis."
            ]
        },
        screenshotPlan: [
            { file: "tab_monitor_wmi_provider.png", goal: { zh: "WMI Provider 区域", en: "WMI provider area" } },
            { file: "tab_monitor_wmi_events.png", goal: { zh: "WMI 事件列表", en: "WMI event list" } },
            { file: "tab_monitor_etw_events.png", goal: { zh: "ETW 事件列表", en: "ETW event list" } }
        ]
    },
    {
        slug: "privilege",
        page: "privilege.html",
        title: { zh: "权限", en: "Privilege" },
        summary: {
            zh: "用于展示与调整令牌权限状态，为高权限操作提供前置检查。",
            en: "Displays and adjusts token privileges for pre-check before high-privilege actions."
        },
        tags: {
            zh: ["安全", "令牌", "状态"],
            en: ["Security", "Token", "Status"]
        },
        overview: {
            zh: "权限模块用于统一检查关键权限位，确保后续动作在正确的权限上下文下执行。",
            en: "Privilege module centralizes key privilege checks to ensure operations run in the correct security context."
        },
        highlights: {
            zh: [
                "展示当前进程权限状态。",
                "支持关键权限开关与状态反馈。",
                "可与进程模块联动执行高权限动作。"
            ],
            en: [
                "Displays privilege state of current process.",
                "Supports key privilege toggles with feedback.",
                "Works with Process module for privileged actions."
            ]
        },
        workflow: {
            zh: [
                "进入权限页确认关键权限状态。",
                "按任务需求启用/调整权限。",
                "返回业务模块执行目标动作。"
            ],
            en: [
                "Open Privilege tab and verify key states.",
                "Enable or adjust privileges based on task need.",
                "Return to business module to execute operations."
            ]
        },
        screenshotPlan: [
            { file: "tab_privilege_main.png", goal: { zh: "权限主界面", en: "Privilege main view" } }
        ]
    },
    {
        slug: "settings",
        page: "settings.html",
        title: { zh: "设置", en: "Settings" },
        summary: {
            zh: "提供外观与主题相关配置，包括深浅模式、透明度和背景资源。",
            en: "Provides appearance settings including theme mode, opacity, and background resource."
        },
        tags: {
            zh: ["外观", "配置", "持久化"],
            en: ["Appearance", "Configuration", "Persistence"]
        },
        overview: {
            zh: "设置模块用于持久化用户偏好，保证每次启动都能恢复一致的界面体验。",
            en: "Settings module persists user preferences so the interface can be restored consistently on startup."
        },
        highlights: {
            zh: [
                "支持深浅主题快速切换。",
                "支持透明度滑块实时反馈。",
                "支持背景路径选择与重置。"
            ],
            en: [
                "Supports quick light/dark theme switching.",
                "Supports real-time opacity slider feedback.",
                "Supports background path selection and reset."
            ]
        },
        workflow: {
            zh: [
                "选择主题模式并确认整体观感。",
                "调整透明度至可读性与观感平衡。",
                "保存设置并重启验证持久化。"
            ],
            en: [
                "Choose theme mode and verify visual style.",
                "Adjust opacity for readability balance.",
                "Save settings and relaunch to verify persistence."
            ]
        },
        screenshotPlan: [
            { file: "tab_settings_appearance.png", goal: { zh: "外观设置页", en: "Appearance settings page" } },
            { file: "tab_settings_theme_switch.png", goal: { zh: "主题切换对比", en: "Theme switching comparison" } }
        ]
    },
    {
        slug: "window",
        page: "window.html",
        title: { zh: "窗口", en: "Window" },
        summary: {
            zh: "面向窗口对象枚举与控制的模块，适合图形界面行为排障。",
            en: "Focused on window object enumeration and control for GUI troubleshooting."
        },
        tags: {
            zh: ["窗口", "对象", "控制"],
            en: ["Window", "Object", "Control"]
        },
        overview: {
            zh: "窗口模块用于查看系统窗口与关联进程信息，辅助定位弹窗、句柄与界面异常。",
            en: "Window module inspects system windows and associated processes to locate popup, handle, and UI anomalies."
        },
        highlights: {
            zh: [
                "可展示窗口标题、句柄、所属 PID。",
                "支持对目标窗口执行基础控制动作。",
                "可联动进程页进行进一步分析。"
            ],
            en: [
                "Can show title, handle, and owner PID.",
                "Supports basic control actions for selected window.",
                "Can link with Process module for deep analysis."
            ]
        },
        workflow: {
            zh: [
                "刷新窗口列表并筛选目标窗口。",
                "执行定位或控制动作观察反馈。",
                "联动进程页确认进程级信息。"
            ],
            en: [
                "Refresh window list and filter target window.",
                "Execute location/control action and observe feedback.",
                "Cross-check process-level details in Process module."
            ]
        },
        screenshotPlan: [
            { file: "tab_window_main.png", goal: { zh: "窗口主界面", en: "Window main view" } }
        ]
    },
    {
        slug: "registry",
        page: "registry.html",
        title: { zh: "注册表", en: "Registry" },
        summary: {
            zh: "支持树浏览、键值编辑、导入导出和异步搜索。",
            en: "Supports tree browsing, value editing, import/export, and async search."
        },
        tags: {
            zh: ["配置", "搜索", "导入导出"],
            en: ["Configuration", "Search", "Import/Export"]
        },
        overview: {
            zh: "注册表模块覆盖从导航到批量迁移的完整流程，是系统配置排障的关键工具。",
            en: "Registry module spans full workflow from navigation to batch migration, essential for system configuration troubleshooting."
        },
        highlights: {
            zh: [
                "支持树形导航、地址栏跳转与历史回退。",
                "支持新建、编辑、重命名、删除键值。",
                "支持异步全文搜索与 .reg 导入导出。"
            ],
            en: [
                "Supports tree navigation, path jump, and history navigation.",
                "Supports create/edit/rename/delete for keys and values.",
                "Supports async search and .reg import/export."
            ]
        },
        workflow: {
            zh: [
                "定位目标路径并检查值表内容。",
                "执行编辑或批量搜索定位关键项。",
                "需要迁移时导出或导入 .reg 文件。"
            ],
            en: [
                "Navigate to target path and inspect value table.",
                "Edit entries or run search to locate key items.",
                "Export/import .reg files for migration scenarios."
            ]
        },
        screenshotPlan: [
            { file: "tab_registry_tree_values.png", goal: { zh: "树与值表界面", en: "Tree and value table view" } },
            { file: "tab_registry_search.png", goal: { zh: "注册表搜索结果", en: "Registry search result" } },
            { file: "tab_registry_edit_dialog.png", goal: { zh: "值编辑对话框", en: "Value edit dialog" } }
        ]
    },
    {
        slug: "current-operation",
        page: "current-operation.html",
        title: { zh: "当前操作", en: "Current Operation" },
        summary: {
            zh: "集中展示后台任务进度卡片，便于跟踪耗时动作状态。",
            en: "Centralized progress cards for tracking long-running background operations."
        },
        tags: {
            zh: ["进度", "任务", "状态"],
            en: ["Progress", "Task", "Status"]
        },
        overview: {
            zh: "当前操作模块承接所有 kProgress 任务，让用户随时观察每项流程的阶段与完成度。",
            en: "Current Operation aggregates all kProgress tasks so users can track stage and completion at any time."
        },
        highlights: {
            zh: [
                "自动刷新任务卡片内容。",
                "显示步骤文本与百分比进度。",
                "任务完成后自动隐藏，保持面板整洁。"
            ],
            en: [
                "Automatically refreshes task card content.",
                "Displays step text and percentage progress.",
                "Auto-hides completed tasks for clean panel view."
            ]
        },
        workflow: {
            zh: [
                "执行耗时动作后切换到当前操作页。",
                "观察步骤变化判断是否卡顿。",
                "完成后返回业务页确认最终结果。"
            ],
            en: [
                "Open this tab after triggering long operations.",
                "Watch step changes to detect stalls early.",
                "Return to business tab after completion."
            ]
        },
        screenshotPlan: [
            { file: "tab_current_operation_cards.png", goal: { zh: "任务卡片列表", en: "Task card list" } }
        ]
    },
    {
        slug: "log-output",
        page: "log-output.html",
        title: { zh: "日志输出", en: "Log Output" },
        summary: {
            zh: "实时日志查看与追踪中心，支持过滤、复制、导出与链路追踪。",
            en: "Real-time log center with filtering, copy, export, and call-chain tracking."
        },
        tags: {
            zh: ["日志", "追踪", "审计"],
            en: ["Logging", "Tracking", "Audit"]
        },
        overview: {
            zh: "日志输出模块基于统一日志管理器，适合进行问题复现、定位和审计留档。",
            en: "Log Output module is built on the unified logger for reproduction, root-cause analysis, and auditing."
        },
        highlights: {
            zh: [
                "支持日志级别过滤，减少噪声。",
                "支持按 GUID 追踪同一调用链。",
                "支持导出日志并双确认清空。"
            ],
            en: [
                "Supports level-based filtering to reduce noise.",
                "Supports GUID tracking for one call chain.",
                "Supports log export and double-confirm clearing."
            ]
        },
        workflow: {
            zh: [
                "先设置级别过滤锁定关键日志。",
                "对异常日志启动 GUID 追踪。",
                "导出日志给开发或运维复盘。"
            ],
            en: [
                "Set level filter to focus on important logs.",
                "Start GUID tracking from suspicious entries.",
                "Export logs for developer or ops review."
            ]
        },
        screenshotPlan: [
            { file: "tab_log_output_table.png", goal: { zh: "日志表格与过滤区", en: "Log table and filters" } },
            { file: "tab_log_output_context_menu.png", goal: { zh: "日志右键菜单", en: "Log context menu" } }
        ]
    },
    {
        slug: "immediate-window",
        page: "immediate-window.html",
        title: { zh: "即时窗口", en: "Immediate Window" },
        summary: {
            zh: "预留即时交互面板，可扩展脚本回显、临时命令与快速结果查看。",
            en: "Reserved instant interaction panel for script output, temporary commands, and quick results."
        },
        tags: {
            zh: ["预留", "扩展", "交互"],
            en: ["Reserved", "Extension", "Interaction"]
        },
        overview: {
            zh: "即时窗口模块作为扩展位，用于承载高频临时操作与快速反馈结果展示。",
            en: "Immediate Window serves as an extension slot for high-frequency temporary operations and quick feedback."
        },
        highlights: {
            zh: [
                "可承载脚本执行输出。",
                "可作为临时命令交互区域。",
                "可与日志模块配合定位问题。"
            ],
            en: [
                "Can host script execution output.",
                "Can serve as temporary command interface.",
                "Can work with Log Output for troubleshooting."
            ]
        },
        workflow: {
            zh: [
                "在即时窗口执行轻量调试指令。",
                "观察回显判断是否继续深挖。",
                "必要时切换到专业模块继续分析。"
            ],
            en: [
                "Run lightweight debug commands here.",
                "Use immediate output to decide next action.",
                "Switch to specialized modules for deep analysis."
            ]
        },
        screenshotPlan: [
            { file: "tab_immediate_window_main.png", goal: { zh: "即时窗口当前界面", en: "Immediate window current view" } }
        ]
    },
    {
        slug: "monitor-panel",
        page: "monitor-panel.html",
        title: { zh: "监视面板", en: "Monitor Panel" },
        summary: {
            zh: "底部监视区预留位，可承载全局告警、状态摘要与跨模块联动看板。",
            en: "Reserved bottom monitor area for global alerts, status summary, and cross-module dashboards."
        },
        tags: {
            zh: ["预留", "总览", "看板"],
            en: ["Reserved", "Overview", "Dashboard"]
        },
        overview: {
            zh: "监视面板模块用于形成全局可视化看板，帮助用户在一处观察关键运行状态。",
            en: "Monitor Panel is designed for global visual dashboards so users can observe key runtime status in one place."
        },
        highlights: {
            zh: [
                "可展示跨模块告警与统计计数。",
                "可承载底部统一状态看板。",
                "便于联动日志与任务进度信息。"
            ],
            en: [
                "Can display cross-module alerts and counters.",
                "Can host a unified bottom status dashboard.",
                "Facilitates integration with logs and progress data."
            ]
        },
        workflow: {
            zh: [
                "执行关键动作时关注面板动态。",
                "发现异常后跳转到对应业务模块。",
                "固定关键指标用于长期观察。"
            ],
            en: [
                "Observe panel dynamics during key operations.",
                "Jump to target module when anomalies appear.",
                "Pin critical indicators for long-term monitoring."
            ]
        },
        screenshotPlan: [
            { file: "tab_monitor_panel_main.png", goal: { zh: "监视面板当前界面", en: "Monitor panel current view" } }
        ]
    }
];

// findTabBySlug 函数用途：
// - 根据 slug 返回对应模块数据对象；
// - 若不存在则返回 undefined。
window.findTabBySlug = function findTabBySlug(tabSlug) {
    return window.kswordTabs.find(function matchTab(tabItem) {
        return tabItem.slug === tabSlug;
    });
};
