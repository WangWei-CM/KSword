# PDB R0 Audit Prep: Inventory and Extractor Notes

## Scope

This note is preparation for a stable, PDB-backed R0 audit workflow. The current pass is intentionally read-only against the PDB cache and only creates inventory/extractor-prep artifacts.

- Repository root: `D:\Projects\Ksword5.1`
- PDB cache root: `E:\KswordPDB\PDB\pdb-cache\amd64`
- Script location: `D:\Projects\Ksword5.1\tools\pdb_audit_prep\pdb_inventory.py`
- Output directory: `D:\Temp\ksword_pdb_audit_prep\pdb_inventory`

No main project source, project files, shared protocol files, driver files, or build outputs were modified for this prep pass.

## Current PDB cache overview

The cache layout is the standard symbol-cache shape:

```text
E:\KswordPDB\PDB\pdb-cache\amd64\<module>.pdb\<GUIDAGE>\<module>.pdb
```

Inventory scan policy:

- enumerate module directories directly under the PDB root;
- enumerate immediate instance directories under each module;
- probe only a typical PDB file path per module;
- do not recursively extract all streams, types, globals, or functions.

Observed inventory from `pdb_inventory_summary.json`:

| Metric | Value |
| --- | ---: |
| Module directories | 126 |
| Total instance directories | 16086 |
| Minimum instances per module | 11 |
| Maximum instances per module | 1535 |

High-instance modules from the current cache:

| Module | Instances | Typical PDB path |
| --- | ---: | --- |
| `ntkrnlmp.pdb` | 1535 | `E:\KswordPDB\PDB\pdb-cache\amd64\ntkrnlmp.pdb\001C250786674BDE9C759A89614EBB931\ntkrnlmp.pdb` |
| `win32kfull.pdb` | 788 | `E:\KswordPDB\PDB\pdb-cache\amd64\win32kfull.pdb\001C937F06114FE68C90F7D9CDEB83061\win32kfull.pdb` |
| `win32kbase.pdb` | 692 | `E:\KswordPDB\PDB\pdb-cache\amd64\win32kbase.pdb\000A7125DE3FCE6C0A3913A002C1163D1\win32kbase.pdb` |
| `ntkrla57.pdb` | 532 | `E:\KswordPDB\PDB\pdb-cache\amd64\ntkrla57.pdb\014556BF455267EA7539D0BD51903FD41\ntkrla57.pdb` |
| `tcpip.pdb` | 483 | `E:\KswordPDB\PDB\pdb-cache\amd64\tcpip.pdb\006AA03464D84ADEA16D3FDAE497C0541\tcpip.pdb` |
| `dxgkrnl.pdb` | 443 | `E:\KswordPDB\PDB\pdb-cache\amd64\dxgkrnl.pdb\0272182AB8A0FF2AB33D0601E67E5FD11\dxgkrnl.pdb` |
| `vmswitch.pdb` | 431 | `E:\KswordPDB\PDB\pdb-cache\amd64\vmswitch.pdb\00B8CC28920794679B843C130D698F641\vmswitch.pdb` |
| `ci.pdb` | 429 | `E:\KswordPDB\PDB\pdb-cache\amd64\ci.pdb\0013753C03684DD683B5569E9CF6B7071\ci.pdb` |
| `ntfs.pdb` | 426 | `E:\KswordPDB\PDB\pdb-cache\amd64\ntfs.pdb\0009FDDE615178EA80CF84B9215F27711\ntfs.pdb` |
| `clfs.pdb` | 367 | `E:\KswordPDB\PDB\pdb-cache\amd64\clfs.pdb\00711C04EB300AE27440A6AB8279E0721\clfs.pdb` |

The full per-module table is in:

- `D:\Temp\ksword_pdb_audit_prep\pdb_inventory\module_inventory.json`
- `D:\Temp\ksword_pdb_audit_prep\pdb_inventory\module_inventory.csv`

## Toolchain availability

The prototype checks both `PATH` and a bounded set of known Visual Studio/LLVM locations. Current results:

