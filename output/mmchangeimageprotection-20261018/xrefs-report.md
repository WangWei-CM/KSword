# `MmChangeImageProtection` caller xrefs from the supplied local PE images

Scope: static bytes from the exact three PE images below. All addresses are RVAs unless prefixed `VA`. No kernel code was executed. This is a report of **statically recoverable** callers, not a claim that no external driver can invoke the export at runtime.

| PE | File version | SHA-256 | RSDS GUID / age | Target export RVA |
| --- | --- | --- | --- | --- |
| `artifacts/symbols/ntkrnlmp.exe/2C33C5081047000/ntkrnlmp.exe` | 10.0.22621.4317 | `8214b4f98d1e18df604bf2a9ed0cb3c6c4df062a95181964e3da5e359cca4008` | `9074fc2b-82ed-2b7e-1cb3-366b64be62f9` / 1 | `0x736CF0` |
| `C:/Windows/System32/ntoskrnl.exe` | 10.0.26100.9022 | `cb0603cd0bc46bfb2d94963e8a525c3c78ba90e3739e09deeb3185fc71613c7b` | `9daab201-5e4c-abf6-9800-9dbd0548b669` / 1 | `0xA62300` |
| `C:/Windows/System32/CI.dll` | 10.0.26100.9022 | `b8cc50fbd7c5e916fccbb87f577485ab461fa8ffa8870e7599405e50c125659d` | `81938abc-afd1-575f-511f-ffaf1515f2a4` / 1 | n/a |

The 22621 nt RSDS identity matches the supplied `ntkrnlmp.pdb` folder, and DbgHelp resolved `MmChangeImageProtection` by name to RVA `0x736CF0`. Both function starts also match the PE export table and disassembled bytes:

```asm
22621: 0x736CF0  48 89 5C 24 08  mov qword ptr [rsp+8], rbx
       0x736CF5  44 89 4C 24 20  mov dword ptr [rsp+20h], r9d
26100: 0xA62300  48 8B C4        mov rax, rsp
       0xA6230F  44 89 48 20     mov dword ptr [rax+20h], r9d
```

## Result

| Image | `E8 rel32` call to target | `E9 rel32` jump to target | decoded RIP-relative reference to target | exact 64-bit image-VA pointer to target |
| --- | ---: | ---: | ---: | ---: |
| 22621 nt, `0x736CF0` | 0 | 0 | 0 | 0 |
| 26100 nt, `0xA62300` | 0 | 0 | 0 | 0 |

The byte scanner visits every section with `IMAGE_SCN_MEM_EXECUTE` and solves the actual x64 relative displacement (`site + 5 + sign_extend(disp32) == target RVA`). A separate Capstone pass decodes all executable sections and checks RIP-relative memory operands; an absolute-pointer pass scans all sections. `dumpbin /disasm:nobytes` over each complete nt image independently printed the function's entry instruction but no other line containing its VA (`0000000140736CF0` or `0000000140A62300`). Raw scan and `dumpbin` therefore agree on zero direct call sites within these two nt images.

The local 26100 CI.dll has 262 imports from `ntoskrnl.exe`, but `MmChangeImageProtection` is absent from its PE import table. Independent `pefile` enumeration found **zero ordinal imports** among those 262 nt imports (and zero among all 278 imports from all four DLLs), so an ordinal import cannot conceal this symbol in the sample's import table. Its raw image also contains neither an ASCII nor UTF-16LE `MmChangeImageProtection` string. Thus **no import-bound CI call site exists in this CI sample**. `MmProtectDriverSection` is present in its nt import list; it is a different symbol and is not counted as a call to the requested function. Calls through an address supplied or calculated at runtime are outside this static import search.

**22621 CI.dll is unavailable in the supplied sample set or the inspected `artifacts/symbols` cache.** The host's `C:/Windows/System32/CI.dll` is version 26100.9022. The shallow component directory check under `C:/Windows/WinSxS` found a 26100 code-integrity component, not a 22621 sample. Therefore this report does not make a 22621 CI caller-completeness claim.

## Consequences for the requested caller table

There are no verified rows for the two nt images or for the local 26100 CI import path. In particular, these PE files provide **no caller instruction** from which to recover a constant protection parameter, a PAGE_READWRITE → PAGE_EXECUTE_READ pair, a hotpatch/fix caller, or a Retpoline/Spectre caller. The `r9d` stores above establish only that the function preserves a fourth register argument, not its meaning or a caller-supplied constant. Any pair or parameter semantics must be established from the function body or from an actual external caller binary, with that evidence labelled separately.

Machine-readable raw scan evidence and the exact reproducer are `xrefs-22621.json`, `xrefs-26100.json`, `xrefs-ci-26100.json`, and `xrefs-scan.py` in this directory.
