<div align="right">
  <a href="./docs/readme_zh.md">简体中文</a> |
  <strong>English</strong>
</div>

<div align="center">

  <img
    src="./Ksword5.1/Ksword5.1/Resource/Logo/KswordHome-En.png"
    alt="KSword ARK Logo"
    width="520"
  />

  <a href="https://github.com/user-attachments/assets/25a3b2e2-4ee0-49aa-bd90-ee6e3ba01fe4">
    <img
      src="https://github.com/user-attachments/assets/25a3b2e2-4ee0-49aa-bd90-ee6e3ba01fe4"
      alt="KSword ARK dark interface"
      width="49%"
    />
  </a>
  <a href="https://github.com/user-attachments/assets/217769a2-0521-41f9-9933-ca7c2fbb1d13">
    <img
      src="https://github.com/user-attachments/assets/217769a2-0521-41f9-9933-ca7c2fbb1d13"
      alt="KSword ARK light interface"
      width="49%"
    />
  </a>

  <br>

  <sub>Dark Mode　|　Light Mode</sub>

</div>

<h1 align="center">Ksword5.1</h1>
<p align="center"><strong>A high-coverage open-source Windows ARK and kernel analysis suite</strong></p>

<p align="center">
  <a href="https://github.com/KSwordDEV/KSword/stargazers">
    <img alt="GitHub stars" src="https://img.shields.io/github/stars/KSwordDEV/KSword.svg?style=for-the-badge" />
  </a>
  <a href="https://github.com/KSwordDEV/KSword/network/members">
    <img alt="GitHub forks" src="https://img.shields.io/github/forks/KSwordDEV/KSword.svg?style=for-the-badge" />
  </a>
  <a href="https://github.com/KSwordDEV/KSword/issues">
    <img alt="GitHub issues" src="https://img.shields.io/github/issues/KSwordDEV/KSword.svg?style=for-the-badge" />
  </a>
  <a href="https://github.com/KSwordDEV/KSword/blob/main/LICENSE">
    <img alt="License" src="https://img.shields.io/github/license/KSwordDEV/KSword.svg?style=for-the-badge" />
  </a>
</p>

---

## Overview

Ksword5.1 is an open-source Windows ARK, kernel-debugging, and system-forensics suite. It includes the full Qt/ADS desktop application, the lightweight native Win32 `KswordARKLight`, the `KswordARKDriver` kernel driver, a CLI, desktop helper components, and an optional installer.

The current codebase focuses on R3/R0 cross-view evidence, PDB/DynData-driven offsets, read-only audit pages, and explicit gates for destructive or mutation-oriented actions.

## Components

