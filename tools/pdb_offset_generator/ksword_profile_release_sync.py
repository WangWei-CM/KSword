#!/usr/bin/env python3
"""
Validate and publish KswordARK DynData profiles.

Inputs:
- A private corpus directory, normally D:\\PDB\\profiles\\ark_dyndata.
- A KswordARK release root, normally the directory containing Ksword5.1.exe.
- Optional local ntoskrnl.exe path for an identity match smoke test.

Processing:
- Parse every JSON profile and enforce the same schema assumptions used by the
  R3 KernelDock loader and R0 packed profile apply protocol.
- Reject profiles with missing module identity, no usable fields/items, unknown
  fields, or offsets outside the shared protocol range.
- Validate optional callbackItems and v3 typed items without requiring them;
  when present, items must use shared field names, allowed item kinds, and safe
  uint32 payload ranges.
- Deduplicate profiles by the R0 identity tuple used for safe apply:
  module class, machine, TimeDateStamp, and SizeOfImage.
- Copy only accepted profile JSON files into Release\\profiles\\ark_dyndata
  when scattered JSON publishing is requested.
- Optionally emit a compact Release\\profiles\\ark_dyndata_pack_v1.json,
  Release\\profiles\\ark_dyndata_pack_v2.json, or
  Release\\profiles\\ark_dyndata_pack_v3.json pack. v1 stores only fields for
  legacy compatibility. v2 adds callbackItems. v3 adds typed StructOffset and
  GlobalRva items plus coverage metadata.
- Write a manifest outside the scanned JSON directory so KernelDock does not
  attempt to parse the manifest as a scattered runtime profile.

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
KSW_PACK_VERSION_V1 = 1
KSW_PACK_VERSION_V2 = 2
KSW_PACK_VERSION_V3 = 3
KSW_DEFAULT_PACK_VERSION = KSW_PACK_VERSION_V1
KSW_SUPPORTED_PACK_VERSIONS = (KSW_PACK_VERSION_V1, KSW_PACK_VERSION_V2, KSW_PACK_VERSION_V3)
DEFAULT_PACK_FILE_NAMES = {
    KSW_PACK_VERSION_V1: "ark_dyndata_pack_v1.json",
    KSW_PACK_VERSION_V2: "ark_dyndata_pack_v2.json",
    KSW_PACK_VERSION_V3: "ark_dyndata_pack_v3.json",
}
# Kept for compatibility with callers and docs that referenced the original
# single-pack default name before v2 publishing was added.
DEFAULT_PACK_FILE_NAME = DEFAULT_PACK_FILE_NAMES[KSW_DEFAULT_PACK_VERSION]
KSW_DYN_PROFILE_OFFSET_MAX = 0x0000FFFF
KSW_DYN_PROFILE_GLOBAL_RVA_MAX = 0x7FFFFFFF
KSW_SCHEMA_VERSION = 1
KSW_PACK_VERSION = KSW_DEFAULT_PACK_VERSION

CALLBACK_ITEM_KIND_GLOBAL_RVA = "GlobalRva"
CALLBACK_ITEM_KIND_STRUCT_OFFSET = "StructOffset"
CALLBACK_ITEM_KINDS = {CALLBACK_ITEM_KIND_GLOBAL_RVA, CALLBACK_ITEM_KIND_STRUCT_OFFSET}

# These names mirror the KSW_DYN_FIELD_ID_CB_* field-id additions in
# shared/driver/KswordArkDynDataIoctl.h. The numeric values are kept here as
# validation evidence and to catch duplicate aliases that target the same shared
# callback field ID before a bad v2 pack reaches R3/R0.
CALLBACK_GLOBAL_RVA_FIELD_IDS = {
    "PspCreateProcessNotifyRoutine": 44,
    "PspCreateThreadNotifyRoutine": 45,
    "PspLoadImageNotifyRoutine": 46,
    "PspNotifyEnableMask": 47,
    "CmCallbackListHead": 48,
}

KERNEL_GLOBAL_RVA_FIELD_IDS = {
    "PspCidTable": 82,
    "PsLoadedModuleList": 83,
    "MmUnloadedDrivers": 84,
    "PiDDBCacheTable": 85,
}

CALLBACK_STRUCT_OFFSET_FIELD_IDS = {
    "_OBJECT_TYPE.CallbackList": 49,
    "_CALLBACK_ENTRY_ITEM.EntryList": 50,
    "_CALLBACK_ENTRY_ITEM.PreOperation": 51,
    "_CALLBACK_ENTRY_ITEM.PostOperation": 52,
    "_CALLBACK_ENTRY_ITEM.Operations": 53,
    "_CALLBACK_ENTRY_ITEM.CallbackEntry": 54,
    "_CALLBACK_ENTRY.Altitude": 55,
    "_CALLBACK_ENTRY.RegistrationContext": 56,
}

KNOWN_FIELD_IDS = {
    "EpObjectTable": 1,
    "EpSectionObject": 2,
    "HtHandleContentionEvent": 3,
    "OtName": 4,
    "OtIndex": 5,
    "ObDecodeShift": 6,
    "ObAttributesShift": 7,
    "KtInitialStack": 8,
    "KtStackLimit": 9,
    "KtStackBase": 10,
    "KtKernelStack": 11,
    "KtReadOperationCount": 12,
    "KtWriteOperationCount": 13,
    "KtOtherOperationCount": 14,
    "KtReadTransferCount": 15,
    "KtWriteTransferCount": 16,
    "KtOtherTransferCount": 17,
    "MmSectionControlArea": 18,
    "MmControlAreaListHead": 19,
    "MmControlAreaLock": 20,
    "AlpcCommunicationInfo": 21,
    "AlpcOwnerProcess": 22,
    "AlpcConnectionPort": 23,
    "AlpcServerCommunicationPort": 24,
    "AlpcClientCommunicationPort": 25,
    "AlpcHandleTable": 26,
    "AlpcHandleTableLock": 27,
    "AlpcAttributes": 28,
    "AlpcAttributesFlags": 29,
    "AlpcPortContext": 30,
    "AlpcPortObjectLock": 31,
    "AlpcSequenceNo": 32,
    "AlpcState": 33,
    "LxPicoProc": 34,
    "LxPicoProcInfo": 35,
    "LxPicoProcInfoPID": 36,
    "LxPicoThrdInfo": 37,
    "LxPicoThrdInfoTID": 38,
    "EpProtection": 39,
    "EpSignatureLevel": 40,
    "EpSectionSignatureLevel": 41,
    "EgeGuid": 42,
    "EreGuidEntry": 43,
    "EpUniqueProcessId": 57,
    "EpActiveProcessLinks": 58,
    "EpThreadListHead": 59,
    "EpImageFileName": 60,
    "EpToken": 61,
    "EtCid": 62,
    "EtThreadListEntry": 63,
    "EtStartAddress": 64,
    "EtWin32StartAddress": 65,
    "KtProcess": 66,
    "HtTableCode": 67,
    "HtHandleCount": 68,
    "HteLowValue": 69,
    "KldrInLoadOrderLinks": 70,
    "KldrDllBase": 71,
    "KldrSizeOfImage": 72,
    "KldrFullDllName": 73,
    "KldrBaseDllName": 74,
    "KldrFlags": 75,
    "DoDriverStart": 76,
    "DoDriverSize": 77,
    "DoDriverSection": 78,
    "DoMajorFunction": 79,
    "DoFastIoDispatch": 80,
    "DoDriverUnload": 81,
}

LXCORE_FIELD_NAMES = {
    "LxPicoProc",
    "LxPicoProcInfo",
    "LxPicoProcInfoPID",
    "LxPicoThrdInfo",
    "LxPicoThrdInfoTID",
}

TYPED_KERNEL_STRUCT_OFFSET_FIELD_IDS = {
    name: field_id for name, field_id in KNOWN_FIELD_IDS.items() if name not in LXCORE_FIELD_NAMES
}

# The generator historically emitted the concrete PDB member spelling
# EntryItemList. The shared protocol field name is EntryList, so release sync
# accepts the generator spelling as an input alias and writes the canonical name
# to v2 packs.
CALLBACK_NAME_ALIASES = {
    "_CALLBACK_ENTRY_ITEM.EntryItemList": "_CALLBACK_ENTRY_ITEM.EntryList",
}

CALLBACK_FIELD_IDS = {
    **CALLBACK_GLOBAL_RVA_FIELD_IDS,
    **CALLBACK_STRUCT_OFFSET_FIELD_IDS,
    **{alias: CALLBACK_STRUCT_OFFSET_FIELD_IDS[canonical] for alias, canonical in CALLBACK_NAME_ALIASES.items()},
}

GLOBAL_RVA_FIELD_IDS = {
    **CALLBACK_GLOBAL_RVA_FIELD_IDS,
    **KERNEL_GLOBAL_RVA_FIELD_IDS,
}

TYPED_STRUCT_OFFSET_FIELD_IDS = {
    **TYPED_KERNEL_STRUCT_OFFSET_FIELD_IDS,
    **CALLBACK_STRUCT_OFFSET_FIELD_IDS,
    **{alias: CALLBACK_STRUCT_OFFSET_FIELD_IDS[canonical] for alias, canonical in CALLBACK_NAME_ALIASES.items()},
}

CALLBACK_KIND_BY_NAME = {
    **{name: CALLBACK_ITEM_KIND_GLOBAL_RVA for name in CALLBACK_GLOBAL_RVA_FIELD_IDS},
    **{name: CALLBACK_ITEM_KIND_STRUCT_OFFSET for name in CALLBACK_STRUCT_OFFSET_FIELD_IDS},
    **{alias: CALLBACK_ITEM_KIND_STRUCT_OFFSET for alias in CALLBACK_NAME_ALIASES},
}

CALLBACK_CANONICAL_NAME_BY_NAME = {
    **{name: name for name in CALLBACK_GLOBAL_RVA_FIELD_IDS},
    **{name: name for name in CALLBACK_STRUCT_OFFSET_FIELD_IDS},
    **CALLBACK_NAME_ALIASES,
}

KNOWN_FIELD_NAMES = set(KNOWN_FIELD_IDS)


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
    - callback_items: Validated callback v2 items normalized for pack output.
    - typed_items: Validated v3 typed items normalized for pack output.
    - missing_fields/missing_globals/coverage_percent: Coverage report values
      propagated from the generator or computed from the accepted payload.

    Processing:
    - The record keeps enough metadata for deduplication, pack generation, and
      manifest output.

    Return behavior:
    - Instances are plain data containers and do not copy files by themselves.
    """

    path: Path
    data: dict[str, Any]
    identity: ProfileIdentity
    field_count: int
    callback_items: list[dict[str, Any]] = field(default_factory=list)
    typed_items: list[dict[str, Any]] = field(default_factory=list)
    missing_fields: list[str] = field(default_factory=list)
    missing_globals: list[str] = field(default_factory=list)
    coverage_percent: float = 0.0

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
    parser.add_argument("--emit-pack", action="store_true", help="Write compact profile pack JSON into the release profiles directory.")
    parser.add_argument("--pack-only", action="store_true", help="Publish only the compact pack and skip scattered JSON copies.")
    parser.add_argument(
        "--pack-version",
        type=int,
        choices=KSW_SUPPORTED_PACK_VERSIONS,
        default=KSW_DEFAULT_PACK_VERSION,
        help=(
            "Compact pack format version to emit when --emit-pack/--pack-only is used; "
            "1 keeps legacy fields-only output, 2 adds callbackItems, 3 adds typed items. Default: 1."
        ),
    )
    parser.add_argument(
        "--pack-output",
        default="",
        help="Pack output path; defaults to <release-root>\\profiles\\ark_dyndata_pack_v<pack-version>.json.",
    )
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


