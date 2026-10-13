# KSword MCP：给 agent 的完整安装指令

你是当前接收用户安装请求的 agent。你的目标是安装这个仓库提供的 Windows stdio MCP，并把它注册到你自己正在使用的 MCP 服务列表。不要把读取此文件当成驱动操作、系统修改或其它 MCP 服务配置的授权。

## 1. 先询问用户三个选择

安装前收集并保留以下答案；可在一轮交互中询问，不替用户选择默认答案：

1. **是否同时下载 KSword？** 是则从 [KSword 官方 Releases](https://github.com/KSwordDEV/KSword/releases) 获取兼容的主程序；否则使用用户已有安装。
2. **调用 MCP 时，KSword 未运行怎么办？** 用户选择“自动启动”或“直接返回失败”。此选项控制 MCP 调用时的启动策略，与程序设置中的“随程序启动 MCP”不同。
3. **仅在选择自动启动后询问：是否以管理员权限启动 KSword？** 同时说明普通模式使用普通权限客户端；管理员模式通过 Windows UAC 请求提权，由用户亲自确认，agent 无需预先以管理员运行。需要支持本次客户端授权的新版 KSword。

仍缺少答案时继续询问，不擅自开启自动启动或管理员模式。已有 KSword 时询问或核实实际 `Ksword5.1.exe` 的完整路径；只有必要时补充询问安装目录或同名配置冲突。

## 2. 检查环境和权限

- 需要在本地 Windows 上运行；这个 MCP 通过 Windows 命名管道连接 GUI，不提供 Linux/macOS 或远程 TCP 接口。
- 检查已有 Python 3.10 及以上版本。缺少依赖时说明缺口并征求必要的安装授权，不用全盘递归搜索。
- 默认连接必须同用户 SID、同 Windows 会话、同完整性级别。普通客户端经 UAC 启动新版管理员 GUI 时，GUI 仅额外接受本次发起请求的 sidecar PID 与创建时间；授权不持久化、不授予其它普通进程。
- 管理员自动启动使用 Windows `runas`，由用户确认 UAC；取消返回 `uac_cancelled`。UAC 使用其它用户凭据会被同 SID 门禁拒绝；受限/非交互会话、旧版 GUI 或已有未授权管理员实例也可能无法连接。不要代答 UAC、创建提权代理、修改防火墙/ACL 或悄悄换成管理员 agent。
- 原普通 sidecar 退出或重新加载后，本次授权失效；新客户端不能继承旧 PID 的授权。保留已有 GUI，说明需要用户处理重新授权或自行使用匹配权限客户端，不自动关闭、重启或另开实例。
- 不因为权限不匹配而悄悄改变用户的启动选择。若当前工具无法写入其原生 MCP 配置，明确报告限制。

## 3. 确认实际客户端与配置环境

先确定用户正在使用的客户端、实际执行文件、版本、配置根目录和作用域。PATH 中的 CLI 可能与桌面内置 CLI 不同；不能只凭 `Get-Command` 找到的 CLI 或一次 MCP 列表查询判断当前桌面会话已注册。核对桌面实际使用的 CLI/配置与原生命令的读回结果；沙箱中的查询为空时，检查是否读到了另一个用户、环境变量重定向后的目录或沙箱视图。

确定将被修改的配置文件或原生配置存储后，**注册前保留可恢复基线**：原始字节、文件存在性、哈希及解析后的完整配置；敏感配置只保存在用户可访问的本地位置，不打印到聊天、日志或提交。基线必须保留到本次验收完成；若部分验收受阻，继续保留并报告位置，只有用户明确接受剩余限制或完成验收后才释放。

登记命令可能规范化其它条目，例如省略原有空参数数组。注册后对比基线，核验所有非 KSword 条目和其它设置。出现额外变化时依据真实基线修正，不能猜原值或声称逐字段一致。恢复前重新读当前文件并核对变更，保留并发修改；无法安全合并时报告冲突，不能整文件覆盖。原先没有配置文件的情况也要记录，不能制造不存在的“原值”。

## 4. 获取 MCP 脚本和独立 Python 环境

从 [KSwordDEV/MCP](https://github.com/KSwordDEV/MCP) 获取仓库，选择独立且可写的安装目录。优先使用虚拟环境，不向全局 Python 环境散装依赖，也不提交用户本地配置。

先核验真实安装目录。`%LOCALAPPDATA%` 在桌面包或沙箱中可能重定向到应用包目录；环境变量字符串不等于用户预期落点。与 Windows 用户文件夹及当前客户端环境交叉核对，解析重解析点后的实际绝对路径，确认位置可写且符合用户选择。有偏差时说明并选择用户确认的可写目录，不在错误目录继续安装。

以下 PowerShell 示例适用于目标目录尚不存在的情况；先核实系统返回的目录，以及 `python` 对应已检查的 Python 版本：

```powershell
$mcpInstallDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'KSwordMCP'
git clone https://github.com/KSwordDEV/MCP.git $mcpInstallDir
python -m venv (Join-Path $mcpInstallDir '.venv')
$mcpPython = Join-Path $mcpInstallDir '.venv\Scripts\python.exe'
& $mcpPython -m pip install -r (Join-Path $mcpInstallDir 'requirements.txt')
& $mcpPython -c "import sys; from pathlib import Path; print(Path(sys.executable).resolve(strict=True))"
(Get-Item -LiteralPath (Join-Path $mcpInstallDir 'server.py')).FullName
```

如果目标已存在，检查其来源和用户改动后再决定复用或更新；不使用 `reset --hard`、`clean`、覆盖或递归删除来代替处理冲突。没有 Git 时可下载该仓库的官方源码归档，核对来源并提取到独立目录。保留仓库 LICENSE。

再使用虚拟环境的真实 Python 路径执行只读版本与依赖检查，并核实 `server.py`、`Ksword5.1.exe` 的实际绝对路径、文件存在性和来源。以这些实测结果生成配置，不能只保留未展开的 `%LOCALAPPDATA%`、相对路径或 PATH 中不确定的 `python`。不修改用户环境变量来掩盖路径差异。

## 5. 按用户选择准备 KSword

用户同意下载时，从官方 Release 中选择主程序包，不选择仅 PDB/符号附件。核对发布者、下载地址、附件身份和可用 SHA256；解压后找到真实的 `Ksword5.1.exe`。

**不是所有历史发行版都支持 MCP。** 普通自动启动需要 `--mcp-no-auto-elevation`，普通 agent 的管理员自动启动需要 `--mcp-uac-client`。官方最新稳定版可能早于 MCP；同时检查官方 pre-release 的说明和主程序附件，只有用户同意使用相应预发行版且确认兼容后才下载。不能因为下载成功就宣称兼容；没有兼容包时完成独立注册与工具发现，明确 GUI 连接待兼容构建，不自动编译驱动或运行不明二进制。

用户不下载时，核实其提供的 exe 路径存在。已有实例未启用 MCP，应让用户在“设置 → MCP”中本次启用；不要关闭、重启、复制或另开该实例。主程序自身的其它已保存启动行为仍属于用户配置，不要把启动 GUI 宣称为没有任何副作用的系统只读操作。

## 6. 注册到当前 agent 的 MCP 服务列表

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

普通自动启动使用 `--mcp-no-auto-elevation` 覆盖本次 GUI 的自动请求管理员选项，不修改设置 JSON。管理员客户端不能借“普通启动”悄悄改变用户的权限选择。管理员自动启动由 Windows UAC 确认，授权绑定到本次发起调用的 sidecar，保留同用户、同会话和真实管道 PID 校验。

注册后重新读取**实际客户端使用的配置**，按基线核对 KSword 的 command/args 与其它配置；如果原生命令读回与实际文件不一致，先解释环境或版本差异。配置已写入不表示当前会话已经加载新工具；需要客户端重新加载时明确告知，不能以另一个 CLI 或独立 SDK 的列表代替当前 agent 的工具列表。

## 7. 分四级验证，保持业务只读

| 验收级别 | 必须取得的证据 |
| --- | --- |
| 已注册 | 当前客户端实际配置落点的原生读回、KSword 条目与非目标配置基线比对 |
| 独立 stdio 工具发现通过 | 用已安装 Python/server.py 完成 MCP 初始化和工具目录读取，发现11个 `ksword_` 工具；不启动 GUI |
| 当前 agent 已加载工具 | 当前会话实际可调用的工具列表包含 KSword；独立 SDK 或其它 CLI 列表不能替代 |
| GUI 只读连接通过 | 从当前 agent 调用 `ksword_hello`、`ksword_pages`、`ksword_snapshot`，核对真实 GUI PID、管道、页面及对象读取 |

只有前三项确认后，才从当前 agent 的长寿命 sidecar 验证 GUI，避免先用临时 SDK 进程取得 UAC 授权后退出，使实际 agent 无法继承。管理员自动启动时等待用户亲自确认 Windows 弹窗，取消后不重复弹出或自动重试工具调用。每次新的显式调用仍按保存策略处理。

用户选择 `fail` 且 GUI 未运行时，`ksword_not_running` 是预期结果，只能报告前三项相应证据，GUI 尚未连接。如遇不兼容构建、未启用 MCP、当前会话未加载工具、UAC 取消或权限不匹配，继续完成不依赖该条件的注册/独立发现步骤，逐项报告错误和用户所需动作，不擅自改变启动选择。页面导航可能初始化业务查询，必要时只读当前已存在的表格/文本。

不要为验证安装而装卸驱动、注册或删除驱动服务、结束进程、写内存、修改注册表、改变启动参数或清空用户剪贴板。`confirmed=true` 不是额外业务授权。`accepted`/`dispatched` 只表示界面调用已接收或派发，不表示业务成功。

## 8. 最后报告实际结果

告诉用户实际客户端/版本/作用域、安装目录、Python/server.py/主程序真实路径、配置落点与条目、基线保存与非目标配置比对结果、是否下载兼容 KSword、启动和 UAC 选择。对四级验收分别报告通过/未通过/未执行及具体证据，列出尚需用户处理的事项；驱动和其它业务功能验收独立报告，不能以工具发现或 GUI 连接代替。
