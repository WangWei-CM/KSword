# Build 22621.4317: `nt!MmChangeImageProtection` static instruction report

All addresses in this report are RVAs of the supplied `ntkrnlmp.exe`. Evidence is the local PE and its matching local PDB; no published description is used to establish behavior.

## Sample identity and extraction

- PE: `artifacts/symbols/ntkrnlmp.exe/2C33C5081047000/ntkrnlmp.exe`; file version `10.0.22621.4317`; image base `0x140000000`; PE timestamp `0x2C33C508`; `SizeOfImage=0x1047000`; SHA-256 `8214b4f98d1e18df604bf2a9ed0cb3c6c4df062a95181964e3da5e359cca4008`.
- PE CodeView RSDS identity `9074FC2B82ED2B7E1CB3366B64BE62F91`, age 1, `ntkrnlmp.pdb`. CDB resolved the supplied local PDB at `artifacts/symbols/ntkrnlmp.pdb/9074FC2B82ED2B7E1CB3366B64BE62F91/ntkrnlmp.pdb`; its SHA-256 is `727DBCFA4C1DF3D71651A5C5A3E4D5D84C2B22E8C0FDA3327EE075EAA614B92A`.
- PDB/export locate `MmChangeImageProtection` at RVA `0x736CF0`. The stated hot size `0x2AB` covers RVA `0x736CF0..0x736F9A`; CDB `uf` also discovers 35 instructions in cold blocks at RVA `0x8C908E..0x8C912B`. The hot region contains 171 instructions, including one unreachable `CC` alignment byte at RVA `0x736F47` that `uf` skipped; cold contains 35. The normalized complete listing has 206 entries and 22 `call` instructions.
- [Complete symbol annotated RVA disassembly](22621-complete-disassembly.txt); [raw CDB `uf` transcript](22621-cdb-uf.txt); [22 call sites](22621-calls.tsv). `22621_extract.py` checked each listed byte against the PE and independently verified Capstone instruction boundaries over both complete ranges.
- CDB has no decompiler. The pseudocode below is a manual, instruction derived reconstruction; field and parameter meanings beyond PDB `_MDL` names are identified as inference.

## Four parameter signature and mode selection

The PDB `_MDL` type has `MdlFlags` at `+0x0A`, `StartVa` at `+0x20`, `ByteCount` at `+0x28`, `ByteOffset` at `+0x2C`, and its PFN array begins at `+0x30` (see `22621-mdl-protection-table.txt`). These are exactly the offsets read at RVA `0x736D38`, `0x736D45`, `0x736D4F`, `0x736D8B`, `0x736DA6`, `0x736E5B`. Thus RCX is a `PMDL` directly, not merely an opaque descriptor.

An instruction supported *approximate* prototype is:

```c
NTSTATUS MmChangeImageProtection(PMDL mdl,
                                 void *address,      // RDX, range start
                                 size_t length,      // R8, constrained to <= UINT32_MAX
                                 unsigned mode);     // R9D, only 1 or 2
```

RVA `0x736D0E..0x736D22` permits only `mode=1` or `2`. RVA `0x736D28..0x736D32` rejects nonzero upper 32 bits of `length`; this alone does not prove whether the source declaration used `size_t` or `ULONG`. The entry sequence `mov [rsp+20h],r9d` at RVA `0x736CF5` stores the fourth register argument in its home slot. After seven pushes and `sub rsp,30h` (total `0x68` stack displacement), `[rsp+88h]` at RVA `0x736EC7` refers back to that same entry `[rsp+20h]`, **not** a fifth parameter. No instruction loads a fifth or sixth parameter.

The caller's `mode=1/2` is distinct from the fourth argument later passed to `MiSetImageProtection`: the latter receives internal protection values `1`, `3`, or `0x13`. They are not Win32 `PAGE_*` constants.

## Manual pseudocode, with exact path anchors

