```text
请在当前 agent 使用的 MCP 服务列表中安装 KSword MCP（https://github.com/KSwordDEV/MCP）。
1. 询问使用 GUI、CLI 还是两者，以及是否下载 KSword。GUI 模式询问未运行时自动启动还是失败；仅在自动启动时询问管理员权限，由我确认 Windows UAC。
2. 使用 ksword-mcp 的 pip/npm 包及独立 Python 环境，核验入口和 Ksword5.1.exe / KswordCLI.exe 的实际绝对路径；CLI 不自动启动 GUI 或提权。
3. 确认实际客户端和原生配置落点；注册前保留可恢复基线，只改 KSword 条目，核验其它配置不变，不猜原值或覆盖并发修改。
4. 按我的回答配置启动和权限策略，不自行改变选择、不重启已有 KSword。
5. 分别验证并报告原生注册、独立 stdio 工具发现、当前 agent 工具加载；GUI 使用 hello/pages/snapshot，CLI 使用 status/help 做只读连接验证。
6. 条件不满足时继续完成独立安装与发现，明确未通过项、具体错误和用户所需操作；基线保留到验收完成。
7. 验证不执行原始 IOCTL、驱动装卸、结束进程或内存写入，不创建提权代理或代答 UAC。
完整安装指南：https://raw.githubusercontent.com/KSwordDEV/MCP/main/INSTALL_FOR_AGENTS.md
```

# KSword MCP

