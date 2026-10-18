"""Normalize CDB `uf` function-discovery output to RVA Capstone listings.

Unlike a contiguous public-symbol range, `uf` can discover jump-linked cold
funclets placed beyond the next public symbol. Every listed instruction is
checked byte-for-byte against the on-disk PE.
"""

from __future__ import annotations

import re
import runpy
from pathlib import Path

import capstone
import pefile

HERE = Path(__file__).resolve().parent
PE = pefile.PE(r"C:\Windows\System32\ntoskrnl.exe")
ns = runpy.run_path(str(HERE / "26100_trace.py"))
sym = ns["sym"]
NAMES = [
    "MmChangeImageProtection",
    "MiLockLoadedDataTableEntryIfNecessary",
    "MiSetImageProtection",
    "MmUnlockLoadedDataTableEntry",
    "MI_IS_PHYSICAL_ADDRESS",
    "MiGetPagePrivilege",
    "MiClearPfnImageVerified",
    "VslValidateDynamicCodePages",
    "MiSetSystemCodeProtection",
    "MiWriteValidPteNewProtection",
    "MiMakeValidPte",
    "MiFlushTbList",
    "VslpEnterIumSecureMode",
    "HvlFlushRangeListTb",
]
BASE = 0x140000000
RE = re.compile(r"^(00000001`[0-9a-f]{8})\s+([0-9a-f]{2,})\s+", re.M | re.I)
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
decoder.detail = True

for name in NAMES:
    text = (HERE / f"26100-uf-{name}.txt").read_text(encoding="utf-8-sig")
    text = text.split("cdb: Reading initial command", 1)[-1]
    insns = {}
    for addr_text, byte_text in RE.findall(text):
        rva = int(addr_text.replace("`", ""), 16) - BASE
        # `uf` can continue past a no-return KeBugCheckEx/int3 into the next
        # adjacent public function. 0x32C640 is the separate
        # MI_IS_PTE_IN_WS_SWAP_SET symbol, with no branch from MiClear to it.
        if name == "MiClearPfnImageVerified" and rva >= 0x32C640:
            continue
        raw = bytes.fromhex(byte_text)
        actual = PE.get_data(rva, len(raw))
        if actual != raw:
            raise RuntimeError(f"CDB/PE bytes disagree at RVA 0x{rva:X} in {name}")
        decoded = list(decoder.disasm(raw, rva, count=1))
        if len(decoded) != 1 or decoded[0].size != len(raw):
            raise RuntimeError(f"Instruction decode failed at RVA 0x{rva:X} in {name}")
        insns[rva] = decoded[0]
    rvas = sorted(insns)
    if not rvas:
        raise RuntimeError(f"No instructions parsed for {name}")
    ranges = []
    start = rvas[0]
    end = start
    for rva in rvas:
        if rva != end:
            ranges.append((start, end))
            start = rva
        end = rva + insns[rva].size
    ranges.append((start, end))
    lines = [
        f"{name}: {len(rvas)} CDB-discovered instructions verified against the local PE",
        "RVA ranges (end exclusive): " + ", ".join(f"0x{a:X}..0x{b:X}" for a, b in ranges),
        "All addresses below are RVAs; direct calls use PDB public-symbol names.",
        "",
    ]
    for rva in rvas:
        insn = insns[rva]
        target = insn.operands[0].imm if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM else None
        note = ""
        if target is not None and (capstone.CS_GRP_CALL in insn.groups or capstone.CS_GRP_JUMP in insn.groups):
            label = f"loc_{target:06X}" if capstone.CS_GRP_JUMP in insn.groups and target in insns else sym(target)
            note = f" ; {label} [RVA 0x{target:X}]"
        lines.append(f"{rva:08X}  {insn.bytes.hex(' '):<47} {insn.mnemonic:<9} {insn.op_str}{note}".rstrip())
    (HERE / f"26100-complete-{name}.asm").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(name, len(rvas), "ranges", [f"0x{a:X}..0x{b:X}" for a,b in ranges])
