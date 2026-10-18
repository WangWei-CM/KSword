# Build 26100.9022: `nt!MmChangeImageProtection` instruction analysis

All code locations below are **RVAs** in the local `C:\Windows\System32\ntoskrnl.exe`. Function behavior is derived from that PE's instructions and its matching Microsoft public PDB, not from a published description. Field meanings beyond PDB type names are marked as inferences.

## Identity and coverage

| Item | Directly observed value |
| --- | --- |
| PE file version | `10.0.26100.9022` |
| PE SHA-256 | `CB0603CD0BC46BFB2D94963E8A525C3C78BA90E3739E09DEEB3185FC71613C7B` |
| PE image base / size | `0x140000000` / `0x1450000` |
| CodeView PDB identity | `ntkrnlmp.pdb`, RSDS `9DAAB201-5E4C-ABF6-9800-9DBD0548B669`, age `1` |
| Matched PDB | Local symbol cache `symbols/ntkrnlmp.pdb/9DAAB2015E4CABF698009DBD0548B6691/ntkrnlmp.pdb` (not committed), SHA-256 `2BC49E428E6AA1AF0B00B8D054997CBA5689895836FEBD68300B272D84673F17` |
| Export and `.pdata` | `MmChangeImageProtection` RVA `0xA62300`, range `0xA62300..0xA62585` (exclusive), size `0x285` |
| Complete function | [158-entry RVA disassembly with bytes and PDB call symbols](26100-mmchange.asm); [CDB `uf` control-flow output](26100-uf-MmChangeImageProtection.txt) finds 157 CFG instructions, excluding the alignment `int3` at `0xA6241E` |

The Capstone listing consumes every byte in the `.pdata` range. Every direct branch target is within that range. CDB `uf` found no external cold block for this function. The [CDB-derived reachable listing](26100-complete-MmChangeImageProtection.asm) was checked byte-for-byte against the PE. IDA Hex-Rays was unavailable because the installed IDA reported `Cannot continue without a valid license`; the pseudocode below is **manual reconstruction**, not decompiler output.

## Parameter signature and full path reconstruction

The matched PDB's [`_MDL` layout](26100-pdb-mdl.txt) places `MdlFlags` at `+0x0A`, `StartVa` at `+0x20`, `ByteCount` at `+0x28`, `ByteOffset` at `+0x2C`, and the PFN array after the 0x30-byte header. The function reads exactly those offsets at `0xA62346`, `0xA62353`, `0xA6235D`, `0xA6236C`, `0xA6249D`. Thus `RCX` is a `PMDL`. At `0xA6230F`, it saves `R9D` in the fourth-argument home slot; `0xA623A6` reads that same slot at `[rsp+0x78]`. No fifth or sixth argument is read. `RDX` is a checked range start, `R8` its length, and `R9D` is a mode restricted to 1 or 2. `0xA6233A..0xA62340` rejects nonzero high 32 bits of `R8`; whether the source declaration says `SIZE_T` or `ULONG` cannot be established from that instruction alone.

An instruction-supported approximate prototype is:

```c
NTSTATUS MmChangeImageProtection(PMDL mdl, void *rangeStart,
                                 size_t rangeLength, unsigned mode);
```

Manual pseudocode (the loaded-entry field names are descriptive placeholders):

