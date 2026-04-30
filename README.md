<p align="center">
  <img src="Website/Images/logo.png" alt="Ksword Logo" width="420" />
</p>

<h1 align="center">Ksword5.1</h1>
<p align="center"><strong>全国最强的 Windows 内核调试工具</strong></p>
<p align="center"><strong>China's Most Powerful Windows Kernel Debugging Suite</strong></p>

<p align="center">
  <a href="#中文">中文</a> · <a href="#english">English</a>
</p>

---

## 中文

### 项目简介
Ksword5.1 是一个面向 Windows 平台的综合调试与分析工具集，覆盖进程、网络、内存、驱动、内核、注册表等核心场景，并提供日志追踪与任务进度面板辅助排障。

### 主要能力
- 进程分析与控制（枚举、详情、创建、终止、挂起/恢复等）
- 网络抓包与连接管理（过滤、连接管控、缓存诊断）
- 内存扫描与查看（区域、搜索、十六进制查看、书签/断点）
- 驱动与内核信息管理（服务操作、对象类型、NtQuery 结果）
- 注册表浏览与异步搜索（编辑、导入/导出）
- 日志输出与调用链追踪（GUID 级追踪）

### ARK 功能（按 Tab 分类）
> 以下内容基于程序实际 UI 文本整理（`MainWindow` 主 Tab、各 Dock 的 `addTab`/按钮提示/表头文案）。
> OpenArk 覆盖对照与缺口 TODO 见：`docs/OpenArk功能对照与TODO.md`。

#### 主工作区 Tab（16个）
| 一级 Tab | 二级 Tab / 关键区 | 主要功能（按 UI 能力归纳） |
|---|---|---|
| 欢迎 | 欢迎页主体 | 展示版本与编译时间、当前 Windows 用户与头像、`Github仓库`/`QQ群`/`网站`入口。 |
| 进程 | `进程列表`、`创建进程`、进程详情窗口（`详细信息`/`线程`/`操作`/`模块`/`令牌`） | 进程枚举与树状/列表切换、右键动作（结束/挂起/恢复/关键进程/优先级/打开目录）、CreateProcess 参数化创建、线程与模块明细、令牌与注入相关调试动作。 |
| 网络 | `流量监控`、`进程限速`、`连接管理(TCP/UDP)`、`请求构造`、`HTTPS解析`、`ARP缓存`、`DNS缓存`、`存活主机` | 抓包与多条件过滤（PID/IP/端口/包长）、连接查看与 TCP 终止、按进程限速、手工构造网络请求、HTTPS 代理解析与证书/系统代理联动、ARP/DNS 缓存维护、IP 段存活扫描。 |
| 内存 | `进程与模块`、`内存区域`、`内存搜索`、`内存查看器`、`断点与书签` | 进程附加与模块列表、内存区域筛选、首次/再次扫描与比较过滤、十六进制查看与地址跳转、断点管理、书签值刷新与跳转。 |
| 文件 | `文件管理`、`文件恢复`、文件属性窗口（`常规信息`/`安全与权限`/`哈希与完整性`/`数字签名`/`PE信息`/`字符串`/`十六进制`） | 双面板文件管理（导航/过滤/排序/视图模式/右键动作）、权限接管、哈希与签名检查、字符串与 Hex 预览、NTFS 删除项扫描与恢复导出。 |
| 驱动 | `驱动概览`、`驱动操作`、`调试输出` | 驱动服务与已加载内核模块总览、驱动服务注册/更新/加载/卸载/删除/状态查询、DBWIN 调试输出捕获。 |
| 内核 | `NtQuery信息` | 刷新 NtQuery 结果并查看类别/函数/查询项/状态/摘要与详情。 |
| 监控 | `进程定向`、`直接内核调用`、`WinAPI`、`WMI`、`ETW监控` | 进程定向 ETW 事件追踪（目标进程树）、直接内核调用 ETW syscall 采集与调用号映射、WinAPI Agent 监控与 Hook 分类开关、WMI Provider/事件类订阅与筛选、ETW Provider/会话管理与采集导出。 |
| 硬件 | `概览`、`利用率`、`CPU`、`显卡`、`内存` | 概览与静态硬件信息、任务管理器风格利用率页（CPU/内存/磁盘/以太网/GPU）、CPU 每逻辑核指标、GPU 与内存详情。 |
| 权限 | `账号`、`权限` | 本地用户列表、创建用户/重置密码、用户组与当前进程权限快照审计。 |
| 设置 | `外观与启动` | 主题模式（跟随系统/浅色/深色）、背景图路径与透明度、默认启动页签、启动最大化、启动自动请求管理员权限。 |
| 窗口 | `窗口列表`、`桌面管理`；窗口详情窗口（`基础属性`/`进程与线程`/`类信息`/`消息钩子`/`高级属性`） | 窗口枚举/筛选/分组/预览、窗口拾取器、窗口控制与进程联动、桌面枚举与切换、消息监控与导出。 |
| 注册表 | `值列表`、`搜索结果` | 注册表树导航、键值增删改查、导入/导出 `.reg`、异步搜索并跳转命中项。 |
| 句柄 | `句柄列表`、`对象类型` | 按 PID/关键字/对象类型过滤句柄，命名对象与名称解析预算控制，查看对象类型统计与详细字段。 |
| 启动项 | `总览`、`登录`、`服务`、`驱动`、`计划任务`、`高级注册表`、`WMI` | 启动项全景汇总与分类查看，过滤/导出，定位文件与注册表位置，支持删除可操作项并可跳转服务管理。 |
| 服务 | 服务主表 + 详情页（`常规`/`登录`/`恢复`/`依存关系`/`审计`） | 服务列表筛选与排序、启动类型调整、启动/停止/暂停/继续、服务属性编辑、依赖关系与审计信息查看、TSV/JSON 导出。 |

