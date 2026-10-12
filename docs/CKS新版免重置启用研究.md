# 新版 Windows 的 CKS 免重置启用研究

研究日期：2026-10-12。范围为已有 Windows 安装中的 Custom Kernel Signers（CKS）首次启用，以及旧 SSDE 方法在 2026 年系统上的适用边界。本记录只做文献、源码和本机状态只读核对，没有执行策略部署、修改许可数据、改变 Secure Boot/HVCI、加载驱动或重启。

## 结论

微软现行文档（2026-08-04 更新）仍要求首次启用时先部署签名策略再重置 Windows；没有给出已有安装免重置首次启用的例外。其“更新策略无需干净安装”指已启用后的策略维护。AMD64 支持范围为 24H2 或更高并安装 2026 年 4 月非安全更新或更新补丁，Home 不支持；文档没有把关闭 HVCI 列为统一前提。[微软 CKS 文档](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers)

**存在公开的免重置实验报告，但没有足够证据把它承诺为新版 Windows 上可靠、受微软支持的启用功能。** 继续研究应核对新版 CI 的锁定状态及 PK/KEK 信任的基础策略，具体证据见下文。

旧 SSDE 主要维持产品许可项。新版 CI 的额外锁定条件不会由这个工具自动满足；许可值为 1、服务为 RUNNING 也不能代替目标驱动加载验收。[SSDE 启用器源码](https://github.com/valinet/ssde/blob/fce909e8e52909ad4d0cb17313382fb8435080b3/ssde_enable/enable.cpp)、[SSDE 驱动源码](https://github.com/valinet/ssde/blob/fce909e8e52909ad4d0cb17313382fb8435080b3/ssde/ssde.c)、[2026 年 issue #17](https://github.com/valinet/ssde/issues/17)

## 公开路径与证据强度

| 路径 | 证据日期与版本 | 机制与适用边界 | 判断 |
| --- | --- | --- | --- |
| 微软现行部署流程 | 文档 2026-08-04；受支持且已更新的 24H2 或更高 | PK/KEK 签名策略 + 首次重置 | 受支持路径；不能满足免重置首次启用 |
| Windows Setup 模式 + SSDE 保活 | 上游最后提交 2021-08-27；README 明确实测 22000.1 | 在 Windows 的特殊启动阶段修改产品许可；驱动监视许可变化并恢复该项 | 历史可行；不能据此断言 24H2/25H2 当前补丁可用 |
| issue #10 的测试签名引导 | 2024-09-22；正文没有精确 build | 先通过临时测试环境让保活驱动进入内核，再恢复正常安全启动 | 历史作者经验；没有当前补丁兼容性证明 |
| 新版 CI 锁定状态的离线实验 | 2026-07-31；作者描述 26200.8037 与 26200.8875 | 新版另检查受保护的首次启用状态，作者声称可在离线环境改变该状态后保持原安装 | 最贴近“新版免重置”的公开候选；未经本项目复现 |

表中微软流程来自 [微软 CKS 文档](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers)；SSDE 版本与源码基线来自 [README](https://github.com/valinet/ssde/blob/fce909e8e52909ad4d0cb17313382fb8435080b3/README.md) 和 [提交元数据](https://github.com/valinet/ssde/commit/fce909e8e52909ad4d0cb17313382fb8435080b3)；其余来自 [issue #10](https://github.com/valinet/ssde/issues/10) 和 [原作者报告](https://linux.do/t/topic/2683240?tl=zh_CN)。这里只记录机制和证据，不把改变受保护启用状态的实验转换成自动操作步骤。

Windows `SYSTEM\Setup` 的 Setup 模式，与 UEFI 固件的 `SetupMode` 是不同概念：前者是旧启用器使用的 Windows 启动状态，后者与固件 PK 管理及安全启动所有权相关。[SSDE 源码](https://github.com/valinet/ssde/blob/fce909e8e52909ad4d0cb17313382fb8435080b3/ssde_enable/enable.cpp)、[微软 Secure Boot 变量文档](https://learn.microsoft.com/windows-hardware/manufacture/desktop/windows-secure-boot-key-creation-and-management-guidance)

## 新版免重置报告能证明什么

2026-07-31 原帖声称在已有 25H2 安装中通过离线改变锁定状态免重置成功，仍要求安全启动及 PK/KEK 信任的基础策略。但正文用 `26200.*`、伪代码用 `26100.*`；作者承认改动引入补丁未知，未提供采样哈希。附件称宿主机 8875、虚拟机 8037，本次下载返回 404。两版本只能视为采样标签，不能当作精确门槛或外推到 26300.9022。[原作者报告及附件](https://linux.do/t/topic/2683240?tl=zh_CN)

另一个原始静态分析来源 Drift Corpus 记录了 CI 从“检查所有策略”改为“锁定状态 + 基础策略”的逻辑，涉及 `CipUpdateCustomKernelSignerEnabledState` 和 `SIPolicyAreCustomKernelSignersAllowed`。该分析由其自动逆向引擎产生，属于可复核的静态线索，不是微软确认或运行验收。[原始报告](https://drift.argus-systems.ai/item/ci_a8f3efcf-ci-report-20260628-222735.html)、[项目说明](https://github.com/ByteRay-AI/drift-corpus)

对应元数据给出的是 **26H1 分支**中的 `ci.dll 10.0.28000.2179 → 10.0.28000.2336`，不是 26200 采样。两者 SHA-256 分别为 `a8f3efcf032602899ca3b396af5e1a07fa20014b781a7e5fb764833987a6de9e`、`7548d7fc4c825b00be905d98b96626deb8f5f93ff6866a7cea5f091db0eba1ba`。[固定版本元数据](https://github.com/ByteRay-AI/drift-corpus/blob/238bc048902d78e43fbabc43c5a6d38683049e43/corpus/ci_a8f3efcf/metadata.json)

Corpus 的日期归属还需复核：数条不同月份记录复用同一 `patched` 文件，部分 `prev.kb_numbers` 指向更晚 KB；因此不以报告的 KB 标签推导精确引入日期。[同库 2 月样本元数据](https://github.com/ByteRay-AI/drift-corpus/blob/238bc048902d78e43fbabc43c5a6d38683049e43/corpus/ci_85c5a4da/metadata.json)、[6 月样本元数据](https://github.com/ByteRay-AI/drift-corpus/blob/238bc048902d78e43fbabc43c5a6d38683049e43/corpus/ci_a8f3efcf/metadata.json)

## SSDE 在新系统上的反例

2026-04-07 的 issue #16 报告 Windows 11 25H2 启动修复认定 `ssde.sys` 为无法使用的关键启动文件；没有精确 build 或确认解决。后续评论调整驱动启动阶段的目的只是让系统可以启动，不证明签名信任已恢复。[issue #16](https://github.com/valinet/ssde/issues/16)

2026-05-28 的 issue #17 报告 25H2、build 26200，`Licensed=1`、`CustomKernelSignerLockedEnabled=1`、SSDE 已启动，但其他自签驱动仍返回 577，并记录 HVCI/VBS 运行及 CI 事件。评论只是建议检查 HVCI/VBS，未确认原因或成功解决；这既不能证明 HVCI 一律不兼容，也足以说明两个注册表值不能作为完整成功条件。[issue #17](https://github.com/valinet/ssde/issues/17)

2025-06-17 的 issue #14 有作者关闭 HVCI 配置后解决其个案的报告，缺少 build、策略与驱动身份；它是配置相关失败线索，不能扩展成所有新版系统必须关闭 HVCI 的要求。[issue #14](https://github.com/valinet/ssde/issues/14)

抽查 fork 时，`qwerty12/ssde` 的 README 仍为旧 22000.1 说明；`Transwarpcom/ssde` 新增说明的明确测试版本是 23H2 22631.3085。没有从这些说明获得当前 26300 构建的免重置验收证据。[qwerty12 README](https://github.com/qwerty12/ssde/blob/woa_main/README.md)、[Transwarpcom README](https://github.com/Transwarpcom/ssde/blob/master/README.md)

## 相关替代方案的边界

测试签名可以用于驱动开发，修改启动配置后需要重启；安全启动可能保护该设置，HVCI 开启时仍要求驱动二进制有测试签名。这是测试环境的加载机制，不构成 CKS 的 PK/KEK 策略登记。[微软测试签名文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)

Windows Driver Policy 限制 WHCP 签名及获允许的旧交叉签名驱动。微软已给出关闭该策略的方式，2026 年 7 月及更新补丁可保持 Secure Boot 操作；这只是解除该附加策略，不能直接建立对任意自签证书的 CKS 信任。需要按 CI 事件中的 Policy ID 区分两类失败。[微软 Windows Driver Policy 说明](https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy)

## 本机只读快照与下一步研究

以下值由本次主 agent 在 2026-10-12 的只读查询采集：

| 项目 | 当前值 | 解释 |
| --- | --- | --- |
| Windows | ProfessionalWorkstation；DisplayVersion 26H2；26300.9022 | 新于公开 25H2 采样；不等同于已支持其操作 |
| `ci.dll` | FileVersion 10.0.26100.8941 | 系统显示 build 与组件文件版本应分别记录 |
| `ci.dll` SHA-256 | `B8CC50FBD7C5E916FCCBB87F577485AB461FA8FFA8870E7599405E50C125659D` | 识别本机精确静态研究对象 |
| 当前控制集 `CI\Protected\Licensed` | 1 | 许可缓存已开启；仍不代表 CKS 可以加载驱动 |
| 当前控制集 `CI\Protected\CustomKernelSignerLockedEnabled` | 0 | 与新版研究报告中的额外启用条件相关 |
| `UEFISecureBootEnabled` | 1 | 读取到安全启动开启状态 |
| HVCI `Enabled` 配置 | 1 | 配置值；本条不证明 HVCI 当前运行状态 |

**推断：** 本机出现 `Licensed=1` 而锁定状态为 0，值得核对精确 `ci.dll` 的初始化与策略验证逻辑。仍需检查实际 PK/KEK 所有权、策略是否为基础策略、签名链与驱动兼容性；不能仅凭注册表项存在或版本数字推定旧 SSDE 可用。本次未执行真实驱动加载或跨重启验收。

可继续做的只读研究是：以文件版本、哈希和 PDB 身份核对本机 CI 中锁定状态的产生与消费者；把其条件和公开 25H2/26H1 报告逐项比较；记录策略列表、PK/KEK 信任关系与实际运行的 HVCI 状态。若以后开展运行验证，需要在明确版本与可恢复的独立环境中验证目标驱动、重启保持和累积更新后的行为，才能形成兼容性结论。
