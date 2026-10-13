"""有界 Windows 本地命名管道客户端；用 OVERLAPPED 避免阻塞 open/read。"""

from __future__ import annotations

import ctypes
import os
import threading
import time
from ctypes import wintypes
from dataclasses import dataclass
from typing import Any

from .protocol import (BridgeError, MAX_REQUEST_BYTES, MAX_RESPONSE_BYTES,
                      decode_response, encode_request, pipe_path, validate_pid)

# Win32 常量分别控制异步打开、错误分类、等待结果与访问权限。
ERROR_FILE_NOT_FOUND = 2
ERROR_PIPE_BUSY = 231
ERROR_IO_PENDING = 997
ERROR_MORE_DATA = 234
ERROR_BROKEN_PIPE = 109
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class Overlapped(ctypes.Structure):
    """Win32 OVERLAPPED 布局；事件和缓冲区必须存活到内核完成本次 I/O。"""

    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


@dataclass(frozen=True)
class PipeTimeouts:
    """每阶段秒数；连接、写入和完整回执各自共享一个有限截止时刻。"""

    connect: float = 2.0
    write: float = 2.0
    read: float = 10.0

    def __post_init__(self) -> None:
        # 每项限制最大 60 秒，避免配置错误把有限等待变成长时间停顿。
        for seconds in (self.connect, self.write, self.read):
            if not isinstance(seconds, (int, float)) or not 0 < seconds <= 60:
                raise ValueError("Each pipe timeout must be in (0, 60] seconds")


def native_api() -> Any:
    """绑定 kernel32 的精确参数类型；非 Windows 平台给出可识别工具错误。"""
    if os.name != "nt":
        raise BridgeError("unsupported_platform", "KSword MCP needs Windows named pipes")
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "CreateFileW": ([wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                         ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                         wintypes.HANDLE], wintypes.HANDLE),
        "WaitNamedPipeW": ([wintypes.LPCWSTR, wintypes.DWORD], wintypes.BOOL),
        "CreateEventW": ([ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL,
                          wintypes.LPCWSTR], wintypes.HANDLE),
        "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
        "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        "CancelIoEx": ([wintypes.HANDLE, ctypes.POINTER(Overlapped)], wintypes.BOOL),
        "GetOverlappedResult": ([wintypes.HANDLE, ctypes.POINTER(Overlapped),
                                  ctypes.POINTER(wintypes.DWORD), wintypes.BOOL], wintypes.BOOL),
        "GetNamedPipeServerProcessId": ([wintypes.HANDLE,
                                          ctypes.POINTER(wintypes.ULONG)], wintypes.BOOL),
    }
    # ReadFile/WriteFile 用 void* 接受 ctypes 的读写缓冲区，不转换成临时字符串。
    transfer_signature = ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                           ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(Overlapped)], wintypes.BOOL)
    signatures["ReadFile"] = transfer_signature
    signatures["WriteFile"] = transfer_signature
    for name, (arguments, result) in signatures.items():
        function = getattr(api, name)
        function.argtypes = arguments
        function.restype = result
    return api


def _failure(code: str, operation: str) -> BridgeError:
    """生成有限诊断；只记录 Win32 错误号，绝不打印请求和 GUI 内容。"""
    return BridgeError(code, f"{operation} failed with Win32 error {ctypes.get_last_error()}")


def _milliseconds(deadline: float) -> int:
    """把单阶段截止时刻转换成 WaitForSingleObject 的有限毫秒值。"""
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        return 0
    return max(1, int(remaining * 1000))


def _retain_cancelled(api: Any, event: int, overlapped: Overlapped, buffer: Any) -> None:
    """极端延迟取消时由守护线程持有内核引用，完成后回收事件；调用者仍有界返回。"""
    def reclaim() -> None:
        # captured overlapped/buffer 保持 ctypes 内存活着，直到事件标识内核完成。
        retained = (overlapped, buffer)
        while api.WaitForSingleObject(event, 250) != WAIT_OBJECT_0:
            # 极端的等待失败也不能释放仍可能被内核使用的内存；有限休眠避免忙循环。
            time.sleep(0.01)
        api.CloseHandle(event)
        del retained

    threading.Thread(target=reclaim, name="ksword-pipe-cancel-cleanup", daemon=True).start()


def transfer(api: Any, handle: int, buffer: Any, size: int,
             *, write: bool, deadline: float) -> int:
    """执行一段异步读写；返回字节数，超时取消并保证 OVERLAPPED 内存生命周期。"""
    event = api.CreateEventW(None, True, False, None)
    if not event:
        raise _failure("pipe_io_failed", "CreateEventW")
    overlapped = Overlapped()  # overlapped 持有本次异步请求的内核状态。
    overlapped.hEvent = event
    transferred = wintypes.DWORD()  # transferred 回读实际完成的字节数。
    pending = False
    close_event = True
    operation = "WriteFile" if write else "ReadFile"
    try:
        if _milliseconds(deadline) == 0:
            raise BridgeError("pipe_timeout", f"{operation} deadline expired")
        succeeded = getattr(api, operation)(handle, buffer, size,
                                             ctypes.byref(transferred), ctypes.byref(overlapped))
        if not succeeded:
            error = ctypes.get_last_error()
            if error == ERROR_IO_PENDING:
                pending = True
            elif error != ERROR_MORE_DATA or write:
                raise _failure("pipe_io_failed", operation)
        # 一段 I/O 的多次分片共用调用方传入的截止时刻，不刷新超时预算。
        if pending:
            wait_result = api.WaitForSingleObject(event, _milliseconds(deadline))
            if wait_result == WAIT_TIMEOUT:
                raise BridgeError("pipe_timeout", f"{operation} timed out; request was not retried")
            if wait_result != WAIT_OBJECT_0:
                raise _failure("pipe_io_failed", "WaitForSingleObject")
            pending = False
            succeeded = api.GetOverlappedResult(handle, ctypes.byref(overlapped),
                                                 ctypes.byref(transferred), False)
            if not succeeded and (ctypes.get_last_error() != ERROR_MORE_DATA or write):
                raise _failure("pipe_io_failed", "GetOverlappedResult")
        return transferred.value
    finally:
        if pending:
            # CancelIoEx 只取消此请求；不会重发动作，取消完成前不得释放 buffer/overlapped。
            api.CancelIoEx(handle, ctypes.byref(overlapped))
            if api.WaitForSingleObject(event, 250) != WAIT_OBJECT_0:
                _retain_cancelled(api, event, overlapped, buffer)
                close_event = False
        if close_event:
            api.CloseHandle(event)


