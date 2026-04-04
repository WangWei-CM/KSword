$ErrorActionPreference = 'Stop'

# Write-Stage 作用：
# - 输出统一格式的阶段日志，方便观察脚本执行过程；
# - 调用方式：Write-Stage "文本"。
function Write-Stage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )
    Write-Host "[ReleaseInfo] $Message"
}

# Escape-CppStringLiteral 作用：
# - 把输入文本转为可安全写入 QStringLiteral("...") 的内容；
# - 调用方式：Escape-CppStringLiteral -RawText "v1.2.3"；
# - 入参 RawText：待写入的原始文本；
# - 出参：转义后的 C++ 字符串文本。
function Escape-CppStringLiteral {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RawText
    )

    # escapedText 作用：保存逐步转义后的文本，先处理反斜杠再处理双引号。
    $escapedText = $RawText.Replace('\', '\\')
    $escapedText = $escapedText.Replace('"', '\"')
    return $escapedText
}

# Resolve-ReleaseType 作用：
# - 把用户输入映射到规范发布类型（normal/preview/dev）；
# - 调用方式：Resolve-ReleaseType -InputText "预览版"；
# - 入参 InputText：用户输入的发布类型文本；
# - 出参：规范类型字符串，无法识别时抛错。
function Resolve-ReleaseType {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InputText
    )

    # normalizedText 作用：统一大小写与空白，降低输入格式差异影响。
    $normalizedText = $InputText.Trim().ToLower()
    switch ($normalizedText) {
        '1' { return 'normal' }
        '2' { return 'preview' }
        '3' { return 'dev' }
        'normal' { return 'normal' }
        'release' { return 'normal' }
        '正式' { return 'normal' }
        '正式版' { return 'normal' }
        '普通' { return 'normal' }
        '普通版' { return 'normal' }
        'preview' { return 'preview' }
        'pre' { return 'preview' }
        '预览' { return 'preview' }
        '预览版' { return 'preview' }
        'dev' { return 'dev' }
        'debug' { return 'dev' }
        '开发' { return 'dev' }
        '开发版' { return 'dev' }
        default {
            throw ("无法识别发布类型：{0}。可用值：normal / preview / dev（或中文：普通/预览/开发）。" -f $InputText)
        }
    }
}

