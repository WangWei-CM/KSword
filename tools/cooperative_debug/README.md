# 主动接入的 x64 协作调试组件

此实现允许**目标自己启用**暂停、真实指令单步、继续和整数寄存器快照，全程不创建 Windows Debug Object。它供自有程序或明确允许改源码的目标使用；没有接入组件的既有进程不会出现可连接端点。

目标静态链接 `CooperativeDebugRuntime.lib`，包含 `CooperativeDebugRuntime.h`，在正常业务初始化中调用 `runtime::Start()`，在允许暂停的位置调用 `runtime::Checkpoint()`，退出业务线程后调用 `runtime::Stop()`。不要在 DllMain 或 loader lock 内调用这些入口。运行时生命周期操作由目标串行调用。

```cpp
if (ksword::cooperative::runtime::Start() == 0)
{
    while (applicationRunning)
    {
        ksword::cooperative::runtime::Checkpoint();
        DoOneUnitOfWork();
    }
    ksword::cooperative::runtime::Stop();
}
```

控制端使用 `shared/debug/CooperativeDebugClient.h/.cpp` 中的 `Session`，不依赖 Qt，也不要求驱动。先以 PID 和进程创建时间调用 `open()`，再 `requestPause()`；目标到达下一检查点后，`query()` 返回 `State::Paused`、非零 `stopId` 和真实异常现场。`step(stopId)` 与 `resume(stopId)` 只能消费一次当前停点；重新暂停后必须取新的 `stopId`。

`step` 设置本组件拥有的 TF，返回用户态后由下一次 `EXCEPTION_SINGLE_STEP` 捕获。不是延时恢复再读取 RIP，也不是调试事件轮询。检查点是一条组件自己的 INT3；处理器只认领 TLS 明确标记且精确地址匹配的 INT3，并跳过该字节。MSVC 增量链接可能生成 ILT 跳板，因此实际指令地址由汇编内部非公开标签返回，不能使用外部函数指针猜测。第一次单步执行的是检查点叶函数的 RET，随后经过检查点包装函数返回业务调用点；这些均为真实机器指令。

持续单步回到组件自己的短临界区时，不能把持锁线程发布成暂停，否则命令、租约和退出收尾都会等同一把锁。运行时在获取锁之前设置本线程的不可停标记，释放锁之后才退出此范围；遇到自有 TF 时立即清除该 TF，在原线程解锁后发布 `State::Interrupted` 并继续业务。此状态不代表单步成功，没有 `stopId` 或寄存器现场；查询保留中断状态，控制端可重新请求暂停。协议与端点为 v2，固定结构大小不变，不与 v1 组件混连。未知异常在获取组件锁之前就交回原异常链。

只暂停命中的一个业务线程，其它业务线程和命令工作线程仍运行。未知异常、其它 INT3 和明确的外部硬件断点原因继续交给原有异常链；已有 TF 不会被接管。无 Debug Object 时，Windows 交给 VEH 的 TF 异常 CONTEXT 可能报告 `Dr6=0`，因此单步所有权由组件已经武装的精确线程和会话租约判定，并拒绝吞掉明确的 B0–B3、BD、BT 原因。快照只包含 GPR、RIP、RSP、RFLAGS；不提供 XSTATE 或寄存器编辑。

端点名称包括目标 PID 和创建时间。服务端使用 First Instance、消息模式、Reject Remote Clients 和当前用户 SID 的显式 ACL；双方验证管道对端 PID 与真实进程对象创建时间。服务端保留控制进程句柄，并验证同用户、随机 128 位会话令牌、递增请求编号及当前停点编号。固定长度 POD 协议对版本、结构大小和保留字段严格检查，错误令牌不会得到当前令牌或现场回显。

`Session::close()` 只取消本地 I/O 并断连，不等待关闭应答。控制者死亡、断连、租约到期以及 `Stop()` 都会释放暂停线程；默认租约 10 秒，由正常查询刷新。如果已经发出 TF 后控制者离开，下一次对应单步异常只清除组件自己的 TF 并继续。旧 TF 或暂停回调尚未收尾时，新控制者会收到 Busy，不能覆盖旧线程的清理所有权。`Stop()` 不会删除仍负责清理已武装 TF 的 handler；如果线程尚未返回用户态，安全 handler 会保留到后续异常，因此本版本仅按静态链接使用，不能把它包装成可立即卸载的 DLL。所有 Checkpoint 调用和已武装线程正常收尾后，再次 `Stop()` 可撤销残留安全 handler。

当前不提供任意进程自动注入、指令补丁、软件/硬件断点安装、步过、步出、全进程停止、内核调试或 HVM 调试。请求暂停需要目标执行检查点；不能承诺暂停一个永远阻塞且不再到达检查点的线程。程序的异常或调度行为可能介入单步，源码级和跨异常“步过”需要额外语义，当前按钮不得声称支持这些能力。

## 构建与自有目标验证

在仓库根运行：

```powershell
& tools/cooperative_debug/Invoke-CooperativeDebugTests.ps1
```

脚本使用 64 位 MSBuild、三项 HostX64 架构属性、MSVC v143、Windows SDK、MASM、C++17、`/W4 /WX`。产物位于 `output/cooperative_debug/Release/`：

- `CooperativeDebugRuntime.lib`：目标主动静态链接的运行时，依赖 Advapi32/Bcrypt。
- `CooperativeDebugDemo.exe`：主动接入的控制台演示目标，输出真实 PID 和创建时间，Ctrl+C 正常退出。
- `CooperativeDebugTests.exe`：创建并清理自己拥有的子进程，不访问用户已有目标。

当前真实测试共 62 项断言通过，保留原 50 项覆盖：两次 TF 单步及 RIP 前进、继续、过期 stopId、未知异常保留、版本拒绝、错误令牌拒绝、创建时间错误、并发查询期间快速关闭、断连恢复、租约恢复、控制者突然死亡（包括刚发出 Step）、暂停时由独立目标线程执行 Stop 以及目标正常退出。新增持续单步穿过业务循环并再次进入 Checkpoint，在临界区明确返回 Interrupted、无停点/现场、清除自有 TF 后业务继续及再次暂停；未知异常测试也覆盖目标正持有组件锁的情况。测试步进前后同时确认 `CheckRemoteDebuggerPresent=false`、`ProcessDebugPort=0` 和 `ProcessDebugObjectHandle=STATUS_PORT_NOT_SET (0xC0000353)` 且句柄为空。

这些是本机 x64 自有目标的后端运行证据，不代表主程序 GUI、其它系统版本、所有指令、WOW64、任意第三方目标或 HVM 路线已验收。