def parse_float_percent(value: Any) -> float | None:
    """Parse a JSON value into a bounded percentage.

    Inputs:
    - value: JSON number or decimal string from profile coverage metadata.

    Processing:
    - Accepts values in the inclusive 0.0 to 100.0 range.
    - Rejects booleans and malformed strings so report data remains numeric.

    Return behavior:
    - Returns a float on success; returns None when the value is absent or
      invalid.
    """
    try:
        if isinstance(value, bool):
            return None
        if isinstance(value, (int, float)):
            parsed = float(value)
        elif isinstance(value, str):
            parsed = float(value.strip())
        else:
            return None
    except ValueError:
        return None
    if parsed < 0.0 or parsed > 100.0:
        return None
    return parsed


def normalize_string_list(value: Any) -> list[str]:
    """Normalize an optional JSON string array for coverage reporting.

    Inputs:
    - value: Candidate JSON array, usually missingFields or missingGlobals.

    Processing:
    - Keeps only non-empty string entries after trimming whitespace.
    - Ignores malformed entries instead of rejecting the profile because
      coverage reporting should not block publishable offsets.

    Return behavior:
    - Returns a list of normalized strings; returns an empty list for invalid
      or absent input.
    """
    if not isinstance(value, list):
        return []
    result: list[str] = []
    for item in value:
        if not isinstance(item, str):
            continue
        text = item.strip()
        if text:
            result.append(text)
    return result


