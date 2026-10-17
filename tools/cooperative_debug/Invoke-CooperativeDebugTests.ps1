param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
# 构建固定使用 64 位 MSBuild 与 HostX64，不安装或启动任何驱动。
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { $msbuild = 'D:\Software\VS\MSBuild\Current\Bin\amd64\MSBuild.exe' }
if (!(Test-Path -LiteralPath $msbuild)) { throw '64-bit MSBuild is required.' }
$architecture = @('/p:PreferredToolArchitecture=x64', '/p:PROCESSOR_ARCHITECTURE=AMD64', '/p:PROCESSOR_ARCHITEW6432=AMD64')
# 读回实际导入的工具搜索路径，不能只相信命令行工具偏好。
$properties = & $msbuild (Join-Path $PSScriptRoot 'CooperativeDebugRuntime.vcxproj') /p:Configuration=Release /p:Platform=x64 @architecture '-getProperty:PreferredToolArchitecture,VC_ExecutablePath_x64' /nologo
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect cooperative build tool architecture.' }
$toolProperties = ($properties | ConvertFrom-Json).Properties
if ($toolProperties.PreferredToolArchitecture -ne 'x64' -or $toolProperties.VC_ExecutablePath_x64 -notmatch 'HostX64') {
    throw 'Cooperative builds require the HostX64 compiler and linker.'
}
Write-Output 'COOPERATIVE_TOOL_HOST=HostX64'
foreach ($project in @('CooperativeDebugDemo.vcxproj', 'CooperativeDebugTests.vcxproj')) {
    & $msbuild (Join-Path $PSScriptRoot $project) /t:Build /p:Configuration=Release /p:Platform=x64 @architecture /m:1 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Cooperative build failed: $project ($LASTEXITCODE)" }
}
if (!$BuildOnly) {
    $repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    & (Join-Path $repository 'output\cooperative_debug\Release\CooperativeDebugTests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Cooperative tests failed: $LASTEXITCODE" }
}