```c
// Names: PDB MDL fields are direct; module and operation meanings are inferred.
NTSTATUS MmChangeImageProtection(PMDL mdl, void *address,
                                 size_t length, unsigned mode)
{
    if (mode != 1 && mode != 2) return 0xC000000D;  // 0x736D0E..0x736D22
    if ((length >> 32) != 0 || (mdl->MdlFlags & 7) != 2 ||
        mdl->ByteOffset != 0 || (mdl->ByteCount & 0xFFF) != 0)
        return 0xC000000D;                           // 0x736D28..0x736D5A

    pageCount = mdl->ByteCount >> 12;                // 0x736D4F..0x736D60
    acquireLoadAndModuleResource();                  // 0x736D64..0x736D86
    entry = MiLookupDataTableEntry(mdl->StartVa, 0); // 0x736D8B..0x736D96
    if (!entry) status = 0xC0000225;                // cold 0x8C90B0
    else {
        moduleBase = entry->field_30;
        moduleEnd = moduleBase + entry->field_40;
        mdlEnd = mdl->StartVa + mdl->ByteCount;
        addressEnd = address + length;
        if (mdlEnd > moduleEnd || address < moduleBase ||
            addressEnd > moduleEnd || addressEnd - 1 < address)
            status = 0xC0000018;                    // 0x736DA2..0x736DD9, cold 0x8C90A6
        else if (entry->field_68 & 0x80000)
            status = 0xC0000043;                    // 0x736DDF..0x736DE9, cold 0x8C908E
        else {
            entry->field_68 |= 0x80000;             // 0x736DEF..0x736DF6
            if (MiGetSystemRegionType(moduleBase) == 1)
                MiSessionReferenceImage(moduleBase); // 0x736DF9..0x736E03, 0x8C9098
            else
                ++entry->field_6C;                  // 0x736E09
            status = 0;
        }
    }
    releaseLoadAndModuleResource();                  // 0x736E0D..0x736E24
    if (status < 0) return status;                   // 0x736E29..0x736E2B
    if (MI_IS_PHYSICAL_ADDRESS(moduleBase)) {
        status = 0xC0000018; goto cleanup;           // 0x736E31..0x736E3C
    }
    for (i = 0; i < pageCount; ++i) {
        pfn = (MMPFN *)(0xFFFFDE0000000000 + 48 * mdl->PfnArray[i]);
        if (MI_PFN_IS_PROTO(pfn)) {
            status = 0xC0000018; goto cleanup;       // 0x736E5B..0x736E79
        }
        if (MiGetPagePrivilege(pfn, 0, 0) & 0x40) {
            status = 0xC0000045; goto cleanup;       // 0x736E7F..0x736E90
        }
    }
    MiSetImageProtection(entry, mdl->StartVa, mdl->ByteCount, 1); // 0x736EB1..0x736EC2
    if (mode == 1) {                                 // 0x736EC7..0x736ECF
        if (MiFlags & 0x4000) {                       // 0x736F48..0x736F52
            status = VslValidateDynamicCodePages(mdl, address, (uint32_t)length);
            if (status < 0) goto cleanup;            // 0x736F54..0x736F68
        }
        // When MiFlags&0x8000, validator status 0x12C selects 0x13;
        // otherwise default 3. Meaning of 0x12C itself is not established.
        finalProtection = (MiFlags & 0x8000 && status == 0x12C) ? 0x13 : 3;
        MiSetImageProtection(entry, mdl->StartVa, mdl->ByteCount, finalProtection);
        status = 0;                                  // 0x736F6E..0x736F96, 0x8C90CE
    } else if ((MiFlags & 0x8000) && pageCount != 0) {
        for (i = 0; i < pageCount; ++i)
            MiClearPfnImageVerified(mdl->PfnArray[i], 0x18); // 0x8C90E2..0x8C911B
    }
cleanup:
    // Reacquire locks, clear entry field_68 bit 19, and call
    // MiUnloadSystemImage(entry, -1); exact refcount effect is not proven here.
    // 0x736EE1..0x736F23
    return status;                                    // 0x736F28..0x736F46
}
```

