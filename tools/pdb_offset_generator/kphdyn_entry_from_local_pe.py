#!/usr/bin/env python3
"""Generate a kphdyn.xml <data> row from a local PE file.

Use case:
- Windows build Y (e.g. 26H2 build 26300) that Winbindex has not indexed yet.
- Operator holds the physical machine's own ntoskrnl.exe (or CI.dll).
- Compute SHA-256, PE TimeDateStamp, PE SizeOfImage, and architecture, emit
  one kphdyn <data> row matching third_party/systeminformer_dyn/kphdyn.xml.

Processing:
- Hash the file with SHA-256.
- Parse the PE headers for TimeDateStamp, SizeOfImage, and Machine.
- Optionally parse CodeView RSDS to display the PDB GUID+Age that the
  symbol server expects for the companion PDB.

Returns:
- Prints a single <data arch=... version=... file=... hash=... timestamp=...
  size=... added=... >1</data> row that can be appended to a kphdyn-extensions
  file consumed by ksword_pdb_profile_generator.py --kphdyn-extra.
- With --append, writes the row to an existing extensions file in-place,
  preserving the surrounding <dyn>...</dyn> structure.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import re
import sys
from pathlib import Path

try:
    import pefile
except ImportError:
    print("pefile is required: pip install pefile", file=sys.stderr)
    sys.exit(2)


MACHINE_TO_ARCH = {
    0x8664: "amd64",
    0xAA64: "arm64",
    0x14C: "x86",
}


def _sha256(path: Path) -> str:
    """Return lowercase SHA-256 hex of file content."""
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _file_version(pe: "pefile.PE") -> str:
    """Return the StringFileInfo FileVersion string, or empty when missing."""
    try:
        for file_info in getattr(pe, "FileInfo", []) or []:
            for entry in file_info:
                if getattr(entry, "Key", None) == b"StringFileInfo":
                    for table in entry.StringTable:
                        for key, value in table.entries.items():
                            if key == b"FileVersion":
                                return value.decode(errors="replace").strip()
    except Exception:
        pass
    return ""


def _codeview_rsds(pe: "pefile.PE") -> dict[str, str] | None:
    """Extract PDB GUID+Age from the PE CodeView debug directory, if any."""
    try:
        for dir_entry in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []) or []:
            info = getattr(dir_entry, "entry", None)
            if info is None:
                continue
            cv_sig = getattr(info, "CvSignature", None)
            if cv_sig != b"RSDS":
                continue
            data6 = bytes(info.Signature_Data6)
            if len(data6) != 6:
                continue
            guid = (
                f"{info.Signature_Data1:08X}{info.Signature_Data2:04X}"
                f"{info.Signature_Data3:04X}"
                f"{info.Signature_Data4:02X}{info.Signature_Data5:02X}"
                + data6.hex().upper()
            )
            age = int(info.Age)
            pdb_name = info.PdbFileName.decode(errors="replace").rstrip("\x00")
            return {
                "guid": guid,
                "age": age,
                "pdb_name": Path(pdb_name).name,
                "symbol_key": f"{guid}{age:X}",
            }
    except Exception:
        pass
    return None


def _normalize_version(version: str) -> str:
    """Keep only the leading 'A.B.C.D' tuple."""
    m = re.match(r"\s*(\d+\.\d+\.\d+\.\d+)", version)
    return m.group(1) if m else version.strip()


def _emit_row(pe_path: Path, *, file_override: str, added: str) -> dict:
    pe = pefile.PE(str(pe_path), fast_load=True)
    pe.parse_data_directories(
        directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"],
        ]
    )
    machine = int(pe.FILE_HEADER.Machine)
    arch = MACHINE_TO_ARCH.get(machine)
    if arch is None:
        raise RuntimeError(f"Unsupported machine type 0x{machine:04X} in {pe_path}")

    tds = int(pe.FILE_HEADER.TimeDateStamp)
    size_of_image = int(pe.OPTIONAL_HEADER.SizeOfImage)
    version = _normalize_version(_file_version(pe))
    if not version:
        raise RuntimeError(f"Could not read FileVersion from {pe_path}")

    sha256 = _sha256(pe_path)
    rsds = _codeview_rsds(pe)
    file_name = file_override or pe_path.name

    row = (
        f'    <data arch="{arch}" version="{version}" file="{file_name}" '
        f'hash="{sha256}" timestamp="0x{tds:08x}" size="0x{size_of_image:08x}" '
        f'added="{added}">1</data>'
    )
    return {
        "arch": arch,
        "version": version,
        "file": file_name,
        "sha256": sha256,
        "timestamp_hex": f"0x{tds:08x}",
        "size_hex": f"0x{size_of_image:08x}",
        "added": added,
        "codeview": rsds,
        "xml_row": row,
    }


def _append_to_extensions(xml_path: Path, row: str) -> None:
    """Insert row just before the closing </dyn> tag, creating the file when absent."""
    if not xml_path.exists():
        xml_path.parent.mkdir(parents=True, exist_ok=True)
        xml_path.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            "<!-- Ksword local-PE additions. -->\n"
            "<dyn>\n"
            f"{row}\n"
            "</dyn>\n",
            encoding="utf-8",
        )
        return
    text = xml_path.read_text(encoding="utf-8")
    closing = re.compile(r"</dyn>\s*$")
    if not closing.search(text):
        raise RuntimeError(f"{xml_path} is not a well-formed <dyn> container")
    xml_path.write_text(closing.sub(f"{row}\n</dyn>\n", text), encoding="utf-8")


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0] if __doc__ else "",
    )
    parser.add_argument("pe", type=Path, help="Path to the local PE file (ntoskrnl.exe, CI.dll, ...)")
    parser.add_argument(
        "--file",
        default="",
        help=(
            "Override the 'file' attribute (useful when the PE on disk is renamed; "
            "defaults to the basename of the PE path)."
        ),
    )
    parser.add_argument(
        "--added",
        default=datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        help="Timestamp for the <data added=...> attribute (defaults to current UTC).",
    )
    parser.add_argument(
        "--append",
        type=Path,
        default=None,
        help="If set, insert the row just before </dyn> in this extensions xml.",
    )
    parser.add_argument(
        "--show-codeview",
        action="store_true",
        help="Print the parsed CodeView RSDS (PDB name/GUID+Age/symbol-server key) for the PDB fetch.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    if not args.pe.exists():
        print(f"PE not found: {args.pe}", file=sys.stderr)
        return 2
    info = _emit_row(args.pe, file_override=args.file, added=args.added)
    print(info["xml_row"])
    if args.show_codeview:
        cv = info.get("codeview")
        if cv:
            print(
                f"# CodeView RSDS: pdb={cv['pdb_name']}  "
                f"GUID={cv['guid']}  Age={cv['age']}  symbol_key={cv['symbol_key']}",
                file=sys.stderr,
            )
            print(
                f"# Public symbol-server PDB URL: "
                f"https://msdl.microsoft.com/download/symbols/{cv['pdb_name']}/{cv['symbol_key']}/{cv['pdb_name']}",
                file=sys.stderr,
            )
        else:
            print("# No CodeView RSDS entry found in the PE.", file=sys.stderr)
    if args.append:
        _append_to_extensions(args.append, info["xml_row"])
        print(f"# appended to {args.append}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
