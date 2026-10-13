"""KSword MCP stdio sidecar；官方 FastMCP 负责握手、工具 schema 和错误响应。"""

from __future__ import annotations

import argparse
import asyncio
from pathlib import Path
from typing import Annotated, Any, Literal

import jsonschema
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from mcp.types import ToolAnnotations
from pydantic import Field

from .protocol import (BridgeError, bounded_id, bounded_int, bounded_value,
                      parent_path, require_confirmation, validate_pid)
from .winpipe import BridgeClient, PipeTimeouts
from .launcher import ManagedBridgeClient, StartupOptions
from .cli import CliClient, MAX_IOCTL_BYTES
from . import __version__

# 类型别名用于官方 SDK 自动生成严格的工具参数 schema。
ObjectId = Annotated[str, Field(min_length=1, max_length=256)]
OptionalId = Annotated[str, Field(max_length=256)]
StrictBool = Annotated[bool, Field(strict=True)]
RowIndex = Annotated[int, Field(strict=True, ge=0, le=2147483647)]
READ_ONLY = ToolAnnotations(readOnlyHint=True, destructiveHint=False,
                            idempotentHint=True, openWorldHint=False)
MUTATION = ToolAnnotations(readOnlyHint=False, destructiveHint=True,
                           idempotentHint=False, openWorldHint=False)
NAVIGATION = ToolAnnotations(readOnlyHint=False, destructiveHint=False,
                             idempotentHint=True, openWorldHint=False)


class StrictFastMCP(FastMCP):
    """保留官方 MCP 实现，但在 SDK 字符串预解析之前按公开 schema 校验原始参数。"""

    async def call_tool(self, name: str, arguments: dict[str, Any]) -> Any:
        """输入原始工具名与参数；拒绝类型变换及未知顶层参数，再返回官方工具结果。"""
        for tool in await self.list_tools():
            if tool.name != name:
                continue
            schema = tool.inputSchema.copy()  # 仅拷贝 schema 顶层，不修改 SDK 工具元数据。
            schema["additionalProperties"] = False
            try:
                jsonschema.validate(arguments, schema)
            except jsonschema.ValidationError:
                # 不输出含参数值的原始校验异常，避免日志泄漏字段内容。
                raise ToolError("invalid_params: Tool arguments must match the declared JSON schema") from None
            break
        return await super().call_tool(name, arguments)


