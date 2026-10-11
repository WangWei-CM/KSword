"""本地 Qt 桥协议：创建有界请求，校验回执，供 server.py 和回归测试调用。"""

from __future__ import annotations

import json
import math
import uuid
from typing import Any

# 协议预算含末尾换行，和主程序中的传输上限一致。
MAX_REQUEST_BYTES = 64 * 1024
MAX_RESPONSE_BYTES = 2 * 1024 * 1024
MAX_PID = 0xFFFFFFFF


class BridgeError(Exception):
    """桥接失败；code 可供 MCP 调用方识别，message 只包含本地诊断。"""

    def __init__(self, code: str, message: str) -> None:
        # code 保存稳定错误分类，message 不回显 GUI 的任意敏感数据。
        self.code = code
        super().__init__(f"{code}: {message}")


def validate_pid(pid: int) -> int:
    """校验传入的进程号；成功返回原值，拒绝 bool、非整数及越界值。"""
    if type(pid) is not int or not 1 <= pid <= MAX_PID:
        raise BridgeError("invalid_pid", "PID must be an integer in 1..4294967295")
    return pid


def pipe_path(pid: int) -> str:
    """由唯一受控的 PID 生成本机管道路径，禁止调用方提供远程路径。"""
    return rf"\\.\pipe\ksword-mcp-{validate_pid(pid)}"


def _reject_constant(value: str) -> None:
    """JSON 解码钩子：拒绝 NaN/Infinity 等非标准 JSON 数值。"""
    raise ValueError(f"Non-JSON constant: {value}")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """JSON 对象钩子：拒绝重复键，以免两侧对同一帧产生不同解释。"""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON object key")
        result[key] = value
    return result


def encode_request(method: str, params: dict[str, Any]) -> tuple[str, bytes]:
    """输入方法和参数；返回请求 UUID 与包含换行的 UTF-8 JSON 帧。"""
    if not isinstance(method, str) or not method or len(method) > 64:
        raise BridgeError("invalid_method", "Expected a bounded method name")
    if type(params) is not dict:
        raise BridgeError("invalid_params", "Expected a JSON parameter object")
    request_id = str(uuid.uuid4())  # request_id 用于严格配对单次工具调用。
    request = {"id": request_id, "method": method, "params": params}
    try:
        frame = (json.dumps(request, ensure_ascii=False, allow_nan=False,
                            separators=(",", ":")) + "\n").encode("utf-8")
    except (ValueError, TypeError, UnicodeError, RecursionError) as exc:
        raise BridgeError("invalid_params", "Parameters must be valid JSON") from exc
    if len(frame) > MAX_REQUEST_BYTES:
        raise BridgeError("request_too_large", "Request exceeds 64 KiB")
    return request_id, frame


def decode_response(frame: bytes, request_id: str) -> dict[str, Any]:
    """输入完整响应帧及请求 ID；返回校验后的回执，错误回执抛 BridgeError。"""
    if len(frame) > MAX_RESPONSE_BYTES:
        raise BridgeError("response_too_large", "Response exceeds 2 MiB")
    if not frame.endswith(b"\n") or frame.count(b"\n") != 1:
        raise BridgeError("invalid_frame", "Expected one newline-delimited response")
    try:
        response = json.loads(frame[:-1].decode("utf-8"),
                              parse_constant=_reject_constant,
                              object_pairs_hook=_unique_object)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise BridgeError("invalid_response", "Response must contain valid UTF-8 JSON") from exc
    if type(response) is not dict:
        raise BridgeError("invalid_response", "Response must be an object")
    if type(response.get("schemaVersion")) is not int or response["schemaVersion"] != 1:
        raise BridgeError("unsupported_schema", "Expected bridge schemaVersion 1")
    if response.get("id") != request_id:
        raise BridgeError("response_id_mismatch", "Response does not match the request UUID")
    status = response.get("status")  # status 只描述桥接接收或调用状态。
    if status not in ("ok", "accepted", "error"):
        raise BridgeError("invalid_response", "Unknown bridge response status")
    if type(response.get("data")) is not dict:
        raise BridgeError("invalid_response", "Response data must be an object")
    if status == "error":
        error = response.get("error")  # error 只提取受限机器码，不回显未知文本。
        if type(error) is not dict or not isinstance(error.get("code"), str):
            raise BridgeError("invalid_response", "Error response needs a machine code")
        code = error["code"]
        if not code or len(code) > 96 or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in code):
            raise BridgeError("invalid_response", "Invalid bridge error machine code")
        raise BridgeError(code, "KSword rejected the request; inspect the GUI if needed")
    return response


def bounded_int(value: int, name: str, minimum: int, maximum: int) -> int:
    """校验工具整数参数；name 用于诊断，范围由主程序协议约定传入。"""
    if type(value) is not int or not minimum <= value <= maximum:
        raise BridgeError("invalid_params", f"{name} must be in {minimum}..{maximum}")
    return value


def bounded_id(value: str, name: str, *, optional: bool = False) -> str:
    """校验不透明对象或页面 ID；不解释其内容，保留主程序的唯一身份校验。"""
    if not isinstance(value, str) or len(value) > 256 or (not value and not optional):
        raise BridgeError("invalid_params", f"{name} must be a bounded ID")
    if any(ord(character) < 32 for character in value):
        raise BridgeError("invalid_params", f"{name} contains control characters")
    return value


def bounded_value(value: Any) -> Any:
    """限制字段赋值为 JSON 标量；复杂 Qt 控件应通过未来专用适配器操作。"""
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, str) and len(value) <= 16000:
        return value
    if type(value) is int and -(2 ** 53) < value < 2 ** 53:
        return value
    if type(value) is float and math.isfinite(value):
        return value
    raise BridgeError("invalid_params", "value must be a bounded JSON scalar")


def require_confirmation(confirmed: bool) -> None:
    """动作前必须显式 confirmed=true；原有业务弹窗仍由 GUI 自行处理。"""
    if confirmed is not True:
        raise BridgeError("confirmation_required", "Pass confirmed=true for a GUI mutation")


def parent_path(parent_rows: list[int] | None) -> list[int]:
    """校验树表父行路径；None 返回空路径，每级是非负模型行号。"""
    rows = [] if parent_rows is None else parent_rows
    if type(rows) is not list or len(rows) > 32:
        raise BridgeError("invalid_params", "parent_rows must contain at most 32 row indices")
    for row in rows:
        bounded_int(row, "parent row", 0, 2147483647)
    return rows.copy()
