# 05 - Storage / BitLocker / File System PDB R0 Audit Prep

## Scope

目标是为后续基于 PDB 的 R0 只读审计能力做设计准备，覆盖这些本地已缓存 PDB 模块：

- BitLocker/FVE/crypto: `fvevol.pdb`, `dumpfve.pdb`, `cng.pdb`, `ksecdd.pdb`, `tpm.pdb`, `tbs.pdb`
- Storage stack: `disk.pdb`, `partmgr.pdb`, `mountmgr.pdb`, `volmgr.pdb`, `volsnap.pdb`, `storport.pdb`, `stornvme.pdb`, `storahci.pdb`, `vhdmp.pdb`, `spaceport.pdb`
- File systems: `ntfs.pdb`, `refs.pdb`, `fastfat.pdb`, `exfat.pdb`

本文件只定义 read-only audit 方向，不定义写入、修复、绕过或密钥导出功能。

## Local PDB availability

只读查看 `E:\KswordPDB\PDB\pdb-cache\amd64` 后确认：目标模块均有 PDB 目录。当前签名目录数量大致如下，用于估算后续 profile 覆盖面：

| Module | Signature dirs |
|---|---:|
| `fvevol.pdb` | 172 |
| `dumpfve.pdb` | 101 |
| `cng.pdb` | 297 |
| `ksecdd.pdb` | 112 |
| `tpm.pdb` | 80 |
| `tbs.pdb` | 53 |
| `disk.pdb` | 46 |
| `partmgr.pdb` | 78 |
| `mountmgr.pdb` | 22 |
| `volmgr.pdb` | 59 |
| `volsnap.pdb` | 38 |
| `storport.pdb` | 213 |
| `stornvme.pdb` | 120 |
| `storahci.pdb` | 44 |
| `vhdmp.pdb` | 224 |
| `spaceport.pdb` | 157 |
| `ntfs.pdb` | 426 |
| `refs.pdb` | 288 |
| `fastfat.pdb` | 119 |
| `exfat.pdb` | 119 |

## Existing Ksword shape to preserve

- R3 `KswordARKLight\Features\File` currently uses Win32 APIs (`GetLogicalDriveStringsW`, `FindFirstFileW`) and a simple `FileEntry` model. New audit UI should not replace this browser; it should add an audit/cross-view page that can correlate R3-visible paths with R0 device-stack data.
- R3 `KswordARKLight\Features\Hardware` currently uses SetupAPI/Configuration Manager (`SetupDi*`, `CM_*`) and stores `instanceId`, `parentInstanceId`, `serviceName`, `driverKey`, `statusFlags`, `problemCode`. Storage audit should reuse these R3 fields for PnP cross-view instead of inventing a separate hardware tree.
- Shared file protocols already use bounded fixed buffers and explicit status/field flags (`KswordArkFileIoctl.h`, `KswordArkFileMonitorIoctl.h`). New protocols should follow that pattern.
- R0 file/section/memory evidence implementations are thin IOCTL handlers plus backend functions. They validate METHOD_BUFFERED buffers, use bounded row counts, and return diagnostic addresses only. Storage/FVE audit should follow the same model.
- `ioctl_registry.c` is exact-match, fail-closed. Future wiring should add one or more explicit rows, not switch-based dispatch.

## Hard safety boundary

This feature family is audit-only.

Allowed:

- Enumerate volume/storage/file-system state.
- Return status, topology, metadata, and hook/integrity evidence.
- Return kernel object addresses as diagnostic display values only.
- Compare R0 observations with R3 WMI/SetupAPI/Win32 views.
- Count key protector types when the type is exposed as metadata.

Forbidden:

- Exporting BitLocker key material of any kind: FVEK, VMK, clear key, recovery password, numerical password, external key blobs, TPM sealed blobs, or intermediate decrypted secrets.
- Unlocking, decrypting, suspending protection, changing protectors, changing conversion state, modifying FVE metadata, or calling any IOCTL/API that changes BitLocker state.
- Bypassing encryption, patching `fvevol`, patching file-system/storage dispatch tables, or altering device stacks.
- Returning arbitrary raw structure memory where key material may be embedded.
- Using R0 audit output as a subsequent write credential. Addresses are evidence only.

