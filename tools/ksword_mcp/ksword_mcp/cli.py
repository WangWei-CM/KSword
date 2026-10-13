"""只通过明确的 KSwordCLI 路径调用原生命令，不启动 GUI、不自动提权或重试。"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from typing import Any, Callable

from .protocol import BridgeError, bounded_int, require_confirmation

MAX_IOCTL_BYTES = 1024 * 1024
MAX_CLI_OUTPUT = 8 * 1024 * 1024


def run_bounded(argv: list[str], timeout: float) -> subprocess.CompletedProcess[bytes]:
    """捕获有界 stdout/stderr；超时只回收本次 CLI，结果未知时绝不重发请求。"""
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, shell=False,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    overflow = threading.Event()  # 任一输出越界会终止自己创建的子进程。
    buffers = [bytearray(), bytearray()]

    def collect(stream, destination: bytearray) -> None:
        """排空单个管道，只保留有限字节，不把大结果全部装入内存。"""
        try:
            while chunk := stream.read(65536):
                available = MAX_CLI_OUTPUT - len(destination)
                destination.extend(chunk[:available])
                if len(chunk) > available:
                    overflow.set()
                    try:
                        process.kill()
                    except OSError:
                        pass  # 子进程可能已退出，越界回执仍然拒绝。
                    break
        finally:
            stream.close()

    readers = [threading.Thread(target=collect, args=(stream, buffers[index]), daemon=True)
               for index, stream in enumerate((process.stdout, process.stderr))]
    for reader in readers:
        reader.start()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise BridgeError("cli_timeout", "CLI timed out; the operation outcome is unknown. Do not retry automatically.") from None
    finally:
        for reader in readers:
            reader.join(timeout=2)
    if overflow.is_set():
        raise BridgeError("cli_output_limit", "CLI output exceeded 8 MiB; the operation outcome may be incomplete.")
    if any(reader.is_alive() for reader in readers):
        raise BridgeError("cli_output_incomplete", "CLI output pipes did not close; no automatic retry is allowed.")
    return subprocess.CompletedProcess(argv, process.returncode, bytes(buffers[0]), bytes(buffers[1]))


def decode_cli_text(value: bytes) -> str:
    """优先保留 CLI UTF-8；兼容带 BOM 的 UTF-16 原生输出，不猜测业务状态。"""
    if value.startswith((b"\xff\xfe", b"\xfe\xff")):
        return value.decode("utf-16", errors="replace")
    return value.decode("utf-8-sig", errors="replace")


class CliClient:
    """固定程序路径和时间预算；初始化只验证配置，工具发现不执行任何原生命令。"""

    def __init__(self, executable: Path, timeout: float = 30.0,
                 runner: Callable = run_bounded):
        self.executable = Path(executable)
        if not self.executable.is_absolute() or self.executable.name.lower() != "kswordcli.exe":
            raise ValueError("--ksword-cli-exe requires an absolute path to KswordCLI.exe")
        if type(timeout) not in (float, int) or not 0 < timeout <= 120:
            raise ValueError("--cli-timeout must be greater than zero and at most 120 seconds")
        self.timeout = float(timeout)
        self.runner = runner

    def status(self) -> dict[str, Any]:
        """只报告实际配置和文件状态，不启动进程或假设驱动已经加载。"""
        return {"executable": str(self.executable), "exists": self.executable.is_file(),
                "platform_supported": os.name == "nt", "timeout_seconds": self.timeout,
                "gui_required": False, "auto_elevation": False}

    def _execute(self, arguments: list[str]) -> dict[str, Any]:
        """以 argv 数组调用固定原生程序，返回退出码、文本和可用 JSON。"""
        if not self.executable.is_file():
            raise BridgeError("cli_not_found", "Configured KswordCLI.exe does not exist")
        if not arguments or len(arguments) > 128 or any(
                type(value) is not str or "\0" in value or len(value) > 32767 for value in arguments):
            raise BridgeError("invalid_arguments", "CLI arguments must be bounded strings without NUL")
        argv = [str(self.executable), *arguments]
        if os.name == "nt" and len(subprocess.list2cmdline(argv)) > 30000:
            raise BridgeError("cli_arguments_limit", "CLI command line exceeds the supported Windows bound")
        try:
            completed = self.runner(argv, self.timeout)
        except OSError as exc:
            raise BridgeError("cli_start_failed", "Could not execute configured KswordCLI.exe") from exc
        stdout, stderr = decode_cli_text(completed.stdout), decode_cli_text(completed.stderr)
        structured = None
        try:
            structured = json.loads(stdout)
        except ValueError:
            pass  # 旧 R0 命令仍可输出文本，不把文本回执伪装为结构化成功。
        return {"exit_code": completed.returncode, "stdout": stdout, "stderr": stderr,
                "json": structured, "success": completed.returncode == 0}

    def help(self, command: list[str]) -> dict[str, Any]:
        """固定 help 入口只读取内置帮助；命令路径只接受普通词，不传入选项。"""
        if len(command) > 12 or any(type(word) is not str or not word or word.startswith("-") or len(word) > 80
                or not all(character.isalnum() or character in "-_" for character in word) for word in command):
            raise BridgeError("invalid_command", "Help path must contain bounded command words")
        return self._execute(["help", *command])

    def execute(self, arguments: list[str], confirmed: bool, json_output: bool = True) -> dict[str, Any]:
        """任意命令可能修改系统，必须明确确认；原生确认参数仍由调用方按帮助传入。"""
        require_confirmation(confirmed)
        argv = list(arguments)
        if json_output and "--json" not in argv:
            argv.append("--json")
        return self._execute(argv)

    def ioctl(self, code: int, input_hex: str, output_bytes: int, confirmed: bool) -> dict[str, Any]:
        """通过 CLI 的 ArkDriverClient 原始入口发一次请求，临时输入文件不进入命令行。"""
        require_confirmation(confirmed)
        bounded_int(code, "code", 0, 0xFFFFFFFF)
        bounded_int(output_bytes, "output_bytes", 0, MAX_IOCTL_BYTES)
        if type(input_hex) is not str or len(input_hex) > MAX_IOCTL_BYTES * 2:
            raise BridgeError("invalid_input", "Raw IOCTL input must be at most 1 MiB of hexadecimal bytes")
        try:
            payload = bytes.fromhex(input_hex)
        except ValueError:
            raise BridgeError("invalid_input", "Raw IOCTL input is not valid hexadecimal") from None
        if len(payload) > MAX_IOCTL_BYTES:
            raise BridgeError("invalid_input", "Raw IOCTL input exceeds 1 MiB")
        arguments = ["system", "ioctl", "call", "--code", hex(code),
                     "--output-bytes", str(output_bytes), "--confirm", "--json"]
        if not payload:
            return self._execute(arguments)
        descriptor, path = tempfile.mkstemp(prefix="ksword-mcp-ioctl-", suffix=".bin")
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(payload)
            return self._execute([*arguments, "--input-file", path])
        finally:
            Path(path).unlink(missing_ok=True)