def require_uint32(value: Any, context: str) -> int:
    """Parse a required unsigned 32-bit value or raise a precise error.

    Inputs:
    - value: JSON value to parse.
    - context: Field name/path used in the error message.

    Processing:
    - Reuses parse_uint32 so pack generation follows the same numeric rules as
      validation.

    Return behavior:
    - Returns the parsed integer on success; raises ValueError on failure.
    """
    parsed = parse_uint32(value)
    if parsed is None:
        raise ValueError(f"required uint32 is invalid: {context}")
    return parsed


def pack_file_name_for_version(pack_version: int) -> str:
    """Return the default compact pack file name for one supported version.

    Inputs:
    - pack_version: Integer selected by --pack-version.

    Processing:
    - Looks up the version-specific file name so v1 remains the default legacy
      artifact and v2 gets its own release-side file.

    Return behavior:
    - Returns the default file name on success; raises ValueError for an
      unsupported version before any release file is written.
    """
    try:
        return DEFAULT_PACK_FILE_NAMES[pack_version]
    except KeyError as exc:
        raise ValueError(f"unsupported pack version: {pack_version}") from exc


def normalize_callback_item_name(name: str) -> str | None:
    """Normalize a callback item name to the shared protocol spelling.

    Inputs:
    - name: JSON callbackItems[].name value after whitespace trimming.

    Processing:
    - Accepts only names that correspond to KSW_DYN_FIELD_ID_CB_* shared field
      IDs.
    - Converts known generator aliases to the canonical shared field-name text
      used by R3/R0.

    Return behavior:
    - Returns the canonical callback name on success; returns None for unknown
      names.
    """
    if name not in CALLBACK_FIELD_IDS:
        return None
    return CALLBACK_CANONICAL_NAME_BY_NAME[name]


