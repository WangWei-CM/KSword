<#
.SYNOPSIS
    WinRT / WinUI 项目清理脚本

.DESCRIPTION
    清理本地项目中的常见中间产物与构建输出文件夹，支持可选删除 Generated Files，
    并在清空 .vs 文件夹时保留 .suo 用户配置文件，避免误删 NuGet 相关路径。

.AUTHOR
    涟幽Alex (AlexAlva)

.VERSION
    0.0.2

.CREATED
    2025-06-25

.LAST UPDATED
    2025-07-01

.NOTES
    - 支持日志级别选择（冗余、仅目录、仅错误）
    - 自动排除 packages、.nuget、Microsoft.NET 等目录
    - 支持检测文件占用，支持排除项目目录
    - 在 Windows PowerShell 5.1 / PowerShell Core 7+ 下测试通过

.EXAMPLE
    运行脚本并选择日志级别，即可自动清理构建文件夹：
    PS> .\ProjCleaner.ps1
#>


# ===================== 初始化 =====================

# ===================== 重入检查 =====================
$lockFile = Join-Path $PSScriptRoot ".lock"

if (-not (Test-Path $lockFile)) {
	# 创建锁文件（防止重复启动脚本）
	New-Item -ItemType File -Path $lockFile -Force | Out-Null
	$lockPath = Join-Path $PSScriptRoot ".lock"

	try {
		$lockStream = [System.IO.File]::Open(
			$lockPath,
			[System.IO.FileMode]::OpenOrCreate,
			[System.IO.FileAccess]::ReadWrite,
			[System.IO.FileShare]::None
		)
	} catch {
		Write-Host "⚠️ 无法锁定 .lock 文件，可能已有实例在运行。" -ForegroundColor Red
		Write-Host "5 秒后自动退出..." -ForegroundColor Yellow
        Start-Sleep -Seconds 5
		exit 1
	}
}  else {
    Write-Host "另一个进程正在使用此文件，脚本将退出。" -ForegroundColor Red
    Write-Host "如果您意外获得此信息，请检查目录下的.lock文件并删除它，然后你将可以继续使用脚本。" -ForegroundColor Red
    Write-Host "5 秒后自动退出..." -ForegroundColor Yellow
    Start-Sleep -Seconds 5
    exit 1
}

# ===================== 脚本简介 =====================
Write-Host ""
Write-Host "🧹 WinRT / WinUI 项目清理工具（v.0.0.2）" -ForegroundColor Cyan
Write-Host "---------------------------------------------"
Write-Host "本脚本将执行以下清理操作：" -ForegroundColor White
Write-Host "1. 删除常见构建目录（Debug、Release 等）"
Write-Host "2. 删除 'Generated Files'（可选）"
Write-Host "3. 清空 .vs 文件夹（保留 .suo 文件）"
Write-Host "4. 支持排除项目目录，支持自动检测文件占用"
Write-Host ""
Write-Host "注意：" -ForegroundColor Yellow
Write-Host " - NuGet packages、.nuget、Microsoft.NET 目录将自动排除，不会被误删"
Write-Host " - 请确保关闭 Visual Studio 后再运行脚本"
Write-Host ""

# ===================== 选择日志级别或退出 =====================
$logLevel = Read-Host "选择日志显示级别：
1）冗余（显示目录和文件）
2）仅限必要（仅显示目录[默认级别]）
3）仅限错误（仅出错提示）
输入 q 退出脚本
请输入对应数字（1/2/3 或 q）"

if ($logLevel -eq 'q' -or $logLevel -eq 'Q') {
    $lockStream.Close()
	$lockStream.Dispose()
	Remove-Item -Path $lockFile -Force -ErrorAction SilentlyContinue
    Write-Host "已退出脚本，未执行任何操作。" -ForegroundColor Yellow
    exit
}

switch ($logLevel) {
    '1' { $logMode = "verbose" }
    '2' { $logMode = "normal" }
    '3' { $logMode = "error" }
    default {
        Write-Host "无效的输入，将使用默认日志级别（normal）" -ForegroundColor Yellow
        $logMode = "normal"
    }
}

# ===================== 枚举同级目录，供用户选择排除 =====================
$basePath = $PSScriptRoot
$projectDirs = Get-ChildItem -Path $basePath -Directory | Where-Object {
    $_.Name -notmatch '^(\.git|\.vs|packages|\.nuget|Microsoft\.NET)$'
}