def create_server(client: BridgeClient | ManagedBridgeClient | None,
                  cli_client: CliClient | None = None) -> FastMCP:
    """GUI 和 CLI 独立配置；只读发现不启动任何程序，stdout 专供 MCP。"""
    mcp = StrictFastMCP(
        "KSword",
        instructions=(
            "Operate the explicitly enabled KSword GUI through a local Qt object bridge. "
            "Inspect pages and snapshots before acting. Object IDs expire on destruction. "
            "All GUI mutations require confirmed=true. Native business dialogs remain active. "
            "accepted/operation_status describe GUI dispatch only, never business completion. "
            "After a timeout, inspect state before any new action; actions are never retried."
            " CLI tools do not need the GUI. Arbitrary CLI commands and raw driver IOCTLs "
            "require confirmed=true and retain native permissions. Raw IOCTL success is "
            "transport completion only; response ABI and business status are caller-owned."
        ),
        log_level="WARNING",
    )
    slots = asyncio.BoundedSemaphore(4)  # 最多四个仍在执行的原生 I/O，防止线程池排队无界。
    # auto 模式的首次工具调用可能启动主程序，不能继续声明整个调用绝对只读。
    read_annotations = NAVIGATION if getattr(client, "may_start_gui", False) else READ_ONLY

    def release_cancelled_worker(worker: asyncio.Task[dict[str, Any]]) -> None:
        """调用方取消后等原生线程真正结束再释放并发槽，并消费无人接收的异常。"""
        if not worker.cancelled():
            worker.exception()
        slots.release()

    async def invoke(method: str, params: dict[str, Any]) -> dict[str, Any]:
        """把有限原生管道调用移到工作线程；BridgeError 由 SDK 编码为工具错误。"""
        try:
            if client is None:
                raise BridgeError("gui_not_configured", "Use CLI tools or configure the GUI bridge")
            await asyncio.wait_for(slots.acquire(), timeout=1.0)
        except asyncio.TimeoutError as exc:
            raise ToolError("bridge_busy: Local bridge concurrency queue timed out") from exc
        worker = asyncio.create_task(asyncio.to_thread(client.call, method, params))
        deferred_release = False  # 已取消的调用仍占槽直到其原生线程完成。
        try:
            return await asyncio.shield(worker)
        except asyncio.CancelledError:
            deferred_release = True
            worker.add_done_callback(release_cancelled_worker)
            raise
        except BridgeError as exc:
            raise ToolError(str(exc)) from exc
        finally:
            if not deferred_release:
                slots.release()

    # ksword_hello：无输入，返回当前桥的版本、PID 和能力回执。
    @mcp.tool(annotations=read_annotations)
    async def ksword_hello() -> dict[str, Any]:
        """Query bridge version, PID and capabilities; auto policy may start KSword and request UAC. Does not load a driver itself."""
        return await invoke("hello", {})

    # ksword_pages：无输入，返回页面目录；不会自动打开懒加载页。
    @mcp.tool(annotations=read_annotations)
    async def ksword_pages() -> dict[str, Any]:
        """List navigable GUI pages, including currently uninitialized lazy pages."""
        return await invoke("pages", {})

    # ksword_open_page：page_id 指定页面，输出原有导航执行回执。
    @mcp.tool(annotations=NAVIGATION)
    async def ksword_open_page(page_id: ObjectId) -> dict[str, Any]:
        """Activate one page using normal GUI navigation and initialize its lazy content."""
        bounded_id(page_id, "page_id")
        return await invoke("open_page", {"page_id": page_id})

    # ksword_snapshot：页面/根对象与节点、深度预算为输入，返回有界平铺控件树。
    @mcp.tool(annotations=read_annotations)
    async def ksword_snapshot(
        page_id: OptionalId = "",
        root_id: OptionalId = "",
        max_nodes: Annotated[int, Field(strict=True, ge=1, le=2000)] = 400,
        depth: Annotated[int, Field(strict=True, ge=0, le=32)] = 12,
    ) -> dict[str, Any]:
        """Read a bounded Qt object tree; nodes include IDs, accessibility names and supported actions."""
        bounded_id(page_id, "page_id", optional=True)
        bounded_id(root_id, "root_id", optional=True)
        bounded_int(max_nodes, "max_nodes", 1, 2000)
        bounded_int(depth, "depth", 0, 32)
        return await invoke("snapshot", {"page_id": page_id, "root_id": root_id,
                                         "max_nodes": max_nodes, "depth": depth})

    # ksword_action：对象 ID、动作名称及明确确认作为输入，输出单次动作接收回执。
    @mcp.tool(annotations=MUTATION)
    async def ksword_action(
        object_id: ObjectId,
        action: Literal["press", "toggle", "show_menu"] = "press",
        confirmed: StrictBool = False,
    ) -> dict[str, Any]:
        """Invoke a supported GUI action once, with confirmed=true; existing GUI dialogs remain active."""
        require_confirmation(confirmed)
        bounded_id(object_id, "object_id")
        return await invoke("action", {"object_id": object_id, "action": action,
                                       "confirmed": confirmed})

    # ksword_set_field：目标、JSON 标量及提交/确认选项为输入，返回用户编辑调用回执。
    @mcp.tool(annotations=MUTATION)
    async def ksword_set_field(
        object_id: ObjectId,
        value: str | bool | int | float | None,
        submit: StrictBool = False,
        confirmed: StrictBool = False,
    ) -> dict[str, Any]:
        """Set a supported Qt field through its user-edit semantics. Use true/false for checkboxes, radio buttons and checkable buttons; repeated setting preserves the chosen state. submit requests a text/numeric commit and is unsupported for boolean fields."""
        require_confirmation(confirmed)
        bounded_id(object_id, "object_id")
        bounded_value(value)
        return await invoke("set_field", {"object_id": object_id, "value": value,
                                          "submit": submit, "confirmed": confirmed})

    # ksword_read_table：目标及父行路径、窗口参数为输入，返回切片和模型修订身份。
    @mcp.tool(annotations=read_annotations)
    async def ksword_read_table(
        object_id: ObjectId,
        row_start: RowIndex = 0,
        row_count: Annotated[int, Field(strict=True, ge=1, le=200)] = 40,
        column_start: RowIndex = 0,
        column_count: Annotated[int, Field(strict=True, ge=1, le=64)] = 16,
        parent_rows: list[RowIndex] | None = None,
    ) -> dict[str, Any]:
        """Read a model window; retain revision and parent_rows when later selecting a row."""
        bounded_id(object_id, "object_id")
        bounded_int(row_start, "row_start", 0, 2147483647)
        bounded_int(row_count, "row_count", 1, 200)
        bounded_int(column_start, "column_start", 0, 2147483647)
        bounded_int(column_count, "column_count", 1, 64)
        # 行列各自合法仍可能组合超预算；先拒绝再连接，避免请求伪装成较小分页。
        if row_count * column_count > 2048:
            raise BridgeError("invalid_params", "Table window must contain at most 2048 cells")
        return await invoke("read_table", {"object_id": object_id, "row_start": row_start,
                                           "row_count": row_count, "column_start": column_start,
                                           "column_count": column_count,
                                           "parent_rows": parent_path(parent_rows)})

    # ksword_select_row：目标行、读取时修订和父路径为输入，返回有条件选行回执。
    @mcp.tool(annotations=MUTATION)
    async def ksword_select_row(
        object_id: ObjectId,
        row: RowIndex,
        revision: ObjectId,
        parent_rows: list[RowIndex] | None = None,
        confirmed: StrictBool = False,
    ) -> dict[str, Any]:
        """Select a model row only if its read_table revision still matches; confirmed=true is required."""
        require_confirmation(confirmed)
        bounded_id(object_id, "object_id")
        bounded_id(revision, "revision")
        bounded_int(row, "row", 0, 2147483647)
        return await invoke("select_row", {"object_id": object_id, "row": row,
                                           "revision": revision,
                                           "parent_rows": parent_path(parent_rows),
                                           "confirmed": confirmed})

    # ksword_read_text：目标及 UTF-16 范围为输入，返回有界文字片段与原有状态。
    @mcp.tool(annotations=read_annotations)
    async def ksword_read_text(
        object_id: ObjectId,
        offset: RowIndex = 0,
        length: Annotated[int, Field(strict=True, ge=1, le=65536)] = 4096,
    ) -> dict[str, Any]:
        """Read a bounded visible text range; offsets use the Qt bridge's UTF-16 string units."""
        bounded_id(object_id, "object_id")
        bounded_int(offset, "offset", 0, 2147483647)
        bounded_int(length, "length", 1, 65536)
        return await invoke("read_text", {"object_id": object_id, "offset": offset,
                                          "length": length})

    # ksword_read_custom：目标及适配器选项为输入，返回主程序预注册控件的只读快照。
    @mcp.tool(annotations=read_annotations)
    async def ksword_read_custom(
        object_id: ObjectId,
        options: dict[str, Any] = {},
    ) -> dict[str, Any]:
        """Read a registered custom control adapter; class selection stays in the GUI, with no reflection."""
        bounded_id(object_id, "object_id")
        if type(options) is not dict:
            raise BridgeError("invalid_params", "options must be a JSON object")
        # options 不在客户端解释或修改；固定适配器只读当前已加载数据，编码时核验预算。
        return await invoke("read_custom", {"object_id": object_id, "options": options.copy()})

    # ksword_operation_status：operation_id 为输入，返回控件调用状态而非业务完成状态。
    @mcp.tool(annotations=read_annotations)
    async def ksword_operation_status(operation_id: ObjectId) -> dict[str, Any]:
        """Query queued/dispatching/dispatched/failed control invocation state; verify business results separately."""
        bounded_id(operation_id, "operation_id")
        return await invoke("operation_status", {"operation_id": operation_id})

    # CLI-only 配置不暴露不可用的 GUI 动作，使用官方 SDK 的公开注册管理入口。
    if client is None:
        for name in ("ksword_hello", "ksword_pages", "ksword_open_page", "ksword_snapshot",
                     "ksword_action", "ksword_set_field", "ksword_read_table", "ksword_select_row",
                     "ksword_read_text", "ksword_read_custom", "ksword_operation_status"):
            mcp.remove_tool(name)

    if cli_client is not None:
        async def invoke_cli(method: str, **params: Any) -> dict[str, Any]:
            """原生 CLI 同样占有有限槽；调用方取消后等待真实 worker 结束，不隐式重发。"""
            try:
                await asyncio.wait_for(slots.acquire(), timeout=1.0)
            except asyncio.TimeoutError as exc:
                raise ToolError("cli_busy: CLI concurrency queue timed out") from exc
            worker = asyncio.create_task(asyncio.to_thread(getattr(cli_client, method), **params))
            deferred_release = False
            try:
                return await asyncio.shield(worker)
            except asyncio.CancelledError:
                deferred_release = True
                worker.add_done_callback(release_cancelled_worker)
                raise
            except BridgeError as exc:
                raise ToolError(str(exc)) from exc
            finally:
                if not deferred_release:
                    slots.release()

        @mcp.tool(annotations=READ_ONLY)
        async def ksword_cli_status() -> dict[str, Any]:
            """Read configured native CLI path and availability; does not start GUI or load a driver."""
            return cli_client.status()

        @mcp.tool(annotations=READ_ONLY)
        async def ksword_cli_help(command: Annotated[list[str], Field(max_length=12)] = []) -> dict[str, Any]:
            """Read native help for a command path such as ['system', 'ioctl', 'call']; no device operation."""
            return await invoke_cli("help", command=command)

        @mcp.tool(annotations=MUTATION)
        async def ksword_cli_execute(arguments: Annotated[list[str], Field(min_length=1, max_length=128)],
                                     confirmed: StrictBool = False,
                                     json_output: StrictBool = True) -> dict[str, Any]:
            """Execute one native CLI argv vector. Every arbitrary command needs confirmed=true; GUI is not required. Pass native --confirm when its help requires it. Results include actual exit code, stdout/stderr and parsed JSON when available. Never automatically retry."""
            require_confirmation(confirmed)
            return await invoke_cli("execute", arguments=arguments, confirmed=confirmed, json_output=json_output)

        @mcp.tool(annotations=MUTATION)
        async def ksword_ioctl(code: Annotated[int, Field(strict=True, ge=0, le=4294967295)],
                               input_hex: Annotated[str, Field(max_length=2097152)] = "",
                               output_bytes: Annotated[int, Field(strict=True, ge=0, le=MAX_IOCTL_BYTES)] = 4096,
                               confirmed: StrictBool = False) -> dict[str, Any]:
            """Send one raw IOCTL to the KSword control device through native ArkDriverClient; GUI is not required. Use the exact shared-driver ABI, at most 1 MiB input/output and confirmed=true. This does not load/elevate the driver or decode business status. No automatic retries."""
            require_confirmation(confirmed)
            return await invoke_cli("ioctl", code=code, input_hex=input_hex,
                                    output_bytes=output_bytes, confirmed=confirmed)

    return mcp


