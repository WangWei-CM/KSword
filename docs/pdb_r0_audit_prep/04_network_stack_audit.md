# PDB R0 Network Stack Audit Prep

> Scope: tcpip/afd/netio/ndis/wfplwfs/fwpkclnt/http/nsiproxy PDB-based, read-only R0 network audit design.
>
> Hard boundary for this prep note: no source implementation, no bypass/disable logic, no WFP/NDIS mutation plan. This document is a field map for later protocol/UI work.

## 0. Inputs read during prep

### Repository sources

- `shared/driver/KswordArkNetworkIoctl.h`
  - Current R0 network protocol only exposes `IOCTL_KSWORD_ARK_NETWORK_SET_RULES` and `IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS`.
  - Current rule model supports allow/block/hide-port rule snapshots for TCP/UDP/ANY, direction, PID, local/remote ports.
- `KswordARKDriver/src/features/network/network_ioctl.c`
  - `SET_RULES` requires write access and logs rule changes.
  - `QUERY_STATUS` is fixed-output, read-only, and returns runtime flags/rules/counters.
- `KswordARKDriver/src/features/network/network_runtime.c`
  - Current runtime stores WFP registration state, rule snapshot, generation, classify count, blocked count.
  - `KswordARKNetworkShouldHidePort()` already exists as a policy hook, but current repo does not have R0 TCP/UDP table enumeration.
- `KswordARKDriver/src/features/network/network_wfp.c`
  - Current R0 WFP use is ALE connect and recv-accept callout/filter registration.
  - It is a policy/classify path, not a WFP inventory/audit path.
- `Ksword5.1/Ksword5.1/ksword/network/network_connection_tools.h`
  - Existing R3 connection snapshot uses `GetExtendedTcpTable`, `GetExtendedUdpTable`, and `SetTcpEntry(DELETE_TCB)` for IPv4 TCP termination.
- `Ksword5.1/Ksword5.1/NetworkDock/NetworkDock.ConnectionManage.cpp`
  - Existing UI has TCP and UDP tables: PID, process name, local endpoint, remote endpoint, TCP state.
- `Ksword5.1/Ksword5.1/NetworkDock/NetworkFirewallPage.*`
  - Existing Qt UI already has WFP net-event history/live view and Windows Firewall rule management through user-mode WFP/COM APIs.
- `docs/OpenArk功能对照与TODO.md`
  - Network TCP/UDP is covered at R3 level, but WFP/NDIS/LSP/filter-chain style enumeration remains a P1 gap.

### PDB cache observations

Read-only inspection under `E:/KswordPDB/PDB/pdb-cache/amd64` found folders for all target modules:

- `tcpip.pdb`
- `afd.pdb`
- `netio.pdb`
- `ndis.pdb`
- `wfplwfs.pdb`
- `fwpkclnt.pdb`
- `http.pdb`
- `nsiproxy.pdb`

Quick string/symbol evidence from cached PDBs:

- `tcpip.pdb`: `TcpEnumerateListeners`, `UdpEnumerateEndpoints`, `TcpCreateEndpoint`, `UdpCreateEndpoint`, `InetLookupPortEndpoint`, `InsertEndpointToPerProcEndpointTable`, `WfpAleEndpointCreationHandler`.
- `afd.pdb`: `AfdEndpointListHead`, `AfdConstrainedEndpointListHead`, `AfdPollListHead`, `AfdCreateConnection`, `AfdCreateConnection`, `AfdGetConnectionReferenceFromEndpoint`, `AfdTransportInfoListHead`.
- `netio.pdb`: `FeInitCalloutTable`, `FeRegisterCalloutEntry`, `FeGetNextFilter`, `NmrfpGetNextFilter`, `KfdRegisterCalloutEntry`, `IoctlKfdBeginEnumFilters`, `WfpQueryLayerStats`.
- `fwpkclnt.pdb`: `FwpsCalloutRegister0/1/2/3`, `FwpmFilterAdd0`, `FwpmFilterGetById0`, `FwpmFilterGetByKey0`, `FwpmCalloutEnum0`, `FwpmProviderEnum0`, `FwpmSubLayerGetByKey0`.
- `ndis.pdb`: `ndisFindMiniportOnGlobalList`, `ndisEtwRundownMiniports`, `ndisEtwRundownFilterDrivers`, `ndisEtwRundownProtocolDrivers`, `ndisAttachFilter`, `ndisDetachFilter`, `NdisFRegisterFilterDriver`, `NdisRegisterProtocolDriver`, `NdisMRegisterMiniportDriver`.
- `http.pdb`: `UlpCreateRequestQueue`, `UlpFindRequestQueue`, `UlQueryRequestQueueIoctl`, `UlAddUrlToConfigGroup`, `UlRemoveUrlFromUrlGroupIoctl`, `UlpAllocateServerSession`, `UxpTlCreateListenEndpoint`, `UxpTlFindListenEndpoint`.
- `nsiproxy.pdb`: `NsippEnumerateObjectsAllParameters`, `NsippGetAllParameters`, `NsiProxyDeviceObject`, imports for `NsiGetAllParametersEx`, `NsiEnumerateObjectsAllParametersEx`, `NsiGetParameterEx`.

