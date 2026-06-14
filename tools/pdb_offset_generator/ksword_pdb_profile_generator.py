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
- Use llvm-pdbutil to resolve selected private structure member offsets and
  callback v2 PDB items.

Returns:
- One JSON profile per PE/PDB identity, suitable for KswordARK R3 parsing.
- Optional callback item diagnostics are included without making missing private
  symbols fatal, because Microsoft public PDB coverage varies by build.
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
from typing import Any, Iterable

import pefile
import requests

DEFAULT_SYMBOL_SERVER = "https://msdl.microsoft.com/download/symbols"
DEFAULT_SYMBOL_ROOT = r"D:\PDB"
DEFAULT_LLVM_PDBUTIL = r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe"
HTTP_TIMEOUT_SECONDS = 600
PDBUTIL_TIMEOUT_SECONDS = 600
KSW_SCHEMA_VERSION = 1
CALLBACK_ITEM_SCHEMA_VERSION = 2

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

CALLBACK_GLOBAL_RVA_NAMES: tuple[str, ...] = (
    "PspCreateProcessNotifyRoutine",
    "PspCreateThreadNotifyRoutine",
    "PspLoadImageNotifyRoutine",
    "PspNotifyEnableMask",
    "CmCallbackListHead",
)

CALLBACK_STRUCT_FIELD_MAP: dict[str, tuple[str, str]] = {
    "_OBJECT_TYPE.CallbackList": ("_OBJECT_TYPE", "CallbackList"),
    "_CALLBACK_ENTRY_ITEM.EntryItemList": ("_CALLBACK_ENTRY_ITEM", "EntryItemList"),
    "_CALLBACK_ENTRY_ITEM.PreOperation": ("_CALLBACK_ENTRY_ITEM", "PreOperation"),
    "_CALLBACK_ENTRY_ITEM.PostOperation": ("_CALLBACK_ENTRY_ITEM", "PostOperation"),
    "_CALLBACK_ENTRY_ITEM.Operations": ("_CALLBACK_ENTRY_ITEM", "Operations"),
    "_CALLBACK_ENTRY_ITEM.CallbackEntry": ("_CALLBACK_ENTRY_ITEM", "CallbackEntry"),
    "_CALLBACK_ENTRY.Altitude": ("_CALLBACK_ENTRY", "Altitude"),
    "_CALLBACK_ENTRY.RegistrationContext": ("_CALLBACK_ENTRY", "RegistrationContext"),
}

