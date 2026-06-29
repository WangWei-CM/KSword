#!/usr/bin/env python3
"""
Bounded PDB extractor probe for KSword R0 audit preparation.

This tool is intentionally read-only. It does not try to dump every stream from
every cached PDB. Instead, it samples selected modules, verifies exact PDB
identity, extracts a few type records by index, probes configured globals by
name in isolated subprocesses, and captures only a bounded prefix of publics.
The output is evidence for what a future stable v4 extractor can safely support.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_PDB_ROOT = Path(r"E:\KswordPDB\PDB\pdb-cache\amd64")
DEFAULT_OUTPUT_DIR = Path(r"D:\Temp\ksword_pdb_audit_prep\pdb_inventory")
DEFAULT_LLVM_PDBUTIL = Path(r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe")
DEFAULT_MODULES = [
    "ntkrnlmp.pdb",
    "win32kbase.pdb",
    "win32kfull.pdb",
    "tcpip.pdb",
    "fltMgr.pdb",
    "fvevol.pdb",
    "ndis.pdb",
]
DEFAULT_TYPE_INDEXES = ["0x1000", "0x1001", "0x1002", "0x1003"]
CANDIDATE_GLOBALS = {
    "ntkrnlmp.pdb": [
        "PsInitialSystemProcess",
        "PsLoadedModuleList",
        "PspCidTable",
        "KeServiceDescriptorTable",
        "KeServiceDescriptorTableShadow",
    ],
    "tcpip.pdb": [
        "TcpPartitionTable",
        "TcpCompartmentSet",
        "UdpEndpointTable",
    ],
    "ndis.pdb": [
        "ndisGlobalOpenList",
        "ndisMiniportList",
        "ndisProtocolList",
    ],
    "fltMgr.pdb": [
        "FltGlobals",
    ],
    "fvevol.pdb": [
        "FveGlobals",
    ],
}
SUMMARY_PATTERNS = {
    "guid": re.compile(r"^\s*GUID:\s*(.+?)\s*$"),
    "age": re.compile(r"^\s*Age:\s*(\d+)\s*$"),
    "has_types": re.compile(r"^\s*Has Types:\s*(\w+)\s*$"),
    "has_ids": re.compile(r"^\s*Has IDs:\s*(\w+)\s*$"),
    "has_globals": re.compile(r"^\s*Has Globals:\s*(\w+)\s*$"),
    "has_publics": re.compile(r"^\s*Has Publics:\s*(\w+)\s*$"),
    "is_stripped": re.compile(r"^\s*Is stripped:\s*(\w+)\s*$"),
}
TYPE_RECORD_RE = re.compile(r"^\s*(0x[0-9A-Fa-f]+)\s+\|\s+([A-Z0-9_]+)\s+\[size\s*=\s*(\d+)\]")
SYMBOL_RECORD_RE = re.compile(r"\b(S_[A-Z0-9_]+)\b")
NAME_RE = re.compile(r"name\s*=\s*`?([^`,]+)`?")


@dataclass
class CommandResult:
    ok: bool
    exit_code: int
    timed_out: bool
    duration_ms: int
    stdout: str
    stderr: str


@dataclass
class TypeProbeRow:
    module: str
    pdb_path: str
    type_index: str
    ok: bool
    record_kind: str
    record_size: str
    detail: str


@dataclass
class GlobalProbeRow:
    module: str
    pdb_path: str
    global_name: str
    ok: bool
    crashed_or_failed: bool
    record_kind: str
    matched_line: str
    note: str


@dataclass
class PublicProbeRow:
    module: str
    pdb_path: str
    ok: bool
    captured_lines: int
    truncated: bool
    sample_symbol_count: int
    first_symbols: list[str]
    note: str


@dataclass
class ModuleProbeRow:
    module: str
    pdb_path: str
    summary_ok: bool
    guid: str
    age: str
    has_types: str
    has_ids: str
    has_globals: str
    has_publics: str
    is_stripped: str
    type_probe_ok: int
    global_probe_ok: int
    global_probe_unresolved: int
    global_primary_failed: int
    publics_ok: bool
    publics_sample_symbols: int


def utc_now_text() -> str:
    """Return an ISO-8601 UTC timestamp for reproducible metadata."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def find_sample_pdb(pdb_root: Path, module: str) -> Path | None:
    """Find one typical PDB path by checking immediate instance directories only."""
    module_dir = pdb_root / module
    try:
        instances = sorted([entry for entry in module_dir.iterdir() if entry.is_dir()], key=lambda p: p.name.lower())
    except OSError:
        return None
    for instance in instances[:64]:
        candidate = instance / module
        if candidate.is_file():
            return candidate
        try:
            for child in instance.iterdir():
                if child.is_file() and child.suffix.lower() == ".pdb":
                    return child
        except OSError:
            continue
    return None


