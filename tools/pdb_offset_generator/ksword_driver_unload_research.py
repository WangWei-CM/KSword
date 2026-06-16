#!/usr/bin/env python3
"""
Build an offline driver-unload research report from Ksword kernel PDB corpus.

Inputs:
- Existing KswordARK DynData JSON profiles, normally D:\\PDB\\profiles\\ark_dyndata.
- Optional matching PE/PDB corpus, normally D:\\PDB\\pe-store and D:\\PDB\\pdb-cache.
- Optional llvm-pdbutil path for direct PDB deep-dive parsing.

Processing:
- Reads scattered profile JSON without modifying the corpus or Release tree.
- Builds a per-Windows-version coverage matrix for DriverObject, KLDR, callback,
  and kernel-global evidence that affects safe unload preflight.
- Optionally parses selected PDBs to resolve additional structure offsets and
  global RVAs that are not present in the existing profile JSON corpus.
- Adds a static unload-path risk model for DriverUnload-only, callback cleanup,
  dispatch/unload-pointer clearing, DeviceObject deletion, and temporary object
  cleanup.

Returns:
- Process exit code 0 when JSON and optional Markdown reports are written.
- Non-zero exit code when the profile root is missing or no profiles are read.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

sys.dont_write_bytecode = True

import ksword_pdb_profile_generator as pdbgen  # noqa: E402


DEFAULT_PROFILE_ROOT = Path(r"D:\PDB\profiles\ark_dyndata")
DEFAULT_PE_ROOT = Path(r"D:\PDB\pe-store")
DEFAULT_PDB_ROOT = Path(r"D:\PDB\pdb-cache")
DEFAULT_LLVM_PDBUTIL = Path(r"D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe")
DEFAULT_REPORT = Path(r"D:\PDB\logs\driver_unload_research_report.json")
DEFAULT_MARKDOWN = Path(r"D:\PDB\logs\driver_unload_research_report.md")

DRIVER_OBJECT_FIELDS = (
    "DoDriverStart",
    "DoDriverSize",
    "DoDriverSection",
    "DoMajorFunction",
    "DoFastIoDispatch",
    "DoDriverUnload",
)

KLDR_FIELDS = (
    "KldrInLoadOrderLinks",
    "KldrDllBase",
    "KldrSizeOfImage",
    "KldrFullDllName",
    "KldrBaseDllName",
    "KldrFlags",
)

KERNEL_GLOBALS = (
    "PspCidTable",
    "PsLoadedModuleList",
    "MmUnloadedDrivers",
    "PiDDBCacheTable",
)

CALLBACK_GLOBALS = (
    "PspCreateProcessNotifyRoutine",
    "PspCreateThreadNotifyRoutine",
    "PspLoadImageNotifyRoutine",
    "PspNotifyEnableMask",
    "CmCallbackListHead",
)

CALLBACK_STRUCT_FIELDS = (
    "_OBJECT_TYPE.CallbackList",
    "_CALLBACK_ENTRY_ITEM.EntryList",
    "_CALLBACK_ENTRY_ITEM.EntryItemList",
    "_CALLBACK_ENTRY_ITEM.PreOperation",
    "_CALLBACK_ENTRY_ITEM.PostOperation",
    "_CALLBACK_ENTRY_ITEM.Operations",
    "_CALLBACK_ENTRY_ITEM.CallbackEntry",
    "_CALLBACK_ENTRY.Altitude",
    "_CALLBACK_ENTRY.RegistrationContext",
)

PROFILE_FIELD_GROUPS = {
    "driverObject": DRIVER_OBJECT_FIELDS,
    "kldr": KLDR_FIELDS,
    "kernelGlobals": KERNEL_GLOBALS,
    "callbackGlobals": CALLBACK_GLOBALS,
    "callbackStructs": CALLBACK_STRUCT_FIELDS,
}

PDB_STRUCT_MEMBER_CANDIDATES = {
    "_DRIVER_OBJECT": (
        "Type",
        "Size",
        "DeviceObject",
        "Flags",
        "DriverStart",
        "DriverSize",
        "DriverSection",
        "DriverExtension",
        "DriverName",
        "FastIoDispatch",
        "DriverInit",
        "DriverStartIo",
        "DriverUnload",
        "MajorFunction",
    ),
    "_DEVICE_OBJECT": (
        "Type",
        "Size",
        "ReferenceCount",
        "DriverObject",
        "NextDevice",
        "AttachedDevice",
        "CurrentIrp",
        "Flags",
        "Characteristics",
        "DeviceExtension",
        "DeviceType",
        "StackSize",
        "AlignmentRequirement",
    ),
    "_KLDR_DATA_TABLE_ENTRY": (
        "InLoadOrderLinks",
        "DllBase",
        "EntryPoint",
        "SizeOfImage",
        "FullDllName",
        "BaseDllName",
        "Flags",
        "LoadCount",
        "SignatureLevel",
        "SignatureType",
        "SectionPointer",
        "CheckSum",
    ),
    "_OBJECT_HEADER": (
        "PointerCount",
        "HandleCount",
        "TypeIndex",
        "InfoMask",
        "Flags",
        "ObjectCreateInfo",
        "QuotaBlockCharged",
        "SecurityDescriptor",
        "Body",
    ),
}

PDB_GLOBAL_CANDIDATES = KERNEL_GLOBALS + CALLBACK_GLOBALS


@dataclass
class ProfileRecord:
    """One profile JSON normalized for unload research.

    Inputs:
    - path: Source JSON profile path.
    - data: Parsed profile dictionary.

    Processing:
    - Normalizes legacy fields, typedItems/items, and callbackItems into one
      flat item map keyed by Ksword field or symbol name.

    Return behavior:
    - The dataclass has no side effects; callers serialize selected attributes.
    """

    path: Path
    data: dict[str, Any]
    item_values: dict[str, str] = field(default_factory=dict)

    @property
    def module(self) -> dict[str, Any]:
        """Return the module metadata dictionary or an empty dictionary."""
        value = self.data.get("module")
        return value if isinstance(value, dict) else {}

    @property
    def profile_name(self) -> str:
        """Return a human-readable profile name for reports."""
        value = self.data.get("profileName")
        return str(value) if value else self.path.stem

    @property
    def version(self) -> str:
        """Return the Windows file version string stored in module metadata."""
        return str(self.module.get("version") or "unknown")

    @property
    def module_class(self) -> str:
        """Return the kernel module class stored in module metadata."""
        return str(self.module.get("class") or "unknown")


@dataclass
class VersionCoverage:
    """Aggregated coverage for one module class and Windows version.

    Inputs:
    - module_class/version: Stable grouping keys.

    Processing:
    - add_profile increments per-field presence counters and tracks observed
      offset/RVA values for drift analysis.

    Return behavior:
    - to_json returns a JSON-ready summary and does not mutate the object.
    """

    module_class: str
    version: str
    profile_count: int = 0
    present: collections.Counter[str] = field(default_factory=collections.Counter)
    values: dict[str, set[str]] = field(default_factory=lambda: collections.defaultdict(set))
    readiness: collections.Counter[str] = field(default_factory=collections.Counter)

    def add_profile(self, record: ProfileRecord) -> None:
        """Add one profile to this version bucket.

        Inputs:
        - record: Normalized profile record.

        Processing:
        - Counts every known unload-relevant item that appears in the profile.
        - Tracks each observed value to detect version-specific offset drift.

        Return behavior:
        - No return value; mutates this aggregation bucket.
        """
        self.profile_count += 1
        for field_name in all_profile_field_names():
            if field_name in record.item_values:
                self.present[field_name] += 1
                self.values.setdefault(field_name, set()).add(record.item_values[field_name])
        self.readiness[classify_profile_readiness(record.item_values)] += 1

    def to_json(self) -> dict[str, Any]:
        """Return a compact JSON-ready coverage summary for this bucket."""
        field_coverage: dict[str, Any] = {}
        for group_name, names in PROFILE_FIELD_GROUPS.items():
            field_coverage[group_name] = {
                name: {
                    "presentProfiles": self.present.get(name, 0),
                    "coveragePercent": percent(self.present.get(name, 0), self.profile_count),
                    "values": sorted(self.values.get(name, set())),
                }
                for name in names
            }
        return {
            "moduleClass": self.module_class,
            "version": self.version,
            "profileCount": self.profile_count,
            "readiness": dict(sorted(self.readiness.items())),
            "fieldCoverage": field_coverage,
        }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments for the research report generator.

    Inputs:
    - argv: Optional argument list for tests; None uses sys.argv.

    Processing:
    - Keeps PDB parsing disabled by default because full corpus parsing is slow.

    Return behavior:
    - Returns argparse.Namespace with normalized option values.
    """
    parser = argparse.ArgumentParser(description="Generate Ksword driver-unload PDB research reports.")
    parser.add_argument("--profile-root", default=str(DEFAULT_PROFILE_ROOT), help="Directory containing scattered DynData JSON profiles.")
    parser.add_argument("--pe-root", default=str(DEFAULT_PE_ROOT), help="PE corpus root used only with --parse-pdb.")
    parser.add_argument("--pdb-root", default=str(DEFAULT_PDB_ROOT), help="PDB corpus root used only with --parse-pdb.")
    parser.add_argument("--llvm-pdbutil", default=str(DEFAULT_LLVM_PDBUTIL), help="llvm-pdbutil.exe path used only with --parse-pdb.")
    parser.add_argument("--output", default=str(DEFAULT_REPORT), help="JSON report path.")
    parser.add_argument("--markdown", default=str(DEFAULT_MARKDOWN), help="Markdown report path; empty disables Markdown output.")
    parser.add_argument("--parse-pdb", action="store_true", help="Directly parse selected PDBs with llvm-pdbutil.")
    parser.add_argument("--max-pdb", type=int, default=0, help="Maximum PDB profiles to deep-parse; 0 means all selected profiles.")
    parser.add_argument("--version-filter", action="append", default=[], help="Substring filter for module.version; can be repeated.")
    parser.add_argument("--module-class", action="append", default=[], help="Restrict deep PDB parsing to module class names, for example ntoskrnl.")
    parser.add_argument("--include-profile-list", action="store_true", help="Include every normalized profile in the JSON report.")
    return parser.parse_args(argv)


