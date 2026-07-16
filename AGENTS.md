# KSword Agent Notes

## 发行版制作流程

本流程用于在仓库根目录 `C:\Users\Felix\CLionProjects\KSword` 生成包含完整 `Release\` 目录的 7z 发行包。默认参考旧包布局：`C:\Users\Felix\Downloads\KswordARK评估版本-260427-未签名R0-进程内存监控增强.7z`。

### 0. GPLv3 许可证发行闸门

项目自有代码现按 `GPL-3.0-only` 发布；Qt Charts 和 EASY-HWID-SPOOFER 与旧自定义
许可证的两个不兼容问题，已经通过选择 GPLv3 组合发布路径关闭。制作对外发行包前
仍必须复核 `docs/许可证兼容性审计.md`，并完成以下发行义务：

- 固定本次所有 exe、DLL、sys 和工具对应的项目源码提交，确保提供的源码能够对应
  并构建实际发行二进制；
- 固定 EASY-HWID-SPOOFER、Qt 6.9.3/Qt Charts、Qt Advanced Docking System、
  FLTK 及其他预编译依赖的准确版本、来源、修改和许可证；
- 按 GPLv3 第 6 节提供完整对应源码，包括项目源码、修改后的第三方源码以及控制
  编译和安装所需的工程、脚本和配置；
- 随包提供根 `LICENSE`、`PROJECT_LICENSE.md`、`COMMUNITY_COVENANT.md`、
  `THIRD_PARTY_NOTICES.md`、组件级 LICENSE/NOTICE、Qt SBOM、模块清单和明确的
  对应源码取得说明；
- 对最终发行目录进行实际二进制依赖审计，确认不存在来源不明、禁止再分发或与
  GPLv3 不兼容的材料。

以上材料未完成前，本流程只能用于本地测试包，不能把产物标记为“GPL 发行合规
已完成”“许可证审计通过”或对外发行。社区公约不是 GPL 附加条件，不能代替上述
法律义务。

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
Copy-Item 'LICENSE' (Join-Path $stage 'LICENSE') -Force
Copy-Item 'PROJECT_LICENSE.md' (Join-Path $stage 'PROJECT_LICENSE.md') -Force
Copy-Item 'COMMUNITY_COVENANT.md' (Join-Path $stage 'COMMUNITY_COVENANT.md') -Force
Copy-Item 'THIRD_PARTY_NOTICES.md' (Join-Path $stage 'THIRD_PARTY_NOTICES.md') -Force

$licenseDir=Join-Path $stage 'licenses\third_party'
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
Copy-Item 'third_party\systeminformer_dyn\LICENSE.txt' (Join-Path $licenseDir 'systeminformer-LICENSE.txt') -Force
Copy-Item 'third_party\systeminformer_dyn\NOTICE.md' (Join-Path $licenseDir 'systeminformer-NOTICE.md') -Force
Copy-Item 'third_party\easy_hwid_spoofer\LICENSE.txt' (Join-Path $licenseDir 'easy-hwid-spoofer-LICENSE.txt') -Force
Copy-Item 'third_party\easy_hwid_spoofer\NOTICE.md' (Join-Path $licenseDir 'easy-hwid-spoofer-NOTICE.md') -Force
Copy-Item 'third_party\fltk\LICENSE.txt' (Join-Path $licenseDir 'fltk-LICENSE.txt') -Force
Copy-Item 'third_party\fltk\NOTICE.md' (Join-Path $licenseDir 'fltk-NOTICE.md') -Force
Copy-Item 'third_party\qt_advanced_docking_system\LICENSE.txt' (Join-Path $licenseDir 'qt-advanced-docking-system-LICENSE.txt') -Force
Copy-Item 'third_party\qt_advanced_docking_system\NOTICE.md' (Join-Path $licenseDir 'qt-advanced-docking-system-NOTICE.md') -Force

$profileDir=Join-Path $stage 'profiles'
if (!(Test-Path $profileDir)) { New-Item -ItemType Directory -Path $profileDir | Out-Null }
Copy-Item 'Ksword5.1\Ksword5.1\x64\Release\profiles\ark_dyndata_pack_v3.json' $profileDir -Force
Copy-Item 'Ksword5.1\Ksword5.1\x64\Release\profiles\ark_dyndata_pack_v2.json' $profileDir -Force
Copy-Item 'Ksword5.1\Ksword5.1\x64\Release\profiles\registry_optimization_items.json' $profileDir -Force
Copy-Item 'Ksword5.1\Ksword5.1\x64\Release\profiles\registry_optimization_assets' $profileDir -Recurse -Force

$languageDir=Join-Path $stage 'languages'
if (Test-Path $languageDir) { Remove-Item -LiteralPath $languageDir -Recurse -Force }
Copy-Item 'Ksword5.1\Ksword5.1\x64\Release\languages' $stage -Recurse -Force

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
& $seven l $archive 'Release\Ksword5.1.exe' 'Release\Taskbar.exe' 'Release\KswordHUD.exe' 'Release\APIMonitor_x64.dll' 'Release\KswordARK.sys' 'Release\KswordARKDriver\KswordARK.sys' 'Release\LICENSE' 'Release\PROJECT_LICENSE.md' 'Release\COMMUNITY_COVENANT.md' 'Release\THIRD_PARTY_NOTICES.md' 'Release\licenses\third_party\systeminformer-LICENSE.txt' 'Release\licenses\third_party\easy-hwid-spoofer-LICENSE.txt' 'Release\licenses\third_party\fltk-LICENSE.txt' 'Release\licenses\third_party\qt-advanced-docking-system-LICENSE.txt' 'Release\profiles\ark_dyndata_pack_v3.json' 'Release\profiles\registry_optimization_items.json' 'Release\profiles\registry_optimization_assets\Config\Data.zip' 'Release\languages\zh-CN.json' 'Release\languages\en-US.json' 'Release\platforms\qwindows.dll'
```

