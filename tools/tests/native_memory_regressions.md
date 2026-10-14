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