```c
NTSTATUS MmChangeImageProtection(PMDL mdl, void *rangeStart,
                                 size_t rangeLength, unsigned mode)
{
    if (mode != 1 && mode != 2) return 0xC000000D;       // A62327..34
    if (rangeLength != (uint32_t)rangeLength ||
        (mdl->MdlFlags & 7) != 2 || mdl->ByteOffset != 0 ||
        (mdl->ByteCount & 0xFFF) != 0)
        return 0xC000000D;                               // A6233A..66

    pageCount = mdl->ByteCount >> 12;                    // A6235D..75
    entry = MiLockLoadedDataTableEntryIfNecessary(mdl->StartVa,
                                                   &lockState); // A6236C..7D
    if (!entry) return 0xC0000225;                       // A62380..8E
    moduleBase = entry->field_30;
    moduleEnd = moduleBase + entry->field_40;
    rangeEnd = rangeStart + rangeLength;
    if (mdl->StartVa + mdl->ByteCount > moduleEnd ||
        rangeStart < moduleBase || rangeEnd > moduleEnd ||
        rangeEnd - 1 < rangeStart) {
        status = 0xC0000018; goto unlock_only;            // A6241F..56
    }
    if (entry->field_68 & 0x80000) {
        status = 0xC0000043; goto unlock_only;            // A6245C..66
    }
    entry->field_68 |= 0x80000;                           // A6246C..73
    status = 0;
    if (MI_IS_PHYSICAL_ADDRESS(moduleBase)) {
        status = 0xC0000018; goto clear_busy;             // A62470..7D
    }
    for (i = 0; i < pageCount; ++i) {
        pfn = (MMPFN *)(0xFFFFDE0000000000 + 48 * mdl->PfnArray[i]);
        if ((int64_t)pfn->qword_28 < 0) {
            status = 0xC0000018; goto clear_busy;         // A6249D..B6
        }
        if (MiGetPagePrivilege(pfn, 0, NULL) & 0x40) {
            status = 0xC0000045; goto clear_busy;         // A624BC..C8
        }
    }
    MiSetImageProtection(entry, mdl->StartVa, mdl->ByteCount, 1); // A62390..A6
    if (mode == 1) {
        if (MiFlags & 0x4000) {                             // A623B1..BC
            status = VslValidateDynamicCodePages(
                mdl, rangeStart, (uint32_t)rangeLength);   // A62522..30
            if (status < 0) goto clear_busy;               // A62532..34
        }
        finalProtection = (MiFlags & 0x8000) && status == 0x12C
                        ? 0x13 : 3;                        // A623C2..D3,A62549..58
        MiSetImageProtection(entry, mdl->StartVa,
                             mdl->ByteCount, finalProtection); // A623D9..E9
        status = 0;
    } else if ((MiFlags & 0x8000) && pageCount) {          // A624D3..E6
        for (i = 0; i < pageCount; ++i)
            MiClearPfnImageVerified(
                (MMPFN *)(0xFFFFDE0000000000 + 48 * mdl->PfnArray[i]), 9);
                                                            // A624F8..A6251B
    }
clear_busy:
    entry->field_68 &= ~0x80000;                          // A623EB
unlock_only:
    if (lockState) MmUnlockLoadedDataTableEntry(entry);   // A623F0..FA
    return status;                                          // A623FF..A6241D
}
```

`MiSetImageProtection` has no status check in this caller. A negative VSL result branches directly to `clear_busy` after the first protection call (`0xA62532..34`); no second protection call is made on that path. This is a control-flow fact, not a measurement of the final runtime PTE after cleanup. The special nonnegative result `0x12C` selects internal value `0x13` through `cmp ebx,0x12c; cmove r9d,eax` at `0xA62549..58`; the meaning of that result is not established by this VTL0 image.

## Every conditional branch in `MmChangeImageProtection`

The target column is the **taken** branch; the other condition falls through. All branch instructions and bytes are in [the complete listing](26100-mmchange.asm) and [machine-readable branch index](26100-mmchange-branches.json).

