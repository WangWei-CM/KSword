# 只编译生产规范文档和报告模型，用 QCoreApplication 验证数据，不创建 GUI 或临时构建目录。
param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$reportRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$reportOutput = Join-Path $reportRepository 'output'
if (!(Test-Path -LiteralPath $reportOutput)) { throw 'Existing output directory is required.' }
$reportQt = Join-Path $reportRepository '.deps/Qt/6.9.3/msvc2022_64'
$reportVc = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207'
$reportSdk = 'C:/Program Files (x86)/Windows Kits/10'
$reportSdkVersion = '10.0.26100.0'
$reportCompiler = Join-Path $reportVc 'bin/Hostx64/x64/cl.exe'
$reportLinker = Join-Path $reportVc 'bin/Hostx64/x64/link.exe'
foreach ($reportRequired in @($reportCompiler, $reportLinker,
    (Join-Path $reportQt 'lib/Qt6Core.lib'), (Join-Path $reportQt 'lib/Qt6Test.lib'))) {
    if (!(Test-Path -LiteralPath $reportRequired)) { throw "Missing HostX64 model dependency: $reportRequired" }
}

# 头文件含控件声明，但测试从不构造或链接 StructuredFieldView；运行对象只有 Qt Core 模型。
$reportIncludes = @(('/I' + $reportRepository), ('/external:I' + (Join-Path $reportVc 'include')))
foreach ($reportModule in @('', 'QtCore', 'QtGui', 'QtWidgets', 'QtTest')) {
    $reportIncludes += '/external:I' + (Join-Path $reportQt ('include/' + $reportModule))
}
foreach ($reportPart in @('ucrt', 'shared', 'um')) {
    $reportIncludes += '/external:I' + (Join-Path $reportSdk ('Include/' + $reportSdkVersion + '/' + $reportPart))
}
$reportSources = @('tools/complex_report_model_tests.cpp',
    'Ksword5.1/Ksword5.1/UI/ComplexReportModel.cpp', 'Ksword5.1/Ksword5.1/UI/FieldDocument.cpp')
$reportLibs = @(('/LIBPATH:' + (Join-Path $reportVc 'lib/x64')), ('/LIBPATH:' + (Join-Path $reportQt 'lib')))
foreach ($reportPart in @('ucrt', 'um')) {
    $reportLibs += '/LIBPATH:' + (Join-Path $reportSdk ('Lib/' + $reportSdkVersion + '/' + $reportPart + '/x64'))
}
$reportPreviousPath = $env:PATH
$reportPreviousPreferred = $env:PreferredToolArchitecture
$reportPreviousArchitecture = $env:PROCESSOR_ARCHITECTURE
$reportPreviousArchitectureW6432 = $env:PROCESSOR_ARCHITEW6432
try {
    # 编译器、链接器和子进程环境统一为 HostX64/x64，测试不需要平台插件或 moc 文件。
    $env:PreferredToolArchitecture = 'x64'
    $env:PROCESSOR_ARCHITECTURE = 'AMD64'
    $env:PROCESSOR_ARCHITEW6432 = 'AMD64'
    $reportObjects = @()
    foreach ($reportSource in $reportSources) {
        $reportObject = Join-Path $reportOutput ('complex_report_' + [IO.Path]::GetFileNameWithoutExtension($reportSource) + '.obj')
        Write-Output ('COMPLEX_REPORT_MODEL_COMPILE=' + $reportSource)
        & $reportCompiler /nologo /std:c++20 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /external:W0 /O1 /Gy /bigobj `
            /DQT_CORE_LIB /DQT_TESTLIB_LIB /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE `
            @reportIncludes /c (Join-Path $reportRepository $reportSource) ('/Fo' + $reportObject)
        if ($LASTEXITCODE -ne 0) { throw "Production model compilation failed: $reportSource" }
        $reportObjects += $reportObject
    }
    $reportExe = Join-Path $reportOutput 'complex_report_model_tests.exe'
    & $reportLinker /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OPT:REF ('/OUT:' + $reportExe) `
        @reportObjects @reportLibs Qt6Core.lib Qt6Test.lib
    if ($LASTEXITCODE -ne 0) { throw 'Production model test link failed.' }
    $env:PATH = (Join-Path $reportQt 'bin') + ';' + $reportPreviousPath
    & $reportExe 2>&1 | Tee-Object -FilePath (Join-Path $reportOutput 'complex_report_model_tests.log')
    $reportExitCode = $LASTEXITCODE
    Write-Output ('COMPLEX_REPORT_MODEL_EXIT_CODE=' + $reportExitCode)
    if ($reportExitCode -ne 0) { throw 'Production report model regression failed.' }
}
finally {
    $env:PATH = $reportPreviousPath
    $env:PreferredToolArchitecture = $reportPreviousPreferred
    $env:PROCESSOR_ARCHITECTURE = $reportPreviousArchitecture
    $env:PROCESSOR_ARCHITEW6432 = $reportPreviousArchitectureW6432
}
