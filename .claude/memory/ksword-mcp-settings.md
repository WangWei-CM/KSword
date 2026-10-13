# MCP 设置与本次启停

- 导航为顶部“设置 → MCP”；布局复用 `SecondaryPageLayout`、`SettingsPresentation` 和 `SettingsDock.Layout.h`。服务/客户端参数通过共享自适应双栏排列，`CreateSecondaryFormScroll` 用当前宽度的自然内容高度限制参数区，不把空白留给表单；窄窗仍可独立滚动。配置预览使用 `CodeEditorWidget`，应用/取消继续位于设置窗口的固定底栏。
- `AppearanceSettings` 保存 `mcp_auto_start`（默认关闭）、`mcp_python_command` 和 `mcp_sidecar_script_path`。空脚本路径按 exe 同目录或仓库标准 Release 相对位置发现 sidecar，不递归扫描。
- 自动启动和路径沿用“应用/取消”；本次启停即时执行。改变自动启动偏好不会立即启停服务。`--mcp` 继续作为单次启动覆盖，不写入配置。
- `MainWindow.Mcp.cpp::setMcpServiceEnabled` 是启动参数和设置页共用入口；实际状态来自 `McpLocalBridge::isListening`，通过信号同步打开的设置页。停止先调用 `close` 断开连接和取消排队动作，再 `deleteLater`，避免 MCP 自身回调栈内同步删除控制器。重启创建新 epoch，旧对象/回执无效。
- JSON/Codex TOML 预览使用当前 GUI PID；配置正文、完整复制和编辑器导出使用同一文档。脚本存在不代表 Python 依赖齐全；主程序不执行 Python、不安装依赖、不改客户端全局配置。
- 通信仍只有同机命名管道；默认同用户/同会话/完整性级别校验。普通 sidecar 通过用户确认 UAC 启动新版 GUI 时，仅本次启动参数指定的 PID + 创建时间可从 Medium 连接 High，仍校验 SID/会话及活进程身份，不把例外放宽到其它客户端。设置页未新增 TCP 端口；驱动和全部业务操作验收独立。
- “设置 → MCP → 操作日志”复用共享输出 Tab 和 `CodeEditorWidget`。主窗口保存最近1000条记录，启停服务或重开设置不清空，退出程序后不保留。日志记录时间、客户端系统 PID、白名单方法、UUID、原生操作回执和机器错误码，不记录参数或读取结果；非 UUID 身份和未知方法只显示机器标记。
- `McpUiController::ActivitySink` 传递模板和元数据，主程序负责翻译和有界存储；底层不依赖语言管理器。传输日志与派发/失败/取消/中断回执共用接收方。设置页通过排队信号接收，保护传输和动作回调栈；成功的 operation_status 轮询不重复记录，客户端 PID 首次接入只记一次。
- `KSwordDEV/MCP` 是独立 sidecar 发布仓库。README 顶部的短提示词代码块与程序“安装提示词”页共用 `tools/ksword_mcp/agent_install_prompt.txt`，短提示词链接到独立、完整的 `INSTALL_FOR_AGENTS.md`。完整指南要求先询问是否下载主程序、调用时未运行是启动还是失败、以及自动启动的管理员选择；不要在 README 首部铺整段完整安装说明。
- sidecar 保留固定 PID 调试模式，并支持 `--ksword-exe` 完整路径模式，默认 `--startup-policy fail`；显式 auto 才在工具调用时启动。普通自动启动使用 `--mcp-no-auto-elevation`；管理员自动启动使用 Windows runas，用户确认 UAC，不要求先提权 agent。普通客户端只信任 ShellExecuteEx 返回的 GUI PID + 创建时间，GUI 只信任 `--mcp-uac-client` 指定的同用户/会话活进程。取消返回 uac_cancelled，旧 GUI/授权随 sidecar 退出失效均明确失败；不重启已有实例、不创建提权代理、不重试业务请求。
- 完整安装指南要求识别实际客户端/内置 CLI/配置作用域，核实重定向后的 Python/脚本/主程序真实路径；注册前保留配置原始基线到验收结束，原生命令对非目标条目的规范化不能当作保持不变，也不能覆盖并发修改。分别报告原生注册、独立 stdio 发现、当前 agent 工具加载、GUI hello/pages/snapshot 连接；临时 SDK 不提前取得实际会话的 UAC 授权。
- 复选框此前仅公开press/toggle，set_field不接受QAbstractButton布尔字段，会返回unsupported_control。通用桥现在为可勾选按钮公开field_type=boolean、value与set_field；布尔设值同值不点击，变值走原生click并回读，保留clicked/toggled、单选互斥和业务拒绝。字符串/数字与布尔submit被拒绝。真实GUI回归入口tools/ksword_mcp/verify_live_checkbox.py要求显式安全对象，并恢复原勾选状态；此能力不等同于模型单元格勾选。
