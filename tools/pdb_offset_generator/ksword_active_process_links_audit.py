#!/usr/bin/env python3
"""
Audit KswordARK ActiveProcessLinks DynData profile coverage.

Inputs:
- A compact DynData v3 pack JSON, normally
  Ksword5.1\\Ksword5.1\\x64\\Release\\profiles\\ark_dyndata_pack_v3.json.
- A local ntoskrnl.exe image used only for PE identity matching.

Processing:
- Parses the PE machine, TimeDateStamp, and SizeOfImage tuple from ntoskrnl.exe.
- Finds the exact matching ntoskrnl profile in the compact pack.
- Reports EpActiveProcessLinks values from both legacy fields pairs and v3
  typed StructOffset items.

Return behavior:
- Exits 0 only when the pack is v3-compatible, the local kernel profile matches,
  and EpActiveProcessLinks is present with at least one non-sentinel offset.
- Exits non-zero with a concise diagnostic when evidence is missing.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_PACK = Path(r"Ksword5.1\Ksword5.1\x64\Release\profiles\ark_dyndata_pack_v3.json")
DEFAULT_KERNEL = Path(r"C:\Windows\System32\ntoskrnl.exe")
ACTIVE_PROCESS_LINKS_NAMES = {"EpActiveProcessLinks", "_EPROCESS.ActiveProcessLinks"}
UNAVAILABLE_OFFSETS = {0xFFFFFFFF, 0x0000FFFF}


@dataclass(frozen=True)
class PeIdentity:
    """PE identity tuple used by R0/R3 profile matching."""

    machine: int
    time_date_stamp: int
    size_of_image: int


def parse_uint32(value: Any) -> int | None:
    """Parse a JSON value as uint32.

    Inputs:
    - value: int/float/string JSON value.

    Processing:
    - Accepts decimal numbers and strings with optional 0x prefix.
    - Rejects negative values and values outside uint32 range.

    Return behavior:
    - Returns the parsed integer on success; returns None on failure.
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        parsed = value
    elif isinstance(value, float) and value.is_integer():
        parsed = int(value)
    elif isinstance(value, str):
        text = value.strip()
        base = 16 if text.lower().startswith("0x") else 10
        if base == 16:
            text = text[2:]
        try:
            parsed = int(text, base)
        except ValueError:
            return None
    else:
        return None
    if 0 <= parsed <= 0xFFFFFFFF:
        return parsed
    return None