The `MiSetImageProtection` calls return no checked value in this function. A negative `VslValidateDynamicCodePages` result is returned after the initial NX change without a second `MiSetImageProtection` call (`0x736F64..0x736F68` jumps to cleanup at `0x736EE1`). This is a direct control flow observation, not a claim about final runtime page state after cleanup.

## Conditional branches of the complete function

Each target is the taken branch; the opposite condition falls through.

| Branch RVA | Predicate at preceding instructions | Taken target / effect |
| --- | --- | --- |
| `0x736D22` | `(mode-1) > 1` unsigned | `0x8C9122`, return `0xC000000D` |
| `0x736D32` | `R8 != zeroExtend32(R8)` | `0x8C9122`, invalid argument |
| `0x736D3F` | `(mdl->MdlFlags & 7) != 2` | `0x8C9122`, invalid argument |
| `0x736D49` | `mdl->ByteOffset != 0` | `0x8C9122`, invalid argument |
| `0x736D5A` | `(mdl->ByteCount & 0xFFF) != 0` | `0x8C9122`, invalid argument |
| `0x736D9C` | `MiLookupDataTableEntry` returned null | `0x8C90B0`, status `0xC0000225` |
| `0x736DBA` | `mdl->StartVa + ByteCount > moduleBase + moduleSize` | `0x8C90A6`, status `0xC0000018` |
| `0x736DC3` | `address < moduleBase` | `0x8C90A6` |
| `0x736DCC` | `address + length > moduleBase + moduleSize` | `0x8C90A6` |
| `0x736DD9` | `address + length - 1 < address` unsigned | `0x8C90A6`; excludes zero length/overflow cases |
| `0x736DE9` | `entry->field_68 & 0x80000` | `0x8C908E`, status `0xC0000043` |
| `0x736E03` | `MiGetSystemRegionType(moduleBase) == 1` | `0x8C9098`, session image reference |
| `0x736E2B` | prior status is negative | `0x736F28`, return |
| `0x736E3C` | `MI_IS_PHYSICAL_ADDRESS(moduleBase) != 0` | `0x8C90BA`, status `0xC0000018` |
| `0x736E56` | `pageCount == 0` | `0x736EB1`, skip PFN checks |
| `0x736E79` | `MI_PFN_IS_PROTO(pfn) != 0` | `0x8C90BA`, status `0xC0000018` |
| `0x736E90` | `MiGetPagePrivilege(pfn,0,0) & 0x40` | `0x8C90C4`, status `0xC0000045` |
| `0x736EAA` | `++i < pageCount` | `0x736E5B`, repeat PFN checks |
| `0x736ECF` | `mode == 1` (saved R9 home slot) | `0x736F48`, validation/restore branch |
| `0x736EDB` | `MiFlags & 0x8000` in mode 2 | `0x8C90E2`, clear verified bit per PFN |
| `0x736F52` | `!(MiFlags & 0x4000)` in mode 1 | `0x736F6E`, skip VSL validator |
| `0x736F68` | VSL result is negative | `0x736EE1`, cleanup and return error |
| `0x736F7E` | `MiFlags & 0x8000` in mode 1 | `0x8C90CE`, conditional 3 versus `0x13` protection |
| `0x8C90E5` | `pageCount == 0` | `0x736EE1`, skip per-PFN clear |
| `0x8C911B` | decremented PFN counter != 0 | `0x8C90F8`, repeat clear |

## All 22 direct `call` instructions in the function

