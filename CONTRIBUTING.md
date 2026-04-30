# Contributing to Ksword ARK

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

## 合并冲突控制

- 协议头变更先合并。
- `.vcxproj`/`.filters` 变更由单一 owner 集中合并。
- 不同 owner 避免同时修改同一个 Dock 大文件。
- R3 监控相关施工期间，如果任务只涉及 R0/DynData，请不要编译主程序、Taskbar 或 HUD；确需验证时先和当前 R3 owner 对齐。