def run_capture(command: list[str], timeout_seconds: int) -> CommandResult:
    """Run one bounded subprocess and capture stdout/stderr for parsing."""
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_seconds,
            check=False,
            errors="replace",
        )
        duration_ms = int((time.monotonic() - started) * 1000)
        return CommandResult(
            ok=completed.returncode == 0,
            exit_code=completed.returncode,
            timed_out=False,
            duration_ms=duration_ms,
            stdout=completed.stdout or "",
            stderr=completed.stderr or "",
        )
    except subprocess.TimeoutExpired as exc:
        duration_ms = int((time.monotonic() - started) * 1000)
        return CommandResult(
            ok=False,
            exit_code=-1,
            timed_out=True,
            duration_ms=duration_ms,
            stdout=(exc.stdout or "") if isinstance(exc.stdout, str) else "",
            stderr=(exc.stderr or "") if isinstance(exc.stderr, str) else "",
        )
    except OSError as exc:
        duration_ms = int((time.monotonic() - started) * 1000)
        return CommandResult(
            ok=False,
            exit_code=-2,
            timed_out=False,
            duration_ms=duration_ms,
            stdout="",
            stderr=str(exc),
        )


def run_stream_prefix(command: list[str], timeout_seconds: int, max_lines: int) -> CommandResult:
    """Capture only the first max_lines from a potentially huge command."""
    started = time.monotonic()
    lines: list[str] = []
    timed_out = False
    truncated = False
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
        )
    except OSError as exc:
        return CommandResult(False, -2, False, 0, "", str(exc))

    assert process.stdout is not None
    deadline = started + timeout_seconds
    while len(lines) < max_lines:
        if time.monotonic() > deadline:
            timed_out = True
            break
        line = process.stdout.readline()
        if line == "":
            break
        lines.append(line)
    if len(lines) >= max_lines and process.poll() is None:
        truncated = True
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
    stderr = ""
    if process.stderr is not None:
        try:
            stderr = process.stderr.read()
        except OSError:
            stderr = ""
    duration_ms = int((time.monotonic() - started) * 1000)
    output = "".join(lines)
    if truncated:
        output += "\n<ksword_truncated_publics_prefix>\n"
    return CommandResult(
        ok=(process.returncode == 0 or truncated) and not timed_out,
        exit_code=process.returncode if process.returncode is not None else -3,
        timed_out=timed_out,
        duration_ms=duration_ms,
        stdout=output,
        stderr=stderr,
    )


def parse_summary(text: str) -> dict[str, str]:
    """Parse llvm-pdbutil summary identity and stream availability fields."""
    parsed = {key: "" for key in SUMMARY_PATTERNS}
    for line in text.splitlines():
        for key, pattern in SUMMARY_PATTERNS.items():
            match = pattern.match(line)
            if match:
                parsed[key] = match.group(1)
    return parsed


def probe_types(llvm: Path, module: str, pdb_path: Path, indexes: list[str], timeout_seconds: int) -> list[TypeProbeRow]:
    """Probe a bounded list of TPI records by explicit type index."""
    rows: list[TypeProbeRow] = []
    for type_index in indexes:
        result = run_capture([str(llvm), "dump", "-types", f"-type-index={type_index}", str(pdb_path)], timeout_seconds)
        record_kind = ""
        record_size = ""
        detail = (result.stderr or "").strip()[:300]
        for line in result.stdout.splitlines():
            match = TYPE_RECORD_RE.match(line)
            if match:
                record_kind = match.group(2)
                record_size = match.group(3)
                detail = line.strip()
                break
        rows.append(TypeProbeRow(
            module=module,
            pdb_path=str(pdb_path),
            type_index=type_index,
            ok=result.ok and bool(record_kind),
            record_kind=record_kind,
            record_size=record_size,
            detail=detail,
        ))
    return rows