| Call RVA | PDB target | Target RVA | Instruction role, direct evidence or inference |
| --- | --- | --- | --- |
| `0x736D64` | `MmAcquireLoadLock` | `0x736FB0` | load lock (name/context inference) |
| `0x736D86` | `ExAcquireResourceExclusiveLite` | `0x218FC0` | module resource lock |
| `0x736D91` | `MiLookupDataTableEntry` | `0x2EA430` | locate image entry for `mdl->StartVa` |
| `0x736DF9` | `MiGetSystemRegionType` | `0x25A780` | classify module address |
| `0x736E14` | `ExReleaseResourceLite` | `0x21D720` | release module resource |
| `0x736E1C` | `KeLeaveCriticalRegionThread` | `0x2244D0` | leave critical region |
| `0x736E24` | `MmReleaseLoadLock` | `0x737050` | release load lock |
| `0x736E35` | `MI_IS_PHYSICAL_ADDRESS` | `0x25A7C0` | reject physical address classification |
| `0x736E72` | `MI_PFN_IS_PROTO` | `0x2E9660` | prototype PFN check; child tests signed `[pfn+0x28] < 0` |
| `0x736E89` | `MiGetPagePrivilege` | `0x25A190` | query PFN privilege; reject returned bit `0x40` |
| `0x736EC2` | `MiSetImageProtection` | `0x2EA3D8` | apply internal protection `1` |
| `0x736EE1` | `MmAcquireLoadLock` | `0x736FB0` | cleanup lock |
| `0x736EF7` | `ExAcquireResourceExclusiveLite` | `0x218FC0` | cleanup resource lock |
| `0x736F08` | `ExReleaseResourceLite` | `0x21D720` | cleanup unlock |
| `0x736F10` | `KeLeaveCriticalRegionThread` | `0x2244D0` | cleanup critical region |
| `0x736F1B` | `MiUnloadSystemImage` | `0x737140` | cleanup image reference; exact effect not established |
| `0x736F23` | `MmReleaseLoadLock` | `0x737050` | cleanup load unlock |
| `0x736F2B` | `KeLeaveCriticalRegionThread` | `0x2244D0` | final critical-region exit |
| `0x736F5F` | `VslValidateDynamicCodePages` | `0x879388` | conditional VTL1 validation path |
| `0x736F8F` | `MiSetImageProtection` | `0x2EA3D8` | mode 1 restoration, protection `3` or `0x13` |
| `0x8C909B` | `MiSessionReferenceImage` | `0x32D638` | session image reference |
| `0x8C910E` | `MiClearPfnImageVerified` | `0x3444CC` | mode 2, clear PFN image verified state |

## Key child calls, page table bits, VTL transition, and TLB

Required direct child disassemblies (up to first 200 instructions or function end; all addresses normalized to RVA):

| Direct child | Listing | Full `uf` instruction count | Calls within listed instructions |
| --- | --- | ---: | --- |
| `MI_PFN_IS_PROTO` | [first 4](22621-mi-pfn-proto-first200.txt) | 4 | none |
| `MiGetPagePrivilege` | [first 200](22621-mi-page-privilege-first200.txt) | 225 | `MiLockPageInline`, `MiGetTopLevelPfn`, `KiRemoveSystemWorkPriorityKick` |
| `MiSetImageProtection` | [first 16](22621-mi-set-first200.txt) | 16 | `MiSetSystemCodeProtection` |
| `VslValidateDynamicCodePages` | [first 92](22621-vsl-validate-first200.txt) | 92 | `MmSizeOfMdl`, `MmProbeAndLockPages`, `VslpLockPagesForTransfer`, `VslpEnterIumSecureMode`, unlock and cookie helpers, `memset` |
| `MiClearPfnImageVerified` | [first 104](22621-mi-clear-pfn-first200.txt) | 104 | `MiSetPfnIdentity`, `MiGetPagePrivilege`, `KeSetPagePrivilege`, lock/bugcheck helpers |
| `MI_IS_PHYSICAL_ADDRESS` | [first 81](22621-mi-physical-address-first200.txt) | 81 | none |

Their raw complete CDB transcripts are the corresponding `*-cdb-uf.txt` files. Additional traced functions likewise have first-200 RVA listings and full raw transcripts; see [PTE writer](22621-mi-write-pte-first200.txt), [PTE constructor](22621-mi-make-pte-first200.txt), [code protection](22621-mi-system-code-first200.txt), [VTL wrapper](22621-hvl-switch-first200.txt), [TLB list flush](22621-mi-tb-flush-first200.txt), [KeFlushTb first 200](22621-ke-flush-tb-first200.txt), [KeFlushTb full raw transcript](22621-ke-flush-tb-cdb-uf.txt), [CR3 flush worker](22621-ki-flush-worker-first200.txt), and [current-TB-only CR3 path](22621-ki-flush-current-only-first200.txt).

