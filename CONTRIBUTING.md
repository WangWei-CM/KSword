# Contributing to Ksword ARK

## 模块边界

- 共享 IOCTL 协议只放在 `shared/driver/`。
- 驱动新 IOCTL 先在 `KswordARKDriver/src/dispatch/ioctl_registry.c` 注册，再在 `src/features/<module>/<module>_ioctl.c` 实现 handler。
- 用户态 R0 调用只通过 `Ksword5.1/Ksword5.1/ArkDriverClient/`。Dock UI 不直接调用 KswordARK `DeviceIoControl`。
- 新增源码必须加入对应 `.vcxproj` 和 `.filters`。
- 第三方代码必须保留 LICENSE/NOTICE。

## 合并冲突控制

- 协议头变更先合并。
- `.vcxproj`/`.filters` 变更由单一 owner 集中合并。
- 不同 owner 避免同时修改同一个 Dock 大文件。
