param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory,
    [string]$QtDirectory = $env:KSWORD_QT_DIR
)
$ErrorActionPreference = 'Stop'
$commitRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $commitRoot '.codex-build-logs/ui-commit-logic-tests' }
$commitOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $commitOutput | Out-Null
if (!$QtDirectory) { $QtDirectory = Join-Path $commitRoot '.deps/Qt/6.9.3/msvc2022_64' }
if (!(Test-Path -LiteralPath (Join-Path $QtDirectory 'lib/Qt6Widgets.lib'))) { $QtDirectory = 'D:/Software/Qt/6.9.3/msvc2022_64' }
$commitVc = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207'
if (!(Test-Path -LiteralPath $commitVc)) { $commitVc = 'D:/Software/VS/VC/Tools/MSVC/14.44.35207' }
$commitSdk = 'C:/Program Files (x86)/Windows Kits/10'
$commitSdkVersion = '10.0.26100.0'
$commitCompiler = Join-Path $commitVc 'bin/Hostx64/x64/cl.exe'
$commitLinker = Join-Path $commitVc 'bin/Hostx64/x64/link.exe'
foreach ($commitRequired in @($commitCompiler, $commitLinker, (Join-Path $QtDirectory 'lib/Qt6Widgets.lib'))) {
    if (!(Test-Path -LiteralPath $commitRequired)) { throw "Missing HostX64 logic-test dependency: $commitRequired" }
}
$commitIncludes = @(('/I' + $commitRoot), ('/external:I' + (Join-Path $commitVc 'include')))
foreach ($commitModule in @('', 'QtCore', 'QtGui', 'QtWidgets')) {
    $commitIncludes += '/external:I' + (Join-Path $QtDirectory ('include/' + $commitModule))
}
foreach ($commitPart in @('ucrt', 'shared', 'um')) {
    $commitIncludes += '/external:I' + (Join-Path $commitSdk ('Include/' + $commitSdkVersion + '/' + $commitPart))
}
$commitObjects = @()
foreach ($commitSource in @('tools/ui_commit_logic_tests.cpp', 'Ksword5.1/Ksword5.1/UI/UiCommitCoordinator.cpp')) {
    $commitObject = Join-Path $commitOutput ([IO.Path]::GetFileNameWithoutExtension($commitSource) + '.obj')
    & $commitCompiler /nologo /std:c++20 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /external:W0 /O1 /Gy `
        /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE `
        @commitIncludes /c (Join-Path $commitRoot $commitSource) ('/Fo' + $commitObject)
    if ($LASTEXITCODE -ne 0) { throw "UI commit logic compilation failed: $commitSource" }
    $commitObjects += $commitObject
}
$commitExe = Join-Path $commitOutput 'ui_commit_logic_tests.exe'
$commitLibs = @(('/LIBPATH:' + (Join-Path $commitVc 'lib/x64')), ('/LIBPATH:' + (Join-Path $QtDirectory 'lib')))
foreach ($commitPart in @('ucrt', 'um')) {
    $commitLibs += '/LIBPATH:' + (Join-Path $commitSdk ('Lib/' + $commitSdkVersion + '/' + $commitPart + '/x64'))
}
& $commitLinker /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OPT:REF ('/OUT:' + $commitExe) `
    @commitObjects @commitLibs Qt6Widgets.lib Qt6Gui.lib Qt6Core.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw 'UI commit logic fixture link failed.' }
$commitOldPath = $env:PATH
try {
    $env:PATH = (Join-Path $QtDirectory 'bin') + ';' + $commitOldPath
    # 测试只创建 QCoreApplication，不创建 QApplication/QWidget 或加载平台插件。
    & $commitExe | Tee-Object -FilePath (Join-Path $commitOutput 'ui_commit_logic_tests.log')
    if ($LASTEXITCODE -ne 0) { throw 'UI commit logic regression failed.' }
}
finally { $env:PATH = $commitOldPath }
