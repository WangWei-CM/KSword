"""真实 GUI 的复选框回归；只使用显式指定的安全对象，不启动程序或执行业务按钮。"""
from __future__ import annotations

import argparse
import time

from protocol import BridgeError
from winpipe import BridgeClient


def checkbox(client: BridgeClient, object_id: str) -> dict:
    """读取真实对象状态；非复选框、不可见或禁用目标不作为安全回归对象。"""
    response = client.call("snapshot", {"root_id": object_id, "max_nodes": 10, "depth": 1})
    nodes = response["data"]["nodes"]
    node = next(item for item in nodes if item["id"] == object_id)
    assert node["class"] == "QCheckBox" and node["visible"] and node["enabled"]
    return node


def settle(client: BridgeClient, response: dict) -> None:
    """等待原生派发回执；测试结果最终以控件回读为准，不以 accepted 冒充状态改变。"""
    assert response["status"] == "accepted", response
    operation = response["data"]["operation_id"]
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        result = client.call("operation_status", {"operation_id": operation})["data"]
        if result["state"] == "dispatched":
            return
        assert result["state"] not in ("failed", "cancelled", "interrupted"), result
        time.sleep(.02)
    raise AssertionError("checkbox dispatch did not finish")


def verify(client: BridgeClient, object_id: str) -> None:
    """验证布尔设置、重复设值、类型与确认边界；finally恢复原先状态。"""
    original = checkbox(client, object_id)["checked"]
    checks = 0
    try:
        for desired in (not original, not original, original):
            response = client.call("set_field", {"object_id": object_id, "value": desired,
                                                  "confirmed": True})
            settle(client, response)
            state = checkbox(client, object_id)
            assert state["checked"] is desired, "checkbox value was not applied or repeated setting toggled it"
            assert "set_field" in state["actions"], "boolean capability was not advertised"
            checks += 1
        for value, confirmed, code in (("true", True, "invalid_value"),
                                       (1, True, "invalid_value"), (not original, False, "confirmation_required")):
            try:
                client.call("set_field", {"object_id": object_id, "value": value, "confirmed": confirmed})
            except BridgeError as error:
                assert error.code == code, error
            else:
                raise AssertionError("invalid or unconfirmed checkbox editing was accepted")
            assert checkbox(client, object_id)["checked"] is original
            checks += 1
        print(f"LIVE_CHECKBOX=PASS checks={checks}", flush=True)
    finally:
        current = checkbox(client, object_id)["checked"]
        if current is not original:
            # 原版本也支持toggle，恢复时仅在回读状态不符后发送一次，不自动重试。
            settle(client, client.call("action", {"object_id": object_id, "action": "toggle", "confirmed": True}))
        assert checkbox(client, object_id)["checked"] is original, "checkbox original value was not restored"
        print("LIVE_CHECKBOX_ORIGINAL_RESTORED=True", flush=True)


def main() -> None:
    """必须由测试者提供已核对无系统副作用的真实复选框；不猜页面或批量勾选。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--object-id", required=True)
    args = parser.parse_args()
    verify(BridgeClient(args.pid), args.object_id)


if __name__ == "__main__":
    main()
