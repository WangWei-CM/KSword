# 直接编译当前生产 MCP Qt/IPC 源码，只写仓库既有 output 目录。
# 默认运行纯 Qt 夹具；BuildOnly 供 Python 跨进程 MCP 验证复用同一可执行文件。
param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$BuildOnly,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$mcpRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path # 唯一构建工作区。
$mcpOutput = Join-Path $mcpRepository 'output' # 已有目录，禁止新建临时 build 树。
$mcpQt = Join-Path $mcpRepository '.deps\Qt\6.9.3\msvc2022_64' # 固定随附 Qt 版本。
$mcpVcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207' # 已知 64 位工具集。
$mcpSdkRoot = 'C:\Program Files (x86)\Windows Kits\10' # 本机 SDK 约定目录。
$mcpSdkVersion = '10.0.26100.0' # 与仓库当前独立夹具保持一致。
$mcpCompiler = Join-Path $mcpVcRoot 'bin\Hostx64\x64\cl.exe' # 必须使用 HostX64。
$mcpLinker = Join-Path $mcpVcRoot 'bin\Hostx64\x64\link.exe' # x64 主机链接器。
$mcpExe = Join-Path $mcpOutput 'mcp_ui_bridge_tests.exe' # 独立夹具产物，不覆盖生产 exe。
$mcpBuildLog = Join-Path $mcpOutput 'mcp-ui-bridge-build.log' # 仅此次编译的诊断记录。
$mcpRunLog = Join-Path $mcpOutput 'mcp-ui-bridge-tests.log' # 行为汇总与退出码证据。

# 所有输入路径都在固定约定目录查验，不递归搜索安装盘根。
if (!(Test-Path -LiteralPath $mcpOutput -PathType Container)) {
    throw 'The existing repository output directory is required.'
}
foreach ($mcpRequired in @($mcpQt, $mcpCompiler, $mcpLinker,
    (Join-Path $mcpQt 'plugins\platforms\qoffscreen.dll'))) {
    if (!(Test-Path -LiteralPath $mcpRequired)) {
        throw "Missing configured MCP fixture dependency: $mcpRequired"
    }
}
if ($mcpCompiler -notmatch '\\Hostx64\\x64\\cl\.exe$') {
    throw 'The MCP fixture compiler must use HostX64/x64.'
}

