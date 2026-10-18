"""Offline instruction and symbol extraction for the exact 26100.9022 kernel PE.

The script reads only the local PE and a `dbh ... "x *"` public-symbol listing.
Addresses in the generated files are PE RVAs.
"""

from __future__ import annotations

import bisect
import json
import re
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_PATH = Path(r"C:\Windows\System32\ntoskrnl.exe")
SYMBOLS_PATH = HERE / "26100-symbols-raw.txt"
BASE = 0x1000000  # dbh's default module base, not the PE's image base
TARGET = 0xA62300


def load_symbols() -> tuple[dict[int, list[str]], list[int]]:
    names: dict[int, list[str]] = {}
    pattern = re.compile(r"^\s*[0-9a-f]+\s+([0-9a-f]+)\s+:\s+(.+?)\s*$", re.I)
    for line in SYMBOLS_PATH.read_text(encoding="utf-8-sig").splitlines():
        match = pattern.match(line)
        if match:
            rva = int(match.group(1), 16) - BASE
            if rva >= 0:
                names.setdefault(rva, []).append(match.group(2))
    return names, sorted(names)


def main() -> None:
    pe = pefile.PE(str(PE_PATH))
    names, symbol_rvas = load_symbols()
    unwind = sorted(
        (entry.struct.BeginAddress, entry.struct.EndAddress)
        for entry in pe.DIRECTORY_ENTRY_EXCEPTION
    )
    begins = [begin for begin, _ in unwind]

    def function_interval(rva: int) -> tuple[int, int] | None:
        idx = bisect.bisect_right(begins, rva) - 1
        if idx >= 0 and rva < unwind[idx][1]:
            return unwind[idx]
        return None

    def symbol(rva: int) -> str:
        if rva in names:
            return "/".join(names[rva])
        idx = bisect.bisect_right(symbol_rvas, rva) - 1
        if idx >= 0 and rva - symbol_rvas[idx] < 0x100:
            return f"{'/'.join(names[symbol_rvas[idx]])}+0x{rva-symbol_rvas[idx]:X}"
        return f"sub_{rva:06X}"

    target_interval = function_interval(TARGET)
    if not target_interval or target_interval[0] != TARGET:
        raise RuntimeError("Target is not an exact .pdata function start")
    start, end = target_interval
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    code = pe.get_data(start, end - start)
    instructions = list(md.disasm(code, start))
    if not instructions or instructions[-1].address + instructions[-1].size != end:
        raise RuntimeError("Disassembly did not consume full .pdata range")

    lines = [
        f"PE: {PE_PATH}",
        f"export: {symbol(start)} at RVA 0x{start:X}",
        f".pdata extent: RVA 0x{start:X}..0x{end:X} (exclusive), size 0x{end-start:X}",
        "All addresses below are RVAs.",
        "",
    ]
    calls = []
    branches = []
    for insn in instructions:
        annotation = ""
        target_rva = None
        if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            target_rva = insn.operands[0].imm
        elif (
            insn.operands
            and insn.operands[0].type == capstone.x86.X86_OP_MEM
            and insn.operands[0].mem.base == capstone.x86.X86_REG_RIP
        ):
            target_rva = insn.address + insn.size + insn.operands[0].mem.disp
        if target_rva is not None and (
            capstone.CS_GRP_CALL in insn.groups or capstone.CS_GRP_JUMP in insn.groups
        ):
            if capstone.CS_GRP_JUMP in insn.groups and start <= target_rva < end:
                target_name = f"loc_{target_rva:06X}"
            else:
                target_name = symbol(target_rva)
            annotation = f" ; {target_name} [RVA 0x{target_rva:X}]"
        line = f"{insn.address:08X}  {insn.bytes.hex(' '):<47} {insn.mnemonic:<9} {insn.op_str}{annotation}"
        lines.append(line.rstrip())
        record = {
            "site_rva": f"0x{insn.address:X}",
            "instruction": f"{insn.mnemonic} {insn.op_str}",
            "bytes": insn.bytes.hex(),
            "target_rva": f"0x{target_rva:X}" if target_rva is not None else None,
            "target_symbol": (
                f"loc_{target_rva:06X}"
                if target_rva is not None and capstone.CS_GRP_JUMP in insn.groups and start <= target_rva < end
                else symbol(target_rva) if target_rva is not None else None
            ),
        }
        if capstone.CS_GRP_CALL in insn.groups:
            calls.append(record)
        elif capstone.CS_GRP_JUMP in insn.groups and insn.mnemonic != "jmp":
            branches.append(record)

    (HERE / "26100-mmchange.asm").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (HERE / "26100-mmchange-calls.json").write_text(
        json.dumps(calls, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    (HERE / "26100-mmchange-branches.json").write_text(
        json.dumps(branches, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"instructions={len(instructions)} calls={len(calls)} branches={len(branches)}")
    print(f"PDB_public_symbols={sum(len(x) for x in names.values())}")


if __name__ == "__main__":
    main()
