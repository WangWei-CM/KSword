# Ksword PDB Offset Generator

This tool builds KswordARK dynamic-offset JSON profiles from Microsoft public symbols.
It uses the same public symbol-server PE key format used by debugger symbol tools:
`/<file>/<TimeDateStamp><SizeOfImage>/<file>`.

The tool intentionally runs outside the driver and outside the released product:

- It may download PE/PDB files into a local corpus/cache.
- It emits small JSON profiles under `profiles/ark_dyndata`.
- KswordARK user mode parses JSON and sends a packed, validated offset packet to the driver.

## Minimal example

```powershell
python tools\pdb_offset_generator\ksword_pdb_profile_generator.py `
  --kphdyn third_party\systeminformer_dyn\kphdyn.xml `
  --version 10.0.26100 `
  --arch amd64 `
  --symbol-root D:\KswordKernelCorpus `
  --llvm-pdbutil D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe `
  --limit 1
```

Output profiles are written to:

```text
D:\KswordKernelCorpus\profiles\ark_dyndata\*.json
```

## Callback item dry-run

Use `--dry-run` with one local PE/PDB pair to validate callback item parsing without
running the full corpus generator, building KswordARK, writing `Release`, or
refreshing pack files:

```powershell
python tools\pdb_offset_generator\ksword_pdb_profile_generator.py `
  --dry-run `
  --local-pe D:\PDB\pe-store\amd64\ntoskrnl.exe.10.0.26100.961\<sha256>\ntoskrnl.exe `
  --local-pdb D:\PDB\pdb-cache\amd64\ntkrnlmp.pdb\<pdb-guid+age>\ntkrnlmp.pdb `
  --llvm-pdbutil D:\Software\VS\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe `
  --output D:\PDB\scratch\callback_profile_dryrun.json
```

The generated JSON keeps the legacy `fields` and `missingFields` keys. Callback
PDB items are written under `callbackItems` with `kind` set to `GlobalRva` or
`StructOffset`. Missing callback candidates are non-fatal and are reported under
`diagnostics.missingItems`.
