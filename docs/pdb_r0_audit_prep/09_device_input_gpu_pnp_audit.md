# 09_device_input_gpu_pnp_audit.md - PDB-based R0 device/input/GPU/PnP audit prep

## Scope and guardrails

This document prepares a read-only R0 audit feature for HID, keyboard, mouse, USB, GPU/display, PnP, ACPI, and PCI device state. It is a design document only. It must not introduce keyboard input capture, mouse input capture, packet interception, IRP modification, filter insertion, device disabling, or driver unload behavior.

Implementation must stay aligned with the Phase -1 rules:

- R0/R3 protocol definitions live only under `shared/driver/`.
- R0 IOCTL handlers are registered only through `KswordARKDriver/src/dispatch/ioctl_registry.c`.
- User-mode KswordARK device access goes through `Ksword5.1/Ksword5.1/ArkDriverClient/`.
- Device audit is cross-view evidence collection, not remediation.

Existing code that should be reused or mirrored:

- `KswordARKLight/Features/Hardware/` already builds a SetupAPI/Configuration Manager devnode tree with instance ID, parent ID, class, service, driver key, location, hardware IDs, status flags, and problem code.
- `shared/driver/KswordArkKeyboardIoctl.h` already defines read-only win32k hotkey/hook enumeration protocol.
- `KswordARKDriver/src/features/keyboard/keyboard_query.c` already enumerates win32k hotkeys and `WH_KEYBOARD` / `WH_KEYBOARD_LL` hook chains without recording keystrokes.
- `KswordARKDriver/src/features/kernel/driver_object_query.c` already references `DriverObject` by name, snapshots `MajorFunction[]`, walks `DriverObject->DeviceObject`, and optionally walks `DeviceObject->AttachedDevice` chains.
- `KswordARKDriver/src/features/kernel/driver_integrity.c` already classifies driver object, module ownership, dispatch pointers, fast I/O pointers, service key evidence, and device chain loop/cross-driver attach risks.

## PDB module inventory

The local read-only cache `E:/KswordPDB/PDB/pdb-cache/amd64` contains the candidate module PDB directories needed for this phase:

| Area | PDB modules |
|---|---|
| Keyboard/mouse/HID | `kbdclass.pdb`, `kbdhid.pdb`, `mouclass.pdb`, `mouhid.pdb`, `hidclass.pdb`, `hidparse.pdb`, `i8042prt.pdb` |
| USB | `usbccgp.pdb`, `usbhub.pdb`, `usbhub3.pdb`, `usbxhci.pdb`, `ucx01000.pdb` |
| GPU/display | `dxgkrnl.pdb`, `dxgmms2.pdb`, `cdd.pdb`, `BasicDisplay.pdb`, `BasicRender.pdb`, `monitor.pdb`, `watchdog.pdb` |
| PnP/power/bus | `acpi.pdb`, `pci.pdb`, `PDC.pdb`, `dam.pdb`, `intelpep.pdb`, `processr.pdb`, `swenum.pdb`, `vdrvroot.pdb` |

PDB use should be optional per module. If a module PDB, type, or field is unavailable on a target build, the feature should still emit driver object, devnode, registry, and device stack evidence with an `unsupported` or `partial` status.

## Shared audit model

### P0 common R0 collection primitives

- Resolve target `DriverObject` by canonical names: `\\Driver\\kbdclass`, `\\Driver\\kbdhid`, `\\Driver\\mouclass`, `\\Driver\\mouhid`, `\\Driver\\HidUsb`, `\\Driver\\HidClass`, `\\Driver\\i8042prt`, `\\Driver\\usbccgp`, `\\Driver\\USBHUB3`, `\\Driver\\USBXHCI`, `\\Driver\\dxgkrnl`, `\\Driver\\ACPI`, `\\Driver\\pci`, and service-derived alternatives.
- Reuse the existing driver object query pattern:
  - `DriverObject->DriverStart`, `DriverSize`, `DriverSection`, `DriverUnload`, `DriverExtension->ServiceKeyName`.
  - `DriverObject->MajorFunction[]` ownership against loaded module ranges.
  - `DriverObject->DeviceObject` and each `DeviceObject->AttachedDevice` chain.
  - loop detection and cross-driver attach detection.