These names are candidates, not contracts. Later implementation must resolve per-build layouts through PDB type/global lookups and fail closed when a structure or global is absent.

---

## 1. Design principles

1. **Read-only first.** All P0/P1 items in this document are inventory/cross-view features. They must not disable WFP filters, detach NDIS filters, mutate endpoint state, or patch kernel lists.
2. **PDB gated.** Each module reader must expose `pdbMatched`, `moduleTimestamp`, `imageBase`, `symbolSetId`, and per-field `fieldAvailable` flags.
3. **Fail closed.** If a symbol, type, field offset, list head, or lock discipline is unknown, return a degraded status instead of walking guessed memory.
4. **Cross-view instead of replacement.** R0 tables should be compared against public API views (`GetExtendedTcpTable`, `GetExtendedUdpTable`, user-mode WFP APIs, adapter APIs, HTTP service configuration), not used as the only truth.
5. **No stealth enablement.** Hidden-connection detection is for reporting anomalies. It must not provide a bypass/restore/disable action path in MVP.
6. **Module isolation.** Future source work should add a new network-audit protocol rather than overloading existing `NETWORK_SET_RULES` policy IOCTL.

---

## 2. P0 capability list

### P0.1 TCP endpoint kernel table audit

| Item | Plan |
|---|---|
| PDB module | `tcpip.pdb` |
| Structure/global candidates | TCP listener/endpoint/TCB structures, per-partition endpoint/TCB hash tables, listener table roots. Candidate symbol evidence: `TcpEnumerateListeners`, `TcpCreateEndpoint`, `TcpTlEndpointIoControlEndpoint`, `InetLookupPortEndpoint`, `InsertEndpointToPerProcEndpointTable`, `TcpRepartitionHashTables`. |
| R0 output fields | `rowId`, `addressFamily`, `protocol=TCP`, `localAddress`, `localPort`, `remoteAddress`, `remotePort`, `state`, `owningPid`, `owningProcessObject`, `endpointObject`, `tcbOrListenerObject`, `compartmentId`, `interfaceLuid/index`, `createTick`, `flags`, `pdbStatus`, `fieldMask`. |
| R3/UI fields | PID, process name/path, TCP state, local endpoint, remote endpoint, AF, compartment, interface, R0 object pointer, API presence, anomaly flags. |
| Public API cross-view | Compare against existing `GetExtendedTcpTable(AF_INET/AF_INET6, TCP_TABLE_OWNER_PID_ALL)` rows used by `ksword/network/network_connection_tools.h`. Key by AF + local/remote address + local/remote port + state + PID when available. |
| Risk/degrade | TCP internals are version-sensitive and may require partition/lock-aware traversal. If list/hash roots or fields are missing, report `PDB_MISSING_FIELD` and skip R0 rows. Avoid holding internal locks unless symbol/type review proves safe; prefer snapshot-safe traversal or bounded guarded reads. |
| Data source | `tcpip.pdb` candidates; existing R3 TCP implementation in `network_connection_tools.h`; UI table in `NetworkDock.ConnectionManage.cpp`. |
| Acceptance | On a clean machine, R0 TCP rows should match all R3 `GetExtendedTcpTable` rows within a refresh window. Mismatches must be classified: `R3_ONLY`, `R0_ONLY`, `PID_MISMATCH`, `STATE_MISMATCH`, `STALE_SNAPSHOT`, `PDB_DEGRADED`. |

### P0.2 UDP endpoint kernel table audit

