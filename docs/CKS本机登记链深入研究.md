# CKS 本机登记链深入研究

研究日期：2026-10-12。目标是寻找保留当前 Windows 安装的 CKS 首次启用路径。本轮重新核对第一方接口，并分析本机 `ci.dll`、`winload.efi`、`skci.dll` 和 `vbssysprep.dll` 的副本，以及安装迁移清单；完整文件身份、现场状态、函数定位与汇编复核见 [本机证据](CKS本机登记链证据-20261012.json)。本轮没有修改系统、部署策略、加载驱动或重启。

## 结论

**找到机制相关的免重置候选，尚未验证成功流程：启动前重新建立 CKS 登记，以及可能触及该登记的保留应用安装。** 所查代码区分“登记尚未建立”与“已经登记为关闭”，首次初始化本身没有调用整机重置；这支持研究免重置可能性，不能直接当作操作成功证明。

本机普通策略刷新不能把当前已登记的关闭状态转换为首次启用。引导器与 CI 都优先消费既有登记状态，SKCI 也消费启动登记结果。保留应用的修复重装尚未证明会重新建立登记；如果继承当前状态，下面的执行链仍会拒绝 CKS。未发现公开、受支持的免重置首次登记接口，也不能据此证明所有安装组件均不存在相关行为。

## 精确对象与现场

| 对象 | 文件版本 | PE 内嵌 RSDS 身份 |
| --- | --- | --- |
| `ci.dll` | 10.0.26100.8941 | `ci.pdb`；81938abc-afd1-575f-511f-ffaf1515f2a4；Age 1 |
| `winload.efi` | 10.0.26100.9022 | `winload_prod.pdb`；b1c103b5-afe1-1c92-3b37-b4cbed88d500；Age 2 |
| `skci.dll` | 10.0.26100.9022 | `skci.pdb`；edd07cb8-5b33-6221-ea9e-190813f3df69；Age 1 |
| `vbssysprep.dll` | 10.0.26100.9022 | `VbsSysprep.pdb`；afe0aef4-b891-4629-6762-4c090e72fc34；Age 1 |

文件 SHA-256 已在证据中固定，不能将 OS 显示 build 26300.9022 当成上述组件版本。本机 `Licensed=1`、`CustomKernelSignerLockedEnabled=0`，Secure Boot 运行状态为 1；CI 查询返回 `0xF701`，包含 HVCI 运行及严格态。许可缓存不等于实际 CKS 信任。

进一步直接查询 `NtQueryLicenseValue("CodeIntegrity-AllowConfigurablePolicy-CustomKernelSigners")` 成功，返回 `REG_DWORD=1`、长度 4。这确认当前产品许可接口也允许该能力，不只是旧缓存为 1；当前研究应集中在登记与策略信任链。定点查询 `sc.exe query ssde` 返回 1060，本机没有此确切服务名；未扫描其它驱动或服务，不能外推全部历史安装情况。

CiTool 查询返回 `0x80070005`，没有取得本轮有效策略清单；CKS 实际信任诊断字段未采集。下文对当前状态的判断来自登记值与静态链路，不能表述为直接测得内核 CKS 运行位。实际 PK/KEK 所有权及私钥可用性也未核验。

分析使用已安装 IDA 9.1；9.3 打开后退出，未将其当成成功分析。以下定位均为 RVA，映像基址为 `0x180000000`。RSDS 来自 PE 调试目录，未单独审核 PDB 文件哈希；它用于识别文件，不单独证明全部反编译类型正确。

## 引导与 CI 的登记链

`winload!OslInitializeCodeIntegrity`（`0x176E0`）读取 Protected 登记字段，已有关闭值不会选入 CKS。`OslBuildCodeIntegrityLoaderBlock`（`0x1980C`）将 `SIPolicyAreCustomKernelSignersAllowed`（`0x2430D0`）的结果放入交给 CI 的 `0x8` 位；升级标志 `0x1` 独立，所查分支没有“升级即覆盖既有关闭登记”的逻辑。

CI 的 `CiInitializePolicy`（`0x54EE0`）通过 `CipCheckLicensing`（`0x55830`）进入 `CipUpdateCustomKernelSignerEnabledState`（`0x18CB4`）。该函数优先使用既有 DWORD；首次登记尚未建立时才消费调用者传入的初始候选。所查直接调用者唯一。`SIPolicyAreCustomKernelSignersAllowed`（`0x75B94`）和对应检查函数（`0x75C18`）在锁定状态关闭时立即拒绝，之后还要求基础策略及合格签名条件。

与旧 SSDE 的许可维持路线相比，本机许可读回已通过，而既有关闭登记仍独立生效；继续围绕许可缓存刷新不能解决这一剩余条件。免重置实验的判据应是引导器和 CI 能否重新取得一致、合格的登记结果，再核对 SKCI 与目标驱动，而不是仅看到某个许可值为 1。

`CipUpdateCiSettingsFromPolicies`（`0x6AE24`）仅在 `g_CiDeveloperMode` 已有 CKS 位开启时重算运行状态，未调用首次登记写入者。因此“策略已部署、刷新成功”不足以跨过当前登记门禁。

隔离侧 `skci!SkciInitialize`（`0xDD40`）从传入启动块的许可位与 CKS 位初始化本地登记门禁；`SIPolicyAreCustomKernelSignersAllowed`（`0x1E798`）先检查该门禁。门禁字节的直接代码引用只发现初始化写入与允许条件读取，两处重要分支已用汇编复核。`SkciSetCodeIntegrityPolicy`（`0x459E0`）走 `CipRefreshPolicies`（`0x43210`）；`CipUpdateCiSettingsFromPolicies`（`0x43958`）同样只在启动已建立的 CKS 位开启时重算。所查隔离侧更新入口没有建立另一套首次登记能力；这也不能外推为 HVCI 一律必须关闭。

