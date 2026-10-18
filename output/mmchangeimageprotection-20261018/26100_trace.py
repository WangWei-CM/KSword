"""Trace the first 200 instructions of each direct callee in the 26100 kernel PE."""

from __future__ import annotations

import bisect
import json
import re
from pathlib import Path

import capstone
import pefile

HERE = Path(__file__).resolve().parent
PE = pefile.PE(r"C:\Windows\System32\ntoskrnl.exe")
SYMBOL_BASE = 0x1000000
TARGETS = [
    0x27C2D8,  # MiLockLoadedDataTableEntryIfNecessary
    0x2C59E0,  # MiSetImageProtection
    0x27BC50,  # MmUnlockLoadedDataTableEntry
    0x2FE1B0,  # MI_IS_PHYSICAL_ADDRESS
    0x299200,  # MiGetPagePrivilege
    0x32C4A0,  # MiClearPfnImageVerified
    0xA6258C,  # VslValidateDynamicCodePages
    0x2C5A30,  # MiSetSystemCodeProtection
    0x32C6F4,  # VslRemoveProtectedPage
    0x32C774,  # VslMakeProtectedPageExecutable
    0x2F8C60,  # VslpEnterIumSecureMode
    0x20B30C,  # VslpLockPagesForTransfer
    0x20BBB8,  # VslpUnlockPagesForTransfer
    0x34D830,  # MiGetTopLevelPfn
    0x34D7C0,  # MiGetPageTablePfnBuddyRaw
    0x367550,  # MiMakeValidPte
    0x2955F0,  # MiWriteValidPteNewProtection
    0x22C9D0,  # MiFlushTbList
    0x2C49D4,  # MiMarkPfnVerified
    0x2B1440,  # MiCaptureDirtyBitToPfn
    0x6A6930,  # HvlSwitchToVsmVtl1
    0x6A6AA0,  # VslpDispatchIumSyscall
    0x229EA0,  # KiCheckVpBackingLongSpinWaitHypercall
    0x229E70,  # HvlNotifyLongSpinWait
    0x6A6A80,  # HvlpSwitchToVsmVtl1RetpolineHelper
    0x2A7450,  # MiInsertTbFlushEntry
    0x310240,  # MiOffsetToProtos
    0x296680,  # MI_READ_PTE_LOCK_FREE
    0x2B3080,  # MiInvalidPteConforms
    0x4FD1E8,  # MiRestrictSystemCodeProtection
    0x22D5E0,  # VmFlushTb
    0x22D544,  # ExFlushTb
    0x22C5E0,  # VmpFlushTb
    0x22D620,  # HvlFlushRangeListTb
    0x22BDD0,  # KiIpiSendRequest
    0x22C480,  # KiFlushRangeWorker
    0x3324F4,  # VslGetNestedPageProtectionFlags
    0x22E040,  # HvcallInitiateHypercall
    0x22DAF0,  # HvcallFastExtended
    0x3DB4E0,  # HvlpFlushRangeListTbEx
    0x22F754,  # VslSlowFlushSecureRangeList
    0x6B5870,  # HvcallpExtendedFastHypercall
    0x6B58D0,  # HvcallpExtendedFastHypercallWithOutput
]

names: dict[int, list[str]] = {}
pattern = re.compile(r"^\s*[0-9a-f]+\s+([0-9a-f]+)\s+:\s+(.+?)\s*$", re.I)
for line in (HERE / "26100-symbols-raw.txt").read_text(encoding="utf-8-sig").splitlines():
    m = pattern.match(line)
    if m:
        rva = int(m.group(1), 16) - SYMBOL_BASE
        if rva >= 0:
            names.setdefault(rva, []).append(m.group(2))
symbols = sorted(names)
unwind = sorted((x.struct.BeginAddress, x.struct.EndAddress) for x in PE.DIRECTORY_ENTRY_EXCEPTION)
begins = [x[0] for x in unwind]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
decoder.detail = True


def sym(rva: int) -> str:
    if rva in names:
        return "/".join(names[rva])
    idx = bisect.bisect_right(symbols, rva) - 1
    if idx >= 0 and rva - symbols[idx] < 0x100:
        return f"{'/'.join(names[symbols[idx]])}+0x{rva-symbols[idx]:X}"
    return f"sub_{rva:06X}"


