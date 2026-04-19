任何人或者AI的代码都必须包含中文注释。每10行代码就要说明这段代码要干什么，所有变量一律说明用途，所有函数说明用途，调用方法，传入传出什么
所有按钮如果含义简单要尝试用图标代替文字。例如开始按钮就找play的svg。所有按钮悬停要出现释义。
代码不准压行，必须保持正常的代码风格。
所有代码文件，除了工具库ksword文件夹里面的，最好不要超过1000行。超过的时候，考虑新增文件。按照图形界面的Tab分割文件是极好的。
文本详情/日志/返回内容类窗口默认必须优先使用项目内置编辑器（CodeEditorWidget），除非明确说明该场景不适用；不要默认改用QPlainTextEdit等外部控件。
如果DevelopPlan.txt不为空的话，那么说明你的开发任务在DevelopPlan.txt中。在完成了一项任务后，去这个txt文件里面对应的功能处写已完成字样以标志进行下一步工作；除非这个txt已经全部完成，否则你继续执行下一个任务txt中的任务。不要中断会话！！
执行完每个任务后进行编译验证，除非在会话中明确说明其他模块无法编译。完成验证后再次审计代码。
多文件严禁使用.inc文件堆叠的方式，必须使用模块分明的.cpp+.h形式
当AI预计要一次性写400行以上的代码的时候，沙盒会限制，请直接写入文件，否则报错Failed to apply patch.
右键菜单（QMenu）是高风险点：默认样式在某些页面会继承透明/未填充背景，浅色模式下可能出现黑底黑字。凡是新增或修改右键菜单，必须显式设置背景样式（至少设置QMenu背景、文字、选中态、禁用态），不要依赖默认样式。
详情弹窗（QDialog）同样是高风险点：如果父容器用了透明或特殊样式，弹窗可能出现黑底。新增详情页时必须显式设置不透明背景样式，确保浅色/深色都可读。

R3 与 R0 的通信结构体/IOCTL 常量统一放在 `shared/driver/` 目录，不允许分散定义在 UI 文件或驱动私有头里。新增通信协议时，必须先在 `shared/driver/` 建头文件，再让 R3/R0 同时 include 这一个文件。
凡是使用 R0 的功能入口（按钮、面板、弹窗、右键菜单、专用功能区），右下角必须放置这张图：`H:\Project\Ksword5.1\Ksword5.1\Ksword5.1\Resource\Kernel.png`。这是统一视觉标识，不能省略。
R0 结束进程功能代码放置规范：
- R3 入口：`Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp` 的进程列表右键菜单动作；动作函数只负责收集 PID、调用驱动、写日志和刷新。
- R3 协议定义：`shared/driver/KswordArkProcessIoctl.h`（PID/退出码结构与 IOCTL 号）。
- R0 分发入口：`KswordARKDriver/Queue.c` 的 `KswordARKDriverEvtIoDeviceControl`；这里做 IOCTL 解析、参数校验、日志。
- R0 执行要求：驱动侧实际结束进程必须走 `ZwTerminateProcess`，并通过现有日志通道输出 `[Info]/[Warn]/[Error]...END_OF_LOG`。

> 本文档描述 `Framework.h` 日志系统的调用方式与语法规范。  
> 适用范围：Windows 平台下的 KswordARK/Ksword5.1 工程。

---

## 1. 基本接入方式

```cpp
// 1) 所有业务代码统一包含 Framework.h。
#include "Framework.h"
```

---

## 2. 日志语法（必须遵守）

### 2.1 每条日志必须先创建并传入 `kLogEvent`

```cpp
{
    // kLogEvent 的作用：生成并携带本次事件 GUID（不可修改）。
    kLogEvent tmpEvent;

    // info 为全局日志流对象；eol 会自动携带文件/行号/函数并提交日志。
    info << tmpEvent << "现在输出一些文字" << eol;

}
```
最重要的一点：kLogEvent的用途是追踪调用链。因此，在连续的过程中只应该有一个kLogEvent变量，让整个操作中的kLogEvent都是一样的，而不要随便到处临时创建临时的kLogEvent。

### 2.2 错误级别示例（自动附带位置信息）

```cpp
{
    // err/fatal 在控制台会追加 (File, Line, FunctionSignature) 信息。
    kLogEvent tmpEvent;
    err << tmpEvent << "现在输出一些文字" << eol;
}
```

### 2.3 可用日志流对象

- `dbg`：Debug
- `info`：Info
- `warn`：Warn
- `err`：Error
- `fatal`：Fatal

> 备注：为了避免与 Qt `QObject::event(...)` 命名冲突，日志事件类型统一使用 `kLogEvent`，不要在 Qt 类成员函数中使用裸 `event` 作为类型名。

---

## 3. eol 宏规范

- `eol` 是日志结束标记，必须作为一次日志表达式的结束部分。
- `eol` 自动采集：
  - `__FILE__`
  - `__LINE__`
  - `__FUNCTION__`
  - `__FUNCSIG__`（MSVC 下完整函数签名）
- 若没有 `eol`，日志不会提交到控制台与日志管理器。

---

## 4. 日志管理器规范