If a PDB-exposed structure contains key-related pointers or buffers, the profile generator must mark those fields as `blocked_sensitive`; R0 must neither read nor serialize them.

## P0/P1/P2 plan

### P0 - MVP: BitLocker status + volume stack + mountmgr cross-view

Goal: high-confidence, read-only volume security visibility with minimal blast radius.

Capabilities:

1. BitLocker/FVE volume state summary.
2. Volume device stack: disk -> partition -> volume manager -> fvevol -> file system.
3. Mount manager drive-letter / Volume GUID cross-view.
4. R3 WMI/manage-bde view compared with R0 device-stack state.

Implementation stance:

- Prefer documented R3 WMI for status labels, and R0 for topology/integrity corroboration.
- R0 does not attempt to derive secrets or parse encrypted metadata blobs.
- PDB offsets are used only to identify driver-specific device extensions and state fields whose semantics are confirmed by offline analysis.

### P1 - Storage topology and file-system integrity evidence

Capabilities:

1. Disk/partition/volume stack rows.
2. `volsnap` snapshot presence and provider rows.
3. `vhdmp` virtual disk identification.
4. Storage Spaces and storage port miniport state summary.
5. File-system driver object/device object inventory.
6. Dispatch and FastIo hook evidence for NTFS/ReFS/FAT/exFAT and selected storage drivers.

### P2 - Deep file-system forensic helpers

Capabilities:

1. NTFS/ReFS/FAT/exFAT internal volume metadata snapshots.
2. NTFS deleted-entry recovery feasibility estimation.
3. Optional offline parser assists in R3 using raw disk/image inputs, not live destructive R0 scanning.

P2 must remain explicitly non-mutating. Deleted-entry recovery enhancement means better identification and triage of recoverable metadata; it does not mean undelete, journal rewriting, or encryption bypass.

## PDB profile strategy

For each target PDB module, generate a versioned profile with:

- Module identity: image name, machine, TimeDateStamp, SizeOfImage, PDB GUID+Age.
- Candidate type offsets: `structName.fieldName -> offset/size/type`.
- Candidate globals: symbol RVA/name/type where public/private PDB exposes them.
- Sensitive-field denylist: key buffers, crypto material, raw metadata blobs.
- Confidence: `confirmed`, `name_only`, `layout_only`, `blocked_sensitive`, `unsupported`.
- Feature mapping: which audit row can use the field.

Profiles should be consumed like existing DynData: exact identity match first, no fuzzy offset reuse across builds.

## BitLocker / FVE read-only audit design

### P0: Volume encryption/protection state

| Item | Design |
|---|---|
| PDB modules | `fvevol.pdb`, `dumpfve.pdb`, `cng.pdb`, `ksecdd.pdb`, `tpm.pdb`, `tbs.pdb` |
| Structure/global candidates | `fvevol` volume/device extension types; FVE volume/context/state enums; FVE metadata header descriptors; mounted volume context; filter/attached-device context; `dumpfve` crash-dump encryption path context; `tpm`/`tbs` device extension status; `cng`/`ksecdd` provider/device state only. Exact names must be discovered offline per PDB. |
| R0 fields | Volume device object address; lower/upper attached device object addresses; driver object names in stack; FVE present flag; FVE state enum if confirmed; conversion percentage if confirmed; protection/lock state if confirmed; metadata status code; read-only confidence; fieldFlags; lastStatus. |
| R3 fields | Drive letter; Volume GUID path; NT device path; WMI `Win32_EncryptableVolume` conversion/protection/lock status; optional `manage-bde -status` text normalized by R3; SetupAPI device instance/service chain. |
| UI suggestion | Table grouped by volume: `Drive`, `VolumeGuid`, `FileSystem`, `WMI Conversion`, `WMI Protection`, `WMI Lock`, `R0 FVE Present`, `R0 FVE State`, `Fvevol Stack Position`, `CrossView`, `Confidence`, `Detail`. Detail pane shows stack and source fields. |
| Acceptance | On BitLocker-enabled, disabled, locked/offline, and decrypting test volumes: R3 WMI values appear; R0 reports whether `fvevol` is in the stack; mismatches are flagged but not auto-corrected. No key bytes or protector values appear in logs/UI. |

Notes:

