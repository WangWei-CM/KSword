"""协议、参数、原生超时与官方 SDK stdio 回归；不启动产品 GUI 或驱动。"""

from __future__ import annotations

import asyncio
import ctypes
import gc
import json
import os
from pathlib import Path
import sys
import threading
import time
import unittest
from ctypes import wintypes

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from mcp.server.fastmcp.exceptions import ToolError

from protocol import (BridgeError, MAX_REQUEST_BYTES, MAX_RESPONSE_BYTES,
                      bounded_value, decode_response, encode_request, parent_path,
                      pipe_path, require_confirmation, validate_pid)
from server import create_server
from winpipe import (BridgeClient, ERROR_IO_PENDING, FILE_FLAG_OVERLAPPED,
                     INVALID_HANDLE_VALUE, Overlapped, PipeTimeouts,
                     WindowsPipeTransport, native_api, transfer)


def response_frame(request_id: str, status: str = "ok", **extra: object) -> bytes:
    """测试回执工厂；输入 ID/状态及覆写字段，返回包含换行的帧。"""
    response = {"schemaVersion": 1, "id": request_id, "status": status, "data": {}}
    response.update(extra)
    return (json.dumps(response, separators=(",", ":")) + "\n").encode()


class RecordingTransport:
    """记录调用次数和最后参数；回执可设成错误或 ID 不匹配以验证失败路径。"""

    def __init__(self, status: str = "ok", mismatch: bool = False) -> None:
        self.status = status
        self.mismatch = mismatch
        self.calls: list[dict] = []

    def exchange(self, frame: bytes) -> bytes:
        """记录一帧；产生与请求配对的测试回执，无任何真实 GUI 副作用。"""
        request = json.loads(frame)
        self.calls.append(request)
        request_id = "unexpected" if self.mismatch else request["id"]
        return response_frame(request_id, self.status, error={"code": "stale_revision"})