def parse_arguments() -> argparse.Namespace:
    """解析固定 PID 或明确程序路径，以及用户安装时选择的启动策略。"""
    parser = argparse.ArgumentParser(description="KSword GUI, CLI and raw IOCTL MCP stdio server")
    parser.add_argument("--version", action="version", version=__version__)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--pid", type=int,
                        help="PID of the KSword process explicitly started with --mcp")
    target.add_argument("--ksword-exe", type=Path, help="Absolute path to Ksword5.1.exe")
    parser.add_argument("--ksword-cli-exe", type=Path, help="Absolute path to KswordCLI.exe; can run without GUI")
    parser.add_argument("--cli-timeout", type=float, default=30.0, metavar="SECONDS")
    parser.add_argument("--startup-policy", choices=("fail", "auto"), default="fail")
    parser.add_argument("--startup-elevated", action="store_true")
    parser.add_argument("--startup-timeout", type=float, default=30.0, metavar="SECONDS")
    parser.add_argument("--connect-timeout", type=float, default=2.0, metavar="SECONDS")
    parser.add_argument("--write-timeout", type=float, default=2.0, metavar="SECONDS")
    parser.add_argument("--read-timeout", type=float, default=10.0, metavar="SECONDS")
    arguments = parser.parse_args()
    try:
        if arguments.pid is not None:
            validate_pid(arguments.pid)
            if arguments.startup_policy != "fail" or arguments.startup_elevated or arguments.startup_timeout != 30.0:
                raise ValueError("Startup options require --ksword-exe")
        elif arguments.ksword_exe is not None:
            StartupOptions(arguments.ksword_exe, arguments.startup_policy,
                           arguments.startup_elevated, arguments.startup_timeout)
        elif arguments.ksword_cli_exe is None:
            raise ValueError("Configure --pid, --ksword-exe or --ksword-cli-exe")
        elif arguments.startup_policy != "fail" or arguments.startup_elevated or arguments.startup_timeout != 30.0:
            raise ValueError("GUI startup options require --ksword-exe")
        cli_path = arguments.ksword_cli_exe
        if cli_path is None and arguments.ksword_exe is not None:
            cli_path = arguments.ksword_exe.parent / "KswordCLI.exe"
        if cli_path is not None:
            CliClient(cli_path, arguments.cli_timeout)
        PipeTimeouts(arguments.connect_timeout, arguments.write_timeout, arguments.read_timeout)
    except (BridgeError, ValueError) as exc:
        parser.error(str(exc))
    return arguments


def main() -> None:
    """从用户配置创建桥客户端；stdout 始终专供 MCP，不用于启动诊断输出。"""
    arguments = parse_arguments()
    timeouts = PipeTimeouts(arguments.connect_timeout, arguments.write_timeout,
                            arguments.read_timeout)
    if arguments.pid is not None:
        client = BridgeClient(arguments.pid, timeouts=timeouts)
    elif arguments.ksword_exe is not None:
        client = ManagedBridgeClient(StartupOptions(arguments.ksword_exe, arguments.startup_policy,
                                                   arguments.startup_elevated, arguments.startup_timeout), timeouts)
    else:
        client = None
    cli_path = arguments.ksword_cli_exe
    if cli_path is None and arguments.ksword_exe is not None:
        cli_path = arguments.ksword_exe.parent / "KswordCLI.exe"
    cli_client = CliClient(cli_path, arguments.cli_timeout) if cli_path is not None else None
    create_server(client, cli_client).run(transport="stdio")


if __name__ == "__main__":
    main()
