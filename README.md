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

### 工程结构
- `Ksword5.1/`：主解决方案与主工程源码
- `Taskbar/`：Taskbar 相关工程
- `KswordHUD/`：HUD 相关工程
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

