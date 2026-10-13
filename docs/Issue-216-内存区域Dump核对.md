# Issue #216 内存区域 Dump 核对

需求来源：[进程详细信息内存区域增加 R3/R0 导出](https://github.com/KSwordDEV/KSword/issues/216)。本次已在浏览器查看原截图。

## 入口和布局

| 实际导航入口 | 页面类型 | 共享组件与处理 |
| --- | --- | --- |
| 进程 → 进程详细信息 → 内存 → 内存区域 | 内存现场列表 | 复用嵌入的 `MemoryDock`、`VisibleTableWidget` / `TablePresentation` 与 Full 表格操作条，仅增加菜单动作。 |
| 主导航 → 内存 → 内存区域 | 同一共享页面 | 使用同一菜单与导出实现，排序、筛选、原复制/查看/搜索及工作台入口保留。 |

菜单新增“R3导出此区域Dump…”、“R0导出此区域Dump…”，后台导出期间提供“取消区域导出”。仍使用项目 `ContextMenuStyle` 的不透明底、文字状态和现有 SVG。禁用项提供依赖提示；不新增布局、窗口皮肤或参数框架。参考既有整进程 Dump 和模块 Dump 的读取/保存边界；新增区域行为位于独立 `MemoryDock.RegionDump.cpp`。

输出为 **原始 `.bin` 字节**，长度等于所选区域长度，不是整进程 `.kmdump` 或 Windows minidump。

## 数据和生命周期

- 菜单前按单元格 `Qt::UserRole` 的原始基址/长度查找并冻结区域元数据，不能用排序后的行号索引缓存或解析格式化显示文本。
- 冻结 PID、创建时间和独立进程对象租约；导出租约使用 `PROCESS_QUERY_INFORMATION | SYNCHRONIZE`。原附加句柄并没有 `SYNCHRONIZE`，不能直接用其复制件执行等待式存活检查。R0 导出租约不额外要求 VM_READ。
- 保存框前后核对当前附加目标；后台每块前后检查目标存活/身份和 `VirtualQueryEx` 的 base/size/protect/state/type，最终提交前再次核对。
- 后端由点击的动作固定为 R3 或 R0，不读取页面当前后端、不自动回退，也不新增驱动协议。R0 经 `MemoryAccessBackend` 与 `ArkDriverClient` 的既有通道。
- 单次读取沿用共享协议上限；逐块要求 `ok && !partial`、实际字节数和数据长度均等于请求。短读、部分复制、补零失败状态不能作为成功的转储保存。
- `QSaveFile` 禁止直接写回退。取消、读取失败、写失败或 commit 失败均不发布半个目标文件；已有目标在发布前保持原样。
- 复用 `AsyncOperation`，后台只持值请求与取消令牌；页面析构先关闭控制器。准入核对 `isInFlight / hasPending`，取消后旧 worker 返回前不启动新的区域导出。
- `Reading → CancelRequested` 与 `Reading → Committing` 竞争同一 atomic CAS。取消先胜出则不 commit；提交先胜出则拒绝取消，并按真实文件结果完成。关窗取消读取阶段，但不能撤回已经进入的原子提交。
- 区域任务使用独立的 `内存区域导出` 进度槽及 `beginCycle` generation；读取进度只传 `0..0.99` 比例，保存结果通过 `finish(Success/Failure/Canceled)` 决定，避免与整进程 Dump 冲突、晚到更新或百分比歧义。
- 分块导出读取运行中的进程，不保证所有字节来自同一个时刻；区域和进程身份检查不能替代一致性快照。

## 验证记录

- 生产函数与真实取消函数抽取回归：533 项断言、0 失败。覆盖固定 R3/R0、精确区域和尾块、各类短读、区域变化、身份变化、取消/提交仲裁、原子文件失败、进度回调和默认比例。
- 这些回归替代 Win32、读取后端和文件 I/O 的外部接口，不等同真实 GUI、R3 实读或已加载 R0 驱动验收。
- 两轮源码审查通过；新增源码登记主工程与 filters；中英文语言包只定点插入词条，审计通过 28,912 个源码字符串。
- Release/x64 构建通过：`BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`、`ARTIFACT_UPDATED=True`，主程序 25,029,120 字节，SHA256 `10319A3B3FCA315C685E30C9847BB38EDCAFF30C73A55BAEBF556B29FA3356B0`。日志 `.codex-build-logs/ksword-build-check-20261013-072448.raw.log`；语言包门禁 28,912 个字符串、主题门禁 14 个动态 token / 1,102 个源码文件通过。第一次构建在长时间运行中断后超时，确认无原 MSBuild/cl/link 进程后恢复同一标准工具链构建通过。
- 真实主程序暗色模式菜单：受控进程存活时 R3/R0 两个区域导出入口可用；首个测试目标按预定时限退出后，同一旧区域的两个动作均正确禁用。
- 真实 R3 GUI 导出通过：受控目标 PID 14844，独立 `MEM_COMMIT / PAGE_READWRITE / PRIVATE` 区域基址 `0x000001F11F0B0000`，长度 65,536 字节。字节模式为 `(offset * 37 + 11) & 255`，输出 `output/issue216-r3-region-20261013.bin` 长度 65,536，SHA256 `6fc179cfd193754e6109ad043f56d146c7e7d7c3623ffceae318266286f58388` 与预期完全一致；真实成功提示明确显示 R3/ReadProcessMemory 和输出路径。
- 实际 GUI 使用主导航内存页的同一 `MemoryDock`；进程详情的嵌入入口已做源码同源核对，未另行运行该独立详情窗口。R0 菜单与固定通道经源码/回归核对，未加载驱动执行 R0 实读。
- 受控目标仅包含已知测试字节，无 GUI/网络/驱动；验收后已分离并通过其停止事件正常退出。没有读取用户业务进程内存，没有用伪窗口或离屏窗口替代主程序。
- 既有布局、窄窗、DPI、全部背景模式和深浅主题的全页面视觉合规不由本次菜单改动或构建结果推定。