def extent(rva: int) -> tuple[int, int] | None:
    idx = bisect.bisect_right(begins, rva) - 1
    if idx >= 0 and rva < unwind[idx][1]:
        return unwind[idx]
    return None


def decode(target: int) -> dict:
    bounds = extent(target)
    # Compiler emits multiple adjacent .pdata entries for one public function
    # (e.g. MiGetPagePrivilege and MiClearPfnImageVerified). A single unwind
    # interval is therefore insufficient. Bound at the next public symbol,
    # with a conservative 0x3000-byte cap; show at most 200 instructions.
    next_idx = bisect.bisect_right(symbols, target)
    next_public = symbols[next_idx] if next_idx < len(symbols) else target + 0x3000
    end = min(next_public, target + 0x3000)
    source = f"next public symbol (first .pdata end 0x{bounds[1]:X})" if bounds else "next public symbol"
    insns = list(decoder.disasm(PE.get_data(target, end - target), target, count=200))
    out = [
        f"{sym(target)} at RVA 0x{target:X}",
        f"extent source: {source}; RVA 0x{target:X}..0x{end:X} (exclusive)",
        f"shown: {len(insns)} instruction(s), max 200",
        "All addresses are RVAs.",
        "",
    ]
    calls = []
    for insn in insns:
        callee = None
        if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            callee = insn.operands[0].imm
        elif (
            insn.operands
            and insn.operands[0].type == capstone.x86.X86_OP_MEM
            and insn.operands[0].mem.base == capstone.x86.X86_REG_RIP
        ):
            callee = insn.address + insn.size + insn.operands[0].mem.disp
        note = ""
        if callee is not None and (
            capstone.CS_GRP_CALL in insn.groups or capstone.CS_GRP_JUMP in insn.groups
        ):
            label = f"loc_{callee:06X}" if capstone.CS_GRP_JUMP in insn.groups and target <= callee < end else sym(callee)
            note = f" ; {label} [RVA 0x{callee:X}]"
        out.append(
            f"{insn.address:08X}  {insn.bytes.hex(' '):<47} {insn.mnemonic:<9} {insn.op_str}{note}".rstrip()
        )
        if capstone.CS_GRP_CALL in insn.groups:
            calls.append(
                {
                    "site_rva": f"0x{insn.address:X}",
                    "bytes": insn.bytes.hex(),
                    "instruction": f"{insn.mnemonic} {insn.op_str}",
                    "target_rva": f"0x{callee:X}" if callee is not None else None,
                    "target_symbol": sym(callee) if callee is not None else None,
                }
            )
    label = sym(target).split("/")[0].replace("?", "_").replace("*", "_")
    (HERE / f"26100-callee-{target:06X}-{label}.asm").write_text("\n".join(out) + "\n", encoding="utf-8")
    return {
        "rva": f"0x{target:X}",
        "symbol": sym(target),
        "extent_end_rva": f"0x{end:X}",
        "extent_source": source,
        "instructions_shown": len(insns),
        "all_instructions_shown": bool(insns and insns[-1].address + insns[-1].size == end),
        "calls": calls,
    }


if __name__ == "__main__":
    results = [decode(rva) for rva in TARGETS]
    (HERE / "26100-callee-index.json").write_text(
        json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    for result in results:
        print(
            result["rva"], result["symbol"],
            f"instructions={result['instructions_shown']}",
            f"all={result['all_instructions_shown']}",
            f"calls={len(result['calls'])}",
        )
    for target in (0x2C5A30, 0x22C9D0, 0x2F8C60):
        next_idx = bisect.bisect_right(symbols, target)
        end = symbols[next_idx]
        rows = [f"{sym(target)} full contiguous public-symbol span RVA 0x{target:X}..0x{end:X}", ""]
        for insn in decoder.disasm(PE.get_data(target, end-target), target):
            called = None
            if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
                called = insn.operands[0].imm
            note = f" ; {sym(called)} [RVA 0x{called:X}]" if called is not None and capstone.CS_GRP_CALL in insn.groups else ""
            rows.append(f"{insn.address:08X}  {insn.bytes.hex(' '):<47} {insn.mnemonic:<9} {insn.op_str}{note}".rstrip())
        (HERE / f"26100-full-{target:06X}-{sym(target)}.asm").write_text("\n".join(rows)+"\n",encoding="utf-8")