校验通过时，`7z t` 输出应包含 `Everything is Ok`，且列表中必须包含 `Release\LICENSE`、`Release\PROJECT_LICENSE.md`、`Release\COMMUNITY_COVENANT.md`、`Release\THIRD_PARTY_NOTICES.md` 和组件级 LICENSE/NOTICE；主程序顶部“许可证”页面从 exe 同目录优先读取根许可证。本流程生成的包根目录必须是 `Release\`，不要把 `dist\KswordARK-release-work\` 或其它临时目录打进包里。Qt GPL/LGPL 文本、Qt SBOM、模块清单和完整对应源码取得材料仍须按第 0 节的发行闸门另行加入并核验。


## Phase -1 协作规范

- 仓库相对根目录为 `H:/Project/Ksword5.1`；文档与规则使用相对路径，不写个人机器路径作为开发落点。
- R0/R3 协议只在 `shared/driver/` 定义。
- 驱动 IOCTL 分发只通过 `KswordARKDriver/src/dispatch/ioctl_registry.c` 注册 handler，`ioctl_dispatch.c` 不再承载业务 switch。
- 用户态 KswordARK 设备访问只通过 `Ksword5.1/Ksword5.1/ArkDriverClient/`，Dock UI 不直接调用 KswordARK `DeviceIoControl`。
- `KswordCLI` 每新增、删除或调整一个命令/别名/参数时，必须同步更新 `KswordCLI` 内置 `help` 命令元数据，并同步更新 `docs/CLI使用文档.md`。
- 新增源码必须同步更新对应 `.vcxproj` 和 `.vcxproj.filters`。
- 第三方代码接入必须带 LICENSE 和 NOTICE。
- 新增、删除或修改主程序用户可见文本时，必须同步 `Ksword5.1/Ksword5.1/languages/zh-CN.json` 与 `Ksword5.1/Ksword5.1/languages/en-US.json`，并通过 `python tools/i18n_language_pack.py audit --source-root Ksword5.1/Ksword5.1 --zh-pack Ksword5.1/Ksword5.1/languages/zh-CN.json --en-pack Ksword5.1/Ksword5.1/languages/en-US.json`。