| Item | Plan |
|---|---|
| PDB module | `tcpip.pdb` |
| Structure/global candidates | UDP endpoint tables and per-process endpoint table candidates. Symbol evidence: `UdpEnumerateEndpoints`, `UdpCreateEndpoint`, `UdpBindEndpointRequestInspectComplete`, `UdpTlProviderEndpoint`, `InsertEndpointToPerProcEndpointTable`, `LookupEndpointFromPerProcEndpointTable`. |
| R0 output fields | `rowId`, `addressFamily`, `protocol=UDP`, `localAddress`, `localPort`, `owningPid`, `owningProcessObject`, `endpointObject`, `compartmentId`, `interfaceLuid/index`, `flags`, `pdbStatus`, `fieldMask`. |
| R3/UI fields | PID, process name/path, local endpoint, AF, compartment, interface, endpoint object, API presence, anomaly flags. |
| Public API cross-view | Compare against existing `GetExtendedUdpTable(AF_INET/AF_INET6, UDP_TABLE_OWNER_PID)` rows. Key by AF + local address + local port + PID when available. |
| Risk/degrade | UDP wildcard and dual-stack sockets can collapse multiple user-mode rows into one kernel endpoint or vice versa. Degrade by showing multiplicity and `wildcardAddress` flags instead of forcing one-to-one matching. |
| Data source | `tcpip.pdb`; existing `EnumerateUdpEndpointRecords()`; `NetworkDock` UDP table. |
| Acceptance | R3 UDP rows must appear in R0 or be explained as wildcard/dual-stack/permission/PDB-degraded. R0-only UDP rows should show enough endpoint/PID data to triage hidden or stale entries. |

### P0.3 Hidden connection detection

#### GetExtendedTcpTable vs tcpip kernel table

- **Detection key:** AF + protocol + local endpoint + remote endpoint + TCP state + PID.
- **Suspicious cases:**
  - `R0_ONLY`: present in tcpip endpoint/TCB/listener table but absent from `GetExtendedTcpTable`.
  - `R3_ONLY`: present in public API but no R0 object; often stale refresh or PDB walk miss.
  - `PID_MISMATCH`: endpoints match but owning PID differs.
  - `STATE_MISMATCH`: same 4-tuple but state differs beyond accepted refresh drift.
- **R0 output addition:** `apiCrossViewStatus`, `apiMatchedRowId`, `hiddenScore`, `hiddenReason`.
- **Acceptance:** Create normal TCP listen/established rows and verify `MATCHED`. Force refresh-race tolerance by taking R3 and R0 snapshots in one command with monotonic timestamps.

#### AFD handle vs endpoint

- **Detection key:** process handle table entries referencing AFD file/socket objects vs AFD endpoint/connection objects vs tcpip endpoint objects.
- **Suspicious cases:**
  - AFD handle exists but no tcpip endpoint (`AFD_ONLY`).
  - tcpip endpoint exists but no owning process AFD handle (`TCPIP_ONLY`), acceptable for kernel/system endpoints but suspicious for user sockets.
  - AFD endpoint points to unusual transport/connection object or freed-looking object.
- **Risk:** AFD object layouts are fragile. MVP should only correlate when PDB fields are available and object type/name validation succeeds.
- **Acceptance:** For a test process with TCP listen, UDP bind, and established TCP, UI can open endpoint detail and show AFD/tcpip relation or a clear degraded reason.

#### WFP callout abnormal address

- **Detection key:** WFP callout classify/notify/flowDelete function address -> owning kernel module range.
- **Suspicious cases:**
  - callout function pointer outside any loaded image.
  - pointer inside unsigned/unknown/non-driver memory.
  - provider/callout metadata missing while a callable function remains registered.
  - callout registered in unexpected layer or with unusual action/filter relation.
- **Acceptance:** All Microsoft callouts resolve to signed Microsoft modules; third-party callouts resolve to their drivers; unresolved pointers are flagged but not modified.

---

## 3. P1 capability list

### P1.1 AFD endpoint/socket object audit

