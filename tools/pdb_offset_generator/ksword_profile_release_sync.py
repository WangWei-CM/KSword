#!/usr/bin/env python3
"""
Validate and publish KswordARK DynData JSON profiles.

Inputs:
- A private corpus directory, normally D:\\PDB\\profiles\\ark_dyndata.
- A KswordARK release root, normally the directory containing Ksword5.1.exe.
- Optional local ntoskrnl.exe path for an identity match smoke test.

Processing:
- Parse every JSON profile and enforce the same schema assumptions used by the
  R3 KernelDock loader and R0 packed profile apply protocol.
- Reject profiles with missing module identity, empty fields, unknown fields, or
  offsets outside the v1 protocol range.
- Deduplicate profiles by the R0 identity tuple used for safe apply:
  module class, machine, TimeDateStamp, and SizeOfImage.
- Copy only accepted profile JSON files into Release\\profiles\\ark_dyndata.
- Write a manifest outside the scanned JSON directory so KernelDock does not
  attempt to parse the manifest as a runtime profile.

Returns:
- Process exit code 0 when validation and optional copy complete.
- Non-zero exit code when the source directory is missing or no publishable
  profiles remain.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

DEFAULT_SOURCE_DIR = Path(r"D:\PDB\profiles\ark_dyndata")
DEFAULT_LOCAL_KERNEL = Path(r"C:\Windows\System32\ntoskrnl.exe")
KSW_DYN_PROFILE_OFFSET_MAX = 0x0000FFFF
KSW_SCHEMA_VERSION = 1

KNOWN_FIELD_NAMES = {
    "EpObjectTable",
    "EpSectionObject",
    "HtHandleContentionEvent",
    "OtName",
    "OtIndex",
    "ObDecodeShift",
    "ObAttributesShift",
    "KtInitialStack",
    "KtStackLimit",
    "KtStackBase",
    "KtKernelStack",
    "KtReadOperationCount",
    "KtWriteOperationCount",
    "KtOtherOperationCount",
    "KtReadTransferCount",
    "KtWriteTransferCount",
    "KtOtherTransferCount",
    "MmSectionControlArea",
    "MmControlAreaListHead",
    "MmControlAreaLock",
    "AlpcCommunicationInfo",
    "AlpcOwnerProcess",
    "AlpcConnectionPort",
    "AlpcServerCommunicationPort",
    "AlpcClientCommunicationPort",
    "AlpcHandleTable",
    "AlpcHandleTableLock",
    "AlpcAttributes",
    "AlpcAttributesFlags",
    "AlpcPortContext",
    "AlpcPortObjectLock",
    "AlpcSequenceNo",
    "AlpcState",
    "LxPicoProc",
    "LxPicoProcInfo",
    "LxPicoProcInfoPID",
    "LxPicoThrdInfo",
    "LxPicoThrdInfoTID",
    "EpProtection",
    "EpSignatureLevel",
    "EpSectionSignatureLevel",
    "EgeGuid",
    "EreGuidEntry",
}


@dataclass(frozen=True)
class ProfileIdentity:
    """Stable identity tuple used by R3/R0 profile matching.

    Inputs:
    - module_class: Ksword module class text, for example ntoskrnl or ntkrla57.
    - machine/time_date_stamp/size_of_image: PE identity fields.

    Processing:
    - The dataclass is frozen and hashable so it can be used as a dictionary key.

    Return behavior:
    - Instances do not perform I/O; callers use them for exact comparisons.
    """

    module_class: str
    machine: int
    time_date_stamp: int
    size_of_image: int


@dataclass
class ProfileRecord:
    """One validated profile candidate.

    Inputs:
    - path: Source JSON profile path.
    - data: Parsed JSON dictionary.
    - identity: R0-safe identity tuple.
    - field_count: Number of accepted fields.

    Processing:
    - The record keeps enough metadata for deduplication and manifest output.

    Return behavior:
    - Instances are plain data containers and do not copy files by themselves.
    """

    path: Path
    data: dict[str, Any]
    identity: ProfileIdentity
    field_count: int

    @property
    def profile_name(self) -> str:
        """Return the human-readable profileName, or the file stem if missing."""
        value = self.data.get("profileName")
        return str(value) if value else self.path.stem


@dataclass
class ValidationState:
    """Validation accumulator for one publish pass.

    Inputs:
    - accepted: publishable profiles after schema validation but before dedup.
    - rejected: rejected file records with reasons.
    - duplicate_groups: duplicate identity groups observed during dedup.

    Processing:
    - Callers append records while scanning and then build a manifest from it.

    Return behavior:
    - The object is mutated by scanner helpers and returned to the caller.
    """

    accepted: list[ProfileRecord] = field(default_factory=list)
    rejected: list[dict[str, str]] = field(default_factory=list)
    duplicate_groups: list[dict[str, Any]] = field(default_factory=list)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line options and return an argparse namespace."""
    parser = argparse.ArgumentParser(description="Validate and publish KswordARK DynData JSON profiles.")
    parser.add_argument("--source", default=str(DEFAULT_SOURCE_DIR), help="Directory containing generated JSON profiles.")
    parser.add_argument(
        "--release-root",
        default=str(Path("Ksword5.1") / "Ksword5.1" / "x64" / "Release"),
        help="KswordARK release root that contains Ksword5.1.exe.",
    )
    parser.add_argument("--local-kernel", default=str(DEFAULT_LOCAL_KERNEL), help="Optional ntoskrnl.exe path for match smoke test.")
    parser.add_argument("--dry-run", action="store_true", help="Validate and write reports without copying profiles.")
    parser.add_argument("--clean-target", action="store_true", help="Delete stale *.json files from target before copying.")
    parser.add_argument("--max-copy", type=int, default=0, help="Copy at most N accepted profiles; 0 means all.")
    parser.add_argument("--manifest", default="", help="Manifest path; defaults to <release-root>\\profiles\\ark_dyndata_manifest.json.")
    parser.add_argument("--report", default=str(Path(r"D:\PDB") / "logs" / "ark_dyndata_publish_report.json"), help="Detailed report JSON path.")
    return parser.parse_args(argv)