class ProtocolTests(unittest.TestCase):
    """验证有限帧、严格 JSON 和调用者输入，不依赖 Windows 原生服务。"""

    def test_pid_and_local_endpoint(self) -> None:
        """布尔、负数、超大 PID 和任意路径均不能进入固定 endpoint。"""
        self.assertEqual(pipe_path(123), r"\\.\pipe\ksword-mcp-123")
        for invalid in (True, 0, -1, 4294967296, "123", 1.0):
            with self.subTest(invalid=invalid), self.assertRaises(BridgeError):
                validate_pid(invalid)

    def test_utf8_request_and_unique_ids(self) -> None:
        """中文参数保留 UTF-8，每次请求都有独立 UUID。"""
        first_id, frame = encode_request("set_field", {"value": "测试"})
        second_id, _ = encode_request("set_field", {"value": "测试"})
        self.assertNotEqual(first_id, second_id)
        self.assertEqual(json.loads(frame)["params"]["value"], "测试")
        self.assertEqual(frame.count(b"\n"), 1)

    def test_request_budget_and_non_json(self) -> None:
        """编码前后拒绝非标准 JSON 与超预算字符串，避免管道发送半帧。"""
        for params in ({"value": "x" * MAX_REQUEST_BYTES}, {"value": float("nan")}, {"value": object()}):
            with self.subTest(params_type=type(params["value"])), self.assertRaises(BridgeError):
                encode_request("set_field", params)

    def test_response_budget(self) -> None:
        """超 2 MiB 回执在 JSON 解析之前失败。"""
        with self.assertRaisesRegex(BridgeError, "response_too_large"):
            decode_response(b"x" * (MAX_RESPONSE_BYTES + 1), "request")

    def test_framing_and_malformed_json(self) -> None:
        """截断、多帧、坏编码、数组、重复键和 NaN 均不可成为有效回执。"""
        frames = [b"{}", b"{}\n{}\n", b"{\xff}\n", b"{oops}\n", b"[]\n",
                  b'{"schemaVersion":1,"schemaVersion":1}\n', b'{"x":NaN}\n']
        for frame in frames:
            with self.subTest(frame=frame), self.assertRaises(BridgeError):
                decode_response(frame, "request")

    def test_strict_envelope(self) -> None:
        """schemaVersion bool、错误 ID、未知状态和非对象 data 都应失败。"""
        alterations = ({"schemaVersion": True}, {"schemaVersion": 2}, {"id": "other"},
                       {"status": "done"}, {"data": []})
        for alteration in alterations:
            with self.subTest(alteration=alteration), self.assertRaises(BridgeError):
                decode_response(response_frame("request", **alteration), "request")

    def test_error_machine_code_and_accepted(self) -> None:
        """错误码不回显自由文本；accepted 被保留，不冒充业务完成。"""
        frame = response_frame("request", "error", error={"code": "stale_revision", "message": "secret"})
        with self.assertRaises(BridgeError) as captured:
            decode_response(frame, "request")
        self.assertEqual(captured.exception.code, "stale_revision")
        self.assertNotIn("secret", str(captured.exception))
        self.assertEqual(decode_response(response_frame("request", "accepted"), "request")["status"], "accepted")
        for error in ({}, {"code": "unexpected message"}, {"code": "x" * 97}):
            with self.subTest(error=error), self.assertRaises(BridgeError):
                decode_response(response_frame("request", "error", error=error), "request")

    def test_no_action_retry(self) -> None:
        """ID 不匹配和 GUI 错误各只发送一次，避免重复危险动作。"""
        for transport in (RecordingTransport(mismatch=True), RecordingTransport(status="error")):
            with self.assertRaises(BridgeError):
                BridgeClient(1, transport).call("action", {"object_id": "x", "confirmed": True})
            self.assertEqual(len(transport.calls), 1)

    def test_parameter_boundaries(self) -> None:
        """确认只接受真布尔；树路径和字段赋值均有结构及大小预算。"""
        for invalid in (False, 1, "true", None):
            with self.assertRaises(BridgeError):
                require_confirmation(invalid)
        require_confirmation(True)
        self.assertEqual(parent_path(None), [])
        self.assertEqual(parent_path([0, 8]), [0, 8])
        for invalid in ([True], [-1], [0] * 33, "0"):
            with self.assertRaises(BridgeError):
                parent_path(invalid)
        for invalid in ({}, [], "x" * 16001, float("inf"), 2 ** 53):
            with self.assertRaises(BridgeError):
                bounded_value(invalid)

    def test_timeout_configuration(self) -> None:
        """无限、零、负数和长时间预算不可开启原生等待。"""
        for invalid in (0, -1, 61, float("inf"), float("nan")):
            with self.assertRaises(ValueError):
                PipeTimeouts(read=invalid)


