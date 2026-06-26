#!/usr/bin/env python3
"""
Create a lightweight inventory for a Windows PDB cache used by KSword R0 audit prep.

The script is intentionally read-only for the PDB cache. It scans only the module
folder level and each module's immediate instance folders, then samples selected
modules with llvm-pdbutil when available. Outputs are written as JSON/CSV into the
configured inventory directory.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

DEFAULT_PDB_ROOT = Path(r"E:\KswordPDB\PDB\pdb-cache\amd64")
DEFAULT_OUTPUT_DIR = Path(r"D:\Temp\ksword_pdb_audit_prep\pdb_inventory")
DEFAULT_SAMPLE_MODULES = [
    "ntkrnlmp.pdb",
    "win32kbase.pdb",
    "win32kfull.pdb",
    "tcpip.pdb",
    "fltMgr.pdb",
    "fvevol.pdb",
    "ndis.pdb",
]
TOOL_NAMES = [
    "llvm-pdbutil.exe",
    "llvm-pdbutil",
    "dia2dump.exe",
    "dia2dump",
    "cvdump.exe",
    "cvdump",
    "dumpbin.exe",
    "dumpbin",
    "link.exe",
    "link",
    "vswhere.exe",
    "vswhere",
    "python.exe",
    "python",
    "py.exe",
    "py",
    "rg.exe",
    "rg",
]
COMMON_TOOL_PATHS = [
    Path(r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe"),
    Path(r"D:\Software\VS\VC\Tools\Llvm\bin\llvm-pdbutil.exe"),
    Path(r"D:\Software\VS\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe"),
    Path(r"D:\Software\VS\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe"),
    Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"),
]
SUMMARY_PATTERNS = {
    "guid": re.compile(r"^\s*GUID:\s*(.+?)\s*$"),
    "age": re.compile(r"^\s*Age:\s*(\d+)\s*$"),
    "has_types": re.compile(r"^\s*Has Types:\s*(\w+)\s*$"),
    "has_ids": re.compile(r"^\s*Has IDs:\s*(\w+)\s*$"),
    "has_globals": re.compile(r"^\s*Has Globals:\s*(\w+)\s*$"),
    "has_publics": re.compile(r"^\s*Has Publics:\s*(\w+)\s*$"),
    "is_stripped": re.compile(r"^\s*Is stripped:\s*(\w+)\s*$"),
}


@dataclass
class ModuleInventoryRow:
    module: str
    instance_count: int
    typical_instance: str
    typical_pdb_path: str
    min_instance_name: str
    max_instance_name: str


@dataclass
class ToolInventoryRow:
    tool: str
    available: bool
    path: str
    source: str


@dataclass
class SampleValidationRow:
    module: str
    sample_pdb_path: str
    summary_ok: bool
    types_ok: bool
    publics_ok: bool
    globals_filtered_ok: bool | None
    guid: str
    age: str
    has_types: str
    has_ids: str
    has_globals: str
    has_publics: str
    is_stripped: str
    notes: str


def utc_now_text() -> str:
    """Return an ISO-8601 UTC timestamp for output metadata."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def direct_child_dirs(path: Path) -> list[Path]:
    """Return immediate child directories only; this deliberately avoids recursion."""
    try:
        return sorted([entry for entry in path.iterdir() if entry.is_dir()], key=lambda p: p.name.lower())
    except OSError:
        return []


def expected_pdb_path(module_dir: Path, instance_dir: Path) -> Path | None:
    """Find the expected PDB file in one instance folder without recursive scanning."""
    candidate = instance_dir / module_dir.name
    if candidate.is_file():
        return candidate
    try:
        for child in instance_dir.iterdir():
            if child.is_file() and child.suffix.lower() == ".pdb":
                return child
    except OSError:
        return None
    return None


