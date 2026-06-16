# Driver Unload PDB Research Manifest

## Scope

This phase adds an offline research workflow for the driver-unload failure
investigation. It does not change R0/R3 unload behavior, does not build R3, and
does not run live unload attempts against the target machine.

The goal is to replace repeated force-unload trial and error with a versioned
evidence model based on local Windows PE/PDB corpus data and the existing
Ksword DynData profile format.

## New tool

- Script: `tools/pdb_offset_generator/ksword_driver_unload_research.py`
- Default profile input: `D:\PDB\profiles\ark_dyndata`
- Optional PE input: `D:\PDB\pe-store`
- Optional PDB input: `D:\PDB\pdb-cache`
- Default output:
  - JSON: `D:\PDB\logs\driver_unload_research_report.json`
  - Markdown: `D:\PDB\logs\driver_unload_research_report.md`

The default mode only reads existing scattered profile JSON. `--parse-pdb`
explicitly enables direct `llvm-pdbutil` parsing for selected local PE/PDB
pairs.

## Evidence model

Profile summary tracks the unload-relevant items below:

- `_DRIVER_OBJECT`: `DriverStart`, `DriverSize`, `DriverSection`,
  `MajorFunction`, `FastIoDispatch`, `DriverUnload`
- `_KLDR_DATA_TABLE_ENTRY`: loader links, base, size, full/base name, flags
- Kernel globals: `PspCidTable`, `PsLoadedModuleList`, `MmUnloadedDrivers`,
  `PiDDBCacheTable`
- Callback globals and callback-registration structure offsets when present

Direct PDB mode additionally probes:

- `_DRIVER_OBJECT.DeviceObject`
- `_DEVICE_OBJECT.DriverObject`, `NextDevice`, `AttachedDevice`,
  `ReferenceCount`, type/flag fields
- `_OBJECT_HEADER.PointerCount`, `HandleCount`, type/index/info fields when
  public symbols expose them

## Unload-path model

The report classifies current unload behavior into five paths:

1. `DriverUnloadOnly`
2. `CallbackCleanupByModuleBase`
3. `ClearDispatchOrUnloadPointer`
4. `DeleteDeviceObjects`
5. `MakeTemporaryObject`

Each path records required preflight evidence, known failure modes, rollback
safety, and a recommendation. The current high-level rule is:

- allow only read-only preflight and DriverUnload-only experiments by default;
- do not run dispatch clearing, DeviceObject deletion, callback cleanup, or
  `ObMakeTemporaryObject` on a non-VM target until dynamic validation proves
  the exact target and OS state;
- preserve failed-state evidence instead of performing irreversible cleanup
  after an unload failure.

## How to run

Fast corpus summary:

```powershell
python tools\pdb_offset_generator\ksword_driver_unload_research.py `
  --output D:\PDB\scratch\driver_unload_research_report.json `
  --markdown D:\PDB\scratch\driver_unload_research_report.md
```

Sample direct PDB parse:

```powershell
python tools\pdb_offset_generator\ksword_driver_unload_research.py `
  --parse-pdb `
  --module-class ntoskrnl `
  --version-filter 10.0.26100 `
  --max-pdb 8 `
  --output D:\PDB\scratch\driver_unload_research_report.json `
  --markdown D:\PDB\scratch\driver_unload_research_report.md
```

Full direct PDB parse is intentionally opt-in because it can be slow across
thousands of profiles.

## Dynamic validation requirements

Use WinDbg/KD after the offline report identifies the target OS build and
available fields:

- VM snapshot, preferably KDNET.
- Breakpoint or trace on target `DriverUnload`.
- Capture `DriverObject`, `DeviceObject`, loader entry, callback entries, and
  dispatch/FastIo ownership before and after each attempt.
- Preserve crash dumps for bugcheck cases. Logs alone are not enough to
  distinguish stale callback, stale DeviceObject, loader-state, and reference
  lifecycle failures.

## Current corpus observation

The local scattered profile corpus is mostly legacy schema. It is sufficient to
show existing DynData gaps, but most profiles do not yet contain the v3
DriverObject/KLDR/global items required for automated unload preflight. Use the
new research tool with `--parse-pdb` on representative builds before designing
any R0 behavior change.

## Files changed in this phase

- `tools/pdb_offset_generator/ksword_driver_unload_research.py`
- `tools/pdb_offset_generator/README.md`
- `docs/next_phase_manifests/driver_unload_pdb_research.md`