- Add a device stack row abstraction that can be shared by input, USB, GPU, and PnP:
  - stack group ID, root device object, current device object, attached depth, owner driver object, owner driver name, device name, device type, characteristics, flags, stack size, alignment requirement, extension pointer, lower/upper relation when derivable, module owner, risk flags.
- Cross-view with R3 SetupAPI rows by instance ID, service name, class GUID/name, hardware IDs, compatible IDs, driver key, location paths, status flags, and problem code.

### P0 common R3 fields

R3 should extend the existing hardware page model rather than replace it. Required fields:

- `instanceId`, `parentInstanceId`, `displayName`, `className`, `classGuid`, `manufacturer`, `serviceName`, `driverKey`, `location`, `hardwareIds`, `compatibleIds`.
- `upperFilters`, `lowerFilters`, `classUpperFilters`, `classLowerFilters` from device and class registry keys.
- `statusFlags`, `problemCode`, `state` using the current Hardware feature conventions.
- Optional R0 correlation keys: service name, driver object name, device object address, PDO/FDO/filter row address, stack group ID.

### P0 risk flags

| Risk | Meaning |
|---|---|
| `STACK_LOOP` | `AttachedDevice` or `NextDevice` traversal loops. |
| `CROSS_DRIVER_ATTACH` | Attached device owner is unexpected for the audited stack. |
| `DISPATCH_OWNER_MISMATCH` | `MajorFunction[]` or Fast I/O routine points outside the owner module or approved paired module. |
| `UNKNOWN_FILTER` | Upper/lower filter service exists but is not on the local baseline/allowlist. |
| `FILTER_ORDER_ANOMALY` | Class driver, HID minidriver, or bus driver order is inconsistent with expected stack role. |
| `DEVNODE_MISSING_R0` | SetupAPI devnode exists but no matching R0 stack row is found. |
| `R0_MISSING_DEVNODE` | R0 device object exists but no SetupAPI devnode/service cross-view is found. |
| `PDB_UNSUPPORTED` | PDB-specific private fields were unavailable; generic WDM evidence remains valid. |
| `PARTIAL_READ` | Safe read failed for optional fields. |

## 1. Input device audit

### 1.1 Keyboard class stack

Priority: P0.

Goal: show keyboard device stacks and filter order without reading keystrokes.

Expected stack patterns:

- USB keyboard: HID PDO/bus path -> `kbdhid` -> optional filters -> `kbdclass` class device.
- PS/2 keyboard: `i8042prt` -> optional filters -> `kbdclass`.
- Virtual keyboard: root/swenum/vdrvroot or vendor bus -> keyboard function driver -> `kbdclass`.

PDB modules:

- P0: `kbdclass.pdb`, `kbdhid.pdb`, `i8042prt.pdb`, `hidclass.pdb`.
- P1: `swenum.pdb`, `vdrvroot.pdb` for virtual keyboard roots.

Candidate structures and symbols:

- Public WDM structures: `DRIVER_OBJECT`, `DEVICE_OBJECT`, `DEVICE_EXTENSION` pointer, `VPB` when present, dispatch table.
- PDB-private candidates to investigate per build: class/device extension types in `kbdclass`, HID keyboard PDO/FDO extension types in `kbdhid`, i8042 keyboard/port extension types in `i8042prt`, HID collection/context types in `hidclass`.
- Stable cross-view sources: registry service key, `Enum\\...` instance ID, `UpperFilters`, `LowerFilters`, class filters under keyboard class GUID.

R0 fields:

