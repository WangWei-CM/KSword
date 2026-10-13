"""自动启动策略回归；只使用进程/启动模拟对象，不启动 KSword、UAC 或驱动。"""
from __future__ import annotations

import asyncio
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from launcher import ManagedBridgeClient, StartupOptions, WindowsRuntime
from protocol import BridgeError
from server import create_server, parse_arguments


class FakeRuntime:
    """固定进程目录与权限结果，记录启动次数以核对无隐式重启。"""
    def __init__(self, pids=(), ready=True, compatible=True):
        self.pids = list(pids)
        self.available = ready
        self.match = compatible
        self.launches = 0

    def discover(self, executable):
        return list(self.pids)

    def compatible(self, pid):
        return self.match

    def ready(self, pid):
        return self.available

    def launch(self, options):
        self.launches += 1
        self.pids = [123]
        self.available = True


class StartupTests(unittest.TestCase):
    """目标选择、显式策略和无业务重试边界，不接触 GUI 或系统配置。"""
    def setUp(self):
        self.path = Path("C:/KSword/Release/Ksword5.1.exe")

    def options(self, policy="fail", elevated=False, timeout=30):
        return StartupOptions(self.path, policy, elevated, timeout)

    def client(self, runtime, policy="fail", factory=None):
        return ManagedBridgeClient(self.options(policy), runtime=runtime,
                                   client_factory=factory or Mock(return_value=Mock(call=Mock(return_value={}))))

    def test_fail_is_default_and_never_launches(self):
        runtime = FakeRuntime()
        with self.assertRaisesRegex(BridgeError, "ksword_not_running"):
            self.client(runtime).call("hello", {})
        self.assertEqual(runtime.launches, 0)

    def test_auto_does_not_launch_at_construction(self):
        runtime = FakeRuntime()
        self.client(runtime, "auto")
        self.assertEqual(runtime.launches, 0)

    def test_auto_launches_once_on_first_tool_call(self):
        runtime = FakeRuntime()
        factory = Mock(return_value=Mock(call=Mock(return_value={"status": "ok"})))
        client = self.client(runtime, "auto", factory)
        client.call("hello", {})
        client.call("pages", {})
        self.assertEqual(runtime.launches, 1)
        self.assertEqual(factory.call_args.args[0], 123)

    def test_reuses_matching_live_process(self):
        runtime = FakeRuntime([45])
        self.assertEqual(self.client(runtime, "auto").resolve(), 45)
        self.assertEqual(runtime.launches, 0)

    def test_existing_disabled_bridge_is_not_restarted(self):
        runtime = FakeRuntime([45], ready=False)
        with self.assertRaisesRegex(BridgeError, "mcp_not_enabled"):
            self.client(runtime, "auto").resolve()
        self.assertEqual(runtime.launches, 0)

    def test_privilege_mismatch_does_not_launch(self):
        runtime = FakeRuntime([45], compatible=False)
        with self.assertRaisesRegex(BridgeError, "privilege_mismatch"):
            self.client(runtime, "auto").resolve()
        self.assertEqual(runtime.launches, 0)

    def test_multiple_ready_instances_require_explicit_pid(self):
        with self.assertRaisesRegex(BridgeError, "multiple_instances"):
            self.client(FakeRuntime([45, 46])).resolve()

    def test_new_pid_is_discovered_on_each_call(self):
        runtime = FakeRuntime([45])
        client = self.client(runtime)
        self.assertEqual(client.resolve(), 45)
        runtime.pids = [46]
        self.assertEqual(client.resolve(), 46)

    def test_business_request_is_sent_once_on_failure(self):
        transport = Mock(call=Mock(side_effect=BridgeError("pipe_read_timeout", "failed")))
        with self.assertRaisesRegex(BridgeError, "pipe_read_timeout"):
            self.client(FakeRuntime([45]), "auto", Mock(return_value=transport)).call("action", {})
        self.assertEqual(transport.call.call_count, 1)

    def test_auto_timeout_is_bounded(self):
        runtime = FakeRuntime()
        runtime.launch = Mock()
        client = ManagedBridgeClient(self.options("auto", timeout=.01), runtime=runtime)
        with self.assertRaisesRegex(BridgeError, "startup_timeout"):
            client.resolve()
        self.assertEqual(runtime.launch.call_count, 1)

    def test_options_require_explicit_executable_and_valid_policy(self):
        for args in ((Path("server.py"),), (self.path, "bad"), (self.path, "fail", True),
                     (self.path, "auto", False, 0), (self.path, "auto", False, 121)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                StartupOptions(*args)

    def test_cli_keeps_explicit_pid_mode(self):
        with patch("sys.argv", ["server.py", "--pid", "123"]):
            args = parse_arguments()
            self.assertEqual(args.pid, 123)
            self.assertIsNone(args.ksword_exe)

    def test_cli_accepts_selected_auto_admin_policy(self):
        with patch("sys.argv", ["server.py", "--ksword-exe", str(self.path),
                                "--startup-policy", "auto", "--startup-elevated"]):
            args = parse_arguments()
            self.assertTrue(args.startup_elevated)
            self.assertEqual(args.startup_policy, "auto")

    def test_auto_tool_annotations_disclose_startup_side_effect(self):
        server = create_server(self.client(FakeRuntime(), "auto"))
        tools = asyncio.run(server.list_tools())
        self.assertFalse(next(t for t in tools if t.name == "ksword_hello").annotations.readOnlyHint)

    def test_fail_tool_annotations_remain_read_only(self):
        server = create_server(self.client(FakeRuntime([45])))
        tools = asyncio.run(server.list_tools())
        self.assertTrue(next(t for t in tools if t.name == "ksword_hello").annotations.readOnlyHint)

    def test_normal_launch_requires_compatible_client_and_build(self):
        runtime = WindowsRuntime.__new__(WindowsRuntime)
        runtime.kernel = Mock()
        runtime.level = Mock(return_value=0x3000)
        with self.assertRaisesRegex(BridgeError, "normal_client_required"):
            runtime.launch(self.options("auto"))
        runtime.level.return_value = 0x2000
        with patch.object(Path, "is_file", return_value=True), \
             patch.object(Path, "stat", return_value=SimpleNamespace(st_size=20)), \
             patch.object(Path, "read_bytes", return_value=b"old build"), \
             patch("launcher.subprocess.Popen") as popen:
            with self.assertRaisesRegex(BridgeError, "unsupported_ksword_build"):
                runtime.launch(self.options("auto"))
            popen.assert_not_called()

    def test_normal_launch_uses_safe_argv_and_does_not_change_settings(self):
        runtime = WindowsRuntime.__new__(WindowsRuntime)
        runtime.kernel = Mock()
        runtime.level = Mock(return_value=0x2000)
        with patch.object(Path, "is_file", return_value=True), \
             patch.object(Path, "stat", return_value=SimpleNamespace(st_size=20)), \
             patch.object(Path, "read_bytes", return_value="--mcp-no-auto-elevation".encode("utf-16-le")), \
             patch("launcher.subprocess.Popen") as popen:
            runtime.launch(self.options("auto"))
            self.assertEqual(popen.call_args.args[0], [str(self.path), "--mcp", "--mcp-no-auto-elevation"])
            self.assertNotIn("shell", popen.call_args.kwargs)

    def test_admin_launch_is_refused_from_normal_client(self):
        runtime = WindowsRuntime.__new__(WindowsRuntime)
        runtime.kernel = Mock()
        runtime.level = Mock(return_value=0x2000)
        with self.assertRaisesRegex(BridgeError, "admin_client_required"):
            runtime.launch(self.options("auto", elevated=True))

    def test_admin_launch_uses_windows_runas_without_broker(self):
        runtime = WindowsRuntime.__new__(WindowsRuntime)
        runtime.kernel = Mock()
        runtime.shell = Mock()
        runtime.level = Mock(return_value=0x3000)
        runtime.shell.ShellExecuteExW.return_value = True
        with patch.object(Path, "is_file", return_value=True):
            runtime.launch(self.options("auto", elevated=True))
        info = runtime.shell.ShellExecuteExW.call_args.args[0]._obj
        self.assertEqual(info.lpVerb, "runas")
        self.assertEqual(info.lpFile, str(self.path))
        self.assertEqual(info.lpParameters, "--mcp")


if __name__ == "__main__":
    unittest.main()