| Branch RVA | Taken predicate from preceding instructions | Taken target and effect |
| --- | --- | --- |
| `0xA62334` | `(mode - 1) > 1`, unsigned | `0xA62567`; `0xC000000D` |
| `0xA62340` | `rangeLength != zeroExtend32(rangeLength)` | `0xA62567`; `0xC000000D` |
| `0xA6234D` | `(mdl->MdlFlags & 7) != 2` | `0xA62567`; `0xC000000D` |
| `0xA62357` | `mdl->ByteOffset != 0` | `0xA62567`; `0xC000000D` |
| `0xA62366` | `mdl->ByteCount & 0xFFF` | `0xA62567`; `0xC000000D` |
| `0xA62383` | loaded table entry pointer is nonnull | `0xA6241F`; continue |
| `0xA623AB` | saved `mode != 1` | `0xA624D3`; mode 2 path |
| `0xA623BC` | `MiFlags & 0x4000` | `0xA62522`; call VSL validator |
| `0xA623D3` | `MiFlags & 0x8000` | `0xA62549`; test validator result for `0x12C` |
| `0xA623F5` | `lockState == 0` | `0xA623FF`; omit unlock call |
| `0xA62437` | `mdl->StartVa + ByteCount > moduleEnd` | `0xA6255D`; `0xC0000018` |
| `0xA62440` | `rangeStart < moduleBase` | `0xA6255D`; `0xC0000018` |
| `0xA62449` | `rangeStart + rangeLength > moduleEnd` | `0xA6255D`; `0xC0000018` |
| `0xA62456` | `rangeEnd - 1 < rangeStart`, unsigned | `0xA6255D`; `0xC0000018` |
| `0xA62466` | `entry->field_68 & 0x80000` | `0xA6253F`; `0xC0000043` |
| `0xA6247D` | `MI_IS_PHYSICAL_ADDRESS(moduleBase)` nonzero | `0xA62571`; `0xC0000018` |
| `0xA62497` | `i >= pageCount`, unsigned | `0xA62390`; start protection change |
| `0xA624B6` | PFN qword at `+0x28` has sign bit | `0xA62571`; `0xC0000018` |
| `0xA624C8` | `MiGetPagePrivilege(...) & 0x40` | `0xA6257B`; `0xC0000045` |
| `0xA624DE` | `(MiFlags & 0x8000) == 0` in mode 2 | `0xA623EB`; skip PFN clear |
| `0xA624E6` | `pageCount == 0` in mode 2 | `0xA623EB`; skip PFN clear |
| `0xA6251B` | decremented PFN count nonzero | `0xA624F8`; repeat clear |
| `0xA62534` | VSL result is negative | `0xA623EB`; return error after cleanup |

## All eight direct calls in the function

| Call RVA | PDB target | Target RVA | Direct role and category |
| --- | --- | --- | --- |
| `0xA62378` | `MiLockLoadedDataTableEntryIfNecessary` | `0x27C2D8` | Map MDL image VA to loaded entry and report lock state |
| `0xA623A1` | `MiSetImageProtection` | `0x2C59E0` | First image PTE protection value `1` |
| `0xA623E4` | `MiSetImageProtection` | `0x2C59E0` | Mode 1 second protection value `3` or `0x13` |
| `0xA623FA` | `MmUnlockLoadedDataTableEntry` | `0x27BC50` | Conditional loaded-entry unlock |
| `0xA62476` | `MI_IS_PHYSICAL_ADDRESS` | `0x2FE1B0` | Reject physical-address representation |
| `0xA624C1` | `MiGetPagePrivilege` | `0x299200` | Read PFN/page privilege; bit `0x40` rejects |
| `0xA6250E` | `MiClearPfnImageVerified` | `0x32C4A0` | Mode 2, `MiFlags&0x8000`: per-PFN verified-state transition |
| `0xA6252B` | `VslValidateDynamicCodePages` | `0xA6258C` | Mode 1, `MiFlags&0x4000`: VTL request and result gate |

There is no direct `Ci*`, `Hash*`, `Integrity*`, `Flush*`, or `Hvcall*` call in this function body; `VslValidateDynamicCodePages` is its visible validation call. The transitive page-table, VTL, and TLB calls below are separately identified from their instructions. The [JSON call index](26100-mmchange-calls.json) records every direct `call` byte sequence.

## Focused children and deeper calls

Each linked `callee` file shows the first 200 instructions from its entry, or the contiguous public-symbol span if shorter. `.pdata` entries can split a single function: the extractor continues past such adjacent entries. CDB `uf` was separately used to recover direct-branch-linked cold blocks; the `complete` files show CDB-discovered instructions validated byte-for-byte against the PE. `uf` can itself overrun a non-returning call into a neighboring symbol, so that boundary was checked; in particular `MI_IS_PTE_IN_WS_SWAP_SET` beginning at `0x32C640` is **not** part of `MiClearPfnImageVerified`.