- Driver object basics for `kbdclass`, `kbdhid`, `i8042prt`, `hidclass`.
- Major dispatch owner for `IRP_MJ_READ`, `IRP_MJ_INTERNAL_DEVICE_CONTROL`, `IRP_MJ_PNP`, `IRP_MJ_POWER`, `IRP_MJ_DEVICE_CONTROL`.
- Device chain rows: device object address, attached depth, owner driver, device type, flags, stack size, extension address.
- Optional PDB fields: class extension self pointer, lower device pointer, PDO/FDO role, remove lock/state, symbolic name if available.

R3 fields:

- Device class `Keyboard`, service name, instance ID, parent instance ID, hardware IDs, compatible IDs, driver key, status/problem.
- Device and class upper/lower filters.
- Existing win32k hotkey/hook count summary from `KswordArkKeyboardIoctl.h` protocol for linkage only.

UI suggestions:

- Add an `Input` audit tab with grouped nodes: `Keyboard`, `Mouse`, `HID Collections`.
- Per keyboard stack, render top-to-bottom rows: `Class/filter/function/bus`, driver name, service, device object, instance ID, filter source, risk badges.
- Add a read-only correlation panel: `Win32k hooks/hotkeys referencing this process/session`. This must not display keystroke data.

Acceptance:

- A normal USB keyboard shows `kbdclass` and `kbdhid`/HID-related rows plus its devnode.
- A PS/2 keyboard or VM keyboard shows `i8042prt` or virtual bus lineage when present.
- Unknown `UpperFilters`/`LowerFilters` are marked without blocking enumeration.
- No scan code, virtual key event stream, key state, or typed content is collected.

### 1.2 Mouse class stack

Priority: P0.

Goal: show mouse device stacks and filter order without reading movement or button data.

Expected stack patterns:

- USB mouse: HID PDO/bus path -> `mouhid` -> optional filters -> `mouclass`.
- PS/2 mouse/touchpad: `i8042prt` or vendor port/filter -> `mouclass`.
- Virtual mouse: root/swenum/vdrvroot path -> mouse function driver -> `mouclass`.

PDB modules:

- P0: `mouclass.pdb`, `mouhid.pdb`, `i8042prt.pdb`, `hidclass.pdb`.
- P1: `swenum.pdb`, `vdrvroot.pdb`.

Candidate structures:

- `DRIVER_OBJECT`, `DEVICE_OBJECT`, device extension/private FDO/PDO types from `mouclass`, `mouhid`, and `i8042prt` PDBs.
- HID collection descriptor linkage from `hidclass`/`hidparse` when the mouse is HID-backed.

R0 fields:

- Same stack and dispatch fields as keyboard.
- Dispatch owner checks for read/internal device control/PnP/power.
- Optional PDB fields: class lower-device pointer, connected port/function driver pointer, remove state, HID collection link.

R3 fields:

- Device class `Mouse`, HID/USB/PS2 instance lineage, filters, service, status/problem.

UI suggestions:

- Reuse the Input stack table, with a `Pointer device` type badge.
- Show HID usage summary for HID mouse devices: usage page, usage, collection count when available.

Acceptance:

- A normal USB mouse shows `mouclass` and `mouhid`/HID rows.
- Touchpads with vendor filters are shown as filters, not hidden.
- No movement/button input stream is collected.

### 1.3 HID stack and descriptors

Priority: P0 for stack and basic descriptor; P1 for decoded report details.

Goal: provide HID device stack visibility and basic descriptor information for input devices and generic HID peripherals.

PDB modules:

- P0: `hidclass.pdb`, `hidparse.pdb`, `kbdhid.pdb`, `mouhid.pdb`.
- P1: `usbccgp.pdb`, `usbhub3.pdb`, `usbxhci.pdb` for USB-backed HID path.

Candidate structures:

- Public HID/USB descriptors where available from R3 SetupAPI registry/device interface APIs.
- PDB-private candidates: `hidclass` device/collection extension types, HID PDO/FDO extension types, `hidparse` preparsed-data/report descriptor helper types.

