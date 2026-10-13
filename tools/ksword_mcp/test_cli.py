"""CLI 与原始 IOCTL 边界回归；不执行产品业务命令或真实驱动请求。"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from mcp.server.fastmcp.exceptions import ToolError

from ksword_mcp.cli import CliClient, MAX_IOCTL_BYTES, decode_cli_text, run_bounded
from ksword_mcp.protocol import BridgeError
from ksword_mcp.server import create_server, parse_arguments


class CliBoundaryTests(unittest.TestCase):
    """注入原生执行回执，核对确认、实际结果和临时输入文件生命周期。"""
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ksword-cli-tests-")
        self.addCleanup(self.directory.cleanup)
        self.executable = Path(self.directory.name) / "KswordCLI.exe"
        self.executable.touch()  # 普通空文件；只供存在性检查，绝不执行。
        self.runner = Mock(return_value=subprocess.CompletedProcess([], 0, b'{"status":"success"}', b""))
        self.client = CliClient(self.executable, runner=self.runner)

    def test_discovery_and_status_do_not_execute(self):
        self.assertTrue(self.client.status()["exists"])
        tools = asyncio.run(create_server(None, self.client).list_tools())
        self.assertEqual({tool.name for tool in tools},
                         {"ksword_cli_status", "ksword_cli_help", "ksword_cli_execute", "ksword_ioctl"})
        self.assertTrue(next(t for t in tools if t.name == "ksword_cli_help").annotations.readOnlyHint)
        self.assertFalse(next(t for t in tools if t.name == "ksword_ioctl").annotations.readOnlyHint)
        self.runner.assert_not_called()

    def test_confirmation_precedes_path_or_input_access(self):
        for confirmed in (False, "true", 1, None):
            with self.subTest(confirmed=confirmed), self.assertRaisesRegex(BridgeError, "confirmation_required"):
                self.client.ioctl(0, "invalid", 0, confirmed)
            with self.assertRaises(BridgeError):
                self.client.execute(["r0", "anything"], confirmed)
        self.runner.assert_not_called()

    def test_help_cannot_smuggle_options(self):
        for command in (["--confirm"], ["system ioctl call"], ["a\0"], ["x"] * 13):
            with self.subTest(command=command), self.assertRaises(BridgeError):
                self.client.help(command)
        self.runner.assert_not_called()
        result = self.client.help(["system", "ioctl", "call"])
        self.assertTrue(result["success"])
        self.assertEqual(self.runner.call_args.args[0][1:], ["help", "system", "ioctl", "call"])

    def test_native_failure_is_preserved_and_not_retried(self):
        self.runner.return_value = subprocess.CompletedProcess([], 3, b'{"status":"failed"}', b"native error")
        result = self.client.execute(["process", "list", "--json"], True)
        self.assertEqual(result["exit_code"], 3)
        self.assertFalse(result["success"])
        self.assertEqual(result["json"]["status"], "failed")
        self.assertEqual(result["stderr"], "native error")
        self.runner.assert_called_once()
        self.assertEqual(self.runner.call_args.args[0].count("--json"), 1)

    def test_unknown_outcome_is_not_retried(self):
        self.runner.side_effect = BridgeError("cli_timeout", "unknown outcome")
        with self.assertRaisesRegex(BridgeError, "cli_timeout"):
            self.client.execute(["system", "ioctl", "call"], True)
        self.runner.assert_called_once()

    def test_raw_input_is_exclusive_file_and_cleaned(self):
        files = []
        def native(argv, timeout):
            path = Path(argv[argv.index("--input-file") + 1])
            files.append(path)
            self.assertEqual(path.read_bytes(), bytes.fromhex("00ff1020"))
            self.assertNotIn("00ff1020", argv)
            self.assertEqual(argv.count("--confirm"), 1)
            return subprocess.CompletedProcess(argv, 0, b'{}', b"")
        self.runner.side_effect = native
        self.client.ioctl(0x80002000, "00ff1020", 0, True)
        self.assertEqual(len(files), 1)
        self.assertFalse(files[0].exists())
        self.runner.assert_called_once()

    def test_input_cleanup_after_launch_failure(self):
        files = []
        def fail(argv, timeout):
            files.append(Path(argv[argv.index("--input-file") + 1]))
            raise OSError("simulated launch failure")
        self.runner.side_effect = fail
        with self.assertRaisesRegex(BridgeError, "cli_start_failed"):
            self.client.ioctl(0, "01", 1, True)
        self.assertFalse(files[0].exists())
        self.runner.assert_called_once()

    def test_invalid_or_oversized_raw_buffers_do_not_execute(self):
        cases = ((True, "", 0), (-1, "", 0), (0x100000000, "", 0),
                 (0, "g0", 0), (0, "0", 0), (0, "00" * (MAX_IOCTL_BYTES + 1), 0),
                 (0, "", True), (0, "", -1), (0, "", MAX_IOCTL_BYTES + 1))
        for code, payload, capacity in cases:
            with self.subTest(code=code, length=len(payload), capacity=capacity), self.assertRaises(BridgeError):
                self.client.ioctl(code, payload, capacity, True)
        self.runner.assert_not_called()

    def test_arguments_preserve_literal_shell_characters(self):
        literal = 'path with space; $(echo secret) & "quoted"'
        self.client.execute(["example", literal], True, json_output=False)
        self.assertEqual(self.runner.call_args.args[0], [str(self.executable), "example", literal])
        for arguments in (["x\0y"], ["x"] * 129):
            with self.assertRaises(BridgeError):
                self.client.execute(arguments, True)

    def test_cli_only_does_not_accept_gui_startup_flags(self):
        with patch("sys.argv", ["ksword-mcp", "--ksword-cli-exe", str(self.executable)]):
            args = parse_arguments()
            self.assertIsNone(args.pid)
            self.assertIsNone(args.ksword_exe)
        with patch("sys.argv", ["ksword-mcp", "--ksword-cli-exe", str(self.executable), "--startup-elevated"]), \
                patch("sys.stderr"), self.assertRaises(SystemExit):
            parse_arguments()

    def test_text_decode_preserves_unicode_and_native_text(self):
        for encoding in ("utf-8-sig", "utf-16"):
            self.assertEqual(decode_cli_text("中文回执".encode(encoding)), "中文回执")
        self.runner.return_value = subprocess.CompletedProcess([], 0, b"plain native output", b"")
        self.assertIsNone(self.client.help([])["json"])


class BoundedProcessTests(unittest.TestCase):
    """仅执行 Python 合成输出子进程，证明超时和输出预算确实生效。"""
    def test_output_streams_and_timeout(self):
        result = run_bounded([sys.executable, "-c", "import sys;print('out');print('err',file=sys.stderr)"], 5)
        self.assertEqual(result.stdout.strip(), b"out")
        self.assertEqual(result.stderr.strip(), b"err")
        with self.assertRaisesRegex(BridgeError, "cli_timeout"):
            run_bounded([sys.executable, "-c", "import time;time.sleep(5)"], .2)

    def test_output_budget_stops_capture(self):
        with patch("ksword_mcp.cli.MAX_CLI_OUTPUT", 1024), self.assertRaisesRegex(BridgeError, "cli_output_limit"):
            run_bounded([sys.executable, "-c", "import sys;sys.stdout.buffer.write(b'x'*65536)"], 5)


class CliSdkTests(unittest.IsolatedAsyncioTestCase):
    """真实 stdio sidecar 只发现工具/查询配置，并验证确认拒绝；不执行产品 CLI。"""
    async def test_confirmed_tools_dispatch_once_with_native_arguments(self):
        client = Mock(spec=CliClient)
        client.ioctl.return_value = {"exit_code": 0, "success": True}
        client.execute.return_value = {"exit_code": 3, "success": False}
        server = create_server(None, client)
        await server.call_tool("ksword_ioctl", {"code": 0x80002000, "input_hex": "0102", "confirmed": True})
        client.ioctl.assert_called_once_with(code=0x80002000, input_hex="0102", output_bytes=4096, confirmed=True)
        await server.call_tool("ksword_cli_execute", {"arguments": ["example"], "confirmed": True})
        client.execute.assert_called_once_with(arguments=["example"], confirmed=True, json_output=True)

    async def test_native_tools_reject_coerced_or_unknown_parameters(self):
        client = Mock(spec=CliClient)
        server = create_server(None, client)
        for name, arguments in (
            ("ksword_ioctl", {"code": "1", "confirmed": True}),
            ("ksword_ioctl", {"code": 1, "output_bytes": True, "confirmed": True}),
            ("ksword_ioctl", {"code": 1, "confirmed": True, "device": "arbitrary"}),
            ("ksword_cli_execute", {"arguments": '["example"]', "confirmed": True}),
            ("ksword_cli_execute", {"arguments": ["example"], "confirmed": "true"}),
        ):
            with self.subTest(name=name), self.assertRaises(ToolError):
                await server.call_tool(name, arguments)
        client.ioctl.assert_not_called()
        client.execute.assert_not_called()

    async def test_cli_only_stdio_and_strict_confirmation(self):
        nonexistent = Path("missing-KSword/Release/KswordCLI.exe").absolute()
        parameters = StdioServerParameters(command=sys.executable,
            args=[str(Path(__file__).with_name("server.py")), "--ksword-cli-exe", str(nonexistent)])
        async with stdio_client(parameters) as (read, write):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                self.assertEqual(initialized.serverInfo.name, "KSword")
                tools = (await session.list_tools()).tools
                self.assertEqual(len(tools), 4)
                status = await session.call_tool("ksword_cli_status", {})
                self.assertFalse(status.isError)
                value = json.loads(status.content[0].text)
                self.assertFalse(value["exists"])
                self.assertFalse(value["gui_required"])
                for arguments in ({"code": 0}, {"code": 0, "confirmed": "true"},
                                  {"code": True, "confirmed": True}):
                    result = await session.call_tool("ksword_ioctl", arguments)
                    self.assertTrue(result.isError)
                    self.assertNotIn("cli_not_found", str(result.content))


if __name__ == "__main__":
    unittest.main()
