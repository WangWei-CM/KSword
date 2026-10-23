# VBS, VTL0 privileged instructions, and page-fault delivery

Research date: 2026-10-18. Scope: public-source architectural research, primarily Intel x64. No driver was loaded, VMCS sampled, fault induced, or protection changed for this report. AMD uses VMCB/NPT rather than Intel VMCS/EPT; Intel field numbers must not be applied to AMD.

## Findings and evidence standard

**Public measurements support descriptor-table exiting being enabled for the physical host's VTL0, including an explicitly HVCI-enabled Windows 11 sample. They also support ordinary VTL0 page faults not being configured to exit in those samples.** Neither finding establishes a universal policy across Windows builds, processor models, configurations, or execution phases. The numerical observations and their provenance appear below.

The Windows **root partition is not VMX-root operation**: on an Intel physical VBS host, its VTL0 kernel still executes in VMX non-root operation under Hyper-V. Physical deployment does not remove VMCS controls from its execution. [Microsoft Hyper-V architecture](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/reference/hyper-v-architecture)

Three questions must remain separate: whether an instruction causes a VM exit, whether its requested architectural effect is accepted, and whether Windows subsequently treats the resulting state as valid. An intercepted instruction may be emulated successfully. Therefore, a changed SIDT result would establish an observed IDTR change, but would not establish that LIDT ran without hypervisor intervention. Similarly, execution of a page-fault handler would establish eventual delivery, not the absence of an earlier intercept and reinjection.

Evidence labels in this report mean:

- **Microsoft documented:** a published interface or Microsoft-authored architectural statement.
- **Researcher observed:** a named researcher's measurements or original artifacts, limited to their stated environment.
- **Architectural inference:** a conclusion calculated from documented hardware semantics and observed values.
- **Unknown:** the reviewed public material does not establish the requested fact.

## 1. LIDT/SIDT and VTL0 control state

**Microsoft documented.** TLFS exposes VSM to both root and guest partitions. Its private-state list includes IDTR, GDTR, CR0, CR3, and CR4; VTL transitions preserve separate instances. The secure-register-intercept interface includes `IdtrWrite` at bit 16 of `HV_REGISTER_CR_INTERCEPT_CONTROL`, configured through `HvX64RegisterCrInterceptControl`. This documents an interception capability, not its default enabled state, and is distinct from a raw VMCS execution control. Higher VTLs can arrange secure intercepts and respond by emulation or exception injection. None of these passages publishes one mandatory Windows VTL0 VMCS template. [Microsoft TLFS, Virtual Secure Mode: Virtual Processor State Isolation and Secure Register Intercepts](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/vsm)

**Intel documented.** Secondary execution-control bit 2 controls descriptor-table instruction exits, including LIDT/SIDT; primary bit 31 enables the secondary controls. Exception interception uses the exception bitmap. Page faults additionally use `PFEC_MASK` and `PFEC_MATCH`: equality in `(PFEC & PFEC_MASK) == PFEC_MATCH` follows bitmap bit 14; inequality reverses it. NMI exiting has its own pin-based control. EPT violations are a separate mechanism; eligible violations can become #VE when that feature is enabled. [Intel SDM Volume 3C, Table 25-7, sections 25.6.3, 26.2 and 29.3.3](https://cdrdv2-public.intel.com/789585/326019-sdm-vol-3c.pdf)

**Researcher observed, 2024.** Satoshi Tanda compared the root partition's VTL0 and VTL1 against a child partition. His stated setup is Windows 11 22H2, a ninth-generation Intel processor, root HVCI enabled, and child HVCI disabled; the child also used Windows 11 22H2 and VM configuration version 11.0. This is relevant host/root-partition evidence. It is not a paired experiment comparing the physical host against an otherwise identical nested VBS host. The article identifies the environment and links the raw debugger output. [Tanda, 2024-02-12, Setup](https://tandasat.github.io/blog/2024/02/12/hyper-v-configs.html#setup)