- `Ksword5.1`: full Qt application with ADS dock layout and the complete ARK workflow.
- `KswordARKLight`: lightweight native Win32 ARK for older systems and low-resource environments, with simpler dependencies, faster startup, and a focused feature set.
- `Launcher`: pure Win32 startup and compatibility assistant. It checks the readable support manifest before launching the Qt or Light target and can prepare an offline developer collection bundle when loaded kernel modules have missing offsets.
- `KswordARKDriver`: kernel driver that implements process, thread, handle, memory, network, kernel-object, device, and security audit protocols.
- `KswordCLI`: command-line interface for automation, validation, and troubleshooting.
- `KswordSetup`: optional installer that extracts the release payload and creates shortcuts or integration settings. Manually extracting the full `Release\` directory is functionally equivalent.
- `Taskbar`, `KswordHUD`, and `APIMonitor_x64`: helper components for desktop integration, HUD, and API monitoring.

## Key Capabilities

- Process, thread, CID, and handle cross-view; hidden-object evidence; thread-stack inspection; process details; PDB field diagnostics; and guarded R0 actions.
- Memory-region browsing, search, hex viewing, bookmarks and breakpoints, R0 memory paths, kernel executable-memory scanning, memory evidence, and PTE/VA translation.
- Packet monitoring, connection management, per-process throttling, request construction, HTTPS analysis, firewall/WFP views, NIDS, segmented downloads, and TCP/UDP/AFD/WFP/NDIS/NSI audit views.
- Driver services, loaded modules, DriverObject/DeviceObject/MajorFunction/FastIo diagnostics, Driver Integrity, Module Cross-View, Unloaded/PiDDB evidence, SSDT/SSSDT, hooks, callbacks, and object-namespace inspection.
- File management and recovery, hashes, signatures, PE/strings/hex views, file unlocker, minifilter/FileObject/Section/storage evidence, hardware utilization, disk monitoring, device trees, and R0 device-stack audits.
- AppLocker, WDAC/Code Integrity, Defender/ASR, VBS/Hyper-V, platform security, driver trust, and event-log diagnostics.
- ADS layout persistence and restoration, lazy initialization of visible docks, top-menu settings, UIAccess/always-on-top policies, log and task-progress panels, and the Taskbar top AppBar with the `S O S Enter` quick launch.

## ARK Features by Main Application Dock

> This inventory is based on recent code, comments, dock-initialization logic, and the R0/R3 protocols. See `docs/OpenArk功能对照与TODO.md` for the OpenArk coverage comparison and remaining TODOs.

### Main Workspace Docks (16)

> Settings have moved from the primary docks to the top menu; the main workspace includes the Miscellaneous dock.

| Primary Dock | Subpages / Key Areas | Primary Capabilities |
|---|---|---|
| Welcome | Welcome page | Shows version, build time, user information, avatar, and project entry points. |
| Process | Process list, create process, details, threads, modules, tokens, Cross-View, PDB Catalog | Process tree/list, icons and difference highlighting, terminate/suspend/resume/priority/critical-process actions, recoverable R0 hiding, R3/R0 process and thread comparison, thread stacks, and risk prompts for PPL/Signature/CID operations. |
| Network | Traffic monitor, per-process throttling, connection management, request builder, HTTPS, ARP/DNS, live hosts, firewall, NIDS, downloads, network audit | Packet capture and filtering, TCP/UDP connection management, WFP firewall events and rules, real-time detection, segmented HTTP/HTTPS downloads, and read-only TCP/UDP/AFD/WFP/NDIS/NSI cross-view. |
| Memory | Processes and modules, regions, search, viewer, breakpoints/bookmarks, R0 read/write, Kernel Exec Scan, Memory Evidence, PTE | R3 memory browsing/search, R0 region reads, kernel executable-memory scanning, kernel/process memory evidence, and page-table/virtual-address translation. |
| File | File manager, recovery, properties, unlock, Minifilter, FileObject, Section, Storage/BitLocker | Dual-pane management, ownership/permission handling, hashes/signatures/PE/strings/hex, NTFS recovery, file-lock and Section mappings, and read-only storage-stack/BitLocker evidence. |
| Driver | Overview, operations, debug output, object information, integrity, module Cross-View, Unloaded/PiDDB | Driver-service registration/load/unload/delete, loaded modules, DBWIN output, DriverObject/DeviceObject/MajorFunction/FastIo, Driver Integrity, and read-only evidence pages. |
| Kernel | Object namespace, atom table, NtQuery, SSDT, SSSDT, Inline Hook, IAT/EAT, CID, IPC, DynData, driver status, callbacks | Recursive object directories, BaseNamedObjects, NamedPipe, symbolic links, device/driver objects, object-type matrix, CID/cross-view, ALPC/IPC, dynamic offsets, capability matrix, and callback enumeration/management. |
| Monitor | Process targeting, direct kernel calls, WinAPI, WMI, ETW, Risk Center | Target-process-tree ETW, syscall capture, WinAPI Agent, WMI subscriptions, ETW provider/session management, and ARK risk aggregation. |
| Hardware | Utilization, overview, CPU, GPU, memory, disk monitoring, device management, R0 device audit | Task-Manager-style performance views, dynamic disk/network/GPU cards, process I/O and ETW file activity, SetupAPI/CfgMgr device tree, and DevNode/USB/HID/PCI/ACPI/GPU/display/watchdog audit. |
| Privileges | Accounts, privileges | Local users, create user/reset password, group information, and the current process privilege snapshot. |
| Windows | Window list, desktop/window details, Win32k/GUI, hotkeys/hooks, clipboard, GPU/display | Window enumeration, filtering, preview, picking, control, desktop management, message monitoring, and structured win32k GUI/session plus hotkey/hook audit. |
| Registry | Tree, value list, search results | Registry browsing, key/value CRUD, `.reg` import/export, asynchronous search, and navigation. |
| Handles | Handle list, object types, object details | PID/keyword/type filtering, named-object resolution, object-type statistics, and HandleTable/ObjectHeader/ObjectType evidence. |
| Startup | Overview, logon, services, drivers, scheduled tasks, advanced registry, WMI | Categorized startup overview, icon rendering, filtering/export, file and registry location lookup, actionable deletion, and navigation to service management. |
| Services | Main service table, general, logon, recovery, dependencies, audit | Service filtering/sorting, startup-type changes, start/stop/pause/continue, property editing, dependency/audit information, and TSV/JSON export. |
| Miscellaneous | Boot, context-menu cleanup, disk editing, application control | BCD/boot entry points, Shell context-menu cleanup, read-only disk editing by default (writes require unlocking), and AppLocker/WDAC/Defender/ASR/platform-security/event-log diagnostics. |

### Auxiliary Panel Docks

| Panel | Key Areas | Primary Capabilities |
|---|---|---|
| Current Operations | Task cards | Shows the steps and progress of background tasks, then hides automatically when complete. |
| Log Output | Level filters, log table, context menu | Log filtering, copy/export, double-confirmation clearing, and GUID call-chain tracing. |
| Immediate Window | Code/text editor | Quick verification, temporary notes, and immediate output. |
| Monitor Panel | CPU/memory/disk/network charts | Bottom real-time performance monitor with multi-line throughput trends. |

## KswordARKLight (Lightweight Edition)

`KswordARKLight` is a lightweight ARK for earlier systems, low-resource environments, and rapid-response scenarios:

- It is implemented in native Win32/C++; its entry point, docks, controls, themes, and placeholder pages do not depend on Qt.
- Modules load on demand: inexpensive placeholder pages are created at startup, then real pages are materialized when a dock is activated to reduce startup stalls.
- Current modules cover processes, memory, registry, files, drivers, kernel, monitoring, hardware, windows, startup items, networking, handles, and miscellaneous security.
- It reuses the `shared/driver/` protocols and `ArkDriverClient`-style wrappers, and is connected to real KswordARK driver calls.
- `DriverService` can restore `KswordARK.sys` from EXE resources when needed, then install, start, stop, and query the service through SCM.
- Recent updates include lightweight kernel-UI layering; monitoring, window, startup, file, and process icons; process-difference highlighting; driver-page details; and unload IOCTL support.

## KswordSetup Installer

`KswordSetup` is a convenience installer for the release package, not a runtime requirement:

- The build script embeds the `Release\` payload into the installer as RCDATA.
- During installation, it extracts files, can optionally write appearance/startup settings, and creates desktop and Start-menu shortcuts.
- It can trigger UAC when needed for system-level options such as Task Manager replacement and test signing.
- If a complete `Release\` directory is already available, extracting it to a target directory and running the main program is functionally equivalent.

## Architecture Notes

- Shared R0/R3 protocols live under `shared/driver/`; new IOCTL headers, structures, and version fields must not be scattered across UI or driver-private directories.
- Driver IOCTL handlers are registered through `KswordARKDriver/src/dispatch/ioctl_registry.c`. `ioctl_dispatch.c` is limited to lookup, validation, invocation, logging, and request completion.
- User-mode access to the KswordARK device goes through `Ksword5.1/Ksword5.1/ArkDriverClient/` or the corresponding lightweight wrapper. Dock/UI code must not open the device or issue raw `DeviceIoControl` calls.
- PDB/DynData uses profile packs generated by `tools/pdb_offset_generator/`. Release packages should prioritize `ark_dyndata_pack_v3.json`; the driver also supports v4 typed-item apply/query and capability/missing-item diagnostics.
- R0 features that depend on undocumented fields must declare their required capability. The dispatch layer applies the gate before the handler; missing DynData or a mismatched profile must degrade safely or fail closed.
- Audit pages should be read-only by default. Unload, delete, patch, bypass, disk-write, and similar mutation actions require a separate entry point, risk notice, and rollback/audit strategy.
- The first DynData phase uses the vendored System Informer dynamic-offset data in `third_party/systeminformer_dyn/`. Ksword integrates only the `KphDynConfig` data and a lightweight parser, not the KPH communication layer, object system, or session tokens.
- The DynData R0/R3 protocol is centralized in `shared/driver/KswordArkDynDataIoctl.h`. KernelDock's Dynamic Offsets page uses `ArkDriverClient` to show profile matches, field sources, and capability gates; when the driver loads, the main window automatically refreshes and applies the DynData profile.
- Process extended information uses the v2 protocol in `shared/driver/KswordArkProcessIoctl.h` to provide session, full image path, Protection/SignatureLevel, ObjectTable/SectionObject availability, field source, and DynData capability. ProcessDock/ProcessDetail shows availability only and does not enumerate the handle table or Section directly when DynData is missing.
- Recoverable R0 process hiding uses `IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY`. The driver changes `_EPROCESS.UniqueProcessId` and unlinks `ActiveProcessLinks` while retaining the PspCidTable record, allowing restoration by the original PID.
- The unified driver status/capability protocol is in `shared/driver/KswordArkCapabilityIoctl.h`. KernelDock's Driver Status page shows Driver Loaded/Missing, Protocol Mismatch, DynData Missing, Limited, security policy, the most recent R0 error, and the feature-capability matrix.
- R0 PPL changes require `KSW_CAP_PROCESS_PROTECTION_PATCH`. The user-mode confirmation dialog must show the current/target Protection, SignatureLevel impact, field source, and rollback risk.
- New source files must be added to the corresponding `.vcxproj` and `.vcxproj.filters` files. Third-party integrations must keep their upstream license text.

## Repository Layout

- `Ksword5.1/`: main solution and full Qt application.
- `KswordARKLight/`: lightweight native Win32 ARK.
- `KswordARKDriver/`: kernel driver.
- `shared/driver/`: shared IOCTL protocol headers.
- `KswordCLI/`: command-line tool.
- `KswordSetup/`: optional installer and payload-generation scripts.
- `Taskbar/`: top AppBar, status display, and `S O S Enter` quick launch of the main application.
- `KswordHUD/`: HUD helper application.
- `APIMonitor_x64/`: API Monitor injection and monitoring component.
- `tools/pdb_offset_generator/`: PDB offset/profile-pack generation and validation tools.
- `docs/pdb_r0_audit_prep/`: PDB/R0 audit-preparation and acceptance documents.
- `third_party/systeminformer_dyn/`: vendored System Informer DynData snapshot with LICENSE/NOTICE and Ksword wrapper headers.
- [KSwordDEV/Website](https://github.com/KSwordDEV/Website): independently maintained product website and module documentation.

## Build Requirements

- Windows 10/11 x64.
- Visual Studio 2022, MSVC, and MSBuild.
- Qt 6.9.3 `msvc2022_64` for the full Qt application and helper UI projects. `KswordARKLight` does not require Qt.
- WDK is required for building the kernel driver.

## Quick Build

Use the repository script first to discover and store the local Qt path, avoiding machine-specific paths in individual `.vcxproj` files:

```powershell
# Run from the repository root; replace the path with the local Qt installation
.\Setup-QtPaths.ps1 -QtDir 'C:\Qt\6.9.3\msvc2022_64'