class SDKTests(unittest.IsolatedAsyncioTestCase):
    """用真实 FastMCP 工具执行和官方 ClientSession 验证 stdio 服务。"""

    async def test_checkbox_values_preserve_json_type(self) -> None:
        """布尔勾选值完整转发，字符串true不能被SDK变成布尔并绕过GUI字段类型校验。"""
        transport = RecordingTransport(status="accepted")
        server = create_server(BridgeClient(1, transport))
        for value in (True, False, "true", "false"):
            with self.subTest(value=value):
                await server.call_tool("ksword_set_field", {"object_id": "check", "value": value, "confirmed": True})
                actual = transport.calls[-1]["params"]["value"]
                self.assertEqual(actual, value)
                self.assertIs(type(actual), type(value))

    async def test_tools_and_confirmation(self) -> None:
        """检查十一个 schema、默认拒绝动作和严格 bool，成功动作保留参数。"""
        transport = RecordingTransport(status="accepted")
        server = create_server(BridgeClient(1, transport))
        tools = {tool.name: tool for tool in await server.list_tools()}
        self.assertEqual(len(tools), 11)
        self.assertTrue(all(name.startswith("ksword_") for name in tools))
        self.assertTrue(tools["ksword_pages"].annotations.readOnlyHint)
        self.assertTrue(tools["ksword_action"].annotations.destructiveHint)
        self.assertFalse(tools["ksword_action"].inputSchema["properties"]["confirmed"]["default"])
        for name, arguments in (
            ("ksword_action", {"object_id": "x"}),
            ("ksword_set_field", {"object_id": "x", "value": "test"}),
            ("ksword_select_row", {"object_id": "x", "row": 0, "revision": "r"}),
            ("ksword_action", {"object_id": "x", "confirmed": 1}),
            ("ksword_action", {"object_id": "x", "confirmed": "true"}),
        ):
            with self.subTest(name=name), self.assertRaises(ToolError):
                await server.call_tool(name, arguments)
        self.assertEqual(transport.calls, [])
        await server.call_tool("ksword_action", {"object_id": "x", "confirmed": True})
        self.assertEqual(transport.calls[-1]["params"], {"object_id": "x", "action": "press", "confirmed": True})

    async def test_bounds_rejected_before_transport(self) -> None:
        """正式 SDK 参数校验和辅助预算都在发送管道请求之前执行。"""
        transport = RecordingTransport()
        server = create_server(BridgeClient(1, transport))
        for name, arguments in (
            ("ksword_snapshot", {"max_nodes": 2001}),
            ("ksword_read_table", {"object_id": "x", "parent_rows": [0] * 33}),
            ("ksword_read_table", {"object_id": "x", "row_count": 200, "column_count": 64}),
            ("ksword_read_text", {"object_id": "x", "length": 65537}),
            ("ksword_set_field", {"object_id": "x", "value": "x" * 16001, "confirmed": True}),
        ):
            with self.subTest(name=name), self.assertRaises(ToolError):
                await server.call_tool(name, arguments)
        self.assertEqual(transport.calls, [])

    async def test_official_stdio_initialize_list_and_error(self) -> None:
        """启动真实 sidecar stdio 子进程，完成握手、list_tools 和工具错误回传。"""
        script = str(Path(__file__).with_name("server.py"))
        parameters = StdioServerParameters(command=sys.executable,
                                           args=[script, "--pid", "4294967295", "--connect-timeout", "0.1"])
        async with stdio_client(parameters) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                initialized = await session.initialize()
                self.assertEqual(initialized.serverInfo.name, "KSword")
                listed = await session.list_tools()
                self.assertEqual(len(listed.tools), 11)
                rejected = await session.call_tool("ksword_action", {"object_id": "x"})
                self.assertTrue(rejected.isError)
                self.assertIn("confirmation_required", rejected.content[0].text)
                disconnected = await session.call_tool("ksword_pages", {})
                self.assertTrue(disconnected.isError)
                expected = "pipe_connection_timeout" if os.name == "nt" else "unsupported_platform"
                self.assertIn(expected, disconnected.content[0].text)

    async def test_custom_adapter_json_options(self) -> None:
        """只读自绘适配器只接收JSON对象；非JSON数值和超帧预算在发送前失败。"""
        transport = RecordingTransport()
        server = create_server(BridgeClient(1, transport))
        tools = {tool.name: tool for tool in await server.list_tools()}
        custom = tools["ksword_read_custom"]
        self.assertTrue(custom.annotations.readOnlyHint)
        self.assertEqual(custom.inputSchema["properties"]["options"]["type"], "object")
        self.assertEqual(set(custom.inputSchema["properties"]), {"object_id", "options"})
        options = {"address": "0x1000", "length": 16}
        await server.call_tool("ksword_read_custom", {"object_id": "canvas", "options": options})
        self.assertEqual(transport.calls[-1]["method"], "read_custom")
        self.assertEqual(transport.calls[-1]["params"], {"object_id": "canvas", "options": options})
        await server.call_tool("ksword_read_custom", {"object_id": "canvas"})
        self.assertEqual(transport.calls[-1]["params"]["options"], {})
        before = len(transport.calls)
        for invalid in ([], "{}", {"value": float("nan")}, {"value": object()},
                        {"value": "x" * MAX_REQUEST_BYTES}):
            with self.subTest(invalid_type=type(invalid)), self.assertRaises(ToolError):
                await server.call_tool("ksword_read_custom", {"object_id": "canvas", "options": invalid})
        with self.assertRaises(ToolError):
            await server.call_tool("ksword_read_custom", {"object_id": "canvas", "class": "QObject"})
        self.assertEqual(len(transport.calls), before)

    async def test_cancelled_calls_hold_bounded_concurrency_slots(self) -> None:
        """MCP调用取消不提前释放原生线程槽，额外调用只能有限排队后失败。"""
        release = threading.Event()
        running = threading.Event()
        lock = threading.Lock()
        count = 0

        class BlockingClient:
            """阻塞到测试释放的有限假客户端，用于制造四个仍在执行的 I/O。"""

            def call(self, method: str, params: dict) -> dict:
                """记录并发调用，输入方法参数无副作用，返回正常的测试数据。"""
                nonlocal count
                with lock:
                    count += 1
                    if count == 4:
                        running.set()
                release.wait(3)
                return {"fixture": True}

        server = create_server(BlockingClient())
        tasks = [asyncio.create_task(server.call_tool("ksword_pages", {})) for _ in range(4)]
        try:
            self.assertTrue(await asyncio.to_thread(running.wait, 1))
            tasks[0].cancel()
            with self.assertRaises(asyncio.CancelledError):
                await tasks[0]
            with self.assertRaisesRegex(ToolError, "bridge_busy"):
                await server.call_tool("ksword_pages", {})
            self.assertEqual(count, 4)
        finally:
            release.set()
            await asyncio.gather(*tasks, return_exceptions=True)
        await server.call_tool("ksword_pages", {})
        self.assertEqual(count, 5)