def normalize_typed_item_name(name: str, kind: str) -> tuple[str, int] | None:
    """Normalize a v3 typed item name and return its shared field ID.

    Inputs:
    - name: JSON items[].name value after whitespace trimming.
    - kind: JSON items[].kind value, either StructOffset or GlobalRva.

    Processing:
    - Routes GlobalRva names through callback and kernel-global maps.
    - Routes StructOffset names through known kernel fields plus callback
      structure offsets, accepting the historical EntryItemList alias.

    Return behavior:
    - Returns (canonicalName, fieldId) when name/kind are valid; returns None
      when the item is unsupported.
    """
    if kind == CALLBACK_ITEM_KIND_GLOBAL_RVA:
        field_id = GLOBAL_RVA_FIELD_IDS.get(name)
        return (name, field_id) if field_id is not None else None
    if kind == CALLBACK_ITEM_KIND_STRUCT_OFFSET:
        canonical_name = CALLBACK_NAME_ALIASES.get(name, name)
        if canonical_name in CALLBACK_STRUCT_OFFSET_FIELD_IDS:
            return canonical_name, CALLBACK_STRUCT_OFFSET_FIELD_IDS[canonical_name]
        field_id = TYPED_STRUCT_OFFSET_FIELD_IDS.get(name)
        return (name, field_id) if field_id is not None else None
    return None


def validate_callback_items(path: Path, data: dict[str, Any], state: ValidationState) -> list[dict[str, Any]] | None:
    """Validate optional callbackItems and return a compact normalized list.

    Inputs:
    - path: Source JSON profile path used in diagnostics.
    - data: Parsed profile root dictionary.
    - state: Validation accumulator used for precise rejection reasons.

    Processing:
    - Treats a missing callbackItems key as a valid empty callback set, which
      keeps legacy profiles publishable.
    - Requires every present item to be an object with a supported name, kind,
      and uint32 value.
    - Enforces name/kind semantics: callback global names must use GlobalRva,
      and callback struct/member names must use StructOffset.
    - Validates values with the same upper bound used by R3/R0 struct offsets;
      GlobalRva values must be non-zero and remain within the shared
      KSW_DYN_PROFILE_GLOBAL_RVA_MAX range.
    - Rejects duplicate shared callback field IDs so aliases cannot create two
      entries for the same R0 item.

    Return behavior:
    - Returns a list of JSON-ready objects containing only name/kind/value when
      validation succeeds.
    - Returns None and records a profile rejection when validation fails.
    """
    if "callbackItems" not in data:
        return []

    callback_items = data.get("callbackItems")
    if callback_items is None:
        return []
    if not isinstance(callback_items, list):
        reject(state, path, "callback_items_not_array")
        return None

    normalized_items: list[dict[str, Any]] = []
    seen_field_ids: set[int] = set()
    for index, item in enumerate(callback_items):
        context = f"callbackItems[{index}]"
        if not isinstance(item, dict):
            reject(state, path, f"callback_item_not_object:{context}")
            return None

        raw_name = item.get("name")
        if not isinstance(raw_name, str):
            reject(state, path, f"callback_name_invalid:{context}")
            return None
        name = raw_name.strip()
        canonical_name = normalize_callback_item_name(name)
        if canonical_name is None:
            reject(state, path, f"callback_name_unknown:{name}")
            return None

        raw_kind = item.get("kind")
        if not isinstance(raw_kind, str):
            reject(state, path, f"callback_kind_invalid:{canonical_name}")
            return None
        kind = raw_kind.strip()
        if kind not in CALLBACK_ITEM_KINDS:
            reject(state, path, f"callback_kind_unsupported:{canonical_name}:{kind}")
            return None

        expected_kind = CALLBACK_KIND_BY_NAME[name]
        if kind != expected_kind:
            reject(state, path, f"callback_kind_mismatch:{canonical_name}:{kind}:expected_{expected_kind}")
            return None

        value = parse_uint32(item.get("value"))
        if value is None:
            reject(state, path, f"callback_value_invalid:{canonical_name}")
            return None
        if kind == CALLBACK_ITEM_KIND_GLOBAL_RVA:
            if value == 0:
                reject(state, path, f"callback_global_rva_zero:{canonical_name}")
                return None
            if value > KSW_DYN_PROFILE_GLOBAL_RVA_MAX:
                reject(state, path, f"callback_global_rva_out_of_range:{canonical_name}")
                return None
        if kind == CALLBACK_ITEM_KIND_STRUCT_OFFSET and (value == 0xFFFFFFFF or value > KSW_DYN_PROFILE_OFFSET_MAX):
            reject(state, path, f"callback_struct_offset_invalid:{canonical_name}")
            return None

        field_id = CALLBACK_FIELD_IDS[name]
        if field_id in seen_field_ids:
            reject(state, path, f"callback_duplicate:{canonical_name}")
            return None
        seen_field_ids.add(field_id)

        normalized_items.append({"name": canonical_name, "kind": kind, "value": value})

    return normalized_items


def typed_items_from_data(data: dict[str, Any]) -> Any:
    """Return the preferred v3 typed item array from one profile root.

    Inputs:
    - data: Parsed profile dictionary.

    Processing:
    - Accepts both items and typedItems so the release tool can read the compact
      pack spelling and the scattered-profile spelling.
    - Prefers items when both are present because it is the v3 pack key.

    Return behavior:
    - Returns the raw JSON value for downstream validation.
    """
    items_value = data.get("items")
    if isinstance(items_value, list) and items_value:
        return items_value
    typed_items_value = data.get("typedItems")
    if isinstance(typed_items_value, list):
        return typed_items_value
    return items_value if "items" in data else typed_items_value