| Function (RVA) | First 200 / to next symbol | CFG and cold-block evidence |
| --- | --- | --- |
| `MiLockLoadedDataTableEntryIfNecessary` (`0x27C2D8`) | [callee](26100-callee-27C2D8-MiLockLoadedDataTableEntryIfNecessary.asm) | [complete](26100-complete-MiLockLoadedDataTableEntryIfNecessary.asm) |
| `MiSetImageProtection` (`0x2C59E0`) | [callee](26100-callee-2C59E0-MiSetImageProtection.asm) | [complete](26100-complete-MiSetImageProtection.asm) |
| `MmUnlockLoadedDataTableEntry` (`0x27BC50`) | [callee](26100-callee-27BC50-MmUnlockLoadedDataTableEntry.asm) | [complete](26100-complete-MmUnlockLoadedDataTableEntry.asm) |
| `MI_IS_PHYSICAL_ADDRESS` (`0x2FE1B0`) | [callee](26100-callee-2FE1B0-MI_IS_PHYSICAL_ADDRESS.asm) | [complete](26100-complete-MI_IS_PHYSICAL_ADDRESS.asm) |
| `MiGetPagePrivilege` (`0x299200`) | [first 200](26100-callee-299200-MiGetPagePrivilege.asm) | [complete, 235 reachable instructions](26100-complete-MiGetPagePrivilege.asm), including real cold jump `0x299533→0x6D34EC` |
| `MiClearPfnImageVerified` (`0x32C4A0`) | [callee](26100-callee-32C4A0-MiClearPfnImageVerified.asm) | [complete, 116 reachable instructions](26100-complete-MiClearPfnImageVerified.asm) |
| `VslValidateDynamicCodePages` (`0xA6258C`) | [callee](26100-callee-A6258C-VslValidateDynamicCodePages.asm) | [complete](26100-complete-VslValidateDynamicCodePages.asm), all instructions within RVA `0xA6258C..0xA6273A` |
| `MiSetSystemCodeProtection` (`0x2C5A30`) | [first 200](26100-callee-2C5A30-MiSetSystemCodeProtection.asm) | [complete, 482 reachable instructions](26100-complete-MiSetSystemCodeProtection.asm), real cold jump `0x2C6155→0x6D489E` |
| `MiMakeValidPte` (`0x367550`) | [callee](26100-callee-367550-MiMakeValidPte.asm) | [complete](26100-complete-MiMakeValidPte.asm) |
| `MiWriteValidPteNewProtection` (`0x2955F0`) | [callee](26100-callee-2955F0-MiWriteValidPteNewProtection.asm) | [complete](26100-complete-MiWriteValidPteNewProtection.asm) |
| `MiFlushTbList` (`0x22C9D0`) | [first 200](26100-callee-22C9D0-MiFlushTbList.asm) | [complete, 642 reachable instructions](26100-complete-MiFlushTbList.asm) |
| `VslpEnterIumSecureMode` (`0x2F8C60`) | [first 200](26100-callee-2F8C60-VslpEnterIumSecureMode.asm) | [complete](26100-complete-VslpEnterIumSecureMode.asm), real cold jump `0x2F90C7→0x6D72BC` |
| `HvlFlushRangeListTb` (`0x22D620`) | [first 200](26100-callee-22D620-HvlFlushRangeListTb.asm) | [complete](26100-complete-HvlFlushRangeListTb.asm), real cold jump `0x22D9F1→0x6D1206` |

The [callee index](26100-callee-index.json) records call instructions within every extracted child. Other linked first-200 evidence includes [MiInsertTbFlushEntry](26100-callee-2A7450-MiInsertTbFlushEntry.asm), [MiOffsetToProtos](26100-callee-310240-MiOffsetToProtos.asm), [MI_READ_PTE_LOCK_FREE](26100-callee-296680-MI_READ_PTE_LOCK_FREE.asm), [MiInvalidPteConforms](26100-callee-2B3080-MiInvalidPteConforms.asm), [VslRemoveProtectedPage](26100-callee-32C6F4-VslRemoveProtectedPage.asm), [VslMakeProtectedPageExecutable](26100-callee-32C774-VslMakeProtectedPageExecutable.asm), [VslpLockPagesForTransfer](26100-callee-20B30C-VslpLockPagesForTransfer.asm), and [VslpUnlockPagesForTransfer](26100-callee-20BBB8-VslpUnlockPagesForTransfer.asm).

### Valid PTE, prototype path, and bit operations