# 明确列出生产翻译单元，不用 glob 混入无关模块或另一进程的对象缓存。
$mcpSources = @(
    'tools\mcp_ui_bridge_tests.cpp',
    'Ksword5.1\Ksword5.1\UI\McpUiController.cpp',
    'Ksword5.1\Ksword5.1\UI\McpUiController.Fields.cpp',
    'Ksword5.1\Ksword5.1\UI\McpUiController.Tables.cpp',
    'Ksword5.1\Ksword5.1\UI\McpUiController.Adapters.cpp',
    'Ksword5.1\Ksword5.1\UI\McpLocalBridge.cpp'
) | ForEach-Object { Join-Path $mcpRepository $_ }
$mcpIncludeArgs = @() # 标准编译器与 ATL 头分别形成独立 argv 项。
$mcpIncludeArgs += '/I' + (Join-Path $mcpVcRoot 'include')
$mcpIncludeArgs += '/I' + (Join-Path $mcpVcRoot 'atlmfc\include')
foreach ($mcpModule in @('', 'QtCore', 'QtGui', 'QtWidgets', 'QtNetwork', 'QtSvg')) {
    $mcpIncludeArgs += '/external:I' + (Join-Path $mcpQt ('include\' + $mcpModule))
}
foreach ($mcpPart in @('ucrt', 'shared', 'um', 'winrt')) {
    $mcpIncludeArgs += '/external:I' + (Join-Path $mcpSdkRoot ('Include\' + $mcpSdkVersion + '\' + $mcpPart))
}
$mcpLibraryArgs = @() # 本机 x64 库，不借生产 Release 对象，每条为独立 argv 项。
$mcpLibraryArgs += '/LIBPATH:' + (Join-Path $mcpQt 'lib')
$mcpLibraryArgs += '/LIBPATH:' + (Join-Path $mcpVcRoot 'lib\x64')
$mcpLibraryArgs += '/LIBPATH:' + (Join-Path $mcpVcRoot 'atlmfc\lib\x64')
foreach ($mcpPart in @('ucrt', 'um')) {
    $mcpLibraryArgs += '/LIBPATH:' + (Join-Path $mcpSdkRoot ('Lib\' + $mcpSdkVersion + '\' + $mcpPart + '\x64'))
}

if (!$SkipBuild) {
    [IO.File]::WriteAllText($mcpBuildLog, '', [Text.UTF8Encoding]::new($false))
    $mcpObjects = @() # 每个独立翻译单元的带前缀对象路径。
    foreach ($mcpSource in $mcpSources) {
        if (!(Test-Path -LiteralPath $mcpSource -PathType Leaf)) {
            throw "Missing production MCP fixture source: $mcpSource"
        }
        $mcpObject = Join-Path $mcpOutput ('mcp-fixture-' + [IO.Path]::GetFileNameWithoutExtension($mcpSource) + '.obj')
        Write-Output ('MCP_BUILD_SOURCE=' + [IO.Path]::GetFileName($mcpSource))
        $mcpDiagnostics = & $mcpCompiler /nologo /c /std:c++20 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /external:W0 `
            /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /DQT_NETWORK_LIB `
            @mcpIncludeArgs $mcpSource ('/Fo' + $mcpObject) 2>&1
        $mcpCompileExit = $LASTEXITCODE # 在写日志前捕获原编译器退出码。
        $mcpDiagnostics | Out-File -LiteralPath $mcpBuildLog -Append -Encoding utf8
        if ($mcpCompileExit -ne 0) {
            $mcpDiagnostics | Select-Object -Last 18 | Write-Output
            throw "MCP fixture compilation failed: $mcpSource (exit $mcpCompileExit)."
        }
        $mcpObjects += $mcpObject
    }
    $mcpLinkDiagnostics = & $mcpLinker /nologo @mcpObjects ('/OUT:' + $mcpExe) `
        /SUBSYSTEM:CONSOLE /MACHINE:X64 /INCREMENTAL:NO @mcpLibraryArgs `
        Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6Network.lib user32.lib advapi32.lib 2>&1
    $mcpLinkExit = $LASTEXITCODE # 链接失败不得被普通 Qt fixture 运行掩盖。
    $mcpLinkDiagnostics | Out-File -LiteralPath $mcpBuildLog -Append -Encoding utf8
    if ($mcpLinkExit -ne 0) {
        $mcpLinkDiagnostics | Select-Object -Last 18 | Write-Output
        throw "MCP fixture link failed (exit $mcpLinkExit)."
    }
    Write-Output 'MCP_BUILD_RESULT=PASS'
    Write-Output ('MCP_BUILD_COMPILER=' + $mcpCompiler)
}
if ($BuildOnly) {
    Write-Output ('MCP_FIXTURE_EXE=' + $mcpExe)
    return
}
if (!(Test-Path -LiteralPath $mcpExe -PathType Leaf)) {
    throw 'MCP fixture executable is missing.'
}

# 只在当前脚本进程临时设置 Qt 运行路径，finally 恢复；不写全局环境或注册表。
$mcpPreviousPath = $env:PATH # 运行前进程 DLL 搜索路径。
$mcpPreviousPlatform = $env:QT_QPA_PLATFORM # 运行前平台插件选择。
$mcpPreviousPlugins = $env:QT_PLUGIN_PATH # 运行前插件搜索路径。
try {
    $env:PATH = (Join-Path $mcpQt 'bin') + ';' + $mcpPreviousPath
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_PLUGIN_PATH = Join-Path $mcpQt 'plugins'
    $mcpRunOutput = & $mcpExe 2>&1 # 仅启动独立合成控件夹具。
    $mcpRunExit = $LASTEXITCODE # 原生退出码与汇总必须同时通过。
    $mcpRunOutput | Out-File -LiteralPath $mcpRunLog -Encoding utf8
    $mcpRunOutput | Where-Object { $_ -match '^(FAIL:|MCP_UI_)' } | Write-Output
    Write-Output ('MCP_UI_EXIT_CODE=' + $mcpRunExit)
    if ($mcpRunExit -ne 0 -or !($mcpRunOutput -match '^MCP_UI_RESULT=PASS$') -or
        !($mcpRunOutput -match '^MCP_UI_FAILURES=0$')) {
        throw 'MCP Qt fixture behavioral regression failed or produced no valid summary.'
    }
}
finally {
    $env:PATH = $mcpPreviousPath
    $env:QT_QPA_PLATFORM = $mcpPreviousPlatform
    $env:QT_PLUGIN_PATH = $mcpPreviousPlugins
}