def validate_typed_items(path: Path, data: dict[str, Any], state: ValidationState) -> list[dict[str, Any]] | None:
    """Validate optional v3 typed items and return normalized pack records.

    Inputs:
    - path: Source JSON profile path used in diagnostics.
    - data: Parsed profile root dictionary.
    - state: Validation accumulator used for precise rejection reasons.

    Processing:
    - Treats a missing items/typedItems key as an empty typed set.
    - Requires every present item to contain a supported name, StructOffset or
      GlobalRva kind, and safe uint32 value.
    - Enforces duplicate detection by shared field ID so aliases cannot target
      the same R0 item twice.

    Return behavior:
    - Returns normalized objects with name/kind/value and optional required flag
      when validation succeeds.
    - Returns None and records a rejection when validation fails.
    """
    raw_items = typed_items_from_data(data)
    if raw_items is None:
        return []
    if not isinstance(raw_items, list):
        reject(state, path, "typed_items_not_array")
        return None

    normalized_items: list[dict[str, Any]] = []
    seen_field_ids: set[int] = set()
    for index, item in enumerate(raw_items):
        context = f"typedItems[{index}]"
        if not isinstance(item, dict):
            reject(state, path, f"typed_item_not_object:{context}")
            return None

        raw_kind = item.get("kind")
        if not isinstance(raw_kind, str):
            reject(state, path, f"typed_kind_invalid:{context}")
            return None
        kind = raw_kind.strip()
        if kind not in CALLBACK_ITEM_KINDS:
            reject(state, path, f"typed_kind_unsupported:{context}:{kind}")
            return None

        raw_name = item.get("name")
        if not isinstance(raw_name, str):
            reject(state, path, f"typed_name_invalid:{context}")
            return None
        name = raw_name.strip()
        normalized_name = normalize_typed_item_name(name, kind)
        if normalized_name is None:
            reject(state, path, f"typed_name_unknown:{name}:{kind}")
            return None
        canonical_name, field_id = normalized_name

        value = parse_uint32(item.get("value"))
        if value is None:
            reject(state, path, f"typed_value_invalid:{canonical_name}")
            return None
        if kind == CALLBACK_ITEM_KIND_GLOBAL_RVA and (value == 0 or value > KSW_DYN_PROFILE_GLOBAL_RVA_MAX):
            reject(state, path, f"typed_global_rva_out_of_range:{canonical_name}")
            return None
        if kind == CALLBACK_ITEM_KIND_STRUCT_OFFSET and (value == 0xFFFFFFFF or value > KSW_DYN_PROFILE_OFFSET_MAX):
            reject(state, path, f"typed_struct_offset_invalid:{canonical_name}")
            return None
        if field_id in seen_field_ids:
            reject(state, path, f"typed_duplicate:{canonical_name}")
            return None
        seen_field_ids.add(field_id)

        normalized_item: dict[str, Any] = {"name": canonical_name, "kind": kind, "value": value}
        if isinstance(item.get("required"), bool):
            normalized_item["required"] = bool(item.get("required"))
        normalized_items.append(normalized_item)

    return normalized_items


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
    fields = data.get("fields", {})
    if not isinstance(module, dict):
        reject(state, path, "module_not_object")
        return None
    if not isinstance(fields, dict):
        reject(state, path, "fields_not_object")
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

    callback_items = validate_callback_items(path, data, state)
    if callback_items is None:
        return None
    typed_items = validate_typed_items(path, data, state)
    if typed_items is None:
        return None
    if not fields and not typed_items:
        reject(state, path, "fields_and_typed_items_empty")
        return None

    missing_fields = normalize_string_list(data.get("missingFields"))
    missing_globals = normalize_string_list(data.get("missingGlobals"))
    coverage_percent = parse_float_percent(data.get("coveragePercent"))
    if coverage_percent is None:
        resolved_count = len(typed_items) if typed_items else (len(fields) + len(callback_items))
        missing_count = len(missing_fields) + len(missing_globals)
        total_count = resolved_count + missing_count
        coverage_percent = 0.0 if total_count == 0 else round((resolved_count / total_count) * 100.0, 1)

    identity = ProfileIdentity(module_class, machine, time_date_stamp, size_of_image)
    return ProfileRecord(
        path=path,
        data=data,
        identity=identity,
        field_count=len(fields),
        callback_items=callback_items,
        typed_items=typed_items,
        missing_fields=missing_fields,
        missing_globals=missing_globals,
        coverage_percent=coverage_percent,
    )


def choose_duplicate_winner(records: list[ProfileRecord]) -> ProfileRecord:
    """Choose a deterministic winner for one duplicate identity group.

    Inputs:
    - records: Profiles with the same class/machine/timestamp/size tuple.

    Processing:
    - Prefers the record with more fields.
    - For equal field coverage, prefers the record with more callbackItems so
      v2 publishing does not discard newer callback metadata.
    - Uses profileName and path as stable tie-breakers to avoid nondeterminism.

    Return behavior:
    - Returns the selected ProfileRecord; does not mutate input records.
    """
    return sorted(
        records,
        key=lambda item: (
            -item.field_count,
            -len(item.typed_items),
            -len(item.callback_items),
            -item.coverage_percent,
            item.profile_name,
            str(item.path),
        ),
    )[0]


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