def all_profile_field_names() -> tuple[str, ...]:
    """Return the flattened unload-relevant profile item names.

    Inputs:
    - None.

    Processing:
    - Flattens PROFILE_FIELD_GROUPS in deterministic group order.

    Return behavior:
    - Returns a tuple with duplicates removed while preserving order.
    """
    ordered: list[str] = []
    seen: set[str] = set()
    for names in PROFILE_FIELD_GROUPS.values():
        for name in names:
            if name not in seen:
                ordered.append(name)
                seen.add(name)
    return tuple(ordered)


def percent(value: int, total: int) -> float:
    """Return a rounded percentage for report output."""
    return 0.0 if total <= 0 else round((float(value) / float(total)) * 100.0, 1)


def normalize_int_text(value: Any) -> str:
    """Normalize JSON numeric or hex text values for stable reports.

    Inputs:
    - value: Profile value from fields/items arrays.

    Processing:
    - Accepts integers and strings. Hex strings are converted to uppercase.

    Return behavior:
    - Returns a string; malformed values are still preserved as text.
    """
    if isinstance(value, bool):
        return str(value)
    if isinstance(value, int):
        return f"0x{value:X}"
    text = str(value).strip()
    try:
        return f"0x{int(text, 16 if text.lower().startswith('0x') else 10):X}"
    except ValueError:
        return text


