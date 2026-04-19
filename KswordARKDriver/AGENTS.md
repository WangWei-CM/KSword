驱动程序每一句都必须要有注释；
任何警告都是不可容忍的。
单个文件长度不允许超过1000行，不允许通过.inc堆叠，而要采用合理的.h和.cpp格式组织

R0 与 R3 的通信协议（IOCTL 常量、输入输出结构体）只能放在 `shared/driver/`，禁止在 `KswordARKDriver` 内部重新定义副本。
R0 结束进程功能必须遵循以下落点：
- `shared/driver/KswordArkProcessIoctl.h`：协议定义。
- `KswordARKDriver/Queue.c`：IOCTL 分发与参数校验。
- 内核执行路径：`Queue.c` 内部调用 `ZwTerminateProcess`，并写入驱动日志通道。
所有新加代码默认要求 `Warning as Error` 下零警告；若新增右值/格式化/类型转换，必须显式消除潜在告警再提交。