# Replace this MSBuild path for the local Visual Studio installation; a Developer PowerShell can use msbuild directly
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'

# Build the full solution, including the main application, Taskbar, HUD, driver, CLI, installer, and lightweight edition
& $msbuild '.\Ksword5.1\Ksword5.1.sln' /t:Build /p:Configuration=Debug /p:Platform=x64 /m
```

Build only the lightweight ARK:

```powershell
& $msbuild '.\KswordARKLight\KswordARKLight.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

Build the native launcher and generate the readable release support manifest:

```powershell
& $msbuild '.\Launcher\Launcher.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

If the current machine does not have a WDK or driver-signing environment, build the user-mode projects first. The release process can reuse an existing unsigned R0 release artifact.

## Release and Run

- A release archive should contain a `Release\` root with `Launcher.exe`, `Ksword5.1.exe`, `KswordARKLight.exe`, helper programs, driver, `profiles\launcher_support_manifest.json`, the DynData packs, Qt dependencies, and Qt plugin directories. Shortcuts and the installer post-install action launch `Launcher.exe`.
- `KswordSetup.exe` is optional. Extracting `Release\` and running `Launcher.exe` is the supported entry point; it chooses `Ksword5.1.exe` or `KswordARKLight.exe` after checking compatibility.
- R0 features require administrator privileges, a running driver service, and compatible system security settings. Test-signing and driver-signing requirements depend on the target system.

## Project Website

The [KSwordDEV/Website](https://github.com/KSwordDEV/Website) repository independently maintains the project website and module introductions.

## Notice

This project includes system-level debugging, auditing, and management capabilities. Use it only in legally authorized and compliant environments.

## License

Ksword is free software under [GNU GPL version 3 only](LICENSE). Use it, study it, change it, share it, sell it, or build services around it—just follow GPLv3 and provide the matching source when you distribute binaries. Third-party components keep their own licenses.

Except where a file says otherwise, Ksword's own code is GPLv3-only. The [Ksword Community Covenant](COMMUNITY_COVENANT.md) is about honesty, attribution, responsible use, and not pretending an unofficial fork is official. It is a community promise, not another layer of license restrictions. Contributions follow [CONTRIBUTING.md](CONTRIBUTING.md).

## Star History

<a href="https://www.star-history.com/?repos=KSwordDEV%2FKSword&type=timeline&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=KSwordDEV/KSword&type=timeline&theme=dark&legend=top-left&sealed_token=z3lDzSCqYcrsHevfwWR-A7GrLv3t7XcjgLhW0yK13FvqFRvYRmGjEY5XNCqGHUE2-mvPVsKjV5EAbAtI7OTPisLY0q0652qBc5LJxbRZQpTYfSMqfcLy1r6MTWGj-JLLrObXIFhsqr5QQpxZJb8h5R5LvFURf-3JusERKoUegibW7nq8ADYLJ2FFdhvy" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=KSwordDEV/KSword&type=timeline&legend=top-left&sealed_token=z3lDzSCqYcrsHevfwWR-A7GrLv3t7XcjgLhW0yK13FvqFRvYRmGjEY5XNCqGHUE2-mvPVsKjV5EAbAtI7OTPisLY0q0652qBc5LJxbRZQpTYfSMqfcLy1r6MTWGj-JLLrObXIFhsqr5QQpxZJb8h5R5LvFURf-3JusERKoUegibW7nq8ADYLJ2FFdhvy" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=KSwordDEV/KSword&type=timeline&legend=top-left&sealed_token=z3lDzSCqYcrsHevfwWR-A7GrLv3t7XcjgLhW0yK13FvqFRvYRmGjEY5XNCqGHUE2-mvPVsKjV5EAbAtI7OTPisLY0q0652qBc5LJxbRZQpTYfSMqfcLy1r6MTWGj-JLLrObXIFhsqr5QQpxZJb8h5R5LvFURf-3JusERKoUegibW7nq8ADYLJ2FFdhvy" />
 </picture>
</a>