def probe_globals(llvm: Path, module: str, pdb_path: Path, names: list[str], timeout_seconds: int) -> list[GlobalProbeRow]:
    """Probe configured globals by name in isolated llvm-pdbutil subprocesses."""
    rows: list[GlobalProbeRow] = []
    for name in names:
        result = run_capture([str(llvm), "dump", "-globals", f"-global-name={name}", str(pdb_path)], timeout_seconds)
        matched_line = ""
        record_kind = ""
        for line in result.stdout.splitlines():
            if name in line or "S_" in line:
                matched_line = line.strip()
                symbol_match = SYMBOL_RECORD_RE.search(line)
                record_kind = symbol_match.group(1) if symbol_match else ""
                break
        note_parts = []
        if result.timed_out:
            note_parts.append("timeout")
        if result.exit_code != 0:
            note_parts.append(f"exit={result.exit_code}")
        if result.stderr.strip():
            note_parts.append(result.stderr.strip()[:300])
        rows.append(GlobalProbeRow(
            module=module,
            pdb_path=str(pdb_path),
            global_name=name,
            ok=result.ok and bool(matched_line),
            crashed_or_failed=not result.ok,
            record_kind=record_kind,
            matched_line=matched_line,
            note="; ".join(note_parts),
        ))
    return rows


