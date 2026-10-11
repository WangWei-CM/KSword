"""真实官方 MCP stdio -> Win32 管道 -> 生产 Qt 桥的独立夹具回归。

输入为已构建的夹具 exe 和当前 Python；输出仅写既有 output 日志。
不会启动 KSword、打开系统目标、加载驱动或修改持久环境。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
from typing import Any

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


CHECKS = 0  # 已执行的真实跨进程断言数，缺少汇总不视为通过。


def check(condition: bool, description: str) -> None:
    """记录条件与用例名；失败抛异常并由 main 返回非零。"""
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(description)


def unpack(result: Any) -> dict[str, Any]:
    """读取官方 SDK structuredContent；旧客户端形态则从文本块解析相同对象。"""
    if isinstance(result.structuredContent, dict):
        return result.structuredContent
    for block in result.content:
        if getattr(block, "type", None) == "text":
            value = json.loads(block.text)  # 只解析已返回的 JSON，不能执行结果内容。
            if isinstance(value, dict):
                return value
    raise AssertionError("MCP result has no structured bridge payload")


async def call(session: ClientSession, name: str, **arguments: Any) -> dict[str, Any]:
    """调用固定 MCP 工具；任何 SDK 或桥错误都保留为测试失败。"""
    result = await session.call_tool(name, arguments)
    diagnostic = " ".join(getattr(block, "text", "") for block in result.content) if result.isError else ""
    check(not result.isError, f"{name} returned a tool error: {diagnostic}")
    response = unpack(result)  # 包含 schemaVersion/status/data 的真实 IPC 回执。
    check(response.get("schemaVersion") == 1, f"{name} lost bridge schemaVersion")
    return response


async def expect_error(session: ClientSession, name: str, **arguments: Any) -> None:
    """错误参数必须成为工具错误，不能成功触发下游控件。"""
    result = await session.call_tool(name, arguments)
    check(bool(result.isError), f"{name} accepted invalid parameters")


def node_id(nodes: list[dict[str, Any]], identifier: str) -> str:
    """仅用真实快照中的稳定标识定位，禁止伪造对象指针或依赖行顺序。"""
    for node in nodes:
        if node.get("accessible_id") == identifier or node.get("object_name") == identifier:
            value = node.get("id", "")  # 生命周期内不透明对象身份。
            if isinstance(value, str) and value:
                return value
    raise AssertionError(f"Fixture node was not discovered: {identifier}")


async def dispatched(session: ClientSession, receipt: dict[str, Any], *,
                     expected: str = "dispatched") -> dict[str, Any]:
    """等实际 UI 调用状态；派发或拒绝仍必须声明业务完成未知。"""
    check(receipt.get("status") == "accepted", "GUI mutation did not return accepted")
    operation_id = receipt["data"]["operation_id"]  # SDK 返回的真实操作回执身份。
    deadline = time.monotonic() + 3.0  # 有界等待，仅用于合成夹具。
    while time.monotonic() < deadline:
        response = await call(session, "ksword_operation_status", operation_id=operation_id)
        status = response["data"]  # 本次 GUI 调用状态，不推断业务任务。
        if status.get("state") not in ("queued", "dispatching"):
            check(status.get("state") == expected, "GUI invocation reached unexpected state")
            check(status.get("business_completion") == "unknown", "GUI receipt claimed business completion")
            return status
        await asyncio.sleep(0.01)
    raise AssertionError("GUI dispatch did not finish within fixture budget")


async def verify(repository: Path, python: str, pid: int) -> None:
    """以官方 SDK 启动当前真实 sidecar，完成只针对夹具的功能链路。"""
    parameters = StdioServerParameters(
        command=python,
        args=[str(repository / "tools/ksword_mcp/server.py"), "--pid", str(pid)],
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        cwd=str(repository),
    )  # stdout 由官方 stdio transport 占有，未使用 shell 拼接。
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer) as session:
            await session.initialize()
            listing = await session.list_tools()  # 实际 FastMCP 工具元数据。
            tools = {tool.name: tool for tool in listing.tools}
            required = {"ksword_hello", "ksword_pages", "ksword_open_page", "ksword_snapshot",
                        "ksword_action", "ksword_set_field", "ksword_read_table",
                        "ksword_select_row", "ksword_read_text", "ksword_operation_status",
                        "ksword_read_custom"}
            check(required.issubset(tools), "MCP tool discovery is incomplete")
            check(all(tools[name].outputSchema is not None for name in required), "tools lack structured output schemas")
            hello = await call(session, "ksword_hello")
            check(hello["data"].get("pid") == pid, "hello is not served by target fixture PID")
            pages = await call(session, "ksword_pages")
            lazy = next(page for page in pages["data"]["pages"] if page["id"] == "fixture.lazy")
            check(lazy["initialized"] is False, "page enumeration constructed lazy content")

            # 原始树先发现 ID，之后每次修改都带明确 confirmed，不影响其他窗口。
            snapshot = await call(session, "ksword_snapshot")
            nodes = snapshot["data"]["nodes"]  # 完整受管合成树，不含系统窗口。
            check("fixture-secret-value" not in json.dumps(snapshot), "MCP snapshot leaked password content")
            edit_id = node_id(nodes, "fixture.integer")
            combo_id = node_id(nodes, "fixture.combo")
            table_id = node_id(nodes, "fixture.table")
            run_id = node_id(nodes, "fixture.run")
            quit_id = node_id(nodes, "fixture.quit")
            disabled_id = node_id(nodes, "fixture.disabled")
            password_id = node_id(nodes, "fixture.password")
            text_id = node_id(nodes, "fixture.text")
            label_id = node_id(nodes, "fixture.custom_label")
            await expect_error(session, "ksword_action", object_id=run_id)
            refused = await call(session, "ksword_action", object_id=disabled_id, confirmed=True)
            failure = await dispatched(session, refused, expected="failed")
            check(failure.get("error_code") == "control_unavailable", "disabled Qt target was not rejected before invocation")
            await expect_error(session, "ksword_snapshot", max_nodes=1.5)
            await expect_error(session, "ksword_set_field", object_id=edit_id, value="42", confirmed="true")

            # 改值 -> 派发回执 -> 再读真实 Qt 状态，不能只相信 accepted。
            receipt = await call(session, "ksword_set_field", object_id=edit_id, value="42", submit=True, confirmed=True)
            await dispatched(session, receipt)
            value = await call(session, "ksword_read_text", object_id=edit_id)
            check(value["data"]["text"] == "42", "MCP field edit was not applied to Qt")
            receipt = await call(session, "ksword_set_field", object_id=combo_id, value=1, confirmed=True)
            await dispatched(session, receipt)
            current = await call(session, "ksword_snapshot", root_id=combo_id)
            check(any(node.get("value") == "Beta" for node in current["data"]["nodes"]), "MCP combo edit was not reflected in snapshot")
            receipt = await call(session, "ksword_action", object_id=run_id, confirmed=True)
            await dispatched(session, receipt)

            # 模型分页提供 revision，再按同一 revision 选择实际行。
            table = await call(session, "ksword_read_table", object_id=table_id, row_count=1, column_count=2)
            check(len(table["data"]["rows"]) == 1, "MCP table exceeded requested row range")
            receipt = await call(session, "ksword_select_row", object_id=table_id, row=1,
                                 revision=table["data"]["revision"], confirmed=True)
            await dispatched(session, receipt)
            await expect_error(session, "ksword_select_row", object_id=table_id, row=0,
                               revision="old-forged-revision", confirmed=True)
            await expect_error(session, "ksword_read_table", object_id=table_id, row_count=200, column_count=64)
            text = await call(session, "ksword_read_text", object_id=text_id, offset=5, length=9)
            check(text["data"]["text"] == "56789abcd", "MCP text window does not match real Qt content")
            await expect_error(session, "ksword_read_text", object_id=password_id)
            custom = await call(session, "ksword_read_custom", object_id=label_id,
                                options={"method": "deleteLater", "className": "QWidget"})
            check(custom["data"].get("kind") == "label", "MCP custom read did not use registered Qt class")
            check(custom["data"].get("text") == "Adapter sample", "MCP custom read changed target text")

            # 显式导航才建立惰性页面，随后仅按本次打开返回根读取。
            opened = await call(session, "ksword_open_page", page_id="fixture.lazy")
            check(bool(opened["data"]["root_id"]), "lazy navigation did not return registered root")
            pages = await call(session, "ksword_pages")
            lazy = next(page for page in pages["data"]["pages"] if page["id"] == "fixture.lazy")
            check(lazy["initialized"] is True, "explicit MCP navigation did not create Qt page")
            bounded = await call(session, "ksword_snapshot", page_id="fixture.lazy", max_nodes=1, depth=0)
            check(len(bounded["data"]["nodes"]) <= 1, "MCP snapshot exceeded node budget")
            stopping = await call(session, "ksword_action", object_id=quit_id, confirmed=True)
            check(stopping.get("status") == "accepted", "fixture shutdown did not return a GUI receipt")


async def verify_destroying_host(repository: Path, python: str, pid: int) -> None:
    """请求实际 read_custom 回调删除 heap 宿主；正确结果是有限传输错误。"""
    parameters = StdioServerParameters(
        command=python,
        args=[str(repository / "tools/ksword_mcp/server.py"), "--pid", str(pid), "--read-timeout", "2"],
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}, cwd=str(repository),
    )  # 原始 Qt Core 和官方 SDK，不注入模拟响应。
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer) as session:
            await session.initialize()
            hello = await call(session, "ksword_hello")
            check(hello["data"].get("pid") == pid, "destruction case connected to wrong server PID")
            snapshot = await call(session, "ksword_snapshot")
            target = node_id(snapshot["data"]["nodes"], "fixture.destroy_label")
            result = await session.call_tool("ksword_read_custom", {"object_id": target})
            check(bool(result.isError), "destroyed Core returned a successful reply")
            diagnostic = " ".join(getattr(block, "text", "") for block in result.content)
            check("pipe_io_failed" in diagnostic or "incomplete_response" in diagnostic,
                  "destroyed heap host was hidden by an unrelated validation error")


def run_destroying_host(repository: Path, output: Path, executable: Path,
                       environment: dict[str, str], flags: int, python: str) -> None:
    """创建第二个纯 Qt 子进程，校验回调确实执行及进程没有发生 UAF 崩溃。"""
    descriptor = output / "mcp-ui-destroy-endpoint.json"  # 和普通夹具分开，不能读到旧 PID。
    log_path = output / "mcp-ui-destroy-fixture.log"  # 只包含生命周期标记，没有用户字段。
    with log_path.open("w", encoding="utf-8") as fixture_log:
        process = subprocess.Popen([str(executable), "--destroy-host", str(descriptor)],
                                   cwd=repository, env=environment, stdout=fixture_log,
                                   stderr=subprocess.STDOUT, creationflags=flags)
        try:
            deadline = time.monotonic() + 10.0  # 保持启动预算有界。
            ready = False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    connection = json.loads(descriptor.read_text(encoding="utf-8"))
                    ready = connection.get("pid") == process.pid
                except (OSError, ValueError):
                    ready = False
                if ready:
                    break
                time.sleep(0.02)
            check(ready, "heap-host fixture did not become ready")
            asyncio.run(verify_destroying_host(repository, python, process.pid))
            check(process.wait(timeout=3.0) == 0, "real Core crashed while read_custom destroyed its heap host")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3.0)
    markers = log_path.read_text(encoding="utf-8")  # 防止仅检测到连接错误却没执行回调。
    check("DESTROY_HOST_ADAPTER=ENTERED" in markers, "destructive adapter callback was not actually reached")
    check("DESTROY_HOST=1" in markers, "owned heap host was not actually destroyed")


def main() -> int:
    """准备受控子进程环境，启动独立 fixture，并记录完整端到端通过汇总。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--python", default=sys.executable)
    arguments = parser.parse_args()  # executable/Python 路径均作为参数数组使用。
    repository = Path(__file__).resolve().parent.parent
    output = repository / "output"  # 只复用既有目录，不创建临时树。
    executable = arguments.exe or output / "mcp_ui_bridge_tests.exe"
    if not output.is_dir() or not executable.is_file():
        parser.error("Existing output directory and built fixture executable are required")
    descriptor = output / "mcp-ui-bridge-endpoint.json"  # 仅本次进程连接描述。
    qt = repository / ".deps/Qt/6.9.3/msvc2022_64"
    environment = {**os.environ, "QT_QPA_PLATFORM": "offscreen",
                   "QT_PLUGIN_PATH": str(qt / "plugins"),
                   "PATH": str(qt / "bin") + os.pathsep + os.environ.get("PATH", "")}
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0  # 不弹出交互终端。
    with (output / "mcp-ui-e2e-fixture.log").open("w", encoding="utf-8") as fixture_log:
        process = subprocess.Popen([str(executable), "--serve", str(descriptor)],
                                   cwd=repository, env=environment, stdout=fixture_log,
                                   stderr=subprocess.STDOUT, creationflags=flags)
        try:
            deadline = time.monotonic() + 10.0  # 启动就绪有界等待，旧描述文件不能冒充当前子进程。
            ready = False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    connection = json.loads(descriptor.read_text(encoding="utf-8"))
                    ready = connection.get("pid") == process.pid
                except (OSError, ValueError):
                    ready = False
                if ready:
                    break
                time.sleep(0.02)
            check(ready, "Qt IPC fixture did not become ready")
            asyncio.run(verify(repository, arguments.python, process.pid))
            check(process.wait(timeout=3.0) == 0, "Qt fixture did not shut down cleanly through original button")
            run_destroying_host(repository, output, executable, environment, flags, arguments.python)
            summary = f"MCP_E2E_CHECKS={CHECKS}\nMCP_E2E_RESULT=PASS\nMCP_E2E_FIXTURE_EXIT_CODE=0\nMCP_E2E_DESTROY_HOST=PASS\n"
            (output / "mcp-ui-e2e-tests.log").write_text(summary, encoding="utf-8")
            print(summary, end="")
            return 0
        except Exception as error:
            # SDK 使用异常组传递异步失败；保留真实子异常，避免只打印笼统 TaskGroup。
            with (output / "mcp-ui-e2e-python.log").open("w", encoding="utf-8") as diagnostic_log:
                traceback.print_exception(error, file=diagnostic_log)
            failures = [error]  # 展开到具体子异常，只包含合成夹具的测试诊断。
            details = []
            while failures:
                failure = failures.pop()
                if isinstance(failure, BaseExceptionGroup):
                    failures.extend(failure.exceptions)
                else:
                    details.append(str(failure))
            summary = f"MCP_E2E_CHECKS={CHECKS}\nMCP_E2E_RESULT=FAIL\nMCP_E2E_ERROR={' | '.join(details)}\n"
            (output / "mcp-ui-e2e-tests.log").write_text(summary, encoding="utf-8")
            print(summary, file=sys.stderr, end="")
            return 1
        finally:
            # 仅停止由此脚本拥有的合成子进程；不操作 KSword 或其它系统进程。
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3.0)


if __name__ == "__main__":
    raise SystemExit(main())
