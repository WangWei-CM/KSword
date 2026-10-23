# 剪贴板保护会话生命周期

- 页面 `MiscDock/ClipboardGuard/ClipboardGuardPage.Session.cpp` 每秒扫描受管进程。Agent 的 `MonitorAgent.cpp::SessionConfigWasReplaced` 把 `session_id` 变化视为新会话，因此同一进程、同一策略的周期扫描不能重写配置或生成新的 `session_id`。动作改变时可保留会话 ID，Agent 每 250 ms 从同一 INI 热更新读、写、枚举动作。
- 新会话先原子提交完整 INI，再清除上次会话遗留的 `stop_<pid>.flag`，之后建立管道客户端并注入 DLL。Agent 的 `WaitForNextSessionConfig` 遇到停止标记会一直等待；顺序颠倒或忘记清除会导致重启无事件。
- 管道是 `PIPE_TYPE_BYTE`，客户端必须累计读取完整的 `ApiMonitorEventPacket` 才解析。停止会话时用 `CancelSynchronousIo` 取消读取线程自己的同步 `ReadFile`，由该线程关闭管道句柄；跨线程直接关闭句柄不能可靠取消读取，还会和读取线程双重关闭。
- Agent 先连接管道、后安装 Hook。页面只有收到内部 `HooksInstalled` 事件才可把进程计入“当前受保护进程”；规则命中或 DLL 注入成功本身不等于监控生效。管道线程退出后应允许下一轮扫描重建会话。
- DLL 的 worker 在停止当前会话后仍常驻。重连同一进程时按 PID 加创建时间复用它，避免每次断线都 `LoadLibraryW` 增加模块引用计数；PID 复用或进程退出时丢弃缓存。
- 当前 `shared/WinApiMonitorProtocol.h::buildSessionDirectory` 依赖调用进程的 `GetTempPathW`。跨用户或 SYSTEM 目标可能看到不同的临时目录，不能把同一用户普通进程上的验证外推为跨账户全局覆盖。
- 该跨账户路径问题已修复：会话文件统一放在 Windows 机器 Temp，配置/停止文件按目标进程 SID 授权，配置携带控制端 SID 并由 Agent 用显式 ACL 创建事件管道；自动子进程继承同一控制端身份与根停止链路。

- API Monitor 新增独立32位 Agent 后，剪贴板保护按进程架构选择同目录的 APIMonitor_x86.dll/APIMonitor_x64.dll，由64位主程序直接注入；会话 INI 同样写入选择后的路径。两个位数仍共享协议、PID租约与驻留/停止规则。发行时携带主程序与两个 DLL；独立注入助手已移除。会话 INI 使用 UTF-16LE BOM，保持中文路径与 Win32 配置读取一致。
