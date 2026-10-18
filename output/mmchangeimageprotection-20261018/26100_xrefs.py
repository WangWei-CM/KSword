"""Find verified direct near-call xrefs to 26100 MmChangeImageProtection."""

import bisect
import json
import runpy
import struct
from pathlib import Path

import capstone
import pefile

HERE = Path(__file__).resolve().parent
PE = pefile.PE(r"C:\Windows\System32\ntoskrnl.exe")
TARGET = 0xA62300
ns = runpy.run_path(str(HERE / "26100_trace.py"))
sym = ns["sym"]
unwind = ns["unwind"]
begins = ns["begins"]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

records = []
raw_candidates = []
for section in PE.sections:
    if not section.Characteristics & 0x20000000:
        continue
    data = section.get_data()
    pos = data.find(b"\xe8")
    while pos >= 0 and pos + 5 <= len(data):
        site = section.VirtualAddress + pos
        displacement = struct.unpack_from("<i", data, pos + 1)[0]
        if site + 5 + displacement == TARGET:
            raw_candidates.append(site)
            idx = bisect.bisect_right(begins, site) - 1
            if idx >= 0 and site < unwind[idx][1]:
                start = unwind[idx][0]
                insns = list(decoder.disasm(PE.get_data(start, site + 5 - start), start))
                if insns and insns[-1].address == site and insns[-1].mnemonic == "call":
                    context = [
                        f"0x{x.address:X}: {x.bytes.hex(' ')}  {x.mnemonic} {x.op_str}"
                        for x in insns[-22:]
                    ]
                    records.append({
                        "call_site_rva": f"0x{site:X}",
                        "enclosing_pdata_start_rva": f"0x{start:X}",
                        "nearest_public_symbol": sym(start),
                        "preceding_instructions": context,
                    })
        pos = data.find(b"\xe8", pos + 1)

(HERE / "26100-nt-xrefs.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
print(f"verified direct near-call xrefs: {len(records)}")
print(f"raw E8 rel32 candidates: {len(raw_candidates)}")
for row in records:
    print(row["call_site_rva"], row["nearest_public_symbol"])