@unittest.skipUnless(os.name == "nt", "Windows native pipe regression")
class NativePipeTests(unittest.TestCase):
    """仅用测试进程内的模拟管道验证异步读写与取消，不访问生产 GUI。"""

    def start_fixture(self, mode: str, endpoint_pid: int | None = None) -> tuple[threading.Thread, threading.Event, list[Exception]]:
        """创建当前测试 PID 的管道；模式控制正常、读超时或写超时回执。"""
        api = native_api()
        api.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                         wintypes.DWORD, ctypes.c_void_p]
        api.CreateNamedPipeW.restype = wintypes.HANDLE
        api.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
        api.ConnectNamedPipe.restype = wintypes.BOOL
        handle = api.CreateNamedPipeW(pipe_path(endpoint_pid or os.getpid()), 3 | FILE_FLAG_OVERLAPPED,
                                      8, 1, 1024, 1024, 0, None)
        self.assertNotEqual(handle, INVALID_HANDLE_VALUE)
        stop = threading.Event()  # stop 让超时夹具在客户端返回后立即关闭。
        errors: list[Exception] = []

        def serve() -> None:
            """夹具线程有限接收一帧；所有异步 I/O 复用生产 transfer 的资源规则。"""
            event = api.CreateEventW(None, True, False, None)
            overlapped = Overlapped()
            overlapped.hEvent = event
            try:
                connected = api.ConnectNamedPipe(handle, ctypes.byref(overlapped))
                error = ctypes.get_last_error()
                if not connected and error == ERROR_IO_PENDING:
                    if api.WaitForSingleObject(event, 3000) != 0:
                        api.CancelIoEx(handle, ctypes.byref(overlapped))
                        api.WaitForSingleObject(event, 1000)
                        raise RuntimeError("fixture connect timeout")
                elif not connected and error != 535:
                    raise RuntimeError(f"fixture connect error {error}")
                if mode in ("write_timeout", "identity_mismatch"):
                    stop.wait(3)
                    return
                request = bytearray()
                while b"\n" not in request:
                    buffer = ctypes.create_string_buffer(4096)
                    count = transfer(api, handle, buffer, 4096, write=False,
                                     deadline=time.monotonic() + 3)
                    request.extend(buffer.raw[:count])
                if mode == "read_timeout":
                    stop.wait(3)
                    return
                decoded = json.loads(request)
                response = response_frame(decoded["id"], data={"fixture": True})
                # 两次写入验证客户端正确累积分片，而非假定一次 ReadFile 得到完整帧。
                for part in (response[:11], response[11:]):
                    buffer = ctypes.create_string_buffer(part)
                    transfer(api, handle, buffer, len(part), write=True,
                             deadline=time.monotonic() + 3)
            except Exception as exc:
                errors.append(exc)
            finally:
                api.CloseHandle(event)
                api.CloseHandle(handle)

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        self.addCleanup(stop.set)
        self.addCleanup(lambda: worker.join(4))
        return worker, stop, errors

    def test_native_fragmented_round_trip(self) -> None:
        """真实 Win32 命名管道双向传输、PID 校验和 UTF-8 帧配对。"""
        worker, stop, errors = self.start_fixture("normal")
        response = BridgeClient(os.getpid()).call("hello", {"unicode": "中文"})
        worker.join(4)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])
        self.assertTrue(response["data"]["fixture"])

    def test_native_read_timeout_is_bounded(self) -> None:
        """服务端不回执时读取按预算取消；不等服务端业务结束。"""
        worker, stop, errors = self.start_fixture("read_timeout")
        started = time.monotonic()
        with self.assertRaisesRegex(BridgeError, "pipe_timeout"):
            BridgeClient(os.getpid(), timeouts=PipeTimeouts(read=0.1)).call("hello", {})
        self.assertLess(time.monotonic() - started, 1.5)
        stop.set()
        worker.join(4)
        self.assertEqual(errors, [])

    def test_native_write_timeout_is_bounded(self) -> None:
        """小管道缓冲区且服务端不读时写入按预算取消，避免阻塞工具进程。"""
        worker, stop, errors = self.start_fixture("write_timeout")
        started = time.monotonic()
        with self.assertRaisesRegex(BridgeError, "pipe_timeout"):
            BridgeClient(os.getpid(), timeouts=PipeTimeouts(write=0.1)).call("set_field", {"value": "x" * 60000})
        self.assertLess(time.monotonic() - started, 1.5)
        stop.set()
        worker.join(4)
        self.assertEqual(errors, [])

    def test_native_connect_timeout_is_bounded(self) -> None:
        """不存在的 PID 只有限尝试连接，未发送任何请求。"""
        started = time.monotonic()
        with self.assertRaisesRegex(BridgeError, "pipe_connection_timeout"):
            BridgeClient(4294967295, timeouts=PipeTimeouts(connect=0.1)).call("hello", {})
        self.assertLess(time.monotonic() - started, 1.0)

    def test_native_server_identity_cannot_be_spoofed(self) -> None:
        """同用户创建了正确名称的管道，真实服务进程号不同也不能发送请求。"""
        target_pid = os.getpid() + 1
        worker, stop, errors = self.start_fixture("identity_mismatch", target_pid)
        with self.assertRaisesRegex(BridgeError, "server_pid_mismatch"):
            BridgeClient(target_pid).call("action", {"object_id": "x", "confirmed": True})
        stop.set()
        worker.join(4)
        self.assertEqual(errors, [])

    def test_native_timeout_reclaims_handles(self) -> None:
        """读取取消后回收客户端 pipe/event 和夹具 pipe/event，句柄数恢复基线。"""
        api = native_api()
        api.GetCurrentProcess.argtypes = []
        api.GetCurrentProcess.restype = wintypes.HANDLE
        api.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        api.GetProcessHandleCount.restype = wintypes.BOOL

        def handle_count() -> int:
            """返回当前测试进程的句柄数，用于检测取消路径泄漏。"""
            count = wintypes.DWORD()
            self.assertTrue(api.GetProcessHandleCount(api.GetCurrentProcess(), ctypes.byref(count)))
            return count.value

        # 基线也先回收前一项测试的不可达对象，避免只在结束时收集导致句柄数下降。
        gc.collect()
        before = handle_count()
        worker, stop, errors = self.start_fixture("read_timeout")
        with self.assertRaisesRegex(BridgeError, "pipe_timeout"):
            BridgeClient(os.getpid(), timeouts=PipeTimeouts(read=0.05)).call("hello", {})
        stop.set()
        worker.join(4)
        self.assertEqual(errors, [])
        self.assertFalse(worker.is_alive())
        # 先释放夹具的 Thread/Event 及 cleanup 引用；Python 3.10 自身的锁也使用句柄。
        self.doCleanups()
        del worker, stop
        gc.collect()
        self.assertEqual(handle_count(), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
