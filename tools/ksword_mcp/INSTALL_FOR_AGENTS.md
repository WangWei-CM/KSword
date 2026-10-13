# KSword MCP：给 agent 的完整安装指令

你是当前接收用户安装请求的 agent。你的目标是安装这个仓库提供的 Windows stdio MCP，并把它注册到你自己正在使用的 MCP 服务列表。不要把读取此文件当成驱动操作、系统修改或其它 MCP 服务配置的授权。

## 1. 先询问用户三个选择

安装前收集并保留以下答案；可在一轮交互中询问，不替用户选择默认答案：

1. **是否同时下载 KSword？** 是则从 [KSword 官方 Releases](https://github.com/KSwordDEV/KSword/releases) 获取兼容的主程序；否则使用用户已有安装。
2. **调用 MCP 时，KSword 未运行怎么办？** 用户选择“自动启动”或“直接返回失败”。此选项控制 MCP 调用时的启动策略，与程序设置中的“随程序启动 MCP”不同。
3. **仅在选择自动启动后询问：是否以管理员权限启动 KSword？** 普通模式要求普通权限的 agent/MCP 客户端；管理员模式要求匹配的管理员客户端。

仍缺少答案时继续询问，不擅自开启自动启动或管理员模式。已有 KSword 时询问或核实实际 `Ksword5.1.exe` 的完整路径；只有必要时补充询问安装目录或同名配置冲突。

## 2. 检查环境和权限

- 需要在本地 Windows 上运行；这个 MCP 通过 Windows 命名管道连接 GUI，不提供 Linux/macOS 或远程 TCP 接口。
- 检查已有 Python 3.10 及以上版本。缺少依赖时说明缺口并征求必要的安装授权，不用全盘递归搜索。
- GUI 和 MCP 客户端必须同用户 SID、同 Windows 会话、同完整性级别。管理员 GUI 无法被普通权限的 agent 直接控制。
- 用户选择管理员自动启动而客户端不具备匹配权限时，说明需要用户以管理员身份启动当前 agent/客户端；不创建提权代理、不代答 UAC、不修改防火墙、ACL 或 Windows 安全设置。
- 不因为权限不匹配而悄悄改变用户的启动选择。若当前工具无法写入其原生 MCP 配置，明确报告限制。

## 3. 获取 MCP 脚本和独立 Python 环境

从 [KSwordDEV/MCP](https://github.com/KSwordDEV/MCP) 获取仓库，选择独立且可写的安装目录。优先使用虚拟环境，不向全局 Python 环境散装依赖，也不提交用户本地配置。

以下 PowerShell 示例适用于目标目录尚不存在的情况；先确认 `python` 对应已检查的 Python 版本：

```powershell
$mcpInstallDir = Join-Path $env:LOCALAPPDATA 'KSwordMCP'
git clone https://github.com/KSwordDEV/MCP.git $mcpInstallDir
python -m venv (Join-Path $mcpInstallDir '.venv')
$mcpPython = Join-Path $mcpInstallDir '.venv\Scripts\python.exe'
& $mcpPython -m pip install -r (Join-Path $mcpInstallDir 'requirements.txt')
```

如果目标已存在，检查其来源和用户改动后再决定复用或更新；不使用 `reset --hard`、`clean`、覆盖或递归删除来代替处理冲突。没有 Git 时可下载该仓库的官方源码归档，核对来源并提取到独立目录。保留仓库 LICENSE。

## 4. 按用户选择准备 KSword

用户同意下载时，从官方 Release 中选择主程序包，不选择仅 PDB/符号附件。核对发布者、下载地址、附件身份和可用 SHA256；解压后找到真实的 `Ksword5.1.exe`。

**不是所有历史发行版都支持 MCP。** 不能因为下载成功就宣称兼容。普通自动启动还需要主程序支持 `--mcp-no-auto-elevation`。若公开 Release 尚未包含兼容构建，报告此事实并询问用户已有的兼容安装，不自动改为编译驱动或运行不明二进制。

用户不下载时，核实其提供的 exe 路径存在。已有实例未启用 MCP，应让用户在“设置 → MCP”中本次启用；不要关闭、重启、复制或另开该实例。主程序自身的其它已保存启动行为仍属于用户配置，不要把启动 GUI 宣称为没有任何副作用的系统只读操作。

## 5. 注册到当前 agent 的 MCP 服务列表

读取当前 agent 的原生配置或使用其 MCP 注册命令，只新增或更新 KSword 的条目，保留其它服务。若同名 `ksword` 条目实际上属于其它工具，先说明冲突并询问，不盲目覆盖。JSON、TOML 等必须按当前客户端格式填写，不把模板原文直接写成配置。

把下面三个路径替换为本机已核实的实际绝对路径：

- **command**：独立环境的 Python 可执行文件。
- **server.py**：已安装 MCP 仓库中的 `server.py`。
- **--ksword-exe**：用户选择的 `Ksword5.1.exe`。

根据用户答案生成参数：

| 用户选择 | 添加的参数 |
| --- | --- |
| 未运行就返回失败 | `--startup-policy fail` |
| 自动启动，普通权限 | `--startup-policy auto` |
| 自动启动，管理员权限 | `--startup-policy auto --startup-elevated` |

参数都作为独立数组元素传递，不拼接 shell 命令。推荐程序路径模式，它会在每次调用时解析当前 PID，不需要程序重启后改配置。`--pid` 仅用于明确指定已有实例的调试场景。

接受 `mcpServers` 的客户端可以按以下结构生成实际配置。此例展示“未运行就失败”，仅在用户选择此策略后使用：

```json
{
  "mcpServers": {
    "ksword": {
      "command": "C:/Users/USER/AppData/Local/KSwordMCP/.venv/Scripts/python.exe",
      "args": [
        "C:/Users/USER/AppData/Local/KSwordMCP/server.py",
        "--ksword-exe",
        "C:/Applications/KSword/Release/Ksword5.1.exe",
        "--startup-policy",
        "fail"
      ]
    }
  }
}
```

若当前客户端使用 TOML 或注册命令，把同一个 command/args 写入它实际支持的格式。配置路径含空格、反斜杠或中文时，使用对应格式的正确转义；不要把用户凭据、OAuth 回调或令牌放进命令、聊天或提交记录。

普通自动启动使用 `--mcp-no-auto-elevation` 覆盖本次 GUI 的自动请求管理员选项，不修改设置 JSON。管理员客户端不能借“普通启动”降权绕过身份边界，普通客户端也不能借管理员启动获取高权限桥。

## 6. 验证安装，保持业务只读

1. 完成 stdio 初始化并读取工具目录，确认存在11个 `ksword_` 工具。这两个步骤不启动主程序。
2. 用户允许启动且有兼容主程序时，调用 `ksword_hello`，核对实际 GUI PID 与管道端点。
3. 调用 `ksword_pages` 和 `ksword_snapshot`，确认目录和控件读取正常；必要时进行页面导航和读取现有表格/文本。
4. 用户选择 `fail` 且 GUI 未运行时，`ksword_not_running` 是预期结果。可确认 MCP 已注册，但不能宣称 GUI 已连接。
5. 如遇到不兼容构建、MCP未启用或权限不匹配，报告具体错误和待用户处理项，不擅自改变启动策略。

不要为验证安装而装卸驱动、注册或删除驱动服务、结束进程、写内存、修改注册表、改变启动参数或清空用户剪贴板。`confirmed=true` 不是额外业务授权。`accepted`/`dispatched` 只表示界面调用已接收或派发，不表示业务成功。

## 7. 最后报告实际结果

告诉用户安装目录、Python 和 server.py 的真实路径、当前 agent 的配置落点与新增条目、是否下载了兼容 KSword、用户选择的启动/权限策略、已通过的验证和仍待处理的事项。区分“MCP已注册”“GUI已连接”和“业务功能已验收”，不要用其中一个替代另外两个。