| Item | Plan |
|---|---|
| PDB module | `afd.pdb` |
| Structure/global candidates | AFD endpoint, connection, poll, transport info lists. Symbol evidence: `AfdEndpointListHead`, `AfdConstrainedEndpointListHead`, `AfdPollListHead`, `AfdTransportInfoListHead`, `AfdCreateConnection`, `AfdGetConnectionReferenceFromEndpoint`, `AfdFreeEndpointResources`. |
| R0 output fields | `afdEndpointObject`, `afdConnectionObject`, `fileObject`, `owningPid`, `processObject`, `socketType`, `addressFamily`, `transportProtocol`, `local/remote endpoint if available`, `state`, `pollFlags`, `transportName`, `tcpipEndpointObject`, `fieldMask`. |
| R3/UI fields | PID/process, socket type, protocol, AFD object, file object, endpoint relation, poll state, related tcpip row, anomaly flags. |
| Public API cross-view | Correlate with process handle enumeration for `\Device\Afd` handles, `GetExtendedTcpTable`, `GetExtendedUdpTable`, and existing process/handle modules. |
| Risk/degrade | AFD may keep endpoint objects after handle cleanup or during async close. Handle table walk needs existing handle capabilities and object reference safety. Degrade to object-pointer-only when socket fields are unavailable. |
| Data source | `afd.pdb`; existing handle/process facilities; existing R3 TCP/UDP tables. |
| Acceptance | A normal socket process shows at least one AFD object linked to process/PID and to a TCP/UDP row when applicable. Orphan classifications must include lifecycle caveat. |

### P1.2 WFP provider/sublayer/filter/callout inventory

| Item | Plan |
|---|---|
| PDB modules | `netio.pdb`, `fwpkclnt.pdb`, optionally `wfplwfs.pdb` for lightweight filter/provider context. |
| Structure/global candidates | Filter engine callout/filter/layer tables. Symbol evidence: `FeInitCalloutTable`, `FeRegisterCalloutEntry`, `FeGetNextFilter`, `NmrfpGetNextFilter`, `KfdRegisterCalloutEntry`, `IoctlKfdBeginEnumFilters`, `FwpmCalloutEnum0`, `FwpmProviderEnum0`, `FwpmFilterGetById0`, `FwpmSubLayerGetByKey0`. |
| R0 output fields | `objectKind` provider/sublayer/filter/callout/layer, GUID, name/description if available, layer key/id, provider key, sublayer key, filter id, weight, action type, callout id/key, classify/notify/flowDelete addresses, owning module, flags, effective state, fieldMask. |
| R3/UI fields | WFP object type, display name, GUID, layer, action, weight, provider, sublayer, callout function module, trust/signature, anomaly flags. |
| Public API cross-view | Compare against user-mode `FwpmProviderEnum0`, `FwpmSubLayerEnum0`, `FwpmFilterEnum0`, `FwpmCalloutEnum0` where available. Existing `NetworkFirewallPage` already opens BFE and enumerates events/rules; future UI can share concepts but not code in this prep. |
| Risk/degrade | BFE user-mode view can differ from kernel effective filters. R0 must not delete, disable, toggle, or reorder filters. If internal WFP locks cannot be safely acquired, use exported enumeration IOCTL-like paths only when documented/safe or return degraded. |
| Data source | `network_wfp.c` current Ksword callout registration; `NetworkFirewallPage.*`; `netio.pdb`/`fwpkclnt.pdb` evidence. |
| Acceptance | R0 inventory sees KswordARK callouts registered by current driver and common Windows callouts. Cross-view row counts should be explainable; every callout function address must map to a loaded module or be flagged. |

### P1.3 NDIS miniport/filter/protocol/binding inventory

| Item | Plan |
|---|---|
| PDB module | `ndis.pdb`; consider `wfplwfs.pdb` when mapping WFP lightweight filter involvement. |
| Structure/global candidates | NDIS miniport global lists, filter driver/block chains, protocol/open bindings. Symbol evidence: `ndisFindMiniportOnGlobalList`, `ndisEtwRundownMiniports`, `ndisEtwRundownFilterDrivers`, `ndisEtwRundownProtocolDrivers`, `ndisAttachFilter`, `ndisDetachFilter`, `NdisFRegisterFilterDriver`, `NdisRegisterProtocolDriver`, `NdisMRegisterMiniportDriver`. |
| R0 output fields | `adapterLuid`, `ifIndex`, `miniportBlock`, `miniportDriver`, `miniportName`, `pnpDeviceInstanceId`, `mediaType`, `operStatus`, `filterChain[]` with filter module/name/order/state, `protocolBindings[]`, `bindingObject`, `driverObject`, `imageBase`, `fieldMask`. |
| R3/UI fields | adapter name, ifIndex/LUID, miniport driver, filter chain order, protocol bindings, driver path/signature, state, anomaly flags. |
| Public API cross-view | Compare with `GetAdaptersAddresses`, `GetIfTable2`, SetupAPI network adapter class, WMI/CIM `MSFT_NetAdapter`, and PowerShell-style adapter/filter bindings when later implemented. |
| Risk/degrade | NDIS chain is highly concurrent and packet-path sensitive. MVP must only enumerate passive snapshots, never pause/restart/detach filters, never alter binding order. Degrade to public adapter-only rows if PDB or lock-safe traversal is unavailable. |
| Data source | `ndis.pdb`; OpenArk TODO P1 filter-chain gap; existing network diagnostics pages for adapter-facing context. |
| Acceptance | UI can show each active adapter with miniport and ordered filters. Counts must align with public adapter APIs, and every filter driver object must map to a loaded image or be flagged. |

