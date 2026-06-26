#!/usr/bin/env python3
r"""
Export selected ETHREAD/KTHREAD member offsets from a local Windows kernel PDB cache.

Inputs:
- E:\KswordPDB\PDB\profiles\ark_dyndata profile JSON files, used as the stable
  version-to-PDB-key index.
- E:\KswordPDB\PDB\pdb-cache\amd64\ntkrnlmp.pdb cached PDB files, used as the
  authoritative type information source.
- llvm-pdbutil.exe, used to print semantic class layouts from each PDB.

Processing:
- Read all kernel profiles and map each module version to the cached PDB whose
  symbol key appears in that profile's module identity.
- For each unique PDB, run llvm-pdbutil pretty --classes --class-definitions=all
  and resolve the requested direct member offsets from the printed layouts.
- Retry E: drive checks, file reads, subprocess launches, and output writes
  after a fixed delay so transient USB/SATA/NVMe disconnects do not abort the
  whole export.

Return behavior:
- Writes one JSON object to D:\ethread_kthread_offsets.json by default.
- The top-level JSON shape is: {version: {itemName: hexOffsetOrNull}}.
- Exits non-zero only when required roots/tools are permanently missing or a
  JSON write never succeeds.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from collections import OrderedDict
from pathlib import Path
from typing import Any, Callable, TypeVar


PDBUTIL_TIMEOUT_SECONDS = 600
DEFAULT_RETRY_SECONDS = 5
DEFAULT_PROFILE_ROOT = Path(r"E:\KswordPDB\PDB\profiles\ark_dyndata")
DEFAULT_PDB_CACHE_ROOT = Path(r"E:\KswordPDB\PDB\pdb-cache\amd64")
DEFAULT_LLVM_PDBUTIL = Path(r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe")
DEFAULT_OUTPUT = Path(r"D:\ethread_kthread_offsets.json")

TARGET_FIELDS: "OrderedDict[str, tuple[str, str]]" = OrderedDict(
    [
        ("_ETHREAD->CreateTime", ("_ETHREAD", "CreateTime")),
        ("_ETHREAD->ExitStatus", ("_ETHREAD", "ExitStatus")),
        ("_ETHREAD->ThreadName", ("_ETHREAD", "ThreadName")),
        ("_KTHREAD->Process", ("_KTHREAD", "Process")),
        ("_KTHREAD->State", ("_KTHREAD", "State")),
        ("_ETHREAD->ContextSwitches", ("_ETHREAD", "ContextSwitches")),
    ]
)

STRUCT_HEADER_RE = re.compile(r"^\s*(struct|class|union)\s+([^\s\[]+)\s+\[sizeof\s*=\s*(\d+)\]\s*\{")
DATA_MEMBER_RE = re.compile(r"^(?P<indent>\s*)data\s+\+(?P<offset>0x[0-9A-Fa-f]+)\s+\[sizeof=(?P<size>\d+)\]\s+.+?\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*\d+)?$")

T = TypeVar("T")


def log(message: str) -> None:
    """Print one status line to stderr and return no value."""
    print(message, file=sys.stderr, flush=True)


def retry_forever(description: str, delay_seconds: int, operation: Callable[[], T]) -> T:
    """Run an operation until it succeeds and return the operation result.

    Inputs:
    - description: Human-readable operation label included in retry messages.
    - delay_seconds: Number of seconds to wait after each failure.
    - operation: Callable that performs one unit of file, process, or JSON work.

    Processing:
    - Calls operation inside a broad exception guard.
    - On any exception, logs the failure, waits, and tries again.  This is
      intentional because the source drive is reported to disappear briefly.

    Return behavior:
    - Returns the first successful operation result.
    - Does not return while the operation continues to fail.
    """
    while True:
        try:
            return operation()
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # noqa: BLE001 - transient drive loss can raise many exception types.
            log(f"[retry] {description} failed: {exc}; retrying in {delay_seconds}s")
            time.sleep(delay_seconds)


def ensure_path(path: Path, kind: str) -> None:
    """Validate a required file or directory and return no value.

    Inputs:
    - path: Path to validate.
    - kind: Either "file" or "directory".

    Processing:
    - Checks existence and type using pathlib.
    - Raises FileNotFoundError or NotADirectoryError on mismatch.

    Return behavior:
    - Returns None when validation succeeds.
    """
    if kind == "file":
        if not path.is_file():
            raise FileNotFoundError(path)
        return
    if kind == "directory":
        if not path.is_dir():
            raise NotADirectoryError(path)
        return
    raise ValueError(f"unknown path kind: {kind}")


def extract_struct_block(lines: list[str], struct_name: str) -> list[str]:
    """Extract the printed class-layout block for one structure.

    Inputs:
    - lines: llvm-pdbutil pretty output split into lines.
    - struct_name: Structure name such as _ETHREAD or _KTHREAD.

    Processing:
    - Finds the exact structure header emitted by llvm-pdbutil pretty.
    - Tracks brace depth using standalone layout braces and member lines that
      open nested aggregate blocks.

    Return behavior:
    - Returns the lines inside the target structure block, excluding the header
      and final closing brace.  Returns an empty list if the structure is absent.
    """
    for index, line in enumerate(lines):
        header = STRUCT_HEADER_RE.match(line)
        if not header or header.group(2) != struct_name:
            continue
        block: list[str] = []
        depth = 1
        for body_line in lines[index + 1 :]:
            stripped = body_line.strip()
            if stripped == "}":
                depth -= 1
                if depth == 0:
                    return block
            block.append(body_line)
            if stripped.endswith("{"):
                depth += 1
        return block
    return []


def leading_spaces(line: str) -> int:
    """Count leading spaces in a text line and return the count."""
    return len(line) - len(line.lstrip(" "))


def resolve_member_offset_from_pretty(lines: list[str], struct_name: str, member_name: str) -> int | None:
    """Resolve one direct structure member offset from llvm-pdbutil pretty text.

    Inputs:
    - lines: llvm-pdbutil pretty output split into lines.
    - struct_name: Structure name containing the requested member.
    - member_name: Direct member name to find.

    Processing:
    - Extracts the structure block.
    - Uses the first direct data-member indentation level as the structure's
      direct field level; nested fields have deeper indentation and are ignored.
    - Matches member names exactly and parses the hexadecimal byte offset.

    Return behavior:
    - Returns the byte offset as an int, or None when the member is missing.
    """
    block = extract_struct_block(lines, struct_name)
    direct_indent: int | None = None
    for line in block:
        match = DATA_MEMBER_RE.match(line)
        if not match:
            continue
        indent = leading_spaces(match.group("indent"))
        if direct_indent is None:
            direct_indent = indent
        if indent != direct_indent or match.group("name") != member_name:
            continue
        return int(match.group("offset"), 16)
    return None


def read_json_file(path: Path) -> dict[str, Any]:
    """Read a UTF-8 JSON object from disk and return it."""
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"top-level JSON is not an object: {path}")
    return data


def profile_json_paths(profile_root: Path) -> list[Path]:
    """List profile JSON paths and return them sorted by file name."""
    return sorted(profile_root.glob("*.json"), key=lambda item: item.name.lower())


def collect_profile_index(profile_root: Path, retry_seconds: int) -> OrderedDict[str, dict[str, str]]:
    """Build a version-to-PDB index from existing profile JSON files.

    Inputs:
    - profile_root: Directory containing ark_dyndata profile JSON files.
    - retry_seconds: Delay used by retry wrappers around E: reads.

    Processing:
    - Reads each profile and extracts module.version, module.pdbSymbolKey,
      module.pdbName, module.file, and profileName.
    - De-duplicates by version; the first profile for a version wins because
      offsets are keyed by version in the requested output.

    Return behavior:
    - Returns an ordered mapping keyed by version string.
    """
    paths = retry_forever("list profile JSON files", retry_seconds, lambda: profile_json_paths(profile_root))
    index: OrderedDict[str, dict[str, str]] = OrderedDict()
    for path in paths:
        data = retry_forever(f"read profile {path}", retry_seconds, lambda path=path: read_json_file(path))
        module = data.get("module")
        if not isinstance(module, dict):
            continue
        version = str(module.get("version") or "").strip()
        symbol_key = str(module.get("pdbSymbolKey") or "").strip().upper()
        if not version or not symbol_key or version in index:
            continue
        index[version] = {
            "pdbSymbolKey": symbol_key,
            "pdbName": str(module.get("pdbName") or "ntkrnlmp.pdb"),
            "moduleFile": str(module.get("file") or ""),
            "profileName": str(data.get("profileName") or path.stem),
        }
    return index


def pdb_path_for_symbol_key(pdb_cache_root: Path, pdb_name: str, symbol_key: str) -> Path:
    r"""Resolve a cached PDB path for one PDB name and symbol key.

    Inputs:
    - pdb_cache_root: Architecture-specific cache root such as
      E:\KswordPDB\PDB\pdb-cache\amd64.
    - pdb_name: PDB file name recorded by the profile, for example
      ntkrnlmp.pdb or ntkrla57.pdb.
    - symbol_key: Uppercase PDB GUID+age cache key.

    Processing:
    - Reconstructs the standard symbol-cache layout used by the existing
      profile generator.

    Return behavior:
    - Returns the expected absolute PDB path.  Existence is checked later inside
      the retry wrapper so transient drive loss is handled uniformly.
    """
    normalized_pdb_name = pdb_name or "ntkrnlmp.pdb"
    return pdb_cache_root / normalized_pdb_name / symbol_key.upper() / normalized_pdb_name


def run_pdbutil_pretty_classes(pdbutil: Path, pdb_path: Path) -> str:
    """Run llvm-pdbutil pretty class-layout output and return stdout text.

    Inputs:
    - pdbutil: llvm-pdbutil executable.
    - pdb_path: Cached PDB to parse.

    Processing:
    - Starts llvm-pdbutil as a child process with the DIA-backed pretty reader.
    - Captures stdout/stderr and enforces a timeout per PDB.

    Return behavior:
    - Returns stdout text on exit code 0.
    - Raises RuntimeError containing stderr/stdout tail on failure.
    """
    completed = subprocess.run(
        [
            str(pdbutil),
            "pretty",
            "--classes",
            "--class-definitions=all",
            str(pdb_path),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=PDBUTIL_TIMEOUT_SECONDS,
    )
    if completed.returncode == 0:
        return completed.stdout
    tail = ((completed.stderr or "") + "\n" + (completed.stdout or ""))[-4000:]
    raise RuntimeError(f"llvm-pdbutil exited {completed.returncode}: {tail}")


def offsets_for_pdb(pdbutil: Path, pdb_path: Path, retry_seconds: int) -> OrderedDict[str, str | None]:
    """Resolve all requested offsets from one PDB and return a field map.

    Inputs:
    - pdbutil: llvm-pdbutil executable.
    - pdb_path: Cached PDB file.
    - retry_seconds: Delay used by retry wrappers around E: reads and parsing.

    Processing:
    - Validates the PDB path each attempt before launching llvm-pdbutil.
    - Parses pretty class-layout lines and resolves each target field
      independently.

    Return behavior:
    - Returns an OrderedDict whose keys match TARGET_FIELDS.
    - Values are uppercase 0xNNNN strings; missing fields are null in JSON.
    """
    def dump() -> str:
        ensure_path(pdb_path, "file")
        return run_pdbutil_pretty_classes(pdbutil, pdb_path)

    pretty_text = retry_forever(f"dump class layouts from {pdb_path}", retry_seconds, dump)
    lines = pretty_text.splitlines()
    result: OrderedDict[str, str | None] = OrderedDict()
    for item_name, (struct_name, member_name) in TARGET_FIELDS.items():
        offset = resolve_member_offset_from_pretty(lines, struct_name, member_name)
        if item_name == "_ETHREAD->ContextSwitches" and offset is None:
            # ETHREAD embeds KTHREAD as Tcb at offset zero on the x64 kernel
            # layouts in this corpus.  Some pretty/DIA output exposes
            # ContextSwitches only as the nested KTHREAD field, while the memory
            # offset relative to ETHREAD remains the same because Tcb starts at
            # +0x0000.
            offset = resolve_member_offset_from_pretty(lines, "_KTHREAD", "ContextSwitches")
        result[item_name] = None if offset is None else f"0x{offset:04X}"
    return result


def version_sort_key(version: str) -> tuple[int, ...]:
    """Return a numeric sort key for dotted Windows version strings."""
    parts: list[int] = []
    for item in version.split("."):
        try:
            parts.append(int(item))
        except ValueError:
            parts.append(0)
    return tuple(parts)


def write_output(path: Path, data: OrderedDict[str, OrderedDict[str, str | None]]) -> None:
    """Write the final JSON output atomically and return no value."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = path.with_suffix(path.suffix + ".tmp")
    with temp_path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    temp_path.replace(path)


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments and return the argparse namespace."""
    parser = argparse.ArgumentParser(description="Export ETHREAD/KTHREAD offsets from cached Windows kernel PDBs")
    parser.add_argument("--profile-root", type=Path, default=DEFAULT_PROFILE_ROOT)
    parser.add_argument("--pdb-cache-root", type=Path, default=DEFAULT_PDB_CACHE_ROOT)
    parser.add_argument("--llvm-pdbutil", type=Path, default=DEFAULT_LLVM_PDBUTIL)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--retry-seconds", type=int, default=DEFAULT_RETRY_SECONDS)
    parser.add_argument("--limit", type=int, default=0, help="Optional maximum versions to process for smoke testing")
    return parser.parse_args()


def main() -> int:
    """Run the export process and return a process exit code."""
    args = parse_args()
    retry_seconds = max(1, args.retry_seconds)

    retry_forever("validate profile root", retry_seconds, lambda: ensure_path(args.profile_root, "directory"))
    retry_forever("validate PDB cache root", retry_seconds, lambda: ensure_path(args.pdb_cache_root, "directory"))
    ensure_path(args.llvm_pdbutil, "file")

    profile_index = collect_profile_index(args.profile_root, retry_seconds)
    if args.limit > 0:
        profile_index = OrderedDict(list(profile_index.items())[: args.limit])
    log(f"[info] indexed {len(profile_index)} versions from {args.profile_root}")

    pdb_offsets_cache: dict[str, OrderedDict[str, str | None]] = {}
    output: OrderedDict[str, OrderedDict[str, str | None]] = OrderedDict()
    for ordinal, version in enumerate(sorted(profile_index.keys(), key=version_sort_key), start=1):
        identity = profile_index[version]
        symbol_key = identity["pdbSymbolKey"]
        pdb_name = identity["pdbName"]
        cache_key = f"{pdb_name.upper()}:{symbol_key}"
        pdb_path = pdb_path_for_symbol_key(args.pdb_cache_root, pdb_name, symbol_key)
        log(f"[info] {ordinal}/{len(profile_index)} version={version} pdb={pdb_name} pdbKey={symbol_key}")
        if cache_key not in pdb_offsets_cache:
            pdb_offsets_cache[cache_key] = offsets_for_pdb(args.llvm_pdbutil, pdb_path, retry_seconds)
        output[version] = OrderedDict(pdb_offsets_cache[cache_key])

    retry_forever(f"write output {args.output}", retry_seconds, lambda: write_output(args.output, output))
    log(f"[ok] wrote {len(output)} versions to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