TYPE_HEADER_RE = re.compile(r"^\s*([0-9A-Fa-fx]+)\s*\|\s*(LF_[A-Z0-9_]+)\b")
FIELD_LIST_RE = re.compile(r"field list:\s*(?:<fieldlist\s+)?([0-9A-Fa-fx]+)")
OFFSET_RE = re.compile(r"offset\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
TYPE_REF_RE = re.compile(r"(?:type|Type)\s*=\s*([^,\]\s]+)")
SYMBOL_HEADER_RE = re.compile(r"^\s*([0-9A-Fa-fx]+)\s*\|\s*(S_[A-Z0-9_]+)\b.*`([^`]+)`")
SYMBOL_ADDR_RE = re.compile(r"\baddr\s*=\s*([0-9A-Fa-f]+):([0-9A-Fa-f]+)")


@dataclass(frozen=True)
class KphDynEntry:
    """One PE row used to seed profile generation.

    Inputs:
    - arch/version/file_name/hash/timestamp/size/class_name: Identity values
      from kphdyn.xml or a local PE dry-run.
    - machine: COFF machine type stored in generated JSON for R3/R0 matching.

    Processing:
    - Instances are immutable data carriers; download and validation helpers
      consume them without mutating profile identity.

    Return behavior:
    - The dataclass has no behavior beyond holding normalized PE metadata.
    """

    arch: str
    version: str
    file_name: str
    sha256: str
    timestamp: int
    size_of_image: int
    class_name: str
    machine: int = 0x8664


@dataclass(frozen=True)
class PdbIdentity:
    """CodeView RSDS identity parsed from the PE image.

    Inputs:
    - pdb_name/pdb_guid/pdb_age: Values from the CodeView RSDS record.
    - symbol_key: Microsoft symbol-server key built from GUID without dashes and
      hexadecimal age.

    Processing:
    - Instances are immutable and are reused for both symbol download paths and
      generated JSON module identity.

    Return behavior:
    - The dataclass does not perform I/O.
    """

    pdb_name: str
    pdb_guid: str
    pdb_age: int
    symbol_key: str


@dataclass(frozen=True)
class SymbolAddress:
    """One CodeView section-relative symbol address from llvm-pdbutil output.

    Inputs:
    - name/kind/source: Symbol metadata from a publics or globals dump.
    - section/offset: CodeView address tuple printed as "section:offset".

    Processing:
    - The address is converted to an RVA later using the PE section table so the
      parser does not rely on optional PDB section-header formatting.

    Return behavior:
    - Instances are immutable data records.
    """

    name: str
    kind: str
    source: str
    section: int
    offset: int


def parse_int(text: str) -> int:
    """Parse decimal or hex text and return an integer."""
    value = text.strip()
    return int(value, 16 if value.lower().startswith("0x") else 10)


def parse_codeview_addr_int(text: str) -> int:
    """Parse one llvm-pdbutil section:offset integer token.

    Inputs:
    - text: The section or offset token from an "addr = section:offset" line.

    Processing:
    - llvm-pdbutil normally prints these tokens as decimal values with leading
      zero padding, for example "0006:345376".
    - Some future/older formats may include hexadecimal letters or a 0x prefix,
      so the helper switches to base 16 only when the token clearly requires it.

    Return behavior:
    - Returns an integer or raises ValueError for malformed text.
    """
    value = text.strip()
    if value.lower().startswith("0x") or re.search(r"[A-Fa-f]", value):
        return int(value, 16)
    return int(value, 10)


def normalize_record_id(record_id: str) -> str:
    """Normalize llvm-pdbutil type IDs for stable comparisons."""
    return record_id.strip().lower().removeprefix("0x")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse CLI options and return argparse namespace.

    Inputs:
    - argv: Optional command-line argument list for tests; None uses sys.argv.

    Processing:
    - Keeps the existing kphdyn-driven corpus workflow as the default mode.
    - Adds an explicit local dry-run mode for one PE/PDB pair so callback item
      parsing can be validated without downloads, Release writes, or corpus
      refreshes.

    Return behavior:
    - Returns argparse.Namespace with normalized option values only; no I/O is
      performed by this helper.
    """
    parser = argparse.ArgumentParser(description="Generate KswordARK PDB offset profiles")
    parser.add_argument("--kphdyn", default="third_party/systeminformer_dyn/kphdyn.xml", help="Path to kphdyn.xml")
    parser.add_argument("--symbol-root", default=DEFAULT_SYMBOL_ROOT, help="Corpus/cache root")
    parser.add_argument("--symbol-server", default=DEFAULT_SYMBOL_SERVER, help="Microsoft symbol server base URL")
    parser.add_argument("--llvm-pdbutil", default=DEFAULT_LLVM_PDBUTIL, help="Path to llvm-pdbutil.exe")
    parser.add_argument("--arch", default="amd64", help="kphdyn arch filter")
    parser.add_argument("--version", default=None, help="Optional version prefix filter, e.g. 10.0.26100")
    parser.add_argument("--limit", type=int, default=0, help="Stop after N generated profiles; 0 means no limit")
    parser.add_argument("--skip-existing", action="store_true", help="Do not regenerate existing JSON profiles")
    parser.add_argument("--file", default="ntoskrnl.exe,ntkrla57.exe", help="Comma-separated PE file names")
    parser.add_argument("--dry-run", action="store_true", help="Process exactly one --local-pe/--local-pdb pair without symbol downloads")
    parser.add_argument("--local-pe", default="", help="Local PE path for dry-run validation")
    parser.add_argument("--local-pdb", default="", help="Local PDB path for dry-run validation")
    parser.add_argument(
        "--output",
        default="",
        help=r"Optional JSON output path for dry-run; use D:\PDB\scratch or another temp directory",
    )
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


def pe_machine_to_arch(machine: int) -> str:
    """Map a COFF machine value to the profile architecture string.

    Inputs:
    - machine: IMAGE_FILE_HEADER.Machine from a PE image.

    Processing:
    - Recognizes the architectures the generator can reasonably label today.
    - Uses a stable hexadecimal fallback so dry-run output remains explicit for
      unexpected machine values.

    Return behavior:
    - Returns a short architecture string and performs no I/O.
    """
    if machine == 0x8664:
        return "amd64"
    if machine == 0xAA64:
        return "arm64"
    if machine == 0x014C:
        return "x86"
    return f"machine_0x{machine:04X}"


def file_version_from_pe(pe: pefile.PE) -> str:
    """Extract a best-effort file version string from a parsed PE.

    Inputs:
    - pe: pefile.PE instance that may contain VERSIONINFO resources.

    Processing:
    - First checks StringFileInfo because it preserves the human-readable build
      number used by existing corpus file names.
    - Falls back to VS_FIXEDFILEINFO numeric components when string metadata is
      absent.

    Return behavior:
    - Returns a non-empty version string; "0.0.0.0" indicates no version data.
    """
    for file_info in getattr(pe, "FileInfo", []) or []:
        for child in getattr(file_info, "StringTable", []) or []:
            entries = getattr(child, "entries", {}) or {}
            for key, value in entries.items():
                key_text = key.decode("utf-8", errors="ignore") if isinstance(key, bytes) else str(key)
                if key_text.lower() != "fileversion":
                    continue
                value_text = value.decode("utf-8", errors="ignore") if isinstance(value, bytes) else str(value)
                normalized = value_text.replace(",", ".").strip()
                if normalized:
                    return normalized
    fixed_info = getattr(pe, "VS_FIXEDFILEINFO", []) or []
    if fixed_info:
        info = fixed_info[0]
        return (
            f"{info.FileVersionMS >> 16}.{info.FileVersionMS & 0xFFFF}."
            f"{info.FileVersionLS >> 16}.{info.FileVersionLS & 0xFFFF}"
        )
    return "0.0.0.0"


def entry_from_local_pe(pe_path: Path) -> KphDynEntry:
    """Build a KphDynEntry from one local PE for dry-run validation.

    Inputs:
    - pe_path: Existing PE image path supplied by --local-pe.

    Processing:
    - Reads COFF identity fields, SizeOfImage, file version, SHA256, and module
      class without downloading or writing corpus files.

    Return behavior:
    - Returns a KphDynEntry suitable for build_profile; raises pefile/OSError
      exceptions if the PE cannot be parsed.
    """
    pe = pefile.PE(str(pe_path), fast_load=False)
    try:
        machine = int(pe.FILE_HEADER.Machine)
        file_name = pe_path.name
        return KphDynEntry(
            arch=pe_machine_to_arch(machine),
            version=file_version_from_pe(pe),
            file_name=file_name,
            sha256=sha256_file(pe_path),
            timestamp=int(pe.FILE_HEADER.TimeDateStamp),
            size_of_image=int(pe.OPTIONAL_HEADER.SizeOfImage),
            class_name=module_class_for_file(file_name),
            machine=machine,
        )
    finally:
        pe.close()


def rva_from_section_offset(pe_path: Path, section_number: int, section_offset: int) -> int | None:
    """Convert a CodeView section:offset tuple to a PE RVA.

    Inputs:
    - pe_path: PE image whose section table corresponds to the PDB.
    - section_number: One-based section index from llvm-pdbutil symbol output.
    - section_offset: Offset within that section from llvm-pdbutil.

    Processing:
    - Uses the PE section table, not PDB section-header text, because PE parsing
      is already required for module identity and is less sensitive to
      llvm-pdbutil formatting changes.
    - Rejects section number zero, indexes outside the PE section table, and
      offsets beyond the larger of VirtualSize and SizeOfRawData.

    Return behavior:
    - Returns an integer RVA on success; returns None when the address cannot be
      safely mapped.
    """
    if section_number <= 0:
        return None
    pe = pefile.PE(str(pe_path), fast_load=True)
    try:
        if section_number > len(pe.sections):
            return None
        section = pe.sections[section_number - 1]
        section_span = max(int(section.Misc_VirtualSize), int(section.SizeOfRawData))
        if section_offset < 0 or section_offset > section_span:
            return None
        return int(section.VirtualAddress) + section_offset
    finally:
        pe.close()


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


def run_llvm_pdbutil_dump(pdb_path: Path, pdbutil_path: str, *options: str) -> str:
    """Run one llvm-pdbutil dump command and return stdout text.

    Inputs:
    - pdb_path: PDB file to inspect.
    - pdbutil_path: llvm-pdbutil executable path or command name.
    - options: Dump options such as -types, -globals, or -publics.

    Processing:
    - Invokes llvm-pdbutil with a fixed timeout and captures stdout/stderr.
    - Leaves error handling to callers so required type parsing can fail loudly
      while optional callback symbol parsing can degrade into diagnostics.

    Return behavior:
    - Returns stdout text on success; subprocess exceptions propagate.
    """
    completed = subprocess.run(
        [pdbutil_path, "dump", *options, str(pdb_path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=PDBUTIL_TIMEOUT_SECONDS,
    )
    return completed.stdout


def run_llvm_pdbutil_types(pdb_path: Path, pdbutil_path: str) -> str:
    """Run llvm-pdbutil dump -types and return stdout text.

    Inputs:
    - pdb_path/pdbutil_path: Required PDB and llvm-pdbutil executable.

    Processing:
    - Delegates to run_llvm_pdbutil_dump with -types because legacy fields and
      callback struct offsets both depend on TPI records.

    Return behavior:
    - Returns the raw type dump text; raises if llvm-pdbutil fails.
    """
    return run_llvm_pdbutil_dump(pdb_path, pdbutil_path, "-types")


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


def parse_symbol_addresses(symbols_text: str, source: str) -> list[SymbolAddress]:
    """Parse CodeView symbol addresses from llvm-pdbutil text.

    Inputs:
    - symbols_text: Output from llvm-pdbutil dump -globals or -publics.
    - source: Human-readable source label stored with parsed addresses.

    Processing:
    - Tracks symbol header records such as S_PUB32 and their following addr line.
    - Does not filter by candidate name here; exact matching is handled by the
      callback builder so the parser remains reusable across item sets.

    Return behavior:
    - Returns every parsed section-relative symbol address. Missing addr lines
      are ignored because not every symbol record carries an address.
    """
    addresses: list[SymbolAddress] = []
    pending: tuple[str, str] | None = None
    for line in symbols_text.splitlines():
        header = SYMBOL_HEADER_RE.match(line)
        if header:
            _record_offset, symbol_kind, symbol_name = header.groups()
            pending = (symbol_kind, symbol_name)
            inline_address = SYMBOL_ADDR_RE.search(line)
            if inline_address:
                section_text, offset_text = inline_address.groups()
                addresses.append(
                    SymbolAddress(
                        name=symbol_name,
                        kind=symbol_kind,
                        source=source,
                        section=parse_codeview_addr_int(section_text),
                        offset=parse_codeview_addr_int(offset_text),
                    )
                )
                pending = None
            continue
        if pending is None:
            continue
        address = SYMBOL_ADDR_RE.search(line)
        if not address:
            continue
        section_text, offset_text = address.groups()
        symbol_kind, symbol_name = pending
        addresses.append(
            SymbolAddress(
                name=symbol_name,
                kind=symbol_kind,
                source=source,
                section=parse_codeview_addr_int(section_text),
                offset=parse_codeview_addr_int(offset_text),
            )
        )
        pending = None
    return addresses


def collect_symbol_addresses(pdb_path: Path, pdbutil_path: str) -> tuple[dict[str, list[SymbolAddress]], list[dict[str, str]]]:
    """Collect optional globals/publics addresses from one PDB.

    Inputs:
    - pdb_path/pdbutil_path: PDB and llvm-pdbutil executable used for callback
      global RVA candidates.

    Processing:
    - Attempts both -globals and -publics because Microsoft public kernel PDBs
      commonly expose data symbols through the public stream while globals may
      be empty.
    - Treats dump failures as diagnostics, not hard failures, because callback
      items are optional extensions over the legacy field profile.

    Return behavior:
    - Returns (addresses_by_name, dump_failures). The first value maps exact
      symbol names to one or more parsed addresses; the second is JSON-ready
      diagnostic metadata.
    """
    addresses_by_name: dict[str, list[SymbolAddress]] = {}
    dump_failures: list[dict[str, str]] = []
    for option, source in (("-globals", "globals"), ("-publics", "publics")):
        try:
            dump_text = run_llvm_pdbutil_dump(pdb_path, pdbutil_path, option)
        except Exception as exc:  # noqa: BLE001 - optional diagnostics preserve any tool failure.
            dump_failures.append({"source": source, "error": str(exc)})
            continue
        for address in parse_symbol_addresses(dump_text, source):
            addresses_by_name.setdefault(address.name, []).append(address)
    return addresses_by_name, dump_failures


def choose_symbol_address(addresses: list[SymbolAddress]) -> SymbolAddress:
    """Choose a deterministic address when a symbol appears in multiple streams.

    Inputs:
    - addresses: Exact-name matches from globals and/or publics.

    Processing:
    - Prefers globals over publics when both are available because globals carry
      richer private-symbol intent in full PDBs.
    - Uses section and offset as stable tie-breakers to avoid nondeterministic
      JSON output.

    Return behavior:
    - Returns one SymbolAddress. Callers must pass a non-empty list.
    """
    source_rank = {"globals": 0, "publics": 1}
    return sorted(addresses, key=lambda item: (source_rank.get(item.source, 9), item.section, item.offset))[0]


def build_callback_items(
    types_text: str,
    pe_path: Path,
    symbol_addresses: dict[str, list[SymbolAddress]],
    symbol_dump_failures: list[dict[str, str]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Build callback v2 item payload and diagnostics.

    Inputs:
    - types_text: llvm-pdbutil -types output used for struct offset candidates.
    - pe_path: PE image used to convert CodeView section:offset symbols to RVA.
    - symbol_addresses: Exact-name symbol address map from globals/publics.
    - symbol_dump_failures: Optional dump failure records from collection.

    Processing:
    - Resolves GlobalRva candidates from publics/globals and maps them to image
      RVAs using the PE section table.
    - Resolves StructOffset candidates from the same type dump used by legacy
      fields.
    - Records every missing or unmappable candidate in diagnostics instead of
      failing profile generation.

    Return behavior:
    - Returns (callback_items, diagnostics). callback_items is JSON-ready and
      diagnostics contains missingItems plus optional symbolDumpFailures.
    """
    callback_items: list[dict[str, Any]] = []
    missing_items: list[dict[str, str]] = []

    for symbol_name in CALLBACK_GLOBAL_RVA_NAMES:
        matches = symbol_addresses.get(symbol_name, [])
        if not matches:
            missing_items.append({"kind": "GlobalRva", "name": symbol_name, "reason": "symbol_not_found"})
            continue
        address = choose_symbol_address(matches)
        rva = rva_from_section_offset(pe_path, address.section, address.offset)
        if rva is None:
            missing_items.append(
                {
                    "kind": "GlobalRva",
                    "name": symbol_name,
                    "reason": "section_offset_unmapped",
                    "section": f"0x{address.section:04X}",
                    "sectionOffset": f"0x{address.offset:X}",
                }
            )
            continue
        callback_items.append(
            {
                "kind": "GlobalRva",
                "name": symbol_name,
                "value": f"0x{rva:08X}",
                "source": address.source,
                "symbolKind": address.kind,
                "section": f"0x{address.section:04X}",
                "sectionOffset": f"0x{address.offset:X}",
            }
        )

    for item_name, (struct_name, member_name) in CALLBACK_STRUCT_FIELD_MAP.items():
        offset = resolve_member_offset(types_text, struct_name, member_name)
        if offset is None:
            missing_items.append(
                {
                    "kind": "StructOffset",
                    "name": item_name,
                    "reason": "member_not_found",
                    "structName": struct_name,
                    "memberName": member_name,
                }
            )
            continue
        callback_items.append(
            {
                "kind": "StructOffset",
                "name": item_name,
                "value": f"0x{offset:04X}",
                "structName": struct_name,
                "memberName": member_name,
            }
        )

    diagnostics: dict[str, Any] = {
        "missingItems": missing_items,
    }
    if symbol_dump_failures:
        diagnostics["symbolDumpFailures"] = symbol_dump_failures
    return callback_items, diagnostics


def build_profile(
    entry: KphDynEntry,
    pe_path: Path,
    pdb_identity: PdbIdentity,
    types_text: str,
    symbol_addresses: dict[str, list[SymbolAddress]] | None = None,
    symbol_dump_failures: list[dict[str, str]] | None = None,
) -> dict[str, object]:
    """Build the Ksword JSON profile object from resolved PDB data.

    Inputs:
    - entry/pe_path/pdb_identity: Stable PE/PDB identity values.
    - types_text: llvm-pdbutil -types output for legacy and callback offsets.
    - symbol_addresses/symbol_dump_failures: Optional globals/publics data used
      only for callback GlobalRva items.

    Processing:
    - Preserves the legacy fields/missingFields schema exactly for current R3
      consumers and v1 release pack tooling.
    - Adds callbackItems as an optional v2 extension and puts all missing
      callback candidates under diagnostics.missingItems.

    Return behavior:
    - Returns a JSON-serializable dictionary; it does not write files.
    """
    resolved_fields: dict[str, str] = {}
    missing_fields: list[str] = []
    for ksword_name, (struct_name, member_name) in FIELD_MAP.items():
        offset = resolve_member_offset(types_text, struct_name, member_name)
        if offset is None:
            missing_fields.append(f"{struct_name}->{member_name}")
            continue
        resolved_fields[ksword_name] = f"0x{offset:04X}"

    callback_items, diagnostics = build_callback_items(
        types_text=types_text,
        pe_path=pe_path,
        symbol_addresses=symbol_addresses or {},
        symbol_dump_failures=symbol_dump_failures or [],
    )

    profile_name = f"{entry.class_name}_{entry.arch}_{entry.version}_{pdb_identity.symbol_key.lower()}"
    return {
        "schemaVersion": KSW_SCHEMA_VERSION,
        "callbackItemSchemaVersion": CALLBACK_ITEM_SCHEMA_VERSION,
        "profileName": profile_name,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "generator": "tools/pdb_offset_generator/ksword_pdb_profile_generator.py",
        "module": {
            "class": entry.class_name,
            "file": entry.file_name,
            "arch": entry.arch,
            "version": entry.version,
            "machine": f"0x{entry.machine:04X}",
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
        "callbackItems": callback_items,
        "diagnostics": diagnostics,
    }


def process_entry(entry: KphDynEntry, root: Path, symbol_server: str, pdbutil_path: str, skip_existing: bool) -> Path | None:
    """Download PE/PDB for one entry, generate JSON, and return the profile path.

    Inputs:
    - entry: kphdyn-derived PE identity to process.
    - root: Corpus/cache root where PE/PDB/profile files are stored.
    - symbol_server: Symbol server base URL.
    - pdbutil_path: llvm-pdbutil executable.
    - skip_existing: Whether to preserve an already generated JSON profile.

    Processing:
    - Preserves the existing corpus workflow: download PE, validate SHA256,
      parse RSDS identity, download matching PDB, then parse PDB data.
    - Adds optional callback symbol collection before profile construction.

    Return behavior:
    - Returns the profile path when a profile exists or is written; returns None
      only for future extension points that intentionally skip a row.
    """
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
    print(f"[SYMS] {pdb_path}")
    symbol_addresses, symbol_dump_failures = collect_symbol_addresses(pdb_path, pdbutil_path)
    profile = build_profile(entry, pe_path, pdb_identity, types_text, symbol_addresses, symbol_dump_failures)
    profile_path.parent.mkdir(parents=True, exist_ok=True)
    profile_path.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"[OK] {profile_path}")
    return profile_path


def default_dry_run_output_path(entry: KphDynEntry, pdb_identity: PdbIdentity) -> Path:
    """Return the default local dry-run output path under D:/PDB/scratch.

    Inputs:
    - entry/pdb_identity: Values used to build a deterministic file name.

    Processing:
    - Mirrors profile_path_for_entry naming without using the corpus profile
      directory, so dry-run validation cannot accidentally refresh runtime
      profile corpus files.

    Return behavior:
    - Returns a Path; the caller creates parent directories when writing.
    """
    safe_version = entry.version.replace(".", "_")
    safe_key = pdb_identity.symbol_key.lower()
    return Path(r"D:\PDB\scratch") / f"{entry.class_name}_{entry.arch}_{safe_version}_{safe_key}.json"


def run_local_dry_run(args: argparse.Namespace) -> int:
    """Run callback parsing against exactly one local PE/PDB pair.

    Inputs:
    - args: Parsed CLI namespace containing --local-pe, --local-pdb, and optional
      --output.

    Processing:
    - Does not download symbols, build projects, write Release directories, or
      touch the normal profile corpus.
    - Writes the JSON only to --output or D:/PDB/scratch by default.

    Return behavior:
    - Returns 0 on success and 2 for argument/path validation failures.
    """
    pe_path = Path(args.local_pe) if args.local_pe else None
    pdb_path = Path(args.local_pdb) if args.local_pdb else None
    if pe_path is None or pdb_path is None:
        print("--dry-run requires --local-pe and --local-pdb", file=sys.stderr)
        return 2
    if not pe_path.exists():
        print(f"local PE not found: {pe_path}", file=sys.stderr)
        return 2
    if not pdb_path.exists():
        print(f"local PDB not found: {pdb_path}", file=sys.stderr)
        return 2

    entry = entry_from_local_pe(pe_path)
    pdb_identity = parse_rsds_identity(pe_path)
    output_path = Path(args.output) if args.output else default_dry_run_output_path(entry, pdb_identity)

    print(f"[DRY-RUN PE] {pe_path}")
    print(f"[DRY-RUN PDB] {pdb_path}")
    print(f"[TYPES] {pdb_path}")
    types_text = run_llvm_pdbutil_types(pdb_path, args.llvm_pdbutil)
    print(f"[SYMS] {pdb_path}")
    symbol_addresses, symbol_dump_failures = collect_symbol_addresses(pdb_path, args.llvm_pdbutil)
    profile = build_profile(entry, pe_path, pdb_identity, types_text, symbol_addresses, symbol_dump_failures)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"[OK] {output_path}")
    print(f"callbackItems={len(profile.get('callbackItems', []))}")
    diagnostics = profile.get("diagnostics", {})
    missing_items = diagnostics.get("missingItems", []) if isinstance(diagnostics, dict) else []
    print(f"missingItems={len(missing_items) if isinstance(missing_items, list) else 0}")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Program entry point. Returns a process exit code.

    Inputs:
    - argv: Optional argument list; None uses process command-line arguments.

    Processing:
    - Validates llvm-pdbutil once.
    - Dispatches either a local dry-run or the existing kphdyn-driven corpus
      generation loop.

    Return behavior:
    - Returns 0 when at least one requested profile path is produced, non-zero
      for validation failures or no generated profiles.
    """
    args = parse_args(argv)
    if not Path(args.llvm_pdbutil).exists() and os.path.sep in args.llvm_pdbutil:
        print(f"llvm-pdbutil not found: {args.llvm_pdbutil}", file=sys.stderr)
        return 2
    if args.dry_run:
        return run_local_dry_run(args)

    root = Path(args.symbol_root)
    files = {item.strip().lower() for item in args.file.split(",") if item.strip()}
    entries = list(iter_kphdyn_entries(Path(args.kphdyn), args.arch, args.version, files))
    if not entries:
        print("No matching kphdyn entries found.", file=sys.stderr)
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
