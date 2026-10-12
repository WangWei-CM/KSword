# DynData v4 读取、提取与分模块回执

- 2026-10-11：协议/配置包含可选 `PspTerminateProcess`（1401），旧 GUI 的手写 ID switch 漏项，导致整份 NTOS profile 无效，EX/V4 都不下发。包格式合规不等于实际消费者能接受；回归必须执行生产 `loadV4ItemsFromJson`。
- 支持的特殊 ID 统一登记于 `shared/driver/KswordArkDynDataIoctl.h` 的 `KSW_DYN_V4_SUPPORTED_SPECIAL_ITEMS`；GUI 使用该头的 `KswordArkDynDataV4ItemIdSupported`，Python 发布器读取同一目录和数值宏。1004/1006 仍保留协议编号，但不进入接受目录。不要恢复独立的 GUI 数字名单。
- 原汇总会把 FLTMGR 成功的 7 项计入 NTOS 的已应用/状态，掩盖 NTOS 读取失败。现有兼容汇总只代表 NTOS；`profileModules` 分别记录 EX/V4 是否尝试、状态、本次接受及最终缓存接受，界面和复制报告共用。缓存接受不能覆盖本轮格式失败。
- 新增 21 个结构字段映射和 2 个句柄位域 shift 映射。`_SECTION.u1.ControlArea`、`_ALPC_PORT.u1.State` 必须按成员的确切 CodeView type ID 追踪匿名 union，不能按重复的 `<anonymous-tag>` 类型名匹配。
- `ObDecodeShift`/`ObAttributesShift` 是位位置，不是成员字节偏移；只从 `_HANDLE_TABLE_ENTRY` 的真实 LF_BITFIELD 元数据产生，并校验 64 位存储及对象指针位域终点。零偏移的 ALPC 成员合法，不能用真假值过滤。
- 当前内核 GUID `63950D24-7362-0017-E1FC-214544590A77`：profile 从 225 项增至 248 项，原有值及 PE/PDB 身份不变；EX 投影 209 项。其余 profile 没有本次补提取，完整 Release 矩阵仍为 2856 项、580004 items。
- `tools/test_dyndata_profile_loader.py` 从生产源码抽取实际 Qt 解析和 EX 投影，离线遍历 Release 包并验证未知/重复 ID、非法 uint32 和错误组计数；生成的源码登记在独立 vcxproj/filters 中，不调用驱动。`--source-ref` 可复现旧消费者拒绝合法包。
- 本次 Python 回归 8 项、Qt 全包 2856 profiles 加 4 个拒绝用例通过；主程序用仓库 HostX64 构建脚本。最初因运行中的 exe 锁定报 LNK1104，用户正常退出后构建成功。
- 没有停用 Runtime pattern，没有更改 R0 ABI 或驱动活动状态提交规则。EX 精确身份/条目校验失败时保留原活动状态；PDB 接受会覆盖候选中的对应字段来源。离线校验和构建不替代用户的实际下发与业务验证。
