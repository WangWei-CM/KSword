"""核验三个包版本；发布时要求同名版本标签，不执行业务程序。"""
from __future__ import annotations
import ast
import json
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
node_version = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))["version"]
python_versions = re.findall(r'^version\s*=\s*"([^"]+)"\s*$',
                            (ROOT / "pyproject.toml").read_text(encoding="utf-8"), re.MULTILINE)
module = ast.parse((ROOT / "ksword_mcp/__init__.py").read_text(encoding="utf-8"))
module_versions = [ast.literal_eval(statement.value) for statement in module.body
                   if isinstance(statement, ast.Assign) and any(
                       isinstance(target, ast.Name) and target.id == "__version__" for target in statement.targets)]
if python_versions != [node_version] or module_versions != [node_version]:
    raise SystemExit("npm, Python project and module versions must match")
if not re.fullmatch(r"\d+\.\d+\.\d+", node_version):
    raise SystemExit("Use a three-part package version")
prompt = (ROOT / "agent_install_prompt.txt").read_text(encoding="utf-8").strip()
fences = (ROOT / "README.md").read_text(encoding="utf-8").split(chr(96) * 3, 2)
if len(fences) != 3 or fences[0].strip() or fences[1].split("\n", 1)[-1].strip() != prompt:
    raise SystemExit("README must start with the shared short agent prompt code block")
if os.environ.get("PUBLISH_REGISTRY", "none") != "none":
    if os.environ.get("GITHUB_REF") != "refs/tags/v" + node_version:
        raise SystemExit("Publishing requires the matching v<version> tag")
print("PACKAGE_VERSION=" + node_version)