def build_module_inventory(pdb_root: Path, max_modules: int | None) -> list[ModuleInventoryRow]:
    """Build one inventory row per module directory using only immediate children."""
    rows: list[ModuleInventoryRow] = []
    module_dirs = direct_child_dirs(pdb_root)
    if max_modules is not None:
        module_dirs = module_dirs[:max_modules]
    for module_dir in module_dirs:
        instances = direct_child_dirs(module_dir)
        sample_path = ""
        for instance_dir in instances[:16]:
            candidate = expected_pdb_path(module_dir, instance_dir)
            if candidate:
                sample_path = str(candidate)
                break
        rows.append(ModuleInventoryRow(
            module=module_dir.name,
            instance_count=len(instances),
            typical_instance=instances[0].name if instances else "",
            typical_pdb_path=sample_path,
            min_instance_name=instances[0].name if instances else "",
            max_instance_name=instances[-1].name if instances else "",
        ))
    return rows


def discover_tools() -> list[ToolInventoryRow]:
    """Locate PDB-related tools through PATH plus a few bounded common VS paths."""
    rows: list[ToolInventoryRow] = []
    seen: set[str] = set()
    for name in TOOL_NAMES:
        if name in seen:
            continue
        seen.add(name)
        found = shutil.which(name)
        rows.append(ToolInventoryRow(
            tool=name,
            available=bool(found),
            path=found or "",
            source="PATH" if found else "not-found",
        ))
    for candidate in COMMON_TOOL_PATHS:
        key = str(candidate).lower()
        if key in seen:
            continue
        seen.add(key)
        rows.append(ToolInventoryRow(
            tool=candidate.name,
            available=candidate.is_file(),
            path=str(candidate) if candidate.is_file() else "",
            source="bounded-common-path",
        ))
    return rows


def choose_llvm_pdbutil(tools: Iterable[ToolInventoryRow], explicit_path: str | None) -> str:
    """Return the llvm-pdbutil path used for sample validation, or an empty string."""
    if explicit_path:
        return explicit_path if Path(explicit_path).is_file() else ""
    for row in tools:
        if row.available and row.tool.lower().startswith("llvm-pdbutil"):
            return row.path
    return ""


def run_tool(command: list[str], timeout_seconds: int, capture_text: bool) -> tuple[bool, str, str]:
    """Run a bounded tool command and return success, captured output, and notes."""
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE if capture_text else subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return False, "", f"timeout after {timeout_seconds}s"
    except OSError as exc:
        return False, "", f"launch failed: {exc}"
    stderr = (completed.stderr or "").strip()
    note = stderr[:500]
    return completed.returncode == 0, completed.stdout or "", note


def parse_summary(summary_text: str) -> dict[str, str]:
    """Extract GUID/Age and stream capability flags from llvm-pdbutil summary text."""
    parsed = {key: "" for key in SUMMARY_PATTERNS}
    for line in summary_text.splitlines():
        for key, pattern in SUMMARY_PATTERNS.items():
            match = pattern.match(line)
            if match:
                parsed[key] = match.group(1)
    return parsed


def validate_samples(pdb_root: Path, modules: list[str], llvm_pdbutil: str, timeout_seconds: int, validate_globals: bool) -> list[SampleValidationRow]:
    """Run bounded sample validation for selected modules only."""
    rows: list[SampleValidationRow] = []
    for module in modules:
        module_dir = pdb_root / module
        instances = direct_child_dirs(module_dir)
        sample_path = ""
        for instance_dir in instances[:32]:
            candidate = expected_pdb_path(module_dir, instance_dir)
            if candidate:
                sample_path = str(candidate)
                break
        if not sample_path or not llvm_pdbutil:
            rows.append(SampleValidationRow(
                module=module,
                sample_pdb_path=sample_path,
                summary_ok=False,
                types_ok=False,
                publics_ok=False,
                globals_filtered_ok=None,
                guid="",
                age="",
                has_types="",
                has_ids="",
                has_globals="",
                has_publics="",
                is_stripped="",
                notes="missing sample pdb" if not sample_path else "llvm-pdbutil unavailable",
            ))
            continue

        summary_ok, summary_text, summary_note = run_tool([llvm_pdbutil, "dump", "-summary", sample_path], timeout_seconds, True)
        summary = parse_summary(summary_text)
        types_ok, _, types_note = run_tool([llvm_pdbutil, "dump", "-types", "-type-index=0x1000", sample_path], timeout_seconds, False)
        publics_ok, _, publics_note = run_tool([llvm_pdbutil, "dump", "-publics", sample_path], timeout_seconds, False)
        globals_ok: bool | None = None
        globals_note = "globals validation skipped by default because llvm-pdbutil may crash on large stripped kernel PDBs"
        if validate_globals:
            globals_ok, _, globals_note = run_tool([llvm_pdbutil, "dump", "-globals", "-global-name=PsInitialSystemProcess", sample_path], timeout_seconds, False)

        notes = "; ".join(note for note in [summary_note, types_note, publics_note, globals_note] if note)
        rows.append(SampleValidationRow(
            module=module,
            sample_pdb_path=sample_path,
            summary_ok=summary_ok,
            types_ok=types_ok,
            publics_ok=publics_ok,
            globals_filtered_ok=globals_ok,
            guid=summary.get("guid", ""),
            age=summary.get("age", ""),
            has_types=summary.get("has_types", ""),
            has_ids=summary.get("has_ids", ""),
            has_globals=summary.get("has_globals", ""),
            has_publics=summary.get("has_publics", ""),
            is_stripped=summary.get("is_stripped", ""),
            notes=notes,
        ))
    return rows