def parse_pe_identity(path: Path) -> PeIdentity:
    """Read a PE image identity.

    Inputs:
    - path: ntoskrnl.exe path.

    Processing:
    - Reads DOS header, NT headers, COFF Machine/TimeDateStamp, and Optional
      Header SizeOfImage. No symbols are loaded.

    Return behavior:
    - Returns PeIdentity on success; raises ValueError/OSError on invalid input.
    """
    data = path.read_bytes()
    if len(data) < 0x100 or data[0:2] != b"MZ":
        raise ValueError(f"not a PE image: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 0x108 >= len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"invalid PE headers: {path}")
    machine, _sections, time_date_stamp = struct.unpack_from("<HHI", data, pe_offset + 4)
    optional_header_offset = pe_offset + 24
    size_of_image = struct.unpack_from("<I", data, optional_header_offset + 56)[0]
    return PeIdentity(machine=machine, time_date_stamp=time_date_stamp, size_of_image=size_of_image)


def field_dictionary_index(field_dictionary: list[Any], names: set[str]) -> int | None:
    """Find one field dictionary index by accepted names.

    Inputs:
    - field_dictionary: pack fieldDictionary array.
    - names: Accepted field spellings.

    Processing:
    - Performs a small linear scan; dictionaries are tiny and deterministic.

    Return behavior:
    - Returns the index on success; returns None when absent.
    """
    for index, item in enumerate(field_dictionary):
        if isinstance(item, str) and item.strip() in names:
            return index
    return None


def extract_field_offsets(profile: dict[str, Any], field_index: int | None) -> list[int]:
    """Extract EpActiveProcessLinks from legacy fields pairs.

    Inputs:
    - profile: Matching compact profile object.
    - field_index: Dictionary index for EpActiveProcessLinks.

    Processing:
    - Scans fields entries of the form [fieldIndex, offset].

    Return behavior:
    - Returns all valid uint32 offsets matching the requested index.
    """
    if field_index is None:
        return []
    offsets: list[int] = []
    for item in profile.get("fields", []):
        if not isinstance(item, list) or len(item) != 2:
            continue
        index = parse_uint32(item[0])
        offset = parse_uint32(item[1])
        if index == field_index and offset is not None:
            offsets.append(offset)
    return offsets


def extract_typed_offsets(profile: dict[str, Any]) -> list[int]:
    """Extract EpActiveProcessLinks from v3 typed items.

    Inputs:
    - profile: Matching compact profile object.

    Processing:
    - Scans items/typedItems for StructOffset entries whose name matches
      EpActiveProcessLinks.

    Return behavior:
    - Returns all valid uint32 typed offsets.
    """
    offsets: list[int] = []
    typed_items = profile.get("items") or profile.get("typedItems") or []
    for item in typed_items:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name", "")).strip()
        kind = str(item.get("kind", "")).strip().lower()
        offset = parse_uint32(item.get("value"))
        if name in ACTIVE_PROCESS_LINKS_NAMES and kind == "structoffset" and offset is not None:
            offsets.append(offset)
    return offsets


def find_matching_profile(pack: dict[str, Any], identity: PeIdentity) -> dict[str, Any] | None:
    """Find the exact ntoskrnl profile for one PE identity.

    Inputs:
    - pack: Parsed compact pack object.
    - identity: Local PE identity.

    Processing:
    - Matches moduleClassId=0 (ntoskrnl), machine, TimeDateStamp, and
      SizeOfImage exactly.

    Return behavior:
    - Returns the matching profile dict; returns None when absent.
    """
    for profile in pack.get("profiles", []):
        if not isinstance(profile, dict):
            continue
        if parse_uint32(profile.get("moduleClassId")) != 0:
            continue
        if parse_uint32(profile.get("machine")) != identity.machine:
            continue
        if parse_uint32(profile.get("timeDateStamp")) != identity.time_date_stamp:
            continue
        if parse_uint32(profile.get("sizeOfImage")) != identity.size_of_image:
            continue
        return profile
    return None


def main() -> int:
    """CLI entry point.

    Inputs:
    - --pack: Compact profile pack path.
    - --kernel: Local ntoskrnl.exe path.

    Processing:
    - Loads pack/kernel identity and audits ActiveProcessLinks coverage.

    Return behavior:
    - Returns process exit code 0 on success; non-zero on missing evidence.
    """
    parser = argparse.ArgumentParser(description="Audit EpActiveProcessLinks coverage in a KswordARK DynData v3 pack.")
    parser.add_argument("--pack", default=str(DEFAULT_PACK), help="Path to ark_dyndata_pack_v3.json.")
    parser.add_argument("--kernel", default=str(DEFAULT_KERNEL), help="Path to local ntoskrnl.exe.")
    args = parser.parse_args()

    pack_path = Path(args.pack)
    kernel_path = Path(args.kernel)
    pack = json.loads(pack_path.read_text(encoding="utf-8"))
    identity = parse_pe_identity(kernel_path)

    print(f"pack={pack_path}")
    print(f"packVersion={pack.get('packVersion')}")
    print(
        f"kernelIdentity=machine=0x{identity.machine:04X}, "
        f"timeDateStamp=0x{identity.time_date_stamp:08X}, sizeOfImage=0x{identity.size_of_image:X}"
    )

    field_dictionary = pack.get("fieldDictionary", [])
    if not isinstance(field_dictionary, list):
        print("ERROR: fieldDictionary is not an array", file=sys.stderr)
        return 2
    field_index = field_dictionary_index(field_dictionary, ACTIVE_PROCESS_LINKS_NAMES)
    print(f"fieldDictionaryIndex={field_index if field_index is not None else '<missing>'}")

    profile = find_matching_profile(pack, identity)
    if profile is None:
        print("ERROR: no exact ntoskrnl profile matches the local kernel identity", file=sys.stderr)
        return 3

    fields_offsets = extract_field_offsets(profile, field_index)
    typed_offsets = extract_typed_offsets(profile)
    usable_offsets = [value for value in [*fields_offsets, *typed_offsets] if value not in UNAVAILABLE_OFFSETS]

    print(f"profileName={profile.get('profileName', '<unnamed>')}")
    print(f"fieldsOffsets={fields_offsets}")
    print(f"typedOffsets={typed_offsets}")
    if not usable_offsets:
        print("ERROR: EpActiveProcessLinks is absent or unavailable in the matching profile", file=sys.stderr)
        return 4

    unique_offsets = sorted(set(usable_offsets))
    print(f"usableActiveProcessLinksOffsets={[f'0x{value:X}' for value in unique_offsets]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
