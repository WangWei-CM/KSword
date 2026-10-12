# CKS 管理页

入口：主程序 → 杂项 → CKS 管理。该页把微软现行的 Custom Kernel Signers 部署路径与 SSDE 旧版实验路径分开显示。它只管理本机的策略部署、状态和 SSDE 服务；不会自动生成证书、修改 UEFI 密钥、切换 Secure Boot/HVCI、重置 Windows 或删除签名策略。

## 系统状态如何解读

- **CKS 产品策略**：`SLGetWindowsInformationDWORD` 读取 `CodeIntegrity-AllowConfigurablePolicy-CustomKernelSigners`。值为 1 只说明许可查询值开启。
- **CI Licensed**：读取 `HKLM\SYSTEM\CurrentControlSet\Control\CI\Protected\Licensed`。查询失败显示未知，不当作 0。
- **安全启动与 HVCI 配置**：读取 Windows 对应状态键。HVCI 配置值不等同于运行中的 HVCI 状态。
- **SSDE 服务**：SCM 回报服务是否存在、是否 RUNNING、是否 BOOT_START；镜像路径必须以 `ssde.sys` 结尾才允许管理。
- **策略列表**：由系统 `CiTool --list-policies -json` 读取。列表的 `IsSignedPolicy` / `IsEnforced` 不证明 CKS 生效，也不识别某条策略是否包含目标内核签名者；`IsEnforced` 是当前生效字段，不等同于策略处于“强制而非审核”模式。最终应以目标驱动的真实加载结果及 Code Integrity 事件为准。

## 微软现行流程

1. 在受支持的 Windows 11 版本上准备自有 PK/KEK 链、更新签名者、内核签名者，以及包含现有 Windows 和驱动信任规则的 App Control 策略。微软目前列出的 AMD64 门槛是 Windows 11 24H2 加 2026 年 4 月非安全更新或更高更新；页面只用 build 26100 作粗门槛，不能判断补丁是否齐全。先审核模式验证；不要直接采用第三方预制策略覆盖本机规则。
2. 在策略中加入更新签名者与内核签名者规则，按微软要求转换成 `.cip` 并使用 PK/KEK 链中的密钥签名。页面不接收或保存私钥与密码。
3. 选择已签名 `.cip`，核对页面显示的 SHA-256，通过系统目录中的 `CiTool --update-policy` 部署或更新。页面不独立验证 `.cip` 的 PK/KEK 签名链或签名者规则；成功回执只表示 CiTool 接受部署。
4. 首次启用必须按微软文档进行 Windows 重置；普通重启不能代替。重置后重新读取策略并实际验证驱动加载、Code Integrity 事件、Windows 更新和其他驱动。

错误签名策略可能使 Windows 无法启动。签名策略更新必须遵守原策略中的更新签名者规则；本页不直接删除 EFI 分区中的策略。

## SSDE 实验流程

1. 自行准备 PK/KEK、签名 CI 策略和恢复手段。选择从 [valinet/ssde](https://github.com/valinet/ssde) 源码构建或已独立核对哈希的 `ssde_enable.exe`。页面只显示所选文件 SHA-256，不能验证其上游来源。
2. 确认 Windows `SYSTEM\Setup` 当前空闲后启动工具。工具在独立控制台处理 Setup 模式和重启；页面不会把“进程已启动”当作启用成功。
3. 重启返回后刷新系统状态。只有安全启动、CKS 产品策略和 CI Licensed 都明确开启，才可选择已签名、当前 Windows 信任的 `ssde.sys`。页面不覆盖现存同名文件或服务；新服务先以按需启动安装。
4. 通过 SCM 启动驱动，等到 RUNNING 再单独选择 BOOT_START。下次启动仍需重新验证。停用开机持久化只改回按需启动，不移除策略、文件或服务。

[SSDE issue #10](https://github.com/valinet/ssde/issues/10) 还记录了一种人工替代路径：在 UEFI 暂停 Secure Boot、安装驱动、开启测试签名并重启两次、检查 Licensed 与服务，再重新启用 Secure Boot。该操作依赖 UEFI 和恢复环境，页面仅展示步骤与状态，不自动执行。

## 资料与版本边界

- [微软 Custom Kernel Signers 文档](https://learn.microsoft.com/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers)：当前支持范围、签名策略、首次重置和验证要求。
- [微软 CiTool 命令](https://learn.microsoft.com/windows/security/application-security/application-control/app-control-for-business/operations/citool-commands)：部署和列出策略。
- [SSDE](https://github.com/valinet/ssde)、[Windows10-CustomKernelSigners](https://github.com/HyperSine/Windows10-CustomKernelSigners)、[Win11 知识库](https://github.com/MysticSnows/Custom-Kernel-Signer-Win11-KnowledgeBase)：历史实验路径。其报告不能替代当前 Windows build 的兼容性验收。

实现没有复制第三方源码或二进制，也没有打包 SSDE 工具。编译和静态检查无法验证 UEFI/PK/KEK、系统重置、签名链、实际驱动加载或跨重启持久性。