#### 辅助面板 Tab（4个）
| 面板 Tab | 关键区 | 主要功能 |
|---|---|---|
| 当前操作 | 任务卡片列表 | 展示后台任务步骤与进度百分比，任务完成后自动隐藏。 |
| 日志输出 | 级别过滤 + 日志表格 + 右键菜单 | 按级别过滤日志、复制可见内容、导出日志、双重确认清空、GUID 调用链追踪。 |
| 即时窗口 | 代码编辑器 | 即时文本/代码输入输出区域（用于快速验证与临时记录）。 |
| 监视面板 | CPU/内存/磁盘/网络四宫格图 | 底部实时性能监视，CPU 按逻辑核心显示，磁盘与网络显示双曲线吞吐趋势。 |


### ARK 协作架构约定
- R0/R3 共享协议统一位于 `shared/driver/`。
- 驱动 IOCTL 分发采用 `KswordARKDriver/src/dispatch/ioctl_registry.c` 注册表模式；`ioctl_dispatch.c` 只负责查表、调用 handler、日志和完成请求。
- 用户态访问 KswordARK 驱动统一通过 `Ksword5.1/Ksword5.1/ArkDriverClient/`。ProcessDock、FileDock、KernelDock 等 UI 模块不直接打开 KswordARK 设备或调用其 IOCTL。
- DynData 第一阶段使用 `third_party/systeminformer_dyn/` 中 vendored System Informer 动态偏移数据；Ksword 只接入 `KphDynConfig` 数据和轻量解析器，不引入 KPH 通信层、对象系统或 session token。
- DynData R0/R3 协议集中在 `shared/driver/KswordArkDynDataIoctl.h`，KernelDock 的“动态偏移”页通过 `ArkDriverClient` 展示 profile 命中、字段来源和 capability gating。
- 驱动统一状态/能力协议集中在 `shared/driver/KswordArkCapabilityIoctl.h`；KernelDock 的“驱动状态”页展示 Driver Loaded/Missing、Protocol Mismatch、DynData Missing、Limited、安全策略、最近 R0 错误和功能能力矩阵。
- 依赖未公开字段的 R0 功能必须在 `ioctl_registry.c` 声明所需 `KSW_CAP_*`，由 dispatch 层在 handler 前统一执行 capability gate。
- 新功能按模块分 owner 与写入范围，新增源码需同步更新 `.vcxproj` 和 `.filters`。

### 工程结构
- `Ksword5.1/`：主解决方案与主工程源码
- `Taskbar/`：Taskbar 相关工程
- `KswordHUD/`：HUD 相关工程
- `shared/driver/`：R0/R3 共享 IOCTL 协议头
- `third_party/systeminformer_dyn/`：System Informer DynData 数据快照、LICENSE、NOTICE 和 Ksword 包装头
- `Website/`：项目官网与模块介绍页面（含中英切换）

### 构建环境
- Windows 10/11 x64
- Visual Studio 2022（MSVC）
- Qt 6.9.3（`msvc2022_64`）

### 快速构建
```powershell
# 1) 进入包含解决方案的目录
cd .\Ksword5.1

# 2) 设置 Qt 路径
$env:KSWORD_QT_DIR='C:\Qt\6.9.3\msvc2022_64'

# 3) 构建解决方案（含主项目/Taskbar/KswordHUD）
msbuild .\Ksword5.1.sln /t:Build "/p:Configuration=Debug;Platform=x64" /m
```

### 网站文档预览
```powershell
# 在仓库根目录启动本地静态服务
python -m http.server 8080 --directory .\Website
```
浏览器访问：`http://127.0.0.1:8080/`

### 声明
本项目包含系统级调试与管理能力，请仅在合法授权和合规场景下使用。

---

## English

### Overview
Ksword5.1 is an integrated Windows debugging and analysis toolkit that covers process, network, memory, driver, kernel, and registry workflows, with built-in logging and progress panels for troubleshooting.

### Key Capabilities
- Process inspection and control (enumeration, details, creation, terminate, suspend/resume)
- Network packet monitoring and connection management (filters, controls, diagnostics)
- Memory scanning and viewing (regions, search, hex viewer, bookmarks/breakpoints)
- Driver and kernel-oriented operations (service actions, object types, NtQuery results)
- Registry browsing and async search (edit, import/export)
- Log output and GUID-based call-chain tracking

### Repository Layout
- `Ksword5.1/`: Main solution and core source code
- `Taskbar/`: Taskbar-related project
- `KswordHUD/`: HUD-related project
- `Website/`: Product website and module documentation (CN/EN switch supported)

### Build Requirements
- Windows 10/11 x64
- Visual Studio 2022 (MSVC)
- Qt 6.9.3 (`msvc2022_64`)

### Quick Build
```powershell
# 1) Enter the directory containing the solution
cd .\Ksword5.1

# 2) Set Qt path
$env:KSWORD_QT_DIR='C:\Qt\6.9.3\msvc2022_64'

# 3) Build full solution (main project + Taskbar + KswordHUD)
msbuild .\Ksword5.1.sln /t:Build "/p:Configuration=Debug;Platform=x64" /m
```

### Preview Website
```powershell
python -m http.server 8080 --directory .\Website
```
Open: `http://127.0.0.1:8080/`

### Notice
This project includes system-level debugging and management features. Use it only in legally authorized and compliant scenarios.