全局日志管理器对象：

```cpp
extern kEventEntry KswordARKEventEntry;
```

### 4.1 API 说明

- `void add(kEvent)`：追加一条日志（框架内部自动调用，业务层通常不手动调）。
- `void clear()`：清空全部日志。
- `bool Save(std::string path)`：导出全部日志到文件（TSV：`\t` 分隔字段，`\n` 分隔行）。
- `std::vector<kEvent> Track(GUID)`：按 GUID 返回关联日志。
- `std::vector<kEvent> Snapshot()`：返回全部日志快照。
- `std::size_t Revision()`：返回日志版本号（用于 UI 增量刷新）。

## 6. 代码风格要求（本模块）

- 新增日志代码必须保持“kLogEvent + 流式 + eol”调用格式。
- 业务日志建议优先 `info/warn/err`，避免滥用 `fatal`。
- 涉及并发输出必须通过框架提供对象，不得直接多线程写 `std::cout`。

---

## 7. 进度条管理规范（kProgress）

全局进度管理器对象：

```cpp
extern kProgress kPro;
```

### 7.1 新增任务

```cpp
// add 返回 PID（后续 set/UI 都依赖该 PID）。
int pid = kPro.add("任务1", "步骤1");
```

### 7.2 更新步骤与进度

```cpp
// progress 既支持 0~1（如 0.70f），也支持 0~100（如 70.0f）。
kPro.set(pid, "步骤2", 0, 70.0f);  // 显示 70%
```

> 约定：当 `progress` 归一化后等于 `1.0f` 时，该任务卡片会从“当前操作”列表隐藏。

### 7.3 阻塞式按钮选择

```cpp
// 返回值从 1 开始，对应点击了第几个选项按钮；取消返回 0。
int choice = kPro.UI(pid, "请选择你的操作", "选项A", "选项B", "选项C");
```

---

## 8. Ksword Win32 工具封装规范（进程模块）

### 8.1 目录与命名空间约定

- 根目录提供统一入口头文件：`Ksword.h`
- 具体实现放在：`ksword/` 递归目录
- 命名空间统一使用小写前缀：`ks::...`
- 每一级命名空间目录必须有同名头文件逐级向上包含，例如：
  - `ksword/process/process.h`
  - `ksword/ksword.h`（包含 `process.h`、`string.h`）
  - `Ksword.h`（包含 `ksword/ksword.h`）

### 8.2 Framework 入口规则

- `Framework.h` 必须包含 `Ksword.h`，作为全项目统一入口。
- 项目源码文件（`.h/.cpp`）统一优先包含 `Framework.h`，避免跨模块直接散乱引入系统头。

### 8.3 调用示例

```cpp
#include "Framework.h"

// 1) 按策略枚举进程
std::vector<ks::process::ProcessRecord> processList =
    ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);

// 2) 获取某 PID 的路径
std::string imagePath = ks::process::QueryProcessPathByPid(1234);

// 3) 操作进程（示例：挂起）
std::string detail;
bool ok = ks::process::SuspendProcess(1234, &detail);
```

### 8.4 ProcessDock 交互规范

- 进程列表刷新必须走异步任务（线程池），禁止阻塞 UI 线程。
- 进程 identity 使用：`PID + creationTime100ns`。
- 若 identity 相同，则复用上一轮静态详情（路径、签名、命令行、用户等），性能计数器除外。
- 新增进程行高亮绿色；退出进程保留一轮并高亮灰色。

---

## 9. 默认构建失败时的处理指引（重要）

### 9.1 先设置 Qt 环境变量（必做）

```powershell
$env:KSWORD_QT_DIR='C:\Qt\6.9.3\msvc2022_64'
```

### 9.2 推荐构建命令

先构建主项目（快速验证）：

```powershell
msbuild .\Ksword5.1\Ksword5.1.vcxproj /t:Build "/p:Configuration=Debug;Platform=x64" /m
```

再构建整套解决方案（含 Taskbar 与 KswordHUD）：

```powershell
msbuild .\Ksword5.1.sln /t:Build "/p:Configuration=Debug;Platform=x64" /m
```

### 9.3 常见失败原因与修复

1. **Qt 未绑定**
   - 报错关键词：`There's no Qt version assigned`
   - 修复：确认 `KSWORD_QT_DIR` 已设置为 `C:\Qt\6.9.3\msvc2022_64`。

2. **权限受限导致写入失败**
   - 报错关键词：`Access denied`、`*.tlog` 写入失败
   - 修复：在 CLI 授权弹窗中对 `msbuild` 选择“允许并记住（Always allow/Remember）”。

3. **PowerShell 参数被分号截断**
   - 现象：`Platform=x64` 被当成独立命令
   - 修复：`/p` 参数必须整体加引号：
     - `"/p:Configuration=Debug;Platform=x64"`

### 9.4 如何让 AI 后续构建尽量无人值守

- 当出现权限确认弹窗时，选择：
  - **允许（Allow）**
  - **记住该前缀规则（Remember this rule）**
- 推荐长期放行前缀：
  - `["msbuild"]`

这样后续大多数构建命令可直接执行，减少重复确认。
