"""从已安装入口握手并读取目录/状态；不执行产品命令、GUI 或驱动请求。"""
from __future__ import annotations
import argparse
import asyncio
import json
import os
from pathlib import Path
import tempfile
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

parser = argparse.ArgumentParser()
parser.add_argument("--command", required=True)
parser.add_argument("--prefix", nargs="*", default=["-m", "ksword_mcp"])
parser.add_argument("--cli", type=Path, help="Optional real CLI path; verify native help only")
parser.add_argument("--npm-package", type=Path, help="Verify the npx entry using a local npm tgz")
args = parser.parse_args()

async def verify():
    cli_path = args.cli or Path(tempfile.gettempdir()) / "ksword-mcp-missing-runtime" / "KswordCLI.exe"
    prefix = ["--yes", "--package", str(args.npm_package.resolve()), "ksword-mcp"] if args.npm_package else args.prefix
    parameters = StdioServerParameters(command=args.command,
        args=[*prefix, "--ksword-cli-exe", str(cli_path)], cwd=tempfile.gettempdir(),
        env={name: os.environ[name] for name in ("KSWORD_MCP_CACHE_DIR", "KSWORD_MCP_PYTHON") if name in os.environ})
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer) as session:
            initialized = await session.initialize()
            assert initialized.serverInfo.name == "KSword"
            tools = (await session.list_tools()).tools
            assert {tool.name for tool in tools} == {
                "ksword_cli_status", "ksword_cli_help", "ksword_cli_execute", "ksword_ioctl"}
            result = await session.call_tool("ksword_cli_status", {})
            assert not result.isError
            value = json.loads(result.content[0].text)
            assert not value["gui_required"] and not value["auto_elevation"]
            if args.cli:
                help_result = await session.call_tool("ksword_cli_help", {"command": ["system", "ioctl", "call"]})
                assert not help_result.isError
                help_value = json.loads(help_result.content[0].text)
                assert help_value["exit_code"] == 0 and "--confirm" in help_value["stdout"]
                print("NATIVE_IOCTL_HELP=PASS (no device call)")
            rejected = await session.call_tool("ksword_ioctl", {"code": 0})
            assert rejected.isError and "confirmation_required" in rejected.content[0].text
    print("INSTALLED_STDIO=PASS (4 CLI tools, status and confirmation boundary)")

asyncio.run(verify())
