"""Normalize WinDbg's offline PE disassembly to RVA-only evidence.

Input is a local CDB `uf` transcript.  This script is deliberately limited to
format conversion; it does not infer behavior from function or symbol names.
"""

from pathlib import Path
import re
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "22621-cdb-uf.txt"
IMAGE_BASE = 0x140000000
PATTERN = re.compile(r"^([0-9a-f]{8})[^0-9a-f]([0-9a-f]{8})\s+([0-9a-f]+)\s+(.+)$", re.I)
TARGET_VA = re.compile(r"\(([0-9a-f]{8})[^0-9a-f]([0-9a-f]{8})\)", re.I)


def rva_asm(asm: str) -> str:
    def replace(match: re.Match[str]) -> str:
        rva = int(match.group(1) + match.group(2), 16) - IMAGE_BASE
        return f"(RVA 0x{rva:X})"

    return TARGET_VA.sub(replace, asm)

rows = []
for line in SOURCE.read_text(encoding="utf-8").splitlines():
    match = PATTERN.match(line)
    if not match:
        continue
    va = int(match.group(1) + match.group(2), 16)
    rva = va - IMAGE_BASE
    if not (0x736CF0 <= rva < 0x736F9B or 0x8C908E <= rva < 0x8C912C):
        continue
    raw, asm = match.group(3), rva_asm(match.group(4))
    rows.append((rva, raw, asm))

# CDB `uf` walks reachable basic blocks and skips this single alignment int3.
rows.insert(next(i for i, (rva, _, _) in enumerate(rows) if rva == 0x736F48),
            (0x736F47, "cc", "int3    ; alignment byte omitted by CDB uf"))

with (ROOT / "22621-complete-disassembly.txt").open("w", encoding="utf-8") as stream:
    stream.write("Build 22621.4317 nt!MmChangeImageProtection\n")
    stream.write("PE image base 0x140000000, all addresses below are RVA.\n")
    stream.write("Hot RVA 0x736CF0..0x736F9A; cold RVA 0x8C908E..0x8C912B.\n")
    stream.write("Source: cdb -z exact local PE -y exact local PDB store; uf ntkrnlmp!MmChangeImageProtection.\n\n")
    for rva, raw, asm in rows:
        stream.write(f"{rva:08X}  {raw:<24} {asm}\n")

calls = [(rva, asm) for rva, _, asm in rows if asm.startswith("call")]
with (ROOT / "22621-calls.tsv").open("w", encoding="utf-8") as stream:
    stream.write("RVA\tCDB symbol and target RVA\n")
    for rva, asm in calls:
        stream.write(f"0x{rva:X}\t{asm}\n")

print(f"instructions={len(rows)} hot={sum(0x736CF0 <= r < 0x736F9B for r, _, _ in rows)} cold={sum(0x8C908E <= r < 0x8C912C for r, _, _ in rows)} calls={len(calls)}")
for rva, asm in calls:
    print(f"0x{rva:X} {asm}")

# Scan executable PE section bytes for direct x86-64 CALL rel32 references.
# Candidates are later checked against CDB disassembly at each caller site.
pe = pefile.PE(str(ROOT.parents[1] / "artifacts/symbols/ntkrnlmp.exe/2C33C5081047000/ntkrnlmp.exe"))
for rva, raw, _ in rows:
    assert pe.get_data(rva, len(raw) // 2).hex().lower() == raw.lower(), f"CDB bytes disagree with PE at 0x{rva:X}"
md = Cs(CS_ARCH_X86, CS_MODE_64)
for begin, end in ((0x736CF0, 0x736F9B), (0x8C908E, 0x8C912C)):
    code = pe.get_data(begin, end - begin)
    decoded = list(md.disasm(code, begin))
    assert sum(ins.size for ins in decoded) == end - begin, f"Capstone did not cover RVA 0x{begin:X}..0x{end-1:X}"
    cdb_addresses = {rva for rva, _, _ in rows if begin <= rva < end}
    capstone_addresses = {ins.address for ins in decoded}
    assert cdb_addresses == capstone_addresses, f"CDB/Capstone instruction boundaries differ in 0x{begin:X}..0x{end-1:X}"
    print(f"PE bytes and Capstone instruction boundaries match CDB at RVA 0x{begin:X}..0x{end-1:X}")
target = 0x736CF0
callers = []
for section in pe.sections:
    if not section.Characteristics & 0x20000000:
        continue
    data = section.get_data()
    base = section.VirtualAddress
    for index in range(len(data) - 4):
        if data[index] != 0xE8:
            continue
        displacement = struct.unpack_from("<i", data, index + 1)[0]
        call_rva = base + index
        if call_rva + 5 + displacement == target:
            callers.append((call_rva, section.Name.rstrip(b"\0").decode(errors="replace")))

with (ROOT / "22621-direct-call-candidates.txt").open("w", encoding="utf-8") as stream:
    stream.write("Raw E8 rel32 candidates in executable sections targeting RVA 0x736CF0.\n")
    stream.write("Instruction boundaries and symbols require separate CDB verification.\n")
    for call_rva, section in callers:
        stream.write(f"0x{call_rva:X} {section}\n")
print("direct call candidates:", [(hex(rva), section) for rva, section in callers])

for source in sorted(ROOT.glob("22621-*-cdb-uf.txt")):
    if source.name == "22621-cdb-uf.txt":
        continue
    text = source.read_text(encoding="utf-8").splitlines()
    start = next((i + 1 for i, line in enumerate(text) if line.startswith("0:000> cdb: Reading initial command")), len(text))
    child_rows = []
    for line in text[start:]:
        match = PATTERN.match(line)
        if match:
            child_rows.append((int(match.group(1) + match.group(2), 16) - IMAGE_BASE, match.group(3), rva_asm(match.group(4))))
    if not child_rows:
        continue
    outfile = source.with_name(source.name.replace("-cdb-uf.txt", "-first200.txt"))
    with outfile.open("w", encoding="utf-8") as stream:
        stream.write(f"Source: {source.name}; first 200 instructions in CDB uf traversal order. All addresses RVA.\n")
        stream.write(f"CDB uf instruction count={len(child_rows)}; listed={min(200, len(child_rows))}\n")
        for rva, raw, asm in child_rows[:200]:
            stream.write(f"{rva:08X}  {raw:<24} {asm}\n")
    print(source.name, "instructions", len(child_rows), "calls_first200", [(hex(rva), asm) for rva, _, asm in child_rows[:200] if asm.startswith("call")])