### P1.4 NSI cross-view

| Item | Plan |
|---|---|
| PDB module | `nsiproxy.pdb`, with `netio.pdb` for provider-side NSI/NMR context. |
| Structure/global candidates | NSI proxy device/provider dispatch and parameter enumeration paths. Symbol evidence: `NsiProxyDeviceObject`, `NsippEnumerateObjectsAllParameters`, `NsippGetAllParameters`, imports for `NsiEnumerateObjectsAllParametersEx`, `NsiGetAllParametersEx`, `NsiGetParameterEx`. |
| R0 output fields | NSI table/module id, object key tuple, parameter blobs decoded when safe, source provider, address family/protocol, PID if present, endpoint state, fieldMask, decodeStatus. |
| R3/UI fields | NSI row type, tuple, PID, decoded state, matching R3 IP Helper row, matching tcpip R0 endpoint, anomaly flags. |
| Public API cross-view | Compare NSI-derived endpoint rows with `GetExtendedTcpTable`/`GetExtendedUdpTable` and tcpip R0 endpoint rows. |
| Risk/degrade | NSI internal parameter layouts are versioned and may be opaque. Treat unknown tables as raw counted objects only; avoid interpreting unrecognized blobs. |
| Data source | `nsiproxy.pdb`; existing R3 IP Helper enumeration. |
| Acceptance | For supported builds, NSI endpoint counts correlate with IP Helper rows. Unknown table ids produce a readable degraded row, not a failed IOCTL. |

---

## 4. P2 capability list

### P2.1 HTTP.sys request queue / URL group feasibility

| Item | Plan |
|---|---|
| PDB module | `http.pdb` |
| Structure/global candidates | Request queue, server session, URL group, listen endpoint and SSL endpoint config objects. Symbol evidence: `UlpCreateRequestQueue`, `UlpFindRequestQueue`, `UlQueryRequestQueueIoctl`, `UlAddUrlToConfigGroup`, `UlRemoveUrlFromUrlGroupIoctl`, `UlpAllocateServerSession`, `UxpTlCreateListenEndpoint`, `UxpTlFindListenEndpoint`, `UxpSslEnumerateEndpointConfigs`. |
| R0 output fields | `requestQueueObject`, queue id/name if available, owning process, server session id, URL group id, registered URL prefix, listen endpoint, SSL binding/cert hash if safely available, request counters, fieldMask. |
| R3/UI fields | HTTP queue, process, URL prefix, URL group, listen endpoint, SSL binding summary, queue counters, anomaly flags. |
| Public API cross-view | Compare with HTTP Server API configuration (`HttpQueryServiceConfiguration` for URL ACL/SSL/IP listen) and `netsh http show servicestate/urlacl/sslcert` style outputs when later implemented. |
| Risk/degrade | HTTP.sys internals are large and request queues are active. P2 should start as feasibility/prototype only; no request cancellation, no queue flushing, no URL deletion. |
| Data source | `http.pdb`; existing network/HTTPS proxy UI concepts for endpoint presentation. |
| Acceptance | Feasibility pass can list URL ACL/SSL public API data first, then mark which R0 HTTP queue fields are PDB-resolvable on the tested build. No MVP dependency. |

### P2.2 wfplwfs correlation

