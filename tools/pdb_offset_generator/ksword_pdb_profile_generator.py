#!/usr/bin/env python3
"""
KswordARK PDB offset profile generator.

Inputs:
- kphdyn.xml rows containing PE TimeDateStamp, SizeOfImage, file version, and SHA256.
- Microsoft public symbol server for PE/PDB downloads.

Processing:
- Download PE by the public symbol-server PE key.
- Verify SHA256 before trusting the image.
- Parse CodeView RSDS data from the PE to obtain PDB name, GUID, and age.
- Download the exact PDB by GUID+age.
- Use llvm-pdbutil to resolve selected private structure member offsets.

Returns:
- One JSON profile per PE/PDB identity, suitable for KswordARK R3 parsing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import pefile
import requests

DEFAULT_SYMBOL_SERVER = "https://msdl.microsoft.com/download/symbols"
DEFAULT_LLVM_PDBUTIL = r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe"
HTTP_TIMEOUT_SECONDS = 600
PDBUTIL_TIMEOUT_SECONDS = 600
KSW_SCHEMA_VERSION = 1

FIELD_MAP: dict[str, tuple[str, str]] = {
    "EpObjectTable": ("_EPROCESS", "ObjectTable"),
    "EpSectionObject": ("_EPROCESS", "SectionObject"),
    "EpProtection": ("_EPROCESS", "Protection"),
    "EpSignatureLevel": ("_EPROCESS", "SignatureLevel"),
    "EpSectionSignatureLevel": ("_EPROCESS", "SectionSignatureLevel"),
    "KtInitialStack": ("_KTHREAD", "InitialStack"),
    "KtStackLimit": ("_KTHREAD", "StackLimit"),
    "KtStackBase": ("_KTHREAD", "StackBase"),
    "KtKernelStack": ("_KTHREAD", "KernelStack"),
    "KtReadOperationCount": ("_KTHREAD", "ReadOperationCount"),
    "KtWriteOperationCount": ("_KTHREAD", "WriteOperationCount"),
    "KtOtherOperationCount": ("_KTHREAD", "OtherOperationCount"),
    "KtReadTransferCount": ("_KTHREAD", "ReadTransferCount"),
    "KtWriteTransferCount": ("_KTHREAD", "WriteTransferCount"),
    "KtOtherTransferCount": ("_KTHREAD", "OtherTransferCount"),
}

TYPE_HEADER_RE = re.compile(r"^\s*([0-9A-Fa-fx]+)\s*\|\s*(LF_[A-Z0-9_]+)\b")
FIELD_LIST_RE = re.compile(r"field list:\s*(?:<fieldlist\s+)?([0-9A-Fa-fx]+)")
OFFSET_RE = re.compile(r"offset\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
TYPE_REF_RE = re.compile(r"(?:type|Type)\s*=\s*([^,\]\s]+)")


@dataclass(frozen=True)
class KphDynEntry:
    """One kphdyn.xml PE row used to seed a symbol-server PE download."""

    arch: str
    version: str
    file_name: str
    sha256: str
    timestamp: int
    size_of_image: int
    class_name: str


@dataclass(frozen=True)
class PdbIdentity:
    """CodeView RSDS identity parsed from the downloaded PE image."""

    pdb_name: str
    pdb_guid: str
    pdb_age: int
    symbol_key: str


def parse_int(text: str) -> int:
    """Parse decimal or hex text and return an integer."""
    value = text.strip()
    return int(value, 16 if value.lower().startswith("0x") else 10)


def normalize_record_id(record_id: str) -> str:
    """Normalize llvm-pdbutil type IDs for stable comparisons."""
    return record_id.strip().lower().removeprefix("0x")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse CLI options and return argparse namespace."""
    parser = argparse.ArgumentParser(description="Generate KswordARK PDB offset profiles")
    parser.add_argument("--kphdyn", default="third_party/systeminformer_dyn/kphdyn.xml", help="Path to kphdyn.xml")
    parser.add_argument("--symbol-root", default=r"D:\KswordKernelCorpus", help="Corpus/cache root")
    parser.add_argument("--symbol-server", default=DEFAULT_SYMBOL_SERVER, help="Microsoft symbol server base URL")
    parser.add_argument("--llvm-pdbutil", default=DEFAULT_LLVM_PDBUTIL, help="Path to llvm-pdbutil.exe")
    parser.add_argument("--arch", default="amd64", help="kphdyn arch filter")
    parser.add_argument("--version", default=None, help="Optional version prefix filter, e.g. 10.0.26100")
    parser.add_argument("--limit", type=int, default=0, help="Stop after N generated profiles; 0 means no limit")
    parser.add_argument("--skip-existing", action="store_true", help="Do not regenerate existing JSON profiles")
    parser.add_argument("--file", default="ntoskrnl.exe,ntkrla57.exe", help="Comma-separated PE file names")
    return parser.parse_args(argv)