### MDL PFN/prototype distinction

RVA `0x736E5B..0x736E6D` reads PFN numbers from `mdl+0x30` and converts each to an MMPFN entry using `0xFFFFDE0000000000 + PFN*48`. `MI_PFN_IS_PROTO` at RVA `0x2E9660..0x2E9669` returns `([pfn+0x28] < 0)` by `cmp ...; setl`. A true result fails at RVA `0x736E79`; `MmChangeImageProtection` therefore excludes that PFN representation before its protection call. This is not the same operation as updating a prototype PTE.

### PTE construction and writes

`MiSetImageProtection` at RVA `0x2EA3D8..0x2EA419` converts the MDL VA range to PTE addresses and calls `MiSetSystemCodeProtection` at RVA `0x2EA410` (target `0x258810`). The latter calls `MiMakeValidPte` at `0x258A8D`, `MiWriteValidPteNewProtection` at `0x258AA9`, and `MiInsertTbFlushEntry` at `0x258AD0` on the observed ordinary valid-PTE path. `MiSetSystemCodeProtection` also calls `MI_READ_PTE_LOCK_FREE` at `0x2589CC`, tests a valid PTE's bit 0 at `0x2589BD`, and tests `pfn+0x28 >= 0` at `0x2589FD`; it can take other branches (raw full `uf` transcript provided).

The `MmProtectToPteMask` symbol is RVA `0x163D0`. `MiMakeValidPte` at `0x24196E..0x2419B7` indexes it by `internalProtection & 0x1F`, combines the PFN field, and at `0x2419BA` performs `or rbx,21h` (Present bit 0 and Accessed bit 5). Actual PE table entries: index 1 at RVA `0x163D8` = `0x8000000000000000` (NX bit 63 set); index 3 at `0x163E8` = `0`; index `0x13` at `0x16468` = `0x18` (NX clear). In `MiMakeValidPte`, the only shown `or 42h` at RVA `0x241A0E` is selected when `(protection & 5)==4` and bit 31 is set (`0x2419DE..0x241A14`); protections 1, 3, and `0x13` do not satisfy that condition. The routine also contains address-specific branches, including possible NX addition at `0x43E916`, so these table entries establish the ordinary mapping, not every possible page state.

`MiWriteValidPteNewProtection` reads the old qword at RVA `0x25A71A`; at `0x25A72F..0x25A742` it forms `new ^ ((old ^ new) & 0x7F00000000000000)`, preserving bits 56..62 of the old PTE while taking other bits, including NX 63, RW 1, and Present 0, from the constructed new value. It writes the qword at `0x25A752` with `mov [rdi],rbx`; in the shadow path it writes at `0x47F296` with `mov` and then calls `MiWritePteShadow` at `0x47F29C`. The analyzed PTE-writing path has **no `lock cmpxchg`**. `MiSetSystemCodeProtection` uses `lock bts` / `lock and` at `0x258A33` / `0x258A74` on `MMPFN+0x18` flags, not on a PTE.

`MiFlushTbList` (target RVA `0x25E410`) chooses among `KeFlushTb` (`0x25E48B`), `KeFlushMultipleRangeTb` (`0x25E4D8`), `KeFlushMultipleRangeCurrentTb` (`0x25E4EE`), or `KeFlushCurrentTbOnly` (`0x4401D8`). `MiSetSystemCodeProtection` calls it at `0x258B24`/`0x258B69` and also `MiInsertTbFlushEntry` at `0x258AD0`. The next layer supplies direct hardware evidence: `KeFlushTb` calls `KiFlushCurrentTbWorker` at `0x25E63C`/`0x25E8D9`; that worker executes `mov rax,cr3; mov cr3,rax` at `0x33154D..0x331550` (and in another branch `0x33159D..0x3315A0`). `KeFlushTb` also calls `KiIpiSendRequest` at `0x25E8D2` and `KiIpiSendRequestEx` at `0x25E989`, showing the cross-processor request path. `KiFlushCurrentTbOnly` likewise reloads CR3 at `0x3B7843..0x3B7846`. The inspected `VmFlushTb` (RVA `0x45D15C`) conditionally calls `VmpFlushTb` at `0x45D18B`; `ExFlushTb` (RVA `0x60A518`) dispatches indirectly through `HalIommuDispatch+0x48/+0x50` at `0x60A566`. No direct `invlpg` or `invept` appears in the examined layers. These alternatives depend on runtime conditions; a particular trace's chosen route is not established by the static PE.

