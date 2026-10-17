# 原生内存与文件解码回归

入口：在仓库根目录执行 `python tools/run_native_memory_regressions.py`。
要求 Windows、64 位 MSBuild、HostX64 v143 MSVC，以及仓库配置的 Qt MSVC SDK。
沿用已有 `output/`；不创建工作树、临时构建目录或替代 GUI。

`native_memory_regressions.vcxproj` 和 `.filters` 登记三个独立测试入口。
它们不进入主程序工程，不会把测试 `main` 链接进产品。编译开启 `/W4 /WX`，
`Fixture=Catalog/Codec/Target` 分别选择进程目录、文本文件解码和受控 R3 目标。
产物、编译元数据及完整日志留在 `output/`，不提交二进制。

| 测试 | 真实调用与验证 |
| --- | --- |
| `memory_debug_process_catalog_tests.cpp` | 编译生产 `MemoryDebugProcessCatalog.cpp`；对本进程及本测试创建的子进程独立读取创建时间，验证目录名称/强身份、写能力查询、陈旧或缺失身份拒绝、正常退出。目录枚举只读系统进程元数据，控制和能力断言限于自有目标。 |
| `code_editor_file_decode_tests.cpp` | 编译生产 `CodeEditorFileSession.cpp`；只使用内存字节，验证完整 UTF-8、中文/emoji、BOM、换行、合法 U+FFFD、空文件、六种残缺尾巴、空元数据指针以及补齐后的多字节尾巴。 |
| `issue216_r3_target.cpp` | 自行分配并核验固定 64 KiB 区域，每字节为 `(offset*37+11)&0xFF`；输出 PID/地址/大小/停止事件后等待。runner 只读取自己启动的这个进程声明的区域，逐字节核对全部内容，再经事件要求自然退出。 |

第三个源文件也可用于人工验收内存工作台。独立启动
`output/issue216_r3_target.exe`，根据 READY 后的输出选择目标及地址。
它不访问其它进程、GUI、驱动或网络；停止事件为当前 PID 专用，未收到停止请求
时最多等待三十分钟。自动回归会及时触发停止事件并检查退出码，不产生区域转储。

进程目录与自有目标检查是本机 Win32 回归，解码检查是 Qt Core 逻辑回归。
这些检查不替代主程序 GUI、R0 驱动、调试器或其它真实目标的验收。

## 不附加线程会话

`memory_debug_thread_session_tests.cpp` 直接编译生产
`MemoryDebugThreadSession.cpp`。只启动本测试二进制作为目标，使用父子共享进度
独立核对暂停、恢复与其它线程仍运行；不会附加调试器、注入、设置 TF/DR/INT3、
写目标上下文或操作用户进程。测试对象均隐藏启动，正常结束经事件通知。

2026-10-17 的 HostX64 MSVC 19.44.35207、Windows SDK 10.0.26100.0、
C++17 `/W4 /WX` 构建与本机受控执行通过：普通构建 **47/47**，
故障构建 **66/66**。普通构建完整输出为
`output/memory_debug_thread_session_tests.log`，尾行为
`THREAD_SESSION_CHECKS=47 FAILURES=0`。故障构建完整输出为
`output/memory_debug_thread_session_fault_tests.log`，尾行为
`THREAD_SESSION_CHECKS=66 FAILURES=0`；产物分别为
`output/memory_debug_thread_session_tests.exe` 和
`output/memory_debug_thread_session_fault_tests.exe`，均不提交。

普通构建覆盖创建时间/owner 强身份、自身目标拒绝、RIP/RSP 等真实只读上下文、
一次单线程暂停、重复操作、外部已有暂停的拒绝与配平、连续临时快照无计数泄漏、
close/析构恢复，以及暂停中目标自行退出。前后独立
`CheckRemoteDebuggerPresent` 都为 false。故障构建只将生产实现的
`GetThreadContext`、`ResumeThread`、`WaitForSingleObject` 替换成一次失败后
继续转发真实 Win32 的入口；覆盖捕获失败仍恢复、恢复失败保留句柄/身份/一次计数、
恢复重试、close 失败不丢所有权、双失败保留原因、失效句柄等待报错和析构再恢复。
这些受控故障检查与未替换 Win32 的普通执行分别保留，不能替代产品 GUI 验收。

在仓库根目录可用下列精确工具链参数重现；沿用既有 `output/`：

```powershell
$threadVc='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207'
$threadSdk='C:/Program Files (x86)/Windows Kits/10'
$threadCompiler=Join-Path $threadVc 'bin/Hostx64/x64/cl.exe'
$threadIncludes=@('/external:I'+(Join-Path $threadVc 'include'))
foreach($threadPart in @('ucrt','shared','um')) {
    $threadIncludes+='/external:I'+(Join-Path $threadSdk ('Include/10.0.26100.0/'+$threadPart))
}
$threadLibs=@('/LIBPATH:'+(Join-Path $threadVc 'lib/x64'))
foreach($threadPart in @('ucrt','um')) {
    $threadLibs+='/LIBPATH:'+(Join-Path $threadSdk ('Lib/10.0.26100.0/'+$threadPart+'/x64'))
}
$env:PreferredToolArchitecture='x64'
$env:PROCESSOR_ARCHITECTURE='AMD64'
$env:PROCESSOR_ARCHITEW6432='AMD64'
$env:PATH=(Join-Path $threadVc 'bin/Hostx64/x64')+';'+$env:PATH
& $threadCompiler /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /external:W0 /O1 `
    /DNOMINMAX /DWIN32_LEAN_AND_MEAN @threadIncludes `
    'tools/tests/memory_debug_thread_session_tests.cpp' `
    'Ksword5.1/Ksword5.1/MemoryDock/MemoryDebugThreadSession.cpp' `
    /Fooutput/ /Feoutput/memory_debug_thread_session_tests.exe /link @threadLibs
if ($LASTEXITCODE -ne 0) { throw 'Thread session native build failed.' }
& ./output/memory_debug_thread_session_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Thread session native regression failed.' }

# 故障版本在测试文件末尾包含生产 cpp，只编译测试文件，避免重复链接实现。
& $threadCompiler /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /external:W0 /O1 `
    /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DMEMORY_DEBUG_THREAD_SESSION_FAULT_TESTS @threadIncludes `
    'tools/tests/memory_debug_thread_session_tests.cpp' `
    /Fooutput/memory_debug_thread_session_fault_tests.obj `
    /Feoutput/memory_debug_thread_session_fault_tests.exe /link @threadLibs
if ($LASTEXITCODE -ne 0) { throw 'Thread session fault build failed.' }
& ./output/memory_debug_thread_session_fault_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Thread session fault regression failed.' }
```

暂停计数由 Windows 线程共享，不提供调用方所有权标记；会话只记录自己增加的一次，
恢复时只调用一次 `ResumeThread`，不会循环清空外部计数。其它工具若并发增减计数，
不能将返回结果解释为全进程一致停止或对其它工具计数的隔离保证。析构为尽力恢复；
控制程序被强制终止不能获得用户态析构恢复保证。本入口没有单步/断点/异常消费能力。