Write-Host ""
Write-Host "📁 当前目录下的项目列表：" -ForegroundColor Cyan
for ($i = 0; $i -lt $projectDirs.Count; $i++) {
    Write-Host "[$($i+1)] $($projectDirs[$i].Name)" -ForegroundColor Gray
}

Write-Host ""
$excludeInput = Read-Host "如需排除部分目录，请输入编号或名称（多个用英文逗号分隔，留空跳过）"

$excludedProjects = @()
if ($excludeInput -ne "") {
    $excludeItems = $excludeInput -split "," | ForEach-Object { $_.Trim() }

    foreach ($item in $excludeItems) {
        if ($item -match '^\d+$') {
            $index = [int]$item - 1
            if ($index -ge 0 -and $index -lt $projectDirs.Count) {
                $excludedProjects += $projectDirs[$index].FullName
            }
        } else {
            $found = $projectDirs | Where-Object { $_.Name -ieq $item }
            if ($found) {
                $excludedProjects += $found.FullName
            }
        }
    }

    Write-Host "🛑 排除以下目录：" -ForegroundColor Yellow
    $excludedProjects | ForEach-Object { Write-Host " - $_" -ForegroundColor DarkYellow }
} else {
    Write-Host "未排除任何目录。" -ForegroundColor Green
}