### VTL1 request boundary and validation result

`VslValidateDynamicCodePages` at RVA `0x879388` prepares MDL/transfer metadata, calls `MmProbeAndLockPages` at `0x87947C`, then `VslpLockPagesForTransfer` at `0x87949B`. It copies the original MDL pointer to request-buffer offset `+0x08` at `0x8794A6`, data from its transfer structure to `+0x18/+0x20` at `0x8794BE..0x8794D0`, then calls `VslpEnterIumSecureMode` at `0x8794EA` with `CL=2` (set at `0x8794E8`), `EDX=0x1F` (`0x8794D8`), `R8D=0` (`0x8794E5`), and `R9=&request` (`0x8794DD`). These values identify an internal IUM service request, **not** a confirmed TLFS hypercall number. The transfer-lock return is checked for negative status at `0x8794A2..0x8794A4`; the IUM return is kept in EBX and passed back after unlock at `0x8794EF..0x879508`.

`VslpEnterIumSecureMode` writes the service discriminator to request `+0x00/+0x02` at `0x33866C..0x33866F`, calls `HvlQueryVsmConnection` at `0x33865F`, calls `HvlSwitchToVsmVtl1` at `0x338763` on the main VSM path, and reads returned status from request `+0x08` at `0x33876F`. `HvlSwitchToVsmVtl1` copies request bytes `[0..0x67]` into RBX/XMM10..XMM15 at `0x410FA8..0x410FC9`; RVA `0x410FDF` loads a function pointer from `HvlpVsmVtlCallVa` (RVA `0xE01868`), and `0x410FE6` calls the helper at RVA `0x411090`, whose only instruction is `jmp rax`. The on-disk global pointer value is zero. This local PE does not contain the final runtime VTL entry code, `vmcall`/`vmmcall`, or an observable TLFS number on that chain. No hash comparison, signature verification routine, or caller identity check is directly visible in `MmChangeImageProtection`; what VTL1 validates is outside the supplied image.

## Direction conclusion and cross-reference limit

The instruction-supported classification is: `mode=2` applies internal protection 1, whose table entry has NX set, and optionally clears PFN image-verified state. `mode=1` first applies the same NX protection, then conditionally invokes VTL1 dynamic-code validation and, on nonnegative status, applies internal protection 3 or `0x13`, whose table entries have NX clear. This supports *downgrade* for mode 2 and an **attempt to restore execution permission** for mode 1; VSL status, address-specific NX branches in `MiMakeValidPte` (for example RVA `0x43E916`), and downstream runtime state constrain the final outcome. It does **not** show a Win32 `PAGE_READWRITE` image PTE: the observed internal values and `MiMakeValidPte` bit construction do not directly set the hardware RW bit for these three values. A separate writable MDL alias or VTL1 page privilege is outside this function's input/output contract.

The mode 1 path has an additional VTL1 communication/validation call when `MiFlags&0x4000`, and it has a second PTE protection call. The mode 2 path has a conditional per-PFN verified-state clear when `MiFlags&0x8000`. Neither direction is identical to the other. The special nonnegative VSL result `0x12C` selects protection `0x13` at RVA `0x8C90CE..0x8C90D9`; its semantic meaning is unknown from the inspected VTL0 code.

A raw executable-section scan of this PE found **zero** `E8 rel32` candidates targeting RVA `0x736CF0` (`22621-direct-call-candidates.txt`). This is evidence only for the absence of direct relative calls in the supplied `ntkrnlmp.exe`; indirect calls or another module's imports require separate analysis.