def parse_uint32(value: Any) -> int | None:
    """Parse a JSON number or string into an unsigned 32-bit integer.

    Inputs:
    - value: JSON value from profile metadata or field offsets.

    Processing:
    - Supports decimal integers and strings with optional 0x prefix.
    - Rejects negative values and values outside uint32.

    Return behavior:
    - Returns an int on success; returns None on invalid input.
    """
    try:
        if isinstance(value, int):
            parsed = value
        elif isinstance(value, float):
            if not value.is_integer():
                return None
            parsed = int(value)
        elif isinstance(value, str):
            text = value.strip()
            base = 16 if text.lower().startswith("0x") else 10
            parsed = int(text, base)
        else:
            return None
    except ValueError:
        return None
    if parsed < 0 or parsed > 0xFFFFFFFF:
        return None
    return parsed


def reject(state: ValidationState, path: Path, reason: str) -> None:
    """Append one rejection record to the validation state.

    Inputs:
    - state: Current validation accumulator.
    - path: Profile path that failed validation.
    - reason: Stable text describing the failure.

    Processing:
    - Adds a small dictionary suitable for JSON manifest/report output.

    Return behavior:
    - No return value; state.rejected is updated in place.
    """
    state.rejected.append({"path": str(path), "reason": reason})


def validate_profile(path: Path, state: ValidationState) -> ProfileRecord | None:
    """Validate one JSON profile and return a publishable record.

    Inputs:
    - path: Candidate JSON file.
    - state: Validation accumulator used to record rejection reasons.

    Processing:
    - Checks root keys, schema version, module identity, field names, and offsets.
    - Mirrors the R3/R0 v1 apply constraints so bad files never enter release.

    Return behavior:
    - Returns ProfileRecord on success; returns None and records a reason on failure.
    """
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - diagnostics must preserve any parser failure.
        reject(state, path, f"invalid_json: {exc}")
        return None

    if not isinstance(data, dict):
        reject(state, path, "root_not_object")
        return None
    if data.get("schemaVersion") != KSW_SCHEMA_VERSION:
        reject(state, path, f"schema_version_not_{KSW_SCHEMA_VERSION}")
        return None

    module = data.get("module")
    fields = data.get("fields")
    if not isinstance(module, dict):
        reject(state, path, "module_not_object")
        return None
    if not isinstance(fields, dict) or not fields:
        reject(state, path, "fields_empty_or_not_object")
        return None

    module_class = str(module.get("class", "")).strip().lower()
    if module_class not in {"ntoskrnl", "ntkrla57"}:
        reject(state, path, f"unsupported_module_class:{module_class}")
        return None

    machine = parse_uint32(module.get("machine"))
    time_date_stamp = parse_uint32(module.get("timeDateStamp"))
    size_of_image = parse_uint32(module.get("sizeOfImage"))
    if machine is None or time_date_stamp is None or size_of_image is None:
        reject(state, path, "module_identity_invalid")
        return None

    for field_name, offset_value in fields.items():
        if field_name not in KNOWN_FIELD_NAMES:
            reject(state, path, f"unknown_field:{field_name}")
            return None
        offset = parse_uint32(offset_value)
        if offset is None or offset == 0xFFFFFFFF or offset > KSW_DYN_PROFILE_OFFSET_MAX:
            reject(state, path, f"offset_invalid:{field_name}")
            return None

    identity = ProfileIdentity(module_class, machine, time_date_stamp, size_of_image)
    return ProfileRecord(path=path, data=data, identity=identity, field_count=len(fields))