- `conversion progress` should be sourced from WMI/manage-bde first. A PDB-backed R0 field may corroborate only after offline field semantics are confirmed across several builds.
- `protection status` and `lock status` must be labels/status enums only. Never expose protector payloads.
- `dumpfve` is useful for crash-dump path presence/integrity, not for key extraction.

### P0/P1: Key protector type statistics

| Item | Design |
|---|---|
| PDB modules | Primarily `fvevol.pdb`; R3 WMI is preferred for protector type enumeration. `tpm.pdb`/`tbs.pdb` provide only TPM/TBS service/device health evidence. |
| Structure/global candidates | FVE metadata/protector list descriptors; protector type enum; count fields. Fields that point to payload blobs are sensitive and blocked. |
| R0 fields | Protector type counts only: TPM, TPM+PIN, recovery password, recovery key, startup key, clear/suspended marker if exposed as status only. No IDs, no raw blobs, no numerical password. |
| R3 fields | WMI protector type list/counts; optional manage-bde normalized counts. |
| UI suggestion | Summary chips: `TPM`, `RecoveryPassword`, `ExternalKey`, `Suspended/ClearKey status present?` with sensitive values hidden as `redacted by design`. |
| Acceptance | Type counts match WMI on controlled volumes. Attempts to request raw protector material are impossible at protocol level. |

### P1: FVE device stack position

| Item | Design |
|---|---|
| PDB modules | `fvevol.pdb`, `volmgr.pdb`, `partmgr.pdb`, `disk.pdb`, `ntfs.pdb`, `refs.pdb`, `fastfat.pdb`, `exfat.pdb` |
| Structure/global candidates | Driver object/device object extension layouts for `fvevol`, volume manager, partition manager and file-system volume devices. Generic `_DRIVER_OBJECT`/`_DEVICE_OBJECT` remains from ntoskrnl profiles. |
| R0 fields | Stack order, device type, device characteristics, driver names, attached/lower device links, file-system top device, FVE index in stack, loop/truncation flags. |
| R3 fields | Drive letter/Volume GUID/NT path mapping; SetupAPI instance and service names; WMI encryption status. |
| UI suggestion | Tree per volume: `\Device\HarddiskVolumeX` -> attached stack rows. Highlight `fvevol.sys` position between volume manager and file system when present. |
| Acceptance | For encrypted NTFS/ReFS volumes, `fvevol` appears in expected stack; unencrypted volumes report absent. Device-chain loops are bounded and reported. |

## Storage stack audit design

### P0: Disk / partition / volume device stack

| Item | Design |
|---|---|
| PDB modules | `disk.pdb`, `partmgr.pdb`, `volmgr.pdb`, `mountmgr.pdb`, plus ntoskrnl `_DEVICE_OBJECT`/`_DRIVER_OBJECT` profile already present. |
| Structure/global candidates | `disk` device extension; partition/partmgr extension; volume manager volume/device extension; mount manager database/list structures. Exact private type names must be extracted per PDB. |
| R0 fields | Device object address; driver name; device name if queryable; lower/attached devices; PDO/FDO/filter classification if inferable; sector size/capacity if safely available; status flags; confidence. |
| R3 fields | `GetLogicalDriveStringsW`, `QueryDosDeviceW`, `GetVolumeNameForVolumeMountPointW`, SetupAPI disk/volume device instance, service name. |
| UI suggestion | `Storage Topology` tab: columns `Volume`, `Disk`, `Partition`, `Driver`, `Role`, `DeviceObject`, `Lower`, `Upper`, `PnP Instance`, `Status`. |
| Acceptance | Common SATA/NVMe disks show chain through `disk`, `partmgr`, `volmgr`; output remains bounded on systems with many volumes. |

### P0: mountmgr drive-letter / Volume GUID cross-view