R0 fields:

- HID class driver object integrity.
- Device object stack rows for HID PDO/FDO/class devices.
- Optional PDB fields: collection number, report descriptor pointer/length, preparsed data pointer, top-level collection usage page/usage.

R3 fields:

- HID device interface path, instance ID, parent USB instance ID, VID/PID/MI, usage page/usage where available.
- Basic HID descriptor display: vendor ID, product ID, version, usage page, usage, report descriptor length.

UI suggestions:

- `HID Collections` table columns: device, VID/PID, usage page, usage, service, parent USB node, R0 stack status.
- Descriptor pane should show parsed metadata only. Avoid raw report stream capture.

Acceptance:

- Keyboard and mouse HID collections can be correlated back to USB composite/interface devnodes.
- Non-keyboard HID devices appear but are not forced into keyboard/mouse categories.
- Descriptor parsing failures produce partial rows, not hard failure.

### 1.4 Key/mouse filter driver order and suspicious filters

Priority: P0.

Filter order model:

1. R3 reads device-level and class-level `UpperFilters`/`LowerFilters` from registry.
2. R0 walks actual `AttachedDevice` chain for class/function/bus driver objects.
3. R3/R0 cross-view aligns registry order, service names, owner driver objects, and loaded module image paths.
4. Risk classification is evidence-based and read-only.

Suspicious filter marks:

- Filter service is present in actual stack but absent from registry filters.
- Registry filter exists but no loaded driver/stack row is found.
- Filter image path is unsigned/unknown when trust metadata is available.
- Dispatch routines owned by another unrelated module.
- Filter sits above `kbdclass`/`mouclass` unexpectedly or attaches below HID/port driver contrary to expected role.
- Device stack has loop, duplicate repeated attached object, or excessive depth.

Win32k hook/hotkey linkage:

- Reuse existing hotkey/hook enumeration only as a separate UI correlation row.
- Suggested join keys: process ID, thread ID, module base, hook procedure address owner, session.
- Do not imply a filter is malicious solely because a process has a hotkey/hook; combine with driver stack evidence and signature/trust context.

Acceptance:

- A system with known vendor keyboard/mouse filter drivers lists them with order and source.
- A test unsigned filter can be highlighted as `UNKNOWN_FILTER` or `DISPATCH_OWNER_MISMATCH` if applicable.
- The feature remains safe on systems without PDB-private type support.

## 2. USB audit

### 2.1 Hub topology

Priority: P0.

Goal: show USB controller -> root hub -> hub -> port -> function/composite/interface topology.

PDB modules:

- P0: `usbhub3.pdb`, `usbxhci.pdb`, `ucx01000.pdb`, `usbccgp.pdb`.
- P1: legacy `usbhub.pdb` for older stacks.

Candidate structures:

- Public R3 USB APIs and SetupAPI location paths are the primary topology source.
- PDB-private candidates: hub FDO/PDO extension types in `usbhub3`, controller/context types in `usbxhci`, UCX controller/endpoint/device context types in `ucx01000`, composite PDO/interface extension types in `usbccgp`.

R0 fields:

- Driver object integrity for `USBXHCI`, `UCX01000`, `USBHUB3`, `usbccgp`.
- Device stack rows for controller, hub, composite, interface, and HID child devices.
- Optional PDB fields: port number, connection state, speed, hub depth, USB device address, interface number, endpoint count.

R3 fields:

- Instance ID, parent instance ID, location path, service, class, hardware IDs.
- VID/PID/REV/MI parsed from instance IDs where possible.
- Device status/problem and removal/phantom state.

UI suggestions:

- `USB Topology` tree: controller -> root hub -> port -> device -> interface -> function driver.
- Detail pane: VID/PID, port/location path, service, class, filters, R0 stack status.

Acceptance:

- A USB keyboard/mouse is visible both under USB topology and under Input/HID views with a cross-link.
- Composite devices show parent composite node and per-interface children.
- Hubs with unavailable PDB fields still display SetupAPI location paths.

### 2.2 Composite devices

Priority: P0.

Goal: identify `usbccgp` composite parents and interface children.

PDB modules:

- P0: `usbccgp.pdb`.
- P1: `usbhub3.pdb`, `hidclass.pdb` for parent/child linkage.

Candidate structures:

- PDB-private composite device extension/interface collection types in `usbccgp`.
- R3 instance IDs containing `MI_XX` and compatible IDs for interface functions.

R0 fields:

- `usbccgp` driver object and device chains.
- Child interface device objects when derivable from PDB fields.
- Dispatch owner for PnP/power/internal device control.

R3 fields:

- Parent USB instance ID, interface number, function service, class, compatible IDs.

UI suggestions:

- Composite node badge: `Composite`.
- Interface child badge: `MI_00`, `MI_01`, etc.

Acceptance:

- A composite keyboard with HID + vendor interface shows both children.
- Interface service mismatch is marked as a risk only when cross-view evidence is strong.

### 2.3 xHCI controller

Priority: P1.

Goal: show controller and UCX driver integrity plus basic controller/hub association.

PDB modules:

- `usbxhci.pdb`, `ucx01000.pdb`, `pci.pdb`.

Candidate structures:

- PDB-private controller/device context types in `usbxhci` and UCX controller object types.
- PCI devnode/configuration source for vendor/device/subsystem IDs.

R0 fields:

- Driver object integrity, dispatch ownership, device stack rows.
- Optional PDB fields: controller state, operational registers address, interrupter/ring pointers, device slot count. These are display-only and should not be dereferenced deeply without strict safe-read bounds.

R3 fields:

- PCI instance ID, vendor/device IDs, location path, service, driver version if available.

UI suggestions:

- Controller summary at top of USB topology.
- Badge if `USBXHCI`/`UCX01000` dispatch ownership is inconsistent.

Acceptance:

- Controller appears even when no external USB devices are connected.
- The feature does not read MMIO registers unless separately designed and guarded.

### 2.4 HID descriptor basic display

Priority: P0 basic, P2 full report decode.

Fields:

- VID/PID/revision, usage page, usage, report descriptor length, collection count, parent USB interface, service name.
- Optional parsed report item summary from `hidparse` only: input/output/feature report counts and max report length. Do not capture actual reports.

Acceptance:

- Basic HID metadata is available for standard keyboard/mouse devices.
- Full raw descriptor can be copied only as static descriptor bytes if exposed by OS APIs; no live report traffic.

## 3. GPU/display audit

### 3.1 Adapter and display path

Priority: P1.

Goal: show display adapter, monitor, basic render/display stack, and active path metadata.

PDB modules:

- P1: `dxgkrnl.pdb`, `dxgmms2.pdb`, `monitor.pdb`, `cdd.pdb`, `BasicDisplay.pdb`, `BasicRender.pdb`.
- P2: `watchdog.pdb` for watchdog/TDR linkage.

Candidate structures:

- Public R3 display APIs should provide stable adapter/path data: display adapters, monitors, LUID, output names, active paths.
- PDB-private candidates: `dxgkrnl` adapter/device/process allocation structures, `dxgmms2` memory manager/process object structures, monitor device extension types.

R0 fields:

- Driver object integrity for `dxgkrnl`, `dxgmms2`, `monitor`, `BasicDisplay`, `BasicRender`, `cdd` where loaded.
- Device stack rows for display adapters and monitor devices.
- Dispatch owner checks for PnP/power/device control.

R3 fields:

- Adapter name, LUID, vendor/device IDs, driver service, display path, monitor instance IDs, status/problem.
- WDDM driver model/version if available through existing system APIs.

UI suggestions:

- `GPU/Display` page sections: `Adapters`, `Display paths`, `Driver integrity`, `TDR/Watchdog`.
- Cross-link adapter devnode to PCI devnode.

Acceptance:

- A physical GPU and Microsoft Basic Display/Render fallback can both be represented.
- Monitor devnodes appear under display path details.

### 3.2 dxgkrnl/dxgmms2 driver object integrity

Priority: P1.

Goal: reuse driver integrity checks for graphics kernel modules.

PDB modules:

- `dxgkrnl.pdb`, `dxgmms2.pdb`, `cdd.pdb`, `BasicDisplay.pdb`, `BasicRender.pdb`.

R0 fields:

- Driver object basics, dispatch table, fast I/O if present, service key, module ownership.
- Optional PDB globals for adapter list are P2 unless field stability is proven.

R3 fields:

- Loaded module path/version/signature, service, display devnode correlation.

UI suggestions:

- Display a concise integrity badge per graphics module.
- Avoid deep private object traversal in MVP.

Acceptance:

- Dispatch routines for graphics drivers resolve to expected owning modules or produce evidence rows explaining mismatch.

### 3.3 TDR/watchdog status

Priority: P2.

Goal: show TDR/watchdog configuration and available kernel watchdog module integrity.

PDB modules:

- `watchdog.pdb`, `dxgkrnl.pdb`, `dxgmms2.pdb`.

R0 fields:

- Watchdog driver object integrity if loaded.
- Optional PDB globals for TDR state only after build-specific validation.

R3 fields:

- Registry TDR settings under graphics drivers key where applicable: delay, DDI delay, level, debug mode.
- Recent system event log correlation can be a later R3-only extension.

UI suggestions:

- `TDR config` table with source, value, effective/default note.
- `Watchdog integrity` row.

Acceptance:

- Systems without watchdog-specific PDB support still show registry TDR settings and graphics driver integrity.

### 3.4 Process GPU object feasibility

Priority: P2 research only.

Feasibility:

- Process GPU object enumeration through `dxgkrnl`/`dxgmms2` private lists is build-sensitive and should not be part of MVP.
- Safer first step is R3 correlation: process -> adapter usage through public performance counters or DXGI/WMI where available.
- R0 PDB path can be explored later for process object address, allocation count, context/device count, but only with strict versioned offsets and safe reads.

Acceptance:

- No MVP dependency on private GPU process object traversal.
- Any later implementation reports `unsupported` when PDB validation fails.

## 4. PnP/ACPI/PCI audit

### 4.1 Devnode cross-view

Priority: P0.

Goal: compare R3 devnode tree with R0 device stack/driver object evidence.

PDB modules:

- P0: `pci.pdb`, `acpi.pdb`.
- P1: `PDC.pdb`, `dam.pdb`, `intelpep.pdb`, `processr.pdb`, `swenum.pdb`, `vdrvroot.pdb`.

Candidate structures:

- R3 stable source: SetupAPI/Configuration Manager devnodes, registry keys, class filters.
- R0 stable source: `DRIVER_OBJECT`, `DEVICE_OBJECT`, attached chains.
- PDB-private candidates: PnP devnode/device node structures if available through kernel PDB in a separate phase; bus-specific extension types in `pci`/`acpi` PDBs.

R0 fields:

- Driver object integrity and device stacks for bus/root drivers: `ACPI`, `pci`, `swenum`, `vdrvroot`, `intelpep`, `processr`, `PDC`, `dam`.
- Optional PDB fields: PDO/FDO role, child list, resource requirements, power state, bus/device/function for PCI, ACPI namespace path.

R3 fields:

- Existing hardware tree fields plus filters, compatible IDs, container ID, class GUID, location paths.

UI suggestions:

- Add `Cross-view` badge to existing Hardware device detail.
- Show `R3 only`, `R0 only`, `matched`, `partial`, `filter mismatch` states.

Acceptance:

- Every shown input/USB/GPU device can link to a Hardware devnode when SetupAPI exposes it.
- Missing R0 or R3 side is shown as evidence, not treated as fatal.

### 4.2 Device stack

Priority: P0.

Goal: present actual kernel stack order for selected device/service families.

R0 fields:

- Stack group ID, root device object, relation depth, attached device, next device, owner driver, device type, flags, characteristics, stack size, name status/name.
- Risk flags: loop, cross-driver attach, excessive depth, unreadable object, missing name.

R3 fields:

- Service/filter names from device/class registry.

UI suggestions:

- For any Hardware tree item, a `Kernel stack` detail subtable appears if matching stack rows exist.
- Stack rows include source: `R0 AttachedDevice`, `Registry UpperFilters`, `Registry LowerFilters`, `PDB private`.

Acceptance:

- Selecting a keyboard/mouse/USB/display/PCI item shows at least registry filters and, when available, R0 attached-chain rows.

### 4.3 Upper/lower filters

Priority: P0.

Data sources:

- Device key: `UpperFilters`, `LowerFilters`.
- Class key: class `UpperFilters`, class `LowerFilters`.
- R0 actual stack: owner driver names by attached-chain order.

Risk rules:

- Unknown filter: service exists outside Microsoft baseline and local allowlist.
- Missing stack row: registry filter not loaded/attached.
- Hidden stack row: R0 attached driver not listed in device or class filters and not expected function/bus/class driver.
- Order mismatch: class/function/bus driver order inconsistent with expected stack role.

Acceptance:

- Filters are reported separately by registry source and actual stack source.
- No automatic enable/disable/uninstall operation is exposed from this audit page.

### 4.4 PCI/ACPI basic information

Priority: P1.

PDB modules:

- `pci.pdb`, `acpi.pdb`, `intelpep.pdb`, `processr.pdb`, `PDC.pdb`, `dam.pdb`.

R0 fields:

- Driver object integrity for bus/power drivers.
- Optional PDB fields: PCI bus/device/function, vendor/device/subsystem IDs, BAR/resource summary, ACPI namespace path, power state hints.

R3 fields:

- PCI instance ID parsing: `VEN_xxxx`, `DEV_xxxx`, `SUBSYS_xxxxxxxx`, `REV_xx`.
- ACPI IDs, location paths, resource descriptors where exposed by SetupAPI/CM.

UI suggestions:

- Hardware detail pane adds normalized parsed IDs.
- `Bus` section links PCI device -> display/USB/controller child devices.

Acceptance:

- PCI USB controller and PCI display adapter can be linked to USB/GPU views.
- ACPI root and processor/power devices show service and status without requiring private PDB fields.

## R0/R3 protocol sketch

### Request classes

P0 protocols should be separate from existing keyboard hotkey/hook protocol:

- `ENUM_DEVICE_STACKS`: filter by service names, class names, or built-in profile (`input`, `usb`, `gpu`, `pnp`).
- `QUERY_DRIVER_DEVICE_STACK`: query one driver object and include attached devices using existing driver object logic.
- `ENUM_FILTER_EVIDENCE`: return registry filter rows supplied by R3 plus R0 stack match results. R0 should not open arbitrary registry paths beyond controlled service/class keys unless explicitly validated.
- `QUERY_HID_BASIC`: optional; basic HID descriptor metadata only, no report traffic.

### Response row fields

P0 `DEVICE_STACK_ROW`:

- `version`, `rowType`, `profile`, `riskFlags`, `confidence`, `status`, `lastStatus`.
- `driverObject`, `deviceObject`, `rootDeviceObject`, `attachedDeviceObject`, `nextDeviceObject`, `deviceExtension`.
- `relationDepth`, `stackGroupId`, `deviceType`, `flags`, `characteristics`, `stackSize`.
- `driverName`, `serviceName`, `deviceName`, `moduleName`, `imagePath`.
- `r3CorrelationId` supplied by R3 when matching a devnode/service.