| Tool | Availability | Path / note |
| --- | --- | --- |
| `llvm-pdbutil` | Available, not on `PATH` | `D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe`; also `D:\Software\VS\VC\Tools\Llvm\bin\llvm-pdbutil.exe` |
| `dia2dump` | Not found | Not present in `PATH` or bounded VS/LLVM search paths used by this pass. |
| `cvdump` | Not found | Not present in `PATH` or bounded VS/LLVM search paths used by this pass. |
| `dumpbin` | Available on `PATH` | `D:\Software\VS\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe` |
| `link` | Available on `PATH` | `D:\Software\VS\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe` |
| `vswhere` | Available, not on `PATH` | `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe` |
| `python` | Available on `PATH` | `C:\Users\Administrator\AppData\Local\Programs\Python\Python312\python.exe` |
| `py` | Available on `PATH` | `C:\WINDOWS\py.exe` |
| `rg` | Available on `PATH` | Codex-bundled `rg.exe` |

Full tool output:

- `D:\Temp\ksword_pdb_audit_prep\pdb_inventory\tool_inventory.json`
- `D:\Temp\ksword_pdb_audit_prep\pdb_inventory\tool_inventory.csv`

## Prototype script

Script:

```text
D:\Projects\Ksword5.1\tools\pdb_audit_prep\pdb_inventory.py
```

Primary command used for this pass:

```powershell
python D:\Projects\Ksword5.1\tools\pdb_audit_prep\pdb_inventory.py `
  --llvm-pdbutil D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe `
  --validate-globals
```

Script behavior:

1. Reads only from `E:\KswordPDB\PDB\pdb-cache\amd64` unless another `--pdb-root` is provided.
2. Writes JSON/CSV only under `D:\Temp\ksword_pdb_audit_prep\pdb_inventory` unless another `--output-dir` is provided.
3. Enumerates module directories and immediate instance directories only.
4. Uses expected cache paths like `<module>.pdb\<GUIDAGE>\<module>.pdb` to find representative files.
5. Uses `llvm-pdbutil dump -summary` to capture GUID/Age and stream flags.
6. Uses bounded sample checks for `-types`, `-publics`, and optionally filtered `-globals`.
7. Applies per-command timeouts so a bad PDB/tool combination does not stall the preparation pass.

Generated files:

| File | Purpose |
| --- | --- |
| `pdb_inventory_summary.json` | Combined metadata, module inventory, tool inventory, sample validation. |
| `module_inventory.json` / `.csv` | Per-module instance counts and typical PDB path. |
| `tool_inventory.json` / `.csv` | Tool discovery results. |
| `sample_validation.json` / `.csv` | Sample module read checks and GUID/Age identity. |

## Verified sample modules

The current sample set was:

- `ntkrnlmp.pdb`
- `win32kbase.pdb`
- `win32kfull.pdb`
- `tcpip.pdb`
- `fltMgr.pdb`
- `fvevol.pdb`
- `ndis.pdb`

Validation result summary:

| Module | Instances | Summary | Types | Public symbols | Filtered globals | GUID | Age | Notes |
| --- | ---: | --- | --- | --- | --- | --- | ---: | --- |
| `ntkrnlmp.pdb` | 1535 | OK | OK | OK | Failed | `{001C2507-8667-4BDE-9C75-9A89614EBB93}` | 2 | `llvm-pdbutil dump -globals -global-name=PsInitialSystemProcess` crashed with `0xC0000005`; summary still reports `Has Globals: true`. |
| `win32kbase.pdb` | 692 | OK | OK | OK | OK | `{000A7125-DE3F-CE6C-0A39-13A002C1163D}` | 3 | Stripped PDB, streams readable. |
| `win32kfull.pdb` | 788 | OK | OK | OK | OK | `{001C937F-0611-4FE6-8C90-F7D9CDEB8306}` | 2 | Stripped PDB, streams readable. |
| `tcpip.pdb` | 483 | OK | OK | OK | OK | `{006AA034-64D8-4ADE-A16D-3FDAE497C054}` | 2 | Stripped PDB, streams readable. |
| `fltMgr.pdb` | 96 | OK | OK | OK | OK | `{00E29C14-008E-2C30-2A1B-B7ADEC6B7480}` | 2 | Stripped PDB, streams readable. |
| `fvevol.pdb` | 172 | OK | OK | OK | OK | `{007BE495-CE14-C738-9848-2E2637FF0A8A}` | 3 | Stripped PDB, streams readable. |
| `ndis.pdb` | 177 | OK | OK | OK | OK | `{01019142-0174-1861-E5D5-EE7503FED1DF}` | 4 | Stripped PDB, streams readable. |