The following are selected raw values. VPID labels are specific to this capture, not universal VTL identifiers. [Tanda, original VMCS logs](https://gist.github.com/tandasat/3a60ee4cc5b9519cadf60393814918e9)

| Field | Root VTL0, VPID 1 | Root VTL1, VPID 2 | Child, VPID 3 |
| --- | --- | --- | --- |
| Pin controls | `0x3f` | `0x3f` | `0x3f` |
| Primary controls | `0x962069fa` | `0x962069fe` | `0xb5a06dfa` |
| Secondary controls | `0x5890af` | `0x1890af` | `0x1890ef` |
| Exception bitmap | `0x60002` | `0x60002` | `0x60002` |
| PF mask / match | `0 / 0` | `0 / 0` | `0 / 0` |
| CR0 mask | `0xffffffffffffffe1` | same | same |
| CR4 mask | `0xfffffffffffffe7f` | `0xfffffffffffff870` | same as VTL1 |
| Guest CS selector | `0x10` (CPL0) | `0x10` (CPL0) | `0x10` (CPL0) |

**Architectural inference.** Primary bit 31 and secondary bit 2 are set: descriptor-table exiting is active. EPT-violation #VE (secondary bit 18) is clear; root VTL0 MBEC (bit 22) is set. CR3/CR8 load/store exit bits are clear; CR0/CR4 remain masked. For CR0/CR4, writes differing from the corresponding read-shadow bits selected by the mask cause exits; the masks do not mean every read exits. [Intel SDM, section 25.6.6](https://cdrdv2-public.intel.com/789585/326019-sdm-vol-3c.pdf)

**Correction:** `0x60002` sets **#DB/#AC/#MC (1/17/18)**, not NMI (2). The blog's decoded table mislabels these. Pin-control bit 3 separately enables NMI exits. [Original numeric evidence](https://gist.github.com/tandasat/3a60ee4cc5b9519cadf60393814918e9)

**Newer corroboration, with a provenance limit.** Tanda's 2025 collection names Hyper-V `10.0.26100.3470`, Intel twelfth-generation and AMD Zen 4 processors, and identifies NTOS as VPID/ASID 1. Intel `intel_vpid_1.log` records primary `0x96a261fa`, secondary `0x045813be`, exception bitmap `0x60002`, PF mask/match zero, and the same CR0/CR4 masks as above. Descriptor-table exiting remains set; #PF remains clear. The README does not explicitly certify HVCI state, so this corroborates later Hyper-V behavior without replacing the explicitly HVCI-enabled 2024 observation. Its changed primary/secondary controls also caution against copying a complete historical template into a current claim. [Tanda, Hyper-V configurations, 2025 edition](https://gist.github.com/tandasat/673a3a19aefc23493e145dec82077368)

## 2. The VTL0 page-fault path

**Architectural inference for the recorded configurations.** With bit 14 clear and PF mask/match zero, ordinary guest NX/non-present #PF follows VTL0's IDT without exiting *because of that #PF*. This assumes no separate EPT conflict during page walking or IDT/handler delivery. A hypervisor-managed memory violation remains a distinct event.

The distinction matters even when the application's eventual symptom looks identical:

| Cause under investigation | What must be established |
| --- | --- |
| VTL0 page-table violation | The guest translation rejected the access; record the faulting address and page-fault error code. |
| EPT/SLAT protection violation | The second-stage permissions rejected access; inspect the hypervisor exit and protection ownership. |
| Fault ultimately delivered to Windows | Determine whether it originated as guest paging or was injected after hypervisor handling. |

**Microsoft documented.** TLFS states that a lower-VTL access violating protections established by a higher VTL generates an intercept for that higher VTL, which can choose a response such as faulting or emulation. Consequently, higher-VTL enforcement must not be described as an unconditional native #PF directly delivered to Windows. [Microsoft TLFS, Memory Access Violations](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/vsm#memory-access-violations)

**Unknown.** The reviewed sources do not publish a universal list of exception bits for all VBS machines. The measured list above is #DB/#AC/#MC, with NMI separately controlled. Neither a generic statement that “Hyper-V handles exceptions” nor an example from another hypervisor establishes that VTL0 #PF is always intercepted.

## 3. Physical hosts and nested VBS

**Microsoft documented.** Guest VBS can run with either nested virtualization or Guest VSM; Microsoft says Guest VSM is enabled by default for Hyper-V Gen2 VMs. Memory integrity is supported in Gen2 VMs and can coexist with nested virtualization. Therefore, “Gen2 VM with VBS” does not identify whether there is a nested L1 Hyper-V instance. Establish which configuration is actually under test. [Microsoft, Virtualization-based Security](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/oem-vbs); [Memory integrity deployment in virtual machines](https://learn.microsoft.com/en-us/windows/security/hardware-security/enable-virtualization-based-protection-of-code-integrity#memory-integrity-deployment-in-virtual-machines)

**Microsoft documented.** Nested TLFS distinguishes L0 Hyper-V on hardware, the Windows root operating system, a guest VM, a nested L1 hypervisor, and its L2 root operating system. Enlightened VMCS is a versioned interface between levels; its nonsynthetic fields correspond to physical VMCS encodings, but L0 can cache state and feature availability constrains its contents. The existence of that interface does not publish Windows' chosen VTL0 interception policy. [Microsoft TLFS, Nested Virtualization](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/nested-virtualization)

**Assessment.** A Gen2 label alone does not specify which VMCS is being inspected. A useful comparison must identify the outer host's physical control structure, the nested hypervisor's requested state, and the effective structure used while the nested VTL0 executes. Reporting only one of those can misattribute an exit to the wrong hypervisor.

No qualifying matched physical-versus-nested-VBS measurements were found for descriptor-table exiting, exception bitmap, CR masks, and CR3/CR8 exits. The 2024 child-partition column is valuable but its HVCI-disabled child does not answer that comparison. Likewise, source defaults in hvpp, HyperDbg, or another independent hypervisor are not evidence of Microsoft's settings.

| Comparison requested | Public result established here |
| --- | --- |
| Physical/root VTL0 descriptor-table exiting | Set in the explicitly HVCI-enabled 2024 measurement. |
| Nested-VBS VTL0 equivalent setting | No matched measurement found. |
| Exception bitmap and PF filters | Exact host values available; matched nested values unknown. |
| CR-access control differences | Host/root versus ordinary child differences available; nested-VBS differences unknown. |

## 4. Practical evidence and exclusions

**Historical researcher observation.** Rafal Wojtczuk's Black Hat 2016 paper, primarily studying Windows 10 Enterprise 1511 on Intel, reports an all-ones exception bitmap while unsigned **user-mode** code executes in the described code-integrity scheme (PDF page 5). That is a different privilege state and historical implementation. It cannot establish that all CPL0 exceptions are intercepted on modern MBEC systems, nor contradict the later CPL0 captures by itself. [Wojtczuk, Analysis of the Attack Surface of Windows 10 VBS](https://www.blackhat.com/docs/us-16/materials/us-16-Wojtczuk-Analysis-Of-The-Attack-Surface-Of-Windows-10-Virtualization-Based-Security-wp.pdf)

**Microsoft-authored conference material.** David Weston and Matt Miller's Black Hat USA 2016 slides describe Hyper Guard protection of descriptor-table registers, control registers, and key MSRs on PDF page 30. Page 31 separately describes Hyper-V trapping SIDT/SGDT to prevent address disclosure from CPL greater than zero. The second statement concerns user-mode disclosure and cannot be promoted into a claim that kernel-mode SIDT always faults. These are historical product statements, not current instruction-by-instruction acceptance criteria. [Windows 10 Mitigation Improvements, PDF pages 30–31](https://blackhat.com/docs/us-16/materials/us-16-Weston-Windows-10-Mitigation-Improvements.pdf)

An open-source driver contains the comment “hvci doesn't support LIDT instruction.” Its header also claims testing under HVCI/Core Isolation. However, that comment supplies no qualified physical-machine transcript, exact hypervisor build, instruction exit, readback, or failure attribution. It is an author assertion, not sufficient evidence of a universal block. [ekknod/acdrv, source header](https://github.com/ekknod/acdrv/blob/main/main.cpp)

A 2026 article titled *IDT Table Hijacking under VBS/HVCI/kCET* explicitly says that IDTR's virtual base remains unchanged. Whatever its other claims, it therefore does not establish successful LIDT replacement of IDTR. No operational technique from that article is needed for this architectural question. [Exploit Pack, 2026-06-26](https://www.exploitpack.com/blogs/research/idt-table-hijacking-under-vbs-hvci-kcet-in-windows-11)

**Search result:** no qualified public before/after transcript was found showing a driver changing IDTR with LIDT on a confirmed physical VBS machine. No qualified rejection transcript was found either. The VMCS dumps establish interception configuration, not rejection of every IDTR update. No reviewed BlueHat or OffensiveCon source closed this gap; absence of a qualifying report does not establish universal impossibility.

## 5. Verification plan and interpretation

The replacement of vector 14 with a counting stub is an **IDT hook**, even if temporary and even if it chains to the original handler. The original 2026-10-18 report supplied an evidence plan and a read-only snapshot observer. The later companion source includes the active experiment described below; its current behavior must not be described as read-only or accepted under a no-hooks scope.

For posture evidence, Microsoft's `Win32_DeviceGuard` documentation distinguishes configured services from running services: `VirtualizationBasedSecurityStatus=2` indicates running VBS, and `SecurityServicesRunning` containing `2` indicates running memory integrity. Record those separately from CPUID and from the debugger experiment. [Microsoft, Validate enabled VBS and memory integrity features](https://learn.microsoft.com/en-us/windows/security/hardware-security/enable-virtualization-based-protection-of-code-integrity#validate-enabled-vbs-and-memory-integrity-features)

For a future laboratory investigation, define the record before execution:

1. Identify the physical hardware, CPU/microcode, Windows and hypervisor builds, boot phase, Secure Boot, VBS/HVCI status, relevant kernel protections, and debugger configuration. Preserve the complete output and exact binary identities. A CPUID hypervisor-present indication alone is insufficient proof of active VBS/HVCI.
2. Identify the partition, VP, VTL, and nesting level for every control snapshot. Capture primary/secondary controls, exception bitmap, both PF filter fields, CR masks/shadows, and guest IDTR. Do not assume VPID 1 always means VTL0.
3. Correlate any observed instruction with the hypervisor exit trace and resulting guest architectural state. Distinguish an intercepted-and-emulated instruction from rejected execution and from direct execution. Timing measurements or SIDT readback alone cannot settle that distinction.
4. Classify any observed fault using the originating access, guest translation, second-stage permissions, exit information, and eventual Windows handler context. A counter is evidence of a handler visit; it cannot identify an earlier hypervisor decision by itself. Avoid treating arbitrary invalid kernel addresses as a controlled, recoverable paging workload.
5. Repeat the same evidence collection on the nested configuration while retaining both outer and inner identities. Report differences only after excluding mismatched builds, enabled features, debugger effects, and different capture phases.

The public `hvext` project supplies Hyper-V debugger inspection functionality, including current VMCS and EPT inspection. Its documentation requires the appropriate hypervisor context, including VMX-root operation for relevant Intel commands; ordinary VTL0 execution is not that context. Tool availability is not proof that a particular current system has been measured. [tandasat/hvext, Setup and Usage on Intel](https://github.com/tandasat/hvext)

Record failures as carefully as successes: preserve debugger stops and crash dumps, including exception/error codes and the last completed stage. A bugcheck or reset may prevent later logging or restoration; missing DbgPrint output cannot distinguish rejection, a transport/logging problem, or a crash. An observed successful restoration also does not prove that all later integrity checks would accept the preceding state.

The companion source [`docs/security/vbs-vtl0-observer/VbsVtl0Observer.c`](security/vbs-vtl0-observer/VbsVtl0Observer.c) records the Windows build, the CPUID hypervisor-presence bit and vendor signature, the raw `CodeIntegrityOptions` and its HVCI KMCI `0x400` flag, and two SIDT snapshots on one processor. A failed CI query keeps its NTSTATUS and reports the option bits as unmeasured. The flag is a Code Integrity observation, not proof that VBS is running; confirm the runtime state separately through `Win32_DeviceGuard`. [Microsoft, `SYSTEM_CODEINTEGRITY_INFORMATION`](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation)

The current companion driver is an active four-phase experiment. After Phase A it copies the current processor's IDT, replaces vector 14 with the assembly counting stub, attempts LIDT, and, if the new IDTR is observed, deliberately reads address 1 under SEH. Phase D checks the original IDTR on the same pinned processor and restores it if necessary. The assembly source remains part of the standalone project. It also persists the report under `HKLM\Software\VbsVtl0Observer`. These operations establish neither absence of hypervisor interception nor a current VMCS configuration.

CPU affinity is established before the original IDTR snapshot and retained through restoration. Every attempted LIDT is followed by an independent restoration check, including an attempt whose immediate SIDT failed. IDT storage and the original-handler pointer are released only after the original base and limit are confirmed. If restoration raises an exception or cannot be confirmed, the driver preserves those resources, clears `DriverUnload`, and returns successful initialization to keep its handler code resident; the report records `RESOURCES_RETAINED=1 UNLOAD_DISABLED=1`. This is an incomplete experiment with retained state until reboot, not successful recovery. `COMPLETE` requires a consistent Phase A, observed installation, the expected access exception with a counter increase across the controlled read, and confirmed restoration. Counter increments before that read do not satisfy it. A silently ignored LIDT, skipped fault phase, or divergent restore cannot produce `COMPLETE`.

Version boundary: the 2026-10-18 read-only revision (`ddc6b452`) was compiled and linked with direct HostX64 MSVC 14.44 and WDK 10.0.26100 using `/W4 /WX /kernel /GS /Qspectre`; its disassembly contained CPUID/SIDT and no LIDT. That historical build does not validate later revisions. Commit `8a1226ce` subsequently added the active LIDT/#PF experiment despite describing itself as read-only. This correction preserves that existing flow and fixes affinity, restoration lifetime, and result reporting. No compile-time or runtime switch is added. The current correction is checked only through source inspection and user-mode verification; no current driver build, signing, loading, VMCS measurement, or fault-delivery acceptance is claimed here.