def choose_duplicate_winner(records: list[ProfileRecord]) -> ProfileRecord:
    """Choose a deterministic winner for one duplicate identity group.

    Inputs:
    - records: Profiles with the same class/machine/timestamp/size tuple.

    Processing:
    - Prefers the record with more fields.
    - Uses profileName and path as stable tie-breakers to avoid nondeterminism.

    Return behavior:
    - Returns the selected ProfileRecord; does not mutate input records.
    """
    return sorted(records, key=lambda item: (-item.field_count, item.profile_name, str(item.path)))[0]


def deduplicate_profiles(state: ValidationState) -> list[ProfileRecord]:
    """Deduplicate accepted profiles by R0-safe module identity.

    Inputs:
    - state: ValidationState containing schema-valid profile records.

    Processing:
    - Groups by ProfileIdentity and chooses one deterministic winner per group.
    - Records duplicate diagnostics for the manifest.

    Return behavior:
    - Returns the final publish list sorted by class and profileName.
    """
    groups: dict[ProfileIdentity, list[ProfileRecord]] = {}
    for record in state.accepted:
        groups.setdefault(record.identity, []).append(record)

    final_records: list[ProfileRecord] = []
    for identity, records in groups.items():
        if len(records) == 1:
            final_records.append(records[0])
            continue
        winner = choose_duplicate_winner(records)
        state.duplicate_groups.append(
            {
                "identity": identity.__dict__,
                "winner": str(winner.path),
                "duplicates": [str(item.path) for item in records],
            }
        )
        final_records.append(winner)

    return sorted(final_records, key=lambda item: (item.identity.module_class, item.profile_name, str(item.path)))


def parse_pe_identity(path: Path) -> ProfileIdentity | None:
    """Parse minimal PE identity for a local ntoskrnl.exe smoke test.

    Inputs:
    - path: PE image path, usually C:\\Windows\\System32\\ntoskrnl.exe.

    Processing:
    - Reads DOS header, NT signature, COFF header, and OptionalHeader SizeOfImage.
    - Does not need pefile or symbol data.

    Return behavior:
    - Returns ProfileIdentity for ntoskrnl on success; returns None on failure.
    """
    try:
        data = path.read_bytes()
        if len(data) < 0x100 or data[0:2] != b"MZ":
            return None
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_offset + 0x108 >= len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
            return None
        machine, _sections, time_date_stamp = struct.unpack_from("<HHI", data, pe_offset + 4)
        optional_header_offset = pe_offset + 24
        size_of_image = struct.unpack_from("<I", data, optional_header_offset + 56)[0]
        return ProfileIdentity("ntoskrnl", machine, time_date_stamp, size_of_image)
    except OSError:
        return None


def ensure_safe_target(target_dir: Path) -> None:
    """Validate that a clean operation is limited to the profile target folder.

    Inputs:
    - target_dir: Directory intended to hold runtime JSON profiles.

    Processing:
    - Resolves the absolute path and checks the expected profiles\\ark_dyndata
      suffix before deleting stale JSON files.

    Return behavior:
    - Raises ValueError if the path is not safe; otherwise returns None.
    """
    resolved = target_dir.resolve()
    if resolved.name.lower() != "ark_dyndata" or resolved.parent.name.lower() != "profiles":
        raise ValueError(f"Refusing to clean unexpected target directory: {resolved}")