P0 `FILTER_EVIDENCE_ROW`:

- `filterName`, `filterSource` (`device-upper`, `device-lower`, `class-upper`, `class-lower`, `r0-attached`), `observedOrder`, `expectedRole`, `matchedDriverObject`, `matchedDeviceObject`, `riskFlags`.

P0 `HID_BASIC_ROW`:

- `instanceIdHash`, `parentInstanceIdHash`, `vid`, `pid`, `mi`, `usagePage`, `usage`, `reportDescriptorLength`, `collectionCount`, `serviceName`, `riskFlags`.

P1/P2 rows can add USB topology, GPU path, TDR, ACPI/PCI details later without changing P0 stack row semantics.

## UI plan

### P0 UI

- Extend Hardware detail with `Filters` and `Kernel stack` panes.
- Add `Device Audit` page with three MVP tabs:
  - `Input stacks`
  - `USB/HID topology`
  - `Filter anomalies`
- Use badges: `OK`, `Partial`, `Unknown filter`, `Order anomaly`, `Dispatch mismatch`, `Cross-view mismatch`, `PDB unsupported`.

### P1 UI

- Add `GPU/Display` page with adapter/path/integrity sections.
- Add `PnP cross-view` filters to show R3-only and R0-only devices.

### P2 UI

- TDR/watchdog details.
- Process GPU object research view if proven safe and version-stable.

## Validation and test strategy

### P0 acceptance tests

1. Clean Windows VM with USB keyboard/mouse:
   - Input stacks enumerate `kbdclass`, `kbdhid`, `mouclass`, `mouhid`, `hidclass` where present.
   - USB/HID topology links devices to USB parents.
   - No keystroke/mouse event data exists in responses or logs.
2. Device with vendor keyboard/mouse filter:
   - Registry filters and actual attached-chain filters are both displayed.
   - Unknown filter baseline produces a warning badge, not a block.
3. Composite USB HID device:
   - `usbccgp` parent and `MI_XX` interfaces are shown.
   - HID descriptor basic metadata appears for HID interfaces.
4. PDB missing/field mismatch simulation:
   - Generic stack evidence still appears.
   - PDB-specific rows show `PDB_UNSUPPORTED` or `PARTIAL_READ`.
5. Dispatch integrity smoke test:
   - Expected Microsoft drivers resolve dispatch routines to owning modules.
   - Artificial mismatch, if tested in a controlled lab, is marked as evidence only.

### P1 acceptance tests

1. GPU/display:
   - Adapter, monitor, and display path rows appear.
   - `dxgkrnl`/`dxgmms2` driver integrity rows are available when loaded.
2. PCI/ACPI:
   - PCI display adapter and USB controller link to GPU/USB views.
   - ACPI/processr/intelpep devices display service/status and driver integrity where present.

### P2 acceptance tests

1. TDR/watchdog:
   - Registry TDR settings display with source.
   - Watchdog driver integrity is shown if module/driver object is present.
2. Process GPU feasibility:
   - Research build returns `unsupported` safely on unknown PDB types.

## MVP definition

MVP is P0 only:

1. Device stack enumeration for input/USB/HID-focused drivers using existing driver object and device chain logic.
2. Filter driver abnormality marking by cross-viewing registry upper/lower filters with actual R0 attached-chain rows.
3. USB/HID basic topology:
   - controller/hub/composite/interface/devnode tree from R3 SetupAPI/CM location paths;
   - R0 driver integrity for USB/HID drivers;
   - basic HID descriptor metadata when available.
4. UI displays evidence and risk badges only. No keyboard input content, mouse movement/button content, USB transfer data, IRP interception, or remediation actions are part of MVP.

## Deferred work

- Full HID report descriptor decode beyond metadata.
- xHCI/UCX deep controller context traversal.
- GPU process object enumeration.
- TDR/watchdog private state traversal.
- ACPI namespace and PCI resource deep parsing from private PDB structures.