def module_class_id_from_text(module_class: str) -> int | None:
    """Map a profile module class string to the shared numeric class id.

    Inputs:
    - module_class: Text value from the source JSON profile.

    Processing:
    - Accepts the stable ntoskrnl/ntkrla57 aliases already used by the corpus.
    - Returns the shared R0/R3 class id so the pack can store a compact integer.

    Return behavior:
    - Returns a class id on success; returns None for unsupported input.
    """
    normalized = module_class.strip().lower()
    if normalized in {"ntoskrnl", "ntoskrnl.exe", "ntkrnlmp", "ntkrnlmp.exe"}:
        return 0
    if normalized in {"ntkrla57", "ntkrla57.exe"}:
        return 1
    if normalized in {"lxcore", "lxcore.exe"}:
        return 2
    return None


def build_pack_field_dictionary(records: list[ProfileRecord]) -> tuple[list[str], dict[str, int]]:
    """Build the compact field dictionary used by the profile pack.

    Inputs:
    - records: Final deduplicated profile records.

    Processing:
    - Collects every field name once and sorts them for deterministic output.
    - Produces both the dictionary list and a name-to-index lookup table.

    Return behavior:
    - Returns (fieldDictionary, fieldIndexMap).
    """
    field_names = sorted({field_name for record in records for field_name in record.data.get("fields", {}).keys()})
    field_index = {name: index for index, name in enumerate(field_names)}
    return field_names, field_index


def build_pack_typed_items(record: ProfileRecord, pack_version: int) -> list[dict[str, Any]]:
    """Build pack typed items for one profile record.

    Inputs:
    - record: Validated profile record.
    - pack_version: Selected compact pack schema version.

    Processing:
    - Reuses prevalidated typed_items when the source profile already provided
      them.
    - Falls back to legacy fields plus callbackItems when a typed v3 payload was
      not present in the source JSON.

    Return behavior:
    - Returns a JSON-ready typed item list.
    """
    if pack_version != KSW_PACK_VERSION_V3:
        return []
    if record.typed_items:
        return [dict(item) for item in record.typed_items]

    typed_items: list[dict[str, Any]] = []
    for field_name, offset_value in sorted(record.data.get("fields", {}).items()):
        if field_name not in TYPED_KERNEL_STRUCT_OFFSET_FIELD_IDS:
            continue
        typed_items.append({"name": field_name, "kind": CALLBACK_ITEM_KIND_STRUCT_OFFSET, "value": require_uint32(offset_value, f"{record.path}:{field_name}")})
    for item in record.callback_items:
        typed_items.append(dict(item))
    return typed_items


def build_pack_profile_entry(record: ProfileRecord, field_index: dict[str, int], pack_version: int) -> dict[str, Any]:
    """Convert one validated profile record into a compact pack entry.

    Inputs:
    - record: Deduplicated profile record.
    - field_index: Field dictionary lookup table.
    - pack_version: Selected compact pack schema version.

    Processing:
    - Normalizes module identity and PDB metadata to compact numeric/string
      values.
    - Stores field payload as [fieldIndex, offset] pairs sorted by field index.
    - For v2 packs, appends the already validated callbackItems payload using
      canonical shared protocol names and compact integer values.

    Return behavior:
    - Returns a JSON-serializable dictionary for the pack file.
    """
    module = record.data.get("module")
    if not isinstance(module, dict):
        raise ValueError(f"invalid module object for {record.path}")

    module_class = module_class_id_from_text(str(module.get("class", "")))
    if module_class is None:
        raise ValueError(f"unsupported module class for {record.path}")

    fields = record.data.get("fields", {})
    if not isinstance(fields, dict) or (not fields and pack_version != KSW_PACK_VERSION_V3):
        raise ValueError(f"invalid field set for {record.path}")

    packed_fields: list[list[int]] = []
    for field_name, offset_value in sorted(fields.items(), key=lambda item: field_index[item[0]]):
        packed_fields.append([field_index[field_name], require_uint32(offset_value, f"{record.path}:{field_name}")])

    pdb_age = parse_uint32(module.get("pdbAge")) or 0

    profile_entry = {
        "moduleClassId": module_class,
        "machine": require_uint32(module.get("machine"), f"{record.path}:machine"),
        "timeDateStamp": require_uint32(module.get("timeDateStamp"), f"{record.path}:timeDateStamp"),
        "sizeOfImage": require_uint32(module.get("sizeOfImage"), f"{record.path}:sizeOfImage"),
        "profileName": record.profile_name,
        "pdbName": str(module.get("pdbName", "")),
        "pdbGuid": str(module.get("pdbGuid", "")),
        "pdbAge": pdb_age,
        "fields": packed_fields,
    }
    if pack_version == KSW_PACK_VERSION_V2:
        profile_entry["callbackItems"] = record.callback_items
    elif pack_version == KSW_PACK_VERSION_V3:
        profile_entry["items"] = build_pack_typed_items(record, pack_version)
        profile_entry["coveragePercent"] = record.coverage_percent
        profile_entry["missingFields"] = record.missing_fields
        profile_entry["missingGlobals"] = record.missing_globals
    return profile_entry


