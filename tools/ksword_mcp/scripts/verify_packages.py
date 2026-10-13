"""验证实际 wheel/sdist/npm tgz 的入口、版本、许可证和源码组成。"""
from __future__ import annotations
import argparse
from email.parser import Parser
import json
from pathlib import Path
import tarfile
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("--dist", type=Path, default=Path("dist"))
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
version = json.loads((root / "package.json").read_text(encoding="utf-8"))["version"]
expected_license = (root / "LICENSE").read_bytes()
required_modules = {"__init__.py", "__main__.py", "server.py", "cli.py", "protocol.py", "launcher.py", "winpipe.py"}

def check_clean(names):
    if any("__pycache__" in name or name.endswith((".pyc", ".pyo")) or "/.venv/" in name for name in names):
        raise SystemExit("Package contains generated runtime files")

def unique(pattern):
    matches = list(args.dist.rglob(pattern))
    if len(matches) != 1:
        raise SystemExit(f"Expected one {pattern}, found {len(matches)}")
    return matches[0]

with zipfile.ZipFile(unique("*.whl")) as archive:
    names = set(archive.namelist())
    check_clean(names)
    assert {"ksword_mcp/" + name for name in required_modules} <= names
    metadata_name = next(name for name in names if name.endswith(".dist-info/METADATA"))
    metadata = Parser().parsestr(archive.read(metadata_name).decode("utf-8"))
    assert metadata["Name"] == "ksword-mcp" and metadata["Version"] == version
    assert metadata["License-Expression"] == "LicenseRef-KSword-Community-Source-1.6"
    license_name = next(name for name in names if name.endswith(".dist-info/licenses/LICENSE"))
    assert archive.read(license_name) == expected_license
    entrypoint = next(name for name in names if name.endswith(".dist-info/entry_points.txt"))
    assert "ksword-mcp = ksword_mcp.server:main" in archive.read(entrypoint).decode("utf-8")

with tarfile.open(unique("*.tar.gz"), "r:gz") as archive:
    names = set(archive.getnames())
    check_clean(names)
    prefix = "ksword_mcp-" + version + "/"
    assert {prefix + "ksword_mcp/" + name for name in required_modules} <= names
    assert archive.extractfile(prefix + "LICENSE").read() == expected_license
    for filename in ("README.md", "INSTALL_FOR_AGENTS.md", "PUBLISHING.md", "pyproject.toml"):
        assert prefix + filename in names

with tarfile.open(unique("*.tgz"), "r:gz") as archive:
    names = set(archive.getnames())
    check_clean(names)
    assert {"package/ksword_mcp/" + name for name in required_modules} <= names
    assert "package/bin/ksword-mcp.cjs" in names
    assert "package/pyproject.toml" in names
    assert archive.extractfile("package/LICENSE").read() == expected_license
    metadata = json.loads(archive.extractfile("package/package.json").read())
    assert metadata["version"] == version
    assert metadata["bin"]["ksword-mcp"] == "bin/ksword-mcp.cjs"
print("PACKAGE_CONTENTS=PASS")
