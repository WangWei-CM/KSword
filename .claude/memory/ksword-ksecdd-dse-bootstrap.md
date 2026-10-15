---
name: ksword-ksecdd-dse-bootstrap
description: KExecDD 与 KexecDDPlus 在杂项 DSE 页的接入边界和验证要求
metadata:
  type: project
---

# KSecDD 驱动加载引导方案

- `KExecDD` 复用 LSASS 已建立的 KSecDD 连接。前置条件检查只能读取提升状态与
  `RunAsPPL`/`RunAsPPLBoot` 配置；注册表未启用不等于已经验证 LSASS 运行态不是 PPL。
- `KexecDDPlus` 通过 Server Silo 建立独立 KSecDD 连接，LSA Protection 不构成前置阻断；
  接入前需确认 Schedule 服务、SeTcbPrivilege 来源和所需 ntdll 入口。
- 上游明确说明 Plus 方案同一启动周期最多安全调用四次，第五次会因 KSecDD 错误释放
  用户缓冲区触发 Bug Check。未来若增加执行路径，必须用跨进程、跨页面、按启动周期
  持久化的计数器 fail-closed，不能只在 QWidget 成员里计数。
- 两个上游仓库在 2026-10-15 检查时均未提供许可证文件。仓库中不得直接复制或分发
  上游源码；按公开行为独立实现，并保留上游链接与来源说明。
- 只读“前置条件满足”不等于漏洞调用成功，更不等于 DSE 已关闭。实际执行必须分别验证：
  KSecDD 通道、目标 `g_CiOptions` 地址、写前值、写后回读、恢复回读和 PatchGuard 风险窗口。
- 页面属于参数/执行工具，使用 `ParameterWorkspace`：前置条件与定位参数独立滚动，动作固定，
  `CodeEditorWidget` 日志占结果区主要高度；窄窗由共享分栏切换为上下布局。