| Item | Design |
|---|---|
| PDB modules | `mountmgr.pdb`, `volmgr.pdb`, `partmgr.pdb` |
| Structure/global candidates | mount manager point database/list; symbolic link entry; unique volume ID mapping; mounted-device notification context. PDB names may vary; validate by cross-checking with documented mount manager IOCTL output. |
| R0 fields | NT volume device path; mount point symbolic link; Volume GUID; drive letters; stale/orphan flags if mountmgr DB exposes them safely; source confidence. |
| R3 fields | `QueryDosDeviceW`, `FindFirstVolumeW/FindNextVolumeW`, `GetVolumePathNamesForVolumeNameW`, optional documented `IOCTL_MOUNTMGR_QUERY_POINTS` from R3. |
| UI suggestion | Cross-view table: `DriveLetter`, `VolumeGuid`, `R3 NtDevice`, `R0 MountMgr Device`, `Volume Stack Top`, `Mismatch`. |
| Acceptance | Drive letter and Volume GUID mappings match R3 on normal volumes; stale mappings are reported as mismatch rows, not removed. |

### P1: volsnap snapshots

| Item | Design |
|---|---|
| PDB modules | `volsnap.pdb`, `volmgr.pdb`, `ntfs.pdb`, `refs.pdb` |
| Structure/global candidates | snapshot volume extension; diff area/file context; provider list; snapshot set ID fields if exposed; snapshot state enum. |
| R0 fields | Snapshot device object; source volume relation; state enum; diff area presence; provider/source driver; timestamps if present and confirmed. |
| R3 fields | VSS/WMI snapshot inventory; Volume GUID path. |
| UI suggestion | `Snapshots` tab with source volume, snapshot device, VSS state, R0 state, diff area hint, cross-view status. |
| Acceptance | Existing restore points/VSS snapshots appear in R3 and are corroborated by `volsnap` stack/device evidence when loaded. |

### P1: vhdmp virtual disks

| Item | Design |
|---|---|
| PDB modules | `vhdmp.pdb`, `volmgr.pdb`, `partmgr.pdb`, `disk.pdb` |
| Structure/global candidates | VHD device extension; backing file/path pointer if exposed as safe string; virtual disk geometry; parent/differencing relation metadata. |
| R0 fields | VHD device object; virtual/physical geometry summary; backing path only if already a safe Unicode path and no raw buffer risk; attached state; parent relation flag. |
| R3 fields | Disk Management/Storage WMI virtual disk rows; SetupAPI service `vhdmp`; mounted VHD path if R3 can resolve it. |
| UI suggestion | `Virtual Disks` tab: `Disk`, `BackingPath`, `VhdType`, `State`, `Stack`, `CrossView`. |
| Acceptance | Mounted VHD/VHDX appears with `vhdmp` in stack and matches R3 disk inventory. |

### P1: Storage Spaces

| Item | Design |
|---|---|
| PDB modules | `spaceport.pdb`, `volmgr.pdb`, `disk.pdb`, `partmgr.pdb` |
| Structure/global candidates | storage pool/space/virtual disk extension types; resiliency/state enums; member disk references. |
| R0 fields | Storage Spaces device object; pool/space ID if safe; member count; health/state enum; lower disk links. |
| R3 fields | Storage Management API / WMI pool/virtual disk information; SetupAPI service chain. |
| UI suggestion | `Storage Spaces` tab: pool/space rows with R3 health and R0 stack corroboration. |
| Acceptance | Systems without Storage Spaces show supported-empty. Systems with spaces show pool/virtual disk rows without mutating health state. |

### P1: storport / NVMe / AHCI base status

| Item | Design |
|---|---|
| PDB modules | `storport.pdb`, `stornvme.pdb`, `storahci.pdb`, `disk.pdb` |
| Structure/global candidates | adapter extension; logical unit extension; queue depth/state; miniport dispatch table; NVMe controller/namespace context; AHCI controller/port context. |
| R0 fields | Adapter/device object; miniport driver name; bus type; queue/state summary if confirmed; error/reset counters if safe; dispatch/FastIo ownership evidence. |
| R3 fields | SetupAPI disk/controller nodes; `SPDRP_SERVICE`, instance ID, hardware IDs; Windows storage property queries in R3 if later added. |
| UI suggestion | `Storage Controllers` tab: controller, miniport, bus, namespaces/LUNs, reset/error summary, hook/integrity status. |
| Acceptance | NVMe and AHCI systems produce bounded controller rows; unsupported fields are explicit `Unavailable`, not guessed. |

## File-system audit design

### P0/P1: NTFS/ReFS/FAT/exFAT volume identification

