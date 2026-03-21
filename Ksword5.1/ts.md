# Ksword5.1 构建与权限指引（Codex CLI）

## 1) 如何“自动允许”我的构建操作（推荐）

在我第一次请求提权执行构建时，你会看到授权弹窗。  
请选择 **“允许并记住该前缀规则（Always allow / Remember）”**，并优先放行以下前缀：

- `["msbuild"]`

这样后续我执行 `msbuild`（含单项目和解决方案构建）通常就不需要你重复点确认，可实现接近无人值守。

---

## 2) 本工程默认构建命令

先设置 Qt 路径（当前环境可用）：

```powershell
$env:KSWORD_QT_DIR='C:\Qt\6.9.3\msvc2022_64'
```

构建主工程：

```powershell
msbuild .\Ksword5.1\Ksword5.1.vcxproj /t:Build "/p:Configuration=Debug;Platform=x64" /m
```

构建整套解决方案（含 Taskbar / KswordHUD）：

```powershell
msbuild .\Ksword5.1.sln /t:Build "/p:Configuration=Debug;Platform=x64" /m
```

---

## 3) 如果“默认构建失败”，按这个顺序处理

1. **Qt 未绑定**  
   报错类似 “There's no Qt version assigned...” 时，先确认已设置：
   - `KSWORD_QT_DIR=C:\Qt\6.9.3\msvc2022_64`

2. **权限/沙箱写入失败**  
   报错类似 “Access denied / 无法写入 ...Taskbar.tlog” 时：
   - 给 `msbuild` 前缀做“允许并记住”授权；
   - 然后重试解决方案构建命令。

3. **PowerShell 参数被分号拆断**  
   `"/p:Configuration=Debug;Platform=x64"` 必须整体加引号，避免 `Platform=x64` 被当成单独命令。

4. **只想先验证主程序可编译**  
   先编 `Ksword5.1.vcxproj`，通过后再编 `.sln`。

---

## 4) 当前已验证的产物（Debug x64）

输出目录：`.\x64\Debug\`

- `Ksword5.1.exe`
- `Taskbar.exe`
- `KswordHUD.exe`

> 编译中存在大量 `C4828` 编码警告，但不阻塞链接产物生成。
