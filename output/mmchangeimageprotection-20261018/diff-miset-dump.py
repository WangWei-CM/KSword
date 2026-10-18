"""Disassemble the full 26100 MiSetSystemCodeProtection extent for diff review."""

import importlib.util
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

base = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("xrefs_scan", base / "xrefs-scan.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

path = Path(r"C:\Windows\System32\ntoskrnl.exe")
pe = pefile.PE(str(path))
start, end = 0x2C5A30, 0x2C6230
symdir = base / "symbols"
symbols = module.DbgSymbols(str(path), pe.OPTIONAL_HEADER.ImageBase, str(symdir))
md = Cs(CS_ARCH_X86, CS_MODE_64)
lines = []
for insn in md.disasm(pe.get_data(start, end - start), start):
    annotation = ""
    if insn.mnemonic == "call" and insn.op_str.startswith("0x"):
        target = int(insn.op_str, 16)
        symbol = symbols.from_addr(target)
        if symbol and symbol["displacement"] == 0:
            annotation = f" ; {symbol['name']} [RVA 0x{target:X}]"
        else:
            annotation = f" ; sub_{target:X}"
    lines.append(f"{insn.address:08X}  {insn.bytes.hex():<24} {insn.mnemonic:<8} {insn.op_str}{annotation}")
symbols.close()
(base / "diff-26100-miset-full.asm").write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"wrote {len(lines)} decoded instructions")