def module_class_for_file(file_name: str) -> str:
    """Map a PE file name to the profile class consumed by KswordARK."""
    lowered = file_name.lower()
    if lowered == "ntkrla57.exe":
        return "ntkrla57"
    return "ntoskrnl"


def iter_kphdyn_entries(path: Path, arch: str, version: str | None, files: set[str]) -> Iterable[KphDynEntry]:
    """Yield matching kphdyn.xml rows with complete PE identity attributes."""
    tree = ET.parse(path)
    root = tree.getroot()
    for data in root.findall("data"):
        file_name = (data.get("file") or "").strip()
        if file_name.lower() not in files:
            continue
        row_arch = (data.get("arch") or "").strip()
        if arch and row_arch != arch:
            continue
        row_version = (data.get("version") or "").strip()
        if version and not row_version.startswith(version):
            continue
        sha256 = (data.get("hash") or "").strip().lower()
        timestamp = (data.get("timestamp") or "").strip()
        size_of_image = (data.get("size") or "").strip()
        if not sha256 or not timestamp or not size_of_image:
            continue
        yield KphDynEntry(
            arch=row_arch,
            version=row_version,
            file_name=file_name,
            sha256=sha256,
            timestamp=parse_int(timestamp),
            size_of_image=parse_int(size_of_image),
            class_name=module_class_for_file(file_name),
        )


def pe_symbol_key(entry: KphDynEntry) -> str:
    """Build the public symbol-server key for a PE image."""
    return f"{entry.timestamp:08X}{entry.size_of_image:X}"


def download_file(url: str, destination: Path) -> bool:
    """Download one URL atomically and return True when a new file was written."""
    if destination.exists() and destination.stat().st_size > 0:
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = destination.with_suffix(destination.suffix + ".tmp")
    response = requests.get(url, timeout=HTTP_TIMEOUT_SECONDS)
    response.raise_for_status()
    tmp_path.write_bytes(response.content)
    tmp_path.replace(destination)
    return True


