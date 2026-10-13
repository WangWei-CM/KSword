"""兼容原有脚本入口；实现统一位于 ksword_mcp 包。"""
import sys
from ksword_mcp import winpipe as _implementation
sys.modules[__name__] = _implementation
