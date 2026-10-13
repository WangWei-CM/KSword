"""按明确路径连接 KSword；仅在用户选择 auto 后启动，绝不重试业务请求。"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import threading
import time
from typing import Any

from protocol import BridgeError, pipe_path
from winpipe import BridgeClient, PipeTimeouts


@dataclass(frozen=True)
class StartupOptions:
    """用户安装时保存的策略；executable 必须是实际 KSword5.1.exe 的完整路径。"""
    executable: Path
    policy: str = "fail"
    elevated: bool = False
    timeout: float = 30.0

    def __post_init__(self) -> None:
        # 只接受两种显式策略；管理员启动仅适用于自动启动。
        if self.policy not in ("fail", "auto") or not 0 < self.timeout <= 120:
            raise ValueError("Startup policy must be fail/auto and timeout must be in (0, 120]")
        if self.elevated and self.policy != "auto":
            raise ValueError("Elevated startup requires the auto policy")
        if not self.executable.is_absolute() or self.executable.name.casefold() != "ksword5.1.exe":
            raise ValueError("Use an absolute path to Ksword5.1.exe")


class ProcessEntry(ctypes.Structure):
    """Toolhelp 进程结构；ULONG_PTR 使用指针宽度，避免 x64 字段错位。"""
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]


class WindowsRuntime:
    """只查询进程身份和管道；启动使用固定参数，不使用 shell 拼接或任意进程执行。"""
    def __init__(self) -> None:
        if os.name != "nt":
            raise BridgeError("unsupported_platform", "KSword MCP requires local Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        self.shell = ctypes.WinDLL("shell32", use_last_error=True)
        bindings = {
            "CreateToolhelp32Snapshot": ([wintypes.DWORD, wintypes.DWORD], wintypes.HANDLE),
            "Process32FirstW": ([wintypes.HANDLE, ctypes.POINTER(ProcessEntry)], wintypes.BOOL),
            "Process32NextW": ([wintypes.HANDLE, ctypes.POINTER(ProcessEntry)], wintypes.BOOL),
            "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
            "GetCurrentProcess": ([], wintypes.HANDLE),
            "QueryFullProcessImageNameW": ([wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                            ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            "ProcessIdToSessionId": ([wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            "WaitNamedPipeW": ([wintypes.LPCWSTR, wintypes.DWORD], wintypes.BOOL),
        }
        for name, (arguments, result) in bindings.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result
        self.advapi.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                               ctypes.POINTER(wintypes.HANDLE)]
        self.advapi.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                   wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        self.advapi.EqualSid.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.advapi.GetSidSubAuthorityCount.argtypes = [ctypes.c_void_p]
        self.advapi.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
        self.advapi.GetSidSubAuthority.argtypes = [ctypes.c_void_p, wintypes.DWORD]
        self.advapi.GetSidSubAuthority.restype = ctypes.POINTER(wintypes.DWORD)

    def token(self, process: Any, kind: int) -> Any:
        """返回调用方拥有的令牌信息缓冲；不输出 SID 或更改任何权限。"""
        token = wintypes.HANDLE()
        if not self.advapi.OpenProcessToken(process, 8, ctypes.byref(token)):
            return None
        try:
            needed = wintypes.DWORD()
            self.advapi.GetTokenInformation(token, kind, None, 0, ctypes.byref(needed))
            if not 0 < needed.value <= 65536:
                return None
            buffer = ctypes.create_string_buffer(needed.value)
            if not self.advapi.GetTokenInformation(token, kind, buffer, needed.value, ctypes.byref(needed)):
                return None
            return buffer
        finally:
            self.kernel.CloseHandle(token)

    def level(self, process: Any) -> int | None:
        """只读完整性标签；查询失败不能当成普通权限继续。"""
        buffer = self.token(process, 25)
        if buffer is None:
            return None
        sid = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_void_p))[0]
        count = self.advapi.GetSidSubAuthorityCount(sid)[0]
        return self.advapi.GetSidSubAuthority(sid, count - 1)[0] if count else None

    def compatible(self, pid: int) -> bool:
        """连接前复核同用户、同会话、同完整性级别，与 GUI 的准入边界保持一致。"""
        process = self.kernel.OpenProcess(0x1000, False, pid)
        if not process:
            return False
        try:
            own = self.kernel.GetCurrentProcess()
            user, own_user = self.token(process, 1), self.token(own, 1)
            session, own_session = wintypes.DWORD(), wintypes.DWORD()
            if user is None or own_user is None:
                return False
            peer_level, own_level = self.level(process), self.level(own)
            return bool(self.kernel.ProcessIdToSessionId(pid, ctypes.byref(session))
                        and self.kernel.ProcessIdToSessionId(os.getpid(), ctypes.byref(own_session))
                        and session.value == own_session.value
                        and peer_level is not None and peer_level == own_level
                        and self.advapi.EqualSid(ctypes.cast(user, ctypes.POINTER(ctypes.c_void_p))[0],
                                                 ctypes.cast(own_user, ctypes.POINTER(ctypes.c_void_p))[0]))
        finally:
            self.kernel.CloseHandle(process)

    def discover(self, executable: Path) -> list[int]:
        """仅枚举进程并匹配系统返回的完整映像路径，不扫描磁盘或猜测程序安装位置。"""
        snapshot = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise BridgeError("process_query_failed", "Unable to inspect running processes")
        matches = []
        try:
            entry = ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            present = self.kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while present:
                if entry.szExeFile.casefold() == executable.name.casefold():
                    process = self.kernel.OpenProcess(0x1000, False, entry.th32ProcessID)
                    if process:
                        try:
                            path = ctypes.create_unicode_buffer(32768)
                            size = wintypes.DWORD(len(path))
                            if self.kernel.QueryFullProcessImageNameW(process, 0, path, ctypes.byref(size)):
                                if os.path.normcase(os.path.realpath(path.value)) == os.path.normcase(str(executable.resolve())):
                                    matches.append(int(entry.th32ProcessID))
                        finally:
                            self.kernel.CloseHandle(process)
                present = self.kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            self.kernel.CloseHandle(snapshot)
        return matches

    def ready(self, pid: int) -> bool:
        """只检查固定 PID 管道是否存在；真正连接仍由原传输核验服务端 PID。"""
        return bool(self.kernel.WaitNamedPipeW(pipe_path(pid), 1) or ctypes.get_last_error() in (121, 231))

    def launch(self, options: StartupOptions) -> None:
        """按用户选定的权限启动一次；不重启旧实例、不改设置，也不代答 UAC。"""
        level = self.level(self.kernel.GetCurrentProcess())
        if options.elevated and (level is None or level < 0x3000):
            raise BridgeError("admin_client_required", "Run the MCP client/agent as administrator before requesting an elevated KSword")
        if not options.elevated and level != 0x2000:
            raise BridgeError("normal_client_required", "Use a normal-permission MCP client for normal KSword startup")
        if not options.executable.is_file():
            raise BridgeError("ksword_not_found", "The configured Ksword5.1.exe does not exist")
        if not options.elevated:
            # 旧版会按保存设置自动请求管理员；缺少单次覆盖开关时拒绝启动，尊重用户选择。
            if options.executable.stat().st_size > 128 * 1024 * 1024 or \
                    "--mcp-no-auto-elevation".encode("utf-16-le") not in options.executable.read_bytes():
                raise BridgeError("unsupported_ksword_build", "Normal auto-start requires a current MCP-enabled build with --mcp-no-auto-elevation")
            subprocess.Popen([str(options.executable), "--mcp", "--mcp-no-auto-elevation"],
                             cwd=str(options.executable.parent), stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        # ShellExecuteEx 原样交给 Windows；已匹配管理员客户端，stdio 不接管权限提示。
        class ShellExecuteInfo(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG), ("hwnd", wintypes.HWND),
                        ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR),
                        ("lpParameters", wintypes.LPCWSTR), ("lpDirectory", wintypes.LPCWSTR),
                        ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
                        ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR),
                        ("hkeyClass", wintypes.HKEY), ("dwHotKey", wintypes.DWORD),
                        ("hIcon", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]
        info = ShellExecuteInfo()
        info.cbSize = ctypes.sizeof(info)
        info.fMask, info.lpVerb, info.lpFile = 0x40, "runas", str(options.executable)
        info.lpParameters, info.lpDirectory, info.nShow = "--mcp", str(options.executable.parent), 1
        self.shell.ShellExecuteExW.argtypes = [ctypes.POINTER(ShellExecuteInfo)]
        if not self.shell.ShellExecuteExW(ctypes.byref(info)):
            raise BridgeError("startup_failed", "Windows rejected or cancelled elevated KSword startup")
        if info.hProcess:
            self.kernel.CloseHandle(info.hProcess)


class ManagedBridgeClient:
    """每次工具调用解析当前 PID；启动锁只保护发现/启动，业务传输不自动重试。"""
    def __init__(self, options: StartupOptions, timeouts: PipeTimeouts | None = None,
                 runtime: Any = None, client_factory: Any = BridgeClient) -> None:
        self.options, self.timeouts = options, timeouts or PipeTimeouts()
        self.runtime = runtime or WindowsRuntime()
        self.client_factory = client_factory
        self.lock = threading.Lock()
        self.may_start_gui = options.policy == "auto"

    def existing(self) -> int | None:
        """有实例但桥未启用时明确失败，不能以 auto 为由另开或终止已有程序。"""
        processes = self.runtime.discover(self.options.executable)
        if not processes:
            return None
        compatible = [pid for pid in processes if self.runtime.compatible(pid)]
        if not compatible:
            raise BridgeError("privilege_mismatch", "KSword and the MCP client must have the same user, session and integrity level")
        ready = [pid for pid in compatible if self.runtime.ready(pid)]
        if len(ready) > 1:
            raise BridgeError("multiple_instances", "Several KSword bridges match this path; use --pid to choose explicitly")
        if not ready:
            raise BridgeError("mcp_not_enabled", "Enable MCP in the existing KSword settings; it will not be restarted")
        return ready[0]

    def resolve(self) -> int:
        """首次启动有限等待，随后每次重新发现；不缓存已退出进程的 PID。"""
        with self.lock:
            pid = self.existing()
            if pid is not None:
                return pid
            if self.options.policy == "fail":
                raise BridgeError("ksword_not_running", "KSword is not running and automatic startup is disabled")
            self.runtime.launch(self.options)
            deadline = time.monotonic() + self.options.timeout
            while time.monotonic() < deadline:
                try:
                    pid = self.existing()
                    if pid is not None:
                        return pid
                except BridgeError as error:
                    if error.code != "mcp_not_enabled":
                        raise
                time.sleep(.1)
            raise BridgeError("startup_timeout", "KSword did not expose its MCP bridge before the startup deadline")

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """解析目标后只发送一次；超时、断线或旧 ID 均交给调用方处理。"""
        pid = self.resolve()
        client = self.client_factory(pid, timeouts=self.timeouts)
        return client.call(method, params)