# ===================== 检查是否有进程占用项目目录 =====================
function Test-DirectoryLockedByIDE {
    param (
        [Parameter(Mandatory = $true)]
        [string]$DirectoryPath,

        [string]$HandlePath = (Join-Path $PSScriptRoot "handle.exe")
    )

    if (-not (Test-Path $HandlePath)) {
        throw "未找到 handle.exe，请确保它在 PATH 中或与脚本同目录。"
    }

    if (-not (Test-Path $DirectoryPath)) {
        throw "目录不存在: $DirectoryPath"
    }

    $DirectoryPath = (Resolve-Path $DirectoryPath).Path.TrimEnd('\')
    $pathNorm = $DirectoryPath.ToLower()

    Write-Host "🔍 正在检查是否有进程访问目录: $DirectoryPath" -ForegroundColor Cyan

    $output = & $HandlePath /accepteula $DirectoryPath 2>$null

    $activeHandles = $output | Where-Object {
        $_ -match "pid:" -and
        ($_ -notmatch "explorer.exe") -and
        ($_.ToLower().Contains($pathNorm))
    }

    if ($activeHandles.Count -eq 0) {
        Write-Host "✅ 当前无非 explorer.exe 进程访问该目录" -ForegroundColor Green
        return ,$false, @(), @()
    }

    # 提取唯一 PID 和进程名
    $pidMap = @{}
    foreach ($line in $activeHandles) {
        if ($line -match "^(?<proc>.+?)\s+pid:\s*(?<pid>\d+)\s+") {
            $procName = $matches["proc"].Trim()
            $procId = [int]$matches["pid"]
            if (-not $pidMap.ContainsKey($procId)) {
                $pidMap[$procId] = $procName
            }
        }
    }

    # 查询进程路径信息
    $processList = @()
    Write-Host "🔒 以下程序正在访问该目录或其子目录：" -ForegroundColor Yellow

    foreach ($kv in $pidMap.GetEnumerator()) {
        $procId = $kv.Key
        $procName = $kv.Value
        try {
            $procObj = Get-Process -Id $procId -ErrorAction Stop
            $exePath = $procObj.Path
        } catch {
            $exePath = "[未知或无权限]"
        }

        $info = [PSCustomObject]@{
            Name = $procName
            PID  = $procId
            Path = $exePath
        }
        $processList += $info
        Write-Host " • $($info.Name) [$($info.PID)] => $($info.Path)" -ForegroundColor DarkYellow
    }

    # 输出原始句柄信息
    Write-Host "`n📝 详细句柄信息：=========================>" -ForegroundColor Red
    $activeHandles | ForEach-Object { Write-Host $_ -ForegroundColor Cyan }

    # 返回三元组：被占用、进程列表、句柄列表
    return ,$true, $processList, $activeHandles
}

function Find-And-Test-ProjectDirs {
    param (
        [string[]]$ExcludedPaths,
        [string]$HandlePath = (Join-Path $PSScriptRoot "handle.exe")
    )

    $projectCandidates = Get-ChildItem -Path $PSScriptRoot -Recurse -Directory | Where-Object {
        $_.FullName -notmatch '\\packages\\|\\.nuget\\|Microsoft\.NET' -and
        -not ($ExcludedPaths -contains $_.FullName)
    }

    $projectDirs = @()

    foreach ($dir in $projectCandidates) {
        $hasSln = Test-Path -Path (Join-Path $dir.FullName "*.sln")
        $hasVs = Test-Path -Path (Join-Path $dir.FullName ".vs")
        if ($hasSln -or $hasVs) {
            $projectDirs += $dir.FullName
        }
    }

    if ($projectDirs.Count -eq 0) {
        Write-Host "⚠️ 未找到包含 .sln 或 .vs 的目录，跳过占用检查。" -ForegroundColor DarkYellow
        return
    }

    foreach ($projDir in $projectDirs) {
        $result = Test-DirectoryLockedByIDE -DirectoryPath $projDir -HandlePath $HandlePath
        $isLocked = $result[0]
        $processList = $result[1]
        #$handleList = $result[2]

        if ($isLocked) {
            $vsProcesses = $processList | Where-Object {
                $_.Name -ieq "devenv" -or
                ($_.Path -match "Microsoft Visual Studio" -or $_.Path -match "Common7\\IDE")
            }

            if ($vsProcesses.Count -gt 0) {
                Write-Host "`n⛔ Visual Studio 正在占用项目目录: $projDir" -ForegroundColor Red
                Write-Host "请关闭 Visual Studio 后重试。" -ForegroundColor Red
                Write-Host "5 秒后自动退出..." -ForegroundColor Yellow
                Start-Sleep -Seconds 5
                $lockStream.Close()
                $lockStream.Dispose()
                Remove-Item -Path $lockFile -Force -ErrorAction SilentlyContinue
                exit 1
            } else {
                Write-Host "`n⚠️ 项目目录正在被非 VS 进程占用: $projDir" -ForegroundColor DarkYellow
                Write-Host "请确认是否安全再执行清理。" -ForegroundColor Yellow
            }
        }
    }

    Write-Host "✅ 所有项目目录均未被占用。" -ForegroundColor Green
}


$projPL = Read-Host "将要检查项目目录是否被占用，您可以选择以下策略：
1）检查占用（可能需要几分钟的时间）
2）跳过检查（您需要确保项目未被 Visual Studio 占用）
请输入对应数字（1/2）"

switch ($projPL) {
    '1' { Find-And-Test-ProjectDirs -ExcludedPaths $excludedProjects }
    '2' { Write-Host "用户已选择跳过目录占用检查。" -ForegroundColor Cyan }
    default {
        Write-Host "无效的输入，默认执行检查。" -ForegroundColor Yellow
        Find-And-Test-ProjectDirs -ExcludedPaths $excludedProjects
    }
}

# =============================================================

function Log($message, $level = "normal", $color = "White") {
    if ($logMode -eq "verbose" -or ($logMode -eq "normal" -and $level -ne "verbose") -or $level -eq "error") {
        Write-Host $message -ForegroundColor $color
    }
}

function IsExcluded($path) {
    foreach ($exclude in $excludedProjects) {
        if ($path.StartsWith($exclude, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

Log "请稍候..." "normal" "Green"

# ===================== 删除构建目录 =====================
$foldersToRemove = @("ARM", "ARM64", "Debug", "Release", "x64")


foreach ($folderName in $foldersToRemove) {
    Log "正在处理构建文件中间目录: $folderName" "normal" "Cyan"
    
    $targets = Get-ChildItem -Recurse -Directory -Force -Include $folderName |
        Where-Object {
            $_.FullName -notmatch '\\packages\\' -and
            $_.FullName -notmatch '\\\.nuget\\' -and
            $_.FullName -notmatch '\\Microsoft\.NET\\' -and
            -not (IsExcluded $_.FullName)
        }

    foreach ($target in $targets) {
		try {
			Log "正在删除目录: $($target.FullName)" "normal" "Cyan"

			# 显示详细删除列表（仅 verbose）
			if ($logMode -eq "verbose") {
				Get-ChildItem -Path $target.FullName -Recurse -Force | ForEach-Object {
					Log "  删除文件: $($_.FullName)" "verbose" "DarkGray"
				}
			}

			Remove-Item -Path $target.FullName -Recurse -Force -ErrorAction Stop
		} catch {
			Log "删除文件或目录失败: $($target.FullName) - $($_.Exception.Message)" "error" "Red"
		}
	}
}

# ===================== 【可选】删除 Generated Files =====================
Log "`n`n将要检查生成器临时文件夹，若您选择清除，则对应项目需要重新编译才能正常使用。`n" "normal" "Red"

$generatedFiles = Get-ChildItem -Recurse -Directory -Force -Include "Generated Files" |
    Where-Object { -not (IsExcluded $_.FullName) }
foreach ($dir in $generatedFiles) {
    $userInput = Read-Host "发现生成器临时文件夹(Generated Files): '$($dir.FullName)'。是否删除？(y/n)"
    if ($userInput -eq 'y') {
        try {
            Log "正在删除目录: $($dir.FullName)" "normal" "Cyan"
            
            # 显示详细删除列表（仅 verbose）
            if ($logMode -eq "verbose") {
                Get-ChildItem -Path $dir.FullName -Recurse -Force | ForEach-Object {
                    Log "  删除文件: $($_.FullName)" "verbose" "DarkGray"
                }
            }
            
            Remove-Item -Path $dir.FullName -Recurse -Force -ErrorAction Stop
        } catch {
            Log "删除文件或目录失败: $($dir.FullName) - $($_.Exception.Message)" "error" "Red"
        }
    } else {
        Log "用户选择保留目录: $($dir.FullName)" "normal" "Yellow"
    }
}

# ===================== 删除 .vs 中内容（保留 .suo 文件） =====================
function Backup-SuoFiles {
    param (
        [string]$vsPath,
        [string]$backupRootPath
    )

    Log "正在处理 .vs 目录: $vsPath" "normal" "Magenta"

    # 查找 .suo 文件
    $suoFiles = Get-ChildItem -Path $vsPath -Recurse -Include *.suo -File -Force
    if ($suoFiles.Count -eq 0) {
        Log "未找到 .suo 文件，继续清理 .vs" "normal" "Gray"
        return
    }

    # 构建子目录名
    $guid = [guid]::NewGuid().ToString()
    $solutionName = Split-Path (Split-Path $vsPath -Parent) -Leaf
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $backupName = "${guid}_${solutionName}_${timestamp}"
    
    $backupPath = Join-Path $backupRoot $backupName
    New-Item -Path $backupPath -ItemType Directory | Out-Null

    # 复制 .suo 文件
    foreach ($suo in $suoFiles) {
        $relative = $suo.FullName.Substring($vsPath.Length + 1)
        $dest = Join-Path $backupPath $relative
        New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
        Copy-Item -Path $suo.FullName -Destination $dest -Force
        Log "备份 .suo 文件: $relative" "verbose" "Gray"
    }

    # 清空 .vs
    try {
        Remove-Item -Path "$vsPath\*" -Recurse -Force -ErrorAction Stop
        Log "清空 .vs 目录完成: $vsPath" "normal" "Cyan"
    } catch {
        Log "清空失败: $vsPath - $($_.Exception.Message)" "error" "Red"
    }

    # 还原 .suo 文件
    Get-ChildItem -Path $backupPath -Recurse -File | ForEach-Object {
        $dest = Join-Path $vsPath ($_.FullName.Substring($backupPath.Length + 1))
        New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
        Copy-Item -Path $_.FullName -Destination $dest -Force
        Log "已还原 .suo 文件: $dest" "verbose" "Green"
    }

    # 删除备份目录（可选）
    Remove-Item -Path $backupPath -Recurse -Force -ErrorAction SilentlyContinue
}

# 主临时目录
$backupRoot = Join-Path $PSScriptRoot ".vs_suo_backup"
if (-not (Test-Path $backupRoot)) {
	New-Item -Path $backupRoot -ItemType Directory | Out-Null
}


# 对所有 .vs 执行
Get-ChildItem -Recurse -Directory -Force -Include ".vs" | 
    Where-Object { -not (IsExcluded $_.FullName) } | ForEach-Object {
    Backup-SuoFiles $_.FullName $backupRoot
}

# 清理备份主目录
if (Test-Path $backupRoot) {
    $remaining = Get-ChildItem -Path $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
    if (-not $remaining) {
        try {
            Remove-Item -Path $backupRoot -Recurse -Force -ErrorAction Stop
            Log "已删除空的 .vs_suo_backup 临时目录: $backupRoot" "verbose" "Gray"
        } catch {
            Log "无法删除 .vs_suo_backup 临时目录: $($_.Exception.Message)" "error" "Red"
        }
    } else {
        Log ".vs_suo_backup 中仍有残留文件，未自动删除: $backupRoot" "normal" "Yellow"
    }
}

# 清理（释放文件句柄 + 删除.lock）
$lockStream.Close()
$lockStream.Dispose()
Remove-Item -Path $lockFile -Force -ErrorAction SilentlyContinue

Log "所有操作已经完成！" "normal" "Red"