def collect_items(data: dict[str, Any]) -> dict[str, str]:
    """Collect legacy and typed profile items into one name-to-value map.

    Inputs:
    - data: Parsed profile root.

    Processing:
    - Reads legacy fields dict, v3 typedItems/items, and v2 callbackItems.
    - Later entries for the same name override earlier ones so typed payloads
      can correct legacy spelling when both are present.

    Return behavior:
    - Returns a normalized dictionary. The input dictionary is not mutated.
    """
    items: dict[str, str] = {}
    fields = data.get("fields")
    if isinstance(fields, dict):
        for name, value in fields.items():
            items[str(name)] = normalize_int_text(value)
    for key in ("typedItems", "items", "callbackItems"):
        typed_value = data.get(key)
        if not isinstance(typed_value, list):
            continue
        for entry in typed_value:
            if not isinstance(entry, dict) or "name" not in entry or "value" not in entry:
                continue
            items[str(entry["name"])] = normalize_int_text(entry["value"])
    if "_CALLBACK_ENTRY_ITEM.EntryItemList" in items and "_CALLBACK_ENTRY_ITEM.EntryList" not in items:
        items["_CALLBACK_ENTRY_ITEM.EntryList"] = items["_CALLBACK_ENTRY_ITEM.EntryItemList"]
    return items