def write_json(path: Path, payload: object) -> None:
    """Write UTF-8 JSON with stable indentation."""
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def write_csv(path: Path, rows: list[object]) -> None:
    """Write dataclass rows to CSV. Empty input creates an empty file."""
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        if not rows:
            return
        fieldnames = list(asdict(rows[0]).keys())
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Inventory KSword kernel PDB cache without modifying PDB files.")
    parser.add_argument("--pdb-root", default=str(DEFAULT_PDB_ROOT), help="PDB cache root directory")
    parser.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR), help="directory for JSON/CSV output")
    parser.add_argument("--llvm-pdbutil", default="", help="explicit llvm-pdbutil.exe path")
    parser.add_argument("--sample-modules", nargs="*", default=DEFAULT_SAMPLE_MODULES, help="module PDB names to sample")
    parser.add_argument("--max-modules", type=int, default=None, help="optional module limit for fast smoke tests")
    parser.add_argument("--timeout-seconds", type=int, default=20, help="per llvm-pdbutil command timeout")
    parser.add_argument("--validate-globals", action="store_true", help="also try llvm-pdbutil -globals; may crash on some PDBs")
    args = parser.parse_args(argv)

    pdb_root = Path(args.pdb_root)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    module_rows = build_module_inventory(pdb_root, args.max_modules)
    tool_rows = discover_tools()
    llvm = choose_llvm_pdbutil(tool_rows, args.llvm_pdbutil or None)
    sample_rows = validate_samples(pdb_root, args.sample_modules, llvm, args.timeout_seconds, args.validate_globals)

    metadata = {
        "generated_utc": utc_now_text(),
        "pdb_root": str(pdb_root),
        "output_dir": str(output_dir),
        "module_count": len(module_rows),
        "sample_modules": args.sample_modules,
        "llvm_pdbutil_used": llvm,
        "validate_globals": args.validate_globals,
        "scan_policy": "module directories + immediate instance directories only; no recursive full PDB extraction",
    }

    write_json(output_dir / "pdb_inventory_summary.json", {
        "metadata": metadata,
        "modules": [asdict(row) for row in module_rows],
        "tools": [asdict(row) for row in tool_rows],
        "sample_validation": [asdict(row) for row in sample_rows],
    })
    write_json(output_dir / "module_inventory.json", [asdict(row) for row in module_rows])
    write_json(output_dir / "tool_inventory.json", [asdict(row) for row in tool_rows])
    write_json(output_dir / "sample_validation.json", [asdict(row) for row in sample_rows])
    write_csv(output_dir / "module_inventory.csv", module_rows)
    write_csv(output_dir / "tool_inventory.csv", tool_rows)
    write_csv(output_dir / "sample_validation.csv", sample_rows)

    print(f"modules={len(module_rows)}")
    print(f"output_dir={output_dir}")
    print(f"llvm_pdbutil={llvm or 'unavailable'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
