"""构建并验证正式进程目录、受控 R3 目标和完整文件解码回归。

只使用既有 output 目录和 HostX64 MSVC。R3 检查只读取本脚本启动的
子进程所声明的固定 64 KiB 区域；退出后不留下事件、进程或字节转储。
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import queue
import re
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]  # 当前脚本所属仓库，不搜索其它 checkout。
OUTPUT = ROOT / "output"  # 既有产物目录；不创建新的临时构建目录。
PROJECT = ROOT / "tools/tests/native_memory_regressions.vcxproj"  # 三个测试入口的正式工程。


def run(command: list[str], environment: dict[str, str], timeout: int = 180) -> str:
    """输入参数数组、环境和等待上限；记录输出，非零退出立即报告失败。"""
    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=timeout)
    text = result.stdout + result.stderr  # 保留编译和运行的完整诊断。
    with (OUTPUT / "native_memory_regressions.log").open("a", encoding="utf-8") as stream:
        stream.write(text + f"\nEXIT_CODE={result.returncode}\n")
    print(text, end="" if text.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise RuntimeError(f"Command failed with exit code {result.returncode}: {command[0]}")
    return text


def check_owned_target(environment: dict[str, str]) -> None:
    """启动自有目标，核对真实区域与字节模式，再通过该目标的停止事件退出。"""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)  # 明确 Win32 签名，避免 x64 句柄截断。
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    kernel.OpenEventW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.OpenEventW.restype = wintypes.HANDLE
    kernel.SetEvent.argtypes = (wintypes.HANDLE,)
    kernel.SetEvent.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL

    target = subprocess.Popen([str(OUTPUT / "issue216_r3_target.exe")], cwd=ROOT, env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              encoding="utf-8", creationflags=subprocess.CREATE_NO_WINDOW)
    lines: queue.Queue[str | None] = queue.Queue()  # 后台只搬运 stdout，主线程保留有限等待。

    def read_lines() -> None:
        """把目标的完整输出逐行送入队列，关闭时发送 EOF。"""
        try:
            assert target.stdout is not None
            for line in target.stdout:
                lines.put(line.rstrip("\r\n"))
        finally:
            lines.put(None)

    reader = threading.Thread(target=read_lines, daemon=True)  # 不允许缺少 READY 无限阻塞。
    reader.start()
    process_handle = None  # 仅拥有当前测试子进程的查询/读取句柄。
    stop_handle = None  # 仅拥有按该 PID 命名的停止事件。
    transcript: list[str] = []  # 测试日志保留目标回执，不写出所读字节。
    try:
        fields: dict[str, str] = {}  # 目标 READY 前发布的只读区域信息。
        for _ in range(16):
            line = lines.get(timeout=10)
            if line is None:
                raise RuntimeError("Owned target exited before READY.")
            transcript.append(line)
            key, _, value = line.partition("=")
            fields[key] = value
            if key == "ISSUE216_TARGET_READY":
                break
        if fields.get("ISSUE216_TARGET_READY") != "1":
            raise RuntimeError("Owned target did not publish READY.")
        if int(fields["ISSUE216_TARGET_PID"]) != target.pid:
            raise RuntimeError("Reported PID is not the child started by this test.")
        expected_event = f"Local\\KSwordIssue216R3TargetStop-{target.pid}"  # 不接受任意事件名称。
        if fields["ISSUE216_TARGET_STOP_EVENT"] != expected_event:
            raise RuntimeError("Owned target stop event identity mismatched.")
        stop_handle = kernel.OpenEventW(0x0002, False, expected_event)
        if not stop_handle:
            raise ctypes.WinError(ctypes.get_last_error())

        size = int(fields["ISSUE216_TARGET_REGION_BYTES"])  # 不从输出接受任意大范围读取。
        address = int(fields["ISSUE216_TARGET_BASE"], 16)  # 仅使用已核验子进程发布的自有地址。
        if size != 64 * 1024 or address == 0:
            raise RuntimeError("Owned target did not declare its fixed 64 KiB region.")
        process_handle = kernel.OpenProcess(0x1000 | 0x0010, False, target.pid)
        if not process_handle:
            raise ctypes.WinError(ctypes.get_last_error())
        buffer = (ctypes.c_ubyte * size)()  # 固定上限的本测试读取缓冲区。
        returned = ctypes.c_size_t()  # 必须确认实际读取长度，不能只看 BOOL。
        if not kernel.ReadProcessMemory(process_handle, address, buffer, size, ctypes.byref(returned)):
            raise ctypes.WinError(ctypes.get_last_error())
        if returned.value != size or bytes(buffer) != bytes((offset * 37 + 11) & 0xFF for offset in range(size)):
            raise RuntimeError("Owned target memory did not match the complete deterministic pattern.")
        if not kernel.SetEvent(stop_handle):
            raise ctypes.WinError(ctypes.get_last_error())
        if target.wait(timeout=10) != 0:
            raise RuntimeError("Owned target cleanup did not return zero.")
        reader.join(timeout=2)
        while not lines.empty():
            line = lines.get_nowait()
            if line is not None:
                transcript.append(line)
        if "ISSUE216_TARGET_EXIT=STOPPED" not in transcript:
            raise RuntimeError("Owned target did not acknowledge normal event-driven cleanup.")
        print("OWNED_R3_TARGET_PASS bytes=65536 normal_stop=1", flush=True)
    finally:
        # 失败也先要求自然退出；仅当自有子进程无法退出时终止该唯一对象。
        if target.poll() is None:
            if not stop_handle:
                stop_handle = kernel.OpenEventW(0x0002, False,
                    f"Local\\KSwordIssue216R3TargetStop-{target.pid}")
            if stop_handle:
                kernel.SetEvent(stop_handle)
            try:
                target.wait(timeout=10)
            except subprocess.TimeoutExpired:
                target.kill()
                target.wait(timeout=10)
        for handle in (process_handle, stop_handle):
            if handle:
                kernel.CloseHandle(handle)
        reader.join(timeout=2)
        if target.stdout is not None:
            target.stdout.close()
        with (OUTPUT / "native_memory_regressions.log").open("a", encoding="utf-8") as stream:
            stream.write("\n".join(transcript) + "\n")


def main() -> None:
    """解析已知工具落点，顺序构建三个入口；全部实际检查通过才返回成功。"""
    if os.name != "nt" or not OUTPUT.is_dir():
        raise RuntimeError("Windows and the existing repository output directory are required.")
    msbuild = next((path for path in (
        Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe"),
        Path("D:/Software/VS/MSBuild/Current/Bin/amd64/MSBuild.exe")) if path.is_file()), None)
    qt = next((path for path in (ROOT / ".deps/Qt/6.9.3/msvc2022_64",
        Path("D:/Software/Qt/6.9.3/msvc2022_64")) if (path / "lib/Qt6Core.lib").is_file()), None)
    if msbuild is None or qt is None:
        raise RuntimeError("The configured x64 MSBuild and MSVC Qt SDK are required.")
    environment = dict(os.environ)  # 子进程独立环境，不改调用终端的工具链。
    environment["PATH"] = str(qt / "bin") + os.pathsep + environment.get("PATH", "")
    (OUTPUT / "native_memory_regressions.log").write_text("HOST_TOOL_ARCHITECTURE=x64\n", encoding="utf-8")
    for fixture, executable, marker in (
        ("Catalog", "memory_debug_process_catalog_tests.exe", "CATALOG_CHECKS"),
        ("Threads", "memory_debug_thread_session_tests.exe", "THREAD_SESSION_CHECKS"),
        ("Codec", "code_editor_file_decode_tests.exe", "FILE_DECODE_CHECKS"),
        ("Target", "issue216_r3_target.exe", "")):
        run([str(msbuild), str(PROJECT), "/t:Build", "/p:Configuration=Release", "/p:Platform=x64",
             "/p:PreferredToolArchitecture=x64", "/p:PROCESSOR_ARCHITECTURE=AMD64",
             "/p:PROCESSOR_ARCHITEW6432=AMD64", f"/p:Fixture={fixture}",
             f"/p:KSWORD_QT_DIR={qt}", "/m:1", "/v:minimal", "/nologo"], environment)
        if fixture == "Target":
            check_owned_target(environment)
        else:
            result = run([str(OUTPUT / executable)], environment, timeout=90)
            if re.search(rf"{marker}=[1-9]\d* FAILURES=0\b", result) is None:
                raise RuntimeError(f"Missing regression summary for {fixture}.")
    print("NATIVE_MEMORY_REGRESSIONS=PASS", flush=True)


if __name__ == "__main__":
    main()