Important interpretation:

- `summary_ok` proves the PDB container and metadata stream are readable.
- `types_ok` proves at least a bounded TPI type record query works.
- `publics_ok` proves public symbol stream reads work for the sampled PDB.
- `globals_filtered_ok` is not yet reliable enough to be treated as extractor readiness because `llvm-pdbutil` crashed on the sampled `ntkrnlmp.pdb` global lookup.

## Extractor capabilities needed next

The next-stage extractor should be designed around stable, versioned R0 audit artifacts rather than ad-hoc string dumps. Required capabilities:

1. **PDB GUID/Age identity**
   - Extract GUID and Age from the PDB itself.
   - Cross-check against the cache instance directory name when possible.
   - Store identity in every extracted artifact so offsets are tied to one exact PDB instance.

2. **Module class mapping**
   - Map modules into audit domains, for example:
     - kernel core: `ntkrnlmp.pdb`, `ntkrla57.pdb`
     - GUI kernel: `win32kbase.pdb`, `win32kfull.pdb`
     - networking: `tcpip.pdb`, `ndis.pdb`, `afd.pdb`, `fwpkclnt.pdb`, `http.pdb`
     - filesystem/filter/storage: `fltMgr.pdb`, `ntfs.pdb`, `refs.pdb`, `fvevol.pdb`, `disk.pdb`, `storport.pdb`
     - security/code-integrity: `ci.pdb`, `cng.pdb`, `ksecdd.pdb`, `securekernel.pdb`, `skci.pdb`
   - Keep the mapping external/configurable so new modules do not require code changes.

3. **Struct layout extraction**
   - Extract named struct/class/union layouts from TPI.
   - Include size, field offset, bitfield location, nested anonymous aggregates, and type names.
   - Preserve PDB type index references for traceability/debugging.

4. **Global RVA extraction**
   - Extract global/public symbols with section+offset and resolve to RVA when image section mapping is available.
   - Handle stripped PDBs where only publics or limited globals are present.
   - Avoid relying only on `llvm-pdbutil -globals` because this pass observed a crash on `ntkrnlmp.pdb`.

5. **Enum extraction**
   - Extract enum name, underlying type/size where available, and all enumerators.
   - Preserve signedness/value width accurately.

6. **Function symbol extraction**
   - Extract function/public symbol names, section+offset, length when available, and calling module context.
   - Support demangled and raw symbol names.
   - Keep function data optional per module because stripped kernel PDBs may vary by stream.

7. **Versioned output model**
   - Emit normalized JSON first; generate compact indexed databases later if needed.
   - Suggested key shape: `{ module, guid, age, symbol_kind, name }`.

## Risks and performance notes

- The cache is large enough that recursive full extraction should not be the default: this pass observed 126 module directories and 16086 instance directories.
- `ntkrnlmp.pdb` alone has 1535 cached instances, so any extractor must support incremental/module-targeted runs.
- `llvm-pdbutil` is useful for summary/types/publics, but filtered `-globals` crashed on the sampled `ntkrnlmp.pdb`. The extractor should consider DIA SDK, LLVM library APIs, or a robust subprocess isolation strategy for global extraction.
- Do not capture unbounded `-types`, `-globals`, `-symbols`, or `-publics` output into memory for all modules. Use filtering, pagination, subprocess timeouts, and per-module output limits.
- Treat stripped PDBs as normal. The sampled modules all reported `Is stripped: true` while still exposing type/public/global stream flags.
- For stable R0 audit usage, never infer offsets from module name alone. Always bind extracted data to PDB GUID/Age and, later, the target image timestamp/size or PE identity.
- If future extraction requires image RVA resolution, pair each PDB with the matching PE image metadata; PDB section offsets alone are not sufficient for every audit use case.