def build_profile_pack(records: list[ProfileRecord], pack_version: int) -> dict[str, Any]:
    """Build the selected compact profile pack from validated records.

    Inputs:
    - records: Final deduplicated publish list.
    - pack_version: Compact pack schema version selected by the caller.

    Processing:
    - Creates a once-per-pack field dictionary.
    - Converts each profile into a compact numeric identity plus field-index
      tuples.
    - Preserves v1 fields-only behavior by omitting callbackItems unless v2 is
      explicitly requested.

    Return behavior:
    - Returns a JSON-serializable pack dictionary.
    """
    if pack_version not in KSW_SUPPORTED_PACK_VERSIONS:
        raise ValueError(f"unsupported pack version: {pack_version}")
    field_dictionary, field_index = build_pack_field_dictionary(records)
    return {
        "schemaVersion": KSW_SCHEMA_VERSION,
        "packVersion": pack_version,
        "fieldDictionary": field_dictionary,
        "profiles": [build_pack_profile_entry(record, field_index, pack_version) for record in records],
    }


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


def clean_scattered_profile_target(target_dir: Path) -> int:
    """Delete stale scattered JSON profiles from the release profile directory.

    Inputs:
    - target_dir: Release\\profiles\\ark_dyndata destination.

    Processing:
    - Verifies the expected directory suffix before deleting anything.
    - Removes only direct child *.json files; pack files and manifests live in
      the parent profiles directory and are never touched here.

    Return behavior:
    - Returns the number of JSON files removed.
    """
    if not target_dir.exists():
        return 0

    ensure_safe_target(target_dir)
    removed_count = 0
    for stale_file in target_dir.glob("*.json"):
        stale_file.unlink()
        removed_count += 1
    return removed_count


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
        clean_scattered_profile_target(target_dir)

    selected = records[:max_copy] if max_copy > 0 else records
    copied: list[Path] = []
    for record in selected:
        destination = target_dir / record.path.name
        shutil.copy2(record.path, destination)
        copied.append(destination)
    return copied


def build_manifest(
    state: ValidationState,
    records: list[ProfileRecord],
    copied: list[Path],
    local_identity: ProfileIdentity | None,
    pack_path: Path | None,
    pack: dict[str, Any] | None,
    removed_scattered_profiles: int,
    pack_only: bool,
    include_profile_list: bool,
) -> dict[str, Any]:
    """Build a manifest/report dictionary for the publish pass.

    Inputs:
    - state: Validation details including rejected and duplicate records.
    - records: Final deduplicated publish list.
    - copied: Destination files copied during this run.
    - local_identity: Optional local ntoskrnl identity.
    - pack_path/pack: Optional compact pack output metadata.
    - removed_scattered_profiles: Count of stale scattered JSON files removed.
    - pack_only: Whether this pass intentionally skipped scattered JSON copies.
    - include_profile_list: Whether to include every profile identity in this
      output. Reports use this for audit detail; Release manifests omit it to
      avoid duplicating pack data.

    Processing:
    - Computes count summaries, class coverage, pack metadata, and local profile
      match status.

    Return behavior:
    - Returns a JSON-serializable dictionary.
    """
    class_counts: dict[str, int] = {}
    local_match: dict[str, Any] | None = None
    callback_item_count = 0
    typed_item_count = 0
    for record in records:
        class_counts[record.identity.module_class] = class_counts.get(record.identity.module_class, 0) + 1
        callback_item_count += len(record.callback_items)
        typed_item_count += len(record.typed_items)
        if local_identity is not None and record.identity == local_identity:
            local_match = {"profile": str(record.path), "profileName": record.profile_name}

    pack_summary = None
    manifest_pack_version = KSW_DEFAULT_PACK_VERSION
    if pack is not None:
        pack_profiles = pack.get("profiles", [])
        pack_fields = pack.get("fieldDictionary", [])
        pack_callback_item_count = 0
        pack_typed_item_count = 0
        if isinstance(pack_profiles, list):
            for profile in pack_profiles:
                if not isinstance(profile, dict):
                    continue
                callback_items = profile.get("callbackItems", [])
                if isinstance(callback_items, list):
                    pack_callback_item_count += len(callback_items)
                typed_items = profile.get("items", [])
                if isinstance(typed_items, list):
                    pack_typed_item_count += len(typed_items)
        manifest_pack_version = parse_uint32(pack.get("packVersion")) or KSW_DEFAULT_PACK_VERSION
        pack_summary = {
            "path": str(pack_path) if pack_path is not None else None,
            "schemaVersion": pack.get("schemaVersion"),
            "packVersion": pack.get("packVersion"),
            "profileCount": len(pack_profiles) if isinstance(pack_profiles, list) else 0,
            "fieldDictionaryCount": len(pack_fields) if isinstance(pack_fields, list) else 0,
            "callbackItemCount": pack_callback_item_count,
            "typedItemCount": pack_typed_item_count,
        }

    manifest = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "schemaVersion": KSW_SCHEMA_VERSION,
        "packVersion": manifest_pack_version,
        "publishMode": "pack-only" if pack_only else ("pack-and-json" if pack is not None else "json"),
        "acceptedBeforeDedup": len(state.accepted),
        "publishedProfiles": len(records),
        "callbackItemCount": callback_item_count,
        "typedItemCount": typed_item_count,
        "copiedProfiles": len(copied),
        "removedScatteredProfiles": removed_scattered_profiles,
        "rejectedProfiles": len(state.rejected),
        "duplicateGroups": len(state.duplicate_groups),
        "classCounts": class_counts,
        "pack": pack_summary,
        "localKernelIdentity": local_identity.__dict__ if local_identity is not None else None,
        "localKernelProfileMatch": local_match,
        "rejected": state.rejected,
        "duplicates": state.duplicate_groups,
    }
    if include_profile_list:
        manifest["profiles"] = [
            {
                "fileName": record.path.name,
                "profileName": record.profile_name,
                "identity": record.identity.__dict__,
                "fieldCount": record.field_count,
                "callbackItemCount": len(record.callback_items),
                "typedItemCount": len(record.typed_items),
                "missingFields": record.missing_fields,
                "missingGlobals": record.missing_globals,
                "coveragePercent": record.coverage_percent,
            }
            for record in records
        ]
    else:
        manifest["profileListOmitted"] = True
    return manifest


