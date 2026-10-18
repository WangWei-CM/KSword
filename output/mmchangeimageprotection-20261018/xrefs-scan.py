"""Offline PE call-site scan for MmChangeImageProtection (no process access).

Requires pefile and capstone.  The optional DbgHelp lookup is used only for
symbolizing addresses from a matching PDB; the call bytes come from the PE.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import struct
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP


class SymbolInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", ctypes.c_ulong),
        ("TypeIndex", ctypes.c_ulong),
        ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", ctypes.c_ulong),
        ("Size", ctypes.c_ulong),
        ("ModBase", ctypes.c_ulonglong),
        ("Flags", ctypes.c_ulong),
        ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong),
        ("Register", ctypes.c_ulong),
        ("Scope", ctypes.c_ulong),
        ("Tag", ctypes.c_ulong),
        ("NameLen", ctypes.c_ulong),
        ("MaxNameLen", ctypes.c_ulong),
        ("Name", ctypes.c_char * 1),
    ]


class DbgSymbols:
    def __init__(self, pe_path: str, base: int, search_path: str) -> None:
        dll_path = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll"
        self.dll = ctypes.WinDLL(dll_path)
        self.kernel = ctypes.WinDLL("kernel32.dll")
        self.kernel.GetCurrentProcess.restype = ctypes.c_void_p
        self.handle = self.kernel.GetCurrentProcess()
        self.dll.SymSetOptions.argtypes = [ctypes.c_ulong]
        self.dll.SymSetOptions.restype = ctypes.c_ulong
        self.dll.SymInitializeW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
        self.dll.SymInitializeW.restype = ctypes.c_int
        self.dll.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                                             ctypes.c_wchar_p, ctypes.c_ulonglong, ctypes.c_ulong,
                                             ctypes.c_void_p, ctypes.c_ulong]
        self.dll.SymLoadModuleExW.restype = ctypes.c_ulonglong
        self.dll.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                         ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_void_p]
        self.dll.SymFromAddr.restype = ctypes.c_int
        self.dll.SymFromName.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
        self.dll.SymFromName.restype = ctypes.c_int
        self.dll.SymCleanup.argtypes = [ctypes.c_void_p]
        self.dll.SymCleanup.restype = ctypes.c_int
        self.dll.SymSetOptions(0x2 | 0x4 | 0x400 | 0x4000)
        if not self.dll.SymInitializeW(self.handle, search_path, False):
            raise OSError(f"SymInitializeW failed: {ctypes.get_last_error()}")
        loaded = self.dll.SymLoadModuleExW(self.handle, None, pe_path, None, base, 0, None, 0)
        if not loaded:
            raise OSError(f"SymLoadModuleExW failed: {ctypes.get_last_error()}")
        self.base = base

    def _info(self):
        buf = ctypes.create_string_buffer(ctypes.sizeof(SymbolInfo) + 2048)
        ptr = ctypes.cast(buf, ctypes.POINTER(SymbolInfo))
        ptr.contents.SizeOfStruct = ctypes.sizeof(SymbolInfo)
        ptr.contents.MaxNameLen = 2048
        return buf, ptr

    def from_addr(self, rva: int):
        buf, ptr = self._info()
        displacement = ctypes.c_ulonglong()
        ok = self.dll.SymFromAddr(self.handle, self.base + rva,
                                  ctypes.byref(displacement), ptr)
        if not ok:
            return None
        name_address = ctypes.addressof(buf) + SymbolInfo.Name.offset
        name = ctypes.string_at(name_address, ptr.contents.NameLen).decode("utf-8", "replace")
        return {"name": name, "symbol_rva": ptr.contents.Address - self.base,
                "displacement": displacement.value, "size": ptr.contents.Size}

    def from_name(self, name: str):
        buf, ptr = self._info()
        ok = self.dll.SymFromName(self.handle, name.encode(), ptr)
        if not ok:
            return None
        return ptr.contents.Address - self.base

    def close(self):
        self.dll.SymCleanup(self.handle)


def get_codeview(pe: pefile.PE):
    for entry in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        if entry.struct.Type != 2:
            continue
        data = pe.get_data(entry.struct.AddressOfRawData, entry.struct.SizeOfData)
        if data.startswith(b"RSDS") and len(data) >= 25:
            raw_guid = data[4:20]
            import uuid
            return {"guid": str(uuid.UUID(bytes_le=raw_guid)),
                    "age": struct.unpack_from("<I", data, 20)[0],
                    "pdb_name": data[24:].split(b"\0", 1)[0].decode("utf-8", "replace")}
    return None


def get_exports(pe: pefile.PE):
    out = {}
    for sym in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []):
        if sym.name:
            out[sym.name.decode("utf-8", "replace")] = sym.address
    return out


def get_imports(pe: pefile.PE):
    out = []
    for dll in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        module = dll.dll.decode("utf-8", "replace")
        for sym in dll.imports:
            out.append({"module": module, "name": sym.name.decode("utf-8", "replace") if sym.name else None,
                        "ordinal": sym.ordinal, "iat_rva": sym.address - pe.OPTIONAL_HEADER.ImageBase})
    return out


def scan_direct_calls(pe: pefile.PE, target_rva: int):
    # First search every executable byte for E8 + rel32, then require Capstone
    # to decode it as a 5-byte call at the candidate location.  A byte scan
    # alone may find embedded data; caller-boundary review remains necessary.
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    calls = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        start = section.VirtualAddress
        data = section.get_data()
        for off in range(len(data) - 5):
            if data[off] != 0xE8:
                continue
            delta = struct.unpack_from("<i", data, off + 1)[0]
            rva = start + off
            if rva + 5 + delta != target_rva:
                continue
            decoded = list(md.disasm(data[off:off + 5], rva))
            if len(decoded) != 1 or decoded[0].mnemonic != "call" or decoded[0].size != 5:
                continue
            lo = max(0, off - 64)
            hi = min(len(data), off + 32)
            calls.append({"rva": rva, "bytes": data[off:off + 5].hex(),
                          "section": section.Name.rstrip(b"\0").decode("ascii", "replace"),
                          "context_start_rva": start + lo,
                          "context_hex": data[lo:hi].hex(),
                          "alignment_offset": off - lo})
    return calls


def scan_direct_jumps(pe: pefile.PE, target_rva: int):
    result = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        start = section.VirtualAddress
        data = section.get_data()
        for off in range(len(data) - 5):
            if data[off] != 0xE9:
                continue
            rva = start + off
            if rva + 5 + struct.unpack_from("<i", data, off + 1)[0] == target_rva:
                result.append({"rva": rva, "bytes": data[off:off + 5].hex(),
                               "section": section.Name.rstrip(b"\0").decode("ascii", "replace")})
    return result


def scan_absolute_pointers(pe: pefile.PE, target_rva: int):
    pattern = struct.pack("<Q", pe.OPTIONAL_HEADER.ImageBase + target_rva)
    refs = []
    for section in pe.sections:
        data = section.get_data()
        pos = 0
        while (pos := data.find(pattern, pos)) >= 0:
            refs.append({"rva": section.VirtualAddress + pos,
                         "section": section.Name.rstrip(b"\0").decode("ascii", "replace")})
            pos += 1
    return refs


def scan_decoded_rip_references(pe: pefile.PE, target_rva: int):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md.skipdata = True
    refs = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        base = section.VirtualAddress
        for insn in md.disasm(section.get_data(), base):
            if insn.id == 0:
                continue
            for operand in insn.operands:
                if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                    continue
                if insn.address + insn.size + operand.mem.disp == target_rva:
                    refs.append({"rva": insn.address, "bytes": insn.bytes.hex(),
                                 "disasm": f"{insn.mnemonic} {insn.op_str}",
                                 "section": section.Name.rstrip(b"\0").decode("ascii", "replace")})
    return refs


def scan_ci_indirect_calls(pe: pefile.PE, import_rvas: list[int]):
    # Find FF 15 disp32 RIP-relative calls to a specific imported IAT slot.
    result = []
    targets = set(import_rvas)
    for sec in pe.sections:
        if not sec.Characteristics & 0x20000000:
            continue
        base = sec.VirtualAddress
        data = sec.get_data()
        for off in range(len(data) - 6):
            if data[off:off + 2] != b"\xff\x15":
                continue
            disp = struct.unpack_from("<i", data, off + 2)[0]
            site = base + off
            iat = site + 6 + disp
            if iat in targets:
                result.append({"rva": site, "iat_rva": iat,
                               "bytes": data[off:off + 6].hex()})
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pe")
    parser.add_argument("--target-rva", type=lambda x: int(x, 0))
    parser.add_argument("--symbol-path", default="")
    parser.add_argument("--out")
    args = parser.parse_args()
    path = Path(args.pe)
    pe = pefile.PE(str(path), fast_load=False)
    data = path.read_bytes()
    base = pe.OPTIONAL_HEADER.ImageBase
    exports = get_exports(pe)
    target = args.target_rva or exports.get("MmChangeImageProtection")
    imports = get_imports(pe)
    result = {"path": str(path.resolve()), "sha256": hashlib.sha256(data).hexdigest(),
              "file_size": len(data), "image_base": base,
              "timestamp": pe.FILE_HEADER.TimeDateStamp,
              "codeview": get_codeview(pe),
              "target_rva": target,
              "exports_MmChangeImageProtection": exports.get("MmChangeImageProtection"),
              "imports_MmChangeImageProtection": [x for x in imports if x["name"] == "MmChangeImageProtection"],
              "imports_ntoskrnl_count": sum(x["module"].lower() in ("ntoskrnl.exe", "ntkrnlmp.exe") for x in imports),
              "name_string_offsets": {
                  "ascii": data.find(b"MmChangeImageProtection"),
                  "utf16le": data.find("MmChangeImageProtection".encode("utf-16le"))},
              "direct_calls": scan_direct_calls(pe, target) if target is not None else [],
              "direct_jumps": scan_direct_jumps(pe, target) if target is not None else [],
              "absolute_pointers": scan_absolute_pointers(pe, target) if target is not None else [],
              "rip_references": scan_decoded_rip_references(pe, target) if target is not None else []}
    syms = None
    if args.symbol_path:
        try:
            syms = DbgSymbols(str(path.resolve()), base, args.symbol_path)
            result["symbol_lookup"] = {"target_by_name": syms.from_name("MmChangeImageProtection")}
            for call in result["direct_calls"]:
                call["nearest_symbol"] = syms.from_addr(call["rva"])
        except Exception as exc:
            result["symbol_lookup"] = {"error": str(exc)}
        finally:
            if syms:
                syms.close()
    if args.out:
        Path(args.out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
