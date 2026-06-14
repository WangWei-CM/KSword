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

## Release sync packs

`ksword_profile_release_sync.py` still emits the legacy fields-only v1 pack by
default. Existing v1 release flows remain valid:

```powershell
python tools\pdb_offset_generator\ksword_profile_release_sync.py `
  --emit-pack
```

To publish the callback-aware pack used by newer R3 loaders, request pack v2
and write it directly to `Release\profiles\ark_dyndata_pack_v2.json`:

```powershell
python tools\pdb_offset_generator\ksword_profile_release_sync.py `
  --emit-pack `
  --pack-version 2 `
  --pack-output Release\profiles\ark_dyndata_pack_v2.json
```

The default output path is version-specific:

- v1: `<release-root>\profiles\ark_dyndata_pack_v1.json`
- v2: `<release-root>\profiles\ark_dyndata_pack_v2.json`

`--pack-only` keeps its existing behavior and can also be combined with
`--pack-version 2`. `--pack-output` overrides the computed v1/v2 path when a
specific destination is required.

For a release-side v2 pack that lands at `Release\profiles\ark_dyndata_pack_v2.json`
and does not publish scattered JSON, use pack-only together with `--clean-target`:

```powershell
python tools\pdb_offset_generator\ksword_profile_release_sync.py `
  --release-root Ksword5.1\Ksword5.1\x64\Release `
  --pack-only `
  --pack-version 2 `
  --pack-output Ksword5.1\Ksword5.1\x64\Release\profiles\ark_dyndata_pack_v2.json `
  --clean-target
```

This keeps the Release tree on the compact pack path only; it skips PDB/PE
publication and skips copying scattered `profiles\ark_dyndata\*.json` payloads.

Pack v2 keeps the v1 top-level layout and adds callback payloads to each profile:

```json
{
  "schemaVersion": 1,
  "packVersion": 2,
  "fieldDictionary": ["EpObjectTable"],
  "profiles": [
    {
      "moduleClassId": 0,
      "machine": 34404,
      "timeDateStamp": 305419896,
      "sizeOfImage": 21299200,
      "profileName": "example",
      "pdbName": "ntkrnlmp.pdb",
      "pdbGuid": "",
      "pdbAge": 1,
      "fields": [[0, 768]],
      "callbackItems": [
        {
          "name": "PspCreateProcessNotifyRoutine",
          "kind": "GlobalRva",
          "value": 305419896
        }
      ]
    }
  ]
}
```

`profile.fields` remains an array of `[fieldIndex, offset]` pairs. In v2,
`profile.callbackItems` is optional per source profile; profiles without source
callback items are emitted with an empty callback array. Each callback item is a
compact object containing at least `name`, `kind`, and numeric `value`.
Source-only diagnostics such as symbol source, section, and member metadata are
not copied into the pack.

## callbackItems release validation

Release sync accepts profiles with missing or empty `callbackItems`; those
profiles remain publishable and simply carry no callback items in v2. If
`callbackItems` is present, every item must pass these checks:

- `kind` must be exactly `GlobalRva` or `StructOffset`.
- `name` must correspond to a shared callback field ID declared in
  `shared/driver/KswordArkDynDataIoctl.h` (`KSW_DYN_FIELD_ID_CB_*`).
- `value` must parse as an unsigned 32-bit integer.
- `GlobalRva` values must be non-zero and must not exceed
  `KSW_DYN_PROFILE_GLOBAL_RVA_MAX`.
- `StructOffset` values must not be `0xFFFFFFFF` and must not exceed
  `KSW_DYN_PROFILE_OFFSET_MAX`.

Allowed `GlobalRva` names:

- `PspCreateProcessNotifyRoutine`
- `PspCreateThreadNotifyRoutine`
- `PspLoadImageNotifyRoutine`
- `PspNotifyEnableMask`
- `CmCallbackListHead`

Allowed `StructOffset` names:

- `_OBJECT_TYPE.CallbackList`
- `_CALLBACK_ENTRY_ITEM.EntryList`
- `_CALLBACK_ENTRY_ITEM.PreOperation`
- `_CALLBACK_ENTRY_ITEM.PostOperation`
- `_CALLBACK_ENTRY_ITEM.Operations`
- `_CALLBACK_ENTRY_ITEM.CallbackEntry`
- `_CALLBACK_ENTRY.Altitude`
- `_CALLBACK_ENTRY.RegistrationContext`

For compatibility with existing generator output, release sync also accepts the
input alias `_CALLBACK_ENTRY_ITEM.EntryItemList` and writes it into v2 packs as
the canonical `_CALLBACK_ENTRY_ITEM.EntryList` name.
Manifest and report JSON include total `callbackItemCount`, and the per-profile
report entries also keep individual callback item counts for audit purposes.