class WindowsPipeTransport:
    """每个请求独立建立本地连接；exchange 输入一帧、输出一帧，没有业务重试。"""

    def __init__(self, pid: int, timeouts: PipeTimeouts | None = None) -> None:
        # pid 只允许正整数；endpoint 永远来自固定本机模板。
        self.pid = validate_pid(pid)
        self.endpoint = pipe_path(pid)
        self.timeouts = timeouts or PipeTimeouts()

    def _connect(self, api: Any) -> int:
        """有限重试仅用于尚未发送任何请求时的连接；并核验服务端进程身份。"""
        deadline = time.monotonic() + self.timeouts.connect
        while _milliseconds(deadline):
            # SECURITY_IDENTIFICATION 限制服务端只能识别客户端身份，不能借用其权限。
            handle = api.CreateFileW(self.endpoint, 0xC0000000, 0, None, 3,
                                     FILE_FLAG_OVERLAPPED | 0x00110000, None)
            if handle != INVALID_HANDLE_VALUE:
                server_pid = wintypes.ULONG()
                if not api.GetNamedPipeServerProcessId(handle, ctypes.byref(server_pid)):
                    api.CloseHandle(handle)
                    raise _failure("server_identity_failed", "GetNamedPipeServerProcessId")
                if server_pid.value != self.pid:
                    api.CloseHandle(handle)
                    raise BridgeError("server_pid_mismatch", "Named pipe belongs to a different process")
                return handle
            error = ctypes.get_last_error()
            if error not in (ERROR_PIPE_BUSY, ERROR_FILE_NOT_FOUND):
                raise _failure("pipe_connection_failed", "CreateFileW")
            if error == ERROR_PIPE_BUSY:
                api.WaitNamedPipeW(self.endpoint, min(25, _milliseconds(deadline)))
            else:
                time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))
        raise BridgeError("pipe_connection_timeout", "Start the chosen KSword process with --mcp")

    def exchange(self, frame: bytes) -> bytes:
        """发送合法请求一次，并在有界预算内读取完整 JSON 换行回执。"""
        if not isinstance(frame, bytes) or not frame.endswith(b"\n"):
            raise BridgeError("invalid_frame", "Expected a complete request frame")
        if len(frame) > MAX_REQUEST_BYTES:
            raise BridgeError("request_too_large", "Request exceeds 64 KiB")
        api = native_api()
        handle = self._connect(api)
        try:
            deadline = time.monotonic() + self.timeouts.write
            sent = 0  # sent 保存分片写入的偏移，已经写入的内容不会再次发送。
            while sent < len(frame):
                buffer = ctypes.create_string_buffer(frame[sent:])
                count = transfer(api, handle, buffer, len(frame) - sent,
                                 write=True, deadline=deadline)
                if not 0 < count <= len(frame) - sent:
                    raise BridgeError("pipe_io_failed", "WriteFile returned an invalid byte count")
                sent += count
            deadline = time.monotonic() + self.timeouts.read
            response = bytearray()  # response 只保留上限以内的累积帧，不做无界读取。
            while True:
                budget = min(65536, MAX_RESPONSE_BYTES + 1 - len(response))
                buffer = ctypes.create_string_buffer(budget)
                count = transfer(api, handle, buffer, budget, write=False, deadline=deadline)
                if not 0 < count <= budget:
                    raise BridgeError("incomplete_response", "Pipe closed before a complete response")
                response.extend(buffer.raw[:count])
                if len(response) > MAX_RESPONSE_BYTES:
                    raise BridgeError("response_too_large", "Response exceeds 2 MiB")
                if b"\n" in response:
                    if not response.endswith(b"\n") or response.count(b"\n") != 1:
                        raise BridgeError("invalid_frame", "Expected exactly one response frame")
                    return bytes(response)
        finally:
            # 关闭每次调用独立的管道句柄，服务器收到断开后回收对应连接。
            api.CloseHandle(handle)


class BridgeClient:
    """连接传输和帧协议；测试可注入只有 exchange(frame) 方法的假传输。"""

    def __init__(self, pid: int, transport: Any = None,
                 timeouts: PipeTimeouts | None = None) -> None:
        self.pid = validate_pid(pid)
        self.transport = transport or WindowsPipeTransport(pid, timeouts)

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """构建单次 UUID 请求；一次 exchange 后严格校验响应，绝不重试。"""
        request_id, frame = encode_request(method, params)
        response = self.transport.exchange(frame)
        return decode_response(response, request_id)
