驱动程序每一句都必须要有注释；
任何警告都是不可容忍的。
单个文件长度不允许超过1000行，不允许通过.inc堆叠，而要采用合理的.h和.cpp格式组织

R0 与 R3 的通信协议（IOCTL 常量、输入输出结构体）只能放在 `shared/driver/`，禁止在 `KswordARKDriver` 内部重新定义副本。
R0 功能必须遵循以下落点：
- `shared/driver/`：唯一 R0/R3 协议目录，新增 IOCTL 常量和输入输出结构体必须先放在这里。
- `KswordARKDriver/src/dispatch/ioctl_registry.c/.h`：只注册 IOCTL 与 handler，不写业务逻辑。
- `KswordARKDriver/src/dispatch/ioctl_validation.c/.h`：放通用 WDF buffer/access 校验。
- `KswordARKDriver/src/dispatch/ioctl_dispatch.c`：只负责查表、调用 handler、统一日志和完成请求。
- `KswordARKDriver/src/features/<module>/<module>_ioctl.c`：放模块自己的 IOCTL handler。
- `KswordARKDriver/src/features/<module>/`：放真实业务实现，例如进程、文件、内核、callback 等。
所有新加代码默认要求 `Warning as Error` 下零警告；若新增右值/格式化/类型转换，必须显式消除潜在告警再提交。


## 多人协作边界

- 新增驱动 IOCTL 时，优先新增/修改对应 `src/features/<module>/<module>_ioctl.c`，不要把业务 case 写回 `ioctl_dispatch.c`。
- 新增协议头必须集中放入仓库根目录 `shared/driver/`，驱动和用户态共同 include。
- 新增 `.c/.h` 必须同步进入 `KswordARKDriver.vcxproj` 和 `KswordARKDriver.vcxproj.filters`。
- 第三方代码接入必须保留 LICENSE/NOTICE，正式位置使用仓库相对路径，不写个人机器绝对路径。
