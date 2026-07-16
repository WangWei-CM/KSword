# Contributing to Ksword ARK

## 许可与贡献规则

本仓库按 GNU General Public License version 3 only（`GPL-3.0-only`）发布。
提交前请阅读根目录 `LICENSE`、`PROJECT_LICENSE.md` 和
`COMMUNITY_COVENANT.md`。社区公约只管理官方社区参与和项目身份，不构成 GPL
附加限制。

向官方仓库提交 Pull Request 或通过仓库指定的其他贡献渠道提交材料，即表示：

- 你有权提交该材料，且不会侵犯他人的版权、专利、商业秘密、合同或其他权利；
- 你保留原创贡献的版权，并同意该贡献作为项目的一部分按
  `GPL-3.0-only` 向所有接收者发布；
- GPLv3 第 11 节规定的贡献者必要专利许可适用于你的贡献；
- 你理解项目和所有下游接收者可以在 GPLv3 下使用、修改、托管、销售和再分发
  你的贡献；这不是版权转让，也不授权他人冒充你的作者身份；
- 第三方内容必须在 Pull Request 中逐项说明来源、准确版本、上游地址、许可证和
  适用文件，并保留完整 LICENSE/NOTICE。提交行为不能改变第三方许可证。

如果你不能接受按 `GPL-3.0-only` 发布，请不要提交代码或其他可受版权/专利保护
的材料；可以仅提交不包含实现材料的问题描述或功能建议。未来如需把你的贡献
改用不同许可证，仍须取得你的同意或有其他充分权利依据。

### 社区参与

提交和讨论应遵守 `COMMUNITY_COVENANT.md`。维护者可以基于社区规则管理官方仓库、
拒绝贡献或取消官方认可，但这些措施不影响任何人已经依据 GPLv3 取得的软件权利。

### 第三方许可检查

引入或改写第三方实现前，必须先检查它与 GPLv3 及最终组合产物是否兼容。
仅把第三方文件放到独立目录、附上 LICENSE，或笼统声明“第三方许可优先”，并不能
解决静态链接、动态链接或派生作品产生的许可证冲突。

Pull Request 至少要回答：

- 这是仅参考公开接口/思想，还是复制、翻译、改写、链接了第三方表达或二进制？
- 第三方许可证是否与 GPLv3 兼容，是否要求公开对应源码、允许替换/重新链接，
  或提供安装信息？
- 发布包是否携带完整许可证、版权声明、NOTICE、对应源码或有效的源码取得方式？
- 商业许可证是由谁持有、覆盖哪些版本/模块/发布主体，是否留有可审计记录？

GPL 兼容性未知、来源不明、仅凭网页口头许可或无法证明商业授权的依赖，在结论
明确之前不得合入可发布产物。AGPL 组件还必须单独评估其网络交互义务。当前仓库
的组件级结论见 `docs/许可证兼容性审计.md`。

## 模块边界

- 共享 IOCTL 协议只放在 `shared/driver/`。
- 驱动新 IOCTL 先在 `KswordARKDriver/src/dispatch/ioctl_registry.c` 注册，再在 `src/features/<module>/<module>_ioctl.c` 实现 handler。
- 用户态 R0 调用只通过 `Ksword5.1/Ksword5.1/ArkDriverClient/`。Dock UI 不直接调用 KswordARK `DeviceIoControl`。
- 新增源码必须加入对应 `.vcxproj` 和 `.filters`。
- 第三方代码必须保留 LICENSE/NOTICE。
- DynData 共享协议只能维护在 `shared/driver/KswordArkDynDataIoctl.h`；驱动侧不要复制结构体定义。
- 统一驱动状态/能力协议只能维护在 `shared/driver/KswordArkCapabilityIoctl.h`；KernelDock 能力页只通过 `ArkDriverClient::queryDriverCapabilities()` 获取状态。
- System Informer DynData 只允许作为 `third_party/systeminformer_dyn/` 数据源接入，禁止顺手搬入 KPH 对象系统、通信层、session token 或 System Informer IOCTL。
- 依赖未公开内核字段的功能必须通过 DynData capability gating 判断，不要在业务功能里散落新增硬编码偏移。
- 新增依赖私有偏移的 IOCTL 必须在 `KswordARKDriver/src/dispatch/ioctl_registry.c` 的 `RequiredCapability` 填写对应 `KSW_CAP_*`；无依赖时才使用 `KSWORD_ARK_IOCTL_CAPABILITY_NONE`（也就是 `0ULL`）。
- 进程扩展信息统一走 `shared/driver/KswordArkProcessIoctl.h` v2；Protection、SignatureLevel、ObjectTable、SectionObject 等 EPROCESS 字段只能来自 DynData/Runtime resolver，并在 UI 展示字段来源。
- PPL 修改属于高风险 R0 写字段动作，必须依赖 `KSW_CAP_PROCESS_PROTECTION_PATCH`，并在用户态二次确认中展示当前值、目标值、签名级别联动和回滚风险。

## 合并冲突控制

- 协议头变更先合并。
- `.vcxproj`/`.filters` 变更由单一 owner 集中合并。
- 不同 owner 避免同时修改同一个 Dock 大文件。
- R3 监控相关施工期间，如果任务只涉及 R0/DynData，请不要编译主程序、Taskbar 或 HUD；确需验证时先和当前 R3 owner 对齐。
