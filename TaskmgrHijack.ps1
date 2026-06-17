#requires -Version 5.1

param(
    [switch]$Install,
    [switch]$Uninstall,
    [string]$TargetExe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ifeoKeyPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe"
$debuggerValueName = "Debugger"

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Ensure-Administrator {
    if (Test-Administrator) {
        return $true
    }

    $scriptPath = $PSCommandPath
    if (-not $scriptPath) {
        throw "Unable to determine the script path for elevation."
    }

    $shellPath = (Get-Process -Id $PID).Path
    $argumentLine = "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`""
    if ($Install) { $argumentLine += " -Install" }
    if ($Uninstall) { $argumentLine += " -Uninstall" }
    if (-not [string]::IsNullOrWhiteSpace($TargetExe)) {
        $argumentLine += " -TargetExe `"$TargetExe`""
    }

    try {
        Start-Process -FilePath $shellPath -Verb RunAs -WorkingDirectory (Get-Location).Path -ArgumentList $argumentLine | Out-Null
        Write-Host "Elevation requested. Continue in the new administrator window."
    } catch [System.ComponentModel.Win32Exception] {
        if ($_.Exception.NativeErrorCode -eq 1223) {
            throw "Administrator elevation was canceled."
        }
        throw
    }

    return $false
}

function Get-TargetExePath {
    param([string]$TargetExe)

    if (-not [string]::IsNullOrWhiteSpace($TargetExe)) {
        if (-not (Test-Path -LiteralPath $TargetExe -PathType Leaf)) {
            throw "Target executable was not found: $TargetExe"
        }
        return (Resolve-Path -LiteralPath $TargetExe).Path
    }

    $legacyPath = Join-Path -Path (Get-Location).Path -ChildPath "Ksword5.1.exe"
    if (Test-Path -LiteralPath $legacyPath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $legacyPath).Path
    }

    throw "Ksword5.1.exe was not found. Use -TargetExe to provide the installed main executable path."
}

function Install-TaskmgrHijack {
    param([string]$TargetExe)

    $exePath = Get-TargetExePath -TargetExe $TargetExe
    $debuggerValue = '"' + $exePath + '"'
    New-Item -Path $ifeoKeyPath -Force | Out-Null
    New-ItemProperty -Path $ifeoKeyPath -Name $debuggerValueName -Value $debuggerValue -PropertyType String -Force | Out-Null

    Write-Host "Install complete."
    Write-Host "taskmgr.exe IFEO Debugger -> $debuggerValue"
}

function Uninstall-TaskmgrHijack {
    if (-not (Test-Path -LiteralPath $ifeoKeyPath)) {
        Write-Host "No IFEO key found for taskmgr.exe."
        return
    }

    Remove-ItemProperty -Path $ifeoKeyPath -Name $debuggerValueName -ErrorAction SilentlyContinue

    $remainingValues = (Get-Item -LiteralPath $ifeoKeyPath).Property
    if (-not $remainingValues -or $remainingValues.Count -eq 0) {
        Remove-Item -LiteralPath $ifeoKeyPath -Force
    }

    Write-Host "Uninstall complete."
    Write-Host "taskmgr.exe IFEO Debugger removed."
}

if (-not (Ensure-Administrator)) {
    exit 0
}

if ($Install -or $Uninstall) {
    if ($Install -and $Uninstall) {
        Write-Error "Specify only one of -Install or -Uninstall."
        exit 1
    }
    if ($Install) {
        Install-TaskmgrHijack -TargetExe $TargetExe
    } else {
        Uninstall-TaskmgrHijack
    }
    exit 0
}

Write-Host "Select action:"
Write-Host "  [I] Install"
Write-Host "  [U] Uninstall"
$choice = (Read-Host "Enter I or U").Trim().ToUpperInvariant()

switch ($choice) {
    "I" { Install-TaskmgrHijack -TargetExe $TargetExe }
    "U" { Uninstall-TaskmgrHijack }
    default {
        Write-Error "Invalid selection: $choice"
        exit 1
    }
}