通过 stdio 提供 KSword 的 Qt 对象桥、原生 CLI 和原始驱动 IOCTL 工具。使用 [官方 MCP Python v1 SDK](https://github.com/modelcontextprotocol/python-sdk/tree/v1.26.0)，GUI、CLI 与 sidecar 是独立进程；CLI 模式可以单独运行。

## pip / npm 安装

需要本地 Windows、Python 3.10+ 和兼容的 KSword。包提供 MCP 服务，KSword 主程序、CLI 和驱动从 [官方 Releases](https://github.com/KSwordDEV/KSword/releases) 获取。下载和 GUI 启动策略由用户选择。

pip 使用独立虚拟环境：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install ksword-mcp
.\.venv\Scripts\ksword-mcp.exe --ksword-cli-exe "C:/Applications/KSword/Release/KswordCLI.exe"
```

npm 需要 Node.js 18+ 和已有 Python 3.10+；入口在版本化用户缓存中准备独立 Python 环境，运行同一份 Python 实现。先完成 `--prepare`，再注册到 MCP 客户端，避免初次依赖安装耗尽客户端握手时间：

```powershell
npx --yes ksword-mcp@0.1.0 --prepare
npx --yes ksword-mcp@0.1.0 --ksword-cli-exe "C:/Applications/KSword/Release/KswordCLI.exe"
```

需要指定解释器时设置 `KSWORD_MCP_PYTHON` 为真实完整路径；`KSWORD_MCP_CACHE_DIR` 可指定独立缓存的绝对路径。默认缓存为用户主目录下 `.cache/ksword-mcp/<version>`。安装诊断走 stderr，stdout 保留给 MCP；npm 不改系统 Python 或 MCP 客户端的依赖。

使用这些选项时，在 MCP 服务配置的 `env` 字段显式传入，并让 `--prepare` 使用同样的值；部分客户端不会继承 PowerShell 中的环境变量。

包与源码入口都支持以下参数：

| 模式 | 参数与工具 |
| --- | --- |
| CLI | `--ksword-cli-exe C:/.../KswordCLI.exe`，4 个 CLI/IOCTL 工具，GUI 无需运行 |
| GUI + CLI | `--ksword-exe C:/.../Ksword5.1.exe`，11 个 GUI 工具及4个 CLI/IOCTL 工具；默认寻找同目录 `KswordCLI.exe`，可显式指定 CLI 路径 |
| 指定 GUI 实例 | `--pid 12345`，11 个 GUI 工具；添加 `--ksword-cli-exe` 可同时启用 CLI |

`--version` 输出包版本；`--help` 查看参数。CLI 每次调用创建一次原生命令进程，继承 MCP 服务的权限，不自动启动 GUI、装载驱动或申请管理员权限。GUI 的 UAC 授权范围保持为对象桥，CLI 的权限条件按原生命令帮助处理。

## 启动

从主仓库源码运行时，在独立环境安装整个包：

```powershell
python -m pip install ./tools/ksword_mcp
```

可从主程序顶部“设置 → MCP”即时启用或停止对象桥；默认关闭。“随程序启动”点击设置窗口底部“应用”后保存，下次启动生效。“取消”放弃待保存偏好和路径，本次启停是即时操作。也可用 `--mcp` 仅对本次启动启用桥。如果主程序已有运行实例，该参数不会改变旧实例的状态，应直接在旧实例的 MCP 设置页启用，或先正常关闭旧实例再启动：

```powershell
$kswordProcess = Start-Process -FilePath '.\Ksword5.1\x64\Release\Ksword5.1.exe' -ArgumentList '--mcp' -PassThru
$kswordProcess.Id
```

以控制台输出的实际 GUI PID 为准。连接前核对运行进程，避免填入 Launcher 或已退出进程的 PID。手动运行 sidecar 的命令如下；它会等待标准输入中的 MCP 请求，通常应由 MCP 客户端代为启动：

```powershell
python tools\ksword_mcp\server.py --pid 12345
```

固定 PID 模式下，GUI 重启后需要更新参数。推荐改用 --ksword-exe 按明确的完整映像路径寻找当前实例；默认不自动启动，用户明确选择 --startup-policy auto 后，首次工具调用才会尝试启动。sidecar 不下载主程序、不安装驱动，也不修改 KSword 设置。

MCP 设置页显示实际监听状态、当前 PID 和命名管道，可填写 Python 命令与 sidecar 脚本路径，并预览/复制下面两种客户端配置。预览使用当前输入，路径在点击“应用”后保存。主程序按标准 Release 位置自动寻找仓库中的 `tools/ksword_mcp/server.py`；安装位置不同或发行目录缺少 sidecar 时可手动选择。复制前会检查命令非空和脚本存在，但不会验证或安装 Python 依赖，也不会修改客户端全局配置。

新版配置预览在 exe 同目录存在 `KswordCLI.exe` 时追加 CLI 路径，两种格式共用同一参数数组。已有客户端条目需重新注册或手动追加 `--ksword-cli-exe` 才会加载这4个新工具；先读取原生 help 核验 CLI 版本。

“设置 → MCP → 操作日志”显示最近1000条服务、连接、请求和操作派发记录，支持共享编辑器的搜索、复制、导出以及即时清空。日志在当前主程序生命周期内保留，重开设置或启停 MCP 不会丢失，退出后不写入磁盘。记录包含时间、客户端 PID、白名单方法、请求 UUID、调用回执和机器错误码；输入值、读取结果、未知方法原文和非 UUID 请求身份不进入日志。成功的操作状态轮询不重复刷屏。`dispatched` 仍只表示控件调用派发，不能作为业务完成依据。

## Agent 安装与启动策略

README 顶部代码块和 `agent_install_prompt.txt` 是可复制给 agent 的短提示词；完整、独立的安装步骤在 [INSTALL_FOR_AGENTS.md](INSTALL_FOR_AGENTS.md)。agent 应先读取该文件，再收集用户的下载、启动和权限选择，不能用配置示例代替用户回答。

推荐使用程序路径模式，避免每次重启修改 PID：

```powershell
python server.py --ksword-exe "C:/path/to/Release/Ksword5.1.exe" --startup-policy fail
python server.py --ksword-exe "C:/path/to/Release/Ksword5.1.exe" --startup-policy auto
python server.py --ksword-exe "C:/path/to/Release/Ksword5.1.exe" --startup-policy auto --startup-elevated
```

- `fail`：默认策略，主程序未运行时返回 `ksword_not_running`，不启动任何进程。
- `auto`：只有工具调用发生后才启动；stdio 初始化和工具发现不启动主程序。默认启动等待30秒，可用 `--startup-timeout` 调整，最大120秒。
- `--startup-elevated`：仅适用于 auto；通过 Windows `runas` 显示 UAC，由用户确认，无需先把 agent 改为管理员。取消返回 `uac_cancelled`，不自动重试。
- 普通 agent 的 UAC 启动需要新版 `--mcp-uac-client` 支持，授权仅绑定本次 sidecar 的真实 PID 与创建时间。GUI 和 sidecar 仍须同用户、同会话；其它普通进程及 PID 复用者被拒绝，不创建提权代理、不持久化授权。
- sidecar 退出或重新加载后授权失效；已有未授权管理员 GUI 仍会拒绝普通客户端。脚本不会为重新授权关闭、重启或另开已有实例，应报告给用户处理。
- 普通 auto 使用 `--mcp-no-auto-elevation` 单次覆盖主程序保存的自动申请管理员选项，不改 JSON；需要包含此开关的当前 MCP 构建。管理员客户端不能悄悄将“普通启动”变成管理员启动，会返回 `normal_client_required`。
- 只匹配指定 exe 的完整路径，并复核用户、会话和完整性级别；已有实例未启用 MCP 时返回 `mcp_not_enabled`，不关闭或重启它；多个可用实例时要求改用 `--pid` 明确选择。
- 主程序退出后再次调用会重新发现 PID；业务请求只发送一次，失败或超时不自动重试。auto 模式的工具声明会反映可能启动 GUI 的副作用。

主程序可从 [官方 Releases](https://github.com/KSwordDEV/KSword/releases) 获取。稳定版可能早于 MCP，应同时检查用户允许使用的 pre-release，选择说明明确兼容的主程序包，避开仅 PDB 的附件。没有兼容构建时仍可完成注册和独立工具发现，明确 GUI 连接待验证；下载成功不等于已经兼容。

独立发布仓库根目录就是这些文件所在目录；主仓库内仍位于 `tools/ksword_mcp/`。源码安装使用 `python -m pip install .`；旧 `server.py` 入口继续兼容，依赖旁边的 `ksword_mcp/` 包目录。Release 构建会将两者一起复制到 `Release/tools/ksword_mcp/`。

## 客户端配置示例

接受 `mcpServers` 配置的客户端可添加以下条目。将 Python 路径、脚本路径与 KSword 路径替换为本机实际值，并填入用户选择的启动策略；这只是配置示例，本工具不会改用户全局配置：

```json
{
  "mcpServers": {
    "ksword": {
      "command": "python",
      "args": [
        "C:/path/to/KSword/tools/ksword_mcp/server.py",
        "--ksword-exe",
        "C:/path/to/Release/Ksword5.1.exe",
        "--startup-policy",
        "fail"
      ]
    }
  }
}
```

Codex 的对应 [TOML 配置](https://developers.openai.com/codex/mcp/)示例（请放进需要启用此桥的配置文件）：

```toml
[mcp_servers.ksword]
command = "python"
args = ["C:/path/to/MCP/server.py", "--ksword-exe", "C:/path/to/Release/Ksword5.1.exe", "--startup-policy", "fail"]
```

如果 MCP 客户端找不到 `python`，将 `command` 换成独立环境 Python 的实际完整路径。核对真实客户端及配置作用域，保留基线到验收完成，确认其它配置不变；环境变量重定向和 PATH 中另一版本的 CLI 不能替代真实落点。普通连接匹配完整性级别，UAC 启动仅对本次指定客户端作例外。分别报告“已注册”“独立 stdio 工具发现”“当前 agent 已加载工具”“GUI 只读连接”，不能互相替代。

## 工具与操作顺序

| 工具 | 用途 |
| --- | --- |
| `ksword_hello` | 读取桥版本、PID 和能力说明 |
| `ksword_pages` | 列出页面和懒加载状态 |
| `ksword_open_page(page_id)` | 通过原有导航打开页面并初始化内容 |
| `ksword_snapshot(page_id?, root_id?, max_nodes?, depth?)` | 获取有界 Qt 控件树、支持动作与可用状态 |
| `ksword_action(object_id, action?, confirmed?)` | `press`、`toggle` 或 `show_menu` |
| `ksword_set_field(object_id, value, submit?, confirmed?)` | 模拟支持控件的用户编辑；复选框/单选及可勾选按钮使用布尔值指定状态 |
| `ksword_read_table(object_id, row_start?, row_count?, column_start?, column_count?, parent_rows?)` | 获取模型切片、表头与 revision |
| `ksword_select_row(object_id, row, revision, parent_rows?, confirmed?)` | 仅在模型 revision 相符时选中对应行 |
| `ksword_read_text(object_id, offset?, length?)` | 按 Qt UTF-16 单位读取有界文本片段 |
| `ksword_read_custom(object_id, options?)` | 读取主程序预注册的特殊控件只读适配器 |
| `ksword_operation_status(operation_id)` | 查询控件调用调度状态 |
| `ksword_cli_status()` | 读取 CLI 的真实路径、文件存在性与超时配置；不执行程序 |
| `ksword_cli_help(command?)` | 读取原生 help；例如 `["system", "ioctl", "call"]` |
| `ksword_cli_execute(arguments, confirmed?, json_output?)` | 调用一次 CLI argv，返回真实退出码、stdout/stderr 和可解析的 JSON |
| `ksword_ioctl(code, input_hex?, output_bytes?, confirmed?)` | 通过 CLI 和现有 ArkDriverClient 向 KSword 控制设备发送一次原始 IOCTL |

CLI 操作先用 `ksword_cli_status` 检查配置，再用 `ksword_cli_help` 查阅原生命令。任意 CLI 命令和原始 IOCTL 都要求 `confirmed=true`；原生命令需要的 `--confirm` 仍由调用方按帮助提供。`ksword_cli_execute` 默认加 `--json`，旧文本命令可设置 `json_output=false`。进程退出码和业务 JSON 原样返回，不把 CLI 的非零退出码当作成功。

`ksword_ioctl` 的 `code` 是 uint32，`input_hex` 是原始字节，输入/输出各上限1 MiB，输出容量默认4096字节。输入通过本次独占临时文件传给 CLI，结束后清理；只输出 `bytesReturned` 范围内的十六进制数据。编码和缓冲区 ABI 须来自配套 `shared/driver/`，需要原生指针或额外内存管理的协议使用相应原生 CLI 命令。Win32 传输成功不代替响应中的业务状态；不会自动加载驱动、提权或重发。

`--cli-timeout` 默认为30秒，最大120秒；stdout/stderr 各限8 MiB。超时仅结束本次创建的 CLI 子进程，操作结果可能未知，应读取状态后再决定下一步。

先调用 `ksword_pages`、`ksword_open_page` 和 `ksword_snapshot` 获取当前对象 ID，之后读取或操作对应控件。对象 ID 是本次进程中的不透明身份，控件销毁后失效；再次创建的控件应重新取 snapshot。树模型用 `parent_rows` 指定父节点路径，选行时同时传入最近一次 `read_table` 返回的 revision。

`action`、`set_field` 和 `select_row` 必须显式传 `confirmed=true`，否则返回 MCP 工具错误。参数先按工具 JSON schema 校验，字符串 `"true"` 或 `"{}"` 不会被转换成布尔确认或对象选项；未知顶层参数会被拒绝。原有业务确认弹窗仍然保留，需按 GUI 正常流程处理。这个确认参数表示调用方已授权当前控件动作，不会绕过 KSword 的业务校验。

回执 `status=accepted` 以及 operation 状态 `queued`、`dispatching`、`dispatched`、`failed` 只描述控件调用。`dispatched` 表示完成了控件方法调用，后台任务可能仍在执行，也可能正在等待业务弹窗。应另行读取对应表格、文本或页面状态判断业务结果。

普通复选框使用 `ksword_set_field(object_id, value=true, confirmed=true)` 勾选，`value=false` 取消；重复设置相同值不会反向切换。snapshot 对可勾选按钮提供 `field_type="boolean"`、`checked`、`value` 与 `set_field` 能力。`ksword_action(..., action="toggle")` 则是反转当前状态；不应把“指定勾选”与“反转”混用。

布尔字段只接受 JSON 的 true/false，不接受字符串 `"true"` 或数字1，且不支持 `submit=true`。修改使用控件原生 click，保留 clicked/toggled、单选互斥和业务校验；业务拒绝指定状态时回执为 `value_rejected`。表格模型单元格的勾选不等同于独立 QCheckBox，不能把表格 object_id 当作普通布尔字段使用。

特殊控件通过主程序预注册的只读适配器扩展。`ksword_read_custom` 只接收对象 ID 和 JSON 对象选项，GUI 按对象实际 Qt 类匹配适配器；客户端不能指定类名、调用任意方法或执行反射。新增只读控件支持只需在主程序注册适配器，无需修改中央协议或 sidecar。

目前 `HexCanvas` 适配器接受 `options={"address":"0x1000","length":16}`，其中地址可省略，默认使用光标地址，长度为 1 至 256 字节。回执包含画布当前已加载字节、地址和选区状态；它不会主动从进程读取内存或写入内容。读取其他特殊控件时，应先查看 snapshot 中支持的动作，再按对应适配器选项调用。

自绘画布、专用编辑器和特殊业务操作可能需要额外适配。当前通用桥及已注册适配器覆盖其支持的 Qt 控件语义，不能把工具存在或点击已调度解释为全部产品功能已经验收。

## 本地边界与超时

- 唯一本地端点为 `\\.\pipe\ksword-mcp-{pid}`；GUI 按 MCP 设置页的本次启停、已保存自动启动偏好或显式 `--mcp` 参数监听，退出时关闭。不会创建网络端口或永久服务。
- 停止立即断开当前连接，并使旧对象 ID、模型 revision 与操作回执失效；再次启用会创建新的桥实例。客户端需重新获取 snapshot，不能重用停止前的 ID。通过 MCP 触发停止时，当前连接也可能在停止回执送达前断开，不应自动重试动作。
- sidecar 用 `GetNamedPipeServerProcessId` 核验服务器进程与 `--pid` 一致，GUI 校验连接方身份及会话/权限边界。
- 每个工具调用独立连接，一次发送、一帧回执。动作从不自动重试；超时可能发生在 GUI 已接受动作之后，调用方应先检查状态再决定下一步。
- 请求上限 64 KiB，回执上限 2 MiB，包括末尾换行。UTF-8、JSON、schemaVersion、状态与 UUID 均严格验证。单次表格最多读取 2048 个单元格；GUI 另外限制每格 1024 个 UTF-16 单位、整片文本 131072 个单位，截断状态由原回执标示。
- 默认连接 2 秒、写入 2 秒、完整读取 10 秒。读写使用 OVERLAPPED + 有限事件等待；超时调用 `CancelIoEx` 并回收句柄。特殊延迟取消期间保留缓冲区直到内核完成，避免释放仍在使用的内存。
- 最多四个仍在执行的管道调用；额外 MCP 请求只排队一秒，随后返回 `bridge_busy`。调用方取消不会提前释放仍在执行的原生 I/O 并发槽。
- 可用 `--connect-timeout`、`--write-timeout`、`--read-timeout` 调整每阶段预算，每项必须大于 0 且不超过 60 秒。多次读写分片共用该阶段截止时刻。

## 验证

从独立仓库根目录运行；主仓库先进入 `tools/ksword_mcp`：

```powershell
python -m unittest discover -s . -p "test_*.py" -v
npm test
```

回归验证帧预算、严格 JSON、ID 配对、无动作重试、参数与确认边界；Windows 回归创建测试进程自己的临时命名管道，检查分片读写、三个阶段的有限超时、服务端身份校验和取消后的句柄回收。官方 `ClientSession` 还会启动真实 sidecar 子进程，验证 stdio 握手、`list_tools` 和 `isError` 回传。

这组测试不会启动产品 GUI 或加载驱动。真实 Qt 对象桥夹具的跨进程测试由仓库独立测试入口负责；产品主程序、驱动和全部业务页面的验收需要分别进行。

## 包发布

独立仓库使用 `.github/workflows/release.yml` 从 `v<version>` 标签构建并通过 GitHub OIDC 发布 npm/PyPI；版本须同时匹配 `package.json`、`pyproject.toml` 和 `ksword_mcp/__init__.py`。配置字段和操作见 [PUBLISHING.md](PUBLISHING.md)。许可证保持 [KSword Community Source License 1.6](LICENSE)。