| Item | Design |
|---|---|
| PDB modules | `ntfs.pdb`, `refs.pdb`, `fastfat.pdb`, `exfat.pdb` |
| Structure/global candidates | file-system VCB/VCB-like volume control block; volume device extension; mounted volume list/global; file-system recognition state; volume serial/label fields if exposed safely. |
| R0 fields | File-system driver name; volume device object; VCB address as diagnostic value; file-system type; volume flags; read-only/dirty/mounted state if confirmed; field confidence. |
| R3 fields | `GetVolumeInformationW`, drive type, Volume GUID, mount points. |
| UI suggestion | `File System Volumes` tab: `Volume`, `R3 FS`, `R0 FS Driver`, `VCB`, `Flags`, `Dirty/ReadOnly`, `CrossView`. |
| Acceptance | NTFS/ReFS/FAT/exFAT volumes are identified consistently with R3; unsupported file systems appear as other/unknown. |

### P1: File-system driver object / device object inventory

| Item | Design |
|---|---|
| PDB modules | `ntfs.pdb`, `refs.pdb`, `fastfat.pdb`, `exfat.pdb`, plus ntoskrnl `_DRIVER_OBJECT` and `_DEVICE_OBJECT`. |
| Structure/global candidates | Driver object globals or exported driver object references if present; file-system device extension; global VCB/list heads. |
| R0 fields | DriverObject; DriverStart/DriverSize; MajorFunction table owner; FastIoDispatch pointer owner; device list; attached filters above FS device. |
| R3 fields | Loaded module path; Service name; File page path context. |
| UI suggestion | `FS Drivers` tab: driver, module base, device count, dispatch anomalies, FastIo anomalies, attached filters. |
| Acceptance | Clean systems show function pointers owned by the expected FS module or ntoskrnl-supported helpers; third-party filters appear as attached devices, not automatically malicious. |

### P1: Dispatch / FastIo hook detection

| Item | Design |
|---|---|
| PDB modules | `ntfs.pdb`, `refs.pdb`, `fastfat.pdb`, `exfat.pdb`; reuse existing driver integrity logic for `_DRIVER_OBJECT.MajorFunction` and `FastIoDispatch`. |
| Structure/global candidates | File-system driver FastIo dispatch table type; known dispatch routines; optional FS-specific operation tables. |
| R0 fields | MajorFunction index; pointer; owner module; expected owner class; FastIo slot; risk flags: outside module, non-executable, third-party filter-owned, null unexpected, self-modified. |
| R3 fields | Module list, file-system type, filter list from object namespace/minifilter pages. |
| UI suggestion | Merge with Driver Integrity or add `FS Dispatch Integrity` focused view. Columns: `FS`, `Slot`, `Pointer`, `Owner`, `Expected`, `Risk`, `Detail`. |
| Acceptance | R0 can run read-only across all target FS drivers without crashes; known clean machine has no high-risk rows except legitimate filter indirections. |

### P2: NTFS deleted-item recovery enhancement feasibility

| Item | Design |
|---|---|
| PDB modules | `ntfs.pdb` primarily; `refs.pdb` only for separate future design. |
| Structure/global candidates | NTFS SCB/FCB/LCB/VCB, MFT-related context, index allocation/index root structures, log/journal context. Candidate names must be validated by PDB and never assumed stable. |
| R0 fields | Feasibility-only metadata: volume identity, MFT/inode reference candidates, deleted index entry presence counters, journal availability flags, read-only status. No raw file content extraction in MVP. |
| R3 fields | User-selected volume/path, offline parser output, file browser selection. |
| UI suggestion | `Deleted Entry Feasibility` page: show whether a volume has enough metadata for offline recovery triage, with a warning that live mutation is unsupported. |
| Acceptance | Feature can report `unsupported/insufficient metadata/supported for offline triage` without reading arbitrary file contents or changing disk state. |

## Proposed shared protocol shape

Future shared headers should remain small and explicit, following existing `KswordArkFileIoctl.h` style.

Candidate IOCTL groups:

1. `IOCTL_KSWORD_ARK_QUERY_VOLUME_SECURITY_AUDIT`
   - Request: flags, maxRows, optional volume NT path / Volume GUID filter.
   - Response rows: volume identity, BitLocker/WMI cross-view fields, FVE stack fields, confidence.
