param([string]$RepositoryRoot=(Split-Path -Parent $PSScriptRoot), [string]$HeaderDirectory='', [string]$BuildDirectory='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath $RepositoryRoot).Path
if (!$HeaderDirectory) { $HeaderDirectory=Join-Path $repo 'Ksword5.1/Ksword5.1/UI' }
if (!$BuildDirectory) { $BuildDirectory=Join-Path $repo '.codex-build-logs/flat-table-model-tests' }
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$qt=Join-Path $repo '.deps/Qt/6.9.3/msvc2022_64'
if (!(Test-Path -LiteralPath "$qt/include/QtCore")) { $qt='D:/Software/Qt/6.9.3/msvc2022_64' }
$vc='D:/Software/VS/VC/Tools/MSVC/14.44.35207'
if (!(Test-Path -LiteralPath "$vc/bin/Hostx64/x64/cl.exe")) {
 $vswhere='C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
 $vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
 $vcVersion=(Get-Content -LiteralPath (Join-Path $vsPath 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
 $vc=Join-Path $vsPath "VC/Tools/MSVC/$vcVersion"
}
$cl=Join-Path $vc 'bin/Hostx64/x64/cl.exe'
$sdk='C:/Program Files (x86)/Windows Kits/10'
$sdkVersion=(Get-ChildItem -LiteralPath "$sdk/Include" -Directory | Where-Object {Test-Path -LiteralPath "$($_.FullName)/ucrt"} | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).Name
$oldInclude=$env:INCLUDE;$oldLib=$env:LIB;$oldPath=$env:PATH
try {
 $env:INCLUDE=@("$vc/include","$sdk/Include/$sdkVersion/ucrt","$sdk/Include/$sdkVersion/shared","$sdk/Include/$sdkVersion/um") -join ';'
 $env:LIB=@("$vc/lib/x64","$sdk/Lib/$sdkVersion/ucrt/x64","$sdk/Lib/$sdkVersion/um/x64","$qt/lib") -join ';'
 $env:PATH="$qt/bin;$oldPath"
 $source=Join-Path $PSScriptRoot 'flat_table_model_tests.cpp'
 foreach($mode in @('release','assertions')) {
  $obj=Join-Path $BuildDirectory "flat_table_model_tests-$mode.obj"
  $exe=Join-Path $BuildDirectory "flat_table_model_tests-$mode.exe"
  $defines=@('/DNOMINMAX')
  if($mode -eq 'release') { $defines+='/DQT_NO_DEBUG' }
  & $cl /nologo /std:c++17 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /O2 /W4 /WX @defines "/I$HeaderDirectory" "/I$repo/Ksword5.1/Ksword5.1/UI" "/I$qt/include" "/I$qt/include/QtCore" $source "/Fo$obj" "/Fe$exe" /link /INCREMENTAL:NO Qt6Core.lib
  if($LASTEXITCODE -ne 0) { throw "FlatTableModel $mode compile failed" }
  & $exe
  if($LASTEXITCODE -ne 0) { throw "FlatTableModel $mode tests failed" }
 }
} finally { $env:INCLUDE=$oldInclude;$env:LIB=$oldLib;$env:PATH=$oldPath }