def sha256_file(path: Path) -> str:
    """Hash a file and return lowercase SHA256 hex."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_path_for_entry(root: Path, entry: KphDynEntry) -> Path:
    """Return the local corpus path for a PE entry."""
    return root / "pe-store" / entry.arch / f"{entry.file_name}.{entry.version}" / entry.sha256 / entry.file_name


def pdb_path_for_identity(root: Path, entry: KphDynEntry, pdb: PdbIdentity) -> Path:
    """Return the local corpus path for a PDB identity."""
    return root / "pdb-cache" / entry.arch / pdb.pdb_name / pdb.symbol_key / pdb.pdb_name


def profile_path_for_entry(root: Path, entry: KphDynEntry, pdb: PdbIdentity) -> Path:
    """Return the output JSON profile path for a PE/PDB identity."""
    safe_version = entry.version.replace(".", "_")
    safe_key = pdb.symbol_key.lower()
    return root / "profiles" / "ark_dyndata" / f"{entry.class_name}_{entry.arch}_{safe_version}_{safe_key}.json"


def parse_rsds_identity(pe_path: Path) -> PdbIdentity:
    """Parse CodeView RSDS data from a PE and return PDB name/GUID/age."""
    pe = pefile.PE(str(pe_path), fast_load=False)
    try:
        if not hasattr(pe, "DIRECTORY_ENTRY_DEBUG"):
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"]])
        for debug_entry in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
            if debug_entry.struct.Type != 2:
                continue
            offset = debug_entry.struct.PointerToRawData
            size = debug_entry.struct.SizeOfData
            data = pe.__data__[offset : offset + size]
            if len(data) < 24 or data[:4] != b"RSDS":
                continue
            guid_bytes = data[4:20]
            age = int.from_bytes(data[20:24], "little")
            pdb_bytes = data[24:].split(b"\x00", 1)[0]
            pdb_name = Path(pdb_bytes.decode("utf-8", errors="replace")).name
            guid_text = (
                f"{int.from_bytes(guid_bytes[0:4], 'little'):08X}-"
                f"{int.from_bytes(guid_bytes[4:6], 'little'):04X}-"
                f"{int.from_bytes(guid_bytes[6:8], 'little'):04X}-"
                f"{guid_bytes[8]:02X}{guid_bytes[9]:02X}-"
                f"{''.join(f'{byte:02X}' for byte in guid_bytes[10:16])}"
            )
            symbol_key_text = guid_text.replace("-", "") + f"{age:X}"
            return PdbIdentity(pdb_name=pdb_name, pdb_guid=guid_text, pdb_age=age, symbol_key=symbol_key_text)
    finally:
        pe.close()
    raise RuntimeError(f"No RSDS CodeView record found in {pe_path}")


def run_llvm_pdbutil_types(pdb_path: Path, pdbutil_path: str) -> str:
    """Run llvm-pdbutil dump -types and return stdout text."""
    completed = subprocess.run(
        [pdbutil_path, "dump", "-types", str(pdb_path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=PDBUTIL_TIMEOUT_SECONDS,
    )
    return completed.stdout


def find_field_list_id(lines: list[str], struct_name: str) -> str | None:
    """Find the LF_FIELDLIST record referenced by a concrete structure."""
    allowed = {"LF_STRUCTURE", "LF_STRUCTURE2", "LF_CLASS", "LF_CLASS2"}
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header:
            continue
        _record_id, kind = header.groups()
        if kind not in allowed or f"`{struct_name}`" not in line or "forward ref" in line:
            continue
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            if "forward ref" in body_line:
                break
            match = FIELD_LIST_RE.search(body_line)
            if match:
                return match.group(1)
    return None


def field_list_lines(lines: list[str], field_list_id: str) -> list[str]:
    """Return the body lines of one LF_FIELDLIST record."""
    wanted = normalize_record_id(field_list_id)
    for index, line in enumerate(lines):
        header = TYPE_HEADER_RE.match(line)
        if not header:
            continue
        record_id, kind = header.groups()
        if kind != "LF_FIELDLIST" or normalize_record_id(record_id) != wanted:
            continue
        result: list[str] = []
        for body_line in lines[index + 1 :]:
            if TYPE_HEADER_RE.match(body_line):
                break
            result.append(body_line)
        return result
    return []


def member_name_from_line(line: str) -> str | None:
    """Extract an LF_MEMBER name from one llvm-pdbutil output line."""
    match = re.search(r"member name = `([^`]+)`", line)
    if match:
        return match.group(1)
    match = re.search(r"name\s*=\s*`([^`]+)`", line)
    return match.group(1) if match else None


def resolve_member_offset(types_text: str, struct_name: str, member_name: str) -> int | None:
    """Resolve one direct structure member offset from llvm-pdbutil type dump text."""
    lines = types_text.splitlines()
    field_list = find_field_list_id(lines, struct_name)
    if field_list is None:
        return None
    for line in field_list_lines(lines, field_list):
        if "LF_MEMBER" not in line or member_name_from_line(line) != member_name:
            continue
        match = OFFSET_RE.search(line)
        if match:
            return parse_int(match.group(1))
        type_match = TYPE_REF_RE.search(line)
        if type_match:
            continue
    return None


def build_profile(entry: KphDynEntry, pe_path: Path, pdb_identity: PdbIdentity, types_text: str) -> dict[str, object]:
    """Build the Ksword JSON profile object from resolved type offsets."""
    resolved_fields: dict[str, str] = {}
    missing_fields: list[str] = []
    for ksword_name, (struct_name, member_name) in FIELD_MAP.items():
        offset = resolve_member_offset(types_text, struct_name, member_name)
        if offset is None:
            missing_fields.append(f"{struct_name}->{member_name}")
            continue
        resolved_fields[ksword_name] = f"0x{offset:04X}"

    profile_name = f"{entry.class_name}_{entry.arch}_{entry.version}_{pdb_identity.symbol_key.lower()}"
    return {
        "schemaVersion": KSW_SCHEMA_VERSION,
        "profileName": profile_name,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "generator": "tools/pdb_offset_generator/ksword_pdb_profile_generator.py",
        "module": {
            "class": entry.class_name,
            "file": entry.file_name,
            "arch": entry.arch,
            "version": entry.version,
            "machine": "0x8664" if entry.arch == "amd64" else "0x0000",
            "timeDateStamp": f"0x{entry.timestamp:08X}",
            "sizeOfImage": f"0x{entry.size_of_image:08X}",
            "sha256": sha256_file(pe_path),
            "pdbName": pdb_identity.pdb_name,
            "pdbGuid": pdb_identity.pdb_guid,
            "pdbAge": pdb_identity.pdb_age,
            "pdbSymbolKey": pdb_identity.symbol_key,
        },
        "fields": resolved_fields,
        "missingFields": missing_fields,
    }


def process_entry(entry: KphDynEntry, root: Path, symbol_server: str, pdbutil_path: str, skip_existing: bool) -> Path | None:
    """Download PE/PDB for one entry, generate JSON, and return the profile path."""
    pe_path = pe_path_for_entry(root, entry)
    pe_url = f"{symbol_server.rstrip('/')}/{entry.file_name}/{pe_symbol_key(entry)}/{entry.file_name}"
    print(f"[PE] {entry.version} {entry.file_name} {pe_symbol_key(entry)}")
    download_file(pe_url, pe_path)
    actual_hash = sha256_file(pe_path)
    if actual_hash.lower() != entry.sha256.lower():
        raise RuntimeError(f"SHA256 mismatch for {pe_path}: expected {entry.sha256}, got {actual_hash}")

    pdb_identity = parse_rsds_identity(pe_path)
    profile_path = profile_path_for_entry(root, entry, pdb_identity)
    if skip_existing and profile_path.exists():
        print(f"[SKIP] {profile_path}")
        return profile_path

    pdb_path = pdb_path_for_identity(root, entry, pdb_identity)
    pdb_url = f"{symbol_server.rstrip('/')}/{pdb_identity.pdb_name}/{pdb_identity.symbol_key}/{pdb_identity.pdb_name}"
    print(f"[PDB] {pdb_identity.pdb_name} {pdb_identity.symbol_key}")
    download_file(pdb_url, pdb_path)

    print(f"[TYPES] {pdb_path}")
    types_text = run_llvm_pdbutil_types(pdb_path, pdbutil_path)
    profile = build_profile(entry, pe_path, pdb_identity, types_text)
    profile_path.parent.mkdir(parents=True, exist_ok=True)
    profile_path.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"[OK] {profile_path}")
    return profile_path


def main(argv: list[str] | None = None) -> int:
    """Program entry point. Returns a process exit code."""
    args = parse_args(argv)
    root = Path(args.symbol_root)
    files = {item.strip().lower() for item in args.file.split(",") if item.strip()}
    entries = list(iter_kphdyn_entries(Path(args.kphdyn), args.arch, args.version, files))
    if not entries:
        print("No matching kphdyn entries found.", file=sys.stderr)
        return 2
    if not Path(args.llvm_pdbutil).exists() and os.path.sep in args.llvm_pdbutil:
        print(f"llvm-pdbutil not found: {args.llvm_pdbutil}", file=sys.stderr)
        return 2

    generated = 0
    for entry in entries:
        try:
            result_path = process_entry(entry, root, args.symbol_server, args.llvm_pdbutil, args.skip_existing)
            if result_path is not None:
                generated += 1
            if args.limit and generated >= args.limit:
                break
        except requests.HTTPError as exc:
            print(f"[HTTP] {entry.version} {entry.file_name}: {exc}", file=sys.stderr)
        except Exception as exc:
            print(f"[ERR] {entry.version} {entry.file_name}: {exc}", file=sys.stderr)
    return 0 if generated > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