2. `IOCTL_KSWORD_ARK_QUERY_STORAGE_TOPOLOGY_AUDIT`
   - Request: flags, maxRows, optional disk/volume filter.
   - Response rows: disk/partition/volume/mountmgr/volsnap/vhd/storage-spaces/controller rows.
3. `IOCTL_KSWORD_ARK_QUERY_FILESYSTEM_AUDIT`
   - Request: flags, maxRows, optional volume/FS type filter.
   - Response rows: FS type, driver/device object, VCB-like diagnostic address, dispatch/FastIo evidence.

Common fields:

- `version`, `size`, `rowSize`, `status`, `sourceFlags`, `fieldFlags`, `returnedRows`, `totalRows`, `truncated`, `lastStatus`.
- Fixed string buffers for names/paths; no variable pointer serialization.
- `confidence` per row: `confirmed`, `partial`, `pdb_missing`, `field_unconfirmed`, `r3_only`, `r0_only`.

## UI integration suggestions

Suggested new dock/page group: `Storage Audit` or a hidden KernelPage-retained feature reused by File/Hardware docks.

Tabs:

1. `BitLocker/FVE` - P0.
2. `Volume Stack` - P0.
3. `Mount Points` - P0.
4. `Storage Devices` - P1.
5. `Snapshots/VHD/Spaces` - P1.
6. `File Systems` - P1.
7. `FS Integrity` - P1.
8. `NTFS Recovery Feasibility` - P2.

Cross-linking:

- From File dock drive root: open selected drive in `BitLocker/FVE` or `Volume Stack`.
- From Hardware dock disk/controller node: open `Storage Devices` filtered by instance ID/service.
- From Driver dock module row: open `FS Integrity` or `Storage Controllers` filtered by driver name.

## Acceptance matrix

| Area | Acceptance |
|---|---|
| Safety | No protocol field can carry key material or raw sensitive blobs; tests/searches confirm no `key`, `FVEK`, `VMK`, `password`, `recovery` payload buffers are serialized except type/status labels. |
| PDB exactness | Profiles are selected by exact module identity. Missing PDB/offset yields explicit unavailable rows. |
| Bounds | Every query has maxRows and hard caps. Device-stack traversal has max depth and loop detection. |
| R3/R0 cross-view | R3-only and R0-only mismatches are visible and do not trigger automatic repair. |
| Empty systems | No BitLocker, no VHD, no Storage Spaces, or no VSS snapshot returns supported-empty, not failure. |
| Existing UI | File and Hardware pages remain usable; new audit pages consume their selected drive/device context but do not replace enumeration logic. |

## MVP detail

MVP should implement only these three read-only outcomes:

1. `BitLocker/FVE Status`
   - R3: WMI/manage-bde normalized status.
   - R0: volume stack contains/does not contain `fvevol`; optional confirmed FVE state fields.
   - Output: one row per volume.

2. `Volume Device Stack`
   - R0: bounded walk of device objects from volume/file-system device downward/upward as available.
   - R3: drive letter, Volume GUID, NT device path, SetupAPI service/instance.
   - Output: one tree or flat grouped table per volume.

3. `MountMgr Cross-View`
   - R3: documented mount-point enumeration.
   - R0: mountmgr/volume device relation if PDB fields are confirmed; otherwise R3-only with explicit `R0 unavailable`.
   - Output: mismatch table.

Defer to P1/P2:

- Protector type counts from R0 internals.
- Storage Spaces deep member topology.
- volsnap/VHD deep metadata.
- FS dispatch/FastIo hook detection.
- NTFS deleted-entry feasibility.

## Open research tasks

- Use DIA/llvm-pdbutil on the local PDB cache to extract candidate type names for each module and build a denylist of sensitive FVE/crypto fields before any R0 code reads them.
- Validate candidate FVE state offsets against at least three builds per major branch: 19041, 22000, 22621/22631, 26100.
- Decide whether mountmgr should be queried by documented IOCTL from R3 first, with R0 PDB-backed fields only as corroboration.
- Define a shared `StorageAuditCommon.h` protocol header only after the MVP row schema stabilizes.
- Add manual test fixtures: unencrypted NTFS, BitLocker encrypted/unlocked, BitLocker locked/offline if available, mounted VHDX, VSS snapshot, Storage Spaces if available.
