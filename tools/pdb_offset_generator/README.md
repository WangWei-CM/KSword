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
