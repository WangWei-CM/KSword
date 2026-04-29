# KSword Agent Notes

## 发行版制作流程

本流程用于在仓库根目录 `C:\Users\Felix\CLionProjects\KSword` 生成包含完整 `Release\` 目录的 7z 发行包。默认参考旧包布局：`C:\Users\Felix\Downloads\KswordARK评估版本-260427-未签名R0-进程内存监控增强.7z`。

### 1. Release 构建

先设置 Qt 路径和 QtMsBuild 路径，再依次构建用户态项目。主程序必须重新构建；Taskbar、KswordHUD、APIMonitor_x64 也要构建后覆盖进包。

```powershell
$msbuild='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$env:KSWORD_QT_DIR=(Resolve-Path '.deps\Qt\6.9.3\msvc2022_64').Path
$qtMsBuild=(Resolve-Path '.deps\QtVsTools\msbuild').Path

& $msbuild 'Ksword5.1\Ksword5.1\Ksword5.1.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtMsBuild=$qtMsBuild /m:1 /v:minimal
& $msbuild 'Taskbar\Taskbar.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtMsBuild=$qtMsBuild /m:1 /v:minimal
& $msbuild 'KswordHUD\KswordHUD.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtMsBuild=$qtMsBuild /m:1 /v:minimal
& $msbuild 'APIMonitor_x64\APIMonitor_x64.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal
```

驱动项目依赖 WDK。如果当前机器无法构建驱动，不要阻塞发行包制作；沿用已有未签名 R0 Release 产物：`KswordARKDriver\x64\Release\KswordARK.sys`、`KswordARKDriver\x64\Release\KswordARK.pdb`、`KswordARKDriver\x64\Release\KswordARKDriver.inf`。

### 2. 搭建发行目录

推荐从参考包提取完整 Qt 依赖与插件目录，再覆盖最新构建产物。这样能保持 `platforms`、`styles`、`imageformats`、`iconengines`、`generic`、`networkinformation`、`tls`、`translations`、`qtadvanceddocking.dll` 等布局一致。

```powershell
$ref='C:\Users\Felix\Downloads\KswordARK评估版本-260427-未签名R0-进程内存监控增强.7z'
$stageRoot=Join-Path (Get-Location) 'dist\KswordARK-release-work'
$stage=Join-Path $stageRoot 'Release'

if (Test-Path $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $stageRoot | Out-Null
tar -xf $ref -C $stageRoot

Copy-Item 'Ksword5.1\Ksword5.1\x64\Release\Ksword5.1.exe' $stage -Force
Copy-Item 'Taskbar\x64\Release\Taskbar.exe' $stage -Force
Copy-Item 'KswordHUD\x64\Release\KswordHUD.exe' $stage -Force
Copy-Item 'APIMonitor_x64\x64\Release\APIMonitor_x64.dll' $stage -Force
Copy-Item 'APIMonitor_x64\x64\Release\APIMonitor_x64.pdb' $stage -Force
Copy-Item 'KswordARKDriver\x64\Release\KswordARK.sys' $stage -Force
Copy-Item 'KswordARKDriver\x64\Release\KswordARK.pdb' $stage -Force
Copy-Item 'KswordARKDriver\x64\Release\KswordARKDriver.inf' $stage -Force

$driverDir=Join-Path $stage 'KswordARKDriver'
if (!(Test-Path $driverDir)) { New-Item -ItemType Directory -Path $driverDir | Out-Null }
Copy-Item 'KswordARKDriver\x64\Release\KswordARK.sys' $driverDir -Force
Copy-Item 'KswordARKDriver\x64\Release\KswordARKDriver.inf' $driverDir -Force
```

### 3. 生成 7z 包

本机可能没有 `7z.exe` 在 PATH 中；已验证可用工具路径：`C:\Users\Felix\CLionProjects\Wisdom-Weasel\7z.exe`。压缩包建议放在 `dist\` 下，文件名带日期和本次功能描述。

```powershell
$seven='C:\Users\Felix\CLionProjects\Wisdom-Weasel\7z.exe'
$date=Get-Date -Format 'yyMMdd'
$archive=Join-Path (Get-Location) ("dist\KswordARK评估版本-$date-未签名R0-功能描述.7z")

if (Test-Path $archive) { Remove-Item -LiteralPath $archive -Force }
Push-Location 'dist\KswordARK-release-work'
& $seven a -t7z -mx=9 -mmt=on $archive 'Release'
$exit=$LASTEXITCODE
Pop-Location
if ($exit -ne 0) { exit $exit }
```

### 4. 校验发行包

生成后必须测试压缩包完整性，并列出关键文件确认最新 exe/dll/sys 已进入 `Release\`。

```powershell
$seven='C:\Users\Felix\CLionProjects\Wisdom-Weasel\7z.exe'
& $seven t $archive
& $seven l $archive 'Release\Ksword5.1.exe' 'Release\Taskbar.exe' 'Release\KswordHUD.exe' 'Release\APIMonitor_x64.dll' 'Release\KswordARK.sys' 'Release\KswordARKDriver\KswordARK.sys' 'Release\platforms\qwindows.dll'
```

校验通过时，`7z t` 输出应包含 `Everything is Ok`。本流程生成的包根目录必须是 `Release\`，不要把 `dist\KswordARK-release-work\` 或其它临时目录打进包里。
