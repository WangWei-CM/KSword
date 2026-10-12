# MCP 设置与本次启停

- 导航为顶部“设置 → MCP”；布局复用 `SecondaryPageLayout`、`SettingsPresentation` 和 `SettingsDock.Layout.h`。服务/客户端参数通过共享自适应双栏排列，`CreateSecondaryFormScroll` 用当前宽度的自然内容高度限制参数区，不把空白留给表单；窄窗仍可独立滚动。配置预览使用 `CodeEditorWidget`，应用/取消继续位于设置窗口的固定底栏。
- `AppearanceSettings` 保存 `mcp_auto_start`（默认关闭）、`mcp_python_command` 和 `mcp_sidecar_script_path`。空脚本路径按 exe 同目录或仓库标准 Release 相对位置发现 sidecar，不递归扫描。
- 自动启动和路径沿用“应用/取消”；本次启停即时执行。改变自动启动偏好不会立即启停服务。`--mcp` 继续作为单次启动覆盖，不写入配置。
- `MainWindow.Mcp.cpp::setMcpServiceEnabled` 是启动参数和设置页共用入口；实际状态来自 `McpLocalBridge::isListening`，通过信号同步打开的设置页。停止先调用 `close` 断开连接和取消排队动作，再 `deleteLater`，避免 MCP 自身回调栈内同步删除控制器。重启创建新 epoch，旧对象/回执无效。
- JSON/Codex TOML 预览使用当前 GUI PID；配置正文、完整复制和编辑器导出使用同一文档。脚本存在不代表 Python 依赖齐全；主程序不执行 Python、不安装依赖、不改客户端全局配置。
- 通信仍只有同机命名管道及原同用户/同会话/完整性级别校验；设置页未新增 TCP 端口或可自定义的准入策略。驱动和全部业务操作的验收独立于设置页。