def write_json(path: Path, data: dict[str, Any]) -> None:
    """Write a JSON file with stable indentation and UTF-8 encoding."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_compact_json(path: Path, data: dict[str, Any]) -> None:
    """Write compact UTF-8 JSON without whitespace-heavy pretty formatting.

    Inputs:
    - path: Destination pack path.
    - data: JSON-serializable pack dictionary.

    Processing:
    - Creates parent directories and writes deterministic key ordering.
    - Uses compact separators so Release carries one small pack file.

    Return behavior:
    - No return value; raises OSError/TypeError if writing fails.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    """Program entry point; returns a process exit code."""
    args = parse_args(argv)
    source_dir = Path(args.source)
    release_root = Path(args.release_root)
    target_dir = release_root / "profiles" / "ark_dyndata"
    manifest_path = Path(args.manifest) if args.manifest else release_root / "profiles" / "ark_dyndata_manifest.json"
    pack_version = int(args.pack_version)
    pack_file_name = pack_file_name_for_version(pack_version)
    pack_path = Path(args.pack_output) if args.pack_output else release_root / "profiles" / pack_file_name
    report_path = Path(args.report)
    emit_pack = bool(args.emit_pack or args.pack_only)

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
    removed_scattered_profiles = 0
    pack: dict[str, Any] | None = None
    if not args.dry_run:
        if args.pack_only and args.clean_target:
            removed_scattered_profiles = clean_scattered_profile_target(target_dir)
        if not args.pack_only:
            copied = copy_profiles(final_records, target_dir, args.clean_target, args.max_copy)
        if emit_pack:
            pack = build_profile_pack(final_records, pack_version)
            write_compact_json(pack_path, pack)
    elif emit_pack:
        pack = build_profile_pack(final_records, pack_version)

    report_manifest = build_manifest(
        state=state,
        records=final_records,
        copied=copied,
        local_identity=local_identity,
        pack_path=pack_path if emit_pack else None,
        pack=pack,
        removed_scattered_profiles=removed_scattered_profiles,
        pack_only=bool(args.pack_only),
        include_profile_list=True,
    )
    release_manifest = build_manifest(
        state=state,
        records=final_records,
        copied=copied,
        local_identity=local_identity,
        pack_path=pack_path if emit_pack else None,
        pack=pack,
        removed_scattered_profiles=removed_scattered_profiles,
        pack_only=bool(args.pack_only),
        include_profile_list=False,
    )
    write_json(report_path, report_manifest)
    if not args.dry_run:
        write_json(manifest_path, release_manifest)

    print(f"sourceProfiles={len(list(source_dir.glob('*.json')))}")
    print(f"acceptedBeforeDedup={len(state.accepted)}")
    print(f"publishedProfiles={len(final_records)}")
    print(f"callbackItems={report_manifest['callbackItemCount']}")
    print(f"typedItems={report_manifest['typedItemCount']}")
    print(f"rejectedProfiles={len(state.rejected)}")
    print(f"duplicateGroups={len(state.duplicate_groups)}")
    print(f"copiedProfiles={len(copied)}")
    print(f"removedScatteredProfiles={removed_scattered_profiles}")
    print(f"targetDir={target_dir}")
    print(f"pack={pack_path if emit_pack and not args.dry_run else ('<dry-run>' if emit_pack else '<disabled>')}")
    print(f"manifest={manifest_path if not args.dry_run else '<dry-run>'}")
    print(f"report={report_path}")
    print(f"localKernelMatch={'yes' if report_manifest['localKernelProfileMatch'] else 'no'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