`MiSetImageProtection` computes self-map PTE addresses from the VA range (`0x2C59E4..0x2C5A1A`) and calls `MiSetSystemCodeProtection` at `0x2C5A1D`. The main function first obtains each `MMPFN` from the MDL PFN array (`0xA6249D..0xA624AF`) and rejects a negative qword at `MMPFN+0x28` (`0xA624AF..0xA624B6`). This is an explicit PFN representation check before changing image PTEs. It aligns with the 22621 build's separately named `MI_PFN_IS_PROTO` check; the 26100 field's precise bitfield name is not inferred solely from its sign test.

Within `MiSetSystemCodeProtection`, the ordinary path sets `R15B=1` at `0x2C5A65` and tests it against the low bit of the loaded PTE at `0x2C5C26..0x2C5C39`; a special path can clear `R15B` at `0x2C5CEA`, so the condition is not universally just bit 0. The valid-PTE path reaches `MiMakeValidPte` (`0x2C5B8D`) and `MiWriteValidPteNewProtection` (`0x2C5BA7`). Other branches read invalid PTEs with `MI_READ_PTE_LOCK_FREE` (`0x2C5F0F`), call `MiInvalidPteConforms` (`0x2C5F31`), and derive prototype locations through `MiOffsetToProtos` (`0x2C5E37`). These are distinct paths in the [complete function](26100-complete-MiSetSystemCodeProtection.asm); the `MiOffsetToProtos` call alone does not prove every prototype PTE is rewritten. The writer also has a separate shadow-PTE branch through `MiSanitizeShadowPxe` (`0x295752`) and `MiWritePteShadow` (`0x295769`).

PDB names the protection mask array `MmProtectToPteMask`, RVA `0x10A00` ([symbol and disk values](26100-pdb-tables.txt)). `MiMakeValidPte` at `0x36755B..0x367584` takes `internalProtection & 0x1F` and loads the qword indexed by it. PE values are `table[1]=0x8000000000000000` (NX bit 63), `table[3]=0`, and `table[0x13]=0x18`. It merges `PFN<<12` at `0x36759F..0x3675A6` and sets `0x21` at `0x3675A9` (Present bit 0 and Accessed bit 5). The ordinary construction for values `1`, `3`, and `0x13` does not set RW bit 1. The helper has an **address-specific exception**: compare with `0xFFFFF6FB7DBEDF68` at `0x367738..0x367745`, then `or rbx,0x8000000000000000` at `0x367781..0x36778B`. Therefore mask values 3/`0x13` do not guarantee that every resulting mapping is executable.

`MiWriteValidPteNewProtection` reads old `[rcx]` at `0x2955FF`, retains old bits 56..62 with `and rbx,0x7F00000000000000` at `0x295643..0x29564F`, retains new bits 0..55 and 63 with `and rdx,0x80FFFFFFFFFFFFFF` at `0x295652..0x29565C`, then merges them and writes `mov [rdi],rbx` at `0x29565F..0x29568A`. Thus NX 63, RW 1, and Present 0 come from the constructed new value on this path. No `lock cmpxchg` occurs in the inspected PTE writer. The `lock bts`/`lock and` in `MiSetSystemCodeProtection` at `0x2C5B19`/`0x2C5B72` operate on `MMPFN+0x18`, rather than the PTE qword. Static code does not establish whether runtime VTL policy grants a separate writable alias.

### TLB invalidation and hypercall wrappers

`MiSetSystemCodeProtection` adds a flush entry at `0x2C5D77` (`MiInsertTbFlushEntry`, RVA `0x2A7450`) and calls `MiFlushTbList` at `0x2C5CF5`, with additional flush calls at `0x2C615E`/`0x2C6173`. In [full `MiFlushTbList`](26100-complete-MiFlushTbList.asm), direct hardware invalidation appears as `invpcid` at `0x22CD6F`/`0x22CDFD` and `invlpg` at `0x22CD79`/`0x22CE07`. Other runtime branches call `KiIpiSendRequest` at `0x22D20B`/`0x22D3E8` and `KiFlushRangeWorker` at `0x22D21A`, or `HvlFlushRangeListTb` at `0x22D0A0`/`0x22D38A`. `MiFlushTbList` also calls `VmFlushTb` (`0x22CB6F`) and `ExFlushTb` (`0x22CB9F`); their [first 21](26100-callee-22D5E0-VmFlushTb.asm) and [first 44](26100-callee-22D544-ExFlushTb.asm) instructions show a conditional `VmpFlushTb` call and HAL dispatch, respectively. The static PE cannot choose a runtime flush branch. No `invept` was found in these inspected functions.