def search_publics_for_names(
    llvm: Path,
    pdb_path: Path,
    names: list[str],
    timeout_seconds: int,
    max_lines: int,
) -> dict[str, str]:
    """Search the publics stream for candidate names and stop when bounded."""
    if not names:
        return {}
    remaining = set(names)
    found: dict[str, str] = {}
    started = time.monotonic()
    try:
        process = subprocess.Popen(
            [str(llvm), "dump", "-publics", str(pdb_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            errors="replace",
        )
    except OSError:
        return found
    assert process.stdout is not None
    line_count = 0
    deadline = started + timeout_seconds
    for line in process.stdout:
        line_count += 1
        for name in list(remaining):
            if name in line:
                found[name] = line.strip()
                remaining.remove(name)
        if not remaining or line_count >= max_lines or time.monotonic() > deadline:
            break
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
    return found


def apply_public_fallback(global_rows: list[GlobalProbeRow], public_hits: dict[str, str]) -> None:
    """Promote failed global probes when the same name is visible in publics."""
    for row in global_rows:
        if row.ok or row.global_name not in public_hits:
            continue
        row.ok = True
        row.record_kind = "S_PUB32"
        row.matched_line = public_hits[row.global_name]
        suffix = "public fallback hit after globals failure"
        row.note = f"{row.note}; {suffix}" if row.note else suffix


def probe_publics(llvm: Path, module: str, pdb_path: Path, timeout_seconds: int, max_lines: int, max_symbols: int) -> PublicProbeRow:
    """Capture a bounded prefix of publics and extract a few symbol-looking rows."""
    result = run_stream_prefix([str(llvm), "dump", "-publics", str(pdb_path)], timeout_seconds, max_lines)
    lines = result.stdout.splitlines()
    symbols: list[str] = []
    for line in lines:
        if "S_PUB" not in line and "S_" not in line:
            continue
        name_match = NAME_RE.search(line)
        text = name_match.group(1).strip() if name_match else line.strip()
        if text and text not in symbols:
            symbols.append(text[:180])
        if len(symbols) >= max_symbols:
            break
    return PublicProbeRow(
        module=module,
        pdb_path=str(pdb_path),
        ok=result.ok,
        captured_lines=len(lines),
        truncated="<ksword_truncated_publics_prefix>" in result.stdout,
        sample_symbol_count=len(symbols),
        first_symbols=symbols,
        note=(result.stderr or "").strip()[:300],
    )


def write_json(path: Path, payload: object) -> None:
    """Write UTF-8 JSON with stable indentation."""
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def write_csv(path: Path, rows: list[object]) -> None:
    """Write dataclass rows to CSV."""
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        if not rows:
            return
        fieldnames = list(asdict(rows[0]).keys())
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Bounded PDB extractor capability probe for KSword R0 audit prep.")
    parser.add_argument("--pdb-root", default=str(DEFAULT_PDB_ROOT), help="PDB cache root")
    parser.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR), help="output JSON/CSV directory")
    parser.add_argument("--llvm-pdbutil", default=str(DEFAULT_LLVM_PDBUTIL), help="llvm-pdbutil.exe path")
    parser.add_argument("--modules", nargs="*", default=DEFAULT_MODULES, help="module PDB names to probe")
    parser.add_argument("--type-indexes", nargs="*", default=DEFAULT_TYPE_INDEXES, help="bounded type indexes to probe")
    parser.add_argument("--timeout-seconds", type=int, default=12, help="per subprocess timeout")
    parser.add_argument("--public-line-limit", type=int, default=800, help="maximum public output lines per module")
    parser.add_argument("--public-global-search-line-limit", type=int, default=200000, help="maximum public lines scanned for global fallback")
    parser.add_argument("--public-symbol-limit", type=int, default=32, help="maximum sampled public symbols per module")
    args = parser.parse_args(argv)

    pdb_root = Path(args.pdb_root)
    output_dir = Path(args.output_dir)
    llvm = Path(args.llvm_pdbutil)
    output_dir.mkdir(parents=True, exist_ok=True)
    if not llvm.is_file():
        print(f"llvm-pdbutil unavailable: {llvm}", file=sys.stderr)
        return 2

    module_rows: list[ModuleProbeRow] = []
    type_rows: list[TypeProbeRow] = []
    global_rows: list[GlobalProbeRow] = []
    public_rows: list[PublicProbeRow] = []

    for module in args.modules:
        pdb_path = find_sample_pdb(pdb_root, module)
        if pdb_path is None:
            module_rows.append(ModuleProbeRow(module, "", False, "", "", "", "", "", "", "", 0, 0, 0, 0, False, 0))
            continue
        summary_result = run_capture([str(llvm), "dump", "-summary", str(pdb_path)], args.timeout_seconds)
        summary = parse_summary(summary_result.stdout)
        module_type_rows = probe_types(llvm, module, pdb_path, args.type_indexes, args.timeout_seconds)
        module_global_rows = probe_globals(llvm, module, pdb_path, CANDIDATE_GLOBALS.get(module, []), args.timeout_seconds)
        failed_global_names = [row.global_name for row in module_global_rows if not row.ok]
        if failed_global_names:
            public_hits = search_publics_for_names(
                llvm,
                pdb_path,
                failed_global_names,
                args.timeout_seconds,
                args.public_global_search_line_limit,
            )
            apply_public_fallback(module_global_rows, public_hits)
        public_row = probe_publics(llvm, module, pdb_path, args.timeout_seconds, args.public_line_limit, args.public_symbol_limit)
        type_rows.extend(module_type_rows)
        global_rows.extend(module_global_rows)
        public_rows.append(public_row)
        module_rows.append(ModuleProbeRow(
            module=module,
            pdb_path=str(pdb_path),
            summary_ok=summary_result.ok,
            guid=summary.get("guid", ""),
            age=summary.get("age", ""),
            has_types=summary.get("has_types", ""),
            has_ids=summary.get("has_ids", ""),
            has_globals=summary.get("has_globals", ""),
            has_publics=summary.get("has_publics", ""),
            is_stripped=summary.get("is_stripped", ""),
            type_probe_ok=sum(1 for row in module_type_rows if row.ok),
            global_probe_ok=sum(1 for row in module_global_rows if row.ok),
            global_probe_unresolved=sum(1 for row in module_global_rows if not row.ok),
            global_primary_failed=sum(1 for row in module_global_rows if row.crashed_or_failed),
            publics_ok=public_row.ok,
            publics_sample_symbols=public_row.sample_symbol_count,
        ))

    metadata = {
        "generated_utc": utc_now_text(),
        "pdb_root": str(pdb_root),
        "llvm_pdbutil": str(llvm),
        "modules": args.modules,
        "type_indexes": args.type_indexes,
        "public_line_limit": args.public_line_limit,
        "public_global_search_line_limit": args.public_global_search_line_limit,
        "policy": "read-only bounded extractor probe; no full recursive PDB dump",
    }
    payload = {
        "metadata": metadata,
        "modules": [asdict(row) for row in module_rows],
        "type_probes": [asdict(row) for row in type_rows],
        "global_probes": [asdict(row) for row in global_rows],
        "public_probes": [asdict(row) for row in public_rows],
    }
    write_json(output_dir / "pdb_extractor_probe.json", payload)
    write_csv(output_dir / "pdb_extractor_probe_modules.csv", module_rows)
    write_csv(output_dir / "pdb_extractor_probe_types.csv", type_rows)
    write_csv(output_dir / "pdb_extractor_probe_globals.csv", global_rows)
    write_csv(output_dir / "pdb_extractor_probe_publics.csv", public_rows)
    print(f"modules={len(module_rows)}")
    print(f"type_rows={len(type_rows)}")
    print(f"global_rows={len(global_rows)}")
    print(f"output={output_dir / 'pdb_extractor_probe.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