| Item | Plan |
|---|---|
| PDB module | `wfplwfs.pdb`, plus `ndis.pdb`/`netio.pdb`. |
| Structure/global candidates | WFP lightweight filter NDIS integration objects, filter module attachment context, layer/flow offload interactions. |
| R0 output fields | WFP LWF filter instance, related miniport, filter state, driver image, bind order, related WFP layers/callouts when resolvable. |
| R3/UI fields | A row or edge in NDIS filter chain graph showing WFP LWF placement and relation to WFP callouts. |
| Public API cross-view | Compare with NDIS adapter/filter binding public view and WFP callout inventory. |
| Risk/degrade | This is correlation glue, not a standalone security verdict. If `wfplwfs` structures are opaque, show only loaded module and NDIS filter placement. |
| Data source | PDB folder presence; NDIS and WFP candidate symbols. |
| Acceptance | If present on the OS, graph identifies WFP LWF in the NDIS chain without changing filter state. |

---

## 5. Safety boundary for WFP/NDIS read-only enumeration

### WFP boundary

Allowed:

- Enumerate provider/sublayer/filter/callout metadata.
- Resolve callout function pointers to loaded modules.
- Cross-view R0 effective objects with user-mode BFE/FWPM enumeration.
- Flag suspicious pointers, missing metadata, unusual layers, and third-party drivers.

Forbidden for this audit feature:

- `FwpmFilterDelete*`, `FwpmCalloutDelete*`, `FwpmSubLayerDelete*` except existing KswordARK cleanup path unrelated to this audit.
- Toggling filter activation, changing weights, disabling providers, or modifying classify actions.
- Patching callout function pointers or flow contexts.
- Offering UI buttons named disable/remove/bypass in MVP.

Acceptance for boundary:

- Code review can grep future network-audit files and find no delete/disable/toggle WFP calls.
- UI only exposes copy/export/open-module/detail actions.

### NDIS boundary

Allowed:

- Enumerate miniports, filters, protocols, bindings, and driver image identity.
- Build a graph showing filter order and protocol binding relation.
- Compare against public adapter/binding views.

Forbidden for this audit feature:

- Detach/pause/restart/remove filters or miniports.
- Changing binding order.
- Sending OID set requests for policy or filter manipulation.
- Packet path hooks, packet injection, or traffic dropping.

Acceptance for boundary:

- Future implementation contains no calls equivalent to detach/pause/restart/filter removal for audit commands.
- R0 IOCTL is marked query-only and uses read access; UI has no mutation verbs.

---

## 6. Proposed R0/R3 data model

### R0 protocol shape, future only

Do not extend current `IOCTL_KSWORD_ARK_NETWORK_SET_RULES` policy IOCTL. Add separate query-only IOCTLs later, for example:

- `QUERY_NETWORK_AUDIT_SUMMARY`
- `ENUM_TCP_ENDPOINTS`
- `ENUM_UDP_ENDPOINTS`
- `ENUM_AFD_ENDPOINTS`
- `ENUM_WFP_OBJECTS`
- `ENUM_NDIS_GRAPH`
- `ENUM_NSI_ENDPOINTS`
- `ENUM_HTTP_OBJECTS` (P2)

Common response header fields:

- `version`, `size`, `status`, `lastStatus`
- `moduleMask`, `pdbMatchedMask`, `degradedMask`
- `entrySize`, `totalCount`, `returnedCount`
- `snapshotId`, `snapshotTick`, `refreshWindowMs`
- `requiredCapabilityMask`

Common row fields:

- `sourceMask`: `R0_TCPIP`, `R0_AFD`, `R0_WFP`, `R0_NDIS`, `R0_NSI`, `R3_API`
- `crossViewStatus`: `MATCHED`, `R0_ONLY`, `R3_ONLY`, `MISMATCH`, `DEGRADED`
- `riskFlags`: hidden, orphan, invalid pointer, unsigned owner, stale, unknown layout
- `fieldMask`: which optional fields were decoded from PDB-backed structures

---

## 7. UI prework recommendations

### P0: Connection cross-view table

Columns:

- Severity / CrossView
- Protocol
- State
- PID
- Process
- Local endpoint
- Remote endpoint
- R3 API presence
- tcpip R0 presence
- AFD presence
- NSI presence
- Object pointers
- Reason

Data source:

- R3: existing `GetExtendedTcpTable` / `GetExtendedUdpTable` path.
- R0: future tcpip/afd/nsi query-only IOCTLs.

Acceptance:

- Existing TCP/UDP connection table can be conceptually extended without losing current columns.
- A normal row shows `MATCHED`; anomalies are filterable.