The TLB-related hypercall route is **separate** from the dynamic-code validation request:

| Route | Exact register/control construction | Call and result observation |
| --- | --- | --- |
| Non-list hypercall branch in `HvlFlushRangeListTb` | `0x22D7B9` stores low qword value `3`; `0x22D7C7` computes `ESI&0xFFF` and `0x22D7D0` writes it to the control qword's high dword. `0x22D7D4` loads the completed qword into `RBX`; at `0x22D83A..0x22D840`, `R8D=0`, `RDX=RDI`, and `RCX=RBX=((ESI&0xFFF)<<32)|3`. The meaning of `RDI` bits is not fully recovered. | `0x22D843` calls `HvcallInitiateHypercall` (RVA `0x22E040`). That wrapper reads PDB global `HvcallCodeVa` RVA `0x1201858` at `0x22E08D` and executes `call rax` at `0x22E0A6`. It returns RAX; the immediate `HvlFlushRangeListTb` callsite has no status test before its next call. |
| Extended fast list branch | `0x22D8AC` stores low qword value `3`, `0x22D897` masks `ESI` with `0xFFF`, `0x22D8B9` writes it to the high dword, and `0x22D90A` ORs low bits `0x10000`. `0x22D912..0x22D921` sets `RCX=((ESI&0xFFF)<<32)|0x10003`, `RDX=&[rbp-0x70]`, `R8D=R10D` (formed from the pre-mask count as `8*count+0x18` at `0x22D88F`), `R9D=0`, and writes `EBX` into the low 32 bits of the fifth stack parameter with `mov dword ptr [rsp+0x20],ebx`. The semantic name and upper 32 bits of that stack field are unknown here. | `0x22D925` calls `HvcallFastExtended` (RVA `0x22DAF0`). Its `HvcallpExtendedFastHypercall` child at `0x6B5899..0x6B58BF` loads up to six 16-byte chunks from the RDX buffer into XMM0..XMM5, the first two qwords into RDX/R8, then calls the same `HvcallCodeVa` pointer. `HvcallpExtendedFastHypercallWithOutput` tests `AX` at `0x6B58E2` and only copies returned register data on zero. The immediate caller at `0x22D925` does not branch on returned RAX. |

The low 32 bits of these control words are `3` or `0x10003`; both carry low 16-bit request number `3`, while the high 32 bits contain the count. This does **not** identify the dynamic-code VSL operation's hypercall number. The [on-disk dispatch qwords and stub instructions](26100-dispatch-pointer-values.txt) show that `HvcallCodeVa` RVA `0x1201858` contains `0x1404BBDE0` (local RVA `0x4BBDE0`, `mov eax,0x1000; ret`), while `HvlpVsmVtlCallVa` RVA `0x1201860` is zero. Runtime binding or rewriting of either pointer was not observed. The inspected on-disk call chain has no `vmcall` or `vmmcall`; the runtime target is unresolved.

### VTL request and validation boundary

`VslValidateDynamicCodePages` (RVA `0xA6258C`) prepares temporary MDL and transfer structures. It calls `MmProbeAndLockPages` at `0xA62680`, then `VslpLockPagesForTransfer` at `0xA6269F`; the latter's negative result branches to unlock at `0xA626A6..0xA626A8`. It stores the input MDL pointer to request `+0x08` at `0xA626AA`, and transfer values to `+0x18/+0x20` at `0xA626C2..0xA626D4`. At `0xA626DC..0xA626EE` it calls `VslpEnterIumSecureMode` with `CL=2`, `EDX=0x20`, `R8D=0`, and `R9=&request`; these are **internal VSL request fields**, not a confirmed TLFS hypercall ID. The return is saved at `0xA626F3` and returned after unlock at `0xA6270C`. The page-count check `0xA62624..0xA62628` rejects a temporary MDL spanning more than one page with `0xC000000D`.