引导器的 PK / KEK 验签函数（`0x1A1504`、`0x1A1624`）读取实际 UEFI 信任库并调用 `BlpSbdiCheckPolicyIsUefiSigned`。这两段 Hex-Rays 输出误呈现为恒定失败；关键汇编已复核，实际返回验签结果。不能用该反编译误差推导“PK/KEK 路径失效”。

本机 Sysprep 注册清单指向 `vbssysprep.dll` 的 Generalize（`0x13D60`）、Specialize Online（`0x14D40`）和 Offline（`0x14910`）入口；所查代码为 HVCI/CET/VBS 配置职责，未见 CKS 专属入口。这项核对限于已知组件及入口。

## 安装迁移的新线索

本机 WinSxS 中的 10 份 CodeIntegrity 清单采用 DCM/PA30 压缩，已成功在内存解码并解析 XML。两份主 CI 清单（1746、9022）的迁移规则一致：保留 `CI` 下的 `IntegrityLevelPolicy`、`UMCIAuditMode`，以及 `CI\Config`、`CI\Policy` 子树和策略文件；排除的是 `ManufacturingMode`。9022 原始解码 XML 的对应 Registry include 位于 260–263 行，exclude 位于 272 行，文件身份见证据。

`CI\Protected` 只出现在普通注册表默认定义中，没有进入这份组件迁移 include 或 exclude；`CustomKernelSignerLockedEnabled` 也没有显式条目。**因此目前不能证明保留应用安装会迁移当前关闭登记，也不能证明它会丢弃或重新建立登记。** 尚需区分组件迁移规则与 Setup 核心对 SYSTEM hive 的整体处理。这比仅依赖“升级中”标志更直接地指向安装候选的剩余问题。

在 `migwiz` 的 `dlmanifests`、`replacementmanifests` 两个明确目录中，定点检索了 258 份 `.man/.xml/.manifest`。相关命中为 `Tcblaunch-Replacement.man` 第 37 行，其 exclude 块排除的是 HVCI `EnabledBootId`，没有建立 CKS 登记的含义。`$WINDOWS.~BT\Sources` 顶层没有名称匹配的 CI/boot 清单，未读取 Panther/Rollback 用户安装日志。这些局部无命中不能外推为全部安装程序不存在相关操作。

解码字典只从已定位的 `wcp.dll` PE 资源读取，没有加载其代码；使用系统 `ApplyDeltaB` 在内存解码。压缩清单、原始解码 XML 和字典均记录 SHA-256，避免把压缩文本“搜不到”误判为没有规则。[ApplyDeltaB 文档](https://learn.microsoft.com/en-us/windows/win32/devnotes/msdelta-applydeltab)

## 入口候选与反证

| 候选 | 本轮判断 |
| --- | --- |
| CiTool、MDM、ConfigCI 正常部署/刷新 | 负责策略管理；本机刷新路径不建立首次登记。`ResetPolicyID` 只重置策略 GUID。 |
| Windows Update 修复重装、保留应用的就地安装 | 官方存在保留应用、文件和设置的流程；CI 自身迁移清单未列出 Protected，核心 SYSTEM hive 迁移行为仍待证。继承当前关闭值时无法启用。 |
| Sysprep / 再次 OOBE | 本机清单只发现 `vbssysprep` 的 HVCI/CET 配置入口，未见 CKS 专属入口；官方不支持在已部署系统上以 Sysprep 重新配置。 |
| 启动前重新建立登记 | 直接对应本机首次初始化分支，机制相关性最强；仍须满足启动许可、PK/KEK 信任的基础策略及实际驱动验证。属于非官方实验候选，本轮只读研究未验证。 |
| PBR 保留文件并恢复应用 | 可降低重置后的恢复成本，仍然重置系统，无法满足免重置目标。 |

微软现行 CKS 文档要求首次部署后重置；已启用后的策略更新免干净安装不能外推到首次登记。[CKS 部署文档](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers)

CiTool 的 `IsAuthorized` 表示 CI token 授权，不需要 token 时等于策略生效状态；CSP token 来源为 `OneCoreDeviceUnlockService`。这些接口没有公开本机自发放 CKS 登记资格的能力。[CiTool](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/operations/citool-commands)、[ApplicationControl CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/applicationcontrol-csp)

修复重装保留应用、文件和设置；Sysprep 则用于映像部署，已部署系统的重新配置不受支持。[修复重装](https://support.microsoft.com/en-us/windows/deployment/install-upgrade/fix-issues-by-reinstalling-the-current-version-of-windows)、[Sysprep 边界](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/sysprep--system-preparation--overview?view=windows-11)、[PBR 机制](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/how-push-button-reset-features-work?view=windows-11)

## 下一步的可核对问题

继续追踪保留应用安装流程对 CI 登记状态的迁移职责，区分普通升级、修复重装和 PBR。当前覆盖四份二进制及已知注册/迁移清单，尚未覆盖全部 Setup/恢复组件、Secure Kernel 宿主及完整驱动验证链；不能宣称修复安装候选已被完全排除。

实际信任可与第一方诊断字段 `State.Current.CiDeveloperMode_SiPolicyCustomKernelSigners`、`State.PolicyDetails.IsBasePolicy`、`PolicyAllowKernelSigners`、`SignatureType` 对照；CiTool 生效状态与注册表值仍不足以代替目标驱动加载验证。[CI 诊断字段](https://learn.microsoft.com/en-us/windows/privacy/required-diagnostic-events-fields-windows-11-24h2)

以上结论来自静态链路与只读现场，尚无本机首次登记成功、跨重启保持或累积更新兼容性的运行证据。后续安装候选应先在可恢复的独立环境中验证，再形成兼容性结论。