### P1: WFP callout table

Columns:

- Risk
- Layer
- Provider
- Sublayer
- Filter id/name
- Action
- Callout id/key
- ClassifyFn
- NotifyFn
- FlowDeleteFn
- Owner module
- Signature/trust
- R3 FWPM presence

Data source:

- R0: `netio/fwpkclnt` inventory.
- R3: existing `NetworkFirewallPage` WFP concepts plus future FWPM object enumeration.

Acceptance:

- KswordARK's own ALE connect/recv-accept callouts are visible and point into `KswordARK.sys`.
- Microsoft callouts resolve to Microsoft modules; unknown pointers are flagged.

### P1: NDIS filter chain graph

Display:

- Adapter/miniport nodes.
- Ordered lightweight filter nodes.
- Protocol binding nodes.
- Edge labels for bind state/order.

Data source:

- R0: `ndis.pdb` miniport/filter/protocol/binding inventory.
- R3: adapter APIs and SetupAPI/WMI adapter metadata.

Acceptance:

- At least one active NIC shows miniport -> filters -> protocols.
- WFP LWF placement is shown when available.

### P0/P1: Endpoint detail drawer

Sections:

- Public API row.
- tcpip object fields.
- AFD object/handle fields.
- NSI fields.
- Owning process and module trust.
- Snapshot/degrade notes.

Acceptance:

- Selecting a row explains why it is matched/anomalous/degraded without requiring kernel debugging knowledge.

---

## 8. MVP

### MVP scope

P0 MVP should include:

1. TCP endpoint R0 table query using `tcpip.pdb` where symbols/types resolve.
2. UDP endpoint R0 table query using `tcpip.pdb` where symbols/types resolve.
3. Cross-view merge with current R3 `GetExtendedTcpTable` / `GetExtendedUdpTable` rows.
4. Hidden-connection classification fields and UI table design.
5. Read-only only; no TCP termination changes and no policy mutation changes.

Optional P0.5 if time allows:

- AFD object correlation for selected endpoint details only, not a full AFD inventory table.

### MVP non-goals

- No WFP disable/remove.
- No NDIS detach/pause/restart.
- No HTTP.sys queue mutation.
- No packet capture rewrite.
- No new blocking/hiding policy beyond the existing network rule IOCTL.

### MVP acceptance standards

- **Data source acceptance:** every row includes source flags showing whether it came from R3 API, tcpip R0, AFD R0, NSI R0, or WFP/NDIS later modules.
- **PDB acceptance:** each query exposes PDB match/degrade fields; unsupported builds fail with a readable degraded status.
- **Cross-view acceptance:** on a normal test machine, TCP and UDP public API rows are matched by R0 rows or explained.
- **Hidden detection acceptance:** artificially stale/race rows are not falsely labeled hidden without a reason; `R0_ONLY` rows show object pointer and source.
- **Safety acceptance:** audit IOCTLs are read-only and require no write access; UI offers no mutation buttons.
- **Performance acceptance:** endpoint enumeration is bounded by caller-supplied max rows and returns partial results with continuation/snapshot metadata if needed.
- **Regression acceptance:** existing `NetworkDock` TCP/UDP tables and firewall event/rule pages can continue to function independently of R0 audit availability.

---

## 9. Phasing summary

### P0

- TCP endpoint kernel table.
- UDP endpoint kernel table.
- R3 IP Helper cross-view.
- Hidden connection detection: tcpip vs `GetExtendedTcpTable`, AFD selected-detail correlation, WFP callout abnormal-address rules.
- UI: connection cross-view table and endpoint detail drawer.

### P1

- Full AFD endpoint/socket object inventory.
- WFP provider/sublayer/filter/callout table.
- NDIS miniport/filter/protocol/binding graph.
- NSI endpoint cross-view.

### P2

- HTTP.sys queue/server-session/URL-group feasibility.
- `wfplwfs` deeper correlation in the NDIS/WFP graph.
- Rich public API parity views for HTTP and adapter binding details.

---

## 10. Final caution notes

- The PDB candidate names in this document are discovery anchors, not stable ABI.
- Do not infer offsets from names alone; use PDB type records and runtime module identity checks.
- Treat network stack state as highly concurrent. Snapshot metadata and degradation are part of the product, not an implementation embarrassment. Tiny goblin of truth: network rows will race; the UI should explain races instead of pretending time froze politely.