`VslpEnterIumSecureMode` writes `CL` to request `+0` and `DX` to request `+2` at `0x2F8CB3..0x2F8CB6`. Its main VSM branch calls `HvlSwitchToVsmVtl1` at `0x2F8DAF`; the latter loads PDB global `HvlpVsmVtlCallVa` at `0x6A69CF` and uses a `jmp rax` helper at `0x6A6A80`. A fallback dispatch path calls `VslpDispatchIumSyscall` at `0x2F8F88`, which in turn performs an indirect `call rax` at `0x6A6AD2`. The request's return/status is read at `0x2F8DBE` and processed at `0x2F8E0F..0x2F8E50`. Nothing in these VTL0 instructions performs a local hash comparison, signature verification, or caller-identity check. The VTL1 side and its exact validation policy are not present in the supplied PE/PDB.

## Direction and cross-reference finding

`mode=2` makes one `MiSetImageProtection(...,1)` call and may clear per-PFN verified state under `MiFlags&0x8000` (`0xA624D3..0xA6251B`). The table entry for internal value 1 sets NX. `mode=1` makes that same first call, conditionally performs VSL dynamic-code validation under `MiFlags&0x4000`, and on nonnegative result makes a second call with internal value 3 or `0x13`, whose table entries have no NX. These two directions therefore have different calls and checks. The image PTE transition directly observed here is **RO/NX versus an attempted executable mapping**, not a hardware PTE with RW bit 1 set by this function. A separate writable alias or actual final executable state requires runtime readback and is not established by these instructions.

For `mode=1`, the extra validation is a VTL request and returned status check; the local image does not reveal what the secure side verifies. For `mode=2`, `MiClearPfnImageVerified` has conditional calls to `VslRemoveProtectedPage` at `0x32C572` and `VslMakeProtectedPageExecutable` at `0x32C5B2`, depending on PFN state and flags. The latter names describe the targets, while their actual VTL result still depends on runtime state.

The [executable-section near-call scan](26100-nt-xrefs.json) found **zero** `E8 rel32` byte candidates targeting RVA `0xA62300` in this local `ntoskrnl.exe`. This bounds only *direct relative calls* within this PE; it does not exclude indirect calls, imports from CI.dll, or other modules.

## Direct comparison with the supplied 22621.4317 PE/PDB

The comparison below uses the independently checked [22621 report and listings](22621-report.md), which provide source RVA instructions for each point.

| Point | 22621.4317 direct evidence | 26100.9022 direct evidence |
| --- | --- | --- |
| Function shape | Hot RVA `0x736CF0..0x736F9A`, plus linked cold blocks `0x8C908E..0x8C912B`; 206 listed instructions, 22 calls | RVA `0xA62300..0xA62585`, no external cold branch; 158 byte-complete instructions, 8 calls |
| Loaded-entry handling | Explicit load-lock and `MiLookupDataTableEntry` calls at 22621 RVA `0x736D64..0x736D96`, followed by separate cleanup | `MiLockLoadedDataTableEntryIfNecessary` at `0xA62378`; conditional `MmUnlockLoadedDataTableEntry` at `0xA623FA` |
| PFN representation check | `MI_PFN_IS_PROTO` called at 22621 RVA `0x736E72` | Inline sign test of `MMPFN+0x28` at `0xA624AF..0xA624B6` |
| Mode 2 PFN call argument | `MiClearPfnImageVerified(pfn,0x18)` at 22621 RVA `0x8C910E` | `MiClearPfnImageVerified(pfn,9)` at `0xA62509..0xA6250E` |
| Mode 1 VSL discriminator | `CL=2`, `EDX=0x1F` at 22621 RVA `0x8794D8..0x8794EA` | `CL=2`, `EDX=0x20` at `0xA626DC..0xA626EE` |
| Top-level validation shape | Initial internal protection 1, optional VSL validation, final 3/`0x13` on nonnegative result | Same visible shape at `0xA62390..0xA62558`; no newly visible local hash/signature/caller-identity check |
| TLB code | 22621's inspected path calls flush helpers and reaches CR3 reload (22621 report) | 26100 `MiFlushTbList` has direct `invpcid`/`invlpg` at `0x22CD6F..0x22CE07` plus IPI and hypervisor branches |

The change in VSL selector and PFN-clear argument is observable; their private protocol semantics cannot be deduced from the callsites alone. The two versions do **not** establish identical runtime validation policy or successful RX restoration. No driver was loaded and no protection transition was executed during this static analysis.