# Update-QStringLiteralByMarker 作用：
# - 通过“同一行注释标记”定位并替换 QStringLiteral("...") 内容；
# - 调用方式：Update-QStringLiteralByMarker -FilePath xxx -Marker xxx -NewValue xxx；
# - 入参 FilePath：目标文件路径；
# - 入参 Marker：注释标记文本；
# - 入参 NewValue：待写入的新值（脚本内部会自动转义）；
# - 出参：无（直接回写文件）。
function Update-QStringLiteralByMarker {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string]$Marker,
        [Parameter(Mandatory = $true)]
        [string]$NewValue
    )

    if (-not (Test-Path -LiteralPath $FilePath)) {
        throw "文件不存在：$FilePath"
    }

    # fileLines 作用：按行加载文件文本，便于基于标记定位替换目标行。
    $fileLines = [System.Collections.Generic.List[string]]::new()
    $fileLines.AddRange([string[]](Get-Content -LiteralPath $FilePath))

    # targetLineIndex 作用：记录包含标记的目标行索引，未找到则保持 -1。
    $targetLineIndex = -1
    for ($lineIndex = 0; $lineIndex -lt $fileLines.Count; $lineIndex++) {
        if ($fileLines[$lineIndex].Contains($Marker)) {
            $targetLineIndex = $lineIndex
            break
        }
    }
    if ($targetLineIndex -lt 0) {
        throw "未找到标记：$Marker（文件：$FilePath）"
    }

    # sourceLine 作用：读取原始目标行，执行正则替换后回写。
    $sourceLine = $fileLines[$targetLineIndex]
    # patternText 作用：匹配单个 QStringLiteral("...") 片段。
    $patternText = 'QStringLiteral\("([^"\\]|\\.)*"\)'
    if (-not [System.Text.RegularExpressions.Regex]::IsMatch($sourceLine, $patternText)) {
        throw ("标记行未找到可替换的 QStringLiteral 字面量：{0}（文件：{1}）" -f $Marker, $FilePath)
    }

    # escapedValue 作用：保存已转义的新值，避免破坏 C++ 字符串语法。
    $escapedValue = Escape-CppStringLiteral -RawText $NewValue
    # replacementText 作用：构造完整 QStringLiteral("...") 片段用于替换。
    $replacementText = "QStringLiteral(`"$escapedValue`")"
    $updatedLine = [System.Text.RegularExpressions.Regex]::Replace(
        $sourceLine,
        $patternText,
        [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $replacementText },
        [System.Text.RegularExpressions.RegexOptions]::None,
        [TimeSpan]::FromSeconds(2))

    $fileLines[$targetLineIndex] = $updatedLine
    try {
        Set-Content -LiteralPath $FilePath -Value $fileLines -Encoding UTF8
    }
    catch {
        throw "写入文件失败（可能被 IDE 占用）：$FilePath；原始错误：$($_.Exception.Message)"
    }
}

# Update-NextLineByMarker 作用：
# - 通过“标记所在行的下一行”替换目标文本；
# - 调用方式：Update-NextLineByMarker -FilePath xxx -Marker xxx -NewLine xxx；
# - 入参 FilePath：目标文件路径；
# - 入参 Marker：注释标记文本；
# - 入参 NewLine：替换后的完整行文本；
# - 出参：无（直接回写文件）。
function Update-NextLineByMarker {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string]$Marker,
        [Parameter(Mandatory = $true)]
        [string]$NewLine
    )

    if (-not (Test-Path -LiteralPath $FilePath)) {
        throw "文件不存在：$FilePath"
    }

    # fileLines 作用：按行加载文件文本，基于标记定位下一行进行替换。
    $fileLines = [System.Collections.Generic.List[string]]::new()
    $fileLines.AddRange([string[]](Get-Content -LiteralPath $FilePath))

    # markerLineIndex 作用：记录包含标记的行索引，用于计算下一行位置。
    $markerLineIndex = -1
    for ($lineIndex = 0; $lineIndex -lt $fileLines.Count; $lineIndex++) {
        if ($fileLines[$lineIndex].Contains($Marker)) {
            $markerLineIndex = $lineIndex
            break
        }
    }
    if ($markerLineIndex -lt 0) {
        throw "未找到标记：$Marker（文件：$FilePath）"
    }
    if ($markerLineIndex + 1 -ge $fileLines.Count) {
        throw "标记后不存在可替换行：$Marker（文件：$FilePath）"
    }

    $fileLines[$markerLineIndex + 1] = $NewLine
    try {
        Set-Content -LiteralPath $FilePath -Value $fileLines -Encoding UTF8
    }
    catch {
        throw "写入文件失败（可能被 IDE 占用）：$FilePath；原始错误：$($_.Exception.Message)"
    }
}

# ===================== 主流程 =====================

# scriptRootPath 作用：脚本所在目录（项目根目录），用于拼接相对路径。
$scriptRootPath = Split-Path -Parent $MyInvocation.MyCommand.Path
# sourceRootPath 作用：Ksword5.1 项目源目录（包含 WelcomeDock / qrc / rc）。
$sourceRootPath = Join-Path $scriptRootPath 'Ksword5.1'

# welcomeDockFilePath 作用：欢迎页源码目标文件。
$welcomeDockFilePath = Join-Path $sourceRootPath 'WelcomeDock\WelcomeDock.cpp'
# appIconRcFilePath 作用：原生 Win32 资源脚本文件（用于替换 EXE 图标）。
$appIconRcFilePath = Join-Path $sourceRootPath 'AppIcon.rc'

Write-Stage "准备更新发布信息。"
Write-Stage "目标目录：$sourceRootPath"

# versionInputText 作用：保存用户输入的版本号字符串（例如 v5.1.0-preview1）。
$versionInputText = Read-Host '请输入当前版本号（字符串）'
if ([string]::IsNullOrWhiteSpace($versionInputText)) {
    throw '版本号不能为空。'
}

# releaseTypeInputText 作用：保存用户输入的发布类型文本。
$releaseTypeInputText = Read-Host '请输入发布类型（1=普通, 2=预览, 3=开发；也支持 normal/preview/dev）'
# releaseTypeKey 作用：发布类型规范化结果，仅取 normal/preview/dev 之一。
$releaseTypeKey = Resolve-ReleaseType -InputText $releaseTypeInputText

# buildTimeText 作用：记录脚本执行时的精确构建时间文本（含毫秒与时区）。
$buildTimeText = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff K')

# appIconRelativePath 作用：根据发布类型选择 EXE 图标对应的 ICO 资源路径。
$appIconRelativePath = switch ($releaseTypeKey) {
    'normal' { 'Resource/Logo/KswordLogo.ico' }
    'preview' { 'Resource/Logo/KswordLogo_PRE.ico' }
    'dev' { 'Resource/Logo/KswordLogo_DEV.ico' }
    default { throw "内部错误：未知发布类型 $releaseTypeKey" }
}

# appIconAbsolutePath 作用：把相对路径映射到项目绝对路径，提前校验 ICO 文件存在性。
$appIconAbsolutePath = Join-Path $sourceRootPath $appIconRelativePath
if (-not (Test-Path -LiteralPath $appIconAbsolutePath)) {
    throw "目标应用程序图标文件不存在：$appIconAbsolutePath"
}

Write-Stage "版本号：$versionInputText"
Write-Stage "发布类型：$releaseTypeKey"
Write-Stage "编译时间：$buildTimeText"
Write-Stage "应用程序图标：$appIconRelativePath"

# 1) 更新欢迎页版本号（更大字号显示，文本源位于 WelcomeDock.cpp 标记行）。
Update-QStringLiteralByMarker `
    -FilePath $welcomeDockFilePath `
    -Marker 'RELEASE_META_VERSION_MARKER' `
    -NewValue $versionInputText

# 2) 更新欢迎页编译时间备注（精确到毫秒）。
Update-QStringLiteralByMarker `
    -FilePath $welcomeDockFilePath `
    -Marker 'RELEASE_META_BUILD_TIME_MARKER' `
    -NewValue $buildTimeText

# 3) 更新原生 Win32 资源图标（按发布类型切换为不同 ICO 文件）。
$appIconResourceLine = "IDI_APP_ICON ICON `"$appIconRelativePath`""
Update-NextLineByMarker `
    -FilePath $appIconRcFilePath `
    -Marker 'RELEASE_APP_ICON_FILE_MARKER' `
    -NewLine $appIconResourceLine

Write-Stage '发布信息更新完成。'
Write-Stage "已更新：$welcomeDockFilePath"
Write-Stage "已更新：$appIconRcFilePath"