def read_profiles(profile_root: Path) -> list[ProfileRecord]:
    """Read every profile JSON under profile_root.

    Inputs:
    - profile_root: Directory containing scattered profile JSON files.

    Processing:
    - Skips malformed JSON but records no output side effects.

    Return behavior:
    - Returns a sorted list of ProfileRecord objects.
    """
    records: list[ProfileRecord] = []
    for path in sorted(profile_root.rglob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001 - malformed research inputs are skipped.
            continue
        if not isinstance(data, dict):
            continue
        records.append(ProfileRecord(path=path, data=data, item_values=collect_items(data)))
    return records


def classify_profile_readiness(items: dict[str, str]) -> str:
    """Classify unload-preflight readiness from existing profile data.

    Inputs:
    - items: Normalized profile item map.

    Processing:
    - Requires DriverObject fields for basic inspection.
    - Requires KLDR fields plus PsLoadedModuleList for loader alignment.

    Return behavior:
    - Returns a stable string used in JSON and Markdown reports.
    """
    driver_required = {"DoDriverStart", "DoDriverSize", "DoDriverSection", "DoMajorFunction", "DoDriverUnload"}
    kldr_required = {"KldrInLoadOrderLinks", "KldrDllBase", "KldrSizeOfImage", "KldrFullDllName", "KldrBaseDllName"}
    has_driver = driver_required.issubset(items)
    has_loader = kldr_required.issubset(items) and "PsLoadedModuleList" in items
    if has_driver and has_loader:
        return "full_loader_alignment"
    if has_driver:
        return "driver_object_only"
    return "profile_gap"


def filter_records_for_pdb_parse(records: list[ProfileRecord], args: argparse.Namespace) -> list[ProfileRecord]:
    """Return records selected for optional direct PDB parsing.

    Inputs:
    - records: All normalized profile records.
    - args: Parsed command-line options containing class/version/limit filters.

    Processing:
    - Applies cheap metadata filters before expensive llvm-pdbutil calls.
    - Keeps deterministic order by module class, version, and profile name.

    Return behavior:
    - Returns a new list; input records are not mutated.
    """
    module_filters = {str(item).lower() for item in args.module_class}
    version_filters = [str(item) for item in args.version_filter]
    selected: list[ProfileRecord] = []
    for record in sorted(records, key=lambda item: (item.module_class, item.version, item.profile_name)):
        if module_filters and record.module_class.lower() not in module_filters:
            continue
        if version_filters and not any(fragment in record.version for fragment in version_filters):
            continue
        selected.append(record)
    if args.max_pdb and args.max_pdb > 0:
        return selected[: args.max_pdb]
    return selected


def profile_pdb_path(record: ProfileRecord, pdb_root: Path) -> Path | None:
    """Resolve the local PDB path for one profile.

    Inputs:
    - record: Profile metadata containing arch, pdbName, and pdbSymbolKey.
    - pdb_root: Corpus root, normally D:\\PDB\\pdb-cache.

    Processing:
    - Mirrors the path convention used by ksword_pdb_profile_generator.

    Return behavior:
    - Returns the path when metadata is complete; otherwise returns None.
    """
    module = record.module
    arch = str(module.get("arch") or "")
    pdb_name = str(module.get("pdbName") or "")
    symbol_key = str(module.get("pdbSymbolKey") or "")
    if not arch or not pdb_name or not symbol_key:
        return None
    return pdb_root / arch / pdb_name / symbol_key / pdb_name


def profile_pe_path(record: ProfileRecord, pe_root: Path) -> Path | None:
    """Resolve the local PE path for one profile.

    Inputs:
    - record: Profile metadata containing arch, file, version, and sha256.
    - pe_root: Corpus root, normally D:\\PDB\\pe-store.

    Processing:
    - Mirrors the path convention used by ksword_pdb_profile_generator.

    Return behavior:
    - Returns the path when metadata is complete; otherwise returns None.
    """
    module = record.module
    arch = str(module.get("arch") or "")
    file_name = str(module.get("file") or "")
    version = str(module.get("version") or "")
    sha256_text = str(module.get("sha256") or "")
    if not arch or not file_name or not version or not sha256_text:
        return None
    return pe_root / arch / f"{file_name}.{version}" / sha256_text / file_name


def parse_pdb_record(record: ProfileRecord, pe_root: Path, pdb_root: Path, pdbutil_path: Path) -> dict[str, Any]:
    """Directly parse one local PE/PDB pair for unload-relevant evidence.

    Inputs:
    - record: Profile whose module metadata points to a local PE/PDB pair.
    - pe_root/pdb_root: Corpus roots.
    - pdbutil_path: llvm-pdbutil executable.

    Processing:
    - Runs llvm-pdbutil -types and resolves selected structure member offsets.
    - Runs -globals/-publics through the existing generator helpers and maps
      selected global symbols to PE RVAs.

    Return behavior:
    - Returns a JSON-ready dictionary. Tool failures are represented in the
      dictionary instead of raising to the whole batch.
    """
    pe_path = profile_pe_path(record, pe_root)
    pdb_path = profile_pdb_path(record, pdb_root)
    result: dict[str, Any] = {
        "profileName": record.profile_name,
        "moduleClass": record.module_class,
        "version": record.version,
        "pePath": str(pe_path) if pe_path else "",
        "pdbPath": str(pdb_path) if pdb_path else "",
        "status": "unknown",
        "structures": {},
        "globals": {},
        "missingStructures": {},
        "missingGlobals": [],
    }
    if pe_path is None or pdb_path is None:
        result["status"] = "metadata_missing"
        return result
    if not pe_path.exists():
        result["status"] = "pe_missing"
        return result
    if not pdb_path.exists():
        result["status"] = "pdb_missing"
        return result
    if not pdbutil_path.exists():
        result["status"] = "llvm_pdbutil_missing"
        return result

    try:
        types_text = pdbgen.run_llvm_pdbutil_types(pdb_path, str(pdbutil_path))
    except Exception as exc:  # noqa: BLE001 - keep batch running and report failure.
        result["status"] = "type_parse_failed"
        result["error"] = str(exc)
        return result

    structures: dict[str, dict[str, str]] = {}
    missing_structures: dict[str, list[str]] = {}
    for struct_name, members in PDB_STRUCT_MEMBER_CANDIDATES.items():
        resolved_members: dict[str, str] = {}
        missing_members: list[str] = []
        for member_name in members:
            offset = pdbgen.resolve_member_offset(types_text, struct_name, member_name)
            if offset is None:
                missing_members.append(member_name)
                continue
            resolved_members[member_name] = f"0x{offset:04X}"
        structures[struct_name] = resolved_members
        if missing_members:
            missing_structures[struct_name] = missing_members

    try:
        symbol_addresses, symbol_dump_failures = pdbgen.collect_symbol_addresses(pdb_path, str(pdbutil_path))
    except Exception as exc:  # noqa: BLE001 - symbol parsing is optional diagnostics.
        symbol_addresses = {}
        symbol_dump_failures = [{"source": "combined", "error": str(exc)}]

    global_items, missing_globals = pdbgen.build_global_rva_items(
        pe_path=pe_path,
        symbol_addresses=symbol_addresses,
        symbol_names=PDB_GLOBAL_CANDIDATES,
    )
    result["status"] = "ok"
    result["structures"] = structures
    result["missingStructures"] = missing_structures
    result["globals"] = {str(item.get("name")): normalize_int_text(item.get("value")) for item in global_items}
    result["missingGlobals"] = [str(item.get("name")) for item in missing_globals if item.get("name")]
    if symbol_dump_failures:
        result["symbolDumpFailures"] = symbol_dump_failures
    return result


def build_coverage(records: list[ProfileRecord]) -> list[dict[str, Any]]:
    """Build per-module-version profile coverage entries.

    Inputs:
    - records: Normalized profiles.

    Processing:
    - Groups by module class and version, then counts unload-relevant items.

    Return behavior:
    - Returns a sorted JSON-ready list.
    """
    buckets: dict[tuple[str, str], VersionCoverage] = {}
    for record in records:
        key = (record.module_class, record.version)
        if key not in buckets:
            buckets[key] = VersionCoverage(module_class=record.module_class, version=record.version)
        buckets[key].add_profile(record)
    return [buckets[key].to_json() for key in sorted(buckets)]


def summarize_missing(records: list[ProfileRecord]) -> list[dict[str, Any]]:
    """Summarize missing unload-relevant fields across all profiles.

    Inputs:
    - records: Normalized profiles.

    Processing:
    - Counts absent fields from the fixed unload-research candidate list.

    Return behavior:
    - Returns sorted JSON-ready rows, highest missing count first.
    """
    rows: list[dict[str, Any]] = []
    total = len(records)
    for group_name, names in PROFILE_FIELD_GROUPS.items():
        for name in names:
            present_count = sum(1 for record in records if name in record.item_values)
            missing_count = total - present_count
            rows.append(
                {
                    "group": group_name,
                    "name": name,
                    "presentProfiles": present_count,
                    "missingProfiles": missing_count,
                    "coveragePercent": percent(present_count, total),
                }
            )
    return sorted(rows, key=lambda item: (-int(item["missingProfiles"]), str(item["group"]), str(item["name"])))


def build_unload_path_model() -> list[dict[str, Any]]:
    """Return the static unload-path risk model for current R0 logic.

    Inputs:
    - None.

    Processing:
    - Encodes current behavior observed in driver_unload.c without executing it.

    Return behavior:
    - Returns JSON-ready risk model rows.
    """
    return [
        {
            "path": "DriverUnloadOnly",
            "currentAction": "Default path calls ZwUnloadDriver with only the service registry path and without holding a DriverObject reference; direct DriverUnload is now only a force fallback after ZwUnloadDriver fails and preflight allows destructive cleanup.",
            "requiredPreflight": [
                "Service registry path can be derived from DriverExtension->ServiceKeyName or the resolved DriverObject name.",
                "Core/self module guards pass before any unload attempt.",
                "DriverObject->DriverStart/DriverSize/DriverSection align with loader module evidence before direct fallback or destructive cleanup.",
            ],
            "rollbackSafety": "No persistent Ksword-side mutation before ZwUnloadDriver; safest current path.",
            "knownFailureModes": [
                "ZwUnloadDriver can fail when the target has external references, no unload routine, active devices, or service metadata mismatch.",
                "Direct fallback can still hang or leave target-managed state incomplete when used in force mode.",
                "Target has callbacks or worker state that neither ZwUnloadDriver nor direct fallback can safely unregister.",
            ],
            "recommendation": "Prefer ZwUnloadDriver; only use direct fallback in VM/confirmed force mode after DynData-backed preflight passes.",
        },
        {
            "path": "CallbackCleanupByModuleBase",
            "currentAction": "After successful unload status, enumerate known callback classes and remove callbacks whose function address belongs to target module base.",
            "requiredPreflight": [
                "Target module base and size are trusted from loader evidence.",
                "Callback entries are removable through documented Ps/Flt APIs or existing safe wrappers.",
                "Each candidate address resolves inside the same module image.",
            ],
            "rollbackSafety": "Partial removal cannot be reconstructed without registration handles; failures must be treated as high risk.",
            "knownFailureModes": [
                "Removing callbacks after DriverUnload can race with target's own cleanup semantics.",
                "Callback address ownership can be ambiguous if loader evidence is stale.",
                "Unsupported callback classes remain registered and still point into target image.",
            ],
            "recommendation": "Move to explicit preflight/report-first mode; avoid removal unless destructive cleanup is separately confirmed.",
        },
        {
            "path": "ClearDispatchOrUnloadPointer",
            "currentAction": "Overwrite FastIoDispatch, MajorFunction entries, or DriverUnload pointer when ALLOW_DESTRUCTIVE_CLEANUP is present.",
            "requiredPreflight": [
                "DriverObject is not expected to go through a later normal SCM unload.",
                "All DeviceObject users are quiesced or intentionally blocked.",
                "Operator accepts that original pointers are not recoverable from current response data.",
            ],
            "rollbackSafety": "Poor; current response does not persist a complete before snapshot for rollback.",
            "knownFailureModes": [
                "Later normal unload sees mutated dispatch or missing DriverUnload and bugchecks.",
                "In-flight IRPs can hit the replacement reject stub while target still expects ownership.",
                "FastIo/dispatch ownership mismatch can hide the real unload blocker.",
            ],
            "recommendation": "Default-deny for normal workflows; require mutation transaction snapshot before any future implementation enables it.",
        },
        {
            "path": "DeleteDeviceObjects",
            "currentAction": "Snapshot DriverObject->DeviceObject/NextDevice chain, validate ownership, then call IoDeleteDevice on each entry.",
            "requiredPreflight": [
                "Device chain has no loop and no cross-driver entries.",
                "No attached devices depend on the target device objects.",
                "Target DriverUnload is absent or has already failed and operator accepts irreversible cleanup.",
            ],
            "rollbackSafety": "None; deleted DeviceObjects cannot be reconstructed safely.",
            "knownFailureModes": [
                "Deleting devices without target private cleanup leaks extensions and external state.",
                "Attached filter stacks can retain stale pointers.",
                "Target's later DriverUnload may double-delete or traverse freed DeviceObjects.",
            ],
            "recommendation": "Treat as last-resort destructive action; current blue-screen report strongly argues for disabling outside isolated VM tests.",
        },
        {
            "path": "MakeTemporaryObject",
            "currentAction": "Call ObMakeTemporaryObject so the DriverObject can be removed when references reach zero.",
            "requiredPreflight": [
                "Object namespace name is no longer needed for a later controlled unload.",
                "Reference owners are understood or observable through debugger validation.",
                "DriverObject is not a system-critical or boot-start driver.",
            ],
            "rollbackSafety": "Poor; object permanence/name state is changed without restoring the full object manager state.",
            "knownFailureModes": [
                "Name removal changes later SCM or object lookup behavior.",
                "Outstanding references keep the object alive but now in a surprising state.",
                "Combining with dispatch/device mutation creates unrecoverable half-cleanup.",
            ],
            "recommendation": "Do not use in product path until dynamic WinDbg validation proves reference lifecycle behavior for each supported OS build.",
        },
    ]


def build_dynamic_validation_plan() -> dict[str, Any]:
    """Return a WinDbg/KD validation checklist for later VM testing.

    Inputs:
    - None.

    Processing:
    - Encodes the dynamic checks needed to validate the offline model.

    Return behavior:
    - Returns JSON-ready checklist data.
    """
    return {
        "tools": [
            "WinDbg or KD from Windows Kits Debuggers x64",
            "symchk for symbol verification",
            "VM snapshot with KDNET preferred",
            "Complete kernel dump or active crash dump for bugcheck cases",
        ],
        "preUnloadBreakpoints": [
            "Break on target DriverUnload when symbol or address is known.",
            "Capture !drvobj <name> 7 and !devobj for every DeviceObject.",
            "Capture loader entry from PsLoadedModuleList when DynData provides offsets.",
            "Capture callback registrations that point into target image.",
        ],
        "postUnloadChecks": [
            "DriverObject->DeviceObject is NULL or unchanged with STATUS_DEVICE_BUSY reported.",
            "DriverSection/KLDR entry no longer appears only after legitimate unload completion.",
            "No callback entry retains a function pointer into an unloaded or half-cleaned image.",
            "Dispatch/FastIo/DriverUnload pointers are not mutated unless a transaction snapshot exists.",
        ],
        "bugcheckEvidence": [
            "Bugcheck code and parameters.",
            "Faulting stack and instruction pointer ownership.",
            "Target DriverObject, DeviceObject chain, DriverSection, and callback state from dump.",
        ],
    }


def build_recommendations(records: list[ProfileRecord], missing_rows: list[dict[str, Any]]) -> list[str]:
    """Build high-level recommendations from corpus coverage.

    Inputs:
    - records: All normalized profiles.
    - missing_rows: Missing-field summary rows.

    Processing:
    - Uses existing profile coverage to decide whether current corpus can
      support unload preflight or needs profile regeneration.

    Return behavior:
    - Returns an ordered list of concise recommendation strings.
    """
    total = len(records)
    driver_ready = sum(1 for record in records if classify_profile_readiness(record.item_values) != "profile_gap")
    global_missing = [row for row in missing_rows if row["group"] in {"kernelGlobals", "callbackGlobals"} and row["presentProfiles"] == 0]
    recommendations = [
        "Keep R0/R3 unload behavior frozen until this report is reviewed against the target driver and OS build.",
        "Use DriverUnload-only as the default experimental path; do not run dispatch clearing, DeviceObject deletion, or ObMakeTemporaryObject on a non-VM target.",
        "Add a read-only unload preflight before any future destructive cleanup: DriverObject fields, loader alignment, DeviceObject chain, callback ownership, and service metadata.",
    ]
    if total > 0 and driver_ready == 0:
        recommendations.append("Existing scattered profiles do not contain DriverObject/KLDR v3 fields; regenerate selected PDB profiles with the current generator before relying on automated preflight.")
    if global_missing:
        recommendations.append("Kernel/callback global RVAs are mostly absent from existing profiles; run this tool with --parse-pdb on representative builds to prove symbol availability before extending DynData.")
    recommendations.append("For the reported blue-screen path, collect a kernel dump and compare DriverObject/DeviceObject state before and after the failed cleanup; logs alone are insufficient.")
    return recommendations


def build_report(records: list[ProfileRecord], args: argparse.Namespace) -> dict[str, Any]:
    """Build the complete JSON research report.

    Inputs:
    - records: Normalized profile corpus.
    - args: Parsed command-line options.

    Processing:
    - Creates coverage, missing-field, unload-risk, optional direct-PDB, and
      dynamic-validation sections.

    Return behavior:
    - Returns a JSON-ready dictionary.
    """
    missing_rows = summarize_missing(records)
    report: dict[str, Any] = {
        "schemaVersion": 1,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "inputs": {
            "profileRoot": str(Path(args.profile_root)),
            "peRoot": str(Path(args.pe_root)),
            "pdbRoot": str(Path(args.pdb_root)),
            "llvmPdbutil": str(Path(args.llvm_pdbutil)),
            "parsePdb": bool(args.parse_pdb),
            "maxPdb": int(args.max_pdb),
            "versionFilter": list(args.version_filter),
            "moduleClassFilter": list(args.module_class),
        },
        "summary": {
            "profileCount": len(records),
            "moduleClassCounts": dict(sorted(collections.Counter(record.module_class for record in records).items())),
            "versionCount": len({(record.module_class, record.version) for record in records}),
            "readinessCounts": dict(sorted(collections.Counter(classify_profile_readiness(record.item_values) for record in records).items())),
        },
        "coverageByVersion": build_coverage(records),
        "missingSummary": missing_rows,
        "unloadPathModel": build_unload_path_model(),
        "dynamicValidationPlan": build_dynamic_validation_plan(),
        "recommendations": build_recommendations(records, missing_rows),
    }
    if args.include_profile_list:
        report["profiles"] = [
            {
                "profileName": record.profile_name,
                "moduleClass": record.module_class,
                "version": record.version,
                "path": str(record.path),
                "readiness": classify_profile_readiness(record.item_values),
                "items": dict(sorted(record.item_values.items())),
            }
            for record in records
        ]
    if args.parse_pdb:
        selected = filter_records_for_pdb_parse(records, args)
        parsed: list[dict[str, Any]] = []
        for index, record in enumerate(selected, start=1):
            print(f"[PDB {index}/{len(selected)}] {record.profile_name}")
            parsed.append(
                parse_pdb_record(
                    record=record,
                    pe_root=Path(args.pe_root),
                    pdb_root=Path(args.pdb_root),
                    pdbutil_path=Path(args.llvm_pdbutil),
                )
            )
        report["pdbDeepDive"] = {
            "selectedProfiles": len(selected),
            "parsedProfiles": parsed,
            "statusCounts": dict(sorted(collections.Counter(item.get("status", "unknown") for item in parsed).items())),
        }
    return report


def write_json(path: Path, data: dict[str, Any]) -> None:
    """Write a JSON file with stable formatting.

    Inputs:
    - path: Destination path.
    - data: JSON-ready dictionary.

    Processing:
    - Creates parent directories as needed and writes UTF-8 text.

    Return behavior:
    - No return value.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")


def top_missing_rows(report: dict[str, Any], limit: int = 16) -> list[dict[str, Any]]:
    """Return the top missing-summary rows for Markdown output."""
    rows = report.get("missingSummary", [])
    if not isinstance(rows, list):
        return []
    return [row for row in rows[:limit] if isinstance(row, dict)]


def write_markdown(path: Path, report: dict[str, Any]) -> None:
    """Write a human-readable Markdown summary.

    Inputs:
    - path: Destination path.
    - report: Complete JSON report dictionary.

    Processing:
    - Extracts summary, missing fields, recommendations, and risk model rows.

    Return behavior:
    - No return value.
    """
    summary = report.get("summary", {})
    lines: list[str] = [
        "# Driver Unload PDB Research Report",
        "",
        "## Summary",
        "",
        f"- Generated: `{report.get('generatedAt', '')}`",
        f"- Profiles: `{summary.get('profileCount', 0)}`",
        f"- Module classes: `{summary.get('moduleClassCounts', {})}`",
        f"- Version buckets: `{summary.get('versionCount', 0)}`",
        f"- Readiness: `{summary.get('readinessCounts', {})}`",
        "",
        "## Top Missing Evidence",
        "",
        "| Group | Name | Present | Missing | Coverage |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for row in top_missing_rows(report):
        lines.append(
            f"| {row.get('group', '')} | `{row.get('name', '')}` | "
            f"{row.get('presentProfiles', 0)} | {row.get('missingProfiles', 0)} | "
            f"{row.get('coveragePercent', 0.0)}% |"
        )
    lines.extend(["", "## Recommendations", ""])
    for recommendation in report.get("recommendations", []):
        lines.append(f"- {recommendation}")
    lines.extend(["", "## Unload Path Risk Model", ""])
    for row in report.get("unloadPathModel", []):
        if not isinstance(row, dict):
            continue
        lines.append(f"### {row.get('path', '')}")
        lines.append("")
        lines.append(f"- Current action: {row.get('currentAction', '')}")
        lines.append(f"- Rollback safety: {row.get('rollbackSafety', '')}")
        lines.append(f"- Recommendation: {row.get('recommendation', '')}")
        lines.append("- Known failure modes:")
        for failure in row.get("knownFailureModes", []):
            lines.append(f"  - {failure}")
        lines.append("")
    if "pdbDeepDive" in report:
        deep = report["pdbDeepDive"]
        lines.extend(
            [
                "## PDB Deep Dive",
                "",
                f"- Selected profiles: `{deep.get('selectedProfiles', 0)}`",
                f"- Status counts: `{deep.get('statusCounts', {})}`",
                "",
            ]
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    """Program entry point.

    Inputs:
    - argv: Optional command-line arguments.

    Processing:
    - Reads profiles, builds the research report, then writes JSON/Markdown.

    Return behavior:
    - Returns process-style exit code 0 on success, 1 on input/report failure.
    """
    args = parse_args(argv)
    profile_root = Path(args.profile_root)
    if not profile_root.exists():
        print(f"profile root not found: {profile_root}", file=sys.stderr)
        return 1
    records = read_profiles(profile_root)
    if not records:
        print(f"no profile JSON files read from {profile_root}", file=sys.stderr)
        return 1
    report = build_report(records, args)
    output_path = Path(args.output)
    write_json(output_path, report)
    markdown_path = Path(args.markdown) if args.markdown else None
    if markdown_path is not None:
        write_markdown(markdown_path, report)
    print(f"profiles={len(records)}")
    print(f"report={output_path}")
    if markdown_path is not None:
        print(f"markdown={markdown_path}")
    if args.parse_pdb:
        print(f"pdbDeepDiveStatus={report.get('pdbDeepDive', {}).get('statusCounts', {})}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
