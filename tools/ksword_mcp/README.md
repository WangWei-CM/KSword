# KSword Qt 对象桥 MCP

这个 sidecar 使用 [官方 MCP Python v1 SDK](https://github.com/modelcontextprotocol/python-sdk/tree/v1.26.0)，以 stdio 向 MCP 客户端提供 11 个工具，再通过本机命名管道操作 KSword 的 Qt 控件。GUI 与 sidecar 是独立进程。

## 启动

需要 Windows、Python 3.10 及以上和匹配当前源码构建的 KSword 主程序。先在仓库根目录安装依赖：

```powershell
python -m pip install -r tools\ksword_mcp\requirements.txt
```

仅本次启动明确加 `--mcp` 的 GUI 会开启对象桥。如果主程序已有运行实例，先正常关闭该实例，再启动新实例，以免单实例跳转到未启用桥的旧实例：

```powershell
$kswordProcess = Start-Process -FilePath '.\Ksword5.1\x64\Release\Ksword5.1.exe' -ArgumentList '--mcp' -PassThru
$kswordProcess.Id
```

以控制台输出的实际 GUI PID 为准。连接前核对运行进程，避免填入 Launcher 或已退出进程的 PID。手动运行 sidecar 的命令如下；它会等待标准输入中的 MCP 请求，通常应由 MCP 客户端代为启动：

```powershell
python tools\ksword_mcp\server.py --pid 12345
```

GUI PID 每次重启可能变化，需要更新客户端参数。sidecar 不自动发现、启动或提权 GUI，也不调用驱动接口。首次 GUI 启动、切页和按钮所触发的业务行为仍与用户手动操作一致。

## 客户端配置示例

接受 `mcpServers` 配置的客户端可添加以下条目。将 Python 路径、仓库路径与 `12345` 替换为本机实际值；这只是配置示例，本工具不会改用户全局配置：

```json
{
  "mcpServers": {
    "ksword": {
      "command": "python",
      "args": [
        "C:/path/to/KSword/tools/ksword_mcp/server.py",
        "--pid",
        "12345"
      ]
    }
  }
}
```

Codex 的对应 [TOML 配置](https://developers.openai.com/codex/mcp/)示例（请放进需要启用此桥的配置文件）：

```toml
[mcp_servers.ksword]
command = "python"
args = ["C:/path/to/KSword/tools/ksword_mcp/server.py", "--pid", "12345"]
```

如果 MCP 客户端找不到 `python`，将 `command` 换成安装了上述依赖的 Python 可执行文件完整路径。GUI 和 sidecar 必须满足桥的同用户、同会话及完整性级别准入条件；管理员 GUI 需要在匹配权限的可信客户端中启动 sidecar。

## 工具与操作顺序

| 工具 | 用途 |
| --- | --- |
| `ksword_hello` | 读取桥版本、PID 和能力说明 |
| `ksword_pages` | 列出页面和懒加载状态 |
| `ksword_open_page(page_id)` | 通过原有导航打开页面并初始化内容 |
| `ksword_snapshot(page_id?, root_id?, max_nodes?, depth?)` | 获取有界 Qt 控件树、支持动作与可用状态 |
| `ksword_action(object_id, action?, confirmed?)` | `press`、`toggle` 或 `show_menu` |
| `ksword_set_field(object_id, value, submit?, confirmed?)` | 模拟支持控件的用户编辑；可请求原有提交动作 |
| `ksword_read_table(object_id, row_start?, row_count?, column_start?, column_count?, parent_rows?)` | 获取模型切片、表头与 revision |
| `ksword_select_row(object_id, row, revision, parent_rows?, confirmed?)` | 仅在模型 revision 相符时选中对应行 |
| `ksword_read_text(object_id, offset?, length?)` | 按 Qt UTF-16 单位读取有界文本片段 |
| `ksword_read_custom(object_id, options?)` | 读取主程序预注册的特殊控件只读适配器 |
| `ksword_operation_status(operation_id)` | 查询控件调用调度状态 |

先调用 `ksword_pages`、`ksword_open_page` 和 `ksword_snapshot` 获取当前对象 ID，之后读取或操作对应控件。对象 ID 是本次进程中的不透明身份，控件销毁后失效；再次创建的控件应重新取 snapshot。树模型用 `parent_rows` 指定父节点路径，选行时同时传入最近一次 `read_table` 返回的 revision。

`action`、`set_field` 和 `select_row` 必须显式传 `confirmed=true`，否则返回 MCP 工具错误。参数先按工具 JSON schema 校验，字符串 `"true"` 或 `"{}"` 不会被转换成布尔确认或对象选项；未知顶层参数会被拒绝。原有业务确认弹窗仍然保留，需按 GUI 正常流程处理。这个确认参数表示调用方已授权当前控件动作，不会绕过 KSword 的业务校验。

回执 `status=accepted` 以及 operation 状态 `queued`、`dispatching`、`dispatched`、`failed` 只描述控件调用。`dispatched` 表示完成了控件方法调用，后台任务可能仍在执行，也可能正在等待业务弹窗。应另行读取对应表格、文本或页面状态判断业务结果。

特殊控件通过主程序预注册的只读适配器扩展。`ksword_read_custom` 只接收对象 ID 和 JSON 对象选项，GUI 按对象实际 Qt 类匹配适配器；客户端不能指定类名、调用任意方法或执行反射。新增只读控件支持只需在主程序注册适配器，无需修改中央协议或 sidecar。

目前 `HexCanvas` 适配器接受 `options={"address":"0x1000","length":16}`，其中地址可省略，默认使用光标地址，长度为 1 至 256 字节。回执包含画布当前已加载字节、地址和选区状态；它不会主动从进程读取内存或写入内容。读取其他特殊控件时，应先查看 snapshot 中支持的动作，再按对应适配器选项调用。

自绘画布、专用编辑器和特殊业务操作可能需要额外适配。当前通用桥及已注册适配器覆盖其支持的 Qt 控件语义，不能把工具存在或点击已调度解释为全部产品功能已经验收。

## 本地边界与超时

- 唯一本地端点为 `\\.\pipe\ksword-mcp-{pid}`；GUI 只在显式 `--mcp` 启动时监听，退出时关闭。不会创建网络端口或永久服务。
- sidecar 用 `GetNamedPipeServerProcessId` 核验服务器进程与 `--pid` 一致，GUI 校验连接方身份及会话/权限边界。
- 每个工具调用独立连接，一次发送、一帧回执。动作从不自动重试；超时可能发生在 GUI 已接受动作之后，调用方应先检查状态再决定下一步。
- 请求上限 64 KiB，回执上限 2 MiB，包括末尾换行。UTF-8、JSON、schemaVersion、状态与 UUID 均严格验证。单次表格最多读取 2048 个单元格；GUI 另外限制每格 1024 个 UTF-16 单位、整片文本 131072 个单位，截断状态由原回执标示。
- 默认连接 2 秒、写入 2 秒、完整读取 10 秒。读写使用 OVERLAPPED + 有限事件等待；超时调用 `CancelIoEx` 并回收句柄。特殊延迟取消期间保留缓冲区直到内核完成，避免释放仍在使用的内存。
- 最多四个仍在执行的管道调用；额外 MCP 请求只排队一秒，随后返回 `bridge_busy`。调用方取消不会提前释放仍在执行的原生 I/O 并发槽。
- 可用 `--connect-timeout`、`--write-timeout`、`--read-timeout` 调整每阶段预算，每项必须大于 0 且不超过 60 秒。多次读写分片共用该阶段截止时刻。

## 验证

从仓库根目录运行：

```powershell
python -m unittest discover -s tools\ksword_mcp -p test_sidecar.py -v
```

回归验证帧预算、严格 JSON、ID 配对、无动作重试、参数与确认边界；Windows 回归创建测试进程自己的临时命名管道，检查分片读写、三个阶段的有限超时、服务端身份校验和取消后的句柄回收。官方 `ClientSession` 还会启动真实 sidecar 子进程，验证 stdio 握手、`list_tools` 和 `isError` 回传。

这组测试不会启动产品 GUI 或加载驱动。真实 Qt 对象桥夹具的跨进程测试由仓库独立测试入口负责；产品主程序、驱动和全部业务页面的验收需要分别进行。