def copy_profiles(records: list[ProfileRecord], target_dir: Path, clean_target: bool, max_copy: int) -> list[Path]:
    """Copy accepted profile JSON files into the release profile directory.

    Inputs:
    - records: Final deduplicated publish list.
    - target_dir: Release\\profiles\\ark_dyndata destination.
    - clean_target: Whether to remove stale target JSON files first.
    - max_copy: Optional copy limit used for small smoke tests.

    Processing:
    - Creates the target directory, optionally cleans stale JSON, and copies files
      while preserving source file names.

    Return behavior:
    - Returns the list of destination paths copied or refreshed.
    """
    target_dir.mkdir(parents=True, exist_ok=True)
    if clean_target:
        ensure_safe_target(target_dir)
        for stale_file in target_dir.glob("*.json"):
            stale_file.unlink()

    selected = records[:max_copy] if max_copy > 0 else records
    copied: list[Path] = []
    for record in selected:
        destination = target_dir / record.path.name
        shutil.copy2(record.path, destination)
        copied.append(destination)
    return copied


def build_manifest(state: ValidationState, records: list[ProfileRecord], copied: list[Path], local_identity: ProfileIdentity | None) -> dict[str, Any]:
    """Build a manifest/report dictionary for the publish pass.

    Inputs:
    - state: Validation details including rejected and duplicate records.
    - records: Final deduplicated publish list.
    - copied: Destination files copied during this run.
    - local_identity: Optional local ntoskrnl identity.

    Processing:
    - Computes count summaries, class coverage, and local profile match status.

    Return behavior:
    - Returns a JSON-serializable dictionary.
    """
    class_counts: dict[str, int] = {}
    local_match: dict[str, Any] | None = None
    for record in records:
        class_counts[record.identity.module_class] = class_counts.get(record.identity.module_class, 0) + 1
        if local_identity is not None and record.identity == local_identity:
            local_match = {"profile": str(record.path), "profileName": record.profile_name}

    return {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "schemaVersion": KSW_SCHEMA_VERSION,
        "acceptedBeforeDedup": len(state.accepted),
        "publishedProfiles": len(records),
        "copiedProfiles": len(copied),
        "rejectedProfiles": len(state.rejected),
        "duplicateGroups": len(state.duplicate_groups),
        "classCounts": class_counts,
        "localKernelIdentity": local_identity.__dict__ if local_identity is not None else None,
        "localKernelProfileMatch": local_match,
        "rejected": state.rejected,
        "duplicates": state.duplicate_groups,
        "profiles": [
            {
                "fileName": record.path.name,
                "profileName": record.profile_name,
                "identity": record.identity.__dict__,
                "fieldCount": record.field_count,
            }
            for record in records
        ],
    }


def write_json(path: Path, data: dict[str, Any]) -> None:
    """Write a JSON file with stable indentation and UTF-8 encoding."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    """Program entry point; returns a process exit code."""
    args = parse_args(argv)
    source_dir = Path(args.source)
    release_root = Path(args.release_root)
    target_dir = release_root / "profiles" / "ark_dyndata"
    manifest_path = Path(args.manifest) if args.manifest else release_root / "profiles" / "ark_dyndata_manifest.json"
    report_path = Path(args.report)

    if not source_dir.exists():
        print(f"source directory does not exist: {source_dir}", file=sys.stderr)
        return 2

    state = ValidationState()
    for path in sorted(source_dir.glob("*.json")):
        record = validate_profile(path, state)
        if record is not None:
            state.accepted.append(record)

    final_records = deduplicate_profiles(state)
    local_identity = parse_pe_identity(Path(args.local_kernel)) if args.local_kernel else None
    if not final_records:
        print("no publishable profiles remain after validation", file=sys.stderr)
        return 3

    copied: list[Path] = []
    if not args.dry_run:
        copied = copy_profiles(final_records, target_dir, args.clean_target, args.max_copy)

    manifest = build_manifest(state, final_records, copied, local_identity)
    write_json(report_path, manifest)
    if not args.dry_run:
        write_json(manifest_path, manifest)

    print(f"sourceProfiles={len(list(source_dir.glob('*.json')))}")
    print(f"acceptedBeforeDedup={len(state.accepted)}")
    print(f"publishedProfiles={len(final_records)}")
    print(f"rejectedProfiles={len(state.rejected)}")
    print(f"duplicateGroups={len(state.duplicate_groups)}")
    print(f"copiedProfiles={len(copied)}")
    print(f"targetDir={target_dir}")
    print(f"manifest={manifest_path if not args.dry_run else '<dry-run>'}")
    print(f"report={report_path}")
    print(f"localKernelMatch={'yes' if manifest['localKernelProfileMatch'] else 'no'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
