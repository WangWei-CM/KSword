# UI 性能采集与离线分析

用于补齐 T00 的外部证据：目标进程的 PresentMon 呈现记录，以及同机 QPC 时钟下的系统 CPU/上下文切换栈。工具独立编译，主程序无需增加代码或体积。默认只输出计划；执行 `Capture` 才采样，不替用户操作界面。

## 构建与采集

使用 64 位 v143 MSBuild 构建 `KernelTraceCapture.vcxproj`，传入 `/p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:PROCESSOR_ARCHITECTURE=AMD64 /p:PROCESSOR_ARCHITEW6432=AMD64`。产物在仓库忽略目录 `.codex-build-logs/ui-perf-tools/bin/`。`KernelTraceCapture.exe --self-test` 仅检查配置与参数，不启动 ETW。

准备自己安装的 PresentMon 2 控制台程序。先自行启动待测 KSword，再在管理员 PowerShell 7 中填入真实 PID：

```powershell
$targetId = 1234
$presentMon = 'C:/Tools/PresentMon/PresentMon.exe'
./tools/ui_perf/Invoke-UiPerfCapture.ps1 -TargetProcessId $targetId -PresentMonPath $presentMon -OutputDirectory .codex-build-logs/ui-perf-captures
# 上一步只显示计划。确认进程后，再手动执行：
./tools/ui_perf/Invoke-UiPerfCapture.ps1 -Action Capture -TargetProcessId $targetId -PresentMonPath $presentMon -OutputDirectory .codex-build-logs/ui-perf-captures -Seconds 30
```

看到 `Capture started` 后，在 30 秒内重复同一个操作，例如已打开 Dock 往返切换或单表滚动。每次只测一种场景；对照版本保持数据量、窗口、DPI、刷新率、主题、背景、动画和 GPU 开关一致。结束后保存打印的 `capture.json` 所在整个目录。

每次创建 UUID 目录、独立 PresentMon 会话和独立系统 logger。采集器仅停止自己成功创建的 ETW 句柄，父 PowerShell 退出或自身计时结束时也走停止路径；不修改其它 WPR 会话、采样间隔、注册表或产品配置。采集失败保留状态和丢失计数，不当作通过。若工具异常超时，按日志中的唯一会话名检查，不能直接取消其它采集任务。

系统 ETL 包含系统范围 CPU/调度事件，分析时按目标进程及 UI 线程筛选。文件顺序写入、上限 1GB；达到上限、丢事件、栈配置或停止失败均不能作为完整数据。权限/会话资源不足时直接报错，不会自动停止其它会话。

## 离线结果及边界

```powershell
python tools/ui_perf/analyze_presentations.py --manifest '<本次目录>/capture.json' --output '<本次目录>/analysis.json'
```

有多个交换链时先看结果，关联操作需另传 `--swapchain 0x地址`；不能把不同交换链合并算帧间隔。分析器校验文件 SHA256、进程/诊断会话身份和时钟，以精确整数/Decimal 处理 QPC，区分未显示记录、缺值及丢失；给出各流间隔与输入延迟分位数。

普通发行版默认编译关闭 `KSWORD_PERF_TRACE_ENABLED`，仍可采集系统栈和呈现。只有另行编译启用诊断的版本才提供操作阶段 JSONL；启动前设置 `KSWORD_PERF_TRACE` 到日志路径，并在采集命令加 `-TracePath <日志路径>`。运行中复制的是快照，不能当成 writer 已完整收尾。多诊断会话必须另传 `--session-id`；身份或时钟不匹配就拒绝关联。

`analysis.json` 不解码 ETL 栈，需另用 WPA 打开 `system.etl` 查看 CPU Sampled/CPU Precise、线程等待和符号。配置栈采样成功不证明实际每个样本均有完整栈，实际覆盖须检查。PresentMon 的 ETW 显示事件不是相机测得的物理发光；Qt 操作后的下一条显示记录只作时间候选，不证明该操作已显示完成。没有记录、诊断关闭、未覆盖的异步路径和不完整采集会列为缺口，工具不会据此将 T00 标为完成。

新诊断日志另有 `scroll_paint_observations`：按真实 viewport 身份统计 Paint 分派间隔、值观测至稳定 Paint 入口，以及同一 Paint 至 GL swap 回调的间隔，动画关闭时也可观察。被替换的值、绘制期间变化/销毁、未知观测时刻和缺失/重复 swap 单列。信号回调的 action 上下文只表示该回调现场；Qt 重入可能先执行其它处理，因此这些数值不作为输入到显示的因果延迟或实际 FPS。

逐操作 `diagnostic_stages` 保留导航路径、目标索引、导航/请求身份、父请求、ready 边界及原始结果；没有输入 begin 的程序化导航另列，不能归到后来的输入。`ready_display_candidates` 为每个已记录的导航 ready 或 `*_business_data_ready` 单独选择时钟误差上界之后的首条可观测显示记录，便于人工比较原始输入候选与 ready 之后的候选。嵌套请求分开保留，重复 ready 拒绝关联；没有后续记录就为空。外壳 ready 仍只代表原外壳边界，这些时间关系不证明目标内容已经呈现或整个动作完成。

## 接口依据

- [PresentMon 控制台参数与 CSV 指标](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)：使用 v2/QPC、独立会话名及定时退出；未显示的 `NA` 不作为零延迟。
- [系统 logger 会话](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-systemtraceprovider-session)：使用独立 GUID 与会话，不占用固定 NT Kernel Logger 身份。
- [TraceSetInformation](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-tracesetinformation)、[采样事件](https://learn.microsoft.com/en-us/windows/win32/etw/perfinfo)、[调度事件](https://learn.microsoft.com/en-us/windows/win32/etw/thread-v2)：配置 SampledProfile 与 CSwitch 栈，保留实际状态和丢失计数；没有自动修改系统内存策略。
